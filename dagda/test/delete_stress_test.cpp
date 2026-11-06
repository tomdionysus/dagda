//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A stress case, not a logic case: the delete path under real concurrency, on a real
// wall clock, for a sanitizer to watch. Heavy, so disabled: run with
// --gtest_also_run_disabled_tests --gtest_filter='DeleteStress.*', and size it with the
// DAGDA_STRESS_* variables (Params, below). It runs what Macha's node ran when live-a
// corrupted its heap (nothing proven, Macha's report of timing only): a storm of deletes
// starting suddenly on a busy node, while the maintenance thread reads held tries.
//
// One head shared by every node, as Macha's every-node space is once converged, with
// real CONTROL retirement batches (heads/retirement.h), so each node's repair, survey,
// collector and local stages see a whole cluster's state:
//  - writers: put, claim, commit a reference; a few bring back what was retired.
//  - deleters: a recursive delete's shape, batches on one node starting all at once:
//    a retirement batch, the commit, and for half of them the instant delete.
//  - on all nodes but the last, Macha's maintenance loop (Maintenance::run at 9a08846,
//    as maintenance_steps_test keeps it) over the steps, on the real clock; on the last,
//    the node's own maintenance thread.
//  - held readers: held_view, the union, Snapshot::get and the rest of its reads.
//  - object readers: strict local reads of what is claimed, and the store's reads.
//  - head churn: a real heads::Space on each node, committing and retiring.
//  - chaos: a stop in the middle of a pass and a restart under the rest, some with the
//    held ledger gone or unseeded, so it is rebuilt.
//  - trie churn: held ledgers on tiny options beside the cluster, checkpointing and
//    rewriting all the time, read through snapshots kept across those.
//
// What it asserts is not timing: every object still claimed reads back, every removed
// object stays gone once it has settled, and the counts agree. Sleeps here only pace
// the load; nothing's correctness waits on one. A clean run clears nothing: it says
// only that this load, on this machine, found nothing.

#include <execinfo.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "dagda/context.h"
#include "dagda/contract/stopping.h"
#include "dagda/heads/control_node_store.h"
#include "dagda/heads/retirement.h"
#include "dagda/ledger/collector.h"
#include "dagda/ledger/held_ledger.h"
#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/trie_union.h"
#include "dagda/node.h"
#include "dagda/store/remove.h"
#include "dagda/store/repair.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/contract/time_source.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;
using contract::Garbage;
using contract::GarbageBatch;
using contract::Hash256;
using contract::MaintenanceStopping;
using contract::Reachability;
using contract::RetentionClass;
using contract::RetentionClock;
using contract::RetentionDot;
using dannan::contract::TimePoint;
using dannan::identity::NodeId;
using heads::Space;
using ledger::ObjectTrie;
using store::Bytes;
using store::ObjectId;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;

using Steady = std::chrono::steady_clock;

// An abort's stack in the log: what ended a run is the finding, and the case may be
// running where nothing else keeps it.
void print_stack_and_die(int signal_number) {
  void* frames[128];
  const int count = ::backtrace(frames, 128);
  constexpr char heading[] = "STRESS fatal signal, stack:\n";
  (void)!::write(2, heading, sizeof(heading) - 1);
  ::backtrace_symbols_fd(frames, count, 2);
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}

uint64_t env_u64(const char* name, uint64_t fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::strtoull(value, nullptr, 10) : fallback;
}

// The size and the length, from the environment.
struct Params {
  // How long the load runs, then how long it may take to settle.
  uint64_t seconds = env_u64("DAGDA_STRESS_SECONDS", 60);
  uint64_t settle_seconds = env_u64("DAGDA_STRESS_SETTLE_SECONDS", 300);
  uint64_t seed = env_u64("DAGDA_STRESS_SEED", 1);
  uint64_t nodes = env_u64("DAGDA_STRESS_NODES", 4);
  // Writers on each node.
  uint64_t writers = env_u64("DAGDA_STRESS_WRITERS", 2);
  // Objects in the head before the first storm, and where writers stop.
  uint64_t preload = env_u64("DAGDA_STRESS_PRELOAD", 5000);
  uint64_t max_live = env_u64("DAGDA_STRESS_MAX_LIVE", 40000);
  // One storm: this many threads on one node, batches of this many, this many in all.
  uint64_t deleters = env_u64("DAGDA_STRESS_DELETERS", 4);
  uint64_t batch = env_u64("DAGDA_STRESS_BATCH", 64);
  uint64_t storm_objects = env_u64("DAGDA_STRESS_STORM_OBJECTS", 2000);
  // Seconds between restarts; zero, none.
  uint64_t restart_seconds = env_u64("DAGDA_STRESS_RESTART_SECONDS", 30);
  // The wait a retirement and an unreferenced object sit out.
  uint64_t grace_ms = env_u64("DAGDA_STRESS_GRACE_MS", 4000);
  uint64_t pass_floor_ms = env_u64("DAGDA_STRESS_PASS_FLOOR_MS", 20);
  // The ledger cache of each node, and the records on each churned ledger.
  uint64_t ledger_cache_bytes = env_u64("DAGDA_STRESS_LEDGER_CACHE", 512 * 1024);
  uint64_t trie_records = env_u64("DAGDA_STRESS_TRIE_RECORDS", 20000);
  // How many objects the end reads through the store on each node.
  uint64_t final_reads = env_u64("DAGDA_STRESS_FINAL_READS", 500);
};

// Counts by name, and failures, across threads. A count takes no lock until its thread
// ends: a lock taken on every count would order the threads for TSan, and hide races.
class Tally {
 public:
  void add(const std::string& name, uint64_t count = 1) { local()[this][name] += count; }
  // A thrown error on a thread that carries on: most are what a stopping peer causes,
  // named in the report. "mutex lock failed", which one TSan run once printed, is a
  // finding.
  void error(const std::string& where, const std::string& what) {
    add("error." + where + ": " + what.substr(0, 96));
    if (what.find("mutex lock failed") != std::string::npos) fail(where + " saw " + what);
  }
  void fail(const std::string& what) {
    add("FAILURE");
    {
      std::lock_guard lock(_mutex);
      if (++_failures > 40) return;
    }
    ADD_FAILURE() << what;
  }
  // Every other thread that counted has ended.
  void report(std::ostream& out) {
    auto& mine = local();
    if (const auto found = mine.find(this); found != mine.end()) {
      merge(found->second);
      mine.erase(found);
    }
    std::lock_guard lock(_mutex);
    for (const auto& [name, count] : _counts) out << "STRESS " << name << " = " << count << "\n";
    out.flush();
  }

 private:
  using Counts = std::map<std::string, uint64_t>;
  struct Local : std::map<Tally*, Counts> {
    ~Local() {
      for (auto& [tally, counts] : *this) tally->merge(counts);
    }
  };
  static Local& local() {
    thread_local Local counts;
    return counts;
  }
  void merge(const Counts& counts) {
    std::lock_guard lock(_mutex);
    for (const auto& [name, count] : counts) _counts[name] += count;
  }
  std::mutex _mutex;
  Counts _counts;
  uint64_t _failures = 0;
};

// Runs a thread's body, naming what escapes it.
void guarded(Tally& tally, const std::string& where, const std::function<void()>& body) {
  try {
    body();
  } catch (const std::exception& error) {
    tally.error(where, error.what());
  } catch (...) {
    tally.error(where, "unknown exception");
  }
}

// An object's bytes from its number: most fit a pack, some are loose.
Bytes bytes_of(uint64_t n, uint32_t size) {
  auto out = test::pattern(size, static_cast<uint8_t>(n));
  std::memcpy(out.data(), &n, sizeof(n));
  return out;
}

ObjectId object_of(uint64_t generation) {
  ObjectId out{};
  for (size_t i = 0; i < 8; ++i) out.bytes[i] = static_cast<uint8_t>(generation >> (8 * i));
  out.bytes[31] = 0xb0;
  return out;
}

inline constexpr heads::Magic batch_magic{'S', 'T', 'R', 'S', 'B', 'T', 'C', '1'};

// ---- The one head --------------------------------------------------------------

// What the shared head says, and what the test knows of every object it ever named.
// A commit is atomic here, as a head is. Each writer has its own origin and its dots
// reach the clock in order, as a head's author's do.
class Head {
 public:
  enum class Stage : uint8_t { live, doomed, retired, reviving };
  struct Entry {
    uint64_t n = 0;
    uint32_t size = 0;
    Stage stage = Stage::live;
    size_t at = 0;
  };
  struct Made {
    ObjectId id;
    uint64_t n = 0;
    uint32_t size = 0;
  };
  struct View {
    uint64_t generation = 0;
    TimePoint since{};
    std::vector<ObjectId> live;
    RetentionClock clock;
    std::vector<GarbageBatch> batches;
    Hash256 hash{};
  };

  uint64_t generation() const {
    std::shared_lock lock(_mutex);
    return _generation;
  }
  View view() const {
    std::shared_lock lock(_mutex);
    View out;
    out.generation = _generation;
    out.since = _since;
    out.live = _live;
    std::sort(out.live.begin(), out.live.end());
    out.clock = _clock;
    out.batches = _batches;
    out.hash = hash_locked();
    return out;
  }
  size_t live_count() const {
    std::shared_lock lock(_mutex);
    return _live.size();
  }
  size_t retired_count() const {
    std::shared_lock lock(_mutex);
    return _retired.size();
  }
  size_t doomed_count() const {
    std::shared_lock lock(_mutex);
    return _doomed;
  }
  size_t batch_count() const {
    std::shared_lock lock(_mutex);
    return _batches.size();
  }
  std::optional<Entry> entry(const ObjectId& id) const {
    std::shared_lock lock(_mutex);
    const auto found = _entries.find(id);
    if (found == _entries.end()) return std::nullopt;
    return found->second;
  }
  // Is it named, and untouched by any delete, right now?
  bool stands(const ObjectId& id) const {
    std::shared_lock lock(_mutex);
    const auto found = _entries.find(id);
    return found != _entries.end() && found->second.stage == Stage::live;
  }
  std::vector<Made> sample(std::mt19937_64& random, size_t count) const {
    std::shared_lock lock(_mutex);
    std::vector<Made> out;
    if (_live.empty()) return out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const auto& id = _live[random() % _live.size()];
      const auto& entry = _entries.at(id);
      out.push_back({id, entry.n, entry.size});
    }
    return out;
  }
  std::vector<Made> all_live() const {
    std::shared_lock lock(_mutex);
    std::vector<Made> out;
    out.reserve(_live.size());
    for (const auto& id : _live) {
      const auto& entry = _entries.at(id);
      out.push_back({id, entry.n, entry.size});
    }
    return out;
  }
  std::vector<Made> all_retired() const {
    std::shared_lock lock(_mutex);
    std::vector<Made> out;
    out.reserve(_retired.size());
    for (const auto& id : _retired) {
      const auto& entry = _entries.at(id);
      out.push_back({id, entry.n, entry.size});
    }
    return out;
  }
  std::set<ObjectId> named_live() const {
    std::shared_lock lock(_mutex);
    return {_live.begin(), _live.end()};
  }

  // A commit naming what its writer made, already claimed under `dot`.
  void commit_add(const std::vector<Made>& made, const RetentionDot& dot) {
    std::unique_lock epoch(_epoch);
    std::unique_lock lock(_mutex);
    for (const auto& one : made) {
      auto& entry = _entries[one.id];
      if (entry.stage == Stage::reviving) {
        erase_from(_retired, entry.at);
        --_reviving;
      }
      entry.n = one.n;
      entry.size = one.size;
      entry.stage = Stage::live;
      entry.at = _live.size();
      _live.push_back(one.id);
    }
    advance_locked(dot);
  }
  // What a delete will take: named until the commit, but no reader counts on them.
  std::vector<ObjectId> doom(std::mt19937_64& random, size_t count) {
    std::unique_lock lock(_mutex);
    std::vector<ObjectId> out;
    // A recursive delete takes what sat together: a run of the namespace, here a
    // run of the list.
    for (size_t tries = 0; tries < count && !_live.empty(); ++tries) {
      const auto id = _live[(_doom_at++ + (random() % 8)) % _live.size()];
      auto& entry = _entries.at(id);
      if (entry.stage != Stage::live) continue;
      entry.stage = Stage::doomed;
      ++_doomed;
      out.push_back(id);
    }
    return out;
  }
  void spare(const std::vector<ObjectId>& ids) {
    std::unique_lock lock(_mutex);
    for (const auto& id : ids) {
      _entries.at(id).stage = Stage::live;
      --_doomed;
    }
  }
  // The commit of a delete: the references go and the batch is named, all at once.
  // Returns the clock it observed, which the instant delete is asked under.
  RetentionClock commit_retire(const std::vector<ObjectId>& ids, const GarbageBatch& batch, const RetentionDot& dot) {
    std::unique_lock epoch(_epoch);
    std::unique_lock lock(_mutex);
    for (const auto& id : ids) {
      auto& entry = _entries.at(id);
      erase_from(_live, entry.at);
      entry.stage = Stage::retired;
      entry.at = _retired.size();
      _retired.push_back(id);
      --_doomed;
    }
    heads::insert_retirement_batch(_batches, batch);
    advance_locked(dot);
    return _clock;
  }
  // One retired object to bring back, its stage kept so that nothing else does.
  std::optional<Made> revive(std::mt19937_64& random) {
    std::unique_lock lock(_mutex);
    if (_retired.empty()) return std::nullopt;
    const auto& id = _retired[random() % _retired.size()];
    auto& entry = _entries.at(id);
    if (entry.stage != Stage::retired) return std::nullopt;
    entry.stage = Stage::reviving;
    ++_reviving;
    return Made{id, entry.n, entry.size};
  }
  void abandon_revival(const ObjectId& id) {
    std::unique_lock lock(_mutex);
    _entries.at(id).stage = Stage::retired;
    --_reviving;
  }
  size_t reviving_count() const {
    std::shared_lock lock(_mutex);
    return _reviving;
  }
  // A commit that names nothing new: a writer's discarded work reaches the clock.
  void advance(const RetentionDot& dot) {
    std::unique_lock epoch(_epoch);
    std::unique_lock lock(_mutex);
    advance_locked(dot);
  }
  void tick() {
    std::unique_lock epoch(_epoch);
    std::unique_lock lock(_mutex);
    ++_generation;
    _since = dannan::contract::steady_time_source().now();
  }
  std::vector<GarbageBatch> batches() const {
    std::shared_lock lock(_mutex);
    return _batches;
  }
  void replace_batches(const std::vector<ObjectId>& drop, const std::vector<GarbageBatch>& add, const RetentionDot& dot) {
    std::unique_lock epoch(_epoch);
    std::unique_lock lock(_mutex);
    for (const auto& id : drop) {
      std::erase_if(_batches, [&](const GarbageBatch& batch) { return batch.id == id; });
      _dropped_batches.push_back(id);
    }
    for (const auto& batch : add) heads::insert_retirement_batch(_batches, batch);
    advance_locked(dot);
  }
  std::vector<ObjectId> dropped_batches() const {
    std::shared_lock lock(_mutex);
    return _dropped_batches;
  }
  bool within_epoch(uint64_t sequence, const std::function<void()>& removal) {
    std::shared_lock epoch(_epoch);
    if (generation() != sequence) return false;
    removal();
    return true;
  }
  // The objects a node's own head space names: not this head's to judge.
  void exempt(const ObjectId& id) {
    std::unique_lock lock(_mutex);
    _exempt.insert(id);
  }
  bool exempted(const ObjectId& id) const {
    std::shared_lock lock(_mutex);
    return _exempt.contains(id);
  }
  std::mutex& collecting() { return _collecting; }

 private:
  void erase_from(std::vector<ObjectId>& list, size_t at) {
    if (at + 1 != list.size()) {
      list[at] = list.back();
      _entries.at(list[at]).at = at;
    }
    list.pop_back();
  }
  void advance_locked(const RetentionDot& dot) {
    auto& seen = _clock[dot.origin];
    seen = std::max(seen, dot.sequence);
    ++_generation;
    _since = dannan::contract::steady_time_source().now();
  }
  Hash256 hash_locked() const {
    Hash256 out{};
    for (size_t i = 0; i < 8; ++i) out.bytes[i] = static_cast<uint8_t>(_generation >> (8 * i));
    out.bytes[31] = 1;
    return out;
  }

  mutable std::shared_mutex _epoch;
  mutable std::shared_mutex _mutex;
  std::mutex _collecting;
  uint64_t _generation = 2;
  TimePoint _since = dannan::contract::steady_time_source().now();
  std::map<ObjectId, Entry> _entries;
  std::vector<ObjectId> _live;
  std::vector<ObjectId> _retired;
  size_t _doom_at = 0;
  size_t _doomed = 0;
  size_t _reviving = 0;
  RetentionClock _clock;
  std::vector<GarbageBatch> _batches;
  std::vector<ObjectId> _dropped_batches;
  std::set<ObjectId> _exempt;
};

// ---- One node's owner ----------------------------------------------------------

// The head as this node's owner answers it: a counted trie for what it refers to, as
// Macha's namespace reference counts are, and the batches read off its CONTROL store.
// Called on one thread: the loop's, or the node's maintenance thread.
class NodeOwner final : public contract::ReachabilityProvider {
 public:
  NodeOwner(Head& head, ClusterNode& node, Tally& tally) : _head(head), _node(node), _tally(tally), _origin(NodeId::random()) {}

  // A new process: what the counted tries hold is not known to be what was counted.
  void restarted() {
    _counted.reset();
    _release.reset();
  }

  uint64_t generation() override { return _head.generation(); }
  uint64_t known_generation() override { return _head.generation(); }
  Reachability inventory(const std::function<void()>& pause) override {
    if (pause) pause();
    auto view = _head.view();
    Reachability out;
    out.generation = view.generation;
    out.batches = view.batches;
    auto reader = heads::ControlTreeNodeStore::for_reading(_node.control(), _node.store());
    size_t unreadable = 0;
    out.garbage = heads::retirements_of(view.batches, reader, batch_magic, &unreadable, pause);
    if (unreadable) _tally.add("owner.batches_unreadable", unreadable);
    if (pause) pause();
    out.views.data = count(view);
    out.complete = unreadable == 0;
    _tally.add("owner.inventories");
    return out;
  }
  std::optional<Hash256> release_head() override {
    _release = _head.view();
    return _release->hash;
  }
  Reachability release(const Hash256& head) override {
    Reachability out;
    out.head = head;
    if (!_release || _release->hash != head) return out;
    out.data = _release->live;
    out.batches = _release->batches;
    out.clock = _release->clock;
    out.complete = true;
    _tally.add("owner.releases");
    return out;
  }
  std::optional<contract::ControlEpoch> control_epoch() override {
    const auto view = _head.view();
    return contract::ControlEpoch{view.generation, view.since};
  }
  bool within_control_epoch(uint64_t sequence, const std::function<void()>& removal) override { return _head.within_epoch(sequence, removal); }
  // As a head's maintain_tombstone_batches commits it: rewrites what an erase leaves
  // of a batch, drops what matured. One node at a time, as one head's mutations are.
  void collect_garbage(const contract::GarbageCollection& collection) override {
    if (collection.erase.empty() && collection.matured_batches.empty()) return;
    std::lock_guard collecting(_head.collecting());
    const auto batches = _head.batches();
    std::vector<ObjectId> drop;
    std::vector<GarbageBatch> add;
    const RetentionDot dot{_origin, ++_sequence};
    if (!collection.erase.empty()) {
      auto reader = heads::ControlTreeNodeStore::for_reading(_node.control(), _node.store());
      auto rewrite = heads::rewrite_retirements(batches, reader, batch_magic, collection.erase);
      drop = rewrite.dropped;
      auto writer = heads::ControlTreeNodeStore::for_commit(_node.control(), _node.store());
      for (auto& rest : rewrite.rest) {
        const auto batch = heads::write_retirement_batch(writer, batch_magic, rest.retired_at_ns, std::move(rest.ids));
        if (!batch) continue;
        if (!_node.store().retain_control({batch->id}, dot, store::DistributedStore::ClaimScope::here))
          throw std::runtime_error("a rewritten batch was not claimed");
        add.push_back(*batch);
        _tally.add("owner.batches_rewritten");
      }
    }
    for (const auto& id : collection.matured_batches)
      if (std::any_of(batches.begin(), batches.end(), [&](const GarbageBatch& batch) { return batch.id == id; })) drop.push_back(id);
    std::sort(drop.begin(), drop.end());
    drop.erase(std::unique(drop.begin(), drop.end()), drop.end());
    if (drop.empty() && add.empty()) return;
    _head.replace_batches(drop, add, dot);
    _tally.add("owner.batches_dropped", drop.size());
    _tally.add("owner.garbage_commits");
  }

 private:
  // Brings the counted trie to this head, by the difference when it can.
  ObjectTrie::Snapshot count(const Head::View& view) {
    auto& counts = _node.dagda_node().references();
    const auto root = object_of(view.generation);
    const ledger::ReferenceCounts::Totals totals{view.live.size(), view.live.size()};
    bool done = false;
    if (_counted) {
      std::vector<ObjectId> in;
      std::vector<ObjectId> out;
      std::set_difference(view.live.begin(), view.live.end(), _counted->begin(), _counted->end(), std::back_inserter(in));
      std::set_difference(_counted->begin(), _counted->end(), view.live.begin(), view.live.end(), std::back_inserter(out));
      done = counts.change(root, std::move(in), std::move(out), {}, {}, totals);
    }
    if (!done) {
      counts.reset(root, view.live, {}, totals);
      _tally.add("owner.count_resets");
    }
    _counted = view.live;
    return counts.views().extents;
  }

  Head& _head;
  ClusterNode& _node;
  Tally& _tally;
  const NodeId _origin;
  uint64_t _sequence = 0;
  std::optional<std::vector<ObjectId>> _counted;
  std::optional<Head::View> _release;
};

// ---- Macha's loop on the real clock --------------------------------------------

constexpr auto gc_quiet_after_change = 50ms;

// Macha's Maintenance::run over the steps, one call of pass() a turn of its loop, as
// maintenance_steps_test's MachaLoop keeps it (its metadata and catalogue stages left
// out), with real time where that has a stepped clock.
class StressLoop {
 public:
  StressLoop(ClusterNode& node, NodeOwner& owner, Head& head, Tally& tally, const Params& params, std::chrono::milliseconds grace)
      : _node(node), _owner(owner), _head(head), _tally(tally), _params(params), _grace(grace), _last_generation(head.generation()) {
    _observed_event = events();
    _node.dagda_node().register_reachability(domain::domain_v1, _owner);
    _thread = std::thread([this] { guarded(_tally, "loop", [this] { run(); }); });
  }
  ~StressLoop() {
    stop();
    _node.dagda_node().unregister_reachability(domain::domain_v1);
  }
  // Macha's stop, as its service asks it: what the pass reaches throws, and the
  // thread is joined. `step`: asked when the pass is at that step, or by `bound`.
  void stop(const std::string& step = {}, std::chrono::milliseconds bound = 3s) {
    if (!step.empty()) {
      const auto until = Steady::now() + bound;
      while (Steady::now() < until && _at.load() != step_code(step)) std::this_thread::yield();
      if (_at.load() == step_code(step)) _tally.add("loop.stopped_at." + step);
    }
    _stop.request_stop();
    if (_thread.joinable()) _thread.join();
  }

 private:
  static int step_code(std::string_view step) {
    static const std::vector<std::string_view> steps{"inventory", "survey", "walks", "transfer", "settle", "tombstones", "release", "control", "data", "local"};
    return static_cast<int>(std::find(steps.begin(), steps.end(), step) - steps.begin());
  }
  void enter(std::string_view step) { _at.store(step_code(step)); }
  uint64_t events() const { return _node.dagda_node().maintenance_events().total(); }
  std::function<void()> pause() {
    return [token = _stop.get_token()] {
      if (token.stop_requested()) throw MaintenanceStopping{};
    };
  }
  void survey() {
    enter("survey");
    try {
      (void)_node.repair().survey(pause());
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception& error) {
      _tally.error("survey", error.what());
    }
  }
  void run() {
    while (!_stop.stop_requested()) {
      pass();
      park();
    }
  }
  void pass() {
    const auto& clock = dannan::contract::steady_time_source();
    auto& repair = _node.repair();
    const auto now = clock.now();
    const std::chrono::milliseconds interval{std::max<uint64_t>(_params.pass_floor_ms, 1)};
    const auto current_event = events();
    const bool event_changed = current_event != _observed_event;
    _observed_event = current_event;
    if (const auto generation = _head.generation(); generation != _last_generation) {
      _last_generation = generation;
      _gc_quiet_until = now + gc_quiet_after_change;
    }
    if (event_changed) {
      _network_quiet_until = TimePoint{};
      _gc_quiet_until = now + gc_quiet_after_change;
    }
    (void)repair.earn_credit(now, interval);
    const auto paced = repair.paced_by();
    const bool busy = (paced & (store::paced_by_foreground | store::paced_by_loader)) != 0;
    bool gc_due_this_pass = false;
    try {
      const bool allow = repair.may_start(now, paced);
      const bool network_due = allow && now >= _network_quiet_until;
      repair.note_gate(network_due ? store::RepairGate::ran : !allow ? store::RepairGate::share : store::RepairGate::quiescent, paced);
      const bool garbage_due = !busy;
      const bool gc_due = !busy && now >= _gc_quiet_until;
      gc_due_this_pass = gc_due;

      if (network_due || garbage_due || gc_due) {
        enter("inventory");
        auto& collector = *_node.dagda_node().collector();
        const auto inventory = collector.inventory_step(pause(), true);
        if (inventory.rebuilt) _gc_quiet_until = clock.now() + gc_quiet_after_change;
        survey();
        if (network_due) {
          auto turn = repair.begin_turn(clock.now(), paced, store::PeerReading::as_begun);
          enter("walks");
          const auto walks = repair.claim_walks(turn);
          enter("transfer");
          const auto step = repair.transfer(turn, repair.lists());
          enter("settle");
          const auto settled = repair.settle(walks, step);
          if (settled == store::RepairSettled::credit) repair.note_gate(store::RepairGate::credit, paced);
          if (settled == store::RepairSettled::quiescent) _network_quiet_until = TimePoint::max();
          _tally.add("loop.repair_turns");
        }
        ledger::PassDue due;
        due.garbage_due = garbage_due;
        due.gc_due = gc_due;
        due.busy = busy;
        due.gc_waiting_for_event = _gc_quiet_until == TimePoint::max();
        enter("tombstones");
        const auto tombstones = collector.tombstone_step(due, inventory);
        if (tombstones.gate.permitted) _tally.add("loop.tombstone_gate_open");
        enter("release");
        const auto release = collector.release_step();
        enter("control");
        const auto control = collector.control_step(due, inventory, release);
        _tally.add("loop.control_released", control.released);
        _tally.add("loop.control_removed", control.removed);
        enter("data");
        const auto data = collector.data_step(due, inventory, release);
        _tally.add("loop.data_released", data.released);
        _tally.add("loop.data_swept", data.swept.objects);
        _tally.add("loop.data_pruned", data.pruned);
        if (data.gate.permitted) _tally.add("loop.data_gate_open");
        if (data.gate.permitted && !data.swept.yielded && data.swept.complete)
          _gc_quiet_until = data.swept.deferred ? clock.now() + data.orphan_grace : TimePoint::max();
      } else {
        survey();
      }
      enter("local");
      local();
      if (gc_due_this_pass && !busy) arm_retirement_wake();
      _tally.add("loop.passes");
    } catch (const MaintenanceStopping&) {
      _tally.add("loop.stopped_by_throw");
    } catch (const std::exception& error) {
      _tally.error("pass", error.what());
    }
  }
  // Macha's local stages. Every eighth turn says nothing is busy, whatever the activity
  // clocks say, so that pack compaction and the claims' compaction run under the load.
  void local() {
    auto& node = _node.dagda_node();
    LocalTurnOptions options;
    options.stop = _stop.get_token();
    options.when_no_time_passed = 1000ms;
    if (++_turns % 8 == 0) options.busy = false;
    auto turn = node.begin_local_stages(options);
    node.local_rebalance(turn);
    node.retention_compact(turn);
    node.compact_packs(turn);
    node.local_scrub(turn);
    node.ledger_verify(turn);
    const auto verified = _node.data().verify_step(16);
    _tally.add("loop.verify_directories", verified.directories);
    _tally.add("loop.packs_compacted", turn.stages.packs_compacted);
    if (turn.stages.retention_compacted) _tally.add("loop.retention_compacted");
    _tally.add("loop.rebalanced", turn.stages.rebalance.objects);
    _tally.add("loop.scrubbed", turn.stages.scrub.objects);
  }
  void arm_retirement_wake() {
    const auto grace = std::chrono::duration_cast<std::chrono::nanoseconds>(_grace).count();
    const auto now_ns = static_cast<int64_t>(dannan::contract::system_wall_clock().unix_ms()) * 1'000'000;
    std::optional<int64_t> earliest;
    const auto inventory = _node.ledger().inventory();
    if (!inventory) return;
    for (const auto& candidate : inventory->garbage()) {
      if (candidate.retired_at_ns <= 0) continue;
      const auto elapsed = now_ns - candidate.retired_at_ns;
      if (elapsed >= grace) continue;
      const auto remaining = grace - elapsed;
      if (!earliest || remaining < *earliest) earliest = remaining;
    }
    if (earliest) _gc_quiet_until = dannan::contract::steady_time_source().now() + std::chrono::nanoseconds(*earliest);
  }
  // Up to the next wake: a change, or the bound. Not for correctness: for the load.
  void park() {
    _at.store(-1);
    const auto floor = Steady::now() + std::chrono::milliseconds(_params.pass_floor_ms);
    const auto bound = Steady::now() + 500ms;
    while (!_stop.stop_requested() && Steady::now() < bound) {
      std::this_thread::sleep_for(2ms);
      if (Steady::now() >= floor && (events() != _observed_event || _head.generation() != _last_generation)) return;
    }
  }

  ClusterNode& _node;
  NodeOwner& _owner;
  Head& _head;
  Tally& _tally;
  const Params& _params;
  const std::chrono::milliseconds _grace;
  std::stop_source _stop;
  std::atomic<int> _at{-1};
  uint64_t _observed_event = 0;
  uint64_t _last_generation = 0;
  uint64_t _turns = 0;
  TimePoint _network_quiet_until{};
  TimePoint _gc_quiet_until{};
  std::thread _thread;
};

// ---- Held ledgers on tiny options, and readers across their checkpoints --------

// A node's two disks' ledgers as the store uses them, on options that checkpoint and
// rewrite constantly: writes queued under one lock and flushed outside it, storms of
// removals, and readers holding views across all of that.
class TrieChurn {
 public:
  TrieChurn(std::filesystem::path dir, const Params& params, Tally& tally) : _params(params), _tally(tally) {
    for (int i = 0; i < 2; ++i) {
      _ledgers.push_back(
          std::make_unique<ledger::HeldLedger>(dir / ("held-" + std::to_string(i)), test::key_of(0x4e), test::small_options(), test::quiet_logger()));
      _ledgers.back()->seed([] { return std::vector<ObjectId>{}; });
    }
  }
  ~TrieChurn() { stop(); }
  void start() {
    for (size_t l = 0; l < _ledgers.size(); ++l)
      for (int w = 0; w < 2; ++w) _threads.emplace_back([this, l, w] { guarded(_tally, "trie.writer", [&] { writer(l, w); }); });
    for (int r = 0; r < 2; ++r) _threads.emplace_back([this, r] { guarded(_tally, "trie.reader", [&] { reader(r); }); });
    _threads.emplace_back([this] { guarded(_tally, "trie.stats", [&] { stats(); }); });
  }
  void stop() {
    _stop = true;
    for (auto& thread : _threads) thread.join();
    _threads.clear();
  }

 private:
  void writer(size_t index, int which) {
    auto& ledger = *_ledgers[index];
    std::mt19937_64 random(_params.seed * 1000 + index * 10 + static_cast<uint64_t>(which));
    std::vector<ObjectId> held;
    const auto most = std::max<size_t>(_params.trie_records / 2, 16);
    while (!_stop.load()) {
      // Mostly a trickle; now and then a storm that takes out most of what it wrote.
      if (held.size() >= most || random() % 64 == 0) {
        const auto count = held.size() / 2 + 1;
        for (size_t i = 0; i < count && !held.empty(); ++i) {
          ledger.record(held.back(), false);
          held.pop_back();
          if (i % 128 == 127) ledger.flush();
        }
        std::shuffle(held.begin(), held.end(), random);
        _tally.add("trie.storms");
      } else {
        const auto count = 1 + random() % 64;
        for (size_t i = 0; i < count; ++i) {
          const auto id = random() % 2 ? test::id_of(random) : test::clustered_id(random);
          ledger.record(id, true);
          held.push_back(id);
        }
      }
      ledger.flush();
      _tally.add("trie.flushes");
    }
  }
  void reader(int which) {
    std::mt19937_64 random(_params.seed * 2000 + static_cast<uint64_t>(which));
    struct Kept {
      std::vector<ledger::HeldLedger::View> views;
      std::shared_ptr<const ledger::TrieUnion> merged;
      ObjectTrie::Summary root;
    };
    std::deque<Kept> kept;
    std::shared_ptr<const ledger::TrieUnion> hint;
    uint64_t round = 0;
    while (!_stop.load()) {
      ++round;
      Kept now;
      std::vector<ObjectTrie::Snapshot> parts;
      for (auto& ledger : _ledgers) {
        now.views.push_back(ledger->view());
        parts.push_back(now.views.back().records);
      }
      now.merged = std::make_shared<const ledger::TrieUnion>(parts, hint.get());
      now.root = now.merged->root();
      hint = now.merged;
      // The union with a hint is the union without one.
      if (round % 16 == 0) {
        const ledger::TrieUnion fresh(parts);
        if (fresh.root() != now.root) _tally.fail("a union made with a hint differs from one made without");
      }
      kept.push_back(std::move(now));
      if (kept.size() > 16) kept.pop_front();
      // Old ones as well as the new: their generations' files are gone by now.
      read_views(kept[random() % kept.size()], random);
      read_views(kept.back(), random);
      _tally.add("trie.reader_rounds");
    }
    (void)kept;
  }
  template <class Kept>
  void read_views(const Kept& at, std::mt19937_64& random) {
    if (at.merged->root() != at.root) _tally.fail("a kept union changed");
    for (const auto& view : at.views) {
      const auto& snapshot = view.records;
      const auto page = snapshot.next(std::nullopt, 64);
      for (const auto& [id, value] : page) {
        if (!snapshot.get(id)) _tally.fail("a kept snapshot lost a record it listed");
        if (!at.merged->get(id)) _tally.fail("a kept union lost a record a part listed");
        _tally.add("trie.snapshot_gets");
      }
      if (snapshot.size() < 4096 && random() % 16 == 0 && snapshot.ids().size() != snapshot.size())
        _tally.fail("a kept snapshot's walk disagrees with its size");
      const auto probe = test::id_of(random);
      (void)snapshot.get(probe);
      const auto prefix = test::prefix_of(probe, static_cast<uint8_t>(random() % 3));
      (void)snapshot.children(std::span(&prefix, 1));
      (void)at.merged->children(std::span(&prefix, 1));
      try {
        (void)at.merged->records(std::span(&prefix, 1), 1024);
      } catch (const std::length_error&) {
      }
    }
  }
  // The owner's other readers: the figures, and the verify's lookup.
  void stats() {
    std::mt19937_64 random(_params.seed * 3000);
    while (!_stop.load()) {
      for (auto& ledger : _ledgers) {
        const auto figures = ledger->stats();
        _checkpoints.store(std::max(_checkpoints.load(), figures.checkpoints));
        _rewrites.store(std::max(_rewrites.load(), figures.rewrites));
        (void)ledger->size();
        (void)ledger->held(test::id_of(random));
        (void)ledger->held_with_prefix(static_cast<uint16_t>(random()));
      }
      _tally.add("trie.stats_rounds");
    }
    _tally.add("trie.checkpoints_seen_most", _checkpoints.load());
    _tally.add("trie.rewrites_seen_most", _rewrites.load());
  }

  const Params& _params;
  Tally& _tally;
  std::vector<std::unique_ptr<ledger::HeldLedger>> _ledgers;
  std::vector<std::thread> _threads;
  std::atomic_bool _stop{false};
  std::atomic<uint64_t> _checkpoints{0};
  std::atomic<uint64_t> _rewrites{0};
};

// ---- The cluster under it ------------------------------------------------------

test::NeutralItem item_naming(const ObjectId& id) {
  test::NeutralItem item;
  item.kind = 2;
  test::NeutralRef ref;
  ref.start = 0;
  ref.span = 4096;
  ref.id = id;
  item.refs.push_back(ref);
  return item;
}

class StressBed {
 public:
  explicit StressBed(const Params& params) : _params(params) {}
  ~StressBed() {
    for (auto& node : _nodes) node->loop.reset();
  }

  void run() {
    const auto grace = std::chrono::milliseconds(_params.grace_ms);
    const std::string describe = describe_params();
    std::cout << "STRESS params " << describe << std::endl;
    for (size_t i = 0; i < _params.nodes; ++i) add_node(i, grace);
    _cluster.start_all();
    ASSERT_TRUE(_cluster.converged(60s));
    for (size_t i = 0; i < _params.nodes; ++i) {
      ASSERT_TRUE(test::wait_until([&] { return _nodes[i]->node->data().held_view().identity.complete; }, 60s));
      found(*_nodes[i]->node);
      start_maintenance(i);
    }

    TrieChurn churn(_cluster.dir() / "trie-churn", _params, _tally);
    churn.start();
    std::vector<std::thread> threads;
    const auto spawn = [&](const std::string& where, std::function<void()> body) {
      threads.emplace_back([this, where, body = std::move(body)] { guarded(_tally, where, body); });
    };
    size_t index = 0;
    for (size_t i = 0; i < _params.nodes; ++i) {
      for (size_t w = 0; w < _params.writers; ++w) {
        const auto seed = ++index;
        spawn("writer", [this, i, seed] { writer(i, seed); });
      }
      const auto seed = ++index;
      spawn("held", [this, i, seed] { held_reader(i, seed); });
      const auto seed2 = ++index;
      spawn("reader", [this, i, seed2] { object_reader(i, seed2); });
      const auto seed3 = ++index;
      spawn("space", [this, i, seed3] { space_churn(i, seed3); });
    }
    for (size_t d = 0; d < _params.deleters; ++d) {
      const auto seed = ++index;
      spawn("deleter", [this, seed] { deleter(seed); });
    }
    if (_params.restart_seconds) spawn("chaos", [this, grace] { chaos(grace); });

    // The writers fill the head; then the phases, for as long as it was asked.
    const bool preloaded = test::wait_until([&] { return _head.live_count() >= _params.preload; }, 300s);
    EXPECT_TRUE(preloaded) << "live=" << _head.live_count();
    const auto started = Steady::now();
    std::mt19937_64 random(_params.seed);
    while (preloaded && Steady::now() - started < std::chrono::seconds(_params.seconds)) cycle(random, started);
    _phase.store(Phase::lull);
    _running.store(false);
    for (auto& thread : threads) thread.join();
    threads.clear();
    churn.stop();
    const auto ran_seconds = std::chrono::duration_cast<std::chrono::seconds>(Steady::now() - started).count();
    _tally.add("load.seconds", static_cast<uint64_t>(ran_seconds));

    finish();
    for (auto& node : _nodes) node->loop.reset();
    for (auto& node : _nodes) {
      for (const auto& line : node->node->log().lines())
        for (const char* word : {"corrected", "discarding corrupt", "read failed", "taken offline", "seeded path"})
          if (line.find(word) != std::string::npos) std::cout << "STRESS log " << node->node->name() << " " << line.substr(0, 220) << "\n";
      node->node->stop();
    }
    _tally.report(std::cout);
  }

 private:
  enum class Phase : uint8_t { busy, storm, lull };
  struct Member {
    ClusterNode* node = nullptr;
    std::unique_ptr<NodeOwner> owner;
    std::unique_ptr<StressLoop> loop;
    std::shared_mutex gate;
    std::atomic_bool up{false};
    std::atomic<uint64_t> incarnation{0};
    bool automatic = false;
  };

  std::string describe_params() const {
    std::ostringstream out;
    out << "seconds=" << _params.seconds << " settle=" << _params.settle_seconds << " seed=" << _params.seed << " nodes=" << _params.nodes
        << " writers=" << _params.writers << " preload=" << _params.preload << " max_live=" << _params.max_live << " deleters=" << _params.deleters
        << " batch=" << _params.batch << " storm_objects=" << _params.storm_objects << " restart_seconds=" << _params.restart_seconds
        << " grace_ms=" << _params.grace_ms << " pass_floor_ms=" << _params.pass_floor_ms << " ledger_cache=" << _params.ledger_cache_bytes
        << " trie_records=" << _params.trie_records;
    return out.str();
  }

  void add_node(size_t index, std::chrono::milliseconds grace) {
    const auto name = "n" + std::to_string(index);
    ClusterNodeOptions options;
    options.limit = 8ULL * 1024 * 1024 * 1024;
    options.packed = true;
    options.pack_target_size = 64 * 1024;
    // Two disks: a union of two held tries that differ, on every node.
    options.more_backends = {_cluster.dir() / name / "data2"};
    options.durability_window = 5ms;
    options.ledger_cache_bytes = _params.ledger_cache_bytes;
    options.cache.path = _cluster.dir() / name / "cache";
    options.cache.max_blocks = 256;
    options.store.replication = 3;
    options.store.write_copies = 2;
    options.store.extent_size = 1024 * 1024;
    options.store.write_stall = 500ms;
    options.store.write_deadline = 10s;
    options.store.read_deadline = 5s;
    options.store.control_call = 3s;
    options.store.call_horizon = 15s;
    options.store.prompt_retry = 1s;
    options.admission_wait = 5s;
    options.no_progress = 10s;
    options.repair.extent_size = options.store.extent_size;
    options.repair.foreground_quiet = 100ms;
    options.repair.peer_window = 500ms;
    options.garbage_grace = grace;
    options.maintenance.collector.retry_window = grace;
    options.maintenance.collector.release_budget = 1024;
    options.maintenance.collector.prune_budget = 1024;
    options.maintenance.collector.data_budget = 1024;
    options.maintenance.collector.control_budget = 256;
    options.maintenance.scrub_interval = 30s;
    options.maintenance.interval = 100ms;
    options.maintenance.gc_interval = 1000ms;
    options.spaces = {test::neutral_spec()};
    auto member = std::make_unique<Member>();
    // The last node runs the node's own maintenance thread, the rest Macha's loop.
    member->automatic = index + 1 == _params.nodes;
    options.maintenance.automatic = member->automatic;
    member->node = &_cluster.add(name, options);
    member->owner = std::make_unique<NodeOwner>(_head, *member->node, _tally);
    _nodes.push_back(std::move(member));
  }
  void start_maintenance(size_t index) {
    auto& member = *_nodes[index];
    if (member.automatic)
      member.node->dagda_node().register_reachability(domain::domain_v1, *member.owner);
    else
      member.loop = std::make_unique<StressLoop>(*member.node, *member.owner, _head, _tally, _params, std::chrono::milliseconds(_params.grace_ms));
    member.up.store(true);
  }

  // Runs `work` on the node if it is up, keeping it up until `work` returns.
  template <class Work>
  bool on(size_t index, Work&& work) {
    auto& member = *_nodes[index];
    if (!member.up.load()) return false;
    std::shared_lock gate(member.gate);
    if (!member.up.load()) return false;
    work(*member.node);
    return true;
  }
  template <class Work>
  void on_guarded(size_t index, const char* where, Work&& work) {
    try {
      (void)on(index, std::forward<Work>(work));
    } catch (const std::exception& error) {
      _tally.error(where, error.what());
    }
  }

  // One turn of the load: busy, then a storm on one node starting all at once, then
  // nothing committing for a while: in a lull the destructive gates can open.
  void cycle(std::mt19937_64& random, Steady::time_point started) {
    const auto left = [&] { return Steady::now() - started < std::chrono::seconds(_params.seconds); };
    _phase.store(Phase::busy);
    const auto busy_until = Steady::now() + std::chrono::milliseconds(1000 + random() % 3000);
    const auto refill_bound = Steady::now() + 120s;
    while (((Steady::now() < busy_until) || (_head.live_count() < _params.preload && Steady::now() < refill_bound)) && left())
      std::this_thread::sleep_for(10ms);
    _storm_node.store(random() % _params.nodes);
    _storm_left.store(static_cast<int64_t>(_params.storm_objects));
    _phase.store(Phase::storm);
    _tally.add("load.storms");
    const auto storm_bound = Steady::now() + 60s;
    while (_storm_left.load() > 0 && Steady::now() < storm_bound && left()) std::this_thread::sleep_for(5ms);
    _storm_left.store(0);
    _phase.store(Phase::lull);
    _tally.add("load.lulls");
    // Now and then long enough for a retirement to mature and a sweep to take it.
    const auto lull = random() % 4 == 0 ? _params.grace_ms + 1000 + random() % 3000 : 500 + random() % 2000;
    const auto lull_until = Steady::now() + std::chrono::milliseconds(lull);
    while (Steady::now() < lull_until && left()) std::this_thread::sleep_for(10ms);
  }

  // ---- Writers ----

  void writer(size_t index, uint64_t seed) {
    std::mt19937_64 random(_params.seed * 7919 + seed);
    const NodeId origin = NodeId::random();
    uint64_t sequence = 0;
    bool unadvanced = false;
    const auto burst = [&] { return 200 + random() % 2000; };
    auto until = Steady::now() + std::chrono::milliseconds(burst());
    bool writing = true;
    while (_running.load()) {
      // Bursts, and none in a lull unless the head has yet to reach its preload.
      if (_phase.load() == Phase::lull && _head.live_count() >= _params.preload) {
        if (unadvanced) {
          _head.advance({origin, sequence});
          unadvanced = false;
        }
        std::this_thread::sleep_for(5ms);
        continue;
      }
      if (Steady::now() >= until) {
        writing = !writing;
        until = Steady::now() + std::chrono::milliseconds(burst());
      }
      if (!writing || _head.live_count() >= _params.max_live) {
        std::this_thread::sleep_for(5ms);
        continue;
      }
      try {
        write_batch(index, random, origin, sequence, unadvanced);
      } catch (const std::exception& error) {
        _tally.error("writer", error.what());
      }
    }
    if (unadvanced) _head.advance({origin, sequence});
  }

  void write_batch(size_t index, std::mt19937_64& random, const NodeId& origin, uint64_t& sequence, bool& unadvanced) {
    const size_t count = 1 + random() % 16;
    std::vector<Head::Made> made;
    made.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      if (random() % 64 == 0) {
        // The same content again, after a retirement: it comes back to life.
        if (const auto revived = _head.revive(random)) {
          made.push_back(*revived);
          _tally.add("write.revivals");
          continue;
        }
      }
      const auto n = _next_object.fetch_add(1);
      // Most fit a pack; one in sixteen is a loose file, past the pack's size.
      const uint32_t size = random() % 16 == 0 ? 96 * 1024 : 512 + static_cast<uint32_t>(random() % (8 * 1024));
      made.push_back({store::object_id(bytes_of(n, size)), n, size});
    }
    const auto reviving = [&](const Head::Made& one) {
      const auto entry = _head.entry(one.id);
      return entry && entry->stage == Head::Stage::reviving;
    };
    const auto give_up = [&](const Head::Made& one) {
      if (reviving(one)) _head.abandon_revival(one.id);
    };
    const RetentionDot dot{origin, ++sequence};
    unadvanced = true;
    std::vector<Head::Made> standing;
    const bool ran = on(index, [&](ClusterNode& node) {
      store::WriteOptions options;
      options.level = store::Durability::replicated;
      options.frame = random() % 2 ? dannan::sched::FrameType::foreground : dannan::sched::FrameType::loader;
      std::vector<ObjectId> ids;
      for (const auto& one : made) {
        const auto bytes = bytes_of(one.n, one.size);
        const auto evidence = node.store().write(one.id, bytes, options);
        _tally.add("write.objects");
        _tally.add("write.bytes", bytes.size());
        if (evidence.holders() == 0) {
          _tally.add("write.landed_nowhere");
          continue;
        }
        ids.push_back(one.id);
      }
      const auto unheld = node.store().retain_data(ids, dot);
      _tally.add("write.claims", ids.size());
      for (const auto& one : made) {
        const bool written = std::find(ids.begin(), ids.end(), one.id) != ids.end();
        if (!written || std::find(unheld.begin(), unheld.end(), one.id) != unheld.end()) continue;
        standing.push_back(one);
      }
    });
    if (!ran) {
      for (const auto& one : made) give_up(one);
      return;
    }
    // Referred to only if a claim stood on some node: otherwise it is left to the sweep.
    std::vector<Head::Made> commit;
    for (const auto& one : standing) {
      if (claimed_somewhere(one.id))
        commit.push_back(one);
      else
        _tally.add("write.unclaimed_discarded");
    }
    for (const auto& one : made)
      if (std::none_of(commit.begin(), commit.end(), [&](const Head::Made& kept) { return kept.id == one.id; })) give_up(one);
    if (commit.empty()) return;
    _head.commit_add(commit, dot);
    unadvanced = false;
    _tally.add("write.committed", commit.size());
  }

  bool claimed_somewhere(const ObjectId& id) {
    for (size_t i = 0; i < _nodes.size(); ++i) {
      bool found_claim = false;
      (void)on(i, [&](ClusterNode& node) { found_claim = node.claims().retained(RetentionClass::data, id); });
      if (found_claim) return true;
    }
    return false;
  }

  // ---- A recursive delete ----

  void deleter(uint64_t seed) {
    std::mt19937_64 random(_params.seed * 104729 + seed);
    const NodeId origin = NodeId::random();
    uint64_t sequence = 0;
    while (_running.load()) {
      if (_phase.load() != Phase::storm || _storm_left.load() <= 0) {
        // Waiting for the storm is the only thing here that polls fast: it starts at once.
        std::this_thread::yield();
        std::this_thread::sleep_for(1ms);
        continue;
      }
      const auto index = _storm_node.load();
      try {
        delete_batch(index, random, origin, sequence);
      } catch (const std::exception& error) {
        _tally.error("deleter", error.what());
      }
    }
  }

  void delete_batch(size_t index, std::mt19937_64& random, const NodeId& origin, uint64_t& sequence) {
    auto ids = _head.doom(random, _params.batch);
    if (ids.empty()) return;
    const auto retired_at = static_cast<int64_t>(dannan::contract::system_wall_clock().unix_ms()) * 1'000'000;
    const RetentionDot dot{origin, ++sequence};
    bool retired = false;
    RetentionClock observed;
    try {
      retired = on(index, [&](ClusterNode& node) {
        auto writer = heads::ControlTreeNodeStore::for_commit(node.control(), node.store());
        const auto batch = heads::write_retirement_batch(writer, batch_magic, retired_at, ids);
        if (!batch) throw std::logic_error("a batch of nothing");
        const auto scope = random() % 2 ? store::DistributedStore::ClaimScope::here : store::DistributedStore::ClaimScope::everywhere;
        if (!node.store().retain_control({batch->id}, dot, scope)) throw std::runtime_error("the batch was not claimed here");
        observed = _head.commit_retire(ids, *batch, dot);
      });
    } catch (...) {
      if (!observed.empty()) throw;
      _head.spare(ids);
      _head.advance(dot);
      throw;
    }
    if (!retired) {
      _head.spare(ids);
      _head.advance(dot);
      return;
    }
    _storm_left.fetch_sub(static_cast<int64_t>(ids.size()));
    _tally.add("delete.retired", ids.size());
    _tally.add("delete.batches");
    // Half go at once as well. The rest wait for the collector: it is what Macha at
    // 9a08846 does for all of them, its instant delete being Dagda's.
    if (random() % 2) return;
    (void)on(index, [&](ClusterNode& node) {
      const auto evidence = node.store().remove(ids, observed, {}, 5000ms);
      _tally.add(evidence.finished ? "delete.instant_finished" : "delete.instant_unfinished");
      _tally.add("delete.instant_copies_removed", evidence.removed());
      for (const auto& on_node : evidence.nodes)
        for (const auto& object : on_node.objects) _tally.add(std::string("delete.instant_state.") + store::remove_state_name(object.state));
    });
  }

  // ---- Readers ----

  void object_reader(size_t index, uint64_t seed) {
    std::mt19937_64 random(_params.seed * 15485863 + seed);
    while (_running.load()) {
      const auto sample = _head.sample(random, 16);
      if (sample.empty()) {
        std::this_thread::sleep_for(5ms);
        continue;
      }
      const auto phase = _phase.load();
      on_guarded(index, "reader", [&](ClusterNode& node) {
        for (const auto& one : sample) read_one(node, one, random, phase);
      });
    }
  }

  void read_one(ClusterNode& node, const Head::Made& one, std::mt19937_64& random, Phase phase) {
    const auto expected = bytes_of(one.n, one.size);
    // Strict: a claim here on what the head names means this copy is here, whatever
    // else is happening. Nothing but a delete may take it, and the head knows of those.
    const bool claimed = node.claims().retained(RetentionClass::data, one.id);
    const auto local = node.data().get(one.id);
    if (local && *local != expected) _tally.fail("a local read returned other bytes for " + store::to_string(one.id));
    if (claimed && !local && _head.stands(one.id)) {
      // Reread under the stage: a claim released by a delete that began since is no loss.
      const auto again = node.claims().retained(RetentionClass::data, one.id);
      if (again && _head.stands(one.id)) _tally.fail(node.name() + " holds a claim on a live object it cannot read: " + explain(node, one.id));
    }
    _tally.add(claimed ? "read.local_claimed" : "read.local_unclaimed");
    (void)node.data().has(one.id);
    (void)node.data().valid(one.id);
    (void)node.store().locally_available(one.id);
    // A store read counts as foreground work, which shuts the destructive gates: none
    // in a lull.
    if (phase == Phase::lull || random() % 4) return;
    store::ReadOptions options;
    options.deadline = 3000ms;
    const auto read = node.store().read(one.id, options);
    if (read.object) {
      if (read.object->bytes != expected) _tally.fail("a store read returned other bytes for " + store::to_string(one.id));
      _tally.add("read.store_found");
    } else if (read.answer == store::FetchAnswer::not_found) {
      // Soft in the run, as a stopped node may hold the only copy it knows of. The end
      // reads everything again.
      _tally.add(_head.stands(one.id) ? "read.store_not_found_while_live" : "read.store_not_found_while_deleting");
    } else {
      _tally.add("read.store_try_later");
    }
  }

  void held_reader(size_t index, uint64_t seed) {
    std::mt19937_64 random(_params.seed * 32452843 + seed);
    uint64_t removals = 0;
    uint64_t incarnation = ~0ULL;
    std::deque<contract::HeldView> kept;
    while (_running.load()) {
      const auto sample = _head.sample(random, 32);
      on_guarded(index, "held", [&](ClusterNode& node) {
        auto& member = *_nodes[index];
        if (const auto now = member.incarnation.load(); now != incarnation) {
          incarnation = now;
          removals = 0;
          kept.clear();
        }
        auto view = node.data().held_view();
        if (view.identity.removals < removals) _tally.fail(node.name() + " held view's removals fell");
        removals = view.identity.removals;
        _tally.add("held.views");
        if (view.identity.complete) _tally.add("held.views_complete");
        read_view(view, sample, random);
        // Each class through the ledger too, and the control store's own.
        const auto control = node.ledger().held_view(RetentionClass::control);
        read_view(control, sample, random);
        if (const auto inventory = node.ledger().inventory()) {
          for (const auto& one : sample) (void)inventory->referenced(RetentionClass::data, one.id);
          (void)inventory->next(RetentionClass::data, std::nullopt, 64);
          if (view.trie && random() % 8 == 0) _tally.add("held.not_held", inventory->not_held(RetentionClass::data, view.trie->parts()).size());
          _tally.add("held.inventories");
        }
        if (const auto release = node.ledger().release()) {
          for (const auto& one : sample) (void)release->referenced(RetentionClass::data, one.id);
        }
        // Views kept across the checkpoints, the rewrites and the node's own new views.
        kept.push_back(std::move(view));
        if (kept.size() > 8) kept.pop_front();
        read_view(kept[random() % kept.size()], sample, random);
        (void)node.data().diagnostics();
        (void)node.data().used();
      });
      std::this_thread::sleep_for(1ms);
    }
  }

  void read_view(const contract::HeldView& view, const std::vector<Head::Made>& sample, std::mt19937_64& random) {
    if (!view.trie) return;
    const auto& trie = *view.trie;
    for (const auto& one : sample) {
      (void)trie.get(one.id);
      (void)view.held(one.id);
    }
    _tally.add("held.gets", sample.size() * 2);
    const auto size = trie.size();
    for (const auto& part : trie.parts()) {
      const auto page = part.next(std::nullopt, 64);
      for (const auto& [id, value] : page)
        if (!part.get(id)) _tally.fail("Snapshot::get lost a record its next listed");
      _tally.add("held.snapshot_gets", page.size());
    }
    ObjectId probe{};
    for (auto& byte : probe.bytes) byte = static_cast<uint8_t>(random());
    const auto prefix = test::prefix_of(probe, static_cast<uint8_t>(random() % 3));
    (void)trie.children(std::span(&prefix, 1));
    try {
      (void)trie.records(std::span(&prefix, 1), 2048);
    } catch (const std::length_error&) {
    }
    if (trie.root().count != size) _tally.fail("a held union's root moved");
  }

  // ---- A real head space on each node ----

  void found(ClusterNode& node) {
    auto& space = space_on(node);
    test::NeutralHead head;
    head.floor = 1;
    auto writer = heads::ControlTreeNodeStore::for_commit(node.control(), node.store());
    head.root = test::neutral_tree().build({{"a", test::make_group(1)}}, writer);
    auto record = test::record_of(2, space.replica().committed().hash, head);
    ASSERT_TRUE(space.replica().store_commit(record));
    ASSERT_TRUE(space.replica().accept_commit({record.generation, record.hash, 1, {node.id()}}));
  }
  static Space& space_on(ClusterNode& node) { return *node.dagda_node().spaces()->find(node.api().heads().id_of("test/neutral")); }

  void space_churn(size_t index, uint64_t seed) {
    std::mt19937_64 random(_params.seed * 49979687 + seed);
    std::deque<std::pair<std::string, ObjectId>> named;
    uint64_t made = 0;
    while (_running.load()) {
      on_guarded(index, "space", [&](ClusterNode& node) {
        auto& space = space_on(node);
        const auto n = _next_object.fetch_add(1);
        const auto bytes = bytes_of(n, 4096);
        const auto id = store::object_id(bytes);
        _head.exempt(id);
        store::WriteOptions options;
        options.level = store::Durability::persisted;
        if (!node.store().write(id, bytes, options).met) return;
        const auto key = "k" + std::to_string(++made);
        (void)space.mutate_delta(test::neutral_mutation([&](test::NeutralEditing& editing) { editing.delta.upsert_keys[key] = item_naming(id); }));
        named.emplace_back(key, id);
        _tally.add("space.commits");
        if (named.size() > 32) {
          // The delete a namespace makes: the key goes, its object is retired in a batch.
          const auto [old_key, old_id] = named.front();
          named.pop_front();
          (void)space.mutate_delta(test::neutral_mutation([&](test::NeutralEditing& editing) {
            editing.delta.erase_keys.push_back(old_key);
            editing.retire.push_back({true, 0, {old_id}});
            const auto batches = test::neutral_batches(editing.head);
            if (batches.size() > 16) editing.drop_batch(batches.front().id);
          }));
          _tally.add("space.retirements");
        }
        const auto view = space.current();
        if (view) {
          auto reader = heads::ControlTreeNodeStore::for_reading(node.control(), node.store());
          size_t unreadable = 0;
          const auto all =
              heads::retirements_of(test::neutral_batches(*test::NeutralCodec::as_head(view->state)), reader, test::neutral_batch_magic, &unreadable);
          if (unreadable) _tally.add("space.batches_unreadable", unreadable);
          _tally.add("space.retirements_read", all.size());
        }
      });
      // Keeps what it leaves claimed modest: this is not the load.
      std::this_thread::sleep_for(std::chrono::milliseconds(10 + random() % 40));
    }
  }

  // ---- Stops ----

  void chaos(std::chrono::milliseconds grace) {
    std::mt19937_64 random(_params.seed * 67867967);
    static const std::vector<std::string> steps{"inventory", "survey", "walks", "transfer", "settle", "tombstones", "release", "control", "data", "local"};
    auto next = Steady::now() + std::chrono::seconds(_params.restart_seconds);
    uint64_t round = 0;
    while (_running.load()) {
      if (Steady::now() < next) {
        std::this_thread::sleep_for(20ms);
        continue;
      }
      ++round;
      const auto index = random() % _params.nodes;
      guarded(_tally, "chaos", [&] { restart(index, steps[random() % steps.size()], round, grace); });
      next = Steady::now() + std::chrono::seconds(_params.restart_seconds);
    }
  }

  void restart(size_t index, const std::string& step, uint64_t round, std::chrono::milliseconds grace) {
    auto& member = *_nodes[index];
    // Macha's stop: asked in the middle of a pass; what the pass reaches throws.
    if (member.loop) {
      member.loop->stop(step);
      member.loop.reset();
    }
    member.up.store(false);
    {
      std::unique_lock gate(member.gate);
      const auto stopping = Steady::now();
      member.node->stop();
      _tally.add("chaos.stop_ms", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now() - stopping).count()));
      std::cout << "STRESS restart round=" << round << " " << member.node->name() << " rebuild="
                << (round % 6 == 0   ? "wiped"
                    : round % 3 == 0 ? "unseeded"
                                     : "no")
                << " head_generation=" << _head.generation() << std::endl;
      // Every third restart the held ledgers are rebuilt from the disks: every sixth by
      // losing their files, the others their seeded marker.
      if (round % 3 == 0) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(member.node->state() / "ledger", error)) {
          if (!entry.is_directory() || !entry.path().filename().string().starts_with("data-")) continue;
          if (round % 6 == 0)
            std::filesystem::remove_all(entry.path(), error);
          else
            std::filesystem::remove(entry.path() / "seeded", error);
        }
        _tally.add("chaos.ledger_rebuilds");
      }
      member.node->start();
    }
    member.owner->restarted();
    member.incarnation.fetch_add(1);
    start_maintenance(index);
    (void)grace;
    _tally.add("chaos.restarts");
  }

  // What a node says of an object, and every line it logged naming it.
  std::string explain(ClusterNode& node, const ObjectId& id) {
    std::ostringstream out;
    const auto name = store::to_string(id);
    const auto entry = _head.entry(id);
    out << name << " head_generation=" << _head.generation() << " stage=" << (entry ? static_cast<int>(entry->stage) : -1) << " exempt=" << _head.exempted(id)
        << " has=" << node.data().has(id) << " valid=" << node.data().valid(id) << " get=" << node.data().get(id).has_value()
        << " claimed=" << node.claims().retained(RetentionClass::data, id);
    const auto view = node.data().held_view();
    out << " view_held=" << view.held(id) << " parts=";
    if (view.trie)
      for (const auto& part : view.trie->parts()) out << part.get(id).has_value() << ",";
    out << " on disk:";
    std::error_code error;
    for (const auto& disk : {node.backend(), node.backend().parent_path() / "data2"}) {
      out << " " << disk.filename().string() << "=" << std::filesystem::exists(test::object_path(disk, id), error);
    }
    for (const auto& line : node.log().lines())
      if (line.find(name) != std::string::npos) out << "\n  log: " << line.substr(0, 420);
    return out.str();
  }

  // The disks and the ledgers disagree: name each object, and what each says.
  void explain_disagreement(ClusterNode& node) {
    const auto view = node.data().held_view();
    if (!view.trie) return;
    const auto held = node.data().list();
    const std::set<ObjectId> listed(held.begin(), held.end());
    std::set<ObjectId> viewed;
    for (const auto& part : view.trie->parts())
      for (const auto& id : part.ids()) viewed.insert(id);
    for (const auto& id : viewed)
      if (!listed.contains(id)) ADD_FAILURE() << node.name() << " ledger only: " << explain(node, id);
    for (const auto& id : listed)
      if (!viewed.contains(id)) ADD_FAILURE() << node.name() << " disk only: " << explain(node, id);
  }

  // ---- The end ----

  // What must hold once the load is gone and maintenance has had its time. Nothing here
  // is about how fast: it waits for these or for the bound, and says which stayed false.
  std::vector<std::string> unsettled(bool whole) {
    std::vector<std::string> out;
    const auto live = _head.named_live();
    size_t sampled = 0;
    const auto limit = whole ? size_t{12} : size_t{4};
    const auto note = [&](const std::string& what) {
      if (sampled++ < limit) out.push_back(what);
    };
    if (_head.doomed_count() != 0) note("doomed objects remain");
    if (_head.reviving_count() != 0) note("reviving objects remain");
    for (size_t i = 0; i < _nodes.size(); ++i) {
      auto& node = *_nodes[i]->node;
      const auto held = node.data().list();
      size_t stray = 0;
      for (const auto& id : held)
        if (!live.contains(id) && !_head.exempted(id)) {
          ++stray;
          note(node.name() + " holds what nothing refers to: " + store::to_string(id) + (node.claims().retained(RetentionClass::data, id) ? " (claimed)" : ""));
        }
      for (const auto& id : node.claims().retained_ids(RetentionClass::data))
        if (!live.contains(id) && !_head.exempted(id)) note(node.name() + " keeps a claim on what nothing refers to: " + store::to_string(id));
      const auto view = node.data().held_view();
      if (!view.identity.complete) note(node.name() + " held view incomplete");
      if (view.trie && view.trie->size() != held.size())
        note(node.name() + " held view counts " + std::to_string(view.trie->size()) + " and the disks " + std::to_string(held.size()));
      if (stray && whole) out.push_back(node.name() + " strays=" + std::to_string(stray));
    }
    if (_head.batch_count() != 0) note("retirement batches still named: " + std::to_string(_head.batch_count()));
    return out;
  }

  // Where each node's sweep is: what is left of it, and why its gate last said what it did.
  void report_settling() {
    const auto live = _head.named_live();
    for (auto& member : _nodes) {
      auto& node = *member->node;
      size_t stray = 0;
      for (const auto& id : node.data().list())
        if (!live.contains(id) && !_head.exempted(id)) ++stray;
      std::string sweep;
      for (const auto& line : node.log().lines())
        if (line.find("garbage collection sweep") != std::string::npos) sweep = line;
      std::cout << "STRESS settling " << node.name() << " strays=" << stray << " last: " << sweep << std::endl;
    }
  }

  void finish() {
    std::cout << "STRESS settling live=" << _head.live_count() << " retired=" << _head.retired_count() << " batches=" << _head.batch_count() << std::endl;
    const auto settling = Steady::now();
    for (auto& node : _nodes) ASSERT_TRUE(node->up.load());
    EXPECT_TRUE(_cluster.converged(60s));
    const auto until = settling + std::chrono::seconds(_params.settle_seconds);
    std::vector<std::string> left;
    uint64_t ticks = 0;
    while (true) {
      left = unsettled(false);
      if (left.empty() || Steady::now() >= until) break;
      if (ticks % 40 == 0) report_settling();
      // The head moves on now and then, so that a later CONTROL epoch stands. Not often:
      // each move builds an inventory and shuts the sweep's gate for that pass, and under
      // a sanitizer a pass can outlast a short tick.
      if (++ticks % 60 == 0) _head.tick();
      std::this_thread::sleep_for(500ms);
    }
    _tally.add("settle.seconds", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(Steady::now() - settling).count()));
    report_settling();
    for (const auto& what : unsettled(true)) ADD_FAILURE() << "did not settle: " << what;
    for (auto& member : _nodes)
      if (const auto view = member->node->data().held_view(); view.trie && view.trie->size() != member->node->data().list().size())
        explain_disagreement(*member->node);

    // Every object still claimed reads back.
    const auto all = _head.all_live();
    size_t unread = 0;
    size_t unclaimed = 0;
    std::map<size_t, size_t> holders;
    for (const auto& one : all) {
      const auto expected = bytes_of(one.n, one.size);
      size_t held = 0;
      bool claimed = false;
      for (auto& member : _nodes) {
        auto& node = *member->node;
        claimed = node.claims().retained(RetentionClass::data, one.id) || claimed;
        const auto bytes = node.data().get(one.id);
        if (!bytes) continue;
        ++held;
        if (*bytes != expected) ADD_FAILURE() << node.name() << " holds other bytes for " << store::to_string(one.id);
      }
      ++holders[held];
      if (!held) ++unread;
      if (!claimed) ++unclaimed;
    }
    EXPECT_EQ(unread, 0U) << "live objects no node can read";
    EXPECT_EQ(unclaimed, 0U) << "live objects no node claims";
    for (const auto& [count, objects] : holders) _tally.add("end.objects_on_" + std::to_string(count) + "_nodes", objects);
    _tally.add("end.live", all.size());
    _tally.add("end.retired", _head.retired_count());

    // Through the store, from every node.
    std::mt19937_64 random(_params.seed * 86028121);
    for (auto& member : _nodes) {
      auto& node = *member->node;
      for (size_t i = 0; i < std::min<size_t>(_params.final_reads, all.size()); ++i) {
        const auto& one = all[random() % all.size()];
        store::ReadOptions options;
        options.deadline = 15000ms;
        const auto read = node.store().read(one.id, options);
        if (!read.object)
          ADD_FAILURE() << node.name() << " cannot read a live object: " << store::to_string(one.id) << " answer=" << static_cast<int>(read.answer);
        else if (read.object->bytes != bytes_of(one.n, one.size))
          ADD_FAILURE() << node.name() << " read other bytes for " << store::to_string(one.id);
        _tally.add("end.store_reads");
      }
    }

    // Every retired object stays gone: on no disk (unsettled() said so of what each node
    // lists), and no node's store finds it. A read cache keeps its copy until its own
    // eviction, by design (S19): that is counted, and names where it came from.
    const auto retired = _head.all_retired();
    for (auto& member : _nodes) {
      auto& node = *member->node;
      for (size_t i = 0; i < std::min<size_t>(_params.final_reads, retired.size()); ++i) {
        const auto& one = retired[random() % retired.size()];
        if (node.data().has(one.id)) ADD_FAILURE() << node.name() << " holds a removed object: " << store::to_string(one.id);
        store::ReadOptions options;
        options.deadline = 15000ms;
        const auto read = node.store().read(one.id, options);
        if (!read.object) {
          _tally.add(read.answer == store::FetchAnswer::not_found ? "end.retired_not_found" : "end.retired_try_later");
        } else if (read.source == store::ReadSource::cache) {
          _tally.add("end.retired_from_a_read_cache");
        } else {
          ADD_FAILURE() << node.name() << " read a removed object, source " << static_cast<int>(read.source) << ": " << store::to_string(one.id);
        }
      }
      for (const auto& id : _head.dropped_batches())
        if (node.control().has(id)) _tally.add("end.dropped_batch_still_held");
    }

    // The counts agree: what the inventory counts is what the head names.
    for (size_t i = 0; i + 1 < _nodes.size(); ++i) {
      auto& node = *_nodes[i]->node;
      const auto reached = test::wait_until(
          [&] {
            const auto inventory = node.ledger().inventory();
            return inventory && inventory->generation() == _head.generation();
          },
          30s);
      EXPECT_TRUE(reached) << node.name() << " inventory never reached the head";
      if (!reached) continue;
      const auto inventory = node.ledger().inventory();
      EXPECT_EQ(inventory->size(RetentionClass::data), all.size()) << node.name();
      EXPECT_TRUE(inventory->complete()) << node.name();
    }
    size_t total = 0;
    for (const auto& [count, objects] : holders) total += objects;
    EXPECT_EQ(total, all.size());
  }

  const Params _params;
  Tally _tally;
  // The owners and the head outlive the nodes that call them: the cluster stops first.
  Head _head;
  std::vector<std::unique_ptr<Member>> _nodes;
  Cluster _cluster{std::chrono::milliseconds(1500)};
  std::atomic_bool _running{true};
  std::atomic<Phase> _phase{Phase::busy};
  std::atomic<size_t> _storm_node{0};
  std::atomic<int64_t> _storm_left{0};
  std::atomic<uint64_t> _next_object{1};
};

TEST(DeleteStress, DISABLED_AStormOfDeletesOnABusyCluster) {
  ::signal(SIGABRT, print_stack_and_die);
  const Params params;
  // Each StressBed holds its cluster, and everything stops before it goes.
  StressBed bed(params);
  bed.run();
}

// The held tries alone, at a far higher rate than beside a cluster.
TEST(DeleteStress, DISABLED_HeldTriesThroughCheckpointsAndRewrites) {
  const Params params;
  Tally tally;
  dannan::test::TempDir dir;
  {
    TrieChurn churn(dir.path(), params, tally);
    churn.start();
    const auto until = Steady::now() + std::chrono::seconds(params.seconds);
    while (Steady::now() < until) std::this_thread::sleep_for(50ms);
  }
  tally.report(std::cout);
}

}  // namespace
}  // namespace dagda
