//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/local_store_files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include "dagda/store/write_behind.h"

namespace dagda::store {
namespace {

void write_all(int fd, std::span<const uint8_t> bytes) {
  size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::strerror(errno));
    }
    done += static_cast<size_t>(n);
  }
}

}  // namespace

void PosixLocalStoreFiles::install(const std::filesystem::path& path, std::span<const uint8_t> head, std::span<const uint8_t> body) {
  std::filesystem::create_directories(path.parent_path());
  // Layout v18's temporary name: the store's start-up deletes whatever it
  // finds named ".tmp". Only a name, so the real clock, not the store's.
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const std::string temp = path.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(now_ms);
  int fd = -1;
  try {
    fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error(std::strerror(errno));
    write_all(fd, head);
    write_all(fd, body);
    // Durability is the domain's barrier, never an fsync here; nothing written
    // should sit dirty behind a control commit either.
    start_writeback(fd);
    const int close_rc = ::close(fd);
    fd = -1;
    if (close_rc != 0) throw std::runtime_error(std::strerror(errno));
    if (::rename(temp.c_str(), path.c_str()) != 0) throw std::runtime_error(std::strerror(errno));
  } catch (...) {
    if (fd >= 0) ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    throw;
  }
}

Bytes PosixLocalStoreFiles::read(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw ObjectReadError("cannot open object: " + std::string(std::strerror(errno)));
  struct stat st{};
  if (::fstat(fd, &st) != 0 || st.st_size < 0) {
    const auto error = std::string(std::strerror(errno));
    ::close(fd);
    throw ObjectReadError("cannot stat object: " + error);
  }
  Bytes out(static_cast<size_t>(st.st_size));
  size_t done = 0;
  while (done < out.size()) {
    const auto n = ::read(fd, out.data() + done, out.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      const auto error = std::string(std::strerror(errno));
      ::close(fd);
      throw ObjectReadError("cannot read object: " + error);
    }
    if (n == 0) {
      ::close(fd);
      throw ObjectReadError("short object read");
    }
    done += static_cast<size_t>(n);
  }
  if (::close(fd) != 0) throw std::runtime_error("cannot close object: " + std::string(std::strerror(errno)));
  return out;
}

std::optional<Bytes> PosixLocalStoreFiles::read_at(const std::filesystem::path& path, uint64_t offset, size_t size) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw ObjectReadError("cannot open pack: " + std::string(std::strerror(errno)));
  std::optional<Bytes> out(std::in_place, size);
  size_t done = 0;
  while (done < size) {
    const auto n = ::pread(fd, out->data() + done, size - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      const auto error = std::string(std::strerror(errno));
      ::close(fd);
      throw ObjectReadError("cannot read pack: " + error);
    }
    if (n == 0) {
      out.reset();
      break;
    }
    done += static_cast<size_t>(n);
  }
  if (::close(fd) != 0) throw std::runtime_error("cannot close pack: " + std::string(std::strerror(errno)));
  return out;
}

void PosixLocalStoreFiles::append(const std::filesystem::path& path, std::span<const uint8_t> head, std::span<const uint8_t> body) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot open pack: " + std::string(std::strerror(errno)));
  try {
    write_all(fd, head);
    write_all(fd, body);
  } catch (...) {
    ::close(fd);
    throw;
  }
  if (::close(fd) != 0) throw std::runtime_error("cannot close pack: " + std::string(std::strerror(errno)));
}

bool PosixLocalStoreFiles::truncate(const std::filesystem::path& path, uint64_t size) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool cut = ::ftruncate(fd, static_cast<off_t>(size)) == 0;
  ::close(fd);
  return cut;
}

std::vector<std::pair<std::filesystem::path, uint64_t>> PosixLocalStoreFiles::list(const std::filesystem::path& dir) {
  std::vector<std::pair<std::filesystem::path, uint64_t>> files;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
    if (error) break;
    if (!entry.is_regular_file()) continue;
    std::error_code size_error;
    const auto size = entry.file_size(size_error);
    if (size_error) throw std::runtime_error("cannot size " + entry.path().string() + ": " + size_error.message());
    files.push_back({entry.path(), size});
  }
  if (error) throw std::runtime_error("cannot list " + dir.string() + ": " + error.message());
  return files;
}

LocalStoreFiles& posix_local_store_files() noexcept {
  static PosixLocalStoreFiles files;
  return files;
}

}  // namespace dagda::store
