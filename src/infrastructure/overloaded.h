#pragma once

namespace infra {

template <class... Ts>
struct Overloaded : Ts...
{
    using Ts::operator()...;
};

} // namespace infra
