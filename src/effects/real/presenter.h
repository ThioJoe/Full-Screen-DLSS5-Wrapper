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

// The picture is composited over the window rather than being the window's own content: clicking through
// needs WS_EX_LAYERED, and a layered window cannot hold a Direct3D 12 swap chain of its own.
[[nodiscard]] infra::Result<Presenter, Error> CreatePresenter(const GpuDevice& gpu, HWND window, const interior::Extent& extent) noexcept;
[[nodiscard]] infra::Status<Error> WaitForNextFrame(const Presenter& presenter) noexcept;
[[nodiscard]] infra::Result<interior::BackBufferIndex, Error> CurrentBackBuffer(const Presenter& presenter) noexcept;
[[nodiscard]] infra::Status<Error> PresentFrame(const Presenter& presenter, bool vsync) noexcept;

} // namespace real
