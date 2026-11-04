//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/reference_counts.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>

#include "dannan/identity/durable_file.h"
#include "dannan/net/codec.h"

namespace dagda::ledger {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

ObjectTrie::Options options_for(size_t cache_bytes) {
  ObjectTrie::Options options;
  options.cache_bytes = cache_bytes;
  return options;
}

Bytes encode_count(uint64_t count) {
  Writer writer;
  writer.u64(count);
  return writer.take();
}

uint64_t decode_count(const Bytes& value) {
  Reader reader(value);
  const auto count = reader.u64();
  reader.finish();
  return count;
}

// One record per distinct id, its value the number of times it appears.
std::vector<ObjectTrie::Record> counted(std::vector<ObjectId> ids) {
  std::sort(ids.begin(), ids.end());
  std::vector<ObjectTrie::Record> records;
  for (auto it = ids.begin(); it != ids.end();) {
    const auto end = std::upper_bound(it, ids.end(), *it);
    records.emplace_back(*it, encode_count(static_cast<uint64_t>(end - it)));
    it = end;
  }
  return records;
}

// The changes `in` and `out` make to `trie`'s counts, or none when a count
// would go below zero.
std::optional<std::vector<ObjectTrie::Change>> changes_for(const ObjectTrie& trie, const std::vector<ObjectId>& in, const std::vector<ObjectId>& out) {
  std::map<ObjectId, int64_t> delta;
  for (const auto& id : in) ++delta[id];
  for (const auto& id : out) --delta[id];
  std::vector<ObjectTrie::Change> changes;
  for (const auto& [id, by] : delta) {
    if (by == 0) continue;
    const auto value = trie.get(id);
    const auto now = static_cast<int64_t>(value ? decode_count(*value) : 0);
    if (now + by < 0) return std::nullopt;
    changes.push_back({id, now + by == 0 ? std::nullopt : std::optional(encode_count(static_cast<uint64_t>(now + by)))});
  }
  return changes;
}

}  // namespace

ReferenceCounts::ReferenceCounts(std::filesystem::path dir, std::shared_ptr<const domain::StorageKey> key, size_t cache_bytes,
                                 dannan::status::ScopedLogger logger)
    : _dir(std::move(dir)),
      _logger(std::move(logger)),
      _extents(_dir / "extents", key, options_for(cache_bytes - cache_bytes / 4), _logger),
      _nodes(_dir / "nodes", key, options_for(cache_bytes / 4), _logger) {
  std::ifstream in(_dir / "state", std::ios::binary);
  if (!in) return;
  const Bytes bytes{std::istreambuf_iterator<char>(in), {}};
  try {
    Reader reader(bytes);
    if (reader.fixed<8>() != reference_counts_magic) return;
    const bool clean = reader.u8() == 1;
    const bool has_root = reader.u8() == 1;
    ObjectId root;
    root.bytes = reader.fixed<32>();
    Totals totals;
    totals.entries = reader.u64();
    totals.extent_count = reader.u64();
    reader.finish();
    if (!clean) return;
    if (has_root) _root = root;
    _totals = totals;
  } catch (const std::exception& error) {
    _logger.warn("reference counts state unreadable path=" + (_dir / "state").string() + " error=" + error.what());
  }
}

void ReferenceCounts::write_state(bool clean) const {
  Writer writer;
  writer.fixed(reference_counts_magic);
  writer.u8(clean ? 1 : 0);
  writer.u8(_root ? 1 : 0);
  writer.fixed(_root ? _root->bytes : ObjectId{}.bytes);
  writer.u64(_totals.entries);
  writer.u64(_totals.extent_count);
  const auto& bytes = writer.data();
  dannan::identity::durable_replace_file(_dir / "state", std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void ReferenceCounts::reset(const std::optional<ObjectId>& root, std::vector<ObjectId> extents, std::vector<ObjectId> nodes, Totals totals) {
  write_state(false);
  _extents.replace_all(counted(std::move(extents)));
  _nodes.replace_all(counted(std::move(nodes)));
  _root = root;
  _totals = totals;
  write_state(true);
}

bool ReferenceCounts::change(const ObjectId& root, std::vector<ObjectId> extents_in, std::vector<ObjectId> extents_out, std::vector<ObjectId> nodes_in,
                             std::vector<ObjectId> nodes_out, Totals totals) {
  // Checked whole before anything is written, so a refusal changes nothing.
  auto extent_changes = changes_for(_extents, extents_in, extents_out);
  auto node_changes = changes_for(_nodes, nodes_in, nodes_out);
  if (!extent_changes || !node_changes) return false;
  write_state(false);
  _extents.apply(*extent_changes);
  _nodes.apply(*node_changes);
  _root = root;
  _totals = totals;
  write_state(true);
  return true;
}

ReferenceCounts::Views ReferenceCounts::views() const { return {_extents.snapshot(), _nodes.snapshot()}; }

}  // namespace dagda::ledger
