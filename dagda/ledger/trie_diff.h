//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "dagda/ledger/object_trie.h"

namespace dagda::ledger {

// One side of a diff: a trie that answers, for a batch of prefixes, the
// subtrees one byte below each and the records below each. A snapshot or a
// union here; a peer's trie over the wire (step 3).
class TrieSource {
 public:
  virtual ~TrieSource() = default;
  virtual ObjectTrie::Summary root() = 0;
  virtual std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix>) = 0;
  virtual std::vector<std::vector<ObjectTrie::Record>> records(std::span<const ObjectTrie::Prefix>) = 0;
};

// A snapshot as a source. Keeps its own copy of the snapshot.
class SnapshotSource final : public TrieSource {
 public:
  explicit SnapshotSource(ObjectTrie::Snapshot snapshot) : _snapshot(std::move(snapshot)) {}
  ObjectTrie::Summary root() override { return {_snapshot.size(), _snapshot.root_hash()}; }
  std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix> p) override { return _snapshot.children(p); }
  std::vector<std::vector<ObjectTrie::Record>> records(std::span<const ObjectTrie::Prefix> p) override { return _snapshot.records(p); }

 private:
  ObjectTrie::Snapshot _snapshot;
};

// An id whose record differs: held by one side only, or with another value.
struct TrieDifference {
  ObjectId id;
  std::optional<Bytes> left;
  std::optional<Bytes> right;
  bool operator==(const TrieDifference&) const = default;
};

// What a diff asked of each side.
struct TrieDiffCost {
  // Batches of questions: one children() and one records() a side count as
  // one round each.
  size_t rounds = 0;
  size_t prefixes = 0;
  size_t records = 0;
};

// Requirement L6. Every id whose record differs between the two sides, each
// once, in no particular order. Descends a level at a time: each side is
// asked once per level about every subtree whose summaries differ, and
// records are read once neither side holds more than a leaf below a prefix.
// Where the sides agree nothing below is read, so the cost follows the
// difference, not the size. `pause`, if given, is called between rounds.
// Waits on what the sources wait on; an exception from a source ends it.
TrieDiffCost diff_tries(TrieSource& left, TrieSource& right, const std::function<void(const TrieDifference&)>& visit, const std::function<void()>& pause = {});

// The records of `wanted` whose ids none of `held` holds, in id order: one
// pass over each in step, a page at a time. `pause`, if given, is called
// between pages. Waits on the state device.
void records_not_held(const ObjectTrie::Snapshot& wanted, std::span<const ObjectTrie::Snapshot> held,
                      const std::function<void(const ObjectTrie::Record&)>& visit, const std::function<void()>& pause = {});

// The wire form of a question to a peer's trie and its answers (requirement
// L7, protocol 23's bytes; the rounds that carry them are step 3's). `root`
// asks for the summary of the peer's current trie and pins that snapshot
// under its hash; `children` and `records` ask about prefixes of the
// snapshot pinned under `at`. At most trie_question_max prefixes for
// children and trie_records_max for records, each below at most a leaf's
// worth. Decoding throws dannan::net::DecodeError.
enum class TrieQuestionKind : uint8_t { root = 0, children = 1, records = 2 };
// Which trie a question names. One today; L7 generalises it at step 3.
enum class TrieName : uint8_t { held_data = 0 };
inline constexpr size_t trie_question_max = 64;
inline constexpr size_t trie_records_max = 512;
// The most prefixes one question of this kind carries.
constexpr size_t trie_question_limit(TrieQuestionKind kind) noexcept { return kind == TrieQuestionKind::records ? trie_records_max : trie_question_max; }
struct TrieQuestion {
  TrieQuestionKind kind = TrieQuestionKind::root;
  TrieName trie = TrieName::held_data;
  Hash256 at{};
  std::vector<ObjectTrie::Prefix> prefixes;
  bool operator==(const TrieQuestion&) const = default;
};
// Throws std::invalid_argument for more prefixes than the kind carries.
Bytes encode_trie_question(const TrieQuestion&);
TrieQuestion decode_trie_question(std::span<const uint8_t>);
Bytes encode_trie_root(const ObjectTrie::Summary&);
ObjectTrie::Summary decode_trie_root(std::span<const uint8_t>);
// Only the non-empty slots travel.
Bytes encode_trie_children(std::span<const ObjectTrie::Children>);
std::vector<ObjectTrie::Children> decode_trie_children(std::span<const uint8_t>);
Bytes encode_trie_records(std::span<const std::vector<ObjectTrie::Record>>);
std::vector<std::vector<ObjectTrie::Record>> decode_trie_records(std::span<const uint8_t>);

}  // namespace dagda::ledger
