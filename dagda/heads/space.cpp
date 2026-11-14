//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/space.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <chrono>
#include <limits>
#include <set>
#include <thread>
#include <utility>

#include "dagda/contract/stopping.h"

namespace dagda::heads {
namespace {

using contract::MaintenanceStopping;
using dannan::contract::Lock;

// Nanoseconds, from a wall clock that keeps milliseconds.
int64_t wall_ns(const dannan::contract::WallClock& wall) { return static_cast<int64_t>(wall.unix_ms()) * 1000000; }

bool newer_than(uint64_t generation, const Hash256& hash, const Record& than) {
  return generation > than.generation || (generation == than.generation && hash > than.hash);
}

uint64_t ms_since(std::chrono::steady_clock::time_point since) {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count());
}

void raise_to(std::atomic_uint64_t& slot, uint64_t value) {
  auto seen = slot.load(std::memory_order_relaxed);
  while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

std::optional<std::string> policy_refusal(const Policy& policy) {
  if (policy.kind == Policy::Kind::every_node || policy.kind == Policy::Kind::node_local) return std::nullopt;
  return "a head space with replicas: " + std::to_string(policy.replicas) +
         " needs holders placed per head, which is L2's work and is not built; every_node opens";
}

Space::Space(SpaceSpec spec, ReplicaOptions replica, SpaceServices services)
    : _spec(std::move(spec)),
      _services(std::move(services)),
      _time(replica.time),
      _costs(replica.costs),
      _logger(replica.logger ? *replica.logger
                             : dannan::status::ScopedLogger(dannan::status::console_sink(dannan::status::LogFormat::text, dannan::status::LogLevel::info),
                                                            "dagda.heads")) {
  if (const auto why = policy_refusal(_spec.policy)) throw std::invalid_argument(*why);
  if (!_spec.codec) throw std::invalid_argument("a head space needs its owner's codec");
  if (!_services.read_nodes || !_services.commit_nodes) throw std::invalid_argument("a head space needs its node stores");
  _replica = std::make_unique<Replica>(std::move(replica));
  if (_services.seed) _services.seed->remember(_services.space, _replica->committed());
}

void Space::enter(const dannan::sched::WorkContext& work, dannan::sched::Waits declared, std::string_view operation) const {
  if (_services.guard) (void)_services.guard->enter(work, declared, operation);
}

// ---- Reads ------------------------------------------------------------------

std::optional<SpaceView> Space::current() const {
  // Nothing is read here: a state already decoded, under a lock nothing does I/O on.
  Lock lock(_view_mutex);
  if (!_view_state) return std::nullopt;
  return SpaceView{_view_generation, _view_revision, _view_hash, _view_state};
}

std::optional<SpaceView> Space::current(const dannan::sched::WorkContext& work) const {
  enter(work, current_waits, "heads::Space::current");
  return current();
}

SpaceView Space::local(const dannan::sched::WorkContext& work) {
  enter(work, local_waits, "heads::Space::local");
  return local();
}

SpaceView Space::converged(const dannan::sched::WorkContext& work) {
  enter(work, converged_waits, "heads::Space::converged");
  return converged();
}

SpaceView Space::local() {
  const auto revision = _replica->heads_revision();
  {
    Lock lock(_view_mutex);
    if (_local_view && _local_view_revision == revision) return *_local_view;
  }
  if (_replica->recovery_required()) return converged();
  const auto identities = _replica->usable_head_identities();
  if (identities.empty() || (identities.size() == 1 && identities.front().generation <= 1)) return converged();

  Record head;
  State decoded;
  {
    // The head set moves far more often than the head this node reads.
    Lock lock(_view_mutex);
    if (identities.size() == 1 && _local_view && _local_view->hash == identities.front().hash) {
      _local_view_revision = revision;
      return *_local_view;
    }
    if (identities.size() == 1 && _view_record && _view_state && _view_record->hash == identities.front().hash && _view_hash == _view_record->hash) {
      head = *_view_record;
      decoded = _view_state;
    }
  }
  if (!decoded) {
    auto heads = usable_heads();
    if (heads.empty()) return converged();
    head = heads.size() == 1 ? std::move(heads.front()) : own_head(heads);
    decoded = state_of(head);
  }
  // The view follows, so the witness moves with this node's own head.
  (void)cache_record(head, decoded);
  Lock lock(_view_mutex);
  SpaceView view{head.generation, _view_hash == head.hash ? _view_revision : 0, head.hash, decoded};
  _local_view = view;
  _local_view_revision = revision;
  return view;
}

SpaceView Space::converged() {
  if (auto cached = cached_view()) return *cached;
  const auto record = read_record();
  if (auto cached = cached_view()) return *cached;
  {
    // A commit in between can make what was just installed look stale. That is no
    // error: the coherent state just obtained serves, and the next read refreshes.
    Lock lock(_view_mutex);
    if (_view_state && (_view_generation > record.generation || (_view_generation == record.generation && _view_hash >= record.hash)))
      return SpaceView{_view_generation, _view_revision, _view_hash, _view_state};
  }
  return SpaceView{record.generation, 0, record.hash, state_of(record)};
}

Record Space::read_record() {
  if (auto cached = cached_record()) return *cached;
  try {
    const auto record = read_uncached();
    return cache_record(record, state_of(record));
  } catch (const MaintenanceStopping&) {
    throw;
  } catch (const std::exception&) {
    // Reads never wait for peers or for a merge: this node's own head serves.
    const auto heads = usable_heads();
    if (!_replica->recovery_required() && !heads.empty()) {
      const auto own = own_head(heads);
      if (own.generation > 1) return cache_record(own, state_of(own));
    }
    throw;
  }
}

Record Space::read_base() {
  if (_services.converge) return _services.converge();
  auto heads = usable_heads();
  if (heads.empty()) throw HeadsNotReady("no head here yet, and this node has no exchange to find one");
  return heads.size() == 1 ? std::move(heads.front()) : own_head(heads);
}

// R6: a read may commit a policy and clear recovery.
Record Space::read_uncached() {
  auto record = maybe_reconfigure(read_base());
  if (_replica->recovery_required()) _replica->mark_recovered();
  return record;
}

Record Space::maybe_reconfigure(const Record& initial) {
  if (!_spec.reconfigure) return initial;
  const auto reconfigured = _spec.reconfigure(state_of(initial));
  if (!reconfigured) return initial;
  const State& changed = *reconfigured;
  // R2: no dot, so its clock equals its parent's. It goes to the active set on the
  // control frame.
  Record proposed;
  proposed.generation = initial.generation + 1;
  proposed.previous = initial.hash;
  proposed.payload = _spec.codec->encode_full(changed);
  proposed.hash = record_hash(proposed.generation, proposed.previous, proposed.payload);
  if (_services.publish_policy) {
    _services.publish_policy(proposed);
  } else {
    HistoryEntry::Body body{};
    accept_locally(proposed, {}, &body);
  }
  _logger.info("metadata policy transition committed generation=" + std::to_string(proposed.generation));
  return cache_record(proposed, changed);
}

std::vector<Record> Space::usable_heads() const {
  if (_replica->set_aside_generation() && _services.membership_stamp && _set_aside_stamp.load(std::memory_order_acquire) != _services.membership_stamp())
    _replica->clear_set_aside();
  if (!_replica->set_aside_generation()) {
    _set_aside_count.store(0, std::memory_order_relaxed);
    return _replica->accepted_heads();
  }
  const auto all = _replica->accepted_heads().size();
  auto usable = _replica->usable_heads();
  _set_aside_count.store(all > usable.size() ? all - usable.size() : 0, std::memory_order_relaxed);
  return usable;
}

Record Space::own_head(const std::vector<Record>& heads) const {
  std::vector<std::pair<std::vector<uint64_t>, const Record*>> ranked;
  std::optional<std::vector<NodeId>> authors;
  for (const auto& head : heads) {
    try {
      const auto clock = _spec.codec->head_fields(state_of(head)).clock;
      if (!authors) authors = _replica->author_ids(_services.self, clock);
      std::vector<uint64_t> sequences;
      for (const auto& author : *authors) {
        const auto found = clock.find(author);
        sequences.push_back(found == clock.end() ? 0 : found->second);
      }
      ranked.emplace_back(std::move(sequences), &head);
    } catch (const MaintenanceStopping&) {
      throw;
    } catch (const std::exception&) {
    }
  }
  if (ranked.empty()) return heads.front();
  // R4: the lower hash on a tie, where the preferred head takes the higher.
  const auto best = std::max_element(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second->hash > b.second->hash;
  });
  return *best->second;
}

bool Space::set_aside(const Record& head, std::string_view reason) {
  if (_services.membership_stamp) _set_aside_stamp.store(_services.membership_stamp(), std::memory_order_release);
  if (!_replica->set_aside(head.hash, _services.wall->unix_ms())) return false;
  _logger.warn("metadata head set aside until membership changes generation=" + std::to_string(head.generation) + " reason=\"" + std::string(reason) + "\"");
  return true;
}

HeadsReport Space::heads() const {
  HeadsReport out;
  out.certificates = _replica->accepted_head_certificates();
  out.usable = _replica->usable_head_identities();
  out.preferred = _replica->committed_identity();
  out.set_aside_generation = _replica->set_aside_generation();
  out.recovery_required = _replica->recovery_required();
  return out;
}

State Space::state_of(const Record& record) const {
  if (const auto held = _replica->materialized(record.hash); held && held->record.generation == record.generation && held->record.payload == record.payload)
    return held->state;
  return _spec.codec->decode_full(record.payload);
}

Record Space::cache_record(const Record& record, State state) {
  if (_spec.on_head) _spec.on_head(state);
  Lock lock(_view_mutex);
  // Reads can complete out of order; never back to an older record.
  if (_view_record && newer_than(_view_record->generation, _view_record->hash, record)) return *_view_record;
  _view_record = record;
  view_taken_locked();
  if (!_view_state || _view_generation != record.generation || _view_hash != record.hash) {
    const bool moved = !_view_state || [&]() DANNAN_REQUIRES(_view_mutex) {
      const auto before = _spec.codec->head_fields(_view_state).root;
      const auto after = _spec.codec->head_fields(state).root;
      return before != after || !after;
    }();
    _view_state = std::move(state);
    _view_generation = record.generation;
    _view_hash = record.hash;
    if (moved) ++_view_revision;
    _available_generation.store(_view_generation, std::memory_order_release);
    _available_revision.store(_view_revision, std::memory_order_release);
  }
  return record;
}

void Space::view_taken_locked() {
  if (_services.view_ttl) _view_until = (_services.view_time ? _services.view_time : _time)->now() + *_services.view_ttl;
  if (_services.remote) _view_remote_epoch = _services.remote().epoch;
}

bool Space::view_current_locked(uint64_t generation) const {
  if (_services.view_ttl && (_services.view_time ? _services.view_time : _time)->now() >= _view_until) return false;
  if (_replica->committed_generation() > generation) return false;
  if (_services.remote) {
    const auto remote = _services.remote();
    if (remote.epoch != _view_remote_epoch || remote.generation > generation) return false;
  }
  return true;
}

std::optional<Record> Space::cached_record() const {
  Lock lock(_view_mutex);
  if (!_view_record || !view_current_locked(_view_record->generation)) return std::nullopt;
  return _view_record;
}

// The decoded state is valid for its record but no proof the record is current: this
// honours cached_record()'s lifetime, so a missed notice eventually forces a real read.
std::optional<SpaceView> Space::cached_view() const {
  Lock lock(_view_mutex);
  if (!_view_record || !_view_state || _view_record->generation != _view_generation || _view_record->hash != _view_hash) return std::nullopt;
  if (!view_current_locked(_view_generation)) return std::nullopt;
  return SpaceView{_view_generation, _view_revision, _view_hash, _view_state};
}

// ---- The commit -------------------------------------------------------------

CommitOutcome Space::mutate_delta(const MutationFactory& mutation, size_t retries, std::optional<ExternalIdentity> identity, const TreePause& pause) {
  return commit_path(mutation, retries, identity, pause, true);
}

CommitOutcome Space::mutate_delta(const dannan::sched::WorkContext& work, const MutationFactory& mutation, size_t retries,
                                  std::optional<ExternalIdentity> identity, const TreePause& pause) {
  enter(work, mutate_waits, "heads::Space::mutate_delta");
  return commit_path(mutation, retries, identity, pause, true);
}

CommitOutcome Space::mutate(const MutationFactory& mutation, size_t retries, std::optional<ExternalIdentity> identity, const TreePause& pause) {
  return commit_path(mutation, retries, identity, pause, false);
}

CommitOutcome Space::mutate(const dannan::sched::WorkContext& work, const MutationFactory& mutation, size_t retries, std::optional<ExternalIdentity> identity,
                            const TreePause& pause) {
  enter(work, mutate_waits, "heads::Space::mutate");
  return commit_path(mutation, retries, identity, pause, false);
}

CommitOutcome Space::commit_path(const MutationFactory& make, size_t retries, const std::optional<ExternalIdentity>& identity, const TreePause& pause,
                                 bool supplied) {
  Lock lock(_mutation_mutex);
  // This mutation's dot, kept across retries so an attempt that was accepted after
  // all is recognised rather than made twice.
  std::optional<Dot> dot;
  if (identity && !identity->sequence) throw std::invalid_argument("metadata mutation identity sequence must be non-zero");

  for (size_t attempt = 0; attempt < retries; ++attempt) {
    Record current;
    auto local_heads = usable_heads();
    const bool recovering = _replica->recovery_required();
    if (recovering || local_heads.empty() || (local_heads.size() == 1 && local_heads.front().generation <= 1)) {
      current = read_uncached();
      if (recovering) _replica->mark_recovered();
    } else {
      // A mutation asks no peer and waits for no merge. With several heads here it
      // extends the one carrying this node's own latest mutation; the merge follows
      // in the background.
      current = maybe_reconfigure(local_heads.size() == 1 ? local_heads.front() : own_head(local_heads));
    }

    const auto parent_state = state_of(current);
    const auto parent_fields = _spec.codec->head_fields(parent_state);
    const auto covered = [&] {
      owe(current, std::nullopt);
      (void)cache_record(current, parent_state);
      CommitOutcome out;
      out.kind = CommitOutcome::Kind::covered;
      out.record = current;
      out.dot = dot;
      out.attempts = attempt + 1;
      return out;
    };
    if (dot && clock_covers(parent_fields.clock, *dot)) return covered();
    if (identity) {
      // Already accepted (before a crash, or merged by a peer).
      const auto found = parent_fields.clock.find(identity->origin);
      if (found != parent_fields.clock.end() && found->second >= identity->sequence) return covered();
    }
    if (_services.scope_refusal) {
      if (const auto why = _services.scope_refusal()) throw std::runtime_error("this node's scope is refused: " + *why);
    }
    if (!dot) dot = _replica->reserve_mutation_dot(_services.self, parent_fields.clock);

    if (_spec.tree && !parent_fields.root) throw std::logic_error("metadata head carries no namespace tree");
    const auto reader = _services.read_nodes();
    const auto nodes = _services.commit_nodes();
    const MutationContext context{current, parent_state, parent_fields, *dot, identity, *reader, *nodes, wall_ns(*_services.wall), pause};
    const auto mutation = make(parent_state);
    mutation->edit(context);
    const auto edited = _spec.codec->head_fields(mutation->state());
    if (edited.policy != parent_fields.policy || edited.floor != parent_fields.floor) throw std::runtime_error("mutation attempted to change cluster policy");
    mutation->stamp(context);
    mutation->advance_clock(context);
    mutation->apply(context);

    const auto next = mutation->state();
    Bytes payload = _spec.codec->encode_full(next);
    if (_costs) _costs->record_bytes_encoded.fetch_add(payload.size(), std::memory_order_relaxed);
    // R1: the dot is already in the clock, and the clock in the payload, so this holds
    // only for an owner whose payload leaves its clock out. A mutation that changes
    // nothing is a new generation.
    if (current.payload == payload) {
      CommitOutcome out;
      out.kind = CommitOutcome::Kind::unchanged;
      out.record = cache_record(current, next);
      out.dot = dot;
      out.attempts = attempt + 1;
      return out;
    }

    Record proposed;
    proposed.generation = current.generation + 1;
    proposed.previous = current.hash;
    proposed.hash = record_hash(proposed.generation, proposed.previous, payload);
    if (_costs) _costs->record_bytes_hashed.fetch_add(payload.size(), std::memory_order_relaxed);
    proposed.payload = std::move(payload);

    Bytes encoded = supplied ? mutation->delta() : _spec.codec->encode_delta(parent_state, next).value_or(Bytes{});
    Bytes delta_payload;
    if (!encoded.empty() && encoded.size() < proposed.payload.size()) delta_payload = std::move(encoded);

    // This node's own claims, then the commit, both on this node alone. The caller is
    // answered from here; the peers' share is owed.
    const auto retention_started = std::chrono::steady_clock::now();
    auto claims = claim(*dot, parent_fields, _spec.codec->head_fields(next), mutation->introduced(context), *reader, pause);
    const auto retention_ms = ms_since(retention_started);

    try {
      CommitOutcome out;
      const auto publish_started = std::chrono::steady_clock::now();
      accept_locally(proposed, delta_payload, &out.body);
      _replica->note_author_accepted(*dot);
      const auto publish_ms = ms_since(publish_started);
      _mutations.fetch_add(1, std::memory_order_relaxed);
      _retention_ms_total.fetch_add(retention_ms, std::memory_order_relaxed);
      _publish_ms_total.fetch_add(publish_ms, std::memory_order_relaxed);
      raise_to(_retention_ms_max, retention_ms);
      raise_to(_publish_ms_max, publish_ms);
      (void)cache_record(proposed, next);
      owe(proposed, OwedClaims{*dot, claims});
      out.record = std::move(proposed);
      out.dot = dot;
      out.attempts = attempt + 1;
      out.claims = std::move(claims);
      return out;
    } catch (const HeadsNotReady& error) {
      // If the commit was accepted but what follows acceptance was interrupted, the
      // head set may already hold it: keep the dot, so the next attempt recognises it
      // rather than minting a second mutation.
      _logger.debug("metadata mutation publish failed sequence=" + std::to_string(dot->sequence) + " attempt=" + std::to_string(attempt + 1) + "/" +
                    std::to_string(retries) + " error=" + error.what());
      if (attempt + 1 == retries) throw;
      // Macha's pace between attempts, not a wait for anything in particular.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  throw HeadsNotReady("metadata mutation could not be accepted");
}

CommitClaims Space::claim(const Dot& dot, const HeadFields& before, const HeadFields& after, const Introduced& introduced, const TreeNodeStore& reader,
                          const TreePause& pause) {
  CommitClaims claims;
  claims.data = introduced.data;
  claims.control = introduced.control;
  // Tree nodes this commit introduced need claims, or the collector sees them as
  // unreferenced.
  if (_spec.tree && after.root && before.root != after.root) _spec.tree->collect_changes(before.root, *after.root, reader, claims.control, pause);
  std::sort(claims.data.begin(), claims.data.end());
  claims.data.erase(std::unique(claims.data.begin(), claims.data.end()), claims.data.end());
  std::sort(claims.control.begin(), claims.control.end());
  claims.control.erase(std::unique(claims.control.begin(), claims.control.end()), claims.control.end());
  if (!_services.claims) return claims;

  // Measured for its log line alone. A slow or refused barrier names its phase.
  const auto started = std::chrono::steady_clock::now();
  uint64_t data_ms = 0;
  uint64_t control_ms = 0;
  const auto report = [&](std::string_view outcome) {
    const auto total = ms_since(started);
    if (total < 250 && outcome == "ok") return;
    _logger.debug("metadata retention barrier dot=" + dannan::identity::to_string(dot.author).substr(0, 6) + ":" + std::to_string(dot.sequence) +
                  " total_ms=" + std::to_string(total) + " data_ms=" + std::to_string(data_ms) + " control_ms=" + std::to_string(control_ms) +
                  " data_objects=" + std::to_string(claims.data.size()) + " control_objects=" + std::to_string(claims.control.size()) +
                  " outcome=" + std::string(outcome));
  };

  // Only an object this commit brings in that is not held here is looked for on the
  // nodes present. Objects the parent already named are not: their holder may be away.
  const auto data_started = std::chrono::steady_clock::now();
  const auto elsewhere = _services.claims->retain_data_here(claims.data, dot);
  const std::set<ObjectId> carried(introduced.carried.begin(), introduced.carried.end());
  std::vector<ObjectId> sought;
  for (const auto& id : introduced.brought)
    if (!carried.contains(id) && std::binary_search(elsewhere.begin(), elsewhere.end(), id)) sought.push_back(id);
  if (!sought.empty() && !_services.claims->retain_data(sought, dot).empty()) {
    data_ms = ms_since(data_started);
    report("data-unheld");
    throw HeadsNotReady(std::string(data_unheld_refusal));
  }
  data_ms = ms_since(data_started);
  const auto control_started = std::chrono::steady_clock::now();
  const bool control_held = claims.control.empty() || _services.claims->retain_control_here(claims.control, dot);
  control_ms = ms_since(control_started);
  if (!control_held) {
    report("control-claim-failed");
    throw std::runtime_error(std::string(control_unclaimed_refusal));
  }
  _services.claims->claimed();
  report("ok");
  return claims;
}

void Space::accept_locally(const Record& record, std::span<const uint8_t> delta, HistoryEntry::Body* body) {
  if (!valid_record(record)) throw std::runtime_error("cannot publish invalid metadata commit");
  bool stored = false;
  if (delta.empty()) {
    stored = _replica->store_commit(record);
  } else {
    stored = _replica->store_commit(record, delta);
    if (!stored) {
      // The full record is in the proposal. A compact body can be rejected for a
      // missing parent or a delta that does not reconstruct; fall back once.
      _logger.warn("local metadata delta rejected; retrying full record generation=" + std::to_string(record.generation));
      stored = _replica->store_commit(record);
    }
  }
  if (!stored) throw std::runtime_error("local metadata commit store failed");
  if (const auto links = _replica->history_links(record.hash)) *body = links->body;

  // Acceptance is evidence about a completed store, not a second consensus (R5: what
  // `required` counts is how many held it). A mutation publishes to this node alone.
  Acceptance acceptance;
  acceptance.generation = record.generation;
  acceptance.hash = record.hash;
  acceptance.required = 1;
  acceptance.replicas = {_services.self};
  if (!accept(acceptance)) throw std::runtime_error("local metadata acceptance persistence failed");
}

bool Space::accept(const Acceptance& acceptance) {
  const auto before = _replica->committed_identity();
  bool heads_changed = false;
  if (!_replica->accept_commit(acceptance, &heads_changed)) return false;
  const auto after = _replica->committed();
  if (heads_changed && after.hash != before.hash && _services.seed) _services.seed->remember(_services.space, after);
  if (_services.accepted) _services.accepted(acceptance, heads_changed, after);
  notice_if_moved();
  return true;
}

uint64_t Space::watch(boost::asio::any_io_executor executor, std::function<void(HeadMoved)> notice) {
  if (!notice) throw std::invalid_argument("heads::Space::watch: no notice");
  Lock lock(_watch_mutex);
  // The first watch takes the head as it stands: what it is told of is a move from here.
  if (_watches.empty()) _told = _replica->committed_identity();
  const auto token = _next_watch++;
  _watches.emplace(token, Watch{std::move(executor), std::move(notice)});
  return token;
}

void Space::unwatch(uint64_t token) {
  Lock lock(_watch_mutex);
  _watches.erase(token);
  if (_watches.empty()) _told.reset();
}

void Space::notice_if_moved() {
  std::vector<Watch> told;
  HeadMoved move;
  {
    // The head is read under the watch lock, so two callers racing tell one chain of
    // moves, each once.
    Lock lock(_watch_mutex);
    if (_watches.empty()) return;
    const auto now = _replica->committed_identity();
    if (_told && *_told == now) return;
    move = {_told.value_or(HeadIdentity{}), now};
    _told = now;
    told.reserve(_watches.size());
    for (const auto& [_, watch] : _watches) told.push_back(watch);
  }
  // The executor rule: posted, never called here.
  for (auto& watch : told) boost::asio::post(watch.executor, [notice = std::move(watch.notice), move] { notice(move); });
}

Space::Upkeep Space::upkeep_node_local(const std::function<void(const std::set<ObjectId>&)>& collect) {
  if (!_spec.policy.is_node_local()) throw std::logic_error("heads::Space::upkeep_node_local on a space that is not node-local");
  Upkeep out;
  Lock mutation(_mutation_mutex);
  if (_replica->recovery_required()) return out;
  const auto before = _replica->diagnostics().history_records;
  // Nobody else holds this space's history, so nothing waits on its older entries:
  // re-rooted whenever one head stands.
  // A record threshold of two: a history already only its root is left as it is.
  (void)_replica->compact_history_if_safe(2, std::numeric_limits<uint64_t>::max());
  out.truncated = _replica->diagnostics().history_records < before;
  const auto heads = _replica->accepted_heads();
  if (heads.empty() || _replica->diagnostics().history_records > heads.size()) return out;
  std::set<ObjectId> reached;
  if (_spec.tree) {
    const auto reader = _services.read_nodes();
    for (const auto& head : heads) {
      const auto held = _replica->materialized(head.hash);
      if (!held) return out;
      if (const auto root = _spec.codec->head_fields(held->state).root) {
        std::vector<ObjectId> nodes;
        _spec.tree->collect_nodes(*root, *reader, nodes);
        reached.insert(nodes.begin(), nodes.end());
      }
    }
  }
  out.reached = reached.size();
  collect(reached);
  out.collected = true;
  return out;
}

std::optional<ObjectId> Space::root_of(const HeadIdentity& head) const {
  const auto held = _replica->materialized(head.hash);
  if (!held || held->record.generation != head.generation)
    throw HeadsNotReady("heads::Space::changed: this node does not hold the head at generation " + std::to_string(head.generation));
  return _spec.codec->head_fields(held->state).root;
}

void Space::owe(Record head, std::optional<OwedClaims> claims) {
  // A node-local space owes nobody anything (D3).
  if (_spec.policy.is_node_local()) return;
  {
    Lock lock(_owed_mutex);
    // Only the newest head is offered: its history carries the others.
    _owed_head = std::move(head);
    if (claims && (!claims->claims.data.empty() || !claims->claims.control.empty())) {
      _owed_claim_bytes += claims->claims.bytes();
      _owed_claims.push_back(std::move(*claims));
      while (_owed_claim_bytes > owed_claim_bytes_max && _owed_claims.size() > 1) {
        _owed_claim_bytes -= _owed_claims.front().claims.bytes();
        _owed_claims.pop_front();
        _owed_dropped.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  if (_services.owed_changed) _services.owed_changed();
}

OwedWork Space::take_owed() {
  Lock lock(_owed_mutex);
  OwedWork out;
  out.claims.swap(_owed_claims);
  _owed_claim_bytes = 0;
  out.head.swap(_owed_head);
  return out;
}

size_t Space::owed_commits() const {
  Lock lock(_owed_mutex);
  return _owed_claims.size() + (_owed_head ? 1 : 0);
}

OwedBacklog Space::owed_backlog() const {
  Lock lock(_owed_mutex);
  return {_owed_claims.size(), _owed_claim_bytes, _owed_dropped.load(std::memory_order_relaxed)};
}

MutationTiming Space::mutation_timing() const noexcept {
  return {_mutations.load(std::memory_order_relaxed), _retention_ms_total.load(std::memory_order_relaxed), _retention_ms_max.load(std::memory_order_relaxed),
          _publish_ms_total.load(std::memory_order_relaxed), _publish_ms_max.load(std::memory_order_relaxed)};
}

}  // namespace dagda::heads
