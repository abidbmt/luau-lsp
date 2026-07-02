#include "doctest.h"
#include "Fixture.h"
#include "RobloxTestConstants.h"
#include "LSP/AutoImportRules.hpp"

using namespace Luau::LanguageServer::AutoImports;

static std::optional<lsp::CompletionItem> getItem(const std::vector<lsp::CompletionItem>& items, const std::string& label)
{
    for (const auto& item : items)
        if (item.label == label)
            return item;

    return std::nullopt;
}

static std::vector<lsp::CompletionItem> requestCompletion(Fixture* fixture, const Uri& uri, const lsp::Position& position)
{
    lsp::CompletionParams params;
    params.textDocument = lsp::TextDocumentIdentifier{uri};
    params.position = position;
    return fixture->workspace.completion(params, nullptr);
}

// A ModuleScript intended to be client-only (e.g. required only by RunContext = Client scripts)
// living inside ReplicatedStorage, where the default heuristics classify everything as Shared
static const std::string SOURCEMAP_WITH_CLIENT_MODULE_IN_REPLICATED_STORAGE = R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {
                    "name": "ClientStuff",
                    "className": "Folder",
                    "children": [
                        {"name": "ClientOnlyModule", "className": "ModuleScript", "filePaths": ["shared/ClientStuff/ClientOnlyModule.luau"]}
                    ]
                },
                {"name": "SharedModule", "className": "ModuleScript", "filePaths": ["shared/SharedModule.luau"]}
            ]
        },
        {
            "name": "ServerScriptService",
            "className": "ServerScriptService",
            "children": [{"name": "ServerModule", "className": "ModuleScript", "filePaths": ["server/ServerModule.luau"]}]
        },
        {
            "name": "StarterPlayer",
            "className": "StarterPlayer",
            "children": [
                {
                    "name": "StarterPlayerScripts",
                    "className": "StarterPlayerScripts",
                    "children": [{"name": "ClientModule", "className": "ModuleScript", "filePaths": ["client/ClientModule.luau"]}]
                }
            ]
        }
    ]
}
)";

// A feature-based layout where each feature has private modules inside an Internal directory
static const std::string SOURCEMAP_FOR_FEATURE_VISIBILITY = R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {
                    "name": "Features",
                    "className": "Folder",
                    "children": [
                        {
                            "name": "FeatureA",
                            "className": "Folder",
                            "children": [
                                {"name": "SystemA", "className": "ModuleScript", "filePaths": ["src/Features/FeatureA/SystemA.luau"]},
                                {
                                    "name": "Internal",
                                    "className": "Folder",
                                    "children": [
                                        {"name": "InternalUtilA", "className": "ModuleScript", "filePaths": ["src/Features/FeatureA/Internal/InternalUtilA.luau"]}
                                    ]
                                },
                                {
                                    "name": "Server",
                                    "className": "Folder",
                                    "children": [
                                        {"name": "ServerSystemA", "className": "ModuleScript", "filePaths": ["src/Features/FeatureA/Server/ServerSystemA.luau"]},
                                        {
                                            "name": "Internal",
                                            "className": "Folder",
                                            "children": [
                                                {"name": "ServerInternalUtilA", "className": "ModuleScript", "filePaths": ["src/Features/FeatureA/Server/Internal/ServerInternalUtilA.luau"]}
                                            ]
                                        }
                                    ]
                                },
                                {
                                    "name": "Shared",
                                    "className": "Folder",
                                    "children": [
                                        {"name": "SharedSystemA", "className": "ModuleScript", "filePaths": ["src/Features/FeatureA/Shared/SharedSystemA.luau"]}
                                    ]
                                }
                            ]
                        },
                        {
                            "name": "FeatureB",
                            "className": "Folder",
                            "children": [
                                {"name": "SystemB", "className": "ModuleScript", "filePaths": ["src/Features/FeatureB/SystemB.luau"]},
                                {
                                    "name": "Internal",
                                    "className": "Folder",
                                    "children": [
                                        {"name": "InternalUtilB", "className": "ModuleScript", "filePaths": ["src/Features/FeatureB/Internal/InternalUtilB.luau"]}
                                    ]
                                }
                            ]
                        }
                    ]
                },
                {"name": "Outside", "className": "ModuleScript", "filePaths": ["src/Outside.luau"]}
            ]
        }
    ]
}
)";

TEST_SUITE_BEGIN("AutoImportRules");

TEST_CASE("parse_boundary_context")
{
    CHECK_EQ(parseBoundaryContext("client"), std::optional(BoundaryContext::Client));
    CHECK_EQ(parseBoundaryContext("server"), std::optional(BoundaryContext::Server));
    CHECK_EQ(parseBoundaryContext("shared"), std::optional(BoundaryContext::Shared));
    CHECK_EQ(parseBoundaryContext("none"), std::optional(BoundaryContext::None));
    CHECK_EQ(parseBoundaryContext("Sever"), std::nullopt);
    CHECK_EQ(parseBoundaryContext(""), std::nullopt);
}

TEST_CASE("boundary_classification_last_matching_rule_wins")
{
    ClientCompletionImportsConfiguration config;
    config.boundaries.rules = {
        {"src/**", "shared"},
        {"src/Client/**", "client"},
    };
    ImportRuleSet rules(config);
    CHECK(rules.warnings.empty());

    CHECK_EQ(rules.classifyBoundary({"src/Client/Controller.luau"}), std::optional(BoundaryContext::Client));
    CHECK_EQ(rules.classifyBoundary({"src/Util.luau"}), std::optional(BoundaryContext::Shared));
    CHECK_EQ(rules.classifyBoundary({"other/File.luau"}), std::nullopt);
}

TEST_CASE("boundary_classification_matches_datamodel_paths")
{
    ClientCompletionImportsConfiguration config;
    config.boundaries.rules = {
        {"ReplicatedStorage/ClientStuff/**", "client"},
        {"game/ServerScriptService/**", "server"},
    };
    ImportRuleSet rules(config);

    // DataModel paths are matched both with and without the "game/" root prefix
    CHECK_EQ(rules.classifyBoundary({"", "game/ReplicatedStorage/ClientStuff/Module"}), std::optional(BoundaryContext::Client));
    CHECK_EQ(rules.classifyBoundary({"", "game/ServerScriptService/Module"}), std::optional(BoundaryContext::Server));
    CHECK_EQ(rules.classifyBoundary({"", "game/ReplicatedStorage/Other"}), std::nullopt);
}

TEST_CASE("permissive_matrix_when_boundaries_unconfigured")
{
    ImportRuleSet rules(ClientCompletionImportsConfiguration{});
    CHECK_FALSE(rules.active());

    // Mirrors isScriptContextCompatible: allowed if either side is Shared
    CHECK(rules.isImportAllowed(BoundaryContext::Client, BoundaryContext::Client));
    CHECK(rules.isImportAllowed(BoundaryContext::Client, BoundaryContext::Shared));
    CHECK_FALSE(rules.isImportAllowed(BoundaryContext::Client, BoundaryContext::Server));
    CHECK(rules.isImportAllowed(BoundaryContext::Server, BoundaryContext::Server));
    CHECK(rules.isImportAllowed(BoundaryContext::Server, BoundaryContext::Shared));
    CHECK_FALSE(rules.isImportAllowed(BoundaryContext::Server, BoundaryContext::Client));
    CHECK(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Shared));
    CHECK(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Client));
    CHECK(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Server));
}

TEST_CASE("strict_matrix_when_boundaries_configured")
{
    ClientCompletionImportsConfiguration config;
    config.boundaries.rules = {{"src/**", "shared"}};
    ImportRuleSet rules(config);
    CHECK(rules.active());

    CHECK(rules.isImportAllowed(BoundaryContext::Client, BoundaryContext::Client));
    CHECK(rules.isImportAllowed(BoundaryContext::Client, BoundaryContext::Shared));
    CHECK_FALSE(rules.isImportAllowed(BoundaryContext::Client, BoundaryContext::Server));
    CHECK(rules.isImportAllowed(BoundaryContext::Server, BoundaryContext::Server));
    CHECK(rules.isImportAllowed(BoundaryContext::Server, BoundaryContext::Shared));
    CHECK_FALSE(rules.isImportAllowed(BoundaryContext::Server, BoundaryContext::Client));
    CHECK(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Shared));
    CHECK_FALSE(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Client));
    CHECK_FALSE(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Server));

    // None (unmatched, no heuristic) stays unrestricted in both directions
    CHECK(rules.isImportAllowed(BoundaryContext::None, BoundaryContext::Server));
    CHECK(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::None));
}

TEST_CASE("allowed_imports_overrides_matrix")
{
    ClientCompletionImportsConfiguration config;
    config.boundaries.allowedImports = {{"shared", {"shared"}}};
    ImportRuleSet rules(config);
    CHECK(rules.warnings.empty());
    // Overriding the matrix alone (without rules) activates boundary checking against heuristics
    CHECK(rules.active());

    CHECK(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Shared));
    CHECK_FALSE(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Server));
    CHECK_FALSE(rules.isImportAllowed(BoundaryContext::Shared, BoundaryContext::Client));
    // Other rows keep the (permissive) defaults
    CHECK(rules.isImportAllowed(BoundaryContext::Client, BoundaryContext::Shared));
    CHECK(rules.isImportAllowed(BoundaryContext::Server, BoundaryContext::Shared));
}

TEST_CASE("invalid_boundary_configuration_produces_warnings")
{
    ClientCompletionImportsConfiguration config;
    config.boundaries.rules = {
        {"", "client"},
        {"src/**", "sever"},
        {"src/Client/**", "client"},
    };
    config.boundaries.allowedImports = {{"schared", {"shared"}}, {"client", {"backend"}}};
    ImportRuleSet rules(config);

    REQUIRE_EQ(rules.warnings.size(), 4);
    // The valid rule still applies
    CHECK_EQ(rules.classifyBoundary({"src/Client/Foo.luau"}), std::optional(BoundaryContext::Client));
}

TEST_CASE("unmatched_paths_fall_back_to_heuristic_contexts")
{
    ClientCompletionImportsConfiguration config;
    config.boundaries.rules = {{"src/Server/**", "server"}};
    ImportRuleSet rules(config);

    // Unmatched from and target: heuristics substituted
    CHECK(rules.isBoundaryImportAllowed({"other/a.luau"}, BoundaryContext::Shared, {"other/b.luau"}, BoundaryContext::Shared));
    CHECK_FALSE(rules.isBoundaryImportAllowed({"other/a.luau"}, BoundaryContext::Shared, {"src/Server/b.luau"}, BoundaryContext::Shared));
    // Explicit rule overrides the heuristic
    CHECK(rules.isBoundaryImportAllowed({"other/a.luau"}, BoundaryContext::Server, {"src/Server/b.luau"}, BoundaryContext::Shared));
}

TEST_CASE("global_visibility_rule_restricts_modules")
{
    ClientCompletionImportsConfiguration config;
    config.visibilityRules = {{"", "**/*.generated.luau", "tools/**"}};
    ImportRuleSet rules(config);
    CHECK(rules.warnings.empty());
    CHECK(rules.active());

    CHECK(rules.isVisibleFrom({"tools/build.luau"}, {"src/Types.generated.luau"}));
    CHECK_FALSE(rules.isVisibleFrom({"src/Main.luau"}, {"src/Types.generated.luau"}));
    // Unrestricted modules are visible from anywhere
    CHECK(rules.isVisibleFrom({"src/Main.luau"}, {"src/Other.luau"}));
}

TEST_CASE("scoped_visibility_rule_restricts_to_owning_scope")
{
    ClientCompletionImportsConfiguration config;
    config.visibilityRules = {{"src/Features/*", "**/Internal/**", ""}};
    ImportRuleSet rules(config);
    CHECK(rules.warnings.empty());

    // Same feature: visible
    CHECK(rules.isVisibleFrom({"src/Features/FeatureA/System.luau"}, {"src/Features/FeatureA/Internal/Util.luau"}));
    CHECK(rules.isVisibleFrom({"src/Features/FeatureA/Server/Deep/File.luau"}, {"src/Features/FeatureA/Internal/Util.luau"}));
    // Different feature: hidden
    CHECK_FALSE(rules.isVisibleFrom({"src/Features/FeatureB/System.luau"}, {"src/Features/FeatureA/Internal/Util.luau"}));
    // Outside all features: hidden
    CHECK_FALSE(rules.isVisibleFrom({"src/Main.luau"}, {"src/Features/FeatureA/Internal/Util.luau"}));
    // Non-internal modules in a feature remain visible from anywhere
    CHECK(rules.isVisibleFrom({"src/Main.luau"}, {"src/Features/FeatureA/System.luau"}));
}

TEST_CASE("scoped_visibility_rule_matches_datamodel_paths")
{
    ClientCompletionImportsConfiguration config;
    config.visibilityRules = {{"ReplicatedStorage/Features/*", "**/Internal/**", ""}};
    ImportRuleSet rules(config);

    CHECK(rules.isVisibleFrom({"", "game/ReplicatedStorage/Features/FeatureA/System"},
        {"", "game/ReplicatedStorage/Features/FeatureA/Internal/Util"}));
    CHECK_FALSE(rules.isVisibleFrom({"", "game/ReplicatedStorage/Features/FeatureB/System"},
        {"", "game/ReplicatedStorage/Features/FeatureA/Internal/Util"}));
}

TEST_CASE("multiple_visibility_rules_all_must_pass")
{
    ClientCompletionImportsConfiguration config;
    config.visibilityRules = {
        {"", "src/Secrets/**", "src/Admin/**"},
        {"", "src/Secrets/Keys/**", "src/Admin/Keys/**"},
    };
    ImportRuleSet rules(config);

    CHECK(rules.isVisibleFrom({"src/Admin/Panel.luau"}, {"src/Secrets/Config.luau"}));
    // Matches both rules: must satisfy both
    CHECK_FALSE(rules.isVisibleFrom({"src/Admin/Panel.luau"}, {"src/Secrets/Keys/ApiKey.luau"}));
    CHECK(rules.isVisibleFrom({"src/Admin/Keys/Manager.luau"}, {"src/Secrets/Keys/ApiKey.luau"}));
}

TEST_CASE("invalid_visibility_rules_produce_warnings")
{
    ClientCompletionImportsConfiguration config;
    config.visibilityRules = {
        {"", "", "tools/**"},        // missing modules
        {"", "**/Internal/**", ""},  // missing scope and visibleFrom
    };
    ImportRuleSet rules(config);
    CHECK_EQ(rules.warnings.size(), 2);
    CHECK_FALSE(rules.active());
}

TEST_CASE("visibility_reinclusion_does_not_bypass_boundaries")
{
    ClientCompletionImportsConfiguration config;
    config.boundaries.rules = {
        {"src/Features/*/Server/**", "server"},
        {"src/Features/*/Shared/**", "shared"},
    };
    config.visibilityRules = {{"src/Features/*", "**/Internal/**", ""}};
    ImportRuleSet rules(config);

    // Same feature, but shared code must not import server-only internals
    CHECK_FALSE(rules.isAutoImportAllowed({"src/Features/FeatureA/Shared/System.luau"}, BoundaryContext::None,
        {"src/Features/FeatureA/Server/Internal/Util.luau"}, BoundaryContext::None));
    // Server code in the same feature may
    CHECK(rules.isAutoImportAllowed({"src/Features/FeatureA/Server/System.luau"}, BoundaryContext::None,
        {"src/Features/FeatureA/Server/Internal/Util.luau"}, BoundaryContext::None));
}

// ===== Integration tests (completion-driven) =====

// Documents the current (upstream) behaviour that boundary rules exist to fix: without
// configuration, a client-only module in ReplicatedStorage is offered to server code
TEST_CASE_FIXTURE(Fixture, "boundaries_unconfigured_client_module_in_replicated_storage_offered_to_server")
{
    client->globalConfig.completion.imports.enabled = true;
    loadSourcemap(SOURCEMAP_WITH_CLIENT_MODULE_IN_REPLICATED_STORAGE);

    auto [source, marker] = sourceWithMarker(R"(|)");
    auto uri = newDocument("server/ServerModule.luau", source);
    auto result = requestCompletion(this, uri, marker);

    CHECK(getItem(result, "ClientOnlyModule"));
    CHECK(getItem(result, "SharedModule"));
}

TEST_CASE_FIXTURE(Fixture, "boundary_rules_hide_client_module_in_replicated_storage_from_server")
{
    client->globalConfig.completion.imports.enabled = true;
    client->globalConfig.completion.imports.boundaries.rules = {{"ReplicatedStorage/ClientStuff/**", "client"}};
    loadSourcemap(SOURCEMAP_WITH_CLIENT_MODULE_IN_REPLICATED_STORAGE);

    auto [source, marker] = sourceWithMarker(R"(|)");
    auto uri = newDocument("server/ServerModule.luau", source);
    auto result = requestCompletion(this, uri, marker);

    CHECK_FALSE(getItem(result, "ClientOnlyModule"));
    CHECK(getItem(result, "SharedModule"));
}

TEST_CASE_FIXTURE(Fixture, "boundary_rules_keep_client_module_visible_to_client")
{
    client->globalConfig.completion.imports.enabled = true;
    client->globalConfig.completion.imports.boundaries.rules = {{"ReplicatedStorage/ClientStuff/**", "client"}};
    loadSourcemap(SOURCEMAP_WITH_CLIENT_MODULE_IN_REPLICATED_STORAGE);

    auto [source, marker] = sourceWithMarker(R"(|)");
    auto uri = newDocument("client/ClientModule.luau", source);
    auto result = requestCompletion(this, uri, marker);

    CHECK(getItem(result, "ClientOnlyModule"));
    CHECK(getItem(result, "SharedModule"));
}

TEST_CASE_FIXTURE(Fixture, "boundary_rules_filesystem_paths_hide_server_and_client_from_shared")
{
    client->globalConfig.completion.imports.enabled = true;
    client->globalConfig.completion.imports.boundaries.rules = {
        {"server/**", "server"},
        {"client/**", "client"},
        {"shared/**", "shared"},
    };
    loadSourcemap(SOURCEMAP_FOR_SERVER_CLIENT_BOUNDARY_AUTO_IMPORTS);

    auto [source, marker] = sourceWithMarker(R"(|)");
    auto uri = newDocument("shared/SharedModule.luau", source);
    auto result = requestCompletion(this, uri, marker);

    CHECK_FALSE(getItem(result, "ServerModule"));
    CHECK_FALSE(getItem(result, "ServerStorageModule"));
    CHECK_FALSE(getItem(result, "ClientModule"));
    CHECK_FALSE(getItem(result, "GuiModule"));
}

TEST_CASE_FIXTURE(Fixture, "allowed_imports_without_rules_fixes_shared_leak")
{
    client->globalConfig.completion.imports.enabled = true;
    client->globalConfig.completion.imports.boundaries.allowedImports = {{"shared", {"shared"}}};
    loadSourcemap(SOURCEMAP_FOR_SERVER_CLIENT_BOUNDARY_AUTO_IMPORTS);

    auto [source, marker] = sourceWithMarker(R"(|)");
    auto uri = newDocument("shared/SharedModule.luau", source);
    auto result = requestCompletion(this, uri, marker);

    // Heuristic classification (ServerScriptService -> server, StarterPlayer -> client) combined
    // with the overridden shared row
    CHECK_FALSE(getItem(result, "ServerModule"));
    CHECK_FALSE(getItem(result, "ClientModule"));
}

TEST_CASE_FIXTURE(Fixture, "boundary_rules_apply_to_string_requires")
{
    client->globalConfig.completion.imports.enabled = true;
    client->globalConfig.completion.imports.stringRequires.enabled = true;
    client->globalConfig.completion.imports.boundaries.rules = {
        {"server/**", "server"},
        {"client/**", "client"},
        {"shared/**", "shared"},
    };
    loadSourcemap(SOURCEMAP_FOR_SERVER_CLIENT_BOUNDARY_AUTO_IMPORTS);

    auto [source, marker] = sourceWithMarker(R"(|)");
    auto uri = newDocument("shared/SharedModule.luau", source);
    auto result = requestCompletion(this, uri, marker);

    CHECK_FALSE(getItem(result, "ServerModule"));
    CHECK_FALSE(getItem(result, "ClientModule"));
}

TEST_CASE_FIXTURE(Fixture, "visibility_scoped_rule_internal_modules")
{
    client->globalConfig.completion.imports.enabled = true;
    client->globalConfig.completion.imports.visibilityRules = {{"src/Features/*", "**/Internal/**", ""}};
    loadSourcemap(SOURCEMAP_FOR_FEATURE_VISIBILITY);

    auto [source, marker] = sourceWithMarker(R"(|)");

    // Same feature: internal module suggested
    auto sameFeature = newDocument("src/Features/FeatureA/SystemA.luau", source);
    auto sameFeatureResult = requestCompletion(this, sameFeature, marker);
    CHECK(getItem(sameFeatureResult, "InternalUtilA"));
    CHECK_FALSE(getItem(sameFeatureResult, "InternalUtilB"));
    CHECK(getItem(sameFeatureResult, "SystemB"));

    // Outside all features: no internal modules suggested
    auto outside = newDocument("src/Outside.luau", source);
    auto outsideResult = requestCompletion(this, outside, marker);
    CHECK_FALSE(getItem(outsideResult, "InternalUtilA"));
    CHECK_FALSE(getItem(outsideResult, "InternalUtilB"));
    CHECK(getItem(outsideResult, "SystemA"));
}

TEST_CASE_FIXTURE(Fixture, "modules_named_internal_unaffected_without_rules")
{
    client->globalConfig.completion.imports.enabled = true;
    loadSourcemap(SOURCEMAP_FOR_FEATURE_VISIBILITY);

    auto [source, marker] = sourceWithMarker(R"(|)");
    auto uri = newDocument("src/Outside.luau", source);
    auto result = requestCompletion(this, uri, marker);

    CHECK(getItem(result, "InternalUtilA"));
    CHECK(getItem(result, "InternalUtilB"));
}

TEST_CASE_FIXTURE(Fixture, "visibility_rule_does_not_bypass_boundary_rules")
{
    client->globalConfig.completion.imports.enabled = true;
    client->globalConfig.completion.imports.boundaries.rules = {
        {"src/Features/*/Server/**", "server"},
        {"src/Features/*/Shared/**", "shared"},
    };
    client->globalConfig.completion.imports.visibilityRules = {{"src/Features/*", "**/Internal/**", ""}};
    loadSourcemap(SOURCEMAP_FOR_FEATURE_VISIBILITY);

    auto [source, marker] = sourceWithMarker(R"(|)");

    // Shared code in the same feature must not see the server-only internal module
    auto sharedFile = newDocument("src/Features/FeatureA/Shared/SharedSystemA.luau", source);
    auto sharedResult = requestCompletion(this, sharedFile, marker);
    CHECK_FALSE(getItem(sharedResult, "ServerInternalUtilA"));
    // The unrestricted same-feature internal module is still visible (heuristic Shared boundary)
    CHECK(getItem(sharedResult, "InternalUtilA"));

    // Server code in the same feature sees it
    auto serverFile = newDocument("src/Features/FeatureA/Server/ServerSystemA.luau", source);
    auto serverResult = requestCompletion(this, serverFile, marker);
    CHECK(getItem(serverResult, "ServerInternalUtilA"));
}

TEST_CASE_FIXTURE(Fixture, "invalid_configuration_sends_warning_notification")
{
    client->globalConfig.completion.imports.boundaries.rules = {{"src/**", "sever"}};
    workspace.setupWithConfiguration(client->globalConfig);

    bool found = false;
    for (const auto& [method, params] : client->notificationQueue)
        if (method == "window/showMessage" && params &&
            params->at("message").get<std::string>().find("unknown context 'sever'") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST_SUITE_END();
