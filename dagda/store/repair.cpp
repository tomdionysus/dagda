//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/repair.h"

#include <algorithm>

#include "dannan/contract/thread_safety.h"

namespace dagda::store {

using dannan::contract::TimePoint;

// The claim walk's restorer: presence costs nothing, and only a missing
// claimed object, which may be fetched, needs an extent of credit.
class Repair::Restorer final : public ledger::ClaimRestorer {
 public:
  Restorer(DistributedStore& store, double& credit, uint64_t extent) : _store(store), _credit(credit), _extent(extent) {}
  Outcome restore(contract::RetentionClass type, const ObjectId& id) override {
    if (_credit < static_cast<double>(_extent)) return Outcome::waiting_for_credit;
    const bool restored = type == contract::RetentionClass::data ? _store.ensure_local(id, false) : _store.ensure_control_local(id);
    if (!restored) return Outcome::not_restored;
    _credit = std::max(0.0, _credit - static_cast<double>(_extent));
    return Outcome::restored;
  }

 private:
  DistributedStore& _store;
  double& _credit;
  const uint64_t _extent;
};

Repair::Repair(dannan::Context& context, DistributedStore& store, AvailabilityService& availability, contract::ObjectLedger& ledger, RepairOptions options)
    : _context(context),
      _store(store),
      _availability(availability),
      _ledger(ledger),
      _options(options),
      _time(context.time().source()),
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

bool Repair::higher_class_active() const {
  auto& activity = _context.activity();
  return activity.recently_active(dannan::sched::WorkClass::foreground, _options.foreground_quiet) ||
         activity.recently_active(dannan::sched::WorkClass::loader, _options.foreground_quiet) || peer_foreground_active();
}

RepairGates Repair::gates() const {
  RepairGates out;
  out.ran = _ran.load(std::memory_order_relaxed);
  out.share = _gated_share.load(std::memory_order_relaxed);
  out.quiescent = _gated_quiescent.load(std::memory_order_relaxed);
  out.credit = _gated_credit.load(std::memory_order_relaxed);
  if (const auto last = _last_gate.load(std::memory_order_relaxed); last & 0x100U) {
    out.last = static_cast<RepairGate>((last >> 4) & 0xFU);
    out.paced_by = static_cast<uint8_t>(last & 0xFU);
  }
  return out;
}

RepairPass Repair::pass(std::stop_token stop) {
  RepairPass out;
  const auto now = _time.now();
  // Credit: the link's rate, a fraction of it, banked to a burst.
  double bandwidth = _store.estimated_network_bps();
  if (bandwidth <= 0.0) bandwidth = static_cast<double>(_options.initial_bandwidth);
  if (_options.max_bandwidth) bandwidth = std::min(bandwidth, static_cast<double>(_options.max_bandwidth));
  const double seconds = _last ? std::chrono::duration<double>(now - *_last).count() : 0.0;
  _last = now;
  const double burst = std::max(static_cast<double>(_options.extent_size), bandwidth * 5.0);
  const double cpu_scale = _cpu.scale(seconds, _options.cpu_target);
  _credit = std::min(burst, _credit + bandwidth * _options.idle_bandwidth_fraction * cpu_scale * std::max(0.0, seconds));

  out.surveyed = _availability.refresh([&stop] {
    if (stop.stop_requested()) throw MaintenanceStopping{};
  });
  auto& activity = _context.activity();
  if (activity.recently_active(dannan::sched::WorkClass::foreground, _options.foreground_quiet)) out.paced_by |= paced_by_foreground;
  if (activity.recently_active(dannan::sched::WorkClass::loader, _options.foreground_quiet)) out.paced_by |= paced_by_loader;
  if (peer_foreground_active()) out.paced_by |= paced_by_peer_foreground;
  const bool busy = out.paced_by != 0;
  const auto note = [&](RepairGate gate) {
    out.gate = gate;
    out.credit = static_cast<uint64_t>(_credit);
    _last_gate.store(static_cast<uint16_t>(0x100U | (static_cast<unsigned>(gate) << 4) | (out.paced_by & 0xFU)), std::memory_order_relaxed);
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
  };

  RepairLists lists;
  lists.inventory = _ledger.inventory();
  lists.pull = _availability.missing_here();
  lists.lacks = _availability.peer_lacks();
  lists.unavailable = [this](const ObjectId& id) { return _availability.unavailable(id); };
  const std::shared_ptr<const void> identity[3] = {lists.inventory, lists.pull, lists.lacks};
  const auto events = _context.events().total();
  if (_quiescent) {
    if (events == _quiescent_events && std::equal(std::begin(identity), std::end(identity), std::begin(_quiescent_lists))) {
      note(RepairGate::quiescent);
      return out;
    }
    _quiescent = false;
  }
  if (!_share.may_start(now, busy)) {
    note(RepairGate::share);
    return out;
  }
  _share.started(now, busy);
  // A throw from either stage must still close the turn, or the share counts
  // it active for ever and never computes another rest.
  struct Turn {
    Repair& repair;
    ~Turn() { repair._share.finished(repair._time.now(), repair.higher_class_active()); }
  } turn{*this};

  const auto extent = std::max<uint64_t>(1, _options.extent_size);
  Restorer restorer(_store, _credit, extent);
  bool waiting_for_credit = false;
  bool walk_unfinished = false;
  for (auto* walk : {&_data_walk, &_control_walk}) {
    const auto step = walk->step(_ledger, restorer);
    out.restored += step.restored;
    waiting_for_credit = waiting_for_credit || step.waiting_for_credit;
    walk_unfinished = walk_unfinished || step.unfinished;
  }

  out.step = _store.repair_step(lists, static_cast<uint64_t>(_credit), _options.operation_budget,
                                [this] { return _share.should_yield(_time.now(), higher_class_active()); });
  _credit = std::max(0.0, _credit - static_cast<double>(out.step.bytes_transferred));
  if (out.step.credit_limited) {
    // The credit accrued by the next pass covers the next extent.
    note(RepairGate::credit);
  } else {
    note(RepairGate::ran);
    if (out.step.complete && !out.step.yielded && !out.restored && !out.step.bytes_transferred && !walk_unfinished && !waiting_for_credit) {
      // Nothing to do: quiet until an event or the survey's lists change.
      _credit = 0.0;
      _quiescent = true;
      _quiescent_events = events;
      std::copy(std::begin(identity), std::end(identity), std::begin(_quiescent_lists));
    }
  }
  return out;
}

}  // namespace dagda::store
