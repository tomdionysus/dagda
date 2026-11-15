//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/control_node_store.h"

#include <stdexcept>

namespace dagda::heads {

ObjectId ControlTreeNodeStore::put(std::span<const uint8_t> node) {
  if (_mode == Mode::read) throw std::logic_error("tree node store opened for reading cannot write a node");
  const auto id = store::object_id(node);
  if (_mode == Mode::replay) {
    if (!_control.put(id, node)) throw std::runtime_error("tree node could not be written locally during replay: " + store::to_string(id));
    _written.push_back(id);
    return id;
  }
  // A node already held is immutable by content address, so skip it. A new one is
  // written here and nowhere else: a commit asks no peer. The peers get it from the
  // replicator's claims, or build it themselves from the commit's delta.
  if (_control.has(id)) return id;
  if (!_control.put(id, node)) throw std::runtime_error("tree node could not be written locally: " + store::to_string(id));
  _written.push_back(id);
  return id;
}

std::optional<Bytes> ControlTreeNodeStore::get(const ObjectId& id) const {
  if (auto local = _control.get(id)) return local;
  if (!_source.ensure_control_local(id)) return std::nullopt;
  return _control.get(id);
}

ObjectId LocalTreeNodeStore::put(std::span<const uint8_t> node) {
  const auto id = store::object_id(node);
  if (!_control.put(id, node)) throw std::runtime_error("tree node could not be written: " + store::to_string(id));
  _written.push_back(id);
  _bytes_written += node.size();
  return id;
}

std::optional<Bytes> LocalTreeNodeStore::get(const ObjectId& id) const { return _control.get(id); }

}  // namespace dagda::heads
