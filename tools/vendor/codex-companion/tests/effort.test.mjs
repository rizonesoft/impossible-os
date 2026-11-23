// Reasoning effort on adversarial-review (Impossible OS patch, see ../PATCHES.md).
// Same fixture and shape as upstream tests/runtime.test.mjs "task forwards model selection and reasoning effort".
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";

import { buildEnv, installFakeCodex } from "./fake-codex-fixture.mjs";
import { initGitRepo, makeTempDir, run } from "./helpers.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const SCRIPT = path.join(ROOT, "scripts", "codex-companion.mjs");

// Each test owns its plugin data (so its broker record), inherits no broker endpoint, and stops every broker it
// started, with that broker's whole process group, before it ends.
function isolatedEnv(binDir, dataDir) {
  const env = { ...buildEnv(binDir), CLAUDE_PLUGIN_DATA: dataDir };
  for (const key of ["CODEX_COMPANION_APP_SERVER_ENDPOINT", "CODEX_COMPANION_APP_SERVER_PID_FILE",
                     "CODEX_COMPANION_APP_SERVER_LOG_FILE", "CODEX_COMPANION_SESSION_ID"]) {
    delete env[key];
  }
  return env;
}

function alive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

function stopBrokers(dataDir) {
  const stateRoot = path.join(dataDir, "state");
  const pids = [];
  for (const dir of fs.existsSync(stateRoot) ? fs.readdirSync(stateRoot) : []) {
    const record = path.join(stateRoot, dir, "broker.json");
    if (!fs.existsSync(record)) {
      continue;
    }
    const pid = JSON.parse(fs.readFileSync(record, "utf8")).pid;
    if (Number.isInteger(pid) && pid > 0) {
      pids.push(pid);
      try {
        process.kill(-pid, "SIGKILL");     // the broker is detached: its group holds it and its app-server
      } catch {
        // already gone
      }
    }
  }
  return pids;
}

function repoWithChange(t) {
  const repo = makeTempDir();
  const binDir = makeTempDir();
  const dataDir = makeTempDir();
  t.after(() => {
    for (const pid of stopBrokers(dataDir)) {
      const until = Date.now() + 5000;
      while (alive(pid) && Date.now() < until) {
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 20);
      }
      assert.equal(alive(pid), false, `broker ${pid} outlived the test`);
    }
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
