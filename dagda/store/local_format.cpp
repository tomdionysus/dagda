//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/local_format.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "dannan/net/codec.h"

namespace dagda::store::local_format {
namespace {

using dannan::net::Reader;
using dannan::net::Writer;

}  // namespace

Bytes encode_loose_header(uint64_t plain_size, const dannan::identity::SealedData& sealed) {
  Writer header;
  header.raw(loose_magic);
  header.u64(plain_size);
  header.fixed(sealed.nonce);
  header.fixed(sealed.tag);
  return header.take();
}

LooseObject decode_loose(std::span<const uint8_t> file) {
  Reader reader(file);
  const auto magic = reader.view(loose_magic.size());
  if (!std::equal(magic.begin(), magic.end(), loose_magic.begin())) throw std::runtime_error("bad object header");
  LooseObject out;
  out.plain_size = reader.u64();
  out.nonce = reader.fixed<12>();
  out.tag = reader.fixed<16>();
  out.ciphertext = reader.view(reader.remaining());
  return out;
}

std::filesystem::path loose_path(const std::filesystem::path& objects, const ObjectId& id) {
  const auto name = to_string(id);
  return objects / name.substr(0, 2) / name.substr(2, 2) / (name + ".obj");
}

std::optional<ObjectId> object_id_from_file_name(std::string_view name) {
  if (name.size() != 64 + 4 || !name.ends_with(".obj")) return std::nullopt;
  ObjectId id;
  for (size_t i = 0; i < 32; ++i) {
    unsigned value = 0;
    for (size_t nibble = 0; nibble < 2; ++nibble) {
      const char c = name[i * 2 + nibble];
      unsigned digit = 0;
      if (c >= '0' && c <= '9')
        digit = static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        digit = static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        digit = static_cast<unsigned>(c - 'A' + 10);
      else
        return std::nullopt;
      value = (value << 4) | digit;
    }
    id.bytes[i] = static_cast<uint8_t>(value);
  }
  return id;
}

Bytes encode_pack_header(const PackHeader& header) {
  Writer prefix;
  prefix.raw(pack_magic);
  prefix.u8(header.type);
  prefix.u64(header.touched_ms);
  prefix.fixed(header.id.bytes);
  prefix.u64(header.plain_size);
  prefix.u64(header.payload_size);
  prefix.fixed(header.nonce);
  prefix.fixed(header.tag);
  if (prefix.data().size() != pack_header_prefix_size) throw std::logic_error("internal pack header size error");
  const auto checksum = dannan::identity::sha256(prefix.data());
  Writer out;
  out.raw(prefix.data());
  out.fixed(checksum.bytes);
  return out.take();
}

std::optional<PackHeader> decode_pack_header(std::span<const uint8_t> bytes) {
  if (bytes.size() != pack_header_size) return std::nullopt;
  const auto prefix = bytes.first(pack_header_prefix_size);
  const auto checksum = dannan::identity::sha256(prefix);
  if (!std::equal(checksum.bytes.begin(), checksum.bytes.end(), bytes.begin() + pack_header_prefix_size)) return std::nullopt;
  try {
    Reader reader(prefix);
    const auto magic = reader.view(pack_magic.size());
    if (!std::equal(magic.begin(), magic.end(), pack_magic.begin())) return std::nullopt;
    PackHeader header;
    header.type = reader.u8();
    header.touched_ms = reader.u64();
    header.id.bytes = reader.fixed<32>();
    header.plain_size = reader.u64();
    header.payload_size = reader.u64();
    header.nonce = reader.fixed<12>();
    header.tag = reader.fixed<16>();
    reader.finish();
    if (header.type < pack_put || header.type > pack_touch) return std::nullopt;
    if (header.type == pack_put) {
      if (!header.payload_size || header.payload_size > pack_payload_safety_limit || header.plain_size != header.payload_size) return std::nullopt;
    } else if (header.payload_size || header.plain_size) {
      return std::nullopt;
    }
    return header;
  } catch (const dannan::net::DecodeError&) {
    return std::nullopt;
  }
}

std::optional<uint64_t> find_next_pack_header(int fd, uint64_t from, uint64_t file_size) {
  constexpr size_t chunk = 1024 * 1024;
  static_assert(chunk > 2 * pack_header_size);
  uint64_t position = from;
  while (position + pack_header_size <= file_size) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(chunk, file_size - position));
    Bytes bytes(want);
    size_t done = 0;
    while (done < want) {
      const auto n = ::pread(fd, bytes.data() + done, want - done, static_cast<off_t>(position + done));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("cannot read pack during recovery");
      done += static_cast<size_t>(n);
    }
    const std::span<const uint8_t> view(bytes);
    size_t search = 0;
    while (search + pack_header_size <= view.size()) {
      const auto hit = std::search(view.begin() + static_cast<std::ptrdiff_t>(search), view.end(), pack_magic.begin(), pack_magic.end());
      if (hit == view.end()) break;
      const size_t index = static_cast<size_t>(hit - view.begin());
      if (index + pack_header_size > view.size()) break;
      if (decode_pack_header(view.subspan(index, pack_header_size))) return position + index;
      search = index + 1;
    }
    if (want < chunk) break;
    // Overlap the next chunk by a header, so one straddling the boundary is
    // still seen whole.
    position += want - (pack_header_size - 1);
  }
  return std::nullopt;
}

std::string pack_name(uint64_t sequence) {
  std::ostringstream out;
  out << "pack-" << std::setw(20) << std::setfill('0') << sequence << ".pack";
  return out.str();
}

std::optional<uint64_t> pack_sequence(std::string_view name) {
  if (name.size() != 30 || !name.starts_with("pack-") || !name.ends_with(".pack")) return std::nullopt;
  try {
    size_t used = 0;
    const auto value = std::stoull(std::string(name.substr(5, 20)), &used, 10);
    if (used != 20 || !value) return std::nullopt;
    return value;
  } catch (const std::logic_error&) {
    return std::nullopt;
  }
}

Bytes encode_accounting(const AccountingRecord& record) {
  Writer body;
  body.raw(accounting_magic);
  body.u64(record.sequence);
  body.u64(record.used);
  body.u8(record.operation);
  body.u64(record.size);
  body.fixed(record.id.bytes);
  while (body.data().size() < accounting_body_size) body.u8(0);
  if (body.data().size() != accounting_body_size) throw std::logic_error("internal accounting record size error");
  const auto hash = dannan::identity::sha256(body.data());
  Writer out;
  out.raw(body.data());
  out.fixed(hash.bytes);
  return out.take();
}

std::optional<AccountingRecord> decode_accounting(std::span<const uint8_t> bytes) {
  if (bytes.size() != accounting_slot_size) return std::nullopt;
  const auto body = bytes.first(accounting_body_size);
  const auto expected = dannan::identity::sha256(body);
  if (!std::equal(expected.bytes.begin(), expected.bytes.end(), bytes.begin() + accounting_body_size)) return std::nullopt;
  try {
    Reader reader(body);
    const auto magic = reader.view(accounting_magic.size());
    if (!std::equal(magic.begin(), magic.end(), accounting_magic.begin())) return std::nullopt;
    AccountingRecord record;
    record.sequence = reader.u64();
    record.used = reader.u64();
    record.operation = reader.u8();
    record.size = reader.u64();
    record.id.bytes = reader.fixed<32>();
    if (record.operation != accounting_clean && record.operation != accounting_dirty) return std::nullopt;
    return record;
  } catch (const dannan::net::DecodeError&) {
    return std::nullopt;
  }
}

}  // namespace dagda::store::local_format
