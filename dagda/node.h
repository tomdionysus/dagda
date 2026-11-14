//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dagda/contract/reachability.h"
#include "dagda/domain/domain.h"
#include "dagda/domain/storage_key.h"
#include "dagda/domain/wrap.h"
#include "dagda/heads/space.h"
#include "dagda/heads/spaces.h"
#include "dagda/lease/keeper.h"
#include "dagda/lease/local_records.h"
#include "dagda/lease/membership.h"
#include "dagda/ledger/collector.h"
#include "dagda/ledger/reference_counts.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/retention_ledger.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/store/availability.h"
#include "dagda/store/control_store.h"
#include "dagda/store/cpu_pacing.h"
#include "dagda/store/data_work.h"
#include "dagda/store/distributed_store.h"
#include "dagda/store/local_store.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/repair.h"
#include "dagda/store/storage_pool.h"
#include "dagda/store/storage_server.h"
#include "dannan/context.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/supervise/startup.h"

namespace dagda {

class Context;

// The node's DATA admission (Macha's data_inflight_bytes and its foreground
// reserve, its background concurrency and no-progress window) and its device
// I/O reserve.
struct AdmissionOptions {
  uint64_t data_inflight_bytes = 128ULL * 1024 * 1024;
  uint64_t foreground_reserve_bytes = 32ULL * 1024 * 1024;
  uint64_t background_concurrency = 4;
  // Positive: a facade given Macha's 0 passes the value named in S.3 of
  // TODO/2026-11-05-second-cutover.md.
  std::chrono::milliseconds no_progress = store::default_no_progress;
  // Background work admitted while the DATA device is slow.
  uint64_t min_background_under_pressure = 1;
  // What counts as slow, Macha's dht.io_pressure_*. A zero slowdown_percent turns the
  // gate off, as Macha's does: admission does not watch the device.
  store::DiskServiceThresholds device_pressure{};
  // Macha has no device reserve: its facade turns this off, so a Macha node's DATA
  // work is not capped by it.
  bool device_reserve = true;
  store::IoReserveOptions io{};
};

// The node's maintenance (the maintainer, 2026-11-04 night: maintenance moves into the
// node): a repair pass and a GC pass, each on its own schedule on the node's
// steady time (context.time().source()), on one supervised thread. Macha's
// pass is one loop every `interval` (config.hpp MaintenanceConfig); repair
// runs every pass, paced inside by its duty share (RepairOptions), and GC
// runs every pass until its sweep completes with nothing deferred, then
// rests gc_interval (Macha's no_progress_backoff, the pause after a pass
// with nothing to do). GC is skipped while foreground or loader work is
// present (repair's foreground_quiet) and its DATA sweep yields to it.
//
// After them each pass runs Macha's local stages in its order: rebalance onto
// the preferred backend, the claims' compaction past 4096 records, DATA pack
// compaction of one victim per backend, the scrub when its campaign is due,
// and the held ledgers' verify. All but the verify are skipped while
// foreground or loader work is present, and rebalance and scrub also yield to
// it within their slice. The verify is paced, never stopped: 4 directories a
// second, 0.25 when busy, a pass then a day's rest. Rebalance and scrub spend
// byte credit: the link's rate times the fraction below, shrunk by the rest of
// the process's CPU, banked to five seconds of the link or an extent.
struct MaintenanceOptions {
  // Whether the node runs its maintenance thread. Off, only
  // run_maintenance_once() runs a pass: a test's deterministic passes.
  bool automatic = true;
  std::chrono::milliseconds interval{1000};
  std::chrono::milliseconds gc_interval{300000};
  // The collector's budgets and retry window, Macha's defaults. The wait is
  // the domain's (NodeOptions::garbage_grace).
  ledger::CollectorBudgets collector;
  // Rebalance's and scrub's share of the link, idle and busy.
  double idle_bandwidth_fraction = 0.10;
  double busy_bandwidth_fraction = 0.0;
  // Of that, the scrub's; zero: no scrub.
  double scrub_fraction = 0.02;
  // A full scrub's period. The next due date is kept at
  // <state>/maintenance/scrub.next, Macha's file, so neither a restart nor the
  // migration postpones it.
  std::chrono::milliseconds scrub_interval{std::chrono::hours(24 * 30)};
  double cpu_target = 0.10;
  store::CpuSource cpu = store::system_cpu_sample;
  // The clocks a pass runs on, for an owner with its own (Macha's MaintenanceClock,
  // which its trace fixtures move by hand): the steady one is what the pass's
  // schedule and credit, the collector, repair (unless RepairOptions::time says
  // otherwise), the survey (AvailabilityOptions::time likewise), the verify's rest
  // and the activity clocks read; the wall one dates the sightings, the retirements'
  // ages and the scrub's campaigns. The stores keep NodeOptions::wall, as Macha's
  // files keep real time. None: the node's, and NodeOptions::wall.
  const dannan::contract::TimeSource* time = nullptr;
  const dannan::contract::WallClock* wall = nullptr;
  // The decision trace, Macha's MaintenanceTraceHook: what the collector, repair's
  // claim walks and the store's repair decide, in Macha's kinds and words. It
  // reaches repair and the store unless RepairOptions::trace and
  // DistributedStoreOptions::repair_trace name their own.
  ledger::Collector::Trace trace;
  // Told each stage a pass reaches, in Macha's names ("metadata-repair", "head-repair",
  // "history-truncate" and so on), on the thread running it: Macha's enter_stage and its
  // maintenance_stage_hook. It may hold a stage; one that throws fails the stage.
  std::function<void(std::string_view)> stage;
};

// What one pass of the local stages did. A `_ran` is false when its gate held
// it: activity, credit, the schedule or having nothing left to do.
struct LocalStages {
  bool rebalance_ran = false;
  store::StoragePool::MaintenanceResult rebalance;
  bool retention_ran = false;
  bool retention_compacted = false;
  bool packs_ran = false;
  size_t packs_compacted = 0;
  bool scrub_ran = false;
  store::StoragePool::MaintenanceResult scrub;
  bool verify_ran = false;
  store::LocalStore::VerifyResult verify;
  // Between the claims' compaction and DATA packs, each in its own try. Head repair runs
  // busy or not, a no-op unless a head is flagged; history truncation only when idle.
  bool heads_repair_ran = false;
  size_t heads_repaired = 0;
  bool history_truncate_ran = false;
  size_t history_truncated = 0;
};

// What begins a pass's local stages, for an owner running them one by one.
struct LocalTurnOptions {
  // Macha's stop: a stage it reaches throws contract::MaintenanceStopping.
  std::stop_token stop;
  // Whether a higher class is present, when the owner's pass has decided. None: the
  // node's activity clocks say.
  std::optional<bool> busy;
  // What to earn for when no time has passed, as Macha's pass earns a whole
  // interval (maintenance.cpp:388-390). None earns nothing.
  std::optional<std::chrono::milliseconds> when_no_time_passed;
};

// What has happened that a Macha pass wakes for, one monotonic count per kind:
// Macha's four events. `topology` is Dannan's membership, the roster's changes less
// its adverts, because Macha's own topology event never moved on a peer's storage
// figures. Metadata and claims are the owner's to notify, under those names.
struct MaintenanceEvents {
  uint64_t storage = 0;
  uint64_t metadata = 0;
  uint64_t topology = 0;
  uint64_t claims = 0;
  uint64_t total() const noexcept { return storage + metadata + topology + claims; }
};

// One pass's local stages: what begin_local_stages() decided, and in `stages`
// what the stages run on it did.
struct LocalTurn {
  bool forced = false;
  bool busy = false;
  bool scrub_due = false;
  dannan::contract::TimePoint now{};
  uint64_t wall_ms = 0;
  // The count of events rebalance's rest is measured against.
  uint64_t events = 0;
  std::stop_token stop;
  LocalStages stages;
};

// The verify's credit in directories after `seconds` more (Macha's rate).
inline double ledger_verify_credit(double credit, double seconds, bool busy) noexcept { return std::min(256.0, credit + seconds * (busy ? 0.25 : 4.0)); }

// What one run_maintenance_once() did: the GC pass's report, or why none
// ran ("no reachability provider registered ..."; the provider's error),
// and the repair pass.
struct MaintenanceReport {
  // What metadata repair did first, a turn per space.
  std::vector<heads::RepairTurn> metadata;
  std::optional<ledger::PassReport> gc;
  std::string gc_skipped;
  store::RepairPass repair;
  LocalStages local;
};

// What a node runs with: plain values an owner's config reader fills. The
// node makes them agree where two parts must: the domain seals the pool, the
// control store, the cache and the state layout and is the store's and the
// server's; the extent size is the store's everywhere; every ledger is under
// the node-local key, and the one ledger cache is divided as Macha divides it.
struct NodeOptions {
  // Dagda's state: the layout marker, the ledgers and, by default, the
  // control store. A directory of its own, never Dannan's state path.
  std::filesystem::path state;
  // The domain this node's store holds and its key; until the catalogue
  // (step 4) a node holds one domain, v1 (Macha's), whose key is
  // StorageKey::derive_v1 of the cluster key. A member's key is required.
  domain::Domain domain = domain::domain_v1_descriptor();
  std::shared_ptr<const domain::StorageKey> key;
  // The domain's wait (the maintainer, 2026-11-04 night: settable per domain): an
  // unreferenced, unclaimed object goes only once this node has seen it so
  // for this long on its own clock, and a retirement is collectable this
  // long after it was stamped. Macha's 30 days. Carried here for the node's
  // one domain until the catalogue (step 4), whose domain descriptor
  // carries it.
  std::chrono::milliseconds garbage_grace{std::chrono::hours(24 * 30)};
  // DATA backends, packing and the durability window.
  store::StoragePoolOptions pool;
  store::ControlStoreOptions control;
  store::CacheOptions cache;
  uint64_t ledger_cache_bytes = store::default_ledger_cache_bytes;
  // Nothing: the node admits everything.
  std::optional<AdmissionOptions> admission = AdmissionOptions{};
  store::StorageServerOptions server;
  store::DistributedStoreOptions store;
  store::AvailabilityOptions availability;
  store::RepairOptions repair;
  MaintenanceOptions maintenance;
  lease::LeaseOptions leases;
  // How often the node's figures are published to Dannan's Status surface.
  std::chrono::milliseconds status_interval{10000};
  // The clock dates on disk are written by; the steady clock is the node's,
  // from context.time().source().
  const dannan::contract::WallClock* wall = &dannan::contract::system_wall_clock();
  // In front of the storage server: a request it answers is not passed on
  // (an owner's fault injection or accounting). Nothing: the server answers.
  std::function<std::optional<dannan::net::Handled>(const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame,
                                                    const dannan::net::Message& request)>
      front;
  // Each stage as it opens and stops ("open storage-pool", "stop
  // storage-pool"), as the node logs it, on the thread opening or stopping the
  // node. An open stage is told before its work, and the node waits for the
  // call: Macha's StartupStageHook, which may hold a stage to stall a startup.
  std::function<void(std::string_view)> trace;
  // Ticked by each unit of recovery as the stores open: an owner's, outliving
  // the node, for its stall gate over the construction (Macha's
  // service_startup_no_progress). Nothing: only the node sees them.
  dannan::supervise::StartupProgress* startup_progress = nullptr;
  // Whether the node runs head spaces. Off, it declares no "metadata" plane and runs
  // no head stage, so an owner whose own replica still runs (Macha through the first
  // stage of the port) declares and marks that plane itself; a Heads noun refuses
  // whatever it is asked, saying why. On is the library's default.
  bool heads = true;
  // The spaces whose replay readiness waits on: opened in the "open heads" stage, and
  // the "metadata" plane marked after them, so at once with none. Context::Heads opens
  // further spaces at any time, and those gate nothing.
  std::vector<heads::SpaceSpec> spaces;
  // What the spaces exchange under, the numbers of their messages among it. An owner
  // whose own registration of these types stands sets `heads` off, and no type is
  // registered here; when its stands no more it passes its numbers here.
  heads::ExchangeOptions exchange;
};

// What a node takes while running: Macha's live reload (LocalState::reconfigure),
// its DATA backends and its cache. Everything else in NodeOptions takes a restart,
// as Macha's replicas and intervals do.
struct NodeReload {
  std::vector<store::StorageBackendOptions> backends;
  store::CacheOptions cache;
};

// Why a node did not open. Nothing it opened is left open, and nothing on a
// disk is changed by a refusal before the state layout is accepted.
class NodeRefused final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A node's store, assembled: what a hosted system starts on a Dannan node to
// have Dagda there, and the dagda::Context it hands that system.
//
// Construction checks the Dannan contract's version, registers Dagda's
// message types on context.routes() (so it must come before the Dannan node
// starts), takes the storage lock on the state path and every store root, and
// then opens, in order: the state layout (a v18 state or backend is refused
// with a reason naming dagda-migrate, and nothing is changed), the node-local
// key and the wrap key (a node without custody is refused; one without a
// certificate opens and advertises no wrap key), the DATA pool and
// its admission, the control store, the cache, the claims and the reference
// counts, the distributed store, the head spaces, availability, repair and the
// storage server. Each readiness plane is reported ready as its part opens; a pool
// that holds nothing it should (every backend offline) is ready but degraded
// (dagda.storage.degraded). The node then publishes its figures to Dannan's Status surface,
// at once and every status_interval. stop() closes it in the reverse order,
// admission first so a waiting request gives up, and releases the locks
// last.
//
// Maintenance is the node's (MaintenanceOptions): the collector and repair
// run on its maintenance thread, the owner supplying only its reachability
// provider per domain, from which the collector builds the inventory repair
// and the survey work from. With no provider registered the collector does
// nothing and says so in Status (dagda.gc.state); repair still runs on
// whatever inventory was last published. stop() stops the thread first.
//
// Construction and stop() wait on the devices and must not run on one of the
// io context's threads; everything else is thread-safe unless it says so.
class Node {
 public:
  using Front = decltype(NodeOptions::front);

  // Throws NodeRefused, with a reason, for a Dannan contract of another
  // version, a Dannan node already started, a held lock, a refused layout, a
  // node without custody, or a part that cannot open. Declares its own
  // readiness planes (data_storage, control_storage, cache, retention, and
  // metadata unless NodeOptions::heads is off).
  Node(dannan::Context& context, NodeOptions options);

  // In two parts, for an owner whose node must answer peers and Status while the
  // store recovers, as Macha's does (service.cpp:582-586): this constructor checks
  // the contract, declares the planes and registers the message types, which must
  // come before the Dannan node starts, and opens nothing, so it waits on no device.
  // open_store() is everything else, after the Dannan node starts, on a thread of
  // the owner's: a request meanwhile finds no handler, as a peer's would on a node
  // still recovering. Each throws NodeRefused as the constructor above does; once
  // refused, the node is stopped. open_store() twice throws std::logic_error.
  struct OpenLater {};
  Node(dannan::Context& context, NodeOptions options, OpenLater);
  void open_store();
  ~Node();
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // Closes the node; idempotent. Waits on the devices and on requests in
  // flight.
  void stop();
  bool running() const noexcept { return _server != nullptr; }

  // The API a hosted system holds.
  Context& context() noexcept { return *_api; }
  dannan::Context& dannan() noexcept { return _context; }
  const NodeOptions& options() const noexcept { return _options; }

  // The parts, for an owner's maintenance and for tests. Valid while
  // running().
  store::StoragePool& data() noexcept { return *_data; }
  store::LocalStore& control() noexcept { return *_control; }
  store::PersistentBlockCache& cache() noexcept { return *_cache; }
  ledger::RetentionStore& claims() noexcept { return *_claims; }
  ledger::RetentionLedger& ledger() noexcept { return *_ledger; }
  ledger::ReferenceCounts& references() noexcept { return *_references; }
  store::DistributedStore& store() noexcept { return *_store; }
  store::AvailabilityService& availability() noexcept { return *_availability; }
  store::Repair& repair() noexcept { return *_repair; }
  store::StorageServer& server() noexcept { return *_server; }
  // Null when the node admits everything.
  store::DataResourceArbiter* credit() noexcept { return _credit.get(); }
  // Null with no device reserve.
  store::IoReserve* io() noexcept { return _io.get(); }
  // When object work of each class was last present: the store's own, in Macha's
  // meaning. Valid while running().
  dannan::status::ActivityClocks& activity() noexcept { return *_activity; }
  const domain::WrapKey& wrap_key() const noexcept { return *_wrap; }
  lease::LeaseKeeper& lease_keeper() noexcept { return *_keeper; }
  lease::LeaseRecords& lease_records() noexcept { return _lease_records; }
  // Null with NodeOptions::heads off. Valid while running().
  heads::Spaces* spaces() noexcept { return _spaces.get(); }

  // The owner's reachability provider for a domain (requirement C3): what
  // its state refers to, from which the inventory is built. One per domain
  // until several are unioned (step 6, with the catalogue); registering
  // again replaces it. Throws std::invalid_argument for a domain this node
  // does not hold. The provider outlives the node or is unregistered first.
  // Both wait for a maintenance pass in progress, so once unregister returns
  // the node no longer reads the provider.
  static constexpr dannan::sched::Waits register_waits = ledger::Collector::pass_waits | store::Repair::pass_waits;
  void register_reachability(const domain::DomainId& domain, contract::ReachabilityProvider& provider);
  void unregister_reachability(const domain::DomainId& domain);
  // Builds the inventory from the provider of the node's domain at its
  // current generation and publishes it to the ledger, as the collector
  // does at the start of its pass: what the survey and repair take as
  // referenced. False when no provider is registered. Throws what the
  // provider throws. Serialised with the maintenance passes.
  static constexpr dannan::sched::Waits publish_inventory_waits = contract::ReachabilityProvider::answer_waits | dannan::sched::Waits::locks;
  bool publish_inventory();

  // For an owner whose own pass drives the steps, as Macha's does until stage 8c
  // (MaintenanceOptions::automatic off, one thread: the steps are single owner, and
  // repair() is the other half). The node's collector over the provider registered
  // for its domain, null with none; registering or unregistering a provider ends the
  // one handed out. It works from the node's sightings, its budgets, the domain's
  // wait, the pass's clocks and the trace. dagda.gc.* count the node's own passes,
  // not an owner's steps. Throws std::logic_error while the node's thread runs.
  static constexpr dannan::sched::ThreadSafety collector_safety = dannan::sched::ThreadSafety::single_owner;
  ledger::Collector* collector();
  // The node's own sightings, Macha's files (requirement L22). Valid while running().
  ledger::UnreferencedSince& data_sightings();
  ledger::UnreferencedSince& control_sightings();

  // One GC pass, due whatever the schedule and the node's activity says, and
  // one repair pass, on the caller's thread: what the maintenance thread
  // runs, for a test that drives the passes itself (with automatic off) or
  // an owner that wants a pass now. Serialised with the thread's passes.
  // Throws nothing: a provider's error is the report's gc_skipped.
  static constexpr dannan::sched::Waits run_maintenance_waits = register_waits | dannan::sched::Waits::locks;
  // The local stages are due as well, with full credit and a whole verify pass,
  // though the scrub still only when its campaign is and the verify not during
  // its day's rest.
  MaintenanceReport run_maintenance_once();
  // One pass of the local stages as the thread runs them: credit earned since
  // the last, and held by activity. Serialised with the thread's passes. A failed
  // stage is logged and ends the pass's stages; `stop`, once requested, throws
  // contract::MaintenanceStopping out of the stage it reaches, and nothing here
  // catches that.
  static constexpr dannan::sched::Waits local_stage_waits =
      dannan::sched::Waits::state_device | dannan::sched::Waits::data_device | dannan::sched::Waits::locks;
  LocalStages run_local_stages(std::stop_token stop = {});

  // The same stages one at a time, so an owner's pass can put its own between them
  // in its own order, as Macha's puts head repair and history truncation between
  // the claims' compaction and pack compaction (maintenance.cpp:1098-1225):
  // begin_local_stages() earns the credit, then local_rebalance(),
  // retention_compact(), compact_packs(), local_scrub() and ledger_verify(), each
  // behind its gate as run_local_stages() has it. Each serialises with the thread's
  // passes for its own length only, so what falls between them may take the node's
  // locks. Unlike run_local_stages(), each throws what its stage throws. Rebalance's
  // rest ends on Macha's four events (maintenance_events()); the composite's, on any
  // event of the node's. A stage looks at the turn's stop as it starts and as it ends,
  // and throws contract::MaintenanceStopping having counted what it did. Within a
  // stage it falls where Macha's can, pack compaction's wait for the accounting walk
  // and its step from one backend to the next, and also between the objects of
  // rebalance and the scrub, which Macha's does not look at. A pack being rewritten and
  // the claims' compaction run to their end, as Macha's do, so what a stop interrupts
  // is whole, and the next run does the rest.
  LocalTurn begin_local_stages(const LocalTurnOptions& options = {});
  // Thread-safe; waits on nothing.
  MaintenanceEvents maintenance_events() const noexcept;
  void local_rebalance(LocalTurn& turn);
  void retention_compact(LocalTurn& turn);
  void compact_packs(LocalTurn& turn);
  void local_scrub(LocalTurn& turn);
  void ledger_verify(LocalTurn& turn);
  // The heads' stages, as run_local_stages() has them, for an owner running them in its own
  // pass. A failure is logged, as Macha's are; only a stop comes through.
  void head_repair(LocalTurn& turn);
  void history_truncate(LocalTurn& turn);
  // One turn of metadata repair on each space, first in Macha's pass.
  std::vector<heads::RepairTurn> metadata_repair(std::stop_token stop = {});

  // Takes `reload` now: a new backend is locked and opened, a removed one goes
  // offline and its lock with it, a changed limit reopens one, and the cache
  // follows at once. Throws NodeRefused, with nothing changed, for a v18 backend
  // or a root another process holds. Serialised with the maintenance passes;
  // not with a reader of options(), which it updates. Waits on the devices.
  static constexpr dannan::sched::Waits reconfigure_waits = run_maintenance_waits;
  void reconfigure(const NodeReload& reload);

  // Publishes the node's figures to context.status() now (the names are
  // listed in node.cpp, every one under "dagda."). Waits on the stores'
  // locks only.
  void publish_status();

 private:
  struct StatusTimer;
  // The GC figures Status shows (dagda.gc.*).
  struct GcFigures {
    uint64_t passes = 0;
    uint64_t skipped = 0;
    uint64_t failures = 0;
    uint64_t inventories = 0;
    uint64_t released_data = 0;
    uint64_t released_control = 0;
    uint64_t removed_control = 0;
    uint64_t examined = 0;
    uint64_t reclaimed_bytes = 0;
    uint64_t pruned = 0;
    std::string state = "not yet run";
    std::string data_gate = "none";
  };

  // The local stages' figures Status shows (dagda.maintenance.*).
  struct LocalFigures {
    uint64_t rebalance_objects = 0;
    uint64_t rebalance_bytes = 0;
    uint64_t retention_compactions = 0;
    uint64_t packs_compacted = 0;
    uint64_t scrub_objects = 0;
    uint64_t scrub_bytes = 0;
    uint64_t scrub_campaigns = 0;
    uint64_t scrub_due_unix_ms = 0;
    uint64_t verify_directories = 0;
    uint64_t verify_recorded = 0;
    uint64_t verify_lost = 0;
    uint64_t verify_passes = 0;
    uint64_t heads_repaired = 0;
    uint64_t history_truncations = 0;
  };

  void refusing(const std::function<void()>& part);
  void prepare();
  void open();
  void step(std::string_view what);
  void ready(const std::string& plane);
  void arm_status();
  void maintenance_loop(std::stop_token stop);
  void stop_maintenance();
  // One GC pass on the provider registered now. `forced`: due whatever the
  // node's activity. Whether the collector is settled: its sweep complete
  // with nothing deferred, or nothing to run.
  bool gc_pass_locked(bool forced, MaintenanceReport& report, std::stop_token stop = {}) DANNAN_REQUIRES(_maintenance_mutex);
  contract::ReachabilityProvider* registered_provider() const;
  // The collector over `provider`, made when there is none or it is over another.
  void ensure_collector_locked(contract::ReachabilityProvider& provider) DANNAN_REQUIRES(_maintenance_mutex);
  bool higher_class_active() const;
  // What rebalance and the scrub yield to within their slice.
  std::function<bool()> local_yield(const LocalTurn& turn) const;
  // The clocks a pass runs on (MaintenanceOptions::time and wall).
  const dannan::contract::TimeSource& pass_time() const;
  const dannan::contract::WallClock& pass_wall() const;
  // The local stages, in Macha's order. `forced` as run_maintenance_once says.
  // Throws only the stop: a failed stage is logged and the next pass tries again.
  LocalStages local_stages_locked(bool forced, std::stop_token stop) DANNAN_REQUIRES(_maintenance_mutex);
  LocalTurn begin_local_locked(bool forced, const LocalTurnOptions& options, uint64_t events) DANNAN_REQUIRES(_maintenance_mutex);
  void local_rebalance_locked(LocalTurn& turn) DANNAN_REQUIRES(_maintenance_mutex);
  void retention_compact_locked(LocalTurn& turn) DANNAN_REQUIRES(_maintenance_mutex);
  void compact_packs_locked(LocalTurn& turn) DANNAN_REQUIRES(_maintenance_mutex);
  void local_scrub_locked(LocalTurn& turn) DANNAN_REQUIRES(_maintenance_mutex);
  void ledger_verify_locked(LocalTurn& turn) DANNAN_REQUIRES(_maintenance_mutex);
  void head_repair_locked(LocalTurn& turn) DANNAN_REQUIRES(_maintenance_mutex);
  void history_truncate_locked(LocalTurn& turn) DANNAN_REQUIRES(_maintenance_mutex);
  std::vector<heads::RepairTurn> metadata_repair_locked(std::stop_token stop, bool forced) DANNAN_REQUIRES(_maintenance_mutex);
  void enter_stage(std::string_view stage) const;
  void wake_maintenance();
  void note_local(const std::function<void(LocalFigures&)>& change);
  void persist_scrub_due() DANNAN_REQUIRES(_maintenance_mutex);

  dannan::Context& _context;
  // The DATA pool is opening: a refusal now is Macha's data storage recovery failing.
  bool _opening_data = false;
  NodeOptions _options;
  const dannan::status::ScopedLogger _logger;
  dannan::supervise::StartupProgress _progress;
  std::vector<std::string> _opened;

  // Takes `root`'s StorageLock once and keeps it until stop; throws if another holds
  // it. Never called for a path that is not a directory: the lock would make it.
  void hold_root(const std::filesystem::path& root) DANNAN_EXCLUDES(_root_locks_mutex);

  std::vector<std::unique_ptr<store::StorageLock>> _locks;
  // The roots a reload can change, by path: the cache's, and each backend's once its
  // directory is there (the pool takes one through hold_root as a disk comes
  // online). Released with the rest at stop. A leaf lock: the pool calls hold_root
  // under a reload's _maintenance_mutex.
  mutable dannan::contract::Mutex _root_locks_mutex;
  std::map<std::filesystem::path, std::unique_ptr<store::StorageLock>> _reloadable_locks DANNAN_GUARDED_BY(_root_locks_mutex);
  std::shared_ptr<const domain::StorageKey> _node_local;
  std::optional<domain::WrapKey> _wrap;
  std::unique_ptr<store::StoragePool> _data;
  std::unique_ptr<store::DataResourceArbiter> _credit;
  std::unique_ptr<store::IoReserve> _io;
  std::unique_ptr<dannan::status::ActivityClocks> _activity;
  std::unique_ptr<store::LocalStore> _control;
  std::unique_ptr<store::PersistentBlockCache> _cache;
  std::unique_ptr<ledger::RetentionStore> _claims;
  std::unique_ptr<ledger::RetentionLedger> _ledger;
  std::unique_ptr<ledger::ReferenceCounts> _references;
  std::unique_ptr<store::DistributedStore> _store;
  std::unique_ptr<heads::Spaces> _spaces;
  std::unique_ptr<store::AvailabilityService> _availability;
  std::unique_ptr<store::Repair> _repair;
  std::unique_ptr<store::StorageServer> _server;

  std::unique_ptr<lease::RosterMembership> _membership;
  std::unique_ptr<lease::LeaseKeeper> _keeper;
  lease::LocalLeaseRecords _lease_records;
  std::unique_ptr<Context> _api;

  // The status timer's hold on the node: cleared, under its lock, at stop.
  std::shared_ptr<StatusTimer> _status;

  // Lock order: _maintenance_mutex, then _providers_mutex, then
  // _gc_figures_mutex. A pass holds _maintenance_mutex across the devices
  // and the provider.
  mutable dannan::contract::Mutex _providers_mutex;
  std::map<domain::DomainId, contract::ReachabilityProvider*> _providers DANNAN_GUARDED_BY(_providers_mutex);

  dannan::status::NodeEvents::Kind _storage_event = 0;
  dannan::status::NodeEvents::Kind _metadata_event = 0;
  dannan::status::NodeEvents::Kind _claims_event = 0;

  dannan::contract::IoMutex _maintenance_mutex;
  std::unique_ptr<ledger::UnreferencedSince> _data_sightings DANNAN_GUARDED_BY(_maintenance_mutex);
  std::unique_ptr<ledger::UnreferencedSince> _control_sightings DANNAN_GUARDED_BY(_maintenance_mutex);
  // The collector over the provider it was made for, remade when another is
  // registered.
  std::unique_ptr<ledger::Collector> _collector DANNAN_GUARDED_BY(_maintenance_mutex);
  contract::ReachabilityProvider* _collector_provider DANNAN_GUARDED_BY(_maintenance_mutex) = nullptr;

  mutable dannan::contract::Mutex _gc_figures_mutex;
  GcFigures _gc_figures DANNAN_GUARDED_BY(_gc_figures_mutex);
  LocalFigures _local_figures DANNAN_GUARDED_BY(_gc_figures_mutex);

  // The local stages' pacing.
  std::optional<store::CpuPacer> _local_cpu DANNAN_GUARDED_BY(_maintenance_mutex);
  std::optional<dannan::contract::TimePoint> _local_at DANNAN_GUARDED_BY(_maintenance_mutex);
  double _local_credit DANNAN_GUARDED_BY(_maintenance_mutex) = 0.0;
  double _scrub_credit DANNAN_GUARDED_BY(_maintenance_mutex) = 0.0;
  uint64_t _scrub_due_unix_ms DANNAN_GUARDED_BY(_maintenance_mutex) = 0;
  // Rebalance found nothing to do at this event count, and waits for another.
  std::optional<uint64_t> _local_quiescent_events DANNAN_GUARDED_BY(_maintenance_mutex);
  double _verify_credit DANNAN_GUARDED_BY(_maintenance_mutex) = 0.0;
  std::optional<dannan::contract::TimePoint> _verify_at DANNAN_GUARDED_BY(_maintenance_mutex);
  dannan::contract::TimePoint _verify_due DANNAN_GUARDED_BY(_maintenance_mutex){};

  // A metadata event since the thread last looked: it wakes for those, and polls the rest.
  std::atomic_bool _maintenance_kicked{false};
  // Guards nothing; the maintenance thread waits on it.
  std::mutex _maintenance_wait;
  std::condition_variable_any _maintenance_wake;
  std::jthread _maintenance;
};

}  // namespace dagda
