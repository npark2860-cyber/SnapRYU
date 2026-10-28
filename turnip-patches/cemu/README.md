# Turnip patches for Cemu

These patches build the Turnip driver we use with Cemu on Windows on ARM (Adreno X1-85). It differs from the SnapRYU driver in the parent folder: it uses only the first three SnapRYU patches, plus two fixes for crashes we only saw in Cemu.

## Base

Same as the parent folder: `https://github.com/strongtz/mesa`, commit `16f87fbcc8e1aced8b2778e4df639762974f9e34`.

## Apply

```
git checkout 16f87fbcc8e1aced8b2778e4df639762974f9e34
git apply ../0001-*.patch ../0002-*.patch ../0003-*.patch
git apply 0101-*.patch 0102-*.patch
```

0004 to 0007 from the parent folder are not used in the Cemu driver.

## Patches

| # | Affected component | Problem | Change |
|---|---|---|---|
| 0101 | ir3 (Mesa) | Cemu crashed while compiling some vertex shaders: `Unhandled ALU op: fmadz` (`ir3_context_error`) | Run `nir_opt_algebraic_late` until it makes no progress. One pass could fuse `fadd(fmulz)` into `fmadz`, and the pass that lowers `fmadz` for ir3 only ran on the next pass, which never came. `ir3_nir.c` already loops its own late pass the same way. |
| 0102 | tu (Mesa) | After 0101, Cemu hit `cnt > 0` in `freedreno_pm4.h` on the first draw after loading: `tu6_emit_scissor` emitted a pkt4 with 0 registers | Emit nothing when `scissor_count` is 0 (a pipeline with no viewport state, for example with rasterizer discard). |

## Build

Same meson options as the parent folder, but `--buildtype=debugoptimized` (assertions on), compiled with clang 22.1.3 for `aarch64-pc-windows-msvc`.

## Status

- With both patches, Breath of the Wild in Cemu got past shader compilation and the first draw to its menu.
- Known open issue: with this driver, Breath of the Wild hits the assertion in `tu6_plane_index` (`tu_image.cc`) in the menu. The caller is not identified yet, so there is no fix here.
- Tested only on the Adreno X1-85 (Surface Pro, Snapdragon X) with Windows on ARM.

## License

Same as the files they change: Mesa's licenses (mostly MIT). See `docs/license.rst` in Mesa.
