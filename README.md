<p align="center">
    <img src="docs/android/images/banner_android_v4.png" alt="Liberty Recompiled for Android" width="800"/>
</p>

# LibertyRecompAndroid — Mali Version (WIP)

> [!WARNING]
> **Experimental, work in progress.** This is a fork of
> [LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid) that gets the game running
> on **ARM Mali GPUs**, tested on the **MediaTek Dimensity 8400 (Mali-G720 MC7)** and
> **MediaTek Dimensity 9000 (Mali-G710 MC10)**. Expect bugs, crashes and rough edges. Use the original project if you have a
> Snapdragon (Adreno) device.

An Android port of [Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp), the static
recompilation of the Xbox 360 version of Grand Theft Auto IV. It runs the recompiled game natively
on arm64 Android devices. There is no emulator in between. This fork targets Mali GPUs through the
phone's own Vulkan driver; the original targets Adreno GPUs through a bundled Mesa Turnip driver.

## Mali version status

### Tested Devices:
- **MediaTek Dimensity 9000 (Mali-G710 MC10)**:
  - Tested on **OnePlus Nord 3 5G** (Android 16, vendor Mali driver `vulkan.mali.so` reporting Vulkan 1.1.177).
  - **In-Game Status:** Fully boots, loads save/intro, enters gameplay. 3D world, geometry, lighting, HUD, and on-screen controls render properly. Audio works (~95% fine via AAudio). Currently runs at **5–6 FPS** using the `--gpu_plugin=xenos` backend on the stock Vulkan 1.1 driver (optimization WIP).
- **MediaTek Dimensity 8400 (Mali-G720 MC7)**:
  - Tested on **Xiaomi** (Android 16, Mali driver r49p1).
  - **In-Game Status:** Boots and plays at **10–25 FPS**, depending on scene complexity.

### What works so far:
- The game installs, boots, loads saves, and runs in 3D gameplay: the world renders with textures, lighting, HUD, subtitles, and touch controls work.
- In-game audio plays properly (tested ~95% clean via low-latency AAudio).
- Compatible with vendor Mali drivers even when limited to **Vulkan 1.1** (via the Xenos GPU plugin).

### Known limitations:
- **Low Framerate on Vulkan 1.1 (Dimensity 9000):** 5–6 FPS on the Dimensity 9000 due to unoptimized primitive fallback paths and software translation in the Xenos backend on Vulkan 1.1 drivers. Performance tuning is actively in progress.
- **User clip planes are skipped:** Mali drivers lack `shaderClipDistance`, so some reflections or cut-off geometry may look slightly off.
- **Higher memory use and possible texture load stutter:** Compressed textures (DXT/BC) are decoded to RGBA8 on the CPU because Mali hardware lacks native BC compression support.
- **Vulkan Dynamic Rendering absent on stock drivers:** Stock vendor drivers for chips like the Dimensity 9000 only expose Vulkan 1.1.177 and lack `VK_KHR_dynamic_rendering`, preventing `gta4-native` from running without a renderpass translation layer.

### Changes made for Mali:

1. **Vulkan 1.1 Support & Driver Bypasses (Dimensity 9000):**
   - Stock MediaTek / Mali drivers on Android often report only Vulkan 1.1.177 and lack Vulkan 1.2.
   - Relaxed the hard Vulkan 1.2 check in `DriverBridge.java` and `driver_proxy.cpp` to allow devices on Vulkan 1.1 to initialize the driver proxy without throwing fatal initialization dialogs.
   - Added `--gpu_plugin=xenos` compatibility to render using standard Vulkan 1.1 `VkRenderPass` and `VkFramebuffer` when `VK_KHR_dynamic_rendering` is unavailable.

2. **ReXGlue Vulkan Hardware Feature Relaxations:**
   - In `vulkan_device.cpp` and `command_processor.cpp`, changed missing `vertexPipelineStoresAndAtomics` checks from fatal crashes (`return nullptr`) to non-fatal warnings.
   - Disabled mandatory assertions for `vulkan_require_geometry_shader` and `vulkan_require_fill_mode_non_solid` on Android, activating ReXGlue's fallback emulation paths so Mali drivers without geometry shader or non-solid fill modes do not abort startup.

3. **Shader Clip Distance Rewriting:**
   - Mali drivers do not offer `shaderClipDistance`, which GTA IV shaders rely on. Shaders are rewritten dynamically during load: clip-distance outputs are converted into regular unused outputs (all ~650 shaders pass Vulkan validation).

4. **BC (DXT) Texture CPU Decompression:**
   - The game's DXT1/3/5 textures cannot be sampled natively by Mali hardware. Shaders and textures missing hardware BC support are decompressed to RGBA8 on the CPU during asset loading.

5. **Android NDK C++ Fallbacks:**
   - Added fallbacks for missing libc++ features in Android NDK (`std::jthread`, `std::stop_token`, floating-point `std::from_chars`, and `std::atomic_ref`).

6. **Automated Host Toolchain & NDK Build Scripts:**
   - Fixed host tool detection in `build_native.sh`, `env.sh`, and `setup_host_tools.sh` to auto-compile and point to native Linux tools (`recomp-clang`, `direct_calls`, `extract-xiso`) without manual intervention.

Devices with an Adreno GPU take the original code paths; the fallbacks above only switch on when
the GPU lacks the feature.

Feedback and logs from other Mali / Dimensity devices are welcome in [Issues](../../issues). When
reporting, attach `Android/data/com.libertyrecomp/files/last_launch.txt` and, if the game starts,
a log from **Play (Debug Log)** (`files/debug_logs/`).

---

The rest of this README is from the original project and describes the Adreno build.

**DISCLAIMER!**

This is pure 100% AI slop. I didn't made this by hand at all, it's all thanks to Claude and I, vaduur, take ZERO credit or responsibility of this port being unstable. It will run shitty, that's a guarantee. All I did is spent couple of sleepless night testing and pointing out the issues. Oh, yeah, and spent some pocket change on Claude sub ofc. Please be aware of it and feel free to fork this repo or use it as a reference in your future ReXGlue to Android ports. Thank you and have a nice day.

BTW, Any feedback is very appreciated, [Issues](https://github.com/vaduur/LibertyRecompAndroid/issues) are open, submit a ticket if you have any.


> [!CAUTION]
> This is an experimental, unofficial port of a project that is itself in early development.
> Expect crashes and rough edges. It has been developed and tested on **one device**, a
> Retroid Pocket 5 (Snapdragon 865 / Adreno 650).

> [!NOTE]
> New features land in **[pre-releases](https://github.com/vaduur/LibertyRecompAndroid/releases)**
> first, while they are being tested. The release marked *Latest* is the stable one. Pre-releases
> are listed above it and marked *Pre-release*. Feedback on them is welcome in
> [Issues](https://github.com/vaduur/LibertyRecompAndroid/issues).

**This project does not include any game assets.** You need your own legally obtained copy of the
Xbox 360 game and its title update. Nothing here helps you get them.

## Status

On the Retroid Pocket 5 with the default settings:

- The game installs, boots and plays: the intro, the early story missions, free roam and driving
  have been played. Saves persist across launches.
- The game renders at the console's native 720p with FXAA. The display scales the image to the panel.
- Frame rate is capped at 30 FPS with VSync. Most places hold a steady 30. The heaviest far views
  (bridges, the skyline at night) drop to about 25–29 FPS.
- Shadows, draw distance and the shadow map size match the console. Traffic and pedestrian density
  are reduced to 40% of the PC defaults to save CPU time.
- Audio works, including the radio, cutscenes and Bink videos.

Known problems:

- Occasional crashes to the home screen still happen. Save often.
- The in-game installer's **Select File / Select Folder** buttons do not work on Android. Install
  from the launcher (0.5.7+) or through the app's `install` folder instead (see
  [INSTALL.md](docs/android/INSTALL.md)).
- One deferred light-volume shader is skipped, because it hangs the GPU under Turnip. A few
  light volumes are missing as a result.
- The episodes (The Lost and Damned, The Ballad of Gay Tony) and online multiplayer have not been
  tested on Android.
- The on-screen gamepad is new (0.5.7.2) and has only been tried on the Retroid Pocket 5 so far
  (see [Controls](#controls)).

## Device requirements

| | Requirement |
|---|---|
| **GPU** | **Qualcomm Adreno 6xx or 7xx** (Snapdragon). Developed and tested on the Adreno 650. **Adreno 8xx: experimental since 0.5.7.5**, see below. |
| **SoC** | Snapdragon 865 class or faster recommended (SD 865/870/888, 8 Gen 1/2/3, 8s Gen 3, …). |
| **CPU** | arm64 with ARMv8.2-A (the build uses LSE atomics). ARMv8.0 chips such as the Snapdragon 835 crash on start. |
| **Android** | Android 10 (API 29) or newer, 64-bit. |
| **RAM** | 8 GB recommended. 6 GB is untested. |
| **Storage** | About 7 GB for the installed game, plus room for the disc image while installing (about 15 GB free in total). |
| **Controller** | An XInput (Xbox-layout) gamepad is strongly recommended, see [Controls](#controls). |

**Not supported:**

- **ARM Mali / Immortalis GPUs** in the normal build. This covers most Exynos chips before the
  2200, MediaTek Dimensity/Helio and Google Tensor. An **untested** Mali build that uses the
  phone's own Vulkan driver is described in [MALI.md](docs/android/MALI.md) (aimed at the
  Dimensity 8400).
- **Samsung Xclipse GPUs** (Exynos 2200 and newer).
- **PowerVR GPUs** and anything else that is not an Adreno.

Why only Adreno: the renderer needs Vulkan 1.2 and was tuned on Turnip. Turnip is Mesa's open-source
Vulkan driver, and it exists only for Adreno GPUs. The stock Qualcomm driver on older devices
(including the Adreno 650) exposes only Vulkan 1.1. On other GPUs the bundled driver cannot load,
and nobody has run the renderer on their vendor drivers.

**Adreno 8xx (Snapdragon 8 Elite and newer): experimental, since 0.5.7.5.** The game has been
started on an Adreno 840 (Samsung, Android 17). The bundled Turnip build does not support Adreno
8xx, so import a Turnip build for it on the launcher (**Import driver (.zip)**), such as the Gen8
Turnip builds made for emulators. This is how it was tested. Expect rough edges, and please report
how it runs in [Issues](https://github.com/vaduur/LibertyRecompAndroid/issues). Low-end Adreno
6xx parts (610/618/619) should start, but expect them to be far too slow.

Since 0.5.6.2, when the bundled Turnip cannot drive the GPU, the app falls back to the device's own
Vulkan driver if that driver offers Vulkan 1.2. This is untested on any GPU. If no usable driver
exists, the app shows a dialog with the reason instead of closing silently, and it writes
`Android/data/com.libertyrecomp/files/last_launch.txt` with the device, GPU and driver details.
Attach that file when you report a problem.

## Controls

**An XInput (Xbox-layout) gamepad is strongly recommended.** For example:

- an Xbox controller over Bluetooth or USB;
- any XInput-compatible pad;
- the built-in controls of a gaming handheld (Retroid, AYN Odin, AYANEO and similar) in Xbox
  mode.

The game is the Xbox 360 version, so its controls and button prompts follow the Xbox layout one to
one. Other controllers that Android recognizes (DualShock, DualSense, Switch Pro and so on) usually
work through SDL. Their buttons are mapped by position, though, and they have seen much less
testing.

**On-screen gamepad (0.5.7.2 and newer).** Without a controller, the game shows a translucent
Xbox 360 controller on the screen. It disappears as soon as a gamepad is connected, and comes back
when it is disconnected.

![The on-screen gamepad](docs/android/images/touch_gamepad.png)

- **Left stick:** touch anywhere on the left half of the screen (outside the buttons). The stick
  centres where your thumb lands.
- **Right stick (camera):** touch anywhere on the right half of the screen. It works like a
  controller's right stick.
- **Buttons:** A/B/X/Y lower right, D-pad next to the left stick, LT/LB above the left stick, RT/RB
  on the right, L3/R3 next to the bumpers, Back/Start at the top. Several fingers work at once.
- **Menus** are driven with the same buttons, as on a controller.

Settings in `args.txt` (see [ADVANCED.md](docs/android/ADVANCED.md#other-useful-settings)):

- `--touch_controls=auto|on|off`. `auto` shows the overlay whenever no gamepad is connected, `on`
  always shows it, `off` never does.
- `--gta4_touch_layout=context` brings back upstream's context-sensitive touch layout instead of
  the gamepad.

## Game files required

- The **Xbox 360 GTA IV disc** as an `.iso` image. Only the **USA (NTSC-U)** release has been
  verified.
- **Title Update 8** for that release (version **0.0.8.5**), either as the STFS package or as a raw
  `default.xexp`. The PAL title update (0.0.8.6) is rejected.

The port was developed and tested with these exact files:

| File | Size | MD5 | SHA-1 |
|---|---|---|---|
| `Grand Theft Auto IV (USA) (En,Fr,De,Es,It).iso` (full disc image) | 7,835,492,352 bytes | `f0a046aed1520a913b2125f0a69ee7d0` | `caa48dfb3b1b2fe3131e61fc66e35a22c6d0ba13` |
| `TU_1A581VI_000000K000000.0000000000205` (Title Update 8, STFS package) | 3,715,072 bytes | `f041b5f6721d7ee578560bac21668826` | `88d1c438814298b6da987b3ad5edc7e48a769ac9` |

If your files match, they are known to work. Other dumps of the same USA disc (for example
trimmed or extracted images) may work too, but they have not been tested. Check your files with
`certutil -hashfile <file> MD5` on Windows, or `md5sum <file>` on Linux and macOS.

See the upstream [dumping guide](docs/DUMPING-en.md) for how to get these files from your own
console and disc.

## Quick start

1. Install `LibertyRecompAndroid-*.apk` from the [Releases](../../releases) page and open it. The
   app opens on a launcher with the game status, the installer, the Vulkan driver choice and
   **Play** (see [INSTALL.md](docs/android/INSTALL.md#2-open-the-app-the-launcher)).
2. In the **Game** panel (0.5.7 and newer), choose your disc image (`*.iso`) and the Title Update 8
   file wherever they are on the device. The app asks once for *All files access*. On older
   versions, or without that permission, copy both into
   `Android/data/com.libertyrecomp/files/install/` instead (a PC over USB is the easiest way).
   The tested files ([details](#game-files-required)):
   - disc: `Grand Theft Auto IV (USA) (En,Fr,De,Es,It).iso`, MD5 `f0a046aed1520a913b2125f0a69ee7d0`
   - TU8: `TU_1A581VI_000000K000000.0000000000205`, MD5 `f041b5f6721d7ee578560bac21668826`

   The file names do not matter, as long as the disc image ends in `.iso`.
3. Press **Install**, wait for it to finish, then press **Play**.

![Installing from the launcher](docs/android/images/launcher_install_v2.png)

The full walkthrough, including the `install/` folder and older versions, is in
[INSTALL.md](docs/android/INSTALL.md).

## Documentation

| Document | Contents |
|---|---|
| [Installation and first launch](docs/android/INSTALL.md) | Installing the APK, copying the game files, the first-run installer, upgrading |
| [Advanced configuration](docs/android/ADVANCED.md) | `args.txt` settings, live tuning, driver selection, logs, performance presets |
| [What was done for Android](docs/android/PORTING.md) | Technical write-up of the port: build, platform layer, driver, renderer and stability work |
| [Building from source](docs/android/BUILDING.md) | Building the APK yourself |
| [Mali build](docs/android/MALI.md) | The untested variant for Mali / Immortalis GPUs (Dimensity 8400) |
| [Upstream README](README_UPSTREAM.md) | The original Liberty Recompiled README (desktop builds, mods, online) |

## Credits

- [Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp) by OZORDI and contributors, and the
  [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) it is built on. Almost all of the hard work
  of recompiling GTA IV is theirs. This fork adds the Android platform layer and the Android
  performance work.
- [Mesa Turnip](https://docs.mesa3d.org/drivers/freedreno.html), the open-source Adreno Vulkan driver.
  The bundled build is StevenMXZ's Turnip 26.3.0-R6.
- [libadrenotools](https://github.com/bylaws/libadrenotools) by bylaws, which loads a custom driver
  into an app.
- The Vulkan driver proxy comes from [skate3-android](https://github.com/andrewnakas/skate3-android)
  and [skate3-pocket](https://github.com/AlanConstantino/skate3-pocket).
- [SDL3](https://github.com/libsdl-org/SDL) for the Android activity, input and audio.
- [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND, a ReXGlue port of Need for Speed:
  Most Wanted to the Nintendo Switch and a great reference for running a ReXGlue recompilation on
  weak hardware. Our direct-calls tool (`gta4-recomp/tools/direct_calls.py`) is adapted from their
  `llamadas_directas.py`. Their work on game busy-waits and thread placement led to this port's
  frame-wait hook and thread pinning experiments.

Grand Theft Auto IV is a trademark of Take-Two Interactive Software. This project is not affiliated
with or endorsed by Rockstar Games or Take-Two Interactive.
