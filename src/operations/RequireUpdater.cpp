#include "LSP/RequireUpdater.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "LSP/LuauExt.hpp"
#include "LSP/Utils.hpp"
#include "LSP/Workspace.hpp"
#include "LuauFileUtils.hpp"
#include "Luau/Parser.h"
#include "Luau/RequireTracer.h"
#include "Luau/StringUtils.h"
#include "Platform/ImportSections.hpp"
#include "Platform/InstanceRequireAutoImporter.hpp"
#include "Platform/RobloxPlatform.hpp"
#include "Platform/StringRequireAutoImporter.hpp"

namespace Luau::LanguageServer::RequireUpdates
{

/// Returns the new URI for `target` if it is the renamed file/folder itself or a descendant of it
static std::optional<Uri> mapMovedUri(const Uri& target, const RenameOperation& operation)
{
    if (target == operation.oldUri)
        return operation.newUri;
    if (operation.oldUri.isAncestorOf(target))
        return operation.newUri.resolvePath(target.lexicallyRelative(operation.oldUri));
    return std::nullopt;
}

/// Maps an extensionless require base path (e.g. resolved from "./Module") onto the rename,
/// trying the extensions and init files that string requires may omit.
/// Returns {oldUriWithExtension, newUri}
static std::optional<std::pair<Uri, Uri>> mapMovedBaseUri(const Uri& base, const RenameOperation& operation)
{
    for (const char* extension : {"", ".luau", ".lua", "/init.luau", "/init.lua"})
    {
        Uri candidate = base;
        candidate.path = base.path + extension;
        if (auto mapped = mapMovedUri(candidate, operation))
            return std::pair{candidate, *mapped};
    }
    return std::nullopt;
}

/// Resolves a require string to its extensionless target path using pure path arithmetic
/// (mirroring LSPPlatform::resolveStringRequire, minus filesystem existence checks), so that
/// requires still resolve after their target has already been moved on disk
static std::optional<Uri> resolveStringRequireBase(
    WorkspaceFolder& workspace, const Luau::ModuleName& fromModule, const Uri& fromUri, const std::string& requiredString)
{
    auto baseUri = fromUri.parent();
    if (!baseUri)
        return std::nullopt;

    auto clientConfig = workspace.client->getConfiguration(workspace.rootUri);
    if (isInitLuauFile(fromUri) && !clientConfig.require.useOriginalRequireByStringSemantics)
    {
        baseUri = baseUri->parent();
        if (!baseUri)
            return std::nullopt;
    }

    auto fileUri = baseUri->resolvePath(requiredString);

    auto luauConfig = workspace.fileResolver.getConfig(fromModule, workspace.limits);
    if (auto aliasedPath = resolveAlias(requiredString, luauConfig, *fromUri.parent()))
        fileUri = *aliasedPath;

    return fileUri;
}

struct FindRequireCallsVisitor : public Luau::AstVisitor
{
    std::vector<Luau::AstExpr*> requireArguments{};

    bool visit(Luau::AstExprCall* call) override
    {
        if (isRequire(call) && call->args.size == 1)
            requireArguments.push_back(call->args.data[0]);
        return true;
    }
};

// Keep in sync with the parser's reserved words (`const` is contextual and deliberately absent)
static bool isReservedKeyword(std::string_view name)
{
    static const std::string_view keywords[] = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local",
        "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    return std::find(std::begin(keywords), std::end(keywords), name) != std::end(keywords);
}

/// The name a module gets from a string require path: the last component minus its extension,
/// with trailing init files resolving to their directory's name
static std::optional<std::string> stringRequireLeafName(std::string path)
{
    for (std::string_view extension : {".luau", ".lua"})
        if (path.size() > extension.size() && path.compare(path.size() - extension.size(), extension.size(), extension) == 0)
        {
            path.resize(path.size() - extension.size());
            break;
        }

    auto lastComponent = [](const std::string& p)
    {
        auto pos = p.find_last_of('/');
        return pos == std::string::npos ? p : p.substr(pos + 1);
    };

    auto leaf = lastComponent(path);
    if (leaf == "init")
    {
        auto pos = path.find_last_of('/');
        if (pos == std::string::npos)
            return std::nullopt;
        leaf = lastComponent(path.substr(0, pos));
    }
    if (leaf.empty())
        return std::nullopt;
    return leaf;
}

/// The name of the instance a require argument currently refers to (the final chain component)
static std::optional<std::string> requireArgumentLeafName(Luau::AstExpr* argument)
{
    if (auto* indexName = argument->as<Luau::AstExprIndexName>())
        return std::string(indexName->index.value);
    if (auto* indexExpr = argument->as<Luau::AstExprIndexExpr>())
        if (auto* str = indexExpr->index->as<Luau::AstExprConstantString>())
            return std::string(str->value.data, str->value.size);
    return std::nullopt;
}

/// The name a module gets from a rewritten instance require path (the final dot component;
/// bracketed or call components produce non-identifiers that the caller rejects)
static std::string instanceRequireLeafName(const std::string& scriptPath)
{
    auto pos = scriptPath.find_last_of('.');
    return pos == std::string::npos ? scriptPath : scriptPath.substr(pos + 1);
}

/// Finds the local a require call's result is directly bound to (`local X = require(...)`,
/// looking through parentheses and type assertions)
struct FindRequireBindingVisitor : public Luau::AstVisitor
{
    Luau::AstExpr* requireArgument;
    Luau::AstLocal* binding = nullptr;

    explicit FindRequireBindingVisitor(Luau::AstExpr* requireArgument)
        : requireArgument(requireArgument)
    {
    }

    bool visit(Luau::AstStatLocal* stat) override
    {
        for (size_t i = 0; i < stat->values.size && i < stat->vars.size; ++i)
        {
            auto* value = stat->values.data[i];
            while (true)
            {
                if (auto* assertion = value->as<Luau::AstExprTypeAssertion>())
                    value = assertion->expr;
                else if (auto* group = value->as<Luau::AstExprGroup>())
                    value = group->expr;
                else
                    break;
            }
            if (auto* call = value->as<Luau::AstExprCall>(); call && call->args.size == 1 && call->args.data[0] == requireArgument)
                binding = stat->vars.data[i];
        }
        return true;
    }
};

/// Surveys binding names before renaming `oldName` to `newName`: more than one binding named
/// `oldName` makes type-prefix references ambiguous, and an existing `newName` would be
/// captured by (or capture) the renamed variable
struct VariableNameSurvey : public Luau::AstVisitor
{
    std::string_view oldName;
    std::string_view newName;
    size_t oldNameBindings = 0;
    bool newNameInUse = false;

    VariableNameSurvey(std::string_view oldName, std::string_view newName)
        : oldName(oldName)
        , newName(newName)
    {
    }

    void checkBinding(Luau::AstLocal* local)
    {
        if (local->name.value == oldName)
            oldNameBindings++;
        if (local->name.value == newName)
            newNameInUse = true;
    }

    bool visit(Luau::AstStatLocal* stat) override
    {
        for (auto* var : stat->vars)
            checkBinding(var);
        return true;
    }

    bool visit(Luau::AstStatLocalFunction* stat) override
    {
        checkBinding(stat->name);
        return true;
    }

    bool visit(Luau::AstExprFunction* function) override
    {
        for (auto* arg : function->args)
            checkBinding(arg);
        if (function->self)
            checkBinding(function->self);
        return true;
    }

    bool visit(Luau::AstStatFor* stat) override
    {
        checkBinding(stat->var);
        return true;
    }

    bool visit(Luau::AstStatForIn* stat) override
    {
        for (auto* var : stat->vars)
            checkBinding(var);
        return true;
    }

    bool visit(Luau::AstExprGlobal* global) override
    {
        if (global->name.value == newName)
            newNameInUse = true;
        return true;
    }
};

enum struct InstanceChainRoot
{
    Script,
    Game,
    ServiceLocal,
    GetServiceCall,
};

/// Analyses a require argument expression, accepting only plain instance chains
/// (`script.Parent.X`, `ReplicatedStorage.X`, `game.ReplicatedStorage.X`,
/// `game:GetService("X").Y`, including `["X"]` indexing). Chains involving other calls
/// (e.g. WaitForChild) are rejected so that their semantics are not silently changed
static std::optional<InstanceChainRoot> analyzeInstanceChain(Luau::AstExpr* expr)
{
    auto* node = expr;
    while (true)
    {
        if (auto* indexName = node->as<Luau::AstExprIndexName>())
            node = indexName->expr;
        else if (auto* indexExpr = node->as<Luau::AstExprIndexExpr>())
        {
            if (!indexExpr->index->is<Luau::AstExprConstantString>())
                return std::nullopt;
            node = indexExpr->expr;
        }
        else
            break;
    }

    if (auto* global = node->as<Luau::AstExprGlobal>())
    {
        if (global->name == "script")
            return InstanceChainRoot::Script;
        if (global->name == "game")
            return InstanceChainRoot::Game;
        return std::nullopt;
    }
    if (node->is<Luau::AstExprLocal>())
        return InstanceChainRoot::ServiceLocal;
    if (auto* call = node->as<Luau::AstExprCall>(); call && isGetService(call))
        return InstanceChainRoot::GetServiceCall;
    return std::nullopt;
}

/// Parses document text into a standalone source module (mirrors the private
/// Luau::Frontend::parse). The frontend's stored ASTs are deliberately not used: a physical
/// file can be indexed under several module names (stale names are never evicted) and some of
/// their ASTs predate the current content - ranges computed from those would corrupt the
/// document on edit
static std::optional<Luau::SourceModule> parseCurrentText(
    WorkspaceFolder& workspace, const Luau::ModuleName& moduleName, const std::string& text)
{
    Luau::SourceModule sourceModule;
    auto parseOptions = workspace.fileResolver.getConfig(moduleName, workspace.limits).parseOptions;
    parseOptions.captureComments = true;

    auto parseResult = Luau::Parser::parse(text.data(), text.size(), *sourceModule.names, *sourceModule.allocator, parseOptions);
    if (!parseResult.root)
        return std::nullopt;

    sourceModule.name = moduleName;
    sourceModule.root = parseResult.root;
    sourceModule.hotcomments = std::move(parseResult.hotcomments);
    sourceModule.commentLocations = std::move(parseResult.commentLocations);
    return sourceModule;
}

RenameOperation captureRenameOperation(WorkspaceFolder& workspace, const Uri& oldUri, const Uri& newUri)
{
    RenameOperation operation{oldUri, newUri};

    if (auto* robloxPlatform = dynamic_cast<RobloxPlatform*>(workspace.platform.get()))
    {
        for (const auto& [virtualPath, node] : robloxPlatform->virtualPathsToSourceNodes)
        {
            if (!node->isScript())
                continue;
            if (auto realPath = robloxPlatform->getRealPathFromSourceNode(node))
                if (auto mapped = mapMovedUri(*realPath, operation))
                    operation.movedInstanceModules.push_back({virtualPath, *realPath, *mapped});
        }
    }

    return operation;
}

namespace
{
struct PendingVariableRename
{
    Luau::AstExpr* argument;
    std::string oldName;
    std::string newName;
};

struct ModuleUpdateContext
{
    WorkspaceFolder& workspace;
    RobloxPlatform* robloxPlatform;
    const RenameOperation& operation;
    const std::unordered_map<std::string, Uri>& movedVirtualPaths;
    RequireUpdateResult& result;

    Luau::ModuleName moduleName;
    Uri fromUriOld;
    Uri fromUriNew;
    bool fromMoved = false;
    const Luau::SourceModule* sourceModule = nullptr;
    const TextDocument* textDocument = nullptr;

    // Lazily computed state for adding missing service imports
    std::optional<AutoImports::RobloxFindImportsVisitor> importsVisitor = std::nullopt;
    std::unordered_set<std::string> addedServices{};

    // Variables named after a module whose name changed, renamed after all requires are rewritten
    bool renameVariables = false;
    std::vector<PendingVariableRename> pendingVariableRenames{};
    std::vector<Luau::Location> rewrittenArgumentLocations{};
};
} // namespace

static void recordSkipped(ModuleUpdateContext& ctx, const Luau::AstExpr* argument, const std::string& reason)
{
    ctx.result.skipped.push_back(
        ctx.fromUriNew.fsPath() + ":" + std::to_string(argument->location.begin.line + 1) + " - " + reason);
}

/// Computes the replacement text for a string require argument (excluding quotes)
static std::optional<std::string> computeNewStringRequire(ModuleUpdateContext& ctx, const Luau::AstExpr* argument,
    const std::string& originalString, const std::optional<Uri>& targetUriOld, const std::optional<Uri>& targetUriNew,
    const std::optional<std::string>& targetOldVirtual)
{
    // Roblox "@game/..." style requires are DataModel-based: recompute from the new virtual path
    if (ctx.robloxPlatform && Luau::startsWith(originalString, "@game/"))
    {
        if (!targetUriNew)
            return std::nullopt; // target did not move: absolute DataModel path is unchanged
        if (auto newVirtual = ctx.robloxPlatform->resolveToVirtualPath(*targetUriNew))
        {
            std::string path = *newVirtual;
            if (Luau::startsWith(path, "game/"))
                path = path.substr(5);
            return "@game/" + path;
        }
        ctx.result.needsSourcemapUpdate = true;
        recordSkipped(ctx, argument, "sourcemap does not yet contain the new location");
        return std::nullopt;
    }

    std::optional<Uri> to = targetUriNew ? targetUriNew : targetUriOld;
    if (!to && targetOldVirtual)
        to = ctx.workspace.fileResolver.getUri(*targetOldVirtual);
    if (!to)
    {
        recordSkipped(ctx, argument, "could not resolve require target");
        return std::nullopt;
    }

    auto style = Luau::startsWith(originalString, "@") && !Luau::startsWith(originalString, "@self")
                     ? ImportRequireStyle::AlwaysAbsolute
                     : ImportRequireStyle::AlwaysRelative;
    auto aliases = ctx.workspace.fileResolver.getConfig(ctx.moduleName, ctx.workspace.limits).aliases;
    return AutoImports::computeRequirePath(ctx.fromUriNew, *to, aliases, style).first;
}

/// Computes the replacement text for an instance require argument, possibly adding a missing
/// service import edit
static std::optional<std::string> computeNewInstanceRequire(
    ModuleUpdateContext& ctx, Luau::AstExpr* argument, const std::optional<Uri>& targetUriNew, const std::optional<std::string>& targetOldVirtual)
{
    if (!ctx.robloxPlatform)
        return std::nullopt;

    auto root = analyzeInstanceChain(argument);
    if (!root)
    {
        recordSkipped(ctx, argument, "require expression is not a plain instance path (e.g. uses WaitForChild), not rewritten");
        return std::nullopt;
    }

    // The new DataModel path of the target
    std::optional<std::string> targetVirtualNew;
    if (targetUriNew)
    {
        targetVirtualNew = ctx.robloxPlatform->resolveToVirtualPath(*targetUriNew);
        if (!targetVirtualNew)
        {
            ctx.result.needsSourcemapUpdate = true;
            recordSkipped(ctx, argument, "sourcemap does not yet contain the new location");
            return std::nullopt;
        }
    }
    else
        targetVirtualNew = targetOldVirtual; // target did not move (but the requiring file did)

    if (!targetVirtualNew)
    {
        recordSkipped(ctx, argument, "could not resolve require target");
        return std::nullopt;
    }

    if (*root == InstanceChainRoot::Script)
    {
        // Relative require: needs the DataModel path of the requiring file
        std::optional<std::string> fromVirtualNew;
        if (ctx.fromMoved)
        {
            fromVirtualNew = ctx.robloxPlatform->resolveToVirtualPath(ctx.fromUriNew);
            if (!fromVirtualNew)
            {
                ctx.result.needsSourcemapUpdate = true;
                recordSkipped(ctx, argument, "sourcemap does not yet contain the new location");
                return std::nullopt;
            }
        }
        else if (ctx.robloxPlatform->isVirtualPath(ctx.moduleName))
            fromVirtualNew = ctx.moduleName;

        if (!fromVirtualNew)
        {
            recordSkipped(ctx, argument, "requiring file is not present in the sourcemap");
            return std::nullopt;
        }

        // HACK: using Uri's purely to access lexicallyRelative (mirrors InstanceRequireAutoImporter)
        auto relativePath = "./" + Uri::file(*targetVirtualNew).lexicallyRelative(Uri::file(*fromVirtualNew));
        return convertToScriptPath(relativePath);
    }

    if (*root == InstanceChainRoot::Game)
        return convertToScriptPath(*targetVirtualNew);

    auto optimised = AutoImports::optimiseAbsoluteRequire(*targetVirtualNew);
    if (optimised == *targetVirtualNew && Luau::startsWith(optimised, "game/"))
    {
        recordSkipped(ctx, argument, "new location is not under a service");
        return std::nullopt;
    }
    auto service = optimised.substr(0, optimised.find('/'));

    if (*root == InstanceChainRoot::GetServiceCall)
    {
        auto rest = optimised.substr(service.size());
        if (!rest.empty())
            rest = "." + convertToScriptPath(rest.substr(1));
        return "game:GetService(\"" + service + "\")" + rest;
    }

    // ServiceLocal: the chain is rooted at a `local X = game:GetService("X")` binding.
    // If the target's service changed and no matching local exists, add a service import
    if (!ctx.importsVisitor)
    {
        ctx.importsVisitor.emplace();
        ctx.importsVisitor->visit(ctx.sourceModule->root);
    }
    if (!contains(ctx.importsVisitor->serviceLineMap, service) && !ctx.addedServices.count(service))
    {
        auto config = ctx.workspace.client->getConfiguration(ctx.workspace.rootUri);
        auto sections = AutoImports::detectImportSections(*ctx.textDocument, config.completion.imports.sections);
        auto hotCommentsLineNumber = AutoImports::computeHotCommentsLineNumber(*ctx.sourceModule);
        auto lineNumber = AutoImports::sectionClamp(sections.services,
            ctx.importsVisitor->findBestLineForService(service, AutoImports::sectionMinimum(sections.services, hotCommentsLineNumber)));
        ctx.result.edit.changes[ctx.fromUriNew].push_back(
            AutoImports::createServiceTextEdit(service, lineNumber, /* appendNewline= */ false, config.completion.imports.useConst));
        ctx.addedServices.insert(service);
    }

    return convertToScriptPath(optimised);
}

/// Renames local variables that were named after a module whose require path rewrite changed
/// the module's name, keeping the variable and the module in sync (e.g. `local Module =
/// require(...Module)` follows a rename of Module). Custom variable names are left alone
static void renamePendingVariables(ModuleUpdateContext& ctx)
{
    std::unordered_set<std::string> introducedNames{};
    for (const auto& pending : ctx.pendingVariableRenames)
    {
        FindRequireBindingVisitor bindingVisitor{pending.argument};
        ctx.sourceModule->root->visit(&bindingVisitor);
        if (!bindingVisitor.binding || bindingVisitor.binding->name.value != pending.oldName)
            continue;

        VariableNameSurvey survey{pending.oldName, pending.newName};
        ctx.sourceModule->root->visit(&survey);
        if (survey.oldNameBindings > 1)
        {
            recordSkipped(ctx, pending.argument, "variable '" + pending.oldName + "' not renamed: multiple bindings share this name");
            continue;
        }
        if (survey.newNameInUse || introducedNames.count(pending.newName) != 0)
        {
            recordSkipped(ctx, pending.argument, "variable '" + pending.oldName + "' not renamed: '" + pending.newName + "' is already in use");
            continue;
        }

        for (const auto& location : findSymbolReferences(*ctx.sourceModule, Luau::Symbol(bindingVisitor.binding)))
        {
            // References inside a rewritten require argument are already covered by the new
            // path text and must not produce overlapping edits
            bool insideRewrite = std::any_of(ctx.rewrittenArgumentLocations.begin(), ctx.rewrittenArgumentLocations.end(),
                [&location](const Luau::Location& argumentLocation)
                {
                    return argumentLocation.contains(location.begin);
                });
            if (insideRewrite)
                continue;
            auto range = lsp::Range{ctx.textDocument->convertPosition(location.begin), ctx.textDocument->convertPosition(location.end)};
            ctx.result.edit.changes[ctx.fromUriNew].push_back(lsp::TextEdit{range, pending.newName});
        }
        ctx.result.renamedVariableCount++;
        introducedNames.insert(pending.newName);
    }
}

static void processModule(ModuleUpdateContext& ctx, const std::optional<std::vector<Uri>>& limitToFiles)
{
    if (limitToFiles)
    {
        bool included = false;
        for (const auto& file : *limitToFiles)
            if (file == ctx.fromUriNew || file == ctx.fromUriOld)
                included = true;
        if (!included)
            return;
    }

    FindRequireCallsVisitor visitor;
    ctx.sourceModule->root->visit(&visitor);
    if (visitor.requireArguments.empty())
        return;

    // The require tracer handles local aliasing (e.g. `local RS = game:GetService(...)`) when
    // resolving instance require chains
    auto traced = Luau::traceRequires(&ctx.workspace.fileResolver, ctx.sourceModule->root, ctx.moduleName, ctx.workspace.limits);

    for (auto* argument : visitor.requireArguments)
    {
        std::optional<std::string> stringContents = std::nullopt;
        if (auto* str = argument->as<Luau::AstExprConstantString>())
            stringContents = std::string(str->value.data, str->value.size);

        // Resolve the require's current target
        std::optional<Uri> targetUriOld = std::nullopt;
        std::optional<std::string> targetOldVirtual = std::nullopt;
        if (const auto* info = traced.exprs.find(argument))
        {
            if (ctx.robloxPlatform && ctx.robloxPlatform->isVirtualPath(info->name))
                targetOldVirtual = info->name;
            else if (!info->name.empty())
                targetUriOld = ctx.workspace.fileResolver.getUri(info->name);
        }

        // Map the target onto the rename
        std::optional<Uri> targetUriNew = std::nullopt;
        if (targetOldVirtual)
        {
            if (auto it = ctx.movedVirtualPaths.find(*targetOldVirtual); it != ctx.movedVirtualPaths.end())
                targetUriNew = it->second;
        }
        if (!targetUriNew && targetUriOld)
            targetUriNew = mapMovedUri(*targetUriOld, ctx.operation);
        if (!targetUriNew && stringContents && !Luau::startsWith(*stringContents, "@game/"))
        {
            // Textual fallback: works even when Luau's resolution failed because the target
            // has already been moved on disk
            if (auto base = resolveStringRequireBase(ctx.workspace, ctx.moduleName, ctx.fromUriOld, *stringContents))
            {
                if (auto mapped = mapMovedBaseUri(*base, ctx.operation))
                {
                    targetUriOld = mapped->first;
                    targetUriNew = mapped->second;
                }
            }
        }

        bool targetMoved = targetUriNew.has_value();
        if (!targetMoved && !ctx.fromMoved)
            continue;
        // Unresolvable (e.g. dynamic) requires are only relevant when they might target the
        // moved module - since resolution failed, we cannot know, so leave them untouched
        if (!targetMoved && !targetUriOld && !targetOldVirtual)
            continue;

        auto range = lsp::Range{
            ctx.textDocument->convertPosition(argument->location.begin), ctx.textDocument->convertPosition(argument->location.end)};
        auto originalText = ctx.textDocument->getText(range);

        std::optional<std::string> newText = std::nullopt;
        if (stringContents)
        {
            char quote = originalText.empty() ? '"' : originalText[0];
            if (quote != '"' && quote != '\'')
            {
                if (targetMoved)
                    recordSkipped(ctx, argument, "unsupported string literal style, not rewritten");
                continue;
            }
            if (auto newPath = computeNewStringRequire(ctx, argument, *stringContents, targetUriOld, targetUriNew, targetOldVirtual))
                newText = quote + *newPath + quote;
        }
        else
        {
            newText = computeNewInstanceRequire(ctx, argument, targetUriNew, targetOldVirtual);
        }

        if (!newText || *newText == originalText)
            continue;

        ctx.result.edit.changes[ctx.fromUriNew].push_back(lsp::TextEdit{range, *newText});
        ctx.result.updatedRequireCount++;
        ctx.rewrittenArgumentLocations.push_back(argument->location);

        if (ctx.renameVariables)
        {
            std::optional<std::string> oldName;
            std::optional<std::string> newName;
            if (stringContents)
            {
                oldName = stringRequireLeafName(*stringContents);
                newName = stringRequireLeafName(newText->substr(1, newText->size() - 2));
            }
            else
            {
                oldName = requireArgumentLeafName(argument);
                newName = instanceRequireLeafName(*newText);
            }
            if (oldName && newName && *oldName != *newName && Luau::isIdentifier(*newName) && !isReservedKeyword(*newName))
                ctx.pendingVariableRenames.push_back({argument, std::move(*oldName), std::move(*newName)});
        }
    }

    renamePendingVariables(ctx);
}

RequireUpdateResult computeRequireUpdates(
    WorkspaceFolder& workspace, const RenameOperation& operation, const std::optional<std::vector<Uri>>& limitToFiles)
{
    RequireUpdateResult result{};

    auto* robloxPlatform = dynamic_cast<RobloxPlatform*>(workspace.platform.get());
    bool renameVariables = workspace.client->getConfiguration(workspace.rootUri).fileOperations.renameVariablesOnRequireUpdate;

    std::unordered_map<std::string, Uri> movedVirtualPaths{};
    for (const auto& moved : operation.movedInstanceModules)
        movedVirtualPaths.emplace(moved.oldVirtualPath, moved.newRealUri);

    std::vector<Luau::ModuleName> moduleNames{};
    for (const auto& [name, _] : workspace.frontend.sourceNodes)
        moduleNames.push_back(name);

    // A stale source node can linger under a module name that resolves to the same physical
    // file as a genuinely moved module (e.g. renaming A -> B and later back to A: the
    // pre-rename node for A is never evicted). Process moved modules first so the
    // physical-file dedup below cannot let a stale twin starve them of their own updates;
    // among moved modules, prefer captured virtual paths (they resolve relative requires)
    auto moduleRank = [&](const Luau::ModuleName& name)
    {
        if (movedVirtualPaths.count(name) != 0)
            return 0;
        auto uri = workspace.fileResolver.getUri(name);
        if (uri.scheme == "file" && mapMovedUri(uri, operation).has_value())
            return 1;
        return 2;
    };
    std::stable_sort(moduleNames.begin(), moduleNames.end(),
        [&](const Luau::ModuleName& a, const Luau::ModuleName& b)
        {
            return moduleRank(a) < moduleRank(b);
        });

    // A moved file can be indexed under both its stale (pre-move) and new module name -
    // process each physical file only once so the edit does not contain overlapping ranges
    std::unordered_set<Uri, UriHash> processedFiles{};

    for (const auto& moduleName : moduleNames)
    {
        auto fromUriOld = workspace.fileResolver.getUri(moduleName);

        // A moved module may still be indexed under its old virtual path, which no longer
        // resolves against the regenerated sourcemap - fall back to the location captured
        // at rename time so its own requires are updated too
        if (robloxPlatform && robloxPlatform->isVirtualPath(moduleName))
            for (const auto& moved : operation.movedInstanceModules)
                if (moved.oldVirtualPath == moduleName)
                    fromUriOld = moved.oldRealUri;

        if (fromUriOld.scheme != "file")
            continue;

        auto fromUriNew = mapMovedUri(fromUriOld, operation).value_or(fromUriOld);
        if (!processedFiles.insert(fromUriNew).second)
            continue;

        // Locate the document text (needed for UTF-16 ranges and existing require text)
        const TextDocument* textDocument = workspace.fileResolver.getTextDocument(fromUriNew);
        if (!textDocument)
            textDocument = workspace.fileResolver.getTextDocument(fromUriOld);
        std::optional<TextDocument> temporaryDocument = std::nullopt;
        if (!textDocument)
        {
            auto contents = Luau::FileUtils::readFile(fromUriNew.fsPath());
            if (!contents)
                contents = Luau::FileUtils::readFile(fromUriOld.fsPath());
            if (!contents)
                continue;
            temporaryDocument.emplace(fromUriNew, "luau", 0, *contents);
            textDocument = &*temporaryDocument;
        }

        auto text = textDocument->getText();
        if (text.find("require") == std::string::npos)
            continue;
        auto sourceModule = parseCurrentText(workspace, moduleName, text);
        if (!sourceModule)
            continue;

        ModuleUpdateContext ctx{
            workspace,
            robloxPlatform,
            operation,
            movedVirtualPaths,
            result,
            moduleName,
            fromUriOld,
            fromUriNew,
            fromUriNew != fromUriOld,
            &*sourceModule,
            textDocument,
        };
        ctx.renameVariables = renameVariables;
        processModule(ctx, limitToFiles);
    }

    return result;
}

} // namespace Luau::LanguageServer::RequireUpdates

// WorkspaceFolder-level flow: prompting, applying, and deferring require updates.
// Implemented here (rather than Workspace.cpp) to keep the upstream file conflict-free

using namespace Luau::LanguageServer;

void WorkspaceFolder::onDidRenameFiles(const std::vector<lsp::FileRename>& renames)
{
    auto config = client->getConfiguration(rootUri);
    auto mode = client->requireUpdateModeOverride.value_or(config.fileOperations.updateRequiresOnMove);
    if (mode == RequireUpdateMode::Never)
        return;

    for (const auto& rename : renames)
    {
        auto operation = RequireUpdates::captureRenameOperation(*this, Uri::parse(rename.oldUri), Uri::parse(rename.newUri));
        applyOrPromptRequireUpdates(std::move(operation), /* allowDefer= */ true);
    }
}

void WorkspaceFolder::processPendingRequireUpdates()
{
    if (pendingRequireUpdates.empty())
        return;

    auto pending = std::move(pendingRequireUpdates);
    pendingRequireUpdates.clear();
    for (auto& operation : pending)
        applyOrPromptRequireUpdates(std::move(operation), /* allowDefer= */ false);
}

void WorkspaceFolder::applyOrPromptRequireUpdates(RequireUpdates::RenameOperation operation, bool allowDefer)
{
    auto mode = client->requireUpdateModeOverride.value_or(client->getConfiguration(rootUri).fileOperations.updateRequiresOnMove);
    if (mode == RequireUpdateMode::Never)
        return;

    auto result = RequireUpdates::computeRequireUpdates(*this, operation);

    if (result.needsSourcemapUpdate && allowDefer)
    {
        client->sendLogMessage(lsp::MessageType::Info,
            "Require updates for '" + operation.newUri.fsPath() + "' are waiting for the sourcemap to be regenerated");
        pendingRequireUpdates.push_back(std::move(operation));
        return;
    }

    for (const auto& skipped : result.skipped)
        client->sendLogMessage(lsp::MessageType::Warning, "Skipped during require update: " + skipped);

    if (result.updatedRequireCount == 0)
        return;

    bool supportsApplyEdit = client->capabilities.workspace && client->capabilities.workspace->applyEdit;
    if (!supportsApplyEdit)
    {
        // Conservative fallback: never rewrite without a way for the user to see/apply the edit.
        // Clients can pull the edit through the `luau-lsp/updateRequiresForRename` request instead
        client->sendWindowMessage(lsp::MessageType::Info,
            std::to_string(result.updatedRequireCount) + " require(s) reference the moved file '" + operation.newUri.fsPath() +
                "'. Use the 'luau-lsp/updateRequiresForRename' request to compute the updates.");
        return;
    }

    if (mode == RequireUpdateMode::Always)
    {
        client->applyEdit({"Update requires on file move", result.edit});
        return;
    }

    auto renames = result.renamedVariableCount != 0 ? " and " + std::to_string(result.renamedVariableCount) + " variable name(s)" : "";
    auto message = "Update " + std::to_string(result.updatedRequireCount) + " require(s)" + renames + " in " +
                   std::to_string(result.edit.changes.size()) + " file(s) for '" + operation.newUri.filename() + "'?";
    std::vector<lsp::MessageActionItem> actions{{"Update requires"}, {"Skip"}, {"Always update"}, {"Never update"}};
    client->showMessageRequest(lsp::MessageType::Info, message, actions,
        [this, edit = std::move(result.edit)](const json_rpc::JsonRpcMessage& response)
        {
            // A null result means the prompt was dismissed: do not update
            if (!response.result || response.result->is_null())
                return;
            lsp::MessageActionItem item = *response.result;
            if (item.title == "Update requires")
                client->applyEdit({"Update requires on file move", edit});
            else if (item.title == "Always update")
            {
                client->requireUpdateModeOverride = RequireUpdateMode::Always;
                // Persist the choice where the client supports it (handled by the VSCode extension)
                client->sendNotification(
                    "$/command", std::make_optional<json>({{"command", "luau-lsp.setUpdateRequiresOnMove"}, {"data", "always"}}));
                client->applyEdit({"Update requires on file move", edit});
            }
            else if (item.title == "Never update")
            {
                client->requireUpdateModeOverride = RequireUpdateMode::Never;
                client->sendNotification(
                    "$/command", std::make_optional<json>({{"command", "luau-lsp.setUpdateRequiresOnMove"}, {"data", "never"}}));
            }
        });
}
