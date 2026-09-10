#pragma once
#include "effects/real/device.h"
#include "interior/units.h"

#include <dcomp.h>

#include <array>

namespace real {

constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint64_t kFrameWaitMicroseconds = 2000000;
constexpr std::uint32_t kDepthRtvSlot = interior::kBackBufferCount;

using BackBuffers = std::array<Com<ID3D12Resource>, interior::kBackBufferCount>;

struct Presenter
{
    Com<IDXGISwapChain3> swapChain;
    Com<IDCompositionDevice> compositionDevice;
    Com<IDCompositionTarget> target;
    Com<IDCompositionVisual> visual;
    UniqueHandle waitable;
    BackBuffers backBuffers;
    interior::Extent extent;
};

// `ownContent` binds the swap chain to the window instead of compositing it over the window. It costs the
// per-pixel alpha a composition gives, and it is what lets a capture leave the picture out by name.
[[nodiscard]] infra::Result<Presenter, Error> CreatePresenter(const GpuDevice& gpu, HWND window, const interior::Extent& extent, bool ownContent) noexcept;
[[nodiscard]] infra::Status<Error> WaitForNextFrame(const Presenter& presenter) noexcept;
[[nodiscard]] infra::Result<interior::BackBufferIndex, Error> CurrentBackBuffer(const Presenter& presenter) noexcept;
[[nodiscard]] infra::Status<Error> PresentFrame(const Presenter& presenter, bool vsync) noexcept;

} // namespace real
