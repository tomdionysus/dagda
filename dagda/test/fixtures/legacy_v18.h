//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// A small writer of layout v18, Macha's, for the migration's tests: the
// state layout marker, a backend's marker and its mirror, loose files and
// pack records sealed with the id alone as the associated data. The ledgers
// of a v18 state are the ledger classes' own files under the domain key, so
// a test writes those through the classes. Test only: the library writes
// v18 nowhere.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>

#include "dagda/domain/storage_key.h"
#include "dagda/migrate/legacy_v18.h"
#include "dagda/store/local_format.h"
#include "dagda/store/object_id.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/node_id.h"

namespace dagda::test::legacy {

using store::Bytes;
using store::ObjectId;

inline void write_file(const std::filesystem::path& path, std::span<const uint8_t> bytes, bool append = false) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good()) << path;
}

inline Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

// "macha-state-layout-v18\n", as Macha's node start wrote it.
inline void write_state_layout_v18(const std::filesystem::path& state) {
  std::filesystem::create_directories(state);
  std::ofstream(state / "storage-layout", std::ios::binary) << migrate::state_layout_v18 << "\n";
}

inline std::string backend_marker_v18(const dannan::identity::NodeId& node, const dannan::identity::NodeId& token) {
  return std::string(migrate::backend_marker_v18) + " " + dannan::identity::to_string(node) + " " + dannan::identity::to_string(token) + "\n";
}

// The backend's `.macha.backend` and, under the state, its mirror named by
// the SHA-256 of the backend's path, as Macha's pool kept them.
inline void write_backend_marker_v18(const std::filesystem::path& state, const std::filesystem::path& backend, const dannan::identity::NodeId& node,
                                     const dannan::identity::NodeId& token, bool mirror = true) {
  std::filesystem::create_directories(backend);
  std::ofstream(backend / ".macha.backend", std::ios::binary) << backend_marker_v18(node, token);
  if (!mirror) return;
  const auto text = backend.lexically_normal().string();
  const auto hash = dannan::identity::sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
  std::filesystem::create_directories(state / "backend-identities");
  std::ofstream(state / "backend-identities" / (dannan::identity::hex(hash) + ".id"), std::ios::binary) << backend_marker_v18(node, token);
}

// Sealed as Macha sealed: the id alone as the associated data, under the
// nonce given or a fresh one.
inline dannan::identity::SealedData seal_v18(const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t> plain,
                                             std::optional<std::array<uint8_t, 12>> nonce = {}) {
  if (nonce) return dannan::identity::aes_gcm_seal(key.bytes(), *nonce, plain, migrate::associated_data_v18(id));
  return dannan::identity::aes_gcm_seal(key.bytes(), plain, migrate::associated_data_v18(id));
}

// A loose object file under `root`/objects.
inline void write_loose_v18(const std::filesystem::path& root, const domain::StorageKey& key, const ObjectId& id, std::span<const uint8_t> plain,
                            std::optional<std::array<uint8_t, 12>> nonce = {}) {
  const auto sealed = seal_v18(key, id, plain, nonce);
  auto file = store::local_format::encode_loose_header(plain.size(), sealed);
  file.insert(file.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  write_file(store::local_format::loose_path(root / "objects", id), file);
}

inline std::filesystem::path pack_path(const std::filesystem::path& root, uint64_t sequence) {
  return root / "packs" / store::local_format::pack_name(sequence);
}

// Appends a put record for `plain` to pack `sequence` under `root`/packs.
inline void append_pack_put_v18(const std::filesystem::path& root, uint64_t sequence, const domain::StorageKey& key, const ObjectId& id,
                                std::span<const uint8_t> plain, uint64_t touched_ms, std::optional<std::array<uint8_t, 12>> nonce = {}) {
  const auto sealed = seal_v18(key, id, plain, nonce);
  store::local_format::PackHeader header;
  header.type = store::local_format::pack_put;
  header.touched_ms = touched_ms;
  header.id = id;
  header.plain_size = plain.size();
  header.payload_size = sealed.ciphertext.size();
  header.nonce = sealed.nonce;
  header.tag = sealed.tag;
  auto record = store::local_format::encode_pack_header(header);
  record.insert(record.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  write_file(pack_path(root, sequence), record, true);
}

// Appends a touch or a tombstone: a header with no payload and nothing
// sealed.
inline void append_pack_record_v18(const std::filesystem::path& root, uint64_t sequence, uint8_t type, const ObjectId& id, uint64_t touched_ms) {
  store::local_format::PackHeader header;
  header.type = type;
  header.touched_ms = touched_ms;
  header.id = id;
  write_file(pack_path(root, sequence), store::local_format::encode_pack_header(header), true);
}

// Every regular file under `root` with its SHA-256: what a run that must
// write nothing is checked against. The stores' empty lock files are left
// out: taking a store's lock (as the migration does, to refuse a running
// node) creates one, and it holds nothing.
inline std::map<std::filesystem::path, std::string> tree_digest(const std::filesystem::path& root) {
  std::map<std::filesystem::path, std::string> out;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().filename() != ".macha.lock")
      out[entry.path()] = dannan::identity::hex(dannan::identity::sha256(read_file(entry.path())));
  return out;
}

}  // namespace dagda::test::legacy
