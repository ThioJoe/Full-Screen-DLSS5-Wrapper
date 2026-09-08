#pragma once
#include "effects/real/resources.h"
#include "interior/ngx_params.h"
#include "interior/plan.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <array>
#include <memory>
#include <optional>
#include <variant>

namespace real {

constexpr NVSDK_NGX_Feature kNeuralRenderingFeature = static_cast<NVSDK_NGX_Feature>(18);

struct NgxSettings
{
    std::optional<interior::NgxAppId> appId;
    interior::ProjectIdText projectId;
    interior::DirectoryPath dataPath;
    interior::DirectoryPath executableDirectory;
    interior::DirectoryPath featurePath;
    interior::NgxLogLevel logLevel;
};

// The driver keeps the path pointers, so an NgxPaths lives on the heap and never moves.
class NgxPaths final
{
public:
    explicit NgxPaths(const NgxSettings& settings) noexcept;
    [[nodiscard]] const NVSDK_NGX_FeatureCommonInfo& Common() const noexcept { return common_; }

private:
    interior::DirectoryPath executable_;
    interior::DirectoryPath feature_;
    std::array<const wchar_t*, 2> pointers_;
    NVSDK_NGX_FeatureCommonInfo common_;
};

struct NgxShutdown
{
    void operator()(ID3D12Device* device) const noexcept;
};

struct ParameterDestroyer
{
    void operator()(NVSDK_NGX_Parameter* parameters) const noexcept;
};

struct FeatureReleaser
{
    void operator()(NVSDK_NGX_Handle* handle) const noexcept;
};

using NgxSession = std::unique_ptr<ID3D12Device, NgxShutdown>;
using NgxParameters = std::unique_ptr<NVSDK_NGX_Parameter, ParameterDestroyer>;
using Feature = std::unique_ptr<NVSDK_NGX_Handle, FeatureReleaser>;

struct NgxRuntime
{
    std::shared_ptr<const NgxPaths> paths;
    Com<ID3D12Device> device;
    NgxSession session;
    NgxParameters parameters;
};

struct Requirement
{
    NVSDK_NGX_Result result;
    std::uint32_t supportMask;
};

struct ModelIo
{
    ID3D12Resource* color;
    ID3D12Resource* depth;
    ID3D12Resource* motionVectors;
    ID3D12Resource* output;
};

struct SrInputs
{
    ModelIo io;
    interior::Extent render;
    bool reset;
};

using NgxSlot = std::variant<unsigned int, float, ID3D12Resource*>;

struct BoundNrParameter
{
    interior::NrParameter name;
    NgxSlot value;
};

using BoundNrParameters = infra::BoundedVector<BoundNrParameter, interior::NrParameterList::Capacity>;

[[nodiscard]] Requirement RequirementOf(const GpuDevice& gpu, const NgxSettings& settings, NVSDK_NGX_Feature feature) noexcept;
[[nodiscard]] infra::Result<NgxRuntime, Error> CreateNgxRuntime(const GpuDevice& gpu, const NgxSettings& settings) noexcept;
[[nodiscard]] infra::Status<Error> RequireSuperResolution(const NgxRuntime& runtime) noexcept;
[[nodiscard]] std::optional<std::uint32_t> NeuralRenderingAvailability(const NgxRuntime& runtime) noexcept;
[[nodiscard]] interior::QualityTable QualityTableFor(const NgxRuntime& runtime, const interior::Extent& target) noexcept;
[[nodiscard]] infra::Result<Feature, Error> CreateSuperResolution(const NgxRuntime& runtime, ID3D12GraphicsCommandList* list, const interior::SrChoice& choice) noexcept;
[[nodiscard]] infra::Result<Feature, Error> CreateNeuralRendering(const NgxRuntime& runtime, ID3D12GraphicsCommandList* list, const interior::NrTuning& tuning, const interior::Extent& work) noexcept;
[[nodiscard]] infra::Status<Error> EvaluateSuperResolution(const NgxRuntime& runtime, const Feature& feature, ID3D12GraphicsCommandList* list, const SrInputs& inputs) noexcept;
[[nodiscard]] infra::Status<Error> EvaluateNeuralRendering(const NgxRuntime& runtime, const Feature& feature, ID3D12GraphicsCommandList* list, const interior::NrTuning& tuning,
                                                           const interior::EvaluateNr& evaluate, const ResourceTable& resources) noexcept;

} // namespace real
