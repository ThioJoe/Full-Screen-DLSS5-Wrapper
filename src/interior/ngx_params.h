#pragma once
#include "infrastructure/bounded_vector.h"
#include "interior/frame.h"
#include "interior/options.h"

#include <string_view>
#include <variant>

namespace interior {

// Names accepted by nvngx_dlssnr.dll (NGX feature 18), as established by OptiScaler's dlssnr module.
enum class NrParameter : std::uint8_t
{
    Enabled, Width, Height, CreationNodeMask, VisibilityNodeMask, HintRenderPreset, Intensity, Style, LocalStructureStrength,
    LocalToneStrength, SkinStructureStrength, UseAutoMask, UiCorrection, Color, Depth, MVec, Output, DepthInverted, Reset,
    ColorSubrectBaseX, ColorSubrectBaseY, ColorSubrectWidth, ColorSubrectHeight, OutputSubrectBaseX, OutputSubrectBaseY,
    OutputSubrectWidth, OutputSubrectHeight, DepthSubrectBaseX, DepthSubrectBaseY, DepthSubrectWidth, DepthSubrectHeight,
    MVecSubrectBaseX, MVecSubrectBaseY, MVecSubrectWidth, MVecSubrectHeight, MVecScaleX, MVecScaleY,
};

using NgxValue = std::variant<std::uint32_t, float, ResourceId>;

struct NrParameterValue
{
    NrParameter name;
    NgxValue value;
    [[nodiscard]] friend constexpr bool operator==(const NrParameterValue&, const NrParameterValue&) noexcept = default;
};

using NrParameterList = infra::BoundedVector<NrParameterValue, 48>;

[[nodiscard]] std::string_view NameOf(NrParameter parameter) noexcept;
[[nodiscard]] std::uint32_t StyleCode(NrStyle style) noexcept;
[[nodiscard]] Result<NrParameterList, infra::CapacityExceeded> NrCreationParameters(const NrTuning& tuning, const Extent& work) noexcept;
[[nodiscard]] Result<NrParameterList, infra::CapacityExceeded> NrEvaluationParameters(const NrTuning& tuning, const EvaluateNr& evaluate) noexcept;

} // namespace interior
