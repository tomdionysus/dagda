//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>

#include "dagda/domain/storage_key.h"
#include "dagda/heads/record.h"
#include "dagda/heads/tree.h"
#include "dagda/ledger/sealed_journal.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/sched/work.h"
#include "dannan/status/log.h"

namespace dagda::heads {

// A node-local space's tree nodes (slice 1, D3; the three-systems design, "How slice 1
// builds node_local"). A shared space's nodes are CONTROL objects, which are replicated,
// repaired and collected with the node's store (H19); a node-local space's must never
// leave its node, so they are kept here instead: one sealed journal in the space's
// directory, <directory>/nodes.pack, a frame a node, sealed under the key given (the
// node-local key), with an index in memory from each node's content address to its
// frame. Nothing here is offered, served or repaired, and only the space collects it.
//
// A put is durable before it returns. Opening replays the journal: a torn last frame,
// a crash's unacknowledged put, is cut; a whole frame that will not open (damage, or
// another key) refuses the open and changes nothing on disk.
//
// Thread-safe. Every operation waits on the state device.
class NodePack {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::thread_safe;
  // The largest node a tree writes is far below this.
  static constexpr uint32_t max_node_bytes = 16U * 1024U * 1024U;

  struct Compaction {
    uint64_t kept = 0;
    uint64_t dropped = 0;
    uint64_t bytes_before = 0;
    uint64_t bytes_after = 0;
  };

  // Throws std::runtime_error for a pack that will not open under `key`, naming the
  // file, and std::invalid_argument for no key. Every frame carries the content address
  // of `aad` (the space's binding) ahead of its node, inside the seal: a pack moved to
  // another space does not open there.
  NodePack(std::filesystem::path directory, std::shared_ptr<const domain::StorageKey> key, Bytes aad, dannan::status::ScopedLogger logger);
  NodePack(const NodePack&) = delete;
  NodePack& operator=(const NodePack&) = delete;

  static std::filesystem::path file(const std::filesystem::path& directory) { return directory / "nodes.pack"; }

  // The node's content address; bytes already held are not written again.
  ObjectId put(std::span<const uint8_t> node) DANNAN_EXCLUDES(_mutex);
  // Empty for a node not held. Throws std::runtime_error for a frame that will not
  // open where the index says it is.
  std::optional<Bytes> get(const ObjectId& id) const DANNAN_EXCLUDES(_mutex);
  bool contains(const ObjectId& id) const DANNAN_EXCLUDES(_mutex);

  // Rewrites the pack holding exactly the nodes of `live` it holds: written whole beside
  // it, synced and renamed over it, so a crash leaves the old pack or the new.
  Compaction compact(const std::set<ObjectId>& live) DANNAN_EXCLUDES(_mutex);

  size_t nodes() const DANNAN_EXCLUDES(_mutex);
  uint64_t bytes() const DANNAN_EXCLUDES(_mutex);

  // A tree's view of the pack, for a space's commit, read and replay.
  std::function<std::unique_ptr<TreeNodeStore>()> factory();

 private:
  // The journal's frame for `node`: the space's binding, then the node.
  Bytes frame_of(std::span<const uint8_t> node) const;
  void replay_locked() DANNAN_REQUIRES(_mutex);

  const std::filesystem::path _directory;
  const std::shared_ptr<const domain::StorageKey> _key;
  const ObjectId _binding;
  const dannan::status::ScopedLogger _logger;

  mutable dannan::contract::Mutex _mutex;
  std::unique_ptr<ledger::SealedJournal> _journal DANNAN_GUARDED_BY(_mutex);
  std::map<ObjectId, uint64_t> _index DANNAN_GUARDED_BY(_mutex);
};

// The highest generation each node-local space has committed (D3, no rollback): one
// small file, <state>/heads/node-local.generations, outside every space's directory and
// sealed under the node-local key, raised durably as each commit is accepted. A space
// opening older than its record is a restored older copy, and is refused. It refuses one
// space's directory restored alone, not a whole state directory restored with it: the
// defence against that is the peers' witness, which is Dannan's to offer (the design's
// question for Tom). Thread-safe; raise() waits on the state device.
class NodeLocalGenerations {
 public:
  // Throws std::runtime_error for a file that will not open under `key`.
  NodeLocalGenerations(std::filesystem::path file, std::shared_ptr<const domain::StorageKey> key, Bytes aad);
  static std::filesystem::path file(const std::filesystem::path& state) { return state / "heads" / "node-local.generations"; }

  // Zero for a space never recorded.
  uint64_t of(const Hash256& space) const DANNAN_EXCLUDES(_mutex);
  // Durable before it returns; a generation at or below the record changes nothing.
  void raise(const Hash256& space, uint64_t generation) DANNAN_EXCLUDES(_mutex);

 private:
  const std::filesystem::path _file;
  const std::shared_ptr<const domain::StorageKey> _key;
  const Bytes _aad;
  mutable dannan::contract::Mutex _mutex;
  std::map<Hash256, uint64_t> _generations DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::heads
