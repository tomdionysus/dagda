//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The tool that says whether a node's data survived the migration. A false pass
// is the worst thing it can do, so most of this is damage: a whole v18 state is
// migrated, one thing is hurt, and the comparison must name that thing and fail.
// Then the opposite direction, the manifest itself damaged, a pass that stops,
// what a pass must not do, and the tool's own output.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/ledger/object_trie.h"
#include "dagda/ledger/reference_counts.h"
#include "dagda/ledger/retention.h"
#include "dagda/ledger/unreferenced_since.h"
#include "dagda/migrate/manifest.h"
#include "dagda/migrate/manifest_format.h"
#include "dagda/migrate/migrate.h"
#include "dagda/test/fixtures/legacy_v18.h"
#include "dagda/test/fixtures/migrate.h"
#include "dagda/test/fixtures/store.h"
#include "dannan/identity/durable_file.h"
#include "dannan/identity/node_identity.h"
#include "dannan/test/fixtures/child_process.h"

namespace dagda::migrate {
namespace {

using namespace std::chrono_literals;
using namespace test::v18;
using test::pattern;
using test::StoreBed;
namespace legacy = test::legacy;

// The state, every category holding something, with the seam for the metadata
// filled so that a pass means a pass, and its manifests beside it (outside the
// node's tree, which is the state's and the disks').
struct Bed {
  Bed() : v18(bed), manifests(bed.dir.path() / "manifests") {
    std::filesystem::create_directories(manifests);
    legacy::write_file(v18.state / "retention" / "unreferenced-data.bin",
                       ledger::encode_unreferenced_since({{v18.loose[2].first, 1'000}, {v18.packed[2].first, 2'000}}));
    legacy::write_file(v18.state / "retention" / "unreferenced-control.bin", ledger::encode_unreferenced_since({{v18.control_objects[1].first, 3'000}}));
    legacy::write_file(v18.backend / ".macha.accounting", accounting(12345));
    legacy::write_file(v18.state / "availability" / "last-survey.bin", bytes_of("a survey"));
    {
      ledger::HeldLedger held(v18.state / "ledger" / "control", bed.key, {}, test::quiet_logger());
      EXPECT_TRUE(held.seeded());
    }
  }

  static Bytes accounting(uint64_t used, uint64_t sequence = 7) {
    store::local_format::AccountingRecord record;
    record.sequence = sequence;
    record.used = used;
    record.operation = store::local_format::accounting_clean;
    auto slot = store::local_format::encode_accounting(record);
    slot.resize(2 * store::local_format::accounting_slot_size);
    return slot;
  }

  ManifestOptions pass_options(std::shared_ptr<MetadataManifestReader> metadata = std::make_shared<FilledMetadata>()) const {
    ManifestOptions options;
    options.backends = {v18.backend};
    options.cache = v18.cache;
    options.node = v18.node;
    options.metadata = std::move(metadata);
    return options;
  }

  ManifestSummary pass(const std::string& name, ManifestOptions options) const {
    return write_manifest(manifests / name, v18.state, v18.cluster(), v18.custody(), options);
  }
  ManifestSummary pass(const std::string& name) const { return pass(name, pass_options()); }

  // Migrated, with a manifest on each side.
  void migrate_with_manifests() {
    (void)pass("before");
    (void)v18.migrate();
    (void)pass("after");
  }

  ComparisonReport compare(const std::string& after = "after") const { return compare_manifests(manifests / "before", manifests / after); }

  ComparisonReport after_damage() {
    (void)pass("damaged");
    return compare("damaged");
  }

  StoreBed bed;
  V18State v18;
  const std::filesystem::path manifests;
};

// Failed, and the line naming the damage is there.
void expect_names(const ComparisonReport& report, std::string_view text) {
  EXPECT_FALSE(report.passed());
  EXPECT_TRUE(report.names(text)) << "no difference names \"" << text << "\" in:\n" << report.summary();
}

// ---- The whole thing -----------------------------------------------------------

// Every category counted, by the numbers the fixture holds: a reader can see each
// was covered, and the result says pass.
TEST(MigrateCompare, AStateMigratedWholePassesWithEveryCategorySized) {
  Bed bed;
  bed.migrate_with_manifests();
  const auto report = bed.compare();
  EXPECT_TRUE(report.passed()) << report.summary();
  const auto sized = [&](const std::string& name, uint64_t count) {
    const auto* category = report.category(name);
    ASSERT_NE(category, nullptr) << name << "\n" << report.summary();
    EXPECT_EQ(category->before, count) << name;
    EXPECT_EQ(category->after, count) << name;
    EXPECT_EQ(category->result(), count ? CategoryResult::same : CategoryResult::zero) << name;
  };
  const auto backend = "backend:" + bed.v18.backend.string();
  const auto control = "control:" + bed.v18.control.string();
  const auto cache = "cache:" + bed.v18.cache.string();
  sized("objects " + backend + " loose", 5);
  sized("objects " + backend + " pack files", 2);
  // Five puts, a touch and a tombstone.
  sized("objects " + backend + " pack records", 7);
  sized("removed " + backend, 1);
  sized("accounting " + backend, 1);
  sized("objects " + control + " loose", 2);
  sized("objects " + control + " pack files", 0);
  sized("objects " + control + " pack records", 0);
  sized("removed " + control, 0);
  sized("objects " + cache + " loose", 1);
  // The state marker, the backend's and its mirror.
  sized("markers", 3);
  sized("retention marker", 1);
  // Control's two; the backend's five loose and four packed, the known-answer
  // object being both.
  sized("held ledger/control", 2);
  sized("held ledger/control markers", 1);
  sized("held ledger/data-" + dannan::identity::to_string(bed.v18.token), 8);
  sized("claims retention/ledger-data", 1);
  sized("claims retention/ledger-control", 1);
  sized("claims retention/claims.log", 1);
  sized("reference counts ledger/referenced/extents", 2);
  sized("reference counts ledger/referenced/nodes", 1);
  sized("reference counts ledger/referenced/state", 1);
  sized("sightings data", 2);
  sized("sightings control", 1);
  sized("other files state", 1);
  sized("metadata certificates", 1);
  sized("metadata heads", 1);
  sized("metadata history", 1);
  EXPECT_NE(report.summary().find("RESULT: PASS"), std::string::npos);
  EXPECT_EQ(report.summary().find("RESULT: FAIL"), std::string::npos);
}

// A pass given no metadata reader leaves the metadata NOT CHECKED and nothing
// passes, however whole the rest. (dagda-migrate always gives one since S7.)
TEST(MigrateCompare, WithNoMetadataReaderTheMetadataIsNotCheckedAndTheComparisonFails) {
  Bed bed;
  const auto options = bed.pass_options(nullptr);
  EXPECT_EQ(options.metadata, nullptr);
  EXPECT_NE(metadata_manifest_reader(), nullptr);
  (void)bed.pass("before", options);
  (void)bed.v18.migrate();
  const auto summary = bed.pass("after", options);
  EXPECT_NE(summary.summary().find("metadata heads: NOT CHECKED"), std::string::npos) << summary.summary();
  const auto report = bed.compare();
  EXPECT_FALSE(report.passed());
  EXPECT_EQ(report.difference_count, 0u) << report.summary();
  EXPECT_EQ(report.not_checked_count, metadata_kinds().size()) << report.summary();
  for (const auto& kind : metadata_kinds()) {
    const auto* category = report.category("metadata " + kind);
    ASSERT_NE(category, nullptr) << kind;
    EXPECT_EQ(category->result(), CategoryResult::not_checked);
  }
  EXPECT_NE(report.summary().find("metadata heads: sized 0 before, 0 after: NOT CHECKED"), std::string::npos) << report.summary();
  EXPECT_NE(report.summary().find("RESULT: FAIL"), std::string::npos);
  EXPECT_EQ(report.summary().find("RESULT: PASS"), std::string::npos);
  // Nor with a reader on one side only.
  (void)bed.pass("after-with-reader");
  EXPECT_FALSE(bed.compare("after-with-reader").passed());
}

// The seam filled: a fact that changes is named.
TEST(MigrateCompare, TheMetadataSeamComparesWhatItIsGiven) {
  Bed bed;
  (void)bed.pass("before");
  (void)bed.v18.migrate();
  (void)bed.pass("after", bed.pass_options(std::make_shared<FilledMetadata>("another head")));
  const auto report = bed.compare();
  EXPECT_EQ(report.not_checked_count, 0u);
  expect_names(report, "metadata heads: head 1 has changed");
}

// A category holding nothing says ZERO in the output: never SAME, never left out.
TEST(MigrateCompare, ACategoryWithNothingIsReportedAsZero) {
  Bed bed;
  std::filesystem::remove(bed.v18.state / "retention" / "unreferenced-control.bin");
  bed.migrate_with_manifests();
  const auto report = bed.compare();
  EXPECT_TRUE(report.passed()) << report.summary();
  EXPECT_NE(report.summary().find("sightings control: sized 0 before, 0 after: ZERO"), std::string::npos) << report.summary();
  EXPECT_EQ(report.category("sightings control")->result(), CategoryResult::zero);
}

// ---- Each thing hurt, by name ---------------------------------------------------

TEST(MigrateCompareDamage, AnObjectLost) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(loose_path(bed.v18.backend / "objects", bed.v18.loose[3].first));
  expect_names(bed.after_damage(), "object " + store::to_string(bed.v18.loose[3].first) + " is lost");
}

TEST(MigrateCompareDamage, AnObjectLostFromControl) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(loose_path(bed.v18.control / "objects", bed.v18.control_objects[0].first));
  const auto report = bed.after_damage();
  expect_names(report, "objects control:");
  expect_names(report, "object " + store::to_string(bed.v18.control_objects[0].first) + " is lost");
}

TEST(MigrateCompareDamage, AnObjectLostFromTheCache) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(loose_path(bed.v18.cache / "objects", bed.v18.cache_objects[0].first));
  const auto report = bed.after_damage();
  expect_names(report, "objects cache:");
  expect_names(report, "object " + store::to_string(bed.v18.cache_objects[0].first) + " is lost");
}

TEST(MigrateCompareDamage, AnObjectsBytesChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  test::corrupt_object(bed.v18.backend, bed.v18.loose[1].first);
  expect_names(bed.after_damage(), "object " + store::to_string(bed.v18.loose[1].first) + " no longer opens: its bytes have changed");
}

TEST(MigrateCompareDamage, APackedObjectsBytesChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  const auto pack = legacy::pack_path(bed.v18.backend, 2);
  auto file = legacy::read_file(pack);
  file[pack_header_size + 3] ^= 0x5a;
  legacy::write_file(pack, file);
  expect_names(bed.after_damage(), "no longer opens: its bytes have changed");
}

TEST(MigrateCompareDamage, APackRecordLost) {
  Bed bed;
  bed.migrate_with_manifests();
  const auto pack = legacy::pack_path(bed.v18.backend, 1);
  auto file = legacy::read_file(pack);
  // The touch off the end.
  file.resize(file.size() - pack_header_size);
  legacy::write_file(pack, file);
  const auto report = bed.after_damage();
  expect_names(report, "is lost (touch of " + store::to_string(bed.v18.known));
  expect_names(report, "pack 1 is");
}

TEST(MigrateCompareDamage, APackLost) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(legacy::pack_path(bed.v18.backend, 2));
  expect_names(bed.after_damage(), "pack 2 is lost");
}

// Written back loose: the tombstone says gone and the object is there.
TEST(MigrateCompareDamage, ARemovedObjectResurrectedByAFile) {
  Bed bed;
  bed.migrate_with_manifests();
  legacy::write_file(loose_path(bed.v18.backend / "objects", bed.v18.tombstoned), bytes_of("any"));
  const auto report = bed.after_damage();
  expect_names(report, "removed object " + store::to_string(bed.v18.tombstoned) + " is resurrected");
  expect_names(report, "object " + store::to_string(bed.v18.tombstoned) + " is new");
}

// Put again after its tombstone, sealed as it should be.
TEST(MigrateCompareDamage, ARemovedObjectResurrectedByAPut) {
  Bed bed;
  bed.migrate_with_manifests();
  store::local_format::PackHeader header;
  const auto data = pattern(700, 13);
  const auto sealed = store::seal_object(*bed.bed.key, v1, bed.v18.tombstoned, data);
  header.type = pack_put;
  header.touched_ms = put_ms + 100;
  header.id = bed.v18.tombstoned;
  header.plain_size = data.size();
  header.payload_size = sealed.ciphertext.size();
  header.nonce = sealed.nonce;
  header.tag = sealed.tag;
  auto record = encode_pack_header(header);
  record.insert(record.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  legacy::write_file(legacy::pack_path(bed.v18.backend, 2), record, true);
  expect_names(bed.after_damage(), "removed object " + store::to_string(bed.v18.tombstoned) + " is resurrected");
}

TEST(MigrateCompareDamage, ATouchedTimeChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  const auto file = loose_path(bed.v18.backend / "objects", bed.v18.loose[1].first);
  std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::hours(1));
  expect_names(bed.after_damage(), "object " + store::to_string(bed.v18.loose[1].first) + " touched time moved");
}

TEST(MigrateCompareDamage, AHeldRecordLost) {
  Bed bed;
  bed.migrate_with_manifests();
  {
    ledger::ObjectTrie trie(bed.v18.state / "ledger" / "control", bed.v18.node_local(), {}, test::quiet_logger());
    trie.apply(std::vector<ledger::ObjectTrie::Change>{{bed.v18.control_objects[0].first, std::nullopt}});
  }
  expect_names(bed.after_damage(), "object " + store::to_string(bed.v18.control_objects[0].first) + " is no longer held");
}

TEST(MigrateCompareDamage, AClaimLost) {
  Bed bed;
  bed.migrate_with_manifests();
  {
    ledger::ObjectTrie trie(bed.v18.state / "retention" / "ledger-data", bed.v18.node_local(), {}, test::quiet_logger());
    trie.apply(std::vector<ledger::ObjectTrie::Change>{{bed.v18.loose[1].first, std::nullopt}});
  }
  expect_names(bed.after_damage(),
               "claim on " + store::to_string(bed.v18.loose[1].first) + " is lost: adds {" + dannan::identity::to_string(bed.v18.origin) + ":1}");
}

TEST(MigrateCompareDamage, ADotChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  {
    ledger::retention_format::ObjectState state;
    state.adds[bed.v18.origin] = 99;
    ledger::ObjectTrie trie(bed.v18.state / "retention" / "ledger-data", bed.v18.node_local(), {}, test::quiet_logger());
    trie.apply(std::vector<ledger::ObjectTrie::Change>{{bed.v18.loose[1].first, ledger::retention_format::encode_state(state)}});
  }
  const auto origin = dannan::identity::to_string(bed.v18.origin);
  expect_names(bed.after_damage(), "changed from adds {" + origin + ":1} removed {} to adds {" + origin + ":99} removed {}");
}

// A claim that lives in the journal: a frame gone is named.
TEST(MigrateCompareDamage, AClaimsJournalFrameLost) {
  Bed bed;
  bed.migrate_with_manifests();
  dannan::identity::durable_replace_file(bed.v18.state / "retention" / "claims.log", {});
  expect_names(bed.after_damage(), "frame 0 is lost: add class");
}

TEST(MigrateCompareDamage, AReferenceCountChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  {
    Bytes three(8, 0);
    three[7] = 3;
    ledger::ObjectTrie trie(bed.v18.state / "ledger" / "referenced" / "extents", bed.v18.node_local(), {}, test::quiet_logger());
    trie.apply(std::vector<ledger::ObjectTrie::Change>{{bed.v18.loose[1].first, three}});
  }
  expect_names(bed.after_damage(), "count of " + store::to_string(bed.v18.loose[1].first) + " changed from 2 to 3");
}

TEST(MigrateCompareDamage, TheReferenceCountsRootChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  auto bytes = legacy::read_file(bed.v18.state / "ledger" / "referenced" / "state");
  bytes[8] = 0;
  legacy::write_file(bed.v18.state / "ledger" / "referenced" / "state", bytes);
  expect_names(bed.after_damage(), "reference counts ledger/referenced/state: state has changed");
}

TEST(MigrateCompareDamage, ASightingChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  legacy::write_file(bed.v18.state / "retention" / "unreferenced-data.bin",
                     ledger::seal_unreferenced_since(*bed.v18.node_local(), {{bed.v18.loose[2].first, 1'001}, {bed.v18.packed[2].first, 2'000}}));
  expect_names(bed.after_damage(), "sighting of " + store::to_string(bed.v18.loose[2].first) + " changed from 1000 to 1001");
}

TEST(MigrateCompareDamage, ASightingLost) {
  Bed bed;
  bed.migrate_with_manifests();
  legacy::write_file(bed.v18.state / "retention" / "unreferenced-data.bin",
                     ledger::seal_unreferenced_since(*bed.v18.node_local(), {{bed.v18.loose[2].first, 1'000}}));
  expect_names(bed.after_damage(), "sighting of " + store::to_string(bed.v18.packed[2].first) + " is lost");
}

TEST(MigrateCompareDamage, TheBackendMarkerLost) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(bed.v18.backend / ".macha.backend");
  expect_names(bed.after_damage(), "marker backend backend:" + bed.v18.backend.string() + " is lost");
}

TEST(MigrateCompareDamage, TheMirrorMarkerLost) {
  Bed bed;
  bed.migrate_with_manifests();
  const auto mirror = store::backend_identity_path(bed.v18.state, bed.v18.backend);
  std::filesystem::remove(mirror);
  expect_names(bed.after_damage(), "marker mirror " + mirror.filename().string() + " is lost");
}

TEST(MigrateCompareDamage, TheRetentionMarkerLost) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(bed.v18.state / "retention" / "ledger");
  expect_names(bed.after_damage(), "retention marker: retention/ledger is lost");
}

TEST(MigrateCompareDamage, TheSeededMarkerLost) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(bed.v18.state / "ledger" / "control" / "seeded");
  expect_names(bed.after_damage(), "held ledger/control markers: seeded is lost");
}

// Back to v18's marker over a v19 disk: what the marker says is checked against
// the layout the manifest was taken under.
TEST(MigrateCompareDamage, TheBackendMarkerSaysAnotherLayout) {
  Bed bed;
  bed.migrate_with_manifests();
  std::ofstream(bed.v18.backend / ".macha.backend", std::ios::trunc) << legacy::backend_marker_v18(bed.v18.node, bed.v18.token);
  expect_names(bed.after_damage(), "not layout v19");
}

TEST(MigrateCompareDamage, TheBackendMarkersKeyCheckFails) {
  Bed bed;
  bed.migrate_with_manifests();
  auto text = read_text(bed.v18.backend / ".macha.backend");
  text[text.size() - 3] = text[text.size() - 3] == '0' ? '1' : '0';
  std::ofstream(bed.v18.backend / ".macha.backend", std::ios::trunc) << text;
  expect_names(bed.after_damage(), "not layout v19");
}

TEST(MigrateCompareDamage, AnAccountingFigureChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  legacy::write_file(bed.v18.backend / ".macha.accounting", Bed::accounting(99999));
  expect_names(bed.after_damage(), "used bytes 12345 before and 99999 after");
}

TEST(MigrateCompareDamage, TheAccountingFileLost) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(bed.v18.backend / ".macha.accounting");
  expect_names(bed.after_damage(), "the accounting file is lost");
}

// Removed whole: every record it held is lost, by name, and it says so.
TEST(MigrateCompareDamage, ALedgerMissing) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove_all(bed.v18.state / "ledger" / ("data-" + dannan::identity::to_string(bed.v18.token)));
  const auto report = bed.after_damage();
  expect_names(report, "held ledger/data-" + dannan::identity::to_string(bed.v18.token));
  expect_names(report, "object " + store::to_string(bed.v18.loose[1].first) + " is no longer held");
}

TEST(MigrateCompareDamage, TheClaimsTrieMissing) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove_all(bed.v18.state / "retention" / "ledger-control");
  expect_names(bed.after_damage(), "claim on " + store::to_string(bed.v18.control_objects[0].first) + " is lost");
}

// A ledger left under the old key: the pass cannot read it, and says so.
TEST(MigrateCompareDamage, ALedgerUnderTheWrongKeyIsNotChecked) {
  Bed bed;
  bed.migrate_with_manifests();
  {
    ledger::ObjectTrie trie(bed.v18.state / "ledger" / "control", bed.v18.node_local(), {}, test::quiet_logger());
    trie.rekey(test::key_of(0x7a));
  }
  const auto report = bed.after_damage();
  EXPECT_FALSE(report.passed());
  EXPECT_GE(report.not_checked_count, 1u);
  EXPECT_NE(report.summary().find("NOT CHECKED not checked: held ledger/control: does not read under the node-local key"), std::string::npos)
      << report.summary();
  expect_names(report, "is no longer held");
}

// A file nothing else covers, byte for byte.
TEST(MigrateCompareDamage, AnotherFileUnderTheStateChanged) {
  Bed bed;
  bed.migrate_with_manifests();
  legacy::write_file(bed.v18.state / "availability" / "last-survey.bin", bytes_of("another survey"));
  expect_names(bed.after_damage(), "other files state: file availability/last-survey.bin has changed");
}

TEST(MigrateCompareDamage, AnotherFileUnderTheStateLost) {
  Bed bed;
  bed.migrate_with_manifests();
  std::filesystem::remove(bed.v18.state / "availability" / "last-survey.bin");
  expect_names(bed.after_damage(), "file availability/last-survey.bin is lost");
}

// Not migrated at all: every record named.
TEST(MigrateCompareDamage, AnObjectLeftUnmigratedIsNamed) {
  Bed bed;
  bed.migrate_with_manifests();
  const auto& [id, data] = bed.v18.loose[2];
  legacy::write_loose_v18(bed.v18.backend, *bed.bed.key, id, data);
  const auto report = bed.after_damage();
  expect_names(report, "object " + store::to_string(id) + " is v18 in a manifest of layout v19: not migrated");
}

// ---- The opposite direction ------------------------------------------------------

TEST(MigrateCompareDamage, AnUnexpectedExtraObject) {
  Bed bed;
  bed.migrate_with_manifests();
  const auto data = pattern(1000, 200);
  const auto sealed = store::seal_object(*bed.bed.key, v1, object_id(data), data);
  auto file = encode_loose_header(data.size(), sealed);
  file.insert(file.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  legacy::write_file(loose_path(bed.v18.backend / "objects", object_id(data)), file);
  expect_names(bed.after_damage(), "object " + store::to_string(object_id(data)) + " is new");
}

TEST(MigrateCompareDamage, AnUnexpectedExtraClaim) {
  Bed bed;
  bed.migrate_with_manifests();
  {
    ledger::retention_format::ObjectState state;
    state.adds[bed.v18.origin] = 5;
    ledger::ObjectTrie trie(bed.v18.state / "retention" / "ledger-data", bed.v18.node_local(), {}, test::quiet_logger());
    trie.apply(std::vector<ledger::ObjectTrie::Change>{{bed.v18.loose[0].first, ledger::retention_format::encode_state(state)}});
  }
  expect_names(bed.after_damage(), "claim on " + store::to_string(bed.v18.loose[0].first) + " is new");
}

TEST(MigrateCompareDamage, AnUnexpectedExtraFile) {
  Bed bed;
  bed.migrate_with_manifests();
  legacy::write_file(bed.v18.state / "something" / "new", bytes_of("hello"));
  expect_names(bed.after_damage(), "something/new is new");
}

// A store named in one pass and not the other.
TEST(MigrateCompareDamage, AStoreLeftOutOfOnePass) {
  Bed bed;
  bed.migrate_with_manifests();
  auto options = bed.pass_options();
  options.cache.clear();
  (void)bed.pass("no-cache", options);
  const auto report = bed.compare("no-cache");
  expect_names(report, "store cache:" + bed.v18.cache.string() + " is in the first manifest alone");
  expect_names(report, "object " + store::to_string(bed.v18.cache_objects[0].first) + " is lost");
}

// The same manifest twice is no migration, and must not read as a pass.
TEST(MigrateCompareDamage, TwoManifestsOfTheSameLayoutAreNotAMigration) {
  Bed bed;
  (void)bed.pass("before");
  (void)bed.pass("again");
  const auto report = bed.compare("again");
  expect_names(report, "the second manifest is of layout v18, not v19");
  ComparisonOptions same;
  same.after_layout = 18;
  EXPECT_TRUE(compare_manifests(bed.manifests / "before", bed.manifests / "again", same).passed());
}

// A backend the state knows and the pass was not told of: a pass over less than
// the node holds is never a pass.
TEST(MigrateCompareDamage, ABackendTheStateKnowsAndThePassWasNotGivenIsNotChecked) {
  Bed bed;
  const auto second = bed.v18.root / "data-2";
  legacy::write_backend_marker_v18(bed.v18.state, second, bed.v18.node, filled(0xce));
  (void)bed.pass("first");
  const auto summary = bed.pass("second");
  EXPECT_NE(summary.summary().find(dannan::identity::to_string(filled(0xce))), std::string::npos) << summary.summary();
  ComparisonOptions same;
  same.after_layout = 18;
  const auto report = compare_manifests(bed.manifests / "first", bed.manifests / "second", same);
  EXPECT_FALSE(report.passed());
  EXPECT_NE(report.summary().find("no --backend does"), std::string::npos) << report.summary();
}

// What could not be read before cannot be proved after: damaged before the
// migration, on both sides, it does not pass.
TEST(MigrateCompareDamage, AnObjectDamagedBeforeTheMigrationIsNotChecked) {
  Bed bed;
  test::corrupt_object(bed.v18.backend, bed.v18.loose[3].first);
  bed.migrate_with_manifests();
  const auto report = bed.compare();
  EXPECT_FALSE(report.passed());
  EXPECT_EQ(report.difference_count, 0u) << report.summary();
  EXPECT_EQ(report.not_checked_count, 1u) << report.summary();
  EXPECT_NE(report.summary().find("object " + store::to_string(bed.v18.loose[3].first) + " opens on neither side"), std::string::npos) << report.summary();
}

// A damaged span in a pack: the objects it held cannot be named, only the span.
TEST(MigrateCompareDamage, ADamagedPackSpanIsNotChecked) {
  Bed bed;
  const auto pack = legacy::pack_path(bed.v18.backend, 2);
  auto file = legacy::read_file(pack);
  file[2] ^= 0xff;
  legacy::write_file(pack, file);
  // The second record has moved nothing, so the damage is the first record's.
  bed.migrate_with_manifests();
  const auto report = bed.compare();
  EXPECT_FALSE(report.passed());
  EXPECT_NE(report.summary().find("what it held cannot be known"), std::string::npos) << report.summary();
}

// A torn tail is what the store's recovery drops: reported, not failed, and
// changed it is a difference.
TEST(MigrateCompare, ATornPackTailIsComparedAndDoesNotFail) {
  Bed bed;
  legacy::write_file(legacy::pack_path(bed.v18.backend, 2), Bytes(40, 0x5a), true);
  bed.migrate_with_manifests();
  EXPECT_TRUE(bed.compare().passed()) << bed.compare().summary();
  legacy::write_file(legacy::pack_path(bed.v18.backend, 2), Bytes(1, 0x5a), true);
  expect_names(bed.after_damage(), "torn tail of 40 bytes");
}

// ---- The manifest itself ----------------------------------------------------------

TEST(MigrateCompareManifest, ATruncatedManifestIsAFailureNotAShorterPass) {
  Bed bed;
  bed.migrate_with_manifests();
  const auto whole = legacy::read_file(bed.manifests / "after");
  ASSERT_TRUE(bed.compare().passed());
  // Cut anywhere: inside the last block, at a block's end, at the very start.
  for (const size_t keep : std::vector<size_t>{whole.size() - 1, whole.size() - 40, whole.size() / 2, 13, 12, 4, 0}) {
    SCOPED_TRACE(keep);
    Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(keep));
    legacy::write_file(bed.manifests / "cut", cut);
    EXPECT_THROW((void)bed.compare("cut"), ManifestDamaged);
  }
  // At every block's end too, where each block's own checksum holds: only the
  // trailer tells.
  size_t offset = 12;
  while (offset < whole.size()) {
    const auto length = manifest_format::number_of(Bytes{0, 0, 0, 0, whole[offset], whole[offset + 1], whole[offset + 2], whole[offset + 3]});
    offset += 4 + length + 32;
    if (offset == whole.size()) break;
    Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(offset));
    legacy::write_file(bed.manifests / "cut", cut);
    EXPECT_THROW((void)bed.compare("cut"), ManifestDamaged) << offset;
  }
}

TEST(MigrateCompareManifest, ABitFlippedOrBytesAddedAreAFailure) {
  Bed bed;
  bed.migrate_with_manifests();
  auto bytes = legacy::read_file(bed.manifests / "after");
  auto flipped = bytes;
  flipped[flipped.size() / 2] ^= 1;
  legacy::write_file(bed.manifests / "flipped", flipped);
  EXPECT_THROW((void)bed.compare("flipped"), ManifestDamaged);
  auto longer = bytes;
  longer.push_back(0);
  legacy::write_file(bed.manifests / "longer", longer);
  EXPECT_THROW((void)bed.compare("longer"), ManifestDamaged);
  EXPECT_THROW((void)bed.compare("nowhere"), ManifestDamaged);
  legacy::write_file(bed.manifests / "not", bytes_of("not a manifest at all"));
  EXPECT_THROW((void)bed.compare("not"), ManifestDamaged);
}

// ---- A pass that stops ---------------------------------------------------------------

// Stopped after each checkpoint in turn, and resumed: the manifest says what one
// pass in one go says, and nothing read twice is more than the checkpoint's worth.
TEST(MigrateComparePass, APassStoppedAtAnyCheckpointResumes) {
  struct Stop {};
  Bed bed;
  auto whole = bed.pass_options();
  whole.checkpoint_records = 1;
  (void)bed.pass("whole", whole);
  ComparisonOptions same;
  same.after_layout = 18;
  size_t stops = 0;
  for (size_t at = 1;; ++at) {
    const auto name = "stopped-" + std::to_string(at);
    size_t seen = 0;
    auto options = whole;
    options.after_checkpoint = [&] {
      if (++seen == at) throw Stop{};
    };
    bool stopped = false;
    try {
      (void)bed.pass(name, options);
    } catch (const Stop&) {
      stopped = true;
    }
    if (!stopped) break;
    ++stops;
    EXPECT_FALSE(std::filesystem::exists(bed.manifests / name));
    EXPECT_TRUE(std::filesystem::exists(bed.manifests / (name + ".partial")));
    // Without --resume it is refused, and the partial stays.
    EXPECT_THROW((void)bed.pass(name, whole), MigrationRefused);
    auto again = whole;
    again.resume = true;
    const auto summary = bed.pass(name, again);
    EXPECT_FALSE(std::filesystem::exists(bed.manifests / (name + ".partial")));
    const auto report = compare_manifests(bed.manifests / "whole", bed.manifests / name, same);
    EXPECT_TRUE(report.passed()) << "stopped at " << at << "\n" << report.summary();
    EXPECT_GE(report.category("objects backend:" + bed.v18.backend.string() + " loose")->after, 5u);
  }
  EXPECT_GE(stops, 13u);
}

// A partial cut through a block goes on from the last block that holds.
TEST(MigrateComparePass, APartialTornAtItsEndResumes) {
  struct Stop {};
  Bed bed;
  auto whole = bed.pass_options();
  whole.checkpoint_records = 1;
  (void)bed.pass("whole", whole);
  size_t seen = 0;
  auto options = whole;
  options.after_checkpoint = [&] {
    if (++seen == 5) throw Stop{};
  };
  EXPECT_THROW((void)bed.pass("torn", options), Stop);
  auto bytes = legacy::read_file(bed.manifests / "torn.partial");
  bytes.resize(bytes.size() - 7);
  legacy::write_file(bed.manifests / "torn.partial", bytes);
  auto again = whole;
  again.resume = true;
  const auto summary = bed.pass("torn", again);
  ComparisonOptions same;
  same.after_layout = 18;
  EXPECT_TRUE(compare_manifests(bed.manifests / "whole", bed.manifests / "torn", same).passed());
}

// The state moved on while a pass was stopped: what the partial holds is no
// longer so, and a manifest built on it would lie.
TEST(MigrateComparePass, AResumeOverAStateThatChangedIsRefused) {
  struct Stop {};
  const auto stop_then = [](const std::function<void(Bed&)>& change) {
    Bed bed;
    auto options = bed.pass_options();
    options.checkpoint_records = 1;
    size_t seen = 0;
    auto stopping = options;
    stopping.after_checkpoint = [&] {
      if (++seen == 10) throw Stop{};
    };
    EXPECT_THROW((void)bed.pass("m", stopping), Stop);
    change(bed);
    options.resume = true;
    EXPECT_THROW((void)bed.pass("m", options), MigrationRefused);
  };
  // A file changed. The time alone is enough.
  stop_then([](Bed& bed) {
    const auto file = loose_path(bed.v18.backend / "objects", bed.v18.loose[0].first);
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::hours(1));
  });
  // A file gone.
  stop_then([](Bed& bed) { std::filesystem::remove(loose_path(bed.v18.backend / "objects", bed.v18.loose[0].first)); });
  // The accounting moved: a node has opened the store.
  stop_then([](Bed& bed) { legacy::write_file(bed.v18.backend / ".macha.accounting", Bed::accounting(1)); });
  // A pack grew.
  stop_then([](Bed& bed) { legacy::append_pack_record_v18(bed.v18.backend, 2, pack_touch, bed.v18.packed[1].first, put_ms + 500); });
}

// ---- What a pass must not do -----------------------------------------------------------

// Nothing is written or created: no file, not a lock file, not a directory.
TEST(MigrateComparePass, APassWritesNothingUnderAnyStore) {
  Bed bed;
  const auto listing = [&] {
    std::map<std::filesystem::path, std::string> out;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(bed.v18.root)) {
      out[entry.path()] = entry.is_regular_file() ? hex(dannan::identity::sha256(legacy::read_file(entry.path()))) : std::string("dir");
    }
    return out;
  };
  const auto times = [&] {
    std::map<std::filesystem::path, std::filesystem::file_time_type> out;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(bed.v18.root)) out[entry.path()] = entry.last_write_time();
    return out;
  };
  // A torn tail and a journal tail, which an open would truncate, and a stale
  // node file, which an open would remove: a pass does neither.
  legacy::write_file(legacy::pack_path(bed.v18.backend, 2), Bytes(40, 0x5a), true);
  legacy::write_file(bed.v18.state / "retention" / "claims.log", Bytes(7, 0x33), true);
  legacy::write_file(bed.v18.state / "ledger" / "control" / "nodes-99.bin", bytes_of("stale"));
  const auto before = listing();
  const auto before_times = times();
  (void)bed.pass("before");
  EXPECT_EQ(listing(), before);
  EXPECT_EQ(times(), before_times);
  EXPECT_FALSE(std::filesystem::exists(bed.v18.state / ".macha.lock"));
  EXPECT_FALSE(std::filesystem::exists(bed.v18.backend / ".macha.lock"));
  // Likewise after the migration.
  (void)bed.v18.migrate();
  const auto migrated = listing();
  (void)bed.pass("after");
  EXPECT_EQ(listing(), migrated);
}

// As the migration: a store a node holds open is refused, every one of them.
TEST(MigrateComparePass, AStoreANodeHoldsOpenIsRefused) {
  Bed bed;
  for (const auto& root : {bed.v18.state, bed.v18.backend, bed.v18.control, bed.v18.cache}) {
    SCOPED_TRACE(root);
    store::StorageLock held(root);
    try {
      (void)bed.pass("m");
      ADD_FAILURE() << "not refused";
    } catch (const MigrationRefused& error) {
      EXPECT_NE(std::string(error.what()).find("is in use"), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(bed.manifests / "m"));
    EXPECT_FALSE(std::filesystem::exists(bed.manifests / "m.partial"));
  }
  // Dannan's lock on the node's state, too.
  {
    dannan::identity::StateLock held(bed.v18.node_state);
    auto options = bed.pass_options();
    options.node_state = bed.v18.node_state;
    EXPECT_THROW((void)bed.pass("m", options), MigrationRefused);
  }
  // Released: it runs, and takes nothing with it.
  (void)bed.pass("m");
  EXPECT_TRUE(std::filesystem::exists(bed.manifests / "m"));
}

// A node that starts while the pass runs, where no lock file was there to hold.
TEST(MigrateComparePass, ALockFileAppearingDuringThePassFailsIt) {
  Bed bed;
  auto options = bed.pass_options();
  options.checkpoint_records = 1;
  options.after_checkpoint = [&] { std::ofstream(bed.v18.backend / ".macha.lock") << ""; };
  EXPECT_THROW((void)bed.pass("m", options), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(bed.manifests / "m"));
}

// A manifest goes where the operator names, never inside what it describes, and
// never over one that exists.
TEST(MigrateComparePass, AManifestInsideAStoreOrOverAnotherIsRefused) {
  Bed bed;
  const auto options = bed.pass_options();
  const auto refused = [&](const std::filesystem::path& where) {
    try {
      (void)write_manifest(where, bed.v18.state, bed.v18.cluster(), bed.v18.custody(), options);
      ADD_FAILURE() << "not refused: " << where;
    } catch (const MigrationRefused& error) {
      EXPECT_NE(std::string(error.what()).find(where.string()), std::string::npos) << error.what();
    }
    EXPECT_FALSE(std::filesystem::exists(where.string() + ".partial"));
  };
  refused(bed.v18.state / "manifest");
  refused(bed.v18.backend / "objects" / "manifest");
  refused(bed.v18.control / "manifest");
  refused(bed.v18.cache / "manifest");
  (void)bed.pass("m");
  const auto before = legacy::read_file(bed.manifests / "m");
  refused(bed.manifests / "m");
  EXPECT_EQ(legacy::read_file(bed.manifests / "m"), before);
}

TEST(MigrateComparePass, AStateNotOfEitherLayoutOrAWrongKeyIsRefused) {
  Bed bed;
  std::ofstream(bed.v18.state / "storage-layout", std::ios::trunc) << "macha-state-layout-v17\n";
  EXPECT_THROW((void)bed.pass("m"), MigrationRefused);
  std::filesystem::remove(bed.v18.state / "storage-layout");
  EXPECT_THROW((void)bed.pass("m"), MigrationRefused);
  legacy::write_state_layout_v18(bed.v18.state);
  const auto wrong = cluster_key_from(bed.bed.dir.path() / "wrong.key", "another cluster\n");
  try {
    (void)write_manifest(bed.manifests / "m", bed.v18.state, wrong, bed.v18.custody(), bed.pass_options());
    ADD_FAILURE() << "not refused";
  } catch (const MigrationRefused& error) {
    EXPECT_NE(std::string(error.what()).find("wrong cluster key"), std::string::npos) << error.what();
  }
}

// Two passes under different keys are not comparable. Their markers and ledgers
// could not read anyway; the header says so at once.
TEST(MigrateCompareDamage, AnotherNodesManifestIsADifference) {
  Bed bed;
  (void)bed.pass("before");
  auto other = bed.pass_options();
  other.node = filled(0x01);
  (void)bed.pass("other", other);
  ComparisonOptions same;
  same.after_layout = 18;
  const auto report = compare_manifests(bed.manifests / "before", bed.manifests / "other", same);
  EXPECT_FALSE(report.passed());
  EXPECT_TRUE(report.names("node " + dannan::identity::to_string(bed.v18.node))) << report.summary();
}

// There and back: objects and markers as they were, with the tags equal.
TEST(MigrateCompare, ThereAndBackComparesEqualUnderLayoutV18) {
  Bed bed;
  (void)bed.pass("before");
  (void)bed.v18.migrate();
  MigrationOptions back;
  back.cache = bed.v18.cache;
  (void)migrate_state_back(bed.v18.state, {bed.v18.backend}, bed.v18.cluster(), bed.v18.custody(), back);
  (void)bed.pass("back");
  ComparisonOptions same;
  same.after_layout = 18;
  const auto report = compare_manifests(bed.manifests / "before", bed.manifests / "back", same);
  EXPECT_TRUE(report.passed()) << report.summary();
}

// ---- The tool --------------------------------------------------------------------------

TEST(MigrateCompareTool, TheToolWritesTheManifestsAndSaysTheResult) {
  Bed bed;
  const std::string tool = std::string(DAGDA_BINARY_DIR) + "/dagda-migrate";
  ASSERT_TRUE(std::filesystem::exists(tool));
  ::setenv("DAGDA_TEST_PASSPHRASE", "dagda", 1);
  const auto deadline = [] { return std::chrono::steady_clock::now() + 60s; };
  const auto run = [&](std::vector<std::string> arguments, const std::string& log) {
    arguments.insert(arguments.begin(), tool);
    dannan::test::ChildProcess child(arguments, bed.bed.dir.path() / log);
    const auto output = child.read_all(deadline());
    const auto code = child.wait(deadline());
    return std::make_pair(code, output);
  };
  const std::vector<std::string> common{
      "--state",          bed.v18.state.string(),  "--node-state", bed.v18.node_state.string(), "--cluster-key", bed.v18.cluster_key_file.string(),
      "--passphrase-env", "DAGDA_TEST_PASSPHRASE", "--backend",    bed.v18.backend.string(),    "--cache",       bed.v18.cache.string()};
  const auto with = [&](std::initializer_list<std::string> more) {
    auto all = common;
    all.insert(all.end(), more);
    return all;
  };
  const auto before = (bed.manifests / "tool-before").string();
  const auto after = (bed.manifests / "tool-after").string();
  {
    const auto [code, output] = run(with({"--write-manifest", before}), "before.log");
    EXPECT_EQ(code, std::optional<int>(0)) << read_text(bed.bed.dir.path() / "before.log");
    EXPECT_NE(output.find("manifest of layout v18"), std::string::npos) << output;
    // The state holds no metadata: every metadata category says so.
    EXPECT_NE(output.find("metadata heads: ZERO"), std::string::npos) << output;
    EXPECT_NE(output.find("objects backend:" + bed.v18.backend.string() + " loose: sized 5"), std::string::npos) << output;
  }
  {
    const auto [code, output] = run(common, "run.log");
    EXPECT_EQ(code, std::optional<int>(0)) << read_text(bed.bed.dir.path() / "run.log");
  }
  {
    const auto [code, output] = run(with({"--write-manifest", after}), "after.log");
    EXPECT_EQ(code, std::optional<int>(0)) << read_text(bed.bed.dir.path() / "after.log");
    EXPECT_NE(output.find("manifest of layout v19"), std::string::npos) << output;
    // Never over one that exists.
    const auto [again, unused] = run(with({"--write-manifest", after}), "again.log");
    EXPECT_EQ(again, std::optional<int>(1));
    EXPECT_NE(read_text(bed.bed.dir.path() / "again.log").find("never overwrites"), std::string::npos);
  }
  {
    // Whole, and the metadata read: a state with none passes, each category ZERO.
    // (Before S7 this said NOT CHECKED and failed: the tool had no reader.)
    const auto [code, output] = run({"--compare-before", before, "--compare-after", after}, "compare.log");
    EXPECT_EQ(code, std::optional<int>(0)) << output;
    EXPECT_NE(output.find("sized 5 before, 5 after: SAME"), std::string::npos) << output;
    EXPECT_NE(output.find("metadata history: sized 0 before, 0 after: ZERO"), std::string::npos) << output;
    EXPECT_NE(output.find("RESULT: PASS"), std::string::npos) << output;
    EXPECT_EQ(output.find("RESULT: FAIL"), std::string::npos) << output;
  }
  {
    // A difference is named on standard output.
    std::filesystem::remove(bed.v18.backend / ".macha.accounting");
    const auto damaged = (bed.manifests / "tool-damaged").string();
    const auto wrote = run(with({"--write-manifest", damaged}), "damaged.log");
    EXPECT_EQ(wrote.first, std::optional<int>(0));
    const auto [code, output] = run({"--compare-before", before, "--compare-after", damaged}, "compare-damaged.log");
    EXPECT_EQ(code, std::optional<int>(1)) << output;
    EXPECT_NE(output.find("DIFFERENT accounting"), std::string::npos) << output;
    EXPECT_NE(output.find("RESULT: FAIL: 1 differences"), std::string::npos) << output;
  }
  {
    // A truncated manifest: exit 1, and it says so.
    auto bytes = legacy::read_file(after);
    bytes.resize(bytes.size() / 2);
    const auto cut = (bed.manifests / "tool-cut").string();
    legacy::write_file(cut, bytes);
    const auto [code, output] = run({"--compare-before", before, "--compare-after", cut}, "compare-cut.log");
    EXPECT_EQ(code, std::optional<int>(1));
    EXPECT_NE(output.find("RESULT: FAIL"), std::string::npos) << output;
  }
  {
    // A usage error.
    const auto [code, output] = run({"--compare-before", before}, "usage.log");
    EXPECT_EQ(code, std::optional<int>(2));
  }
}

// ---- The readers ----------------------------------------------------------------------------

// The trie's reader against the trie itself: every record, a journal unfolded on
// top of saved nodes, a change and an erase in the journal, and the files as they
// were.
TEST(MigrateCompareReaders, TheReadOnlyTrieWalkAgreesWithTheTrie) {
  test::StoreBed bed;
  const auto dir = bed.dir.path() / "trie";
  const auto key = test::key_of(0x21);
  std::vector<ledger::ObjectTrie::Change> changes;
  for (uint64_t n = 0; n < 1000; ++n) {
    const auto data = test::numbered(n);
    changes.push_back({object_id(data), Bytes(8, static_cast<uint8_t>(n))});
  }
  {
    ledger::ObjectTrie trie(dir, key, {}, test::quiet_logger());
    trie.apply(changes);
    trie.checkpoint();
    // In the journal, over the saved nodes: one erased, one changed, one new.
    const auto fresh = pattern(16, 9);
    trie.apply(std::vector<ledger::ObjectTrie::Change>{{changes[0].id, std::nullopt}, {changes[1].id, Bytes{9}}, {object_id(fresh), Bytes{7}}});
  }
  const auto files = legacy::tree_digest(dir);
  std::vector<std::pair<ledger::ObjectId, Bytes>> walked;
  const auto walk = ledger::ObjectTrie::read_only(
      dir, *key, [&](const ledger::ObjectId& id, std::span<const uint8_t> value) { walked.emplace_back(id, Bytes(value.begin(), value.end())); });
  EXPECT_EQ(legacy::tree_digest(dir), files);
  EXPECT_EQ(walk.records, 1000u);
  EXPECT_EQ(walk.journal_frames, 1u);
  EXPECT_FALSE(walk.journal_damaged);
  ledger::ObjectTrie trie(dir, key, {}, test::quiet_logger());
  EXPECT_EQ(walked, trie.next(std::nullopt, 2000));
}

TEST(MigrateCompareReaders, TheReadOnlyTrieWalkRefusesAnotherKeyAndAMissingFile) {
  test::StoreBed bed;
  const auto dir = bed.dir.path() / "trie";
  const auto key = test::key_of(0x21);
  const auto visit = [](const ledger::ObjectId&, std::span<const uint8_t>) {};
  // A trie that is not there is empty, and is not made.
  EXPECT_EQ(ledger::ObjectTrie::read_only(dir, *key, visit).records, 0u);
  EXPECT_FALSE(std::filesystem::exists(dir));
  {
    ledger::ObjectTrie trie(dir, key, {}, test::quiet_logger());
    std::vector<ledger::ObjectTrie::Change> changes{{object_id(pattern(16, 1)), Bytes{1}}};
    trie.apply(changes);
    trie.checkpoint();
    trie.apply(std::vector<ledger::ObjectTrie::Change>{{object_id(pattern(16, 2)), Bytes{2}}});
  }
  EXPECT_ANY_THROW((void)ledger::ObjectTrie::read_only(dir, *test::key_of(0x22), visit));
  for (const auto& entry : std::filesystem::directory_iterator(dir))
    if (entry.path().filename().string().starts_with("nodes-")) std::filesystem::remove(entry.path());
  EXPECT_ANY_THROW((void)ledger::ObjectTrie::read_only(dir, *key, visit));
}

// A journal read leaves a torn tail where it was, and tells it from a frame that
// will not open.
TEST(MigrateCompareReaders, TheReadOnlyJournalScanLeavesTheFileAndTellsATearFromDamage) {
  test::StoreBed bed;
  const auto path = bed.dir.path() / "journal";
  const auto key = test::key_of(0x31);
  const std::array<uint8_t, 8> aad{'T', 'E', 'S', 'T', 'A', 'A', 'D', '1'};
  {
    ledger::SealedJournal journal(path, key, aad, 1 << 20, test::quiet_logger());
    journal.append(bytes_of("one"));
    journal.append(bytes_of("two"));
  }
  const auto good = legacy::read_file(path);
  legacy::write_file(path, Bytes(7, 0x11), true);
  const auto torn = legacy::read_file(path);
  std::vector<std::string> seen;
  const auto visit = [&](std::span<const uint8_t> plain) { seen.emplace_back(plain.begin(), plain.end()); };
  auto scan = ledger::SealedJournal::read_only(path, *key, aad, 1 << 20, visit);
  EXPECT_EQ(seen, (std::vector<std::string>{"one", "two"}));
  EXPECT_EQ(scan.frames, 2u);
  EXPECT_EQ(scan.tail_bytes, 7u);
  EXPECT_FALSE(scan.stopped_at_a_whole_frame);
  EXPECT_EQ(legacy::read_file(path), torn);
  // A whole frame under another key.
  scan = ledger::SealedJournal::read_only(path, *test::key_of(0x32), aad, 1 << 20, visit);
  EXPECT_EQ(scan.frames, 0u);
  EXPECT_TRUE(scan.stopped_at_a_whole_frame);
  // Nothing there, nothing made.
  const auto missing = bed.dir.path() / "missing";
  EXPECT_EQ(ledger::SealedJournal::read_only(missing, *key, aad, 1 << 20, visit).frames, 0u);
  EXPECT_FALSE(std::filesystem::exists(missing));
}

}  // namespace
}  // namespace dagda::migrate
