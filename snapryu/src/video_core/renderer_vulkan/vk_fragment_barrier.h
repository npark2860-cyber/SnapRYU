// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// b48: the b45/b46 deferral of guest FragmentBarrier and WaitForIdle inside safe render passes is
// release code, default on. EDEN_FRAGMENT_BARRIER_DEFER=0 or EDEN_WFI_DEFER=0 disables it (the
// b46 "inspect" launcher sets EDEN_WFI_DEFER=0). Periodic reports stay diagnostic-only.
#if SNAPRYU_DIAGNOSTICS
#include <array>
#endif
#include <chrono>
#include <cstdlib>
#include <span>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::FragmentBarrier {
// Default on since b48; "0" disables.
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_FRAGMENT_BARRIER_DEFER");
        const bool off = value && value[0] == '0' && value[1] == '\0';
        LOG_INFO(Render_Vulkan, "FRAGMENT_BARRIER_DEFER enabled={} default=on "
                 "policy=checked_non_feedback_draws extra_gpu_queries=0 extra_gpu_waits=0", !off);
        return !off;
    }();
    return enabled;
}

// Compare whole native images, deliberately rejecting even disjoint subresources.
// Null/texel-buffer/unknown inputs and uninspected helper draws must fall back.
template <typename Image>
bool SafeImages(std::span<const Image> attachments, std::span<const Image> inputs,
                bool checked_draw) {
    if (!checked_draw || attachments.empty()) return false;
    for (const auto attachment : attachments) {
        if (!attachment) return false;
        for (const auto input : inputs) {
            if (!input || input == attachment) return false;
        }
    }
    return true;
}

#if SNAPRYU_DIAGNOSTICS
// b47/b49: SafeImages without the attachment/input equality test. The written rule
// (EDEN_WFI_DEFER=2, diagnostic builds only) decides overlap per pass from what earlier draws
// wrote and sampled instead.
template <typename Image>
bool CheckedInputs(std::span<const Image> attachments, std::span<const Image> inputs,
                   bool checked_draw) {
    if (!checked_draw || attachments.empty()) return false;
    for (const auto attachment : attachments) {
        if (!attachment) return false;
    }
    for (const auto input : inputs) {
        if (!input) return false;
    }
    return true;
}
#endif

inline s64 NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// b46: why a request could not be deferred (b45 only counted "immediate").
struct ImmediateCounts {
    u64 no_pass{}, deferred_clear{}, unsafe_pass{}, full_barrier{};

    void Note(bool has_pass, bool has_deferred_clear, bool pass_safe, bool full_barriers) {
        if (!has_pass) {
            ++no_pass;
        } else if (has_deferred_clear) {
            ++deferred_clear;
        } else if (!pass_safe) {
            ++unsafe_pass;
        } else if (full_barriers) {
            ++full_barrier;
        }
    }
};

// Owned by one Scheduler, accessed only on its producer thread.
struct State {
    bool pending{}, pass_safe{true};
    u64 requests{}, deferred{}, coalesced{}, immediate{}, materialized{}, reused_draws{};
    u64 overlap_or_unknown{}, polls{}, reports{};
    ImmediateCounts why_immediate;
    s64 last_report_us{};

    bool Request(bool has_pass, bool has_deferred_clear, bool full_barriers) {
        ++requests;
        const bool eligible = has_pass && !has_deferred_clear && pass_safe && !full_barriers;
        if (!eligible) {
            ++immediate;
            why_immediate.Note(has_pass, has_deferred_clear, pass_safe, full_barriers);
            return false;
        }
        ++deferred;
        coalesced += pending;
        pending = true;
        return true;
    }
    void End() {
        materialized += pending;
        pending = false;
        pass_safe = true;
    }
    void MaybeReport() {
#if SNAPRYU_DIAGNOSTICS
        if (!Enabled() || (++polls & 255) != 0) return;
        const s64 now = NowUs();
        if (!last_report_us) { last_report_us = now; return; }
        if (now - last_report_us < 10'000'000) return;
        last_report_us = now;
        LOG_INFO(Render_Vulkan, "GPU_FRAGMENT_BARRIER report={} requests={} deferred={} "
                 "coalesced={} immediate={} materialized={} reused_draws={} "
                 "overlap_or_unknown={} pending={} immediate_no_pass={} "
                 "immediate_deferred_clear={} immediate_unsafe_pass={} "
                 "immediate_full_barrier={} cumulative_scheduler=true",
                 ++reports, requests, deferred, coalesced, immediate, materialized,
                 reused_draws, overlap_or_unknown, pending, why_immediate.no_pass,
                 why_immediate.deferred_clear, why_immediate.unsafe_pass,
                 why_immediate.full_barrier);
#endif
    }
};
} // namespace Vulkan::FragmentBarrier

// b46: guest WaitForIdle deferral. A guest WFI arriving inside a render pass whose draws are
// all "checked" (no storage/global writes, no texel/storage images, no XFB, no MSAA, no
// attachment sampled by any draw of the pass) is kept pending; the pass stays open for the next
// draw, which is subject to the same overlap check as the b45 FragmentBarrier deferral. The WFI's
// execution barrier (SetEvent/WaitEvents, no memory barrier, identical to the original
// RasterizerVulkan::WaitForIdle recording) is materialised right after the pass's attachment
// barriers when the pass ends for any reason: next overlapping/unchecked draw, framebuffer
// change, deferred clear, copy/compute/query sync (RequestOutsideRenderPassOperationContext),
// or submission. Requires EDEN_FRAGMENT_BARRIER_DEFER=1, which maintains State::pass_safe.
namespace Vulkan::WfiDefer {
// Default on since b48; "0" disables. Still requires FragmentBarrier::Enabled().
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_WFI_DEFER");
        const bool requested = !(value && value[0] == '0' && value[1] == '\0');
        const bool on = requested && FragmentBarrier::Enabled();
        LOG_INFO(Render_Vulkan, "WFI_DEFER requested={} enabled={} default=on "
                 "requires=EDEN_FRAGMENT_BARRIER_DEFER!=0 policy=same_safe_pass_rules "
                 "extra_gpu_queries=0 extra_gpu_waits=0", requested, on);
        return on;
    }();
    return enabled;
}

#if SNAPRYU_DIAGNOSTICS
// b47/b49 (diagnostic builds only): EDEN_WFI_DEFER=2 selects the "written" rule. A draw may join
// the open pass unless it samples an attachment that an earlier draw or load-op clear of this pass
// wrote (RAW), or writes an attachment that an earlier draw of this pass sampled (WAR). Checked
// draws only; any other value keeps the b48 release rule (no draw of the pass may sample any
// attachment of its framebuffer). Release builds never read "2".
inline bool WrittenRule() {
    static const bool written = [] {
        const char* value = std::getenv("EDEN_WFI_DEFER");
        const bool requested = value && value[0] == '2' && value[1] == '\0';
        const bool on = requested && Enabled();
        LOG_INFO(Render_Vulkan, "WFI_DEFER_RULE requested_written={} enabled={} policy={}",
                 requested, on, on ? "written" : "same_safe_pass_rules");
        return on;
    }();
    return written;
}

// b47/b49: whole native images of the open render pass's framebuffer that this pass has written
// (draw colour write mask / depth write / stencil test, or a load-op clear) or sampled (checked
// draws). A framebuffer has at most 9 images, so overflow only happens if the invariant breaks;
// it is then treated as "every image", never as "none".
struct AttachmentSet {
    std::array<VkImage, 9> images{};
    u32 count{};
    bool overflow{};

    void Clear() {
        count = 0;
        overflow = false;
    }
    bool Contains(VkImage image) const {
        if (overflow) return true;
        for (u32 i = 0; i < count; ++i) {
            if (images[i] == image) return true;
        }
        return false;
    }
    void Add(VkImage image) {
        if (!image || Contains(image)) return;
        if (count < images.size()) {
            images[count++] = image;
        } else {
            overflow = true;
        }
    }
    void AddAll(std::span<const VkImage> list) {
        for (const VkImage image : list) Add(image);
    }
    bool Any(std::span<const VkImage> list) const {
        for (const VkImage image : list) {
            if (image && Contains(image)) return true;
        }
        return false;
    }
    std::span<const VkImage> Span() const {
        return {images.data(), count};
    }
};
#endif

// EDEN_WFI_MODE=barrier replaces the SetEvent/WaitEvents pair by one execution-only
// vkCmdPipelineBarrier with the same stage masks (A/B against VkEvent handling in the driver).
// Only used on the deferral path (EDEN_WFI_DEFER=1); the original path is untouched.
inline bool UseBarrier() {
    static const bool use_barrier = [] {
        const char* value = std::getenv("EDEN_WFI_MODE");
        const bool on = value && std::string_view{value} == "barrier";
        LOG_INFO(Render_Vulkan, "WFI_MODE barrier={} (default=event)", on);
        return on;
    }();
    return use_barrier;
}

// Owned by one Scheduler, accessed only on its producer thread.
struct State {
    bool pending{};
    VkEvent event{};
    VkPipelineStageFlags flags{};
    u64 requests{}, deferred{}, coalesced{}, immediate{}, materialized{}, reused_draws{};
    u64 overlap_or_unknown{}, polls{}, reports{};
#if SNAPRYU_DIAGNOSTICS
    // b47/b49 (written rule only): draws that joined a pass although they sample one of its
    // attachments, draws rejected by the RAW/WAR test, and texture-cache depth feedback
    // requests that were skipped (depth not written in this pass) or honoured.
    u64 written_merges{}, written_rejects{}, feedback_skips{}, feedback_breaks{};
#endif
    FragmentBarrier::ImmediateCounts why_immediate;
    s64 last_report_us{};

    bool Request(bool has_pass, bool has_deferred_clear, bool pass_safe, bool full_barriers,
                 VkEvent event_, VkPipelineStageFlags flags_) {
        ++requests;
        const bool eligible = has_pass && !has_deferred_clear && pass_safe && !full_barriers;
        if (!eligible) {
            ++immediate;
            why_immediate.Note(has_pass, has_deferred_clear, pass_safe, full_barriers);
            return false;
        }
        ++deferred;
        coalesced += pending;
        flags = pending ? (flags | flags_) : flags_;
        event = event_;
        pending = true;
        return true;
    }
    // Returns true when a deferred WFI must be recorded now; event/flags stay readable.
    bool End() {
        const bool had_pending = pending;
        materialized += had_pending;
        pending = false;
        return had_pending;
    }
    void MaybeReport() {
#if SNAPRYU_DIAGNOSTICS
        if (!Enabled() || (++polls & 255) != 0) return;
        const s64 now = FragmentBarrier::NowUs();
        if (!last_report_us) { last_report_us = now; return; }
        if (now - last_report_us < 10'000'000) return;
        last_report_us = now;
        LOG_INFO(Render_Vulkan, "GPU_WFI_DEFER report={} requests={} deferred={} coalesced={} "
                 "immediate={} materialized={} reused_draws={} overlap_or_unknown={} "
                 "pending={} immediate_no_pass={} immediate_deferred_clear={} "
                 "immediate_unsafe_pass={} immediate_full_barrier={} mode={} policy={} "
                 "written_merges={} written_rejects={} feedback_skips={} feedback_breaks={} "
                 "cumulative_scheduler=true",
                 ++reports, requests, deferred, coalesced, immediate, materialized,
                 reused_draws, overlap_or_unknown, pending, why_immediate.no_pass,
                 why_immediate.deferred_clear, why_immediate.unsafe_pass,
                 why_immediate.full_barrier, UseBarrier() ? "barrier" : "event",
                 WrittenRule() ? "written" : "same_safe_pass_rules", written_merges,
                 written_rejects, feedback_skips, feedback_breaks);
#endif
    }
};
} // namespace Vulkan::WfiDefer
