# OptiScaler for Baldur's Gate 3 on Windows ARM

**The purpose of this fork is to add FSR 3.1 upscaling to Baldur's Gate 3 on Windows ARM, using the game's Vulkan renderer.**

This experimental fork of [OptiScaler](https://github.com/optiscaler/OptiScaler) replaces BG3's native FSR 2.2 with FSR 3.1. It builds an ARM64EC proxy DLL for the x64 game running under Windows 11 ARM64 emulation. Development and testing have been on a Snapdragon / Adreno X1 system with the GOG version of BG3.

FSR 3 here means **upscaling**. Frame generation is not part of the verified setup. Better image quality or higher performance than native FSR 2.2 is not guaranteed; the replacement has used more GPU time in testing.

## What this fork changes

- Replaces Microsoft Detours with **KNSoft.SlimDetours**, including mixed x64 / ARM64EC hooking and x64-compatible proxy exports.
- Intercepts BG3's native Vulkan FSR 2.2 passes. It supplies the replacement with the game's color, depth, motion vectors, jitter, exposure, reactive mask, and transparency/composition mask.
- Supports the tested Vulkan FSR 3.1 backend and the built-in FSR 2.1.2 / 2.2.1 alternatives through OptiScaler's menu.
- Skips redundant native Reconstruct, DepthClip, Lock, and Accumulate dispatches when the replacement succeeds. The native mask generation pass remains necessary. With replacement auto exposure explicitly enabled, native luminance generation can also be skipped.
- Retains the original native commands for fallback if a replacement cannot be recorded safely, and keeps replacement contexts alive until submitted GPU work finishes.
- Keeps the game's native Vulkan swapchain when frame generation is disabled and defers cursor changes to the window thread.

## Supported setup

The native integration is deliberately limited to the verified **Vulkan `bg3.exe`** build and shader signatures. A different executable or shader build disables this integration rather than guessing offsets. The tested executable has SHA-256:

```text
B275FE5207E680F2046BB1A1D4C4B98E4E5C5A0BB455415F987E07F98B3048A6
```

The tested AMD Vulkan runtime is `OptiScaler/amd_fidelityfx_vk.dll`, exposing FSR **3.1.4** and **2.3.3**. Its SHA-256 is:

```text
A1624CC4238FEF046F30C4D80CE3F47BE63FC5F5373F49E3EE9EDB9960F54C78
```

DX12-only choices, including the tested FSR 3.1.5 DX12 runtime, cannot be used by this native Vulkan bridge. Steam builds, other BG3 patches, other ARM GPUs, and frame generation have not been validated. Minimize/restore and backend changes received fixes during development, but this remains an experimental integration; an unexplained focus/minimize event was observed in one profiling run.

## Installation

1. Close BG3 and back up any existing OptiScaler files and configuration in the game's `bin` folder.
2. Start with an [upstream OptiScaler distribution](https://github.com/optiscaler/OptiScaler/releases) containing the Vulkan AMD runtime. Keep its dependency folder beside the game executable. Match the tested runtime above when reproducing this setup.
3. Place this fork's built DLL beside `bg3.exe`, named **`winmm.dll`**. Replace an existing OptiScaler proxy rather than loading two copies. Keep `OptiScaler.ini` beside it.
4. Merge the following settings into the corresponding sections of `OptiScaler.ini`:

```ini
[Inputs]
ReplaceBg3Fsr2=true
OptimizeBg3Fsr2=true

[Upscalers]
VulkanUpscaler=ffx

[FrameGen]
Enabled=false
```

5. Launch BG3 with **Vulkan** and select **FSR 2.2 in the game's graphics settings**. Leave that selected: it provides the native inputs intercepted by this fork.
6. Press **Insert** to open OptiScaler. Select the Vulkan FSR 3.1 backend/version available from the tested AMD runtime. The game will still report FSR 2.2 because replacement happens underneath it.

To let the replacement compute its own exposure and skip the native luminance pass, optionally add:

```ini
[InitFlags]
AutoExposure=true
```

Auto exposure can change the image. Without that explicit option, native exposure generation is retained. Mask generation is retained in both cases because the replacement uses its results. For comparison with the original game, close BG3 and set `ReplaceBg3Fsr2=false` before launching it again.

## Building

Clone this fork with its submodules:

```powershell
git clone --recurse-submodules https://github.com/sr-tream/OptiScaler-BG3-Windows-ARM.git
cd OptiScaler-BG3-Windows-ARM
.\Build-SlimDetours.ps1
```

The tested toolchain is Visual Studio 2026 Community, MSVC v145, ARM64EC C++ tools, and Windows SDK 10.0.26100.0, on Windows ARM64. The script discovers Visual Studio with `vswhere`; `-VisualStudio` and `-Toolset` can override its choices. Other toolchains have not been verified.

The output is `ARM64EC/Release/OptiScaler.dll`; rename the copy used by BG3 to `winmm.dll`. Proxy forwarding is intentionally compiled as x64 while the main DLL uses ARM64EC. Preserve that per-file setting.

The vendored SlimDetours headers and static libraries are from KNSoft.SlimDetours 1.2.3-beta. Package provenance and its MIT license are in [`external/SlimDetours`](external/SlimDetours).

Regression and GPU smoke tests are in [`tests`](tests). They cover mixed-architecture hooks, command replay, GPU resource lifetime, cursor focus behavior, DLL/Vulkan loading, the overlay, and synthetic native-format frames with both masks. The test scripts currently target Windows ARM64 with the documented MSVC/SDK versions; GPU tests require the AMD Vulkan runtime from an OptiScaler distribution. They do not establish performance or visual correctness in every BG3 scene.

## Upstream and license

This is an independent experimental fork. See the preserved [upstream README](README.upstream.md) and [OptiScaler wiki](https://github.com/optiscaler/OptiScaler/wiki) for the original project and general configuration. General upstream compatibility claims should not be read as validation of this ARM64EC fork.

OptiScaler's [GPL-3.0 license](LICENSE) is retained. Credit belongs to the OptiScaler contributors, [KNSoft.SlimDetours](https://github.com/KNSoft/KNSoft.SlimDetours), and the respective FidelityFX and other dependency authors. BG3 binaries, game shaders, diagnostic dumps, and proprietary game assets are not included.
