// Replays a folder rename (PlayersHooks <-> PlayerssHooks) in a real Roblox workspace (by
// default the game-template-single repo) against the real server, with the full event traffic
// VSCode produces (didRenameFiles, watcher events, editor didOpen, diagnostic pulls) and the
// extension's exact fflag handling (live Roblox sync + new solver + overrides). Asserts the
// require-update prompt fires and the workspace edit covers all consumers, including the
// moved folder's own modules.
//
// Starts from the workspace's current consistent state, replays a rename to the other name,
// and restores everything afterwards.
//
// Usage: node e2e/lsp-e2e-folder.mjs
//   E2E_WORKSPACE    overrides the workspace repo path
//   LUAU_LSP_SERVER  overrides the server binary path
//   E2E_FLAGS_FILE   JSON file of fflags to use instead of the extension's flag computation
//   E2E_STOCK=1      stock-server mode: skips the fork-only require-update flow
//                    (didRenameFiles notification, prompt, applyEdit) and instead reports
//                    whether the rename+recheck crashes the server. Exit 0 = ASAN
//                    heap-use-after-free reproduced; exit 1 = no crash / other crash.
//   E2E_DOUBLE=1     after the first rename passes, apply its edits to disk the way VSCode
//                    would (writes + didChange for open docs), then rename BACK and assert
//                    the second edit also covers every consumer - including the moved
//                    folder's own module (regression: a stale frontend source node under
//                    the original module name used to starve it via the physical-file dedup).
//                    Applying the second edit must then restore every file byte-for-byte
//                    (requires AND module-named variables round-trip cleanly)
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(process.env.E2E_WORKSPACE ?? "C:/Users/User/Programming/Roblox/game-template-single");
const STOCK = process.env.E2E_STOCK === "1";
const DOUBLE = process.env.E2E_DOUBLE === "1";
const SERVER = process.env.LUAU_LSP_SERVER ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../build/Release/luau-lsp.exe");
const STORAGE = (process.env.APPDATA ?? "") + "/Code/User/globalStorage/johnnymorganz.luau-lsp";
const SCRATCH = fs.mkdtempSync(path.join(os.tmpdir(), "lsp-e2e-folder-"));
const PROJECT = path.join(ROOT, "default.project.json");

// ---------- settings ----------
const rawSettings = fs.readFileSync(path.join(ROOT, ".vscode/settings.json"), "utf8");
const allSettings = JSON.parse(rawSettings.replace(/,(\s*[}\]])/g, "$1"));
const lspSettings = Object.fromEntries(Object.entries(allSettings).filter(([k]) => k.startsWith("luau-lsp.")));
const settingsPath = path.join(SCRATCH, "e2e-settings.json");
fs.writeFileSync(settingsPath, JSON.stringify(lspSettings, null, 2));
const nested = {};
for (const [key, value] of Object.entries(lspSettings)) {
  const parts = key.split(".").slice(1);
  let cur = nested;
  for (let i = 0; i < parts.length - 1; i++) cur = cur[parts[i]] ??= {};
  cur[parts[parts.length - 1]] = value;
}

const fileUri = (p) => "file:///" + p.replace(/\\/g, "/").replace(/^([A-Za-z]):/, (_, d) => d.toLowerCase() + "%3A");
const regenSourcemap = () =>
  execFileSync("rojo", ["sourcemap", "default.project.json", "-o", "sourcemap.json", "--include-non-scripts"], { cwd: ROOT });

// ---------- detect direction from the current (consistent) repo state ----------
const projectBefore = fs.readFileSync(PROJECT, "utf8");
const CUR = projectBefore.includes("PlayerssHooks") ? "PlayerssHooks" : "PlayersHooks";
const NEXT = CUR === "PlayersHooks" ? "PlayerssHooks" : "PlayersHooks";
const OLD_DIR = path.join(ROOT, "src/Features/EventHooks", CUR);
const NEW_DIR = path.join(ROOT, "src/Features/EventHooks", NEXT);
if (!fs.existsSync(OLD_DIR)) throw new Error(`expected ${OLD_DIR} to exist (project.json says ${CUR})`);
const projectAfter = projectBefore.replaceAll(CUR, NEXT);
const requirePattern = `EventHooks.${CUR}.${CUR}`;
console.log(`replaying rename: ${CUR} -> ${NEXT}`);

const consumerCount = fs.readdirSync(path.join(ROOT, "src"), { recursive: true })
  .filter((f) => f.endsWith(".luau"))
  .filter((f) => fs.readFileSync(path.join(ROOT, "src", f), "utf8").includes(requirePattern)).length;
if (consumerCount === 0) throw new Error(`no consumers of ${requirePattern} found - repo state inconsistent?`);

const sleep = (ms) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);

// Double mode edits files on disk - snapshot everything (under the CUR layout) to restore later
const contentSnapshots = new Map();
for (const f of fs.readdirSync(path.join(ROOT, "src"), { recursive: true }))
  if (f.endsWith(".luau"))
    contentSnapshots.set(path.join(ROOT, "src", f), fs.readFileSync(path.join(ROOT, "src", f), "utf8"));

function restoreState() {
  // A crashed server can briefly hold handles on the renamed folder - retry
  for (let attempt = 0; ; attempt++) {
    try {
      if (fs.existsSync(NEW_DIR)) fs.renameSync(NEW_DIR, OLD_DIR);
      break;
    } catch (e) {
      if (attempt >= 20) {
        console.error(`FAILED to restore folder rename (${e.message}) - repo left at ${NEXT}!`);
        break;
      }
      sleep(500);
    }
  }
  for (const [file, contents] of contentSnapshots)
    try {
      if (fs.readFileSync(file, "utf8") !== contents) fs.writeFileSync(file, contents);
    } catch (e) {
      console.error(`FAILED to restore ${file}: ${e.message}`);
    }
  fs.writeFileSync(PROJECT, projectBefore);
  try {
    regenSourcemap();
  } catch (e) {
    console.error("sourcemap regeneration failed during restore:", e.message);
  }
  console.log(`state restored (${CUR})`);
}

// ---------- LSP text edit application (edits are non-overlapping; apply back-to-front) ----------
function offsetAt(text, pos) {
  let offset = 0;
  for (let line = 0; line < pos.line; line++) {
    const next = text.indexOf("\n", offset);
    if (next === -1) return text.length;
    offset = next + 1;
  }
  return offset + pos.character;
}

function applyTextEdits(text, edits) {
  const sorted = [...edits].sort((a, b) => offsetAt(text, b.range.start) - offsetAt(text, a.range.start));
  for (const e of sorted) text = text.slice(0, offsetAt(text, e.range.start)) + e.newText + text.slice(offsetAt(text, e.range.end));
  return text;
}

// ---------- fflags: mirror the extension (live sync + new solver + overrides) ----------
const FFLAG_KINDS = ["FFlag", "FInt", "DFFlag", "DFInt"];
const fflags = {};
if (process.env.E2E_FLAGS_FILE) {
  Object.assign(fflags, JSON.parse(fs.readFileSync(process.env.E2E_FLAGS_FILE, "utf8")));
  console.log(`using ${Object.keys(fflags).length} fflags from ${process.env.E2E_FLAGS_FILE}`);
} else {
  if (lspSettings["luau-lsp.fflags.sync"] !== false) {
    try {
      const r = await fetch("https://clientsettingscdn.roblox.com/v1/settings/application?applicationName=PCStudioApp");
      const settings = (await r.json()).applicationSettings;
      for (const [name, value] of Object.entries(settings))
        for (const kind of FFLAG_KINDS)
          if (name.startsWith(`${kind}Luau`)) fflags[name.substring(kind.length)] = value;
      console.log(`synced ${Object.keys(fflags).length} live Luau fflags`);
    } catch (e) {
      console.log("fflag sync failed (continuing without):", e.message);
    }
  }
  if (lspSettings["luau-lsp.fflags.enableNewSolver"]) fflags["LuauSolverV2"] = "true";
  for (let [name, value] of Object.entries(lspSettings["luau-lsp.fflags.override"] ?? {})) {
    for (const kind of FFLAG_KINDS) if (name.startsWith(kind)) name = name.substring(kind.length);
    fflags[name] = String(value);
  }
}
const flagsSnapshotPath = path.join(SCRATCH, "e2e-flags.json");
fs.writeFileSync(flagsSnapshotPath, JSON.stringify(fflags, null, 2));
console.log(`flags snapshot: ${flagsSnapshotPath}`);

// ---------- LSP plumbing ----------
const flagArgs = Object.entries(fflags).map(([k, v]) => `--flag:${k}=${v}`);
const server = spawn(SERVER, [
  "lsp",
  "--stdio",
  `--definitions:@roblox=${STORAGE}/globalTypes.PluginSecurity.d.luau`,
  `--docs=${STORAGE}/api-docs.json`,
  `--settings=${settingsPath}`,
  ...flagArgs,
], { cwd: ROOT });

let finished = false;
let crashed = false;
const stderrLog = [];
server.stderr.on("data", (d) => stderrLog.push(d.toString()));
server.on("exit", (code) => {
  if (!finished) {
    crashed = true;
    console.log(`\n*** SERVER EXITED code=${code} (0x${(code >>> 0).toString(16)}) ***`);
    console.log("--- recent server log ---\n" + serverLog.slice(-10).join("\n"));
    const stderrPath = path.join(SCRATCH, "server-stderr.txt");
    fs.writeFileSync(stderrPath, stderrLog.join(""));
    console.log(`--- stderr (full copy: ${stderrPath}) ---\n` + stderrLog.join("").split("\n").slice(-40).join("\n"));
  }
});

let nextId = 1;
const pending = new Map();
const serverLog = [];
const appliedEdits = [];
const prompts = [];

function send(msg) {
  if (server.exitCode !== null) return;
  const body = JSON.stringify({ jsonrpc: "2.0", ...msg });
  server.stdin.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}
const notify = (method, params) => send({ method, params });
function request(method, params, timeoutMs = 60000) {
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`timeout waiting for ${method}`));
    }, timeoutMs);
    pending.set(id, { resolve, reject, timer });
    send({ id, method, params });
  });
}

let buffer = Buffer.alloc(0);
server.stdout.on("data", (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);
  for (;;) {
    const headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd === -1) return;
    const header = buffer.subarray(0, headerEnd).toString();
    const length = Number(/Content-Length: (\d+)/i.exec(header)?.[1]);
    if (buffer.length < headerEnd + 4 + length) return;
    const body = buffer.subarray(headerEnd + 4, headerEnd + 4 + length).toString();
    buffer = buffer.subarray(headerEnd + 4 + length);
    handle(JSON.parse(body));
  }
});

function handle(msg) {
  if (msg.method !== undefined && msg.id !== undefined) {
    let result = null;
    if (msg.method === "workspace/configuration") result = msg.params.items.map(() => nested);
    else if (msg.method === "workspace/applyEdit") {
      appliedEdits.push(msg.params);
      result = { applied: true };
    } else if (msg.method === "window/showMessageRequest") {
      prompts.push(msg.params.message);
      console.log("PROMPT:", msg.params.message);
      result = msg.params.actions?.find((a) => a.title === "Update requires") ?? null;
    }
    send({ id: msg.id, result });
  } else if (msg.id !== undefined) {
    const entry = pending.get(msg.id);
    if (!entry) return;
    pending.delete(msg.id);
    clearTimeout(entry.timer);
    if (msg.error) entry.reject(new Error(`${msg.error.code}: ${msg.error.message}`));
    else entry.resolve(msg.result);
  } else if (msg.method === "window/logMessage" || msg.method === "window/showMessage") {
    serverLog.push(msg.params.message);
  }
}

async function waitFor(predicate, what, timeoutMs = 30000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    if (predicate() || crashed) return;
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`timeout waiting for ${what}`);
}

// ---------- replay ----------
try {
  await request("initialize", {
    processId: process.pid,
    rootUri: fileUri(ROOT),
    workspaceFolders: [{ uri: fileUri(ROOT), name: path.basename(ROOT) }],
    capabilities: {
      workspace: { applyEdit: true },
      textDocument: {
        completion: { completionItem: { labelDetailsSupport: true } },
        diagnostic: { relatedDocumentSupport: false },
      },
    },
  });
  notify("initialized", {});
  notify("workspace/didChangeConfiguration", { settings: nested });

  // Warm up / initialize the workspace like VSCode does with an open file
  const deathAbs = path.join(ROOT, "src/Features/Death/Server/System.luau");
  notify("textDocument/didOpen", {
    textDocument: { uri: fileUri(deathAbs), languageId: "luau", version: 1, text: fs.readFileSync(deathAbs, "utf8") },
  });
  await request("textDocument/completion", { textDocument: { uri: fileUri(deathAbs) }, position: { line: 0, character: 0 } });
  console.log("workspace warmed up");

  // 1. The user renames the folder in the VSCode explorer
  fs.renameSync(OLD_DIR, NEW_DIR);
  if (!STOCK) notify("workspace/didRenameFiles", { files: [{ oldUri: fileUri(OLD_DIR), newUri: fileUri(NEW_DIR) }] });

  // 2. VSCode's file watcher reports the file deletes/creates
  notify("workspace/didChangeWatchedFiles", {
    changes: [
      { uri: fileUri(path.join(OLD_DIR, "Shared/API.luau")), type: 3 },
      { uri: fileUri(path.join(OLD_DIR, "Shared/System.luau")), type: 3 },
      { uri: fileUri(path.join(NEW_DIR, "Shared/API.luau")), type: 1 },
      { uri: fileUri(path.join(NEW_DIR, "Shared/System.luau")), type: 1 },
    ],
  });

  // 3. The user's editor tab follows the rename: didOpen of the moved file at its new path
  const movedSystemAbs = path.join(NEW_DIR, "Shared/System.luau");
  notify("textDocument/didOpen", {
    textDocument: { uri: fileUri(movedSystemAbs), languageId: "luau", version: 1, text: fs.readFileSync(movedSystemAbs, "utf8") },
  });
  await request("textDocument/diagnostic", { textDocument: { uri: fileUri(movedSystemAbs) } }).catch((e) => console.log("diag(moved):", e.message));

  // 4. The user fixes default.project.json; rojo regenerates the sourcemap; the watcher notifies
  fs.writeFileSync(PROJECT, projectAfter);
  regenSourcemap();
  notify("workspace/didChangeWatchedFiles", { changes: [{ uri: fileUri(path.join(ROOT, "sourcemap.json")), type: 2 }] });

  // 5. Like VSCode, react to the server's diagnostics refresh with real diagnostic pulls
  const wsDiag = request("workspace/diagnostic", { previousResultIds: [] }, 120000)
    .then((r) => console.log(`workspace diagnostics: ${r.items?.length ?? 0} reports`))
    .catch((e) => console.log("workspace diagnostics failed:", e.message));
  const docDiag = request("textDocument/diagnostic", { textDocument: { uri: fileUri(deathAbs) } }, 120000)
    .then(() => console.log("document diagnostics ok"))
    .catch((e) => console.log("document diagnostics failed:", e.message));

  if (STOCK) {
    // No require-update feature on stock servers - just let the rename+recheck play out
    // and report whether the server survives it.
    await Promise.allSettled([wsDiag, docDiag]);
    for (const rel of ["src/Features/Cleanup/Client/System.luau", "src/Features/Spawn/Server/System.luau"]) {
      if (crashed) break;
      const abs = path.join(ROOT, rel);
      notify("textDocument/didOpen", {
        textDocument: { uri: fileUri(abs), languageId: "luau", version: 1, text: fs.readFileSync(abs, "utf8") },
      });
      await request("textDocument/diagnostic", { textDocument: { uri: fileUri(abs) } }, 60000).catch((e) => console.log(`diag(${rel}):`, e.message));
    }
    if (!crashed) await new Promise((r) => setTimeout(r, 3000));
    if (crashed && stderrLog.join("").includes("heap-use-after-free")) {
      console.log("\nREPRODUCED  stock server crashed with ASAN heap-use-after-free after the folder rename");
    } else if (crashed) {
      console.log("\nCRASHED  server exited, but stderr has no ASAN use-after-free report (see log above)");
      process.exitCode = 1;
    } else {
      console.log("\nNO CRASH  server survived the rename + recheck in stock mode");
      process.exitCode = 1;
    }
  } else {
    await waitFor(() => appliedEdits.length > 0, "workspace/applyEdit after folder rename", 120000);
    await Promise.allSettled([wsDiag, docDiag]);

    let serverLogCursor = 0;
    const reportEdit = (editParams, expected) => {
      for (const line of serverLog.slice(serverLogCursor).filter((l) => l.includes("Skipped during require update")))
        console.log("SERVER:", line);
      serverLogCursor = serverLog.length;
      const changes = editParams?.edit?.changes ?? {};
      console.log(`\napplyEdit received: ${Object.keys(changes).length} file(s)`);
      for (const [uri, edits] of Object.entries(changes))
        for (const e of edits)
          console.log(`  ${decodeURIComponent(uri).replace(/.*\/src\//, "src/")}:${e.range.start.line + 1}: ${e.newText}`);
      // every consumer names its variable after the module, so each file must get the
      // require rewrite plus at least one bare variable-rename edit
      return Object.keys(changes).length === consumerCount &&
        Object.values(changes).every((edits) =>
          edits.some((e) => e.newText.includes(`${expected}.${expected}`)) &&
          edits.some((e) => e.newText === expected) &&
          edits.every((e) => e.newText.includes(expected)));
    };

    let ok = !crashed && reportEdit(appliedEdits[0], NEXT);
    console.log(ok
      ? `\nPASS  folder rename updated requires and variables in all ${consumerCount} consumers (including the moved one)`
      : `\nFAIL  expected ${consumerCount} files with ${NEXT} requires and variable renames`);

    if (ok && DOUBLE) {
      // ---- VSCode applies the edit: disk writes + didChange for the open documents ----
      const openVersions = new Map([[deathAbs.toLowerCase(), 1], [movedSystemAbs.toLowerCase(), 1]]);
      for (const [uri, edits] of Object.entries(appliedEdits[0].edit.changes)) {
        const p = path.normalize(fileURLToPath(uri));
        const updated = applyTextEdits(fs.readFileSync(p, "utf8"), edits);
        fs.writeFileSync(p, updated);
        if (openVersions.has(p.toLowerCase())) {
          const version = openVersions.get(p.toLowerCase()) + 1;
          openVersions.set(p.toLowerCase(), version);
          notify("textDocument/didChange", {
            textDocument: { uri: fileUri(p), version },
            contentChanges: [{ text: updated }],
          });
        }
        notify("workspace/didChangeWatchedFiles", { changes: [{ uri: fileUri(p), type: 2 }] });
      }
      // let diagnostics re-parse everything, like the editor continuously does
      await request("workspace/diagnostic", { previousResultIds: [] }, 120000).catch(() => {});
      console.log("\npass-1 edits applied to disk; replaying rename back");

      // ---- the user renames the folder BACK in the explorer ----
      fs.renameSync(NEW_DIR, OLD_DIR);
      notify("workspace/didRenameFiles", { files: [{ oldUri: fileUri(NEW_DIR), newUri: fileUri(OLD_DIR) }] });
      notify("workspace/didChangeWatchedFiles", {
        changes: [
          { uri: fileUri(path.join(NEW_DIR, "Shared/API.luau")), type: 3 },
          { uri: fileUri(path.join(NEW_DIR, "Shared/System.luau")), type: 3 },
          { uri: fileUri(path.join(OLD_DIR, "Shared/API.luau")), type: 1 },
          { uri: fileUri(path.join(OLD_DIR, "Shared/System.luau")), type: 1 },
        ],
      });

      // the editor tab follows the rename back
      const restoredSystemAbs = path.join(OLD_DIR, "Shared/System.luau");
      notify("textDocument/didClose", { textDocument: { uri: fileUri(movedSystemAbs) } });
      notify("textDocument/didOpen", {
        textDocument: { uri: fileUri(restoredSystemAbs), languageId: "luau", version: 1, text: fs.readFileSync(restoredSystemAbs, "utf8") },
      });

      // the user restores default.project.json; rojo regenerates; the watcher notifies
      fs.writeFileSync(PROJECT, projectBefore);
      regenSourcemap();
      notify("workspace/didChangeWatchedFiles", { changes: [{ uri: fileUri(path.join(ROOT, "sourcemap.json")), type: 2 }] });
      const wsDiag2 = request("workspace/diagnostic", { previousResultIds: [] }, 120000).catch(() => {});

      await waitFor(() => appliedEdits.length > 1, "workspace/applyEdit after rename back", 120000);
      await wsDiag2;

      const movedCovered = !crashed && Object.keys(appliedEdits[1]?.edit?.changes ?? {}).some(
        (uri) => path.normalize(fileURLToPath(uri)).toLowerCase() === restoredSystemAbs.toLowerCase());
      ok = !crashed && reportEdit(appliedEdits[1], CUR) && movedCovered;
      console.log(ok
        ? `\nPASS  rename back updated all ${consumerCount} consumers, including the moved folder's own module`
        : movedCovered
          ? `\nFAIL  rename back: expected ${consumerCount} files with ${CUR} requires`
          : `\nFAIL  rename back did not update the moved folder's own module (${restoredSystemAbs})`);

      if (ok) {
        // ---- apply the second edit too: the round trip must restore every file byte-for-byte ----
        for (const [uri, edits] of Object.entries(appliedEdits[1].edit.changes)) {
          const p = path.normalize(fileURLToPath(uri));
          fs.writeFileSync(p, applyTextEdits(fs.readFileSync(p, "utf8"), edits));
        }
        const mismatched = [...contentSnapshots].filter(([file, contents]) => fs.readFileSync(file, "utf8") !== contents);
        for (const [file] of mismatched) console.log(`  MISMATCH after round trip: ${file}`);
        ok = mismatched.length === 0;
        console.log(ok
          ? `PASS  round trip restored every file byte-for-byte`
          : `\nFAIL  ${mismatched.length} file(s) differ after the round trip`);
      }
    }

    if (!ok) process.exitCode = 1;
  }

  try { await request("shutdown", null, 3000); notify("exit"); } catch { /* crashed */ }
} catch (err) {
  console.error("harness error:", err.message);
  if (!crashed) console.error("--- recent server log ---\n" + serverLog.slice(-10).join("\n"));
  process.exitCode = 2;
} finally {
  finished = true;
  server.kill();
  restoreState();
}
