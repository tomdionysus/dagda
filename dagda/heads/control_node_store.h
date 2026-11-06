//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "dagda/heads/tree.h"
#include "dagda/store/control_objects.h"
#include "dagda/store/local_store.h"
#include "dannan/sched/work.h"

namespace dagda::heads {

// A tree in the content-addressed control store, read through
// `ensure_control_local` as any other control object is: it inherits replication,
// repair and GC with no second durability model (requirement H19). The only place
// a tree (node-store agnostic, so it can be built offline) meets the store.
//
// Single owner: one read, commit or replay at a time.
class ControlTreeNodeStore final : public TreeNodeStore {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::single_owner;
  static constexpr dannan::sched::Waits put_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::Waits get_waits = dannan::sched::local_waits | dannan::sched::Waits::network;

  // A reader refuses `put`.
  static ControlTreeNodeStore for_reading(store::LocalStore& control, store::ControlObjectSource& source) {
    return ControlTreeNodeStore(control, source, Mode::read);
  }
  // A commit writes each new node on this node alone; a root is committed once
  // this node holds every node it addresses. The peers are sent them afterwards.
  static ControlTreeNodeStore for_commit(store::LocalStore& control, store::ControlObjectSource& source) {
    return ControlTreeNodeStore(control, source, Mode::commit);
  }
  // Replay writes every node it is given: content addressing makes a locally
  // rebuilt node identical to the committed one.
  static ControlTreeNodeStore for_replay(store::LocalStore& control, store::ControlObjectSource& source) {
    return ControlTreeNodeStore(control, source, Mode::replay);
  }

  // Writes the node and returns its content address. Throws std::logic_error on a
  // reader, and std::runtime_error when this node cannot hold it, so a commit never
  // publishes a root it cannot read.
  ObjectId put(std::span<const uint8_t> node) override;

  // Fetches the node, pulling it local first. Empty, not a throw, when it cannot
  // be obtained: a caller distinguishes "not here" from "not found" and needs to
  // know which node was missing. Bytes that do not open are the store's throw.
  std::optional<Bytes> get(const ObjectId& id) const override;

  // Nodes written since construction, in write order.
  const std::vector<ObjectId>& written() const noexcept { return _written; }

 private:
  enum class Mode : uint8_t { read, commit, replay };
  ControlTreeNodeStore(store::LocalStore& control, store::ControlObjectSource& source, Mode mode) : _control(control), _source(source), _mode(mode) {}

  store::LocalStore& _control;
  store::ControlObjectSource& _source;
  Mode _mode;
  std::vector<ObjectId> _written;
};

// The tree over one node's control store alone, for offline tools (a migration
// runs with the cluster stopped): nothing goes to or comes from peers. Single
// owner. Waits on the state device.
class LocalTreeNodeStore final : public TreeNodeStore {
 public:
  explicit LocalTreeNodeStore(store::LocalStore& control) : _control(control) {}

  ObjectId put(std::span<const uint8_t> node) override;
  std::optional<Bytes> get(const ObjectId& id) const override;

  const std::vector<ObjectId>& written() const noexcept { return _written; }
  uint64_t bytes_written() const noexcept { return _bytes_written; }

 private:
  store::LocalStore& _control;
  std::vector<ObjectId> _written;
  uint64_t _bytes_written = 0;
};

}  // namespace dagda::heads
