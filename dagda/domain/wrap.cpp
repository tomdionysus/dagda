//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/domain/wrap.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "dannan/identity/signing.h"
#include "dannan/net/codec.h"

namespace dagda::domain {
namespace {

using dannan::identity::NodeId;
using dannan::identity::Secret;
using dannan::net::Reader;
using dannan::net::Writer;

std::span<const uint8_t> text(std::string_view value) { return {reinterpret_cast<const uint8_t*>(value.data()), value.size()}; }

// The sealing key of a wrap, in secure memory; the shared secret is wiped
// once it is derived from.
Secret sealing_key(const WrapKey& own, std::span<const uint8_t, 32> peer_public) {
  auto shared = own.shared(peer_public);
  auto key = dannan::identity::hkdf_sha256_secret(shared, {}, text(wrap_sealing_info));
  dannan::identity::cleanse(shared);
  return key;
}

std::span<const uint8_t, 32> key_span(const Secret& secret) { return std::span<const uint8_t, 32>(secret.reveal().data(), 32); }

}  // namespace

std::optional<WrapKey> WrapKey::derive(const dannan::Identity& identity) {
  if (!identity.has_custody()) return std::nullopt;
  return from_private(identity.derive_local(wrap_label));
}

WrapKey WrapKey::from_private(Secret seed) {
  if (seed.size() != 32) throw std::invalid_argument("an X25519 private key is 32 bytes");
  dannan::identity::SecureBytes clamped(32);
  std::copy(seed.reveal().begin(), seed.reveal().end(), clamped.data());
  clamped.data()[0] &= 248;
  clamped.data()[31] &= 127;
  clamped.data()[31] |= 64;
  return WrapKey(Secret(std::move(clamped)));
}

WrapKey::WrapKey(Secret clamped) : _private(std::move(clamped)) { _public = dannan::identity::x25519_public(key_span(_private)); }

std::array<uint8_t, 32> WrapKey::shared(std::span<const uint8_t, 32> peer_public) const {
  return dannan::identity::x25519_shared(key_span(_private), peer_public);
}

Bytes wrap_associated_data(const DomainId& domain, const NodeId& member, const NodeId& wrapper) {
  Writer out;
  out.raw(text(wrap_label));
  out.fixed(domain.bytes);
  out.fixed(member.bytes);
  out.fixed(wrapper.bytes);
  return out.take();
}

WrappedKey wrap(const StorageKey& key, const DomainId& domain, const NodeId& wrapper, const WrapKey& wrapper_key, const NodeId& member,
                std::span<const uint8_t, 32> member_public) {
  const auto nonce = dannan::identity::random_bytes(12);
  return wrap(key, domain, wrapper, wrapper_key, member, member_public, std::span<const uint8_t, 12>(nonce.data(), 12));
}

WrappedKey wrap(const StorageKey& key, const DomainId& domain, const NodeId& wrapper, const WrapKey& wrapper_key, const NodeId& member,
                std::span<const uint8_t, 32> member_public, std::span<const uint8_t, 12> nonce) {
  const auto sealing = sealing_key(wrapper_key, member_public);
  auto sealed = dannan::identity::aes_gcm_seal(key_span(sealing), nonce, key.bytes(), wrap_associated_data(domain, member, wrapper));
  WrappedKey out;
  out.wrapper = wrapper;
  out.member = member;
  out.domain = domain;
  out.nonce = sealed.nonce;
  out.tag = sealed.tag;
  out.ciphertext = std::move(sealed.ciphertext);
  return out;
}

StorageKey unwrap(const WrappedKey& wrapped, const WrapKey& member_key, std::span<const uint8_t, 32> wrapper_public) {
  const auto sealing = sealing_key(member_key, wrapper_public);
  auto plain = dannan::identity::aes_gcm_open_secure(key_span(sealing), wrapped.nonce, wrapped.tag, wrapped.ciphertext,
                                                     wrap_associated_data(wrapped.domain, wrapped.member, wrapped.wrapper));
  if (plain.size() != 32) throw std::runtime_error("a wrapped domain key is 32 bytes");
  return StorageKey(Secret(std::move(plain)));
}

Bytes encode_wrapped_key(const WrappedKey& wrapped) {
  Writer out;
  out.fixed(wrapped.wrapper.bytes);
  out.fixed(wrapped.member.bytes);
  out.fixed(wrapped.domain.bytes);
  out.fixed(wrapped.nonce);
  out.fixed(wrapped.tag);
  out.bytes(wrapped.ciphertext);
  return out.take();
}

WrappedKey decode_wrapped_key(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  WrappedKey out;
  out.wrapper.bytes = reader.fixed<16>();
  out.member.bytes = reader.fixed<16>();
  out.domain.bytes = reader.fixed<16>();
  out.nonce = reader.fixed<12>();
  out.tag = reader.fixed<16>();
  out.ciphertext = reader.bytes(1024);
  reader.finish();
  return out;
}

Bytes wrap_advert_message(const NodeId& node, std::span<const uint8_t, 32> public_key) {
  Writer out;
  out.raw(text(wrap_label));
  out.fixed(node.bytes);
  out.raw(public_key);
  return out.take();
}

Bytes encode_wrap_advert(const WrapAdvert& advert) {
  Writer out;
  out.fixed(advert.public_key);
  out.bytes(advert.signature);
  out.bytes(advert.certificate);
  return out.take();
}

std::optional<WrapAdvert> decode_wrap_advert(std::span<const uint8_t> bytes) {
  try {
    Reader reader(bytes);
    WrapAdvert out;
    out.public_key = reader.fixed<32>();
    out.signature = reader.bytes(256);
    out.certificate = reader.bytes(dannan::membership::max_advert_value);
    reader.finish();
    return out;
  } catch (const dannan::net::DecodeError&) {
    return std::nullopt;
  }
}

void advertise_wrap_key(dannan::Context& context, const WrapKey& key) {
  auto& identity = context.identity();
  if (!identity.has_custody()) throw std::logic_error("a wrap advert needs the node's custody");
  const auto certificate = identity.certificate();
  if (!certificate) throw std::logic_error("a wrap advert is signed by the node's certificate key; this node has no certificate");
  WrapAdvert advert;
  std::copy(key.public_key().begin(), key.public_key().end(), advert.public_key.begin());
  advert.signature = identity.sign(wrap_advert_message(identity.id(), key.public_key()));
  advert.certificate = certificate->der();
  context.nodes().advertise(wrap_advert, encode_wrap_advert(advert));
}

WrapAdvertCheck verify_wrap_advert(const dannan::membership::NodeRecord& peer, const std::optional<dannan::identity::Certificate>& ca, uint64_t now_unix_ms,
                                   const std::vector<dannan::identity::Revocation>& revocations) {
  WrapAdvertCheck out;
  const auto bytes = peer.advert(wrap_advert);
  if (!bytes) {
    out.why = "no wrap advert";
    return out;
  }
  const auto advert = decode_wrap_advert(*bytes);
  if (!advert) {
    out.why = "wrap advert unreadable";
    return out;
  }
  std::optional<dannan::identity::Certificate> certificate;
  try {
    certificate = dannan::identity::Certificate::from_der(advert->certificate);
  } catch (const std::runtime_error& error) {
    out.why = std::string("wrap advert certificate unreadable: ") + error.what();
    return out;
  }
  if (certificate->node_id() != peer.id) {
    out.why = "wrap advert certificate names another node";
    return out;
  }
  if (ca) {
    if (const auto refused = dannan::identity::check_node_certificate(*certificate, *ca, now_unix_ms); !refused.empty()) {
      out.why = "wrap advert certificate refused: " + refused;
      return out;
    }
    for (const auto& revocation : revocations)
      if (revocation.covers(*certificate)) {
        out.why = "wrap advert certificate revoked: " + revocation.reason;
        return out;
      }
    out.issuer_checked = true;
  }
  const auto public_key = certificate->public_key();
  const auto algorithm = dannan::identity::algorithm_of_public_key(public_key);
  if (!algorithm || !dannan::identity::verify(*algorithm, public_key, wrap_advert_message(peer.id, advert->public_key), advert->signature)) {
    out.why = "wrap advert signature does not verify under the certificate";
    return out;
  }
  out.ok = true;
  out.public_key = advert->public_key;
  if (!ca) out.why = "issuer unchecked: no CA certificate given";
  return out;
}

}  // namespace dagda::domain
