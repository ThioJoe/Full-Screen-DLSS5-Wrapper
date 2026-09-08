# DlssScreen

DlssScreen applies **DLSS 5 Neural Rendering** (NGX feature 18, `nvngx_dlssnr.dll`) to the live desktop.
It captures the composited screen with Windows Graphics Capture, runs the model on its own Direct3D 12
device and presents the result in its own borderless, topmost, click-through window that is excluded
from capture, so the picture never feeds back into itself. Optionally DLSS Super Resolution (feature 1)
upscales the capture to a larger target monitor first.

The tool never injects into, hooks, or opens handles to any other process. It only uses OS-level screen
capture and its own GPU device, which keeps it invisible to anti-cheat.

This branch is a from-scratch rewrite under the *Rules for AI-Written Code* (see `COMPLIANCE.md`):
a pure planning core, a seeded simulator of the effect layer, property tests with mutation testing,
build-applied call tracing, contracts that run in production, and a lint that enforces the rules.

## Requirements

- Windows 10 2004 or newer (Windows Graphics Capture, DirectComposition), Windows 11 recommended.
- An NVIDIA RTX GPU with driver **616.64 or newer** for neural rendering. 616.64 is the first driver
  whose NGX loader offers DLSS 5 (feature 18) itself; on 616.56 and older the loader answers
  `NotImplemented` to the requirements query and cannot build the feature, and DlssScreen stops at
  start-up with a message naming the installed and the required driver.
- The [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (`lib/Windows_x86_64/x64/nvsdk_ngx_s.lib`, `include/`, `lib/Windows_x86_64/rel/nvngx_dlss.dll`).
- NVIDIA's DLSS 5 model, `nvngx_dlssnr.dll`, next to `DlssScreen.exe` or in the folder given by
  `--ngx-path`. NGX looks for feature DLLs in the application folder and the listed paths, the way
  games ship `nvngx_dlss.dll`; the driver does not install this one and DlssScreen does not ship it.
  Without it the loader reports `DLSSNR.Available = 0` and DlssScreen stops with a message saying so.
- Visual Studio 2022 17.8+ (MSVC 19.38), CMake 3.21+, Ninja or MSBuild, the Windows 10 SDK (dxc.exe).
- Optional: the NVIDIA Optical Flow SDK for the hardware motion-vector backend (`-DDSCREEN_ENABLE_NVOF=ON`).

There are no third-party dependencies beyond NVIDIA's SDKs and the platform (see `gate/dependencies.lock`).

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DDLSS_SDK_DIR=C:\path\to\DLSS
cmake --build build --config Release
```

The portable core, simulator, tests and gate tools also build on Linux with GCC 13+:

```
cmake -S . -B build-linux -G Ninja && cmake --build build-linux && ctest --test-dir build-linux
```

## Run

```
DlssScreen.exe                       # neural rendering on the primary monitor
DlssScreen.exe --monitor 1 --target 0  # capture monitor 1, upscale with DLSS and present on monitor 0
DlssScreen.exe --list-monitors
DlssScreen.exe --help
```

Hotkeys (global): `Ctrl+Alt+Shift+O` original/processed, `Ctrl+Alt+Shift+C` split view, `Ctrl+Alt+Shift+Q` quit.

Every failure stops the program with a message and a non-zero exit code. There are no silent fallbacks:
if neural rendering is requested and unavailable, the tool exits instead of running as a passthrough
(pass `--nr off` or `--sr off` to run without a model). A contract violation aborts and writes the call
trace ring to `dlssscreen-trace.txt` next to the working directory.

## How it works

1. **Capture**: one `Direct3D11CaptureFramePool` per source monitor (free-threaded, polled every frame)
   on a plain Direct3D 11 device, the only kind the capture API accepts. That device owns the canvas
   and copies frames into it; the models see the same texture through a shared handle, and two shared
   fences order the two devices on the GPU, so no CPU wait sits between them. The output window is
   excluded with
   `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`.
2. **Synthesised inputs**: the desktop has no depth or motion vectors. A constant depth plane and motion
   vectors from a GPU coarse-to-fine block matcher (or NVIDIA Optical Flow, or zeros) feed the models.
   A high fraction of unmatched pixels or a long pause resets the models' temporal history.
3. **Models**: DLSS Super Resolution bridges source and target sizes; DLSS 5 Neural Rendering runs on
   the result through the driver's NGX core using the `DLSSNR.*` parameter names.
4. **Present**: a DirectComposition flip-model swap chain with a frame-latency waitable object.

## Layout

| Directory | Contents |
| --- | --- |
| `src/infrastructure` | generic primitives: results, strong types, bounded containers, checked arithmetic, contracts, tracing |
| `src/interior` | the pure core: options, monitors, planning, the per-frame step planner, NGX parameter lists |
| `src/effects/sim` | seeded simulator of the effect layer with failure injection |
| `src/effects/real` | Windows: D3D12, DirectComposition, Windows Graphics Capture, NGX, optical flow |
| `src/app` | the generic session loop and the composition root |
| `tests` | property tests and the seed fuzzer |
| `gate` | the rules lint, mutation testing, dependency lock, gate scripts |
| `shaders` | HLSL compute and blit kernels compiled by DXC at build time |

## The gate

`gate/gate.sh` (Linux, portable targets) and `gate/gate.ps1` (Windows, everything) run: the build with
warnings as errors and tracing, the formatter check, the rules lint with its function index and
inventories, the property tests under several seeds, the sanitizers, mutation testing, and the
dependency-lock check. `COMPLIANCE.md` maps every rule to its enforcement.
