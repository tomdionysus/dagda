//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/spaces.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "dagda/heads/control_node_store.h"

namespace dagda::heads {
namespace {

using dannan::contract::Lock;

contract::RetentionDot retention_dot(const Dot& dot) { return {dot.author, dot.sequence}; }

}  // namespace

std::vector<ObjectId> StoreClaimBarrier::retain_data_here(const std::vector<ObjectId>& ids, const Dot& dot) {
  return _store.retain_data_here(ids, retention_dot(dot));
}

std::vector<ObjectId> StoreClaimBarrier::retain_data(const std::vector<ObjectId>& ids, const Dot& dot) { return _store.retain_data(ids, retention_dot(dot)); }

bool StoreClaimBarrier::retain_control_here(const std::vector<ObjectId>& ids, const Dot& dot) {
  return _store.retain_control(ids, retention_dot(dot), store::DistributedStore::ClaimScope::here);
}

Spaces::Spaces(Services services)
    : _services(std::move(services)),
      _barrier(_services.store),
      _guard(_services.context.logger("dagda.heads")),
      _logger(_services.context.logger("dagda.heads")),
      _thread([this](std::stop_token stop) { _services.context.threads().loop("dagda.heads", stop, [this, stop] { worker(stop); }); }) {}

Spaces::~Spaces() { stop(); }

void Spaces::stop() {
  {
    Lock lock(_queue_mutex);
    _stopping.store(true, std::memory_order_release);
  }
  if (_thread.joinable()) {
    _thread.request_stop();
    _queue_changed.notify_all();
    _thread.join();
  }
}

bool Spaces::submit(std::function<void()> job) {
  {
    Lock lock(_queue_mutex);
    if (_stopping.load(std::memory_order_acquire)) return false;
    _queue.push_back(std::move(job));
  }
  _queue_changed.notify_one();
  return true;
}

void Spaces::worker(std::stop_token stop) {
  for (;;) {
    std::function<void()> job;
    {
      Lock lock(_queue_mutex);
      _queue_changed.wait(lock.native(), stop, [&]() DANNAN_REQUIRES(_queue_mutex) { return !_queue.empty(); });
      if (_queue.empty()) return;
      job = std::move(_queue.front());
      _queue.pop_front();
    }
    job();
  }
}

Space& Spaces::open(SpaceSpec spec) {
  if (const auto why = policy_refusal(spec.policy)) throw std::invalid_argument(*why);
  const auto id = id_of(spec.name);
  Lock lock(_mutex);
  if (_spaces.contains(id)) throw std::invalid_argument("head space " + spec.name + " is open already");

  auto seed = std::make_unique<CacheSeed>(_services.cache, _services.content_key, _services.domain, spec.seed_magic, _logger);

  ReplicaOptions replica;
  replica.directory = space_directory(_services.state, id);
  replica.content_key = _services.content_key;
  replica.local_key = _services.local_key;
  replica.magics = spec.magics;
  replica.aad = replica_aad(_services.domain, id);
  replica.codec = spec.codec;
  replica.recovery_seed = seed->load(id);
  replica.accept_pristine_genesis_authority = spec.accept_pristine_genesis_authority;
  replica.materialization_cache_limit_bytes = spec.materialization_cache_limit_bytes;
  auto& control = _services.control;
  auto& source = _services.store;
  // Replay writes nodes locally and replicates nothing: the commit reached its holders
  // when made, and rebuilding a local head must not depend on peers.
  replica.tree_store = [&control, &source] { return std::make_unique<ControlTreeNodeStore>(ControlTreeNodeStore::for_replay(control, source)); };
  replica.time = &_services.context.time().source();
  replica.logger = _logger;

  SpaceServices services;
  services.self = _services.context.identity().id();
  services.commit_nodes = [&control, &source] { return std::make_unique<ControlTreeNodeStore>(ControlTreeNodeStore::for_commit(control, source)); };
  services.read_nodes = [&control, &source] { return std::make_unique<ControlTreeNodeStore>(ControlTreeNodeStore::for_reading(control, source)); };
  services.claims = &_barrier;
  services.wall = _services.wall;
  services.guard = &_guard;
  services.seed = seed.get();
  services.space = id;
  auto& nodes = _services.context.nodes();
  services.membership_stamp = [&nodes] {
    auto active = nodes.active();
    std::sort(active.begin(), active.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    uint64_t stamp = 1469598103934665603ULL;
    for (const auto& peer : active)
      for (const auto byte : peer.id.bytes) stamp = (stamp ^ byte) * 1099511628211ULL;
    return stamp;
  };

  auto space = std::make_unique<Space>(std::move(spec), std::move(replica), std::move(services));
  auto& out = *space;
  _seeds.emplace(id, std::move(seed));
  _spaces.emplace(id, std::move(space));
  return out;
}

Space* Spaces::find(const SpaceId& space) const {
  Lock lock(_mutex);
  const auto found = _spaces.find(space);
  return found == _spaces.end() ? nullptr : found->second.get();
}

}  // namespace dagda::heads
