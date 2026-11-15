//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/placement.h"

#include <utility>

namespace dagda::store {

const dannan::placement::Scheme& data_scheme() {
  static const dannan::placement::Scheme scheme{data_fallback_salt, data_extra_domain_salt, capacity_advert};
  return scheme;
}

dannan::membership::NodeRecord placement_candidate(const dannan::identity::NodeId& id, uint64_t capacity, std::string failure_domain) {
  dannan::membership::NodeRecord record;
  record.id = id;
  record.failure_domain = std::move(failure_domain);
  record.adverts[capacity_advert] = dannan::membership::u64_advert(capacity);
  return record;
}

std::vector<dannan::identity::NodeId> data_order(std::span<const uint8_t> key, const std::vector<dannan::membership::NodeRecord>& candidates, size_t count) {
  std::vector<dannan::identity::NodeId> out;
  for (const auto& record : dannan::placement::by_capacity(key, candidates, count, data_scheme())) out.push_back(record.id);
  return out;
}

}  // namespace dagda::store
