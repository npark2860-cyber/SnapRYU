// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Diagnostic provenance only. Never changes a predicate, query, wait or submission.
// Completed query metadata is retained separately from pending_producers. A retained
// report or matching write is a candidate writer, not proof that native CPU stores
// did not subsequently change the bytes.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "common/guest_write_watch.h"
#include "common/logging.h"

namespace VideoCommon::PredicateTrace {
namespace GW = Common::GuestWriteWatch;
inline bool Enabled() {
#if SNAPRYU_DIAGNOSTICS
    static const bool enabled = [] {
        const char* p = std::getenv("EDEN_PREDICATE_TRACE");
        return p && p[0] == '1' && p[1] == '\0';
    }();
    return enabled;
#else
    return false;
#endif
}
inline constexpr u64 UNKNOWN_DRAM = ~u64{};
inline constexpr size_t REPORTS = 32768;
inline constexpr size_t MAX_KEYS = 16384;
inline constexpr size_t MAX_DRAWS = 8;
inline constexpr size_t DRAW_RING = 16384;

struct Report {
    u64 sequence{}, gpu{}, dram{UNKNOWN_DRAM}, tick{}, query_id{};
    u64 raw{}, amendment{}, value{};
    s64 request_ns{}, published_ns{};
    u32 type{}, bytes{}, payload{}, flags{}, bank{}, banks{}, slot{}, slots{};
    u64 callbacks{}, semiflushes{};
    s64 last_callback_ns{};
    bool published{};
    u32 publish_site{};
    u64 engine{}, epoch{}, first_draw{}, last_draw{}, zpass_attempts{}, zpass_skipped{};
};
struct QueryWrite {
    u64 sequence{}, latest_exact_request{}, gpu{}, query_id{}, value{};
    s64 request_ns{}, last_callback_ns{};
    u64 callbacks{}, semiflushes{};
    u32 type{}, flags{}, bytes{}, overlap_mask{};
    bool metadata{};
    std::array<u32, 4> before{}, after{};
};
struct ReadEvidence {
    bool valid{};
    u64 decision{}, gpu{}, frame{};
    u32 valid_mask{}, buffer_visits{}, query_visits{}, write_count{}, dropped{};
    std::array<u32, 4> before_buffer{}, after_buffer{}, before_query{}, after_query{}, used{};
    std::array<QueryWrite, 4> writes{};
};
struct Draw {
    u64 id{}, condition{}, program{}, vs_pc{}, fs_pc{}, rt{}, depth{}, vs{}, fs{};
    u32 kind{};
    u64 count{};
    u32 instances{}, width{}, height{}, format{}, zpass{}, rt_count{};
    bool execute{}, hashes{};
    u64 engine{};
};
struct Decision {
    u64 id{}, engine{}, gpu{}, frame{};
    std::array<u64, 4> dram{UNKNOWN_DRAM, UNKNOWN_DRAM, UNKNOWN_DRAM, UNKNOWN_DRAM};
    std::array<u32, 4> words{};
    u32 mode{};
    u8 result{2};
};
struct Key {
    u64 engine{}, gpu{};
    u32 mode{}, occurrence{};
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        return static_cast<size_t>(GW::Mix(GW::Mix(k.engine, k.gpu),
                                           (u64{k.mode} << 32) | k.occurrence));
    }
};
struct History {
    Decision previous{};
    std::array<Draw, MAX_DRAWS> draws{};
    u32 draw_count{}, stable{};
    u64 burst{}, false_frame{};
    bool pending_return{};
};
struct Active {
    Key key{};
    Decision decision{};
    const char* phase{};
    u64 burst{};
    bool valid{};
};
struct Store {
    std::mutex reports_mutex;
    std::unordered_map<const void*, u64> query_sequences;
    std::unordered_map<u64, u64> latest_exact_requests;
    ReadEvidence last_read{}; // GPU command-processing thread only; never historical.
    u64 frame_draws{}, frame_skipped{}, frame_vertices{}, frame_dispatches{};
    s64 frame_start_ns{}, frame_log_ns{};
    std::vector<Report> reports = std::vector<Report>(REPORTS);
    // The following state belongs exclusively to the GPU command-processing thread.
    std::unordered_map<Key, History, KeyHash> histories;
    std::unordered_map<u64, u32> uses;
    std::unordered_map<u64, Active> active;
    struct Counter {
        u64 epoch{}, first_draw{}, attempts{}, skipped{};
    };
    std::unordered_map<u64, Counter> counters;
    std::vector<Draw> draw_ring = std::vector<Draw>(DRAW_RING);
    u64 frame{}, next_condition{}, next_draw{}, next_burst{};
    s64 window_ns{};
    u32 bursts_in_window{};
    u64 suppressed{}, evicted{}, published_missing{};
    Key last_draw_key{};
    u32 last_draw_index{};
    u64 last_draw_id{}, last_draw_engine{};
};
inline Store& Data() {
    static Store* data = new Store; // diagnostics may outlive renderer shutdown
    return *data;
}
inline void ResetCounter(const void* engine) {
    if (!Enabled()) return;
    auto& d = Data();
    auto& c = d.counters[reinterpret_cast<uintptr_t>(engine)];
    ++c.epoch; c.first_draw = d.next_draw + 1; c.attempts = c.skipped = 0;
}
inline void CounterSnapshot(Report& report, const void* engine) {
    if (!Enabled()) return;
    auto& d = Data();
    report.engine = reinterpret_cast<uintptr_t>(engine);
    const auto& c = d.counters[report.engine];
    report.epoch = c.epoch; report.first_draw = c.first_draw;
    report.last_draw = d.next_draw;
    report.zpass_attempts = c.attempts; report.zpass_skipped = c.skipped;
}
inline std::atomic<u64> report_sequence{0};
inline u64 NextReport() {
    return Enabled() ? report_sequence.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
}
inline void Request(Report report, const void* host, const void* query = nullptr) {
    if (!Enabled()) return;
    if (!GW::DramOffset(host, report.dram)) report.dram = UNKNOWN_DRAM;
    report.request_ns = GW::NowNs();
    auto& d = Data();
    std::scoped_lock lock{d.reports_mutex};
    d.reports[report.sequence % REPORTS] = report;
    if (report.dram != UNKNOWN_DRAM) {
        if (d.latest_exact_requests.size() >= REPORTS && !d.latest_exact_requests.contains(report.dram))
            d.latest_exact_requests.clear();
        d.latest_exact_requests[report.dram] = report.sequence;
    }
    if (query) {
        if (d.query_sequences.size() >= REPORTS && !d.query_sequences.contains(query))
            d.query_sequences.clear();
        d.query_sequences[query] = report.sequence;
    }
}
inline void Publish(u64 sequence, u64 raw, u64 amendment, u64 value, u32 flags, u32 site = 0) {
    if (!Enabled()) return;
    auto& d = Data();
    std::scoped_lock lock{d.reports_mutex};
    auto& r = d.reports[sequence % REPORTS];
    if (r.sequence != sequence) { ++d.published_missing; return; }
    r.raw = raw; r.amendment = amendment; r.value = value;
    r.flags = flags; r.published_ns = GW::NowNs(); r.published = true; r.publish_site = site;
    if (site == 0) { ++r.callbacks; r.last_callback_ns = r.published_ns; }
    if (site == 2) ++r.semiflushes;
}
inline QueryWrite QueryWriteMetadata(const void* query) {
    QueryWrite result{};
    if (!Enabled()) return result;
    auto& d = Data();
    std::scoped_lock lock{d.reports_mutex};
    const auto it = d.query_sequences.find(query);
    if (it == d.query_sequences.end()) return result;
    const auto& r = d.reports[it->second % REPORTS];
    if (r.sequence != it->second) return result;
    result.sequence = r.sequence; result.gpu = r.gpu; result.query_id = r.query_id;
    result.type = r.type; result.value = r.value; result.request_ns = r.request_ns;
    result.callbacks = r.callbacks; result.semiflushes = r.semiflushes;
    result.last_callback_ns = r.last_callback_ns; result.metadata = true;
    const auto latest = d.latest_exact_requests.find(r.dram);
    if (latest != d.latest_exact_requests.end()) result.latest_exact_request = latest->second;
    return result;
}
inline void PublishByQuery(const void* query, u64 value, u32 flags) {
    if (!Enabled()) return;
    u64 sequence{};
    {
        auto& d = Data(); std::scoped_lock lock{d.reports_mutex};
        const auto it = d.query_sequences.find(query);
        if (it == d.query_sequences.end()) return;
        sequence = it->second;
    }
    Publish(sequence, value, 0, value, flags, 2);
}
inline void Dispatch() { if (Enabled()) ++Data().frame_dispatches; }
inline bool Overlaps(u64 a, u64 size, u64 b, u64 bsize) {
    return a != UNKNOWN_DRAM && b != UNKNOWN_DRAM && a < b + bsize && b < a + size;
}
inline void PrintDraw(u64 burst, const char* phase, const Draw& draw) {
    LOG_INFO(HW_GPU, "PRED_DRAW burst={} phase={} decision={} draw={} kind={} execute={} "
             "count={} instances={} program=0x{:x} vs_pc=0x{:x} fs_pc=0x{:x} "
             "rt=0x{:x} size={}x{} format={} rt_count={} depth=0x{:x} zpass={} "
             "hashes={} vs={:016x} fs={:016x}",
             burst, phase, draw.condition, draw.id, draw.kind, draw.execute, draw.count,
             draw.instances, draw.program, draw.vs_pc, draw.fs_pc, draw.rt, draw.width,
             draw.height, draw.format, draw.rt_count, draw.depth, draw.zpass,
             draw.hashes, draw.vs, draw.fs);
}
inline void PrintReadEvidence(const Decision& c, u64 burst, const char* phase) {
    const auto& read = Data().last_read;
    if (!read.valid || read.decision != c.id || read.gpu != c.gpu || read.frame != c.frame) {
        LOG_INFO(HW_GPU, "PRED_READ burst={} phase={} decision={} matched=false", burst, phase, c.id);
        return;
    }
    LOG_INFO(HW_GPU, "PRED_READ burst={} phase={} decision={} matched=true mask=0x{:x} "
             "buffer_visits={} query_visits={} writes={} dropped={} "
             "before_buffer={:08x},{:08x},{:08x},{:08x} after_buffer={:08x},{:08x},{:08x},{:08x} "
             "before_query={:08x},{:08x},{:08x},{:08x} after_query={:08x},{:08x},{:08x},{:08x} "
             "used={:08x},{:08x},{:08x},{:08x}",
             burst,phase,c.id,read.valid_mask,read.buffer_visits,read.query_visits,read.write_count,read.dropped,
             read.before_buffer[0],read.before_buffer[1],read.before_buffer[2],read.before_buffer[3],
             read.after_buffer[0],read.after_buffer[1],read.after_buffer[2],read.after_buffer[3],
             read.before_query[0],read.before_query[1],read.before_query[2],read.before_query[3],
             read.after_query[0],read.after_query[1],read.after_query[2],read.after_query[3],
             read.used[0],read.used[1],read.used[2],read.used[3]);
    for (u32 i = 0; i < read.write_count; ++i) {
        const auto& w = read.writes[i];
        LOG_INFO(HW_GPU, "PRED_QUERY_WRITE burst={} phase={} decision={} index={} "
                 "site=SemiFlush metadata={} sequence={} latest_exact={} gpu=0x{:x} query={} type={} "
                 "flags=0x{:x} bytes={} overlap=0x{:x} value={} callbacks_before={} semiflushes_before={} "
                 "request_ns={} last_callback_ns={} "
                 "before={:08x},{:08x},{:08x},{:08x} after={:08x},{:08x},{:08x},{:08x}",
                 burst,phase,c.id,i,w.metadata,w.sequence,w.latest_exact_request,w.gpu,w.query_id,w.type,
                 w.flags,w.bytes,w.overlap_mask,w.value,w.callbacks,w.semiflushes,w.request_ns,w.last_callback_ns,
                 w.before[0],w.before[1],w.before[2],w.before[3],w.after[0],w.after[1],w.after[2],w.after[3]);
    }
}
inline void Evidence(const Decision& c, u64 burst, const char* phase) {
    PrintReadEvidence(c, burst, phase);
    LOG_INFO(HW_GPU, "PRED_DECISION burst={} phase={} id={} engine=0x{:x} frame={} "
             "gpu=0x{:x} mode={} result={} words={:08x},{:08x},{:08x},{:08x} "
             "dram={:x},{:x},{:x},{:x} write_watch={}",
             burst, phase, c.id, c.engine, c.frame, c.gpu, c.mode, c.result,
             c.words[0], c.words[1], c.words[2], c.words[3],
             c.dram[0], c.dram[1], c.dram[2], c.dram[3], GW::Enabled());
    std::vector<Report> candidates;
    u64 missing{};
    {
        auto& d = Data();
        std::scoped_lock lock{d.reports_mutex};
        missing = d.published_missing;
        for (const auto& r : d.reports) {
            if (!r.sequence) continue;
            if (std::any_of(c.dram.begin(), c.dram.end(), [&](u64 offset) {
                    return Overlaps(r.dram, r.bytes, offset, 4);
                })) candidates.push_back(r);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Report& a, const Report& b) { return a.sequence < b.sequence; });
    const size_t begin = candidates.size() > 12 ? candidates.size() - 12 : 0;
    for (size_t i = begin; i < candidates.size(); ++i) {
        const auto& r = candidates[i];
        LOG_INFO(HW_GPU, "PRED_REPORT burst={} phase={} decision={} sequence={} "
                 "gpu=0x{:x} dram=0x{:x} bytes={} type={} payload={} query={} tick={} "
                 "bank={}:{} slot={}:{} published={} raw={} amend={} value={} flags=0x{:x} "
                 "request_ns={} published_ns={} publish_site={}",
                 burst, phase, c.id, r.sequence, r.gpu, r.dram, r.bytes, r.type, r.payload,
                 r.query_id, r.tick, r.bank, r.banks, r.slot, r.slots,
                 r.published, r.raw, r.amendment, r.value, r.flags,
                 r.request_ns, r.published_ns, r.publish_site);
        if (r.type == 21) {
            auto& d = Data();
            const u64 oldest = d.next_draw >= DRAW_RING ? d.next_draw - DRAW_RING + 1 : 1;
            LOG_INFO(HW_GPU, "PRED_COUNTER burst={} sequence={} epoch={} first_draw={} "
                     "last_draw={} zpass_attempts={} zpass_skipped={} ring_oldest={}",
                     burst, r.sequence, r.epoch, r.first_draw, r.last_draw,
                     r.zpass_attempts, r.zpass_skipped, oldest);
            u32 printed = 0;
            // Most recent candidates first. A report range can include more draws
            // than this bounded ring/print budget; absence never proves no draw.
            for (u64 id = r.last_draw; id >= (std::max)(oldest, r.first_draw) && id; --id) {
                const auto& draw = d.draw_ring[id % DRAW_RING];
                if (draw.id != id || draw.engine != r.engine || !draw.zpass) continue;
                if (printed++ == MAX_DRAWS) break;
                LOG_INFO(HW_GPU, "PRED_COUNTER_DRAW burst={} sequence={} draw={}",
                         burst, r.sequence, draw.id);
                PrintDraw(burst, "counter_candidate", draw);
            }
        }
    }
    std::array<std::pair<u64, u64>, 4> ranges{};
    for (size_t i = 0; i < ranges.size(); ++i)
        ranges[i] = {c.dram[i] == UNKNOWN_DRAM ? 0 : c.dram[i],
                     c.dram[i] == UNKNOWN_DRAM ? 0 : 4};
    GW::RingStats stats{};
    const auto writes = GW::FindOverlappingMany(ranges, 8, stats);
    for (size_t word = 0; word < writes.size(); ++word) {
        if (c.dram[word] == UNKNOWN_DRAM) continue;
        for (const auto& w : writes[word]) {
            LOG_INFO(HW_GPU, "PRED_WRITE burst={} phase={} decision={} word={} record={} "
                     "dram=0x{:x} size={} site={} tag={} first_bytes=0x{:x} "
                     "extra0=0x{:x} extra1={} begin_ns={} end_ns={}",
                     burst, phase, c.id, word, w.number, w.dram, w.size,
                     GW::SITE_NAMES[static_cast<size_t>(w.site)],
                     GW::TAG_NAMES[static_cast<size_t>(w.tag)], w.value,
                     w.extra0, w.extra1, w.begin_ns, w.end_ns);
        }
    }
    LOG_INFO(HW_GPU, "PRED_COVERAGE burst={} phase={} reports={} report_limit={} "
             "publish_metadata_misses={} writes_oldest={} writes_total={} torn={} "
             "native_stores=untracked absence_is_not_exclusion=true",
             burst, phase, candidates.size(), REPORTS, missing,
             stats.oldest, stats.total, stats.torn);
}
inline void Begin(const void* engine) {
    if (!Enabled()) return;
    auto& d = Data();
    d.active[reinterpret_cast<uintptr_t>(engine)].valid = false;
    d.last_read = {};
    d.last_draw_id = 0;
}
inline void Frame() {
    if (!Enabled()) return;
    auto& d = Data();
    ++d.frame; d.uses.clear();
    const s64 now = GW::NowNs();
    const s64 elapsed = d.frame_start_ns ? now - d.frame_start_ns : 0;
    if (now - d.frame_log_ns >= 1'000'000'000LL || elapsed >= 80'000'000LL) {
        LOG_INFO(HW_GPU, "LIGHT_FRAME frame={} cpu_frame_interval_ms={:.3f} draws={} skipped={} "
                 "vertices={} dispatches={} gpu_time=unmeasured",
                 d.frame, static_cast<double>(elapsed) / 1e6, d.frame_draws,
                 d.frame_skipped, d.frame_vertices, d.frame_dispatches);
        d.frame_log_ns = now;
    }
    d.frame_start_ns = now;
    d.frame_draws = d.frame_skipped = d.frame_vertices = d.frame_dispatches = 0;
}
inline void Condition(const void* engine, u64 gpu, u32 mode, u8 result,
                      const std::array<u32, 4>& words,
                      const std::array<const void*, 4>& hosts) {
    if (!Enabled()) return;
    auto& d = Data();
    const u64 owner = reinterpret_cast<uintptr_t>(engine);
    Decision c{.id = ++d.next_condition, .engine = owner, .gpu = gpu, .frame = d.frame,
               .words = words, .mode = mode, .result = result};
    for (size_t i = 0; i < hosts.size(); ++i)
        if (!GW::DramOffset(hosts[i], c.dram[i])) c.dram[i] = UNKNOWN_DRAM;
    // Engine and mode are part of the identity; occurrence is only a matching heuristic.
    const u64 use_key = GW::Mix(owner, gpu);
    const Key key{owner, gpu, mode, d.uses[use_key]++};
    if (d.histories.size() >= MAX_KEYS && !d.histories.contains(key)) {
        d.evicted += d.histories.size(); d.histories.clear();
        LOG_INFO(HW_GPU, "PRED_EVICT count={}", d.evicted);
    }
    if (d.uses.size() > MAX_KEYS * 2) d.uses.clear();
    auto& history = d.histories[key];
    Active active{.key = key, .decision = c, .valid = true};
    const s64 now = GW::NowNs();
    if (now - d.window_ns > 60'000'000'000LL) {
        d.window_ns = now; d.bursts_in_window = 0;
    }
    if (result == 0 && history.previous.result == 1 && history.stable >= 3) {
        if (d.bursts_in_window < 16) {
            ++d.bursts_in_window;
            active.burst = history.burst = ++d.next_burst;
            active.phase = "zero";
            LOG_INFO(HW_GPU, "PRED_BURST id={} stable={} occurrence={} suppressed={}",
                     active.burst, history.stable, key.occurrence, d.suppressed);
            // Only decision/draw facts were captured at the before time. Do not
            // mislabel a current writer-ring scan as a historical snapshot.
            const auto& p = history.previous;
            LOG_INFO(HW_GPU, "PRED_BEFORE burst={} decision={} frame={} result={} "
                     "words={:08x},{:08x},{:08x},{:08x} draws={} stored={}",
                     active.burst, p.id, p.frame, p.result, p.words[0], p.words[1],
                     p.words[2], p.words[3], history.draw_count,
                     (std::min)(history.draw_count, static_cast<u32>(MAX_DRAWS)));
            for (u32 i = 0; i < (std::min)(history.draw_count, static_cast<u32>(MAX_DRAWS)); ++i)
                PrintDraw(active.burst, "before", history.draws[i]);
            Evidence(c, active.burst, "zero");
            history.pending_return = true; history.false_frame = c.frame;
        } else ++d.suppressed;
    } else if (history.pending_return) {
        if (c.frame - history.false_frame <= 3) {
            active.burst = history.burst;
            active.phase = result == 1 ? "return" : "still_zero";
            Evidence(c, active.burst, active.phase);
        }
        if (result == 1 || c.frame - history.false_frame > 3)
            history.pending_return = false;
    }
    history.stable = result == 1 ? (history.previous.result == 1 ? history.stable + 1 : 1) : 0;
    history.previous = c; history.draw_count = 0;
    d.active[owner] = active;
}
inline void NoteDraw(const void* engine, Draw draw) {
    if (!Enabled()) return;
    auto& d = Data();
    const u64 owner = reinterpret_cast<uintptr_t>(engine);
    draw.id = ++d.next_draw; draw.engine = owner;
    if (draw.execute) {
        ++d.frame_draws;
        if (draw.kind <= 1) d.frame_vertices += draw.count * draw.instances;
    } else ++d.frame_skipped;
    d.last_draw_id = draw.id; d.last_draw_engine = owner;
    if (draw.zpass) {
        auto& counter = d.counters[owner];
        ++counter.attempts; counter.skipped += draw.execute ? 0 : 1;
    }
    d.draw_ring[draw.id % DRAW_RING] = draw;
    const auto it = d.active.find(owner);
    if (it == d.active.end() || !it->second.valid) return;
    const auto& a = it->second;
    const auto h = d.histories.find(a.key);
    if (h == d.histories.end()) return;
    draw.condition = a.decision.id;
    d.draw_ring[draw.id % DRAW_RING] = draw;
    const u32 index = h->second.draw_count++;
    if (index < MAX_DRAWS) {
        h->second.draws[index] = draw;
        d.last_draw_key = a.key; d.last_draw_index = index;
        d.last_draw_id = draw.id; d.last_draw_engine = owner;
        if (a.burst) PrintDraw(a.burst, a.phase, draw);
    } else if (index == MAX_DRAWS && a.burst) {
        LOG_INFO(HW_GPU, "PRED_DRAW_LIMIT burst={} decision={} first_omitted={}",
                 a.burst, a.decision.id, draw.id);
    }
}
inline void Pipeline(const void* engine, u64 vs, u64 fs) {
    if (!Enabled()) return;
    auto& d = Data();
    if (!d.last_draw_id || d.last_draw_engine != reinterpret_cast<uintptr_t>(engine)) return;
    auto& ring_draw = d.draw_ring[d.last_draw_id % DRAW_RING];
    if (ring_draw.id == d.last_draw_id) {
        ring_draw.vs = vs; ring_draw.fs = fs; ring_draw.hashes = true;
    }
    auto h = d.histories.find(d.last_draw_key);
    if (h == d.histories.end()) return;
    auto& draw = h->second.draws[d.last_draw_index];
    if (draw.id != d.last_draw_id) return;
    draw.vs = vs; draw.fs = fs; draw.hashes = true;
    const auto a = d.active.find(d.last_draw_engine);
    if (a != d.active.end() && a->second.burst)
        LOG_INFO(HW_GPU, "PRED_PIPELINE burst={} decision={} draw={} vs={:016x} fs={:016x} "
                 "stage=configure_success worker_execution_unconfirmed",
                 a->second.burst, draw.condition, draw.id, vs, fs);
}
} // namespace VideoCommon::PredicateTrace

