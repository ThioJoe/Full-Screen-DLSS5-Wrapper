#pragma once

namespace infra {

[[noreturn]] void ContractViolation(const char* predicate, const char* file, int line) noexcept;

inline void Contract(bool holds, const char* predicate, const char* file, int line) noexcept
{
    if (!holds)
        ContractViolation(predicate, file, line);
}

} // namespace infra

#define REQUIRE(predicate) ::infra::Contract((predicate), #predicate, __FILE__, __LINE__)
#define ENSURE(predicate) ::infra::Contract((predicate), #predicate, __FILE__, __LINE__)
