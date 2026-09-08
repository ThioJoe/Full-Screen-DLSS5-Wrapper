#include "effects/real/ngx.h"

#include "infrastructure/fold.h"
#include "infrastructure/overloaded.h"

#include <algorithm>
#include <string_view>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;

constexpr std::array<const char*, 6> kPresetNames{
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality
};
constexpr std::array<interior::SrQuality, 6> kQualities{ interior::SrQuality::Dlaa,     interior::SrQuality::UltraQuality, interior::SrQuality::Quality,
                                                         interior::SrQuality::Balanced, interior::SrQuality::Performance,  interior::SrQuality::UltraPerformance };
constexpr char kNeuralRenderingAvailable[] = "DLSSNR.Available";
constexpr std::wstring_view kNeuralRenderingModel = L"\\nvngx_dlssnr.dll";
constexpr std::size_t kModelPathCapacity = interior::DirectoryPath::Capacity + kNeuralRenderingModel.size() + 1;

[[nodiscard]] std::array<wchar_t, kModelPathCapacity> ModelPathIn(std::wstring_view directory) noexcept
{
    std::array<wchar_t, kModelPathCapacity> chars{};
    // WAIVER(R2): a local buffer filled once, before use, from two bounded pieces.
    std::ranges::copy(directory, chars.begin());
    std::ranges::copy(kNeuralRenderingModel, chars.begin() + static_cast<std::ptrdiff_t>(directory.size()));
    return chars;
}

[[nodiscard]] bool HasModel(const interior::DirectoryPath& directory) noexcept
{
    return !directory.IsEmpty() && ::GetFileAttributesW(ModelPathIn(directory.Get()).data()) != INVALID_FILE_ATTRIBUTES;
}

[[nodiscard]] std::optional<interior::DirectoryPath> ModelIn(const interior::DirectoryPath& directory) noexcept
{
    if (!HasModel(directory))
        return std::nullopt;
    return directory;
}

[[nodiscard]] Status<Error> CheckNgx(NVSDK_NGX_Result result, ApiCall call) noexcept
{
    if (NVSDK_NGX_FAILED(result))
        return Fail(Error{ call, static_cast<std::uint32_t>(result) });
    return {};
}

[[nodiscard]] Error FromCapacity(infra::CapacityExceeded) noexcept
{
    return Error{ ApiCall::NgxParameterList, 0 };
}

[[nodiscard]] NVSDK_NGX_Logging_Level LoggingLevelOf(interior::NgxLogLevel level) noexcept
{
    switch (level)
    {
    case interior::NgxLogLevel::Off: return NVSDK_NGX_LOGGING_LEVEL_OFF;
    case interior::NgxLogLevel::On: return NVSDK_NGX_LOGGING_LEVEL_ON;
    case interior::NgxLogLevel::Verbose: return NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    }
    return NVSDK_NGX_LOGGING_LEVEL_OFF;
}

[[nodiscard]] unsigned int PathCount(const NgxSettings& s) noexcept
{
    return s.featurePath.IsEmpty() ? 1u : 2u;
}

[[nodiscard]] NVSDK_NGX_FeatureCommonInfo CommonInfoOf(const std::array<const wchar_t*, 2>& pointers, unsigned int count, interior::NgxLogLevel level) noexcept
{
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = NVSDK_NGX_PathListInfo{ pointers.data(), count };
    info.LoggingInfo = NVSDK_NGX_LoggingInfo{ nullptr, LoggingLevelOf(level), false };
    return info;
}

[[nodiscard]] NVSDK_NGX_Application_Identifier AppIdentifier(interior::NgxAppId id) noexcept
{
    NVSDK_NGX_Application_Identifier identifier{};
    identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
    identifier.v.ApplicationId = id.Get();
    return identifier;
}

[[nodiscard]] NVSDK_NGX_Application_Identifier ProjectIdentifier(const NgxSettings& s) noexcept
{
    NVSDK_NGX_Application_Identifier identifier{};
    identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    identifier.v.ProjectDesc = NVSDK_NGX_ProjectIdDescription{ s.projectId.CString(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, DSCREEN_VERSION_STRING };
    return identifier;
}

[[nodiscard]] NVSDK_NGX_Application_Identifier IdentifierOf(const NgxSettings& s) noexcept
{
    return s.appId.has_value() ? AppIdentifier(*s.appId) : ProjectIdentifier(s);
}

[[nodiscard]] NVSDK_NGX_Result InitWithAppId(interior::NgxAppId id, const NgxSettings& s, ID3D12Device* device, const NVSDK_NGX_FeatureCommonInfo& common) noexcept
{
    return NVSDK_NGX_D3D12_Init(id.Get(), s.dataPath.CString(), device, &common, NVSDK_NGX_Version_API);
}

[[nodiscard]] NVSDK_NGX_Result InitWithProjectId(const NgxSettings& s, ID3D12Device* device, const NVSDK_NGX_FeatureCommonInfo& common) noexcept
{
    return NVSDK_NGX_D3D12_Init_with_ProjectID(s.projectId.CString(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, DSCREEN_VERSION_STRING, s.dataPath.CString(), device, &common, NVSDK_NGX_Version_API);
}

[[nodiscard]] NVSDK_NGX_Result Init(const NgxSettings& s, ID3D12Device* device, const NVSDK_NGX_FeatureCommonInfo& common) noexcept
{
    return s.appId.has_value() ? InitWithAppId(*s.appId, s, device, common) : InitWithProjectId(s, device, common);
}

[[nodiscard]] Result<NgxParameters, Error> OwnedParameters(NVSDK_NGX_Parameter* raw) noexcept
{
    if (raw == nullptr)
        return Fail(Error{ ApiCall::NgxGetCapabilityParameters, 0 });
    return NgxParameters(raw);
}

[[nodiscard]] Result<NgxParameters, Error> CapabilityParameters() noexcept
{
    NVSDK_NGX_Parameter* raw = nullptr;
    return CheckNgx(NVSDK_NGX_D3D12_GetCapabilityParameters(&raw), ApiCall::NgxGetCapabilityParameters).and_then([raw] { return OwnedParameters(raw); });
}

[[nodiscard]] Result<NgxRuntime, Error> Initialized(const GpuDevice& gpu, const std::shared_ptr<const NgxPaths>& paths) noexcept
{
    NgxSession session(gpu.device.Get());
    return CapabilityParameters().transform([&](NgxParameters parameters) { return NgxRuntime{ paths, gpu.device, std::move(session), std::move(parameters) }; });
}

[[nodiscard]] std::optional<int> IntOf(const NVSDK_NGX_Parameter* p, const char* name) noexcept
{
    int value = 0;
    if (NVSDK_NGX_FAILED(p->Get(name, &value)))
        return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<unsigned int> UIntOf(const NVSDK_NGX_Parameter* p, const char* name) noexcept
{
    unsigned int value = 0;
    if (NVSDK_NGX_FAILED(p->Get(name, &value)))
        return std::nullopt;
    return value;
}

[[nodiscard]] bool NeedsDriverUpdate(const NVSDK_NGX_Parameter* p) noexcept
{
    return IntOf(p, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver).value_or(0) != 0;
}

[[nodiscard]] bool IsSuperResolutionAvailable(const NVSDK_NGX_Parameter* p) noexcept
{
    return IntOf(p, NVSDK_NGX_Parameter_SuperSampling_Available).value_or(0) != 0;
}

[[nodiscard]] Status<Error> RequireCurrentDriver(const NVSDK_NGX_Parameter* p) noexcept
{
    if (NeedsDriverUpdate(p))
        return Fail(Error{ ApiCall::NgxSuperResolutionUnavailable, 1 });
    return {};
}

[[nodiscard]] Status<Error> RequireAvailable(const NVSDK_NGX_Parameter* p) noexcept
{
    if (!IsSuperResolutionAvailable(p))
        return Fail(Error{ ApiCall::NgxSuperResolutionUnavailable, 2 });
    return {};
}

[[nodiscard]] NVSDK_NGX_PerfQuality_Value PerfQualityOf(interior::SrQuality quality) noexcept
{
    switch (quality)
    {
    case interior::SrQuality::Dlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case interior::SrQuality::UltraQuality: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    case interior::SrQuality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case interior::SrQuality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case interior::SrQuality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case interior::SrQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
    return NVSDK_NGX_PerfQuality_Value_Balanced;
}

struct OptimalSettings
{
    unsigned int optimalWidth;
    unsigned int optimalHeight;
    unsigned int maxWidth;
    unsigned int maxHeight;
    unsigned int minWidth;
    unsigned int minHeight;
    float sharpness;
    NVSDK_NGX_Result result;
};

[[nodiscard]] OptimalSettings Optimal(NVSDK_NGX_Parameter* p, const interior::Extent& target, NVSDK_NGX_PerfQuality_Value quality) noexcept
{
    OptimalSettings o{};
    o.result =
        NGX_DLSS_GET_OPTIMAL_SETTINGS(p, target.width.Get(), target.height.Get(), quality, &o.optimalWidth, &o.optimalHeight, &o.maxWidth, &o.maxHeight, &o.minWidth, &o.minHeight, &o.sharpness);
    return o;
}

[[nodiscard]] std::optional<interior::Extent> ExtentOf(unsigned int width, unsigned int height) noexcept
{
    return interior::PixelCountTag::Parse(width)
        .and_then([height](interior::PixelCount w) { return interior::PixelCountTag::Parse(height).transform([w](interior::PixelCount h) { return interior::Extent{ w, h }; }); })
        .transform([](const interior::Extent& e) { return std::optional<interior::Extent>{ e }; })
        .value_or(std::nullopt);
}

[[nodiscard]] std::optional<interior::QualityRange> RangeFrom(interior::SrQuality quality, const OptimalSettings& o) noexcept
{
    if (NVSDK_NGX_FAILED(o.result))
        return std::nullopt;
    return ExtentOf(o.optimalWidth, o.optimalHeight).and_then([&](const interior::Extent& optimal) {
        return ExtentOf(o.minWidth, o.minHeight).and_then([&](const interior::Extent& minimum) {
            return ExtentOf(o.maxWidth, o.maxHeight).transform([&](const interior::Extent& maximum) { return interior::QualityRange{ quality, optimal, minimum, maximum }; });
        });
    });
}

[[nodiscard]] interior::QualityTable WithRange(const interior::QualityTable& table, const std::optional<interior::QualityRange>& range) noexcept
{
    if (!range.has_value())
        return table;
    const Result<interior::QualityTable, infra::CapacityExceeded> pushed = table.Push(*range);
    ENSURE(pushed.has_value());
    return *pushed;
}

[[nodiscard]] const char* CName(interior::NrParameter parameter) noexcept
{
    const std::string_view name = interior::NameOf(parameter);
    REQUIRE(name.data()[name.size()] == '\0');
    return name.data();
}

void Write(NVSDK_NGX_Parameter* p, const char* name, const NgxSlot& value) noexcept
{
    std::visit([p, name](auto v) { p->Set(name, v); }, value);
}

[[nodiscard]] bool ReadsBack(const NVSDK_NGX_Parameter* p, const char* name, const NgxSlot& value) noexcept
{
    return std::visit(infra::Overloaded{
                          [p, name](unsigned int v) {
                              unsigned int back = 0;
                              return !NVSDK_NGX_FAILED(p->Get(name, &back)) && back == v;
                          },
                          [p, name](float v) {
                              float back = 0.0f;
                              return !NVSDK_NGX_FAILED(p->Get(name, &back)) && back == v;
                          },
                          [p, name](ID3D12Resource* v) {
                              ID3D12Resource* back = nullptr;
                              return !NVSDK_NGX_FAILED(p->Get(name, &back)) && back == v;
                          },
                      },
                      value);
}

[[nodiscard]] Status<Error> WriteVerified(NVSDK_NGX_Parameter* p, const char* name, const NgxSlot& value) noexcept
{
    Write(p, name, value);
    if (!ReadsBack(p, name, value))
        return Fail(Error{ ApiCall::NgxParameterRoundTrip, static_cast<std::uint32_t>(value.index()) });
    return {};
}

[[nodiscard]] Status<Error> WriteAll(NVSDK_NGX_Parameter* p, const BoundNrParameters& list) noexcept
{
    return infra::ForEach(list.Items(), Status<Error>{}, [p](const BoundNrParameter& b) { return WriteVerified(p, CName(b.name), b.value); });
}

[[nodiscard]] Status<Error> WritePresets(NVSDK_NGX_Parameter* p, interior::SrPreset preset) noexcept
{
    if (preset.Get() == 0)
        return {};
    return infra::ForEach(kPresetNames, Status<Error>{}, [p, preset](const char* name) { return WriteVerified(p, name, NgxSlot{ preset.Get() }); });
}

[[nodiscard]] Result<NgxSlot, Error> SlotOf(const interior::NgxValue& value, const ResourceTable& table) noexcept
{
    return std::visit(infra::Overloaded{
                          [](std::uint32_t v) -> Result<NgxSlot, Error> { return NgxSlot{ v }; },
                          [](float v) -> Result<NgxSlot, Error> { return NgxSlot{ v }; },
                          [&table](const interior::ResourceId& id) -> Result<NgxSlot, Error> { return Lookup(table, id).transform([](ID3D12Resource* r) { return NgxSlot{ r }; }); },
                      },
                      value);
}

[[nodiscard]] Result<BoundNrParameters, Error> WithBound(const BoundNrParameters& acc, const interior::NrParameterValue& v, const ResourceTable& table) noexcept
{
    return SlotOf(v.value, table).and_then([&](const NgxSlot& slot) { return acc.Push(BoundNrParameter{ v.name, slot }).transform_error(FromCapacity); });
}

[[nodiscard]] int CreateFlagsOf(bool hdr) noexcept
{
    return hdr ? (NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR) : NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
}

[[nodiscard]] NVSDK_NGX_DLSS_Create_Params CreateParamsOf(const interior::SrChoice& c) noexcept
{
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature = NVSDK_NGX_Feature_Create_Params{ c.input.width.Get(), c.input.height.Get(), c.output.width.Get(), c.output.height.Get(), PerfQualityOf(c.quality) };
    create.InFeatureCreateFlags = CreateFlagsOf(c.hdr);
    create.InEnableOutputSubrects = false;
    return create;
}

[[nodiscard]] NVSDK_NGX_D3D12_DLSS_Eval_Params WithRender(NVSDK_NGX_D3D12_DLSS_Eval_Params eval, const SrInputs& in) noexcept
{
    eval.InRenderSubrectDimensions = NVSDK_NGX_Dimensions{ in.render.width.Get(), in.render.height.Get() };
    eval.InReset = in.reset ? 1 : 0;
    return eval;
}

[[nodiscard]] NVSDK_NGX_D3D12_DLSS_Eval_Params WithNeutralExposure(NVSDK_NGX_D3D12_DLSS_Eval_Params eval) noexcept
{
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;
    return eval;
}

[[nodiscard]] NVSDK_NGX_D3D12_DLSS_Eval_Params EvalParamsOf(const SrInputs& in) noexcept
{
    NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
    eval.Feature = NVSDK_NGX_D3D12_Feature_Eval_Params{ in.io.color, in.io.output, 0.0f };
    eval.pInDepth = in.io.depth;
    eval.pInMotionVectors = in.io.motionVectors;
    return WithNeutralExposure(WithRender(eval, in));
}

[[nodiscard]] Result<Feature, Error> OwnedFeature(NVSDK_NGX_Handle* raw) noexcept
{
    if (raw == nullptr)
        return Fail(Error{ ApiCall::NgxCreateFeature, 0 });
    return Feature(raw);
}

[[nodiscard]] Result<Feature, Error> Created(NVSDK_NGX_Handle* raw, NVSDK_NGX_Result result) noexcept
{
    return CheckNgx(result, ApiCall::NgxCreateFeature).and_then([raw] { return OwnedFeature(raw); });
}

[[nodiscard]] Result<BoundNrParameters, Error> Bound(const interior::NrParameterList& list, const ResourceTable& table) noexcept
{
    return infra::FoldResult(list.Items(), Result<BoundNrParameters, Error>(BoundNrParameters{}),
                             [&table](const BoundNrParameters& acc, const interior::NrParameterValue& v) { return WithBound(acc, v, table); });
}

[[nodiscard]] Result<BoundNrParameters, Error> BoundOrFull(const Result<interior::NrParameterList, infra::CapacityExceeded>& list, const ResourceTable& table) noexcept
{
    return list.transform_error(FromCapacity).and_then([&table](const interior::NrParameterList& l) { return Bound(l, table); });
}

} // namespace

NgxPaths::NgxPaths(const NgxSettings& settings) noexcept
    : executable_(settings.executableDirectory), feature_(settings.featurePath), pointers_{ executable_.CString(), feature_.CString() },
      common_(CommonInfoOf(pointers_, PathCount(settings), settings.logLevel))
{
}

void NgxShutdown::operator()(ID3D12Device* device) const noexcept
{
    ENSURE(!NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_Shutdown1(device)));
}

void ParameterDestroyer::operator()(NVSDK_NGX_Parameter* parameters) const noexcept
{
    ENSURE(!NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_DestroyParameters(parameters)));
}

void FeatureReleaser::operator()(NVSDK_NGX_Handle* handle) const noexcept
{
    ENSURE(!NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_ReleaseFeature(handle)));
}

std::optional<interior::DirectoryPath> NeuralRenderingModelLocation(const NgxSettings& settings) noexcept
{
    return ModelIn(settings.executableDirectory).or_else([&settings] { return ModelIn(settings.featurePath); });
}

Requirement RequirementOf(const GpuDevice& gpu, const NgxSettings& settings, NVSDK_NGX_Feature feature) noexcept
{
    const NgxPaths paths{ settings };
    const NVSDK_NGX_FeatureDiscoveryInfo info{ NVSDK_NGX_Version_API, feature, IdentifierOf(settings), settings.dataPath.CString(), &paths.Common() };
    NVSDK_NGX_FeatureRequirement requirement{ static_cast<NVSDK_NGX_Feature_Support_Result>(0xFFFFFFFFu), 0, {} };
    const NVSDK_NGX_Result result = NVSDK_NGX_D3D12_GetFeatureRequirements(gpu.adapter.Get(), &info, &requirement);
    return Requirement{ result, static_cast<std::uint32_t>(requirement.FeatureSupported) };
}

// The model draws its own overlay, naming its version, the preset it resolved and the sizes it is
// working at, when this reads exactly 1024. It is read as the model loads, so it is asked for first.
[[nodiscard]] Status<Error> RequestIndicator(bool wanted) noexcept
{
    if (!wanted)
        return {};
    return CheckBool(::SetEnvironmentVariableW(L"__NGX_SHOW_INDICATOR", L"1024"), ApiCall::SetEnvironmentVariable);
}

Result<NgxRuntime, Error> CreateNgxRuntime(const GpuDevice& gpu, const NgxSettings& settings) noexcept
{
    const std::shared_ptr<const NgxPaths> paths = std::make_shared<const NgxPaths>(settings);
    return RequestIndicator(settings.indicator).and_then([&] { return CheckNgx(Init(settings, gpu.device.Get(), paths->Common()), ApiCall::NgxInit); }).and_then([&] {
        return Initialized(gpu, paths);
    });
}

Status<Error> RequireSuperResolution(const NgxRuntime& runtime) noexcept
{
    return RequireCurrentDriver(runtime.parameters.get()).and_then([&runtime] { return RequireAvailable(runtime.parameters.get()); });
}

std::optional<std::uint32_t> NeuralRenderingAvailability(const NgxRuntime& runtime) noexcept
{
    return UIntOf(runtime.parameters.get(), kNeuralRenderingAvailable);
}

interior::QualityTable QualityTableFor(const NgxRuntime& runtime, const interior::Extent& target) noexcept
{
    return std::ranges::fold_left(kQualities, interior::QualityTable{}, [&](const interior::QualityTable& acc, interior::SrQuality quality) {
        return WithRange(acc, RangeFrom(quality, Optimal(runtime.parameters.get(), target, PerfQualityOf(quality))));
    });
}

Result<Feature, Error> CreateSuperResolution(const NgxRuntime& runtime, ID3D12GraphicsCommandList* list, const interior::SrChoice& choice) noexcept
{
    NVSDK_NGX_DLSS_Create_Params create = CreateParamsOf(choice);
    NVSDK_NGX_Handle* raw = nullptr;
    return WritePresets(runtime.parameters.get(), choice.preset).and_then([&] { return Created(raw, NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &raw, runtime.parameters.get(), &create)); });
}

Result<Feature, Error> CreateNeuralRendering(const NgxRuntime& runtime, ID3D12GraphicsCommandList* list, const interior::NrTuning& tuning, const interior::Extent& work) noexcept
{
    NVSDK_NGX_Handle* raw = nullptr;
    return BoundOrFull(interior::NrCreationParameters(tuning, work), ResourceTable{})
        .and_then([&](const BoundNrParameters& parameters) { return WriteAll(runtime.parameters.get(), parameters); })
        .and_then([&] { return Created(raw, NVSDK_NGX_D3D12_CreateFeature(list, kNeuralRenderingFeature, runtime.parameters.get(), &raw)); });
}

Status<Error> EvaluateSuperResolution(const NgxRuntime& runtime, const Feature& feature, ID3D12GraphicsCommandList* list, const SrInputs& inputs) noexcept
{
    NVSDK_NGX_D3D12_DLSS_Eval_Params eval = EvalParamsOf(inputs);
    return CheckNgx(NGX_D3D12_EVALUATE_DLSS_EXT(list, feature.get(), runtime.parameters.get(), &eval), ApiCall::NgxEvaluateFeature);
}

Status<Error> EvaluateNeuralRendering(const NgxRuntime& runtime, const Feature& feature, ID3D12GraphicsCommandList* list, const interior::NrTuning& tuning, const interior::EvaluateNr& evaluate,
                                      const ResourceTable& resources) noexcept
{
    return BoundOrFull(interior::NrEvaluationParameters(tuning, evaluate), resources)
        .and_then([&](const BoundNrParameters& parameters) { return WriteAll(runtime.parameters.get(), parameters); })
        .and_then([&] { return CheckNgx(NVSDK_NGX_D3D12_EvaluateFeature(list, feature.get(), runtime.parameters.get(), nullptr), ApiCall::NgxEvaluateFeature); });
}

} // namespace real
