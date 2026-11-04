//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "dagda/contract/object_ledger.h"
#include "dagda/ledger/availability.h"
#include "dannan/context.h"
#include "dannan/contract/published.h"
#include "dannan/contract/time_source.h"

namespace dagda::store {

// What the survey runs with: Macha's figures (availability_service.hpp).
struct AvailabilityOptions {
  // Each question to a peer's trie.
  std::chrono::milliseconds call{30000};
  // A survey reads the whole referenced set, so the next waits this many
  // times the last one's cost: a twentieth of the node's time.
  unsigned share = 20;
  // While a disk is being seeded the holdings cannot be named: looked at
  // again this often.
  std::chrono::milliseconds seed_retry{5000};
  // A peer that could not answer is asked again after this, doubling to the
  // ceiling; an answer clears it.
  std::chrono::milliseconds retry_floor{1000};
  std::chrono::milliseconds retry_ceiling{300000};
};

// One survey's result: immutable once published.
struct AvailabilitySnapshot {
  // The inventory's generation it was made against.
  uint64_t generation = 0;
  uint64_t surveyed_unix_ms = 0;
  ledger::AvailabilitySurvey survey;
};

// The availability survey (requirement L8; Macha's service/availability_
// service.cpp, its path table left to Macha): what is referenced, as the
// inventory the collector built from the owner's ReachabilityProvider says,
// joined against this node's held trie for what it lacks (missing_here), and
// diffed against each extent-hosting peer's held trie over the network for
// what each peer lacks (peer_lacks) and what no reachable node holds
// (unavailable). Each diff runs its rounds against the one snapshot its root
// question pinned on the peer (the storage server answers them). Repair
// works from these lists: it never walks the store or probes a peer.
//
// Peers are asked again when the membership changed, a hosting peer joined
// or left, a peer's storage shrank, or after a backoff when one could not
// answer. What the inventory or this node's holdings changed, or a peer's
// storage growing while something is missing, waits for the duty share. A
// gain alone asks nobody: what this node gained was already available.
// Nothing is surveyed while a disk is being seeded or before the first
// inventory.
//
// refresh() has one owner (the node's maintenance); every reader is
// thread-safe and waits on nothing.
class AvailabilityService {
 public:
  using MissingHere = std::shared_ptr<const std::vector<ObjectId>>;
  using Lacks = std::shared_ptr<const ledger::PeerLacks>;

  // `ledger` supplies the inventory and this node's held view; both outlive
  // the service.
  AvailabilityService(dannan::Context& context, const contract::ObjectLedger& ledger, AvailabilityOptions options = {});
  AvailabilityService(const AvailabilityService&) = delete;
  AvailabilityService& operator=(const AvailabilityService&) = delete;

  // Brings the survey up to date if something that could change the answer
  // has happened since the last. Returns whether peers were asked. `pause`
  // is called between pages and rounds. Single owner.
  static constexpr dannan::sched::Waits refresh_waits = dannan::sched::Waits::state_device | dannan::sched::Waits::network | dannan::sched::Waits::locks;
  bool refresh(const std::function<void()>& pause = {});

  // The last survey, or null before the first. A pointer copy.
  std::shared_ptr<const AvailabilitySnapshot> snapshot() const { return _snapshot.handle(); }
  // What is referenced that this node lacked at the last survey, sorted;
  // null before the first. Repair pulls from it.
  MissingHere missing_here() const { return _missing.handle(); }
  // Per peer, what it lacked of what this node holds; repair pushes from it.
  Lacks peer_lacks() const { return _lacks.handle(); }
  // The last survey found no reachable node holding it.
  bool unavailable(const ObjectId& id) const;
  // When refresh() next has something to do with no new event.
  std::optional<dannan::contract::TimePoint> due() const noexcept { return _due; }

 private:
  dannan::Context& _context;
  const contract::ObjectLedger& _ledger;
  const AvailabilityOptions _options;
  const dannan::contract::TimeSource& _time;
  const dannan::status::ScopedLogger _logger;
  dannan::contract::Published<AvailabilitySnapshot> _snapshot;
  dannan::contract::Published<ledger::PeerLacks> _lacks;
  dannan::contract::Published<std::vector<ObjectId>> _missing;

  // refresh()'s own: what the last survey saw.
  bool _surveyed = false;
  contract::ObjectLedger::InventoryHandle _joined_inventory;
  contract::HeldIdentity _joined_identity;
  dannan::contract::TimePoint _surveyed_at{};
  dannan::contract::Clock::duration _surveyed_cost{};
  dannan::contract::Clock::duration _retry_backoff{};
  std::optional<dannan::contract::TimePoint> _retry_at;
  std::optional<dannan::contract::TimePoint> _due;
  // NodeEvents::membership at the last survey: joins, leaves, place,
  // capabilities, path class and incarnation, never an advert.
  uint64_t _surveyed_membership = 0;
  // Each extent-hosting peer and the storage it advertised.
  std::vector<std::pair<dannan::identity::NodeId, uint64_t>> _surveyed_peers;
};

}  // namespace dagda::store
