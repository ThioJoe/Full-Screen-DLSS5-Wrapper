#include "effects/real/capture.h"

#include "infrastructure/checked.h"
#include "infrastructure/fold.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winstring.h>

#include <algorithm>
#include <ranges>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;
namespace WGC = ABI::Windows::Graphics::Capture;
namespace WGD = ABI::Windows::Graphics::DirectX;
namespace WGD11 = ABI::Windows::Graphics::DirectX::Direct3D11;

constexpr wchar_t kPoolClass[] = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
constexpr wchar_t kItemClass[] = L"Windows.Graphics.Capture.GraphicsCaptureItem";
constexpr wchar_t kSessionClass[] = L"Windows.Graphics.Capture.GraphicsCaptureSession";
constexpr std::uint32_t kMaxFramesDrained = 8;
constexpr INT32 kPoolBuffers = 2;
constexpr DXGI_FORMAT kCanvasFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

struct CopyRegion;
struct Pending;
[[nodiscard]] std::optional<CopyRegion> RegionFrom(const Capture& capture, const Pending& p, const D3D11_TEXTURE2D_DESC& desc, std::int32_t x, std::int32_t y) noexcept;

// The fast-pass HSTRING points into the header, so both live only inside this call.
[[nodiscard]] Status<Error> ActivationFactory(std::wstring_view className, REFIID iid, void** factory) noexcept
{
    REQUIRE(className.data()[className.size()] == L'\0');
    HSTRING_HEADER header{};
    HSTRING string = nullptr;
    return Check(::WindowsCreateStringReference(className.data(), static_cast<UINT32>(className.size()), &header, &string), ApiCall::WindowsCreateStringReference).and_then([&] {
        return Check(::RoGetActivationFactory(string, iid, factory), ApiCall::RoGetActivationFactory);
    });
}

[[nodiscard]] Result<Com<IGraphicsCaptureItemInterop>, Error> ItemInterop() noexcept
{
    Com<IGraphicsCaptureItemInterop> interop;
    return ActivationFactory(kItemClass, IID_PPV_ARGS(&interop)).transform([&interop] { return interop; });
}

[[nodiscard]] Result<Com<WGC::IDirect3D11CaptureFramePoolStatics2>, Error> PoolStatics() noexcept
{
    Com<WGC::IDirect3D11CaptureFramePoolStatics2> statics;
    return ActivationFactory(kPoolClass, IID_PPV_ARGS(&statics)).transform([&statics] { return statics; });
}

[[nodiscard]] Result<Com<WGC::IGraphicsCaptureSessionStatics>, Error> SessionStatics() noexcept
{
    Com<WGC::IGraphicsCaptureSessionStatics> statics;
    return ActivationFactory(kSessionClass, IID_PPV_ARGS(&statics)).transform([&statics] { return statics; });
}

[[nodiscard]] Result<Com<ID3D11Device>, Error> CreateDevice11(const GpuDevice& gpu, Com<ID3D11DeviceContext>& context) noexcept
{
    Com<ID3D11Device> device;
    const HRESULT hr = ::D3D11CreateDevice(gpu.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    return Check(hr, ApiCall::D3D11CreateDevice).transform([&device] { return device; });
}

// The free-threaded pool uses the context from the capture service's thread, so it is protected.
[[nodiscard]] Status<Error> ProtectContext(const Com<ID3D11DeviceContext>& context) noexcept
{
    return As<ID3D11Multithread>(context, ApiCall::QueryInterface).transform([](const Com<ID3D11Multithread>& multithread) { multithread->SetMultithreadProtected(TRUE); });
}

[[nodiscard]] Result<Com<WGD11::IDirect3DDevice>, Error> WinrtDeviceOf(const Com<ID3D11Device>& device11) noexcept
{
    return As<IDXGIDevice>(device11, ApiCall::QueryInterface).and_then([](const Com<IDXGIDevice>& dxgi) {
        Com<IInspectable> inspectable;
        const HRESULT hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &inspectable);
        return Check(hr, ApiCall::CreateDirect3D11DeviceFromDXGIDevice).and_then([&inspectable] { return As<WGD11::IDirect3DDevice>(inspectable, ApiCall::QueryInterface); });
    });
}

[[nodiscard]] D3D11_TEXTURE2D_DESC CanvasDescription(const interior::Extent& extent) noexcept
{
    return D3D11_TEXTURE2D_DESC{ extent.width.Get(),
                                 extent.height.Get(),
                                 1,
                                 1,
                                 kCanvasFormat,
                                 { 1, 0 },
                                 D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                 0,
                                 D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
}

[[nodiscard]] Result<Com<ID3D11Texture2D>, Error> CreateCanvas(const Com<ID3D11Device>& device11, const interior::Extent& extent) noexcept
{
    const D3D11_TEXTURE2D_DESC desc = CanvasDescription(extent);
    Com<ID3D11Texture2D> canvas;
    return Check(device11->CreateTexture2D(&desc, nullptr, &canvas), ApiCall::CreateTexture2D).transform([&canvas] { return canvas; });
}

[[nodiscard]] Result<UniqueHandle, Error> ResourceHandle(const Com<ID3D11Texture2D>& canvas) noexcept
{
    return As<IDXGIResource1>(canvas, ApiCall::QueryInterface).and_then([](const Com<IDXGIResource1>& resource) -> Result<UniqueHandle, Error> {
        HANDLE handle = nullptr;
        const HRESULT hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        return Check(hr, ApiCall::CreateSharedHandle).transform([handle] { return UniqueHandle(handle); });
    });
}

[[nodiscard]] Result<Com<ID3D11Fence>, Error> CreateSharedFence(const Com<ID3D11Device>& device11) noexcept
{
    return As<ID3D11Device5>(device11, ApiCall::QueryInterface).and_then([](const Com<ID3D11Device5>& device5) -> Result<Com<ID3D11Fence>, Error> {
        Com<ID3D11Fence> fence;
        return Check(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), ApiCall::D3D11CreateFence).transform([&fence] { return fence; });
    });
}

[[nodiscard]] Result<UniqueHandle, Error> FenceHandle(const Com<ID3D11Fence>& fence) noexcept
{
    HANDLE handle = nullptr;
    return Check(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), ApiCall::CreateSharedHandle).transform([handle] { return UniqueHandle(handle); });
}

[[nodiscard]] Result<Com<ID3D12Resource>, Error> OpenedCanvas(const GpuDevice& gpu, const UniqueHandle& handle) noexcept
{
    Com<ID3D12Resource> opened;
    return Check(gpu.device->OpenSharedHandle(handle.get(), IID_PPV_ARGS(&opened)), ApiCall::OpenSharedHandle).transform([&opened] { return opened; });
}

// WAIVER(R7): the same open over another interface; a template here would put metaprogramming outside infrastructure.
[[nodiscard]] Result<Com<ID3D12Fence>, Error> OpenedFence(const GpuDevice& gpu, const UniqueHandle& handle) noexcept
{
    Com<ID3D12Fence> opened;
    return Check(gpu.device->OpenSharedHandle(handle.get(), IID_PPV_ARGS(&opened)), ApiCall::OpenSharedHandle).transform([&opened] { return opened; });
}

[[nodiscard]] Result<Com<WGC::IGraphicsCaptureItem>, Error> ItemFor(const interior::MonitorInfo& monitor) noexcept
{
    return ItemInterop().and_then([&monitor](const Com<IGraphicsCaptureItemInterop>& interop) {
        Com<WGC::IGraphicsCaptureItem> item;
        const HRESULT hr = interop->CreateForMonitor(reinterpret_cast<HMONITOR>(monitor.handle.Get()), IID_PPV_ARGS(&item));
        return Check(hr, ApiCall::CreateForMonitor).transform([&item] { return item; });
    });
}

[[nodiscard]] Result<ABI::Windows::Graphics::SizeInt32, Error> SizeOf(WGC::IGraphicsCaptureItem* item) noexcept
{
    ABI::Windows::Graphics::SizeInt32 size{};
    return Check(item->get_Size(&size), ApiCall::GetContentSize).transform([&size] { return size; });
}

[[nodiscard]] Result<Com<WGC::IDirect3D11CaptureFramePool>, Error> PoolFor(WGD11::IDirect3DDevice* device, WGC::IGraphicsCaptureItem* item) noexcept
{
    return PoolStatics().and_then([&](const Com<WGC::IDirect3D11CaptureFramePoolStatics2>& statics) {
        return SizeOf(item).and_then([&](ABI::Windows::Graphics::SizeInt32 size) -> Result<Com<WGC::IDirect3D11CaptureFramePool>, Error> {
            Com<WGC::IDirect3D11CaptureFramePool> pool;
            const HRESULT hr = statics->CreateFreeThreaded(device, WGD::DirectXPixelFormat_B8G8R8A8UIntNormalized, kPoolBuffers, size, &pool);
            return Check(hr, ApiCall::CreateFreeThreaded).transform([&pool] { return pool; });
        });
    });
}

[[nodiscard]] Status<Error> ApplyCursor(const Com<WGC::IGraphicsCaptureSession>& session, bool cursor) noexcept
{
    return As<WGC::IGraphicsCaptureSession2>(session, ApiCall::PutIsCursorCaptureEnabled).and_then([cursor](const Com<WGC::IGraphicsCaptureSession2>& s2) {
        return Check(s2->put_IsCursorCaptureEnabled(cursor ? 1 : 0), ApiCall::PutIsCursorCaptureEnabled);
    });
}

[[nodiscard]] Status<Error> ApplyBorder(const Com<WGC::IGraphicsCaptureSession>& session, bool border) noexcept
{
    if (border)
        return {};
    return As<WGC::IGraphicsCaptureSession3>(session, ApiCall::PutIsBorderRequired).and_then([](const Com<WGC::IGraphicsCaptureSession3>& s3) {
        return Check(s3->put_IsBorderRequired(0), ApiCall::PutIsBorderRequired);
    });
}

[[nodiscard]] Result<Com<WGC::IGraphicsCaptureSession>, Error> SessionFor(WGC::IDirect3D11CaptureFramePool* pool, WGC::IGraphicsCaptureItem* item, const CaptureSettings& settings) noexcept
{
    Com<WGC::IGraphicsCaptureSession> session;
    return Check(pool->CreateCaptureSession(item, &session), ApiCall::CreateCaptureSession)
        .and_then([&] { return ApplyCursor(session, settings.cursor); })
        .and_then([&] { return ApplyBorder(session, settings.border); })
        .and_then([&] { return Check(session->StartCapture(), ApiCall::StartCapture); })
        .transform([&session] { return session; });
}

[[nodiscard]] Result<MonitorSession, Error> StartSession(WGD11::IDirect3DDevice* device, const interior::MonitorInfo& monitor, const CaptureSettings& settings) noexcept
{
    return ItemFor(monitor).and_then([&](const Com<WGC::IGraphicsCaptureItem>& item) {
        return PoolFor(device, item.Get()).and_then([&](const Com<WGC::IDirect3D11CaptureFramePool>& pool) {
            return SessionFor(pool.Get(), item.Get(), settings).transform([&](const Com<WGC::IGraphicsCaptureSession>& session) { return MonitorSession{ item, pool, session, monitor }; });
        });
    });
}

using Sessions = infra::BoundedVector<MonitorSession, interior::kMaxMonitors>;

[[nodiscard]] Result<Sessions, Error> StartAll(WGD11::IDirect3DDevice* device, const interior::MonitorList& monitors, const CaptureSettings& settings) noexcept
{
    return infra::FoldResult(monitors.Items(), Result<Sessions, Error>(Sessions{}), [&](const Sessions& acc, const interior::MonitorInfo& monitor) {
        return StartSession(device, monitor, settings).and_then([&acc](const MonitorSession& s) {
            return acc.Push(s).transform_error([](infra::CapacityExceeded) { return Error{ ApiCall::CreateCaptureSession, 1 }; });
        });
    });
}

struct Devices
{
    Com<ID3D11Device> device11;
    Com<ID3D11DeviceContext4> context;
    Com<WGD11::IDirect3DDevice> winrtDevice;
};

[[nodiscard]] Result<Devices, Error> CreateDevices(const GpuDevice& gpu) noexcept
{
    Com<ID3D11DeviceContext> context;
    return CreateDevice11(gpu, context).and_then([&](const Com<ID3D11Device>& device11) {
        return ProtectContext(context).and_then([&] { return As<ID3D11DeviceContext4>(context, ApiCall::QueryInterface); }).and_then([&](const Com<ID3D11DeviceContext4>& context4) {
            return WinrtDeviceOf(device11).transform([&](const Com<WGD11::IDirect3DDevice>& winrt) { return Devices{ device11, context4, winrt }; });
        });
    });
}

// The objects both devices see. Each is created by the capture device and opened on the Direct3D 12 one.
struct Bridge
{
    Com<ID3D11Texture2D> canvas;
    Com<ID3D12Resource> sharedCanvas;
    Com<ID3D11Fence> canvasFree;
    Com<ID3D12Fence> sharedCanvasFree;
    Com<ID3D11Fence> canvasReady;
    Com<ID3D12Fence> sharedCanvasReady;
};

struct SharedFence
{
    Com<ID3D11Fence> fence;
    Com<ID3D12Fence> shared;
};

[[nodiscard]] Result<SharedFence, Error> SharedFenceFor(const Devices& d, const GpuDevice& gpu) noexcept
{
    return CreateSharedFence(d.device11).and_then([&](const Com<ID3D11Fence>& fence) {
        return FenceHandle(fence).and_then(
            [&](const UniqueHandle& handle) { return OpenedFence(gpu, handle).transform([&fence](const Com<ID3D12Fence>& shared) { return SharedFence{ fence, shared }; }); });
    });
}

struct SharedCanvas
{
    Com<ID3D11Texture2D> canvas;
    Com<ID3D12Resource> shared;
};

[[nodiscard]] Result<SharedCanvas, Error> SharedCanvasFor(const Devices& d, const GpuDevice& gpu, const interior::Extent& extent) noexcept
{
    return CreateCanvas(d.device11, extent).and_then([&](const Com<ID3D11Texture2D>& canvas) {
        return ResourceHandle(canvas).and_then(
            [&](const UniqueHandle& handle) { return OpenedCanvas(gpu, handle).transform([&canvas](const Com<ID3D12Resource>& shared) { return SharedCanvas{ canvas, shared }; }); });
    });
}

[[nodiscard]] Result<Bridge, Error> CreateBridge(const Devices& d, const GpuDevice& gpu, const interior::Extent& extent) noexcept
{
    return SharedCanvasFor(d, gpu, extent).and_then([&](const SharedCanvas& canvas) {
        return SharedFenceFor(d, gpu).and_then([&](const SharedFence& free) {
            return SharedFenceFor(d, gpu).transform([&](const SharedFence& ready) { return Bridge{ canvas.canvas, canvas.shared, free.fence, free.shared, ready.fence, ready.shared }; });
        });
    });
}

// --- per-frame acquisition -------------------------------------------------------------------------

struct Drain
{
    Com<WGC::IDirect3D11CaptureFrame> latest;
    bool done;
};

[[nodiscard]] Status<Error> CloseFrame(const Com<WGC::IDirect3D11CaptureFrame>& frame) noexcept
{
    if (!frame)
        return {};
    return As<ABI::Windows::Foundation::IClosable>(frame, ApiCall::CloseFrame).and_then([](const Com<ABI::Windows::Foundation::IClosable>& closable) {
        return Check(closable->Close(), ApiCall::CloseFrame);
    });
}

[[nodiscard]] Result<Drain, Error> Replace(const Drain& d, const Com<WGC::IDirect3D11CaptureFrame>& next) noexcept
{
    return CloseFrame(d.latest).transform([&next] { return Drain{ next, false }; });
}

[[nodiscard]] Result<Drain, Error> Received(const Drain& d, const Com<WGC::IDirect3D11CaptureFrame>& next) noexcept
{
    if (!next)
        return Drain{ d.latest, true };
    return Replace(d, next);
}

[[nodiscard]] Result<Drain, Error> DrainOne(const Drain& d, WGC::IDirect3D11CaptureFramePool* pool) noexcept
{
    if (d.done)
        return d;
    Com<WGC::IDirect3D11CaptureFrame> next;
    return Check(pool->TryGetNextFrame(&next), ApiCall::TryGetNextFrame).and_then([&] { return Received(d, next); });
}

[[nodiscard]] Result<Com<WGC::IDirect3D11CaptureFrame>, Error> LatestFrame(WGC::IDirect3D11CaptureFramePool* pool) noexcept
{
    return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, kMaxFramesDrained), Result<Drain, Error>(Drain{ nullptr, false }),
                             [pool](const Drain& d, std::uint32_t) { return DrainOne(d, pool); })
        .transform([](const Drain& d) { return d.latest; });
}

struct Pending
{
    Com<WGC::IDirect3D11CaptureFrame> frame;
    Com<ID3D11Texture2D> texture;
    interior::MonitorInfo monitor;
};

using PendingList = infra::BoundedVector<Pending, interior::kMaxMonitors>;

[[nodiscard]] Result<Com<ID3D11Texture2D>, Error> TextureOf(WGC::IDirect3D11CaptureFrame* frame) noexcept
{
    Com<WGD11::IDirect3DSurface> surface;
    return Check(frame->get_Surface(&surface), ApiCall::GetSurface)
        .and_then([&] { return As<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>(surface, ApiCall::GetInterface); })
        .and_then([](const Com<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>& access) -> Result<Com<ID3D11Texture2D>, Error> {
            Com<ID3D11Texture2D> texture;
            return Check(access->GetInterface(IID_PPV_ARGS(&texture)), ApiCall::GetInterface).transform([&texture] { return texture; });
        });
}

[[nodiscard]] Result<PendingList, Error> AppendPending(const PendingList& acc, const MonitorSession& session, const Com<WGC::IDirect3D11CaptureFrame>& frame) noexcept
{
    if (!frame)
        return acc;
    return TextureOf(frame.Get()).and_then([&](const Com<ID3D11Texture2D>& texture) {
        return acc.Push(Pending{ frame, texture, session.monitor }).transform_error([](infra::CapacityExceeded) { return Error{ ApiCall::TryGetNextFrame, 1 }; });
    });
}

[[nodiscard]] Result<PendingList, Error> CollectPending(const Capture& capture) noexcept
{
    return infra::FoldResult(capture.sessions.Items(), Result<PendingList, Error>(PendingList{}), [](const PendingList& acc, const MonitorSession& session) {
        return LatestFrame(session.pool.Get()).and_then([&](const Com<WGC::IDirect3D11CaptureFrame>& frame) { return AppendPending(acc, session, frame); });
    });
}

struct CopyRegion
{
    UINT x;
    UINT y;
    D3D11_BOX box;
};

[[nodiscard]] UINT ClampedSpan(std::uint32_t textureSpan, std::uint32_t monitorSpan, std::uint32_t available) noexcept
{
    return static_cast<UINT>(std::min(textureSpan, std::min(monitorSpan, available)));
}

[[nodiscard]] std::optional<CopyRegion> RegionOf(const Capture& capture, const Pending& p) noexcept
{
    D3D11_TEXTURE2D_DESC desc{};
    p.texture->GetDesc(&desc);
    const std::int32_t x = p.monitor.rect.Left().Get() - capture.canvasRect.Left().Get();
    const std::int32_t y = p.monitor.rect.Top().Get() - capture.canvasRect.Top().Get();
    return RegionFrom(capture, p, desc, x, y);
}

[[nodiscard]] bool IsOutsideCanvas(std::int32_t x, std::int32_t y) noexcept
{
    return x < 0 || y < 0;
}

[[nodiscard]] std::optional<CopyRegion> RegionFrom(const Capture& capture, const Pending& p, const D3D11_TEXTURE2D_DESC& desc, std::int32_t x, std::int32_t y) noexcept
{
    if (IsOutsideCanvas(x, y))
        return std::nullopt;
    const UINT w = ClampedSpan(desc.Width, static_cast<std::uint32_t>(p.monitor.rect.Right().Get() - p.monitor.rect.Left().Get()), capture.canvasExtent.width.Get() - static_cast<std::uint32_t>(x));
    const UINT h = ClampedSpan(desc.Height, static_cast<std::uint32_t>(p.monitor.rect.Bottom().Get() - p.monitor.rect.Top().Get()), capture.canvasExtent.height.Get() - static_cast<std::uint32_t>(y));
    return CopyRegion{ static_cast<UINT>(x), static_cast<UINT>(y), D3D11_BOX{ 0, 0, 0, w, h, 1 } };
}

void CopyOne(const Capture& capture, const Pending& p) noexcept
{
    const std::optional<CopyRegion> region = RegionOf(capture, p);
    if (region.has_value())
        capture.context->CopySubresourceRegion(capture.canvas.Get(), 0, region->x, region->y, 0, p.texture.Get(), 0, &region->box);
}

void CopyPending(const Capture& capture, const PendingList& pending) noexcept
{
    std::ranges::for_each(pending.Items(), [&capture](const Pending& p) { CopyOne(capture, p); });
}

// Ordering, entirely on the GPU: the queue signals that everything submitted so far has finished with the
// canvas, the capture device waits for that before writing it, and the queue waits for the write to land.
[[nodiscard]] Status<Error> ReleaseCanvas(const Capture& capture, interior::FenceValue value) noexcept
{
    return Check(capture.queue->Signal(capture.sharedCanvasFree.Get(), value.Get()), ApiCall::QueueSignal);
}

[[nodiscard]] Status<Error> AwaitCanvas(const Capture& capture, interior::FenceValue value) noexcept
{
    return Check(capture.context->Wait(capture.canvasFree.Get(), value.Get()), ApiCall::ContextWait);
}

[[nodiscard]] Status<Error> SignalCopied(const Capture& capture, interior::FenceValue value) noexcept
{
    const HRESULT hr = capture.context->Signal(capture.canvasReady.Get(), value.Get());
    capture.context->Flush();
    return Check(hr, ApiCall::ContextSignal);
}

[[nodiscard]] Status<Error> QueueAwaitsCopy(const Capture& capture, interior::FenceValue value) noexcept
{
    return Check(capture.queue->Wait(capture.sharedCanvasReady.Get(), value.Get()), ApiCall::QueueWait);
}

[[nodiscard]] Status<Error> CopiedAndSignalled(const Capture& capture, const PendingList& pending, interior::FenceValue value) noexcept
{
    CopyPending(capture, pending);
    return SignalCopied(capture, value);
}

[[nodiscard]] Status<Error> CopyOrdered(const Capture& capture, const PendingList& pending, interior::FenceValue value) noexcept
{
    return ReleaseCanvas(capture, value).and_then([&] { return AwaitCanvas(capture, value); }).and_then([&] { return CopiedAndSignalled(capture, pending, value); }).and_then([&] {
        return QueueAwaitsCopy(capture, value);
    });
}

[[nodiscard]] Status<Error> CloseAll(const PendingList& pending) noexcept
{
    return infra::ForEach(pending.Items(), Status<Error>{}, [](const Pending& p) { return CloseFrame(p.frame); });
}

[[nodiscard]] Result<bool, Error> CopyAndClose(const Capture& capture, const PendingList& pending, interior::FenceValue value) noexcept
{
    if (pending.IsEmpty())
        return false;
    return CopyOrdered(capture, pending, value).and_then([&pending] { return CloseAll(pending); }).transform([] { return true; });
}

// One fence value per frame, never zero, so the values only ever rise.
[[nodiscard]] interior::FenceValue ValueOf(interior::FrameNumber number) noexcept
{
    return interior::FenceValueTag::Parse(number.Get() + 1);
}

} // namespace

Status<Error> InitializeRuntime() noexcept
{
    const HRESULT hr = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE)
        return {};
    return Check(hr, ApiCall::RoInitialize);
}

Status<Error> RequireCaptureSupport() noexcept
{
    return SessionStatics().and_then([](const Com<WGC::IGraphicsCaptureSessionStatics>& statics) -> Status<Error> {
        boolean supported = 0;
        return Check(statics->IsSupported(&supported), ApiCall::IsCaptureSupported).and_then([supported]() -> Status<Error> {
            if (supported == 0)
                return Fail(Error{ ApiCall::IsCaptureSupported, 0 });
            return {};
        });
    });
}

Result<Capture, Error> CreateCapture(const GpuDevice& gpu, const interior::ScreenRect& canvasRect, const interior::Extent& canvasExtent, const interior::MonitorList& monitors,
                                     const CaptureSettings& settings) noexcept
{
    return CreateDevices(gpu).and_then([&](const Devices& d) {
        return CreateBridge(d, gpu, canvasExtent).and_then([&](const Bridge& bridge) {
            return StartAll(d.winrtDevice.Get(), monitors, settings).transform([&](const Sessions& sessions) {
                return Capture{ d.device11, d.context, d.winrtDevice, bridge.canvas, bridge.sharedCanvas, bridge.canvasFree, bridge.sharedCanvasFree, bridge.canvasReady, bridge.sharedCanvasReady,
                                gpu.queue,  sessions,  canvasRect,    canvasExtent };
            });
        });
    });
}

Result<bool, Error> AcquireFrames(const Capture& capture, interior::FrameNumber number) noexcept
{
    return CollectPending(capture).and_then([&](const PendingList& pending) { return CopyAndClose(capture, pending, ValueOf(number)); });
}

} // namespace real
