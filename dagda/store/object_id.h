//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dannan/identity/crypto.h"

namespace dagda::store {

using Bytes = dannan::identity::Bytes;

// An object's name: the SHA-256 of its plaintext (requirement S1), Dannan's
// Hash256 byte for byte, so ids, their order and their hex are Macha's.
using ObjectId = dannan::identity::Hash256;

// Waits on nothing; thread-safe.
ObjectId object_id(std::span<const uint8_t> plaintext);

// Sixty-four lowercase hex digits.
std::string to_string(const ObjectId& id);

}  // namespace dagda::store
