#pragma once

#include <cstdint>

namespace pbc {

// Version string of the core library (git describe, or 0.0.0 outside a checkout).
const char* version();

// The earliest instant a reader's clock can plausibly show (2026-10-01 00:00:00 UTC). A fixed floor, not the build time, so that builds stay
// reproducible. Raise it at a release when convenient; a clock before it is reported as wrong before any connection is made.
inline constexpr int64_t kClockFloorEpoch = 1790812800;

}  // namespace pbc
