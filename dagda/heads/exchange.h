//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "dagda/heads/convergence_demand.h"
#include "dagda/heads/messages.h"
#include "dagda/heads/space.h"
#include "dannan/context.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/session.h"
#include "dannan/sched/work.h"
#include "dannan/status/log.h"

namespace dagda::heads {

// What a node's head spaces exchange under: Macha's config at 9a08846 by default, the
// names beside each its keys, so a facade fills them (second cutover 8c.2, 8c.3).
struct ExchangeOptions {
  message::Numbers numbers;
  // A pristine node with bootstrap peers configured: it never holds genesis as authority
  // and never forms a space alone. A founder: none configured. Macha's `bootstrap`.
  bool joiner = false;
  // dht.metadata_write_copies: what a merge, a founding and a policy commit seek, never
  // a mutation (R7). Advertised, and a peer's change of it is a membership event (H28).
  size_t write_copies = 2;
  // A peer that made no progress on a commit call for this long is not asked again for
  // `dead_after`: by then the roster has dropped it or it is answering.
  std::chrono::milliseconds write_stall{2500};
  std::chrono::milliseconds dead_after{30000};
  // What bounds any other call to a peer: a metadata call in Macha's port (its
  // call_deadline.hpp, Tom 2026-11-11). One it waits on synchronously ends sooner when
  // it moves no byte for call_no_progress (control_no_progress_deadline); a commit
  // call is watched by write_stall instead, and an asynchronous one by nothing else.
  std::chrono::milliseconds call_deadline{std::chrono::minutes(5)};
  std::chrono::milliseconds call_no_progress{30000};
  // dht.metadata_cache: a decoded view is reused this long with no notice (H31).
  std::chrono::milliseconds view_ttl{250};
  // How often the generation is advertised, beside each acceptance: Macha's heartbeat.
  std::chrono::milliseconds advert_interval{5000};
  // maintenance.convergence_settle and .convergence_max_delay: after a failed run, when
  // a burst of events has settled, or at the latest, the run is retried.
  std::chrono::milliseconds convergence_settle{1000};
  std::chrono::milliseconds convergence_max_delay{10000};
  // The wait after a failed run, doubling to the most: maintenance_background_interval
  // and no_progress_backoff.
  std::chrono::milliseconds retry_backoff{30000};
  std::chrono::milliseconds retry_backoff_most{300000};
  // History truncation's thresholds. No key on either side.
  size_t truncate_records = 256;
  uint64_t truncate_bytes = 64ULL * 1024 * 1024;
  // Flagged heads one pass of head repair takes.
  size_t repair_heads_per_pass = 64;
  // What the decoded view's lifetime is measured by; none: the replica's clock.
  const dannan::contract::TimeSource* view_time = nullptr;
  // A commit's claims; none: the store's, as a commit brings them in.
  ClaimBarrier* claims = nullptr;
  // Told each stage a metadata-repair run enters, on the thread running it, and allowed
  // to throw: Macha's maintenance_stage_hook, "metadata-repair-begin" among them.
  std::function<void(std::string_view)> stage;
};

// `replicas`, local first, then by measured latency, then the rest in their given order:
// the order a publish asks them in (H4).
std::vector<dannan::membership::NodeRecord> order_commit_replicas(std::vector<dannan::membership::NodeRecord> replicas, const NodeId& local,
                                                                  const std::function<std::optional<std::chrono::milliseconds>(const NodeId&)>& latency);

// Whether a space's head may be written here now. Every known node is capable, and any
// node may accept a mutation alone.
enum class Availability : uint8_t { unavailable, read_only, writable };
const char* availability_name(Availability availability) noexcept;

// Macha's MetadataClusterStatus: what Status shows. Refreshed by the repair owner.
struct ExchangeStatus {
  uint64_t generation = 0;
  uint64_t observed_unix_ms = 0;
  uint32_t replicas = 0;
  uint32_t replicas_online = 0;
  uint32_t write_replicas_required = 0;
  Availability availability = Availability::unavailable;
  bool stable = false;
  bool write_available = false;
  uint64_t head_holders = 0;
  uint64_t head_present = 0;
};

struct TransferDiagnostics {
  uint64_t transfers = 0;
  uint64_t entries_submitted = 0;
  uint64_t peak_in_flight = 0;
};

struct ReconcileDiagnostics {
  uint64_t merges = 0;
  uint64_t conflicts_created = 0;
  uint64_t conflicts_superseded = 0;
  uint64_t set_aside = 0;
};

// One peer's refusal, as it said it: the scope check's words and generation.
struct PeerRefusal {
  NodeId peer{};
  std::string why;
};

// A commit as a publish left it: who holds it, who accepted it, who refused and why.
struct PublishedCommit {
  Record record;
  Acceptance acceptance;
  std::vector<NodeId> stored_on;
  std::vector<NodeId> accepted_on;
  std::vector<PeerRefusal> refused;
};

// What one turn of metadata repair did.
struct RepairTurn {
  // Demand stands, or stands again.
  bool dirty = false;
  // It got as far as a run.
  bool ran = false;
  // The run did not complete.
  bool failed = false;
  // A virgin follower or a settling founder, waiting: the node goes on with what does
  // not depend on the head.
  bool waited = false;
  // The run failed with recovery required or no head yet: nothing else in the pass may
  // proceed (maintenance.cpp:514-517). Macha rethrows; this is that, as evidence.
  bool hold_pass = false;
  std::string why;
};

// One space's exchange with its peers: Macha's MetadataManager at 9a08846 from here out
// (import, push, publish, survey, founding, the fold of several heads, repair, the
// replicator) and MetadataServer (what a peer's request does, the adverts and notices), and
// the metadata half of its maintenance loop. Over an owner's Codec and Merge and with no
// owner's type.
//
// Thread-safe, except that repair_turn() has one owner, the node's maintenance thread.
// Lock order: Space's _mutation_mutex, then _reconciliation_mutex, then a replica's, then
// _owed_mutex, _stalled_mutex or _unacceptable_mutex, each a leaf. No lock here is held
// across a call to a peer but the mutation mutex, which a mutation's reconfigure holds across
// a publish, as Macha's does (the plan's D6), and the reconciliation's, across a merge's.
class Exchange {
 public:
  static constexpr dannan::sched::ThreadSafety safety = dannan::sched::ThreadSafety::thread_safe;
  // The survey, the import, the publish and the repair all wait on each peer in turn,
  // each call within its deadline.
  // A merge among it: its claims check DATA presence, and it waits for another reconciler.
  static constexpr dannan::sched::Waits repair_waits = dannan::sched::local_waits | dannan::sched::Waits::data_device | dannan::sched::Waits::network;
  static constexpr dannan::sched::Waits head_repair_waits = dannan::sched::local_waits | dannan::sched::Waits::network;
  static constexpr dannan::sched::Waits truncate_waits = dannan::sched::local_waits;
  // A wait of the caller's own, on the replicator.
  static constexpr dannan::sched::Waits wait_replicated_waits = dannan::sched::Waits::locks;
  static constexpr dannan::sched::Waits status_waits = dannan::sched::Waits::none;

  struct Services {
    dannan::Context& context;
    ExchangeOptions options;
    SpaceId space{};
    std::string name;
    // This space's messages carry no space id: the one an owner's peers expect.
    bool unaddressed = false;
    // The adverts, under their names; none: Dagda's own under the space's id.
    std::string generation_advert;
    std::string write_copies_advert;
    // What the peers' share of a commit's claims goes through.
    ClaimBarrier* claims = nullptr;
    // Dates a set-aside head; its wait is `garbage_grace`.
    const dannan::contract::WallClock* wall = nullptr;
    std::chrono::milliseconds garbage_grace{std::chrono::hours(24 * 30)};
    // What repair's turns are timed by: the node's maintenance clock.
    const dannan::contract::TimeSource* time = nullptr;
    dannan::status::NodeEvents::Kind metadata_event = 0;
    // Told, on any thread and with nothing held, that a turn may be due.
    std::function<void()> wake;
    // The node's check on what a caller's work may wait on.
    dannan::sched::WaitGuard* guard = nullptr;
  };

  explicit Exchange(Services services);
  ~Exchange();
  Exchange(const Exchange&) = delete;
  Exchange& operator=(const Exchange&) = delete;

  // Starts the replicator, the adverts and the roster watch over `space`, which outlives
  // this. Once.
  void attach(Space& space);
  // Ends them. What is owed stays owed: the commits are accepted here and repair offers their
  // heads after a restart. Nothing restores a claim record that did not reach a peer:
  // repair_once offers heads only, and the peer's copies are protected by the head's inventory
  // and the 30-day gates, as in Macha. A call in flight is cancelled. Idempotent.
  void stop() DANNAN_EXCLUDES(_owed_mutex);

  const std::string& name() const noexcept { return _services.name; }
  const SpaceId& space_id() const noexcept { return _services.space; }
  bool unaddressed() const noexcept { return _services.unaddressed; }
  Space& space() noexcept { return *_space; }

  // ---- The seams a Space runs on ------------------------------------------
  // What a read or a mutation with no usable head stands on: Macha's read_record_base.
  Record read_base();
  // A commit accepted here: the adverts, and a notice when the head set moved.
  void accepted(const Acceptance& acceptance, bool heads_changed, const Record& committed);
  // A policy commit, published to the active set on the control frame (R2).
  PublishedCommit publish_to_active(const Record& record);
  RemoteKnown remote_known() const noexcept;
  std::optional<std::string> scope_refusal(const NodeId& writer) const;

  // ---- A peer's request ----------------------------------------------------
  // A notice of a head set moved: an equal generation may carry a sibling, and only one
  // made stale by a strictly newer generation is ignored.
  void note_notice(uint64_t generation);

  // ---- The replica's answers, remote's and known generations ---------------
  uint64_t remote_generation() const noexcept;
  uint64_t known_generation() const;
  uint64_t remote_epoch() const noexcept { return _remote_epoch.load(std::memory_order_acquire); }
  uint64_t announcements() const noexcept { return _announcements.load(std::memory_order_relaxed); }
  // The heads, as the exchange's own survey asks of this node.
  std::vector<Acceptance> certificates() const;

  // ---- Publish -------------------------------------------------------------
  // Stores `record` on `nodes`, nearest first, until the write copies hold it, this node
  // being the first and required, and accepts it on each, its ancestry pushed first. A
  // peer's failure is recorded and does not fail it. Throws when this node cannot.
  PublishedCommit publish_commit(const std::vector<dannan::membership::NodeRecord>& nodes, const Record& record, std::span<const uint8_t> delta,
                                 dannan::sched::FrameType frame, const TreePause& pause = {});

  // ---- Repair --------------------------------------------------------------
  // Macha's repair_once: selects a head under the space's mutation lock, then offers it
  // to every node present without. Throws HeadsNotReady when it cannot, and what `pause`
  // throws.
  void repair_once(const TreePause& pause = {});
  void repair_once(const dannan::sched::WorkContext& work, const TreePause& pause = {});
  // Each head flagged unreconstructable, fetched whole from a peer and re-anchored. How
  // many were. A peer's failure moves on to the next.
  size_t repair_unreconstructable_heads(dannan::sched::FrameType frame, const TreePause& pause = {});
  size_t repair_unreconstructable_heads(const dannan::sched::WorkContext& work, dannan::sched::FrameType frame, const TreePause& pause = {});
  // Expires what was set aside past the grace, then re-roots history past the thresholds.
  // Under the space's mutation lock, so a commit waits for it.
  void truncate_history(size_t record_threshold, uint64_t byte_threshold);
  void truncate_history();
  void truncate_history(const dannan::sched::WorkContext& work);
  // One turn of the maintenance loop's metadata half, Macha's maintenance.cpp:344-542:
  // events absorbed, then a run if demand stands and it is due. `forced` ignores the
  // settle and the backoff, as run_maintenance_once does. Throws only what a stop throws.
  RepairTurn repair_turn(std::stop_token stop, bool forced = false);
  // When a turn next has anything to do, with demand standing.
  std::optional<dannan::contract::TimePoint> turn_due() const;

  // ---- The replicator ------------------------------------------------------
  // Commits accepted here that the replicator has not yet offered.
  size_t replication_pending() const DANNAN_EXCLUDES(_owed_mutex);
  // False on timeout.
  bool wait_replicated(std::chrono::milliseconds timeout) DANNAN_EXCLUDES(_owed_mutex);
  // The space's seam: a commit is owed.
  void wake_replicator() DANNAN_EXCLUDES(_owed_mutex);

  // ---- What Status shows ---------------------------------------------------
  void note_validation(bool validated, std::string_view reason = {});
  ExchangeStatus status() const noexcept;
  ConvergenceDemandDiagnostics demand() const noexcept { return _demand.diagnostics(); }
  TransferDiagnostics transfers() const noexcept {
    return {_history_transfers.load(std::memory_order_relaxed), _history_entries_submitted.load(std::memory_order_relaxed),
            _history_peak_in_flight.load(std::memory_order_relaxed)};
  }
  // What the fold has done since start.
  ReconcileDiagnostics reconciliations() const noexcept {
    return {_reconciliations.load(std::memory_order_relaxed), _conflicts_created.load(std::memory_order_relaxed),
            _conflicts_superseded.load(std::memory_order_relaxed), _heads_set_aside.load(std::memory_order_relaxed)};
  }
  // The refusals peers gave this node last, one per peer.
  std::vector<PeerRefusal> refusals() const DANNAN_EXCLUDES(_refusals_mutex);
  // Whether `peer` is in its time out from commit calls.
  bool stalled(const NodeId& peer) const DANNAN_EXCLUDES(_stalled_mutex);
  // The turn's demand, for an owner whose own event wants a run.
  void request_demand();

 private:
  struct PendingCall {
    std::shared_ptr<std::promise<dannan::net::CallOutcome>> outcome;
    std::future<dannan::net::CallOutcome> future;
    std::shared_ptr<dannan::net::CallHandle> handle;
  };

  using Peer = dannan::membership::NodeRecord;
  using FrameType = dannan::sched::FrameType;

  NodeId self() const;
  Bytes addressed(Bytes payload) const;
  PendingCall start_call(const Peer& owner, message::MessageType type, Bytes payload, FrameType frame, std::chrono::milliseconds deadline,
                         dannan::net::CallWatch watch = {});
  // The outcome, by the call's deadline; throws std::runtime_error while stopping.
  dannan::net::CallOutcome wait_call(PendingCall& pending);
  dannan::net::CallOutcome call(const Peer& owner, message::MessageType type, Bytes payload, FrameType frame);
  // A reply, or throw why none came.
  dannan::net::Message reply_of(dannan::net::CallOutcome outcome) const;
  // True for a yes. A refusal is noted against the peer; anything else is a no.
  bool bool_of(const Peer& owner, const dannan::net::Message& reply);
  void note_refusal(const Peer& owner, const dannan::net::Message& reply) DANNAN_EXCLUDES(_refusals_mutex);

  bool import_history_from_peer(const Peer& owner, const Hash256& target, FrameType frame, const TreePause& pause);
  bool push_history_to_peer(const Peer& owner, const Hash256& target, FrameType frame, const TreePause& pause);
  HistoryEntry commit_history_entry(const Record& record, std::span<const uint8_t> delta = {}) const;
  std::vector<Hash256> record_parents(const Record& record) const;
  // A peer's commit call, waited for while it makes progress. Throws when the peer is
  // stalled or stalls.
  dannan::net::Message commit_call(const Peer& owner, message::MessageType type, Bytes payload, FrameType frame) DANNAN_EXCLUDES(_stalled_mutex);
  bool store_commit_on(const Peer& owner, const HistoryEntry& compact, const Record& record, FrameType frame, const TreePause& pause);
  bool accept_commit_on(const Peer& owner, const Acceptance& acceptance, FrameType frame);
  std::vector<std::pair<Peer, Acceptance>> discover_accepted_heads(const std::vector<Peer>& nodes, FrameType frame);
  bool replicate_accepted_head(const Peer& owner, const Record& record, const Acceptance& acceptance, FrameType frame, const TreePause& pause);
  // The survey, the import, the fold of what stands (mgr:1127-1315), and the one head left.
  // A head no merge can be made with is set aside, so this never waits for one. Throws
  // HeadsNotReady when no head stands, or several and this space's owner gave no merge.
  Record read_group(const std::vector<Peer>& nodes, FrameType frame, const TreePause& pause = {}) DANNAN_EXCLUDES(_unacceptable_mutex);
  // One merge and its publish: `left` is the lower hash. Throws what sets a head aside, a
  // MergeUnavailable or a DecodeError, and anything else for what ends the read.
  void reconcile(const std::vector<Peer>& nodes, const Record& left, const Record& right, FrameType frame, const TreePause& pause, size_t remaining);
  struct RecoverySurvey {
    bool complete = false;
    bool durable_history = false;
  };
  RecoverySurvey recover_from_committed_checkpoints(const std::vector<Peer>& active);
  Record discover_or_form(const TreePause& pause = {});
  void enter(const dannan::sched::WorkContext& work, dannan::sched::Waits declared, std::string_view operation) const;

  void advertise_generation(uint64_t generation);
  void announce(uint64_t generation);
  void raise_remote(uint64_t generation) noexcept;
  void notify_metadata();
  void observe_roster(const dannan::RosterChange& change) DANNAN_EXCLUDES(_roster_mutex);
  void replication_loop(std::stop_token stop) DANNAN_EXCLUDES(_owed_mutex);
  void advert_loop(std::stop_token stop);
  void absorb_events();
  void note_retry_trigger(dannan::contract::TimePoint now);
  std::optional<dannan::contract::TimePoint> retry_trigger_due() const;
  void enter_stage(std::string_view stage) const;
  void publish_replica_state(bool validated, std::string_view reason);

  const Services _services;
  const ExchangeOptions& _options;
  dannan::Context& _context;
  const dannan::contract::TimeSource& _time;
  const dannan::status::ScopedLogger _logger;
  Space* _space = nullptr;
  std::atomic_bool _stopping{false};

  std::atomic_uint64_t _remote_generation{0};
  std::atomic_uint64_t _remote_epoch{0};
  std::atomic_uint64_t _announcements{0};
  std::atomic_uint64_t _advertised{0};

  mutable dannan::contract::Mutex _stalled_mutex;
  mutable std::map<NodeId, dannan::contract::TimePoint> _stalled_until DANNAN_GUARDED_BY(_stalled_mutex);
  // Peer certificates this replica cannot accept, not tried again for 30 s: read_group
  // runs on nearly every read, and would reject and log them afresh.
  mutable dannan::contract::Mutex _unacceptable_mutex;
  std::map<Hash256, dannan::contract::TimePoint> _unacceptable_until DANNAN_GUARDED_BY(_unacceptable_mutex);
  mutable dannan::contract::Mutex _refusals_mutex;
  std::map<NodeId, std::string> _refusals DANNAN_GUARDED_BY(_refusals_mutex);

  // Each peer's write-copies advert as last seen; nothing runs under it.
  dannan::contract::Mutex _roster_mutex;
  std::map<NodeId, std::optional<uint64_t>> _peer_write_copies DANNAN_GUARDED_BY(_roster_mutex);
  uint64_t _roster_watch = 0;
  // The watch's hold on this object, so a change posted as it stops finds nothing.
  struct RosterHold {
    dannan::contract::Mutex mutex;
    Exchange* exchange DANNAN_GUARDED_BY(mutex) = nullptr;
  };
  std::shared_ptr<RosterHold> _roster_hold;

  // Guards no state: serialises merge and publish, so readers who all see one divergence do
  // not each mint a commit. Held across the merge's tree reads, claims and publish, and taken
  // only on seeing several heads. After Space's _mutation_mutex, which a repair run holds
  // here and a read does not.
  dannan::contract::IoMutex _reconciliation_mutex;
  std::atomic_uint64_t _reconciliations{0};
  std::atomic_uint64_t _conflicts_created{0};
  std::atomic_uint64_t _conflicts_superseded{0};
  std::atomic_uint64_t _heads_set_aside{0};

  // What the replicator is doing, beside the space's own queue.
  mutable dannan::contract::Mutex _owed_mutex;
  std::condition_variable_any _owed_changed;
  bool _replicating DANNAN_GUARDED_BY(_owed_mutex) = false;

  std::atomic_uint64_t _history_transfers{0};
  std::atomic_uint64_t _history_entries_submitted{0};
  std::atomic_uint64_t _history_peak_in_flight{0};

  std::atomic_uint64_t _status_generation{0};
  std::atomic_uint64_t _status_observed_unix_ms{0};
  std::atomic_uint32_t _status_replicas{0};
  std::atomic_uint32_t _status_replicas_online{0};
  std::atomic_uint32_t _status_write_required{0};
  std::atomic<Availability> _status_availability{Availability::unavailable};
  std::atomic_bool _status_stable{false};
  std::atomic_bool _status_write_available{false};
  std::atomic_uint64_t _head_holders{0};
  std::atomic_uint64_t _head_present{0};

  // The turn's: repair_turn() alone reads and writes these.
  ConvergenceDemand _demand;
  uint64_t _absorbed_metadata = 0;
  uint64_t _absorbed_topology = 0;
  uint64_t _last_remote_epoch = 0;
  uint64_t _last_local_generation = 0;
  std::vector<NodeId> _last_active;
  dannan::contract::TimePoint _retry_due{};
  dannan::contract::TimePoint _formation_settle_due{};
  std::chrono::milliseconds _retry_backoff{0};
  std::optional<dannan::contract::TimePoint> _retry_first;
  dannan::contract::TimePoint _retry_last{};
  bool _failed = false;

  std::jthread _replicator;
  std::jthread _adverts;
  std::condition_variable_any _adverts_wake;
  std::mutex _adverts_wait;
};

}  // namespace dagda::heads
