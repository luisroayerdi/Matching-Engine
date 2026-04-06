// Copyright (c) 2026 IEX Matching Engine Project

#include "platform/thread.hpp"

#include <cstdio>

namespace iex::platform {

void pin_thread_to_core(int core_id) {
    // pthread_setaffinity_np does not exist on XNU. Thread affinity hints via
    // thread_policy_set are deprecated and have no effect on Apple Silicon.
    (void)core_id;
}

void set_realtime_priority(int priority) {
    // SCHED_FIFO for user threads is restricted to kernel/audio contexts on XNU.
    // Log once so the absence of real-time scheduling is visible in benchmark runs.
    std::fprintf(stderr,
        "[iex::platform] set_realtime_priority(%d): no-op on macOS — "
        "SCHED_FIFO is not available to user processes.\n", priority);
}

} // namespace iex::platform
