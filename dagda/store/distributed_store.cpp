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
#include <utility>

#include "dagda/store/messages.h"
#include "dagda/store/placement.h"
#include "dannan/net/outbound.h"
#include "dannan/placement/placement.h"

namespace dagda::store {
namespace {

using Clock = std::chrono::steady_clock;
using dannan::contract::Lock;
using dannan::contract::ReadLock;
using dannan::contract::WriteLock;
using dannan::identity::NodeId;
using dannan::membership::NodeRecord;
using dannan::net::CallOutcome;
using dannan::sched::FrameType;

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
// `deadline`; the outcomes in order. Waits on the peers.
struct Request {
  NodeRecord peer;
  dannan::net::MessageType type{};
  Bytes payload;
  FrameType frame = FrameType::control;
};

std::vector<CallOutcome> call_all(dannan::Routes& routes, const boost::asio::any_io_executor& executor, std::vector<Request> requests, size_t window,
                                  std::chrono::milliseconds deadline) {
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
    routes.call(requests[i].peer, requests[i].type, std::move(requests[i].payload), requests[i].frame, deadline, executor,
                [promise](CallOutcome outcome) { promise->set_value(std::move(outcome)); });
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

struct Waiter {
  boost::asio::any_io_executor executor;
  DistributedStore::WriteDone done;
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
  std::vector<ReplicaRecord> replicas DANNAN_GUARDED_BY(mutex);
  std::vector<std::shared_ptr<Waiter>> waiters DANNAN_GUARDED_BY(mutex);

  WriteEvidence evidence_locked() const DANNAN_REQUIRES(mutex) {
    WriteEvidence out;
    out.id = id;
    out.level = level;
    out.deferred = deferred;
    out.sought = sought;
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

  // The waiters to tell, once nothing is awaited.
  std::vector<std::shared_ptr<Waiter>> settled_waiters_locked() DANNAN_REQUIRES(mutex) {
    if (!evidence_locked().settled) return {};
    return std::exchange(waiters, {});
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
    told = ticket->settled_waiters_locked();
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

DistributedStore::DistributedStore(dannan::Context& context, boost::asio::any_io_executor executor, Stores stores, DistributedStoreOptions options)
    : _context(context),
      _executor(std::move(executor)),
      _data(stores.data),
      _control(stores.control),
      _cache(stores.cache),
      _claims(stores.claims),
      _options(options),
      _time(options.time ? *options.time : dannan::contract::steady_time_source()),
      _self(context.identity().id()),
      _logger(context.logger("dagda.store")),
      _storage_event(context.events().kind("storage")),
      _control_fetch(context, stores.control, options.control_call),
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

std::chrono::milliseconds DistributedStore::write_deadline_of(const WriteOptions& options) const {
  return options.deadline.count() > 0 ? options.deadline : _options.write_deadline;
}

std::shared_ptr<DistributedStore::TicketRecord> DistributedStore::new_ticket(const ObjectId& id, Durability level, bool deferred) {
  std::vector<std::shared_ptr<Waiter>> forgotten;
  WriteEvidence forgotten_evidence;
  std::shared_ptr<TicketRecord> record;
  {
    Lock lock(_shared->mutex);
    record = std::make_shared<TicketRecord>(_shared->next_serial++, id, level, deferred);
    _shared->tickets.emplace(record->serial, record);
    // Past the bound, the oldest settled ticket goes first, else the oldest.
    while (_shared->tickets.size() > std::max<size_t>(1, _options.max_tickets)) {
      auto victim = _shared->tickets.begin();
      for (auto it = _shared->tickets.begin(); it != _shared->tickets.end(); ++it) {
        Lock ticket_lock(it->second->mutex);
        if (it->second->evidence_locked().settled) {
          victim = it;
          break;
        }
      }
      {
        Lock ticket_lock(victim->second->mutex);
        forgotten = std::exchange(victim->second->waiters, {});
        if (!forgotten.empty()) forgotten_evidence = victim->second->evidence_locked();
      }
      _shared->tickets.erase(victim);
    }
  }
  for (const auto& waiter : forgotten) tell(waiter, forgotten_evidence);
  return record;
}

WriteEvidence DistributedStore::run_write(const ObjectId& id, std::span<const uint8_t> data, const WriteOptions& options, std::shared_ptr<TicketRecord> into) {
  _context.activity().note(options.frame, data.size());
  const auto deadline = Clock::now() + write_deadline_of(options);
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
  const auto payload = message::encode_object(id, data);
  const auto type = deferred ? message::put_object_deferred : message::put_object;
  size_t next_fallback = initial;
  size_t replacement_needed = 0;

  const auto launch = [&](const NodeRecord& node) {
    size_t index = 0;
    {
      Lock lock(run->mutex);
      run->calls.push_back({});
      run->calls.back().node = node;
      index = run->calls.size() - 1;
    }
    if (node.id == _self) {
      bool ok = false;
      std::string why;
      std::optional<DurabilityTicket> durability;
      try {
        const bool held = _data.has(id);
        if (deferred) {
          durability = _data.put_deferred(id, data);
          ok = durability.has_value();
        } else {
          ok = _data.put(id, data);
        }
        if (ok && !held) _context.events().notify(_storage_event);
        if (!ok) why = "507 storage_full: storage limit reached";
      } catch (const std::exception& error) {
        why = error.what();
      }
      {
        Lock lock(run->mutex);
        auto& call = run->calls[index];
        call.done = true;
        call.ok = ok;
        call.refused = !ok;
        call.why = why;
        call.durability = durability;
      }
      return;
    }
    // A call runs on past the write to call_horizon, so a slow peer's answer
    // still reaches the ticket.
    auto handle = _context.routes().call(node, type, payload, options.frame, _options.call_horizon, _executor,
                                         [run, index](CallOutcome outcome) { run->complete(index, std::move(outcome)); });
    Lock lock(run->mutex);
    run->calls[index].handle = std::move(handle);
  };

  for (size_t i = 0; i < initial; ++i) launch(order[i]);

  size_t success = 0;
  {
    Lock lock(run->mutex);
    while (true) {
      success = 0;
      size_t unfinished = 0;
      size_t stalled = 0;
      size_t waited_unfinished = 0;
      size_t waited_ok = 0;
      auto wake = deadline;
      const auto now = Clock::now();
      for (auto& call : run->calls) {
        const bool member_waited = waited.contains(call.node.id);
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
        // A durable write waits on a live member to its deadline; the
        // others spill past a peer stalled beyond write_stall.
        if (level != Durability::durable && !call.spilled && call.handle) {
          // A call not placed yet has made no progress to judge.
          const auto idle = call.handle->idle_for();
          if (idle == std::chrono::milliseconds::max()) {
            wake = std::min(wake, now + std::max(std::chrono::milliseconds(1), _options.write_stall / 4));
          } else if (idle >= _options.write_stall) {
            call.spilled = true;
            call.counted = true;
            ++replacement_needed;
            _logger.debug("object write stalled; spilling id=" + to_string(id) + " peer=" + dannan::identity::to_string(call.node.id).substr(0, 12));
          } else {
            wake = std::min(wake, now + (_options.write_stall - idle));
          }
        }
        if (call.spilled) ++stalled;
      }
      const bool satisfied = level == Durability::durable ? (waited_ok == waited.size() && success > 0) : (floor && success >= floor);
      if (satisfied) break;
      if ((options.cancelled && options.cancelled->load(std::memory_order_relaxed)) || _stopping.load()) {
        for (auto& call : run->calls)
          if (!call.done && call.handle) call.handle->cancel();
        break;
      }
      if (now >= deadline) break;
      if (replacement_needed && next_fallback < order.size()) {
        --replacement_needed;
        const auto node = order[next_fallback++];
        lock.unlock();
        launch(node);
        lock.lock();
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
      run->changed.wait_until(lock.native(), wake);
    }

    // The evidence, built under the run's lock: a call answering from here on
    // lands in the ticket instead.
    auto record = into ? std::move(into) : new_ticket(id, options.level, deferred);
    {
      Lock ticket_lock(record->mutex);
      record->sought = floor;
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
  std::vector<std::shared_ptr<Waiter>> told;
  WriteEvidence evidence;
  {
    Lock lock(into->mutex);
    told = into->settled_waiters_locked();
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
  const auto id = object_id(data);
  const auto evidence = write(id, data, options);
  if (!evidence.holders() && options.level != Durability::ephemeral) {
    if (options.cancelled && options.cancelled->load(std::memory_order_relaxed)) throw std::runtime_error("object replication cancelled");
    throw StorageUnavailable("object replication quorum unavailable");
  }
  return id;
}

bool DistributedStore::put(const ObjectId& id, std::span<const uint8_t> data, WriteOptions options) {
  const auto evidence = write(id, data, options);
  return options.level == Durability::ephemeral ? !evidence.lost : evidence.holders() > 0;
}

ObjectId DistributedStore::put_deferred(std::span<const uint8_t> data, WriteOptions options) {
  options.deferred = true;
  return put(data, options);
}

bool DistributedStore::put_here(const ObjectId& id, std::span<const uint8_t> data, FrameType frame) {
  WriteOptions options;
  options.frame = frame;
  if (!holds_extents()) return put(id, data, options);
  check_write(id, data);
  const bool held = _data.has(id);
  if (!_data.put(id, data)) return put(id, data, options);
  if (!held) _context.events().notify(_storage_event);
  queue_prompt_replication(id);
  return true;
}

bool DistributedStore::put_on(const NodeRecord& target, const ObjectId& id, std::span<const uint8_t> data, FrameType frame) {
  if (target.id == _self) {
    const bool held = _data.has(id);
    const bool ok = _data.put(id, data);
    if (ok && !held) _context.events().notify(_storage_event);
    return ok;
  }
  const auto outcome = _context.routes().call_and_wait(target, message::put_object, message::encode_object(id, data), frame, _options.call_horizon);
  return outcome.reply && outcome.reply->type == dannan::net::message::ok;
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
  return record->evidence_locked();
}

void DistributedStore::wait(Ticket ticket, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, WriteDone done) {
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
    return;
  }
  auto waiter = std::make_shared<Waiter>();
  waiter->executor = executor;
  waiter->done = std::move(done);
  // The timer is armed and cancelled only on its own strand.
  const auto strand = boost::asio::make_strand(_executor);
  waiter->timer = std::make_shared<boost::asio::steady_timer>(strand, deadline);
  bool barriers = false;
  {
    Lock lock(record->mutex);
    for (const auto& replica : record->replicas)
      if (replica.awaiting == Awaiting::barrier && !replica.running) barriers = true;
    const auto evidence = record->evidence_locked();
    if (evidence.settled) {
      lock.unlock();
      tell(waiter, evidence);
      return;
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
  if (barriers) submit([this, record, deadline] { run_barriers(record, deadline); });
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
  return evidence;
}

void DistributedStore::run_barriers(const std::shared_ptr<TicketRecord>& record, std::chrono::milliseconds deadline) {
  std::vector<std::pair<NodeId, DurabilityTicket>> due;
  {
    Lock lock(record->mutex);
    for (auto& replica : record->replicas)
      if (replica.awaiting == Awaiting::barrier && !replica.running && replica.evidence.durability) {
        replica.running = true;
        due.emplace_back(replica.evidence.node, *replica.evidence.durability);
      }
  }
  const auto id = record->id;
  // A refusal because a peer's process or backend changed means "your ticket
  // is dead", not "your bytes are gone" (discipline 1): asked again with the
  // id, the peer answers from disk with a fresh ticket.
  const auto transient = [](ReplicaRecord& replica, const std::string& why) {
    replica.evidence.why = "awaiting barrier: " + why;
    replica.running = false;
  };
  // Remote barriers go out first, so the local one's batch window does not
  // serialise them.
  std::map<NodeId, NodeRecord> peers;
  for (const auto& peer : _context.nodes().roster()) peers.emplace(peer.id, peer);
  for (const auto& [node, ticket] : due) {
    if (node == _self) continue;
    const auto found = peers.find(node);
    if (found == peers.end()) {
      update(record, node, [](ReplicaRecord& replica) {
        replica.evidence.state = ReplicaState::refused;
        replica.evidence.why = "unknown node";
        replica.awaiting = Awaiting::none;
        replica.running = false;
      });
      continue;
    }
    const auto peer = found->second;
    message::BarrierRequest request{ticket, {}};
    _context.routes().call(peer, message::object_durability_barrier, message::encode_barrier(request), FrameType::loader, deadline, _executor,
                           [this, weak = std::weak_ptr<Shared>(_shared), record, node, peer, ticket, id, deadline, transient](CallOutcome outcome) {
                             if (outcome.reply && outcome.reply->type == dannan::net::message::ok) {
                               update(record, node, [](ReplicaRecord& replica) { acknowledge(replica); });
                               return;
                             }
                             if (!refused_with(outcome, 409)) {
                               const auto why = outcome_text(outcome);
                               update(record, node, [&](ReplicaRecord& replica) { transient(replica, why); });
                               return;
                             }
                             const auto shared = weak.lock();
                             if (!shared) return;
                             ReadLock alive(shared->alive);
                             if (!shared->store) {
                               update(record, node, [](ReplicaRecord& replica) { replica.running = false; });
                               return;
                             }
                             message::BarrierRequest probe{ticket, {id}};
                             _context.routes().call(peer, message::object_durability_barrier, message::encode_barrier(probe), FrameType::loader, deadline,
                                                    _executor, [record, node, id, transient](CallOutcome answer) {
                                                      if (!answer.reply || answer.reply->type != dannan::net::message::ok) {
                                                        const auto why = outcome_text(answer);
                                                        update(record, node, [&](ReplicaRecord& replica) { transient(replica, "probe: " + why); });
                                                        return;
                                                      }
                                                      try {
                                                        const auto fresh = message::decode_probe_answer(answer.reply->payload);
                                                        for (const auto& [present, durability] : fresh.present)
                                                          if (present == id) {
                                                            update(record, node, [&](ReplicaRecord& replica) { acknowledge(replica, durability); });
                                                            return;
                                                          }
                                                        // Definitive: the peer no longer holds it. Put it again.
                                                        update(record, node, [](ReplicaRecord& replica) {
                                                          replica.evidence.state = ReplicaState::refused;
                                                          replica.evidence.why = "absent on the peer after its restart";
                                                          replica.awaiting = Awaiting::none;
                                                          replica.running = false;
                                                        });
                                                      } catch (const std::exception& error) {
                                                        const std::string why = error.what();
                                                        update(record, node, [&](ReplicaRecord& replica) { transient(replica, "probe: " + why); });
                                                      }
                                                    });
                           });
  }
  for (const auto& [node, ticket] : due) {
    if (node != _self) continue;
    try {
      _data.durability_barrier(ticket, DurabilityUrgency::batchable);
      update(record, node, [](ReplicaRecord& replica) { acknowledge(replica); });
    } catch (const StaleDurabilityTicket&) {
      // This node's backend reopened under the ticket: the same answer from
      // disk, no call.
      const auto fresh = _data.reassert_durable(id);
      update(record, node, [&](ReplicaRecord& replica) {
        if (fresh) {
          acknowledge(replica, fresh);
          return;
        }
        replica.evidence.state = ReplicaState::refused;
        replica.evidence.why = "absent here after the backend changed";
        replica.awaiting = Awaiting::none;
        replica.running = false;
      });
    } catch (const std::exception& error) {
      const std::string why = error.what();
      update(record, node, [&](ReplicaRecord& replica) { transient(replica, why); });
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
      const auto data = _data.get(*id);
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
  const bool foreground = options.frame == FrameType::foreground;
  const bool interactive = foreground || options.frame == FrameType::read_ahead;
  // Foreground demand is recorded before any storage access, so the first
  // read already holds back lower classes.
  if (foreground) _context.activity().note(options.frame, 0);
  if (auto local = _data.get(id)) {
    if (interactive) _context.activity().note(options.frame, local->size());
    return {buffer_of(std::move(*local)), ReadSource::local, _self, FetchAnswer::ok};
  }
  if (auto cached = _cache.get(id)) {
    if (interactive) _context.activity().note(options.frame, cached->size());
    if (should_own(id)) enqueue_local_copy(id, *cached, true);
    return {buffer_of(std::move(*cached)), ReadSource::cache, _self, FetchAnswer::ok};
  }
  if (options.cancelled && options.cancelled->load(std::memory_order_relaxed)) return {};
  auto result = get_remote(id, options, interactive);
  if (result.object && options.frame == FrameType::read_ahead) _context.activity().note(options.frame, result.object->bytes.size());
  return result;
}

ReadResult DistributedStore::get_remote(const ObjectId& id, const ReadOptions& options, bool opportunistic_persist) {
  const auto deadline = Clock::now() + (options.deadline.count() > 0 ? options.deadline : _options.read_deadline);
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
    auto owners = ranked(id);
    if (owners.size() > _options.replication) owners.resize(_options.replication);
    {
      Lock lock(fetch->mutex);
      for (const auto& node : owners) fetch->preferred.insert(node.id);
      for (auto& node : owners)
        if (node.id != _self) fetch->candidates.push_back(std::move(node));
    }
    fetch_next(fetch);
  }

  ReadResult out;
  std::shared_ptr<dannan::net::CallHandle> abandon;
  {
    Lock lock(fetch->mutex);
    while (!fetch->done && Clock::now() < deadline && !(options.cancelled && options.cancelled->load(std::memory_order_relaxed))) {
      auto wake = deadline;
      if (options.cancelled) wake = std::min(wake, Clock::now() + std::chrono::milliseconds(25));
      fetch->changed.wait_until(lock.native(), wake);
    }
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
  if (out.object && foreground) _context.activity().note(options.frame, out.object->bytes.size());
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
  auto handle = _context.routes().call(target, message::get_object, message::encode_id(fetch->id), frame, _options.call_horizon, _executor,
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
                                       });
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
    } catch (const std::exception& error) {
      _logger.debug("object read " + target.host + ": " + error.what());
    }
  } else {
    absent = refused_with(outcome, 404);
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
      _logger.debug("opportunistic persistence skipped object=" + to_string(id) + " reason=queue_full");
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
  const auto outcomes = call_all(_context.routes(), _executor, std::move(requests), _options.retention_check_concurrency, _options.control_call);
  for (size_t i = 0; i < chunks.size(); ++i) {
    const auto& [node, ids] = chunks[i];
    std::vector<bool> present(ids.size(), false);
    const auto& outcome = outcomes[i];
    if (outcome.reply && outcome.reply->type == message::have_objects_reply) {
      try {
        present = message::decode_presence(outcome.reply->payload, ids.size());
      } catch (const std::exception& error) {
        _logger.debug(std::string("retention presence batch: ") + error.what());
      }
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
                                 const contract::RetentionDot& dot) {
  if (input.empty()) return true;
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (target.id == _self) {
    // A claim says "this node holds the object": index presence, not a
    // read. The bytes were verified when put; scrub finds later corruption.
    for (const auto& id : ids) {
      const bool present = object_class == contract::RetentionClass::data ? _data.has(id) : _control.has(id);
      if (!present) return false;
    }
    _claims.retain_batch(object_class, ids, dot);
    return true;
  }
  const auto outcome = _context.routes().call_and_wait(target, message::retain_objects, message::encode_retain({object_class, dot, ids}), FrameType::loader,
                                                       _options.control_call);
  if (!outcome.reply || outcome.reply->type != dannan::net::message::ok) {
    _logger.debug("retention claim peer=" + target.host + " error=" + outcome_text(outcome));
    return false;
  }
  return true;
}

std::vector<ObjectId> DistributedStore::retain_data(const std::vector<ObjectId>& input, const contract::RetentionDot& dot) {
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<ObjectId> unheld;
  if (ids.empty()) return unheld;
  // Claims sought per object. A holder that is away is not claimed on: its
  // own head and the deletion grace protect its copy until it returns.
  const size_t floor = std::max<size_t>(1, _options.write_copies);
  std::map<ObjectId, std::vector<NodeRecord>> candidates_by_object;
  for (const auto& id : ids) candidates_by_object.emplace(id, ranked(id));
  auto selected_by_object = select_present_batched(candidates_by_object, floor);

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
  // One claim per selected node: claims are independent.
  std::map<ObjectId, std::set<NodeId>> claimed;
  for (const auto& [node, batch] : batches)
    if (retain_on(node_info.at(node), contract::RetentionClass::data, batch, dot))
      for (const auto& id : batch) claimed[id].insert(node);

  for (const auto& id : ids) {
    auto& successful = claimed[id];
    if (successful.size() >= floor) continue;
    if (absent.contains(id)) {
      unheld.push_back(id);
      continue;
    }
    for (const auto& candidate : candidates_by_object[id]) {
      if (successful.size() >= floor) break;
      if (successful.contains(candidate.id)) continue;
      if (has_on(candidate, id) && retain_on(candidate, contract::RetentionClass::data, {id}, dot)) successful.insert(candidate.id);
    }
    if (successful.empty()) unheld.push_back(id);
  }
  return unheld;
}

bool DistributedStore::ensure_control_local(const ObjectId& id) { return _control_fetch.ensure_control_local(id); }

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
  const auto outcomes = call_all(_context.routes(), _executor, std::move(requests), window, _options.control_call);
  bool ok = true;
  for (size_t i = 0; i < outcomes.size(); ++i) {
    if (outcomes[i].reply && outcomes[i].reply->type == dannan::net::message::ok) {
      out.bytes_sent += sizes[i];
      ++out.objects_sent;
    } else {
      _logger.debug("CONTROL object store peer=" + target.host + " error=" + outcome_text(outcomes[i]));
      ok = false;
    }
  }
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
    if (ids.empty() || offer_control_on(target, ids, FrameType::speculative, out)) ++out.held_by;
  }
  return out;
}

bool DistributedStore::retain_control(const std::vector<ObjectId>& input, const contract::RetentionDot& dot, ClaimScope scope) {
  auto ids = input;
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (ids.empty()) return true;
  const size_t sought = scope == ClaimScope::here         ? 1
                        : scope == ClaimScope::every_node ? std::numeric_limits<size_t>::max()
                                                          : std::max<size_t>(1, _options.metadata_write_copies);
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
  bool local_retained = false;
  for (const auto& candidate : active) {
    if (!put_graph_on(candidate)) continue;
    if (retain_on(candidate, contract::RetentionClass::control, ids, dot)) {
      ++retained;
      local_retained = local_retained || candidate.id == _self;
    }
    if (retained >= sought) break;
  }
  return local_retained;
}

}  // namespace dagda::store
