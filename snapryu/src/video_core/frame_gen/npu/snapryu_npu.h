// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>
#include <wchar.h>

#if defined(_WIN32) && defined(SNAPRYU_NPU_BUILD_DLL)
#define SNAPRYU_NPU_API __declspec(dllexport)
#else
#define SNAPRYU_NPU_API
#endif
#if defined(_WIN32)
#define SNAPRYU_NPU_CALL __cdecl
#else
#define SNAPRYU_NPU_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum { SNAPRYU_NPU_ABI = 1, SNAPRYU_NPU_WIDTH = 1280, SNAPRYU_NPU_HEIGHT = 720 };

typedef struct SnapRyuNpu SnapRyuNpu;

typedef enum SnapRyuNpuResult {
    SNAPRYU_NPU_OK = 0,
    SNAPRYU_NPU_EMPTY = 1,
    SNAPRYU_NPU_BUSY = 2,
    SNAPRYU_NPU_WARMING = 3,
    SNAPRYU_NPU_UNAVAILABLE = 4,
    SNAPRYU_NPU_STALE = 5,
    SNAPRYU_NPU_INVALID = -1,
    SNAPRYU_NPU_ERROR = -2
} SnapRyuNpuResult;

typedef enum SnapRyuNpuState {
    SNAPRYU_NPU_INITIALIZING = 0,
    SNAPRYU_NPU_READY = 1,
    SNAPRYU_NPU_FAILED = 2,
    SNAPRYU_NPU_STOPPING = 3
} SnapRyuNpuState;

typedef enum SnapRyuNpuPixelFormat {
    SNAPRYU_NPU_RGB24 = 0,
    SNAPRYU_NPU_RGBA8 = 1,
    SNAPRYU_NPU_BGRA8 = 2
} SnapRyuNpuPixelFormat;

typedef struct SnapRyuNpuConfig {
    uint32_t struct_size;
    uint32_t abi_version;
    const wchar_t* runtime_directory;
    const wchar_t* motion_model_path;
    const wchar_t* anvil_model_path;
    // 1 or 2. Counts pending, executing, and completed-but-not-taken jobs together.
    uint32_t capacity;
    uint32_t reserved;
} SnapRyuNpuConfig;

typedef struct SnapRyuNpuImage {
    const uint8_t* pixels;
    uint64_t data_bytes;
    uint32_t width;
    uint32_t height;
    uint32_t row_bytes;
    uint32_t format;
} SnapRyuNpuImage;

typedef struct SnapRyuNpuPair {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t epoch;
    uint64_t previous_id;
    uint64_t current_id;
    // Use SnapRyuNpuNowNs(), not wall-clock or guest emulation time.
    int64_t deadline_ns;
    SnapRyuNpuImage previous;
    SnapRyuNpuImage current;
} SnapRyuNpuPair;

typedef struct SnapRyuNpuOutput {
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t epoch;
    uint64_t previous_id;
    uint64_t current_id;
    int64_t deadline_ns;
    double job_ms;
    double motion_ms;
    double prepare_ms;
    double anvil_ms;
    double compose_ms;
    uint32_t scene_cut;
    uint32_t valid_pixels;
} SnapRyuNpuOutput;

typedef struct SnapRyuNpuStats {
    uint32_t struct_size;
    uint32_t state;
    uint64_t epoch;
    uint64_t accepted;
    uint64_t completed;
    uint64_t delivered;
    uint64_t busy;
    uint64_t expired_or_reset;
    uint64_t errors;
    uint32_t occupied;
    uint32_t capacity;
    double last_job_ms;
    char last_error[384];
} SnapRyuNpuStats;

SNAPRYU_NPU_API uint32_t SNAPRYU_NPU_CALL SnapRyuNpuAbiVersion(void);
SNAPRYU_NPU_API int64_t SNAPRYU_NPU_CALL SnapRyuNpuNowNs(void);

// Returns immediately after starting the initialization worker. READY only means
// both HTP sessions are ready; it does not mean a generated frame was displayed.
SNAPRYU_NPU_API int32_t SNAPRYU_NPU_CALL SnapRyuNpuCreate(const SnapRyuNpuConfig* config,
                                                          SnapRyuNpu** output);

// Submit/Take/Stats/Reset may run concurrently; input/output buffers remain
// caller-owned for the duration of each call. Destroy requires all callers to
// have stopped, is shutdown-only, and waits for an active Run.
SNAPRYU_NPU_API void SNAPRYU_NPU_CALL SnapRyuNpuDestroy(SnapRyuNpu* context);

// Invalidate queued/completed output without waiting for active inference.
// Caller must use the returned epoch for all later submissions.
SNAPRYU_NPU_API uint64_t SNAPRYU_NPU_CALL SnapRyuNpuReset(SnapRyuNpu* context);

// Call only after GPU capture completion + mapped-memory invalidation. Successful
// submit copies both inputs during this call. No caller pointer is retained.
// BUSY/WARMING/UNAVAILABLE: keep presenting originals, never wait for capacity.
SNAPRYU_NPU_API int32_t SNAPRYU_NPU_CALL SnapRyuNpuSubmit(SnapRyuNpu* context,
                                                          const SnapRyuNpuPair* pair);

// Copies RGBA8 (alpha 255) into caller-owned storage and consumes one completion.
// EMPTY/BUSY: no output. Caller must still reject stale source IDs before upload,
// and pin its upload buffer until the GPU has finished reading it.
SNAPRYU_NPU_API int32_t SNAPRYU_NPU_CALL SnapRyuNpuTake(SnapRyuNpu* context,
                                                        SnapRyuNpuOutput* output, uint8_t* rgba,
                                                        uint64_t data_bytes, uint32_t row_bytes);

SNAPRYU_NPU_API int32_t SNAPRYU_NPU_CALL SnapRyuNpuGetStats(SnapRyuNpu* context,
                                                            SnapRyuNpuStats* output);

#ifdef __cplusplus
}
#endif
