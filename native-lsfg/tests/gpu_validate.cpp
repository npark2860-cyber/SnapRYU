// SPDX-License-Identifier: GPL-3.0-or-later
// Load both DLLs through their public C ABI; compare complete RGBA images.
#include <windows.h>
#undef CreateSemaphore
#include "lsfg_cemu.h"
#include "lsfg-vk-common/vulkan/vulkan.hpp"
#include "lsfg-vk-common/vulkan/buffer.hpp"
#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << std::endl;
        ExitProcess(2); // Do not unwind/destroy possibly in-flight GPU resources.
    }
}
void check(VkResult result, const char* operation) {
    require(result == VK_SUCCESS, std::string(operation) + ": " + std::to_string(result));
}
struct Host {
    HMODULE loader{};
    PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    bool fp16{};
    std::string name;
    std::unique_ptr<vk::Vulkan> v;
    Host() {
        loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(loader != nullptr, "system ARM64 Vulkan loader unavailable");
        gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        require(gipa != nullptr, "Vulkan entry point missing");
        auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "LSFG ARM64 DLL validation"; app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo = &app;
        check(create(&ii, nullptr, &instance), "create instance");
        const auto fi = vk::initVulkanInstanceFuncs(instance, gipa, false);
        gdpa = fi.GetDeviceProcAddr;
        uint32_t count{};
        check(fi.EnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(fi.EnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            fi.GetPhysicalDeviceProperties2(candidate, &p);
            if (p.properties.vendorID == 0x5143 || std::strstr(p.properties.deviceName, "Adreno")) {
                physical = candidate; name = p.properties.deviceName; break;
            }
        }
        require(physical != nullptr, "intended physical Adreno GPU not found; no fallback");
        VkPhysicalDeviceVulkan12Features available12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &available12};
        fi.GetPhysicalDeviceFeatures2(physical, &available);
        require(available12.timelineSemaphore && available.features.shaderStorageImageExtendedFormats,
                "required Vulkan features unavailable");
        fp16 = available12.shaderFloat16 != 0;
        VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled12.timelineSemaphore = VK_TRUE; enabled12.shaderFloat16 = fp16;
        VkPhysicalDeviceFeatures enabled{}; enabled.shaderStorageImageExtendedFormats = VK_TRUE;
        fi.GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        fi.GetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        for (; family < count; ++family) if (queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
        require(family < count, "compute queue missing");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.pNext = &enabled12; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.pEnabledFeatures = &enabled;
        check(fi.CreateDevice(physical, &di, nullptr, &device), "create device");
        const auto fd = vk::initVulkanDeviceFuncs(fi, device, false);
        fd.GetDeviceQueue(device, family, 0, &queue);
        v = std::make_unique<vk::Vulkan>(instance, device, physical, fi, fd, false,
                                        std::nullopt, std::nullopt, family, queue, fp16);
    }
    ~Host() {
        v.reset();
        if (device) reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device, "vkDestroyDevice"))(device, nullptr);
        if (instance) reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"))(instance, nullptr);
        if (loader) FreeLibrary(loader);
    }
};
struct Api {
    HMODULE library{};
#define PROC(n) decltype(&lsfg_cemu_##n) n{}
    PROC(create); PROC(set_enabled); PROC(create_context); PROC(generate); PROC(generate_async);
    PROC(reset_history); PROC(drain); PROC(destroy_context); PROC(destroy); PROC(get_stats);
    PROC(status_string);
#undef PROC
    explicit Api(const std::filesystem::path& path) {
        library = LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(library != nullptr, "cannot load ARM64 DLL (Win32 " + std::to_string(GetLastError()) + ")");
#define LOAD(n) n = reinterpret_cast<decltype(n)>(GetProcAddress(library, "lsfg_cemu_" #n)); require(n != nullptr, "export missing: " #n)
        LOAD(create); LOAD(set_enabled); LOAD(create_context); LOAD(generate); LOAD(generate_async);
        LOAD(reset_history); LOAD(drain); LOAD(destroy_context); LOAD(destroy); LOAD(get_stats); LOAD(status_string);
#undef LOAD
    }
    ~Api() { FreeLibrary(library); }
};
void okay(LsfgCemuStatus status, const LsfgCemuError& error) {
    require(status == LSFG_CEMU_OK, "LSFG status " + std::to_string(status) + ": " + error.message);
}
struct Module {
    Api& api; LsfgCemu* module{};
    Module(Api& a, Host& host, const wchar_t* source, bool fp16) : api(a) {
        LsfgCemuCreateInfo ci{}; ci.struct_size = sizeof(ci); ci.abi_version = LSFG_CEMU_ABI_VERSION;
        ci.instance = host.instance; ci.physical_device = host.physical; ci.device = host.device;
        ci.queue = host.queue; ci.queue_family_index = host.family;
        ci.get_instance_proc_addr = host.gipa; ci.get_device_proc_addr = host.gdpa;
        ci.lossless_dll_path = source; ci.gpu_wait_timeout_ns = 10'000'000'000ULL;
        ci.timeline_semaphore_enabled = ci.shader_storage_image_extended_formats_enabled = VK_TRUE;
        ci.fp16_shaders_enabled = fp16;
        LsfgCemuError e{}; auto s = api.create(&ci, &module, &e); okay(s, e);
    }
    void enable(bool value) { LsfgCemuError e{}; auto s = api.set_enabled(module, value, &e); okay(s, e); }
    LsfgCemuStats stats() {
        LsfgCemuStats s{}; s.struct_size = sizeof(s); LsfgCemuError e{};
        auto result = api.get_stats(module, &s, &e); okay(result, e); return s;
    }
    ~Module() { if (module) require(api.destroy(&module, nullptr) == LSFG_CEMU_OK, "module teardown failed"); }
};
struct Context {
    Module& m; LsfgCemuContext* context{};
    Context(Module& owner, uint32_t w, uint32_t h, float flow) : m(owner) {
        LsfgCemuContextInfo ci{sizeof(ci), w, h, VK_FORMAT_R8G8B8A8_UNORM, flow};
        LsfgCemuError e{}; auto s = m.api.create_context(m.module, &ci, &context, &e); okay(s, e);
    }
    void run(VkImage a, VkImage b, VkImage out, uint64_t seq, bool async) {
        LsfgCemuGenerateInfo gi{sizeof(gi), a, b, out, seq, seq + 1, VK_NULL_HANDLE};
        LsfgCemuError e{};
        auto s = (async ? m.api.generate_async : m.api.generate)(context, &gi, &e); okay(s, e);
        // Explicitly drain before readback, resource reuse and destruction.
        s = m.api.drain(context, 10'000'000'000ULL, &e); okay(s, e);
    }
    void reset() { LsfgCemuError e{}; auto s = m.api.reset_history(context, &e); okay(s, e); }
    ~Context() { if (context) require(m.api.destroy_context(&context, nullptr) == LSFG_CEMU_OK, "context teardown failed"); }
};
VkImageMemoryBarrier barrier(VkImage image, VkImageLayout old, VkAccessFlags src, VkAccessFlags dst) {
    return {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, src, dst, old, VK_IMAGE_LAYOUT_GENERAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
}
void finish(const vk::Vulkan& v, vk::CommandBuffer& cmd) {
    cmd.end(v);
    VkFence fence{}; VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(v.df().CreateFence(v.dev(), &ci, nullptr, &fence), "create fence");
    cmd.submit(v, {}, VK_NULL_HANDLE, 0, {}, VK_NULL_HANDLE, 0, fence);
    check(v.df().WaitForFences(v.dev(), 1, &fence, VK_TRUE, 10'000'000'000ULL), "finite GPU wait");
    v.df().DestroyFence(v.dev(), fence, nullptr);
}
void image_barrier(const vk::Vulkan& v, vk::CommandBuffer& cmd, const VkImageMemoryBarrier& b) {
    v.df().CmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
void upload(const vk::Vulkan& v, const vk::Image& image, const std::vector<uint8_t>& bytes, bool first) {
    vk::Buffer buffer(v, bytes.data(), bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    vk::CommandBuffer cmd(v); cmd.begin(v);
    image_barrier(v, cmd, barrier(image.handle(), first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
        first ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT));
    cmd.copyBufferToImage(v, buffer, image);
    image_barrier(v, cmd, barrier(image.handle(), VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
    finish(v, cmd);
}
std::vector<uint8_t> readback(const vk::Vulkan& v, const vk::Image& image) {
    auto extent = image.getExtent();
    std::vector<uint8_t> bytes(size_t(extent.width) * extent.height * 4);
    vk::Buffer buffer(v, bytes.data(), bytes.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    vk::CommandBuffer cmd(v); cmd.begin(v);
    image_barrier(v, cmd, barrier(image.handle(), VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
    auto copy = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(v.fi().GetDeviceProcAddr(v.dev(), "vkCmdCopyImageToBuffer"));
    VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    copy(cmd.handle(), image.handle(), VK_IMAGE_LAYOUT_GENERAL, buffer.handle(), 1, &region);
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    v.df().CmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 1, &b, 0, nullptr, 0, nullptr);
    finish(v, cmd);
    void* mapped{}; check(v.df().MapMemory(v.dev(), buffer.allocation(), 0, bytes.size(), 0, &mapped), "map");
    std::memcpy(bytes.data(), mapped, bytes.size()); v.df().UnmapMemory(v.dev(), buffer.allocation());
    return bytes;
}
std::vector<uint8_t> fixture(unsigned w, unsigned h, int frame) {
    std::vector<uint8_t> bytes(size_t(w) * h * 4);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const auto i = (size_t(y) * w + x) * 4;
        const int left = int(w / 3) + frame * 8;
        const bool rect = int(x) >= left && int(x) < left + int(w / 4) && y > h / 4 && y < 3 * h / 4;
        bytes[i] = rect ? 220 : uint8_t(24 + x * 48 / w);
        bytes[i + 1] = rect ? 128 : uint8_t(36 + y * 64 / h);
        bytes[i + 2] = rect ? 48 : 52; bytes[i + 3] = 255;
    }
    return bytes;
}
void save_ppm(const std::filesystem::path& p, const std::vector<uint8_t>& bytes, unsigned w, unsigned h) {
    std::ofstream f(p, std::ios::binary); f << "P6\n" << w << " " << h << "\n255\n";
    for (size_t i = 0; i < bytes.size(); i += 4) f.write(reinterpret_cast<const char*>(&bytes[i]), 3);
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 5) { std::cerr << "usage: lsfg_gpu_validate embedded.dll reference.dll original-Lossless.dll output-dir\n"; return 2; }
    try {
        std::filesystem::path out = argv[4]; std::filesystem::create_directories(out);
        Host host; std::cout << "Physical GPU: " << host.name << std::endl;
        Api embedded(argv[1]), reference(argv[2]);
        std::ofstream report(out / "gpu-validation.json");
        report << "{\n\"device\":\"" << host.name << "\",\n\"comparison\":\"full RGBA readback vs preserved external-shader DLL\",\n\"cases\":[\n";
        unsigned cases = 0; uint64_t generated = 0;
        for (bool fp16 : {false, true}) {
            if (fp16 && !host.fp16) { std::cout << "FP16 unsupported on device\n"; continue; }
            Module current(embedded, host, nullptr, fp16), baseline(reference, host, argv[3], fp16);
            for (const auto cfg : {std::tuple{256u,256u,1.f}, {1280u,720u,1.f}, {1920u,1080u,1.f}, {1280u,720u,.75f}}) {
                const auto [w,h,flow] = cfg;
                std::cout << "Validating " << w << 'x' << h << " FP" << (fp16 ? 16 : 32) << " flow=" << flow << std::endl;
                Context c(current,w,h,flow), r(baseline,w,h,flow);
                current.enable(false); baseline.enable(false);
                auto before = current.stats();
                require(embedded.generate(c.context, nullptr, nullptr) == LSFG_CEMU_DISABLED, "OFF sync contract");
                require(embedded.generate_async(c.context, nullptr, nullptr) == LSFG_CEMU_DISABLED, "OFF async contract");
                require(current.stats().queue_submissions == before.queue_submissions, "OFF submits work");
                current.enable(true); baseline.enable(true);
                constexpr auto usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                vk::Image a(*host.v,{w,h},VK_FORMAT_R8G8B8A8_UNORM,usage), b(*host.v,{w,h},VK_FORMAT_R8G8B8A8_UNORM,usage);
                vk::Image result(*host.v,{w,h},VK_FORMAT_R8G8B8A8_UNORM,usage);
                upload(*host.v,result,std::vector<uint8_t>(size_t(w)*h*4),true);
                unsigned max_error = 0; uint64_t differences = 0;
                double total_ms = 0;
                for (unsigned step = 0; step < 4; ++step) {
                    if (step == 3) { c.reset(); r.reset(); }
                    upload(*host.v,a,fixture(w,h,int(step)),step == 0);
                    upload(*host.v,b,fixture(w,h,int(step)+1),step == 0);
                    auto seq = step == 3 ? 100u : step + 1;
                    r.run(a.handle(),b.handle(),result.handle(),seq,false);
                    const auto expected = readback(*host.v,result);
                    const auto start = std::chrono::steady_clock::now();
                    c.run(a.handle(),b.handle(),result.handle(),seq,step != 0);
                    total_ms += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
                    const auto actual = readback(*host.v,result);
                    for (size_t i=0;i<actual.size();++i) {
                        auto d=unsigned(std::abs(int(actual[i])-int(expected[i])));
                        max_error=std::max(max_error,d); differences += d != 0;
                    }
                    require(actual != fixture(w,h,int(step)+1), "output is merely the current original frame");
                    if (step == 1) save_ppm(out / (std::to_string(w)+"x"+std::to_string(h)+"-fp"+(fp16?"16":"32")+"-flow"+std::to_string(flow)+".ppm"),actual,w,h);
                    ++generated;
                }
                if (cases++) report << ",\n";
                report << "{\"width\":"<<w<<",\"height\":"<<h<<",\"precision\":"<<(fp16?16:32)
                       <<",\"flow_scale\":"<<flow<<",\"generated_frames\":4,\"max_byte_error\":"<<max_error
                       <<",\"different_bytes\":"<<differences<<",\"total_generate_and_drain_ms_including_warmup\":"<<total_ms<<"}";
                report.flush();
                require(differences == 0, "DLL output differs from preserved reference");
            }
            auto stats=current.stats();
            require(stats.shader_extraction_pass && stats.shader_modules == 51 && stats.compute_pipelines == 51,
                    "real original shader pipeline not initialized");
            require(stats.async_generations != 0, "async path was not exercised");
            require(stats.live_contexts == 0, "contexts leaked");
        }
        report << "\n],\n\"case_count\":"<<cases<<",\"generated_frames\":"<<generated
               <<",\n\"original_lossless_file_required_by_candidate\":false,\n\"status\":\"PASS\","
               <<"\n\"scope\":\"DLL fidelity and GPU execution; not game pacing, commercial-app parity or NPU proof\"\n}\n";
        std::cout << "PASS: "<<cases<<" configurations, "<<generated<<" generated frames, full RGBA byte equality" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        // Preserve any GPU resources whose completion is unknown; process cleanup owns them.
        ExitProcess(2);
    }
}
