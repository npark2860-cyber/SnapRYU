// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "common/snapryu_go_api.h"

namespace SnapGo {
struct PairingGrid {
    std::uint32_t side{};
    std::vector<std::uint8_t> modules;
};

// Configuration only. OFF does not load the module or create its host instance.
bool Initialize(const std::filesystem::path& profile, const std::filesystem::path& module);
void Shutdown();
bool SetEnabled(bool enabled);
bool SetBindAddress(const std::string& address);
bool InitializeIdentity();
bool ResetPairing();
std::optional<PairingGrid> OpenPairing();
void CancelPairing();
bool SetGameRunning(bool running);
// Immutable for a game session: never changes saved global/per-game settings.
void PrepareGameRenderProfile();
bool UsesCompactRenderProfile();
void SetSuspended(bool suspended);
SnapGoStatus GetStatus();
std::vector<std::string> ListAddresses();

// Call once at the HID producer tick; all HID consumers use its cached result.
SnapGoInput ReadInput();
void SetRumble(std::uint32_t side, float low_amplitude, float high_amplitude);
void SetInputBlocked(bool blocked);
bool IsInputBlocked();

bool IsRemotePlayActive();
void SetCompactRequested(bool requested);
bool IsCompactRequested();
bool IsCompactActive();
SnapGoVideoDemand VideoDemand();
bool PushVideoFrame(const SnapGoVideoFrame& frame);
bool PushAudioPcm16(std::uint64_t stream_id, std::uint64_t timestamp_us,
                    std::uint32_t channels, std::uint32_t frames, const std::int16_t* samples);
void CloseAudioStream(std::uint64_t stream_id);
bool RemoteAudioActive();
bool RemoteAudioPlaying();
// Captures before local mute; returns whether Compact should mute this callback.
bool ProcessAudioCallback(std::uint64_t stream_id, std::uint64_t timestamp_us,
                          std::uint32_t channels, std::uint32_t frames,
                          const std::int16_t* samples);
} // namespace SnapGo
