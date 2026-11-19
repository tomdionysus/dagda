//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>

#include "dagda/ledger/retention.h"
#include "dagda/migrate/manifest.h"
#include "dagda/migrate/manifest_format.h"
#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"

namespace dagda::migrate {
namespace {

using namespace manifest_format;
using dannan::identity::hex;

// Hex of an id or other binary key; a text key as it is.
std::string key_text(SectionKind kind, const Bytes& key) {
  switch (kind) {
    case SectionKind::loose: {
      // An id, then the path of a file not where its id says.
      std::string out = hex(std::span<const uint8_t>(key).first(std::min<size_t>(key.size(), 32)));
      if (key.size() > 32) out += " at " + std::string(key.begin() + 32, key.end());
      return out;
    }
    case SectionKind::pack_files:
      return "pack " + std::to_string(number_of(key));
    case SectionKind::pack_records:
      return "pack " + std::to_string(number_of(std::span<const uint8_t>(key).first(8))) + " offset " +
             std::to_string(number_of(std::span<const uint8_t>(key).subspan(8)));
    case SectionKind::claim_frames:
      return "frame " + std::to_string(number_of(key));
    case SectionKind::removed:
    case SectionKind::held:
    case SectionKind::claims:
    case SectionKind::counts:
    case SectionKind::sightings:
      return hex(key);
    default:
      return std::string(key.begin(), key.end());
  }
}

std::string layout_text(uint8_t layout) {
  switch (layout) {
    case record_v18:
      return "v18";
    case record_v19:
      return "v19";
    case record_empty:
      return "empty";
    case record_unreadable:
      return "unreadable";
    case record_unsealed:
      return "unsealed";
    default:
      return "layout " + std::to_string(layout);
  }
}

std::string dots(const std::map<ledger::NodeId, uint64_t>& side) {
  std::string out = "{";
  for (const auto& [origin, sequence] : side) {
    if (out.size() > 1) out += ", ";
    out += dannan::identity::to_string(origin) + ":" + std::to_string(sequence);
  }
  return out + "}";
}

std::string claim_text(const Bytes& value) {
  try {
    const auto state = ledger::retention_format::decode_state(value);
    return "adds " + dots(state.adds) + " removed " + dots(state.removed);
  } catch (const std::exception&) {
    return "undecodable " + hex(value);
  }
}

// What a claims journal frame says: its op, class, dot and how many ids.
std::string frame_text(const Bytes& value) {
  try {
    dannan::net::Reader reader(value);
    const auto op = reader.u8();
    const auto klass = reader.u8();
    std::string out = std::string(op == ledger::retention_format::op_add       ? "add"
                                  : op == ledger::retention_format::op_release ? "release"
                                                                               : "prune") +
                      " class " + std::to_string(klass);
    if (op == ledger::retention_format::op_add) {
      const auto origin = reader.fixed<16>();
      out += " dot " + hex(origin) + ":" + std::to_string(reader.u64());
    }
    return out + " (" + std::to_string(value.size()) + " bytes)";
  } catch (const std::exception&) {
    return "undecodable (" + std::to_string(value.size()) + " bytes)";
  }
}

// Reads a manifest's sections, one begin to its end at a time.
class Stream {
 public:
  explicit Stream(const std::filesystem::path& path) : _reader(path) {
    const auto first = _reader.next();
    if (!first || first->type != RecordType::header) throw ManifestDamaged("manifest " + path.string() + " has no header");
    _header = first->header;
    advance();
  }

  const Header& header() const noexcept { return _header; }
  bool in_section() const noexcept { return _begin.has_value(); }
  const Begin& begin() const { return *_begin; }
  // The current item; nothing at the section's end.
  const Record* item() const noexcept { return _item ? &*_item : nullptr; }
  void next_item() {
    const auto record = _reader.next();
    if (!record) throw ManifestDamaged("manifest " + _reader.path().string() + " ends inside a section");
    if (record->type == RecordType::item) {
      _item = *record;
      return;
    }
    if (record->type != RecordType::end) throw ManifestDamaged("manifest " + _reader.path().string() + " has a section with no end");
    _item.reset();
  }
  void next_section() { advance(); }

 private:
  void advance() {
    _item.reset();
    _begin.reset();
    const auto record = _reader.next();
    if (!record) throw ManifestDamaged("manifest " + _reader.path().string() + " ends with no trailer: it is truncated");
    if (record->type == RecordType::trailer) return;
    if (record->type != RecordType::begin) throw ManifestDamaged("manifest " + _reader.path().string() + " has an item outside a section");
    _begin = record->begin;
    next_item();
  }

  ManifestReader _reader;
  Header _header;
  std::optional<Begin> _begin;
  std::optional<Record> _item;
};

class Comparison {
 public:
  Comparison(const ComparisonOptions& options, ComparisonReport& report) : _options(options), _report(report) {}

  void run(const std::filesystem::path& before_path, const std::filesystem::path& after_path) {
    Stream before(before_path);
    Stream after(after_path);
    say("comparing " + before_path.string() + " (layout v" + std::to_string(before.header().layout) + ") with " + after_path.string() + " (layout v" +
        std::to_string(after.header().layout) + ")");
    check_headers(before.header(), after.header());
    _from = static_cast<uint8_t>(before.header().layout);
    _to = static_cast<uint8_t>(after.header().layout);
    while (before.in_section() || after.in_section()) {
      if (!after.in_section() || (before.in_section() && before.begin().order < after.begin().order)) {
        compare_section(&before, nullptr);
        before.next_section();
      } else if (!before.in_section() || after.begin().order < before.begin().order) {
        compare_section(nullptr, &after);
        after.next_section();
      } else {
        compare_section(&before, &after);
        before.next_section();
        after.next_section();
      }
    }
  }

 private:
  void say(const std::string& line) const {
    if (_options.say) _options.say(line);
  }

  void difference(ComparedCategory& category, const std::string& what) {
    ++category.differences;
    ++_report.difference_count;
    const auto line = category.name + ": " + what;
    if (_report.differences.size() < _options.keep) _report.differences.push_back(line);
    say("DIFFERENT " + line);
  }

  void unchecked(ComparedCategory& category, const std::string& what) {
    ++category.not_checked;
    ++_report.not_checked_count;
    const auto line = category.name + ": " + what;
    if (_report.not_checked.size() < _options.keep) _report.not_checked.push_back(line);
    say("NOT CHECKED " + line);
  }

  void check_headers(const Header& before, const Header& after) {
    ComparedCategory category;
    category.name = "identity";
    category.before = 1;
    category.after = 1;
    if (before.layout != _options.before_layout)
      difference(category,
                 "the first manifest is of layout v" + std::to_string(before.layout) + ", not v" + std::to_string(_options.before_layout) + " as it must be");
    if (after.layout != _options.after_layout)
      difference(category,
                 "the second manifest is of layout v" + std::to_string(after.layout) + ", not v" + std::to_string(_options.after_layout) + " as it must be");
    if (before.domain != after.domain) difference(category, "domain " + before.domain + " against " + after.domain);
    if (before.node != after.node) difference(category, "node " + before.node + " against " + after.node);
    if (before.key_check != after.key_check) difference(category, "taken under different cluster keys");
    if (before.stores != after.stores) {
      for (const auto& store : before.stores)
        if (std::find(after.stores.begin(), after.stores.end(), store) == after.stores.end())
          difference(category, "store " + store + " is in the first manifest alone");
      for (const auto& store : after.stores)
        if (std::find(before.stores.begin(), before.stores.end(), store) == before.stores.end())
          difference(category, "store " + store + " is in the second manifest alone");
    }
    finish(category);
  }

  void finish(ComparedCategory& category) {
    std::ostringstream line;
    line << category.name << ": sized " << category.before << " before, " << category.after << " after: ";
    switch (category.result()) {
      case CategoryResult::same:
        line << "SAME";
        break;
      case CategoryResult::zero:
        line << "ZERO";
        break;
      case CategoryResult::different:
        line << "DIFFERENT (" << category.differences << ")";
        break;
      case CategoryResult::not_checked:
        line << "NOT CHECKED" << (category.why.empty() ? "" : ": " + category.why);
        break;
    }
    say(line.str());
    _report.categories.push_back(std::move(category));
  }

  void compare_section(Stream* before, Stream* after) {
    const auto& begin = before ? before->begin() : after->begin();
    if (begin.kind == SectionKind::not_checked) {
      read_not_checked(before, after);
      return;
    }
    ComparedCategory category;
    category.name = begin.display;
    if (!before || !after) difference(category, std::string("the section is in the ") + (before ? "first" : "second") + " manifest alone");
    const std::string why = before && !before->begin().not_checked.empty() ? before->begin().not_checked
                            : after && !after->begin().not_checked.empty() ? after->begin().not_checked
                                                                           : std::string();
    if (!why.empty()) {
      category.why = why;
      ++category.not_checked;
      ++_report.not_checked_count;
      if (_report.not_checked.size() < _options.keep) _report.not_checked.push_back(category.name + ": " + why);
    }
    const auto kind = begin.kind;
    // Each branch tests the cursor it advances, so the compiler sees it too.
    for (;;) {
      const Record* a = before ? before->item() : nullptr;
      const Record* b = after ? after->item() : nullptr;
      if (before != nullptr && a != nullptr && (b == nullptr || a->key < b->key)) {
        ++category.before;
        lost(category, kind, *a);
        before->next_item();
      } else if (after != nullptr && b != nullptr && (a == nullptr || b->key < a->key)) {
        ++category.after;
        extra(category, kind, *b);
        after->next_item();
      } else if (before != nullptr && after != nullptr && a != nullptr && b != nullptr) {
        ++category.before;
        ++category.after;
        compare_item(category, kind, *a, *b);
        before->next_item();
        after->next_item();
      } else {
        break;
      }
    }
    finish(category);
  }

  void read_not_checked(Stream* before, Stream* after) {
    ComparedCategory category;
    category.name = "not checked";
    const auto drain = [&](Stream* stream, uint64_t& count, std::map<std::string, std::string>& into) {
      if (!stream) return;
      while (const auto* item = stream->item()) {
        ++count;
        into[std::string(item->key.begin(), item->key.end())] = std::string(item->value.begin(), item->value.end());
        stream->next_item();
      }
    };
    std::map<std::string, std::string> found;
    drain(before, category.before, found);
    drain(after, category.after, found);
    for (const auto& [name, why] : found) unchecked(category, name + ": " + why);
    finish(category);
  }

  void lost(ComparedCategory& category, SectionKind kind, const Record& item) {
    const auto name = key_text(kind, item.key);
    switch (kind) {
      case SectionKind::loose:
        difference(category, "object " + name + " is lost");
        break;
      case SectionKind::pack_records:
        difference(category, "record at " + name + " is lost (" + pack_record_text(decode_pack_value(item.value)) + ")");
        break;
      case SectionKind::removed:
        difference(category, "removed object " + name + " is resurrected: it is no longer removed");
        break;
      case SectionKind::held:
        difference(category, "object " + name + " is no longer held");
        break;
      case SectionKind::claims:
        difference(category, "claim on " + name + " is lost: " + claim_text(item.value));
        break;
      case SectionKind::claim_frames:
        difference(category, name + " is lost: " + frame_text(item.value));
        break;
      case SectionKind::counts:
        difference(category, "count of " + name + " is lost");
        break;
      case SectionKind::sightings:
        difference(category, "sighting of " + name + " is lost");
        break;
      case SectionKind::markers:
        difference(category, "marker " + name + " is lost");
        break;
      case SectionKind::bytes:
        difference(category, name + " is lost");
        break;
      case SectionKind::accounting:
        difference(category, "the accounting file is lost");
        break;
      case SectionKind::other:
        difference(category, "file " + name + " is lost");
        break;
      case SectionKind::pack_files:
        difference(category, name + " is lost");
        break;
      case SectionKind::metadata: {
        const auto value = decode_metadata_value(item.value).value;
        difference(category, name + " is lost (" + std::string(value.begin(), value.end()) + ")");
        break;
      }
      case SectionKind::not_checked:
        break;
    }
  }

  void extra(ComparedCategory& category, SectionKind kind, const Record& item) {
    const auto name = key_text(kind, item.key);
    switch (kind) {
      case SectionKind::loose:
        difference(category, "object " + name + " is new: it was not there before");
        break;
      case SectionKind::pack_records:
        difference(category, "record at " + name + " is new (" + pack_record_text(decode_pack_value(item.value)) + ")");
        break;
      case SectionKind::removed:
        difference(category, "object " + name + " is removed now and was not before");
        break;
      case SectionKind::claims:
        difference(category, "claim on " + name + " is new: " + claim_text(item.value));
        break;
      case SectionKind::claim_frames:
        difference(category, name + " is new: " + frame_text(item.value));
        break;
      default:
        difference(category, name + " is new: it was not there before");
        break;
    }
  }

  std::string pack_record_text(const PackValue& value) {
    if (value.span == span_tail) return "torn tail of " + std::to_string(value.length) + " bytes";
    if (value.span == span_damaged) return "damaged span of " + std::to_string(value.length) + " bytes";
    const char* type = value.header.type == store::local_format::pack_put      ? "put"
                       : value.header.type == store::local_format::pack_remove ? "tombstone"
                                                                               : "touch";
    return std::string(type) + " of " + store::to_string(value.header.id);
  }

  // A sealed record: it must have opened under the first layout and open under
  // the second, or the name says which did not.
  void compare_layouts(ComparedCategory& category, const std::string& name, uint8_t before, uint8_t after) {
    if (before == record_unreadable && after == record_unreadable) {
      unchecked(category, name + " opens on neither side: damaged before the migration, its plaintext cannot be checked");
      return;
    }
    if (before == record_unreadable) {
      difference(category, name + " did not open before and opens now");
      return;
    }
    if (after == record_unreadable) {
      difference(category, name + " no longer opens: its bytes have changed");
      return;
    }
    if (before != _from) difference(category, name + " was " + layout_text(before) + " in a manifest of layout v" + std::to_string(_from));
    if (after != _to) difference(category, name + " is " + layout_text(after) + " in a manifest of layout v" + std::to_string(_to) + ": not migrated");
  }

  void compare_item(ComparedCategory& category, SectionKind kind, const Record& a, const Record& b) {
    const auto name = key_text(kind, a.key);
    switch (kind) {
      case SectionKind::loose: {
        const auto before = decode_loose_value(a.value);
        const auto after = decode_loose_value(b.value);
        if (before.layout == record_empty || after.layout == record_empty) {
          if (before.layout != after.layout)
            difference(category, "object " + name + " is " + layout_text(before.layout) + " before and " + layout_text(after.layout) + " after");
        } else {
          compare_layouts(category, "object " + name, before.layout, after.layout);
        }
        if (before.plain_size != after.plain_size)
          difference(category,
                     "object " + name + " has size " + std::to_string(before.plain_size) + " before and " + std::to_string(after.plain_size) + " after");
        if (before.file_size != after.file_size)
          difference(category,
                     "object " + name + " file is " + std::to_string(before.file_size) + " bytes before and " + std::to_string(after.file_size) + " after");
        if (before.nonce != after.nonce) difference(category, "object " + name + " has another nonce");
        if (before.mtime_ns != after.mtime_ns)
          difference(category, "object " + name + " touched time moved from " + std::to_string(before.mtime_ns) + " to " + std::to_string(after.mtime_ns));
        if (_from == _to && before.tag != after.tag) difference(category, "object " + name + " has another tag under the same layout");
        break;
      }
      case SectionKind::pack_records: {
        const auto before = decode_pack_value(a.value);
        const auto after = decode_pack_value(b.value);
        const auto label = "record at " + name + " (" + pack_record_text(before) + ")";
        if (before.span != after.span) {
          difference(category, label + " is now " + pack_record_text(after));
          break;
        }
        if (before.span != span_record) {
          if (before.length != after.length || before.digest != after.digest)
            difference(category, label + " has changed");
          else if (before.span == span_damaged)
            unchecked(category, label + ": what it held cannot be known");
          break;
        }
        if (before.header.type != after.header.type || before.header.id != after.header.id || before.header.touched_ms != after.header.touched_ms ||
            before.header.plain_size != after.header.plain_size || before.header.payload_size != after.header.payload_size ||
            before.header.nonce != after.header.nonce) {
          difference(category, label + " is now " + pack_record_text(after) + " with other fields");
          break;
        }
        if (before.header.type == store::local_format::pack_put) {
          compare_layouts(category, label, before.layout, after.layout);
          if (_from == _to && before.header.tag != after.header.tag) difference(category, label + " has another tag under the same layout");
        } else if (before.header.tag != after.header.tag) {
          difference(category, label + " carries nothing sealed and has another tag");
        }
        break;
      }
      case SectionKind::pack_files: {
        // Its first eight bytes are the size; a pack's time means nothing.
        if (number_of(std::span<const uint8_t>(a.value).first(8)) != number_of(std::span<const uint8_t>(b.value).first(8)))
          difference(category, name + " is " + std::to_string(number_of(std::span<const uint8_t>(a.value).first(8))) + " bytes before and " +
                                   std::to_string(number_of(std::span<const uint8_t>(b.value).first(8))) + " after");
        break;
      }
      case SectionKind::removed:
        if (a.value != b.value) difference(category, "removed object " + name + " has another tombstone time");
        break;
      case SectionKind::accounting: {
        const auto before = decode_accounting_value(a.value);
        const auto after = decode_accounting_value(b.value);
        if (before.used != after.used)
          difference(category, "used bytes " + std::to_string(before.used) + " before and " + std::to_string(after.used) + " after");
        if (before.sequence != after.sequence || before.operation != after.operation || before.digest != after.digest)
          difference(category, "the accounting file has changed");
        if (!before.decoded && !after.decoded) unchecked(category, "the accounting file decodes on neither side");
        break;
      }
      case SectionKind::markers: {
        const auto before = decode_marker_value(a.value);
        const auto after = decode_marker_value(b.value);
        if (before.layout != _from) difference(category, "marker " + name + " says \"" + before.text + "\", not layout v" + std::to_string(_from));
        if (after.layout != _to) difference(category, "marker " + name + " says \"" + after.text + "\", not layout v" + std::to_string(_to));
        if (before.node != after.node || before.token != after.token)
          difference(category, "marker " + name + " names node " + before.node + " token " + before.token + " before and node " + after.node + " token " +
                                   after.token + " after");
        break;
      }
      case SectionKind::held:
        if (a.value != b.value) difference(category, "object " + name + " held value " + hex(a.value) + " before and " + hex(b.value) + " after");
        break;
      case SectionKind::claims:
        if (a.value != b.value) difference(category, "claim on " + name + " changed from " + claim_text(a.value) + " to " + claim_text(b.value));
        break;
      case SectionKind::claim_frames:
        if (a.value != b.value) difference(category, name + " changed from " + frame_text(a.value) + " to " + frame_text(b.value));
        break;
      case SectionKind::counts:
        if (a.value != b.value)
          difference(category, "count of " + name + " changed from " + std::to_string(number_of(a.value)) + " to " + std::to_string(number_of(b.value)));
        break;
      case SectionKind::sightings:
        if (a.value != b.value)
          difference(category, "sighting of " + name + " changed from " + std::to_string(number_of(a.value)) + " to " + std::to_string(number_of(b.value)));
        break;
      case SectionKind::other: {
        const auto before = decode_other_value(a.value);
        const auto after = decode_other_value(b.value);
        if (before.size != after.size || before.digest != after.digest) difference(category, "file " + name + " has changed");
        break;
      }
      case SectionKind::bytes:
        if (a.value != b.value) difference(category, name + " has changed");
        break;
      case SectionKind::metadata: {
        // A fact's value is text; a reader's that is not is shown as it is.
        const auto before = decode_metadata_value(a.value);
        const auto after = decode_metadata_value(b.value);
        const auto text = [](const Bytes& value) { return std::string(value.begin(), value.end()); };
        if (before.value != after.value)
          difference(category, name + " has changed from \"" + text(before.value) + "\" to \"" + text(after.value) + "\"");
        else if (before.unknown != after.unknown)
          difference(category, name + " is " + (after.unknown ? "unknown" : "known") + " now and was not before");
        else if (before.unknown)
          unchecked(category, name + ": " + text(before.value) + ": what it held cannot be known");
        break;
      }
      case SectionKind::not_checked:
        break;
    }
  }

  const ComparisonOptions& _options;
  ComparisonReport& _report;
  uint8_t _from = 0;
  uint8_t _to = 0;
};

}  // namespace

CategoryResult ComparedCategory::result() const noexcept {
  if (not_checked) return CategoryResult::not_checked;
  if (differences) return CategoryResult::different;
  if (!before && !after) return CategoryResult::zero;
  return CategoryResult::same;
}

bool ComparisonReport::names(std::string_view text) const {
  return std::any_of(differences.begin(), differences.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
}

const ComparedCategory* ComparisonReport::category(std::string_view name) const {
  for (const auto& category : categories)
    if (category.name == name) return &category;
  return nullptr;
}

std::string ComparisonReport::summary() const {
  std::ostringstream out;
  for (const auto& category : categories) {
    out << category.name << ": sized " << category.before << " before, " << category.after << " after: ";
    switch (category.result()) {
      case CategoryResult::same:
        out << "SAME";
        break;
      case CategoryResult::zero:
        out << "ZERO";
        break;
      case CategoryResult::different:
        out << "DIFFERENT (" << category.differences << ")";
        break;
      case CategoryResult::not_checked:
        out << "NOT CHECKED" << (category.why.empty() ? "" : ": " + category.why);
        break;
    }
    out << "\n";
  }
  for (const auto& line : differences) out << "DIFFERENT " << line << "\n";
  for (const auto& line : not_checked) out << "NOT CHECKED " << line << "\n";
  if (passed()) {
    out << "RESULT: PASS\n";
  } else {
    out << "RESULT: FAIL: " << difference_count << " differences, " << not_checked_count << " not checked\n";
  }
  return out.str();
}

ComparisonReport compare_manifests(const std::filesystem::path& before, const std::filesystem::path& after, const ComparisonOptions& options) {
  // Whole, both of them, before a line is said: a truncated manifest is a
  // failure and never a shorter comparison.
  (void)verify_manifest(before);
  (void)verify_manifest(after);
  ComparisonReport report;
  Comparison comparison(options, report);
  comparison.run(before, after);
  return report;
}

}  // namespace dagda::migrate
