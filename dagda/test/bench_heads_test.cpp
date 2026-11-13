//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Slice 0's head-commit figures (TODO/2026-11-13-three-systems-design.md, "The commit
// rate, measured first", 1 and 3): today's head commit, on the in-process cluster over
// the neutral owner, on the real clock. Recorded, never asserted; disabled, so run with
// --gtest_also_run_disabled_tests --gtest_filter='BenchHeads.*'. Each prints BENCH lines.
//
// A commit here returns once this node holds it (Macha's persisted); a peer has it when
// its own replica's generation reaches it, which is timed as "visible". A head-backed
// lease's acquire (Lugh's TOTP) would be a commit plus that round to its replicas.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dagda/context.h"
#include "dagda/node.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/heads_space.h"
#include "dagda/test/fixtures/tree.h"

namespace dagda {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::founded_spec;
using test::make_group;
using test::neutral_mutation;
using test::NeutralEditing;

double ms_between(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Figures {
  std::mutex mutex;
  std::vector<double> ms;
  void add(double v) {
    std::lock_guard lock(mutex);
    ms.push_back(v);
  }
  double at(double q) {
    std::lock_guard lock(mutex);
    if (ms.empty()) return 0;
    std::sort(ms.begin(), ms.end());
    return ms[static_cast<size_t>(q * static_cast<double>(ms.size() - 1))];
  }
  size_t n() {
    std::lock_guard lock(mutex);
    return ms.size();
  }
};

void bench(const std::string& name, const std::string& labels, Figures& f, double seconds) {
  char line[512];
  std::snprintf(line, sizeof(line), "BENCH name=%s %s n=%zu per_s=%.1f p50_ms=%.2f p90_ms=%.2f p99_ms=%.2f max_ms=%.2f", name.c_str(), labels.c_str(),
                f.n(), seconds > 0 ? static_cast<double>(f.n()) / seconds : 0.0, f.at(0.5), f.at(0.9), f.at(0.99), f.at(1.0));
  std::cout << line << std::endl;
}

heads::Space& space_of(ClusterNode& node, const heads::SpaceId& id) {
  auto* found = node.dagda_node().spaces()->find(id);
  if (!found) throw std::runtime_error("no such space");
  return *found;
}

// Times, for sampled commits, when `peer`'s replica of the space first holds a generation
// at least the commit's: one watcher thread polling each pending sample.
class Visibility {
 public:
  explicit Visibility(ClusterNode& peer) : _peer(peer), _thread([this] { run(); }) {}
  ~Visibility() {
    _stop = true;
    _thread.join();
  }
  void expect(const heads::SpaceId& space, uint64_t generation, Clock::time_point committed) {
    std::lock_guard lock(_mutex);
    _pending.push_back({space, generation, committed});
  }
  Figures& figures() { return _figures; }
  size_t unseen() {
    std::lock_guard lock(_mutex);
    return _pending.size();
  }
  void drain(std::chrono::milliseconds within) {
    const auto until = Clock::now() + within;
    while (unseen() > 0 && Clock::now() < until) std::this_thread::sleep_for(5ms);
  }

 private:
  struct Pending {
    heads::SpaceId space;
    uint64_t generation;
    Clock::time_point committed;
  };
  void run() {
    while (!_stop) {
      {
        std::lock_guard lock(_mutex);
        const auto now = Clock::now();
        std::erase_if(_pending, [&](const Pending& p) {
          if (space_of(_peer, p.space).current_generation() < p.generation) return false;
          _figures.add(ms_between(p.committed, now));
          return true;
        });
      }
      std::this_thread::sleep_for(1ms);
    }
  }
  ClusterNode& _peer;
  std::mutex _mutex;
  std::deque<Pending> _pending;
  Figures _figures;
  std::atomic<bool> _stop{false};
  std::thread _thread;
};

heads::MutationFactory edit(bool tree, uint64_t n) {
  if (tree) return neutral_mutation([n](NeutralEditing& editing) { editing.delta.upsert_keys["k/" + std::to_string(n % 1000)] = make_group(n); });
  return neutral_mutation([n](NeutralEditing& editing) { editing.set_note("k", std::to_string(n)); });
}

// 1. Commits a second and their latency at 1, 16 and 256 spaces, on one node and on
// three, with a note-only change and with a tree change; on three, when node b sees them.
TEST(BenchHeads, DISABLED_CommitsAcrossSpaces) {
  constexpr auto run_for = 5s;
  constexpr int writers = 4;
  for (const int nodes : {1, 3}) {
    for (const int spaces : {1, 16, 256}) {
      for (const bool tree : {false, true}) {
        Cluster cluster;
        ClusterNodeOptions options;
        for (int i = 0; i < spaces; ++i) options.spaces.push_back(founded_spec("bench/" + std::to_string(i)));
        auto& a = cluster.add("a", options);
        ClusterNode* b = nullptr;
        if (nodes == 3) {
          b = &cluster.add("b", options);
          cluster.add("c", options);
        }
        cluster.start_all();
        ASSERT_TRUE(cluster.converged());
        std::vector<heads::SpaceId> ids;
        for (int i = 0; i < spaces; ++i) ids.push_back(a.api().heads().id_of("bench/" + std::to_string(i)));
        std::optional<Visibility> seen;
        if (b) seen.emplace(*b);

        Figures commit;
        std::atomic<uint64_t> next{0};
        std::atomic<uint64_t> refused{0};
        const auto start = Clock::now();
        std::vector<std::thread> threads;
        for (int w = 0; w < writers; ++w) {
          threads.emplace_back([&] {
            while (Clock::now() - start < run_for) {
              const auto n = next++;
              const auto& id = ids[n % ids.size()];
              const auto before = Clock::now();
              const auto result = a.api().heads().mutate_delta(id, edit(tree, n), 10s);
              const auto after = Clock::now();
              if (!result.finished || !result.outcome) {
                ++refused;
                continue;
              }
              commit.add(ms_between(before, after));
              if (seen && n % 4 == 0) seen->expect(id, result.outcome->record.generation, after);
            }
          });
        }
        for (auto& t : threads) t.join();
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        const std::string labels =
            "nodes=" + std::to_string(nodes) + " spaces=" + std::to_string(spaces) + " change=" + (tree ? "tree" : "note") + " refused=" + std::to_string(refused.load());
        bench("heads.commit", labels, commit, seconds);
        if (seen) {
          seen->drain(10s);
          bench("heads.visible_on_b", labels + " unseen=" + std::to_string(seen->unseen()), seen->figures(), 0);
        }
      }
    }
  }
}

// 3. Lugh's shape: one space of 50,000 keys taking 50 commits a second for a minute, and
// when another node sees each: Lugh's "disabled on A, refused on B", and the round a
// head-backed lease's acquire would add to a TOTP sign-in. The commits come from node a
// alone: the neutral owner has no merge for writers on several nodes at once.
TEST(BenchHeads, DISABLED_LughsShape) {
  constexpr int keys = 50000;
  constexpr auto run_for = 60s;
  constexpr auto every = 20ms;
  Cluster cluster;
  ClusterNodeOptions options;
  options.spaces.push_back(founded_spec("lugh/bench"));
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.add("c", options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto id = a.api().heads().id_of("lugh/bench");

  const auto filled_from = Clock::now();
  for (int from = 0; from < keys; from += 1000) {
    const auto filled = a.api().heads().mutate_delta(id, neutral_mutation([from](NeutralEditing& editing) {
                                                       for (int k = from; k < from + 1000; ++k) editing.delta.upsert_keys["user/" + std::to_string(k)] = make_group(k);
                                                     }),
                                                     60s);
    ASSERT_TRUE(filled.finished) << filled.why;
  }
  std::cout << "BENCH name=lugh.fill keys=" << keys << " ms=" << ms_between(filled_from, Clock::now()) << std::endl;

  Visibility seen(b);
  Figures commit;
  std::mt19937_64 random(7);
  uint64_t refused = 0;
  const auto start = Clock::now();
  auto due = start;
  uint64_t n = 0;
  while (Clock::now() - start < run_for) {
    std::this_thread::sleep_until(due);
    due += every;
    const auto key = random() % keys;
    const auto before = Clock::now();
    const auto result =
        a.api().heads().mutate_delta(id, neutral_mutation([key, n](NeutralEditing& editing) { editing.delta.upsert_keys["user/" + std::to_string(key)] = make_group(key * 7 + n); }), 10s);
    const auto after = Clock::now();
    ++n;
    if (!result.finished || !result.outcome) {
      ++refused;
      continue;
    }
    commit.add(ms_between(before, after));
    seen.expect(id, result.outcome->record.generation, after);
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  seen.drain(10s);
  const std::string labels = "keys=" + std::to_string(keys) + " rate=50 refused=" + std::to_string(refused);
  bench("lugh.commit", labels, commit, seconds);
  bench("lugh.visible_on_b", labels + " unseen=" + std::to_string(seen.unseen()), seen.figures(), 0);
}

}  // namespace
}  // namespace dagda
