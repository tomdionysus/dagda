//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Moved from Macha's tests/test_presence.cpp (develop 5a06d3c): the two
// PresenceIndex cases. The store and pool cases there move with LocalStore
// and StoragePool.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

#include "dagda/store/presence_index.h"

namespace dagda::store {
namespace {

struct ModelId {
  bool present = false;
  bool pruned = false;
  bool forgotten = false;
};

struct Model {
  std::array<ModelId, 2> ids{};
  bool authoritative = false;

  PresenceIndex::Answer answer(size_t index) const {
    if (ids[index].present) return PresenceIndex::Answer::present;
    return authoritative ? PresenceIndex::Answer::absent : PresenceIndex::Answer::unknown;
  }
};

enum class Op { installed, observed, forgotten, pruned, listed_one, listed_both, warmed };
constexpr std::array ops{Op::installed, Op::observed, Op::forgotten, Op::pruned, Op::listed_one, Op::listed_both, Op::warmed};

// Applies one event to both the index and the model, the model written from
// the table in presence_index.h rather than from the implementation.
void apply(PresenceIndex& index, Model& model, Op op, size_t target, const std::array<ObjectId, 2>& ids) {
  auto& id = model.ids[target];
  switch (op) {
    case Op::installed:
      index.installed(ids[target]);
      id.present = true;
      id.pruned = false;
      break;
    case Op::observed:
      index.observed(ids[target]);
      id.present = true;
      break;
    case Op::forgotten:
      index.forgotten(ids[target]);
      id.present = false;
      if (!model.authoritative) id.forgotten = true;
      break;
    case Op::pruned:
      index.pruned(ids[target]);
      id.present = false;
      id.pruned = true;
      if (!model.authoritative) id.forgotten = true;
      break;
    case Op::listed_one: {
      const std::array one{ids[target]};
      index.listed(one);
      if (!id.pruned && !id.forgotten) id.present = true;
      break;
    }
    case Op::listed_both:
      index.listed(ids);
      for (auto& each : model.ids)
        if (!each.pruned && !each.forgotten) each.present = true;
      break;
    case Op::warmed:
      index.warmed();
      model.authoritative = true;
      for (auto& each : model.ids) each.forgotten = false;
      break;
  }
}

// Macha: test_presence_index_matches_its_model_over_every_short_history.
TEST(StorePresenceIndex, MatchesItsModelOverEveryShortHistory) {
  std::array<ObjectId, 2> ids{};
  ids[0].bytes[0] = 1;
  ids[1].bytes[0] = 2;
  // Every event, on either id, in every order, up to five events long:
  // (7 x 2)^5 histories, each checked after every event.
  constexpr size_t choices = ops.size() * 2;
  constexpr size_t length = 5;
  size_t histories = 1;
  for (size_t i = 0; i < length; ++i) histories *= choices;
  size_t checked = 0;
  for (size_t history = 0; history < histories; ++history) {
    PresenceIndex index;
    Model model;
    auto code = history;
    for (size_t step = 0; step < length; ++step) {
      const auto choice = code % choices;
      code /= choices;
      apply(index, model, ops[choice / 2], choice % 2, ids);
      for (size_t target = 0; target < 2; ++target) ASSERT_EQ(index.answer(ids[target]), model.answer(target));
      ASSERT_EQ(index.authoritative(), model.authoritative);
      ASSERT_EQ(index.size(), static_cast<size_t>(model.ids[0].present) + model.ids[1].present);
      ++checked;
    }
  }
  EXPECT_EQ(checked, histories * length);
}

// Macha: test_presence_index_warmup_race_cases_by_name.
TEST(StorePresenceIndex, WarmupRaceCasesByName) {
  ObjectId removed{};
  removed.bytes[0] = 7;
  const std::array listing{removed};

  // Removed after warm-up listed it and before the listing was published.
  PresenceIndex index;
  index.observed(removed);
  index.forgotten(removed);
  index.listed(listing);
  EXPECT_EQ(index.answer(removed), PresenceIndex::Answer::unknown);
  index.warmed();
  EXPECT_EQ(index.answer(removed), PresenceIndex::Answer::absent);

  // Put again after that removal: present, whatever warm-up listed.
  index.installed(removed);
  EXPECT_EQ(index.answer(removed), PresenceIndex::Answer::present);

  // Once authoritative, a later removal needs no remembering, and a stale
  // listing (there is none after warm-up, but if there were) cannot
  // republish a pruned object.
  PresenceIndex pruned;
  pruned.warmed();
  pruned.pruned(removed);
  pruned.listed(listing);
  EXPECT_EQ(pruned.answer(removed), PresenceIndex::Answer::absent);
}

// What seeds the ledger is every id known present, in id order.
TEST(StorePresenceIndex, PresentListsWhatIsKnownInIdOrder) {
  ObjectId low{};
  low.bytes[0] = 1;
  ObjectId high{};
  high.bytes[0] = 9;
  PresenceIndex index;
  index.observed(high);
  index.installed(low);
  ASSERT_EQ(index.present().size(), 2u);
  EXPECT_EQ(index.present()[0], low);
  EXPECT_EQ(index.present()[1], high);
}

}  // namespace
}  // namespace dagda::store
