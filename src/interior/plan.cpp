#include "interior/plan.h"

#include "infrastructure/fold.h"
#include "interior/pyramid.h"

#include <algorithm>
#include <array>

namespace interior {
namespace {

using infra::Fail;

using QualityOrder = std::array<SrQuality, 6>;

constexpr QualityOrder kNativeOrder{ SrQuality::Dlaa, SrQuality::Quality, SrQuality::UltraQuality, SrQuality::Balanced, SrQuality::Performance, SrQuality::UltraPerformance };
constexpr QualityOrder kQualityOrder{ SrQuality::Quality, SrQuality::Balanced, SrQuality::UltraQuality, SrQuality::Dlaa, SrQuality::Performance, SrQuality::UltraPerformance };
constexpr QualityOrder kBalancedOrder{ SrQuality::Balanced, SrQuality::Quality, SrQuality::Performance, SrQuality::UltraQuality, SrQuality::UltraPerformance, SrQuality::Dlaa };
constexpr QualityOrder kPerformanceOrder{ SrQuality::Performance, SrQuality::Balanced, SrQuality::UltraPerformance, SrQuality::Quality, SrQuality::UltraQuality, SrQuality::Dlaa };
constexpr QualityOrder kUltraOrder{ SrQuality::UltraPerformance, SrQuality::Performance, SrQuality::Balanced, SrQuality::Quality, SrQuality::UltraQuality, SrQuality::Dlaa };

[[nodiscard]] float Ratio(const Extent& input, const Extent& output) noexcept
{
    return static_cast<float>(output.width.Get()) / static_cast<float>(input.width.Get());
}

[[nodiscard]] const QualityOrder& OrderAbovePerformance(float ratio) noexcept
{
    return ratio <= 2.4f ? kPerformanceOrder : kUltraOrder;
}

[[nodiscard]] const QualityOrder& OrderAboveQuality(float ratio) noexcept
{
    return ratio <= 1.85f ? kBalancedOrder : OrderAbovePerformance(ratio);
}

[[nodiscard]] const QualityOrder& OrderAboveNative(float ratio) noexcept
{
    return ratio <= 1.6f ? kQualityOrder : OrderAboveQuality(ratio);
}

[[nodiscard]] const QualityOrder& OrderFor(float ratio) noexcept
{
    return ratio <= 1.05f ? kNativeOrder : OrderAboveNative(ratio);
}

[[nodiscard]] bool IsWithin(PixelCount value, PixelCount low, PixelCount high) noexcept
{
    return value >= low && value <= high;
}

[[nodiscard]] bool ContainsWidth(const QualityRange& range, const Extent& input) noexcept
{
    return IsWithin(input.width, range.minimum.width, range.maximum.width);
}

[[nodiscard]] bool ContainsHeight(const QualityRange& range, const Extent& input) noexcept
{
    return IsWithin(input.height, range.minimum.height, range.maximum.height);
}

[[nodiscard]] bool Contains(const QualityRange& range, const Extent& input) noexcept
{
    return ContainsWidth(range, input) && ContainsHeight(range, input);
}

[[nodiscard]] std::optional<QualityRange> RangeOf(const QualityTable& table, SrQuality quality) noexcept
{
    const auto items = table.Items();
    const auto found = std::ranges::find_if(items, [quality](const QualityRange& r) { return r.quality == quality; });
    if (found == items.end())
        return std::nullopt;
    return *found;
}

[[nodiscard]] bool Fits(const QualityTable& table, SrQuality quality, const Extent& input) noexcept
{
    const std::optional<QualityRange> range = RangeOf(table, quality);
    return range.has_value() && Contains(*range, input);
}

[[nodiscard]] bool IsSameExtent(const Extent& a, const Extent& b) noexcept
{
    return a == b;
}

[[nodiscard]] bool IsAutoUpscale(const Options& options, const Extent& source, const Extent& target) noexcept
{
    return options.sr == SrMode::Auto && !IsSameExtent(source, target);
}

[[nodiscard]] Result<Scale, PlanError> ScaleBetween(PixelCount work, PixelCount source) noexcept
{
    return ScaleTag::Parse(static_cast<float>(work.Get()) / static_cast<float>(source.Get())).transform_error([](UnitError) { return PlanError::ScaleOutOfRange; });
}

[[nodiscard]] bool WantsCursor(const Options& options, const Geometry& geometry) noexcept
{
    switch (options.cursor)
    {
    case CursorMode::On: return true;
    case CursorMode::Off: return false;
    case CursorMode::Auto: return !IsSameRect(geometry.sourceRect, geometry.targetRect);
    }
    return false;
}

[[nodiscard]] Result<std::optional<SrChoice>, PlanError> RequiredSuperResolution(const Options& options, const Geometry& geometry, const QualityTable& table) noexcept
{
    const std::optional<SrQuality> quality = ChooseQuality(table, geometry.sourceExtent, geometry.targetExtent);
    if (!quality.has_value())
        return Fail(PlanError::SuperResolutionCannotBridge);
    return std::optional<SrChoice>{ SrChoice{ *quality, geometry.sourceExtent, geometry.targetExtent, options.srPreset, options.format == ColorFormat::Rgba16f } };
}

[[nodiscard]] Result<std::optional<SrChoice>, PlanError> ChooseSuperResolution(const Options& options, const Geometry& geometry, const QualityTable& table) noexcept
{
    if (!WantsSuperResolution(options, geometry.sourceExtent, geometry.targetExtent))
        return std::optional<SrChoice>{};
    return RequiredSuperResolution(options, geometry, table);
}

[[nodiscard]] Extent WorkExtent(const std::optional<SrChoice>& sr, const Extent& source) noexcept
{
    return sr.has_value() ? sr->output : source;
}

[[nodiscard]] LevelIndex ClampedFinest(LevelIndex requested, LevelCount levels) noexcept
{
    const Result<LevelIndex, UnitError> clamped = LevelIndexTag::Parse(std::min(requested.Get(), levels.Get() - 1));
    ENSURE(clamped.has_value());
    return *clamped;
}

// The model is told how far a motion vector reaches at its own working size, which is the ratio between
// that size and the captured one. An operator who asks for a scale of their own is given it instead.
[[nodiscard]] MotionScale ChosenScale(const std::optional<MotionScale>& asked, Scale ratio) noexcept
{
    return asked.value_or(MotionScaleTag::Of(ratio));
}

[[nodiscard]] SessionPlan Assemble(const Options& o, const Geometry& g, const std::optional<SrChoice>& sr, const Extent& work, const Extent& flow, Scale scaleX, Scale scaleY) noexcept
{
    const LevelCount levels = LevelCountFor(g.sourceExtent);
    return SessionPlan{ g.sourceExtent,
                        g.targetExtent,
                        work,
                        sr,
                        o.neuralRendering,
                        o.tuning,
                        o.motion,
                        levels,
                        ClampedFinest(o.motionFinestLevel, levels),
                        o.nvofGrid,
                        o.nvofPerf,
                        flow,
                        ChosenScale(o.mvScaleX, scaleX),
                        ChosenScale(o.mvScaleY, scaleY),
                        o.depthInverted,
                        o.depthValue,
                        o.resetThreshold,
                        o.format,
                        InitialDisplay(o.compare),
                        WantsCursor(o, g),
                        o.vsync };
}

[[nodiscard]] Result<SessionPlan, PlanError> WithScales(const Options& o, const Geometry& g, const std::optional<SrChoice>& sr, const Extent& work, const Extent& flow) noexcept
{
    return ScaleBetween(work.width, g.sourceExtent.width).and_then([&](Scale scaleX) {
        return ScaleBetween(work.height, g.sourceExtent.height).transform([&](Scale scaleY) { return Assemble(o, g, sr, work, flow, scaleX, scaleY); });
    });
}

} // namespace

bool WantsSuperResolution(const Options& options, const Extent& source, const Extent& target) noexcept
{
    return options.sr == SrMode::Dlaa || IsAutoUpscale(options, source, target);
}

std::optional<SrQuality> ChooseQuality(const QualityTable& table, const Extent& input, const Extent& output) noexcept
{
    const QualityOrder& order = OrderFor(Ratio(input, output));
    const auto found = std::ranges::find_if(order, [&](SrQuality q) { return Fits(table, q, input); });
    if (found == order.end())
        return std::nullopt;
    return *found;
}

LiveSettings StartingLive(const SessionPlan& plan) noexcept
{
    return LiveSettings{ plan.neuralRendering, plan.tuning, plan.depthInverted, plan.mvScaleX, plan.mvScaleY, plan.vsync, plan.resetThreshold, plan.depth };
}

DisplayMode InitialDisplay(CompareMode compare) noexcept
{
    switch (compare)
    {
    case CompareMode::Off: return DisplayMode::Processed;
    case CompareMode::Split: return DisplayMode::Split;
    case CompareMode::Original: return DisplayMode::Original;
    }
    return DisplayMode::Processed;
}

Result<SessionPlan, PlanError> PlanSession(const Options& options, const Geometry& geometry, const QualityTable& table) noexcept
{
    return ChooseSuperResolution(options, geometry, table).and_then([&](const std::optional<SrChoice>& sr) {
        return GridExtent(geometry.sourceExtent, GridCells(options.nvofGrid)).transform_error([](PyramidError) { return PlanError::Arithmetic; }).and_then([&](const Extent& flow) {
            return WithScales(options, geometry, sr, WorkExtent(sr, geometry.sourceExtent), flow);
        });
    });
}

std::string_view Describe(PlanError error) noexcept
{
    switch (error)
    {
    case PlanError::SuperResolutionCannotBridge: return "DLSS cannot scale between the source and target sizes; pass --sr off to scale bilinearly";
    case PlanError::ScaleOutOfRange: return "the target is too many times larger than the source";
    case PlanError::Arithmetic: return "arithmetic overflow while planning";
    case PlanError::Levels: return "no usable pyramid level";
    }
    return "planning error";
}

std::string_view Describe(SrQuality quality) noexcept
{
    switch (quality)
    {
    case SrQuality::Dlaa: return "DLAA";
    case SrQuality::UltraQuality: return "Ultra Quality";
    case SrQuality::Quality: return "Quality";
    case SrQuality::Balanced: return "Balanced";
    case SrQuality::Performance: return "Performance";
    case SrQuality::UltraPerformance: return "Ultra Performance";
    }
    return "?";
}

} // namespace interior
