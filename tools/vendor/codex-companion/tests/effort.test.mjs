// Reasoning effort on adversarial-review (Impossible OS patch, see ../PATCHES.md).
// Same fixture and shape as upstream tests/runtime.test.mjs "task forwards model selection and reasoning effort".
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

import { buildEnv, installFakeCodex } from "./fake-codex-fixture.mjs";
import { initGitRepo, makeTempDir, run } from "./helpers.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const SCRIPT = path.join(ROOT, "scripts", "codex-companion.mjs");

// Each test owns its plugin data, inherits no broker endpoint, and stops every process carrying that plugin data
// (the detached broker and its app-server, published or not) before it ends.
function isolatedEnv(binDir, dataDir) {
  const env = { ...buildEnv(binDir), CLAUDE_PLUGIN_DATA: dataDir };
  for (const key of ["CODEX_COMPANION_APP_SERVER_ENDPOINT", "CODEX_COMPANION_APP_SERVER_PID_FILE",
                     "CODEX_COMPANION_APP_SERVER_LOG_FILE", "CODEX_COMPANION_SESSION_ID"]) {
    delete env[key];
  }
  return env;
}

function running(pid) {
  try {
    return fs.readFileSync(`/proc/${pid}/stat`, "utf8").split(") ").pop()[0] !== "Z";
  } catch {
    return false;
  }
}

// Every process of this test's plugin data: the companion's detached broker and its app-server inherit
// CLAUDE_PLUGIN_DATA, so they are found by it whether or not the broker ever published broker.json.
function marked(dataDir) {
  const want = `CLAUDE_PLUGIN_DATA=${dataDir}`;
  const pids = [];
  for (const name of fs.readdirSync("/proc")) {
    if (!/^\d+$/.test(name) || Number(name) === process.pid) {
      continue;
    }
    try {
      if (fs.readFileSync(`/proc/${name}/environ`, "utf8").split("\0").includes(want)) {
        pids.push(Number(name));
      }
    } catch {
      // exited, or not ours to read
    }
  }
  return pids;
}

function stopMarked(dataDir) {
  const pids = marked(dataDir);
  for (const pid of pids) {
    try {
      process.kill(pid, "SIGKILL");
    } catch {
      // already gone
    }
  }
  return pids;
}

function repoWithChange(t) {
  const repo = makeTempDir();
  const binDir = makeTempDir();
  const dataDir = makeTempDir();
  t.after(() => {
    for (const pid of stopMarked(dataDir)) {
      const until = Date.now() + 5000;
      while (running(pid) && Date.now() < until) {
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 20);
      }
      assert.equal(running(pid), false, `process ${pid} of this test outlived it`);
    }
    assert.deepEqual(marked(dataDir).filter(running), [], "a process of this test outlived it");
  });
  installFakeCodex(binDir);
  initGitRepo(repo);
  fs.mkdirSync(path.join(repo, "src"));
  fs.writeFileSync(path.join(repo, "src", "app.js"), "export const value = items[0];\n");
  run("git", ["add", "src/app.js"], { cwd: repo });
  run("git", ["commit", "-m", "init"], { cwd: repo });
  fs.writeFileSync(path.join(repo, "src", "app.js"), "export const value = items[0].id;\n");
  return { repo, env: isolatedEnv(binDir, dataDir), statePath: path.join(binDir, "fake-codex-state.json") };
}

test("adversarial review forwards reasoning effort to turn/start", (t) => {
  const { repo, env, statePath } = repoWithChange(t);
  const result = run("node", [SCRIPT, "adversarial-review", "--effort", "high", "check the guard"], {
    cwd: repo,
    env
  });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.effort, "high");
});

test("adversarial review without --effort sends null", (t) => {
  const { repo, env, statePath } = repoWithChange(t);
  const result = run("node", [SCRIPT, "adversarial-review", "check the guard"], {
    cwd: repo,
    env
  });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.effort, null);
});

test("adversarial review rejects an unknown effort", (t) => {
  const { repo, env, statePath } = repoWithChange(t);
  const result = run("node", [SCRIPT, "adversarial-review", "--effort", "extreme", "check the guard"], {
    cwd: repo,
    env
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /Unsupported reasoning effort/);
  const state = fs.existsSync(statePath) ? JSON.parse(fs.readFileSync(statePath, "utf8")) : {};
  assert.equal(state.lastTurnStart, undefined);
});

test("native review refuses --effort it cannot apply", (t) => {
  const { repo, env } = repoWithChange(t);
  const result = run("node", [SCRIPT, "review", "--effort", "high"], {
    cwd: repo,
    env
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /--effort/);
  assert.doesNotMatch(result.stdout, /Review|review mode/i);
});


test("a turn whose app-server exits after accepting it fails instead of waiting forever", (t) => {
  const { repo, env, statePath } = repoWithChange(t);
  // The generated fake (this test's temp file, not the fixture) acknowledges turn/start and then exits.
  const binDir = path.dirname(statePath);
  const fake = path.join(binDir, "codex");
  const ack = "send({ id: message.id, result: { turn: buildTurn(turnId) } });";
  const text = fs.readFileSync(fake, "utf8");
  assert.ok(text.includes(ack));
  fs.writeFileSync(fake, text.replace(ack, `${ack} setTimeout(() => process.exit(1), 20); return;`));
  // helpers.run passes no timeout to spawnSync, so this call owns its own: a regression fails here, never hangs.
  // The broker it starts is detached; the t.after teardown above stops it by its plugin data.
  const started = Date.now();
  const result = spawnSync("node", [SCRIPT, "adversarial-review", "check the guard"],
                           { cwd: repo, env, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.equal(result.error?.code, undefined, `the companion did not end by itself: ${result.error?.code}`);
  assert.notEqual(result.status, 0, result.stdout);
  assert.ok(Date.now() - started < 30000, `took ${Date.now() - started} ms`);
  assert.equal(JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.prompt.length > 0, true);
  assert.match(`${result.stdout}${result.stderr}`, /closed|exited/i, "the failure was not the lost connection");
});


test("an untracked symlink to an idle FIFO never blocks the review", (t) => {
  const { repo, env } = repoWithChange(t);
  const fifo = path.join(makeTempDir(), "idle.fifo");
  assert.equal(spawnSync("mkfifo", [fifo]).status, 0);
  fs.symlinkSync(fifo, path.join(repo, "untracked-link"));
  const result = spawnSync("node", [SCRIPT, "adversarial-review", "check the guard"],
                           { cwd: repo, env, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.equal(result.error?.code, undefined, `the review blocked: ${result.error?.code}`);
  assert.equal(result.status, 0, result.stderr);
});


test("an untracked file whose name git quotes still reaches the review with its contents", (t) => {
  const { repo, env, statePath } = repoWithChange(t);
  fs.writeFileSync(path.join(repo, "tab\tname.txt"), "QUOTED-NAME-CONTENT\n");
  const result = spawnSync("node", [SCRIPT, "adversarial-review", "check the guard"],
                           { cwd: repo, env, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.equal(result.status, 0, result.stderr);
  const prompt = JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.prompt;
  assert.match(prompt, /QUOTED-NAME-CONTENT/);
});

test("in self-collection mode untracked files are listed, not read into the prompt", (t) => {
  const { repo, env, statePath } = repoWithChange(t);
  for (const n of [1, 2, 3]) {
    fs.writeFileSync(path.join(repo, `extra-${n}.txt`), `SUMMARY-BODY-${n}\n`);
  }
  const result = spawnSync("node", [SCRIPT, "adversarial-review", "check the guard"],
                           { cwd: repo, env, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.equal(result.status, 0, result.stderr);
  const prompt = JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.prompt;
  assert.match(prompt, /3 untracked file\(s\); list them with `git ls-files --others --exclude-standard -z`/);
  assert.doesNotMatch(prompt, /SUMMARY-BODY-/);
  assert.doesNotMatch(prompt, /extra-1\.txt/);
});


test("two untracked names that decode alike each bring their own contents", (t) => {
  const { env, statePath } = repoWithChange(t);
  const repo = makeTempDir();
  initGitRepo(repo);
  fs.writeFileSync(path.join(repo, "README.md"), "hello\n");
  run("git", ["add", "README.md"], { cwd: repo });
  run("git", ["commit", "-m", "init"], { cwd: repo });
  const bad = Buffer.concat([Buffer.from(repo + "/name-"), Buffer.from([0xff]), Buffer.from(".txt")]);
  fs.writeFileSync(bad, "BYTE-NAME-CONTENT\n");
  fs.writeFileSync(path.join(repo, "name-\uFFFD.txt"), "REPLACEMENT-NAME-CONTENT\n");
  const result = spawnSync("node", [SCRIPT, "adversarial-review", "--scope", "working-tree", "check"],
                           { cwd: repo, env, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.equal(result.status, 0, result.stderr);
  const prompt = JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.prompt;
  assert.match(prompt, /BYTE-NAME-CONTENT/);
  assert.match(prompt, /REPLACEMENT-NAME-CONTENT/);
});


test("three names that decode alike are three files for the inline limit", (t) => {
  const { env, statePath } = repoWithChange(t);
  const repo = makeTempDir();
  initGitRepo(repo);
  fs.writeFileSync(path.join(repo, "README.md"), "hello\n");
  run("git", ["add", "README.md"], { cwd: repo });
  run("git", ["commit", "-m", "init"], { cwd: repo });
  for (const byte of [0x80, 0x81, 0x82]) {
    fs.writeFileSync(Buffer.concat([Buffer.from(repo + "/same-"), Buffer.from([byte])]), `ALIKE-BODY-${byte}\n`);
  }
  const result = spawnSync("node", [SCRIPT, "adversarial-review", "--scope", "working-tree", "check"],
                           { cwd: repo, env, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.equal(result.status, 0, result.stderr);
  const prompt = JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.prompt;
  assert.match(prompt, /3 untracked file\(s\)/);
  assert.doesNotMatch(prompt, /ALIKE-BODY-/);
});

test("a worktree whose name ends in a space is reviewed, not its trimmed sibling", (t) => {
  const { env, statePath } = repoWithChange(t);
  const base = makeTempDir();
  const main = path.join(base, "repair");
  fs.mkdirSync(main);
  initGitRepo(main);
  fs.writeFileSync(path.join(main, "README.md"), "hello\n");
  run("git", ["add", "README.md"], { cwd: main });
  run("git", ["commit", "-m", "init"], { cwd: main });
  const spaced = path.join(base, "repair ");
  run("git", ["worktree", "add", "--detach", spaced], { cwd: main });
  fs.writeFileSync(path.join(spaced, "only-in-spaced.txt"), "SPACED-WORKTREE-BODY\n");
  const result = spawnSync("node", [SCRIPT, "adversarial-review", "--scope", "working-tree", "check"],
                           { cwd: spaced, env, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.equal(result.status, 0, result.stderr);
  const prompt = JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.prompt;
  assert.match(prompt, /SPACED-WORKTREE-BODY/);
});


test("distinct names never share a heading: an escape-looking name, an invalid byte, a byte-order mark", (t) => {
  const { env, statePath } = repoWithChange(t);
  const repo = makeTempDir();
  initGitRepo(repo);
  fs.writeFileSync(path.join(repo, "README.md"), "hello\n");
  run("git", ["add", "README.md"], { cwd: repo });
  run("git", ["commit", "-m", "init"], { cwd: repo });
  fs.writeFileSync(path.join(repo, "n\\xff"), "LITERAL-ESCAPE-BODY\n");
  fs.writeFileSync(Buffer.concat([Buffer.from(repo + "/n"), Buffer.from([0xff])]), "INVALID-BYTE-BODY\n");
  const result = spawnSync("node", [SCRIPT, "adversarial-review", "--scope", "working-tree", "check"],
                           { cwd: repo, env: { ...env }, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.equal(result.status, 0, result.stderr);
  const prompt = JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.prompt;
  const heading = (name) => prompt.split("\n").indexOf(`### ${name}`);
  assert.notEqual(heading('"n\\x5cxff"'), -1, prompt);
  assert.notEqual(heading('"n\\xff"'), -1, prompt);
  const literal = prompt.slice(prompt.indexOf('### "n\\x5cxff"'));
  assert.match(literal.split("### ")[1], /LITERAL-ESCAPE-BODY/);
  const invalid = prompt.slice(prompt.indexOf('### "n\\xff"'));
  assert.match(invalid.split("### ")[1], /INVALID-BYTE-BODY/);
  const bom = Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from("a")]);
  const display = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(bom);
  assert.equal(display.charCodeAt(0), 0xfeff, "the decoder kept the byte-order mark for the quoting rule");
});

test("a working directory whose path cannot be named exactly is refused, never replaced by a sibling", (t) => {
  const { env, statePath } = repoWithChange(t);
  const base = makeTempDir();
  const sibling = path.join(base, "w�");
  fs.mkdirSync(sibling);
  fs.mkdirSync(Buffer.concat([Buffer.from(base + "/w"), Buffer.from([0xff])]));
  const result = spawnSync("bash", ["-c", `cd "$1"/w$'\\xff' && exec node "$2" adversarial-review check`, "_", base, SCRIPT],
                           { env, encoding: "utf8", timeout: 30000, killSignal: "SIGKILL" });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /cannot be named exactly/);
  const turn = fs.existsSync(statePath) ? JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart : undefined;
  assert.equal(turn, undefined, "a turn started in a directory that could not be named");
});
