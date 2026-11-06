//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/migrate/manifest_format.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "dannan/net/codec.h"

namespace dagda::migrate::manifest_format {
namespace {

using dannan::net::DecodeError;
using dannan::net::Reader;
using dannan::net::Writer;

// The largest a block may claim: its records are read whole into memory.
constexpr uint32_t max_block = 1024U * 1024U * 1024U;

bool read_all(int fd, std::span<uint8_t> into, uint64_t offset) {
  size_t done = 0;
  while (done < into.size()) {
    const auto n = ::pread(fd, into.data() + done, into.size() - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

void write_strings(Writer& writer, const std::vector<std::string>& strings) {
  writer.u32(static_cast<uint32_t>(strings.size()));
  for (const auto& string : strings) writer.string(string);
}

std::vector<std::string> read_strings(Reader& reader) {
  std::vector<std::string> out;
  const auto count = reader.u32();
  for (uint32_t i = 0; i < count; ++i) out.push_back(reader.string());
  return out;
}

}  // namespace

Bytes encode(const LooseValue& value) {
  Writer writer;
  writer.u8(value.layout);
  writer.u64(value.plain_size);
  writer.u64(value.file_size);
  writer.u64(static_cast<uint64_t>(value.mtime_ns));
  writer.fixed(value.nonce);
  writer.fixed(value.tag);
  return writer.take();
}

LooseValue decode_loose_value(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  LooseValue value;
  value.layout = reader.u8();
  value.plain_size = reader.u64();
  value.file_size = reader.u64();
  value.mtime_ns = static_cast<int64_t>(reader.u64());
  value.nonce = reader.fixed<12>();
  value.tag = reader.fixed<16>();
  reader.finish();
  return value;
}

Bytes encode(const PackValue& value) {
  Writer writer;
  writer.u8(value.span);
  if (value.span == span_record) {
    writer.u8(value.layout);
    writer.u8(value.header.type);
    writer.u64(value.header.touched_ms);
    writer.fixed(value.header.id.bytes);
    writer.u64(value.header.plain_size);
    writer.u64(value.header.payload_size);
    writer.fixed(value.header.nonce);
    writer.fixed(value.header.tag);
  } else {
    writer.u64(value.length);
    writer.fixed(value.digest.bytes);
  }
  return writer.take();
}

PackValue decode_pack_value(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  PackValue value;
  value.span = reader.u8();
  if (value.span == span_record) {
    value.layout = reader.u8();
    value.header.type = reader.u8();
    value.header.touched_ms = reader.u64();
    value.header.id.bytes = reader.fixed<32>();
    value.header.plain_size = reader.u64();
    value.header.payload_size = reader.u64();
    value.header.nonce = reader.fixed<12>();
    value.header.tag = reader.fixed<16>();
  } else {
    value.length = reader.u64();
    value.digest.bytes = reader.fixed<32>();
  }
  reader.finish();
  return value;
}

Bytes encode(const MarkerValue& value) {
  Writer writer;
  writer.u8(value.layout);
  writer.string(value.node);
  writer.string(value.token);
  writer.string(value.text);
  return writer.take();
}

MarkerValue decode_marker_value(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  MarkerValue value;
  value.layout = reader.u8();
  value.node = reader.string();
  value.token = reader.string();
  value.text = reader.string();
  reader.finish();
  return value;
}

Bytes encode(const AccountingValue& value) {
  Writer writer;
  writer.u8(value.decoded ? 1 : 0);
  writer.u64(value.sequence);
  writer.u64(value.used);
  writer.u8(value.operation);
  writer.fixed(value.digest.bytes);
  return writer.take();
}

AccountingValue decode_accounting_value(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  AccountingValue value;
  value.decoded = reader.u8() != 0;
  value.sequence = reader.u64();
  value.used = reader.u64();
  value.operation = reader.u8();
  value.digest.bytes = reader.fixed<32>();
  reader.finish();
  return value;
}

Bytes encode(const OtherValue& value) {
  Writer writer;
  writer.u64(value.size);
  writer.fixed(value.digest.bytes);
  return writer.take();
}

OtherValue decode_other_value(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  OtherValue value;
  value.size = reader.u64();
  value.digest.bytes = reader.fixed<32>();
  reader.finish();
  return value;
}

Bytes number_key(uint64_t number) {
  Writer writer;
  writer.u64(number);
  return writer.take();
}

Bytes pack_key(uint64_t sequence, uint64_t offset) {
  Writer writer;
  writer.u64(sequence);
  writer.u64(offset);
  return writer.take();
}

uint64_t number_of(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  return reader.u64();
}

Bytes encode_record(const Record& record) {
  Writer writer;
  writer.u8(static_cast<uint8_t>(record.type));
  switch (record.type) {
    case RecordType::header:
      writer.u32(record.header.layout);
      writer.string(record.header.domain);
      writer.string(record.header.node);
      writer.string(record.header.key_check);
      write_strings(writer, record.header.stores);
      write_strings(writer, record.header.accounting_digests);
      break;
    case RecordType::begin:
      writer.string(record.begin.order);
      writer.string(record.begin.display);
      writer.u8(static_cast<uint8_t>(record.begin.kind));
      writer.string(record.begin.not_checked);
      break;
    case RecordType::item:
      writer.bytes(record.key);
      writer.bytes(record.value);
      break;
    case RecordType::end:
      writer.u64(record.count);
      break;
    case RecordType::trailer:
      writer.u64(record.count);
      writer.u64(record.sections);
      break;
  }
  return writer.take();
}

ManifestReader::ManifestReader(const std::filesystem::path& path, bool lenient) : _path(path), _lenient(lenient) {
  _fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (_fd < 0) throw ManifestDamaged("cannot open manifest " + path.string() + ": " + std::strerror(errno));
  struct stat st{};
  if (::fstat(_fd, &st) != 0 || st.st_size < 0) throw ManifestDamaged("cannot stat manifest " + path.string());
  _size = static_cast<uint64_t>(st.st_size);
  std::array<uint8_t, magic.size() + 4> head{};
  if (_size < head.size() || !read_all(_fd, head, 0) || !std::equal(magic.begin(), magic.end(), head.begin()))
    throw ManifestDamaged("manifest " + path.string() + " is not a manifest");
  Reader version(std::span<const uint8_t>(head).subspan(magic.size()));
  if (version.u32() != format_version) throw ManifestDamaged("manifest " + path.string() + " is of another version");
  _offset = head.size();
  _block_end = _offset;
}

ManifestReader::~ManifestReader() {
  if (_fd >= 0) ::close(_fd);
}

bool ManifestReader::next_block() {
  if (_offset >= _size) return false;
  if (_offset >= _limit) return false;
  const auto refuse = [&](const std::string& why) -> bool {
    if (_lenient) return false;
    throw ManifestDamaged("manifest " + _path.string() + " is damaged at " + std::to_string(_offset) + ": " + why);
  };
  std::array<uint8_t, 4> length_bytes{};
  if (_size - _offset < length_bytes.size() + 32 || !read_all(_fd, length_bytes, _offset)) return refuse("a torn block");
  Reader length_reader{std::span<const uint8_t>(length_bytes)};
  const auto length = length_reader.u32();
  if (length > max_block || _size - _offset < length_bytes.size() + static_cast<uint64_t>(length) + 32) return refuse("a torn block");
  Bytes payload(length);
  std::array<uint8_t, 32> stored{};
  if (!read_all(_fd, payload, _offset + length_bytes.size()) || !read_all(_fd, stored, _offset + length_bytes.size() + length)) return refuse("a short read");
  dannan::identity::Sha256Hasher hasher;
  hasher.update(_chain.bytes);
  hasher.update(payload);
  const auto chained = hasher.finish();
  if (chained.bytes != stored) return refuse("a block whose checksum does not hold");
  _chain = chained;
  _payload = std::move(payload);
  _at = 0;
  _offset += length_bytes.size() + length + 32;
  _block_end = _offset;
  return true;
}

std::optional<Record> ManifestReader::next() {
  if (_ended) return std::nullopt;
  while (_at >= _payload.size()) {
    if (next_block()) continue;
    if (_lenient) return std::nullopt;
    throw ManifestDamaged("manifest " + _path.string() + " ends with no trailer: it is truncated");
  }
  try {
    Reader reader(std::span<const uint8_t>(_payload).subspan(_at));
    const auto before = reader.remaining();
    Record record;
    record.type = static_cast<RecordType>(reader.u8());
    switch (record.type) {
      case RecordType::header:
        record.header.layout = reader.u32();
        record.header.domain = reader.string();
        record.header.node = reader.string();
        record.header.key_check = reader.string();
        record.header.stores = read_strings(reader);
        record.header.accounting_digests = read_strings(reader);
        break;
      case RecordType::begin:
        record.begin.order = reader.string();
        record.begin.display = reader.string();
        record.begin.kind = static_cast<SectionKind>(reader.u8());
        if (record.begin.kind < SectionKind::loose || record.begin.kind > SectionKind::not_checked) throw DecodeError("a section of an unknown kind");
        record.begin.not_checked = reader.string();
        break;
      case RecordType::item:
        record.key = reader.bytes();
        record.value = reader.bytes();
        break;
      case RecordType::end:
        record.count = reader.u64();
        break;
      case RecordType::trailer:
        record.count = reader.u64();
        record.sections = reader.u64();
        // Nothing follows the trailer: not in its block, not in the file.
        if (reader.remaining() != 0 || _offset != _size) throw ManifestDamaged("manifest " + _path.string() + " has bytes after its trailer");
        _ended = true;
        break;
      default:
        throw ManifestDamaged("manifest " + _path.string() + " holds an unknown record");
    }
    _at += before - reader.remaining();
    return record;
  } catch (const DecodeError& error) {
    if (_lenient) return std::nullopt;
    throw ManifestDamaged("manifest " + _path.string() + " holds a record that does not decode: " + error.what());
  }
}

Header verify_manifest(const std::filesystem::path& path) {
  ManifestReader reader(path);
  const auto first = reader.next();
  if (!first || first->type != RecordType::header) throw ManifestDamaged("manifest " + path.string() + " has no header");
  std::optional<std::string> order;
  bool open = false;
  uint64_t items = 0;
  uint64_t in_section = 0;
  uint64_t sections = 0;
  Bytes last_key;
  while (const auto record = reader.next()) {
    switch (record->type) {
      case RecordType::begin:
        if (open || (order && !(*order < record->begin.order))) throw ManifestDamaged("manifest " + path.string() + " has its sections out of order");
        order = record->begin.order;
        open = true;
        in_section = 0;
        last_key.clear();
        ++sections;
        break;
      case RecordType::item:
        if (!open || (in_section && !(last_key < record->key))) throw ManifestDamaged("manifest " + path.string() + " has its items out of order");
        last_key = record->key;
        ++in_section;
        ++items;
        break;
      case RecordType::end:
        if (!open || record->count != in_section) throw ManifestDamaged("manifest " + path.string() + " has a section short of its count");
        open = false;
        break;
      case RecordType::trailer:
        if (open || record->count != items || record->sections != sections)
          throw ManifestDamaged("manifest " + path.string() + " is short of its trailer's counts");
        return first->header;
      case RecordType::header:
        throw ManifestDamaged("manifest " + path.string() + " has a second header");
    }
  }
  throw ManifestDamaged("manifest " + path.string() + " ends with no trailer: it is truncated");
}

}  // namespace dagda::migrate::manifest_format
