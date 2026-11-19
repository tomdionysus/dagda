//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// What the ledger's tests share: keys, a logger that keeps what it is told,
// and Macha's trie helpers from tests/test_object_ledger.cpp (develop
// 5a06d3c).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/domain/storage_key.h"
#include "dagda/ledger/object_trie.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/secure_memory.h"
#include "dannan/status/log.h"

namespace dagda::test {

using ledger::Bytes;
using ledger::ObjectId;
using ledger::ObjectTrie;

// Keeps every record it is given, at every level. Thread-safe.
class RecordingSink final : public dannan::status::LogSink {
 public:
  bool enabled(dannan::status::LogLevel) const noexcept override { return true; }
  void write(dannan::status::LogLevel level, std::string_view scope, std::string_view message) override {
    dannan::contract::Lock lock(_mutex);
    _lines.push_back(std::string(scope) + ": " + std::string(message));
    _levels.push_back(level);
  }
  std::vector<std::string> lines() {
    dannan::contract::Lock lock(_mutex);
    return _lines;
  }
  bool saw(std::string_view text) {
    const auto all = lines();
    return std::any_of(all.begin(), all.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
  }
  // A line holding `text`, written at `level`.
  bool saw(dannan::status::LogLevel level, std::string_view text) {
    dannan::contract::Lock lock(_mutex);
    for (size_t i = 0; i < _lines.size(); ++i)
      if (_levels[i] == level && _lines[i].find(text) != std::string::npos) return true;
    return false;
  }

 private:
  dannan::contract::Mutex _mutex;
  std::vector<std::string> _lines DANNAN_GUARDED_BY(_mutex);
  std::vector<dannan::status::LogLevel> _levels DANNAN_GUARDED_BY(_mutex);
};

inline dannan::status::ScopedLogger logger_into(std::shared_ptr<RecordingSink> sink) { return dannan::status::ScopedLogger(std::move(sink), "ledger"); }

inline dannan::status::ScopedLogger quiet_logger() { return logger_into(std::make_shared<RecordingSink>()); }

// Every byte `fill`, as Macha's tests key their tries.
inline std::shared_ptr<const domain::StorageKey> key_of(uint8_t fill) {
  std::array<uint8_t, 32> key{};
  key.fill(fill);
  return std::make_shared<const domain::StorageKey>(dannan::identity::Secret(key));
}

// Domain v1's storage key from 1a's known-answer cluster key file
// ("dagda known-answer cluster key\r\n"), pinned in domain_storage_key_test.
inline std::shared_ptr<const domain::StorageKey> known_answer_key() {
  const auto bytes = dannan::identity::unhex("879277d5274170fabe955dd8b14c14edbd54533790e24617b7cff11a9679bbd8").value();
  return std::make_shared<const domain::StorageKey>(dannan::identity::Secret(bytes));
}

inline ObjectId id_of(std::mt19937_64& random) {
  ObjectId id;
  for (auto& byte : id.bytes) byte = static_cast<uint8_t>(random());
  return id;
}

// Of varying length, empty included.
inline Bytes value_of(std::mt19937_64& random) {
  Bytes value(random() % 24);
  for (auto& byte : value) byte = static_cast<uint8_t>(random());
  return value;
}

// Ids that share leading bytes, so subtrees run several levels deep.
inline ObjectId clustered_id(std::mt19937_64& random) {
  auto id = id_of(random);
  id.bytes[0] = static_cast<uint8_t>(random() % 4);
  id.bytes[1] = static_cast<uint8_t>(random() % 3);
  if (random() % 2) id.bytes[2] = 9;
  return id;
}

inline ObjectTrie::Options small_options() {
  ObjectTrie::Options options;
  options.cache_bytes = 64 * 1024;
  options.checkpoint_bytes = 64 * 1024;
  options.rewrite_floor_bytes = 32 * 1024;
  return options;
}

// Everything the trie holds, read back in order through next().
inline std::map<ObjectId, Bytes> contents(const ObjectTrie& trie) {
  std::map<ObjectId, Bytes> out;
  std::optional<ObjectId> after;
  while (true) {
    const auto page = trie.next(after, 97);
    for (const auto& [id, value] : page) out.emplace(id, value);
    if (page.size() < 97) break;
    after = page.back().first;
  }
  return out;
}

inline void check_matches(const ObjectTrie& trie, const std::map<ObjectId, Bytes>& model, std::mt19937_64& random) {
  EXPECT_EQ(trie.size(), model.size());
  EXPECT_TRUE(contents(trie) == model);
  for (const auto& [id, value] : model) EXPECT_TRUE(trie.get(id) == std::optional<Bytes>(value));
  for (int i = 0; i < 50; ++i) EXPECT_FALSE(trie.get(id_of(random)).has_value());
}

// Unsaved until the caller checkpoints: in-memory nodes and saved ones are
// both read by the tests that need each.
inline std::unique_ptr<ObjectTrie> trie_of(const std::filesystem::path& dir, const std::map<ObjectId, Bytes>& records, std::mt19937_64& random) {
  auto options = small_options();
  options.checkpoint_bytes = 64ULL * 1024 * 1024;
  auto trie = std::make_unique<ObjectTrie>(dir, key_of(8), options, quiet_logger());
  std::vector<ObjectTrie::Change> changes;
  for (const auto& [id, value] : records) changes.push_back({id, value});
  std::shuffle(changes.begin(), changes.end(), random);
  for (size_t from = 0; from < changes.size(); from += 997)
    trie->apply(std::span<const ObjectTrie::Change>(changes).subspan(from, std::min<size_t>(997, changes.size() - from)));
  return trie;
}

inline ObjectTrie::Prefix prefix_of(const ObjectId& id, uint8_t length) {
  ObjectTrie::Prefix prefix;
  std::copy_n(id.bytes.begin(), length, prefix.bytes.begin());
  prefix.length = length;
  return prefix;
}

}  // namespace dagda::test
