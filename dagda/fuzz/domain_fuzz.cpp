//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A domain's bytes from other nodes (domain/wrap.h): a wrapped domain key, decoded
// (net::DecodeError) and unwrapped (std::runtime_error), and a node record's wrap
// advert, decoded and checked, which answers and never throws. And a domain id as
// text, which parses to nothing on anything else.

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dagda/domain/domain_id.h"
#include "dagda/domain/wrap.h"
#include "dagda/fuzz/samples.h"
#include "dannan/fuzz/fuzz.h"
#include "dannan/identity/certificate.h"
#include "dannan/identity/signing.h"
#include "dannan/membership/node_record.h"

namespace dagda::fuzz {
namespace {

using dannan::fuzz::Registration;
using dannan::net::Bytes;
using dannan::net::DecodeError;

constexpr uint64_t issued_unix_ms = 1'800'000'000'000;
constexpr uint64_t checked_unix_ms = issued_unix_ms + 60'000;
constexpr uint8_t advertiser = 3;

domain::WrapKey wrap_key(uint8_t fill) {
  std::array<uint8_t, 32> seed{};
  seed.fill(fill);
  return domain::WrapKey::from_private(dannan::identity::Secret(std::span<const uint8_t>(seed)));
}

// Two nodes' wrap keys, and a cluster whose CA certified the advertiser: made once,
// since each is a key generation.
struct WrapFixture {
  domain::WrapKey wrapper = wrap_key(0x11);
  domain::WrapKey member = wrap_key(0x22);
  dannan::identity::CertificateAuthority ca = dannan::identity::CertificateAuthority::create("fuzz", issued_unix_ms);
  dannan::identity::SigningKey node_key = dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::es256);
  dannan::identity::Certificate certificate = ca.issue(sample_node(advertiser), node_key.public_key(), 7, issued_unix_ms);
};

const WrapFixture& wrap_fixture() {
  static const WrapFixture fixture;
  return fixture;
}

const Registration wrapped_key(
    "domain_wrapped_key",
    [](std::span<const uint8_t> data) {
      const auto& fixture = wrap_fixture();
      try {
        const auto wrapped = domain::decode_wrapped_key(data);
        try {
          (void)domain::unwrap(wrapped, fixture.member, fixture.wrapper.public_key());
        } catch (const std::runtime_error&) {
        }
      } catch (const DecodeError&) {
      }
    },
    [] {
      const auto& fixture = wrap_fixture();
      std::array<uint8_t, 12> nonce{};
      nonce.fill(9);
      const auto wrapped = domain::wrap(sample_key(), domain::domain_v1, sample_node(1), fixture.wrapper, sample_node(2), fixture.member.public_key(),
                                        std::span<const uint8_t, 12>(nonce));
      return std::vector<Bytes>{domain::encode_wrapped_key(wrapped)};
    });

// The advert as a peer's record carries it, checked with the cluster's CA and without.
const Registration wrap_advert_record(
    "domain_wrap_advert",
    [](std::span<const uint8_t> data) {
      const auto& fixture = wrap_fixture();
      (void)domain::decode_wrap_advert(data);
      dannan::membership::NodeRecord peer;
      peer.id = sample_node(advertiser);
      peer.adverts[domain::wrap_advert] = Bytes(data.begin(), data.end());
      (void)domain::verify_wrap_advert(peer, fixture.ca.certificate(), checked_unix_ms);
      (void)domain::verify_wrap_advert(peer, std::nullopt, checked_unix_ms);
    },
    [] {
      const auto& fixture = wrap_fixture();
      domain::WrapAdvert advert;
      const auto public_key = fixture.wrapper.public_key();
      std::copy(public_key.begin(), public_key.end(), advert.public_key.begin());
      advert.signature = fixture.node_key.sign(domain::wrap_advert_message(sample_node(advertiser), public_key));
      advert.certificate = fixture.certificate.der();
      return std::vector<Bytes>{domain::encode_wrap_advert(advert)};
    });

const Registration domain_id(
    "domain_id",
    [](std::span<const uint8_t> data) {
      const std::string text(data.begin(), data.end());
      (void)domain::DomainId::parse(text);
    },
    [] {
      const auto text = domain::to_string(domain::domain_v1);
      return std::vector<Bytes>{Bytes(text.begin(), text.end())};
    });

}  // namespace
}  // namespace dagda::fuzz
