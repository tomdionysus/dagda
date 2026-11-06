//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/store/object_id.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/key_custody.h"
#include "dannan/identity/node_id.h"

// What says whether a node's data survived the migration. The disks are
// migrated in place with no second copy to hold them against, so a read-only
// pass over the state before the migration writes a manifest, the same pass
// after it writes a second, and the comparison reports every difference by
// name. A false pass is the worst thing this can do: what it cannot check is
// NOT CHECKED, which fails, and a category that held nothing says ZERO.
//
// A pass opens every sealed record (AES-GCM, then the plaintext's SHA-256
// against the id) and walks every ledger, so a record listed as opened is the
// object its id names. It writes nothing under the state, a backend, the
// control store or the cache, not even a lock file: it takes a shared flock on
// each `.macha.lock` that exists, which refuses a node holding it and refuses
// a node trying to start meanwhile, and it fails if a lock file that was not
// there has appeared by the end. See docs/migration.md.
namespace dagda::migrate {

using store::Bytes;

// A manifest that is truncated, altered or not a manifest at all: never a
// shorter comparison.
class ManifestDamaged : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// ---- The seam for the metadata ----------------------------------------------

// What the metadata reader is given: the roots, and the keys of the pass's
// layout.
struct MetadataManifestInput {
  std::filesystem::path state;
  std::filesystem::path cache;
  std::shared_ptr<const domain::StorageKey> domain_key;
  std::shared_ptr<const domain::StorageKey> node_local_key;
  // 18 or 19.
  uint32_t layout = 0;
};

// One thing the metadata holds: `kind` is "certificates", "heads" or "history"
// and `name` what the comparison calls it. Two facts of a name compare by value.
struct MetadataFact {
  std::string kind;
  std::string name;
  Bytes value;
};

// The seam heads session S7 fills (TODO/2026-11-05-heads-plan.md section 4):
// heads, certificates and history replay, read under either layout. Until a
// reader is given all three categories are NOT CHECKED and the comparison
// fails, so nothing passes without them.
class MetadataManifestReader {
 public:
  virtual ~MetadataManifestReader() = default;
  // Emits every fact, strictly increasing by (kind, name); a fact out of order
  // fails the pass. Reads only. Throws for what it cannot read, which fails the
  // pass. Waits on the state device.
  virtual void read(const MetadataManifestInput& input, const std::function<void(const MetadataFact&)>& emit) = 0;
  // Whether a file under `root` belongs to the metadata, so that the pass leaves
  // it out of the files it compares byte for byte. Waits on nothing.
  virtual bool covers(const std::filesystem::path& root, const std::filesystem::path& relative) const = 0;
};

// The reader dagda-migrate runs with: nothing until S7 returns its own here.
std::shared_ptr<MetadataManifestReader> metadata_manifest_reader();

// ---- The pass ---------------------------------------------------------------

struct ManifestOptions {
  // As migrate_state takes them, and they must be spelled the same in both
  // passes: a store is known by its path.
  std::vector<std::filesystem::path> backends{};
  std::filesystem::path control{};
  std::filesystem::path cache{};
  // The id of the node these disks belong to, and Dannan's state directory,
  // whose lock is checked too. A manifest records the id; a marker naming
  // another is NOT CHECKED.
  std::optional<dannan::identity::NodeId> node{};
  std::filesystem::path node_state{};
  // Continue the pass `<manifest>.partial` holds. Refused if the state has
  // changed since it began.
  bool resume = false;
  std::shared_ptr<MetadataManifestReader> metadata{};
  // What the manifest is made durable after, whichever comes first: a stopped
  // pass reads again at most this much.
  uint64_t checkpoint_records = 256;
  uint64_t checkpoint_bytes = 1ULL << 30;
  // A progress line every so many records; zero: none.
  uint64_t progress_every = 4096;
  std::function<void(std::string_view)> say{};
  // Called after every durable checkpoint: a test stops the pass here.
  std::function<void()> after_checkpoint{};
};

struct ManifestCategory {
  std::string name;
  uint64_t items = 0;
  bool checked = true;
  std::string why{};
};

struct ManifestSummary {
  // 18 or 19.
  uint32_t layout = 0;
  uint64_t manifest_bytes = 0;
  uint64_t bytes_read = 0;
  uint64_t items = 0;
  // Records the pass could take from `<manifest>.partial` without reading.
  uint64_t resumed_items = 0;
  std::vector<ManifestCategory> categories;
  // What the pass could not check, by name, with why.
  std::vector<std::string> not_checked;
  // Bytes after a pack's or a journal's last whole record: what the store's own
  // recovery drops. Reported, not failed.
  uint64_t tail_bytes = 0;

  std::string summary() const;
};

// Writes the manifest of the state at `manifest`, through `<manifest>.partial`
// renamed when whole. Refuses (MigrationRefused) a state it does not recognise,
// a store a node holds open, a manifest inside any store or that exists, a key
// that opens none of the first records found. Throws std::runtime_error for a
// device that failed. Waits on the devices; single owner.
ManifestSummary write_manifest(const std::filesystem::path& manifest, const std::filesystem::path& state, const dannan::identity::ClusterKey& cluster,
                               const dannan::identity::KeyCustody& custody, const ManifestOptions& options);

// ---- The comparison ---------------------------------------------------------

struct ComparisonOptions {
  // The layouts each manifest must have been written under. Going forward: 18
  // then 19. There and back: 18 then 18. Back alone: 19 then 18.
  uint32_t before_layout = 18;
  uint32_t after_layout = 19;
  // Every line as it is found; nothing: only the report.
  std::function<void(std::string_view)> say{};
  // How many differences the report keeps; all are counted and said.
  size_t keep = 4096;
};

enum class CategoryResult : uint8_t { same, zero, different, not_checked };

struct ComparedCategory {
  std::string name;
  uint64_t before = 0;
  uint64_t after = 0;
  uint64_t differences = 0;
  uint64_t not_checked = 0;
  std::string why{};
  CategoryResult result() const noexcept;
};

struct ComparisonReport {
  std::vector<ComparedCategory> categories;
  // "<category>: <what differs, by name>".
  std::vector<std::string> differences;
  std::vector<std::string> not_checked;
  uint64_t difference_count = 0;
  uint64_t not_checked_count = 0;

  // Nothing differs and everything was checked.
  bool passed() const noexcept { return difference_count == 0 && not_checked_count == 0; }
  // Whether a difference line holds `text`.
  bool names(std::string_view text) const;
  const ComparedCategory* category(std::string_view name) const;
  // A line a category and the result.
  std::string summary() const;
};

// Throws ManifestDamaged for a manifest that is not whole. Waits on the device
// the manifests are on; memory is bounded whatever their size. Thread-safe.
ComparisonReport compare_manifests(const std::filesystem::path& before, const std::filesystem::path& after, const ComparisonOptions& options = {});

}  // namespace dagda::migrate
