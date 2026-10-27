// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>

namespace VideoCore {
class ShaderNotify {
public:
    [[nodiscard]] int ShadersBuilding() noexcept;

    // Plain counter reads for the performance overlay. Read completed before started so
    // started >= completed (a shader is marked building before it is marked complete).
    [[nodiscard]] int ShadersStarted() const noexcept {
        return num_building.load();
    }
    [[nodiscard]] int ShadersCompleted() const noexcept {
        return num_complete.load();
    }

    void MarkShaderComplete() noexcept {
        ++num_complete;
    }

    void MarkShaderBuilding() noexcept {
        ++num_building;
    }

private:
    std::atomic_int num_building{};
    std::atomic_int num_complete{};
    int report_base{};

    bool completed{};
    int num_when_completed{};
    std::chrono::steady_clock::time_point complete_time;
};
} // namespace VideoCore
