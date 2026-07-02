#include "LSP/AutoImportRules.hpp"

#include <cstring>

#include "Luau/StringUtils.h"
#include "glob/match.h"

namespace Luau::LanguageServer::AutoImports
{

static constexpr const char* kDataModelRoots[] = {"game/", "ProjectRoot/"};

std::optional<BoundaryContext> parseBoundaryContext(const std::string& name)
{
    if (name == "none")
        return BoundaryContext::None;
    if (name == "client")
        return BoundaryContext::Client;
    if (name == "server")
        return BoundaryContext::Server;
    if (name == "shared")
        return BoundaryContext::Shared;
    return std::nullopt;
}

/// All ways a module path may be written in a rule: the workspace-relative filesystem path, the
/// DataModel path, and the DataModel path with its root ("game/" / "ProjectRoot/") stripped
static std::vector<std::string> pathRepresentations(const ModulePathInfo& paths)
{
    std::vector<std::string> representations{};
    if (!paths.fsPath.empty())
        representations.push_back(paths.fsPath);
    if (paths.dataModelPath)
    {
        representations.push_back(*paths.dataModelPath);
        for (const char* root : kDataModelRoots)
            if (Luau::startsWith(*paths.dataModelPath, root))
                representations.push_back(paths.dataModelPath->substr(strlen(root)));
    }
    return representations;
}

static bool matchesModulePaths(const std::string& pattern, const ModulePathInfo& paths)
{
    for (const auto& representation : pathRepresentations(paths))
        if (glob::gitignore_glob_match(representation, pattern))
            return true;
    return false;
}

ImportRuleSet::ImportRuleSet(const ClientCompletionImportsConfiguration& config)
{
    for (size_t i = 0; i < config.boundaries.rules.size(); i++)
    {
        const auto& rule = config.boundaries.rules[i];
        if (rule.glob.empty())
        {
            warnings.push_back("completion.imports.boundaries.rules[" + std::to_string(i) + "]: 'glob' must not be empty, rule ignored");
            continue;
        }
        auto context = parseBoundaryContext(rule.context);
        if (!context)
        {
            warnings.push_back("completion.imports.boundaries.rules[" + std::to_string(i) + "]: unknown context '" + rule.context +
                               "' (expected 'client', 'server', 'shared' or 'none'), rule ignored");
            continue;
        }
        boundaryRules.push_back({rule.glob, *context});
    }

    // None is unrestricted in both directions
    for (auto context : {BoundaryContext::None, BoundaryContext::Client, BoundaryContext::Server, BoundaryContext::Shared})
    {
        allowedMatrix[size_t(BoundaryContext::None)][size_t(context)] = true;
        allowedMatrix[size_t(context)][size_t(BoundaryContext::None)] = true;
    }

    for (auto from : {BoundaryContext::Client, BoundaryContext::Server, BoundaryContext::Shared})
        for (auto target : {BoundaryContext::Client, BoundaryContext::Server, BoundaryContext::Shared})
        {
            if (boundaryRules.empty())
                // Permissive default, matching isScriptContextCompatible: allowed if either side is Shared
                allowedMatrix[size_t(from)][size_t(target)] =
                    from == target || from == BoundaryContext::Shared || target == BoundaryContext::Shared;
            else
                // Strict default: client <- {client, shared}, server <- {server, shared}, shared <- {shared}
                allowedMatrix[size_t(from)][size_t(target)] = from == target || target == BoundaryContext::Shared;
        }

    for (const auto& [from, targets] : config.boundaries.allowedImports)
    {
        auto fromContext = parseBoundaryContext(from);
        if (!fromContext || *fromContext == BoundaryContext::None)
        {
            warnings.push_back("completion.imports.boundaries.allowedImports: unknown boundary '" + from + "', entry ignored");
            continue;
        }
        bool row[4] = {true, false, false, false}; // None stays unrestricted
        bool valid = true;
        for (const auto& target : targets)
        {
            auto targetContext = parseBoundaryContext(target);
            if (!targetContext || *targetContext == BoundaryContext::None)
            {
                warnings.push_back(
                    "completion.imports.boundaries.allowedImports." + from + ": unknown boundary '" + target + "', entry ignored");
                valid = false;
                break;
            }
            row[size_t(*targetContext)] = true;
        }
        if (!valid)
            continue;
        for (size_t target = 0; target < 4; target++)
            allowedMatrix[size_t(*fromContext)][target] = row[target];
        matrixCustomised = true;
    }

    for (size_t i = 0; i < config.visibilityRules.size(); i++)
    {
        const auto& rule = config.visibilityRules[i];
        if (rule.modules.empty())
        {
            warnings.push_back("completion.imports.visibilityRules[" + std::to_string(i) + "]: 'modules' is required, rule ignored");
            continue;
        }
        if (rule.scope.empty() && rule.visibleFrom.empty())
        {
            warnings.push_back(
                "completion.imports.visibilityRules[" + std::to_string(i) + "]: either 'scope' or 'visibleFrom' is required, rule ignored");
            continue;
        }
        visibilityRules.push_back({
            rule.scope.empty() ? std::nullopt : std::optional(rule.scope),
            rule.modules,
            rule.visibleFrom.empty() ? "**" : rule.visibleFrom,
        });
    }
}

std::optional<BoundaryContext> ImportRuleSet::classifyBoundary(const ModulePathInfo& paths) const
{
    // Later rules override earlier ones
    std::optional<BoundaryContext> result = std::nullopt;
    for (const auto& rule : boundaryRules)
        if (matchesModulePaths(rule.glob, paths))
            result = rule.context;
    return result;
}

bool ImportRuleSet::isImportAllowed(BoundaryContext from, BoundaryContext target) const
{
    return allowedMatrix[size_t(from)][size_t(target)];
}

/// Finds the concrete ancestor directory of `paths` matching the scope glob, e.g. resolving the
/// scope "src/Features/*" to "src/Features/MyFeature". Returns the longest (innermost) matching
/// prefix across all path representations
static std::optional<std::string> findScopeDirectory(const ModulePathInfo& paths, const std::string& scopeGlob)
{
    std::optional<std::string> best = std::nullopt;
    for (const auto& representation : pathRepresentations(paths))
    {
        // Iterate proper ancestor prefixes, innermost first; the first match is the longest for this representation
        for (size_t pos = representation.rfind('/'); pos != std::string::npos && pos != 0; pos = representation.rfind('/', pos - 1))
        {
            auto prefix = representation.substr(0, pos);
            if (glob::gitignore_glob_match(prefix, scopeGlob))
            {
                if (!best || prefix.size() > best->size())
                    best = prefix;
                break;
            }
        }
    }
    return best;
}

/// Whether any representation of `paths` lies inside `directory` and its remaining path matches `pattern`
static bool matchesRelativeToDirectory(const ModulePathInfo& paths, const std::string& directory, const std::string& pattern)
{
    auto prefix = directory + "/";
    for (const auto& representation : pathRepresentations(paths))
        if (Luau::startsWith(representation, prefix) && glob::gitignore_glob_match(representation.substr(prefix.size()), pattern))
            return true;
    return false;
}

bool ImportRuleSet::isVisibleFrom(const ModulePathInfo& from, const ModulePathInfo& candidate) const
{
    for (const auto& rule : visibilityRules)
    {
        if (rule.scope)
        {
            auto scopeDirectory = findScopeDirectory(candidate, *rule.scope);
            if (!scopeDirectory)
                continue; // candidate is not inside any scope directory - rule does not apply
            if (!matchesRelativeToDirectory(candidate, *scopeDirectory, rule.modules))
                continue; // candidate is not restricted by this rule
            if (!matchesRelativeToDirectory(from, *scopeDirectory, rule.visibleFrom))
                return false;
        }
        else
        {
            if (!matchesModulePaths(rule.modules, candidate))
                continue;
            if (!matchesModulePaths(rule.visibleFrom, from))
                return false;
        }
    }
    return true;
}

bool ImportRuleSet::isBoundaryImportAllowed(
    const ModulePathInfo& from, BoundaryContext fromHeuristic, const ModulePathInfo& target, BoundaryContext targetHeuristic) const
{
    return isImportAllowed(classifyBoundary(from).value_or(fromHeuristic), classifyBoundary(target).value_or(targetHeuristic));
}

bool ImportRuleSet::isAutoImportAllowed(
    const ModulePathInfo& from, BoundaryContext fromHeuristic, const ModulePathInfo& target, BoundaryContext targetHeuristic) const
{
    if (!isVisibleFrom(from, target))
        return false;
    return isBoundaryImportAllowed(from, fromHeuristic, target, targetHeuristic);
}

std::vector<std::string> validateAutoImportRules(const ClientCompletionImportsConfiguration& config)
{
    return ImportRuleSet(config).warnings;
}

} // namespace Luau::LanguageServer::AutoImports
