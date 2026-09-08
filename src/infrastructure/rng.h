#pragma once
#include <cstdint>
#include <utility>

namespace infra {

// SplitMix64, Steele/Lea/Flood 2014; a pure step so the state is a value.
struct RngState
{
    std::uint64_t value = 0;
    [[nodiscard]] friend constexpr bool operator==(const RngState&, const RngState&) noexcept = default;
};

struct RngDraw
{
    std::uint64_t value = 0;
    RngState next{};
};

[[nodiscard]] constexpr RngState SeedRng(std::uint64_t seed) noexcept
{
    return RngState{ seed };
}

[[nodiscard]] constexpr RngDraw NextRandom(RngState state) noexcept
{
    const std::uint64_t z0 = state.value + 0x9E3779B97F4A7C15ull;
    const std::uint64_t z1 = (z0 ^ (z0 >> 30)) * 0xBF58476D1CE4E5B9ull;
    const std::uint64_t z2 = (z1 ^ (z1 >> 27)) * 0x94D049BB133111EBull;
    return RngDraw{ z2 ^ (z2 >> 31), RngState{ z0 } };
}

} // namespace infra
