//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/ledger/sealed_journal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"

namespace dagda::ledger {
namespace {

void write_all(int fd, std::span<const uint8_t> bytes) {
  size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error(std::string("journal write failed: ") + std::strerror(errno));
    done += static_cast<size_t>(n);
  }
}

bool pread_all(int fd, std::span<uint8_t> bytes, uint64_t offset) {
  size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::pread(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

void fsync_checked(int fd) {
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) throw std::runtime_error(std::string("journal sync failed: ") + std::strerror(errno));
}

uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) | (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
}

constexpr uint64_t frame_header = 4 + 12 + 16;

std::shared_ptr<const domain::StorageKey> required(std::shared_ptr<const domain::StorageKey> key) {
  if (!key) throw std::invalid_argument("a sealed journal needs a key");
  return key;
}

Bytes seal_frame(const domain::StorageKey& key, std::span<const uint8_t> aad, std::span<const uint8_t> plaintext, uint32_t max_frame,
                 const std::filesystem::path& path) {
  const auto sealed = dannan::identity::aes_gcm_seal(key.bytes(), plaintext, aad);
  if (sealed.ciphertext.size() > max_frame) throw std::runtime_error("journal frame too large: " + path.string());
  const auto length = static_cast<uint32_t>(sealed.ciphertext.size());
  Bytes frame;
  frame.reserve(frame_header + sealed.ciphertext.size());
  frame.push_back(static_cast<uint8_t>(length >> 24U));
  frame.push_back(static_cast<uint8_t>(length >> 16U));
  frame.push_back(static_cast<uint8_t>(length >> 8U));
  frame.push_back(static_cast<uint8_t>(length));
  frame.insert(frame.end(), sealed.nonce.begin(), sealed.nonce.end());
  frame.insert(frame.end(), sealed.tag.begin(), sealed.tag.end());
  frame.insert(frame.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  return frame;
}

Bytes read_frame(int fd, const domain::StorageKey& key, const std::array<uint8_t, 8>& aad, uint32_t max_frame, uint64_t limit, uint64_t offset) {
  std::array<uint8_t, 4> length_bytes{};
  if (offset + frame_header > limit || !pread_all(fd, length_bytes, offset)) throw std::runtime_error("no journal frame at offset " + std::to_string(offset));
  const uint32_t length = read_be32(length_bytes.data());
  if (length > max_frame || offset + frame_header + length > limit)
    throw std::runtime_error("journal frame at offset " + std::to_string(offset) + " runs past the end");
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
  Bytes ciphertext(length);
  if (!pread_all(fd, nonce, offset + 4) || !pread_all(fd, tag, offset + 16) || !pread_all(fd, ciphertext, offset + 32))
    throw std::runtime_error("short journal frame at offset " + std::to_string(offset));
  return dannan::identity::aes_gcm_open(key.bytes(), nonce, tag, ciphertext, aad);
}

}  // namespace

SealedJournal::SealedJournal(std::filesystem::path path, std::shared_ptr<const domain::StorageKey> key, std::array<uint8_t, 8> aad, uint32_t max_frame,
                             dannan::status::ScopedLogger logger)
    : _path(std::move(path)), _key(required(std::move(key))), _aad(aad), _max_frame(max_frame), _logger(std::move(logger)) {
  // A file read by offset and never replayed (the trie's nodes) appends
  // after what is already there.
  std::error_code ec;
  if (const auto size = std::filesystem::file_size(_path, ec); !ec) _bytes = size;
}

SealedJournal::~SealedJournal() { close_fd(); }

int SealedJournal::fd() const {
  if (_fd < 0) {
    _fd = ::open(_path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (_fd < 0) throw std::runtime_error("cannot open journal " + _path.string() + ": " + std::strerror(errno));
  }
  return _fd;
}

void SealedJournal::close_fd() const noexcept {
  if (_fd >= 0) {
    ::close(_fd);
    _fd = -1;
  }
}

size_t SealedJournal::replay(const std::function<void(std::span<const uint8_t>)>& apply, uint32_t max_read_frame, std::string_view what) {
  close_fd();
  _frames = 0;
  _bytes = 0;
  if (!std::filesystem::exists(_path)) return 0;
  const int fd = ::open(_path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("cannot read " + std::string(what) + " " + _path.string());
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    const auto saved = errno;
    ::close(fd);
    throw std::runtime_error("cannot stat " + std::string(what) + ": " + std::strerror(saved));
  }
  const auto file_size = static_cast<uint64_t>(st.st_size);
  uint64_t offset = 0;
  uint64_t durable = 0;
  while (offset + 4 <= file_size) {
    std::array<uint8_t, 4> length_bytes{};
    if (!pread_all(fd, length_bytes, offset)) break;
    const uint32_t length = read_be32(length_bytes.data());
    if (length > max_read_frame) break;
    const uint64_t frame_size = frame_header + static_cast<uint64_t>(length);
    if (offset > std::numeric_limits<uint64_t>::max() - frame_size || offset + frame_size > file_size) break;
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 16> tag{};
    Bytes ciphertext(length);
    if (!pread_all(fd, nonce, offset + 4) || !pread_all(fd, tag, offset + 16) || !pread_all(fd, ciphertext, offset + 32)) break;
    try {
      apply(dannan::identity::aes_gcm_open(_key->bytes(), nonce, tag, ciphertext, _aad));
    } catch (const std::exception& error) {
      _logger.warn(std::string(what) + " recovered path=" + _path.string() + " offset=" + std::to_string(offset) + " reason=" + error.what());
      break;
    }
    offset += frame_size;
    durable = offset;
    ++_frames;
  }
  if (durable != file_size) {
    if (::ftruncate(fd, static_cast<off_t>(durable)) != 0) {
      const auto error = errno;
      ::close(fd);
      throw std::runtime_error("cannot truncate " + std::string(what) + ": " + std::strerror(error));
    }
    try {
      fsync_checked(fd);
    } catch (...) {
      ::close(fd);
      throw;
    }
  }
  if (::close(fd) != 0) throw std::runtime_error("cannot close " + std::string(what));
  _bytes = durable;
  return _frames;
}

void SealedJournal::append(std::span<const uint8_t> plaintext) {
  (void)append_unsynced(plaintext);
  sync();
}

uint64_t SealedJournal::append_unsynced(std::span<const uint8_t> plaintext) {
  const auto frame = seal_frame(*_key, _aad, plaintext, _max_frame, _path);
  const auto offset = _bytes;
  write_all(fd(), frame);
  ++_frames;
  _bytes += frame.size();
  return offset;
}

void SealedJournal::sync() {
  if (_fd >= 0) fsync_checked(_fd);
}

Bytes SealedJournal::read_at(uint64_t offset) const { return read_frame(fd(), *_key, _aad, _max_frame, _bytes, offset); }

SealedJournal::FrameReader::FrameReader(int fd, std::shared_ptr<const domain::StorageKey> key, std::array<uint8_t, 8> aad, uint32_t max_frame, uint64_t limit)
    : _fd(fd), _key(std::move(key)), _aad(aad), _max_frame(max_frame), _limit(limit) {}

SealedJournal::FrameReader::~FrameReader() { ::close(_fd); }

Bytes SealedJournal::FrameReader::read_at(uint64_t offset) const { return read_frame(_fd, *_key, _aad, _max_frame, _limit, offset); }

std::shared_ptr<const SealedJournal::FrameReader> SealedJournal::reader() const {
  const int copy = ::fcntl(fd(), F_DUPFD_CLOEXEC, 0);
  if (copy < 0) throw std::runtime_error("cannot duplicate journal " + _path.string() + ": " + std::strerror(errno));
  return std::shared_ptr<const FrameReader>(new FrameReader(copy, _key, _aad, _max_frame, _bytes));
}

void SealedJournal::reset() {
  close_fd();
  dannan::identity::durable_replace_file(_path, {});
  _frames = 0;
  _bytes = 0;
}

}  // namespace dagda::ledger
