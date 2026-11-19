//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/trie_diff.h"

namespace dagda::ledger {

// Several tries' records read as one trie (requirement L5: a node's held
// trie is the union of its disks'): each id once, with the value of the
// first part that holds it. Its summaries are those one trie over the same
// records would have, so it diffs against any trie.
//
// A subtree only one part holds, or that every part holding it holds alike,
// is that part's own: nothing below it is read. Only subtrees several parts
// hold differently are worked out, reading the parts' nodes below them; a
// union made with the one before it as a hint reuses each subtree whose
// parts are as they were, so its cost follows what changed.
//
// Immutable; thread-safe; waits on the state device (the parts' nodes) only
// while it is made and when asked about a subtree several parts share.
class TrieUnion {
 public:
  explicit TrieUnion(std::vector<ObjectTrie::Snapshot> parts, const TrieUnion* hint = nullptr);

  ObjectTrie::Summary root() const noexcept;
  uint64_t size() const noexcept { return root().count; }
  Hash256 root_hash() const noexcept { return root().hash; }
  std::optional<Bytes> get(const ObjectId&) const;
  // As ObjectTrie::Snapshot's.
  std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix>) const;
  std::vector<std::vector<ObjectTrie::Record>> records(std::span<const ObjectTrie::Prefix>, uint64_t most = UINT64_MAX) const;
  const std::vector<ObjectTrie::Snapshot>& parts() const noexcept { return _parts; }

 private:
  struct Node;

  std::shared_ptr<const Node> merge(const ObjectTrie::Prefix&, std::vector<ObjectTrie::Summary> inputs, const std::shared_ptr<const Node>& hint) const;
  std::vector<ObjectTrie::Record> merged_records(const ObjectTrie::Prefix&, uint64_t most) const;

  std::vector<ObjectTrie::Snapshot> _parts;
  std::shared_ptr<const Node> _root;
};

// A union as a source.
class UnionSource final : public TrieSource {
 public:
  explicit UnionSource(std::shared_ptr<const TrieUnion> trie) : _trie(std::move(trie)) {}
  ObjectTrie::Summary root() override { return _trie->root(); }
  std::vector<ObjectTrie::Children> children(std::span<const ObjectTrie::Prefix> p) override { return _trie->children(p); }
  std::vector<std::vector<ObjectTrie::Record>> records(std::span<const ObjectTrie::Prefix> p) override { return _trie->records(p); }

 private:
  std::shared_ptr<const TrieUnion> _trie;
};

}  // namespace dagda::ledger
