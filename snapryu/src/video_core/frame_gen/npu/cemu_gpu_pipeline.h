// SPDX-License-Identifier: MPL-2.0
// Descriptor/dispatch contract adapted from the preserved local Cemu gpu_resources.h.
// Executes the original profile-specific SPIR-V. No queue submission or waiting here.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace SnapRyuCemu {
struct Profile {
    uint32_t width{}, height{}, grid_width{}, grid_height{};
    size_t pixels{}, input_bytes{}, residual_bytes{}, flow_bytes{};
    const wchar_t* label{};
    static Profile FromHeight(uint32_t h) {
        if (h != 720 && h != 1080)
            throw std::invalid_argument("Cemu processing height must be720 or1080");
        uint32_t w = h == 720 ? 1280 : 1920, gw = (w + 15) / 16, gh = (h + 15) / 16;
        size_t p = size_t(w) * h;
        return {w, h, gw, gh, p, p * 6, p * 3, size_t(gw) * gh * 16, h == 720 ? L"720p" : L"1080p"};
    }
};
inline void Check(VkResult r, const char* where) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(where) + " Vulkan=" + std::to_string(r));
}
#define SNAPRYU_CEMU_GPU_FUNCTIONS(X)                                                              \
    X(CreateBuffer)                                                                                \
    X(DestroyBuffer)                                                                               \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory)             \
        X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges)     \
            X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory)        \
                X(CreateImageView) X(DestroyImageView) X(CreateDescriptorSetLayout)                \
                    X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
                        X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout)  \
                            X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule)  \
                                X(CreateComputePipelines) X(DestroyPipeline) X(CmdPipelineBarrier) \
                                    X(CmdBindPipeline) X(CmdBindDescriptorSets)                    \
                                        X(CmdPushConstants) X(CmdDispatch)
struct Api {
#define DECL(n) PFN_vk##n vk##n{};
    SNAPRYU_CEMU_GPU_FUNCTIONS(DECL)
#undef DECL
    template <class Dispatch>
    explicit Api(const Dispatch& d) {
#define COPY(n) vk##n = d.vk##n;
        SNAPRYU_CEMU_GPU_FUNCTIONS(COPY)
#undef COPY
    }
};
class Pipeline {
public:
    struct Image {
        VkImage image{};
        VkImageView view{};
        VkDeviceMemory memory{};
    };
    struct Buffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        uint8_t* mapped{};
        size_t bytes{};
    };
    enum ImageIndex { A, B, Composed, QuarterA, QuarterB, WarpedA, WarpedB };
    enum BufferIndex { Packed, Residual, Forward, Backward };
    const Profile profile;
    std::array<Image, 7> images{};
    std::array<Buffer, 4> buffers{};
    Pipeline(Api api, VkDevice device, VkPhysicalDeviceMemoryProperties memory, Profile p,
             const std::filesystem::path& directory)
        : profile(p), api{api}, device{device}, memory{memory} {
        try {
            for (unsigned i = 0; i < images.size(); ++i)
                CreateImage(images[i],
                            i == QuarterA || i == QuarterB ? profile.width / 4 : profile.width,
                            i == QuarterA || i == QuarterB ? profile.height / 4 : profile.height);
            CreateBuffer(buffers[Packed], profile.input_bytes);
            CreateBuffer(buffers[Residual], profile.residual_bytes);
            CreateBuffer(buffers[Forward], profile.flow_bytes);
            CreateBuffer(buffers[Backward], profile.flow_bytes);
            std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
            for (uint32_t i = 0; i < bindings.size(); ++i)
                bindings[i] = {i,
                               (i == 2 || i == 3 || i == 5 || i == 6)
                                   ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                               1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            si.bindingCount = uint32_t(bindings.size());
            si.pBindings = bindings.data();
            Check(api.vkCreateDescriptorSetLayout(device, &si, nullptr, &set_layout),
                  "Cemu descriptor layout");
            std::array<VkDescriptorPoolSize, 2> sizes{
                {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 1;
            pi.poolSizeCount = 2;
            pi.pPoolSizes = sizes.data();
            Check(api.vkCreateDescriptorPool(device, &pi, nullptr, &pool), "Cemu descriptor pool");
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &set_layout;
            Check(api.vkAllocateDescriptorSets(device, &ai, &set), "Cemu descriptor set");
            const std::array<unsigned, 7> image_bindings{0, 1, 4, 7, 8, 9, 10};
            const std::array<unsigned, 4> buffer_bindings{2, 3, 5, 6};
            std::array<VkDescriptorImageInfo, 11> image_info{};
            std::array<VkDescriptorBufferInfo, 11> buffer_info{};
            for (unsigned i = 0; i < images.size(); ++i)
                image_info[image_bindings[i]] = {VK_NULL_HANDLE, images[i].view,
                                                 VK_IMAGE_LAYOUT_GENERAL};
            for (unsigned i = 0; i < buffers.size(); ++i)
                buffer_info[buffer_bindings[i]] = {buffers[i].buffer, 0, buffers[i].bytes};
            std::array<VkWriteDescriptorSet, 11> writes{};
            for (unsigned i = 0; i < writes.size(); ++i) {
                auto& w = writes[i];
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = set;
                w.dstBinding = i;
                w.descriptorCount = 1;
                w.descriptorType = bindings[i].descriptorType;
                if (w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                    w.pBufferInfo = &buffer_info[i];
                else
                    w.pImageInfo = &image_info[i];
            }
            api.vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
            VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            li.setLayoutCount = 1;
            li.pSetLayouts = &set_layout;
            li.pushConstantRangeCount = 1;
            li.pPushConstantRanges = &push;
            Check(api.vkCreatePipelineLayout(device, &li, nullptr, &layout),
                  "Cemu pipeline layout");
            const std::array<const wchar_t*, 4> files{
                L"motion1080-downsample.spv", L"motion1080-flow.spv", L"motion1080-pack.spv",
                L"motion1080-compose.spv"};
            for (unsigned i = 0; i < files.size(); ++i) {
                const auto file = directory / files[i];
                auto bytes = std::filesystem::file_size(file);
                if (!bytes || bytes % 4 || bytes > 1024 * 1024)
                    throw std::runtime_error("Cemu SPIR-V size invalid");
                std::vector<uint32_t> code(bytes / 4);
                std::ifstream f(file, std::ios::binary);
                f.read(reinterpret_cast<char*>(code.data()), bytes);
                if (!f || code[0] != 0x07230203)
                    throw std::runtime_error("Cemu SPIR-V invalid");
                VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                mi.codeSize = bytes;
                mi.pCode = code.data();
                VkShaderModule module{};
                Check(api.vkCreateShaderModule(device, &mi, nullptr, &module),
                      "Cemu shader module");
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.layout = layout;
                ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.stage.module = module;
                ci.stage.pName = "main";
                const auto r = api.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr,
                                                            &pipelines[i]);
                api.vkDestroyShaderModule(device, module, nullptr);
                Check(r, "Cemu compute pipeline");
            }
        } catch (...) {
            Cleanup();
            throw;
        }
    }
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    ~Pipeline() {
        Cleanup();
    }
    void Initialize(VkCommandBuffer cmd) {
        if (initialized)
            return;
        std::array<VkImageMemoryBarrier, 7> barriers{};
        for (unsigned i = 0; i < images.size(); ++i) {
            auto& b = barriers[i];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = images[i].image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        api.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                 uint32_t(barriers.size()), barriers.data());
        initialized = true;
    }
    void RecordBarrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds) const {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        api.vkCmdPipelineBarrier(cmd, ss, ds, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    void Prepare(VkCommandBuffer cmd) const {
        RecordBarrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        Dispatch(cmd, 0, (profile.width / 4 + 15) / 16, (profile.height / 4 + 15) / 16, 1);
        ComputeBarrier(cmd);
        Dispatch(cmd, 1, (profile.grid_width + 7) / 8, (profile.grid_height + 7) / 8, 2);
        ComputeBarrier(cmd);
        Dispatch(cmd, 2, uint32_t((profile.pixels / 2 + 255) / 256), 1, 1);
        RecordBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
    }
    void Compose(VkCommandBuffer cmd) const {
        RecordBarrier(
            cmd, VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        Dispatch(cmd, 3, uint32_t((profile.pixels / 4 + 255) / 256), 1, 1);
        RecordBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    void Invalidate(BufferIndex which) const {
        MappedRange(which, false);
    }
    void Flush(BufferIndex which) const {
        MappedRange(which, true);
    }

private:
    Api api;
    VkDevice device{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    VkPipelineLayout layout{};
    std::array<VkPipeline, 4> pipelines{};
    bool initialized{};
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("Cemu Vulkan memory type unavailable");
    }
    void CreateBuffer(Buffer& b, size_t bytes) {
        b.bytes = bytes;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(api.vkCreateBuffer(device, &bi, nullptr, &b.buffer), "Cemu buffer");
        VkMemoryRequirements req{};
        api.vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.buffer = b.buffer;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            MemoryType(req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(api.vkAllocateMemory(device, &ai, nullptr, &b.memory), "Cemu normal buffer memory");
        Check(api.vkBindBufferMemory(device, b.buffer, b.memory, 0), "Cemu bind buffer");
        void* ptr{};
        Check(api.vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &ptr), "Cemu map buffer");
        b.mapped = static_cast<uint8_t*>(ptr);
    }
    void CreateImage(Image& im, uint32_t w, uint32_t h) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(api.vkCreateImage(device, &ci, nullptr, &im.image), "Cemu image");
        VkMemoryRequirements req{};
        api.vkGetImageMemoryRequirements(device, im.image, &req);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = im.image;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(api.vkAllocateMemory(device, &ai, nullptr, &im.memory), "Cemu image memory");
        Check(api.vkBindImageMemory(device, im.image, im.memory, 0), "Cemu bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(api.vkCreateImageView(device, &vi, nullptr, &im.view), "Cemu image view");
    }
    void ComputeBarrier(VkCommandBuffer cmd) const {
        RecordBarrier(cmd, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    void Dispatch(VkCommandBuffer cmd, unsigned index, uint32_t x, uint32_t y, uint32_t z) const {
        api.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[index]);
        api.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                    nullptr);
        uint32_t phase{};
        api.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &phase);
        api.vkCmdDispatch(cmd, x, y, z);
    }
    void MappedRange(BufferIndex which, bool flush) const {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = buffers[which].memory;
        range.size = VK_WHOLE_SIZE;
        Check(flush ? api.vkFlushMappedMemoryRanges(device, 1, &range)
                    : api.vkInvalidateMappedMemoryRanges(device, 1, &range),
              "Cemu mapped range");
    }
    void Cleanup() {
        for (auto p : pipelines)
            if (p)
                api.vkDestroyPipeline(device, p, nullptr);
        if (layout)
            api.vkDestroyPipelineLayout(device, layout, nullptr);
        if (pool)
            api.vkDestroyDescriptorPool(device, pool, nullptr);
        if (set_layout)
            api.vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        for (auto& b : buffers) {
            if (b.mapped)
                api.vkUnmapMemory(device, b.memory);
            if (b.buffer)
                api.vkDestroyBuffer(device, b.buffer, nullptr);
            if (b.memory)
                api.vkFreeMemory(device, b.memory, nullptr);
        }
        for (auto& im : images) {
            if (im.view)
                api.vkDestroyImageView(device, im.view, nullptr);
            if (im.image)
                api.vkDestroyImage(device, im.image, nullptr);
            if (im.memory)
                api.vkFreeMemory(device, im.memory, nullptr);
        }
    }
};
#undef SNAPRYU_CEMU_GPU_FUNCTIONS
} // namespace SnapRyuCemu
