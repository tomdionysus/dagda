//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/migrate/retag.h"
#include "dagda/store/object_id.h"
#include "dagda/store/sealing.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/key_custody.h"
#include "dannan/identity/node_id.h"

// The migration of a node's layout v18 state and disks (Macha's) to layout
// v19, in place (the maintainer, 2026-11-04 night: one layout, no backward
// compatibility, a migration that works and needs no second copy). Manual
// only: the store never migrates a disk at open or activate; the operator
// runs dagda-migrate (tools/dagda_migrate.cpp), which calls migrate_state.
//
// What changes on a disk, and nothing else:
//
// - Every sealed object keeps its nonce and ciphertext and has its 16-byte
//   GCM tag rewritten: AES-GCM's ciphertext depends on the key, the nonce
//   and the plaintext alone, so a record opened under v18's associated data
//   (the id) and sealed again under v19's (label, domain, id) with the same
//   nonce yields the same ciphertext and another tag. The same nonce under
//   the same key and plaintext gives no new information to anyone. A loose
//   file has its tag overwritten at a fixed offset; a pack record has its
//   tag and the SHA-256 over its header prefix overwritten in one write.
//   Nothing is copied; the bytes written are 16 a loose object and 48 a
//   pack record.
// - Every ledger a node keeps of its own (held tries, claims and their
//   tries, reference counts) is re-sealed from the domain key to the
//   node-local key, file by file: a journal is read frame by frame and
//   written whole to a sibling that is renamed over it; a trie's nodes are
//   written into its next generation's file and its root published. Peak
//   extra space is one ledger file.
// - The backend markers, their mirrors under the state and the state
//   layout marker are rewritten last, so a disk is v18 until everything on
//   it is v19.
//
// The tag is derived, not sealed again (migrate/retag.h): a record is opened
// once, under v18, which is what lists a damaged one, and the v19 tag follows
// from the v18 tag without another pass over the object.
//
// A crash leaves each record v18 or v19 (told by which associated data
// authenticates it), or for a write the power cut short part one tag and part
// the other (told by trying what the tear could have left, since the two differ
// by a value known from the associated data alone), and each ledger file under
// the old key or the new (told by which key opens it); a second run finishes,
// skipping what is done. A
// v19 state is left alone. It refuses, with a reason, a state it does not
// recognise, a backend with no marker or a marker of another form, a store
// in use, and a key under which the first records found open under neither
// layout; nothing is written before the key has opened a record or a
// ledger.
namespace dagda::migrate {

using store::Bytes;
using store::ObjectId;

// Why the migration cannot go on. Thrown before any write when the state,
// a backend or the key is refused; after some writes only for a ledger that
// opens under neither key, with everything written so far whole.
class MigrationRefused : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// What a sealed record authenticates under: v19's associated data, v18's,
// or neither (the key is wrong or the record damaged). Waits on nothing;
// thread-safe.
enum class RecordLayout : uint8_t { v18, v19, neither };
RecordLayout record_layout(const domain::StorageKey& key, const store::SealingScheme& to, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                           std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size);

// The record's tag under v19: the record opened under v18, sealed again
// under v19 with the same nonce, the ciphertext checked identical. Throws
// std::runtime_error if the record does not open under v18 or the cipher
// gives another ciphertext (it cannot). Waits on nothing; thread-safe.
std::array<uint8_t, 16> migrated_tag(const domain::StorageKey& key, const store::SealingScheme& to, const ObjectId& id, std::span<const uint8_t, 12> nonce,
                                     std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext, uint64_t plain_size);

// The evidence a run reports: what was migrated, what was already v19,
// what was left as it was, and the bytes read and written.
struct MigrationReport {
  // The state was v19 already: nothing was done.
  bool already_v19 = false;
  bool dry_run = false;
  uint64_t loose_migrated = 0;
  uint64_t loose_already = 0;
  uint64_t loose_unreadable = 0;
  // Records an earlier run's write was cut short on, part old tag and part new,
  // told apart from damage and finished. They count as migrated too.
  uint64_t loose_torn_finished = 0;
  uint64_t pack_records_torn_finished = 0;
  uint64_t packs = 0;
  uint64_t pack_records_migrated = 0;
  uint64_t pack_records_already = 0;
  uint64_t pack_records_unreadable = 0;
  // Tombstones and touches: nothing sealed in them.
  uint64_t pack_records_unsealed = 0;
  // Undecodable spans inside a pack, skipped as the store's recovery skips
  // them, and bytes of a torn tail left for the store to truncate.
  uint64_t pack_regions_skipped = 0;
  uint64_t pack_tail_bytes = 0;
  // Ledger files and tries: journals (claims.log) and tries (held, claims,
  // reference counts) each count one.
  uint64_t ledgers_migrated = 0;
  uint64_t ledgers_already = 0;
  uint64_t ledgers_empty = 0;
  // Bytes after a journal's last frame that opened: a torn tail, dropped as
  // replay would have dropped it.
  uint64_t ledger_tail_bytes_dropped = 0;
  uint64_t markers_rewritten = 0;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  // Records left as they were, by file and id, with why.
  std::vector<std::string> unreadable;
  // The run went back to v18.
  bool reverse = false;
  // Records looked at, and when the run began: the progress lines' rate.
  uint64_t records_seen = 0;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

  // One line a figure, for the tool's output.
  std::string summary() const;
};

struct MigrationOptions {
  // The control store's root; empty: <state>/metadata-objects.
  std::filesystem::path control{};
  // The persistent cache's root; empty: no cache.
  std::filesystem::path cache{};
  // Classify and count without writing: the report says what a run would
  // do, and the refusals are the same.
  bool dry_run = false;
  // The id of the node that will open these disks (Dannan's node-id file).
  // A marker keeps the id it was written with and a pool refuses another
  // node's disk, so a v18 marker naming another is refused here, before any
  // write. Nothing: not checked.
  std::optional<dannan::identity::NodeId> node{};
  // The run goes from v19 to v18. Set by migrate_state_back; read by the
  // record functions.
  bool reverse = false;
  // Stop at the first record that opens: the key proved and no more read.
  // With dry_run, what a run does before its first write.
  bool probe_only = false;
  // A progress line (records, bytes read, the rate) every so many records.
  // Zero: none.
  uint64_t progress_every = 4096;
  // Progress, a line at a time; nothing: silent.
  std::function<void(std::string_view)> say{};
  // Called after every record, ledger and marker written, with the report
  // so far: a test ends the process here to leave a half-migrated disk.
  std::function<void(const MigrationReport&)> after_step{};
};

// Migrates the node's state directory and its DATA backends in place, as
// described above: the backends' objects, the control store's and the
// cache's, then every ledger under `state`, then the markers. `cluster`
// gives domain v1's key (StorageKey::derive_v1); `custody` the node-local
// key (StorageKey::derive_node_local). Takes every store's lock, so a
// running node is refused. Throws MigrationRefused as described above, and
// std::runtime_error for a device that failed. Waits on the devices; single
// owner.
MigrationReport migrate_state(const std::filesystem::path& state, const std::vector<std::filesystem::path>& backends,
                              const dannan::identity::ClusterKey& cluster, const dannan::identity::KeyCustody& custody, const MigrationOptions& options = {});

// The way back, so a node can return to Macha 0.92.x: the same derivation and
// order the other way (v19 tags to v18, ledgers to the domain key, sightings to
// the clear, markers last), under the same rules: no second copy, every refusal
// before the first write, a crash finished by a second run. Only domain v1, and
// it accepts a v18 marker too, so a forward run that crashed can be taken back.
// Objects are byte for byte as Macha wrote them; a ledger holds what it held,
// sealed under a fresh nonce. Waits on the devices; single owner.
MigrationReport migrate_state_back(const std::filesystem::path& state, const std::vector<std::filesystem::path>& backends,
                                   const dannan::identity::ClusterKey& cluster, const dannan::identity::KeyCustody& custody,
                                   const MigrationOptions& options = {});

// One loose file: its tag rewritten if the record is v18, counted if v19 or
// unreadable. One pack: each put record likewise, the pack's checksummed
// headers recomputed. Both are what migrate_state runs over every file;
// exposed for the tests that prove the bytes. The report's probe state is
// the caller's: `proved` says a record has opened under the key, so writes
// may begin. Only `dry_run` and `after_step` of the options are read.
void migrate_loose_file(const std::filesystem::path& file, const ObjectId& id, const domain::StorageKey& key, const store::SealingScheme& to,
                        MigrationReport& report, bool& proved, const MigrationOptions& options, const TagDeriver* deriver = nullptr);
void migrate_pack_file(const std::filesystem::path& pack, const domain::StorageKey& key, const store::SealingScheme& to, MigrationReport& report, bool& proved,
                       const MigrationOptions& options, const TagDeriver* deriver = nullptr);

}  // namespace dagda::migrate
