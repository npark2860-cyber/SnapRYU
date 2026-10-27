// SPDX-FileCopyrightText: Copyright 2026 SnapRYU
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if SNAPRYU_DIAGNOSTICS
// b86 VIS_RING (EDEN_VIS_RING=1, diagnostic builds only).
//
// The b85 run could not say in which frames the lake water was gone: the user's F8 comes about
// 0.3 s late, some pops are missed (a blink) and the water does not vanish at every dive. VIS_RING
// takes the picture itself. Every Composite, RendererVulkan::VisCapture draws the guest
// framebuffers into a SRC_W x SRC_H frame of its own (a third BlitScreen, the applet capture
// filters) and downloads it into one of SLOTS host buffers; when the GPU is done with a download
// Add gets its pixels with the PopTrace tick of the frame and boxes them to a W x H RGB thumbnail.
//
// A thumbnail equal to the one before (mean difference under DUP) is a repeat. For every other one
// (f) the mean absolute difference per channel (0..255) to the 1..4 thumbnails before it is kept
// (d1..d4). A glitch is a run of 1..MAX_LEN thumbnails [a, e] that differs from both neighbours
// while the neighbours are alike: jump = min(D(a-1, a), D(e, f)) >= JUMP_MIN and
// jump >= SCORE_MIN * (D(a-1, f) + 0.5). A glitch at least GAP thumbnails after the last one asks
// PopTrace for a mark at the tick of a (why=vis: the usual report with the BIND_POP and DRAW_POP
// lines, rel=0 at the first glitch frame) and writes the thumbnails around it, at most MAX_DUMPS
// times. An F8 mark gets the thumbnails around it as well.
//
// log/vis_ring_<unix seconds>/ gets metrics.tsv (one row per thumbnail: seq, composite, tick,
// t_ms, dup, d1..d4), dumps.tsv (one row per written dump) and dump_<n>.bin ("VISR86\0\0", u32
// version, u32 width, u32 height, u32 frames, then per frame u64 seq, composite, tick, t_ms and
// width * height RGB bytes). VIS_RING lines in the log: ready, VIS_POP per glitch, VIS_DUMP per
// written dump, VIS_END at exit. Nothing here changes what is drawn or presented; the extra
// framebuffer draw and download run on the GPU thread every Composite while EDEN_VIS_RING=1.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/texture_cache/pop_trace.h"

namespace VideoCommon::VisRing {

inline constexpr u32 SRC_W = 320;          // the frame VisCapture draws
inline constexpr u32 SRC_H = 180;
inline constexpr u32 W = SRC_W / 2;        // the thumbnail (2 x 2 boxes of the frame)
inline constexpr u32 H = SRC_H / 2;
inline constexpr size_t SLOTS = 8;         // downloads in flight
inline constexpr size_t RING = 240;        // thumbnails kept
inline constexpr size_t BEFORE = 40;       // thumbnails written before a glitch
inline constexpr size_t KEY_BEFORE = 120;  // before an F8 mark (the press comes late)
inline constexpr size_t AFTER = 16;        // after either
inline constexpr size_t MAX_LEN = 3;       // glitch thumbnails at most
inline constexpr double JUMP_MIN = 2.0;
inline constexpr double SCORE_MIN = 1.6;
inline constexpr double DUP = 0.05;
inline constexpr size_t GAP = 45;          // thumbnails between two glitch marks
inline constexpr u32 MAX_DUMPS = 48;
inline constexpr size_t FLUSH_ROWS = 30;   // metrics rows between flushes

struct Thumb {
    u64 seq = 0;
    u64 composite = 0;
    u64 tick = 0;
    u64 t_ms = 0;
    std::array<double, 4> d{}; // D to the 1..4 thumbnails before (0 where there is none)
    std::vector<u8> rgb;
};

struct Dump {
    bool pending = false;
    u32 n = 0;
    const char* why = "";
    u64 mark_tick = 0;
    u64 from = 0;  // first seq written
    u64 until = 0; // last seq written (written once it arrived)
    std::string detail;
};

struct State {
    bool on = false;
    bool init = false;
    std::deque<Thumb> ring;
    u64 seq = 0;          // seq of the next thumbnail
    u64 frames = 0;       // Add calls
    u64 repeats = 0;
    u64 dropped = 0;      // Composites without a free slot
    u64 glitches = 0;     // glitches found (marked or not)
    u64 last_mark = 0;    // seq + 1 of the last glitch mark
    u32 dumps = 0;
    u64 key_marks = 0;    // PopTrace::key_marks seen
    u64 key_tick = 0;     // an F8 mark waiting for its thumbnail, 0 = none
    Dump dump;
    std::filesystem::path dir;
    std::ofstream metrics;
    std::ofstream dumps_tsv;
    size_t rows = 0;
};

inline State& Get() {
    static State s;
    if (!s.init) [[unlikely]] {
        s.init = true;
        const char* const env = std::getenv("EDEN_VIS_RING");
        s.on = env != nullptr && env[0] == '1' && env[1] == '\0' && PopTrace::Active();
        if (s.on) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
            s.dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                    fmt::format("vis_ring_{}", seconds);
            std::error_code error;
            std::filesystem::create_directories(s.dir, error);
            s.metrics.open(s.dir / "metrics.tsv", std::ios::binary);
            s.dumps_tsv.open(s.dir / "dumps.tsv", std::ios::binary);
            s.metrics << "seq\tcomposite\ttick\tt_ms\tdup\td1\td2\td3\td4\n";
            s.dumps_tsv << "n\twhy\tmark_tick\tfrom_seq\tuntil_seq\tframes\tdetail\n";
            s.on = s.metrics.good() && s.dumps_tsv.good();
        }
        LOG_WARNING(Render_Vulkan,
                    "VIS_RING ready b86 on={} src={}x{} thumb={}x{} slots={} ring={} before={} "
                    "key_before={} after={} max_len={} jump_min={} score_min={} dup={} gap={} "
                    "max_dumps={} dir={}",
                    s.on, SRC_W, SRC_H, W, H, SLOTS, RING, BEFORE, KEY_BEFORE, AFTER, MAX_LEN,
                    JUMP_MIN, SCORE_MIN, DUP, GAP, MAX_DUMPS, Common::FS::PathToUTF8String(s.dir));
    }
    return s;
}

inline bool Active() {
    return Get().on;
}

/// Mean absolute difference per channel of two thumbnails (0..255).
inline double Diff(const std::vector<u8>& x, const std::vector<u8>& y) {
    u64 sum = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        sum += static_cast<u64>(x[i] > y[i] ? x[i] - y[i] : y[i] - x[i]);
    }
    return x.empty() ? 0.0 : static_cast<double>(sum) / static_cast<double>(x.size());
}

/// The thumbnail with this seq, nullptr when it left the ring (or has not come yet).
inline const Thumb* At(const State& s, u64 seq) {
    if (s.ring.empty() || seq < s.ring.front().seq || seq > s.ring.back().seq) {
        return nullptr;
    }
    return &s.ring[static_cast<size_t>(seq - s.ring.front().seq)];
}

inline void WriteU32(std::ofstream& file, u32 v) {
    file.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

inline void WriteU64(std::ofstream& file, u64 v) {
    file.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

/// Writes the pending dump (the thumbnails from .. until still in the ring).
inline void WriteDump(State& s) {
    Dump& d = s.dump;
    d.pending = false;
    const u64 from = (std::max)(d.from, s.ring.front().seq);
    const u64 until = (std::min)(d.until, s.ring.back().seq);
    const u32 count = until >= from ? static_cast<u32>(until - from + 1) : 0;
    const std::filesystem::path path = s.dir / fmt::format("dump_{}.bin", d.n);
    std::ofstream file(path, std::ios::binary);
    file.write("VISR86\0\0", 8);
    WriteU32(file, 1);
    WriteU32(file, W);
    WriteU32(file, H);
    WriteU32(file, count);
    for (u64 q = from; count != 0 && q <= until; ++q) {
        const Thumb* const t = At(s, q);
        WriteU64(file, t->seq);
        WriteU64(file, t->composite);
        WriteU64(file, t->tick);
        WriteU64(file, t->t_ms);
        file.write(reinterpret_cast<const char*>(t->rgb.data()),
                   static_cast<std::streamsize>(t->rgb.size()));
    }
    const bool ok = static_cast<bool>(file);
    s.dumps_tsv << fmt::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", d.n, d.why, d.mark_tick, from,
                               until, count, d.detail);
    s.dumps_tsv.flush();
    LOG_WARNING(Render_Vulkan, "VIS_DUMP n={} why={} mark_tick={} from_seq={} until_seq={} "
                               "frames={} ok={} file={}",
                d.n, d.why, d.mark_tick, from, until, count, ok,
                Common::FS::PathToUTF8String(path.filename()));
}

/// Starts a dump of the thumbnails [from, until] (one at a time; a second waits for no one).
inline bool StartDump(State& s, const char* why, u64 mark_tick, u64 from, u64 until,
                      std::string detail) {
    if (s.dump.pending || s.dumps >= MAX_DUMPS) {
        return false;
    }
    ++s.dumps;
    s.dump = Dump{true, s.dumps, why, mark_tick, from, until, std::move(detail)};
    return true;
}

/// GPU thread (RendererVulkan::VisCapture): a download of the frame drawn at that Composite is
/// done. pixels = SRC_W x SRC_H texels of 4 bytes (bgra: B, G, R, A order, else R, G, B, A).
inline void Add(std::span<const u8> pixels, bool bgra, u64 composite, u64 tick, u64 t_ms) {
    State& s = Get();
    if (!s.on || pixels.size() < static_cast<size_t>(SRC_W) * SRC_H * 4) {
        return;
    }
    ++s.frames;
    Thumb t;
    t.composite = composite;
    t.tick = tick;
    t.t_ms = t_ms;
    t.rgb.resize(static_cast<size_t>(W) * H * 3);
    const size_t r = bgra ? 2 : 0;
    const size_t b = bgra ? 0 : 2;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const u8* const p0 = pixels.data() + (static_cast<size_t>(2 * y) * SRC_W + 2 * x) * 4;
            const u8* const p1 = p0 + static_cast<size_t>(SRC_W) * 4;
            u8* const out = t.rgb.data() + (static_cast<size_t>(y) * W + x) * 3;
            const auto box = [&](size_t c) {
                return static_cast<u8>((p0[c] + p0[4 + c] + p1[c] + p1[4 + c] + 2) / 4);
            };
            out[0] = box(r);
            out[1] = box(1);
            out[2] = box(b);
        }
    }
    const double d1 = s.ring.empty() ? 1e9 : Diff(t.rgb, s.ring.back().rgb);
    if (d1 < DUP) {
        ++s.repeats;
        s.metrics << fmt::format("{}\t{}\t{}\t{}\t1\t{:.3f}\t\t\t\n", s.ring.back().seq,
                                 composite, tick, t_ms, d1);
    } else {
        t.seq = s.seq++;
        const size_t n = s.ring.size();
        for (size_t k = 1; k <= 4 && k <= n; ++k) {
            t.d[k - 1] = k == 1 ? d1 : Diff(t.rgb, s.ring[n - k].rgb);
        }
        s.metrics << fmt::format("{}\t{}\t{}\t{}\t0\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\n", t.seq,
                                 composite, tick, t_ms, t.d[0], t.d[1], t.d[2], t.d[3]);
        s.ring.push_back(std::move(t));
        if (s.ring.size() > RING) {
            s.ring.pop_front();
        }
        // A glitch [a, e] ending just before this thumbnail (f), the best one of 1..MAX_LEN.
        const size_t f = s.ring.size() - 1;
        const Thumb& tf = s.ring[f];
        size_t best_len = 0;
        double best_score = 0.0, best_jump = 0.0, best_base = 0.0;
        for (size_t len = 1; len <= MAX_LEN && len + 1 <= f; ++len) {
            const size_t a = f - len;
            const double jump = (std::min)(s.ring[a].d[0], tf.d[0]);
            const double base = tf.d[len];
            const double score = jump / (base + 0.5);
            if (jump >= JUMP_MIN && score >= SCORE_MIN && score > best_score) {
                best_len = len;
                best_score = score;
                best_jump = jump;
                best_base = base;
            }
        }
        if (best_len != 0) {
            ++s.glitches;
            const Thumb& ta = s.ring[f - best_len];
            const Thumb& te = s.ring[f - 1];
            const bool due = s.last_mark == 0 || tf.seq + 1 >= s.last_mark + GAP;
            bool marked = false;
            if (due && !s.dump.pending && s.dumps < MAX_DUMPS) {
                marked = PopTrace::RequestMark(ta.tick);
                if (marked) {
                    s.last_mark = tf.seq + 1;
                    StartDump(s, "vis", ta.tick, ta.seq >= BEFORE ? ta.seq - BEFORE : 0,
                              tf.seq + AFTER,
                              fmt::format("len={} seq_a={} seq_e={} tick_a={} tick_e={} "
                                          "jump={:.2f} base={:.2f} score={:.2f}",
                                          best_len, ta.seq, te.seq, ta.tick, te.tick, best_jump,
                                          best_base, best_score));
                }
            }
            LOG_WARNING(Render_Vulkan,
                        "VIS_POP n={} len={} seq_a={} seq_e={} tick_a={} tick_e={} t_ms_a={} "
                        "jump={:.2f} base={:.2f} score={:.2f} marked={}",
                        s.glitches, best_len, ta.seq, te.seq, ta.tick, te.tick, ta.t_ms,
                        best_jump, best_base, best_score, marked);
        }
    }
    // An F8 mark: the thumbnails around the first one of its frame or later.
    if (PopTrace::key_marks != s.key_marks) {
        s.key_marks = PopTrace::key_marks;
        s.key_tick = PopTrace::key_mark_tick;
    }
    if (s.key_tick != 0 && !s.ring.empty() && s.ring.back().tick >= s.key_tick) {
        u64 at = s.ring.back().seq;
        for (const Thumb& x : s.ring) {
            if (x.tick >= s.key_tick) {
                at = x.seq;
                break;
            }
        }
        if (StartDump(s, "F8", s.key_tick, at >= KEY_BEFORE ? at - KEY_BEFORE : 0, at + AFTER,
                      fmt::format("seq_mark={}", at)) ||
            s.dumps >= MAX_DUMPS) {
            s.key_tick = 0;
        }
    }
    if (s.dump.pending && !s.ring.empty() && s.ring.back().seq >= s.dump.until) {
        WriteDump(s);
    }
    if (++s.rows % FLUSH_ROWS == 0) {
        s.metrics.flush();
    }
}

/// GPU thread (RendererVulkan::VisCapture): every slot was still in flight at a Composite.
inline void Dropped() {
    ++Get().dropped;
}

/// RendererVulkan destructor: the totals, and the dump still waiting for its last thumbnails.
inline void End() {
    State& s = Get();
    if (!s.on) {
        return;
    }
    if (s.dump.pending && !s.ring.empty()) {
        WriteDump(s);
    }
    s.metrics.flush();
    LOG_WARNING(Render_Vulkan,
                "VIS_END frames={} thumbs={} repeats={} dropped={} glitches={} dumps={}", s.frames,
                s.seq, s.repeats, s.dropped, s.glitches, s.dumps);
}

} // namespace VideoCommon::VisRing
#endif
