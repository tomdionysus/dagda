//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "dagda/store/object_id.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/identity/node_id.h"

namespace dagda::store {

// What this node's repair is about to receive (the repair redesign, stage 3): the
// objects its own pull is fetching now, and those it has told an offering peer it
// wants. A peer's offer is answered from it, so each copy moves once: one this node
// is fetching is declined, and one it has said it wants is not fetched as well. A
// promise lasts until the copy arrives, the offering peer is gone, or the transfer's
// deadline (object_transfer_call) has passed: no new number. Thread-safe; waits on
// nothing.
class OfferBook {
 public:
  using NodeId = dannan::identity::NodeId;

  OfferBook(const dannan::contract::TimeSource& time, std::chrono::milliseconds hold);

  // A pull marked as in flight, until this goes.
  class Fetch {
   public:
    Fetch(Fetch&& other) noexcept : _book(other._book), _id(other._id) { other._book = nullptr; }
    Fetch& operator=(Fetch&&) = delete;
    ~Fetch();

   private:
    friend class OfferBook;
    Fetch(OfferBook& book, const ObjectId& id) : _book(&book), _id(id) {}
    OfferBook* _book;
    ObjectId _id;
  };

  // `peer` offers `id`: true when it is wanted from that peer, promised (again) from
  // now; false when this node's repair is fetching it, or another peer holds an
  // unexpired promise for it.
  bool want(const NodeId& peer, const ObjectId& id);
  // The copy of `id` landed here: its promise goes.
  void arrived(const ObjectId& id);
  // This node's repair is about to fetch `id`: nothing when a peer that `present`
  // names has an unexpired promise for it (the pull waits for that copy); otherwise
  // the pull is marked in flight, and the promise of a peer gone or expired is
  // dropped. `present` is asked under the book's lock: the lock order is the book's,
  // then any `present` takes.
  std::optional<Fetch> begin_fetch(const ObjectId& id, const std::function<bool(const NodeId&)>& present);
  // The promises held now, expired ones included until next touched.
  size_t promised() const;

 private:
  struct Promise {
    NodeId peer;
    dannan::contract::TimePoint until{};
  };
  void end_fetch(const ObjectId& id);

  const dannan::contract::TimeSource& _time;
  const std::chrono::milliseconds _hold;
  mutable dannan::contract::Mutex _mutex;
  std::set<ObjectId> _fetching DANNAN_GUARDED_BY(_mutex);
  std::map<ObjectId, Promise> _promised DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::store
