//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/migrate/disk_order.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace dagda::migrate {

namespace {

struct Place {
  // 0: placed by its first extent; 1: by its inode only.
  int rank = 1;
  uint64_t where = 0;
};

#if defined(__linux__)
std::optional<uint64_t> first_extent(int fd) {
  // The request and room for one extent after it: struct fiemap ends in its array.
  alignas(struct fiemap) unsigned char request[sizeof(struct fiemap) + sizeof(struct fiemap_extent)]{};
  auto* map = reinterpret_cast<struct fiemap*>(request);
  map->fm_start = 0;
  map->fm_length = std::numeric_limits<uint64_t>::max();
  map->fm_flags = 0;
  map->fm_extent_count = 1;
  if (::ioctl(fd, FS_IOC_FIEMAP, map) != 0 || map->fm_mapped_extents == 0) return std::nullopt;
  return map->fm_extents[0].fe_physical;
}
#endif

}  // namespace

std::vector<size_t> disk_order(const std::vector<std::filesystem::path>& files) {
  std::vector<Place> places(files.size());
  for (size_t i = 0; i < files.size(); ++i) {
    const int fd = ::open(files[i].c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + files[i].string() + ": " + std::strerror(errno));
    struct stat st{};
    const bool stated = ::fstat(fd, &st) == 0;
    const int saved = errno;
#if defined(__linux__)
    const auto extent = first_extent(fd);
#else
    const std::optional<uint64_t> extent;
#endif
    ::close(fd);
    if (!stated) throw std::runtime_error("cannot stat " + files[i].string() + ": " + std::strerror(saved));
    if (extent)
      places[i] = {0, *extent};
    else
      places[i] = {1, static_cast<uint64_t>(st.st_ino)};
  }
  std::vector<size_t> order(files.size());
  std::iota(order.begin(), order.end(), size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (places[a].rank != places[b].rank) return places[a].rank < places[b].rank;
    return places[a].where < places[b].where;
  });
  return order;
}

}  // namespace dagda::migrate
