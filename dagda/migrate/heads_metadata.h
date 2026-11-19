//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/migrate/manifest.h"
#include "dagda/migrate/migrate.h"

// Macha's metadata, moved to the heads' layout (TODO/2026-11-05-heads-plan.md
// section 4). What 0.93.4 (9a08846) left in `<state>/metadata/` becomes one head
// space's directory, `<state>/heads/<space id hex>/`, and the cache seed
// `<cache>/metadata/current.meta` becomes `<cache>/metadata/<space id hex>.meta`:
//
// - history.log and journal.log: each frame that authenticates keeps its length,
//   nonce and ciphertext and has its tag moved from v18's associated data (the
//   magic) to v19's, by derivation (migrate/retag.h). Every other byte is carried
//   as it is: a damaged region between frames that authenticate (Macha 0.93.5
//   leaves those), and a torn tail, which the reader cuts at open as Macha's does.
//   So the file keeps its size and every frame its offset. It is written whole to a
//   sibling, checked to read as the old one did frame for frame, and renamed over
//   it: a reader never meets a file that is part one layout and part the other.
// - checkpoint.meta: the same, one sealed record, under the domain's key.
// - heads.meta and author.meta: this node's standing, re-sealed from the domain's
//   key to the node-local key under a fresh nonce; the plaintext byte for byte.
// - set-aside.meta: plain in Macha's (its finding D2), sealed under the node-local
//   key with a magic of its own; the plaintext byte for byte, whether or not it
//   parses, so a reader judges it as Macha's would.
// - the seed: written at its new name with its tag moved, then the old removed.
// - everything else in the directory (recovery.required, author-chain-broken, a
//   quarantine `*.corrupt.<stamp>`, a file no version reads) goes with the
//   directory, byte for byte.
//
// The directory is renamed last, which is the mark that the space is done. Each
// file before that tells by which associated data or key opens it how far a run
// got, so a second run after a crash finishes; nothing needs another marker.
namespace dagda::migrate {

// Macha's metadata at 9a08846: its magics (metadata.cpp, persistent_cache.cpp),
// kept in each file, and the space's name under v19.
namespace macha_metadata {
inline constexpr heads::Magic checkpoint_magic{'D', 'H', 'T', 'M', 'D', 'B', '0', '1'};
inline constexpr heads::Magic journal_magic{'D', 'H', 'T', 'M', 'J', 'N', 'L', '1'};
inline constexpr heads::Magic history_magic{'D', 'H', 'T', 'M', 'H', 'S', 'T', '1'};
inline constexpr heads::Magic heads_magic{'D', 'H', 'T', 'M', 'A', 'C', 'C', '1'};
inline constexpr heads::Magic author_magic{'D', 'H', 'T', 'M', 'A', 'U', 'T', '1'};
// New at v19: Macha's set-aside.meta has no magic and no seal.
inline constexpr heads::Magic set_aside_magic{'D', 'H', 'T', 'M', 'S', 'A', 'S', '1'};
inline constexpr heads::Magic seed_magic{'D', 'H', 'T', 'C', 'M', 'E', 'T', '1'};
// The plan's proposed name; dagda-migrate --space names another.
inline constexpr std::string_view space_name = "macha/namespace";
heads::ReplicaMagics magics();
}  // namespace macha_metadata

// Where one space's metadata is under each layout.
struct HeadsPlaces {
  heads::SpaceId space{};
  // <state>/metadata and <state>/heads/<space id hex>.
  std::filesystem::path v18_directory;
  std::filesystem::path v19_directory;
  // <cache>/metadata/current.meta and <cache>/metadata/<space id hex>.meta;
  // empty without a cache.
  std::filesystem::path v18_seed;
  std::filesystem::path v19_seed;
};
HeadsPlaces heads_places(const std::filesystem::path& state, const std::filesystem::path& cache, std::string_view space_name);

// The associated data each layout seals a magic under: v18's is the magic alone,
// v19's names the domain and the space (heads::replica_aad, heads::seed_aad).
Bytes v18_aad(const heads::Magic& magic);
Bytes v19_aad(const heads::SpaceId& space, const heads::Magic& magic);
Bytes v19_seed_aad(const heads::SpaceId& space, const heads::Magic& magic);

// One span of a file of frames, as the replica's reader meets it
// (heads::Replica::load_history, Macha's at 98b816d): a frame that authenticates,
// the damaged bytes between one and the next, or a tail after the last.
struct FrameSpan {
  enum class Kind : uint8_t { frame, damaged, tail };
  Kind kind = Kind::frame;
  uint64_t offset = 0;
  // The whole span, a frame's length word included.
  uint64_t length = 0;
  // For a frame: the index of the associated data that opened it, and its seal.
  size_t opened_by = 0;
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
  uint64_t ciphertext_bytes = 0;
};

// Walks `path` as the replica reads it, a frame opening under any of `aads`:
// the length word, the frame if it opens; if not, forward to the next offset
// holding one that does (heads::next_frame_from), the bytes passed over a damaged
// span; none, and the rest is a tail. `visit` sees each span in order with a
// frame's plaintext. A read that fails throws, so it is never taken for damage.
// Waits on the state device.
void walk_frames(const std::filesystem::path& path, const domain::StorageKey& key, const std::vector<Bytes>& aads,
                 const std::function<void(const FrameSpan&, const Bytes* plaintext)>& visit);

// The metadata pass of migrate_state (forward) and migrate_state_back (reverse),
// for the space `options.space_name`. `plan_only` classifies every file and throws
// MigrationRefused for what a run must refuse, writing nothing: the refusals come
// before the first write. Otherwise it plans again and moves what is not yet moved
// (nothing under `options.dry_run`), calling `options.after_step` after each
// durable step. The caller holds the state's and the cache's locks and has written
// the state's marker. Waits on the state and cache devices; single owner.
void migrate_heads_metadata(const std::filesystem::path& state, const domain::StorageKey& domain_key, const domain::StorageKey& node_local,
                            const MigrationOptions& options, MigrationReport& report, bool plan_only);

// Whether there is anything of Macha's metadata to move forward: `<state>/metadata`
// or the old seed. Waits on the state device.
bool heads_metadata_pending(const std::filesystem::path& state, const std::filesystem::path& cache, std::string_view space_name);

// The comparison's reader of the metadata (manifest.h): every fact of the space
// under whichever layout holds it, `<state>/heads/<space id hex>/` read under
// v19's forms and `<state>/metadata/` under v18's, so a state whose metadata has
// not moved yet compares too. Kinds, in order: author, certificates, checkpoint,
// heads (each certificate's head and how history replays it), history (every span
// by offset), journal, markers (every other file of the directory), seed,
// set-aside. A damaged span or a file that opens under neither form is a fact
// whose bytes are compared and whose meaning cannot be known.
class HeadsMetadataReader final : public MetadataManifestReader {
 public:
  explicit HeadsMetadataReader(std::string space_name = std::string(macha_metadata::space_name));
  void read(const MetadataManifestInput& input, const std::function<void(const MetadataFact&)>& emit) override;
  bool covers(const std::filesystem::path& root, const std::filesystem::path& relative) const override;

 private:
  const std::string _space_name;
  const std::string _space_hex;
};

}  // namespace dagda::migrate
