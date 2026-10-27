// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// b59/b61 PASS_CAPTURE (EDEN_PASS_CAPTURE=1, diagnostic builds only).
//
// F3 captures the next EDEN_PASS_CAPTURE_FRAMES frames (default 2, at most 4) render pass by
// render pass. When a render pass that ran at least one guest draw ends, every attachment
// (colour, depth and stencil aspects; base mip; at most 6 layers; the render area) is copied to a
// host buffer (tag "pass"). Draws whose fragment shader is listed in EDEN_PASS_CAPTURE_FOCUS
// (default 9e71b54b624b118a, the ground lighting draw of the 10-17 bisect; "none" for no focus)
// get a render pass of their own, at most EDEN_PASS_CAPTURE_FOCUS_MAX per frame (default 4, at
// most 1024): the pass holding the earlier draws ends, the focus framebuffer's attachments are
// copied ("before"), every image view the draw samples or stores to is copied ("input"), the
// guest constant buffers of every stage are read from game memory, and the pass ends right after
// the draw ("after").
//
// b61: block-compressed images (BCn, ASTC) are copied as raw blocks. Once
// EDEN_PASS_CAPTURE_SPILL_MB (default 1024) of copies wait in host memory, the next draw first
// ends the open render pass, waits for the GPU and writes them out ("spill"), so host memory no
// longer bounds a capture; EDEN_PASS_CAPTURE_MB (default 16384) only bounds the bytes copied. A
// copy whose bytes equal an earlier copy of the same capture is not written again: its index
// row names the earlier file.
//
// log/pass_capture_<unix time>_<n>/ gets summary.txt, passes.tsv, draws.tsv, spills.tsv,
// index.tsv (one img_<id>.raw per distinct copy: tightly packed texels, or blocks, of the host
// Vulkan format) and cbuf.tsv (one cbuf_<id>.bin per constant buffer). The game stalls for the
// copies and the writes (seconds).
//
// Copies skip MSAA images, unknown formats and copies above 256 MiB; skipped copies are still
// listed with the reason. At most EDEN_PASS_CAPTURE_MAX captures (default 3, at most 8) per
// session.
//
// The focus split adds two render pass breaks per focus draw while a capture runs, and each
// spill one more; nothing else changes outside the capture frames.

#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) short
#if !defined(_M_ARM64) && !defined(__aarch64__)
    __stdcall
#endif
    GetAsyncKeyState(int vKey);
#endif

namespace Vulkan::PassCapture {

using VideoCore::Surface::PixelFormat;

inline constexpr int CAPTURE_KEY = 0x72; // VK_F3. F8 is Eden's "Change Adapting Filter".
inline constexpr u32 MAX_LAYERS = 6;
inline constexpr u64 MAX_COPY_BYTES = u64{256} << 20;
inline constexpr u32 MAX_CBUF_BYTES = 64 * 1024;

struct Config {
    bool enabled = false;
    u32 frames = 2;
    u32 max_captures = 3;
    u32 focus_max = 4;
    u64 budget = u64{16384} << 20;
    u64 spill = u64{1024} << 20;
    std::unordered_set<u64> focus;
    std::string focus_text;
};

inline u64 EnvNumber(const char* name, u64 fallback, u64 low, u64 high) {
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end{};
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return std::clamp<u64>(parsed, low, high);
}

inline const Config& GetConfig() {
    static const Config config = [] {
        Config result;
        const char* const value = std::getenv("EDEN_PASS_CAPTURE");
        result.enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
        result.frames = static_cast<u32>(EnvNumber("EDEN_PASS_CAPTURE_FRAMES", 2, 1, 4));
        result.max_captures = static_cast<u32>(EnvNumber("EDEN_PASS_CAPTURE_MAX", 3, 1, 8));
        result.focus_max = static_cast<u32>(EnvNumber("EDEN_PASS_CAPTURE_FOCUS_MAX", 4, 1, 1024));
        result.budget = EnvNumber("EDEN_PASS_CAPTURE_MB", 16384, 64, 262144) << 20;
        result.spill = EnvNumber("EDEN_PASS_CAPTURE_SPILL_MB", 1024, 64, 8192) << 20;
        const char* const focus = std::getenv("EDEN_PASS_CAPTURE_FOCUS");
        const std::string list = focus != nullptr ? focus : "9e71b54b624b118a";
        size_t begin = 0;
        while (begin < list.size()) {
            size_t end = list.find_first_of(", ;", begin);
            if (end == std::string::npos) {
                end = list.size();
            }
            const std::string token = list.substr(begin, end - begin);
            char* parse_end{};
            const unsigned long long hash = std::strtoull(token.c_str(), &parse_end, 16);
            if (!token.empty() && *parse_end == '\0' && hash != 0) {
                result.focus.insert(static_cast<u64>(hash));
                result.focus_text += fmt::format("{}{:016x}", result.focus_text.empty() ? "" : ",",
                                                 static_cast<u64>(hash));
            }
            begin = end + 1;
        }
        if (result.focus_text.empty()) {
            result.focus_text = "none";
        }
        return result;
    }();
    return config;
}

enum class Tag : u8 { Pass, Before, After, Input };

inline const char* TagName(Tag tag) {
    switch (tag) {
    case Tag::Pass:
        return "pass";
    case Tag::Before:
        return "before";
    case Tag::After:
        return "after";
    case Tag::Input:
        return "input";
    }
    return "?";
}

template <typename T>
inline u64 Bits(T handle) {
    u64 bits = 0;
    std::memcpy(&bits, &handle, (std::min)(sizeof(handle), sizeof(bits)));
    return bits;
}

/// Bytes per texel of `aspect` of `format` in a buffer copy; 0 when the copy is not supported
/// here (block-compressed, packed 3-byte and unknown formats).
inline u32 TexelBytes(VkFormat format, VkImageAspectFlagBits aspect) {
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
        switch (format) {
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 1;
        default:
            return 0;
        }
    }
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
        switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT:
            return 2;
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 4;
        default:
            return 0;
        }
    }
    if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        return 0;
    }
    switch (format) {
    case VK_FORMAT_R4G4_UNORM_PACK8:
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_SRGB:
        return 1;
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
    case VK_FORMAT_A4B4G4R4_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT:
        return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SNORM:
    case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UINT_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT:
        return 4;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32_UINT:
    case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R32G32B32_SFLOAT:
        return 12;
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}

struct Block {
    u32 width = 1;
    u32 height = 1;
    u32 bytes = 0; // 0: the copy is not supported here
};

/// Texel block of `aspect` of `format` in a buffer copy: the compression block of BCn and ASTC
/// formats, one texel (TexelBytes) otherwise. A view's format shares its image's block size: view
/// compatibility keeps compressed views within their own compression class.
inline Block BlockOf(VkFormat format, VkImageAspectFlagBits aspect) {
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return {4, 4, 8};
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
            return {4, 4, 16};
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
            return {5, 4, 16};
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
            return {5, 5, 16};
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
            return {6, 5, 16};
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
            return {6, 6, 16};
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
            return {8, 5, 16};
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
            return {8, 6, 16};
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
            return {8, 8, 16};
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
            return {10, 5, 16};
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
            return {10, 6, 16};
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
            return {10, 8, 16};
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
            return {10, 10, 16};
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
            return {12, 10, 16};
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return {12, 12, 16};
        default:
            break;
        }
    }
    return {1, 1, TexelBytes(format, aspect)};
}

/// Two independent 64-bit hashes of `data` (b61 duplicate detection).
inline std::pair<u64, u64> HashBytes(std::span<const u8> data) {
    u64 a = 0xcbf29ce484222325ULL ^ data.size();
    u64 b = 0x9e3779b97f4a7c15ULL + data.size();
    const auto mix = [&](u64 word) {
        a = (a ^ word) * 0x100000001b3ULL;
        b += word * 0xc2b2ae3d27d4eb4fULL;
        b = ((b << 31) | (b >> 33)) * 0x9e3779b185ebca87ULL;
    };
    size_t i = 0;
    for (; i + 8 <= data.size(); i += 8) {
        u64 word;
        std::memcpy(&word, data.data() + i, 8);
        mix(word);
    }
    if (i < data.size()) {
        u64 tail = 0;
        std::memcpy(&tail, data.data() + i, data.size() - i);
        mix(tail);
    }
    a ^= a >> 29;
    b ^= b >> 32;
    return {a, b};
}

struct Copy {
    u64 id{};
    u32 frame{};
    u64 pass{};
    u32 group{}; // focus draw number (1-based) for before/input/after copies, 0 otherwise
    Tag tag{};
    u32 slot{}; // attachment index (Framebuffer::Images() order) or image view binding index
    u64 view{};  // input copies: image view id
    VkImage image{};
    VkImageAspectFlagBits aspect{};
    u32 mip{};
    u32 layer{};
    VkOffset3D offset{};
    VkExtent3D extent{};
    VkFormat host_format{};
    PixelFormat format{};
    u32 texel_bytes{}; // bytes per texel block
    u32 block_width = 1;
    u32 block_height = 1;
    u64 bytes{};
    std::string skip; // non-empty: not copied, the reason
    vk::Buffer buffer; // held until written (spill or end of the capture)
    std::string file;  // written: own file, or the earlier copy with the same bytes
    u64 hash_a{};
    u64 hash_b{};
};

struct Region {
    VkImage image;
    VkBuffer buffer;
    VkBufferImageCopy copy;
};

struct PassInfo {
    bool noted = false;
    VkFramebuffer framebuffer{};
    VkExtent2D area{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::vector<PixelFormat> formats; // one per attachment, Framebuffer::Images() order
    u32 draws = 0;
    u32 focus_draws = 0;
    u32 other_framebuffer_draws = 0;
    bool after = false; // ended right after a focus draw that had the pass to itself
};

struct CbufDump {
    u64 id{};
    u32 frame{};
    u64 pass{};
    u32 group{};
    u64 fs{};
    u32 stage{};
    u32 index{};
    u64 address{};
    u32 size{};
    u32 bytes{};
    std::vector<u8> data; // released once written
    std::string file;
};

struct State {
    std::recursive_mutex mutex;
    MemoryAllocator* allocator = nullptr;
    const Device* device = nullptr;
    bool announced = false;
    bool key_was_down = false;
    bool limit_logged = false;
    u32 captures = 0;
    u32 frame = 0;
    u64 pass = 0;
    u64 next_id = 0;
    u64 used = 0;
    u64 held = 0; // bytes of copies waiting in host memory
    u64 held_max = 0;
    u32 spills = 0;
    size_t first_unwritten = 0;
    size_t first_unwritten_cbuf = 0;
    u64 written = 0;
    u64 written_bytes = 0;
    u64 duplicates = 0;
    u64 duplicate_bytes = 0;
    u64 write_failed = 0;
    std::unordered_map<u64, std::vector<size_t>> by_hash; // hash_a -> written copies
    std::string spills_tsv;
    u64 budget_skips = 0;
    u64 draws = 0;
    u64 empty_passes = 0;
    u64 mismatched_passes = 0;
    u32 focus_this_frame = 0;
    u32 focus_seq = 0;
    u32 focus_skipped = 0;
    bool focus_split_pending = false;
    u64 focus_views_fs = 0;
    std::vector<VideoCommon::ImageViewId> focus_views;
    PassInfo current;
    std::vector<Copy> copies;
    std::vector<CbufDump> cbufs;
    std::string passes_tsv;
    std::string draws_tsv;
    std::filesystem::path dir;
    std::chrono::steady_clock::time_point started{};
};

// Never destroyed: the copy buffers must go before the device (Shutdown, ~RasterizerVulkan).
inline State& GetState() {
    static State* const state = new State{};
    return *state;
}

inline std::atomic<bool> capturing{false};

inline bool Capturing() {
    return capturing.load(std::memory_order_relaxed);
}

inline bool IsFocus(u64 fragment_hash) {
    return GetConfig().focus.contains(fragment_hash);
}

inline std::vector<PixelFormat> AttachmentFormats(const Framebuffer* framebuffer) {
    std::vector<PixelFormat> formats;
    const auto& key = framebuffer->RenderPassKeyBase();
    for (const PixelFormat format : key.color_formats) {
        if (format != PixelFormat::Invalid) {
            formats.push_back(format);
        }
    }
    if (key.depth_format != PixelFormat::Invalid) {
        formats.push_back(key.depth_format);
    }
    return formats;
}

inline void AddCopy(State& state, std::vector<Region>& regions, Copy copy, const char* skip) {
    const Config& config = GetConfig();
    copy.id = state.next_id++;
    copy.frame = state.frame;
    const Block block = BlockOf(copy.host_format, copy.aspect);
    copy.texel_bytes = block.bytes;
    copy.block_width = block.width;
    copy.block_height = block.height;
    copy.bytes = u64{(copy.extent.width + block.width - 1) / block.width} *
                 ((copy.extent.height + block.height - 1) / block.height) * copy.extent.depth *
                 block.bytes;
    if (skip == nullptr && copy.texel_bytes == 0) {
        skip = "format";
    }
    if (skip == nullptr && copy.bytes == 0) {
        skip = "empty";
    }
    if (skip == nullptr && copy.bytes > MAX_COPY_BYTES) {
        skip = "too_big";
    }
    if (skip == nullptr && state.used + copy.bytes > config.budget) {
        skip = "budget";
        ++state.budget_skips;
    }
    if (skip == nullptr) {
        const VkBufferCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = copy.bytes,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        try {
            copy.buffer = state.allocator->CreateBuffer(ci, MemoryUsage::Download);
        } catch (const std::exception&) {
            skip = "alloc";
        }
        if (skip == nullptr && !copy.buffer.IsHostVisible()) {
            copy.buffer = vk::Buffer{};
            skip = "not_mapped";
        }
    }
    if (skip == nullptr) {
        state.used += copy.bytes;
        state.held += copy.bytes;
        state.held_max = (std::max)(state.held_max, state.held);
        regions.push_back(Region{
            .image = copy.image,
            .buffer = *copy.buffer,
            .copy =
                VkBufferImageCopy{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource =
                        VkImageSubresourceLayers{
                            .aspectMask = static_cast<VkImageAspectFlags>(copy.aspect),
                            .mipLevel = copy.mip,
                            .baseArrayLayer = copy.layer,
                            .layerCount = 1,
                        },
                    .imageOffset = copy.offset,
                    .imageExtent = copy.extent,
                },
        });
    } else {
        copy.skip = skip;
    }
    state.copies.push_back(std::move(copy));
}

/// Records the copies outside any render pass. Images stay in VK_IMAGE_LAYOUT_GENERAL.
inline void RecordRegions(Scheduler& scheduler, std::vector<Region>&& regions) {
    if (regions.empty()) {
        return;
    }
    scheduler.Record([regions = std::move(regions)](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, before);
        for (const Region& region : regions) {
            cmdbuf.CopyImageToBuffer(region.image, VK_IMAGE_LAYOUT_GENERAL, region.buffer,
                                     region.copy);
        }
        // Copied bytes -> host read; ALL_COMMANDS also makes later writes to the copied images
        // wait for the copies' reads.
        const VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
                               after);
    });
}

inline void AddAttachment(State& state, std::vector<Region>& regions, Tag tag, u32 slot,
                          u64 pass, u32 group, VkImage image, const VkImageSubresourceRange& range,
                          PixelFormat format, VkExtent2D area, VkSampleCountFlagBits samples) {
    const VkFormat host =
        MaxwellToVK::SurfaceFormat(*state.device, FormatType::Optimal, true, format).format;
    const u32 layers = range.layerCount == VK_REMAINING_ARRAY_LAYERS
                           ? 1
                           : (std::min)(range.layerCount, MAX_LAYERS);
    for (const VkImageAspectFlagBits aspect :
         {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT}) {
        if ((range.aspectMask & aspect) == 0) {
            continue;
        }
        for (u32 layer = 0; layer < layers; ++layer) {
            Copy copy{};
            copy.pass = pass;
            copy.group = group;
            copy.tag = tag;
            copy.slot = slot;
            copy.image = image;
            copy.aspect = aspect;
            copy.mip = range.baseMipLevel;
            copy.layer = range.baseArrayLayer + layer;
            copy.extent = VkExtent3D{area.width, area.height, 1};
            copy.host_format = host;
            copy.format = format;
            AddCopy(state, regions, std::move(copy),
                    samples != VK_SAMPLE_COUNT_1_BIT ? "msaa" : nullptr);
        }
    }
}

inline void AddView(State& state, std::vector<Region>& regions, u32 slot, u64 pass, u32 group,
                    VideoCommon::ImageViewId id, const ImageView& view) {
    using VideoCore::Surface::SurfaceType;
    const VkFormat host =
        MaxwellToVK::SurfaceFormat(*state.device, FormatType::Optimal, true, view.format).format;
    const bool is_3d = view.type == VideoCommon::ImageViewType::e3D;
    const bool slice = True(view.flags & VideoCommon::ImageViewFlagBits::Slice);
    u32 width = view.size.width;
    u32 height = view.size.height;
    const char* skip = view.Samples() != VK_SAMPLE_COUNT_1_BIT ? "msaa" : nullptr;
    if (view.IsRescaled()) {
        // As Image::ScaleUp: the width always, the height of 2D images only. A scaled mip level
        // may round differently from the scaled image, so only level 0 is copied.
        const bool image_2d = !is_3d && !slice && view.type != VideoCommon::ImageViewType::e1D &&
                              view.type != VideoCommon::ImageViewType::e1DArray;
        width = Settings::values.resolution_info.ScaleUp(width);
        if (image_2d) {
            height = Settings::values.resolution_info.ScaleUp(height);
        }
        if (view.range.base.level != 0 && skip == nullptr) {
            skip = "rescaled_mip";
        }
    }
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (VideoCore::Surface::GetFormatType(view.format)) {
    case SurfaceType::Depth:
        aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;
    case SurfaceType::Stencil:
        aspects = VK_IMAGE_ASPECT_STENCIL_BIT;
        break;
    case SurfaceType::DepthStencil:
        aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        break;
    default:
        break;
    }
    const u32 layers = (is_3d || slice) ? 1
                                        : (std::min)(static_cast<u32>(view.range.extent.layers),
                                                     MAX_LAYERS);
    for (const VkImageAspectFlagBits aspect :
         {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT}) {
        if ((aspects & aspect) == 0) {
            continue;
        }
        for (u32 layer = 0; layer < layers; ++layer) {
            Copy copy{};
            copy.pass = pass;
            copy.group = group;
            copy.tag = Tag::Input;
            copy.slot = slot;
            copy.view = id.index;
            copy.image = view.ImageHandle();
            copy.aspect = aspect;
            copy.mip = static_cast<u32>(view.range.base.level);
            if (is_3d || slice) {
                // 3D images: layer 0; a slice view reads one depth slice.
                copy.layer = 0;
                copy.offset = VkOffset3D{0, 0, slice ? view.range.base.layer : 0};
                copy.extent = VkExtent3D{width, height, slice ? 1U : view.size.depth};
            } else {
                copy.layer = static_cast<u32>(view.range.base.layer) + layer;
                copy.extent = VkExtent3D{width, height, 1};
            }
            copy.host_format = host;
            copy.format = view.format;
            AddCopy(state, regions, std::move(copy), skip);
        }
    }
}

/// Scheduler::EndRenderPass, after the pass's end barriers: copy the attachments of a pass that
/// ran guest draws.
inline void PassEnded(Scheduler& scheduler, VkFramebuffer framebuffer,
                      std::span<const VkImage> images,
                      std::span<const VkImageSubresourceRange> ranges, u32 reason) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!Capturing()) {
        state.current = {};
        return;
    }
    const u64 pass = state.pass++;
    const PassInfo info = std::exchange(state.current, PassInfo{});
    const char* status = "ok";
    if (!info.noted) {
        status = "no_draws";
        ++state.empty_passes;
    } else if (info.framebuffer != framebuffer || info.formats.size() != images.size()) {
        status = "mismatch";
        ++state.mismatched_passes;
    }
    std::string formats;
    for (const PixelFormat format : info.formats) {
        formats += fmt::format("{}{}", formats.empty() ? "" : ",", static_cast<u32>(format));
    }
    std::string handles;
    for (const VkImage image : images) {
        handles += fmt::format("{}{:x}", handles.empty() ? "" : ",", Bits(image));
    }
    state.passes_tsv += fmt::format(
        "{}\t{}\t{}\t{}\t{:x}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", state.frame, pass, status,
        reason, Bits(framebuffer), info.area.width, info.area.height,
        static_cast<u32>(info.samples), info.draws, info.focus_draws,
        info.other_framebuffer_draws, info.after ? 1 : 0, formats.empty() ? "-" : formats,
        handles.empty() ? "-" : handles);
    if (std::strcmp(status, "ok") != 0) {
        return;
    }
    const Tag tag = info.after ? Tag::After : Tag::Pass;
    const u32 group = info.after ? state.focus_seq : 0;
    std::vector<Region> regions;
    for (size_t i = 0; i < images.size() && i < ranges.size(); ++i) {
        AddAttachment(state, regions, tag, static_cast<u32>(i), pass, group, images[i], ranges[i],
                      info.formats[i], info.area, info.samples);
    }
    RecordRegions(scheduler, std::move(regions));
}

/// GraphicsPipeline::ConfigureImpl: the image views of a focus draw.
inline void NoteViews(u64 fragment_hash, std::span<const VideoCommon::ImageViewInOut> views) {
    if (!IsFocus(fragment_hash)) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.focus_views_fs = fragment_hash;
    state.focus_views.clear();
    for (const VideoCommon::ImageViewInOut& view : views) {
        state.focus_views.push_back(view.id);
    }
}

/// RasterizerVulkan::PrepareDraw, after Configure and before the draw is recorded. Returns true
/// when the focus draw got a pass of its own (FocusAfter must follow the draw).
inline bool FocusBefore(Scheduler& scheduler, TextureCache& texture_cache,
                        const Framebuffer* framebuffer, u64 fragment_hash,
                        const Tegra::Engines::Maxwell3D& maxwell3d,
                        const Tegra::MemoryManager& gpu_memory) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!Capturing()) {
        return false;
    }
    if (state.focus_this_frame >= GetConfig().focus_max) {
        ++state.focus_skipped;
        return false;
    }
    ++state.focus_this_frame;
    const u32 group = ++state.focus_seq;
    // Ends the pass holding the earlier draws (PassEnded copies it).
    scheduler.RequestOutsideRenderPassOperationContext();
    const u64 pass = state.pass;
    std::vector<Region> regions;
    const std::vector<PixelFormat> formats = AttachmentFormats(framebuffer);
    const u32 num_images = framebuffer->NumImages();
    if (formats.size() == num_images) {
        for (u32 i = 0; i < num_images; ++i) {
            AddAttachment(state, regions, Tag::Before, i, pass, group, framebuffer->Images()[i],
                          framebuffer->ImageRanges()[i], formats[i], framebuffer->RenderArea(),
                          framebuffer->Samples());
        }
    } else {
        ++state.mismatched_passes;
    }
    if (state.focus_views_fs == fragment_hash) {
        for (size_t slot = 0; slot < state.focus_views.size(); ++slot) {
            const VideoCommon::ImageViewId id = state.focus_views[slot];
            if (!id) {
                continue;
            }
            const ImageView& view = texture_cache.GetImageView(id);
            if (view.IsBuffer() || !view.image_id) {
                continue;
            }
            AddView(state, regions, static_cast<u32>(slot), pass, group, id, view);
        }
    }
    RecordRegions(scheduler, std::move(regions));
    for (u32 stage = 0; stage < Tegra::Engines::Maxwell3D::Regs::MaxShaderStage; ++stage) {
        const auto& buffers = maxwell3d.state.shader_stages[stage].const_buffers;
        for (u32 index = 0; index < buffers.size(); ++index) {
            const auto& buffer = buffers[index];
            if (!buffer.enabled || buffer.size == 0 || buffer.address == 0) {
                continue;
            }
            CbufDump dump{};
            dump.id = state.cbufs.size();
            dump.frame = state.frame;
            dump.pass = pass;
            dump.group = group;
            dump.fs = fragment_hash;
            dump.stage = stage;
            dump.index = index;
            dump.address = buffer.address;
            dump.size = buffer.size;
            dump.data.resize((std::min)(buffer.size, MAX_CBUF_BYTES));
            dump.bytes = static_cast<u32>(dump.data.size());
            gpu_memory.ReadBlockUnsafe(buffer.address, dump.data.data(), dump.data.size());
            state.cbufs.push_back(std::move(dump));
        }
    }
    scheduler.RequestRenderpass(framebuffer);
    state.focus_split_pending = true;
    return true;
}

/// RasterizerVulkan::PrepareDraw, after every recorded draw.
inline void NoteDraw(const Framebuffer* framebuffer, const std::array<u64, 6>& hashes,
                     bool focus) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!Capturing()) {
        return;
    }
    PassInfo& info = state.current;
    if (!info.noted) {
        info.noted = true;
        info.framebuffer = framebuffer->Handle();
        info.area = framebuffer->RenderArea();
        info.samples = framebuffer->Samples();
        info.formats = AttachmentFormats(framebuffer);
    } else if (info.framebuffer != framebuffer->Handle()) {
        ++info.other_framebuffer_draws;
    }
    const u64 vs = hashes[1] != 0 ? hashes[1] : hashes[0];
    state.draws_tsv +=
        fmt::format("{}\t{}\t{}\t{:016x}\t{:016x}\t{:x}\t{}\t{}\t{}\n", state.frame, state.pass,
                    info.draws, vs, hashes[5], Bits(framebuffer->Handle()),
                    framebuffer->RenderArea().width, framebuffer->RenderArea().height,
                    focus ? state.focus_seq : 0);
    ++info.draws;
    ++state.draws;
    if (focus) {
        ++info.focus_draws;
    }
}

/// RasterizerVulkan::PrepareDraw, after a focus draw that FocusBefore split off.
inline void FocusAfter(Scheduler& scheduler) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.focus_split_pending) {
        return;
    }
    state.focus_split_pending = false;
    state.current.after = state.current.noted && state.current.draws == 1;
    scheduler.RequestOutsideRenderPassOperationContext();
}

inline bool KeyPressed(State& state) {
#ifdef _WIN32
    const bool down = (GetAsyncKeyState(CAPTURE_KEY) & 0x8000) != 0;
#elif defined(SNAPRYU_PASS_CAPTURE_MOCK_KEY)
    const bool down = SNAPRYU_PASS_CAPTURE_MOCK_KEY();
#else
    const bool down = false;
#endif
    const bool pressed = down && !state.key_was_down;
    state.key_was_down = down;
    return pressed;
}

inline bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(file);
}

/// Writes the copies and constant buffers recorded since the last write and frees their host
/// memory. The GPU must have finished every recorded copy (Scheduler::Finish).
inline void WriteHeld(State& state) {
    std::error_code error;
    std::filesystem::create_directories(state.dir, error);
    for (size_t i = state.first_unwritten; i < state.copies.size(); ++i) {
        Copy& copy = state.copies[i];
        if (!copy.skip.empty()) {
            continue;
        }
        if (!copy.buffer.IsHostCoherent()) {
            copy.buffer.Invalidate();
        }
        const std::span<const u8> mapped = std::as_const(copy.buffer).Mapped();
        if (mapped.size() < copy.bytes) {
            copy.skip = "write";
            ++state.write_failed;
        } else {
            const std::span<const u8> bytes = mapped.first(static_cast<size_t>(copy.bytes));
            std::tie(copy.hash_a, copy.hash_b) = HashBytes(bytes);
            std::vector<size_t>& same = state.by_hash[copy.hash_a];
            for (const size_t earlier : same) {
                const Copy& other = state.copies[earlier];
                if (other.bytes == copy.bytes && other.hash_b == copy.hash_b) {
                    copy.file = other.file;
                    break;
                }
            }
            if (!copy.file.empty()) {
                ++state.duplicates;
                state.duplicate_bytes += copy.bytes;
            } else if (const std::string file = fmt::format("img_{}.raw", copy.id);
                       WriteFile(state.dir / file, bytes.data(), bytes.size())) {
                copy.file = file;
                same.push_back(i);
                ++state.written;
                state.written_bytes += copy.bytes;
            } else {
                copy.skip = "write";
                ++state.write_failed;
            }
        }
        copy.buffer = vk::Buffer{};
    }
    state.first_unwritten = state.copies.size();
    state.held = 0;
    for (size_t i = state.first_unwritten_cbuf; i < state.cbufs.size(); ++i) {
        CbufDump& dump = state.cbufs[i];
        const std::string file = fmt::format("cbuf_{}.bin", dump.id);
        if (WriteFile(state.dir / file, dump.data.data(), dump.data.size())) {
            dump.file = file;
        }
        dump.data = {};
    }
    state.first_unwritten_cbuf = state.cbufs.size();
}

inline long long Ms(std::chrono::steady_clock::time_point a,
                    std::chrono::steady_clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
}

/// RasterizerVulkan::PrepareDraw, before the draw is configured: true once the held copies reach
/// EDEN_PASS_CAPTURE_SPILL_MB.
inline bool SpillDue() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    return Capturing() && state.held >= GetConfig().spill;
}

/// Ends the open render pass (PassEnded records its copies), waits for the GPU and writes the held
/// copies. Runs before the next draw is configured, so that draw starts from a fresh command
/// buffer.
inline void Spill(Scheduler& scheduler) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!Capturing()) {
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    const u64 held = state.held;
    const auto wait_begin = std::chrono::steady_clock::now();
    scheduler.Finish();
    const auto write_begin = std::chrono::steady_clock::now();
    WriteHeld(state);
    const auto write_end = std::chrono::steady_clock::now();
    ++state.spills;
    state.spills_tsv += fmt::format("{}\t{}\t{}\t{}\t{}\t{}\n", state.spills, state.frame,
                                    state.pass, held, Ms(wait_begin, write_begin),
                                    Ms(write_begin, write_end));
}

inline void Flush(State& state, Scheduler& scheduler) {
    const Config& config = GetConfig();
    const auto wait_begin = std::chrono::steady_clock::now();
    // Every copy was recorded before this point; PassEnded on the submit path sees no capture.
    scheduler.Finish();
    const auto write_begin = std::chrono::steady_clock::now();
    WriteHeld(state);
    std::string index =
        "id\tframe\tpass\tgroup\ttag\tslot\tview\timage\taspect\tmip\tlayer\tz\twidth\theight\t"
        "depth\tvk_format\tpixel_format\ttexel_bytes\tbytes\tfile\tskip\tblock_width\t"
        "block_height\thash\n";
    u64 skipped = 0;
    for (const Copy& copy : state.copies) {
        if (!copy.skip.empty()) {
            ++skipped;
        }
        index += fmt::format(
            "{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:x}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}"
            "\t{}\t{:016x}\n",
            copy.id, copy.frame, copy.pass, copy.group, TagName(copy.tag), copy.slot, copy.view,
            Bits(copy.image), static_cast<u32>(copy.aspect), copy.mip, copy.layer, copy.offset.z,
            copy.extent.width, copy.extent.height, copy.extent.depth,
            static_cast<u32>(copy.host_format), static_cast<u32>(copy.format), copy.texel_bytes,
            copy.bytes, copy.file.empty() ? "-" : copy.file, copy.skip.empty() ? "-" : copy.skip,
            copy.block_width, copy.block_height, copy.hash_a);
    }
    std::string cbufs = "id\tframe\tpass\tgroup\tfs\tstage\tindex\taddress\tsize\tbytes\tfile\n";
    for (const CbufDump& dump : state.cbufs) {
        cbufs += fmt::format("{}\t{}\t{}\t{}\t{:016x}\t{}\t{}\t{:#x}\t{}\t{}\t{}\n", dump.id,
                             dump.frame, dump.pass, dump.group, dump.fs, dump.stage, dump.index,
                             dump.address, dump.size, dump.bytes,
                             dump.file.empty() ? "-" : dump.file);
    }
    const auto write_end = std::chrono::steady_clock::now();
    const std::string summary = fmt::format(
        "PASS_CAPTURE b61\ncapture={}\nframes={}\nkey=F3\nfocus={}\nfocus_max={}\nbudget_mib={}\n"
        "spill_mib={}\npasses={}\ndraws={}\nempty_passes={}\nmismatched_passes={}\n"
        "focus_draws={}\nfocus_skipped={}\ncopies={}\nwritten={}\nwritten_bytes={}\n"
        "duplicates={}\nduplicate_bytes={}\nskipped={}\nbudget_skips={}\nwrite_failed={}\n"
        "cbufs={}\nspills={}\nheld_max_bytes={}\ncapture_ms={}\nwait_ms={}\nwrite_ms={}\n"
        "resolution_up_scale={}\nresolution_down_shift={}\n",
        state.captures, config.frames, config.focus_text, config.focus_max, config.budget >> 20,
        config.spill >> 20, state.pass, state.draws, state.empty_passes, state.mismatched_passes,
        state.focus_seq, state.focus_skipped, state.copies.size(), state.written,
        state.written_bytes, state.duplicates, state.duplicate_bytes, skipped, state.budget_skips,
        state.write_failed, state.cbufs.size(), state.spills, state.held_max,
        Ms(state.started, wait_begin), Ms(wait_begin, write_begin), Ms(write_begin, write_end),
        Settings::values.resolution_info.up_scale, Settings::values.resolution_info.down_shift);
    const std::string spills = "spill\tframe\tpass\theld_bytes\twait_ms\twrite_ms\n" +
                               state.spills_tsv;
    const bool ok = WriteFile(state.dir / "index.tsv", index.data(), index.size()) &&
                    WriteFile(state.dir / "passes.tsv", state.passes_tsv.data(),
                              state.passes_tsv.size()) &&
                    WriteFile(state.dir / "draws.tsv", state.draws_tsv.data(),
                              state.draws_tsv.size()) &&
                    WriteFile(state.dir / "cbuf.tsv", cbufs.data(), cbufs.size()) &&
                    WriteFile(state.dir / "spills.tsv", spills.data(), spills.size()) &&
                    WriteFile(state.dir / "summary.txt", summary.data(), summary.size());
    LOG_WARNING(Render_Vulkan,
                "PASS_CAPTURE done n={} dir={} ok={} frames={} passes={} draws={} empty_passes={} "
                "mismatched_passes={} focus_draws={} focus_skipped={} copies={} written={} "
                "written_mib={} duplicates={} skipped={} budget_skips={} write_failed={} cbufs={} "
                "spills={} held_max_mib={} capture_ms={} wait_ms={} write_ms={}",
                state.captures, Common::FS::PathToUTF8String(state.dir), ok, config.frames,
                state.pass, state.draws, state.empty_passes, state.mismatched_passes,
                state.focus_seq, state.focus_skipped, state.copies.size(), state.written,
                state.written_bytes >> 20, state.duplicates, skipped, state.budget_skips,
                state.write_failed, state.cbufs.size(), state.spills, state.held_max >> 20,
                Ms(state.started, wait_begin), Ms(wait_begin, write_begin),
                Ms(write_begin, write_end));
    state.copies.clear();
    state.cbufs.clear();
    state.by_hash.clear();
    state.passes_tsv.clear();
    state.draws_tsv.clear();
    state.spills_tsv.clear();
    state.focus_views.clear();
}

inline void Start(State& state) {
    const Config& config = GetConfig();
    ++state.captures;
    state.frame = 0;
    state.pass = 0;
    state.next_id = 0;
    state.used = 0;
    state.held = 0;
    state.held_max = 0;
    state.spills = 0;
    state.first_unwritten = 0;
    state.first_unwritten_cbuf = 0;
    state.written = 0;
    state.written_bytes = 0;
    state.duplicates = 0;
    state.duplicate_bytes = 0;
    state.write_failed = 0;
    state.by_hash.clear();
    state.spills_tsv.clear();
    state.budget_skips = 0;
    state.draws = 0;
    state.empty_passes = 0;
    state.mismatched_passes = 0;
    state.focus_this_frame = 0;
    state.focus_seq = 0;
    state.focus_skipped = 0;
    state.focus_split_pending = false;
    state.focus_views_fs = 0;
    state.focus_views.clear();
    state.current = {};
    state.copies.clear();
    state.cbufs.clear();
    state.passes_tsv = "frame\tpass\tstatus\treason\tframebuffer\twidth\theight\tsamples\tdraws\t"
                       "focus_draws\tother_framebuffer_draws\tafter\tpixel_formats\timages\n";
    state.draws_tsv = "frame\tpass\tdraw\tvs\tfs\tframebuffer\twidth\theight\tfocus_group\n";
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    state.dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                fmt::format("pass_capture_{}_{}", seconds, state.captures);
    state.started = std::chrono::steady_clock::now();
    capturing.store(true, std::memory_order_relaxed);
    LOG_WARNING(Render_Vulkan,
                "PASS_CAPTURE start n={} frames={} focus={} focus_max={} budget_mib={} spill_mib={} "
                "dir={}",
                state.captures, config.frames, config.focus_text, config.focus_max,
                config.budget >> 20, config.spill >> 20, Common::FS::PathToUTF8String(state.dir));
}

/// RasterizerVulkan::TickFrame: F3 arms a capture of the next frames; the last one flushes it.
inline void Frame(Scheduler& scheduler, MemoryAllocator& allocator, const Device& device) {
    const Config& config = GetConfig();
    if (!config.enabled) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.allocator = &allocator;
    state.device = &device;
    if (!state.announced) {
        state.announced = true;
        LOG_WARNING(Render_Vulkan,
                    "PASS_CAPTURE ready b61 key=F3 frames={} focus={} focus_max={} budget_mib={} "
                    "spill_mib={} max_captures={} (EDEN_PASS_CAPTURE)",
                    config.frames, config.focus_text, config.focus_max, config.budget >> 20,
                    config.spill >> 20, config.max_captures);
    }
    if (Capturing()) {
        state.focus_this_frame = 0;
        if (++state.frame < config.frames) {
            return;
        }
        capturing.store(false, std::memory_order_relaxed);
        Flush(state, scheduler);
        return;
    }
    if (!KeyPressed(state)) {
        return;
    }
    if (state.captures >= config.max_captures) {
        if (!state.limit_logged) {
            state.limit_logged = true;
            LOG_WARNING(Render_Vulkan, "PASS_CAPTURE limit reached ({} captures); F3 ignored",
                        config.max_captures);
        }
        return;
    }
    Start(state);
}

/// ~RasterizerVulkan, after the scheduler finished: the copy buffers go before the device.
inline void Shutdown() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    capturing.store(false, std::memory_order_relaxed);
    state.copies.clear();
    state.cbufs.clear();
    state.by_hash.clear();
}

} // namespace Vulkan::PassCapture
#endif
