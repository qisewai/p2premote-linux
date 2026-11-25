#include "agent_audit.h"

#include <windows.h>
#include <winrt/base.h>

#include <cstdio>
#include <mutex>

namespace p2premote {
namespace {

constexpr DWORD audit_limit = 256 * 1024;
std::mutex audit_mutex;

const char* name(AgentAuditResult result) noexcept {
    switch (result) {
    case AgentAuditResult::approved: return "approved";
    case AgentAuditResult::denied: return "denied";
    case AgentAuditResult::expired: return "expired";
    case AgentAuditResult::revoked: return "revoked";
    case AgentAuditResult::cancelled: return "cancelled";
    case AgentAuditResult::disconnected: return "disconnected";
    case AgentAuditResult::unavailable: return "unavailable";
    }
    return "unavailable";
}

winrt::handle OpenAudit(const std::filesystem::path& path, BY_HANDLE_FILE_INFORMATION& info) {
    winrt::handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return {};
    return file;
}

} // namespace

bool RecordAgentAudit(const std::filesystem::path& directory, std::uint32_t id,
    AgentAuditResult result, std::uint64_t elapsed_ms, AgentTool tool) noexcept {
    try {
        if (directory.empty()) return false;
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        char line[256]{};
        const int length = std::snprintf(line, sizeof(line),
            "{\"utc\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\",\"id\":%u,\"tool\":\"%s\","
            "\"result\":\"%s\",\"elapsed_ms\":%llu}\n",
            static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth), static_cast<unsigned>(utc.wDay),
            static_cast<unsigned>(utc.wHour), static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond),
            id, tool == AgentTool::screen_snapshot ? "screen_snapshot" : "system_info",
            name(result), static_cast<unsigned long long>(elapsed_ms));
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(line)) return false;
        const auto bytes = static_cast<DWORD>(length);

        const std::lock_guard lock(audit_mutex);
        std::filesystem::create_directories(directory);
        const auto attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto path = directory / L"ai-agent.audit.jsonl";
        BY_HANDLE_FILE_INFORMATION info{};
        auto file = OpenAudit(path, info);
        if (!file) return false;
        SYSTEMTIME last_write{};
        const bool new_day = info.nFileSizeLow && FileTimeToSystemTime(&info.ftLastWriteTime, &last_write) &&
            (last_write.wYear != utc.wYear || last_write.wMonth != utc.wMonth || last_write.wDay != utc.wDay);
        if (new_day || info.nFileSizeHigh || info.nFileSizeLow > audit_limit - bytes) {
            file.close();
            const auto previous = directory / L"ai-agent.audit.previous.jsonl";
            if (!MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
            file = OpenAudit(path, info);
        }
        DWORD written{};
        return file && !info.nFileSizeHigh && info.nFileSizeLow <= audit_limit - bytes &&
            WriteFile(file.get(), line, bytes, &written, nullptr) && written == bytes;
    } catch (...) {
        return false;
    }
}

} // namespace p2premote
