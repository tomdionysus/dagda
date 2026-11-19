//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/heads/cache_seed.h"
#include "dagda/heads/codec.h"
#include "dagda/heads/record.h"
#include "dagda/heads/replica.h"
#include "dagda/heads/tree.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/work.h"
#include "dannan/status/log.h"

namespace dagda::heads {

// A mutation or a read this node cannot serve now, and may on another try: Macha's
// MetadataNotReady. A commit's own refusal says which floor failed.
class HeadsNotReady final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Exchange;

// A key a caller owns, naming its mutation so that it can tell after a crash whether
// it took effect: Macha's MetadataMutationIdentity. `sequence` increases across that
// caller's mutations, and a head whose clock has reached it holds the mutation.
struct ExternalIdentity {
  NodeId origin{};
  uint64_t sequence = 0;
};

// Where a space's heads are kept. Macha has one every-node space. `node_local` is
// this node's alone (slice 1, D3): never advertised, exchanged, offered or repaired,
// its tree nodes in a pack of its own under the node-local key. `replicas: N` is L2's,
// which waits on holders that do not exist yet (slice 2), and open() refuses it.
struct Policy {
  enum class Kind : uint8_t { every_node, replicas, node_local };
  Kind kind = Kind::every_node;
  uint32_t replicas = 0;

  static Policy every_node() { return {}; }
  static Policy replicas_of(uint32_t count) { return {Kind::replicas, count}; }
  static Policy node_local() { return {Kind::node_local, 0}; }
  bool is_node_local() const noexcept { return kind == Kind::node_local; }
};

// Nothing, or why a space may not open under `policy`.
std::optional<std::string> policy_refusal(const Policy& policy);

// The claims a commit named: what the peers' share is, not a second derivation.
struct CommitClaims {
  std::vector<ObjectId> data;
  std::vector<ObjectId> control;
  uint64_t bytes() const noexcept { return (data.size() + control.size()) * sizeof(ObjectId); }
};

// Two heads the reconciler is folding, in the one order every reconciler takes them:
// `primary` has the lower hash, and is the commit's previous; `secondary` is its one merge
// parent. Valid for the call.
struct MergeHeads {
  const Record& primary;
  const Record& secondary;
  const State& primary_state;
  const State& secondary_state;
  // The node store as it stands, and the one that writes the merged tree, on this node alone.
  const TreeNodeStore& reader;
  TreeNodeStore& nodes;
  const TreePause& pause;
};

// The owner's merge of two heads: an owner's type lives behind this, and Dagda knows only
// what it says of the result. Thread-safe.
//
// Every reconciler of a pair must mint one commit (H8), so the state is a function of the
// two heads alone: their clocks joined and nothing of this node's in them (R3), their tree
// merged by what differs, and merge parents of exactly the secondary's hash.
//
// `base` is the two heads' common ancestor as this node holds it, and is outside that
// promise. History truncates per node (H16), so one reconciler may hold the ancestor and
// another not; a merge that lets `base` change its result makes sibling commits, which are
// two heads like any others and are folded in their turn. Such a merge must settle when it
// folds its own siblings, or the nodes never reach one head (the plan's Q5, C40). The walk
// is only made for a merge that says it reads `base`; Macha's does not.
class Merge {
 public:
  struct Result {
    // Empty, with `why`: the merge cannot be made with what this node can reach now, and
    // the head that is not this node's own is set aside (H10).
    State state;
    std::string why;
    // The change against the primary, when the owner can say it. The frame is this if it is
    // strictly smaller than the record, else the record (H9).
    Bytes delta;
    // What the commit brings in beside its tree's nodes, which Dagda collects itself: objects
    // the change names, conflict alternatives among them. All claimed here under the merge's
    // dot (H22). None is looked for elsewhere: a merge refuses nothing for an object this node
    // does not hold, as its parents' holders may be away.
    CommitClaims claims;
    // For the reconciliation's line and figures. The conflicts are the owner's blob (H11).
    uint64_t conflicts_created = 0;
    uint64_t conflicts_superseded = 0;
    uint64_t conflicts_standing = 0;
  };
  virtual ~Merge() = default;
  // Throwing DecodeError is as `why`: a node one branch needs that no node present can supply.
  // Anything else ends the read or the repair run that asked, and what a pause throws goes
  // through.
  virtual Result merge(const std::optional<State>& base, const MergeHeads& heads) const = 0;
  virtual bool reads_base() const { return false; }
  // Told once the merge is published here, holding the reconciliation's lock: what the owner
  // says of it, such as its conflicts, in its own words.
  virtual void published(const Result& result, const Record& commit) const {
    (void)result;
    (void)commit;
  }
};

// What an owner gives a mutation attempt. Valid for that attempt.
struct MutationContext {
  const Record& parent;
  const State& parent_state;
  const HeadFields& parent_fields;
  // Durable before the owner sees it, and the same on every attempt.
  Dot dot;
  const std::optional<ExternalIdentity>& identity;
  // The node store as it stands, and the one that writes this attempt's nodes, here
  // alone: a tree's, and any other CONTROL object the head will name.
  const TreeNodeStore& reader;
  TreeNodeStore& nodes;
  // The node's wall clock at this attempt.
  int64_t now_ns = 0;
  const TreePause& pause;
};

// What a commit brings in, as the owner reads it from its own change (Macha's
// retain_metadata_publication, service/node_services.cpp:376-449). `data`: every DATA
// object it refers to afresh, claimed here when held here. `brought`: those a node
// present must hold, or the commit would publish bytes nobody here has. `carried`:
// what the parent already named, whose holder may be away and which is not looked for.
// `control`: CONTROL objects the head names beside its tree's nodes.
struct Introduced {
  std::vector<ObjectId> data;
  std::vector<ObjectId> brought;
  std::vector<ObjectId> carried;
  std::vector<ObjectId> control;
};

// The owner's half of one attempt, called in Macha's order (mgr:1699-1811) and never
// from two threads at once. An owner's state is opaque here, so everything that edits
// it is the owner's and everything about the order is Dagda's. Whatever is thrown
// before the commit is accepted ends the mutation, unless it is HeadsNotReady from
// the acceptance, which the path retries with the same dot.
class Mutation {
 public:
  virtual ~Mutation() = default;
  // The caller's edit, on a working copy of the parent with its merge parents cleared.
  virtual void edit(const MutationContext& context) = 0;
  // What every change says of where it came from, against the parent. After the
  // policy is checked, and with the working clock still the parent's.
  virtual void stamp(const MutationContext& context) = 0;
  // Writes the dot, and the identity when there is one, into the clock and the delta.
  virtual void advance_clock(const MutationContext& context) = 0;
  // The change applied to the tree through `context.nodes`, and whatever else the
  // head names written there; then what depends on the tree as it now stands.
  virtual void apply(const MutationContext& context) = 0;
  // The working copy, cheap to take at any point after edit().
  virtual State state() const = 0;
  // The exact change, for mutate_delta. Its size decides the frame's body.
  virtual Bytes delta() const = 0;
  virtual Introduced introduced(const MutationContext& context) const = 0;
};

using MutationFactory = std::function<std::unique_ptr<Mutation>(const State& parent)>;

// A commit's claims on this node, on the store (H22). Thread-safe.
class ClaimBarrier {
 public:
  virtual ~ClaimBarrier() = default;
  // Claims here whichever of `ids` this node holds, and returns the rest sorted.
  virtual std::vector<ObjectId> retain_data_here(const std::vector<ObjectId>& ids, const Dot& dot) = 0;
  // Claims on a node present; returns, sorted, those no node present holds.
  virtual std::vector<ObjectId> retain_data(const std::vector<ObjectId>& ids, const Dot& dot) = 0;
  // Claims here; false when the claim did not hold.
  virtual bool retain_control_here(const std::vector<ObjectId>& ids, const Dot& dot) = 0;
  // A commit's claims on this node stand: what a claim's waiters wake on.
  virtual void claimed() {}
  // The peers' share of a commit already accepted here, from the replicator. An object no
  // node present holds is repair's to find, not a reason to stop. May throw, leaving it owed.
  virtual void retain_on_peers(const Dot& dot, const CommitClaims& claims) {
    (void)dot;
    (void)claims;
  }
};

// One commit's claims, kept for the peers.
struct OwedClaims {
  Dot dot;
  CommitClaims claims;
};

// What the replicator (session S5) owes the peers: each commit's claims in order, and
// the newest head, whose history carries the rest. Past the bound the oldest are
// dropped and the peer's own head protects what it holds.
struct OwedWork {
  std::deque<OwedClaims> claims;
  std::optional<Record> head;
};

// What the peers are owed, as Status shows it: commits whose claims are queued, those
// claims' size, and claims dropped past the bound since start.
struct OwedBacklog {
  size_t commits = 0;
  uint64_t claim_bytes = 0;
  uint64_t dropped = 0;
};

// Mutation wall time split into the claims and the local commit; totals and maxima since
// start. Measured for Status alone.
struct MutationTiming {
  uint64_t mutations = 0;
  uint64_t retention_ms_total = 0;
  uint64_t retention_ms_max = 0;
  uint64_t publish_ms_total = 0;
  uint64_t publish_ms_max = 0;
};

// What the exchange knows of the peers' heads beyond this node's own: an epoch that moves
// with every notice, and the highest generation heard of that is news.
struct RemoteKnown {
  uint64_t epoch = 0;
  uint64_t generation = 0;
};

// What a space's reads are at: a head, its state, and the witness that moves when the
// tree does.
struct SpaceView {
  uint64_t generation = 0;
  uint64_t revision = 0;
  Hash256 hash{};
  State state;
};

// A move of this node's committed head, as a watch is told of it (the three-systems
// design, D4): the head before and the head after.
struct HeadMoved {
  HeadIdentity before;
  HeadIdentity after;
  bool operator==(const HeadMoved&) const = default;
};

// What a mutation came to. `covered`: an earlier attempt's dot, or the identity, is
// already in the head, and nothing was committed. `unchanged`: the payload equalled
// the parent's (Macha's branch, unreachable while the clock is in the payload, R1).
struct CommitOutcome {
  enum class Kind : uint8_t { committed, covered, unchanged };
  Kind kind = Kind::committed;
  Record record;
  std::optional<Dot> dot;
  HistoryEntry::Body body = HistoryEntry::Body::full;
  size_t attempts = 0;
  CommitClaims claims;
};

// The heads as this node holds them, as evidence.
struct HeadsReport {
  std::vector<Acceptance> certificates;
  std::vector<HeadIdentity> usable;
  HeadIdentity preferred;
  uint64_t set_aside_generation = 0;
  bool recovery_required = false;
};

// What an owner names at open(). `codec` and `merge` are the owner's; so are the
// magics, the tree's shape (none: a space with no tree) and the rest.
struct SpaceSpec {
  std::string name;
  std::shared_ptr<const Codec> codec;
  std::shared_ptr<const Merge> merge;
  Policy policy;
  ReplicaMagics magics;
  // The cache seed's magic.
  Magic seed_magic{};
  std::shared_ptr<const TreeShape> tree;
  // A founder holds genesis as an accepted head; a joiner never does.
  bool accept_pristine_genesis_authority = true;
  uint64_t materialization_cache_limit_bytes = 128ULL * 1024 * 1024;
  // A policy commit is the owner's call, carrying no dot (R2). Told each head a commit
  // or a read is about to stand on: its state with the policy changed and its merge
  // parents cleared, or nothing. May throw, ending what asked. None: never.
  std::function<std::optional<State>(const State&)> reconfigure;
  // Told of every head as it becomes the one reads are at.
  std::function<void(const State&)> on_head;
  // Generation 2 from nothing, on the deterministic founder: the state the owner was
  // configured with, its tree written through `nodes`. A function of that alone, so
  // founders configured alike build one commit. None: this node forms no space.
  std::function<State(TreeNodeStore& nodes)> founding;
  // This space's messages carry no space id, and an unaddressed message is this space's:
  // the one space an owner's peers already speak to. At most one on a node.
  bool unaddressed = false;
  // The adverts of the generation and the write copies, under names of the owner's. Empty:
  // Dagda's own, named by the space's id.
  std::string generation_advert;
  std::string write_copies_advert;
};

// What a space runs on. The node fills this; a test may hold any part.
struct SpaceServices {
  NodeId self{};
  std::function<std::unique_ptr<TreeNodeStore>()> commit_nodes;
  std::function<std::unique_ptr<TreeNodeStore>()> read_nodes;
  ClaimBarrier* claims = nullptr;
  const dannan::contract::WallClock* wall = &dannan::contract::system_wall_clock();
  dannan::sched::WaitGuard* guard = nullptr;
  // None: nothing kept in the cache.
  CacheSeed* seed = nullptr;
  SpaceId space{};
  // Moves when the membership does. A head set aside is retried when it has (R6, R8).
  std::function<uint64_t()> membership_stamp;
  // What a read or a mutation with no usable head stands on: the exchange's survey, its
  // import, its founding and its fold of what they find. None: this node's own head.
  std::function<Record()> converge;
  // A commit was accepted here, and whether the head set and the committed record moved,
  // for the exchange's adverts and notices. HeadsNotReady from here is an acceptance
  // interrupted, which the mutation path retries with the same dot; nothing at Macha's
  // 9a08846 raises one on this path (the plan's D13).
  std::function<void(const Acceptance&, bool heads_changed, const Record& committed)> accepted;
  // A commit is owed; the replicator wakes.
  std::function<void()> owed_changed;
  // A policy commit reaches the active set through this (R2). None: this node alone.
  std::function<void(const Record&)> publish_policy;
  // How long a decoded view is reused with nothing heard, on the replica's clock (H31),
  // and what the peers are known to hold. None of either: only this node's commits make
  // a view stale.
  std::optional<std::chrono::milliseconds> view_ttl;
  // What that is measured by; none: the replica's clock.
  const dannan::contract::TimeSource* view_time = nullptr;
  std::function<RemoteKnown()> remote;
  // Why this node may not write the space now, as every replica that counts checks its
  // writer before storing (H29). None: it may.
  std::function<std::optional<std::string>()> scope_refusal;
};

// One head space on this node: Macha's MetadataManager at 9a08846 as far as one node
// goes, over an owner's Codec, with no owner's type here. Everything to do with a peer is
// the Exchange's, through SpaceServices, the fold of several heads among it. Macha has one
// every-node space.
//
// Every operation is thread-safe. Lock order: _mutation_mutex, then the replica's,
// then _view_mutex or _owed_mutex. A mutation holds _mutation_mutex across its local
// commit: its device writes, its claims and any fetch a tree's read makes.
class Space {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::thread_safe;

  // Replays locally and fetches a tree node it lacks. Throws, naming the file, when
  // the primary state fails and there is no valid seed.
  static constexpr dannan::sched::Waits open_waits = Replica::open_waits;
  // The newest decoded state this node holds, or nothing before the first. Never reads.
  static constexpr dannan::sched::Waits current_waits = dannan::sched::Waits::none;
  // This node's own head: every commit it has made or accepted, with no peer asked.
  // Reads the state device once after the head set moves.
  static constexpr dannan::sched::Waits local_waits = dannan::sched::local_waits;
  // The state at the newest generation known: the cache when current, else as
  // `converge` says. That can be a merge, and a merge's claims check DATA presence.
  static constexpr dannan::sched::Waits converged_waits = dannan::sched::local_waits | dannan::sched::Waits::data_device | dannan::sched::Waits::network;
  // A commit on this node. Claims check DATA presence and may ask a peer.
  static constexpr dannan::sched::Waits mutate_waits = dannan::sched::local_waits | dannan::sched::Waits::data_device | dannan::sched::Waits::network;
  // A page of a view's tree. Nodes this node lacks are fetched from peers.
  static constexpr dannan::sched::Waits entries_waits = dannan::sched::local_waits | dannan::sched::Waits::network;
  static constexpr dannan::sched::Waits heads_waits = dannan::sched::Waits::locks;
  static constexpr dannan::sched::Waits generation_waits = dannan::sched::Waits::none;

  // A claim barrier's refusal, in Macha's words: it names the floor that failed.
  static constexpr std::string_view data_unheld_refusal = "DATA object is held by no node present before metadata publication";
  static constexpr std::string_view control_unclaimed_refusal = "CONTROL retention claim could not be recorded on this node";
  static constexpr uint64_t owed_claim_bytes_max = 32ULL * 1024 * 1024;
  static constexpr size_t default_retries = 8;

  // Throws what Replica throws, and std::invalid_argument for what `spec` cannot be.
  Space(SpaceSpec spec, ReplicaOptions replica, SpaceServices services);
  Space(const Space&) = delete;
  Space& operator=(const Space&) = delete;

  const std::string& name() const noexcept { return _spec.name; }
  const Policy& policy() const noexcept { return _spec.policy; }
  // What the exchange folds several heads with. None: they stand until a commit's clock
  // covers them, and a read falls back to this node's own.
  const std::shared_ptr<const Merge>& merge() const noexcept { return _spec.merge; }
  const std::shared_ptr<const Codec>& codec() const noexcept { return _spec.codec; }
  // The node's replica of this space, for its exchange and for tests.
  Replica& replica() noexcept { return *_replica; }
  const SpaceSpec& spec() const noexcept { return _spec; }
  // Takes a certificate for a commit this node stores, moving the seed and telling the
  // exchange what moved: what a peer's acceptance and this node's own go through. False
  // when the replica would not hold it. Waits as Replica::accept_commit does.
  bool accept(const Acceptance& acceptance);

  std::optional<SpaceView> current() const DANNAN_EXCLUDES(_view_mutex);
  std::optional<SpaceView> current(const dannan::sched::WorkContext& work) const DANNAN_EXCLUDES(_view_mutex);
  SpaceView local() DANNAN_EXCLUDES(_view_mutex);
  SpaceView local(const dannan::sched::WorkContext& work) DANNAN_EXCLUDES(_view_mutex);
  SpaceView converged() DANNAN_EXCLUDES(_view_mutex);
  SpaceView converged(const dannan::sched::WorkContext& work) DANNAN_EXCLUDES(_view_mutex);
  // current()'s generation and witness, without taking the view. generation_waits.
  uint64_t current_generation() const noexcept { return _available_generation.load(std::memory_order_acquire); }
  uint64_t current_revision() const noexcept { return _available_revision.load(std::memory_order_acquire); }
  HeadsReport heads() const;
  HeadFields fields(const SpaceView& view) const { return _spec.codec->head_fields(view.state); }

  // The caller supplies the exact change (Mutation::delta), and this is the only way a
  // commit carries one it did not diff. Applied only if the identity, when given, has
  // not yet been reached. Throws HeadsNotReady for a floor that failed or retries spent,
  // std::logic_error on a head with no tree in a space that has one, and
  // std::invalid_argument for an identity of sequence zero. Everything a pause throws
  // goes through.
  CommitOutcome mutate_delta(const MutationFactory& mutation, size_t retries = default_retries, std::optional<ExternalIdentity> identity = {},
                             const TreePause& pause = {}) DANNAN_EXCLUDES(_view_mutex, _owed_mutex);
  CommitOutcome mutate_delta(const dannan::sched::WorkContext& work, const MutationFactory& mutation, size_t retries = default_retries,
                             std::optional<ExternalIdentity> identity = {}, const TreePause& pause = {}) DANNAN_EXCLUDES(_view_mutex, _owed_mutex);
  // The same path, with the delta the codec's encode_delta makes of the two states;
  // none when it can say no such thing, and then the frame is whole.
  CommitOutcome mutate(const MutationFactory& mutation, size_t retries = default_retries, std::optional<ExternalIdentity> identity = {},
                       const TreePause& pause = {}) DANNAN_EXCLUDES(_view_mutex, _owed_mutex);
  CommitOutcome mutate(const dannan::sched::WorkContext& work, const MutationFactory& mutation, size_t retries = default_retries,
                       std::optional<ExternalIdentity> identity = {}, const TreePause& pause = {}) DANNAN_EXCLUDES(_view_mutex, _owed_mutex);

  // Of several heads, the one carrying the most of what this node authored: by its
  // current author id first, then each earlier one, and the lower hash on a tie (R4).
  Record own_head(const std::vector<Record>& heads) const;
  // Sets `head` aside until the membership changes, for the reconciler.
  // False when it is the only head not set aside, as a node always keeps one.
  bool set_aside(const Record& head, std::string_view reason);
  // Accepted heads, without those set aside. Reads can write (R6): a set-aside whose
  // membership has moved is cleared here.
  std::vector<Record> usable_heads() const;

  // Takes what is owed for the replicator to deliver.
  OwedWork take_owed() DANNAN_EXCLUDES(_owed_mutex);
  uint64_t owed_dropped() const noexcept { return _owed_dropped.load(std::memory_order_relaxed); }
  size_t owed_commits() const DANNAN_EXCLUDES(_owed_mutex);
  OwedBacklog owed_backlog() const DANNAN_EXCLUDES(_owed_mutex);
  MutationTiming mutation_timing() const noexcept;
  // Heads set aside as this node last looked. Waits on nothing.
  uint64_t set_aside_count() const noexcept { return _set_aside_count.load(std::memory_order_relaxed); }

  // Walks a view's tree a page at a time, Macha's entries(): a page of `view` after
  // `from`, in key order, one budget operation per entry. A view with no tree has
  // nothing in it.
  template <class Value>
  dannan::sched::Page<typename Tree<Value>::Item, std::string> entries(const Tree<Value>& tree, const SpaceView& view, dannan::sched::Cursor<std::string> from,
                                                                       dannan::sched::Budget& budget) const {
    const auto root = _spec.codec->head_fields(view.state).root;
    if (!root) {
      dannan::sched::Page<typename Tree<Value>::Item, std::string> page;
      page.stopped = dannan::sched::Stop::end;
      return page;
    }
    const auto reader = _services.read_nodes();
    return tree.entries(*root, reader.get(), from, budget, *_time);
  }
  template <class Value>
  dannan::sched::Page<typename Tree<Value>::Item, std::string> entries(const dannan::sched::WorkContext& work, const Tree<Value>& tree, const SpaceView& view,
                                                                       dannan::sched::Cursor<std::string> from, dannan::sched::Budget& budget) const {
    enter(work, entries_waits, "heads::Space::entries");
    return entries(tree, view, from, budget);
  }

  // The change notice (D4). `notice` is posted to `executor`, never called inline, each
  // time this node's committed head moves: a commit here, or one taken from a peer. The
  // notices are posted in the order of the moves, each one's `before` the last one's
  // `after`; a watcher on an executor of several threads orders them by generation. The
  // executor must outlive the watch. Waits on nothing. Returns the token unwatch() takes.
  uint64_t watch(boost::asio::any_io_executor executor, std::function<void(HeadMoved)> notice) DANNAN_EXCLUDES(_watch_mutex);
  // After it returns nothing more is posted for the token; a notice already posted still
  // runs. An unknown token is nothing to withdraw.
  void unwatch(uint64_t token) DANNAN_EXCLUDES(_watch_mutex);
  // A node-local space's upkeep (D3), what the exchange's truncation and the store's
  // collector do for a shared one. Under the mutation lock, so no commit writes a node
  // meanwhile: history is re-rooted at the one head once it stands, and then, when
  // history holds nothing but the heads, `collect` is handed every tree node they reach,
  // which is all the space still needs. Waits on the state device.
  struct Upkeep {
    bool truncated = false;
    bool collected = false;
    uint64_t reached = 0;
  };
  Upkeep upkeep_node_local(const std::function<void(const std::set<ObjectId>&)>& collect) DANNAN_EXCLUDES(_view_mutex, _owed_mutex);

  // Tells the watches if the committed head moved since they were last told. accept()
  // calls it; so does any path that moves the head without it (an imported history that
  // prunes an accepted head). Waits on nothing but the replica's memory.
  void notice_if_moved() DANNAN_EXCLUDES(_watch_mutex);

  // The keys whose entries differ between two heads this node holds, in key order, one
  // budget operation per key, after `from`. Only where the two trees differ is read
  // (Tree::diff), so the cost follows the change, not the tree; each page reads that
  // difference again. Keys present on one side only are named; a head with no tree has
  // no keys. Throws HeadsNotReady for a head this node does not hold. entries_waits.
  template <class Value>
  dannan::sched::Page<std::string, std::string> changed(const Tree<Value>& tree, const HeadIdentity& before, const HeadIdentity& after,
                                                        dannan::sched::Cursor<std::string> from, dannan::sched::Budget& budget) const {
    dannan::sched::Page<std::string, std::string> page;
    page.next = from;
    std::vector<std::string> keys;
    if (before != after) {
      const auto before_root = root_of(before);
      const auto after_root = root_of(after);
      const auto reader = _services.read_nodes();
      if (before_root && after_root) {
        for (auto& [key, _] : tree.diff(*before_root, *after_root, *reader)) keys.push_back(key);
      } else if (before_root || after_root) {
        tree.walk(before_root ? *before_root : *after_root, *reader, [&](const std::string& key, const Value&) { keys.push_back(key); });
      }
    }
    for (auto& key : keys) {
      if (from.after && key <= *from.after) continue;
      if (const auto stop = budget.must_stop(_time->now())) {
        page.stopped = *stop;
        return page;
      }
      if (!budget.take_operation()) {
        page.stopped = dannan::sched::Stop::budget;
        return page;
      }
      page.next.after = key;
      page.items.push_back(std::move(key));
    }
    page.next = {};
    page.stopped = dannan::sched::Stop::end;
    return page;
  }

 private:
  friend class Exchange;
  struct Watch {
    boost::asio::any_io_executor executor;
    std::function<void(HeadMoved)> notice;
  };
  // A held head's tree root; none for a head with no tree. Throws HeadsNotReady for a
  // head this node does not hold.
  std::optional<ObjectId> root_of(const HeadIdentity& head) const;
  Record read_base();
  Record read_uncached();
  Record read_record();
  Record maybe_reconfigure(const Record& initial);
  CommitOutcome commit_path(const MutationFactory& make, size_t retries, const std::optional<ExternalIdentity>& identity, const TreePause& pause,
                            bool supplied);
  CommitClaims claim(const Dot& dot, const HeadFields& before, const HeadFields& after, const Introduced& introduced, const TreeNodeStore& reader,
                     const TreePause& pause);
  void accept_locally(const Record& record, std::span<const uint8_t> delta, HistoryEntry::Body* body);
  void owe(Record head, std::optional<OwedClaims> claims) DANNAN_EXCLUDES(_owed_mutex);
  // The state of a record this node holds, shared where it can be. Throws when the
  // record decodes to nothing.
  State state_of(const Record& record) const;
  // Reads move forward only: the view never goes back to an older record.
  Record cache_record(const Record& record, State state) DANNAN_EXCLUDES(_view_mutex);
  std::optional<Record> cached_record() const DANNAN_EXCLUDES(_view_mutex);
  std::optional<SpaceView> cached_view() const DANNAN_EXCLUDES(_view_mutex);
  // A cached record at `generation` still stands: its lifetime, the peers' epoch, and
  // nothing newer known here or there.
  bool view_current_locked(uint64_t generation) const DANNAN_REQUIRES(_view_mutex);
  void view_taken_locked() DANNAN_REQUIRES(_view_mutex);
  std::unique_ptr<TreeNodeStore> commit_nodes() const { return _services.commit_nodes(); }
  void enter(const dannan::sched::WorkContext& work, dannan::sched::Waits declared, std::string_view operation) const;

  const SpaceSpec _spec;
  const SpaceServices _services;
  const dannan::contract::TimeSource* const _time;
  CostCounters* const _costs;
  const dannan::status::ScopedLogger _logger;
  std::unique_ptr<Replica> _replica;

  // Guards no state: serialises mutations, as Macha's does, across each local commit.
  dannan::contract::IoMutex _mutation_mutex;
  // The membership heads were set aside under.
  mutable std::atomic_uint64_t _set_aside_stamp{0};
  mutable std::atomic_uint64_t _set_aside_count{0};

  // The newest view; nothing is read holding it.
  mutable dannan::contract::Mutex _view_mutex;
  std::optional<Record> _view_record DANNAN_GUARDED_BY(_view_mutex);
  State _view_state DANNAN_GUARDED_BY(_view_mutex);
  uint64_t _view_generation DANNAN_GUARDED_BY(_view_mutex) = 0;
  uint64_t _view_revision DANNAN_GUARDED_BY(_view_mutex) = 0;
  Hash256 _view_hash DANNAN_GUARDED_BY(_view_mutex){};
  // local()'s answer, and the head-set revision it was taken at.
  std::optional<SpaceView> _local_view DANNAN_GUARDED_BY(_view_mutex);
  uint64_t _local_view_revision DANNAN_GUARDED_BY(_view_mutex) = 0;
  dannan::contract::TimePoint _view_until DANNAN_GUARDED_BY(_view_mutex){};
  uint64_t _view_remote_epoch DANNAN_GUARDED_BY(_view_mutex) = 0;
  std::atomic_uint64_t _available_generation{0};
  std::atomic_uint64_t _available_revision{0};

  // The watches (D4) and the head they were last told of, none while nobody watches.
  // Taken before the replica's own locks, and never held while anything is posted.
  mutable dannan::contract::Mutex _watch_mutex;
  std::map<uint64_t, Watch> _watches DANNAN_GUARDED_BY(_watch_mutex);
  uint64_t _next_watch DANNAN_GUARDED_BY(_watch_mutex) = 1;
  std::optional<HeadIdentity> _told DANNAN_GUARDED_BY(_watch_mutex);

  // What is owed the peers. Nothing is sent holding it.
  mutable dannan::contract::Mutex _owed_mutex;
  std::deque<OwedClaims> _owed_claims DANNAN_GUARDED_BY(_owed_mutex);
  uint64_t _owed_claim_bytes DANNAN_GUARDED_BY(_owed_mutex) = 0;
  std::optional<Record> _owed_head DANNAN_GUARDED_BY(_owed_mutex);
  std::atomic_uint64_t _owed_dropped{0};

  std::atomic_uint64_t _mutations{0};
  std::atomic_uint64_t _retention_ms_total{0};
  std::atomic_uint64_t _retention_ms_max{0};
  std::atomic_uint64_t _publish_ms_total{0};
  std::atomic_uint64_t _publish_ms_max{0};
};

}  // namespace dagda::heads
