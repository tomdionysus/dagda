//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/spaces.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "dagda/heads/control_node_store.h"
#include "dagda/store/messages.h"

namespace dagda::heads {
namespace {

using dannan::contract::Lock;

contract::RetentionDot retention_dot(const Dot& dot) { return {dot.author, dot.sequence}; }

}  // namespace

std::vector<ObjectId> StoreClaimBarrier::retain_data_here(const std::vector<ObjectId>& ids, const Dot& dot) {
  return _store.retain_data_here(ids, retention_dot(dot));
}

std::vector<ObjectId> StoreClaimBarrier::retain_data(const std::vector<ObjectId>& ids, const Dot& dot) { return _store.retain_data(ids, retention_dot(dot)); }

bool StoreClaimBarrier::retain_control_here(const std::vector<ObjectId>& ids, const Dot& dot) {
  return _store.retain_control(ids, retention_dot(dot), store::DistributedStore::ClaimScope::here);
}

void StoreClaimBarrier::retain_on_peers(const Dot& dot, const CommitClaims& claims) {
  const auto claim = retention_dot(dot);
  if (!claims.data.empty()) (void)_store.retain_data(claims.data, claim);
  if (!claims.control.empty()) (void)_store.retain_control(claims.control, claim, store::DistributedStore::ClaimScope::every_node);
}

Spaces::Spaces(Services services)
    : _services(std::move(services)),
      _barrier(_services.store, [this] { _services.context.events().notify(_services.claims_event); }),
      _guard(_services.context.logger("dagda.heads")),
      _logger(_services.context.logger("dagda.heads")),
      _thread([this](std::stop_token stop) { _services.context.threads().loop("dagda.heads", stop, [this, stop] { worker(stop); }); }) {
  bind_routes();
}

Spaces::~Spaces() { stop(); }

void Spaces::stop() {
  {
    Lock lock(_queue_mutex);
    _stopping.store(true, std::memory_order_release);
  }
  // Routes first, so no request reaches a space while its exchange stops.
  unbind_routes();
  for (auto* exchange : exchanges()) exchange->stop();
  if (_thread.joinable()) {
    _thread.request_stop();
    _queue_changed.notify_all();
    _thread.join();
  }
}

bool Spaces::submit(std::function<void()> job) {
  {
    Lock lock(_queue_mutex);
    if (_stopping.load(std::memory_order_acquire)) return false;
    _queue.push_back(std::move(job));
  }
  _queue_changed.notify_one();
  return true;
}

void Spaces::worker(std::stop_token stop) {
  for (;;) {
    std::function<void()> job;
    {
      Lock lock(_queue_mutex);
      _queue_changed.wait(lock.native(), stop, [&]() DANNAN_REQUIRES(_queue_mutex) { return !_queue.empty(); });
      if (_queue.empty()) return;
      job = std::move(_queue.front());
      _queue.pop_front();
    }
    job();
  }
}

Space& Spaces::open(SpaceSpec spec) {
  if (const auto why = policy_refusal(spec.policy)) throw std::invalid_argument(*why);
  const auto id = id_of(spec.name);
  Lock lock(_mutex);
  if (_spaces.contains(id)) throw std::invalid_argument("head space " + spec.name + " is open already");

  auto seed = std::make_unique<CacheSeed>(_services.cache, _services.content_key, _services.domain, spec.seed_magic, _logger);

  ReplicaOptions replica;
  replica.directory = space_directory(_services.state, id);
  replica.content_key = _services.content_key;
  replica.local_key = _services.local_key;
  replica.magics = spec.magics;
  replica.aad = replica_aad(_services.domain, id);
  replica.codec = spec.codec;
  replica.recovery_seed = seed->load(id);
  // A joiner never holds genesis as authority, whatever the spec says of a founder.
  replica.accept_pristine_genesis_authority = spec.accept_pristine_genesis_authority && !_services.exchange.joiner;
  replica.materialization_cache_limit_bytes = spec.materialization_cache_limit_bytes;
  auto& control = _services.control;
  auto& source = _services.store;
  // Replay writes nodes locally and replicates nothing: the commit reached its holders
  // when made, and rebuilding a local head must not depend on peers.
  replica.tree_store = [&control, &source] { return std::make_unique<ControlTreeNodeStore>(ControlTreeNodeStore::for_replay(control, source)); };
  replica.time = &_services.context.time().source();
  replica.logger = _logger;

  SpaceServices services;
  services.self = _services.context.identity().id();
  services.commit_nodes = [&control, &source] { return std::make_unique<ControlTreeNodeStore>(ControlTreeNodeStore::for_commit(control, source)); };
  services.read_nodes = [&control, &source] { return std::make_unique<ControlTreeNodeStore>(ControlTreeNodeStore::for_reading(control, source)); };
  ClaimBarrier* const barrier = _services.exchange.claims ? _services.exchange.claims : &_barrier;
  services.claims = barrier;
  services.wall = _services.wall;
  services.guard = &_guard;
  services.seed = seed.get();
  services.space = id;
  auto& nodes = _services.context.nodes();
  services.membership_stamp = [&nodes] {
    auto active = nodes.active();
    std::sort(active.begin(), active.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    uint64_t stamp = 1469598103934665603ULL;
    for (const auto& peer : active)
      for (const auto byte : peer.id.bytes) stamp = (stamp ^ byte) * 1099511628211ULL;
    return stamp;
  };

  if (spec.unaddressed && _unaddressed)
    throw std::invalid_argument("head space " + spec.name + " and " + _unaddressed->name() + " both take unaddressed messages");

  auto exchange = std::make_unique<Exchange>(Exchange::Services{_services.context, _services.exchange, id, spec.name, spec.unaddressed, spec.generation_advert,
                                                                spec.write_copies_advert, barrier, _services.wall, _services.garbage_grace, _services.time,
                                                                _services.metadata_event, _services.wake, &_guard});
  auto* exchanging = exchange.get();
  services.converge = [exchanging] { return exchanging->read_base(); };
  services.accepted = [exchanging](const Acceptance& acceptance, bool heads_changed, const Record& committed) {
    exchanging->accepted(acceptance, heads_changed, committed);
  };
  services.owed_changed = [exchanging] { exchanging->wake_replicator(); };
  services.publish_policy = [exchanging](const Record& record) { (void)exchanging->publish_to_active(record); };
  services.view_ttl = _services.exchange.view_ttl;
  services.view_time = _services.exchange.view_time;
  services.remote = [exchanging] { return exchanging->remote_known(); };
  const auto writer = services.self;
  services.scope_refusal = [exchanging, writer] { return exchanging->scope_refusal(writer); };

  const bool unaddressed = spec.unaddressed;
  auto space = std::make_unique<Space>(std::move(spec), std::move(replica), std::move(services));
  auto& out = *space;
  exchanging->attach(out);
  if (unaddressed) _unaddressed = exchanging;
  _opened.push_back(exchanging);
  _exchanges.emplace(id, std::move(exchange));
  _seeds.emplace(id, std::move(seed));
  _spaces.emplace(id, std::move(space));
  return out;
}

Exchange* Spaces::exchange(const SpaceId& space) const {
  Lock lock(_mutex);
  const auto found = _exchanges.find(space);
  return found == _exchanges.end() ? nullptr : found->second.get();
}

std::vector<Exchange*> Spaces::exchanges() const {
  Lock lock(_mutex);
  return _opened;
}

Exchange* Spaces::addressed_to(const std::optional<SpaceId>& space) const {
  if (space) return exchange(*space);
  Lock lock(_mutex);
  return _unaddressed;
}

void Spaces::bind_routes() {
  const auto& n = _services.exchange.numbers;
  _bound = {n.put_history_entry, n.put_commit,   n.accept_commit, n.get_history_entry, n.has_history_entry, n.get_heads, n.get_history_record, n.get_committed,
            n.get_current,       n.get_identity, n.notice};
  for (const auto type : _bound)
    _services.context.routes().bind(type,
                                    [this](const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame, const dannan::net::Message& request) {
                                      if (_services.front) {
                                        if (auto answer = _services.front(peer, frame, request)) return std::move(*answer);
                                      }
                                      return handle(peer, frame, request);
                                    });
}

void Spaces::unbind_routes() {
  for (const auto type : _bound) _services.context.routes().unbind(type);
  _bound.clear();
}

dannan::net::Handled Spaces::handle(const dannan::membership::NodeRecord& peer, dannan::sched::FrameType, const dannan::net::Message& request) {
  using dannan::net::Handled;
  using dannan::net::Message;
  namespace store_message = store::message;
  const auto& n = _services.exchange.numbers;
  const auto not_open = [] { return Handled::failed(503, "space_not_open", "the head space asked of is not open on this node"); };
  const auto yes_no = [](bool answer) { return Handled::processed(Message{store_message::bool_reply, store_message::encode_bool(answer), {}}); };
  const auto payload = std::span<const uint8_t>(request.payload);
  const auto type = request.type;

  if (type == n.put_history_entry || type == n.put_commit) {
    auto entry = message::addressed(payload, [](std::span<const uint8_t> bytes) { return decode_history_entry(bytes); });
    auto* exchange = addressed_to(entry.space);
    if (!exchange) return not_open();
    if (const auto why = exchange->scope_refusal(peer.id)) return Handled::failed(403, "scope_refused", *why);
    const bool imported = exchange->space().replica().import_history(entry.form);
    // An import can prune an accepted head and so move the committed one, with no accept.
    exchange->space().notice_if_moved();
    return yes_no(imported);
  }
  if (type == n.accept_commit) {
    auto acceptance = message::addressed(payload, [](std::span<const uint8_t> bytes) { return decode_acceptance(bytes); });
    auto* exchange = addressed_to(acceptance.space);
    if (!exchange) return not_open();
    if (const auto why = exchange->scope_refusal(peer.id)) return Handled::failed(403, "scope_refused", *why);
    return yes_no(exchange->space().accept(acceptance.form));
  }
  if (type == n.notice) {
    const auto notice = message::addressed(payload, [](std::span<const uint8_t> bytes) { return message::decode_notice(bytes); });
    if (auto* exchange = addressed_to(notice.space)) exchange->note_notice(notice.form);
    return Handled::processed();
  }
  if (type == n.get_history_entry || type == n.get_history_record || type == n.has_history_entry) {
    const auto hash = message::addressed(payload, [](std::span<const uint8_t> bytes) { return message::decode_hash(bytes); });
    auto* exchange = addressed_to(hash.space);
    if (!exchange) return not_open();
    auto& replica = exchange->space().replica();
    if (type == n.has_history_entry) return yes_no(replica.history_contains(hash.form));
    // Repair of a peer's unreconstructable head is served as a whole body, whatever shape
    // is stored.
    const auto entry = type == n.get_history_entry ? replica.history_entry(hash.form) : replica.full_history_record(hash.form);
    if (!entry)
      return Handled::failed(404, "not_found", type == n.get_history_entry ? "metadata history entry unavailable" : "metadata history record unavailable");
    return Handled::processed(Message{n.history_entry_reply, encode_history_entry(*entry), {}});
  }

  auto* exchange = addressed_to(message::space_of_empty(payload));
  if (!exchange) return not_open();
  auto& replica = exchange->space().replica();
  if (type == n.get_heads) return Handled::processed(Message{n.heads_reply, encode_acceptance_set(exchange->certificates()), {}});
  if (type == n.get_committed) return Handled::processed(Message{n.record_reply, encode_record(replica.committed()), {}});
  if (type == n.get_current) return Handled::processed(Message{n.record_reply, encode_record(replica.current()), {}});
  // R11: the committed identity. Macha's case compares it with current_identity(), equal
  // unless a journal is mid-replay.
  if (type == n.get_identity) return Handled::processed(Message{n.identity_reply, message::encode_identity(replica.committed_identity()), {}});
  return Handled::unrecognised();
}

Space* Spaces::find(const SpaceId& space) const {
  Lock lock(_mutex);
  const auto found = _spaces.find(space);
  return found == _spaces.end() ? nullptr : found->second.get();
}

}  // namespace dagda::heads
