//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// What the local store's tests share: what a store runs on, Macha's test
// patterns (tests/test_support.hpp, develop 5a06d3c), and its interposed
// device (tests/interposed_local_store_files.hpp).

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dagda/store/durability_domain.h"
#include "dagda/store/local_format.h"
#include "dagda/store/local_store.h"
#include "dagda/store/local_store_files.h"
#include "dagda/store/storage_pool.h"
#include "dagda/test/fixtures/ledger.h"
#include "dannan/identity/node_id.h"
#include "dannan/status/observation.h"
#include "dannan/supervise/startup.h"
#include "dannan/supervise/supervised.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::test {

using store::Bytes;

// Macha's test pattern: an xorshift stream.
inline Bytes pattern(size_t n, uint8_t salt = 0) {
  Bytes out(n);
  uint64_t x = 0x123456789abcdef0ULL ^ salt;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    out[i] = static_cast<uint8_t>(x);
  }
  return out;
}

// 4 KiB of pattern with its first eight bytes replaced by n, so every n
// names a distinct object.
inline Bytes numbered(uint64_t n) {
  auto bytes = pattern(4096);
  std::memcpy(bytes.data(), &n, sizeof(n));
  return bytes;
}

// Polls until `done` or the bound: for a store's own background work (its
// scan, its presence walk), which no test can step. A safety net against a
// hang, not a timing.
inline bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds bound = std::chrono::seconds(10)) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

template <class T>
bool ready(std::future<T>& future, std::chrono::milliseconds within = std::chrono::seconds(10)) {
  return future.wait_for(within) == std::future_status::ready;
}

// What a store runs on in a test: a directory, the node's services, a
// logger that keeps what it is told, stepped time and a stepped wall clock.
// The wall starts at the real date unless told otherwise, so it agrees with
// the modification time a new file gets. The key is domain v1's from 1a's
// known-answer cluster key.
struct StoreBed {
  explicit StoreBed(uint64_t wall_start_ms = dannan::contract::system_wall_clock().unix_ms()) : wall(wall_start_ms) {}

  dannan::test::TempDir dir;
  dannan::supervise::SupervisedThreads threads;
  dannan::status::Observations observations;
  dannan::supervise::StartupProgress progress;
  std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  std::shared_ptr<const domain::StorageKey> key = known_answer_key();

  // A store that makes its own domain gets the steady clock for its window:
  // a test that awaits a batch hands in a domain on `time` instead.
  store::LocalStore::Services services() {
    return {threads, observations, progress, dannan::status::ScopedLogger(sink, "store"), dannan::contract::steady_time_source(), wall};
  }
  std::shared_ptr<store::DurabilityDomain> domain(const std::filesystem::path& root, std::chrono::milliseconds window) {
    std::filesystem::create_directories(root);
    return std::make_shared<store::DurabilityDomain>(1, root, store::DurabilityDomain::Services{threads, observations, time}, window);
  }
  std::unique_ptr<store::LocalStore> open(const std::filesystem::path& root, store::LocalStoreOptions options,
                                          store::LocalStoreMode mode = store::LocalStoreMode::authoritative,
                                          std::shared_ptr<store::DurabilityDomain> domain = {},
                                          store::LocalStoreFiles& files = store::posix_local_store_files()) {
    return std::make_unique<store::LocalStore>(root, options, key, services(), mode, std::move(domain), files);
  }

  // A pool's services: its domains' windows on `time` (the steady clock
  // unless a test steps them).
  store::LocalStore::Services pool_services(const dannan::contract::TimeSource& on = dannan::contract::steady_time_source()) {
    return {threads, observations, progress, dannan::status::ScopedLogger(sink, "pool"), on, wall};
  }
  // A pool of loose objects only, as Macha's StoragePool constructor makes by
  // default; its own node and process epoch unless given.
  std::unique_ptr<store::StoragePool> pool(const std::filesystem::path& state, std::vector<store::StorageBackendOptions> backends,
                                           std::chrono::milliseconds window = std::chrono::milliseconds(500),
                                           dannan::identity::NodeId node = dannan::identity::NodeId::random(),
                                           const dannan::contract::TimeSource& on = dannan::contract::steady_time_source()) {
    store::StoragePoolOptions options;
    options.backends = std::move(backends);
    options.durability_batch_window = window;
    options.pack_threshold = 0;
    options.pack_target_size = 0;
    return std::make_unique<store::StoragePool>(state, node, dannan::identity::NodeId::random(), options, key, pool_services(on));
  }
};

// Where Macha's loose layout keeps an object under a store's root.
inline std::filesystem::path object_path(const std::filesystem::path& root, const store::ObjectId& id) {
  return store::local_format::loose_path(root / "objects", id);
}

// Flips the last byte of a loose object, as Macha's tests corrupt one.
inline void corrupt_object(const std::filesystem::path& root, const store::ObjectId& id) {
  std::fstream file(object_path(root, id), std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good());
  file.seekg(-1, std::ios::end);
  char byte{};
  file.read(&byte, 1);
  ASSERT_TRUE(file.good());
  byte = static_cast<char>(byte ^ 0x5a);
  file.seekp(-1, std::ios::end);
  file.write(&byte, 1);
  file.flush();
  ASSERT_TRUE(file.good());
}

// A batchable barrier on stepped time: the window ends because the test
// moves time on, never by waiting. Moving it on more than once is harmless.
inline void barrier_through_window(store::LocalStore& local, uint64_t generation, store::DurabilityDomain& domain, dannan::test::SteppedTime& time,
                                   std::chrono::milliseconds window) {
  auto waiter = std::async(std::launch::async, [&] { local.durability_barrier(generation); });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (waiter.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready && std::chrono::steady_clock::now() < until) {
    time.advance(window);
    domain.time_advanced();
  }
  ASSERT_EQ(waiter.wait_for(std::chrono::seconds(0)), std::future_status::ready);
  waiter.get();
}

// The device with a step the test sets, run before each operation: it may
// block the caller, throw as a failing device would, or record what was
// asked. Outlives the store it is handed to.
class InterposedLocalStoreFiles final : public store::LocalStoreFiles {
 public:
  enum class Op { install, read, read_at, append, truncate, list };
  using Step = std::function<void(Op, const std::filesystem::path&)>;

  void before(Step step) {
    std::lock_guard lock(_mutex);
    _step = std::move(step);
  }

  void install(const std::filesystem::path& path, std::span<const uint8_t> head, std::span<const uint8_t> body) override {
    run(Op::install, path);
    _device.install(path, head, body);
  }
  Bytes read(const std::filesystem::path& path) override {
    run(Op::read, path);
    return _device.read(path);
  }
  std::optional<Bytes> read_at(const std::filesystem::path& path, uint64_t offset, size_t size) override {
    run(Op::read_at, path);
    return _device.read_at(path, offset, size);
  }
  void append(const std::filesystem::path& path, std::span<const uint8_t> head, std::span<const uint8_t> body) override {
    run(Op::append, path);
    _device.append(path, head, body);
  }
  bool truncate(const std::filesystem::path& path, uint64_t size) override {
    run(Op::truncate, path);
    return _device.truncate(path, size);
  }
  std::vector<std::pair<std::filesystem::path, uint64_t>> list(const std::filesystem::path& dir) override {
    run(Op::list, dir);
    return _device.list(dir);
  }

 private:
  void run(Op op, const std::filesystem::path& path) {
    Step step;
    {
      std::lock_guard lock(_mutex);
      step = _step;
    }
    if (step) step(op, path);
  }

  std::mutex _mutex;
  Step _step;
  store::PosixLocalStoreFiles _device;
};

}  // namespace dagda::test
