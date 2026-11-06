//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// The neutral owner of a tree (the plan's section 2.0, Q1): a record with a kind,
// a few numbers and references, nothing any real owner holds. Its forms are its
// own, so nothing here can pass by matching another owner's bytes.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dagda/heads/tree.h"
#include "dannan/identity/crypto.h"

namespace dagda::test {

using heads::Bytes;
using heads::ObjectId;

// A reference: where in the item it stands, whether it is only a gap, and what it
// names.
struct NeutralRef {
  uint64_t start = 0;
  uint32_t span = 0;
  bool gap = false;
  ObjectId id{};
  bool operator==(const NeutralRef&) const = default;
};
inline constexpr size_t neutral_ref_bytes = 8 + 4 + 1 + 32;

struct NeutralItem {
  // 1 or 2; anything else on a leaf is refused.
  uint8_t kind = 1;
  uint32_t tag = 0;
  int64_t stamp = 0;
  uint64_t weight = 0;
  std::vector<NeutralRef> refs;
  bool operator==(const NeutralItem&) const = default;
};

class NeutralValues final : public heads::TreeValues<NeutralItem> {
 public:
  void encode(dannan::net::Writer& out, const NeutralItem& item, heads::TreeChildWriter& children) const override {
    out.u8(item.kind);
    out.u32(item.tag);
    out.i64(item.stamp);
    out.u64(item.weight);
    if (item.refs.empty()) {
      out.u8(form_none);
      return;
    }
    if (item.refs.size() <= children.inline_max()) {
      out.u8(form_inline);
      out.u32(static_cast<uint32_t>(item.refs.size()));
      out.raw(flat(item.refs));
      return;
    }
    out.u8(form_child);
    out.u64(item.refs.size());
    out.fixed(children.build(flat(item.refs)).bytes);
  }

  NeutralItem decode(dannan::net::Reader& in, const heads::TreeChildReader& children, bool with_children) const override {
    NeutralItem item;
    item.kind = in.u8();
    if (item.kind < 1 || item.kind > 2) throw heads::DecodeError("bad neutral kind");
    item.tag = in.u32();
    item.stamp = in.i64();
    item.weight = in.u64();
    switch (in.u8()) {
      case form_none:
        break;
      case form_inline: {
        const auto count = in.u32();
        for (uint32_t i = 0; i < count; ++i) item.refs.push_back(ref_of(in));
        break;
      }
      case form_child: {
        (void)in.u64();
        const ObjectId root{in.fixed<32>()};
        if (with_children) {
          Bytes records;
          children.read(root, records);
          dannan::net::Reader reader(records);
          while (reader.remaining()) item.refs.push_back(ref_of(reader));
        }
        break;
      }
      default:
        throw heads::DecodeError("bad neutral form");
    }
    return item;
  }

  heads::TreeReferences references(const NeutralItem& item, heads::TreeChildWriter& children) const override {
    heads::TreeReferences out;
    out.items = item.refs.size();
    for (const auto& ref : item.refs)
      if (!ref.gap) out.data.push_back(ref.id);
    if (item.refs.size() > children.inline_max()) (void)children.build(flat(item.refs));
    return out;
  }

  void skip(dannan::net::Reader& in) const override { (void)shape(in); }

  heads::TreeRecordShape shape(dannan::net::Reader& in) const override {
    heads::TreeRecordShape out;
    (void)in.view(1 + 4 + 8 + 8);
    switch (in.u8()) {
      case form_none:
        break;
      case form_inline: {
        out.items = in.u32();
        for (uint64_t i = 0; i < out.items; ++i) {
          const auto ref = ref_of(in);
          if (!ref.gap) out.data.push_back(ref.id);
        }
        break;
      }
      case form_child:
        out.items = in.u64();
        out.child_root = ObjectId{in.fixed<32>()};
        break;
      default:
        throw heads::DecodeError("bad neutral form");
    }
    return out;
  }

  std::span<const uint8_t> child_boundary(std::span<const uint8_t> record) const override { return record.subspan(8 + 4 + 1, 32); }

  std::optional<ObjectId> child_data(std::span<const uint8_t> record) const override {
    dannan::net::Reader reader(record);
    const auto ref = ref_of(reader);
    if (ref.gap) return std::nullopt;
    return ref.id;
  }

 private:
  static constexpr uint8_t form_none = 0;
  static constexpr uint8_t form_inline = 1;
  static constexpr uint8_t form_child = 2;

  static Bytes flat(const std::vector<NeutralRef>& refs) {
    dannan::net::Writer writer;
    for (const auto& ref : refs) {
      writer.u64(ref.start);
      writer.u32(ref.span);
      writer.u8(ref.gap ? 1 : 0);
      writer.fixed(ref.id.bytes);
    }
    return writer.take();
  }
  static NeutralRef ref_of(dannan::net::Reader& in) {
    NeutralRef ref;
    ref.start = in.u64();
    ref.span = in.u32();
    ref.gap = in.u8() != 0;
    ref.id.bytes = in.fixed<32>();
    return ref;
  }
};

inline heads::TreeForm neutral_form(heads::TreeLimits limits = {}) {
  heads::TreeForm form;
  form.leaf_magic = {'T', 'Q', 'L', '1'};
  form.branch_magic = {'T', 'Q', 'B', '1'};
  form.child_leaf_magic = {'T', 'Q', 'X', '1'};
  form.child_branch_magic = {'T', 'Q', 'Y', '1'};
  form.entry_domain = "dagda/test/tree/entry/v1";
  form.branch_domain = "dagda/test/tree/branch/v1";
  form.child_domain = "dagda/test/tree/child/v1";
  form.child_branch_domain = "dagda/test/tree/child-branch/v1";
  form.child_record_bytes = neutral_ref_bytes;
  form.limits = limits;
  return form;
}

using NeutralTree = heads::Tree<NeutralItem>;
using NeutralEntries = NeutralTree::Entries;

inline const NeutralValues& neutral_values() {
  static const NeutralValues values;
  return values;
}

inline NeutralTree neutral_tree(heads::TreeLimits limits = {}) { return NeutralTree(neutral_values(), neutral_form(limits)); }

inline ObjectId fake_object(uint64_t seed) {
  ObjectId id{};
  for (auto& byte : id.bytes) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    byte = static_cast<uint8_t>(seed >> 33);
  }
  return id;
}

inline NeutralItem make_item(uint64_t seed, size_t refs) {
  NeutralItem item;
  item.kind = 2;
  item.tag = 0644;
  item.weight = static_cast<uint64_t>(refs) * 4096;
  item.stamp = static_cast<int64_t>(seed) * 1000;
  item.refs.reserve(refs);
  for (size_t i = 0; i < refs; ++i) {
    NeutralRef ref;
    ref.start = static_cast<uint64_t>(i) * 4096;
    ref.span = 4096;
    ref.id = fake_object(seed * 1000 + i);
    item.refs.push_back(ref);
  }
  return item;
}

inline NeutralItem make_group(uint64_t seed) {
  NeutralItem item;
  item.kind = 1;
  item.tag = 0755;
  item.stamp = static_cast<int64_t>(seed);
  return item;
}

// A deep, unbalanced shape: nested groups, a few very long items and a long tail
// of short ones, keys sharing prefixes as real ones do.
inline NeutralEntries shaped_entries(size_t groups, size_t items_per_group) {
  NeutralEntries entries;
  entries["a"] = make_group(0);
  entries["a/all"] = make_group(1);
  for (size_t g = 0; g < groups; ++g) {
    const auto group = "a/all/group " + std::to_string(g);
    entries[group] = make_group(100 + g);
    for (size_t i = 0; i < items_per_group; ++i) {
      const auto part = group + "/part " + std::to_string(i % 4 + 1);
      entries[part] = make_group(200 + g * 10 + i);
      // Every sixteenth item has enough references to force a child sequence.
      const size_t refs = (g * items_per_group + i) % 16 == 0 ? 600 : 3;
      entries[part + "/item " + std::to_string(i)] = make_item(1000 + g * 100 + i, refs);
    }
  }
  return entries;
}

}  // namespace dagda::test
