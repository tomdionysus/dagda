//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Macha's TorrentRequest as the first instantiation of the claimed-collection
// and lease templates (requirements 5.4, step 8b): identical bytes and
// answers. The bytes are pinned by known answers computed outside both
// codebases (Python's struct and hashlib, from the layout of
// torrent_request.cpp:89-120 and the collection's sections at
// metadata.cpp:856-876 and :1499-1507, Macha develop 5a06d3c), the merge
// results written out by hand from the rules. The cases moved from Macha
// (develop 5a06d3c) keep every assertion that needs neither a head nor the
// FS; those that need a head wait for step 4 and are named where they were.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "dagda/lease/keeper.h"
#include "dagda/test/fixtures/lease.h"
#include "dagda/test/fixtures/torrent_request.h"
#include "dannan/identity/crypto.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dagda::test::macha {
namespace {

using namespace std::chrono_literals;
using dannan::identity::hex;
using dannan::net::Bytes;
using dannan::net::DecodeError;
using dannan::net::Reader;
using dannan::net::Writer;

Bytes unhex(const std::string& text) { return dannan::identity::unhex(text).value(); }

TorrentRequest sample_torrent_request(const std::string& id, const std::string& hash, uint64_t created) {
  TorrentRequest r;
  r.id = id;
  r.info_hash = hash;
  r.source = "magnet:?xt=urn:btih:" + hash;
  r.created_unix_ms = created;
  r.created_by = NodeId{};
  r.name = "Sample";
  return r;
}

Bytes encoded(const TorrentRequest& request) {
  Writer writer;
  encode_torrent_request(writer, request);
  return writer.take();
}

Bytes encoded(const TorrentRequests& collection) {
  Writer writer;
  collection.encode(writer);
  return writer.take();
}

Bytes encoded(const TorrentRequests::Delta& delta) {
  Writer writer;
  TorrentRequests::encode(writer, delta);
  return writer.take();
}

TorrentRequests decoded(const Bytes& bytes) {
  Reader reader(bytes);
  auto out = TorrentRequests::decode(reader);
  reader.finish();
  return out;
}

TorrentRequests::Delta decoded_delta(const Bytes& bytes) {
  Reader reader(bytes);
  auto out = TorrentRequests::decode_delta(reader);
  reader.finish();
  return out;
}

TorrentRequests collection_of(std::vector<TorrentRequest> requests) {
  TorrentRequestMap map;
  for (auto& request : requests) map.emplace(request.id, std::move(request));
  return TorrentRequests(std::move(map));
}

// Moved from test_storage_metadata.cpp, test_torrent_requests_join_without_conflicts
// (Macha develop 5a06d3c, :4568): every assertion.
TEST(LeaseTorrentRequest, RequestsJoinWithoutConflicts) {
  // Metadata writes are not compare-and-swap and history branches, so two
  // replicas can change one request at once. Every field has a rule and the
  // merge is a join: the same answer from either side, and again.
  const auto a_node = NodeId::random();
  const auto b_node = NodeId::random();
  auto base = sample_torrent_request(std::string(32, 'a'), std::string(40, '1'), 100);

  {
    auto full = base;
    full.pinned_node_id = a_node;
    full.remove_after_ms = 0;
    full.claim = TorrentClaim{b_node, 2, 500};
    full.phase = TorrentPhase::importing;
    full.phase_epoch = 2;
    full.ingest_job_id = "ingest";
    full.error_code = "code";
    full.removed_unix_ms = 9;
    const auto bytes = encoded(full);
    Reader reader(bytes);
    EXPECT_EQ(decode_torrent_request(reader), full);
    reader.finish();
  }

  auto paused = base;
  paused.desired = TorrentDesired::paused;
  paused.desired_changed_unix_ms = 200;
  paused.desired_changed_by = a_node;
  auto cancelled = base;
  cancelled.desired = TorrentDesired::cancelled;
  cancelled.desired_changed_unix_ms = 150;  // earlier, and still final
  cancelled.desired_changed_by = b_node;
  auto claimed = base;
  claimed.claim = TorrentClaim{a_node, 1, 300};
  claimed.phase = TorrentPhase::downloading;
  claimed.phase_epoch = 1;
  claimed.progress_unix_ms = 300;
  auto rival = base;
  rival.claim = TorrentClaim{b_node, 1, 290};  // same epoch, earlier: wins
  auto takeover = base;
  takeover.claim = TorrentClaim{b_node, 2, 900};
  takeover.phase = TorrentPhase::downloading;
  takeover.phase_epoch = 2;
  takeover.progress_unix_ms = 900;
  auto removed = base;
  removed.removed_unix_ms = 1000;

  const std::vector<TorrentRequest> variants{base, paused, cancelled, claimed, rival, takeover, removed};
  for (const auto& x : variants) {
    EXPECT_EQ(merge_torrent_request(x, x), x);
    for (const auto& y : variants) {
      EXPECT_EQ(merge_torrent_request(x, y), merge_torrent_request(y, x));
      for (const auto& z : variants) EXPECT_EQ(merge_torrent_request(merge_torrent_request(x, y), z), merge_torrent_request(x, merge_torrent_request(y, z)));
    }
  }
  EXPECT_EQ(merge_torrent_request(paused, cancelled).desired, TorrentDesired::cancelled);
  EXPECT_EQ(merge_torrent_request(claimed, rival).claim->holder, b_node);
  EXPECT_EQ(merge_torrent_request(claimed, takeover).claim->epoch, 2u);
  EXPECT_EQ(merge_torrent_request(claimed, takeover).phase_epoch, 2u);
  EXPECT_EQ(merge_torrent_request(claimed, removed).removed_unix_ms, 1000u);

  // Collections: a tombstone erased on one side stays erased unless the
  // other changed it; two live requests for one torrent keep the earlier.
  const TorrentRequestMap m_base{{base.id, removed}};
  const TorrentRequestMap erased{};
  const TorrentRequestMap untouched{{base.id, removed}};
  EXPECT_TRUE(merge_torrent_requests(m_base, erased, untouched).empty());
  auto touched = removed;
  touched.desired = TorrentDesired::cancelled;
  touched.desired_changed_unix_ms = 2000;
  EXPECT_EQ(merge_torrent_requests(m_base, erased, {{base.id, touched}}).size(), 1u);

  const auto first = sample_torrent_request(std::string(32, 'b'), std::string(40, '2'), 100);
  const auto second = sample_torrent_request(std::string(32, 'c'), std::string(40, '2'), 101);
  const auto both = merge_torrent_requests({}, {{first.id, first}}, {{second.id, second}});
  ASSERT_EQ(both.size(), 2u);
  EXPECT_EQ(both.at(first.id).desired, TorrentDesired::active);
  EXPECT_EQ(both.at(second.id).desired, TorrentDesired::cancelled);
  EXPECT_EQ(both.at(second.id).error_code, "duplicate_torrent");
  EXPECT_EQ(merge_torrent_requests({}, {{second.id, second}}, {{first.id, first}}), both);
}

// Moved from test_storage_metadata.cpp, test_torrent_requests_ride_the_snapshot_and_its_deltas
// (:4764): the collection's own section and delta, kept. Waiting for step 4,
// which carries the section inside a head: the magics (DHTMETB5, DHTMETB6,
// DHTMETB4, DHTMDLT9, and "not DLT9" for a quiet delta), the whole snapshot's
// bytes round trip and its return to the pre-request encoding, and the head
// merge's conflicts_created == 0.
TEST(LeaseTorrentRequest, RequestsRideTheSnapshotSectionAndItsDeltas) {
  const TorrentRequests base;
  const auto base_bytes = encoded(base);
  EXPECT_EQ(hex(base_bytes), "00000000");

  auto with_map = base.records();
  auto request = sample_torrent_request(std::string(32, 'd'), std::string(40, '3'), 100);
  request.claim = TorrentClaim{NodeId::random(), 1, 150};
  with_map[request.id] = request;
  const TorrentRequests with(with_map);
  const auto with_bytes = encoded(with);
  EXPECT_EQ(decoded(with_bytes).records(), with.records());
  EXPECT_EQ(encoded(decoded(with_bytes)), with_bytes);

  const auto added = TorrentRequests::diff(base, with);
  EXPECT_EQ(added.upserts.size(), 1u);
  auto applied = base;
  applied.apply(decoded_delta(encoded(added)));
  EXPECT_EQ(encoded(applied), with_bytes);

  auto changed_map = with.records();
  changed_map[request.id].phase = TorrentPhase::downloading;
  changed_map[request.id].phase_epoch = 1;
  const TorrentRequests changed(changed_map);
  const auto update = TorrentRequests::diff(with, changed);
  applied = with;
  applied.apply(decoded_delta(encoded(update)));
  EXPECT_EQ(encoded(applied), encoded(changed));

  // Erased again: the section returns to its pre-request encoding.
  const auto erased = TorrentRequests::diff(changed, base);
  EXPECT_EQ(erased.erasures, std::vector<std::string>{request.id});
  applied = changed;
  applied.apply(decoded_delta(encoded(erased)));
  EXPECT_EQ(encoded(applied), base_bytes);

  // A change that does not touch the collection is no change to it.
  EXPECT_TRUE(TorrentRequests::diff(with, with).empty());

  // Reconciliation joins the collection (Macha's head merge passes no base).
  auto left = with.records();
  left[request.id].desired = TorrentDesired::paused;
  left[request.id].desired_changed_unix_ms = 300;
  auto right = with.records();
  right[request.id].phase = TorrentPhase::downloading;
  right[request.id].phase_epoch = 1;
  right[request.id].progress_unix_ms = 400;
  const auto merged = merge_torrent_requests({}, left, right);
  const auto& joined = merged.at(request.id);
  EXPECT_EQ(joined.desired, TorrentDesired::paused);
  EXPECT_EQ(joined.phase, TorrentPhase::downloading);
}

const NodeId a_node = node_of(0x11);
const NodeId b_node = node_of(0x22);

// The record Macha's own round trip writes, byte for byte.
TEST(LeaseTorrentRequest, ARecordIsMachasBytes) {
  auto full = sample_torrent_request(std::string(32, 'a'), std::string(40, '1'), 100);
  full.pinned_node_id = a_node;
  full.remove_after_ms = 0;
  full.claim = TorrentClaim{b_node, 2, 500};
  full.phase = TorrentPhase::importing;
  full.phase_epoch = 2;
  full.ingest_job_id = "ingest";
  full.error_code = "code";
  full.removed_unix_ms = 9;
  const std::string expected =
      "01000000206161616161616161616161616161616161616161616161616161616161616161000000283131313131313131313131313131313131313131313131313131313131"
      "31313131313131313131310000003c6d61676e65743a3f78743d75726e3a627469683a3131313131313131313131313131313131313131313131313131313131313131313131"
      "31313131310000000000000064000000000000000000000000000000000111111111111111111111111111111111010000000000000000000000000000000000000000000000"
      "000000000000000000000000000000000000000000000000000000000000000000000122222222222222222222222222222222000000000000000200000000000001f4020000"
      "00000000000200000000000000000000000653616d706c65000000000000000000000006696e6765737400000004636f64650000000000000000000000000000000000000009";
  EXPECT_EQ(hex(encoded(full)), expected);
  const auto bytes = unhex(expected);
  Reader reader(bytes);
  EXPECT_EQ(decode_torrent_request(reader), full);
  reader.finish();
}

// Macha's claim is the lease, byte for byte: holder, epoch, since.
TEST(LeaseTorrentRequest, TheClaimIsTheLeasesBytes) {
  Writer writer;
  lease::encode(writer, Lease{a_node, 2, 500});
  EXPECT_EQ(hex(writer.data()), "11111111111111111111111111111111000000000000000200000000000001f4");
}

// Two sides of a partition: a later pause against a cancel, a takeover, a
// tombstone against a newer pause, and two adds of one torrent.
struct Sides {
  TorrentRequests left;
  TorrentRequests right;
};

Sides sides() {
  const auto i1 = std::string(32, '1'), i2 = std::string(32, '2'), i3 = std::string(32, '3'), i4 = std::string(32, '4'), i5 = std::string(32, '5');
  auto l1 = sample_torrent_request(i1, std::string(40, 'a'), 100);
  l1.desired = TorrentDesired::paused;
  l1.desired_changed_unix_ms = 200;
  l1.desired_changed_by = a_node;
  auto r1 = sample_torrent_request(i1, std::string(40, 'a'), 100);
  r1.desired = TorrentDesired::cancelled;
  r1.desired_changed_unix_ms = 150;
  r1.desired_changed_by = b_node;

  auto l2 = sample_torrent_request(i2, std::string(40, 'b'), 100);
  l2.claim = TorrentClaim{a_node, 1, 300};
  l2.phase = TorrentPhase::downloading;
  l2.phase_epoch = 1;
  l2.progress_unix_ms = 300;
  auto r2 = sample_torrent_request(i2, std::string(40, 'b'), 100);
  r2.claim = TorrentClaim{b_node, 2, 900};
  r2.phase = TorrentPhase::downloading;
  r2.phase_epoch = 2;
  r2.progress_unix_ms = 900;

  auto l3 = sample_torrent_request(i3, std::string(40, 'c'), 100);
  l3.removed_unix_ms = 1000;
  auto r3 = sample_torrent_request(i3, std::string(40, 'c'), 100);
  r3.desired = TorrentDesired::paused;
  r3.desired_changed_unix_ms = 500;
  r3.desired_changed_by = b_node;

  const auto added = [](const std::string& id, uint64_t created, const NodeId& by) {
    auto r = sample_torrent_request(id, std::string(40, 'd'), created);
    r.created_by = r.settings_changed_by = r.desired_changed_by = by;
    r.settings_changed_unix_ms = r.desired_changed_unix_ms = r.progress_unix_ms = created;
    return r;
  };
  return {collection_of({l1, l2, l3, added(i4, 100, a_node)}), collection_of({r1, r2, r3, added(i5, 101, b_node)})};
}

// The merged set, as SM16, SM17 and SM19 carry it: cancel dominates, the
// takeover's claim stands, the tombstone is the greater, and the later add of
// one torrent lands as duplicate_torrent, the earlier (created_unix_ms, id)
// keeping it.
TEST(LeaseTorrentRequest, TheMergedSetIsMachasBytes) {
  const auto [left, right] = sides();
  const auto merged = TorrentRequests::merge(nullptr, left, right);
  ASSERT_EQ(merged.size(), 5u);
  EXPECT_EQ(merged.find(std::string(32, '1'))->desired, TorrentDesired::cancelled);
  EXPECT_EQ(merged.find(std::string(32, '2'))->claim, (TorrentClaim{b_node, 2, 900}));
  EXPECT_EQ(merged.find(std::string(32, '3'))->removed_unix_ms, 1000u);
  EXPECT_EQ(merged.find(std::string(32, '4'))->desired, TorrentDesired::active);
  const auto& loser = *merged.find(std::string(32, '5'));
  EXPECT_EQ(loser.desired, TorrentDesired::cancelled);
  EXPECT_EQ(loser.desired_changed_unix_ms, 101u);
  EXPECT_EQ(loser.desired_changed_by, NodeId{});
  EXPECT_EQ(loser.error_code, "duplicate_torrent");
  EXPECT_EQ(loser.error, "request " + std::string(32, '4') + " holds the same torrent");

  const auto bytes = encoded(merged);
  EXPECT_EQ(bytes.size(), 1536u);
  EXPECT_EQ(hex(dannan::identity::sha256(bytes)), "0572e11809ef6a401d4a2021161f17d6bd52acb73da948f45ea1e28bbc8a0af4");
  EXPECT_EQ(decoded(bytes), merged);
  // From either side, and from the merged set again.
  EXPECT_EQ(TorrentRequests::merge(nullptr, right, left), merged);
  EXPECT_EQ(TorrentRequests::merge(nullptr, merged, merged), merged);
}

// DLT9's section: from the left side to the merged set, then the sweep.
TEST(LeaseTorrentRequest, TheDeltasAreMachasBytes) {
  const auto [left, right] = sides();
  const auto merged = TorrentRequests::merge(nullptr, left, right);
  const auto delta = TorrentRequests::diff(left, merged);
  EXPECT_EQ(delta.upserts.size(), 4u);
  EXPECT_TRUE(delta.erasures.empty());
  const auto bytes = encoded(delta);
  EXPECT_EQ(bytes.size(), 1256u);
  EXPECT_EQ(hex(dannan::identity::sha256(bytes)), "588a624f26c4456695a999940b925ccdd36a98f7cf7ca4bb46668ab3fd7a48a6");
  auto applied = left;
  applied.apply(decoded_delta(bytes));
  EXPECT_EQ(applied, merged);

  auto swept = merged;
  swept.erase(std::string(32, '3'));
  const auto sweep = encoded(TorrentRequests::diff(merged, swept));
  EXPECT_EQ(hex(sweep), "0000000000000001000000203333333333333333333333333333333333333333333333333333333333333333");
  applied = merged;
  applied.apply(decoded_delta(sweep));
  EXPECT_EQ(applied, swept);
}

// Macha's bounds (torrent_request.hpp:114-115, torrent_request.cpp:122-161,
// metadata.cpp:866-876 and :1731-1750), each refused as Macha refuses it.
TEST(LeaseTorrentRequest, DecodeKeepsMachasBounds) {
  const auto request = sample_torrent_request(std::string(32, 'a'), std::string(40, '1'), 100);
  const auto record = encoded(request);
  const auto refused = [](Bytes bytes) {
    Reader reader(bytes);
    (void)decode_torrent_request(reader);
  };
  // Offsets into the record: the format byte, the pin flag, the
  // remove_after flag, desired, the claim flag, the phase.
  const size_t pin = 1 + 4 + 32 + 4 + 40 + 4 + 60 + 8 + 16;
  const size_t remove_after = pin + 1;
  const size_t desired = remove_after + 1 + 8 + 16;
  const size_t claim = desired + 1 + 8 + 16;
  const size_t phase = claim + 1;
  ASSERT_EQ(record[pin], 0);
  ASSERT_EQ(record[phase], 0);
  for (const auto& [at, value] : std::vector<std::pair<size_t, uint8_t>>{{0, 2}, {pin, 2}, {remove_after, 2}, {desired, 3}, {claim, 2}, {phase, 6}}) {
    auto bad = record;
    bad[at] = value;
    EXPECT_THROW(refused(bad), DecodeError) << at;
  }
  // Text past 8,192 bytes.
  auto long_name = request;
  long_name.name = std::string(max_torrent_request_text + 1, 'x');
  EXPECT_THROW(refused(encoded(long_name)), DecodeError);
  long_name.name.pop_back();
  EXPECT_NO_THROW(refused(encoded(long_name)));

  // More than 65,536 records, in the snapshot and in a delta.
  Writer many;
  many.u32(max_torrent_requests + 1);
  EXPECT_THROW(decoded(many.data()), DecodeError);
  EXPECT_THROW(decoded_delta(many.data()), DecodeError);
  Writer erasures;
  erasures.u32(0);
  erasures.u32(max_torrent_requests + 1);
  EXPECT_THROW(decoded_delta(erasures.data()), DecodeError);

  // A record with no id, or two under one, in the snapshot.
  Writer unkeyed;
  unkeyed.u32(1);
  encode_torrent_request(unkeyed, sample_torrent_request("", std::string(40, '1'), 100));
  EXPECT_THROW(decoded(unkeyed.data()), DecodeError);
  Writer twice;
  twice.u32(2);
  encode_torrent_request(twice, request);
  encode_torrent_request(twice, request);
  EXPECT_THROW(decoded(twice.data()), DecodeError);
  // In a delta: written twice, or written and erased.
  EXPECT_THROW(decoded_delta(twice.data()), DecodeError);
  Writer both;
  both.u32(1);
  encode_torrent_request(both, request);
  both.u32(1);
  both.string(request.id);
  EXPECT_THROW(decoded_delta(both.data()), DecodeError);

  // A record keyed by another id is refused at encode.
  TorrentRequestMap mis_keyed{{"another", request}};
  EXPECT_THROW(TorrentRequests{mis_keyed}, std::invalid_argument);
  TorrentRequests::Delta mis_keyed_delta;
  mis_keyed_delta.upserts.emplace("another", request);
  EXPECT_THROW(encoded(mis_keyed_delta), std::invalid_argument);
}

// The merge laws over many versions of one request: each group's clock names
// one write, as every writer stamps its own (changed_ms, node) or its
// (phase_epoch, phase, progress_ms). Commutative, associative, idempotent.
TEST(LeaseTorrentRequest, TheRecordMergeIsAJoin) {
  std::mt19937_64 random(5);
  const std::vector<NodeId> nodes{a_node, b_node, node_of(0x33)};
  const auto pick = [&](size_t n) { return static_cast<size_t>(random() % n); };
  std::vector<TorrentRequest> versions;
  for (int i = 0; i < 40; ++i) {
    auto r = sample_torrent_request(std::string(32, 'a'), std::string(40, '1'), 100);
    // Settings: written at ms by node, the values a function of the write.
    const auto settings_ms = pick(4) * 100;
    const auto& settings_by = nodes[pick(3)];
    r.settings_changed_unix_ms = settings_ms;
    r.settings_changed_by = settings_by;
    if (settings_ms) r.remove_after_ms = settings_ms + settings_by.bytes[0];
    const auto desired_ms = pick(4) * 100;
    const auto& desired_by = nodes[pick(3)];
    r.desired_changed_unix_ms = desired_ms;
    r.desired_changed_by = desired_by;
    if (desired_ms) r.desired = static_cast<TorrentDesired>((desired_ms / 100 + desired_by.bytes[0]) % 3);
    if (pick(3)) r.claim = TorrentClaim{nodes[pick(3)], 1 + pick(3), 100 * pick(4)};
    r.phase_epoch = r.claim ? r.claim->epoch : 0;
    r.phase = static_cast<TorrentPhase>(r.phase_epoch ? 1 + pick(4) : 0);
    r.progress_unix_ms = 100 * pick(3);
    r.name = "phase " + std::to_string(r.phase_epoch) + "/" + std::to_string(static_cast<int>(r.phase)) + "/" + std::to_string(r.progress_unix_ms);
    if (!pick(4)) r.removed_unix_ms = 1000 * (1 + pick(2));
    versions.push_back(r);
  }
  for (const auto& x : versions) {
    EXPECT_EQ(merge_torrent_request(x, x), x);
    for (const auto& y : versions) {
      const auto xy = merge_torrent_request(x, y);
      EXPECT_EQ(xy, merge_torrent_request(y, x));
      for (const auto& z : versions) ASSERT_EQ(merge_torrent_request(xy, z), merge_torrent_request(x, merge_torrent_request(y, z)));
    }
  }
}

std::string summary(const TorrentRequest& r) {
  return "hash=" + r.info_hash.substr(0, 1) + " created=" + std::to_string(r.created_unix_ms) + " desired=" + std::to_string(static_cast<int>(r.desired)) +
         "@" + std::to_string(r.desired_changed_unix_ms) + " claim=" + (r.claim ? std::to_string(r.claim->epoch) : "-") +
         " phase=" + std::to_string(static_cast<int>(r.phase)) + " error=" + r.error_code + " removed=" + std::to_string(r.removed_unix_ms);
}

// Where two collections differ, readably.
std::string difference(const TorrentRequestMap& x, const TorrentRequestMap& y) {
  std::string out;
  std::set<std::string> keys;
  for (const auto& [key, _] : x) keys.insert(key);
  for (const auto& [key, _] : y) keys.insert(key);
  for (const auto& key : keys) {
    const auto in_x = x.find(key);
    const auto in_y = y.find(key);
    if (in_x != x.end() && in_y != y.end() && in_x->second == in_y->second) continue;
    out += key.substr(0, 4) + ": " + (in_x == x.end() ? "absent" : summary(in_x->second)) + " | " + (in_y == y.end() ? "absent" : summary(in_y->second)) + "\n";
  }
  return out;
}

std::string sides_of(const TorrentRequestMap* base, const std::vector<const TorrentRequestMap*>& sides) {
  std::string out = base ? "with a base\n" : "without a base\n";
  if (base)
    for (const auto& [key, r] : *base) out += "  base " + key.substr(0, 4) + ": " + summary(r) + "\n";
  for (size_t i = 0; i < sides.size(); ++i)
    for (const auto& [key, r] : *sides[i]) out += "  " + std::string(1, static_cast<char>('a' + i)) + " " + key.substr(0, 4) + ": " + summary(r) + "\n";
  return out;
}

// The collection merge, with and without a common base: commutative and
// idempotent, and associative over sides that share a base. Each side is a
// branch from the base: requests changed, added, swept. As in every head
// Macha writes, no side holds two live requests for one torrent (its add
// refuses one); two branches' concurrent adds of one torrent are what the
// duplicate rule settles.
TEST(LeaseTorrentRequest, TheCollectionMergeIsAJoinWithAndWithoutABase) {
  std::mt19937_64 random(7);
  const auto pick = [&](size_t n) { return static_cast<size_t>(random() % n); };
  const std::vector<NodeId> nodes{a_node, b_node, node_of(0x33)};
  TorrentRequestMap base;
  for (char c : std::string("abcdef")) {
    auto r = sample_torrent_request(std::string(32, c), std::string(40, c), 100 + static_cast<uint64_t>(c));
    if (c == 'e') r.removed_unix_ms = 500;
    base.emplace(r.id, r);
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
        it->second.desired = TorrentDesired::paused;
        it->second.desired_changed_unix_ms = 1000 + static_cast<uint64_t>(salt);
        it->second.desired_changed_by = nodes[static_cast<size_t>(salt) % 3];
      } else if (roll == 2) {
        const auto epoch = 1 + pick(2);
        it->second.claim = TorrentClaim{nodes[static_cast<size_t>(salt) % 3], epoch, 2000 + static_cast<uint64_t>(salt)};
        it->second.phase = TorrentPhase::downloading;
        it->second.phase_epoch = epoch;
        it->second.progress_unix_ms = 2000 + static_cast<uint64_t>(salt);
      } else if (roll == 3) {
        it->second.removed_unix_ms = 3000 + static_cast<uint64_t>(salt);
      }
      ++it;
    }
    if (pick(2)) {
      // A new add, of a torrent no request here holds live: a fresh one, or
      // the removed request's. One other branch may add the same at once
      // (three at once: the next case).
      auto id = "n" + std::to_string(salt);
      id.resize(32, 'n');
      const char torrent = salt % 3 == 2 ? 'z' : pick(2) ? 'e' : 'y';
      auto r = sample_torrent_request(id, std::string(40, torrent), 4000 + static_cast<uint64_t>(salt));
      side.emplace(r.id, r);
    }
    return side;
  };
  for (int round = 0; round < 200; ++round) {
    const auto a = branch(3 * round), b = branch(3 * round + 1), c = branch(3 * round + 2);
    for (const TorrentRequestMap* with : {static_cast<const TorrentRequestMap*>(&base), static_cast<const TorrentRequestMap*>(nullptr)}) {
      const auto ab = TorrentRequests::merge(with, a, b);
      const auto ba = TorrentRequests::merge(with, b, a);
      ASSERT_TRUE(ab == ba) << round << "\n" << difference(ab, ba);
      const auto again = TorrentRequests::merge(with, ab, ab);
      ASSERT_TRUE(again == ab) << round << "\n" << difference(again, ab);
      const auto left_first = TorrentRequests::merge(with, ab, c);
      const auto right_first = TorrentRequests::merge(with, a, TorrentRequests::merge(with, b, c));
      ASSERT_TRUE(left_first == right_first) << round << "\n" << sides_of(with, {&a, &b, &c}) << difference(left_first, right_first);
    }
  }
}

// Finding (Macha too, torrent_request.cpp:234-241, kept for identical
// answers): with three concurrent adds of one torrent the collection merge
// is not associative. The third request is cancelled by whichever request
// beat it first, so its cancel stamp (max of its own and that keeper's
// created_unix_ms) and its error text depend on the order heads were merged.
// Replicas that merged in different orders hold different bytes until they
// merge each other's result, which settles on one answer.
TEST(LeaseTorrentRequest, ThreeConcurrentAddsSettleOnlyAtTheNextMerge) {
  const auto add = [](char tag, uint64_t created) {
    auto r = sample_torrent_request(std::string(32, tag), std::string(40, 'y'), created);
    return TorrentRequestMap{{r.id, r}};
  };
  const auto a = add('1', 4177), b = add('2', 4178), c = add('3', 4179);
  const auto left_first = TorrentRequests::merge(nullptr, TorrentRequests::merge(nullptr, a, b), c);
  const auto right_first = TorrentRequests::merge(nullptr, a, TorrentRequests::merge(nullptr, b, c));
  const auto& third_left = left_first.at(std::string(32, '3'));
  const auto& third_right = right_first.at(std::string(32, '3'));
  EXPECT_EQ(third_left.desired_changed_unix_ms, 4177u);
  EXPECT_EQ(third_left.error, "request " + std::string(32, '1') + " holds the same torrent");
  EXPECT_EQ(third_right.desired_changed_unix_ms, 4178u);
  EXPECT_EQ(third_right.error, "request " + std::string(32, '2') + " holds the same torrent");
  // Either way the earliest keeps the torrent and the others are cancelled.
  for (const auto* merged : {&left_first, &right_first}) {
    EXPECT_EQ(merged->at(std::string(32, '1')).desired, TorrentDesired::active);
    EXPECT_EQ(merged->at(std::string(32, '2')).desired, TorrentDesired::cancelled);
    EXPECT_EQ(merged->at(std::string(32, '3')).desired, TorrentDesired::cancelled);
  }
  // The next merge settles it, the same from either side.
  const auto settled = TorrentRequests::merge(nullptr, left_first, right_first);
  EXPECT_EQ(settled, TorrentRequests::merge(nullptr, right_first, left_first));
  EXPECT_EQ(TorrentRequests::merge(nullptr, settled, left_first), settled);
  EXPECT_EQ(TorrentRequests::merge(nullptr, settled, right_first), settled);
  EXPECT_EQ(settled.at(std::string(32, '3')).desired_changed_unix_ms, 4178u);
}

// Requirement E3: a tombstone is kept 7 days; then the sweeper, the node
// with no active member of a lower id, erases it.
TEST(LeaseTorrentRequest, ATombstoneIsSweptAfterSevenDaysByTheLowestMember) {
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  TestMembership low_members(a_node, time);
  TestMembership high_members(b_node, time);
  low_members.join(b_node);
  high_members.join(a_node);
  lease::LeaseKeeper low_keeper(low_members, time, wall);
  lease::LeaseKeeper high_keeper(high_members, time, wall);
  auto removed = sample_torrent_request(std::string(32, 'a'), std::string(40, '1'), 100);
  removed.removed_unix_ms = wall.unix_ms();
  auto low_head = collection_of({removed});
  auto high_head = low_head;
  TorrentOwner low(low_head, low_keeper, low_members, wall);
  TorrentOwner high(high_head, high_keeper, high_members, wall);

  wall.advance(7 * 24h);
  low.pass();
  EXPECT_EQ(low_head.size(), 1u);  // strictly past the window
  wall.advance(1ms);
  high.pass();
  EXPECT_EQ(high_head.size(), 1u);  // a lower member is active
  low.pass();
  EXPECT_TRUE(low_head.empty());
  // With the lower member gone, the next node sweeps.
  high_members.leave(a_node);
  high.pass();
  EXPECT_TRUE(high_head.empty());
}

// Finding (Macha too, torrent_coordinator.cpp:1066-1068, by reading): the
// window is computed as `now - removed > grace` on unsigned milliseconds, so
// a tombstone stamped by a node whose clock is ahead of the sweeper's wraps
// round and is erased at once, letting a branch that missed the removal
// bring the request back. Kept as Macha has it for identical answers;
// disabled until the maintainer decides (this is what it should do).
TEST(LeaseTorrentRequest, DISABLED_ATombstoneFromAClockAheadIsKeptItsWindow) {
  auto removed = sample_torrent_request(std::string(32, 'a'), std::string(40, '1'), 100);
  removed.removed_unix_ms = 2'000'000;
  const auto head = collection_of({removed});
  EXPECT_TRUE(head.expired(1'000'000).empty());
}

// Finding (Macha too, by reading: metadata.cpp:873 refuses a request with no
// id in a snapshot, :1735-1740 accepts one in DLT9): a delta can install a
// record the snapshot decoder refuses, so a head that applied it encodes a
// snapshot it cannot read back. Kept as Macha has it for identical answers;
// disabled until the maintainer decides (this is what it should do).
TEST(LeaseTorrentRequest, DISABLED_ADeltaCannotInstallARecordTheSnapshotRefuses) {
  Writer delta;
  delta.u32(1);
  encode_torrent_request(delta, sample_torrent_request("", std::string(40, '1'), 100));
  delta.u32(0);
  EXPECT_THROW(decoded_delta(delta.data()), DecodeError);
}

// What a head that took that delta holds today: it encodes, and its own
// bytes are refused.
TEST(LeaseTorrentRequest, AnUnkeyedRecordFromADeltaPoisonsTheSnapshot) {
  Writer delta;
  delta.u32(1);
  encode_torrent_request(delta, sample_torrent_request("", std::string(40, '1'), 100));
  delta.u32(0);
  TorrentRequests head;
  head.apply(decoded_delta(delta.data()));
  EXPECT_EQ(head.size(), 1u);
  EXPECT_THROW(decoded(encoded(head)), DecodeError);
}

// Moved from test_hydration_catalogue.cpp,
// test_torrent_requests_belong_to_the_cluster_and_are_driven_by_their_owner
// (:2378), through the lease template, on stepped time. Kept: every
// assertion about the requests, their claims and their owner. Not moved,
// being Macha's and staying its gate: the HTTP API's bodies and status
// fields, the engine (job states, the local job a request adopts on
// migration), the ingest sources and search. Waiting for step 4: the records
// living in a head (mutate_delta here is a join into this node's copy).
TEST(LeaseTorrentRequest, RequestsBelongToTheClusterAndAreDrivenByTheirOwner) {
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  const auto self = node_of(0x40);
  TestMembership members(self, time);
  lease::LeaseKeeper keeper(members, time, wall);
  TorrentRequests head;
  TorrentOwner coordinator(head, keeper, members, wall);
  const auto tick = [&] {
    time.advance(2s);
    wall.advance(2s);
  };

  // Nothing provides torrents: the node takes adds that wait for a capable
  // node.
  {
    const auto added = coordinator.add(std::string(32, '1'), "1234567890123456789012345678901234567890", "Waiting");
    ASSERT_EQ(added.status, 202);
    EXPECT_EQ(added.request->info_hash, "1234567890123456789012345678901234567890");
    EXPECT_EQ(added.request->phase, TorrentPhase::awaiting_node);
    EXPECT_EQ(added.request->name, "Waiting");
    EXPECT_FALSE(added.request->claim.has_value());
    const auto id = added.request->id;
    coordinator.pass();
    EXPECT_FALSE(coordinator.request(id)->claim.has_value());  // not capable

    // One request holds a torrent: a second add is refused naming it.
    const auto again = coordinator.add(std::string(32, '2'), "1234567890123456789012345678901234567890", "");
    EXPECT_EQ(again.status, 409);
    EXPECT_EQ(again.code, "torrent_already_added");
    EXPECT_EQ(again.request->id, id);
    EXPECT_FALSE(again.node.has_value());

    EXPECT_EQ(coordinator.requests().size(), 1u);
    tick();
    const auto paused = coordinator.act(id, "pause");
    EXPECT_EQ(paused.status, 202);
    EXPECT_EQ(paused.request->desired, TorrentDesired::paused);
    const auto patched = coordinator.patch_remove_after(id, 3600000);
    EXPECT_EQ(patched.status, 200);
    EXPECT_EQ(patched.request->remove_after_ms, std::optional<uint64_t>(3600000));
    EXPECT_EQ(coordinator.act(id, "clear").status, 202);
    EXPECT_TRUE(coordinator.requests().empty());

    // Added already paused, in the same write: no pause to race.
    const auto held = coordinator.add(std::string(32, '3'), "abcdefabcdefabcdefabcdefabcdefabcdefabcd", "Held", true);
    ASSERT_EQ(held.status, 202);
    EXPECT_EQ(held.request->desired, TorrentDesired::paused);
    tick();
    const auto resumed = coordinator.act(held.request->id, "resume");
    EXPECT_EQ(resumed.status, 202);
    EXPECT_EQ(resumed.request->desired, TorrentDesired::active);
    EXPECT_EQ(coordinator.act(held.request->id, "clear").status, 202);
    EXPECT_EQ(coordinator.act("whatever", "pause").status, 404);
    EXPECT_FALSE(coordinator.request("whatever").has_value());
  }

  // This node can run torrents now.
  coordinator.capable = true;

  // An add is a request; the capable node claims it, runs it under the
  // request's id, applies the operator's intent, and lets it go when cleared.
  {
    tick();
    const auto added = coordinator.add(std::string(32, '8'), std::string(40, '8'), "Owned");
    ASSERT_EQ(added.status, 202);
    const auto id = added.request->id;
    EXPECT_EQ(added.request->phase, TorrentPhase::awaiting_node);
    EXPECT_FALSE(added.request->remove_after_ms.has_value());  // the cluster default is off

    coordinator.pass();
    auto r = coordinator.request(id);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->claim.has_value());
    EXPECT_EQ(r->claim->holder, self);
    EXPECT_EQ(r->claim->epoch, 1u);
    EXPECT_EQ(r->phase, TorrentPhase::downloading);
    ASSERT_TRUE(coordinator.jobs.contains(id));  // run under the request's id

    tick();
    EXPECT_EQ(coordinator.act(id, "pause").status, 202);
    coordinator.pass();
    EXPECT_TRUE(coordinator.jobs.at(id).paused);
    tick();
    EXPECT_EQ(coordinator.act(id, "resume").status, 202);
    coordinator.pass();
    EXPECT_FALSE(coordinator.jobs.at(id).paused);

    EXPECT_EQ(coordinator.act(id, "clear").status, 409);  // still running
    tick();
    EXPECT_EQ(coordinator.act(id, "cancel").status, 202);
    coordinator.pass();  // stops it locally
    coordinator.pass();  // and records it
    EXPECT_EQ(coordinator.request(id)->phase, TorrentPhase::cancelled);
    EXPECT_EQ(coordinator.act(id, "clear").status, 202);
    coordinator.pass();
    EXPECT_FALSE(coordinator.request(id).has_value());
    EXPECT_FALSE(coordinator.jobs.contains(id));
  }

  // Removal after completion is off unless asked; with a delay of 0 the
  // owner removes the job and the request at its next pass.
  {
    tick();
    const auto added = coordinator.add(std::string(32, '9'), std::string(40, '9'), "Finished");
    const auto id = added.request->id;
    coordinator.pass();
    coordinator.jobs.at(id).phase = TorrentPhase::completed;
    tick();
    coordinator.pass();
    auto r = coordinator.request(id);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->phase, TorrentPhase::completed);
    EXPECT_EQ(r->claim->holder, self);
    tick();
    coordinator.pass();
    EXPECT_TRUE(coordinator.request(id).has_value());  // no delay set: kept
    tick();
    ASSERT_EQ(coordinator.patch_remove_after(id, 0).status, 200);
    coordinator.pass();
    EXPECT_FALSE(coordinator.request(id).has_value());
    EXPECT_FALSE(coordinator.jobs.contains(id));
  }

  // A claim holds while its node is a member; past the absence window
  // another node takes it at the next epoch. A node that finds its own
  // claim superseded stops and lets its copy go.
  {
    const auto gone = node_of(0x77);  // never a member
    TorrentRequest lapsed;
    lapsed.id = std::string(32, 'e');
    lapsed.info_hash = std::string(40, 'a');
    lapsed.source = "magnet:?xt=urn:btih:" + lapsed.info_hash;
    lapsed.created_unix_ms = 1;
    lapsed.claim = TorrentClaim{gone, 1, 1};
    lapsed.phase = TorrentPhase::downloading;
    lapsed.phase_epoch = 1;
    head.join(lapsed);

    // The default window is ten minutes: an absent owner keeps its claim.
    coordinator.pass();
    time.advance(10min - 1ms);
    wall.advance(10min - 1ms);
    coordinator.pass();
    EXPECT_EQ(coordinator.request(lapsed.id)->claim->holder, gone);
    EXPECT_FALSE(coordinator.jobs.contains(lapsed.id));

    time.advance(1ms);
    wall.advance(1ms);
    coordinator.pass();  // the window has passed: taken over
    auto r = coordinator.request(lapsed.id);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->claim->holder, self);
    EXPECT_EQ(r->claim->epoch, 2u);
    EXPECT_EQ(r->phase_epoch, 2u);
    ASSERT_TRUE(coordinator.jobs.contains(lapsed.id));

    // A pin holds while the node it names is known, and no longer once that
    // node is forgotten: then any node may take the request.
    const auto known_peer = node_of(0x55);
    coordinator.known.insert(known_peer);
    const auto pinned_to = [&](char tag, const NodeId& node) {
      TorrentRequest pinned;
      pinned.id = std::string(32, tag);
      pinned.info_hash = std::string(40, tag);
      pinned.source = "magnet:?xt=urn:btih:" + pinned.info_hash;
      pinned.created_unix_ms = 2;
      pinned.phase = TorrentPhase::awaiting_node;
      pinned.pinned_node_id = node;
      head.join(pinned);
      return pinned.id;
    };
    const auto held = pinned_to('b', known_peer);
    const auto freed = pinned_to('c', gone);
    coordinator.pass();
    coordinator.pass();
    EXPECT_FALSE(coordinator.request(held)->claim.has_value());
    ASSERT_TRUE(coordinator.request(freed)->claim.has_value());
    EXPECT_EQ(coordinator.request(freed)->claim->holder, self);

    // Another node's newer claim wins; this node lets its copy go. (Macha's
    // case names `gone` here, absent for milliseconds of real time; on
    // stepped time `gone` is ten minutes absent and would be taken over at
    // once, so the newer claim is a member's.)
    const auto rival = node_of(0x66);
    members.join(rival);
    auto superseding = *r;
    superseding.claim = TorrentClaim{rival, 3, wall.unix_ms()};
    superseding.phase_epoch = 3;
    superseding.progress_unix_ms = wall.unix_ms() + 1;
    head.join(superseding);
    coordinator.pass();  // no takeover, only the let-go
    EXPECT_FALSE(coordinator.jobs.contains(lapsed.id));
    EXPECT_EQ(coordinator.request(lapsed.id)->claim->holder, rival);
  }
}

// Requirement E7: ranked takers wait 30 s per rank from when they first
// found the request claimable, so the preferred node goes first.
TEST(LeaseTorrentRequest, RankedTakersWaitTheirTurn) {
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  TestMembership members(a_node, time);
  lease::LeaseKeeper keeper(members, time, wall);
  TorrentRequests head;
  TorrentOwner owner(head, keeper, members, wall);
  owner.capable = true;
  const auto added = owner.add(std::string(32, 'r'), std::string(40, 'r'), "Ranked");
  owner.ranks[added.request->id] = 2;
  owner.pass();
  time.advance(59s);
  owner.pass();
  EXPECT_FALSE(owner.request(added.request->id)->claim.has_value());
  time.advance(1s);
  owner.pass();
  EXPECT_EQ(owner.request(added.request->id)->claim->holder, a_node);
}

}  // namespace
}  // namespace dagda::test::macha
