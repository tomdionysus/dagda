//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// An in-process cluster of Dagda nodes on Dannan's node: each a
// dannan::Node on loopback with its own state, DATA pool, control store,
// cache, claims, storage server and distributed store. What Macha's
// StoreBench and StorageClusterNode were (tests/test_backend_support.hpp,
// develop 5a06d3c), on real Dannan nodes rather than a fake transport: a
// test interposes on a node's requests (to hold, refuse or count them) in
// front of its storage server.

#include <gtest/gtest.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dagda/ledger/retention.h"
#include "dagda/store/control_store.h"
#include "dagda/store/distributed_store.h"
#include "dagda/store/messages.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/placement.h"
#include "dagda/store/storage_pool.h"
#include "dagda/store/storage_server.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/node.h"
#include "dannan/supervise/startup.h"
#include "dannan/test/fixtures/ports.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::test {

// The io context every node of a cluster runs on, on four threads.
struct ClusterIo {
  boost::asio::io_context context;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{context.get_executor()};
  std::vector<std::thread> threads;
  ClusterIo() {
    for (int i = 0; i < 4; ++i) threads.emplace_back([this] { context.run(); });
  }
  ~ClusterIo() {
    work.reset();
    context.stop();
    for (auto& thread : threads) thread.join();
  }
  ClusterIo(const ClusterIo&) = delete;
  ClusterIo& operator=(const ClusterIo&) = delete;
};

// What one node runs with. Macha's test defaults where it had them: a 64 MiB
// backend, loose objects only, a short durability window so a batched
// barrier does not dominate a case.
struct ClusterNodeOptions {
  uint64_t limit = 64ULL * 1024 * 1024;
  bool hosts_extents = true;
  bool packed = false;
  std::chrono::milliseconds durability_window{20};
  std::string failure_domain;
  store::CacheOptions cache;
  store::DistributedStoreOptions store;
};

// Answers a request in the server's place when it returns a value; nothing
// lets the server answer. It may block (a held peer) or count.
using Interpose = std::function<std::optional<dannan::net::Handled>(dannan::net::MessageType, const dannan::membership::NodeRecord& peer,
                                                                    const dannan::net::Message& request)>;

class Cluster;

// One node. Its directories, port and node id outlive restarts; its
// process (the durability epoch) does not.
class ClusterNode {
 public:
  ClusterNode(Cluster& cluster, std::string name, ClusterNodeOptions options);
  ~ClusterNode() { stop(); }
  ClusterNode(const ClusterNode&) = delete;
  ClusterNode& operator=(const ClusterNode&) = delete;

  // Builds the node and its store, then starts the node, which bootstraps
  // from every other node's port.
  void start();
  // Stops the store, then the node, then closes the stores, as a process
  // exiting cleanly would.
  void stop();
  void restart() {
    stop();
    start();
  }
  bool running() const { return _node != nullptr; }

  dannan::Node& node() { return *_node; }
  dannan::Context& context() { return _node->context(); }
  dannan::identity::NodeId id() const { return _id; }
  uint16_t port() const { return _port; }
  const std::string& name() const { return _name; }
  store::DistributedStore& store() { return *_store; }
  store::StorageServer& server() { return *_server; }
  store::StoragePool& data() { return *_data; }
  store::LocalStore& control() { return *_control; }
  store::PersistentBlockCache& cache() { return *_cache; }
  ledger::RetentionStore& claims() { return *_claims; }
  std::filesystem::path backend() const { return _dir / "data"; }
  ClusterNodeOptions& options() { return _options; }

  // Requests of the store's types this node was asked, and the hook in
  // front of its server.
  size_t calls_of(dannan::net::MessageType type) const {
    dannan::contract::Lock lock(_mutex);
    const auto found = _calls.find(type);
    return found == _calls.end() ? 0 : found->second;
  }
  void interpose(Interpose hook) {
    dannan::contract::Lock lock(_mutex);
    _interpose = std::move(hook);
  }

 private:
  Cluster& _cluster;
  const std::string _name;
  ClusterNodeOptions _options;
  const std::filesystem::path _dir;
  const uint16_t _port;
  dannan::identity::NodeId _id;
  dannan::supervise::StartupProgress _progress;
  std::shared_ptr<RecordingSink> _sink = std::make_shared<RecordingSink>();
  std::unique_ptr<dannan::Node> _node;
  std::unique_ptr<store::StoragePool> _data;
  std::unique_ptr<store::LocalStore> _control;
  std::unique_ptr<store::PersistentBlockCache> _cache;
  std::unique_ptr<ledger::RetentionStore> _claims;
  std::unique_ptr<store::StorageServer> _server;
  std::unique_ptr<store::DistributedStore> _store;
  mutable dannan::contract::Mutex _mutex;
  std::map<dannan::net::MessageType, size_t> _calls DANNAN_GUARDED_BY(_mutex);
  Interpose _interpose DANNAN_GUARDED_BY(_mutex);
};

// Nodes on one io context under one temporary directory, with one cluster
// key. Heartbeats are short, so a stopped node is dead to the others within
// dead_after.
class Cluster {
 public:
  explicit Cluster(std::chrono::milliseconds dead_after = std::chrono::milliseconds(1500)) : _dead_after(dead_after) {
    std::ofstream(_dir.path() / "cluster.key") << "dagda cluster test";
    _key = dannan::identity::load_cluster_key(_dir.path() / "cluster.key");
  }
  ~Cluster() {
    // Every node stops before the io context it runs on.
    for (auto& node : _nodes) node->stop();
  }

  // Added, not started.
  ClusterNode& add(std::string name, ClusterNodeOptions options = {}) {
    _nodes.push_back(std::make_unique<ClusterNode>(*this, std::move(name), std::move(options)));
    return *_nodes.back();
  }
  // Added and started, each in order.
  void start_all() {
    for (auto& node : _nodes)
      if (!node->running()) node->start();
  }
  std::vector<std::unique_ptr<ClusterNode>>& nodes() { return _nodes; }
  ClusterNode& operator[](size_t i) { return *_nodes.at(i); }

  // Every running node sees every running node active, each hosting node
  // with its adverts, and every running node the same capacities: placement
  // agrees everywhere.
  bool converged(std::chrono::milliseconds within = std::chrono::seconds(15)) {
    const bool ok = converged_once(within);
    if (!ok)
      for (auto& node : _nodes) {
        if (!node->running()) continue;
        for (const auto& record : node->context().nodes().active())
          ADD_FAILURE() << node->name() << " sees " << dannan::identity::to_string(record.id) << " adverts=" << record.adverts.size()
                        << " extents=" << record.advert(store::extents_advert).has_value()
                        << " capacity=" << record.advert_u64(store::capacity_advert).value_or(0);
      }
    return ok;
  }
  bool converged_once(std::chrono::milliseconds within) {
    return wait_until(
        [&] {
          std::vector<ClusterNode*> running;
          for (auto& node : _nodes)
            if (node->running()) running.push_back(node.get());
          std::optional<std::map<dannan::identity::NodeId, std::pair<bool, uint64_t>>> first;
          for (auto* observer : running) {
            std::map<dannan::identity::NodeId, std::pair<bool, uint64_t>> view;
            for (const auto& record : observer->context().nodes().active())
              view[record.id] = {record.advert(store::extents_advert).has_value(), record.advert_u64(store::capacity_advert).value_or(0)};
            for (auto* other : running) {
              const auto found = view.find(other->id());
              if (found == view.end()) return false;
              if (found->second.first != other->options().hosts_extents) return false;
              if (other->options().hosts_extents && !found->second.second) return false;
            }
            if (view.size() != running.size()) return false;
            if (first && *first != view) return false;
            first = view;
          }
          return true;
        },
        within);
  }
  // `observer` no longer counts `gone` among the active nodes.
  bool dead_to(ClusterNode& observer, const dannan::identity::NodeId& gone, std::chrono::milliseconds within = std::chrono::seconds(15)) {
    return wait_until(
        [&] {
          for (const auto& record : observer.context().nodes().active())
            if (record.id == gone) return false;
          return true;
        },
        within);
  }

  size_t calls_of(dannan::net::MessageType type) const {
    size_t total = 0;
    for (const auto& node : _nodes) total += node->calls_of(type);
    return total;
  }

  ClusterIo& io() { return _io; }
  const std::filesystem::path& dir() const { return _dir.path(); }
  const dannan::identity::ClusterKey& key() const { return _key; }
  std::chrono::milliseconds dead_after() const { return _dead_after; }
  std::vector<dannan::net::Endpoint> seeds_for(const ClusterNode& node) const {
    std::vector<dannan::net::Endpoint> out;
    for (const auto& other : _nodes)
      if (other.get() != &node) out.push_back({"127.0.0.1", other->port()});
    return out;
  }

 private:
  dannan::test::TempDir _dir;
  dannan::identity::ClusterKey _key;
  const std::chrono::milliseconds _dead_after;
  ClusterIo _io;
  std::vector<std::unique_ptr<ClusterNode>> _nodes;
};

inline ClusterNode::ClusterNode(Cluster& cluster, std::string name, ClusterNodeOptions options)
    : _cluster(cluster), _name(std::move(name)), _options(std::move(options)), _dir(cluster.dir() / _name), _port(dannan::test::free_port()) {
  std::filesystem::create_directories(_dir);
}

inline void ClusterNode::start() {
  if (_node) return;
  dannan::NodeConfig config;
  config.state_path = _dir / "node";
  config.key = _cluster.key();
  config.listen_host = "127.0.0.1";
  config.advertise_host = "127.0.0.1";
  config.port = _port;
  config.node_name = _name;
  // Loopback nodes are independent failure domains, as Macha's test nodes
  // are, unless a case says otherwise.
  config.failure_domain = _options.failure_domain.empty() ? _name : _options.failure_domain;
  config.bootstrap = _cluster.seeds_for(*this);
  config.inbound = dannan::reach::Inbound::yes;
  config.heartbeat = std::chrono::milliseconds(100);
  config.dead_after = _cluster.dead_after();
  config.telemetry_interval = std::chrono::milliseconds(200);
  config.connect_timeout = std::chrono::milliseconds(1000);
  config.version = "dagda-test";
  config.log = _sink;
  _node = std::make_unique<dannan::Node>(_cluster.io().context, config);
  auto& context = _node->context();
  _id = context.identity().id();
  store::message::register_types(context.routes());

  const auto key = known_answer_key();
  const store::LocalStore::Services services{context.threads(),
                                             context.observations(),
                                             _progress,
                                             context.logger("dagda.pool"),
                                             dannan::contract::steady_time_source(),
                                             dannan::contract::system_wall_clock()};
  store::StoragePoolOptions pool;
  if (_options.hosts_extents) {
    std::filesystem::create_directories(backend());
    pool.backends.push_back({backend(), _options.limit, 0});
  }
  pool.durability_batch_window = _options.durability_window;
  if (!_options.packed) {
    pool.pack_threshold = 0;
    pool.pack_target_size = 0;
  }
  _data = std::make_unique<store::StoragePool>(_dir / "state", _id, context.identity().durability_epoch(), pool, key, services);
  _control = store::open_control_store(_dir / "state", {}, key, services);
  _cache = std::make_unique<store::PersistentBlockCache>(_options.cache, key, services);
  _claims = std::make_unique<ledger::RetentionStore>(_dir / "state", key, context.logger("dagda.claims"));

  store::StorageServerOptions server;
  server.extent_size = _options.store.extent_size;
  server.hosts_extents = _options.hosts_extents;
  server.refresh_interval = std::chrono::milliseconds(500);
  _server = std::make_unique<store::StorageServer>(context, store::StorageServer::Stores{*_data, *_control, *_claims}, server);
  // In front of the server: count, then let the test answer instead.
  for (const auto type :
       {store::message::have_object, store::message::get_object, store::message::put_object, store::message::get_control_object,
        store::message::put_control_object, store::message::put_object_deferred, store::message::object_durability_barrier, store::message::retain_objects,
        store::message::have_objects, store::message::have_control_objects, store::message::durability_covered})
    context.routes().bind(type, [this](const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame, const dannan::net::Message& request) {
      Interpose hook;
      {
        dannan::contract::Lock lock(_mutex);
        ++_calls[request.type];
        hook = _interpose;
      }
      if (hook)
        if (auto answer = hook(request.type, peer, request)) return std::move(*answer);
      return _server->handle(peer, frame, request);
    });
  _store = std::make_unique<store::DistributedStore>(context, _cluster.io().context.get_executor(),
                                                     store::DistributedStore::Stores{*_data, *_control, *_cache, *_claims}, _options.store);
  _node->start();
}

inline void ClusterNode::stop() {
  if (!_node) return;
  _store.reset();
  _server.reset();
  _node->stop();
  _claims.reset();
  _cache.reset();
  _control.reset();
  _data.reset();
  _node.reset();
}

}  // namespace dagda::test
