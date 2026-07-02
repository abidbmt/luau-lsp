#pragma once
#include <optional>
#include <string>
#include "Protocol/Structures.hpp"
#include "nlohmann/json.hpp"

namespace lsp
{
struct DidChangeConfigurationParams
{
    json settings;
};
NLOHMANN_DEFINE_OPTIONAL(DidChangeConfigurationParams, settings)

struct ConfigurationItem
{
    std::optional<DocumentUri> scopeUri = std::nullopt;
    std::optional<std::string> section = std::nullopt;
};
NLOHMANN_DEFINE_OPTIONAL(ConfigurationItem, scopeUri, section)

struct ConfigurationParams
{
    std::vector<ConfigurationItem> items{};
};
NLOHMANN_DEFINE_OPTIONAL(ConfigurationParams, items)

using GetConfigurationResponse = std::vector<json>;

using Pattern = std::string;
using GlobPattern = Pattern; // | RelativePattern

enum WatchKind
{
    Create = 1,
    Change = 2,
    Delete = 4,
};

struct FileSystemWatcher
{
    GlobPattern globPattern;
    int kind = WatchKind::Create | WatchKind::Change | WatchKind::Delete;
};
NLOHMANN_DEFINE_OPTIONAL(FileSystemWatcher, globPattern, kind)

struct DidChangeWatchedFilesRegistrationOptions
{
    std::vector<FileSystemWatcher> watchers{};
};
NLOHMANN_DEFINE_OPTIONAL(DidChangeWatchedFilesRegistrationOptions, watchers)

enum struct FileChangeType
{
    Created = 1,
    Changed = 2,
    Deleted = 3,
};

struct FileEvent
{
    DocumentUri uri;
    FileChangeType type = FileChangeType::Created;
};
NLOHMANN_DEFINE_OPTIONAL(FileEvent, uri, type)

struct DidChangeWatchedFilesParams
{
    std::vector<FileEvent> changes{};
};
NLOHMANN_DEFINE_OPTIONAL(DidChangeWatchedFilesParams, changes)

struct WorkspaceFoldersChangeEvent
{
    std::vector<WorkspaceFolder> added{};
    std::vector<WorkspaceFolder> removed{};
};
NLOHMANN_DEFINE_OPTIONAL(WorkspaceFoldersChangeEvent, added, removed)

struct DidChangeWorkspaceFoldersParams
{
    WorkspaceFoldersChangeEvent event;
};
NLOHMANN_DEFINE_OPTIONAL(DidChangeWorkspaceFoldersParams, event)

enum struct FileOperationPatternKind
{
    File,
    Folder,
};
NLOHMANN_JSON_SERIALIZE_ENUM(FileOperationPatternKind, {
                                                           {FileOperationPatternKind::File, "file"},
                                                           {FileOperationPatternKind::Folder, "folder"},
                                                       })

struct FileOperationPattern
{
    std::string glob;
    std::optional<FileOperationPatternKind> matches = std::nullopt;
};
NLOHMANN_DEFINE_OPTIONAL(FileOperationPattern, glob, matches)

struct FileOperationFilter
{
    std::optional<std::string> scheme = std::nullopt;
    FileOperationPattern pattern;
};
NLOHMANN_DEFINE_OPTIONAL(FileOperationFilter, scheme, pattern)

struct FileOperationRegistrationOptions
{
    std::vector<FileOperationFilter> filters{};
};
NLOHMANN_DEFINE_OPTIONAL(FileOperationRegistrationOptions, filters)

/// Represents information on a file/folder rename
struct FileRename
{
    /// A file:// URI for the original location of the file/folder being renamed
    std::string oldUri;
    /// A file:// URI for the new location of the file/folder being renamed
    std::string newUri;
};
NLOHMANN_DEFINE_OPTIONAL(FileRename, oldUri, newUri)

/// The parameters sent in notifications/requests for user-initiated renames of files
struct RenameFilesParams
{
    /// An array of all files/folders renamed in this operation. When a folder is renamed,
    /// only the folder will be included, and not its children
    std::vector<FileRename> files{};
};
NLOHMANN_DEFINE_OPTIONAL(RenameFilesParams, files)

struct ApplyWorkspaceEditParams
{
    std::optional<std::string> label = std::nullopt;
    WorkspaceEdit edit;
};
NLOHMANN_DEFINE_OPTIONAL(ApplyWorkspaceEditParams, label, edit)

struct ApplyWorkspaceEditResult
{
    bool applied = false;
    std::optional<std::string> failureReason = std::nullopt;
    std::optional<size_t> failedChange = std::nullopt;
};
NLOHMANN_DEFINE_OPTIONAL(ApplyWorkspaceEditResult, applied, failureReason, failedChange)
} // namespace lsp
