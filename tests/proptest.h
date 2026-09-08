#pragma once
// Property-test runner: generated inputs from a seeded pure RNG, every failure
// reported with the seed that reproduces it.
#include "infrastructure/rng.h"

#include <cstdint>
#include <cstdio>
#include <optional>

namespace proptest {

struct Case
{
    std::uint64_t seed;
    infra::RngState rng;
};

struct Outcome
{
    std::uint32_t passed;
    std::optional<std::uint64_t> failingSeed;
};

[[nodiscard]] inline std::uint64_t Draw(infra::RngState& rng) noexcept // WAIVER(R2): test generator cursor.
{
    const infra::RngDraw d = infra::NextRandom(rng);
    rng = d.next;
    return d.value;
}

[[nodiscard]] inline std::uint32_t DrawBelow(infra::RngState& rng, std::uint32_t bound) noexcept
{
    return static_cast<std::uint32_t>(Draw(rng) % bound);
}

[[nodiscard]] inline std::uint32_t DrawBetween(infra::RngState& rng, std::uint32_t low, std::uint32_t high) noexcept
{
    return low + DrawBelow(rng, high - low + 1);
}

[[nodiscard]] inline bool DrawBool(infra::RngState& rng) noexcept
{
    return (Draw(rng) & 1u) != 0u;
}

[[nodiscard]] inline float DrawUnit(infra::RngState& rng) noexcept
{
    return static_cast<float>(Draw(rng) % 100001u) / 100000.0f;
}

template <class Property>
[[nodiscard]] Outcome ForAll(const char* name, std::uint64_t baseSeed, std::uint32_t cases, Property property) noexcept
{
    for (std::uint32_t i = 0; i < cases; ++i) // WAIVER(R2): test driver loop.
    {
        const std::uint64_t seed = baseSeed + i;
        infra::RngState rng = infra::SeedRng(seed);
        if (!property(rng))
        {
            std::printf("FAIL %s seed=%llu\n", name, static_cast<unsigned long long>(seed));
            return Outcome{ i, seed };
        }
    }
    std::printf("ok   %s (%u cases)\n", name, cases);
    return Outcome{ cases, std::nullopt };
}

} // namespace proptest
