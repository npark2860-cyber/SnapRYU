// SPDX-FileCopyrightText: Copyright 2026 SnapRYU contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "process_utilization.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#endif

namespace Frontend {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}

#ifdef _WIN32
bool ReadCpuTime(std::uint64_t& total) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return false;
    }
    const auto ticks = [](const FILETIME& time) {
        return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    total = ticks(kernel) + ticks(user);
    return true;
}

class GpuCounter {
public:
    GpuCounter() : pid_prefix(L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_") {}
    ~GpuCounter() {
        Close();
    }

    void Close() {
        if (query) {
            PdhCloseQuery(query);
        }
        query = nullptr;
        counter = nullptr;
    }

    bool Open() {
        Close();
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) !=
                ERROR_SUCCESS) {
            Close();
            return false;
        }
        // A rate needs two collections. Instance enumeration stays dynamic through the wildcard.
        PdhCollectQueryData(query);
        return true;
    }

    [[nodiscard]] bool IsOpen() const {
        return query != nullptr;
    }

    double Sample() {
        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return -1.0;
        }
        // The set of GPU engines can change between size and data calls. Retry from size zero.
        for (int attempt = 0; attempt < 2; ++attempt) {
            DWORD bytes = 0;
            DWORD count = 0;
            constexpr DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
            auto status = PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, nullptr);
            if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0 ||
                bytes > 16 * 1024 * 1024) {
                return -1.0;
            }
            // max_align_t storage supplies the alignment required by PDH's returned structures.
            storage.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
            status = PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, items);
            if (status == static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
                status == static_cast<PDH_STATUS>(PDH_INVALID_ARGUMENT)) {
                continue;
            }
            if (status != ERROR_SUCCESS) {
                return -1.0;
            }
            double busiest = -1.0;
            for (DWORD i = 0; i < count; ++i) {
                const auto& item = items[i];
                if (!item.szName || !std::wstring_view{item.szName}.starts_with(pid_prefix) ||
                    (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                     item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)) {
                    continue;
                }
                const double value = item.FmtValue.doubleValue;
                if (std::isfinite(value) && value >= 0.0) {
                    busiest = std::max(busiest, std::min(100.0, value));
                }
            }
            return busiest;
        }
        return -1.0;
    }

private:
    PDH_HQUERY query{};
    PDH_HCOUNTER counter{};
    std::wstring pid_prefix;
    std::vector<std::max_align_t> storage;
};
#endif
} // namespace

ProcessUtilization& ProcessUtilization::Instance() {
    static ProcessUtilization instance;
    return instance;
}

ProcessUtilization::ProcessUtilization()
    : worker([this](std::stop_token stop) { Run(stop); }) {}

ProcessUtilization::~ProcessUtilization() {
    worker.request_stop();
    wake.notify_all();
}

void ProcessUtilization::AddClient() {
    {
        // Couple the predicate change to wait(): otherwise a notification can be lost.
        // The worker never holds this mutex while querying Windows providers.
        std::lock_guard lock{wait_mutex};
        clients.fetch_add(1, std::memory_order_relaxed);
    }
    wake.notify_all();
}

void ProcessUtilization::RemoveClient() {
    {
        std::lock_guard lock{wait_mutex};
        clients.fetch_sub(1, std::memory_order_relaxed);
    }
    wake.notify_all();
}

ProcessUtilizationSnapshot ProcessUtilization::Read() const {
    const auto sampled = sampled_at_ms.load(std::memory_order_acquire);
    if (sampled == 0 || NowMs() - sampled > 3500) {
        return {};
    }
    return {cpu_percent.load(std::memory_order_relaxed),
            gpu_percent.load(std::memory_order_relaxed),
            ram_bytes.load(std::memory_order_relaxed)};
}

void ProcessUtilization::Run(std::stop_token stop) {
    try {
#ifdef _WIN32
        GpuCounter gpu;
        const auto processor_count = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#endif
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock{wait_mutex};
                wake.wait(lock, stop, [this] { return clients.load(std::memory_order_relaxed) > 0; });
            }
            if (stop.stop_requested()) {
                break;
            }
            sampled_at_ms.store(0, std::memory_order_release);
#ifdef _WIN32
            std::uint64_t previous_cpu = 0;
            bool previous_valid = ReadCpuTime(previous_cpu);
            auto previous_time = Clock::now();
            gpu.Open();
            auto retry_at = previous_time + 30s;
#endif
            while (!stop.stop_requested() && clients.load(std::memory_order_relaxed) > 0) {
                {
                    std::unique_lock lock{wait_mutex};
                    wake.wait_for(lock, stop, 1s,
                                  [this] { return clients.load(std::memory_order_relaxed) <= 0; });
                }
                if (stop.stop_requested() || clients.load(std::memory_order_relaxed) <= 0) {
                    break;
                }
                double cpu_value = -1.0;
                double gpu_value = -1.0;
                std::int64_t ram_value = -1;
#ifdef _WIN32
                PROCESS_MEMORY_COUNTERS_EX memory{};
                memory.cb = sizeof(memory);
                if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
                    ram_value = static_cast<std::int64_t>(memory.WorkingSetSize);
                }
                std::uint64_t current_cpu = 0;
                const bool current_valid = ReadCpuTime(current_cpu);
                const auto now = Clock::now();
                const double seconds = std::chrono::duration<double>(now - previous_time).count();
                if (current_valid && previous_valid && current_cpu >= previous_cpu && seconds > 0) {
                    // FILETIME is 100 ns. Normalize all process threads to total logical CPU capacity.
                    cpu_value = std::clamp((current_cpu - previous_cpu) * 1.0e-7 * 100.0 /
                                               (seconds * processor_count),
                                           0.0, 100.0);
                }
                previous_cpu = current_cpu;
                previous_valid = current_valid;
                previous_time = now;
                if (!gpu.IsOpen() && now >= retry_at) {
                    gpu.Open();
                    retry_at = now + 30s;
                } else {
                    gpu_value = gpu.Sample();
                }
#endif
                cpu_percent.store(cpu_value, std::memory_order_relaxed);
                gpu_percent.store(gpu_value, std::memory_order_relaxed);
                ram_bytes.store(ram_value, std::memory_order_relaxed);
                sampled_at_ms.store(NowMs(), std::memory_order_release);
            }
#ifdef _WIN32
            gpu.Close();
#endif
            sampled_at_ms.store(0, std::memory_order_release);
        }
    } catch (...) {
        // Optional UI telemetry must not terminate emulation if allocation/provider setup fails.
        sampled_at_ms.store(0, std::memory_order_release);
    }
}

} // namespace Frontend
