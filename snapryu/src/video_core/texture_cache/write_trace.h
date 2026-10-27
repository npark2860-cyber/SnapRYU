// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// WRITE_TRACE / SQUARES_WRITE: when did the GPU last make the content of each texture sampled
// while terrain squares are on screen (TotK NCE, 2026-10-17)?
//
// Observation (user, 2026-10-17): a square is a shadow decision and keeps one brightness while
// the terrain around it follows the time of day (darker at noon, brighter at sunset). So the
// texture behind it looks frozen: the GPU made its content at some earlier time and did not make
// it again since. b51 showed that every clean sampled texture matches guest memory; the 260x260
// R8G8 terrain tiles are GPU-made (GpuModified), which b51 could not compare.
//
// Question: is a frozen texture
//   (a) never rewritten: no GPU write reached that memory since (the game, or a skipped draw), or
//   (b) rewritten elsewhere: a newer GPU write went to another image over the same memory, and
//       the sampled image never received it (texture cache)?
//
// Each image records, per mip level and layer (up to 16 x 64), the frame of the GPU write that
// made its current content:
//   target   render target or clear (the view's levels and layers)
//   storage  shader image store (whole image)
//   dma      DMA buffer to image (the copied level and layer)
//   blit     2D engine blit destination (whole image)
//   join / alias   texture cache copies: the destination takes the source's content frame per
//                  copied subresource, so a copy does not make old content look new
// An upload from guest memory resets the content to "cpu".
//
// F7 (alias_trace.h) arms a report for the next frame. Every sampled image that has GPU content
// or a GPU write history is listed once with its content age per level, its GPU write count, and
// the image over the same memory whose content is newest, with its relation (alias: synchronized
// by SynchronizeAliases; bad_overlap or none: never copied). A sampled image older than such an
// overlap is case (b). Groups (format and size) show whether the other tiles are being remade.
//
// Diagnostic builds only; release builds compile none of this. Used by the GPU thread only.
#pragma once

#if SNAPRYU_DIAGNOSTICS
#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/texture_cache/alias_trace.h"
#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/tile_trace.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon::WriteTrace {

enum class Writer : u8 { Target, Storage, Dma, Blit, Join, Alias };

inline const char* WriterName(Writer writer) {
    switch (writer) {
    case Writer::Target:
        return "target";
    case Writer::Storage:
        return "storage";
    case Writer::Dma:
        return "dma";
    case Writer::Blit:
        return "blit";
    case Writer::Join:
        return "join";
    case Writer::Alias:
        return "alias";
    }
    return "?";
}

inline constexpr s32 MAX_LEVELS = 16;
inline constexpr s32 MAX_LAYERS = 64;
inline constexpr size_t MAX_INFOS = size_t{1} << 17;
inline constexpr size_t MAX_ROWS = 160;
inline constexpr size_t MAX_GROUPS = 48;
inline constexpr u64 NONE = ~u64{0};

struct Info {
    GPUVAddr gpu_addr{};
    u32 size{};
    s32 levels = 1;
    s32 layers = 1;
    u64 created = 0;  // frame the image was created (0: unknown)
    u64 upload = 0;   // last upload from guest memory (0: none)
    u64 writes = 0;   // GPU writes and copies since creation
    u32 kinds = 0;    // bit per Writer
    Writer last_kind = Writer::Target;
    std::vector<u64> content; // [layer * levels + level]: frame of the GPU write behind it, 0: cpu
};

inline ankerl::unordered_dense::map<u32, Info> infos;
inline u64 dropped = 0;
inline Writer current_writer = Writer::Target;
inline std::optional<SubresourceRange> current_range;
inline std::vector<float> frame_seconds; // [frame_tick]: start of that frame, seconds since boot

// Writer of the PrepareImage modifications made while the scope lives.
struct WriterScope {
    explicit WriterScope(Writer writer) : previous{current_writer} {
        current_writer = writer;
    }
    ~WriterScope() {
        current_writer = previous;
    }
    WriterScope(const WriterScope&) = delete;
    WriterScope& operator=(const WriterScope&) = delete;
    Writer previous;
};

// Subresources of the render target view (or DMA level) being prepared.
struct RangeScope {
    explicit RangeScope(const SubresourceRange& range) : previous{current_range} {
        current_range = range;
    }
    ~RangeScope() {
        current_range = previous;
    }
    RangeScope(const RangeScope&) = delete;
    RangeScope& operator=(const RangeScope&) = delete;
    std::optional<SubresourceRange> previous;
};

inline void Reset(Info& info, const ImageBase& image, u64 created) {
    info.gpu_addr = image.gpu_addr;
    info.size = image.guest_size_bytes;
    info.levels = std::clamp<s32>(static_cast<s32>(image.info.resources.levels), 1, MAX_LEVELS);
    info.layers = std::clamp<s32>(static_cast<s32>(image.info.resources.layers), 1, MAX_LAYERS);
    info.created = created;
    info.upload = 0;
    info.writes = 0;
    info.kinds = 0;
    info.last_kind = Writer::Target;
    info.content.assign(static_cast<size_t>(info.levels * info.layers), 0);
}

// Info of the image now in this slot. With create, a slot that holds another image is started
// again (creation unknown). Without create nothing is inserted, so other pointers stay valid.
inline Info* Find(u32 index, const ImageBase& image, bool create) {
    const auto it = infos.find(index);
    if (it != infos.end()) {
        Info& info = it->second;
        if (info.gpu_addr == image.gpu_addr && info.size == image.guest_size_bytes) {
            return &info;
        }
        if (!create) {
            return nullptr;
        }
        Reset(info, image, 0);
        return &info;
    }
    if (!create) {
        return nullptr;
    }
    if (infos.size() >= MAX_INFOS) {
        ++dropped;
        return nullptr;
    }
    Info& info = infos[index];
    Reset(info, image, 0);
    return &info;
}

inline void Created(u32 index, const ImageBase& image, u64 frame) {
    if (infos.size() >= MAX_INFOS && !infos.contains(index)) {
        ++dropped;
        return;
    }
    Reset(infos[index], image, frame);
}

inline void Forget(u32 index) {
    infos.erase(index);
}

inline void Uploaded(u32 index, const ImageBase& image, u64 frame) {
    if (Info* const info = Find(index, image, true)) {
        info->upload = frame;
        std::ranges::fill(info->content, u64{0});
    }
}

inline void Note(Info& info, Writer kind) {
    ++info.writes;
    info.kinds |= 1u << static_cast<u32>(kind);
    info.last_kind = kind;
}

inline void SetRange(Info& info, s32 base_level, s32 levels, s32 base_layer, s32 layers,
                     u64 value) {
    const s32 level_end = std::min(base_level + levels, info.levels);
    const s32 layer_end = std::min(base_layer + layers, info.layers);
    for (s32 layer = std::max(base_layer, 0); layer < layer_end; ++layer) {
        for (s32 level = std::max(base_level, 0); level < level_end; ++level) {
            info.content[static_cast<size_t>(layer * info.levels + level)] = value;
        }
    }
}

// A GPU write (render target, storage, DMA, blit) made content in this frame.
inline void Wrote(u32 index, const ImageBase& image, u64 frame, Writer kind) {
    TileTrace::Wrote(index, image, frame, static_cast<u8>(kind), current_range); // b63
    Info* const info = Find(index, image, true);
    if (!info) {
        return;
    }
    Note(*info, kind);
    if (current_range && (kind == Writer::Target || kind == Writer::Dma)) {
        const SubresourceRange& range = *current_range;
        SetRange(*info, range.base.level, range.extent.levels, range.base.layer,
                 range.extent.layers, frame);
    } else {
        std::ranges::fill(info->content, frame);
    }
}

inline void Wrote(u32 index, const ImageBase& image, u64 frame) {
    Wrote(index, image, frame, current_writer);
}

inline u64 Newest(const Info& info) {
    return info.content.empty() ? 0 : *std::ranges::max_element(info.content);
}

inline u64 LevelNewest(const Info& info, s32 level, s32 base_layer, s32 layers) {
    u64 newest = 0;
    const s32 layer_end = std::min(base_layer + layers, info.layers);
    for (s32 layer = std::max(base_layer, 0); layer < layer_end; ++layer) {
        newest = std::max(newest, info.content[static_cast<size_t>(layer * info.levels + level)]);
    }
    return newest;
}

// A texture cache copy from src onto dst: each copied dst subresource takes the content frame
// of its src subresource (0 when src content came from guest memory or src is untracked).
template <typename Copies>
inline void Copied(u32 dst_index, const ImageBase& dst, u32 src_index, const ImageBase& src,
                   const Copies& copies, Writer kind) {
    TileTrace::Copied(dst_index, dst, src_index, src, copies.size(), static_cast<u8>(kind)); // b63
    Info* const dst_info = Find(dst_index, dst, true);
    if (!dst_info) {
        return;
    }
    const Info* const src_info = Find(src_index, src, false);
    Note(*dst_info, kind);
    for (const ImageCopy& copy : copies) {
        u64 value = 0;
        if (src_info) {
            const s32 level = copy.src_subresource.base_level;
            value = level >= 0 && level < src_info->levels
                        ? LevelNewest(*src_info, level, copy.src_subresource.base_layer,
                                      copy.src_subresource.num_layers)
                        : Newest(*src_info);
        }
        SetRange(*dst_info, copy.dst_subresource.base_level, 1, copy.dst_subresource.base_layer,
                 copy.dst_subresource.num_layers, value);
    }
}

// ---- SQUARES_WRITE report (collects while alias_trace.h collects) ----

struct Overlap {
    u32 count = 0;
    u64 newest = 0;
    const char* relation = "none"; // of the newest: alias (aliased_images), bad_overlap, none
    std::string line;
};

struct Row {
    u32 format = 0;
    u32 width = 0;
    u32 height = 0;
    s32 levels = 0;
    s32 layers = 0;
    u64 newest_age = NONE; // frames since the newest content in the image (NONE: cpu or untracked)
    bool newer_overlap = false;
    std::string line;
};

inline ankerl::unordered_dense::set<u32> sampled;
inline std::vector<Row> rows;
inline u32 untracked = 0;

inline float Seconds(u64 frame) {
    return frame < frame_seconds.size() ? frame_seconds[frame] : 0.0f;
}

inline std::string AgeText(u64 frame, u64 content) {
    if (content == 0 || content > frame) {
        return "cpu";
    }
    return fmt::format("{:.1f}", Seconds(frame) - Seconds(content));
}

// True once per image and report for a sampled image with GPU content or a GPU write history.
inline bool Wants(u32 index, const ImageBase& image) {
    if (!AliasTrace::collecting) {
        return false;
    }
    const Info* const info = Find(index, image, false);
    const bool gpu = True(image.flags & ImageFlagBits::GpuModified) || (info && info->writes > 0);
    return gpu && sampled.insert(index).second;
}

// Called for each other image over the sampled image's memory.
inline void Consider(Overlap& overlap, u32 other_index, const ImageBase& other,
                     const ImageBase& image, u64 frame) {
    ++overlap.count;
    const Info* const info = Find(other_index, other, false);
    if (!info) {
        return;
    }
    const u64 newest = Newest(*info);
    if (newest <= overlap.newest) {
        return;
    }
    overlap.newest = newest;
    const bool aliased = std::ranges::any_of(
        image.aliased_images,
        [other_index](const AliasedImage& alias) { return alias.id.index == other_index; });
    const bool bad_overlap = std::ranges::any_of(
        image.overlapping_images, [other_index](ImageId id) { return id.index == other_index; });
    overlap.relation = aliased ? "alias" : bad_overlap ? "bad_overlap" : "none";
    overlap.line = fmt::format("{} writes={} last={} content_age_s={}",
                               AliasTrace::Describe(other_index, other), info->writes,
                               WriterName(info->last_kind), AgeText(frame, newest));
}

inline void Sample(u32 index, const ImageBase& image, const Overlap& overlap, u64 frame) {
    Row row;
    row.format = static_cast<u32>(image.info.format);
    row.width = image.info.size.width;
    row.height = image.info.size.height;
    row.levels = static_cast<s32>(image.info.resources.levels);
    row.layers = static_cast<s32>(image.info.resources.layers);
    const Info* const info = Find(index, image, false);
    std::string ages = "untracked";
    std::string oldest_layer = "-";
    u64 own_newest = 0;
    if (info) {
        ages.clear();
        for (s32 level = 0; level < info->levels; ++level) {
            ages += (level ? "," : "") + AgeText(frame, LevelNewest(*info, level, 0, info->layers));
        }
        if (info->layers > 1) {
            u64 oldest = NONE;
            for (s32 layer = 0; layer < info->layers; ++layer) {
                oldest = std::min(oldest, info->content[static_cast<size_t>(layer * info->levels)]);
            }
            oldest_layer = AgeText(frame, oldest);
        }
        own_newest = Newest(*info);
        if (own_newest != 0 && own_newest <= frame) {
            row.newest_age = frame - own_newest;
        }
    } else {
        ++untracked;
    }
    row.newer_overlap = overlap.newest > own_newest;
    row.line = fmt::format(
        "{} writes={} last={} kinds={:#x} level_age_s=[{}] l0_oldest_layer_s={} created_s={} "
        "upload_s={} overlaps={} newer_overlap={}",
        AliasTrace::Describe(index, image), info ? info->writes : 0,
        info ? WriterName(info->last_kind) : "-", info ? info->kinds : 0, ages, oldest_layer,
        info && info->created ? AgeText(frame, info->created) : "-",
        info && info->upload ? AgeText(frame, info->upload) : "-", overlap.count,
        row.newer_overlap);
    if (overlap.newest != 0) {
        row.line += fmt::format(" overlap_relation={} | overlap {}", overlap.relation, overlap.line);
    }
    rows.push_back(std::move(row));
}

inline void FinishReport(u32 number, u64 frame) {
    using Key = std::tuple<u32, u32, u32, s32, s32>;
    struct Group {
        Key key;
        std::vector<u64> ages;
        u32 count = 0;
        u32 cpu = 0;
        u32 newer_overlap = 0;
    };
    std::vector<Group> groups;
    u32 newer_overlap = 0;
    for (const Row& row : rows) {
        const Key key{row.format, row.width, row.height, row.levels, row.layers};
        auto it = std::ranges::find_if(groups, [&key](const Group& g) { return g.key == key; });
        if (it == groups.end()) {
            groups.push_back(Group{key, {}, 0, 0, 0});
            it = groups.end() - 1;
        }
        ++it->count;
        if (row.newest_age == NONE) {
            ++it->cpu;
        } else {
            it->ages.push_back(row.newest_age);
        }
        if (row.newer_overlap) {
            ++it->newer_overlap;
            ++newer_overlap;
        }
    }
    std::ranges::sort(groups, [](const Group& a, const Group& b) { return a.count > b.count; });
    LOG_INFO(Render_Vulkan,
             "SQUARES_WRITE n={} frame={} t={:.1f} sampled_gpu={} groups={} newer_overlap={} "
             "untracked={} tracked_images={} dropped={}",
             number, frame, Seconds(frame), rows.size(), groups.size(), newer_overlap, untracked,
             infos.size(), dropped);
    size_t shown_groups = 0;
    for (Group& g : groups) {
        if (++shown_groups > MAX_GROUPS) {
            break;
        }
        std::ranges::sort(g.ages);
        const auto at = [&g](size_t i) { return g.ages.empty() ? u64{0} : g.ages[i]; };
        const u64 median = at(g.ages.size() / 2);
        const u64 limit = std::max<u64>(median * 4, 300);
        const auto old = std::ranges::count_if(g.ages, [limit](u64 age) { return age > limit; });
        LOG_INFO(Render_Vulkan,
                 "SQUARES_WRITE group format={} {}x{} levels={} layers={} count={} cpu={} "
                 "newer_overlap={} age_frames_min={} med={} max={} old={} n={}",
                 std::get<0>(g.key), std::get<1>(g.key), std::get<2>(g.key), std::get<3>(g.key),
                 std::get<4>(g.key), g.count, g.cpu, g.newer_overlap, at(0), median,
                 at(g.ages.empty() ? 0 : g.ages.size() - 1), old, number);
    }
    // Newer overlaps first, then the oldest content (cpu content of a GPU-written image counts
    // as oldest: its GPU content was replaced by an upload).
    std::ranges::stable_sort(rows, [](const Row& a, const Row& b) {
        if (a.newer_overlap != b.newer_overlap) {
            return a.newer_overlap;
        }
        return a.newest_age > b.newest_age;
    });
    size_t shown = 0;
    for (const Row& row : rows) {
        if (shown == MAX_ROWS) {
            break;
        }
        ++shown;
        LOG_INFO(Render_Vulkan, "SQUARES_WRITE row {} n={}", row.line, number);
    }
    LOG_INFO(Render_Vulkan, "SQUARES_WRITE end n={} rows_listed={} rows_not_listed={}", number,
             shown, rows.size() - shown);
}

// Called once per frame by the texture cache after it advances frame_tick and before
// AliasTrace::EndFrame, so a collecting report is still open here.
inline void EndFrame(u64 frame_tick) {
    static const auto boot = std::chrono::steady_clock::now();
    if (AliasTrace::collecting) {
        FinishReport(AliasTrace::report.number, frame_tick - 1);
        rows.clear();
        sampled.clear();
        untracked = 0;
    }
    if (frame_seconds.size() <= frame_tick) {
        frame_seconds.resize(static_cast<size_t>(frame_tick) + 1, 0.0f);
    }
    frame_seconds[frame_tick] = std::chrono::duration<float>(std::chrono::steady_clock::now() - boot).count();
}

} // namespace VideoCommon::WriteTrace
#endif
