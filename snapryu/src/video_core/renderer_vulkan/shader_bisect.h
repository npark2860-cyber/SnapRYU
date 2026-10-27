// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Interactive shader bisection for rendering bugs (EDEN_SHADER_BISECT=1).
//
// While the game runs, every few seconds Eden writes log/bisect_seen.txt: the guest shader hashes
// used by draws/dispatches since the previous report, one per line as "<stage> <hash> <count>"
// (stage: V vertex, F fragment, G geometry, T tessellation, C compute). Every second it re-reads
// log/bisect_skip.txt: one hex hash per line ('#' starts a comment). Any draw or dispatch that uses
// a listed shader is skipped, so the file can be edited while the game keeps running to find which
// shader produces a visual artifact.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/path_util.h"

namespace Vulkan::ShaderBisect {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_SHADER_BISECT");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

namespace Detail {

struct SeenEntry {
    char stage{};
    u64 count{};
};

struct StorageUse {
    char stage{};
    bool written{};
    u64 count{};
};

struct State {
    std::mutex mutex;
    // Storage buffer consumers: gpu address -> (shader hash -> use)
    std::unordered_map<u64, std::unordered_map<u64, StorageUse>> storage;
    std::array<u64, 6> current_hashes{};
    u64 current_compute{};
    bool current_is_compute{};
    int storage_stage = -1; // Maxwell graphics stage of the binding being recorded, -1 compute
    std::unordered_set<u64> skip;
    std::unordered_map<u64, SeenEntry> seen;
    std::chrono::steady_clock::time_point last_reload{};
    std::chrono::steady_clock::time_point last_report{};
    u64 skipped_since_report{};
};

inline State& GetState() {
    static State state;
    return state;
}

inline std::filesystem::path Dir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir);
}

inline void ReloadSkip(State& state) {
    std::ifstream file{Dir() / "bisect_skip.txt"};
    std::unordered_set<u64> skip;
    std::string line;
    while (std::getline(file, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        const auto begin = line.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            continue;
        }
        const auto end = line.find_first_of(" \t\r\n", begin);
        const std::string token = line.substr(begin, end == std::string::npos ? end : end - begin);
        char* parse_end{};
        const unsigned long long value = std::strtoull(token.c_str(), &parse_end, 16);
        if (parse_end != token.c_str() && value != 0) {
            skip.insert(static_cast<u64>(value));
        }
    }
    state.skip = std::move(skip);
}

inline void Report(State& state) {
    std::vector<std::pair<u64, SeenEntry>> entries(state.seen.begin(), state.seen.end());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.second.stage != b.second.stage) {
            return a.second.stage < b.second.stage;
        }
        return a.second.count > b.second.count;
    });
    std::ofstream file{Dir() / "bisect_seen.txt", std::ios::trunc};
    file << fmt::format("# skipping {} shaders, {} draws/dispatches skipped in this window\n",
                        state.skip.size(), state.skipped_since_report);
    for (const auto& [hash, entry] : entries) {
        file << fmt::format("{} {:016x} {}\n", entry.stage, hash, entry.count);
    }
    state.seen.clear();
    state.skipped_since_report = 0;

    std::ofstream ssbo_file{Dir() / "bisect_ssbo.txt", std::ios::trunc};
    std::vector<u64> addresses;
    addresses.reserve(state.storage.size());
    for (const auto& [address, users] : state.storage) {
        addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());
    for (const u64 address : addresses) {
        ssbo_file << fmt::format("{:#014x}:", address);
        for (const auto& [hash, use] : state.storage[address]) {
            ssbo_file << fmt::format(" {}{}:{:016x}x{}", use.stage, use.written ? "W" : "R",
                                     hash, use.count);
        }
        ssbo_file << '\n';
    }
    state.storage.clear();
}

inline void Tick(State& state) {
    const auto now = std::chrono::steady_clock::now();
    if (now - state.last_reload >= std::chrono::seconds{1}) {
        state.last_reload = now;
        ReloadSkip(state);
    }
    if (now - state.last_report >= std::chrono::seconds{3}) {
        state.last_report = now;
        Report(state);
    }
}

} // namespace Detail

/// Records the shaders of a draw (unique_hashes order: VA, VB, TC, TE, GS, FS) and returns true
/// when the draw must be skipped.
inline bool ShouldSkipDraw(const std::array<u64, 6>& hashes) {
    static constexpr std::array<char, 6> stages{'V', 'V', 'T', 'T', 'G', 'F'};
    auto& state = Detail::GetState();
    std::scoped_lock lock{state.mutex};
    Detail::Tick(state);
    state.current_hashes = hashes;
    state.current_is_compute = false;
    bool skip = false;
    for (size_t index = 0; index < hashes.size(); ++index) {
        const u64 hash = hashes[index];
        if (hash == 0) {
            continue;
        }
        auto& entry = state.seen[hash];
        entry.stage = stages[index];
        ++entry.count;
        skip = skip || state.skip.contains(hash);
    }
    if (skip) {
        ++state.skipped_since_report;
    }
    return skip;
}

inline bool ShouldSkipDispatch(u64 hash) {
    auto& state = Detail::GetState();
    std::scoped_lock lock{state.mutex};
    Detail::Tick(state);
    state.current_compute = hash;
    state.current_is_compute = true;
    auto& entry = state.seen[hash];
    entry.stage = 'C';
    ++entry.count;
    const bool skip = state.skip.contains(hash);
    if (skip) {
        ++state.skipped_since_report;
    }
    return skip;
}

/// Sets the Maxwell graphics stage (0 vertex .. 4 fragment) of the next storage binding,
/// or -1 for compute.
inline void SetStorageStage(int stage) {
    if (!Enabled()) {
        return;
    }
    auto& state = Detail::GetState();
    std::scoped_lock lock{state.mutex};
    state.storage_stage = stage;
}

/// Records a storage buffer bound for the draw/dispatch most recently passed to ShouldSkip*.
inline void RecordStorage(u64 gpu_addr, bool written) {
    if (!Enabled()) {
        return;
    }
    static constexpr std::array<char, 6> stages{'V', 'V', 'T', 'T', 'G', 'F'};
    auto& state = Detail::GetState();
    std::scoped_lock lock{state.mutex};
    auto& users = state.storage[gpu_addr];
    u64 hash = 0;
    char stage = '?';
    if (state.current_is_compute || state.storage_stage < 0) {
        hash = state.current_compute;
        stage = 'C';
    } else {
        const size_t index = static_cast<size_t>(state.storage_stage) + 1;
        if (index < state.current_hashes.size()) {
            hash = state.current_hashes[index];
            stage = stages[index];
        }
    }
    auto& use = users[hash];
    use.stage = stage;
    use.written = use.written || written;
    ++use.count;
}

} // namespace Vulkan::ShaderBisect
