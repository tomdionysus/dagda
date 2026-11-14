//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The migration of Macha's layout v18 disks to the one layout, in place
// (Tom, 2026-11-04 night; migrate/migrate.h). First the mechanism: under
// the same key and nonce, v18's associated data (the id) and v19's (the
// label, the domain, the id) give the same ciphertext and another tag, so
// only the tag moves. Then the files: a loose object's 16 bytes, a pack
// record's tag and checksum, the ledgers re-sealed under the node-local key,
// the markers last. Then a whole v18 state built from the known answers,
// migrated, opened by the v19 store and read back; the second run finding
// nothing to do; a wrong key refused before any write; a crash between
// records left for a second run to finish; and the tool itself.
//
// The known answers are step 1a's v18 bytes and step 6b's v19 ones
// (build/kat/step6b/kat.py: GCM by hand over `openssl enc` blocks, checked
// against the GCM specification's test case 16 and the pinned v18 tag). The
// cases that opened Macha's files through the store
// (store_local_format_test's OpensALooseObjectMachaWrote and
// OpensAPackMachaWrote) are here, every assertion kept, after a migration.

#include "dagda/migrate/migrate.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "dagda/ledger/held_ledger.h"
#include "dagda/ledger/reference_counts.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/migrate/legacy_v18.h"
#include "dagda/store/control_store.h"
#include "dagda/store/local_format.h"
#include "dagda/store/local_store.h"
#include "dagda/store/persistent_cache.h"
#include "dagda/store/sealing.h"
#include "dagda/store/state_layout.h"
#include "dagda/store/storage_pool.h"
#include "dagda/test/fixtures/child_exit.h"
#include "dagda/test/fixtures/ledger.h"
#include "dagda/test/fixtures/legacy_v18.h"
#include "dagda/test/fixtures/migrate.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/argon2.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/key_custody.h"
#include "dannan/test/fixtures/child_process.h"

namespace dagda::migrate {
namespace {

using namespace std::chrono_literals;
using namespace test::v18;
using dannan::identity::hex;
using dannan::identity::NodeId;
using store::object_id;
using test::pattern;
using test::StoreBed;
using namespace store::local_format;
namespace legacy = test::legacy;

// ---- The mechanism ----------------------------------------------------------

// Under the same key and nonce, the id alone and the label, domain and id
// as associated data give the same ciphertext (the keystream does not see
// the associated data) and different tags (GHASH does). The migrated tag of
// Macha's record is step 6b's known answer; the layout a record carries is
// told by which associated data authenticates it.
TEST(MigrateSealing, TheSameKeyAndNonceGiveTheSameCiphertextAndAnotherTag) {
  const auto key = test::known_answer_key();
  const auto id = the_id();
  ASSERT_EQ(store::to_string(id), object_hex);
  const auto nonce = the_nonce();
  const auto plain = bytes_of(plaintext);
  const auto v18 = dannan::identity::aes_gcm_seal(key->bytes(), nonce, plain, associated_data_v18(id));
  const auto v19 = dannan::identity::aes_gcm_seal(key->bytes(), nonce, plain, v1.associated_data(id));
  EXPECT_EQ(hex(v18.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(v19.ciphertext), ciphertext_hex);
  EXPECT_EQ(hex(v18.tag), v18_tag_hex);
  EXPECT_EQ(hex(v19.tag), v1_tag_hex);
  EXPECT_NE(v18.tag, v19.tag);
  // The rewrite: opened under v18, sealed again under v19 with the same
  // nonce, the ciphertext checked identical, only the tag returned.
  EXPECT_EQ(hex(migrated_tag(*key, v1, id, nonce, v18.tag, v18.ciphertext, plain.size())), v1_tag_hex);
  EXPECT_EQ(record_layout(*key, v1, id, nonce, v18.tag, v18.ciphertext, plain.size()), RecordLayout::v18);
  EXPECT_EQ(record_layout(*key, v1, id, nonce, v19.tag, v19.ciphertext, plain.size()), RecordLayout::v19);
  auto flipped = v18.tag;
  flipped[0] ^= 1;
  EXPECT_EQ(record_layout(*key, v1, id, nonce, flipped, v18.ciphertext, plain.size()), RecordLayout::neither);
  EXPECT_EQ(record_layout(*test::key_of(7), v1, id, nonce, v18.tag, v18.ciphertext, plain.size()), RecordLayout::neither);
  EXPECT_THROW((void)migrated_tag(*key, v1, id, nonce, v19.tag, v19.ciphertext, plain.size()), std::runtime_error);
  // The migrated record opens as v19 and not as v18 any more.
  EXPECT_EQ(store::open_object(*key, v1, id, nonce, v19.tag, v18.ciphertext, plain.size()), plain);
  EXPECT_THROW((void)open_object_v18(*key, id, nonce, v19.tag, v18.ciphertext, plain.size()), std::runtime_error);
}

// A loose file: sixteen bytes at offset 28 change and nothing else. A second
// pass finds it v19; a dry run writes nothing.
TEST(MigrateLoose, OnlyTheTagMoves) {
  StoreBed bed;
  const auto file = loose_path(bed.dir.path() / "objects", the_id());
  legacy::write_file(file, unhex(loose_v18_hex));
  MigrationReport report;
  bool proved = false;
  MigrationOptions dry;
  dry.dry_run = true;
  migrate_loose_file(file, the_id(), *bed.key, v1, report, proved, dry);
  EXPECT_EQ(hex(legacy::read_file(file)), loose_v18_hex);
  EXPECT_EQ(report.loose_migrated, 1u);
  EXPECT_EQ(report.bytes_written, 0u);
  EXPECT_TRUE(proved);
  report = {};
  migrate_loose_file(file, the_id(), *bed.key, v1, report, proved, {});
  const auto after = legacy::read_file(file);
  EXPECT_EQ(hex(after), loose_v19_hex);
  const auto before = unhex(loose_v18_hex);
  ASSERT_EQ(after.size(), before.size());
  for (size_t i = 0; i < after.size(); ++i)
    if (i < loose_tag_offset || i >= loose_tag_offset + 16) {
      EXPECT_EQ(after[i], before[i]) << i;
    }
  EXPECT_EQ(report.loose_migrated, 1u);
  EXPECT_EQ(report.bytes_written, 16u);
  report = {};
  migrate_loose_file(file, the_id(), *bed.key, v1, report, proved, {});
  EXPECT_EQ(report.loose_already, 1u);
  EXPECT_EQ(report.loose_migrated, 0u);
  EXPECT_EQ(hex(legacy::read_file(file)), loose_v19_hex);
}

// Macha: OpensALooseObjectMachaWrote. A loose object file Macha wrote opens
// in the store once migrated, and not before.
TEST(MigrateLoose, ALooseObjectMachaWroteOpensOnceMigrated) {
  StoreBed bed;
  const auto root = bed.dir.path() / "store";
  const auto file = loose_path(root / "objects", the_id());
  legacy::write_file(file, unhex(loose_v18_hex));
  store::LocalStoreOptions packing;
  packing.limit = 64 * mib;
  {
    auto store = bed.open(root, packing);
    EXPECT_TRUE(store->has(the_id()));
    EXPECT_THROW((void)store->get(the_id()), std::runtime_error);
  }
  MigrationReport report;
  bool proved = false;
  migrate_loose_file(file, the_id(), *bed.key, v1, report, proved, {});
  auto store = bed.open(root, packing);
  EXPECT_TRUE(store->has(the_id()));
  EXPECT_EQ(store->get(the_id()), std::optional<Bytes>(bytes_of(plaintext)));
  EXPECT_FALSE(store->is_packed(the_id()));
}

// A pack: a put record's tag and the checksum after it change (48 bytes in
// one write) and nothing else; a touch and a tombstone, sealing nothing,
// are untouched. Macha: OpensAPackMachaWrote, after the migration: a put
// and then a touch keep the object and take the touch's time; a tombstone
// after them removes it.
TEST(MigratePack, OnlyTheTagAndChecksumMoveAndAPackMachaWroteOpens) {
  StoreBed bed(put_ms + 500);
  const auto root = bed.dir.path() / "store";
  Bytes pack = unhex(put_header_v18_hex);
  const auto ciphertext = unhex(ciphertext_hex);
  pack.insert(pack.end(), ciphertext.begin(), ciphertext.end());
  const auto touch = unhex(touch_hex);
  pack.insert(pack.end(), touch.begin(), touch.end());
  const auto file = legacy::pack_path(root, 1);
  legacy::write_file(file, pack);
  MigrationReport report;
  bool proved = false;
  migrate_pack_file(file, *bed.key, v1, report, proved, {});
  EXPECT_EQ(report.pack_records_migrated, 1u);
  EXPECT_EQ(report.pack_records_unsealed, 1u);
  EXPECT_EQ(report.bytes_written, 48u);
  auto expected = unhex(put_header_v19_hex);
  expected.insert(expected.end(), ciphertext.begin(), ciphertext.end());
  expected.insert(expected.end(), touch.begin(), touch.end());
  const auto after = legacy::read_file(file);
  EXPECT_EQ(hex(after), hex(expected));
  for (size_t i = 0; i < after.size(); ++i)
    if (i < pack_tag_offset || i >= pack_header_size) {
      EXPECT_EQ(after[i], pack[i]) << i;
    }
  ASSERT_TRUE(decode_pack_header(std::span<const uint8_t>(after).first(pack_header_size)).has_value());
  report = {};
  migrate_pack_file(file, *bed.key, v1, report, proved, {});
  EXPECT_EQ(report.pack_records_already, 1u);
  EXPECT_EQ(report.pack_records_migrated, 0u);
  EXPECT_EQ(hex(legacy::read_file(file)), hex(expected));

  store::LocalStoreOptions packing;
  packing.limit = 64 * mib;
  {
    auto store = bed.open(root, packing);
    EXPECT_TRUE(store->has(the_id()));
    EXPECT_TRUE(store->is_packed(the_id()));
    EXPECT_EQ(store->get(the_id()), std::optional<Bytes>(bytes_of(plaintext)));
    EXPECT_EQ(store->stored_size(the_id()), pack_header_size + plaintext.size());
    // Touched at put_ms + 500, which is now: not older than a millisecond.
    EXPECT_FALSE(store->older_than(the_id(), std::chrono::milliseconds(0)));
    bed.wall.advance(std::chrono::milliseconds(2));
    EXPECT_TRUE(store->older_than(the_id(), std::chrono::milliseconds(1)));
  }
  const auto tombstone = unhex(tombstone_hex);
  legacy::write_file(file, tombstone, true);
  auto store = bed.open(root, packing);
  EXPECT_FALSE(store->has(the_id()));
  EXPECT_FALSE(store->get(the_id()).has_value());
}

// ---- A whole state ------------------------------------------------------------

// A v18 state built from the known answers migrates in place, counts what
// it did, and opens as v19 with every object, claim and count read back;
// the second run finds it v19 and touches nothing.
TEST(MigrateState, AV18StateMigratesInPlaceAndOpensAsV19) {
  StoreBed bed;
  V18State v18(bed);
  const auto before = v18.digest();
  std::vector<std::string> said;
  MigrationOptions options;
  options.say = [&](std::string_view line) { said.emplace_back(line); };
  const auto report = v18.migrate(options);
  EXPECT_FALSE(report.already_v19);
  EXPECT_EQ(report.loose_migrated, 5u + 2 + 1);
  EXPECT_EQ(report.loose_already, 0u);
  EXPECT_EQ(report.loose_unreadable, 0u);
  EXPECT_EQ(report.packs, 2u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.pack_records_unsealed, 2u);
  EXPECT_EQ(report.pack_records_already, 0u);
  EXPECT_EQ(report.pack_regions_skipped, 0u);
  EXPECT_EQ(report.pack_tail_bytes, 0u);
  // The two held tries, the claims journal and its two tries, the two
  // reference tries.
  EXPECT_EQ(report.ledgers_migrated, 7u);
  EXPECT_EQ(report.ledgers_already, 0u);
  EXPECT_EQ(report.ledger_tail_bytes_dropped, 0u);
  // The backend's marker, its mirror, the state layout.
  EXPECT_EQ(report.markers_rewritten, 3u);
  EXPECT_GE(report.bytes_written, 8u * 16 + 5 * 48);
  EXPECT_TRUE(report.unreadable.empty());
  EXPECT_NE(report.summary().find("loose objects: 8 migrated"), std::string::npos) << report.summary();
  EXPECT_NE(report.summary().find("markers rewritten: 3"), std::string::npos);
  EXPECT_FALSE(said.empty());
  // Only tags, checksums, ledgers and markers changed: every loose file's
  // length is as it was, and the accounting file (none here) and the
  // custody are untouched.
  const auto after = v18.digest();
  EXPECT_EQ(after.size(), before.size() + 0u);
  EXPECT_EQ(after.at(v18.node_state / "identity" / "custody"), before.at(v18.node_state / "identity" / "custody"));

  // Idempotent: nothing to do, nothing written.
  const auto again = v18.migrate();
  EXPECT_TRUE(again.already_v19);
  EXPECT_EQ(again.loose_migrated + again.pack_records_migrated + again.ledgers_migrated + again.markers_rewritten, 0u);
  EXPECT_EQ(again.summary(), "state is layout v19 already: nothing to do\n");
  EXPECT_EQ(v18.digest(), after);

  expect_v19_opens(bed, v18);
}

// v18 wrote the collector's sightings in the clear; the migration seals
// them under the node-local key, so a migrated node keeps every deletion
// wait it had instead of starting them all again. A table already sealed is
// left alone.
TEST(MigrateState, ThePlainSightingsFilesAreSealed) {
  StoreBed bed;
  V18State v18(bed);
  const std::map<ObjectId, uint64_t> seen{{v18.loose[2].first, 1'000}, {v18.packed[2].first, 2'000}};
  const auto plain = ledger::encode_unreferenced_since(seen);
  legacy::write_file(v18.state / "retention" / "unreferenced-data.bin", plain);
  legacy::write_file(v18.state / "retention" / "unreferenced-control.bin", ledger::encode_unreferenced_since({}));
  const auto report = v18.migrate();
  // The seven ledgers of the plain fixture, and the two sightings files.
  EXPECT_EQ(report.ledgers_migrated, 9u);
  const auto read = [](const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const auto sealed = read(v18.state / "retention" / "unreferenced-data.bin");
  EXPECT_NE(sealed, plain);
  EXPECT_EQ(ledger::open_unreferenced_since(*v18.node_local(), sealed), seen);
  ledger::UnreferencedSince loaded(v18.state / "retention" / "unreferenced-data.bin", v18.node_local(), test::quiet_logger());
  EXPECT_EQ(loaded.size(), 2u);
  EXPECT_TRUE(v18.migrate().already_v19);
  EXPECT_EQ(read(v18.state / "retention" / "unreferenced-data.bin"), sealed);
  expect_v19_opens(bed, v18);
}

// A real Macha state directory holds more than the store's ledgers:
// metadata/ has a journal.log and is not a trie, and it, the repair position
// and the last survey are not the migration's to read. Left byte for byte, in
// a dry run and a real one.
TEST(MigrateState, WhatIsNotAKnownLedgerIsLeftAlone) {
  StoreBed bed;
  V18State v18(bed);
  const std::vector<std::filesystem::path> others{v18.state / "metadata" / "journal.log", v18.state / "metadata" / "checkpoint.meta",
                                                  v18.state / "metadata" / "root", v18.state / "repair" / "push-position",
                                                  v18.state / "availability" / "last-survey.bin"};
  for (const auto& path : others) legacy::write_file(path, bytes_of("not a ledger: " + path.filename().string()));
  const auto unchanged = [&] {
    for (const auto& path : others) EXPECT_EQ(read_text(path), "not a ledger: " + path.filename().string());
  };
  MigrationOptions dry;
  dry.dry_run = true;
  EXPECT_EQ(v18.migrate(dry).ledgers_migrated, 7u);
  unchanged();
  EXPECT_EQ(v18.migrate().ledgers_migrated, 7u);
  unchanged();
  expect_v19_opens(bed, v18);
}

// What the store would refuse to open once migrated is refused before the
// migration writes: a checkpoint from before the claims ledger, or tries
// holding claims, with no ledger marker (RetentionStore's own rule).
TEST(MigrateState, RetentionTheStoreWouldRefuseIsRefusedBeforeAnyWrite) {
  const auto expect_refused = [](V18State& v18, std::string_view why) {
    const auto before = v18.digest();
    try {
      (void)v18.migrate();
      ADD_FAILURE() << "not refused";
    } catch (const MigrationRefused& error) {
      EXPECT_NE(std::string(error.what()).find(why), std::string::npos) << error.what();
    }
    EXPECT_EQ(v18.digest(), before);
  };
  {
    StoreBed bed;
    V18State v18(bed);
    std::filesystem::remove(v18.state / "retention" / "ledger");
    legacy::write_file(v18.state / "retention" / "claims.meta", bytes_of("MRTS0001"));
    expect_refused(v18, "checkpoint");
  }
  {
    StoreBed bed;
    V18State v18(bed);
    std::filesystem::remove(v18.state / "retention" / "ledger");
    expect_refused(v18, "no ledger marker");
  }
  {
    // With the marker, the reserved names are the store's to remove at open.
    StoreBed bed;
    V18State v18(bed);
    legacy::write_file(v18.state / "retention" / "claims.meta", bytes_of("MRTS0001"));
    EXPECT_EQ(v18.migrate().ledgers_migrated, 7u);
    expect_v19_opens(bed, v18);
  }
}

// A migrated marker keeps the node id it was written with, and the pool
// refuses another node's disk: an id that differs is refused here, before any
// write, not by the node that later cannot open the disk.
TEST(MigrateState, ABackendOfAnotherNodeIdIsRefusedBeforeAnyWrite) {
  StoreBed bed;
  V18State v18(bed);
  const auto before = v18.digest();
  MigrationOptions options;
  options.node = filled(0x01);
  try {
    (void)v18.migrate(options);
    ADD_FAILURE() << "not refused";
  } catch (const MigrationRefused& error) {
    const std::string why = error.what();
    EXPECT_NE(why.find(dannan::identity::to_string(v18.node)), std::string::npos) << why;
    EXPECT_NE(why.find(dannan::identity::to_string(filled(0x01))), std::string::npos) << why;
    EXPECT_NE(why.find("node-id"), std::string::npos) << why;
  }
  EXPECT_EQ(v18.digest(), before);
  options.node = v18.node;
  EXPECT_EQ(v18.migrate(options).loose_migrated, 8u);
  expect_v19_opens(bed, v18);
}

// On a disk of a million objects the operator sees it moving.
TEST(MigrateState, ARunReportsItsProgressAndRate) {
  StoreBed bed;
  V18State v18(bed);
  std::vector<std::string> said;
  MigrationOptions options;
  options.progress_every = 2;
  options.say = [&](std::string_view line) { said.emplace_back(line); };
  (void)v18.migrate(options);
  const auto lines =
      std::count_if(said.begin(), said.end(), [](const std::string& line) { return line.starts_with("progress: ") && line.ends_with(" MiB/s"); });
  EXPECT_GE(lines, 4);
}

// Forward and back leaves every object and marker as Macha wrote it, byte for
// byte. The ledgers are re-sealed, not restored, so forward again proves they
// hold what they held.
TEST(MigrateBack, ThereAndBackLeavesTheObjectsAndMarkersAsTheyWere) {
  StoreBed bed;
  V18State v18(bed);
  const auto sightings = v18.state / "retention" / "unreferenced-data.bin";
  const auto plain = ledger::encode_unreferenced_since({{v18.loose[2].first, 1'000}});
  legacy::write_file(sightings, plain);
  const auto store_files = [&] {
    auto digest = v18.digest();
    std::erase_if(digest, [&](const auto& entry) {
      const auto text = entry.first.string();
      return text.find("/ledger/") != std::string::npos || text.find("/ledger-") != std::string::npos || text.find("claims.log") != std::string::npos ||
             text.find(".lock") != std::string::npos;
    });
    return digest;
  };
  const auto before = store_files();
  (void)v18.migrate();
  EXPECT_NE(store_files(), before);
  MigrationOptions options;
  options.cache = v18.cache;
  const auto report = migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
  EXPECT_TRUE(report.reverse);
  EXPECT_EQ(report.loose_migrated, 8u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.ledgers_migrated, 8u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  EXPECT_NE(report.summary().find("back to layout v18"), std::string::npos);
  EXPECT_EQ(store_files(), before);
  EXPECT_EQ(hex(legacy::read_file(loose_path(v18.backend / "objects", v18.known))), loose_v18_hex);
  // Done already: every record looked at, nothing moved.
  const auto again = migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
  EXPECT_EQ(again.loose_migrated + again.pack_records_migrated + again.ledgers_migrated + again.markers_rewritten, 0u);
  EXPECT_EQ(again.loose_already, 8u);
  EXPECT_EQ(store_files(), before);
  // The disk is v18 to the forward run in every part, ledgers included.
  const auto forward = v18.migrate();
  EXPECT_EQ(forward.loose_migrated, 8u);
  EXPECT_EQ(forward.ledgers_migrated, 8u);
  expect_v19_opens(bed, v18);
}

// An end part-way, in either direction, is finished going back.
TEST(MigrateBack, ARunThatStoppedIsFinishedByASecondRun) {
  struct Stop {};
  const auto stopping_after = [](size_t steps, size_t& seen) {
    return [steps, &seen](const MigrationReport&) {
      if (++seen == steps) throw Stop{};
    };
  };
  {
    // Forward stopped among its records: the state marker says so.
    StoreBed bed;
    V18State v18(bed);
    const auto backend_marker = read_text(v18.backend / ".macha.backend");
    size_t seen = 0;
    MigrationOptions options;
    options.after_step = stopping_after(3, seen);
    EXPECT_THROW((void)v18.migrate(options), Stop);
    EXPECT_EQ(read_text(v18.state / "storage-layout"), "dagda-state-layout-going-forward\n");
    MigrationOptions back;
    back.cache = v18.cache;
    const auto report = migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), back);
    EXPECT_EQ(report.loose_migrated, 3u);
    EXPECT_EQ(report.loose_already, 5u);
    EXPECT_EQ(read_text(v18.backend / ".macha.backend"), backend_marker);
    EXPECT_EQ(hex(legacy::read_file(loose_path(v18.backend / "objects", v18.known))), loose_v18_hex);
    EXPECT_EQ(v18.migrate().loose_migrated, 8u);
    expect_v19_opens(bed, v18);
  }
  {
    // Back stopped among its records: the markers are still v19.
    StoreBed bed;
    V18State v18(bed);
    (void)v18.migrate();
    size_t seen = 0;
    MigrationOptions options;
    options.cache = v18.cache;
    options.after_step = stopping_after(3, seen);
    EXPECT_THROW((void)migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options), Stop);
    EXPECT_NE(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
    options.after_step = {};
    const auto report = migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
    EXPECT_EQ(report.loose_already, 3u);
    EXPECT_EQ(report.loose_migrated, 5u);
    EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
    EXPECT_EQ(v18.migrate().loose_migrated, 8u);
    expect_v19_opens(bed, v18);
  }
}

// A loose object's modification time is when it was last touched: the store
// ages it by that (older_than, remove_if_older_than), so a migration that left
// every file newly written would restart every deletion wait on the disk.
TEST(MigrateState, ALooseObjectKeepsItsTouchedTime) {
  StoreBed bed;
  V18State v18(bed);
  const auto file = loose_path(v18.backend / "objects", v18.loose[1].first);
  const auto long_ago = std::filesystem::last_write_time(file) - std::chrono::hours(24 * 30);
  std::filesystem::last_write_time(file, long_ago);
  (void)v18.migrate();
  EXPECT_EQ(std::filesystem::last_write_time(file), long_ago);
  MigrationOptions options;
  options.cache = v18.cache;
  (void)migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
  EXPECT_EQ(std::filesystem::last_write_time(file), long_ago);
}

// The wrong key, a dry run, another node's id and a running node change nothing.
TEST(MigrateBack, RefusalsAndADryRunWriteNothing) {
  StoreBed bed;
  V18State v18(bed);
  (void)v18.migrate();
  const auto before = v18.digest();
  MigrationOptions options;
  options.cache = v18.cache;
  const auto wrong = cluster_key_from(bed.dir.path() / "wrong.key", "another cluster\n");
  const auto refused = [&](const std::function<void()>& run, std::string_view expect) {
    try {
      run();
      ADD_FAILURE() << "not refused";
    } catch (const MigrationRefused& error) {
      EXPECT_NE(std::string(error.what()).find(expect), std::string::npos) << error.what();
    }
    EXPECT_EQ(v18.digest(), before);
  };
  refused([&] { (void)migrate_state_back(v18.state, {v18.backend}, wrong, v18.custody(), options); }, "wrong cluster key");
  auto other = options;
  other.node = filled(0x01);
  refused([&] { (void)migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), other); }, "belongs to node");
  {
    store::StorageLock held(v18.backend);
    refused([&] { (void)migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options); }, "is in use");
  }
  auto dry = options;
  dry.dry_run = true;
  const auto report = migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), dry);
  EXPECT_EQ(report.loose_migrated, 8u);
  EXPECT_EQ(report.bytes_written, 0u);
  EXPECT_EQ(v18.digest(), before);
}

// A dry run classifies and counts as a run would and writes nothing.
TEST(MigrateState, ADryRunWritesNothing) {
  StoreBed bed;
  V18State v18(bed);
  const auto before = v18.digest();
  MigrationOptions options;
  options.dry_run = true;
  const auto report = v18.migrate(options);
  EXPECT_TRUE(report.dry_run);
  EXPECT_EQ(report.loose_migrated, 8u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.ledgers_migrated, 7u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  EXPECT_EQ(report.bytes_written, 0u);
  EXPECT_EQ(v18.digest(), before);
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  EXPECT_NE(report.summary().find("dry run"), std::string::npos);
}

// The wrong cluster key is refused before any write, and so is a disk the
// migration does not recognise: a state with no marker or another layout's,
// a v19 state of another domain, a backend without a marker, a store in
// use. Nothing on the disk changes by any refusal.
TEST(MigrateState, AWrongKeyAndAnUnrecognisedDiskAreRefusedBeforeAnyWrite) {
  StoreBed bed;
  V18State v18(bed);
  // The test's own files go beside the node, so the node's digest is the
  // disk's alone.
  const auto scratch = bed.dir.path() / "scratch";
  std::filesystem::create_directories(scratch);
  const auto before = v18.digest();
  const auto refused = [&](const std::function<void()>& run) -> std::string {
    try {
      run();
    } catch (const MigrationRefused& error) {
      EXPECT_EQ(v18.digest(), before);
      return error.what();
    }
    ADD_FAILURE() << "not refused";
    return {};
  };
  const auto wrong = cluster_key_from(scratch / "wrong.key", "another cluster\n");
  MigrationOptions options;
  options.cache = v18.cache;
  auto why = refused([&] { migrate_state(v18.state, {v18.backend}, wrong, v18.custody(), options); });
  EXPECT_NE(why.find("wrong cluster key"), std::string::npos) << why;
  EXPECT_NE(why.find("opens none of the first 8 records"), std::string::npos) << why;

  why = refused([&] { migrate_state(v18.state, {v18.backend, scratch / "nowhere"}, v18.cluster(), v18.custody(), options); });
  EXPECT_NE(why.find("nowhere"), std::string::npos) << why;
  const auto unmarked = scratch / "unmarked";
  std::filesystem::create_directories(unmarked);
  why = refused([&] { migrate_state(v18.state, {v18.backend, unmarked}, v18.cluster(), v18.custody(), options); });
  EXPECT_NE(why.find("has no .macha.backend marker"), std::string::npos) << why;
  why = refused([&] { migrate_state(scratch / "no-state", {v18.backend}, v18.cluster(), v18.custody(), options); });
  EXPECT_NE(why.find("not a directory"), std::string::npos) << why;
  {
    // In use: the backend's lock held, as a running node holds it.
    store::StorageLock held(v18.backend);
    why = refused([&] { migrate_state(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options); });
    EXPECT_NE(why.find("is in use"), std::string::npos) << why;
  }
  // Other states: no marker, another layout, another domain's v19.
  for (const auto& [text, expect] : std::vector<std::pair<std::string, std::string>>{
           {"macha-state-layout-v17\n", "neither layout v18"}, {"dagda-state-layout-v19 00112233445566778899aabbccddeeff\n", "neither layout v18"}}) {
    const auto other = scratch / "other-state";
    std::filesystem::create_directories(other);
    std::ofstream(other / "storage-layout", std::ios::binary) << text;
    why = refused([&] { migrate_state(other, {}, v18.cluster(), v18.custody(), {}); });
    EXPECT_NE(why.find(expect), std::string::npos) << why;
    std::filesystem::remove_all(other);
  }
  std::filesystem::create_directories(scratch / "bare");
  why = refused([&] { migrate_state(scratch / "bare", {}, v18.cluster(), v18.custody(), {}); });
  EXPECT_NE(why.find("no storage-layout marker"), std::string::npos) << why;
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
}

// A ledger that opens under neither key (another node's, or damaged) is
// refused, naming it, before any write: the objects are checked after the
// ledgers are, so nothing on the disk has moved. Once it is dealt with a run
// migrates everything. A trie under a name the store does not give a ledger is
// not the migration's business.
TEST(MigrateState, ALedgerUnderNeitherKeyIsRefusedAndTheStateStaysV18) {
  StoreBed bed;
  V18State v18(bed);
  const auto stranger_at = [&](const std::filesystem::path& dir) {
    ledger::ObjectTrie stranger(dir, test::key_of(0x77), {}, test::quiet_logger());
    stranger.apply(std::vector<ledger::ObjectTrie::Change>{{v18.known, Bytes{1}}});
    stranger.checkpoint();
  };
  stranger_at(v18.state / "ledger" / "data-stranger");
  stranger_at(v18.state / "ledger" / "stranger");
  const auto before = v18.digest();
  std::string why;
  try {
    (void)v18.migrate();
  } catch (const MigrationRefused& error) {
    why = error.what();
  }
  EXPECT_NE(why.find("ledger/data-stranger"), std::string::npos) << why;
  EXPECT_NE(why.find("neither the node-local key nor the domain key"), std::string::npos) << why;
  EXPECT_EQ(v18.digest(), before);
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  EXPECT_EQ(read_text(v18.backend / ".macha.backend"), legacy::backend_marker_v18(v18.node, v18.token));
  EXPECT_EQ(hex(legacy::read_file(loose_path(v18.backend / "objects", v18.known))), loose_v18_hex);
  std::filesystem::remove_all(v18.state / "ledger" / "data-stranger");
  const auto report = v18.migrate();
  EXPECT_EQ(report.loose_migrated, 8u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  expect_v19_opens(bed, v18);
}

// A crash between records (a child that ends the process after three)
// leaves every record v18 or v19, the ledgers under one key or the other,
// and the markers not v19, so the store still refuses the disk; a second run
// finishes, skipping what was done, and the store opens.
TEST(MigrateState, ACrashMidWayLeavesADiskASecondRunFinishes) {
  StoreBed bed;
  V18State v18(bed);
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      // Secure memory is not inherited across a fork: the child derives its
      // own keys from the files and never touches the parent's.
      MigrationOptions options;
      options.cache = v18.cache;
      options.after_step = [](const MigrationReport& so_far) {
        if (so_far.loose_migrated + so_far.pack_records_migrated >= 3) test::child_exit(0);
      };
      (void)migrate_state(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
      test::child_exit(21);
    } catch (...) {
      test::child_exit(22);
    }
  }
  ASSERT_EQ(test::exit_code(child), std::optional<int>(0));

  // What it left: three records v19, the rest v18, the backend's marker v18 and
  // the state's saying it is part way.
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "dagda-state-layout-going-forward\n");
  EXPECT_EQ(read_text(v18.backend / ".macha.backend"), legacy::backend_marker_v18(v18.node, v18.token));
  size_t v19_records = 0;
  size_t v18_records = 0;
  for (const auto& [id, data] : v18.loose) {
    const auto file = legacy::read_file(loose_path(v18.backend / "objects", id));
    const auto loose = decode_loose(file);
    const auto layout = record_layout(*bed.key, v1, id, loose.nonce, loose.tag, loose.ciphertext, loose.plain_size);
    EXPECT_NE(layout, RecordLayout::neither);
    (layout == RecordLayout::v19 ? v19_records : v18_records) += 1;
  }
  EXPECT_EQ(v19_records, 3u);
  EXPECT_EQ(v18_records, v18.loose.size() - 3);
  // The store refuses the half-migrated disk, naming the migration.
  {
    store::StoragePoolOptions options;
    options.backends = {{v18.backend, 64 * mib, 0}};
    options.ledger_key = v18.node_local();
    store::StoragePool pool(v18.state, v18.node, NodeId::random(), options, bed.key, bed.pool_services());
    EXPECT_EQ(pool.online_backends(), 0u);
    EXPECT_TRUE(bed.sink->saw("dagda-migrate --state " + v18.state.string()));
  }
  EXPECT_THROW(store::open_state_layout(v18.state, v1), std::runtime_error);

  const auto report = v18.migrate();
  EXPECT_EQ(report.loose_already, 3u);
  EXPECT_EQ(report.loose_migrated, 5u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.ledgers_migrated, 7u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  expect_v19_opens(bed, v18);
}

// A crash between ledgers: after the objects and some ledgers, with the
// markers still v18. The second run finds every object v19, the ledgers
// done under the node-local key and the rest under the domain key.
TEST(MigrateState, ACrashBetweenLedgersIsResumed) {
  StoreBed bed;
  V18State v18(bed);
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      MigrationOptions options;
      options.cache = v18.cache;
      options.after_step = [](const MigrationReport& so_far) {
        if (so_far.ledgers_migrated >= 3) test::child_exit(0);
      };
      (void)migrate_state(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
      test::child_exit(21);
    } catch (...) {
      test::child_exit(22);
    }
  }
  ASSERT_EQ(test::exit_code(child), std::optional<int>(0));
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "dagda-state-layout-going-forward\n");
  const auto report = v18.migrate();
  EXPECT_EQ(report.loose_already, 8u);
  EXPECT_EQ(report.pack_records_already, 5u);
  EXPECT_EQ(report.ledgers_already, 3u);
  EXPECT_EQ(report.ledgers_migrated, 4u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  expect_v19_opens(bed, v18);
}

// A torn tail on a pack and on a journal, and a damaged record: the tail is
// left for the store to truncate, the journal's dropped as its replay would
// drop it, the record listed and left, and the rest migrates.
TEST(MigrateState, TornTailsAndADamagedRecordAreReportedAndTheRestMigrates) {
  StoreBed bed;
  V18State v18(bed);
  // A torn pack tail: half a header.
  legacy::write_file(legacy::pack_path(v18.backend, 2), Bytes(40, 0x5a), true);
  // A damaged loose record: its last byte flipped.
  test::corrupt_object(v18.backend, v18.loose[2].first);
  // A torn journal tail: bytes after the claims' last frame.
  legacy::write_file(v18.state / "retention" / "claims.log", Bytes(7, 0x33), true);
  const auto report = v18.migrate();
  EXPECT_EQ(report.pack_tail_bytes, 40u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.loose_migrated, 7u);
  EXPECT_EQ(report.loose_unreadable, 1u);
  ASSERT_EQ(report.unreadable.size(), 1u);
  EXPECT_NE(report.unreadable[0].find(store::to_string(v18.loose[2].first)), std::string::npos);
  EXPECT_EQ(report.ledger_tail_bytes_dropped, 7u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  EXPECT_NE(report.summary().find("left as it was"), std::string::npos);
  // The damaged one stays damaged, the rest open.
  const auto damaged = v18.loose[2];
  v18.loose.erase(v18.loose.begin() + 2);
  expect_v19_opens(bed, v18);
  store::StoragePoolOptions options;
  options.backends = {{v18.backend, 64 * mib, 0}};
  options.ledger_key = v18.node_local();
  store::StoragePool pool(v18.state, v18.node, NodeId::random(), options, bed.key, bed.pool_services());
  ASSERT_EQ(pool.online_backends(), 1u);
  EXPECT_FALSE(pool.valid(damaged.first));
}

// The tool: usage, --version, a run that migrates (its evidence on
// stdout), a second that finds nothing to do, and a refusal with its reason
// on stderr and exit code 1.
TEST(MigrateTool, DagdaMigrateRunsTheMigration) {
  StoreBed bed;
  V18State v18(bed);
  const std::string tool = std::string(DAGDA_BINARY_DIR) + "/dagda-migrate";
  ASSERT_TRUE(std::filesystem::exists(tool));
  ::setenv("DAGDA_TEST_PASSPHRASE", "dagda", 1);
  const auto deadline = [] { return std::chrono::steady_clock::now() + 60s; };
  const auto run = [&](std::vector<std::string> arguments, const std::string& log) {
    arguments.insert(arguments.begin(), tool);
    dannan::test::ChildProcess child(arguments, bed.dir.path() / log);
    const auto output = child.read_all(deadline());
    const auto code = child.wait(deadline());
    return std::make_pair(code, output);
  };
  const std::vector<std::string> common{
      "--state",          v18.state.string(),      "--node-state", v18.node_state.string(), "--cluster-key", v18.cluster_key_file.string(),
      "--passphrase-env", "DAGDA_TEST_PASSPHRASE", "--backend",    v18.backend.string(),    "--cache",       v18.cache.string()};
  {
    const auto [code, output] = run({"--version"}, "version.log");
    EXPECT_EQ(code, std::optional<int>(0));
    EXPECT_EQ(output.substr(0, 6), "0.1.5 ");
  }
  {
    const auto [code, output] = run({}, "usage.log");
    EXPECT_EQ(code, std::optional<int>(2));
    EXPECT_NE(read_text(bed.dir.path() / "usage.log").find("usage: dagda-migrate"), std::string::npos);
  }
  {
    auto wrong = common;
    wrong[5] = (v18.root / "wrong.key").string();
    (void)cluster_key_from(v18.root / "wrong.key", "another cluster\n");
    const auto [code, output] = run(wrong, "wrong.log");
    EXPECT_EQ(code, std::optional<int>(1));
    EXPECT_NE(read_text(bed.dir.path() / "wrong.log").find("wrong cluster key"), std::string::npos);
    EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  }
  {
    // --node-state must hold the id the disks carry, and is never given one.
    const auto id_file = v18.node_state / "node-id";
    const auto id_text = read_text(id_file);
    std::filesystem::remove(id_file);
    auto [code, output] = run(common, "no-id.log");
    EXPECT_EQ(code, std::optional<int>(1));
    EXPECT_NE(read_text(bed.dir.path() / "no-id.log").find("no node-id"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(id_file));
    std::ofstream(id_file) << dannan::identity::to_string(filled(0x01)) << "\n";
    std::tie(code, output) = run(common, "other-id.log");
    EXPECT_EQ(code, std::optional<int>(1));
    EXPECT_NE(read_text(bed.dir.path() / "other-id.log").find("belongs to node"), std::string::npos);
    std::ofstream(id_file, std::ios::trunc) << id_text;
    EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  }
  {
    const auto [code, output] = run(common, "run.log");
    EXPECT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / "run.log");
    EXPECT_NE(output.find("loose objects: 8 migrated"), std::string::npos) << output;
    EXPECT_NE(output.find("pack records: 5 migrated"), std::string::npos) << output;
    EXPECT_NE(output.find("ledgers: 7 re-sealed"), std::string::npos) << output;
    EXPECT_NE(output.find("markers rewritten: 3"), std::string::npos) << output;
  }
  {
    const auto [code, output] = run(common, "again.log");
    EXPECT_EQ(code, std::optional<int>(0));
    EXPECT_NE(output.find("nothing to do"), std::string::npos) << output;
  }
  {
    // Back, then forward again.
    auto back = common;
    back.emplace_back("--reverse");
    const auto [code, output] = run(back, "back.log");
    EXPECT_EQ(code, std::optional<int>(0)) << read_text(bed.dir.path() / "back.log");
    EXPECT_NE(output.find("back to layout v18"), std::string::npos) << output;
    EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
    const auto [forward_code, forward] = run(common, "forward.log");
    EXPECT_EQ(forward_code, std::optional<int>(0)) << read_text(bed.dir.path() / "forward.log");
    EXPECT_NE(forward.find("loose objects: 8 migrated"), std::string::npos) << forward;
  }
  expect_v19_opens(bed, v18);
}

}  // namespace
}  // namespace dagda::migrate
