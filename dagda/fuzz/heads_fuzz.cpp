//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The heads' bytes: the messages peers exchange (heads/messages.h), the records,
// certificates and history entries they carry (heads/record.h), a space's sealed files
// and frames (heads/sealed_file.h), retirement batches, and the register map's heads,
// deltas and tree nodes, which arrive from peers as control objects. Each may throw
// only net::DecodeError, but for a sealed file, which also says std::runtime_error.

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/fuzz/samples.h"
#include "dagda/heads/messages.h"
#include "dagda/heads/record.h"
#include "dagda/heads/register_map.h"
#include "dagda/heads/retirement.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/heads/tree.h"
#include "dannan/fuzz/fuzz.h"

namespace dagda::fuzz {
namespace {

using dannan::fuzz::Registration;
using dannan::net::Bytes;
using dannan::net::DecodeError;

constexpr heads::Magic retirement_magic{'D', 'G', 'F', 'Z', 'T', 'O', 'M', 'B'};
constexpr heads::Magic file_magic{'D', 'G', 'F', 'Z', 'F', 'I', 'L', 'E'};
// The register map's forms (register_map.cpp), written here by hand: its codec keeps
// them to itself.
constexpr heads::Magic register_head_magic{'D', 'G', 'R', 'G', 'H', 'E', 'D', '1'};
constexpr heads::Magic register_delta_magic{'D', 'G', 'R', 'G', 'D', 'L', 'T', '1'};
constexpr size_t register_entries = 160;

Bytes sample_aad() {
  const std::string_view text = "dagda/fuzz/aad";
  return Bytes(text.begin(), text.end());
}

heads::HeadClock sample_clock() { return {{sample_node(1), 3}, {sample_node(2), 4}}; }

// ---- The register map's fixture ----------------------------------------------

std::string register_key(size_t index) {
  const auto digits = std::to_string(index);
  return "k" + std::string(3 - std::min<size_t>(digits.size(), 3), '0') + digits;
}

// Values of every shape a leaf holds: plain, tombstoned, expiring, refs inline and refs
// past child_inline_max in a child sequence.
heads::Register sample_register(size_t index) {
  heads::Register value;
  value.value = Bytes{static_cast<uint8_t>(index & 0xff), 0x42};
  value.stamp = index + 1;
  value.node = sample_node(1);
  value.written_ns = 1'000'000;
  if (index % 7 == 0) value.refs = {sample_id(1), sample_id(2)};
  if (index % 50 == 0)
    for (uint8_t ref = 0; ref < 10; ++ref) value.refs.push_back(sample_id(static_cast<uint8_t>(0x80 + ref)));
  if (index % 11 == 0) value.tombstone = true;
  if (index % 13 == 0) value.expires_ns = 2'000'000;
  return value;
}

Bytes register_head(const std::optional<store::ObjectId>& root, const heads::HeadClock& clock, const std::vector<heads::Hash256>& parents) {
  dannan::net::Writer out;
  out.raw(register_head_magic);
  out.u32(1);
  heads::encode_clock(out, clock);
  heads::encode_merge_parents(out, parents);
  out.u8(root ? 1 : 0);
  if (root) out.fixed(root->bytes);
  out.u8(static_cast<uint8_t>(heads::StampKind::lamport));
  out.u8(static_cast<uint8_t>(heads::TombstonePolicy::Kind::window));
  out.u64(static_cast<uint64_t>(heads::register_tombstone_window.count()));
  out.u8(0);
  out.u64(register_entries);
  return out.take();
}

void write_register(dannan::net::Writer& out, const heads::Register& value) {
  uint8_t flags = value.tombstone ? uint8_t{1} : uint8_t{0};
  if (value.expires_ns) flags = static_cast<uint8_t>(flags | 2);
  out.u8(flags);
  out.u64(value.stamp);
  out.fixed(value.node.bytes);
  out.i64(value.written_ns);
  if (value.expires_ns) out.i64(*value.expires_ns);
  out.bytes(value.value);
  out.u32(static_cast<uint32_t>(value.refs.size()));
  for (const auto& ref : value.refs) out.fixed(ref.bytes);
}

Bytes register_delta(const heads::HeadClock& clock, const std::optional<std::vector<heads::Hash256>>& parents,
                     const std::map<std::string, heads::Register>& upsert, const std::vector<std::string>& erase) {
  dannan::net::Writer out;
  out.raw(register_delta_magic);
  heads::encode_clock(out, clock);
  out.u8(parents ? 1 : 0);
  if (parents) heads::encode_merge_parents(out, *parents);
  out.u64(register_entries + 1);
  out.u32(static_cast<uint32_t>(upsert.size()));
  for (const auto& [key, value] : upsert) {
    out.string(key);
    write_register(out, value);
  }
  out.u32(static_cast<uint32_t>(erase.size()));
  for (const auto& key : erase) out.string(key);
  return out.take();
}

// A map with default options, a tree of every register shape deep enough for a branch,
// and a head naming it: what a delta applies to and what a fetched node is read against.
struct RegisterFixture {
  heads::RegisterMap map{heads::RegisterOptions{}};
  heads::MemoryTreeNodeStore nodes;
  store::ObjectId root{};
  heads::State parent;

  RegisterFixture() {
    heads::Tree<heads::Register>::Entries entries;
    for (size_t i = 0; i < register_entries; ++i) entries.emplace(register_key(i), sample_register(i));
    root = map.tree().build(entries, nodes);
    parent = map.codec()->decode_full(register_head(root, sample_clock(), {}));
  }
};

const RegisterFixture& register_fixture() {
  static const RegisterFixture fixture;
  return fixture;
}

// The fixture's nodes under whatever a run writes, so no run changes the next one's.
class OverlayStore final : public heads::TreeNodeStore {
 public:
  explicit OverlayStore(const heads::MemoryTreeNodeStore& base) : _base(base) {}
  store::ObjectId put(std::span<const uint8_t> node) override { return _added.put(node); }
  std::optional<Bytes> get(const store::ObjectId& id) const override {
    if (auto found = _added.get(id)) return found;
    return _base.get(id);
  }

 private:
  const heads::MemoryTreeNodeStore& _base;
  heads::MemoryTreeNodeStore _added;
};

// ---- The messages -------------------------------------------------------------

const Registration hash(
    "heads_hash",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::message::decode_hash(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{heads::message::encode_hash(sample_id(1))}; });

const Registration notice(
    "heads_notice",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::message::decode_notice(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{heads::message::encode_notice(42)}; });

const Registration identity_reply(
    "heads_identity",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::message::decode_identity(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{heads::message::encode_identity({5, sample_id(2)})}; });

// A request or reply with a space id after its form, and a request with no form.
const Registration addressed(
    "heads_addressed",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::message::addressed(data, [](std::span<const uint8_t> payload) { return heads::message::decode_identity(payload); });
      } catch (const DecodeError&) {
      }
      try {
        (void)heads::message::space_of_empty(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      const auto form = heads::message::encode_identity({5, sample_id(2)});
      return std::vector<Bytes>{form, heads::message::with_space(form, sample_id(9)), Bytes(32, uint8_t{7}), Bytes{}};
    });

// ---- The records --------------------------------------------------------------

const Registration record(
    "heads_record",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::decode_record(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      const Bytes founding{1, 2, 3};
      const auto first = heads::genesis_record(founding);
      heads::Record second;
      second.generation = 2;
      second.previous = first.hash;
      second.payload = Bytes{4, 5};
      second.hash = heads::record_hash(second.generation, second.previous, second.payload.span());
      return std::vector<Bytes>{heads::encode_record(first), heads::encode_record(second)};
    });

heads::Acceptance sample_acceptance(uint8_t hash_fill, uint32_t required) {
  heads::Acceptance out;
  out.generation = 3;
  out.hash = sample_id(hash_fill);
  out.required = required;
  for (uint32_t i = 0; i < required; ++i) out.replicas.push_back(sample_node(static_cast<uint8_t>(i + 1)));
  return out;
}

const Registration acceptance(
    "heads_acceptance",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::decode_acceptance(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{heads::encode_acceptance(sample_acceptance(4, 2)), heads::encode_acceptance(sample_acceptance(5, 0))}; });

const Registration acceptance_set(
    "heads_acceptance_set",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::decode_acceptance_set(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{heads::encode_acceptance_set({sample_acceptance(4, 2), sample_acceptance(5, 1)})}; });

const Registration history_entry(
    "heads_history_entry",
    [](std::span<const uint8_t> data) {
      try {
        const auto entry = heads::decode_history_entry(data);
        (void)heads::materialization_dependencies(entry);
      } catch (const DecodeError&) {
      }
    },
    [] {
      heads::HistoryEntry full;
      full.generation = 1;
      full.hash = sample_id(9);
      full.payload = Bytes{1, 2};
      heads::HistoryEntry delta;
      delta.generation = 3;
      delta.previous = sample_id(8);
      delta.hash = sample_id(10);
      delta.previous_known = true;
      delta.merge_parents = {sample_id(11)};
      delta.body = heads::HistoryEntry::Body::delta;
      delta.payload = Bytes{3};
      return std::vector<Bytes>{heads::encode_history_entry(full), heads::encode_history_entry(delta)};
    });

// The sections every owner's record frames alike: a clock, then merge parents.
const Registration clock(
    "heads_clock",
    [](std::span<const uint8_t> data) {
      try {
        dannan::net::Reader reader(data);
        (void)heads::decode_clock(reader);
        (void)heads::decode_merge_parents(reader);
        reader.finish();
      } catch (const DecodeError&) {
      }
    },
    [] {
      dannan::net::Writer writer;
      heads::encode_clock(writer, sample_clock());
      heads::encode_merge_parents(writer, {sample_id(1), sample_id(2)});
      return std::vector<Bytes>{writer.take()};
    });

const Registration retirement_batch(
    "heads_retirement_batch",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::decode_retirement_batch(retirement_magic, data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{heads::encode_retirement_batch(retirement_magic, 1000, {sample_id(1), sample_id(2), sample_id(3)})}; });

// ---- A space's files ----------------------------------------------------------

const Registration sealed_file(
    "heads_sealed_file",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::open_file(sample_key(), file_magic, sample_aad(), data);
      } catch (const std::runtime_error&) {
      }
    },
    [] {
      const Bytes plain{1, 2, 3, 4};
      return std::vector<Bytes>{heads::seal_file(sample_key(), file_magic, sample_aad(), plain)};
    });

// A journal or history frame's envelope, the bytes after its length: parsed, and
// opened, which says nothing rather than throw.
const Registration frame_envelope(
    "heads_frame_envelope",
    [](std::span<const uint8_t> data) {
      try {
        (void)heads::parse_frame_envelope(data);
      } catch (const DecodeError&) {
      }
      (void)heads::open_frame(sample_key(), sample_aad(), data);
    },
    [] {
      const Bytes plain{1, 2, 3, 4};
      const auto frame = heads::seal_frame(sample_key(), sample_aad(), plain);
      return std::vector<Bytes>{Bytes(frame.begin() + 4, frame.end())};
    });

// ---- The register map ---------------------------------------------------------

const Registration register_head_form(
    "heads_register_head",
    [](std::span<const uint8_t> data) {
      const auto& codec = *register_fixture().map.codec();
      try {
        const auto state = codec.decode_full(data);
        (void)codec.encode_full(state);
        (void)codec.head_fields(state);
        (void)codec.resident_bytes(state);
      } catch (const DecodeError&) {
      }
    },
    [] {
      const auto& fixture = register_fixture();
      return std::vector<Bytes>{fixture.map.codec()->genesis(), register_head(fixture.root, sample_clock(), {}),
                                register_head(std::nullopt, {}, {sample_id(3)})};
    });

// A delta checked, applied to the fixture's head over its nodes, and applied with no
// node store, which a delta naming keys is refused.
const Registration register_delta_form(
    "heads_register_delta",
    [](std::span<const uint8_t> data) {
      const auto& fixture = register_fixture();
      const auto& codec = *fixture.map.codec();
      try {
        codec.check_delta(data);
      } catch (const DecodeError&) {
      }
      try {
        (void)codec.encode_after_delta(data, fixture.parent);
      } catch (const DecodeError&) {
      }
      OverlayStore nodes(fixture.nodes);
      try {
        const auto next = codec.apply_delta(fixture.parent, data, &nodes);
        (void)codec.encode_full(next);
        (void)codec.head_fields(next);
      } catch (const DecodeError&) {
      }
      try {
        (void)codec.apply_delta(fixture.parent, data, nullptr);
      } catch (const DecodeError&) {
      }
    },
    [] {
      heads::HeadClock moved = sample_clock();
      moved[sample_node(3)] = 1;
      std::map<std::string, heads::Register> upsert;
      upsert.emplace(register_key(100), sample_register(101));
      upsert.emplace("zz", sample_register(0));
      return std::vector<Bytes>{register_delta(moved, std::nullopt, {}, {}), register_delta(moved, std::vector<heads::Hash256>{sample_id(4)}, {}, {}),
                                register_delta(moved, std::nullopt, upsert, {register_key(7), register_key(8)})};
    });

// A tree node as a peer serves it: its children read from its bytes alone, then the
// tree under it walked, sized and collected over the fixture's nodes.
const Registration tree_node(
    "heads_tree_node",
    [](std::span<const uint8_t> data) {
      const auto& fixture = register_fixture();
      const auto& tree = fixture.map.tree();
      try {
        (void)tree.node_children(data);
      } catch (const DecodeError&) {
      }
      OverlayStore nodes(fixture.nodes);
      const auto root = nodes.put(data);
      try {
        (void)tree.stats(root, nodes);
      } catch (const DecodeError&) {
      }
      try {
        tree.for_each_with_prefix(root, nodes, "", [](const std::string&, const heads::Register&) {});
      } catch (const DecodeError&) {
      }
      try {
        std::vector<store::ObjectId> held;
        tree.collect_nodes(root, nodes, held);
      } catch (const DecodeError&) {
      }
    },
    [] {
      const auto& fixture = register_fixture();
      std::vector<Bytes> out;
      for (const auto& id : fixture.nodes.written())
        if (auto node = fixture.nodes.get(id)) out.push_back(std::move(*node));
      return out;
    });

}  // namespace
}  // namespace dagda::fuzz
