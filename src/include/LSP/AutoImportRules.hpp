#pragma once

#include <optional>
#include <string>
#include <vector>

#include "LSP/ClientConfiguration.hpp"

namespace Luau::LanguageServer::AutoImports
{

/// The execution boundary a module belongs to, derived from configured boundary rules.
/// `None` represents an unclassified module (no rule matched, no platform heuristic)
enum class BoundaryContext
{
    None,
    Client,
    Server,
    Shared,
};

std::optional<BoundaryContext> parseBoundaryContext(const std::string& name);

/// The known path representations of a module, used when matching boundary and visibility rules.
/// All paths use forward slashes
struct ModulePathInfo
{
    /// Workspace-relative filesystem path (empty if unknown)
    std::string fsPath = "";
    /// Roblox DataModel virtual path, e.g. "game/ReplicatedStorage/Module" (nullopt on non-Roblox platforms).
    /// Rules are matched against this path both with and without the "game/" / "ProjectRoot/" root prefix
    std::optional<std::string> dataModelPath = std::nullopt;
};

/// Compiled form of `completion.imports.boundaries` and `completion.imports.visibilityRules`.
/// Cheap to construct - build once per auto-import request. When no configuration is present,
/// all checks preserve the default (upstream) behaviour
class ImportRuleSet
{
public:
    explicit ImportRuleSet(const ClientCompletionImportsConfiguration& config);

    /// Whether any boundary rules, visibility rules, or matrix overrides are configured
    [[nodiscard]] bool active() const
    {
        return !boundaryRules.empty() || !visibilityRules.empty() || matrixCustomised;
    }

    [[nodiscard]] bool boundariesConfigured() const
    {
        return !boundaryRules.empty();
    }

    /// Classify a module against the configured boundary rules. Later rules override earlier ones.
    /// Returns nullopt when no rule matches (callers substitute a platform heuristic);
    /// an explicit "none" rule returns BoundaryContext::None (unrestricted)
    [[nodiscard]] std::optional<BoundaryContext> classifyBoundary(const ModulePathInfo& paths) const;

    /// Whether a module in boundary `from` may import a module in boundary `target`.
    /// When boundaries are unconfigured, this reproduces the permissive default matrix
    /// (compatible if either side is Shared)
    [[nodiscard]] bool isImportAllowed(BoundaryContext from, BoundaryContext target) const;

    /// Whether `candidate` passes every configured visibility rule when imported from `from`.
    /// Rules are restrictions: all matching rules must pass
    [[nodiscard]] bool isVisibleFrom(const ModulePathInfo& from, const ModulePathInfo& candidate) const;

    /// Boundary matrix check only. `fromHeuristic`/`targetHeuristic` are platform-derived
    /// contexts substituted for paths that no boundary rule matches
    [[nodiscard]] bool isBoundaryImportAllowed(
        const ModulePathInfo& from, BoundaryContext fromHeuristic, const ModulePathInfo& target, BoundaryContext targetHeuristic) const;

    /// Combined visibility + boundary check
    [[nodiscard]] bool isAutoImportAllowed(
        const ModulePathInfo& from, BoundaryContext fromHeuristic, const ModulePathInfo& target, BoundaryContext targetHeuristic) const;

    /// Configuration problems discovered while compiling the rules. Invalid entries are skipped
    std::vector<std::string> warnings{};

private:
    struct CompiledBoundaryRule
    {
        std::string glob;
        BoundaryContext context;
    };

    struct CompiledVisibilityRule
    {
        std::optional<std::string> scope;
        std::string modules;
        std::string visibleFrom;
    };

    std::vector<CompiledBoundaryRule> boundaryRules{};
    std::vector<CompiledVisibilityRule> visibilityRules{};
    // allowedMatrix[from][target], indexed by BoundaryContext. None is always unrestricted
    bool allowedMatrix[4][4] = {};
    bool matrixCustomised = false;
};

/// Validates the boundary / visibility configuration, returning warnings for invalid entries
std::vector<std::string> validateAutoImportRules(const ClientCompletionImportsConfiguration& config);

/// Whether a module name looks like a Roblox DataModel virtual path
inline bool isDataModelPath(const std::string& name)
{
    return name == "game" || name == "ProjectRoot" || name.rfind("game/", 0) == 0 || name.rfind("ProjectRoot/", 0) == 0;
}

} // namespace Luau::LanguageServer::AutoImports
