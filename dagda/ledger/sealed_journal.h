//
// Dagda - The Store
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
#include <optional>
#include <span>
#include <string_view>

#include "dagda/domain/storage_key.h"
#include "dagda/store/object_id.h"
#include "dannan/status/log.h"

namespace dagda::ledger {

using Bytes = store::Bytes;

// An append-only journal of sealed frames, the one crash-safe journal the
// retention store and the object trie share (requirement L2; Macha's layout
// v18's "sealed journal frame"). A frame is be32 length | 12-byte nonce |
// 16-byte tag | AES-GCM ciphertext, sealed under the key given (the
// node-local key, for a node's own ledgers) and the journal's own 8-byte
// associated data; the file has no magic. An append is
// written and fsynced before it returns. Replay reads frames in order and
// stops at the first that is torn, short or will not open, or that the
// caller refuses; the file is truncated there, so the next append follows
// the last durable frame. Frames can also be appended unsynced, made durable
// together by sync(), and read back by offset: the object trie keeps its
// nodes this way.
//
// Every operation but reader() and its FrameReader waits on the state
// device. Not thread-safe: its owner serialises every call, const ones
// included.
class SealedJournal {
 public:
  // Opens nothing yet; the file is created on the first append.
  SealedJournal(std::filesystem::path path, std::shared_ptr<const domain::StorageKey> key, std::array<uint8_t, 8> aad, uint32_t max_frame,
                dannan::status::ScopedLogger logger);
  ~SealedJournal();
  SealedJournal(const SealedJournal&) = delete;
  SealedJournal& operator=(const SealedJournal&) = delete;

  // Calls `apply` with each durable frame's plaintext, oldest first. A frame
  // `apply` throws on ends the replay as a torn one does, and is logged with
  // `what` and its offset. Frames up to `max_read_frame` bytes are read (an
  // older writer may have written larger ones). Returns the frames applied.
  // Throws std::runtime_error if the file cannot be read or truncated.
  size_t replay(const std::function<void(std::span<const uint8_t>)>& apply, uint32_t max_read_frame, std::string_view what);
  // Throws std::runtime_error for a frame over max_frame (writing nothing)
  // or a failed write or sync.
  void append(std::span<const uint8_t> plaintext);
  // Appends without syncing; returns the frame's offset.
  uint64_t append_unsynced(std::span<const uint8_t> plaintext);
  // Makes every frame appended so far durable.
  void sync();
  // The plaintext of the frame at `offset`, as an append returned it.
  // Throws std::runtime_error if it is not a whole frame that opens.
  Bytes read_at(uint64_t offset) const;

  // A read-only handle on the frames appended so far, with its own
  // descriptor: it stays readable after the file is replaced or removed.
  // Thread-safe; waits on the state device.
  class FrameReader {
   public:
    ~FrameReader();
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;
    Bytes read_at(uint64_t offset) const;

   private:
    friend class SealedJournal;
    FrameReader(int fd, std::shared_ptr<const domain::StorageKey> key, std::array<uint8_t, 8> aad, uint32_t max_frame, uint64_t limit);
    const int _fd;
    const std::shared_ptr<const domain::StorageKey> _key;
    const std::array<uint8_t, 8> _aad;
    const uint32_t _max_frame;
    const uint64_t _limit;
  };
  std::shared_ptr<const FrameReader> reader() const;
  // Empties the journal durably, once what it held is checkpointed.
  void reset();

  // Whether the frame at `offset` opens under `key` and `aad`: nothing for
  // a missing or empty file, which opens under any key. For the migration
  // (migrate/), which must know a file's key before opening it as a journal,
  // since replay truncates at the first frame that will not open. Waits on
  // the state device.
  static std::optional<bool> probe(const std::filesystem::path& path, const domain::StorageKey& key, std::array<uint8_t, 8> aad, uint32_t max_read_frame,
                                   uint64_t offset = 0);
  // Whether the file holds nothing but the front of a first frame: too short for
  // a frame's header, or a length that runs past its end. A crash's torn append,
  // which replay drops; no key opens it and no key is proved by it. Waits on the
  // state device.
  static bool holds_only_a_torn_frame(const std::filesystem::path& path, uint32_t max_read_frame);
  struct Rekeyed {
    uint64_t frames = 0;
    // Bytes after the last frame that opened under `from`: a torn tail,
    // which replay would have truncated, left behind.
    uint64_t dropped_bytes = 0;
  };
  // Re-seals every frame that opens under `from` with `to` (fresh nonces,
  // the same plaintexts, so every frame keeps its offset and size), written
  // whole to a sibling file and renamed over the journal: a reader sees the
  // old file or the new, never a mixture. Peak extra space: one copy of the
  // file. Throws std::runtime_error if the first frame does not open under
  // `from`, writing nothing. Waits on the state device.
  static Rekeyed rekey(const std::filesystem::path& path, const domain::StorageKey& from, const domain::StorageKey& to, std::array<uint8_t, 8> aad,
                       uint32_t max_read_frame);

  // Frames replayed or appended since open; bytes() is the file's length.
  uint64_t frames() const noexcept { return _frames; }
  uint64_t bytes() const noexcept { return _bytes; }
  const std::filesystem::path& path() const noexcept { return _path; }

 private:
  int fd() const;
  void close_fd() const noexcept;

  const std::filesystem::path _path;
  const std::shared_ptr<const domain::StorageKey> _key;
  const std::array<uint8_t, 8> _aad;
  const uint32_t _max_frame;
  const dannan::status::ScopedLogger _logger;
  uint64_t _frames = 0;
  uint64_t _bytes = 0;
  // Opened on first use and kept; closed before a replay or a reset.
  mutable int _fd = -1;
};

}  // namespace dagda::ledger
