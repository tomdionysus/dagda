//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "dagda/store/sealing.h"

// The state directory's layout marker, Dagda's because it names the store's
// layout: `<state>/storage-layout` holding "dagda-state-layout-v19 <domain>\n".
// Macha's v18 marker ("macha-state-layout-v18") is recognised only to be
// refused: the migration is manual (Tom, 2026-11-04 night), by dagda-migrate.
namespace dagda::store {

inline constexpr std::string_view state_layout_v19 = "dagda-state-layout-v19";

// The marker's text for a domain, without its line end.
std::string state_layout_marker(const SealingScheme& sealing);

// The reason a v18 state or backend is refused: it names the command that
// migrates it and the state path.
std::string migrate_first(const std::filesystem::path& state);

// Accepts the state directory for the scheme's domain. A marked directory
// must name v19 and that domain; an unmarked one must be empty but for the
// storage lock's `.macha.lock`, and is then marked. A v18 state is refused
// with a reason naming dagda-migrate; anything else is refused too. Nothing
// is changed by a refusal. A missing directory is created first. Call it
// holding the state directory's StorageLock. Waits on the state device.
void open_state_layout(const std::filesystem::path& state, const SealingScheme& sealing);

}  // namespace dagda::store
