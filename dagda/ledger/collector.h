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
#include <optional>
#include <string>
#include <string_view>

#include "dagda/contract/gates.h"
#include "dagda/contract/id_lookup.h"
#include "dagda/contract/object_ledger.h"
#include "dagda/contract/reachability.h"
#include "dagda/ledger/node_horizon_builder.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/store/local_store.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/storage_pool.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "dannan/status/observation.h"

namespace dagda::ledger {

// Macha's maintenance settings the collector reads (config.hpp
// MaintenanceConfig), with its defaults: the node's own budgets, which a
// node's options carry, and the domain's wait.
struct CollectorBudgets {
  // DATA no reference has reached yet may be the data half of a publication
  // still in flight: it waits at least this retry window, whatever the grace
  // (Macha's no_progress_backoff).
  std::chrono::milliseconds retry_window{300000};
  // Per pass: claims released and causality tombstones pruned per class,
  // objects the DATA and CONTROL sweeps examine.
  size_t release_budget = 64;
  size_t prune_budget = 64;
  size_t data_budget = 64;
  size_t control_budget = 32;
};

struct CollectorOptions : CollectorBudgets {
  // The absence horizon: an object goes only once this node has seen it
  // unreferenced and unclaimed for this long, on its own clock, and a
  // retirement is collectable this long after it was stamped.
  std::chrono::milliseconds garbage_grace{std::chrono::hours(24 * 30)};
  // A test's seam: told as a CONTROL object is about to go, past every check.
  std::function<void(const store::ObjectId&)> before_control_removal;
};

// What the collector runs on, each one per node and injected.
struct CollectorServices {
  dannan::status::ScopedLogger logger;
  dannan::status::Observations& observations;
  // When the sightings are saved, and how long a CONTROL epoch has stood.
  const dannan::contract::TimeSource& time = dannan::contract::steady_time_source();
  // This node's own clock: sightings and retirement ages.
  const dannan::contract::WallClock& wall = dannan::contract::system_wall_clock();
};

// What the caller's pass knows of the node's activity.
struct PassDue {
  // Tombstone collection is due: no foreground work, the owner's state not
  // dirty.
  bool garbage_due = false;
  // Garbage collection is due: no foreground work, past its quiet window.
  bool gc_due = false;
  bool busy = false;
  bool gc_waiting_for_event = false;
  // Asked between objects of the DATA sweep: foreground or loader work in
  // its quiet window.
  std::function<bool()> should_yield;
  // Given to the owner's inventory walk: throws MaintenanceStopping to end it.
  std::function<void()> pause;
};

struct PassReport {
  // An inventory was built and published this pass: nothing destructive used
  // it, so the caller schedules a pass after its quiet window that will.
  bool rebuilt_inventory = false;
  // The gates the pass reached, in order.
  std::optional<contract::GateVerdict> tombstones;
  std::optional<contract::GateVerdict> control;
  std::optional<contract::GateVerdict> data;
  size_t released_control = 0;
  size_t removed_control = 0;
  size_t pruned_control = 0;
  size_t released_data = 0;
  store::StoragePool::MaintenanceResult swept_data;
  size_t pruned_data = 0;
};

// What inventory_step() found: the horizon a pass reads. `rebuilt`: built and
// published by this step, so nothing destructive may use it in this pass, and
// the caller schedules a pass after its quiet window that will. `held_back`: it
// is behind, and the caller said not to rebuild it yet.
struct InventoryStep {
  contract::ObjectLedger::InventoryHandle inventory;
  bool rebuilt = false;
  bool held_back = false;
};

struct TombstoneStep {
  contract::GateVerdict gate;
};

// What release_step() found. `head`: the sole accepted head, none while there
// are several or none. `built`: a head the ledger did not hold was asked for,
// and `complete` says whether what it answered was published. `release`: the
// horizon the sweeps read, the previous one when the build was refused.
struct ReleaseStep {
  std::optional<contract::Hash256> head;
  bool built = false;
  bool complete = false;
  contract::ObjectLedger::ReleaseHandle release;
};

struct ControlStep {
  contract::GateVerdict gate;
  size_t released = 0;
  size_t removed = 0;
  size_t pruned = 0;
};

// `orphan_grace`: how long DATA no reference has reached waits, so a caller
// knows when a sweep that deferred one is worth repeating.
struct DataStep {
  contract::GateVerdict gate;
  size_t released = 0;
  store::StoragePool::MaintenanceResult swept;
  size_t pruned = 0;
  std::chrono::milliseconds orphan_grace{0};
};

// Garbage collection on one node (requirements L17 to L22): the inventory
// and release horizons built from the owner's ReachabilityProvider and
// published to the ledger, then the three destructive gates and what each
// lets through, in Macha's order (service/maintenance.cpp:620-660,
// 866-1071). Tombstone collection hands its decisions to the owner. The
// CONTROL sweep releases CONTROL claims, removes unreferenced CONTROL objects
// (Macha's CatalogueManager::control_gc_step, moved here, its root epoch the
// provider's CONTROL epoch) and prunes; the DATA sweep releases DATA claims,
// runs the pool's sweep and prunes.
//
// What it keeps, as Macha does: an inventory is never used destructively in
// the pass that built it; an incomplete release is never published; claims
// go only as the release head's clock dominates them; an object goes only
// once this node has seen it unreferenced and unclaimed for the grace on its
// own clock, and a copy with any local claim is untouchable, checked again
// just before the delete; deletion is the store's, lazily durable.
//
// The pass is five steps, public so that an owner's own pass can put its other
// stages between them in its own order, as Macha's does (repair runs between
// inventory_step() and tombstone_step()): inventory_step(), tombstone_step(),
// release_step(), control_step(), data_step(). Each traces what it decides and
// returns it. No step looks at the owner's stop: only `pause`, inside the
// inventory's walk, may end one early with MaintenanceStopping, and no step
// catches that or anything the provider throws, so what ends the pass and what
// the rest of it does then is the caller's. A step whose turn is not due runs
// its gate, traces it shut and does nothing else.
//
// Single owner: one thread runs every pass and every step. The references
// outlive it.
class Collector {
 public:
  // The decision trace: `kind` names what was decided (a gate, a horizon, a
  // sweep's work) and `detail` how, deterministically. Macha's kinds and
  // words.
  using Trace = std::function<void(std::string_view kind, std::string_view detail)>;

  Collector(contract::ObjectLedger& ledger, contract::ReachabilityProvider& provider, store::StoragePool& data, store::LocalStore& control,
            UnreferencedSince& data_sightings, UnreferencedSince& control_sightings, CollectorOptions options, CollectorServices services,
            store::PersistentBlockCache* cache = nullptr, Trace trace = {});

  // One pass. Nothing happens unless tombstone collection or GC is due.
  // Throws what the provider throws for an inventory it cannot answer,
  // having removed nothing in this pass.
  static constexpr dannan::sched::Waits pass_waits = contract::ReachabilityProvider::answer_waits | dannan::sched::Waits::data_device;
  PassReport pass(const PassDue& due);

  // The horizon a pass reads, rebuilt and published when the provider's
  // generation has moved or it is incomplete. `may_rebuild` false holds a stale
  // one back (Macha's repair-only pass while its metadata is not ready), and
  // never the first. Throws what the provider throws, and what `pause` throws.
  static constexpr dannan::sched::ThreadSafety step_safety = dannan::sched::ThreadSafety::single_owner;
  static constexpr dannan::sched::Waits inventory_step_waits = contract::ReachabilityProvider::answer_waits;
  InventoryStep inventory_step(const std::function<void()>& pause = {}, bool may_rebuild = true);

  // The tombstone gate and, open, what it decides, handed to the provider.
  // Throws what the provider's collect_garbage throws.
  static constexpr dannan::sched::Waits tombstone_step_waits = contract::ReachabilityProvider::answer_waits;
  TombstoneStep tombstone_step(const PassDue& due, const InventoryStep& inventory);

  // The release horizon, built and published when the sole accepted head moved.
  // A head the provider cannot answer is an incomplete build, not a throw.
  static constexpr dannan::sched::Waits release_step_waits = contract::ReachabilityProvider::answer_waits;
  ReleaseStep release_step();

  // The CONTROL gate and, open, its release, sweep and prune.
  static constexpr dannan::sched::Waits control_step_waits = dannan::sched::local_waits;
  ControlStep control_step(const PassDue& due, const InventoryStep& inventory, const ReleaseStep& release);

  // The DATA gate and, open, its release, sweep and prune. `due.should_yield`
  // is asked between objects.
  static constexpr dannan::sched::Waits data_step_waits = dannan::sched::local_waits | dannan::sched::Waits::data_device;
  DataStep data_step(const PassDue& due, const InventoryStep& inventory, const ReleaseStep& release);

  // The CONTROL sweep's step: up to `budget` objects of the CONTROL store
  // from its cursor. An object `live` refers to stays, and so does a claimed
  // one; an unreferenced one goes only once a CONTROL epoch later than the
  // one it was first seen unreferenced in stands, it has not been touched
  // since that epoch began, it is older than `grace` and, given `sightings`,
  // this node has seen it so for `grace` at `now_unix_ms`. Returns the
  // objects removed.
  size_t control_gc_step(const contract::IdLookup& live, std::chrono::milliseconds grace, size_t budget, UnreferencedSince* sightings, uint64_t now_unix_ms);

 private:
  contract::PassFacts facts(const PassDue& due, const InventoryStep& inventory) const;
  void trace(std::string_view kind, const std::string& detail) const;
  void trace_gate(std::string_view gate, const contract::GateVerdict& verdict) const;
  void save_sightings(UnreferencedSince&, dannan::contract::TimePoint& saved, bool complete);
  void collect_tombstones(const contract::InventoryHorizon&);

  contract::ObjectLedger& _ledger;
  contract::ReachabilityProvider& _provider;
  store::StoragePool& _data;
  store::LocalStore& _control;
  UnreferencedSince& _data_sightings;
  UnreferencedSince& _control_sightings;
  const CollectorOptions _options;
  const CollectorServices _services;
  store::PersistentBlockCache* const _cache;
  const Trace _trace;
  NodeHorizonBuilder _builder;
  dannan::contract::TimePoint _data_saved{};
  dannan::contract::TimePoint _control_saved{};
  store::LocalStore::Cursor _control_cursor;
  // The CONTROL epoch each object was first seen unreferenced in.
  std::map<ObjectId, uint64_t> _unreferenced_epoch;
  std::string _last_skip_reason;
  bool _release_incomplete_logged = false;
};

}  // namespace dagda::ledger
