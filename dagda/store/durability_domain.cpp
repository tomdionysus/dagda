//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/durability_domain.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace dagda::store {
namespace {

using dannan::contract::Lock;

#if !defined(__linux__)
// The per-file fallback. A file or directory gone since its mutation (a
// later remove, a compaction) has nothing left to make durable.
void sync_path(const std::filesystem::path& path, int flags, const char* what) {
  if (path.empty()) return;
  const int fd = ::open(path.c_str(), flags | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return;
    throw std::runtime_error(std::string("cannot open durability ") + what + ": " + std::strerror(errno));
  }
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  const int saved = errno;
  ::close(fd);
  if (rc != 0) throw std::runtime_error(std::string("cannot sync durability ") + what + ": " + std::strerror(saved));
}
#endif

}  // namespace

DurabilityDomain::DurabilityDomain(uint64_t id, std::filesystem::path representative, Services services, std::chrono::milliseconds batch_window)
    : _id(id),
      // Named after the directory it was made for (the control store's or a
      // DATA backend's), so their barriers are told apart.
      _barrier_series("durability.barrier_us." + representative.filename().string()),
      _batch_window(batch_window),
      _threads(services.threads),
      _observations(services.observations),
      _time(services.time),
      _on_failure(std::move(services.on_failure)),
      _before_sync(std::move(services.before_sync)) {
  if (!_id) throw std::invalid_argument("durability domain id must be non-zero");
  if (_batch_window < std::chrono::milliseconds::zero()) throw std::invalid_argument("durability batch window cannot be negative");
  add_representative(std::move(representative));
  _worker = std::jthread([this](std::stop_token stop) { _threads.loop("durability-domain", stop, [this, stop] { loop(stop); }); });
}

DurabilityDomain::~DurabilityDomain() {
  _worker.request_stop();
  if (_worker.joinable()) _worker.join();
}

void DurabilityDomain::add_representative(std::filesystem::path path) {
  if (path.empty()) return;
  path = path.lexically_normal();
  Lock lock(_mutex);
  if (std::find(_representatives.begin(), _representatives.end(), path) == _representatives.end()) _representatives.push_back(std::move(path));
}

DurabilityDomain::Generation DurabilityDomain::complete_mutation(std::filesystem::path file, std::filesystem::path directory) {
  Lock lock(_mutex);
  if (_failure) std::rethrow_exception(_failure);
  const auto generation = ++_mutation_generation;
#if !defined(__linux__)
  _portable_mutations.push_back({generation, std::move(file), std::move(directory)});
#else
  (void)file;
  (void)directory;
#endif
  return generation;
}

void DurabilityDomain::await_durable(Generation generation, DurabilityUrgency urgency) {
  if (!generation) return;
  Lock lock(_mutex);
  if (generation > _mutation_generation) throw std::runtime_error("durability generation was never admitted");
  if (_failure) std::rethrow_exception(_failure);
  if (generation <= _durable_generation) return;

  const bool had_request = _requested_generation > _durable_generation;
  _requested_generation = std::max(_requested_generation, generation);
  if (urgency == DurabilityUrgency::immediate) {
    _immediate_requested = true;
  } else if (!had_request || !_batch_deadline) {
    _batch_deadline = _time.now() + _batch_window;
  }
  _wake.notify_all();

  _wake.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return _durable_generation >= generation || _failure != nullptr; });
  if (_failure) std::rethrow_exception(_failure);
}

void DurabilityDomain::flush() {
  Generation generation;
  {
    Lock lock(_mutex);
    generation = _mutation_generation;
  }
  await_durable(generation, DurabilityUrgency::immediate);
}

void DurabilityDomain::time_advanced() {
  {
    // Taken so the worker, between its reading and its wait, cannot miss it.
    Lock lock(_mutex);
  }
  _wake.notify_all();
}

DurabilityDomain::Generation DurabilityDomain::current_generation() const {
  Lock lock(_mutex);
  return _mutation_generation;
}

DurabilityDomain::Generation DurabilityDomain::durable_generation() const {
  Lock lock(_mutex);
  return _durable_generation;
}

bool DurabilityDomain::failed() const {
  Lock lock(_mutex);
  return _failure != nullptr;
}

void DurabilityDomain::perform_barrier(Generation cut, const std::vector<PortableMutation>& portable) {
  const auto started = dannan::contract::Clock::now();
  if (_before_sync) _before_sync();
#if defined(__linux__)
  (void)cut;
  (void)portable;
  std::vector<std::filesystem::path> representatives;
  {
    Lock lock(_mutex);
    representatives = _representatives;
  }
  int last_error = ENOENT;
  bool synced = false;
  for (const auto& representative : representatives) {
    const int fd = ::open(representative.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
      last_error = errno;
      continue;
    }
    int rc;
    do {
      rc = ::syncfs(fd);
    } while (rc != 0 && errno == EINTR);
    last_error = errno;
    ::close(fd);
    if (rc == 0) {
      synced = true;
      break;
    }
  }
  if (!synced) throw std::runtime_error("cannot sync durability domain: " + std::string(std::strerror(last_error)));
#else
  std::vector<std::filesystem::path> files;
  std::vector<std::filesystem::path> directories;
  for (const auto& mutation : portable) {
    if (mutation.generation > cut) continue;
    if (!mutation.file.empty()) files.push_back(mutation.file);
    if (!mutation.directory.empty()) directories.push_back(mutation.directory);
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  for (const auto& file : files) sync_path(file, O_RDONLY, "file");
  std::sort(directories.begin(), directories.end());
  directories.erase(std::unique(directories.begin(), directories.end()), directories.end());
  for (const auto& directory : directories) sync_path(directory, O_RDONLY | O_DIRECTORY, "directory");
#endif
  _physical_barriers.fetch_add(1, std::memory_order_relaxed);
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(dannan::contract::Clock::now() - started).count();
  _observations.record(_barrier_series, static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)));
}

void DurabilityDomain::loop(std::stop_token stop) {
  while (true) {
    Generation cut = 0;
    std::vector<PortableMutation> portable;
    {
      Lock lock(_mutex);
      _wake.wait(lock.native(), stop, [&]() DANNAN_REQUIRES(_mutex) { return _failure || _requested_generation > _durable_generation; });
      if (_failure) return;
      if (stop.stop_requested() && _requested_generation <= _durable_generation) return;

      // A batch waits out its window on the injected time source; an
      // immediate request, a failure or a stop ends the wait early.
      while (!_immediate_requested && !stop.stop_requested()) {
        if (!_batch_deadline) _batch_deadline = _time.now() + _batch_window;
        const auto now = _time.now();
        if (now >= *_batch_deadline) break;
        const auto deadline = *_batch_deadline;
        _wake.wait_for(lock.native(), stop, deadline - now,
                       [&]() DANNAN_REQUIRES(_mutex) { return _failure || _immediate_requested || _time.now() >= deadline; });
        if (_failure) return;
      }

      // The current generation holds only mutations that completed their
      // syscalls and then registered. Writers go on while the barrier runs;
      // whatever registers afterwards gets a later generation and is left
      // out of this cut.
      cut = _mutation_generation;
      if (cut <= _durable_generation) {
        _requested_generation = std::max(_requested_generation, _durable_generation);
        _immediate_requested = false;
        _batch_deadline.reset();
        continue;
      }
#if !defined(__linux__)
      for (const auto& mutation : _portable_mutations)
        if (mutation.generation <= cut) portable.push_back(mutation);
#endif
      _immediate_requested = false;
      _batch_deadline.reset();
    }

    try {
      perform_barrier(cut, portable);
    } catch (...) {
      const auto failure = std::current_exception();
      // Recorded before any waiter hears of it, so none can re-derive first.
      if (_on_failure) {
        try {
          _on_failure(durable_generation());
        } catch (...) {
        }
      }
      Lock lock(_mutex);
      _failure = failure;
      _wake.notify_all();
      return;
    }

    {
      Lock lock(_mutex);
      _durable_generation = std::max(_durable_generation, cut);
#if !defined(__linux__)
      const auto durable = _durable_generation;
      _portable_mutations.erase(_portable_mutations.begin(), std::find_if(_portable_mutations.begin(), _portable_mutations.end(),
                                                                          [&](const PortableMutation& mutation) { return mutation.generation > durable; }));
#endif
      if (_requested_generation <= _durable_generation) _requested_generation = _durable_generation;
      _wake.notify_all();
    }
  }
}

}  // namespace dagda::store
