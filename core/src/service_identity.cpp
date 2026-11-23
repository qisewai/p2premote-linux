#include <p2premote/service_identity.h>

#include <windows.h>
#include <wincrypt.h>
#include <winrt/Windows.Security.Cryptography.h>

#include <array>
#include <memory>

namespace p2premote {

bool IsWuhanService(const winrt::Windows::Foundation::Uri& endpoint) {
    const auto scheme = endpoint.SchemeName();
    return (scheme == L"https" || scheme == L"wss")
        && endpoint.Host() == L"111.170.34.243" && endpoint.Port() == 8443;
}

bool MatchesWuhanServerKey(
    const winrt::Windows::Security::Cryptography::Certificates::Certificate& certificate) noexcept {
    try {
        using winrt::Windows::Security::Cryptography::CryptographicBuffer;
        if (!certificate) return false;
        winrt::com_array<uint8_t> der;
        CryptographicBuffer::CopyToByteArray(certificate.GetCertificateBlob(), der);
        const std::unique_ptr<const CERT_CONTEXT, decltype(&CertFreeCertificateContext)> context(
            CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), der.size()), CertFreeCertificateContext);
        if (!context) return false;

        void* encoded_key{};
        DWORD encoded_size{};
        if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &context->pCertInfo->SubjectPublicKeyInfo,
            CRYPT_ENCODE_ALLOC_FLAG, nullptr, &encoded_key, &encoded_size)) {
            return false;
        }
        const std::unique_ptr<void, decltype(&LocalFree)> key(encoded_key, LocalFree);
        std::array<uint8_t, 32> hash{};
        DWORD hash_size = static_cast<DWORD>(hash.size());
        if (!CryptHashCertificate2(L"SHA256", 0, nullptr, static_cast<const BYTE*>(key.get()), encoded_size,
            hash.data(), &hash_size) || hash_size != hash.size()) {
            return false;
        }
        const auto pin = CryptographicBuffer::EncodeToBase64String(CryptographicBuffer::CreateFromByteArray(hash));
        return pin == winrt::to_hstring(wuhan_public_key_pin + sizeof("sha256//") - 1);
    } catch (...) {
        return false;
    }
}

} // namespace p2premote
