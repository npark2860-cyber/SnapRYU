// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <mutex>
#include "common/settings_setting.h"
namespace Settings {
enum class NpuProcessingResolution : u32 { P720, P1080 };
template <>
inline std::vector<std::pair<std::string_view, NpuProcessingResolution>>
EnumMetadata<NpuProcessingResolution>::Canonicalizations() {
    return {{"720p", NpuProcessingResolution::P720}, {"1080p", NpuProcessingResolution::P1080}};
}
template <>
inline u32 EnumMetadata<NpuProcessingResolution>::Index() {
    return 251;
}
template <>
inline NpuProcessingResolution EnumMetadata<NpuProcessingResolution>::GetFirst() {
    return NpuProcessingResolution::P720;
}
template <>
inline NpuProcessingResolution EnumMetadata<NpuProcessingResolution>::GetLast() {
    return NpuProcessingResolution::P1080;
}
namespace NpuSession {
enum class Phase { Idle, Loading, Active, Stopping };
struct State {
    std::recursive_mutex mutex;
    Phase phase{Phase::Idle};
    u32 height{720};
    bool frozen{};
    std::string failure;
};
inline State& Get() {
    static State state;
    return state;
}
inline thread_local unsigned startup_read_depth{};
inline bool Busy() {
    std::scoped_lock lock{Get().mutex};
    return Get().phase != Phase::Idle;
}
inline bool CanRead() {
    std::scoped_lock lock{Get().mutex};
    return Get().phase == Phase::Idle ||
           (Get().phase == Phase::Loading && !Get().frozen && startup_read_depth);
}
inline bool CanSet() {
    return CanRead();
}
inline bool BeginLoading() {
    std::scoped_lock lock{Get().mutex};
    if (Get().phase != Phase::Idle)
        return false;
    Get().phase = Phase::Loading;
    Get().frozen = false;
    Get().failure.clear();
    return true;
}
class StartupRead {
public:
    StartupRead() {
        std::scoped_lock lock{Get().mutex};
        if (Get().phase == Phase::Loading && !Get().frozen) {
            ++startup_read_depth;
            entered = true;
        }
    }
    ~StartupRead() {
        if (entered)
            --startup_read_depth;
    }
    StartupRead(const StartupRead&) = delete;

private:
    bool entered{};
};
inline void Freeze(u32 height) {
    std::scoped_lock lock{Get().mutex};
    if (Get().phase == Phase::Idle)
        Get().phase = Phase::Loading;
    if (!Get().frozen) {
        Get().height = height == 1080 ? 1080 : 720;
        Get().frozen = true;
    }
}
inline void Active() {
    std::scoped_lock lock{Get().mutex};
    if (Get().phase == Phase::Loading)
        Get().phase = Phase::Active;
}
inline void Stopping() {
    std::scoped_lock lock{Get().mutex};
    if (Get().phase != Phase::Idle)
        Get().phase = Phase::Stopping;
}
inline void Drained() {
    std::scoped_lock lock{Get().mutex};
    Get().phase = Phase::Idle;
    Get().frozen = false;
}
inline u32 Height() {
    std::scoped_lock lock{Get().mutex};
    return Get().height;
}
inline void Failure(std::string reason) {
    std::scoped_lock lock{Get().mutex};
    Get().failure = std::move(reason);
}
inline std::string Failure() {
    std::scoped_lock lock{Get().mutex};
    return Get().failure;
}
inline std::unique_lock<std::recursive_mutex> LockForSetting(const BasicSetting* setting) {
    std::unique_lock<std::recursive_mutex> lock{Get().mutex, std::defer_lock};
    if (setting->GetLabel() == "npu_processing_resolution")
        lock.lock();
    return lock;
}
} // namespace NpuSession
class NpuResolutionSetting final : public Setting<NpuProcessingResolution, true> {
public:
    explicit NpuResolutionSetting(Linkage& linkage)
        : Setting{linkage,
                  NpuProcessingResolution::P720,
                  "npu_processing_resolution",
                  Category::RendererAdvanced,
                  Specialization::Paired,
                  true,
                  false} {
        linkage.restore_functions.emplace_back([this] { SetGlobal(true); });
    }
    void SetGlobal(bool global) override {
        std::scoped_lock lock{NpuSession::Get().mutex};
        if (NpuSession::CanSet())
            use_global = global;
    }
    bool UsingGlobal() const override {
        std::scoped_lock lock{NpuSession::Get().mutex};
        return use_global;
    }
    const NpuProcessingResolution& GetValue() const override {
        return use_global ? value : custom;
    }
    const NpuProcessingResolution& GetValue(bool global) const override {
        return global || use_global ? value : custom;
    }
    void SetValue(const NpuProcessingResolution& val) override {
        std::scoped_lock lock{NpuSession::Get().mutex};
        if (!NpuSession::CanSet())
            return;
        const auto normalized =
            val == NpuProcessingResolution::P1080 ? val : NpuProcessingResolution::P720;
        (use_global ? value : custom) = normalized;
    }
    bool Switchable() const override {
        return true;
    }
    std::string ToStringGlobal() const override {
        return Setting::ToString(value);
    }
    const NpuProcessingResolution& operator=(const NpuProcessingResolution& val) override {
        SetValue(val);
        return GetValue();
    }
    explicit operator const NpuProcessingResolution&() const override {
        return GetValue();
    }

private:
    bool use_global{true};
    NpuProcessingResolution custom{NpuProcessingResolution::P720};
};
extern NpuResolutionSetting npu_processing_resolution;
inline void FreezeNpuProcessingProfile() {
    std::scoped_lock lock{NpuSession::Get().mutex};
    NpuSession::Freeze(
        npu_processing_resolution.GetValue() == NpuProcessingResolution::P1080 ? 1080 : 720);
}
} // namespace Settings
