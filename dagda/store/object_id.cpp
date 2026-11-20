//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/object_id.h"

#include "dannan/encoding/base.h"

namespace dagda::store {

ObjectId object_id(std::span<const uint8_t> plaintext) { return dannan::identity::sha256(plaintext); }

std::string to_string(const ObjectId& id) { return dannan::encoding::hex(id.bytes); }

}  // namespace dagda::store
