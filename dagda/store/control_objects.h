//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include "dagda/store/local_store.h"
#include "dagda/store/object_id.h"
#include "dannan/context.h"

namespace dagda::store {

// Brings a CONTROL object this node lacks into its control store.
class ControlObjectSource {
 public:
  virtual ~ControlObjectSource() = default;
  // True when the object is local on return.
  virtual bool ensure_control_local(const ObjectId& id) = 0;
};

// Pulls CONTROL objects from peers (requirement S29; Macha's
// cluster/control_objects.*). They are head data, not DATA replicas: every
// active peer is asked over CONTROL at speculative priority, so the search
// never holds up health or a commit's own traffic and needs no DATA session,
// and it takes no DATA credit (the control store is not on the DATA device).
// What a peer sends is checked against the id before it is kept.
//
// Thread-safe. pull() waits on the peers, each for at most `per_peer`; never
// from one of the io context's threads.
class ControlObjectFetch final : public ControlObjectSource {
 public:
  // Told the bytes and duration of each object received.
  using Observer = std::function<void(uint64_t bytes, std::chrono::steady_clock::duration)>;

  ControlObjectFetch(dannan::Context& context, LocalStore& control, std::chrono::milliseconds per_peer = std::chrono::seconds(30));

  bool ensure_control_local(const ObjectId& id) override { return pull(id, {}); }
  bool pull(const ObjectId& id, const Observer& observe);

 private:
  dannan::Context& _context;
  LocalStore& _control;
  const std::chrono::milliseconds _per_peer;
  const dannan::status::ScopedLogger _logger;
};

}  // namespace dagda::store
