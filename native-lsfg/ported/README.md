# lsfg-cemu-arm64

Windows ARM64용 재사용 LSFG compute 모듈. Cemu의 기존 `VkInstance`, `VkPhysicalDevice`, `VkDevice`, `VkQueue`, queue family와 frame 이미지를 빌려 사용한다. Cemu 저장소는 수정하지 않았다. Present/window/swapchain 코드는 라이브러리에 없다.

**개발자가 추가할 파일:** 이 폴더 전체를 Cemu dependency로 복사하고 `add_subdirectory(lsfg-cemu-arm64)` → `target_link_libraries(Cemu PRIVATE LSFGCemu::LSFGCemu)`를 적용한다. Cemu source에서는 `include/lsfg_cemu.h`를 include하고 `create` → `create_context` → `set_enabled` → `generate` → `destroy_context` → `destroy`를 호출한다. Runtime 변경·resize에는 `reset_history`/context 재생성 계약을 따른다.

정확한 코드, 이미지 layout/usage, synchronization, shutdown 및 DLL 준비는 **[CEMU_INTEGRATION.md](docs/CEMU_INTEGRATION.md)**를 따른다. Prebuilt `lib/lsfg_cemu_arm64.lib` 외에도 source/CMake target과 vendored dependencies를 모두 제공한다.

```text
include/lsfg_cemu.h             작은 C ABI, exception 차단/status 반환
src/lsfg_cemu.cpp               borrowed 객체와 host VkImage adapter
third_party/                   고정 legacy core 복사본 + Vulkan C headers
cmake/LSFGCemu.cmake            Cemu target 연결 helper
tests/                         C header와 오류/수명 계약 검사
lib/lsfg_cemu_arm64.lib         ARM64 Release /MD 정적 라이브러리
docs/CEMU_INTEGRATION.md       Cemu 통합 계약과 최소 호출 예제
third_party/lsfg_legacy/       upstream commit과 파일별 hash를 기록한 provenance
```

범위는 SDR `R8G8B8A8_UNORM`, 2X, FP32, Quality, explicit history reset이다. OFF가 기본이며 OFF generate에는 GPU 조회/작업·대기·history 처리가 없다. 첫 ON generate는 검증된 3 warmup을 내부 수행한다. Host가 이전/현재/출력 이미지를 모두 소유한다. 내부 GPU copy를 통해 고정 legacy descriptor/history를 재사용한다.

**ABI v2 (SnapRYU):** `generate_async`는 같은 검증·history 규칙으로 작업을 queue에 제출만 하고 반환한다. 순서는 queue 제출 순서와 ALL_COMMANDS barrier로 보장하고, CPU 대기는 같은 context의 이전 생성 완료 확인 한 번뿐이다(history 재구성 warmup은 기존처럼 동기). `fp16_shaders_enabled`는 같은 DLL의 FP16 shader 세트(51 pipeline 동일)를, `flow_scale`(0.25..1, 0=1)은 optical flow 해상도를 정한다. FP16·flow<1·async는 위 검증 범위 밖의 확장이다.

동일 device 내부 timeline과 finite fence wait를 사용한다. D3D/WGC/external memory/external semaphore/NPU/UI/scaling/present 기능은 제공하지 않는다. Timeout은 자원을 유지하고 오류로 반환한다. Queue와 module API는 host가 직렬화해야 한다.

## Source build

Native MSVC ARM64 developer shell에서 이 폴더를 기준으로:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLSFG_CEMU_BUILD_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Visual Studio generator는 `-A ARM64`를 지정하고 `--config Release`로 빌드한다. Package target은 ARM64EC/x64를 거부한다. 테스트 옵션은 기본 OFF라 Cemu source 통합에 test executable을 강제하지 않는다. 바이너리의 CRT/toolset 조건은 [통합 문서](docs/CEMU_INTEGRATION.md)에 있다.

Lossless.dll은 포함하지 않는다. User-selected UTF-16 경로를 host가 전달하고 PE resource만 읽는다. 기존 실기 PASS shader ABI의 SHA-256을 검사하므로 다른 DLL을 임의로 호환 처리하지 않는다. GPL-3.0-or-later legacy core의 source/license notices와 수정 이력을 보존했다. Vulkan headers는 Apache-2.0 notices를 유지한다.

이 패키지의 ARM64 빌드와 C ABI/오류 처리 계약 테스트는 통과했다. 실제 Cemu renderer에서 모듈을 실행하는 검증과 full-frame 정확도 검증은 아직 남아 있으며, 알려진 A=A 입력 결과는 경계 5픽셀에서 최대 RGB 오차 8이다. 검증 세부사항은 [validation/summary.json](docs/validation/summary.json)에 있다.
