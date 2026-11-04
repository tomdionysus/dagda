//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/migrate/retag.h"

#include <algorithm>
#include <vector>

#include "dannan/identity/crypto.h"

namespace dagda::migrate {
namespace {

// An element of GF(2^128) in GCM's bit order: the first bit is the x^0 term.
struct Element {
  uint64_t high = 0;
  uint64_t low = 0;
  Element operator^(const Element& other) const noexcept { return {high ^ other.high, low ^ other.low}; }
};

constexpr Element one{1ULL << 63, 0};

Element load(std::span<const uint8_t> block) {
  std::array<uint8_t, 16> padded{};
  std::copy_n(block.begin(), std::min<size_t>(block.size(), 16), padded.begin());
  Element out;
  for (size_t i = 0; i < 8; ++i) out.high = (out.high << 8) | padded[i];
  for (size_t i = 8; i < 16; ++i) out.low = (out.low << 8) | padded[i];
  return out;
}

TagDeriver::Tag store(const Element& element) {
  TagDeriver::Tag out{};
  for (size_t i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(element.high >> (56 - 8 * i));
  for (size_t i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(element.low >> (56 - 8 * i));
  return out;
}

// Bit by bit: a handful of multiplications a record, never the bulk of an
// object, which is why this need not be fast and has no secret-dependent
// table. The migration runs on the operator's own machine.
Element multiply(const Element& x, const Element& y) noexcept {
  Element z;
  Element v = x;
  for (int i = 0; i < 128; ++i) {
    const uint64_t bit = i < 64 ? (y.high >> (63 - i)) & 1 : (y.low >> (127 - i)) & 1;
    if (bit) z = z ^ v;
    const bool carry = v.low & 1;
    v.low = (v.low >> 1) | (v.high << 63);
    v.high >>= 1;
    if (carry) v.high ^= 0xe1ULL << 56;
  }
  return z;
}

Element power(Element base, uint64_t exponent) noexcept {
  Element out = one;
  while (exponent) {
    if (exponent & 1) out = multiply(out, base);
    base = multiply(base, base);
    exponent >>= 1;
  }
  return out;
}

// GHASH's Horner over the padded associated data.
Element horner(std::span<const uint8_t> associated, const Element& h) noexcept {
  Element x;
  for (size_t offset = 0; offset < associated.size(); offset += 16)
    x = multiply(x ^ load(associated.subspan(offset, std::min<size_t>(16, associated.size() - offset))), h);
  return x;
}

}  // namespace

TagDeriver::TagDeriver(const domain::StorageKey& key) {
  // One block of associated data over nothing: tag = A * H^2 xor L * H xor
  // E(J0). Two A that differ by one leave H^2; 127 squarings take its root.
  const std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> a{};
  std::array<uint8_t, 16> b{};
  a[0] = 0x80;
  const auto sealed_a = dannan::identity::aes_gcm_seal(key.bytes(), nonce, {}, a);
  const auto sealed_b = dannan::identity::aes_gcm_seal(key.bytes(), nonce, {}, b);
  Element h = load(sealed_a.tag) ^ load(sealed_b.tag);
  for (int i = 0; i < 127; ++i) h = multiply(h, h);
  _h_high = h.high;
  _h_low = h.low;
}

TagDeriver::Tag TagDeriver::derive(std::span<const uint8_t, 16> tag, std::span<const uint8_t> from, std::span<const uint8_t> to,
                                   uint64_t ciphertext_size) const {
  const Element h{_h_high, _h_low};
  const uint64_t blocks = ciphertext_size / 16 + (ciphertext_size % 16 ? 1 : 0);
  const Element associated = multiply(horner(from, h) ^ horner(to, h), power(h, blocks + 1));
  const Element lengths{(static_cast<uint64_t>(from.size()) * 8) ^ (static_cast<uint64_t>(to.size()) * 8), 0};
  return store(load(tag) ^ associated ^ multiply(lengths, h));
}

TagDeriver::Tag TagDeriver::hash_key() const noexcept { return store(Element{_h_high, _h_low}); }

}  // namespace dagda::migrate
