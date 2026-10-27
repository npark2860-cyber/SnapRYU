// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// DRAW_TARGET (b69, 2026-10-19): did any draw or clear target the memory of a terrain tile that
// reads back empty, and did it run?
//
// b67/b68 (runs\B67-READALL-20261019-014438-592, runs\B68-DMA-20261019-031906-832): after a save
// reload the empty tiles were never written by anyone: no GPU write into the tile image, no
// render target image at its memory, no DMA copy into it (3161 launches, ring complete), no
// buffer cache GPU write, CPU writes on one page of one tile. A good tile is baked by render
// target draws into its memory (mip 0 as a 288x260 target at the tile address, mip 1 right after
// it, ...). So either the game never bound a render target there, or it did and the draw or clear
// was dropped before the texture cache saw it: render condition false (Maxwell3D::ShouldExecute),
// no graphics pipeline, a clear with no channels.
//
// Every draw and clear notes its bound targets (the color targets UpdateRenderTargets binds,
// index < rt_control.count with an address and a format, and zeta when enabled) in DrawManager,
// and the rasterizer reports what became of it. Per target address: counts per kind (draw,
// clear, indirect) and outcome, first and last frame, the render condition of the newest skip.
// At F7 every zero tile tile_trace.h lists lists the targets inside its memory (DRAW_TILE hit=in)
// and near it (hit=near, within NEAR bytes: the neighbouring baked tiles for comparison).
// DRAW_TARGET gives the totals since boot.
//
// Diagnostic builds only; release builds compile none of this. GPU thread (DrawManager, the
// rasterizer, the texture cache TickFrame), behind one mutex.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::DrawTargetTrace {

enum class Kind : u8 { Draw, Clear, Indirect, Count };

enum class Outcome : u8 {
    Ran,              // reached the texture cache: Clear's UpdateRenderTargets, or a draw's
                      // GraphicsPipeline::Configure (ConfigureImpl binds the render targets
                      // before its only early return)
    Predicate,        // render condition false: DrawManager did not call the rasterizer
    NullPipeline,     // PrepareDraw: no graphics pipeline
    Bisect,           // PrepareDraw: shader bisect skip
    ClearNoChannels,  // Clear with no color, depth or stencil bit
    Unreported,       // the rasterizer reported nothing before the next draw
    Count,
};

inline const char* OutcomeName(Outcome outcome) {
    static constexpr std::array<const char*, static_cast<size_t>(Outcome::Count)> names{
        "ran", "predicate", "null_pipeline", "bisect", "clear_no_channels", "unreported"};
    const size_t index = static_cast<size_t>(outcome);
    return index < names.size() ? names[index] : "?";
}

inline constexpr size_t KINDS = static_cast<size_t>(Kind::Count);
inline constexpr size_t OUTCOMES = static_cast<size_t>(Outcome::Count);
inline constexpr size_t MAX_BOUND = 9; // 8 color targets + zeta
inline constexpr size_t MAX_TARGETS = size_t{1} << 20;
inline constexpr u64 NEAR_BYTES = 0x40000;
inline constexpr size_t MAX_IN_LISTED = 12;
inline constexpr size_t MAX_NEAR_LISTED = 16;

struct Bound {
    u64 gpu = 0;
    u32 width = 0;
    u32 height = 0;
    u32 format = 0; // RenderTargetFormat or DepthFormat, raw
    bool zeta = false;
};

struct Note {
    Kind kind = Kind::Draw;
    bool execute = true;
    u32 mode = 0;      // render_enable.mode
    u32 override_mode = 0; // render_enable_override
    u64 condition = 0; // render_enable address
    u64 fs = 0;        // fragment program address
    u32 count = 0;     // vertices / indices / layers / max draws
    size_t bound = 0;
    std::array<Bound, MAX_BOUND> targets{};
};

struct Target {
    u64 gpu = 0;
    u32 width = 0;
    u32 height = 0;
    u32 format = 0;
    u32 shapes = 0; // times width, height or format changed
    bool zeta = false;
    u64 first_frame = 0;
    u64 last_frame = 0;
    u64 last_ran_frame = 0;
    u64 last_skip_frame = 0;
    Outcome last_skip = Outcome::Ran;
    u32 skip_mode = 0;
    u32 skip_override = 0;
    u64 skip_condition = 0;
    u64 fs = 0;
    std::array<std::array<u32, OUTCOMES>, KINDS> counts{};

    u64 Total(Outcome outcome) const {
        u64 total = 0;
        for (const auto& kind : counts) {
            total += kind[static_cast<size_t>(outcome)];
        }
        return total;
    }
    u64 Skipped() const {
        u64 total = 0;
        for (size_t o = 1; o < OUTCOMES; ++o) {
            total += Total(static_cast<Outcome>(o));
        }
        return total;
    }
};

inline std::mutex mutex;
inline std::unordered_map<u64, Target>& targets = *new std::unordered_map<u64, Target>();
inline std::array<std::array<u64, OUTCOMES>, KINDS> totals{};
inline u64 notes = 0;
inline u64 notes_without_target = 0;
inline u64 dropped_targets = 0; // not kept: MAX_TARGETS reached
inline u64 frame = 0;           // texture cache frame_tick, set by TileTrace::EndFrame
inline Note pending{};
inline bool open = false;

inline u64 Key(u64 gpu, bool zeta) {
    return (gpu << 1) | (zeta ? 1U : 0U);
}

inline void Apply(const Note& note, Outcome outcome) {
    const size_t kind = static_cast<size_t>(note.kind);
    const size_t index = static_cast<size_t>(outcome);
    ++totals[kind][index];
    for (size_t i = 0; i < note.bound; ++i) {
        const Bound& b = note.targets[i];
        const u64 key = Key(b.gpu, b.zeta);
        auto it = targets.find(key);
        if (it == targets.end()) {
            if (targets.size() >= MAX_TARGETS) {
                ++dropped_targets;
                continue;
            }
            Target fresh{};
            fresh.gpu = b.gpu;
            fresh.width = b.width;
            fresh.height = b.height;
            fresh.format = b.format;
            fresh.zeta = b.zeta;
            fresh.first_frame = frame;
            it = targets.emplace(key, fresh).first;
        }
        Target& t = it->second;
        if (t.width != b.width || t.height != b.height || t.format != b.format) {
            ++t.shapes;
            t.width = b.width;
            t.height = b.height;
            t.format = b.format;
        }
        t.last_frame = frame;
        t.fs = note.fs;
        ++t.counts[kind][index];
        if (outcome == Outcome::Ran) {
            t.last_ran_frame = frame;
        } else {
            t.last_skip_frame = frame;
            t.last_skip = outcome;
            t.skip_mode = note.mode;
            t.skip_override = note.override_mode;
            t.skip_condition = note.condition;
        }
    }
}

// DrawManager (TraceConditionalDraw), before it calls the rasterizer.
inline void Begin(const Note& note) {
    std::scoped_lock lock{mutex};
    ++notes;
    if (open) {
        Apply(pending, Outcome::Unreported);
        open = false;
    }
    if (note.bound == 0) {
        ++notes_without_target;
    }
    if (!note.execute) {
        Apply(note, Outcome::Predicate);
        return;
    }
    pending = note;
    open = true;
}

// The rasterizer: what became of the draw or clear DrawManager just noted.
inline void Report(Outcome outcome) {
    std::scoped_lock lock{mutex};
    if (!open) {
        return;
    }
    open = false;
    Apply(pending, outcome);
}

inline bool Inside(u64 addr, u64 gpu, u64 size) {
    return addr >= gpu && addr < gpu + size;
}

inline bool IsNear(u64 addr, u64 gpu, u64 size) {
    return addr + NEAR_BYTES >= gpu && addr < gpu + size + NEAR_BYTES;
}

template <typename AgeText>
inline void LogTarget(const char* hit, const Target& t, u64 gpu, u64 now, AgeText&& age, u32 id,
                      u32 number) {
    const auto& d = t.counts[static_cast<size_t>(Kind::Draw)];
    const auto& c = t.counts[static_cast<size_t>(Kind::Clear)];
    const auto& n = t.counts[static_cast<size_t>(Kind::Indirect)];
    const s64 offset = static_cast<s64>(t.gpu) - static_cast<s64>(gpu);
    LOG_INFO(Render_Vulkan,
             "DRAW_TILE hit={} id={} target={:#x} offset={}{:#x} zeta={} {}x{} format={} shapes={} "
             "first_frame={} last_frame={} last_age_s={} draw_ran={} draw_predicate={} "
             "clear_ran={} clear_predicate={} indirect_ran={} indirect_predicate={} "
             "null_pipeline={} bisect={} clear_no_channels={} unreported={} "
             "last_ran_frame={} last_skip_frame={} last_skip={} skip_mode={} skip_override={} "
             "skip_condition={:#x} fs={:#x} n={}",
             hit, id, t.gpu, offset < 0 ? "-" : "+", offset < 0 ? -offset : offset, t.zeta,
             t.width, t.height, t.format, t.shapes, t.first_frame, t.last_frame,
             age(now, t.last_frame), d[0], d[1], c[0], c[1], n[0], n[1],
             t.Total(Outcome::NullPipeline), t.Total(Outcome::Bisect),
             t.Total(Outcome::ClearNoChannels),
             t.Total(Outcome::Unreported), t.last_ran_frame, t.last_skip_frame,
             t.last_skip_frame != 0 ? OutcomeName(t.last_skip) : "-", t.skip_mode,
             t.skip_override, t.skip_condition, t.fs, number);
}

// F7, for a listed zero tile: the targets inside its memory and near it.
template <typename AgeText>
inline void ListFor(u32 id, u64 gpu, u64 size, u64 now, AgeText&& age, u32 number) {
    std::scoped_lock lock{mutex};
    std::vector<const Target*> inside;
    std::vector<const Target*> nearby; // not "near": a Windows macro
    for (const auto& [key, t] : targets) {
        if (Inside(t.gpu, gpu, size)) {
            inside.push_back(&t);
        } else if (IsNear(t.gpu, gpu, size)) {
            nearby.push_back(&t);
        }
    }
    const auto by_address = [](const Target* a, const Target* b) {
        return a->gpu != b->gpu ? a->gpu < b->gpu : a->zeta < b->zeta;
    };
    std::sort(inside.begin(), inside.end(), by_address);
    std::sort(nearby.begin(), nearby.end(), by_address);
    u64 ran = 0;
    u64 skipped = 0;
    for (size_t i = 0; i < inside.size(); ++i) {
        ran += inside[i]->Total(Outcome::Ran);
        skipped += inside[i]->Skipped();
        if (i < MAX_IN_LISTED) {
            LogTarget("in", *inside[i], gpu, now, age, id, number);
        }
    }
    for (size_t i = 0; i < nearby.size() && i < MAX_NEAR_LISTED; ++i) {
        LogTarget("near", *nearby[i], gpu, now, age, id, number);
    }
    LOG_INFO(Render_Vulkan,
             "DRAW_TILE end id={} gpu={:#x} size={:#x} in_targets={} in_ran={} in_skipped={} "
             "near_targets={} n={}",
             id, gpu, size, inside.size(), ran, skipped, nearby.size(), number);
}

// F7, every zero tile.
struct TileCount {
    size_t tiles = 0;
    size_t with_target = 0;
    size_t with_ran = 0;
    size_t with_skip = 0;
    size_t with_skip_only = 0; // targets inside, none of their draws or clears ran
};

inline void CountFor(TileCount& count, u64 gpu, u64 size) {
    std::scoped_lock lock{mutex};
    ++count.tiles;
    bool any = false;
    u64 ran = 0;
    u64 skipped = 0;
    for (const auto& [key, t] : targets) {
        if (Inside(t.gpu, gpu, size)) {
            any = true;
            ran += t.Total(Outcome::Ran);
            skipped += t.Skipped();
        }
    }
    count.with_target += any ? 1 : 0;
    count.with_ran += ran != 0 ? 1 : 0;
    count.with_skip += skipped != 0 ? 1 : 0;
    count.with_skip_only += any && ran == 0 ? 1 : 0;
}

inline void Summary(const TileCount& count, u32 number) {
    std::scoped_lock lock{mutex};
    const auto k = [](Kind kind, Outcome outcome) {
        return totals[static_cast<size_t>(kind)][static_cast<size_t>(outcome)];
    };
    u64 null_pipeline = 0;
    u64 bisect = 0;
    u64 unreported = 0;
    for (size_t kind = 0; kind < KINDS; ++kind) {
        null_pipeline += totals[kind][static_cast<size_t>(Outcome::NullPipeline)];
        bisect += totals[kind][static_cast<size_t>(Outcome::Bisect)];
        unreported += totals[kind][static_cast<size_t>(Outcome::Unreported)];
    }
    LOG_INFO(Render_Vulkan,
             "DRAW_TARGET n={} notes={} without_target={} targets={} dropped_targets={} "
             "draw_ran={} draw_predicate={} clear_ran={} clear_predicate={} indirect_ran={} "
             "indirect_predicate={} null_pipeline={} bisect={} "
             "clear_no_channels={} unreported={} frame={} zero_tiles={} zero_tiles_target={} "
             "zero_tiles_ran={} zero_tiles_skipped={} zero_tiles_skipped_only={}",
             number, notes, notes_without_target, targets.size(), dropped_targets,
             k(Kind::Draw, Outcome::Ran), k(Kind::Draw, Outcome::Predicate),
             k(Kind::Clear, Outcome::Ran), k(Kind::Clear, Outcome::Predicate),
             k(Kind::Indirect, Outcome::Ran), k(Kind::Indirect, Outcome::Predicate),
             null_pipeline, bisect,
             k(Kind::Clear, Outcome::ClearNoChannels), unreported, frame, count.tiles,
             count.with_target, count.with_ran, count.with_skip, count.with_skip_only);
}

} // namespace VideoCommon::DrawTargetTrace
#endif
