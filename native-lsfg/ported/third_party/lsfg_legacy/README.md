# Private legacy LSFG Windows experiment

Selected files originate from
`https://github.com/FrankBarretta/lsfg-vk-aarch64`, full commit
`617203ee5c65c3ec858778366f08814197415bd2`, GPL-3.0-or-later. The complete applicable
license is retained as `LICENSE.md`; per-file SPDX identifiers are preserved.
`provenance.json` lists each copied file's original and adapted SHA256.

Only the benchmark's selected common Vulkan helpers, error helpers, backend
extraction/registry, shaderchains, and context are compiled. No Linux layer,
configuration application, UI, or proprietary shader/binary is included.

Native adapter changes are reproducible in
`apps/vulkan_lsfg_bench/port_legacy.py`: system Vulkan loader; one host-owned device;
borrowed image/semaphore wrappers; no required external extensions; FP32 plus
queried core timeline and extended-storage-format features; bounded waits;
completion-safe cleanup; image/buffer destruction order; black-image clearing and
appropriate transition scope; empty wait-vector guard; instrumentation and
private transformed-shader dump. The existing registry IDs, shaderchains,
descriptor bindings, constants, samplers, and original generate-format patch are
preserved. No SPIR-V version rewrite is performed.

This is a standalone GPL benchmark dependency. It is not a replacement for, or
claim of compatibility with, the current official LSFG Vulkan shader DLL.
