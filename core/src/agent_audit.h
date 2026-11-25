#pragma once

#include <cstdint>
#include <filesystem>

#include <p2premote/plugin_workspace.h>
namespace p2premote {

enum class AgentAuditResult { approved, denied, expired, revoked, cancelled, disconnected, unavailable };

bool RecordAgentAudit(const std::filesystem::path& directory, std::uint32_t id,
    AgentAuditResult result, std::uint64_t elapsed_ms, AgentTool tool = AgentTool::system_info) noexcept;

} // namespace p2premote
