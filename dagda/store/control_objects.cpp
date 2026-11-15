//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/control_objects.h"

#include <string>

#include "dagda/store/messages.h"
#include "dannan/net/message.h"

namespace dagda::store {

ControlObjectFetch::ControlObjectFetch(dannan::Context& context, LocalStore& control, std::chrono::milliseconds per_peer, dannan::net::CallWatch watch)
    : _context(context), _control(control), _per_peer(per_peer), _watch(watch), _logger(context.logger("dagda.control")) {}

bool ControlObjectFetch::pull(const ObjectId& id, const Observer& observe) {
  if (_control.valid(id)) return true;
  const auto payload = message::encode_id(id);
  const auto self = _context.identity().id();
  size_t asked = 0;
  std::string last_failure = "no active peer";
  for (const auto& target : _context.nodes().active()) {
    if (target.id == self) continue;
    ++asked;
    const auto started = std::chrono::steady_clock::now();
    const auto outcome =
        _context.routes().call_and_wait(target, message::get_control_object, payload, dannan::sched::FrameType::speculative, _per_peer, _watch);
    if (!outcome.reply) {
      // Macha's transport threw here, and its catch said so (control_objects.cpp:56).
      _logger.debug("control object read " + target.host + ": " + outcome.failure);
      last_failure = target.host + ": " + outcome.failure;
      continue;
    }
    if (outcome.reply->type != message::control_object_reply) {
      // Macha names the reply's type: a refusal is its "error" (control_objects.cpp:37).
      const auto refusal = dannan::net::refusal_of(*outcome.reply);
      last_failure = target.host + ": " + (refusal ? std::string("error") : "reply type " + std::to_string(static_cast<uint16_t>(outcome.reply->type)));
      continue;
    }
    try {
      const auto object = message::decode_object(outcome.reply->payload);
      if (object.id != id || object_id(object.data) != id) {
        _logger.debug("control object read " + target.host + ": integrity failure");
        last_failure = target.host + ": integrity failure";
        continue;
      }
      if (observe) observe(object.data.size(), std::chrono::steady_clock::now() - started);
      if (_control.put(id, object.data)) return true;
      last_failure = "local control store put failed";
    } catch (const std::exception& error) {
      _logger.debug("control object read " + target.host + ": " + error.what());
      last_failure = target.host + ": " + error.what();
    }
  }
  _logger.warn("control object unavailable id=" + to_string(id) + " peers_asked=" + std::to_string(asked) + " last_failure=" + last_failure);
  return false;
}

}  // namespace dagda::store
