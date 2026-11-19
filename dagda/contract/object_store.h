//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "dagda/ledger/trie_union.h"
#include "dagda/store/object_id.h"
#include "dannan/identity/crypto.h"
#include "dannan/sched/work.h"

// Bytes by id: what a store holds and whether it holds an object, nothing of
// references or claims. Implemented by a local store, and by a pool of them
// (step 1d). Requirements S18 and L4.
namespace dagda::contract {

using ObjectId = store::ObjectId;

// The identity of what a store holds: its held trie's root hash, which
// depends only on the held ids, so it changes when and only when what is
// held changes, and any two stores holding the same ids share it. `removals`
// counts ids that stopped being held since the process started (a removal, a
// loss, a disk going away): a view whose removals equal the store's never
// claims what the store has dropped, though the store may have gained since.
// `complete` is false while a disk is still being seeded, when the store
// cannot say what it holds.
struct HeldIdentity {
  dannan::identity::Hash256 hash{};
  uint64_t removals = 0;
  bool complete = false;
  bool operator==(const HeldIdentity&) const = default;
};

// A frozen view of what a store holds. `held` reads the view, not the store:
// ledger pages, through the ledger's cache from the state device, never a
// DATA device. Safe from any thread for as long as the view is kept.
struct HeldView {
  HeldIdentity identity;
  std::function<bool(const ObjectId&)> held;
  // Every held id: the union of each online disk's held trie, of those seeded
  // while the identity is incomplete. Null from a disk's own store until it
  // is seeded.
  std::shared_ptr<const ledger::TrieUnion> trie;
};

class ObjectStore {
 public:
  virtual ~ObjectStore() = default;

  // Whether this node holds the object. Never reports an object a put is
  // still writing, never decrypts, and changes nothing. Once the store's
  // presence is warm it waits on nothing; until then a miss is checked on
  // the store's device under the object's lock, which a put of that object
  // holds across its write.
  static constexpr dannan::sched::Waits has_waits = dannan::sched::Waits::data_device | dannan::sched::Waits::locks;
  static constexpr dannan::sched::ThreadSafety has_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual bool has(const ObjectId&) const = 0;

  // What this store holds, frozen at one moment, and the identity that names
  // it. Taken from the held ledgers: it reads no DATA device, only the ledger
  // nodes below what changed. A memo derived from a view is current exactly
  // while the store's identity equals the view's.
  static constexpr dannan::sched::Waits held_view_waits = dannan::sched::local_waits;
  static constexpr dannan::sched::ThreadSafety held_view_safety = dannan::sched::ThreadSafety::thread_safe;
  virtual HeldView held_view() const = 0;
};

}  // namespace dagda::contract
