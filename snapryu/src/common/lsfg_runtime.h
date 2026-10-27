// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <mutex>
#include <string>
namespace Settings::LsfgRuntime {
inline std::atomic<bool> paused{false};
inline std::atomic<bool> active{false};
inline std::mutex failure_mutex;
inline std::string failure;
inline void Failure(std::string message) {
    std::scoped_lock lock{failure_mutex};
    failure = std::move(message);
}
inline std::string Failure() {
    std::scoped_lock lock{failure_mutex};
    return failure;
}
inline void Reset() {
    paused.store(false);
    active.store(false);
    Failure({});
}
} // namespace Settings::LsfgRuntime
