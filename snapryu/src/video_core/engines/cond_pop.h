// SPDX-FileCopyrightText: Copyright 2026 SnapRYU
// SPDX-License-Identifier: GPL-3.0-or-later

// b77 (diagnostic builds only).
//
// CondPop: the render conditions Maxwell3D::ProcessQueryCondition evaluates in each frame and the
// draws each one lets through or skips. Adreno on Windows has no VK_EXT_conditional_rendering, so
// the condition (two query reports in guest memory) is read on the CPU after the pending query
// results land (renderer_vulkan/cond_wait.h). The 44 s and 50 s pops of the skydive (terrain and
// lake patches missing for 1-3 frames) also happen in the original Eden.
//
// A condition that was true for STEADY frames, then false or not evaluated at all for 1-SHORT
// frames, then true again ("dip") skips its draws for those frames: a 1-3 frame hole. false means
// the query values said "do not draw"; absent means the game did not issue that condition (and its
// draws) in those frames. PopTrace (texture_cache/pop_trace.h) reports the dips with the condition
// words before and during the dip on F8 and every 30 s.
//
// EDEN_COND_POP=1 records. EDEN_COND_FORCE=1 also lets every conditional draw run (the real
// result is still recorded): if the pops are gone with it, the render condition is the cause.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::CondPop {

inline constexpr size_t RING = 192;         // frames kept (PopTrace reports up to 160)
inline constexpr size_t MAX_RECS = 4096;    // conditions recorded per frame
inline constexpr size_t MAX_LISTED = 48;    // COND_DIP lines per report
inline constexpr u32 SHORT = 3;             // a dip lasts at most 3 frames
inline constexpr u32 STEADY = 4;            // with at least 4 true frames each side
inline constexpr size_t NONE = std::numeric_limits<size_t>::max();

struct Rec {
    u64 gpu = 0;
    std::array<u32, 4> words{}; // report A value lo/hi (+0, +4), report B value lo/hi (+16, +20)
    u32 draws = 0;              // draws and clears issued while this condition was in force
    u16 occurrence = 0;         // how many conditions at gpu came before it in the frame
    u8 mode = 0;                // 2 Conditional, 3 IfEqual, 4 IfNotEqual
    u8 result = 0;              // the real result (before EDEN_COND_FORCE)
};

struct Frame {
    u64 tick = 0;
    u64 t_ms = 0;
    std::vector<Rec> recs;
    u32 conds = 0;   // conditional evaluations
    u32 falses = 0;  // of which false
    u32 dropped = 0; // not recorded (MAX_RECS)
    u32 run = 0;     // draws under a true condition
    u32 skipped = 0; // draws under a false condition that did not run
    u32 forced = 0;  // draws under a false condition that ran (EDEN_COND_FORCE)
    u32 uncond = 0;  // draws with no condition in force
};

struct InForce {
    bool conditional = false;
    bool result = true;
    size_t rec = NONE;
};

struct State {
    std::mutex mutex;
    bool record = false;
    bool force = false;
    std::deque<Frame> frames;
    Frame current;
    ankerl::unordered_dense::map<u64, u16> uses;              // gpu -> conditions so far this frame
    ankerl::unordered_dense::map<uintptr_t, InForce> engines; // per Maxwell3D engine
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

inline bool EnvOn(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

inline State& Get() {
    static State state;
    [[maybe_unused]] static const bool ready = [](State& s) {
        s.force = EnvOn("EDEN_COND_FORCE");
        s.record = EnvOn("EDEN_COND_POP") || s.force;
        LOG_WARNING(Render_Vulkan,
                    "COND_POP ready b77 record={} force={} ring={} max_recs={} short={} steady={}",
                    s.record, s.force, RING, MAX_RECS, SHORT, STEADY);
        return true;
    }(state);
    return state;
}

inline bool Active() {
    return Get().record;
}

/// Maxwell3D::ProcessQueryCondition (GPU thread): the condition now in force for engine.
/// conditional: UseRenderEnable with mode Conditional, IfEqual or IfNotEqual. Returns the value
/// for execute_on: result, or true for a conditional one under EDEN_COND_FORCE=1.
inline bool Condition(const void* engine, u64 gpu, u32 mode, bool conditional, bool result,
                      const std::array<u32, 4>& words) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    InForce& in_force = s.engines[reinterpret_cast<uintptr_t>(engine)];
    in_force = InForce{conditional, result, NONE};
    if (!conditional) {
        return result;
    }
    ++s.current.conds;
    s.current.falses += result ? 0 : 1;
    if (s.current.recs.size() < MAX_RECS) {
        u16& uses = s.uses[gpu];
        in_force.rec = s.current.recs.size();
        s.current.recs.push_back(Rec{gpu, words, 0, uses, static_cast<u8>(mode),
                                     static_cast<u8>(result ? 1 : 0)});
        uses = static_cast<u16>((std::min)(u32{uses} + 1, u32{0xffff}));
    } else {
        ++s.current.dropped;
    }
    return result || s.force;
}

/// DrawManager (GPU thread): a draw or clear of engine; runs = Maxwell3D::ShouldExecute().
inline void Draw(const void* engine, bool runs) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    const auto it = s.engines.find(reinterpret_cast<uintptr_t>(engine));
    if (it == s.engines.end() || !it->second.conditional) {
        ++s.current.uncond;
        return;
    }
    const InForce& in_force = it->second;
    if (in_force.rec != NONE && in_force.rec < s.current.recs.size()) {
        ++s.current.recs[in_force.rec].draws;
    }
    if (in_force.result) {
        ++s.current.run;
    } else if (runs) {
        ++s.current.forced;
    } else {
        ++s.current.skipped;
    }
}

/// PopTrace::EndFrame (GPU thread): closes the frame PopTrace calls tick.
inline void EndFrame(u64 tick) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    s.current.tick = tick;
    s.current.t_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - s.start)
                                          .count());
    s.frames.push_back(std::move(s.current));
    s.current = Frame{};
    if (s.frames.size() > RING) {
        s.frames.pop_front();
    }
    s.uses.clear();
    // A condition stays in force across frames until the game sets another one.
    for (auto& [engine, in_force] : s.engines) {
        in_force.rec = NONE;
    }
}

inline std::string Words(const std::array<u32, 4>& w) {
    return fmt::format("{:08x}{:08x},{:08x}{:08x}", w[1], w[0], w[3], w[2]);
}

/// PopTrace::Report: COND_* lines for the frames [first_tick, last_tick], rel to mark_tick.
inline void Report(u64 n, u64 first_tick, u64 mark_tick, u64 last_tick,
                   std::vector<std::string>& lines) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    std::vector<const Frame*> window;
    for (const Frame& frame : s.frames) {
        if (frame.tick >= first_tick && frame.tick <= last_tick) {
            window.push_back(&frame);
        }
    }
    const size_t count = window.size();
    if (count == 0) {
        lines.push_back(fmt::format("COND_REPORT n={} frames=0 force={}", n, s.force));
        return;
    }
    size_t mark = count - 1;
    for (size_t f = 0; f < count; ++f) {
        if (window[f]->tick == mark_tick) {
            mark = f;
        }
    }
    const auto rel = [&](size_t f) {
        return static_cast<long long>(f) - static_cast<long long>(mark);
    };
    // Per condition (gpu, occurrence): 0 absent, 1 false, 2 true in each frame, and its record.
    struct Row {
        std::vector<u8> state;
        std::vector<const Rec*> rec;
    };
    ankerl::unordered_dense::map<u64, Row> rows;
    for (size_t f = 0; f < count; ++f) {
        for (const Rec& r : window[f]->recs) {
            Row& row = rows[(r.gpu << 16) | r.occurrence];
            if (row.state.empty()) {
                row.state.assign(count, 0);
                row.rec.assign(count, nullptr);
            }
            row.state[f] = r.result ? 2 : 1;
            row.rec[f] = &r;
        }
    }
    std::vector<u32> dip_false(count), dip_absent(count), dip_draws(count), appear(count);
    std::vector<std::string> listed;
    u64 false_total = 0, absent_total = 0, appear_total = 0;
    for (const auto& [key, row] : rows) {
        size_t a = 0;
        while (a < count) {
            size_t e = a;
            while (e + 1 < count && (row.state[e + 1] == 2) == (row.state[a] == 2)) {
                ++e;
            }
            const size_t len = e - a + 1;
            const bool off = row.state[a] != 2;
            if (len <= SHORT && a >= STEADY && e + STEADY < count) {
                bool around = true;
                for (size_t k = 1; k <= STEADY; ++k) {
                    const bool before_on = row.state[a - k] == 2;
                    const bool after_on = row.state[e + k] == 2;
                    // A dip needs true on both sides; an appearance needs absent/false.
                    around = around && (off ? before_on && after_on : !before_on && !after_on);
                }
                if (around && off) {
                    bool all_false = true, all_absent = true;
                    for (size_t f = a; f <= e; ++f) {
                        all_false = all_false && row.state[f] == 1;
                        all_absent = all_absent && row.state[f] == 0;
                    }
                    const Rec* before = row.rec[a - 1];
                    const Rec* during = nullptr;
                    for (size_t f = a; f <= e && during == nullptr; ++f) {
                        during = row.rec[f];
                    }
                    for (size_t f = a; f <= e; ++f) {
                        ++(row.state[f] == 1 ? dip_false : dip_absent)[f];
                        dip_draws[f] += before->draws;
                    }
                    ++(all_absent ? absent_total : false_total);
                    if (listed.size() < MAX_LISTED) {
                        listed.push_back(fmt::format(
                            "COND_DIP n={} kind={} rel={}..{} gpu={:#x} occ={} mode={} "
                            "draws_before={} words_before={} words_dip={}",
                            n, all_false ? "false" : (all_absent ? "absent" : "mixed"), rel(a),
                            rel(e), before->gpu, before->occurrence, before->mode, before->draws,
                            Words(before->words),
                            during != nullptr ? Words(during->words) : std::string{"-"}));
                    }
                } else if (around) {
                    for (size_t f = a; f <= e; ++f) {
                        ++appear[f];
                    }
                    ++appear_total;
                }
            }
            a = e + 1;
        }
    }
    u64 conds_sum = 0;
    for (const Frame* frame : window) {
        conds_sum += frame->conds;
    }
    lines.push_back(fmt::format("COND_REPORT n={} frames={} mark_tick={} force={} conds_avg={} "
                                "keys={} dips_false={} dips_absent={} appear={} listed={}",
                                n, count, window[mark]->tick, s.force, conds_sum / count,
                                rows.size(), false_total, absent_total, appear_total,
                                listed.size()));
    for (size_t f = 0; f < count; ++f) {
        const Frame& frame = *window[f];
        lines.push_back(fmt::format(
            "COND_FRAME n={} rel={} tick={} t_ms={} conds={} false={} run={} skipped={} "
            "forced={} uncond={} dropped={} dip_false={} dip_absent={} dip_draws={} appear={}",
            n, rel(f), frame.tick, frame.t_ms, frame.conds, frame.falses, frame.run,
            frame.skipped, frame.forced, frame.uncond, frame.dropped, dip_false[f],
            dip_absent[f], dip_draws[f], appear[f]));
    }
    for (std::string& line : listed) {
        lines.push_back(std::move(line));
    }
    lines.push_back(fmt::format("COND_REPORT end n={}", n));
}

} // namespace VideoCommon::CondPop
#endif
