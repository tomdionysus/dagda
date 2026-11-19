//
// Dagda - The Distributed Object Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <stdexcept>

namespace dagda::contract {

// Thrown by a long walk's pause callback when the maintenance pass is asked to
// stop (Macha's contract/stopping.hpp); it passes through the walk to the pass,
// which then sees the stop and ends. Nothing on the way treats it as the walk's
// own failure.
struct MaintenanceStopping final : std::runtime_error {
  MaintenanceStopping() : std::runtime_error("maintenance stopping") {}
};

}  // namespace dagda::contract
