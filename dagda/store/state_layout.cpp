//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/store/state_layout.h"

#include <fstream>
#include <stdexcept>
#include <string>

#include "dannan/identity/durable_file.h"

namespace dagda::store {
namespace {

std::string marker_of(const SealingScheme& sealing) {
  if (sealing.layout == 18) return std::string(state_layout_v18);
  return std::string(state_layout_v19) + " " + domain::to_string(sealing.domain);
}

}  // namespace

void open_state_layout(const std::filesystem::path& state) { open_state_layout(state, SealingScheme::v18()); }

void open_state_layout(const std::filesystem::path& state, const SealingScheme& sealing) {
  if (sealing.layout != 18 && sealing.layout != 19) throw std::invalid_argument("state layout must be 18 or 19");
  std::filesystem::create_directories(state);
  const auto marker = state / "storage-layout";
  const auto expected = marker_of(sealing);
  if (std::filesystem::exists(marker)) {
    std::ifstream input(marker);
    std::string value;
    std::getline(input, value);
    if (!input && value.empty()) throw std::runtime_error("cannot read storage layout marker");
    if (value == expected) return;
    if (value == state_layout_v18)
      throw std::runtime_error("state is layout v18, domain v1's (Macha's); this store is domain " + domain::to_string(sealing.domain));
    if (value.starts_with(state_layout_v19)) throw std::runtime_error("state is layout v19 of another domain (" + value + "); this store is " + expected);
    throw std::runtime_error("incompatible Macha storage layout; 0.18 requires a fresh namespace");
  }
  // No migration path: an older layout is refused, not reinterpreted.
  for (const auto& entry : std::filesystem::directory_iterator(state)) {
    if (entry.path().filename() == ".macha.lock") continue;
    throw std::runtime_error("existing unversioned Macha state detected; 0.18 requires a fresh namespace");
  }
  dannan::identity::durable_replace_file(marker, expected + "\n");
}

}  // namespace dagda::store
