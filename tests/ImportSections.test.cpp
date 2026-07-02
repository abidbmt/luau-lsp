#include "doctest.h"
#include "Fixture.h"
#include "Platform/ImportSections.hpp"

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

static const std::string SOURCEMAP_FOR_SECTION_TESTS = R"(
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
                        {"name": "getPlayerName", "className": "ModuleScript", "filePaths": ["shared/Utils/getPlayerName.luau"]},
                        {"name": "getPlayerAge", "className": "ModuleScript", "filePaths": ["shared/Utils/getPlayerAge.luau"]}
                    ]
                }
            ]
        },
        {
            "name": "ServerScriptService",
            "className": "ServerScriptService",
            "children": [{"name": "Main", "className": "ModuleScript", "filePaths": ["server/Main.luau"]}]
        }
    ]
}
)";

static void configureSections(Fixture* fixture)
{
    fixture->client->globalConfig.completion.imports.enabled = true;
    fixture->client->globalConfig.completion.imports.sections.services = "^--\\s*Services\\b";
    fixture->client->globalConfig.completion.imports.sections.modules = "^--\\s*Modules\\b";
}

TEST_SUITE_BEGIN("ImportSections");

// Documents the current (upstream) behaviour that sections exist to fix: without configuration,
// generated imports are inserted at the top of the file, above any organisational headings
TEST_CASE_FIXTURE(Fixture, "insertion_ignores_section_headings_without_configuration")
{
    client->globalConfig.completion.imports.enabled = true;
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    auto [source, marker] = sourceWithMarker(R"(-- Services -------------------------

-- Modules --------------------------

local Module = {}
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto item = getItem(result, "getPlayerId");
    REQUIRE(item);
    REQUIRE_EQ(item->additionalTextEdits.size(), 2);
    CHECK_EQ(item->additionalTextEdits[0].range, lsp::Range{{0, 0}, {0, 0}});
    CHECK_EQ(item->additionalTextEdits[1].range, lsp::Range{{0, 0}, {0, 0}});
}

TEST_CASE_FIXTURE(Fixture, "service_and_require_inserted_into_their_sections")
{
    configureSections(this);
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    auto [source, marker] = sourceWithMarker(R"(-- Services -------------------------

-- Modules --------------------------

-- Types ----------------------------

local Module = {}
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto item = getItem(result, "getPlayerId");
    REQUIRE(item);
    REQUIRE_EQ(item->additionalTextEdits.size(), 2);
    CHECK_EQ(item->additionalTextEdits[0].newText, "local ReplicatedStorage = game:GetService(\"ReplicatedStorage\")\n");
    CHECK_EQ(item->additionalTextEdits[0].range, lsp::Range{{1, 0}, {1, 0}});
    CHECK_EQ(item->additionalTextEdits[1].newText, "local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)\n");
    CHECK_EQ(item->additionalTextEdits[1].range, lsp::Range{{3, 0}, {3, 0}});

    CHECK_EQ(applyEdit(source, item->additionalTextEdits), R"(-- Services -------------------------
local ReplicatedStorage = game:GetService("ReplicatedStorage")

-- Modules --------------------------
local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)

-- Types ----------------------------

local Module = {}
)");
}

TEST_CASE_FIXTURE(Fixture, "require_inserted_into_modules_section_only")
{
    configureSections(this);
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    auto [source, marker] = sourceWithMarker(R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

-- Modules --------------------------
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto item = getItem(result, "getPlayerId");
    REQUIRE(item);
    // Service already imported: only the require edit is generated, inside the modules section
    REQUIRE_EQ(item->additionalTextEdits.size(), 1);
    CHECK_EQ(item->additionalTextEdits[0].newText, "local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)\n");
    CHECK_EQ(item->additionalTextEdits[0].range, lsp::Range{{3, 0}, {3, 0}});
}

TEST_CASE_FIXTURE(Fixture, "requires_keep_sorted_order_within_modules_section")
{
    configureSections(this);
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    auto [source, marker] = sourceWithMarker(R"(local ReplicatedStorage = game:GetService("ReplicatedStorage")

-- Modules --------------------------
local getPlayerAge = require(ReplicatedStorage.Utils.getPlayerAge)
local getPlayerName = require(ReplicatedStorage.Utils.getPlayerName)
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto item = getItem(result, "getPlayerId");
    REQUIRE(item);
    REQUIRE_EQ(item->additionalTextEdits.size(), 1);
    // Sorted between getPlayerAge and getPlayerName
    CHECK_EQ(item->additionalTextEdits[0].range, lsp::Range{{4, 0}, {4, 0}});
}

TEST_CASE_FIXTURE(Fixture, "fallback_when_sections_do_not_match")
{
    configureSections(this);
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    auto [source, marker] = sourceWithMarker(R"(local Module = {}
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto item = getItem(result, "getPlayerId");
    REQUIRE(item);
    REQUIRE_EQ(item->additionalTextEdits.size(), 2);
    CHECK_EQ(item->additionalTextEdits[0].range, lsp::Range{{0, 0}, {0, 0}});
    CHECK_EQ(item->additionalTextEdits[1].range, lsp::Range{{0, 0}, {0, 0}});
}

TEST_CASE_FIXTURE(Fixture, "heading_detection_tolerates_decorations_and_whitespace")
{
    configureSections(this);
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    auto [source, marker] = sourceWithMarker(R"(--   Services ======================

--Modules
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto item = getItem(result, "getPlayerId");
    REQUIRE(item);
    REQUIRE_EQ(item->additionalTextEdits.size(), 2);
    CHECK_EQ(item->additionalTextEdits[0].range, lsp::Range{{1, 0}, {1, 0}});
    CHECK_EQ(item->additionalTextEdits[1].range, lsp::Range{{3, 0}, {3, 0}});
}

TEST_CASE_FIXTURE(Fixture, "multiple_matching_headings_first_wins")
{
    configureSections(this);
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    auto [source, marker] = sourceWithMarker(R"(-- Modules --------------------------

-- Modules (secondary) ---------------
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto item = getItem(result, "getPlayerId");
    REQUIRE(item);
    REQUIRE_EQ(item->additionalTextEdits.size(), 2);
    // The require goes under the first matching heading
    CHECK_EQ(item->additionalTextEdits[1].range, lsp::Range{{1, 0}, {1, 0}});
}

TEST_CASE_FIXTURE(Fixture, "invalid_section_pattern_falls_back_and_warns")
{
    client->globalConfig.completion.imports.enabled = true;
    client->globalConfig.completion.imports.sections.services = "([";
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    workspace.setupWithConfiguration(client->globalConfig);
    bool found = false;
    for (const auto& [method, params] : client->notificationQueue)
        if (method == "window/showMessage" && params &&
            params->at("message").get<std::string>().find("invalid regular expression") != std::string::npos)
            found = true;
    CHECK(found);

    auto [source, marker] = sourceWithMarker(R"(-- Services -------------------------
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto item = getItem(result, "getPlayerId");
    REQUIRE(item);
    REQUIRE_EQ(item->additionalTextEdits.size(), 2);
    // Invalid pattern: default insertion behaviour
    CHECK_EQ(item->additionalTextEdits[0].range, lsp::Range{{0, 0}, {0, 0}});
}

TEST_CASE_FIXTURE(Fixture, "service_suggestions_inserted_into_services_section")
{
    configureSections(this);
    client->globalConfig.completion.imports.suggestServices = true;
    loadSourcemap(SOURCEMAP_FOR_SECTION_TESTS);

    auto [source, marker] = sourceWithMarker(R"(-- Services -------------------------

-- Modules --------------------------
|)");
    auto uri = newDocument("server/Main.luau", source);
    auto result = requestCompletion(this, uri, marker);

    auto serviceImport = getItem(result, "ReplicatedStorage");
    REQUIRE(serviceImport);
    CHECK_EQ(serviceImport->detail, "Auto-import");
    REQUIRE_EQ(serviceImport->additionalTextEdits.size(), 1);
    CHECK_EQ(serviceImport->additionalTextEdits[0].range, lsp::Range{{1, 0}, {1, 0}});
}

TEST_SUITE_END();
