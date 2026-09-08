#pragma once
// WAIVER(R31): the COM helpers below are the effect interface for the Windows layer.
#include "infrastructure/bounded_string.h"
#include "infrastructure/contracts.h"
#include "infrastructure/result.h"

#include <unknwn.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

namespace real {

enum class ApiCall : std::uint8_t {
    CreateDXGIFactory2,
    EnumAdapters1,
    D3D12CreateDevice,
    CreateCommandQueue,
    CreateFence,
    CreateEventW,
    CreateDescriptorHeap,
    CreateCommandAllocator,
    CreateCommandList,
    CloseCommandList,
    ResetCommandList,
    ResetAllocator,
    CreateCommittedResource,
    MapResource,
    SetEventOnCompletion,
    WaitForFence,
    QueueSignal,
    QueueWait,
    CreateSwapChainForComposition,
    QueryInterface,
    SetMaximumFrameLatency,
    GetBuffer,
    Present,
    WaitForFrame,
    DCompositionCreateDevice2,
    CreateTargetForHwnd,
    CreateVisual,
    SetContent,
    SetRoot,
    Commit,
    SerializeRootSignature,
    CreateRootSignature,
    CreateComputePipelineState,
    CreateGraphicsPipelineState,
    RegisterClassExW,
    CreateWindowExW,
    SetWindowDisplayAffinity,
    RegisterHotKey,
    RoInitialize,
    RoGetActivationFactory,
    WindowsCreateStringReference,
    D3D11CreateDevice,
    CreateSharedHandle,
    OpenSharedResource,
    OpenSharedFence,
    ContextWait,
    ContextSignal,
    CreateDirect3D11DeviceFromDXGIDevice,
    CreateForMonitor,
    CreateFreeThreaded,
    CreateCaptureSession,
    StartCapture,
    TryGetNextFrame,
    GetSurface,
    GetInterface,
    GetContentSize,
    PutIsCursorCaptureEnabled,
    PutIsBorderRequired,
    CloseFrame,
    IsCaptureSupported,
    GetMonitorInfoW,
    EnumDisplayMonitors,
    QueryPerformanceCounter,
    QueryPerformanceFrequency,
    OpenLogFile,
    WriteLog,
    NgxInit,
    NgxGetCapabilityParameters,
    NgxCreateFeature,
    NgxEvaluateFeature,
    NgxOptimalSettings,
    NgxParameterRoundTrip,
    NgxNeuralRenderingUnavailable,
    NgxSuperResolutionUnavailable,
    TextureDescriptionMismatch,
    PlanFrame,
    LoadOpticalFlow,
    OpticalFlowCreate,
    OpticalFlowInit,
    OpticalFlowRegister,
    OpticalFlowExecute,
    ResourceMissing,
    ArgumentCount,
    DpiAwareness,
    SetProcessDpiAwareness,
    GetMonitorRect,
    AdapterNotFound,
    NotNvidia,
    DescriptorBudget,
    StatsOutOfRange,
    GetModuleFileNameW,
    CommandLineToArgvW,
    ResolveGeometry,
    PlanSession,
    NgxParameterList,
    NgxGetFeatureRequirements,
    NgxShutdown,
    OpticalFlowUnavailable,
    GetCurrentBackBufferIndex,
    ExecutableDirectory,
};

struct Error
{
    ApiCall call;
    std::uint32_t code;
    [[nodiscard]] friend constexpr bool operator==(const Error&, const Error&) noexcept = default;
};

template <class T>
using Com = Microsoft::WRL::ComPtr<T>;

using ErrorText = infra::BoundedString<char, 200>;

[[nodiscard]] std::string_view Describe(ApiCall call) noexcept;
[[nodiscard]] ErrorText Describe(const Error& error) noexcept;

[[nodiscard]] constexpr bool IsFailure(HRESULT hr) noexcept
{
    return hr < 0;
}

[[nodiscard]] inline infra::Status<Error> Check(HRESULT hr, ApiCall call) noexcept
{
    if (IsFailure(hr))
        return infra::Fail(Error{ call, static_cast<std::uint32_t>(hr) });
    return {};
}

[[nodiscard]] inline Error LastError(ApiCall call) noexcept
{
    return Error{ call, static_cast<std::uint32_t>(::GetLastError()) };
}

[[nodiscard]] inline infra::Status<Error> CheckBool(BOOL ok, ApiCall call) noexcept
{
    if (ok == FALSE)
        return infra::Fail(LastError(call));
    return {};
}

template <class T, class U>
[[nodiscard]] infra::Result<Com<T>, Error> As(const Com<U>& source, ApiCall call) noexcept
{
    Com<T> out;
    const HRESULT hr = source.As(&out);
    return Check(hr, call).transform([&out] { return out; });
}

struct HandleCloser
{
    void operator()(void* handle) const noexcept { ENSURE(::CloseHandle(handle) != FALSE); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct WindowDestroyer
{
    void operator()(HWND window) const noexcept { ENSURE(::DestroyWindow(window) != FALSE); }
};
using UniqueWindow = std::unique_ptr<std::remove_pointer_t<HWND>, WindowDestroyer>;

} // namespace real
