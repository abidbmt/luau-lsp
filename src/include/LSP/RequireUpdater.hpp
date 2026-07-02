#pragma once

#include <optional>
#include <string>
#include <vector>

#include "LSP/Uri.hpp"
#include "Protocol/Structures.hpp"

class WorkspaceFolder;

namespace Luau::LanguageServer::RequireUpdates
{

/// A script that was part of a rename, captured from the sourcemap before it is regenerated.
/// Used to match instance requires (e.g. `ReplicatedStorage.Utils.Module`) after the move
struct MovedInstanceModule
{
    /// The DataModel path of the module before the move (e.g. "game/ReplicatedStorage/Utils/Module")
    std::string oldVirtualPath;
    /// Where the module's file lives after the move
    Uri newRealUri;
};

/// A file or folder rename, with enough captured state to resolve requires that referenced
/// the old location even after the sourcemap / frontend state has been rebuilt
struct RenameOperation
{
    Uri oldUri;
    Uri newUri;
    std::vector<MovedInstanceModule> movedInstanceModules{};
};

struct RequireUpdateResult
{
    lsp::WorkspaceEdit edit{};
    size_t updatedRequireCount = 0;
    /// Human-readable descriptions of requires that resolve to the moved module but could not
    /// be safely rewritten
    std::vector<std::string> skipped{};
    /// True when some rewrites failed only because the sourcemap does not yet contain the new
    /// file locations - recompute after the next sourcemap update
    bool needsSourcemapUpdate = false;
};

/// Captures a rename operation, recording the DataModel paths of all moved scripts while the
/// (pre-regeneration) sourcemap still knows about the old location
RenameOperation captureRenameOperation(WorkspaceFolder& workspace, const Uri& oldUri, const Uri& newUri);

/// Computes the workspace edit that rewrites all requires resolving to the renamed file/folder
/// so they resolve to the new location, preserving each require's existing style. Only requires
/// whose resolved target matches the rename are rewritten. When `limitToFiles` is given, only
/// requires inside those files are updated
RequireUpdateResult computeRequireUpdates(
    WorkspaceFolder& workspace, const RenameOperation& operation, const std::optional<std::vector<Uri>>& limitToFiles = std::nullopt);

} // namespace Luau::LanguageServer::RequireUpdates
