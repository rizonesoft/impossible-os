// Two companion processes updating one workspace's job state (Impossible OS patch, see ../PATCHES.md).
// A round and a performance review run at once; neither may delete the other's job record or log.
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

import { makeTempDir } from "./helpers.mjs";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const STATE = path.join(ROOT, "scripts", "lib", "state.mjs");

function writer(workspace, dataDir, name, count) {
  const source = `
    import fs from "node:fs";
    import path from "node:path";
    import { upsertJob, resolveJobsDir, writeJobFile } from ${JSON.stringify(STATE)};
    const cwd = ${JSON.stringify(workspace)};
    for (let i = 0; i < ${count}; i++) {
      const id = ${JSON.stringify(name)} + "-" + i;
      const logFile = path.join(resolveJobsDir(cwd), id + ".log");
      writeJobFile(cwd, id, { id });
      fs.writeFileSync(logFile, "log\\n");
      upsertJob(cwd, { id, status: "running", logFile });
      upsertJob(cwd, { id, status: "completed" });
    }
  `;
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, ["--input-type=module", "-e", source], {
      cwd: workspace,
      env: { ...process.env, CLAUDE_PLUGIN_DATA: dataDir },
      stdio: ["ignore", "ignore", "pipe"]
    });
    let stderr = "";
    child.stderr.on("data", (chunk) => (stderr += chunk));
    const timer = setTimeout(() => child.kill("SIGKILL"), 60000);
    child.on("exit", (code) => {
      clearTimeout(timer);
      code === 0 ? resolve() : reject(new Error(`writer ${name} exited ${code}: ${stderr}`));
    });
  });
}

test("two writers keep every job record and log", async () => {
  const workspace = makeTempDir();
  const dataDir = makeTempDir();
  fs.mkdirSync(path.join(workspace, ".git"));
  await Promise.all([writer(workspace, dataDir, "round", 20), writer(workspace, dataDir, "perf", 20)]);
  const stateDir = fs.readdirSync(path.join(dataDir, "state")).map((d) => path.join(dataDir, "state", d))[0];
  const state = JSON.parse(fs.readFileSync(path.join(stateDir, "state.json"), "utf8"));
  const ids = new Set(state.jobs.map((job) => job.id));
  assert.equal(ids.size, 40, `jobs kept: ${ids.size}`);
  for (const id of ids) {
    assert.ok(fs.existsSync(path.join(stateDir, "jobs", `${id}.json`)), `record ${id}`);
    assert.ok(fs.existsSync(path.join(stateDir, "jobs", `${id}.log`)), `log ${id}`);
  }
});
