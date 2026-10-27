// Test-only ABI status provider. No sockets, files, identity, input or media.
#define SNAP_GO_BUILD_DLL
#include "common/snapryu_go_api.h"

namespace {
SnapGoStatus status{};
}

int32_t SnapGo_GetApi(uint32_t version, uint32_t capacity, SnapGoApi* out) {
    if (version != SNAP_GO_ABI || capacity < sizeof(SnapGoApi) || !out) return 0;
    SnapGoApi api{};
    api.size = sizeof(api); api.version = SNAP_GO_ABI;
    api.create = [](const wchar_t*) -> void* {
        status = {}; status.size = sizeof(status); status.version = SNAP_GO_ABI;
        return &status;
    };
    api.destroy = [](void*) {};
    api.set_enabled = [](void*, uint32_t value) -> int32_t { status.enabled = value; return 1; };
    api.set_bind_address = [](void*, const char*) -> int32_t { return 0; };
    api.initialize_identity = api.reset_pairing = [](void*) -> int32_t { return 0; };
    api.open_pairing = [](void*, uint8_t*, uint32_t, uint32_t*) -> int32_t { return 0; };
    api.cancel_pairing = [](void*) {};
    api.set_game_running = [](void*, uint32_t value) -> int32_t { status.game_running = value; return 1; };
    api.set_suspended = [](void*, uint32_t value) { status.suspended = value; };
    api.get_status = [](void*, SnapGoStatus* value) -> int32_t { *value = status; return 1; };
    api.list_addresses = [](void*, char*, uint32_t) -> int32_t { return 0; };
    api.read_input = [](void*, SnapGoInput*) -> int32_t { return 0; };
    api.set_rumble = [](void*, uint32_t, float, float) {};
    api.video_demand = [](void*, SnapGoVideoDemand*) -> int32_t { return 0; };
    api.push_video = [](void*, const SnapGoVideoFrame*) -> int32_t { return 0; };
    api.push_audio = [](void*, uint64_t, uint64_t, uint32_t, uint32_t, const int16_t*) -> int32_t { return 0; };
    api.close_audio = [](void*, uint64_t) {};
    api.set_input_blocked = [](void*, uint32_t) {};
    *out = api;
    return 1;
}
