//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/register_map.h"

#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <tuple>
#include <utility>

namespace dagda::heads {

using dannan::net::Reader;
using dannan::net::Writer;
using dannan::sched::Stop;

namespace {

constexpr Magic head_magic{'D', 'G', 'R', 'G', 'H', 'E', 'D', '1'};
constexpr Magic delta_magic{'D', 'G', 'R', 'G', 'D', 'L', 'T', '1'};
constexpr Magic genesis_payload{'D', 'G', 'R', 'G', 'G', 'E', 'N', '1'};
constexpr Magic register_seed_magic{'D', 'G', 'R', 'G', 'S', 'E', 'E', 'D'};

constexpr uint8_t flag_tombstone = 1;
constexpr uint8_t flag_expiry = 2;
constexpr uint8_t refs_none = 0;
constexpr uint8_t refs_inline = 1;
constexpr uint8_t refs_child = 2;
constexpr size_t node_bytes = std::tuple_size_v<decltype(NodeId::bytes)>;

const ReplicaMagics& register_magics() {
  static const ReplicaMagics magics{{'D', 'G', 'R', 'G', 'C', 'H', 'K', '1'}, {'D', 'G', 'R', 'G', 'J', 'N', 'L', '1'},
                                    {'D', 'G', 'R', 'G', 'H', 'S', 'T', '1'}, {'D', 'G', 'R', 'G', 'A', 'C', 'C', '1'},
                                    {'D', 'G', 'R', 'G', 'A', 'U', 'T', '1'}, {'D', 'G', 'R', 'G', 'S', 'A', 'S', '1'}};
  return magics;
}

TreeForm register_form() {
  TreeForm form;
  form.leaf_magic = {'R', 'G', 'L', '1'};
  form.branch_magic = {'R', 'G', 'B', '1'};
  form.child_leaf_magic = {'R', 'G', 'X', '1'};
  form.child_branch_magic = {'R', 'G', 'Y', '1'};
  form.entry_domain = "dagda/register/entry/v1";
  form.branch_domain = "dagda/register/branch/v1";
  form.child_domain = "dagda/register/child/v1";
  form.child_branch_domain = "dagda/register/child-branch/v1";
  form.child_record_bytes = sizeof(ObjectId::bytes);
  return form;
}

// The fields every form of a register shares, before its refs.
void write_fields(Writer& out, const Register& value) {
  uint8_t flags = 0;
  if (value.tombstone) flags = flag_tombstone;
  if (value.expires_ns) flags = static_cast<uint8_t>(flags | flag_expiry);
  out.u8(flags);
  out.u64(value.stamp);
  out.fixed(value.node.bytes);
  out.i64(value.written_ns);
  if (value.expires_ns) out.i64(*value.expires_ns);
  out.bytes(value.value);
}

void read_fields(Reader& in, Register& value) {
  const auto flags = in.u8();
  if (flags & ~(flag_tombstone | flag_expiry)) throw DecodeError("bad register flags");
  value.tombstone = (flags & flag_tombstone) != 0;
  value.stamp = in.u64();
  value.node.bytes = in.fixed<node_bytes>();
  value.written_ns = in.i64();
  if (flags & flag_expiry) value.expires_ns = in.i64();
  value.value = in.bytes(RegisterMap::value_max_bytes);
}

Bytes flat_refs(const std::vector<ObjectId>& refs) {
  Writer out;
  for (const auto& ref : refs) out.fixed(ref.bytes);
  return out.take();
}

// A register whole, refs inline: what a delta carries and what an equal order is settled by.
void write_flat(Writer& out, const Register& value) {
  write_fields(out, value);
  out.u32(static_cast<uint32_t>(value.refs.size()));
  out.raw(flat_refs(value.refs));
}

Register read_flat(Reader& in) {
  Register value;
  read_fields(in, value);
  const auto count = in.u32();
  if (count > RegisterMap::refs_max) throw DecodeError("register names too many objects");
  value.refs.reserve(count);
  for (uint32_t i = 0; i < count; ++i) value.refs.push_back(ObjectId{in.fixed<32>()});
  return value;
}

Bytes flat_bytes(const Register& value) {
  Writer out;
  write_flat(out, value);
  return out.take();
}

void write_form(Writer& out, const RegisterForm& form) {
  out.u8(static_cast<uint8_t>(form.stamp));
  out.u8(static_cast<uint8_t>(form.tombstones.kind));
  out.u64(static_cast<uint64_t>(form.tombstones.window.count()));
  out.u8(form.tombstone_wins ? 1 : 0);
}

RegisterForm read_form(Reader& in) {
  RegisterForm form;
  const auto stamp = in.u8();
  if (stamp != static_cast<uint8_t>(StampKind::lamport) && stamp != static_cast<uint8_t>(StampKind::hlc)) throw DecodeError("bad register stamp kind");
  form.stamp = static_cast<StampKind>(stamp);
  const auto kind = in.u8();
  if (kind != static_cast<uint8_t>(TombstonePolicy::Kind::window) && kind != static_cast<uint8_t>(TombstonePolicy::Kind::never))
    throw DecodeError("bad register tombstone policy");
  form.tombstones.kind = static_cast<TombstonePolicy::Kind>(kind);
  form.tombstones.window = std::chrono::milliseconds(static_cast<int64_t>(in.u64()));
  const auto wins = in.u8();
  if (wins > 1) throw DecodeError("bad register tombstone rule");
  form.tombstone_wins = wins != 0;
  return form;
}

std::string form_text(const RegisterForm& form) {
  std::string out = form.stamp == StampKind::lamport ? "stamp=lamport" : "stamp=hlc";
  out += form.tombstones.kind == TombstonePolicy::Kind::never ? " tombstones=never"
                                                              : " tombstones=window:" + std::to_string(form.tombstones.window.count()) + "ms";
  out += form.tombstone_wins ? " tombstone_wins=true" : " tombstone_wins=false";
  return out;
}

std::string form_refusal(const RegisterForm& recorded, const RegisterForm& opened) {
  return "register map head records " + form_text(recorded) + " and this node opened it with " + form_text(opened);
}

const RegisterHead& head_cast(const State& state) { return *std::static_pointer_cast<const RegisterHead>(state); }

Bytes encode_head(const RegisterHead& head) {
  if (head.floor == 0) return Bytes(genesis_payload.begin(), genesis_payload.end());
  Writer out;
  out.raw(head_magic);
  out.u32(head.floor);
  encode_clock(out, head.clock);
  encode_merge_parents(out, head.merge_parents);
  out.u8(head.root ? 1 : 0);
  if (head.root) out.fixed(head.root->bytes);
  write_form(out, head.form);
  out.u64(head.lamport);
  return out.take();
}

RegisterHead decode_head(std::span<const uint8_t> payload) {
  RegisterHead head;
  if (std::equal(payload.begin(), payload.end(), genesis_payload.begin(), genesis_payload.end())) {
    head.floor = 0;
    return head;
  }
  Reader in(payload);
  const auto magic = in.view(head_magic.size());
  if (!std::equal(magic.begin(), magic.end(), head_magic.begin())) throw DecodeError("bad register map head");
  head.floor = in.u32();
  if (head.floor == 0) throw DecodeError("bad register map floor");
  head.clock = decode_clock(in);
  head.merge_parents = decode_merge_parents(in);
  if (in.u8() != 0) head.root = ObjectId{in.fixed<32>()};
  head.form = read_form(in);
  head.lamport = in.u64();
  in.finish();
  return head;
}

// What a delta says: the clock's entries that moved, the parents replaced, the stamp's
// high water and the tree's keys. A floor or a form never changes in one.
struct RegisterDelta {
  HeadClock clock;
  std::optional<std::vector<Hash256>> merge_parents;
  uint64_t lamport = 0;
  std::map<std::string, Register> upsert;
  std::vector<std::string> erase;
};

Bytes encode_delta_bytes(const RegisterDelta& delta) {
  Writer out;
  out.raw(delta_magic);
  encode_clock(out, delta.clock);
  out.u8(delta.merge_parents ? 1 : 0);
  if (delta.merge_parents) encode_merge_parents(out, *delta.merge_parents);
  out.u64(delta.lamport);
  out.u32(static_cast<uint32_t>(delta.upsert.size()));
  for (const auto& [key, value] : delta.upsert) {
    out.string(key);
    write_flat(out, value);
  }
  out.u32(static_cast<uint32_t>(delta.erase.size()));
  for (const auto& key : delta.erase) out.string(key);
  return out.take();
}

RegisterDelta decode_delta_bytes(std::span<const uint8_t> bytes, size_t key_max) {
  Reader in(bytes);
  const auto magic = in.view(delta_magic.size());
  if (!std::equal(magic.begin(), magic.end(), delta_magic.begin())) throw DecodeError("bad register map delta");
  RegisterDelta delta;
  delta.clock = decode_clock(in);
  if (in.u8() != 0) delta.merge_parents = decode_merge_parents(in);
  delta.lamport = in.u64();
  for (uint32_t i = 0, n = in.u32(); i < n; ++i) {
    auto key = in.string(key_max);
    delta.upsert[std::move(key)] = read_flat(in);
  }
  for (uint32_t i = 0, n = in.u32(); i < n; ++i) delta.erase.push_back(in.string(key_max));
  in.finish();
  return delta;
}

// The delta from `before` to `after`, the tree aside; nothing if a clock goes back or a
// floor or a form changes.
std::optional<RegisterDelta> head_delta(const RegisterHead& before, const RegisterHead& after) {
  if (before.floor != after.floor || !(before.form == after.form)) return std::nullopt;
  RegisterDelta delta;
  for (const auto& [author, sequence] : before.clock) {
    const auto found = after.clock.find(author);
    if (found == after.clock.end() || found->second < sequence) return std::nullopt;
  }
  for (const auto& [author, sequence] : after.clock) {
    const auto found = before.clock.find(author);
    if (found == before.clock.end() || found->second != sequence) delta.clock[author] = sequence;
  }
  if (before.merge_parents != after.merge_parents) delta.merge_parents = after.merge_parents;
  delta.lamport = after.lamport;
  return delta;
}

HeadClock joined_clock(const HeadClock& left, const HeadClock& right) {
  auto out = left;
  for (const auto& [author, sequence] : right) {
    auto& current = out[author];
    current = std::max(current, sequence);
  }
  return out;
}

void check_bounds(const RegisterOp& op) {
  if (op.key.empty()) throw std::invalid_argument("a register key is never empty");
  if (op.key.size() > TreeLimits{}.key_max_bytes) throw std::invalid_argument("register key past " + std::to_string(TreeLimits{}.key_max_bytes) + " bytes");
  if (op.value.size() > RegisterMap::value_max_bytes)
    throw std::invalid_argument("register value of " + std::to_string(op.value.size()) + " bytes past the bound of " +
                                std::to_string(RegisterMap::value_max_bytes));
  if (op.refs.size() > RegisterMap::refs_max)
    throw std::invalid_argument("register names " + std::to_string(op.refs.size()) + " objects, past the bound of " + std::to_string(RegisterMap::refs_max));
}

}  // namespace

// ---- The tree's value --------------------------------------------------------

// A register in its leaf: its fields, then its refs none, inline, or past the tree's
// child_inline_max as a child sequence of their own, so the leaf stays small.
class RegisterValues final : public TreeValues<Register> {
 public:
  void encode(Writer& out, const Register& value, TreeChildWriter& children) const override {
    write_fields(out, value);
    if (value.refs.empty()) {
      out.u8(refs_none);
      return;
    }
    if (value.refs.size() <= children.inline_max()) {
      out.u8(refs_inline);
      out.u32(static_cast<uint32_t>(value.refs.size()));
      out.raw(flat_refs(value.refs));
      return;
    }
    out.u8(refs_child);
    out.u64(value.refs.size());
    out.fixed(children.build(flat_refs(value.refs)).bytes);
  }

  Register decode(Reader& in, const TreeChildReader& children, bool with_children) const override {
    Register value;
    read_fields(in, value);
    switch (in.u8()) {
      case refs_none:
        break;
      case refs_inline: {
        const auto count = in.u32();
        if (count > RegisterMap::refs_max) throw DecodeError("register names too many objects");
        for (uint32_t i = 0; i < count; ++i) value.refs.push_back(ObjectId{in.fixed<32>()});
        break;
      }
      case refs_child: {
        const auto count = in.u64();
        if (count > RegisterMap::refs_max) throw DecodeError("register names too many objects");
        const ObjectId root{in.fixed<32>()};
        if (with_children) {
          Bytes records;
          children.read(root, records);
          if (records.size() != count * sizeof(ObjectId::bytes)) throw DecodeError("register child sequence is not its count");
          Reader reader(records);
          while (reader.remaining()) value.refs.push_back(ObjectId{reader.fixed<32>()});
        }
        break;
      }
      default:
        throw DecodeError("bad register refs form");
    }
    return value;
  }

  TreeReferences references(const Register& value, TreeChildWriter& children) const override {
    TreeReferences out;
    out.items = value.refs.size();
    out.data = value.refs;
    if (value.refs.size() > children.inline_max()) (void)children.build(flat_refs(value.refs));
    return out;
  }

  void skip(Reader& in) const override { (void)shape(in); }

  TreeRecordShape shape(Reader& in) const override {
    TreeRecordShape out;
    const auto flags = in.u8();
    if (flags & ~(flag_tombstone | flag_expiry)) throw DecodeError("bad register flags");
    (void)in.view(8 + node_bytes + 8 + ((flags & flag_expiry) ? 8 : 0));
    (void)in.view_bytes(RegisterMap::value_max_bytes);
    switch (in.u8()) {
      case refs_none:
        break;
      case refs_inline:
        out.items = in.u32();
        if (out.items > RegisterMap::refs_max) throw DecodeError("register names too many objects");
        for (uint64_t i = 0; i < out.items; ++i) out.data.push_back(ObjectId{in.fixed<32>()});
        break;
      case refs_child:
        out.items = in.u64();
        out.child_root = ObjectId{in.fixed<32>()};
        break;
      default:
        throw DecodeError("bad register refs form");
    }
    return out;
  }

  std::span<const uint8_t> child_boundary(std::span<const uint8_t> record) const override { return record; }

  std::optional<ObjectId> child_data(std::span<const uint8_t> record) const override {
    Reader reader(record);
    return ObjectId{reader.fixed<32>()};
  }
};

namespace {

const RegisterValues& register_values() {
  static const RegisterValues values;
  return values;
}

}  // namespace

// The register map's tree. Its walks are Tree's own (leaf_holding, walk_from): the node
// format is read in one place.
class RegisterTree final : public Tree<Register> {
 public:
  using Visit = std::function<bool(const std::string& key, const Register& value)>;

  RegisterTree() : Tree<Register>(register_values(), register_form()) {}

  // The leaf `key` falls in, or would, its registers without their child records.
  Leaf leaf_of(const ObjectId& root, std::string_view key, const TreeNodeStore& store) const { return leaf_holding(root, key, store); }

  // Each register at or after `from` in key order, read whole, until `visit` says no.
  void walk_from(const ObjectId& root, const TreeNodeStore& store, std::string_view from, const Visit& visit) const {
    Tree<Register>::walk_from(root, store, from, [&](Item&& item) { return visit(item.first, item.second); });
  }
};

namespace {

// ---- The stamp seam ---------------------------------------------------------

// What a write's stamp is. Lamport only, until Dannan's HlcClock::admit is released.
class RegisterStamp {
 public:
  virtual ~RegisterStamp() = default;
  // The stamp every register of one commit on `parent` takes.
  virtual uint64_t next(const RegisterHead& parent, const MutationContext& context) const = 0;
};

// One more than the highest stamp the parent's map has carried, so a node's next write is
// above everything it has seen.
class LamportStamp final : public RegisterStamp {
 public:
  uint64_t next(const RegisterHead& parent, const MutationContext&) const override { return parent.lamport + 1; }
};

const RegisterStamp& lamport_stamp() {
  static const LamportStamp stamp;
  return stamp;
}

// ---- The codec --------------------------------------------------------------

class RegisterCodec final : public Codec {
 public:
  explicit RegisterCodec(std::shared_ptr<const RegisterTree> tree) : _tree(std::move(tree)) {}

  Bytes genesis() const override { return Bytes(genesis_payload.begin(), genesis_payload.end()); }
  State decode_full(std::span<const uint8_t> payload) const override { return std::make_shared<const RegisterHead>(decode_head(payload)); }
  Bytes encode_full(const State& state) const override { return encode_head(head_cast(state)); }

  // Only what is not the tree: a change of root is said by the mutation's own delta.
  std::optional<Bytes> encode_delta(const State& before, const State& after) const override {
    const auto& from = head_cast(before);
    const auto& to = head_cast(after);
    if (from.root != to.root) return std::nullopt;
    const auto delta = head_delta(from, to);
    if (!delta) return std::nullopt;
    return encode_delta_bytes(*delta);
  }

  void check_delta(std::span<const uint8_t> delta) const override { (void)decode_delta_bytes(delta, _tree->form().limits.key_max_bytes); }

  State apply_delta(const State& parent, std::span<const uint8_t> bytes, TreeNodeStore* tree) const override {
    const auto delta = decode_delta_bytes(bytes, _tree->form().limits.key_max_bytes);
    auto next = std::make_shared<RegisterHead>(head_cast(parent));
    for (const auto& [author, sequence] : delta.clock) next->clock[author] = sequence;
    if (delta.merge_parents) next->merge_parents = *delta.merge_parents;
    next->lamport = delta.lamport;
    if (!delta.upsert.empty() || !delta.erase.empty()) {
      if (!next->root) throw DecodeError("a register map delta names keys of a head with no tree");
      if (tree == nullptr) throw DecodeError("a register map delta needs a node store");
      Tree<Register>::Delta changes;
      changes.erase = delta.erase;
      changes.upsert = delta.upsert;
      next->root = _tree->apply(*next->root, *tree, changes);
    }
    return next;
  }

  Bytes encode_after_delta(std::span<const uint8_t> delta, const State& state) const override {
    if (form_version(delta, "DGRGDLT") != 1) throw DecodeError("bad register map delta");
    return encode_head(head_cast(state));
  }

  HeadFields head_fields(const State& state) const override {
    const auto& head = head_cast(state);
    HeadFields fields{head.clock, head.merge_parents, head.floor, head.root, {}};
    // The options are the space's policy: no mutation moves them.
    if (head.floor != 0) {
      Writer out;
      write_form(out, head.form);
      fields.policy = out.take();
    }
    return fields;
  }

  uint64_t resident_bytes(const State& state) const override {
    const auto& head = head_cast(state);
    return sizeof(RegisterHead) + head.clock.size() * 64 + head.merge_parents.size() * 32;
  }

 private:
  const std::shared_ptr<const RegisterTree> _tree;
};

// ---- The merge --------------------------------------------------------------

// Per key, the greater register, and nothing else: no base, no clock of this node's, so
// every reconciler of a pair mints one commit (H8). Tombstones are kept whatever their
// age: only a mutation drops one.
class RegisterMerge final : public Merge {
 public:
  explicit RegisterMerge(std::shared_ptr<const RegisterTree> tree) : _tree(std::move(tree)) {}

  Result merge(const std::optional<State>& base, const MergeHeads& heads) const override {
    (void)base;
    const auto& primary = head_cast(heads.primary_state);
    const auto& secondary = head_cast(heads.secondary_state);
    Result out;
    if (primary.floor && secondary.floor && !(primary.form == secondary.form)) {
      out.why = "the two heads record different register options: " + form_text(primary.form) + " against " + form_text(secondary.form);
      return out;
    }
    auto merged = std::make_shared<RegisterHead>();
    merged->floor = std::max(primary.floor, secondary.floor);
    merged->clock = joined_clock(primary.clock, secondary.clock);
    merged->merge_parents = {heads.secondary.hash};
    merged->form = primary.floor ? primary.form : secondary.form;
    merged->lamport = std::max(primary.lamport, secondary.lamport);

    Tree<Register>::Changes changes;
    if (primary.root && secondary.root) {
      const bool wins = merged->form.tombstone_wins;
      for (auto& [key, difference] : _tree->diff(*primary.root, *secondary.root, heads.reader, heads.pause)) {
        if (heads.pause) heads.pause();
        std::optional<Register> winner;
        if (difference.before && difference.after) {
          winner = greater(*difference.before, *difference.after, wins, out.conflicts_created);
        } else {
          winner = difference.before ? difference.before : difference.after;
        }
        if (winner != difference.before) changes[key] = std::move(winner);
      }
      merged->root = _tree->update(*primary.root, heads.nodes, changes);
    } else {
      merged->root = primary.root ? primary.root : secondary.root;
    }

    for (const auto& [key, value] : changes)
      if (value) out.claims.data.insert(out.claims.data.end(), value->refs.begin(), value->refs.end());
    if (primary.root || !secondary.root) {
      if (auto delta = head_delta(primary, *merged)) {
        for (const auto& [key, value] : changes) {
          if (value)
            delta->upsert[key] = *value;
          else
            delta->erase.push_back(key);
        }
        out.delta = encode_delta_bytes(*delta);
      }
    }
    out.state = std::move(merged);
    return out;
  }

 private:
  // Equal order with different bytes cannot come from one node; should it, the lexically
  // greater bytes, counted as a conflict.
  static const Register& greater(const Register& a, const Register& b, bool tombstone_wins, uint64_t& conflicts) {
    if (register_before(a, b, tombstone_wins)) return b;
    if (register_before(b, a, tombstone_wins)) return a;
    if (a == b) return a;
    ++conflicts;
    return flat_bytes(a) > flat_bytes(b) ? a : b;
  }

  const std::shared_ptr<const RegisterTree> _tree;
};

// ---- The mutation -----------------------------------------------------------

class RegisterMutation final : public Mutation {
 public:
  RegisterMutation(const State& parent, std::shared_ptr<const RegisterTree> tree, RegisterForm form, std::shared_ptr<const std::vector<RegisterOp>> ops,
                   std::shared_ptr<RegisterWritten> written)
      : _parent(head_cast(parent)),
        _next(std::make_shared<RegisterHead>(_parent)),
        _tree(std::move(tree)),
        _form(form),
        _ops(std::move(ops)),
        _written(std::move(written)) {
    _delta.lamport = _parent.lamport;
  }

  // Every conditional op checked against this node's parent, before anything is written.
  void edit(const MutationContext& context) override {
    if (_parent.floor && !(_parent.form == _form)) throw RegisterRefused(form_refusal(_parent.form, _form));
    _next->merge_parents.clear();
    if (!_parent.merge_parents.empty()) _delta.merge_parents = _next->merge_parents;
    const bool never_reused = _form.tombstones.kind == TombstonePolicy::Kind::never && _form.tombstone_wins;
    for (const auto& op : *_ops) {
      const auto& held = parent_register(op.key, context);
      // What a read of the parent says: an expired register is absent.
      std::optional<Register> readable;
      if (held && !RegisterMap::expired_at(*held, context.now_ns)) readable = held;
      if (op.kind == RegisterOp::Kind::put && never_reused && held && held->tombstone)
        throw RegisterConflict(op.key, readable, "a removed key is never written again in this space");
      switch (op.expect.kind) {
        case RegisterExpect::Kind::any:
          break;
        case RegisterExpect::Kind::absent:
          if (readable && (!readable->tombstone || never_reused)) throw RegisterConflict(op.key, readable, "expected the key absent");
          break;
        case RegisterExpect::Kind::version:
          if (!readable || readable->version() != op.expect.version) throw RegisterConflict(op.key, readable, "expected another version");
          break;
      }
    }
  }

  void stamp(const MutationContext& context) override {
    if (_ops->empty()) return;
    const auto next_stamp = lamport_stamp().next(_parent, context);
    if (_written) _written->versions.clear();
    for (const auto& op : *_ops) {
      Register value;
      value.tombstone = op.kind == RegisterOp::Kind::remove;
      if (!value.tombstone) {
        value.value = op.value;
        value.refs = op.refs;
        value.expires_ns = op.expires_ns;
      }
      value.stamp = next_stamp;
      value.node = context.dot.author;
      value.written_ns = context.now_ns;
      if (_written) _written->versions[op.key] = value.version();
      _delta.upsert[op.key] = std::move(value);
    }
    _next->lamport = next_stamp;
    _delta.lamport = next_stamp;
  }

  void advance_clock(const MutationContext& context) override {
    _next->clock[context.dot.author] = context.dot.sequence;
    _delta.clock[context.dot.author] = context.dot.sequence;
    if (context.identity) {
      _next->clock[context.identity->origin] = context.identity->sequence;
      _delta.clock[context.identity->origin] = context.identity->sequence;
    }
  }

  // The ops, and in each leaf they touch the expired registers and the tombstones past the
  // window, dropped: the merge never drops one.
  void apply(const MutationContext& context) override {
    if (_delta.upsert.empty()) return;
    const auto window_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(_form.tombstones.window).count();
    const bool windowed = _form.tombstones.kind == TombstonePolicy::Kind::window;
    std::set<ObjectId> swept;
    std::set<std::string> dropped;
    for (const auto& [key, value] : _delta.upsert) {
      auto leaf = _tree->leaf_of(*_parent.root, key, context.reader);
      if (!swept.insert(leaf.id).second) continue;
      for (const auto& [neighbour, held] : leaf.items) {
        if (_delta.upsert.contains(neighbour)) continue;
        const bool lapsed = RegisterMap::expired_at(held, context.now_ns);
        const bool aged = windowed && held.tombstone && context.now_ns - held.written_ns > window_ns;
        if (lapsed || aged) dropped.insert(neighbour);
      }
    }
    _delta.erase.assign(dropped.begin(), dropped.end());
    Tree<Register>::Changes changes;
    for (const auto& key : _delta.erase) changes[key] = std::nullopt;
    for (const auto& [key, value] : _delta.upsert) changes[key] = value;
    _next->root = _tree->update(*_parent.root, context.nodes, changes);
  }

  State state() const override { return _next; }
  Bytes delta() const override { return encode_delta_bytes(_delta); }

  // What the registers written name, claimed under the dot; what the parent's named for
  // the same keys is carried, not sought.
  Introduced introduced(const MutationContext& context) const override {
    Introduced out;
    for (const auto& [key, value] : _delta.upsert) {
      out.data.insert(out.data.end(), value.refs.begin(), value.refs.end());
      out.brought.insert(out.brought.end(), value.refs.begin(), value.refs.end());
      if (const auto found = _parents.find(key); found != _parents.end() && found->second)
        out.carried.insert(out.carried.end(), found->second->refs.begin(), found->second->refs.end());
    }
    (void)context;
    return out;
  }

 private:
  const std::optional<Register>& parent_register(const std::string& key, const MutationContext& context) {
    const auto found = _parents.find(key);
    if (found != _parents.end()) return found->second;
    std::optional<Register> held;
    if (_parent.root) held = _tree->lookup(*_parent.root, key, context.reader);
    return _parents.emplace(key, std::move(held)).first->second;
  }

  const RegisterHead _parent;
  std::shared_ptr<RegisterHead> _next;
  const std::shared_ptr<const RegisterTree> _tree;
  const RegisterForm _form;
  const std::shared_ptr<const std::vector<RegisterOp>> _ops;
  const std::shared_ptr<RegisterWritten> _written;
  std::map<std::string, std::optional<Register>> _parents;
  RegisterDelta _delta;
};

}  // namespace

// ---- The public face --------------------------------------------------------

bool register_before(const Register& a, const Register& b, bool tombstone_wins) noexcept {
  if (tombstone_wins && a.tombstone != b.tombstone) return b.tombstone;
  return a.version() < b.version();
}

RegisterOp RegisterOp::put(std::string name, Bytes bytes, std::vector<ObjectId> objects, std::optional<int64_t> expiry, RegisterExpect expected) {
  RegisterOp op;
  op.kind = Kind::put;
  op.key = std::move(name);
  op.value = std::move(bytes);
  op.refs = std::move(objects);
  op.expires_ns = expiry;
  op.expect = expected;
  check_bounds(op);
  return op;
}

RegisterOp RegisterOp::remove(std::string name, RegisterExpect expected) {
  RegisterOp op;
  op.kind = Kind::remove;
  op.key = std::move(name);
  op.expect = expected;
  check_bounds(op);
  return op;
}

RegisterConflict::RegisterConflict(std::string key, std::optional<Register> found, const std::string& why)
    : std::runtime_error("register conflict on " + key + ": " + why), _key(std::move(key)), _found(std::move(found)) {}

RegisterMap::RegisterMap(RegisterOptions options, const dannan::contract::WallClock& wall, const dannan::contract::TimeSource& time)
    : _options(std::move(options)), _wall(wall), _time(time) {
  if (_options.form.stamp == StampKind::hlc)
    throw RegisterRefused(
        "register map stamp kind hlc waits on Dannan's HlcClock::admit, which Dannan 0.10.0 does not have: a peer's stamp must be admitted at acceptance "
        "before it is applied");
  if (_options.form.tombstone_wins && _options.form.tombstones.kind != TombstonePolicy::Kind::never)
    throw std::invalid_argument("tombstone_wins is a rule of TombstonePolicy::never: a window would drop the tombstone that wins");
  if (_options.form.tombstones.kind == TombstonePolicy::Kind::window && _options.form.tombstones.window.count() <= 0)
    throw std::invalid_argument("a tombstone window must be longer than zero");
  for (const auto& [key, value] : _options.founding) (void)RegisterOp::put(key, value);
  _tree = std::make_shared<const RegisterTree>();
  _codec = std::make_shared<const RegisterCodec>(_tree);
  _merge = std::make_shared<const RegisterMerge>(_tree);
}

RegisterMap::~RegisterMap() = default;

const Tree<Register>& RegisterMap::tree() const noexcept { return *_tree; }

SpaceSpec RegisterMap::spec(std::string name) const {
  SpaceSpec spec;
  spec.name = std::move(name);
  spec.codec = _codec;
  spec.merge = _merge;
  spec.policy = _options.policy;
  spec.magics = register_magics();
  spec.seed_magic = register_seed_magic;
  spec.tree = _tree;
  // A function of the options alone, so founders configured alike build one commit.
  spec.founding = [tree = _tree, form = _options.form, founding = _options.founding](TreeNodeStore& nodes) -> State {
    auto head = std::make_shared<RegisterHead>();
    head->floor = 1;
    head->form = form;
    Tree<Register>::Entries entries;
    for (const auto& [key, value] : founding) {
      Register entry;
      entry.value = value;
      entries.emplace(key, std::move(entry));
    }
    head->root = tree->build(entries, nodes);
    return head;
  };
  return spec;
}

SpaceSpec RegisterMap::spec(std::string name, RegisterOptions options) {
  // The spec's codec, merge and founding hold the tree; the map itself need not outlive it.
  const RegisterMap map(std::move(options));
  return map.spec(std::move(name));
}

MutationFactory RegisterMap::mutation(std::vector<RegisterOp> ops, std::shared_ptr<RegisterWritten> written) const {
  for (const auto& op : ops) check_bounds(op);
  auto shared = std::make_shared<const std::vector<RegisterOp>>(std::move(ops));
  return [tree = _tree, form = _options.form, shared, written](const State& parent) -> std::unique_ptr<Mutation> {
    return std::make_unique<RegisterMutation>(parent, tree, form, shared, written);
  };
}

RegisterCommit RegisterMap::commit(Space& space, std::vector<RegisterOp> ops, std::optional<ExternalIdentity> identity) const {
  auto written = std::make_shared<RegisterWritten>();
  RegisterCommit out;
  out.outcome = space.mutate_delta(mutation(std::move(ops), written), Space::default_retries, identity);
  // A covered outcome's versions are the accepted attempt's, or none when nothing was made.
  out.written = written->versions;
  return out;
}

std::optional<std::string> RegisterMap::refusal(const SpaceView& view) const {
  const auto& head = head_of(view);
  if (head.floor == 0 || head.form == _options.form) return std::nullopt;
  return form_refusal(head.form, _options.form);
}

void RegisterMap::check(const SpaceView& view) const {
  if (const auto why = refusal(view)) throw RegisterRefused(*why);
}

const RegisterHead& RegisterMap::head_of(const SpaceView& view) { return head_cast(view.state); }
const RegisterHead& RegisterMap::head_of(const State& state) { return head_cast(state); }

int64_t RegisterMap::now() const noexcept { return static_cast<int64_t>(_wall.unix_ms()) * 1000000; }

std::optional<Register> RegisterMap::held(const SpaceView& view, std::string_view key, const TreeNodeStore& nodes) const {
  check(view);
  const auto& head = head_of(view);
  if (!head.root || key.empty()) return std::nullopt;
  return _tree->lookup(*head.root, key, nodes);
}

std::optional<Register> RegisterMap::get(const SpaceView& view, std::string_view key, const TreeNodeStore& nodes) const {
  auto value = held(view, key, nodes);
  if (!value || value->tombstone || expired(*value)) return std::nullopt;
  return value;
}

std::optional<Register> RegisterMap::get_register(const SpaceView& view, std::string_view key, const TreeNodeStore& nodes) const {
  auto value = held(view, key, nodes);
  if (!value || expired(*value)) return std::nullopt;
  return value;
}

dannan::sched::Page<std::pair<std::string, Register>, std::string> RegisterMap::list(const SpaceView& view, std::string_view prefix,
                                                                                     dannan::sched::Cursor<std::string> cursor, dannan::sched::Budget& budget,
                                                                                     const TreeNodeStore& nodes) const {
  check(view);
  dannan::sched::Page<std::pair<std::string, Register>, std::string> page;
  page.next = cursor;
  const auto& head = head_of(view);
  bool stopped = false;
  if (head.root) {
    std::string from(prefix);
    if (cursor.after && *cursor.after >= from) from = *cursor.after + std::string(1, '\0');
    const auto now_ns = now();
    _tree->walk_from(*head.root, nodes, from, [&](const std::string& key, const Register& value) {
      if (!key.starts_with(prefix)) return false;
      if (const auto stop = budget.must_stop(_time.now())) {
        page.stopped = *stop;
        stopped = true;
        return false;
      }
      if (!budget.take_operation()) {
        page.stopped = Stop::budget;
        stopped = true;
        return false;
      }
      page.next.after = key;
      if (!value.tombstone && !expired_at(value, now_ns)) page.items.emplace_back(key, value);
      return true;
    });
  }
  if (!stopped) {
    page.next = {};
    page.stopped = Stop::end;
  }
  return page;
}

dannan::sched::Page<RegisterChange, std::string> RegisterMap::changed(const SpaceView& before, const SpaceView& after, std::string_view prefix,
                                                                      dannan::sched::Cursor<std::string> cursor, dannan::sched::Budget& budget,
                                                                      const TreeNodeStore& nodes) const {
  check(before);
  check(after);
  const auto& before_root = head_of(before).root;
  const auto& after_root = head_of(after).root;
  std::vector<RegisterChange> changes;
  if (before_root && after_root) {
    for (auto& [key, difference] : _tree->diff(*before_root, *after_root, nodes))
      if (key.starts_with(prefix)) changes.push_back({key, std::move(difference.before), std::move(difference.after)});
  } else if (before_root || after_root) {
    _tree->for_each_with_prefix(before_root ? *before_root : *after_root, nodes, prefix, [&](const std::string& key, const Register& value) {
      RegisterChange change{key, {}, {}};
      (before_root ? change.before : change.after) = value;
      changes.push_back(std::move(change));
    });
  }

  dannan::sched::Page<RegisterChange, std::string> page;
  page.next = cursor;
  for (auto& change : changes) {
    if (cursor.after && change.key <= *cursor.after) continue;
    if (const auto stop = budget.must_stop(_time.now())) {
      page.stopped = *stop;
      return page;
    }
    if (!budget.take_operation()) {
      page.stopped = Stop::budget;
      return page;
    }
    page.next.after = change.key;
    page.items.push_back(std::move(change));
  }
  page.next = {};
  page.stopped = Stop::end;
  return page;
}

std::vector<ObjectId> RegisterMap::named_objects(const SpaceView& view, const TreeNodeStore& nodes, const TreePause& pause) const {
  check(view);
  std::vector<ObjectId> out;
  const auto& head = head_of(view);
  if (!head.root) return out;
  // An expired register still names its objects until a mutation removes it: the safe way.
  _tree->walk(*head.root, nodes, [&](const std::string&, const Register& value) { out.insert(out.end(), value.refs.begin(), value.refs.end()); }, pause);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace dagda::heads
