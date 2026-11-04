//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/contract/reachability.h"
#include "dagda/domain/domain.h"
#include "dagda/domain/storage_key.h"
#include "dagda/domain/wrap.h"
#include "dagda/lease/keeper.h"
#include "dagda/lease/local_records.h"
#include "dagda/lease/membership.h"
#include "dagda/ledger/reference_counts.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/retention_ledger.h"
#include "dagda/store/availability.h"
#include "dagda/store/control_store.h"
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

// The readiness planes the node reports, Macha's names: the DATA pool, the
// CONTROL store, the cache, and the claims with the reference counts. An
// owner names them in dannan::NodeConfig::planes, since Dannan's planes are
// declared when its node is made (heads add "metadata" at step 4).
std::vector<std::string> readiness_planes();

// The node's DATA admission (Macha's data_inflight_bytes and its foreground
// reserve, its background concurrency and no-progress window) and its device
// I/O reserve.
struct AdmissionOptions {
  uint64_t data_inflight_bytes = 128ULL * 1024 * 1024;
  uint64_t foreground_reserve_bytes = 32ULL * 1024 * 1024;
  uint64_t background_concurrency = 4;
  std::chrono::milliseconds no_progress{std::chrono::seconds(30)};
  // Background work admitted while the DATA device is slow.
  uint64_t min_background_under_pressure = 1;
  store::IoReserveOptions io{};
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
  // storage-pool"), as the node logs it.
  std::function<void(std::string_view)> trace;
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
// key and the wrap key (a node without custody is refused), the DATA pool and
// its admission, the control store, the cache, the claims and the reference
// counts, the distributed store, availability, repair and the storage
// server. Each readiness plane is reported ready as its part opens; a pool
// that holds nothing it should (every backend offline) leaves data_storage
// unready. The node then publishes its figures to Dannan's Status surface,
// at once and every status_interval. stop() closes it in the reverse order,
// admission first so a waiting request gives up, and releases the locks
// last.
//
// The owner's reachability provider is registered per domain and builds the
// inventory repair and the survey work from; the collector's pass and the
// maintenance loop that drives repair stay the owner's (Macha's, at the
// port).
//
// Construction and stop() wait on the devices and must not run on one of the
// io context's threads; everything else is thread-safe unless it says so.
class Node {
 public:
  using Front = decltype(NodeOptions::front);

  // Throws NodeRefused, with a reason, for a Dannan contract of another
  // version, a Dannan node already started, a held lock, a refused layout, a
  // node without custody, an undeclared readiness plane, or a part that
  // cannot open.
  Node(dannan::Context& context, NodeOptions options);
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
  store::IoReserve* io() noexcept { return _io.get(); }
  const domain::WrapKey& wrap_key() const noexcept { return *_wrap; }
  lease::LeaseKeeper& lease_keeper() noexcept { return *_keeper; }
  lease::LeaseRecords& lease_records() noexcept { return _lease_records; }

  // The owner's reachability provider for a domain (requirement C3): what
  // its state refers to, from which the inventory is built. One per domain
  // until several are unioned (step 6, with the catalogue); registering
  // again replaces it. Throws std::invalid_argument for a domain this node
  // does not hold. The provider outlives the node or is unregistered first.
  void register_reachability(const domain::DomainId& domain, contract::ReachabilityProvider& provider);
  void unregister_reachability(const domain::DomainId& domain);
  // Builds the inventory from the provider of the node's domain at its
  // current generation and publishes it to the ledger, as the collector
  // does at the start of its pass: what the survey and repair take as
  // referenced. False when no provider is registered. Throws what the
  // provider throws. Single owner (the owner's maintenance).
  static constexpr dannan::sched::Waits publish_inventory_waits = contract::ReachabilityProvider::answer_waits;
  bool publish_inventory();

  // Publishes the node's figures to context.status() now (the names are
  // listed in node.cpp, every one under "dagda."). Waits on the stores'
  // locks only.
  void publish_status();

 private:
  struct StatusTimer;

  void open();
  void step(std::string_view what);
  void ready(const std::string& plane);
  void arm_status();

  dannan::Context& _context;
  NodeOptions _options;
  const dannan::status::ScopedLogger _logger;
  dannan::supervise::StartupProgress _progress;
  std::vector<std::string> _opened;

  std::vector<std::unique_ptr<store::StorageLock>> _locks;
  std::shared_ptr<const domain::StorageKey> _node_local;
  std::optional<domain::WrapKey> _wrap;
  std::unique_ptr<store::StoragePool> _data;
  std::unique_ptr<store::DataResourceArbiter> _credit;
  std::unique_ptr<store::IoReserve> _io;
  std::unique_ptr<store::LocalStore> _control;
  std::unique_ptr<store::PersistentBlockCache> _cache;
  std::unique_ptr<ledger::RetentionStore> _claims;
  std::unique_ptr<ledger::RetentionLedger> _ledger;
  std::unique_ptr<ledger::ReferenceCounts> _references;
  std::unique_ptr<store::DistributedStore> _store;
  std::unique_ptr<store::AvailabilityService> _availability;
  std::unique_ptr<store::Repair> _repair;
  std::unique_ptr<store::StorageServer> _server;

  std::unique_ptr<lease::RosterMembership> _membership;
  std::unique_ptr<lease::LeaseKeeper> _keeper;
  lease::LocalLeaseRecords _lease_records;
  std::unique_ptr<Context> _api;

  // The status timer's hold on the node: cleared, under its lock, at stop.
  std::shared_ptr<StatusTimer> _status;

  mutable dannan::contract::Mutex _providers_mutex;
  std::map<domain::DomainId, contract::ReachabilityProvider*> _providers DANNAN_GUARDED_BY(_providers_mutex);
};

}  // namespace dagda
