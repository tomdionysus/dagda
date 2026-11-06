//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/node.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>

#include "dagda/context.h"
#include "dagda/contract/stopping.h"
#include "dagda/ledger/node_horizon_builder.h"
#include "dagda/store/messages.h"
#include "dagda/store/sealing.h"
#include "dagda/store/slow_stage.h"
#include "dagda/store/state_layout.h"
#include "dannan/identity/durable_file.h"

namespace dagda {

namespace {

using store::ledger_cache_share;
using store::LedgerShare;

constexpr const char* plane_data = "data_storage";
constexpr const char* plane_control = "control_storage";
constexpr const char* plane_cache = "cache";
constexpr const char* plane_retention = "retention";
constexpr const char* plane_metadata = "metadata";

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
// CONTROL store, the cache, the claims with the reference counts, and the heads
// when the node runs them.
std::vector<std::string> readiness_planes(bool heads) {
  std::vector<std::string> planes{plane_data, plane_control, plane_cache, plane_retention};
  if (heads) planes.emplace_back(plane_metadata);
  return planes;
}

void throw_if_stopped(const LocalTurn& turn) {
  if (turn.stop.stop_requested()) throw contract::MaintenanceStopping{};
}

}  // namespace

// The status timer's hold on the node, so a timer that fires as the node
// stops finds nothing.
struct Node::StatusTimer {
  dannan::contract::Mutex mutex;
  Node* node DANNAN_GUARDED_BY(mutex) = nullptr;
};

Node::Node(dannan::Context& context, NodeOptions options) : Node(context, std::move(options), OpenLater{}) { open_store(); }

Node::Node(dannan::Context& context, NodeOptions options, OpenLater) : _context(context), _options(std::move(options)), _logger(context.logger("dagda.node")) {
  refusing([this] { prepare(); });
}

void Node::open_store() {
  if (_server) throw std::logic_error("the store is open already");
  refusing([this] { open(); });
}

void Node::refusing(const std::function<void()>& part) {
  try {
    part();
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

void Node::prepare() {
  if (_context.version() != dannan::context_version)
    throw NodeRefused("the Dannan node serves dannan::Context version " + std::to_string(_context.version()) + " and Dagda was built against version " +
                      std::to_string(dannan::context_version));
  if (_options.state.empty()) throw NodeRefused("no state path: the node's state is a directory of its own");
  _storage_event = _context.events().kind("storage");
  _metadata_event = _context.events().kind("metadata");
  _claims_event = _context.events().kind("claims");
  // Every plane the node will report is known before the Dannan node starts,
  // so it never reports ready without them.
  try {
    for (const auto& plane : readiness_planes(_options.heads)) _context.readiness().declare(plane);
  } catch (const std::logic_error&) {
    throw NodeRefused("the Dannan node has started: a dagda::Node opens before the Dannan node starts");
  }
  if (_options.domain.role != domain::Role::member)
    throw NodeRefused("a blind node's store waits on the catalogue (step 4): domain " + domain::to_string(_options.domain.id) + " is " +
                      domain::role_name(_options.domain.role) + " here");
  if (!_options.key) throw NodeRefused("no key for domain " + domain::to_string(_options.domain.id) + ", of which this node is a member");

  // Before the node starts, so a hosted system cannot forget it (C5).
  step("open message-types");
  try {
    store::message::register_types(_context.routes());
    if (_options.heads) heads::message::register_types(_context.routes(), _options.exchange.numbers);
  } catch (const std::invalid_argument& error) {
    throw NodeRefused(std::string("Dagda's message types are registered already, so a store runs on this Dannan node: ") + error.what());
  } catch (const std::logic_error& error) {
    throw NodeRefused(std::string("Dagda's message types are registered before the Dannan node starts: ") + error.what());
  }
}

void Node::open() {
  const auto scheme = store::SealingScheme::v19(_options.domain.id);

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
  const auto lock_reloadable = [&](const std::filesystem::path& root) {
    try {
      _reloadable_locks.emplace(root, std::make_unique<store::StorageLock>(root));
    } catch (const std::exception& error) {
      throw NodeRefused(root.string() + " is in use (another node, or dagda-migrate, holds it): " + error.what());
    }
  };
  for (const auto& backend : _options.pool.backends) lock_reloadable(backend.path);
  lock(store::control_store_path(_options.state, _options.control));
  if (!_options.cache.path.empty()) lock_reloadable(_options.cache.path);

  step("open node-local-key");
  try {
    _node_local = std::make_shared<const domain::StorageKey>(domain::StorageKey::require_node_local(_context.identity()));
  } catch (const std::exception& error) {
    throw NodeRefused(error.what());
  }
  _wrap = domain::WrapKey::derive(_context.identity());
  if (!_wrap) throw NodeRefused("this node has no key custody, so no wrap key: a store needs custody");
  // The advert is signed by the certificate's key, so a node with none (Macha's,
  // under its cluster key) publishes none. A peer reads that as "no wrap advert".
  if (_context.identity().certificate()) domain::advertise_wrap_key(_context, *_wrap);

  const auto services = [&](std::string scope) {
    auto& progress = _options.startup_progress ? *_options.startup_progress : _progress;
    return store::LocalStore::Services{_context.threads(),       _context.observations(), progress, _context.logger(std::move(scope)),
                                       _context.time().source(), *_options.wall};
  };
  const auto cache_bytes = _options.ledger_cache_bytes;
  const auto& self = _context.identity();

  _activity = std::make_unique<dannan::status::ActivityClocks>(&pass_time());

  step("open storage-pool");
  auto pool = _options.pool;
  pool.sealing = scheme;
  pool.role = _options.domain.role;
  pool.ledger_key = _node_local;
  pool.ledger_cache_bytes = ledger_cache_share(cache_bytes, LedgerShare::data_held);
  if (_options.admission && _options.admission->device_pressure.slowdown_percent) pool.service_thresholds = _options.admission->device_pressure;
  _data = std::make_unique<store::StoragePool>(_options.state, self.id(), self.durability_epoch(), pool, _options.key, services("dagda.pool"));
  // A pool whose every backend is offline holds nothing it should: ready, but
  // degraded, and it says so (Tom, 2026-11-05), so a node whose only disk is
  // offline does not crash-loop on its startup timeout.
  if (_data->degraded()) _logger.warn("data storage degraded: every configured backend is offline");
  ready(plane_data);

  if (_options.admission) {
    step("open admission");
    const auto& admission = *_options.admission;
    const auto quiet = _options.repair.foreground_quiet;
    auto& activity = *_activity;
    _credit = std::make_unique<store::DataResourceArbiter>(
        admission.data_inflight_bytes, admission.foreground_reserve_bytes, admission.background_concurrency, admission.no_progress,
        [&activity, quiet] { return activity.recently_active(dannan::sched::WorkClass::foreground, quiet); }, _context.logger("dagda.credit"),
        _context.time().source());
    // Device-pressure admission; a zero target turns it off, as Macha's does.
    if (admission.device_pressure.slowdown_percent) _credit->observe_device(&_data->service_monitor(), admission.min_background_under_pressure);
    if (admission.device_reserve) _io = std::make_unique<store::IoReserve>(admission.io, _context.time().source());
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
  if (!store_options.repair_trace) store_options.repair_trace = _options.maintenance.trace;
  _store = std::make_unique<store::DistributedStore>(
      _context, store::DistributedStore::Stores{*_data, *_control, *_cache, *_claims, _credit.get(), _io.get(), _ledger.get(), _activity.get()}, store_options);

  if (_options.heads) {
    // A space replays with the owner's codec, so its plane cannot be marked until it
    // has: no peer is asked unless a node of the tree is missing here.
    step("open heads");
    auto exchange = _options.exchange;
    // Both hear of a stage: the owner's pass and the exchange's own.
    exchange.stage = [this, own = exchange.stage](std::string_view stage) {
      if (own) own(stage);
      enter_stage(stage);
    };
    decltype(heads::Spaces::Services::front) front;
    if (_options.front)
      front = [this](const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame, const dannan::net::Message& request) {
        return _options.front(peer, frame, request);
      };
    _spaces = std::make_unique<heads::Spaces>(heads::Spaces::Services{_context, *_control, *_store, *_cache, _options.key, _node_local, _options.domain.id,
                                                                      _options.state, _options.wall, std::move(exchange), _options.garbage_grace, &pass_time(),
                                                                      std::move(front), [this] { wake_maintenance(); }, _metadata_event, _claims_event});
    for (auto spec : _options.spaces) {
      const auto name = spec.name;
      try {
        _spaces->open(std::move(spec));
      } catch (const std::exception& error) {
        throw NodeRefused("head space " + name + " cannot open: " + error.what());
      }
      (_options.startup_progress ? *_options.startup_progress : _progress).note();
    }
    ready(plane_metadata);
  }

  step("open availability");
  auto availability = _options.availability;
  if (!availability.time) availability.time = _options.maintenance.time;
  _availability = std::make_unique<store::AvailabilityService>(_context, *_ledger, availability);

  step("open repair");
  auto repair = _options.repair;
  repair.extent_size = store_options.extent_size;
  if (!repair.time) repair.time = _options.maintenance.time;
  if (!repair.trace) repair.trace = _options.maintenance.trace;
  _repair = std::make_unique<store::Repair>(_context, *_store, *_availability, *_ledger, repair, _activity.get());

  step("open storage-server");
  auto server = _options.server;
  server.extent_size = store_options.extent_size;
  server.domain = _options.domain;
  _server =
      std::make_unique<store::StorageServer>(_context, store::StorageServer::Stores{*_data, *_control, *_claims, _cache.get(), _ledger.get(), _activity.get()},
                                             server, store::StorageServerAdmission{_credit.get(), _io.get()});
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
    _local_cpu.emplace(_options.maintenance.cpu);
    // The scrub's next campaign: kept across a restart, and from a date
    // Macha's maintenance left. A schedule that cannot be read starts afresh.
    {
      std::ifstream in(_options.state / "maintenance" / "scrub.next");
      uint64_t due = 0;
      if (in >> due && due) _scrub_due_unix_ms = due;
    }
    if (!_scrub_due_unix_ms) {
      _scrub_due_unix_ms = pass_wall().unix_ms() + static_cast<uint64_t>(std::max<int64_t>(1, _options.maintenance.scrub_interval.count()));
      persist_scrub_due();
    }
    dannan::contract::Lock figures(_gc_figures_mutex);
    _local_figures.scrub_due_unix_ms = _scrub_due_unix_ms;
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
  if (_spaces) _spaces->stop();
  close("heads", _spaces);
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
    _reloadable_locks.clear();
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

contract::ReachabilityProvider* Node::registered_provider() const {
  dannan::contract::Lock lock(_providers_mutex);
  const auto found = _providers.find(_options.domain.id);
  return found == _providers.end() ? nullptr : found->second;
}

void Node::ensure_collector_locked(contract::ReachabilityProvider& provider) {
  if (_collector && _collector_provider == &provider) return;
  ledger::CollectorOptions options;
  static_cast<ledger::CollectorBudgets&>(options) = _options.maintenance.collector;
  options.garbage_grace = _options.garbage_grace;
  _collector = std::make_unique<ledger::Collector>(*_ledger, provider, *_data, *_control, *_data_sightings, *_control_sightings, options,
                                                   ledger::CollectorServices{_context.logger("dagda.gc"), _context.observations(), pass_time(), pass_wall()},
                                                   _cache.get(), _options.maintenance.trace);
  _collector_provider = &provider;
}

ledger::Collector* Node::collector() {
  if (_options.maintenance.automatic) throw std::logic_error("the node's maintenance thread owns its collector: an owner's pass needs automatic off");
  dannan::contract::Lock maintenance(_maintenance_mutex);
  if (!_repair) return nullptr;
  auto* provider = registered_provider();
  if (!provider) return nullptr;
  ensure_collector_locked(*provider);
  return _collector.get();
}

ledger::UnreferencedSince& Node::data_sightings() {
  dannan::contract::Lock maintenance(_maintenance_mutex);
  return *_data_sightings;
}

ledger::UnreferencedSince& Node::control_sightings() {
  dannan::contract::Lock maintenance(_maintenance_mutex);
  return *_control_sightings;
}

bool Node::publish_inventory() {
  dannan::contract::Lock maintenance(_maintenance_mutex);
  auto* provider = registered_provider();
  if (!provider) return false;
  ledger::NodeHorizonBuilder builder(*provider, _context.logger("dagda.horizon"));
  _ledger->publish(builder.inventory());
  return true;
}

// ---- Maintenance ------------------------------------------------------------

const dannan::contract::TimeSource& Node::pass_time() const { return _options.maintenance.time ? *_options.maintenance.time : _context.time().source(); }

const dannan::contract::WallClock& Node::pass_wall() const { return _options.maintenance.wall ? *_options.maintenance.wall : *_options.wall; }

std::function<bool()> Node::local_yield(const LocalTurn& turn) const {
  return [this, forced = turn.forced, stop = turn.stop] { return stop.stop_requested() || (!forced && higher_class_active()); };
}

bool Node::higher_class_active() const {
  auto& activity = *_activity;
  const auto quiet = _options.repair.foreground_quiet;
  return activity.recently_active(dannan::sched::WorkClass::foreground, quiet) || activity.recently_active(dannan::sched::WorkClass::loader, quiet);
}

bool Node::gc_pass_locked(bool forced, MaintenanceReport& report, std::stop_token stop) {
  auto* provider = registered_provider();
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
  ensure_collector_locked(*provider);
  const bool busy = !forced && higher_class_active();
  ledger::PassDue due;
  due.garbage_due = !busy;
  due.gc_due = !busy;
  due.busy = busy;
  if (!forced) due.should_yield = [this] { return higher_class_active(); };
  due.pause = [stop] {
    if (stop.stop_requested()) throw contract::MaintenanceStopping{};
  };
  try {
    report.gc = _collector->pass(due);
  } catch (const contract::MaintenanceStopping&) {
    throw;
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

void Node::reconfigure(const NodeReload& reload) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (!_data) throw NodeRefused("the node is not running");
  std::set<std::filesystem::path> wanted;
  for (const auto& backend : reload.backends) {
    if (const auto why = store::backend_refusal_v18(_options.state, backend.path)) throw NodeRefused(*why);
    wanted.insert(backend.path);
  }
  if (!reload.cache.path.empty()) wanted.insert(reload.cache.path);
  // Every new root locked before anything changes.
  std::map<std::filesystem::path, std::unique_ptr<store::StorageLock>> added;
  for (const auto& root : wanted) {
    if (_reloadable_locks.contains(root)) continue;
    try {
      added.emplace(root, std::make_unique<store::StorageLock>(root));
    } catch (const std::exception& error) {
      throw NodeRefused(root.string() + " is in use (another node, or dagda-migrate, holds it): " + error.what());
    }
  }
  _reloadable_locks.merge(added);
  _data->reconfigure(reload.backends);
  _data->refresh();
  auto cache = reload.cache;
  cache.sealing = store::SealingScheme::v19(_options.domain.id);
  _cache->reconfigure(cache);
  // What has been let go, its lock with it.
  std::erase_if(_reloadable_locks, [&](const auto& entry) { return !wanted.contains(entry.first); });
  _options.pool.backends = reload.backends;
  _options.cache = reload.cache;
  _context.events().notify(dannan::status::NodeEvents::topology);
  _logger.info("reloaded: " + std::to_string(_data->online_backends()) + " backends online, cache " + (_cache->enabled() ? "on" : "off"));
}

void Node::persist_scrub_due() {
  // A hint for neighbourliness: failing to keep it stops nothing, and this
  // process still honours the date it holds.
  try {
    const auto dir = _options.state / "maintenance";
    std::filesystem::create_directories(dir);
    dannan::identity::durable_replace_file(dir / "scrub.next", std::to_string(_scrub_due_unix_ms) + "\n");
  } catch (const std::exception& error) {
    _logger.debug(std::string("scrub schedule not kept: ") + error.what());
  }
}

void Node::enter_stage(std::string_view stage) const {
  if (_options.maintenance.stage) _options.maintenance.stage(stage);
}

void Node::wake_maintenance() {
  _maintenance_kicked.store(true, std::memory_order_release);
  // Taken so the thread between its check and its wait cannot miss this.
  {
    std::lock_guard lock(_maintenance_wait);
  }
  _maintenance_wake.notify_all();
}

void Node::note_local(const std::function<void(LocalFigures&)>& change) {
  dannan::contract::Lock lock(_gc_figures_mutex);
  change(_local_figures);
}

LocalTurn Node::begin_local_locked(bool forced, const LocalTurnOptions& options, uint64_t events) {
  LocalTurn turn;
  turn.forced = forced;
  turn.events = events;
  turn.stop = options.stop;
  turn.now = pass_time().now();
  double seconds = _local_at ? std::max(0.0, std::chrono::duration<double>(turn.now - *_local_at).count()) : 0.0;
  _local_at = turn.now;
  if (seconds <= 0.0 && options.when_no_time_passed) seconds = std::chrono::duration<double>(*options.when_no_time_passed).count();
  turn.busy = !forced && options.busy.value_or(higher_class_active());
  const auto& policy = _options.maintenance;

  double bandwidth = _store->estimated_network_bps();
  if (bandwidth <= 0.0) bandwidth = static_cast<double>(_options.repair.initial_bandwidth);
  if (_options.repair.max_bandwidth) bandwidth = std::min(bandwidth, static_cast<double>(_options.repair.max_bandwidth));
  const double rate = bandwidth * (turn.busy ? policy.busy_bandwidth_fraction : policy.idle_bandwidth_fraction) * _local_cpu->scale(seconds, policy.cpu_target);
  const double extent = static_cast<double>(_options.store.extent_size);
  const double burst = std::max(extent, bandwidth * 5.0);
  _local_credit = forced ? burst : std::min(burst, _local_credit + rate * seconds);
  turn.wall_ms = pass_wall().unix_ms();
  turn.scrub_due = policy.scrub_fraction > 0.0 && turn.wall_ms >= _scrub_due_unix_ms;
  // Outside a campaign nothing is banked, so a due date is not a burst.
  if (!turn.scrub_due)
    _scrub_credit = 0.0;
  else
    _scrub_credit = forced ? burst : std::min(burst, _scrub_credit + rate * policy.scrub_fraction * seconds);
  return turn;
}

void Node::local_rebalance_locked(LocalTurn& turn) {
  throw_if_stopped(turn);
  auto& out = turn.stages;
  if (_local_quiescent_events && *_local_quiescent_events != turn.events) _local_quiescent_events.reset();
  if (turn.busy || _local_quiescent_events || _local_credit < static_cast<double>(_options.store.extent_size)) return;
  out.rebalance_ran = true;
  const auto started = dannan::contract::Clock::now();
  out.rebalance = _data->rebalance_step(static_cast<uint64_t>(_local_credit), 64, local_yield(turn));
  store::log_slow_stage(_logger, "local-rebalance", started,
                        "bytes=" + std::to_string(out.rebalance.bytes) + " objects=" + std::to_string(out.rebalance.objects));
  if (out.rebalance.bytes) _local_credit = std::max(0.0, _local_credit - static_cast<double>(out.rebalance.bytes));
  note_local([&](LocalFigures& figures) {
    figures.rebalance_objects += out.rebalance.objects;
    figures.rebalance_bytes += out.rebalance.bytes;
  });
  if (!out.rebalance.yielded && out.rebalance.complete && !out.rebalance.bytes) {
    _local_credit = 0.0;
    _local_quiescent_events = turn.events;
  }
  throw_if_stopped(turn);
}

void Node::retention_compact_locked(LocalTurn& turn) {
  throw_if_stopped(turn);
  // Only here, so a foreground claim never pays for the rewrite.
  if (turn.busy) return;
  enter_stage("retention-compact");
  auto& out = turn.stages;
  out.retention_ran = true;
  out.retention_compacted = _claims->compact_if_needed(4096);
  note_local([&](LocalFigures& figures) { figures.retention_compactions += out.retention_compacted ? 1 : 0; });
  throw_if_stopped(turn);
}

// An accepted head this replica cannot replay is excluded from reads and re-anchored here
// from any peer that can materialise it. A no-op unless a head is flagged. Any activity.
void Node::head_repair_locked(LocalTurn& turn) {
  throw_if_stopped(turn);
  if (!_spaces) return;
  const auto pause = [&turn] { throw_if_stopped(turn); };
  auto& out = turn.stages;
  try {
    enter_stage("head-repair");
    out.heads_repair_ran = true;
    const dannan::sched::WorkContext background(dannan::sched::FrameType::speculative, {}, nullptr, "maintenance::head-repair");
    for (auto* exchange : _spaces->exchanges())
      out.heads_repaired += exchange->repair_unreconstructable_heads(background, dannan::sched::FrameType::speculative, pause);
  } catch (const contract::MaintenanceStopping&) {
    throw;
  } catch (const std::exception& error) {
    _logger.debug(std::string("maintenance: metadata head repair failed: ") + error.what());
  }
  note_local([&](LocalFigures& figures) { figures.heads_repaired += out.heads_repaired; });
  throw_if_stopped(turn);
}

void Node::history_truncate_locked(LocalTurn& turn) {
  throw_if_stopped(turn);
  if (!_spaces || turn.busy) return;
  auto& out = turn.stages;
  try {
    enter_stage("history-truncate");
    out.history_truncate_ran = true;
    const dannan::sched::WorkContext background(dannan::sched::FrameType::speculative, {}, nullptr, "maintenance::history-truncate");
    for (auto* exchange : _spaces->exchanges()) {
      const auto before = exchange->space().replica().diagnostics().history_records;
      exchange->truncate_history(background);
      if (exchange->space().replica().diagnostics().history_records < before) ++out.history_truncated;
    }
  } catch (const contract::MaintenanceStopping&) {
    throw;
  } catch (const std::exception& error) {
    _logger.debug(std::string("maintenance: history truncation failed: ") + error.what());
  }
  note_local([&](LocalFigures& figures) { figures.history_truncations += out.history_truncated; });
  throw_if_stopped(turn);
}

std::vector<heads::RepairTurn> Node::metadata_repair_locked(std::stop_token stop, bool forced) {
  std::vector<heads::RepairTurn> turns;
  if (!_spaces) return turns;
  for (auto* exchange : _spaces->exchanges()) turns.push_back(exchange->repair_turn(stop, forced));
  return turns;
}

std::vector<heads::RepairTurn> Node::metadata_repair(std::stop_token stop) {
  dannan::contract::Lock lock(_maintenance_mutex);
  return metadata_repair_locked(stop, false);
}

void Node::head_repair(LocalTurn& turn) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (_repair) head_repair_locked(turn);
}

void Node::history_truncate(LocalTurn& turn) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (_repair) history_truncate_locked(turn);
}

void Node::compact_packs_locked(LocalTurn& turn) {
  throw_if_stopped(turn);
  if (turn.busy) return;
  enter_stage("compact-packs");
  auto& out = turn.stages;
  out.packs_ran = true;
  out.packs_compacted = _data->compact_packs(turn.stop);
  note_local([&](LocalFigures& figures) { figures.packs_compacted += out.packs_compacted; });
  throw_if_stopped(turn);
}

void Node::local_scrub_locked(LocalTurn& turn) {
  throw_if_stopped(turn);
  if (turn.busy || !turn.scrub_due || _scrub_credit < static_cast<double>(_options.store.extent_size)) return;
  auto& out = turn.stages;
  out.scrub_ran = true;
  const auto started = dannan::contract::Clock::now();
  out.scrub = _data->scrub_step(static_cast<uint64_t>(_scrub_credit), 64, local_yield(turn));
  store::log_slow_stage(_logger, "scrub", started, "bytes=" + std::to_string(out.scrub.bytes) + " objects=" + std::to_string(out.scrub.objects));
  if (out.scrub.bytes) _scrub_credit = std::max(0.0, _scrub_credit - static_cast<double>(out.scrub.bytes));
  const bool finished = !out.scrub.yielded && out.scrub.complete;
  if (finished) {
    _scrub_credit = 0.0;
    _scrub_due_unix_ms = turn.wall_ms + static_cast<uint64_t>(std::max<int64_t>(1, _options.maintenance.scrub_interval.count()));
    persist_scrub_due();
  }
  note_local([&, due = _scrub_due_unix_ms](LocalFigures& figures) {
    figures.scrub_objects += out.scrub.objects;
    figures.scrub_bytes += out.scrub.bytes;
    figures.scrub_campaigns += finished ? 1 : 0;
    figures.scrub_due_unix_ms = due;
  });
  throw_if_stopped(turn);
}

void Node::ledger_verify_locked(LocalTurn& turn) {
  throw_if_stopped(turn);
  if (turn.now < _verify_due) return;
  const double since = _verify_at ? std::max(0.0, std::chrono::duration<double>(turn.now - *_verify_at).count()) : 1.0;
  _verify_at = turn.now;
  _verify_credit = turn.forced ? 65536.0 : ledger_verify_credit(_verify_credit, since, turn.busy);
  const auto directories = static_cast<size_t>(_verify_credit);
  if (!directories) return;
  _verify_credit -= static_cast<double>(directories);
  auto& out = turn.stages;
  out.verify_ran = true;
  const auto data = _data->verify_step(directories);
  const auto control = _control->verify_step(directories);
  out.verify.directories = data.directories + control.directories;
  out.verify.recorded = data.recorded + control.recorded;
  out.verify.lost = data.lost + control.lost;
  out.verify.complete = data.complete && control.complete;
  auto& observations = _context.observations();
  observations.add("ledger.verify.directories", directories);
  observations.add("ledger.verify.recorded", out.verify.recorded);
  observations.add("ledger.verify.lost", out.verify.lost);
  if (out.verify.complete) {
    _verify_due = turn.now + std::chrono::hours(24);
    _verify_at.reset();
    _logger.info("storage held ledgers verified against the disk");
  }
  note_local([&](LocalFigures& figures) {
    figures.verify_directories += directories;
    figures.verify_recorded += out.verify.recorded;
    figures.verify_lost += out.verify.lost;
    figures.verify_passes += out.verify.complete ? 1 : 0;
  });
  throw_if_stopped(turn);
}

LocalStages Node::local_stages_locked(bool forced, std::stop_token stop) {
  LocalTurnOptions options;
  options.stop = stop;
  auto turn = begin_local_locked(forced, options, _context.events().total());
  try {
    local_rebalance_locked(turn);
    retention_compact_locked(turn);
    head_repair_locked(turn);
    history_truncate_locked(turn);
    compact_packs_locked(turn);
    local_scrub_locked(turn);
    ledger_verify_locked(turn);
  } catch (const contract::MaintenanceStopping&) {
    throw;
  } catch (const std::exception& error) {
    _logger.debug(std::string("local maintenance: ") + error.what());
  }
  return std::move(turn.stages);
}

LocalStages Node::run_local_stages(std::stop_token stop) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (!_repair) return {};
  return local_stages_locked(false, stop);
}

MaintenanceEvents Node::maintenance_events() const noexcept {
  const auto& events = _context.events();
  return {events.count(_storage_event), events.count(_metadata_event), events.count(dannan::status::NodeEvents::membership), events.count(_claims_event)};
}

LocalTurn Node::begin_local_stages(const LocalTurnOptions& options) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (!_repair) return {};
  return begin_local_locked(false, options, maintenance_events().total());
}

void Node::local_rebalance(LocalTurn& turn) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (_repair) local_rebalance_locked(turn);
}

void Node::retention_compact(LocalTurn& turn) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (_repair) retention_compact_locked(turn);
}

void Node::compact_packs(LocalTurn& turn) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (_repair) compact_packs_locked(turn);
}

void Node::local_scrub(LocalTurn& turn) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (_repair) local_scrub_locked(turn);
}

void Node::ledger_verify(LocalTurn& turn) {
  dannan::contract::Lock lock(_maintenance_mutex);
  if (_repair) ledger_verify_locked(turn);
}

MaintenanceReport Node::run_maintenance_once() {
  MaintenanceReport report;
  dannan::contract::Lock lock(_maintenance_mutex);
  if (!_repair) return report;
  report.metadata = metadata_repair_locked({}, true);
  for (const auto& turn : report.metadata) {
    if (!turn.hold_pass) continue;
    report.gc_skipped = "metadata repair failed with no head to stand on: " + turn.why;
    return report;
  }
  (void)gc_pass_locked(true, report);
  try {
    report.repair = _repair->pass();
  } catch (const std::exception& error) {
    _logger.debug(std::string("repair pass: ") + error.what());
  }
  report.local = local_stages_locked(true, {});
  return report;
}

void Node::maintenance_loop(std::stop_token stop) {
  const auto& time = pass_time();
  const auto interval = std::max(_options.maintenance.interval, std::chrono::milliseconds(1));
  const auto gc_interval = std::max(_options.maintenance.gc_interval, interval);
  auto next_repair = time.now();
  auto next_gc = next_repair;
  while (!stop.stop_requested()) {
    const auto now = time.now();
    bool hold_pass = false;
    std::optional<dannan::contract::TimePoint> metadata_due;
    {
      dannan::contract::Lock lock(_maintenance_mutex);
      _maintenance_kicked.store(false, std::memory_order_release);
      try {
        for (const auto& turn : metadata_repair_locked(stop, false)) hold_pass = hold_pass || turn.hold_pass;
      } catch (const contract::MaintenanceStopping&) {
        return;
      }
      if (_spaces)
        for (auto* exchange : _spaces->exchanges())
          if (const auto due = exchange->turn_due()) metadata_due = metadata_due ? std::min(*metadata_due, *due) : *due;
      if (hold_pass) {
        // Recovery required, or no head yet: nothing that depends on the head proceeds.
      } else if (now >= next_gc) {
        MaintenanceReport report;
        try {
          next_gc = now + (gc_pass_locked(false, report, stop) ? gc_interval : interval);
        } catch (const contract::MaintenanceStopping&) {
          return;
        }
      }
      if (!hold_pass && now >= next_repair) {
        try {
          (void)_repair->pass(stop);
        } catch (const store::MaintenanceStopping&) {
          // Asked to stop inside a long walk: the loop's condition ends it.
          return;
        } catch (const std::exception& error) {
          _logger.debug(std::string("repair pass: ") + error.what());
        }
        try {
          (void)local_stages_locked(false, stop);
        } catch (const contract::MaintenanceStopping&) {
          return;
        }
        next_repair = now + interval;
      }
    }
    // Waits in real time for at most an interval, then reads the node's time
    // again: a stepped clock moved meanwhile is seen within an interval.
    const auto after = time.now();
    auto due = hold_pass ? after + interval : std::min(next_gc, next_repair);
    if (metadata_due) due = std::min(due, *metadata_due);
    if (due <= after) continue;
    const auto wait = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(due - after) + std::chrono::milliseconds(1), interval);
    std::unique_lock lock(_maintenance_wait);
    _maintenance_wake.wait_for(lock, stop, wait, [this] { return _maintenance_kicked.load(std::memory_order_acquire); });
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
//   data_store.loose_reaffirmation_fast_paths,
//   data_store.loose_reaffirmation_full_validations,
//   data_store.presence_index_entries, data_store.pack_recovery_truncated_tails,
//   data_store.pack_recovery_skipped_regions, data_store.pack_recovery_skipped_bytes;
//   cache.enabled, cache.blocks, cache.capacity_bytes, cache.used_bytes, cache.hits,
//   cache.misses, cache.evictions;
//   activity.foreground_idle_ms, activity.loader_idle_ms;
//   references.entries, references.extents;
//   repair.passes, repair.copies_pushed, repair.copies_pulled,
//   repair.local_drops, repair.bytes, repair.unsourceable,
//   repair.local_unreadable, repair.push_examined, repair.pull_examined,
//   repair.push_phase_complete, repair.unsourceable_sample (ids, comma-separated),
//   repair.estimated_network_bps, repair.last_credit_bytes, repair.paced_by (the
//   RepairPacedBy bits at the last gate); repair.gate.ran, repair.gate.share,
//   repair.gate.quiescent, repair.gate.credit, repair.gate.last;
//   maintenance.automatic; maintenance.rebalance_objects,
//   maintenance.rebalance_bytes, maintenance.retention_compactions,
//   maintenance.packs_compacted, maintenance.scrub_objects,
//   maintenance.scrub_bytes, maintenance.scrub_campaigns,
//   maintenance.scrub_due_unix_ms, maintenance.verify_directories,
//   maintenance.verify_recorded, maintenance.verify_lost,
//   maintenance.verify_passes, maintenance.heads_repaired,
//   maintenance.history_truncations;
//   heads.spaces; and for each space under heads.<name>.: generation, history_records,
//   history_file_bytes, history_resident_payload_bytes, historical_requests,
//   historical_reconstructions, historical_deltas_applied,
//   materialization_cache_hits, _misses, _evictions, _entries, _bytes, _limit_bytes,
//   accepted_head_persistence_writes, _bytes, _failures; head_holders,
//   head_holders_present, heads_set_aside; reconciliations, conflicts_created,
//   conflicts_superseded; mutations, mutation_retention_ms_total, _max,
//   mutation_publish_ms_total, _max; claims_owed_commits, claims_owed_bytes,
//   claims_owed_dropped; availability, write_available, stable, replicas, replicas_online,
//   write_replicas_required, observed_unix_ms; history_transfers,
//   history_entries_submitted, history_peak_in_flight; announcements; convergence.events_received,
//   .runs_scheduled, .runs_completed, .requested_epoch, .completed_epoch,
//   .latest_generation, .scheduled;
//   gc.provider, gc.grace_ms, gc.passes, gc.skipped
//   (no provider), gc.failures (the provider could not answer),
//   gc.inventories, gc.released_data, gc.released_control,
//   gc.removed_control, gc.examined, gc.reclaimed_bytes, gc.pruned, gc.state
//   (what the last pass came to, in words), gc.data_gate (open, or why
//   shut);
//   prompt.queued, prompt.copies, prompt.failures, prompt.skipped_no_room,
//   prompt.dropped;
//   and with admission: admission.credit_used, admission.device_pressured,
//   admission.pressure_refusals, admission.capacity_bytes,
//   admission.foreground_reserve_bytes, admission.peak_used_bytes,
//   admission.foreground_admissions, admission.loader_admissions,
//   admission.speculative_admissions, admission.foreground_waits,
//   admission.loader_waits, admission.speculative_waits, admission.cancelled_waits,
//   admission.background_limit, admission.background_active,
//   admission.peak_background_active, admission.device_service_us,
//   admission.device_worst_us, admission.device_slowdown_percent,
//   admission.device_pressure_onsets, admission.no_progress_failures,
//   admission.data_in_flight, admission.control_in_flight.
void Node::publish_status() {
  if (!_data) return;
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

  if (_spaces) {
    const auto exchanges = _spaces->exchanges();
    put("heads.spaces", u64(exchanges.size()));
    for (const auto* exchange_const : exchanges) {
      auto* exchange = const_cast<heads::Exchange*>(exchange_const);
      const auto head = [&](const std::string& name, dannan::status::Surface::Value value) { put("heads." + exchange->name() + "." + name, std::move(value)); };
      auto& space = exchange->space();
      const auto replica = space.replica().diagnostics();
      head("generation", u64(space.current_generation()));
      head("history_records", u64(replica.history_records));
      head("history_file_bytes", u64(replica.history_file_bytes));
      head("history_resident_payload_bytes", u64(replica.history_resident_payload_bytes));
      head("historical_requests", u64(replica.historical_requests));
      head("historical_reconstructions", u64(replica.historical_reconstructions));
      head("historical_deltas_applied", u64(replica.historical_deltas_applied));
      head("materialization_cache_hits", u64(replica.materialization_cache_hits));
      head("materialization_cache_misses", u64(replica.materialization_cache_misses));
      head("materialization_cache_evictions", u64(replica.materialization_cache_evictions));
      head("materialization_cache_entries", u64(replica.materialization_cache_entries));
      head("materialization_cache_bytes", u64(replica.materialization_cache_bytes));
      head("materialization_cache_limit_bytes", u64(replica.materialization_cache_limit_bytes));
      head("accepted_head_persistence_writes", u64(replica.accepted_head_persistence_writes));
      head("accepted_head_persistence_bytes", u64(replica.accepted_head_persistence_bytes));
      head("accepted_head_persistence_failures", u64(replica.accepted_head_persistence_failures));
      const auto status = exchange->status();
      head("head_holders", u64(status.head_holders));
      head("head_holders_present", u64(status.head_present));
      head("heads_set_aside", u64(space.set_aside_count()));
      const auto reconciled = exchange->reconciliations();
      head("reconciliations", u64(reconciled.merges));
      head("conflicts_created", u64(reconciled.conflicts_created));
      head("conflicts_superseded", u64(reconciled.conflicts_superseded));
      const auto timing = space.mutation_timing();
      head("mutations", u64(timing.mutations));
      head("mutation_retention_ms_total", u64(timing.retention_ms_total));
      head("mutation_retention_ms_max", u64(timing.retention_ms_max));
      head("mutation_publish_ms_total", u64(timing.publish_ms_total));
      head("mutation_publish_ms_max", u64(timing.publish_ms_max));
      const auto owed = space.owed_backlog();
      head("claims_owed_commits", u64(owed.commits));
      head("claims_owed_bytes", u64(owed.claim_bytes));
      head("claims_owed_dropped", u64(owed.dropped));
      head("availability", std::string(heads::availability_name(status.availability)));
      head("write_available", status.write_available);
      head("stable", status.stable);
      head("replicas", u64(status.replicas));
      head("replicas_online", u64(status.replicas_online));
      head("write_replicas_required", u64(status.write_replicas_required));
      head("observed_unix_ms", u64(status.observed_unix_ms));
      const auto transfers = exchange->transfers();
      head("history_transfers", u64(transfers.transfers));
      head("history_entries_submitted", u64(transfers.entries_submitted));
      head("history_peak_in_flight", u64(transfers.peak_in_flight));
      head("announcements", u64(exchange->announcements()));
      const auto demand = exchange->demand();
      head("convergence.events_received", u64(demand.events_received));
      head("convergence.runs_scheduled", u64(demand.runs_scheduled));
      head("convergence.runs_completed", u64(demand.runs_completed));
      head("convergence.requested_epoch", u64(demand.requested_epoch));
      head("convergence.completed_epoch", u64(demand.completed_epoch));
      head("convergence.latest_generation", u64(demand.latest_generation));
      head("convergence.scheduled", demand.scheduled);
    }
  }

  const auto data_store = _data->diagnostics();
  put("data_store.loose_reaffirmation_fast_paths", u64(data_store.loose_reaffirmation_fast_paths));
  put("data_store.loose_reaffirmation_full_validations", u64(data_store.loose_reaffirmation_full_validations));
  put("data_store.presence_index_entries", u64(data_store.presence_index_entries));
  put("data_store.pack_recovery_truncated_tails", u64(data_store.pack_recovery_truncated_tails));
  put("data_store.pack_recovery_skipped_regions", u64(data_store.pack_recovery_skipped_regions));
  put("data_store.pack_recovery_skipped_bytes", u64(data_store.pack_recovery_skipped_bytes));

  const auto cache = _cache->stats();
  put("cache.enabled", _cache->enabled());
  put("cache.blocks", u64(_cache->blocks()));
  put("cache.capacity_bytes", u64(_options.cache.max_blocks * _options.store.extent_size));
  put("cache.used_bytes", u64(_cache->blocks() * _options.store.extent_size));
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
  put("repair.push_examined", u64(repair.push_examined));
  put("repair.pull_examined", u64(repair.pull_examined));
  put("repair.push_phase_complete", repair.push_phase_complete);
  std::string sample;
  for (const auto& id : repair.unsourceable_sample) sample += (sample.empty() ? "" : ",") + store::to_string(id);
  put("repair.unsourceable_sample", std::move(sample));
  put("repair.estimated_network_bps", u64(static_cast<uint64_t>(_store->estimated_network_bps())));
  const auto gates = _repair->gates();
  put("repair.gate.ran", u64(gates.ran));
  put("repair.gate.share", u64(gates.share));
  put("repair.gate.quiescent", u64(gates.quiescent));
  put("repair.gate.credit", u64(gates.credit));
  put("repair.gate.last", std::string(gates.last ? gate_name(*gates.last) : "none"));
  put("repair.last_credit_bytes", u64(gates.last_credit_bytes));
  put("repair.paced_by", u64(gates.paced_by));
  const auto idle_ms = [&](dannan::sched::WorkClass work) { return u64(static_cast<uint64_t>(std::max<int64_t>(0, _store->idle_for(work).count()))); };
  put("activity.foreground_idle_ms", idle_ms(dannan::sched::WorkClass::foreground));
  put("activity.loader_idle_ms", idle_ms(dannan::sched::WorkClass::loader));

  bool provider = false;
  {
    dannan::contract::Lock lock(_providers_mutex);
    provider = _providers.contains(_options.domain.id);
  }
  GcFigures gc;
  LocalFigures local;
  {
    dannan::contract::Lock lock(_gc_figures_mutex);
    gc = _gc_figures;
    local = _local_figures;
  }
  put("maintenance.automatic", _options.maintenance.automatic);
  put("maintenance.rebalance_objects", u64(local.rebalance_objects));
  put("maintenance.rebalance_bytes", u64(local.rebalance_bytes));
  put("maintenance.retention_compactions", u64(local.retention_compactions));
  put("maintenance.packs_compacted", u64(local.packs_compacted));
  put("maintenance.scrub_objects", u64(local.scrub_objects));
  put("maintenance.scrub_bytes", u64(local.scrub_bytes));
  put("maintenance.scrub_campaigns", u64(local.scrub_campaigns));
  put("maintenance.scrub_due_unix_ms", u64(local.scrub_due_unix_ms));
  put("maintenance.verify_directories", u64(local.verify_directories));
  put("maintenance.verify_recorded", u64(local.verify_recorded));
  put("maintenance.verify_lost", u64(local.verify_lost));
  put("maintenance.verify_passes", u64(local.verify_passes));
  put("maintenance.heads_repaired", u64(local.heads_repaired));
  put("maintenance.history_truncations", u64(local.history_truncations));
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
    put("admission.capacity_bytes", u64(credit.capacity_bytes));
    put("admission.foreground_reserve_bytes", u64(credit.foreground_reserve_bytes));
    put("admission.peak_used_bytes", u64(credit.peak_used_bytes));
    put("admission.foreground_admissions", u64(credit.foreground_admissions));
    put("admission.loader_admissions", u64(credit.loader_admissions));
    put("admission.speculative_admissions", u64(credit.speculative_admissions));
    put("admission.foreground_waits", u64(credit.foreground_waits));
    put("admission.loader_waits", u64(credit.loader_waits));
    put("admission.speculative_waits", u64(credit.speculative_waits));
    put("admission.cancelled_waits", u64(credit.cancelled_waits));
    put("admission.background_limit", u64(credit.background_limit));
    put("admission.background_active", u64(credit.background_active));
    put("admission.peak_background_active", u64(credit.peak_background_active));
    put("admission.device_service_us", u64(credit.device_service_us));
    put("admission.device_worst_us", u64(credit.device_worst_us));
    put("admission.device_slowdown_percent", u64(credit.device_slowdown_percent));
    put("admission.device_pressure_onsets", u64(credit.device_pressure_onsets));
    put("admission.no_progress_failures", u64(credit.no_progress_failures));
  }
  if (_io) {
    const auto io = _io->stats();
    put("admission.data_in_flight", u64(io.data_in_flight));
    put("admission.control_in_flight", u64(io.control_in_flight));
  }
}

}  // namespace dagda
