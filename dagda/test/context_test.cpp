//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Step 7a: dagda::Context, the API a hosted system holds (requirements C1,
// C2; stack document, "Async shape" and "Versions and message numbers"):
// the version check, and each noun's call completing on the executor the
// caller passed, never inline, on a two-node cluster.

#include "dagda/context.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "dagda/test/fixtures/cluster.h"

namespace dagda {
namespace {

using test::Cluster;
using test::ClusterNodeOptions;

// Version 2 is served from slice 1, and version 1, frozen at 0.1.4, beside it (Tom,
// 2026-11-13, "Ruled" 3): a system built against either is accepted, any other refused.
TEST(Context, TheVersionCheckRefusesAMismatch) {
  Cluster cluster;
  auto& a = cluster.add("a");
  a.start();
  auto& api = a.api();
  EXPECT_EQ(api.version(), 2u);
  EXPECT_EQ(api.version(), context_version);
  const auto same = api.check_version(context_version);
  EXPECT_TRUE(same.accepted);
  EXPECT_TRUE(same.why.empty());
  const auto frozen = api.check_version(context_version_frozen);
  EXPECT_EQ(context_version_frozen, 1u);
  EXPECT_TRUE(frozen.accepted);
  EXPECT_TRUE(frozen.why.empty());
  const auto other = api.check_version(context_version + 1);
  EXPECT_FALSE(other.accepted);
  EXPECT_EQ(other.built_against, context_version + 1);
  EXPECT_EQ(other.served, context_version);
  EXPECT_NE(other.why.find("version 3 was asked for"), std::string::npos) << other.why;
  EXPECT_NO_THROW(api.require_version(context_version));
  EXPECT_NO_THROW(api.require_version(context_version_frozen));
  EXPECT_THROW(api.require_version(0), ContextVersionMismatch);
  a.stop();
}

// Calls `call` from inside `strand`, handing it a completion that records
// whether it ran on that strand after the call had returned. The strand runs
// one handler at a time, so a completion called inline would see the call
// still in progress.
template <class T, class Call>
T on_strand(boost::asio::strand<boost::asio::io_context::executor_type>& strand, Call call) {
  auto promise = std::make_shared<std::promise<T>>();
  auto future = promise->get_future();
  auto in_call = std::make_shared<bool>(false);
  boost::asio::post(strand, [&strand, promise, in_call, call]() mutable {
    *in_call = true;
    call(boost::asio::any_io_executor(strand), [&strand, promise, in_call](T value) {
      EXPECT_TRUE(strand.running_in_this_thread()) << "completed off the caller's strand";
      EXPECT_FALSE(*in_call) << "completed inline";
      promise->set_value(std::move(value));
    });
    *in_call = false;
  });
  EXPECT_EQ(future.wait_for(std::chrono::seconds(30)), std::future_status::ready);
  return future.get();
}

TEST(Context, EachNounCompletesOnTheCallersStrandOnATwoNodeCluster) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.store.replication = 2;
  options.store.write_copies = 2;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  auto strand = boost::asio::make_strand(cluster.io().context);
  const auto deadline = std::chrono::milliseconds(10000);

  const auto domains = a.api().domains().list();
  ASSERT_EQ(domains.size(), 1u);
  EXPECT_EQ(domains[0].id, domain::domain_v1);
  EXPECT_TRUE(a.api().domains().describe(domain::domain_v1).has_value());
  EXPECT_FALSE(a.api().domains().describe(domain::node_local_domain(a.id())).has_value());

  // Write on a, replicated: both nodes, by node id.
  const auto data = test::numbered(42);
  const auto id = store::object_id(data);
  WriteOptions write;
  write.level = Durability::replicated;
  const auto evidence = on_strand<WriteEvidence>(strand, [&](boost::asio::any_io_executor executor, Context::Objects::WriteDone done) {
    a.api().objects().write(data, write, deadline, executor, std::move(done));
  });
  EXPECT_EQ(evidence.id, id);
  EXPECT_TRUE(evidence.met);
  EXPECT_EQ(evidence.acknowledged(), 2u);
  ASSERT_TRUE(evidence.ticket.valid());
  EXPECT_NE(evidence.on(b.id()), nullptr);

  // Read on b.
  const auto read = on_strand<ReadResult>(strand, [&](boost::asio::any_io_executor executor, Context::Objects::ReadDone done) {
    b.api().objects().read(id, {}, deadline, executor, std::move(done));
  });
  ASSERT_EQ(read.answer, store::FetchAnswer::ok);
  ASSERT_TRUE(read.object);
  EXPECT_EQ(read.object->bytes, data);

  // The ticket, waited for.
  const auto waited = on_strand<std::optional<WriteEvidence>>(strand, [&](boost::asio::any_io_executor executor, Context::Tickets::Done done) {
    a.api().tickets().wait(evidence.ticket, deadline, executor, std::move(done));
  });
  ASSERT_TRUE(waited.has_value());
  EXPECT_TRUE(waited->settled);
  EXPECT_TRUE(a.api().tickets().status(evidence.ticket).has_value());

  // A DATA claim from a: held where the object is.
  const contract::RetentionDot dot{a.id(), 1};
  const auto claim = on_strand<ClaimEvidence>(strand, [&](boost::asio::any_io_executor executor, Context::Claims::Done done) {
    a.api().claims().retain(contract::RetentionClass::data, {id}, dot, {}, deadline, executor, std::move(done));
  });
  EXPECT_TRUE(claim.finished) << claim.why;
  EXPECT_EQ(claim.sought, 1u);
  EXPECT_TRUE(claim.unheld.empty());
  EXPECT_TRUE(a.api().claims().retained(contract::RetentionClass::data, id) || b.api().claims().retained(contract::RetentionClass::data, id));

  // A lease: taken by a, then held.
  const auto taken = on_strand<lease::Acquisition>(strand, [&](boost::asio::any_io_executor executor, Context::Leases::Done done) {
    a.api().leases().acquire("test", "work", deadline, executor, std::move(done));
  });
  EXPECT_EQ(taken.outcome, lease::Acquisition::Outcome::taken) << taken.why;
  ASSERT_TRUE(taken.lease.has_value());
  EXPECT_EQ(taken.lease->holder, a.id());
  EXPECT_TRUE(a.api().leases().standing("test", "work", *taken.lease).holds);

  // The blocking forms wait on the same completions.
  const auto again = a.api().leases().acquire("test", "work", deadline);
  EXPECT_EQ(again.outcome, lease::Acquisition::Outcome::held) << again.why;
  const auto more = test::numbered(43);
  const auto blocking = a.api().objects().write(std::span<const uint8_t>(more), write, deadline);
  EXPECT_TRUE(blocking.met);
  EXPECT_EQ(b.api().objects().read(blocking.id, {}, deadline).answer, store::FetchAnswer::ok);
  const auto second_claim = a.api().claims().retain(contract::RetentionClass::data, {blocking.id}, {a.id(), 2}, {}, deadline);
  EXPECT_TRUE(second_claim.finished) << second_claim.why;

  // The instant delete (step 7b): a's claim, observed, goes, and with it
  // every copy, the answer on the caller's strand; the blocking form waits
  // on the same completion.
  const auto removed = on_strand<RemoveEvidence>(strand, [&](boost::asio::any_io_executor executor, Context::Objects::RemoveDone done) {
    a.api().objects().remove({id}, {{a.id(), 1}}, {}, deadline, executor, std::move(done));
  });
  EXPECT_TRUE(removed.finished) << removed.why;
  EXPECT_EQ(removed.state(a.id(), id), RemoveState::removed);
  EXPECT_EQ(removed.state(b.id(), id), RemoveState::removed);
  const auto blocking_removed = b.api().objects().remove({blocking.id}, {{a.id(), 2}}, {}, deadline);
  EXPECT_EQ(blocking_removed.removed(), 2u);
  EXPECT_FALSE(a.api().objects().has(blocking.id));

  // Arguments a contract refuses: no deadline, a dot with no sequence.
  EXPECT_THROW(a.api().objects().remove({id}, {}, {}, std::chrono::milliseconds(0)), std::invalid_argument);
  EXPECT_THROW(a.api().objects().read(id, {}, std::chrono::milliseconds(0)), std::invalid_argument);
  EXPECT_THROW(a.api().claims().retain(contract::RetentionClass::data, {id}, {a.id(), 0}, {}, deadline), std::invalid_argument);
}

// Contract version 2's object forms, Macha's (Tom, 2026-11-14, the route: Context
// serves every store call Macha makes, with Macha's semantics): a write naming its
// id, hashed once; a blocking read and write on the caller's own thread, no worker
// hop, with no deadline of their own unless the options name one; and one wait over
// many tickets, one barrier a place.
TEST(Context, VersionTwoServesMachasObjectForms) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.store.replication = 2;
  options.store.write_copies = 2;
  struct Stages {
    std::mutex mutex;
    std::vector<std::thread::id> on;
  };
  auto stages = std::make_shared<Stages>();
  auto& a = cluster.add("a", [&] {
    auto out = options;
    out.store.read_trace = [stages](std::string_view) {
      std::lock_guard lock(stages->mutex);
      stages->on.push_back(std::this_thread::get_id());
    };
    return out;
  }());
  cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  const auto data = test::numbered(44);
  const auto id = store::object_id(data);
  WriteOptions write;
  write.level = Durability::replicated;
  const auto written = a.api().objects().write(id, std::span<const uint8_t>(data), write);
  EXPECT_EQ(written.id, id);
  EXPECT_TRUE(written.met);
  // A wrong id is refused before anything is written.
  EXPECT_THROW((void)a.api().objects().write(store::object_id(test::numbered(45)), std::span<const uint8_t>(data), write), std::exception);

  {
    std::lock_guard lock(stages->mutex);
    stages->on.clear();
  }
  const auto read = a.api().objects().read(id, ReadOptions{});
  ASSERT_NE(read.object, nullptr);
  EXPECT_EQ(read.end, store::ReadEnd::served);
  {
    std::lock_guard lock(stages->mutex);
    ASSERT_FALSE(stages->on.empty());
    EXPECT_NE(std::find(stages->on.begin(), stages->on.end(), std::this_thread::get_id()), stages->on.end()) << "a blocking read hopped to a worker";
  }

  // Deferred writes, then one wait over all their tickets.
  WriteOptions deferred = write;
  deferred.deferred = true;
  std::vector<Ticket> tickets;
  for (int n = 46; n < 50; ++n) {
    const auto bytes = test::numbered(n);
    tickets.push_back(a.api().objects().write(store::object_id(bytes), std::span<const uint8_t>(bytes), deferred).ticket);
  }
  const auto waited = a.api().tickets().wait(tickets, std::chrono::seconds(10));
  ASSERT_EQ(waited.size(), tickets.size());
  for (const auto& one : waited) EXPECT_TRUE(one.met);

  auto strand = boost::asio::make_strand(cluster.io().context);
  const auto many = on_strand<std::vector<WriteEvidence>>(strand, [&](boost::asio::any_io_executor executor, Context::Tickets::ManyDone done) {
    a.api().tickets().wait(tickets, std::chrono::seconds(10), executor, std::move(done));
  });
  ASSERT_EQ(many.size(), tickets.size());
  for (const auto& one : many) EXPECT_TRUE(one.met);
}

// Contract version 2's filesystem forms, Macha's (distributed_store.hpp at f0597a4,
// the callers' map's filesystem and FUSE sections): a write durable here with the
// further copies owed, and its deferred form; the local view (pool, then cache);
// the cache written through and hydrated; a DATA object made local; claims here for
// what is held; and the reachability epoch a parked write retries on.
TEST(Context, VersionTwoServesMachasFilesystemForms) {
  Cluster cluster;
  ClusterNodeOptions options;
  options.store.replication = 2;
  options.store.write_copies = 2;
  auto& a = cluster.add("a", options);
  auto& b = cluster.add("b", options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  auto& objects = a.api().objects();

  const auto here = test::numbered(70);
  const auto here_id = store::object_id(here);
  EXPECT_TRUE(objects.put_here(here_id, std::span<const uint8_t>(here)));
  EXPECT_TRUE(objects.has(here_id));
  EXPECT_TRUE(objects.locally_available(here_id));

  const auto deferred = test::numbered(71);
  const auto deferred_id = store::object_id(deferred);
  const auto evidence = objects.put_deferred_here(deferred_id, std::span<const uint8_t>(deferred));
  ASSERT_TRUE(evidence.ticket.valid());
  const auto waited = a.api().tickets().wait(evidence.ticket, std::chrono::seconds(10));
  ASSERT_TRUE(waited.has_value());
  EXPECT_NE(waited->on(a.id()), nullptr);

  // Made local on b from a, then held there.
  EXPECT_TRUE(b.api().objects().ensure_local(here_id));
  EXPECT_TRUE(b.api().objects().has(here_id));

  // Claims here: what a holds, claimed; the rest returned.
  const auto absent = store::object_id(test::numbered(72));
  const auto rest = a.api().claims().retain_data_here({here_id, absent}, {a.id(), 9});
  ASSERT_EQ(rest.size(), 1u);
  EXPECT_EQ(rest[0], absent);
  EXPECT_TRUE(a.api().claims().retained(contract::RetentionClass::data, here_id));

  // No cache configured here: nothing to hydrate into, nothing written through.
  EXPECT_EQ(objects.hydration_available(), a.dagda_node().store().hydration_available());
  EXPECT_EQ(objects.reachability_epoch(), a.dagda_node().store().reachability_epoch());
}

// Contract version 2's bare CONTROL forms, Macha's catalogue and metadata calls (the
// callers' map, Macha d53ddfe): held here and read back; a strict local put, durable,
// that offers and claims nothing; a fetch to local from a peer, verified, that claims
// nothing; and an offer to every node, writing no claim. Claims stay Claims::retain.
TEST(Context, VersionTwoServesMachasBareControlForms) {
  Cluster cluster;
  auto& a = cluster.add("a");
  auto& b = cluster.add("b");
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  const auto data = test::numbered(60);
  const auto id = store::object_id(data);
  EXPECT_FALSE(a.api().control().has(id));
  EXPECT_FALSE(a.api().control().get(id).has_value());
  EXPECT_TRUE(a.api().control().put(id, std::span<const uint8_t>(data)));
  EXPECT_TRUE(a.api().control().has(id));
  ASSERT_TRUE(a.api().control().get(id).has_value());
  EXPECT_EQ(*a.api().control().get(id), data);
  EXPECT_FALSE(b.api().control().has(id));
  EXPECT_FALSE(a.api().claims().retained(contract::RetentionClass::control, id));
  EXPECT_THROW((void)a.api().control().put(store::object_id(test::numbered(61)), std::span<const uint8_t>(data)), std::exception);

  // Fetched to b from a, nothing claimed.
  EXPECT_TRUE(b.api().control().ensure_local(id));
  ASSERT_TRUE(b.api().control().get(id).has_value());
  EXPECT_EQ(*b.api().control().get(id), data);
  EXPECT_FALSE(b.api().claims().retained(contract::RetentionClass::control, id));
  EXPECT_FALSE(b.api().control().ensure_local(store::object_id(test::numbered(62))));

  // Offered from a to every node, nothing claimed.
  const auto more = test::numbered(63);
  const auto more_id = store::object_id(more);
  ASSERT_TRUE(a.api().control().put(more_id, std::span<const uint8_t>(more)));
  const auto offered = a.api().control().offer({more_id});
  EXPECT_EQ(offered.held_by, 2u);
  EXPECT_TRUE(b.api().control().has(more_id));
  EXPECT_FALSE(b.api().claims().retained(contract::RetentionClass::control, more_id));
  EXPECT_FALSE(a.api().claims().retained(contract::RetentionClass::control, more_id));
}

}  // namespace
}  // namespace dagda
