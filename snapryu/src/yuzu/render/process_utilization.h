// SPDX-FileCopyrightText: Copyright 2026 SnapRYU contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace Frontend {

struct ProcessUtilizationSnapshot {
    // Negative means unavailable, warming up, or stale, not zero utilization.
    double cpu_percent = -1.0;
    double gpu_percent = -1.0;
    std::int64_t ram_bytes = -1;
};

// Process-lifetime worker: closing/hiding an overlay never joins a PDH call on the UI thread.
// Sampling sleeps while there are no visible overlays. All OS counters belong to the worker.
class ProcessUtilization {
public:
    static ProcessUtilization& Instance();
    void AddClient();
    void RemoveClient();
    [[nodiscard]] ProcessUtilizationSnapshot Read() const;

private:
    ProcessUtilization();
    ~ProcessUtilization();
    void Run(std::stop_token stop);

    std::atomic<int> clients{};
    std::atomic<double> cpu_percent{-1.0};
    std::atomic<double> gpu_percent{-1.0};
    std::atomic<std::int64_t> ram_bytes{-1};
    std::atomic<std::int64_t> sampled_at_ms{};
    std::mutex wait_mutex;
    std::condition_variable_any wake;
    // Last member: joined before synchronization primitives and published state are destroyed.
    std::jthread worker;
};

} // namespace Frontend
