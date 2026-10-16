# Liberty360 — Android Mali Edition

> [!NOTE]
> **Active Development.** This is an Android port of [Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp) (Grand Theft Auto IV static recompilation for Xbox 360) focused exclusively on **ARM Mali GPUs**, specifically targeting the **MediaTek Dimensity 9000 (Mali-G710 MC10)** using native Android Vulkan vendor drivers.

The recompiled game runs natively on arm64 Android devices without an emulator. This fork addresses Mali hardware and vendor driver limitations through dedicated Vulkan compatibility layers, shader rewrites, and hardware feature fallbacks.

---

## Status (MediaTek Dimensity 9000)

- **Target Device:** OnePlus Nord 3 5G (MediaTek Dimensity 9000, Mali-G710 MC10)
- **OS:** Android 14 / Android 16 (API 36)
- **Vulkan Driver:** Stock vendor driver (`vulkan.mali.so` reporting Vulkan 1.1.177)
- **In-Game Status:** **Fully Working In-Game**
  - Installs, boots, and loads save files / intro smoothly.
  - 3D world geometry, lighting, shadows, textures, HUD, subtitles, and touch controls render correctly with the default **`gta4-native`** backend.
  - Audio runs cleanly (~95% working via low-latency AAudio stereo 48 kHz).
  - Current framerate: **~14–15 FPS** in 3D gameplay (measured in Hove Beach via the game's frame counter / `scripts/fps.sh`); optimization is in active progress. The legacy `--gpu_plugin=xenos` backend remains selectable for A/B testing.

---

## Changes Made for Mali

1. **Vulkan 1.1 Compatibility & Driver Bypass:**
   - Stock vendor drivers on MediaTek Dimensity 9000 devices report Vulkan 1.1 (`1.1.177`) and lack `VK_KHR_dynamic_rendering` (`vkCmdBeginRendering`).
   - Bypassed the hard Vulkan 1.2 requirement in `DriverBridge.java` and `driver_proxy.cpp`, allowing devices running Vulkan 1.1 to initialize the driver proxy without fatal exit dialogs.
   - Configured `--gpu_plugin=xenos` compatibility to utilize standard Vulkan 1.1 `VkRenderPass` and `VkFramebuffer` render passes.

2. **ReXGlue Vulkan Device Relaxations:**
   - Relaxed `vertexPipelineStoresAndAtomics` checks in `vulkan_device.cpp` and `command_processor.cpp` so that missing hardware atomic support emits a non-fatal warning instead of aborting device creation.
   - Disabled mandatory assertions for `geometryShader` and `fillModeNonSolid` on Android, enabling solid-fill fallback paths for Mali hardware without hardware geometry shaders.

3. **Shader Clip Distance Rewriting:**
   - Mali drivers do not support `shaderClipDistance`. All clip-distance shader outputs are dynamically transformed during loading into standard unused outputs, allowing all game shaders to pass Vulkan validation cleanly.

4. **BC (DXT) Texture CPU Decompression:**
   - Mali GPUs lack hardware support for desktop BC1/BC3/BC5 (DXT) texture formats. Game textures are dynamically decoded to RGBA8 on the CPU during asset streaming.

5. **Android NDK C++ Fallbacks:**
   - Added polyfills for missing Android NDK libc++ features (`std::jthread`, `std::stop_token`, floating-point `std::from_chars`, and `std::atomic_ref`).

6. **CLI11 CVar Deduplication & Custom Launcher Settings:**
   - Enabled `take_last` option policy in `cvar.cpp` and deduplication in `LibertyActivity.java`, allowing custom graphics settings in the launcher (resolution scaling, FPS limit, shadow quality, traffic density) to seamlessly override base arguments without parse errors.

7. **Automated Host Toolchain & NDK Build Scripts:**
   - Automated detection and building of required host tools (`recomp-clang`, `direct_calls`, `extract-xiso`) in `build_native.sh`, `env.sh`, and `setup_host_tools.sh`.

8. **Native (`gta4-native`) black-world fix — Mali per-format MSAA capability mismatch (2026-10-16):**
   - GTA IV's scene render targets are 2× MSAA. Mali's *device-wide* limits advertise 2×, but the *per-format* query (`vkGetPhysicalDeviceImageFormatProperties`) returns `sampleCounts=0x1D` (1×/4×/8×/16× — **no 2×**) for the scene tuples, so the strict capability check rejected the scene targets before `vkCreateImage` ever ran — the 3D world drew black while HUD/2D stayed visible.
   - Fixed by settling **one coherent, driver-advertised sample count per attachment family** before image creation: `QueryImageFormatSampleCounts()` reuses the same driver query as validation, a per-surface clamp degrades 4→2→1 where a tuple lacks the requested count, and the scene MSAA override is family-validated across color + depth. Strict validation stays intact — no bypasses.
   - Applies to every path (scene targets, reflections, explicit depth handoff). Verified on-device: zero surface rejects, world renders. Full evidence in `docs/android/MALI_FINDINGS.md` (F11/F14).

9. **Native surface diagnostics & recovery tooling:**
   - `CreateDedicatedAllocationImage` logs each step/result (`support-query` → `bind-memory`) with exact `VkResult` names and driver limits, and capability rejections now name the failing predicate (`predicate=sample-count … sampleCounts=…`).
   - Surfaces gained the same one-pass reclamation/retry recovery that textures already had (safety net under real memory pressure).
   - `fps.sh` now measures via the game's per-frame counter (SurfaceFlinger's latency buffer is empty for BLAST layers on modern Android).

---

## Device Requirements

| Component | Requirement |
|---|---|
| **GPU** | **ARM Mali-G710** or compatible Mali / Immortalis GPU |
| **SoC** | MediaTek Dimensity 9000 or equivalent |
| **CPU** | arm64 with ARMv8.2-A support (uses LSE atomics) |
| **Android** | Android 10 (API 29) or newer, 64-bit |
| **RAM** | 8 GB or more recommended |
| **Storage** | ~7 GB for installed game files, plus ~8 GB for the installation ISO |
| **Controller** | Built-in on-screen touch controls, or an XInput (Xbox layout) Bluetooth / USB gamepad |

---

## Controls

### On-Screen Gamepad
A translucent on-screen controller overlay is enabled by default:
- **Left Stick:** Touch anywhere on the left half of the screen.
- **Right Stick (Camera):** Touch and drag anywhere on the right half of the screen.
- **Face Buttons:** A, B, X, Y on the lower right.
- **Shoulders / Triggers:** LB, LT on the left; RB, RT on the right.
- **D-Pad & Navigation:** Directional pad on the left, Back and Start at the top.

### Physical Controllers
Any standard Android-supported gamepad (Xbox, DualShock, DualSense, Switch Pro) can be used over Bluetooth or USB.

---

## Game Files Required

This project does not contain any game assets. You must provide your own legally obtained copies:
- **GTA IV Xbox 360 Disc Image:** USA (NTSC-U) release as a `.iso` file.
- **Title Update 8:** Version `0.0.8.5` (`TU_1A581VI_000000K000000.0000000000205` or extracted `default.xexp`).

---

## Quick Start

1. Install the built APK onto your Android device:
   ```bash
   adb install -r app-release.apk
   ```
2. Place your game `.iso` and Title Update 8 into `Android/data/com.libertyrecomp/files/install/` or select them via the launcher.
3. Open the launcher, verify **System driver** is selected, and press **Install**.
4. Once installation completes, press **Play**.

---

## Building from Source

### Prerequisites
- Linux host (Ubuntu 22.04+ or similar)
- Android SDK & NDK (r27 or newer)
- CMake 3.22+, Ninja, Clang/LLVM, Python 3

### Build Native Libraries & APK
```bash
# 1. Build native libraries with Mali target
LIBERTY_GPU=mali ./os/android/scripts/build_native.sh

# 2. Package release APK
LIBERTY_GPU=mali ./os/android/scripts/build_apk.sh
```

The resulting APK will be located at `os/android/app/build/outputs/apk/release/app-release.apk`.

---

## Credits & Acknowledgements

- [LibertyRecompAndroidMaliVer](https://github.com/ZackyJade123/LibertyRecompAndroidMaliVer) by [ZackyJade123](https://github.com/ZackyJade123) (prollynotzjay) for the original Android Mali port groundwork.
- [Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp) by OZORDI and contributors.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).
- [SDL3](https://github.com/libsdl-org/SDL) for cross-platform Android activity, windowing, and audio.
- [skate3-android](https://github.com/andrewnakas/skate3-android) & [skate3-pocket](https://github.com/AlanConstantino/skate3-pocket) for the Vulkan driver proxy architecture.
- [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND for direct-calls tooling and busy-wait optimizations.

---

*Grand Theft Auto IV is a trademark of Take-Two Interactive Software. This project is not affiliated with or endorsed by Rockstar Games or Take-Two Interactive.*
