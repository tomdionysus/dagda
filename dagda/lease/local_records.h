//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <map>
#include <optional>
#include <string>

#include "dagda/lease/keeper.h"
#include "dagda/lease/lease_table.h"
#include "dannan/contract/thread_safety.h"

namespace dagda::lease {

// Lease records held in this node's memory, one LeaseTable per scope: what a
// node keeps until heads exist (step 4), when the records live in a head
// space the owner owns and are merged and replicated with it. Until then a
// record is this node's alone: it is not seen by another node, and it does
// not survive a restart. A join is applied at once and its completion posted.
// Thread-safe; nothing waits.
class LocalLeaseRecords final : public LeaseRecords {
 public:
  std::optional<Lease> read(const std::string& scope, const std::string& name) const override;
  void join(const std::string& scope, const std::string& name, const Lease& proposed, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
            JoinDone done) override;

 private:
  mutable dannan::contract::Mutex _mutex;
  std::map<std::string, LeaseTable> _tables DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dagda::lease
