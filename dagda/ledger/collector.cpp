//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/collector.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "dagda/store/slow_stage.h"

namespace dagda::ledger {
namespace {

using contract::RetentionClass;

std::string flag(std::string_view name, bool value) { return std::string(name) + (value ? "=1" : "=0"); }

// Whether a retirement's grace has run at `now_ns` on this node's clock. One
// never stamped never has.
bool matured(int64_t retired_at_ns, int64_t now_ns, int64_t grace_ns) {
  return retired_at_ns > 0 && now_ns >= retired_at_ns && now_ns - retired_at_ns >= grace_ns;
}

int64_t grace_ns(std::chrono::milliseconds grace) { return std::chrono::duration_cast<std::chrono::nanoseconds>(grace).count(); }

std::string joined_ids(const std::vector<contract::Garbage>& refs) {
  std::vector<std::string> hex;
  for (const auto& ref : refs) hex.push_back(store::to_string(ref.id));
  std::sort(hex.begin(), hex.end());
  std::string joined;
  for (const auto& id : hex) joined += (joined.empty() ? "" : ",") + id;
  return joined;
}

}  // namespace

Collector::Collector(contract::ObjectLedger& ledger, contract::ReachabilityProvider& provider, store::StoragePool& data, store::LocalStore& control,
                     UnreferencedSince& data_sightings, UnreferencedSince& control_sightings, CollectorOptions options, CollectorServices services,
                     store::PersistentBlockCache* cache, Trace trace)
    : _ledger(ledger),
      _provider(provider),
      _data(data),
      _control(control),
      _data_sightings(data_sightings),
      _control_sightings(control_sightings),
      _options(options),
      _services(std::move(services)),
      _cache(cache),
      _trace(std::move(trace)),
      _builder(provider, _services.logger) {}

void Collector::trace(std::string_view kind, const std::string& detail) const {
  if (_trace) _trace(kind, detail);
}

// A sweep slice can add sightings every pass; the table is written when a
// sweep completes and otherwise at most twice a minute. A sighting lost to a
// crash is taken again, which only delays that object's deletion.
void Collector::save_sightings(UnreferencedSince& sightings, dannan::contract::TimePoint& saved, bool complete) {
  const auto now = _services.time.now();
  if (!complete && now - saved < std::chrono::seconds(30)) return;
  saved = now;
  sightings.save();
}

void Collector::collect_tombstones(const contract::InventoryHorizon& inventory) {
  const auto now_ns = static_cast<int64_t>(_services.wall.unix_ms()) * 1'000'000;
  const auto grace = grace_ns(_options.garbage_grace);
  contract::GarbageCollection collection;
  collection.erase = inventory.stale_garbage();
  size_t collected = 0;
  for (const auto& candidate : inventory.garbage()) {
    // A zero retirement time is one never stamped: ineligible until the
    // owner stamps it, which grants it a full grace from then.
    if (candidate.retired_at_ns == 0) collection.stamp.push_back(candidate);
    if (!matured(candidate.retired_at_ns, now_ns, grace)) continue;
    // A matured retirement no longer protects the object from the sweep. Its
    // bytes are not deleted here: the sweep's age check and remove spares an
    // object reaffirmed since, even if this inventory is stale. A cache copy
    // is disposable and goes now.
    if (_cache) _cache->remove(candidate.id);
    collection.erase.push_back(candidate);
    ++collected;
  }
  for (const auto& batch : inventory.batches())
    if (matured(batch.retired_at_ns, now_ns, grace)) collection.matured_batches.push_back(batch.id);
  _services.observations.add("maintenance.tombstones.collected", collected);
  if (!collection.erase.empty()) trace("tombstones", "erase=[" + joined_ids(collection.erase) + "]");
  // Macha's pass stamps nothing, so its trace has no word for this.
  if (!collection.stamp.empty()) trace("tombstones", "stamp=[" + joined_ids(collection.stamp) + "]");
  _provider.collect_garbage(collection);
}

contract::PassFacts Collector::facts(const PassDue& due, const InventoryStep& inventory) const {
  contract::PassFacts out;
  out.garbage_due = due.garbage_due;
  out.gc_due = due.gc_due;
  out.busy = due.busy;
  out.gc_waiting_for_event = due.gc_waiting_for_event;
  out.rebuilt_inventory = inventory.rebuilt;
  return out;
}

void Collector::trace_gate(std::string_view gate, const contract::GateVerdict& verdict) const {
  trace(gate, (verdict.permitted ? "open " : "shut ") + verdict.conditions);
}

InventoryStep Collector::inventory_step(const std::function<void()>& pause, bool may_rebuild) {
  InventoryStep out;
  const auto started = dannan::contract::Clock::now();
  // The horizon this pass reads: the ledger's, as published.
  out.inventory = _ledger.inventory();
  const auto generation = _provider.generation();
  const bool stale = !out.inventory || !out.inventory->complete() || out.inventory->generation() != generation;
  if (!stale) return out;
  if (out.inventory && !may_rebuild) {
    out.held_back = true;
    return out;
  }
  // The pass reads what the ledger holds, never its own copy.
  _ledger.publish(_builder.inventory(pause));
  out.inventory = _ledger.inventory();
  out.rebuilt = true;
  _services.observations.record("maintenance.inventory.build_us", store::stage_elapsed_us(started));
  trace("inventory",
        "generation=" + std::to_string(out.inventory->generation()) + " live=" + std::to_string(out.inventory->size(RetentionClass::data)) +
            " control_live=" + std::to_string(out.inventory->size(RetentionClass::control)) + " garbage=" + std::to_string(out.inventory->garbage().size()) +
            " stale_garbage=" + std::to_string(out.inventory->stale_garbage().size()) + " " + flag("catalogue_complete", out.inventory->complete()));
  _services.observations.add("maintenance.inventory.live_objects", out.inventory->size(RetentionClass::data));
  return out;
}

TombstoneStep Collector::tombstone_step(const PassDue& due, const InventoryStep& inventory) {
  TombstoneStep out;
  out.gate = contract::tombstone_gate(facts(due, inventory), inventory.inventory.get());
  trace_gate("gate.tombstones", out.gate);
  if (out.gate.permitted && inventory.inventory) collect_tombstones(*inventory.inventory);
  return out;
}

ReleaseStep Collector::release_step() {
  ReleaseStep out;
  out.release = _ledger.release();
  // Release is local and causal: the sole accepted head gives a complete
  // set and the clock of the claim writes it has observed. Claims from
  // branches it has not seen are not dominated by that clock and survive.
  out.head = _provider.release_head();
  if (out.head && *out.head != (out.release ? out.release->head() : contract::Hash256{})) {
    dannan::status::ObservedDuration horizon_build(_services.observations, "maintenance.release_horizon.build_us");
    auto built = _builder.release(*out.head);
    out.built = true;
    out.complete = built.complete;
    trace("release-horizon", std::string(built.complete ? "complete" : "incomplete") +
                                 " data_live=" + std::to_string(built.horizon->size(RetentionClass::data)) +
                                 " control_live=" + std::to_string(built.horizon->size(RetentionClass::control)));
    // Refused when incomplete: the ledger keeps the previous.
    (void)_ledger.publish(std::move(built));
    out.release = _ledger.release();
  }
  return out;
}

ControlStep Collector::control_step(const PassDue& due, const InventoryStep& inventory, const ReleaseStep& release) {
  ControlStep out;
  auto gate_facts = facts(due, inventory);
  gate_facts.release_view = release.head.has_value();
  gate_facts.known_generation = _provider.known_generation();
  out.gate = contract::control_gate(gate_facts, inventory.inventory.get());
  // The rule every gate keeps: a newly built inventory is never used
  // destructively in the pass that built it.
  if (inventory.rebuilt) trace("control-gate-in-rebuilding-pass", out.gate.permitted ? "open" : "shut");
  trace_gate("gate.control", out.gate);
  if (!out.gate.permitted) return out;
  // What another node's branch may still refer to is kept by the grace the
  // sweep applies, on this node's clock.
  if (release.release) {
    out.released = _ledger.release_unreferenced(RetentionClass::control, *release.release, _options.release_budget);
    _services.observations.add("retention.released.control", out.released);
    if (out.released) trace("release.control", std::to_string(out.released));
  }
  out.removed = control_gc_step(inventory.inventory->lookup(RetentionClass::control), _options.garbage_grace, _options.control_budget, &_control_sightings,
                                _services.wall.unix_ms());
  save_sightings(_control_sightings, _control_saved, false);
  _services.observations.add("catalogue.control_gc.removed", out.removed);
  if (out.removed) trace("control-gc", std::to_string(out.removed));
  out.pruned = _ledger.prune_unclaimed(RetentionClass::control, _options.prune_budget);
  _services.observations.add("retention.pruned.control", out.pruned);
  if (out.pruned) trace("prune.control", std::to_string(out.pruned));
  if (out.removed) _services.logger.debug("control GC removed=" + std::to_string(out.removed));
  return out;
}

DataStep Collector::data_step(const PassDue& due, const InventoryStep& inventory, const ReleaseStep& release) {
  DataStep out;
  // The sweep also catches objects that never got a retirement (a DATA put
  // whose process died before its commit). Recent and unstamped retirements
  // stay protected for the grace. Claims are checked before every delete, so
  // an inventory older than the known generation is still safe for orphans.
  auto gate_facts = facts(due, inventory);
  gate_facts.release_view = release.head.has_value();
  gate_facts.known_generation = _provider.known_generation();
  out.gate = contract::data_gate(gate_facts, inventory.inventory.get());
  trace_gate("gate.data", out.gate);
  if (out.gate.reason != _last_skip_reason) {
    _last_skip_reason = out.gate.reason;
    _services.logger.debug(std::string("garbage collection sweep") + (_last_skip_reason.empty() ? " enabled" : " skipped: " + _last_skip_reason));
  }
  // A retirement carries its own deadline. Unretired bytes may be the data
  // half of a publication with no reference yet, so they wait one retry
  // window; a zero grace must not collapse it.
  out.orphan_grace = std::max(_options.garbage_grace, _options.retry_window);
  if (!out.gate.permitted) return out;

  if (release.release) {
    out.released = _ledger.release_unreferenced(RetentionClass::data, *release.release, _options.release_budget);
    _services.observations.add("retention.released.data", out.released);
    if (out.released) trace("release.data", std::to_string(out.released));
  } else if (!_release_incomplete_logged) {
    _release_incomplete_logged = true;
    _services.logger.debug("retention release skipped: horizon incomplete");
  }
  const auto now_ms = _services.wall.unix_ms();
  const auto now_ns = static_cast<int64_t>(now_ms) * 1'000'000;
  const auto& live = *inventory.inventory;
  std::vector<ObjectId> protected_ids;
  protected_ids.reserve(live.garbage().size());
  for (const auto& candidate : live.garbage())
    if (!matured(candidate.retired_at_ns, now_ns, grace_ns(_options.garbage_grace))) protected_ids.push_back(candidate.id);
  std::sort(protected_ids.begin(), protected_ids.end());
  protected_ids.erase(std::unique(protected_ids.begin(), protected_ids.end()), protected_ids.end());

  const auto started = dannan::contract::Clock::now();
  out.swept = _data.gc_step(
      live.lookup(RetentionClass::data), protected_ids, out.orphan_grace, _options.data_budget, due.should_yield,
      [this](const ObjectId& id) { return _ledger.retained(RetentionClass::data, id); }, &_data_sightings, now_ms, _options.garbage_grace);
  save_sightings(_data_sightings, _data_saved, out.swept.complete);
  if (out.swept.bytes) trace("gc", "reclaimed_bytes=" + std::to_string(out.swept.bytes));
  _services.observations.add("maintenance.gc.examined", out.swept.objects);
  _services.observations.add("maintenance.gc.reclaimed_bytes", out.swept.bytes);
  const auto step_us = store::stage_elapsed_us(started);
  _services.observations.record("maintenance.gc.step_us", step_us);
  // Objects examined, reclaimed or not.
  if (out.swept.objects) _services.observations.record("maintenance.gc.per_object_ns", step_us * 1000 / out.swept.objects);
  store::log_slow_stage(_services.logger, "garbage-collect", started,
                        "reclaimed_bytes=" + std::to_string(out.swept.bytes) + " objects=" + std::to_string(out.swept.objects));
  out.pruned = _ledger.prune_unclaimed(RetentionClass::data, _options.prune_budget);
  _services.observations.add("retention.pruned.data", out.pruned);
  if (out.pruned) trace("prune.data", std::to_string(out.pruned));
  return out;
}

PassReport Collector::pass(const PassDue& due) {
  PassReport report;
  if (!due.garbage_due && !due.gc_due) return report;
  const auto inventory = inventory_step(due.pause);
  report.rebuilt_inventory = inventory.rebuilt;
  report.tombstones = tombstone_step(due, inventory).gate;
  const auto release = release_step();
  auto control = control_step(due, inventory, release);
  report.control = std::move(control.gate);
  report.released_control = control.released;
  report.removed_control = control.removed;
  report.pruned_control = control.pruned;
  auto data = data_step(due, inventory, release);
  report.data = std::move(data.gate);
  report.released_data = data.released;
  report.swept_data = data.swept;
  report.pruned_data = data.pruned;
  return report;
}

size_t Collector::control_gc_step(const contract::IdLookup& live, std::chrono::milliseconds grace, size_t budget, UnreferencedSince* sightings,
                                  uint64_t now_unix_ms) {
  if (!budget) return 0;
  // Publication is data before metadata, so a future state's CONTROL objects
  // are briefly unreferenced, and time is no fence: the state may move on
  // between staging and a pass. An unreferenced object survives the epoch
  // in which GC first sees (or re-affirms) it; only a later epoch orphans it.
  const auto epoch = _provider.control_epoch();
  if (!epoch) return 0;
  const auto since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(_services.time.now() - epoch->since);
  const auto staged_since_epoch = since_epoch + std::chrono::milliseconds(1);

  size_t removed = 0;
  bool exhausted = false;
  for (size_t operations = 0; operations < budget && !exhausted; ++operations) {
    const auto id = _control.next_object(_control_cursor, exhausted);
    if (!id) continue;

    if (live.contains(*id)) {
      if (sightings) sightings->forget(*id);
      _unreferenced_epoch.erase(*id);
      continue;
    }
    if (sightings && _ledger.retained(RetentionClass::control, *id)) {
      sightings->forget(*id);
      continue;
    }
    const bool waited = !sightings || sightings->matured(*id, now_unix_ms, grace);

    // A content-addressed object may be reused by the current publication;
    // put() touches it, so any object touched since the epoch began stays.
    if (!_control.older_than(*id, staged_since_epoch)) {
      (void)_provider.within_control_epoch(epoch->sequence, [&] { _unreferenced_epoch[*id] = epoch->sequence; });
      continue;
    }

    // Held in its epoch, so GC never races a new one and deletes what it
    // staged.
    (void)_provider.within_control_epoch(epoch->sequence, [&] {
      auto [seen, inserted] = _unreferenced_epoch.emplace(*id, epoch->sequence);
      if (inserted || seen->second == epoch->sequence) return;
      if (!waited || _ledger.retained(RetentionClass::control, *id)) return;
      if (_control.remove_if_older_than(*id, grace)) {
        _unreferenced_epoch.erase(seen);
        if (sightings) sightings->forget(*id);
        ++removed;
      }
    });
  }
  if (exhausted && sightings) sightings->pass_complete();
  if (exhausted && removed) (void)_control.compact_packs();
  return removed;
}

}  // namespace dagda::ledger
