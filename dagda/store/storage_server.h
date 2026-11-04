//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "dagda/contract/claim_store.h"
#include "dagda/store/local_store.h"
#include "dagda/store/storage_pool.h"
#include "dannan/context.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/net/scheduler.h"

namespace dagda::store {

// Macha's figures for the serving side, with its defaults.
struct StorageServerOptions {
  // The largest DATA object a peer may put (Macha's extent_size); larger is
  // refused 413.
  size_t extent_size = 16 * 1024 * 1024;
  // Whether this node hosts DATA extents, and so is a placement candidate.
  // Macha's `hosts_extents: auto`: when DATA backends are configured.
  bool hosts_extents = true;
  // How often the pool is refreshed and its figures advertised.
  std::chrono::milliseconds refresh_interval{10000};
};

// The serving side of this node's stores (requirement S30; Macha's
// cluster/storage_server.cpp): answers peers' DATA and CONTROL object
// requests (presence, reads, writes, durability tickets and claims) as
// Dagda's routes on Dannan (messages.h), with protocol 23's payloads, and
// keeps the store's adverts current: the pool's limit as its capacity, what
// it holds, and whether it hosts extents.
//
// Not moved here (step 3b): have_valid_objects and trie_diff (repair's
// presence and the diff), delete_object, and the DATA credit arbiter's
// admission of each request; nothing here waits for DATA credit.
//
// The types must be registered (message::register_types) before the node
// starts; the server binds its handlers on construction and unbinds them,
// then stops its refresher, on destruction. Thread-safe; handlers run on
// Dannan's server workers and wait on the devices.
class StorageServer {
 public:
  struct Stores {
    StoragePool& data;
    LocalStore& control;
    contract::ClaimStore& claims;
  };

  StorageServer(dannan::Context& context, Stores stores, StorageServerOptions options = {});
  ~StorageServer();
  StorageServer(const StorageServer&) = delete;
  StorageServer& operator=(const StorageServer&) = delete;

  // Refreshes the pool and advertises its figures. Waits on the devices.
  void refresh();
  // Advertises the pool's figures as they are. Waits on nothing.
  void advertise();

  // One request, as the route bound for its type answers it: for a wrapper
  // that bound its own handler in front of the server's (a test's fault).
  dannan::net::Handled handle(const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame, const dannan::net::Message& request);

 private:
  void refresh_loop(std::stop_token stop);

  dannan::Context& _context;
  StoragePool& _data;
  LocalStore& _control;
  contract::ClaimStore& _claims;
  const StorageServerOptions _options;
  const dannan::status::ScopedLogger _logger;
  const dannan::status::NodeEvents::Kind _storage_event;
  // Guards nothing; the refresher waits on it.
  std::mutex _refresh_mutex;
  std::condition_variable_any _refresh_wake;
  std::jthread _refresher;
};

}  // namespace dagda::store
