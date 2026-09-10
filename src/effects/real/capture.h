#pragma once
#include "effects/real/device.h"
#include "interior/monitors.h"

#include <d3d11_4.h>
#include <span>
#include <windows.graphics.capture.h>
#include <windows.graphics.directx.direct3d11.h>

namespace real {

struct CaptureSettings
{
    bool cursor;
    bool border;
};

struct MonitorSession
{
    Com<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> item;
    Com<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool> pool;
    Com<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession> session;
    interior::MonitorInfo monitor;
    bool excluding; // whether this session agreed to leave our own windows out of what it captures
};

// Capture runs on its own Direct3D 11 device, the only kind Windows Graphics Capture accepts, and that
// device owns the canvas and both fences: sharing runs 11 to 12, the direction the platform supports.
struct Capture
{
    Com<ID3D11Device> device11;
    Com<ID3D11DeviceContext4> context;
    Com<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> winrtDevice;
    Com<ID3D11Texture2D> canvas;
    Com<ID3D12Resource> sharedCanvas; // the same texture, as the models see it
    Com<ID3D11Fence> canvasFree;      // signalled by the queue when the last frame is done reading
    Com<ID3D12Fence> sharedCanvasFree;
    Com<ID3D11Fence> canvasReady; // signalled by the capture device when the copy is done
    Com<ID3D12Fence> sharedCanvasReady;
    Com<ID3D12CommandQueue> queue;
    infra::BoundedVector<MonitorSession, interior::kMaxMonitors> sessions;
    interior::ScreenRect canvasRect;
    interior::Extent canvasExtent;
    bool excludesOurWindows; // whether every session agreed to leave our own windows out of what it captures
};

[[nodiscard]] infra::Status<Error> InitializeRuntime() noexcept;
[[nodiscard]] infra::Status<Error> RequireCaptureSupport() noexcept;
// `ours` are this program's own windows. Where the session can be told to leave them out, it is, and the
// display affinity that hides them from every capture on the machine is not needed.
[[nodiscard]] infra::Result<Capture, Error> CreateCapture(const GpuDevice& gpu, const interior::ScreenRect& canvasRect, const interior::Extent& canvasExtent, const interior::MonitorList& monitors,
                                                          const CaptureSettings& settings, std::span<const HWND> ours) noexcept;
// Copies the newest frame of every monitor into the canvas, after the work already submitted to the queue
// has finished reading it and before this frame's work runs. True when a frame arrived.
[[nodiscard]] infra::Result<bool, Error> AcquireFrames(const Capture& capture, interior::FrameNumber number) noexcept;

// Changes what the running capture sessions include; both are settings of the session, not of the frame.
[[nodiscard]] infra::Status<Error> ApplyCaptureSettings(const Capture& capture, const CaptureSettings& settings) noexcept;

} // namespace real
