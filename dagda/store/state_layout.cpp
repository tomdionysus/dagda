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

void open_state_layout(const std::filesystem::path& state) {
  std::filesystem::create_directories(state);
  const auto marker = state / "storage-layout";
  if (std::filesystem::exists(marker)) {
    std::ifstream input(marker);
    std::string value;
    std::getline(input, value);
    if (!input && value.empty()) throw std::runtime_error("cannot read storage layout marker");
    if (value != state_layout_v18) throw std::runtime_error("incompatible Macha storage layout; 0.18 requires a fresh namespace");
    return;
  }
  // No migration path: an older layout is refused, not reinterpreted.
  for (const auto& entry : std::filesystem::directory_iterator(state)) {
    if (entry.path().filename() == ".macha.lock") continue;
    throw std::runtime_error("existing unversioned Macha state detected; 0.18 requires a fresh namespace");
  }
  dannan::identity::durable_replace_file(marker, std::string(state_layout_v18) + "\n");
}

}  // namespace dagda::store
