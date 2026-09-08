#include "effects/real/nvof.h"

#include "interior/pyramid.h"

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;

using CreateInstance = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);

constexpr auto kZeroLevel = interior::LevelIndexTag::Parse(0);
static_assert(kZeroLevel.has_value());

[[nodiscard]] Status<Error> CheckFlow(NV_OF_STATUS status, ApiCall call) noexcept
{
    if (status != NV_OF_SUCCESS)
        return Fail(Error{ call, static_cast<std::uint32_t>(status) });
    return {};
}

[[nodiscard]] Result<UniqueModule, Error> LoadedLibrary() noexcept
{
    HMODULE module = ::LoadLibraryW(L"nvofapi64.dll");
    if (module == nullptr)
        return Fail(LastError(ApiCall::LoadOpticalFlow));
    return UniqueModule(module);
}

[[nodiscard]] Result<CreateInstance, Error> EntryPoint(HMODULE module) noexcept
{
    const FARPROC proc = ::GetProcAddress(module, "NvOFAPICreateInstanceD3D12");
    if (proc == nullptr)
        return Fail(LastError(ApiCall::LoadOpticalFlow));
    return reinterpret_cast<CreateInstance>(reinterpret_cast<void*>(proc));
}

[[nodiscard]] Result<NV_OF_D3D12_API_FUNCTION_LIST, Error> ApiOf(CreateInstance create) noexcept
{
    NV_OF_D3D12_API_FUNCTION_LIST api{};
    return CheckFlow(create(NV_OF_API_VERSION, &api), ApiCall::OpticalFlowCreate).transform([&api] { return api; });
}

[[nodiscard]] Result<OpticalFlowSession, Error> SessionOf(const NV_OF_D3D12_API_FUNCTION_LIST& api, ID3D12Device* device) noexcept
{
    NvOFHandle handle = nullptr;
    return CheckFlow(api.nvCreateOpticalFlowD3D12(device, &handle), ApiCall::OpticalFlowCreate).transform([&] { return OpticalFlowSession(handle, SessionDestroyer{ api.nvOFDestroy }); });
}

[[nodiscard]] NV_OF_OUTPUT_VECTOR_GRID_SIZE OutputGridOf(interior::GridSize grid) noexcept
{
    switch (grid)
    {
    case interior::GridSize::One: return NV_OF_OUTPUT_VECTOR_GRID_SIZE_1;
    case interior::GridSize::Two: return NV_OF_OUTPUT_VECTOR_GRID_SIZE_2;
    case interior::GridSize::Four: return NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    }
    return NV_OF_OUTPUT_VECTOR_GRID_SIZE_1;
}

[[nodiscard]] NV_OF_HINT_VECTOR_GRID_SIZE HintGridOf(interior::GridSize grid) noexcept
{
    switch (grid)
    {
    case interior::GridSize::One: return NV_OF_HINT_VECTOR_GRID_SIZE_1;
    case interior::GridSize::Two: return NV_OF_HINT_VECTOR_GRID_SIZE_2;
    case interior::GridSize::Four: return NV_OF_HINT_VECTOR_GRID_SIZE_4;
    }
    return NV_OF_HINT_VECTOR_GRID_SIZE_1;
}

[[nodiscard]] NV_OF_PERF_LEVEL PerfLevelOf(interior::PerfLevel level) noexcept
{
    switch (level)
    {
    case interior::PerfLevel::Slow: return NV_OF_PERF_LEVEL_SLOW;
    case interior::PerfLevel::Medium: return NV_OF_PERF_LEVEL_MEDIUM;
    case interior::PerfLevel::Fast: return NV_OF_PERF_LEVEL_FAST;
    }
    return NV_OF_PERF_LEVEL_MEDIUM;
}

[[nodiscard]] NV_OF_INIT_PARAMS InitParamsOf(const interior::SessionPlan& plan) noexcept
{
    return NV_OF_INIT_PARAMS{ plan.source.width.Get(), plan.source.height.Get(), OutputGridOf(plan.nvofGrid), HintGridOf(plan.nvofGrid), NV_OF_MODE_OPTICALFLOW,
                              PerfLevelOf(plan.nvofPerf), NV_OF_FALSE, NV_OF_FALSE, nullptr, NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED, NV_OF_FALSE };
}

[[nodiscard]] Status<Error> Initialized(const NV_OF_D3D12_API_FUNCTION_LIST& api, NvOFHandle session, const interior::SessionPlan& plan) noexcept
{
    const NV_OF_INIT_PARAMS init = InitParamsOf(plan);
    return CheckFlow(api.nvOFInit(session, &init), ApiCall::OpticalFlowInit);
}

[[nodiscard]] Result<Com<ID3D12Fence>, Error> CompletionFence(const GpuDevice& gpu) noexcept
{
    Com<ID3D12Fence> fence;
    const HRESULT hr = gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    return Check(hr, ApiCall::CreateFence).transform([&fence] { return fence; });
}

struct Registration
{
    const NV_OF_D3D12_API_FUNCTION_LIST* api;
    NvOFHandle session;
    ID3D12Fence* input;
    ID3D12Fence* output;
};

[[nodiscard]] Result<RegisteredBuffer, Error> Registered(const Registration& r, ID3D12Resource* resource) noexcept
{
    NvOFGPUBufferHandle handle = nullptr;
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{ resource, &handle, NV_OF_FENCE_POINT{ r.input, 0 }, NV_OF_FENCE_POINT{ r.output, 0 } };
    return CheckFlow(r.api->nvOFRegisterResourceD3D12(r.session, &params), ApiCall::OpticalFlowRegister)
        .transform([&] { return RegisteredBuffer(handle, BufferUnregister{ r.api->nvOFUnregisterResourceD3D12 }); });
}

[[nodiscard]] Result<RegisteredBuffer, Error> RegisteredResource(const Registration& r, const ResourceTable& resources, const interior::ResourceId& id) noexcept
{
    return Lookup(resources, id).and_then([&r](ID3D12Resource* resource) { return Registered(r, resource); });
}

[[nodiscard]] interior::ResourceId LumaIdOf(std::uint32_t set) noexcept
{
    const Result<interior::SetIndex, interior::UnitError> s = interior::SetIndexTag::Parse(set);
    ENSURE(s.has_value());
    return interior::LumaId(*s, *kZeroLevel);
}

struct Loaded
{
    UniqueModule library;
    NV_OF_D3D12_API_FUNCTION_LIST api;
};

[[nodiscard]] Result<Loaded, Error> LoadApi() noexcept
{
    return LoadedLibrary().and_then([](UniqueModule library)
    {
        return EntryPoint(library.get()).and_then(ApiOf).transform([&library](const NV_OF_D3D12_API_FUNCTION_LIST& api) { return Loaded{ std::move(library), api }; });
    });
}

[[nodiscard]] Result<OpticalFlow, Error> Assembled(Loaded loaded, OpticalFlowSession session, const Com<ID3D12Fence>& completion, const GpuDevice& gpu, const ResourceTable& resources) noexcept
{
    const Registration r{ &loaded.api, session.get(), gpu.fence.Get(), completion.Get() };
    return RegisteredResource(r, resources, LumaIdOf(0)).and_then([&](RegisteredBuffer first)
    {
        return RegisteredResource(r, resources, LumaIdOf(1)).and_then([&](RegisteredBuffer second)
        {
            return RegisteredResource(r, resources, interior::SimpleId(interior::ResourceKind::OpticalFlowOutput)).transform([&](RegisteredBuffer flow)
            {
                return OpticalFlow{ std::move(loaded.library), loaded.api, std::move(session), completion, { std::move(first), std::move(second) }, std::move(flow) };
            });
        });
    });
}

[[nodiscard]] Result<OpticalFlow, Error> WithSession(Loaded loaded, const GpuDevice& gpu, const interior::SessionPlan& plan, const ResourceTable& resources) noexcept
{
    return SessionOf(loaded.api, gpu.device.Get()).and_then([&](OpticalFlowSession session)
    {
        return Initialized(loaded.api, session.get(), plan)
            .and_then([&] { return CompletionFence(gpu); })
            .and_then([&](const Com<ID3D12Fence>& completion) { return Assembled(std::move(loaded), std::move(session), completion, gpu, resources); });
    });
}

[[nodiscard]] std::uint32_t OtherSet(interior::SetIndex set) noexcept
{
    return set.Get() ^ 1u;
}

[[nodiscard]] NV_OF_EXECUTE_INPUT_PARAMS_D3D12 InputParamsOf(const OpticalFlow& f, const OpticalFlowFrame& frame, NV_OF_FENCE_POINT* wait) noexcept
{
    return NV_OF_EXECUTE_INPUT_PARAMS_D3D12{ f.luma[frame.set.Get()].get(), f.luma[OtherSet(frame.set)].get(), nullptr, NV_OF_FALSE, 0, wait, 1, 0, nullptr, 0, nullptr };
}

[[nodiscard]] NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 OutputParamsOf(const OpticalFlow& f, const OpticalFlowFrame& frame) noexcept
{
    return NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12{ f.flow.get(), nullptr, NV_OF_FENCE_POINT{ f.completion.Get(), frame.number.Get() + 1 }, nullptr };
}

} // namespace

void ModuleFreer::operator()(HMODULE module) const noexcept
{
    ENSURE(::FreeLibrary(module) != FALSE);
}

void SessionDestroyer::operator()(NvOFHandle session) const noexcept
{
    ENSURE(destroy(session) == NV_OF_SUCCESS);
}

void BufferUnregister::operator()(NvOFGPUBufferHandle buffer) const noexcept
{
    NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{ buffer };
    ENSURE(unregister(&params) == NV_OF_SUCCESS);
}

Result<OpticalFlow, Error> CreateOpticalFlow(const GpuDevice& gpu, const interior::SessionPlan& plan, const ResourceTable& resources) noexcept
{
    return LoadApi().and_then([&](Loaded loaded) { return WithSession(std::move(loaded), gpu, plan, resources); });
}

Status<Error> ExecuteOpticalFlow(const OpticalFlow& flow, const GpuDevice& gpu, const OpticalFlowFrame& frame) noexcept
{
    if (!frame.hasPrevious)
        return {};
    NV_OF_FENCE_POINT wait{ gpu.fence.Get(), frame.phaseOne.Get() };
    const NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in = InputParamsOf(flow, frame, &wait);
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out = OutputParamsOf(flow, frame);
    return CheckFlow(flow.api.nvOFExecuteD3D12(flow.session.get(), &in, &out), ApiCall::OpticalFlowExecute).and_then([&] { return Check(gpu.queue->Wait(flow.completion.Get(), out.fencePoint.value), ApiCall::QueueWait); });
}

} // namespace real
