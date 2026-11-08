//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// Macha's metadata as opaque bytes, for the migration's tests: where Macha's
// fixtures are, their test cluster key, the hex states checked in, and an owner
// that holds a payload as bytes and reads no more of it than the head section's
// clock and merge parents (as heads_replica_bytes_test.cpp's, which it mirrors).
// It cannot apply a delta: only Macha's codec can (the heads plan's section 4.3).

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>

#include "dagda/domain/storage_key.h"
#include "dagda/heads/codec.h"
#include "dagda/heads/record.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::test::macha {

using heads::Bytes;
using heads::Codec;
using heads::DecodeError;
using heads::HeadFields;
using heads::State;

inline constexpr char fixture_cluster_key[] = "macha-fixture-cluster-key\n";

inline std::filesystem::path fixtures() {
  if (const char* given = std::getenv("DAGDA_MACHA_FIXTURES")) return given;
  return "/Users/tom/devroot/macha/build/macha-fixtures";
}

inline std::filesystem::path checked_in() { return std::filesystem::path(DAGDA_SOURCE_DIR) / "dagda/test/data/replica-0.93.4"; }

inline Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

inline void write_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A checked-in state: file name to bytes.
inline std::map<std::string, Bytes> read_checked_in(const std::string& name) {
  std::map<std::string, Bytes> out;
  std::ifstream in(checked_in() / (name + ".txt"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto gap = line.find(' ');
    const auto value = dannan::identity::unhex(line.substr(gap + 1));
    if (!value) throw std::runtime_error("bad hex in " + name);
    out[line.substr(0, gap)] = *value;
  }
  return out;
}

struct OpaqueState {
  Bytes payload;
};

// The head section as Macha's SM forms lay it out, read as far as the merge parents.
inline HeadFields opaque_fields(std::span<const uint8_t> payload) {
  static const Bytes genesis{'D', 'H', 'T', 'M', 'G', 'E', 'N', '1'};
  if (std::equal(payload.begin(), payload.end(), genesis.begin(), genesis.end())) return {};
  HeadFields out;
  out.floor = 1;
  dannan::net::Reader in(payload);
  const auto magic = in.view(8);
  const std::string form(magic.begin(), magic.end());
  if (form != "DHTMETB4" && form != "DHTMETB6" && form != "DHTMETB7" && form != "DHTMETB9") throw DecodeError("not a whole record");
  const auto voters = in.u32();
  (void)in.view(size_t{voters} * 16);
  (void)in.view(4 + 8);
  out.clock = heads::decode_clock(in);
  (void)in.view(32);
  if (in.u8() != 0) (void)in.view(32);
  const auto garbage = in.u32();
  (void)in.view(size_t{garbage} * 56);
  if (in.u32() != 0 || in.u32() != 0) throw DecodeError("a status or reset section this test does not read");
  out.merge_parents = heads::decode_merge_parents(in);
  return out;
}

class OpaqueCodec final : public Codec {
 public:
  using Known = std::map<heads::Hash256, Bytes>;
  explicit OpaqueCodec(Known known = {}) : _known(std::move(known)) {}

  Bytes genesis() const override { return Bytes{'D', 'H', 'T', 'M', 'G', 'E', 'N', '1'}; }
  State decode_full(std::span<const uint8_t> payload) const override {
    (void)opaque_fields(payload);
    return std::make_shared<const OpaqueState>(OpaqueState{Bytes(payload.begin(), payload.end())});
  }
  Bytes encode_full(const State& state) const override { return as_state(state).payload; }
  std::optional<Bytes> encode_delta(const State&, const State&) const override { return std::nullopt; }
  void check_delta(std::span<const uint8_t> delta) const override {
    if (heads::form_version(delta, "DHTMDLT") < 5) throw DecodeError("bad metadata delta");
  }
  State apply_delta(const State&, std::span<const uint8_t> delta, heads::TreeNodeStore*) const override {
    const auto found = _known.find(dannan::identity::sha256(delta));
    if (found == _known.end()) throw DecodeError("only the owner's codec can apply this delta");
    return std::make_shared<const OpaqueState>(OpaqueState{found->second});
  }
  Bytes encode_after_delta(std::span<const uint8_t>, const State& state) const override { return as_state(state).payload; }
  HeadFields head_fields(const State& state) const override { return opaque_fields(as_state(state).payload); }
  uint64_t resident_bytes(const State& state) const override { return as_state(state).payload.size(); }

 private:
  static const OpaqueState& as_state(const State& state) { return *static_cast<const OpaqueState*>(state.get()); }
  const Known _known;
};

}  // namespace dagda::test::macha
