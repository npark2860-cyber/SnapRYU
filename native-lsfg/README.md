# native-lsfg

Source for `lsfg_arm64.dll`, the native ARM64 frame-generation DLL that SnapRYU loads for its optional LSFG feature.

## What this is

- A Windows ARM64 DLL with a small C ABI (`LSFG_CEMU_ABI_VERSION 2`), declared in `ported/include/lsfg_cemu.h`.
- It exports 11 `lsfg_cemu_*` functions (see `exports.def`).
- SnapRYU loads it by path from the folder next to the emulator exe (`lsfg_arm64.dll`).

## Shaders are not included

This source and the public DLL contain **no Lossless Scaling shaders**.
LSFG works only if you own Lossless Scaling and copy your own `Lossless.dll` next to the SnapRYU exe.
SnapRYU passes that file's path in `lossless_dll_path`; if the file is missing, LSFG stays off.

Do not redistribute `Lossless.dll` or anything extracted from it.

## Building (public build)

Requirements: Visual Studio 2022 with ARM64 tools, CMake, Ninja.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build
```

- `/MT` (`MultiThreaded`) links the VC++ runtime statically, so no `msvcp140.dll` or `vcruntime140.dll` is needed.
- Leave `LSFG_SHADER_SOURCE` empty. Setting it embeds shaders from a local `Lossless.dll`; that is for private testing only, and such a build must never be shared.
- The finished DLL imports only `KERNEL32.dll` and `bcrypt.dll`.

## Tests

```
copy /y build\lsfg_arm64.dll build\lsfg_arm64_v2.dll
ctest --test-dir build --output-on-failure
```

`exports.def` names the module `lsfg_arm64_v2`, so the test exe imports that name. The copy is for tests only; the package ships `lsfg_arm64.dll`.

`lsfg_gpu_validate` is a separate GPU check and does not run during CTest.

## License and origin

GPL-3.0-or-later (`ported/LICENSE.md`).
The frame-generation code is ported from [lsfg-vk-aarch64](https://github.com/FrankBarretta/lsfg-vk-aarch64), commit `617203ee5c65c3ec858778366f08814197415bd2`, which is based on [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk).
Details are in `ported/third_party/lsfg_legacy/README.md` and `provenance.json`.

Lossless Scaling is a separate commercial product and is not covered by this license.
