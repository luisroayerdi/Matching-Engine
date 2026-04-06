#pragma once
// Copyright (c) 2026 IEX Matching Engine Project
// Platform clock. All callers use these functions — never call OS clock APIs
// directly. macOS only.

#include <cstdint>

namespace iex::platform {

// Returns nanoseconds from an arbitrary epoch (boot time).
// Uses clock_gettime_nsec_np(CLOCK_UPTIME_RAW): monotonic, not NTP-adjusted.
[[nodiscard]] uint64_t now_ns() noexcept;

// Raw cycle counter for hot-path microbenchmarks only.
// ARM64: cntvct_el0 virtual counter (~24 MHz on Apple Silicon).
// x86-64: RDTSC.
// Never call __rdtsc() directly — it does not compile on ARM64.
[[nodiscard]] uint64_t rdtsc() noexcept;

} // namespace iex::platform
