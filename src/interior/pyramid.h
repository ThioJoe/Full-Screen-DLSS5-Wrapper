#pragma once
#include "infrastructure/bounded_vector.h"
#include "interior/enums.h"
#include "interior/units.h"

namespace interior {

using LevelExtents = infra::BoundedVector<Extent, kMaxLevels>;

enum class PyramidError : std::uint8_t { Arithmetic, Capacity, Unit };

constexpr std::uint32_t kMinLevelSize = 16;
constexpr std::uint32_t kCoarsestLevel = 5;
constexpr std::uint32_t kGroupSize = 8;

[[nodiscard]] Extent Halved(const Extent& extent) noexcept;
[[nodiscard]] LevelCount LevelCountFor(const Extent& source) noexcept;
[[nodiscard]] Result<LevelExtents, PyramidError> LevelExtentsOf(const Extent& source, LevelCount levels) noexcept;
[[nodiscard]] Result<ThreadGroups, PyramidError> GroupsFor(const Extent& extent) noexcept;
[[nodiscard]] Result<Extent, PyramidError> GridExtent(const Extent& source, std::uint32_t grid) noexcept;
[[nodiscard]] std::uint32_t GridCells(GridSize grid) noexcept;
[[nodiscard]] bool IsLevelUsable(const Extent& source, std::uint32_t level) noexcept;

} // namespace interior
