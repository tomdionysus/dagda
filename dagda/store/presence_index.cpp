//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/presence_index.h"

namespace dagda::store {

void PresenceIndex::installed(const ObjectId& id) {
  _present.insert(id);
  _pruned.erase(id);
}

void PresenceIndex::observed(const ObjectId& id) { _present.insert(id); }

void PresenceIndex::forgotten(const ObjectId& id) {
  _present.erase(id);
  if (!_authoritative) _forgotten_while_warming.insert(id);
}

void PresenceIndex::pruned(const ObjectId& id) {
  forgotten(id);
  _pruned.insert(id);
}

void PresenceIndex::listed(std::span<const ObjectId> ids) {
  for (const auto& id : ids)
    if (!_pruned.contains(id) && !_forgotten_while_warming.contains(id)) _present.insert(id);
}

void PresenceIndex::warmed() {
  _authoritative = true;
  _forgotten_while_warming.clear();
}

PresenceIndex::Answer PresenceIndex::answer(const ObjectId& id) const {
  if (_present.contains(id)) return Answer::present;
  return _authoritative ? Answer::absent : Answer::unknown;
}

}  // namespace dagda::store
