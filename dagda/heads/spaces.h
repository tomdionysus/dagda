//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "dagda/domain/domain_id.h"
#include "dagda/domain/storage_key.h"
#include "dagda/heads/cache_seed.h"
#include "dagda/heads/exchange.h"
#include "dagda/heads/node_pack.h"
#include "dagda/heads/space.h"
#include "dagda/store/distributed_store.h"
#include "dagda/store/local_store.h"
#include "dagda/store/persistent_cache.h"
#include "dannan/context.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/sched/work.h"

namespace dagda::heads {

// A commit's claims on the node's store, as Macha's retain_metadata_publication makes
// them.
class StoreClaimBarrier final : public ClaimBarrier {
 public:
  StoreClaimBarrier(store::DistributedStore& store, std::function<void()> on_claimed) : _store(store), _on_claimed(std::move(on_claimed)) {}
  void claimed() override {
    if (_on_claimed) _on_claimed();
  }
  std::vector<ObjectId> retain_data_here(const std::vector<ObjectId>& ids, const Dot& dot) override;
  std::vector<ObjectId> retain_data(const std::vector<ObjectId>& ids, const Dot& dot) override;
  bool retain_control_here(const std::vector<ObjectId>& ids, const Dot& dot) override;
  void retain_on_peers(const Dot& dot, const CommitClaims& claims) override;

 private:
  store::DistributedStore& _store;
  const std::function<void()> _on_claimed;
};

// A node-local space's claims (D3). Its tree nodes are in its own pack, which the space
// collects itself, so it claims no CONTROL object, and nothing goes to a peer. It names
// no DATA object: a claim made here under a node-local dot would have nothing to release
// it, so every DATA object is answered unheld and such a commit is refused.
class NodeLocalClaimBarrier final : public ClaimBarrier {
 public:
  void claimed() override {}
  std::vector<ObjectId> retain_data_here(const std::vector<ObjectId>& ids, const Dot&) override { return ids; }
  std::vector<ObjectId> retain_data(const std::vector<ObjectId>& ids, const Dot&) override { return ids; }
  bool retain_control_here(const std::vector<ObjectId>&, const Dot&) override { return true; }
  void retain_on_peers(const Dot&, const CommitClaims&) override {}
};

// The node's head spaces, at <state>/heads/<space id hex>/, and the one thread the
// Context's asynchronous forms run on: commits wait on devices and locks, which the io
// context's threads must not. Replay's node store writes every node it is given and
// fetches what it reads through the store's ensure_control_local.
//
// What peers ask of a space comes in here and goes to its Exchange, whichever space the
// message's id names, and the handler bound for each type is told who asked.
//
// Thread-safe. Lock order: _mutex, then a space's; the queue's lock nests in nothing.
class Spaces {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::thread_safe;
  static constexpr dannan::sched::Waits open_waits = Space::open_waits;
  static constexpr dannan::sched::Waits find_waits = dannan::sched::Waits::none;

  struct Services {
    dannan::Context& context;
    store::LocalStore& control;
    store::DistributedStore& store;
    const store::PersistentBlockCache& cache;
    std::shared_ptr<const domain::StorageKey> content_key;
    std::shared_ptr<const domain::StorageKey> local_key;
    domain::DomainId domain{};
    std::filesystem::path state;
    const dannan::contract::WallClock* wall = nullptr;
    ExchangeOptions exchange;
    // A set-aside head's wait, the domain's.
    std::chrono::milliseconds garbage_grace{std::chrono::hours(24 * 30)};
    // What repair's turns are timed by; none: the node's.
    const dannan::contract::TimeSource* time = nullptr;
    // In front of the handlers, as the node's Front is of the storage server's.
    std::function<std::optional<dannan::net::Handled>(const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame,
                                                      const dannan::net::Message& request)>
        front;
    // Told that a repair turn may be due.
    std::function<void()> wake;
    dannan::status::NodeEvents::Kind metadata_event = 0;
    dannan::status::NodeEvents::Kind claims_event = 0;
  };

  explicit Spaces(Services services);
  ~Spaces();
  Spaces(const Spaces&) = delete;
  Spaces& operator=(const Spaces&) = delete;

  // Opens a space and replays it. Throws std::invalid_argument for a policy that
  // waits on another session or a name already open, and what Replica throws,
  // naming the file, for primary state that fails with no valid seed. A node-local
  // space (D3) has no exchange: nothing is advertised, no route answers for it, and
  // its replica and tree nodes are sealed under the node-local key.
  Space& open(SpaceSpec spec) DANNAN_EXCLUDES(_mutex);

  // Each node-local space's upkeep (Space::upkeep_node_local), its pack compacted to
  // what its heads reach. The node's maintenance runs it beside history truncation.
  // Waits on the state device. Returns how many packs were compacted.
  size_t upkeep_node_local() DANNAN_EXCLUDES(_mutex);
  // The open space; null for one not open.
  Space* find(const SpaceId& space) const DANNAN_EXCLUDES(_mutex);
  // Its exchange; null for one not open.
  Exchange* exchange(const SpaceId& space) const DANNAN_EXCLUDES(_mutex);
  // Every exchange, in opening order. Each lives as long as this.
  std::vector<Exchange*> exchanges() const DANNAN_EXCLUDES(_mutex);
  SpaceId id_of(std::string_view name) const { return space_id(_services.domain, name); }

  // Runs `job` on the spaces' thread, unless stopping: false then, and it never runs.
  bool submit(std::function<void()> job) DANNAN_EXCLUDES(_queue_mutex);
  // Ends the thread; what is queued runs, told by `stopping()`. Waits on what is
  // running. Idempotent.
  void stop() DANNAN_EXCLUDES(_queue_mutex);
  bool stopping() const noexcept { return _stopping.load(std::memory_order_acquire); }

  dannan::sched::WaitGuard& guard() noexcept { return _guard; }

 private:
  Space& open_node_local_locked(SpaceSpec spec, const SpaceId& id) DANNAN_REQUIRES(_mutex);
  void worker(std::stop_token stop) DANNAN_EXCLUDES(_queue_mutex);
  // What a peer's request does.
  dannan::net::Handled handle(const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame, const dannan::net::Message& request);
  // The exchange a message's space id names, or the space that takes unaddressed ones.
  Exchange* addressed_to(const std::optional<SpaceId>& space) const DANNAN_EXCLUDES(_mutex);
  void bind_routes();
  void unbind_routes();

  const Services _services;
  StoreClaimBarrier _barrier;
  NodeLocalClaimBarrier _node_local_barrier;
  dannan::sched::WaitGuard _guard;
  const dannan::status::ScopedLogger _logger;

  // Held across a replay: spaces open one at a time.
  mutable dannan::contract::IoMutex _mutex;
  // Node-local spaces' generations and node packs: declared before the spaces, so they
  // outlive them. No generations without the node-local key.
  std::unique_ptr<NodeLocalGenerations> _generations;
  std::map<SpaceId, std::unique_ptr<NodePack>> _packs DANNAN_GUARDED_BY(_mutex);
  std::map<SpaceId, std::unique_ptr<Space>> _spaces DANNAN_GUARDED_BY(_mutex);
  std::map<SpaceId, std::unique_ptr<CacheSeed>> _seeds DANNAN_GUARDED_BY(_mutex);
  std::map<SpaceId, std::unique_ptr<Exchange>> _exchanges DANNAN_GUARDED_BY(_mutex);
  std::vector<Exchange*> _opened DANNAN_GUARDED_BY(_mutex);
  Exchange* _unaddressed DANNAN_GUARDED_BY(_mutex) = nullptr;
  std::vector<dannan::net::MessageType> _bound;

  dannan::contract::Mutex _queue_mutex;
  std::condition_variable_any _queue_changed;
  std::deque<std::function<void()>> _queue DANNAN_GUARDED_BY(_queue_mutex);
  std::atomic_bool _stopping{false};
  std::jthread _thread;
};

}  // namespace dagda::heads
