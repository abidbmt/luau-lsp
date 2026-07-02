#pragma once
#include <optional>
#include <string>
#include <vector>

namespace lsp
{
enum struct MessageType
{
    Error = 1,
    Warning = 2,
    Info = 3,
    Log = 4,
};

struct LogMessageParams
{
    MessageType type = MessageType::Error;
    std::string message;
};
NLOHMANN_DEFINE_OPTIONAL(LogMessageParams, type, message)

struct ShowMessageParams
{
    MessageType type = MessageType::Error;
    std::string message;
};
NLOHMANN_DEFINE_OPTIONAL(ShowMessageParams, type, message)

struct MessageActionItem
{
    /// A short title like 'Retry', 'Open Log' etc.
    std::string title;
};
NLOHMANN_DEFINE_OPTIONAL(MessageActionItem, title)

struct ShowMessageRequestParams
{
    MessageType type = MessageType::Error;
    std::string message;
    /// The message action items to present
    std::optional<std::vector<MessageActionItem>> actions = std::nullopt;
};
NLOHMANN_DEFINE_OPTIONAL(ShowMessageRequestParams, type, message, actions)
} // namespace lsp
