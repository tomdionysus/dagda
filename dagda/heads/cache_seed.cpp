//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/cache_seed.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"

namespace dagda::heads {
namespace {

std::filesystem::path seed_path(const store::PersistentBlockCache::SeedPlace& place, const SpaceId& space) {
  return place.directory / (dannan::identity::hex(space) + ".meta");
}

}  // namespace

CacheSeed::CacheSeed(const store::PersistentBlockCache& cache, std::shared_ptr<const domain::StorageKey> key, domain::DomainId domain, Magic magic,
                     dannan::status::ScopedLogger logger)
    : _cache(cache), _key(std::move(key)), _domain(domain), _magic(magic), _logger(std::move(logger)) {
  if (!_key) throw std::invalid_argument("a cache seed needs its key");
}

void CacheSeed::remember(const SpaceId& space, const Record& record) {
  (void)_cache.with_seed([&](const store::PersistentBlockCache::SeedPlace& place) {
    {
      dannan::contract::Lock lock(_mutex);
      if (const auto found = _remembered.find(space); found != _remembered.end() && found->second.hash == record.hash && found->second.opening == place.opening)
        return;
    }
    try {
      const auto sealed = seal_file(*_key, _magic, seed_aad(_domain, space, _magic), encode_record(record));
      dannan::identity::durable_replace_file(seed_path(place, space), std::string_view(reinterpret_cast<const char*>(sealed.data()), sealed.size()));
      dannan::contract::Lock lock(_mutex);
      _remembered[space] = {record.hash, place.opening};
    } catch (const std::exception& error) {
      _logger.debug("persistent metadata cache write: " + std::string(error.what()));
    }
  });
}

std::optional<Record> CacheSeed::load(const SpaceId& space) {
  std::optional<Record> out;
  (void)_cache.with_seed([&](const store::PersistentBlockCache::SeedPlace& place) {
    try {
      const auto path = seed_path(place, space);
      if (!std::filesystem::exists(path)) return;
      std::ifstream stream(path, std::ios::binary);
      const Bytes bytes(std::istreambuf_iterator<char>(stream), {});
      auto record = decode_record(open_file(*_key, _magic, seed_aad(_domain, space, _magic), bytes));
      dannan::contract::Lock lock(_mutex);
      _remembered[space] = {record.hash, place.opening};
      out = std::move(record);
    } catch (const std::exception& error) {
      _logger.debug("persistent metadata cache read: " + std::string(error.what()));
    }
  });
  return out;
}

}  // namespace dagda::heads
