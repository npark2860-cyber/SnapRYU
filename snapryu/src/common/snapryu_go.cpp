// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/snapryu_go.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SnapGo {
namespace {
struct Host {
    SnapGoApi api{};
    SnapGoMediaStateFn media_state{};
    void* instance{};
#ifdef _WIN32
    HMODULE module{};
#endif
    ~Host() {
        if (instance && api.destroy) api.destroy(instance);
#ifdef _WIN32
        if (module) FreeLibrary(module);
#endif
    }
};
std::mutex state_mutex;
std::shared_ptr<Host> host;
std::filesystem::path profile_path, module_path;
std::string last_error;
bool requested_game{}, requested_suspend{};
std::atomic_bool compact_requested{}, input_blocked{};
std::atomic_bool compact_render_profile{};

template <typename T> T Empty() {
    T value{};
    value.size = sizeof(T);
    value.version = SNAP_GO_ABI;
    return value;
}

std::shared_ptr<Host> GetHost(bool prepare = false) {
    std::lock_guard lock{state_mutex};
    if (host || !prepare) return host;
    if (profile_path.empty() || module_path.empty()) {
        last_error = "SnapRYU Go! has not been configured.";
        return {};
    }
#ifdef _WIN32
    auto next = std::make_shared<Host>();
    next->module = LoadLibraryExW(module_path.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!next->module) {
        last_error = "SnapRYU Go! host could not be loaded (Windows error " +
                     std::to_string(GetLastError()) + ").";
        return {};
    }
    const auto get_api = reinterpret_cast<SnapGoGetApiFn>(GetProcAddress(next->module, "SnapGo_GetApi"));
    if (!get_api || !get_api(SNAP_GO_ABI, sizeof(SnapGoApi), &next->api) ||
        next->api.version != SNAP_GO_ABI || next->api.size < sizeof(SnapGoApi) ||
        !next->api.create || !next->api.destroy || !next->api.set_enabled ||
        !next->api.set_bind_address || !next->api.initialize_identity ||
        !next->api.reset_pairing || !next->api.open_pairing || !next->api.cancel_pairing ||
        !next->api.set_game_running || !next->api.set_suspended || !next->api.get_status ||
        !next->api.list_addresses || !next->api.read_input || !next->api.set_rumble ||
        !next->api.video_demand || !next->api.push_video || !next->api.push_audio ||
        !next->api.close_audio || !next->api.set_input_blocked) {
        last_error = "SnapRYU Go! host API version does not match this application.";
        return {};
    }
    next->media_state = reinterpret_cast<SnapGoMediaStateFn>(
        GetProcAddress(next->module, "SnapGo_GetMediaState"));
    next->instance = next->api.create(profile_path.c_str());
    if (!next->instance) {
        last_error = "SnapRYU Go! host could not be created.";
        return {};
    }
    next->api.set_game_running(next->instance, requested_game);
    next->api.set_suspended(next->instance, requested_suspend);
    next->api.set_input_blocked(next->instance, input_blocked.load());
    host = next;
    last_error.clear();
    return next;
#else
    last_error = "SnapRYU Go! requires Windows.";
    return {};
#endif
}

bool Valid(const SnapGoInput& value) {
    if (value.size < sizeof(value) || value.version != SNAP_GO_ABI ||
        value.buttons_wire17 >> 17) return false;
    for (float v : value.sticks) if (!std::isfinite(v) || std::abs(v) > 1.001f) return false;
    for (float v : value.gyro_rad_s) if (!std::isfinite(v) || std::abs(v) > 100.0f) return false;
    for (float v : value.accel_g) if (!std::isfinite(v) || std::abs(v) > 21.0f) return false;
    float norm{};
    for (float v : value.quaternion_wxyz) {
        if (!std::isfinite(v)) return false;
        norm += v * v;
    }
    if (value.motion_active && (norm < 0.9f || norm > 1.1f)) return false;
    return std::isfinite(value.touch_x) && std::isfinite(value.touch_y) &&
           value.touch_x >= 0 && value.touch_x <= 1 && value.touch_y >= 0 && value.touch_y <= 1;
}
} // namespace

bool Initialize(const std::filesystem::path& profile, const std::filesystem::path& module) {
    std::lock_guard lock{state_mutex};
    if (!profile.is_absolute() || !module.is_absolute() || host) return false;
    profile_path = profile;
    module_path = module;
    last_error.clear();
    input_blocked.store(false);
    return true;
}

void Shutdown() {
    compact_render_profile.store(false);
    std::shared_ptr<Host> retired;
    {
        std::lock_guard lock{state_mutex};
        retired = std::move(host);
        requested_game = requested_suspend = false;
        profile_path.clear();
        module_path.clear();
    }
    input_blocked.store(true);
    if (retired) retired->api.set_enabled(retired->instance, false);
    // Each active call retains its Host. Destruction cannot unload executing code.
}

bool SetEnabled(bool enabled) {
    auto owner = GetHost(enabled);
    if (!owner) return !enabled;
    if (!enabled) {
        owner->api.set_rumble(owner->instance, 0, 0, 0);
        owner->api.set_rumble(owner->instance, 1, 0, 0);
    }
    return owner->api.set_enabled(owner->instance, enabled) != 0;
}
bool SetBindAddress(const std::string& address) {
    auto owner = GetHost(true);
    return owner && owner->api.set_bind_address(owner->instance, address.c_str()) != 0;
}
bool InitializeIdentity() {
    auto owner = GetHost(true);
    return owner && owner->api.initialize_identity(owner->instance) != 0;
}
bool ResetPairing() {
    auto owner = GetHost(true);
    return owner && owner->api.reset_pairing(owner->instance) != 0;
}
std::optional<PairingGrid> OpenPairing() {
    auto owner = GetHost(true);
    if (!owner) return {};
    PairingGrid grid;
    grid.modules.resize(177 * 177);
    if (!owner->api.open_pairing(owner->instance, grid.modules.data(),
                                  static_cast<std::uint32_t>(grid.modules.size()), &grid.side) ||
        grid.side < 21 || grid.side > 177) return {};
    grid.modules.resize(grid.side * grid.side);
    if (std::any_of(grid.modules.begin(), grid.modules.end(), [](auto v) { return v > 1; })) return {};
    return grid;
}
void CancelPairing() {
    if (auto owner = GetHost()) owner->api.cancel_pairing(owner->instance);
}
bool SetGameRunning(bool running) {
    if (!running) compact_render_profile.store(false);
    { std::lock_guard lock{state_mutex}; requested_game = running; }
    auto owner = GetHost();
    return !owner || owner->api.set_game_running(owner->instance, running) != 0;
}
void PrepareGameRenderProfile() {
    compact_render_profile.store(compact_requested.load() && GetStatus().enabled);
}
bool UsesCompactRenderProfile() { return compact_render_profile.load(); }
void SetSuspended(bool suspended) {
    { std::lock_guard lock{state_mutex}; requested_suspend = suspended; }
    if (auto owner = GetHost()) owner->api.set_suspended(owner->instance, suspended);
}
SnapGoStatus GetStatus() {
    auto status = Empty<SnapGoStatus>();
    if (auto owner = GetHost()) {
        if (owner->api.get_status(owner->instance, &status) &&
            status.size >= sizeof(status) && status.version == SNAP_GO_ABI) {
            status.bind_address[sizeof(status.bind_address) - 1] = 0;
            status.message[sizeof(status.message) - 1] = 0;
            return status;
        }
        status = Empty<SnapGoStatus>();
        status.fault = true;
        std::strncpy(status.message, "SnapRYU Go! host status is unavailable.", sizeof(status.message)-1);
        return status;
    }
    std::lock_guard lock{state_mutex};
    status.game_running = requested_game;
    status.suspended = requested_suspend;
    status.fault = !last_error.empty();
    std::strncpy(status.message, last_error.empty() ? "SnapRYU Go! is off." : last_error.c_str(), sizeof(status.message)-1);
    return status;
}
std::vector<std::string> ListAddresses() {
    auto owner = GetHost(true);
    if (!owner) return {};
    std::string buffer(8192, '\0');
    if (!owner->api.list_addresses(owner->instance, buffer.data(), static_cast<std::uint32_t>(buffer.size()))) return {};
    buffer.back() = 0;
    std::istringstream lines{buffer.c_str()};
    std::vector<std::string> result;
    for (std::string line; std::getline(lines, line); ) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) result.push_back(std::move(line));
    }
    return result;
}
SnapGoInput ReadInput() {
    auto input = Empty<SnapGoInput>();
    if (auto owner = GetHost()) {
        if (!owner->api.read_input(owner->instance, &input) || !Valid(input)) return Empty<SnapGoInput>();
    }
    return input;
}
void SetRumble(std::uint32_t side, float low, float high) {
    if (side > 1 || !std::isfinite(low) || !std::isfinite(high)) return;
    if (auto owner = GetHost()) owner->api.set_rumble(owner->instance, side,
        std::clamp(low, 0.0f, 1.0f), std::clamp(high, 0.0f, 1.0f));
}
void SetInputBlocked(bool blocked) {
    input_blocked.store(blocked);
    if (auto owner = GetHost()) owner->api.set_input_blocked(owner->instance, blocked);
}
bool IsInputBlocked() { return input_blocked.load(); }
bool IsRemotePlayActive() {
    const auto status = GetStatus();
    return status.enabled && status.connected && status.game_running && !status.suspended && !status.fault;
}
void SetCompactRequested(bool requested) { compact_requested.store(requested); }
bool IsCompactRequested() { return compact_requested.load(); }
bool IsCompactActive() {
    if (!UsesCompactRenderProfile() || !IsCompactRequested() || !IsRemotePlayActive()) return false;
    return VideoDemand().enabled != 0;
}
SnapGoVideoDemand VideoDemand() {
    auto value = Empty<SnapGoVideoDemand>();
    if (auto owner = GetHost()) {
        if (!owner->api.video_demand(owner->instance, &value) || value.size < sizeof(value) ||
            value.version != SNAP_GO_ABI || (value.enabled &&
            (value.width != SNAP_GO_WIDTH || value.height != SNAP_GO_HEIGHT ||
             value.fps == 0 || value.fps > 120))) return Empty<SnapGoVideoDemand>();
    }
    return value;
}
bool PushVideoFrame(const SnapGoVideoFrame& frame) {
    auto owner = GetHost();
    return owner && owner->api.push_video(owner->instance, &frame) != 0;
}
bool PushAudioPcm16(std::uint64_t stream, std::uint64_t time, std::uint32_t channels,
                    std::uint32_t frames, const std::int16_t* samples) {
    auto owner = GetHost();
    return owner && owner->api.push_audio(owner->instance, stream, time, channels, frames, samples) != 0;
}
bool ProcessAudioCallback(std::uint64_t stream, std::uint64_t time, std::uint32_t channels,
                          std::uint32_t frames, const std::int16_t* samples) {
    // Keep one owner throughout the callback, including during shutdown.
    auto owner = GetHost();
    if (!owner) return false;
    std::uint32_t flags{};
    if (owner->media_state) {
        flags = owner->media_state(owner->instance);
    } else {
        // Compatibility with pre-optimization ABI-1 hosts: one status query.
        auto status = Empty<SnapGoStatus>();
        if (!owner->api.get_status(owner->instance, &status) ||
            status.version != SNAP_GO_ABI || status.size < sizeof(status) ||
            !status.enabled || !status.connected || !status.game_running ||
            status.suspended || status.fault) return false;
        if (status.audio_active) flags |= SNAP_GO_MEDIA_AUDIO;
        if (status.audio_playback) flags |= SNAP_GO_MEDIA_PLAYBACK;
        if (UsesCompactRenderProfile() && IsCompactRequested()) {
            auto video = Empty<SnapGoVideoDemand>();
            if (owner->api.video_demand(owner->instance, &video) &&
                video.version == SNAP_GO_ABI && video.size >= sizeof(video) && video.enabled)
                flags |= SNAP_GO_MEDIA_VIDEO;
        }
    }
    if (flags & SNAP_GO_MEDIA_AUDIO)
        owner->api.push_audio(owner->instance, stream, time, channels, frames, samples);
    return UsesCompactRenderProfile() && IsCompactRequested() &&
        (flags & SNAP_GO_MEDIA_PLAYBACK) && (flags & SNAP_GO_MEDIA_VIDEO);
}
void CloseAudioStream(std::uint64_t stream) {
    if (auto owner = GetHost()) owner->api.close_audio(owner->instance, stream);
}
bool RemoteAudioActive() {
    const auto status = GetStatus();
    return status.enabled && status.connected && status.game_running && status.audio_active &&
           !status.suspended && !status.fault;
}
bool RemoteAudioPlaying() {
    const auto status = GetStatus();
    return status.enabled && status.connected && status.game_running && status.audio_playback &&
           !status.suspended && !status.fault;
}
} // namespace SnapGo
