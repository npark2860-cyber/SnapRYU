# SnapRYU Turnip patches

These patches turn strongtz's Turnip for Windows into the Turnip driver that ships with SnapRYU 1.0 (b110), `turnip\vulkan_freedreno.dll`.

We publish them so anyone can rebuild the driver, check what we changed, and reuse the fixes in Turnip for Windows on ARM or in Mesa.

## Base

- Repository: `https://github.com/strongtz/mesa`
- Commit: `16f87fbcc8e1aced8b2778e4df639762974f9e34` (Mesa upstream base `7a51c0f5326ee011b16a854499abf1db6f6c8b29`)

## Apply

```
git checkout 16f87fbcc8e1aced8b2778e4df639762974f9e34
git apply 0001-*.patch 0002-*.patch 0003-*.patch 0004-*.patch 0005-*.patch
git apply 0006-*.patch 0007-*.patch
```

0001 to 0005 are `git format-patch` files, so `git am` also works for them. 0006 and 0007 are plain unified diffs.

We checked that applying all seven to the base gives exactly the source files the shipped DLL was built from.

## Patches

| # | Affected component | Problem | Change |
|---|---|---|---|
| 0001 | ir3 (Mesa) | The shader compiler asserted in `ir3_ra.c` (`interval_insert`) on a compute shader in Hyrule Warriors: Age of Calamity | `ir3_merge_regs` treats a partial overlap with a killed source as interference |
| 0002 | tu WDDM (strongtz) | Large aligned buffer copies through a compute path caused device lost (Age of Calamity after about a minute, Breath of the Wild menu) | Drop the compute path; use the normal blit path |
| 0003 | tu WDDM (strongtz) | Far textures showed wrong colors (Age of Calamity) | Lay out the linear mip fallback in texels, matching the register value the kernel sets |
| 0004 | tu WDDM (strongtz) | With LSFG frame generation on, the GPU hung | Still write `TPL1_DBG_ECO_CNTL` when the kernel sets the magic registers |
| 0005 | tu render passes (Mesa) | A yellow ghost image (Breath of the Wild) when one image was bound to two attachments | Use sysmem rendering when two attachments alias the same subresource |
| 0006 | tu WDDM and Vulkan WSI (SnapRYU) | Each frame was copied once more before display | Present native tiled images on Windows. Also adds frame statistics and GPU timing hooks, which stay off in the shipped build |
| 0007 | ir3 (SnapRYU) | Some shaders spilled half-size registers to memory | Keep half-size spills in free full registers. Set `IR3_REGSPILL_AUDIT=0` to use the original path |

## Options used by SnapRYU

SnapRYU sets these environment variables for the driver: `TU_WSI_NATIVE_TILED=1`, `TU_WSI_LINEAR_SHARED=0`, `TU_WSI_NATIVE_MAX_LATENCY=2`.

## Build

The shipped DLL is a meson build for Windows ARM64 (`-Dplatforms=windows -Dvulkan-drivers=freedreno -Dfreedreno-kmds=wddm`) compiled with clang 22.1.3 at `-O3 -DNDEBUG`. The public package uses the static C runtime (`-fms-runtime-lib=static`). 0007's files are compiled with `-DSNAPRYU_REGSPILL_DIAGNOSTIC=0`.

## Tested

Only on the Adreno X1-85 (Surface Pro, Snapdragon X) with Windows on ARM.

## License

Same as the files they change: Mesa's licenses (mostly MIT). See `docs/license.rst` in Mesa.
