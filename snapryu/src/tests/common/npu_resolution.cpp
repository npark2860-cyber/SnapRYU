// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include "common/npu_resolution_setting.h"
#include "common/scope_exit.h"
TEST_CASE("NPU processing resolution stays fixed until complete session drain", "[npu][settings]") {
    using namespace Settings;
    NpuSession::Drained();
    SCOPE_EXIT {
        NpuSession::Drained();
    };
    Linkage linkage;
    NpuResolutionSetting setting{linkage};
    REQUIRE(setting.GetValue() == NpuProcessingResolution::P720);
    REQUIRE(setting.GetDefault() == NpuProcessingResolution::P720);
    REQUIRE_FALSE(setting.RuntimeModifiable());
    setting.SetValue(NpuProcessingResolution::P1080);
    REQUIRE(NpuSession::BeginLoading());
    REQUIRE_FALSE(NpuSession::BeginLoading());
    setting.LoadString("0");
    setting.SetGlobal(false);
    REQUIRE(setting.GetValue() == NpuProcessingResolution::P1080);
    REQUIRE(setting.UsingGlobal());
    {
        NpuSession::StartupRead startup;
        setting.SetGlobal(false);
        setting.LoadString("0");
        REQUIRE(setting.GetValue() == NpuProcessingResolution::P720);
        REQUIRE_FALSE(setting.UsingGlobal());
        setting.SetValue(NpuProcessingResolution::P1080);
        // Startup reads are allowed, startup writes to the INI remain forbidden.
        auto lock = NpuSession::LockForSetting(&setting);
        REQUIRE(lock.owns_lock());
        REQUIRE(NpuSession::Busy());
    }
    NpuSession::Freeze(1080);
    REQUIRE(NpuSession::Height() == 1080);
    {
        NpuSession::StartupRead too_late;
        setting.LoadString("0");
    }
    REQUIRE(setting.GetValue() == NpuProcessingResolution::P1080);
    NpuSession::Freeze(720);
    REQUIRE(NpuSession::Height() == 1080);
    for (bool stopping : {false, true}) {
        if (stopping)
            NpuSession::Stopping();
        else
            NpuSession::Active();
        REQUIRE(NpuSession::Busy());
        REQUIRE_FALSE(NpuSession::CanRead());
        setting.LoadString("");
        setting = NpuProcessingResolution::P720;
        setting.SetGlobal(true);
        REQUIRE(setting.GetValue() == NpuProcessingResolution::P1080);
        REQUIRE_FALSE(setting.UsingGlobal());
        for (auto& restore : linkage.restore_functions)
            restore();
        REQUIRE_FALSE(setting.UsingGlobal());
        REQUIRE(NpuSession::Height() == 1080);
    }
    NpuSession::Drained();
    REQUIRE_FALSE(NpuSession::Busy());
    setting.SetGlobal(true);
    setting.SetValue(NpuProcessingResolution::P720);
    REQUIRE(setting.UsingGlobal());
    REQUIRE(setting.GetValue() == NpuProcessingResolution::P720);
    REQUIRE(NpuSession::BeginLoading());
    NpuSession::Freeze(720);
    REQUIRE(NpuSession::Height() == 720);
}
