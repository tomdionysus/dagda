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
//   harness-macha <state directory> <workload>
//   (recovery-fill and recovery-fill-small keep the state directory; recovery-open
//   reopens node "a" of it alone)
//   harness-macha <directory> repair-node <name> <host> <port> [<seed host> <seed port>]
//   (workload 7's node: a whole Service, the cluster key read from <directory>/cluster.key)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "cluster/cluster.hpp"
#include "cluster/distributed_store.hpp"
#include "cluster/local_services.hpp"
#include "cluster/node_resources.hpp"
#include "config.hpp"
#include "crypto.hpp"
#include "filesystem/filesystem.hpp"
#include "harness.h"
#include "service/service.hpp"
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
  // `keep`: the root outlives the run (recovery's fill), for a later run to reopen.
  explicit MachaStore(const std::filesystem::path& root, bool keep = false) : _root(root), _keep(keep) {
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
    if (_keep) return;
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
  bool _keep = false;
  std::vector<std::unique_ptr<HarnessNode>> _nodes;
};

// Recovery's reopen: node "a" alone over what the fill left under `root`, its key and
// config as the fill's, timed from its construction (the Macha session's points,
// 2026-11-16): data open is RecoveryProgress's data_storage plane, marked after
// StoragePool's constructor and its pack replay; ready is RecoveryProgress complete
// (local_state_ready), as /health and the peers see it; then one foreground read of a
// random extent of the fill's, the store built as distributed_store_for builds it.
void recovery_open(const std::filesystem::path& root, const harness::Report& report) {
  const auto ids = harness::read_ids(root / "ids");
  const auto key = root / "cluster.key";
  const auto keys = load_cluster_keys(key);
  harness::Reopened out;
  const auto before = harness::process_figures();
  const auto read_before = harness::io_field("read_bytes");
  const auto wall_started =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  const auto started = harness::Clock::now();
  auto node = std::make_unique<HarnessNode>(config_for(root / "a", key, free_port(), {}), keys);
  std::atomic<bool> stop{false};
  std::thread watch([&] {
    while (!stop.load()) {
      if (node->progress.has(RecoveryProgress::data_storage)) {
        out.data_open = harness::Clock::now() - started;
        return;
      }
      std::this_thread::sleep_for(1ms);
    }
  });
  node->start();
  const bool ready = node->progress.wait_complete(10min);
  stop = true;
  watch.join();
  if (ready) {
    out.ready = std::chrono::milliseconds(node->progress.ready_unix_ms() - wall_started);
    const auto at_ready = harness::process_figures();
    out.cpu_to_ready = at_ready.cpu_seconds - before.cpu_seconds;
    out.disk_read_to_ready = harness::io_field("read_bytes") - read_before;
    std::random_device random;
    const auto& id = ids[random() % ids.size()];
    auto& store = node->store();
    out.store_built = harness::Clock::now() - started;
    const auto shared = store.get_shared(object_of(id), 0, FrameType::foreground, Clock::now() + 60s);
    out.read_ok = shared != nullptr;
    out.first_read = harness::Clock::now() - started;
  } else {
    out.failure = node->progress.failed() ? "recovery failed: " + node->progress.error() : "not ready within 10 minutes";
  }
  node->stop();
  harness::report_reopen(out, report);
}

// Workload 7's node (the Macha session's review, 2026-11-16): Macha's repair lives in
// its maintenance pass, which takes what is referenced from the node's own metadata, so
// the node is a whole Service as the extract's TestService builds one
// (tests/test_support.hpp:347-405) on the real clock, each object a file of one extent
// written through its filesystem. Its values are the live nodes' (macnessa and inverbeg,
// read by the Macha session the same day): replicas 2, write_copies 1,
// metadata_write_copies 1, 4 MiB extents, a 5 s heartbeat, 30 s dead_after, a 2.5 s
// connect timeout, 10 s telemetry, repair's idle_bandwidth_fraction 0.5 and every other
// maintenance value its default. The isolated profile's switches keep the rest quiet:
// no catalogue scanner or API, no ingest, torrent, hydration or plugins. Packing is the
// harness's, as Dagda's side: 1 MiB into 64 MiB packs.
class MachaRepairNode final : public harness::RepairNode {
 public:
  MachaRepairNode(const std::filesystem::path& dir, const std::string& name, const std::string& host, uint16_t port, std::vector<Endpoint> seeds) {
    const auto key = dir / "cluster.key";
    const bool joining = !seeds.empty();
    if (!std::filesystem::exists(key)) throw std::runtime_error("no cluster key at " + key.string() + ": the script writes one and copies it to both machines");
    auto config = config_for(dir / name, key, port, std::move(seeds));
    config.listen_host = host;
    config.advertise_host = host;
    config.metadata_write_copies = 1;
    config.heartbeat = 5s;
    config.telemetry_interval = 10s;
    config.maintenance.idle_bandwidth_fraction = 0.5;
    config.catalogue.scanner.enabled = false;
    config.catalogue.api.enabled = false;
    config.ingest.enabled = false;
    config.torrent.enabled = false;
    _service = std::make_unique<Service>(config, load_cluster_keys(key), 500ms);
    _service->start();
    auto& files = _service->filesystem();
    // Only the node that starts the cluster makes the directory: a joining node's
    // metadata is not writable until it has met its bootstrap peer, and it gets the
    // directory with everything else (a baseline run was lost to this, 2026-11-16).
    if (!joining) {
      try {
        files.mkdir(directory, 0755, getuid(), getgid());
      } catch (const FsError& error) {
        if (error.code() != EEXIST) throw;
      }
    }
  }
  ~MachaRepairNode() override { _service->stop(); }

  std::string describe() const override { return "macha f0597a4 on dannan 0.5.3"; }
  // As the extract's write_file (tests/test_support.hpp:825-835): created, written whole
  // and committed; its one extent's id read back from the entry.
  std::optional<harness::Id> put(uint64_t seed, std::span<const uint8_t> data) override {
    auto& files = _service->filesystem();
    const auto path = path_of(seed);
    try {
      files.create_file(path, 0644, getuid(), getgid());
    } catch (const FsError& error) {
      if (error.code() != EEXIST) throw;
    }
    auto writer = files.open_write(path, true);
    if (writer->write(0, data) != data.size()) return std::nullopt;
    writer->commit();
    const auto entry = files.getattr(path);
    if (entry.extents.size() != 1) throw std::runtime_error(path + " has " + std::to_string(entry.extents.size()) + " extents, not one");
    harness::Id out;
    std::copy(entry.extents.front().id.bytes.begin(), entry.extents.front().id.bytes.end(), out.bytes.begin());
    return out;
  }
  bool holds(const harness::Id& id) override { return _service->local_state().data().has(object_of(id)); }
  // Through the filesystem, as a viewer reads: what Macha's pacing notes as foreground.
  std::optional<harness::Bytes> read(const harness::Listed& object, std::chrono::milliseconds deadline) override {
    auto& files = _service->filesystem();
    const auto path = path_of(object.seed);
    const auto reader = files.open_read(files.getattr(path), path, false, FrameType::foreground);
    harness::Bytes out(object.size);
    const auto got = reader->read(0, out, Clock::now() + deadline);
    if (got != object.size) return std::nullopt;
    return out;
  }
  // The node's own metadata names every one: its head has each file with the extent
  // listed. Nothing hands a Macha node a referenced set.
  void reference(const std::vector<harness::Listed>& objects) override {
    auto& files = _service->filesystem();
    const auto until = harness::Clock::now() + 30min;
    size_t next = 0;
    while (next < objects.size()) {
      bool named = false;
      try {
        const auto entry = files.getattr(path_of(objects[next].seed));
        named = entry.extents.size() == 1 &&
                std::equal(entry.extents.front().id.bytes.begin(), entry.extents.front().id.bytes.end(), objects[next].id.bytes.begin());
      } catch (const FsError&) {
      }
      if (named) {
        ++next;
        continue;
      }
      if (harness::Clock::now() > until) throw std::runtime_error("the node's metadata never named " + path_of(objects[next].seed));
      std::this_thread::sleep_for(50ms);
    }
  }
  size_t peers() override {
    const auto active = _service->node().membership().active();
    return active.empty() ? 0 : active.size() - 1;
  }
  std::map<std::string, double> counters() override {
    std::map<std::string, double> out;
    const auto repair = _service->repair_diagnostics();
    // Macha at f0597a4 counts no copies pushed or pulled; the wire figures say what moved.
    out["repair_bytes"] = static_cast<double>(repair.bytes_transferred);
    out["push_examined"] = static_cast<double>(repair.push_examined);
    out["pull_examined"] = static_cast<double>(repair.pull_examined);
    out["passes_completed"] = static_cast<double>(repair.passes_completed);
    out["gate_ran"] = static_cast<double>(repair.gate_ran);
    out["gate_share"] = static_cast<double>(repair.gate_share);
    out["gate_quiescent"] = static_cast<double>(repair.gate_quiescent);
    out["gate_credit"] = static_cast<double>(repair.gate_credit);
    out["maintenance_wakeups"] = static_cast<double>(_service->maintenance_wakeups());
    return out;
  }

 private:
  static constexpr const char* directory = "/repair";
  static std::string path_of(uint64_t seed) { return std::string(directory) + "/" + std::to_string(seed); }

  std::unique_ptr<Service> _service;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[2]) == "repair-node" && (argc == 6 || argc == 8)) {
    try {
      const std::filesystem::path dir = argv[1];
      std::vector<Endpoint> seeds;
      if (argc == 8) seeds.push_back(Endpoint{argv[6], static_cast<uint16_t>(std::stoul(argv[7]))});
      MachaRepairNode node(dir, argv[3], argv[4], static_cast<uint16_t>(std::stoul(argv[5])), std::move(seeds));
      return harness::repair_node(node, dir, harness::Report(node.describe(), "repair"));
    } catch (const std::exception& error) {
      std::fprintf(stderr, "harness-macha: %s\n", error.what());
      return 1;
    }
  }
  if (argc != 3) {
    std::fprintf(stderr, "harness-macha <state directory> <workload>\n");
    return 2;
  }
  try {
    const std::string workload = argv[2];
    if (workload == "recovery-open") {
      recovery_open(argv[1], harness::Report("macha f0597a4 on dannan 0.5.3", workload));
      return 0;
    }
    if (workload == "recovery-fill" || workload == "recovery-fill-small") {
      MachaStore store(argv[1], true);
      const harness::Report report(store.describe(), workload);
      harness::recovery_fill(store, workload == "recovery-fill" ? 14 : 1, 20000, std::filesystem::path(argv[1]) / "ids", report);
      return 0;
    }
    MachaStore store(argv[1]);
    return harness::run(store, argv[2]);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "harness-macha: %s\n", error.what());
    return 1;
  }
}
