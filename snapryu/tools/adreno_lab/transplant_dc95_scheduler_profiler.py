#!/usr/bin/env python3
from pathlib import Path
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected 1 match, got {count}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: transplant_dc95_scheduler_profiler.py <vk_scheduler.cpp>")

    path = Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include "video_core/renderer_vulkan/vk_command_pool.h"\n',
        '#include "video_core/renderer_vulkan/vk_adreno_profiler.h"\n'
        '#include "video_core/renderer_vulkan/vk_command_pool.h"\n',
        "include",
    )

    text = replace_once(
        text,
        '''    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
    AllocateNewContext();''',
        '''    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    auto& profiler = AdrenoProfiler::Get();
    const auto wait_start = profiler.Enabled() ? AdrenoProfiler::Now() : AdrenoProfiler::TimePoint{};
    Wait(presubmit_tick);
    if (profiler.Enabled()) {
        profiler.RecordFinishWait(AdrenoProfiler::ElapsedNs(wait_start));
    }
    AllocateNewContext();''',
        "Finish",
    )

    text = replace_once(
        text,
        '''void Scheduler::WaitWorker() {
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}''',
        '''void Scheduler::WaitWorker() {
    auto& profiler = AdrenoProfiler::Get();
    const auto wait_start = profiler.Enabled() ? AdrenoProfiler::Now() : AdrenoProfiler::TimePoint{};
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
    if (profiler.Enabled()) {
        profiler.RecordWorkerWait(AdrenoProfiler::ElapsedNs(wait_start));
    }
}''',
        "WaitWorker",
    )

    text = replace_once(
        text,
        '''    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
}''',
        '''    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    AdrenoProfiler::Get().RecordRenderPassBegin(framebuffer->NumImages());
}''',
        "BeginRenderPassImpl",
    )

    text = replace_once(
        text,
        '''    const DeferredClear dc = deferred_clear;
    deferred_clear = {};

    std::array<VkClearValue, 9> clear_values{};''',
        '''    const DeferredClear dc = deferred_clear;
    deferred_clear = {};
    AdrenoProfiler::Get().RecordDeferredClear();

    std::array<VkClearValue, 9> clear_values{};''',
        "RealizeDeferredClear",
    )

    text = replace_once(
        text,
        '''    if (renderpass == state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        return;
    }''',
        '''    if (renderpass == state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        AdrenoProfiler::Get().RecordRenderPassReuse();
        return;
    }''',
        "RequestRenderpass reuse",
    )

    text = replace_once(
        text,
        '''    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    return true;''',
        '''    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    AdrenoProfiler::Get().RecordDescriptorBufferBind();
    return true;''',
        "UpdateDescriptorBufferChunk",
    )

    text = replace_once(
        text,
        '''    chunk->MarkSubmit();
    DispatchWork();
    return signal_value;''',
        '''    chunk->MarkSubmit();
    AdrenoProfiler::Get().RecordSubmit();
    DispatchWork();
    return signal_value;''',
        "SubmitExecution",
    )

    text = replace_once(
        text,
        '''        if (!state.renderpass) {
            return;
        }

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);''',
        '''        if (!state.renderpass) {
            return;
        }

        AdrenoProfiler::Get().RecordRenderPassEnd(num_renderpass_images);
        AdrenoProfiler::Get().RecordPostRenderPassImageBarriers(num_renderpass_images);

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);''',
        "EndRenderPass",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Transplanted dc95 scheduler profiler hooks into {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
