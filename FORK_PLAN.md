# Fork Implementation Plan

Implementation proposal for four fork features, designed for minimal upstream merge-conflict risk:

1. Configurable client/server/shared boundaries for auto-import visibility
2. Section-aware auto-import insertion
3. Require updates when modules are moved or renamed
4. Context-aware (scoped) auto-import visibility for internal modules

All line numbers reference the current `main` (`d2f53fc`).

---

## 1. Repository architecture summary (relevant subsystems)

### 1.1 Auto-import pipeline

Candidates are enumerated **on demand per completion request** — there is no persistent candidate index.

| Concern | Where |
|---|---|
| Candidate enumeration (Roblox) | `RobloxPlatform::virtualPathsToSourceNodes` (`src/include/Platform/RobloxPlatform.hpp:104`), iterated by `computeAllInstanceRequires` (`src/platform/roblox/InstanceRequireAutoImporter.cpp:33-111`) |
| Candidate enumeration (string requires) | `defaultModuleVisitor` over `frontend.sourceNodes` (`src/include/Platform/StringRequireAutoImporter.hpp:12-19`); Roblox override `RobloxPlatform::getAutoImportsModuleVisitor` (`src/platform/roblox/RobloxFileResolver.cpp:317-327`) |
| Ignore-glob filter | `WorkspaceFolder::isIgnoredFileForAutoImports` (`src/Workspace.cpp:273-287`) using `config.completion.imports.ignoreGlobs`, `glob::gitignore_glob_match`, and `uri.lexicallyRelative(rootUri)` |
| Existing-import dedup | `FindImportsVisitor::containsRequire` / `serviceLineMap` (`src/platform/AutoImports.cpp:20-68`, `src/include/Platform/InstanceRequireAutoImporter.hpp:14-62`) — keyed by **local variable name** |
| Boundary check (today) | `isScriptContextCompatible` (`src/include/Platform/RobloxPlatform.hpp:25-30`), called from `InstanceRequireAutoImporter.cpp:55` and `computeSourcemapRequirePath` (`src/platform/roblox/RobloxFileResolver.cpp:230`) |
| Boundary classification (today) | `RobloxPlatform::writePathsToMap` (`src/platform/roblox/RobloxSourcemap.cpp:475-506`): `Script`→Server, `LocalScript`→Client, container names (`ServerScriptService`/`ServerStorage`→Server; `StarterPlayer`/`StarterGui`/`StarterPack`/`ReplicatedFirst`→Client), **everything else inherits, default `Shared`** |
| Insertion position | `computeHotCommentsLineNumber`, `computeMinimumLineNumberForRequire`, `computeBestLineForRequire` (`src/platform/AutoImports.cpp:7-18, 110-173`); `RobloxFindImportsVisitor::findBestLineForService` (`InstanceRequireAutoImporter.hpp:20-33`) |
| Edit construction | `createRequireTextEdit` / `createSuggestRequire` (`AutoImports.cpp:80-107`); `createServiceTextEdit` (`InstanceRequireAutoImporter.cpp:9-16`) |
| Require-path computation | Roblox instance: inline in `computeAllInstanceRequires` + `convertToScriptPath` (`src/Utils.cpp:61-93`); Roblox string: `computeSourcemapRequirePath` (`RobloxFileResolver.cpp:212-315`); generic string: `computeRequirePath` / `computeBestAliasedPath` (`src/platform/StringRequireAutoImporter.cpp:25-103`) |
| Code-action auto-import | `handleUnknownSymbolFix` / `computeAddAllMissingImportsEdits` (`src/platform/LSPPlatform.cpp:230-324`, `src/platform/roblox/RobloxCodeAction.cpp:80-279`), invoked from `src/operations/CodeAction.cpp:335-353, 444-467` |

**The two defects the fork fixes in feature 1 are confirmed:**
- `isScriptContextCompatible` returns `true` whenever *either* side is `Shared` — so shared callers receive client-only and server-only suggestions.
- Everything under `ReplicatedStorage` is classified `Shared` by the `writePathsToMap` heuristic; `RunContext` is not present in Rojo sourcemaps, so location-based heuristics cannot see it. Path rules are the correct lever.

### 1.2 Module resolution & require graph

- ModuleName ↔ Uri bridge: `WorkspaceFileResolver::getModuleName` / `getUri` (`src/WorkspaceFileResolver.cpp:23-46`). A ModuleName is a Roblox virtual path (`game/...`, `ProjectRoot/...`), a raw fs path, or a non-file URI.
- Virtual path maps: `virtualPathsToSourceNodes` / `realPathsToSourceNodes`, rebuilt wholesale by `updateSourceNodeMap` → `writePathsToMap` (`RobloxSourcemap.cpp:475-536`) whenever the sourcemap changes.
- Require resolution: `RobloxPlatform::resolveModule` (`RobloxFileResolver.cpp:358-428`) interprets `script`/`game`/`Parent`/`GetService`/`FindFirstChild` expressions; `LSPPlatform::resolveStringRequire` (`src/platform/LSPPlatform.cpp:131-181`) handles `./`, `../`, `@alias`, `@self`, init.luau semantics.
- **Reverse dependencies exist**: `WorkspaceFolder::findReverseDependencies` (`src/operations/References.cpp:111-134`) walks Luau's `SourceNode::dependents`. Forward requires with locations: `Luau::SourceNode::requireLocations` (`luau/Analysis/include/Luau/Frontend.h:75`) — note the `Location` covers the **whole `require(...)` call**, so rewriting requires re-descending to the argument expression via the AST.
- The `luau-lsp/requireGraph` extension (`src/operations/RequireGraph.cpp`) is forward-only and computed on demand; it is not a reverse index.

### 1.3 Configuration

- Schema: single header `src/include/LSP/ClientConfiguration.hpp`, `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT` macros; imports config is `ClientCompletionImportsConfiguration` (lines 141-164).
- Flow: `workspace/configuration` pull → `configStore` per workspace root (`src/Client.cpp:160-224`); `configChangedCallback` → `WorkspaceFolder::setupWithConfiguration` (`src/LanguageServer.cpp:749-827`, `src/Workspace.cpp:664`). CLI: `dottedToClientConfiguration` (`src/CliConfigurationParser.cpp`), global config only.
- **There is no config validation anywhere.** Bad globs silently never match; unknown enum values silently map to the first `NLOHMANN_JSON_SERIALIZE_ENUM` entry. Validation is greenfield.
- VSCode settings: `editors/code/package.json` `contributes.configuration` (`luau-lsp.completion.imports.*` at lines 584-662), 1:1 with the struct.

### 1.4 LSP dispatch, capabilities, file operations

- Dispatch: linear if/else chains in `LanguageServer::onRequest` (`src/LanguageServer.cpp:161-410`) and `onNotification` (`:412-486`).
- **No file-operation support exists**: no `workspace/willRenameFiles`/`didRenameFiles` handlers, no `FileRename`/`RenameFilesParams` protocol types, `WorkspaceCapabilities.fileOperations` is a comment placeholder (`src/include/Protocol/ServerCapabilities.hpp:92`), client `fileOperations`/`applyEdit`/`workspaceEdit` capabilities are unparsed (`src/include/Protocol/ClientCapabilities.hpp:454-489`).
- **No `window/showMessageRequest`**: `Protocol/Window.hpp` has only the `showMessage` notification.
- Server→client request machinery **works and is reusable**: `LSPClient::sendRequest(id, method, params, ResponseHandler)` + `handleResponse` (`src/Client.cpp:18-33, 260-285`), routed via `LanguageServer::handleMessage:547`. Model example with response handling: `requestConfiguration` (`Client.cpp:172-224`). `workspace/applyEdit` round-trip is proven by autocomplete-end (`src/operations/Completion.cpp:239-250`).
- `WorkspaceEdit` supports only `changes` (uri → TextEdit[]) (`src/include/Protocol/Structures.hpp:157-162`) — sufficient for this plan; `documentChanges` not needed.
- File renames currently arrive only as `Deleted`+`Created` watcher events (`WorkspaceFolder::onDidChangeWatchedFiles`, `src/Workspace.cpp:196-254`) which `markDirty` modules; nothing rewrites requires.
- Dynamic capability registration exists (`client->registerCapability`, used for watchers at `LanguageServer.cpp:800-818`).
- Custom extension methods (`luau-lsp/bytecode`, `luau-lsp/requireGraph`, …) + the `$/command` notification handled by the VSCode extension (`editors/code/src/extension.ts:540-542`) are the established escape hatches for editor-specific behavior.

### 1.5 Test infrastructure

- `Fixture` (`tests/Fixture.h/.cpp`): temp-dir-rooted workspace, Roblox platform by default; config via `client->globalConfig.… = value` (+ `workspace.setupWithConfiguration(...)` when re-init needed); `loadSourcemap(json)`, `sourceWithMarker("…|…")`, `newDocument`, `applyEdit(source, edits)`, `switchToStandardPlatform()`, real files via `TempDir::write_child`.
- Auto-import assertions: `workspace.completion(params, nullptr)` → `CompletionItem.additionalTextEdits` (`tests/AutoImports.test.cpp`), with `SOURCEMAP_FOR_SERVER_CLIENT_BOUNDARY_AUTO_IMPORTS` in `tests/RobloxTestConstants.h` already covering the existing boundary behavior.
- WorkspaceEdit assertions: returned edit (`tests/Rename.test.cpp`) or `client->requestQueue.back()` == `workspace/applyEdit` (`tests/Autocomplete.test.cpp:1353-1364`).
- **Gap**: `TestClient::sendRequest` drops the `ResponseHandler` (`tests/TestClient.cpp:10-13`), so client responses (prompt answers) cannot be simulated yet. Must be extended for feature 3.

---

## 2. Proposed configuration schema

All new fields are **additive, empty/disabled by default**, and appended at the end of their structs (merge-friendly). Names follow existing `luau-lsp.completion.imports.*` conventions.

```jsonc
{
    // ── Feature 1: execution boundaries ─────────────────────────────
    "luau-lsp.completion.imports.boundaries.rules": [
        // matched against BOTH the workspace-relative filesystem path
        // and (Roblox) the data-model path with and without the "game/" root
        { "glob": "src/Client/**",                  "context": "client" },
        { "glob": "src/Server/**",                  "context": "server" },
        { "glob": "src/Shared/**",                  "context": "shared" },
        { "glob": "ReplicatedStorage/ClientOnly/**","context": "client" }
    ],
    // Optional matrix override. This default applies once any rule is configured:
    "luau-lsp.completion.imports.boundaries.allowedImports": {
        "client": ["client", "shared"],
        "server": ["server", "shared"],
        "shared": ["shared"]
    },

    // ── Feature 2: section-aware insertion ──────────────────────────
    // ECMAScript regex, matched per line. Absent/never-matching → current behavior.
    "luau-lsp.completion.imports.sections.services": "^--\\s*Services\\b",
    "luau-lsp.completion.imports.sections.modules":  "^--\\s*Modules\\b",

    // ── Feature 4: scoped visibility ─────────────────────────────────
    "luau-lsp.completion.imports.visibilityRules": [
        // Scoped form: for each concrete directory matching `scope`,
        // modules matching `modules` (relative to that directory) are visible
        // only from files matching `visibleFrom` (relative to that directory;
        // default "**" = anywhere inside the scope directory).
        { "scope": "src/Features/*", "modules": "**/Internal/**" },
        // Global form (no scope): workspace-relative patterns.
        { "modules": "**/*.spec.luau", "visibleFrom": "tests/**" }
    ],

    // ── Feature 3: require updates on move/rename ────────────────────
    "luau-lsp.fileOperations.updateRequiresOnMove": "prompt" // "prompt" | "always" | "never"
}
```

Struct additions in `ClientConfiguration.hpp`:

- `ClientCompletionImportsBoundariesConfiguration { std::vector<BoundaryRule> rules; std::map<std::string, std::vector<std::string>> allowedImports; }`
- `ClientCompletionImportsSectionsConfiguration { std::optional<std::string> services; std::optional<std::string> modules; }`
- `std::vector<ImportVisibilityRule> visibilityRules` on `ClientCompletionImportsConfiguration`
- New top-level `ClientFileOperationsConfiguration { UpdateRequiresOnMove updateRequiresOnMove = Prompt; }`

`BoundaryRule.context` and `updateRequiresOnMove` get **hand-written `from_json`** (not `NLOHMANN_JSON_SERIALIZE_ENUM`) so unknown values are *detected* and reported instead of silently mapping to the first enum entry.

### Design decisions and resolved ambiguities

- **Visibility rules are restrictions, not reinclusions.** The user's problem statement framed feature 4 as "global ignoreGlobs + contextual reinclusion". Reinclusion-over-exclusion needs a precedence engine and makes `ignoreGlobs` order-sensitive. Instead: `ignoreGlobs` stays exactly as upstream (unconditional), and `visibilityRules` independently restrict matched modules to matched contexts. Users move `**/Internal/**` from `ignoreGlobs` into a visibility rule. This satisfies every scenario in the request with simpler, deterministic semantics. (Documented as a migration note.)
- **No `$1` capture engine.** The scoped form gets the same expressive power by testing the *ancestor prefixes* of a path against the `scope` glob: the matching prefix is the concrete scope directory; `modules`/`visibleFrom` are then matched relative to it. This reuses `glob::gitignore_glob_match` unmodified and avoids a second, incompatible pattern dialect. Package-private, feature-private, layer-private, test-only, and descendants-only patterns are all expressible.
- **Dual-representation matching.** Every module gets a `ModulePathInfo { std::string fsPath; std::optional<std::string> dataModelPath; }` (fs path = `uri.lexicallyRelative(rootUri)`; DM path = virtual path, matched both as `game/Foo/...` and `Foo/...`). A glob matches a module if it matches any representation. Separators are already normalized to `/` by `Uri`.
- **Default matrix when rules configured** is the strict one above (this is the point of the feature); when `boundaries.rules` is empty the entire subsystem is inert and upstream behavior — including the permissive `isScriptContextCompatible` — is byte-for-byte preserved.
- **Unmatched paths** keep their heuristic classification (Roblox `scriptContext`) and, on the standard platform, are unrestricted (`none` context: allowed both directions). Least surprising; documented.

---

## 3. Rule-precedence semantics

Documented, deterministic, and shared by features 1 and 4:

1. **Boundary rules: last matching rule wins** (gitignore precedent). "More specific overrides broader" is achieved by ordering; no fragile specificity metric.
2. **Visibility rules: all matching rules must pass (AND).** Adding a rule can only narrow visibility, never widen it. No inter-rule precedence needed.
3. **Filter chain for an auto-import candidate** (documented order, cheapest first):

```
indexed (sourcemap / frontend.sourceNodes)
AND same-file / non-ModuleScript / already-imported dedup   (upstream, unchanged)
AND not matched by completion.imports.ignoreGlobs           (upstream, unchanged)
AND passes every matching visibilityRule                    (feature 4)
AND import allowed by boundary matrix                       (feature 1)
```

A contextual visibility grant can therefore never bypass an execution boundary — the checks are independent ANDs, matching the requested semantics (`TestFeature2/Shared` never sees `TestFeature2/Server/Internal`).
4. **Sections: first matching heading wins** when a pattern matches multiple lines. Section body = heading line + 1 through min(next line matching *any* configured section pattern, first AST statement that is neither a require/service local nor a comment/blank) − used both to bound sorted insertion and to clamp the insertion range.
5. **Invalid patterns**: reported once per config load via `window/showMessage` (Warning) and the rule/section is treated as absent — never a hard failure, never silent.

---

## 4. Editor and LSP capability requirements

| Behavior | Mechanism | Requirement |
|---|---|---|
| Features 1, 2, 4 | Pure server-side filtering/insertion in completion + code actions | None beyond existing support |
| Move/rename detection | `workspace/didRenameFiles` notification; server registers `fileOperations.didRename` (statically in `initialize`, filters `**/*.{lua,luau}` + folders) | Client must implement file-operation notifications (VSCode: yes, automatic once advertised; Neovim/others: varies — documented) |
| Pre-move edits (stage 2, optional) | `workspace/willRenameFiles` request returning a `WorkspaceEdit` applied atomically with the move | Client `workspace.fileOperations.willRename`; **no user prompt is possible inside willRename** (editors time-box it), so this path is only used when the effective mode is `always` |
| Prompt | `window/showMessageRequest` with `MessageActionItem`s (`Update all` / `Skip` / `Always` / `Never`) | Universally implemented but not capability-gated by the spec; dismissal (null response) = "do not update". Fallback below if no response support is detected |
| Applying edits | `workspace/applyEdit` (existing `client->applyEdit`) | Parse & gate on client `workspace.applyEdit` capability (currently unparsed — will be added) |
| Fallback without prompt/applyEdit | Custom request `luau-lsp/updateRequiresForRename` (established `luau-lsp/*` extension pattern) returning the computed `WorkspaceEdit`; VSCode command + any editor can invoke it | Documented; conservative default = do nothing automatically |
| Persisting "Always"/"Never" from the prompt | Server updates its in-memory session preference always; on VSCode additionally sends `$/command` → new extension command `luau-lsp.setUpdateRequiresOnMove` that calls `workspace.getConfiguration().update(...)` | Durable persistence is editor-specific; session-scoped elsewhere (documented) |
| Cancelling the underlying move | **Not claimed.** `didRenameFiles` is post-hoc; the server never reverts filesystem operations | — |
| "Current file only" scope | LSP servers do not know editor focus. Approximated by a per-file code action / the custom request with a `uris` filter, not a prompt button | Documented deviation |

---

## 5. File-by-file implementation plan

### New files (zero merge risk)

| File | Contents |
|---|---|
| `src/include/LSP/AutoImportRules.hpp` + `src/AutoImportRules.cpp` | `ModulePathInfo`; `BoundaryKind {Client, Server, Shared, None}`; `BoundaryMatcher` (compiled from config once per request: `classify(ModulePathInfo) -> BoundaryKind`, `isImportAllowed(from, target)`, falls back to `isScriptContextCompatible(scriptContext)` semantics when no rules configured); `VisibilityMatcher` (`isVisible(currentFile, candidate)` implementing scoped-prefix + global rules); `validateImportRules(config) -> std::vector<std::string>` warnings |
| `src/include/Platform/ImportSections.hpp` + `src/platform/ImportSections.cpp` | `ImportSections { std::optional<SectionRange> services, modules; }`; `detectImportSections(const TextDocument&, const SectionsConfig&)` (regex per line, first match wins, section-end rule from §3.4); helpers `adjustMinimumLineForSection(...)`, `clampLineToSection(...)` |
| `src/include/LSP/RequireUpdater.hpp` + `src/operations/RequireUpdater.cpp` | `collectAffectedRequires(workspace, oldModuleNamePrefix)` — walks `findReverseDependencies`, re-visits each dependent's `SourceModule` AST for `require(...)` calls (via `isRequire`, `src/LuauExt.cpp:780-805`), resolves each argument with `platform->resolveModule`, keeps exact-prefix matches, records the **argument expression** location + classified style (relative-instance / service-instance / string-relative / string-alias / string-absolute); `computeUpdatedRequire(...)` — recomputes the path in the same style (reusing `convertToScriptPath`, `computeSourcemapRequirePath`, `computeRequirePath`); `buildRequireUpdateWorkspaceEdit(...)`; skipped-require reporting |
| `tests/AutoImportRules.test.cpp`, `tests/ImportSections.test.cpp`, `tests/RequireUpdater.test.cpp` | See §6 |

### Modified files (small, hook-style diffs)

| File | Change | Size |
|---|---|---|
| `src/include/LSP/ClientConfiguration.hpp` | Append new structs + fields (end of struct + end of macro list) | ~60 lines, additive |
| `src/include/Platform/InstanceRequireAutoImporter.hpp` / `StringRequireAutoImporter.hpp` | Add `const ImportSections* sections` + `const AutoImportRuleSet* rules` pointers to the two context structs | ~4 lines |
| `src/platform/roblox/InstanceRequireAutoImporter.cpp` | Line 55: `isScriptContextCompatible(...)` → `ctx.rules->isImportAllowed(...)` (which itself falls back to the old check); add visibility check next to the ignore-glob check (~line 53); wrap lines 36/78/87 minimum-line/best-line values with section adjustment | ~10 lines |
| `src/platform/roblox/RobloxFileResolver.cpp` | Line 230: same boundary swap inside `computeSourcemapRequirePath` (rules reachable via the `platform`/context already passed) | ~3 lines |
| `src/platform/StringRequireAutoImporter.cpp` | Add boundary + visibility checks beside `isIgnoredFileForAutoImports` (line ~124); section adjustment around `computeBestLineForRequire` | ~10 lines |
| `src/platform/roblox/RobloxCompletion.cpp`, `RobloxCodeAction.cpp`, `src/platform/LSPPlatform.cpp` | Construct `ImportSections` + rule set once per request and pass through contexts; service-section adjustment at the `findBestLineForService` call sites | ~8 lines each |
| `src/Workspace.cpp` / `src/include/LSP/Workspace.hpp` | Call `validateImportRules` in `setupWithConfiguration` (warn once); new `onDidRenameFiles(RenameFilesParams)` member driving the RequireUpdater + prompt flow; expose `findReverseDependencies` to the updater | ~40 lines |
| `src/LanguageServer.cpp` | Dispatch `workspace/didRenameFiles` (notification) and `luau-lsp/updateRequiresForRename` (request); advertise `fileOperations.didRename` in `getServerCapabilities()` | ~25 lines |
| `src/include/Protocol/Workspace.hpp` | `FileRename`, `RenameFilesParams`, `FileOperationPattern/Filter/RegistrationOptions` | additive |
| `src/include/Protocol/Window.hpp` | `MessageActionItem`, `ShowMessageRequestParams` | additive |
| `src/include/Protocol/ServerCapabilities.hpp` | `FileOperationsServerCapabilities` in `WorkspaceCapabilities` (replaces the `// fileOperations` placeholder) | additive |
| `src/include/Protocol/ClientCapabilities.hpp` | Parse `workspace.fileOperations`, `workspace.applyEdit` | additive |
| `src/include/LSP/Client.hpp` / `src/Client.cpp` | `showMessageRequest(type, message, actions, ResponseHandler)` wrapper modeled on `requestConfiguration` | ~25 lines |
| `tests/TestClient.h/.cpp` | Retain `ResponseHandler`s; add `respondToLastRequest(json result)` | ~15 lines |
| `editors/code/package.json` / `extension.ts` | New settings; `luau-lsp.setUpdateRequiresOnMove` command for prompt persistence | additive |
| `CMakeLists.txt`, `CHANGELOG.md`, docs | Source list, changelog entries, settings documentation + migration notes | additive |

### Feature 3 flow (didRename, per rename event)

1. `onDidRenameFiles`: for each `{oldUri, newUri}`, compute old ModuleName(s): file → `resolveToVirtualPath(oldUri)` (sourcemap still stale-old at this instant) or fs path; folder → prefix enumeration over `virtualPathsToSourceNodes` / `frontend.sourceNodes`.
2. Collect affected requires **immediately** (dependent ASTs still reference the old module names regardless of the move).
3. Determine new module names: standard platform / alias-based string requires → directly from `newUri`. Roblox instance / `@game` requires → **defer until the next completed `updateSourceMap()`** (hook after `RobloxPlatform::updateSourceNodeMap`, with a ~10s timeout), then resolve `newUri` → new virtual path. No prediction, no guessing.
4. Apply mode: `never` → drop. `always` → build edit, `client->applyEdit`. `prompt` → `showMessageRequest` ("Update N require(s) in M file(s) for '<name>'?", actions Update / Skip / Always (session) / Never (session)); null/dismiss = Skip; then apply if accepted. Requires that cannot be rewritten in their original style are skipped and listed in a `window/showMessage` summary.
5. `luau-lsp/updateRequiresForRename { oldUri, newUri, uris? }` computes and returns the same `WorkspaceEdit` for clients without prompt/didRename support (and powers a per-file scope).

---

## 6. Test plan

All tests use existing `Fixture` patterns; boundary/visibility/section tests need no new infrastructure. Feature 3 tests need the `TestClient` response-simulation extension.

- **`tests/AutoImportRules.test.cpp`** — full 9-cell matrix (client/server/shared × client/server/shared) for instance requires (sourcemap via `SOURCEMAP_FOR_SERVER_CLIENT_BOUNDARY_AUTO_IMPORTS`-style constants + `boundaries.rules`) and string requires (`switchToStandardPlatform` + `TempDir`); nested/overlapping rules (last-wins); unmatched paths (heuristic fallback); fs-path rules vs DM-path rules; custom `allowedImports` matrix; config-off = upstream behavior (regression against existing tests); Windows/Unix separator normalization; invalid `context` value → warning notification in `client->notificationQueue`. Visibility: same-feature Internal included, cross-feature excluded, outside-all-features excluded, global-form rules, multiple nested Internal dirs, AND-of-overlapping-rules, boundary-overrides-visibility (`Shared` file never sees `Server/Internal` even same feature), modules literally named `Internal` with no rule configured (unaffected), invalid pattern → warning.
- **`tests/ImportSections.test.cpp`** — insertion into existing services section / modules section / both; fallback with no headings (byte-identical to current output); varied dash counts and whitespace; `--[[ block ]]`-style headings if regex written for them; duplicate prevention unchanged; ordering among existing imports inside a section; insertion clamped when unconfigured headings (`-- Types`) follow; multiple matching headings → first wins; invalid regex → warning + fallback; interaction with `separateGroupsWithLine` and hot-comments.
- **`tests/RequireUpdater.test.cpp`** — rename module / move module / rename parent dir / move parent dir / move dir with multiple descendants; service-based, `script.Parent`-relative, alias, and `./` requires; different importing files receiving different rewritten paths; alias local names preserved; unrelated strings & comments containing the old path untouched; dynamic (`require(x .. "y")`) and unresolved requires skipped + reported; multi-file `WorkspaceEdit` shape; sourcemap-backed resolution (drive via `loadSourcemap` old → simulate `didRenameFiles` → `loadSourcemap` new, asserting the deferred edit); prompt accept-all / decline / dismiss(cancel) / Always / Never using `TestClient::respondToLastRequest`; `never` config = no request sent; no-applyEdit-capability client → no edit, custom request still works; `luau-lsp/updateRequiresForRename` direct request incl. per-file scope.
- **Stage-0 regression tests (written first, demonstrating current wrong behavior)**: shared file receives server-only suggestion; `RunContext`-style client module in ReplicatedStorage offered to server file; auto-import inserted above `-- Services` heading.

---

## 7. Known limitations

1. **Roblox instance-require updates wait on Rojo.** New virtual paths are only knowable after sourcemap regeneration; the updater defers until the next sourcemap update (bounded timeout) and reports if it never arrives. No prediction of DM paths from fs paths.
2. **The move cannot be cancelled.** `didRenameFiles` is post-hoc. Stage-2 `willRenameFiles` can make edits atomic with the move, but only in `always` mode (no prompting inside willRename) and only where new paths are computable pre-move (standard platform / alias requires).
3. **"Current file only" prompt scope** is not offered as a prompt button (the server does not know editor focus); it is provided via the custom request/code action instead.
4. **"Always"/"Never" prompt persistence** is durable only on VSCode (extension command); other clients get session-scoped persistence.
5. Boundary/visibility rules affect **auto-import suggestions and add-missing-require code actions only** — not diagnostics or type checking (per spec; could be a later opt-in).
6. Glob matching is case-sensitive (upstream `NOCASEGLOB 0`) while URI equality is case-insensitive on Windows/macOS; documented, mitigated by matching workspace-relative paths produced by `lexicallyRelative`.
7. CLI (`luau-lsp analyze`) parses the new fields but they have no effect there (completion-only features).
8. Requires inside `--[[ ]]` comments, strings, or built dynamically are never rewritten (by construction — AST-based).

---

## 8. Upstream synchronization risks

| Area | Risk | Mitigation |
|---|---|---|
| New files | None | — |
| `ClientConfiguration.hpp` appends | Low | Append-only at struct/macro ends |
| `InstanceRequireAutoImporter.cpp`, `RobloxFileResolver.cpp`, `StringRequireAutoImporter.cpp` hooks | **Moderate — this is upstream's most active auto-import area** (the `scriptContext` feature itself is recent) | Keep each hook 1–3 lines delegating into fork-owned files; never duplicate upstream logic |
| `LanguageServer.cpp` dispatch | Low | Append near the end of the chains |
| Protocol headers | Low | Additive structs matching LSP spec naming, plausible upstream contributions |
| `CMakeLists.txt`, `package.json`, `CHANGELOG.md` | Trivial textual conflicts | Mechanical resolution |
| Upstream implements the same features | Real (active repo) | **Recommended: propose sections, config validation, and didRenameFiles/require-updating upstream as PRs** — they are platform-neutral and un-opinionated; keep only the boundary matrix + visibility rules fork-local if upstream declines |

---

## 9. Staged implementation strategy

Each stage is independently shippable and testable; later stages never block earlier ones.

- **Stage 0** — Regression tests demonstrating current incorrect behavior (§6 last bullet).
- **Stage 1** — `AutoImportRules` infra: `ModulePathInfo`, `BoundaryMatcher`, `VisibilityMatcher`, config structs, validation warnings. No behavior change yet.
- **Stage 2** — Boundary filtering wired into the three candidate filters (feature 1).
- **Stage 3** — Visibility rules wired in beside ignore-globs (feature 4).
- **Stage 4** — `ImportSections` detection + insertion adjustment (feature 2).
- **Stage 5** — RequireUpdater core + `luau-lsp/updateRequiresForRename` + `didRenameFiles` handling with `always`/`never` modes; protocol types; TestClient response simulation.
- **Stage 6** — `showMessageRequest` prompt flow, session preferences, sourcemap-deferred Roblox updates, VSCode persistence command, capability gating and fallbacks.
- **Stage 7** — Documentation, settings reference, migration notes (`ignoreGlobs` → visibility rules), CHANGELOG, and a final diff review specifically for upstream merge-conflict surface.

---

## 10. Example behavior

**Feature 1** — with the `src/Client|Server|Shared` rules configured: completing in `src/Shared/util.luau` no longer offers `src/Server/PlayerData.luau` (today it does, because both classify `Shared` / the matrix is permissive). A `ReplicatedStorage/ClientOnly/**` rule makes RunContext-Client modules invisible to server scripts even though they live under ReplicatedStorage.

**Feature 2** — auto-importing `getPlayerId` into the sectioned file from the request produces exactly the expected output: the `game:GetService("ReplicatedStorage")` line lands under `-- Services ----`, the `require` under `-- Modules ----`, sorted among existing entries, no template required; delete the headings and behavior reverts to today's.

**Feature 3** — moving `ReplicatedStorage/Utils/getPlayerId` → `ReplicatedStorage/Players/getPlayerId` in VSCode: after Rojo regenerates the sourcemap, a prompt offers to update 3 requires in 3 files; accepting rewrites `require(ReplicatedStorage.Utils.getPlayerId)` → `require(ReplicatedStorage.Players.getPlayerId)`, `require(script.Parent.Utils.getPlayerId)` → a recomputed relative path, leaves `-- see Utils/getPlayerId` comments untouched, and reports one skipped dynamic require.

**Feature 4** — `{ "scope": "src/Features/*", "modules": "**/Internal/**" }`: `TestFeature2/Server/System.luau` still gets `TestFeature2/Server/Internal/TestUtil`; `TestFeature1/**` and files outside `src/Features` do not; `TestFeature2/Shared/System.luau` also does not (server boundary wins over the visibility grant).
