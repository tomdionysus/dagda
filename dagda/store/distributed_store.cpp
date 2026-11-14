//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/distributed_store.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <cstring>
#include <future>
#include <limits>
#include <tuple>
#include <utility>

#include "dagda/store/messages.h"
#include "dagda/store/placement.h"
#include "dannan/net/outbound.h"
#include "dannan/placement/placement.h"

namespace dagda::store {
namespace {

using Clock = std::chrono::steady_clock;

// A write's or a read's deadline is on the node's clock, which nothing notifies
// when it moves other than with the steady clock: a wait looks again this often.
constexpr std::chrono::milliseconds look_at_the_clock{25};

// Dannan's call takes a deadline. A peer's put or fetch is bounded by its no-progress
// watch instead, as Macha's has no absolute limit: this is past any transfer.
using dannan::contract::Lock;
using dannan::contract::ReadLock;
using dannan::contract::WriteLock;
using dannan::identity::NodeId;
using dannan::membership::NodeRecord;
using dannan::net::CallOutcome;
using dannan::sched::FrameType;

// A scope refusal as the evidence carries it: the status and code every
// replica answers with, the source's reason, and the generation of the
// scope data it read.
std::string scope_refusal_text(const dannan::contract::ScopeDecision& decision) {
  return "403 scope_refused: " + decision.why + " (scope generation " + std::to_string(decision.generation) + ")";
}

// Why a call came to nothing: the peer's refusal, or why no reply came.
std::string outcome_text(const CallOutcome& outcome) {
  if (!outcome.reply) return outcome.failure.empty() ? "no reply" : outcome.failure;
  if (const auto refusal = dannan::net::refusal_of(*outcome.reply))
    return std::to_string(refusal->status) + " " + refusal->code + (refusal->message.empty() ? "" : ": " + refusal->message);
  return "reply type " + std::to_string(static_cast<uint16_t>(outcome.reply->type));
}

// A refusal that stands: the node answered and asking again will not change
// it now. Anything else (no reply, a busy peer) is a call that came to
// nothing this time.
bool refused(const CallOutcome& outcome) {
  if (!outcome.reply) return false;
  const auto refusal = dannan::net::refusal_of(*outcome.reply);
  return refusal && !dannan::net::try_later(*refusal);
}

bool refused_with(const CallOutcome& outcome, uint16_t status) {
  if (!outcome.reply) return false;
  const auto refusal = dannan::net::refusal_of(*outcome.reply);
  return refusal && refusal->status == status;
}

// The object in a get_object reply: checked against the id and its own
// hash, then slid over its prefix to keep the allocation and the frame's
// retained memory.
ObjectData take_object_reply(dannan::net::Message message, const ObjectId& expected) {
  if (message.type != message::object_reply) throw std::runtime_error("remote object reply has the wrong message type");
  dannan::net::Reader reader(message.payload);
  const ObjectId returned{reader.fixed<32>()};
  const auto size = reader.u32();
  if (size > message::max_object_bytes || reader.remaining() != size) throw dannan::net::DecodeError("invalid remote object reply size");
  constexpr size_t prefix = 32 + 4;
  const auto data = std::span<const uint8_t>(message.payload).subspan(prefix, size);
  if (returned != expected || object_id(data) != expected) throw std::runtime_error("remote integrity failure");
  if (size) std::memmove(message.payload.data(), message.payload.data() + prefix, size);
  message.payload.resize(size);
  auto object = std::make_shared<ObjectBuffer>();
  object->bytes = std::move(message.payload);
  object->memory = std::move(message.memory);
  return object;
}

ObjectData buffer_of(Bytes bytes) {
  auto object = std::make_shared<ObjectBuffer>();
  object->bytes = std::move(bytes);
  return object;
}

// Calls `requests` concurrently, at most `window` at once, each within
// `deadline` and under `watch`; the outcomes in order. Waits on the peers.
struct Request {
  NodeRecord peer;
  dannan::net::MessageType type{};
  Bytes payload;
  FrameType frame = FrameType::control;
};

std::vector<CallOutcome> call_all(dannan::Routes& routes, const boost::asio::any_io_executor& executor, std::vector<Request> requests, size_t window,
                                  std::chrono::milliseconds deadline, dannan::net::CallWatch watch = {}) {
  std::vector<CallOutcome> out(requests.size());
  std::vector<std::future<CallOutcome>> in_flight;
  size_t first = 0;
  const auto drain = [&](size_t until) {
    for (; first < until; ++first) {
      auto& future = in_flight[first];
      // Dannan answers by the deadline; the margin covers only the executor.
      if (future.wait_for(deadline + std::chrono::seconds(5)) == std::future_status::ready)
        out[first] = future.get();
      else
        out[first].failure = "no answer";
    }
  };
  for (size_t i = 0; i < requests.size(); ++i) {
    if (i >= first + std::max<size_t>(1, window)) drain(i + 1 - std::max<size_t>(1, window));
    auto promise = std::make_shared<std::promise<CallOutcome>>();
    in_flight.push_back(promise->get_future());
    routes.call(
        requests[i].peer, requests[i].type, std::move(requests[i].payload), requests[i].frame, deadline, executor,
        [promise](CallOutcome outcome) { promise->set_value(std::move(outcome)); }, watch);
  }
  drain(requests.size());
  return out;
}

}  // namespace

const char* durability_name(Durability level) noexcept {
  switch (level) {
    case Durability::ephemeral:
      return "ephemeral";
    case Durability::persisted:
      return "persisted";
    case Durability::replicated:
      return "replicated";
    case Durability::durable:
      return "durable";
  }
  return "unknown";
}

const char* replica_state_name(ReplicaState state) noexcept {
  switch (state) {
    case ReplicaState::acknowledged:
      return "acknowledged";
    case ReplicaState::pending:
      return "pending";
    case ReplicaState::refused:
      return "refused";
  }
  return "unknown";
}

size_t WriteEvidence::acknowledged() const noexcept {
  return static_cast<size_t>(std::count_if(replicas.begin(), replicas.end(), [](const ReplicaEvidence& r) { return r.state == ReplicaState::acknowledged; }));
}

size_t WriteEvidence::holders() const noexcept {
  return static_cast<size_t>(std::count_if(replicas.begin(), replicas.end(), [](const ReplicaEvidence& r) {
    return r.state == ReplicaState::acknowledged || (r.state == ReplicaState::pending && r.durability);
  }));
}

const ReplicaEvidence* WriteEvidence::on(const NodeId& node) const noexcept {
  for (const auto& replica : replicas)
    if (replica.node == node) return &replica;
  return nullptr;
}

// What completions, timers and roster changes reach: the store while it
// lives, and the tickets.
struct DistributedStore::Shared {
  dannan::contract::SharedMutex alive;
  DistributedStore* store DANNAN_GUARDED_BY(alive) = nullptr;
  dannan::contract::Mutex mutex;
  uint64_t next_serial DANNAN_GUARDED_BY(mutex) = 1;
  std::map<uint64_t, std::shared_ptr<TicketRecord>> tickets DANNAN_GUARDED_BY(mutex);
  // The size at which the next walk for settled tickets runs (new_ticket).
  size_t ticket_walk_at DANNAN_GUARDED_BY(mutex) = 0;
};

namespace {

// What a pending replica still waits on: nothing the write is doing (owed to
// prompt replication or repair), one of its calls, a barrier, a dead node's
// return, or the ephemeral queue. Only the last four keep a ticket open.
enum class Awaiting : uint8_t { none, call, barrier, dead, queued };

struct ReplicaRecord {
  ReplicaEvidence evidence;
  // A member the level waits for (durable: the live members).
  bool waited = false;
  Awaiting awaiting = Awaiting::none;
  // A barrier or a catch-up of it is under way.
  bool running = false;
};

// A local copy left undone on purpose, already logged.
struct Skipped {};

struct Waiter {
  boost::asio::any_io_executor executor;
  DistributedStore::WriteDone done;
  // Told when no member is pending at all, not when the ticket settles.
  bool complete = false;
  std::shared_ptr<boost::asio::steady_timer> timer;
  std::atomic<bool> fired{false};
};

// Posts the evidence to the waiter once, never inline.
void tell(const std::shared_ptr<Waiter>& waiter, const WriteEvidence& evidence) {
  if (waiter->fired.exchange(true)) return;
  if (waiter->timer) boost::asio::post(waiter->timer->get_executor(), [timer = waiter->timer] { timer->cancel(); });
  boost::asio::post(waiter->executor, [done = waiter->done, evidence] { done(evidence); });
}

}  // namespace

struct DistributedStore::TicketRecord {
  TicketRecord(uint64_t serial_, const ObjectId& id_, Durability level_, bool deferred_) : serial(serial_), id(id_), level(level_), deferred(deferred_) {}

  const uint64_t serial;
  const ObjectId id;
  const Durability level;
  const bool deferred;
  mutable dannan::contract::Mutex mutex;
  size_t sought DANNAN_GUARDED_BY(mutex) = 0;
  bool cancelled DANNAN_GUARDED_BY(mutex) = false;
  // A caller has asked after it (a wait armed or a status read): a settled deferred
  // ticket may be forgotten only then, as Macha's batch kept each extent until its
  // publisher consumed it.
  mutable bool asked DANNAN_GUARDED_BY(mutex) = false;
  std::vector<ReplicaRecord> replicas DANNAN_GUARDED_BY(mutex);
  std::vector<std::shared_ptr<Waiter>> waiters DANNAN_GUARDED_BY(mutex);

  WriteEvidence evidence_locked() const DANNAN_REQUIRES(mutex) {
    WriteEvidence out;
    out.id = id;
    out.level = level;
    out.deferred = deferred;
    out.sought = sought;
    out.cancelled = cancelled;
    out.ticket = Ticket{serial};
    bool awaited = false;
    bool all_waited = true;
    for (const auto& replica : replicas) {
      out.replicas.push_back(replica.evidence);
      if (replica.evidence.state == ReplicaState::pending && replica.awaiting != Awaiting::none) awaited = true;
      if (replica.waited && replica.evidence.state != ReplicaState::acknowledged) all_waited = false;
    }
    const auto acknowledged = out.acknowledged();
    out.settled = !awaited;
    out.complete = std::none_of(out.replicas.begin(), out.replicas.end(), [](const ReplicaEvidence& r) { return r.state == ReplicaState::pending; });
    out.lost = !acknowledged && !awaited && !out.holders();
    switch (level) {
      case Durability::ephemeral:
        out.met = !out.lost;
        break;
      case Durability::durable:
        out.met = acknowledged > 0 && all_waited;
        break;
      default:
        out.met = acknowledged > 0 && acknowledged >= sought;
        break;
    }
    return out;
  }

  ReplicaRecord& replica_locked(const NodeId& node) DANNAN_REQUIRES(mutex) {
    for (auto& replica : replicas)
      if (replica.evidence.node == node) return replica;
    ReplicaRecord extra;
    extra.evidence.node = node;
    replicas.push_back(std::move(extra));
    return replicas.back();
  }

  // The waiters to tell now: those waiting for the ticket to settle once
  // nothing is awaited, those waiting for it to complete once nothing is
  // pending.
  std::vector<std::shared_ptr<Waiter>> due_waiters_locked() DANNAN_REQUIRES(mutex) {
    const auto evidence = evidence_locked();
    std::vector<std::shared_ptr<Waiter>> due;
    std::erase_if(waiters, [&](const std::shared_ptr<Waiter>& waiter) {
      if (!(waiter->complete ? evidence.complete : evidence.settled)) return false;
      due.push_back(waiter);
      return true;
    });
    return due;
  }
};

namespace {

// Changes one replica of a ticket and tells its waiters if that settled it.
template <class Record, class Change>
void update(const std::shared_ptr<Record>& ticket, const NodeId& node, Change change) {
  std::vector<std::shared_ptr<Waiter>> told;
  WriteEvidence evidence;
  {
    Lock lock(ticket->mutex);
    change(ticket->replica_locked(node));
    told = ticket->due_waiters_locked();
    if (!told.empty()) evidence = ticket->evidence_locked();
  }
  for (const auto& waiter : told) tell(waiter, evidence);
}

void acknowledge(ReplicaRecord& replica, std::optional<DurabilityTicket> durability = std::nullopt) {
  replica.evidence.state = ReplicaState::acknowledged;
  replica.evidence.why.clear();
  if (durability) replica.evidence.durability = durability;
  replica.awaiting = Awaiting::none;
  replica.running = false;
}

}  // namespace

// One write's calls, shared with their completions: until the write returns
// they are counted here; after, each lands in the write's ticket.
struct DistributedStore::WriteRun {
  struct Call {
    NodeRecord node;
    std::shared_ptr<dannan::net::CallHandle> handle;
    bool done = false;
    bool ok = false;
    bool refused = false;
    // Counted toward replacement once, when it failed or stalled.
    bool counted = false;
    bool spilled = false;
    std::string why;
    std::optional<DurabilityTicket> durability;
    // When it was placed, and how long it took to answer (Macha's local and
    // remote times on the write's quorum line).
    std::chrono::steady_clock::time_point started{};
    std::chrono::milliseconds took{0};
  };

  dannan::contract::Mutex mutex;
  std::condition_variable_any changed;
  std::deque<Call> calls DANNAN_GUARDED_BY(mutex);
  std::shared_ptr<TicketRecord> ticket DANNAN_GUARDED_BY(mutex);
  bool deferred = false;

  // A peer's answer, from the executor. Waits on nothing.
  void complete(size_t index, CallOutcome outcome) {
    bool ok = false;
    bool definitive = false;
    std::string why;
    std::optional<DurabilityTicket> durability;
    if (outcome.reply && outcome.reply->type == dannan::net::message::ok) {
      try {
        if (deferred) durability = message::decode_ticket(outcome.reply->payload);
        ok = true;
      } catch (const std::exception& error) {
        why = std::string("invalid durability ticket: ") + error.what();
      }
    } else {
      definitive = refused(outcome);
      why = outcome_text(outcome);
    }
    std::shared_ptr<TicketRecord> landed;
    NodeId node;
    {
      Lock lock(mutex);
      auto& call = calls[index];
      call.done = true;
      call.ok = ok;
      call.refused = definitive;
      call.why = why;
      call.durability = durability;
      call.took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - call.started);
      landed = ticket;
      node = call.node.id;
    }
    changed.notify_all();
    if (!landed) return;
    update(landed, node, [&](ReplicaRecord& replica) {
      if (replica.evidence.state == ReplicaState::acknowledged) return;
      if (ok && durability) {
        replica.evidence.state = ReplicaState::pending;
        replica.evidence.why = "awaiting barrier";
        replica.evidence.durability = durability;
        replica.awaiting = Awaiting::barrier;
      } else if (ok) {
        acknowledge(replica);
      } else if (definitive) {
        replica.evidence.state = ReplicaState::refused;
        replica.evidence.why = why;
        replica.awaiting = Awaiting::none;
      } else {
        replica.evidence.state = ReplicaState::pending;
        replica.evidence.why = "owed to repair: " + why;
        replica.awaiting = Awaiting::none;
      }
    });
  }
};

// One object's fetch, shared by every reader of it until it ends.
struct DistributedStore::SharedFetch {
  explicit SharedFetch(const ObjectId& id_) : id(id_) {}

  const ObjectId id;
  dannan::contract::Mutex mutex;
  std::condition_variable_any changed;
  size_t waiters DANNAN_GUARDED_BY(mutex) = 0;
  bool done DANNAN_GUARDED_BY(mutex) = false;
  ObjectData result DANNAN_GUARDED_BY(mutex);
  FetchAnswer answer DANNAN_GUARDED_BY(mutex) = FetchAnswer::try_later;
  std::optional<NodeId> served_by DANNAN_GUARDED_BY(mutex);
  FrameType frame DANNAN_GUARDED_BY(mutex) = FrameType::speculative;
  bool foreground DANNAN_GUARDED_BY(mutex) = false;
  bool persist DANNAN_GUARDED_BY(mutex) = false;
  size_t stripe DANNAN_GUARDED_BY(mutex) = 0;
  // The preferred owners first, then the other ranked nodes.
  bool fallback_phase DANNAN_GUARDED_BY(mutex) = false;
  std::set<NodeId> preferred DANNAN_GUARDED_BY(mutex);
  std::vector<NodeRecord> candidates DANNAN_GUARDED_BY(mutex);
  size_t attempt DANNAN_GUARDED_BY(mutex) = 0;
  size_t asked DANNAN_GUARDED_BY(mutex) = 0;
  bool all_absent DANNAN_GUARDED_BY(mutex) = true;
  std::optional<NodeId> active_peer DANNAN_GUARDED_BY(mutex);
  ReplicaWorkClass active_class DANNAN_GUARDED_BY(mutex) = ReplicaWorkClass::speculative;
  std::shared_ptr<dannan::net::CallHandle> handle DANNAN_GUARDED_BY(mutex);
};

// Repair's place and counters. The cursors and lists belong to the one
// caller of repair_step; the counters and the sample are read by anyone.
struct DistributedStore::RepairState {
  using TimePoint = dannan::contract::TimePoint;
  struct Retry {
    TimePoint due{};
    std::chrono::milliseconds delay{};
  };
  // Where each pass is: the last object settled in its list.
  std::optional<ObjectId> push_after;
  std::optional<ObjectId> pull_after;
  // The lists the positions are in, by identity: held, so a list freed and
  // another made at its address is never taken for it.
  std::shared_ptr<const ledger::PeerLacks> push_source;
  uint64_t push_generation = 0;
  std::shared_ptr<const std::vector<ObjectId>> pull_source;
  // Every peer's lacks as one sorted list: what the push considers.
  std::vector<ObjectId> push_list;
  bool listed = false;
  bool push_complete = false;
  bool pull_complete = false;
  // A list changed under a pass: what it passed may have changed, so the
  // pass is followed by another rather than reported settled.
  bool spans_change = false;
  // Copies pushed since the lacks were surveyed: known held since.
  std::set<std::pair<NodeId, ObjectId>> pushed;
  // Objects a peer could not settle now, each asked again after its own
  // delay, doubling from a second to ten minutes; every one is due at once
  // when the reachable set changes (discipline 2). Past retries_max an
  // object is met again on the next pass instead.
  std::map<ObjectId, Retry> pull_retries;
  std::map<ObjectId, Retry> push_retries;
  uint64_t retry_epoch = 0;

  std::atomic<uint64_t> pull_unsourceable{0};
  std::atomic<uint64_t> local_unreadable{0};
  std::atomic<uint64_t> push_examined{0};
  std::atomic<uint64_t> pull_examined{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> passes{0};
  std::atomic<uint64_t> pushed_copies{0};
  std::atomic<uint64_t> pulled_copies{0};
  std::atomic<uint64_t> local_drops{0};
  std::atomic<bool> push_phase_complete{false};
  mutable dannan::contract::Mutex sample_mutex;
  std::deque<ObjectId> unsourceable_sample DANNAN_GUARDED_BY(sample_mutex);
  std::chrono::steady_clock::time_point unsourceable_logged DANNAN_GUARDED_BY(sample_mutex){};
  std::chrono::steady_clock::time_point unreadable_logged DANNAN_GUARDED_BY(sample_mutex){};

  static constexpr std::chrono::milliseconds retry_first{1000};
  static constexpr std::chrono::milliseconds retry_cap{600000};
  static constexpr size_t retries_max = 4096;
  static constexpr size_t sample_size = 32;
  // Concurrent pushes, and objects needing nothing passed in one step.
  static constexpr size_t sends_in_flight = 8;
  static constexpr size_t skip_budget = 65536;

  // Schedules `id` again: first after retry_first, then each time after
  // twice the delay before, up to retry_cap.
  static void retry_later(std::map<ObjectId, Retry>& retries, const ObjectId& id, TimePoint now) {
    if (const auto found = retries.find(id); found != retries.end()) {
      found->second.delay = std::min(found->second.delay * 2, retry_cap);
      found->second.due = now + found->second.delay;
    } else if (retries.size() < retries_max) {
      retries.emplace(id, Retry{now + retry_first, retry_first});
    }
  }
};

DistributedStore::DistributedStore(dannan::Context& context, Stores stores, DistributedStoreOptions options)
    : _context(context),
      _activity(stores.activity ? *stores.activity : context.activity()),
      _executor(context.time().executor()),
      _data(stores.data),
      _control(stores.control),
      _cache(stores.cache),
      _claims(stores.claims),
      _ledger(stores.ledger),
      _options(options),
      _time(context.time().source()),
      _self(context.identity().id()),
      _logger(context.logger("dagda.store")),
      _storage_event(context.events().kind("storage")),
      _credit(stores.credit),
      _io(stores.io),
      _control_fetch(context, stores.control, options.metadata_call, dannan::net::CallWatch{options.control_no_progress, std::nullopt}),
      _repair(std::make_unique<RepairState>()),
      _shared(std::make_shared<Shared>()) {
  if (!_options.replication) throw std::invalid_argument("the store needs at least one replica");
  {
    WriteLock lock(_shared->alive);
    _shared->store = this;
  }
  for (size_t i = 0; i < std::max<size_t>(1, _options.workers); ++i)
    _workers.emplace_back([this](std::stop_token stop) { _context.threads().loop("dagda.store-worker", stop, [this, stop] { worker_loop(stop); }); });
  _prompt_thread = std::jthread(
      [this](std::stop_token stop) { _context.threads().loop("dagda.prompt-replication", stop, [this, stop] { prompt_replication_loop(stop); }); });
  _local_writer =
      std::jthread([this](std::stop_token stop) { _context.threads().loop("dagda.local-copy-writer", stop, [this, stop] { local_writer_loop(stop); }); });
  _watch = _context.nodes().watch(_executor, [weak = std::weak_ptr<Shared>(_shared)](dannan::RosterChange change) {
    const auto shared = weak.lock();
    if (!shared) return;
    ReadLock lock(shared->alive);
    if (shared->store) shared->store->on_roster(change);
  });
}

DistributedStore::~DistributedStore() {
  _context.nodes().unwatch(_watch);
  _stopping = true;
  {
    WriteLock lock(_shared->alive);
    _shared->store = nullptr;
  }
  {
    Lock lock(_jobs_mutex);
    _jobs_stopped = true;
  }
  _jobs_wake.notify_all();
  for (auto& worker : _workers) worker.request_stop();
  for (auto& worker : _workers)
    if (worker.joinable()) worker.join();
  _local_writer.request_stop();
  _local_copy_wake.notify_all();
  if (_local_writer.joinable()) _local_writer.join();
  _prompt_thread.request_stop();
  _prompt_wake.notify_all();
  if (_prompt_thread.joinable()) _prompt_thread.join();
}

// ---- Placement -------------------------------------------------------------

std::vector<NodeRecord> DistributedStore::hosting(const std::vector<NodeRecord>& nodes) const { return dannan::placement::with_advert(nodes, extents_advert); }

std::vector<NodeRecord> DistributedStore::ranked(const ObjectId& id) const {
  // The one choke point for DATA placement: owners, fallbacks, claims and
  // prompt replication all use it, so only nodes that host extents are ever
  // candidates. Every node gossips the same adverts, so all compute the same.
  return _context.nodes().placement(id.bytes, _options.replication, data_scheme(), extents_advert);
}

std::vector<NodeRecord> DistributedStore::replication_set(const ObjectId& id) const {
  auto set = dannan::placement::by_capacity(id.bytes, hosting(_context.nodes().roster()), _options.replication, data_scheme());
  if (set.size() > _options.replication) set.resize(_options.replication);
  return set;
}

bool DistributedStore::should_own(const ObjectId& id) const {
  auto owners = ranked(id);
  if (owners.size() > _options.replication) owners.resize(_options.replication);
  return std::any_of(owners.begin(), owners.end(), [&](const NodeRecord& node) { return node.id == _self; });
}

bool DistributedStore::holds_extents() const { return _context.nodes().self().advert(extents_advert).has_value(); }

bool DistributedStore::has(const ObjectId& id) const { return _data.has(id); }

bool DistributedStore::locally_available(const ObjectId& id) const { return _data.has(id) || _cache.has(id); }

bool DistributedStore::cache_local(const ObjectId& id, std::span<const uint8_t> data) {
  if (!_cache.enabled()) return false;
  try {
    const auto admitted = admit_data(FrameType::speculative, data.size(), no_caller_deadline);
    return admitted && _cache.put(id, data);
  } catch (const std::exception& error) {
    _logger.debug("cache write-through skipped object=" + to_string(id) + " error=" + error.what());
    return false;
  }
}

bool DistributedStore::hydration_available() const { return _cache.enabled(); }

bool DistributedStore::hydrate(const ObjectId& id, ReadOptions options) {
  if (locally_available(id)) return true;
  if (!_cache.enabled()) return false;
  const auto read = get_remote(id, options, false);
  if (!read.object) return false;
  const auto admitted = admit_data(options.frame, read.object->bytes.size(), no_caller_deadline);
  return admitted && _cache.put(id, read.object->bytes);
}

std::vector<ObjectId> DistributedStore::retain_data_here(const std::vector<ObjectId>& input, const contract::RetentionDot& dot) {
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<ObjectId> elsewhere;
  (void)_data.claim_locks().in_batches(ids, [&](std::span<const ObjectId> part) {
    std::vector<ObjectId> held;
    for (const auto& id : part) {
      if (!_data.claim_locks().deleting(id) && _data.has(id))
        held.push_back(id);
      else
        elsewhere.push_back(id);
    }
    if (!held.empty()) _claims.retain_batch(contract::RetentionClass::data, held, dot);
    return true;
  });
  std::sort(elsewhere.begin(), elsewhere.end());
  return elsewhere;
}

uint64_t DistributedStore::reachability_epoch() const noexcept {
  return (_context.events().count(dannan::status::NodeEvents::membership) << 8) + _data.online_backends();
}

// ---- Workers ---------------------------------------------------------------

bool DistributedStore::submit(std::function<void()> job, size_t ephemeral_bytes) {
  {
    Lock lock(_jobs_mutex);
    if (_jobs_stopped) return false;
    if (ephemeral_bytes) {
      if (_ephemeral_bytes + ephemeral_bytes > _options.ephemeral_queue_bytes) return false;
      _ephemeral_bytes += ephemeral_bytes;
      job = [this, inner = std::move(job), ephemeral_bytes] {
        struct Release {
          DistributedStore& store;
          size_t bytes;
          ~Release() {
            Lock lock(store._jobs_mutex);
            store._ephemeral_bytes -= bytes;
          }
        } release{*this, ephemeral_bytes};
        inner();
      };
    }
    _jobs.push_back(std::move(job));
  }
  _jobs_wake.notify_one();
  return true;
}

void DistributedStore::worker_loop(std::stop_token stop) {
  while (true) {
    std::function<void()> job;
    {
      Lock lock(_jobs_mutex);
      _jobs_wake.wait(lock.native(), stop, [&]() DANNAN_REQUIRES(_jobs_mutex) { return !_jobs.empty() || _jobs_stopped; });
      // Jobs still queued as the store goes are dropped: an ephemeral write
      // among them is lost, as its level allows.
      if (stop.stop_requested() || _jobs_stopped) return;
      job = std::move(_jobs.front());
      _jobs.pop_front();
    }
    try {
      job();
    } catch (const std::exception& error) {
      _logger.warn(std::string("store job failed: ") + error.what());
    }
  }
}

// ---- Writes ----------------------------------------------------------------

void DistributedStore::check_write(const ObjectId& id, std::span<const uint8_t> data) const {
  if (object_id(data) != id) throw std::runtime_error("object hash mismatch");
  if (data.size() > _options.extent_size) throw std::runtime_error("DATA object exceeds configured extent size");
}

message::WritePrincipal DistributedStore::writer_of(const std::optional<dannan::contract::Principal>& principal, const std::string& prefix) const {
  message::WritePrincipal out;
  out.principal = principal ? *principal : dannan::contract::Principal{dannan::contract::Principal::Kind::node, dannan::identity::to_string(_self)};
  out.prefix = prefix.empty() ? _options.domain.name : prefix;
  return out;
}

std::optional<std::string> DistributedStore::scope_refusal(const message::WritePrincipal& writer) const {
  const auto decision = _context.identity().verify(writer.principal, writer.prefix);
  if (decision.allowed) return std::nullopt;
  return scope_refusal_text(decision);
}

std::shared_ptr<DistributedStore::TicketRecord> DistributedStore::new_ticket(const ObjectId& id, Durability level, bool deferred) {
  // Each forgotten ticket's waiters, told its own evidence.
  std::vector<std::pair<std::shared_ptr<Waiter>, WriteEvidence>> forgotten;
  size_t over_ceiling = 0;
  std::shared_ptr<TicketRecord> record;
  {
    Lock lock(_shared->mutex);
    record = std::make_shared<TicketRecord>(_shared->next_serial++, id, level, deferred);
    _shared->tickets.emplace(record->serial, record);
    // Past the bound, settled tickets go, oldest first; one still waiting never does, nor
    // a deferred one nobody has yet asked after, as Macha's batch, the caller's, kept
    // every extent it was handed until its publisher consumed it. One walk frees what
    // it can; when tickets still waiting keep the table over its bound, the next walk is
    // put off until it has grown as much again, so a publication of many extents costs
    // amortized constant time a write, not a walk each.
    const auto bound = std::max<size_t>(1, _options.max_tickets);
    if (_shared->tickets.size() > std::max(bound, _shared->ticket_walk_at)) {
      for (auto it = _shared->tickets.begin(); it != _shared->tickets.end() && _shared->tickets.size() > bound;) {
        Lock ticket_lock(it->second->mutex);
        // A deferred ticket nobody has asked after is a publication's extent its
        // publisher has yet to consume: kept, settled or not.
        if (!it->second->evidence_locked().settled || (it->second->deferred && !it->second->asked)) {
          ++it;
          continue;
        }
        auto waiters = std::exchange(it->second->waiters, {});
        if (!waiters.empty()) {
          const auto evidence = it->second->evidence_locked();
          for (auto& waiter : waiters) forgotten.emplace_back(std::move(waiter), evidence);
        }
        ticket_lock.unlock();
        it = _shared->tickets.erase(it);
      }
      _shared->ticket_walk_at = _shared->tickets.size() > bound ? _shared->tickets.size() * 2 : bound;
    }
    // The ceiling on tickets still waiting: the oldest goes, its waiters told.
    const auto ceiling = std::max(bound, _options.max_waiting_tickets);
    while (_shared->tickets.size() > ceiling) {
      auto oldest = _shared->tickets.begin();
      {
        Lock ticket_lock(oldest->second->mutex);
        auto waiters = std::exchange(oldest->second->waiters, {});
        if (!waiters.empty()) {
          const auto evidence = oldest->second->evidence_locked();
          for (auto& waiter : waiters) forgotten.emplace_back(std::move(waiter), evidence);
        }
      }
      _shared->tickets.erase(oldest);
      ++over_ceiling;
    }
  }
  if (over_ceiling)
    _logger.warn("deferred write tickets over their ceiling: " + std::to_string(over_ceiling) +
                 " still waiting forgotten, oldest first; a deferred write nobody waits on holds its ticket until then");
  for (const auto& [waiter, evidence] : forgotten) tell(waiter, evidence);
  return record;
}

WriteEvidence DistributedStore::run_write(const ObjectId& id, std::span<const uint8_t> data, const WriteOptions& options, std::shared_ptr<TicketRecord> into) {
  _activity.note(options.frame, data.size());
  const auto write_started = std::chrono::steady_clock::now();
  // A deadline of the caller's runs from here. With none, no clock runs on the
  // write at all, as Macha's (replicate): its local copy waits under the arbiter's
  // window and each peer's put under call_no_progress, and it ends when each has
  // answered or been spilled past.
  const bool own_deadline = options.deadline.count() > 0;
  const auto deadline = _time.now() + options.deadline;
  // An ephemeral write runs in the background as the store's ordinary write.
  const auto level = options.level == Durability::ephemeral ? Durability::replicated : options.level;
  const bool deferred = options.deferred;

  const auto set = replication_set(id);
  auto nodes = ranked(id);
  // The local store is the first replica tried, then placement order, so an
  // offsite writer's first copy and its readers do not depend on the WAN.
  if (const auto self = std::find_if(nodes.begin(), nodes.end(), [&](const NodeRecord& node) { return node.id == _self; });
      self != nodes.end() && self != nodes.begin())
    std::rotate(nodes.begin(), self, self + 1);
  std::set<NodeId> live;
  for (const auto& node : nodes) live.insert(node.id);
  std::set<NodeId> members;
  for (const auto& node : set) members.insert(node.id);

  // Durable writes to the local store and every live member at once, the
  // rest standing by for a member that refuses. The others seek their floor
  // from the first nodes in order.
  std::vector<NodeRecord> order;
  std::set<NodeId> waited;
  size_t floor = 0;
  size_t initial = 0;
  if (level == Durability::durable) {
    if (!nodes.empty() && nodes.front().id == _self) order.push_back(nodes.front());
    for (const auto& node : nodes)
      if (members.contains(node.id)) {
        waited.insert(node.id);
        if (node.id != _self) order.push_back(node);
      }
    initial = order.size();
    for (const auto& node : nodes)
      if (std::none_of(order.begin(), order.end(), [&](const NodeRecord& o) { return o.id == node.id; })) order.push_back(node);
    floor = waited.size();
  } else {
    order = nodes;
    if (!nodes.empty()) floor = level == Durability::persisted ? 1 : std::max<size_t>(1, std::min(_options.write_copies, nodes.size()));
    initial = std::min(floor, order.size());
  }
  const size_t target = std::min(_options.replication, nodes.size());

  auto run = std::make_shared<WriteRun>();
  run->deferred = deferred;
  const auto writer = writer_of(options.principal, options.prefix);
  const auto payload = message::encode_object(id, data, writer);
  const auto type = deferred ? message::put_object_deferred : message::put_object;
  size_t next_fallback = initial;
  size_t replacement_needed = 0;

  const auto launch = [&](const NodeRecord& node) {
    size_t index = 0;
    {
      Lock lock(run->mutex);
      run->calls.push_back({});
      run->calls.back().node = node;
      run->calls.back().started = std::chrono::steady_clock::now();
      index = run->calls.size() - 1;
    }
    if (node.id == _self) {
      bool ok = false;
      bool busy = false;
      std::string why;
      std::optional<DurabilityTicket> durability;
      try {
        // This replica checks the writer's scope before it stores, as every
        // replica does; a refusal is evidence, never a bare failure.
        const auto refusal = scope_refusal(writer);
        // This node's device admits the write as a peer's would: no lease is
        // held across another node's answer.
        // A write with a deadline of its own keeps it; one with none waits as a
        // peer's server would, and fails in its words.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - _time.now());
        const auto admitted =
            refusal ? std::nullopt
                    : admit_data(options.frame, data.size(), own_deadline ? std::optional(std::max(left, std::chrono::milliseconds(1))) : no_caller_deadline,
                                 options.cancelled.get());
        if (refusal) {
          why = *refusal;
        } else if (!admitted) {
          busy = true;
          why = own_deadline ? "503 storage_busy: DATA resource admission busy or stopping" : "503 stopping: DATA resource admission stopping";
        } else {
          const bool held = _data.has(id);
          if (deferred) {
            durability = _data.put_deferred(id, data);
            ok = durability.has_value();
          } else {
            ok = _data.put(id, data);
          }
          if (ok && !held) _context.events().notify(_storage_event);
          if (!ok) why = "507 storage_full: storage limit reached";
        }
      } catch (const std::exception& error) {
        why = error.what();
      }
      {
        Lock lock(run->mutex);
        auto& call = run->calls[index];
        call.done = true;
        call.ok = ok;
        call.refused = !ok && !busy;
        call.why = why;
        call.durability = durability;
        call.took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - call.started);
      }
      return;
    }
    // A call runs on past the write while it moves, so a slow peer's answer still
    // reaches the ticket.
    auto handle = _context.routes().call(
        node, type, payload, options.frame, _options.object_transfer_call, _executor,
        [run, index](CallOutcome outcome) { run->complete(index, std::move(outcome)); }, peer_call_watch());
    Lock lock(run->mutex);
    run->calls[index].handle = std::move(handle);
  };

  // This node, when it holds a copy, is first in order, and only its launch waits.
  for (size_t i = 0; i < initial; ++i) launch(order[i]);

  // The caller's budget over the whole write (Macha's put_impl, 98b816d
  // :352-386): any copy landing, fallback starting, pending call moving within it
  // or move of the caller's counter re-arms it.
  const auto budget = options.no_progress_budget;
  const bool budgeted = budget.count() > 0;
  uint64_t progress_seen = budgeted && options.progress ? options.progress->load(std::memory_order_relaxed) : 0;
  auto window_started = _time.now();
  size_t done_seen = 0;
  // The caller's cancel, seen while the write still waited (Macha's put fails then).
  bool cancelled_seen = false;

  size_t success = 0;
  {
    Lock lock(run->mutex);
    while (true) {
      success = 0;
      size_t unfinished = 0;
      size_t stalled = 0;
      size_t waited_unfinished = 0;
      size_t waited_ok = 0;
      size_t done = 0;
      bool moving = false;
      const auto now = _time.now();
      auto wake = own_deadline ? deadline : now + look_at_the_clock;
      for (auto& call : run->calls) {
        const bool member_waited = waited.contains(call.node.id);
        if (call.done) ++done;
        if (!call.done && call.handle && call.handle->idle_for() < budget) moving = true;
        if (call.done) {
          if (call.ok) {
            ++success;
            if (member_waited) ++waited_ok;
          } else if (!call.counted) {
            call.counted = true;
            if (!call.spilled) ++replacement_needed;
          }
          continue;
        }
        ++unfinished;
        if (member_waited) ++waited_unfinished;
        // A durable write waits on a live member; the others spill past a peer
        // stalled beyond write_stall.
        if (level != Durability::durable && !call.spilled && call.handle) {
          // A call not placed yet has made no progress to judge.
          const auto idle = call.handle->idle_for();
          if (idle == std::chrono::milliseconds::max()) {
            wake = std::min(wake, now + std::max(std::chrono::milliseconds(1), _options.write_stall / 4));
          } else if (idle >= _options.write_stall) {
            call.spilled = true;
            call.counted = true;
            ++replacement_needed;
            _logger.debug("object write stalled; spilling id=" + to_string(id) + " peer=" + dannan::identity::to_string(call.node.id).substr(0, 12) +
                          " no_progress_ms=" + std::to_string(idle.count()));
          } else {
            wake = std::min(wake, now + (_options.write_stall - idle));
          }
        }
        if (call.spilled) ++stalled;
      }
      const bool satisfied = level == Durability::durable ? (waited_ok == waited.size() && success > 0) : (floor && success >= floor);
      if (satisfied) break;
      if (budgeted) {
        const auto counted = options.progress ? options.progress->load(std::memory_order_relaxed) : progress_seen;
        if (done != done_seen || moving || counted != progress_seen) window_started = now;
        done_seen = done;
        progress_seen = counted;
        if (now - window_started >= budget) {
          for (auto& call : run->calls)
            if (!call.done && call.handle) call.handle->cancel();
          _logger.debug("object write made no progress within budget id=" + to_string(id) + " budget_ms=" + std::to_string(budget.count()) +
                        " success=" + std::to_string(success) + " sought=" + std::to_string(floor));
          break;
        }
        wake = std::min(wake, window_started + budget);
      }
      if ((options.cancelled && options.cancelled->load(std::memory_order_relaxed)) || _stopping.load()) {
        for (auto& call : run->calls)
          if (!call.done && call.handle) call.handle->cancel();
        cancelled_seen = options.cancelled && options.cancelled->load(std::memory_order_relaxed);
        break;
      }
      if (own_deadline && now >= deadline) break;
      if (replacement_needed && next_fallback < order.size()) {
        --replacement_needed;
        const auto node = order[next_fallback++];
        lock.unlock();
        launch(node);
        lock.lock();
        window_started = _time.now();
        continue;
      }
      const size_t untried = order.size() - next_fallback;
      if (level == Durability::durable) {
        // Every member has answered; what refused has spilled to whoever
        // would stand in.
        if (!waited_unfinished && !unfinished) break;
      } else {
        // No further copy can be had promptly: what landed is the write. A
        // stalled peer is waited for only while nothing has landed.
        if (success > 0 && success + (unfinished - stalled) + untried < floor) break;
        if (!success && !unfinished && !untried) break;
      }
      if (options.cancelled) wake = std::min(wake, now + std::chrono::milliseconds(20));
      wake = std::max(wake, now + std::chrono::milliseconds(1));
      run->changed.wait_for(lock.native(), std::min<std::chrono::nanoseconds>(wake - now, look_at_the_clock));
    }

    // Each remote copy that landed teaches the network estimate, timed by its own call,
    // as Macha's write does (ds:435 at f0597a4).
    for (const auto& call : run->calls)
      if (call.done && call.ok && call.node.id != _self) note_network(data.size(), call.took);

    // The evidence, built under the run's lock: a call answering from here on
    // lands in the ticket instead.
    auto record = into ? std::move(into) : new_ticket(id, options.level, deferred);
    {
      Lock ticket_lock(record->mutex);
      record->sought = floor;
      record->cancelled = cancelled_seen;
      record->replicas.clear();
      const auto from_call = [&](ReplicaRecord& replica, const WriteRun::Call& call) {
        if (!call.done) {
          replica.evidence.state = ReplicaState::pending;
          replica.evidence.why = "no answer yet";
          replica.awaiting = Awaiting::call;
        } else if (call.ok && call.durability) {
          replica.evidence.state = ReplicaState::pending;
          replica.evidence.why = "awaiting barrier";
          replica.evidence.durability = call.durability;
          replica.awaiting = Awaiting::barrier;
        } else if (call.ok) {
          acknowledge(replica);
        } else if (call.refused) {
          replica.evidence.state = ReplicaState::refused;
          replica.evidence.why = call.why;
        } else {
          replica.evidence.state = ReplicaState::pending;
          replica.evidence.why = "owed to repair: " + call.why;
        }
      };
      const auto call_on = [&](const NodeId& node) DANNAN_REQUIRES(run->mutex) -> const WriteRun::Call* {
        for (const auto& call : run->calls)
          if (call.node.id == node) return &call;
        return nullptr;
      };
      for (const auto& member : set) {
        ReplicaRecord replica;
        replica.evidence.node = member.id;
        replica.evidence.in_set = true;
        replica.waited = waited.contains(member.id);
        if (const auto* call = call_on(member.id)) {
          from_call(replica, *call);
        } else if (!live.contains(member.id)) {
          // Dead to Dannan: not waited on. A durable write's ticket brings it
          // the object when it is back.
          replica.evidence.state = ReplicaState::pending;
          replica.evidence.why = "dead";
          replica.awaiting = level == Durability::durable ? Awaiting::dead : Awaiting::none;
        } else {
          replica.evidence.state = ReplicaState::pending;
          replica.evidence.why = "owed to repair";
        }
        record->replicas.push_back(std::move(replica));
      }
      for (const auto& call : run->calls) {
        if (members.contains(call.node.id)) continue;
        // A node outside the set that never answered at all is no evidence.
        if (call.done && !call.ok && !call.refused) continue;
        ReplicaRecord replica;
        replica.evidence.node = call.node.id;
        from_call(replica, call);
        record->replicas.push_back(std::move(replica));
      }
    }
    run->ticket = record;
    into = record;
  }

  if (success > 0 && success < target) queue_prompt_replication(id);
  // Macha's slow-write line (distributed_store.cpp:279-289 at 98b816d): a write of
  // half a second or more says where its time went.
  const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - write_started);
  if (total >= std::chrono::milliseconds(500) && _logger.enabled(dannan::status::LogLevel::debug)) {
    std::chrono::milliseconds local{0};
    std::chrono::milliseconds remote_max{0};
    {
      Lock lock(run->mutex);
      for (const auto& call : run->calls) {
        if (!call.done) continue;
        if (call.node.id == _self)
          local = call.took;
        else
          remote_max = std::max(remote_max, call.took);
      }
    }
    _logger.debug("object write quorum id=" + to_string(id) + " bytes=" + std::to_string(data.size()) + " replicas=" + std::to_string(target) +
                  " required=" + std::to_string(floor) + " minimum=" + std::to_string(floor) + " success=" + std::to_string(success) +
                  " local_ms=" + std::to_string(local.count()) + " remote_max_ms=" + std::to_string(remote_max.count()) +
                  " total_ms=" + std::to_string(total.count()) + " result=" + std::to_string(success >= floor && success > 0 ? 1 : 0));
  }
  std::vector<std::shared_ptr<Waiter>> told;
  WriteEvidence evidence;
  {
    Lock lock(into->mutex);
    told = into->due_waiters_locked();
    evidence = into->evidence_locked();
  }
  for (const auto& waiter : told) tell(waiter, evidence);
  if (!success) _logger.debug("object write landed nowhere id=" + to_string(id) + " level=" + durability_name(options.level));
  return evidence;
}

WriteEvidence DistributedStore::write(const ObjectId& id, std::span<const uint8_t> data, WriteOptions options) {
  check_write(id, data);
  if (options.level != Durability::ephemeral) return run_write(id, data, options);
  // Received and queued: the set is named pending until the queued write
  // runs.
  auto record = new_ticket(id, Durability::ephemeral, options.deferred);
  {
    Lock lock(record->mutex);
    for (const auto& member : replication_set(id)) {
      ReplicaRecord replica;
      replica.evidence.node = member.id;
      replica.evidence.in_set = true;
      replica.evidence.why = "queued";
      replica.awaiting = Awaiting::queued;
      record->replicas.push_back(std::move(replica));
    }
    if (record->replicas.empty()) {
      ReplicaRecord replica;
      replica.evidence.node = _self;
      replica.evidence.why = "queued";
      replica.awaiting = Awaiting::queued;
      record->replicas.push_back(std::move(replica));
    }
  }
  Bytes copy(data.begin(), data.end());
  const auto size = copy.size();
  const bool queued =
      submit([this, id, bytes = std::move(copy), options, record]() mutable { (void)run_write(id, bytes, options, record); }, std::max<size_t>(1, size));
  Lock lock(record->mutex);
  if (!queued)
    for (auto& replica : record->replicas) {
      replica.evidence.state = ReplicaState::refused;
      replica.evidence.why = "the ephemeral queue is full";
      replica.awaiting = Awaiting::none;
    }
  return record->evidence_locked();
}

void DistributedStore::write(const ObjectId& id, Bytes data, WriteOptions options, boost::asio::any_io_executor executor, WriteDone done) {
  check_write(id, data);
  if (options.level == Durability::ephemeral) {
    auto evidence = write(id, std::span<const uint8_t>(data), options);
    boost::asio::post(executor, [done = std::move(done), evidence = std::move(evidence)] { done(evidence); });
    return;
  }
  auto job = [this, id, bytes = std::move(data), options, executor, done]() {
    auto evidence = run_write(id, bytes, options);
    boost::asio::post(executor, [done, evidence = std::move(evidence)] { done(evidence); });
  };
  if (!submit(std::move(job))) {
    WriteEvidence refused_evidence;
    refused_evidence.id = id;
    refused_evidence.level = options.level;
    refused_evidence.settled = true;
    refused_evidence.lost = true;
    boost::asio::post(executor, [done = std::move(done), refused_evidence] { done(refused_evidence); });
  }
}

ObjectId DistributedStore::put(std::span<const uint8_t> data, WriteOptions options) {
  const auto started = std::chrono::steady_clock::now();
  const auto id = object_id(data);
  const auto evidence = write(id, data, options);
  // Macha's: a cancel seen while the write waited fails it, whatever landed.
  if (evidence.cancelled) throw std::runtime_error("object replication cancelled");
  if (!evidence.holders() && options.level != Durability::ephemeral) throw StorageUnavailable("object replication quorum unavailable");
  if (_logger.enabled(dannan::status::LogLevel::all))
    _logger.trace("DIAG object-put id=" + to_string(id) + " bytes=" + std::to_string(data.size()) +
                  " ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
  return id;
}

bool DistributedStore::put(const ObjectId& id, std::span<const uint8_t> data, WriteOptions options) {
  const auto evidence = write(id, data, options);
  if (evidence.cancelled) return false;
  return options.level == Durability::ephemeral ? !evidence.lost : evidence.holders() > 0;
}

ObjectId DistributedStore::put_deferred(std::span<const uint8_t> data, WriteOptions options) {
  options.deferred = true;
  return put(data, options);
}

WriteEvidence DistributedStore::put_deferred_here(const ObjectId& id, std::span<const uint8_t> data, FrameType frame) {
  WriteOptions options;
  options.deferred = true;
  options.frame = frame;
  // Here is one copy, and this node is tried first; on a node that hosts none,
  // as put_deferred.
  if (holds_extents()) options.level = Durability::persisted;
  return write(id, data, options);
}

bool DistributedStore::put_here(const ObjectId& id, std::span<const uint8_t> data, FrameType frame) {
  WriteOptions options;
  options.frame = frame;
  if (!holds_extents()) return put(id, data, options);
  check_write(id, data);
  if (const auto refusal = scope_refusal(writer_of(std::nullopt, {}))) {
    _logger.debug("local write refused id=" + to_string(id) + " " + *refusal);
    return false;
  }
  bool held = false;
  {
    const auto admitted = admit_data(frame, data.size(), no_caller_deadline);
    if (!admitted) return put(id, data, options);
    held = _data.has(id);
    if (!_data.put(id, data)) return put(id, data, options);
  }
  if (!held) _context.events().notify(_storage_event);
  queue_prompt_replication(id);
  return true;
}

bool DistributedStore::put_on(const NodeRecord& target, const ObjectId& id, std::span<const uint8_t> data, FrameType frame) {
  if (target.id == _self) {
    // A copy this node takes for repair or replication is its own write.
    if (const auto refusal = scope_refusal(writer_of(std::nullopt, {}))) {
      _logger.debug("local copy refused id=" + to_string(id) + " " + *refusal);
      return false;
    }
    const auto admitted = admit_data(frame, data.size(), no_caller_deadline);
    if (!admitted) return false;
    const bool held = _data.has(id);
    const bool ok = _data.put(id, data);
    if (ok && !held) _context.events().notify(_storage_event);
    return ok;
  }
  // No lease here across the peer's admission: two nodes replicating to each
  // other would each wait on the other's slot.
  const auto started = Clock::now();
  const auto outcome =
      _context.routes().call_and_wait(target, message::put_object, message::encode_object(id, data), frame, _options.object_transfer_call, peer_call_watch());
  const bool ok = outcome.reply && outcome.reply->type == dannan::net::message::ok;
  if (ok) note_network(data.size(), Clock::now() - started);
  return ok;
}

// ---- Tickets ---------------------------------------------------------------

std::optional<WriteEvidence> DistributedStore::status(Ticket ticket) const {
  std::shared_ptr<TicketRecord> record;
  {
    Lock lock(_shared->mutex);
    const auto found = _shared->tickets.find(ticket.serial);
    if (found == _shared->tickets.end()) return std::nullopt;
    record = found->second;
  }
  Lock lock(record->mutex);
  record->asked = true;
  return record->evidence_locked();
}

void DistributedStore::wait(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, WriteDone done) {
  wait_until(ticket, false, deadline, std::move(executor), std::move(done));
}

void DistributedStore::wait_complete(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, WriteDone done) {
  wait_until(ticket, true, deadline, std::move(executor), std::move(done));
}

void DistributedStore::wait_until(Ticket ticket, bool complete, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, WriteDone done) {
  auto record = arm_wait(ticket, complete, deadline, std::move(executor), std::move(done));
  if (record) submit([this, record = std::move(record), deadline] { run_barriers({record}, deadline); });
}

void DistributedStore::wait(std::vector<Ticket> tickets, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, ManyDone done) {
  if (tickets.empty()) {
    boost::asio::post(executor, [done = std::move(done)] { done({}); });
    return;
  }
  // Each slot is written by its own completion; the last to finish reads them.
  struct Gathered {
    std::vector<WriteEvidence> evidence;
    std::atomic<size_t> left;
    boost::asio::any_io_executor executor;
    ManyDone done;
  };
  auto gathered = std::make_shared<Gathered>();
  gathered->evidence.resize(tickets.size());
  gathered->left = tickets.size();
  gathered->executor = std::move(executor);
  gathered->done = std::move(done);
  std::vector<std::shared_ptr<TicketRecord>> due;
  for (size_t i = 0; i < tickets.size(); ++i) {
    auto record = arm_wait(tickets[i], false, deadline, _executor, [gathered, i](WriteEvidence evidence) {
      gathered->evidence[i] = std::move(evidence);
      if (gathered->left.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
      boost::asio::post(gathered->executor, [gathered] { gathered->done(std::move(gathered->evidence)); });
    });
    if (record) due.push_back(std::move(record));
  }
  // Only once every ticket is in: the one job sees them all.
  if (!due.empty()) submit([this, due = std::move(due), deadline] { run_barriers(due, deadline); });
}

std::vector<WriteEvidence> DistributedStore::wait(const std::vector<Ticket>& tickets, std::chrono::milliseconds deadline) {
  auto promise = std::make_shared<std::promise<std::vector<WriteEvidence>>>();
  auto future = promise->get_future();
  wait(tickets, deadline, _executor, [promise](std::vector<WriteEvidence> evidence) { promise->set_value(std::move(evidence)); });
  // The deadline answers first; the margin only covers the executor.
  if (future.wait_for(deadline + std::chrono::seconds(5)) == std::future_status::ready) {
    auto evidence = future.get();
    for (const auto& one : evidence) note_unmet_barrier(one);
    return evidence;
  }
  std::vector<WriteEvidence> out;
  for (const auto& ticket : tickets) {
    auto evidence = status(ticket);
    out.push_back(evidence ? std::move(*evidence) : WriteEvidence{});
  }
  return out;
}

std::shared_ptr<DistributedStore::TicketRecord> DistributedStore::arm_wait(Ticket ticket, bool complete, std::chrono::milliseconds deadline,
                                                                           boost::asio::any_io_executor executor, WriteDone done) {
  std::shared_ptr<TicketRecord> record;
  {
    Lock lock(_shared->mutex);
    if (const auto found = _shared->tickets.find(ticket.serial); found != _shared->tickets.end()) record = found->second;
  }
  if (!record) {
    // Forgotten: an evidence with no ticket, settled.
    WriteEvidence unknown;
    unknown.settled = true;
    boost::asio::post(executor, [done = std::move(done), unknown] { done(unknown); });
    return nullptr;
  }
  auto waiter = std::make_shared<Waiter>();
  waiter->executor = executor;
  waiter->done = std::move(done);
  waiter->complete = complete;
  // The timer is armed and cancelled only on its own strand.
  const auto strand = boost::asio::make_strand(_executor);
  waiter->timer = std::make_shared<boost::asio::steady_timer>(strand, deadline);
  bool barriers = false;
  {
    Lock lock(record->mutex);
    record->asked = true;
    for (const auto& replica : record->replicas)
      if (replica.awaiting == Awaiting::barrier && !replica.running) barriers = true;
    const auto evidence = record->evidence_locked();
    if (complete ? evidence.complete : evidence.settled) {
      lock.unlock();
      tell(waiter, evidence);
      return nullptr;
    }
    record->waiters.push_back(waiter);
  }
  boost::asio::post(strand, [waiter, record] {
    waiter->timer->async_wait([waiter, record](const boost::system::error_code&) {
      WriteEvidence evidence;
      {
        Lock lock(record->mutex);
        std::erase(record->waiters, waiter);
        evidence = record->evidence_locked();
      }
      tell(waiter, evidence);
    });
  });
  return barriers ? record : nullptr;
}

std::optional<WriteEvidence> DistributedStore::wait(Ticket ticket, std::chrono::milliseconds deadline) {
  if (!status(ticket)) return std::nullopt;
  auto promise = std::make_shared<std::promise<WriteEvidence>>();
  auto future = promise->get_future();
  wait(ticket, deadline, _executor, [promise](WriteEvidence evidence) { promise->set_value(std::move(evidence)); });
  // The deadline answers first; the margin only covers the executor.
  if (future.wait_for(deadline + std::chrono::seconds(5)) != std::future_status::ready) return status(ticket);
  auto evidence = future.get();
  if (!evidence.ticket.valid()) return std::nullopt;
  note_unmet_barrier(evidence);
  return evidence;
}

// Macha's line when a barrier leaves a write short of its durability
// (distributed_store.cpp:753-764 at 98b816d): each copy, its place and why.
void DistributedStore::note_unmet_barrier(const WriteEvidence& evidence) const {
  if (!evidence.deferred || evidence.met || !evidence.ticket.valid() || !_logger.enabled(dannan::status::LogLevel::debug)) return;
  bool transient = false;
  std::string detail;
  for (const auto& replica : evidence.replicas) {
    if (replica.state == ReplicaState::pending) transient = true;
    detail += " replica=" + dannan::identity::to_string(replica.node).substr(0, 12);
    if (replica.durability)
      detail += " epoch=" + dannan::identity::to_string(replica.durability->epoch).substr(0, 8) + " gen=" + std::to_string(replica.durability->generation);
    const auto outcome = replica.state == ReplicaState::acknowledged ? std::string("durable") : replica.why.empty() ? std::string("unexamined") : replica.why;
    detail += " outcome=\"" + outcome + "\"";
  }
  _logger.debug("object durability quorum unavailable id=" + to_string(evidence.id) + " required=" + std::to_string(evidence.sought) +
                " durable=" + std::to_string(evidence.acknowledged()) + (transient ? " transient=yes" : " transient=no") + detail);
}

std::optional<WriteEvidence> DistributedStore::wait_complete(Ticket ticket, std::chrono::milliseconds deadline) {
  if (!status(ticket)) return std::nullopt;
  auto promise = std::make_shared<std::promise<WriteEvidence>>();
  auto future = promise->get_future();
  wait_complete(ticket, deadline, _executor, [promise](WriteEvidence evidence) { promise->set_value(std::move(evidence)); });
  if (future.wait_for(deadline + std::chrono::seconds(5)) != std::future_status::ready) return status(ticket);
  auto evidence = future.get();
  if (!evidence.ticket.valid()) return std::nullopt;
  return evidence;
}

void DistributedStore::run_barriers(const std::vector<std::shared_ptr<TicketRecord>>& records, std::chrono::milliseconds deadline) {
  // One place, and every ticket with a copy awaiting its barrier there.
  struct Place {
    NodeId node;
    DurabilityTicket ticket;
    std::vector<std::shared_ptr<TicketRecord>> records;
  };
  using PlaceKey = std::tuple<NodeId, NodeId, uint64_t, uint64_t>;
  std::map<PlaceKey, std::shared_ptr<Place>> places;
  for (const auto& record : records) {
    Lock lock(record->mutex);
    for (auto& replica : record->replicas) {
      if (replica.awaiting != Awaiting::barrier || replica.running || !replica.evidence.durability) continue;
      replica.running = true;
      const auto& ticket = *replica.evidence.durability;
      auto& place = places[{replica.evidence.node, ticket.epoch, ticket.domain, ticket.backend_instance}];
      if (!place) {
        place = std::make_shared<Place>();
        place->node = replica.evidence.node;
        place->ticket = ticket;
      }
      place->ticket.generation = std::max(place->ticket.generation, ticket.generation);
      place->records.push_back(record);
    }
  }
  // A refusal because a peer's process or backend changed means "your ticket
  // is dead", not "your bytes are gone" (discipline 1): asked again with the
  // ids, the peer answers from disk with fresh tickets.
  const auto transient = [](ReplicaRecord& replica, const std::string& why) {
    replica.evidence.why = "awaiting barrier: " + why;
    replica.running = false;
  };
  const auto refuse = [](ReplicaRecord& replica, BarrierLoss loss, const std::string& why) {
    replica.evidence.state = ReplicaState::refused;
    replica.evidence.loss = loss;
    replica.evidence.why = why;
    replica.awaiting = Awaiting::none;
    replica.running = false;
  };
  const auto each = [](const std::vector<std::shared_ptr<TicketRecord>>& tickets, const NodeId& node, const auto& change) {
    for (const auto& record : tickets) update(record, node, change);
  };
  // Remote barriers go out first, so the local one's batch window does not
  // serialise them.
  std::map<NodeId, NodeRecord> peers;
  for (const auto& peer : _context.nodes().roster()) peers.emplace(peer.id, peer);
  for (const auto& [key, place] : places) {
    if (place->node == _self) continue;
    const auto found = peers.find(place->node);
    if (found == peers.end()) {
      each(place->records, place->node, [&](ReplicaRecord& replica) { refuse(replica, BarrierLoss::unknown_node, "unknown node"); });
      continue;
    }
    const auto peer = found->second;
    message::BarrierRequest request{place->ticket, {}};
    _context.routes().call(peer, message::object_durability_barrier, message::encode_barrier(request), FrameType::loader, deadline, _executor,
                           [this, weak = std::weak_ptr<Shared>(_shared), asked = place, peer, deadline, transient, refuse, each](CallOutcome outcome) {
                             if (outcome.reply && outcome.reply->type == dannan::net::message::ok) {
                               each(asked->records, asked->node, [](ReplicaRecord& replica) { acknowledge(replica); });
                               return;
                             }
                             if (!refused_with(outcome, 409)) {
                               const auto why = outcome_text(outcome);
                               each(asked->records, asked->node, [&](ReplicaRecord& replica) { transient(replica, why); });
                               return;
                             }
                             const auto shared = weak.lock();
                             if (!shared) return;
                             ReadLock alive(shared->alive);
                             if (!shared->store) {
                               each(asked->records, asked->node, [](ReplicaRecord& replica) { replica.running = false; });
                               return;
                             }
                             // One probe with every id that named this place, as many as a
                             // request may carry; once every part has answered, Macha's line
                             // (distributed_store.cpp:725 at 98b816d) for this peer.
                             struct Rederived {
                               std::atomic<size_t> reasserted{0};
                               std::atomic<size_t> absent{0};
                               std::atomic<size_t> left{0};
                             };
                             auto rederived = std::make_shared<Rederived>();
                             rederived->left = (asked->records.size() + message::max_barrier_probe - 1) / message::max_barrier_probe;
                             const auto part_done = [this, rederived] {
                               if (rederived->left.fetch_sub(1) != 1) return;
                               _logger.info("object durability re-derived after incarnation change reasserted=" + std::to_string(rederived->reasserted.load()) +
                                            " absent=" + std::to_string(rederived->absent.load()) + " peers=1");
                             };
                             for (size_t start = 0; start < asked->records.size(); start += message::max_barrier_probe) {
                               const auto count = std::min<size_t>(message::max_barrier_probe, asked->records.size() - start);
                               std::vector<std::shared_ptr<TicketRecord>> part(asked->records.begin() + static_cast<std::ptrdiff_t>(start),
                                                                               asked->records.begin() + static_cast<std::ptrdiff_t>(start + count));
                               message::BarrierRequest probe{asked->ticket, {}};
                               for (const auto& record : part) probe.probe.push_back(record->id);
                               _context.routes().call(peer, message::object_durability_barrier, message::encode_barrier(probe), FrameType::loader, deadline,
                                                      _executor, [part, node = asked->node, transient, refuse, each, rederived, part_done](CallOutcome answer) {
                                                        if (!answer.reply || answer.reply->type != dannan::net::message::ok) {
                                                          const auto why = outcome_text(answer);
                                                          each(part, node, [&](ReplicaRecord& replica) { transient(replica, "probe: " + why); });
                                                          part_done();
                                                          return;
                                                        }
                                                        try {
                                                          const auto fresh = message::decode_probe_answer(answer.reply->payload);
                                                          const std::map<ObjectId, DurabilityTicket> present(fresh.present.begin(), fresh.present.end());
                                                          for (const auto& record : part) {
                                                            const auto held = present.find(record->id);
                                                            // Definitive when absent: the peer no longer holds it. Put it again.
                                                            (held != present.end() ? rederived->reasserted : rederived->absent).fetch_add(1);
                                                            update(record, node, [&](ReplicaRecord& replica) {
                                                              if (held != present.end())
                                                                acknowledge(replica, held->second);
                                                              else
                                                                refuse(replica, BarrierLoss::absent_on_peer, "absent on the peer after its restart");
                                                            });
                                                          }
                                                        } catch (const std::exception& error) {
                                                          const std::string why = error.what();
                                                          each(part, node, [&](ReplicaRecord& replica) { transient(replica, "probe: " + why); });
                                                        }
                                                        part_done();
                                                      });
                             }
                           });
  }
  for (const auto& [key, place] : places) {
    if (place->node != _self) continue;
    try {
      _data.durability_barrier(place->ticket, DurabilityUrgency::batchable);
      each(place->records, place->node, [](ReplicaRecord& replica) { acknowledge(replica); });
    } catch (const std::exception& error) {
      // A stale ticket, or a barrier that failed: either way the answer comes from
      // disk, no call, and heals (the maintainer, 2026-11-14: "healing is better", as Macha's
      // re-derivation at f0597a4, distributed_store.cpp:606-646). Only another
      // process's ticket is not worth a warning. A copy that cannot be re-derived is
      // absent, and the object is put again.
      if (place->ticket.epoch == _context.identity().durability_epoch() || !dynamic_cast<const StaleDurabilityTicket*>(&error))
        _logger.warn("local storage durability barrier failed: " + std::string(error.what()));
      for (const auto& record : place->records) {
        const auto fresh = _data.reassert_durable(record->id, place->ticket);
        update(record, place->node, [&](ReplicaRecord& replica) {
          if (fresh)
            acknowledge(replica, fresh);
          else
            refuse(replica, BarrierLoss::absent_here, "absent here: not re-derived from disk");
        });
      }
    }
  }
}

void DistributedStore::note_acknowledged(const ObjectId& id, const NodeId& node) {
  std::vector<std::shared_ptr<TicketRecord>> records;
  {
    Lock lock(_shared->mutex);
    for (const auto& [serial, record] : _shared->tickets)
      if (record->id == id) records.push_back(record);
  }
  for (const auto& record : records)
    update(record, node, [](ReplicaRecord& replica) {
      if (replica.evidence.state == ReplicaState::pending && replica.awaiting == Awaiting::barrier) return;
      if (replica.evidence.state != ReplicaState::acknowledged) replica.evidence.repaired = true;
      acknowledge(replica);
    });
}

void DistributedStore::on_roster(const dannan::RosterChange& change) {
  if (change.kind == dannan::RosterChange::Kind::left) return;
  // A member a durable write found dead is back: its ticket brings it the
  // object.
  std::vector<std::shared_ptr<TicketRecord>> records;
  {
    Lock lock(_shared->mutex);
    for (const auto& [serial, record] : _shared->tickets) records.push_back(record);
  }
  for (const auto& record : records) {
    bool due = false;
    {
      Lock lock(record->mutex);
      for (auto& replica : record->replicas)
        if (replica.evidence.node == change.node.id && replica.awaiting == Awaiting::dead && !replica.running) {
          replica.running = true;
          due = true;
        }
    }
    if (due)
      submit([this, id = record->id, node = change.node, record] {
        catch_up(id, node);
        update(record, node.id, [](ReplicaRecord& replica) { replica.running = false; });
      });
  }
}

void DistributedStore::catch_up(const ObjectId& id, const NodeRecord& node) {
  std::optional<Bytes> bytes = _data.get(id);
  if (!bytes) {
    ReadOptions options;
    options.frame = FrameType::speculative;
    auto read = read_impl(id, options);
    if (read.object) bytes = read.object->bytes;
  }
  if (!bytes) {
    _logger.debug("catch-up found no copy id=" + to_string(id));
    return;
  }
  if (put_on(node, id, *bytes, FrameType::speculative)) note_acknowledged(id, node.id);
}

// ---- Prompt replication ----------------------------------------------------

DistributedStore::PromptReplicationStats DistributedStore::prompt_replication_stats() const {
  PromptReplicationStats out;
  {
    Lock lock(_prompt_mutex);
    out.queued = _prompt_queue.size();
  }
  out.copies = _prompt_copies.load(std::memory_order_relaxed);
  out.failures = _prompt_failures.load(std::memory_order_relaxed);
  out.skipped_no_room = _prompt_skipped_no_room.load(std::memory_order_relaxed);
  out.dropped = _prompt_dropped.load(std::memory_order_relaxed);
  return out;
}

void DistributedStore::time_advanced() { _prompt_wake.notify_all(); }

void DistributedStore::queue_prompt_replication(const ObjectId& id) {
  {
    Lock lock(_prompt_mutex);
    if (!_prompt_queued.insert(id).second) return;
    _prompt_queue.push_back(id);
  }
  _prompt_wake.notify_one();
}

void DistributedStore::prompt_replication_loop(std::stop_token stop) {
  // One object at a time: the local copy is pushed to the first owner that
  // lacks it, as speculative DATA work. Copies beyond the second are
  // repair's; this loop only closes the single-copy window. Repair is the
  // guarantee, so it sends only where there is room and gives up on an
  // object after a few refusals.
  struct Retry {
    ObjectId id;
    dannan::contract::TimePoint due;
    unsigned attempts = 0;
  };
  constexpr unsigned max_attempts = 5;
  std::deque<Retry> retry;
  while (!stop.stop_requested()) {
    std::optional<ObjectId> id;
    unsigned attempts = 0;
    {
      Lock lock(_prompt_mutex);
      const auto ready = [&]() DANNAN_REQUIRES(_prompt_mutex) { return !_prompt_queue.empty() || (!retry.empty() && retry.front().due <= _time.now()); };
      while (!stop.stop_requested() && !ready()) {
        if (retry.empty()) {
          _prompt_wake.wait(lock.native(), stop, ready);
        } else {
          const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(retry.front().due - _time.now());
          _prompt_wake.wait_for(lock.native(), stop, std::clamp(left, std::chrono::milliseconds(1), std::chrono::milliseconds(1000)), ready);
        }
      }
      if (stop.stop_requested()) return;
      if (!_prompt_queue.empty()) {
        id = _prompt_queue.front();
        _prompt_queue.pop_front();
        _prompt_queued.erase(*id);
      }
    }
    if (!id) {
      id = retry.front().id;
      attempts = retry.front().attempts;
      retry.pop_front();
    }
    const auto retry_later = [&] {
      _prompt_failures.fetch_add(1, std::memory_order_relaxed);
      if (attempts + 1 >= max_attempts) {
        _prompt_dropped.fetch_add(1, std::memory_order_relaxed);
        return;  // repair's to finish
      }
      // 30 s, 60 s, 120 s, 240 s at Macha's first delay.
      retry.push_back({*id, _time.now() + _options.prompt_retry * (1u << attempts), attempts + 1});
      std::sort(retry.begin(), retry.end(), [](const Retry& a, const Retry& b) { return a.due < b.due; });
    };
    try {
      if (!_data.has(*id)) continue;  // gone, or never local
      const auto nodes = ranked(*id);
      const size_t target = std::min(_options.replication, nodes.size());
      if (target < 2) continue;
      const uint64_t room_needed = std::max<uint64_t>(_options.extent_size, 1);
      size_t holders = 1;  // the local copy
      bool skipped_full = false;
      std::optional<NodeRecord> destination;
      for (const auto& node : nodes) {
        if (node.id == _self) continue;
        if (has_on(node, *id)) {
          if (++holders >= 2) break;
          continue;
        }
        // Room as the node advertises it; a refused put wastes the link.
        const auto capacity = node.advert_u64(capacity_advert).value_or(0);
        const auto used = node.advert_u64(used_advert).value_or(0);
        if (capacity && used + room_needed > capacity) {
          skipped_full = true;
          continue;
        }
        if (!destination) destination = node;
      }
      if (holders >= 2) continue;
      if (!destination) {
        if (skipped_full) {
          _prompt_skipped_no_room.fetch_add(1, std::memory_order_relaxed);
          _prompt_dropped.fetch_add(1, std::memory_order_relaxed);
        }
        continue;
      }
      std::optional<Bytes> data;
      {
        std::optional<DataAdmission> admitted;
        if (_options.prompt_read_admission) {
          admitted = admit_data(FrameType::speculative, _options.extent_size, no_caller_deadline);
          if (!admitted) throw std::runtime_error("DATA resource admission busy or stopping");
        }
        data = _data.get(*id);
      }
      if (!data) continue;
      if (put_on(*destination, *id, *data, FrameType::speculative)) {
        _prompt_copies.fetch_add(1, std::memory_order_relaxed);
        _context.events().notify(_storage_event);
        note_acknowledged(*id, destination->id);
      } else {
        retry_later();
      }
    } catch (const std::exception& error) {
      _logger.debug("prompt replication id=" + to_string(*id) + " error=" + error.what());
      retry_later();
    }
    if (retry.size() > 4096) {
      retry.pop_front();
      _prompt_dropped.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

// ---- Reads -----------------------------------------------------------------

ReadResult DistributedStore::read(const ObjectId& id, ReadOptions options) { return read_impl(id, options); }

void DistributedStore::read(const ObjectId& id, ReadOptions options, boost::asio::any_io_executor executor, ReadDone done) {
  auto job = [this, id, options, executor, done]() {
    auto result = read_impl(id, options);
    boost::asio::post(executor, [done, result = std::move(result)] { done(result); });
  };
  if (!submit(std::move(job))) boost::asio::post(executor, [done = std::move(done)] { done(ReadResult{}); });
}

std::optional<Bytes> DistributedStore::get(const ObjectId& id, ReadOptions options) {
  auto result = read_impl(id, options);
  if (!result.object) return std::nullopt;
  return result.object->bytes;
}

ReadResult DistributedStore::read_impl(const ObjectId& id, const ReadOptions& options) {
  const auto until = _time.now() + options.deadline;
  auto result = read_stages(id, options);
  if (result.object)
    result.end = ReadEnd::served;
  else if (result.answer == FetchAnswer::not_found)
    result.end = ReadEnd::not_found;
  else if (options.cancelled && options.cancelled->load(std::memory_order_relaxed))
    result.end = ReadEnd::cancelled;
  else if (options.deadline.count() > 0 && _time.now() >= until)
    result.end = ReadEnd::timed_out;
  else
    result.end = ReadEnd::unavailable;
  return result;
}

ReadResult DistributedStore::read_stages(const ObjectId& id, const ReadOptions& options) {
  const bool foreground = options.frame == FrameType::foreground;
  const bool interactive = foreground || options.frame == FrameType::read_ahead;
  // Foreground demand is recorded before any storage access, so the first
  // read already holds back lower classes.
  if (foreground) _activity.note(options.frame, 0);
  // Macha's read lines (distributed_store.cpp:1731-1811 at 98b816d): a slow
  // foreground read by source, and every read at trace.
  const auto started = std::chrono::steady_clock::now();
  const auto said = [&](std::string_view source, size_t bytes, bool ok) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (foreground && elapsed >= std::chrono::milliseconds(250))
      _logger.debug("playback object read source=" + std::string(source) + " stripe=" + std::to_string(options.stripe) + " id=" + to_string(id) +
                    " result=" + std::to_string(ok ? 1 : 0) + " bytes=" + std::to_string(bytes) + " elapsed_ms=" + std::to_string(elapsed.count()));
    if (_logger.enabled(dannan::status::LogLevel::all)) {
      std::string line = "DIAG object-get id=" + to_string(id) + " source=" + std::string(source);
      if (source == "remote") line += " result=" + std::to_string(ok ? 1 : 0);
      _logger.trace(line + " bytes=" + std::to_string(bytes) + " ms=" + std::to_string(elapsed.count()));
    }
  };
  // A caller's deadline is one time across the local, cache and remote stages,
  // as Macha's is: a stage reached past it goes no further.
  const bool own_deadline = options.deadline.count() > 0;
  const auto until = _time.now() + options.deadline;
  const auto past_deadline = [&] { return own_deadline && _time.now() >= until; };
  const auto wait = [&] { return own_deadline ? std::optional(std::chrono::ceil<std::chrono::milliseconds>(until - _time.now())) : no_caller_deadline; };
  std::optional<Bytes> local;
  {
    const auto admitted = admit_data(options.frame, _options.extent_size, wait(), options.cancelled.get());
    if (!admitted) return {};
    local = _data.get(id);
  }
  if (_options.read_trace) _options.read_trace("local");
  if (local) {
    if (interactive) _activity.note(options.frame, local->size());
    said("owned", local->size(), true);
    return {buffer_of(std::move(*local)), ReadSource::local, _self, FetchAnswer::ok};
  }
  if (past_deadline()) return {};
  std::optional<Bytes> cached;
  {
    const auto admitted = admit_data(options.frame, _options.extent_size, wait(), options.cancelled.get());
    if (!admitted) return {};
    cached = _cache.get(id);
  }
  if (_options.read_trace) _options.read_trace("cache");
  if (cached) {
    if (interactive) _activity.note(options.frame, cached->size());
    if (should_own(id)) enqueue_local_copy(id, *cached, true);
    said("cache", cached->size(), true);
    return {buffer_of(std::move(*cached)), ReadSource::cache, _self, FetchAnswer::ok};
  }
  if (past_deadline() || (options.cancelled && options.cancelled->load(std::memory_order_relaxed))) return {};
  auto result = get_remote(id, options, interactive, own_deadline ? std::optional(until) : std::nullopt);
  if (result.object && options.frame == FrameType::read_ahead) _activity.note(options.frame, result.object->bytes.size());
  said("remote", result.object ? result.object->bytes.size() : 0, static_cast<bool>(result.object));
  return result;
}

ReadResult DistributedStore::get_remote(const ObjectId& id, const ReadOptions& options, bool opportunistic_persist,
                                        std::optional<dannan::contract::TimePoint> until) {
  // None of the caller's: no clock of its own, as Macha's. The wait ends when the
  // fetch does, and each peer's answer is under call_no_progress.
  // No deadline is the end of time.
  const auto deadline = until ? *until : options.deadline.count() > 0 ? _time.now() + options.deadline : dannan::contract::TimePoint::max();
  const bool foreground = options.frame == FrameType::foreground;
  std::shared_ptr<SharedFetch> fetch;
  bool leader = false;
  std::shared_ptr<dannan::net::CallHandle> promote;
  {
    Lock lock(_fetch_mutex);
    if (const auto found = _fetches.find(id); found != _fetches.end()) fetch = found->second.lock();
    if (fetch) {
      Lock fetch_lock(fetch->mutex);
      if (fetch->done) {
        fetch.reset();
      } else {
        ++fetch->waiters;
        // A higher class joining raises the transfer it joins.
        if (dannan::sched::frame_priority(options.frame) < dannan::sched::frame_priority(fetch->frame)) {
          fetch->frame = options.frame;
          promote = fetch->handle;
        }
        if (foreground && !fetch->foreground) {
          fetch->foreground = true;
          if (fetch->active_peer && fetch->active_class == ReplicaWorkClass::speculative) {
            _selector.promoted(*fetch->active_peer);
            fetch->active_class = ReplicaWorkClass::foreground;
          }
        }
        if (opportunistic_persist) fetch->persist = true;
      }
    }
    if (!fetch) {
      fetch = std::make_shared<SharedFetch>(id);
      Lock fetch_lock(fetch->mutex);
      fetch->waiters = 1;
      fetch->frame = options.frame;
      fetch->foreground = foreground;
      fetch->persist = opportunistic_persist;
      fetch->stripe = options.stripe;
      _fetches[id] = fetch;
      leader = true;
    }
  }
  if (promote && (options.frame == FrameType::foreground || options.frame == FrameType::read_ahead)) promote->promote(options.frame);
  if (leader) {
    // A fetch that cannot be scheduled finds nothing, as Macha's
    // (distributed_store.cpp:1694-1700 at 98b816d).
    const auto unscheduled = [&](const std::string& why) {
      _logger.debug("remote object scheduling " + to_string(id) + ": " + why);
      {
        Lock lock(fetch->mutex);
        fetch->done = true;
      }
      fetch->changed.notify_all();
    };
    try {
      auto owners = ranked(id);
      if (owners.size() > _options.replication) owners.resize(_options.replication);
      {
        Lock lock(fetch->mutex);
        for (const auto& node : owners) fetch->preferred.insert(node.id);
        for (auto& node : owners)
          if (node.id != _self) fetch->candidates.push_back(std::move(node));
      }
      fetch_next(fetch);
    } catch (const std::exception& error) {
      unscheduled(error.what());
    } catch (...) {
      unscheduled("unknown error");
    }
  }

  ReadResult out;
  std::shared_ptr<dannan::net::CallHandle> abandon;
  {
    Lock lock(fetch->mutex);
    while (!fetch->done && !_stopping.load() && _time.now() < deadline && !(options.cancelled && options.cancelled->load(std::memory_order_relaxed)))
      fetch->changed.wait_for(lock.native(), deadline == dannan::contract::TimePoint::max()
                                                 ? std::chrono::nanoseconds(look_at_the_clock)
                                                 : std::min<std::chrono::nanoseconds>(deadline - _time.now(), look_at_the_clock));
    --fetch->waiters;
    if (!fetch->done) {
      // The transfer belongs to the object, not to any one reader: it is
      // abandoned only when nobody waits for it.
      if (!fetch->waiters) abandon = fetch->handle;
    } else {
      out.object = fetch->result;
      out.answer = fetch->answer;
      out.served_by = fetch->served_by;
      out.source = out.object ? ReadSource::remote : ReadSource::none;
    }
  }
  if (abandon) abandon->cancel();
  if (out.object && foreground) _activity.note(options.frame, out.object->bytes.size());
  return out;
}

void DistributedStore::fetch_next(const std::shared_ptr<SharedFetch>& fetch) {
  NodeRecord target;
  ReplicaWorkClass work = ReplicaWorkClass::speculative;
  FrameType frame = FrameType::speculative;
  bool conclude = false;
  bool not_found = false;
  {
    Lock lock(fetch->mutex);
    if (fetch->done) return;
    if (!fetch->waiters) {
      conclude = true;
    } else {
      // Objects may live on fallback nodes when an owner is full, and on old
      // owners while the membership changes.
      if (fetch->candidates.empty() && !fetch->fallback_phase) {
        fetch->fallback_phase = true;
        for (auto& node : ranked(fetch->id))
          if (node.id != _self && !fetch->preferred.contains(node.id)) fetch->candidates.push_back(std::move(node));
      }
      if (fetch->candidates.empty()) {
        conclude = true;
        // Not found needs every candidate asked and every one saying so.
        not_found = fetch->asked && fetch->all_absent;
      } else {
        work = fetch->foreground ? ReplicaWorkClass::foreground : ReplicaWorkClass::speculative;
        const auto ordered =
            _selector.order(fetch->candidates, fetch->stripe + fetch->attempt, work, [this](const NodeId& node) { return _context.nodes().latency(node); });
        target = ordered.front();
        std::erase_if(fetch->candidates, [&](const NodeRecord& node) { return node.id == target.id; });
        ++fetch->attempt;
        fetch->active_peer = target.id;
        fetch->active_class = work;
        frame = fetch->frame;
        _selector.started(target.id, work);
      }
    }
  }
  if (conclude) {
    finish_fetch(fetch, {}, not_found ? FetchAnswer::not_found : FetchAnswer::try_later, std::nullopt);
    return;
  }
  const auto started = Clock::now();
  auto handle = _context.routes().call(
      target, message::get_object, message::encode_id(fetch->id), frame, _options.object_transfer_call, _executor,
      [weak = std::weak_ptr<Shared>(_shared), fetch, target, work, started](CallOutcome outcome) {
        const auto shared = weak.lock();
        if (shared) {
          ReadLock alive(shared->alive);
          if (shared->store) {
            shared->store->fetched(fetch, target, work, started, std::move(outcome));
            return;
          }
        }
        {
          Lock lock(fetch->mutex);
          fetch->done = true;
        }
        fetch->changed.notify_all();
      },
      peer_call_watch());
  Lock lock(fetch->mutex);
  if (!fetch->done) fetch->handle = std::move(handle);
}

void DistributedStore::fetched(const std::shared_ptr<SharedFetch>& fetch, const NodeRecord& target, ReplicaWorkClass, Clock::time_point started,
                               CallOutcome outcome) {
  ObjectData data;
  bool absent = false;
  if (outcome.reply && outcome.reply->type == message::object_reply) {
    try {
      data = take_object_reply(std::move(*outcome.reply), fetch->id);
      // A fetch teaches the network estimate, as Macha's (ds:1427-1428 at f0597a4).
      if (data) note_network(data->bytes.size(), Clock::now() - started);
    } catch (const std::exception& error) {
      _logger.debug("object read: " + std::string(error.what()));
    }
  } else {
    absent = refused_with(outcome, 404);
    if (!outcome.reply) _logger.debug("object read: " + outcome_text(outcome));
  }
  {
    Lock lock(fetch->mutex);
    _selector.finished(target.id, fetch->active_class, data ? data->bytes.size() : 0, Clock::now() - started, static_cast<bool>(data));
    fetch->active_peer.reset();
    fetch->handle.reset();
    if (!data) {
      ++fetch->asked;
      fetch->all_absent = fetch->all_absent && absent;
    }
  }
  if (data) {
    finish_fetch(fetch, std::move(data), FetchAnswer::ok, target.id);
    return;
  }
  fetch_next(fetch);
}

void DistributedStore::finish_fetch(const std::shared_ptr<SharedFetch>& fetch, ObjectData result, FetchAnswer answer, std::optional<NodeId> served_by) {
  bool persist = false;
  {
    Lock lock(fetch->mutex);
    if (fetch->done) return;
    persist = result && fetch->persist;
  }
  // The local copy is queued before any reader is told, so a reader that
  // then waits for the copies settled waits for this one too.
  if (persist) enqueue_local_copy(fetch->id, result->bytes, should_own(fetch->id));
  {
    Lock lock(fetch->mutex);
    if (fetch->done) return;
    fetch->done = true;
    fetch->result = result;
    fetch->answer = answer;
    fetch->served_by = served_by;
    fetch->handle.reset();
  }
  fetch->changed.notify_all();
  {
    Lock lock(_fetch_mutex);
    if (const auto found = _fetches.find(fetch->id); found != _fetches.end() && found->second.lock() == fetch) _fetches.erase(found);
  }
}

void DistributedStore::enqueue_local_copy(const ObjectId& id, std::span<const uint8_t> data, bool promote) {
  const bool cache = _cache.enabled();
  if (!cache && !promote) return;
  // Opportunistic persistence must not hold back a reader: past the bound
  // the copy is dropped and repair converges later.
  {
    Lock lock(_local_copy_mutex);
    if (data.size() > _options.local_copy_bytes || _local_copy_bytes + data.size() > _options.local_copy_bytes) {
      _logger.debug("opportunistic persistence skipped object=" + to_string(id) + " reason=queue_full queued_bytes=" + std::to_string(_local_copy_bytes));
      return;
    }
    _local_copy_bytes += data.size();
    _local_copies.push_back({id, Bytes(data.begin(), data.end()), promote});
  }
  _local_copy_wake.notify_one();
}

void DistributedStore::wait_local_copies_settled() {
  Lock lock(_local_copy_mutex);
  _local_copy_settled.wait(lock.native(), [&]() DANNAN_REQUIRES(_local_copy_mutex) { return _local_copies.empty() && !_local_copy_writing; });
}

void DistributedStore::local_writer_loop(std::stop_token stop) {
  while (true) {
    LocalCopy job;
    {
      Lock lock(_local_copy_mutex);
      _local_copy_wake.wait(lock.native(), stop, [&]() DANNAN_REQUIRES(_local_copy_mutex) { return !_local_copies.empty(); });
      if (_local_copies.empty()) return;
      job = std::move(_local_copies.front());
      _local_copies.pop_front();
      _local_copy_bytes -= job.data.size();
      _local_copy_writing = true;
    }
    try {
      const auto admitted = admit_data(FrameType::speculative, job.data.size(), no_caller_deadline);
      if (!admitted) {
        if (!stop.stop_requested()) _logger.debug("opportunistic persistence skipped object=" + to_string(job.id) + " reason=data_credit");
        throw Skipped{};
      }
      const bool cache = _cache.enabled();
      bool cached = false;
      if (cache) {
        cached = _cache.put(job.id, job.data);
        if (!cached) _logger.debug("opportunistic persistence skipped object=" + to_string(job.id) + " reason=cache_put_failed");
      }
      // With a persistent cache, fetches land on the cache device and
      // promotion is left to idle maintenance. If the cache write fails (or
      // there is no cache), promote here rather than fetch again.
      if (job.promote && (!cache || !cached)) (void)_data.put(job.id, job.data);
    } catch (const Skipped&) {
    } catch (const std::exception& error) {
      _logger.debug("opportunistic persistence failed object=" + to_string(job.id) + ": " + error.what());
    }
    {
      Lock lock(_local_copy_mutex);
      _local_copy_writing = false;
    }
    _local_copy_settled.notify_all();
    if (stop.stop_requested()) return;
  }
}

// ---- Presence and claims ---------------------------------------------------

bool DistributedStore::has_on(const NodeRecord& target, const ObjectId& id) {
  // A presence probe, not a claim: no decrypt here, an index lookup.
  if (target.id == _self) return _data.has(id);
  const auto outcome = _context.routes().call_and_wait(target, message::have_object, message::encode_id(id), FrameType::speculative, _options.control_call);
  if (!outcome.reply || outcome.reply->type != message::bool_reply) return false;
  try {
    return message::decode_bool(outcome.reply->payload);
  } catch (const std::exception&) {
    return false;
  }
}

std::map<NodeId, std::map<ObjectId, bool>> DistributedStore::batched_have_objects(const std::map<NodeId, NodeRecord>& node_info,
                                                                                  const std::map<NodeId, std::vector<ObjectId>>& ids_by_node) {
  std::map<NodeId, std::map<ObjectId, bool>> results;
  // This node: an index lookup, no call.
  if (const auto self = ids_by_node.find(_self); self != ids_by_node.end()) {
    auto& out = results[_self];
    for (const auto& id : self->second) out[id] = _data.has(id);
  }
  const size_t batch = std::max<size_t>(1, _options.retention_check_batch_size);
  std::vector<Request> requests;
  std::vector<std::pair<NodeId, std::vector<ObjectId>>> chunks;
  for (const auto& [node, ids] : ids_by_node) {
    if (node == _self) continue;
    const auto found = node_info.find(node);
    for (size_t offset = 0; offset < ids.size(); offset += batch) {
      std::vector<ObjectId> chunk(ids.begin() + static_cast<std::ptrdiff_t>(offset),
                                  ids.begin() + static_cast<std::ptrdiff_t>(std::min(ids.size(), offset + batch)));
      if (found == node_info.end()) {
        for (const auto& id : chunk) results[node].emplace(id, false);
        continue;
      }
      requests.push_back({found->second, message::have_objects, message::encode_ids(chunk), FrameType::loader});
      chunks.emplace_back(node, std::move(chunk));
    }
  }
  const auto outcomes = call_all(_context.routes(), _executor, std::move(requests), _options.retention_check_concurrency, _options.presence_call);
  for (size_t i = 0; i < chunks.size(); ++i) {
    const auto& [node, ids] = chunks[i];
    std::vector<bool> present(ids.size(), false);
    const auto& outcome = outcomes[i];
    const auto host = node_info.contains(node) ? node_info.at(node).host : std::string();
    if (outcome.reply && outcome.reply->type == message::have_objects_reply) {
      try {
        present = message::decode_presence(outcome.reply->payload, ids.size());
      } catch (const std::exception& error) {
        _logger.debug("retention presence batch peer=" + host + " error=" + error.what());
      }
    } else if (!outcome.reply) {
      _logger.debug("retention presence batch peer=" + host + " error=" + outcome_text(outcome));
    }
    auto& out = results[node];
    for (size_t j = 0; j < ids.size(); ++j) out[ids[j]] = present[j];
  }
  return results;
}

std::map<ObjectId, std::vector<NodeRecord>> DistributedStore::select_present_batched(const std::map<ObjectId, std::vector<NodeRecord>>& candidates_by_object,
                                                                                     size_t floor) {
  // For each object, its candidates in preference order until `floor` hold
  // it, the presence asked a node at a time for every object in one batch.
  std::map<ObjectId, std::vector<NodeRecord>> selected;
  std::map<ObjectId, size_t> next_index;
  std::vector<ObjectId> pending;
  for (const auto& [id, candidates] : candidates_by_object) {
    selected[id];
    next_index[id] = 0;
    if (!candidates.empty()) pending.push_back(id);
  }
  while (!pending.empty()) {
    std::map<NodeId, NodeRecord> node_info;
    std::map<NodeId, std::vector<ObjectId>> ids_by_node;
    std::vector<ObjectId> next_pending;
    for (const auto& id : pending) {
      if (selected[id].size() >= floor) continue;
      auto& index = next_index[id];
      const auto& candidates = candidates_by_object.at(id);
      if (index >= candidates.size()) continue;
      const auto& candidate = candidates[index++];
      node_info[candidate.id] = candidate;
      ids_by_node[candidate.id].push_back(id);
      next_pending.push_back(id);
    }
    if (ids_by_node.empty()) break;
    const auto results = batched_have_objects(node_info, ids_by_node);
    for (const auto& [node, ids] : ids_by_node) {
      const auto found = results.find(node);
      for (const auto& id : ids)
        if (found != results.end() && found->second.contains(id) && found->second.at(id)) selected[id].push_back(node_info.at(node));
    }
    pending = std::move(next_pending);
    std::erase_if(pending, [&](const ObjectId& id) { return selected[id].size() >= floor || next_index[id] >= candidates_by_object.at(id).size(); });
  }
  return selected;
}

bool DistributedStore::retain_on(const NodeRecord& target, contract::RetentionClass object_class, const std::vector<ObjectId>& input,
                                 const contract::RetentionDot& dot, const message::WritePrincipal& writer) {
  if (input.empty()) return true;
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (target.id == _self) {
    // A claim is checked as a write is: this node claims nothing for a
    // writer it refuses.
    if (const auto refusal = scope_refusal(writer)) {
      _logger.debug("local claim refused " + *refusal);
      return false;
    }
    // A claim says "this node holds the object": index presence, not a
    // read. The bytes were verified when put; scrub finds later corruption.
    // Under the objects' claim guard a sub-batch at a time, as the server's
    // claim is. A claim's journal is control work: a control slot, never DATA
    // credit, and none is waited for under the guard.
    const auto slot = admit_control();
    if (!slot) return false;
    const auto claim = [&](std::span<const ObjectId> part) {
      for (const auto& id : part)
        if (_data.claim_locks().deleting(id) || !(object_class == contract::RetentionClass::data ? _data.has(id) : _control.has(id))) return false;
      _claims.retain_batch(object_class, std::vector<ObjectId>(part.begin(), part.end()), dot);
      return true;
    };
    return _data.claim_locks().in_batches(ids, claim);
  }
  const auto outcome = _context.routes().call_and_wait(target, message::retain_objects, message::encode_retain({object_class, dot, ids, writer}),
                                                       FrameType::loader, _options.control_call);
  if (!outcome.reply || outcome.reply->type != dannan::net::message::ok) {
    _logger.debug("retention claim peer=" + target.host + " error=" + outcome_text(outcome));
    return false;
  }
  return true;
}

std::vector<ObjectId> DistributedStore::retain_data(const std::vector<ObjectId>& input, const contract::RetentionDot& dot, const WriteScope& scope) {
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<ObjectId> unheld;
  if (ids.empty()) return unheld;
  const auto writer = writer_of(scope.principal, scope.prefix);
  // Claims sought per object. A holder that is away is not claimed on: its
  // own head and the deletion grace protect its copy until it returns.
  const size_t floor = std::max<size_t>(1, _options.write_copies);
  const auto started = std::chrono::steady_clock::now();
  const auto since_ms = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(); };
  std::map<ObjectId, std::vector<NodeRecord>> candidates_by_object;
  for (const auto& id : ids) candidates_by_object.emplace(id, ranked(id));
  auto selected_by_object = select_present_batched(candidates_by_object, floor);
  const auto scan_ms = since_ms();

  // An object found on no node present has nothing to copy from and nobody
  // to claim on: it is unheld, and nothing more is asked about it. One found
  // on fewer nodes than sought is placed again first.
  std::set<ObjectId> absent;
  std::vector<ObjectId> short_ids;
  for (const auto& id : ids) {
    const auto present = selected_by_object[id].size();
    if (!present)
      absent.insert(id);
    else if (present < floor)
      short_ids.push_back(id);
  }
  if (!short_ids.empty()) {
    std::map<ObjectId, std::vector<NodeRecord>> rescan;
    for (const auto& id : short_ids) {
      ReadOptions read_options;
      read_options.frame = FrameType::speculative;
      auto data = get(id, read_options);
      if (!data || !put(id, *data)) continue;
      auto candidates = ranked(id);
      candidates_by_object[id] = candidates;
      rescan.emplace(id, std::move(candidates));
    }
    for (auto& [id, selected] : select_present_batched(rescan, floor)) selected_by_object[id] = std::move(selected);
  }

  std::map<NodeId, NodeRecord> node_info;
  std::map<NodeId, std::vector<ObjectId>> batches;
  for (const auto& id : ids)
    for (const auto& candidate : selected_by_object[id]) {
      node_info[candidate.id] = candidate;
      batches[candidate.id].push_back(id);
    }
  // One claim per selected node, all at once: claims are independent, so a claim costs
  // the slowest holder, not their sum (Macha's std::async a node, distributed_store.cpp
  // :924-954 at f0597a4). The futures join before anything they read goes.
  std::map<ObjectId, std::set<NodeId>> claimed;
  std::vector<std::pair<NodeId, std::future<bool>>> running;
  running.reserve(batches.size());
  for (const auto& [node, batch] : batches)
    running.emplace_back(node, std::async(std::launch::async, [this, &target = node_info.at(node), &batch, &dot, &writer] {
                           return retain_on(target, contract::RetentionClass::data, batch, dot, writer);
                         }));
  for (auto& [node, done] : running)
    if (done.get())
      for (const auto& id : batches.at(node)) claimed[id].insert(node);

  // Where a slow claim spends its time, as Macha's says (distributed_store.cpp:878-895
  // at 98b816d): the presence scan, objects placed again, per-object fallbacks.
  size_t fallback_claims = 0;
  size_t under_claimed = 0;
  for (const auto& id : ids) {
    auto& successful = claimed[id];
    if (successful.size() >= floor) continue;
    if (absent.contains(id)) {
      ++under_claimed;
      unheld.push_back(id);
      continue;
    }
    for (const auto& candidate : candidates_by_object[id]) {
      if (successful.size() >= floor) break;
      if (successful.contains(candidate.id)) continue;
      ++fallback_claims;
      if (has_on(candidate, id) && retain_on(candidate, contract::RetentionClass::data, {id}, dot, writer)) successful.insert(candidate.id);
    }
    if (successful.size() < floor) ++under_claimed;
    if (successful.empty()) unheld.push_back(id);
  }
  if (const auto total = since_ms(); total >= 250)
    _logger.debug("DATA retention barrier ids=" + std::to_string(ids.size()) + " nodes=" + std::to_string(batches.size()) +
                  " total_ms=" + std::to_string(total) + " scan_ms=" + std::to_string(scan_ms) + " short=" + std::to_string(short_ids.size()) +
                  " fallback_claims=" + std::to_string(fallback_claims) + " under_claimed=" + std::to_string(under_claimed));
  return unheld;
}

// The pull's transfers teach the network estimate, as Macha's (ds:2292-2295 at f0597a4).
bool DistributedStore::ensure_control_local(const ObjectId& id) {
  return _control_fetch.pull(id, [this](uint64_t bytes, std::chrono::steady_clock::duration elapsed) { note_network(bytes, elapsed); });
}

bool DistributedStore::offer_control_on(const NodeRecord& target, const std::vector<ObjectId>& ids, FrameType frame, ControlOffer& out) {
  // Ask the peer what it lacks before sending anything, so the cost scales
  // with what changed, not with the graph. A peer that cannot answer is sent
  // everything.
  std::vector<ObjectId> missing;
  constexpr size_t probe_batch = 4096;
  for (size_t offset = 0; offset < ids.size(); offset += probe_batch) {
    const auto end = std::min(ids.size(), offset + probe_batch);
    const std::span<const ObjectId> chunk(ids.data() + offset, end - offset);
    const auto outcome =
        _context.routes().call_and_wait(target, message::have_control_objects, message::encode_ids(chunk), FrameType::control, _options.control_call);
    try {
      if (!outcome.reply || outcome.reply->type != message::have_control_objects_reply) throw std::runtime_error(outcome_text(outcome));
      const auto present = message::decode_presence(outcome.reply->payload, chunk.size());
      for (size_t i = 0; i < chunk.size(); ++i)
        if (!present[i]) missing.push_back(chunk[i]);
    } catch (const std::exception& error) {
      _logger.debug("CONTROL presence probe peer=" + target.host + " error=" + error.what() + "; sending the whole graph");
      missing = ids;
      break;
    }
  }
  if (missing.size() != ids.size())
    _logger.debug("CONTROL graph peer=" + target.host + " referenced=" + std::to_string(ids.size()) + " missing=" + std::to_string(missing.size()));
  if (missing.empty()) return true;
  // Puts to a peer are pipelined within a quarter of what one connection
  // holds, since the graph shares the lane with health and commits.
  const size_t window = std::max<size_t>(1, std::min(dannan::net::max_pending_replies, dannan::net::max_outbound_messages) / 4);
  std::vector<Request> requests;
  std::vector<size_t> sizes;
  for (const auto& id : missing) {
    // Bytes are read only for what is sent.
    const auto bytes = _control.get(id);
    if (!bytes) return false;
    sizes.push_back(bytes->size());
    requests.push_back({target, message::put_control_object, message::encode_object(id, *bytes), frame});
  }
  const auto put_started = Clock::now();
  const auto outcomes = call_all(_context.routes(), _executor, std::move(requests), window, _options.metadata_call);
  bool ok = true;
  uint64_t bytes_sent = 0;
  for (size_t i = 0; i < outcomes.size(); ++i) {
    if (outcomes[i].reply && outcomes[i].reply->type == dannan::net::message::ok) {
      bytes_sent += sizes[i];
      ++out.objects_sent;
    } else {
      _logger.debug("CONTROL object store peer=" + target.host + " error=" + outcome_text(outcomes[i]));
      ok = false;
    }
  }
  out.bytes_sent += bytes_sent;
  // One sample for the offer: its sends shared the link (Macha's ds:2197-2200).
  if (bytes_sent) note_network(bytes_sent, Clock::now() - put_started);
  return ok;
}

ControlOffer DistributedStore::offer_control(const std::vector<ObjectId>& input) {
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  ControlOffer out;
  // What this node does not hold is not offered.
  if (std::any_of(ids.begin(), ids.end(), [&](const ObjectId& id) { return !_control.has(id); })) return out;
  for (const auto& target : _context.nodes().active()) {
    if (target.id == _self) {
      ++out.held_by;
      continue;
    }
    try {
      if (ids.empty() || offer_control_on(target, ids, FrameType::speculative, out)) ++out.held_by;
    } catch (const std::exception& error) {
      _logger.debug("CONTROL offer peer=" + target.host + " error=" + error.what());
    }
  }
  return out;
}

bool DistributedStore::retain_control(const std::vector<ObjectId>& input, const contract::RetentionDot& dot, ClaimScope scope, const WriteScope& by) {
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (ids.empty()) return true;
  const auto writer = writer_of(by.principal, by.prefix);
  const size_t sought = scope == ClaimScope::here         ? 1
                        : scope == ClaimScope::every_node ? std::numeric_limits<size_t>::max()
                                                          : std::max<size_t>(1, _options.metadata_write_copies);
  const auto started = std::chrono::steady_clock::now();
  auto active = _context.nodes().active();
  if (scope == ClaimScope::here) std::erase_if(active, [&](const NodeRecord& node) { return node.id != _self; });
  // Local first, then the nearest measured peer: this runs inside a writer's
  // commit.
  active = dannan::placement::order_commit_replicas(active, _self, [this](const NodeId& peer) { return _context.nodes().latency(peer); });
  // A claim asserts this node holds the object and peers' copies derive from
  // it. An object that cannot be made local now is not claimed here; its
  // holder's own head protects it.
  std::erase_if(ids, [&](const ObjectId& id) { return !ensure_control_local(id); });
  if (ids.empty()) return true;

  const auto put_graph_on = [&](const NodeRecord& target) {
    if (target.id == _self) {
      // Not filtered by presence: a put refreshes an existing object's age,
      // so collection cannot race a commit that reuses an old orphan.
      const auto slot = admit_control();
      if (!slot) return false;
      for (const auto& id : ids) {
        const auto bytes = _control.get(id);
        if (!bytes || !_control.put(id, *bytes)) return false;
      }
      return true;
    }
    ControlOffer sent;
    return offer_control_on(target, ids, FrameType::control, sent);
  };
  // A commit claims on a few nodes, not on the membership; background
  // control repair fans out later. The local claim is the one that must
  // hold.
  size_t retained = 0;
  size_t tried = 0;
  bool local_retained = false;
  for (const auto& candidate : active) {
    ++tried;
    if (!put_graph_on(candidate)) continue;
    if (retain_on(candidate, contract::RetentionClass::control, ids, dot, writer)) {
      ++retained;
      local_retained = local_retained || candidate.id == _self;
    }
    if (retained >= sought) break;
  }
  if (const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(); total >= 250)
    _logger.debug("CONTROL retention claim objects=" + std::to_string(ids.size()) + " sought=" + std::to_string(sought) + " tried=" + std::to_string(tried) +
                  " retained=" + std::to_string(retained) + " total_ms=" + std::to_string(total));
  return local_retained;
}

void DistributedStore::retain(contract::RetentionClass type, std::vector<ObjectId> ids, contract::RetentionDot dot, WriteScope writer, ClaimScope scope,
                              boost::asio::any_io_executor executor, RetainDone done) {
  auto job = [this, type, ids, dot, writer = std::move(writer), scope, executor, done]() {
    RetainOutcome outcome;
    try {
      if (type == contract::RetentionClass::data) {
        outcome.unheld = retain_data(ids, dot, writer);
      } else {
        outcome.held_here = retain_control(ids, dot, scope, writer);
      }
      outcome.ran = true;
    } catch (const std::exception& error) {
      outcome.why = error.what();
    }
    boost::asio::post(executor, [done, outcome = std::move(outcome)] { done(outcome); });
  };
  if (!submit(std::move(job))) {
    RetainOutcome refused;
    refused.why = "the store is stopping";
    boost::asio::post(executor, [done = std::move(done), refused = std::move(refused)] { done(refused); });
  }
}

// ---- The instant delete ----------------------------------------------------

RemoveEvidence DistributedStore::remove(const std::vector<ObjectId>& input, const contract::RetentionClock& given, const WriteScope& scope,
                                        std::chrono::milliseconds deadline) {
  RemoveEvidence evidence;
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  evidence.sought = ids;
  if (ids.empty()) {
    evidence.finished = true;
    return evidence;
  }
  // An entry with no node or no sequence observes nothing.
  contract::RetentionClock observed;
  for (const auto& [node, sequence] : given)
    if (node != NodeId{} && sequence) observed[node] = sequence;
  const auto writer = writer_of(scope.principal, scope.prefix);
  const auto call_deadline = deadline.count() > 0 ? std::min(deadline, _options.control_call) : _options.control_call;
  const auto every = [&](NodeRemoval& node, RemoveState state) {
    node.objects.clear();
    for (const auto& id : ids) node.objects.push_back({id, state, {}});
  };

  // This node first, checked as a peer checks: its scope, then its half.
  NodeRemoval local;
  local.node = _self;
  const auto decision = _context.identity().verify(writer.principal, writer.prefix);
  if (!decision.allowed) {
    local.why = decision.why;
    local.scope_generation = decision.generation;
    every(local, RemoveState::refused);
  } else if (const auto admitted =
                 admit_data(FrameType::loader, _options.extent_size, deadline.count() > 0 ? std::optional(call_deadline) : no_caller_deadline);
             !admitted) {
    local.why = "storage_busy: DATA admission busy or stopping";
    every(local, RemoveState::unreachable);
  } else {
    local.objects = remove_here({_data, _claims, _ledger, &_cache}, ids, observed);
  }
  evidence.nodes.push_back(std::move(local));

  // Every other node present: any may hold a copy or a claim (a fallback
  // that took a spilled write, a node that stopped hosting), and one that
  // holds neither answers absent.
  auto peers = _context.nodes().active();
  std::erase_if(peers, [&](const NodeRecord& node) { return node.id == _self; });
  std::sort(peers.begin(), peers.end(), [](const NodeRecord& a, const NodeRecord& b) { return a.id < b.id; });
  std::vector<Request> requests;
  struct Part {
    size_t peer = 0;
    size_t begin = 0;
    size_t end = 0;
  };
  std::vector<Part> parts;
  for (size_t p = 0; p < peers.size(); ++p)
    for (size_t begin = 0; begin < ids.size(); begin += message::max_remove_batch) {
      const auto end = std::min(ids.size(), begin + message::max_remove_batch);
      std::vector<ObjectId> part(ids.begin() + static_cast<std::ptrdiff_t>(begin), ids.begin() + static_cast<std::ptrdiff_t>(end));
      requests.push_back({peers[p], message::remove_objects, message::encode_remove({observed, std::move(part), writer}), FrameType::loader});
      parts.push_back({p, begin, end});
    }
  const auto outcomes = call_all(_context.routes(), _executor, std::move(requests), 16, call_deadline);
  std::vector<NodeRemoval> answers(peers.size());
  for (size_t p = 0; p < peers.size(); ++p) answers[p].node = peers[p].id;
  for (size_t i = 0; i < outcomes.size(); ++i) {
    auto& answer = answers[parts[i].peer];
    const auto unanswered = [&](RemoveState state, const std::string& why) {
      answer.why = why;
      for (size_t k = parts[i].begin; k < parts[i].end; ++k) answer.objects.push_back({ids[k], state, {}});
    };
    const auto& outcome = outcomes[i];
    if (!outcome.reply || outcome.reply->type != message::remove_objects_reply) {
      unanswered(RemoveState::unreachable, outcome_text(outcome));
      continue;
    }
    try {
      auto removal = message::decode_removal(outcome.reply->payload);
      if (removal.refused) {
        answer.scope_generation = removal.scope_generation;
        unanswered(RemoveState::refused, removal.why);
      } else {
        for (auto& object : removal.objects) answer.objects.push_back(std::move(object));
      }
    } catch (const std::exception& error) {
      unanswered(RemoveState::unreachable, std::string("undecodable answer: ") + error.what());
    }
  }
  for (auto& answer : answers) evidence.nodes.push_back(std::move(answer));

  // A member of an object's replication set that is not present: nothing is
  // known of its copy or its claims.
  std::map<NodeId, NodeRemoval> away;
  for (const auto& id : ids)
    for (const auto& member : replication_set(id)) {
      if (member.id == _self || std::any_of(peers.begin(), peers.end(), [&](const NodeRecord& peer) { return peer.id == member.id; })) continue;
      auto& node = away[member.id];
      node.node = member.id;
      node.why = "not present";
      node.objects.push_back({id, RemoveState::unreachable, {}});
    }
  for (auto& [node, removal] : away) evidence.nodes.push_back(std::move(removal));
  evidence.finished = true;
  return evidence;
}

void DistributedStore::remove(std::vector<ObjectId> ids, contract::RetentionClock observed, WriteScope writer, std::chrono::milliseconds deadline,
                              boost::asio::any_io_executor executor, RemoveDone done) {
  auto job = [this, ids, observed, writer = std::move(writer), deadline, executor, done]() {
    RemoveEvidence evidence;
    try {
      evidence = remove(ids, observed, writer, deadline);
    } catch (const std::exception& error) {
      evidence.sought = ids;
      evidence.why = error.what();
    }
    boost::asio::post(executor, [done, evidence = std::move(evidence)] { done(evidence); });
  };
  if (!submit(std::move(job))) {
    RemoveEvidence refused;
    refused.sought = std::move(ids);
    refused.why = "the store is stopping";
    boost::asio::post(executor, [done = std::move(done), refused = std::move(refused)] { done(refused); });
  }
}

// ---- Admission -------------------------------------------------------------

std::optional<DataAdmission> DistributedStore::admit_data(FrameType frame, uint64_t bytes, std::optional<std::chrono::milliseconds> wait,
                                                          std::atomic_bool* cancelled) const {
  return store::admit_data(_credit, _io, frame, bytes, wait, cancelled);
}

std::optional<IoReserve::Slot> DistributedStore::admit_control() const { return store::admit_control(_io, _options.admission_wait); }

dannan::net::CallWatch DistributedStore::peer_call_watch() const {
  dannan::net::CallWatch watch;
  watch.no_progress = _options.call_no_progress;
  return watch;
}

dannan::net::CallWatch DistributedStore::control_call_watch() const {
  dannan::net::CallWatch watch;
  watch.no_progress = _options.control_no_progress;
  return watch;
}

void DistributedStore::note_network(uint64_t bytes, std::chrono::steady_clock::duration elapsed) {
  // Background credit is spent an extent at a time, so the estimate must
  // reflect an extent's cost: a transfer under half an extent is round trip
  // and the far end's write, not bandwidth.
  const auto seconds = std::chrono::duration<double>(elapsed).count();
  if (!bytes || seconds <= 0.0 || bytes < _options.extent_size / 2) return;
  const double sample = static_cast<double>(bytes) / seconds;
  const double old = _network_bps.load(std::memory_order_relaxed);
  _network_bps.store(old > 0.0 ? old * 0.80 + sample * 0.20 : sample, std::memory_order_relaxed);
}

// ---- Validated presence, every node, deletes ---------------------------------

std::map<ObjectId, bool> DistributedStore::valid_on(const NodeRecord& target, const std::vector<ObjectId>& ids) {
  std::map<ObjectId, bool> out;
  if (target.id == _self) {
    for (const auto& id : ids) {
      const auto admitted = admit_data(FrameType::speculative, _options.extent_size, no_caller_deadline);
      if (admitted) out[id] = _data.valid(id);
    }
    return out;
  }
  std::vector<Request> requests;
  std::vector<std::vector<ObjectId>> chunks;
  for (size_t offset = 0; offset < ids.size(); offset += message::max_valid_batch) {
    chunks.emplace_back(ids.begin() + static_cast<std::ptrdiff_t>(offset),
                        ids.begin() + static_cast<std::ptrdiff_t>(std::min(ids.size(), offset + message::max_valid_batch)));
    requests.push_back({target, message::have_valid_objects, message::encode_ids(chunks.back()), FrameType::speculative});
  }
  const auto outcomes = call_all(_context.routes(), _executor, std::move(requests), chunks.size(), _options.presence_call);
  for (size_t i = 0; i < chunks.size(); ++i) {
    const auto& outcome = outcomes[i];
    if (!outcome.reply) _logger.debug("repair presence batch peer=" + target.host + " error=" + outcome_text(outcome));
    if (!outcome.reply || outcome.reply->type != message::have_valid_objects_reply) continue;
    try {
      const auto answers = message::decode_validity(outcome.reply->payload, chunks[i].size());
      for (size_t j = 0; j < answers.size(); ++j)
        if (answers[j] != message::Validity::cannot_say) out[chunks[i][j]] = answers[j] == message::Validity::intact;
    } catch (const std::exception& error) {
      _logger.debug("repair presence batch peer=" + target.host + " error=" + error.what());
    }
  }
  return out;
}

size_t DistributedStore::replicate_all(const ObjectId& id, std::span<const uint8_t> data, bool foreground) {
  size_t success = 0;
  const auto frame = foreground ? FrameType::foreground : FrameType::speculative;
  for (const auto& target : hosting(_context.nodes().active())) {
    try {
      if (put_on(target, id, data, frame)) ++success;
    } catch (const std::exception& error) {
      _logger.debug("universal object write " + target.host + ": " + error.what());
    }
  }
  return success;
}

void DistributedStore::erase_all(const ObjectId& id) {
  for (const auto& target : _context.nodes().active()) {
    try {
      if (target.id == _self) {
        const auto admitted = admit_data(FrameType::speculative, _options.extent_size, no_caller_deadline);
        if (admitted) {
          // The claim check marks the object for the delete.
          const auto deletion = _data.claim_locks().deletion(id, [&] { return _claims.retained(contract::RetentionClass::data, id); });
          if (!deletion.claimed()) (void)_data.remove(id);
        }
        (void)_cache.remove(id);
      } else {
        (void)_context.routes().call_and_wait(target, message::delete_object, message::encode_id(id), FrameType::speculative, _options.delete_call,
                                              control_call_watch());
      }
    } catch (const std::exception& error) {
      _logger.debug("object delete " + target.host + ": " + error.what());
    }
  }
}

bool DistributedStore::ensure_local(const ObjectId& id, bool foreground) {
  const auto frame = foreground ? FrameType::foreground : FrameType::speculative;
  {
    const auto admitted = admit_data(frame, _options.extent_size, no_caller_deadline);
    if (!admitted) return false;
    if (_data.valid(id)) return true;
  }
  const auto keep = [&](std::span<const uint8_t> bytes) {
    const auto admitted = admit_data(frame, bytes.size(), no_caller_deadline);
    if (!admitted || !_data.put(id, bytes)) return false;
    _context.events().notify(_storage_event);
    note_acknowledged(id, _self);
    return true;
  };
  if (const auto cached = _cache.get(id); cached && keep(*cached)) return true;
  ReadOptions options;
  options.frame = frame;
  const auto read = get_remote(id, options, false);
  return read.object && keep(read.object->bytes);
}

// ---- Repair ----------------------------------------------------------------

void DistributedStore::trace_repair(const std::string& detail) const {
  if (_options.repair_trace) _options.repair_trace("repair", detail);
}

void DistributedStore::note_unsourceable(const ObjectId& id) {
  const auto total = _repair->pull_unsourceable.fetch_add(1, std::memory_order_relaxed) + 1;
  bool warn = false;
  {
    Lock lock(_repair->sample_mutex);
    auto& sample = _repair->unsourceable_sample;
    if (std::find(sample.begin(), sample.end(), id) == sample.end()) {
      sample.push_back(id);
      while (sample.size() > RepairState::sample_size) sample.pop_front();
    }
    // At most one warning a minute: a lost node can leave many objects
    // missing at once. The counter is the measure.
    const auto now = std::chrono::steady_clock::now();
    if (_repair->unsourceable_logged == std::chrono::steady_clock::time_point{} || now - _repair->unsourceable_logged >= std::chrono::minutes(1)) {
      _repair->unsourceable_logged = now;
      warn = true;
    }
  }
  if (warn)
    _logger.warn("repair cannot source an object this node should own object=" + to_string(id) + " unsourceable_total=" + std::to_string(total) +
                 "; the live namespace references it and no peer would supply it");
}

void DistributedStore::note_local_unreadable(const ObjectId& id) {
  const auto total = _repair->local_unreadable.fetch_add(1, std::memory_order_relaxed) + 1;
  bool warn = false;
  {
    Lock lock(_repair->sample_mutex);
    // At most one warning a minute, as for an object no peer would supply.
    const auto now = std::chrono::steady_clock::now();
    if (_repair->unreadable_logged == std::chrono::steady_clock::time_point{} || now - _repair->unreadable_logged >= std::chrono::minutes(1)) {
      _repair->unreadable_logged = now;
      warn = true;
    }
  }
  if (warn)
    _logger.warn("repair found a local object it cannot read back object=" + to_string(id) + " local_unreadable_total=" + std::to_string(total) +
                 "; the store listed it but the read failed");
}

RepairDiagnostics DistributedStore::repair_diagnostics() const {
  RepairDiagnostics out;
  const auto& state = *_repair;
  out.pull_unsourceable = state.pull_unsourceable.load(std::memory_order_relaxed);
  out.local_unreadable = state.local_unreadable.load(std::memory_order_relaxed);
  out.push_examined = state.push_examined.load(std::memory_order_relaxed);
  out.pull_examined = state.pull_examined.load(std::memory_order_relaxed);
  out.bytes_transferred = state.bytes.load(std::memory_order_relaxed);
  out.passes_completed = state.passes.load(std::memory_order_relaxed);
  out.push_phase_complete = state.push_phase_complete.load(std::memory_order_relaxed);
  out.copies_pushed = state.pushed_copies.load(std::memory_order_relaxed);
  out.copies_pulled = state.pulled_copies.load(std::memory_order_relaxed);
  out.local_drops = state.local_drops.load(std::memory_order_relaxed);
  Lock lock(state.sample_mutex);
  out.unsourceable_sample.assign(state.unsourceable_sample.begin(), state.unsourceable_sample.end());
  return out;
}

RepairResult DistributedStore::repair_step(const RepairLists& lists, uint64_t byte_budget, size_t operation_budget, const std::function<bool()>& should_yield) {
  RepairResult result;
  result.complete = false;
  auto& state = *_repair;
  auto& transferred = result.bytes_transferred;
  size_t operations = 0;
  const auto now = _time.now();
  // A peer arriving or leaving, or a disk coming or going, can settle what
  // was waiting: every retry is due at once.
  if (const auto epoch = (_context.events().count(dannan::status::NodeEvents::topology) << 8) + _data.online_backends(); epoch != state.retry_epoch) {
    state.retry_epoch = epoch;
    for (auto* retries : {&state.pull_retries, &state.push_retries})
      for (auto& [id, retry] : *retries) retry = {now, RepairState::retry_first};
  }
  // Nothing referenced is known: there is nothing to repair towards.
  if (!lists.inventory) {
    result.complete = true;
    return result;
  }
  const auto live = lists.inventory->lookup(contract::RetentionClass::data);

  // The push works from the lacks of every peer that answered for this inventory,
  // whether or not every hosting peer did (the maintainer, 2026-11-14: "push to the peers that
  // answered"): diff only, no walk, no probe. A peer the survey did not settle is
  // neither a keeper nor sent a copy, and is asked nothing.
  const auto* lacks = lists.lacks.get();
  const auto generation = lists.inventory->generation();
  if (lists.lacks != state.push_source || generation != state.push_generation) {
    if (state.push_after) state.spans_change = true;
    state.push_source = lists.lacks;
    state.push_generation = generation;
    // A new list has not been walked, whatever became of the last one.
    state.push_complete = false;
    state.listed = lacks && lacks->generation == generation;
    state.push_list.clear();
    state.pushed.clear();
    if (state.listed) {
      for (const auto& [peer, ids] : lacks->lacks) state.push_list.insert(state.push_list.end(), ids->begin(), ids->end());
      std::sort(state.push_list.begin(), state.push_list.end());
      state.push_list.erase(std::unique(state.push_list.begin(), state.push_list.end()), state.push_list.end());
    }
  }
  static const std::vector<ObjectId> nothing;
  const auto& pull_list = lists.pull ? *lists.pull : nothing;
  if (lists.pull != state.pull_source) {
    if (state.pull_after) state.spans_change = true;
    state.pull_source = lists.pull;
    state.pull_complete = false;
  }
  if (pull_list.empty()) state.pull_complete = true;
  if (state.push_list.empty() && !state.push_after) state.push_complete = true;

  const auto yielded = [&] {
    if (should_yield && should_yield()) {
      result.yielded = true;
      return true;
    }
    return false;
  };
  const auto reserve_operation = [&] {
    if (operation_budget && operations >= operation_budget) return false;
    ++operations;
    result.remote_operations = operations;
    return true;
  };
  // A settled object needs no call, so the call budget alone would not bound
  // a step: what a step examines is bounded too.
  const size_t scan_budget = operation_budget ? std::clamp<size_t>(operation_budget * 4, 16, 64) : std::numeric_limits<size_t>::max();
  const uint64_t extent = std::max<uint64_t>(1, _options.extent_size);
  // A peer whose advertised room cannot take an extent is not sent one, so a
  // full peer cannot spend the step; the copy stays short, which placement
  // reports.
  const auto has_room = [&](const NodeRecord& peer) {
    if (peer.id == _self) return true;
    const auto capacity = peer.advert_u64(capacity_advert).value_or(0);
    return !capacity || peer.advert_u64(used_advert).value_or(0) + extent <= capacity;
  };
  // What the diff says a peer holds, or nothing for a peer it did not ask.
  const auto known_present = [&](const NodeId& peer, const ObjectId& id) -> std::optional<bool> {
    if (!state.listed) return std::nullopt;
    const auto found = lacks->lacks.find(peer);
    if (found == lacks->lacks.end()) return std::nullopt;
    if (state.pushed.contains({peer, id})) return true;
    return !std::binary_search(found->second->begin(), found->second->end(), id);
  };

  // ---- Push: copies to the owners that lack them.
  if (!state.push_complete) {
    // An object whose first owners all hold it, this node among them, needs
    // nothing and is passed without spending the scan budget.
    const auto settled_by_holdings = [&](const ObjectId& id) {
      const auto nodes = ranked(id);
      const size_t target = std::min(_options.replication, nodes.size());
      size_t keepers = 0;
      bool kept_here = false;
      for (const auto& peer : nodes) {
        if (keepers >= target) break;
        if (!has_room(peer)) continue;
        bool present = false;
        if (peer.id == _self) {
          present = _data.has(id);
          kept_here = present;
        } else if (const auto known = known_present(peer.id, id)) {
          present = *known;
        } else {
          return false;
        }
        if (!present) return false;
        ++keepers;
      }
      return keepers >= target && kept_here;
    };
    // Retried objects whose time has come go first, at most half the window.
    std::vector<ObjectId> window;
    std::set<ObjectId> retrying;
    for (auto retry = state.push_retries.begin(); retry != state.push_retries.end() && window.size() < std::max<size_t>(1, scan_budget / 2);) {
      const auto id = retry->first;
      if (!_data.has(id) || !live.contains(id)) {
        retry = state.push_retries.erase(retry);
        continue;
      }
      if (retry->second.due <= now) {
        window.push_back(id);
        retrying.insert(id);
      }
      ++retry;
    }
    auto at = state.push_after ? std::upper_bound(state.push_list.begin(), state.push_list.end(), *state.push_after) : state.push_list.begin();
    size_t skipped = 0;
    while (window.size() < scan_budget && at != state.push_list.end() && skipped < RepairState::skip_budget) {
      const auto id = *at;
      // Listed, and no longer held here or referenced, or needing nothing:
      // passed, while nothing unsettled is ahead of it.
      const bool idle = !_data.has(id) || !live.contains(id) || retrying.contains(id);
      if (window.size() == retrying.size()) {
        if (idle || settled_by_holdings(id)) {
          state.push_after = id;
          ++at;
          ++skipped;
          ++result.push_examined;
          continue;
        }
      } else if (idle) {
        break;
      }
      window.push_back(id);
      ++at;
    }
    const bool list_ended = at == state.push_list.end();

    struct Send {
      ObjectId id;
      NodeRecord peer;
      std::shared_ptr<const Bytes> data;
    };
    struct Plan {
      ObjectId id;
      size_t target = 0;
      std::set<NodeId> keepers;
      // A peer could not say whether it holds a copy.
      bool undecided = false;
      bool unreadable = false;
    };
    const uint64_t batch_bytes_cap = 2 * extent;
    size_t next = 0;
    bool stopped = false;
    // The list's position moves over what settled in order; an object that
    // must be tried again holds it.
    bool in_order = true;
    while (!stopped && next < window.size()) {
      if (yielded()) break;
      std::vector<Plan> plans;
      std::vector<Send> sends;
      uint64_t batch_bytes = 0;
      for (size_t i = next; i < window.size(); ++i) {
        if (sends.size() >= RepairState::sends_in_flight || (!sends.empty() && batch_bytes >= batch_bytes_cap)) break;
        Plan plan;
        plan.id = window[i];
        const auto nodes = ranked(plan.id);
        plan.target = std::min(_options.replication, nodes.size());
        std::shared_ptr<const Bytes> source;
        std::vector<Send> object_sends;
        uint64_t object_bytes = 0;
        bool stop = false;
        for (const auto& peer : nodes) {
          if (plan.keepers.size() + object_sends.size() >= plan.target) break;
          if (!has_room(peer)) continue;
          bool present = false;
          if (peer.id == _self) {
            // Index presence: a corrupt local copy is found and removed by
            // scrub and reads, after which repair sees it absent.
            present = _data.has(plan.id);
          } else if (const auto known = known_present(peer.id, plan.id)) {
            present = *known;
          } else {
            // Neither a keeper nor sent a copy until a survey says.
            plan.undecided = true;
            continue;
          }
          if (present) {
            plan.keepers.insert(peer.id);
            continue;
          }
          if (peer.id == _self) continue;
          if (!source) {
            std::optional<Bytes> read;
            {
              const auto admitted = admit_data(FrameType::speculative, extent, no_caller_deadline);
              if (!admitted) {
                stop = true;
                break;
              }
              read = _data.get(plan.id);
            }
            if (!read) {
              note_local_unreadable(plan.id);
              plan.unreadable = true;
              break;
            }
            source = std::make_shared<const Bytes>(std::move(*read));
          }
          // Credit pays for bytes put on the wire, never for finding out
          // which are needed.
          if (transferred + batch_bytes + object_bytes + source->size() > byte_budget) {
            result.credit_limited = true;
            stop = true;
            break;
          }
          if (!reserve_operation()) {
            stop = true;
            break;
          }
          object_sends.push_back({plan.id, peer, source});
          object_bytes += source->size();
        }
        if (stop) {
          stopped = true;
          break;
        }
        batch_bytes += object_bytes;
        for (auto& send : object_sends) sends.push_back(std::move(send));
        plans.push_back(std::move(plan));
      }

      // Every send goes out before any answer is awaited; each completes and
      // is kept even if repair's turn ends meanwhile.
      std::vector<Request> requests;
      for (const auto& send : sends) requests.push_back({send.peer, message::put_object, message::encode_object(send.id, *send.data), FrameType::speculative});
      const auto dispatched = Clock::now();
      const auto outcomes = call_all(_context.routes(), _executor, std::move(requests), sends.size(), _options.object_transfer_call, peer_call_watch());
      std::set<ObjectId> failed;
      uint64_t delivered = 0;
      for (size_t i = 0; i < sends.size(); ++i) {
        const auto& send = sends[i];
        const bool ok = outcomes[i].reply && outcomes[i].reply->type == dannan::net::message::ok;
        trace_repair("push " + to_string(send.id) + " to " + dannan::identity::to_string(send.peer.id) + (ok ? " delivered" : " failed"));
        if (!ok) {
          _logger.debug("repair push peer=" + send.peer.host + " error=" + outcome_text(outcomes[i]));
          failed.insert(send.id);
          continue;
        }
        delivered += send.data->size();
        state.pushed.insert({send.peer.id, send.id});
        state.pushed_copies.fetch_add(1, std::memory_order_relaxed);
        for (auto& plan : plans)
          if (plan.id == send.id) plan.keepers.insert(send.peer.id);
        // The copy reaches every ticket that names the peer pending.
        note_acknowledged(send.id, send.peer.id);
      }
      if (delivered) {
        transferred += delivered;
        // One sample for the batch: its sends shared the link.
        note_network(delivered, Clock::now() - dispatched);
      }

      for (const auto& plan : plans) {
        ++next;
        if (failed.contains(plan.id)) {
          // Tried again from here next step.
          in_order = in_order && retrying.contains(plan.id);
          continue;
        }
        if (plan.undecided && plan.keepers.size() < plan.target)
          RepairState::retry_later(state.push_retries, plan.id, now);
        else
          state.push_retries.erase(plan.id);
        // No owner known means nowhere it is kept, not every owner keeping
        // it: the copy stays until a host is known.
        if (!plan.unreadable && plan.target > 0 && plan.keepers.size() >= plan.target && !plan.keepers.contains(_self) &&
            !_claims.retained(contract::RetentionClass::data, plan.id)) {
          const auto admitted = admit_data(FrameType::speculative, extent, no_caller_deadline);
          if (admitted && _data.remove(plan.id)) {
            state.local_drops.fetch_add(1, std::memory_order_relaxed);
            trace_repair("drop-local " + to_string(plan.id));
          }
        }
        if (retrying.contains(plan.id)) continue;
        ++result.push_examined;
        if (in_order) state.push_after = plan.id;
      }
      if (!failed.empty() || plans.empty()) break;
    }
    if (next >= window.size() && list_ended && in_order && !stopped && !result.yielded) state.push_complete = true;
  }

  // ---- Pull: what this node should own and lacks, the retries due first.
  // Nothing is pulled while no local backend could store it.
  if (_data.online_backends() && !result.credit_limited) {
    bool stopped = false;
    // The pull's own share of the step: a push pass that fills every step
    // must not hold the pull back until it ends.
    size_t pull_scanned = 0;
    // Fetches one object and keeps it; nothing when this node's device will
    // not admit it.
    const auto pull = [&](const ObjectId& id) -> std::optional<FetchAnswer> {
      ReadOptions options;
      options.frame = FrameType::speculative;
      // The fetch runs to completion and is kept even if repair yields, so a
      // node that is never quiet still pulls.
      const auto read = get_remote(id, options, false);
      if (read.object) {
        {
          const auto admitted = admit_data(FrameType::speculative, read.object->bytes.size(), no_caller_deadline);
          if (!admitted) return std::nullopt;
          if (_data.put(id, read.object->bytes)) {
            transferred += read.object->bytes.size();
            state.pulled_copies.fetch_add(1, std::memory_order_relaxed);
          }
        }
        _context.events().notify(_storage_event);
        note_acknowledged(id, _self);
        trace_repair("pull " + to_string(id) + " fetched");
      } else if (read.answer == FetchAnswer::not_found) {
        // Should own it, not here, not cached, and every peer present that
        // might hold it says it does not.
        note_unsourceable(id);
        ++result.pull_unsourceable;
        trace_repair("pull " + to_string(id) + " unsourceable");
      } else {
        trace_repair("pull " + to_string(id) + " try-later");
      }
      return read.answer;
    };

    for (auto retry = state.pull_retries.begin(); retry != state.pull_retries.end() && pull_scanned < scan_budget;) {
      const auto id = retry->first;
      if (retry->second.due > now) {
        ++retry;
        continue;
      }
      if (!should_own(id) || _data.has(id) || (lists.unavailable && lists.unavailable(id))) {
        retry = state.pull_retries.erase(retry);
        continue;
      }
      if (transferred + extent > byte_budget) {
        result.credit_limited = true;
        break;
      }
      if (!reserve_operation()) break;
      const auto answer = pull(id);
      if (!answer) {
        stopped = true;
        break;
      }
      ++pull_scanned;
      if (*answer != FetchAnswer::try_later) {
        retry = state.pull_retries.erase(retry);
        continue;
      }
      RepairState::retry_later(state.pull_retries, id, now);
      ++retry;
    }

    if (!stopped && !result.credit_limited && !state.pull_complete && !pull_list.empty()) {
      auto it = state.pull_after ? std::upper_bound(pull_list.begin(), pull_list.end(), *state.pull_after) : pull_list.begin();
      // An object that needs nothing is an index lookup: it counts against
      // its own, far larger budget, so a settled stretch passes in one step.
      size_t skipped = 0;
      const auto skip = [&](const ObjectId& id) {
        state.pull_after = id;
        ++it;
        ++skipped;
        ++result.pull_examined;
      };
      while (it != pull_list.end() && pull_scanned < scan_budget && skipped < RepairState::skip_budget) {
        if (skipped % 1024 == 0 && yielded()) break;
        const auto id = *it;
        if (!should_own(id) || _data.has(id) || state.pull_retries.contains(id)) {
          skip(id);
          continue;
        }
        // A copy playback already cached is promoted before any network
        // work, so convergence never downloads twice.
        if (const auto cached = _cache.get(id)) {
          {
            const auto admitted = admit_data(FrameType::speculative, cached->size(), no_caller_deadline);
            if (!admitted) break;
            (void)_data.put(id, *cached);
          }
          note_acknowledged(id, _self);
          trace_repair("pull " + to_string(id) + " from-cache");
          state.pull_after = id;
          ++it;
          ++pull_scanned;
          ++result.pull_examined;
          continue;
        }
        // No work that cannot complete: the survey asks again when the
        // membership or storage could change the answer.
        if (lists.unavailable && lists.unavailable(id)) {
          trace_repair("pull " + to_string(id) + " unavailable");
          skip(id);
          continue;
        }
        if (transferred + extent > byte_budget) {
          result.credit_limited = true;
          break;
        }
        if (!reserve_operation()) break;
        const auto answer = pull(id);
        if (!answer) break;
        if (*answer == FetchAnswer::try_later) RepairState::retry_later(state.pull_retries, id, _time.now());
        state.pull_after = id;
        ++it;
        ++pull_scanned;
        ++result.pull_examined;
      }
      if (it == pull_list.end()) {
        state.pull_complete = true;
        state.pull_after.reset();
      }
    }
  }

  result.remote_operations = operations;
  state.push_examined.fetch_add(result.push_examined, std::memory_order_relaxed);
  state.pull_examined.fetch_add(result.pull_examined, std::memory_order_relaxed);
  state.bytes.fetch_add(result.bytes_transferred, std::memory_order_relaxed);
  state.push_phase_complete.store(state.push_complete, std::memory_order_relaxed);
  if (state.push_complete && state.pull_complete) {
    // A pass a list changed under may have passed objects before they were
    // listed: another runs rather than report settled.
    result.complete = !state.spans_change;
    state.spans_change = false;
    state.passes.fetch_add(1, std::memory_order_relaxed);
    state.push_after.reset();
    state.pull_after.reset();
    state.push_complete = state.push_list.empty();
    state.pull_complete = pull_list.empty();
  }
  return result;
}

}  // namespace dagda::store
