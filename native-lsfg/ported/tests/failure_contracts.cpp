// SPDX-License-Identifier: GPL-3.0-or-later
// C ABI failure contracts. No Vulkan loader or physical GPU is used.
#include "lsfg_cemu.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
unsigned checks{}, failures{}, instanceLookups{}, deviceLookups{}, waits{},
    unexpectedCalls{};
enum class WaitMode { Timeout, Lost, Throw, Success };
WaitMode waitMode = WaitMode::Timeout;

template <class T> T Handle(uintptr_t n) {
  if constexpr (std::is_pointer_v<T>)
    return reinterpret_cast<T>(n);
  else
    return static_cast<T>(n);
}
void Expect(bool value, const char *message) {
  ++checks;
  if (!value) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", message);
  }
}
VKAPI_ATTR VkResult VKAPI_CALL MockWait(VkDevice device, uint32_t count,
                                        const VkFence *fences, VkBool32 all,
                                        uint64_t timeout) {
  ++waits;
  Expect(device == Handle<VkDevice>(0x300),
         "exact borrowed device passed to readiness wait");
  Expect(count == 1 && fences && *fences == Handle<VkFence>(0x700) &&
             all == VK_TRUE,
         "host readiness fence forwarded unchanged");
  Expect(timeout <= 1'000'000'000ULL,
         "readiness wait is finite and within host budget");
  switch (waitMode) {
  case WaitMode::Timeout:
    return VK_TIMEOUT;
  case WaitMode::Lost:
    return VK_ERROR_DEVICE_LOST;
  case WaitMode::Throw:
    throw std::runtime_error("synthetic host dispatch exception");
  case WaitMode::Success:
    return VK_SUCCESS;
  }
  return VK_ERROR_UNKNOWN;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL MockGipa(VkInstance, const char *) {
  ++instanceLookups;
  ++unexpectedCalls;
  return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL MockGdpa(VkDevice device,
                                                  const char *name) {
  ++deviceLookups;
  Expect(device == Handle<VkDevice>(0x300),
         "dispatch lookup uses exact borrowed device");
  if (name && std::strcmp(name, "vkWaitForFences") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&MockWait);
  ++unexpectedCalls;
  return nullptr;
}
LsfgCemuCreateInfo CreateInfo(const wchar_t *path) {
  LsfgCemuCreateInfo ci{};
  ci.struct_size = sizeof(ci);
  ci.abi_version = LSFG_CEMU_ABI_VERSION;
  ci.instance = Handle<VkInstance>(0x100);
  ci.physical_device = Handle<VkPhysicalDevice>(0x200);
  ci.device = Handle<VkDevice>(0x300);
  ci.queue = Handle<VkQueue>(0x400);
  ci.queue_family_index = 7;
  ci.get_instance_proc_addr = MockGipa;
  ci.get_device_proc_addr = MockGdpa;
  ci.lossless_dll_path = path;
  ci.gpu_wait_timeout_ns = 1'000'000'000ULL;
  ci.timeline_semaphore_enabled = VK_TRUE;
  ci.shader_storage_image_extended_formats_enabled = VK_TRUE;
  return ci;
}
LsfgCemuContextInfo ContextInfo() {
  LsfgCemuContextInfo ci{};
  ci.struct_size = sizeof(ci);
  ci.width = 256;
  ci.height = 256;
  ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  return ci;
}
LsfgCemuGenerateInfo GenerateInfo() {
  LsfgCemuGenerateInfo gi{};
  gi.struct_size = sizeof(gi);
  gi.previous_image = Handle<VkImage>(0x500);
  gi.current_image = Handle<VkImage>(0x600);
  gi.output_image = Handle<VkImage>(0x800);
  gi.previous_frame_id = 10;
  gi.current_frame_id = 11;
  gi.input_ready_fence = Handle<VkFence>(0x700);
  return gi;
}
LsfgCemuStats Stats(LsfgCemu *module) {
  LsfgCemuStats s{};
  s.struct_size = sizeof(s);
  Expect(lsfg_cemu_get_stats(module, &s, nullptr) == LSFG_CEMU_OK,
         "stats query succeeds");
  Expect(s.shader_modules == 0 && s.compute_pipelines == 0 &&
             s.recorded_dispatches == 0 && s.queue_submissions == 0 &&
             s.completed_generations == 0 && s.generated_outputs == 0,
         "failure path performs no LSFG GPU work");
  return s;
}
void Destroy(LsfgCemu *&module, LsfgCemuContext *&context) {
  Expect(lsfg_cemu_destroy_context(&context, nullptr) == LSFG_CEMU_OK &&
             !context,
         "CPU-only context can be destroyed after failed generation");
  Expect(lsfg_cemu_destroy_context(&context, nullptr) == LSFG_CEMU_OK,
         "null context destruction is idempotent");
  Expect(lsfg_cemu_destroy(&module, nullptr) == LSFG_CEMU_OK && !module,
         "module destruction succeeds without touching host Vulkan objects");
  Expect(lsfg_cemu_destroy(&module, nullptr) == LSFG_CEMU_OK,
         "null module destruction is idempotent");
}
} // namespace

int main() {
  try {
    LsfgCemuError error{};
    LsfgCemu *module = nullptr;
    LsfgCemuContext *context = nullptr;
    const auto prefix =
        std::filesystem::temp_directory_path() /
        ("lsfg-cemu-contracts-" + std::to_string(GetCurrentProcessId()));
    const auto missing = prefix.wstring() + L"-missing.dll";
    auto ci = CreateInfo(missing.c_str());
    auto cci = ContextInfo();
    auto gi = GenerateInfo();

    Expect(lsfg_cemu_create(nullptr, &module, &error) ==
                   LSFG_CEMU_INVALID_ARGUMENT &&
               !module,
           "null create input contained");
    Expect(!error.vk_result_valid,
           "validation error is not reported as Vulkan driver error");
    auto invalid = ci;
    invalid.gpu_wait_timeout_ns = UINT64_MAX;
    Expect(lsfg_cemu_create(&invalid, &module, &error) ==
               LSFG_CEMU_INVALID_ARGUMENT,
           "unbounded GPU timeout rejected");
    invalid = ci;
    invalid.timeline_semaphore_enabled = VK_FALSE;
    Expect(lsfg_cemu_create(&invalid, &module, &error) == LSFG_CEMU_UNSUPPORTED,
           "missing enabled timeline feature rejected without querying device");
    invalid = ci;
    invalid.abi_version = 1u;
    Expect(lsfg_cemu_create(&invalid, &module, &error) ==
                   LSFG_CEMU_INVALID_ARGUMENT &&
               !module,
           "ABI v1 host rejected after the v2 struct changes");
    invalid = ci;
    invalid.fp16_shaders_enabled = 2;
    Expect(lsfg_cemu_create(&invalid, &module, &error) ==
               LSFG_CEMU_INVALID_ARGUMENT,
           "invalid fp16 flag rejected");
    Expect(lsfg_cemu_create(&ci, &module, &error) == LSFG_CEMU_OK && module,
           "OFF creation accepts a host-owned DLL path without opening it");
    Expect(instanceLookups == 0 && deviceLookups == 0,
           "OFF creation resolves no Vulkan commands");
    Expect(lsfg_cemu_create_context(module, &cci, &context, &error) ==
               LSFG_CEMU_OK,
           "context creation is CPU-only");
    auto badFlow = cci;
    badFlow.flow_scale = 0.1f;
    LsfgCemuContext *rejected = nullptr;
    Expect(lsfg_cemu_create_context(module, &badFlow, &rejected, &error) ==
                   LSFG_CEMU_INVALID_ARGUMENT &&
               !rejected,
           "flow_scale below 0.25 rejected (0 means 1.0)");
    const auto savedModule = module;
    Expect(lsfg_cemu_destroy(&module, &error) == LSFG_CEMU_BUSY &&
               module == savedModule,
           "live context keeps module ownership intact");
    Expect(lsfg_cemu_generate(context, nullptr, &error) == LSFG_CEMU_DISABLED,
           "OFF generate does not inspect input pointer");
    Expect(lsfg_cemu_generate_async(context, nullptr, &error) ==
               LSFG_CEMU_DISABLED,
           "OFF generate_async does not inspect input pointer");
    auto stats = Stats(module);
    Expect(!stats.enabled && stats.live_contexts == 1 &&
               stats.history_resets == 0,
           "default OFF does not process history");
    Expect(instanceLookups == 0 && deviceLookups == 0,
           "OFF context and generate use no Vulkan functions");
    Expect(lsfg_cemu_set_enabled(module, 2, &error) ==
               LSFG_CEMU_INVALID_ARGUMENT,
           "invalid enable value rejected");
    Expect(lsfg_cemu_set_enabled(module, VK_TRUE, &error) == LSFG_CEMU_OK,
           "enable is CPU-only");

    waitMode = WaitMode::Timeout;
    Expect(
        lsfg_cemu_generate(context, &gi, &error) == LSFG_CEMU_TIMEOUT,
        "host input fence timeout returns status -5 without exiting process");
    Expect(error.vk_result_valid && error.vk_result == VK_TIMEOUT,
           "actual readiness VkResult 2 is retained");
    Expect(std::strstr(error.message, "host input readiness fence") != nullptr,
           "timeout identifies readiness boundary");
    Stats(module);
    const auto beforeOff = waits;
    Expect(lsfg_cemu_set_enabled(module, VK_FALSE, nullptr) == LSFG_CEMU_OK,
           "disable remains available after pre-submit timeout");
    Expect(lsfg_cemu_generate(context, nullptr, nullptr) ==
                   LSFG_CEMU_DISABLED &&
               waits == beforeOff,
           "OFF bypasses readiness wait after timeout");
    Expect(lsfg_cemu_set_enabled(module, VK_TRUE, nullptr) == LSFG_CEMU_OK,
           "reenable remains CPU-only");
    waitMode = WaitMode::Throw;
    Expect(lsfg_cemu_generate(context, &gi, &error) == LSFG_CEMU_INTERNAL_ERROR,
           "host dispatch C++ exception is contained by C ABI");
    Expect(!error.vk_result_valid &&
               std::strstr(error.message, "synthetic host dispatch exception"),
           "C++ failure remains distinct from actual VkResult");
    waitMode = WaitMode::Success;
    Expect(lsfg_cemu_generate(context, &gi, &error) == LSFG_CEMU_DLL_ERROR,
           "successful readiness then nonexistent DLL returns DLL_ERROR");
    Expect(!error.vk_result_valid, "missing DLL is not a Vulkan error");
    Stats(module);
    Expect(lsfg_cemu_reset_history(context, nullptr) == LSFG_CEMU_OK,
           "unsubmitted DLL failure can be reset without GPU work");
    Destroy(module, context);

    // Device loss is terminal for this borrowed VkDevice even if it was
    // first observed by the optional host fence before GPU initialization.
    Expect(lsfg_cemu_create(&ci, &module, nullptr) == LSFG_CEMU_OK,
           "create lost-device case");
    Expect(lsfg_cemu_create_context(module, &cci, &context, nullptr) ==
               LSFG_CEMU_OK,
           "create lost-device context");
    lsfg_cemu_set_enabled(module, VK_TRUE, nullptr);
    waitMode = WaitMode::Lost;
    Expect(lsfg_cemu_generate(context, &gi, &error) == LSFG_CEMU_DEVICE_LOST,
           "host fence device loss returns terminal status");
    Expect(error.vk_result_valid && error.vk_result == VK_ERROR_DEVICE_LOST,
           "actual device-loss VkResult -4 is retained");
    const auto afterLoss = deviceLookups;
    auto retry = gi;
    retry.input_ready_fence = VK_NULL_HANDLE;
    Expect(lsfg_cemu_generate(context, &retry, &error) == LSFG_CEMU_DEVICE_LOST,
           "known lost host device rejects later generation before "
           "initialization");
    Expect(deviceLookups == afterLoss,
           "lost module performs no new Vulkan lookup/work");
    Expect(lsfg_cemu_reset_history(context, &error) == LSFG_CEMU_DEVICE_LOST,
           "history reset cannot revive lost host device");
    Stats(module);
    Destroy(module, context);

    // This file contains no executable PE or vendor resources. The hash
    // guard must reject it without loading code or entering Vulkan.
    const std::filesystem::path malformed(prefix.wstring() + L"-malformed.dll");
    struct RemoveFixture {
      std::filesystem::path path;
      ~RemoveFixture() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
      }
    } cleanup{malformed};
    {
      std::ofstream f(malformed, std::ios::binary);
      f << "not a PE resource DLL";
    }
    auto malformedPath = malformed.wstring();
    ci = CreateInfo(malformedPath.c_str());
    Expect(lsfg_cemu_create(&ci, &module, nullptr) == LSFG_CEMU_OK,
           "create malformed-DLL case OFF");
    Expect(lsfg_cemu_create_context(module, &cci, &context, nullptr) ==
               LSFG_CEMU_OK,
           "create malformed-DLL context");
    lsfg_cemu_set_enabled(module, VK_TRUE, nullptr);
    Expect(lsfg_cemu_generate(context, &retry, &error) == LSFG_CEMU_DLL_ERROR,
           "malformed DLL fails validated legacy hash preflight");
    Expect(!error.vk_result_valid,
           "malformed DLL produces no fabricated VkResult");
    Stats(module);
    Destroy(module, context);
    Expect(instanceLookups == 0 && unexpectedCalls == 0,
           "all failure fixtures remained outside Vulkan GPU initialization");
    Expect(std::strcmp(lsfg_cemu_status_string(LSFG_CEMU_TIMEOUT), "TIMEOUT") ==
               0,
           "status text is stable");
    Expect(
        std::strcmp(lsfg_cemu_status_string(static_cast<LsfgCemuStatus>(999)),
                    "UNKNOWN_STATUS") == 0,
        "unknown status text is safe");
    std::printf(
        "{\"suite\":\"failure_contracts\",\"checks\":%u,\"failures\":%u,"
        "\"mock_fence_waits\":%u,\"instance_lookups\":%u,"
        "\"unexpected_dispatch_lookups\":%u,\"gpu_submissions\":0}\n",
        checks, failures, waits, instanceLookups, unexpectedCalls);
    return failures ? 1 : 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "test harness failed: %s\n", e.what());
    return 2;
  }
}
