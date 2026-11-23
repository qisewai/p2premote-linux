#include "credentials.h"

#include <windows.h>
#include <wincred.h>

#include <memory>

namespace {

std::wstring targetFor(const QString& server) {
    return (QStringLiteral("P2PRemote/Qt/") + server.trimmed()).toStdWString();
}

} // namespace

Credentials::~Credentials() {
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
}

std::optional<Credentials> readCredentials(const QString& server) {
    const auto target = targetFor(server);
    PCREDENTIALW raw{};
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw)) return std::nullopt;
    const std::unique_ptr<CREDENTIALW, decltype(&CredFree)> entry(raw, CredFree);
    if (!entry->UserName || !entry->CredentialBlob || !entry->CredentialBlobSize ||
        entry->CredentialBlobSize % sizeof(wchar_t)) return std::nullopt;
    const auto* data = reinterpret_cast<const wchar_t*>(entry->CredentialBlob);
    const auto length = entry->CredentialBlobSize / sizeof(wchar_t);
    std::optional<Credentials> credentials(std::in_place, server, QString::fromWCharArray(entry->UserName),
        std::wstring(data, length), true);
    SecureZeroMemory(entry->CredentialBlob, entry->CredentialBlobSize);
    return credentials;
}

bool saveCredentials(const Credentials& credentials) {
    auto target = targetFor(credentials.server);
    auto user = credentials.user.toStdWString();
    const auto bytes = credentials.password.size() * sizeof(wchar_t);
    if (bytes > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
    CREDENTIALW entry{};
    entry.Type = CRED_TYPE_GENERIC;
    entry.TargetName = target.data();
    entry.UserName = user.data();
    entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
    entry.CredentialBlobSize = static_cast<DWORD>(bytes);
    entry.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(credentials.password.data()));
    return CredWriteW(&entry, 0) != FALSE;
}

bool deleteCredentials(const QString& server) {
    const auto target = targetFor(server);
    return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
}
