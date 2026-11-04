//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <set>
#include <span>
#include <vector>

#include "dagda/store/object_id.h"

namespace dagda::store {

// Which loose objects a local store holds, as far as it knows, and whether it
// knows everything (requirement S18: has() is a cheap presence answer, never
// a decrypt). A primitive: no threads, no I/O, no lock of its own; the store
// holds its index mutex around every call, so its whole phase space is
// tested exhaustively against a model. Single owner: the store.
//
// Events, and the state each leaves for the id it names:
//   installed  a put finished writing it:      present, not pruned (a forgotten
//                                              mark may stay: only a listing of
//                                              an absent id reads it, and the id
//                                              can only become absent again by an
//                                              event that sets it anew)
//   observed   a stat or the scan saw it:      present
//   forgotten  removed (or its stamp lost):    absent; remembered while warming
//   pruned     an empty file was deleted:      absent and pruned; remembered while warming
//   listed     warm-up read its name:          present, unless pruned or forgotten
//   warmed     warm-up listed every name:      authoritative from now on; the
//                                              forgotten set is no longer needed
//
// Invariants: an id forgotten or pruned before warm-up publishes a listing
// that contains it stays absent; once authoritative, an id is present exactly
// when its last event was installed, observed or listed-and-kept.
class PresenceIndex {
 public:
  enum class Answer { present, absent, unknown };

  void installed(const ObjectId& id);
  void observed(const ObjectId& id);
  void forgotten(const ObjectId& id);
  void pruned(const ObjectId& id);
  void listed(std::span<const ObjectId> ids);
  void warmed();

  // present if known present; absent once authoritative; else unknown, and
  // the caller asks the device.
  Answer answer(const ObjectId& id) const;
  bool authoritative() const noexcept { return _authoritative; }
  size_t size() const noexcept { return _present.size(); }
  // Every id known present, in order: what seeds the store's ledger.
  std::vector<ObjectId> present() const { return {_present.begin(), _present.end()}; }

 private:
  std::set<ObjectId> _present;
  std::set<ObjectId> _pruned;
  std::set<ObjectId> _forgotten_while_warming;
  bool _authoritative = false;
};

}  // namespace dagda::store
