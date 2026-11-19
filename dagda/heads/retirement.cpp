//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/heads/retirement.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

#include "dagda/contract/stopping.h"

namespace dagda::heads {

using dannan::net::Reader;
using dannan::net::Writer;

Bytes encode_retirement_batch(const Magic& magic, int64_t retired_at_ns, const std::vector<ObjectId>& ids) {
  if (ids.empty() || retired_at_ns < 0) throw std::invalid_argument("empty or unretired tombstone batch");
  for (size_t i = 1; i < ids.size(); ++i)
    if (!(ids[i - 1] < ids[i])) throw std::invalid_argument("tombstone batch ids not sorted and unique");
  Writer out;
  out.raw(magic);
  out.i64(retired_at_ns);
  out.u32(static_cast<uint32_t>(ids.size()));
  for (const auto& id : ids) out.fixed(id.bytes);
  return out.take();
}

RetirementContent decode_retirement_batch(const Magic& magic, std::span<const uint8_t> bytes) {
  Reader in(bytes);
  const auto found = in.view(magic.size());
  if (!std::equal(found.begin(), found.end(), magic.begin())) throw DecodeError("not a tombstone batch");
  RetirementContent batch;
  batch.retired_at_ns = in.i64();
  const auto count = in.u32();
  if (batch.retired_at_ns < 0 || count == 0 || count > in.remaining() / 32) throw DecodeError("bad tombstone batch");
  batch.ids.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const ObjectId id{in.fixed<32>()};
    if (!batch.ids.empty() && !(batch.ids.back() < id)) throw DecodeError("tombstone batch ids out of order");
    batch.ids.push_back(id);
  }
  in.finish();
  return batch;
}

std::optional<GarbageBatch> write_retirement_batch(TreeNodeStore& nodes, const Magic& magic, int64_t retired_at_ns, std::vector<ObjectId> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (ids.empty()) return std::nullopt;
  GarbageBatch batch;
  batch.id = nodes.put(encode_retirement_batch(magic, retired_at_ns, ids));
  batch.retired_at_ns = retired_at_ns;
  batch.count = static_cast<uint32_t>(ids.size());
  return batch;
}

void insert_retirement_batch(std::vector<GarbageBatch>& batches, const GarbageBatch& batch) {
  const auto at = std::lower_bound(batches.begin(), batches.end(), batch.id, [](const GarbageBatch& held, const ObjectId& id) { return held.id < id; });
  if (at == batches.end() || at->id != batch.id) batches.insert(at, batch);
}

std::vector<Garbage> retirements_of(std::span<const GarbageBatch> batches, const TreeNodeStore& store, const Magic& magic, size_t* unreadable,
                                    const TreePause& pause) {
  std::vector<Garbage> all;
  for (const auto& batch : batches) {
    if (pause) pause();
    std::optional<RetirementContent> content;
    try {
      if (const auto bytes = store.get(batch.id)) content = decode_retirement_batch(magic, *bytes);
    } catch (const contract::MaintenanceStopping&) {
      throw;
    } catch (const std::exception&) {
    }
    if (!content || content->ids.size() != batch.count || content->retired_at_ns != batch.retired_at_ns) {
      if (unreadable) ++*unreadable;
      continue;
    }
    NodeId token;
    std::copy_n(batch.id.bytes.begin(), token.bytes.size(), token.bytes.begin());
    for (const auto& id : content->ids) all.push_back({id, batch.retired_at_ns, token});
  }
  // The latest retirement per object.
  std::sort(all.begin(), all.end(), [](const Garbage& a, const Garbage& b) {
    if (a.id != b.id) return a.id < b.id;
    return std::tie(a.retired_at_ns, a.token) > std::tie(b.retired_at_ns, b.token);
  });
  all.erase(std::unique(all.begin(), all.end(), [](const Garbage& a, const Garbage& b) { return a.id == b.id; }), all.end());
  return all;
}

RetirementRewrite rewrite_retirements(std::span<const GarbageBatch> batches, const TreeNodeStore& store, const Magic& magic, std::span<const Garbage> erase) {
  std::map<ObjectId, Garbage> expected;
  for (const auto& candidate : erase) expected[candidate.id] = candidate;
  RetirementRewrite out;
  for (const auto& batch : batches) {
    const auto bytes = store.get(batch.id);
    if (!bytes) {
      out.kept.push_back(batch);
      continue;
    }
    auto content = decode_retirement_batch(magic, *bytes);
    RetirementContent rest{content.retired_at_ns, {}};
    for (const auto& id : content.ids) {
      const auto found = expected.find(id);
      if (found == expected.end() || content.retired_at_ns > found->second.retired_at_ns) rest.ids.push_back(id);
    }
    if (rest.ids.size() == content.ids.size()) {
      out.kept.push_back(batch);
      continue;
    }
    out.dropped.push_back(batch.id);
    if (!rest.ids.empty()) out.rest.push_back(std::move(rest));
  }
  std::sort(out.dropped.begin(), out.dropped.end());
  return out;
}

}  // namespace dagda::heads
