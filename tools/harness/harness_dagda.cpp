//
// Dagda - The Store
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

#include <cstdio>
#include <exception>
#include <string>

#include "dagda/context.h"
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
  DagdaStore() : _cluster(10min) {
    _nodes.push_back(&_cluster.add("a", node_options()));
    _nodes.push_back(&_cluster.add("b", node_options()));
    _cluster.start_all();
    if (!_cluster.converged()) throw std::runtime_error("the two nodes did not converge");
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
    dagda::WriteOptions write;
    write.level = dagda::Durability::replicated;
    write.frame = frame_of(frame);
    return api(node).objects().write(object_of(id), data, write).met;
  }

  class DagdaPublication final : public Publication {
   public:
    DagdaPublication(dagda::Context& api, harness::Frame frame) : _api(api) {
      _write.level = dagda::Durability::replicated;
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

  dagda::test::Cluster _cluster;
  std::vector<dagda::test::ClusterNode*> _nodes;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "harness-dagda <workload>\n");
    return 2;
  }
  try {
    DagdaStore store;
    return harness::run(store, argv[1]);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "harness-dagda: %s\n", error.what());
    return 1;
  }
}
