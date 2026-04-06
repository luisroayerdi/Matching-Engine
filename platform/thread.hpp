#pragma once
// Copyright (c) 2026 IEX Matching Engine Project
// Thread affinity and priority. macOS only.
// Both functions are intentional no-ops: XNU does not expose pthread_setaffinity_np
// or user-space SCHED_FIFO. Benchmark variance is higher as a result — this is
// documented and expected. See DESIGN.md §1.

namespace iex::platform {

// No-op on macOS. pthread_setaffinity_np does not exist on XNU.
void pin_thread_to_core(int core_id);

// No-op on macOS. Logs a warning. SCHED_FIFO is restricted to kernel/audio threads.
void set_realtime_priority(int priority);

} // namespace iex::platform
