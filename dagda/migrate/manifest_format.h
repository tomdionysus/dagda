//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dagda/migrate/manifest.h"
#include "dagda/store/local_format.h"
#include "dagda/store/object_id.h"
#include "dannan/identity/crypto.h"

// A manifest's bytes, shared by the pass and the comparison. The file is the
// magic, then blocks, each be32 length | payload | SHA-256 chained over the
// block before and this payload; the last block ends with the trailer and
// nothing may follow it. A payload is records: the header, then sections in
// strictly increasing order, each with its items in strictly increasing key
// order and a closing count. Both orders are checked on read, so the comparison
// is a merge of two streams and its memory does not grow with the state.
namespace dagda::migrate::manifest_format {

using store::ObjectId;

inline constexpr std::array<uint8_t, 8> magic{'D', 'A', 'G', 'D', 'A', 'M', 'A', 'N'};
// 2: a metadata item's value says whether its meaning can be known.
inline constexpr uint32_t format_version = 2;

enum class RecordType : uint8_t { header = 1, begin = 2, item = 3, end = 4, trailer = 5 };

// What a section's items are, which decides how two are compared and named.
enum class SectionKind : uint8_t {
  loose = 1,
  pack_files = 2,
  pack_records = 3,
  removed = 4,
  accounting = 5,
  markers = 6,
  bytes = 7,
  held = 8,
  claims = 9,
  claim_frames = 10,
  counts = 11,
  sightings = 12,
  other = 13,
  metadata = 14,
  not_checked = 15,
};

// A sealed record's state, in the manifest: the layout that opened it, or why
// none did.
inline constexpr uint8_t record_v18 = 18;
inline constexpr uint8_t record_v19 = 19;
inline constexpr uint8_t record_empty = 1;
inline constexpr uint8_t record_unreadable = 2;
inline constexpr uint8_t record_unsealed = 3;

struct Header {
  uint32_t layout = 0;
  std::string domain;
  std::string node;
  // Of the cluster key: two passes under different keys are not comparable.
  std::string key_check;
  // Every store's label, sorted, and what fixes a pass that resumes to the
  // state it began on.
  std::vector<std::string> stores;
  std::vector<std::string> accounting_digests;
  bool operator==(const Header&) const = default;
};

struct Begin {
  std::string order;
  std::string display;
  SectionKind kind = SectionKind::bytes;
  // Empty unless the whole section could not be checked.
  std::string not_checked;
};

struct LooseValue {
  uint8_t layout = record_unreadable;
  uint64_t plain_size = 0;
  uint64_t file_size = 0;
  int64_t mtime_ns = 0;
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
};
Bytes encode(const LooseValue&);
LooseValue decode_loose_value(std::span<const uint8_t>);

// A pack record, or a span no record could be read from.
inline constexpr uint8_t span_record = 1;
inline constexpr uint8_t span_damaged = 2;
inline constexpr uint8_t span_tail = 3;

struct PackValue {
  uint8_t span = span_record;
  uint8_t layout = record_unsealed;
  store::local_format::PackHeader header{};
  // Of a damaged span or a tail: its length and SHA-256.
  uint64_t length = 0;
  dannan::identity::Hash256 digest{};
  // The bytes from this key's offset to the next.
  uint64_t total() const noexcept { return span == span_record ? store::local_format::pack_header_size + header.payload_size : length; }
};
Bytes encode(const PackValue&);
PackValue decode_pack_value(std::span<const uint8_t>);

struct MarkerValue {
  // 18, 19, or 0 for a marker that is neither.
  uint8_t layout = 0;
  std::string node;
  std::string token;
  std::string text;
};
Bytes encode(const MarkerValue&);
MarkerValue decode_marker_value(std::span<const uint8_t>);

struct AccountingValue {
  // Whether a slot decoded.
  bool decoded = false;
  uint64_t sequence = 0;
  uint64_t used = 0;
  uint8_t operation = 0;
  dannan::identity::Hash256 digest{};
};
Bytes encode(const AccountingValue&);
AccountingValue decode_accounting_value(std::span<const uint8_t>);

struct OtherValue {
  uint64_t size = 0;
  dannan::identity::Hash256 digest{};
};
Bytes encode(const OtherValue&);
OtherValue decode_other_value(std::span<const uint8_t>);

// A metadata fact's value: whether its meaning cannot be known, then its bytes.
struct MetadataValue {
  bool unknown = false;
  Bytes value;
};
Bytes encode_metadata_value(bool unknown, std::span<const uint8_t> value);
MetadataValue decode_metadata_value(std::span<const uint8_t>);

// be64: a key that sorts as the number does.
Bytes number_key(uint64_t);
Bytes pack_key(uint64_t sequence, uint64_t offset);
uint64_t number_of(std::span<const uint8_t>);

// One record out of a manifest.
struct Record {
  RecordType type = RecordType::item;
  // For header.
  Header header{};
  // For begin.
  Begin begin{};
  // For item.
  Bytes key{};
  Bytes value{};
  // For end: the items the section held. For trailer: the items and sections
  // the manifest held.
  uint64_t count = 0;
  uint64_t sections = 0;
};

Bytes encode_record(const Record&);

// Reads a manifest's records in order, every block's chain checked. Throws
// ManifestDamaged for anything that is not a whole manifest. With `lenient`
// (a partial, to resume) a block that does not hold simply ends the read.
class ManifestReader {
 public:
  explicit ManifestReader(const std::filesystem::path& path, bool lenient = false);
  ~ManifestReader();
  ManifestReader(const ManifestReader&) = delete;
  ManifestReader& operator=(const ManifestReader&) = delete;

  // The next record; nothing past the trailer, or in a lenient read at the
  // first block that does not hold.
  std::optional<Record> next();
  // Reads no record that does not start before `limit`; lenient reads only.
  void limit_to(uint64_t bytes) noexcept { _limit = bytes; }
  // Where the block last read ends, and the chain there.
  uint64_t block_end() const noexcept { return _block_end; }
  const dannan::identity::Hash256& chain() const noexcept { return _chain; }
  // Whether every record of the last block has been returned.
  bool at_block_end() const noexcept { return _at >= _payload.size(); }
  const std::filesystem::path& path() const noexcept { return _path; }

 private:
  bool next_block();

  std::filesystem::path _path;
  const bool _lenient;
  int _fd = -1;
  uint64_t _size = 0;
  uint64_t _offset = 0;
  uint64_t _block_end = 0;
  uint64_t _limit = UINT64_MAX;
  dannan::identity::Hash256 _chain{};
  Bytes _payload;
  size_t _at = 0;
  bool _ended = false;
};

// Reads the whole manifest; throws ManifestDamaged unless it is whole and its
// orders and counts hold. Returns its header.
Header verify_manifest(const std::filesystem::path& path);

}  // namespace dagda::migrate::manifest_format
