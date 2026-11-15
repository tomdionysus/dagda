//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The harness's Macha half: Macha's store at f0597a4, built from Dagda's read-only
// extract (build/macha-f0597a4) against its macha_core library, on Dannan 0.5.3. Two
// nodes in one process over Dannan on loopback, each as the extract's tests'
// BareNode builds one (tests/test_support.hpp:417-568 at f0597a4), copied here
// without its accounts and test framework, which the store does not use; the
// DistributedStore as distributed_store_for builds it (:564-568). Configured as the
// Macha session asked: replicas 2, write_copies 1, 4 MiB extents, packing at 1 MiB
// into 64 MiB packs, a 500 ms durability window; Macha's defaults otherwise.
//
//   harness-macha <workload>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include "cluster/cluster.hpp"
#include "cluster/distributed_store.hpp"
#include "cluster/local_services.hpp"
#include "cluster/node_resources.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "harness.h"
#include "supervised.hpp"

namespace {

using namespace std::chrono_literals;
using namespace macha;

FrameType frame_of(harness::Frame frame) {
  switch (frame) {
    case harness::Frame::foreground:
      return FrameType::foreground;
    case harness::Frame::read_ahead:
      return FrameType::read_ahead;
    case harness::Frame::loader:
      return FrameType::loader;
    case harness::Frame::speculative:
      return FrameType::speculative;
  }
  return FrameType::loader;
}

ObjectId object_of(const harness::Id& id) {
  ObjectId out;
  std::copy(id.bytes.begin(), id.bytes.end(), out.bytes.begin());
  return out;
}

// A loopback port nothing holds now (the extract's free_port, without its test
// case blocks).
uint16_t free_port() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("cannot open a socket to find a port");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  socklen_t length = sizeof(addr);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
    close(fd);
    throw std::runtime_error("cannot bind a loopback port");
  }
  close(fd);
  return ntohs(addr.sin_port);
}

// The extract's config_for (tests/test_support.hpp:260-312) in its functional
// profile, with the harness's values.
Config config_for(const std::filesystem::path& path, const std::filesystem::path& key, uint16_t port, std::vector<Endpoint> bootstrap) {
  std::filesystem::create_directories(path);
  Config c;
  c.state_path = path.parent_path() / (path.filename().string() + ".state");
  c.storage_backends = {{path, 16ULL * 1024 * 1024 * 1024}};
  c.key_file = key;
  c.listen_host = "127.0.0.1";
  c.advertise_host = "127.0.0.1";
  c.port = port;
  c.extent_size = harness::extent_bytes;
  c.storage_packing.threshold = 1024 * 1024;
  c.storage_packing.target_size = 64ULL * 1024 * 1024;
  c.replication = 2;
  c.write_copies = 1;
  c.metadata_write_copies = 2;
  c.fuse.spool_reserve_free = 0;
  c.hydration.enabled = false;
  c.dead_after = 30s;
  c.connect_timeout = 2500ms;
  c.bootstrap = std::move(bootstrap);
  c.plugin_path = std::filesystem::path{};
  c.read_ahead_extents = 2;
  c.heartbeat = 1s;
  return c;
}

// The extract's BareNodeResources and BareNode (tests/test_support.hpp:417-560),
// without the accounts.
struct NodeParts {
  StorageLock state_lock;
  NodeIdentity identity;
  RecoveryProgress progress;
  MessageRoutes routes;
  LocalState::StageHook stage_hook;
  NodeBooks books;
  NodeParts(const Config& config, ClusterKeys keys) : state_lock(config.state_path), identity(config.state_path, std::move(keys)), books(config) {}
};

class HarnessNode : public NodeParts, public NodeRuntime {
 public:
  NodeResources resources;

  HarnessNode(Config config, ClusterKeys keys)
      : NodeParts(config, keys),
        NodeRuntime(std::move(config), identity, progress, books.transcode_rates, routes, books.events, {}),
        resources(NodeRuntime::config(), books, node().memory()) {}
  ~HarnessNode() { stop(); }
  HarnessNode(const HarnessNode&) = delete;
  HarnessNode& operator=(const HarnessNode&) = delete;

  void start() {
    NodeRuntime::start();
    if (_recovery.joinable()) return;
    _recovery = std::jthread([this](std::stop_token stop) {
      run_supervised_once("cluster-state-recovery", [this, stop] {
        try {
          _local = std::make_unique<LocalServices>(config(), identity, progress, stage_hook, stop, *this, resources, routes, 500ms);
        } catch (const std::exception&) {
          return;
        }
        progress.mark_complete(unix_ms());
        signal_telemetry_refresh();
      });
    });
  }
  void stop() {
    _store.reset();
    resources.stop();
    NodeRuntime::stop();
    if (_recovery.joinable()) {
      _recovery.request_stop();
      _recovery.join();
    }
  }
  LocalState& local_state() {
    if (!progress.wait_complete(std::chrono::minutes(10))) throw std::runtime_error("local state is still recovering");
    return _local->state();
  }
  // As the extract's distributed_store_for builds it.
  DistributedStore& store() {
    if (!_store) _store = std::make_unique<DistributedStore>(*this, local_state(), resources.activity, resources.data, resources.memory, resources.events);
    return *_store;
  }

 private:
  std::unique_ptr<LocalServices> _local;
  std::jthread _recovery;
  std::unique_ptr<DistributedStore> _store;
};

class MachaStore final : public harness::Store {
 public:
  explicit MachaStore(const std::filesystem::path& root) : _root(root) {
    std::filesystem::remove_all(_root);
    std::filesystem::create_directories(_root);
    const auto key = _root / "cluster.key";
    {
      // One key for this run, so no other process's node joins.
      std::random_device random;
      std::ofstream out(key, std::ios::binary | std::ios::trunc);
      out << "macha harness cluster key " << random() << random() << '\n';
    }
    const auto keys = load_cluster_keys(key);
    const auto first = free_port();
    _nodes.push_back(std::make_unique<HarnessNode>(config_for(_root / "a", key, first, {}), keys));
    _nodes.push_back(std::make_unique<HarnessNode>(config_for(_root / "b", key, free_port(), {Endpoint{"127.0.0.1", first}}), keys));
    for (auto& node : _nodes) node->start();
    for (auto& node : _nodes) (void)node->store();
    // Both nodes active, each with its capacity known, as the extract's tests wait.
    const auto until = harness::Clock::now() + 2min;
    while (harness::Clock::now() < until) {
      const auto active = _nodes[0]->membership().active();
      if (active.size() == 2 && std::all_of(active.begin(), active.end(), [](const NodeInfo& node) { return node.capacity > 0; })) return;
      std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("the two nodes did not converge");
  }
  ~MachaStore() override {
    for (auto& node : _nodes) node->stop();
    _nodes.clear();
    std::error_code ignored;
    std::filesystem::remove_all(_root, ignored);
  }

  std::string describe() const override { return "macha f0597a4 on dannan 0.5.3"; }
  size_t nodes() const override { return _nodes.size(); }
  harness::Id id_of(std::span<const uint8_t> data) const override {
    harness::Id out;
    const auto id = object_id(data);
    std::copy(id.bytes.begin(), id.bytes.end(), out.bytes.begin());
    return out;
  }

  bool put(size_t node, const harness::Id& id, std::span<const uint8_t> data, harness::Frame frame) override {
    return store(node).put(object_of(id), data, frame_of(frame));
  }

  class MachaPublication final : public Publication {
   public:
    MachaPublication(DistributedStore& store, harness::Frame frame) : _store(store), _frame(frame_of(frame)) {}
    bool extent(const harness::Id& id, std::span<const uint8_t> data) override { return _store.put_deferred(object_of(id), data, _batch, _frame); }
    bool finish() override { return _store.durability_barrier(_batch, _frame); }

   private:
    DistributedStore& _store;
    FrameType _frame;
    DistributedStore::DurabilityBatch _batch;
  };
  std::unique_ptr<Publication> publication(size_t node, harness::Frame frame) override { return std::make_unique<MachaPublication>(store(node), frame); }

  // As Macha's readers read (FUSE's ReadHandle, the media inputs): the shared buffer,
  // copied once.
  std::optional<harness::Bytes> read(size_t node, const harness::Id& id, size_t stripe, harness::Frame frame, std::chrono::milliseconds deadline) override {
    const auto shared = store(node).get_shared(object_of(id), stripe, frame_of(frame), Clock::now() + deadline);
    if (!shared) return std::nullopt;
    return harness::Bytes(shared->bytes.begin(), shared->bytes.end());
  }

  bool remove_local(size_t node, const harness::Id& id) override { return _nodes.at(node)->local_state().data().remove(object_of(id)); }
  bool holds(size_t node, const harness::Id& id) override { return _nodes.at(node)->local_state().data().has(object_of(id)); }
  size_t compact(size_t node) override { return static_cast<size_t>(_nodes.at(node)->local_state().data().compact_packs(std::stop_token{})); }
  // Macha's pool counts no barriers of its own: the figure comes from strace on a
  // rerun instead (the plan), so this reads zero.
  uint64_t barriers(size_t) const override { return 0; }

 private:
  DistributedStore& store(size_t node) { return _nodes.at(node)->store(); }

  std::filesystem::path _root;
  std::vector<std::unique_ptr<HarnessNode>> _nodes;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "harness-macha <state directory> <workload>\n");
    return 2;
  }
  try {
    MachaStore store(argv[1]);
    return harness::run(store, argv[2]);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "harness-macha: %s\n", error.what());
    return 1;
  }
}
