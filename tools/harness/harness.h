//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// The side-by-side harness (TODO/2026-11-15-side-by-side-harness.md): one set of
// workloads, written once against Harness::Store, run against Macha's store at f0597a4
// (harness_macha.cpp) and against Dagda (harness_dagda.cpp), each writing the same JSON
// figures. Only the adapters differ; a workload is the same bytes in the same order on
// both. Header-only and standard C++, so the Macha half needs nothing of Dagda's.

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace harness {

using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

// The work classes both stores share.
enum class Frame : uint8_t { foreground, read_ahead, loader, speculative };

// A content address, opaque here: each adapter makes and reads its own.
struct Id {
  std::array<uint8_t, 32> bytes{};
  auto operator<=>(const Id&) const = default;
};

// What a workload asks of a store. Each call blocks on the caller's thread and is
// thread-safe, as both stores' are. Node indices run from 0 to nodes() - 1.
class Store {
 public:
  virtual ~Store() = default;
  // Which store, its commit and the Dannan it runs on, for every figure.
  virtual std::string describe() const = 0;
  virtual size_t nodes() const = 0;
  virtual Id id_of(std::span<const uint8_t> data) const = 0;

  // A blocking put at the store's write floor (write_copies); false when it landed
  // nowhere.
  virtual bool put(size_t node, const Id& id, std::span<const uint8_t> data, Frame frame) = 0;

  // A publication: extents written deferred, then made durable by one barrier over
  // them all (Macha's DurabilityBatch and durability_barrier; Dagda's tickets and a
  // many-ticket wait). finish() is true when every extent reached its floor.
  class Publication {
   public:
    virtual ~Publication() = default;
    virtual bool extent(const Id& id, std::span<const uint8_t> data) = 0;
    virtual bool finish() = 0;
  };
  virtual std::unique_ptr<Publication> publication(size_t node, Frame frame) = 0;

  // A read through the store (local, cache, then peers), as Macha's readers make it: the
  // store's shared buffer, copied once into the result (get_shared on Macha's side, the
  // ReadResult's object on Dagda's). `stripe` is the extent's index in its file, as
  // FUSE's ReadHandle passes it. Nothing when it brought nothing by the deadline.
  virtual std::optional<Bytes> read(size_t node, const Id& id, size_t stripe, Frame frame, std::chrono::milliseconds deadline) = 0;

  // The node's own copy removed and one bounded compaction of dead pack bytes: what the
  // collector does after the garbage grace (Macha removes a deleted file's bytes only
  // there, never at the delete).
  virtual bool remove_local(size_t node, const Id& id) = 0;
  virtual size_t compact(size_t node) = 0;
  // Whether `node`'s own store holds `id` (its DATA, not a cache).
  virtual bool holds(size_t node, const Id& id) = 0;

  // The node's physical barriers so far (syncfs or fsync calls the store made).
  virtual uint64_t barriers(size_t node) const = 0;
};

// ---- Figures ----------------------------------------------------------------

struct ProcessFigures {
  double cpu_seconds = 0;
  uint64_t rss_bytes = 0;
  uint64_t hwm_bytes = 0;
  uint64_t write_bytes = 0;
};

inline uint64_t status_kib(const std::string& key) {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line))
    if (line.rfind(key + ":", 0) == 0) return std::stoull(line.substr(key.size() + 1)) * 1024;
  return 0;
}

inline uint64_t io_field(const std::string& key) {
  std::ifstream in("/proc/self/io");
  std::string line;
  while (std::getline(in, line))
    if (line.rfind(key + ":", 0) == 0) return std::stoull(line.substr(key.size() + 1));
  return 0;
}

inline ProcessFigures process_figures() {
  ProcessFigures out;
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  out.cpu_seconds =
      static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
  out.rss_bytes = status_kib("VmRSS");
  out.hwm_bytes = status_kib("VmHWM");
  out.write_bytes = io_field("write_bytes");
  return out;
}

// Latencies of one kind of call, in microseconds.
class Latencies {
 public:
  void add(Clock::duration took) {
    std::lock_guard lock(_mutex);
    _us.push_back(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(took).count()));
  }
  // p in [0, 1]; 0 when empty.
  uint64_t quantile(double p) const {
    std::lock_guard lock(_mutex);
    if (_us.empty()) return 0;
    auto sorted = _us;
    std::sort(sorted.begin(), sorted.end());
    const auto at = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(at, sorted.size() - 1)];
  }
  size_t count() const {
    std::lock_guard lock(_mutex);
    return _us.size();
  }

 private:
  mutable std::mutex _mutex;
  std::vector<uint64_t> _us;
};

// One JSON line a figure, to stdout: {"store", "workload", "figure", "value", "unit"}.
class Report {
 public:
  Report(std::string store, std::string workload) : _store(std::move(store)), _workload(std::move(workload)) {}
  void figure(const std::string& name, double value, const std::string& unit) const {
    std::ostringstream line;
    line << "{\"store\":\"" << _store << "\",\"workload\":\"" << _workload << "\",\"figure\":\"" << name << "\",\"value\":" << value << ",\"unit\":\"" << unit
         << "\"}";
    std::printf("%s\n", line.str().c_str());
    std::fflush(stdout);
  }
  void latencies(const std::string& name, const Latencies& of) const {
    figure(name + ".p50", static_cast<double>(of.quantile(0.50)), "us");
    figure(name + ".p99", static_cast<double>(of.quantile(0.99)), "us");
    figure(name + ".max", static_cast<double>(of.quantile(1.0)), "us");
    figure(name + ".count", static_cast<double>(of.count()), "calls");
  }
  // The process's and the store's figures across a span of work of `bytes`.
  void span(const ProcessFigures& before, const ProcessFigures& after, Clock::duration took, uint64_t bytes, uint64_t barriers) const {
    const double seconds = std::chrono::duration<double>(took).count();
    figure("seconds", seconds, "s");
    if (bytes) {
      figure("throughput", static_cast<double>(bytes) / seconds / 1e6, "MB/s");
      figure("cpu_per_gb", (after.cpu_seconds - before.cpu_seconds) / (static_cast<double>(bytes) / 1e9), "s/GB");
      figure("write_amplification", static_cast<double>(after.write_bytes - before.write_bytes) / static_cast<double>(bytes), "x");
    }
    figure("cpu", after.cpu_seconds - before.cpu_seconds, "s");
    figure("rss", static_cast<double>(after.rss_bytes), "B");
    figure("rss_hwm", static_cast<double>(after.hwm_bytes), "B");
    figure("disk_write", static_cast<double>(after.write_bytes - before.write_bytes), "B");
    figure("barriers", static_cast<double>(barriers), "calls");
  }

 private:
  std::string _store;
  std::string _workload;
};

// ---- Data -------------------------------------------------------------------

// Deterministic bytes: the same on both stores for the same seed and size. A word at a
// time, so making the data costs the driver little beside the store; it is made inside
// the timed work on both sides alike.
inline Bytes pattern(size_t size, uint64_t seed) {
  Bytes out(size);
  uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 1;
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    std::memcpy(out.data() + i, &x, 8);
  }
  for (; i < size; ++i) out[i] = static_cast<uint8_t>(x >> (8 * (i % 8)));
  return out;
}

inline uint64_t all_barriers(const Store& store) {
  uint64_t total = 0;
  for (size_t n = 0; n < store.nodes(); ++n) total += store.barriers(n);
  return total;
}

constexpr size_t extent_bytes = 4 * 1024 * 1024;

// ---- Workloads (the Macha session's list, 2026-11-15) -------------------------

// 1. Publication: 4 MiB extents written deferred with one barrier a file; one 512 MiB
// file, then `concurrent` files at once (the ingest's max_concurrent_jobs), each
// barriered over its own extents.
inline void publication(Store& store, size_t file_bytes, size_t concurrent, const Report& report) {
  const size_t extents = file_bytes / extent_bytes;
  std::atomic<bool> failed{false};
  Latencies barrier;
  Latencies extent;
  std::mutex ids_mutex;
  std::vector<Id> ids;
  const auto one_file = [&](uint64_t file) {
    auto publishing = store.publication(0, Frame::loader);
    for (size_t e = 0; e < extents; ++e) {
      const auto data = pattern(extent_bytes, file * 1000003 + e);
      const auto id = store.id_of(data);
      {
        std::lock_guard lock(ids_mutex);
        ids.push_back(id);
      }
      const auto wrote = Clock::now();
      if (!publishing->extent(id, data)) failed = true;
      extent.add(Clock::now() - wrote);
    }
    const auto started = Clock::now();
    if (!publishing->finish()) failed = true;
    barrier.add(Clock::now() - started);
  };
  const auto before = process_figures();
  const auto barriers_before = all_barriers(store);
  const auto started = Clock::now();
  if (concurrent <= 1) {
    one_file(1);
  } else {
    std::vector<std::thread> files;
    for (size_t f = 0; f < concurrent; ++f) files.emplace_back(one_file, 100 + f);
    for (auto& file : files) file.join();
  }
  const auto took = Clock::now() - started;
  const auto bytes = file_bytes * std::max<size_t>(1, concurrent);
  const auto after = process_figures();
  report.span(before, after, took, bytes, all_barriers(store) - barriers_before);
  report.latencies("barrier", barrier);
  report.latencies("extent", extent);
  // How far replication had got when the barrier answered: the extents every node
  // already held.
  size_t everywhere = 0;
  for (const auto& id : ids) {
    bool all = true;
    for (size_t n = 0; n < store.nodes() && all; ++n) all = store.holds(n, id);
    if (all) ++everywhere;
  }
  report.figure("replicated_at_barrier", ids.empty() ? 0 : static_cast<double>(everywhere) / static_cast<double>(ids.size()), "fraction");
  // Settled: every extent held by every node, so the replication the barrier did not
  // wait for is counted too, and the stores compare at equal work. Polled each 100 ms,
  // for at most ten minutes.
  const auto settle_until = Clock::now() + std::chrono::minutes(10);
  size_t next_unheld = 0;
  std::vector<std::pair<size_t, Id>> pending;
  for (const auto& id : ids)
    for (size_t n = 0; n < store.nodes(); ++n) pending.push_back({n, id});
  while (next_unheld < pending.size() && Clock::now() < settle_until) {
    while (next_unheld < pending.size() && store.holds(pending[next_unheld].first, pending[next_unheld].second)) ++next_unheld;
    if (next_unheld < pending.size()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const auto settled = process_figures();
  report.figure("settled", next_unheld == pending.size() ? 1 : 0, "bool");
  report.figure("settled_seconds", std::chrono::duration<double>(Clock::now() - started).count(), "s");
  report.figure("settled_cpu_per_gb", (settled.cpu_seconds - before.cpu_seconds) / (static_cast<double>(bytes) / 1e9), "s/GB");
  report.figure("settled_write_amplification", static_cast<double>(settled.write_bytes - before.write_bytes) / static_cast<double>(bytes), "x");
  report.figure("failed", failed ? 1 : 0, "bool");
}

// 2. Small files: `count` files of `size`, each published as FUSE publishes a small file
// (one extent written deferred, then its barrier), by `publishers` at once as Macha's
// commit workers do, so the durability window group-commits barriers across files. Then
// the collector's part, after the garbage grace: every copy removed and the dead bytes
// compacted (packing, removes and compaction: the pattern live-b crashed under).
inline void small_files(Store& store, size_t count, size_t size, size_t publishers, const Report& report) {
  std::vector<Id> ids(count);
  Latencies file;
  std::atomic<size_t> next{0};
  std::atomic<size_t> failed{0};
  const auto before = process_figures();
  const auto barriers_before = all_barriers(store);
  const auto started = Clock::now();
  std::vector<std::thread> workers;
  for (size_t w = 0; w < publishers; ++w)
    workers.emplace_back([&] {
      for (size_t i = next++; i < count; i = next++) {
        const auto data = pattern(size, 7000000 + i);
        ids[i] = store.id_of(data);
        const auto call = Clock::now();
        auto publishing = store.publication(0, Frame::loader);
        if (!publishing->extent(ids[i], data) || !publishing->finish()) ++failed;
        file.add(Clock::now() - call);
      }
    });
  for (auto& worker : workers) worker.join();
  const auto wrote = Clock::now();
  for (const auto& id : ids)
    for (size_t n = 0; n < store.nodes(); ++n) (void)store.remove_local(n, id);
  size_t compactions = 0;
  for (size_t n = 0; n < store.nodes(); ++n)
    for (int round = 0; round < 64; ++round) {
      const auto done = store.compact(n);
      compactions += done;
      if (!done) break;
    }
  const auto took = Clock::now() - started;
  report.span(before, process_figures(), took, count * size, all_barriers(store) - barriers_before);
  report.latencies("file", file);
  report.figure("publish_seconds", std::chrono::duration<double>(wrote - started).count(), "s");
  report.figure("gc_removal_and_compaction_seconds", std::chrono::duration<double>(Clock::now() - wrote).count(), "s");
  report.figure("compactions", static_cast<double>(compactions), "packs");
  report.figure("failed", static_cast<double>(failed.load()), "files");
}

// 4. A viewer under load: foreground reads of objects already held, timed, while a
// loader writes as fast as it can for `seconds`. The viewer must not wait.
inline void viewer_under_load(Store& store, std::chrono::seconds seconds, const Report& report) {
  std::vector<Id> held;
  for (size_t i = 0; i < 64; ++i) {
    const auto data = pattern(extent_bytes, 9000000 + i);
    const auto id = store.id_of(data);
    if (!store.put(0, id, data, Frame::loader)) throw std::runtime_error("the viewer's objects could not be written");
    held.push_back(id);
  }
  // The viewer alone first, for its unloaded latency.
  Latencies alone;
  for (size_t stripe = 0; stripe < held.size(); ++stripe) {
    const auto started = Clock::now();
    if (!store.read(0, held[stripe], stripe, Frame::foreground, std::chrono::seconds(30))) throw std::runtime_error("an unloaded read brought nothing");
    alone.add(Clock::now() - started);
  }
  report.latencies("viewer_alone", alone);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> loaded_bytes{0};
  std::thread loader([&] {
    uint64_t n = 0;
    while (!stop.load()) {
      auto publishing = store.publication(0, Frame::loader);
      for (int e = 0; e < 8 && !stop.load(); ++e) {
        const auto data = pattern(extent_bytes, 20000000 + n++);
        (void)publishing->extent(store.id_of(data), data);
        loaded_bytes += data.size();
      }
      (void)publishing->finish();
    }
  });
  Latencies viewer;
  size_t misses = 0;
  const auto until = Clock::now() + seconds;
  size_t i = 0;
  while (Clock::now() < until) {
    const auto started = Clock::now();
    const auto stripe = i++ % held.size();
    if (!store.read(0, held[stripe], stripe, Frame::foreground, std::chrono::seconds(30))) ++misses;
    viewer.add(Clock::now() - started);
  }
  stop = true;
  loader.join();
  report.latencies("viewer_loaded", viewer);
  report.figure("viewer_misses", static_cast<double>(misses), "reads");
  report.figure("loader_throughput", static_cast<double>(loaded_bytes.load()) / static_cast<double>(seconds.count()) / 1e6, "MB/s");
}

// Idle: both nodes up and converged, nothing asked of the store, for `seconds`. What
// the store's own loops cost a second, which every timed workload carries beside its
// work.
inline void idle(Store& store, std::chrono::seconds seconds, const Report& report) {
  const auto before = process_figures();
  const auto barriers_before = all_barriers(store);
  const auto started = Clock::now();
  std::this_thread::sleep_for(seconds);
  report.span(before, process_figures(), Clock::now() - started, 0, all_barriers(store) - barriers_before);
}

// The workloads by name, as both drivers take them on the command line.
inline int run(Store& store, const std::string& workload) {
  const Report report(store.describe(), workload);
  if (workload == "idle") {
    idle(store, std::chrono::seconds(20), report);
  } else if (workload == "publication-one") {
    publication(store, 512ULL * 1024 * 1024, 1, report);
  } else if (workload == "publication-ten") {
    // 512 MiB a file, as baseline.py's and as ingest's media files are: 5 GiB a run.
    publication(store, 512ULL * 1024 * 1024, 10, report);
  } else if (workload == "small-files") {
    small_files(store, 200, 64 * 1024, 8, report);
  } else if (workload == "viewer-under-load") {
    viewer_under_load(store, std::chrono::seconds(30), report);
  } else {
    std::fprintf(stderr, "unknown workload %s: idle, publication-one, publication-ten, small-files, viewer-under-load\n", workload.c_str());
    return 2;
  }
  return 0;
}

}  // namespace harness
