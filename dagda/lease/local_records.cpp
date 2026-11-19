//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/lease/local_records.h"

#include <boost/asio/post.hpp>
#include <utility>

namespace dagda::lease {

std::optional<Lease> LocalLeaseRecords::read(const std::string& scope, const std::string& name) const {
  dannan::contract::Lock lock(_mutex);
  const auto table = _tables.find(scope);
  if (table == _tables.end()) return std::nullopt;
  const auto* entry = table->second.find(name);
  if (!entry) return std::nullopt;
  return entry->lease;
}

void LocalLeaseRecords::join(const std::string& scope, const std::string& name, const Lease& proposed, std::chrono::milliseconds,
                             boost::asio::any_io_executor executor, JoinDone done) {
  Joined joined;
  {
    dannan::contract::Lock lock(_mutex);
    joined.standing = _tables[scope].join(LeaseEntry{name, proposed, 0}).lease;
  }
  boost::asio::post(executor, [done = std::move(done), joined = std::move(joined)]() mutable { done(std::move(joined)); });
}

}  // namespace dagda::lease
