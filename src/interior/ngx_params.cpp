#include "interior/ngx_params.h"

#include "infrastructure/fold.h"

#include <array>

namespace interior {
namespace {

using Pair = NrParameterValue;

[[nodiscard]] Pair UInt(NrParameter name, std::uint32_t value) noexcept
{
    return Pair{ name, NgxValue{ value } };
}

[[nodiscard]] Pair Float(NrParameter name, float value) noexcept
{
    return Pair{ name, NgxValue{ value } };
}

[[nodiscard]] Pair Resource(NrParameter name, ResourceId id) noexcept
{
    return Pair{ name, NgxValue{ id } };
}

[[nodiscard]] std::uint32_t BoolCode(bool value) noexcept
{
    return value ? 1u : 0u;
}

[[nodiscard]] Result<NrParameterList, infra::CapacityExceeded> PushAll(const NrParameterList& list, std::span<const Pair> pairs) noexcept
{
    return infra::FoldResult(pairs, Result<NrParameterList, infra::CapacityExceeded>(list), [](const NrParameterList& acc, const Pair& pair) { return acc.Push(pair); });
}

[[nodiscard]] std::array<Pair, 8> TuningPairs(const NrTuning& t) noexcept
{
    return { UInt(NrParameter::HintRenderPreset, t.preset.Get()),
             Float(NrParameter::Intensity, t.intensity.Get()),
             UInt(NrParameter::Style, StyleCode(t.style)),
             Float(NrParameter::LocalStructureStrength, t.localStructure.Get()),
             Float(NrParameter::LocalToneStrength, t.localTone.Get()),
             Float(NrParameter::SkinStructureStrength, t.skinStructure.Get()),
             UInt(NrParameter::UseAutoMask, BoolCode(t.autoMask)),
             UInt(NrParameter::UiCorrection, BoolCode(t.uiCorrection)) };
}

[[nodiscard]] std::array<Pair, 5> CreationPairs(const Extent& work) noexcept
{
    return { UInt(NrParameter::Enabled, 1u), UInt(NrParameter::Width, work.width.Get()), UInt(NrParameter::Height, work.height.Get()), UInt(NrParameter::CreationNodeMask, 1u),
             UInt(NrParameter::VisibilityNodeMask, 1u) };
}

[[nodiscard]] std::array<Pair, 9> ResourcePairs(const EvaluateNr& e) noexcept
{
    return { Resource(NrParameter::Color, e.io.color),       Resource(NrParameter::Depth, e.io.depth), Resource(NrParameter::MVec, e.io.motionVectors),
             Resource(NrParameter::Output, e.io.output),     UInt(NrParameter::Enabled, 1u),           UInt(NrParameter::Width, e.work.width.Get()),
             UInt(NrParameter::Height, e.work.height.Get()), UInt(NrParameter::DepthInverted, 0u),     UInt(NrParameter::Reset, BoolCode(e.reset)) };
}

[[nodiscard]] std::array<Pair, 8> ColorSubrectPairs(const EvaluateNr& e) noexcept
{
    return { UInt(NrParameter::ColorSubrectBaseX, 0u),
             UInt(NrParameter::ColorSubrectBaseY, 0u),
             UInt(NrParameter::ColorSubrectWidth, e.work.width.Get()),
             UInt(NrParameter::ColorSubrectHeight, e.work.height.Get()),
             UInt(NrParameter::OutputSubrectBaseX, 0u),
             UInt(NrParameter::OutputSubrectBaseY, 0u),
             UInt(NrParameter::OutputSubrectWidth, e.work.width.Get()),
             UInt(NrParameter::OutputSubrectHeight, e.work.height.Get()) };
}

[[nodiscard]] std::array<Pair, 10> GuidePairs(const EvaluateNr& e) noexcept
{
    return { UInt(NrParameter::DepthSubrectBaseX, 0u),
             UInt(NrParameter::DepthSubrectBaseY, 0u),
             UInt(NrParameter::DepthSubrectWidth, e.guide.width.Get()),
             UInt(NrParameter::DepthSubrectHeight, e.guide.height.Get()),
             UInt(NrParameter::MVecSubrectBaseX, 0u),
             UInt(NrParameter::MVecSubrectBaseY, 0u),
             UInt(NrParameter::MVecSubrectWidth, e.guide.width.Get()),
             UInt(NrParameter::MVecSubrectHeight, e.guide.height.Get()),
             Float(NrParameter::MVecScaleX, e.mvScaleX.Get()),
             Float(NrParameter::MVecScaleY, e.mvScaleY.Get()) };
}

} // namespace

std::string_view NameOf(NrParameter parameter) noexcept
{
    switch (parameter)
    {
    case NrParameter::Enabled: return "DLSSNR.Enabled";
    case NrParameter::Width: return "DLSSNR.Width";
    case NrParameter::Height: return "DLSSNR.Height";
    case NrParameter::CreationNodeMask: return "CreationNodeMask";
    case NrParameter::VisibilityNodeMask: return "VisibilityNodeMask";
    case NrParameter::HintRenderPreset: return "DLSSNR.Hint.Render.Preset";
    case NrParameter::Intensity: return "DLSSNR.Intensity";
    case NrParameter::Style: return "DLSSNR.Style";
    case NrParameter::LocalStructureStrength: return "DLSSNR.LocalStructureStrength";
    case NrParameter::LocalToneStrength: return "DLSSNR.LocalToneStrength";
    case NrParameter::SkinStructureStrength: return "DLSSNR.SkinStructureStrength";
    case NrParameter::UseAutoMask: return "DLSSNR.UseAutoMask";
    case NrParameter::UiCorrection: return "DLSSNR.UICorrection";
    case NrParameter::Color: return "DLSSNR.Color";
    case NrParameter::Depth: return "DLSSNR.Depth";
    case NrParameter::MVec: return "DLSSNR.MVec";
    case NrParameter::Output: return "DLSSNR.Output";
    case NrParameter::DepthInverted: return "DLSSNR.DepthInverted";
    case NrParameter::Reset: return "DLSSNR.Reset";
    case NrParameter::ColorSubrectBaseX: return "DLSSNR.ColorSubrectBaseX";
    case NrParameter::ColorSubrectBaseY: return "DLSSNR.ColorSubrectBaseY";
    case NrParameter::ColorSubrectWidth: return "DLSSNR.ColorSubrectWidth";
    case NrParameter::ColorSubrectHeight: return "DLSSNR.ColorSubrectHeight";
    case NrParameter::OutputSubrectBaseX: return "DLSSNR.OutputSubrectBaseX";
    case NrParameter::OutputSubrectBaseY: return "DLSSNR.OutputSubrectBaseY";
    case NrParameter::OutputSubrectWidth: return "DLSSNR.OutputSubrectWidth";
    case NrParameter::OutputSubrectHeight: return "DLSSNR.OutputSubrectHeight";
    case NrParameter::DepthSubrectBaseX: return "DLSSNR.DepthSubrectBaseX";
    case NrParameter::DepthSubrectBaseY: return "DLSSNR.DepthSubrectBaseY";
    case NrParameter::DepthSubrectWidth: return "DLSSNR.DepthSubrectWidth";
    case NrParameter::DepthSubrectHeight: return "DLSSNR.DepthSubrectHeight";
    case NrParameter::MVecSubrectBaseX: return "DLSSNR.MVecSubrectBaseX";
    case NrParameter::MVecSubrectBaseY: return "DLSSNR.MVecSubrectBaseY";
    case NrParameter::MVecSubrectWidth: return "DLSSNR.MVecSubrectWidth";
    case NrParameter::MVecSubrectHeight: return "DLSSNR.MVecSubrectHeight";
    case NrParameter::MVecScaleX: return "DLSSNR.MVecScaleX";
    case NrParameter::MVecScaleY: return "DLSSNR.MVecScaleY";
    }
    return "";
}

std::uint32_t StyleCode(NrStyle style) noexcept
{
    switch (style)
    {
    case NrStyle::Standard: return 0;
    case NrStyle::Natural: return 1;
    case NrStyle::Cinematic: return 2;
    }
    return 0;
}

Result<NrParameterList, infra::CapacityExceeded> NrCreationParameters(const NrTuning& tuning, const Extent& work) noexcept
{
    return PushAll(NrParameterList{}, CreationPairs(work)).and_then([&tuning](const NrParameterList& list) { return PushAll(list, TuningPairs(tuning)); });
}

Result<NrParameterList, infra::CapacityExceeded> NrEvaluationParameters(const NrTuning& tuning, const EvaluateNr& evaluate) noexcept
{
    return PushAll(NrParameterList{}, ResourcePairs(evaluate))
        .and_then([&evaluate](const NrParameterList& list) { return PushAll(list, ColorSubrectPairs(evaluate)); })
        .and_then([&evaluate](const NrParameterList& list) { return PushAll(list, GuidePairs(evaluate)); })
        .and_then([&tuning](const NrParameterList& list) { return PushAll(list, TuningPairs(tuning)); });
}

} // namespace interior
