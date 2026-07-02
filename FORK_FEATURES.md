# Fork Features

This fork extends [luau-lsp](https://github.com/JohnnyMorganz/luau-lsp) with four features. All of
them are **disabled by default** — with no configuration, behaviour is identical to upstream.

See [FORK_PLAN.md](./FORK_PLAN.md) for the architecture/implementation plan behind these features.

---

## 1. Configurable execution boundaries for auto-imports

Upstream infers client/server/shared purely from Roblox DataModel locations (`ServerScriptService`
→ server, `StarterPlayer` → client, everything else — including all of `ReplicatedStorage` —
shared), and its compatibility check lets *shared* code see *everything*. Modern projects that
place client-only modules in `ReplicatedStorage` (e.g. used by `RunContext = Client` scripts) get
incorrect suggestions.

### Configuration

```jsonc
{
    "luau-lsp.completion.imports.boundaries.rules": [
        { "glob": "src/Client/**", "context": "client" },
        { "glob": "src/Server/**", "context": "server" },
        { "glob": "src/Shared/**", "context": "shared" },
        // DataModel paths also work (with or without the "game/" prefix):
        { "glob": "ReplicatedStorage/ClientOnly/**", "context": "client" }
    ],
    // Optional matrix override (this is the default applied once any rule is configured):
    "luau-lsp.completion.imports.boundaries.allowedImports": {
        "client": ["client", "shared"],
        "server": ["server", "shared"],
        "shared": ["shared"]
    }
}
```

### Semantics

- **Matching**: each glob (gitignore-style, same engine as `ignoreGlobs`) is tested against the
  module's workspace-relative file path **and** (Roblox platform) its DataModel path, both as
  `game/Foo/...` and `Foo/...`. Paths always use forward slashes. A pattern without `/` matches
  the basename only (gitignore semantics).
- **Precedence**: later rules override earlier ones (like gitignore). Write broad rules first,
  specific overrides after.
- **Unmatched modules** keep the platform's heuristic classification (Roblox service locations);
  on the standard platform they are unrestricted.
- **`context: "none"`** explicitly exempts matching modules from boundary filtering (overriding
  the heuristic).
- **Matrix**: with no rules configured, the upstream permissive matrix applies (compatible if
  either side is shared). Once any rule exists, the strict matrix above becomes the default.
  `allowedImports` overrides individual rows and can also be used *without* rules to restrict
  the heuristic classification (e.g. `{ "shared": ["shared"] }` alone stops shared code from
  seeing server/client modules).
- Applies to completion auto-imports **and** the "Add require for..." / "Add all missing
  requires" code actions, for both instance requires and string requires.

## 2. Contextual visibility rules (feature-private modules)

Restrict where certain modules may be auto-imported from, without hiding them globally the way
`completion.imports.ignoreGlobs` does.

### Configuration

```jsonc
{
    "luau-lsp.completion.imports.visibilityRules": [
        // Scoped form: for each concrete directory matching `scope`, modules matching `modules`
        // (relative to that directory) are visible only from files matching `visibleFrom`
        // (relative to that directory; default "**" = anywhere inside it).
        { "scope": "src/Features/*", "modules": "**/Internal/**" },

        // Global form (no scope): workspace-relative patterns
        { "modules": "**/*.spec.luau", "visibleFrom": "tests/**" }
    ]
}
```

With the scoped rule above, `src/Features/A/Server/System.luau` is offered
`src/Features/A/Internal/Util.luau`, but files in `src/Features/B/**` or outside `src/Features`
are not.

### Semantics

- Rules are **restrictions**, not reinclusions: a module matching several rules must satisfy all
  of them (AND). Adding a rule can only narrow visibility. There is no rule-vs-rule precedence.
- `ignoreGlobs` is unchanged and unconditional. **Migration note**: if you previously used
  `"**/Internal/**"` in `ignoreGlobs` to hide internals (and lost same-feature suggestions),
  move the pattern into a scoped visibility rule instead.
- Scope resolution tests the *ancestor prefixes* of a candidate's paths against the scope glob.
  Every ancestor that matches `scope` and under which the candidate matches `modules` is a scope
  directory, and the rule is enforced relative to each of them. No `$1` capture syntax is needed.
  Broad scope globs work: `{ "scope": "src/**", "modules": "**/Internal/**" }` makes any
  `Internal` folder under `src` private to the directory that contains it, at any depth.
- Boundary rules and visibility rules compose as independent AND-ed checks — a visibility grant
  can never bypass an execution boundary (a shared file never sees `Server/Internal` modules of
  its own feature).
- Directory names have no built-in meaning — `Internal` is only special if your rules say so.

## 3. Section-aware auto-import insertion

Insert generated declarations under your file's organisational headings instead of at the first
syntactically valid position.

### Configuration

```jsonc
{
    "luau-lsp.completion.imports.sections.services": "^--\\s*Services",
    "luau-lsp.completion.imports.sections.modules": "^--\\s*Modules"
}
```

With headings `-- Services ----` / `-- Modules ----` present, auto-importing a module produces:

```luau
-- Services -------------------------
local ReplicatedStorage = game:GetService("ReplicatedStorage")

-- Modules --------------------------
local getPlayerId = require(ReplicatedStorage.Utils.getPlayerId)
```

### Semantics

- Patterns are ECMAScript regexes matched against each line (so any number of dashes/whitespace
  can be tolerated by your pattern). The **first matching line wins**; a line claimed by the
  services pattern is not considered for the modules pattern.
- Insertion stays sorted among existing imports inside the section and is clamped to the section
  (a section ends at the next configured heading below it).
- If a pattern is invalid (reported via a warning notification) or matches nothing, the default
  insertion behaviour applies. No file template is required.
- Duplicate-prevention is unchanged. `separateGroupsWithLine` blank-line insertion is suppressed
  for a group when its section heading exists (the headings already separate the groups).
- Applies to completion auto-imports and the service/require code actions.

## 4. Require updates on file move/rename

When files or folders are renamed (VSCode: any rename/move in the explorer), the server computes
updates for all requires that *resolve to* the moved module(s) — no text matching — and offers to
apply them, similar to TypeScript's "update imports on file move".

### Configuration

```jsonc
{
    "luau-lsp.fileOperations.updateRequiresOnMove": "prompt" // "prompt" | "always" | "never"
}
```

### Behaviour

- Style is preserved per require: service-based (`ReplicatedStorage.Utils.M`), `script`-relative,
  `game:GetService("X").Y`, string-relative (`./x`, `../x`, `@self/x`) and string-aliased
  (`@alias/x`) requires are each rewritten in their own style. Alias local variable names are
  never touched (renaming the variable remains a separate symbol rename).
- Cross-service moves add the missing `game:GetService(...)` declaration (honouring section
  configuration).
- Only requires whose **resolved target** matches the move are changed; comments, unrelated
  strings, and dynamic requires are untouched. Requires that cannot be rewritten safely (e.g.
  `WaitForChild` chains, unusual string delimiters) are skipped and reported in the log.
- Folder renames update requires of all affected descendant modules.
- The prompt offers **Update requires / Skip / Always update / Never update**. Dismissing the
  prompt updates nothing. "Always"/"Never" persist for the session on all clients, and are also
  written to your user settings on VSCode (via the `luau-lsp.setUpdateRequiresOnMove` command).
- **Roblox platform**: when the new location isn't in the sourcemap yet, the update waits for the
  next sourcemap regeneration (Rojo) and then prompts. If the sourcemap never regenerates, the
  affected requires are reported as skipped.

### Client/editor requirements and fallbacks

| Requirement | Notes |
|---|---|
| `workspace/didRenameFiles` | Advertised via `fileOperations.didRename` server capability. VSCode sends this automatically. The move has already happened when it arrives — the server never claims to cancel filesystem operations. |
| `window/showMessageRequest` | Used for the prompt. Dismissal = no update. |
| `workspace.applyEdit` client capability | Required to apply edits. Without it, the server only logs a message and never rewrites anything. |
| Fallback for any client | The `luau-lsp/updateRequiresForRename` request (`{ oldUri, newUri, uris? }`) returns the computed `WorkspaceEdit` without applying it. Passing `uris` restricts the scope (e.g. current file only). |

### Known limitations

- `didRenameFiles` arrives after the move; if Rojo regenerates the sourcemap *before* the
  notification is processed, instance-require detection for that rename may miss (string requires
  are unaffected — they are resolved by path arithmetic that tolerates the move).
- A moved file's own `script`-relative requires are updated only once the new location is present
  in the sourcemap; on the standard platform its own relative string requires update immediately.
- "Update only the current file" is not offered as a prompt button (an LSP server does not know
  which editor is focused); use the `luau-lsp/updateRequiresForRename` request with `uris` for
  scoped updates.
- Requires built dynamically or through non-plain instance chains are never rewritten.

---

## Configuration validation

Invalid boundary contexts, missing visibility-rule fields, and invalid section regexes produce
`window/showMessage` warnings when the configuration is loaded, and the offending rule is ignored
(never a hard failure).

## CLI note

`luau-lsp analyze` parses the new settings for config-file compatibility, but they only affect
editor features (completion, code actions, file renames) and have no effect on analysis output.
