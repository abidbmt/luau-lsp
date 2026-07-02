// End-to-end test of the luau-lsp fork against a real Roblox workspace (by default the
// game-template-single repo). Drives the real server binary over LSP stdio, exactly like the
// VSCode extension does: same binary, same settings, same sourcemap. Asserts on completion
// results and the require-update-on-rename flow.
//
// Usage: node e2e/lsp-e2e.mjs
//   E2E_WORKSPACE    overrides the workspace repo path
//   LUAU_LSP_SERVER  overrides the server binary path
// Requires: rojo on PATH, the luau-lsp extension's downloaded Roblox type definitions, and
// the fork features configured in the workspace's .vscode/settings.json (visibility rules,
// boundaries, sections, updateRequiresOnMove). The SecretTest visibility fixture
// (src/Libraries/Shared/Internal/SecretTest.luau) is created for the run and removed after.
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(process.env.E2E_WORKSPACE ?? "C:/Users/User/Programming/Roblox/game-template-single");
const SERVER = process.env.LUAU_LSP_SERVER ?? path.resolve(HERE, "../build/Release/luau-lsp.exe");
const STORAGE = (process.env.APPDATA ?? "") + "/Code/User/globalStorage/johnnymorganz.luau-lsp";
const SCRATCH = fs.mkdtempSync(path.join(os.tmpdir(), "lsp-e2e-"));

const regenSourcemap = () =>
  execFileSync("rojo", ["sourcemap", "default.project.json", "-o", "sourcemap.json", "--include-non-scripts"], { cwd: ROOT });

// ---------- SecretTest visibility fixture (created for the run, removed afterwards) ----------
const FIXTURE = path.join(ROOT, "src/Libraries/Shared/Internal/SecretTest.luau");
const createdFixture = !fs.existsSync(FIXTURE);
if (createdFixture) {
  fs.mkdirSync(path.dirname(FIXTURE), { recursive: true });
  fs.writeFileSync(FIXTURE, `-- Disposable module for testing the luau-lsp fork's visibility rules
const SecretTest = {}

function SecretTest.greet(): string
\treturn "hello from Internal"
end

return table.freeze(SecretTest)
`);
  regenSourcemap();
}

// ---------- settings: reuse the workspace's real .vscode/settings.json ----------
const rawSettings = fs.readFileSync(path.join(ROOT, ".vscode/settings.json"), "utf8");
const allSettings = JSON.parse(rawSettings.replace(/,(\s*[}\]])/g, "$1")); // strip JSONC trailing commas
const lspSettings = Object.fromEntries(Object.entries(allSettings).filter(([k]) => k.startsWith("luau-lsp.")));
const settingsPath = path.join(SCRATCH, "e2e-settings.json");
fs.writeFileSync(settingsPath, JSON.stringify(lspSettings, null, 2));

// Nested form, in case the server asks via workspace/configuration
const nested = {};
for (const [key, value] of Object.entries(lspSettings)) {
  const parts = key.split(".").slice(1);
  let cur = nested;
  for (let i = 0; i < parts.length - 1; i++) cur = cur[parts[i]] ??= {};
  cur[parts[parts.length - 1]] = value;
}

const fileUri = (p) => "file:///" + p.replace(/\\/g, "/").replace(/^([A-Za-z]):/, (_, d) => d.toLowerCase() + "%3A");

// ---------- LSP plumbing ----------
const server = spawn(SERVER, [
  "lsp",
  "--stdio",
  `--definitions:@roblox=${STORAGE}/globalTypes.PluginSecurity.d.luau`,
  `--docs=${STORAGE}/api-docs.json`,
  `--settings=${settingsPath}`,
], { cwd: ROOT });

const stderrLog = [];
server.stderr.on("data", (d) => stderrLog.push(d.toString()));
server.on("exit", (code) => {
  if (!finished) {
    console.error(`server exited early with code ${code}\n${stderrLog.join("")}`);
    process.exit(2);
  }
});
let finished = false;

let nextId = 1;
const pending = new Map();
const serverLog = [];
const appliedEdits = [];
const prompts = [];

function send(msg) {
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

const DEBUG = process.env.E2E_DEBUG === "1";
function handle(msg) {
  if (DEBUG) {
    const tag = msg.method ?? (msg.error ? `ERROR(${msg.error.message})` : "response");
    console.error(`<< id=${msg.id ?? "-"} ${tag}${msg.method === "$/logTrace" || msg.method === "window/logMessage" ? ": " + msg.params.message : ""}`);
  }
  if (msg.method !== undefined && msg.id !== undefined) {
    // server -> client request
    let result = null;
    if (msg.method === "workspace/configuration") result = msg.params.items.map(() => nested);
    else if (msg.method === "workspace/applyEdit") {
      appliedEdits.push(msg.params);
      result = { applied: true };
    } else if (msg.method === "window/showMessageRequest") {
      prompts.push(msg.params.message);
      // Accept the "Update requires" prompt like a user clicking the button
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

// ---------- test helpers ----------
const PROBE = "local __probe = ";
async function completionItems(relPath) {
  const abs = path.join(ROOT, relPath);
  const lines = fs.readFileSync(abs, "utf8").split("\n");
  let insertLine = lines.length;
  for (let i = lines.length - 1; i >= 0; i--) {
    if (/^return\b/.test(lines[i])) { insertLine = i; break; }
  }
  const text = [...lines.slice(0, insertLine), PROBE, ...lines.slice(insertLine)].join("\n");
  const uri = fileUri(abs);
  notify("textDocument/didOpen", { textDocument: { uri, languageId: "luau", version: 1, text } });
  const result = await request("textDocument/completion", {
    textDocument: { uri },
    position: { line: insertLine, character: PROBE.length },
  });
  notify("textDocument/didClose", { textDocument: { uri } });
  const items = Array.isArray(result) ? result : result?.items ?? [];
  if (DEBUG) fs.writeFileSync(path.join(SCRATCH, `items-${path.basename(path.dirname(relPath))}-${path.basename(relPath)}.json`), JSON.stringify(items, null, 1));
  return items;
}

async function waitFor(predicate, what, timeoutMs = 30000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    if (predicate()) return;
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`timeout waiting for ${what}`);
}
const findImport = (items, label) => items.find((i) => i.label === label && i.additionalTextEdits?.length);

const results = [];
function check(name, ok, detail = "") {
  results.push({ name, ok, detail });
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? `  (${detail})` : ""}`);
}

// ---------- run ----------
try {
  await request("initialize", {
    processId: process.pid,
    rootUri: fileUri(ROOT),
    workspaceFolders: [{ uri: fileUri(ROOT), name: path.basename(ROOT) }],
    capabilities: {
      workspace: { applyEdit: true },
      textDocument: {
        completion: { completionItem: { labelDetailsSupport: true } },
        // pull-diagnostics support, like VSCode: without it the server pushes workspace
        // diagnostics during lazy workspace init and aborts on its own isReady guard
        diagnostic: { relatedDocumentSupport: false },
      },
    },
    trace: "verbose",
  });
  notify("initialized", {});
  notify("workspace/didChangeConfiguration", { settings: nested });

  // 1+2+3: visibility rules — SecretTest lives in src/Libraries/Shared/Internal/
  const inLibrary = await completionItems("src/Libraries/Shared/Framework/init.luau");
  check("visibility: SecretTest offered inside src/Libraries/Shared", !!findImport(inLibrary, "SecretTest"),
    `items=${inLibrary.length}`);

  const inDeath = await completionItems("src/Features/Death/Server/System.luau");
  check("visibility: SecretTest hidden from src/Features/Death", !findImport(inDeath, "SecretTest"));

  const inLogger = await completionItems("src/Features/EventLoggers/PlayerLogger/Shared/System.luau");
  check("visibility: SecretTest hidden from src/Features/EventLoggers", !findImport(inLogger, "SecretTest"));

  // 4+5+6: boundaries — LOCAL_PLAYER is client (src/**/Client/**)
  const inClient = await completionItems("src/Features/Cleanup/Client/System.luau");
  check("boundaries: LOCAL_PLAYER (client) offered to client file", !!findImport(inClient, "LOCAL_PLAYER"),
    `items=${inClient.length}`);
  check("boundaries: LOCAL_PLAYER (client) hidden from shared file", !findImport(inLogger, "LOCAL_PLAYER"));
  check("boundaries: LOCAL_PLAYER (client) hidden from server file", !findImport(inDeath, "LOCAL_PLAYER"));
  check("boundaries: Promise (shared) still offered to server file", !!findImport(inDeath, "Promise"));

  // 7: sections — insertion positions in Death/Server/System.luau
  // Services block: lines 1-3 (after "-- Services" heading), Modules block: lines 5-9
  const signal = findImport(inDeath, "Signal");
  const signalEdit = signal?.additionalTextEdits?.find((e) => e.newText.includes("require"));
  check("sections: Signal require inserted under -- Modules heading",
    !!signalEdit && signalEdit.range.start.line >= 5 && signalEdit.range.start.line <= 10,
    signalEdit ? `line=${signalEdit.range.start.line}` : "no edit");

  // NOTE: modules named System.luau (e.g. SpawnSystem) are excluded by the workspace's own
  // completion.imports.ignoreGlobs, so service insertion is tested via a service auto-import
  const runService = findImport(inDeath, "RunService");
  const serviceEdit = runService?.additionalTextEdits?.find((e) => e.newText.includes("GetService"));
  check("sections: new service inserted under -- Services heading",
    !!serviceEdit && serviceEdit.range.start.line >= 1 && serviceEdit.range.start.line <= 4,
    serviceEdit ? `line=${serviceEdit.range.start.line}` : "no edit");

  // 8: rename — real flow: rename on disk, didRenameFiles (defers), sourcemap regen +
  // didChangeWatchedFiles (retriggers), prompt accepted, workspace/applyEdit captured
  const oldAbs = path.join(ROOT, "src/Utils/Shared/getPlayerId.luau");
  const newAbs = path.join(ROOT, "src/Utils/Shared/getUserId.luau");
  try {
    fs.renameSync(oldAbs, newAbs);
    notify("workspace/didRenameFiles", { files: [{ oldUri: fileUri(oldAbs), newUri: fileUri(newAbs) }] });
    regenSourcemap();
    notify("workspace/didChangeWatchedFiles", {
      changes: [{ uri: fileUri(path.join(ROOT, "sourcemap.json")), type: 2 }],
    });
    await waitFor(() => appliedEdits.length > 0, "workspace/applyEdit after rename");

    check("rename: prompt offered before applying", prompts.some((p) => p.includes("Update")),
      prompts[prompts.length - 1] ?? "no prompt");
    const changes = appliedEdits[0]?.edit?.changes ?? {};
    const files = Object.keys(changes);
    const allEdits = files.flatMap((f) => changes[f]);
    check("rename: requires + module-named variables updated in 7 consuming files",
      files.length === 7 && allEdits.every((e) => e.newText.includes("getUserId")) &&
        files.every((f) => changes[f].some((e) => e.newText === "getUserId")),
      `files=${files.length}, edits=${allEdits.length}`);
  } finally {
    if (fs.existsSync(newAbs)) fs.renameSync(newAbs, oldAbs);
    regenSourcemap();
  }

  try { await request("shutdown", null, 5000); notify("exit"); } catch { /* best effort */ }
} catch (err) {
  console.error("harness error:", err.message);
  console.error("--- recent server log ---\n" + serverLog.slice(-15).join("\n"));
  console.error("--- stderr ---\n" + stderrLog.slice(-20).join(""));
  process.exitCode = 2;
} finally {
  finished = true;
  server.kill();
  if (createdFixture) {
    fs.rmSync(FIXTURE, { force: true });
    try { fs.rmdirSync(path.dirname(FIXTURE)); } catch { /* not empty: leave it */ }
    try { regenSourcemap(); } catch (e) { console.error("sourcemap regeneration failed during cleanup:", e.message); }
  }
}

const failed = results.filter((r) => !r.ok);
console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
if (failed.length) {
  console.log("--- recent server log ---\n" + serverLog.slice(-15).join("\n"));
  process.exitCode = 1;
}
