//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include "dagda/contract/object_ledger.h"
#include "dagda/ledger/claim_walk.h"
#include "dagda/store/availability.h"
#include "dagda/store/cpu_pacing.h"
#include "dagda/store/distributed_store.h"
#include "dannan/context.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/duty_share.h"

namespace dagda::store {

// The decision trace: `kind` names what was decided and `detail` how,
// deterministically. Macha's kinds and words ("claim-walk", "data <id>
// restored").
using RepairTrace = std::function<void(std::string_view kind, std::string_view detail)>;

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
  // The credit's burst is the larger of this and five seconds of the link; it is
  // also the window of transfers a turn keeps in flight (the repair redesign, stage 4,
  // where the 16-call count a step went).
  size_t extent_size = 16 * 1024 * 1024;
  // The clock repair's credit and share run on. None: the node's. An owner's
  // pass on its own clock gives it, as Macha's trace fixtures move its pass's
  // time while reachability stays on real time.
  const dannan::contract::TimeSource* time = nullptr;
  // The claim walk's restorer reports each missing claim here.
  RepairTrace trace;
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

// What earn_credit() found: the credit now, in bytes, the rate it earns at and
// the seconds it earned for.
struct RepairCredit {
  uint64_t credit = 0;
  double rate = 0.0;
  double seconds = 0.0;
};

// What claim_walks() did: the claims each class's walk examined, those not
// held, those brought back. `waiting_for_credit`: a walk met a missing claim
// it could not afford, and stays before it. `unfinished`: a walk reached its
// bound with claims left.
struct RepairWalks {
  size_t examined = 0;
  size_t missing = 0;
  size_t restored = 0;
  bool waiting_for_credit = false;
  bool unfinished = false;
};

// What a turn leaves owing, in Macha's order of asking: the transfer ran out of
// credit, and earning the next extent's is what to wait for; it yielded to a
// higher class; there is work left, or work was done, so another turn is due
// after one interval; only a claim walk still waits for credit; or nothing to
// do, the credit is dropped and repair waits for an event.
//
// Yielded owes another turn after one interval too, as more_work does. The class
// it yielded to may have arrived after the turn began, so a wake computed from
// what the pass found when it began (a busy wake, the share's next turn) may not
// be set, and with the credit past an extent no credit wake is either: a caller
// that waits only on those sleeps until an unrelated event (Macha 0.93.6).
enum class RepairSettled : uint8_t { credit, yielded, more_work, waiting_for_credit, quiescent };

// How a turn reads a peer's foreground traffic, whenever it asks whether a higher
// class is present: from telemetry each time, or as the pass found it when it
// began the turn, as Macha's does (maintenance.cpp:710-714).
enum class PeerReading : uint8_t { each_time, as_begun };

struct RepairGates {
  uint64_t ran = 0;
  uint64_t share = 0;
  uint64_t quiescent = 0;
  uint64_t credit = 0;
  std::optional<RepairGate> last;
  uint8_t paced_by = 0;
  // The credit when a gate was last noted, in bytes (Macha's last_credit_bytes).
  uint64_t last_credit_bytes = 0;
};

// The repair stage of a node's maintenance pass (Macha's
// service/maintenance.cpp:309-313 and :674-852): the availability survey
// brought up to date, the claim walks with a restorer that spends network
// credit, then one repair step from the survey's lists, paced against
// foreground and loader work by a duty share and never stopped (law 3: a
// node that is always busy still repairs). The rest of Macha's pass (its
// metadata and catalogue stages, scrub, local rebalance) is not here.
//
// pass() is these steps in Dagda's order, public so that an owner's pass can put
// its own stages between them and decide for itself when repair is due, as
// Macha's does: earn_credit() every pass, then paced_by(), may_start() and
// note_gate() to decide, the collector's inventory step, survey(), and on a turn
// begin_turn(), claim_walks(), lists(), transfer() and settle(). Whether repair
// is quiet is the caller's: settle() says when it may be, and pass() alone keeps
// its own state for that. No step looks at the owner's stop: only `pause`, inside
// the survey's walks, may end one early with MaintenanceStopping, and no step
// catches that or anything else, so what the rest of the pass does then is the
// caller's.
//
// Single owner: one thread calls pass() and the steps. gates() is thread-safe.
class Repair {
 public:
  // `activity`: the clocks the store notes object work on. None: the node's.
  Repair(dannan::Context& context, DistributedStore& store, AvailabilityService& availability, contract::ObjectLedger& ledger, RepairOptions options = {},
         dannan::status::ActivityClocks* activity = nullptr);
  ~Repair();
  Repair(const Repair&) = delete;
  Repair& operator=(const Repair&) = delete;

  static constexpr dannan::sched::Waits pass_waits = AvailabilityService::refresh_waits | dannan::sched::Waits::data_device;
  // Throws MaintenanceStopping out of the survey's walks once `stop` is
  // requested.
  RepairPass pass(std::stop_token stop = {});
  RepairGates gates() const;

  // A turn of repair: begun against the duty share, and closed when this goes,
  // whatever ended it, so a throw cannot leave the share counting it active.
  class Turn {
   public:
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
    ~Turn();

   private:
    friend class Repair;
    Turn(Repair& repair, std::optional<bool> peer) : _repair(repair), _peer(peer) {}
    Repair& _repair;
    // The peer reading the turn keeps; none: it reads telemetry each time.
    const std::optional<bool> _peer;
  };

  static constexpr dannan::sched::ThreadSafety step_safety = dannan::sched::ThreadSafety::single_owner;

  // Accrues credit for the time since the last call, at `now` on repair's
  // clock. `when_no_time_passed`: what to earn for instead when none has, as
  // Macha's pass earns a whole interval (maintenance.cpp:388-390); none earns
  // nothing, as pass() does. Reads the process's CPU.
  static constexpr dannan::sched::Waits earn_credit_waits = dannan::sched::Waits::none;
  RepairCredit earn_credit(dannan::contract::TimePoint now, std::optional<std::chrono::milliseconds> when_no_time_passed = std::nullopt);
  // The credit now, in bytes. Single owner.
  uint64_t credit() const noexcept { return static_cast<uint64_t>(_credit); }

  // Brings the availability survey up to date; whether peers were asked.
  // `pause` is called between pages and rounds. A peer that cannot answer is the
  // survey's own; anything else, and what `pause` throws, is thrown from here.
  static constexpr dannan::sched::Waits survey_waits = AvailabilityService::refresh_waits;
  bool survey(const std::function<void()>& pause = {});

  // The higher classes present now, as RepairPacedBy bits.
  static constexpr dannan::sched::Waits paced_by_waits = dannan::sched::Waits::none;
  uint8_t paced_by() const;
  // Whether the duty share lets a turn start, against `paced`. Macha's gate.repair
  // "share".
  static constexpr dannan::sched::Waits may_start_waits = dannan::sched::Waits::none;
  bool may_start(dannan::contract::TimePoint now, uint8_t paced);
  // How long until may_start() can say yes; zero now. What a pass parks for.
  static constexpr dannan::sched::Waits turn_wait_waits = dannan::sched::Waits::none;
  std::chrono::milliseconds turn_wait(dannan::contract::TimePoint now, uint8_t paced);
  // Counts a pass's gate in gates(), with the credit there is now. A pass may note
  // more than one, as Macha's notes `credit` after `ran`; pass() notes one.
  static constexpr dannan::sched::Waits note_gate_waits = dannan::sched::Waits::none;
  void note_gate(RepairGate gate, uint8_t paced);

  static constexpr dannan::sched::Waits begin_turn_waits = dannan::sched::Waits::none;
  [[nodiscard]] Turn begin_turn(dannan::contract::TimePoint now, uint8_t paced, PeerReading reading = PeerReading::each_time);

  // One bounded step of the DATA claim walk, then one of the CONTROL walk, a
  // missing claim spending an extent of credit and traced through
  // RepairOptions::trace. Runs to its bound: nothing here looks at a stop.
  static constexpr dannan::sched::Waits claim_walks_waits =
      dannan::sched::Waits::state_device | dannan::sched::Waits::data_device | dannan::sched::Waits::network | dannan::sched::Waits::locks;
  RepairWalks claim_walks(Turn& turn);

  // What a transfer works from, as the ledger and the survey stand now.
  static constexpr dannan::sched::Waits lists_waits = dannan::sched::Waits::none;
  RepairLists lists() const;

  // One repair step from `lists` on the credit there is, which it spends: it tops up
  // the window of transfers (the credit's burst) and returns without waiting for what
  // it started, reporting what landed since the last (DistributedStore::repair_step).
  // Yields at an operation boundary on the duty share, and otherwise runs until the
  // credit or the window is spent: nothing here looks at a stop.
  static constexpr dannan::sched::Waits transfer_waits = claim_walks_waits;
  RepairResult transfer(Turn& turn, const RepairLists& lists);

  // Waits up to `deadline` for what the transfers have in flight, then reports what
  // landed since the last step, its pulls paid for from the credit, and starts nothing:
  // for an owner's orderly stop, before its transport goes.
  static constexpr dannan::sched::Waits drain_waits = dannan::sched::Waits::network | dannan::sched::Waits::data_device;
  RepairResult drain(std::chrono::milliseconds deadline);

  // What the turn leaves owing; drops the credit when that is nothing. Anything in
  // flight is more work: a node never goes quiet with copies on the wire.
  static constexpr dannan::sched::Waits settle_waits = dannan::sched::Waits::none;
  RepairSettled settle(const RepairWalks& walks, const RepairResult& step);

 private:
  class Restorer;
  bool higher_class_active(const Turn& turn) const;
  // The link's rate as the credit takes it, and the credit's burst at that rate.
  double bandwidth() const;
  uint64_t burst() const;
  // A step's or a drain's figures in the node's observations.
  void observe(const RepairResult& out, const std::string& load);
  // A peer's telemetry shows foreground traffic now.
  bool peer_foreground_active() const;

  dannan::Context& _context;
  dannan::status::ActivityClocks& _activity;
  DistributedStore& _store;
  AvailabilityService& _availability;
  contract::ObjectLedger& _ledger;
  const RepairOptions _options;
  const dannan::status::ScopedLogger _logger;
  const dannan::contract::TimeSource& _time;
  // Repair's duty share against the classes above it.
  dannan::sched::DutyShare _share;
  ledger::ClaimWalk _data_walk;
  ledger::ClaimWalk _control_walk;
  CpuPacer _cpu;
  double _credit = 0.0;
  // What this turn's claim walks restored, for the transfer's figures.
  size_t _walks_restored = 0;
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
  std::atomic<uint64_t> _last_credit{0};
};

}  // namespace dagda::store
