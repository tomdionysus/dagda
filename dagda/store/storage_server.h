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
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "dagda/contract/claim_store.h"
#include "dagda/contract/object_ledger.h"
#include "dagda/domain/domain.h"
#include "dagda/ledger/trie_union.h"
#include "dagda/store/data_work.h"
#include "dagda/store/local_store.h"
#include "dagda/store/messages.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/storage_pool.h"
#include "dannan/context.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/net/scheduler.h"

namespace dagda::store {

// Macha's figures for the serving side, with its defaults, and Dagda's.
struct StorageServerOptions {
  // The largest DATA object a peer may put (Macha's extent_size); larger is
  // refused 413.
  size_t extent_size = 16 * 1024 * 1024;
  // Whether this node hosts DATA extents, and so is a placement candidate.
  // Macha's `hosts_extents: auto`: when DATA backends are configured.
  bool hosts_extents = true;
  // How often the pool is refreshed and its figures advertised.
  std::chrono::milliseconds refresh_interval{10000};
  // How long a served request waits for a control slot before it is refused
  // 503 storage_busy. A DATA request names no deadline: it waits under the
  // arbiter's no-progress window, as Macha's does.
  std::chrono::milliseconds admission_wait{30000};
  // The domain served: its name is the prefix a write or a claim is checked
  // under when the request names none (the maintainer, 2026-11-04 evening).
  domain::Domain domain = domain::domain_v1_descriptor();
};

// What a served request is admitted by: the node's DATA credit arbiter and
// its device I/O reserve. Either may be absent (a node or test that admits
// everything); both outlive the server.
struct StorageServerAdmission {
  DataResourceArbiter* credit = nullptr;
  IoReserve* io = nullptr;
};

// The serving side of this node's stores (requirement S30; Macha's
// cluster/storage_server.cpp): answers peers' DATA and CONTROL object
// requests (presence, validated presence, reads, writes, deletes, durability
// tickets and claims) and their questions to this node's held trie, as
// Dagda's routes on Dannan (messages.h), with protocol 23's payloads, and
// keeps the store's adverts current: the pool's limit as its capacity, what
// it holds, and whether it hosts extents.
//
// Scope (step 6): a put, a CONTROL put and a claim are checked through
// Identity::verify before anything is stored, under the principal and
// prefix the request carries (messages.h, WritePrincipal) or, for one that
// carries none, the sending node under the domain's name; a refusal is 403
// scope_refused with the source's reason and generation, and stores
// nothing. An instant delete (remove_objects, step 7b) is checked the same
// way under the writer it carries; a refusal is the reply's own evidence
// (remove_objects_reply, refused, with the source's reason and generation),
// and changes nothing.
//
// Admission (step 3b): a request that reads or writes the DATA device holds
// DATA credit for its class and a DATA slot of the device reserve across the
// device operation, as Macha's handlers held their lease; one on the CONTROL
// store or the claims (put_control_object, get_control_object,
// have_control_objects, retain_objects) holds a control slot and never DATA
// credit (law 1). A trie question reads the state device only and takes
// neither. A read or write of DATA (get_object, put_object, remove_objects)
// waits under the arbiter's no-progress window and fails 503 stopping, as
// Macha's does; a probe that must not wait (have_object, have_objects,
// have_valid_objects, delete_object) tries once and fails 503 storage_busy.
//
// The trie questions (requirement L7): a root question pins the node's
// current held union under its hash, at most four, each dropped two minutes
// after it was last asked about; later questions name it. 503
// holdings_seeding while a disk is being seeded, 410 snapshot_gone for a
// snapshot no longer pinned, 413 prefix_too_large for a records question
// past a leaf.
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
    // A delete removes the object here too, when given.
    PersistentBlockCache* cache = nullptr;
    // The ledger whose published inventory an instant delete checks: an
    // object it refers to is kept. None: no inventory.
    const contract::ObjectLedger* ledger = nullptr;
    // As DistributedStore::Stores: object bytes this node served. None: the node's.
    dannan::status::ActivityClocks* activity = nullptr;
  };

  StorageServer(dannan::Context& context, Stores stores, StorageServerOptions options = {}, StorageServerAdmission admission = {});
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

  // The messages the server answers, for a wrapper that binds in front.
  static const std::vector<dannan::net::MessageType>& served();

  // Whether this node is a placement candidate from now on: Macha's `auto`, which
  // follows the dial-back. Copies it holds stay, and repair drains them. Thread-safe.
  void set_hosts_extents(bool hosts);

 private:
  std::optional<DataAdmission> admit_data(dannan::sched::FrameType frame, uint64_t bytes, bool wait);
  std::optional<IoReserve::Slot> admit_control();
  // The refusal of a writer this node will not store for; nothing when it
  // may write.
  std::optional<dannan::net::Handled> scope_refusal(const dannan::membership::NodeRecord& peer, const std::optional<message::WritePrincipal>& writer) const;
  dannan::net::Handled answer_trie(const dannan::net::Message& request);
  void refresh_loop(std::stop_token stop);

  dannan::Context& _context;
  StoragePool& _data;
  LocalStore& _control;
  contract::ClaimStore& _claims;
  PersistentBlockCache* const _cache;
  const contract::ObjectLedger* const _ledger;
  const StorageServerOptions _options;
  const StorageServerAdmission _admission;
  const dannan::status::ScopedLogger _logger;
  const dannan::status::NodeEvents::Kind _storage_event;
  dannan::status::ActivityClocks& _activity;
  std::atomic<size_t> _online_backends{0};
  std::atomic_bool _hosts_extents;

  // The held unions pinned for peers' diffs, newest last, aged on the node's
  // time source (Macha reads the real clock here).
  struct Pinned {
    dannan::identity::Hash256 hash{};
    std::shared_ptr<const ledger::TrieUnion> trie;
    dannan::contract::TimePoint used{};
  };
  static constexpr size_t pinned_max = 4;
  static constexpr std::chrono::minutes pinned_for{2};
  dannan::contract::Mutex _pins_mutex;
  std::vector<Pinned> _pins DANNAN_GUARDED_BY(_pins_mutex);

  // Guards nothing; the refresher waits on it.
  std::mutex _refresh_mutex;
  std::condition_variable_any _refresh_wake;
  std::jthread _refresher;
};

}  // namespace dagda::store
