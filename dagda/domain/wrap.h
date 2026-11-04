//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dagda/domain/domain_id.h"
#include "dagda/domain/storage_key.h"
#include "dannan/context.h"
#include "dannan/identity/certificate.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/node_id.h"
#include "dannan/identity/secure_memory.h"
#include "dannan/membership/node_record.h"

// Key wrap (the maintainer, 2026-11-04 evening): a node certificate's key is a signing
// key, so each node derives an X25519 wrap key from its custody, publishes
// the public half as a Dagda advert signed with its certificate key, and a
// domain key is sealed to each member under HKDF of the X25519 shared secret
// with AES-GCM, the domain and the member bound in the associated data.
namespace dagda::domain {

using Bytes = std::vector<uint8_t>;

// The custody label the wrap key is derived under; also the label the
// advert's signature and the wrap's associated data start with.
inline constexpr std::string_view wrap_label = "dagda/wrap/1";
// The HKDF info the sealing key is derived under.
inline constexpr std::string_view wrap_sealing_info = "dagda/domain-wrap/1";
// The advert on the node record carrying the signed public half.
inline constexpr const char* wrap_advert = "dagda.wrap";

// This node's X25519 wrap key: the private half in secure memory, clamped
// as X25519 requires; the public half derived from it. Move-only; read-only
// once made, so any thread may read it.
class WrapKey {
 public:
  // From the node's custody under wrap_label; nothing without custody.
  // Waits on nothing.
  static std::optional<WrapKey> derive(const dannan::Identity& identity);
  // From 32 bytes of private key, clamped here. Throws std::invalid_argument
  // for another size.
  static WrapKey from_private(dannan::identity::Secret seed);

  std::span<const uint8_t, 32> public_key() const noexcept { return _public; }
  // X25519 with a peer's public half. Waits on nothing.
  std::array<uint8_t, 32> shared(std::span<const uint8_t, 32> peer_public) const;

 private:
  explicit WrapKey(dannan::identity::Secret clamped);

  dannan::identity::Secret _private;
  std::array<uint8_t, 32> _public{};
};

// A domain key sealed to one member by one wrapper: AES-256-GCM of the 32
// key bytes under HKDF-SHA256(X25519(wrapper private, member public), no
// salt, wrap_sealing_info), with associated data wrap_label || domain id ||
// member id || wrapper id, so a record cannot be passed off under another
// domain, to another member or from another wrapper.
struct WrappedKey {
  dannan::identity::NodeId wrapper;
  dannan::identity::NodeId member;
  DomainId domain{};
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
  Bytes ciphertext;
  bool operator==(const WrappedKey&) const = default;
};

// The associated data of a wrap.
Bytes wrap_associated_data(const DomainId& domain, const dannan::identity::NodeId& member, const dannan::identity::NodeId& wrapper);
// Seals `key` to `member` under a fresh nonce. Throws std::runtime_error if
// the cipher fails. Waits on nothing; thread-safe.
WrappedKey wrap(const StorageKey& key, const DomainId& domain, const dannan::identity::NodeId& wrapper, const WrapKey& wrapper_key,
                const dannan::identity::NodeId& member, std::span<const uint8_t, 32> member_public);
// As above under the nonce given: to reproduce a known answer in a test.
WrappedKey wrap(const StorageKey& key, const DomainId& domain, const dannan::identity::NodeId& wrapper, const WrapKey& wrapper_key,
                const dannan::identity::NodeId& member, std::span<const uint8_t, 32> member_public, std::span<const uint8_t, 12> nonce);
// Opens what wrap sealed, on the member, with the wrapper's public half from
// its advert. The key lands in secure memory. Throws std::runtime_error when
// authentication fails. Waits on nothing; thread-safe.
StorageKey unwrap(const WrappedKey& wrapped, const WrapKey& member_key, std::span<const uint8_t, 32> wrapper_public);

// Dagda's own bytes: wrapper, member, domain, nonce, tag, then the ciphertext
// as a blob. Decoding throws dannan::net::DecodeError.
Bytes encode_wrapped_key(const WrappedKey& wrapped);
WrappedKey decode_wrapped_key(std::span<const uint8_t> bytes);

// The advert: the public half, a signature by the node's certificate key
// over wrap_label || node id || public half, and the node's certificate
// (DER), by which a peer checks the signer. The certificate travels in the
// advert because one advert goes to every node, including those that have
// never met this one, for which Nodes::certificate has nothing.
struct WrapAdvert {
  std::array<uint8_t, 32> public_key{};
  Bytes signature;
  Bytes certificate;
  bool operator==(const WrapAdvert&) const = default;
};
Bytes wrap_advert_message(const dannan::identity::NodeId& node, std::span<const uint8_t, 32> public_key);
Bytes encode_wrap_advert(const WrapAdvert& advert);
// Nothing for bytes that are not an advert.
std::optional<WrapAdvert> decode_wrap_advert(std::span<const uint8_t> bytes);
// Signs and publishes this node's advert (Nodes::advertise). Throws
// std::logic_error on a node without custody or a certificate. Waits on
// nothing.
void advertise_wrap_key(dannan::Context& context, const WrapKey& key);

// What checking a peer's advert found. With the cluster's CA the
// certificate is checked as a node certificate of this cluster
// (check_node_certificate) and against the revocations given; without it the
// issuer goes unchecked and `issuer_checked` says so: the signature then
// proves only that the advert's certificate and public half belong together
// and name the peer.
struct WrapAdvertCheck {
  bool ok = false;
  bool issuer_checked = false;
  std::string why;
  std::array<uint8_t, 32> public_key{};
};
// `now_unix_ms` is the receiver's clock. Waits on nothing; thread-safe.
WrapAdvertCheck verify_wrap_advert(const dannan::membership::NodeRecord& peer, const std::optional<dannan::identity::Certificate>& ca, uint64_t now_unix_ms,
                                   const std::vector<dannan::identity::Revocation>& revocations = {});

}  // namespace dagda::domain
