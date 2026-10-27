// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace Vulkan::SnapGoTransfer {
constexpr uint32_t Width=1280,Height=720;
constexpr VkDeviceSize Bytes=VkDeviceSize{Width}*Height*4;
constexpr VkImageSubresourceLayers Layers{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
struct Commands {
    PFN_vkCmdPipelineBarrier barrier;
    PFN_vkCmdCopyImage copy;
    PFN_vkCmdBlitImage blit;
    PFN_vkCmdCopyImageToBuffer download;
};
struct Transfer {
    VkImage source{},encoded{},scaled{};
    VkBuffer destination{};
    uint32_t source_width{},source_height{};
    VkOffset3D crop0{},crop1{};
    bool scale{},initialized{};
};
inline VkImageMemoryBarrier Barrier(VkImage image,VkImageLayout old_layout,VkImageLayout layout,
                                    VkAccessFlags from,VkAccessFlags to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image=image;b.oldLayout=old_layout;b.newLayout=layout;b.srcAccessMask=from;b.dstAccessMask=to;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};return b;
}
// This exact recording routine is exercised by the standalone hardware probe.
// A same-size SRGB->UNORM image copy preserves encoded bytes before UNORM scaling.
inline void Record(const Commands& d,VkCommandBuffer cmd,const Transfer& t) {
    auto image_barrier=[&](VkPipelineStageFlags from,VkPipelineStageFlags to,const VkImageMemoryBarrier& b){
        d.barrier(cmd,from,to,0,0,nullptr,0,nullptr,1,&b);
    };
    const auto before=Barrier(t.source,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    image_barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,before);
    VkImage read=t.source;
    if(t.scale){
        const auto encoded_before=Barrier(t.encoded,t.initialized?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,t.initialized?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
        image_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,encoded_before);
        const VkImageCopy copy{Layers,{},Layers,{},{t.source_width,t.source_height,1}};
        d.copy(cmd,t.source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t.encoded,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        const std::array before_blit{
            Barrier(t.encoded,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT),
            Barrier(t.scaled,t.initialized?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    t.initialized?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT)};
        d.barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,before_blit.data());
        const VkImageBlit blit{Layers,{t.crop0,t.crop1},Layers,{{0,0,0},{Width,Height,1}}};
        d.blit(cmd,t.encoded,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t.scaled,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
        const auto ready=Barrier(t.scaled,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        image_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,ready);read=t.scaled;
    }
    const VkBufferImageCopy copy{0,0,0,Layers,{},{Width,Height,1}};
    d.download(cmd,read,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,t.destination,1,&copy);
    const auto after=Barrier(t.source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,
                              VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    host.buffer=t.destination;host.size=Bytes;
    d.barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,
              0,0,nullptr,1,&host,1,&after);
}
}
