#pragma once

#if defined(_WIN32)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Cryptography.Certificates.h>
#endif

namespace p2premote {

inline constexpr wchar_t wuhan_service_address[] = L"https://111.170.34.243:8443";
inline constexpr char wuhan_public_key_pin[] = "sha256//2usEgxHqDFfik2rJRjTyfUbQVWdV64AxCzRfU34dVB4=";
inline constexpr char wuhan_ca_certificate[] = R"pem(-----BEGIN CERTIFICATE-----
MIIBsTCCAVigAwIBAgIUNvMOdraMOmYLrb5CL69Ej055gYowCgYIKoZIzj0EAwIw
JTEjMCEGA1UEAwwaUDJQUmVtb3RlIFd1aGFuIFByaXZhdGUgQ0EwHhcNMjYxMTIy
MTQzNTMyWhcNMzYxMTE5MTQzNTMyWjAlMSMwIQYDVQQDDBpQMlBSZW1vdGUgV3Vo
YW4gUHJpdmF0ZSBDQTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABPHfGaJQjGLr
eOM2wHUzfeG5oGMOJUxdbqDwu72sCiyc6eJMPyiA6hsFsiCBfFOameKuPtvsE2kL
d/DrwbRuKEmjZjBkMB0GA1UdDgQWBBRcIbzmPdT7v5/U2GbaKVqqUc/EADAfBgNV
HSMEGDAWgBRcIbzmPdT7v5/U2GbaKVqqUc/EADASBgNVHRMBAf8ECDAGAQH/AgEA
MA4GA1UdDwEB/wQEAwIBBjAKBggqhkjOPQQDAgNHADBEAiApP9cFwXhG6iJwVlLL
wxS5CxfWc+KP+E4C87/LhMy34gIgRo8lpnUFX0+kc5BGVbJGctxjlgtUwnigwPW4
mAvduqw=
-----END CERTIFICATE-----
)pem";

#if defined(_WIN32)
bool IsWuhanService(const winrt::Windows::Foundation::Uri& endpoint);
bool MatchesWuhanServerKey(const winrt::Windows::Security::Cryptography::Certificates::Certificate& certificate) noexcept;
#endif

} // namespace p2premote
