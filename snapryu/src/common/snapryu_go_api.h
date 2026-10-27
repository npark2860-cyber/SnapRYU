#ifndef SNAPRYU_GO_API_H
#define SNAPRYU_GO_API_H
#include <stddef.h>
#include <stdint.h>

/* MSVC/MinGW ARM64 boundary: POD only, caller-owned buffers, no CRT ownership. */
#define SNAP_GO_ABI 1u
#define SNAP_GO_WIDTH 1280u
#define SNAP_GO_HEIGHT 720u
#define SNAP_GO_PORT 26762u

typedef struct SnapGoInput {
    uint32_t size, version;
    uint64_t sample_sequence, ownership_epoch, motion_sample_sequence, motion_delta_us;
    uint32_t remote_session_active, pad_owned, pad_valid, release_required;
    uint32_t motion_active, motion_neutralize, buttons_wire17, reserved;
    float sticks[4];
    uint32_t touch_valid, touch_down, touch_id, reserved_touch;
    float touch_x, touch_y;
    /* CemuPad's physical, pre-VPAD axes. Gyro rad/s, acceleration g, w/x/y/z. */
    float gyro_rad_s[3], accel_g[3], quaternion_wxyz[4];
} SnapGoInput;

typedef struct SnapGoStatus {
    uint32_t size, version;
    uint32_t enabled, game_running, connected, video_active, audio_active;
    uint32_t fault, paired, suspended, port, audio_playback;
    uint64_t video_frames, video_generation;
    char bind_address[64];
    char message[512];
} SnapGoStatus;

typedef struct SnapGoVideoDemand {
    uint32_t size, version, enabled, width, height, fps;
    uint64_t generation;
} SnapGoVideoDemand;

typedef struct SnapGoVideoFrame {
    uint32_t size, version, width, height, stride, format;
    uint64_t generation, timestamp_us;
    /* format 0 = RGBA8, 1 = BGRA8. Bytes already encoded RGB, no gamma transform. */
    const uint8_t* pixels;
    uint64_t bytes;
} SnapGoVideoFrame;

typedef struct SnapGoApi {
    uint32_t size, version;
    void* (*create)(const wchar_t* absolute_profile_directory);
    void (*destroy)(void*);
    int32_t (*set_enabled)(void*, uint32_t);
    int32_t (*set_bind_address)(void*, const char*);
    int32_t (*initialize_identity)(void*);
    int32_t (*reset_pairing)(void*);
    /* Modules contain 0/1 cells without quiet zone. Never log/capture this buffer. */
    int32_t (*open_pairing)(void*, uint8_t* modules, uint32_t capacity, uint32_t* side);
    void (*cancel_pairing)(void*);
    int32_t (*set_game_running)(void*, uint32_t);
    void (*set_suspended)(void*, uint32_t);
    int32_t (*get_status)(void*, SnapGoStatus*);
    int32_t (*list_addresses)(void*, char* newline_separated, uint32_t capacity);
    int32_t (*read_input)(void*, SnapGoInput*);
    /* Left/right channels are combined to phone/USB haptics, renewed until zero. */
    void (*set_rumble)(void*, uint32_t side, float low_amplitude, float high_amplitude);
    int32_t (*video_demand)(void*, SnapGoVideoDemand*);
    int32_t (*push_video)(void*, const SnapGoVideoFrame*);
    /* 48kHz PCM16 interleaved. Bounded copy only. stream_id stays stable to close. */
    int32_t (*push_audio)(void*, uint64_t stream_id, uint64_t timestamp_us,
                          uint32_t channels, uint32_t frames, const int16_t* samples);
    void (*close_audio)(void*, uint64_t stream_id);
    void (*set_input_blocked)(void*, uint32_t blocked);
} SnapGoApi;

#ifdef __cplusplus
extern "C" {
#endif
/* Returns nonzero only for matching version and adequate caller table size. */
typedef int32_t (*SnapGoGetApiFn)(uint32_t version, uint32_t capacity, SnapGoApi*);
/* Optional ABI-1 extension: no strings, allocation or cached lifecycle state. */
#define SNAP_GO_MEDIA_AUDIO 1u
#define SNAP_GO_MEDIA_PLAYBACK 2u
#define SNAP_GO_MEDIA_VIDEO 4u
typedef uint32_t (*SnapGoMediaStateFn)(void*);
#ifdef SNAP_GO_BUILD_DLL
__declspec(dllexport)
#endif
int32_t SnapGo_GetApi(uint32_t version, uint32_t capacity, SnapGoApi*);
#ifdef __cplusplus
}
static_assert(sizeof(float)==4 && sizeof(uint64_t)==8);
static_assert(offsetof(SnapGoInput, sample_sequence)==8);
static_assert(offsetof(SnapGoVideoFrame, generation)==24);
#endif
#endif
