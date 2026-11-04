//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Key wrap (step 6; the maintainer, 2026-11-04 evening): the X25519 wrap key from a
// node's custody, its signed advert, and a domain key sealed to a member.
// The known answers were computed outside Dagda's code path: X25519 by the
// RFC 7748 ladder in Python (checked against the RFC's vector), AES blocks
// by `openssl enc -aes-256-ecb`, GCM by hand (checked against the GCM
// specification's test case 16), HKDF by RFC 5869 with hashlib (checked
// against its test case 1); build/kat/step6/kat.py at the time of writing.

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include "dagda/domain/storage_key.h"
#include "dagda/domain/wrap.h"
#include "dannan/identity/argon2.h"
#include "dannan/identity/certificate.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/key_custody.h"
#include "dannan/identity/node_identity.h"
#include "dannan/identity/signing.h"
#include "dannan/net/codec.h"
#include "dannan/node.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dagda::domain {
namespace {

using dannan::identity::hex;
using dannan::identity::NodeId;
using dannan::identity::Secret;

Bytes unhex(const std::string& text) { return dannan::identity::unhex(text).value(); }

Secret secret_of(const std::string& hex_text) { return Secret(unhex(hex_text)); }

NodeId node_filled(uint8_t byte) {
  NodeId out;
  out.bytes.fill(byte);
  return out;
}

NodeId node_of(uint8_t first) {
  NodeId out;
  for (size_t i = 0; i < 16; ++i) out.bytes[i] = static_cast<uint8_t>(first + i);
  return out;
}

// The known-answer keys: private seeds 01..20 and 41..60.
constexpr char wrapper_seed_hex[] = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
constexpr char wrapper_public_hex[] = "07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c";
constexpr char member_seed_hex[] = "4142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60";
constexpr char member_public_hex[] = "64b101b1d0be5a8704bd078f9895001fc03e8e9f9522f188dd128d9846d48466";
constexpr char shared_hex[] = "26c2c17fdb82161cb21ad16e721315355b64d1763119b10bfc962530dc7cc163";
constexpr char domain_key_hex[] = "879277d5274170fabe955dd8b14c14edbd54533790e24617b7cff11a9679bbd8";
constexpr char domain_hex[] = "00112233445566778899aabbccddeeff";
constexpr char nonce_hex[] = "000102030405060708090a0b";
constexpr char wrap_ciphertext_hex[] = "5ca606ee08aef3cdbe72b4c79785c536c2bd18e8ce9ed9b02e1b6732155036d9";
constexpr char wrap_tag_hex[] = "c47a13d27c98e355b69a6d92d179a33f";

// The public half is the clamped private scalar times the base point: RFC
// 7748's X25519, computed through Dannan's x25519_shared with u = 9.
TEST(DomainWrap, ThePublicHalfIsDerivedFromThePrivate) {
  const auto wrapper = WrapKey::from_private(secret_of(wrapper_seed_hex));
  EXPECT_EQ(hex(wrapper.public_key()), wrapper_public_hex);
  const auto member = WrapKey::from_private(secret_of(member_seed_hex));
  EXPECT_EQ(hex(member.public_key()), member_public_hex);
  EXPECT_EQ(hex(wrapper.shared(member.public_key())), shared_hex);
  EXPECT_EQ(hex(member.shared(wrapper.public_key())), shared_hex);
  // Clamping: the seed's low three bits and top bit are cleared and bit 254
  // set, so a seed already clamped is the same key.
  auto clamped = unhex(wrapper_seed_hex);
  clamped[0] &= 248;
  clamped[31] = static_cast<uint8_t>((clamped[31] & 127) | 64);
  EXPECT_EQ(hex(WrapKey::from_private(Secret(clamped)).public_key()), wrapper_public_hex);
  EXPECT_THROW(WrapKey::from_private(Secret::random(31)), std::invalid_argument);
}

// The wrap bytes: AES-256-GCM of the domain key under HKDF of the shared
// secret, with the domain, member and wrapper in the associated data.
TEST(DomainWrap, WrapsAKeyToAMemberAsTheKnownAnswer) {
  const auto wrapper = WrapKey::from_private(secret_of(wrapper_seed_hex));
  const auto member = WrapKey::from_private(secret_of(member_seed_hex));
  const StorageKey key(secret_of(domain_key_hex));
  const auto domain = DomainId::parse(domain_hex).value();
  const auto wrapper_id = node_filled(0xaa);
  const auto member_id = node_filled(0xbb);
  const auto nonce = unhex(nonce_hex);
  EXPECT_EQ(hex(wrap_associated_data(domain, member_id, wrapper_id)),
            "64616764612f777261702f3100112233445566778899aabbccddeeffbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  const auto wrapped = wrap(key, domain, wrapper_id, wrapper, member_id, member.public_key(), std::span<const uint8_t, 12>(nonce.data(), 12));
  EXPECT_EQ(wrapped.wrapper, wrapper_id);
  EXPECT_EQ(wrapped.member, member_id);
  EXPECT_EQ(wrapped.domain, domain);
  EXPECT_EQ(hex(wrapped.nonce), nonce_hex);
  EXPECT_EQ(hex(wrapped.ciphertext), wrap_ciphertext_hex);
  EXPECT_EQ(hex(wrapped.tag), wrap_tag_hex);
  // The member opens it with the wrapper's public half.
  const auto opened = unwrap(wrapped, member, wrapper.public_key());
  EXPECT_EQ(hex(opened.bytes()), domain_key_hex);
}

TEST(DomainWrap, AFreshNonceEachWrapAndTheMemberOpensIt) {
  const auto wrapper = WrapKey::from_private(Secret::random(32));
  const auto member = WrapKey::from_private(Secret::random(32));
  const StorageKey key(Secret::random(32));
  const auto domain = node_local_domain(node_of(1));
  const auto first = wrap(key, domain, node_of(1), wrapper, node_of(2), member.public_key());
  const auto second = wrap(key, domain, node_of(1), wrapper, node_of(2), member.public_key());
  EXPECT_NE(first.nonce, second.nonce);
  EXPECT_EQ(first.ciphertext.size(), 32u);
  EXPECT_EQ(hex(unwrap(first, member, wrapper.public_key()).bytes()), hex(key.bytes()));
  EXPECT_EQ(hex(unwrap(second, member, wrapper.public_key()).bytes()), hex(key.bytes()));
}

// The binding: another domain, another member, another wrapper, another
// member key or another wrapper public half opens nothing.
TEST(DomainWrap, AWrapOpensOnlyForWhomAndWhatItNames) {
  const auto wrapper = WrapKey::from_private(secret_of(wrapper_seed_hex));
  const auto member = WrapKey::from_private(secret_of(member_seed_hex));
  const auto stranger = WrapKey::from_private(Secret::random(32));
  const StorageKey key(secret_of(domain_key_hex));
  const auto domain = DomainId::parse(domain_hex).value();
  const auto wrapped = wrap(key, domain, node_filled(0xaa), wrapper, node_filled(0xbb), member.public_key());
  auto other_domain = wrapped;
  other_domain.domain.bytes[0] ^= 1;
  EXPECT_THROW((void)unwrap(other_domain, member, wrapper.public_key()), std::runtime_error);
  auto other_member = wrapped;
  other_member.member = node_filled(0xcc);
  EXPECT_THROW((void)unwrap(other_member, member, wrapper.public_key()), std::runtime_error);
  auto other_wrapper = wrapped;
  other_wrapper.wrapper = node_filled(0xcc);
  EXPECT_THROW((void)unwrap(other_wrapper, member, wrapper.public_key()), std::runtime_error);
  EXPECT_THROW((void)unwrap(wrapped, stranger, wrapper.public_key()), std::runtime_error);
  EXPECT_THROW((void)unwrap(wrapped, member, stranger.public_key()), std::runtime_error);
  auto flipped = wrapped;
  flipped.ciphertext[5] ^= 1;
  EXPECT_THROW((void)unwrap(flipped, member, wrapper.public_key()), std::runtime_error);
  EXPECT_EQ(hex(unwrap(wrapped, member, wrapper.public_key()).bytes()), domain_key_hex);
}

// Dagda's own bytes: wrapper, member, domain, nonce, tag, the ciphertext as
// a blob.
TEST(DomainWrap, AWrappedKeyRoundTripsThroughItsBytes) {
  const auto wrapper = WrapKey::from_private(secret_of(wrapper_seed_hex));
  const auto member = WrapKey::from_private(secret_of(member_seed_hex));
  const StorageKey key(secret_of(domain_key_hex));
  const auto nonce = unhex(nonce_hex);
  const auto wrapped = wrap(key, DomainId::parse(domain_hex).value(), node_filled(0xaa), wrapper, node_filled(0xbb), member.public_key(),
                            std::span<const uint8_t, 12>(nonce.data(), 12));
  const auto bytes = encode_wrapped_key(wrapped);
  EXPECT_EQ(hex(bytes), std::string(32, 'a') + std::string(32, 'b') + domain_hex + nonce_hex + wrap_tag_hex + "00000020" + wrap_ciphertext_hex);
  EXPECT_EQ(decode_wrapped_key(bytes), wrapped);
  auto truncated = bytes;
  truncated.pop_back();
  EXPECT_THROW(decode_wrapped_key(truncated), dannan::net::DecodeError);
}

// The advert: the signed message is the label, the node id and the public
// half; the certificate rides along so a peer can check the signer.
TEST(DomainWrap, TheAdvertIsSignedByTheCertificateKeyAndChecked) {
  const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  const auto ca = dannan::identity::CertificateAuthority::create("dagda test", now - 60000);
  const auto other_ca = dannan::identity::CertificateAuthority::create("dagda test", now - 60000);
  const auto node = node_of(0x10);
  const auto signing = dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::es256);
  const auto certificate = ca.issue(node, signing.public_key(), 7, now - 1000);
  const auto wrapper = WrapKey::from_private(secret_of(wrapper_seed_hex));
  EXPECT_EQ(hex(wrap_advert_message(node, wrapper.public_key())),
            std::string("64616764612f777261702f31") + "101112131415161718191a1b1c1d1e1f" + wrapper_public_hex);

  WrapAdvert advert;
  std::copy(wrapper.public_key().begin(), wrapper.public_key().end(), advert.public_key.begin());
  advert.signature = signing.sign(wrap_advert_message(node, wrapper.public_key()));
  advert.certificate = certificate.der();
  const auto bytes = encode_wrap_advert(advert);
  EXPECT_LE(bytes.size(), dannan::membership::max_advert_value);
  EXPECT_EQ(decode_wrap_advert(bytes), advert);
  EXPECT_FALSE(decode_wrap_advert(Bytes(31, 0)).has_value());

  dannan::membership::NodeRecord peer;
  peer.id = node;
  peer.adverts[wrap_advert] = bytes;
  const auto checked = verify_wrap_advert(peer, ca.certificate(), now);
  EXPECT_TRUE(checked.ok) << checked.why;
  EXPECT_TRUE(checked.issuer_checked);
  EXPECT_EQ(hex(checked.public_key), wrapper_public_hex);
  // Without the CA the signature and the name are checked, the issuer not,
  // and the check says so.
  const auto unchecked = verify_wrap_advert(peer, std::nullopt, now);
  EXPECT_TRUE(unchecked.ok);
  EXPECT_FALSE(unchecked.issuer_checked);
  EXPECT_NE(unchecked.why.find("issuer unchecked"), std::string::npos);
  // Another CA's node, a certificate of another node, a tampered public
  // half, a tampered signature, a revoked certificate, no advert: refused.
  EXPECT_FALSE(verify_wrap_advert(peer, other_ca.certificate(), now).ok);
  dannan::membership::NodeRecord impostor = peer;
  impostor.id = node_of(0x20);
  EXPECT_FALSE(verify_wrap_advert(impostor, ca.certificate(), now).ok);
  auto tampered = advert;
  tampered.public_key[0] ^= 1;
  peer.adverts[wrap_advert] = encode_wrap_advert(tampered);
  EXPECT_FALSE(verify_wrap_advert(peer, ca.certificate(), now).ok);
  tampered = advert;
  tampered.signature[3] ^= 1;
  peer.adverts[wrap_advert] = encode_wrap_advert(tampered);
  EXPECT_FALSE(verify_wrap_advert(peer, ca.certificate(), now).ok);
  peer.adverts[wrap_advert] = bytes;
  const auto revoked = verify_wrap_advert(peer, ca.certificate(), now, {ca.revoke(node, 7, now, "lost")});
  EXPECT_FALSE(revoked.ok);
  EXPECT_NE(revoked.why.find("revoked"), std::string::npos);
  EXPECT_TRUE(verify_wrap_advert(peer, ca.certificate(), now, {ca.revoke(node, 8, now, "another serial")}).ok);
  peer.adverts.erase(wrap_advert);
  EXPECT_FALSE(verify_wrap_advert(peer, ca.certificate(), now).ok);
}

// A node's keys come from its custody through the Context: the node-local
// domain key and the wrap key, each the same every time and never the
// other; without custody both are absent, never made up; the advert needs a
// certificate to sign with.
TEST(DomainWrap, TheNodesKeysComeFromItsCustodyOrAreAbsent) {
  boost::asio::io_context io;
  dannan::test::TempDir dir;
  std::ofstream(dir.path() / "cluster.key") << "wrap test";
  const auto cluster = dannan::identity::load_cluster_key(dir.path() / "cluster.key");
  const auto source = dannan::identity::KeySource::typed_passphrase(
      [] {
        const std::string p = "node";
        return Secret(std::span(reinterpret_cast<const uint8_t*>(p.data()), p.size()));
      },
      dannan::identity::Argon2Costs{1024, 1, 1});
  const auto config_of = [&](const std::filesystem::path& state) {
    dannan::NodeConfig config;
    config.state_path = state;
    config.key = cluster;
    config.listen_host = "127.0.0.1";
    config.advertise_host = "127.0.0.1";
    config.port = 0;
    config.version = "dagda-test";
    return config;
  };
  (void)dannan::identity::KeyCustody::create(dir.path() / "a", source);
  auto with = config_of(dir.path() / "a");
  with.custody = source;
  dannan::Node a(io, with);
  dannan::Node b(io, config_of(dir.path() / "b"));

  const auto local = StorageKey::derive_node_local(a.context().identity());
  ASSERT_TRUE(local.has_value());
  EXPECT_EQ(hex(local->bytes()), hex(StorageKey::derive_node_local(a.context().identity())->bytes()));
  const auto wrap_key = WrapKey::derive(a.context().identity());
  ASSERT_TRUE(wrap_key.has_value());
  EXPECT_EQ(hex(wrap_key->public_key()), hex(WrapKey::derive(a.context().identity())->public_key()));
  EXPECT_NE(hex(local->bytes()), hex(a.context().identity().derive_local(wrap_label).reveal()));
  EXPECT_FALSE(StorageKey::derive_node_local(b.context().identity()).has_value());
  EXPECT_FALSE(WrapKey::derive(b.context().identity()).has_value());
  // Custody but no certificate: nothing to sign the advert with.
  EXPECT_THROW(advertise_wrap_key(a.context(), *wrap_key), std::logic_error);

  // With a certificate the advert is published on the node's record and
  // verifies under the CA.
  const auto now = a.context().time().unix_ms();
  const auto ca = dannan::identity::CertificateAuthority::create("dagda test", now - 60000);
  const auto c_state = dir.path() / "c";
  (void)dannan::identity::KeyCustody::create(c_state, source);
  const auto c_id = dannan::identity::load_or_create_node_id(c_state);
  const auto c_public = dannan::identity::KeyCustody::open(c_state, source).node_key().public_key();
  auto certified = config_of(c_state);
  certified.custody = source;
  certified.ca_certificate = ca.certificate();
  certified.certificate = ca.issue(c_id, c_public, 1, now - 1000);
  dannan::Node c(io, certified);
  const auto c_wrap = WrapKey::derive(c.context().identity());
  ASSERT_TRUE(c_wrap.has_value());
  advertise_wrap_key(c.context(), *c_wrap);
  const auto checked = verify_wrap_advert(c.context().nodes().self(), ca.certificate(), now);
  EXPECT_TRUE(checked.ok) << checked.why;
  EXPECT_TRUE(checked.issuer_checked);
  EXPECT_EQ(hex(checked.public_key), hex(c_wrap->public_key()));
}

}  // namespace
}  // namespace dagda::domain
