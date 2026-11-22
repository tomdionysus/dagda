//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/storage_server.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dagda/ledger/trie_diff.h"
#include "dagda/store/messages.h"
#include "dagda/store/placement.h"
#include "dannan/contract/scope.h"
#include "dannan/identity/node_id.h"
#include "dannan/membership/node_record.h"

namespace dagda::store {
namespace {

using dannan::net::Handled;
using dannan::net::Message;
using dannan::sched::FrameType;

Handled reply(dannan::net::MessageType type, Bytes payload) { return Handled::processed({type, std::move(payload), {}}); }

// A request that tried once, or a control slot not free.
Handled busy() { return Handled::failed(503, "storage_busy", "DATA resource admission busy or stopping"); }
// A request that waited with no deadline and still has no admission: the node is
// stopping, or its arbiter stalled for a whole no-progress window. Macha's words for both.
Handled gave_up() { return Handled::failed(503, "stopping", "DATA resource admission stopping"); }

}  // namespace

const std::vector<dannan::net::MessageType>& StorageServer::served() {
  static const std::vector<dannan::net::MessageType> types{
      message::have_object,        message::get_object,         message::put_object,           message::delete_object,
      message::get_control_object, message::put_control_object, message::put_object_deferred,  message::object_durability_barrier,
      message::retain_objects,     message::have_objects,       message::have_control_objects, message::have_valid_objects,
      message::trie_diff,          message::durability_covered, message::remove_objects,       message::offer_objects,
      message::get_objects,
  };
  return types;
}

StorageServer::StorageServer(dannan::Context& context, Stores stores, StorageServerOptions options, StorageServerAdmission admission)
    : _context(context),
      _data(stores.data),
      _control(stores.control),
      _claims(stores.claims),
      _cache(stores.cache),
      _ledger(stores.ledger),
      _offers(stores.offers),
      _options(options),
      _admission(admission),
      _logger(context.logger("dagda.server")),
      _storage_event(context.events().kind("storage")),
      _activity(stores.activity ? *stores.activity : context.activity()),
      _hosts_extents(options.hosts_extents) {
  refresh();
  for (const auto type : served())
    _context.routes().bind(
        type, [this](const dannan::membership::NodeRecord& peer, FrameType frame, const Message& request) { return handle(peer, frame, request); });
  _refresher = std::jthread([this](std::stop_token stop) { _context.threads().loop("dagda.storage-refresh", stop, [this, stop] { refresh_loop(stop); }); });
}

// Routes first, so no request reaches a store while the refresher stops.
StorageServer::~StorageServer() {
  for (const auto type : served()) _context.routes().unbind(type);
  _refresher.request_stop();
  _refresh_wake.notify_all();
  if (_refresher.joinable()) _refresher.join();
}

void StorageServer::refresh() {
  const auto started = std::chrono::steady_clock::now();
  _data.refresh();
  const auto refresh_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  if (refresh_ms >= 100) _logger.trace("DIAG node-stage stage=storage-refresh elapsed_ms=" + std::to_string(refresh_ms));
  advertise();
  // A disk come back changes what this node holds, with nothing written.
  const auto online = _data.online_backends();
  if (online > _online_backends.exchange(online, std::memory_order_relaxed)) _context.events().notify(_storage_event);
}

void StorageServer::set_hosts_extents(bool hosts) {
  _hosts_extents.store(hosts, std::memory_order_relaxed);
  advertise();
}

void StorageServer::advertise() {
  auto& nodes = _context.nodes();
  nodes.advertise(capacity_advert, dannan::membership::u64_advert(_data.limit()));
  StoreFigures figures;
  figures.used = _data.used();
  figures.backends_online = _data.online_backends();
  if (_cache) {
    const auto extent = static_cast<uint64_t>(_options.extent_size);
    const auto stats = _cache->stats();
    figures.has_cache = true;
    figures.cache_capacity = static_cast<uint64_t>(_cache->max_blocks()) * extent;
    figures.cache_used = static_cast<uint64_t>(_cache->blocks()) * extent;
    figures.cache_hits = stats.hits;
    figures.cache_misses = stats.misses;
    figures.cache_evictions = stats.evictions;
  }
  nodes.advertise(figures_advert, encode_figures(figures));
  if (_hosts_extents.load(std::memory_order_relaxed))
    nodes.advertise(extents_advert, Bytes{1});
  else
    nodes.withdraw(extents_advert);
}

void StorageServer::refresh_loop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(_refresh_mutex);
      _refresh_wake.wait_for(lock, stop, _options.refresh_interval, [] { return false; });
    }
    if (stop.stop_requested()) return;
    refresh();
  }
}

std::optional<DataAdmission> StorageServer::admit_data(FrameType frame, uint64_t bytes, bool wait) {
  return store::admit_data(_admission.credit, _admission.io, frame, bytes, wait ? no_caller_deadline : std::optional(std::chrono::milliseconds::zero()));
}

std::optional<IoReserve::Slot> StorageServer::admit_control() { return store::admit_control(_admission.io, _options.admission_wait); }

std::optional<Handled> StorageServer::scope_refusal(const dannan::membership::NodeRecord& peer, const std::optional<message::WritePrincipal>& writer) const {
  const dannan::contract::Principal sender{dannan::contract::Principal::Kind::node, dannan::identity::to_string(peer.id)};
  const auto& principal = writer ? writer->principal : sender;
  const auto& prefix = writer && !writer->prefix.empty() ? writer->prefix : _options.domain.name;
  const auto decision = _context.identity().verify(principal, prefix);
  if (decision.allowed) return std::nullopt;
  return Handled::failed(403, "scope_refused", decision.why + " (scope generation " + std::to_string(decision.generation) + ")");
}

Handled StorageServer::answer_trie(const Message& request) {
  const auto question = ledger::decode_trie_question(request.payload);
  const auto now = _context.time().source().now();
  if (question.kind == ledger::TrieQuestionKind::root) {
    auto view = _data.held_view();
    if (!view.identity.complete || !view.trie) return Handled::failed(503, "holdings_seeding", "this node is still listing what it holds");
    const auto summary = view.trie->root();
    {
      dannan::contract::Lock lock(_pins_mutex);
      std::erase_if(_pins, [&](const Pinned& pinned) { return pinned.hash == summary.hash || now - pinned.used > pinned_for; });
      if (_pins.size() >= pinned_max) _pins.erase(_pins.begin());
      _pins.push_back({summary.hash, std::move(view.trie), now});
    }
    return reply(message::trie_diff_reply, ledger::encode_trie_root(summary));
  }
  std::shared_ptr<const ledger::TrieUnion> trie;
  {
    dannan::contract::Lock lock(_pins_mutex);
    std::erase_if(_pins, [&](const Pinned& pinned) { return now - pinned.used > pinned_for; });
    for (auto& pinned : _pins)
      if (pinned.hash == question.at) {
        pinned.used = now;
        trie = pinned.trie;
      }
  }
  if (!trie) return Handled::failed(410, "snapshot_gone", "the trie snapshot asked about is no longer pinned");
  if (question.kind == ledger::TrieQuestionKind::children)
    return reply(message::trie_diff_reply, ledger::encode_trie_children(trie->children(question.prefixes)));
  // Records only where a diff reads them: below a leaf's worth.
  try {
    return reply(message::trie_diff_reply, ledger::encode_trie_records(trie->records(question.prefixes, ledger::ObjectTrie::leaf_max)));
  } catch (const std::length_error&) {
    return Handled::failed(413, "prefix_too_large", "a prefix asked for holds more than a leaf");
  }
}

Handled StorageServer::handle(const dannan::membership::NodeRecord& peer, FrameType frame, const Message& request) {
  const auto type = request.type;
  const bool control_frame = dannan::sched::work_class(frame) == dannan::sched::WorkClass::control;
  const uint64_t extent = std::max<uint64_t>(1, _options.extent_size);
  try {
    if (type == message::have_object) {
      const auto id = message::decode_id(request.payload);
      // Control work never enters the DATA arbiter, and this reads DATA.
      if (control_frame) return Handled::failed(400, "invalid_request", "a full read of DATA is not control work");
      const auto admitted = admit_data(frame, extent, false);
      if (!admitted) return busy();
      // Repair trusts this "present" without verifying again, so the copy is
      // read, opened and hashed: a corrupt one never counts as a holder.
      // (have_objects and retain_objects check presence only.)
      return reply(message::bool_reply, message::encode_bool(_data.valid(id)));
    }
    if (type == message::have_objects || type == message::have_control_objects) {
      const auto ids = message::decode_ids(request.payload);
      std::vector<bool> present;
      present.reserve(ids.size());
      if (type == message::have_control_objects) {
        // The CONTROL counterpart answers from the control store and never
        // touches DATA (law 1).
        const auto slot = admit_control();
        if (!slot) return busy();
        for (const auto& id : ids) present.push_back(_control.has(id));
        return reply(message::have_control_objects_reply, message::encode_presence(present));
      }
      // One admission for the batch: index lookups, no object read.
      std::optional<DataAdmission> admitted;
      if (!control_frame) {
        admitted = admit_data(frame, extent, false);
        if (!admitted) return busy();
      }
      for (const auto& id : ids) present.push_back(_data.has(id));
      return reply(message::have_objects_reply, message::encode_presence(present));
    }
    if (type == message::offer_objects) {
      // Repair's offer: what is held from the index, then whether a copy could be
      // stored, then the book's say. One admission for the batch, as a presence.
      const auto ids = message::decode_ids(request.payload, message::max_offer_batch);
      std::optional<DataAdmission> admitted;
      if (!control_frame) {
        admitted = admit_data(frame, extent, false);
        if (!admitted) return busy();
      }
      const auto limit = _data.limit();
      const bool room = _data.online_backends() > 0 && (!limit || _data.used() + extent <= limit);
      std::vector<message::OfferAnswer> answers;
      answers.reserve(ids.size());
      for (const auto& id : ids) {
        if (_data.has(id))
          answers.push_back(message::OfferAnswer::held);
        else if (!room)
          answers.push_back(message::OfferAnswer::no_room);
        else if (_offers && !_offers->want(peer.id, id))
          answers.push_back(message::OfferAnswer::fetching);
        else
          answers.push_back(message::OfferAnswer::wanted);
      }
      return reply(message::offer_objects_reply, message::encode_offer_answers(answers));
    }
    if (type == message::have_valid_objects) {
      const auto ids = message::decode_ids(request.payload, message::max_valid_batch);
      // Per id: absence is an index lookup, final with every disk online; a
      // held copy is read, opened and hashed under one DATA admission, and
      // while admission is busy it is "cannot say now".
      std::vector<message::Validity> answers;
      answers.reserve(ids.size());
      bool refused_admission = false;
      for (const auto& id : ids) {
        auto held = message::Validity::cannot_say;
        const bool every_backend = _data.every_backend_online();
        if (!_data.has(id)) {
          if (every_backend && _data.every_backend_online()) held = message::Validity::absent;
        } else if (!refused_admission) {
          if (const auto admitted = admit_data(frame, extent, false))
            held = _data.valid(id) ? message::Validity::intact : _data.every_backend_online() ? message::Validity::absent : message::Validity::cannot_say;
          else
            refused_admission = true;
        }
        answers.push_back(held);
      }
      return reply(message::have_valid_objects_reply, message::encode_validity(answers));
    }
    if (type == message::get_object) {
      const auto id = message::decode_id(request.payload);
      const auto timing = [&](std::string_view stage, uint64_t bytes) {
        if (_options.serve_timing) _options.serve_timing("get_object " + std::string(stage) + " peer=" + peer.host + " bytes=" + std::to_string(bytes));
      };
      timing("arrived", 0);
      auto admitted = admit_data(frame, extent, true);
      if (!admitted) return gave_up();
      timing("admitted", 0);
      // Not found is final only when no offline backend could hold it.
      const bool every_backend = _data.every_backend_online();
      auto data = _data.get(id);
      admitted.reset();
      timing("read", data ? data->size() : 0);
      if (!data)
        return every_backend && _data.every_backend_online() ? Handled::failed(404, "not_found", "object not found")
                                                             : Handled::failed(503, "storage_unavailable", "object not found on the backends online");
      _activity.note(frame, data->size());
      return reply(message::object_reply, message::encode_object(id, *data));
    }
    if (type == message::get_objects) {
      // Repair's fetch of many: one admission of an extent, then the objects in order
      // while the reply stays within it, each sized from the index before it is read.
      const auto ids = message::decode_ids(request.payload, message::max_fetch_batch);
      const auto timing = [&](std::string_view stage, uint64_t bytes) {
        if (_options.serve_timing)
          _options.serve_timing("get_objects " + std::string(stage) + " peer=" + peer.host + " ids=" + std::to_string(ids.size()) +
                                " bytes=" + std::to_string(bytes));
      };
      timing("arrived", 0);
      auto admitted = admit_data(frame, extent, true);
      if (!admitted) return gave_up();
      timing("admitted", 0);
      std::vector<message::FetchedObject> objects(ids.size());
      uint64_t sent = 0;
      bool full = false;
      for (size_t i = 0; i < ids.size(); ++i) {
        auto& object = objects[i];
        if (full) continue;
        // Not found is final only when no offline backend could hold it.
        const bool every_backend = _data.every_backend_online();
        if (!_data.has(ids[i])) {
          object.as = every_backend && _data.every_backend_online() ? message::Fetched::absent : message::Fetched::unavailable;
          continue;
        }
        if (sent && sent + _data.payload_size(ids[i]) > extent) {
          full = true;
          continue;
        }
        auto data = _data.get(ids[i]);
        if (!data) {
          object.as = message::Fetched::unavailable;
          continue;
        }
        sent += data->size();
        object.as = message::Fetched::sent;
        object.data = std::move(*data);
      }
      admitted.reset();
      _activity.note(frame, sent);
      timing("read", sent);
      return reply(message::get_objects_reply, message::encode_fetched(objects));
    }
    if (type == message::get_control_object) {
      const auto id = message::decode_id(request.payload);
      std::optional<Bytes> data;
      {
        const auto slot = admit_control();
        if (!slot) return busy();
        data = _control.get(id);
      }
      if (!data) return Handled::failed(404, "not_found", "control object not found");
      return reply(message::control_object_reply, message::encode_object(id, *data));
    }
    if (type == message::put_object || type == message::put_object_deferred) {
      // Stored from the request's own bytes: no second buffer of an extent.
      const auto object = message::decode_object_view(request.payload);
      if (object.data.size() > _options.extent_size) return Handled::failed(413, "too_large", "DATA object exceeds configured extent size");
      // Checked before the device is asked for anything: a refused writer
      // costs no admission.
      if (auto refused = scope_refusal(peer, object.principal)) return std::move(*refused);
      const auto admitted = admit_data(frame, object.data.size(), true);
      if (!admitted) return gave_up();
      _activity.note(frame, object.data.size());
      if (!_data.has(object.id)) _context.events().notify(_storage_event);
      if (type == message::put_object_deferred) {
        // Bound to this process and the backend incarnation that took it: a
        // later barrier for a covered generation waits on nothing.
        const auto ticket = _data.put_deferred(object.id, object.data);
        if (!ticket) return Handled::failed(507, "storage_full", "storage limit reached");
        if (_offers) _offers->arrived(object.id);
        advertise();
        return reply(dannan::net::message::ok, message::encode_ticket(*ticket));
      }
      if (!_data.put(object.id, object.data)) return Handled::failed(507, "storage_full", "storage limit reached");
      if (_offers) _offers->arrived(object.id);
      advertise();
      return Handled::processed();
    }
    if (type == message::put_control_object) {
      const auto object = message::decode_object(request.payload);
      if (auto refused = scope_refusal(peer, object.principal)) return std::move(*refused);
      const auto slot = admit_control();
      if (!slot) return busy();
      if (!_control.put(object.id, object.data)) return Handled::failed(507, "storage_full", "control storage limit reached");
      return Handled::processed();
    }
    if (type == message::delete_object) {
      const auto id = message::decode_id(request.payload);
      {
        // The claim check first, as Macha's handler (storage_server.cpp:393-410 at
        // f0597a4): a claimed object is claimed whatever the node's load. It marks the
        // object for the delete; admission is tried once under the mark, never waited for.
        const auto deletion = _data.claim_locks().deletion(id, [&] { return _claims.retained(contract::RetentionClass::data, id); });
        if (deletion.claimed()) return Handled::failed(409, "claimed", "object has an active retention claim");
        const auto admitted = admit_data(frame, extent, false);
        if (!admitted) return busy();
        (void)_data.remove(id);
      }
      if (_cache) (void)_cache->remove(id);
      advertise();
      return Handled::processed();
    }
    if (type == message::object_durability_barrier) {
      const auto barrier = message::decode_barrier(request.payload);
      const auto epoch = _context.identity().durability_epoch();
      if (barrier.ticket.epoch != epoch) {
        // The asker's ticket is from a previous process and can never come
        // true, but the objects may be on disk. With ids, answer from disk
        // with fresh tickets (discipline 1: re-derive, do not assert);
        // without, refuse.
        const auto expected = dannan::identity::to_string(barrier.ticket.epoch).substr(0, 8);
        const auto current = dannan::identity::to_string(epoch).substr(0, 8);
        if (barrier.probe.empty()) {
          _logger.debug("object durability barrier refused: epoch changed expected=" + expected + " current=" + current +
                        " domain=" + std::to_string(barrier.ticket.domain) + " generation=" + std::to_string(barrier.ticket.generation));
          return Handled::failed(409, "epoch_changed", "storage durability epoch changed");
        }
        message::BarrierProbeAnswer answer{epoch, {}};
        for (const auto& id : barrier.probe)
          if (auto ticket = _data.reassert_durable(id, barrier.ticket)) answer.present.emplace_back(id, *ticket);
        _logger.info("object durability re-derived after epoch change present=" + std::to_string(answer.present.size()) + "/" +
                     std::to_string(barrier.probe.size()) + " expected=" + expected + " current=" + current);
        advertise();
        return reply(dannan::net::message::ok, message::encode_probe_answer(answer));
      }
      try {
        _data.durability_barrier(barrier.ticket, DurabilityUrgency::batchable);
      } catch (const std::exception& error) {
        return Handled::failed(500, "barrier_failed", std::string("storage durability barrier failed: ") + error.what());
      }
      advertise();
      return Handled::processed();
    }
    if (type == message::durability_covered) {
      const auto ticket = message::decode_ticket(request.payload);
      if (ticket.epoch != _context.identity().durability_epoch()) return Handled::failed(409, "epoch_changed", "storage durability epoch changed");
      return reply(message::bool_reply, message::encode_bool(_data.durability_covered(ticket)));
    }
    if (type == message::retain_objects) {
      const auto retain = message::decode_retain(request.payload);
      if (auto refused = scope_refusal(peer, retain.principal)) return std::move(*refused);
      // A claim says "this node holds the object": index presence, not a
      // read. The bytes were verified when put and on every read; scrub
      // finds later corruption. The claim's journal is control work. Under
      // the objects' claim guard, either class, a sub-batch at a time, so no
      // delete removes a copy between this check and the claim and a large
      // publication does not stop every delete behind it. One a delete has
      // marked is not present.
      const auto slot = admit_control();
      if (!slot) return busy();
      const auto claim = [&](std::span<const ObjectId> part) {
        for (const auto& id : part)
          if (_data.claim_locks().deleting(id) || !(retain.object_class == contract::RetentionClass::data ? _data.has(id) : _control.has(id))) return false;
        _claims.retain_batch(retain.object_class, std::vector<ObjectId>(part.begin(), part.end()), retain.dot);
        return true;
      };
      if (!_data.claim_locks().in_batches(retain.ids, claim)) return Handled::failed(409, "not_present", "retention object is not durably present");
      return Handled::processed();
    }
    if (type == message::remove_objects) {
      auto remove = message::decode_remove(request.payload);
      ReplicaRemoval answer;
      const auto& prefix = remove.writer.prefix.empty() ? _options.domain.name : remove.writer.prefix;
      const auto decision = _context.identity().verify(remove.writer.principal, prefix);
      if (!decision.allowed) {
        answer.refused = true;
        answer.scope_generation = decision.generation;
        answer.why = decision.why;
        return reply(message::remove_objects_reply, message::encode_removal(answer));
      }
      // The claims' journal and the unlinks: DATA device work.
      const auto admitted = admit_data(frame, extent, true);
      if (!admitted) return gave_up();
      answer.objects = remove_here({_data, _claims, _ledger, _cache}, std::move(remove.ids), remove.observed);
      advertise();
      return reply(message::remove_objects_reply, message::encode_removal(answer));
    }
    if (type == message::trie_diff) return answer_trie(request);
  } catch (const dannan::net::DecodeError& error) {
    return Handled::failed(400, "invalid_request", error.what());
  }
  return Handled::unrecognised();
}

}  // namespace dagda::store
