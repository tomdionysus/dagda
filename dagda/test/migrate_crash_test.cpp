//
// Dagda - The Store
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

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dagda/migrate/migrate.h"
#include "dagda/migrate/retag.h"
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
    write_at(pack, pack_tag_offset, torn(old_bytes, new_bytes, cut));

    const auto report = v18.migrate();
    EXPECT_EQ(report.pack_regions_skipped, 0u);
    EXPECT_EQ(report.pack_records_unreadable, 0u);
    EXPECT_EQ(report.pack_records_torn_finished, 1u);
    EXPECT_EQ(report.pack_records_migrated, 5u);
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

}  // namespace
}  // namespace dagda::migrate
