#include "effects/real/com.h"

#include "infrastructure/text.h"

namespace real {

std::string_view Describe(ApiCall call) noexcept
{
    switch (call)
    {
    case ApiCall::CreateDXGIFactory2: return "CreateDXGIFactory2";
    case ApiCall::EnumAdapters1: return "IDXGIFactory::EnumAdapters1";
    case ApiCall::D3D12CreateDevice: return "D3D12CreateDevice";
    case ApiCall::CreateCommandQueue: return "ID3D12Device::CreateCommandQueue";
    case ApiCall::CreateFence: return "ID3D12Device::CreateFence";
    case ApiCall::CreateEventW: return "CreateEventW";
    case ApiCall::CreateDescriptorHeap: return "ID3D12Device::CreateDescriptorHeap";
    case ApiCall::CreateCommandAllocator: return "ID3D12Device::CreateCommandAllocator";
    case ApiCall::CreateCommandList: return "ID3D12Device::CreateCommandList";
    case ApiCall::CloseCommandList: return "ID3D12GraphicsCommandList::Close";
    case ApiCall::ResetCommandList: return "ID3D12GraphicsCommandList::Reset";
    case ApiCall::ResetAllocator: return "ID3D12CommandAllocator::Reset";
    case ApiCall::CreateCommittedResource: return "ID3D12Device::CreateCommittedResource";
    case ApiCall::MapResource: return "ID3D12Resource::Map";
    case ApiCall::SetEventOnCompletion: return "ID3D12Fence::SetEventOnCompletion";
    case ApiCall::WaitForFence: return "fence wait timed out (GPU hang?)";
    case ApiCall::QueueSignal: return "ID3D12CommandQueue::Signal";
    case ApiCall::QueueWait: return "ID3D12CommandQueue::Wait";
    case ApiCall::CreateSwapChainForComposition: return "IDXGIFactory2::CreateSwapChainForComposition";
    case ApiCall::QueryInterface: return "QueryInterface";
    case ApiCall::SetMaximumFrameLatency: return "IDXGISwapChain2::SetMaximumFrameLatency";
    case ApiCall::GetBuffer: return "IDXGISwapChain::GetBuffer";
    case ApiCall::Present: return "IDXGISwapChain::Present";
    case ApiCall::WaitForFrame: return "frame latency wait timed out";
    case ApiCall::DCompositionCreateDevice2: return "DCompositionCreateDevice2";
    case ApiCall::CreateTargetForHwnd: return "IDCompositionDevice::CreateTargetForHwnd";
    case ApiCall::CreateVisual: return "IDCompositionDevice::CreateVisual";
    case ApiCall::SetContent: return "IDCompositionVisual::SetContent";
    case ApiCall::SetRoot: return "IDCompositionTarget::SetRoot";
    case ApiCall::Commit: return "IDCompositionDevice::Commit";
    case ApiCall::SerializeRootSignature: return "D3D12SerializeRootSignature";
    case ApiCall::CreateRootSignature: return "ID3D12Device::CreateRootSignature";
    case ApiCall::CreateComputePipelineState: return "ID3D12Device::CreateComputePipelineState";
    case ApiCall::CreateGraphicsPipelineState: return "ID3D12Device::CreateGraphicsPipelineState";
    case ApiCall::RegisterClassExW: return "RegisterClassExW";
    case ApiCall::CreateWindowExW: return "CreateWindowExW";
    case ApiCall::SetWindowDisplayAffinity: return "SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)";
    case ApiCall::RegisterHotKey: return "RegisterHotKey";
    case ApiCall::CreateProcess: return "CreateProcessW";
    case ApiCall::RoInitialize: return "RoInitialize";
    case ApiCall::SetEnvironmentVariable: return "SetEnvironmentVariableW";
    case ApiCall::RoGetActivationFactory: return "RoGetActivationFactory";
    case ApiCall::WindowsCreateStringReference: return "WindowsCreateStringReference";
    case ApiCall::D3D11CreateDevice: return "D3D11CreateDevice";
    case ApiCall::CreateTexture2D: return "ID3D11Device::CreateTexture2D";
    case ApiCall::D3D11CreateFence: return "ID3D11Device5::CreateFence";
    case ApiCall::CreateSharedHandle: return "CreateSharedHandle";
    case ApiCall::OpenSharedHandle: return "ID3D12Device::OpenSharedHandle";
    case ApiCall::ContextWait: return "ID3D11DeviceContext4::Wait";
    case ApiCall::ContextSignal: return "ID3D11DeviceContext4::Signal";
    case ApiCall::CreateDirect3D11DeviceFromDXGIDevice: return "CreateDirect3D11DeviceFromDXGIDevice";
    case ApiCall::CreateForMonitor: return "IGraphicsCaptureItemInterop::CreateForMonitor";
    case ApiCall::CreateFreeThreaded: return "Direct3D11CaptureFramePool::CreateFreeThreaded";
    case ApiCall::CreateCaptureSession: return "Direct3D11CaptureFramePool::CreateCaptureSession";
    case ApiCall::StartCapture: return "GraphicsCaptureSession::StartCapture";
    case ApiCall::TryGetNextFrame: return "Direct3D11CaptureFramePool::TryGetNextFrame";
    case ApiCall::GetSurface: return "Direct3D11CaptureFrame::Surface";
    case ApiCall::GetInterface: return "IDirect3DDxgiInterfaceAccess::GetInterface";
    case ApiCall::GetContentSize: return "Direct3D11CaptureFrame::ContentSize";
    case ApiCall::PutIsCursorCaptureEnabled: return "GraphicsCaptureSession::IsCursorCaptureEnabled";
    case ApiCall::PutIsBorderRequired: return "GraphicsCaptureSession::IsBorderRequired";
    case ApiCall::CloseFrame: return "Direct3D11CaptureFrame::Close";
    case ApiCall::IsCaptureSupported: return "GraphicsCaptureSession::IsSupported";
    case ApiCall::GetMonitorInfoW: return "GetMonitorInfoW";
    case ApiCall::EnumDisplayMonitors: return "EnumDisplayMonitors";
    case ApiCall::QueryPerformanceCounter: return "QueryPerformanceCounter";
    case ApiCall::QueryPerformanceFrequency: return "QueryPerformanceFrequency";
    case ApiCall::OpenLogFile: return "opening the log file";
    case ApiCall::WriteLog: return "writing the log";
    case ApiCall::NgxInit: return "NVSDK_NGX_D3D12_Init";
    case ApiCall::NgxGetCapabilityParameters: return "NVSDK_NGX_D3D12_GetCapabilityParameters";
    case ApiCall::NgxCreateFeature: return "NVSDK_NGX_D3D12_CreateFeature";
    case ApiCall::NgxEvaluateFeature: return "NVSDK_NGX_D3D12_EvaluateFeature";
    case ApiCall::NgxOptimalSettings: return "NGX_DLSS_GET_OPTIMAL_SETTINGS";
    case ApiCall::NgxParameterRoundTrip: return "an NGX parameter did not read back the value written";
    case ApiCall::OpenModelFile: return "opening nvngx_dlssnr.dll to check its signature";
    case ApiCall::ModelNotSigned: return "nvngx_dlssnr.dll carries no signature Windows will trust";
    case ApiCall::ModelNotFromNvidia: return "nvngx_dlssnr.dll is signed, but not by NVIDIA";
    case ApiCall::NgxNeuralRenderingUnavailable: return "DLSS 5 Neural Rendering is unavailable (DLSSNR.Available)";
    case ApiCall::TextureDescriptionMismatch: return "a created texture does not match its description";
    case ApiCall::PlanFrame: return "frame planning";
    case ApiCall::LoadOpticalFlow: return "loading nvofapi64.dll";
    case ApiCall::OpticalFlowCreate: return "NvOFAPICreateInstanceD3D12 / nvCreateOpticalFlowD3D12";
    case ApiCall::OpticalFlowInit: return "nvOFInit";
    case ApiCall::OpticalFlowRegister: return "nvOFRegisterResourceD3D12";
    case ApiCall::OpticalFlowExecute: return "nvOFExecuteD3D12";
    case ApiCall::ResourceMissing: return "a planned resource does not exist";
    case ApiCall::ArgumentCount: return "too many command-line arguments";
    case ApiCall::DpiAwareness: return "SetProcessDpiAwarenessContext";
    case ApiCall::SetProcessDpiAwareness: return "SetProcessDpiAwarenessContext";
    case ApiCall::GetMonitorRect: return "monitor rectangle";
    case ApiCall::AdapterNotFound: return "no Direct3D 12 adapter matched the request";
    case ApiCall::NotNvidia: return "the selected adapter is not an NVIDIA GPU";
    case ApiCall::DescriptorBudget: return "a frame needs more descriptors than its ring holds";
    case ApiCall::StatsOutOfRange: return "the unmatched-pixel count exceeds the pixel count";
    case ApiCall::GetModuleFileNameW: return "GetModuleFileNameW";
    case ApiCall::CommandLineToArgvW: return "CommandLineToArgvW";
    case ApiCall::ResolveGeometry: return "monitor selection";
    case ApiCall::PlanSession: return "session planning";
    case ApiCall::NgxParameterList: return "the DLSS 5 parameter list exceeded its capacity";
    case ApiCall::NgxGetFeatureRequirements: return "NVSDK_NGX_D3D12_GetFeatureRequirements";
    case ApiCall::NgxShutdown: return "NVSDK_NGX_D3D12_Shutdown1";
    case ApiCall::OpticalFlowUnavailable: return "this build has no NVIDIA Optical Flow backend (configure with -DDSCREEN_ENABLE_NVOF=ON)";
    case ApiCall::GetCurrentBackBufferIndex: return "IDXGISwapChain3::GetCurrentBackBufferIndex";
    case ApiCall::ExecutableDirectory: return "the executable path has no directory";
    }
    return "unknown call";
}

ErrorText Describe(const Error& error) noexcept
{
    return infra::Formatted<ErrorText::Capacity>("{} failed with 0x{:08X}", Describe(error.call), error.code);
}

} // namespace real
