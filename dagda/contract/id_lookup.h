//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "dagda/store/object_id.h"

namespace dagda::contract {

using ObjectId = store::ObjectId;

// A set of object ids asked one at a time: a sorted, duplicate-free span, or
// any membership test (a horizon's on-disk view). Empty by default. A span is
// not copied: it must outlive the lookup. Thread-safe if its test is.
class IdLookup {
 public:
  IdLookup() = default;
  IdLookup(std::span<const ObjectId> sorted) : _sorted(sorted) {}
  IdLookup(const std::vector<ObjectId>& sorted) : _sorted(sorted) {}
  explicit IdLookup(std::function<bool(const ObjectId&)> contains) : _contains(std::move(contains)) {}

  bool contains(const ObjectId& id) const { return _contains ? _contains(id) : std::binary_search(_sorted.begin(), _sorted.end(), id); }
  // The span it was made from; empty for a membership test.
  std::span<const ObjectId> sorted() const noexcept { return _contains ? std::span<const ObjectId>{} : _sorted; }

 private:
  std::span<const ObjectId> _sorted;
  std::function<bool(const ObjectId&)> _contains;
};

}  // namespace dagda::contract
