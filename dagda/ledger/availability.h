//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dagda/contract/id_lookup.h"
#include "dagda/ledger/trie_diff.h"
#include "dagda/ledger/trie_union.h"
#include "dannan/identity/node_id.h"

// Which referenced objects some reachable node holds, found by diffing this
// node's held trie against each peer's (requirement L8; Macha's
// ledger/availability, replication by diff). The path table over a
// namespace that Macha builds on it is Macha's.
//
// Primitives: no locks, no threads; I/O only through what they are given.
namespace dagda::ledger {

// Whether this node holds a DATA object.
using HeldFn = std::function<bool(const ObjectId&)>;

// What the reachable nodes hold between them.
struct AvailabilitySurvey {
  // Referenced, and held by no node asked; sorted, unique.
  std::vector<ObjectId> unavailable;
  // Referenced, not held here or by any peer that answered, while some peer
  // did not answer: whether that peer holds them is not known. Sorted,
  // unique.
  std::vector<ObjectId> unknown;
  size_t peers_asked = 0;
  size_t peers_failed = 0;
  // Diff cost: rounds of questions, and prefixes asked about.
  size_t rounds = 0;
  uint64_t nodes_asked = 0;

  bool is_unavailable(const ObjectId&) const;
  bool is_unknown(const ObjectId&) const;
};

// One peer's part in a survey. `answered`: the diff ran to the end, and
// `root` is the held trie it was of. `lacks`: what is referenced, held here
// and not by the peer, sorted. `why`: why a peer did not answer.
struct PeerDiff {
  bool answered = false;
  Hash256 root{};
  std::vector<ObjectId> lacks;
  std::string why;
};

// Per peer, what is referenced, held here and lacking on that peer, as of a
// survey; a peer missing from the map could not say. What repair pushes.
struct PeerLacks {
  // The inventory's generation: the lists say nothing about an object
  // referenced since.
  uint64_t generation = 0;
  std::map<dannan::identity::NodeId, std::shared_ptr<const std::vector<ObjectId>>> lacks;
  // Every extent-hosting peer answered.
  bool complete = false;
};

// The survey from trie diffs: this node's held trie against each peer's.
// `missing_here` (sorted) is what is referenced that this node lacks; what an
// answering peer holds of it is available. The rest is unavailable when every
// peer answered and unknown otherwise. `peers` gets one entry per source, in
// order; a source that fails is not asked again in this survey. `pause`, if
// given, is called between rounds.
AvailabilitySurvey survey_by_diff(const std::shared_ptr<const TrieUnion>& mine, const std::vector<ObjectId>& missing_here, const contract::IdLookup& referenced,
                                  std::span<TrieSource* const> sources, std::vector<PeerDiff>& peers, const std::function<void()>& pause = {});

// For a survey some peer could not finish: what `last` found held by no
// reachable node stays so in `next`, less what this node now holds, and is no
// longer unknown.
void keep_unavailable(AvailabilitySurvey& next, const AvailabilitySurvey& last, const HeldFn& held);

}  // namespace dagda::ledger
