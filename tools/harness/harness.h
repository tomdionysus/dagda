//
// Dagda - The Distributed Object Store
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
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

// When HARNESS_PERF_CTL names perf's control fifo (perf record -D -1 --control
// fifo:<path>), the profile covers only the measured span: enabled where a workload's
// figures start, disabled where they end, so each store's startup and teardown stay out.
inline void perf_control(const char* command) {
  const char* path = std::getenv("HARNESS_PERF_CTL");
  if (!path || !*path) return;
  std::ofstream out(path);
  out << command << '\n';
}

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
  void clear() {
    std::lock_guard lock(_mutex);
    _us.clear();
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
  perf_control("enable");
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
  perf_control("disable");
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
// compacted (packing, removes and compaction: the pattern inverbeg crashed under).
inline void small_files(Store& store, size_t count, size_t size, size_t publishers, const Report& report) {
  std::vector<Id> ids(count);
  Latencies file;
  std::atomic<size_t> next{0};
  std::atomic<size_t> failed{0};
  perf_control("enable");
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
  const auto after = process_figures();
  perf_control("disable");
  report.span(before, after, took, count * size, all_barriers(store) - barriers_before);
  report.latencies("file", file);
  report.figure("publish_seconds", std::chrono::duration<double>(wrote - started).count(), "s");
  report.figure("gc_removal_and_compaction_seconds", std::chrono::duration<double>(Clock::now() - wrote).count(), "s");
  report.figure("compactions", static_cast<double>(compactions), "packs");
  report.figure("failed", static_cast<double>(failed.load()), "files");
}

// The page cache emptied, as a cold run starts (root only; false when it could not be).
inline bool cold_cache() {
  ::sync();
  std::ofstream out("/proc/sys/vm/drop_caches");
  if (!out) return false;
  out << "3\n";
  out.flush();
  return static_cast<bool>(out);
}

// A file of `bytes` published on node 0 as workload 1 publishes one; its extents' ids in
// order.
inline std::vector<Id> publish_file(Store& store, uint64_t bytes, uint64_t seed) {
  std::vector<Id> ids;
  auto publishing = store.publication(0, Frame::loader);
  for (uint64_t at = 0, e = 0; at < bytes; at += extent_bytes, ++e) {
    const auto data = pattern(static_cast<size_t>(std::min<uint64_t>(extent_bytes, bytes - at)), seed + e);
    ids.push_back(store.id_of(data));
    if (!publishing->extent(ids.back(), data)) throw std::runtime_error("a file's extent was not written");
  }
  if (!publishing->finish()) throw std::runtime_error("a file's barrier was not met");
  return ids;
}

// Every node holds every id, polled each 100 ms for at most `within`.
inline bool settled(Store& store, const std::vector<Id>& ids, std::chrono::seconds within) {
  const auto until = Clock::now() + within;
  for (const auto& id : ids)
    for (size_t n = 0; n < store.nodes(); ++n)
      while (!store.holds(n, id)) {
        if (Clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
  return true;
}

// One file read on `node` as Macha's FUSE reader reads it (the Macha session's review,
// 2026-11-16, from filesystem.cpp:116-210 and fuse_frontend.cpp:6001-6009 at its port
// branch): 1 MiB requests, as baseline.py's unbuffered reads, copied out of the one
// extent the handle holds; the extent covering a request read synchronously on the
// reader's thread at loader class when the reader enters it, the one before dropped
// first; no read-ahead (a FUSE handle tracks no playback, so the hydrator has none to
// make). Each request's latency is added and the first one's is `first`; false when an
// extent came back short or not at all.
inline bool read_file(Store& store, size_t node, const std::vector<Id>& ids, Latencies& requests, Clock::duration& first) {
  constexpr size_t request_bytes = 1024 * 1024;
  constexpr auto deadline = std::chrono::seconds(60);
  Bytes out(request_bytes);
  bool ok = true;
  std::optional<Bytes> extent;
  for (size_t e = 0; e < ids.size(); ++e) {
    auto asked = Clock::now();
    extent.reset();
    extent = store.read(node, ids[e], e, Frame::loader, deadline);
    if (e == 0) first = Clock::now() - asked;
    if (!extent || extent->empty()) {
      ok = false;
      requests.add(Clock::now() - asked);
      continue;
    }
    for (size_t at = 0; at < extent->size(); at += request_bytes) {
      const auto size = std::min(request_bytes, extent->size() - at);
      std::memcpy(out.data(), extent->data() + at, size);
      requests.add(Clock::now() - asked);
      asked = Clock::now();
    }
  }
  return ok;
}

// 3. Reads, local: one file of 512 MiB and four of 128 MiB published on node 0 and held
// by both nodes, the page cache dropped, then read back on node 0: the large file by one
// reader alone, then the four at once, each by its own reader, as baseline.py's read
// phase (which reads existing library files from a random 4 MiB-aligned offset; here
// from the start of files just written, and cold).
inline void reads_local(Store& store, const Report& report) {
  constexpr uint64_t mib = 1024 * 1024;
  const auto large = publish_file(store, 512 * mib, 30000000);
  std::vector<std::vector<Id>> small;
  for (uint64_t f = 0; f < 4; ++f) small.push_back(publish_file(store, 128 * mib, 31000000 + f * 1000));
  std::vector<Id> all = large;
  for (const auto& file : small) all.insert(all.end(), file.begin(), file.end());
  if (!settled(store, all, std::chrono::seconds(600))) throw std::runtime_error("the files did not reach both nodes");
  report.figure("cold", cold_cache() ? 1 : 0, "bool");

  perf_control("enable");
  Latencies alone;
  Clock::duration alone_first{};
  auto before = process_figures();
  auto started = Clock::now();
  const bool alone_ok = read_file(store, 0, large, alone, alone_first);
  auto took = Clock::now() - started;
  auto after = process_figures();
  report.figure("one_reader_first_ms", std::chrono::duration<double, std::milli>(alone_first).count(), "ms");
  report.figure("one_reader_seconds", std::chrono::duration<double>(took).count(), "s");
  report.figure("one_reader_throughput", static_cast<double>(512 * mib) / std::chrono::duration<double>(took).count() / 1e6, "MB/s");
  report.figure("one_reader_cpu_per_gb", (after.cpu_seconds - before.cpu_seconds) / (static_cast<double>(512 * mib) / 1e9), "s/GB");
  report.latencies("one_reader_request", alone);

  report.figure("cold_again", cold_cache() ? 1 : 0, "bool");
  Latencies together;
  std::atomic<bool> together_ok{true};
  before = process_figures();
  started = Clock::now();
  std::vector<std::thread> readers;
  std::vector<Clock::duration> firsts(small.size());
  for (size_t f = 0; f < small.size(); ++f)
    readers.emplace_back([&, f] {
      if (!read_file(store, 0, small[f], together, firsts[f])) together_ok = false;
    });
  for (auto& reader : readers) reader.join();
  took = Clock::now() - started;
  after = process_figures();
  perf_control("disable");
  report.figure("four_readers_first_ms_max", std::chrono::duration<double, std::milli>(*std::max_element(firsts.begin(), firsts.end())).count(), "ms");
  report.figure("four_readers_seconds", std::chrono::duration<double>(took).count(), "s");
  report.figure("four_readers_throughput", static_cast<double>(4 * 128 * mib) / std::chrono::duration<double>(took).count() / 1e6, "MB/s");
  report.figure("four_readers_cpu_per_gb", (after.cpu_seconds - before.cpu_seconds) / (static_cast<double>(4 * 128 * mib) / 1e9), "s/GB");
  report.latencies("four_readers_request", together);
  report.figure("rss_hwm", static_cast<double>(after.hwm_bytes), "B");
  report.figure("failed", alone_ok && together_ok ? 0 : 1, "bool");
}

// 8. Recovery (as agreed with the Macha session, 2026-11-16): a fill, then node 0
// reopened alone over what it left, by a second run of the driver, timed from its
// construction. The fill is here; each driver opens its own node for the reopen and
// reports what it timed with report_reopen.

// The extents a fill leaves, one hex id a line, for the reopen's first read.
inline void write_ids(const std::filesystem::path& path, const std::vector<Id>& ids) {
  std::ofstream out(path, std::ios::trunc);
  for (const auto& id : ids) {
    for (const auto byte : id.bytes) out << "0123456789abcdef"[byte >> 4] << "0123456789abcdef"[byte & 15];
    out << '\n';
  }
  if (!out) throw std::runtime_error("the fill's ids could not be written");
}

inline std::vector<Id> read_ids(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<Id> out;
  std::string line;
  const auto nibble = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
  while (std::getline(in, line)) {
    if (line.size() != 64) continue;
    Id id;
    for (size_t i = 0; i < 32; ++i) id.bytes[i] = static_cast<uint8_t>(nibble(line[2 * i]) << 4 | nibble(line[2 * i + 1]));
    out.push_back(id);
  }
  if (out.empty()) throw std::runtime_error("the fill left no ids at " + path.string());
  return out;
}

// The fill: `large` files of 512 MiB and `small` files of 4 to 64 KiB on node 0, every
// one made durable, the small ones a hundred to a publication by eight publishers (the
// layout on disk is what is tested here, not how it was written). The large files'
// extents go to `ids`. Its last figure, "filled", says the state is down: a script
// that kills the driver -9 waits for that line.
inline void recovery_fill(Store& store, size_t large, size_t small, const std::filesystem::path& ids, const Report& report) {
  constexpr uint64_t mib = 1024 * 1024;
  const auto started = Clock::now();
  const auto before = process_figures();
  std::vector<Id> extents;
  for (size_t f = 0; f < large; ++f) {
    const auto file = publish_file(store, 512 * mib, 40000000 + f * 1000);
    extents.insert(extents.end(), file.begin(), file.end());
  }
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> small_bytes{0};
  constexpr size_t batch = 100;
  std::vector<std::thread> publishers;
  for (size_t p = 0; p < 8; ++p)
    publishers.emplace_back([&] {
      for (size_t first = next.fetch_add(batch); first < small; first = next.fetch_add(batch)) {
        auto publishing = store.publication(0, Frame::loader);
        for (size_t i = first; i < std::min(small, first + batch); ++i) {
          // 4 KiB to 64 KiB, the same sizes on both stores.
          const auto data = pattern(4096 + (i * 7919) % (60 * 1024), 50000000 + i);
          small_bytes += data.size();
          if (!publishing->extent(store.id_of(data), data)) failed = true;
        }
        if (!publishing->finish()) failed = true;
      }
    });
  for (auto& publisher : publishers) publisher.join();
  write_ids(ids, extents);
  const auto after = process_figures();
  report.figure("fill_seconds", std::chrono::duration<double>(Clock::now() - started).count(), "s");
  report.figure("fill_objects", static_cast<double>(extents.size() + small), "objects");
  report.figure("fill_bytes", static_cast<double>(large * 512 * mib + small_bytes.load()), "B");
  report.figure("fill_disk_write", static_cast<double>(after.write_bytes - before.write_bytes), "B");
  report.figure("failed", failed ? 1 : 0, "bool");
  report.figure("filled", 1, "bool");
  // For the kill -9 case: the process waits here, its nodes running, until the script
  // kills it, so no clean stop can come first.
  if (const char* hold = std::getenv("HARNESS_HOLD_AFTER_FILL"); hold && *hold)
    while (true) std::this_thread::sleep_for(std::chrono::hours(1));
}

// What a driver timed in its reopen, from node 0's construction; nothing for a point
// it never reached.
struct Reopened {
  std::optional<Clock::duration> data_open;
  std::optional<Clock::duration> ready;
  // When the store a read goes through exists: built lazily after ready on Macha's
  // side, as its NodeServices builds it; inside the node, before ready, on Dagda's.
  std::optional<Clock::duration> store_built;
  std::optional<Clock::duration> first_read;
  bool read_ok = false;
  double cpu_to_ready = 0;
  uint64_t disk_read_to_ready = 0;
  // Why the reopen failed, when it did: a store that never comes ready is a failure
  // with its reason, never a missing figure (the Macha session's review).
  std::string failure;
};

inline void report_reopen(const Reopened& reopened, const Report& report) {
  const auto ms = [](const std::optional<Clock::duration>& at) { return at ? std::chrono::duration<double, std::milli>(*at).count() : -1.0; };
  report.figure("data_open_ms", ms(reopened.data_open), "ms");
  report.figure("ready_ms", ms(reopened.ready), "ms");
  report.figure("store_built_ms", ms(reopened.store_built), "ms");
  report.figure("first_read_ms", ms(reopened.first_read), "ms");
  report.figure("cpu_to_ready", reopened.cpu_to_ready, "s");
  report.figure("disk_read_to_ready", static_cast<double>(reopened.disk_read_to_ready), "B");
  auto failure = reopened.failure;
  if (failure.empty() && !reopened.ready) failure = "never ready";
  if (failure.empty() && !reopened.read_ok) failure = "the first read brought nothing";
  report.figure("failed", failure.empty() ? 0 : 1, "bool");
  if (!failure.empty()) std::fprintf(stderr, "reopen failed: %s\n", failure.c_str());
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
  } else if (workload == "reads-local") {
    reads_local(store, report);
  } else {
    std::fprintf(stderr, "unknown workload %s: idle, publication-one, publication-ten, small-files, viewer-under-load, reads-local\n", workload.c_str());
    return 2;
  }
  return 0;
}

// ---- 7. Repair, across two machines (the harness plan, "Workload 7") ------------

// One object of a repair run: its seed (its bytes are pattern(size, seed) on both
// stores), its size and the store's own id for it.
struct Listed {
  uint64_t seed = 0;
  uint64_t size = 0;
  Id id;
};

// One node of a cluster whose other node runs in another process on another machine,
// running as a deployed node does: its own maintenance, the survey and repair working
// towards what its state refers to. On Macha's side that is a whole Service, each object
// a file of one extent written through its filesystem, since Macha's repair lives in its
// maintenance pass and takes what is referenced from the node's own metadata (the Macha
// session's review, 2026-11-16); on Dagda's it is a node told what is referenced.
class RepairNode {
 public:
  virtual ~RepairNode() = default;
  virtual std::string describe() const = 0;
  // Writes the object as a loader would and says the store's id for it; nothing when it
  // landed nowhere.
  virtual std::optional<Id> put(uint64_t seed, std::span<const uint8_t> data) = 0;
  // Whether this node's own store holds it.
  virtual bool holds(const Id& id) = 0;
  // A foreground read, as a viewer's: noted as foreground activity by the store's pacing.
  virtual std::optional<Bytes> read(const Listed& object, std::chrono::milliseconds deadline) = 0;
  // Returns once this node's state refers to every one of `objects`: at once where the
  // node is told (Dagda), when its own metadata names them all where it learns (Macha).
  virtual void reference(const std::vector<Listed>& objects) = 0;
  // The other nodes this one sees active.
  virtual size_t peers() = 0;
  // The store's own repair counters so far, by name; every one only grows.
  virtual std::map<std::string, double> counters() = 0;
};

// The objects a repair run is about, one a line as id in hex, size and seed: written by
// the node that fills, copied by the script to the other.
inline std::vector<Listed> read_listed(const std::filesystem::path& path) {
  std::vector<Listed> out;
  std::ifstream in(path);
  std::string hex;
  uint64_t size = 0;
  uint64_t seed = 0;
  while (in >> hex >> size >> seed) {
    if (hex.size() != 64) throw std::runtime_error("a line of " + path.string() + " is not an id");
    Listed one;
    for (size_t i = 0; i < 32; ++i) one.id.bytes[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    one.size = size;
    one.seed = seed;
    out.push_back(one);
  }
  return out;
}

inline void append_listed(const std::filesystem::path& path, const std::vector<Listed>& listed) {
  std::ofstream out(path, std::ios::app);
  char hex[3];
  for (const auto& one : listed) {
    for (const auto byte : one.id.bytes) {
      std::snprintf(hex, sizeof(hex), "%02x", byte);
      out << hex;
    }
    out << ' ' << one.size << ' ' << one.seed << '\n';
  }
}

// The node does what the script tells it, a line at a time from `<dir>/cmd` (written
// whole and renamed into place; several lines run in order), and answers each with a
// line in `<dir>/log`: "done <line>" or "failed <line>: <why>". The commands:
//   fill <first> <count> <size>   puts `count` objects of `size` bytes, seeds from `first`,
//                                 and appends them to <dir>/ids
//   reference                     returns once the node refers to everything in <dir>/ids,
//                                 and reports how long that took
//   peer                          waits until another node is active
//   measure <label>               waits until this node holds everything in <dir>/ids,
//                                 and reports the span's figures as <label>.*
//   figures <label>               reports the counters and process figures now
//   viewer-start                  a foreground read of a random listed object each second
//   viewer-stop <label>           ends it and reports its latencies as <label>.*
//   stop                          ends the process
inline int repair_node(RepairNode& node, const std::filesystem::path& dir, const Report& report) {
  const auto ids_path = dir / "ids";
  const auto answer = [&](const std::string& line) { std::ofstream(dir / "log", std::ios::app) << line << '\n'; };
  const auto now_figures = [&](const std::string& label, const std::map<std::string, double>& since, const ProcessFigures& before) {
    for (const auto& [name, value] : node.counters()) {
      const auto found = since.find(name);
      report.figure(label + "." + name, value - (found == since.end() ? 0.0 : found->second), "count");
    }
    const auto after = process_figures();
    report.figure(label + ".cpu", after.cpu_seconds - before.cpu_seconds, "s");
    report.figure(label + ".rss_hwm", static_cast<double>(after.hwm_bytes), "B");
    report.figure(label + ".disk_write", static_cast<double>(after.write_bytes - before.write_bytes), "B");
  };
  std::atomic<bool> viewing{false};
  std::thread viewer;
  Latencies views;
  std::atomic<size_t> view_misses{0};
  const auto stop_viewer = [&] {
    viewing = false;
    if (viewer.joinable()) viewer.join();
  };
  const ProcessFigures at_start = process_figures();
  while (true) {
    std::vector<std::string> lines;
    {
      std::ifstream in(dir / "cmd");
      std::string line;
      while (std::getline(in, line))
        if (!line.empty()) lines.push_back(line);
    }
    if (lines.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    std::filesystem::remove(dir / "cmd");
    for (const auto& line : lines) {
      std::istringstream words(line);
      std::string command;
      words >> command;
      try {
        if (command == "fill") {
          uint64_t first = 0;
          size_t count = 0;
          size_t size = 0;
          words >> first >> count >> size;
          std::vector<Listed> listed;
          for (size_t i = 0; i < count; ++i) {
            const auto data = pattern(size, first + i);
            const auto id = node.put(first + i, data);
            if (!id) throw std::runtime_error("a put landed nowhere");
            listed.push_back({first + i, size, *id});
          }
          append_listed(ids_path, listed);
        } else if (command == "reference") {
          const auto started = Clock::now();
          node.reference(read_listed(ids_path));
          report.figure("reference.seconds", std::chrono::duration<double>(Clock::now() - started).count(), "s");
        } else if (command == "peer") {
          const auto until = Clock::now() + std::chrono::minutes(10);
          while (node.peers() == 0) {
            if (Clock::now() > until) throw std::runtime_error("no other node became active");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
        } else if (command == "measure") {
          std::string label;
          words >> label;
          const auto listed = read_listed(ids_path);
          const auto since = node.counters();
          const auto before = process_figures();
          const auto started = Clock::now();
          uint64_t missing_bytes = 0;
          std::vector<Listed> missing;
          for (const auto& one : listed)
            if (!node.holds(one.id)) {
              missing.push_back(one);
              missing_bytes += one.size;
            }
          const size_t needed = missing.size();
          const auto until = started + std::chrono::hours(2);
          while (!missing.empty()) {
            if (Clock::now() > until) throw std::runtime_error(std::to_string(missing.size()) + " objects never arrived");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::erase_if(missing, [&](const Listed& one) { return node.holds(one.id); });
          }
          const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
          report.figure(label + ".seconds", seconds, "s");
          report.figure(label + ".objects_needed", static_cast<double>(needed), "objects");
          report.figure(label + ".bytes_needed", static_cast<double>(missing_bytes), "B");
          report.figure(label + ".throughput", seconds > 0 ? static_cast<double>(missing_bytes) / seconds / 1e6 : 0.0, "MB/s");
          now_figures(label, since, before);
        } else if (command == "figures") {
          std::string label;
          words >> label;
          now_figures(label, {}, at_start);
        } else if (command == "viewer-start") {
          stop_viewer();
          const auto listed = read_listed(ids_path);
          if (listed.empty()) throw std::runtime_error("nothing is listed to view");
          views.clear();
          view_misses = 0;
          viewing = true;
          viewer = std::thread([&, listed] {
            uint64_t x = 88172645463325252ULL;
            while (viewing.load()) {
              x ^= x << 13;
              x ^= x >> 7;
              x ^= x << 17;
              const auto started = Clock::now();
              if (!node.read(listed[x % listed.size()], std::chrono::seconds(30))) ++view_misses;
              views.add(Clock::now() - started);
              for (int tenth = 0; tenth < 10 && viewing.load(); ++tenth) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
          });
        } else if (command == "viewer-stop") {
          std::string label;
          words >> label;
          stop_viewer();
          report.latencies(label, views);
          report.figure(label + ".misses", static_cast<double>(view_misses.load()), "reads");
        } else if (command == "stop") {
          stop_viewer();
          answer("done " + line);
          return 0;
        } else {
          throw std::runtime_error("unknown command");
        }
        answer("done " + line);
      } catch (const std::exception& error) {
        answer("failed " + line + ": " + error.what());
      }
    }
  }
}

}  // namespace harness
