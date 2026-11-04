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

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <future>
#include <memory>
#include <string>

#include "dagda/test/fixtures/cluster.h"

namespace dagda {
namespace {

using test::Cluster;
using test::ClusterNodeOptions;

TEST(Context, TheVersionCheckRefusesAMismatch) {
  Cluster cluster;
  auto& a = cluster.add("a");
  a.start();
  auto& api = a.api();
  EXPECT_EQ(api.version(), 1u);
  EXPECT_EQ(api.version(), context_version);
  const auto same = api.check_version(context_version);
  EXPECT_TRUE(same.accepted);
  EXPECT_TRUE(same.why.empty());
  const auto other = api.check_version(context_version + 1);
  EXPECT_FALSE(other.accepted);
  EXPECT_EQ(other.built_against, context_version + 1);
  EXPECT_EQ(other.served, context_version);
  EXPECT_NE(other.why.find("version 2 was asked for"), std::string::npos) << other.why;
  EXPECT_NO_THROW(api.require_version(context_version));
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

}  // namespace
}  // namespace dagda
