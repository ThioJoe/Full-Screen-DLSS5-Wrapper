#pragma once
#include "infrastructure/bounded_vector.h"
#include "interior/enums.h"
#include "interior/monitors.h"
#include "interior/options.h"
#include "interior/units.h"

#include <optional>

namespace interior {

struct QualityRange
{
    SrQuality quality;
    Extent optimal;
    Extent minimum;
    Extent maximum;
    [[nodiscard]] friend constexpr bool operator==(const QualityRange&, const QualityRange&) noexcept = default;
};

using QualityTable = infra::BoundedVector<QualityRange, 6>;

struct SrChoice
{
    SrQuality quality;
    Extent input;
    Extent output;
    SrPreset preset;
    bool hdr;
    [[nodiscard]] friend constexpr bool operator==(const SrChoice&, const SrChoice&) noexcept = default;
};

struct SessionPlan
{
    Extent source;
    Extent target;
    Extent work;
    std::optional<SrChoice> superResolution;
    bool neuralRendering;
    NrTuning tuning;
    MotionBackend motion;
    LevelCount levels;
    LevelIndex finestLevel;
    GridSize nvofGrid;
    PerfLevel nvofPerf;
    Extent flowExtent;
    MotionScale mvScaleX;
    MotionScale mvScaleY;
    bool depthInverted;
    DepthValue depth;
    Fraction resetThreshold;
    ColorFormat format;
    DisplayMode initialDisplay;
    bool captureCursor;
    bool vsync;
    [[nodiscard]] friend constexpr bool operator==(const SessionPlan&, const SessionPlan&) noexcept = default;
};

enum class PlanError : std::uint8_t { SuperResolutionCannotBridge, ScaleOutOfRange, Arithmetic, Levels };

[[nodiscard]] bool WantsSuperResolution(const Options& options, const Extent& source, const Extent& target) noexcept;
[[nodiscard]] std::optional<SrQuality> ChooseQuality(const QualityTable& table, const Extent& input, const Extent& output) noexcept;
[[nodiscard]] Result<SessionPlan, PlanError> PlanSession(const Options& options, const Geometry& geometry, const QualityTable& table) noexcept;
[[nodiscard]] DisplayMode InitialDisplay(CompareMode compare) noexcept;
// What the session starts running with, which is what the panel starts showing. The motion scales are the
// planner's answer rather than the options', so a session left to work them out shows the numbers it uses.
[[nodiscard]] LiveSettings StartingLive(const SessionPlan& plan) noexcept;
[[nodiscard]] std::string_view Describe(PlanError error) noexcept;
[[nodiscard]] std::string_view Describe(SrQuality quality) noexcept;

} // namespace interior
