// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include "common/scope_exit.h"
#include "common/snapryu_go.h"

// The TOTK fixed 1080p option moved to a mod; this keeps the Compact boot-profile checks that
// tests/core/totk_resolution.cpp made alongside it.
#if defined(_WIN32) && defined(SNAPGO_COMPACT_TEST_HOST_PATH)
TEST_CASE("Compact render profile is frozen at boot", "[snapgo]") {
    const bool old_compact = SnapGo::IsCompactRequested();
    SCOPE_EXIT {
        SnapGo::SetGameRunning(false);
        SnapGo::Shutdown();
        SnapGo::SetCompactRequested(old_compact);
    };
    REQUIRE(SnapGo::Initialize(std::filesystem::temp_directory_path() / "snapgo-compact-test-unused",
                              std::filesystem::u8path(SNAPGO_COMPACT_TEST_HOST_PATH)));
    for (unsigned bits = 0; bits < 4; ++bits) {
        const bool enabled = bits & 1, compact = bits & 2;
        CAPTURE(enabled, compact);
        REQUIRE(SnapGo::SetGameRunning(false));
        REQUIRE(SnapGo::SetEnabled(enabled));
        SnapGo::SetCompactRequested(compact);
        SnapGo::PrepareGameRenderProfile();
        const bool profile = enabled && compact;
        REQUIRE(SnapGo::UsesCompactRenderProfile() == profile);

        // A loaded session must not change its profile on disconnect, Go OFF, or a
        // future request change. Only the frozen boot profile controls this.
        REQUIRE(SnapGo::SetGameRunning(true));
        REQUIRE_FALSE(SnapGo::IsRemotePlayActive()); // Fake never connects.
        REQUIRE(SnapGo::SetEnabled(false));
        SnapGo::SetCompactRequested(!compact);
        REQUIRE(SnapGo::UsesCompactRenderProfile() == profile);
        REQUIRE(SnapGo::SetGameRunning(false));
        REQUIRE_FALSE(SnapGo::UsesCompactRenderProfile());
    }
}
#endif
