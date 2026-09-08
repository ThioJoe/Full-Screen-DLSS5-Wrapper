#include "interior/monitors.h"

#include "infrastructure/checked.h"
#include "infrastructure/fold.h"

#include <algorithm>
#include <ranges>

namespace interior {
namespace {

using infra::Fail;

[[nodiscard]] bool IsPrimaryFirst(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return a.primary && !b.primary;
}

[[nodiscard]] bool IsPrimaryEqual(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return a.primary == b.primary;
}

[[nodiscard]] bool IsLeftBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return a.rect.Left() < b.rect.Left();
}

[[nodiscard]] bool IsLeftEqual(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return a.rect.Left() == b.rect.Left();
}

[[nodiscard]] bool IsTopBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return a.rect.Top() < b.rect.Top();
}

[[nodiscard]] bool IsTopEqual(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return a.rect.Top() == b.rect.Top();
}

[[nodiscard]] bool IsHandleBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return a.handle < b.handle;
}

[[nodiscard]] bool IsTopTiedAndHandleBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return IsTopEqual(a, b) && IsHandleBefore(a, b);
}

[[nodiscard]] bool IsTopOrHandleBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return IsTopBefore(a, b) || IsTopTiedAndHandleBefore(a, b);
}

[[nodiscard]] bool IsLeftTiedAndLaterBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return IsLeftEqual(a, b) && IsTopOrHandleBefore(a, b);
}

[[nodiscard]] bool IsLeftOrLaterBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return IsLeftBefore(a, b) || IsLeftTiedAndLaterBefore(a, b);
}

[[nodiscard]] bool IsPrimaryTiedAndBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return IsPrimaryEqual(a, b) && IsLeftOrLaterBefore(a, b);
}

[[nodiscard]] std::size_t RankOf(const MonitorList& monitors, const MonitorInfo& monitor) noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(monitors.Items(), [&monitor](const MonitorInfo& other) { return ComesBefore(other, monitor); }));
}

[[nodiscard]] bool HasRank(const MonitorList& monitors, const MonitorInfo& monitor, std::size_t rank) noexcept
{
    return RankOf(monitors, monitor) == rank;
}

[[nodiscard]] Result<MonitorList, infra::CapacityExceeded> AppendRanked(const MonitorList& monitors, const MonitorList& acc, std::size_t rank) noexcept
{
    const auto items = monitors.Items();
    const auto found = std::ranges::find_if(items, [&](const MonitorInfo& m) { return HasRank(monitors, m, rank); });
    if (found == items.end())
        return acc;
    return acc.Push(*found);
}

[[nodiscard]] Coordinate Min(Coordinate a, Coordinate b) noexcept
{
    return a < b ? a : b;
}

[[nodiscard]] Coordinate Max(Coordinate a, Coordinate b) noexcept
{
    return a > b ? a : b;
}

[[nodiscard]] Result<ScreenRect, UnitError> Union(const ScreenRect& a, const ScreenRect& b) noexcept
{
    return ScreenRectTag::Parse(Min(a.Left(), b.Left()), Min(a.Top(), b.Top()), Max(a.Right(), b.Right()), Max(a.Bottom(), b.Bottom()));
}

[[nodiscard]] Result<ScreenRect, UnitError> UnionWith(const ScreenRect& acc, const MonitorInfo& monitor) noexcept
{
    return Union(acc, monitor.rect);
}

[[nodiscard]] MonitorError AsMonitorError(UnitError) noexcept
{
    return MonitorError::EmptyArea;
}

[[nodiscard]] MonitorError AsMonitorError(infra::CapacityExceeded) noexcept
{
    return MonitorError::TooManyMonitors;
}

[[nodiscard]] MonitorError AsMonitorError(infra::ArithmeticError) noexcept
{
    return MonitorError::EmptyArea;
}

[[nodiscard]] Result<PixelCount, MonitorError> Span(Coordinate low, Coordinate high) noexcept
{
    return infra::CheckedSub(high.Get(), low.Get()).transform_error([](infra::ArithmeticError e) { return AsMonitorError(e); }).and_then([](std::int32_t length) {
        return PixelCountTag::Parse(static_cast<std::uint32_t>(length)).transform_error([](UnitError e) { return AsMonitorError(e); });
    });
}

[[nodiscard]] Result<MonitorList, MonitorError> PrimaryOf(const MonitorList& ordered) noexcept
{
    return MonitorList{}.Push(ordered.At(0)).transform_error([](infra::CapacityExceeded e) { return AsMonitorError(e); });
}

[[nodiscard]] Result<MonitorList, MonitorError> IndexedOf(const MonitorList& ordered, RequestedMonitor requested) noexcept
{
    if (!ordered.HasIndex(requested.Get()))
        return Fail(MonitorError::IndexOutOfRange);
    return MonitorList{}.Push(ordered.At(requested.Get())).transform_error([](infra::CapacityExceeded e) { return AsMonitorError(e); });
}

[[nodiscard]] Result<ScreenRect, MonitorError> TargetRectOf(const MonitorList& ordered, const ScreenRect& sourceRect, std::optional<RequestedMonitor> target) noexcept
{
    if (!target.has_value())
        return sourceRect;
    return IndexedOf(ordered, *target).transform([](const MonitorList& list) { return list.At(0).rect; });
}

[[nodiscard]] Result<Geometry, MonitorError> GeometryFrom(const MonitorList& source, const ScreenRect& sourceRect, const ScreenRect& targetRect) noexcept
{
    return ExtentOf(sourceRect).and_then([&](Extent sourceExtent) {
        return ExtentOf(targetRect).transform([&](Extent targetExtent) { return Geometry{ source, sourceRect, targetRect, sourceExtent, targetExtent }; });
    });
}

[[nodiscard]] Result<Geometry, MonitorError> GeometryOf(const MonitorList& ordered, const MonitorList& source, std::optional<RequestedMonitor> target) noexcept
{
    return UnionRect(source).and_then(
        [&](const ScreenRect& sourceRect) { return TargetRectOf(ordered, sourceRect, target).and_then([&](const ScreenRect& targetRect) { return GeometryFrom(source, sourceRect, targetRect); }); });
}

} // namespace

bool ComesBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept
{
    return IsPrimaryFirst(a, b) || IsPrimaryTiedAndBefore(a, b);
}

MonitorList Ordered(const MonitorList& monitors) noexcept
{
    const Result<MonitorList, infra::CapacityExceeded> ordered = infra::FoldResult(std::views::iota(std::size_t{ 0 }, monitors.Size()), Result<MonitorList, infra::CapacityExceeded>(MonitorList{}),
                                                                                   [&monitors](const MonitorList& acc, std::size_t rank) { return AppendRanked(monitors, acc, rank); });
    ENSURE(ordered.has_value());
    return *ordered;
}

Result<ScreenRect, MonitorError> UnionRect(const MonitorList& monitors) noexcept
{
    if (monitors.IsEmpty())
        return Fail(MonitorError::NoMonitors);
    return infra::FoldResult(monitors.Items(), Result<ScreenRect, UnitError>(monitors.At(0).rect), UnionWith).transform_error([](UnitError e) { return AsMonitorError(e); });
}

Result<Extent, MonitorError> ExtentOf(const ScreenRect& rect) noexcept
{
    return Span(rect.Left(), rect.Right()).and_then([&rect](PixelCount width) { return Span(rect.Top(), rect.Bottom()).transform([width](PixelCount height) { return Extent{ width, height }; }); });
}

[[nodiscard]] Result<MonitorList, MonitorError> SelectFrom(const MonitorList& ordered, const SourceSelection& selection) noexcept
{
    switch (selection.kind)
    {
    case MonitorSelectionKind::Primary: return PrimaryOf(ordered);
    case MonitorSelectionKind::All: return ordered;
    case MonitorSelectionKind::Index: return IndexedOf(ordered, selection.index);
    }
    return Fail(MonitorError::NoMonitors);
}

Result<MonitorList, MonitorError> SelectSource(const MonitorList& ordered, const SourceSelection& selection) noexcept
{
    if (ordered.IsEmpty())
        return Fail(MonitorError::NoMonitors);
    return SelectFrom(ordered, selection);
}

Result<Geometry, MonitorError> ResolveGeometry(const MonitorList& monitors, const Options& options) noexcept
{
    const MonitorList ordered = Ordered(monitors);
    return SelectSource(ordered, options.source).and_then([&](const MonitorList& source) { return GeometryOf(ordered, source, options.target); });
}

bool IsSameRect(const ScreenRect& a, const ScreenRect& b) noexcept
{
    return a == b;
}

std::string_view Describe(MonitorError error) noexcept
{
    switch (error)
    {
    case MonitorError::NoMonitors: return "no monitors found";
    case MonitorError::IndexOutOfRange: return "no monitor has that index (use --list-monitors)";
    case MonitorError::EmptyArea: return "the selected monitors have an empty or oversized area";
    case MonitorError::TooManyMonitors: return "more monitors than supported";
    }
    return "monitor error";
}

} // namespace interior
