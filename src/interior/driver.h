#pragma once
#include <cstdint>

namespace interior {

// The user-mode driver version DXGI reports for an adapter, four 16-bit fields such as 32.0.16.1656.
struct DriverVersion
{
    std::uint16_t product;
    std::uint16_t version;
    std::uint16_t subVersion;
    std::uint16_t build;
    [[nodiscard]] friend constexpr bool operator==(const DriverVersion&, const DriverVersion&) noexcept = default;
};

constexpr std::uint32_t kDriverFieldBits = 16;
constexpr std::uint64_t kDriverFieldMask = 0xFFFF;
constexpr std::uint32_t kNvidiaBuildBase = 10000;
constexpr std::uint32_t kNvidiaMinorBase = 100;

// The first NVIDIA driver whose NGX loader offers DLSS 5 Neural Rendering (feature 18) itself:
// 616.64. Earlier loaders answer 0xBAD00012 to the requirements query and cannot build the feature.
constexpr std::uint32_t kFirstNeuralRenderingDriver = 61664;

[[nodiscard]] constexpr std::uint16_t DriverField(std::uint64_t packed, std::uint32_t index) noexcept
{
    return static_cast<std::uint16_t>((packed >> (kDriverFieldBits * (3 - index))) & kDriverFieldMask);
}

// Unpacks the 64-bit value of IDXGIAdapter::CheckInterfaceSupport: the fields sit high to low.
[[nodiscard]] constexpr DriverVersion DriverVersionOf(std::uint64_t packed) noexcept
{
    return DriverVersion{ DriverField(packed, 0), DriverField(packed, 1), DriverField(packed, 2), DriverField(packed, 3) };
}

// NVIDIA's public number: the last digit of the third field followed by the fourth, as one integer.
// 32.0.16.1656 is 616.56 (61656) and 32.0.15.6094 is 560.94 (56094).
[[nodiscard]] constexpr std::uint32_t NvidiaDriverNumber(const DriverVersion& v) noexcept
{
    return (v.subVersion % 10u) * kNvidiaBuildBase + v.build;
}

[[nodiscard]] constexpr std::uint32_t NvidiaDriverMajor(std::uint32_t number) noexcept
{
    return number / kNvidiaMinorBase;
}

[[nodiscard]] constexpr std::uint32_t NvidiaDriverMinor(std::uint32_t number) noexcept
{
    return number % kNvidiaMinorBase;
}

[[nodiscard]] constexpr bool OffersNeuralRendering(const DriverVersion& v) noexcept
{
    return NvidiaDriverNumber(v) >= kFirstNeuralRenderingDriver;
}

} // namespace interior
