//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/node.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>

#include "dagda/context.h"
#include "dagda/ledger/node_horizon_builder.h"
#include "dagda/store/messages.h"
#include "dagda/store/sealing.h"
#include "dagda/store/state_layout.h"

namespace dagda {

namespace {

using store::ledger_cache_share;
using store::LedgerShare;

constexpr const char* plane_data = "data_storage";
constexpr const char* plane_control = "control_storage";
constexpr const char* plane_cache = "cache";
constexpr const char* plane_retention = "retention";

uint64_t u64(uint64_t value) { return value; }

const char* gate_name(store::RepairGate gate) noexcept {
  switch (gate) {
    case store::RepairGate::ran:
      return "ran";
    case store::RepairGate::share:
      return "share";
    case store::RepairGate::quiescent:
      return "quiescent";
    case store::RepairGate::credit:
      return "credit";
  }
  return "unknown";
}

// The readiness planes the node reports, Macha's names: the DATA pool, the
// CONTROL store, the cache, and the claims with the reference counts (heads
// add "metadata" at step 4).
std::vector<std::string> readiness_planes() { return {plane_data, plane_control, plane_cache, plane_retention}; }

}  // namespace

// The status timer's hold on the node, so a timer that fires as the node
// stops finds nothing.
struct Node::StatusTimer {
  dannan::contract::Mutex mutex;
  Node* node DANNAN_GUARDED_BY(mutex) = nullptr;
};

Node::Node(dannan::Context& context, NodeOptions options) : _context(context), _options(std::move(options)), _logger(context.logger("dagda.node")) {
  try {
    open();
  } catch (const std::exception& error) {
    const std::string why = error.what();
    _logger.error("node refused: " + why);
    _context.readiness().fail("dagda: " + why);
    stop();
    if (dynamic_cast<const NodeRefused*>(&error)) throw;
    throw NodeRefused(why);
  }
}

Node::~Node() { stop(); }

void Node::step(std::string_view what) {
  _logger.info(std::string(what));
  if (_options.trace) _options.trace(what);
}

void Node::ready(const std::string& plane) { _context.readiness().ready(plane); }

void Node::open() {
  if (_context.version() != dannan::context_version)
    throw NodeRefused("the Dannan node serves dannan::Context version " + std::to_string(_context.version()) + " and Dagda was built against version " +
                      std::to_string(dannan::context_version));
  if (_options.state.empty()) throw NodeRefused("no state path: the node's state is a directory of its own");
  // Every plane the node will report is known before the Dannan node starts,
  // so it never reports ready without them.
  try {
    for (const auto& plane : readiness_planes()) _context.readiness().declare(plane);
  } catch (const std::logic_error&) {
    throw NodeRefused("the Dannan node has started: a dagda::Node opens before the Dannan node starts");
  }
  if (_options.domain.role != domain::Role::member)
    throw NodeRefused("a blind node's store waits on the catalogue (step 4): domain " + domain::to_string(_options.domain.id) + " is " +
                      domain::role_name(_options.domain.role) + " here");
  if (!_options.key) throw NodeRefused("no key for domain " + domain::to_string(_options.domain.id) + ", of which this node is a member");
  const auto scheme = store::SealingScheme::v19(_options.domain.id);

  // Before the node starts, so a hosted system cannot forget it (C5).
  step("open message-types");
  try {
    store::message::register_types(_context.routes());
  } catch (const std::invalid_argument& error) {
    throw NodeRefused(std::string("Dagda's message types are registered already, so a store runs on this Dannan node: ") + error.what());
  } catch (const std::logic_error& error) {
    throw NodeRefused(std::string("Dagda's message types are registered before the Dannan node starts: ") + error.what());
  }

  // The state first: its lock, then nothing else on it until its layout is
  // accepted. A v18 backend is refused here too, before anything is written.
  step("open state-layout");
  const auto lock = [&](const std::filesystem::path& root) {
    try {
      _locks.push_back(std::make_unique<store::StorageLock>(root));
    } catch (const std::exception& error) {
      throw NodeRefused(root.string() + " is in use (another node, or dagda-migrate, holds it): " + error.what());
    }
  };
  lock(_options.state);
  for (const auto& backend : _options.pool.backends)
    if (const auto why = store::backend_refusal_v18(_options.state, backend.path)) throw NodeRefused(*why);
  try {
    store::open_state_layout(_options.state, scheme);
  } catch (const std::exception& error) {
    throw NodeRefused(error.what());
  }
  for (const auto& backend : _options.pool.backends) lock(backend.path);
  lock(store::control_store_path(_options.state, _options.control));
  if (!_options.cache.path.empty()) lock(_options.cache.path);

  step("open node-local-key");
  try {
    _node_local = std::make_shared<const domain::StorageKey>(domain::StorageKey::require_node_local(_context.identity()));
  } catch (const std::exception& error) {
    throw NodeRefused(error.what());
  }
  _wrap = domain::WrapKey::derive(_context.identity());
  if (!_wrap) throw NodeRefused("this node has no key custody, so no wrap key: a store needs custody");
  domain::advertise_wrap_key(_context, *_wrap);

  const auto services = [&](std::string scope) {
    return store::LocalStore::Services{_context.threads(),       _context.observations(), _progress, _context.logger(std::move(scope)),
                                       _context.time().source(), *_options.wall};
  };
  const auto cache_bytes = _options.ledger_cache_bytes;
  const auto& self = _context.identity();

  step("open storage-pool");
  auto pool = _options.pool;
  pool.sealing = scheme;
  pool.role = _options.domain.role;
  pool.ledger_key = _node_local;
  pool.ledger_cache_bytes = ledger_cache_share(cache_bytes, LedgerShare::data_held);
  _data = std::make_unique<store::StoragePool>(_options.state, self.id(), self.durability_epoch(), pool, _options.key, services("dagda.pool"));
  // A pool whose every backend is offline holds nothing it should: it says
  // so rather than report itself ready.
  if (_data->degraded())
    _logger.warn("data storage degraded: every configured backend is offline");
  else
    ready(plane_data);

  if (_options.admission) {
    step("open admission");
    const auto& admission = *_options.admission;
    const auto quiet = _options.repair.foreground_quiet;
    auto& activity = _context.activity();
    _credit = std::make_unique<store::DataResourceArbiter>(
        admission.data_inflight_bytes, admission.foreground_reserve_bytes, admission.background_concurrency, admission.no_progress,
        [&activity, quiet] { return activity.recently_active(dannan::sched::WorkClass::foreground, quiet); }, _context.logger("dagda.credit"),
        _context.time().source());
    _credit->observe_device(&_data->service_monitor(), admission.min_background_under_pressure);
    _io = std::make_unique<store::IoReserve>(admission.io, _context.time().source());
  }

  step("open control-store");
  auto control = _options.control;
  control.sealing = scheme;
  _control = store::open_control_store(_options.state, control, _options.key, _node_local, services("dagda.control"),
                                       ledger_cache_share(cache_bytes, LedgerShare::control_held));
  ready(plane_control);

  step("open cache");
  auto cache = _options.cache;
  cache.sealing = scheme;
  _cache = std::make_unique<store::PersistentBlockCache>(cache, _options.key, services("dagda.cache"));
  ready(plane_cache);

  step("open claims");
  _claims = std::make_unique<ledger::RetentionStore>(_options.state, _node_local, _context.logger("dagda.claims"),
                                                     ledger_cache_share(cache_bytes, LedgerShare::claims));
  _ledger = std::make_unique<ledger::RetentionLedger>(*_claims, *_data, *_control, _context.time().source());

  step("open reference-counts");
  _references = std::make_unique<ledger::ReferenceCounts>(_options.state / "ledger" / "referenced", _node_local,
                                                          ledger_cache_share(cache_bytes, LedgerShare::references), _context.logger("dagda.references"));
  ready(plane_retention);

  step("open distributed-store");
  auto store_options = _options.store;
  store_options.domain = _options.domain;
  _store = std::make_unique<store::DistributedStore>(
      _context, store::DistributedStore::Stores{*_data, *_control, *_cache, *_claims, _credit.get(), _io.get(), _ledger.get()}, store_options);

  step("open availability");
  _availability = std::make_unique<store::AvailabilityService>(_context, *_ledger, _options.availability);

  step("open repair");
  auto repair = _options.repair;
  repair.extent_size = store_options.extent_size;
  _repair = std::make_unique<store::Repair>(_context, *_store, *_availability, *_ledger, repair);

  step("open storage-server");
  auto server = _options.server;
  server.extent_size = store_options.extent_size;
  server.domain = _options.domain;
  _server = std::make_unique<store::StorageServer>(_context, store::StorageServer::Stores{*_data, *_control, *_claims, _cache.get(), _ledger.get()}, server,
                                                   store::StorageServerAdmission{_credit.get(), _io.get()});
  if (_options.front) {
    // The server outlives its routes: its destructor unbinds them, waiting
    // for the calls in flight, before it goes.
    auto* serving = _server.get();
    for (const auto type : store::StorageServer::served())
      _context.routes().bind(type,
                             [this, serving](const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame, const dannan::net::Message& request) {
                               if (auto answer = _options.front(peer, frame, request)) return std::move(*answer);
                               return serving->handle(peer, frame, request);
                             });
  }

  _membership = std::make_unique<lease::RosterMembership>(_context.nodes(), _context.time().source());
  _keeper = std::make_unique<lease::LeaseKeeper>(*_membership, _context.time().source(), *_options.wall, _options.leases);
  _api = std::make_unique<Context>(*this);

  _status = std::make_shared<StatusTimer>();
  {
    dannan::contract::Lock held(_status->mutex);
    _status->node = this;
  }
  // The collector's sightings, Macha's files (requirement L22); the thread
  // last, once everything it reaches is open.
  step("open maintenance");
  {
    dannan::contract::Lock held(_maintenance_mutex);
    _data_sightings =
        std::make_unique<ledger::UnreferencedSince>(_options.state / "retention" / "unreferenced-data.bin", _node_local, _context.logger("dagda.gc"));
    _control_sightings =
        std::make_unique<ledger::UnreferencedSince>(_options.state / "retention" / "unreferenced-control.bin", _node_local, _context.logger("dagda.gc"));
  }
  publish_status();
  arm_status();
  if (_options.maintenance.automatic)
    _maintenance = std::jthread([this](std::stop_token stop) { _context.threads().loop("dagda.maintenance", stop, [this, stop] { maintenance_loop(stop); }); });
  _logger.info("node open: domain " + _options.domain.name + " (" + domain::to_string(_options.domain.id) + "), " + std::to_string(_data->online_backends()) +
               " backends online, used " + std::to_string(_data->used()) + " of " + std::to_string(_data->limit()));
}

void Node::stop() {
  // The maintenance thread first: nothing it reaches has gone yet.
  stop_maintenance();
  {
    dannan::contract::Lock lock(_maintenance_mutex);
    _collector.reset();
    _collector_provider = nullptr;
    if (_data_sightings || _control_sightings) step("stop maintenance");
    _data_sightings.reset();
    _control_sightings.reset();
  }
  if (_status) {
    dannan::contract::Lock lock(_status->mutex);
    _status->node = nullptr;
  }
  _status.reset();
  // Admission first: a request waiting for it gives up rather than hold the
  // server's unbinding.
  if (_credit) _credit->stop();
  if (_io) _io->stop();
  const auto close = [&](std::string_view what, auto& part) {
    if (!part) return;
    part.reset();
    step("stop " + std::string(what));
  };
  _api.reset();
  _keeper.reset();
  _membership.reset();
  close("storage-server", _server);
  close("repair", _repair);
  close("availability", _availability);
  close("distributed-store", _store);
  close("reference-counts", _references);
  _ledger.reset();
  close("claims", _claims);
  close("cache", _cache);
  close("control-store", _control);
  _io.reset();
  close("admission", _credit);
  close("storage-pool", _data);
  _node_local.reset();
  close("node-local-key", _wrap);
  if (!_locks.empty()) {
    _locks.clear();
    step("stop state-layout");
  }
}

void Node::arm_status() {
  std::weak_ptr<StatusTimer> weak = _status;
  _context.time().after(_options.status_interval, [weak] {
    const auto timer = weak.lock();
    if (!timer) return;
    dannan::contract::Lock lock(timer->mutex);
    if (!timer->node) return;
    timer->node->publish_status();
    timer->node->arm_status();
  });
}

void Node::register_reachability(const domain::DomainId& domain, contract::ReachabilityProvider& provider) {
  if (domain != _options.domain.id) throw std::invalid_argument("this node does not hold domain " + domain::to_string(domain));
  // After a pass in progress; the collector is made again for this provider.
  dannan::contract::Lock maintenance(_maintenance_mutex);
  _collector.reset();
  _collector_provider = nullptr;
  dannan::contract::Lock lock(_providers_mutex);
  _providers[domain] = &provider;
}

void Node::unregister_reachability(const domain::DomainId& domain) {
  dannan::contract::Lock maintenance(_maintenance_mutex);
  _collector.reset();
  _collector_provider = nullptr;
  dannan::contract::Lock lock(_providers_mutex);
  _providers.erase(domain);
}

bool Node::publish_inventory() {
  dannan::contract::Lock maintenance(_maintenance_mutex);
  contract::ReachabilityProvider* provider = nullptr;
  {
    dannan::contract::Lock lock(_providers_mutex);
    const auto found = _providers.find(_options.domain.id);
    if (found != _providers.end()) provider = found->second;
  }
  if (!provider) return false;
  ledger::NodeHorizonBuilder builder(*provider, _context.logger("dagda.horizon"));
  _ledger->publish(builder.inventory());
  return true;
}

// ---- Maintenance ------------------------------------------------------------

bool Node::higher_class_active() const {
  auto& activity = _context.activity();
  const auto quiet = _options.repair.foreground_quiet;
  return activity.recently_active(dannan::sched::WorkClass::foreground, quiet) || activity.recently_active(dannan::sched::WorkClass::loader, quiet);
}

bool Node::gc_pass_locked(bool forced, MaintenanceReport& report) {
  contract::ReachabilityProvider* provider = nullptr;
  {
    dannan::contract::Lock lock(_providers_mutex);
    const auto found = _providers.find(_options.domain.id);
    if (found != _providers.end()) provider = found->second;
  }
  const auto note = [&](const std::function<void(GcFigures&)>& change) {
    dannan::contract::Lock lock(_gc_figures_mutex);
    change(_gc_figures);
  };
  if (!provider) {
    // Nothing destructive without the owner's word on what is referenced.
    _collector.reset();
    _collector_provider = nullptr;
    report.gc_skipped = "no reachability provider registered for domain " + domain::to_string(_options.domain.id) + ": the collector removes nothing";
    note([&](GcFigures& figures) {
      ++figures.skipped;
      figures.state = report.gc_skipped;
    });
    return true;
  }
  if (!_collector || _collector_provider != provider) {
    ledger::CollectorOptions options;
    static_cast<ledger::CollectorBudgets&>(options) = _options.maintenance.collector;
    options.garbage_grace = _options.garbage_grace;
    _collector = std::make_unique<ledger::Collector>(
        *_ledger, *provider, *_data, *_control, *_data_sightings, *_control_sightings, options,
        ledger::CollectorServices{_context.logger("dagda.gc"), _context.observations(), _context.time().source(), *_options.wall}, _cache.get());
    _collector_provider = provider;
  }
  const bool busy = !forced && higher_class_active();
  ledger::PassDue due;
  due.garbage_due = !busy;
  due.gc_due = !busy;
  due.busy = busy;
  if (!forced) due.should_yield = [this] { return higher_class_active(); };
  try {
    report.gc = _collector->pass(due);
  } catch (const std::exception& error) {
    report.gc_skipped = std::string("the reachability provider could not answer: ") + error.what();
    _logger.debug("garbage collection: " + report.gc_skipped);
    note([&](GcFigures& figures) {
      ++figures.failures;
      figures.state = report.gc_skipped;
    });
    return false;
  }
  const auto& pass = *report.gc;
  note([&](GcFigures& figures) {
    ++figures.passes;
    figures.inventories += pass.rebuilt_inventory ? 1 : 0;
    figures.released_data += pass.released_data;
    figures.released_control += pass.released_control;
    figures.removed_control += pass.removed_control;
    figures.examined += pass.swept_data.objects;
    figures.reclaimed_bytes += pass.swept_data.bytes;
    figures.pruned += pass.pruned_data + pass.pruned_control;
    figures.state = busy ? "busy: foreground or loader work present" : "ran";
    figures.data_gate = !pass.data ? "not reached" : pass.data->permitted ? "open" : pass.data->reason;
  });
  // Settled: the sweep went all the way round with nothing left waiting.
  return !busy && !pass.rebuilt_inventory && pass.data && pass.data->permitted && pass.swept_data.complete && !pass.swept_data.deferred &&
         !pass.swept_data.yielded;
}

MaintenanceReport Node::run_maintenance_once() {
  MaintenanceReport report;
  dannan::contract::Lock lock(_maintenance_mutex);
  if (!_repair) return report;
  (void)gc_pass_locked(true, report);
  try {
    report.repair = _repair->pass();
  } catch (const std::exception& error) {
    _logger.debug(std::string("repair pass: ") + error.what());
  }
  return report;
}

void Node::maintenance_loop(std::stop_token stop) {
  const auto& time = _context.time().source();
  const auto interval = std::max(_options.maintenance.interval, std::chrono::milliseconds(1));
  const auto gc_interval = std::max(_options.maintenance.gc_interval, interval);
  auto next_repair = time.now();
  auto next_gc = next_repair;
  while (!stop.stop_requested()) {
    const auto now = time.now();
    {
      dannan::contract::Lock lock(_maintenance_mutex);
      if (now >= next_gc) {
        MaintenanceReport report;
        next_gc = now + (gc_pass_locked(false, report) ? gc_interval : interval);
      }
      if (now >= next_repair) {
        try {
          (void)_repair->pass();
        } catch (const std::exception& error) {
          _logger.debug(std::string("repair pass: ") + error.what());
        }
        next_repair = now + interval;
      }
    }
    // Waits in real time for at most an interval, then reads the node's time
    // again: a stepped clock moved meanwhile is seen within an interval.
    const auto after = time.now();
    const auto due = std::min(next_gc, next_repair);
    if (due <= after) continue;
    const auto wait = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(due - after) + std::chrono::milliseconds(1), interval);
    std::unique_lock lock(_maintenance_wait);
    _maintenance_wake.wait_for(lock, stop, wait, [] { return false; });
  }
}

void Node::stop_maintenance() {
  if (!_maintenance.joinable()) return;
  _maintenance.request_stop();
  _maintenance_wake.notify_all();
  _maintenance.join();
}

// Every figure is under "dagda.":
//   context_version; domain, domain.name, domain.role;
//   storage.used, storage.limit, storage.backends_online, storage.degraded,
//   storage.barriers (the DATA pool); control.used, control.limit;
//   cache.enabled, cache.blocks, cache.hits, cache.misses, cache.evictions;
//   references.entries, references.extents;
//   repair.passes, repair.copies_pushed, repair.copies_pulled,
//   repair.local_drops, repair.bytes, repair.unsourceable,
//   repair.local_unreadable; repair.gate.ran, repair.gate.share,
//   repair.gate.quiescent, repair.gate.credit, repair.gate.last;
//   maintenance.automatic; gc.provider, gc.grace_ms, gc.passes, gc.skipped
//   (no provider), gc.failures (the provider could not answer),
//   gc.inventories, gc.released_data, gc.released_control,
//   gc.removed_control, gc.examined, gc.reclaimed_bytes, gc.pruned, gc.state
//   (what the last pass came to, in words), gc.data_gate (open, or why
//   shut);
//   prompt.queued, prompt.copies, prompt.failures, prompt.skipped_no_room,
//   prompt.dropped;
//   and with admission: admission.credit_used, admission.device_pressured,
//   admission.pressure_refusals, admission.data_in_flight,
//   admission.control_in_flight.
void Node::publish_status() {
  auto& surface = _context.status();
  const auto put = [&](const std::string& name, dannan::status::Surface::Value value) { surface.publish("dagda." + name, std::move(value)); };
  put("context_version", u64(context_version));
  put("domain", domain::to_string(_options.domain.id));
  put("domain.name", _options.domain.name);
  put("domain.role", std::string(domain::role_name(_options.domain.role)));

  put("storage.used", u64(_data->used()));
  put("storage.limit", u64(_data->limit()));
  put("storage.backends_online", u64(_data->online_backends()));
  put("storage.degraded", _data->degraded());
  put("storage.barriers", u64(_data->physical_barriers()));
  put("control.used", u64(_control->used()));
  put("control.limit", u64(_control->limit()));

  const auto cache = _cache->stats();
  put("cache.enabled", _cache->enabled());
  put("cache.blocks", u64(_cache->blocks()));
  put("cache.hits", u64(cache.hits));
  put("cache.misses", u64(cache.misses));
  put("cache.evictions", u64(cache.evictions));

  const auto totals = _references->totals();
  put("references.entries", u64(totals.entries));
  put("references.extents", u64(totals.extent_count));

  const auto repair = _store->repair_diagnostics();
  put("repair.passes", u64(repair.passes_completed));
  put("repair.copies_pushed", u64(repair.copies_pushed));
  put("repair.copies_pulled", u64(repair.copies_pulled));
  put("repair.local_drops", u64(repair.local_drops));
  put("repair.bytes", u64(repair.bytes_transferred));
  put("repair.unsourceable", u64(repair.pull_unsourceable));
  put("repair.local_unreadable", u64(repair.local_unreadable));
  const auto gates = _repair->gates();
  put("repair.gate.ran", u64(gates.ran));
  put("repair.gate.share", u64(gates.share));
  put("repair.gate.quiescent", u64(gates.quiescent));
  put("repair.gate.credit", u64(gates.credit));
  put("repair.gate.last", std::string(gates.last ? gate_name(*gates.last) : "none"));

  bool provider = false;
  {
    dannan::contract::Lock lock(_providers_mutex);
    provider = _providers.contains(_options.domain.id);
  }
  GcFigures gc;
  {
    dannan::contract::Lock lock(_gc_figures_mutex);
    gc = _gc_figures;
  }
  put("maintenance.automatic", _options.maintenance.automatic);
  put("gc.provider", provider);
  put("gc.grace_ms", u64(static_cast<uint64_t>(_options.garbage_grace.count())));
  put("gc.passes", u64(gc.passes));
  put("gc.skipped", u64(gc.skipped));
  put("gc.failures", u64(gc.failures));
  put("gc.inventories", u64(gc.inventories));
  put("gc.released_data", u64(gc.released_data));
  put("gc.released_control", u64(gc.released_control));
  put("gc.removed_control", u64(gc.removed_control));
  put("gc.examined", u64(gc.examined));
  put("gc.reclaimed_bytes", u64(gc.reclaimed_bytes));
  put("gc.pruned", u64(gc.pruned));
  put("gc.state", provider || gc.state != "not yet run"
                      ? gc.state
                      : "no reachability provider registered for domain " + domain::to_string(_options.domain.id) + ": the collector removes nothing");
  put("gc.data_gate", gc.data_gate);

  const auto prompt = _store->prompt_replication_stats();
  put("prompt.queued", u64(prompt.queued));
  put("prompt.copies", u64(prompt.copies));
  put("prompt.failures", u64(prompt.failures));
  put("prompt.skipped_no_room", u64(prompt.skipped_no_room));
  put("prompt.dropped", u64(prompt.dropped));

  if (_credit) {
    const auto credit = _credit->stats();
    put("admission.credit_used", u64(credit.used_bytes));
    put("admission.device_pressured", credit.device_pressured);
    put("admission.pressure_refusals", u64(credit.pressure_refusals));
  }
  if (_io) {
    const auto io = _io->stats();
    put("admission.data_in_flight", u64(io.data_in_flight));
    put("admission.control_in_flight", u64(io.control_in_flight));
  }
}

}  // namespace dagda
