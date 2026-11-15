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

namespace {
constexpr uint8_t figures_version = 1;
constexpr size_t figures_count = 7;
}  // namespace

std::vector<uint8_t> encode_figures(const StoreFigures& figures) {
  const uint64_t values[figures_count] = {figures.used,       figures.backends_online, figures.cache_capacity, figures.cache_used,
                                          figures.cache_hits, figures.cache_misses,    figures.cache_evictions};
  std::vector<uint8_t> out;
  out.reserve(2 + 8 * figures_count);
  out.push_back(figures_version);
  out.push_back(figures.has_cache ? 1 : 0);
  for (const auto value : values)
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(value >> shift));
  return out;
}

std::optional<StoreFigures> figures_of(const dannan::membership::NodeRecord& node) {
  const auto found = node.adverts.find(figures_advert);
  if (found == node.adverts.end()) return std::nullopt;
  const auto& bytes = found->second;
  if (bytes.size() < 2 + 8 * figures_count || bytes[0] < figures_version) return std::nullopt;
  uint64_t values[figures_count] = {};
  for (size_t i = 0; i < figures_count; ++i)
    for (size_t b = 0; b < 8; ++b) values[i] = (values[i] << 8) | bytes[2 + 8 * i + b];
  StoreFigures out;
  out.has_cache = (bytes[1] & 1) != 0;
  out.used = values[0];
  out.backends_online = values[1];
  out.cache_capacity = values[2];
  out.cache_used = values[3];
  out.cache_hits = values[4];
  out.cache_misses = values[5];
  out.cache_evictions = values[6];
  return out;
}

uint64_t used_of(const dannan::membership::NodeRecord& node) {
  const auto figures = figures_of(node);
  return figures ? figures->used : 0;
}

std::vector<dannan::identity::NodeId> data_order(std::span<const uint8_t> key, const std::vector<dannan::membership::NodeRecord>& candidates, size_t count) {
  std::vector<dannan::identity::NodeId> out;
  for (const auto& record : dannan::placement::by_capacity(key, candidates, count, data_scheme())) out.push_back(record.id);
  return out;
}

}  // namespace dagda::store
