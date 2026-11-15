//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Barriers over the wire. From Macha's tests/test_invariants.cpp (808fa0f):
// test_deferred_object_barrier_rejects_stale_process_epoch and the two
// test_rpc_durability_barrier_* cases, through the storage server. Macha counted
// syncfs calls, Linux only; here the domain's own count of barriers, on any
// platform, and its window ends because the case moves the server's time on,
// never by waiting.
//
// And a wait over many tickets (Macha's test_durability_batch_retains_exact_
// nondominated_frontier and the batch's durability_barrier; the Macha session's
// answers, 2026-11-05): tickets replace the batch, so its frontier has no type
// here, and what it was for is proved instead. Each node, epoch, domain and
// backend instance is asked once, at the highest generation, or a FUSE group
// commit becomes N round trips; each ticket still answers for its own object.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <set>
#include <vector>

#include "dagda/store/distributed_store.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::identity::NodeId;
using dannan::net::CallOutcome;
using dannan::net::Handled;
using dannan::net::MessageType;
using dannan::sched::FrameType;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::pattern;
using test::wait_until;

ClusterNodeOptions options(size_t replication, size_t write_copies = 1) {
  ClusterNodeOptions out;
  out.store.replication = replication;
  out.store.write_copies = write_copies;
  return out;
}

// What Macha's BareNode::call did: one request from `client` to `server`,
// not through the store.
struct Wire {
  ClusterNode& client;
  ClusterNode& server;

  CallOutcome call(MessageType type, const Bytes& payload) {
    std::optional<dannan::membership::NodeRecord> record;
    for (const auto& known : client.context().nodes().active())
      if (known.id == server.id()) record = known;
    if (!record) {
      CallOutcome none;
      none.failure = "the server is not active";
      return none;
    }
    return client.context().routes().call_and_wait(*record, type, payload, FrameType::loader, 30s);
  }
  // Writes a new object and returns the place it took.
  DurabilityTicket defer(uint8_t seed) {
    const auto bytes = pattern(64 * 1024, seed);
    const auto outcome = call(message::put_object_deferred, message::encode_object(object_id(bytes), bytes));
    if (!outcome.reply || outcome.reply->type != dannan::net::message::ok) throw std::runtime_error("a deferred put was not taken: " + outcome.failure);
    return message::decode_ticket(outcome.reply->payload);
  }
  CallOutcome barrier(const DurabilityTicket& ticket) { return call(message::object_durability_barrier, message::encode_barrier({ticket, {}})); }
};

bool answered_ok(const CallOutcome& outcome) { return outcome.reply && outcome.reply->type == dannan::net::message::ok; }

// A server whose windows move only when the case moves them, known to its
// client. Its heartbeats are on that time too, so nobody declares it dead while
// it stands still.
struct SteppedServer {
  dannan::test::SteppedTime time;
  Cluster cluster{std::chrono::minutes(10)};
  ClusterNode& server;
  ClusterNode& client;
  static constexpr auto window = 500ms;

  SteppedServer() : server(cluster.add("server", server_options(time))), client(cluster.add("client", options(1))) {
    cluster.start_all();
    converged = wait_until(
        [&] {
          time.advance(1s);
          server.context().telemetry().refresh();
          return cluster.converged_once(50ms);
        },
        60s);
  }
  // The window ends, as often as it takes, until `done`.
  bool step_until(const std::function<bool()>& done) {
    return wait_until(
        [&] {
          time.advance(window);
          server.data().time_advanced();
          return done();
        },
        30s);
  }
  bool converged = false;

 private:
  static ClusterNodeOptions server_options(dannan::test::SteppedTime& on) {
    auto out = options(1);
    out.time = &on;
    out.durability_window = window;
    return out;
  }
};

// Macha: test_deferred_object_barrier_rejects_stale_process_epoch. A deferred
// put answers with the server's own epoch; a barrier naming any other is
// refused, and the one naming it succeeds.
TEST(StoreDurabilityBarrier, AStaleProcessEpochIsRefused) {
  Cluster cluster;
  auto& server = cluster.add("server", options(1));
  auto& client = cluster.add("client", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  Wire wire{client, server};

  const auto ticket = wire.defer(46);
  EXPECT_EQ(ticket.epoch, server.context().identity().durability_epoch());

  auto stale = ticket;
  stale.epoch.bytes[0] ^= 0x01;
  const auto rejected = wire.barrier(stale);
  ASSERT_TRUE(rejected.reply);
  const auto refusal = dannan::net::refusal_of(*rejected.reply);
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->status, 409);
  EXPECT_EQ(refusal->code, "epoch_changed");
  // Macha's line (storage_server.cpp:310 at 98b816d).
  EXPECT_TRUE(server.log().saw(dannan::status::LogLevel::debug,
                               "object durability barrier refused: epoch changed expected=" + dannan::identity::to_string(stale.epoch).substr(0, 8) +
                                   " current=" + dannan::identity::to_string(ticket.epoch).substr(0, 8) + " domain="));

  EXPECT_TRUE(answered_ok(wire.barrier(ticket)));
}

// Macha: test_rpc_durability_barrier_group_commits_independent_publications.
// Three publications that each ask for their own barrier, all inside one
// window, cost one barrier on the device.
TEST(StoreDurabilityBarrier, IndependentPublicationsGroupCommitOverTheWire) {
  SteppedServer bed;
  ASSERT_TRUE(bed.converged);
  auto& server = bed.server;
  Wire wire{bed.client, server};
  const auto barriers_asked = [&] { return server.calls_of(message::object_durability_barrier); };

  const auto a = wire.defer(53);
  const auto before = server.data().physical_barriers();
  auto first = std::async(std::launch::async, [&] { return wire.barrier(a); });
  ASSERT_TRUE(wait_until([&] { return barriers_asked() == 1; }));
  const auto b = wire.defer(54);
  auto second = std::async(std::launch::async, [&] { return wire.barrier(b); });
  ASSERT_TRUE(wait_until([&] { return barriers_asked() == 2; }));
  const auto c = wire.defer(55);
  auto third = std::async(std::launch::async, [&] { return wire.barrier(c); });
  ASSERT_TRUE(wait_until([&] { return barriers_asked() == 3; }));
  // The window has not ended, so nobody has been answered.
  EXPECT_EQ(server.data().physical_barriers(), before);
  EXPECT_EQ(first.wait_for(0ms), std::future_status::timeout);

  ASSERT_TRUE(bed.step_until([&] {
    return first.wait_for(0ms) == std::future_status::ready && second.wait_for(0ms) == std::future_status::ready &&
           third.wait_for(0ms) == std::future_status::ready;
  }));
  EXPECT_TRUE(answered_ok(first.get()));
  EXPECT_TRUE(answered_ok(second.get()));
  EXPECT_TRUE(answered_ok(third.get()));

  EXPECT_EQ(a.epoch, b.epoch);
  EXPECT_EQ(b.epoch, c.epoch);
  EXPECT_EQ(a.domain, b.domain);
  EXPECT_EQ(b.domain, c.domain);
  EXPECT_EQ(a.backend_instance, b.backend_instance);
  EXPECT_EQ(b.backend_instance, c.backend_instance);
  EXPECT_EQ(server.data().physical_barriers(), before + 1);
}

// Macha: test_rpc_durability_barrier_reuses_already_covered_generation. A
// generation a past cut covered costs nothing, and answers without a window
// ending, however dirty the domain is now; a newer one costs one.
TEST(StoreDurabilityBarrier, ACoveredGenerationCostsNoBarrierOverTheWire) {
  SteppedServer bed;
  ASSERT_TRUE(bed.converged);
  auto& server = bed.server;
  Wire wire{bed.client, server};

  const auto a = wire.defer(56);
  const auto b = wire.defer(57);
  ASSERT_EQ(a.epoch, b.epoch);
  ASSERT_EQ(a.domain, b.domain);
  ASSERT_EQ(a.backend_instance, b.backend_instance);
  ASSERT_GT(b.generation, a.generation);

  const auto start = server.data().physical_barriers();
  auto first = std::async(std::launch::async, [&] { return wire.barrier(a); });
  ASSERT_TRUE(bed.step_until([&] { return first.wait_for(0ms) == std::future_status::ready; }));
  EXPECT_TRUE(answered_ok(first.get()));
  EXPECT_EQ(server.data().physical_barriers(), start + 1);

  // The domain is dirty again with c; waiting for b must not flush it. No
  // window ends here: the answer comes with time standing still.
  const auto c = wire.defer(58);
  ASSERT_EQ(c.epoch, a.epoch);
  ASSERT_EQ(c.domain, a.domain);
  ASSERT_EQ(c.backend_instance, a.backend_instance);
  ASSERT_GT(c.generation, b.generation);
  EXPECT_TRUE(answered_ok(wire.barrier(b)));
  EXPECT_EQ(server.data().physical_barriers(), start + 1);

  auto latest = std::async(std::launch::async, [&] { return wire.barrier(c); });
  ASSERT_TRUE(bed.step_until([&] { return latest.wait_for(0ms) == std::future_status::ready; }));
  EXPECT_TRUE(answered_ok(latest.get()));
  EXPECT_EQ(server.data().physical_barriers(), start + 2);
}

// ---- A wait over many tickets -----------------------------------------------

// The barriers a node is asked for, as they arrived.
struct AskedBarriers {
  mutable dannan::contract::Mutex mutex;
  std::vector<message::BarrierRequest> asked DANNAN_GUARDED_BY(mutex);

  test::Interpose hook() {
    return [this](MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message& request) -> std::optional<Handled> {
      if (type == message::object_durability_barrier) {
        dannan::contract::Lock lock(mutex);
        asked.push_back(message::decode_barrier(request.payload));
      }
      return std::nullopt;
    };
  }
  std::vector<message::BarrierRequest> all() const {
    dannan::contract::Lock lock(mutex);
    return asked;
  }
};

WriteOptions deferred_replicated() {
  WriteOptions out;
  out.deferred = true;
  out.level = Durability::replicated;
  return out;
}

// A publication with more extents than the ticket table holds: Macha's batch is the
// caller's and has no bound, so every deferred write waited on is still barriered and
// met. The table forgets only settled tickets past its bound, never one still waiting
// (the store map, DistributedStore, gap 1: at Macha's 4 MiB extents a 16 GiB file is
// 4,096 of them).
TEST(StoreDurabilityBarrier, APublicationLargerThanTheTicketTableStillBarriersEveryExtent) {
  Cluster cluster;
  auto small_table = options(2, 2);
  small_table.store.max_tickets = 8;
  auto& a = cluster.add("a", small_table);
  cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  std::vector<Ticket> tickets;
  for (uint8_t i = 0; i < 20; ++i) {
    const auto bytes = pattern(32 * 1024, static_cast<uint8_t>(40 + i));
    const auto written = a.store().write(object_id(bytes), bytes, deferred_replicated());
    ASSERT_FALSE(written.met);
    tickets.push_back(written.ticket);
  }
  for (const auto& ticket : tickets) EXPECT_TRUE(a.store().status(ticket).has_value());
  const auto evidence = a.store().wait(tickets, 10s);
  ASSERT_EQ(evidence.size(), tickets.size());
  for (size_t i = 0; i < evidence.size(); ++i) {
    EXPECT_EQ(evidence[i].ticket, tickets[i]);
    EXPECT_TRUE(evidence[i].met) << "extent " << i;
  }
}

// A deferred write nobody waits on cannot hold memory for ever: past the ceiling on
// tickets still waiting, the oldest is forgotten and the store says so.
TEST(StoreDurabilityBarrier, TicketsStillWaitingAreBoundedByTheirCeiling) {
  Cluster cluster;
  auto tight = options(2, 2);
  tight.store.max_tickets = 2;
  tight.store.max_waiting_tickets = 4;
  auto& a = cluster.add("a", tight);
  cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  std::vector<Ticket> tickets;
  for (uint8_t i = 0; i < 6; ++i) {
    const auto bytes = pattern(32 * 1024, static_cast<uint8_t>(70 + i));
    tickets.push_back(a.store().write(object_id(bytes), bytes, deferred_replicated()).ticket);
  }
  EXPECT_FALSE(a.store().status(tickets[0]).has_value());
  EXPECT_FALSE(a.store().status(tickets[1]).has_value());
  for (size_t i = 2; i < tickets.size(); ++i) EXPECT_TRUE(a.store().status(tickets[i]).has_value()) << i;
  EXPECT_TRUE(a.log().saw(dannan::status::LogLevel::warn, "deferred write tickets over their ceiling"));
}

// Many deferred writes, one peer: one barrier call, at the highest generation
// the tickets name, and every ticket met by it. A second wait asks nothing. The
// same tickets waited on one at a time cost a call each.
TEST(StoreDurabilityBarrier, AWaitOverManyTicketsAsksEachPeerForOneBarrier) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  AskedBarriers asked;
  b.interpose(asked.hook());

  std::vector<Ticket> tickets;
  uint64_t highest = 0;
  for (uint8_t i = 0; i < 16; ++i) {
    const auto bytes = pattern(32 * 1024, static_cast<uint8_t>(100 + i));
    const auto written = a.store().write(object_id(bytes), bytes, deferred_replicated());
    ASSERT_EQ(written.holders(), 2u);
    EXPECT_FALSE(written.met);
    ASSERT_TRUE(written.on(b.id())->durability);
    highest = std::max(highest, written.on(b.id())->durability->generation);
    tickets.push_back(written.ticket);
  }

  const auto evidence = a.store().wait(tickets, 10s);
  ASSERT_EQ(evidence.size(), tickets.size());
  for (size_t i = 0; i < evidence.size(); ++i) {
    EXPECT_EQ(evidence[i].ticket, tickets[i]);
    EXPECT_TRUE(evidence[i].met);
    EXPECT_TRUE(evidence[i].settled);
    EXPECT_EQ(evidence[i].acknowledged(), 2u);
  }
  auto calls = asked.all();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front().ticket.generation, highest);
  EXPECT_TRUE(calls.front().probe.empty());

  (void)a.store().wait(tickets, 10s);
  EXPECT_EQ(asked.all().size(), 1u);

  std::vector<Ticket> alone;
  for (uint8_t i = 0; i < 3; ++i) {
    const auto bytes = pattern(32 * 1024, static_cast<uint8_t>(150 + i));
    const auto written = a.store().write(object_id(bytes), bytes, deferred_replicated());
    ASSERT_EQ(written.holders(), 2u);
    alone.push_back(written.ticket);
  }
  for (const auto& ticket : alone) ASSERT_TRUE(a.store().wait(ticket, 10s)->met);
  EXPECT_EQ(asked.all().size(), 4u);
}

// Replicas differ from object to object: each peer that holds any is asked
// once, whichever objects it holds, and this node's own copies wait on one
// barrier of its own.
TEST(StoreDurabilityBarrier, EachPeerIsAskedOnceWhicheverObjectsItHolds) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  auto& c = cluster.add("c", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  AskedBarriers on_b;
  AskedBarriers on_c;
  b.interpose(on_b.hook());
  c.interpose(on_c.hook());

  std::vector<Ticket> tickets;
  size_t held_by_b = 0;
  size_t held_by_c = 0;
  for (uint8_t i = 0; i < 24; ++i) {
    const auto bytes = pattern(16 * 1024, static_cast<uint8_t>(i));
    const auto written = a.store().write(object_id(bytes), bytes, deferred_replicated());
    ASSERT_GE(written.holders(), 2u);
    if (const auto* on = written.on(b.id()); on && on->durability) ++held_by_b;
    if (const auto* on = written.on(c.id()); on && on->durability) ++held_by_c;
    tickets.push_back(written.ticket);
  }
  // Any 24 objects land on both, or this case proves less than it says.
  ASSERT_GT(held_by_b, 1u);
  ASSERT_GT(held_by_c, 1u);

  const auto barriers_before = a.data().physical_barriers();
  for (const auto& evidence : a.store().wait(tickets, 10s)) EXPECT_TRUE(evidence.met);
  EXPECT_EQ(on_b.all().size(), 1u);
  EXPECT_EQ(on_c.all().size(), 1u);
  EXPECT_LE(a.data().physical_barriers(), barriers_before + 1);
}

// A place is its process: tickets from before a peer restarted and from after
// it are asked for apart. The dead ones are refused once and probed once with
// every id that named them; what the peer still holds comes back with a fresh
// ticket, and what it no longer holds refuses that ticket alone.
TEST(StoreDurabilityBarrier, ARestartedPeerIsProbedOnceWithEveryIdThatNamedIt) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  const auto write = [&](uint8_t seed) {
    const auto bytes = pattern(32 * 1024, seed);
    const auto written = a.store().write(object_id(bytes), bytes, deferred_replicated());
    EXPECT_EQ(written.holders(), 2u);
    return written;
  };
  std::vector<WriteEvidence> before;
  for (uint8_t i = 0; i < 6; ++i) before.push_back(write(static_cast<uint8_t>(200 + i)));
  const auto old_epoch = b.context().identity().durability_epoch();
  b.restart();
  ASSERT_TRUE(cluster.converged());
  AskedBarriers asked;
  b.interpose(asked.hook());
  ASSERT_TRUE(b.data().remove(before.back().id));
  const auto after = write(220);
  ASSERT_TRUE(after.on(b.id())->durability);
  EXPECT_EQ(after.on(b.id())->durability->epoch, b.context().identity().durability_epoch());

  std::vector<Ticket> tickets;
  for (const auto& written : before) tickets.push_back(written.ticket);
  tickets.push_back(after.ticket);
  std::vector<WriteEvidence> evidence;
  ASSERT_TRUE(wait_until(
      [&] {
        evidence = a.store().wait(tickets, 2s);
        return std::all_of(evidence.begin(), evidence.end(), [](const WriteEvidence& one) { return one.settled; });
      },
      30s));

  const auto epoch = b.context().identity().durability_epoch();
  for (size_t i = 0; i + 1 < before.size(); ++i) {
    EXPECT_TRUE(evidence[i].met) << i;
    ASSERT_TRUE(evidence[i].on(b.id())->durability);
    EXPECT_EQ(evidence[i].on(b.id())->durability->epoch, epoch);
  }
  EXPECT_FALSE(evidence[before.size() - 1].met);
  EXPECT_EQ(evidence[before.size() - 1].on(b.id())->state, ReplicaState::refused);
  EXPECT_EQ(evidence[before.size() - 1].on(b.id())->loss, BarrierLoss::absent_on_peer);
  EXPECT_EQ(evidence[before.size() - 1].on(a.id())->state, ReplicaState::acknowledged);
  EXPECT_EQ(evidence[before.size() - 1].on(a.id())->loss, BarrierLoss::none);
  EXPECT_TRUE(evidence.back().met);

  size_t plain = 0;
  size_t probes = 0;
  for (const auto& call : asked.all()) {
    if (call.probe.empty()) {
      ++plain;
    } else {
      ++probes;
      EXPECT_EQ(call.probe.size(), before.size());
    }
  }
  EXPECT_EQ(plain, 2u);
  EXPECT_EQ(probes, 1u);

  // Macha's lines on both sides (storage_server.cpp:331, distributed_store.cpp:725 and
  // :761 at 98b816d): the peer answers from disk, this node says what it re-derived,
  // and the copy the peer lost leaves its write short.
  using dannan::status::LogLevel;
  EXPECT_TRUE(b.log().saw(LogLevel::info,
                          "object durability re-derived after epoch change present=5/6 expected=" + dannan::identity::to_string(old_epoch).substr(0, 8) +
                              " current=" + dannan::identity::to_string(epoch).substr(0, 8)));
  EXPECT_TRUE(a.log().saw(LogLevel::info, "object durability re-derived after incarnation change reasserted=5 absent=1 peers=1"));
  EXPECT_TRUE(a.log().saw(LogLevel::debug, "object durability quorum unavailable id=" + to_string(before.back().id) + " required=2 durable=1 transient=no"));
}

// Macha's distributed_store.cpp:580 at 98b816d: a local barrier that fails is warned
// of. The copy is re-derived from disk (Tom, 2026-11-14: "healing is better"); with
// the backend gone it is absent, and the write is short of it.
TEST(StoreDurabilityBarrier, ALocalBarrierThatFailsIsWarnedOf) {
  Cluster cluster;
  // Nothing flushed by itself before the backend goes.
  auto one = options(1);
  one.durability_window = 2s;
  auto& a = cluster.add("a", one);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(32 * 1024, 77);
  const auto written = a.store().write(object_id(bytes), bytes, deferred_replicated());
  ASSERT_NE(written.on(a.id()), nullptr);
  ASSERT_TRUE(written.on(a.id())->durability);
  // The device's file and the filesystem's representative gone: no barrier can pass.
  std::filesystem::remove_all(a.backend());
  const auto evidence = a.store().wait(written.ticket, 10s);
  ASSERT_TRUE(evidence);
  EXPECT_FALSE(evidence->met);
  EXPECT_TRUE(a.log().saw(dannan::status::LogLevel::warn, "local storage durability barrier failed: "));
  // Definitive, and typed: put it again.
  ASSERT_NE(evidence->on(a.id()), nullptr);
  EXPECT_EQ(evidence->on(a.id())->state, ReplicaState::refused);
  EXPECT_EQ(evidence->on(a.id())->loss, BarrierLoss::absent_here);
}

// A barrier travels at the frame of the work it makes durable, as Macha's does: the
// artwork's at speculative (catalogue.hpp:520-521 at f0597a4), a publication's at its
// work frame (filesystem.cpp:1201), so artwork's barriers never compete with a
// publication's at loader (the store map, distributed store gap 3).
TEST(StoreDurabilityBarrier, ABarrierTravelsAtTheFrameItsWriteWasMadeAt) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  auto artwork = deferred_replicated();
  artwork.frame = FrameType::speculative;
  const auto bytes = pattern(32 * 1024, 91);
  const auto written = a.store().write(object_id(bytes), bytes, artwork);
  ASSERT_EQ(written.holders(), 2u);
  const auto evidence = a.store().wait(written.ticket, 10s);
  ASSERT_TRUE(evidence);
  EXPECT_TRUE(evidence->met);
  EXPECT_EQ(b.frames_of(message::object_durability_barrier), std::set<FrameType>{FrameType::speculative});
}

// Macha's barrier answers false at once on a transient failure (distributed_store.cpp
// :766-767 at f0597a4): the wait answers when its barriers have, with the copy still
// owed, not at the caller's deadline (the store map, distributed store gap 2).
TEST(StoreDurabilityBarrier, ATransientBarrierFailureAnswersAtOnceNotAtTheDeadline) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  b.interpose([](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == message::object_durability_barrier) return Handled::failed(500, "barrier_failed", "storage durability barrier failed: injected");
    return std::nullopt;
  });
  const auto bytes = pattern(32 * 1024, 92);
  const auto written = a.store().write(object_id(bytes), bytes, deferred_replicated());
  ASSERT_EQ(written.holders(), 2u);
  const auto started = std::chrono::steady_clock::now();
  const auto evidence = a.store().wait(written.ticket, 30s);
  const auto took = std::chrono::steady_clock::now() - started;
  ASSERT_TRUE(evidence);
  EXPECT_LT(took, 10s) << "the wait ran on to its deadline";
  EXPECT_FALSE(evidence->met);
  EXPECT_FALSE(evidence->settled);
  ASSERT_NE(evidence->on(b.id()), nullptr);
  EXPECT_EQ(evidence->on(b.id())->state, ReplicaState::pending);
  EXPECT_EQ(evidence->on(b.id())->loss, BarrierLoss::none);
  EXPECT_EQ(evidence->on(a.id())->state, ReplicaState::acknowledged);
}

// A deferred write that ended short of its floor (no further copy could be had
// promptly) is made durable at what landed, as Macha's batch required what landed
// (distributed_store.cpp:375, :475 at f0597a4); the copies it is owed are prompt
// replication's and repair's (the store map, distributed store gap 4).
TEST(StoreDurabilityBarrier, AWriteThatEndedShortIsMetAtWhatLanded) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  b.interpose([](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == message::put_object_deferred || seen == message::put_object) return Handled::failed(507, "storage_full", "storage limit reached");
    return std::nullopt;
  });
  const auto bytes = pattern(32 * 1024, 93);
  const auto written = a.store().write(object_id(bytes), bytes, deferred_replicated());
  ASSERT_EQ(written.holders(), 1u);
  EXPECT_EQ(written.sought, 1u);
  const auto evidence = a.store().wait(written.ticket, 10s);
  ASSERT_TRUE(evidence);
  EXPECT_TRUE(evidence->met);
  EXPECT_EQ(evidence->on(a.id())->state, ReplicaState::acknowledged);
}

// The asynchronous form posts its evidence, never inline, in the order asked;
// nothing to wait for and a forgotten ticket are answers too.
TEST(StoreDurabilityBarrier, AWaitOverManyTicketsPostsItsEvidenceInOrder) {
  Cluster cluster;
  auto& a = cluster.add("a", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(32 * 1024, 7);
  WriteOptions deferred;
  deferred.deferred = true;
  deferred.level = Durability::persisted;
  const auto written = a.store().write(object_id(bytes), bytes, deferred);

  boost::asio::io_context caller;
  std::optional<std::vector<WriteEvidence>> told;
  a.store().wait({Ticket{987654321}, written.ticket}, 10s, caller.get_executor(), [&](std::vector<WriteEvidence> evidence) { told = std::move(evidence); });
  EXPECT_FALSE(told.has_value());
  ASSERT_TRUE(wait_until([&] {
    (void)caller.poll();
    caller.restart();
    return told.has_value();
  }));
  ASSERT_EQ(told->size(), 2u);
  EXPECT_FALSE((*told)[0].ticket.valid());
  EXPECT_EQ((*told)[1].ticket, written.ticket);
  EXPECT_TRUE((*told)[1].met);

  told.reset();
  a.store().wait(std::vector<Ticket>{}, 10s, caller.get_executor(), [&](std::vector<WriteEvidence> evidence) { told = std::move(evidence); });
  EXPECT_FALSE(told.has_value());
  (void)caller.poll();
  ASSERT_TRUE(told.has_value());
  EXPECT_TRUE(told->empty());
}

}  // namespace
}  // namespace dagda::store
