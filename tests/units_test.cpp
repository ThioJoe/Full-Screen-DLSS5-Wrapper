// WAIVER(R2): test suites accumulate failure counts and drive generated sequences with loops.
#include "infrastructure/bounded_vector.h"
#include "infrastructure/checked.h"
#include "interior/units.h"
#include "tests/test_registry.h"

#include <cstring>

namespace tests {
namespace {

using namespace interior;

[[nodiscard]] bool PixelCountAcceptsExactlyRange(infra::RngState& rng) noexcept
{
    const std::uint32_t raw = proptest::DrawBelow(rng, 40000);
    const bool inRange = raw >= 1 && raw <= kMaxPixelCount;
    return PixelCountTag::Parse(raw).has_value() == inRange;
}

[[nodiscard]] bool FractionRejectsOutside(infra::RngState& rng) noexcept
{
    const float raw = proptest::DrawUnit(rng) * 3.0f - 1.0f;
    const bool inRange = raw >= 0.0f && raw <= 1.0f;
    return FractionTag::Parse(raw).has_value() == inRange;
}

[[nodiscard]] bool SkinStrengthAcceptsMinusOne(infra::RngState&) noexcept
{
    return SkinStrengthTag::Parse(-1.0f).has_value() && !SkinStrengthTag::Parse(-1.5f).has_value();
}

[[nodiscard]] bool NanIsRejectedEverywhere(infra::RngState&) noexcept
{
    const float nan = std::bit_cast<float>(0x7FC00000u);
    return !FractionTag::Parse(nan).has_value() && !ScaleTag::Parse(nan).has_value();
}

[[nodiscard]] bool RectRequiresPositiveArea(infra::RngState& rng) noexcept
{
    const std::int32_t left = static_cast<std::int32_t>(proptest::DrawBelow(rng, 200)) - 100;
    const std::int32_t width = static_cast<std::int32_t>(proptest::DrawBelow(rng, 20)) - 5;
    const auto rect = CoordinateTag::Parse(left).and_then([&](Coordinate l)
    {
        return CoordinateTag::Parse(left + width).and_then([&](Coordinate r)
        {
            return CoordinateTag::Parse(0).and_then([&](Coordinate t) { return CoordinateTag::Parse(10).and_then([&](Coordinate b) { return ScreenRectTag::Parse(l, t, r, b); }); });
        });
    });
    return rect.has_value() == (width > 0);
}

[[nodiscard]] bool CheckedAddMatchesWideArithmetic(infra::RngState& rng) noexcept
{
    const std::uint32_t a = static_cast<std::uint32_t>(proptest::Draw(rng));
    const std::uint32_t b = static_cast<std::uint32_t>(proptest::Draw(rng));
    const std::uint64_t wide = static_cast<std::uint64_t>(a) + b;
    const auto result = infra::CheckedAdd(a, b);
    return result.has_value() ? (*result == wide) : (wide > 0xFFFFFFFFull);
}

[[nodiscard]] bool CheckedMulMatchesWideArithmetic(infra::RngState& rng) noexcept
{
    const std::uint32_t a = static_cast<std::uint32_t>(proptest::Draw(rng) % 100000u);
    const std::uint32_t b = static_cast<std::uint32_t>(proptest::Draw(rng) % 100000u);
    const std::uint64_t wide = static_cast<std::uint64_t>(a) * b;
    const auto result = infra::CheckedMul(a, b);
    return result.has_value() ? (*result == wide) : (wide > 0xFFFFFFFFull);
}

[[nodiscard]] bool CheckedDivRejectsZero(infra::RngState& rng) noexcept
{
    const std::uint32_t a = static_cast<std::uint32_t>(proptest::Draw(rng));
    const std::uint32_t b = proptest::DrawBelow(rng, 3);
    const auto result = infra::CheckedDiv(a, b);
    return b == 0 ? !result.has_value() : (result.has_value() && *result == a / b);
}

[[nodiscard]] bool BoundedVectorStopsAtCapacity(infra::RngState& rng) noexcept
{
    using V = infra::BoundedVector<std::uint32_t, 4>;
    const std::uint32_t count = proptest::DrawBelow(rng, 7);
    V v;
    bool ok = true;
    for (std::uint32_t i = 0; i < count; ++i) // WAIVER(R2): test enumerates pushes.
    {
        const auto pushed = v.Push(i);
        ok = ok && (pushed.has_value() == (i < 4));
        if (pushed.has_value())
            v = *pushed;
    }
    return ok && v.Size() == (count < 4 ? count : 4);
}

[[nodiscard]] bool ProjectIdRequiresGuidShape(infra::RngState& rng) noexcept
{
    const char* good = "5e9b2a44-7c31-4d0e-9f2b-8d3c1a6e7f10";
    const bool flip = proptest::DrawBool(rng);
    const char* text = flip ? "5e9b2a44-7c31-4d0e-9f2b-8d3c1a6e7f1g" : good;
    return ParseProjectId(text).has_value() == !flip;
}

} // namespace

std::uint32_t UnitsSuite(std::uint64_t seed) noexcept
{
    std::uint32_t failures = 0;
    failures += Failures(proptest::ForAll("PixelCount accepts exactly 1..16384", seed, 500, PixelCountAcceptsExactlyRange));
    failures += Failures(proptest::ForAll("Fraction rejects values outside 0..1", seed, 500, FractionRejectsOutside));
    failures += Failures(proptest::ForAll("SkinStrength accepts -1 and rejects below", seed, 1, SkinStrengthAcceptsMinusOne));
    failures += Failures(proptest::ForAll("NaN is rejected", seed, 1, NanIsRejectedEverywhere));
    failures += Failures(proptest::ForAll("ScreenRect requires positive area", seed, 500, RectRequiresPositiveArea));
    failures += Failures(proptest::ForAll("CheckedAdd matches 64-bit arithmetic", seed, 2000, CheckedAddMatchesWideArithmetic));
    failures += Failures(proptest::ForAll("CheckedMul matches 64-bit arithmetic", seed, 2000, CheckedMulMatchesWideArithmetic));
    failures += Failures(proptest::ForAll("CheckedDiv rejects zero", seed, 500, CheckedDivRejectsZero));
    failures += Failures(proptest::ForAll("BoundedVector stops at capacity", seed, 200, BoundedVectorStopsAtCapacity));
    failures += Failures(proptest::ForAll("ProjectId requires GUID shape", seed, 20, ProjectIdRequiresGuidShape));
    return failures;
}

} // namespace tests
