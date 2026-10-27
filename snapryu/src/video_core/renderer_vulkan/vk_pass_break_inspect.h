// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <cstdlib>
#include <span>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_adreno_profiler.h"
#include "video_core/renderer_vulkan/vk_fragment_barrier.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

// b46, EDEN_PASS_BREAK_INSPECT=1: for every draw that opens a new render pass right after a
// pass that held exactly one draw (the b43-b45 stall pattern: draw, FragmentBarrier, WFI, draw),
// classify what the new draw needs from the previous pass: same framebuffer or not, whether it
// samples an attachment of the previous pass (true feedback), whether it samples an attachment
// of its own framebuffer, and whether the draw was inspectable at all. The guest shader pair
// that dominated the b43-b45 long waits is counted separately. Producer thread only.
// b47 adds the written rule's view of the same break: did the draw sample an attachment the
// previous pass actually WROTE (colour mask / depth write / stencil / load-op clear), did it write
// an attachment the previous pass SAMPLED, and which framebuffer slots were involved. A break is
// "mergeable" when the previous pass was checked, same framebuffer, and neither test fires.
namespace Vulkan::PassBreakInspect {
inline constexpr u64 CULPRIT_VERTEX = 0x46cd8b9ce104b053ULL;
inline constexpr u64 CULPRIT_FRAGMENT = 0x53fe89d883ee502cULL;

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_PASS_BREAK_INSPECT");
        const bool on = value && value[0] == '1' && value[1] == '\0';
        LOG_INFO(Render_Vulkan,
                 "PASS_BREAK_INSPECT enabled={} culprit_vs={:016x} culprit_fs={:016x} "
                 "scope=draws_opening_a_pass_after_a_single_draw_pass extra_gpu_queries=0 "
                 "extra_gpu_waits=0",
                 on, CULPRIT_VERTEX, CULPRIT_FRAGMENT);
        return on;
    }();
    return enabled;
}

inline bool IsCulprit(u64 vertex_hash, u64 fragment_hash) {
    return vertex_hash == CULPRIT_VERTEX && fragment_hash == CULPRIT_FRAGMENT;
}

inline bool AnyShared(std::span<const VkImage> a, std::span<const VkImage> b) {
    for (const VkImage x : a) {
        if (!x) {
            continue;
        }
        for (const VkImage y : b) {
            if (x == y) {
                return true;
            }
        }
    }
    return false;
}

// b47: per framebuffer slot (0..8 in Framebuffer::Images() order), count slots whose image is in
// `subset`.
inline void CountSlots(std::array<u64, 9>& hist, std::span<const VkImage> fb_images,
                       std::span<const VkImage> subset) {
    for (size_t i = 0; i < fb_images.size() && i < hist.size(); ++i) {
        if (!fb_images[i]) {
            continue;
        }
        for (const VkImage y : subset) {
            if (y == fb_images[i]) {
                ++hist[i];
                break;
            }
        }
    }
}

inline const char* ReasonName(RenderPassEndReason reason) {
    switch (reason) {
    case RenderPassEndReason::DeferredClear:
        return "deferred_clear";
    case RenderPassEndReason::FramebufferChange:
        return "framebuffer";
    case RenderPassEndReason::OutsideOperation:
        return "outside";
    case RenderPassEndReason::Submit:
        return "submit";
    case RenderPassEndReason::FlushDeferredClear:
        return "flush_deferred";
    default:
        return "unknown";
    }
}

// Owned by one Scheduler, accessed only on its producer thread.
struct State {
    u32 current_pass_draws{}, last_pass_draws{};
    VkFramebuffer last_framebuffer{};
    RenderPassEndReason last_reason{RenderPassEndReason::Unknown};
    std::array<VkImage, 9> last_images{};
    u32 num_last_images{};
    // b47: what the previous pass wrote and sampled (scheduler attachment sets at its end).
    std::array<VkImage, 9> last_written{}, last_sampled{};
    u32 num_last_written{}, num_last_sampled{};
    bool last_unchecked{true};

    u64 draws{}, new_passes{}, after_single{}, single_same_fb{}, single_reads_prev{},
        single_reads_new{}, single_unchecked{}, single_prev_outside{};
    u64 culprit_draws{}, culprit_reuse{}, culprit_new_pass{}, culprit_after_single{},
        culprit_same_fb{}, culprit_reads_prev{}, culprit_reads_new{}, culprit_unchecked{},
        culprit_prev_outside{};
    // b47 written-rule classification of the same breaks.
    u64 single_reads_written_prev{}, single_writes_sampled_prev{}, single_prev_unchecked{},
        single_mergeable{};
    u64 culprit_reads_written_prev{}, culprit_writes_sampled_prev{}, culprit_prev_unchecked{},
        culprit_mergeable{}, culprit_no_writes{};
    std::array<u64, 9> culprit_sampled_slots{}, culprit_written_slots{},
        culprit_prev_written_slots{};
    std::array<u64, 10> culprit_images_hist{};
    u32 culprit_width{}, culprit_height{};
    u64 samples{}, polls{}, reports{};
    s64 last_report_us{};

    void PassEnded(RenderPassEndReason reason, VkFramebuffer framebuffer,
                   std::span<const VkImage> images, std::span<const VkImage> written,
                   std::span<const VkImage> sampled, bool unchecked) {
        if (!Enabled()) {
            return;
        }
        last_pass_draws = current_pass_draws;
        current_pass_draws = 0;
        last_framebuffer = framebuffer;
        last_reason = reason;
        num_last_images = static_cast<u32>(std::min<size_t>(images.size(), last_images.size()));
        std::copy_n(images.begin(), num_last_images, last_images.begin());
        num_last_written = static_cast<u32>(std::min<size_t>(written.size(), last_written.size()));
        std::copy_n(written.begin(), num_last_written, last_written.begin());
        num_last_sampled = static_cast<u32>(std::min<size_t>(sampled.size(), last_sampled.size()));
        std::copy_n(sampled.begin(), num_last_sampled, last_sampled.begin());
        last_unchecked = unchecked;
    }

    // reuse: the draw continues the open pass. Otherwise a new pass begins with this draw.
    void Draw(bool reuse, u64 vertex_hash, u64 fragment_hash, bool checked,
              VkFramebuffer framebuffer, std::span<const VkImage> fb_images, VkExtent2D area,
              std::span<const VkImage> shader_images, std::span<const VkImage> written_images) {
        if (!Enabled()) {
            return;
        }
        ++draws;
        ++current_pass_draws;
        const bool culprit = IsCulprit(vertex_hash, fragment_hash);
        if (culprit) {
            ++culprit_draws;
        }
        if (reuse) {
            if (culprit) {
                ++culprit_reuse;
            }
            return;
        }
        ++new_passes;
        if (culprit) {
            ++culprit_new_pass;
        }
        if (last_pass_draws != 1) {
            return;
        }
        const bool same_fb = framebuffer == last_framebuffer;
        const bool reads_prev = AnyShared(shader_images, {last_images.data(), num_last_images});
        const bool reads_new = AnyShared(shader_images, fb_images);
        const bool prev_outside = last_reason == RenderPassEndReason::OutsideOperation;
        // b47: the written rule's tests against the previous (single-draw) pass.
        const bool reads_written_prev =
            AnyShared(shader_images, {last_written.data(), num_last_written});
        const bool writes_sampled_prev =
            AnyShared(written_images, {last_sampled.data(), num_last_sampled});
        const bool mergeable = same_fb && checked && !last_unchecked && !reads_written_prev &&
                               !writes_sampled_prev;
        ++after_single;
        single_same_fb += same_fb;
        single_reads_prev += reads_prev;
        single_reads_new += reads_new;
        single_unchecked += !checked;
        single_prev_outside += prev_outside;
        single_reads_written_prev += reads_written_prev;
        single_writes_sampled_prev += writes_sampled_prev;
        single_prev_unchecked += last_unchecked;
        single_mergeable += mergeable;
        if (!culprit) {
            return;
        }
        ++culprit_after_single;
        culprit_same_fb += same_fb;
        culprit_reads_prev += reads_prev;
        culprit_reads_new += reads_new;
        culprit_unchecked += !checked;
        culprit_prev_outside += prev_outside;
        culprit_reads_written_prev += reads_written_prev;
        culprit_writes_sampled_prev += writes_sampled_prev;
        culprit_prev_unchecked += last_unchecked;
        culprit_mergeable += mergeable;
        culprit_no_writes += written_images.empty();
        CountSlots(culprit_sampled_slots, fb_images, shader_images);
        CountSlots(culprit_written_slots, fb_images, written_images);
        CountSlots(culprit_prev_written_slots, fb_images, {last_written.data(), num_last_written});
        ++culprit_images_hist[std::min<size_t>(fb_images.size(), culprit_images_hist.size() - 1)];
        culprit_width = area.width;
        culprit_height = area.height;
        if (samples < 32 || samples % 4096 == 0) {
            LOG_INFO(Render_Vulkan,
                     "GPU_PASS_BREAK_SAMPLE n={} culprit=1 prev_reason={} prev_draws=1 same_fb={} "
                     "reads_prev_attachment={} reads_new_attachment={} checked={} images={} "
                     "area={}x{} shader_images={} reads_written_prev={} writes_sampled_prev={} "
                     "prev_unchecked={} mergeable={} written_images={} log_first=32 "
                     "then_every=4096",
                     samples, ReasonName(last_reason), same_fb, reads_prev, reads_new, checked,
                     fb_images.size(), area.width, area.height, shader_images.size(),
                     reads_written_prev, writes_sampled_prev, last_unchecked, mergeable,
                     written_images.size());
        }
        ++samples;
    }

    void MaybeReport() {
        if (!Enabled() || (++polls & 255) != 0) {
            return;
        }
        const s64 now = FragmentBarrier::NowUs();
        if (!last_report_us) {
            last_report_us = now;
            return;
        }
        if (now - last_report_us < 10'000'000) {
            return;
        }
        last_report_us = now;
        LOG_INFO(Render_Vulkan,
                 "GPU_PASS_BREAK_INSPECT report={} draws={} new_passes={} "
                 "after_single_draw_pass={} same_fb={} reads_prev_attachment={} "
                 "reads_new_attachment={} unchecked={} prev_outside={} | culprit_draws={} "
                 "culprit_reuse={} culprit_new_pass={} culprit_after_single={} "
                 "culprit_same_fb={} culprit_reads_prev={} culprit_reads_new={} "
                 "culprit_unchecked={} culprit_prev_outside={} "
                 "culprit_images_hist=[{},{},{},{},{},{},{},{},{},{}] culprit_area={}x{} "
                 "cumulative_scheduler=true",
                 ++reports, draws, new_passes, after_single, single_same_fb, single_reads_prev,
                 single_reads_new, single_unchecked, single_prev_outside, culprit_draws,
                 culprit_reuse, culprit_new_pass, culprit_after_single, culprit_same_fb,
                 culprit_reads_prev, culprit_reads_new, culprit_unchecked, culprit_prev_outside,
                 culprit_images_hist[0], culprit_images_hist[1], culprit_images_hist[2],
                 culprit_images_hist[3], culprit_images_hist[4], culprit_images_hist[5],
                 culprit_images_hist[6], culprit_images_hist[7], culprit_images_hist[8],
                 culprit_images_hist[9], culprit_width, culprit_height);
        // b47: the written rule's view of the same breaks (slot order = Framebuffer::Images()).
        LOG_INFO(Render_Vulkan,
                 "GPU_PASS_BREAK_WRITTEN report={} after_single_draw_pass={} "
                 "reads_written_prev={} writes_sampled_prev={} prev_unchecked={} mergeable={} | "
                 "culprit_after_single={} culprit_reads_written_prev={} "
                 "culprit_writes_sampled_prev={} culprit_prev_unchecked={} culprit_mergeable={} "
                 "culprit_no_writes={} culprit_sampled_slots=[{},{},{},{},{},{},{},{},{}] "
                 "culprit_written_slots=[{},{},{},{},{},{},{},{},{}] "
                 "culprit_prev_written_slots=[{},{},{},{},{},{},{},{},{}] cumulative_scheduler=true",
                 reports, after_single, single_reads_written_prev, single_writes_sampled_prev,
                 single_prev_unchecked, single_mergeable, culprit_after_single,
                 culprit_reads_written_prev, culprit_writes_sampled_prev, culprit_prev_unchecked,
                 culprit_mergeable, culprit_no_writes, culprit_sampled_slots[0],
                 culprit_sampled_slots[1], culprit_sampled_slots[2], culprit_sampled_slots[3],
                 culprit_sampled_slots[4], culprit_sampled_slots[5], culprit_sampled_slots[6],
                 culprit_sampled_slots[7], culprit_sampled_slots[8], culprit_written_slots[0],
                 culprit_written_slots[1], culprit_written_slots[2], culprit_written_slots[3],
                 culprit_written_slots[4], culprit_written_slots[5], culprit_written_slots[6],
                 culprit_written_slots[7], culprit_written_slots[8], culprit_prev_written_slots[0],
                 culprit_prev_written_slots[1], culprit_prev_written_slots[2],
                 culprit_prev_written_slots[3], culprit_prev_written_slots[4],
                 culprit_prev_written_slots[5], culprit_prev_written_slots[6],
                 culprit_prev_written_slots[7], culprit_prev_written_slots[8]);
    }
};
} // namespace Vulkan::PassBreakInspect
#endif
