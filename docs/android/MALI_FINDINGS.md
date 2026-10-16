# Liberty360 Mali — Findings: Black 3D World on Dimensity 9000 (2026-10-16)

**Status (updated after fifth session, 2026-10-16 late evening — ROOT-CAUSED, FIXED, VERIFIED ON DEVICE):** the scene targets were rejected **before `vkCreateImage`** by a **sample-count capability mismatch**, not by allocation failure: the host MSAA override selected **2×** from **device-wide** limit masks (which advertise 2×), while Mali's **per-format** `vkGetPhysicalDeviceImageFormatProperties` for the exact tuples (RGBA8 `usage=0x17`, D24S8 `usage=0x27`, OPTIMAL, 1206x540) returns `sampleCounts=0x1D` — **1×/4×/8×/16×, no 2×** — so the strict predicate `(properties.sampleCounts & create_info.samples) != 0` failed every request. The logged `result=unknown(-13)` was the util's own local-limit marker, not a driver error. Fix (per-format-coherent sample-count selection, F14) built and verified at **18:12**: `surface-sample-clamp requested=2 effective=1 tuple-sampleCounts=1D`, **0 surface rejects, 0 allocation failures**, world renders (§8 fifth evidence block). Measured ≈**15.5 fps** in Hove Beach via the game's frame counter (`fps.sh`).
**Companion docs:** `MALI_FIX_PLAN.md` (overall plan), `PERFORMANCE_DIAGNOSTICS.md` (older perf work), `MALI.md` (older bring-up notes).

---

## 1. Summary

| Symptom | Verified state |
|---|---|
| 3D world completely black in gameplay | **Reproduced on-device.** Only HUD (minimap incl. route line, on-screen gamepad, subtitles) renders. |
| "5–6 FPS" performance | **Measured after the fix: ≈14–15.5 fps present rate** (game frame counter / `fps.sh`, Hove Beach) while `title-fps: 29.5` was logged under a `--gta4_frame_limit=30` — the limiter-vs-present gap needs a perf pass. The old 5–6 FPS complaint referred to older xenos builds per `PERFORMANCE_DIAGNOSTICS.md`. |
| Per-frame error spam | **RESOLVED (18:12 run: zero `surface reject`, zero `image allocation failed`, zero `support-check rejected`, handoff `result=ok`).** Previously ≈93–175 rejects/s retrying four 1206x540 scene surfaces (F10) plus ≈4,100 downstream `explicit-depth-handoff … rejected reason=surface-create` — all mechanism-corrected in F11/F14: the rejects were **capability rejections before `vkCreateImage`**, not failed allocations. |
| Vulkan 1.1 emulation proxy | **No warnings during the faulty session** (`LibertyDriver` tag silent) → proxy render-pass/framebuffer emulation is *not* the active failure; the fault is inside `gta4-native`'s surface manager. |

**Root cause (final, verified):** the four 1206x540 scene targets (1 depth + 3 colors) never got host images because the host requested **2× MSAA** for these guest-k2X deferred attachments from **device-wide** limit masks, while the driver's **per-format** query advertises `sampleCounts=0x1D` (no 2×) for their tuples; `IsImageCreateInfoSupported` therefore rejected each request **before creation** — the explicit depth-handoff had no source (F1), every command binding those targets failed (`failure-target / failure-state`), and the world stayed black while HUD content (smaller, creatable surfaces) rendered. Creation was never the gate: the F-D5 probes always created the tuple successfully (drivers are lenient about MSAA tuples the query does not advertise). The fix (F14) settles **one coherent, driver-advertised sample count per attachment family** (2→1 degradation where the tuple has no 2×) before the cache lookup, so the scene path, the depth handoff and any guest-derived request all resolve to the same images.

---

## 2. Session environment (what the findings were collected on)

- **Device:** OnePlus Nord 3 5G (`CPH2491`), Android 16, SoC **MT6983 = MediaTek Dimensity 9000**, GPU Mali-G710 MC10, stock vendor Vulkan driver (SDK-reported earlier as 1.1.177).
- **App:** `com.libertyrecomp`, installed APK `Liberty360-v0.5.8-mali-alpha1.6.apk` (note: internal `versionName` still reports `0.5.8-mali-alpha-1.3` — cosmetic versioning bug, see F6).
- **Driver selection:** System driver (default; `driver.txt: system`).
- **Launch args** (on-device `args.txt`, created from bundled defaults):
  `--gpu_plugin=gta4-native`, `--gta4_output_height_cap=540` (render 1206x540), `--gta4_native_anti_aliasing=fxaa`, `--gta4_frame_limit=30`, `--gta4_present_mode=vsync`, `--gta4_thread_pinning=3`, `--gta4_native_max_queued_frames=3`, `--audio_maxqframes=64`, `--log_level=warn` (plain Play).
- **Game files:** ISO `Grand Theft Auto IV (USA) …` (7.8 GB) + `TU_1A581VI_…0205` pushed to `Android/data/com.libertyrecomp/files/install/`; on-device install extracted to `…/files/LibertyRecomp/game/` (default.xex, common.rpf, audio, update, …). ~14 GB used, 8.6 GB free.

---

## 3. Reproduction (verified steps)

1. Launch app → **Play** (or **Play (Debug Log)** for info-level logs).
2. New game → intro/prologue. World viewport is **fully black**; HUD minimap, control overlay, subtitles render.
3. `adb logcat` (tag `LibertyRecomp`, warn level is enough) shows continuous:
   ```
   E LibertyRecomp: [error] [core] gta4-native-cause: point=explicit-depth-handoff frame=4889 cmd=567
     result=rejected reason=surface-create
     source-wrapper=DB569D60 source=40226600/00010000
     destination-wrapper=400007C0 destination=400007C0/00010000
     source-texture=D9132320@43 source-host=0x0:s0 destination-host=1206x540:s1
   ```
   (Fields: `handle/address`; `source-host=0x0:s0` ⇒ the resolved **source surface is null**; `destination-host=1206x540:s1` ⇒ destination exists, 1206x540, 1 sample.)

---

## 4. Findings

### F1 — Per-frame explicit-depth-handoff rejection (downstream symptom of F10)
- Frequency: 2 log lines per frame, frames ~4860–4889 observed in a 3-second window ⇒ continuous since at least frame ~4300; ~29.5 fps submitted.
- The rejection is emitted at **`glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp:26077`** (reject lambda) and returned at **`:26107`**:
  ```cpp
  NativeSurfaceImage* source = GetOrCreateSurfaceImage(handoff.source, true);       // :26092  → null
  NativeTextureImage* source_texture = … native_texture_images_.find(generation);   // :26093+
  NativeSurfaceImage* destination = GetOrCreateSurfaceImage(handoff.destination, true); // :26103 → OK
  …
  if (!source || !source_texture || !destination) {
    return reject("surface-create", source, destination);                            // :26107
  }
  ```
- The **destination** (title-owned persistent surface `400007C0`, 1206x540, D32S8, 1x) resolves fine; the **source** (handle varies per process — `40226600` in session 1, `40226690` in session 3; same guest address `0x00010000`, same 1206x540 D32S8 shape) does not. In session 3 the source re-creation attempt itself is what logs `surface reject … reason=image-allocation` (F10).

### F2 — Guest address aliasing storm at `0x00010000`
`gta4-native-cause: point=surface-address-alias-create` (logged at `graphics_system.cpp:20581`, REXLOG_ERROR) fired for a newly created depth surface whose guest address collides with many live, differently-shaped depth surfaces:
```
handle=402182E0 address=00010000 depth=true guest=256x256 host=256x256 format=130 (D32S8) samples=1 written-alias=true
aliases=[
  400007C0:guest=1206x540:host=1206x540:s1:layout=3:written=true,   ← handoff DESTINATION
  402157E0:guest=1280x1280:host=1280x1280:s1:layout=3,
  40238400:guest=320x180:s4, 402385A0:guest=256x256:s4,
  40238620:guest=128x128:s1, 40226600:guest=1206x540:s1:layout=3:written=true,  ← handoff SOURCE
  40226870:guest=603x270:s1:written=false, 4022DD10:guest=64x64:s1 ]
```
Also one color case: `handle=4022E060 address=00000001 depth=false guest=64x64 format=37 …` aliasing 1206x540 color surfaces.
Interpretation: the guest allocator legally reuses one guest address range for many logical surfaces (EDRAM-style sharing). The renderer gives each descriptor its own dedicated host `VkImage` (no host-side sharing), which is fine — but the bookkeeping around these aliases is where the handoff source resolution breaks (F3/F4).

### F3 — Where the source resolution can fail (code map)
`Gta4NativeGraphicsSystem::GetOrCreateSurfaceImage` (`graphics_system.cpp:~20317`):
1. Cache hit requires: same handle bucket, not in `pending_surface_release_ids_`, equal aspect/width/height/samples, **exact `SurfaceDescriptorsEqual`**, same reflection status (`:~20420`).
2. On full miss with `allow_allocation=true` it allocates a fresh dedicated image (`D32S8`, 1206x540, s1, DEPTH+TRANSFER+SAMPLED usage) or fails via one of the reject paths: `missing-handle-or-dimensions`, `unsupported-format`, `unsupported-samples`, `dimension-limit`, `native-msaa-sample-capabilities`, `reflection-sample-capabilities`, `image-allocation`, `image-view`, `sampled-depth-view` (`:~20330–20570`).
3. Reject reasons are only logged **for the first 32 occurrences and every 1024th** per counter (`NextNativeTraceDiagnosticCount`, `:1538`), **and only when the `native-trace` diagnostics category is enabled** (F8) — which is why the reason for the null source was invisible until the third session enabled that category (F7/F10).
4. Silent-`nullptr` path exists only for `allow_allocation=false` (`:~20444`); the handoff call uses the default `true`, so the failure must have gone through a **reject path** (or a cached image was skipped as *pending release* and re-creation also failed).

### F4 — Proxy (Vulkan 1.1 emulation) is quiet — not the current blocker
- Zero `LibertyDriver` messages during the faulty run: no `Color/Depth attachment uses unknown imageView`, no `Failed to create emulated VkRenderPass/VkFramebuffer`.
- Draw census (below) proves render passes and pipelines work; the depth handoff is an **engine-internal copy/transfer command** (see `DepthSurfaceHandoffCommand` handling at `:26373+` — stencil scratch path, MRT/scene sample promotion hints), not a proxy feature.

### F5 — World draws ARE issued; HUD draws are a small minority (draw census)
```
gta4-draw-census: commands=488 draws=446
  c1206x540/d1206x540/p0=154   ← main scene color+depth
  c256x256/d256x256/p0=274     ← (shadow/reflection/glyph class targets)
  c1206x540/d0x0/p0=1, c10x10…, c1x1…, c40x40…, c301x135…, c5x5…, c75x33…  ← HUD/small targets
```
(`graphics_system.cpp:6101` emitter.) So geometry submission and the main pass are alive; the failure is in the handoff step that the engine expects to feed subsequent passes.

### F6 — Environment/tooling quirks observed (minor)
- Launcher `versionLine()` reads install `versionName` (`0.5.8-mali-alpha-1.3`) — every alpha APK reports the same string; APK file names are the only reliable identifier. (Also `versionCode=15` unchanged across alphas.)
- Launcher label for System driver still says *“needs Vulkan 1.2”* although the Mali 1.1 bypass makes it the recommended default — misleading but harmless.
- `fps.sh` layer grep is fixed in the working tree (uncommitted) — needed for ground-truth FPS measurement.
- `shadow-trace: heartbeat value=0 frame=4888` appears in gameplay; not yet interpreted (possibly normal when shadows resolve into the main target).

### F7 — RESOLVED: the reject reason is `image-allocation` (mechanism later corrected — F14)
The third session (with `--diagnostics_categories=logging,native-trace`, the F8 recipe) shows **every** reject is `reason=image-allocation`: `GetOrCreateSurfaceImage` attempts a fresh creation and `CreateDedicatedAllocationImage` returns false (F11). The **label was misleading**: the `false` came from the capability check *before* `vkCreateImage` (F14). Descriptor-mismatch bookkeeping and pending-release skips are **not** the active cause.

### F8 — Reject reasons are invisible by default (diagnostics-gating trap)
The second session (fresh process, plain relaunch) reproduced the per-frame handoff rejection but logged **no** `surface reject #` lines at all. Cause found in the diagnostics gating:

```cpp
// graphics_system.cpp:1538
uint64_t NextNativeTraceDiagnosticCount(std::atomic<uint64_t>& counter) {
  if (!rex::diagnostics::IsEnabled(Category::kNativeTrace)) return UINT64_MAX;   // ← gate
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}
// graphics_system.cpp:20319 (surface reject lambda)
const uint64_t count = NextNativeTraceDiagnosticCount(rejection_count);
if (count <= 32 || !(count % 1024)) REXLOG_WARN("gta4-native-diag: surface reject #{} reason={} …");
return nullptr;   // the reject itself always happens; only the LOG is gated
```
- With the `native-trace` category off, `count = UINT64_MAX` ⇒ `count <= 32` false and `UINT64_MAX % 1024 == 1023` false ⇒ **no log**, yet `reject()` still returns `nullptr`. The failure is invisible unless diagnostics are enabled.
- The app enables only `--diagnostics=true --diagnostics_categories=logging` by default (`LibertyActivity.java:267-269`), so all reject reasons are suppressed in normal runs — including the first 32.
- Diagnostics categories are **configure-once per process** (`src/core/diagnostics_policy.cpp`: `g_configured` refuses reconfiguration) — no live-cvar flip; a relaunch with args is required.
- "Play (Debug Log)" **does not help**: `LibertyActivity.java:261-266` *removes* any `--diagnostics*` and `--log_level*` args from `args.txt` before adding its own defaults, so custom categories never survive that button. Use plain **Play** with the args written into `args.txt` (recipe in §9).

### F9 — Real driver capability snapshot (second session, `LibertyDriver: Driver initialized`)
```
{"name":"Mali-G710 MC10","api_version":4198577,"driver_version":134221824,"vendor_id":5045,
 "features":{"shaderInt64":0,"depthClamp":1,"shaderClipDistance":0,"sampleRateShading":1,
             "independentBlend":1,"runtimeDescriptorArray":1,"descriptorBindingPartiallyBound":1,
             "bufferDeviceAddress":1,"dynamicRendering":1}}
```
- `api_version 4198577` = Vulkan **1.1.177**, `vendor_id 5045` = ARM.
- **`depthClamp: 1`** — the vendor driver **does** support depth clamp (the proxy does not fake it). This **refutes** the plan's H2 assumption ("Mali stock drivers normally report `depthClamp=false`"): the `depth-clamp-unsupported` reject path is not active on this device. Depth-clamp emulation is only needed for other drivers (keep as a portability path).
- `shaderClipDistance: 0` — clip-distance stripping remains required (already implemented).
- `dynamicRendering: 1` — **faked by the proxy** (real device is Vulkan 1.1); all dynamic-rendering→render-pass emulation paths are active and produced zero warnings during the failure.
- The second session also reproduced the handoff rejection (`source-host=0x0:s0`); the referenced source-texture generation had advanced across sessions (@43 → @396), evidence the source texture is re-created over the run.

### F10 — Four scene surfaces rejected every frame (decisive capture; **mechanism corrected in F11/F14**)
**Correction (fifth session):** the `reason=image-allocation` rejects did **not** fail inside `vkCreateImage` or memory allocation — the gate was `IsImageCreateInfoSupported`'s **local sample-count predicate** (`sampleCounts=0x1D` has no 2×, while the host asked for 2×; F11/F14). The probe "created the exact tuple fine" because creation was never the gate; driver leniency at create is not evidence of supported MSAA. The history below remains accurate as *observed symptom history*.

Rate-limited reject log (first 32 + every 1024th, F8) shows a steady **~175 rejects/sec** storm — always the same three **1206x540** surfaces:
```
surface reject #2048 reason=image-allocation depth=false handle=40226720 flags=00100004 base=00810050 address=00000001 packed=12D410D8 format=405275014 size=1206x540 samples=1
surface reject #3072 reason=image-allocation depth=true  handle=40226690 flags=00100004 base=00810050 address=00010000 packed=12D410D8 format=438436247 size=1206x540 samples=1
surface reject #4096 reason=image-allocation depth=false handle=402267B0 flags=00100004 base=00810050 address=00000002 packed=12D410D8 format=405275014 size=1206x540 samples=1
```
- Color raw `405275014` = `kD3dFmtA8R8G8B8` (0x18280186) → **`VK_FORMAT_R8G8B8A8_UNORM`**; depth raw `438436247` = `kD3dFmtD24FS8` (0x1A220197) → **`VK_FORMAT_D32_SFLOAT_S8_UINT`** (constants `graphics_system.cpp:1839-1844`). Ordinary, driver-supported formats — **not** an unsupported-format rejection.
- The depth handle (`address=00010000`) is the handoff *source* identity → the F1 handoff rejection is **downstream of this allocation failure**; every source re-creation attempt fails the same way.
- Storm progression: #2048 @ 17:00:39.934 → #3072 @ 17:00:45.769 (≈175/s) → **#28672** by capture end (17:03:28); never recovers. Frames 1142–5772.
- Handoff totals over the same window: 8,205 log lines, **all rejected, 0 ok** (each rejection prints ≈2 lines incl. split, so ≈4,100 occurrences).
- Companion failures identify the missing targets: `gta4-modern-exec: backend=vulkan event=failure-target command=410646 slot=1 handle=402267B0 extent=1206x540 format=18280186` (hex-formatted guest format of the same color target) and the `event=failure-state … bindings-ready=false environment-valid=0` flood (355 lines).
- The three handles are **absent from the live-surface census**: frame 1200 reports `pipelines=281 textures=582 surfaces=35` — the scene targets have no host image at all.
- The identical create-info **does** succeed elsewhere: the handoff destination `400007C0` (same 1206x540 D32S8 shape, same guest address) has a host image ⇒ the failure is **state-dependent (time/load)**, not a descriptor or format property — consistent with cumulative allocation pressure and/or a driver allocation limit.

**Session 4 update (first-32 captured; host copy `/tmp/opencode/fresh_capture.log`):**
- The failing set is **four** 1206x540 surfaces: depth `40226600` (address `0x10000`) and colors `40226690`/`40226720`/`402267F0` (addresses `0x1`/`0x2`/`0x3` — a triple-buffered output chain + its depth). Reject cycle = depth×2 + `…690`×2 + `…720` + `…7F0` = 6 rejects/frame, **~93/s steady** (`#24576` by 17:33:03), identical descriptor bits for all four (`flags=00100004 base=00810050 packed=12D410D8`, `samples=1`).
- **Reject #1 fires with the very first world frame** (frame ~797, `17:29:00.702`). Loading screens render fine into 1206x540 targets (`draw-census 845 draws @ 17:28:32`), so the failure begins exactly at the loading→world transition — not after gradual pressure.
- **Each of the four allocated successfully once** at world start: the success-only `surface-address-alias-create` log fired for all four between `17:29:00.690` and `17:29:00.715`. Every later request misses the cache (failed images are never cached, F12) and re-runs the full create → fails forever. The same-spec sibling `40226760` (color 1206x540 f37 s1, address `0x1`) created 13 ms after `40226690` and **never rejected** (0 rejects all run) — the failure is state-dependent *and* stuck.
- Retries continue ≈93×/s for 4+ minutes with **zero** successes, while smaller surfaces (64x64 … 1280x1280 — shadows, reflections, glyphs) create fine throughout.
- Process memory during the storm: `GL mtrack ≈556 MB`; native heap ≈800 MB with **441 MB swapped**; system `MemFree 168 MB` / `MemAvailable 1.17 GB` of 7.5 GB — genuine sustained system-level pressure. `gralloc4` also failed 4 buffer allocations at process start (17:28:25).
- The proxy `vkCreateImage` (`driver_proxy.cpp:1063`) is a **clean pass-through** (samples bookkeeping only; the strip logic at `:1301-1329` touches pipeline pNext chains and MSAA state, not images) → not the culprit.
- In-game screenshots show the black world with HUD only (Hove Beach, `$25`) — consistent with the final-composite targets missing while small-surface HUD content renders.

### F11 — RESOLVED: the failing step was the support-query sample-count predicate (not the driver query, not allocation)
`ui::vulkan::util::CreateDedicatedAllocationImage` (`glue/rexglue-sdk-main/src/ui/vulkan/vulkan_util.cpp:116`) returned `false` silently at one of these five points; instrumentation (F-D5) showed step 1, and the fifth session isolated the exact comparison:
1. `IsImageCreateInfoSupported` (`:161`) — `vkGetPhysicalDeviceImageFormatProperties` fails, or extent/mip/array/sample limits fail.
2. `vkCreateImage` fails.
3. `ChooseMemoryType` → `UINT32_MAX` (no memory type for the purpose).
4. `vkAllocateMemory` fails (dedicated allocation attached when `KHR_dedicated_allocation` — every image/texture gets its own `VkDeviceMemory`).
5. `vkBindImageMemory` fails.

The driver proxy intercepts **none** of the memory/format queries (no `vkAllocateMemory` / `vkBindImageMemory` / `vkGetPhysicalDeviceImageFormatProperties` / `vkGetImageMemoryRequirements` wrappers in `os/android/native/driver_proxy.cpp`) — the failure is genuine driver-side or util-side. Distinguishing matters: byte-exhaustion (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) and object-count cap (`VK_ERROR_TOO_MANY_OBJECTS` / `maxMemoryAllocationCount`) need different fixes.

**Session 4 — step identified; session 5 — predicate identified.** Every failure logged `step=support-query result=unknown(-13)`. That value is ambiguous *by construction*: `IsImageCreateInfoSupported` reports `VK_ERROR_UNKNOWN` both when the driver's format-properties query fails and when the query succeeds but a reported limit (extent/mips/layers/sample counts) is exceeded (now documented in `util.h`). The differential: a fixed-runtime build carrying a query-failure deferral produced **zero deferral lines** and byte-identical rejects → the query always returned `VK_SUCCESS`; the local comparison chain failed. All predicates except the sample-count one hold trivially here (1206x540 vs `maxExtent`, mips=1, layers=1), and `samples=2` is not in the driver's per-format `sampleCounts=0x1D` (1×/4×/8×/16×) → **the scene targets were rejected at the capability check because the host asked for 2× MSAA the per-format query does not advertise**. The F-D5 probes always "succeeded" because they ran `vkCreateImage`/allocation directly, which Mali accepts for such tuples — creation leniency was never the gate. A keep-in-build diagnostic now logs `vulkan-util: support-check rejected #… predicate=sample-count … sampleCounts=1D …` with driver values, so this cannot be misread again.

### F12 — Surfaces have no allocation-failure recovery; textures do (recovery asymmetry)
- **Textures** (`GetOrCreateTextureImage`, `graphics_system.cpp:17816+`) retry on allocation failure: `ShouldRetryNativeTextureAllocation` → `TrimTextureAllocationPool(true)` → `CompleteSecondaryNativeFrameSlot()` → `EvictNativeTextureImages()` → `ReleaseRetiredTextureImages(true)` → `texture_reclaimer_->Drain()` → retry; plus pooled reuse (`texture_allocation_pool_`). Textures keep streaming all session (bc-decode log continues past 17:01).
- **Surfaces** (`GetOrCreateSurfaceImage`): cache-hit **or** allocate-once → `reject("image-allocation")` → `nullptr`. `ReleasePendingSurfaceImages` (`:20286`) only frees surfaces whose GPU submission already completed — nothing triggers it as pressure relief.
- Net effect: in an allocation-constrained state, textures self-heal while the scene surfaces fail **forever** → black world until process restart.

**Session 4 update — F-R2 implemented in this build.** `GetOrCreateSurfaceImage` now retries once after a bounded reclamation pass identical to the texture path (`TrimTextureAllocationPool(true)` → `CompleteSecondaryNativeFrameSlot()` → `EvictNativeTextureImages(active_texture_frame_, true)` → `ReleaseRetiredTextureImages(true)` → `texture_reclaimer_->Drain()` → `ReleasePendingSurfaceImages()`), logs `point=surface-allocation-recovery handle=… attempt=… slot-completed=… retired-released=…` (first 16 + every 512th), and is guarded against reentrancy with at most one pass per texture frame. If the failure is pressure-based the retry recovers in place; if it is tuple/alignment-based the F-D5 line + probes decide the next fix.

**Fifth-session note:** the `image-allocation` rejects this path was built to recover from turned out to be **capability rejections** (F14) — the clamp removes the trigger entirely; F-R2 stays as a safety net for genuine allocation failures.

### F13 — Textures are CPU-decoded BC→RGBA8 (context for the ASTC discussion, §10)
- Mali exposes no `textureCompressionBC`; `graphics_system.cpp:18163` decodes every BC1/2/3 mip/slice to RGBA8 (`native_bc_decode.h`) and uploads `VK_FORMAT_R8G8B8A8_UNORM` — 4 B/texel vs ~0.5–1 B/texel in the RPF.
- Observed volume: `bc-decode: 108 textures, 12.2 MB, 13.6 ms total, 6 worker threads` → live texture footprint at 582 images ≈ tens–low-hundreds of MB (not GB-scale). A bandwidth/memory optimization opportunity (ASTC), **not** by itself an explanation of the scene-surface failure.

### F14 — ROOT CAUSE + FIX (verified on device, 18:12 run): per-format sample-count coherence
**Mechanism.** GTA IV's scene targets are guest k2X MSAA (deferred attachments). The host scene MSAA override selected **2×** from **device-wide** `VkPhysicalDeviceLimits` masks (`framebuffer*SampleCounts ∩ sampledImage*SampleCounts`, which advertise 2×), then `IsImageCreateInfoSupported` validated against the **per-format** query (`sampleCounts=0x1D`, no 2×) → every scene-surface request was rejected before `vkCreateImage`, every frame, forever, and nothing was ever cached (F12).

**Fix (all in this build):**
1. `ui::vulkan::util::QueryImageFormatSampleCounts()` (`util.h`, `vulkan_util.cpp`) exposes the driver's per-format sample mask — the same query validation runs, so selection and validation cannot drift.
2. **Per-surface clamp** in `GetOrCreateSurfaceImage` (before the cache lookup): the effective count settles on the per-format answer (4→2→1 degradation; `reject("image-format-samples-unsupported")` only if even 1× is missing). Applies to every path — scene override, guest-derived counts, reflections, and the depth handoff — so they all resolve to the same cached image. Logs `gta4-native-diag: surface-sample-clamp … requested=2 effective=1 tuple-sampleCounts=1D`.
3. **Family-validated scene override**: the override is intersected with the per-format masks of every color + depth tuple of the target (reflection surfaces excluded — they manage their own family), so the whole attachment family settles on one count instead of each surface deciding independently.
4. The session-4 query-deferral experiment was **reverted** (no evidence the query ever failed; strict validation restored), and the strict-reject path now logs the failing predicate + driver values (F11).
`NativeSurfaceImageUsage()` shares the exact usage bits between creation and the queries.

**Verified (18:12, `/tmp/opencode/capture_clamp.log`; screenshot `/tmp/opencode/clamp_1.png`: world fully rendered — Hove Beach, shadows, storefronts, NPCs, minimap):**
```
10-16 18:12:24.143 W … gta4-native-diag: surface-sample-clamp handle=40226600 depth=true format=130 extent=1206x540 requested=2 effective=1 tuple-sampleCounts=1D
10-16 18:12:24.143 W … gta4-native-diag: surface-sample-clamp handle=40226690 depth=false format=37 extent=1206x540 requested=2 effective=1 tuple-sampleCounts=1D
10-16 18:12:24.144 W … gta4-native-diag: surface-sample-clamp handle=40226720 … format=37 … requested=2 effective=1 tuple-sampleCounts=1D
10-16 18:12:24.144 W … gta4-native-diag: surface-sample-clamp handle=402267F0 … format=37 … requested=2 effective=1 tuple-sampleCounts=1D
```
- `surface reject`: **0** (was ~93–175/s); `image allocation failed`: **0**; `support-check rejected`: **0**; `deferring to vkCreateImage`: **0**; handoff rejects: **0** (`result=ok` lines present). Frame counter: `frame=3995 → 4409` over 29 s = **14.3 fps**; `fps.sh` = 15.5 fps. Residual `failure-target` diagnostics ≈320–500 over the session (was 1500–1700), mostly null slots — pre-existing, non-blocking follow-up.

---

## 5. Ranked hypotheses (with confirmation)

| # | Hypothesis | Confidence | Confirmed by |
|---|---|---|---|
| H1 | ~~Alias bookkeeping loses the handoff source.~~ **Refuted as the active cause**: re-creation is attempted on every request and is rejected at the capability check (`sampleCounts`, F14) — no descriptor bookkeeping error involved. | Confirmed | F7, F10, F14 |
| H2 | ~~Allocation failure under memory pressure.~~ **REFUTED (fifth session):** the rejects were **capability rejections before any allocation** (F11/F14); the same tuples create and allocate cleanly under identical pressure (F-D5 probes), and the live census was never the issue. | Refuted | F11, F14 |
| H2b | ~~Which step of `CreateDedicatedAllocationImage` fails.~~ **ANSWERED:** the `support-query` step — specifically its local `sampleCounts` predicate (driver query returns `VK_SUCCESS` with `sampleCounts=0x1D`; host asked 2×). Not `vkCreateImage` (probe-verified lenient), not memory. | Resolved | F11, F14 |
| H2c | ~~Tuple/alignment rejection.~~ **REFUTED:** the tuple was never submitted to `vkCreateImage` by the failing path; the probe's plain create of the exact tuple succeeds. | Refuted | F-D5 probe, F14 |
| H3 | `source_texture` (generation 43) lookup fails *in addition* (host=0x0 for texture too) → handoff refers to a texture that was never loaded/promoted on this path. | Medium | Debug log lines for texture create for handle D9132320 |
| H4 | The world-black symptom is *also* affected by the proxy's stripped depth-clip-control (fake `VK_EXT_depth_clip_control`, real struct deletion) making 3D clip/depth wrong — secondary to F1; must be fixed for correct rendering once the handoff works. (**Depth *clamp* is genuinely supported on this device — F9 — so only clip-control/clip-space paths remain suspect.**) | Medium (secondary) | Compare with H1 fixed; validate no z-clipping after fix |
| H5 | Performance claim (5–6 FPS) is stale for alpha1.6; real issue was rendering. **Measured now that the world renders: ≈14–15.5 fps** present rate in Hove Beach (game frame counter / `fps.sh`), while `title-fps` previously logged 29.5 under a 30 limiter — reconcile in the perf pass. | Open (perf) | Fifth-session `fps.sh` |

---

## 6. Proposed fixes

### Immediate (unblocks diagnosis — small, safe)
1. **F-D1 — Surface-reject reason at the handoff site.** Add context to the reject path: give `GetOrCreateSurfaceImage` an optional out-param (`const char** reason` / enum) and log it in `RecordDepthSurfaceHandoff`’s reject lambda. Keeps the existing first-32 policy for the generic diag, but the handoff reject then always states *why* the source nulled (e.g. `unsupported-samples`, `image-allocation`). **Also log the first few rejects *unconditionally* (not gated by `kNativeTrace`) — see F8: a rejected command changes rendering behavior, so its reason must be visible by default.**
2. **F-D2 — Log severity/levelling.** Demote `surface-address-alias-create` from ERROR to WARN/INFO (expected guest address reuse); keep the handoff reject rate-limited (first N + every 1024th) but make the first N **unconditional** (F8).
3. **F-D3 — Versioned identity.** Bump `versionSuffix` per alpha build (build.gradle.kts) so on-device `versionName` identifies the APK.
4. **F-D4 — Don’t strip caller-supplied diagnostics args in Debug Log mode.** `LibertyActivity.java:261-263` removes all `--diagnostics*` args when Debug Log is requested, so custom categories can never be captured from that button (cost us a capture cycle — F8). Preserve explicit args, or add a `native-trace` launcher toggle.
5. **F-D5 — Instrument the silent allocation failure + driver limits — IMPLEMENTED (this build).** `CreateDedicatedAllocationImage` now logs `step=support-query|create-image|choose-memory-type|allocate-memory|bind-memory` with numeric + named `VkResult` (rate-limited 32 + every 1024th); `IsImageCreateInfoSupported` gained the optional `VkResult*` out-param; the first failure dumps `maxMemoryAllocationCount`, `maxImageDimension2D`, heaps and memory types once; capped failure probes distinguish transient / aligned-extent / linear-tiling / minimal-usage / plain-allocation causes. The proxy `CapabilityJson` edit was dropped — the util-side limits dump delivers the same facts with less churn.

### Root-cause fix candidates (choose after §9 data — F-D5 first)
6. **F-R2 — Surface allocation recovery + pressure relief (PRIMARY FIX) — IMPLEMENTED (this build).** On `CreateDedicatedAllocationImage` failure inside `GetOrCreateSurfaceImage`: one reclamation pass (`TrimTextureAllocationPool(true)` → `CompleteSecondaryNativeFrameSlot()` → `EvictNativeTextureImages(active_texture_frame_, true)` → `ReleaseRetiredTextureImages(true)` → `texture_reclaimer_->Drain()` → `ReleasePendingSurfaceImages()`) then a single retry; at most one pass per texture frame, reentrancy-guarded, logged as `point=surface-allocation-recovery handle=… attempt=… slot-completed=… retired-released=…`. Follow-ups depending on F-D5 data: pooled surface allocations keyed by (format,extent,samples,usage) like `texture_allocation_pool_`; shared host image per (address,format,extent,samples) family for already-discarded aliases (`written=false`).
7. **F-R4 — Match the fix to the failing step — RESOLVED (implemented as F14).** The failing step was `IsImageCreateInfoSupported`'s **local sample-count predicate** (the driver query returned success with `sampleCounts=0x1D`; the host asked 2×). Implemented: per-format `QueryImageFormatSampleCounts()` + per-surface clamp + family-validated scene override (F14). The memory-pressure and object-count sub-scenarios did not apply (`maxMemoryAllocationCount=4294967295`; probe allocations of the same size always succeeded; creation always worked when reached).
8. **F-R1 — Alias-aware handoff resolution (downgraded — not the active cause; keep for robustness).** When resolving an explicitly referenced persistent surface (handoff source/destination), do not refuse re-creation for a handle whose cached entries are pending release; prefer **re-creating** the surface with the descriptor captured in the handoff command. If a descriptor-equality miss is caused by a stale virtual-resource registration (e.g. address registered as 256x256 while the handoff expects 1206x540), fix the registration/priority so the handoff’s descriptor wins for that handle. Add a pinned-lifetime guard: mark handoff source/destination `MarkNativeSurfaceImageUsed` at *record* time and exclude them from retirement until the command has executed.
9. **F-R3 — Texture-generation fallback (if H3).** If `depth_handoff_source` generation lookup misses, fall back to the texture bound by the current draw state for that handle (log when used) instead of rejecting.

### Secondary (from the fix plan — still required, not the current blocker)
10. **Proxy honesty + clip emulation (plan Phase 1.2/1.3; scope reduced by F9).** Faking `VK_EXT_depth_clip_control` (`driver_proxy.cpp:1119-1213`) while deleting the struct (`:1301-1329`) stays suspect for true 3D correctness — make it honest and implement the z-remap `(z+w)*0.5` SPIR-V variant in the engine’s shader-patch pipeline. The depth-**clamp** variant is *portability-only* (target device reports `depthClamp=1`, F9); the `depth-clamp-unsupported` reject is not firing here. Re-check the clip-space rejects at `graphics_system.cpp:21251-21258` after the handoff fix.
11. **Proxy hardening (plan Phase 1.4)** — now lower priority since no proxy errors were observed; keep for validation-layer pass later.
12. **Performance measurement (plan Phase 2.1)** — after the world renders: `fps.sh` + presenter diagnostics + `gta4_native_frame_timeline`; current `title-fps 29.5` suggests the frame limiter (30) is the observable cap.

### Verification protocol after each fix
- `adb logcat` clean of `gta4-native-cause` under 5 min of gameplay; handoff `result=ok` (there is an existing `result=ok` log at `graphics_system.cpp:26673`).
- Screenshot shows the world; enter/exit interior; drive (shadow + reflection passes active).
- `fps.sh` median before/after.

---

## 7. Key code references

| Area | Location |
|---|---|
| Depth handoff processing | `glue/rexglue-sdk-main/src/graphics/gta4_native/graphics_system.cpp:26020` (`EnsureDepthHandoffStencilScratch`), `:26071` (`RecordDepthSurfaceHandoff`), `:26092/:26103/:26107` (source/destination/reject), `:26373+` (handoff execution) |
| Surface creation/rejects | `:20317` (`GetOrCreateSurfaceImage`), `:~20420` (cache-hit rules), `:~20444` (silent null path), `:20581` (alias-create log) |
| First-32 diag policy | `:1538` (`NextNativeTraceDiagnosticCount`) |
| Draw census | `:6101` |
| Native clip control (secondary fix) | `native_clip_control.h:20-38`; reject sites `graphics_system.cpp:21251-21258`; pipeline clip struct `native_pipeline_recipe.h:268-272` |
| Proxy fake/strip (secondary fix) | `os/android/native/driver_proxy.cpp:1119-1177` (ext spoof), `:1179-1213` (feature forcing), `:1301-1329` (struct strip) |
| Silent allocation failure (instrument next) | `glue/rexglue-sdk-main/src/ui/vulkan/vulkan_util.cpp:116` (`CreateDedicatedAllocationImage`), `:161` (`IsImageCreateInfoSupported`) |
| Texture recovery loop (parity template) | `graphics_system.cpp:17816+`; helpers `graphics_system.h:1677` `EvictNativeTextureImages`, `:1679` `ReleaseRetiredTextureImages`, `:1804` `CompleteSecondaryNativeFrameSlot`, `:2506` `texture_reclaimer_`, `:2509` `TrimTextureAllocationPool` |
| Surface release (GPU-retirement only) | `graphics_system.cpp:20286` (`ReleasePendingSurfaceImages`), `:20266` (`QueueSurfaceImageRelease`) |
| Scene-target failure evidence | `gta4-modern-exec event=failure-target` (target `402267B0`, `format=18280186`), `event=failure-state` |
| BC→RGBA8 decode | `graphics_system.cpp:18163+`, `native_bc_decode.h` |
| Proxy capability JSON | `os/android/native/driver_proxy.cpp:543` (`CapabilityJson`) |

---

## 8. Evidence excerpts (raw)

**Fifth session — fix verified (18:12–18:16; host copy `/tmp/opencode/capture_clamp.log`; screenshot `/tmp/opencode/clamp_1.png`):**

```
10-16 18:12:24.143 W LibertyRecomp: [warning] [core] gta4-native-diag: surface-sample-clamp handle=40226600 depth=true format=130 extent=1206x540 requested=2 effective=1 tuple-sampleCounts=1D
10-16 18:12:24.143 W … gta4-native-diag: surface-sample-clamp handle=40226690 depth=false format=37 extent=1206x540 requested=2 effective=1 tuple-sampleCounts=1D
10-16 18:12:24.144 W … gta4-native-diag: surface-sample-clamp handle=40226720/402267F0 … format=37 … requested=2 effective=1 tuple-sampleCounts=1D
10-16 18:14:xx W … point=surface-address-alias-create handle=40226600 address=00010000 depth=true guest=1206x540 host=1206x540 format=130 samples=1   ← scene depth creates at 1×, no rejection
10-16 18:15:55 … point=command-frame-boundary boundary=present frame=3995 → 18:16:24 frame=4409 = 14.3 fps
run totals (18:12–18:17, frames 1→5365): surface reject = 0 · image allocation failed = 0 · support-check rejected = 0 · deferring to vkCreateImage = 0 · handoff rejects = 0 (result=ok present) · sample clamps = 40 (all handled)
```

**Fourth session — first-32 rejects + world-start transition (17:27–17:33; host copy `/tmp/opencode/fresh_capture.log`; 16 MB buffer, logcat streamed from before launch):**

```
10-16 17:29:00.690 E LibertyRecomp: [error] [core] gta4-native-cause: point=surface-address-alias-create handle=40226600 address=00010000 depth=true guest=1206x540 host=1206x540 format=130 samples=1 written-alias=true aliases=[400007C0:guest=1206x540:…,40238670:guest=256x256:…]
10-16 17:29:00.702 W LibertyRecomp: [warning] [core] gta4-native-diag: surface reject #1 reason=image-allocation depth=true handle=40226600 flags=00100004 base=00810050 address=00010000 packed=12D410D8 format=438436247 size=1206x540 samples=1
10-16 17:29:00.702 W … surface reject #2 … depth=false handle=40226690 … address=00000001 … format=405275014 size=1206x540 samples=1
10-16 17:29:00.702 W … surface reject #4 … handle=40226720 … address=00000002
10-16 17:29:00.702 W … surface reject #5 … handle=402267F0 … address=00000003
10-16 17:29:00.715 E … point=surface-address-alias-create handle=40226760 address=00000001 depth=false guest=1206x540 host=1206x540 format=37 samples=1 …   ← same spec as 40226690; created OK; never rejected
10-16 17:33:03.750 W … surface reject #24576 reason=image-allocation depth=true handle=40226600 …   (≈93 rejects/s; no success after world start)
meminfo during the storm: Graphics 555988 kB; native heap 364580 kB Pss + 441463 kB swap; /proc/meminfo: MemFree 168716 kB, MemAvailable 1177680 kB
```

**Third session — decisive capture (17:00:39–17:03:28, frames 1142–5772; host copy `/tmp/opencode/capture_nativetrace.log`):**

```
10-16 17:00:39.934 W LibertyRecomp: [warning] [core] gta4-native-diag: surface reject #2048 reason=image-allocation depth=false handle=40226720 flags=00100004 base=00810050 address=00000001 packed=12D410D8 format=405275014 size=1206x540 samples=1
10-16 17:00:45.769 W LibertyRecomp: [warning] [core] gta4-native-diag: surface reject #3072 reason=image-allocation depth=true handle=40226690 … address=00010000 … format=438436247 size=1206x540 samples=1
10-16 17:00:51.532 W LibertyRecomp: [warning] [core] gta4-native-diag: surface reject #4096 reason=image-allocation depth=false handle=402267B0 … address=00000002 … format=405275014 size=1206x540 samples=1
10-16 17:00:39.101 E LibertyRecomp: [error] [core] gta4-native-cause: point=explicit-depth-handoff frame=1142 cmd=571 result=rejected reason=surface-create source-wrapper=DB569D60 source=40226690/00010000 destination-wrapper=400007C0 destination=400007C0/00010000 source-texture=D9132320@342 source-host=0x0:s0 destination-host=1206x540:s1
10-16 17:00:39.573 E LibertyRecomp: [error] [core] gta4-modern-exec: backend=vulkan event=failure-target command=410646 slot=1 handle=402267B0 extent=1206x540 format=18280186
… rate-limited rejects reach #28672 by capture end (≈175 rejects/sec); explicit-depth-handoff: 8,205 lines, all rejected, 0 ok; frame census: pipelines=281 textures=582 surfaces=35
```

**Older excerpts (second session):**

```
10-16 13:06:54.935 W LibertyRecomp: title-fps: 29.5 (game frames submitted per second)
10-16 13:06:54.939 W LibertyRecomp: [warning] [core] gta4-draw-census: commands=488 draws=446
    c0x0/d603x270/p0=1 c10x10/d0x0/p6=1 c1206x540/d0x0/p0=1 c1206x540/d1206x540/p0=154
    c1206x540/d1206x540/p6=1 c1x1/d0x0/p6=2 c256x256/d0x0/p0=2 c256x256/d256x256/p0=274
    c301x135/d0x0/p6=4 c320x180/d320x180/p0=3 c40x40/d0x0/p6=1 c5x5/d0x0/p6=1 c75x33/d0x0/p6=1
10-16 13:06:55.084 E LibertyRecomp: [error] [core] gta4-native-cause: point=explicit-depth-handoff frame=4889 cmd=567
    result=rejected reason=surface-create source-wrapper=DB569D60 source=40226600/00010000
    destination-wrapper=400007C0 destination=400007C0/00010000 source-texture=D9132320@43
    source-host=0x0:s0 destination-host=1206x540:s1
10-16 13:06:45.019 E LibertyRecomp: [error] [core] gta4-native-cause: point=surface-address-alias-create
    handle=402182E0 address=00010000 depth=true guest=256x256 host=256x256 format=130 samples=1
    written-alias=true aliases=[400007C0:guest=1206x540:…, 40226600:guest=1206x540:host=1206x540:format=130:samples=1:layout=3:written=true, …]
10-16 13:11:46.367 I LibertyDriver: Driver initialized: {"name":"Mali-G710 MC10","api_version":4198577,
    "features":{"depthClamp":1,"shaderClipDistance":0,"sampleRateShading":1,"dynamicRendering":1,…}}  ← see F9
```
(Third-session host capture: `/tmp/opencode/capture_nativetrace.log`; the older pre-restart capture had been lost. Device debug logs remain under `Android/data/com.libertyrecomp/files/debug_logs/`.)

---

## 9. Next steps (root cause fixed and verified on device; remaining work is follow-up)

**The black-world bug is fixed (F14) and verified (18:12 run: zero rejects, world renders, ≈14–15.5 fps).** Remaining follow-ups: commit the uncommitted work; performance pass (reconcile ≈15 fps present rate vs `title-fps 29.5` under a 30 limiter); triage the residual ≈300–500 `failure-target` diagnostics (pre-existing, mostly null slots); keep the secondary correctness items (proxy clip-control honesty, clip-space rejects); keep F-R2 as a safety net; treat ASTC (§10) as a perf project only.

Reproduction/verification recipe (unchanged):

```bash
adb logcat -c -G 16M
adb logcat -v threadtime > /tmp/opencode/next.log &   # start BEFORE tapping Play
```

**Expected in a healthy run (matches the 18:12 verification):**
1. `surface-sample-clamp … requested=2 effective=1 tuple-sampleCounts=1D` for the scene handles at world start (F14).
2. **Zero** `surface reject`, `image allocation failed`, `support-check rejected`, `deferring to vkCreateImage`.
3. Handoff: no `reason=surface-create` rejects; `result=ok` present; `surface-address-alias-create` shows `samples=1` host images for the scene handles.
4. World visible (screenshot), HUD/minimap correct, ≈15 fps via `fps.sh`.

---

## 10. Would converting textures to ASTC fix the black world? (discussion only — raised 2026-10-16)

**Short answer: no — ASTC was not the fix.** It is a texture storage/sampling optimization; the black world came from a **sample-count capability mismatch** (F11/F14 — fixed), *not* from allocation failure or texture storage.

- **The failing objects were not textures.** The four rejected surfaces are the scene's color/depth targets (RGBA8 + D32S8, 1206x540); their formats were always fine — the reject was the **sample-count** comparison inside `IsImageCreateInfoSupported` (F11/F14). Texture pixel storage never entered that code path.
- **The allocation-failure scenario did not apply** (F14: creation always worked when reached; probes succeeded under identical pressure). BC→RGBA8 decoding (F13) remains a **bandwidth/footprint optimization target** — ASTC (1 / ~0.45 / ~0.22 B/texel vs 4 B/texel) is still attractive for a bandwidth-bound TBDR part like Mali-G710 — a perf/thermal project, not a correctness fix.
- **Object-count cap scenario also excluded** (`maxMemoryAllocationCount=4294967295`, F-D5 limits dump); every image still carries a dedicated `VkDeviceMemory`, which remains worth revisiting for footprint reasons, but it never caused these rejects.
- **Costs/risks:** ASTC requires *encoding* at load (the current BC decode is cheap — 12.2 MB in 13.6 ms across 6 workers; astcenc-class encoding is 1–2 orders slower), plus quality/mip tuning, non-BC format coverage (`k_8_8_8_8`, floats, DXN/CTX1), and a transcode cache to avoid re-encoding each launch.
- **Upside beyond memory:** Mali-G710 is a bandwidth-bound TBDR part; ASTC sampling beats RGBA8 sampling for both bandwidth and cache — a likely FPS win **after** the world renders.

**Recommendation:** revisit ASTC as a performance/bandwidth project once fps/thermal profiling is available (scene MSAA is now 1×, textures are 4 B/texel RGBA8, live set ≈582 images) — it is no longer tied to any correctness fix.
