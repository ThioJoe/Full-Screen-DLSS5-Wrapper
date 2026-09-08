#pragma once
#include "infrastructure/result.h"

#include <cstdint>

namespace infra {

enum class ArithmeticError : std::uint8_t { Overflow, DivisionByZero };

[[nodiscard]] Result<std::uint32_t, ArithmeticError> CheckedAdd(std::uint32_t a, std::uint32_t b) noexcept;
[[nodiscard]] Result<std::uint32_t, ArithmeticError> CheckedSub(std::uint32_t a, std::uint32_t b) noexcept;
[[nodiscard]] Result<std::uint32_t, ArithmeticError> CheckedMul(std::uint32_t a, std::uint32_t b) noexcept;
[[nodiscard]] Result<std::uint32_t, ArithmeticError> CheckedDiv(std::uint32_t a, std::uint32_t b) noexcept;
[[nodiscard]] Result<std::uint64_t, ArithmeticError> CheckedAdd(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] Result<std::uint64_t, ArithmeticError> CheckedSub(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] Result<std::uint64_t, ArithmeticError> CheckedMul(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] Result<std::int32_t, ArithmeticError> CheckedAdd(std::int32_t a, std::int32_t b) noexcept;
[[nodiscard]] Result<std::int32_t, ArithmeticError> CheckedSub(std::int32_t a, std::int32_t b) noexcept;
[[nodiscard]] Result<std::int32_t, ArithmeticError> CheckedMul(std::int32_t a, std::int32_t b) noexcept;

} // namespace infra
