//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/state_layout.h"

#include <array>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "dagda/store/local_store.h"
#include "dannan/identity/durable_file.h"

namespace dagda::store {

namespace {

constexpr const char* not_dagda_v19 = "not a Dagda v19 state";

// What a store keeps in its state directory, as a file or a directory: Dagda's own (its
// ledgers, claims, CONTROL objects, backend identities, maintenance figures, heads) and
// what an older store kept there (Macha's metadata, its survey, its repair position, an
// older namespace's file). Anything else is its host's: for Macha the state directory is
// the node's one, Dannan's too. A name Dagda's state gains joins this list.
constexpr std::array<std::string_view, 10> store_names{"ledger",   "retention",    "metadata-objects", "backend-identities", "maintenance", "heads",
                                                       "metadata", "availability", "repair",           "metadata.bin"};

}  // namespace

std::string state_layout_marker(const SealingScheme& sealing) { return std::string(state_layout_v19) + " " + domain::to_string(sealing.domain); }

std::string migrate_first(const std::filesystem::path& state) {
  return "layout v18 (Macha's) is not opened by the store: migrate it in place first with `dagda-migrate --state " + state.string() + " ...`";
}

void open_state_layout(const std::filesystem::path& state, const SealingScheme& sealing) {
  std::filesystem::create_directories(state);
  const auto marker = state / "storage-layout";
  const auto expected = state_layout_marker(sealing);
  if (std::filesystem::exists(marker)) {
    std::ifstream input(marker);
    std::string value;
    std::getline(input, value);
    if (!input && value.empty()) throw std::runtime_error("cannot read storage layout marker");
    if (value == expected) return;
    if (value == "macha-state-layout-v18") throw std::runtime_error("state " + state.string() + " is " + migrate_first(state));
    if (value == state_layout_going_forward)
      throw std::runtime_error("state " + state.string() + " stopped part way to layout v19: finish it with `dagda-migrate --state " + state.string() +
                               " ...`");
    if (value == state_layout_going_forward_heads)
      throw std::runtime_error("state " + state.string() + " stopped part way through moving its metadata: finish it with `dagda-migrate --heads --state " +
                               state.string() + " ...`");
    if (value == state_layout_going_back)
      throw std::runtime_error("state " + state.string() + " stopped on its way back to layout v18: finish it with `dagda-migrate --reverse --state " +
                               state.string() + " ...`");
    if (value.starts_with(state_layout_v19)) throw std::runtime_error("state is layout v19 of another domain (" + value + "); this store is " + expected);
    throw std::runtime_error("state " + state.string() + " is marked \"" + value + "\", which is " + not_dagda_v19 +
                             ": the store opens layout v19 alone, dagda-migrate moves Macha's v18 to it, and any other layout needs an empty state directory");
  }
  // Unmarked: never read as v19. A store is begun where no store has been: beside its
  // host's files, never over a store's.
  for (const auto name : store_names) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(state / name, error);
    if (error && error != std::errc::no_such_file_or_directory) throw std::filesystem::filesystem_error("cannot look at the state", state / name, error);
    if (!std::filesystem::exists(status)) continue;
    throw std::runtime_error("state " + state.string() + " holds a store's " + std::string(name) + " and no storage-layout marker, so it is " + not_dagda_v19 +
                             ": the store begins only in an empty state directory, or one holding nothing but its host's files");
  }
  dannan::identity::durable_replace_file(marker, expected + "\n");
}

}  // namespace dagda::store
