#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace CemuPad::Media {
// Per-readback-slot image. The owner must finish GPU use before resizing/reset.
// Used by the renderer and by the native Adreno conversion/readback probe.
struct VulkanRgbaConversion {
    VkDevice device{};VkImage image{};VkDeviceMemory memory{};
    std::uint32_t width=0,height=0;bool readable=false;
    VkPhysicalDevice checkedPhysical{};VkFormat checkedFormat=VK_FORMAT_UNDEFINED;bool supported=false;
    VulkanRgbaConversion()=default;
    VulkanRgbaConversion(const VulkanRgbaConversion&)=delete;
    VulkanRgbaConversion& operator=(const VulkanRgbaConversion&)=delete;
    ~VulkanRgbaConversion(){Reset();}
    void Reset(){
        if(image)vkDestroyImage(device,image,nullptr);
        if(memory)vkFreeMemory(device,memory,nullptr);
        image={};memory={};device={};width=height=0;readable=false;
    }
    bool Supports(VkPhysicalDevice physical,VkFormat source){
        if(checkedPhysical!=physical||checkedFormat!=source){
            VkFormatProperties from{},to{};
            vkGetPhysicalDeviceFormatProperties(physical,source,&from);
            vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&to);
            supported=(from.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)&&(to.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT);
            checkedPhysical=physical;checkedFormat=source;
        }
        return supported;
    }
    bool Ensure(VkPhysicalDevice physical,VkDevice logical,VkFormat source,std::uint32_t w,std::uint32_t h){
        if(!Supports(physical,source))return false;
        if(device==logical&&image&&width==w&&height==h)return true;
        Reset();device=logical;
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};create.imageType=VK_IMAGE_TYPE_2D;
        create.format=VK_FORMAT_R8G8B8A8_UNORM;create.extent={w,h,1};create.mipLevels=1;create.arrayLayers=1;
        create.samples=VK_SAMPLE_COUNT_1_BIT;create.tiling=VK_IMAGE_TILING_OPTIMAL;
        create.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(vkCreateImage(device,&create,nullptr,&image)!=VK_SUCCESS)return false;
        VkMemoryRequirements requirement{};vkGetImageMemoryRequirements(device,image,&requirement);
        VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        std::uint32_t type=properties.memoryTypeCount;
        for(std::uint32_t i=0;i<properties.memoryTypeCount;++i)if(requirement.memoryTypeBits&(1u<<i)){
            if(type==properties.memoryTypeCount)type=i;
            if(properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT){type=i;break;}
        }
        if(type==properties.memoryTypeCount){Reset();return false;}
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=requirement.size;allocate.memoryTypeIndex=type;
        if(vkAllocateMemory(device,&allocate,nullptr,&memory)!=VK_SUCCESS||vkBindImageMemory(device,image,memory,0)!=VK_SUCCESS){Reset();return false;}
        width=w;height=h;return true;
    }
    void Record(VkCommandBuffer command,VkImage source){
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask=readable?VK_ACCESS_TRANSFER_READ_BIT:0;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout=readable?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(command,readable?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;
        blit.srcOffsets[1]={static_cast<std::int32_t>(width),static_cast<std::int32_t>(height),1};blit.dstOffsets[1]=blit.srcOffsets[1];
        vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        readable=true;
    }
};
}
