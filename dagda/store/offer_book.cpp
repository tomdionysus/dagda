//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/offer_book.h"

namespace dagda::store {

using dannan::contract::Lock;

OfferBook::OfferBook(const dannan::contract::TimeSource& time, std::chrono::milliseconds hold) : _time(time), _hold(hold) {}

OfferBook::Fetch::~Fetch() {
  if (_book) _book->end_fetch(_id);
}

bool OfferBook::want(const NodeId& peer, const ObjectId& id) {
  const auto now = _time.now();
  Lock lock(_mutex);
  // Expired promises go as offers come, so the table holds what is in flight.
  std::erase_if(_promised, [&](const auto& entry) { return entry.second.until <= now; });
  if (_fetching.contains(id)) return false;
  auto& promise = _promised[id];
  if (promise.until > now && promise.peer != peer) return false;
  promise = {peer, now + _hold};
  return true;
}

void OfferBook::arrived(const ObjectId& id) {
  Lock lock(_mutex);
  _promised.erase(id);
}

std::optional<OfferBook::Fetch> OfferBook::begin_fetch(const ObjectId& id, const std::function<bool(const NodeId&)>& present) {
  const auto now = _time.now();
  Lock lock(_mutex);
  if (const auto found = _promised.find(id); found != _promised.end()) {
    if (found->second.until > now && present(found->second.peer)) return std::nullopt;
    _promised.erase(found);
  }
  _fetching.insert(id);
  return Fetch(*this, id);
}

void OfferBook::end_fetch(const ObjectId& id) {
  Lock lock(_mutex);
  _fetching.erase(id);
}

size_t OfferBook::promised() const {
  Lock lock(_mutex);
  return _promised.size();
}

}  // namespace dagda::store
