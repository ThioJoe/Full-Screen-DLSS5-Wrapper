#include "tests/test_registry.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {

[[nodiscard]] std::uint64_t SeedFromArgs(int argc, char** argv) noexcept
{
    return argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000u;
}

} // namespace

int main(int argc, char** argv)
{
    const std::uint64_t seed = SeedFromArgs(argc, argv);
    const std::array<tests::Suite, 6> suites{ tests::UnitsSuite, tests::OptionsSuite, tests::MonitorsSuite, tests::PlanSuite, tests::FrameSuite, tests::SimulationSuite };
    std::uint32_t failures = 0;
    for (const tests::Suite suite : suites) // WAIVER(R2): test driver loop.
        failures += suite(seed);
    std::printf("%s: %u failing propert%s (base seed %llu)\n", failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "y" : "ies", static_cast<unsigned long long>(seed));
    return failures == 0 ? 0 : 1;
}
