// WAIVER(R2): test suites accumulate failure counts and drive generated sequences with loops.
#include "interior/monitors.h"
#include "tests/test_registry.h"

#include <algorithm>

namespace tests {
namespace {

using namespace interior;

[[nodiscard]] std::optional<MonitorInfo> RandomMonitor(infra::RngState& rng, std::uintptr_t handle, bool primary) noexcept
{
    const std::int32_t left = static_cast<std::int32_t>(proptest::DrawBelow(rng, 8000)) - 4000;
    const std::int32_t top = static_cast<std::int32_t>(proptest::DrawBelow(rng, 4000)) - 2000;
    const std::int32_t width = static_cast<std::int32_t>(proptest::DrawBetween(rng, 640, 3840));
    const std::int32_t height = static_cast<std::int32_t>(proptest::DrawBetween(rng, 480, 2160));
    const auto rect = CoordinateTag::Parse(left).and_then([&](Coordinate l)
    {
        return CoordinateTag::Parse(top).and_then([&](Coordinate t)
        {
            return CoordinateTag::Parse(left + width).and_then([&](Coordinate r) { return CoordinateTag::Parse(top + height).and_then([&](Coordinate b) { return ScreenRectTag::Parse(l, t, r, b); }); });
        });
    });
    const auto h = MonitorHandleTag::Parse(handle);
    if (!rect.has_value() || !h.has_value())
        return std::nullopt;
    return MonitorInfo{ *h, *rect, primary, DeviceName{} };
}

[[nodiscard]] MonitorList RandomMonitors(infra::RngState& rng) noexcept
{
    const std::uint32_t count = proptest::DrawBetween(rng, 1, 6);
    const std::uint32_t primaryAt = proptest::DrawBelow(rng, count);
    MonitorList list;
    for (std::uint32_t i = 0; i < count; ++i) // WAIVER(R2): test generator.
    {
        const std::optional<MonitorInfo> m = RandomMonitor(rng, 0x1000u + i, i == primaryAt);
        if (m.has_value())
            list = list.Push(*m).value_or(list);
    }
    return list;
}

[[nodiscard]] bool OrderedPutsPrimaryFirst(infra::RngState& rng) noexcept
{
    const MonitorList monitors = RandomMonitors(rng);
    const MonitorList ordered = Ordered(monitors);
    return ordered.Size() == monitors.Size() && ordered.At(0).primary;
}

[[nodiscard]] bool OrderedIsSortedByRank(infra::RngState& rng) noexcept
{
    const MonitorList ordered = Ordered(RandomMonitors(rng));
    return std::ranges::is_sorted(ordered.Items(), [](const MonitorInfo& a, const MonitorInfo& b) { return ComesBefore(a, b); });
}

[[nodiscard]] bool UnionContainsEveryMonitor(infra::RngState& rng) noexcept
{
    const MonitorList monitors = RandomMonitors(rng);
    const auto rect = UnionRect(monitors);
    return rect.has_value() && std::ranges::all_of(monitors.Items(), [&](const MonitorInfo& m)
    {
        return m.rect.Left() >= rect->Left() && m.rect.Right() <= rect->Right() && m.rect.Top() >= rect->Top() && m.rect.Bottom() <= rect->Bottom();
    });
}

[[nodiscard]] bool IndexSelectionMatchesOrdering(infra::RngState& rng) noexcept
{
    const MonitorList ordered = Ordered(RandomMonitors(rng));
    const std::uint32_t index = proptest::DrawBelow(rng, static_cast<std::uint32_t>(ordered.Size()) + 2);
    const auto selected = SelectSource(ordered, SourceSelection{ MonitorSelectionKind::Index, RequestedMonitorTag::Parse(index) });
    return index < ordered.Size() ? (selected.has_value() && selected->At(0) == ordered.At(index)) : !selected.has_value();
}

[[nodiscard]] bool GeometryTargetDefaultsToSource(infra::RngState& rng) noexcept
{
    const MonitorList monitors = RandomMonitors(rng);
    const Options options = DefaultOptions();
    const auto geometry = ResolveGeometry(monitors, options);
    return geometry.has_value() && geometry->sourceRect == geometry->targetRect && geometry->sourceExtent == geometry->targetExtent;
}

} // namespace

std::uint32_t MonitorsSuite(std::uint64_t seed) noexcept
{
    std::uint32_t failures = 0;
    failures += Failures(proptest::ForAll("Ordered puts the primary monitor first", seed, 500, OrderedPutsPrimaryFirst));
    failures += Failures(proptest::ForAll("Ordered is sorted by ComesBefore", seed, 500, OrderedIsSortedByRank));
    failures += Failures(proptest::ForAll("UnionRect contains every monitor", seed, 500, UnionContainsEveryMonitor));
    failures += Failures(proptest::ForAll("index selection matches ordering", seed, 500, IndexSelectionMatchesOrdering));
    failures += Failures(proptest::ForAll("target defaults to the source", seed, 300, GeometryTargetDefaultsToSource));
    return failures;
}

} // namespace tests
