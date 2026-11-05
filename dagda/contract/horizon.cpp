//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/contract/horizon.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "dagda/ledger/trie_diff.h"

namespace dagda::contract {
namespace {

std::vector<ObjectId> sorted_unique(std::vector<ObjectId> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

}  // namespace

ReferencedSets::Set ReferencedSets::make(std::optional<ledger::ObjectTrie::Snapshot> view, std::vector<ObjectId> others) {
  Set set;
  set.others = sorted_unique(std::move(others));
  set.size = set.others.size();
  if (view) {
    for (const auto& id : set.others)
      if (view->get(id)) --set.size;
    set.size += view->size();
  }
  set.view = std::move(view);
  return set;
}

ReferencedSets::ReferencedSets(std::vector<ObjectId> data, std::vector<ObjectId> control)
    : _data(make(std::nullopt, std::move(data))), _control(make(std::nullopt, std::move(control))) {}

ReferencedSets::ReferencedSets(Views views, std::vector<ObjectId> data, std::vector<ObjectId> control)
    : _data(make(std::move(views.data), std::move(data))), _control(make(std::move(views.control), std::move(control))) {}

const ReferencedSets::Set& ReferencedSets::of(RetentionClass type) const { return type == RetentionClass::data ? _data : _control; }

bool ReferencedSets::referenced(RetentionClass type, const ObjectId& id) const {
  const auto& set = of(type);
  return std::binary_search(set.others.begin(), set.others.end(), id) || (set.view && set.view->get(id).has_value());
}

IdLookup ReferencedSets::lookup(RetentionClass type) const {
  const auto& set = of(type);
  if (!set.view) return IdLookup(std::span<const ObjectId>(set.others));
  return IdLookup([this, type](const ObjectId& id) { return referenced(type, id); });
}

std::vector<ObjectId> ReferencedSets::next(RetentionClass type, const std::optional<ObjectId>& after, size_t limit) const {
  const auto& set = of(type);
  std::vector<ObjectId> viewed;
  if (set.view)
    for (auto& record : set.view->next(after, limit)) viewed.push_back(record.first);
  // A full page of the view says nothing past its last id.
  const bool bounded = viewed.size() == limit;
  auto from = after ? std::upper_bound(set.others.begin(), set.others.end(), *after) : set.others.begin();
  auto to = bounded ? std::upper_bound(from, set.others.end(), viewed.back()) : set.others.end();
  if (static_cast<size_t>(to - from) > limit) to = from + static_cast<std::ptrdiff_t>(limit);
  std::vector<ObjectId> out;
  out.reserve(viewed.size() + static_cast<size_t>(to - from));
  std::set_union(viewed.begin(), viewed.end(), from, to, std::back_inserter(out));
  if (out.size() > limit) out.resize(limit);
  return out;
}

std::vector<ObjectId> ReferencedSets::all(RetentionClass type) const {
  std::vector<ObjectId> out;
  std::optional<ObjectId> after;
  while (true) {
    const auto page = next(type, after, 4096);
    out.insert(out.end(), page.begin(), page.end());
    if (page.size() < 4096) return out;
    after = page.back();
  }
}

std::vector<ObjectId> ReferencedSets::not_held(RetentionClass type, std::span<const ledger::ObjectTrie::Snapshot> held,
                                               const std::function<void()>& pause) const {
  const auto& set = of(type);
  std::vector<ObjectId> viewed;
  if (set.view) ledger::records_not_held(*set.view, held, [&](const ledger::ObjectTrie::Record& record) { viewed.push_back(record.first); }, pause);
  std::vector<ObjectId> others;
  size_t seen = 0;
  for (const auto& id : set.others) {
    if (pause && ++seen % 4096 == 0) pause();
    if (std::none_of(held.begin(), held.end(), [&](const ledger::ObjectTrie::Snapshot& part) { return part.get(id).has_value(); })) others.push_back(id);
  }
  std::vector<ObjectId> out;
  out.reserve(viewed.size() + others.size());
  std::set_union(viewed.begin(), viewed.end(), others.begin(), others.end(), std::back_inserter(out));
  return out;
}

size_t ReferencedSets::size(RetentionClass type) const { return of(type).size; }

InventoryHorizon::InventoryHorizon(uint64_t generation, bool complete, std::vector<ObjectId> data, std::vector<ObjectId> control,
                                   const std::vector<Garbage>& garbage)
    : InventoryHorizon(generation, complete, ReferencedSets(std::move(data), std::move(control)), garbage) {}

InventoryHorizon::InventoryHorizon(uint64_t generation, bool complete, ReferencedSets sets, const std::vector<Garbage>& garbage,
                                   std::vector<GarbageBatch> batches)
    : ReferencedSets(std::move(sets)), _generation(generation), _complete(complete), _batches(std::move(batches)) {
  _garbage.reserve(garbage.size());
  _stale_garbage.reserve(garbage.size());
  for (const auto& candidate : garbage) {
    if (referenced(RetentionClass::data, candidate.id))
      _stale_garbage.push_back(candidate);
    else
      _garbage.push_back(candidate);
  }
}

ReleaseHorizon::ReleaseHorizon(Hash256 head, RetentionClock clock, std::vector<ObjectId> data, std::vector<ObjectId> control)
    : ReferencedSets(std::move(data), std::move(control)), _head(head), _clock(std::move(clock)) {}

ReleaseHorizon::ReleaseHorizon(Hash256 head, RetentionClock clock, ReferencedSets sets)
    : ReferencedSets(std::move(sets)), _head(head), _clock(std::move(clock)) {}

}  // namespace dagda::contract
