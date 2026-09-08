#include "effects/real/clock.h"

namespace real {
namespace {

[[nodiscard]] std::uint64_t Microseconds(std::uint64_t counter, std::uint64_t frequency) noexcept
{
    return (counter / frequency) * 1000000u + ((counter % frequency) * 1000000u) / frequency;
}

} // namespace

infra::Result<interior::Instant, Error> Now() noexcept
{
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    return CheckBool(::QueryPerformanceCounter(&counter), ApiCall::QueryPerformanceCounter)
        .and_then([&] { return CheckBool(::QueryPerformanceFrequency(&frequency), ApiCall::QueryPerformanceFrequency); })
        .transform([&] { return interior::InstantTag::Parse(Microseconds(static_cast<std::uint64_t>(counter.QuadPart), static_cast<std::uint64_t>(frequency.QuadPart))); });
}

} // namespace real
