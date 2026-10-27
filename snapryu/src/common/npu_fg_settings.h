// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include "common/npu_fg_control.h"
#include "common/settings_setting.h"
#include "common/npu_resolution_setting.h"
namespace Settings {
template <>
inline std::vector<std::pair<std::string_view, NpuFgMode>>
EnumMetadata<NpuFgMode>::Canonicalizations() {
    return {{"Off", NpuFgMode::Off}, {"Auto", NpuFgMode::Auto}, {"Fixed2x", NpuFgMode::Fixed2x}};
}
template <> inline u32 EnumMetadata<NpuFgMode>::Index() { return 250; }
template <> inline NpuFgMode EnumMetadata<NpuFgMode>::GetFirst() { return NpuFgMode::Off; }
template <> inline NpuFgMode EnumMetadata<NpuFgMode>::GetLast() { return NpuFgMode::Fixed2x; }
extern SwitchableSetting<NpuFgMode> npu_fg_mode;
extern SwitchableSetting<u32, true> npu_fg_target_fps;
// Runtime-only bypass for a game started with frame generation enabled. Keep the
// session's model/profile alive, and reset this flag on the next game boot.
inline std::atomic<bool> npu_fg_paused{false};
} // namespace Settings
