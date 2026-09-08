#pragma once
#include "tests/proptest.h"

#include <cstdint>

namespace tests {

using Suite = std::uint32_t (*)(std::uint64_t baseSeed);

[[nodiscard]] std::uint32_t UnitsSuite(std::uint64_t baseSeed) noexcept;
[[nodiscard]] std::uint32_t OptionsSuite(std::uint64_t baseSeed) noexcept;
[[nodiscard]] std::uint32_t MonitorsSuite(std::uint64_t baseSeed) noexcept;
[[nodiscard]] std::uint32_t PlanSuite(std::uint64_t baseSeed) noexcept;
[[nodiscard]] std::uint32_t FrameSuite(std::uint64_t baseSeed) noexcept;
[[nodiscard]] std::uint32_t SimulationSuite(std::uint64_t baseSeed) noexcept;

[[nodiscard]] inline std::uint32_t Failures(const proptest::Outcome& outcome) noexcept
{
    return outcome.failingSeed.has_value() ? 1u : 0u;
}

} // namespace tests
