//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <string_view>

#include "dagda/store/object_id.h"

namespace dagda::store {
namespace {

Bytes bytes_of(std::string_view text) { return Bytes(text.begin(), text.end()); }

// Requirement S1: an object is named by the SHA-256 of its plaintext (FIPS
// 180-2's "abc" and the empty message).
TEST(StoreObjectId, IsTheSha256OfThePlaintext) {
  EXPECT_EQ(to_string(object_id(bytes_of("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(to_string(object_id({})), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// Ids order as Macha's do, byte by byte from the first: the order of sets,
// listings and the trie.
TEST(StoreObjectId, OrdersByItsBytesFromTheFirst) {
  ObjectId a{};
  ObjectId b{};
  a.bytes[0] = 1;
  b.bytes[31] = 0xff;
  EXPECT_LT(b, a);
  b.bytes[0] = 1;
  EXPECT_LT(a, b);
}

}  // namespace
}  // namespace dagda::store
