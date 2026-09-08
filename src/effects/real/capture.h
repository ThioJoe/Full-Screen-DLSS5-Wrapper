#pragma once
#include "effects/real/device.h"
#include "interior/monitors.h"

#include <d3d11on12.h>
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

struct Capture
{
    Com<ID3D11Device> device11;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11On12Device> on12;
    Com<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> winrtDevice;
    Com<ID3D11Resource> wrappedCanvas;
    infra::BoundedVector<MonitorSession, interior::kMaxMonitors> sessions;
    interior::ScreenRect canvasRect;
    interior::Extent canvasExtent;
};

[[nodiscard]] infra::Status<Error> InitializeRuntime() noexcept;
[[nodiscard]] infra::Status<Error> RequireCaptureSupport() noexcept;
[[nodiscard]] infra::Result<Capture, Error> CreateCapture(const GpuDevice& gpu, ID3D12Resource* canvas, const interior::ScreenRect& canvasRect, const interior::Extent& canvasExtent,
                                                          const interior::MonitorList& monitors, const CaptureSettings& settings) noexcept;
[[nodiscard]] infra::Result<bool, Error> AcquireFrames(const Capture& capture) noexcept;

} // namespace real
