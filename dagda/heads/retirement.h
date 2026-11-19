//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "dagda/contract/horizon.h"
#include "dagda/heads/record.h"
#include "dagda/heads/sealed_file.h"
#include "dagda/heads/tree.h"

// Retirement batches (requirement H23): one retiring mutation's objects, kept as one
// immutable CONTROL object the head names, dropped whole once its grace has run.
// Macha's tombstone batches at 9a08846, its magic a parameter: the magic, retired_at
// (i64be), a count (u32be) and the ids, sorted and unique. A head holds only the
// names, so it stays small however much is retired. Pure over the store it is given;
// thread-safe if that store is; waits as that store does.
namespace dagda::heads {

using contract::Garbage;
using contract::GarbageBatch;

struct RetirementContent {
  int64_t retired_at_ns = 0;
  std::vector<ObjectId> ids;
};

// Throws std::invalid_argument for no ids, a negative time, or ids not sorted and
// unique.
Bytes encode_retirement_batch(const Magic& magic, int64_t retired_at_ns, const std::vector<ObjectId>& ids);
// Throws DecodeError for anything else.
RetirementContent decode_retirement_batch(const Magic& magic, std::span<const uint8_t> bytes);

// Sorts and uniques `ids` and writes their batch through `nodes`; nothing when none
// are left.
std::optional<GarbageBatch> write_retirement_batch(TreeNodeStore& nodes, const Magic& magic, int64_t retired_at_ns, std::vector<ObjectId> ids);
// Into a list kept sorted by id; a batch already named is not named twice.
void insert_retirement_batch(std::vector<GarbageBatch>& batches, const GarbageBatch& batch);

// Every retirement `batches` hold, the latest per object, each stamped with its
// batch's id as its token so an equal one is never mistaken for another. A batch
// that cannot be read, or does not match its name, is counted in `unreadable` and
// left out. `pause` runs per batch and may throw contract::MaintenanceStopping.
std::vector<Garbage> retirements_of(std::span<const GarbageBatch> batches, const TreeNodeStore& store, const Magic& magic, size_t* unreadable = nullptr,
                                    const TreePause& pause = {});

// What erasing `erase` does to a head's batches: a batch holding one to erase is
// dropped and the rest of it written again at its own time; a later retirement of
// the same object is another batch and is kept, as is one that cannot be read.
struct RetirementRewrite {
  std::vector<GarbageBatch> kept;
  std::vector<ObjectId> dropped;
  std::vector<RetirementContent> rest;
};
RetirementRewrite rewrite_retirements(std::span<const GarbageBatch> batches, const TreeNodeStore& store, const Magic& magic, std::span<const Garbage> erase);

}  // namespace dagda::heads
