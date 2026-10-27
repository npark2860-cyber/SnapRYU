// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// b61 SHADER_RECORD (EDEN_SHADER_RECORD=1, diagnostic builds only).
//
// Every pipeline translated in the session leaves its translator inputs and output in
// log/shader_record_<unix time>/:
//   envs.bin   GenericEnvironment::Serialize of each stage, back to back: the bytes the persistent
//              pipeline cache stores (guest code, texture types and pixel formats and constant
//              buffer values read from game memory during translation, program header);
//   envs.tsv   kind (g graphics, c compute), pipeline hash, program slot, shader hash, offset and
//              size in envs.bin, unbound instructions, pipeline built;
//   spirv.tsv  kind, pipeline hash, program slot, shader hash, SPIR-V words, hash of the words.
// The variable also disables the persistent caches (vulkan_device.cpp CollectOutputDiagnostics),
// so the recording run translates every pipeline from game memory. The records of an NCE run and
// a Dynamic run show whether both CPU backends give the GPU the same shaders.

#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <mutex>
#include <span>
#include <system_error>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/shader_environment.h"

namespace Vulkan::ShaderRecord {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_SHADER_RECORD");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

struct State {
    std::mutex mutex;
    bool opened = false;
    bool ok = false;
    std::filesystem::path dir;
    std::ofstream envs_bin;
    std::ofstream envs_tsv;
    std::ofstream spirv_tsv;
};

// Never destroyed: pipelines may still be translated while the process exits.
inline State& Get() {
    static State* const state = new State{};
    return *state;
}

/// Opens the record folder on first use. The caller holds the mutex.
inline bool Open(State& state) {
    if (state.opened) {
        return state.ok;
    }
    state.opened = true;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    state.dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                fmt::format("shader_record_{}", seconds);
    std::error_code error;
    std::filesystem::create_directories(state.dir, error);
    const auto mode = std::ios::binary | std::ios::trunc;
    state.envs_bin.open(state.dir / "envs.bin", mode);
    state.envs_tsv.open(state.dir / "envs.tsv", mode);
    state.spirv_tsv.open(state.dir / "spirv.tsv", mode);
    state.ok = state.envs_bin.is_open() && state.envs_tsv.is_open() && state.spirv_tsv.is_open();
    if (state.ok) {
        state.envs_tsv << "kind\tpipeline\tslot\tshader\toffset\tsize\tunbound\tbuilt\n";
        state.spirv_tsv << "kind\tpipeline\tslot\tshader\twords\tspirv_hash\n";
        state.envs_tsv.flush();
        state.spirv_tsv.flush();
    }
    LOG_WARNING(Render_Vulkan, "SHADER_RECORD dir={} ok={}",
                Common::FS::PathToUTF8String(state.dir), state.ok);
    return state.ok;
}

/// PipelineCache::CreateGraphicsPipeline / CreateComputePipeline, after translation: the
/// environment of one program slot.
inline void NoteEnv(char kind, u64 pipeline, size_t slot, u64 shader,
                    const VideoCommon::GenericEnvironment& env, bool built) {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    if (!Open(state)) {
        return;
    }
    const auto offset = static_cast<u64>(state.envs_bin.tellp());
    env.Serialize(state.envs_bin);
    state.envs_bin.flush();
    const auto size = static_cast<u64>(state.envs_bin.tellp()) - offset;
    state.envs_tsv << fmt::format("{}\t{:016x}\t{}\t{:016x}\t{}\t{}\t{}\t{}\n", kind, pipeline,
                                  slot, shader, offset, size, env.CanBeSerialized() ? 0 : 1,
                                  built ? 1 : 0);
    state.envs_tsv.flush();
}

/// After EmitSPIRV: the generated module of one program slot.
inline void NoteSpirv(char kind, u64 pipeline, size_t slot, u64 shader,
                      std::span<const u32> code) {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    if (!Open(state)) {
        return;
    }
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(code.data()),
                                        code.size_bytes());
    state.spirv_tsv << fmt::format("{}\t{:016x}\t{}\t{:016x}\t{}\t{:016x}\n", kind, pipeline,
                                   slot, shader, code.size(), hash);
    state.spirv_tsv.flush();
}

} // namespace Vulkan::ShaderRecord
#endif
