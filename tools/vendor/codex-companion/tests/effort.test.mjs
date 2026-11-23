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

function repoWithChange() {
  const repo = makeTempDir();
  const binDir = makeTempDir();
  installFakeCodex(binDir);
  initGitRepo(repo);
  fs.mkdirSync(path.join(repo, "src"));
  fs.writeFileSync(path.join(repo, "src", "app.js"), "export const value = items[0];\n");
  run("git", ["add", "src/app.js"], { cwd: repo });
  run("git", ["commit", "-m", "init"], { cwd: repo });
  fs.writeFileSync(path.join(repo, "src", "app.js"), "export const value = items[0].id;\n");
  return { repo, binDir, statePath: path.join(binDir, "fake-codex-state.json") };
}

test("adversarial review forwards reasoning effort to turn/start", () => {
  const { repo, binDir, statePath } = repoWithChange();
  const result = run("node", [SCRIPT, "adversarial-review", "--effort", "high", "check the guard"], {
    cwd: repo,
    env: buildEnv(binDir)
  });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.effort, "high");
});

test("adversarial review without --effort sends null", () => {
  const { repo, binDir, statePath } = repoWithChange();
  const result = run("node", [SCRIPT, "adversarial-review", "check the guard"], {
    cwd: repo,
    env: buildEnv(binDir)
  });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(JSON.parse(fs.readFileSync(statePath, "utf8")).lastTurnStart.effort, null);
});

test("adversarial review rejects an unknown effort", () => {
  const { repo, binDir, statePath } = repoWithChange();
  const result = run("node", [SCRIPT, "adversarial-review", "--effort", "extreme", "check the guard"], {
    cwd: repo,
    env: buildEnv(binDir)
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /Unsupported reasoning effort/);
  const state = fs.existsSync(statePath) ? JSON.parse(fs.readFileSync(statePath, "utf8")) : {};
  assert.equal(state.lastTurnStart, undefined);
});

test("native review refuses --effort it cannot apply", () => {
  const { repo, binDir } = repoWithChange();
  const result = run("node", [SCRIPT, "review", "--effort", "high"], {
    cwd: repo,
    env: buildEnv(binDir)
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /--effort/);
  assert.doesNotMatch(result.stdout, /Review|review mode/i);
});
