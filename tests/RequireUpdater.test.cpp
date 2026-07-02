#include "doctest.h"
#include "Fixture.h"
#include "LSP/RequireUpdater.hpp"

using namespace Luau::LanguageServer;

static const std::string SOURCEMAP_BEFORE_MOVE = R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {
                    "name": "Utils",
                    "className": "Folder",
                    "children": [
                        {"name": "getPlayerId", "className": "ModuleScript", "filePaths": ["shared/Utils/getPlayerId.luau"]},
                        {"name": "getPlayerName", "className": "ModuleScript", "filePaths": ["shared/Utils/getPlayerName.luau"]}
                    ]
                },
                {"name": "Players", "className": "Folder", "children": []}
            ]
        },
        {
            "name": "ServerScriptService",
            "className": "ServerScriptService",
            "children": [
                {"name": "Main", "className": "ModuleScript", "filePaths": ["server/Main.luau"]}
            ]
        },
        {"name": "ServerStorage", "className": "ServerStorage", "children": []}
    ]
}
)";

// getPlayerId moved: ReplicatedStorage/Utils -> ReplicatedStorage/Players
static const std::string SOURCEMAP_AFTER_MOVE = R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {
                    "name": "Utils",
                    "className": "Folder",
                    "children": [
                        {"name": "getPlayerName", "className": "ModuleScript", "filePaths": ["shared/Utils/getPlayerName.luau"]}
                    ]
                },
                {
                    "name": "Players",
                    "className": "Folder",
                    "children": [
                        {"name": "getPlayerId", "className": "ModuleScript", "filePaths": ["shared/Players/getPlayerId.luau"]}
                    ]
                }
            ]
        },
        {
            "name": "ServerScriptService",
            "className": "ServerScriptService",
            "children": [
                {"name": "Main", "className": "ModuleScript", "filePaths": ["server/Main.luau"]}
            ]
        },
        {"name": "ServerStorage", "className": "ServerStorage", "children": []}
    ]
}
)";

// getPlayerId moved: ReplicatedStorage/Utils -> ServerStorage (cross-service)
static const std::string SOURCEMAP_AFTER_CROSS_SERVICE_MOVE = R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {"name": "Utils", "className": "Folder", "children": []},
                {"name": "Players", "className": "Folder", "children": []}
            ]
        },
        {
            "name": "ServerScriptService",
            "className": "ServerScriptService",
            "children": [
                {"name": "Main", "className": "ModuleScript", "filePaths": ["server/Main.luau"]}
            ]
        },
        {
            "name": "ServerStorage",
            "className": "ServerStorage",
            "children": [
                {"name": "getPlayerId", "className": "ModuleScript", "filePaths": ["storage/getPlayerId.luau"]}
            ]
        }
    ]
}
)";

// Utils directory renamed to Lib (with all its children)
static const std::string SOURCEMAP_AFTER_DIRECTORY_MOVE = R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {
                    "name": "Lib",
                    "className": "Folder",
                    "children": [
                        {"name": "getPlayerId", "className": "ModuleScript", "filePaths": ["shared/Lib/getPlayerId.luau"]},
                        {"name": "getPlayerName", "className": "ModuleScript", "filePaths": ["shared/Lib/getPlayerName.luau"]}
                    ]
                },
                {"name": "Players", "className": "Folder", "children": []}
            ]
        },
        {
            "name": "ServerScriptService",
            "className": "ServerScriptService",
            "children": [
                {"name": "Main", "className": "ModuleScript", "filePaths": ["server/Main.luau"]}
            ]
        },
        {"name": "ServerStorage", "className": "ServerStorage", "children": []}
    ]
}
)";

static void setApplyEditCapability(Fixture* fixture)
{
    lsp::ClientWorkspaceCapabilities workspaceCapabilities;
    workspaceCapabilities.applyEdit = true;
    fixture->client->capabilities.workspace = workspaceCapabilities;
}

static size_t countRequests(Fixture* fixture, const std::string& method)
{
    size_t count = 0;
    for (const auto& [requestMethod, _] : fixture->client->requestQueue)
        if (requestMethod == method)
            count++;
    return count;
}

TEST_SUITE_BEGIN("RequireUpdater");

TEST_CASE_FIXTURE(Fixture, "service_based_require_updated_when_module_moved")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)
)";
    auto mainUri = newDocument("server/Main.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("shared/Players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    REQUIRE_EQ(operation.movedInstanceModules.size(), 1);
    CHECK_EQ(operation.movedInstanceModules[0].oldVirtualPath, "game/ReplicatedStorage/Utils/getPlayerId");

    loadSourcemap(SOURCEMAP_AFTER_MOVE);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    CHECK_FALSE(result.needsSourcemapUpdate);
    CHECK(result.skipped.empty());
    REQUIRE_EQ(result.updatedRequireCount, 1);
    REQUIRE_EQ(result.edit.changes.size(), 1);
    CHECK_EQ(applyEdit(source, result.edit.changes.at(mainUri)), R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local getPlayerId = require(ReplicatedStorage.Players.getPlayerId)
)");
}

TEST_CASE_FIXTURE(Fixture, "get_service_call_require_updated_when_module_moved")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local getPlayerId = require(game:GetService("ReplicatedStorage").Utils.getPlayerId)
)";
    auto mainUri = newDocument("server/Main.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("shared/Players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    loadSourcemap(SOURCEMAP_AFTER_MOVE);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    REQUIRE_EQ(result.updatedRequireCount, 1);
    CHECK_EQ(applyEdit(source, result.edit.changes.at(mainUri)),
        "local getPlayerId = require(game:GetService(\"ReplicatedStorage\").Players.getPlayerId)\n");
}

TEST_CASE_FIXTURE(Fixture, "script_relative_require_updated_when_module_moved")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local getPlayerName = require(script.Parent.Parent.ReplicatedStorage.Utils.getPlayerId)
)";
    // NOTE: relative DataModel paths cannot cross the DataModel root, so we express the
    // dependent as another module in ReplicatedStorage instead
    source = R"(local getPlayerId = require(script.Parent.Parent.Utils.getPlayerId)
)";
    loadSourcemap(R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {
                    "name": "Utils",
                    "className": "Folder",
                    "children": [
                        {"name": "getPlayerId", "className": "ModuleScript", "filePaths": ["shared/Utils/getPlayerId.luau"]}
                    ]
                },
                {"name": "Players", "className": "Folder", "children": []},
                {
                    "name": "Consumers",
                    "className": "Folder",
                    "children": [
                        {"name": "Consumer", "className": "ModuleScript", "filePaths": ["shared/Consumers/Consumer.luau"]}
                    ]
                }
            ]
        }
    ]
}
)");
    auto consumerUri = newDocument("shared/Consumers/Consumer.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("shared/Players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    loadSourcemap(R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {"name": "Utils", "className": "Folder", "children": []},
                {
                    "name": "Players",
                    "className": "Folder",
                    "children": [
                        {"name": "getPlayerId", "className": "ModuleScript", "filePaths": ["shared/Players/getPlayerId.luau"]}
                    ]
                },
                {
                    "name": "Consumers",
                    "className": "Folder",
                    "children": [
                        {"name": "Consumer", "className": "ModuleScript", "filePaths": ["shared/Consumers/Consumer.luau"]}
                    ]
                }
            ]
        }
    ]
}
)");
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    REQUIRE_EQ(result.updatedRequireCount, 1);
    CHECK_EQ(applyEdit(source, result.edit.changes.at(consumerUri)),
        "local getPlayerId = require(script.Parent.Parent.Players.getPlayerId)\n");
}

TEST_CASE_FIXTURE(Fixture, "cross_service_move_adds_missing_service_import")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)
)";
    auto mainUri = newDocument("server/Main.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("storage/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    loadSourcemap(SOURCEMAP_AFTER_CROSS_SERVICE_MOVE);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    REQUIRE_EQ(result.updatedRequireCount, 1);
    CHECK_EQ(applyEdit(source, result.edit.changes.at(mainUri)), R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")
local ServerStorage = game:GetService("ServerStorage")

local getPlayerId = require(ServerStorage.getPlayerId)
)");
}

TEST_CASE_FIXTURE(Fixture, "directory_move_updates_all_descendant_requires")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)
local getPlayerName = require(ReplicatedStorage.Utils.getPlayerName)
)";
    auto mainUri = newDocument("server/Main.luau", source);
    newDocument("shared/Utils/getPlayerId.luau", "return {}");
    newDocument("shared/Utils/getPlayerName.luau", "return {}");
    auto oldUri = workspace.rootUri.resolvePath("shared/Utils");
    auto newUri = workspace.rootUri.resolvePath("shared/Lib");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    REQUIRE_EQ(operation.movedInstanceModules.size(), 2);

    loadSourcemap(SOURCEMAP_AFTER_DIRECTORY_MOVE);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    REQUIRE_EQ(result.updatedRequireCount, 2);
    CHECK_EQ(applyEdit(source, result.edit.changes.at(mainUri)), R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local getPlayerId = require(ReplicatedStorage.Lib.getPlayerId)
local getPlayerName = require(ReplicatedStorage.Lib.getPlayerName)
)");
}

TEST_CASE_FIXTURE(Fixture, "unrelated_strings_and_comments_untouched")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"lua(local ReplicatedStorage = game:GetService("ReplicatedStorage")

-- see require(ReplicatedStorage.Utils.getPlayerId) for details
local notARequire = "require(ReplicatedStorage.Utils.getPlayerId)"
local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)
)lua";
    auto mainUri = newDocument("server/Main.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("shared/Players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    loadSourcemap(SOURCEMAP_AFTER_MOVE);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    REQUIRE_EQ(result.updatedRequireCount, 1);
    CHECK_EQ(applyEdit(source, result.edit.changes.at(mainUri)), R"lua(local ReplicatedStorage = game:GetService("ReplicatedStorage")

-- see require(ReplicatedStorage.Utils.getPlayerId) for details
local notARequire = "require(ReplicatedStorage.Utils.getPlayerId)"
local getPlayerId = require(ReplicatedStorage.Players.getPlayerId)
)lua");
}

TEST_CASE_FIXTURE(Fixture, "dynamic_requires_untouched")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")
local name = "getPlayerId"
local dynamic = require(ReplicatedStorage.Utils[name])
)";
    newDocument("server/Main.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("shared/Players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    loadSourcemap(SOURCEMAP_AFTER_MOVE);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    CHECK_EQ(result.updatedRequireCount, 0);
    CHECK(result.edit.changes.empty());
}

TEST_CASE_FIXTURE(Fixture, "wait_for_child_requires_skipped_and_reported")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")
local getPlayerId = require(ReplicatedStorage.Utils:WaitForChild("getPlayerId"))
)";
    newDocument("server/Main.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("shared/Players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    loadSourcemap(SOURCEMAP_AFTER_MOVE);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    CHECK_EQ(result.updatedRequireCount, 0);
    REQUIRE_EQ(result.skipped.size(), 1);
    CHECK(result.skipped[0].find("not a plain instance path") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "needs_sourcemap_update_when_new_location_unknown")
{
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")
local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)
)";
    newDocument("server/Main.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("shared/Players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    // Sourcemap NOT regenerated: the new location is unknown
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    CHECK(result.needsSourcemapUpdate);
    CHECK_EQ(result.updatedRequireCount, 0);
}

TEST_CASE_FIXTURE(Fixture, "relative_string_requires_updated")
{
    switchToStandardPlatform();
    std::string source = R"(local getPlayerId = require("./utils/getPlayerId")
)";
    auto mainUri = newDocument("main.luau", source);
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    CHECK_FALSE(result.needsSourcemapUpdate);
    REQUIRE_EQ(result.updatedRequireCount, 1);
    CHECK_EQ(applyEdit(source, result.edit.changes.at(mainUri)), "local getPlayerId = require(\"./players/getPlayerId\")\n");
}

TEST_CASE_FIXTURE(Fixture, "aliased_string_requires_keep_alias_when_possible")
{
    switchToStandardPlatform();
    loadLuaurc(R"({"aliases": {"utils": "utils"}})");
    std::string source = R"(local getPlayerId = require("@utils/getPlayerId")
)";
    auto mainUri = newDocument("main.luau", source);
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("utils/getPlayerIdentifier.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    REQUIRE_EQ(result.updatedRequireCount, 1);
    CHECK_EQ(applyEdit(source, result.edit.changes.at(mainUri)), "local getPlayerId = require(\"@utils/getPlayerIdentifier\")\n");
}

TEST_CASE_FIXTURE(Fixture, "different_importing_files_get_different_paths")
{
    switchToStandardPlatform();
    std::string mainSource = R"(local getPlayerId = require("./utils/getPlayerId")
)";
    std::string consumerSource = R"(local getPlayerId = require("../utils/getPlayerId")
)";
    auto mainUri = newDocument("main.luau", mainSource);
    auto consumerUri = newDocument("sub/consumer.luau", consumerSource);
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    REQUIRE_EQ(result.updatedRequireCount, 2);
    REQUIRE_EQ(result.edit.changes.size(), 2);
    CHECK_EQ(applyEdit(mainSource, result.edit.changes.at(mainUri)), "local getPlayerId = require(\"./players/getPlayerId\")\n");
    CHECK_EQ(applyEdit(consumerSource, result.edit.changes.at(consumerUri)), "local getPlayerId = require(\"../players/getPlayerId\")\n");
}

TEST_CASE_FIXTURE(Fixture, "unresolved_string_requires_untouched")
{
    switchToStandardPlatform();
    std::string source = R"(local thing = require("./somewhere/else")
)";
    newDocument("main.luau", source);
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation);

    CHECK_EQ(result.updatedRequireCount, 0);
}

TEST_CASE_FIXTURE(Fixture, "limit_to_files_restricts_update_scope")
{
    switchToStandardPlatform();
    std::string mainSource = R"(local getPlayerId = require("./utils/getPlayerId")
)";
    std::string consumerSource = R"(local getPlayerId = require("../utils/getPlayerId")
)";
    auto mainUri = newDocument("main.luau", mainSource);
    newDocument("sub/consumer.luau", consumerSource);
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    auto operation = RequireUpdates::captureRenameOperation(workspace, oldUri, newUri);
    auto result = RequireUpdates::computeRequireUpdates(workspace, operation, std::vector<Uri>{mainUri});

    REQUIRE_EQ(result.updatedRequireCount, 1);
    REQUIRE_EQ(result.edit.changes.size(), 1);
    CHECK(result.edit.changes.count(mainUri));
}

// ===== onDidRenameFiles flow (prompting, modes, capability gating) =====

TEST_CASE_FIXTURE(Fixture, "prompt_accept_applies_edit")
{
    switchToStandardPlatform();
    setApplyEditCapability(this);
    std::string source = R"(local getPlayerId = require("./utils/getPlayerId")
)";
    auto mainUri = newDocument("main.luau", source);
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});

    REQUIRE(!client->requestQueue.empty());
    REQUIRE_EQ(client->requestQueue.back().first, "window/showMessageRequest");

    client->respondToLastRequest(json{{"title", "Update requires"}});

    REQUIRE_EQ(client->requestQueue.back().first, "workspace/applyEdit");
    lsp::ApplyWorkspaceEditParams editParams = client->requestQueue.back().second.value();
    REQUIRE_EQ(editParams.edit.changes.size(), 1);
    CHECK_EQ(applyEdit(source, editParams.edit.changes.at(mainUri)), "local getPlayerId = require(\"./players/getPlayerId\")\n");
}

TEST_CASE_FIXTURE(Fixture, "prompt_skip_does_not_apply_edit")
{
    switchToStandardPlatform();
    setApplyEditCapability(this);
    newDocument("main.luau", "local getPlayerId = require(\"./utils/getPlayerId\")\n");
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});
    REQUIRE_EQ(client->requestQueue.back().first, "window/showMessageRequest");

    client->respondToLastRequest(json{{"title", "Skip"}});
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 0);
}

TEST_CASE_FIXTURE(Fixture, "prompt_dismissed_does_not_apply_edit")
{
    switchToStandardPlatform();
    setApplyEditCapability(this);
    newDocument("main.luau", "local getPlayerId = require(\"./utils/getPlayerId\")\n");
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});
    REQUIRE_EQ(client->requestQueue.back().first, "window/showMessageRequest");

    client->respondToLastRequest(std::nullopt); // null result = dismissed
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 0);
}

TEST_CASE_FIXTURE(Fixture, "always_mode_applies_without_prompting")
{
    switchToStandardPlatform();
    setApplyEditCapability(this);
    client->globalConfig.fileOperations.updateRequiresOnMove = RequireUpdateMode::Always;
    newDocument("main.luau", "local getPlayerId = require(\"./utils/getPlayerId\")\n");
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});

    CHECK_EQ(countRequests(this, "window/showMessageRequest"), 0);
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 1);
}

TEST_CASE_FIXTURE(Fixture, "never_mode_does_nothing")
{
    switchToStandardPlatform();
    setApplyEditCapability(this);
    client->globalConfig.fileOperations.updateRequiresOnMove = RequireUpdateMode::Never;
    newDocument("main.luau", "local getPlayerId = require(\"./utils/getPlayerId\")\n");
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});

    CHECK_EQ(countRequests(this, "window/showMessageRequest"), 0);
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 0);
}

TEST_CASE_FIXTURE(Fixture, "prompt_always_choice_persists_for_session")
{
    switchToStandardPlatform();
    setApplyEditCapability(this);
    newDocument("main.luau", "local getPlayerId = require(\"./utils/getPlayerId\")\n");
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});
    client->respondToLastRequest(json{{"title", "Always update"}});
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 1);

    // Second rename: applied automatically, no further prompt
    auto secondNewUri = workspace.rootUri.resolvePath("misc/getPlayerId.luau");
    workspace.onDidRenameFiles({{oldUri.toString(), secondNewUri.toString()}});
    CHECK_EQ(countRequests(this, "window/showMessageRequest"), 1);
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 2);
}

TEST_CASE_FIXTURE(Fixture, "prompt_never_choice_persists_for_session")
{
    switchToStandardPlatform();
    setApplyEditCapability(this);
    newDocument("main.luau", "local getPlayerId = require(\"./utils/getPlayerId\")\n");
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});
    client->respondToLastRequest(json{{"title", "Never update"}});
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 0);

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});
    CHECK_EQ(countRequests(this, "window/showMessageRequest"), 1);
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 0);
}

TEST_CASE_FIXTURE(Fixture, "client_without_apply_edit_gets_fallback_message")
{
    switchToStandardPlatform();
    // No applyEdit capability set
    newDocument("main.luau", "local getPlayerId = require(\"./utils/getPlayerId\")\n");
    auto oldUri = newDocument("utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});

    CHECK_EQ(countRequests(this, "window/showMessageRequest"), 0);
    CHECK_EQ(countRequests(this, "workspace/applyEdit"), 0);
    bool found = false;
    for (const auto& [method, params] : client->notificationQueue)
        if (method == "window/showMessage" && params &&
            params->at("message").get<std::string>().find("luau-lsp/updateRequiresForRename") != std::string::npos)
            found = true;
    CHECK(found);
}

TEST_CASE_FIXTURE(Fixture, "roblox_updates_deferred_until_sourcemap_regenerates")
{
    setApplyEditCapability(this);
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    std::string source = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)
)";
    auto mainUri = newDocument("server/Main.luau", source);
    auto oldUri = newDocument("shared/Utils/getPlayerId.luau", "return {}");
    auto newUri = workspace.rootUri.resolvePath("shared/Players/getPlayerId.luau");

    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});

    // The sourcemap does not yet know the new location: no prompt yet
    CHECK_EQ(countRequests(this, "window/showMessageRequest"), 0);

    // Rojo regenerates the sourcemap - the pending update is recomputed and prompts
    loadSourcemap(SOURCEMAP_AFTER_MOVE);
    REQUIRE_EQ(countRequests(this, "window/showMessageRequest"), 1);

    client->respondToLastRequest(json{{"title", "Update requires"}});
    REQUIRE_EQ(client->requestQueue.back().first, "workspace/applyEdit");
    lsp::ApplyWorkspaceEditParams editParams = client->requestQueue.back().second.value();
    CHECK_EQ(applyEdit(source, editParams.edit.changes.at(mainUri)), R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local getPlayerId = require(ReplicatedStorage.Players.getPlayerId)
)");
}

// Mirrors a Rojo project that pins instance names per file: renaming the folder on disk is
// accompanied by renaming the instances in the project file, and one of the moved modules
// itself requires its moved sibling
static const std::string SOURCEMAP_HOOKS_BEFORE = R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {
                    "name": "EventHooks",
                    "className": "Folder",
                    "children": [
                        {
                            "name": "PlayersHooks",
                            "className": "Folder",
                            "children": [
                                {"name": "PlayersHooks", "className": "ModuleScript", "filePaths": ["src/EventHooks/PlayersHooks/API.luau"]},
                                {"name": "PlayersHooksSystem", "className": "ModuleScript", "filePaths": ["src/EventHooks/PlayersHooks/System.luau"]}
                            ]
                        }
                    ]
                }
            ]
        },
        {
            "name": "ServerScriptService",
            "className": "ServerScriptService",
            "children": [
                {"name": "DeathSystem", "className": "ModuleScript", "filePaths": ["src/Death/System.luau"]}
            ]
        }
    ]
}
)";

static const std::string SOURCEMAP_HOOKS_AFTER = R"(
{
    "name": "game",
    "className": "DataModel",
    "children": [
        {
            "name": "ReplicatedStorage",
            "className": "ReplicatedStorage",
            "children": [
                {
                    "name": "EventHooks",
                    "className": "Folder",
                    "children": [
                        {
                            "name": "PlayerssHooks",
                            "className": "Folder",
                            "children": [
                                {"name": "PlayerssHooks", "className": "ModuleScript", "filePaths": ["src/EventHooks/PlayerssHooks/API.luau"]},
                                {"name": "PlayerssHooksSystem", "className": "ModuleScript", "filePaths": ["src/EventHooks/PlayerssHooks/System.luau"]}
                            ]
                        }
                    ]
                }
            ]
        },
        {
            "name": "ServerScriptService",
            "className": "ServerScriptService",
            "children": [
                {"name": "DeathSystem", "className": "ModuleScript", "filePaths": ["src/Death/System.luau"]}
            ]
        }
    ]
}
)";

TEST_CASE_FIXTURE(Fixture, "directory_move_with_instance_renames_updates_moved_and_unmoved_consumers")
{
    setApplyEditCapability(this);
    loadSourcemap(SOURCEMAP_HOOKS_BEFORE);

    std::string systemSource = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local PlayersHooks = require(ReplicatedStorage.EventHooks.PlayersHooks.PlayersHooks)
)";
    std::string deathSource = R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local PlayersHooks = require(ReplicatedStorage.EventHooks.PlayersHooks.PlayersHooks)
)";
    newDocument("src/EventHooks/PlayersHooks/API.luau", "return {}");
    auto movedConsumerOldUri = newDocument("src/EventHooks/PlayersHooks/System.luau", systemSource);
    auto deathUri = newDocument("src/Death/System.luau", deathSource);

    auto oldUri = workspace.rootUri.resolvePath("src/EventHooks/PlayersHooks");
    auto newUri = workspace.rootUri.resolvePath("src/EventHooks/PlayerssHooks");
    workspace.onDidRenameFiles({{oldUri.toString(), newUri.toString()}});

    // Deferred: the sourcemap does not yet contain the new locations
    CHECK_EQ(countRequests(this, "window/showMessageRequest"), 0);

    // Rojo regenerated with the renamed instances (project file was updated by the user)
    loadSourcemap(SOURCEMAP_HOOKS_AFTER);
    REQUIRE_EQ(countRequests(this, "window/showMessageRequest"), 1);

    client->respondToLastRequest(json{{"title", "Update requires"}});
    REQUIRE_EQ(client->requestQueue.back().first, "workspace/applyEdit");
    lsp::ApplyWorkspaceEditParams editParams = client->requestQueue.back().second.value();

    // The unmoved consumer is updated in place
    CHECK_EQ(applyEdit(deathSource, editParams.edit.changes.at(deathUri)), R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local PlayersHooks = require(ReplicatedStorage.EventHooks.PlayerssHooks.PlayerssHooks)
)");

    // The moved consumer's edit must target its NEW uri (the old file no longer exists)
    auto movedConsumerNewUri = workspace.rootUri.resolvePath("src/EventHooks/PlayerssHooks/System.luau");
    CHECK_EQ(editParams.edit.changes.count(movedConsumerOldUri), 0);
    REQUIRE_EQ(editParams.edit.changes.count(movedConsumerNewUri), 1);
    CHECK_EQ(applyEdit(systemSource, editParams.edit.changes.at(movedConsumerNewUri)), R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

local PlayersHooks = require(ReplicatedStorage.EventHooks.PlayerssHooks.PlayerssHooks)
)");
}

// Regression scaffold for a new-solver use-after-free: after a sourcemap change moves a module,
// re-checking a consumer replaces the dirty dependency mid-queue and the consumer's solve read
// freed interface types. Only meaningful under ASAN; asserts nothing beyond surviving
TEST_CASE_FIXTURE(Fixture, "new_solver_module_replacement_after_sourcemap_change_no_uaf")
{
    ENABLE_NEW_SOLVER();
    loadSourcemap(SOURCEMAP_BEFORE_MOVE);
    auto aUri = newDocument("shared/Utils/getPlayerId.luau", "return { id = 1 }");
    auto bUri = newDocument("server/Main.luau", R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")
local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)
return getPlayerId.id
)");
    auto aName = workspace.fileResolver.getModuleName(aUri);
    auto bName = workspace.fileResolver.getModuleName(bUri);

    workspace.checkStrict(bName, /* cancellationToken= */ nullptr, /* forAutocomplete= */ false);

    // Rojo regenerated the sourcemap: getPlayerId moved to another folder; requires are stale
    loadSourcemap(SOURCEMAP_AFTER_MOVE);
    workspace.frontend.markDirty(aName);
    workspace.frontend.markDirty(bName);

    workspace.checkStrict(bName, /* cancellationToken= */ nullptr, /* forAutocomplete= */ false);
}

TEST_SUITE_END();
