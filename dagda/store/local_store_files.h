//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "dagda/store/object_id.h"
#include "dannan/contract/thread_safety.h"

namespace dagda::store {

// The device work behind LocalStore's object reads, object writes and pack
// compaction, which the store does with its index mutex released. Handed to
// the store at construction and owned by whoever constructed it, which keeps
// it alive for the store's lifetime; a test hands in one that holds or fails
// an operation. PosixLocalStoreFiles is the device. Every operation is
// thread-safe and waits on the device holding its path.
class LocalStoreFiles {
 public:
  virtual ~LocalStoreFiles() = default;
  // Writes head then body to a new file, creating its directory, and renames
  // it to `path`. A throw leaves nothing at `path`.
  virtual void install(const std::filesystem::path&, std::span<const uint8_t> head, std::span<const uint8_t> body) DANNAN_EXCLUDES(dannan::contract::no_io) = 0;
  // The whole file. Throws unless it is read in full.
  virtual Bytes read(const std::filesystem::path&) DANNAN_EXCLUDES(dannan::contract::no_io) = 0;
  // `size` bytes at `offset`, or none if the file is shorter. Throws if the
  // file cannot be opened or closed.
  virtual std::optional<Bytes> read_at(const std::filesystem::path&, uint64_t offset, size_t size) DANNAN_EXCLUDES(dannan::contract::no_io) = 0;
  // As read_at, into the caller's buffer, so an extent read needs no second one:
  // false if the file is shorter, `into` then holding what was read. This one
  // goes through read_at, for a test's files; the device reads in place.
  virtual bool read_at_into(const std::filesystem::path&, uint64_t offset, std::span<uint8_t> into) DANNAN_EXCLUDES(dannan::contract::no_io);
  // Appends head then body, creating the file. A throw may leave part of
  // them appended.
  virtual void append(const std::filesystem::path&, std::span<const uint8_t> head, std::span<const uint8_t> body) DANNAN_EXCLUDES(dannan::contract::no_io) = 0;
  // Cuts the file to `size`; false if it cannot.
  virtual bool truncate(const std::filesystem::path&, uint64_t size) DANNAN_EXCLUDES(dannan::contract::no_io) = 0;
  // The regular files in `dir` with their sizes. Throws if it cannot list or
  // size them.
  virtual std::vector<std::pair<std::filesystem::path, uint64_t>> list(const std::filesystem::path& dir) DANNAN_EXCLUDES(dannan::contract::no_io) = 0;
};

// The files on the local filesystem through POSIX calls. Stateless.
class PosixLocalStoreFiles final : public LocalStoreFiles {
 public:
  void install(const std::filesystem::path&, std::span<const uint8_t> head, std::span<const uint8_t> body) override DANNAN_EXCLUDES(dannan::contract::no_io);
  Bytes read(const std::filesystem::path&) override DANNAN_EXCLUDES(dannan::contract::no_io);
  std::optional<Bytes> read_at(const std::filesystem::path&, uint64_t offset, size_t size) override DANNAN_EXCLUDES(dannan::contract::no_io);
  bool read_at_into(const std::filesystem::path&, uint64_t offset, std::span<uint8_t> into) override DANNAN_EXCLUDES(dannan::contract::no_io);
  void append(const std::filesystem::path&, std::span<const uint8_t> head, std::span<const uint8_t> body) override DANNAN_EXCLUDES(dannan::contract::no_io);
  bool truncate(const std::filesystem::path&, uint64_t size) override DANNAN_EXCLUDES(dannan::contract::no_io);
  std::vector<std::pair<std::filesystem::path, uint64_t>> list(const std::filesystem::path& dir) override DANNAN_EXCLUDES(dannan::contract::no_io);
};

// The process's PosixLocalStoreFiles. It holds no state, so one instance
// living for the whole process serves every store.
LocalStoreFiles& posix_local_store_files() noexcept;

}  // namespace dagda::store
