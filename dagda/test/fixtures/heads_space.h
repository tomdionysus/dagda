//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// The neutral owner's side of a mutation (fixtures/heads.h has its record and codec):
// what Macha's mutate_delta does between its own steps, in neutral form, and a space
// to run it in. Beside the keys and notes a delta already says, a head's retirement
// batches are notes under "batch/" and its policy notes under "policy/". An item's
// `stamp` says which mutation wrote it, as a provenance dot does.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "dagda/contract/reachability.h"
#include "dagda/heads/messages.h"
#include "dagda/heads/retirement.h"
#include "dagda/heads/space.h"
#include "dagda/test/fixtures/heads.h"

namespace dagda::test {

using heads::ClaimBarrier;
using heads::CommitOutcome;
using heads::ExternalIdentity;
using heads::GarbageBatch;
using heads::Introduced;
using heads::Mutation;
using heads::MutationContext;
using heads::MutationFactory;
using heads::Space;
using heads::SpaceServices;
using heads::SpaceSpec;
using heads::SpaceView;

// Provenance, for a case whose merge reads it: an item's `tag` names its author, its `stamp` the
// sequence that wrote it. A plain tag says nothing, as an entry from before provenance does.
inline constexpr uint32_t provenance_bit = 0x80000000U;
inline uint32_t author_tag(const NodeId& author) {
  uint32_t tag = 0;
  for (const auto byte : author.bytes) tag = tag * 31 + byte;
  return tag | provenance_bit;
}
inline bool carries_provenance(const NeutralItem& item) { return (item.tag & provenance_bit) != 0; }
inline void stamp_item(NeutralItem& item, const Dot& dot) {
  item.tag = author_tag(dot.author);
  item.stamp = static_cast<int64_t>(dot.sequence);
}

inline constexpr Magic neutral_batch_magic{'N', 'T', 'R', 'L', 'B', 'T', 'C', '1'};
inline constexpr Magic neutral_seed_magic{'N', 'T', 'R', 'L', 'S', 'E', 'E', 'D'};

inline std::string batch_note(const ObjectId& id) { return "batch/" + store::to_string(id); }

// The batches a head names, sorted by id.
inline std::vector<GarbageBatch> neutral_batches(const NeutralHead& head) {
  std::vector<GarbageBatch> out;
  const auto& notes = head.notes.get();
  for (auto at = notes.lower_bound("batch/"); at != notes.end() && at->first.starts_with("batch/"); ++at) {
    const auto unhexed = dannan::identity::unhex(at->first.substr(6));
    if (!unhexed || unhexed->size() != 32) throw DecodeError("bad neutral batch name");
    GarbageBatch batch;
    std::copy(unhexed->begin(), unhexed->end(), batch.id.bytes.begin());
    const auto colon = at->second.find(':');
    batch.retired_at_ns = std::stoll(at->second.substr(0, colon));
    batch.count = static_cast<uint32_t>(std::stoul(at->second.substr(colon + 1)));
    out.push_back(batch);
  }
  std::sort(out.begin(), out.end(), [](const GarbageBatch& a, const GarbageBatch& b) { return a.id < b.id; });
  return out;
}

// One group of retirements a mutation asks for. `at_commit`: retired when it commits.
struct NeutralRetirement {
  bool at_commit = true;
  int64_t retired_at_ns = 0;
  std::vector<ObjectId> ids;
};

// What a neutral edit may do: the head as a working copy and the delta beside it. A
// note set on the head alone is a change the delta omits.
struct NeutralEditing {
  NeutralHead& head;
  NeutralDelta& delta;
  std::vector<NeutralRetirement>& retire;
  const MutationContext& context;
  void set_note(const std::string& key, const std::string& value) {
    head.notes.set(key, value);
    delta.set_notes[key] = value;
  }
  void erase_note(const std::string& key) {
    head.notes.erase(key);
    delta.erase_notes.push_back(key);
  }
  void drop_batch(const ObjectId& id) { erase_note(batch_note(id)); }
};
using NeutralEdit = std::function<void(NeutralEditing&)>;

// Records the order of the owner's steps.
using StepLog = std::vector<std::string>;

class NeutralMutation final : public Mutation {
 public:
  NeutralMutation(const State& parent, NeutralEdit edit, StepLog* steps = nullptr, bool provenance = false)
      : _parent(*NeutralCodec::as_head(parent)),
        _next(std::make_shared<NeutralHead>(_parent)),
        _edit(std::move(edit)),
        _steps(steps),
        _provenance(provenance) {}

  void edit(const MutationContext& context) override {
    log("edit");
    _next->merge_parents.clear();
    NeutralEditing editing{*_next, _delta, _retire, context};
    if (_edit) _edit(editing);
    if (!_parent.merge_parents.empty()) _delta.merge_parents = _next->merge_parents;
  }
  void stamp(const MutationContext& context) override {
    log(_next->clock == _parent.clock ? "stamp" : "stamp after the clock moved");
    for (auto& [key, item] : _delta.upsert_keys) {
      if (_provenance)
        stamp_item(item, context.dot);
      else
        item.stamp = static_cast<int64_t>(context.dot.sequence);
    }
  }
  void advance_clock(const MutationContext& context) override {
    log("advance");
    _next->clock[context.dot.author] = context.dot.sequence;
    _delta.clock[context.dot.author] = context.dot.sequence;
    if (context.identity) {
      _next->clock[context.identity->origin] = context.identity->sequence;
      _delta.clock[context.identity->origin] = context.identity->sequence;
    }
  }
  void apply(const MutationContext& context) override {
    log("apply");
    if (!_delta.upsert_keys.empty() || !_delta.erase_keys.empty()) {
      NeutralTree::Delta changes;
      changes.upsert = _delta.upsert_keys;
      changes.erase = _delta.erase_keys;
      _next->root = replay_tree().apply(*_next->root, context.nodes, changes);
    }
    for (auto& group : _retire) {
      const auto batch =
          heads::write_retirement_batch(context.nodes, neutral_batch_magic, group.at_commit ? context.now_ns : group.retired_at_ns, std::move(group.ids));
      if (!batch) continue;
      const auto value = std::to_string(batch->retired_at_ns) + ":" + std::to_string(batch->count);
      _next->notes.set(batch_note(batch->id), value);
      _delta.set_notes[batch_note(batch->id)] = value;
      _named.push_back(batch->id);
    }
    _retire.clear();
  }
  State state() const override { return _next; }
  Bytes delta() const override { return encode_neutral_delta(_delta); }
  Introduced introduced(const MutationContext& context) const override {
    Introduced out;
    const auto carry = [&](const std::string& key) {
      if (!_parent.root) return;
      if (const auto prior = replay_tree().lookup(*_parent.root, key, context.reader))
        for (const auto& ref : prior->refs)
          if (!ref.gap) out.carried.push_back(ref.id);
    };
    for (const auto& key : _delta.erase_keys) carry(key);
    for (const auto& [key, item] : _delta.upsert_keys) {
      for (const auto& ref : item.refs) {
        if (ref.gap) continue;
        out.data.push_back(ref.id);
        out.brought.push_back(ref.id);
      }
      carry(key);
    }
    out.control = _named;
    return out;
  }

 private:
  void log(const char* step) {
    if (_steps) _steps->push_back(step);
  }
  const NeutralHead _parent;
  std::shared_ptr<NeutralHead> _next;
  NeutralDelta _delta;
  std::vector<NeutralRetirement> _retire;
  std::vector<ObjectId> _named;
  NeutralEdit _edit;
  StepLog* _steps;
  const bool _provenance;
};

inline MutationFactory neutral_mutation(NeutralEdit edit, StepLog* steps = nullptr) {
  return [edit = std::move(edit), steps](const State& parent) { return std::make_unique<NeutralMutation>(parent, edit, steps); };
}

// A mutation whose keys carry provenance, for a head a merge will read.
inline MutationFactory stamped_mutation(NeutralEdit edit) {
  return [edit = std::move(edit)](const State& parent) { return std::make_unique<NeutralMutation>(parent, edit, nullptr, true); };
}

// A claim barrier a case scripts: what this node holds, what a node present holds,
// and everything it was asked in order. Thread-safe.
class ScriptedBarrier final : public ClaimBarrier {
 public:
  struct Claim {
    std::string kind;
    std::vector<ObjectId> ids;
    Dot dot;
  };
  std::set<ObjectId> here;
  std::set<ObjectId> elsewhere;
  bool control_holds = true;
  std::function<void()> on_claim;

  std::vector<ObjectId> retain_data_here(const std::vector<ObjectId>& ids, const Dot& dot) override {
    note({"data-here", ids, dot});
    std::vector<ObjectId> rest;
    for (const auto& id : ids)
      if (!here.contains(id)) rest.push_back(id);
    return rest;
  }
  std::vector<ObjectId> retain_data(const std::vector<ObjectId>& ids, const Dot& dot) override {
    note({"data", ids, dot});
    std::vector<ObjectId> unheld;
    for (const auto& id : ids)
      if (!elsewhere.contains(id)) unheld.push_back(id);
    return unheld;
  }
  bool retain_control_here(const std::vector<ObjectId>& ids, const Dot& dot) override {
    note({"control", ids, dot});
    return control_holds;
  }
  std::vector<Claim> taken() {
    std::lock_guard lock(_mutex);
    return _claims;
  }

 private:
  void note(Claim claim) {
    if (on_claim) on_claim();
    std::lock_guard lock(_mutex);
    _claims.push_back(std::move(claim));
  }
  std::mutex _mutex;
  std::vector<Claim> _claims;
};

inline SpaceSpec neutral_spec(std::string name = "test/neutral", std::shared_ptr<const Codec> codec = std::make_shared<NeutralCodec>()) {
  SpaceSpec spec;
  spec.name = std::move(name);
  spec.codec = std::move(codec);
  spec.magics = neutral_magics();
  spec.seed_magic = neutral_seed_magic;
  spec.tree = std::make_shared<NeutralTree>(neutral_tree());
  return spec;
}

// What founding a neutral space makes: the same head from the same entries, as founders
// configured alike agree. A note past what a delta says keeps a delta smaller than a
// whole record, as a real head does.
inline std::function<State(heads::TreeNodeStore&)> neutral_founding(NeutralEntries entries = {{"a", make_group(1)}}, std::string note = {}) {
  return [entries = std::move(entries), note = std::move(note)](heads::TreeNodeStore& nodes) -> State {
    auto head = std::make_shared<NeutralHead>();
    head->floor = 1;
    head->notes.set("padding", std::string(1024, '.'));
    if (!note.empty()) head->notes.set("founding", note);
    head->root = neutral_tree().build(entries, nodes);
    return head;
  };
}

// The space as founded on its node.
inline SpaceSpec founded_spec(std::string name = "test/neutral") {
  auto spec = neutral_spec(std::move(name));
  spec.founding = neutral_founding();
  return spec;
}

// The table of the plan's 2.3, which on an owner's node of those numbers takes the place
// of Dagda's own: 512 plus each number the owner's peers know. Tests hold it; the library
// does not.
inline heads::message::Numbers owner_numbers() {
  const auto at = [](uint16_t number) { return dannan::net::MessageType{static_cast<uint16_t>(512 + number)}; };
  heads::message::Numbers numbers;
  numbers.put_history_entry = at(28);
  numbers.put_commit = at(30);
  numbers.accept_commit = at(31);
  numbers.get_history_entry = at(26);
  numbers.has_history_entry = at(27);
  numbers.get_heads = at(29);
  numbers.get_history_record = at(41);
  numbers.get_committed = at(10);
  numbers.get_current = at(6);
  numbers.get_identity = at(21);
  numbers.notice = at(9);
  numbers.record_reply = at(105);
  numbers.identity_reply = at(108);
  numbers.history_entry_reply = at(111);
  numbers.heads_reply = at(112);
  return numbers;
}

// A space on its own: its replica on a temporary directory, its nodes in memory.
struct SpaceBed {
  CostCounters costs;
  SharedTreeStore store{&costs};
  ReplicaBed bed;
  ScriptedBarrier barrier;
  dannan::test::SteppedWall wall;
  dannan::sched::WaitGuard guard{quiet_logger(), dannan::sched::WaitGuard::Mode::throw_on_violation};
  std::shared_ptr<RecordingSink> sink = std::make_shared<RecordingSink>();
  NodeId self = author(1);
  SpaceSpec spec = neutral_spec();
  std::function<void(const Acceptance&, bool, const Record&)> accepted;
  std::function<uint64_t()> membership;
  std::function<Record()> converge;

  std::unique_ptr<Space> open(const std::string& name = "space", bool pristine = true) {
    auto replica = bed.options(name);
    replica.tree_store = store.factory();
    replica.costs = &costs;
    replica.accept_pristine_genesis_authority = pristine;
    replica.logger = logger_into(sink);
    SpaceServices services;
    services.self = self;
    services.commit_nodes = store.factory();
    services.read_nodes = store.factory();
    services.claims = &barrier;
    services.wall = &wall;
    services.guard = &guard;
    services.space = neutral_space();
    services.membership_stamp = membership;
    services.accepted = accepted;
    services.converge = converge;
    auto named = spec;
    named.codec = bed.codec;
    return std::make_unique<Space>(std::move(named), std::move(replica), std::move(services));
  }

  // A head on a tree holding `entries`: what S5's founding will make, a generation 2
  // commit with no dot, made here by hand.
  NeutralHead found(Space& space, const NeutralEntries& entries = {{"a", make_group(1)}}) {
    NeutralHead head;
    head.floor = 1;
    // Past what a delta with a key or two in it says, as a real head is.
    head.notes.set("padding", std::string(1024, '.'));
    const auto writer = store.factory()();
    head.root = neutral_tree().build(entries, *writer);
    commit_record(space.replica(), record_of(2, bed.genesis().hash, head));
    return head;
  }
};

// An owner's reachability over a neutral space, as Macha's filesystem answers it over
// its namespace: what the head's tree refers to, and its batches. A retirement is
// collected by a mutation, as Macha's maintain_garbage_metadata and
// maintain_tombstone_batches commit theirs.
class NeutralSpaceProvider final : public contract::ReachabilityProvider {
 public:
  NeutralSpaceProvider(Space& space, std::function<std::unique_ptr<heads::TreeNodeStore>()> read_nodes) : _space(space), _read_nodes(std::move(read_nodes)) {}

  uint64_t generation() override { return _space.replica().generation(); }
  uint64_t known_generation() override { return generation(); }
  contract::Reachability inventory(const std::function<void()>& pause) override {
    const auto view = _space.local();
    auto out = answer(*NeutralCodec::as_head(view.state), pause);
    const auto reader = _read_nodes();
    out.batches = neutral_batches(*NeutralCodec::as_head(view.state));
    out.garbage = heads::retirements_of(out.batches, *reader, neutral_batch_magic, nullptr, pause);
    out.generation = view.generation;
    out.complete = true;
    latest = out;
    return out;
  }
  std::optional<contract::Hash256> release_head() override {
    const auto heads = _space.usable_heads();
    if (heads.size() != 1) return std::nullopt;
    return heads.front().hash;
  }
  contract::Reachability release(const contract::Hash256& head) override {
    const auto held = _space.replica().materialized(head);
    if (!held) throw std::runtime_error("head unreadable");
    const auto& neutral = *NeutralCodec::as_head(held->state);
    auto out = answer(neutral, {});
    out.head = head;
    out.clock = neutral.clock;
    out.complete = true;
    return out;
  }
  std::optional<contract::ControlEpoch> control_epoch() override { return contract::ControlEpoch{1, {}}; }
  bool within_control_epoch(uint64_t, const std::function<void()>& removal) override {
    removal();
    return true;
  }
  void collect_garbage(const contract::GarbageCollection& collection) override {
    if (!collection.erase.empty()) {
      const auto erase = collection.erase;
      const auto reader = _read_nodes();
      _space.mutate_delta(neutral_mutation([erase, &reader](NeutralEditing& editing) {
        const auto batches = neutral_batches(editing.head);
        if (batches.empty()) return;
        const auto rewrite = heads::rewrite_retirements(batches, *reader, neutral_batch_magic, erase);
        for (const auto& dropped : rewrite.dropped) editing.drop_batch(dropped);
        for (const auto& rest : rewrite.rest) editing.retire.push_back({false, rest.retired_at_ns, rest.ids});
      }));
    }
    if (!collection.matured_batches.empty()) {
      const auto named = neutral_batches(*NeutralCodec::as_head(_space.local().state));
      std::vector<ObjectId> matured;
      for (const auto& id : collection.matured_batches)
        if (std::any_of(named.begin(), named.end(), [&](const GarbageBatch& batch) { return batch.id == id; })) matured.push_back(id);
      if (!matured.empty())
        _space.mutate_delta(neutral_mutation([matured](NeutralEditing& editing) {
          for (const auto& id : matured) editing.drop_batch(id);
        }));
    }
    collected.push_back(collection);
  }

  contract::Reachability latest;
  std::vector<contract::GarbageCollection> collected;
  // What else the owner's head roots beyond its tree: its conflicts' values, for one.
  std::function<void(const NeutralHead&, contract::Reachability&)> extra_roots;

 private:
  contract::Reachability answer(const NeutralHead& head, const std::function<void()>& pause) {
    contract::Reachability out;
    if (!head.root) return out;
    const auto reader = _read_nodes();
    replay_tree().collect_nodes(*head.root, *reader, out.control, pause);
    replay_tree().walk(
        *head.root, *reader,
        [&](const std::string&, const NeutralItem& item) {
          for (const auto& ref : item.refs)
            if (!ref.gap) out.data.push_back(ref.id);
        },
        pause);
    if (extra_roots) extra_roots(head, out);
    return out;
  }

  Space& _space;
  const std::function<std::unique_ptr<heads::TreeNodeStore>()> _read_nodes;
};

}  // namespace dagda::test
