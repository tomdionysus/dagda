//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/ledger/object_trie.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/status/log.h"

namespace dagda::ledger {

// The state file's magic: `state` is MREF0001 | u8 clean | u8 has root | root
// 32 | u64 entries | u64 extent count, written whole by a durable replace.
inline constexpr std::array<uint8_t, 8> reference_counts_magic{'M', 'R', 'E', 'F', '0', '0', '0', '1'};

// What an owner's state refers to, counted per object and kept on the state
// device (requirement L15): its DATA (one count per reference, so an object
// two references share goes only with its last) in `extents`, and its own
// CONTROL nodes in `nodes`, at the root they were last brought to. Moved from
// root to root by the owner's diff, so a restart resumes where it stopped
// instead of walking the state. The counting is Dagda's; what is counted is
// the owner's, and a ReachabilityProvider hands its frozen views to the
// horizons. A count is a u64 trie value. The files are Macha's layout v18's
// (its namespace reference counts): two tries and `state`, the tries sealed
// under the node-local key.
//
// Single owner (the owner's census); views and totals are read from any
// thread (the node's status timer reads the totals). Every operation but
// totals() waits on the state device.
class ReferenceCounts {
 public:
  // Opens or creates the counts in `dir`, `cache_bytes` shared by the tries,
  // three quarters to the DATA one. A missing, damaged or dirty state leaves
  // no root, so the next count walks.
  ReferenceCounts(std::filesystem::path dir, std::shared_ptr<const domain::StorageKey> key, size_t cache_bytes, dannan::status::ScopedLogger logger);

  struct Totals {
    uint64_t entries = 0;
    uint64_t extent_count = 0;
  };
  // The root the counts are at; none before the first count and after an
  // interrupted change.
  std::optional<ObjectId> root() const noexcept { return _root; }
  // As last set, both figures from one reset or change. Waits on nothing.
  Totals totals() const;

  // Replaces every count: one item per reference, in any order.
  void reset(const std::optional<ObjectId>& root, std::vector<ObjectId> extents, std::vector<ObjectId> nodes, Totals);
  // One added for each item in, one taken for each item out. False, and
  // nothing changed, when something to take out is not counted.
  bool change(const ObjectId& root, std::vector<ObjectId> extents_in, std::vector<ObjectId> extents_out, std::vector<ObjectId> nodes_in,
              std::vector<ObjectId> nodes_out, Totals);

  // Frozen views of the ids counted now (a record's value is its count).
  struct Views {
    ObjectTrie::Snapshot extents;
    ObjectTrie::Snapshot nodes;
  };
  Views views() const;

 private:
  // What the tries hold: a root and totals, or dirty while they change.
  void write_state(bool clean) const;

  const std::filesystem::path _dir;
  const dannan::status::ScopedLogger _logger;
  ObjectTrie _extents;
  ObjectTrie _nodes;
  std::optional<ObjectId> _root;
  // Memory only: the status timer reads the totals while the owner sets them.
  mutable dannan::contract::Mutex _totals_mutex;
  Totals _totals DANNAN_GUARDED_BY(_totals_mutex);
};

}  // namespace dagda::ledger
