//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Requirement L2 and layout v18's sealed journal frame.
//
// Moved from Macha's tests/test_object_ledger.cpp (develop 5a06d3c):
// test_a_sealed_journal_replays_what_was_appended_in_order,
// test_a_torn_journal_keeps_what_came_before_the_tear,
// test_a_damaged_or_refused_frame_ends_the_replay_there,
// test_a_reset_journal_is_empty_and_keeps_appending. The known answers were
// computed outside both codebases from Macha's sealed_journal.cpp:59-195:
// AES-256 blocks by `openssl enc -aes-256-ecb`, GHASH by hand, checked
// first against the GCM specification's test case 16.

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dagda/ledger/sealed_journal.h"
#include "dagda/test/fixtures/ledger.h"
#include "dannan/identity/crypto.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::ledger {
namespace {

using test::key_of;
using test::quiet_logger;

constexpr std::array<uint8_t, 8> test_aad{'M', 'A', 'C', 'H', 'T', 'E', 'S', 'T'};

Bytes frame_text(int i) {
  const auto text = "frame-" + std::to_string(i);
  return Bytes(text.begin(), text.end());
}

std::vector<std::string> replay_all(SealedJournal& journal) {
  std::vector<std::string> seen;
  journal.replay([&](std::span<const uint8_t> plain) { seen.emplace_back(plain.begin(), plain.end()); }, 1U << 20, "test journal");
  return seen;
}

Bytes unhex(const std::string& text) { return dannan::identity::unhex(text).value(); }

void write_file(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Macha: test_a_sealed_journal_replays_what_was_appended_in_order.
TEST(LedgerSealedJournal, ReplaysWhatWasAppendedInOrder) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "journal.log";
  {
    SealedJournal journal(path, key_of(1), test_aad, 1U << 20, quiet_logger());
    EXPECT_TRUE(replay_all(journal).empty());
    for (int i = 0; i < 5; ++i) journal.append(frame_text(i));
    EXPECT_EQ(journal.frames(), 5u);
  }
  SealedJournal reopened(path, key_of(1), test_aad, 1U << 20, quiet_logger());
  const auto seen = replay_all(reopened);
  ASSERT_EQ(seen.size(), 5u);
  for (int i = 0; i < 5; ++i) EXPECT_EQ(seen[static_cast<size_t>(i)], "frame-" + std::to_string(i));
  EXPECT_EQ(reopened.frames(), 5u);
  EXPECT_EQ(reopened.bytes(), std::filesystem::file_size(path));
  // Replaying again changes nothing.
  EXPECT_EQ(replay_all(reopened).size(), 5u);
}

// Macha: test_a_torn_journal_keeps_what_came_before_the_tear.
TEST(LedgerSealedJournal, ATornJournalKeepsWhatCameBeforeTheTear) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "journal.log";
  {
    SealedJournal journal(path, key_of(2), test_aad, 1U << 20, quiet_logger());
    for (int i = 0; i < 4; ++i) journal.append(frame_text(i));
  }
  const auto whole = std::filesystem::file_size(path);
  // Every cut inside the last frame keeps the first three, and the file is
  // truncated to them, so the next append follows them.
  SealedJournal probe(path, key_of(2), test_aad, 1U << 20, quiet_logger());
  ASSERT_EQ(replay_all(probe).size(), 4u);
  for (uint64_t cut = 1; cut < whole; cut += 7) {
    dannan::test::TempDir copy_dir;
    const auto copy = copy_dir.path() / "journal.log";
    std::filesystem::copy_file(path, copy);
    std::filesystem::resize_file(copy, cut);
    SealedJournal torn(copy, key_of(2), test_aad, 1U << 20, quiet_logger());
    const auto seen = replay_all(torn);
    EXPECT_LT(seen.size(), 4u);
    EXPECT_EQ(std::filesystem::file_size(copy), torn.bytes());
    torn.append(frame_text(99));
    SealedJournal after(copy, key_of(2), test_aad, 1U << 20, quiet_logger());
    const auto again = replay_all(after);
    ASSERT_EQ(again.size(), seen.size() + 1);
    EXPECT_EQ(again.back(), "frame-99");
  }
}

// Macha: test_a_damaged_or_refused_frame_ends_the_replay_there.
TEST(LedgerSealedJournal, ADamagedOrRefusedFrameEndsTheReplayThere) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "journal.log";
  uint64_t second_frame_end = 0;
  {
    SealedJournal journal(path, key_of(3), test_aad, 1U << 20, quiet_logger());
    journal.append(frame_text(0));
    journal.append(frame_text(1));
    second_frame_end = journal.bytes();
    journal.append(frame_text(2));
  }
  // A flipped byte in the third frame's ciphertext: two frames survive. Flipped, not set:
  // the byte is random and is the set value once in 256 runs, which left the frame whole.
  {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    const auto last = static_cast<std::streamoff>(std::filesystem::file_size(path) - 1);
    file.seekg(last);
    const auto byte = static_cast<char>(file.get() ^ 0x01);
    file.seekp(last);
    file.put(byte);
  }
  const auto sink = std::make_shared<test::RecordingSink>();
  SealedJournal damaged(path, key_of(3), test_aad, 1U << 20, test::logger_into(sink));
  EXPECT_EQ(replay_all(damaged).size(), 2u);
  EXPECT_EQ(std::filesystem::file_size(path), second_frame_end);
  // Said where it stopped, as Macha logs it.
  EXPECT_TRUE(sink->saw("test journal recovered path=" + path.string() + " offset=" + std::to_string(second_frame_end)));

  // A frame the reader refuses ends the replay as a damaged one does.
  SealedJournal refusing(path, key_of(3), test_aad, 1U << 20, quiet_logger());
  int applied = 0;
  refusing.replay(
      [&](std::span<const uint8_t>) {
        if (applied == 1) throw std::runtime_error("refused");
        ++applied;
      },
      1U << 20, "test journal");
  EXPECT_EQ(applied, 1);
  EXPECT_EQ(refusing.frames(), 1u);

  // Another key, or another journal's associated data, opens nothing.
  SealedJournal other_key(path, key_of(4), test_aad, 1U << 20, quiet_logger());
  EXPECT_TRUE(replay_all(other_key).empty());
}

// Macha: test_a_reset_journal_is_empty_and_keeps_appending.
TEST(LedgerSealedJournal, AResetJournalIsEmptyAndKeepsAppending) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "journal.log";
  SealedJournal journal(path, key_of(5), test_aad, 1U << 20, quiet_logger());
  journal.append(frame_text(0));
  journal.reset();
  EXPECT_EQ(journal.frames(), 0u);
  EXPECT_EQ(journal.bytes(), 0u);
  EXPECT_EQ(std::filesystem::file_size(path), 0u);
  journal.append(frame_text(1));
  SealedJournal reopened(path, key_of(5), test_aad, 1U << 20, quiet_logger());
  EXPECT_EQ(replay_all(reopened), std::vector<std::string>{"frame-1"});

  // A frame over the limit is refused, and nothing is written.
  SealedJournal small(dir.path() / "small.log", key_of(5), test_aad, 8, quiet_logger());
  EXPECT_THROW(small.append(frame_text(12345)), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "small.log"));
}

// The trie journal's frame that the object_trie known answers use: three
// changes under domain v1's known-answer storage key, nonce 00..0b.
constexpr char known_plaintext[] =
    "020000000384547628a69e4abe5b2bcf22a82f7e892b20cb1388251f8d20d1f2e075a5ef5c0100000000e6245056833abaff8e8b187d7772c70a01e84fcc7adbc1c64e8d43ae5806641f00ea7b"
    "c72"
    "e698d9fbf0f3dd51d9d82d3cc9b48853c92b33e4d120de4694f7db49f01000000080000000000000001";
constexpr char known_frame[] =
    "00000078000102030405060708090a0bead7a793135e705a4d752d5e369c5bdd003b2b73b546a36043e6fd31e3f0d5634d254866e463f5c21fa93d6bc5c3ee45d76b51c25977430d7f2865092a"
    "22fb6716bc99834b91dae8653606f348b708837e4c87bd9f4caa594bab735d184f325add0039f251632f9bb71f33d621a92d12fcd7abde887608f51bed1b8214278fcb47b4732c41bf8b4d";
constexpr std::array<uint8_t, 8> trie_journal_aad{'M', 'A', 'C', 'H', 'L', 'J', '0', '1'};

// A frame Macha wrote replays here: be32 length, nonce, tag, ciphertext,
// the journal's eight bytes as the associated data, no file magic.
TEST(LedgerSealedJournal, ReplaysAFrameMachaWrote) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "journal.log";
  write_file(path, unhex(known_frame));
  SealedJournal journal(path, test::known_answer_key(), trie_journal_aad, 4U << 20, quiet_logger());
  std::vector<Bytes> seen;
  journal.replay([&](std::span<const uint8_t> plain) { seen.emplace_back(plain.begin(), plain.end()); }, 4U << 20, "test journal");
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(dannan::identity::hex(seen.front()), known_plaintext);
  EXPECT_EQ(journal.frames(), 1u);
  EXPECT_EQ(journal.bytes(), 32u + 120u);
  // And by offset, as the trie reads its nodes.
  EXPECT_EQ(dannan::identity::hex(journal.read_at(0)), known_plaintext);
}

// What Dagda appends is that frame: under the same nonce, the same bytes.
TEST(LedgerSealedJournal, AppendsMachasFraming) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "journal.log";
  const auto key = test::known_answer_key();
  const auto plain = unhex(known_plaintext);
  {
    SealedJournal journal(path, key, trie_journal_aad, 4U << 20, quiet_logger());
    journal.append(plain);
    EXPECT_EQ(journal.bytes(), 32u + plain.size());
  }
  std::ifstream in(path, std::ios::binary);
  const Bytes written{std::istreambuf_iterator<char>(in), {}};
  ASSERT_EQ(written.size(), 32u + plain.size());
  EXPECT_EQ(dannan::identity::hex(std::span(written).first(4)), "00000078");
  const auto nonce = std::span<const uint8_t, 12>(written.data() + 4, 12);
  const auto tag = std::span<const uint8_t, 16>(written.data() + 16, 16);
  const auto ciphertext = std::span(written).subspan(32);
  EXPECT_EQ(dannan::identity::aes_gcm_open(key->bytes(), nonce, tag, ciphertext, trie_journal_aad), plain);

  // Sealed under the known nonce, the frame is Macha's byte for byte.
  const auto known_nonce = unhex("000102030405060708090a0b");
  const auto sealed = dannan::identity::aes_gcm_seal(key->bytes(), std::span<const uint8_t, 12>(known_nonce.data(), 12), plain, trie_journal_aad);
  Bytes frame(written.begin(), written.begin() + 4);
  frame.insert(frame.end(), sealed.nonce.begin(), sealed.nonce.end());
  frame.insert(frame.end(), sealed.tag.begin(), sealed.tag.end());
  frame.insert(frame.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  EXPECT_EQ(dannan::identity::hex(frame), known_frame);
}

}  // namespace
}  // namespace dagda::ledger
