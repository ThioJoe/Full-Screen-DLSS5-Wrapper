#include "effects/real/capture.h"

#include "effects/real/exclusion.h"

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

[[nodiscard]] Status<Error> ApplyCursor(const Com<WGC::IGraphicsCaptureSession>& session, bool cursor) noexcept
{
    return As<WGC::IGraphicsCaptureSession2>(session, ApiCall::PutIsCursorCaptureEnabled).and_then([cursor](const Com<WGC::IGraphicsCaptureSession2>& s2) {
        return Check(s2->put_IsCursorCaptureEnabled(cursor ? 1 : 0), ApiCall::PutIsCursorCaptureEnabled);
    });
}

// Whether to draw the border the system puts around what is being captured is a setting Windows has only
// from build 20348; an older one does not carry the interface at all and draws its border whatever is
// asked. Nothing but that border turns on it, so a Windows without the setting is not a refusal: the
// session runs and says so, which is what the answer carries.
[[nodiscard]] Result<bool, Error> AppliedBorder(const Com<WGC::IGraphicsCaptureSession>& session, bool border) noexcept
{
    Com<WGC::IGraphicsCaptureSession3> settable; // WAIVER(R2): the answer of one query, read once after it.
    if (FAILED(session.As(&settable)))
        return false;
    return Check(settable->put_IsBorderRequired(border ? 1 : 0), ApiCall::PutIsBorderRequired).transform([] { return true; });
}

// A session that can be told to leave our own windows out is told before it starts, so no frame it ever
// delivers holds them. Whether it agreed is carried out, because uncovering the windows depends on it.
struct Started
{
    Com<WGC::IGraphicsCaptureSession> session;
    bool excluding;
    bool controlsBorder;
};

using Sessions = infra::BoundedVector<MonitorSession, interior::kMaxMonitors>;

struct Devices
{
    Com<ID3D11Device> device11;
    Com<ID3D11DeviceContext4> context;
    Com<WGD11::IDirect3DDevice> winrtDevice;
};

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

struct SharedCanvas
{
    Com<ID3D11Texture2D> canvas;
    Com<ID3D12Resource> shared;
};

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

struct Pending
{
    Com<WGC::IDirect3D11CaptureFrame> frame;
    Com<ID3D11Texture2D> texture;
    interior::MonitorInfo monitor;
};

using PendingList = infra::BoundedVector<Pending, interior::kMaxMonitors>;

struct CopyRegion
{
    UINT x;
    UINT y;
    D3D11_BOX box;
};

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
    static constexpr auto SessionStatics = [] [[nodiscard]] () noexcept -> Result<Com<WGC::IGraphicsCaptureSessionStatics>, Error> {
        Com<WGC::IGraphicsCaptureSessionStatics> statics;
        return ActivationFactory(kSessionClass, IID_PPV_ARGS(&statics)).transform([&statics] { return statics; });
    };
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
                                     const CaptureSettings& settings, std::span<const HWND> ours) noexcept
{
    static constexpr auto StartAll = [] [[nodiscard]] (WGD11::IDirect3DDevice * device, const interior::MonitorList& monitors, const CaptureSettings& settings,
                                                       std::span<const HWND> ours) noexcept -> Result<Sessions, Error> {
        static constexpr auto StartSession = [] [[nodiscard]] (WGD11::IDirect3DDevice * device, const interior::MonitorInfo& monitor, const CaptureSettings& settings,
                                                               std::span<const HWND> ours) noexcept -> Result<MonitorSession, Error> {
            static constexpr auto ItemFor = [] [[nodiscard]] (const interior::MonitorInfo& monitor) noexcept -> Result<Com<WGC::IGraphicsCaptureItem>, Error> {
                static constexpr auto ItemInterop = [] [[nodiscard]] () noexcept -> Result<Com<IGraphicsCaptureItemInterop>, Error> {
                    Com<IGraphicsCaptureItemInterop> interop;
                    return ActivationFactory(kItemClass, IID_PPV_ARGS(&interop)).transform([&interop] { return interop; });
                };

                // A window is asked for by its own handle, which gives its content whatever is in front of it, and follows
                // it as it moves. A monitor is asked for by monitor. Neither opens anything belonging to another process.
                static constexpr auto ItemOf = [] [[nodiscard]] (const Com<IGraphicsCaptureItemInterop>& interop, const interior::MonitorInfo& source,
                                                                 Com<WGC::IGraphicsCaptureItem>& item) noexcept -> HRESULT {
                    if (source.kind == interior::SourceKind::Window)
                        return interop->CreateForWindow(reinterpret_cast<HWND>(source.handle.Get()), IID_PPV_ARGS(&item));
                    return interop->CreateForMonitor(reinterpret_cast<HMONITOR>(source.handle.Get()), IID_PPV_ARGS(&item));
                };
                return ItemInterop().and_then([&monitor](const Com<IGraphicsCaptureItemInterop>& interop) {
                    Com<WGC::IGraphicsCaptureItem> item;
                    return Check(ItemOf(interop, monitor, item), ApiCall::CreateForMonitor).transform([&item] { return item; });
                });
            };

            static constexpr auto PoolFor = [] [[nodiscard]] (WGD11::IDirect3DDevice * device, WGC::IGraphicsCaptureItem * item) noexcept -> Result<Com<WGC::IDirect3D11CaptureFramePool>, Error> {
                static constexpr auto PoolStatics = [] [[nodiscard]] () noexcept -> Result<Com<WGC::IDirect3D11CaptureFramePoolStatics2>, Error> {
                    Com<WGC::IDirect3D11CaptureFramePoolStatics2> statics;
                    return ActivationFactory(kPoolClass, IID_PPV_ARGS(&statics)).transform([&statics] { return statics; });
                };

                static constexpr auto SizeOf = [] [[nodiscard]] (WGC::IGraphicsCaptureItem * item) noexcept -> Result<ABI::Windows::Graphics::SizeInt32, Error> {
                    ABI::Windows::Graphics::SizeInt32 size{};
                    return Check(item->get_Size(&size), ApiCall::GetContentSize).transform([&size] { return size; });
                };

                // A window that is closing answers with a size of nothing, and a pool of that size is refused with an
                // invalid argument. It is caught here so the session can fall back rather than fail with a dialog.
                static constexpr auto PoolOf = [] [[nodiscard]] (WGD11::IDirect3DDevice * device, WGC::IDirect3D11CaptureFramePoolStatics2 * statics,
                                                                 ABI::Windows::Graphics::SizeInt32 size) noexcept -> Result<Com<WGC::IDirect3D11CaptureFramePool>, Error> {
                    static constexpr auto HasArea = [] [[nodiscard]] (ABI::Windows::Graphics::SizeInt32 size) noexcept -> bool { return size.Width > 0 && size.Height > 0; };
                    if (!HasArea(size))
                        return Fail(Error{ ApiCall::CreateFreeThreaded, 1 });
                    Com<WGC::IDirect3D11CaptureFramePool> pool;
                    const HRESULT hr = statics->CreateFreeThreaded(device, WGD::DirectXPixelFormat_B8G8R8A8UIntNormalized, kPoolBuffers, size, &pool);
                    return Check(hr, ApiCall::CreateFreeThreaded).transform([&pool] { return pool; });
                };
                return PoolStatics().and_then([&](const Com<WGC::IDirect3D11CaptureFramePoolStatics2>& statics) {
                    return SizeOf(item).and_then([&](ABI::Windows::Graphics::SizeInt32 size) { return PoolOf(device, statics.Get(), size); });
                });
            };

            static constexpr auto SessionFor = [] [[nodiscard]] (WGC::IDirect3D11CaptureFramePool * pool, WGC::IGraphicsCaptureItem * item, const CaptureSettings& settings,
                                                                 std::span<const HWND> ours) noexcept -> Result<Started, Error> {
                // The list is handed over twice, because a session that takes it while stopped need not be the one that
                // reads it while running. Either time sticking is enough for the windows to be uncovered.
                static constexpr auto ExcludedAgain = [] [[nodiscard]] (WGC::IGraphicsCaptureSession * session, std::span<const HWND> ours, bool before) noexcept -> bool {
                    NoteExclusionList(session, "--- after StartCapture");
                    const bool after = ExcludeWindowsFrom(session, ours);
                    return after || before;
                };
                Com<WGC::IGraphicsCaptureSession> session;
                bool excluding = false;      // WAIVER(R2): the answer of one call, read once after it.
                bool controlsBorder = false; // WAIVER(R2): the answer of one call, read once after it.
                return Check(pool->CreateCaptureSession(item, &session), ApiCall::CreateCaptureSession)
                    .and_then([&] { return ApplyCursor(session, settings.cursor); })
                    .and_then([&] { return AppliedBorder(session, settings.border); })
                    .and_then([&](bool controlled) {
                        controlsBorder = controlled;
                        excluding = ExcludeWindowsFrom(session.Get(), ours);
                        return Check(session->StartCapture(), ApiCall::StartCapture);
                    })
                    .transform([&] { return Started{ session, ExcludedAgain(session.Get(), ours, excluding), controlsBorder }; });
            };
            return ItemFor(monitor).and_then([&](const Com<WGC::IGraphicsCaptureItem>& item) {
                return PoolFor(device, item.Get()).and_then([&](const Com<WGC::IDirect3D11CaptureFramePool>& pool) {
                    return SessionFor(pool.Get(), item.Get(), settings, ours).transform([&](const Started& started) {
                        return MonitorSession{ item, pool, started.session, monitor, started.excluding, started.controlsBorder };
                    });
                });
            });
        };
        return infra::FoldResult(monitors.Items(), Result<Sessions, Error>(Sessions{}), [&](const Sessions& acc, const interior::MonitorInfo& monitor) {
            return StartSession(device, monitor, settings, ours).and_then([&acc](const MonitorSession& s) {
                return acc.Push(s).transform_error([](infra::CapacityExceeded) { return Error{ ApiCall::CreateCaptureSession, 1 }; });
            });
        });
    };

    static constexpr auto CreateDevices = [] [[nodiscard]] (const GpuDevice& gpu) noexcept -> Result<Devices, Error> {
        static constexpr auto CreateDevice11 = [] [[nodiscard]] (const GpuDevice& gpu, Com<ID3D11DeviceContext>& context) noexcept -> Result<Com<ID3D11Device>, Error> {
            Com<ID3D11Device> device;
            const HRESULT hr = ::D3D11CreateDevice(gpu.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
            return Check(hr, ApiCall::D3D11CreateDevice).transform([&device] { return device; });
        };

        // The free-threaded pool uses the context from the capture service's thread, so it is protected.
        static constexpr auto ProtectContext = [] [[nodiscard]] (const Com<ID3D11DeviceContext>& context) noexcept -> Status<Error> {
            return As<ID3D11Multithread>(context, ApiCall::QueryInterface).transform([](const Com<ID3D11Multithread>& multithread) { multithread->SetMultithreadProtected(TRUE); });
        };

        static constexpr auto WinrtDeviceOf = [] [[nodiscard]] (const Com<ID3D11Device>& device11) noexcept -> Result<Com<WGD11::IDirect3DDevice>, Error> {
            return As<IDXGIDevice>(device11, ApiCall::QueryInterface).and_then([](const Com<IDXGIDevice>& dxgi) {
                Com<IInspectable> inspectable;
                const HRESULT hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), &inspectable);
                return Check(hr, ApiCall::CreateDirect3D11DeviceFromDXGIDevice).and_then([&inspectable] { return As<WGD11::IDirect3DDevice>(inspectable, ApiCall::QueryInterface); });
            });
        };
        Com<ID3D11DeviceContext> context;
        return CreateDevice11(gpu, context).and_then([&](const Com<ID3D11Device>& device11) {
            return ProtectContext(context).and_then([&] { return As<ID3D11DeviceContext4>(context, ApiCall::QueryInterface); }).and_then([&](const Com<ID3D11DeviceContext4>& context4) {
                return WinrtDeviceOf(device11).transform([&](const Com<WGD11::IDirect3DDevice>& winrt) { return Devices{ device11, context4, winrt }; });
            });
        });
    };

    static constexpr auto CreateBridge = [] [[nodiscard]] (const Devices& d, const GpuDevice& gpu, const interior::Extent& extent) noexcept -> Result<Bridge, Error> {
        static constexpr auto SharedFenceFor = [] [[nodiscard]] (const Devices& d, const GpuDevice& gpu) noexcept -> Result<SharedFence, Error> {
            static constexpr auto CreateSharedFence = [] [[nodiscard]] (const Com<ID3D11Device>& device11) noexcept -> Result<Com<ID3D11Fence>, Error> {
                return As<ID3D11Device5>(device11, ApiCall::QueryInterface).and_then([](const Com<ID3D11Device5>& device5) -> Result<Com<ID3D11Fence>, Error> {
                    Com<ID3D11Fence> fence;
                    return Check(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), ApiCall::D3D11CreateFence).transform([&fence] { return fence; });
                });
            };

            static constexpr auto FenceHandle = [] [[nodiscard]] (const Com<ID3D11Fence>& fence) noexcept -> Result<UniqueHandle, Error> {
                HANDLE handle = nullptr;
                return Check(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), ApiCall::CreateSharedHandle).transform([handle] { return UniqueHandle(handle); });
            };

            // WAIVER(R7): the same open over another interface; a template here would put metaprogramming outside infrastructure.
            static constexpr auto OpenedFence = [] [[nodiscard]] (const GpuDevice& gpu, const UniqueHandle& handle) noexcept -> Result<Com<ID3D12Fence>, Error> {
                Com<ID3D12Fence> opened;
                return Check(gpu.device->OpenSharedHandle(handle.get(), IID_PPV_ARGS(&opened)), ApiCall::OpenSharedHandle).transform([&opened] { return opened; });
            };
            return CreateSharedFence(d.device11).and_then([&](const Com<ID3D11Fence>& fence) {
                return FenceHandle(fence).and_then(
                    [&](const UniqueHandle& handle) { return OpenedFence(gpu, handle).transform([&fence](const Com<ID3D12Fence>& shared) { return SharedFence{ fence, shared }; }); });
            });
        };

        static constexpr auto SharedCanvasFor = [] [[nodiscard]] (const Devices& d, const GpuDevice& gpu, const interior::Extent& extent) noexcept -> Result<SharedCanvas, Error> {
            static constexpr auto CreateCanvas = [] [[nodiscard]] (const Com<ID3D11Device>& device11, const interior::Extent& extent) noexcept -> Result<Com<ID3D11Texture2D>, Error> {
                static constexpr auto CanvasDescription = [] [[nodiscard]] (const interior::Extent& extent) noexcept -> D3D11_TEXTURE2D_DESC {
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
                };
                const D3D11_TEXTURE2D_DESC desc = CanvasDescription(extent);
                Com<ID3D11Texture2D> canvas;
                return Check(device11->CreateTexture2D(&desc, nullptr, &canvas), ApiCall::CreateTexture2D).transform([&canvas] { return canvas; });
            };

            static constexpr auto ResourceHandle = [] [[nodiscard]] (const Com<ID3D11Texture2D>& canvas) noexcept -> Result<UniqueHandle, Error> {
                return As<IDXGIResource1>(canvas, ApiCall::QueryInterface).and_then([](const Com<IDXGIResource1>& resource) -> Result<UniqueHandle, Error> {
                    HANDLE handle = nullptr;
                    const HRESULT hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
                    return Check(hr, ApiCall::CreateSharedHandle).transform([handle] { return UniqueHandle(handle); });
                });
            };

            static constexpr auto OpenedCanvas = [] [[nodiscard]] (const GpuDevice& gpu, const UniqueHandle& handle) noexcept -> Result<Com<ID3D12Resource>, Error> {
                Com<ID3D12Resource> opened;
                return Check(gpu.device->OpenSharedHandle(handle.get(), IID_PPV_ARGS(&opened)), ApiCall::OpenSharedHandle).transform([&opened] { return opened; });
            };
            return CreateCanvas(d.device11, extent).and_then([&](const Com<ID3D11Texture2D>& canvas) {
                return ResourceHandle(canvas).and_then(
                    [&](const UniqueHandle& handle) { return OpenedCanvas(gpu, handle).transform([&canvas](const Com<ID3D12Resource>& shared) { return SharedCanvas{ canvas, shared }; }); });
            });
        };
        return SharedCanvasFor(d, gpu, extent).and_then([&](const SharedCanvas& canvas) {
            return SharedFenceFor(d, gpu).and_then([&](const SharedFence& free) {
                return SharedFenceFor(d, gpu).transform([&](const SharedFence& ready) { return Bridge{ canvas.canvas, canvas.shared, free.fence, free.shared, ready.fence, ready.shared }; });
            });
        });
    };

    // Our windows stay covered unless every session agreed to leave them out: one that did not would capture
    // them, and the model would be fed its own answer.
    static constexpr auto AllExcluding = [] [[nodiscard]] (const Sessions& sessions) noexcept -> bool {
        return !sessions.IsEmpty() && std::ranges::all_of(sessions.Items(), [](const MonitorSession& s) { return s.excluding; });
    };

    // The border is ours to decide only where every session's Windows carries the setting.
    static constexpr auto AllControllingBorder = [] [[nodiscard]] (const Sessions& sessions) noexcept -> bool {
        return !sessions.IsEmpty() && std::ranges::all_of(sessions.Items(), [](const MonitorSession& s) { return s.controlsBorder; });
    };
    return CreateDevices(gpu).and_then([&](const Devices& d) {
        return CreateBridge(d, gpu, canvasExtent).and_then([&](const Bridge& bridge) {
            return StartAll(d.winrtDevice.Get(), monitors, settings, ours).transform([&](const Sessions& sessions) {
                // WAIVER(R1): every field named, so a field added later cannot quietly take another's place.
                return Capture{ .device11 = d.device11,
                                .context = d.context,
                                .winrtDevice = d.winrtDevice,
                                .canvas = bridge.canvas,
                                .sharedCanvas = bridge.sharedCanvas,
                                .canvasFree = bridge.canvasFree,
                                .sharedCanvasFree = bridge.sharedCanvasFree,
                                .canvasReady = bridge.canvasReady,
                                .sharedCanvasReady = bridge.sharedCanvasReady,
                                .queue = gpu.queue,
                                .sessions = sessions,
                                .canvasRect = canvasRect,
                                .canvasExtent = canvasExtent,
                                .excludesOurWindows = AllExcluding(sessions),
                                .controlsBorder = AllControllingBorder(sessions) };
            });
        });
    });
}

Status<Error> ApplyCaptureSettings(const Capture& capture, const CaptureSettings& settings) noexcept
{
    return infra::ForEach(capture.sessions.Items(), Status<Error>{}, [&settings](const MonitorSession& session) {
        return ApplyCursor(session.session, settings.cursor).and_then([&] { return AppliedBorder(session.session, settings.border).transform([](bool) {}); });
    });
}

Result<bool, Error> AcquireFrames(const Capture& capture, interior::FrameNumber number) noexcept
{
    static constexpr auto CollectPending = [] [[nodiscard]] (const Capture& capture) noexcept -> Result<PendingList, Error> {
        static constexpr auto LatestFrame = [] [[nodiscard]] (WGC::IDirect3D11CaptureFramePool * pool) noexcept -> Result<Com<WGC::IDirect3D11CaptureFrame>, Error> {
            static constexpr auto DrainOne = [] [[nodiscard]] (const Drain& d, WGC::IDirect3D11CaptureFramePool* pool) noexcept -> Result<Drain, Error> {
                static constexpr auto Received = [] [[nodiscard]] (const Drain& d, const Com<WGC::IDirect3D11CaptureFrame>& next) noexcept -> Result<Drain, Error> {
                    static constexpr auto Replace = [] [[nodiscard]] (const Drain& d, const Com<WGC::IDirect3D11CaptureFrame>& next) noexcept -> Result<Drain, Error> {
                        return CloseFrame(d.latest).transform([&next] { return Drain{ next, false }; });
                    };
                    if (!next)
                        return Drain{ d.latest, true };
                    return Replace(d, next);
                };
                if (d.done)
                    return d;
                Com<WGC::IDirect3D11CaptureFrame> next;
                return Check(pool->TryGetNextFrame(&next), ApiCall::TryGetNextFrame).and_then([&] { return Received(d, next); });
            };
            return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, kMaxFramesDrained), Result<Drain, Error>(Drain{ nullptr, false }),
                                     [pool](const Drain& d, std::uint32_t) { return DrainOne(d, pool); })
                .transform([](const Drain& d) { return d.latest; });
        };

        static constexpr auto AppendPending = [] [[nodiscard]] (const PendingList& acc, const MonitorSession& session,
                                                                const Com<WGC::IDirect3D11CaptureFrame>& frame) noexcept -> Result<PendingList, Error> {
            static constexpr auto TextureOf = [] [[nodiscard]] (WGC::IDirect3D11CaptureFrame * frame) noexcept -> Result<Com<ID3D11Texture2D>, Error> {
                Com<WGD11::IDirect3DSurface> surface;
                return Check(frame->get_Surface(&surface), ApiCall::GetSurface)
                    .and_then([&] { return As<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>(surface, ApiCall::GetInterface); })
                    .and_then([](const Com<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>& access) -> Result<Com<ID3D11Texture2D>, Error> {
                        Com<ID3D11Texture2D> texture;
                        return Check(access->GetInterface(IID_PPV_ARGS(&texture)), ApiCall::GetInterface).transform([&texture] { return texture; });
                    });
            };
            if (!frame)
                return acc;
            NoteFrameConfiguration(frame.Get());
            return TextureOf(frame.Get()).and_then([&](const Com<ID3D11Texture2D>& texture) {
                return acc.Push(Pending{ frame, texture, session.monitor }).transform_error([](infra::CapacityExceeded) { return Error{ ApiCall::TryGetNextFrame, 1 }; });
            });
        };
        return infra::FoldResult(capture.sessions.Items(), Result<PendingList, Error>(PendingList{}), [](const PendingList& acc, const MonitorSession& session) {
            return LatestFrame(session.pool.Get()).and_then([&](const Com<WGC::IDirect3D11CaptureFrame>& frame) { return AppendPending(acc, session, frame); });
        });
    };

    static constexpr auto CopyAndClose = [] [[nodiscard]] (const Capture& capture, const PendingList& pending, interior::FenceValue value) noexcept -> Result<bool, Error> {
        static constexpr auto CopyOrdered = [] [[nodiscard]] (const Capture& capture, const PendingList& pending, interior::FenceValue value) noexcept -> Status<Error> {
            // Ordering, entirely on the GPU: the queue signals that everything submitted so far has finished with the
            // canvas, the capture device waits for that before writing it, and the queue waits for the write to land.
            static constexpr auto ReleaseCanvas = [] [[nodiscard]] (const Capture& capture, interior::FenceValue value) noexcept -> Status<Error> {
                return Check(capture.queue->Signal(capture.sharedCanvasFree.Get(), value.Get()), ApiCall::QueueSignal);
            };

            static constexpr auto AwaitCanvas = [] [[nodiscard]] (const Capture& capture, interior::FenceValue value) noexcept -> Status<Error> {
                return Check(capture.context->Wait(capture.canvasFree.Get(), value.Get()), ApiCall::ContextWait);
            };

            static constexpr auto QueueAwaitsCopy = [] [[nodiscard]] (const Capture& capture, interior::FenceValue value) noexcept -> Status<Error> {
                return Check(capture.queue->Wait(capture.sharedCanvasReady.Get(), value.Get()), ApiCall::QueueWait);
            };

            static constexpr auto CopiedAndSignalled = [] [[nodiscard]] (const Capture& capture, const PendingList& pending, interior::FenceValue value) noexcept -> Status<Error> {
                static constexpr auto CopyPending = [](const Capture& capture, const PendingList& pending) noexcept -> void {
                    static constexpr auto CopyOne = [](const Capture& capture, const Pending& p) noexcept -> void {
                        static constexpr auto RegionOf = [] [[nodiscard]] (const Capture& capture, const Pending& p) noexcept -> std::optional<CopyRegion> {
                            static constexpr auto RegionFrom = [] [[nodiscard]] (const Capture& capture, const Pending& p, const D3D11_TEXTURE2D_DESC& desc, std::int32_t x,
                                                                                 std::int32_t y) noexcept -> std::optional<CopyRegion> {
                                static constexpr auto ClampedSpan = [] [[nodiscard]] (std::uint32_t textureSpan, std::uint32_t monitorSpan, std::uint32_t available) noexcept -> UINT {
                                    return static_cast<UINT>(std::min(textureSpan, std::min(monitorSpan, available)));
                                };

                                static constexpr auto IsOutsideCanvas = [] [[nodiscard]] (std::int32_t x, std::int32_t y) noexcept -> bool { return x < 0 || y < 0; };
                                if (IsOutsideCanvas(x, y))
                                    return std::nullopt;
                                const UINT w = ClampedSpan(desc.Width, static_cast<std::uint32_t>(p.monitor.rect.Right().Get() - p.monitor.rect.Left().Get()),
                                                           capture.canvasExtent.width.Get() - static_cast<std::uint32_t>(x));
                                const UINT h = ClampedSpan(desc.Height, static_cast<std::uint32_t>(p.monitor.rect.Bottom().Get() - p.monitor.rect.Top().Get()),
                                                           capture.canvasExtent.height.Get() - static_cast<std::uint32_t>(y));
                                return CopyRegion{ static_cast<UINT>(x), static_cast<UINT>(y), D3D11_BOX{ 0, 0, 0, w, h, 1 } };
                            };
                            D3D11_TEXTURE2D_DESC desc{};
                            p.texture->GetDesc(&desc);
                            const std::int32_t x = p.monitor.rect.Left().Get() - capture.canvasRect.Left().Get();
                            const std::int32_t y = p.monitor.rect.Top().Get() - capture.canvasRect.Top().Get();
                            return RegionFrom(capture, p, desc, x, y);
                        };
                        const std::optional<CopyRegion> region = RegionOf(capture, p);
                        if (region.has_value())
                            capture.context->CopySubresourceRegion(capture.canvas.Get(), 0, region->x, region->y, 0, p.texture.Get(), 0, &region->box);
                    };
                    std::ranges::for_each(pending.Items(), [&capture](const Pending& p) { CopyOne(capture, p); });
                };

                static constexpr auto SignalCopied = [] [[nodiscard]] (const Capture& capture, interior::FenceValue value) noexcept -> Status<Error> {
                    const HRESULT hr = capture.context->Signal(capture.canvasReady.Get(), value.Get());
                    capture.context->Flush();
                    return Check(hr, ApiCall::ContextSignal);
                };
                CopyPending(capture, pending);
                return SignalCopied(capture, value);
            };
            return ReleaseCanvas(capture, value).and_then([&] { return AwaitCanvas(capture, value); }).and_then([&] { return CopiedAndSignalled(capture, pending, value); }).and_then([&] {
                return QueueAwaitsCopy(capture, value);
            });
        };

        static constexpr auto CloseAll = [] [[nodiscard]] (const PendingList& pending) noexcept -> Status<Error> {
            return infra::ForEach(pending.Items(), Status<Error>{}, [](const Pending& p) { return CloseFrame(p.frame); });
        };
        if (pending.IsEmpty())
            return false;
        return CopyOrdered(capture, pending, value).and_then([&pending] { return CloseAll(pending); }).transform([] { return true; });
    };

    // One fence value per frame, never zero, so the values only ever rise.
    static constexpr auto ValueOf = [] [[nodiscard]] (interior::FrameNumber number) noexcept -> interior::FenceValue { return interior::FenceValueTag::Parse(number.Get() + 1); };
    return CollectPending(capture).and_then([&](const PendingList& pending) { return CopyAndClose(capture, pending, ValueOf(number)); });
}

} // namespace real
