# Fork maintenance runbook

Operational handbook for maintaining this fork (abidbmt/luau-lsp). Written so that any
maintainer or coding agent can pick up the work with no prior context. Fork *feature*
documentation is in `FORK_FEATURES.md`; the original feature design/implementation notes
are in `FORK_PLAN.md`; upstream's generic architecture/build guide is in `CLAUDE.md`.

Last full verification: 2026-07-06 (synced to upstream @ Luau 0.728, 927/927 tests,
both e2e harnesses green).

## 1. Repo identity and layout

- `origin` = https://github.com/abidbmt/luau-lsp (this fork)
- `upstream` = https://github.com/JohnnyMorganz/luau-lsp
- The fork is upstream + four auto-import/rename features (`FORK_FEATURES.md`, all
  disabled by default) + additional fixes (some fork-only, e.g. require-update bugs) +
  a vendored-Luau patch (`patches/`).
- Companion test workspace: `C:\Users\User\Programming\Roblox\game-template-single`
  (a Roblox game **template** — see §6 for its rules).

## 2. Hard rules (do not violate)

1. **Never commit the `luau` submodule pointer while it references a local-only
   commit.** The use-after-free fix exists only as a local commit in the submodule
   working tree; committing that gitlink would break every fresh clone
   (`git submodule update` would fail on an unfetchable SHA). The committed gitlink
   must always be a public JohnnyMorganz/luau SHA. The portable form of the fix is
   `patches/luau-module-dependency-lifetime.patch` — that file is the source of truth.
2. **Do not push to `origin` without the user's explicit OK** (per-batch is how it has
   worked so far). Never push to `upstream`.
3. **Do not file GitHub issues or PRs on upstream repos** (JohnnyMorganz/luau-lsp or
   luau-lang/luau) — the user files those personally. `UPSTREAM_ISSUE_DRAFT.md` is the
   prepared but unfiled writeup of the Luau UAF (see §8).
4. **Keep the template workspace clean** — no LSP test files, fixtures, or scripts may
   be committed to game-template-single (see §6).
5. New user-facing changes need a `CHANGELOG.md` entry (upstream convention), and new
   settings must be added to BOTH `src/include/LSP/ClientConfiguration.hpp` (incl. the
   NLOHMANN macro) and `editors/code/package.json`.

## 3. Build, test, package (Windows, this machine)

CMake (MSVC 18 BuildTools):

```powershell
& "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" `
  --build build --config Release --target Luau.LanguageServer.CLI Luau.LanguageServer.Test -j 14
```

- Binaries land in `build/Release/`.
- **Run tests from the repo root** (`./build/Release/Luau.LanguageServer.Test.exe`) —
  definitions/testdata load relative to CWD. Useful doctest flags: `--test-case=...`,
  `--list-test-cases`, `--new-solver`, `--fflags=true`.

VSIX (the user runs the fork server through the VSCode extension):

```powershell
Copy-Item build/Release/luau-lsp.exe editors/code/bin/server.exe -Force
cd editors/code
npx @vscode/vsce package --allow-missing-repository --skip-license -o luau-lsp-fork-<version>.vsix
code --install-extension luau-lsp-fork-<version>.vsix --force
```

Same extension ID as the marketplace build, so it replaces it in place. The user must
**reload the VSCode window** afterwards to pick up the new server binary.

ASAN build (used to root-cause the UAF): lives in `build-asan/` (git-ignored).
Configure with `-DCMAKE_CXX_FLAGS="/fsanitize=address /Zi"
-DCMAKE_EXE_LINKER_FLAGS="/DEBUG"` for global instrumentation — the upstream
`LSP_BUILD_ASAN` option's target list is incomplete on MSVC. Copy
`clang_rt.asan_dynamic-x86_64.dll` next to the exe. Reports were kept in
`asan-reports/` (git-ignored).

## 4. Syncing with upstream (runbook)

This is exactly the procedure last performed on 2026-07-06 (Luau 0.726 → 0.728):

1. `git fetch upstream` and review `git log main..upstream/main`. Watch especially for
   "Sync to upstream Luau 0.XXX" commits — those move the submodule.
2. Before touching the submodule, **pin the current local UAF commit**:
   `git -C luau branch uaf-fix-<oldver> <sha>` (it lives on a detached HEAD; without a
   branch it becomes GC-bait). Existing pin: `uaf-fix-726`.
3. `git merge upstream/main`. Fork features were designed for low conflict risk
   (`FORK_PLAN.md`), but watch `src/operations/Completion.cpp`,
   `src/operations/RequireUpdater.cpp` (fork-only file), and the fork's test files.
4. `git submodule update --init luau` (checks out the new upstream Luau SHA), then
   re-apply the patch:
   `git -C luau apply --check ../patches/luau-module-dependency-lifetime.patch` — if
   clean, apply it, `git -C luau add -A`, and commit it locally in the submodule
   (working-tree-only commit; see rule 1). If the patch no longer applies, upstream
   Luau changed that code — check whether the UAF is fixed upstream (see
   `UPSTREAM_ISSUE_DRAFT.md` for the repro) before adapting the patch.
5. Grep fork code for fast flags the Luau bump removed (e.g. `LuauConst2` disappeared
   in 0.728) — stale `ScopedFastFlag` references fail at runtime.
6. Rebuild both targets, run the full suite from the repo root.
7. Run both e2e harnesses (§5), including `E2E_DOUBLE=1` for the folder harness.
8. Only after everything is green: push (with user OK) and rebuild/install the VSIX.

Note on definitions drift: the VSCode extension re-downloads Roblox type definitions
(`globalTypes.*.d.luau`) from upstream's master into
`%APPDATA%/Code/User/globalStorage/johnnymorganz.luau-lsp/`. When upstream migrates
definition syntax (e.g. `declare class` → `declare extern type` in 0.728), an
un-synced fork server can end up parsing definitions newer than its vendored Luau —
sync promptly when upstream bumps Luau.

## 5. End-to-end harnesses (`e2e/`)

Both drive the real `build/Release/luau-lsp.exe` over LSP stdio against the companion
workspace. Overrides: `E2E_WORKSPACE` (workspace path), `LUAU_LSP_SERVER` (binary).
They read fork-feature settings from the workspace's `.vscode/settings.json` and the
Roblox definitions from the VSCode globalStorage path above. Requires `rojo` on PATH.

- `node e2e/lsp-e2e.mjs` — 11 checks covering visibility rules, client/server
  boundaries, section-aware insertion, and file rename (requires + variable renames).
  Creates its `SecretTest.luau` fixture in the workspace on the fly, regenerates the
  sourcemap, and removes the fixture afterwards.
- `node e2e/lsp-e2e-folder.mjs` — folder-rename replay with VSCode-fidelity event
  traffic (watcher storms, didRenameFiles, sourcemap regen). Direction-agnostic
  (PlayersHooks ↔ PlayerssHooks). Env switches:
  - `E2E_DOUBLE=1` — renames A→B, applies the edits to disk, renames back B→A,
    applies those too, then asserts every file is restored **byte-for-byte**. This is
    the strongest regression net for the require-update machinery; run it on every
    sync and every RequireUpdater change.
  - `E2E_FLAGS_FILE` — override fast flags passed to the server.
  - `E2E_STOCK=1` — mode used to reproduce the UAF on unmodified upstream builds
    (skips fork-only steps; exit 0 = crash reproduced).

## 6. The companion workspace (game-template-single)

- It is a **template repo**: never commit LSP test files, fixtures, or tooling there.
  Harness fixtures must be created and removed at runtime (lsp-e2e.mjs does this).
- Its `.vscode/settings.json` is tracked and deliberately carries fork-feature
  settings (visibility/boundaries/sections + `enableNewSolver: true`). The harnesses
  read it — **do not revert it**, even though it shows as modified.
- `sourcemap.json` is git-ignored there; regenerate with rojo when needed.

## 7. Debugging playbooks (hard-won)

**Completion issues** ("X is missing / ranked wrong"): first check whether the server
offers the item at all in that exact context, before suspecting ranking. Probe with a
scratch Node script speaking LSP stdio (didOpen → completion → inspect
label/sortText/kind; pattern preserved in the harnesses). Two server paths matter:
classic (`WorkspaceFolder::completion`) and **fragment autocomplete**
(`completion.enableFragmentAutocomplete`, taken on re-completion in a dirty module) —
always probe both; a second completion in the same file exercises the fragment path.
Fragment results have a **fragment-rooted ancestry** that may lack enclosing blocks —
recompute ancestry from the full source module (`findAncestryAtPositionForAutocomplete`)
when structure matters (see `addReturnBlockCloserKeywords` in
`src/operations/Completion.cpp`). SortText constants are in
`src/include/LSP/Completion.hpp` ("0" best … "9" worst; contextual keywords
else/elseif/until/end always get "0"). VSCode ranks by fuzzy score FIRST and sortText
only as tiebreak — a case-exact identifier match (e.g. typing `En` → `EncodingService`)
legitimately beats a keyword.

**Require-update / rename issues**: Luau's `Frontend` has two traps that shaped
`src/operations/RequireUpdater.cpp`: (a) `Frontend::sourceNodes` **never evicts** — a
rename leaves stale nodes under old module names that can claim physical files in
dedup logic (fixed by ranking moved modules first); (b) a failed `readSource` **erases
the stored sourceModule** (`Frontend::getSourceNode`), so after folder renames a
never-reparsed twin can supply an AST whose ranges don't match current text. Rule
adopted: `computeRequireUpdates` NEVER trusts frontend-stored ASTs — it re-parses the
current text (`parseCurrentText`; `Frontend::parse` is private, hence the mirror).
Folder renames arrive as watcher delete/create storms plus `didRenameFiles`; replay
fidelity matters, use the folder harness. The `E2E_DOUBLE` byte-for-byte round trip
exists because a subtly-wrong edit range can look fine in a single pass.

**Server crashes (0xC0000005) after renames**: almost certainly the vendored-Luau UAF
resurfacing — first check the patch is still applied:
`git -C luau log --oneline -1` should show the "Fix use-after-free" commit on top of
the release sync. `git submodule update` silently drops it (re-apply per §4 step 4).
Full analysis in `patches/README.md` and `UPSTREAM_ISSUE_DRAFT.md`; deterministic
repro via the folder harness under ASAN.

## 8. Current state and open items (as of 2026-07-06)

- Vendored Luau: 0.728 (`ddcea05e`) + local UAF commit on top (working tree only).
  Pin branch for the previous base: `uaf-fix-726`.
- The Luau UAF is filed upstream as
  [luau-lang/luau#2485](https://github.com/luau-lang/luau/issues/2485) (open as of
  2026-07-06). Until it is fixed upstream, every Luau bump must re-apply the patch;
  once a Luau release fixes it, drop the patch and the local submodule commit.
  `UPSTREAM_ISSUE_DRAFT.md` (untracked) was the source of the issue text and can be
  deleted.
- Now removable (kept only as evidence for the issue): `build-asan/`,
  `asan-reports/`, and the `luau-lsp-stock` git worktree (all local/ignored).
- Considered but not done: forking luau-lang/luau on GitHub so the submodule pointer
  could reference a public SHA of the patched commit (would make clones self-contained
  and let the pointer be committed). The `patches/` flow is the chosen alternative.
- Fork-only commit history worth knowing: `0fcff96`-era feature commits (boundaries /
  sections / visibility / require-updates), `0706864` (UAF patch + tests), `1758c58`
  (rename-back starvation), `cafa881` (stale-AST fix + variable renames follow module
  renames), `2cca9ca` (e2e harnesses), `c4429d9` (block-closer keywords after
  `return`), `c015539` (merge of upstream Luau 0.728 sync).
