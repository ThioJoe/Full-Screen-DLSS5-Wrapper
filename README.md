# DlssScreen

DlssScreen applies **DLSS 5 Neural Rendering** (NGX feature 18, `nvngx_dlssnr.dll`) to the live desktop.
It captures the composited screen with Windows Graphics Capture, runs the model on its own Direct3D 12
device and presents the result in its own borderless, topmost, click-through window that is excluded
from capture, so the picture never feeds back into itself. Optionally DLSS Super Resolution (feature 1)
upscales the capture to a larger target monitor first.

The tool never injects into, hooks, or opens handles to any other process. It uses only the OS-level
screen capture API and its own GPU device, and it runs entirely outside whatever it is showing you.

That is a statement about what this code does, not a promise about how any particular anti-cheat will
treat it. Anti-cheat systems are free to look at running processes, loaded modules, window and capture
activity, and overlays drawn over a game, and any of that can be noticed. Some also object to a
third-party overlay on principle, whatever it is doing. Check the rules of anything you run this
alongside; the design avoids the techniques anti-cheat is built to catch, but it cannot make the tool
invisible, and nothing here should be read as a guarantee against a ban.

This branch is a from-scratch rewrite under the *Rules for AI-Written Code* (see `COMPLIANCE.md`):
a pure planning core, a seeded simulator of the effect layer, property tests with mutation testing,
build-applied call tracing, contracts that run in production, and a lint that enforces the rules.

## This is not what DLSS 5 looks like in a game

A game hands the model its own motion vectors, its own depth buffer and the sub-pixel jitter it rendered
with, frame by frame, before anything is composited. DlssScreen has none of that. It captures the finished
desktop and makes substitutes: one flat depth plane, and motion guessed by matching blocks between two
pictures that have already been drawn, resized and blended by the window manager.

So the model is working from worse inputs than it was built for, on an image that has already lost the
information it wants. What it does to the desktop is not what it does in a game, and neither is what it
costs: the capture, the matching and the extra copies are all work a game would not be doing, and none of
it is part of DLSS. Judge DLSS 5 by a game that implements it. This is a way to watch the model run on
something it was never given.

The control panel says the same thing at the foot of its window.

## Working on one window

On the panel's Start-up page, drag the crosshair onto a window to work on that one window instead of a
monitor. The title under the pointer is shown beside it as you drag; letting go over the desktop goes back
to capturing a monitor. From the command line, `--window` takes either part of a window's title, ignoring
case, or a window handle as `0x...` — which is what the panel writes when it starts a new session.

This uses Windows Graphics Capture's own per-window item, so the model sees that window's content and
nothing else — not what is stacked in front of it — and the capture follows the window as it moves. It
still opens nothing belonging to the other process. The overlay follows the window each frame.

The sizes of everything downstream are fixed when the session starts, so resizing the window is not
followed: the capture is cropped to the size that was planned for until a new session is started.

## The model file

`nvngx_dlssnr.dll` is NVIDIA's, is not in this repository, and is not in the build artifact. Put it next to
`DlssScreen.exe` or point `--ngx-path` at the folder that holds it.

Because the NGX loader picks that file up by name from a folder anyone can write to, DlssScreen checks it
before the loader gets there: Windows must accept its Authenticode signature, and the signing certificate
must name NVIDIA. A file that fails stops the session. The file is then held open, shared for reading only,
for as long as the session runs, so it cannot be written to, deleted or renamed afterwards — the file that
was checked is the file that loads. This does not defend against a machine that was already compromised
before the check, and it says nothing about a model loaded from anywhere else.

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

Double-clicking the executable opens the control panel and the overlay, with no console window. Every
command line option except `--help`, `--list-monitors`, `--gui` and the NGX runtime settings has a control
on the panel, on one of three pages: **Model** for what the model reads, **View** for the window and the
capture, and **Start-up** for what only a fresh session can change. A number is a slider to sweep it, a box
to type an exact value and arrows to step it, with a reset beside each; a choice is a row of buttons; a
path is a box to type in. Every row is measured from the height of the display's own text, so nothing
crowds or clips whatever the scaling.

Moving anything on the first two pages takes effect at once. Changing the tuning rebuilds the model's
feature, because the model reads it while the feature is built rather than on each frame, and the frame is
drawn again even when the desktop has sent nothing new, so a change shows even while the panel sits on
another monitor. The Start-up page describes a session rather than changing this one: **Start a new session
with these** writes those settings out as a command line and launches it, carrying the NGX settings the
panel has no control for through unchanged. Closing the panel ends the session. Started from a console
instead, the program attaches to it and logs there; `--console on` forces one, `--gui off` leaves the
overlay to run alone.

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

## What the model does with these values

The 310.8 model carries a single set of weights, under preset 1, which is what DlssScreen asks for; any
other number falls back to it and says so in the NGX log. Style takes 0, 1 or 2 and is clamped by the
model itself. The strengths are unbounded floats: the model applies no limit of its own, so DlssScreen
imposes none either, though the values it was authored around sit between 0 and 1. Skin structure and
local structure only do anything while auto mask is on, and UI correction reads a UI layer that
DlssScreen does not supply, so it is inert as wired. The model runs one-to-one and does no scaling; any
resizing comes from the separate super resolution pass.

The motion scales (`--mv-scale-x`, `--mv-scale-y`) are what the model multiplies the motion vectors by.
Left out, each is the ratio between the model's working size and the captured one, which is what the
synthesised vectors are measured in; given, the operator's number is used instead, and a negative one
flips that axis. `--depth-inverted` tells the model the depth plane counts the other way, which with one
flat plane changes little. None of these carries a range, so DlssScreen imposes none.

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
