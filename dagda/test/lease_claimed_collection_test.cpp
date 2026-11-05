//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The claimed-collection template (requirements E1 to E6) over a record of
// the tests' own: a task any node may add, whose operator intent and owner's
// progress are separate field groups, and which two live records must not
// share a subject. The template's laws, codec bounds and the two findings
// about its tombstone window and delta form.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "dagda/lease/claimed_collection.h"
#include "dagda/lease/lease.h"
#include "dagda/test/fixtures/lease.h"

namespace dagda::test {
namespace {

using namespace std::chrono_literals;
using dannan::net::Bytes;
using dannan::net::DecodeError;
using dannan::net::Reader;
using dannan::net::Writer;
using lease::Lease;
using lease::NodeId;

constexpr size_t max_task_text = 256;

struct Task {
  std::string id;
  std::string subject;
  uint64_t created_unix_ms{};

  uint64_t priority{};
  uint64_t priority_changed_unix_ms{};
  NodeId priority_changed_by{};

  bool cancelled{};
  uint64_t intent_changed_unix_ms{};
  NodeId intent_changed_by{};

  std::optional<Lease> lease;

  uint8_t phase{};
  uint64_t phase_epoch{};
  uint64_t progress_unix_ms{};

  uint64_t removed_unix_ms{};

  auto operator<=>(const Task&) const = default;
};

void write_node(Writer& writer, const NodeId& node) { writer.fixed(node.bytes); }
NodeId read_node(Reader& reader) {
  NodeId node;
  node.bytes = reader.fixed<16>();
  return node;
}

struct TaskTraits {
  using Record = Task;
  using Key = std::string;
  using DuplicateKey = std::string;

  static const std::string& key_of(const Task& task) { return task.id; }

  // Priority: last writer wins. Intent: the same, a cancel first. Progress:
  // the owner's, by (phase_epoch, phase, progress_unix_ms).
  static constexpr auto groups =
      std::make_tuple(lease::field_group([](const Task& t) { return lease::written(t.priority_changed_unix_ms, t.priority_changed_by); },
                                         [](Task& out, const Task& from) {
                                           out.priority = from.priority;
                                           out.priority_changed_unix_ms = from.priority_changed_unix_ms;
                                           out.priority_changed_by = from.priority_changed_by;
                                         }),
                      lease::field_group([](const Task& t) { return lease::dominating(t.cancelled, t.intent_changed_unix_ms, t.intent_changed_by); },
                                         [](Task& out, const Task& from) {
                                           out.cancelled = from.cancelled;
                                           out.intent_changed_unix_ms = from.intent_changed_unix_ms;
                                           out.intent_changed_by = from.intent_changed_by;
                                         }),
                      lease::field_group([](const Task& t) { return std::make_tuple(t.phase_epoch, t.phase, t.progress_unix_ms); },
                                         [](Task& out, const Task& from) {
                                           out.phase = from.phase;
                                           out.phase_epoch = from.phase_epoch;
                                           out.progress_unix_ms = from.progress_unix_ms;
                                         }));

  static const std::optional<Lease>& lease_of(const Task& task) { return task.lease; }
  static void set_lease(Task& task, std::optional<Lease> lease) { task.lease = lease; }
  static uint64_t removed_unix_ms(const Task& task) { return task.removed_unix_ms; }
  static void set_removed_unix_ms(Task& task, uint64_t ms) { task.removed_unix_ms = ms; }

  // Two live tasks for one subject: the earlier (created_unix_ms, id) keeps
  // it, the other is cancelled. The loser's stamp is its own, so the rule
  // does not depend on which task kept the subject.
  static std::optional<std::string> duplicate_key(const Task& task) {
    if (task.removed_unix_ms || task.cancelled || task.subject.empty()) return std::nullopt;
    return task.subject;
  }
  static bool keeps(const Task& x, const Task& y) { return std::tie(x.created_unix_ms, x.id) < std::tie(y.created_unix_ms, y.id); }
  static void lose(Task& loser, const Task&) {
    loser.cancelled = true;
    loser.intent_changed_unix_ms = std::max(loser.intent_changed_unix_ms, loser.created_unix_ms);
    loser.intent_changed_by = NodeId{};
  }

  static void encode(Writer& writer, const Task& t) {
    writer.string(t.id);
    writer.string(t.subject);
    writer.u64(t.created_unix_ms);
    writer.u64(t.priority);
    writer.u64(t.priority_changed_unix_ms);
    write_node(writer, t.priority_changed_by);
    writer.u8(t.cancelled ? 1 : 0);
    writer.u64(t.intent_changed_unix_ms);
    write_node(writer, t.intent_changed_by);
    lease::encode(writer, t.lease);
    writer.u8(t.phase);
    writer.u64(t.phase_epoch);
    writer.u64(t.progress_unix_ms);
    writer.u64(t.removed_unix_ms);
  }
  static Task decode(Reader& reader) {
    Task t;
    t.id = reader.string(max_task_text);
    t.subject = reader.string(max_task_text);
    t.created_unix_ms = reader.u64();
    t.priority = reader.u64();
    t.priority_changed_unix_ms = reader.u64();
    t.priority_changed_by = read_node(reader);
    const auto cancelled = reader.u8();
    if (cancelled > 1) throw DecodeError("bad task intent");
    t.cancelled = cancelled;
    t.intent_changed_unix_ms = reader.u64();
    t.intent_changed_by = read_node(reader);
    t.lease = lease::decode_optional_lease(reader);
    t.phase = reader.u8();
    t.phase_epoch = reader.u64();
    t.progress_unix_ms = reader.u64();
    t.removed_unix_ms = reader.u64();
    return t;
  }
  static void encode_key(Writer& writer, const std::string& id) { writer.string(id); }
  static std::string decode_key(Reader& reader) { return reader.string(max_task_text); }

  static constexpr uint32_t max_records = 64;
  static constexpr std::chrono::milliseconds retention = 24h;
};

using Tasks = lease::ClaimedCollection<TaskTraits>;
using TaskMap = Tasks::Map;

const NodeId a_node = node_of(0x11);
const NodeId b_node = node_of(0x22);
const NodeId c_node = node_of(0x33);

Task task(const std::string& id, const std::string& subject, uint64_t created) {
  Task t;
  t.id = id;
  t.subject = subject;
  t.created_unix_ms = created;
  return t;
}

Bytes encoded(const Tasks& collection) {
  Writer writer;
  collection.encode(writer);
  return writer.take();
}

Bytes encoded(const Tasks::Delta& delta) {
  Writer writer;
  Tasks::encode(writer, delta);
  return writer.take();
}

Tasks decoded(const Bytes& bytes) {
  Reader reader(bytes);
  auto out = Tasks::decode(reader);
  reader.finish();
  return out;
}

Tasks::Delta decoded_delta(const Bytes& bytes) {
  Reader reader(bytes);
  auto out = Tasks::decode_delta(reader);
  reader.finish();
  return out;
}

Tasks collection_of(std::vector<Task> tasks) {
  TaskMap map;
  for (auto& t : tasks) map.emplace(t.id, std::move(t));
  return Tasks(std::move(map));
}

// Each group is taken whole from its later write; a cancel beats a later
// resume; the lease is the lease order's winner; the tombstone the greater.
TEST(LeaseClaimedCollection, RecordsJoinGroupByGroup) {
  auto left = task("t1", "s", 100);
  left.priority = 5;
  left.priority_changed_unix_ms = 300;
  left.priority_changed_by = a_node;
  left.cancelled = true;
  left.intent_changed_unix_ms = 200;
  left.intent_changed_by = a_node;
  left.lease = Lease{a_node, 1, 150};
  left.phase = 1;
  left.phase_epoch = 1;
  left.progress_unix_ms = 160;

  auto right = task("t1", "s", 100);
  right.priority = 7;
  right.priority_changed_unix_ms = 250;
  right.priority_changed_by = b_node;
  right.intent_changed_unix_ms = 400;  // a later resume
  right.intent_changed_by = b_node;
  right.lease = Lease{b_node, 2, 350};
  right.phase = 1;
  right.phase_epoch = 2;
  right.progress_unix_ms = 360;
  right.removed_unix_ms = 500;

  const auto merged = Tasks::merge(left, right);
  EXPECT_EQ(merged, Tasks::merge(right, left));
  EXPECT_EQ(merged.priority, 5u);
  EXPECT_EQ(merged.priority_changed_by, a_node);
  EXPECT_TRUE(merged.cancelled);
  EXPECT_EQ(merged.intent_changed_unix_ms, 200u);
  ASSERT_TRUE(merged.lease.has_value());
  EXPECT_EQ(merged.lease->holder, b_node);
  EXPECT_EQ(merged.lease->epoch, 2u);
  EXPECT_EQ(merged.phase_epoch, 2u);
  EXPECT_EQ(merged.progress_unix_ms, 360u);
  EXPECT_EQ(merged.removed_unix_ms, 500u);

  // join() is the merge onto what the collection holds.
  auto head = collection_of({left});
  EXPECT_EQ(head.join(right), merged);
  EXPECT_EQ(head.size(), 1u);
  head.join(task("t2", "other", 120));
  EXPECT_EQ(head.size(), 2u);
}

// The merge laws over many versions of one record, each group's clock naming
// one write: commutative, associative, idempotent.
TEST(LeaseClaimedCollection, TheRecordMergeIsAJoin) {
  std::mt19937_64 random(5);
  const std::vector<NodeId> nodes{a_node, b_node, c_node};
  const auto pick = [&](size_t n) { return static_cast<size_t>(random() % n); };
  std::vector<Task> versions;
  for (int i = 0; i < 40; ++i) {
    auto t = task("t1", "s", 100);
    const auto priority_ms = pick(4) * 100;
    const auto& priority_by = nodes[pick(3)];
    t.priority_changed_unix_ms = priority_ms;
    t.priority_changed_by = priority_by;
    if (priority_ms) t.priority = priority_ms + priority_by.bytes[0];
    const auto intent_ms = pick(4) * 100;
    const auto& intent_by = nodes[pick(3)];
    t.intent_changed_unix_ms = intent_ms;
    t.intent_changed_by = intent_by;
    if (intent_ms) t.cancelled = (intent_ms / 100 + intent_by.bytes[0]) % 2;
    if (pick(3)) t.lease = Lease{nodes[pick(3)], 1 + pick(3), 100 * pick(4)};
    t.phase_epoch = t.lease ? t.lease->epoch : 0;
    t.phase = static_cast<uint8_t>(t.phase_epoch ? 1 + pick(3) : 0);
    t.progress_unix_ms = 100 * pick(3);
    if (!pick(4)) t.removed_unix_ms = 1000 * (1 + pick(2));
    versions.push_back(t);
  }
  for (const auto& x : versions) {
    EXPECT_EQ(Tasks::merge(x, x), x);
    for (const auto& y : versions) {
      const auto xy = Tasks::merge(x, y);
      EXPECT_EQ(xy, Tasks::merge(y, x));
      for (const auto& z : versions) ASSERT_EQ(Tasks::merge(xy, z), Tasks::merge(x, Tasks::merge(y, z)));
    }
  }
}

// The collection merge, with and without a common base: commutative,
// idempotent and associative over branches of the base that change, add,
// tombstone and sweep tasks, concurrent adds of one subject included.
TEST(LeaseClaimedCollection, TheCollectionMergeIsAJoinWithAndWithoutABase) {
  std::mt19937_64 random(7);
  const auto pick = [&](size_t n) { return static_cast<size_t>(random() % n); };
  const std::vector<NodeId> nodes{a_node, b_node, c_node};
  TaskMap base;
  for (char c : std::string("abcdef")) {
    auto t = task(std::string(1, c), std::string(1, c), 100 + static_cast<uint64_t>(c));
    if (c == 'e') t.removed_unix_ms = 500;
    base.emplace(t.id, t);
  }
  const auto branch = [&](int salt) {
    auto side = base;
    for (auto it = side.begin(); it != side.end();) {
      const auto roll = pick(5);
      if (roll == 0 && it->second.removed_unix_ms) {
        it = side.erase(it);  // swept
        continue;
      }
      if (roll == 1) {
        it->second.cancelled = true;
        it->second.intent_changed_unix_ms = 1000 + static_cast<uint64_t>(salt);
        it->second.intent_changed_by = nodes[static_cast<size_t>(salt) % 3];
      } else if (roll == 2) {
        const auto epoch = 1 + pick(2);
        it->second.lease = Lease{nodes[static_cast<size_t>(salt) % 3], epoch, 2000 + static_cast<uint64_t>(salt)};
        it->second.phase = 1;
        it->second.phase_epoch = epoch;
        it->second.progress_unix_ms = 2000 + static_cast<uint64_t>(salt);
      } else if (roll == 3) {
        it->second.removed_unix_ms = 3000 + static_cast<uint64_t>(salt);
      }
      ++it;
    }
    if (pick(2)) {
      const char subject = pick(2) ? 'e' : 'y';
      auto t = task("n" + std::to_string(salt), std::string(1, subject), 4000 + static_cast<uint64_t>(salt));
      side.emplace(t.id, t);
    }
    return side;
  };
  for (int round = 0; round < 200; ++round) {
    const auto a = branch(3 * round), b = branch(3 * round + 1), c = branch(3 * round + 2);
    for (const TaskMap* with : {static_cast<const TaskMap*>(&base), static_cast<const TaskMap*>(nullptr)}) {
      const auto ab = Tasks::merge(with, a, b);
      ASSERT_EQ(ab, Tasks::merge(with, b, a)) << round;
      ASSERT_EQ(Tasks::merge(with, ab, ab), ab) << round;
      ASSERT_EQ(Tasks::merge(with, ab, c), Tasks::merge(with, a, Tasks::merge(with, b, c))) << round;
    }
  }
}

// With a base, an erasure on one side stands unless the other side changed
// the record since; without one, a record on one side is kept.
TEST(LeaseClaimedCollection, AnErasureStandsUnlessTheOtherSideChangedTheRecord) {
  auto t = task("t1", "s", 100);
  t.removed_unix_ms = 200;
  const TaskMap base{{t.id, t}};
  const TaskMap swept;
  EXPECT_TRUE(Tasks::merge(&base, swept, base).empty());
  EXPECT_TRUE(Tasks::merge(&base, base, swept).empty());
  EXPECT_EQ(Tasks::merge(nullptr, swept, base), base);

  auto changed = t;
  changed.priority = 3;
  changed.priority_changed_unix_ms = 300;
  changed.priority_changed_by = b_node;
  const TaskMap kept{{t.id, changed}};
  EXPECT_EQ(Tasks::merge(&base, swept, kept), kept);
}

// Two live tasks for one subject: the earlier keeps it, the other is
// cancelled; a cancelled or removed task holds no subject.
TEST(LeaseClaimedCollection, TheDuplicateRuleKeepsTheEarlierTask) {
  const auto first = task("t1", "s", 100);
  const auto second = task("t2", "s", 200);
  const TaskMap left{{first.id, first}};
  const TaskMap right{{second.id, second}};
  const auto merged = Tasks::merge(nullptr, left, right);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_FALSE(merged.at("t1").cancelled);
  EXPECT_TRUE(merged.at("t2").cancelled);
  EXPECT_EQ(merged.at("t2").intent_changed_unix_ms, 200u);
  EXPECT_EQ(merged, Tasks::merge(nullptr, right, left));

  // Three concurrent adds settle in one merge whatever its order.
  const TaskMap third{{"t3", task("t3", "s", 300)}};
  EXPECT_EQ(Tasks::merge(nullptr, Tasks::merge(nullptr, left, right), third), Tasks::merge(nullptr, left, Tasks::merge(nullptr, right, third)));

  auto removed = first;
  removed.removed_unix_ms = 150;
  const auto after_removal = Tasks::merge(nullptr, TaskMap{{removed.id, removed}}, right);
  EXPECT_FALSE(after_removal.at("t2").cancelled);
}

// The snapshot and delta forms round-trip; a delta applied to its before is
// its after.
TEST(LeaseClaimedCollection, TheSnapshotAndDeltaFormsRoundTrip) {
  auto t1 = task("t1", "s1", 100);
  t1.lease = Lease{a_node, 1, 110};
  const auto t2 = task("t2", "s2", 200);
  const auto before = collection_of({t1, t2});
  EXPECT_EQ(decoded(encoded(before)), before);
  EXPECT_EQ(decoded(encoded(Tasks{})), Tasks{});

  auto after = before;
  auto changed = t1;
  changed.removed_unix_ms = 300;
  after.join(changed);
  after.erase("t2");
  after.join(task("t3", "s3", 400));
  const auto delta = Tasks::diff(before, after);
  EXPECT_EQ(delta.upserts.size(), 2u);
  EXPECT_EQ(delta.erasures, std::vector<std::string>{"t2"});
  const auto carried = decoded_delta(encoded(delta));
  EXPECT_EQ(carried, delta);
  auto applied = before;
  applied.apply(carried);
  EXPECT_EQ(applied, after);
  EXPECT_TRUE(Tasks::diff(after, after).empty());
}

// The template's bytes, pinned on the neutral record: what Macha's own record
// takes through the same template, whose known answers are Macha's port gate
// (the maintainer, 2026-11-04 night). Any change to the sections shows here.
TEST(LeaseClaimedCollection, TheSectionsBytesArePinned) {
  auto t1 = task("t1", "s1", 100);
  t1.lease = Lease{a_node, 1, 110};
  const auto before = collection_of({t1, task("t2", "s2", 200)});
  EXPECT_EQ(dannan::identity::hex(encoded(before)),
            "00000002000000027431000000027331000000000000006400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
            "000000000000000001111111111111111111111111111111110000000000000001000000000000006e0000000000000000000000000000000000000000000000000000000002743200"
            "000002733200000000000000c8000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000");
  auto after = before;
  auto changed = t1;
  changed.removed_unix_ms = 300;
  after.join(changed);
  after.erase("t2");
  EXPECT_EQ(
      dannan::identity::hex(encoded(Tasks::diff(before, after))),
      "00000001000000027431000000027331000000000000006400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
      "000000000001111111111111111111111111111111110000000000000001000000000000006e0000000000000000000000000000000000000000000000012c00000001000000027432");
}

// The template's bounds, in the snapshot and the delta.
TEST(LeaseClaimedCollection, DecodeKeepsItsBounds) {
  const auto t = task("t1", "s", 100);
  Writer many;
  many.u32(TaskTraits::max_records + 1);
  EXPECT_THROW(decoded(many.data()), DecodeError);
  EXPECT_THROW(decoded_delta(many.data()), DecodeError);
  Writer erasures;
  erasures.u32(0);
  erasures.u32(TaskTraits::max_records + 1);
  EXPECT_THROW(decoded_delta(erasures.data()), DecodeError);

  // A record with no key, or two under one, in the snapshot.
  Writer unkeyed;
  unkeyed.u32(1);
  TaskTraits::encode(unkeyed, task("", "s", 100));
  EXPECT_THROW(decoded(unkeyed.data()), DecodeError);
  Writer twice;
  twice.u32(2);
  TaskTraits::encode(twice, t);
  TaskTraits::encode(twice, t);
  EXPECT_THROW(decoded(twice.data()), DecodeError);
  // In a delta: written twice, or written and erased.
  EXPECT_THROW(decoded_delta(twice.data()), DecodeError);
  Writer both;
  both.u32(1);
  TaskTraits::encode(both, t);
  both.u32(1);
  both.string(t.id);
  EXPECT_THROW(decoded_delta(both.data()), DecodeError);

  // A record keyed by another key is refused, held or encoded.
  const TaskMap mis_keyed{{"another", t}};
  EXPECT_THROW(Tasks{mis_keyed}, std::invalid_argument);
  Tasks::Delta mis_keyed_delta;
  mis_keyed_delta.upserts.emplace("another", t);
  EXPECT_THROW(encoded(mis_keyed_delta), std::invalid_argument);
  // More records than the bound are refused at encode.
  TaskMap full;
  for (uint32_t i = 0; i <= TaskTraits::max_records; ++i) full.emplace("t" + std::to_string(i), task("t" + std::to_string(i), "", 100));
  EXPECT_THROW(encoded(Tasks(full)), std::invalid_argument);
}

// Requirement E3: a tombstone is kept its retention, and expires strictly
// past it.
TEST(LeaseClaimedCollection, ATombstoneExpiresStrictlyPastItsRetention) {
  auto removed = task("t1", "s", 100);
  removed.removed_unix_ms = 1'000;
  const auto head = collection_of({removed, task("t2", "s2", 100)});
  const uint64_t window = std::chrono::milliseconds(TaskTraits::retention).count();
  EXPECT_TRUE(head.expired(1'000 + window).empty());
  EXPECT_EQ(head.expired(1'000 + window + 1), std::vector<std::string>{"t1"});
}

// Finding (Macha, its tombstone sweep, by reading): the window was computed
// as `now - removed > retention` on unsigned milliseconds, so a tombstone
// stamped by a node whose clock is ahead of the sweeper's wrapped round and
// expired at once, letting a branch that missed the removal bring the record
// back. Decided (the maintainer, 2026-11-04): a future stamp is fresh; Macha the same
// at the port.
TEST(LeaseClaimedCollection, ATombstoneFromAClockAheadIsKeptItsWindow) {
  auto removed = task("t1", "s", 100);
  removed.removed_unix_ms = 2'000'000;
  const auto head = collection_of({removed});
  EXPECT_TRUE(head.expired(1'000'000).empty());
  EXPECT_TRUE(head.expired(2'000'000).empty());
  const uint64_t window = std::chrono::milliseconds(TaskTraits::retention).count();
  EXPECT_EQ(head.expired(2'000'000 + window + 1), std::vector<std::string>{"t1"});
}

// Finding (Macha, by reading: its snapshot refuses a record with no key, its
// delta accepts one): a delta could install a record the snapshot decoder
// refuses, so a head that applied it encoded a snapshot it could not read
// back. Decided (the maintainer, 2026-11-04): the delta refuses it too; Macha's DLT9
// the same at the port.
TEST(LeaseClaimedCollection, ADeltaCannotInstallARecordTheSnapshotRefuses) {
  Writer delta;
  delta.u32(1);
  TaskTraits::encode(delta, task("", "s", 100));
  delta.u32(0);
  EXPECT_THROW(decoded_delta(delta.data()), DecodeError);
}

// So a head that took every delta the decoder passed always reads its own
// snapshot back: the unkeyed record never reaches it.
TEST(LeaseClaimedCollection, AnUnkeyedRecordFromADeltaNeverReachesTheSnapshot) {
  Writer delta;
  delta.u32(1);
  TaskTraits::encode(delta, task("", "s", 100));
  delta.u32(0);
  Tasks head;
  head.join(task("t1", "s", 100));
  try {
    head.apply(decoded_delta(delta.data()));
  } catch (const DecodeError&) {
  }
  EXPECT_EQ(head.size(), 1u);
  EXPECT_EQ(decoded(encoded(head)), head);
}

}  // namespace
}  // namespace dagda::test
