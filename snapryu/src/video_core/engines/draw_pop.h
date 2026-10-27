// SPDX-FileCopyrightText: Copyright 2026 SnapRYU
// SPDX-License-Identifier: GPL-3.0-or-later

// b78 (diagnostic builds only).
//
// DrawPop: every draw of every frame, by its shader pair (vertex and fragment program address),
// seen from both ends of the emulator. DrawManager says what the game issued: direct draws with
// their vertex or index count and instance count, indirect draws with their maximum draw count,
// clears, and whether the render condition let it run. The rasterizer says what reached the
// command buffer (vkCmdDraw*, direct or indirect) or why a draw was dropped there (no graphics
// pipeline, shader bisect, Configure refused). The buffer cache says, for each indirect draw,
// whether Eden held the argument bytes as GPU-written and whether a CPU upload went over them
// while they were bound. The query cache says how many query reports it published to guest
// memory in the frame and how many of them were zero.
//
// The 44 s and 50 s pops of the skydive (lake and terrain patches missing for 1-3 frames) happen
// under every CPU backend and in the original Eden; b77 ruled out the render conditions and the
// SYNC-FULL run ruled out missing GPU barriers. A "dip" is a shader pair that put a steady number
// of draws into the command buffer for STEADY frames, fewer (or none) for 1-SHORT frames, then the
// steady number again. Its kind says where the draws went: game (the game issued fewer), dropped
// (the game issued the same, fewer reached the command buffer; the drop counts say why), prims
// (the same draws ran with less geometry: counts of 0 or smaller). PopTrace
// (texture_cache/pop_trace.h) reports the dips around each F8 mark and every 30 s.
//
// EDEN_DRAW_POP=1 records. Nothing here changes what is drawn. GPU thread only, behind one mutex.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon::DrawPop {

inline constexpr size_t RING = 192;         // frames kept (PopTrace reports up to 160)
inline constexpr size_t MAX_KEYS = 2048;    // shader pairs recorded per frame
inline constexpr size_t MAX_LISTED = 48;    // DRAW_DIP lines per report
inline constexpr u32 SHORT = 3;             // a dip lasts at most 3 frames
inline constexpr u32 STEADY = 4;            // with at least 4 steady frames each side
inline constexpr u64 STEADY_PERMILLE = 125; // steady: (max - min) * 1000 <= max * 125

/// Why a draw the game issued did not reach the command buffer.
enum class Drop : u8 { Condition, NullPipeline, Bisect, Configure, Count };
inline constexpr size_t DROPS = static_cast<size_t>(Drop::Count);

struct Rec {
    u64 vs = 0;              // vertex program address (program_region + pipelines[1].offset)
    u64 fs = 0;              // fragment program address (pipelines[5])
    u32 issued = 0;          // direct draws the game issued
    u32 issued_indirect = 0; // indirect draws the game issued
    u32 clears = 0;          // clears issued under this pair
    u32 ran = 0;             // direct draws recorded into the command buffer
    u32 ran_indirect = 0;    // indirect draws recorded
    u32 zero = 0;            // direct draws issued with a count or an instance count of 0
    u64 prims = 0;           // sum over direct draws of count x instances
    u64 indirect_max = 0;    // sum over indirect draws of max_draw_counts
    std::array<u32, DROPS> drops{}; // draws that did not reach the command buffer, by reason
};

struct Frame {
    u64 tick = 0;
    u64 t_ms = 0;
    std::vector<Rec> recs;
    u32 dropped_keys = 0;      // draws of pairs not recorded (MAX_KEYS)
    u32 issued = 0;            // direct draws issued by the game
    u32 issued_indirect = 0;
    u32 clears = 0;
    u32 ran = 0;               // direct draws recorded
    u32 ran_indirect = 0;
    std::array<u32, DROPS> drops{};
    u32 args_gpu = 0;          // indirect bindings whose argument bytes Eden holds as GPU-written
    u32 args_cpu = 0;          // indirect bindings whose argument bytes it does not
    u32 args_uploads = 0;      // CPU uploads made while binding indirect arguments or counts
    u64 args_upload_bytes = 0;
    u32 queries = 0;           // query reports published to guest memory
    u32 queries_zero = 0;      // of which value 0
};

struct State {
    std::mutex mutex;
    bool record = false;
    std::deque<Frame> frames;
    Frame current;
    ankerl::unordered_dense::map<u64, u32> index; // pair key -> index in current.recs
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

inline bool EnvOn(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

inline State& Get() {
    static State state;
    [[maybe_unused]] static const bool ready = [](State& s) {
        s.record = EnvOn("EDEN_DRAW_POP");
        LOG_WARNING(Render_Vulkan,
                    "DRAW_POP ready b78 record={} ring={} max_keys={} short={} steady={} "
                    "steady_permille={}",
                    s.record, RING, MAX_KEYS, SHORT, STEADY, STEADY_PERMILLE);
        return true;
    }(state);
    return state;
}

inline bool Active() {
    return Get().record;
}

/// The shader pair of the current draw, from the Maxwell3D registers (any type with
/// program_region.Address() and pipelines[].offset).
template <class Regs>
inline u64 Vs(const Regs& regs) {
    return regs.program_region.Address() + regs.pipelines[1].offset;
}
template <class Regs>
inline u64 Fs(const Regs& regs) {
    return regs.program_region.Address() + regs.pipelines[5].offset;
}

/// The record of the pair in the current frame (nullptr once MAX_KEYS pairs are recorded).
inline Rec* Find(State& s, u64 vs, u64 fs) {
    const u64 key = vs ^ (fs * 0x9E3779B97F4A7C15ULL);
    const auto it = s.index.find(key);
    if (it != s.index.end()) {
        return &s.current.recs[it->second];
    }
    if (s.current.recs.size() >= MAX_KEYS) {
        ++s.current.dropped_keys;
        return nullptr;
    }
    s.index.emplace(key, static_cast<u32>(s.current.recs.size()));
    s.current.recs.push_back(Rec{vs, fs});
    return &s.current.recs.back();
}

/// DrawManager (GPU thread): the game issued a draw (kind 0 arrays, 1 indexed, 3 indirect) or a
/// clear (kind 2); count = vertex/index count, or max_draw_counts for an indirect draw;
/// runs = Maxwell3D::ShouldExecute() (the render condition).
inline void Issued(u64 vs, u64 fs, u32 kind, u64 count, u32 instances, bool runs) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Rec* const rec = Find(s, vs, fs);
    Frame& f = s.current;
    if (kind == 2) {
        ++f.clears;
        if (rec) {
            ++rec->clears;
        }
        return;
    }
    if (kind == 3) {
        ++f.issued_indirect;
        if (rec) {
            ++rec->issued_indirect;
            rec->indirect_max += count;
        }
    } else {
        ++f.issued;
        if (rec) {
            ++rec->issued;
            rec->prims += count * instances;
            rec->zero += (count == 0 || instances == 0) ? 1 : 0;
        }
    }
    if (!runs) {
        ++f.drops[static_cast<size_t>(Drop::Condition)];
        if (rec) {
            ++rec->drops[static_cast<size_t>(Drop::Condition)];
        }
    }
}

/// Rasterizer (GPU thread): the draw was recorded into the command buffer.
inline void Ran(u64 vs, u64 fs, bool indirect) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Rec* const rec = Find(s, vs, fs);
    if (indirect) {
        ++s.current.ran_indirect;
        if (rec) {
            ++rec->ran_indirect;
        }
    } else {
        ++s.current.ran;
        if (rec) {
            ++rec->ran;
        }
    }
}

/// Rasterizer (GPU thread): the draw did not reach the command buffer.
inline void Dropped(u64 vs, u64 fs, Drop reason) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Rec* const rec = Find(s, vs, fs);
    ++s.current.drops[static_cast<size_t>(reason)];
    if (rec) {
        ++rec->drops[static_cast<size_t>(reason)];
    }
}

/// Buffer cache (GPU thread): an indirect argument binding; gpu_modified = the buffer cache holds
/// the argument bytes as written by the GPU.
inline void IndirectArgs(bool gpu_modified) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    ++(gpu_modified ? s.current.args_gpu : s.current.args_cpu);
}

/// Buffer cache (GPU thread): a CPU upload of bytes made while binding indirect arguments or
/// counts (BufTrace kind Indirect).
inline void IndirectUpload(u64 bytes) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    ++s.current.args_uploads;
    s.current.args_upload_bytes += bytes;
}

/// Query cache (GPU thread): a query report value was published to guest memory.
inline void QueryPublished(u64 value) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    ++s.current.queries;
    s.current.queries_zero += value == 0 ? 1 : 0;
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
    s.index.clear();
    if (s.frames.size() > RING) {
        s.frames.pop_front();
    }
}

/// A run of frames [a, e] and the steady value around it (the smaller of the two sides' minima).
struct Dip {
    size_t a = 0;
    size_t e = 0;
    u64 lo = 0;
};

/// Steady: STEADY values from begin, all > 0, spread at most STEADY_PERMILLE of the maximum.
/// Returns the minimum, or 0 when not steady.
inline u64 SteadyMin(const std::vector<u64>& v, size_t begin) {
    u64 lo = v[begin], hi = v[begin];
    for (size_t k = 1; k < STEADY; ++k) {
        lo = (std::min)(lo, v[begin + k]);
        hi = (std::max)(hi, v[begin + k]);
    }
    if (lo == 0 || (hi - lo) * 1000 > hi * STEADY_PERMILLE) {
        return 0;
    }
    return lo;
}

/// Runs of 1-SHORT frames whose values are all below the steady minima of the STEADY frames
/// before and after.
inline std::vector<Dip> FindDips(const std::vector<u64>& v) {
    std::vector<Dip> dips;
    const size_t count = v.size();
    size_t a = STEADY;
    while (a + STEADY < count) {
        const u64 before = SteadyMin(v, a - STEADY);
        if (before == 0 || v[a] >= before) {
            ++a;
            continue;
        }
        size_t e = a;
        while (e + 1 < count && e + 1 - a < SHORT && v[e + 1] < before) {
            ++e;
        }
        if (e + STEADY >= count) {
            break;
        }
        const u64 after = SteadyMin(v, e + 1);
        const u64 lo = (std::min)(before, after);
        bool below = after != 0;
        for (size_t f = a; f <= e && below; ++f) {
            below = v[f] < lo;
        }
        if (below) {
            dips.push_back(Dip{a, e, lo});
            a = e + STEADY + 1;
        } else {
            a = e + 1;
        }
    }
    return dips;
}

/// Runs of 1-SHORT frames with a value > 0 between STEADY frames of 0 on each side.
inline u32 Appearances(const std::vector<u64>& v, std::vector<u32>& per_frame) {
    u32 total = 0;
    const size_t count = v.size();
    size_t a = STEADY;
    while (a + STEADY < count) {
        if (v[a] == 0) {
            ++a;
            continue;
        }
        size_t e = a;
        while (e + 1 < count && v[e + 1] != 0) {
            ++e;
        }
        if (e - a + 1 <= SHORT && e + STEADY < count) {
            bool zero = true;
            for (size_t k = 1; k <= STEADY && zero; ++k) {
                zero = v[a - k] == 0 && v[e + k] == 0;
            }
            if (zero) {
                ++total;
                for (size_t f = a; f <= e; ++f) {
                    ++per_frame[f];
                }
            }
        }
        a = e + 1;
    }
    return total;
}

inline std::string DropsText(const std::array<u32, DROPS>& d) {
    return fmt::format("{}/{}/{}/{}", d[0], d[1], d[2], d[3]);
}

/// PopTrace::Report: DRAW_* lines for the frames [first_tick, last_tick], rel to mark_tick.
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
        lines.push_back(fmt::format("DRAW_REPORT n={} frames=0", n));
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
    // Per pair: its record in each frame (nullptr when absent).
    ankerl::unordered_dense::map<u64, std::vector<const Rec*>> rows;
    for (size_t f = 0; f < count; ++f) {
        for (const Rec& r : window[f]->recs) {
            auto& row = rows[r.vs ^ (r.fs * 0x9E3779B97F4A7C15ULL)];
            if (row.empty()) {
                row.assign(count, nullptr);
            }
            row[f] = &r;
        }
    }
    std::vector<u32> dip_game(count), dip_drop(count), dip_prims(count), appear(count);
    std::vector<std::string> listed;
    u64 game_total = 0, drop_total = 0, prims_total = 0, appear_total = 0;
    std::vector<u64> ran(count), issued(count), prims(count);
    for (const auto& [key, row] : rows) {
        for (size_t f = 0; f < count; ++f) {
            const Rec* r = row[f];
            ran[f] = r ? u64{r->ran} + r->ran_indirect : 0;
            issued[f] = r ? u64{r->issued} + r->issued_indirect : 0;
            prims[f] = r ? r->prims : 0;
        }
        appear_total += Appearances(ran, appear);
        std::vector<Dip> ran_dips = FindDips(ran);
        for (const Dip& d : ran_dips) {
            const Rec* before = row[d.a - 1];
            // Issued fewer in every dip frame: the game. Otherwise the drops explain it.
            bool game = true;
            u64 issued_lo = issued[d.a - 1];
            for (size_t k = 1; k <= STEADY; ++k) {
                issued_lo = (std::min)(issued_lo, (std::min)(issued[d.a - k], issued[d.e + k]));
            }
            u64 ran_dip = ran[d.a], issued_dip = issued[d.a], prims_dip = prims[d.a];
            u64 indirect_dip = 0, zero_dip = 0;
            std::array<u32, DROPS> drops_dip{};
            for (size_t f = d.a; f <= d.e; ++f) {
                game = game && issued[f] < issued_lo;
                ran_dip = (std::min)(ran_dip, ran[f]);
                issued_dip = (std::min)(issued_dip, issued[f]);
                prims_dip = (std::min)(prims_dip, prims[f]);
                if (const Rec* r = row[f]) {
                    indirect_dip += r->issued_indirect;
                    zero_dip += r->zero;
                    for (size_t i = 0; i < DROPS; ++i) {
                        drops_dip[i] += r->drops[i];
                    }
                }
            }
            for (size_t f = d.a; f <= d.e; ++f) {
                ++(game ? dip_game : dip_drop)[f];
            }
            ++(game ? game_total : drop_total);
            if (listed.size() < MAX_LISTED) {
                listed.push_back(fmt::format(
                    "DRAW_DIP n={} kind={} rel={}..{} vs={:#x} fs={:#x} ran_before={} ran_dip={} "
                    "issued_before={} issued_dip={} prims_before={} prims_dip={} "
                    "indirect_before={} indirect_dip={} drops_before={} drops_dip={} "
                    "zero_before={} zero_dip={}",
                    n, game ? "game" : "dropped", rel(d.a), rel(d.e), before->vs, before->fs,
                    ran[d.a - 1], ran_dip, issued[d.a - 1], issued_dip, prims[d.a - 1], prims_dip,
                    before->issued_indirect, indirect_dip, DropsText(before->drops),
                    DropsText(drops_dip), before->zero, zero_dip));
            }
        }
        // The same draws with less geometry, where the draw count itself did not dip.
        for (const Dip& d : FindDips(prims)) {
            bool inside_ran_dip = false;
            for (const Dip& rd : ran_dips) {
                inside_ran_dip = inside_ran_dip || (d.a <= rd.e && rd.a <= d.e);
            }
            if (inside_ran_dip) {
                continue;
            }
            const Rec* before = row[d.a - 1];
            u64 prims_dip = prims[d.a], zero_dip = 0;
            for (size_t f = d.a; f <= d.e; ++f) {
                prims_dip = (std::min)(prims_dip, prims[f]);
                zero_dip += row[f] ? row[f]->zero : 0;
                ++dip_prims[f];
            }
            ++prims_total;
            if (listed.size() < MAX_LISTED) {
                listed.push_back(fmt::format(
                    "DRAW_DIP n={} kind=prims rel={}..{} vs={:#x} fs={:#x} ran_before={} ran_dip={} "
                    "issued_before={} issued_dip={} prims_before={} prims_dip={} "
                    "indirect_before={} indirect_dip={} drops_before={} drops_dip={} "
                    "zero_before={} zero_dip={}",
                    n, rel(d.a), rel(d.e), before->vs, before->fs, ran[d.a - 1], ran[d.a],
                    issued[d.a - 1], issued[d.a], prims[d.a - 1], prims_dip,
                    before->issued_indirect, before->issued_indirect, DropsText(before->drops),
                    DropsText(before->drops), before->zero, zero_dip));
            }
        }
    }
    u64 issued_sum = 0, ran_sum = 0;
    for (const Frame* frame : window) {
        issued_sum += u64{frame->issued} + frame->issued_indirect;
        ran_sum += u64{frame->ran} + frame->ran_indirect;
    }
    lines.push_back(fmt::format(
        "DRAW_REPORT n={} frames={} mark_tick={} issued_avg={} ran_avg={} keys={} dips_game={} "
        "dips_dropped={} dips_prims={} appear={} listed={}",
        n, count, window[mark]->tick, issued_sum / count, ran_sum / count, rows.size(),
        game_total, drop_total, prims_total, appear_total, listed.size()));
    for (size_t f = 0; f < count; ++f) {
        const Frame& frame = *window[f];
        lines.push_back(fmt::format(
            "DRAW_FRAME n={} rel={} tick={} t_ms={} issued={} indirect={} clears={} ran={} "
            "ran_indirect={} drops={} args_gpu={} args_cpu={} args_uploads={} "
            "args_upload_bytes={} queries={} queries_zero={} dropped_keys={} dip_game={} "
            "dip_dropped={} dip_prims={} appear={}",
            n, rel(f), frame.tick, frame.t_ms, frame.issued, frame.issued_indirect, frame.clears,
            frame.ran, frame.ran_indirect, DropsText(frame.drops), frame.args_gpu, frame.args_cpu,
            frame.args_uploads, frame.args_upload_bytes, frame.queries, frame.queries_zero,
            frame.dropped_keys, dip_game[f], dip_drop[f], dip_prims[f], appear[f]));
    }
    for (std::string& line : listed) {
        lines.push_back(std::move(line));
    }
    lines.push_back(fmt::format("DRAW_REPORT end n={}", n));
}

} // namespace VideoCommon::DrawPop
#endif
