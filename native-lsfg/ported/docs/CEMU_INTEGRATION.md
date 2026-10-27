# Cemu Windows ARM64 integration contract — C ABI v1

이 폴더는 Cemu가 소유한 Vulkan 객체와 최종 frame을 받아 2X generated image를 만드는 정적 라이브러리다. Cemu 저장소에는 아직 적용하지 않았다. 실제 게임 호환성을 완료했다는 뜻이 아니다. 기존 standalone PoC/Layer는 그대로 보존했고, 모듈은 별도 target과 core 복사본을 사용한다.

## 1. CMake와 파일 추가

폴더 전체를 예를 들어 Cemu source의 `dependencies/lsfg-cemu-arm64`에 복사한다. `include`, `src`, `third_party`, `cmake`, `CMakeLists.txt`, 라이선스와 provenance를 함께 유지한다. 소스 통합 방식:

```cmake
# Cemu target이 정의된 뒤. 실제 target 이름에 맞춰 Cemu만 바꾼다.
add_subdirectory(dependencies/lsfg-cemu-arm64)
target_link_libraries(Cemu PRIVATE LSFGCemu::LSFGCemu)
```

또는:

```cmake
include(dependencies/lsfg-cemu-arm64/cmake/LSFGCemu.cmake)
lsfg_cemu_attach(Cemu)
```

둘 중 하나만 사용한다. Public header는 `#include "lsfg_cemu.h"`다. Target은 include path와 Windows `bcrypt` 의존성을 전달하며, Vulkan loader import library를 강제하지 않는다. Host의 `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`를 전달한다. Public header는 C로도 컴파일되며 core C++ headers는 Cemu에 노출하지 않는다.

소스 빌드는 native Windows ARM64 MSVC와 CMake 3.24+, C++20을 사용한다. 내부 namespace 격리 매크로는 target PRIVATE다. `lib/lsfg_cemu_arm64.lib`는 Release `/MD`, MSVC 14.52 ARM64 빌드 참고본이다. 직접 사용하면 public include와 `third_party/vulkan_headers/include`, `bcrypt.lib`를 연결한다. Debug `/MDd`, 다른 CRT·toolset·옵션을 쓰는 Cemu는 source target을 다시 빌드한다. `.lib`에 모듈 core object가 포함되어 별도 legacy library를 추가할 필요가 없다.

## 2. 초기화와 borrowed 객체

Cemu renderer가 만든 다음 객체를 전달한다. 모듈은 instance/device/queue를 생성하거나 파괴하지 않고, queue를 index 0에서 재조회하지 않는다.

| CreateInfo field | Host 요구조건 |
|---|---|
| `instance` | 유효한 Vulkan 1.2 이상 instance |
| `physical_device`, `device` | 같은 host instance/device 관계. 생성에 쓴 physical device |
| `queue`, `queue_family_index` | 그 device에서 생성한 실제 **unprotected compute-capable queue**와 실제 family. Index가 0일 필요 없음 |
| `get_instance_proc_addr`, `get_device_proc_addr` | 해당 host loader/device의 유효한 함수. Layer next-chain 추측을 하지 않음 |
| `timeline_semaphore_enabled` | 기존 `VkDevice` 생성 시 `VkPhysicalDeviceVulkan12Features.timelineSemaphore=VK_TRUE`를 실제 활성화했다는 선언 |
| `shader_storage_image_extended_formats_enabled` | 기존 device의 `VkPhysicalDeviceFeatures.shaderStorageImageExtendedFormats=VK_TRUE` 활성화 선언 |
| `lossless_dll_path` | 사용자 설치 DLL의 UTF-16 경로. `create` 때 복사됨 |
| `gpu_wait_timeout_ns` | 1ns–60초. 보통 `10'000'000'000ULL`. 무한 대기 불허 |

기능 지원 query와 실제 device enable은 다르다. Host가 필요한 features를 활성화한 device를 준비해야 한다. Module은 이미 생성된 device의 feature를 나중에 켜지 못한다. 따라서 runtime 체크박스를 지원하려면 Cemu의 Vulkan device 생성 단계에서 두 features를 지원 여부에 따라 활성화해야 한다. 이는 Cemu 측 후속 작업이며 이 패키지가 renderer 설정을 변경하지는 않는다.

`lsfg_cemu_create`와 `lsfg_cemu_create_context`는 CPU metadata만 만든다. 초기 상태는 **OFF**이며 shader 추출·Vulkan query·GPU object 생성·submit은 첫 ON `generate`까지 지연한다. 실패는 `LsfgCemuStatus`와 선택적 caller-owned `LsfgCemuError`로 반환한다. C++ 예외는 C ABI를 넘어가지 않는다.

## 3. Context와 첫 버전 범위

`LsfgCemuContextInfo`에 `struct_size=sizeof(info)`, final-frame width/height, `VK_FORMAT_R8G8B8A8_UNORM`을 지정한다. 최소 extent는 각 축 128이며 device의 `maxImageDimension2D`를 넘을 수 없다. 최초 구현은 SDR RGBA8·2X·FP32·Quality 고정이다. FP16/Performance/HDR/3X/4X options는 API에 없다.

모듈은 context당 내부 input pair, generated image, history resources를 소유한다. 이 내부 image들은 외부로 공개하지 않는다. `previous_image/current_image/output_image`는 모두 **Cemu 소유**다. Host output에 same-device `vkCmdCopyImage`로 결과를 기록하며 소유권을 이전하지 않는다. CPU staging/readback은 모듈 경로에 없다.

## 4. 입력·출력 이미지 계약

| 항목 | previous/current | output |
|---|---|---|
| Format | `VK_FORMAT_R8G8B8A8_UNORM` | 동일 |
| Extent | context width×height | 동일 |
| Type/tiling | 2D, optimal tiling, single sample | 동일 |
| Subresource | mip 0, array layer 0, color; v1은 single mip/layer image | 동일 |
| Required usage | `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` | `VK_IMAGE_USAGE_TRANSFER_DST_BIT` 및 이후 Cemu 소비 경로에 필요한 flags |
| Entry layout | `VK_IMAGE_LAYOUT_GENERAL` | `VK_IMAGE_LAYOUT_GENERAL` |
| Success return layout | `VK_IMAGE_LAYOUT_GENERAL` | `VK_IMAGE_LAYOUT_GENERAL` |
| Queue ownership | 전달한 queue family가 소유, unprotected | 동일 |
| Memory/aliasing | valid bound memory. 두 입력이 같은 image여도 가능 | 입력과 다른 image 및 겹치지 않는 memory |

RGBA8가 아닌 Cemu 최종 출력(BGRA, sRGB, HDR 등)을 이 API에 그대로 넘기지 않는다. 필요한 format/layout 변환은 Cemu 쪽 renderer가 판단한다. 이 모듈은 기존 FSR/scaling을 대체하지 않는다.

```text
Cemu game rendering → 기존 FSR/scaling/post-process → final RGBA8 Vulkan frame
                                                       ↓ previous/current
                                                   LSFG compute
                                                       ↓ host output image
                                               Cemu 기존 output/present
```

Host는 첫 사용 전 output도 GENERAL로 전이한다(UNDEFINED를 API 입력 layout으로 전달하지 않음). 모듈은 host image layout을 GENERAL로 유지한 채 transfer-read/write visibility를 처리한다. 성공 시 output transfer write는 이후 same-queue memory read/write에 사용 가능하다. Cemu가 sampler/renderpass/다른 queue/layout으로 넘길 때 필요한 추가 transition/ownership transfer는 Cemu가 수행한다. Swapchain 이미지, surface, present 함수는 API에 없다.

## 5. Synchronization·thread safety·lifetime

**v1 API는 외부 직렬화가 필요하다.** 같은 module/context의 모든 API를 host mutex로 직렬화한다. Enabled `generate`, 그리고 ON/OFF 여부와 무관한 `drain`/`destroy_context` 동안에는 전달한 queue를 다른 thread나 renderer submit에서 사용하지 않도록 **Cemu의 queue mutex**도 잡는다. 다른 module이라도 같은 VkQueue를 공유하면 같은 queue lock을 사용한다. 다른 queue/family 소유권을 module이 자동으로 해결하지 않는다.

Host는 previous/current의 쓰기와 output의 기존 사용이 끝났고 GENERAL layout·family ownership을 만족하도록 준비한다. 동일 queue의 선행 rendering과 모듈 transfer는 module barrier로 순서화된다. 다른 queue에서 준비한 경우 host가 적절한 semaphore/ownership transfer/완료를 이미 처리해야 한다. 선택적 `input_ready_fence`는 이 readiness를 나타내는 **borrowed fence**이며 module은 finite wait만 하고 reset/destroy하지 않는다. 여러 queue 작업을 fence 하나로 표현할 경우 host가 해당 의존성을 결합해야 한다.

API는 binary wait semaphore를 소비하지 않는다. Cemu acquire/render-complete/present semaphore를 module에 넘기거나 두 번 기다리지 않는다. Module의 timeline semaphore는 동일 device의 내부 synchronization 전용이며 external semaphore나 Win32 handle은 없다.

`LSFG_CEMU_OK` 반환은 모든 module GPU work와 output copy가 fence로 완료됐다는 뜻이다. Host는 이후 자신의 output submit/present 경로를 진행한다. GPU wait는 해당 API 호출의 남은 유한 budget으로 제한한다. CPU shader compilation이나 임의 driver entry point 자체의 wall-clock 실행 시간까지 중단하는 API는 아니다.

Host images/device/queue는 호출 도중 파괴·재할당·재사용하면 안 된다. 성공 후 host 이미지는 module descriptor에 보관되지 않으며 재사용할 수 있다. Submit 이후 실패/timeout이면 **모든 해당 host image와 Vulkan 객체를 유지**하고 `lsfg_cemu_drain`으로 완료를 확인한다. Input readiness fence만 timeout하고 module 제출 전이면 추가 pending GPU 자원은 없다. 실패 출력은 사용하지 않는다.

## 6. Warmup와 temporal history

첫 enabled generation과 reset 이후 generation은 previous를 내부 input pair에 채운 뒤 **정확히 3회 warmup**한다. Legacy Alpha1/Beta0의 temporal 슬롯 3개를 채우기 위한 기존 PASS 절차다. 이어 current를 올바른 parity slot에 넣고 generated midpoint 한 장을 계산한다. Host는 warmup 호출을 따로 할 필요가 없다. Warmup 출력은 host output에 내보내지 않는다.

연속 프레임에서는 current 한 장만 새로 복사하고 history를 유지한다. `previous_frame_id/current_frame_id`는 frame 내용의 순서 번호다. Swapchain image index가 아니다. `current > previous`여야 하고 다음 호출의 `previous`는 직전 성공한 `current`와 같아야 한다. Host는 이 ID가 실제 같은 이미지 내용을 뜻하도록 관리한다. 불연속이면 `NEEDS_RESET`이고, reset 없이 history를 임의 추정하지 않는다.

Scene cut, seek/load-state, pause 후 불연속 재개, frame drop/skip, renderer/output 형식 변경, FSR 최종 extent 변경, ON/OFF 전환 때 history를 reset한다. `lsfg_cemu_reset_history`는 CPU flag만 설정하며 다음 enabled generation이 context history와 timeline을 재생성하고 3 warmup을 수행한다. 이미 pending이면 BUSY이므로 drain이 먼저다.

## 7. Resize·swapchain recreation·체크박스

- **OFF 기본:** `create` 후 그대로 두거나 모듈 자체를 만들지 않아도 된다. OFF `generate(context, NULL, ...)`도 `DISABLED`를 반환하며 Vulkan 조회/대기/submit/dispatch/이미지 작업/history 처리를 하지 않는다. Cemu는 원래 renderer/present를 그대로 실행한다.
- **ON / 2X:** `lsfg_cemu_set_enabled(module,VK_TRUE,...)`. 이 호출은 GPU 작업을 하지 않는다. 유효한 previous/current pair를 준비한 다음 `generate`를 호출한다. 첫 ON의 준비 지연은 이 호출에 발생한다.
- **OFF 전환:** `set_enabled(...,VK_FALSE,...)`는 GPU 작업 없이 다음 ON의 history rebuild를 예약한다. 이미 제출된 실패 작업을 취소하거나 완료로 간주하지 않는다. 정상 OFF 중 내부 자원은 보관되며, 필요하면 안전하게 context를 destroy해 해제한다.
- **다시 ON:** 다음 enabled `generate`에서 history rebuild + 3 warmup. OFF 기간의 프레임을 몰아서 처리하지 않는다.
- **크기 변경:** Cemu가 진행 중인 frame 소비를 정리하고 module drain/destroy_context 성공을 확인한 뒤, 새 extent의 host 이미지 및 context를 만든다. Context extent를 즉석에서 바꾸는 API는 없다.
- **같은 크기 swapchain recreation:** module은 swapchain을 소유하지 않는다. 이전 GPU 소비가 완료됐고 동일 frame stream이면 새 host image handles를 전달할 수 있다. 보통 discontinuity가 동반되므로 명시적 history reset을 권장한다. 실패/pending을 먼저 drain하고 old host image를 해제한다.

## 8. 실패 반환과 종료 순서

`LsfgCemuError.status`, 고정 길이 `message`, `vk_result_valid`, `vk_result`를 로그에 남긴다. `vk_result_valid=false`인 ABI/DLL/C++ 오류에 Vulkan driver 코드를 임의로 부여하지 않는다. `DISABLED`는 오류가 아니다. Shader extraction PASS/compute dispatch PASS와 interpolation correctness는 별도 판정이다.

Submit 이후 TIMEOUT/오류는 context를 poisoned 상태로 둔다. `generate` 재시도 대신 `drain(context, finite_timeout)` → `reset_history` → 새 frame pair 순서로 복구한다. `destroy_context(&context,...)`가 timeout이면 pointer를 NULL로 만들지 않고 자원을 보존한다. 다시 drain/destroy할 수 있다. Module은 프로세스를 종료하지 않는다.

Device loss는 Cemu renderer 전체의 복구 대상이다. Tail fence의 `vkWaitForFences`가 DEVICE_LOST로 완료되면 Vulkan의 in-use lifetime 규칙에 따라 모듈 자원을 해제할 수 있지만 생성 결과는 계속 실패다. `destroy_context`의 OK는 자원 해제 성공만 뜻한다. Module은 lost device에서 새로운 generation을 거부한다. 새 host device에는 새 module을 만든다. 관련 계약: [Lost Device](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device), [vkWaitForFences](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForFences.html).

정상 shutdown:

1. Cemu가 새 LSFG 호출을 중지하고 renderer/module/queue lock을 확보한다.
2. Host의 output 소비 완료와 module `drain` 결과를 확인한다.
3. 모든 `lsfg_cemu_destroy_context(&ctx,...)` 성공을 확인한다.
4. `lsfg_cemu_destroy(&module,...)`을 호출한다. Live context가 있으면 BUSY이며 module pointer가 유지된다.
5. Cemu가 자기 images/fences/queue/device/instance를 기존 순서로 해제한다.

Module은 host image, fence, instance, physical device, device, queue를 destroy하지 않는다. Pending timeout인 handle을 버리거나 host device를 먼저 파괴하지 않는다.

## 9. 최소 호출 예제

아래 코드는 **Cemu가 이미 만든 객체**를 연결하는 예다. Return code 분기는 생략하지 않는다. 현재 배포물에는 독립 Vulkan runtime host가 포함되지 않으며, 아래 코드는 Cemu 통합을 위한 호출 계약이다.

```cpp
#include "lsfg_cemu.h"
LsfgCemu* lsfg = nullptr;
LsfgCemuContext* ctx = nullptr;
LsfgCemuError error{};

LsfgCemuCreateInfo ci{};
ci.struct_size = sizeof(ci);
ci.abi_version = LSFG_CEMU_ABI_VERSION;
ci.instance = cemuInstance;
ci.physical_device = cemuPhysicalDevice;
ci.device = cemuDevice;
ci.queue = cemuQueue;
ci.queue_family_index = cemuQueueFamily;
ci.get_instance_proc_addr = hostGIPA;
ci.get_device_proc_addr = hostGDPA;
ci.lossless_dll_path = userSelectedLosslessDllPath; // const wchar_t*, owned install
ci.gpu_wait_timeout_ns = 10'000'000'000ULL;
ci.timeline_semaphore_enabled = VK_TRUE; // actually enabled by Cemu device creation
ci.shader_storage_image_extended_formats_enabled = VK_TRUE;
if (lsfg_cemu_create(&ci, &lsfg, &error) != LSFG_CEMU_OK) {
    ReportLsfgError(error); // keep original Cemu rendering
    return;
}
LsfgCemuContextInfo contextInfo{sizeof(contextInfo), width, height,
                              VK_FORMAT_R8G8B8A8_UNORM};
if (lsfg_cemu_create_context(lsfg, &contextInfo, &ctx, &error) != LSFG_CEMU_OK) {
    ReportLsfgError(error);
    lsfg_cemu_destroy(&lsfg, nullptr);
    return;
}
// Default is OFF. Only when the user checks LSFG:
if (lsfg_cemu_set_enabled(lsfg, VK_TRUE, &error) != LSFG_CEMU_OK) {
    ReportLsfgError(error);
    return;
}

// Under Cemu's renderer/module/queue locks, after final scaling and readiness:
LsfgCemuGenerateInfo frame{};
frame.struct_size = sizeof(frame);
frame.previous_image = previousFinalImage;
frame.current_image = currentFinalImage;
frame.output_image = cemuOwnedGeneratedImage;
frame.previous_frame_id = previousSequence;
frame.current_frame_id = currentSequence;
frame.input_ready_fence = optionalHostReadinessFence; // or VK_NULL_HANDLE
auto status = lsfg_cemu_generate(ctx, &frame, &error);
if (status == LSFG_CEMU_OK) {
    CemuPresentGeneratedThenOriginal(cemuOwnedGeneratedImage, currentFinalImage);
} else if (status == LSFG_CEMU_DISABLED) {
    CemuPresentOriginal(currentFinalImage);
} else {
    ReportLsfgError(error);
    // Keep the frame resources alive; drain before reusing/presenting an image
    // touched by a possibly submitted operation. Do not assume safe fallback.
}

// Cemu shutdown/recreation, after its output consumers have completed:
if (lsfg_cemu_destroy_context(&ctx, &error) != LSFG_CEMU_OK) {
    ReportLsfgError(error); // ctx remains valid; drain/retry; retain host resources
    return;
}
if (lsfg_cemu_destroy(&lsfg, &error) != LSFG_CEMU_OK) ReportLsfgError(error);
```

`ReportLsfgError`, `CemuPresentGeneratedThenOriginal` 등은 통합 위치를 설명하는 host pseudocode다. 실제 Cemu 함수명이나 renderer 수정본이 아니다. Cemu가 frame cadence·원본/생성 순서·present를 담당한다.

## 10. DLL과 provenance·정확성 한계

사용자는 정식 Lossless Scaling 설치본의 `Lossless.dll` 경로를 준비한다. 일반 Steam 경로의 예는 `C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling\Lossless.dll`이나, module은 이 경로를 하드코딩하지 않는다. Host가 넘긴 파일을 읽어 PE resources만 추출하며 `LoadLibrary`로 해당 DLL 코드를 실행하지 않는다. DLL 및 추출 shader payload는 패키지에 없다.

검증된 legacy DLL SHA-256은 `626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb`, SPIR-V resources 98개, 사용 module/pipeline 각각 51개다. 다른 SHA는 DLL_ERROR로 명시적 거부하며 새로운 ABI를 추측하지 않는다. 고정 GPL legacy core/Windows port/module 변경과 Vulkan headers는 [source-lock](../provenance/source-lock.json)에 기록한다. Core shaderchain·sampler·상수·기존 shader 변환은 기준선과 동일하게 보존한다.

Known motion midpoint smoke 및 generated transport PASS가 전체 품질 PASS를 뜻하지 않는다. 기준선 A=A의 경계 5 pixels, 최대 RGB error 8, interior max 0은 별도 FAIL로 유지한다. Upstream 전체 미수정 runtime의 동일 fixture 재현은 아직 미완료다. 이 패키지는 성능·HDR·범용 emulator 호환성이나 실제 Cemu frame 삽입을 인증하지 않는다.
