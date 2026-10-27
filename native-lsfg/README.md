# Original LSFG ARM64 DLL — 2026-10-15

사용자 보유 `Lossless.dll` 3.2.2.0의 SPIR-V 셰이더 98개를 그대로 내장한 Windows ARM64 네이티브 Vulkan 보간 DLL이다. Anvil/ONNX/QNN을 사용하지 않는다. 기존 legacy LSFG 연산 경로와 ABI 2를 유지한다.

## 배포 파일

- `bin/lsfg_arm64.dll`: 원본 셰이더 내장 DLL, ARM64 AA64, Release /MD.
- `bin/msvcp140.dll`, `bin/vcruntime140.dll`: 대응하는 ARM64 MSVC 실행 의존성.
- `lib/lsfg_arm64.lib`: ARM64 import library.
- `include/lsfg_cemu.h`: C ABI 2 헤더. Vulkan 헤더는 `source/native-lsfg/ported/third_party/vulkan_headers/include`에 포함한다.
- `source/native-lsfg`: 빌드 가능한 어댑터·legacy core·테스트 소스 및 기존 라이선스/출처 기록.
- `validation`: 실제 GPU 결과, CTest 기록, DLL import/export 목록, 원본 셰이더 해시 목록.
- `SHA256SUMS.txt`: 패키지 파일별 SHA-256.

이 파일은 독립적으로 호출하는 compute DLL이다. 상용 Lossless Scaling 앱의 DLL ABI를 그대로 구현한 제품은 아니다. 기존 앱에서 사용하려면 아래 ABI로 연결해야 한다. 별도 b30-lsfg-embedded 실행 파일은 이 이름과 내장 셰이더 경로로 연결된다. b29 이하 실행 파일에 DLL만 교체하는 방식은 지원하지 않는다.

## 호스트 연결

1. ARM64 호스트가 `bin`의 DLL 3개를 같은 디렉터리에 두고 `lsfg_arm64.lib`로 링크하거나 `LoadLibraryExW`/`GetProcAddress`로 헤더의 11개 함수를 가져온다. 동적 로드 시 DLL 디렉터리와 System32를 검색하도록 지정한다.
2. 기존 호스트의 Vulkan instance/physical device/device/queue와 proc-address 함수를 `LsfgCemuCreateInfo`로 전달한다. `struct_size = sizeof(...)`, `abi_version = LSFG_CEMU_ABI_VERSION`를 지정한다.
3. **`lossless_dll_path = nullptr` 또는 빈 문자열로 지정하면 내장 셰이더를 사용한다. 별도 원본 DLL은 필요 없다.** 비어 있지 않은 경로는 기존 외부 파일 경로를 사용하며 원본 SHA-256이 일치해야 한다.
4. `lsfg_cemu_create` → `lsfg_cemu_create_context` → `lsfg_cemu_set_enabled(..., VK_TRUE, ...)` 순서로 준비한다. 초기 상태는 OFF다.
5. 이전·현재 원본 프레임과 출력 이미지를 `generate` 또는 `generate_async`에 전달한다. 연속 호출에서 다음 `previous_frame_id`는 직전 `current_frame_id`와 같아야 한다. 이미지 번호가 아닌 콘텐츠 프레임 번호를 사용한다.
6. 종료·리셋·CPU readback 전에 `drain` 성공을 확인한다. `destroy_context` 후 `destroy`를 호출한다. 이미지 크기 변경은 drain 후 context를 다시 만든다.

Vulkan 1.2 이상과 실제 활성화한 timeline semaphore, shaderStorageImageExtendedFormats 기능이 필요하다. FP16 사용 시 shaderFloat16을 활성화하고 `fp16_shaders_enabled = VK_TRUE`로 전달한다. Windows Vulkan 드라이버/로더는 호스트 환경에서 제공한다.

입출력은 SDR `VK_FORMAT_R8G8B8A8_UNORM`, 동일 크기, 2D/single mip/layer/sample 이미지이며 호출 전후 layout은 GENERAL이다. 입력에는 TRANSFER_SRC, 출력에는 TRANSFER_DST usage가 필요하다. 출력은 입력이나 그 메모리와 겹치면 안 된다. 큐와 객체는 호스트 소유이며 모듈이 파괴하지 않는다. 호스트는 모듈 API와 같은 큐의 제출을 직렬화해야 한다.

`generate`의 OK는 GPU 완료를 뜻한다. `generate_async`의 OK는 제출 완료를 뜻하며, 같은 큐에 뒤이어 제출한 작업이 생성 결과를 사용한다. 다른 큐를 자동 동기화하지 않는다. 최초 실행·history reset의 3회 warmup, 이전 생성 작업 대기, 입력 준비 대기는 발생할 수 있으므로 항상 비차단인 API는 아니다. 대기 제한은 유한값으로 지정한다. 제출 후 timeout/오류가 발생하면 drain 성공 전까지 객체와 이미지를 유지해야 한다.

OFF의 generate는 `LSFG_CEMU_DISABLED`를 반환하며 GPU 작업·대기·history 변경을 하지 않는다. 호스트가 원본 프레임을 표시한다. 출력 배율은 2X이며 원본 한 쌍 사이의 중간 프레임 하나를 생성한다. 게임 시간 진행, 1X 속도 유지, 표시 시간과 프레임 폐기는 호스트가 담당한다.

`flow_scale`은 0.25..1.0이고 0은 1.0으로 해석한다. 화질 우선이면 1.0을 사용한다. FP32/FP16, flow 1.0과 0.75를 아래 검증에서 확인했다. 모든 배율과 모든 GPU를 검증한 것은 아니다.

## 검증 결과

- 테스트한 DLL SHA-256: `f8a406629cc7cf0071b21a1b1486e7dc7f84af83d78de9888acf9939eba811f3`.
- 원본 파일 SHA-256: `626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb`.
- 원본 SPIR-V 리소스 ID 303..400, 총 98개·2,058,104바이트가 DLL 내장 리소스와 바이트 단위로 일치한다. ARM64 PE 형식과 11개 export도 확인했다.
- CTest 3/3 PASS. DLL 경계 오류/수명 계약 80 checks, 정적 모듈 계약 80 checks, C 헤더 검사 통과.
- 실제 Qualcomm Adreno X1-85에서 8개 설정, 생성 프레임 32개를 전체 RGBA readback으로 비교했다. 보존한 외부 원본 셰이더 사용 ARM64 DLL과 모든 바이트가 일치했다(max byte error 0).
- FP32/FP16 각각 256×256·1280×720·1920×1080에서 flow 1.0, 1280×720에서 flow 0.75를 실행했다. 동기/비동기 생성, history reset과 OFF 동작을 포함한다. 후보 DLL에는 원본 파일 경로를 전달하지 않았다.
- 결과 비교 기준 DLL SHA-256: `0e9314e8899802781b75a8b914e4d2f05376ac1de1e3e1035e54871d506582cf`.

검증은 원본 셰이더 보존과 기존 compute 결과의 동일성에 대한 것이다. 상용 앱 전체와의 동일성, 실제 게임의 모든 장면 화질·속도, NPU 실행을 입증하지 않는다. legacy 경로에 기록된 A=A 입력 경계 5픽셀/최대 RGB 오차 8 문제를 이번 작업에서 수정했다고 주장하지 않는다. GPU 결과의 시간 값은 warmup을 포함한 측정 기록이며 성능 벤치마크 수치로 해석하지 않는다.

`ported/README.md`와 그 아래 문서는 이전 정적 모듈 배포 시점 기록을 포함한다. 현재 내장 셰이더 경로·ABI 2·검증 범위는 이 문서와 `include/lsfg_cemu.h`, `validation`을 기준으로 한다. 이전 정적 `.lib`는 이번 패키지에 포함하지 않았다.

## 재빌드

Native MSVC ARM64 개발자 셸, CMake 3.24+, Ninja, Python 3, Windows SDK가 필요하다. 패키지 루트에서 실행하며 원본 DLL은 위 SHA-256과 일치하는 사용자 보유 파일을 지정한다. 재빌드에만 원본 파일이 필요하고, 완성된 내장 DLL 실행에는 필요 없다.

```powershell
cmake -S source/native-lsfg -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release "-DLSFG_SHADER_SOURCE=C:/path/to/Lossless.dll"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python source/native-lsfg/tools/shader_resources.py C:/path/to/Lossless.dll --verify build/lsfg_arm64.dll
```

옵션을 생략하면 기존 외부 파일 전용 빌드가 되므로 내장 배포본 재빌드에는 `LSFG_SHADER_SOURCE`를 지정해야 한다. 셰이더 원본 해시와 내장 payload 해시를 검사하며 원본 x64 CPU 코드는 실행하지 않는다.

실기 비교 도구는 자동 CTest에 포함하지 않는다. 같은 물리 GPU의 기존 외부 셰이더 DLL을 보유한 경우 다음처럼 명시적으로 실행한다. Adreno 드라이버의 출력이 많으므로 로그를 파일로 저장한다.

```powershell
./build/lsfg_gpu_validate.exe ./build/lsfg_arm64.dll C:/path/to/reference-lsfg-arm64-v2.dll C:/path/to/Lossless.dll ./gpu-results > gpu-run.log 2>&1
```

소스의 기존 GPL-3.0-or-later 및 Vulkan 헤더 고지사항은 각 디렉터리에 보존했다. 내장 원본 셰이더와 Microsoft 실행 의존성에는 각각 원래 권리자의 조건이 적용된다.
