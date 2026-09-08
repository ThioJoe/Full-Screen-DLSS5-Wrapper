// WAIVER(R2): test suites accumulate failure counts and drive generated sequences with loops.
#include "interior/plan.h"
#include "interior/pyramid.h"
#include "tests/test_registry.h"

#include <algorithm>
#include <array>
#include <ranges>

namespace tests {
namespace {

using namespace interior;

[[nodiscard]] Extent RandomExtent(infra::RngState& rng) noexcept
{
    const auto w = PixelCountTag::Parse(proptest::DrawBetween(rng, 16, 8192));
    const auto h = PixelCountTag::Parse(proptest::DrawBetween(rng, 16, 4320));
    REQUIRE(w.has_value() && h.has_value());
    return Extent{ *w, *h };
}

[[nodiscard]] QualityRange RangeFor(SrQuality quality, const Extent& output, std::uint32_t minPercent, std::uint32_t maxPercent) noexcept
{
    const auto scale = [](PixelCount v, std::uint32_t percent) { return PixelCountTag::Parse(std::max(1u, v.Get() * percent / 100u)); };
    const auto minW = scale(output.width, minPercent);
    const auto minH = scale(output.height, minPercent);
    const auto maxW = scale(output.width, maxPercent);
    const auto maxH = scale(output.height, maxPercent);
    REQUIRE(minW.has_value() && minH.has_value() && maxW.has_value() && maxH.has_value());
    return QualityRange{ quality, Extent{ *minW, *minH }, Extent{ *minW, *minH }, Extent{ *maxW, *maxH } };
}

[[nodiscard]] QualityTable TableFor(const Extent& output) noexcept
{
    QualityTable table;
    table = table.Push(RangeFor(SrQuality::Dlaa, output, 100, 100)).value_or(table);
    table = table.Push(RangeFor(SrQuality::Quality, output, 50, 99)).value_or(table);
    table = table.Push(RangeFor(SrQuality::Balanced, output, 50, 99)).value_or(table);
    table = table.Push(RangeFor(SrQuality::Performance, output, 50, 99)).value_or(table);
    table = table.Push(RangeFor(SrQuality::UltraPerformance, output, 33, 50)).value_or(table);
    return table;
}

[[nodiscard]] Geometry GeometryFor(const Extent& source, const Extent& target) noexcept
{
    const auto zero = CoordinateTag::Parse(0);
    const auto sr = CoordinateTag::Parse(static_cast<std::int32_t>(source.width.Get())).and_then([&](Coordinate r) {
        return CoordinateTag::Parse(static_cast<std::int32_t>(source.height.Get())).and_then([&](Coordinate b) { return ScreenRectTag::Parse(*zero, *zero, r, b); });
    });
    const auto tr = CoordinateTag::Parse(static_cast<std::int32_t>(target.width.Get())).and_then([&](Coordinate r) {
        return CoordinateTag::Parse(static_cast<std::int32_t>(target.height.Get())).and_then([&](Coordinate b) { return ScreenRectTag::Parse(*zero, *zero, r, b); });
    });
    REQUIRE(sr.has_value() && tr.has_value());
    return Geometry{ MonitorList{}, *sr, *tr, source, target };
}

[[nodiscard]] bool ChosenQualityContainsInput(infra::RngState& rng) noexcept
{
    const Extent input = RandomExtent(rng);
    const Extent output = RandomExtent(rng);
    const QualityTable table = TableFor(output);
    const std::optional<SrQuality> quality = ChooseQuality(table, input, output);
    if (!quality.has_value())
        return true;
    const auto items = table.Items();
    const auto range = std::ranges::find_if(items, [&](const QualityRange& r) { return r.quality == *quality; });
    return range != items.end() && input.width >= (*range).minimum.width && input.width <= (*range).maximum.width;
}

[[nodiscard]] bool SameSizePrefersDlaa(infra::RngState& rng) noexcept
{
    const Extent size = RandomExtent(rng);
    return ChooseQuality(TableFor(size), size, size) == std::optional<SrQuality>{ SrQuality::Dlaa };
}

[[nodiscard]] bool WorkExtentIsTargetOnlyWithSr(infra::RngState& rng) noexcept
{
    const Extent source = RandomExtent(rng);
    const Extent target = RandomExtent(rng);
    const auto plan = PlanSession(DefaultOptions(), GeometryFor(source, target), TableFor(target));
    if (!plan.has_value())
        return true;
    return plan->superResolution.has_value() ? plan->work == target : plan->work == source;
}

[[nodiscard]] bool ScalesMatchWorkOverSource(infra::RngState& rng) noexcept
{
    const Extent source = RandomExtent(rng);
    const Extent target = RandomExtent(rng);
    const auto plan = PlanSession(DefaultOptions(), GeometryFor(source, target), TableFor(target));
    if (!plan.has_value())
        return true;
    const float expected = static_cast<float>(plan->work.width.Get()) / static_cast<float>(source.width.Get());
    return plan->mvScaleX.Get() == expected;
}

[[nodiscard]] bool SrOffNeverPlansSuperResolution(infra::RngState& rng) noexcept
{
    const Extent source = RandomExtent(rng);
    const Extent target = RandomExtent(rng);
    const std::array<std::wstring_view, 1> args{ L"--sr=off" };
    const auto options = ParseOptions(args);
    const auto plan = PlanSession(*options, GeometryFor(source, target), TableFor(target));
    return plan.has_value() && !plan->superResolution.has_value();
}

[[nodiscard]] bool LevelExtentsHalveAndStayPositive(infra::RngState& rng) noexcept
{
    const Extent source = RandomExtent(rng);
    const LevelCount levels = LevelCountFor(source);
    const auto extents = LevelExtentsOf(source, levels);
    if (!extents.has_value() || extents->Size() != levels.Get())
        return false;
    return std::ranges::all_of(std::views::iota(std::size_t{ 1 }, extents->Size()), [&](std::size_t i) { return extents->At(i).width.Get() == std::max(1u, extents->At(i - 1).width.Get() / 2); });
}

[[nodiscard]] bool GroupsCoverExtent(infra::RngState& rng) noexcept
{
    const Extent e = RandomExtent(rng);
    const auto groups = GroupsFor(e);
    return groups.has_value() && groups->x.Get() * kGroupSize >= e.width.Get() && (groups->x.Get() - 1) * kGroupSize < e.width.Get();
}

} // namespace

std::uint32_t PlanSuite(std::uint64_t seed) noexcept
{
    std::uint32_t failures = 0;
    failures += Failures(proptest::ForAll("chosen quality contains the input", seed, 1000, ChosenQualityContainsInput));
    failures += Failures(proptest::ForAll("same size prefers DLAA", seed, 200, SameSizePrefersDlaa));
    failures += Failures(proptest::ForAll("work extent is the target only with SR", seed, 500, WorkExtentIsTargetOnlyWithSr));
    failures += Failures(proptest::ForAll("motion scales are work over source", seed, 500, ScalesMatchWorkOverSource));
    failures += Failures(proptest::ForAll("--sr off never plans SR", seed, 300, SrOffNeverPlansSuperResolution));
    failures += Failures(proptest::ForAll("pyramid levels halve and stay positive", seed, 500, LevelExtentsHalveAndStayPositive));
    failures += Failures(proptest::ForAll("thread groups cover the extent", seed, 500, GroupsCoverExtent));
    return failures;
}

} // namespace tests
