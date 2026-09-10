#include "effects/real/presenter.h"

#include "effects/real/exclusion.h"

#include "effects/real/resources.h"
#include "infrastructure/fold.h"

#include <ranges>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;

[[nodiscard]] DXGI_SWAP_CHAIN_DESC1 SwapChainDescription(const interior::Extent& extent) noexcept
{
    return DXGI_SWAP_CHAIN_DESC1{ extent.width.Get(),
                                  extent.height.Get(),
                                  kSwapChainFormat,
                                  FALSE,
                                  DXGI_SAMPLE_DESC{ 1, 0 },
                                  DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                  interior::kBackBufferCount,
                                  DXGI_SCALING_STRETCH,
                                  DXGI_SWAP_EFFECT_FLIP_DISCARD,
                                  DXGI_ALPHA_MODE_IGNORE,
                                  DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT };
}

[[nodiscard]] Result<Com<IDXGISwapChain1>, Error> CreateCompositionSwapChain(const GpuDevice& gpu, const interior::Extent& extent) noexcept
{
    const DXGI_SWAP_CHAIN_DESC1 desc = SwapChainDescription(extent);
    Com<IDXGISwapChain1> chain;
    const HRESULT hr = gpu.factory->CreateSwapChainForComposition(gpu.queue.Get(), &desc, nullptr, &chain);
    return Check(hr, ApiCall::CreateSwapChainForComposition).transform([&chain] { return chain; });
}

// Bound to the window rather than composited over it, so the picture is the window's own content. A
// capture told to leave the window out then leaves the picture out; over a composition it does not.
[[nodiscard]] Result<Com<IDXGISwapChain1>, Error> CreateWindowSwapChain(const GpuDevice& gpu, HWND window, const interior::Extent& extent) noexcept
{
    const DXGI_SWAP_CHAIN_DESC1 desc = SwapChainDescription(extent);
    Com<IDXGISwapChain1> chain;
    const HRESULT hr = gpu.factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &chain);
    return Check(hr, ApiCall::CreateSwapChainForComposition).transform([&chain] { return chain; });
}

struct Chain
{
    Com<IDXGISwapChain1> chain;
    bool ownContent;
};

[[nodiscard]] Result<Chain, Error> AsComposition(const GpuDevice& gpu, const interior::Extent& extent) noexcept
{
    return CreateCompositionSwapChain(gpu, extent).transform([](const Com<IDXGISwapChain1>& made) { return Chain{ made, false }; });
}

// A layered window may refuse a swap chain of its own, and clicking through to another program needs the
// layering. Where it refuses, the composition is used and the picture stays inside our own capture.
[[nodiscard]] Result<Chain, Error> TriedOnWindow(const GpuDevice& gpu, HWND window, const interior::Extent& extent) noexcept
{
    const Result<Com<IDXGISwapChain1>, Error> made = CreateWindowSwapChain(gpu, window, extent);
    if (made.has_value())
        return Chain{ *made, true };
    NoteExclusion("the window refused a swap chain of its own; presenting a composition over it instead");
    return AsComposition(gpu, extent);
}

[[nodiscard]] Result<Chain, Error> CreateChainFor(const GpuDevice& gpu, HWND window, const interior::Extent& extent, bool ownContent) noexcept
{
    if (ownContent)
        return TriedOnWindow(gpu, window, extent);
    return AsComposition(gpu, extent);
}

[[nodiscard]] Result<Com<IDXGISwapChain3>, Error> WithLatencyOne(const Com<IDXGISwapChain3>& chain) noexcept
{
    return Check(chain->SetMaximumFrameLatency(1), ApiCall::SetMaximumFrameLatency).transform([&chain] { return chain; });
}

struct Ready
{
    Com<IDXGISwapChain3> chain;
    bool ownContent;
};

[[nodiscard]] Result<Ready, Error> CreateSwapChain(const GpuDevice& gpu, HWND window, const interior::Extent& extent, bool ownContent) noexcept
{
    return CreateChainFor(gpu, window, extent, ownContent).and_then([](const Chain& made) {
        return As<IDXGISwapChain3>(made.chain, ApiCall::QueryInterface).and_then(WithLatencyOne).transform([&made](const Com<IDXGISwapChain3>& three) { return Ready{ three, made.ownContent }; });
    });
}

[[nodiscard]] Result<UniqueHandle, Error> WaitableOf(IDXGISwapChain3* chain) noexcept
{
    HANDLE handle = chain->GetFrameLatencyWaitableObject();
    if (handle == nullptr)
        return Fail(Error{ ApiCall::WaitForFrame, 0 });
    return UniqueHandle(handle);
}

struct Composition
{
    Com<IDCompositionDevice> device;
    Com<IDCompositionTarget> target;
    Com<IDCompositionVisual> visual;
};

[[nodiscard]] Result<Com<IDCompositionDevice>, Error> CreateCompositionDevice() noexcept
{
    Com<IDCompositionDevice> device;
    const HRESULT hr = ::DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&device));
    return Check(hr, ApiCall::DCompositionCreateDevice2).transform([&device] { return device; });
}

[[nodiscard]] Result<Com<IDCompositionTarget>, Error> CreateTarget(IDCompositionDevice* device, HWND window) noexcept
{
    Com<IDCompositionTarget> target;
    const HRESULT hr = device->CreateTargetForHwnd(window, TRUE, &target);
    return Check(hr, ApiCall::CreateTargetForHwnd).transform([&target] { return target; });
}

[[nodiscard]] Result<Com<IDCompositionVisual>, Error> CreateVisual(IDCompositionDevice* device) noexcept
{
    Com<IDCompositionVisual> visual;
    const HRESULT hr = device->CreateVisual(&visual);
    return Check(hr, ApiCall::CreateVisual).transform([&visual] { return visual; });
}

[[nodiscard]] Status<Error> Bind(const Composition& c, IDXGISwapChain3* chain) noexcept
{
    return Check(c.visual->SetContent(chain), ApiCall::SetContent).and_then([&c] { return Check(c.target->SetRoot(c.visual.Get()), ApiCall::SetRoot); }).and_then([&c] {
        return Check(c.device->Commit(), ApiCall::Commit);
    });
}

[[nodiscard]] Result<Composition, Error> Bound(const Composition& c, IDXGISwapChain3* chain) noexcept
{
    return Bind(c, chain).transform([&c] { return c; });
}

[[nodiscard]] Result<Composition, Error> CreateComposition(HWND window, IDXGISwapChain3* chain) noexcept
{
    return CreateCompositionDevice().and_then([window, chain](const Com<IDCompositionDevice>& device) {
        return CreateTarget(device.Get(), window).and_then([&](const Com<IDCompositionTarget>& target) {
            return CreateVisual(device.Get()).and_then([&](const Com<IDCompositionVisual>& visual) { return Bound(Composition{ device, target, visual }, chain); });
        });
    });
}

[[nodiscard]] Result<Com<ID3D12Resource>, Error> NamedBuffer(const Com<ID3D12Resource>& buffer) noexcept
{
    return Check(buffer->SetName(L"Back buffer"), ApiCall::GetBuffer).transform([&buffer] { return buffer; });
}

[[nodiscard]] Result<Com<ID3D12Resource>, Error> BufferAt(IDXGISwapChain3* chain, std::uint32_t index) noexcept
{
    Com<ID3D12Resource> buffer;
    const HRESULT hr = chain->GetBuffer(index, IID_PPV_ARGS(&buffer));
    return Check(hr, ApiCall::GetBuffer).and_then([&buffer] { return NamedBuffer(buffer); });
}

[[nodiscard]] Result<BackBuffers, Error> WithBuffer(const BackBuffers& buffers, const GpuDevice& gpu, IDXGISwapChain3* chain, std::uint32_t index) noexcept
{
    return BufferAt(chain, index).transform([&](const Com<ID3D12Resource>& buffer) {
        CreateRtv(gpu, buffer.Get(), kSwapChainFormat, RtvHandle(gpu, index));
        return infra::WithElement(buffers, index, buffer);
    });
}

[[nodiscard]] Result<BackBuffers, Error> CollectBuffers(const GpuDevice& gpu, IDXGISwapChain3* chain) noexcept
{
    return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, interior::kBackBufferCount), Result<BackBuffers, Error>(BackBuffers{}),
                             [&](const BackBuffers& acc, std::uint32_t index) { return WithBuffer(acc, gpu, chain, index); });
}

[[nodiscard]] Result<Presenter, Error> Rest(const GpuDevice& gpu, const Composition& composition, const Com<IDXGISwapChain3>& chain, const interior::Extent& extent) noexcept
{
    return WaitableOf(chain.Get()).and_then([&](UniqueHandle waitable) {
        return CollectBuffers(gpu, chain.Get()).transform([&](const BackBuffers& buffers) {
            return Presenter{ chain, composition.device, composition.target, composition.visual, std::move(waitable), buffers, extent };
        });
    });
}

// A window swap chain needs no composition of its own: the window already shows what is presented to it.
[[nodiscard]] Result<Presenter, Error> Assemble(const GpuDevice& gpu, HWND window, const Com<IDXGISwapChain3>& chain, const interior::Extent& extent, bool ownContent) noexcept
{
    if (ownContent)
        return Rest(gpu, Composition{ nullptr, nullptr, nullptr }, chain, extent);
    return CreateComposition(window, chain.Get()).and_then([&](const Composition& composition) { return Rest(gpu, composition, chain, extent); });
}

[[nodiscard]] bool IsWaitFailure(DWORD result) noexcept
{
    return result == WAIT_FAILED;
}

} // namespace

Result<Presenter, Error> CreatePresenter(const GpuDevice& gpu, HWND window, const interior::Extent& extent, bool ownContent) noexcept
{
    return CreateSwapChain(gpu, window, extent, ownContent).and_then([&](const Ready& made) { return Assemble(gpu, window, made.chain, extent, made.ownContent); });
}

Status<Error> WaitForNextFrame(const Presenter& presenter) noexcept
{
    const DWORD result = ::WaitForSingleObjectEx(presenter.waitable.get(), static_cast<DWORD>(kFrameWaitMicroseconds / 1000u), TRUE);
    if (IsWaitFailure(result))
        return Fail(LastError(ApiCall::WaitForFrame));
    return {};
}

Result<interior::BackBufferIndex, Error> CurrentBackBuffer(const Presenter& presenter) noexcept
{
    return interior::BackBufferIndexTag::Parse(presenter.swapChain->GetCurrentBackBufferIndex()).transform_error([](interior::UnitError) { return Error{ ApiCall::GetCurrentBackBufferIndex, 0 }; });
}

Status<Error> PresentFrame(const Presenter& presenter, bool vsync) noexcept
{
    return Check(presenter.swapChain->Present(vsync ? 1u : 0u, 0), ApiCall::Present);
}

} // namespace real
