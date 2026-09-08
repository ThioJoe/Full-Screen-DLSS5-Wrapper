#include "effects/real/trust.h"

#include <softpub.h>
#include <wintrust.h>

#include <array>
#include <string_view>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;

// WINTRUST_ACTION_GENERIC_VERIFY_V2 is a macro over an initialiser, so it is named once here. The call
// takes a mutable pointer to it and does not write through it.
GUID kVerifyAction = WINTRUST_ACTION_GENERIC_VERIFY_V2; // WAIVER(R2): a constant the API insists on being handed by non-const pointer.
constexpr std::wstring_view kSigner = L"NVIDIA";
constexpr std::size_t kNameCapacity = 256;

// Shared for reading only, so nothing else may write to the file, delete it or rename it while it is held.
[[nodiscard]] Result<UniqueHandle, Error> OpenForReading(const wchar_t* path) noexcept
{
    void* handle = ::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return Fail(LastError(ApiCall::OpenModelFile));
    return UniqueHandle(handle);
}

[[nodiscard]] WINTRUST_FILE_INFO FileInfoFor(const wchar_t* path, void* handle) noexcept
{
    WINTRUST_FILE_INFO file{}; // WAIVER(R2): a request record filled once, before it is asked.
    file.cbStruct = sizeof(WINTRUST_FILE_INFO);
    file.pcwszFilePath = path;
    file.hFile = handle;
    return file;
}

// Asked of the handle already held rather than of the path, so it cannot be answered about another file.
// WAIVER(R1): one API record, filled field by field because that is the only way it can be filled.
[[nodiscard]] WINTRUST_DATA RequestFor(WINTRUST_FILE_INFO* file) noexcept
{
    WINTRUST_DATA request{}; // WAIVER(R2): a request record filled once, before it is asked.
    request.cbStruct = sizeof(WINTRUST_DATA);
    request.dwUIChoice = WTD_UI_NONE;
    request.fdwRevocationChecks = WTD_REVOKE_NONE;
    request.dwUnionChoice = WTD_CHOICE_FILE;
    request.pFile = file;
    request.dwStateAction = WTD_STATEACTION_VERIFY;
    request.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;
    return request;
}

[[nodiscard]] CRYPT_PROVIDER_SGNR* SignerOf(CRYPT_PROVIDER_DATA* provider) noexcept
{
    return provider == nullptr ? nullptr : ::WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
}

[[nodiscard]] CRYPT_PROVIDER_CERT* CertificateOf(CRYPT_PROVIDER_SGNR* signer) noexcept
{
    return signer == nullptr ? nullptr : ::WTHelperGetProvCertFromChain(signer, 0);
}

[[nodiscard]] const CERT_CONTEXT* SigningCertificate(HANDLE state) noexcept
{
    CRYPT_PROVIDER_CERT* certificate = CertificateOf(SignerOf(::WTHelperProvDataFromStateData(state)));
    return certificate == nullptr ? nullptr : certificate->pCert;
}

[[nodiscard]] std::array<wchar_t, kNameCapacity> NameOfCertificate(const CERT_CONTEXT* certificate) noexcept
{
    std::array<wchar_t, kNameCapacity> name{}; // WAIVER(R2): a local buffer filled once, before use.
    (void)::CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), kNameCapacity);
    return name;
}

[[nodiscard]] bool NamesNvidia(HANDLE state) noexcept
{
    const CERT_CONTEXT* certificate = SigningCertificate(state);
    if (certificate == nullptr)
        return false;
    const std::array<wchar_t, kNameCapacity> name = NameOfCertificate(certificate);
    return std::wstring_view(name.data()).starts_with(kSigner);
}

[[nodiscard]] Status<Error> FromNvidia(HANDLE state) noexcept
{
    if (!NamesNvidia(state))
        return Fail(Error{ ApiCall::ModelNotFromNvidia, 0 });
    return {};
}

[[nodiscard]] Status<Error> SignedByNvidia(HANDLE state, LONG verdict) noexcept
{
    if (verdict != ERROR_SUCCESS)
        return Fail(Error{ ApiCall::ModelNotSigned, static_cast<std::uint32_t>(verdict) });
    return FromNvidia(state);
}

// The verification allocates state that has to be given back whatever the answer was.
void CloseVerification(WINTRUST_DATA& request) noexcept
{
    request.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)::WinVerifyTrust(nullptr, &kVerifyAction, &request);
}

[[nodiscard]] Status<Error> Answered(WINTRUST_DATA& request) noexcept
{
    const LONG verdict = ::WinVerifyTrust(nullptr, &kVerifyAction, &request);
    const Status<Error> answer = SignedByNvidia(request.hWVTStateData, verdict);
    CloseVerification(request);
    return answer;
}

[[nodiscard]] Status<Error> Verified(const wchar_t* path, void* handle) noexcept
{
    WINTRUST_FILE_INFO file = FileInfoFor(path, handle);
    WINTRUST_DATA request = RequestFor(&file); // WAIVER(R2): the call writes its state into the record it is given.
    return Answered(request);
}

} // namespace

Result<TrustedFile, Error> OpenTrusted(const interior::FilePath& path) noexcept
{
    return OpenForReading(path.CString()).and_then([&path](UniqueHandle handle) { return Verified(path.CString(), handle.get()).transform([&handle] { return TrustedFile{ std::move(handle) }; }); });
}

} // namespace real
