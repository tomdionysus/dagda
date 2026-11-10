//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The distributed store on Dannan, on in-process clusters: the write path
// and its four durability levels, tickets, the read path, CONTROL objects
// and claims (requirements S22 to S30). Macha's cases come from
// tests/test_rpc_cluster.cpp and tests/test_storage_v18.cpp (develop
// 5a06d3c), converted to GoogleTest on real Dannan nodes in place of
// Macha's fake transport: a peer that Macha's fake answered by hand is a
// node whose requests the test interposes on. Where Macha's assertion was
// "returns after write_copies", the case writes at the matching level.

#include "dagda/test/fixtures/store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <future>
#include <thread>

#include "dagda/ledger/collector.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/store/distributed_store.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/reachability.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::identity::NodeId;
using dannan::net::Handled;
using dannan::net::MessageType;
using dannan::sched::FrameType;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::pattern;
using test::wait_until;

ClusterNodeOptions options(size_t replication, size_t write_copies = 1, uint64_t limit = 64ULL * 1024 * 1024) {
  ClusterNodeOptions out;
  out.limit = limit;
  out.store.replication = replication;
  out.store.write_copies = write_copies;
  return out;
}

// Holds every request of `type` at `gate` until it opens, then lets the
// server answer.
test::Interpose hold(MessageType type, dannan::test::Gate& gate) {
  return [type, &gate](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == type) gate.enter_and_wait();
    return std::nullopt;
  };
}

// An object of `size` bytes whose first placement choice, as `observer`
// ranks it, is `preferred` (Macha's preferred_for).
Bytes preferred_for(ClusterNode& observer, const NodeId& preferred, size_t size, uint8_t salt = 0) {
  auto data = pattern(size, salt);
  for (uint32_t i = 0; i < 4096; ++i) {
    for (size_t b = 0; b < 4 && b < data.size(); ++b) data[b] = static_cast<uint8_t>(i >> (8 * b));
    const auto ranked = observer.store().ranked(object_id(data));
    if (!ranked.empty() && ranked.front().id == preferred) return data;
  }
  throw std::runtime_error("could not find an object preferring the requested node");
}

// Puts objects until the pool refuses one (Macha's fill_data_store).
void fill_data_store(StoragePool& pool, size_t object_bytes = 96 * 1024) {
  for (unsigned i = 0; i < 4096; ++i) {
    auto data = pattern(object_bytes, static_cast<uint8_t>(i));
    data[0] ^= static_cast<uint8_t>(i * 17U);
    data[1] = static_cast<uint8_t>(i >> 8);
    if (!pool.put(object_id(data), data)) return;
  }
  throw std::runtime_error("test data store did not reach its admission limit");
}

// ---- Moved from Macha's test_rpc_cluster.cpp --------------------------------

// test_store_put_is_loader_activity_not_viewer_activity: a put is loader
// work: it refreshes the loader clock and leaves the viewer clocks, which
// pace maintenance and gate DATA pressure, untouched.
TEST(DistributedStore, APutIsLoaderActivityNotViewerActivity) {
  Cluster cluster;
  auto& node = cluster.add("solo", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  auto& activity = node.dagda_node().activity();
  for (const auto frame : {FrameType::loader, FrameType::foreground, FrameType::read_ahead}) (void)activity.take_bytes(frame);
  const auto bytes = pattern(256 * 1024, 91);
  ASSERT_EQ(node.store().put(bytes), object_id(bytes));
  EXPECT_LT(activity.idle_for(dannan::sched::WorkClass::loader), 5s);
  EXPECT_GE(activity.take_bytes(FrameType::loader), bytes.size());
  EXPECT_EQ(activity.take_bytes(FrameType::foreground), 0u);
  EXPECT_EQ(activity.take_bytes(FrameType::read_ahead), 0u);
  EXPECT_FALSE(activity.recently_active(dannan::sched::WorkClass::foreground, 30s));
}

// test_store_put_publishes_at_the_floor_and_copies_promptly: a put returns
// at write_copies with the local copy, never waiting for owners beyond the
// floor; the prompt worker then copies it to one more owner, and never to an
// owner that advertises no room.
TEST(DistributedStore, APutPublishesAtTheFloorAndCopiesPromptly) {
  {
    Cluster cluster;
    auto& writer = cluster.add("writer", options(3));
    auto& first = cluster.add("first", options(3));
    auto& second = cluster.add("second", options(3));
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    dannan::test::Gate owners_answer;
    dannan::test::GateOpener opener(owners_answer);
    first.interpose(hold(message::put_object, owners_answer));
    second.interpose(hold(message::put_object, owners_answer));

    const auto data = pattern(128 * 1024);
    const auto id = object_id(data);
    const auto evidence = writer.store().write(id, data, {});
    EXPECT_EQ(owners_answer.entered(), 0u);
    EXPECT_TRUE(evidence.met);
    EXPECT_EQ(evidence.acknowledged(), 1u);
    EXPECT_TRUE(writer.data().has(id));
    owners_answer.open();
    ASSERT_TRUE(wait_until([&] { return writer.store().prompt_replication_stats().copies == 1; }));
    EXPECT_EQ(cluster.calls_of(message::put_object), 1u);
    // The copy reaches the write's ticket.
    ASSERT_TRUE(wait_until([&] { return writer.store().status(evidence.ticket)->acknowledged() == 2; }));
  }
  {
    // An owner advertising no room for an extent is no destination: nothing
    // is sent, and the object is left to repair.
    Cluster cluster;
    auto& writer = cluster.add("writer", options(2));
    auto& full = cluster.add("full", options(2, 1, 8ULL * 1024 * 1024));
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    const auto other = pattern(128 * 1024, 7);
    ASSERT_TRUE(writer.store().put(object_id(other), other));
    ASSERT_TRUE(wait_until([&] { return writer.store().prompt_replication_stats().skipped_no_room == 1; }));
    const auto stats = writer.store().prompt_replication_stats();
    EXPECT_EQ(stats.dropped, 1u);
    EXPECT_EQ(stats.copies, 0u);
    EXPECT_EQ(stats.failures, 0u);
    EXPECT_EQ(full.calls_of(message::put_object), 0u);
  }
}

// test_store_put_replaces_a_replica_that_cannot_be_launched: a replica whose
// call fails at once counts as failed, so the put launches the next owner in
// its place rather than waiting on it.
TEST(DistributedStore, APutReplacesAReplicaThatFailsAtOnce) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(3, 2));
  cluster.add("one", options(3, 2));
  cluster.add("two", options(3, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto data = pattern(64 * 1024, 5);
  std::vector<NodeId> remote;
  for (const auto& node : writer.store().ranked(object_id(data)))
    if (node.id != writer.id()) remote.push_back(node.id);
  ASSERT_EQ(remote.size(), 2u);
  ClusterNode* refused = nullptr;
  ClusterNode* standby = nullptr;
  for (auto& node : cluster.nodes()) {
    if (node->id() == remote[0]) refused = node.get();
    if (node->id() == remote[1]) standby = node.get();
  }
  refused->interpose([](MessageType type, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (type == message::put_object) return Handled::failed(503, "unavailable", "this replica cannot take a call");
    return std::nullopt;
  });
  // Bounds a put that never replaces the refused replica.
  std::atomic_bool cancelled{false};
  std::jthread watchdog([&](std::stop_token stop) {
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(5ms);
    if (!stop.stop_requested()) cancelled = true;
  });
  WriteOptions write;
  write.cancelled = &cancelled;
  EXPECT_TRUE(writer.store().put(object_id(data), data, write));
  watchdog.request_stop();
  EXPECT_FALSE(cancelled.load());
  EXPECT_EQ(standby->calls_of(message::put_object), 1u);
}

// test_store_claims_ask_nothing_more_about_objects_nobody_holds: claiming
// objects no node present holds costs the one batched presence scan and
// nothing per object: they are reported unheld. An object the peer does
// hold is claimed there. (The claiming node hosts no extents here, so the
// peer is every object's one candidate, as Macha's bench made it by
// capacity.)
TEST(DistributedStore, ClaimsAskNothingMoreAboutObjectsNobodyHolds) {
  Cluster cluster;
  auto writer_options = options(2);
  writer_options.hosts_extents = false;
  auto& writer = cluster.add("writer", writer_options);
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto held_bytes = pattern(256, 201);
  const auto held = object_id(held_bytes);
  ASSERT_TRUE(peer.data().put(held, held_bytes));
  std::vector<ObjectId> ids{held};
  for (uint8_t i = 0; i < 40; ++i) ids.push_back(object_id(pattern(128, i)));
  const auto unheld = writer.store().retain_data(ids, contract::RetentionDot{writer.id(), 1});
  EXPECT_EQ(unheld.size(), 40u);
  EXPECT_FALSE(std::binary_search(unheld.begin(), unheld.end(), held));
  EXPECT_EQ(peer.calls_of(message::have_objects), 1u);
  EXPECT_EQ(peer.calls_of(message::get_object), 0u);
  EXPECT_EQ(peer.calls_of(message::have_object), 0u);
  EXPECT_EQ(peer.calls_of(message::retain_objects), 1u);
  EXPECT_TRUE(peer.claims().retained(contract::RetentionClass::data, held));
}

// test_store_put_to_a_silent_replica_returns_on_the_local_copy: a put whose
// second replica never answers returns on the local copy once the peer has
// stalled, well inside its deadline, and the same put reaches the peer once
// it answers.
TEST(DistributedStore, APutToASilentReplicaReturnsOnTheLocalCopy) {
  Cluster cluster;
  auto silent_options = options(2, 2);
  silent_options.store.write_stall = 50ms;
  auto& writer = cluster.add("writer", silent_options);
  auto& peer = cluster.add("peer", silent_options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  dannan::test::Gate held;
  dannan::test::GateOpener opener(held);
  peer.interpose(hold(message::put_object_deferred, held));

  const auto data = pattern(64 * 1024, 3);
  WriteOptions write;
  write.deferred = true;
  write.deadline = 30s;
  const auto started = std::chrono::steady_clock::now();
  const auto evidence = writer.store().write(object_id(data), data, write);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(elapsed, 50ms);
  EXPECT_LT(elapsed, 5s);
  EXPECT_TRUE(writer.data().has(object_id(data)));
  EXPECT_GE(evidence.holders(), 1u);
  EXPECT_EQ(evidence.on(peer.id())->state, ReplicaState::pending);

  held.open();
  const auto before = peer.calls_of(message::put_object_deferred);
  const auto more = pattern(64 * 1024, 4);
  EXPECT_EQ(writer.store().put_deferred(more, write), object_id(more));
  EXPECT_EQ(peer.calls_of(message::put_object_deferred), before + 1);
}

// test_store_concurrent_readers_share_one_fetch_and_buffer: readers of one
// missing object share a single fetch and a single buffer; the reply's
// retained memory lives as long as the last reader's handle.
TEST(DistributedStore, ConcurrentReadersShareOneFetchAndBuffer) {
  Cluster cluster;
  auto& reader = cluster.add("reader", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(512 * 1024, 73);
  const auto id = object_id(bytes);
  ASSERT_TRUE(peer.data().put(id, bytes));
  dannan::test::Gate reply_gate;
  dannan::test::GateOpener opener(reply_gate);
  peer.interpose(hold(message::get_object, reply_gate));
  const auto held = [&] {
    const auto stats = reader.node().memory().stats();
    const auto found = stats.owner_bytes.find(dannan::sched::MemoryOwner::rpc_frame);
    return found == stats.owner_bytes.end() ? uint64_t{0} : found->second;
  };

  ReadOptions read;
  read.deadline = 30s;
  auto first = std::async(std::launch::async, [&] { return reader.store().read(id, read); });
  ASSERT_TRUE(reply_gate.wait_for_entries(1));
  auto second = std::async(std::launch::async, [&] { return reader.store().read(id, read); });
  // The second reader joins the fetch in flight; it has no signal to wait on.
  std::this_thread::sleep_for(20ms);
  reply_gate.open();
  auto a = first.get();
  auto b = second.get();
  ASSERT_NE(a.object, nullptr);
  ASSERT_NE(b.object, nullptr);
  EXPECT_EQ(a.object.get(), b.object.get());
  EXPECT_EQ(a.object->bytes, bytes);
  EXPECT_EQ(a.source, ReadSource::remote);
  EXPECT_EQ(a.served_by, peer.id());
  EXPECT_EQ(peer.calls_of(message::get_object), 1u);
  EXPECT_GE(held(), bytes.size());
  a.object.reset();
  EXPECT_GE(held(), bytes.size());
  b.object.reset();
  EXPECT_TRUE(wait_until([&] { return held() == 0; }));
}

// test_store_heals_a_bad_local_copy_from_a_peer: a corrupt local copy (it
// fails authentication) is never served: the read comes from a peer and the
// fetched bytes replace it, as they do a copy that is gone. Unread, scrub
// finds a corrupt copy and discards it, and repair restores it from what
// this node lacks.
TEST(DistributedStore, HealsABadLocalCopyFromAPeer) {
  Cluster cluster;
  auto& reader = cluster.add("reader", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(256 * 1024, 7);
  const auto id = object_id(bytes);
  ASSERT_TRUE(peer.data().put(id, bytes));
  auto& data = reader.data();
  ASSERT_TRUE(data.put(id, bytes));

  test::corrupt_object(reader.backend(), id);
  ASSERT_FALSE(data.get(id).has_value());
  const auto fetched = reader.store().get(id);
  ASSERT_TRUE(fetched.has_value());
  EXPECT_EQ(*fetched, bytes);
  reader.store().wait_local_copies_settled();
  EXPECT_EQ(data.get(id), bytes);

  ASSERT_TRUE(data.remove(id));
  EXPECT_EQ(reader.store().get(id), bytes);
  reader.store().wait_local_copies_settled();
  EXPECT_EQ(data.get(id), bytes);

  test::corrupt_object(reader.backend(), id);
  (void)data.scrub_step(128ULL * 1024 * 1024, 256);
  EXPECT_FALSE(data.has(id));
  const std::vector<ObjectId> live{id};
  const RepairLists lists{std::make_shared<const contract::InventoryHorizon>(1, true, live, std::vector<ObjectId>{}, std::vector<contract::Garbage>{}),
                          std::make_shared<const std::vector<ObjectId>>(live),
                          {},
                          {}};
  for (int step = 0; step < 4 && !data.has(id); ++step) (void)reader.store().repair_step(lists, 128ULL * 1024 * 1024, 0);
  EXPECT_EQ(data.get(id), bytes);
}

// test_store_runtime_cache_keeps_a_playback_fetch: a persistent cache
// enabled at runtime keeps a playback fetch: the bytes land in the cache
// rather than the DATA store, and answer a later read.
TEST(DistributedStore, ARuntimeCacheKeepsAPlaybackFetch) {
  Cluster cluster;
  auto& reader = cluster.add("reader", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(256 * 1024, 11);
  const auto id = object_id(bytes);
  ASSERT_TRUE(peer.data().put(id, bytes));
  reader.cache().reconfigure({cluster.dir() / "reader" / "cache", 8, true});
  ASSERT_TRUE(reader.cache().enabled());

  ReadOptions playback;
  playback.frame = FrameType::foreground;
  ASSERT_EQ(reader.store().get(id, playback), bytes);
  reader.store().wait_local_copies_settled();
  EXPECT_TRUE(reader.cache().has(id));
  EXPECT_FALSE(reader.data().has(id));
  const auto gets = peer.calls_of(message::get_object);
  const auto again = reader.store().read(id, playback);
  ASSERT_NE(again.object, nullptr);
  EXPECT_EQ(again.object->bytes, bytes);
  EXPECT_EQ(again.source, ReadSource::cache);
  EXPECT_EQ(peer.calls_of(message::get_object), gets);
}

// test_store_control_objects_take_no_data_credit_and_commit_alone: the
// control store is not on the DATA device: finding a local CONTROL object
// never waits for DATA credit, here with every byte of background credit
// and every DATA slot of the device held, and asks no peer. (The namespace
// node store is Macha's FS.)
TEST(DistributedStore, ALocalControlObjectIsFoundWithoutAskingAnyone) {
  Cluster cluster;
  auto node_options = options(1);
  const uint64_t extent = node_options.store.extent_size;
  node_options.data_inflight_bytes = 2 * extent;
  node_options.foreground_reserve_bytes = extent;
  node_options.background_concurrency = 1;
  auto& node = cluster.add("solo", node_options);
  auto& peer = cluster.add("peer", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(18 * 1024, 91);
  const auto id = object_id(bytes);
  ASSERT_TRUE(node.control().put(id, bytes));
  auto held = node.credit()->acquire(DataWorkContext(FrameType::loader, extent), extent);
  ASSERT_TRUE(held.has_value());
  std::vector<IoReserve::Slot> slots;
  for (size_t i = 0; i < node.io()->data_share(); ++i) slots.push_back(std::move(*node.io()->try_data(DataWorkContext(FrameType::loader, extent))));
  auto found = std::async(std::launch::async, [&] { return node.store().ensure_control_local(id); });
  const bool prompt = found.wait_for(2s) == std::future_status::ready;
  // Lets a regressed build finish rather than hang the suite.
  held.reset();
  slots.clear();
  EXPECT_TRUE(prompt);
  EXPECT_TRUE(found.get());
  EXPECT_EQ(peer.calls_of(message::get_control_object), 0u);
  // A commit's CONTROL write and claim on this node alone, with DATA held
  // again: control work takes no DATA credit.
  held = node.credit()->acquire(DataWorkContext(FrameType::loader, extent), extent);
  ASSERT_TRUE(held.has_value());
  const auto credit_before = node.credit()->stats();
  EXPECT_TRUE(node.store().retain_control({id}, contract::RetentionDot{node.id(), 1}, DistributedStore::ClaimScope::here));
  EXPECT_TRUE(node.claims().retained(contract::RetentionClass::control, id));
  const auto credit_after = node.credit()->stats();
  EXPECT_EQ(credit_after.loader_admissions + credit_after.speculative_admissions + credit_after.foreground_admissions,
            credit_before.loader_admissions + credit_before.speculative_admissions + credit_before.foreground_admissions);
}

// ---- Moved from Macha's test_storage_v18.cpp --------------------------------

// test_a_full_preferred_node_spills_data_but_not_control (VALIDATION
// "Placement and the write floor", "Class separation"): an r=1 DATA put
// spills from a full preferred node to the next candidate and reads back
// from either; CONTROL objects ignore the DATA quota and reach the full
// node. (Macha's catalogue and its artwork are the FS's; their store
// operations are these.)
TEST(DistributedStore, AFullPreferredNodeSpillsDataButNotControl) {
  Cluster cluster;
  auto small_options = options(1, 1, 384ULL * 1024);
  small_options.store.metadata_write_copies = 2;
  auto large_options = options(1, 1, 16ULL * 1024 * 1024);
  large_options.store.metadata_write_copies = 2;
  auto& small = cluster.add("small", small_options);
  auto& large = cluster.add("large", large_options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  fill_data_store(small.data());
  const auto data = preferred_for(large, small.id(), 128 * 1024, 77);
  const auto id = object_id(data);
  ASSERT_TRUE(large.store().put(id, data));
  EXPECT_FALSE(small.data().has(id));
  EXPECT_TRUE(large.data().has(id));
  EXPECT_EQ(large.store().get(id), std::optional<Bytes>{data});
  // Read from the full node too.
  EXPECT_EQ(small.store().get(id), std::optional<Bytes>{data});

  // CONTROL is its own class: writable on the full node, and delivered to
  // it, never into either DATA store.
  const auto control_bytes = pattern(64 * 1024, 123);
  const auto control_id = object_id(control_bytes);
  ASSERT_TRUE(large.control().put(control_id, control_bytes));
  ASSERT_TRUE(large.store().retain_control({control_id}, contract::RetentionDot{large.id(), 1}));
  EXPECT_TRUE(small.control().has(control_id));
  EXPECT_FALSE(small.data().has(control_id));
  EXPECT_FALSE(large.data().has(control_id));
  EXPECT_TRUE(small.claims().retained(contract::RetentionClass::control, control_id));
}

// test_min_write_two_uses_fallback_when_preferred_replica_is_full
// (VALIDATION "Placement and the write floor"): write_copies 2 is met by a
// fallback when a preferred owner is full.
TEST(DistributedStore, WriteCopiesTwoUsesAFallbackWhenAPreferredOwnerIsFull) {
  Cluster cluster;
  constexpr uint64_t node_limit = 2ULL * 1024 * 1024;
  auto& a = cluster.add("a", options(2, 2, node_limit));
  auto& b = cluster.add("b", options(2, 2, node_limit));
  auto& c = cluster.add("c", options(2, 2, node_limit));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  fill_data_store(a.data());
  const auto data = preferred_for(b, a.id(), 128 * 1024);
  const auto id = object_id(data);
  WriteOptions write;
  write.level = Durability::replicated;
  const auto evidence = b.store().write(id, data, write);
  EXPECT_TRUE(evidence.met);
  EXPECT_FALSE(a.data().has(id));
  EXPECT_EQ(static_cast<unsigned>(b.data().has(id)) + static_cast<unsigned>(c.data().has(id)), 2u);
  EXPECT_EQ(evidence.on(a.id())->state, ReplicaState::refused);
}

// test_min_write_two_uses_fallback_when_preferred_replica_is_full as Macha wrote
// it: its put(), named no level, seeks write_copies, and a full preferred owner
// is passed for a fallback before it returns.
TEST(StoreDistributedStore, APutSeeksWriteCopiesThroughAFallback) {
  Cluster cluster;
  constexpr uint64_t node_limit = 2ULL * 1024 * 1024;
  auto& a = cluster.add("a", options(2, 2, node_limit));
  auto& b = cluster.add("b", options(2, 2, node_limit));
  auto& c = cluster.add("c", options(2, 2, node_limit));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  fill_data_store(a.data());
  const auto data = preferred_for(b, a.id(), 128 * 1024);
  const auto id = object_id(data);
  ASSERT_TRUE(b.store().put(id, data));
  EXPECT_FALSE(a.data().has(id));
  EXPECT_EQ(static_cast<unsigned>(b.data().has(id)) + static_cast<unsigned>(c.data().has(id)), 2u);
}

// test_a_control_graph_larger_than_the_connection_budget_still_publishes:
// claims pipeline their puts, but a connection holds only so many replies; a
// graph larger than that still publishes, a second claim sends nothing, and
// what the peer lost is sent again exactly.
TEST(DistributedStore, AControlGraphLargerThanTheConnectionBudgetStillPublishes) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 1));
  auto& b = cluster.add("b", options(2, 1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const size_t count = dannan::net::max_pending_replies * 2;
  std::vector<ObjectId> ids;
  for (size_t i = 0; i < count; ++i) {
    auto bytes = pattern(128, static_cast<uint8_t>(i % 251));
    bytes[0] = static_cast<uint8_t>(i & 0xff);
    bytes[1] = static_cast<uint8_t>((i >> 8) & 0xff);
    const auto id = object_id(bytes);
    ASSERT_TRUE(a.control().put(id, bytes));
    ids.push_back(id);
  }
  ASSERT_TRUE(a.store().retain_control(ids, contract::RetentionDot{a.id(), 1}));
  size_t on_peer = 0;
  for (const auto& id : ids) on_peer += b.control().has(id) ? 1 : 0;
  EXPECT_EQ(on_peer, ids.size());

  const auto puts_before = b.calls_of(message::put_control_object);
  ASSERT_GT(puts_before, 0u);
  ASSERT_TRUE(a.store().retain_control(ids, contract::RetentionDot{a.id(), 2}));
  const auto puts_after = b.calls_of(message::put_control_object);
  EXPECT_EQ(puts_after, puts_before);

  for (size_t i = 0; i < 10; ++i) ASSERT_TRUE(b.control().remove(ids[i]));
  ASSERT_TRUE(a.store().retain_control(ids, contract::RetentionDot{a.id(), 3}));
  EXPECT_EQ(b.calls_of(message::put_control_object), puts_after + 10);
  for (size_t i = 0; i < 10; ++i) EXPECT_TRUE(b.control().has(ids[i]));
}

// test_a_control_offer_sends_only_what_the_peer_lacks: an offer asks each
// node what it lacks and sends only that; a graph the peer holds costs one
// question and no bytes; what this node does not hold is not offered.
TEST(DistributedStore, AControlOfferSendsOnlyWhatThePeerLacks) {
  Cluster cluster;
  auto& a = cluster.add("a", options(1, 1, 8ULL * 1024 * 1024));
  auto& b = cluster.add("b", options(1, 1, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto held = pattern(64 * 1024, 31);
  const auto lacking = pattern(64 * 1024, 32);
  const auto held_id = object_id(held);
  const auto lacking_id = object_id(lacking);
  ASSERT_TRUE(a.control().put(held_id, held));
  ASSERT_TRUE(a.control().put(lacking_id, lacking));
  ASSERT_TRUE(b.control().put(held_id, held));

  const auto first = a.store().offer_control({held_id, lacking_id});
  EXPECT_EQ(first.held_by, 2u);
  EXPECT_EQ(first.objects_sent, 1u);
  EXPECT_EQ(first.bytes_sent, lacking.size());
  EXPECT_TRUE(b.control().valid(lacking_id));

  const auto again = a.store().offer_control({held_id, lacking_id});
  EXPECT_EQ(again.held_by, 2u);
  EXPECT_EQ(again.objects_sent, 0u);
  EXPECT_EQ(again.bytes_sent, 0u);

  const auto absent = a.store().offer_control({object_id(pattern(64, 33))});
  EXPECT_EQ(absent.held_by, 0u);
  EXPECT_EQ(absent.objects_sent, 0u);
}

// test_catalogue_control_objects_recover_on_metadata_replica, the store's
// half (VALIDATION "Class separation"): CONTROL objects one replica lost are
// fetched back from another, checked, and kept durably.
TEST(DistributedStore, ControlObjectsRecoverOntoAReplicaFromAnother) {
  Cluster cluster;
  auto& a = cluster.add("a", options(1, 1, 8ULL * 1024 * 1024));
  auto& b = cluster.add("b", options(1, 1, 8ULL * 1024 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  std::vector<ObjectId> referenced;
  for (uint8_t i = 0; i < 3; ++i) {
    const auto bytes = pattern(4096, static_cast<uint8_t>(60 + i));
    referenced.push_back(object_id(bytes));
    ASSERT_TRUE(b.control().put(referenced.back(), bytes));
  }
  ASSERT_TRUE(b.store().retain_control(referenced, contract::RetentionDot{b.id(), 1}));
  for (const auto& id : referenced) ASSERT_TRUE(a.control().get(id).has_value());
  for (const auto& id : referenced) ASSERT_TRUE(a.control().remove(id));
  for (const auto& id : referenced) EXPECT_FALSE(a.control().has(id));
  for (const auto& id : referenced) EXPECT_TRUE(a.store().ensure_control_local(id));
  for (const auto& id : referenced) EXPECT_TRUE(a.control().valid(id));
  EXPECT_EQ(b.calls_of(message::get_control_object), referenced.size());
}

// test_edge_node_never_owns_and_its_writes_land_on_owners: a node that hosts
// no extents is never an owner or fallback for any key, and its writes land
// on nodes that host extents; it reads them back like any non-owner.
TEST(DistributedStore, AnEdgeNodeNeverOwnsAndItsWritesLandOnOwners) {
  Cluster cluster;
  auto& s1 = cluster.add("s1", options(1));
  auto& s2 = cluster.add("s2", options(1));
  auto edge_options = options(1);
  edge_options.hosts_extents = false;
  auto& edge = cluster.add("edge", edge_options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  EXPECT_FALSE(edge.store().holds_extents());
  EXPECT_EQ(edge.data().limit(), 0u);

  size_t owned_by_s1 = 0;
  size_t owned_by_s2 = 0;
  for (unsigned i = 0; i < 512; ++i) {
    // Macha's pattern salt is a byte: the high bits make the 512 distinct.
    auto bytes = pattern(64, static_cast<uint8_t>(i));
    bytes[0] = static_cast<uint8_t>(i >> 8);
    const auto key = object_id(bytes);
    EXPECT_FALSE(edge.store().should_own(key));
    owned_by_s1 += s1.store().should_own(key) ? 1 : 0;
    owned_by_s2 += s2.store().should_own(key) ? 1 : 0;
  }
  EXPECT_GT(owned_by_s1, 0u);
  EXPECT_GT(owned_by_s2, 0u);
  EXPECT_EQ(owned_by_s1 + owned_by_s2, 512u);

  const auto data = pattern(256 * 1024, 7);
  WriteOptions write;
  write.frame = FrameType::loader;
  const auto id = edge.store().put(data, write);
  EXPECT_FALSE(edge.data().has(id));
  EXPECT_TRUE(s1.data().has(id) || s2.data().has(id));
  ReadOptions read;
  read.frame = FrameType::foreground;
  const auto back = edge.store().get(id, read);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, data);
  EXPECT_FALSE(edge.data().has(id));
}

// test_durability_barrier_across_peer_restarts: a deferred write's barrier
// re-derives a peer's place when the peer restarted before it (the peer
// answers from disk with a fresh ticket); a peer that is down is "ask
// again", never "put again"; and a peer that no longer holds the object
// after its restart is "put again". Here, once on disk a copy stays
// acknowledged: the wait settles it, and a later restart cannot undo it.
TEST(DistributedStore, ADeferredWritesBarrierAcrossPeerRestarts) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  WriteOptions deferred;
  deferred.deferred = true;
  deferred.level = Durability::replicated;

  // Ordinary: both copies awaiting the barrier, then on disk.
  const auto data = pattern(64 * 1024, 91);
  const auto first = a.store().write(object_id(data), data, deferred);
  ASSERT_EQ(first.holders(), 2u);
  EXPECT_FALSE(first.met);
  EXPECT_EQ(first.on(b.id())->why, "awaiting barrier");
  ASSERT_TRUE(b.data().has(object_id(data)));
  const auto settled = a.store().wait(first.ticket, 10s);
  ASSERT_TRUE(settled);
  EXPECT_TRUE(settled->met);
  EXPECT_TRUE(settled->settled);
  EXPECT_EQ(settled->acknowledged(), 2u);

  // The peer restarted before the barrier: re-derived and restamped.
  const auto more = pattern(64 * 1024, 92);
  const auto second = a.store().write(object_id(more), more, deferred);
  ASSERT_EQ(second.holders(), 2u);
  const auto old_epoch = b.context().identity().durability_epoch();
  b.restart();
  ASSERT_NE(b.context().identity().durability_epoch(), old_epoch);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(b.data().has(object_id(more)));
  std::optional<WriteEvidence> restamped;
  ASSERT_TRUE(wait_until(
      [&] {
        restamped = a.store().wait(second.ticket, 2s);
        return restamped && restamped->settled;
      },
      30s));
  EXPECT_TRUE(restamped->met);
  ASSERT_TRUE(restamped->on(b.id())->durability);
  EXPECT_EQ(restamped->on(b.id())->durability->epoch, b.context().identity().durability_epoch());

  // A peer that is down: transient, still pending, not refused.
  const auto third_bytes = pattern(64 * 1024, 93);
  const auto third = a.store().write(object_id(third_bytes), third_bytes, deferred);
  ASSERT_EQ(third.holders(), 2u);
  b.stop();
  const auto down = a.store().wait(third.ticket, 500ms);
  ASSERT_TRUE(down);
  EXPECT_FALSE(down->met);
  EXPECT_EQ(down->on(b.id())->state, ReplicaState::pending);

  // Back, but without the object: definitive, refused, put it again.
  b.start();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(b.data().remove(object_id(third_bytes)));
  std::optional<WriteEvidence> gone;
  ASSERT_TRUE(wait_until(
      [&] {
        gone = a.store().wait(third.ticket, 2s);
        return gone && gone->settled;
      },
      30s));
  EXPECT_FALSE(gone->met);
  EXPECT_EQ(gone->on(b.id())->state, ReplicaState::refused);
  EXPECT_EQ(gone->on(a.id())->state, ReplicaState::acknowledged);
}

// ---- The durability levels and tickets (new) --------------------------------

// Ephemeral: received and queued, returned at once while the copies are
// still being made; the ticket reports them as they land.
TEST(DistributedStore, EphemeralReturnsAtOnceAndItsTicketReportsTheCopies) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2, 2));
  auto& peer = cluster.add("peer", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  dannan::test::Gate held;
  dannan::test::GateOpener opener(held);
  peer.interpose(hold(message::put_object, held));
  const auto data = pattern(32 * 1024, 41);
  WriteOptions write;
  write.level = Durability::ephemeral;
  const auto evidence = writer.store().write(object_id(data), data, write);
  EXPECT_TRUE(evidence.met);
  EXPECT_FALSE(evidence.settled);
  EXPECT_EQ(evidence.acknowledged(), 0u);
  for (const auto& replica : evidence.replicas) EXPECT_EQ(replica.state, ReplicaState::pending);
  ASSERT_TRUE(held.wait_for_entries(1));
  held.open();
  const auto landed = writer.store().wait(evidence.ticket, 10s);
  ASSERT_TRUE(landed);
  EXPECT_TRUE(landed->settled);
  EXPECT_EQ(landed->acknowledged(), 2u);
  EXPECT_TRUE(peer.data().has(object_id(data)));
}

// Persisted: on disk on one node, returned without waiting for any other,
// even with write_copies 2; the rest is owed.
TEST(DistributedStore, PersistedReturnsOnTheFirstCopyOnDisk) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2, 2));
  auto& peer = cluster.add("peer", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  dannan::test::Gate held;
  dannan::test::GateOpener opener(held);
  peer.interpose(hold(message::put_object, held));
  const auto data = pattern(32 * 1024, 42);
  WriteOptions write;
  write.level = Durability::persisted;
  write.deadline = 10s;
  const auto started = std::chrono::steady_clock::now();
  const auto evidence = writer.store().write(object_id(data), data, write);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
  EXPECT_TRUE(evidence.met);
  EXPECT_EQ(evidence.sought, 1u);
  EXPECT_EQ(evidence.on(writer.id())->state, ReplicaState::acknowledged);
  EXPECT_EQ(evidence.on(peer.id())->state, ReplicaState::pending);
  EXPECT_TRUE(evidence.on(writer.id())->in_set);
  held.open();
}

// Replicated: on disk on k = write_copies nodes before it returns.
TEST(DistributedStore, ReplicatedWaitsForWriteCopies) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(3, 2));
  cluster.add("one", options(3, 2));
  cluster.add("two", options(3, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto data = pattern(32 * 1024, 43);
  WriteOptions write;
  write.level = Durability::replicated;
  const auto evidence = writer.store().write(object_id(data), data, write);
  EXPECT_TRUE(evidence.met);
  EXPECT_EQ(evidence.sought, 2u);
  EXPECT_GE(evidence.acknowledged(), 2u);
  size_t holding = 0;
  for (auto& node : cluster.nodes()) holding += node->data().has(object_id(data)) ? 1 : 0;
  EXPECT_GE(holding, 2u);
  // Every member of the replication set is named.
  EXPECT_EQ(std::count_if(evidence.replicas.begin(), evidence.replicas.end(), [](const ReplicaEvidence& r) { return r.in_set; }), 3);
}

// Durable: on disk on every live member of the replication set, named by
// node id, the ticket settled.
TEST(DistributedStore, DurableWritesEveryLiveMember) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(3));
  cluster.add("one", options(3));
  cluster.add("two", options(3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto data = pattern(32 * 1024, 44);
  WriteOptions write;
  write.level = Durability::durable;
  write.deadline = 10s;
  const auto evidence = writer.store().write(object_id(data), data, write);
  EXPECT_TRUE(evidence.met);
  EXPECT_TRUE(evidence.settled);
  EXPECT_EQ(evidence.acknowledged(), 3u);
  for (auto& node : cluster.nodes()) {
    ASSERT_NE(evidence.on(node->id()), nullptr);
    EXPECT_EQ(evidence.on(node->id())->state, ReplicaState::acknowledged);
    EXPECT_TRUE(node->data().has(object_id(data)));
  }
}

// Durable with a member Dannan already sees as dead: not waited on, named
// pending; when it is back, the ticket brings it the object and reports
// again.
TEST(DistributedStore, DurableSkipsADeadMemberAndCatchesItUp) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(3));
  cluster.add("one", options(3));
  auto& dead = cluster.add("dead", options(3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto dead_id = dead.id();
  dead.stop();
  ASSERT_TRUE(cluster.dead_to(writer, dead_id));

  const auto data = pattern(32 * 1024, 45);
  WriteOptions write;
  write.level = Durability::durable;
  write.deadline = 10s;
  const auto started = std::chrono::steady_clock::now();
  const auto evidence = writer.store().write(object_id(data), data, write);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
  EXPECT_TRUE(evidence.met);
  EXPECT_FALSE(evidence.settled);
  ASSERT_NE(evidence.on(dead_id), nullptr);
  EXPECT_TRUE(evidence.on(dead_id)->in_set);
  EXPECT_EQ(evidence.on(dead_id)->state, ReplicaState::pending);
  EXPECT_EQ(evidence.on(dead_id)->why, "dead");
  EXPECT_EQ(evidence.acknowledged(), 2u);

  dead.start();
  const auto caught_up = writer.store().wait(evidence.ticket, 20s);
  ASSERT_TRUE(caught_up);
  EXPECT_TRUE(caught_up->settled);
  EXPECT_EQ(caught_up->on(dead_id)->state, ReplicaState::acknowledged);
  EXPECT_EQ(caught_up->acknowledged(), 3u);
  EXPECT_TRUE(dead.data().has(object_id(data)));
}

// Durable with a live member that does not answer: waited on to the
// deadline, never longer; the evidence says pending, and the ticket reports
// once the member's copy lands.
TEST(DistributedStore, DurableWaitsForASilentLiveMemberToTheDeadline) {
  Cluster cluster;
  auto& writer = cluster.add("writer", options(3));
  cluster.add("one", options(3));
  auto& silent = cluster.add("silent", options(3));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  dannan::test::Gate held;
  dannan::test::GateOpener opener(held);
  silent.interpose(hold(message::put_object, held));

  const auto data = pattern(32 * 1024, 46);
  WriteOptions write;
  write.level = Durability::durable;
  write.deadline = 400ms;
  const auto started = std::chrono::steady_clock::now();
  const auto evidence = writer.store().write(object_id(data), data, write);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(elapsed, 400ms);
  EXPECT_LT(elapsed, 3s);
  EXPECT_FALSE(evidence.met);
  EXPECT_FALSE(evidence.settled);
  EXPECT_EQ(evidence.on(silent.id())->state, ReplicaState::pending);
  EXPECT_EQ(evidence.on(silent.id())->why, "no answer yet");
  EXPECT_EQ(evidence.acknowledged(), 2u);
  // A wait that ends before the member answers says the same, and may be
  // called again.
  const auto still = writer.store().wait(evidence.ticket, 100ms);
  ASSERT_TRUE(still);
  EXPECT_FALSE(still->settled);

  ASSERT_TRUE(held.wait_for_entries(1));
  held.open();
  const auto caught_up = writer.store().wait(evidence.ticket, 10s);
  ASSERT_TRUE(caught_up);
  EXPECT_TRUE(caught_up->settled);
  EXPECT_TRUE(caught_up->met);
  EXPECT_EQ(caught_up->on(silent.id())->state, ReplicaState::acknowledged);
  EXPECT_EQ(writer.store().status(evidence.ticket)->acknowledged(), 3u);
}

// 0.91.5: nothing can take the write. The id-returning forms throw
// StorageUnavailable; the bool form says false; the evidence says lost.
TEST(DistributedStore, NothingCanTakeTheWrite) {
  Cluster cluster;
  auto& small = cluster.add("small", options(1, 1, 64 * 1024));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto data = pattern(128 * 1024, 47);
  EXPECT_THROW(small.store().put(data), StorageUnavailable);
  EXPECT_THROW(small.store().put_deferred(data), StorageUnavailable);
  EXPECT_FALSE(small.store().put(object_id(data), data));
  const auto evidence = small.store().write(object_id(data), data, {});
  EXPECT_TRUE(evidence.lost);
  EXPECT_FALSE(evidence.met);
  EXPECT_EQ(evidence.on(small.id())->state, ReplicaState::refused);
  // Bytes that are not the id's are the caller's mistake.
  EXPECT_THROW(small.store().write(object_id(pattern(16)), data, {}), std::runtime_error);
}

// The executor rule: every completion is posted to the caller's executor,
// never run inline, even when the answer is known at once.
TEST(DistributedStore, CompletionsArePostedNeverRunInline) {
  Cluster cluster;
  auto& node = cluster.add("solo", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  boost::asio::io_context caller;
  const auto data = pattern(8 * 1024, 48);
  const auto id = object_id(data);

  std::optional<WriteEvidence> written;
  node.store().write(id, data, {}, caller.get_executor(), [&](WriteEvidence evidence) { written = std::move(evidence); });
  // Ephemeral is answered at once: still posted.
  std::optional<WriteEvidence> queued;
  WriteOptions ephemeral;
  ephemeral.level = Durability::ephemeral;
  node.store().write(object_id(pattern(8 * 1024, 49)), pattern(8 * 1024, 49), ephemeral, caller.get_executor(),
                     [&](WriteEvidence evidence) { queued = std::move(evidence); });
  EXPECT_FALSE(queued);
  ASSERT_TRUE(wait_until([&] {
    caller.restart();
    caller.poll();
    return written && queued;
  }));
  EXPECT_TRUE(written->met);
  EXPECT_TRUE(queued->met);

  // A settled ticket's wait, known before the call returns: posted.
  std::optional<WriteEvidence> waited;
  node.store().wait(written->ticket, 1s, caller.get_executor(), [&](WriteEvidence evidence) { waited = std::move(evidence); });
  EXPECT_FALSE(waited);
  caller.restart();
  caller.poll();
  ASSERT_TRUE(waited);
  EXPECT_TRUE(waited->settled);

  // A read, local and immediate: posted.
  std::optional<ReadResult> read;
  node.store().read(id, {}, caller.get_executor(), [&](ReadResult result) { read = std::move(result); });
  EXPECT_FALSE(read);
  ASSERT_TRUE(wait_until([&] {
    caller.restart();
    caller.poll();
    return read.has_value();
  }));
  ASSERT_NE(read->object, nullptr);
  EXPECT_EQ(read->source, ReadSource::local);
  EXPECT_EQ(read->served_by, node.id());

  // An unknown ticket: posted, and says so.
  std::optional<WriteEvidence> unknown;
  node.store().wait(Ticket{999999}, 1s, caller.get_executor(), [&](WriteEvidence evidence) { unknown = std::move(evidence); });
  EXPECT_FALSE(unknown);
  caller.restart();
  caller.poll();
  ASSERT_TRUE(unknown);
  EXPECT_FALSE(unknown->ticket.valid());
}

// get returns the bytes and which node served them; has() is the local
// ground truth; an object nobody holds is not found, not an error.
TEST(DistributedStore, AReadNamesTheNodeThatServedIt) {
  Cluster cluster;
  auto& reader = cluster.add("reader", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(16 * 1024, 50);
  const auto id = object_id(bytes);
  ASSERT_TRUE(peer.data().put(id, bytes));
  EXPECT_FALSE(reader.store().has(id));
  ReadOptions speculative;
  speculative.frame = FrameType::speculative;
  const auto remote = reader.store().read(id, speculative);
  ASSERT_NE(remote.object, nullptr);
  EXPECT_EQ(remote.source, ReadSource::remote);
  EXPECT_EQ(remote.served_by, peer.id());
  EXPECT_EQ(remote.answer, FetchAnswer::ok);
  // A speculative read keeps no copy here.
  reader.store().wait_local_copies_settled();
  EXPECT_FALSE(reader.store().has(id));
  EXPECT_GE(reader.store().replica_stats(peer.id()).successes, 1u);

  const auto nowhere = reader.store().read(object_id(pattern(16, 51)), speculative);
  EXPECT_EQ(nowhere.object, nullptr);
  EXPECT_EQ(nowhere.answer, FetchAnswer::not_found);

  const auto local = peer.store().read(id);
  EXPECT_EQ(local.source, ReadSource::local);
  EXPECT_EQ(local.served_by, peer.id());
}

// Macha's forms seek write_copies before they return (distributed_store.cpp:226,
// docs/durability.md:7): at Macha's default of 1 a peer that never answers does
// not hold them up and the rest is owed; at 2 they wait for it. put_deferred_here
// is one copy here either way.
TEST(StoreDistributedStore, MachasFormsSeekWriteCopies) {
  dannan::test::Gate gate;
  Cluster cluster;
  auto& writer = cluster.add("writer", options(2, 1));
  auto& peer = cluster.add("peer", options(2, 1));
  auto& careful = cluster.add("careful", options(3, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto held = [&gate](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == message::put_object || seen == message::put_object_deferred) gate.enter_and_wait();
    return std::nullopt;
  };
  peer.interpose(held);
  writer.interpose(held);

  const auto bytes = pattern(32 * 1024, 41);
  auto one = std::async(std::launch::async, [&] {
    const auto evidence = careful.store().put_deferred_here(object_id(bytes), bytes);
    return evidence;
  });
  const bool deferred_returned = one.wait_for(10s) == std::future_status::ready;
  const auto two_bytes = pattern(32 * 1024, 42);
  auto two = std::async(std::launch::async, [&] { return careful.store().put(object_id(two_bytes), two_bytes); });
  const bool waited = two.wait_for(300ms) == std::future_status::timeout;
  gate.open();
  EXPECT_TRUE(deferred_returned);
  EXPECT_TRUE(waited);
  EXPECT_TRUE(two.get());
  const auto evidence = one.get();
  EXPECT_TRUE(evidence.deferred);
  EXPECT_EQ(evidence.level, Durability::persisted);
  EXPECT_TRUE(careful.data().has(object_id(bytes)));
  const auto settled = careful.store().wait(evidence.ticket, 10s);
  ASSERT_TRUE(settled.has_value());
  EXPECT_GE(settled->holders(), 1u);

  // write_copies 1: returned on the one copy, whoever else is held.
  dannan::test::Gate second_gate;
  peer.interpose([&second_gate](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == message::put_object) second_gate.enter_and_wait();
    return std::nullopt;
  });
  writer.interpose({});
  careful.interpose({});
  const auto one_copy = pattern(32 * 1024, 43);
  auto quick = std::async(std::launch::async, [&] { return writer.store().put(object_id(one_copy), one_copy); });
  const bool returned = quick.wait_for(10s) == std::future_status::ready;
  second_gate.open();
  EXPECT_TRUE(returned);
  EXPECT_TRUE(quick.get());
  EXPECT_TRUE(writer.data().has(object_id(one_copy)));
}

// A large publication goes through the guard a part at a time: the first part
// done, a stripe it used is free while a later part waits for another.
TEST(StoreClaimLocks, ABatchHoldsOnlyPartOfTheGuardAtATime) {
  ClaimLocks locks;
  std::vector<ObjectId> ids;
  constexpr size_t part = 4096;
  for (uint32_t n = 0; n < 2 * part; ++n) ids.push_back(object_id(test::numbered(n)));
  const auto in_stripe = [&](size_t stripe) {
    return *std::find_if(ids.begin(), ids.end(), [&](const ObjectId& id) { return id.bytes[0] % ClaimLocks::stripes == stripe; });
  };
  const auto low = in_stripe(0);
  const auto high = in_stripe(ClaimLocks::stripes - 1);
  std::atomic<size_t> seen{0};
  std::atomic<size_t> parts{0};
  std::optional<ClaimLocks::Guard> held;
  held.emplace(locks.lock(std::span<const ObjectId>(&high, 1)));
  auto batch = std::async(std::launch::async, [&] {
    return locks.in_batches(
        ids,
        [&](std::span<const ObjectId> ids_in_part) {
          seen += ids_in_part.size();
          ++parts;
          return true;
        },
        part);
  });
  ASSERT_TRUE(wait_until([&] { return parts.load() == 1; }));
  auto through = std::async(std::launch::async, [&] { (void)locks.lock(std::span<const ObjectId>(&low, 1)); });
  const bool got_through = through.wait_for(10s) == std::future_status::ready;
  EXPECT_EQ(parts.load(), 1u);
  held.reset();
  EXPECT_TRUE(batch.get());
  through.get();
  EXPECT_TRUE(got_through);
  EXPECT_EQ(seen.load(), ids.size());
  EXPECT_EQ(parts.load(), 2u);
  // A step that says no stops it.
  size_t calls = 0;
  EXPECT_FALSE(locks.in_batches(ids, [&](std::span<const ObjectId>) { return ++calls == 0; }));
  EXPECT_EQ(calls, 1u);
}

// Macha's test_retain_data_batches_a_large_publication_within_bounded_time: one
// publication referencing thousands of extents is checked in batches, never a
// round trip an extent, and every copy ends up claimed.
TEST(StoreDistributedStore, RetainDataBatchesALargePublicationWithinBoundedTime) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  constexpr size_t extents = 3000;
  std::vector<ObjectId> ids;
  for (size_t i = 0; i < extents; ++i) {
    auto data = pattern(256, static_cast<uint8_t>(i));
    data[0] ^= static_cast<uint8_t>(i >> 8);
    const auto id = object_id(data);
    ASSERT_TRUE(a.data().put_deferred(id, data));
    ASSERT_TRUE(b.data().put_deferred(id, data));
    ids.push_back(id);
  }
  const contract::RetentionDot dot{a.id(), 1};
  const auto started = std::chrono::steady_clock::now();
  const auto unheld = a.store().retain_data(ids, dot);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_TRUE(unheld.empty());
  EXPECT_LT(elapsed, 10s);
  for (const auto& id : {ids.front(), ids.back()}) {
    EXPECT_TRUE(a.claims().retained(contract::RetentionClass::data, id));
    EXPECT_TRUE(b.claims().retained(contract::RetentionClass::data, id));
  }
}

// Macha's test_a_delete_marks_its_object_and_holds_no_stripe.
TEST(StoreClaimLocks, ADeleteMarksItsObjectAndHoldsNoStripe) {
  ClaimLocks locks;
  const auto id = object_id(pattern(64));
  {
    auto deletion = locks.deletion(id, [] { return false; });
    EXPECT_FALSE(deletion.claimed());
    // The stripe is free while the delete goes, or this would deadlock: a claim
    // path gets it and finds the mark.
    const auto guard = locks.lock(std::span<const ObjectId>(&id, 1));
    EXPECT_TRUE(locks.deleting(id));
  }
  EXPECT_FALSE(locks.deleting(id));
  // Moved, the mark lasts as long as the new owner.
  {
    auto first = locks.deletion(id, [] { return false; });
    auto second = std::move(first);
    EXPECT_TRUE(locks.deleting(id));
  }
  EXPECT_FALSE(locks.deleting(id));
  // A standing claim refuses the delete and marks nothing.
  const auto refused = locks.deletion(id, [] { return true; });
  EXPECT_TRUE(refused.claimed());
  EXPECT_FALSE(locks.deleting(id));
  // Two deletes at once: the first to finish does not end the second's mark.
  {
    auto first = locks.deletion(id, [] { return false; });
    {
      auto second = locks.deletion(id, [] { return false; });
    }
    EXPECT_TRUE(locks.deleting(id));
  }
  EXPECT_FALSE(locks.deleting(id));
}

// A claim, local or from a peer, finds a marked object not present.
TEST(StoreClaimLocks, AClaimFindsAMarkedObjectNotPresent) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(4096, 51);
  const auto id = object_id(bytes);
  ASSERT_TRUE(a.data().put(id, bytes));
  ASSERT_TRUE(b.data().put(id, bytes));
  const contract::RetentionDot dot{a.id(), 7};
  {
    const auto on_a = a.data().claim_locks().deletion(id, [] { return false; });
    const auto on_b = b.data().claim_locks().deletion(id, [] { return false; });
    (void)a.store().retain_data({id}, dot);
    EXPECT_FALSE(a.claims().retained(contract::RetentionClass::data, id));
    EXPECT_FALSE(b.claims().retained(contract::RetentionClass::data, id));
  }
  (void)a.store().retain_data({id}, dot);
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::data, id));
  EXPECT_TRUE(b.claims().retained(contract::RetentionClass::data, id));
}

// A node's CONTROL sweep driven by the case, on the node's own stores and claims.
struct ControlSweep {
  explicit ControlSweep(ClusterNode& swept, std::function<void(const ObjectId&)> before_removal = {}) : node(swept) {
    const auto key = std::make_shared<const domain::StorageKey>(node.node_local_key());
    data_seen = std::make_unique<ledger::UnreferencedSince>(node.state() / "sweep-data.bin", key, node.context().logger("sweep"));
    control_seen = std::make_unique<ledger::UnreferencedSince>(node.state() / "sweep-control.bin", key, node.context().logger("sweep"));
    ledger::CollectorOptions swept_options;
    swept_options.before_control_removal = std::move(before_removal);
    collector = std::make_unique<ledger::Collector>(node.ledger(), provider, node.data(), node.control(), *data_seen, *control_seen, swept_options,
                                                    ledger::CollectorServices{node.context().logger("sweep"), node.context().observations(), time});
    provider.epoch = contract::ControlEpoch{1, time.now()};
  }

  // First seen unreferenced in one epoch, so the next can take it.
  void see_unreferenced() {
    (void)step();
    provider.epoch = contract::ControlEpoch{2, time.now()};
  }
  size_t step() {
    size_t removed = 0;
    for (int i = 0; i < 4; ++i) removed += collector->control_gc_step(std::vector<ObjectId>{}, 0ms, 64, nullptr, 0);
    return removed;
  }

  ClusterNode& node;
  dannan::test::SteppedTime time;
  test::TestProvider provider;
  std::unique_ptr<ledger::UnreferencedSince> data_seen;
  std::unique_ptr<ledger::UnreferencedSince> control_seen;
  std::unique_ptr<ledger::Collector> collector;
};

dannan::net::CallOutcome ask_claim(ClusterNode& asker, ClusterNode& asked, contract::RetentionClass object_class, const ObjectId& id) {
  for (const auto& record : asker.context().nodes().active())
    if (record.id == asked.id())
      return asker.context().routes().call_and_wait(record, message::retain_objects,
                                                    message::encode_retain({object_class, {asker.id(), 9}, {id}, std::nullopt}), FrameType::loader, 10s);
  throw std::runtime_error("the asker does not see that node");
}

bool claimed(const dannan::net::CallOutcome& outcome) { return outcome.reply && outcome.reply->type == dannan::net::message::ok; }

// src/catalogue/catalogue.cpp at Macha 98b816d (:2650-2653) and the handler
// (src/cluster/storage_server.cpp :374-381): the sweep's check marks the object, every
// class. Held past the check, a peer's claim finds it not present, so no claim stands
// with no object.
TEST(StoreClaimLocks, APeersClaimOnAControlObjectAboutToGoIsRefused) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(4096, 52);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.control().put(id, bytes));
  ASSERT_TRUE(wait_until([&] { return b.control().older_than(id, 2ms); }));
  bool asked = false;
  ControlSweep sweep(b, [&](const ObjectId& going) {
    if (going != id) return;
    asked = true;
    const auto outcome = ask_claim(a, b, contract::RetentionClass::control, id);
    EXPECT_FALSE(claimed(outcome));
    ASSERT_TRUE(outcome.reply.has_value());
    const auto refusal = dannan::net::refusal_of(*outcome.reply);
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(refusal->status, 409);
  });
  sweep.see_unreferenced();
  EXPECT_EQ(sweep.step(), 1u);
  EXPECT_TRUE(asked);
  EXPECT_FALSE(b.control().has(id));
  EXPECT_FALSE(b.claims().retained(contract::RetentionClass::control, id));
}

// This node's own claim, at the same point (distributed_store.cpp :811-825).
TEST(StoreClaimLocks, ALocalClaimOnAControlObjectAboutToGoIsRefused) {
  Cluster cluster;
  auto& node = cluster.add("node", options(1));
  node.start();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(4096, 53);
  const auto id = object_id(bytes);
  ASSERT_TRUE(node.control().put(id, bytes));
  ASSERT_TRUE(wait_until([&] { return node.control().older_than(id, 2ms); }));
  bool asked = false;
  ControlSweep sweep(node, [&](const ObjectId& going) {
    if (going != id) return;
    asked = true;
    EXPECT_FALSE(node.store().retain_control({id}, contract::RetentionDot{node.id(), 9}, DistributedStore::ClaimScope::here));
  });
  sweep.see_unreferenced();
  (void)sweep.step();
  EXPECT_TRUE(asked);
  EXPECT_FALSE(node.claims().retained(contract::RetentionClass::control, id));
}

// The other side: a claim that holds the object's stripe keeps the sweep's check
// waiting, and the check then finds the claim. A peer's CONTROL claim takes the same
// stripe.
TEST(StoreClaimLocks, AControlClaimInProgressHoldsTheSweepsCheckBack) {
  Cluster cluster;
  auto& a = cluster.add("a", options(2, 2));
  auto& b = cluster.add("b", options(2, 2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto bytes = pattern(4096, 54);
  const auto id = object_id(bytes);
  ASSERT_TRUE(b.control().put(id, bytes));
  ASSERT_TRUE(wait_until([&] { return b.control().older_than(id, 2ms); }));
  ControlSweep sweep(b);
  sweep.see_unreferenced();
  std::future<size_t> swept;
  std::future<dannan::net::CallOutcome> peers;
  {
    const auto guard = b.data().claim_locks().lock(std::span<const ObjectId>(&id, 1));
    swept = std::async(std::launch::async, [&] { return sweep.step(); });
    peers = std::async(std::launch::async, [&] { return ask_claim(a, b, contract::RetentionClass::control, id); });
    // Neither gets past the stripe while it is held.
    EXPECT_EQ(swept.wait_for(200ms), std::future_status::timeout);
    EXPECT_EQ(peers.wait_for(200ms), std::future_status::timeout);
    b.claims().retain_batch(contract::RetentionClass::control, {id}, contract::RetentionDot{b.id(), 1});
  }
  EXPECT_EQ(swept.get(), 0u);
  EXPECT_TRUE(claimed(peers.get()));
  EXPECT_TRUE(b.control().has(id));
  EXPECT_TRUE(b.claims().retained(contract::RetentionClass::control, id));
}

// The handler at Macha 98b816d (src/cluster/storage_server.cpp :343-385) takes no
// admission, nor does the local claim (distributed_store.cpp :817). With the device
// reserve off, as on a Macha node, Dagda's has none to refuse: no slot is asked for
// (admit_control hands an empty one), and DATA credit plays no part, every byte of it
// taken here. Macha never has a scope either: a claim with no principal is the
// node's own, and is allowed for this node and for any node of the cluster.
TEST(StoreDistributedStore, AClaimIsNeverRefusedWithTheDeviceReserveOff) {
  auto reserve_off = options(2, 2);
  reserve_off.device_reserve = false;
  Cluster cluster;
  auto& a = cluster.add("a", reserve_off);
  auto& b = cluster.add("b", reserve_off);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_EQ(a.io(), nullptr);
  ASSERT_EQ(b.io(), nullptr);
  const auto full_a = a.credit()->try_acquire(DataWorkContext(FrameType::foreground, 0), a.credit()->stats().capacity_bytes);
  const auto full_b = b.credit()->try_acquire(DataWorkContext(FrameType::foreground, 0), b.credit()->stats().capacity_bytes);
  ASSERT_TRUE(full_a.has_value());
  ASSERT_TRUE(full_b.has_value());

  const auto data_bytes = pattern(4096, 55);
  const auto control_bytes = pattern(4096, 56);
  const auto data_id = object_id(data_bytes);
  const auto control_id = object_id(control_bytes);
  ASSERT_TRUE(b.data().put(data_id, data_bytes));
  ASSERT_TRUE(b.control().put(control_id, control_bytes));
  ASSERT_TRUE(a.control().put(control_id, control_bytes));

  EXPECT_TRUE(claimed(ask_claim(a, b, contract::RetentionClass::data, data_id)));
  EXPECT_TRUE(b.claims().retained(contract::RetentionClass::data, data_id));
  EXPECT_TRUE(claimed(ask_claim(a, b, contract::RetentionClass::control, control_id)));
  EXPECT_TRUE(b.claims().retained(contract::RetentionClass::control, control_id));

  EXPECT_TRUE(a.store().retain_control({control_id}, contract::RetentionDot{a.id(), 9}, DistributedStore::ClaimScope::here));
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::control, control_id));

  ASSERT_TRUE(a.data().put(data_id, data_bytes));
  EXPECT_TRUE(a.store().retain_data_here({data_id}, contract::RetentionDot{a.id(), 10}).empty());
  EXPECT_TRUE(a.claims().retained(contract::RetentionClass::data, data_id));
  EXPECT_EQ(a.credit()->stats().foreground_waits + a.credit()->stats().loader_waits + a.credit()->stats().speculative_waits, 0u);
  EXPECT_EQ(b.credit()->stats().foreground_waits + b.credit()->stats().loader_waits + b.credit()->stats().speculative_waits, 0u);
}

// Macha's forms over the cache: a write through, hydrating from a peer, and neither
// without a cache.
TEST(StoreDistributedStore, CacheLocalAndHydrateFillTheCache) {
  ClusterNodeOptions cached = options(1);
  cached.hosts_extents = false;
  cached.keeps_backend = true;
  Cluster cluster;
  auto& reader = [&]() -> ClusterNode& {
    cached.cache.path = cluster.dir() / "reader-cache";
    cached.cache.max_blocks = 16;
    return cluster.add("reader", cached);
  }();
  auto& holder = cluster.add("holder", options(1));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  EXPECT_TRUE(reader.store().hydration_available());
  EXPECT_FALSE(holder.store().hydration_available());

  const auto written = pattern(4096, 71);
  EXPECT_TRUE(reader.store().cache_local(object_id(written), written));
  EXPECT_TRUE(reader.store().locally_available(object_id(written)));
  EXPECT_FALSE(reader.store().held_here(object_id(written)));
  EXPECT_FALSE(holder.store().cache_local(object_id(written), written));

  const auto remote = pattern(4096, 72);
  ASSERT_TRUE(holder.data().put(object_id(remote), remote));
  EXPECT_FALSE(holder.store().hydrate(object_id(pattern(4096, 73))));
  EXPECT_TRUE(holder.store().hydrate(object_id(remote)));
  EXPECT_TRUE(reader.store().hydrate(object_id(remote)));
  EXPECT_TRUE(reader.cache().has(object_id(remote)));
  EXPECT_FALSE(reader.store().hydrate(object_id(pattern(4096, 74))));
}

// Claims what this node holds and hands back the rest, sorted and unique; one a
// delete has marked is not held.
TEST(StoreDistributedStore, RetainDataHereClaimsWhatIsHeldAndReturnsTheRest) {
  Cluster cluster;
  auto& node = cluster.add("node", options(1));
  cluster.start_all();
  const auto held = pattern(4096, 75);
  const auto marked = pattern(4096, 76);
  const auto absent = pattern(4096, 77);
  ASSERT_TRUE(node.data().put(object_id(held), held));
  ASSERT_TRUE(node.data().put(object_id(marked), marked));
  const contract::RetentionDot dot{node.id(), 3};
  const auto deletion = node.data().claim_locks().deletion(object_id(marked), [] { return false; });
  auto rest = node.store().retain_data_here({object_id(held), object_id(absent), object_id(absent), object_id(marked)}, dot);
  std::vector<ObjectId> expected{object_id(absent), object_id(marked)};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(rest, expected);
  EXPECT_TRUE(node.claims().retained(contract::RetentionClass::data, object_id(held)));
  EXPECT_FALSE(node.claims().retained(contract::RetentionClass::data, object_id(marked)));
  EXPECT_FALSE(node.claims().retained(contract::RetentionClass::data, object_id(absent)));
}

// What this node can reach changed: a peer gone, or a disk.
TEST(StoreDistributedStore, TheReachabilityEpochMovesWithMembershipAndDisks) {
  Cluster cluster;
  auto& node = cluster.add("node", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  const auto first = node.store().reachability_epoch();
  peer.stop();
  EXPECT_TRUE(wait_until([&] { return node.store().reachability_epoch() != first; }, 30s));
  const auto second = node.store().reachability_epoch();
  auto backends = node.dagda_node().options().pool.backends;
  backends.push_back({cluster.dir() / "another", 64ULL * 1024 * 1024, 0});
  node.dagda_node().reconfigure({backends, {}});
  EXPECT_NE(node.store().reachability_epoch(), second);
}

// A node stops being a placement candidate and starts again, as `auto` follows the
// dial-back, without a restart.
TEST(StoreDistributedStore, HostingExtentsChangesWhileRunning) {
  Cluster cluster;
  auto& node = cluster.add("node", options(2));
  auto& peer = cluster.add("peer", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  EXPECT_TRUE(node.store().holds_extents());
  node.server().set_hosts_extents(false);
  EXPECT_TRUE(wait_until([&] { return !node.store().holds_extents(); }));
  const auto hosts_on_peer = [&] {
    const auto hosting = peer.context().nodes().active();
    return std::any_of(hosting.begin(), hosting.end(), [&](const auto& record) { return record.id == node.id() && record.advert(extents_advert); });
  };
  EXPECT_TRUE(wait_until([&] { return !hosts_on_peer(); }));
  node.server().set_hosts_extents(true);
  EXPECT_TRUE(wait_until([&] { return node.store().holds_extents() && hosts_on_peer(); }));
}

// Macha's activity means object bytes this store moved. Dannan's clocks count every
// inbound request, so traffic that is not object work does not read as present here.
TEST(StoreDistributedStore, ActivityIsObjectWorkInMachasMeaning) {
  Cluster cluster;
  auto& node = cluster.add("node", options(1));
  cluster.start_all();
  auto& store = node.store();
  const auto before = store.idle_for(dannan::sched::WorkClass::foreground);
  node.context().activity().note(FrameType::foreground, 4096);
  EXPECT_GE(store.idle_for(dannan::sched::WorkClass::foreground), before);
  EXPECT_EQ(store.take_foreground_bytes(), 0u);
  const auto data = pattern(4096, 81);
  ASSERT_TRUE(store.put(object_id(data), data));
  ReadOptions read;
  read.frame = FrameType::foreground;
  EXPECT_TRUE(store.get(object_id(data), read).has_value());
  EXPECT_LT(store.idle_for(dannan::sched::WorkClass::foreground), 10s);
  store.foreground_activity(100);
  EXPECT_GT(store.take_foreground_bytes(), 0u);
  EXPECT_EQ(store.take_foreground_bytes(), 0u);
}

// Refuses every request of `type`, as a peer with no room would.
test::Interpose refuse(MessageType type) {
  return [type](MessageType seen, const dannan::membership::NodeRecord&, const dannan::net::Message&) -> std::optional<Handled> {
    if (seen == type) return Handled::failed(507, "storage_full", "storage limit reached");
    return std::nullopt;
  };
}

// S28: put_here and put_deferred_here (Macha's distributed_store.cpp:1016-1050
// at 98b816d). On a node holding extents the copy is here and the call returns,
// the second copy prompt replication's; on a node that holds none they are a
// put and a deferred put, the copy landing on an owner and nothing kept here.
TEST(StoreDistributedStore, PutHereIsLocalFirstAndAPutWhereNoExtentsAreHeld) {
  Cluster cluster;
  auto& s1 = cluster.add("s1", options(2));
  auto& s2 = cluster.add("s2", options(2));
  auto edge_options = options(2);
  edge_options.hosts_extents = false;
  auto& edge = cluster.add("edge", edge_options);
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());

  const auto here = pattern(64 * 1024, 101);
  ASSERT_TRUE(s1.store().put_here(object_id(here), here));
  EXPECT_TRUE(s1.data().has(object_id(here)));
  ASSERT_TRUE(wait_until([&] { return s1.store().prompt_replication_stats().copies == 1; }));
  EXPECT_TRUE(s2.data().has(object_id(here)));

  ASSERT_FALSE(edge.store().holds_extents());
  const auto put = pattern(64 * 1024, 102);
  ASSERT_TRUE(edge.store().put_here(object_id(put), put));
  EXPECT_FALSE(edge.data().has(object_id(put)));
  EXPECT_TRUE(s1.data().has(object_id(put)) || s2.data().has(object_id(put)));

  const auto deferred = pattern(64 * 1024, 103);
  const auto evidence = edge.store().put_deferred_here(object_id(deferred), deferred);
  EXPECT_GT(evidence.holders(), 0u);
  EXPECT_EQ(evidence.on(edge.id()), nullptr);
  EXPECT_FALSE(edge.data().has(object_id(deferred)));
  EXPECT_TRUE(s1.data().has(object_id(deferred)) || s2.data().has(object_id(deferred)));
}

// S26: a prompt copy refused is retried after 30, 60, 120 and 240 s on the
// node's clock, five attempts in all, then left to repair (Macha's
// distributed_store.cpp:1230 and :1264-1266 at 98b816d).
TEST(StoreDistributedStore, PromptReplicationRetriesAt30_60_120And240SecondsThenGivesUp) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto writer = options(2);
  writer.time = &time;
  auto& a = cluster.add("a", writer);
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  b.interpose(refuse(message::put_object));
  const auto failures = [&] { return a.store().prompt_replication_stats().failures; };

  const auto data = pattern(64 * 1024, 104);
  ASSERT_TRUE(a.store().put_here(object_id(data), data));
  ASSERT_TRUE(wait_until([&] { return failures() == 1; }));
  uint64_t seen = 1;
  for (const auto delay : {30s, 60s, 120s, 240s}) {
    time.advance(delay - 1s);
    // The loop looks at least once a second: two seconds is two looks.
    EXPECT_FALSE(wait_until([&] { return failures() > seen; }, 2s)) << "retried before " << delay.count() << " s";
    time.advance(1s);
    ASSERT_TRUE(wait_until([&] { return failures() == seen + 1; })) << "no retry at " << delay.count() << " s";
    ++seen;
  }
  EXPECT_EQ(a.store().prompt_replication_stats().dropped, 1u);
  time.advance(3600s);
  EXPECT_FALSE(wait_until([&] { return failures() > 5; }, 2s));
  EXPECT_EQ(a.store().prompt_replication_stats().copies, 0u);
}

// S26: at most 4096 refused copies wait for their retry; one more drops the
// soonest due (Macha's distributed_store.cpp:1318-1320). Time stands still, so
// none comes due and each object is tried once.
TEST(StoreDistributedStore, PromptReplicationKeepsAtMost4096Retries) {
  dannan::test::SteppedTime time;
  Cluster cluster;
  auto writer = options(2);
  writer.time = &time;
  auto& a = cluster.add("a", writer);
  auto& b = cluster.add("b", options(2));
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  b.interpose(refuse(message::put_object));
  constexpr unsigned objects = 4097;
  for (unsigned i = 0; i < objects; ++i) {
    auto bytes = pattern(64, static_cast<uint8_t>(i));
    bytes[0] = static_cast<uint8_t>(i >> 8);
    bytes[1] = 0xa5;
    ASSERT_TRUE(a.store().put_here(object_id(bytes), bytes));
  }
  ASSERT_TRUE(wait_until([&] { return a.store().prompt_replication_stats().failures == objects; }, 300s));
  const auto stats = a.store().prompt_replication_stats();
  EXPECT_EQ(stats.dropped, 1u);
  EXPECT_EQ(stats.queued, 0u);
  EXPECT_EQ(stats.copies, 0u);
}

}  // namespace
}  // namespace dagda::store
