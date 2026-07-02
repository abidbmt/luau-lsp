# Vendored Luau patches

Patches applied on top of the `luau` submodule (pinned to upstream 0.726,
`86d2a9dc`). The submodule working tree in this checkout carries them as a
local commit; on a fresh clone, re-apply with:

```sh
git -C luau apply ../patches/<patch>.patch
```

## luau-module-dependency-lifetime.patch

Fixes a heap-use-after-free in `Luau::Frontend` (hits both solvers
structurally, observed with `LuauSolverV2`). A module's type graph holds raw
`TypeId`s into the interface arenas of the modules it requires, with no
ownership. A module whose source file is renamed or deleted can never be
re-enqueued for recheck (`Frontend::parseGraph` silently skips deps whose
source is unreadable) but remains resolvable through
`FrontendModuleResolver::getModule` — so when one of its dependencies is
rechecked and replaced, the stale module dangles into the freed arena and
the next consumer that resolves it crashes the server (exit 0xC0000005;
`IterativeTypeVisitor::process` under ASAN).

The patch gives `Module` strong refs (`dependencyModules`) to every module
it pulled types from, in both solvers' require paths. Refs always point at
modules created earlier in time, so no `shared_ptr` cycles.

Repro/verification: `tools/lsp-e2e-folder.mjs` in the companion test repo
(folder rename + sourcemap regen + diagnostics pull) — deterministic UAF
under ASAN before the patch, clean after; 911/911 unit tests pass under
ASAN. See `UPSTREAM_ISSUE_DRAFT.md` for the full writeup.
