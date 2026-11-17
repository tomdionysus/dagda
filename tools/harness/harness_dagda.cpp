//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The harness's Dagda half: two Dagda nodes in one process over Dannan on
// loopback (the test fixture's Cluster, as Macha's half uses its tests' BareNode),
// configured as the Macha session asked: replicas 2, write_copies 1, 4 MiB extents,
// packing at 1 MiB into 64 MiB packs, a 500 ms durability window. Every call goes
// through the Context, as Macha's port will.
//
// No maintenance runs, as none runs on Macha's BareNode: the fixture's maintenance is
// not automatic, and the driver calls no pass, so no collector, repair, survey, scrub
// or local stage carries load the Macha side does not. Prompt replication runs on both.
//
//   harness-dagda <workload>
//   harness-dagda recovery-fill|recovery-fill-small|recovery-open <kept directory>
//   harness-dagda repair-node <directory> <name> <host> <port> [<seed host> <seed port>]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dagda/context.h"
#include "dagda/contract/reachability.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/version.h"
#include "dannan/version.h"
#include "harness.h"

namespace {

using namespace std::chrono_literals;

dannan::sched::FrameType frame_of(harness::Frame frame) {
  switch (frame) {
    case harness::Frame::foreground:
      return dannan::sched::FrameType::foreground;
    case harness::Frame::read_ahead:
      return dannan::sched::FrameType::read_ahead;
    case harness::Frame::loader:
      return dannan::sched::FrameType::loader;
    case harness::Frame::speculative:
      return dannan::sched::FrameType::speculative;
  }
  return dannan::sched::FrameType::loader;
}

dagda::store::ObjectId object_of(const harness::Id& id) {
  dagda::store::ObjectId out;
  std::copy(id.bytes.begin(), id.bytes.end(), out.bytes.begin());
  return out;
}

dagda::test::ClusterNodeOptions node_options() {
  dagda::test::ClusterNodeOptions out;
  out.limit = 16ULL * 1024 * 1024 * 1024;
  out.packed = true;
  out.pack_target_size = 64ULL * 1024 * 1024;
  // Packing at 1 MiB, as Macha's side: a 4 MiB extent is a loose file on both.
  out.pack_threshold = 1024 * 1024;
  out.durability_window = 500ms;
  out.store.replication = 2;
  out.store.write_copies = 1;
  out.store.extent_size = harness::extent_bytes;
  // Macha's values where Dagda's fixture differs: its DATA no-progress window, half a
  // Pi 4's threads for background work, no device reserve (Macha has none), and a
  // prompt replication read that takes no admission (Macha's facade's).
  out.no_progress = 120s;
  out.background_concurrency = 2;
  out.device_reserve = false;
  out.store.prompt_read_admission = false;
  // The cadences Macha's side runs (its harness's 1 s heartbeat, which its storage
  // server also refreshes at, and its default 10 s telemetry), not the fixture's test
  // speeds: a 100 ms heartbeat cost Dagda's side about 25 ms of CPU a second at idle.
  out.heartbeat = 1s;
  out.telemetry_interval = 10s;
  out.status_interval = 10s;
  out.refresh_interval = 1s;
  return out;
}

class DagdaStore final : public harness::Store {
 public:
  // Under a temporary directory, or over `kept`, which outlives the run (recovery's fill).
  explicit DagdaStore(std::optional<std::filesystem::path> kept = std::nullopt)
      : _cluster(kept ? std::make_unique<dagda::test::Cluster>(*kept, 10min) : std::make_unique<dagda::test::Cluster>(10min)) {
    _nodes.push_back(&_cluster->add("a", node_options()));
    _nodes.push_back(&_cluster->add("b", node_options()));
    _cluster->start_all();
    if (!_cluster->converged()) throw std::runtime_error("the two nodes did not converge");
    // What each roster record carries, which every copy of it copies: to stderr, beside
    // the figures.
    for (const auto& record : _nodes[0]->context().nodes().roster()) {
      size_t bytes = 0;
      std::string keys;
      for (const auto& [key, value] : record.adverts) {
        bytes += key.size() + value.size();
        keys += (keys.empty() ? "" : ",") + key;
      }
      std::fprintf(stderr, "harness-dagda: record adverts=%zu bytes=%zu keys=%s\n", record.adverts.size(), bytes, keys.c_str());
    }
  }

  std::string describe() const override { return std::string("dagda ") + dagda::library_version() + " on dannan " + dannan::library_version(); }
  size_t nodes() const override { return _nodes.size(); }
  harness::Id id_of(std::span<const uint8_t> data) const override {
    harness::Id out;
    const auto id = dagda::store::object_id(data);
    std::copy(id.bytes.begin(), id.bytes.end(), out.bytes.begin());
    return out;
  }

  bool put(size_t node, const harness::Id& id, std::span<const uint8_t> data, harness::Frame frame) override {
    // Persisted, as Macha's port writes (its floor at write_copies 1; the stack's
    // acknowledgement standard, Tom, 2026-11-15), so both sides do the same work.
    dagda::WriteOptions write;
    write.ack_level = dagda::Durability::persisted;
    write.frame = frame_of(frame);
    return api(node).objects().write(object_of(id), data, write).met;
  }

  class DagdaPublication final : public Publication {
   public:
    DagdaPublication(dagda::Context& api, harness::Frame frame) : _api(api) {
      _write.ack_level = dagda::Durability::persisted;
      _write.deferred = true;
      _write.frame = frame_of(frame);
    }
    bool extent(const harness::Id& id, std::span<const uint8_t> data) override {
      const auto written = _api.objects().write(object_of(id), data, _write);
      _tickets.push_back(written.ticket);
      return written.holders() > 0;
    }
    bool finish() override {
      const auto evidence = _api.tickets().wait(_tickets, 30min);
      return std::all_of(evidence.begin(), evidence.end(), [](const dagda::WriteEvidence& one) { return one.met; });
    }

   private:
    dagda::Context& _api;
    dagda::WriteOptions _write;
    std::vector<dagda::Ticket> _tickets;
  };
  std::unique_ptr<Publication> publication(size_t node, harness::Frame frame) override { return std::make_unique<DagdaPublication>(api(node), frame); }

  std::optional<harness::Bytes> read(size_t node, const harness::Id& id, size_t stripe, harness::Frame frame, std::chrono::milliseconds deadline) override {
    dagda::ReadOptions options;
    options.frame = frame_of(frame);
    options.deadline = deadline;
    options.stripe = stripe;
    const auto result = api(node).objects().read(object_of(id), options);
    if (!result.object) return std::nullopt;
    return result.object->bytes;
  }

  bool remove_local(size_t node, const harness::Id& id) override { return _nodes.at(node)->data().remove(object_of(id)); }
  size_t compact(size_t node) override { return _nodes.at(node)->data().compact_packs(); }
  bool holds(size_t node, const harness::Id& id) override { return _nodes.at(node)->data().has(object_of(id)); }
  uint64_t barriers(size_t node) const override { return _nodes.at(node)->data().physical_barriers(); }

 private:
  dagda::Context& api(size_t node) { return _nodes.at(node)->api(); }

  std::unique_ptr<dagda::test::Cluster> _cluster;
  std::vector<dagda::test::ClusterNode*> _nodes;
};

// Recovery's reopen: node "a" alone over what the fill left in `kept`, timed from its
// opening. Data open is the pool's line, "node data storage ready", as the node logs it
// once the pool's constructor (its pack replay) returns; ready is Dannan's readiness
// complete, every plane marked, as the peers and Status see it; then one foreground
// read of a random extent of the fill's.
void recovery_open(const std::filesystem::path& kept, const harness::Report& report) {
  const auto ids = harness::read_ids(kept / "ids");
  dagda::test::Cluster cluster(kept, 10min);
  auto& a = cluster.add("a", node_options());
  harness::Reopened out;
  const auto before = harness::process_figures();
  const auto read_before = harness::io_field("read_bytes");
  const auto wall_started = dannan::contract::system_wall_clock().unix_ms();
  const auto started = harness::Clock::now();
  std::atomic<bool> stop{false};
  std::thread watch([&] {
    while (!stop.load()) {
      if (a.log().saw("node data storage ready")) {
        out.data_open = harness::Clock::now() - started;
        return;
      }
      std::this_thread::sleep_for(1ms);
    }
  });
  try {
    a.start();
  } catch (const std::exception& error) {
    stop = true;
    watch.join();
    out.failure = std::string("the node refused to open: ") + error.what();
    harness::report_reopen(out, report);
    return;
  }
  const bool ready = a.context().readiness().wait_complete(10min);
  stop = true;
  watch.join();
  if (!ready) {
    out.failure = a.context().readiness().failed() ? "recovery failed: " + a.context().readiness().error() : "not ready within 10 minutes";
  } else {
    out.ready = std::chrono::milliseconds(a.context().readiness().ready_unix_ms() - wall_started);
    // Dagda builds its store inside the node, before ready.
    out.store_built = out.ready;
    const auto at_ready = harness::process_figures();
    out.cpu_to_ready = at_ready.cpu_seconds - before.cpu_seconds;
    out.disk_read_to_ready = harness::io_field("read_bytes") - read_before;
    std::random_device random;
    const auto& id = ids[random() % ids.size()];
    dagda::ReadOptions read;
    read.frame = dannan::sched::FrameType::foreground;
    read.deadline = 60s;
    dagda::store::ObjectId object;
    std::copy(id.bytes.begin(), id.bytes.end(), object.bytes.begin());
    out.read_ok = a.api().objects().read(object, read).object != nullptr;
    out.first_read = harness::Clock::now() - started;
  }
  harness::report_reopen(out, report);
}

// What a repair node's state refers to: the ids it was last told, each telling a new
// generation. It collects nothing: the harness deletes nothing.
class ListedProvider final : public dagda::contract::ReachabilityProvider {
 public:
  void refer_to(std::vector<dagda::store::ObjectId> ids) {
    std::sort(ids.begin(), ids.end());
    std::lock_guard lock(_mutex);
    _ids = std::move(ids);
    ++_generation;
  }
  uint64_t generation() override {
    std::lock_guard lock(_mutex);
    return _generation;
  }
  uint64_t known_generation() override { return generation(); }
  dagda::contract::Reachability inventory(const std::function<void()>&) override {
    std::lock_guard lock(_mutex);
    dagda::contract::Reachability out;
    out.data = _ids;
    out.complete = true;
    out.generation = _generation;
    return out;
  }
  std::optional<dagda::contract::Hash256> release_head() override { return std::nullopt; }
  dagda::contract::Reachability release(const dagda::contract::Hash256&) override { throw std::runtime_error("the harness has no heads"); }
  std::optional<dagda::contract::ControlEpoch> control_epoch() override { return std::nullopt; }
  bool within_control_epoch(uint64_t, const std::function<void()>&) override { return false; }
  void collect_garbage(const dagda::contract::GarbageCollection&) override {}

 private:
  std::mutex _mutex;
  std::vector<dagda::store::ObjectId> _ids;
  uint64_t _generation = 1;
};

// Workload 7's node: one Dagda node on this machine's address, of a cluster whose other
// node is another process's (the fixture's Cluster over <directory>/cluster, its CA
// copied there by the script). The node's own maintenance runs, at the live nodes'
// values: its thread, the survey and repair with their defaults and the process's CPU
// pacing them, with repair's share of the link 0.5 as theirs is.
class DagdaRepairNode final : public harness::RepairNode {
 public:
  DagdaRepairNode(const std::filesystem::path& dir, const std::string& name, const std::string& host, uint16_t port, std::vector<dannan::net::Endpoint> seeds)
      : _cluster(dir / "cluster", 30s) {
    auto options = node_options();
    options.host = host;
    options.port = port;
    options.seeds = std::move(seeds);
    options.maintenance = dagda::MaintenanceOptions{};
    options.availability = dagda::store::AvailabilityOptions{};
    options.repair = dagda::store::RepairOptions{};
    options.repair.idle_bandwidth_fraction = 0.5;
    // The live nodes' network values (macnessa and inverbeg, read by the Macha session,
    // 2026-11-16): a 5 s heartbeat, 10 s telemetry, and so a peer's viewer seen for three
    // telemetry intervals.
    options.heartbeat = 5s;
    options.telemetry_interval = 10s;
    options.status_interval = 10s;
    options.repair.peer_window = 30s;
    // HARNESS_REPAIR_TRACE set: repair's decisions, and every quarter second its credit,
    // window and link estimate, on stderr, each line timed from the node's start. For
    // finding a mechanism, never for a measured run.
    const bool traced = std::getenv("HARNESS_REPAIR_TRACE") != nullptr;
    const auto began = std::chrono::steady_clock::now();
    const auto at = [began] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count(); };
    if (traced) {
      static std::mutex lines;
      options.store.repair_trace = [at](std::string_view kind, std::string_view detail) {
        std::lock_guard lock(lines);
        std::cerr << "repair-trace " << at() << " " << kind << " " << detail << "\n";
      };
      options.store.repair_timing = [at](std::string_view event) {
        std::lock_guard lock(lines);
        std::cerr << "repair-time " << at() << " " << event << "\n";
      };
    }
    _node = &_cluster.add(name, options);
    _node->start();
    _node->dagda_node().register_reachability(dagda::domain::domain_v1, _provider);
    if (traced)
      _sampler = std::jthread([this, at](std::stop_token stop) {
        while (!stop.stop_requested()) {
          const auto gates = _node->repair().gates();
          const auto repair = _node->store().repair_diagnostics();
          std::cerr << "repair-sample " << at() << " credit=" << gates.last_credit_bytes << " gate=" << (gates.last ? static_cast<int>(*gates.last) : -1)
                    << " ran=" << gates.ran << " credit_gates=" << gates.credit << " in_flight=" << repair.in_flight << " pushed=" << repair.copies_pushed
                    << " pulled=" << repair.copies_pulled << " bps=" << static_cast<uint64_t>(_node->store().estimated_network_bps()) << "\n";
          std::this_thread::sleep_for(250ms);
        }
      });
  }
  ~DagdaRepairNode() override {
    _sampler = {};
    _node->dagda_node().unregister_reachability(dagda::domain::domain_v1);
  }

  std::string describe() const override { return std::string("dagda ") + dagda::library_version() + " on dannan " + dannan::library_version(); }
  std::optional<harness::Id> put(uint64_t, std::span<const uint8_t> data) override {
    const auto id = dagda::store::object_id(data);
    dagda::WriteOptions write;
    write.ack_level = dagda::Durability::persisted;
    write.frame = dannan::sched::FrameType::loader;
    if (!_node->api().objects().write(id, data, write).met) return std::nullopt;
    harness::Id out;
    std::copy(id.bytes.begin(), id.bytes.end(), out.bytes.begin());
    return out;
  }
  bool holds(const harness::Id& id) override { return _node->data().has(object_of(id)); }
  std::optional<harness::Bytes> read(const harness::Listed& object, std::chrono::milliseconds deadline) override {
    dagda::ReadOptions options;
    options.frame = dannan::sched::FrameType::foreground;
    options.deadline = deadline;
    const auto result = _node->api().objects().read(object_of(object.id), options);
    if (!result.object) return std::nullopt;
    return result.object->bytes;
  }
  void reference(const std::vector<harness::Listed>& objects) override {
    std::vector<dagda::store::ObjectId> ids;
    ids.reserve(objects.size());
    for (const auto& object : objects) ids.push_back(object_of(object.id));
    _provider.refer_to(std::move(ids));
  }
  size_t peers() override {
    size_t others = 0;
    for (const auto& record : _node->context().nodes().active())
      if (record.id != _node->id()) ++others;
    return others;
  }
  std::map<std::string, double> counters() override {
    std::map<std::string, double> out;
    const auto repair = _node->store().repair_diagnostics();
    out["copies_pushed"] = static_cast<double>(repair.copies_pushed);
    out["copies_pulled"] = static_cast<double>(repair.copies_pulled);
    out["repair_bytes"] = static_cast<double>(repair.bytes_transferred);
    out["push_examined"] = static_cast<double>(repair.push_examined);
    out["pull_examined"] = static_cast<double>(repair.pull_examined);
    out["passes_completed"] = static_cast<double>(repair.passes_completed);
    out["offer_bytes"] = static_cast<double>(repair.offer_bytes);
    out["in_flight"] = static_cast<double>(repair.in_flight);
    const auto gates = _node->repair().gates();
    out["gate_ran"] = static_cast<double>(gates.ran);
    out["gate_share"] = static_cast<double>(gates.share);
    out["gate_quiescent"] = static_cast<double>(gates.quiescent);
    out["gate_credit"] = static_cast<double>(gates.credit);
    // What this node was asked, by message: the other node's calls to it.
    out["asked_put_object"] = static_cast<double>(_node->calls_of(dagda::store::message::put_object));
    out["asked_get_object"] = static_cast<double>(_node->calls_of(dagda::store::message::get_object));
    out["asked_get_objects"] = static_cast<double>(_node->calls_of(dagda::store::message::get_objects));
    out["asked_offer_objects"] = static_cast<double>(_node->calls_of(dagda::store::message::offer_objects));
    out["asked_have_objects"] = static_cast<double>(_node->calls_of(dagda::store::message::have_objects));
    out["asked_trie_diff"] = static_cast<double>(_node->calls_of(dagda::store::message::trie_diff));
    return out;
  }

 private:
  dagda::test::Cluster _cluster;
  ListedProvider _provider;
  dagda::test::ClusterNode* _node = nullptr;
  std::jthread _sampler;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string workload = argc > 1 ? argv[1] : "";
  const bool recovery = workload.rfind("recovery-", 0) == 0;
  if (workload == "repair-node" && (argc == 6 || argc == 8)) {
    try {
      const std::filesystem::path dir = argv[2];
      std::vector<dannan::net::Endpoint> seeds;
      if (argc == 8) seeds.push_back({argv[6], static_cast<uint16_t>(std::stoul(argv[7]))});
      DagdaRepairNode node(dir, argv[3], argv[4], static_cast<uint16_t>(std::stoul(argv[5])), std::move(seeds));
      return harness::repair_node(node, dir, harness::Report(node.describe(), "repair"));
    } catch (const std::exception& error) {
      std::fprintf(stderr, "harness-dagda: %s\n", error.what());
      return 1;
    }
  }
  if (argc != (recovery ? 3 : 2)) {
    std::fprintf(stderr, "harness-dagda <workload>\nharness-dagda recovery-fill|recovery-fill-small|recovery-open <kept directory>\n");
    return 2;
  }
  try {
    if (recovery) {
      const std::filesystem::path kept = argv[2];
      if (workload == "recovery-open") {
        const harness::Report report(std::string("dagda ") + dagda::library_version() + " on dannan " + dannan::library_version(), workload);
        recovery_open(kept, report);
        return 0;
      }
      if (workload != "recovery-fill" && workload != "recovery-fill-small") throw std::runtime_error("unknown workload " + workload);
      std::filesystem::remove_all(kept);
      DagdaStore store(kept);
      const harness::Report report(store.describe(), workload);
      harness::recovery_fill(store, workload == "recovery-fill" ? 14 : 1, 20000, kept / "ids", report);
      return 0;
    }
    DagdaStore store;
    return harness::run(store, argv[1]);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "harness-dagda: %s\n", error.what());
    return 1;
  }
}
