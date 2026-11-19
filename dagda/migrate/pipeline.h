//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

// A pass over a disk at the disk's rate (TODO/2026-11-19-migrate-speed.md): one thread
// produces the items in order (reading what must be read in order), workers transform
// them on their own threads (authenticating, reading what can be read apart), and the
// caller consumes the results in item order, never more than a window ahead of it. So
// the disk is never idle while the CPU checks, and the caller's output, its checkpoints
// and its resume points are exactly a serial pass's.
namespace dagda::migrate {

struct PipelineLimits {
  // Worker threads; at least one.
  size_t workers = 1;
  // Items produced and not yet consumed: at most this many, and while more than one
  // is held, at most `bytes` of them by their weight.
  size_t items = 16;
  uint64_t bytes = 64ULL << 20;
};

// The cores less one for the caller, at least one.
inline size_t pipeline_workers() {
  const auto cores = std::thread::hardware_concurrency();
  return cores > 1 ? cores - 1 : 1;
}

// Runs the pipeline to the end, or to the first exception: a producer's, a worker's
// (at its item's turn, so everything before it is consumed first) or the consumer's.
// That exception is rethrown here once every thread has stopped. `produce` returns the
// next item and its weight in bytes, or nothing at the end; it runs on one thread of
// its own. `work` runs on the workers, any number at once. `consume` runs on the
// calling thread. Waits on the threads it starts; single owner.
template <typename Input, typename Result>
void run_ordered(const PipelineLimits& limits, const std::function<std::optional<std::pair<Input, uint64_t>>()>& produce,
                 const std::function<Result(Input&)>& work, const std::function<void(Result&)>& consume) {
  struct Slot {
    std::optional<Input> input;
    std::optional<Result> result;
    std::exception_ptr error;
    uint64_t weight = 0;
    bool taken = false;
  };
  std::mutex mutex;
  std::condition_variable changed;
  std::map<uint64_t, Slot> slots;  // guarded by mutex: produced and not yet consumed
  uint64_t produced = 0;           // guarded by mutex
  uint64_t held_bytes = 0;         // guarded by mutex
  bool finished = false;           // guarded by mutex: the producer has no more
  std::exception_ptr produce_error;
  bool stopping = false;  // guarded by mutex
  const size_t window = std::max<size_t>(1, limits.items);

  const auto producer = [&] {
    try {
      while (true) {
        {
          std::unique_lock lock(mutex);
          changed.wait(lock, [&] { return stopping || (slots.size() < window && (slots.empty() || held_bytes < limits.bytes)); });
          if (stopping) return;
        }
        auto next = produce();
        std::lock_guard lock(mutex);
        if (!next) {
          finished = true;
          changed.notify_all();
          return;
        }
        auto& slot = slots[produced++];
        slot.input.emplace(std::move(next->first));
        slot.weight = next->second;
        held_bytes += slot.weight;
        changed.notify_all();
      }
    } catch (...) {
      std::lock_guard lock(mutex);
      produce_error = std::current_exception();
      finished = true;
      changed.notify_all();
    }
  };
  const auto worker = [&] {
    while (true) {
      Slot* slot = nullptr;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
          if (stopping) return true;
          for (auto& [_, candidate] : slots)
            if (!candidate.taken) return true;
          return finished;
        });
        if (stopping) return;
        for (auto& [_, candidate] : slots)
          if (!candidate.taken) {
            slot = &candidate;
            break;
          }
        if (!slot) return;  // the producer is done and nothing is left to take
        slot->taken = true;
      }
      // The slot stays where it is until the consumer erases it, after this result.
      std::optional<Result> result;
      std::exception_ptr error;
      try {
        result.emplace(work(*slot->input));
      } catch (...) {
        error = std::current_exception();
      }
      std::lock_guard lock(mutex);
      slot->input.reset();
      slot->result = std::move(result);
      slot->error = error;
      changed.notify_all();
    }
  };

  std::vector<std::thread> threads;
  const auto stop_all = [&] {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    changed.notify_all();
    for (auto& thread : threads)
      if (thread.joinable()) thread.join();
  };
  try {
    threads.emplace_back(producer);
    for (size_t i = 0; i < std::max<size_t>(1, limits.workers); ++i) threads.emplace_back(worker);
    for (uint64_t next = 0;; ++next) {
      std::optional<Result> result;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
          const auto found = slots.find(next);
          if (found != slots.end()) return found->second.result.has_value() || found->second.error != nullptr;
          return finished && next >= produced;
        });
        const auto found = slots.find(next);
        if (found == slots.end()) {
          if (produce_error) std::rethrow_exception(produce_error);
          break;
        }
        if (found->second.error) std::rethrow_exception(found->second.error);
        result = std::move(found->second.result);
        held_bytes -= std::min(held_bytes, found->second.weight);
        slots.erase(found);
      }
      changed.notify_all();
      consume(*result);
    }
  } catch (...) {
    stop_all();
    throw;
  }
  stop_all();
}

}  // namespace dagda::migrate
