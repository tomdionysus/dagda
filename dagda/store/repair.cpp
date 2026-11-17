//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/repair.h"

#include <algorithm>
#include <string>

#include "dagda/store/slow_stage.h"
#include "dannan/contract/thread_safety.h"

namespace dagda::store {

using dannan::contract::TimePoint;

// The claim walk's restorer: presence costs nothing, and only a missing
// claimed object, which may be fetched, needs an extent of credit.
class Repair::Restorer final : public ledger::ClaimRestorer {
 public:
  Restorer(DistributedStore& store, double& credit, uint64_t extent, const RepairTrace& trace)
      : _store(store), _credit(credit), _extent(extent), _trace(trace) {}
  Outcome restore(contract::RetentionClass type, const ObjectId& id) override {
    const std::string walked = std::string(type == contract::RetentionClass::data ? "data " : "control ") + to_string(id);
    const auto trace = [&](const char* outcome) {
      if (_trace) _trace("claim-walk", walked + " " + outcome);
    };
    if (_credit < static_cast<double>(_extent)) {
      trace("waiting-for-credit");
      return Outcome::waiting_for_credit;
    }
    const bool restored = type == contract::RetentionClass::data ? _store.ensure_local(id, false) : _store.ensure_control_local(id);
    trace(restored ? "restored" : "not-restored");
    if (!restored) return Outcome::not_restored;
    _credit = std::max(0.0, _credit - static_cast<double>(_extent));
    return Outcome::restored;
  }

 private:
  DistributedStore& _store;
  double& _credit;
  const uint64_t _extent;
  const RepairTrace& _trace;
};

// A throw from either stage must still close the turn, or the share counts it
// active for ever and never computes another rest.
Repair::Turn::~Turn() { _repair._share.finished(_repair._time.now(), _repair.higher_class_active(*this)); }

Repair::Repair(dannan::Context& context, DistributedStore& store, AvailabilityService& availability, contract::ObjectLedger& ledger, RepairOptions options,
               dannan::status::ActivityClocks* activity)
    : _context(context),
      _activity(activity ? *activity : context.activity()),
      _store(store),
      _availability(availability),
      _ledger(ledger),
      _options(options),
      _logger(context.logger("dagda.repair")),
      _time(options.time ? *options.time : context.time().source()),
      _share(options.foreground_weight, options.repair_weight, options.slice),
      _data_walk(contract::RetentionClass::data, &context.observations()),
      _control_walk(contract::RetentionClass::control, &context.observations()),
      _cpu(options.cpu) {}

Repair::~Repair() = default;

bool Repair::peer_foreground_active() const {
  const auto self = _context.identity().id();
  for (const auto& view : _context.telemetry().views(_options.peer_window)) {
    if (!view.fresh || view.telemetry.node_id() == self) continue;
    for (const auto& entry : view.telemetry.traffic()) {
      const auto frame = dannan::sched::decode_frame_type(entry.frame_class);
      if (frame && dannan::sched::work_class(*frame) == dannan::sched::WorkClass::foreground && (entry.in_bytes_per_s || entry.out_bytes_per_s)) return true;
    }
  }
  return false;
}

bool Repair::higher_class_active(const Turn& turn) const {
  auto& activity = _activity;
  return activity.recently_active(dannan::sched::WorkClass::foreground, _options.foreground_quiet) ||
         activity.recently_active(dannan::sched::WorkClass::loader, _options.foreground_quiet) || (turn._peer ? *turn._peer : peer_foreground_active());
}

RepairGates Repair::gates() const {
  RepairGates out;
  out.ran = _ran.load(std::memory_order_relaxed);
  out.share = _gated_share.load(std::memory_order_relaxed);
  out.quiescent = _gated_quiescent.load(std::memory_order_relaxed);
  out.credit = _gated_credit.load(std::memory_order_relaxed);
  out.last_credit_bytes = _last_credit.load(std::memory_order_relaxed);
  if (const auto last = _last_gate.load(std::memory_order_relaxed); last & 0x100U) {
    out.last = static_cast<RepairGate>((last >> 4) & 0xFU);
    out.paced_by = static_cast<uint8_t>(last & 0xFU);
  }
  return out;
}

double Repair::bandwidth() const {
  double out = _store.estimated_network_bps();
  if (out <= 0.0) out = static_cast<double>(_options.initial_bandwidth);
  if (_options.max_bandwidth) out = std::min(out, static_cast<double>(_options.max_bandwidth));
  return out;
}

uint64_t Repair::burst() const { return static_cast<uint64_t>(std::max(static_cast<double>(_options.extent_size), bandwidth() * 5.0)); }

RepairCredit Repair::earn_credit(TimePoint now, std::optional<std::chrono::milliseconds> when_no_time_passed) {
  // Credit: the link's rate, a fraction of it, banked to a burst.
  const double link = bandwidth();
  double seconds = _last ? std::chrono::duration<double>(now - *_last).count() : 0.0;
  _last = now;
  if (seconds <= 0.0 && when_no_time_passed) seconds = std::chrono::duration<double>(*when_no_time_passed).count();
  const double cpu_scale = _cpu.scale(seconds, _options.cpu_target);
  const double rate = link * _options.idle_bandwidth_fraction * cpu_scale;
  _credit = std::min(static_cast<double>(burst()), _credit + rate * std::max(0.0, seconds));
  return {credit(), rate, seconds};
}

bool Repair::survey(const std::function<void()>& pause) { return _availability.refresh(pause); }

uint8_t Repair::paced_by() const {
  uint8_t out = 0;
  auto& activity = _activity;
  if (activity.recently_active(dannan::sched::WorkClass::foreground, _options.foreground_quiet)) out |= paced_by_foreground;
  if (activity.recently_active(dannan::sched::WorkClass::loader, _options.foreground_quiet)) out |= paced_by_loader;
  if (peer_foreground_active()) out |= paced_by_peer_foreground;
  return out;
}

bool Repair::may_start(TimePoint now, uint8_t paced) { return _share.may_start(now, paced != 0); }

std::chrono::milliseconds Repair::turn_wait(TimePoint now, uint8_t paced) { return _share.wait(now, paced != 0); }

void Repair::note_gate(RepairGate gate, uint8_t paced) {
  _last_credit.store(credit(), std::memory_order_relaxed);
  _last_gate.store(static_cast<uint16_t>(0x100U | (static_cast<unsigned>(gate) << 4) | (paced & 0xFU)), std::memory_order_relaxed);
  switch (gate) {
    case RepairGate::ran:
      _ran.fetch_add(1, std::memory_order_relaxed);
      break;
    case RepairGate::share:
      _gated_share.fetch_add(1, std::memory_order_relaxed);
      break;
    case RepairGate::quiescent:
      _gated_quiescent.fetch_add(1, std::memory_order_relaxed);
      break;
    case RepairGate::credit:
      _gated_credit.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

Repair::Turn Repair::begin_turn(TimePoint now, uint8_t paced, PeerReading reading) {
  _share.started(now, paced != 0);
  return Turn(*this, reading == PeerReading::as_begun ? std::optional<bool>((paced & paced_by_peer_foreground) != 0) : std::nullopt);
}

RepairWalks Repair::claim_walks(Turn&) {
  RepairWalks out;
  const auto extent = std::max<uint64_t>(1, _options.extent_size);
  Restorer restorer(_store, _credit, extent, _options.trace);
  for (auto* walk : {&_data_walk, &_control_walk}) {
    const auto step = walk->step(_ledger, restorer);
    out.examined += step.examined;
    out.missing += step.missing;
    out.restored += step.restored;
    out.waiting_for_credit = out.waiting_for_credit || step.waiting_for_credit;
    out.unfinished = out.unfinished || step.unfinished;
  }
  _walks_restored = out.restored;
  return out;
}

RepairLists Repair::lists() const {
  RepairLists out;
  out.inventory = _ledger.inventory();
  out.pull = _availability.missing_here();
  out.lacks = _availability.peer_lacks();
  out.unavailable = [this](const ObjectId& id) { return _availability.unavailable(id); };
  out.holds = _availability.peer_holds();
  return out;
}

RepairResult Repair::transfer(Turn& turn, const RepairLists& lists) {
  const auto started = dannan::contract::Clock::now();
  auto out = _store.repair_step(lists, static_cast<uint64_t>(_credit), RepairWindow{burst()},
                                [&] { return _share.should_yield(_time.now(), higher_class_active(turn)); });
  // Split by whether a higher class was present as the step ended: repair idle
  // against repair on its share.
  const std::string load = higher_class_active(turn) ? "loaded" : "idle";
  auto& observations = _context.observations();
  observations.record("maintenance.repair.step_us." + load, stage_elapsed_us(started));
  observe(out, load);
  observations.add("maintenance.repair.retained_repairs", _walks_restored);
  log_slow_stage(_logger, "network-repair", started,
                 "bytes=" + std::to_string(out.bytes_transferred) + " retained_repairs=" + std::to_string(_walks_restored) +
                     " push_examined=" + std::to_string(out.push_examined) + " pull_examined=" + std::to_string(out.pull_examined) +
                     " remote_ops=" + std::to_string(out.remote_operations) + " in_flight=" + std::to_string(out.in_flight) +
                     " complete=" + std::to_string(out.complete ? 1 : 0) + " yielded=" + std::to_string(out.yielded ? 1 : 0));
  _walks_restored = 0;
  _credit = std::max(0.0, _credit - static_cast<double>(out.credit_spent));
  return out;
}

void Repair::observe(const RepairResult& out, const std::string& load) {
  auto& observations = _context.observations();
  observations.add("maintenance.repair.bytes." + load, out.bytes_transferred);
  observations.add("maintenance.repair.push_examined." + load, out.push_examined);
  observations.add("maintenance.repair.pull_examined." + load, out.pull_examined);
}

RepairResult Repair::drain(std::chrono::milliseconds deadline) {
  auto out = _store.repair_drain(deadline);
  _credit = std::max(0.0, _credit - static_cast<double>(out.credit_spent));
  // What landed is recorded as a step's is, split by whether a higher class is present.
  observe(out, paced_by() ? "loaded" : "idle");
  return out;
}

RepairSettled Repair::settle(const RepairWalks& walks, const RepairResult& step) {
  if (step.credit_limited) return RepairSettled::credit;
  if (step.yielded) return RepairSettled::yielded;
  if (!step.complete || step.in_flight || walks.restored || step.bytes_transferred || walks.unfinished) return RepairSettled::more_work;
  if (walks.waiting_for_credit) return RepairSettled::waiting_for_credit;
  _credit = 0.0;
  return RepairSettled::quiescent;
}

RepairPass Repair::pass(std::stop_token stop) {
  RepairPass out;
  const auto now = _time.now();
  (void)earn_credit(now);
  out.surveyed = survey([&stop] {
    if (stop.stop_requested()) throw MaintenanceStopping{};
  });
  out.paced_by = paced_by();
  const auto note = [&](RepairGate gate) {
    out.gate = gate;
    out.credit = credit();
    note_gate(gate, out.paced_by);
  };

  const auto current = lists();
  const std::shared_ptr<const void> identity[3] = {current.inventory, current.pull, current.lacks};
  const auto events = _context.events().total();
  if (_quiescent) {
    if (events == _quiescent_events && std::equal(std::begin(identity), std::end(identity), std::begin(_quiescent_lists))) {
      note(RepairGate::quiescent);
      return out;
    }
    _quiescent = false;
  }
  if (!may_start(now, out.paced_by)) {
    note(RepairGate::share);
    return out;
  }
  auto turn = begin_turn(now, out.paced_by);
  const auto walks = claim_walks(turn);
  out.restored = walks.restored;
  out.step = transfer(turn, current);
  if (out.step.credit_limited) {
    // The credit accrued by the next pass covers the next extent.
    note(RepairGate::credit);
    return out;
  }
  note(RepairGate::ran);
  if (settle(walks, out.step) == RepairSettled::quiescent) {
    // Nothing to do: quiet until an event or the survey's lists change.
    _quiescent = true;
    _quiescent_events = events;
    std::copy(std::begin(identity), std::end(identity), std::begin(_quiescent_lists));
  }
  return out;
}

}  // namespace dagda::store
