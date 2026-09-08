#pragma once
#include "effects/real/device.h"
#include "interior/monitors.h"

#include <d3d11_4.h>
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
};

// Capture runs on its own Direct3D 11 device, the only kind Windows Graphics Capture accepts. Frames
// are copied into the D3D12 canvas, opened here as a shared texture; two shared fences order the devices.
struct Capture
{
    Com<ID3D11Device> device11;
    Com<ID3D11DeviceContext4> context;
    Com<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> winrtDevice;
    Com<ID3D11Texture2D> canvas;     // the D3D12 canvas as seen by the capture device
    Com<ID3D11Fence> frameFence;     // the D3D12 frame fence: waited on before the canvas is written
    Com<ID3D12Fence> captureFence12; // signalled by the capture device after a copy, waited on by the queue
    Com<ID3D11Fence> captureFence;
    Com<ID3D12CommandQueue> queue;
    infra::BoundedVector<MonitorSession, interior::kMaxMonitors> sessions;
    interior::ScreenRect canvasRect;
    interior::Extent canvasExtent;
};

[[nodiscard]] infra::Status<Error> InitializeRuntime() noexcept;
[[nodiscard]] infra::Status<Error> RequireCaptureSupport() noexcept;
[[nodiscard]] infra::Result<Capture, Error> CreateCapture(const GpuDevice& gpu, ID3D12Resource* canvas, const interior::ScreenRect& canvasRect, const interior::Extent& canvasExtent,
                                                          const interior::MonitorList& monitors, const CaptureSettings& settings) noexcept;
// Copies the newest frame of every monitor into the canvas after the frame that signalled lastFrame is done
// with it, and makes the queue wait for the copy before frame number's work. True when a frame arrived.
[[nodiscard]] infra::Result<bool, Error> AcquireFrames(const Capture& capture, interior::FrameNumber number, interior::FenceValue lastFrame) noexcept;

} // namespace real
