//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/node_pack.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "dagda/store/object_id.h"

namespace dagda::heads {
namespace {

using dannan::contract::Lock;

// The journal's own associated data: what kind of file this is.
constexpr std::array<uint8_t, 8> pack_aad{'D', 'G', 'N', 'P', 'A', 'C', 'K', '1'};
// be32 length, nonce, tag: what a frame adds to its plaintext.
constexpr uint64_t frame_overhead = 4 + 12 + 16;
constexpr size_t binding_bytes = sizeof(ObjectId{}.bytes);

void sync_directory(const std::filesystem::path& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot open " + directory.string() + " to sync it: " + std::strerror(errno));
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  const auto error = errno;
  ::close(fd);
  if (rc != 0) throw std::runtime_error("cannot sync " + directory.string() + ": " + std::strerror(error));
}

}  // namespace

NodePack::NodePack(std::filesystem::path directory, std::shared_ptr<const domain::StorageKey> key, Bytes aad, dannan::status::ScopedLogger logger)
    : _directory(std::move(directory)), _key(std::move(key)), _binding(store::object_id(aad)), _logger(std::move(logger)) {
  if (!_key) throw std::invalid_argument("a node pack needs a key");
  std::filesystem::create_directories(_directory);
  Lock lock(_mutex);
  replay_locked();
}

Bytes NodePack::frame_of(std::span<const uint8_t> node) const {
  Bytes out;
  out.reserve(binding_bytes + node.size());
  out.insert(out.end(), _binding.bytes.begin(), _binding.bytes.end());
  out.insert(out.end(), node.begin(), node.end());
  return out;
}

void NodePack::replay_locked() {
  const auto path = file(_directory);
  // Replay cuts at the first frame that will not open or that its reader refuses, which
  // under the wrong key is the whole file. So look first, changing nothing, and refuse
  // anything but a torn tail: only that may be cut.
  const auto bound = [this](std::span<const uint8_t> plaintext) {
    if (plaintext.size() < binding_bytes || !std::equal(_binding.bytes.begin(), _binding.bytes.end(), plaintext.begin()))
      throw std::runtime_error("a node of another space");
  };
  const auto scan = ledger::SealedJournal::read_only(path, *_key, pack_aad, max_node_bytes + binding_bytes, bound);
  if (scan.stopped_at_a_whole_frame)
    throw std::runtime_error("node pack " + path.string() + " holds a frame that does not open here, at frame " + std::to_string(scan.frames) +
                             ": damaged, another node's or another space's; nothing was changed");
  _journal = std::make_unique<ledger::SealedJournal>(path, _key, pack_aad, max_node_bytes + binding_bytes, _logger);
  std::map<ObjectId, uint64_t> index;
  uint64_t offset = 0;
  _journal->replay(
      [&](std::span<const uint8_t> plaintext) {
        index.emplace(store::object_id(plaintext.subspan(binding_bytes)), offset);
        offset += frame_overhead + plaintext.size();
      },
      max_node_bytes + binding_bytes, "node pack");
  _index = std::move(index);
}

ObjectId NodePack::put(std::span<const uint8_t> node) {
  if (node.size() > max_node_bytes) throw std::runtime_error("a tree node of " + std::to_string(node.size()) + " bytes is over the node pack's limit");
  const auto id = store::object_id(node);
  Lock lock(_mutex);
  if (_index.contains(id)) return id;
  const auto offset = _journal->append_unsynced(frame_of(node));
  _journal->sync();
  _index.emplace(id, offset);
  return id;
}

std::optional<Bytes> NodePack::get(const ObjectId& id) const {
  Lock lock(_mutex);
  const auto found = _index.find(id);
  if (found == _index.end()) return std::nullopt;
  auto plaintext = _journal->read_at(found->second);
  return Bytes(plaintext.begin() + binding_bytes, plaintext.end());
}

bool NodePack::contains(const ObjectId& id) const {
  Lock lock(_mutex);
  return _index.contains(id);
}

NodePack::Compaction NodePack::compact(const std::set<ObjectId>& live) {
  Lock lock(_mutex);
  Compaction out;
  out.bytes_before = _journal->bytes();
  const auto path = file(_directory);
  const auto beside = _directory / "nodes.pack.compacting";
  std::filesystem::remove(beside);
  {
    ledger::SealedJournal fresh(beside, _key, pack_aad, max_node_bytes + binding_bytes, _logger);
    for (const auto& [id, offset] : _index) {
      if (!live.contains(id)) {
        ++out.dropped;
        continue;
      }
      (void)fresh.append_unsynced(_journal->read_at(offset));
      ++out.kept;
    }
    fresh.sync();
  }
  // An empty pack is no file: a journal is created on its first append.
  if (!std::filesystem::exists(beside)) std::ofstream(beside, std::ios::binary).close();
  _journal.reset();
  std::filesystem::rename(beside, path);
  sync_directory(_directory);
  replay_locked();
  out.bytes_after = _journal->bytes();
  return out;
}

size_t NodePack::nodes() const {
  Lock lock(_mutex);
  return _index.size();
}

uint64_t NodePack::bytes() const {
  Lock lock(_mutex);
  return _journal->bytes();
}

namespace {

class PackFacade final : public TreeNodeStore {
 public:
  explicit PackFacade(NodePack& pack) : _pack(pack) {}
  ObjectId put(std::span<const uint8_t> node) override { return _pack.put(node); }
  std::optional<Bytes> get(const ObjectId& id) const override { return _pack.get(id); }

 private:
  NodePack& _pack;
};

}  // namespace

std::function<std::unique_ptr<TreeNodeStore>()> NodePack::factory() {
  return [this]() -> std::unique_ptr<TreeNodeStore> { return std::make_unique<PackFacade>(*this); };
}

}  // namespace dagda::heads
