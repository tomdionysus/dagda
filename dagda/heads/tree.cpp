//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/tree.h"

#include <iterator>
#include <limits>

#include "dagda/contract/stopping.h"
#include "dannan/identity/crypto.h"

namespace dagda::heads {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

std::span<const uint8_t> key_span(std::string_view key) { return {reinterpret_cast<const uint8_t*>(key.data()), key.size()}; }

uint8_t next_spine_level(uint8_t level) { return level < std::numeric_limits<uint8_t>::max() ? static_cast<uint8_t>(level + 1) : level; }

// A sequence node's children, the items passed over.
std::vector<ObjectId> read_sequence_children(Reader& reader) {
  (void)reader.u8();
  const auto count = reader.u32();
  std::vector<ObjectId> children;
  children.reserve(std::min<size_t>(count, reader.remaining() / 40));
  for (uint32_t i = 0; i < count; ++i) {
    children.push_back(ObjectId{reader.fixed<32>()});
    (void)reader.u64();
  }
  return children;
}

}  // namespace

uint64_t tree_boundary_hash(std::string_view domain, uint8_t level, std::span<const uint8_t> key) {
  dannan::identity::Sha256Hasher hasher;
  hasher.update(key_span(domain));
  hasher.update({&level, 1});
  hasher.update(key);
  const auto digest = hasher.finish();
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value = (value << 8) | digest.bytes[i];
  return value;
}

bool is_tree_boundary(uint64_t hash, size_t target) noexcept {
  if (target <= 1) return true;
  return hash < std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(target);
}

std::string tree_prefix_upper_bound(std::string_view prefix) {
  std::string upper(prefix);
  while (!upper.empty()) {
    auto& last = reinterpret_cast<unsigned char&>(upper.back());
    if (last != 0xFF) {
      ++last;
      return upper;
    }
    upper.pop_back();
  }
  return {};
}

ObjectId MemoryTreeNodeStore::put(std::span<const uint8_t> node) {
  const auto id = store::object_id(node);
  const auto [it, inserted] = _nodes.try_emplace(id, Bytes(node.begin(), node.end()));
  if (inserted) {
    _bytes += node.size();
    _written.push_back(id);
  }
  return id;
}

std::optional<Bytes> MemoryTreeNodeStore::get(const ObjectId& id) const {
  ++_reads;
  const auto found = _nodes.find(id);
  if (found == _nodes.end()) return std::nullopt;
  return found->second;
}

void MemoryTreeNodeStore::put_at(const ObjectId& id, Bytes node) {
  _bytes += node.size();
  _nodes[id] = std::move(node);
}

ObjectId TreeChildWriter::build(std::span<const uint8_t> records) {
  const auto record_bytes = _form.child_record_bytes;
  if (!record_bytes || records.size() % record_bytes) throw std::logic_error("tree child records are not whole");
  const auto& limits = _form.limits;
  std::vector<TreeShape::Child> chunks;
  size_t run_begin = 0;
  const auto flush = [&](size_t run_end) {
    if (run_end == run_begin) return;
    Writer writer;
    writer.raw(_form.child_leaf_magic);
    writer.u32(static_cast<uint32_t>(run_end - run_begin));
    writer.raw(records.subspan(run_begin * record_bytes, (run_end - run_begin) * record_bytes));
    const auto encoded = writer.take();
    chunks.push_back(TreeShape::Child{{}, _store.put(encoded), static_cast<uint64_t>(run_end - run_begin)});
    run_begin = run_end;
  };
  const auto count = records.size() / record_bytes;
  for (size_t i = 0; i < count; ++i) {
    const auto record = records.subspan(i * record_bytes, record_bytes);
    if (is_tree_boundary(tree_boundary_hash(_form.child_domain, 0, _records.child_boundary(record)), limits.child_target_fanout) ||
        i + 1 - run_begin >= limits.child_max_fanout)
      flush(i + 1);
  }
  flush(count);
  const TreeShape shape(_records, _form);
  return shape.build_spine(std::move(chunks), _store, false, limits.child_target_fanout, limits.child_max_fanout);
}

void TreeChildReader::read(const ObjectId& root, Bytes& out) const {
  auto encoded = _store.get(root);
  if (!encoded) throw DecodeError("tree child node unavailable: " + store::to_string(root));
  Reader reader(*encoded);
  const auto magic = reader.fixed<4>();
  if (magic == _form.child_leaf_magic) {
    const auto count = reader.u32();
    if (!_form.child_record_bytes) throw DecodeError("tree form has no child records");
    // Bytes that exist bound the reservation; the count is the node's own claim.
    out.reserve(out.size() + std::min<size_t>(static_cast<size_t>(count) * _form.child_record_bytes, reader.remaining()));
    for (uint32_t i = 0; i < count; ++i) {
      const auto record = reader.view(_form.child_record_bytes);
      out.insert(out.end(), record.begin(), record.end());
    }
    reader.finish();
    return;
  }
  if (magic != _form.child_branch_magic) throw DecodeError("not a tree child node");
  const auto children = read_sequence_children(reader);
  reader.finish();
  for (const auto& child : children) read(child, out);
}

bool TreeShape::ends_a_leaf(std::string_view key) const {
  return is_tree_boundary(tree_boundary_hash(_form.entry_domain, 0, key_span(key)), _form.limits.entry_target_fanout);
}

ObjectId TreeShape::empty_leaf(TreeNodeStore& store) const {
  Writer writer;
  writer.raw(_form.leaf_magic);
  writer.u32(0);
  const auto encoded = writer.take();
  return store.put(encoded);
}

bool TreeShape::branch_boundary(const Child& child, uint8_t level, bool keyed, size_t target) const {
  const auto hash =
      keyed ? tree_boundary_hash(_form.branch_domain, level, key_span(child.first_key)) : tree_boundary_hash(_form.child_branch_domain, level, child.id.bytes);
  return is_tree_boundary(hash, target);
}

TreeShape::Child TreeShape::put_spine_node(const std::vector<Child>& run, uint8_t level, bool keyed, TreeNodeStore& store) const {
  Writer writer;
  writer.raw(keyed ? _form.branch_magic : _form.child_branch_magic);
  writer.u8(level);
  writer.u32(static_cast<uint32_t>(run.size()));
  uint64_t items = 0;
  for (const auto& child : run) {
    if (keyed) writer.string(child.first_key);
    writer.fixed(child.id.bytes);
    writer.u64(child.items);
    items += child.items;
  }
  const auto encoded = writer.take();
  return Child{run.front().first_key, store.put(encoded), items};
}

// One spine level: groups `children` into the nodes above them. Grouping hashes
// each child's key (its content address on a keyless child sequence), so the
// shape is a function of the child sequence alone.
std::vector<TreeShape::Child> TreeShape::build_spine_level(const std::vector<Child>& children, uint8_t level, TreeNodeStore& store, bool keyed, size_t target,
                                                           size_t maximum) const {
  std::vector<Child> parents;
  std::vector<Child> run;
  const auto flush = [&] {
    if (run.empty()) return;
    parents.push_back(put_spine_node(run, level, keyed, store));
    run.clear();
  };

  // `packed` ignores the boundary test and groups by the count cap alone: the
  // fallback for a level where every child hashed as a boundary and nothing
  // reduced. It makes progress unconditional: at maximum >= 2, n >= 2 children
  // become at most ceil(n/maximum) < n parents. History independence holds: the
  // trigger and the packing are both functions of this level's child sequence.
  const auto build_level = [&](bool packed) {
    parents.clear();
    run.clear();
    for (const auto& child : children) {
      run.push_back(child);
      if ((!packed && branch_boundary(child, level, keyed, target)) || run.size() >= maximum) flush();
    }
    flush();
  };

  build_level(false);
  if (parents.size() >= children.size() && children.size() > 1) build_level(true);
  if (parents.size() >= children.size() && children.size() > 1)
    throw std::logic_error("tree spine made no progress even packed: level=" + std::to_string(level) + " keyed=" + (keyed ? "1" : "0") +
                           " children=" + std::to_string(children.size()) + " parents=" + std::to_string(parents.size()) + " max=" + std::to_string(maximum));
  return parents;
}

// Groups children into spine nodes until one remains.
ObjectId TreeShape::build_spine(std::vector<Child> children, TreeNodeStore& store, bool keyed, size_t target, size_t maximum) const {
  if (children.empty()) throw std::logic_error("tree spine over no children");
  uint8_t level = 1;
  while (children.size() > 1) {
    children = build_spine_level(children, level, store, keyed, target, maximum);
    level = next_spine_level(level);
  }
  return children.front().id;
}

ObjectId TreeShape::build_spine(std::vector<Child> children, TreeNodeStore& store) const {
  return build_spine(std::move(children), store, true, _form.limits.branch_target_fanout, _form.limits.branch_max_fanout);
}

TreeShape::SpineNode TreeShape::read_spine_node(const ObjectId& id, const TreeNodeStore& store) const {
  auto encoded = store.get(id);
  if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(id));
  Reader reader(*encoded);
  const auto magic = reader.fixed<4>();
  SpineNode node;
  if (magic == _form.leaf_magic) return node;
  if (magic != _form.branch_magic) throw DecodeError("not a tree node");
  node.level = reader.u8();
  const auto count = reader.u32();
  if (!node.level || !count) throw DecodeError("malformed tree branch");
  node.children.reserve(std::min<size_t>(count, reader.remaining() / 44));
  for (uint32_t i = 0; i < count; ++i) {
    Child child;
    child.first_key = reader.string(_form.limits.key_max_bytes);
    child.id = ObjectId{reader.fixed<32>()};
    child.items = reader.u64();
    node.children.push_back(std::move(child));
  }
  reader.finish();
  return node;
}

// Replaces the item at `at` with its children, one level down.
void TreeShape::expand_spine_item(std::vector<SpineItem>& items, size_t at, const TreeNodeStore& store) const {
  const auto level = items[at].level;
  auto node = read_spine_node(items[at].child.id, store);
  if (node.level != level) throw DecodeError("tree node at an unexpected level");
  std::vector<SpineItem> children;
  children.reserve(node.children.size());
  for (auto& child : node.children) children.push_back({std::move(child), static_cast<uint8_t>(level - 1), false});
  items.erase(items.begin() + static_cast<std::ptrdiff_t>(at));
  items.insert(items.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(children.begin()), std::make_move_iterator(children.end()));
}

// Every item brought down to `level`, reading whatever stands above it.
void TreeShape::expand_spine_to(std::vector<SpineItem>& items, uint8_t level, const TreeNodeStore& store) const {
  for (size_t i = 0; i < items.size();) {
    if (items[i].level > level) {
      expand_spine_item(items, i, store);
    } else {
      ++i;
    }
  }
}

// The level above `items`, which stand at `level`. A node above the level being
// built is kept unread wherever the grouping reaches it at a group boundary: it
// was built from the same children by the same rule. One the grouping reaches
// mid-group is opened and its children regrouped, until the groups fall on an old
// boundary again.
std::vector<TreeShape::SpineItem> TreeShape::splice_spine_level(std::vector<SpineItem> items, uint8_t level, TreeNodeStore& store) const {
  const auto above = next_spine_level(level);
  const auto target = _form.limits.branch_target_fanout;
  const auto maximum = _form.limits.branch_max_fanout;
  // The whole level, as a full build makes it.
  const auto exact = [&] {
    expand_spine_to(items, level, store);
    std::vector<Child> children;
    children.reserve(items.size());
    for (auto& item : items) children.push_back(std::move(item.child));
    std::vector<SpineItem> parents;
    for (auto& parent : build_spine_level(children, above, store, true, target, maximum)) parents.push_back({std::move(parent), above, true});
    return parents;
  };
  if (std::all_of(items.begin(), items.end(), [&](const SpineItem& item) { return item.level == level; })) return exact();

  std::vector<SpineItem> parents;
  std::vector<Child> run;
  // Whether some group made here holds more than one child.
  bool several = false;
  bool deeper = false;
  const auto flush = [&] {
    if (run.empty()) return;
    several = several || run.size() > 1;
    parents.push_back({put_spine_node(run, above, true, store), above, true});
    run.clear();
  };
  for (size_t i = 0; i < items.size();) {
    if (items[i].level == level) {
      run.push_back(items[i].child);
      if (branch_boundary(items[i].child, above, true, target) || run.size() >= maximum) flush();
      ++i;
    } else if (run.empty()) {
      deeper = deeper || items[i].level > above;
      parents.push_back(items[i]);
      ++i;
    } else {
      expand_spine_item(items, i, store);
    }
  }
  flush();

  // A full build regroups a whole level by count alone when grouping by key
  // reduces nothing: when every group is a single child. A level of a few nodes
  // is settled as a full build settles it. A larger one needs only a group of
  // several, made here or found among those kept.
  constexpr size_t few = 8;
  if (!deeper && parents.size() <= few) return exact();
  if (several) return parents;
  for (const auto& parent : parents) {
    if (parent.fresh) continue;
    ObjectId id = parent.child.id;
    auto node = read_spine_node(id, store);
    while (node.level > above) {
      id = node.children.front().id;
      node = read_spine_node(id, store);
    }
    if (node.level != above) throw DecodeError("tree node at an unexpected level");
    // A child ending a group short of its node's end means the node was grouped
    // by count: the level it came from reduced nothing by key.
    for (size_t i = 0; i + 1 < node.children.size(); ++i)
      if (branch_boundary(node.children[i], above, true, target)) return exact();
    if (node.children.size() > 1) return parents;
  }
  return exact();
}

void TreeShape::read_leaf_records(const ObjectId& id, const TreeNodeStore& store, std::deque<Bytes>& backing, std::vector<LeafRecord>& out) const {
  auto encoded = store.get(id);
  if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(id));
  backing.push_back(std::move(*encoded));
  const Bytes& bytes = backing.back();
  Reader reader(bytes);
  if (reader.fixed<4>() != _form.leaf_magic) throw DecodeError("not a tree leaf");
  const auto count = reader.u32();
  for (uint32_t i = 0; i < count; ++i) {
    const auto start = bytes.size() - reader.remaining();
    auto key = reader.string(_form.limits.key_max_bytes);
    _records.skip(reader);
    const auto end = bytes.size() - reader.remaining();
    out.push_back({std::move(key), std::span<const uint8_t>(bytes).subspan(start, end - start)});
  }
  reader.finish();
}

std::vector<std::pair<std::string, ObjectId>> TreeShape::read_branch_keys(Reader& reader) const {
  const auto count = reader.u32();
  std::vector<std::pair<std::string, ObjectId>> children;
  children.reserve(std::min<size_t>(count, reader.remaining() / 44));
  for (uint32_t i = 0; i < count; ++i) {
    auto key = reader.string(_form.limits.key_max_bytes);
    children.emplace_back(std::move(key), ObjectId{reader.fixed<32>()});
    (void)reader.u64();
  }
  reader.finish();
  return children;
}

void TreeShape::diff_nodes(const ObjectId& before, const ObjectId& after, const TreeNodeStore& store, std::vector<ObjectId>& added,
                           std::vector<ObjectId>& removed, const TreePause& pause) const {
  if (before == after) return;
  // Both trees, walked together. A subtree both stand at is passed over unread.
  // A branch or leaf holds its keys, so the same node cannot stand at two places:
  // one opened on one side only is in that tree only, once those opened on both
  // sides are set aside.
  struct Side {
    std::vector<SpineItem> pending;
    std::vector<ObjectId> opened;
  };
  const auto begin = [&](const ObjectId& root) {
    Side side;
    side.pending.push_back({Child{{}, root, 0}, read_spine_node(root, store).level, false});
    return side;
  };
  const auto open = [&](Side& side) {
    if (pause) pause();
    const auto item = std::move(side.pending.back());
    side.pending.pop_back();
    side.opened.push_back(item.child.id);
    if (item.level == 0) return;
    auto node = read_spine_node(item.child.id, store);
    if (node.level != item.level) throw DecodeError("tree node at an unexpected level");
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
      side.pending.push_back({std::move(*child), static_cast<uint8_t>(item.level - 1), false});
  };
  Side old_side = begin(before);
  Side new_side = begin(after);
  while (!old_side.pending.empty() || !new_side.pending.empty()) {
    if (old_side.pending.empty()) {
      open(new_side);
      continue;
    }
    if (new_side.pending.empty()) {
      open(old_side);
      continue;
    }
    const auto& a = old_side.pending.back();
    const auto& b = new_side.pending.back();
    if (a.level == b.level && a.child.id == b.child.id) {
      old_side.pending.pop_back();
      new_side.pending.pop_back();
    } else if (a.level > b.level) {
      open(old_side);
    } else if (b.level > a.level) {
      open(new_side);
    } else if (a.level > 0) {
      open(old_side);
      open(new_side);
    } else if (a.child.first_key <= b.child.first_key) {
      // Two different leaves: the one that starts first is passed.
      open(old_side);
    } else {
      open(new_side);
    }
  }
  for (auto* side : {&old_side, &new_side}) {
    std::sort(side->opened.begin(), side->opened.end());
    side->opened.erase(std::unique(side->opened.begin(), side->opened.end()), side->opened.end());
  }
  std::set_difference(new_side.opened.begin(), new_side.opened.end(), old_side.opened.begin(), old_side.opened.end(), std::back_inserter(added));
  std::set_difference(old_side.opened.begin(), old_side.opened.end(), new_side.opened.begin(), new_side.opened.end(), std::back_inserter(removed));
}

void TreeShape::walk_child_stats(const ObjectId& id, const TreeNodeStore& store, TreeStats& stats) const {
  auto encoded = store.get(id);
  if (!encoded) throw DecodeError("tree child node unavailable: " + store::to_string(id));
  ++stats.child_nodes;
  stats.bytes += encoded->size();
  stats.largest_node_bytes = std::max<uint64_t>(stats.largest_node_bytes, encoded->size());
  Reader reader(*encoded);
  const auto magic = reader.fixed<4>();
  if (magic == _form.child_leaf_magic) return;  // counted through the entry's own total
  if (magic != _form.child_branch_magic) throw DecodeError("not a tree child node");
  for (const auto& child : read_sequence_children(reader)) walk_child_stats(child, store, stats);
}

void TreeShape::walk_stats(const ObjectId& id, const TreeNodeStore& store, TreeStats& stats, size_t depth) const {
  auto encoded = store.get(id);
  if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(id));
  stats.bytes += encoded->size();
  stats.largest_node_bytes = std::max<uint64_t>(stats.largest_node_bytes, encoded->size());
  stats.depth = std::max(stats.depth, depth);
  Reader reader(*encoded);
  const auto magic = reader.fixed<4>();
  if (magic == _form.leaf_magic) {
    ++stats.leaves;
    const auto count = reader.u32();
    stats.entries += count;
    std::vector<ObjectId> child_roots;
    for (uint32_t i = 0; i < count; ++i) {
      (void)reader.string(_form.limits.key_max_bytes);
      const auto shape = _records.shape(reader);
      stats.items += shape.items;
      if (shape.child_root) child_roots.push_back(*shape.child_root);
    }
    for (const auto& root : child_roots) walk_child_stats(root, store, stats);
    return;
  }
  if (magic != _form.branch_magic) throw DecodeError("not a tree node");
  ++stats.branches;
  (void)reader.u8();
  const auto count = reader.u32();
  std::vector<ObjectId> children;
  for (uint32_t i = 0; i < count; ++i) {
    (void)reader.string(_form.limits.key_max_bytes);
    children.push_back(ObjectId{reader.fixed<32>()});
    (void)reader.u64();
  }
  for (const auto& child : children) walk_stats(child, store, stats, depth + 1);
}

TreeStats TreeShape::stats(const ObjectId& root, const TreeNodeStore& store) const {
  TreeStats out;
  walk_stats(root, store, out, 1);
  return out;
}

namespace {

// One node's structure, read without fetching anything beyond it: a branch's
// children, or a leaf's child roots.
struct NodeShape {
  bool leaf = false;
  std::vector<std::pair<std::string, ObjectId>> children;
  std::vector<std::optional<ObjectId>> child_roots;
};

NodeShape read_shape(const ObjectId& id, const TreeNodeStore& store, const TreeForm& form, const TreeRecords& records) {
  auto encoded = store.get(id);
  if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(id));
  Reader reader(*encoded);
  const auto magic = reader.fixed<4>();
  NodeShape shape;
  if (magic == form.leaf_magic) {
    shape.leaf = true;
    const auto count = reader.u32();
    for (uint32_t i = 0; i < count; ++i) {
      (void)reader.string(form.limits.key_max_bytes);
      shape.child_roots.push_back(records.shape(reader).child_root);
    }
    reader.finish();
    return shape;
  }
  if (magic != form.branch_magic) throw DecodeError("not a tree node");
  (void)reader.u8();
  const auto count = reader.u32();
  for (uint32_t i = 0; i < count; ++i) {
    auto key = reader.string(form.limits.key_max_bytes);
    ObjectId child{reader.fixed<32>()};
    (void)reader.u64();
    shape.children.emplace_back(std::move(key), child);
  }
  reader.finish();
  return shape;
}

}  // namespace

void TreeShape::collect_child_sequence(const ObjectId& id, const TreeNodeStore& store, std::vector<ObjectId>& out) const {
  out.push_back(id);
  auto encoded = store.get(id);
  if (!encoded) throw DecodeError("tree child node unavailable: " + store::to_string(id));
  Reader reader(*encoded);
  const auto magic = reader.fixed<4>();
  if (magic == _form.child_leaf_magic) return;
  if (magic != _form.child_branch_magic) throw DecodeError("not a tree child node");
  for (const auto& child : read_sequence_children(reader)) collect_child_sequence(child, store, out);
}

void TreeShape::collect_all(const ObjectId& id, const TreeNodeStore& store, std::vector<ObjectId>& out, const TreePause& pause) const {
  if (pause) pause();
  out.push_back(id);
  const auto shape = read_shape(id, store, _form, _records);
  if (shape.leaf) {
    for (const auto& child_root : shape.child_roots)
      if (child_root) collect_child_sequence(*child_root, store, out);
    return;
  }
  for (const auto& [_, child] : shape.children) collect_all(child, store, out, pause);
}

void TreeShape::collect_nodes(const ObjectId& root, const TreeNodeStore& store, std::vector<ObjectId>& out, const TreePause& pause) const {
  collect_all(root, store, out, pause);
}

// The parallel walk. If `before` is absent, unreadable or of a different kind,
// nothing is pruned and the subtree under `after` is collected in full.
void TreeShape::collect_changed(const ObjectId& after, const std::optional<ObjectId>& before, const TreeNodeStore& store, std::vector<ObjectId>& out,
                                const TreePause& pause) const {
  if (before && *before == after) return;
  if (pause) pause();
  out.push_back(after);
  const auto shape = read_shape(after, store, _form, _records);
  if (shape.leaf) {
    // A changed leaf contributes the child sequences its counterpart did not
    // already name: those are what the change introduced. With no readable
    // counterpart leaf, all of them.
    std::set<ObjectId> named;
    if (before) {
      try {
        const auto old = read_shape(*before, store, _form, _records);
        if (old.leaf)
          for (const auto& child_root : old.child_roots)
            if (child_root) named.insert(*child_root);
      } catch (const contract::MaintenanceStopping&) {
        throw;
      } catch (const std::exception&) {
      }
    }
    for (const auto& child_root : shape.child_roots)
      if (child_root && !named.contains(*child_root)) collect_child_sequence(*child_root, store, out);
    return;
  }
  std::optional<NodeShape> old;
  if (before) {
    try {
      auto shape_before = read_shape(*before, store, _form, _records);
      if (!shape_before.leaf) old = std::move(shape_before);
    } catch (const contract::MaintenanceStopping&) {
      throw;
    } catch (const std::exception&) {
      // Unreadable old node: prune nothing, over-collect.
    }
  }
  for (const auto& [key, child] : shape.children) {
    std::optional<ObjectId> counterpart;
    if (old) {
      // Skip a child whose id appears on the old side; otherwise pair it by key
      // with the old child most likely to share descendants.
      bool shared = false;
      for (const auto& [_, old_child] : old->children)
        if (old_child == child) {
          shared = true;
          break;
        }
      if (shared) continue;
      for (const auto& [old_key, old_child] : old->children)
        if (old_key <= key) counterpart = old_child;
    }
    collect_changed(child, counterpart, store, out, pause);
  }
}

void TreeShape::collect_changes(const std::optional<ObjectId>& before, const ObjectId& after, const TreeNodeStore& store, std::vector<ObjectId>& out,
                                const TreePause& pause) const {
  collect_changed(after, before, store, out, pause);
}

std::vector<TreeNodeChild> TreeShape::node_children(std::span<const uint8_t> node) const {
  std::vector<TreeNodeChild> children;
  Reader reader(node);
  const auto magic = reader.fixed<4>();
  if (magic == _form.leaf_magic) {
    const auto count = reader.u32();
    for (uint32_t i = 0; i < count; ++i) {
      (void)reader.string(_form.limits.key_max_bytes);
      const auto shape = _records.shape(reader);
      for (const auto& id : shape.data) children.push_back({true, id});
      if (shape.child_root) children.push_back({false, *shape.child_root});
    }
  } else if (magic == _form.branch_magic) {
    (void)reader.u8();
    const auto count = reader.u32();
    for (uint32_t i = 0; i < count; ++i) {
      (void)reader.string(_form.limits.key_max_bytes);
      children.push_back({false, ObjectId{reader.fixed<32>()}});
      (void)reader.u64();
    }
  } else if (magic == _form.child_leaf_magic) {
    const auto count = reader.u32();
    for (uint32_t i = 0; i < count; ++i) {
      const auto id = _records.child_data(reader.view(_form.child_record_bytes));
      if (id) children.push_back({true, *id});
    }
  } else if (magic == _form.child_branch_magic) {
    (void)reader.u8();
    const auto count = reader.u32();
    for (uint32_t i = 0; i < count; ++i) {
      children.push_back({false, ObjectId{reader.fixed<32>()}});
      (void)reader.u64();
    }
  } else {
    throw DecodeError("not a tree node");
  }
  reader.finish();
  return children;
}

}  // namespace dagda::heads
