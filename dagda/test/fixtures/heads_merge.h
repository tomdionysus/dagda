//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// The neutral owner's merge, behind SpaceSpec::merge: what Macha's merge_metadata_heads_over
// and merge_tree_backed_heads do at 98b816d, with their filesystem taken out. Two heads say
// everything: a key's provenance and each head's clock tell "removed there" from "never seen
// there" and "newer" from "concurrent". What only a filesystem has (identity across a rename,
// a directory coming back for its child, a catalogue root) stays Macha's, and its own cases
// prove it. A standing conflict is a note under "conflict/", the owner's blob as far as the
// reconciler can tell.

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dagda/heads/space.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dannan/encoding/base.h"

namespace dagda::test {

using heads::Merge;
using heads::MergeHeads;

inline HeadClock joined_clock(const HeadClock& left, const HeadClock& right) {
  auto out = left;
  for (const auto& [author, sequence] : right) {
    auto& current = out[author];
    current = std::max(current, sequence);
  }
  return out;
}

// Whether `viewer` has incorporated what gave an item its value. An item from before provenance
// has been seen by every head: nothing here keeps a clock from before it, where Macha's legacy
// clock judges one.
inline bool item_seen(const HeadClock& viewer, const NeutralItem& item) {
  if (!carries_provenance(item)) return true;
  return std::any_of(viewer.begin(), viewer.end(),
                     [&](const auto& entry) { return author_tag(entry.first) == item.tag && static_cast<int64_t>(entry.second) >= item.stamp; });
}

inline Bytes neutral_item_bytes(const NeutralItem& item) {
  dannan::net::Writer out;
  encode_neutral_item(out, item);
  return out.take();
}

inline std::string item_dot(const NeutralItem& item) { return std::to_string(item.tag) + ":" + std::to_string(item.stamp); }

inline constexpr char conflict_separator = '\x1f';

// A standing conflict: `key` holds the value with dot `installed`, and `alternative` is kept for
// whoever decides; `refs` is what either refers to.
struct NeutralConflict {
  std::string key;
  std::string installed;
  std::string alternative;
  std::vector<ObjectId> refs;
};

inline std::string conflict_note(const NeutralConflict& conflict) {
  return "conflict/" + conflict.key + conflict_separator + conflict.installed + conflict_separator + conflict.alternative;
}

inline std::string conflict_refs(const std::vector<ObjectId>& refs) {
  std::string out;
  for (const auto& ref : refs) out += store::to_string(ref);
  return out;
}

inline std::vector<NeutralConflict> neutral_conflicts(const NeutralHead& head) {
  std::vector<NeutralConflict> out;
  const auto& notes = head.notes.get();
  for (auto at = notes.lower_bound("conflict/"); at != notes.end() && at->first.starts_with("conflict/"); ++at) {
    const auto first = at->first.find(conflict_separator);
    const auto second = at->first.find(conflict_separator, first + 1);
    if (first == std::string::npos || second == std::string::npos) throw DecodeError("bad neutral conflict");
    NeutralConflict conflict;
    conflict.key = at->first.substr(9, first - 9);
    conflict.installed = at->first.substr(first + 1, second - first - 1);
    conflict.alternative = at->first.substr(second + 1);
    for (size_t i = 0; i + 64 <= at->second.size(); i += 64) {
      const auto unhexed = dannan::encoding::unhex(at->second.substr(i, 64));
      if (!unhexed || unhexed->size() != 32) throw DecodeError("bad neutral conflict reference");
      ObjectId id;
      std::copy(unhexed->begin(), unhexed->end(), id.bytes.begin());
      conflict.refs.push_back(id);
    }
    out.push_back(std::move(conflict));
  }
  return out;
}

struct NeutralMerged {
  // Merge parents and root are the caller's.
  NeutralHead head;
  NeutralEntries entries;
  uint64_t conflicts_created = 0;
  uint64_t conflicts_superseded = 0;
  std::vector<std::string> created_keys;
};

// What Macha's merge_metadata_heads_over does over the entries it is given: the same answer
// whichever way round the two come, by taking them in hash order.
inline NeutralMerged neutral_merge_over(const NeutralHead& left, const NeutralHead& right, const NeutralEntries& left_entries,
                                        const NeutralEntries& right_entries, const Hash256& left_hash, const Hash256& right_hash) {
  if (left_hash == right_hash) {
    NeutralMerged same;
    same.head = left;
    same.head.root.reset();
    same.entries = left_entries;
    return same;
  }
  if (right_hash < left_hash) return neutral_merge_over(right, left, right_entries, left_entries, right_hash, left_hash);

  NeutralMerged out;
  out.head.floor = std::max(left.floor, right.floor);
  out.head.clock = joined_clock(left.clock, right.clock);

  // Values of the space rather than of a key: the greater where the heads differ.
  NeutralNotes notes = left.notes.get();
  for (const auto& [key, value] : right.notes.get()) {
    const auto found = notes.find(key);
    if (found == notes.end() || found->second < value) notes[key] = value;
  }

  const auto settle = [&](const std::string& key, const NeutralItem& a, const NeutralItem& b) -> const NeutralItem& {
    const bool left_seen = item_seen(right.clock, a);
    const bool right_seen = item_seen(left.clock, b);
    if (left_seen && !right_seen) return b;
    if (right_seen && !left_seen) return a;
    // Concurrent: the later installed, the other kept for whoever decides.
    const bool a_later = std::pair(a.stamp, neutral_item_bytes(a)) > std::pair(b.stamp, neutral_item_bytes(b));
    const auto& installed = a_later ? a : b;
    const auto& other = a_later ? b : a;
    NeutralConflict conflict;
    conflict.key = key;
    conflict.installed = item_dot(installed);
    conflict.alternative = item_dot(other);
    // Both values, as Macha's conflict keeps both entries' extents as roots.
    std::vector<ObjectId> refs;
    for (const auto* value : {&installed, &other})
      for (const auto& ref : value->refs)
        if (!ref.gap) refs.push_back(ref.id);
    if (notes.emplace(conflict_note(conflict), conflict_refs(refs)).second) {
      ++out.conflicts_created;
      out.created_keys.push_back(key);
    }
    return installed;
  };

  std::set<std::string> keys;
  for (const auto* entries : {&left_entries, &right_entries})
    for (const auto& [key, item] : *entries) keys.insert(key);
  for (const auto& key : keys) {
    const auto a = left_entries.find(key);
    const auto b = right_entries.find(key);
    const bool in_left = a != left_entries.end();
    const bool in_right = b != right_entries.end();
    if (in_left && in_right) {
      out.entries[key] = a->second == b->second ? a->second : settle(key, a->second, b->second);
      continue;
    }
    // On one side only: removed by the other, or never seen by it.
    const auto& present = in_left ? a->second : b->second;
    const auto& other = in_left ? right.clock : left.clock;
    if (!item_seen(other, present)) out.entries[key] = present;
  }

  // Standing conflicts from either head; those since decided are dropped, by the value their
  // subject now holds.
  out.head.notes = SharedNotes(notes);
  for (const auto& conflict : neutral_conflicts(out.head)) {
    const auto subject = out.entries.find(conflict.key);
    if (subject != out.entries.end() && item_dot(subject->second) == conflict.installed) continue;
    out.head.notes.erase(conflict_note(conflict));
    ++out.conflicts_superseded;
  }
  return out;
}

struct NeutralTreeMerge {
  NeutralMerged merged;
  NeutralTree::Changes changes;
  ObjectId onto{};
  uint64_t keys_examined = 0;
};

// What Macha's merge_tree_backed_heads does: the two trees merge by what differs between them,
// and the changes are against the lower head's tree, so they are also the commit's delta.
inline NeutralTreeMerge neutral_tree_merge(const NeutralHead& left, const NeutralHead& right, const Hash256& left_hash, const Hash256& right_hash,
                                           const heads::TreeNodeStore& store, const heads::TreePause& pause = {}) {
  if (!left.root || !right.root) throw std::logic_error("a tree merge requires two tree-backed heads");
  const bool left_primary = !(right_hash < left_hash);
  NeutralTreeMerge out;
  out.onto = left_primary ? *left.root : *right.root;
  if (left_hash == right_hash) {
    out.merged.head = left;
    out.merged.head.root.reset();
    return out;
  }
  const auto differing = replay_tree().diff(*left.root, *right.root, store, pause);

  // What the merge must see: each key that differs, and each standing conflict's subject.
  std::set<std::string> keys;
  for (const auto& [key, difference] : differing) keys.insert(key);
  for (const auto* head : {&left, &right})
    for (const auto& conflict : neutral_conflicts(*head)) keys.insert(conflict.key);

  NeutralEntries left_entries, right_entries;
  for (const auto& key : keys) {
    if (pause) pause();
    const auto found = differing.find(key);
    if (found != differing.end()) {
      if (found->second.before) left_entries.emplace(key, *found->second.before);
      if (found->second.after) right_entries.emplace(key, *found->second.after);
      continue;
    }
    // Not a difference: both heads hold the same value, or neither.
    if (auto same = replay_tree().lookup(*left.root, key, store)) {
      left_entries.emplace(key, *same);
      right_entries.emplace(key, std::move(*same));
    }
  }
  out.keys_examined = keys.size();
  out.merged = neutral_merge_over(left, right, left_entries, right_entries, left_hash, right_hash);
  const auto& primary = left_primary ? left_entries : right_entries;
  for (const auto& key : keys) {
    const auto merged = out.merged.entries.find(key);
    const auto was = primary.find(key);
    const bool present = merged != out.merged.entries.end();
    if (present != (was != primary.end()) || (present && merged->second != was->second))
      out.changes[key] = present ? std::optional<NeutralItem>(merged->second) : std::optional<NeutralItem>();
  }
  out.merged.entries.clear();
  return out;
}

// Macha's tree_merge_delta.
inline std::optional<NeutralDelta> neutral_tree_merge_delta(const NeutralHead& primary, const NeutralHead& merged, const NeutralTree::Changes& changes) {
  auto delta = neutral_delta(primary, merged);
  if (!delta) return std::nullopt;
  for (const auto& [key, item] : changes) {
    if (item)
      delta->upsert_keys[key] = *item;
    else
      delta->erase_keys.push_back(key);
  }
  return delta;
}

// The neutral owner's merge. Counts what it was asked, and lets a case stand in the way.
class NeutralMerge : public Merge {
 public:
  Result merge(const std::optional<State>& base, const MergeHeads& heads) const override {
    (void)base;
    calls.fetch_add(1);
    if (before) before();
    const auto& primary = *NeutralCodec::as_head(heads.primary_state);
    const auto& secondary = *NeutralCodec::as_head(heads.secondary_state);
    auto tree_merge = neutral_tree_merge(primary, secondary, heads.primary.hash, heads.secondary.hash, heads.reader, heads.pause);
    keys_examined.fetch_add(tree_merge.keys_examined);
    auto merged = std::make_shared<NeutralHead>(tree_merge.merged.head);
    merged->merge_parents = {heads.secondary.hash};
    merged->root = replay_tree().update(tree_merge.onto, heads.nodes, tree_merge.changes);

    Result out;
    out.conflicts_created = tree_merge.merged.conflicts_created;
    out.conflicts_superseded = tree_merge.merged.conflicts_superseded;
    // What the change names, and the conflict alternatives that are new to the primary.
    for (const auto& [key, item] : tree_merge.changes)
      if (item)
        for (const auto& ref : item->refs)
          if (!ref.gap) out.claims.data.push_back(ref.id);
    std::set<std::string> held;
    for (const auto& conflict : neutral_conflicts(primary)) held.insert(conflict_note(conflict));
    const auto standing = neutral_conflicts(*merged);
    out.conflicts_standing = standing.size();
    for (const auto& conflict : standing)
      if (!held.contains(conflict_note(conflict))) out.claims.data.insert(out.claims.data.end(), conflict.refs.begin(), conflict.refs.end());
    if (const auto delta = neutral_tree_merge_delta(primary, *merged, tree_merge.changes)) out.delta = encode_neutral_delta(*delta);
    out.state = std::move(merged);
    return out;
  }
  void published(const Result& result, const heads::Record& commit) const override {
    (void)result;
    std::lock_guard lock(_mutex);
    _commits.push_back(commit.hash);
  }
  std::vector<Hash256> published_commits() const {
    std::lock_guard lock(_mutex);
    return _commits;
  }

  mutable std::atomic_uint64_t calls{0};
  mutable std::atomic_uint64_t keys_examined{0};
  // Called first in every merge, and may throw what a merge may.
  std::function<void()> before;

 private:
  mutable std::mutex _mutex;
  mutable std::vector<Hash256> _commits;
};

// A merge whose result reads `base`, as Q5 warns of: a note says what the two heads and the
// ancestor it could reach have shown it, the greater winning. Two reconcilers, one holding the
// ancestor and one not, make sibling commits; once either head shows the note its fold of those
// siblings no longer depends on `base`, so it settles.
class BaseReadingMerge final : public NeutralMerge {
 public:
  static constexpr const char* witness = "witness";
  Result merge(const std::optional<State>& base, const MergeHeads& heads) const override {
    auto result = NeutralMerge::merge(base, heads);
    if (!result.state) return result;
    auto next = std::make_shared<NeutralHead>(*NeutralCodec::as_head(result.state));
    const auto& notes = next->notes.get();
    const auto found = notes.find(witness);
    auto value = found == notes.end() ? std::string("1-no-ancestor") : found->second;
    if (base) value = std::max(value, std::string("2-ancestor"));
    next->notes.set(witness, value);
    result.state = std::move(next);
    // The note is not in the owner's delta: the whole record says it.
    result.delta.clear();
    return result;
  }
  bool reads_base() const override { return true; }
};

inline SpaceSpec reconciling_spec(std::string name = "test/neutral", std::shared_ptr<const Merge> merge = std::make_shared<NeutralMerge>()) {
  auto spec = founded_spec(std::move(name));
  spec.merge = std::move(merge);
  return spec;
}

// A commit's body as a test builds one outside any mutation path: another author's change to
// `base`, stamped as a commit stamps what it writes.
struct NeutralBranch {
  Record record;
  NeutralHead head;
};

inline NeutralBranch neutral_branch(const Record& base, const NeutralHead& base_head, const Dot& dot, const NeutralEntries& upsert,
                                    const std::vector<std::string>& erase, heads::TreeNodeStore& nodes) {
  NeutralBranch out;
  out.head = base_head;
  out.head.merge_parents.clear();
  NeutralTree::Delta changes;
  changes.erase = erase;
  for (const auto& [key, item] : upsert) {
    auto stamped = item;
    stamp_item(stamped, dot);
    changes.upsert[key] = std::move(stamped);
  }
  out.head.root = replay_tree().apply(*out.head.root, nodes, changes);
  out.head.clock[dot.author] = dot.sequence;
  out.record = record_after(base, neutral_payload(out.head));
  return out;
}

}  // namespace dagda::test
