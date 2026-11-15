//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/object_trie.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/durable_file.h"
#include "dannan/net/codec.h"

namespace dagda::ledger {
namespace {

using dannan::contract::Lock;
using dannan::identity::sha256;
using dannan::net::DecodeError;
using dannan::net::Reader;
using dannan::net::Writer;

constexpr std::array<uint8_t, 8> journal_aad{'M', 'A', 'C', 'H', 'L', 'J', '0', '1'};
constexpr std::array<uint8_t, 8> nodes_aad{'M', 'A', 'C', 'H', 'L', 'N', '0', '1'};
constexpr std::array<uint8_t, 8> root_magic{'M', 'L', 'T', 'R', '0', '0', '0', '2'};
// Format 1 held a u64 value a record; it is read once and rewritten.
constexpr std::array<uint8_t, 8> format1_root_magic{'M', 'L', 'T', 'R', '0', '0', '0', '1'};
constexpr uint32_t max_frame = 4U * 1024U * 1024U;
constexpr size_t changes_per_frame = 65536;
constexpr uint8_t kind_leaf = 1;
constexpr uint8_t kind_interior = 2;
constexpr uint8_t journal_version = 2;
constexpr uint8_t format1_journal_version = 1;

Hash256 leaf_hash(std::span<const ObjectTrie::Record> records) {
  if (records.empty()) return {};
  Writer writer;
  writer.u8('L');
  for (const auto& [id, value] : records) {
    writer.fixed(id.bytes);
    writer.bytes(value);
  }
  return sha256(writer.data());
}

// Over each non-empty slot's index, hash and count.
template <typename Slots>
Hash256 interior_hash(const Slots& slots) {
  Writer writer;
  writer.u8('I');
  for (size_t i = 0; i < slots.size(); ++i) {
    if (!slots[i].count) continue;
    writer.u8(static_cast<uint8_t>(i));
    writer.fixed(slots[i].hash.bytes);
    writer.u64(slots[i].count);
  }
  return sha256(writer.data());
}

Bytes format1_value(uint64_t value) {
  Writer writer;
  writer.u64(value);
  return writer.take();
}

Hash256 format1_leaf_hash(Reader& reader, uint32_t count, std::vector<ObjectTrie::Record>& out) {
  Writer writer;
  writer.u8('L');
  for (uint32_t i = 0; i < count; ++i) {
    ObjectTrie::Record record;
    record.first.bytes = reader.fixed<32>();
    const auto value = reader.u64();
    record.second = format1_value(value);
    writer.fixed(record.first.bytes);
    writer.u64(value);
    out.push_back(std::move(record));
  }
  return sha256(writer.data());
}

}  // namespace

class ObjectTrie::NodeCache {
 public:
  explicit NodeCache(size_t bound) : _bound(bound) {}

  NodePtr find(uint64_t generation, uint64_t offset) {
    Lock lock(_mutex);
    const auto found = _entries.find({generation, offset});
    if (found == _entries.end()) return {};
    _recent.splice(_recent.begin(), _recent, found->second.recent);
    return found->second.node;
  }

  void insert(uint64_t generation, uint64_t offset, NodePtr node, size_t bytes) {
    Lock lock(_mutex);
    ++_loads;
    const Key key{generation, offset};
    if (_entries.contains(key)) return;
    _recent.push_front(key);
    _entries.emplace(key, Cached{std::move(node), bytes, _recent.begin()});
    _used += bytes;
    while (_used > _bound && _recent.size() > 1) {
      const auto victim = _entries.find(_recent.back());
      _recent.pop_back();
      _used -= victim->second.bytes;
      _entries.erase(victim);
    }
  }

  void drop_generation(uint64_t generation) {
    Lock lock(_mutex);
    for (auto it = _entries.begin(); it != _entries.end();) {
      if (it->first.first != generation) {
        ++it;
        continue;
      }
      _used -= it->second.bytes;
      _recent.erase(it->second.recent);
      it = _entries.erase(it);
    }
  }

  struct Usage {
    size_t bytes = 0;
    size_t nodes = 0;
    uint64_t loads = 0;
  };
  Usage usage() const {
    Lock lock(_mutex);
    return {_used, _entries.size(), _loads};
  }

 private:
  using Key = std::pair<uint64_t, uint64_t>;
  struct Cached {
    NodePtr node;
    size_t bytes = 0;
    std::list<Key>::iterator recent;
  };
  mutable dannan::contract::Mutex _mutex;
  std::map<Key, Cached> _entries DANNAN_GUARDED_BY(_mutex);
  std::list<Key> _recent DANNAN_GUARDED_BY(_mutex);
  size_t _used DANNAN_GUARDED_BY(_mutex) = 0;
  const size_t _bound;
  uint64_t _loads DANNAN_GUARDED_BY(_mutex) = 0;
};

struct ObjectTrie::Snapshot::Source {
  uint64_t generation = 0;
  std::shared_ptr<const SealedJournal::FrameReader> file;
  std::shared_ptr<NodeCache> cache;
};

std::filesystem::path ObjectTrie::nodes_path(uint64_t generation) const { return _dir / ("nodes-" + std::to_string(generation) + ".bin"); }

std::unique_ptr<SealedJournal> ObjectTrie::open_nodes(uint64_t generation) const {
  return std::make_unique<SealedJournal>(nodes_path(generation), _key, nodes_aad, max_frame, _logger);
}

size_t ObjectTrie::footprint(const Node& node) noexcept {
  size_t values = 0;
  for (const auto& record : node.records) values += record.second.capacity();
  return sizeof(Node) + node.records.capacity() * sizeof(Record) + values + node.children.capacity() * sizeof(Child);
}

ObjectTrie::NodePtr ObjectTrie::make_leaf(uint8_t depth, std::vector<Record> records) {
  auto node = std::make_shared<Node>();
  node->depth = depth;
  node->leaf = true;
  node->count = records.size();
  node->hash = leaf_hash(records);
  node->records = std::move(records);
  return node;
}

ObjectTrie::NodePtr ObjectTrie::make_interior(uint8_t depth, std::vector<Child> children) {
  auto node = std::make_shared<Node>();
  node->depth = depth;
  node->leaf = false;
  for (const auto& child : children) node->count += child.count;
  node->hash = interior_hash(children);
  node->children = std::move(children);
  return node;
}

ObjectTrie::Summary ObjectTrie::leaf_summary(std::span<const Record> records) { return {records.size(), leaf_hash(records)}; }

ObjectTrie::Summary ObjectTrie::interior_summary(const Children& children) {
  Summary out{0, interior_hash(children)};
  for (const auto& child : children) out.count += child.count;
  return out;
}

ObjectTrie::Child ObjectTrie::child_of(NodePtr node) {
  Child child;
  child.count = node->count;
  child.hash = node->hash;
  child.node = std::move(node);
  return child;
}

Bytes ObjectTrie::encode(const Node& node) const {
  Writer writer;
  if (node.leaf) {
    writer.u8(kind_leaf);
    writer.u8(node.depth);
    writer.u32(static_cast<uint32_t>(node.records.size()));
    for (const auto& [id, value] : node.records) {
      writer.fixed(id.bytes);
      writer.bytes(value);
    }
    return writer.take();
  }
  writer.u8(kind_interior);
  writer.u8(node.depth);
  std::array<uint8_t, 32> present{};
  for (size_t i = 0; i < 256; ++i)
    if (!node.children[i].empty()) present[i / 8] |= static_cast<uint8_t>(1U << (i % 8));
  writer.fixed(present);
  for (const auto& child : node.children) {
    if (child.empty()) continue;
    if (child.offset == unsaved) throw std::logic_error("object trie: encoding a node with an unsaved child");
    writer.u64(child.offset);
    writer.u32(child.size);
    writer.u64(child.count);
    writer.fixed(child.hash.bytes);
  }
  return writer.take();
}

ObjectTrie::NodePtr ObjectTrie::decode(std::span<const uint8_t> bytes, bool format1) {
  Reader reader(bytes);
  const auto kind = reader.u8();
  const auto depth = reader.u8();
  if (depth >= 32) throw DecodeError("object trie node too deep");
  if (kind == kind_leaf) {
    const auto count = reader.u32();
    if (count == 0 || count > leaf_max) throw DecodeError("object trie leaf of the wrong size");
    std::vector<Record> records;
    records.reserve(count);
    if (format1) {
      const auto hash = format1_leaf_hash(reader, count, records);
      reader.finish();
      for (size_t i = 1; i < records.size(); ++i)
        if (!(records[i - 1].first < records[i].first)) throw DecodeError("object trie leaf out of order");
      auto node = std::make_shared<Node>();
      node->depth = depth;
      node->count = records.size();
      node->hash = hash;
      node->records = std::move(records);
      return node;
    }
    for (uint32_t i = 0; i < count; ++i) {
      Record record;
      record.first.bytes = reader.fixed<32>();
      record.second = reader.bytes(value_max);
      if (!records.empty() && !(records.back().first < record.first)) throw DecodeError("object trie leaf out of order");
      records.push_back(record);
    }
    reader.finish();
    return make_leaf(depth, std::move(records));
  }
  if (kind != kind_interior) throw DecodeError("unknown object trie node");
  const auto present = reader.fixed<32>();
  std::vector<Child> children(256);
  for (size_t i = 0; i < 256; ++i) {
    if (!(present[i / 8] & (1U << (i % 8)))) continue;
    auto& child = children[i];
    child.offset = reader.u64();
    child.size = reader.u32();
    child.count = reader.u64();
    child.hash.bytes = reader.fixed<32>();
    if (child.offset == unsaved || child.count == 0) throw DecodeError("object trie child not saved");
  }
  reader.finish();
  auto node = make_interior(depth, std::move(children));
  if (node->count <= leaf_max) throw DecodeError("object trie interior node too small");
  return node;
}

ObjectTrie::NodePtr ObjectTrie::load_from(const Child& child, uint64_t generation, const Read& read, NodeCache& cache, bool format1) {
  if (child.node) return child.node;
  if (child.empty() || child.offset == unsaved) throw std::logic_error("object trie: loading an empty child");
  if (auto cached = cache.find(generation, child.offset)) return cached;
  auto node = decode(read(child.offset), format1);
  if (node->hash != child.hash || node->count != child.count)
    throw std::runtime_error("object trie node at " + std::to_string(child.offset) + " does not match its parent");
  cache.insert(generation, child.offset, node, footprint(*node));
  return node;
}

ObjectTrie::NodePtr ObjectTrie::load(const Child& child) const {
  return load_from(child, _generation, [this](uint64_t offset) { return _nodes->read_at(offset); }, *_cache, _format1);
}

ObjectTrie::Child ObjectTrie::build(uint8_t depth, std::vector<Record> records) const {
  if (records.empty()) return {};
  if (records.size() <= leaf_max) return child_of(make_leaf(depth, std::move(records)));
  if (depth >= 32) throw std::logic_error("object trie: more records than ids allow");
  std::vector<Child> children(256);
  auto begin = records.begin();
  while (begin != records.end()) {
    const auto byte = begin->first.bytes[depth];
    auto end = std::find_if(begin, records.end(), [&](const Record& r) { return r.first.bytes[depth] != byte; });
    children[byte] = build(static_cast<uint8_t>(depth + 1), std::vector<Record>(begin, end));
    begin = end;
  }
  return child_of(make_interior(depth, std::move(children)));
}

void ObjectTrie::gather(const Child& child, std::vector<Record>& out, bool superseding) {
  if (child.empty()) return;
  const auto node = load(child);
  if (superseding && child.offset != unsaved) _superseded_bytes += child.size;
  if (node->leaf) {
    out.insert(out.end(), node->records.begin(), node->records.end());
    return;
  }
  for (const auto& below : node->children) gather(below, out, superseding);
}

ObjectTrie::Child ObjectTrie::mutate(const Child& child, uint8_t depth, std::span<const Change> changes) {
  if (changes.empty()) return child;
  const NodePtr node = child.empty() ? NodePtr{} : load(child);
  if (!node || node->leaf) {
    if (node && child.offset != unsaved) _superseded_bytes += child.size;
    static const std::vector<Record> none;
    const auto& old = node ? node->records : none;
    std::vector<Record> merged;
    merged.reserve(old.size() + changes.size());
    auto at = old.begin();
    for (const auto& change : changes) {
      while (at != old.end() && at->first < change.id) merged.push_back(*at++);
      if (at != old.end() && at->first == change.id) ++at;
      if (change.value) merged.emplace_back(change.id, *change.value);
    }
    merged.insert(merged.end(), at, old.end());
    return build(depth, std::move(merged));
  }
  if (child.offset != unsaved) _superseded_bytes += child.size;
  auto children = node->children;
  auto begin = changes.begin();
  while (begin != changes.end()) {
    const auto byte = begin->id.bytes[depth];
    auto end = std::find_if(begin, changes.end(), [&](const Change& c) { return c.id.bytes[depth] != byte; });
    children[byte] = mutate(children[byte], static_cast<uint8_t>(depth + 1), std::span<const Change>(&*begin, static_cast<size_t>(end - begin)));
    begin = end;
  }
  uint64_t count = 0;
  for (const auto& below : children) count += below.count;
  if (count <= leaf_max) {
    std::vector<Record> all;
    all.reserve(count);
    for (const auto& below : children) gather(below, all, true);
    return build(depth, std::move(all));
  }
  return child_of(make_interior(depth, std::move(children)));
}

ObjectTrie::Child ObjectTrie::save(const Child& child, SealedJournal& into) {
  if (child.empty() || child.offset != unsaved) return child;
  const auto& node = *child.node;
  NodePtr saved;
  if (node.leaf) {
    saved = child.node;
  } else {
    std::vector<Child> children(256);
    for (size_t i = 0; i < 256; ++i) {
      children[i] = save(node.children[i], into);
      if (children[i].offset != unsaved) children[i].node.reset();
    }
    saved = make_interior(node.depth, std::move(children));
  }
  const auto bytes = encode(*saved);
  const auto before = into.bytes();
  const auto offset = into.append_unsynced(bytes);
  Child out;
  out.offset = offset;
  out.size = static_cast<uint32_t>(into.bytes() - before);
  out.count = saved->count;
  out.hash = saved->hash;
  return out;
}

void ObjectTrie::publish_root() const {
  Writer writer;
  writer.fixed(root_magic);
  writer.u64(_generation);
  writer.u64(_root.offset);
  writer.u32(_root.size);
  writer.u64(_root.count);
  writer.fixed(_root.hash.bytes);
  writer.u64(_live_bytes);
  const auto check = sha256(writer.data());
  writer.fixed(check.bytes);
  const auto& bytes = writer.data();
  dannan::identity::durable_replace_file(_dir / "root", std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

ObjectTrie::ObjectTrie(std::filesystem::path dir, std::shared_ptr<const domain::StorageKey> key, Options options, dannan::status::ScopedLogger logger)
    : _dir(std::move(dir)), _key(std::move(key)), _options(options), _logger(std::move(logger)), _cache(std::make_shared<NodeCache>(options.cache_bytes)) {
  if (!_key) throw std::invalid_argument("an object trie needs a key");
  std::filesystem::create_directories(_dir);
  if (std::ifstream in(_dir / "root", std::ios::binary); in) {
    const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
    Reader reader(bytes);
    const auto magic = reader.fixed<8>();
    _format1 = magic == format1_root_magic;
    if (magic != root_magic && !_format1) throw DecodeError("object trie root of another format");
    _generation = reader.u64();
    _root.offset = reader.u64();
    _root.size = reader.u32();
    _root.count = reader.u64();
    _root.hash.bytes = reader.fixed<32>();
    _live_bytes = reader.u64();
    const auto check = reader.fixed<32>();
    reader.finish();
    if (sha256(std::span<const uint8_t>(bytes.data(), bytes.size() - 32)).bytes != check) throw DecodeError("object trie root is damaged");
    if (_root.count == 0) _root = {};
  }
  // Node files of other generations are a rewrite that was not published,
  // or one that was and whose predecessor was not yet removed.
  for (const auto& entry : std::filesystem::directory_iterator(_dir)) {
    const auto name = entry.path().filename().string();
    if (name.starts_with("nodes-") && entry.path() != nodes_path(_generation)) {
      std::error_code ec;
      std::filesystem::remove(entry.path(), ec);
    }
  }
  _nodes = open_nodes(_generation);
  if (!_root.empty()) _root.node = load(_root);
  _journal = std::make_unique<SealedJournal>(_dir / "journal.log", _key, journal_aad, max_frame, _logger);
  _journal->replay([&](std::span<const uint8_t> plain) { apply_unjournaled(decode_journal_frame(plain)); }, max_frame, "object trie journal");
  if (_format1) {
    // Each value becomes its eight big-endian bytes, in the current format.
    auto records = next(std::nullopt, _root.count);
    _format1 = false;
    replace_all(std::move(records));
    return;
  }
  if (_journal->bytes() >= _options.checkpoint_bytes) checkpoint();
}

ObjectTrie::~ObjectTrie() = default;

void ObjectTrie::apply_unjournaled(std::span<const Change> changes) { _root = mutate(_root, 0, changes); }

void ObjectTrie::write(std::span<const Change> sorted_unique) {
  for (const auto& change : sorted_unique)
    if (change.value && change.value->size() > value_max) throw std::invalid_argument("object trie value too large");
  for (size_t from = 0; from < sorted_unique.size(); from += changes_per_frame) {
    const auto part = sorted_unique.subspan(from, std::min(changes_per_frame, sorted_unique.size() - from));
    Writer writer;
    writer.u8(journal_version);
    writer.u32(static_cast<uint32_t>(part.size()));
    for (const auto& change : part) {
      writer.fixed(change.id.bytes);
      writer.u8(change.value ? 1 : 0);
      if (change.value) writer.bytes(*change.value);
    }
    _journal->append(writer.data());
  }
}

void ObjectTrie::install(std::span<const Change> sorted_unique) {
  if (sorted_unique.empty()) return;
  apply_unjournaled(sorted_unique);
  if (_journal->bytes() >= _options.checkpoint_bytes) checkpoint();
}

void ObjectTrie::apply(std::span<const Change> input) {
  if (input.empty()) return;
  // Sorted by id, a later change to the same id winning.
  std::vector<Change> changes(input.begin(), input.end());
  std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.id < b.id; });
  std::vector<Change> unique;
  unique.reserve(changes.size());
  for (auto& change : changes) {
    if (!unique.empty() && unique.back().id == change.id)
      unique.back() = change;
    else
      unique.push_back(change);
  }
  write(unique);
  install(unique);
}

void ObjectTrie::replace_all(std::vector<Record> records) {
  // Everything saved is superseded; the rewrite reclaims it.
  _superseded_bytes = _live_bytes;
  _root = build(0, std::move(records));
  checkpoint();
}

void ObjectTrie::checkpoint() {
  const auto before = _nodes->bytes();
  auto saved = save(_root, *_nodes);
  _nodes->sync();
  const auto written = _nodes->bytes() - before;
  _live_bytes = _live_bytes + written - std::min(_live_bytes + written, _superseded_bytes);
  _superseded_bytes = 0;
  _root = saved;
  _root.node.reset();
  publish_root();
  _journal->reset();
  ++_checkpoints;
  if (!_root.empty()) _root.node = load(_root);

  // More superseded than live, and enough of it to matter: rewrite.
  const auto file = _nodes->bytes();
  if (file > 2 * _live_bytes && file - _live_bytes > _options.rewrite_floor_bytes) rewrite(_key);
}

void ObjectTrie::rewrite(std::shared_ptr<const domain::StorageKey> key) {
  const auto next = _generation + 1;
  std::error_code ec;
  std::filesystem::remove(nodes_path(next), ec);
  auto fresh = std::make_unique<SealedJournal>(nodes_path(next), key, nodes_aad, max_frame, _logger);
  // Every node is copied: unsave the tree, then save it into the new file.
  std::function<Child(const Child&)> copy = [&](const Child& child) -> Child {
    if (child.empty()) return {};
    const auto node = load(child);
    if (node->leaf) return child_of(make_leaf(node->depth, node->records));
    std::vector<Child> children(256);
    for (size_t i = 0; i < 256; ++i) children[i] = copy(node->children[i]);
    return child_of(make_interior(node->depth, std::move(children)));
  };
  auto rewritten = save(copy(_root), *fresh);
  fresh->sync();
  const auto old = nodes_path(_generation);
  _generation = next;
  _root = rewritten;
  _live_bytes = fresh->bytes();
  _nodes = std::move(fresh);
  _key = std::move(key);
  _cache->drop_generation(_generation - 1);
  publish_root();
  std::filesystem::remove(old, ec);
  ++_rewrites;
  if (!_root.empty()) _root.node = load(_root);
}

void ObjectTrie::rekey(std::shared_ptr<const domain::StorageKey> key) {
  if (!key) throw std::invalid_argument("an object trie needs a key");
  checkpoint();
  rewrite(std::move(key));
  // The emptied journal appends under the new key from here.
  _journal = std::make_unique<SealedJournal>(_dir / "journal.log", _key, journal_aad, max_frame, _logger);
}

std::vector<ObjectTrie::Change> ObjectTrie::decode_journal_frame(std::span<const uint8_t> plain) {
  Reader reader(plain);
  const auto version = reader.u8();
  const bool u64_values = version == format1_journal_version;
  if (version != journal_version && !u64_values) throw DecodeError("object trie journal of another version");
  const auto count = reader.u32();
  if (count > changes_per_frame) throw DecodeError("object trie journal frame too large");
  std::vector<Change> changes;
  changes.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Change change;
    change.id.bytes = reader.fixed<32>();
    if (reader.u8()) change.value = u64_values ? format1_value(reader.u64()) : reader.bytes(value_max);
    if (!changes.empty() && !(changes.back().id < change.id)) throw DecodeError("object trie journal frame out of order");
    changes.push_back(change);
  }
  reader.finish();
  return changes;
}

ObjectTrie::ReadOnlyWalk ObjectTrie::read_only(const std::filesystem::path& dir, const domain::StorageKey& key,
                                               const std::function<void(const ObjectId&, std::span<const uint8_t>)>& visit) {
  constexpr uint32_t lenient = 64U * 1024U * 1024U;
  ReadOnlyWalk out;
  uint64_t generation = 0;
  bool format1 = false;
  Child root;
  if (std::ifstream in(dir / "root", std::ios::binary); in) {
    const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
    Reader reader(bytes);
    const auto magic = reader.fixed<8>();
    format1 = magic == format1_root_magic;
    if (magic != root_magic && !format1) throw DecodeError("object trie root of another format");
    generation = reader.u64();
    root.offset = reader.u64();
    root.size = reader.u32();
    root.count = reader.u64();
    root.hash.bytes = reader.fixed<32>();
    (void)reader.u64();
    const auto check = reader.fixed<32>();
    reader.finish();
    if (sha256(std::span<const uint8_t>(bytes.data(), bytes.size() - 32)).bytes != check) throw DecodeError("object trie root is damaged");
  }

  // A later frame's change to an id wins, as replay's order gives it.
  std::map<ObjectId, std::optional<Bytes>> journal;
  const auto scan = SealedJournal::read_only(dir / "journal.log", key, journal_aad, lenient, [&](std::span<const uint8_t> plain) {
    for (auto& change : decode_journal_frame(plain)) journal[change.id] = std::move(change.value);
  });
  out.journal_frames = scan.frames;
  out.journal_tail_bytes = scan.tail_bytes;
  out.journal_damaged = scan.stopped_at_a_whole_frame;

  auto pending = journal.begin();
  const auto emit = [&](const ObjectId& id, std::span<const uint8_t> value) {
    visit(id, value);
    ++out.records;
  };
  const auto saved_record = [&](const Record& record) {
    for (; pending != journal.end() && pending->first < record.first; ++pending)
      if (pending->second) emit(pending->first, *pending->second);
    if (pending != journal.end() && pending->first == record.first) {
      if (pending->second) emit(pending->first, *pending->second);
      ++pending;
      return;
    }
    emit(record.first, record.second);
  };

  if (root.count) {
    const auto nodes = dir / ("nodes-" + std::to_string(generation) + ".bin");
    const int fd = ::open(nodes.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + nodes.string() + ": " + std::strerror(errno));
    const struct Closing {
      int fd;
      ~Closing() { ::close(fd); }
    } closing{fd};
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) throw std::runtime_error("cannot stat " + nodes.string());
    const auto size = static_cast<uint64_t>(st.st_size);
    uint64_t saved = 0;
    std::function<void(const Child&)> walk = [&](const Child& child) {
      const auto node = decode(SealedJournal::read_only_frame(fd, key, nodes_aad, lenient, size, child.offset), format1);
      if (node->hash != child.hash || node->count != child.count)
        throw std::runtime_error("object trie node at " + std::to_string(child.offset) + " of " + nodes.string() + " does not match its parent");
      if (node->leaf) {
        saved += node->records.size();
        for (const auto& record : node->records) saved_record(record);
        return;
      }
      for (const auto& below : node->children)
        if (!below.empty()) walk(below);
    };
    walk(root);
    if (saved != root.count)
      throw std::runtime_error("object trie " + dir.string() + " holds " + std::to_string(saved) + " records where its root says " +
                               std::to_string(root.count));
  }
  for (; pending != journal.end(); ++pending)
    if (pending->second) emit(pending->first, *pending->second);
  return out;
}

std::optional<bool> ObjectTrie::probe(const std::filesystem::path& dir, const domain::StorageKey& key) {
  constexpr uint32_t lenient = 64U * 1024U * 1024U;
  if (std::ifstream in(dir / "root", std::ios::binary); in) {
    const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
    Reader reader(bytes);
    const auto magic = reader.fixed<8>();
    if (magic != root_magic && magic != format1_root_magic) throw DecodeError("object trie root of another format");
    const auto generation = reader.u64();
    const auto offset = reader.u64();
    (void)reader.u32();
    const auto count = reader.u64();
    if (count) {
      const auto nodes = dir / ("nodes-" + std::to_string(generation) + ".bin");
      // The root's own node: a published node is always a whole frame,
      // where the file's first bytes may be a tail a crash left.
      if (const auto opens = SealedJournal::probe(nodes, key, nodes_aad, lenient, offset); opens.has_value()) return opens;
    }
  }
  if (SealedJournal::holds_only_a_torn_frame(dir / "journal.log", lenient)) return std::nullopt;
  return SealedJournal::probe(dir / "journal.log", key, journal_aad, lenient);
}

std::optional<Bytes> ObjectTrie::find(const Child& root, const ObjectId& id, const std::function<NodePtr(const Child&)>& load) {
  if (root.empty()) return std::nullopt;
  auto node = load(root);
  while (!node->leaf) {
    const auto& child = node->children[id.bytes[node->depth]];
    if (child.empty()) return std::nullopt;
    node = load(child);
  }
  const auto found =
      std::lower_bound(node->records.begin(), node->records.end(), id, [](const Record& record, const ObjectId& key) { return record.first < key; });
  if (found == node->records.end() || found->first != id) return std::nullopt;
  return found->second;
}

void ObjectTrie::collect(const Child& child, const std::optional<ObjectId>& after, bool bounded, size_t limit, std::vector<Record>& out,
                         const std::function<NodePtr(const Child&)>& load) {
  if (child.empty() || out.size() >= limit) return;
  const auto node = load(child);
  if (node->leaf) {
    for (const auto& record : node->records) {
      if (out.size() >= limit) return;
      if (bounded && !(*after < record.first)) continue;
      out.push_back(record);
    }
    return;
  }
  const size_t start = bounded ? after->bytes[node->depth] : 0;
  for (size_t i = start; i < 256 && out.size() < limit; ++i) collect(node->children[i], after, bounded && i == start, limit, out, load);
}

std::optional<Bytes> ObjectTrie::get(const ObjectId& id) const {
  return find(_root, id, [this](const Child& child) { return load(child); });
}

std::vector<ObjectTrie::Record> ObjectTrie::next(const std::optional<ObjectId>& after, size_t limit) const {
  std::vector<Record> out;
  collect(_root, after, after.has_value(), limit, out, [this](const Child& child) { return load(child); });
  return out;
}

ObjectTrie::Snapshot ObjectTrie::snapshot() const {
  Snapshot out;
  out._root = _root;
  out._source = std::make_shared<const Snapshot::Source>(Snapshot::Source{_generation, _nodes->reader(), _cache});
  return out;
}

namespace {

// How a snapshot reads a saved node: through its own handle on its
// generation's node file, by way of the shared cache.
template <typename Source, typename LoadFrom>
auto snapshot_loader(const Source& source, LoadFrom load_from) {
  return [&source, load_from](const auto& child) {
    return load_from(child, source.generation, [&source](uint64_t offset) { return source.file->read_at(offset); }, *source.cache, false);
  };
}

}  // namespace

std::optional<Bytes> ObjectTrie::Snapshot::get(const ObjectId& id) const {
  if (_root.empty()) return std::nullopt;
  return find(_root, id, snapshot_loader(*_source, &ObjectTrie::load_from));
}

std::vector<ObjectId> ObjectTrie::Snapshot::ids() const {
  std::vector<ObjectId> out;
  out.reserve(size());
  std::optional<ObjectId> after;
  while (true) {
    const auto page = next(after, 4096);
    for (const auto& record : page) out.push_back(record.first);
    if (page.size() < 4096) return out;
    after = page.back().first;
  }
}

std::vector<ObjectTrie::Record> ObjectTrie::Snapshot::next(const std::optional<ObjectId>& after, size_t limit) const {
  std::vector<Record> out;
  if (_root.empty()) return out;
  collect(_root, after, after.has_value(), limit, out, snapshot_loader(*_source, &ObjectTrie::load_from));
  return out;
}

ObjectTrie::NodePtr ObjectTrie::descend(const Child& root, const Prefix& prefix, const std::function<NodePtr(const Child&)>& load) {
  if (root.empty()) return {};
  auto node = load(root);
  while (!node->leaf && node->depth < prefix.length) {
    const auto& child = node->children[prefix.bytes[node->depth]];
    if (child.empty()) return {};
    node = load(child);
  }
  return node;
}

std::vector<ObjectTrie::Children> ObjectTrie::Snapshot::children(std::span<const Prefix> prefixes) const {
  std::vector<Children> out(prefixes.size());
  if (_root.empty()) return out;
  const std::function<NodePtr(const Child&)> load = snapshot_loader(*_source, &ObjectTrie::load_from);
  for (size_t i = 0; i < prefixes.size(); ++i) {
    const auto& prefix = prefixes[i];
    if (prefix.length >= 32) throw std::logic_error("object trie: a whole id has no children");
    const auto node = descend(_root, prefix, load);
    if (!node) continue;
    if (!node->leaf) {
      for (size_t slot = 0; slot < 256; ++slot) out[i][slot] = {node->children[slot].count, node->children[slot].hash};
      continue;
    }
    // Below a leaf every subtree is a leaf of its own records.
    auto at = node->records.begin();
    while (at != node->records.end()) {
      if (!prefix.covers(at->first)) {
        ++at;
        continue;
      }
      const auto byte = at->first.bytes[prefix.length];
      auto end = at;
      while (end != node->records.end() && prefix.covers(end->first) && end->first.bytes[prefix.length] == byte) ++end;
      const std::vector<Record> group(at, end);
      out[i][byte] = {group.size(), leaf_hash(group)};
      at = end;
    }
  }
  return out;
}

std::vector<std::vector<ObjectTrie::Record>> ObjectTrie::Snapshot::records(std::span<const Prefix> prefixes, uint64_t most) const {
  std::vector<std::vector<Record>> out(prefixes.size());
  if (_root.empty()) return out;
  const std::function<NodePtr(const Child&)> load = snapshot_loader(*_source, &ObjectTrie::load_from);
  for (size_t i = 0; i < prefixes.size(); ++i) {
    const auto node = descend(_root, prefixes[i], load);
    if (!node) continue;
    if (node->leaf) {
      for (const auto& record : node->records)
        if (prefixes[i].covers(record.first)) out[i].push_back(record);
      continue;
    }
    if (node->count > most) throw std::length_error("object trie: more records below a prefix than asked for");
    Child whole;
    whole.count = node->count;
    whole.hash = node->hash;
    whole.node = node;
    collect(whole, std::nullopt, false, node->count, out[i], load);
  }
  return out;
}

uint64_t ObjectTrie::size() const noexcept { return _root.count; }

Hash256 ObjectTrie::root_hash() const noexcept { return _root.hash; }

ObjectTrie::Stats ObjectTrie::stats() const noexcept {
  const auto usage = _cache->usage();
  return {_journal->bytes(), _nodes->bytes(), _live_bytes, usage.bytes, usage.nodes, usage.loads, _checkpoints, _rewrites};
}

}  // namespace dagda::ledger
