//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <filesystem>
#include <string_view>

#include "dagda/store/sealing.h"

// The state directory's layout marker (layout v18, requirements 4.1), Dagda's
// because it names the store's layout: `<state>/storage-layout` holding
// "macha-state-layout-v18\n", byte for byte what Macha writes
// (cluster/node_identity.cpp:15-40). A state directory of any other domain is
// layout v19 (the maintainer, 2026-11-04 evening): "dagda-state-layout-v19 <domain>\n".
namespace dagda::store {

inline constexpr std::string_view state_layout_v18 = "macha-state-layout-v18";
inline constexpr std::string_view state_layout_v19 = "dagda-state-layout-v19";

// Accepts the state directory for layout v18. A marked directory must name
// v18; an unmarked one must be empty but for the storage lock's `.macha.lock`,
// and is then marked. There is no migration from an older layout: anything
// else throws std::runtime_error and changes nothing. A missing directory is
// created first (Macha's root had made it). Call it holding the state
// directory's StorageLock. Waits on the state device.
void open_state_layout(const std::filesystem::path& state);
// As above for the scheme's layout: v18 is the form above, v19 names the
// domain, and a directory of the other layout, or of another domain, is
// refused.
void open_state_layout(const std::filesystem::path& state, const SealingScheme& sealing);

}  // namespace dagda::store
