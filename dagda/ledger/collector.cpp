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
  if (!collection.erase.empty() || !collection.stamp.empty())
    trace("tombstones", "erase=[" + joined_ids(collection.erase) + "] stamp=[" + joined_ids(collection.stamp) + "]");
  _provider.collect_garbage(collection);
}

PassReport Collector::pass(const PassDue& due) {
  PassReport report;
  if (!due.garbage_due && !due.gc_due) return report;

  // The horizons this pass reads: the ledger's, as published.
  auto inventory = _ledger.inventory();
  auto release = _ledger.release();
  const auto generation = _provider.generation();
  if (!inventory || !inventory->complete() || inventory->generation() != generation) {
    // The pass reads what the ledger holds, never its own copy.
    _ledger.publish(_builder.inventory());
    inventory = _ledger.inventory();
    report.rebuilt_inventory = true;
    trace("inventory", "generation=" + std::to_string(inventory->generation()) + " live=" + std::to_string(inventory->size(RetentionClass::data)) +
                           " control_live=" + std::to_string(inventory->size(RetentionClass::control)) +
                           " garbage=" + std::to_string(inventory->garbage().size()) + " stale_garbage=" + std::to_string(inventory->stale_garbage().size()) +
                           " " + flag("catalogue_complete", inventory->complete()));
    _services.observations.add("maintenance.inventory.live_objects", inventory->size(RetentionClass::data));
  }

  contract::PassFacts facts;
  facts.garbage_due = due.garbage_due;
  facts.gc_due = due.gc_due;
  facts.busy = due.busy;
  facts.gc_waiting_for_event = due.gc_waiting_for_event;
  facts.rebuilt_inventory = report.rebuilt_inventory;
  const auto trace_gate = [&](std::string_view gate, const contract::GateVerdict& verdict) {
    trace(gate, (verdict.permitted ? "open " : "shut ") + verdict.conditions);
  };

  report.tombstones = contract::tombstone_gate(facts, inventory.get());
  trace_gate("gate.tombstones", *report.tombstones);
  if (report.tombstones->permitted && inventory) collect_tombstones(*inventory);

  // Release is local and causal: the sole accepted head gives a complete
  // set and the clock of the claim writes it has observed. Claims from
  // branches it has not seen are not dominated by that clock and survive.
  const auto head = _provider.release_head();
  if (head && *head != (release ? release->head() : contract::Hash256{})) {
    auto built = _builder.release(*head);
    trace("release-horizon", std::string(built.complete ? "complete" : "incomplete") +
                                 " data_live=" + std::to_string(built.horizon->size(RetentionClass::data)) +
                                 " control_live=" + std::to_string(built.horizon->size(RetentionClass::control)));
    // Refused when incomplete: the ledger keeps the previous.
    (void)_ledger.publish(std::move(built));
    release = _ledger.release();
  }

  facts.release_view = head.has_value();
  facts.known_generation = _provider.known_generation();
  report.control = contract::control_gate(facts, inventory.get());
  // The rule every gate keeps: a newly built inventory is never used
  // destructively in the pass that built it.
  if (report.rebuilt_inventory) trace("control-gate-in-rebuilding-pass", report.control->permitted ? "open" : "shut");
  trace_gate("gate.control", *report.control);
  if (report.control->permitted) {
    // What another node's branch may still refer to is kept by the grace the
    // sweep applies, on this node's clock.
    if (release) {
      report.released_control = _ledger.release_unreferenced(RetentionClass::control, *release, _options.release_budget);
      _services.observations.add("retention.released.control", report.released_control);
      if (report.released_control) trace("release.control", std::to_string(report.released_control));
    }
    report.removed_control = control_gc_step(inventory->lookup(RetentionClass::control), _options.garbage_grace, _options.control_budget, &_control_sightings,
                                             _services.wall.unix_ms());
    save_sightings(_control_sightings, _control_saved, false);
    _services.observations.add("catalogue.control_gc.removed", report.removed_control);
    if (report.removed_control) trace("control-gc", std::to_string(report.removed_control));
    report.pruned_control = _ledger.prune_unclaimed(RetentionClass::control, _options.prune_budget);
    _services.observations.add("retention.pruned.control", report.pruned_control);
    if (report.pruned_control) trace("prune.control", std::to_string(report.pruned_control));
    if (report.removed_control) _services.logger.debug("control GC removed=" + std::to_string(report.removed_control));
  }

  // The sweep also catches objects that never got a retirement (a DATA put
  // whose process died before its commit). Recent and unstamped retirements
  // stay protected for the grace. Claims are checked before every delete, so
  // an inventory older than the known generation is still safe for orphans.
  facts.known_generation = _provider.known_generation();
  report.data = contract::data_gate(facts, inventory.get());
  trace_gate("gate.data", *report.data);
  if (report.data->reason != _last_skip_reason) {
    _last_skip_reason = report.data->reason;
    _services.logger.debug(std::string("garbage collection sweep") + (_last_skip_reason.empty() ? " enabled" : " skipped: " + _last_skip_reason));
  }
  if (!report.data->permitted) return report;

  if (release) {
    report.released_data = _ledger.release_unreferenced(RetentionClass::data, *release, _options.release_budget);
    _services.observations.add("retention.released.data", report.released_data);
    if (report.released_data) trace("release.data", std::to_string(report.released_data));
  } else if (!_release_incomplete_logged) {
    _release_incomplete_logged = true;
    _services.logger.debug("retention release skipped: horizon incomplete");
  }
  const auto now_ms = _services.wall.unix_ms();
  const auto now_ns = static_cast<int64_t>(now_ms) * 1'000'000;
  std::vector<ObjectId> protected_ids;
  protected_ids.reserve(inventory->garbage().size());
  for (const auto& candidate : inventory->garbage())
    if (!matured(candidate.retired_at_ns, now_ns, grace_ns(_options.garbage_grace))) protected_ids.push_back(candidate.id);
  std::sort(protected_ids.begin(), protected_ids.end());
  protected_ids.erase(std::unique(protected_ids.begin(), protected_ids.end()), protected_ids.end());

  // A retirement carries its own deadline. Unretired bytes may be the data
  // half of a publication with no reference yet, so they wait one retry
  // window; a zero grace must not collapse it.
  const auto orphan_grace = std::max(_options.garbage_grace, _options.retry_window);
  report.swept_data = _data.gc_step(
      inventory->lookup(RetentionClass::data), protected_ids, orphan_grace, _options.data_budget, due.should_yield,
      [this](const ObjectId& id) { return _ledger.retained(RetentionClass::data, id); }, &_data_sightings, now_ms, _options.garbage_grace);
  save_sightings(_data_sightings, _data_saved, report.swept_data.complete);
  if (report.swept_data.bytes) trace("gc", "reclaimed_bytes=" + std::to_string(report.swept_data.bytes));
  _services.observations.add("maintenance.gc.examined", report.swept_data.objects);
  _services.observations.add("maintenance.gc.reclaimed_bytes", report.swept_data.bytes);
  report.pruned_data = _ledger.prune_unclaimed(RetentionClass::data, _options.prune_budget);
  _services.observations.add("retention.pruned.data", report.pruned_data);
  if (report.pruned_data) trace("prune.data", std::to_string(report.pruned_data));
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
