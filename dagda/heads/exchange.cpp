//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/exchange.h"

#include <algorithm>
#include <boost/asio/strand.hpp>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "dagda/contract/stopping.h"
#include "dagda/store/messages.h"
#include "dannan/encoding/base.h"
#include "dannan/identity/node_id.h"
#include "dannan/membership/node_record.h"
#include "dannan/placement/placement.h"

namespace dagda::heads {
namespace {

using contract::MaintenanceStopping;
using dannan::contract::Lock;
using dannan::membership::NodeRecord;
using dannan::net::CallOutcome;
using dannan::net::Message;

constexpr auto call_poll = std::chrono::milliseconds(20);
// How long a peer's certificate this replica cannot accept is left alone.
constexpr auto unacceptable_cooldown = std::chrono::seconds(30);
// The in-flight window of one history push: requests to one peer run in arrival order at
// the receiver, so a few overlap latency and appends while bounding what a call holds.
constexpr size_t transfer_window = 8;

// A merge that cannot be made with what this node can reach now.
struct MergeUnavailable : std::runtime_error {
  using std::runtime_error::runtime_error;
};

uint64_t microseconds_since(const dannan::contract::TimeSource& time, dannan::contract::TimePoint from) {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(time.now() - from).count());
}

std::string default_advert(std::string_view what, const SpaceId& space) {
  return "dagda/heads/" + std::string(what) + "/" + dannan::encoding::hex(space.bytes);
}

Exchange::Services resolved(Exchange::Services services) {
  if (services.generation_advert.empty()) services.generation_advert = default_advert("generation", services.space);
  if (services.write_copies_advert.empty()) services.write_copies_advert = default_advert("write-copies", services.space);
  return services;
}

}  // namespace

std::vector<NodeRecord> order_commit_replicas(std::vector<NodeRecord> replicas, const NodeId& local,
                                              const std::function<std::optional<std::chrono::milliseconds>(const NodeId&)>& latency) {
  const auto rank = [&](const NodeRecord& node) -> std::pair<int, int64_t> {
    if (node.id == local) return {0, 0};
    const auto measured = latency ? latency(node.id) : std::nullopt;
    if (measured) return {1, measured->count()};
    return {2, 0};
  };
  std::stable_sort(replicas.begin(), replicas.end(), [&](const NodeRecord& a, const NodeRecord& b) { return rank(a) < rank(b); });
  return replicas;
}

const char* availability_name(Availability availability) noexcept {
  switch (availability) {
    case Availability::unavailable:
      return "unavailable";
    case Availability::read_only:
      return "read-only";
    case Availability::writable:
      return "writable";
  }
  return "unavailable";
}

Exchange::Exchange(Services services)
    : _services(resolved(std::move(services))),
      _options(_services.options),
      _context(_services.context),
      _time(_services.time ? *_services.time : _context.time().source()),
      _logger(_context.logger("dagda.heads")) {}

Exchange::~Exchange() { stop(); }

dannan::sched::RetryPolicy Exchange::retry_policy() const noexcept {
  // Never parked: repair is retried for as long as there is demand.
  return dannan::sched::RetryPolicy::unbounded(_options.retry_backoff, _options.retry_backoff_most);
}

std::chrono::milliseconds Exchange::waited_backoff() const noexcept { return std::max(_retry.current_backoff(), _options.retry_backoff); }

void Exchange::attach(Space& space) {
  _space = &space;
  // Events before this are not replayed; one run validates the state they left.
  _absorbed_metadata = _context.events().count(_services.metadata_event);
  _absorbed_topology = _context.events().count(dannan::status::NodeEvents::membership);
  _last_remote_epoch = remote_epoch();
  _last_local_generation = space.replica().committed_generation();
  _demand.request(known_generation());
  auto& nodes = _context.nodes();
  advertise_generation(space.replica().generation());
  try {
    nodes.advertise(_services.write_copies_advert, dannan::membership::u64_advert(_options.write_copies));
  } catch (const std::invalid_argument& error) {
    // Dannan's cap on a node's adverts; Macha's one space is well under it (the plan's F5).
    _logger.warn("metadata write copies not advertised: " + std::string(error.what()));
  }
  _roster_hold = std::make_shared<RosterHold>();
  {
    Lock lock(_roster_hold->mutex);
    _roster_hold->exchange = this;
  }
  _roster_watch = nodes.watch(boost::asio::make_strand(_context.time().executor()), [hold = _roster_hold](dannan::RosterChange change) {
    Lock lock(hold->mutex);
    if (hold->exchange) hold->exchange->observe_roster(change);
  });
  _replicator =
      std::jthread([this](std::stop_token stop) { _context.threads().loop("dagda.heads.replicator", stop, [this, stop] { replication_loop(stop); }); });
  _adverts = std::jthread([this](std::stop_token stop) { _context.threads().loop("dagda.heads.adverts", stop, [this, stop] { advert_loop(stop); }); });
}

void Exchange::stop() {
  _stopping.store(true, std::memory_order_release);
  if (_roster_hold) {
    _context.nodes().unwatch(_roster_watch);
    {
      Lock lock(_roster_hold->mutex);
      _roster_hold->exchange = nullptr;
    }
    _roster_hold.reset();
  }
  if (_replicator.joinable()) {
    _replicator.request_stop();
    {
      Lock lock(_owed_mutex);
    }
    _owed_changed.notify_all();
    _replicator.join();
  }
  if (_adverts.joinable()) {
    _adverts.request_stop();
    _adverts_wake.notify_all();
    _adverts.join();
  }
}

NodeId Exchange::self() const { return _context.identity().id(); }

Bytes Exchange::addressed(Bytes payload) const {
  return message::with_space(std::move(payload), _services.unaddressed ? std::nullopt : std::optional<SpaceId>(_services.space));
}

// ---- Calls ------------------------------------------------------------------

Exchange::PendingCall Exchange::start_call(const Peer& owner, message::MessageType type, Bytes payload, FrameType frame, std::chrono::milliseconds deadline,
                                           dannan::net::CallWatch watch) {
  if (_stopping.load(std::memory_order_acquire)) throw std::runtime_error("node is stopping; outbound calls are unavailable");
  PendingCall pending;
  pending.outcome = std::make_shared<std::promise<CallOutcome>>();
  pending.future = pending.outcome->get_future();
  pending.handle = _context.routes().call(
      owner, type, std::move(payload), frame, deadline, _context.time().executor(),
      [outcome = pending.outcome](CallOutcome result) { outcome->set_value(std::move(result)); }, watch);
  return pending;
}

CallOutcome Exchange::wait_call(PendingCall& pending) {
  while (pending.future.wait_for(call_poll) != std::future_status::ready) {
    if (_stopping.load(std::memory_order_acquire)) {
      pending.handle->cancel();
      throw std::runtime_error("node is stopping; a call to a peer was cancelled");
    }
  }
  return pending.future.get();
}

CallOutcome Exchange::call(const Peer& owner, message::MessageType type, Bytes payload, FrameType frame) {
  // Waited on here, as Macha's port waits on a synchronous call: its window too.
  auto pending = start_call(owner, type, std::move(payload), frame, _options.call_deadline, dannan::net::CallWatch{_options.call_no_progress, std::nullopt});
  return wait_call(pending);
}

Message Exchange::reply_of(CallOutcome outcome) const {
  if (!outcome.reply) throw std::runtime_error(outcome.failure.empty() ? std::string("no reply") : outcome.failure);
  return std::move(*outcome.reply);
}

void Exchange::note_refusal(const Peer& owner, const Message& reply) {
  const auto refusal = dannan::net::refusal_of(reply);
  if (!refusal) return;
  const auto why = std::to_string(refusal->status) + " " + refusal->code + ": " + refusal->message;
  _logger.debug("metadata refused by peer=" + owner.host + " " + why);
  Lock lock(_refusals_mutex);
  _refusals[owner.id] = why;
}

bool Exchange::bool_of(const Peer& owner, const Message& reply) {
  if (reply.type != store::message::bool_reply) {
    note_refusal(owner, reply);
    return false;
  }
  return store::message::decode_bool(reply.payload);
}

std::vector<PeerRefusal> Exchange::refusals() const {
  Lock lock(_refusals_mutex);
  std::vector<PeerRefusal> out;
  for (const auto& [peer, why] : _refusals) out.push_back({peer, why});
  return out;
}

bool Exchange::stalled(const NodeId& peer) const {
  Lock lock(_stalled_mutex);
  const auto found = _stalled_until.find(peer);
  if (found == _stalled_until.end()) return false;
  if (_time.now() < found->second) return true;
  _stalled_until.erase(found);
  return false;
}

Message Exchange::commit_call(const Peer& owner, message::MessageType type, Bytes payload, FrameType frame) {
  if (stalled(owner.id)) throw std::runtime_error("peer stalled on an earlier commit call");
  auto pending = start_call(owner, type, std::move(payload), frame, _options.call_deadline);
  const auto started = _time.now();
  while (pending.future.wait_for(call_poll) != std::future_status::ready) {
    if (_stopping.load(std::memory_order_acquire)) {
      pending.handle->cancel();
      throw std::runtime_error("node is stopping; a call to a peer was cancelled");
    }
    // A route that cannot say how long it has been idle is judged by how long the call
    // has taken.
    auto quiet = pending.handle->idle_for();
    if (quiet == std::chrono::milliseconds::max()) quiet = std::chrono::duration_cast<std::chrono::milliseconds>(_time.now() - started);
    if (quiet < _options.write_stall) continue;
    pending.handle->cancel();
    {
      Lock lock(_stalled_mutex);
      _stalled_until[owner.id] = _time.now() + _options.dead_after;
    }
    _logger.warn("metadata commit call stalled peer=" + owner.host + " no_progress_ms=" + std::to_string(_options.write_stall.count()) +
                 "; not asked again for " + std::to_string(_options.dead_after.count()) + " ms");
    throw std::runtime_error("peer made no progress on a commit call");
  }
  return reply_of(pending.future.get());
}

// ---- History ----------------------------------------------------------------

bool Exchange::import_history_from_peer(const Peer& owner, const Hash256& target, FrameType frame, const TreePause& pause) {
  auto& local = _space->replica();
  if (local.history_contains(target)) {
    // A peer's full checkpoint may lack an older part of its ancestry, so a hole below a
    // continuous suffix can make a real ancestor look rootless. Heal only while the
    // accepted heads lack a common ancestor, and stop once it is provable. Traverses the
    // resident link index, not payloads.
    const auto remains_rootless = [&] {
      const auto certificates = local.accepted_head_certificates();
      for (size_t left = 0; left < certificates.size(); ++left)
        for (size_t right = left + 1; right < certificates.size(); ++right)
          if (!local.history_common_ancestor(certificates[left].hash, certificates[right].hash)) return true;
      return false;
    };
    if (remains_rootless() && owner.id != self()) {
      std::vector<Hash256> pending{target};
      std::set<Hash256> inspected;
      while (!pending.empty()) {
        if (pause) pause();
        const auto hash = pending.back();
        pending.pop_back();
        if (!inspected.insert(hash).second) continue;
        const auto entry = local.history_links(hash);
        if (!entry) continue;
        std::vector<Hash256> parents = entry->merge_parents;
        if (entry->previous != Hash256{}) parents.push_back(entry->previous);
        for (const auto& parent : parents) {
          if (!local.history_contains(parent)) {
            (void)import_history_from_peer(owner, parent, frame, pause);
            if (!remains_rootless()) return true;
          }
          if (local.history_contains(parent)) pending.push_back(parent);
        }
      }
    }
    return true;
  }
  if (owner.id == self()) return false;

  struct Task {
    Hash256 hash{};
    bool required = false;
    bool loaded = false;
    HistoryEntry entry;
  };
  std::vector<Task> stack;
  std::set<Hash256> active;
  stack.push_back({target, true, false, {}});
  active.insert(target);

  while (!stack.empty()) {
    if (pause) pause();
    auto& task = stack.back();
    if (local.history_contains(task.hash)) {
      active.erase(task.hash);
      stack.pop_back();
      continue;
    }
    if (!task.loaded) {
      try {
        auto reply = reply_of(call(owner, _options.numbers.get_history_entry, addressed(message::encode_hash(task.hash)), frame));
        if (reply.type != _options.numbers.history_entry_reply) throw std::runtime_error("metadata history entry unavailable");
        task.entry = decode_history_entry(reply.payload);
        if (task.entry.hash != task.hash) throw std::runtime_error("metadata history reply identity mismatch");
        task.loaded = true;
      } catch (const MaintenanceStopping&) {
        throw;
      } catch (const std::exception&) {
        const bool required = task.required;
        active.erase(task.hash);
        stack.pop_back();
        if (required) return false;
        continue;
      }

      // A whole body is self-contained: its links are ancestry, not what the transfer
      // needs, and following them would copy unrelated retained history. A delta needs
      // its primary, so follow that chain to the nearest whole body.
      const auto dependencies = materialization_dependencies(task.entry);
      bool pushed = false;
      for (auto it = dependencies.rbegin(); it != dependencies.rend(); ++it) {
        if (local.history_contains(*it)) continue;
        if (!active.insert(*it).second) return false;
        // `task` is not touched past this point: the stack may move it.
        stack.push_back({*it, true, false, {}});
        pushed = true;
      }
      if (pushed) continue;
    }

    auto& ready = stack.back();
    const bool required = ready.required;
    const auto hash = ready.hash;
    const auto entry = ready.entry;
    active.erase(hash);
    stack.pop_back();
    if (!local.import_history(entry) && required) return false;
  }
  return local.history_contains(target);
}

size_t Exchange::repair_unreconstructable_heads(FrameType frame, const TreePause& pause) {
  auto& local = _space->replica();
  auto broken = local.unreconstructable_heads();
  if (broken.empty()) return 0;
  if (broken.size() > _options.repair_heads_per_pass) broken.resize(_options.repair_heads_per_pass);
  std::vector<Peer> peers;
  for (auto& peer : _context.nodes().active())
    if (peer.id != self()) peers.push_back(std::move(peer));
  if (peers.empty()) {
    _logger.debug("metadata head repair: " + std::to_string(broken.size()) + " unreconstructable head(s) flagged but no peer is reachable");
    return 0;
  }

  size_t repaired = 0;
  for (const auto& hash : broken) {
    bool done = false;
    for (const auto& owner : peers) {
      if (pause) pause();
      try {
        auto reply = reply_of(call(owner, _options.numbers.get_history_record, addressed(message::encode_hash(hash)), frame));
        if (reply.type != _options.numbers.history_entry_reply) {
          _logger.debug("metadata head repair: " + owner.host + " cannot serve hash=" + dannan::encoding::hex(hash.bytes));
          continue;
        }
        const auto entry = decode_history_entry(reply.payload);
        if (entry.hash != hash || entry.body != HistoryEntry::Body::full) {
          _logger.warn("metadata head repair: " + owner.host + " returned a mismatched record for hash=" + dannan::encoding::hex(hash.bytes));
          continue;
        }
        if (!local.reanchor_history(entry)) {
          _logger.warn("metadata head repair: record from " + owner.host + " rejected locally hash=" + dannan::encoding::hex(hash.bytes));
          continue;
        }
        _logger.info("metadata accepted head repaired from peer " + owner.host + " hash=" + dannan::encoding::hex(hash.bytes) +
                     " generation=" + std::to_string(entry.generation));
        done = true;
        break;
      } catch (const MaintenanceStopping&) {
        throw;
      } catch (const std::exception& error) {
        _logger.debug("metadata head repair: " + owner.host + ": " + error.what());
      }
    }
    if (done) ++repaired;
  }
  return repaired;
}

bool Exchange::push_history_to_peer(const Peer& owner, const Hash256& target, FrameType frame, const TreePause& pause) {
  auto& local = _space->replica();
  if (owner.id == self()) return local.history_contains(target);
  if (!local.history_contains(target) || stalled(owner.id)) return false;

  std::map<Hash256, bool> remote_presence;
  const auto remote_has = [&](const Hash256& hash) {
    if (const auto found = remote_presence.find(hash); found != remote_presence.end()) return found->second;
    bool present = false;
    try {
      const auto reply = reply_of(call(owner, _options.numbers.has_history_entry, addressed(message::encode_hash(hash)), frame));
      present = bool_of(owner, reply);
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception&) {
    }
    remote_presence.emplace(hash, present);
    return present;
  };

  struct Task {
    Hash256 hash{};
    bool required = false;
    bool expanded = false;
    HistoryEntry entry;
  };
  std::vector<Task> stack;
  std::set<Hash256> active;
  std::set<Hash256> planned;
  std::vector<std::pair<HistoryEntry, bool>> transfer;
  stack.push_back({target, true, false, {}});
  active.insert(target);

  while (!stack.empty()) {
    if (pause) pause();
    const auto hash_now = stack.back().hash;
    if (planned.contains(hash_now) || remote_has(hash_now)) {
      active.erase(hash_now);
      stack.pop_back();
      continue;
    }
    if (!stack.back().expanded) {
      const auto entry = local.history_entry(hash_now);
      if (!entry) {
        const bool required = stack.back().required;
        active.erase(hash_now);
        stack.pop_back();
        if (required) return false;
        continue;
      }
      stack.back().entry = *entry;
      stack.back().expanded = true;
      // As on the pull side: only a delta's primary is required; following optional links
      // would exchange entire retained histories.
      const auto dependencies = materialization_dependencies(*entry);
      bool pushed = false;
      for (auto it = dependencies.rbegin(); it != dependencies.rend(); ++it) {
        if (planned.contains(*it) || remote_has(*it)) continue;
        if (!active.insert(*it).second) return false;
        stack.push_back({*it, true, false, {}});
        pushed = true;
      }
      if (pushed) continue;
    }
    auto& task = stack.back();
    const bool required = task.required;
    transfer.emplace_back(std::move(task.entry), required);
    planned.insert(hash_now);
    active.erase(hash_now);
    stack.pop_back();
  }

  struct PendingTransfer {
    bool required = false;
    PendingCall call;
  };
  std::deque<PendingTransfer> pending;
  _history_transfers.fetch_add(1, std::memory_order_relaxed);

  const auto finish_oldest = [&] {
    auto item = std::move(pending.front());
    pending.pop_front();
    try {
      return bool_of(owner, reply_of(wait_call(item.call))) || !item.required;
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception&) {
      return !item.required;
    }
  };
  const auto note_depth = [&] {
    auto peak = _history_peak_in_flight.load(std::memory_order_relaxed);
    while (peak < pending.size() && !_history_peak_in_flight.compare_exchange_weak(peak, pending.size(), std::memory_order_relaxed)) {
    }
  };

  for (auto& [entry, required] : transfer) {
    if (pause) pause();
    try {
      pending.push_back(
          {required, start_call(owner, _options.numbers.put_history_entry, addressed(encode_history_entry(entry)), frame, _options.call_deadline)});
      _history_entries_submitted.fetch_add(1, std::memory_order_relaxed);
      note_depth();
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception&) {
      if (required) return false;
    }
    if (pending.size() == transfer_window && !finish_oldest()) return false;
  }
  while (!pending.empty())
    if (!finish_oldest()) return false;
  return true;
}

HistoryEntry Exchange::commit_history_entry(const Record& record, std::span<const uint8_t> delta) const {
  if (!valid_record(record)) throw std::runtime_error("cannot publish invalid metadata commit");
  HistoryEntry entry;
  entry.generation = record.generation;
  entry.previous = record.previous;
  entry.hash = record.hash;
  entry.previous_known = record.generation > 1 && record.previous != Hash256{};
  entry.merge_parents = _space->codec()->head_fields(_space->codec()->decode_full(record.payload)).merge_parents;
  if (!delta.empty()) {
    entry.body = HistoryEntry::Body::delta;
    entry.payload.assign(delta.begin(), delta.end());
  } else {
    entry.body = HistoryEntry::Body::full;
    entry.payload.assign(record.payload.begin(), record.payload.end());
  }
  return entry;
}

std::vector<Hash256> Exchange::record_parents(const Record& record) const {
  if (!valid_record(record)) throw DecodeError("invalid metadata record");
  std::vector<Hash256> parents;
  if (record.generation > 1 && record.previous != Hash256{}) parents.push_back(record.previous);
  for (const auto& parent : _space->codec()->head_fields(_space->codec()->decode_full(record.payload)).merge_parents) {
    if (parent == Hash256{} || parent == record.hash) throw DecodeError("bad metadata merge parent");
    if (std::find(parents.begin(), parents.end(), parent) == parents.end()) parents.push_back(parent);
  }
  return parents;
}

bool Exchange::store_commit_on(const Peer& owner, const HistoryEntry& compact, const Record& record, FrameType frame, const TreePause& pause) {
  if (owner.id == self()) {
    auto& local = _space->replica();
    if (compact.body != HistoryEntry::Body::delta) return local.store_commit(record);
    if (local.store_commit(record, compact.payload)) return true;
    // The full record is in the proposal. A compact body can be rejected for a missing
    // parent or a delta that does not reconstruct; fall back once, as a peer does.
    _logger.warn("local metadata delta rejected; retrying full record generation=" + std::to_string(record.generation));
    return local.store_commit(record);
  }
  try {
    if (bool_of(owner, commit_call(owner, _options.numbers.put_commit, addressed(encode_history_entry(compact)), frame))) return true;
    // A valid replica may not yet have the delta's parent. Supply it and retry the compact
    // body before a full fallback, so replica lag does not turn every reconciliation into
    // a full snapshot.
    if (compact.body == HistoryEntry::Body::delta) {
      if (push_history_to_peer(owner, compact.previous, frame, pause)) {
        if (bool_of(owner, commit_call(owner, _options.numbers.put_commit, addressed(encode_history_entry(compact)), frame))) return true;
      }
      const auto full = commit_history_entry(record);
      return bool_of(owner, commit_call(owner, _options.numbers.put_commit, addressed(encode_history_entry(full)), frame));
    }
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (const std::exception& error) {
    _logger.debug("metadata commit store " + owner.host + ": " + error.what());
  }
  return false;
}

bool Exchange::accept_commit_on(const Peer& owner, const Acceptance& acceptance, FrameType frame) {
  // The space detects and announces accepted-head changes for the local path; nothing
  // here announces again.
  if (owner.id == self()) return _space->accept(acceptance);
  try {
    return bool_of(owner, commit_call(owner, _options.numbers.accept_commit, addressed(encode_acceptance(acceptance)), frame));
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (const std::exception& error) {
    _logger.debug("metadata acceptance " + owner.host + ": " + error.what());
    return false;
  }
}

PublishedCommit Exchange::publish_commit(const std::vector<Peer>& nodes, const Record& record, std::span<const uint8_t> delta, FrameType frame,
                                         const TreePause& pause) {
  // A commit is accepted once this node holds it. Further copies are sought from the
  // nodes present and owed to the rest; repair delivers them.
  const size_t sought = _options.write_copies;
  const auto compact = commit_history_entry(record, delta);
  PublishedCommit out;
  out.record = record;

  const auto me = self();
  auto& roster = _context.nodes();
  const auto ordered = order_commit_replicas(nodes, me, [&roster](const NodeId& peer) { return roster.latency(peer); });

  // Store on the nearest until enough hold it or none is left to ask. Receivers validate
  // and append to the DAG without comparing to their head. The local replica must hold it.
  std::vector<Peer> stored_on;
  for (const auto& owner : ordered) {
    if (store_commit_on(owner, compact, record, frame, pause)) stored_on.push_back(owner);
    if (stored_on.size() >= sought) break;
  }
  if (std::none_of(stored_on.begin(), stored_on.end(), [&](const Peer& owner) { return owner.id == me; }))
    throw std::runtime_error("local metadata commit store failed");

  // R5: `required` is how many held it. Never a condition.
  out.acceptance.generation = record.generation;
  out.acceptance.hash = record.hash;
  out.acceptance.required = static_cast<uint32_t>(stored_on.size());
  for (const auto& owner : stored_on) out.acceptance.replicas.push_back(owner.id);
  std::sort(out.acceptance.replicas.begin(), out.acceptance.replicas.end());
  out.acceptance.replicas.erase(std::unique(out.acceptance.replicas.begin(), out.acceptance.replicas.end()), out.acceptance.replicas.end());
  out.stored_on = out.acceptance.replicas;

  // Acceptance is evidence about completed stores, not a second consensus. Each holder
  // first receives the commit's parent history, to reconstruct the branch later.
  const auto parents = record_parents(record);
  // Local first: a peer must never hold an accepted commit its author lacks.
  std::stable_partition(stored_on.begin(), stored_on.end(), [&](const Peer& owner) { return owner.id == me; });
  bool local_accepted = false;
  for (const auto& owner : stored_on) {
    if (owner.id != me && !local_accepted) throw std::runtime_error("local metadata acceptance persistence failed");
    bool ancestry_ready = true;
    if (owner.id != me) {
      for (const auto& parent : parents) {
        if (!push_history_to_peer(owner, parent, frame, pause)) {
          ancestry_ready = false;
          break;
        }
      }
    }
    const bool accepted_here = ancestry_ready && accept_commit_on(owner, out.acceptance, frame);
    if (accepted_here) {
      out.accepted_on.push_back(owner.id);
      if (owner.id == me) local_accepted = true;
    }
  }
  if (!local_accepted) throw std::runtime_error("local metadata acceptance persistence failed");
  for (auto&& refusal : refusals())
    if (std::any_of(ordered.begin(), ordered.end(), [&](const Peer& owner) { return owner.id == refusal.peer; })) out.refused.push_back(std::move(refusal));
  return out;
}

PublishedCommit Exchange::publish_to_active(const Record& record) { return publish_commit(_context.nodes().active(), record, {}, FrameType::control); }

std::vector<Acceptance> Exchange::certificates() const { return _space->replica().accepted_head_certificates(); }

std::vector<std::pair<Exchange::Peer, Acceptance>> Exchange::discover_accepted_heads(const std::vector<Peer>& nodes, FrameType frame) {
  std::vector<std::pair<Peer, Acceptance>> out;
  for (const auto& owner : nodes) {
    try {
      std::vector<Acceptance> heads;
      if (owner.id == self()) {
        heads = certificates();
      } else {
        const auto reply = reply_of(call(owner, _options.numbers.get_heads, addressed({}), frame));
        if (reply.type != _options.numbers.heads_reply) continue;
        heads = decode_acceptance_set(reply.payload);
      }
      for (auto& head : heads) out.emplace_back(owner, std::move(head));
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception& error) {
      _logger.debug("metadata head survey " + owner.host + ": " + error.what());
    }
  }
  return out;
}

bool Exchange::replicate_accepted_head(const Peer& owner, const Record& record, const Acceptance& acceptance, FrameType frame, const TreePause& pause) {
  if (owner.id == self()) {
    if (!_space->replica().store_commit(record)) return false;
    return _space->accept(acceptance);
  }
  if (!push_history_to_peer(owner, record.hash, frame, pause)) {
    // Local history may be compactly rooted at this record; a full commit suffices for any
    // replica without earlier ancestry.
    if (!store_commit_on(owner, commit_history_entry(record), record, frame, pause)) return false;
  }
  return accept_commit_on(owner, acceptance, frame);
}

Record Exchange::read_group(const std::vector<Peer>& nodes, FrameType frame, const TreePause& pause) {
  if (nodes.empty()) throw HeadsNotReady("metadata replicas unavailable");

  struct ObservedHead {
    std::vector<Acceptance> certificates;
    std::vector<Peer> owners;
  };
  std::map<Hash256, ObservedHead> observed;
  for (auto& [owner, acceptance] : discover_accepted_heads(nodes, frame)) {
    auto& head = observed[acceptance.hash];
    if (std::none_of(head.owners.begin(), head.owners.end(), [&](const Peer& value) { return value.id == owner.id; })) head.owners.push_back(owner);
    if (std::find(head.certificates.begin(), head.certificates.end(), acceptance) == head.certificates.end())
      head.certificates.push_back(std::move(acceptance));
  }
  if (observed.empty()) throw HeadsNotReady("no accepted metadata heads available");

  // Import each accepted head independently: the receiver stores the DAG material and the
  // certificate, and is never asked to replace its head.
  auto& local = _space->replica();
  for (const auto& [hash, head] : observed) {
    if (pause) pause();
    {
      Lock lock(_unacceptable_mutex);
      const auto found = _unacceptable_until.find(hash);
      if (found != _unacceptable_until.end() && _time.now() < found->second) continue;
    }
    std::vector<Peer> history_sources = head.owners;
    for (const auto& certificate : head.certificates) {
      for (const auto& witness : certificate.replicas) {
        const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const Peer& peer) { return peer.id == witness; });
        if (found != nodes.end() && std::none_of(history_sources.begin(), history_sources.end(), [&](const Peer& peer) { return peer.id == found->id; }))
          history_sources.push_back(*found);
      }
    }
    bool imported = local.history_contains(hash);
    for (const auto& owner : history_sources) {
      if (owner.id == self()) continue;
      imported = import_history_from_peer(owner, hash, frame, pause) || imported;
    }
    if (!imported) continue;
    // The owner's admission of a peer's head (D6's HLC), judged against the peer that holds it:
    // refused, it waits out the cooldown as an unacceptable head does, with its reason.
    std::optional<std::string> refused;
    for (const auto& owner : head.owners) {
      if (owner.id == self()) continue;
      refused = _space->admit(hash, owner.id);
      break;
    }
    bool accepted = false;
    if (!refused)
      for (const auto& certificate : head.certificates) accepted = _space->accept(certificate) || accepted;
    if (!accepted) {
      {
        Lock lock(_unacceptable_mutex);
        _unacceptable_until[hash] = _time.now() + unacceptable_cooldown;
      }
      if (refused)
        _logger.warn("not admitting metadata head hash=" + dannan::encoding::hex(hash.bytes) + ": " + *refused);
      else
        _logger.warn("ignoring metadata head without a valid acceptance certificate hash=" + dannan::encoding::hex(hash.bytes));
    } else {
      {
        Lock lock(_unacceptable_mutex);
        _unacceptable_until.erase(hash);
      }
      // This certificate may expose a second head whose history this node lacks; revisit
      // it so the rootless healer fetches the missing records now.
      for (const auto& owner : history_sources)
        if (owner.id != self()) (void)import_history_from_peer(owner, hash, frame, pause);
    }
  }

  // Taken lazily on seeing more than one head; heads are re-read once held. Guards no state,
  // so the analysis need not see it held.
  std::optional<Lock> reconciliation;
  for (;;) {
    if (pause) pause();
    auto heads = _space->usable_heads();
    if (heads.empty()) throw HeadsNotReady("metadata accepted-head set is empty");
    std::sort(heads.begin(), heads.end(), [](const Record& a, const Record& b) {
      if (a.hash != b.hash) return a.hash < b.hash;
      return a.generation < b.generation;
    });

    if (heads.size() == 1) {
      const auto& selected = heads.front();
      const auto materialized = local.materialized(selected.hash);
      if (!materialized) throw HeadsNotReady("selected metadata head cannot be materialized");
      return _space->cache_record(selected, materialized->state);
    }

    // A read falls back to this node's own head, as Macha's read_record does on any failure,
    // and a run stays dirty.
    if (!_space->merge())
      throw HeadsNotReady("metadata histories have diverged: " + std::to_string(heads.size()) + " heads, and this space's owner gave no merge");
    if (!reconciliation) {
      const auto waited = _time.now();
      reconciliation.emplace(_reconciliation_mutex);
      _context.observations().record("heads.reconcile_wait_us", microseconds_since(_time, waited));
      continue;  // The heads again, now that this holds the lock: they may be resolved.
    }

    // The maximal head set folded deterministically, two branches at a time. A merge that
    // cannot be made now never holds reads or writes: the head that is not this node's own is
    // set aside and the loop goes on with the rest. An accepted merge drops its parents from
    // the head set, so the loop folds what remains into it.
    const auto aside = [&](std::string_view reason) {
      const auto own = _space->own_head({heads[0], heads[1]});
      if (_space->set_aside(own.hash == heads[0].hash ? heads[1] : heads[0], reason)) _heads_set_aside.fetch_add(1, std::memory_order_relaxed);
    };
    try {
      reconcile(nodes, heads[0], heads[1], frame, pause, heads.size() - 1);
    } catch (const MergeUnavailable& error) {
      aside(error.what());
    } catch (const DecodeError& error) {
      // A tree node of one branch that no node present can supply.
      aside(error.what());
    }
  }
}

void Exchange::reconcile(const std::vector<Peer>& nodes, const Record& left, const Record& right, FrameType frame, const TreePause& pause, size_t remaining) {
  const auto started = _time.now();
  // Where a reconciliation's time goes, stage by stage.
  auto stage_started = started;
  const auto stage_ms = [&] {
    const auto now = _time.now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - stage_started).count();
    stage_started = now;
    return ms;
  };

  auto& local = _space->replica();
  const auto& codec = *_space->codec();
  const auto& merge = *_space->merge();
  const auto left_materialized = local.materialized(left.hash);
  const auto right_materialized = local.materialized(right.hash);
  if (!left_materialized || !right_materialized) throw MergeUnavailable("metadata merge head cannot be materialized");
  const auto materialise_ms = stage_ms();
  const auto left_fields = codec.head_fields(left_materialized->state);
  const auto right_fields = codec.head_fields(right_materialized->state);
  if (_space->spec().tree && (!left_fields.root || !right_fields.root)) throw MergeUnavailable("a metadata head to merge holds no namespace tree");

  std::optional<State> base;
  if (merge.reads_base())
    if (const auto common = local.history_common_ancestor(left.hash, right.hash))
      if (const auto ancestor = local.materialized(*common)) base = ancestor->state;

  const auto reader = _space->_services.read_nodes();
  const auto commit_nodes = _space->commit_nodes();
  // `left` is the primary parent, the lower hash, so every reconciler of the same head pair
  // produces the same merge commit. A merge is a pure join, not a mutation: a fresh dot of this
  // node's in its clock would make simultaneous reconcilers mint sibling merges for ever.
  auto result = merge.merge(base, MergeHeads{left, right, left_materialized->state, right_materialized->state, *reader, *commit_nodes, pause});
  if (!result.state) throw MergeUnavailable(result.why.empty() ? "the owner's merge gave no state" : result.why);
  const auto merged_fields = codec.head_fields(result.state);
  if (merged_fields.merge_parents != std::vector<Hash256>{right.hash}) throw std::logic_error("a merge names exactly its secondary as its merge parent");
  const auto merge_ms = stage_ms();

  Record reconciliation;
  reconciliation.generation = std::max(left.generation, right.generation) + 1;
  reconciliation.previous = left.hash;
  reconciliation.payload = codec.encode_full(result.state);
  reconciliation.hash = record_hash(reconciliation.generation, reconciliation.previous, reconciliation.payload);
  if (auto* costs = _space->_costs) {
    costs->record_bytes_encoded.fetch_add(reconciliation.payload.size(), std::memory_order_relaxed);
    costs->record_bytes_hashed.fetch_add(reconciliation.payload.size(), std::memory_order_relaxed);
  }
  Bytes delta;
  if (!result.delta.empty() && result.delta.size() < reconciliation.payload.size()) delta = std::move(result.delta);
  const auto encode_ms = stage_ms();

  // The merge claims what it introduces before it is published, as a mutation does. This is
  // one route by which an object regains a claim it has lost. Its dot is a fresh local sequence
  // the merged clock does not carry (R3), so every reconciler still mints the same commit and
  // no earlier release here can void the claim; the head that carries this node's next
  // mutation covers it.
  if (_space->_services.claims) {
    const auto dot = local.reserve_mutation_dot(_space->_services.self, merged_fields.clock);
    Introduced introduced;
    introduced.data = result.claims.data;
    introduced.control = result.claims.control;
    const auto claims = _space->claim(dot, left_fields, merged_fields, introduced, *reader, pause);
    // A merge is made off every caller's path, so the peers' share is done here too, before
    // the merge is offered to them.
    if (_services.claims) {
      try {
        _services.claims->retain_on_peers(dot, claims);
      } catch (const MaintenanceStopping&) {
        throw;
      } catch (const std::exception& error) {
        _logger.debug("metadata merge claims on peers deferred: " + std::string(error.what()));
      }
    }
  }
  const auto retention_ms = stage_ms();
  (void)publish_commit(nodes, reconciliation, delta, frame, pause);
  const auto publish_ms = stage_ms();

  _reconciliations.fetch_add(1, std::memory_order_relaxed);
  _conflicts_created.fetch_add(result.conflicts_created, std::memory_order_relaxed);
  _conflicts_superseded.fetch_add(result.conflicts_superseded, std::memory_order_relaxed);
  const auto took_us = microseconds_since(_time, started);
  _context.observations().record("heads.reconcile_us", took_us);
  _logger.info("metadata histories reconciled generation=" + std::to_string(reconciliation.generation) + " ms=" + std::to_string(took_us / 1000) +
               " materialise_ms=" + std::to_string(materialise_ms) + " merge_ms=" + std::to_string(merge_ms) + " encode_ms=" + std::to_string(encode_ms) +
               " retention_ms=" + std::to_string(retention_ms) + " publish_ms=" + std::to_string(publish_ms) +
               " history_body=" + std::string(delta.empty() ? "full" : "delta") +
               " history_bytes=" + std::to_string(delta.empty() ? reconciliation.payload.size() : delta.size()) +
               " conflicts=" + std::to_string(result.conflicts_created) + " superseded=" + std::to_string(result.conflicts_superseded) +
               " standing=" + std::to_string(result.conflicts_standing) + " remaining_heads=" + std::to_string(remaining));
  merge.published(result, reconciliation);
}

Exchange::RecoverySurvey Exchange::recover_from_committed_checkpoints(const std::vector<Peer>& active) {
  RecoverySurvey survey;
  if (active.empty()) return survey;

  struct PendingRead {
    PendingCall call;
    bool done = false;
  };
  std::vector<PendingRead> pending;
  size_t failed = 0;
  bool durable_history = false;

  for (const auto& owner : active) {
    if (owner.id == self()) {
      durable_history = durable_history || _space->replica().committed().generation > 1;
      continue;
    }
    try {
      pending.push_back({start_call(owner, _options.numbers.get_committed, addressed({}), FrameType::control, _options.call_deadline), false});
    } catch (const std::exception&) {
      ++failed;
    }
  }
  // Each call completes by its deadline, so this does too.
  for (auto& item : pending) {
    try {
      const auto reply = reply_of(wait_call(item.call));
      if (reply.type != _options.numbers.record_reply) {
        ++failed;
        continue;
      }
      durable_history = durable_history || decode_record(reply.payload).generation > 1;
    } catch (const std::exception&) {
      ++failed;
    }
  }
  // A joiner may form a virgin space only once every active bootstrap peer has shown no
  // durable post-genesis history exists.
  survey.complete = failed == 0;
  survey.durable_history = durable_history;
  return survey;
}

Record Exchange::discover_or_form(const TreePause& pause) {
  const auto active = _context.nodes().active();
  if (_options.joiner && active.size() == 1) throw HeadsNotReady("metadata replica set forming: waiting for bootstrap peer");

  // Import any post-genesis accepted commit, and siblings, through the ordinary path.
  for (const auto& [owner, acceptance] : discover_accepted_heads(active, FrameType::control))
    if (acceptance.generation > 1) return read_group(active, FrameType::control, pause);

  if (_options.joiner) {
    const auto survey = recover_from_committed_checkpoints(active);
    if (!survey.complete) throw HeadsNotReady("metadata replica set forming: waiting for bootstrap checkpoint survey");
    if (survey.durable_history) return read_group(active, FrameType::control, pause);
  }

  const auto& founding = _space->spec().founding;
  if (!founding) throw HeadsNotReady("no head here yet, and this space's owner founds none here");

  const auto base = genesis_record(_space->codec()->genesis());
  // Founders agree with no coordinator: the state is a function of what the owner was
  // configured with, so its tree is too.
  const auto nodes = _space->commit_nodes();
  State state = founding(*nodes);
  Record formed;
  formed.generation = base.generation + 1;
  formed.previous = base.hash;
  formed.payload = _space->codec()->encode_full(state);
  formed.hash = record_hash(formed.generation, formed.previous, formed.payload);
  (void)publish_commit(active, formed, {}, FrameType::control);
  _logger.info("metadata replica set formed active=" + std::to_string(active.size()));
  return _space->cache_record(formed, std::move(state));
}

Record Exchange::read_base() {
  if (_space->replica().committed().generation <= 1) return discover_or_form();
  return read_group(_context.nodes().active(), FrameType::control);
}

// ---- Adverts and notices ----------------------------------------------------

void Exchange::advertise_generation(uint64_t generation) {
  _advertised.store(generation, std::memory_order_relaxed);
  try {
    _context.nodes().advertise(_services.generation_advert, dannan::membership::u64_advert(generation));
  } catch (const std::invalid_argument& error) {
    _logger.debug("metadata generation not advertised: " + std::string(error.what()));
  }
}

void Exchange::notify_metadata() {
  _context.events().notify(_services.metadata_event);
  if (_services.wake) _services.wake();
}

void Exchange::announce(uint64_t generation) {
  // The accepted-head set can change without a higher generation: a concurrent
  // same-generation sibling. A space's cached views key off this epoch too, so advance it
  // before broadcasting, or a node serves its pre-sibling view until the TTL ends.
  _announcements.fetch_add(1, std::memory_order_relaxed);
  _remote_epoch.fetch_add(1, std::memory_order_acq_rel);
  notify_metadata();
  advertise_generation(generation);
  _context.channels().broadcast({_options.numbers.notice, addressed(message::encode_notice(generation)), {}});
}

void Exchange::accepted(const Acceptance& acceptance, bool heads_changed, const Record& committed) {
  const auto generation = std::max(committed.generation, acceptance.generation);
  advertise_generation(generation);
  // Decided under the replica's lock: a same-generation sibling may leave the preferred
  // head unchanged, and peers still need the wake-up.
  if (heads_changed) announce(generation);
}

void Exchange::raise_remote(uint64_t generation) noexcept {
  auto current = _remote_generation.load(std::memory_order_relaxed);
  while (current < generation && !_remote_generation.compare_exchange_weak(current, generation)) {
  }
}

void Exchange::note_notice(uint64_t generation) {
  auto current = _remote_generation.load(std::memory_order_relaxed);
  while (current < generation && !_remote_generation.compare_exchange_weak(current, generation)) {
  }
  // A notice is sent only when a peer's head set changes, so an equal generation can still
  // carry a sibling. Only one made stale by a strictly newer generation is ignored.
  if (current <= generation) {
    _remote_epoch.fetch_add(1, std::memory_order_acq_rel);
    notify_metadata();
  }
}

uint64_t Exchange::remote_generation() const noexcept {
  // A generation no higher than a head set aside is that head again, not news.
  const auto remote = _remote_generation.load(std::memory_order_acquire);
  return _space && remote > _space->replica().set_aside_generation() ? remote : 0;
}

uint64_t Exchange::known_generation() const { return std::max(_space->replica().generation(), remote_generation()); }

RemoteKnown Exchange::remote_known() const noexcept { return {remote_epoch(), remote_generation()}; }

std::optional<std::string> Exchange::scope_refusal(const NodeId& writer) const {
  const dannan::contract::Principal principal{dannan::contract::Principal::Kind::node, dannan::identity::to_string(writer)};
  const auto decision = _context.identity().verify(principal, _services.name);
  if (decision.allowed) return std::nullopt;
  return decision.why + " (scope generation " + std::to_string(decision.generation) + ")";
}

void Exchange::observe_roster(const dannan::RosterChange& change) {
  if (change.kind == dannan::RosterChange::Kind::left || change.node.id == self()) return;
  bool moved = false;
  if (const auto generation = change.node.advert_u64(_services.generation_advert)) {
    const auto before = _remote_generation.load(std::memory_order_relaxed);
    raise_remote(*generation);
    moved = *generation > before;
  }
  bool write_copies_changed = false;
  {
    const auto copies = change.node.advert_u64(_services.write_copies_advert);
    Lock lock(_roster_mutex);
    const auto [at, inserted] = _peer_write_copies.try_emplace(change.node.id, copies);
    if (!inserted && at->second != copies) {
      at->second = copies;
      write_copies_changed = true;
    }
  }
  // A mismatch marks membership changed and does not fail closed (H28); Dannan's own
  // membership event leaves adverts out, and this one is what makes a peer re-ask.
  if (write_copies_changed) {
    _context.events().notify(dannan::status::NodeEvents::membership);
    if (_services.wake) _services.wake();
  }
  if (moved) notify_metadata();
}

void Exchange::advert_loop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(_adverts_wait);
      _adverts_wake.wait_for(lock, stop, _options.advert_interval, [] { return false; });
    }
    if (stop.stop_requested()) return;
    advertise_generation(_space->replica().generation());
  }
}

// ---- The replicator ---------------------------------------------------------

void Exchange::replication_loop(std::stop_token stop) {
  const TreePause pause = [&stop] {
    if (stop.stop_requested()) throw MaintenanceStopping{};
  };
  for (;;) {
    OwedWork owed;
    {
      Lock lock(_owed_mutex);
      _owed_changed.wait(lock.native(), stop, [&]() DANNAN_REQUIRES(_owed_mutex) { return _space->owed_commits() > 0; });
      if (stop.stop_requested()) return;
      owed = _space->take_owed();
      _replicating = true;
    }
    try {
      // Each commit's claims on the peers, in order, then the newest head once: its history
      // carries the commits before it.
      for (const auto& claims : owed.claims) {
        if (stop.stop_requested()) break;
        if (!_services.claims) continue;
        try {
          _services.claims->retain_on_peers(claims.dot, claims.claims);
        } catch (const MaintenanceStopping&) {
          throw;
        } catch (const std::exception& error) {
          _logger.debug("metadata claims on peers deferred sequence=" + std::to_string(claims.dot.sequence) + " error=" + error.what());
        }
      }
      if (owed.head && !stop.stop_requested()) {
        // To every node present, not only the copies a commit seeks: nobody is waiting on
        // this.
        const auto acceptance = _space->replica().acceptance(owed.head->hash);
        for (const auto& owner : _context.nodes().active()) {
          if (!acceptance || owner.id == self() || stop.stop_requested()) continue;
          try {
            if (!replicate_accepted_head(owner, *owed.head, *acceptance, FrameType::read_ahead, pause))
              _logger.debug("metadata head offer deferred peer=" + owner.host + " generation=" + std::to_string(owed.head->generation));
          } catch (const MaintenanceStopping&) {
            throw;
          } catch (const std::exception& error) {
            _logger.debug("metadata head offer deferred peer=" + owner.host + " generation=" + std::to_string(owed.head->generation) +
                          " error=" + error.what());
          }
        }
      }
    } catch (const MaintenanceStopping&) {
      Lock lock(_owed_mutex);
      _replicating = false;
      _owed_changed.notify_all();
      return;
    }
    {
      Lock lock(_owed_mutex);
      _replicating = false;
    }
    _owed_changed.notify_all();
  }
}

size_t Exchange::replication_pending() const {
  Lock lock(_owed_mutex);
  return _space->owed_commits() + (_replicating ? 1 : 0);
}

bool Exchange::wait_replicated(std::chrono::milliseconds timeout) {
  Lock lock(_owed_mutex);
  return _owed_changed.wait_for(lock.native(), timeout, [&]() DANNAN_REQUIRES(_owed_mutex) { return _space->owed_commits() == 0 && !_replicating; });
}

void Exchange::wake_replicator() {
  // Taken so a replicator between its check and its wait cannot miss this.
  {
    Lock lock(_owed_mutex);
  }
  _owed_changed.notify_all();
}

// ---- Repair -----------------------------------------------------------------

void Exchange::enter(const dannan::sched::WorkContext& work, dannan::sched::Waits declared, std::string_view operation) const {
  if (_services.guard) (void)_services.guard->enter(work, declared, operation);
}

void Exchange::repair_once(const dannan::sched::WorkContext& work, const TreePause& pause) {
  enter(work, repair_waits, "heads::Exchange::repair_once");
  repair_once(pause);
}

size_t Exchange::repair_unreconstructable_heads(const dannan::sched::WorkContext& work, FrameType frame, const TreePause& pause) {
  enter(work, head_repair_waits, "heads::Exchange::repair_unreconstructable_heads");
  return repair_unreconstructable_heads(frame, pause);
}

void Exchange::truncate_history(const dannan::sched::WorkContext& work) {
  enter(work, truncate_waits, "heads::Exchange::truncate_history");
  truncate_history();
}

void Exchange::repair_once(const TreePause& pause) {
  std::vector<Peer> active;
  Record record;
  std::optional<Acceptance> acceptance;
  {
    // Must not race a foreground mutation through discovery, head selection or
    // reconfiguration.
    Lock mutation(_space->_mutation_mutex);
    active = _context.nodes().active();
    if (active.empty()) throw HeadsNotReady("metadata replicas unavailable");
    if (_space->replica().committed().generation <= 1) {
      record = discover_or_form(pause);
    } else {
      record = _space->maybe_reconfigure(read_group(active, FrameType::speculative, pause));
    }
    acceptance = _space->replica().acceptance(record.hash);
    if (!acceptance) throw HeadsNotReady("selected metadata head has no acceptance certificate");
  }

  // Convergence is replication, not head replacement: every active node is offered the head
  // and its proof; a node on another branch keeps it as a second head to reconcile. Runs
  // without the mutation lock. A node known only through gossip cannot be offered anything
  // from here and does not hold convergence back.
  const auto selected_generation = record.generation;
  bool incomplete = false;
  uint64_t holders = 0;
  for (const auto& owner : active) {
    if (pause) pause();
    if (replicate_accepted_head(owner, record, *acceptance, FrameType::speculative, pause))
      ++holders;
    else if (_context.nodes().directly_reachable(owner.id))
      incomplete = true;
  }
  _head_holders.store(holders, std::memory_order_relaxed);
  _head_present.store(active.size(), std::memory_order_relaxed);
  Lock mutation(_space->_mutation_mutex);
  if (incomplete) throw HeadsNotReady("metadata accepted-head replication incomplete");
  if (_space->replica().committed_generation() > selected_generation) return;
  const auto materialized = _space->replica().materialized(record.hash);
  if (!materialized) throw HeadsNotReady("metadata repair head cannot be materialized");
  // Release follows each node's sole accepted head and its clock; unknown concurrent claim
  // dots survive.
  _space->replica().compact();
  (void)_space->cache_record(record, materialized->state);
}

void Exchange::truncate_history(size_t record_threshold, uint64_t byte_threshold) {
  // Serialised against foreground mutations: both read and change the accepted-head set.
  Lock mutation(_space->_mutation_mutex);
  auto& replica = _space->replica();
  if (replica.recovery_required()) return;
  if (const auto dropped = replica.expire_set_aside(_services.wall ? _services.wall->unix_ms() : _context.time().unix_ms(), _services.garbage_grace))
    _logger.warn("metadata: dropped " + std::to_string(dropped) + " head(s) set aside for the absence horizon; their content never arrived");
  // A node's history is its own: once it holds one head and the history has grown past the
  // thresholds, the head becomes its root. Nothing a peer may still hold depends on it, as
  // heads merge without ancestry.
  (void)replica.compact_history_if_safe(record_threshold, byte_threshold);
}

void Exchange::truncate_history() { truncate_history(_options.truncate_records, _options.truncate_bytes); }

void Exchange::request_demand() {
  _demand.request(known_generation());
  if (_services.wake) _services.wake();
}

void Exchange::absorb_events() {
  const auto& events = _context.events();
  const auto metadata = events.count(_services.metadata_event);
  const auto topology = events.count(dannan::status::NodeEvents::membership);
  if (metadata != _absorbed_metadata || topology != _absorbed_topology) {
    if (!_demand.request(known_generation())) note_retry_trigger(_time.now());
  }
  _absorbed_metadata = metadata;
  _absorbed_topology = topology;
}

void Exchange::note_retry_trigger(dannan::contract::TimePoint now) {
  if (!_retry_first) _retry_first = now;
  _retry_last = now;
}

std::optional<dannan::contract::TimePoint> Exchange::retry_trigger_due() const {
  if (!_retry_first) return std::nullopt;
  if (!_failed) return *_retry_first;
  return std::min(*_retry_first + _options.convergence_max_delay, _retry_last + _options.convergence_settle);
}

std::optional<dannan::contract::TimePoint> Exchange::turn_due() const {
  if (!_demand.pending()) return std::nullopt;
  const auto now = _time.now();
  auto due = _retry_due == dannan::contract::TimePoint{} ? now : _retry_due;
  if (const auto trigger = retry_trigger_due()) due = std::min(due, std::max(*trigger, now));
  return due;
}

void Exchange::enter_stage(std::string_view stage) const {
  if (_options.stage) _options.stage(stage);
}

RepairTurn Exchange::repair_turn(std::stop_token stop, bool forced) {
  RepairTurn turn;
  const auto now = _time.now();
  absorb_events();
  turn.dirty = _demand.pending();

  // Whether this node's generation moved while a run stood: a burst's trigger.
  const auto local_generation = _space->replica().committed_generation();
  if (local_generation != _last_local_generation) {
    if (turn.dirty) note_retry_trigger(now);
    _last_local_generation = local_generation;
  }
  std::vector<NodeId> active_nodes;
  bool established_peer = false;
  for (const auto& peer : _context.nodes().active()) {
    active_nodes.push_back(peer.id);
    if (peer.id == self()) continue;
    const auto generation = peer.advert_u64(_services.generation_advert);
    established_peer = established_peer || (generation && *generation > 1);
  }
  std::sort(active_nodes.begin(), active_nodes.end());
  const auto epoch = remote_epoch();
  const bool topology_changed = active_nodes != _last_active;
  if (epoch != _last_remote_epoch || topology_changed) {
    if (turn.dirty) note_retry_trigger(now);
    if (topology_changed) _formation_settle_due = now + _options.dead_after;
    _last_remote_epoch = epoch;
    _last_active = std::move(active_nodes);
  }
  if (const auto due = retry_trigger_due(); due && now >= *due) {
    _retry_due = {};
    _retry_first.reset();
  }

  if (!turn.dirty) return turn;
  if (!forced && _retry_due != dannan::contract::TimePoint{} && now < _retry_due) return turn;

  auto& replica = _space->replica();
  const bool virgin = replica.committed_generation() <= 1;
  // A space its owner founds elsewhere, or by hand, has no virgin run to wait for.
  if (virgin && !_space->spec().founding && !established_peer) {
    turn.waited = true;
    _retry_due = now + waited_backoff();
    return turn;
  }
  if (virgin && !established_peer && !forced && now < _formation_settle_due) {
    turn.waited = true;
    _retry_due = _formation_settle_due;
    return turn;
  }
  // Exactly one deterministic founder forms generation 2. Followers stay event-driven and
  // wake on its notice; the retry deadline only covers a founder that disappeared without a
  // last connectivity event.
  if (virgin && !established_peer && !_last_active.empty() && self() != *std::min_element(_last_active.begin(), _last_active.end())) {
    turn.waited = true;
    _retry_due = _time.now() + waited_backoff();
    return turn;
  }

  const auto run = _demand.begin();
  // This run covers every demand so far.
  _retry_first.reset();
  _failed = false;
  if (!run) {
    turn.dirty = false;
    return turn;
  }
  enter_stage("metadata-repair");
  const auto pause = [&stop] {
    if (stop.stop_requested()) throw MaintenanceStopping{};
  };
  // A failed run: retried after a backoff, and repair goes on beside it unless the
  // replica cannot yet be a source. `why` empty is Macha's line without a reason.
  const auto deferred = [&](const std::string& validation, const std::string& why) {
    note_validation(false, validation);
    _failed = true;
    turn.ran = true;
    turn.failed = true;
    turn.why = why.empty() ? validation : why;
    const auto failed_at = _time.now();
    _retry_due = failed_at + _retry.failed(retry_policy(), failed_at).value_or(_options.retry_backoff_most);
    // Failing to reconcile every active head must not stall non-destructive repair: a
    // locally committed branch is still a valid reachability source.
    if (replica.recovery_required() || replica.committed_generation() <= 1)
      turn.hold_pass = true;
    else
      _logger.debug("metadata repair deferred; continuing non-destructive maintenance" + (why.empty() ? std::string() : ": " + why));
  };
  try {
    enter_stage("metadata-repair-begin");
    repair_once(dannan::sched::WorkContext(FrameType::speculative, {}, nullptr, "heads::metadata-repair"), pause);
    note_validation(true);
    turn.dirty = _demand.complete(*run);
    turn.ran = true;
    _retry_due = {};
    _retry.succeeded();
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (const std::exception& error) {
    deferred(error.what(), error.what());
  } catch (...) {
    // Macha's catch of anything else (maintenance.cpp:517-531 at 98b816d): no reason to give.
    deferred("metadata validation failed", {});
  }
  return turn;
}

void Exchange::note_validation(bool validated, std::string_view reason) { publish_replica_state(validated, reason); }

void Exchange::publish_replica_state(bool validated, std::string_view reason) {
  // Every known node is capable and any node may accept a mutation alone; copies on other
  // nodes are sought, never required.
  const auto view = _space->current();
  const auto membership = _context.nodes().snapshot();
  const size_t required = _options.write_copies;
  const size_t known = membership.all.size();
  const size_t online = membership.active.size();
  const uint64_t generation = view ? view->generation : 0;

  // Validation describes convergence, not write authority: a local committed branch
  // suffices to mutate, and divergent peers are reconciled on the mutation and read paths.
  const auto next = view ? Availability::writable : Availability::unavailable;

  _status_generation.store(generation, std::memory_order_release);
  _status_replicas.store(static_cast<uint32_t>(known), std::memory_order_release);
  _status_replicas_online.store(static_cast<uint32_t>(online), std::memory_order_release);
  _status_write_required.store(static_cast<uint32_t>(required), std::memory_order_release);
  _status_observed_unix_ms.store(_context.time().unix_ms(), std::memory_order_release);
  _status_write_available.store(next == Availability::writable, std::memory_order_release);
  _status_stable.store(validated, std::memory_order_release);

  const auto previous = _status_availability.exchange(next, std::memory_order_acq_rel);
  if (previous == next) return;

  std::string transition;
  if (next == Availability::writable) {
    transition = validated ? "local metadata state ready" : "local metadata state ready; reconciliation pending";
  } else if (!reason.empty()) {
    transition.assign(reason);
  } else {
    transition = "coherent metadata unavailable";
  }
  std::string line = "metadata availability changed state=" + std::string(availability_name(next)) + " previous=" + availability_name(previous) + " reason=\"" +
                     transition + "\"";
  if (generation)
    line += " generation=" + std::to_string(generation) + " replicas=" + std::to_string(online) + "/" + std::to_string(known) +
            " required=" + std::to_string(required);
  if (next == Availability::writable || (previous == Availability::unavailable && next == Availability::read_only))
    _logger.info(line);
  else
    _logger.warn(line);
}

ExchangeStatus Exchange::status() const noexcept {
  ExchangeStatus out;
  out.generation = _status_generation.load(std::memory_order_acquire);
  out.observed_unix_ms = _status_observed_unix_ms.load(std::memory_order_acquire);
  out.replicas = _status_replicas.load(std::memory_order_acquire);
  out.replicas_online = _status_replicas_online.load(std::memory_order_acquire);
  out.write_replicas_required = _status_write_required.load(std::memory_order_acquire);
  out.availability = _status_availability.load(std::memory_order_acquire);
  out.stable = _status_stable.load(std::memory_order_acquire);
  out.write_available = _status_write_available.load(std::memory_order_acquire);
  out.head_holders = _head_holders.load(std::memory_order_relaxed);
  out.head_present = _head_present.load(std::memory_order_relaxed);
  return out;
}

}  // namespace dagda::heads
