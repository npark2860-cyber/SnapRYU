# Vulkan C headers used by the native diagnostic tools

This project copies the generated C headers (`thirdparty/vulkan/*.h` and `thirdparty/vk_video/*.h`) verbatim from the reviewed `lsfg-vk` source commit `2a5dc88c9bf787efa2e6e4af46286aaaf99f9dbd`. The bundle is Vulkan-Headers **1.4.350** and contains per-file Khronos copyright and `SPDX-License-Identifier: Apache-2.0` notices. The license terms are at the [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0). See `docs/source-lock.json` for the source commit and per-file SHA-256 inventory.

The copy is limited to C headers used by the Windows diagnostic tools; C++ Vulkan-Hpp headers and binaries are not copied. The application dynamically loads the installed Vulkan loader. It does not link or ship a Vulkan loader/driver. Keep these generated headers unchanged; update the full SHA-256 inventory when replacing the source version.
