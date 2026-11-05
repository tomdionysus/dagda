//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement S29: pulling a CONTROL object from a peer. From Macha's
// tests/test_root_parts.cpp (808fa0f), on real Dannan nodes: a peer Macha's fake
// answered by hand is a node whose requests the case interposes on, and a peer
// Macha's fake refused to place is a node that has stopped, its record still
// active (a long dead_after keeps it so).

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "dagda/store/control_objects.h"
#include "dagda/store/messages.h"
#include "dagda/test/fixtures/cluster.h"
#include "dagda/test/fixtures/store.h"

namespace dagda::store {
namespace {

using namespace std::chrono_literals;
using dannan::net::Handled;
using dannan::net::Message;
using dannan::net::MessageType;
using dannan::sched::FrameType;
using test::Cluster;
using test::ClusterNode;
using test::ClusterNodeOptions;
using test::pattern;

// Dagda's reply types carry no name to log: a reply of another type is logged
// by number where Macha's logged the name.
const auto reply_type_ok = "reply type " + std::to_string(static_cast<uint16_t>(dannan::net::message::ok));

// The one warning a failed pull logs, or empty.
std::string unavailable_line(ClusterNode& node) {
  for (const auto& line : node.log().lines())
    if (line.find("control object unavailable") != std::string::npos) return line;
  return {};
}

Handled control_reply(const ObjectId& id, std::span<const uint8_t> bytes) {
  return Handled::processed(Message{message::control_object_reply, message::encode_object(id, bytes), {}});
}

// Answers every get_control_object with `answer`.
test::Interpose answering(std::function<Handled()> answer) {
  return [answer = std::move(answer)](MessageType type, const dannan::membership::NodeRecord&, const Message&) -> std::optional<Handled> {
    if (type == message::get_control_object) return answer();
    return std::nullopt;
  };
}

// Macha: test_control_fetch_stores_only_the_object_it_asked_for. A peer's answer
// is stored only when it is the object asked for: the right reply, naming the
// id, with bytes that hash to it. Each other answer leaves the store without
// the object and is named as the failure. Asked once, at speculative priority.
TEST(StoreControlFetch, StoresOnlyTheObjectItAskedFor) {
  const auto bytes = pattern(4096, 7);
  const auto id = object_id(bytes);
  const auto other = pattern(4096, 8);

  {
    Cluster cluster;
    auto& alone = cluster.add("alone");
    alone.start();
    ControlObjectFetch fetch(alone.context(), alone.control());
    EXPECT_FALSE(fetch.pull(id, {}));
    EXPECT_NE(unavailable_line(alone).find("peers_asked=0 last_failure=no active peer"), std::string::npos) << unavailable_line(alone);
  }

  struct Case {
    const char* what;
    std::function<Handled()> answer;
    std::string failure;
  };
  const std::vector<Case> cases{
      {"another reply type", [] { return Handled::processed(); }, ": " + reply_type_ok},
      {"another object's id", [&] { return control_reply(object_id(other), other); }, ": integrity failure"},
      {"bytes that do not hash to the id", [&] { return control_reply(id, other); }, ": integrity failure"},
      {"a truncated reply", [&] { return Handled::processed(Message{message::control_object_reply, Bytes(id.bytes.begin(), id.bytes.end()), {}}); }, ": "},
  };
  for (const auto& one : cases) {
    SCOPED_TRACE(one.what);
    Cluster cluster;
    auto& asker = cluster.add("asker");
    auto& peer = cluster.add("peer");
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    peer.interpose(answering(one.answer));
    ControlObjectFetch fetch(asker.context(), asker.control());
    size_t observed = 0;
    EXPECT_FALSE(fetch.pull(id, [&](uint64_t, std::chrono::steady_clock::duration) { ++observed; }));
    EXPECT_EQ(observed, 0u);
    EXPECT_FALSE(asker.control().valid(id));
    const auto line = unavailable_line(asker);
    EXPECT_NE(line.find("peers_asked=1 last_failure=127.0.0.1" + one.failure), std::string::npos) << line;
    EXPECT_EQ(peer.calls_of(message::get_control_object), 1u);
    EXPECT_EQ(peer.frames_of(message::get_control_object), std::set<FrameType>{FrameType::speculative});
  }
}

// Macha: test_control_fetch_passes_over_a_failing_peer. A peer that cannot be
// reached, and one that refuses, are passed over for one that answers; the
// object is then local, the observer is told its size once, and a second pull
// asks nobody. The same through the interface a replay holds, with no observer.
TEST(StoreControlFetch, PassesOverAFailingPeer) {
  const auto bytes = pattern(8192, 9);
  const auto id = object_id(bytes);

  {
    Cluster cluster(10min);
    auto& asker = cluster.add("asker");
    auto& dead = cluster.add("dead");
    cluster.start_all();
    ASSERT_TRUE(cluster.converged());
    dead.stop();
    ControlObjectFetch fetch(asker.context(), asker.control());
    ControlObjectSource& source = fetch;
    EXPECT_FALSE(source.ensure_control_local(id));
    const auto line = unavailable_line(asker);
    EXPECT_NE(line.find("peers_asked=1 last_failure=127.0.0.1: "), std::string::npos) << line;
  }

  Cluster cluster(10min);
  auto& asker = cluster.add("asker");
  auto& dead = cluster.add("dead");
  auto& refusing = cluster.add("refusing");
  auto& holder = cluster.add("holder");
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  // Peers are asked in roster order; here the holder is asked last, whatever
  // that order is, because only it answers and the others must be passed.
  const auto second = pattern(4096, 11);
  ASSERT_TRUE(holder.control().put(id, bytes));
  ASSERT_TRUE(holder.control().put(object_id(second), second));
  refusing.interpose(answering([] { return Handled::failed(404, "not_found", "control object not found"); }));
  dead.stop();

  ControlObjectFetch fetch(asker.context(), asker.control());
  ControlObjectSource& source = fetch;
  std::vector<uint64_t> observed;
  ASSERT_TRUE(fetch.pull(id, [&](uint64_t size, std::chrono::steady_clock::duration) { observed.push_back(size); }));
  EXPECT_EQ(observed, std::vector<uint64_t>{bytes.size()});
  EXPECT_TRUE(asker.control().valid(id));
  EXPECT_EQ(asker.control().get(id), std::optional<Bytes>{bytes});
  EXPECT_EQ(holder.calls_of(message::get_control_object), 1u);
  EXPECT_LE(refusing.calls_of(message::get_control_object), 1u);

  const auto refused = refusing.calls_of(message::get_control_object);
  const auto held = holder.calls_of(message::get_control_object);
  EXPECT_TRUE(source.ensure_control_local(id));
  EXPECT_EQ(refusing.calls_of(message::get_control_object), refused);
  EXPECT_EQ(holder.calls_of(message::get_control_object), held);

  EXPECT_TRUE(source.ensure_control_local(object_id(second)));
  EXPECT_EQ(asker.control().get(object_id(second)), std::optional<Bytes>{second});
}

// The order Macha's case pinned: a failing peer ahead of the holder is asked,
// once, and passed over.
TEST(StoreControlFetch, AFailingPeerAheadOfTheHolderIsAskedOnceAndPassed) {
  const auto bytes = pattern(8192, 12);
  const auto id = object_id(bytes);
  Cluster cluster;
  auto& asker = cluster.add("asker");
  auto& one = cluster.add("one");
  auto& two = cluster.add("two");
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  std::vector<dannan::identity::NodeId> order;
  for (const auto& record : asker.context().nodes().active())
    if (record.id != asker.id()) order.push_back(record.id);
  ASSERT_EQ(order.size(), 2u);
  auto& failing = order[0] == one.id() ? one : two;
  auto& holder = order[0] == one.id() ? two : one;
  ASSERT_TRUE(holder.control().put(id, bytes));
  failing.interpose(answering([] { return Handled::failed(503, "busy", "try later"); }));

  ControlObjectFetch fetch(asker.context(), asker.control());
  EXPECT_TRUE(fetch.pull(id, {}));
  EXPECT_EQ(failing.calls_of(message::get_control_object), 1u);
  EXPECT_EQ(holder.calls_of(message::get_control_object), 1u);
}

// Macha: test_control_fetch_fails_when_the_store_cannot_take_the_object. A good
// answer the control store cannot take is a failure, not a success. It arrived,
// so the transfer is observed; it is not local.
TEST(StoreControlFetch, FailsWhenTheStoreCannotTakeTheObject) {
  const auto bytes = pattern(256 * 1024, 10);
  const auto id = object_id(bytes);
  Cluster cluster;
  ClusterNodeOptions small;
  small.control.limit = 64 * 1024;
  auto& asker = cluster.add("asker", small);
  auto& holder = cluster.add("holder");
  cluster.start_all();
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(holder.control().put(id, bytes));

  ControlObjectFetch fetch(asker.context(), asker.control());
  size_t observed = 0;
  EXPECT_FALSE(fetch.pull(id, [&](uint64_t, std::chrono::steady_clock::duration) { ++observed; }));
  EXPECT_EQ(observed, 1u);
  EXPECT_FALSE(asker.control().valid(id));
  const auto line = unavailable_line(asker);
  EXPECT_NE(line.find("last_failure=local control store put failed"), std::string::npos) << line;
}

}  // namespace
}  // namespace dagda::store
