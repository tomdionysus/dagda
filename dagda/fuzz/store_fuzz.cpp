//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The store's messages as a peer sends them (store/messages.h), and a local store's
// files as a disk holds them (store/local_format.h). A message decoder may throw only
// net::DecodeError; a file decoder what its header says.

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/fuzz/samples.h"
#include "dagda/store/durability_ticket.h"
#include "dagda/store/local_format.h"
#include "dagda/store/messages.h"
#include "dannan/fuzz/fuzz.h"
#include "dannan/identity/crypto.h"

namespace dagda::fuzz {
namespace {

using dannan::fuzz::Registration;
using dannan::net::Bytes;
using dannan::net::DecodeError;
namespace message = store::message;
namespace local_format = store::local_format;

// How many answers a reply decoder is told to expect: as many as its seeds carry.
constexpr size_t expected_answers = 3;

message::WritePrincipal sample_principal(dannan::contract::Principal::Kind kind) {
  message::WritePrincipal out;
  out.principal.kind = kind;
  out.principal.id = "user-1";
  out.prefix = "photos/";
  return out;
}

store::DurabilityTicket sample_ticket() {
  store::DurabilityTicket out;
  out.epoch = sample_node(7);
  out.domain = 1;
  out.generation = 2;
  out.backend_instance = 3;
  return out;
}

const Registration id(
    "store_id",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_id(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{message::encode_id(sample_id(1))}; });

const Registration object(
    "store_object",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_object(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      const Bytes bytes{1, 2, 3};
      const Bytes none;
      return std::vector<Bytes>{message::encode_object(sample_id(1), bytes),
                                message::encode_object(sample_id(2), bytes, sample_principal(dannan::contract::Principal::Kind::directory)),
                                message::encode_object(sample_id(3), none, sample_principal(dannan::contract::Principal::Kind::node))};
    });

const Registration principal(
    "store_principal",
    [](std::span<const uint8_t> data) {
      try {
        dannan::net::Reader reader(data);
        (void)message::decode_principal(reader);
        reader.finish();
      } catch (const DecodeError&) {
      }
    },
    [] {
      dannan::net::Writer writer;
      message::encode_principal(writer, sample_principal(dannan::contract::Principal::Kind::directory));
      return std::vector<Bytes>{writer.take()};
    });

const Registration boolean(
    "store_bool",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_bool(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{message::encode_bool(true), message::encode_bool(false)}; });

// have_objects at the presence bound, have_valid_objects at its own.
const Registration ids(
    "store_ids",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_ids(data);
      } catch (const DecodeError&) {
      }
      try {
        (void)message::decode_ids(data, message::max_valid_batch);
      } catch (const DecodeError&) {
      }
    },
    [] {
      const std::vector<store::ObjectId> sought{sample_id(1), sample_id(2), sample_id(3)};
      return std::vector<Bytes>{message::encode_ids(sought)};
    });

const Registration presence(
    "store_presence",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_presence(data, expected_answers);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{message::encode_presence({true, false, true})}; });

const Registration validity(
    "store_validity",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_validity(data, expected_answers);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{message::encode_validity({message::Validity::intact, message::Validity::absent, message::Validity::cannot_say})}; });

const Registration offer_answers(
    "store_offer_answers",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_offer_answers(data, expected_answers);
      } catch (const DecodeError&) {
      }
    },
    [] {
      return std::vector<Bytes>{message::encode_offer_answers({message::OfferAnswer::wanted, message::OfferAnswer::held, message::OfferAnswer::no_room})};
    });

const Registration fetched(
    "store_fetched",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_fetched(data, expected_answers);
      } catch (const DecodeError&) {
      }
    },
    [] {
      std::vector<message::FetchedObject> objects(expected_answers);
      objects[0].as = message::Fetched::sent;
      objects[0].data = Bytes{1, 2, 3};
      objects[1].as = message::Fetched::absent;
      objects[2].as = message::Fetched::left;
      return std::vector<Bytes>{message::encode_fetched(objects)};
    });

const Registration ticket(
    "store_ticket",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_ticket(data);
      } catch (const DecodeError&) {
      }
    },
    [] { return std::vector<Bytes>{message::encode_ticket(sample_ticket())}; });

const Registration barrier(
    "store_barrier",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_barrier(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      message::BarrierRequest plain;
      plain.ticket = sample_ticket();
      message::BarrierRequest probe = plain;
      probe.probe = {sample_id(1), sample_id(2)};
      return std::vector<Bytes>{message::encode_barrier(plain), message::encode_barrier(probe)};
    });

const Registration probe_answer(
    "store_probe_answer",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_probe_answer(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      message::BarrierProbeAnswer answer;
      answer.epoch = sample_node(7);
      answer.present.emplace_back(sample_id(1), sample_ticket());
      answer.present.emplace_back(sample_id(2), sample_ticket());
      return std::vector<Bytes>{message::encode_probe_answer(answer)};
    });

const Registration retain(
    "store_retain",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_retain(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      message::RetainRequest plain;
      plain.object_class = contract::RetentionClass::control;
      plain.dot = {sample_node(2), 9};
      plain.ids = {sample_id(1), sample_id(2)};
      message::RetainRequest written = plain;
      written.object_class = contract::RetentionClass::data;
      written.principal = sample_principal(dannan::contract::Principal::Kind::directory);
      return std::vector<Bytes>{message::encode_retain(plain), message::encode_retain(written)};
    });

const Registration remove_request(
    "store_remove",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_remove(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      message::RemoveRequest request;
      request.observed = {{sample_node(1), 4}, {sample_node(2), 5}};
      request.ids = {sample_id(1), sample_id(2)};
      request.writer = sample_principal(dannan::contract::Principal::Kind::node);
      return std::vector<Bytes>{message::encode_remove(request)};
    });

const Registration removal(
    "store_removal",
    [](std::span<const uint8_t> data) {
      try {
        (void)message::decode_removal(data);
      } catch (const DecodeError&) {
      }
    },
    [] {
      store::ReplicaRemoval refused;
      refused.refused = true;
      refused.scope_generation = 3;
      refused.why = "no scope";
      store::ReplicaRemoval answered;
      store::RemovedObject kept;
      kept.id = sample_id(1);
      kept.state = store::RemoveState::kept_by_claim;
      kept.claims = {{sample_node(1), 2}, {sample_node(2), 3}};
      store::RemovedObject gone;
      gone.id = sample_id(2);
      gone.state = store::RemoveState::removed;
      answered.objects = {kept, gone};
      return std::vector<Bytes>{message::encode_removal(refused), message::encode_removal(answered)};
    });

// A loose object's file: "bad object header" (std::runtime_error) for another magic,
// DecodeError (one) for a file shorter than its header.
const Registration loose(
    "store_loose",
    [](std::span<const uint8_t> data) {
      try {
        const auto opened = local_format::decode_loose(data);
        (void)opened.ciphertext.size();
      } catch (const std::runtime_error&) {
      }
    },
    [] {
      const Bytes plain{1, 2, 3, 4, 5};
      const auto sealed = dannan::identity::aes_gcm_seal(sample_key().bytes(), plain);
      auto file = local_format::encode_loose_header(plain.size(), sealed);
      file.insert(file.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
      return std::vector<Bytes>{file};
    });

// A pack record's header, checked and as the migration reads a torn one: nothing for
// anything else, never a throw.
const Registration pack_header(
    "store_pack_header",
    [](std::span<const uint8_t> data) {
      (void)local_format::decode_pack_header(data);
      (void)local_format::decode_pack_header_unchecked(data);
    },
    [] {
      local_format::PackHeader put;
      put.type = local_format::pack_put;
      put.touched_ms = 1;
      put.id = sample_id(1);
      put.plain_size = 10;
      put.payload_size = 10;
      put.nonce.fill(2);
      put.tag.fill(3);
      local_format::PackHeader removed;
      removed.type = local_format::pack_remove;
      removed.touched_ms = 2;
      removed.id = sample_id(2);
      return std::vector<Bytes>{local_format::encode_pack_header(put), local_format::encode_pack_header(removed)};
    });

// The accounting file's slot: nothing for anything else, never a throw.
const Registration accounting(
    "store_accounting", [](std::span<const uint8_t> data) { (void)local_format::decode_accounting(data); },
    [] {
      local_format::AccountingRecord record;
      record.sequence = 1;
      record.used = 100;
      record.operation = local_format::accounting_dirty;
      record.id = sample_id(1);
      record.size = 10;
      return std::vector<Bytes>{local_format::encode_accounting(record)};
    });

// The names a store's directories hold: nothing for any other name, never a throw.
const Registration file_names(
    "store_file_names",
    [](std::span<const uint8_t> data) {
      const std::string name(data.begin(), data.end());
      (void)local_format::object_id_from_file_name(name);
      (void)local_format::pack_sequence(name);
    },
    [] {
      const auto loose_name = store::to_string(sample_id(0xab)) + ".obj";
      const auto pack_file = local_format::pack_name(5);
      return std::vector<Bytes>{Bytes(loose_name.begin(), loose_name.end()), Bytes(pack_file.begin(), pack_file.end())};
    });

}  // namespace
}  // namespace dagda::fuzz
