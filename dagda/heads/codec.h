//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dagda/heads/record.h"
#include "dagda/heads/tree.h"

namespace dagda::heads {

// An owner's state: opaque to Dagda, immutable once made, and shared, so a delta
// can share what it did not touch.
using State = std::shared_ptr<const void>;

// What Dagda reads of a state.
struct HeadFields {
  // The idempotency clock. Empty says nothing about what the head holds.
  HeadClock clock;
  // The commit's parents beyond its primary.
  std::vector<Hash256> merge_parents;
  // Zero only on genesis: its certificate names nobody.
  uint32_t floor = 0;
  // The tree this head names; none on a head with no tree.
  std::optional<ObjectId> root;
  // Whatever of the state only a policy commit may change, in any form that compares:
  // a mutation that moves it is refused (mgr:1713-1718).
  Bytes policy;
};

// The bytes of a head space are the owner's (the plan's section 2.0): Dagda holds
// no owner's type. Dagda's hash covers whatever `encode_full` writes, so replay
// holds only if re-encoding is byte exact, every slot and form included.
//
// Thread-safe: any call from any thread. Waits on nothing but `apply_delta`, which
// waits on the tree store it is given. A decode that cannot be completed throws;
// the replica treats a throw as a record it cannot use, except
// contract::MaintenanceStopping, which it passes through.
class Codec {
 public:
  virtual ~Codec() = default;

  // The whole payload of generation 1, a constant of the space: a space opens at
  // genesis before any commit. Must decode_full to a state with floor zero.
  virtual Bytes genesis() const = 0;
  virtual State decode_full(std::span<const uint8_t> payload) const = 0;
  virtual Bytes encode_full(const State& state) const = 0;
  // The delta from `before` to `after`; nothing when one cannot express it. For
  // Space::mutate; replay never calls it.
  virtual std::optional<Bytes> encode_delta(const State& before, const State& after) const = 0;
  // Throws unless `delta` decodes. Called on a frame's body at open, so a delta
  // that can never apply is skipped there as Macha's load_history skips it. Reads
  // no tree.
  virtual void check_delta(std::span<const uint8_t> delta) const = 0;
  // The state after `delta` on `parent`. `tree` is empty when the replica has no
  // node store, and a delta that needs one is then refused, not applied to what
  // was never read. Pure over its inputs: no clock, no config, no dot.
  virtual State apply_delta(const State& parent, std::span<const uint8_t> delta, TreeNodeStore* tree) const = 0;
  // The payload of `state`, reached by `delta`. Dagda hashes it against the frame.
  virtual Bytes encode_after_delta(std::span<const uint8_t> delta, const State& state) const = 0;
  virtual HeadFields head_fields(const State& state) const = 0;
  // Conservative decoded weight of `state`, as the materialisation cache charges it.
  virtual uint64_t resident_bytes(const State& state) const = 0;
};

// Counts of the work the replica's paths do, for the cost baselines (the plan's
// section 2.6): counts, never time. Handed in, not a global. Nodes and parent
// copies are the owner's to count; the replica counts the rest. Atomic; any thread.
struct CostCounters {
  std::atomic_uint64_t tree_nodes_read{0};
  std::atomic_uint64_t tree_nodes_written{0};
  std::atomic_uint64_t record_bytes_encoded{0};
  std::atomic_uint64_t record_bytes_hashed{0};
  std::atomic_uint64_t full_decodes{0};
  std::atomic_uint64_t parent_states_copied{0};
  std::atomic_uint64_t history_frames_written{0};
  std::atomic_uint64_t syncs{0};
};

}  // namespace dagda::heads
