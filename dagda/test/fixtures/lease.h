//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// What the lease tests drive: a membership the test sets on stepped time, and
// lease records held in memory, one LeaseTable per scope, standing in for the
// head space step 4 builds. Several nodes share one MemoryLeaseRecords when
// they hold one converged head; each has its own across a partition, and
// reconcile() is the heads' merge when it heals.

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <chrono>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "dagda/lease/keeper.h"
#include "dagda/lease/lease_table.h"
#include "dagda/lease/membership.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dagda::test {

inline lease::NodeId node_of(uint8_t fill) {
  lease::NodeId node;
  node.bytes.fill(fill);
  return node;
}

// Members are active until left(); a node never joined is absent from when
// it is first asked about, as Dannan's roster has it.
class TestMembership final : public lease::Membership {
 public:
  TestMembership(lease::NodeId self, const dannan::contract::TimeSource& time) : _self(self), _time(time) {}

  void join(const lease::NodeId& node) {
    dannan::contract::Lock lock(_mutex);
    _active.insert(node);
    _absent.erase(node);
  }
  void leave(const lease::NodeId& node) {
    dannan::contract::Lock lock(_mutex);
    _active.erase(node);
    _absent.try_emplace(node, _time.now());
  }

  lease::NodeId self() const override { return _self; }
  std::optional<dannan::contract::TimePoint> absent_since(const lease::NodeId& node) const override {
    if (node == _self) return std::nullopt;
    dannan::contract::Lock lock(_mutex);
    if (_active.contains(node)) return std::nullopt;
    return _absent.try_emplace(node, _time.now()).first->second;
  }
  std::vector<lease::NodeId> active() const override {
    dannan::contract::Lock lock(_mutex);
    std::vector<lease::NodeId> out{_self};
    for (const auto& node : _active)
      if (node != _self) out.push_back(node);
    return out;
  }

 private:
  const lease::NodeId _self;
  const dannan::contract::TimeSource& _time;
  mutable dannan::contract::Mutex _mutex;
  std::set<lease::NodeId> _active DANNAN_GUARDED_BY(_mutex);
  mutable std::map<lease::NodeId, dannan::contract::TimePoint> _absent DANNAN_GUARDED_BY(_mutex);
};

class MemoryLeaseRecords final : public lease::LeaseRecords {
 public:
  // The next join fails with this reason, as a head write past its deadline.
  void fail_next(std::string reason) {
    dannan::contract::Lock lock(_mutex);
    _failure = std::move(reason);
  }

  std::optional<lease::Lease> read(const std::string& scope, const std::string& name) const override {
    dannan::contract::Lock lock(_mutex);
    const auto table = _tables.find(scope);
    if (table == _tables.end()) return std::nullopt;
    const auto* entry = table->second.find(name);
    if (!entry) return std::nullopt;
    return entry->lease;
  }

  void join(const std::string& scope, const std::string& name, const lease::Lease& proposed, std::chrono::milliseconds, boost::asio::any_io_executor executor,
            JoinDone done) override {
    Joined joined;
    {
      dannan::contract::Lock lock(_mutex);
      ++_joins;
      if (!_failure.empty()) {
        joined.failure = std::exchange(_failure, {});
      } else {
        joined.standing = _tables[scope].join(lease::LeaseEntry{name, proposed, 0}).lease;
      }
    }
    boost::asio::post(executor, [done = std::move(done), joined = std::move(joined)]() mutable { done(std::move(joined)); });
  }

  // The heads' merge when a partition heals: both sides end with the join.
  static void reconcile(MemoryLeaseRecords& a, MemoryLeaseRecords& b) {
    dannan::contract::Lock lock_a(a._mutex);
    dannan::contract::Lock lock_b(b._mutex);
    std::set<std::string> scopes;
    for (const auto& [scope, _] : a._tables) scopes.insert(scope);
    for (const auto& [scope, _] : b._tables) scopes.insert(scope);
    for (const auto& scope : scopes) {
      const auto merged = lease::LeaseTable::merge(nullptr, a._tables[scope], b._tables[scope]);
      a._tables[scope] = merged;
      b._tables[scope] = merged;
    }
  }

  int joins() const {
    dannan::contract::Lock lock(_mutex);
    return _joins;
  }

 private:
  // Two records' mutexes nest only in reconcile(), which the tests call
  // with nothing else running.
  mutable dannan::contract::Mutex _mutex;
  std::map<std::string, lease::LeaseTable> _tables DANNAN_GUARDED_BY(_mutex);
  std::string _failure DANNAN_GUARDED_BY(_mutex);
  int _joins DANNAN_GUARDED_BY(_mutex) = 0;
};

// acquire() run to its completion, waited on with a deadline.
inline lease::Acquisition acquire(lease::LeaseKeeper& keeper, lease::LeaseRecords& records, const std::string& scope, const std::string& name,
                                  boost::asio::any_io_executor executor, size_t rank = 0) {
  auto promise = std::make_shared<std::promise<lease::Acquisition>>();
  auto future = promise->get_future();
  keeper.acquire(
      records, scope, name, std::chrono::seconds(5), std::move(executor), [promise](lease::Acquisition out) { promise->set_value(std::move(out)); }, rank);
  if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) throw std::runtime_error("acquire did not complete");
  return future.get();
}

}  // namespace dagda::test
