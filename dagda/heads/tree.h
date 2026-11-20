//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dagda/store/object_id.h"
#include "dannan/contract/time_source.h"
#include "dannan/net/codec.h"
#include "dannan/sched/walk.h"
#include "dannan/sched/work.h"

// A content-addressed Merkle tree over an owner's keys (requirement H18),
// moved from Macha's metadata/namespace_tree at 9a08846 and generalised: the
// branches are Dagda's, the leaf records the owner's. A node's id is its
// object id (store/object_id.h); a head carries only the root, and the nodes
// are CONTROL objects stored before any head names that root (H19).
//
// Two load-bearing properties:
//
// 1. History independence. The same entry set yields the same root however it
//    was reached, so nodes that reconcile to the same state agree on the root
//    and root comparison is a valid divergence witness. Hence boundaries are
//    chosen by hashing keys, not by fill factor.
//
// 2. Boundaries depend on keys only, never on values. A value change rewrites
//    exactly one leaf and its path to the root. A cap on item count is still a
//    function of the sorted keys and preserves property 1; a cap on encoded
//    bytes would not, so there is none.
//
// Every byte of a node comes from the form it is given (TreeForm) and the
// owner's codec (TreeValues), so a tree is reproduced exactly, on disk and in
// its hashes, by giving the same parameters.
namespace dagda::heads {

using dannan::net::DecodeError;
using store::Bytes;
using store::ObjectId;

// Sizes that shape a tree. Any change moves every root. The defaults are the
// ones every tree has used.
struct TreeLimits {
  // Average entries per leaf: the boundary test fires with probability
  // 1/target per key, so sizes vary geometrically.
  size_t entry_target_fanout = 32;
  // Hard cap on entries per leaf. Count-based, so still key-determined.
  size_t entry_max_fanout = 128;
  size_t branch_target_fanout = 32;
  size_t branch_max_fanout = 128;
  // At or below this many child records a value keeps them in its leaf; above
  // it they are a child sequence of their own (the owner's choice to honour).
  size_t child_inline_max = 8;
  size_t child_target_fanout = 256;
  size_t child_max_fanout = 1024;
  // The longest key a reader accepts. A writer does not check it.
  size_t key_max_bytes = 8192;
};

using NodeMagic = std::array<uint8_t, 4>;

// What makes one owner's tree its own: a node of the wrong kind is a decode
// failure, not a misread, and the domain strings separate the boundary tests so
// a key is not a boundary at every level at once. Parameters of a head space.
struct TreeForm {
  NodeMagic leaf_magic{};
  NodeMagic branch_magic{};
  NodeMagic child_leaf_magic{};
  NodeMagic child_branch_magic{};
  std::string entry_domain;
  std::string branch_domain;
  std::string child_domain;
  std::string child_branch_domain;
  // A child sequence's records are all this long.
  size_t child_record_bytes = 0;
  TreeLimits limits{};
};

// Whether a boundary test on `key` fires: the first eight bytes, big-endian, of
// SHA-256(domain, level, key), compared with u64 max / target. Waits on nothing.
uint64_t tree_boundary_hash(std::string_view domain, uint8_t level, std::span<const uint8_t> key);
bool is_tree_boundary(uint64_t hash, size_t target) noexcept;

// The exclusive upper bound of keys starting with `prefix`: the prefix with its
// last non-0xFF byte incremented and trailing 0xFF bytes dropped. Empty means
// unbounded.
std::string tree_prefix_upper_bound(std::string_view prefix);

// Where nodes are read and written. A put of bytes already held returns the same
// id and is not an error. A get that finds nothing is empty; bytes that cannot
// be opened are the implementation's throw, passed through.
class TreeNodeStore {
 public:
  virtual ~TreeNodeStore() = default;
  // The implementation's waits, whichever it is: at most these.
  static constexpr dannan::sched::Waits put_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::Waits get_waits = dannan::sched::local_waits | dannan::sched::Waits::network;
  virtual ObjectId put(std::span<const uint8_t> node) = 0;
  virtual std::optional<Bytes> get(const ObjectId& id) const = 0;
};

// An in-memory store, for tests and offline measurement. Single owner. Waits on
// nothing.
class MemoryTreeNodeStore final : public TreeNodeStore {
 public:
  ObjectId put(std::span<const uint8_t> node) override;
  std::optional<Bytes> get(const ObjectId& id) const override;
  // Stores bytes under an id they need not hash to, simulating a corrupt
  // control-store object. Tests only.
  void put_at(const ObjectId& id, Bytes node);

  size_t nodes() const noexcept { return _nodes.size(); }
  // Reads served since the last `forget_reads`; lets a caller prove a stat-only
  // read fetches no child node.
  size_t reads() const noexcept { return _reads; }
  void forget_reads() noexcept { _reads = 0; }
  uint64_t bytes() const noexcept { return _bytes; }
  // Ids newly written (not already present): the dirty set a commit would
  // replicate.
  const std::vector<ObjectId>& written() const noexcept { return _written; }
  void forget_written() noexcept { _written.clear(); }

 private:
  std::map<ObjectId, Bytes> _nodes;
  std::vector<ObjectId> _written;
  uint64_t _bytes = 0;
  mutable size_t _reads = 0;
};

class TreeRecords;

// The child-tree hook, written side: what an owner's codec calls to put a
// value's long record list into nodes of its own. Chunk boundaries hash each
// record's boundary key (TreeRecords::child_boundary), so an append cannot move
// existing boundaries and rewrites only the last chunk and the spine.
class TreeChildWriter {
 public:
  TreeChildWriter(TreeNodeStore& store, const TreeForm& form, const TreeRecords& records) noexcept : _store(store), _form(form), _records(records) {}
  size_t inline_max() const noexcept { return _form.limits.child_inline_max; }
  // `records` is a whole number of records of the form's length, at least one.
  // Throws std::logic_error otherwise. Returns the sequence's root.
  ObjectId build(std::span<const uint8_t> records);

 private:
  TreeNodeStore& _store;
  const TreeForm& _form;
  const TreeRecords& _records;
};

// The hook's read side.
class TreeChildReader {
 public:
  TreeChildReader(const TreeNodeStore& store, const TreeForm& form) noexcept : _store(store), _form(form) {}
  // Appends the sequence's records, whole and in order. Throws DecodeError if a
  // node cannot be read or is not a node of the sequence.
  void read(const ObjectId& root, Bytes& out) const;

 private:
  const TreeNodeStore& _store;
  const TreeForm& _form;
};

// One record's references as its leaf holds them, without fetching anything.
struct TreeRecordShape {
  // Inline DATA references.
  std::vector<ObjectId> data;
  // How many child records the value says it has, inline or in its sequence.
  uint64_t items = 0;
  std::optional<ObjectId> child_root;
};

// What a tree needs to know of a leaf record's value whatever its type. A record
// has no length of its own, so Dagda cannot walk a leaf without these. Thread-safe.
class TreeRecords {
 public:
  virtual ~TreeRecords() = default;
  // Steps over the value, reading only what lies in the leaf. Throws DecodeError.
  virtual void skip(dannan::net::Reader& value) const = 0;
  // Steps over the value and says what it references. Throws DecodeError.
  virtual TreeRecordShape shape(dannan::net::Reader& value) const = 0;
  // What a child record is hashed by for its chunk boundary (it must be a part
  // of the record) and the DATA it names, if it names any.
  virtual std::span<const uint8_t> child_boundary(std::span<const uint8_t> record) const = 0;
  virtual std::optional<ObjectId> child_data(std::span<const uint8_t> record) const = 0;
};

// What one value references (section 2.5): the census's counts come from this.
struct TreeReferences {
  std::vector<ObjectId> data;
  // The child-sequence nodes it holds; none when its records are inline. Filled
  // by the tree.
  std::vector<ObjectId> children;
  // How many child records it has, whether or not each names DATA.
  uint64_t items = 0;
};

// The value codec: the owner's half of a leaf record.
template <class Value>
class TreeValues : public TreeRecords {
 public:
  // Writes the value after its key. A value with a long record list puts it
  // through `children`.
  virtual void encode(dannan::net::Writer& out, const Value& value, TreeChildWriter& children) const = 0;
  // Reads what encode wrote. With `with_children` false nothing is fetched: no
  // child node is read and the value's child records are left out.
  virtual Value decode(dannan::net::Reader& in, const TreeChildReader& children, bool with_children) const = 0;
  // What the value references. It must call children.build exactly as encode
  // does (the tree takes the ids from those calls) and fills `data` and `items`.
  // Reads nothing.
  virtual TreeReferences references(const Value& value, TreeChildWriter& children) const = 0;
};

struct TreeStats {
  uint64_t entries = 0;
  uint64_t items = 0;
  uint64_t leaves = 0;
  uint64_t branches = 0;
  uint64_t child_nodes = 0;
  uint64_t bytes = 0;
  uint64_t largest_node_bytes = 0;
  size_t depth = 0;
};

// One direct child of a tree node: another tree node or a DATA object.
struct TreeNodeChild {
  bool data = false;
  ObjectId id{};
  auto operator<=>(const TreeNodeChild&) const = default;
};

// What a scan does after an entry: go on, end, or jump to the first entry at or
// after a later key.
struct TreeScanStep {
  enum class Kind : uint8_t { next, stop, seek };
  Kind kind = Kind::next;
  std::string to;
  static TreeScanStep next() { return {}; }
  static TreeScanStep stop() { return {Kind::stop, {}}; }
  static TreeScanStep seek(std::string key) { return {Kind::seek, std::move(key)}; }
};

// A long walk's stop: when given it runs between the entries or nodes the walk
// takes and may throw contract::MaintenanceStopping. It passes through the walk
// untouched.
using TreePause = std::function<void()>;

// The part of a tree that does not depend on the type of its values. Every read
// is safe from any thread if the store's are; the store's waits are the
// operation's. A node that cannot be read throws DecodeError unless an operation
// says otherwise, so an incomplete answer is never taken for a complete one.
class TreeShape {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::thread_safe;
  static constexpr dannan::sched::Waits read_waits = TreeNodeStore::get_waits;
  static constexpr dannan::sched::Waits write_waits = TreeNodeStore::get_waits | TreeNodeStore::put_waits;

  // `records` must outlive the tree.
  TreeShape(const TreeRecords& records, TreeForm form) : _records(records), _form(std::move(form)) {}

  const TreeForm& form() const noexcept { return _form; }

  // The branch and leaf nodes one tree holds and the other does not: `added` in
  // `after` only, `removed` in `before` only, each sorted. Child sequences are
  // not among them: one may be shared by entries anywhere in a tree, so they are
  // counted by entry (Tree::references). Reads only where the trees differ.
  void diff_nodes(const ObjectId& before, const ObjectId& after, const TreeNodeStore& store, std::vector<ObjectId>& added, std::vector<ObjectId>& removed,
                  const TreePause& pause = {}) const;

  TreeStats stats(const ObjectId& root, const TreeNodeStore& store) const;

  // Every control object in the tree: branches, leaves and child sequences, for
  // the reachability live set. An incomplete live set must not be deleted
  // against, so a node that cannot be read throws.
  void collect_nodes(const ObjectId& root, const TreeNodeStore& store, std::vector<ObjectId>& out, const TreePause& pause = {}) const;

  // Nodes reachable from `after` that `before` does not share: what a commit
  // must claim. Subtrees with the same id on both sides are pruned unread. A
  // changed leaf contributes the child sequences its counterpart did not name;
  // an old node that cannot be read prunes nothing, over-collecting, the safe
  // direction. No `before` means everything is new.
  void collect_changes(const std::optional<ObjectId>& before, const ObjectId& after, const TreeNodeStore& store, std::vector<ObjectId>& out,
                       const TreePause& pause = {}) const;

  // A node's direct children in the order it lists them: a function of its bytes
  // alone, so every holder numbers its children alike. Throws DecodeError on
  // anything that is not a node of this form.
  std::vector<TreeNodeChild> node_children(std::span<const uint8_t> node) const;

 protected:
  // A spine node's child. `items` counts the entries beneath it, so a subtree
  // can be sized without descending. The key is empty on a child sequence.
  struct Child {
    std::string first_key;
    ObjectId id{};
    uint64_t items = 0;
  };
  // A node standing in a spine sequence. One whose level is above the level
  // being built stands for everything beneath it, unread.
  struct SpineItem {
    Child child;
    uint8_t level = 0;
    // Written by this update, not carried over from the tree it started from.
    bool fresh = false;
  };
  // A node's own level (0 for a leaf) and, for a branch, its children.
  struct SpineNode {
    uint8_t level = 0;
    std::vector<Child> children;
  };
  // A leaf record as it lies in its leaf.
  struct LeafRecord {
    std::string key;
    std::span<const uint8_t> raw;
  };

  bool ends_a_leaf(std::string_view key) const;
  ObjectId empty_leaf(TreeNodeStore& store) const;
  ObjectId build_spine(std::vector<Child> children, TreeNodeStore& store) const;
  SpineNode read_spine_node(const ObjectId& id, const TreeNodeStore& store) const;
  void expand_spine_item(std::vector<SpineItem>& items, size_t at, const TreeNodeStore& store) const;
  std::vector<SpineItem> splice_spine_level(std::vector<SpineItem> items, uint8_t level, TreeNodeStore& store) const;
  // A leaf's records by key, each with its encoded bytes: nothing a value refers
  // to is read. `backing` keeps the leaf's bytes alive.
  void read_leaf_records(const ObjectId& id, const TreeNodeStore& store, std::deque<Bytes>& backing, std::vector<LeafRecord>& out) const;
  // A branch's (first key, id) children, the items passed over.
  std::vector<std::pair<std::string, ObjectId>> read_branch_keys(dannan::net::Reader& reader) const;

  const TreeRecords& _records;
  const TreeForm _form;

 private:
  friend class TreeChildWriter;
  friend class TreeChildReader;

  bool branch_boundary(const Child& child, uint8_t level, bool keyed, size_t target) const;
  Child put_spine_node(const std::vector<Child>& run, uint8_t level, bool keyed, TreeNodeStore& store) const;
  std::vector<Child> build_spine_level(const std::vector<Child>& children, uint8_t level, TreeNodeStore& store, bool keyed, size_t target,
                                       size_t maximum) const;
  ObjectId build_spine(std::vector<Child> children, TreeNodeStore& store, bool keyed, size_t target, size_t maximum) const;
  void expand_spine_to(std::vector<SpineItem>& items, uint8_t level, const TreeNodeStore& store) const;
  void walk_stats(const ObjectId& id, const TreeNodeStore& store, TreeStats& stats, size_t depth) const;
  void walk_child_stats(const ObjectId& id, const TreeNodeStore& store, TreeStats& stats) const;
  void collect_child_sequence(const ObjectId& id, const TreeNodeStore& store, std::vector<ObjectId>& out) const;
  void collect_all(const ObjectId& id, const TreeNodeStore& store, std::vector<ObjectId>& out, const TreePause& pause) const;
  void collect_changed(const ObjectId& after, const std::optional<ObjectId>& before, const TreeNodeStore& store, std::vector<ObjectId>& out,
                       const TreePause& pause) const;
};

// The tree over one owner's values. `Value` is regular, with operator==.
template <class Value>
class Tree : public TreeShape {
 public:
  using Entries = std::map<std::string, Value>;
  // A change set: a value to upsert, or nothing to delete the key.
  using Changes = std::map<std::string, std::optional<Value>>;
  using Visitor = std::function<void(const std::string& key, const Value& value)>;
  using ScanVisitor = std::function<TreeScanStep(const std::string& key, const Value& value)>;
  using Item = std::pair<std::string, Value>;

  // A key whose value differs between two trees; absent on a side is none.
  struct Difference {
    std::optional<Value> before;
    std::optional<Value> after;
    bool operator==(const Difference&) const = default;
  };
  using Differences = std::map<std::string, Difference>;

  // What a head's delta does to the tree: erases, then upserts, then appends.
  // An append runs on this delta's pending value, else the tree's, and throws
  // if the value is not what it was made against. A missing one is refused.
  struct Delta {
    std::vector<std::string> erase;
    Entries upsert;
    std::map<std::string, std::function<void(Value&)>> append;
  };

  Tree(const TreeValues<Value>& values, TreeForm form) : TreeShape(values, std::move(form)), _values(values) {}

  // Builds the tree and returns the root. Deterministic in the entry set alone.
  // An empty set is one empty leaf, so a root always exists.
  ObjectId build(const Entries& entries, TreeNodeStore& store) const {
    std::vector<EntryRef> ordered;
    ordered.reserve(entries.size());
    for (const auto& [key, value] : entries) ordered.emplace_back(&key, &value);
    auto leaves = chunk_leaves(ordered, store);
    if (leaves.empty()) return empty_leaf(store);
    return build_spine(std::move(leaves), store);
  }

  // Applies a change set to an existing tree and returns the new root. The
  // result is byte-identical to `build` over the resulting entries (property 1).
  //
  // It reads the path to each leaf a change falls in, cuts those leaves again,
  // and regroups the nodes above only as far as the groups differ from the old
  // ones. A subtree no change falls in is neither read nor written, and an entry
  // that does not change is copied as its leaf holds it, so its child sequence
  // is not read either. The cost follows the change and the tree's depth.
  ObjectId update(const ObjectId& root, TreeNodeStore& store, const Changes& changes) const {
    if (changes.empty()) return root;

    // The leaf sequence, with every subtree no change falls in left unread. A
    // change belongs to the last leaf whose first key is at or before its key,
    // else to the first leaf (where a full build would put it).
    std::vector<SpineItem> sequence;
    sequence.push_back({Child{{}, root, 0}, read_spine_node(root, store).level, false});
    std::vector<SpineItem> leaves;
    std::deque<Bytes> backing;
    size_t at = 0;
    auto change = changes.begin();
    while (change != changes.end()) {
      while (at + 1 < sequence.size() && sequence[at + 1].child.first_key <= change->first) leaves.push_back(std::move(sequence[at++]));
      if (at < sequence.size() && sequence[at].level > 0) {
        expand_spine_item(sequence, at, store);
        continue;
      }

      // A window of leaves: read, changed, cut into leaves again. While the last
      // leaf cut is open (did not end on a boundary key) the window takes the
      // next leaf, until its cuts fall where a full build's do.
      std::vector<LeafRecord> window;
      backing.clear();
      if (at < sequence.size()) read_leaf_records(sequence[at++].child.id, store, backing, window);
      const auto first_change = change;
      std::vector<Child> cut;
      for (;;) {
        const std::string* beyond = at < sequence.size() ? &sequence[at].child.first_key : nullptr;
        while (change != changes.end() && (!beyond || change->first < *beyond)) ++change;
        std::vector<NewRecord> merged;
        merged.reserve(window.size() + 8);
        auto record = window.begin();
        for (auto applied = first_change; record != window.end() || applied != change;) {
          if (applied == change || (record != window.end() && record->key < applied->first)) {
            merged.push_back({record->key, record->raw, nullptr});
            ++record;
            continue;
          }
          if (record != window.end() && record->key == applied->first) ++record;
          if (applied->second) merged.push_back({applied->first, {}, &*applied->second});
          ++applied;
        }
        bool open = false;
        cut = chunk_records(merged, store, open);
        // An open tail with nothing left to take is the sequence end, where a
        // full build flushes too.
        if (!open || at >= sequence.size()) break;
        while (sequence[at].level > 0) expand_spine_item(sequence, at, store);
        read_leaf_records(sequence[at++].child.id, store, backing, window);
      }
      for (auto& leaf : cut) leaves.push_back({std::move(leaf), 0, true});
    }
    for (; at < sequence.size(); ++at) leaves.push_back(std::move(sequence[at]));

    uint8_t level = 0;
    for (;;) {
      if (leaves.empty()) return empty_leaf(store);
      if (leaves.size() == 1) {
        // A full build stops at the first level that is one node. A subtree kept
        // unread may stand over a chain of single children, left when its
        // neighbours went: the root is the foot of it.
        auto id = leaves.front().child.id;
        for (auto above = leaves.front().level; above > level; --above) {
          auto node = read_spine_node(id, store);
          if (node.children.size() != 1) break;
          id = node.children.front().id;
        }
        return id;
      }
      leaves = splice_spine_level(std::move(leaves), level, store);
      level = level < 255 ? static_cast<uint8_t>(level + 1) : level;
    }
  }

  // Applies a delta. The owner's own checks on the result are the owner's.
  ObjectId apply(const ObjectId& root, TreeNodeStore& store, const Delta& delta) const {
    Changes changes;
    for (const auto& key : delta.erase) changes[key] = std::nullopt;
    for (const auto& [key, value] : delta.upsert) changes[key] = value;
    for (const auto& [key, append] : delta.append) {
      std::optional<Value> base;
      if (const auto pending = changes.find(key); pending != changes.end()) {
        base = pending->second;
      } else {
        base = lookup(root, key, store, true);
      }
      if (!base) throw DecodeError("tree delta append base mismatch");
      append(*base);
      changes[key] = std::move(base);
    }
    return update(root, store, changes);
  }

  // Visits entries in key order without materialising them. With `nodes`, also
  // appends every node it read: branches, leaves and child sequences, which a
  // complete walk reads all of, so a walk that returns has named the whole tree.
  void walk(const ObjectId& root, const TreeNodeStore& store, const Visitor& visit, const TreePause& pause = {}, std::vector<ObjectId>* nodes = nullptr) const {
    const Visitor visited = pause ? Visitor([&](const std::string& key, const Value& value) {
      pause();
      visit(key, value);
    })
                                  : visit;
    if (nodes) {
      const Recording recording(store, *nodes);
      walk_subtree(root, recording, visited);
      return;
    }
    walk_subtree(root, store, visited);
  }

  // Materialises the whole tree. For round-trip proofs; nothing on a hot path
  // should need it.
  Entries read(const ObjectId& root, const TreeNodeStore& store) const {
    Entries out;
    walk_subtree(root, store, [&](const std::string& key, const Value& value) {
      if (!out.emplace(key, value).second) throw DecodeError("tree contains a duplicate key");
    });
    return out;
  }

  // One key in O(log n) node fetches. With `with_children` false it fetches the
  // path to the leaf and no child node at all. A key below the first child's key
  // is absent.
  std::optional<Value> lookup(const ObjectId& root, std::string_view key, const TreeNodeStore& store, bool with_children = true) const {
    ObjectId current = root;
    for (;;) {
      auto encoded = store.get(current);
      if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(current));
      dannan::net::Reader reader(*encoded);
      const auto magic = reader.fixed<4>();
      if (magic == _form.leaf_magic) {
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
          auto item = decode_record(reader, store, false, with_children ? key : std::string_view{});
          if (item.first == key) return std::move(item.second);
          // Leaf records are in key order: a later key means absent.
          if (item.first > key) return std::nullopt;
        }
        return std::nullopt;
      }
      if (magic != _form.branch_magic) throw DecodeError("not a tree node");
      (void)reader.u8();
      const auto count = reader.u32();
      std::optional<ObjectId> next;
      for (uint32_t i = 0; i < count; ++i) {
        auto first_key = reader.string(_form.limits.key_max_bytes);
        ObjectId child{reader.fixed<32>()};
        (void)reader.u64();
        if (first_key <= key) {
          next = child;
        } else if (next) {
          break;
        }
      }
      if (!next) return std::nullopt;
      current = *next;
    }
  }

  // Existence only: copies no child record and fetches no child node.
  bool contains(const ObjectId& root, std::string_view key, const TreeNodeStore& store) const { return lookup(root, key, store, false).has_value(); }

  // Every entry whose key starts with `prefix`, in key order. A branch child
  // whose key range cannot intersect the prefix is never fetched, so a listing
  // costs the subtree rather than the tree.
  void for_each_with_prefix(const ObjectId& root, const TreeNodeStore& store, std::string_view prefix, const Visitor& visit) const {
    walk_subtree_prefix(root, store, prefix, tree_prefix_upper_bound(prefix), visit);
  }

  // Visits entries at or after `from` in key order, without child records, until
  // the visitor ends it. Each leaf reached costs one descent, a seek costs one
  // more, and nothing between is read: the cost follows what is visited, not the
  // tree. A seek at or before the current key throws std::logic_error.
  void scan(const ObjectId& root, const TreeNodeStore& store, std::string_view from, const ScanVisitor& visit) const {
    std::string cursor(from);
    for (;;) {
      auto run = leaf_run_from(root, store, cursor);
      bool sought = false;
      for (size_t i = 0; i < run.items.size(); ++i) {
        const auto& [key, value] = run.items[i];
        auto step = visit(key, value);
        if (step.kind == TreeScanStep::Kind::stop) return;
        if (step.kind != TreeScanStep::Kind::seek) continue;
        if (step.to <= key) throw std::logic_error("tree scan sought backwards");
        // A target inside this leaf is reached by stepping over what lies before
        // it; only one beyond the leaf costs a descent.
        if (!run.next || step.to < *run.next) {
          while (i + 1 < run.items.size() && run.items[i + 1].first < step.to) ++i;
          continue;
        }
        cursor = std::move(step.to);
        sought = true;
        break;
      }
      if (sought) continue;
      if (!run.next) return;
      cursor = std::move(*run.next);
    }
  }

  // The entries after `from`, in key order, one budget operation per entry.
  // Subtrees ending at or before the cursor are skipped, so a resumed page reads
  // only its own path. At the end the cursor is the start again and the page is
  // complete. No store, or a node that cannot be fetched, throws: a tree is
  // never read as empty for want of one. `time` is what the budget's deadline is
  // judged by.
  dannan::sched::Page<Item, std::string> entries(const ObjectId& root, const TreeNodeStore* store, dannan::sched::Cursor<std::string> from,
                                                 dannan::sched::Budget& budget, const dannan::contract::TimeSource& time) const {
    using dannan::sched::Stop;
    dannan::sched::Page<Item, std::string> page;
    page.next = from;
    if (!store) throw DecodeError("tree read with no node store");
    // One operation per entry; stops before an entry it cannot pay for.
    const std::function<bool(Item&&)> take = [&](Item&& item) {
      if (const auto stop = budget.must_stop(time.now())) {
        page.stopped = *stop;
        return false;
      }
      if (!budget.take_operation()) {
        page.stopped = Stop::budget;
        return false;
      }
      page.next.after = item.first;
      page.items.push_back(std::move(item));
      return true;
    };
    if (!walk_subtree_after(root, *store, from.after, take)) {
      page.next = {};
      page.stopped = Stop::end;
    }
    return page;
  }

  // Every key whose entry differs, child records included. Reads both trees'
  // branch nodes and the leaves only one of them has: the cost follows what
  // differs. `pause` runs per node opened.
  Differences diff(const ObjectId& before, const ObjectId& after, const TreeNodeStore& store, const TreePause& pause = {}) const {
    Differences out;
    if (before == after) return out;
    // Two streams of entries, walked together. Where both stand at the start of
    // the same node, that subtree is the same in both and is passed over unread;
    // otherwise the one standing higher is opened. Entries are compared as their
    // leaves hold them, and only one that differs is decoded, children and all.
    DiffStream old_side(*this, store, before, pause);
    DiffStream new_side(*this, store, after, pause);
    const auto removed = [&](const LeafRecord& record) { out[record.key].before = decode_raw_record(record, store); };
    const auto added = [&](const LeafRecord& record) { out[record.key].after = decode_raw_record(record, store); };
    for (;;) {
      if (!old_side.buffered() && !new_side.buffered()) {
        if (old_side.pending.empty() && new_side.pending.empty()) break;
        if (old_side.pending.empty()) {
          new_side.fill();
          continue;
        }
        if (new_side.pending.empty()) {
          old_side.fill();
          continue;
        }
        const auto& a = old_side.pending.back();
        const auto& b = new_side.pending.back();
        if (a.level == b.level && a.child.id == b.child.id) {
          old_side.pending.pop_back();
          new_side.pending.pop_back();
        } else if (a.level > b.level) {
          old_side.open();
        } else if (b.level > a.level) {
          new_side.open();
        } else {
          old_side.open();
          new_side.open();
        }
        continue;
      }
      if (!old_side.buffered()) {
        old_side.fill();
        if (!old_side.buffered()) added(new_side.records[new_side.next++]);
        continue;
      }
      if (!new_side.buffered()) {
        new_side.fill();
        if (!new_side.buffered()) removed(old_side.records[old_side.next++]);
        continue;
      }
      const auto& a = old_side.records[old_side.next];
      const auto& b = new_side.records[new_side.next];
      if (a.key < b.key) {
        removed(a);
        ++old_side.next;
      } else if (b.key < a.key) {
        added(b);
        ++new_side.next;
      } else {
        if (!std::equal(a.raw.begin(), a.raw.end(), b.raw.begin(), b.raw.end())) {
          Difference difference;
          difference.before = decode_raw_record(a, store);
          difference.after = decode_raw_record(b, store);
          if (difference.before != difference.after) out.emplace(a.key, std::move(difference));
        }
        ++old_side.next;
        ++new_side.next;
      }
    }
    return out;
  }

  // What the value references, its child nodes computed from it alone: it reads
  // nothing and writes nothing.
  TreeReferences references(const Value& value) const {
    AddressOnly addresses;
    TreeChildWriter children(addresses, _form, _values);
    auto out = _values.references(value, children);
    out.children = std::move(addresses.ids);
    return out;
  }

  // The leaf that holds `key`, or would: its id and every entry in it, without child
  // records. One descent; no child node is read. What an owner sweeping a leaf it is
  // about to rewrite reads.
  struct Leaf {
    ObjectId id{};
    std::vector<Item> items;
  };
  Leaf leaf_holding(const ObjectId& root, std::string_view key, const TreeNodeStore& store) const {
    auto run = leaf_run_from(root, store, key, true);
    return {run.leaf, std::move(run.items)};
  }

  // Each entry at or after `from` in key order, child records read, until `take`
  // returns false. A subtree ending before `from` is not read.
  void walk_from(const ObjectId& root, const TreeNodeStore& store, std::string_view from, const std::function<bool(Item&&)>& take) const {
    (void)walk_subtree_after(root, store, std::string(from), take, true);
  }

 private:
  using EntryRef = std::pair<const std::string*, const Value*>;

  // A record to cut into a leaf: one that lies in a leaf already is copied as it
  // stands, so an untouched neighbour's children are neither read nor rebuilt.
  struct NewRecord {
    std::string key;
    std::span<const uint8_t> raw;
    const Value* value = nullptr;
  };

  // Names every node read through it.
  class Recording final : public TreeNodeStore {
   public:
    Recording(const TreeNodeStore& inner, std::vector<ObjectId>& read) : _inner(inner), _read(read) {}
    ObjectId put(std::span<const uint8_t>) override { throw std::logic_error("a tree walk does not write"); }
    std::optional<Bytes> get(const ObjectId& id) const override {
      auto node = _inner.get(id);
      if (node) _read.push_back(id);
      return node;
    }

   private:
    const TreeNodeStore& _inner;
    std::vector<ObjectId>& _read;
  };

  // Records what would be written, and writes nothing.
  class AddressOnly final : public TreeNodeStore {
   public:
    ObjectId put(std::span<const uint8_t> node) override {
      const auto id = store::object_id(node);
      ids.push_back(id);
      return id;
    }
    std::optional<Bytes> get(const ObjectId&) const override { return std::nullopt; }
    std::vector<ObjectId> ids;
  };

  void encode_record(dannan::net::Writer& writer, const std::string& key, const Value& value, TreeNodeStore& store) const {
    writer.string(key);
    TreeChildWriter children(store, _form, _values);
    _values.encode(writer, value, children);
  }

  // Reads one record. Child nodes are fetched only for an entry whose key `want`
  // accepts; the decision follows the key parse, so a reader pays nothing for
  // entries it walks past.
  template <class Want>
  Item decode_record_when(dannan::net::Reader& reader, const TreeNodeStore& store, const Want& want) const {
    auto key = reader.string(_form.limits.key_max_bytes);
    const bool load = want(key);
    const TreeChildReader children(store, _form);
    auto value = _values.decode(reader, children, load);
    return {std::move(key), std::move(value)};
  }

  // Children for every entry when `load_all`, otherwise only for the entry whose
  // key equals `load_only_for`. `false` and empty fetches none.
  Item decode_record(dannan::net::Reader& reader, const TreeNodeStore& store, bool load_all, std::string_view load_only_for = {}) const {
    return decode_record_when(reader, store, [&](const std::string& key) { return load_all || (!load_only_for.empty() && key == load_only_for); });
  }

  Value decode_raw_record(const LeafRecord& record, const TreeNodeStore& store) const {
    dannan::net::Reader reader(record.raw);
    auto item = decode_record(reader, store, true);
    reader.finish();
    return std::move(item.second);
  }

  // Chunks entries into leaves exactly as a full build does.
  std::vector<Child> chunk_leaves(const std::vector<EntryRef>& entries, TreeNodeStore& store) const {
    std::vector<Child> leaves;
    std::vector<EntryRef> run;
    const auto flush = [&] {
      if (run.empty()) return;
      dannan::net::Writer writer;
      writer.raw(_form.leaf_magic);
      writer.u32(static_cast<uint32_t>(run.size()));
      for (const auto& [key, value] : run) encode_record(writer, *key, *value, store);
      const auto encoded = writer.take();
      leaves.push_back(Child{*run.front().first, store.put(encoded), static_cast<uint64_t>(run.size())});
      run.clear();
    };
    for (const auto& item : entries) {
      run.push_back(item);
      if (ends_a_leaf(*item.first) || run.size() >= _form.limits.entry_max_fanout) flush();
    }
    flush();
    return leaves;
  }

  // chunk_leaves over records. `open` is set when the final run did not end on a
  // boundary key: an update window has not yet re-synchronised with the global
  // partition and must absorb the next leaf.
  std::vector<Child> chunk_records(const std::vector<NewRecord>& records, TreeNodeStore& store, bool& open) const {
    std::vector<Child> leaves;
    size_t run_begin = 0;
    bool ended_on_boundary = true;
    const auto flush = [&](size_t run_end) {
      if (run_end == run_begin) return;
      dannan::net::Writer writer;
      writer.raw(_form.leaf_magic);
      writer.u32(static_cast<uint32_t>(run_end - run_begin));
      for (size_t i = run_begin; i < run_end; ++i) {
        if (records[i].value) {
          encode_record(writer, records[i].key, *records[i].value, store);
        } else {
          writer.raw(records[i].raw);
        }
      }
      const auto encoded = writer.take();
      leaves.push_back(Child{records[run_begin].key, store.put(encoded), static_cast<uint64_t>(run_end - run_begin)});
      run_begin = run_end;
    };
    for (size_t i = 0; i < records.size(); ++i) {
      if (ends_a_leaf(records[i].key) || i + 1 - run_begin >= _form.limits.entry_max_fanout) {
        ended_on_boundary = true;
        flush(i + 1);
      } else {
        ended_on_boundary = false;
      }
    }
    flush(records.size());
    open = !ended_on_boundary;
    return leaves;
  }

  void walk_subtree(const ObjectId& id, const TreeNodeStore& store, const Visitor& visit) const {
    auto encoded = store.get(id);
    if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(id));
    dannan::net::Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == _form.leaf_magic) {
      const auto count = reader.u32();
      for (uint32_t i = 0; i < count; ++i) {
        auto item = decode_record(reader, store, true);
        visit(item.first, item.second);
      }
      reader.finish();
      return;
    }
    if (magic != _form.branch_magic) throw DecodeError("not a tree node");
    (void)reader.u8();
    const auto children = read_branch_keys(reader);
    for (const auto& child : children) walk_subtree(child.second, store, visit);
  }

  void walk_subtree_prefix(const ObjectId& id, const TreeNodeStore& store, std::string_view prefix, std::string_view upper, const Visitor& visit) const {
    auto encoded = store.get(id);
    if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(id));
    dannan::net::Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == _form.leaf_magic) {
      const auto count = reader.u32();
      for (uint32_t i = 0; i < count; ++i) {
        auto item = decode_record_when(reader, store, [&](const std::string& key) { return key.starts_with(prefix); });
        if (item.first.starts_with(prefix)) visit(item.first, item.second);
      }
      reader.finish();
      return;
    }
    if (magic != _form.branch_magic) throw DecodeError("not a tree node");
    (void)reader.u8();
    const auto children = read_branch_keys(reader);
    for (size_t i = 0; i < children.size(); ++i) {
      // A child spans its first key up to the next child's; skip it when that
      // range lies wholly before the prefix or at or after `upper`.
      const bool last = i + 1 == children.size();
      if (!last && children[i + 1].first <= prefix) continue;
      if (!upper.empty() && children[i].first >= upper) break;
      walk_subtree_prefix(children[i].second, store, prefix, upper, visit);
    }
  }

  // Feeds entries after `after` (at or after it when `inclusive`) to `take` until it
  // returns false; returns whether it stopped. A child whose successor starts at or
  // before `after` is not read.
  bool walk_subtree_after(const ObjectId& id, const TreeNodeStore& store, const std::optional<std::string>& after, const std::function<bool(Item&&)>& take,
                          bool inclusive = false) const {
    auto encoded = store.get(id);
    if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(id));
    dannan::net::Reader reader(*encoded);
    const auto magic = reader.fixed<4>();
    if (magic == _form.leaf_magic) {
      const auto count = reader.u32();
      for (uint32_t i = 0; i < count; ++i) {
        auto item = decode_record(reader, store, true);
        if (after && (inclusive ? item.first < *after : item.first <= *after)) continue;
        if (!take(std::move(item))) return true;
      }
      reader.finish();
      return false;
    }
    if (magic != _form.branch_magic) throw DecodeError("not a tree node");
    (void)reader.u8();
    const auto children = read_branch_keys(reader);
    for (size_t i = 0; i < children.size(); ++i) {
      if (after && i + 1 < children.size() && children[i + 1].first <= *after) continue;
      if (walk_subtree_after(children[i].second, store, after, take, inclusive)) return true;
    }
    return false;
  }

  // The entries at or after `from` in the leaf that would hold it (every entry in it
  // when `whole`), without child records, the leaf's id, and the first key of the leaf
  // after: one descent.
  struct LeafRun {
    std::vector<Item> items;
    std::optional<std::string> next;
    ObjectId leaf{};
  };

  LeafRun leaf_run_from(const ObjectId& root, const TreeNodeStore& store, std::string_view from, bool whole = false) const {
    LeafRun run;
    ObjectId current = root;
    for (;;) {
      auto encoded = store.get(current);
      if (!encoded) throw DecodeError("tree node unavailable: " + store::to_string(current));
      dannan::net::Reader reader(*encoded);
      const auto magic = reader.fixed<4>();
      if (magic == _form.leaf_magic) {
        const auto count = reader.u32();
        for (uint32_t i = 0; i < count; ++i) {
          auto item = decode_record(reader, store, false);
          if (whole || item.first >= from) run.items.push_back(std::move(item));
        }
        reader.finish();
        run.leaf = current;
        return run;
      }
      if (magic != _form.branch_magic) throw DecodeError("not a tree node");
      (void)reader.u8();
      const auto count = reader.u32();
      if (!count) throw DecodeError("tree branch without children");
      // The last child whose first key is at or before `from`, else the first;
      // the child after it bounds this subtree from above.
      ObjectId chosen{};
      bool have = false;
      std::optional<std::string> after;
      for (uint32_t i = 0; i < count; ++i) {
        auto first_key = reader.string(_form.limits.key_max_bytes);
        ObjectId child{reader.fixed<32>()};
        (void)reader.u64();
        if (!have || first_key <= from) {
          chosen = child;
          have = true;
          continue;
        }
        after = std::move(first_key);
        break;
      }
      if (after) run.next = std::move(after);
      current = chosen;
    }
  }

  // One tree's entries in key order, read a node at a time. `pending` holds what
  // has not been opened, next at the back; `records` the leaf being read, each as
  // encoded. Equal bytes are equal entries, children included: a child sequence's
  // root is a function of its records.
  struct DiffStream {
    const Tree& tree;
    const TreeNodeStore& store;
    const TreePause& pause;
    std::vector<SpineItem> pending;
    std::deque<Bytes> backing;
    std::vector<LeafRecord> records;
    size_t next = 0;

    DiffStream(const Tree& owner, const TreeNodeStore& nodes, const ObjectId& root, const TreePause& stop) : tree(owner), store(nodes), pause(stop) {
      pending.push_back({Child{{}, root, 0}, tree.read_spine_node(root, nodes).level, false});
    }
    bool buffered() const { return next < records.size(); }
    // Opens the next node: a branch into its children, a leaf into records.
    void open() {
      if (pause) pause();
      auto item = std::move(pending.back());
      pending.pop_back();
      if (item.level == 0) {
        records.clear();
        backing.clear();
        next = 0;
        tree.read_leaf_records(item.child.id, store, backing, records);
        return;
      }
      auto node = tree.read_spine_node(item.child.id, store);
      if (node.level != item.level) throw DecodeError("tree node at an unexpected level");
      for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
        pending.push_back({std::move(*child), static_cast<uint8_t>(item.level - 1), false});
    }
    // Reads on until a record is buffered or nothing is left.
    void fill() {
      while (!buffered() && !pending.empty()) open();
    }
  };

  const TreeValues<Value>& _values;
};

}  // namespace dagda::heads
