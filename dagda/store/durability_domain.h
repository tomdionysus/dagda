//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/observation.h"
#include "dannan/supervise/supervised.h"

namespace dagda::store {

enum class DurabilityUrgency : uint8_t {
  // Waits for the batch window, so concurrent writers share one barrier.
  batchable,
  // Wakes the worker at once.
  immediate,
};

// The stable-storage barriers of one mounted filesystem (requirements S13,
// S14). Stores never fsync or syncfs their data themselves: they report a
// completed filesystem mutation and receive a generation, numbered from 1,
// and whoever needs it durable waits for that generation here. One worker
// makes every generation up to a cut durable with one syncfs where the
// platform has it, or an fsync of each file and directory the cut's
// mutations named where it does not.
//
// Generations are process-local: a caller binds one to the process's
// durability epoch, the domain and the backend incarnation (a
// DurabilityTicket) before it is evidence to anyone else.
//
// A failed barrier is sticky: every later mutation and wait rethrows it, and
// the domain is never reused. Thread-safe.
class DurabilityDomain {
 public:
  using Generation = uint64_t;

  // What the domain runs on, each one per node and injected.
  struct Services {
    dannan::supervise::SupervisedThreads& threads;
    // Each barrier's time, in microseconds, under
    // "durability.barrier_us.<representative's directory name>".
    dannan::status::Observations& observations;
    // When a batch window ends.
    const dannan::contract::TimeSource& time = dannan::contract::steady_time_source();
  };

  // `id` is non-zero; `batch_window` is not negative. Throws
  // std::invalid_argument otherwise. Starts the worker.
  DurabilityDomain(uint64_t id, std::filesystem::path representative, Services services,
                   std::chrono::milliseconds batch_window = std::chrono::milliseconds(500));
  // Makes what was asked for durable, then stops the worker.
  ~DurabilityDomain();
  DurabilityDomain(const DurabilityDomain&) = delete;
  DurabilityDomain& operator=(const DurabilityDomain&) = delete;

  uint64_t id() const noexcept { return _id; }

  // Another directory on the same filesystem that a syncfs may be issued
  // through. Waits on nothing.
  void add_representative(std::filesystem::path path);

  // Called only after the mutation's write, rename or unlink has returned. A
  // cut taken before this call cannot claim the mutation; a cut taken after
  // it can: the generation invariant. `file` and `directory` are what the
  // per-file fallback syncs; either may be empty. Rethrows a sticky failure.
  // Waits on nothing.
  Generation complete_mutation(std::filesystem::path file = {}, std::filesystem::path directory = {});

  // Returns once `generation` is durable; 0 returns at once. Throws
  // std::runtime_error for a generation never issued (it may come from a
  // peer's ticket), and rethrows a sticky failure. Waits on the device, up to a batch window first when
  // batchable.
  void await_durable(Generation generation, DurabilityUrgency urgency = DurabilityUrgency::batchable);
  // Everything issued so far, immediately.
  void flush();

  // The worker re-reads the time source: for a source that moves other than
  // with the steady clock (a test's stepped time). Waits on nothing.
  void time_advanced();

  Generation current_generation() const;
  Generation durable_generation() const;
  bool failed() const;
  // Barriers made since construction.
  uint64_t physical_barriers() const noexcept { return _physical_barriers.load(std::memory_order_relaxed); }

 private:
  struct PortableMutation {
    Generation generation = 0;
    std::filesystem::path file;
    std::filesystem::path directory;
  };

  void loop(std::stop_token stop);
  void perform_barrier(Generation cut, const std::vector<PortableMutation>& portable);

  const uint64_t _id;
  const std::string _barrier_series;
  const std::chrono::milliseconds _batch_window;
  dannan::supervise::SupervisedThreads& _threads;
  dannan::status::Observations& _observations;
  const dannan::contract::TimeSource& _time;

  mutable dannan::contract::Mutex _mutex;
  std::condition_variable_any _wake;
  std::vector<std::filesystem::path> _representatives DANNAN_GUARDED_BY(_mutex);
  Generation _mutation_generation DANNAN_GUARDED_BY(_mutex) = 0;
  Generation _durable_generation DANNAN_GUARDED_BY(_mutex) = 0;
  Generation _requested_generation DANNAN_GUARDED_BY(_mutex) = 0;
  bool _immediate_requested DANNAN_GUARDED_BY(_mutex) = false;
  std::optional<dannan::contract::TimePoint> _batch_deadline DANNAN_GUARDED_BY(_mutex);
  std::exception_ptr _failure DANNAN_GUARDED_BY(_mutex);
  // Used only where there is no syncfs.
  std::vector<PortableMutation> _portable_mutations DANNAN_GUARDED_BY(_mutex);
  std::atomic<uint64_t> _physical_barriers{0};
  // Started last in the constructor, stopped first in the destructor.
  std::jthread _worker;
};

}  // namespace dagda::store
