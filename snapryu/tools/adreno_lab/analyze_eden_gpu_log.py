#!/usr/bin/env python3
"""Summarize Eden GPU/Vulkan logs for Adreno lab baseline work.

This tool is intentionally read-only: it extracts counters and coarse timing from
Eden's existing logging plus the opt-in P0/P0.2 Adreno profiler summaries without
changing renderer behaviour.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path

TIMESTAMP = re.compile(r"^\[\s*(?P<ts>[0-9]+(?:\.[0-9]+)?)\]")
RENDERPASS_BEGIN = re.compile(r"\[RenderPass\] Begin: .*?numImages=(?P<images>\d+)")
RENDERPASS_END = re.compile(r"\[RenderPass\] End")
VULKAN_CALL = re.compile(r"\[Vulkan\].*?\s(?P<call>vk[A-Za-z0-9_]+)\(")
PIPELINE_STATE = re.compile(r"\[Pipeline\] State change: (?P<state>.+)$")
PIPELINE_BIND = re.compile(r"\[Pipeline\] Bind (?P<type>Graphics|Compute) pipeline:")
DESCRIPTOR_BIND = re.compile(r"\[Descriptor\] Bind:")
MEM_ALLOC = re.compile(r"\[Memory\] Allocated (?P<size>[0-9.]+) (?P<unit>B|KB|MB|GB) .*?\(Device:(?P<device>Yes|No), Host:(?P<host>Yes|No)\)")
MEM_FREE = re.compile(r"\[Memory\] Deallocated (?P<size>[0-9.]+) (?P<unit>B|KB|MB|GB)")
GRAPHICS_FAIL = re.compile(r"Graphics pipeline build failed")
COMPUTE_FAIL = re.compile(r"Adreno rejected compute shader")

P0_SUMMARY = re.compile(
    r"\[ADRENO-P0\] frames=(?P<frames>\d+) \| "
    r"RP begin=(?P<rp_begin>\d+) \([^)]+\) reuse=(?P<rp_reuse>\d+) end=(?P<rp_end>\d+) images=(?P<rp_images>\d+) "
    r"postRPbarriers=(?P<rp_barriers>\d+) \([^)]+\) deferredClear=(?P<deferred_clear>\d+) \| "
    r"submit=(?P<submit>\d+) \([^)]+\) \| "
    r"finishWait=(?P<finish_waits>\d+) (?P<finish_ms>[0-9.]+)ms "
    r"workerWait=(?P<worker_waits>\d+) (?P<worker_ms>[0-9.]+)ms \| "
    r"gfxPipe=(?P<gfx_pipe>\d+) fail=(?P<gfx_fail>\d+) (?P<gfx_ms>[0-9.]+)ms "
    r"computePipe=(?P<compute_pipe>\d+) fail=(?P<compute_fail>\d+) (?P<compute_ms>[0-9.]+)ms \| "
    r"descReserve=(?P<desc_reserve>\d+) entries=(?P<desc_entries>\d+) \([^)]+\) "
    r"dbufEntries=(?P<dbuf_entries>\d+) dbufBinds=(?P<dbuf_binds>\d+) overflow=(?P<overflow>\d+)"
)

P02_SUMMARY = re.compile(
    r"\[ADRENO-P0\.2\] frames=(?P<frames>\d+) \| "
    r"RPend unknown=(?P<rp_unknown>\d+) deferred=(?P<rp_deferred>\d+) framebuffer=(?P<rp_framebuffer>\d+) "
    r"outside=(?P<rp_outside>\d+) submit=(?P<rp_submit>\d+) flushDeferred=(?P<rp_flush_deferred>\d+) \| "
    r"stagingUpload=(?P<staging_upload>\d+) (?P<staging_upload_mib>[0-9.]+)MiB "
    r"stagingDownload=(?P<staging_download>\d+) (?P<staging_download_mib>[0-9.]+)MiB "
    r"deferredDownload=(?P<deferred_download>\d+) (?P<deferred_download_mib>[0-9.]+)MiB \| "
    r"bufferCopy=(?P<buffer_copy>\d+) (?P<buffer_copy_mib>[0-9.]+)MiB "
    r"reorderedUpload=(?P<reordered_upload>\d+) (?P<reordered_upload_mib>[0-9.]+)MiB"
)

UNITS = {"B": 1, "KB": 1024, "MB": 1024**2, "GB": 1024**3}

P0_INT_FIELDS = (
    "frames", "rp_begin", "rp_reuse", "rp_end", "rp_images", "rp_barriers",
    "deferred_clear", "submit", "finish_waits", "worker_waits", "gfx_pipe",
    "gfx_fail", "compute_pipe", "compute_fail", "desc_reserve", "desc_entries",
    "dbuf_entries", "dbuf_binds", "overflow",
)
P0_FLOAT_FIELDS = ("finish_ms", "worker_ms", "gfx_ms", "compute_ms")
P02_INT_FIELDS = (
    "frames", "rp_unknown", "rp_deferred", "rp_framebuffer", "rp_outside",
    "rp_submit", "rp_flush_deferred", "staging_upload", "staging_download",
    "deferred_download", "buffer_copy", "reordered_upload",
)
P02_FLOAT_FIELDS = (
    "staging_upload_mib", "staging_download_mib", "deferred_download_mib",
    "buffer_copy_mib", "reordered_upload_mib",
)


def bytes_from_match(match: re.Match[str]) -> int:
    return int(float(match.group("size")) * UNITS[match.group("unit")])


def per_frame(value: int | float, frames: int) -> float:
    return 0.0 if frames == 0 else float(value) / float(frames)


def parse(path: Path) -> dict:
    calls: Counter[str] = Counter()
    pipeline_states: Counter[str] = Counter()
    rp_begin = rp_end = rp_images = 0
    gfx_binds = compute_binds = descriptor_binds = 0
    gfx_failures = compute_failures = 0
    alloc_count = free_count = 0
    alloc_bytes = free_bytes = 0
    device_local_allocs = host_visible_allocs = dual_visible_allocs = 0
    first_ts = last_ts = None
    lines = 0

    p0_reports = 0
    p02_reports = 0
    p0_ints: Counter[str] = Counter()
    p0_floats: Counter[str] = Counter()
    p02_ints: Counter[str] = Counter()
    p02_floats: Counter[str] = Counter()

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            lines += 1
            line = raw.rstrip("\n")

            ts = TIMESTAMP.match(line)
            if ts:
                value = float(ts.group("ts"))
                first_ts = value if first_ts is None else first_ts
                last_ts = value

            match = P0_SUMMARY.search(line)
            if match:
                p0_reports += 1
                for field in P0_INT_FIELDS:
                    p0_ints[field] += int(match.group(field))
                for field in P0_FLOAT_FIELDS:
                    p0_floats[field] += float(match.group(field))

            match = P02_SUMMARY.search(line)
            if match:
                p02_reports += 1
                for field in P02_INT_FIELDS:
                    p02_ints[field] += int(match.group(field))
                for field in P02_FLOAT_FIELDS:
                    p02_floats[field] += float(match.group(field))

            match = RENDERPASS_BEGIN.search(line)
            if match:
                rp_begin += 1
                rp_images += int(match.group("images"))
            if RENDERPASS_END.search(line):
                rp_end += 1

            match = VULKAN_CALL.search(line)
            if match:
                calls[match.group("call")] += 1

            match = PIPELINE_STATE.search(line)
            if match:
                pipeline_states[match.group("state").strip()] += 1

            match = PIPELINE_BIND.search(line)
            if match:
                if match.group("type") == "Graphics":
                    gfx_binds += 1
                else:
                    compute_binds += 1

            if DESCRIPTOR_BIND.search(line):
                descriptor_binds += 1
            if GRAPHICS_FAIL.search(line):
                gfx_failures += 1
            if COMPUTE_FAIL.search(line):
                compute_failures += 1

            match = MEM_ALLOC.search(line)
            if match:
                alloc_count += 1
                alloc_bytes += bytes_from_match(match)
                device = match.group("device") == "Yes"
                host = match.group("host") == "Yes"
                device_local_allocs += int(device)
                host_visible_allocs += int(host)
                dual_visible_allocs += int(device and host)

            match = MEM_FREE.search(line)
            if match:
                free_count += 1
                free_bytes += bytes_from_match(match)

    duration = None
    if first_ts is not None and last_ts is not None and last_ts >= first_ts:
        duration = last_ts - first_ts

    queue_submits = calls.get("vkQueueSubmit", 0) + calls.get("vkQueueSubmit2", 0)
    pipeline_creates = sum(
        count for name, count in calls.items()
        if name in {"vkCreateGraphicsPipelines", "vkCreateComputePipelines"}
    )

    p0_frames = p0_ints["frames"]
    p02_frames = p02_ints["frames"]
    reordered_share = None
    if p02_floats["buffer_copy_mib"] > 0:
        reordered_share = p02_floats["reordered_upload_mib"] / p02_floats["buffer_copy_mib"]

    return {
        "file": str(path),
        "lines": lines,
        "logged_duration_seconds": duration,
        "adreno_profiler": {
            "p0": {
                "reports": p0_reports,
                "frames": p0_frames,
                "render_pass": {
                    "begin": p0_ints["rp_begin"],
                    "reuse": p0_ints["rp_reuse"],
                    "end": p0_ints["rp_end"],
                    "images": p0_ints["rp_images"],
                    "post_render_pass_barriers": p0_ints["rp_barriers"],
                    "deferred_clears": p0_ints["deferred_clear"],
                    "begin_per_frame": per_frame(p0_ints["rp_begin"], p0_frames),
                    "barriers_per_frame": per_frame(p0_ints["rp_barriers"], p0_frames),
                },
                "submission": {
                    "submits": p0_ints["submit"],
                    "submits_per_frame": per_frame(p0_ints["submit"], p0_frames),
                },
                "waits": {
                    "finish_count": p0_ints["finish_waits"],
                    "finish_ms": p0_floats["finish_ms"],
                    "worker_count": p0_ints["worker_waits"],
                    "worker_ms": p0_floats["worker_ms"],
                },
                "pipeline": {
                    "graphics_builds": p0_ints["gfx_pipe"],
                    "graphics_failures": p0_ints["gfx_fail"],
                    "graphics_build_ms": p0_floats["gfx_ms"],
                    "compute_builds": p0_ints["compute_pipe"],
                    "compute_failures": p0_ints["compute_fail"],
                    "compute_build_ms": p0_floats["compute_ms"],
                },
                "descriptor": {
                    "reservations": p0_ints["desc_reserve"],
                    "entries": p0_ints["desc_entries"],
                    "entries_per_frame": per_frame(p0_ints["desc_entries"], p0_frames),
                    "descriptor_buffer_entries": p0_ints["dbuf_entries"],
                    "descriptor_buffer_binds": p0_ints["dbuf_binds"],
                    "overflows": p0_ints["overflow"],
                },
            },
            "p0_2": {
                "reports": p02_reports,
                "frames": p02_frames,
                "render_pass_end_reason": {
                    "unknown": p02_ints["rp_unknown"],
                    "deferred_clear": p02_ints["rp_deferred"],
                    "framebuffer_change": p02_ints["rp_framebuffer"],
                    "outside_operation": p02_ints["rp_outside"],
                    "submit": p02_ints["rp_submit"],
                    "flush_deferred_clear": p02_ints["rp_flush_deferred"],
                },
                "transfer": {
                    "staging_upload_requests": p02_ints["staging_upload"],
                    "staging_upload_mib_reported": p02_floats["staging_upload_mib"],
                    "staging_download_requests": p02_ints["staging_download"],
                    "staging_download_mib_reported": p02_floats["staging_download_mib"],
                    "deferred_download_requests": p02_ints["deferred_download"],
                    "deferred_download_mib_reported": p02_floats["deferred_download_mib"],
                    "buffer_copy_calls": p02_ints["buffer_copy"],
                    "buffer_copy_mib_reported": p02_floats["buffer_copy_mib"],
                    "reordered_upload_calls": p02_ints["reordered_upload"],
                    "reordered_upload_mib_reported": p02_floats["reordered_upload_mib"],
                    "reordered_upload_share_of_buffer_copy_reported": reordered_share,
                },
            },
        },
        "render_pass": {
            "begin": rp_begin,
            "end": rp_end,
            "attachment_images_on_begin": rp_images,
            "begin_end_delta": rp_begin - rp_end,
        },
        "submission": {
            "queue_submit_calls": queue_submits,
            "vkQueueSubmit": calls.get("vkQueueSubmit", 0),
            "vkQueueSubmit2": calls.get("vkQueueSubmit2", 0),
        },
        "pipeline": {
            "create_calls": pipeline_creates,
            "graphics_binds": gfx_binds,
            "compute_binds": compute_binds,
            "graphics_build_failures": gfx_failures,
            "compute_build_failures": compute_failures,
            "state_changes": dict(pipeline_states.most_common(20)),
        },
        "descriptor": {"logged_binds": descriptor_binds},
        "memory": {
            "allocations": alloc_count,
            "deallocations": free_count,
            "allocated_bytes_logged": alloc_bytes,
            "deallocated_bytes_logged": free_bytes,
            "device_local_allocations": device_local_allocs,
            "host_visible_allocations": host_visible_allocs,
            "device_local_and_host_visible_allocations": dual_visible_allocs,
        },
        "top_vulkan_calls": calls.most_common(25),
    }


def print_text(data: dict) -> None:
    profiler = data["adreno_profiler"]
    p0 = profiler["p0"]
    p02 = profiler["p0_2"]
    rp = data["render_pass"]
    submit = data["submission"]
    pipe = data["pipeline"]
    mem = data["memory"]
    duration = data["logged_duration_seconds"]

    print("=== Eden Adreno P0/P0.2 log baseline ===")
    print(f"file: {data['file']}")
    if duration is not None:
        print(f"logged span: {duration:.3f}s")

    if p0["reports"]:
        prp = p0["render_pass"]
        waits = p0["waits"]
        pipeline = p0["pipeline"]
        desc = p0["descriptor"]
        print(
            "P0 profiler: "
            f"reports={p0['reports']} frames={p0['frames']} "
            f"RP={prp['begin']} ({prp['begin_per_frame']:.2f}/f) "
            f"postRPbarriers={prp['post_render_pass_barriers']} ({prp['barriers_per_frame']:.2f}/f) "
            f"submit={p0['submission']['submits']} ({p0['submission']['submits_per_frame']:.2f}/f)"
        )
        print(
            "P0 waits/pipelines: "
            f"finish={waits['finish_count']} {waits['finish_ms']:.3f}ms "
            f"worker={waits['worker_count']} {waits['worker_ms']:.3f}ms "
            f"gfx={pipeline['graphics_builds']} fail={pipeline['graphics_failures']} {pipeline['graphics_build_ms']:.3f}ms "
            f"compute={pipeline['compute_builds']} fail={pipeline['compute_failures']} {pipeline['compute_build_ms']:.3f}ms"
        )
        print(
            "P0 descriptors: "
            f"reservations={desc['reservations']} entries={desc['entries']} "
            f"({desc['entries_per_frame']:.1f}/f) dbufEntries={desc['descriptor_buffer_entries']} "
            f"dbufBinds={desc['descriptor_buffer_binds']} overflow={desc['overflows']}"
        )

    if p02["reports"]:
        reasons = p02["render_pass_end_reason"]
        transfer = p02["transfer"]
        print(
            "P0.2 RP-end reasons: "
            f"unknown={reasons['unknown']} deferred={reasons['deferred_clear']} "
            f"framebuffer={reasons['framebuffer_change']} outside={reasons['outside_operation']} "
            f"submit={reasons['submit']} flushDeferred={reasons['flush_deferred_clear']}"
        )
        print(
            "P0.2 transfer: "
            f"stagingUpload={transfer['staging_upload_requests']} {transfer['staging_upload_mib_reported']:.3f}MiB "
            f"stagingDownload={transfer['staging_download_requests']} {transfer['staging_download_mib_reported']:.3f}MiB "
            f"deferredDownload={transfer['deferred_download_requests']} {transfer['deferred_download_mib_reported']:.3f}MiB "
            f"bufferCopy={transfer['buffer_copy_calls']} {transfer['buffer_copy_mib_reported']:.3f}MiB "
            f"reorderedUpload={transfer['reordered_upload_calls']} {transfer['reordered_upload_mib_reported']:.3f}MiB"
        )

    print(f"existing-log render pass: begin={rp['begin']} end={rp['end']} attachment-images={rp['attachment_images_on_begin']}")
    print(f"existing-log queue submits: {submit['queue_submit_calls']} (submit={submit['vkQueueSubmit']}, submit2={submit['vkQueueSubmit2']})")
    print(
        "existing-log pipeline: "
        f"create-calls={pipe['create_calls']} gfx-binds={pipe['graphics_binds']} "
        f"compute-binds={pipe['compute_binds']} gfx-fail={pipe['graphics_build_failures']} "
        f"compute-fail={pipe['compute_build_failures']}"
    )
    print(f"existing-log descriptor binds: {data['descriptor']['logged_binds']}")
    print(
        "existing-log memory: "
        f"alloc={mem['allocations']} free={mem['deallocations']} "
        f"device-local={mem['device_local_allocations']} host-visible={mem['host_visible_allocations']} "
        f"dual-visible={mem['device_local_and_host_visible_allocations']}"
    )
    if data["top_vulkan_calls"]:
        print("top Vulkan calls:")
        for name, count in data["top_vulkan_calls"]:
            print(f"  {name}: {count}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="Eden main log or eden_gpu.log")
    parser.add_argument("--json", action="store_true", help="emit JSON")
    args = parser.parse_args()

    result = parse(args.log)
    if args.json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
    else:
        print_text(result)
