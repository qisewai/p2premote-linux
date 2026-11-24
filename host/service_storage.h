#pragma once

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <winrt/base.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace p2premote::service {

using LocalAllocation = std::unique_ptr<void, decltype(&LocalFree)>;
inline constexpr DWORD kMaxConfigBytes = 64 * 1024;

inline std::filesystem::path ServiceDirectory() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT_PATH, nullptr, &folder))) return {};
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> free_folder(folder, CoTaskMemFree);
    return std::filesystem::path(folder) / L"P2PRemoteHost";
}

inline std::wstring UniqueSuffix() {
    GUID guid{};
    wchar_t text[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, text, static_cast<int>(std::size(text)))) return {};
    return text;
}

inline bool IsProtectedDirectory(const std::filesystem::path& path) {
    const HANDLE opened = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return false;
    const winrt::handle directory{opened};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))
            || !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;

    PSID owner{};
    PACL acl{};
    PSECURITY_DESCRIPTOR descriptor{};
    if (GetSecurityInfo(directory.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    const LocalAllocation free_descriptor(descriptor, LocalFree);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    if (!owner || (!IsWellKnownSid(owner, WinBuiltinAdministratorsSid) && !IsWellKnownSid(owner, WinLocalSystemSid))
            || !GetSecurityDescriptorControl(descriptor, &control, &revision) || !(control & SE_DACL_PROTECTED)
            || !acl || acl->AceCount != 2) return false;

    bool system{}, admins{};
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* entry{};
        if (!GetAce(acl, index, &entry)) return false;
        const auto& ace = *static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if (ace.Header.AceType != ACCESS_ALLOWED_ACE_TYPE
                || ace.Header.AceFlags != (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)
                || ace.Mask != FILE_ALL_ACCESS) return false;
        system |= IsWellKnownSid(const_cast<DWORD*>(&ace.SidStart), WinLocalSystemSid) != FALSE;
        admins |= IsWellKnownSid(const_cast<DWORD*>(&ace.SidStart), WinBuiltinAdministratorsSid) != FALSE;
    }
    return system && admins;
}

inline bool EnsureProtectedDirectory(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr)) return false;
    const LocalAllocation free_descriptor(descriptor, LocalFree);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    return IsProtectedDirectory(path);
}

inline bool WriteEncryptedConfig(const std::filesystem::path& path, const std::string& plaintext, bool staging) {
    if (plaintext.empty() || plaintext.size() > kMaxConfigBytes / 2) return false;
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"P2PRemoteServiceConfig", nullptr, nullptr, nullptr,
            CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    const LocalAllocation free_output(output.pbData, LocalFree);

    PSECURITY_DESCRIPTOR descriptor{};
    if (staging && !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)",
            SDDL_REVISION_1, &descriptor, nullptr)) return false;
    const LocalAllocation free_descriptor(descriptor, LocalFree);
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    HANDLE opened = CreateFileW(path.c_str(), GENERIC_WRITE, 0, staging ? &attributes : nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return false;
    winrt::handle file{opened};
    DWORD written{};
    const bool ok = WriteFile(file.get(), output.pbData, output.cbData, &written, nullptr)
        && written == output.cbData && FlushFileBuffers(file.get());
    file.close();
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

inline std::optional<std::string> ReadEncryptedConfig(const std::filesystem::path& path) {
    HANDLE opened = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return std::nullopt;
    const winrt::handle file{opened};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER length{};
    if (GetFileType(file.get()) != FILE_TYPE_DISK
            || !GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))
            || (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
            || !GetFileSizeEx(file.get(), &length) || length.QuadPart <= 0 || length.QuadPart > kMaxConfigBytes) {
        return std::nullopt;
    }
    std::vector<BYTE> bytes(static_cast<std::size_t>(length.QuadPart));
    DWORD read{};
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
        return std::nullopt;
    }
    DATA_BLOB input{read, bytes.data()};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return std::nullopt;
    }
    const LocalAllocation free_output(output.pbData, LocalFree);
    std::string result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    return result;
}

} // namespace p2premote::service
