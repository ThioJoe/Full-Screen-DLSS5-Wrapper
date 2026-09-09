#pragma once
#include "infrastructure/bounded_vector.h"
#include "interior/options.h"
#include "interior/units.h"

namespace interior {

// What a source is: a monitor Windows enumerated, or one window asked for by name. The capture layer opens
// a different kind of item for each; everything above this point treats them alike.
enum class SourceKind : std::uint8_t { Monitor, Window };

struct MonitorInfo
{
    MonitorHandle handle;
    ScreenRect rect;
    bool primary;
    DeviceName name;
    SourceKind kind;
    [[nodiscard]] friend constexpr bool operator==(const MonitorInfo&, const MonitorInfo&) noexcept = default;
};

using MonitorList = infra::BoundedVector<MonitorInfo, kMaxMonitors>;

enum class MonitorError : std::uint8_t { NoMonitors, IndexOutOfRange, EmptyArea, TooManyMonitors };

struct Geometry
{
    MonitorList source;
    ScreenRect sourceRect;
    ScreenRect targetRect;
    Extent sourceExtent;
    Extent targetExtent;
    [[nodiscard]] friend constexpr bool operator==(const Geometry&, const Geometry&) noexcept = default;
};

[[nodiscard]] bool ComesBefore(const MonitorInfo& a, const MonitorInfo& b) noexcept;
[[nodiscard]] MonitorList Ordered(const MonitorList& monitors) noexcept;
[[nodiscard]] Result<ScreenRect, MonitorError> UnionRect(const MonitorList& monitors) noexcept;
[[nodiscard]] Result<Extent, MonitorError> ExtentOf(const ScreenRect& rect) noexcept;
[[nodiscard]] Result<MonitorList, MonitorError> SelectSource(const MonitorList& ordered, const SourceSelection& selection) noexcept;
[[nodiscard]] Result<Geometry, MonitorError> ResolveGeometry(const MonitorList& monitors, const Options& options) noexcept;
[[nodiscard]] bool IsSameRect(const ScreenRect& a, const ScreenRect& b) noexcept;
[[nodiscard]] std::string_view Describe(MonitorError error) noexcept;

} // namespace interior
