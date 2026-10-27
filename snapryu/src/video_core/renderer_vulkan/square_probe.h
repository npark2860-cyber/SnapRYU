// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// b62 SQUARE_PROBE (EDEN_SQUARE_PROBE=1, diagnostic builds only).
//
// The b61 capture frames had no terrain squares in the G-buffer normal although the squares were
// on screen when F3 was pressed. Around the terrain draws those frames changed three things: every
// terrain draw had a render pass of its own (with the barriers of its copies), the GPU was waited
// for before and between the terrain draws (spills), and the GPU thread stalled for seconds.
// One key per change switches it on or off for every frame from then on, so the squares can be
// watched while it changes:
//   F1 BARRIER  every terrain draw gets a render pass of its own between two full memory barriers
//               (ALL_COMMANDS, MEMORY_WRITE -> MEMORY_READ | MEMORY_WRITE). GPU ordering only.
//   F3 WAIT     the first terrain draw of a run (a terrain draw right after a non-terrain draw)
//               first ends the render pass and waits for the GPU (Scheduler::Finish), as a b61
//               spill does.
//   F12 SLEEP   the first terrain draw of a run first sleeps EDEN_SQUARE_PROBE_SLEEP_MS (default
//               100, 1..2000) on the GPU thread. No GPU change, only time for the CPU side.
// Terrain draws are the draws whose fragment shader is listed in EDEN_SQUARE_PROBE_FS (default:
// the five terrain shaders that wrote the squares in b60). Nothing changes before a key is
// pressed. Every press logs the new state and, for the period since the previous press, the
// frames, terrain draws, runs, barriers, waits and sleeps (SQUARE_PROBE, Render_Vulkan warning);
// the renderer's shutdown logs the last period.
// The keys are read with GetAsyncKeyState, also when another window has the focus.

#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) short
#if !defined(_M_ARM64) && !defined(__aarch64__)
    __stdcall
#endif
    GetAsyncKeyState(int vKey);
#endif

namespace Vulkan::SquareProbe {

// The emulator binds F2, F4 to F6 and F8 to F11 (qt-config), and the b51 SQUARES_MARK check
// (alias_trace.h) reads F7 in every diagnostic build; F1, F3 and F12 are free.
inline constexpr int BARRIER_KEY = 0x70; // VK_F1
inline constexpr int WAIT_KEY = 0x72;    // VK_F3
inline constexpr int SLEEP_KEY = 0x7B;   // VK_F12
inline constexpr u32 MAX_SLEEP_MS = 2000;
inline constexpr const char* DEFAULT_TERRAIN_FS =
    "f4203d0bb4614acf,dbfaefab36a351a8,65230e5813102d6f,1762a3e7f922b76e,56ae4688574e6cba";

struct Config {
    bool enabled = false;
    u32 sleep_ms = 100;
    std::unordered_set<u64> terrain;
    std::string terrain_text;
};

inline const Config& GetConfig() {
    static const Config config = [] {
        Config result;
        const char* const on = std::getenv("EDEN_SQUARE_PROBE");
        result.enabled = on != nullptr && on[0] == '1' && on[1] == '\0';
        const char* const sleep = std::getenv("EDEN_SQUARE_PROBE_SLEEP_MS");
        if (sleep != nullptr && sleep[0] != '\0') {
            char* end{};
            const unsigned long long parsed = std::strtoull(sleep, &end, 10);
            if (end != sleep && *end == '\0') {
                result.sleep_ms =
                    static_cast<u32>(std::clamp<unsigned long long>(parsed, 1, MAX_SLEEP_MS));
            }
        }
        const char* const fs = std::getenv("EDEN_SQUARE_PROBE_FS");
        const std::string list = fs != nullptr && fs[0] != '\0' ? fs : DEFAULT_TERRAIN_FS;
        size_t begin = 0;
        while (begin <= list.size()) {
            const size_t comma = std::min(list.find(',', begin), list.size());
            const std::string item = list.substr(begin, comma - begin);
            char* end{};
            const unsigned long long hash = std::strtoull(item.c_str(), &end, 16);
            if (!item.empty() && *end == '\0' && result.terrain.insert(hash).second) {
                result.terrain_text +=
                    fmt::format("{}{:016x}", result.terrain_text.empty() ? "" : ",", hash);
            }
            begin = comma + 1;
        }
        if (result.terrain_text.empty()) {
            result.terrain_text = "none";
        }
        return result;
    }();
    return config;
}

/// EDEN_SQUARE_PROBE=1, read once.
inline bool Enabled() {
    static const bool enabled = GetConfig().enabled;
    return enabled;
}

struct Counts {
    std::atomic<u64> frames{};
    std::atomic<u64> terrain_draws{};
    std::atomic<u64> runs{};
    std::atomic<u64> barriers{};
    std::atomic<u64> waits{};
    std::atomic<u64> wait_us{};
    std::atomic<u64> sleeps{};
    std::atomic<u64> sleep_us{};
};

struct State {
    std::atomic<bool> barrier{false};
    std::atomic<bool> wait{false};
    std::atomic<bool> sleep{false};
    std::atomic<bool> previous_terrain{false};
    Counts counts;
    std::array<bool, 3> keys_down{};
    bool announced = false;
    u64 presses = 0;
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point period_start{};
};

inline State& GetState() {
    static State state;
    return state;
}

inline bool IsTerrain(u64 fragment_hash) {
    return GetConfig().terrain.contains(fragment_hash);
}

inline u64 MicrosecondsSince(std::chrono::steady_clock::time_point begin) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count());
}

/// RasterizerVulkan::PrepareDraw, before the buffer and texture cache locks (where b61 spills):
/// counts terrain draws; at the first terrain draw of a run, WAIT (F3) and SLEEP (F12) act.
inline void BeforeLock(Scheduler& scheduler, u64 fragment_hash) {
    State& state = GetState();
    const bool terrain = IsTerrain(fragment_hash);
    const bool was_terrain = state.previous_terrain.exchange(terrain, std::memory_order_relaxed);
    if (!terrain) {
        return;
    }
    state.counts.terrain_draws.fetch_add(1, std::memory_order_relaxed);
    if (was_terrain) {
        return;
    }
    state.counts.runs.fetch_add(1, std::memory_order_relaxed);
    if (state.wait.load(std::memory_order_relaxed)) {
        const auto begin = std::chrono::steady_clock::now();
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Finish();
        state.counts.waits.fetch_add(1, std::memory_order_relaxed);
        state.counts.wait_us.fetch_add(MicrosecondsSince(begin), std::memory_order_relaxed);
    }
    if (state.sleep.load(std::memory_order_relaxed)) {
        const auto begin = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(GetConfig().sleep_ms));
        state.counts.sleeps.fetch_add(1, std::memory_order_relaxed);
        state.counts.sleep_us.fetch_add(MicrosecondsSince(begin), std::memory_order_relaxed);
    }
}

/// RasterizerVulkan::PrepareDraw, after the pipeline is configured: with BARRIER (F1) on, a terrain
/// draw gets a render pass of its own. True when BarrierAfter must follow the draw.
inline bool BarrierBefore(Scheduler& scheduler, const Framebuffer* framebuffer, u64 fragment_hash) {
    State& state = GetState();
    if (!state.barrier.load(std::memory_order_relaxed) || !IsTerrain(fragment_hash)) {
        return false;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.RecordFullBarrierNow();
    scheduler.RequestRenderpass(framebuffer);
    state.counts.barriers.fetch_add(1, std::memory_order_relaxed);
    return true;
}

/// RasterizerVulkan::PrepareDraw, right after a draw that BarrierBefore isolated.
inline void BarrierAfter(Scheduler& scheduler) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.RecordFullBarrierNow();
}

inline bool KeyDown(int key) {
#ifdef _WIN32
    return (GetAsyncKeyState(key) & 0x8000) != 0;
#elif defined(SNAPRYU_SQUARE_PROBE_MOCK_KEYS)
    return SNAPRYU_SQUARE_PROBE_MOCK_KEYS(key);
#else
    static_cast<void>(key);
    return false;
#endif
}

inline const char* OnOff(bool value) {
    return value ? "on" : "off";
}

inline std::string StateText(const State& state) {
    return fmt::format("barrier={} wait={} sleep={}",
                       OnOff(state.barrier.load(std::memory_order_relaxed)),
                       OnOff(state.wait.load(std::memory_order_relaxed)),
                       OnOff(state.sleep.load(std::memory_order_relaxed)));
}

/// Logs the new state and the period since the previous press (or the start), which ran with
/// `before`, then starts a new period.
inline void LogPeriod(State& state, std::string_view event, std::string_view before) {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - state.period_start).count();
    const double since_start = std::chrono::duration<double>(now - state.start).count();
    const u64 frames = state.counts.frames.exchange(0, std::memory_order_relaxed);
    const u64 terrain_draws = state.counts.terrain_draws.exchange(0, std::memory_order_relaxed);
    const u64 runs = state.counts.runs.exchange(0, std::memory_order_relaxed);
    const u64 barriers = state.counts.barriers.exchange(0, std::memory_order_relaxed);
    const u64 waits = state.counts.waits.exchange(0, std::memory_order_relaxed);
    const u64 wait_us = state.counts.wait_us.exchange(0, std::memory_order_relaxed);
    const u64 sleeps = state.counts.sleeps.exchange(0, std::memory_order_relaxed);
    const u64 sleep_us = state.counts.sleep_us.exchange(0, std::memory_order_relaxed);
    LOG_WARNING(Render_Vulkan,
                "SQUARE_PROBE {} t={:.1f}s now {} | previous {:.1f}s with {}: frames={} "
                "fps={:.1f} terrain_draws={} runs={} barriers={} waits={} wait_ms={:.1f} "
                "sleeps={} sleep_ms={:.1f}",
                event, since_start, StateText(state), seconds, before, frames,
                seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0, terrain_draws, runs,
                barriers, waits, static_cast<double>(wait_us) / 1000.0, sleeps,
                static_cast<double>(sleep_us) / 1000.0);
    state.period_start = now;
}

/// RasterizerVulkan::TickFrame: reads the keys; a press switches its probe for the next frames.
inline void Frame() {
    if (!Enabled()) {
        return;
    }
    State& state = GetState();
    if (!state.announced) {
        state.announced = true;
        state.start = state.period_start = std::chrono::steady_clock::now();
        LOG_WARNING(Render_Vulkan,
                    "SQUARE_PROBE ready b62 keys F1=barrier F3=wait F12=sleep sleep_ms={} "
                    "terrain_fs={} (EDEN_SQUARE_PROBE)",
                    GetConfig().sleep_ms, GetConfig().terrain_text);
    }
    state.counts.frames.fetch_add(1, std::memory_order_relaxed);
    state.previous_terrain.store(false, std::memory_order_relaxed);
    static constexpr std::array<int, 3> KEYS{BARRIER_KEY, WAIT_KEY, SLEEP_KEY};
    static constexpr std::array<std::string_view, 3> NAMES{"F1", "F3", "F12"};
    const std::array<std::atomic<bool>*, 3> probes{&state.barrier, &state.wait, &state.sleep};
    for (size_t i = 0; i < KEYS.size(); ++i) {
        const bool down = KeyDown(KEYS[i]);
        const bool pressed = down && !state.keys_down[i];
        state.keys_down[i] = down;
        if (!pressed) {
            continue;
        }
        const std::string before = StateText(state);
        probes[i]->store(!probes[i]->load(std::memory_order_relaxed), std::memory_order_relaxed);
        ++state.presses;
        LogPeriod(state, fmt::format("press={} key={}", state.presses, NAMES[i]), before);
    }
}

/// ~RasterizerVulkan: the period after the last press.
inline void Shutdown() {
    if (!Enabled() || !GetState().announced) {
        return;
    }
    State& state = GetState();
    LogPeriod(state, "end", StateText(state));
}

} // namespace Vulkan::SquareProbe
#endif
