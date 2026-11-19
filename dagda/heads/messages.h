//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "dagda/heads/record.h"
#include "dagda/heads/sealed_file.h"
#include "dannan/context.h"
#include "dannan/net/routes.h"

// The heads' messages on Dannan (the plan's 2.3). The payloads are the forms record.h
// writes, byte for byte what a peer of the space's owner exchanged, so nothing about a
// record or a certificate is new on the wire. Every type is on the CONTROL lane in any
// class, as the callers choose control, read-ahead and speculative; the three that
// change a replica run in arrival order per peer, on Dannan's ordered pool.
//
// A type's number is the owner's to name (`Numbers`): a node with an owner whose own
// numbers are already in use keeps them, so a mixed roll works. `register_types` adds
// exactly what it is given, and a node with NodeOptions::heads off adds nothing, so a
// registration its owner keeps through an earlier stage of a port stands alone.
namespace dagda::heads::message {

using dannan::net::MessageType;

// Dagda's own, from `first`, in the order below. An owner replaces any of them. The
// reply to the two puts, the accept and has_history_entry is the store's bool_reply,
// which they share.
struct Numbers {
  static constexpr uint16_t first = 784;

  // Change a replica, in arrival order per peer.
  MessageType put_history_entry{first + 0};
  MessageType put_commit{first + 1};
  MessageType accept_commit{first + 2};
  // Read one.
  MessageType get_history_entry{first + 3};
  MessageType has_history_entry{first + 4};
  MessageType get_heads{first + 5};
  MessageType get_history_record{first + 6};
  MessageType get_committed{first + 7};
  MessageType get_current{first + 8};
  MessageType get_identity{first + 9};
  // One way, to every peer.
  MessageType notice{first + 10};
  // Replies.
  MessageType record_reply{first + 11};
  MessageType identity_reply{first + 12};
  MessageType history_entry_reply{first + 13};
  MessageType heads_reply{first + 14};
};

// Registers each type's wire rules (not their handlers) on `routes`. Before the node
// starts: Dannan refuses registration after. Throws as Routes::add does, so a number
// already taken refuses the node.
void register_types(dannan::Routes& routes, const Numbers& numbers);

// get_history_entry, get_history_record, has_history_entry: the hash.
Bytes encode_hash(const Hash256& hash);
Hash256 decode_hash(std::span<const uint8_t> payload);

// notice: the generation.
Bytes encode_notice(uint64_t generation);
uint64_t decode_notice(std::span<const uint8_t> payload);

// identity_reply: the generation, then the hash.
Bytes encode_identity(const HeadIdentity& identity);
HeadIdentity decode_identity(std::span<const uint8_t> payload);

// A space other than the one an owner's peers expect adds its id after the form, so
// the first space's payloads carry no byte more than the owner's decoders allow; its
// decoders call finish(), and a space id never goes to them.
Bytes with_space(Bytes payload, const std::optional<SpaceId>& space);
// A form's bytes and the id after them. What `decode` refuses whole is taken to end in
// a space id, and refused in turn if the rest is not the form. `Decode` returns the
// form and throws DecodeError for anything else, trailing bytes included.
template <class Decode>
auto addressed(std::span<const uint8_t> payload, const Decode& decode) {
  using Form = decltype(decode(payload));
  struct Addressed {
    Form form;
    std::optional<SpaceId> space;
  };
  try {
    return Addressed{decode(payload), std::nullopt};
  } catch (const DecodeError&) {
    if (payload.size() < sizeof(SpaceId{}.bytes)) throw;
  }
  const auto split = payload.size() - sizeof(SpaceId{}.bytes);
  SpaceId space;
  std::copy(payload.begin() + static_cast<std::ptrdiff_t>(split), payload.end(), space.bytes.begin());
  return Addressed{decode(payload.first(split)), space};
}
// A request with no form of its own: nothing, or the id. Throws DecodeError otherwise.
std::optional<SpaceId> space_of_empty(std::span<const uint8_t> payload);

}  // namespace dagda::heads::message
