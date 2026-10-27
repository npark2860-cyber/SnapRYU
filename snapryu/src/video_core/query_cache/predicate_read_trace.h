// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstring>
#include "video_core/query_cache/predicate_trace.h"

namespace VideoCommon::PredicateReadTrace {
using Words = std::array<u32, 4>;
enum class Stage { BeforeBuffer, AfterBuffer, BeforeQuery, AfterQuery };
#if SNAPRYU_DIAGNOSTICS
struct State {
    bool active{}, buffer_seen{}, query_seen{};
    u32 valid_mask{}, buffer_visits{}, query_visits{};
    u64 gpu{}, frame{};
    u32 write_count{}, writes_dropped{};
    std::array<PredicateTrace::QueryWrite, 4> writes{};
    std::array<const void*, 4> hosts{};
    Words begin{}, before_buffer{}, after_buffer{}, before_query{}, after_query{}, used{};
};
inline thread_local State current;
struct Counters {
    u64 reads{}, buffer_changed{}, query_changed{}, one_to_zero{}, printed{};
    u64 direct_writes{}, direct_changes{}, changes_after_callback{}, changes_after_semiflush{}, metadata_missing{};
};
inline thread_local Counters counts;
inline Words Snapshot() {
    Words words{};
    for (size_t i = 0; i < 4; ++i)
        if (current.valid_mask & (1U << i)) std::memcpy(&words[i], current.hosts[i], 4);
    return words;
}
inline bool Changed(const Words& a, const Words& b) {
    for (size_t i = 0; i < 4; ++i)
        if ((current.valid_mask & (1U << i)) && a[i] != b[i]) return true;
    return false;
}
inline void Observe(Stage stage) {
    if (!current.active) return;
    const auto words = Snapshot();
    switch (stage) {
    case Stage::BeforeBuffer: current.before_buffer = words; ++current.buffer_visits; break;
    case Stage::AfterBuffer: current.after_buffer = words; current.buffer_seen = true; break;
    case Stage::BeforeQuery: current.before_query = words; ++current.query_visits; break;
    case Stage::AfterQuery: current.after_query = words; current.query_seen = true; break;
    }
}
struct WriteToken { bool active{}; PredicateTrace::QueryWrite write{}; };
inline WriteToken BeforeQueryWrite(const void* query, const void* ptr, u32 bytes, u64 value, u32 flags) {
    WriteToken token{};
    if (!current.active) return token;
    u64 destination{};
    if (!Common::GuestWriteWatch::DramOffset(ptr, destination)) return token;
    u32 overlap{};
    for (size_t i=0; i<4; ++i) {
        if (!(current.valid_mask & (1U << i))) continue;
        u64 word{};
        if (Common::GuestWriteWatch::DramOffset(current.hosts[i], word) &&
            PredicateTrace::Overlaps(destination, bytes, word, 4)) overlap |= 1U << i;
    }
    if (!overlap) return token;
    token.active = true;
    token.write = PredicateTrace::QueryWriteMetadata(query);
    token.write.overlap_mask = overlap; token.write.bytes = bytes;
    token.write.value = value; token.write.flags = flags;
    token.write.before = Snapshot();
    return token;
}
inline void AfterQueryWrite(WriteToken token) {
    if (!token.active) return;
    token.write.after = Snapshot();
    ++counts.direct_writes;
    counts.metadata_missing += !token.write.metadata;
    bool changed = false;
    for (size_t i=0; i<4; ++i)
        if ((token.write.overlap_mask & (1U << i)) && token.write.before[i] != token.write.after[i])
            changed = true;
    counts.direct_changes += changed;
    counts.changes_after_callback += changed && token.write.callbacks != 0;
    counts.changes_after_semiflush += changed && token.write.semiflushes != 0;
    if (current.write_count < current.writes.size())
        current.writes[current.write_count++] = token.write;
    else ++current.writes_dropped;
}
class Scope {
public:
    Scope(u64 gpu, const std::array<const void*,4>& hosts) : previous{current} {
        if (!PredicateTrace::Enabled()) return;
        current = {}; current.active = true; current.gpu = gpu; current.hosts = hosts;
        current.frame = PredicateTrace::Data().frame;
        for (size_t i=0; i<4; ++i) {
            u64 offset{};
            if (Common::GuestWriteWatch::DramOffset(hosts[i], offset) &&
                offset + 4 <= Common::GuestWriteWatch::g_dram_size.load(std::memory_order_relaxed))
                current.valid_mask |= 1U << i;
        }
        current.begin = Snapshot();
    }
    ~Scope() { current = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Complete(const Words& used) {
        if (!current.active) return;
        current.used = used;
        auto& data = PredicateTrace::Data();
        data.last_read = {.valid=true, .decision=data.next_condition+1,
            .gpu=current.gpu, .frame=current.frame, .valid_mask=current.valid_mask,
            .buffer_visits=current.buffer_visits, .query_visits=current.query_visits,
            .write_count=current.write_count, .dropped=current.writes_dropped,
            .before_buffer=current.before_buffer, .after_buffer=current.after_buffer,
            .before_query=current.before_query, .after_query=current.after_query,
            .used=used, .writes=current.writes};
        ++counts.reads;
        const bool buffer_changed = current.buffer_seen && Changed(current.before_buffer, current.after_buffer);
        const bool query_changed = current.query_seen && Changed(current.before_query, current.after_query);
        counts.buffer_changed += buffer_changed;
        counts.query_changed += query_changed;
        bool one_to_zero = false;
        for (size_t i=0; i<4; ++i)
            if ((current.valid_mask & (1U << i)) && current.query_seen &&
                current.before_query[i] == 1 && current.after_query[i] == 0 && used[i] == 0)
                one_to_zero = true;
        counts.one_to_zero += one_to_zero;

    }
private:
    State previous;
};
inline void Frame() {
    if (!PredicateTrace::Enabled()) return;
    if (PredicateTrace::Data().frame % 30 == 0) {
        LOG_INFO(HW_GPU, "QUERY_WRITE_STATS direct_writes={} direct_changes={} changes_after_callback={} "
                 "changes_after_semiflush={} metadata_missing={} linked_detail=per_predicate_burst",
                 counts.direct_writes,counts.direct_changes,counts.changes_after_callback,
                 counts.changes_after_semiflush,counts.metadata_missing);
        LOG_INFO(HW_GPU, "READ_ORDER_STATS reads={} buffer_changed={} query_changed={} one_to_zero={} "
                 "printed={} timing_only_no_behavior_change=true", counts.reads,counts.buffer_changed,
                 counts.query_changed,counts.one_to_zero,counts.printed);
    }
}
#else
// Release never reads guest words or copies trace TLS state.
struct WriteToken {};
inline void Observe(Stage) {}
inline WriteToken BeforeQueryWrite(const void*, const void*, u32, u64, u32) { return {}; }
inline void AfterQueryWrite(WriteToken) {}
class Scope {
public:
    Scope(u64, const std::array<const void*, 4>&) noexcept {}
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Complete(const Words&) noexcept {}
};
inline void Frame() {}
#endif
} // namespace VideoCommon::PredicateReadTrace
