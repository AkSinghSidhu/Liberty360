# Mali Build (MediaTek Dimensity 9000 & ARM Mali GPUs)

> [!NOTE]
> **Tested & Working In-Game:** Verified working in-game on the **MediaTek Dimensity 9000 (Mali-G710 MC10)** (OnePlus Nord 3 5G, Android 16, stock vendor driver reporting Vulkan 1.1.177).

Target: MediaTek **Dimensity 9000** (Mali-G710 MC10) and other ARM Mali / Immortalis devices running Android 10 or newer.

## Why Mali Requires Special Handling

1. **Vendor Driver Vulkan Version:**
   Many MediaTek vendor drivers (such as those for the Dimensity 9000) only expose **Vulkan 1.1.177** and do not expose `VK_KHR_dynamic_rendering` (`vkCmdBeginRendering`).
2. **Missing Desktop GPU Features:**
   Mali vendor drivers lack `vertexPipelineStoresAndAtomics`, hardware geometry shaders (`geometryShader`), and non-solid fill modes (`fillModeNonSolid`).
3. **Texture Compression Formats:**
   Mali GPUs do not natively support desktop BC1/BC3/BC5 (DXT) texture compression formats, requiring CPU decompression to RGBA8 on asset load.
4. **Shader Clip Distance:**
   Mali drivers do not support `shaderClipDistance`, necessitating dynamic shader rewriting.

## Changes Included in this Fork

- **Vulkan 1.1 Bypass:** Relaxed the minimum Vulkan version check in `DriverBridge.java` and `driver_proxy.cpp` so the app initializes properly on stock Vulkan 1.1 vendor drivers.
- **Vulkan 1.1 RenderPass Pipeline:** Uses `--gpu_plugin=xenos` in `args.txt` to render using standard `VkRenderPass` and `VkFramebuffer` instead of requiring dynamic rendering.
- **ReXGlue Feature Relaxations:** Relaxed `vertexPipelineStoresAndAtomics` checks in `vulkan_device.cpp` and `command_processor.cpp` to non-fatal warnings, and disabled mandatory geometry shader / fillModeNonSolid assertions on Android.
- **BC Texture Decompression:** Decompresses DXT textures into RGBA8 in memory.
- **Shader Clip Distance Rewriting:** Rewrites clip-distance outputs in all game shaders.

## Building

```bash
# 1. Build native libraries
LIBERTY_GPU=mali bash os/android/scripts/build_native.sh

# 2. Package release APK
LIBERTY_GPU=mali bash os/android/scripts/build_apk.sh
```

## First Launch & Verification

1. Open the launcher. **System driver** should be selected by default.
2. Press **Play**.
3. For live logs:
   ```bash
   adb logcat -s LibertyRecomp LibertyDriver SDL DEBUG
   ```
