#pragma once
#include "effects/real/com.h"
#include "interior/units.h"

namespace real {

[[nodiscard]] infra::Result<interior::Instant, Error> Now() noexcept;

} // namespace real
