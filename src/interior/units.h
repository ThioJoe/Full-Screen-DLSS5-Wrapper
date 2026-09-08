#pragma once
#include "infrastructure/bounded_string.h"
#include "infrastructure/result.h"
#include "infrastructure/strong.h"

#include <cstdint>

namespace interior {

using infra::Result;

enum class UnitError : std::uint8_t {
    PixelCountZero,
    PixelCountTooLarge,
    CoordinateTooLarge,
    RectangleEmpty,
    MonitorCountZero,
    MonitorCountTooLarge,
    MonitorIndexOutOfRange,
    HandleZero,
    NotFinite,
    FractionOutOfRange,
    StrengthOutOfRange,
    SkinStrengthOutOfRange,
    DepthOutOfRange,
    ScaleOutOfRange,
    LevelOutOfRange,
    LevelCountOutOfRange,
    PresetOutOfRange,
    SlotOutOfRange,
    BackBufferOutOfRange,
    GroupCountZero,
    AppIdZero,
    ProjectIdMalformed,
};

constexpr std::uint32_t kMaxPixelCount = 16384;
constexpr std::int32_t kMaxCoordinate = 65536;
constexpr std::uint32_t kMaxMonitors = 16;
constexpr std::uint32_t kMaxLevels = 8;
constexpr std::uint32_t kMaxNgxPreset = 3;
constexpr std::uint32_t kMaxSrPreset = 15;
constexpr std::uint32_t kDescriptorsPerFrame = 512;
constexpr std::uint32_t kBackBufferCount = 3;
constexpr std::uint32_t kFrameSlotCount = 2;
constexpr float kMaxScale = 64.0f;

struct PixelCountTag;
using PixelCount = infra::Strong<std::uint32_t, PixelCountTag>;
struct PixelCountTag
{
    [[nodiscard]] static constexpr Result<PixelCount, UnitError> Parse(std::uint32_t raw) noexcept;

private:
    [[nodiscard]] static constexpr Result<PixelCount, UnitError> ParseNonZero(std::uint32_t raw) noexcept;
};

struct CoordinateTag;
using Coordinate = infra::Strong<std::int32_t, CoordinateTag>;
struct CoordinateTag
{
    [[nodiscard]] static constexpr Result<Coordinate, UnitError> Parse(std::int32_t raw) noexcept;
};

struct Extent
{
    PixelCount width;
    PixelCount height;
    [[nodiscard]] friend constexpr bool operator==(const Extent&, const Extent&) noexcept = default;
};

struct ScreenRectTag;
class ScreenRect final
{
public:
    [[nodiscard]] constexpr Coordinate Left() const noexcept { return left_; }
    [[nodiscard]] constexpr Coordinate Top() const noexcept { return top_; }
    [[nodiscard]] constexpr Coordinate Right() const noexcept { return right_; }
    [[nodiscard]] constexpr Coordinate Bottom() const noexcept { return bottom_; }
    [[nodiscard]] friend constexpr bool operator==(const ScreenRect&, const ScreenRect&) noexcept = default;

private:
    friend ScreenRectTag;
    constexpr ScreenRect(Coordinate left, Coordinate top, Coordinate right, Coordinate bottom) noexcept : left_(left), top_(top), right_(right), bottom_(bottom) {}
    Coordinate left_;
    Coordinate top_;
    Coordinate right_;
    Coordinate bottom_;
};
struct ScreenRectTag
{
    [[nodiscard]] static constexpr Result<ScreenRect, UnitError> Parse(Coordinate left, Coordinate top, Coordinate right, Coordinate bottom) noexcept;
};

struct MonitorCountTag;
using MonitorCount = infra::Strong<std::uint32_t, MonitorCountTag>;
struct MonitorCountTag
{
    [[nodiscard]] static constexpr Result<MonitorCount, UnitError> Parse(std::uint32_t raw) noexcept;

private:
    [[nodiscard]] static constexpr Result<MonitorCount, UnitError> ParseNonZero(std::uint32_t raw) noexcept;
};

struct MonitorIndexTag;
using MonitorIndex = infra::Strong<std::uint32_t, MonitorIndexTag>;
struct MonitorIndexTag
{
    [[nodiscard]] static constexpr Result<MonitorIndex, UnitError> Parse(std::uint32_t raw, MonitorCount count) noexcept;
};

struct RequestedMonitorTag;
using RequestedMonitor = infra::Strong<std::uint32_t, RequestedMonitorTag>;
struct RequestedMonitorTag
{
    [[nodiscard]] static constexpr RequestedMonitor Parse(std::uint32_t raw) noexcept;
};

struct RequestedAdapterTag;
using RequestedAdapter = infra::Strong<std::uint32_t, RequestedAdapterTag>;
struct RequestedAdapterTag
{
    [[nodiscard]] static constexpr RequestedAdapter Parse(std::uint32_t raw) noexcept;
};

struct MonitorHandleTag;
using MonitorHandle = infra::Strong<std::uintptr_t, MonitorHandleTag>;
struct MonitorHandleTag
{
    [[nodiscard]] static constexpr Result<MonitorHandle, UnitError> Parse(std::uintptr_t raw) noexcept;
};

struct FractionTag;
using Fraction = infra::Strong<float, FractionTag>;
struct FractionTag
{
    [[nodiscard]] static constexpr Result<Fraction, UnitError> Parse(float raw) noexcept;
};

struct StrengthTag;
using Strength = infra::Strong<float, StrengthTag>;
struct StrengthTag
{
    [[nodiscard]] static constexpr Result<Strength, UnitError> Parse(float raw) noexcept;
};

struct SkinStrengthTag;
using SkinStrength = infra::Strong<float, SkinStrengthTag>;
struct SkinStrengthTag
{
    [[nodiscard]] static constexpr Result<SkinStrength, UnitError> Parse(float raw) noexcept;
};

struct DepthValueTag;
using DepthValue = infra::Strong<float, DepthValueTag>;
struct DepthValueTag
{
    [[nodiscard]] static constexpr Result<DepthValue, UnitError> Parse(float raw) noexcept;
};

struct ScaleTag;
using Scale = infra::Strong<float, ScaleTag>;
struct ScaleTag
{
    [[nodiscard]] static constexpr Result<Scale, UnitError> Parse(float raw) noexcept;
};

struct LevelIndexTag;
using LevelIndex = infra::Strong<std::uint32_t, LevelIndexTag>;
struct LevelIndexTag
{
    [[nodiscard]] static constexpr Result<LevelIndex, UnitError> Parse(std::uint32_t raw) noexcept;
};

struct LevelCountTag;
using LevelCount = infra::Strong<std::uint32_t, LevelCountTag>;
struct LevelCountTag
{
    [[nodiscard]] static constexpr Result<LevelCount, UnitError> Parse(std::uint32_t raw) noexcept;

private:
    [[nodiscard]] static constexpr Result<LevelCount, UnitError> ParseNonZero(std::uint32_t raw) noexcept;
};

struct NgxPresetTag;
using NgxPreset = infra::Strong<std::uint32_t, NgxPresetTag>;
struct NgxPresetTag
{
    [[nodiscard]] static constexpr Result<NgxPreset, UnitError> Parse(std::uint32_t raw) noexcept;
};

struct SrPresetTag;
using SrPreset = infra::Strong<std::uint32_t, SrPresetTag>;
struct SrPresetTag
{
    [[nodiscard]] static constexpr Result<SrPreset, UnitError> Parse(std::uint32_t raw) noexcept;
};

struct FrameNumberTag;
using FrameNumber = infra::Strong<std::uint64_t, FrameNumberTag>;
struct FrameNumberTag
{
    [[nodiscard]] static constexpr FrameNumber Parse(std::uint64_t raw) noexcept;
};

struct FenceValueTag;
using FenceValue = infra::Strong<std::uint64_t, FenceValueTag>;
struct FenceValueTag
{
    [[nodiscard]] static constexpr FenceValue Parse(std::uint64_t raw) noexcept;
};

struct FrameSlotTag;
using FrameSlot = infra::Strong<std::uint32_t, FrameSlotTag>;
struct FrameSlotTag
{
    [[nodiscard]] static constexpr Result<FrameSlot, UnitError> Parse(std::uint32_t raw) noexcept;
};

struct SetIndexTag;
using SetIndex = infra::Strong<std::uint32_t, SetIndexTag>;
struct SetIndexTag
{
    [[nodiscard]] static constexpr Result<SetIndex, UnitError> Parse(std::uint32_t raw) noexcept;
};

struct BackBufferIndexTag;
using BackBufferIndex = infra::Strong<std::uint32_t, BackBufferIndexTag>;
struct BackBufferIndexTag
{
    [[nodiscard]] static constexpr Result<BackBufferIndex, UnitError> Parse(std::uint32_t raw) noexcept;
};

struct DescriptorSlotTag;
using DescriptorSlot = infra::Strong<std::uint32_t, DescriptorSlotTag>;
struct DescriptorSlotTag
{
    [[nodiscard]] static constexpr Result<DescriptorSlot, UnitError> Parse(std::uint32_t raw) noexcept;
};

struct GroupCountTag;
using GroupCount = infra::Strong<std::uint32_t, GroupCountTag>;
struct GroupCountTag
{
    [[nodiscard]] static constexpr Result<GroupCount, UnitError> Parse(std::uint32_t raw) noexcept;
};

struct ThreadGroups
{
    GroupCount x;
    GroupCount y;
    [[nodiscard]] friend constexpr bool operator==(const ThreadGroups&, const ThreadGroups&) noexcept = default;
};

struct ByteCountTag;
using ByteCount = infra::Strong<std::uint32_t, ByteCountTag>;
struct ByteCountTag
{
    [[nodiscard]] static constexpr ByteCount Parse(std::uint32_t raw) noexcept;
};

struct MicrosecondsTag;
using Microseconds = infra::Strong<std::uint64_t, MicrosecondsTag>;
struct MicrosecondsTag
{
    [[nodiscard]] static constexpr Microseconds Parse(std::uint64_t raw) noexcept;
};

struct InstantTag;
using Instant = infra::Strong<std::uint64_t, InstantTag>;
struct InstantTag
{
    [[nodiscard]] static constexpr Instant Parse(std::uint64_t microseconds) noexcept;
};

struct NgxAppIdTag;
using NgxAppId = infra::Strong<std::uint64_t, NgxAppIdTag>;
struct NgxAppIdTag
{
    [[nodiscard]] static constexpr Result<NgxAppId, UnitError> Parse(std::uint64_t raw) noexcept;
};

using ProjectIdText = infra::BoundedString<char, 36>;
using DirectoryPath = infra::BoundedString<wchar_t, 260>;
using DeviceName = infra::BoundedString<wchar_t, 32>;
using AdapterName = infra::BoundedString<wchar_t, 128>;

[[nodiscard]] Result<ProjectIdText, UnitError> ParseProjectId(std::string_view raw) noexcept;

[[nodiscard]] constexpr bool IsZero(std::uint32_t value) noexcept
{
    return value == 0;
}
[[nodiscard]] constexpr bool IsAbove(float value, float limit) noexcept
{
    return value > limit;
}
[[nodiscard]] constexpr bool IsBelow(float value, float limit) noexcept
{
    return value < limit;
}
[[nodiscard]] constexpr bool IsNaN(float value) noexcept
{
    return value != value;
}
[[nodiscard]] constexpr bool IsOutside(float value, float low, float high) noexcept
{
    return IsBelow(value, low) || IsAbove(value, high);
}
[[nodiscard]] constexpr bool IsOutsideOrNaN(float value, float low, float high) noexcept
{
    return IsNaN(value) || IsOutside(value, low, high);
}

// --- parser definitions -------------------------------------------------------

constexpr Result<PixelCount, UnitError> PixelCountTag::ParseNonZero(std::uint32_t raw) noexcept
{
    if (raw > kMaxPixelCount)
        return infra::Fail(UnitError::PixelCountTooLarge);
    return PixelCount(raw);
}

constexpr Result<PixelCount, UnitError> PixelCountTag::Parse(std::uint32_t raw) noexcept
{
    if (IsZero(raw))
        return infra::Fail(UnitError::PixelCountZero);
    return ParseNonZero(raw);
}

[[nodiscard]] constexpr bool IsBeyond(std::int32_t value, std::int32_t limit) noexcept
{
    return value > limit || value < -limit;
}

constexpr Result<Coordinate, UnitError> CoordinateTag::Parse(std::int32_t raw) noexcept
{
    if (IsBeyond(raw, kMaxCoordinate))
        return infra::Fail(UnitError::CoordinateTooLarge);
    return Coordinate(raw);
}

[[nodiscard]] constexpr bool IsEmptyArea(Coordinate left, Coordinate top, Coordinate right, Coordinate bottom) noexcept
{
    return right <= left || bottom <= top;
}

constexpr Result<ScreenRect, UnitError> ScreenRectTag::Parse(Coordinate left, Coordinate top, Coordinate right, Coordinate bottom) noexcept
{
    if (IsEmptyArea(left, top, right, bottom))
        return infra::Fail(UnitError::RectangleEmpty);
    return ScreenRect(left, top, right, bottom);
}

constexpr Result<MonitorCount, UnitError> MonitorCountTag::ParseNonZero(std::uint32_t raw) noexcept
{
    if (raw > kMaxMonitors)
        return infra::Fail(UnitError::MonitorCountTooLarge);
    return MonitorCount(raw);
}

constexpr Result<MonitorCount, UnitError> MonitorCountTag::Parse(std::uint32_t raw) noexcept
{
    if (IsZero(raw))
        return infra::Fail(UnitError::MonitorCountZero);
    return ParseNonZero(raw);
}

constexpr Result<MonitorIndex, UnitError> MonitorIndexTag::Parse(std::uint32_t raw, MonitorCount count) noexcept
{
    if (raw >= count.Get())
        return infra::Fail(UnitError::MonitorIndexOutOfRange);
    return MonitorIndex(raw);
}

constexpr RequestedMonitor RequestedMonitorTag::Parse(std::uint32_t raw) noexcept
{
    return RequestedMonitor(raw);
}

constexpr RequestedAdapter RequestedAdapterTag::Parse(std::uint32_t raw) noexcept
{
    return RequestedAdapter(raw);
}

constexpr Result<MonitorHandle, UnitError> MonitorHandleTag::Parse(std::uintptr_t raw) noexcept
{
    if (raw == 0)
        return infra::Fail(UnitError::HandleZero);
    return MonitorHandle(raw);
}

constexpr Result<Fraction, UnitError> FractionTag::Parse(float raw) noexcept
{
    if (IsOutsideOrNaN(raw, 0.0f, 1.0f))
        return infra::Fail(UnitError::FractionOutOfRange);
    return Fraction(raw);
}

constexpr Result<Strength, UnitError> StrengthTag::Parse(float raw) noexcept
{
    if (IsOutsideOrNaN(raw, 0.0f, 2.0f))
        return infra::Fail(UnitError::StrengthOutOfRange);
    return Strength(raw);
}

constexpr Result<SkinStrength, UnitError> SkinStrengthTag::Parse(float raw) noexcept
{
    if (IsOutsideOrNaN(raw, -1.0f, 2.0f))
        return infra::Fail(UnitError::SkinStrengthOutOfRange);
    return SkinStrength(raw);
}

constexpr Result<DepthValue, UnitError> DepthValueTag::Parse(float raw) noexcept
{
    if (IsOutsideOrNaN(raw, 0.0f, 1.0f))
        return infra::Fail(UnitError::DepthOutOfRange);
    return DepthValue(raw);
}

constexpr Result<Scale, UnitError> ScaleTag::Parse(float raw) noexcept
{
    if (IsOutsideOrNaN(raw, 0.0f, kMaxScale))
        return infra::Fail(UnitError::ScaleOutOfRange);
    return Scale(raw);
}

constexpr Result<LevelIndex, UnitError> LevelIndexTag::Parse(std::uint32_t raw) noexcept
{
    if (raw >= kMaxLevels)
        return infra::Fail(UnitError::LevelOutOfRange);
    return LevelIndex(raw);
}

constexpr Result<LevelCount, UnitError> LevelCountTag::ParseNonZero(std::uint32_t raw) noexcept
{
    if (raw > kMaxLevels)
        return infra::Fail(UnitError::LevelCountOutOfRange);
    return LevelCount(raw);
}

constexpr Result<LevelCount, UnitError> LevelCountTag::Parse(std::uint32_t raw) noexcept
{
    if (IsZero(raw))
        return infra::Fail(UnitError::LevelCountOutOfRange);
    return ParseNonZero(raw);
}

constexpr Result<NgxPreset, UnitError> NgxPresetTag::Parse(std::uint32_t raw) noexcept
{
    if (raw > kMaxNgxPreset)
        return infra::Fail(UnitError::PresetOutOfRange);
    return NgxPreset(raw);
}

constexpr Result<SrPreset, UnitError> SrPresetTag::Parse(std::uint32_t raw) noexcept
{
    if (raw > kMaxSrPreset)
        return infra::Fail(UnitError::PresetOutOfRange);
    return SrPreset(raw);
}

constexpr FrameNumber FrameNumberTag::Parse(std::uint64_t raw) noexcept
{
    return FrameNumber(raw);
}

constexpr FenceValue FenceValueTag::Parse(std::uint64_t raw) noexcept
{
    return FenceValue(raw);
}

constexpr Result<FrameSlot, UnitError> FrameSlotTag::Parse(std::uint32_t raw) noexcept
{
    if (raw >= kFrameSlotCount)
        return infra::Fail(UnitError::SlotOutOfRange);
    return FrameSlot(raw);
}

constexpr Result<SetIndex, UnitError> SetIndexTag::Parse(std::uint32_t raw) noexcept
{
    if (raw >= 2)
        return infra::Fail(UnitError::SlotOutOfRange);
    return SetIndex(raw);
}

constexpr Result<BackBufferIndex, UnitError> BackBufferIndexTag::Parse(std::uint32_t raw) noexcept
{
    if (raw >= kBackBufferCount)
        return infra::Fail(UnitError::BackBufferOutOfRange);
    return BackBufferIndex(raw);
}

constexpr Result<DescriptorSlot, UnitError> DescriptorSlotTag::Parse(std::uint32_t raw) noexcept
{
    if (raw >= kDescriptorsPerFrame)
        return infra::Fail(UnitError::SlotOutOfRange);
    return DescriptorSlot(raw);
}

constexpr Result<GroupCount, UnitError> GroupCountTag::Parse(std::uint32_t raw) noexcept
{
    if (IsZero(raw))
        return infra::Fail(UnitError::GroupCountZero);
    return GroupCount(raw);
}

constexpr ByteCount ByteCountTag::Parse(std::uint32_t raw) noexcept
{
    return ByteCount(raw);
}

constexpr Microseconds MicrosecondsTag::Parse(std::uint64_t raw) noexcept
{
    return Microseconds(raw);
}

constexpr Instant InstantTag::Parse(std::uint64_t microseconds) noexcept
{
    return Instant(microseconds);
}

constexpr Result<NgxAppId, UnitError> NgxAppIdTag::Parse(std::uint64_t raw) noexcept
{
    if (raw == 0)
        return infra::Fail(UnitError::AppIdZero);
    return NgxAppId(raw);
}

} // namespace interior
