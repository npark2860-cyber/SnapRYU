# SnapRYU

SnapRYU is a fork of the [Eden](https://git.eden-emu.dev/eden-emu/eden) Nintendo Switch emulator, built to run games with **NCE on Windows on ARM (ARM64) PCs**.
NCE runs the game's ARM64 instructions natively on the CPU, while the emulator still handles memory, system services and exceptions.

SnapRYU targets Windows on ARM64 PCs. It has been tested only on an Adreno X1-85 PC; other GPUs and drivers have not been verified yet.
With Go mode, you can also play on an Android phone or tablet.

| Group | Who | What you get |
|---|---|---|
| 1 | Other Windows on ARM PCs (not tested) | The emulator with the Standard backend |
| 2 | PC with the Adreno X1-85 | Group 1, plus the Adreno backend and the bundled Turnip driver |
| 3 | Android phone or tablet | Go mode: play a game running on a Group 1 or 2 PC |

Version 1.0 (b110). Tested on a Surface Pro (Snapdragon X, Adreno X1-85, Qualcomm driver 512.886).

> SnapRYU does not include games, keys or firmware. Dump them from your own Switch.

## What is different from Eden

- **NCE on Windows ARM64.** Compatible game instructions run natively on the ARM64 CPU instead of going through the Dynarmic recompiler. Dynarmic is still available as a per-game fallback.
- **Adreno Vulkan backend.** Eden has only the Standard Vulkan backend. SnapRYU adds **Vulkan Backend: Adreno (Experimental)**. It checks that the GPU is a Qualcomm Adreno running the Qualcomm or Turnip driver, picks a matching profile, and keeps a separate shader cache for each driver. On other GPUs it falls back to Standard.
- **Qualcomm fixes in the Vulkan renderer.** These include fixes for 8/16-bit storage buffer writes on Qualcomm, geometry shader layer passthrough, and a null texture buffer view that crashed some games.
- **Preserve submitted draw data (UB_KEEP).** This setting keeps constant buffer data that is still in use when the CPU rewrites it. Turn it on only for games that need it (Tears of the Kingdom).
- **Bundled Turnip driver.** An open-source Vulkan driver for Adreno, based on strongtz's Turnip for Windows (see Credits).
- **LSFG frame generation (optional).** A native ARM64 port of Lossless Scaling Frame Generation. You must supply your own `Lossless.dll`.
- **Go mode.** Play on your phone or tablet. The SnapRYU Go Android app receives video and sends input over your local network.

## Download

From the Releases page:

| File | What it is |
|---|---|
| `SnapRYU-1.0-b110-Windows-ARM64.zip` | The emulator |
| `SnapRYU-Go-1.0-b110-ARM64.apk` | Android app for Go mode |
| `SnapRYU-1.0-b110-source.zip` | Source code (GPL) |

## Install

1. Unzip anywhere. SnapRYU finds all of its files next to `SnapRYU-b110.exe`, so you can move the folder.
2. Run `SnapRYU-b110.exe`.
3. Add your keys and firmware as you would in Eden.

You don't need to install the Visual C++ runtime. Before packaging, a check script reads the imports of every exe and DLL in the package and stops if any of them needs it.

Settings, saves and keys go to `%APPDATA%\SnapRYU`.
For a portable setup, create a folder named `user` next to the exe, and SnapRYU will use it instead.

## Which setup is for you

SnapRYU runs only on Windows on ARM. On an x86/x64 PC, use [Eden](https://git.eden-emu.dev/eden-emu/eden) instead.

### Group 1: Windows on ARM PC without an Adreno X1-85

Use the graphics driver that came with your PC.

- **Vulkan Backend:** Standard. It works on any GPU.
- **GPU Fence Behavior:** Balanced
- **CPU:** NCE. If a game fails on NCE, switch that game to Dynarmic.
- **Preserve submitted draw data (UB_KEEP):** off globally. Turn it on only in the per-game settings for Tears of the Kingdom.
- **Frame generation (optional):** see LSFG below.

### Group 2: Snapdragon X PC with Adreno X1-85

SnapRYU was tested on this hardware. You get everything in Group 1, plus the Adreno backend and the bundled Turnip driver:

- **Vulkan Backend:** Adreno (Experimental)
- **Driver:** Turnip (bundled). The Qualcomm system driver also works.
- The other settings are the same as Group 1.

**Choosing the driver:** in the graphics settings, set **Vulkan Driver** to **Turnip (bundled)** or **System**, then restart the game.

**Checking which driver is active:** open `log\snapryu_log.txt` in the settings folder and look for `Vulkan driver: Turnip (...)` or `Vulkan driver: System`.

**If something goes wrong:** set **Vulkan Driver** back to **System**. If the `turnip\` folder is missing, SnapRYU logs an error and uses the system driver by itself.

The bundled Turnip is in the `turnip\` folder next to the exe. Keep `vulkan_freedreno.dll` and `turnip_icd.json` together.
It was built and tested only for the Adreno X1-85, so don't use it on other GPUs.

### Group 3: Android phone or tablet

Play on your phone or tablet with Go mode. The game runs on a Group 1 or Group 2 PC, and the SnapRYU Go app receives the video and sends your input. See Go mode below.

## Turnip driver history

The bundled Turnip driver (`turnip\vulkan_freedreno.dll`) was built in these steps:

1. **Mesa** (upstream commit `7a51c0f5`, 2026-10-06): the Turnip Vulkan driver for Adreno.
2. **strongtz/mesa** (commit `16f87fbc`): strongtz added a Qualcomm WDDM backend, so Turnip can run on Windows on ARM.
3. **SnapRYU patches**, each added after a game showed the bug:

| Patch | Affected component | What went wrong | What we changed |
|---|---|---|---|
| ir3fix | Mesa | The shader compiler crashed (`ir3_ra.c:143` assert) on one compute shader | Fixed register merging in `ir3_merge_regs` |
| nocc | strongtz | Large buffer copies went through a compute path and caused device lost | Removed that compute path; copies use the normal blit path |
| mipfix + tpl1fix | strongtz | On WDDM, a texture unit register was not set, so small mipmap levels broke and LSFG could hang | Made the driver's mipmap layout match the register value the hardware uses |
| aliasfix | Mesa (with Eden) | When one image was bound to two render targets, tile rendering wrote an old copy back (ghost images) | Render those passes directly to memory (SYSMEM) |
| native tiled present | SnapRYU | The extra copy of each frame before display cost GPU time | Present tiled images directly |
| regspill | SnapRYU | Some shaders spilled registers to memory | Keep half-size spills in full registers instead. Set `IR3_REGSPILL_AUDIT=0` to turn it off |

4. **Published patches:** every change above is in `turnip-patches/` in the source zip. Applying the seven patches to strongtz/mesa `16f87fbc` gives exactly the source the shipped driver was built from. Feel free to use them in your own Turnip builds.
5. **Public build:** the same source, rebuilt with the static C runtime (`/MT`) so you don't need the Visual C++ runtime.

Tested on the Adreno X1-85 only.

**Cemu branch (not bundled).** We also run this Turnip with our Windows on ARM build of [Cemu](https://github.com/cemu-project/Cemu) (Wii U). Cemu hit two more Turnip bugs, and we fixed them in a separate Cemu build:

- **fmadz:** a multiply-add where anything times 0 is 0. The shader compiler fused it in a late pass and then could not lower it, so it stopped.
- **Zero scissors:** a draw with no scissor rectangles tripped an assert.

SnapRYU has not hit either bug, so these two fixes are not in the SnapRYU driver.

## Where parts came from: Cemu

Some of SnapRYU started in our work on Cemu for Windows on ARM:

- **Go mode** is a port of CemuPad, our add-on that streams Cemu to a phone or tablet over Wi-Fi.
- **The LSFG DLL** was first written for Cemu. That is why its API is named `lsfg_cemu`.
- **Turnip** was tested with Cemu as well (see the Cemu branch above).

## Frame generation (LSFG)

LSFG is off unless you add `Lossless.dll` yourself.

1. Buy [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) on Steam.
2. Copy `Lossless.dll` from its install folder next to `SnapRYU-b110.exe`.
3. Turn on **Enable LSFG 2X (GPU)**.

SnapRYU does not include `Lossless.dll` or its shaders.

## Go mode (play on a phone or tablet)

1. Install `SnapRYU-Go-1.0-b110-ARM64.apk` on your Android phone or tablet.
2. Connect the PC and the phone or tablet to the same network.
3. Start Go mode in SnapRYU. It shows a one-time pairing QR code. Scan it with the app to pair.
4. After pairing, the app connects to the PC on TCP port 26762.

Requirements and security:

- Android 12 or newer.
- Pairing uses a one-time code that expires. Only a paired device can connect. 
- Video and audio use DTLS-SRTP.
- In the Windows firewall, allow SnapRYU on **private** networks only. Don't use Go mode on public Wi-Fi.

If the app can't connect, check that both devices are on the same network, check the firewall rule, and pair again.

## Tested games

These results come from one Surface Pro (Snapdragon X, Adreno X1-85, Qualcomm driver 512.886).
A successful launch or test session does not guarantee full compatibility or stability.

| Game | Settings | Result |
|---|---|---|
| The Legend of Zelda: Tears of the Kingdom (1.4.2) | NCE, UB_KEEP on, LSFG on, Turnip | Long run: 2 h 12 min, no crash, no errors in the log |
| Fire Emblem Warriors: Three Hopes | NCE | Short run: the crash fixed in this build no longer happens. Play time not recorded |
| Xenoblade Chronicles 3 | NCE, UB_KEEP off | Short run on an earlier 1.0 build: about 5 min at the 30 FPS cap |
| Xenoblade Chronicles 3 | Dynarmic, UB_KEEP off, Qualcomm driver | 18 min on an earlier 1.0 build, no hangs |
| Bayonetta 2 | UB_KEEP off | Short run on an earlier 1.0 build: no hangs. Play time not recorded |

## NCE vs Dynarmic

One side-by-side measurement so far: Xenoblade Chronicles 3, same save point, earlier 1.0 build, UB_KEEP off, Qualcomm driver, frame generation on.

| CPU backend | Game FPS | Frame gaps over 100 ms |
|---|---|---|
| Dynarmic | About 27 (25.5 to 29.2 per 10 s window) | None after loading |
| NCE | 30.0 in every 10 s window (the game's cap) | None after loading |

NCE reached the game's 30 FPS cap; Dynarmic stayed below it. We have no comparison yet for other games or for games running at 60 FPS.

## Known issues

- Xenoblade Chronicles 3 hangs with UB_KEEP on. Keep it off for this game.
- Bayonetta (the first game): the opening movie stutters.
- Grendizer: The Feast of the Wolves crashes at start on NCE. Use Dynarmic for this game.
- Double Dragon Revive shows a black screen after an online error.
- With LSFG on, some 60 FPS games show visual errors.

## Building from source

`SnapRYU-1.0-b110-source.zip` contains:

- `snapryu/`: the emulator (Eden based, CMake, clang for ARM64)
- `native-lsfg/`: the LSFG DLL
- `go-host/`: the Go mode host DLL
- `turnip-patches/`: the seven Turnip patches on top of strongtz/mesa `16f87fbc`, with a README that explains each one and how to apply them

The source for the Android app is not published.

## Credits

- **[Eden](https://git.eden-emu.dev/eden-emu/eden)** and **yuzu**: the emulator SnapRYU is built on.
- **[strongtz](https://github.com/strongtz/mesa-turnip-windows)**: Turnip for Windows, the WDDM backend that makes Mesa's Turnip driver run on Windows on ARM. SnapRYU's bundled driver is built from strongtz's tree with extra fixes found while testing games, such as small mipmap levels, register spilling, and native tiled presentation. Thank you, strongtz. Without your work, SnapRYU would have no open driver on Windows.
- **[Mesa](https://mesa3d.org/)**: the Turnip Vulkan driver and the ir3 shader compiler.
- **[lsfg-vk-aarch64](https://github.com/FrankBarretta/lsfg-vk-aarch64)** (from [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk)): the LSFG frame generation code that SnapRYU's native port uses.
- **Lossless Scaling** by THS: the frame generation shaders, which you load from your own copy.
- **[Cemu](https://github.com/cemu-project/Cemu)**: Go mode is ported from our CemuPad add-on for Cemu. The host code keeps Cemu's MPL-2.0 license (`HOST-LICENSES\Cemu-LICENSE.txt`).

SnapRYU is not affiliated with Nintendo, the Eden team, strongtz, Mesa or Lossless Scaling.

## License

| Component | License | Source |
|---|---|---|
| SnapRYU emulator (Eden fork) | GPL-3.0-or-later | `snapryu/` in the source zip |
| LSFG DLL | GPL-3.0-or-later. Code taken from lsfg-vk-aarch64 commit `617203ee5c65c3ec858778366f08814197415bd2`, which is GPL-3.0-or-later; each file keeps its SPDX line | `native-lsfg/` in the source zip, with `provenance.json` |
| Go mode host | Code from CemuPad, which is based on Cemu: MPL-2.0 (`HOST-LICENSES\Cemu-LICENSE.txt`). Libraries: OpenSSL, Opus, libSRTP, ZXing, nlohmann-json (`HOST-LICENSES\`) | `go-host/` in the source zip |
| Turnip driver (Mesa) | Mostly MIT. Some Mesa files use other licenses; see `turnip\MESA-LICENSE.rst` | `turnip-patches/` in the source zip |
| Android app | SnapRYU's own code. Source not published | - |

Other components are under the licenses in `LICENSES\`.
