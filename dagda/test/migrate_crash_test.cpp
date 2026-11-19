//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// What a process that dies part-way through dagda-migrate leaves, at every step
// the migration has, in both directions. A power cut is the nodes' to show; here
// a child ends the process at the step, or a test lays out the bytes a write cut
// short would leave. Whatever is left, a second run finishes it, the comparison
// passes and the store reads every byte back.

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/migrate/manifest.h"
#include "dagda/migrate/migrate.h"
#include "dagda/migrate/retag.h"
#include "dagda/test/fixtures/child_exit.h"
#include "dagda/test/fixtures/legacy_v18.h"
#include "dagda/test/fixtures/migrate.h"
#include "dagda/test/fixtures/store.h"

namespace dagda::migrate {
namespace {

using namespace test::v18;
using test::pattern;
using test::StoreBed;
namespace legacy = test::legacy;

void write_at(const std::filesystem::path& path, uint64_t offset, std::span<const uint8_t> bytes) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good()) << path;
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(file.good()) << path;
}

// ---- A write cut short --------------------------------------------------------

// Part of the move's bytes reached the disk: the front of them (a write that
// stopped), or the back (a later sector that got there first). What is on the
// disk is one of the record's two tags in part and the other in part.
struct Cut {
  size_t bytes;
  bool front;
};

Bytes torn(std::span<const uint8_t> old_bytes, std::span<const uint8_t> new_bytes, const Cut& cut) {
  Bytes out(old_bytes.begin(), old_bytes.end());
  if (cut.front) {
    std::copy_n(new_bytes.begin(), cut.bytes, out.begin());
  } else {
    std::copy_n(new_bytes.begin() + static_cast<std::ptrdiff_t>(cut.bytes), new_bytes.size() - cut.bytes, out.begin() + static_cast<std::ptrdiff_t>(cut.bytes));
  }
  return out;
}

// A pack record's 48 bytes (tag, then the checksum after it) cross a sector
// edge for about one record in eighty-five. Torn, the header's checksum fails,
// and the record read as a damaged span with the object in it gone.
TEST(MigrateCrash, APackRecordWhoseWriteWasCutShortIsFinished) {
  const std::vector<Cut> cuts{{1, true},  {8, true},  {15, true}, {16, true},  {17, true},  {30, true},
                              {47, true}, {1, false}, {8, false}, {15, false}, {16, false}, {30, false}};
  for (const auto& cut : cuts) {
    SCOPED_TRACE("cut " + std::to_string(cut.bytes) + (cut.front ? " front" : " back"));
    StoreBed bed;
    V18State v18(bed);
    // The first record of pack 2: forward from v18, the record cut short.
    const auto pack = legacy::pack_path(v18.backend, 2);
    const auto file = legacy::read_file(pack);
    const auto header = decode_pack_header(std::span<const uint8_t>(file).first(pack_header_size)).value();
    TagDeriver deriver(*bed.key);
    auto moved = header;
    moved.tag = deriver.derive(header.tag, associated_data_v18(header.id), v1.associated_data(header.id), header.payload_size);
    const auto old_bytes = std::span<const uint8_t>(file).first(pack_header_size).subspan(pack_tag_offset);
    const auto new_header = encode_pack_header(moved);
    const auto new_bytes = std::span<const uint8_t>(new_header).subspan(pack_tag_offset);
    const auto on_disk = torn(old_bytes, new_bytes, cut);
    write_at(pack, pack_tag_offset, on_disk);
    // The bytes the cut left old can equal the new ones by chance (the last checksum
    // byte, at a cut of 47, about one run in 256): then the record is whole and moved,
    // which is what the run must find.
    const bool whole = std::equal(on_disk.begin(), on_disk.end(), new_bytes.begin(), new_bytes.end());

    const auto report = v18.migrate();
    EXPECT_EQ(report.pack_regions_skipped, 0u);
    EXPECT_EQ(report.pack_records_unreadable, 0u);
    EXPECT_EQ(report.pack_records_torn_finished, whole ? 0u : 1u);
    EXPECT_EQ(report.pack_records_migrated, whole ? 4u : 5u);
    EXPECT_EQ(hex(legacy::read_file(pack)).substr(0, 2 * pack_header_size), hex(new_header));
    expect_v19_opens(bed, v18);
  }
}

// The same going back, from v19.
TEST(MigrateCrash, APackRecordWhoseWriteBackWasCutShortIsFinished) {
  StoreBed bed;
  V18State v18(bed);
  const auto pack = legacy::pack_path(v18.backend, 2);
  const auto as_v18 = legacy::read_file(pack);
  (void)v18.migrate();
  const auto as_v19 = legacy::read_file(pack);
  const auto old_bytes = std::span<const uint8_t>(as_v19).first(pack_header_size).subspan(pack_tag_offset);
  const auto new_bytes = std::span<const uint8_t>(as_v18).first(pack_header_size).subspan(pack_tag_offset);
  write_at(pack, pack_tag_offset, torn(old_bytes, new_bytes, {8, true}));
  MigrationOptions options;
  options.cache = v18.cache;
  const auto report = migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
  EXPECT_EQ(report.pack_records_torn_finished, 1u);
  EXPECT_EQ(report.pack_regions_skipped, 0u);
  EXPECT_EQ(legacy::read_file(pack), as_v18);
}

// A loose object's tag cut short: told apart from rot by trying what the tear
// could have left.
TEST(MigrateCrash, ALooseObjectWhoseWriteWasCutShortIsFinished) {
  for (const auto& cut : std::vector<Cut>{{1, true}, {8, true}, {15, true}, {1, false}, {8, false}, {15, false}}) {
    SCOPED_TRACE("cut " + std::to_string(cut.bytes) + (cut.front ? " front" : " back"));
    StoreBed bed;
    V18State v18(bed);
    const auto& [id, data] = v18.loose[1];
    const auto file = loose_path(v18.backend / "objects", id);
    const auto before = legacy::read_file(file);
    const auto loose = decode_loose(before);
    TagDeriver deriver(*bed.key);
    const auto new_tag = deriver.derive(loose.tag, associated_data_v18(id), v1.associated_data(id), loose.ciphertext.size());
    write_at(file, loose_tag_offset, torn(loose.tag, new_tag, cut));

    const auto report = v18.migrate();
    EXPECT_EQ(report.loose_unreadable, 0u);
    EXPECT_EQ(report.loose_torn_finished, 1u);
    expect_v19_opens(bed, v18);
  }
}

// Rot is not a tear: a byte of the ciphertext flipped stays listed and left.
TEST(MigrateCrash, ARecordThatIsDamagedIsNotTakenForATear) {
  StoreBed bed;
  V18State v18(bed);
  test::corrupt_object(v18.backend, v18.loose[2].first);
  const auto report = v18.migrate();
  EXPECT_EQ(report.loose_unreadable, 1u);
  EXPECT_EQ(report.loose_torn_finished, 0u);
}

// ---- What a crash leaves beside the files --------------------------------------

// durable_replace_file writes `<path>.tmp.<pid>.<n>` and renames it. A crash
// between the two leaves the temporary, as large as the ledger it was replacing,
// and nothing else ever removes it: a second run does.
TEST(MigrateCrash, TheTemporariesACrashLeavesAreRemovedByTheSecondRun) {
  StoreBed bed;
  V18State v18(bed);
  const std::vector<std::filesystem::path> temporaries{
      v18.state / "storage-layout.tmp.4242.0",
      v18.backend / ".macha.backend.tmp.4242.1",
      store::backend_identity_path(v18.state, v18.backend).string() + ".tmp.4242.2",
      v18.state / "retention" / "claims.log.tmp.4242.3",
      v18.state / "retention" / "unreferenced-data.bin.tmp.4242.4",
      v18.state / "ledger" / "control" / "root.tmp.4242.5",
      v18.state / "retention" / "ledger-data" / "journal.log.tmp.4242.6",
  };
  for (const auto& path : temporaries) legacy::write_file(path, bytes_of("half of something"));
  // Not the migration's: what it did not make it leaves.
  const auto not_ours = v18.state / "retention" / "somebody-elses.tmp.4242.7";
  legacy::write_file(not_ours, bytes_of("kept"));

  MigrationOptions dry;
  dry.dry_run = true;
  (void)v18.migrate(dry);
  for (const auto& path : temporaries) EXPECT_TRUE(std::filesystem::exists(path)) << path;

  (void)v18.migrate();
  for (const auto& path : temporaries) EXPECT_FALSE(std::filesystem::exists(path)) << path;
  EXPECT_TRUE(std::filesystem::exists(not_ours));
  expect_v19_opens(bed, v18);
}

// A claims journal holding only the torn front of its first frame (a crash of
// Macha's just after a checkpoint emptied it) holds nothing: the store's replay
// drops it. It is not a ledger under another key, and no run can get past a
// refusal of it.
TEST(MigrateCrash, AJournalHoldingOnlyATornFrameIsNotRefused) {
  StoreBed bed;
  V18State v18(bed);
  // Whole frames out, a short tail in.
  legacy::write_file(v18.state / "retention" / "claims.log", Bytes(20, 0x33));
  // And a ledger with no root that holds the same.
  const auto empty_trie = v18.state / "ledger" / ("data-" + dannan::identity::to_string(filled(0xee)));
  legacy::write_file(empty_trie / "journal.log", Bytes(20, 0x44));
  const auto report = v18.migrate();
  EXPECT_EQ(report.markers_rewritten, 3u);
  ledger::RetentionStore claims(v18.state, v18.node_local(), test::quiet_logger());
  EXPECT_EQ(claims.claims(contract::RetentionClass::data, v18.loose[1].first).adds, (std::map<NodeId, uint64_t>{{v18.origin, 1}}));
  EXPECT_EQ(std::filesystem::file_size(v18.state / "retention" / "claims.log"), 0u);
}

// ---- A backend left out -------------------------------------------------------

// A second backend the state's mirror names, left off the command line. The run
// would finish, mark the state v19, mark the backend's mirror v19, and leave the
// disk v18 with nothing that will ever migrate it: the next run says nothing to
// do. Refused before any write, naming the backend.
TEST(MigrateState, ABackendTheStateKnowsAndTheRunWasNotGivenIsRefused) {
  StoreBed bed;
  V18State v18(bed);
  const auto second = v18.root / "data-2";
  legacy::write_backend_marker_v18(v18.state, second, v18.node, filled(0xce));
  const auto data = pattern(2048, 77);
  legacy::write_loose_v18(second, *bed.key, object_id(data), data);
  const auto before = v18.digest();
  try {
    (void)v18.migrate();
    ADD_FAILURE() << "not refused";
  } catch (const MigrationRefused& error) {
    EXPECT_NE(std::string(error.what()).find(dannan::identity::to_string(filled(0xce))), std::string::npos) << error.what();
    EXPECT_NE(std::string(error.what()).find("--backend"), std::string::npos) << error.what();
  }
  EXPECT_EQ(v18.digest(), before);
  // Given, it migrates, and both markers follow.
  MigrationOptions options;
  options.cache = v18.cache;
  const auto report = migrate_state(v18.state, {v18.backend, second}, v18.cluster(), v18.custody(), options);
  EXPECT_EQ(report.loose_migrated, 9u);
  EXPECT_EQ(report.markers_rewritten, 5u);
  EXPECT_TRUE(read_text(second / ".macha.backend").starts_with("dagda-backend-v19"));
}

// A v19 state given a backend still v18 has something to do, and saying "nothing
// to do" of it would strand the disk.
TEST(MigrateState, AV19StateGivenAV18BackendIsRefusedNotCalledDone) {
  StoreBed bed;
  V18State v18(bed);
  (void)v18.migrate();
  const auto second = v18.root / "data-2";
  legacy::write_backend_marker_v18(v18.state, second, v18.node, filled(0xce), false);
  MigrationOptions options;
  options.cache = v18.cache;
  try {
    (void)migrate_state(v18.state, {v18.backend, second}, v18.cluster(), v18.custody(), options);
    ADD_FAILURE() << "not refused";
  } catch (const MigrationRefused& error) {
    EXPECT_NE(std::string(error.what()).find(second.string()), std::string::npos) << error.what();
  }
}

// ---- A crash on the way back --------------------------------------------------

// Going back rewrites the markers last, so a crash among its records left a
// state marked v19 over records part v18: the store opened it and read the moved
// records as corrupt, and a forward run called it nothing to do. The state says
// it is on its way back from the first write until the last, the store refuses
// it, a forward run refuses it, and only going back finishes it.
TEST(MigrateBack, AStateCrashedOnTheWayBackIsRefusedUntilItIsFinished) {
  struct Stop {};
  StoreBed bed;
  V18State v18(bed);
  (void)v18.migrate();
  size_t seen = 0;
  MigrationOptions options;
  options.cache = v18.cache;
  options.after_step = [&](const MigrationReport&) {
    if (++seen == 3) throw Stop{};
  };
  EXPECT_THROW((void)migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options), Stop);
  options.after_step = {};

  EXPECT_THROW(store::open_state_layout(v18.state, v1), std::runtime_error);
  try {
    (void)v18.migrate();
    ADD_FAILURE() << "a forward run did not refuse";
  } catch (const MigrationRefused& error) {
    EXPECT_NE(std::string(error.what()).find("--reverse"), std::string::npos) << error.what();
  }

  const auto report = migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
  EXPECT_EQ(report.loose_already, 3u);
  EXPECT_EQ(read_text(v18.state / "storage-layout"), "macha-state-layout-v18\n");
  EXPECT_EQ(v18.migrate().loose_migrated, 8u);
  expect_v19_opens(bed, v18);
}

// ---- Every step, a process that dies there ----------------------------------------

// The steps migrate_state has, in its order, each one durable before the next:
// 5 loose objects, 7 pack records of which 5 are puts (2 packs); 2 loose objects
// of the control store; 1 of the cache; claims.log, the 2 sightings files and 6
// tries; the backend's marker, its mirror, the state's marker. The count is pinned
// below, so a step added to the migration fails here until it has its cases.
constexpr size_t forward_steps = 5 + 5 + 2 + 1 + 1 + 2 + 6 + 3;

struct CrashBed {
  CrashBed() : v18(bed), manifests(bed.dir.path() / "manifests") {
    std::filesystem::create_directories(manifests);
    legacy::write_file(v18.state / "retention" / "unreferenced-data.bin", ledger::encode_unreferenced_since({{v18.loose[2].first, 1'000}}));
    legacy::write_file(v18.state / "retention" / "unreferenced-control.bin", ledger::encode_unreferenced_since({{v18.control_objects[1].first, 2'000}}));
    pass("before");
  }

  void pass(const std::string& name) const {
    ManifestOptions options;
    options.backends = {v18.backend};
    options.cache = v18.cache;
    options.node = v18.node;
    options.metadata = std::make_shared<FilledMetadata>();
    (void)write_manifest(manifests / name, v18.state, v18.cluster(), v18.custody(), options);
  }

  MigrationOptions run_options() const {
    MigrationOptions options;
    options.cache = v18.cache;
    options.batch_records = batch;
    return options;
  }
  MigrationReport back() const { return migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), run_options()); }
  MigrationReport forward() const { return migrate_state(v18.state, {v18.backend}, v18.cluster(), v18.custody(), run_options()); }

  // A run's batch and resume window (MigrationOptions::batch_records); two, so a run over
  // these few records meets both.
  uint64_t batch = 256;

  // A child that runs `direction` and ends the process when `step` steps have
  // been done, as a kill would, with no teardown. Secure memory is not inherited
  // across a fork: the child derives its own keys from the files. True if it died
  // there; false if the run ended first, having done everything.
  bool die_after(size_t step, bool going_back) const {
    const auto child = ::fork();
    if (child < 0) {
      ADD_FAILURE() << "cannot fork";
      return false;
    }
    if (child == 0) {
      try {
        auto options = run_options();
        size_t seen = 0;
        options.after_step = [&seen, step](const MigrationReport&) {
          if (++seen == step) test::child_exit(0);
        };
        if (going_back)
          (void)migrate_state_back(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
        else
          (void)migrate_state(v18.state, {v18.backend}, v18.cluster(), v18.custody(), options);
        test::child_exit(21);
      } catch (...) {
        test::child_exit(22);
      }
    }
    const auto code = test::exit_code(child);
    EXPECT_TRUE(code == std::optional<int>(0) || code == std::optional<int>(21)) << "the child ended with " << code.value_or(-1);
    return code == std::optional<int>(0);
  }

  // Compared with the manifest from before the first run: every object, ledger
  // and marker by name, and nothing lost.
  void expect_comparison_passes(uint32_t after_layout, const std::string& name) const {
    pass(name);
    ComparisonOptions options;
    options.after_layout = after_layout;
    const auto report = compare_manifests(manifests / "before", manifests / name, options);
    EXPECT_TRUE(report.passed()) << report.summary();
  }

  StoreBed bed;
  V18State v18;
  const std::filesystem::path manifests;
};

// At every step, forward: the disk left is finished by a second run, the
// comparison passes and the store reads every byte back.
TEST(MigrateCrashPhases, AProcessThatDiesAfterAnyStepIsFinishedByASecondRun) {
  size_t died = 0;
  for (size_t step = 1;; ++step) {
    SCOPED_TRACE("died after step " + std::to_string(step));
    CrashBed crash;
    if (!crash.die_after(step, false)) break;
    ++died;
    const auto report = crash.v18.migrate();
    // Every record is accounted for, moved now or before.
    EXPECT_EQ(report.loose_unreadable + report.pack_records_unreadable, 0u);
    if (!report.already_v19) {
      EXPECT_EQ(report.loose_migrated + report.loose_already, 8u);
      EXPECT_EQ(report.pack_records_migrated + report.pack_records_already, 5u);
    }
    crash.expect_comparison_passes(19, "after");
    expect_v19_opens(crash.bed, crash.v18);
  }
  EXPECT_EQ(died, forward_steps);
}

// At every step, forward, and then taken back instead of finished: as Macha wrote
// it, by the same comparison, tags included; and forward again from there.
TEST(MigrateCrashPhases, AProcessThatDiesAfterAnyStepCanBeTakenBack) {
  size_t died = 0;
  for (size_t step = 1;; ++step) {
    SCOPED_TRACE("died after step " + std::to_string(step));
    CrashBed crash;
    if (!crash.die_after(step, false)) break;
    ++died;
    (void)crash.back();
    EXPECT_EQ(read_text(crash.v18.state / "storage-layout"), "macha-state-layout-v18\n");
    crash.expect_comparison_passes(18, "back");
    (void)crash.v18.migrate();
    crash.expect_comparison_passes(19, "forward");
  }
  EXPECT_EQ(died, forward_steps);
}

// At every step of going back: a second run going back finishes it.
TEST(MigrateCrashPhases, AProcessThatDiesGoingBackAfterAnyStepIsFinishedByASecondRun) {
  size_t died = 0;
  for (size_t step = 1;; ++step) {
    SCOPED_TRACE("died after step " + std::to_string(step));
    CrashBed crash;
    (void)crash.v18.migrate();
    if (!crash.die_after(step, true)) break;
    ++died;
    (void)crash.back();
    EXPECT_EQ(read_text(crash.v18.state / "storage-layout"), "macha-state-layout-v18\n");
    crash.expect_comparison_passes(18, "back");
  }
  EXPECT_EQ(died, forward_steps);
}

// The second run dies too, somewhere in each phase; a third finishes. Steps are
// the second run's own count: it has fewer to do.
TEST(MigrateCrashPhases, ACrashDuringTheSecondRunIsFinishedByAThird) {
  size_t cases = 0;
  for (const size_t first : std::vector<size_t>{1, 3, 5, 8, 10, 13, 14, 16, 19, 22, 23, 24}) {
    for (const size_t second : std::vector<size_t>{1, 2, 5, 9}) {
      SCOPED_TRACE("died after step " + std::to_string(first) + ", then after " + std::to_string(second));
      CrashBed crash;
      ASSERT_TRUE(crash.die_after(first, false));
      if (!crash.die_after(second, false)) continue;
      ++cases;
      (void)crash.v18.migrate();
      crash.expect_comparison_passes(19, "after");
      expect_v19_opens(crash.bed, crash.v18);
    }
  }
  EXPECT_GE(cases, 30u);
}

// Three in a row going back, from a crash going forward.
TEST(MigrateCrashPhases, ACrashDuringASecondRunGoingBackIsFinishedByAThird) {
  for (const size_t first : std::vector<size_t>{3, 14, 23}) {
    SCOPED_TRACE(first);
    CrashBed crash;
    ASSERT_TRUE(crash.die_after(first, false));
    (void)crash.die_after(2, true);
    (void)crash.back();
    crash.expect_comparison_passes(18, "back");
  }
}

// Died before any write: after the refusals, as the first line goes out. Nothing
// on the disk moved, no step was taken, and the second run does the lot.
TEST(MigrateCrashPhases, ASecondRunWhoseFirstDiedBeforeAnyWriteDoesEverything) {
  CrashBed crash;
  const auto before = crash.v18.digest();
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      auto options = crash.run_options();
      options.say = [](std::string_view) { test::child_exit(0); };
      (void)migrate_state(crash.v18.state, {crash.v18.backend}, crash.v18.cluster(), crash.v18.custody(), options);
      test::child_exit(21);
    } catch (...) {
      test::child_exit(22);
    }
  }
  ASSERT_EQ(test::exit_code(child), std::optional<int>(0));
  EXPECT_EQ(crash.v18.digest(), before);
  const auto report = crash.v18.migrate();
  EXPECT_EQ(report.loose_migrated, 8u);
  EXPECT_EQ(report.loose_already, 0u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  EXPECT_EQ(report.ledgers_migrated, 9u);
  EXPECT_EQ(report.markers_rewritten, 3u);
  crash.expect_comparison_passes(19, "after");
  expect_v19_opens(crash.bed, crash.v18);
}

// ---- Inside a ledger's move: what the steps above cannot stop between ---------------

// A trie's journal folded into its nodes, under the old key, and no further: the
// checkpoint that opens ObjectTrie::rekey.
TEST(MigrateCrashPhases, ATrieStoppedAfterItsCheckpointIsFinished) {
  CrashBed crash;
  {
    ledger::ObjectTrie trie(crash.v18.state / "retention" / "ledger-data", crash.bed.key, {}, test::quiet_logger());
    trie.checkpoint();
  }
  (void)crash.v18.migrate();
  crash.expect_comparison_passes(19, "after");
  expect_v19_opens(crash.bed, crash.v18);
}

std::vector<std::filesystem::path> node_files(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
    if (entry.path().filename().string().starts_with("nodes-")) out.push_back(entry.path());
  return out;
}

// The next generation's node file part written under the new key, the root not
// yet published: the trie is whole under the old key, and the part is removed.
TEST(MigrateCrashPhases, ATrieStoppedPartWayThroughItsNextGenerationIsFinished) {
  CrashBed crash;
  const auto dir = crash.v18.state / "ledger" / ("data-" + dannan::identity::to_string(crash.v18.token));
  const auto current = node_files(dir).at(0);
  const auto name = current.filename().string();
  const auto generation = std::stoull(name.substr(6, name.size() - 10));
  legacy::write_file(dir / ("nodes-" + std::to_string(generation + 1) + ".bin"), Bytes(4000, 0x5e));
  (void)crash.v18.migrate();
  crash.expect_comparison_passes(19, "after");
  EXPECT_EQ(node_files(dir).size(), 1u);
  expect_v19_opens(crash.bed, crash.v18);
}

// The root published under the new key, the old node file not yet removed. The
// second run finds it done; the stale file is no ledger's and costs nothing the
// next open does not take back.
TEST(MigrateCrashPhases, ATrieStoppedBeforeRemovingItsOldNodesIsDone) {
  CrashBed crash;
  const auto dir = crash.v18.state / "ledger" / "control";
  const auto old_file = node_files(dir).at(0);
  const auto old_bytes = legacy::read_file(old_file);
  {
    ledger::ObjectTrie trie(dir, crash.bed.key, {}, test::quiet_logger());
    trie.rekey(crash.v18.node_local());
  }
  legacy::write_file(old_file, old_bytes);
  const auto report = crash.v18.migrate();
  EXPECT_EQ(report.ledgers_already, 1u);
  crash.expect_comparison_passes(19, "after");
  expect_v19_opens(crash.bed, crash.v18);
  EXPECT_FALSE(std::filesystem::exists(old_file));
}

// The bytes of a pack record cut short and a kill, together: the comparison holds
// the second run to every record.
TEST(MigrateCrashPhases, AWriteCutShortAmongRecordsAlreadyMovedIsFinished) {
  CrashBed crash;
  ASSERT_TRUE(crash.die_after(7, false));
  // Five loose done and two of pack 1's and 2's puts: the next is pack 2's second.
  const auto pack = legacy::pack_path(crash.v18.backend, 2);
  const auto file = legacy::read_file(pack);
  const auto first = decode_pack_header(std::span<const uint8_t>(file).first(pack_header_size)).value();
  const uint64_t offset = pack_header_size + first.payload_size;
  const auto header = decode_pack_header(std::span<const uint8_t>(file).subspan(offset, pack_header_size)).value();
  TagDeriver deriver(*crash.bed.key);
  auto moved = header;
  moved.tag = deriver.derive(header.tag, associated_data_v18(header.id), v1.associated_data(header.id), header.payload_size);
  const auto new_header = encode_pack_header(moved);
  const auto new_bytes = std::span<const uint8_t>(new_header).subspan(pack_tag_offset);
  write_at(pack, offset + pack_tag_offset, torn(std::span<const uint8_t>(file).subspan(offset + pack_tag_offset, 48), new_bytes, {20, true}));
  const auto report = crash.v18.migrate();
  EXPECT_EQ(report.pack_records_torn_finished, 1u);
  EXPECT_EQ(report.pack_regions_skipped, 0u);
  crash.expect_comparison_passes(19, "after");
  expect_v19_opens(crash.bed, crash.v18);
}

// A journal's torn tail, a trie's and the claims', after the first pass: what
// replay drops, the migration drops, and no record went with it.
TEST(MigrateCrashPhases, TornJournalTailsAreDroppedAndNothingWithThem) {
  CrashBed crash;
  legacy::write_file(crash.v18.state / "retention" / "claims.log", Bytes(9, 0x33), true);
  legacy::write_file(crash.v18.state / "retention" / "ledger-data" / "journal.log", Bytes(9, 0x44), true);
  const auto report = crash.v18.migrate();
  EXPECT_EQ(report.ledger_tail_bytes_dropped, 9u);
  crash.expect_comparison_passes(19, "after");
  expect_v19_opens(crash.bed, crash.v18);
}

// What a crash left beside a file being replaced: gone after the second run, and
// never counted as a difference, whichever pass saw it.
TEST(MigrateCrashPhases, TemporariesLeftByACrashAreNotADifference) {
  CrashBed crash;
  ASSERT_TRUE(crash.die_after(14, false));
  legacy::write_file(crash.v18.state / "retention" / "claims.log.tmp.4242.0", bytes_of("half of a journal"));
  legacy::write_file(crash.v18.state / "storage-layout.tmp.4242.1", bytes_of("half of a marker"));
  (void)crash.v18.migrate();
  EXPECT_FALSE(std::filesystem::exists(crash.v18.state / "retention" / "claims.log.tmp.4242.0"));
  crash.expect_comparison_passes(19, "after");
}

// A crash going forward, then the machine comes back and its service starts the
// old binary. Under a v18 marker Macha opens the disk and reads every record
// already moved as corrupt, and a store with no other copy loses it. From the first
// write to the last the state says it is part way: Macha refuses any marker but its
// own, the store refuses this one, and a run in either direction finishes it.
TEST(MigrateCrashPhases, AStateCrashedGoingForwardIsOpenedByNobody) {
  CrashBed crash;
  ASSERT_TRUE(crash.die_after(3, false));
  EXPECT_NE(read_text(crash.v18.state / "storage-layout"), "macha-state-layout-v18\n");
  try {
    store::open_state_layout(crash.v18.state, v1);
    ADD_FAILURE() << "the store opened it";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("dagda-migrate --state"), std::string::npos) << error.what();
  }
  (void)crash.v18.migrate();
  crash.expect_comparison_passes(19, "after");
}

// A refusal leaves the marker as it found it: nothing has been written.
TEST(MigrateCrashPhases, ARefusedRunLeavesTheMarkerAlone) {
  CrashBed crash;
  store::StorageLock held(crash.v18.backend);
  EXPECT_THROW((void)crash.v18.migrate(), MigrationRefused);
  EXPECT_EQ(read_text(crash.v18.state / "storage-layout"), "macha-state-layout-v18\n");
}

// ---- What the comparison makes of a run that did not finish ---------------------------

// A pass over a disk left part way, by a run nobody finished: refused, naming
// what the state says. A record moved under a v18 marker can only be got at by a
// marker made by hand, and then the comparison names it.
TEST(MigrateCrashPhases, AManifestOfAHalfMigratedStateIsRefusedOrNamesItsRecords) {
  CrashBed crash;
  ASSERT_TRUE(crash.die_after(3, false));
  EXPECT_THROW(crash.pass("half"), MigrationRefused);
  EXPECT_FALSE(std::filesystem::exists(crash.manifests / "half"));
  std::ofstream(crash.v18.state / "storage-layout", std::ios::trunc) << "macha-state-layout-v18\n";
  crash.pass("half");
  const auto report = compare_manifests(crash.manifests / "before", crash.manifests / "half");
  EXPECT_FALSE(report.passed());
  EXPECT_TRUE(report.names("the second manifest is of layout v18, not v19")) << report.summary();
  ComparisonOptions same;
  same.after_layout = 18;
  const auto as_v18 = compare_manifests(crash.manifests / "before", crash.manifests / "half", same);
  EXPECT_FALSE(as_v18.passed());
  EXPECT_TRUE(as_v18.names("is v19 in a manifest of layout v18: not migrated")) << as_v18.summary();
}

// ---- Moved from their headers (TODO/2026-11-19-migrate-speed.md) --------------------------

// A whole v18 state is moved from its records' headers: only the first record of each
// store's loose objects and of each pack is read whole (to check the record), the rest
// from their headers, and the record of where the run got to is gone at the end. The
// comparison passes and the store reads every byte back.
TEST(MigrateHeaderOnly, AWholeStateIsMovedFromItsHeaders) {
  CrashBed crash;
  const auto report = crash.forward();
  EXPECT_EQ(report.loose_migrated, 8u);
  EXPECT_EQ(report.pack_records_migrated, 5u);
  // Of 8 loose objects in three stores and 5 puts in two packs, at most five read whole.
  EXPECT_GE(report.records_header_only, 8u);
  EXPECT_EQ(report.loose_unreadable + report.pack_records_unreadable, 0u);
  EXPECT_FALSE(std::filesystem::exists(crash.v18.state / progress_record_name));
  crash.expect_comparison_passes(19, "after");
  expect_v19_opens(crash.bed, crash.v18);
}

// At every step, with a batch of two: the second run passes what the record says is done
// without reading it, reads the next two whole (a tear or a record moved last is found
// there) and moves the rest from their headers.
TEST(MigrateHeaderOnly, AProcessThatDiesAfterAnyStepIsFinishedFromTheRecord) {
  size_t died = 0;
  for (size_t step = 1;; ++step) {
    SCOPED_TRACE("died after step " + std::to_string(step));
    CrashBed crash;
    crash.batch = 2;
    if (!crash.die_after(step, false)) break;
    ++died;
    EXPECT_TRUE(std::filesystem::exists(crash.v18.state / progress_record_name));
    const auto report = crash.forward();
    EXPECT_EQ(report.loose_unreadable + report.pack_records_unreadable, 0u);
    EXPECT_FALSE(std::filesystem::exists(crash.v18.state / progress_record_name));
    crash.expect_comparison_passes(19, "after");
    expect_v19_opens(crash.bed, crash.v18);
  }
  EXPECT_EQ(died, forward_steps);
}

// The same going back, from a whole v19 state.
TEST(MigrateHeaderOnly, AProcessThatDiesGoingBackAfterAnyStepIsFinishedFromTheRecord) {
  size_t died = 0;
  for (size_t step = 1;; ++step) {
    SCOPED_TRACE("died after step " + std::to_string(step));
    CrashBed crash;
    crash.batch = 2;
    (void)crash.forward();
    if (!crash.die_after(step, true)) break;
    ++died;
    (void)crash.back();
    EXPECT_EQ(read_text(crash.v18.state / "storage-layout"), "macha-state-layout-v18\n");
    EXPECT_FALSE(std::filesystem::exists(crash.v18.state / progress_record_name));
    crash.expect_comparison_passes(18, "back");
  }
  EXPECT_EQ(died, forward_steps);
}

// A record that says less than the disk holds (set back by hand here) is caught by the
// record read whole at the start of a batch past the window: from there every record of
// the store is read whole, and nothing is moved twice. Without the check, a record moved
// already and moved again from its header would be back in v18 under a v19 marker.
TEST(MigrateHeaderOnly, ARecordBehindTheDiskIsCaughtAndNothingIsMovedTwice) {
  CrashBed crash;
  crash.batch = 2;
  ASSERT_TRUE(crash.die_after(5, false));
  // The backend's five loose objects are moved; the record says none are.
  std::ofstream(crash.v18.state / progress_record_name, std::ios::trunc) << "dagda-migrate progress 1\nforward\n3\n"
                                                                         << crash.v18.backend.string() << "\n"
                                                                         << (crash.v18.state / "metadata-objects").string() << "\n"
                                                                         << crash.v18.cache.string() << "\n0 0 0 0 end\n";
  std::vector<std::string> said;
  auto options = crash.run_options();
  options.say = [&](std::string_view line) { said.emplace_back(line); };
  const auto report = migrate_state(crash.v18.state, {crash.v18.backend}, crash.v18.cluster(), crash.v18.custody(), options);
  EXPECT_TRUE(std::any_of(said.begin(), said.end(), [](const std::string& line) { return line.find("moved already") != std::string::npos; }));
  EXPECT_EQ(report.loose_unreadable + report.pack_records_unreadable, 0u);
  crash.expect_comparison_passes(19, "after");
  expect_v19_opens(crash.bed, crash.v18);
}

// A state left part way with no record (a run of an earlier version, or the record lost)
// is moved reading every record whole, as before.
TEST(MigrateHeaderOnly, AStateLeftPartWayWithNoRecordIsReadWhole) {
  CrashBed crash;
  ASSERT_TRUE(crash.die_after(3, false));
  std::filesystem::remove(crash.v18.state / progress_record_name);
  const auto report = crash.forward();
  EXPECT_EQ(report.records_header_only, 0u);
  EXPECT_EQ(report.loose_unreadable + report.pack_records_unreadable, 0u);
  crash.expect_comparison_passes(19, "after");
  expect_v19_opens(crash.bed, crash.v18);
}

// A dry run reads every record whole and writes nothing, the record included.
TEST(MigrateHeaderOnly, ADryRunReadsEveryRecordWholeAndWritesNoRecord) {
  CrashBed crash;
  auto options = crash.run_options();
  options.dry_run = true;
  const auto report = migrate_state(crash.v18.state, {crash.v18.backend}, crash.v18.cluster(), crash.v18.custody(), options);
  EXPECT_EQ(report.records_header_only, 0u);
  EXPECT_FALSE(std::filesystem::exists(crash.v18.state / progress_record_name));
  EXPECT_EQ(read_text(crash.v18.state / "storage-layout"), "macha-state-layout-v18\n");
}

}  // namespace
}  // namespace dagda::migrate
