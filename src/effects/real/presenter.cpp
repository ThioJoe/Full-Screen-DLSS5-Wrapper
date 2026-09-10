#include "effects/real/presenter.h"

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

[[nodiscard]] Result<Com<IDXGISwapChain3>, Error> WithLatencyOne(const Com<IDXGISwapChain3>& chain) noexcept
{
    return Check(chain->SetMaximumFrameLatency(1), ApiCall::SetMaximumFrameLatency).transform([&chain] { return chain; });
}

[[nodiscard]] Result<Com<IDXGISwapChain3>, Error> CreateSwapChain(const GpuDevice& gpu, const interior::Extent& extent) noexcept
{
    return CreateCompositionSwapChain(gpu, extent).and_then([](const Com<IDXGISwapChain1>& made) { return As<IDXGISwapChain3>(made, ApiCall::QueryInterface).and_then(WithLatencyOne); });
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

[[nodiscard]] bool IsWaitFailure(DWORD result) noexcept
{
    return result == WAIT_FAILED;
}

} // namespace

Result<Presenter, Error> CreatePresenter(const GpuDevice& gpu, HWND window, const interior::Extent& extent) noexcept
{
    return CreateSwapChain(gpu, extent).and_then([&](const Com<IDXGISwapChain3>& chain) {
        return CreateComposition(window, chain.Get()).and_then([&](const Composition& composition) { return Rest(gpu, composition, chain, extent); });
    });
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
