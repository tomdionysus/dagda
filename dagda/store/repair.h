//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "dagda/contract/object_ledger.h"
#include "dagda/ledger/claim_walk.h"
#include "dagda/store/availability.h"
#include "dagda/store/cpu_pacing.h"
#include "dagda/store/distributed_store.h"
#include "dannan/context.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/duty_share.h"

namespace dagda::store {

// Macha's maintenance figures that pace repair (config.hpp
// MaintenanceConfig), with its defaults.
struct RepairOptions {
  // Repair shares time with every higher class by weight (Dannan's
  // DutyShare): while foreground or loader work is present, a turn of
  // `slice` earns a rest of foreground_weight / repair_weight times the
  // turn. Both at least 1.
  uint32_t foreground_weight = 95;
  uint32_t repair_weight = 5;
  std::chrono::milliseconds slice{25};
  // A class counts as present this long after its last activity.
  std::chrono::milliseconds foreground_quiet{2000};
  // A peer's foreground traffic, in telemetry no older than this, paces
  // repair as this node's own does: repair shares their links (Macha's
  // three telemetry intervals, at least 3 s).
  std::chrono::milliseconds peer_window{3000};
  // Repair's byte credit accrues at this fraction of the link's estimated
  // rate (initial_bandwidth until one is measured, at most max_bandwidth
  // when set), banked to five seconds of the link or an extent.
  double idle_bandwidth_fraction = 0.10;
  uint64_t initial_bandwidth = 32ULL * 1024 * 1024;
  uint64_t max_bandwidth = 0;
  // Above this share of a CPU in the rest of the process, repair's credit
  // shrinks in proportion (Macha's cpu_target). No source: not paced by CPU.
  double cpu_target = 0.10;
  CpuSource cpu = system_cpu_sample;
  // Calls one step may make.
  size_t operation_budget = 16;
  size_t extent_size = 16 * 1024 * 1024;
};

// Why a pass's repair did or did not run: it ran; a higher class held it to
// its share; it is waiting for an event after a pass with nothing to do; or
// its credit could not cover the next extent.
enum class RepairGate : uint8_t { ran, share, quiescent, credit };

// The higher classes whose activity held repair to its share.
enum RepairPacedBy : uint8_t { paced_by_foreground = 1, paced_by_loader = 2, paced_by_peer_foreground = 4 };

struct RepairPass {
  bool surveyed = false;
  RepairGate gate = RepairGate::ran;
  uint8_t paced_by = 0;
  // The credit when the pass decided, in bytes.
  uint64_t credit = 0;
  // Claimed objects the claim walks brought back.
  size_t restored = 0;
  RepairResult step;
};

struct RepairGates {
  uint64_t ran = 0;
  uint64_t share = 0;
  uint64_t quiescent = 0;
  uint64_t credit = 0;
  std::optional<RepairGate> last;
  uint8_t paced_by = 0;
};

// The repair stage of a node's maintenance pass (Macha's
// service/maintenance.cpp:309-313 and :674-852): the availability survey
// brought up to date, the claim walks with a restorer that spends network
// credit, then one repair step from the survey's lists, paced against
// foreground and loader work by a duty share and never stopped (law 3: a
// node that is always busy still repairs). The rest of Macha's pass (its
// metadata and catalogue stages, scrub, local rebalance) is not here.
//
// Single owner: one thread calls pass(). gates() is thread-safe.
class Repair {
 public:
  Repair(dannan::Context& context, DistributedStore& store, AvailabilityService& availability, contract::ObjectLedger& ledger, RepairOptions options = {});
  ~Repair();
  Repair(const Repair&) = delete;
  Repair& operator=(const Repair&) = delete;

  static constexpr dannan::sched::Waits pass_waits = AvailabilityService::refresh_waits | dannan::sched::Waits::data_device;
  RepairPass pass();
  RepairGates gates() const;

 private:
  class Restorer;
  bool higher_class_active() const;
  // A peer's telemetry shows foreground traffic now.
  bool peer_foreground_active() const;

  dannan::Context& _context;
  DistributedStore& _store;
  AvailabilityService& _availability;
  contract::ObjectLedger& _ledger;
  const RepairOptions _options;
  const dannan::contract::TimeSource& _time;
  // Repair's duty share against the classes above it.
  dannan::sched::DutyShare _share;
  ledger::ClaimWalk _data_walk;
  ledger::ClaimWalk _control_walk;
  CpuPacer _cpu;
  double _credit = 0.0;
  std::optional<dannan::contract::TimePoint> _last;
  // Quiet after a pass with nothing to do, until an event or new lists.
  bool _quiescent = false;
  uint64_t _quiescent_events = 0;
  // The lists it went quiet on, held so another made at a freed one's
  // address is never taken for it.
  std::shared_ptr<const void> _quiescent_lists[3];
  std::atomic<uint64_t> _ran{0};
  std::atomic<uint64_t> _gated_share{0};
  std::atomic<uint64_t> _gated_quiescent{0};
  std::atomic<uint64_t> _gated_credit{0};
  // 0x100 | gate << 4 | paced_by, so a reader never pairs a gate with
  // another pass's classes.
  std::atomic<uint16_t> _last_gate{0};
};

}  // namespace dagda::store
