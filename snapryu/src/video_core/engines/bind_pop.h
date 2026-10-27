// SPDX-FileCopyrightText: Copyright 2026 SnapRYU
// SPDX-License-Identifier: GPL-3.0-or-later

// b79 + b81 (diagnostic builds only).
//
// BindPop: what every draw of every frame was bound to, and (b81) what those bindings held, by
// its shader pair (vertex and fragment program address, as DRAW_POP keys it). b78 showed that at
// the 44 s and 50 s pops of the skydive the game issued the same draws and every one of them
// reached the command buffer; b79 showed they were bound to the same objects (pipeline,
// framebuffer, texture handles, image views, buffers). So the draw that lost its lake or terrain
// patch ran with the same objects holding other contents, or the GPU made something else of the
// same work. b81 records the content side the GPU thread can see.
//
// Each draw is reduced to one hash per part, in the order the pipeline binds them:
//   pipe    the GraphicsPipeline object (its key: shaders and fixed state)
//   fb      the VkImage handles of the colour and depth attachments of the draw's framebuffer
//   tic     the texture handles the game put in its constant buffers (guest side)
//   view    the ImageViewIds the texture cache resolved those handles to (host side)
//   geo_g   index and vertex buffer bindings by guest address and size
//   geo_h   the same bindings by host BufferId
//   ubo_g   uniform buffer bindings by guest address and size, per stage and slot
//   ubo_h   the same by host BufferId (NULL for the fast stream-buffer path)
//   ssbo_g  storage and texture buffer bindings by guest address and size
//   ssbo_h  the same by host BufferId
//   ubo_c   b81: the bytes of each uniform buffer binding as guest memory holds them when the
//           draw is configured (the first UBO_HASH_BYTES of each binding), per stage and slot
//   img_v   b81: the content version of each sampled image: its texture-cache slot, the uploads
//           from guest memory it received (RefreshContents) and its GPU modification tick, the
//           latter replaced by a sentinel for VOLATILE_FRAMES frames after it changed so that an
//           image the GPU rewrites every frame (render targets) stays steady and an image written
//           once (a baked terrain tile) shows a short volatile run and then its new version
// Per shader pair and frame the draws' part hashes are folded in order into one sequence hash
// per part, with the draw count. A pair whose sequence hash of a part was identical for STEADY
// frames, differed for 1-SHORT frames with the same draw count, and was identical again for
// STEADY frames is a "flip" of that part; identical again but to another value is a "shift";
// (b81) differed for 1-SHORT frames and then settled on a third value is a "bump" (the shape of
// a content change that went through a wrong state: old, wrong for a moment, new).
// The first draw's bindings and contents of each pair and frame are kept as a sample so a change
// can be named: the uniform buffer slots whose bytes differ, the images whose version differs.
// PopTrace (texture_cache/pop_trace.h) reports flips, shifts and bumps around each F8 mark and
// every 30 s (BIND_REPORT / BIND_FRAME / BIND_FLIP lines).
//
// EDEN_BIND_POP=1 records. Nothing here changes what is drawn, bound or uploaded. GPU thread
// only, behind one mutex. The uniform buffer bytes are read from guest memory the way the fast
// uniform path reads them (ReadBlockUnsafe), at most UBO_HASH_BYTES per binding.
#pragma once

#if SNAPRYU_DIAGNOSTICS
// b82 WATER_WATCH: live shader hiding (EDEN_SHADER_BISECT) found the lake surface drawn by three
// fragment shaders: 019330b0c04d7fa1 (the water while the camera holds still), dbfaefab36a351a8
// (the water while the camera moves) and 65230e5813102d6f (a rare single frame); with all three
// hidden no water frame is left. The skydive pop drops the water for 1-3 frames. For every shader
// pair whose fragment shader is one of WATCH, each report lists every frame of its window, the
// frames the pair drew nothing included: draws, the part sequence hashes and per draw (up to
// WATCH_DRAWS) the mesh (geo_g: index and vertex buffers by guest address and size) and the
// content (ubo_c and img_v of that draw). BIND_WATCH_FRAME sums the draws per water shader.
// b83: Eden's log keeps about 600 characters of a line, which cut the b82 per-draw lists. The
// draws now go to BIND_WATCH_DRAWS lines of WATCH_PER_LINE draws: mesh (geo_g), ubo_g (uniform
// bindings by guest address and size), vs0..vs3 (the bytes of the first VS_SLOTS uniform buffers
// of the vertex stages, one hash per binding with its slot, so a camera slot that changes every
// frame and a tile position slot that holds are apart) and img_v (sampled image versions). The
// first-draw sample moves to BIND_WATCH_FIRST, written only when it changed.
// b84 TEX_CHECK: the b83 runs showed every water tile drawn with the same vertex uniform bytes and
// the same meshes; per tile only its sampled images differ (views 0 and 1). Around each F8 all
// water tiles switched to other images at once, images last modified 30-60 s before. For every
// image a water draw samples, the graphics pipeline asks TexDue whether to check it: the frame it
// starts being sampled by water draws and the next TEX_FOLLOW-1 frames, when its modification tick
// changed, and every TEX_RECHECK frames otherwise, within TEX_BUDGET guest bytes per frame. A check
// (TextureCache::DiagGuestState) hashes the guest bytes the image covers now and takes the hash its
// last upload read (RemapTrace::RecordUpload, every RefreshContents); an image the GPU wrote is
// noted once without hashing. TexNote names the state:
//   same      guest bytes = what the last upload read
//   STALE     neither CPU- nor GPU-modified, and the guest bytes differ from the last upload: the
//             cache shows content guest memory no longer holds and nothing told it
//   gpu       GPU-modified: the GPU wrote the image, guest memory is not its content
//   cpu       CPU-modified: a re-upload is pending (should not happen at a draw)
//   decoding  an asynchronous decode is still running
//   nosnap    no upload recorded for this slot (made by the GPU or a copy)
//   other     the recorded upload was of another image in this slot (other address or size)
// Reports list BIND_TEX_FRAME per frame with checks and BIND_TEX per check (all but "same" ones
// of follow-up frames); a STALE check is also logged at once (TEX_CHECK stale, first 64). The
// BIND_WATCH_DRAWS entries gain i0/i1, the image slots of views 0 and 1 of the draw.
// b85 TILE_CHASE: b84 found every image the water tiles sample equal to guest memory (no STALE in
// about 9800 checks). The b84 runs show all main water tiles bound to the same index and vertex
// buffer (one 2.25 MB vertex buffer, one 5.6 KB index buffer), so what tells two tiles apart is
// not in the b83 per-draw hashes: the draw parameters (base vertex, first index, count, base
// instance, instances), the bytes of the geometry buffers, or a uniform buffer other than the
// first four of the vertex stage (ubo_g, the binding addresses, differs from draw to draw while
// vs0..vs3 did not). Per watched draw b85 keeps:
//   draw    base vertex, first index, index (or vertex) count, base instance, instances, from the
//           draw state when the pipeline is configured (Water)
//   geo     the hash of the bytes of the index and vertex bindings in guest memory (the first
//           GEO_MAX bytes of each; each binding once per frame, GEO_BUDGET bytes per frame;
//           0 = not hashed)
//   ubo     every uniform buffer binding of every stage (UBO_KEPT), slot, size and bytes hash
// Per watched pair and frame BIND_WATCH_UBO lists the first draw's bindings and the slots whose
// bytes differ between the draws of the frame ("vary"); each BIND_WATCH_DRAWS entry carries the
// bytes hash of the first UBO_VARY varying slots (u0..u2). vs0..vs3 are gone (ubo covers them).
// Water also starts the pending draw afresh, so bindings of a draw the pipeline gave up on before
// Draw / Discard no longer leak into the next one.
// b87 LATE_MIX: b86 (VIS_RING) caught the water pop in the picture: one frame, the lake bed shows
// through tile-shaped holes, with every water draw still issued. That frame is exactly a frame
// where the water tiles' vertex uniform slot 15 (s0i15) and the images of view 0 change (they
// change every 9 frames; the pop comes at some of those changes only). The b85/b86 hashes are of
// guest memory when the GPU thread reads it, not of what the GPU got: the fast uniform path gets
// the bytes as they were when the draw's command list was pushed (UB_KEEP), while the classic
// uniform path, storage buffers, the texture handles (read from constant buffers by the
// pipeline) and the TIC/TSC descriptors get guest memory as it is now. If the CPU already wrote
// the next update when the GPU thread reaches the draw, a water draw mixes pushed and newer
// inputs. EDEN_LATE_MIX=1 (with EDEN_BIND_POP=1, UB_KEEP on) compares, for every draw of a WATCH
// fragment shader, each input with the bytes as they were when its list was pushed:
//   F  fast uniform buffers: guest memory now (raw) against what UbKeep::FastRead gave the GPU
//   C  classic uniform buffers: now (what the upload reads) against the pushed bytes (PeekRead)
//   S  storage buffers: the same (the first LATE_PEEK_MAX bytes)
//   H  texture handles in constant buffers: the word now against the pushed word
//   D  TIC/TSC descriptor entries: now against pushed
// PeekRead arms the pages it reads (as FastRead does) so that a later CPU write is copied; a read
// that armed a page first ("fresh") cannot know an earlier write. Nothing the GPU reads changes.
// Reports add BIND_LATE per watched pair and frame: per kind late/compared/fresh, and the bindings
// late in some draw with the first such draw's raw>kept hashes.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "common/ub_keep.h"

namespace VideoCommon::BindPop {

inline constexpr size_t RING = 192;        // frames kept (PopTrace reports up to 160)
inline constexpr size_t MAX_KEYS = 2048;   // shader pairs recorded per frame
inline constexpr size_t MAX_LISTED = 64;   // BIND_FLIP lines per report
inline constexpr u32 SHORT = 3;            // a flip or bump lasts at most 3 frames
inline constexpr u32 STEADY = 4;           // with at least 4 identical frames each side
inline constexpr size_t SAMPLE_VIEWS = 12; // texture handles kept of a sample draw
inline constexpr size_t SAMPLE_VERTEX = 3; // vertex buffer bindings kept of a sample draw
inline constexpr size_t SAMPLE_UBOS = 8;   // uniform buffer bindings kept of a sample draw
inline constexpr u32 INDEX_STAGE = 0xFF;   // the index buffer binding (vertex buffers: stage 0)
inline constexpr size_t UBO_HASH_BYTES = 8192; // bytes of a uniform buffer binding hashed
inline constexpr u64 VOLATILE_FRAMES = 1;  // frames after a GPU write during which img_v is the sentinel
inline constexpr u64 VOLATILE = 0xB81B81B81B81B81BULL; // the sentinel
inline constexpr u32 NO_IMAGE = 0xFFFFFFFFu; // a view without an image (null view)
// b82: the water fragment shaders (shader cache hashes, as EDEN_SHADER_BISECT names them)
inline constexpr std::array<u64, 3> WATCH{0x019330b0c04d7fa1ULL, 0xdbfaefab36a351a8ULL,
                                          0x65230e5813102d6fULL};
inline constexpr size_t WATCH_DRAWS = 96; // draws listed per watched pair and frame
inline constexpr size_t WATCH_PAIRS = 32; // watched pairs listed per report
inline constexpr size_t WATCH_PER_LINE = 3; // b85: draws per BIND_WATCH_DRAWS line (< 600 chars)
inline constexpr size_t UBO_KEPT = 14;      // b85: uniform bindings (all stages) kept per watched draw
inline constexpr size_t UBO_VARY = 3;       // b85: varying uniform bindings listed per draw
inline constexpr u64 GEO_BUDGET = 16ULL << 20; // b85: geometry bytes of water draws hashed per frame
inline constexpr u32 GEO_MAX = 4u << 20;    // b85: bytes hashed of one geometry binding (its start)
inline constexpr size_t TILE_VIEWS = 2;     // b84: image slots of views 0..1 kept per watched draw
inline constexpr u64 TEX_FOLLOW = 6;        // b84: frames an image is checked after it appears
inline constexpr u64 TEX_RECHECK = 30;      // b84: frames between checks of an image otherwise
inline constexpr u64 TEX_BUDGET = 16ULL << 20; // b84: guest bytes hashed per frame
inline constexpr size_t TEX_KEPT = 64;      // b84: checks kept per frame
inline constexpr u32 TEX_LOGGED = 64;       // b84: STALE checks logged at once
// b84: ImageFlagBits (texture_cache/image_base.h) TexNote reads.
inline constexpr u32 TEX_CPU_MODIFIED = 1u << 2;
inline constexpr u32 TEX_GPU_MODIFIED = 1u << 3;
inline constexpr u32 TEX_DECODING = 1u << 17;
// b87 LATE_MIX
inline constexpr size_t LATE_SLOTS = 24;          // buffer bindings compared per watched draw
inline constexpr u32 LATE_PEEK_MAX = 32u << 10;   // bytes of a classic or storage binding compared
inline constexpr u64 LATE_BUDGET = 16ULL << 20;   // classic and storage bytes compared per frame
inline constexpr size_t LATE_LISTED = 8;          // late bindings named per BIND_LATE line
enum class LateKind : u8 { Fast, Classic, Storage, Handle, Desc, Count };
inline constexpr size_t LATE_KINDS = static_cast<size_t>(LateKind::Count);
inline constexpr std::array<char, LATE_KINDS> LATE_CODES{'F', 'C', 'S', 'H', 'D'};

enum class Part : u8 {
    Pipe,
    Fb,
    Tic,
    View,
    GeoGuest,
    GeoHost,
    UboGuest,
    UboHost,
    SsboGuest,
    SsboHost,
    UboContent,
    ImgVer,
    Count
};
inline constexpr size_t PARTS = static_cast<size_t>(Part::Count);
inline constexpr std::array<const char*, PARTS> PART_NAMES{"pipe",   "fb",     "tic",    "view",
                                                           "geo_g",  "geo_h",  "ubo_g",  "ubo_h",
                                                           "ssbo_g", "ssbo_h", "ubo_c",  "img_v"};

enum class Kind : u8 { Flip, Shift, Bump };
inline constexpr std::array<const char*, 3> KIND_NAMES{"flip", "shift", "bump"};

/// Order-sensitive 64-bit fold.
inline u64 Mix(u64 h, u64 x) {
    h ^= x + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h * 0x100000001B3ULL + 1;
}

/// A Vulkan handle (pointer on 64-bit targets, integer on 32-bit) as bits.
template <class H>
inline u64 Bits(H handle) {
    if constexpr (std::is_pointer_v<H>) {
        return static_cast<u64>(reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<u64>(handle);
    }
}

/// Hash of a byte range (eight bytes at a time, then the tail), seeded with its size.
inline u64 HashBytes(const void* data, size_t size) {
    const u8* bytes = static_cast<const u8*>(data);
    u64 h = Mix(0x5A17, size);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 word;
        std::memcpy(&word, bytes + i, 8);
        h = Mix(h, word);
    }
    if (i < size) {
        u64 word = 0;
        std::memcpy(&word, bytes + i, size - i);
        h = Mix(h, word);
    }
    return h;
}

/// Buffer cache (GPU thread): the hash of the first min(size, UBO_HASH_BYTES) bytes of a guest
/// range, read with the caller's reader (device_memory.ReadBlockUnsafe); 0 for an empty range.
template <class Reader>
inline u64 HashGuest(u32 size, Reader&& reader) {
    thread_local std::vector<u8> buffer(UBO_HASH_BYTES);
    const size_t n = (std::min)(static_cast<size_t>(size), UBO_HASH_BYTES);
    if (n == 0) {
        return 0;
    }
    reader(buffer.data(), n);
    return HashBytes(buffer.data(), n);
}

struct BufSample {
    u64 addr = 0;
    u32 size = 0;
    u32 id = 0;
};

/// A uniform buffer binding of a sample draw with the hash of its bytes.
struct UboSample {
    u32 stage = 0;
    u32 index = 0;
    u32 size = 0;
    u64 hash = 0;
};

/// A texture of a sample draw: handle, image view, image slot and content version.
struct ViewSample {
    u32 tic = 0;
    u32 view = 0;
    u32 image = NO_IMAGE;
    u64 version = 0;
};

/// A texture binding of the draw being configured, as the graphics pipeline passes it.
struct ViewRef {
    u32 tic = 0;             // the texture handle (ImageViewInOut::index)
    u32 view = 0;            // the image view it resolved to (ImageViewInOut::id.index)
    u32 image = NO_IMAGE;    // the texture-cache image slot of that view (NO_IMAGE for none)
    u64 modification = 0;    // the image's modification tick (ImageBase::modification_tick)
};

/// The bindings of one draw, as far as a log line can carry them.
struct Sample {
    u64 pipe = 0;
    u64 fb = 0;
    std::array<ViewSample, SAMPLE_VIEWS> view{};
    u32 views = 0;    // texture handles of the draw (all of them, not only the sampled ones)
    BufSample index;  // size 0 when the draw was not indexed
    std::array<BufSample, SAMPLE_VERTEX> vertex{};
    u32 vertices = 0; // vertex buffer bindings of the draw (all of them)
    std::array<UboSample, SAMPLE_UBOS> ubo{};
    u32 ubos = 0;     // uniform buffer bindings of the draw (all of them)
};

/// b82: the index of fs_hash in WATCH, or WATCH.size().
inline size_t WatchIndex(u64 fs_hash) {
    for (size_t i = 0; i < WATCH.size(); ++i) {
        if (WATCH[i] == fs_hash) {
            return i;
        }
    }
    return WATCH.size();
}

/// b85: a uniform buffer binding of a watched draw.
struct UboSlot {
    u32 slot = 0;    // stage << 8 | index
    u32 size = 0;
    u64 content = 0; // the hash of its bytes (HashGuest)
};

/// b87: one comparison of a watched draw: what guest memory holds now (raw) and what the draw's
/// command reads as UB_KEEP keeps it (kept: the bytes when its list was pushed; for the fast path
/// what the GPU got). 32-bit hashes (handles: the word). fresh = the pages were armed by this read.
struct LateSlot {
    u32 slot = 0; // stage << 8 | index
    LateKind kind = LateKind::Fast;
    bool fresh = false;
    u32 size = 0;
    u32 raw = 0;
    u32 kept = 0;
};

/// b87: the comparisons of the draw being configured (GPU thread; LateScope starts it afresh).
struct LateAcc {
    std::array<LateSlot, LATE_SLOTS> slot{}; // buffer bindings (F, C, S) in binding order
    u32 slots = 0;
    std::array<u16, LATE_KINDS> reads{};
    std::array<u16, LATE_KINDS> late{}; // raw != kept
    std::array<u16, LATE_KINDS> fresh{};
    u8 first_kind = 0xFF;               // the first late handle or descriptor: kind, raw, kept
    u32 first_raw = 0;
    u32 first_kept = 0;
};

/// b82/b83: one draw of a watched pair.
struct WatchDraw {
    u64 mesh = 0;  // geo_g
    u64 ubo_g = 0; // uniform bindings by guest address and size
    u64 img_v = 0; // sampled image versions
    std::array<u32, TILE_VIEWS> img{NO_IMAGE, NO_IMAGE}; // b84: image slots of views 0..1
    u32 base_vertex = 0; // b85: the draw parameters
    u32 first_index = 0;
    u32 count = 0;
    u32 base_instance = 0;
    u32 instances = 0;
    u64 geo_c = 0;       // b85: bytes of the index and vertex bindings (0 = not hashed)
    u32 ubos = 0;        // b85: uniform bindings of the draw (all of them; UBO_KEPT kept)
    std::array<UboSlot, UBO_KEPT> ubo{};
    LateAcc late;        // b87
};

/// b84: what TextureCache::DiagGuestState found for an image a water draw samples.
struct TexState {
    u64 gpu_addr = 0;
    u32 size = 0;          // guest_size_bytes
    u32 format = 0;        // PixelFormat
    u32 flags = 0;         // ImageFlagBits
    u64 modification = 0;  // modification_tick
    u32 snapshot = 0;      // 0 no upload recorded, 1 recorded, 2 recorded for another image
    u64 upload_hash = 0;   // what the last upload read
    u64 upload_tick = 0;   // the texture cache frame tick of that upload
    u64 now_hash = 0;      // the guest bytes now
    u64 frame_tick = 0;    // the texture cache frame tick now
};

enum class TexWhy : u8 { None, First, Follow, Version, Periodic };
inline constexpr std::array<const char*, 5> TEX_WHY_NAMES{"-", "first", "follow", "version",
                                                           "periodic"};
enum class TexVerdict : u8 { Same, Stale, Gpu, Cpu, Decoding, NoSnap, Other, Count };
inline constexpr std::array<const char*, 7> TEX_VERDICT_NAMES{"same", "STALE",  "gpu",  "cpu",
                                                               "decoding", "nosnap", "other"};
inline constexpr size_t TEX_VERDICTS = static_cast<size_t>(TexVerdict::Count);

/// b84: one check of a frame.
struct TexCheck {
    u32 image = 0;
    TexWhy why = TexWhy::None;
    TexVerdict verdict = TexVerdict::Same;
    TexState state;
};

/// b84: when an image sampled by water draws was seen and checked (frame numbers).
struct TexTrack {
    u32 gen = 0;          // ImageVer::gen when tracked (a deleted image starts over)
    u64 first = 0;        // the frame it started being sampled by water draws
    u64 seen = 0;         // the last frame it was
    u64 checked = 0;      // the last frame it was checked
    u64 modification = 0; // its modification tick at that check
};

/// The draw being configured: its part hashes and sample, filled by Buf() and sealed by Draw().
struct Pending {
    std::array<u64, PARTS> h{};
    Sample sample;
    std::array<UboSlot, UBO_KEPT> ubo{}; // b85: the uniform bindings, in binding order
    u32 ubos = 0;
    bool water = false; // b85: a draw of a watched shader (Water)
    u32 base_vertex = 0;
    u32 first_index = 0;
    u32 count = 0;
    u32 base_instance = 0;
    u32 instances = 0;
    u64 geo_c = 0;
};

struct Rec {
    u64 vs = 0;
    u64 fs = 0;
    u64 vs_hash = 0; // the shader cache hashes of the pipeline's vertex and fragment shaders
    u64 fs_hash = 0;
    u32 draws = 0;
    std::array<u64, PARTS> seq{}; // fold of the draws' part hashes, in draw order
    Sample first;                 // the first draw of the pair in this frame
    std::vector<WatchDraw> watch; // b82: the draws of a watched pair (up to WATCH_DRAWS)
};

struct Frame {
    u64 tick = 0;
    u64 t_ms = 0;
    std::vector<Rec> recs;
    u32 dropped_keys = 0; // draws of pairs not recorded (MAX_KEYS)
    u32 draws = 0;        // draws configured (bound) in the frame
    u32 new_views = 0;    // ImageViewIds bound for the first time since the start
    u32 uploads = 0;      // b81: image uploads from guest memory (RefreshContents) in the frame
    std::vector<TexCheck> tex;                // b84: the checks of the frame (up to TEX_KEPT)
    std::array<u32, TEX_VERDICTS> tex_counts{}; // b84: all checks of the frame by verdict
    u32 tex_skipped = 0;                      // b84: checks put off by TEX_BUDGET
    u64 tex_bytes = 0;                        // b84: guest bytes hashed
    u32 geo_hashed = 0;                       // b85: geometry bindings hashed
    u64 geo_bytes = 0;                        // b85: their bytes
    u32 geo_skipped = 0;                      // b85: bindings left unhashed by GEO_BUDGET
    u64 late_bytes = 0;                       // b87: classic and storage bytes compared
    u32 late_skipped = 0;                     // b87: comparisons left out by LATE_BUDGET
};

/// b81: what BindPop knows of a texture-cache image slot.
struct ImageVer {
    u32 gen = 0;           // bumped when the slot's image is deleted (the slot may be reused)
    u32 uploads = 0;       // RefreshContents uploads since the image was created
    u64 modification = 0;  // the modification tick last seen at a draw
    u64 changed_frame = 0; // frame number + 1 at which the tick was last seen to change
};

struct State {
    std::mutex mutex;
    bool record = false;
    std::deque<Frame> frames;
    Frame current;
    ankerl::unordered_dense::map<u64, u32> index; // pair key -> index in current.recs
    ankerl::unordered_dense::set<u32> seen_views;
    Pending pending;
    std::vector<ImageVer> images; // by image slot index
    ankerl::unordered_dense::map<u32, TexTrack> tex; // b84: by image slot index
    u32 tex_logged = 0;                              // b84: STALE checks logged at once
    std::array<u64, TEX_VERDICTS> tex_total{};       // b84: all checks since the start
    ankerl::unordered_dense::map<u64, u64> geo_cache; // b85: geometry bytes hashed this frame
    bool late = false;            // b87: EDEN_LATE_MIX=1 (with record)
    u64 frame_no = 1;             // the frame being recorded (EndFrame calls + 1)
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

inline bool EnvOn(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

inline State& Get() {
    static State state;
    [[maybe_unused]] static const bool ready = [](State& s) {
        s.record = EnvOn("EDEN_BIND_POP");
        s.late = s.record && EnvOn("EDEN_LATE_MIX"); // b87
        LOG_WARNING(Render_Vulkan,
                    "BIND_POP ready b87 record={} ring={} max_keys={} short={} steady={} parts={} "
                    "ubo_bytes={} volatile_frames={} watch={:016x}/{:016x}/{:016x} watch_draws={} "
                    "per_line={} ubo_kept={} ubo_vary={} geo_budget={} geo_max={} tile_views={} "
                    "tex_follow={} tex_recheck={} tex_budget={} tex_kept={} late={} late_slots={} "
                    "late_peek_max={} late_budget={}",
                    s.record, RING, MAX_KEYS, SHORT, STEADY, PARTS, UBO_HASH_BYTES,
                    VOLATILE_FRAMES, WATCH[0], WATCH[1], WATCH[2], WATCH_DRAWS, WATCH_PER_LINE,
                    UBO_KEPT, UBO_VARY, GEO_BUDGET, GEO_MAX, TILE_VIEWS, TEX_FOLLOW, TEX_RECHECK,
                    TEX_BUDGET, TEX_KEPT, s.late, LATE_SLOTS, LATE_PEEK_MAX, LATE_BUDGET);
        return true;
    }(state);
    return state;
}

inline bool Active() {
    return Get().record;
}

/// The record of the pair in the current frame (nullptr once MAX_KEYS pairs are recorded).
inline Rec* Find(State& s, u64 vs, u64 fs) {
    const u64 key = vs ^ (fs * 0x9E3779B97F4A7C15ULL);
    const auto it = s.index.find(key);
    if (it != s.index.end()) {
        return &s.current.recs[it->second];
    }
    if (s.current.recs.size() >= MAX_KEYS) {
        ++s.current.dropped_keys;
        return nullptr;
    }
    s.index.emplace(key, static_cast<u32>(s.current.recs.size()));
    Rec rec;
    rec.vs = vs;
    rec.fs = fs;
    s.current.recs.push_back(rec);
    return &s.current.recs.back();
}

inline ImageVer& ImageSlot(State& s, u32 image) {
    if (s.images.size() <= image) {
        s.images.resize(static_cast<size_t>(image) + 1);
    }
    return s.images[image];
}

/// Texture cache (GPU thread, RefreshContents): the image's content is replaced from guest
/// memory (synchronous upload, asynchronous decode or GPU unswizzle alike).
inline void ImageUploaded(u32 image) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    ++ImageSlot(s, image).uploads;
    ++s.current.uploads;
}

/// Texture cache (GPU thread, DeleteImage): the slot may hold another image from now on.
inline void ImageDeleted(u32 image) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    ImageVer& v = ImageSlot(s, image);
    ++v.gen;
    v.uploads = 0;
    v.modification = 0;
    v.changed_frame = 0;
}

/// The content version of an image at a draw of the current frame (see img_v above).
inline u64 Version(State& s, u32 image, u64 modification) {
    if (image == NO_IMAGE) {
        return 0;
    }
    ImageVer& v = ImageSlot(s, image);
    if (v.modification != modification) {
        v.modification = modification;
        v.changed_frame = s.frame_no;
    }
    const bool recent = v.changed_frame != 0 && s.frame_no - v.changed_frame <= VOLATILE_FRAMES;
    return Mix(Mix(Mix(image, v.gen), v.uploads), recent ? VOLATILE : modification);
}

/// b85, graphics pipeline (GPU thread, Configure, before the buffer cache binds anything): a draw
/// starts. water = its fragment shader is one of WATCH; the draw parameters as the draw state
/// holds them (base vertex or first vertex, first index, index or vertex count, base instance,
/// instances). The pending draw starts afresh.
inline void Water(bool water, u32 base_vertex, u32 first_index, u32 count, u32 base_instance,
                  u32 instances) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Pending& p = s.pending;
    p = Pending{};
    p.water = water;
    p.base_vertex = base_vertex;
    p.first_index = first_index;
    p.count = count;
    p.base_instance = base_instance;
    p.instances = instances;
}

/// b87 LATE_MIX (GPU thread): inside the configure of a watched draw (LateScope).
inline thread_local bool late_active = false;
inline thread_local LateAcc late_acc;

/// b87, graphics pipeline (ConfigureImpl, before anything is read): the configure of a draw of a
/// WATCH fragment shader compares its inputs (LateFast, LateBuffer, LateHandle, LateDescriptor);
/// Draw keeps the result with the draw.
class LateScope {
public:
    explicit LateScope(u64 fs_hash) {
        if (Get().late && WatchIndex(fs_hash) < WATCH.size()) {
            late_acc = LateAcc{};
            late_active = true;
        }
    }
    ~LateScope() {
        late_active = false;
    }
    LateScope(const LateScope&) = delete;
    LateScope& operator=(const LateScope&) = delete;
};

/// b87: one comparison of the draw being configured.
inline void LateNote(LateKind kind, u32 slot, u32 size, u32 raw, u32 kept, bool fresh) {
    LateAcc& a = late_acc;
    const size_t k = static_cast<size_t>(kind);
    const bool late = raw != kept;
    ++a.reads[k];
    if (late) {
        ++a.late[k];
    }
    if (fresh) {
        ++a.fresh[k];
    }
    if (kind == LateKind::Fast || kind == LateKind::Classic || kind == LateKind::Storage) {
        if (a.slots < LATE_SLOTS) {
            a.slot[a.slots++] = LateSlot{slot, kind, fresh, size, raw, kept};
        }
    } else if (late && a.first_kind == 0xFF) {
        a.first_kind = static_cast<u8>(kind);
        a.first_raw = raw;
        a.first_kept = kept;
    }
}

/// b87, buffer cache fast uniform path: the hash of the bytes just read from guest memory, before
/// UB_KEEP puts the pushed bytes over them (0 outside a watched draw).
inline u64 LateHash(const u8* data, u32 size) {
    return late_active ? HashBytes(data, size) : 0;
}

/// b87, buffer cache fast uniform path, after UbKeep::FastRead: data = the bytes the GPU reads.
inline void LateFast(u32 stage, u32 index, u32 size, u64 raw, const u8* data) {
    if (late_active) {
        LateNote(LateKind::Fast, (stage << 8) | index, size, static_cast<u32>(raw),
                 static_cast<u32>(HashBytes(data, size)), false);
    }
}

/// b87, buffer cache (classic uniform path before its upload, storage buffers): guest memory now,
/// read with the caller's reader, against the pushed bytes; the first LATE_PEEK_MAX bytes, within
/// LATE_BUDGET bytes per frame.
template <class Reader>
inline void LateBuffer(LateKind kind, u32 slot, u64 daddr, u32 size, Reader&& reader) {
    if (!late_active || size == 0) {
        return;
    }
    const u32 n = (std::min)(size, LATE_PEEK_MAX);
    {
        State& s = Get();
        std::scoped_lock lock{s.mutex};
        if (s.current.late_bytes + n > LATE_BUDGET) {
            ++s.current.late_skipped;
            return;
        }
        s.current.late_bytes += n;
    }
    thread_local std::vector<u8> raw;
    thread_local std::vector<u8> kept;
    raw.resize(n);
    reader(raw.data(), static_cast<size_t>(n));
    kept = raw;
    const Common::UbKeep::Peek peek = Common::UbKeep::PeekRead(daddr, kept.data(), n);
    const u32 raw_hash = static_cast<u32>(HashBytes(raw.data(), n));
    const u32 kept_hash = peek.used ? static_cast<u32>(HashBytes(kept.data(), n)) : raw_hash;
    LateNote(kind, slot, size, raw_hash, kept_hash, peek.fresh);
}

/// b87, graphics pipeline (a texture handle in a constant buffer): the word at gpu_addr now
/// against the pushed word. The pipeline reads the handle itself; this reads it once more.
template <class MemoryManager>
inline void LateHandle(const MemoryManager& gpu_memory, u64 gpu_addr) {
    if (!late_active) {
        return;
    }
    const auto daddr = gpu_memory.GpuToCpuAddress(gpu_addr);
    if (!daddr) {
        return;
    }
    u32 now = 0;
    gpu_memory.ReadBlockUnsafe(gpu_addr, &now, sizeof(now));
    u32 kept = now;
    const Common::UbKeep::Peek peek =
        Common::UbKeep::PeekRead(*daddr, reinterpret_cast<u8*>(&kept), sizeof(kept));
    LateNote(LateKind::Handle, 0, sizeof(now), now, kept, peek.fresh);
}

/// b87, descriptor table (a TIC or TSC entry just read into now): against the pushed entry.
template <class MemoryManager>
inline void LateDescriptor(const MemoryManager& gpu_memory, u64 gpu_addr, const void* now,
                           size_t size) {
    if (!late_active) {
        return;
    }
    const auto daddr = gpu_memory.GpuToCpuAddress(gpu_addr);
    if (!daddr) {
        return;
    }
    std::array<u8, 64> kept{};
    size = (std::min)(size, kept.size());
    std::memcpy(kept.data(), now, size);
    const Common::UbKeep::Peek peek = Common::UbKeep::PeekRead(*daddr, kept.data(), size);
    const u32 raw_hash = static_cast<u32>(HashBytes(now, size));
    const u32 kept_hash = peek.used ? static_cast<u32>(HashBytes(kept.data(), size)) : raw_hash;
    LateNote(LateKind::Desc, 0, static_cast<u32>(size), raw_hash, kept_hash, peek.fresh);
}

/// b85, buffer cache (GPU thread): the hash of a geometry binding's bytes for a water draw, read
/// with the caller's reader (device_memory.ReadBlockUnsafe): the first GEO_MAX bytes, each
/// binding once per frame, GEO_BUDGET bytes per frame. 0 = not a water draw, or not hashed.
template <class Reader>
inline u64 GeoContent(u64 addr, u32 size, Reader&& reader) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    if (!s.pending.water || size == 0) {
        return 0;
    }
    const u64 key = Mix(addr, size);
    if (const auto it = s.geo_cache.find(key); it != s.geo_cache.end()) {
        return it->second;
    }
    const u32 n = (std::min)(size, GEO_MAX);
    if (s.current.geo_bytes + n > GEO_BUDGET) {
        ++s.current.geo_skipped;
        return 0;
    }
    thread_local std::vector<u8> buffer;
    buffer.resize(n);
    reader(buffer.data(), static_cast<size_t>(n));
    const u64 hash = HashBytes(buffer.data(), n) | 1; // never 0 (0 = not hashed)
    s.current.geo_bytes += n;
    ++s.current.geo_hashed;
    s.geo_cache.emplace(key, hash);
    return hash;
}

/// Buffer cache (GPU thread, inside GraphicsPipeline::Configure): one buffer binding of the
/// draw being configured. guest = GeoGuest, UboGuest or SsboGuest; the host part is the next
/// one. stage = shader stage (INDEX_STAGE for the index buffer), index = slot, addr/size = the
/// guest binding, id = the host BufferId index, content = (UboGuest) the hash of the bytes the
/// binding holds in guest memory (HashGuest), folded into ubo_c.
inline void Buf(Part guest, u32 stage, u32 index, u64 addr, u32 size, u32 id, u64 content = 0) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Pending& p = s.pending;
    const size_t g = static_cast<size_t>(guest);
    const size_t h = g + 1;
    const u64 slot = (static_cast<u64>(stage) << 32) | index;
    p.h[g] = Mix(Mix(Mix(p.h[g], slot), addr), size);
    p.h[h] = Mix(Mix(p.h[h], slot), id);
    if (guest == Part::GeoGuest) {
        if (content != 0) { // b85: GeoContent (0 = not hashed; geo_c stays 0 if none was)
            p.geo_c = Mix(Mix(p.geo_c, slot), content);
        }
        if (stage == INDEX_STAGE) {
            p.sample.index = BufSample{addr, size, id};
        } else {
            if (p.sample.vertices < SAMPLE_VERTEX) {
                p.sample.vertex[p.sample.vertices] = BufSample{addr, size, id};
            }
            ++p.sample.vertices;
        }
    } else if (guest == Part::UboGuest) {
        const size_t c = static_cast<size_t>(Part::UboContent);
        p.h[c] = Mix(Mix(p.h[c], slot), content);
        if (p.ubos < UBO_KEPT) {
            p.ubo[p.ubos] = UboSlot{(stage << 8) | index, size, content}; // b85
        }
        ++p.ubos;
        if (p.sample.ubos < SAMPLE_UBOS) {
            p.sample.ubo[p.sample.ubos] = UboSample{stage, index, size, content};
        }
        ++p.sample.ubos;
    }
}

/// Graphics pipeline (GPU thread): the draw was configured and will be recorded. pipe = the
/// pipeline object, fb = the hash of the framebuffer's attachment images, views = the texture
/// bindings in binding order (handle, image view, image slot, modification tick).
inline void Draw(u64 vs, u64 fs, u64 vs_hash, u64 fs_hash, const void* pipe, u64 fb,
                 const ViewRef* views, size_t count) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    Pending& p = s.pending;
    p.h[static_cast<size_t>(Part::Pipe)] = Mix(0, Bits(pipe));
    p.h[static_cast<size_t>(Part::Fb)] = fb;
    p.sample.pipe = Bits(pipe);
    p.sample.fb = fb;
    for (size_t i = 0; i < count; ++i) {
        const u32 tic = views[i].tic;
        const u32 view = views[i].view;
        const u64 version = Version(s, views[i].image, views[i].modification);
        p.h[static_cast<size_t>(Part::Tic)] = Mix(p.h[static_cast<size_t>(Part::Tic)], tic);
        p.h[static_cast<size_t>(Part::View)] = Mix(p.h[static_cast<size_t>(Part::View)], view);
        p.h[static_cast<size_t>(Part::ImgVer)] =
            Mix(p.h[static_cast<size_t>(Part::ImgVer)], Mix(views[i].image, version));
        if (i < SAMPLE_VIEWS) {
            p.sample.view[i] = ViewSample{tic, view, views[i].image, version};
        }
        if (s.seen_views.insert(view).second) {
            ++s.current.new_views;
        }
    }
    p.sample.views = static_cast<u32>(count);
    ++s.current.draws;
    if (Rec* const rec = Find(s, vs, fs)) {
        if (rec->draws == 0) {
            rec->first = p.sample;
            rec->vs_hash = vs_hash;
            rec->fs_hash = fs_hash;
        }
        ++rec->draws;
        for (size_t i = 0; i < PARTS; ++i) {
            rec->seq[i] = Mix(rec->seq[i], p.h[i]);
        }
        if (WatchIndex(rec->fs_hash) < WATCH.size() && rec->watch.size() < WATCH_DRAWS) {
            WatchDraw w{};
            w.mesh = p.h[static_cast<size_t>(Part::GeoGuest)];
            w.ubo_g = p.h[static_cast<size_t>(Part::UboGuest)];
            w.img_v = p.h[static_cast<size_t>(Part::ImgVer)];
            w.base_vertex = p.base_vertex; // b85
            w.first_index = p.first_index;
            w.count = p.count;
            w.base_instance = p.base_instance;
            w.instances = p.instances;
            w.geo_c = p.geo_c;
            w.ubos = p.ubos;
            w.ubo = p.ubo;
            if (late_active) {
                w.late = late_acc; // b87
            }
            for (size_t i = 0; i < TILE_VIEWS && i < count; ++i) {
                w.img[i] = views[i].image; // b84
            }
            rec->watch.push_back(w);
        }
    }
    p = Pending{};
}

/// b84 TEX_CHECK, graphics pipeline (GPU thread): whether to check an image a water draw samples
/// (see TEX_CHECK above); None, or why. size = its guest bytes, counted against TEX_BUDGET. An
/// image the GPU wrote (gpu_modified) is checked only the frame it appears: its guest bytes are not
/// its content, so DiagGuestState does not hash them and its new ticks ask for no check.
inline TexWhy TexDue(u32 image, u64 modification, u32 size, bool gpu_modified) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    const u64 now = s.frame_no;
    const u32 gen = ImageSlot(s, image).gen;
    TexTrack& t = s.tex[image];
    if (t.gen != gen || t.seen == 0 || t.seen + 1 < now) {
        t = TexTrack{};
        t.gen = gen;
        t.first = now;
    }
    t.seen = now;
    if (t.checked == now) {
        return TexWhy::None;
    }
    TexWhy why = TexWhy::None;
    if (now - t.first < TEX_FOLLOW) {
        why = t.first == now ? TexWhy::First : TexWhy::Follow;
    } else if (t.modification != modification) {
        why = TexWhy::Version;
    } else if (now - t.checked >= TEX_RECHECK) {
        why = TexWhy::Periodic;
    }
    if (gpu_modified && why != TexWhy::First) {
        why = TexWhy::None;
    }
    if (why == TexWhy::None) {
        return why;
    }
    if (gpu_modified) {
        size = 0;
    }
    if (s.current.tex_bytes != 0 && s.current.tex_bytes + size > TEX_BUDGET) {
        ++s.current.tex_skipped;
        return TexWhy::None;
    }
    s.current.tex_bytes += size;
    t.checked = now;
    t.modification = modification;
    return why;
}

/// b84: the state of a check (see TEX_CHECK above).
inline TexVerdict TexJudge(const TexState& x) {
    if ((x.flags & TEX_DECODING) != 0) {
        return TexVerdict::Decoding;
    }
    if ((x.flags & TEX_GPU_MODIFIED) != 0) {
        return TexVerdict::Gpu;
    }
    if ((x.flags & TEX_CPU_MODIFIED) != 0) {
        return TexVerdict::Cpu;
    }
    if (x.snapshot == 0) {
        return TexVerdict::NoSnap;
    }
    if (x.snapshot != 1) {
        return TexVerdict::Other;
    }
    return x.now_hash == x.upload_hash ? TexVerdict::Same : TexVerdict::Stale;
}

inline std::string TexText(const TexCheck& c) {
    const TexState& x = c.state;
    return fmt::format("image={} why={} state={} gpu={:#x} size={:#x} fmt={} flags={:#x} mod={} "
                       "up_tick={} tick={} up={:016x} now={:016x}",
                       c.image, TEX_WHY_NAMES[static_cast<size_t>(c.why)],
                       TEX_VERDICT_NAMES[static_cast<size_t>(c.verdict)], x.gpu_addr, x.size,
                       x.format, x.flags, x.modification, x.upload_tick, x.frame_tick,
                       x.upload_hash, x.now_hash);
}

/// b84 TEX_CHECK, graphics pipeline (GPU thread): the result of a check TexDue asked for.
inline void TexNote(u32 image, TexWhy why, const TexState& state) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    TexCheck c{image, why, TexJudge(state), state};
    const size_t v = static_cast<size_t>(c.verdict);
    ++s.current.tex_counts[v];
    ++s.tex_total[v];
    if (s.current.tex.size() < TEX_KEPT) {
        s.current.tex.push_back(c);
    }
    if (c.verdict == TexVerdict::Stale && s.tex_logged < TEX_LOGGED) {
        ++s.tex_logged;
        LOG_WARNING(Render_Vulkan, "TEX_CHECK stale frame={} {}", s.frame_no, TexText(c));
    }
}

/// Graphics pipeline (GPU thread): Configure refused the draw after bindings were recorded.
inline void Discard() {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    s.pending = Pending{};
}

/// PopTrace::EndFrame (GPU thread): closes the frame PopTrace calls tick.
inline void EndFrame(u64 tick) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    s.current.tick = tick;
    s.current.t_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - s.start)
                                          .count());
    s.frames.push_back(std::move(s.current));
    s.current = Frame{};
    s.index.clear();
    s.pending = Pending{};
    s.geo_cache.clear(); // b85
    ++s.frame_no;
    if (s.frames.size() > RING) {
        s.frames.pop_front();
    }
}

/// b87: the BIND_LATE line of a watched pair's frame: per kind late/compared/fresh over its draws
/// (F fast uniform, C classic uniform, S storage, H texture handle, D descriptor), the bindings
/// late in some draw (slot and kind: draws late, the first such draw's raw>kept hashes) and the
/// first late handle or descriptor (kind:raw>kept).
inline std::string LateText(u64 n, long long rel, u64 vs, u64 fs,
                            const std::vector<WatchDraw>& draws) {
    struct Named {
        u32 slot;
        LateKind kind;
        u32 draws;
        u32 raw;
        u32 kept;
    };
    std::array<u32, LATE_KINDS> reads{};
    std::array<u32, LATE_KINDS> late{};
    std::array<u32, LATE_KINDS> fresh{};
    std::vector<Named> named;
    std::string hd = "-";
    for (const WatchDraw& x : draws) {
        for (size_t k = 0; k < LATE_KINDS; ++k) {
            reads[k] += x.late.reads[k];
            late[k] += x.late.late[k];
            fresh[k] += x.late.fresh[k];
        }
        for (u32 i = 0; i < x.late.slots; ++i) {
            const LateSlot& ls = x.late.slot[i];
            if (ls.raw == ls.kept) {
                continue;
            }
            auto it = std::find_if(named.begin(), named.end(), [&](const Named& m) {
                return m.slot == ls.slot && m.kind == ls.kind;
            });
            if (it != named.end()) {
                ++it->draws;
            } else {
                named.push_back(Named{ls.slot, ls.kind, 1, ls.raw, ls.kept});
            }
        }
        if (hd == "-" && x.late.first_kind < LATE_KINDS) {
            hd = fmt::format("{}:{:08x}>{:08x}", LATE_CODES[x.late.first_kind], x.late.first_raw,
                             x.late.first_kept);
        }
    }
    std::string kinds;
    for (size_t k = 0; k < LATE_KINDS; ++k) {
        kinds += fmt::format(" {}={}/{}/{}", LATE_CODES[k], late[k], reads[k], fresh[k]);
    }
    std::string list;
    for (size_t i = 0; i < named.size() && i < LATE_LISTED; ++i) {
        const Named& m = named[i];
        list += fmt::format("{}s{}i{}{}:{}#{:08x}>{:08x}", i ? "," : "", m.slot >> 8, m.slot & 0xFF,
                            LATE_CODES[static_cast<size_t>(m.kind)], m.draws, m.raw, m.kept);
    }
    return fmt::format("BIND_LATE n={} rel={} vs={:#x} fs={:#x} draws={}{} late={} named={} hd={}",
                       n, rel, vs, fs, draws.size(), kinds, list.empty() ? "-" : list,
                       named.size(), hd);
}

/// b85: the bytes hash of a draw's uniform binding slot (stage << 8 | index), 0 if it lacks it.
inline u64 SlotContent(const WatchDraw& x, u32 slot) {
    for (size_t k = 0; k < (std::min)(static_cast<size_t>(x.ubos), UBO_KEPT); ++k) {
        if (x.ubo[k].slot == slot) {
            return x.ubo[k].content;
        }
    }
    return 0;
}

/// b85: the uniform binding slots whose bytes differ between the draws (or that only some draws
/// have), in slot order.
inline std::vector<u32> VarySlots(const std::vector<WatchDraw>& draws) {
    std::vector<u32> all;
    for (const WatchDraw& x : draws) {
        for (size_t k = 0; k < (std::min)(static_cast<size_t>(x.ubos), UBO_KEPT); ++k) {
            all.push_back(x.ubo[k].slot);
        }
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::vector<u32> vary;
    for (const u32 slot : all) {
        bool differs = false;
        bool seen = false;
        u64 first = 0;
        for (const WatchDraw& x : draws) {
            bool has = false;
            u64 content = 0;
            for (size_t k = 0; k < (std::min)(static_cast<size_t>(x.ubos), UBO_KEPT); ++k) {
                if (x.ubo[k].slot == slot) {
                    has = true;
                    content = x.ubo[k].content;
                    break;
                }
            }
            if (!has || (seen && content != first)) {
                differs = true;
                break;
            }
            if (!seen) {
                seen = true;
                first = content;
            }
        }
        if (differs) {
            vary.push_back(slot);
        }
    }
    return vary;
}

inline std::string SampleText(const Sample& x) {
    std::string views;
    for (size_t i = 0; i < (std::min)(static_cast<size_t>(x.views), SAMPLE_VIEWS); ++i) {
        const ViewSample& v = x.view[i];
        if (v.image == NO_IMAGE) {
            views += fmt::format("{}{:#x}:{}", i ? "," : "", v.tic, v.view);
        } else {
            views += fmt::format("{}{:#x}:{}@{}#{:x}", i ? "," : "", v.tic, v.view, v.image,
                                 v.version);
        }
    }
    std::string vertex;
    for (size_t i = 0; i < (std::min)(static_cast<size_t>(x.vertices), SAMPLE_VERTEX); ++i) {
        vertex += fmt::format("{}{:#x}/{}/{}", i ? "," : "", x.vertex[i].addr, x.vertex[i].size,
                              x.vertex[i].id);
    }
    std::string ubos;
    for (size_t i = 0; i < (std::min)(static_cast<size_t>(x.ubos), SAMPLE_UBOS); ++i) {
        ubos += fmt::format("{}s{}i{}/{}#{:x}", i ? "," : "", x.ubo[i].stage, x.ubo[i].index,
                            x.ubo[i].size, x.ubo[i].hash);
    }
    return fmt::format("pipe={:#x} fb={:#x} views={}[{}] idx={:#x}/{}/{} vtx={}[{}] ubo={}[{}]",
                       x.pipe, x.fb, x.views, views, x.index.addr, x.index.size, x.index.id,
                       x.vertices, vertex, x.ubos, ubos);
}

/// b81: the sampled slots of a part that differ between two samples (ubo_c: uniform buffer
/// bindings by stage and slot; img_v: textures by position), "-" for the other parts or none.
inline std::string DiffText(Part part, const Sample& a, const Sample& b) {
    std::string text;
    if (part == Part::UboContent) {
        const size_t n = (std::min)(static_cast<size_t>((std::max)(a.ubos, b.ubos)), SAMPLE_UBOS);
        for (size_t i = 0; i < n; ++i) {
            const UboSample& x = a.ubo[i];
            const UboSample& y = b.ubo[i];
            if (x.stage != y.stage || x.index != y.index || x.size != y.size || x.hash != y.hash) {
                text += fmt::format("{}s{}i{}/{}#{:x}->s{}i{}/{}#{:x}", text.empty() ? "" : ",",
                                    x.stage, x.index, x.size, x.hash, y.stage, y.index, y.size,
                                    y.hash);
            }
        }
    } else if (part == Part::ImgVer) {
        const size_t n = (std::min)(static_cast<size_t>((std::max)(a.views, b.views)), SAMPLE_VIEWS);
        for (size_t i = 0; i < n; ++i) {
            const ViewSample& x = a.view[i];
            const ViewSample& y = b.view[i];
            if (x.image != y.image || x.version != y.version) {
                text += fmt::format("{}v{}:{}#{:x}->{}#{:x}", text.empty() ? "" : ",", i, x.image,
                                    x.version, y.image, y.version);
            }
        }
    }
    return text.empty() ? "-" : text;
}

inline std::string CountsText(const std::array<u32, PARTS>& c) {
    std::string text;
    for (size_t i = 0; i < PARTS; ++i) {
        text += fmt::format("{}{}", i ? "/" : "", c[i]);
    }
    return text;
}

inline std::string PartsText() {
    std::string text;
    for (size_t i = 0; i < PARTS; ++i) {
        text += fmt::format("{}{}", i ? "/" : "", PART_NAMES[i]);
    }
    return text;
}

/// A change of a part value after STEADY identical frames: a flip is a run of 1-SHORT frames
/// [a, e] with other values followed by STEADY frames equal to the frames before; a shift is a
/// new value at a that then holds for STEADY frames (e = a); a bump (b81) is a run of 1-SHORT
/// frames [a, e] with other values followed by STEADY frames of a third value.
struct Change {
    size_t a = 0;
    size_t e = 0;
    Kind kind = Kind::Flip;
};

/// True when v[begin .. begin + STEADY) are all equal to v[begin] and non-zero.
inline bool SteadySame(const std::vector<u64>& v, size_t begin) {
    if (v[begin] == 0) {
        return false;
    }
    for (size_t k = 1; k < STEADY; ++k) {
        if (v[begin + k] != v[begin]) {
            return false;
        }
    }
    return true;
}

/// Changes of a value series (0 = the pair was absent) where the draw count held throughout.
inline std::vector<Change> FindChanges(const std::vector<u64>& v, const std::vector<u64>& draws) {
    std::vector<Change> changes;
    const size_t count = v.size();
    size_t a = STEADY;
    while (a + STEADY < count) {
        if (!SteadySame(v, a - STEADY) || v[a] == v[a - 1]) {
            ++a;
            continue;
        }
        const u64 before = v[a - 1];
        const auto same_draws = [&](size_t e) {
            for (size_t f = a; f <= e + STEADY; ++f) {
                if (draws[f] != draws[a - 1]) {
                    return false;
                }
            }
            return true;
        };
        if (a + STEADY <= count && SteadySame(v, a)) {
            // The new value holds: a shift at a.
            if (same_draws(a)) {
                changes.push_back(Change{a, a, Kind::Shift});
            }
            a += STEADY;
            continue;
        }
        // A run of 1-SHORT other values, then STEADY frames of one value.
        size_t run = 0;
        for (size_t k = 1; k <= SHORT && a + k + STEADY <= count; ++k) {
            if (v[a + k - 1] == before) {
                break;
            }
            if (SteadySame(v, a + k)) {
                run = k;
                break;
            }
        }
        if (run == 0) {
            ++a;
            continue;
        }
        const size_t e = a + run - 1;
        const u64 after = v[e + 1];
        if (!same_draws(e)) {
            a = e + 1;
            continue;
        }
        if (after == before) {
            changes.push_back(Change{a, e, Kind::Flip});
        } else if (after != v[e]) {
            changes.push_back(Change{a, e, Kind::Bump});
        }
        a = e + STEADY + 1;
    }
    return changes;
}

/// b82: BIND_WATCH_FRAME per frame and BIND_WATCH per watched pair and frame (see WATER_WATCH).
inline void WatchReport(u64 n, const std::vector<const Frame*>& window, size_t mark,
                        const ankerl::unordered_dense::map<u64, std::vector<const Rec*>>& rows,
                        std::vector<std::string>& lines) {
    const size_t count = window.size();
    const auto rel = [&](size_t f) {
        return static_cast<long long>(f) - static_cast<long long>(mark);
    };
    struct Watched {
        size_t which = 0;
        u64 vs = 0;
        u64 fs = 0;
        const std::vector<const Rec*>* row = nullptr;
    };
    std::vector<Watched> watched;
    for (const auto& [key, row] : rows) {
        for (const Rec* r : row) {
            if (r != nullptr) {
                const size_t which = WatchIndex(r->fs_hash);
                if (which < WATCH.size()) {
                    watched.push_back(Watched{which, r->vs, r->fs, &row});
                }
                break;
            }
        }
    }
    std::sort(watched.begin(), watched.end(), [](const Watched& a, const Watched& b) {
        if (a.which != b.which) {
            return a.which < b.which;
        }
        return a.vs != b.vs ? a.vs < b.vs : a.fs < b.fs;
    });
    const size_t total_pairs = watched.size();
    if (watched.size() > WATCH_PAIRS) {
        watched.resize(WATCH_PAIRS);
    }
    for (size_t f = 0; f < count; ++f) {
        std::array<u32, 3> draws{};
        std::array<u32, 3> pairs{};
        for (const Rec& r : window[f]->recs) {
            const size_t which = WatchIndex(r.fs_hash);
            if (which < WATCH.size()) {
                draws[which] += r.draws;
                ++pairs[which];
            }
        }
        lines.push_back(fmt::format(
            "BIND_WATCH_FRAME n={} rel={} tick={} t_ms={} draws={} w0193={}/{} wdbfa={}/{} "
            "w6523={}/{} geo_hashed={} geo_bytes={} geo_skipped={} late_bytes={} late_skipped={}",
            n, rel(f), window[f]->tick, window[f]->t_ms, window[f]->draws, draws[0], pairs[0],
            draws[1], pairs[1], draws[2], pairs[2], window[f]->geo_hashed, window[f]->geo_bytes,
            window[f]->geo_skipped, window[f]->late_bytes, window[f]->late_skipped));
        // b84 TEX_CHECK: the frame's checks (all but the "same" ones of follow-up checks).
        const Frame& fr = *window[f];
        u32 checks = 0;
        for (const u32 c : fr.tex_counts) {
            checks += c;
        }
        if (checks == 0 && fr.tex_skipped == 0) {
            continue;
        }
        std::string counts;
        for (size_t v = 0; v < TEX_VERDICTS; ++v) {
            counts += fmt::format(" {}={}", TEX_VERDICT_NAMES[v], fr.tex_counts[v]);
        }
        lines.push_back(fmt::format("BIND_TEX_FRAME n={} rel={} checks={}{} skipped={} bytes={} "
                                    "kept={}",
                                    n, rel(f), checks, counts, fr.tex_skipped, fr.tex_bytes,
                                    fr.tex.size()));
        for (const TexCheck& c : fr.tex) {
            if (c.verdict == TexVerdict::Same && c.why != TexWhy::First) {
                continue;
            }
            lines.push_back(fmt::format("BIND_TEX n={} rel={} {}", n, rel(f), TexText(c)));
        }
    }
    for (const Watched& w : watched) {
        std::string previous;
        for (size_t f = 0; f < count; ++f) {
            const Rec* r = (*w.row)[f];
            if (r == nullptr) {
                lines.push_back(fmt::format(
                    "BIND_WATCH n={} rel={} tick={} fs_hash={:016x} vs={:#x} fs={:#x} draws=0",
                    n, rel(f), window[f]->tick, WATCH[w.which], w.vs, w.fs));
                continue;
            }
            std::string seq;
            for (size_t p = 0; p < PARTS; ++p) {
                seq += fmt::format("{}{:016x}", p ? "/" : "", r->seq[p]);
            }
            lines.push_back(fmt::format(
                "BIND_WATCH n={} rel={} tick={} fs_hash={:016x} vs_hash={:016x} vs={:#x} fs={:#x} "
                "draws={} listed={} seq={}",
                n, rel(f), window[f]->tick, WATCH[w.which], r->vs_hash, w.vs, w.fs, r->draws,
                r->watch.size(), seq));
            // b85: the uniform bindings of the frame's draws: the first draw's (slot, size, bytes
            // hash) and the slots whose bytes differ between the draws (or that some draws lack).
            const std::vector<u32> vary = VarySlots(r->watch);
            std::string slots;
            std::string varying;
            if (!r->watch.empty()) {
                const WatchDraw& x = r->watch[0];
                for (size_t k = 0; k < (std::min)(static_cast<size_t>(x.ubos), UBO_KEPT); ++k) {
                    slots += fmt::format("{}s{}i{}:{:x}#{:08x}", k ? "," : "", x.ubo[k].slot >> 8,
                                         x.ubo[k].slot & 0xFF, x.ubo[k].size,
                                         static_cast<u32>(x.ubo[k].content));
                }
                for (size_t k = 0; k < vary.size(); ++k) {
                    varying += fmt::format("{}s{}i{}", k ? "," : "", vary[k] >> 8, vary[k] & 0xFF);
                }
                lines.push_back(fmt::format(
                    "BIND_WATCH_UBO n={} rel={} vs={:#x} fs={:#x} ubos={} slots={} vary={}", n,
                    rel(f), w.vs, w.fs, x.ubos, slots.empty() ? "-" : slots,
                    varying.empty() ? "-" : varying));
                if (Get().late) {
                    lines.push_back(LateText(n, rel(f), w.vs, w.fs, r->watch));
                }
            }
            // b83: the draws, WATCH_PER_LINE per line, 32 bits each: b85 mesh/ubo_g/img_v/i0/i1/
            // bv/fi/nc/bi/ni (draw parameters)/geo/u0/u1/u2 (bytes of the first UBO_VARY varying
            // slots, 0 when the draw lacks the slot)
            for (size_t d0 = 0; d0 < r->watch.size(); d0 += WATCH_PER_LINE) {
                std::string list;
                const size_t d1 = (std::min)(r->watch.size(), d0 + WATCH_PER_LINE);
                for (size_t d = d0; d < d1; ++d) {
                    const WatchDraw& x = r->watch[d];
                    std::array<u32, UBO_VARY> u{};
                    for (size_t k = 0; k < UBO_VARY && k < vary.size(); ++k) {
                        u[k] = static_cast<u32>(SlotContent(x, vary[k]));
                    }
                    list += fmt::format("{}{:08x}/{:08x}/{:08x}/{:08x}/{:08x}/{:08x}/{:08x}/{:08x}/"
                                        "{:08x}/{:08x}/{:08x}/{:08x}/{:08x}/{:08x}",
                                        d == d0 ? "" : ",", static_cast<u32>(x.mesh),
                                        static_cast<u32>(x.ubo_g), static_cast<u32>(x.img_v),
                                        x.img[0], x.img[1], x.base_vertex, x.first_index, x.count,
                                        x.base_instance, x.instances, static_cast<u32>(x.geo_c),
                                        u[0], u[1], u[2]);
                }
                lines.push_back(fmt::format("BIND_WATCH_DRAWS n={} rel={} vs={:#x} fs={:#x} at={} d={}",
                                            n, rel(f), w.vs, w.fs, d0, list));
            }
            std::string sample = SampleText(r->first);
            if (sample != previous) {
                lines.push_back(fmt::format("BIND_WATCH_FIRST n={} rel={} vs={:#x} fs={:#x} {}", n,
                                            rel(f), w.vs, w.fs, sample));
            }
            previous = std::move(sample);
        }
    }
    lines.push_back(fmt::format("BIND_WATCH_END n={} pairs={} listed={} watch_draws={}", n,
                                total_pairs, watched.size(), WATCH_DRAWS));
    // b84: the checks since the start, by verdict.
    State& s = Get();
    std::string totals;
    for (size_t v = 0; v < TEX_VERDICTS; ++v) {
        totals += fmt::format(" {}={}", TEX_VERDICT_NAMES[v], s.tex_total[v]);
    }
    lines.push_back(fmt::format("BIND_TEX_END n={}{} tracked={} logged={}", n, totals, s.tex.size(),
                                s.tex_logged));
}

/// PopTrace::Report: BIND_* lines for the frames [first_tick, last_tick], rel to mark_tick.
inline void Report(u64 n, u64 first_tick, u64 mark_tick, u64 last_tick,
                   std::vector<std::string>& lines) {
    State& s = Get();
    std::scoped_lock lock{s.mutex};
    std::vector<const Frame*> window;
    for (const Frame& frame : s.frames) {
        if (frame.tick >= first_tick && frame.tick <= last_tick) {
            window.push_back(&frame);
        }
    }
    const size_t count = window.size();
    if (count == 0) {
        lines.push_back(fmt::format("BIND_REPORT n={} frames=0", n));
        return;
    }
    size_t mark = count - 1;
    for (size_t f = 0; f < count; ++f) {
        if (window[f]->tick == mark_tick) {
            mark = f;
        }
    }
    const auto rel = [&](size_t f) {
        return static_cast<long long>(f) - static_cast<long long>(mark);
    };
    ankerl::unordered_dense::map<u64, std::vector<const Rec*>> rows;
    for (size_t f = 0; f < count; ++f) {
        for (const Rec& r : window[f]->recs) {
            auto& row = rows[r.vs ^ (r.fs * 0x9E3779B97F4A7C15ULL)];
            if (row.empty()) {
                row.assign(count, nullptr);
            }
            row[f] = &r;
        }
    }
    using Counts = std::array<u32, PARTS>;
    std::array<std::vector<Counts>, 3> per_frame{std::vector<Counts>(count),
                                                 std::vector<Counts>(count),
                                                 std::vector<Counts>(count)};
    std::array<Counts, 3> total{};
    std::vector<std::string> listed;
    std::vector<u64> value(count), draws(count);
    for (const auto& [key, row] : rows) {
        for (size_t f = 0; f < count; ++f) {
            draws[f] = row[f] ? row[f]->draws : 0;
        }
        for (size_t p = 0; p < PARTS; ++p) {
            for (size_t f = 0; f < count; ++f) {
                value[f] = row[f] ? Mix(row[f]->seq[p], row[f]->draws) : 0;
            }
            for (const Change& c : FindChanges(value, draws)) {
                const size_t k = static_cast<size_t>(c.kind);
                for (size_t f = c.a; f <= c.e; ++f) {
                    ++per_frame[k][f][p];
                }
                ++total[k][p];
                if (listed.size() < MAX_LISTED) {
                    const Rec* before = row[c.a - 1];
                    const Rec* during = row[c.a];
                    const Rec* after = row[c.e + 1];
                    const Part part = static_cast<Part>(p);
                    listed.push_back(fmt::format(
                        "BIND_FLIP n={} kind={} part={} rel={}..{} vs={:#x} fs={:#x} "
                        "vs_hash={:016x} fs_hash={:016x} draws={} diff: {} after_diff: {} "
                        "before: {} during: {}",
                        n, KIND_NAMES[k], PART_NAMES[p], rel(c.a), rel(c.e), before->vs,
                        before->fs, before->vs_hash, before->fs_hash, before->draws,
                        DiffText(part, before->first, during->first),
                        c.kind == Kind::Bump ? DiffText(part, during->first, after->first) : "-",
                        SampleText(before->first), SampleText(during->first)));
                }
            }
        }
    }
    u64 draws_sum = 0;
    for (const Frame* frame : window) {
        draws_sum += frame->draws;
    }
    lines.push_back(fmt::format(
        "BIND_REPORT n={} frames={} mark_tick={} draws_avg={} keys={} parts={} flips={} shifts={} "
        "bumps={} listed={}",
        n, count, window[mark]->tick, draws_sum / count, rows.size(), PartsText(),
        CountsText(total[0]), CountsText(total[1]), CountsText(total[2]), listed.size()));
    for (size_t f = 0; f < count; ++f) {
        const Frame& frame = *window[f];
        lines.push_back(fmt::format(
            "BIND_FRAME n={} rel={} tick={} t_ms={} draws={} keys={} new_views={} uploads={} "
            "dropped_keys={} flip={} shift={} bump={}",
            n, rel(f), frame.tick, frame.t_ms, frame.draws, frame.recs.size(), frame.new_views,
            frame.uploads, frame.dropped_keys, CountsText(per_frame[0][f]),
            CountsText(per_frame[1][f]), CountsText(per_frame[2][f])));
    }
    for (std::string& line : listed) {
        lines.push_back(std::move(line));
    }
    WatchReport(n, window, mark, rows, lines);
    lines.push_back(fmt::format("BIND_REPORT end n={}", n));
}

} // namespace VideoCommon::BindPop
#endif
