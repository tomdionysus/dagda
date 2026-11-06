//
// Dagda - The Store
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

Handled busy() { return Handled::failed(503, "storage_busy", "DATA resource admission busy or stopping"); }

}  // namespace

const std::vector<dannan::net::MessageType>& StorageServer::served() {
  static const std::vector<dannan::net::MessageType> types{
      message::have_object,        message::get_object,         message::put_object,           message::delete_object,
      message::get_control_object, message::put_control_object, message::put_object_deferred,  message::object_durability_barrier,
      message::retain_objects,     message::have_objects,       message::have_control_objects, message::have_valid_objects,
      message::trie_diff,          message::durability_covered, message::remove_objects,
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
      _options(options),
      _admission(admission),
      _logger(context.logger("dagda.server")),
      _storage_event(context.events().kind("storage")) {
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
  _data.refresh();
  advertise();
  // A disk come back changes what this node holds, with nothing written.
  const auto online = _data.online_backends();
  if (online > _online_backends.exchange(online, std::memory_order_relaxed)) _context.events().notify(_storage_event);
}

void StorageServer::advertise() {
  auto& nodes = _context.nodes();
  nodes.advertise(capacity_advert, dannan::membership::u64_advert(_data.limit()));
  nodes.advertise(used_advert, dannan::membership::u64_advert(_data.used()));
  if (_options.hosts_extents)
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
  return store::admit_data(_admission.credit, _admission.io, frame, bytes, wait ? _options.admission_wait : std::chrono::milliseconds::zero());
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
  const auto now = std::chrono::steady_clock::now();
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
      auto admitted = admit_data(frame, extent, true);
      if (!admitted) return busy();
      // Not found is final only when no offline backend could hold it.
      const bool every_backend = _data.every_backend_online();
      auto data = _data.get(id);
      admitted.reset();
      if (!data)
        return every_backend && _data.every_backend_online() ? Handled::failed(404, "not_found", "object not found")
                                                             : Handled::failed(503, "storage_unavailable", "object not found on the backends online");
      _context.activity().note(frame, data->size());
      return reply(message::object_reply, message::encode_object(id, *data));
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
      auto object = message::decode_object(request.payload);
      if (object.data.size() > _options.extent_size) return Handled::failed(413, "too_large", "DATA object exceeds configured extent size");
      // Checked before the device is asked for anything: a refused writer
      // costs no admission.
      if (auto refused = scope_refusal(peer, object.principal)) return std::move(*refused);
      const auto admitted = admit_data(frame, object.data.size(), true);
      if (!admitted) return busy();
      _context.activity().note(frame, object.data.size());
      if (!_data.has(object.id)) _context.events().notify(_storage_event);
      if (type == message::put_object_deferred) {
        // Bound to this process and the backend incarnation that took it: a
        // later barrier for a covered generation waits on nothing.
        const auto ticket = _data.put_deferred(object.id, object.data);
        if (!ticket) return Handled::failed(507, "storage_full", "storage limit reached");
        advertise();
        return reply(dannan::net::message::ok, message::encode_ticket(*ticket));
      }
      if (!_data.put(object.id, object.data)) return Handled::failed(507, "storage_full", "storage limit reached");
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
      const auto admitted = admit_data(frame, extent, false);
      if (!admitted) return busy();
      {
        // The claim check marks the object for the delete.
        const auto deletion = _data.claim_locks().deletion(id, [&] { return _claims.retained(contract::RetentionClass::data, id); });
        if (deletion.claimed()) return Handled::failed(409, "claimed", "object has an active retention claim");
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
        if (barrier.probe.empty()) return Handled::failed(409, "epoch_changed", "storage durability epoch changed");
        message::BarrierProbeAnswer answer{epoch, {}};
        for (const auto& id : barrier.probe)
          if (auto ticket = _data.reassert_durable(id)) answer.present.emplace_back(id, *ticket);
        _logger.info("object durability re-derived after epoch change present=" + std::to_string(answer.present.size()) + "/" +
                     std::to_string(barrier.probe.size()));
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
      // the objects' claim guard, a sub-batch at a time, so no instant delete
      // removes a copy between this check and the claim and a large
      // publication does not stop every delete behind it. One a delete has
      // marked is not present.
      const auto slot = admit_control();
      if (!slot) return busy();
      const auto claim = [&](std::span<const ObjectId> part) {
        for (const auto& id : part)
          if (!(retain.object_class == contract::RetentionClass::data ? !_data.claim_locks().deleting(id) && _data.has(id) : _control.has(id))) return false;
        _claims.retain_batch(retain.object_class, std::vector<ObjectId>(part.begin(), part.end()), retain.dot);
        return true;
      };
      const bool claimed = retain.object_class == contract::RetentionClass::data ? _data.claim_locks().in_batches(retain.ids, claim) : claim(retain.ids);
      if (!claimed) return Handled::failed(409, "not_present", "retention object is not durably present");
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
      if (!admitted) return busy();
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
