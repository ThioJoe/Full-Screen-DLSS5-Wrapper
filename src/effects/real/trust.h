#pragma once
#include "effects/real/com.h"
#include "interior/units.h"

namespace real {

// A file Windows says is signed, held open so it stays the file that was checked: the handle is shared for
// reading only, so nothing may write to it, delete it or rename it while the session holds it.
struct TrustedFile
{
    UniqueHandle handle;
};

// Verifies the Authenticode signature and that the certificate names NVIDIA. Revocation is not chased,
// which would mean a network call on a path that has to work offline.
[[nodiscard]] infra::Result<TrustedFile, Error> OpenTrusted(const interior::FilePath& path) noexcept;

} // namespace real
