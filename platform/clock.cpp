// Copyright (c) 2026 IEX Matching Engine Project

#include "platform/clock.hpp"

#include <time.h>  // clock_gettime_nsec_np, CLOCK_UPTIME_RAW

namespace iex::platform {

uint64_t now_ns() noexcept {
    // CLOCK_UPTIME_RAW: monotonic, not NTP-adjusted, no leap-second jumps.
    // clock_gettime_nsec_np is an Apple extension that returns uint64_t
    // directly, avoiding the two-field struct copy of clock_gettime().
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t rdtsc() noexcept {
#if defined(__aarch64__)
    // ARM64 virtual counter. Frequency is ~24 MHz on Apple Silicon (SoC-fixed,
    // not the CPU clock). Use only for relative intervals in microbenchmarks.
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#elif defined(__x86_64__)
    // RDTSC: not serializing w.r.t. out-of-order execution, but sufficient
    // for start/end bracketing in microbenchmarks.
    return __builtin_ia32_rdtsc();
#else
#  error "rdtsc(): unsupported CPU architecture"
#endif
}

} // namespace iex::platform
