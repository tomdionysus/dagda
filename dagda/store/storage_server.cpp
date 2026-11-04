//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/storage_server.h"

#include <string>
#include <utility>
#include <vector>

#include "dagda/store/messages.h"
#include "dagda/store/placement.h"
#include "dannan/membership/node_record.h"

namespace dagda::store {
namespace {

using dannan::net::Handled;
using dannan::net::Message;

constexpr dannan::net::MessageType served[] = {
    message::have_object,
    message::get_object,
    message::put_object,
    message::get_control_object,
    message::put_control_object,
    message::put_object_deferred,
    message::object_durability_barrier,
    message::retain_objects,
    message::have_objects,
    message::have_control_objects,
    message::durability_covered,
};

Handled reply(dannan::net::MessageType type, Bytes payload) { return Handled::processed({type, std::move(payload), {}}); }

}  // namespace

StorageServer::StorageServer(dannan::Context& context, Stores stores, StorageServerOptions options)
    : _context(context),
      _data(stores.data),
      _control(stores.control),
      _claims(stores.claims),
      _options(options),
      _logger(context.logger("dagda.server")),
      _storage_event(context.events().kind("storage")) {
  refresh();
  for (const auto type : served)
    _context.routes().bind(type, [this](const dannan::membership::NodeRecord& peer, dannan::sched::FrameType frame, const Message& request) {
      return handle(peer, frame, request);
    });
  _refresher = std::jthread([this](std::stop_token stop) { _context.threads().loop("dagda.storage-refresh", stop, [this, stop] { refresh_loop(stop); }); });
}

// Routes first, so no request reaches a store while the refresher stops.
StorageServer::~StorageServer() {
  for (const auto type : served) _context.routes().unbind(type);
  _refresher.request_stop();
  _refresh_wake.notify_all();
  if (_refresher.joinable()) _refresher.join();
}

void StorageServer::refresh() {
  _data.refresh();
  advertise();
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

Handled StorageServer::handle(const dannan::membership::NodeRecord&, dannan::sched::FrameType frame, const Message& request) {
  const auto type = request.type;
  try {
    if (type == message::have_object) {
      // Repair trusts this "present" without verifying again, so the copy is
      // read, opened and hashed: a corrupt one never counts as a holder.
      // (have_objects and retain_objects check presence only.)
      return reply(message::bool_reply, message::encode_bool(_data.valid(message::decode_id(request.payload))));
    }
    if (type == message::have_objects || type == message::have_control_objects) {
      // One answer from the index for the batch: no object is read. The
      // CONTROL form answers from the control store and never touches DATA
      // (law 1).
      const auto ids = message::decode_ids(request.payload);
      std::vector<bool> present;
      present.reserve(ids.size());
      for (const auto& id : ids) present.push_back(type == message::have_objects ? _data.has(id) : _control.has(id));
      return reply(type == message::have_objects ? message::have_objects_reply : message::have_control_objects_reply, message::encode_presence(present));
    }
    if (type == message::get_object) {
      const auto id = message::decode_id(request.payload);
      // Not found is final only when no offline backend could hold it.
      const bool every_backend = _data.every_backend_online();
      auto data = _data.get(id);
      if (!data)
        return every_backend && _data.every_backend_online() ? Handled::failed(404, "not_found", "object not found")
                                                             : Handled::failed(503, "storage_unavailable", "object not found on the backends online");
      _context.activity().note(frame, data->size());
      return reply(message::object_reply, message::encode_object(id, *data));
    }
    if (type == message::get_control_object) {
      const auto id = message::decode_id(request.payload);
      auto data = _control.get(id);
      if (!data) return Handled::failed(404, "not_found", "control object not found");
      return reply(message::control_object_reply, message::encode_object(id, *data));
    }
    if (type == message::put_object || type == message::put_object_deferred) {
      auto object = message::decode_object(request.payload);
      if (object.data.size() > _options.extent_size) return Handled::failed(413, "too_large", "DATA object exceeds configured extent size");
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
      if (!_control.put(object.id, object.data)) return Handled::failed(507, "storage_full", "control storage limit reached");
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
      // A claim says "this node holds the object": index presence, not a
      // read. The bytes were verified when put and on every read; scrub
      // finds later corruption.
      for (const auto& id : retain.ids) {
        const bool present = retain.object_class == contract::RetentionClass::data ? _data.has(id) : _control.has(id);
        if (!present) return Handled::failed(409, "not_present", "retention object is not durably present");
      }
      _claims.retain_batch(retain.object_class, retain.ids, retain.dot);
      return Handled::processed();
    }
  } catch (const dannan::net::DecodeError& error) {
    return Handled::failed(400, "invalid_request", error.what());
  }
  return Handled::unrecognised();
}

}  // namespace dagda::store
