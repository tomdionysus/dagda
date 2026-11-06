//
// Dagda - The Store
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"

namespace dagda::store {

// How long a maintenance stage has taken, in real time whatever clock its pass
// runs on, as Macha measures it.
inline uint64_t stage_elapsed_us(dannan::contract::Clock::time_point started) {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(dannan::contract::Clock::now() - started).count());
}

// Macha's slow-stage line: a stage of 50 ms or more, at the log's most verbose level.
inline void log_slow_stage(const dannan::status::ScopedLogger& logger, std::string_view stage, dannan::contract::Clock::time_point started,
                           const std::string& detail = {}) {
  const auto elapsed_ms = stage_elapsed_us(started) / 1000;
  if (elapsed_ms < 50 || !logger.enabled(dannan::status::LogLevel::all)) return;
  logger.trace("DIAG maintenance-stage stage=" + std::string(stage) + " elapsed_ms=" + std::to_string(elapsed_ms) + (detail.empty() ? "" : " " + detail));
}

}  // namespace dagda::store
