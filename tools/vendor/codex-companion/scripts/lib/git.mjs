import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

import { isProbablyText } from "./fs.mjs";
import { formatCommandFailure, runCommand, runCommandChecked } from "./process.mjs";

const MAX_UNTRACKED_BYTES = 24 * 1024;
const DEFAULT_INLINE_DIFF_MAX_FILES = 2;
const DEFAULT_INLINE_DIFF_MAX_BYTES = 256 * 1024;

function git(cwd, args, options = {}) {
  return runCommand("git", args, { cwd, ...options });
}

function gitChecked(cwd, args, options = {}) {
  return runCommandChecked("git", args, { cwd, ...options });
}

function listUniqueFiles(...groups) {
  return [...new Set(groups.flat().filter(Boolean))].sort();
}

function normalizeMaxInlineFiles(value) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < 0) {
    return DEFAULT_INLINE_DIFF_MAX_FILES;
  }
  return Math.floor(parsed);
}

function normalizeMaxInlineDiffBytes(value) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed) || parsed < 0) {
    return DEFAULT_INLINE_DIFF_MAX_BYTES;
  }
  return Math.floor(parsed);
}

function measureGitOutputBytes(cwd, args, maxBytes) {
  const result = git(cwd, args, { maxBuffer: maxBytes + 1 });
  if (result.error && /** @type {NodeJS.ErrnoException} */ (result.error).code === "ENOBUFS") {
    return maxBytes + 1;
  }
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    throw new Error(formatCommandFailure(result));
  }
  return Buffer.byteLength(result.stdout, "utf8");
}

function measureCombinedGitOutputBytes(cwd, argSets, maxBytes) {
  let totalBytes = 0;
  for (const args of argSets) {
    const remainingBytes = maxBytes - totalBytes;
    if (remainingBytes < 0) {
      return maxBytes + 1;
    }
    totalBytes += measureGitOutputBytes(cwd, args, remainingBytes);
    if (totalBytes > maxBytes) {
      return totalBytes;
    }
  }
  return totalBytes;
}

function buildBranchComparison(cwd, baseRef) {
  const mergeBase = gitChecked(cwd, ["merge-base", "HEAD", baseRef]).stdout.trim();
  return {
    mergeBase,
    commitRange: `${mergeBase}..HEAD`,
    reviewRange: `${baseRef}...HEAD`
  };
}

export function ensureGitRepository(cwd) {
  const result = git(cwd, ["rev-parse", "--show-toplevel"]);
  const errorCode = result.error && "code" in result.error ? result.error.code : null;
  if (errorCode === "ENOENT") {
    throw new Error("git is not installed. Install Git and retry.");
  }
  if (result.status !== 0) {
    throw new Error("This command must run inside a Git repository.");
  }
  return withoutNewline(result.stdout);
}

// Impossible OS patch (../../PATCHES.md): only git's own newline is removed. trim() also removed a root's
// trailing spaces, so a worktree named "repair " was reviewed as its sibling "repair".
function withoutNewline(text) {
  return text.endsWith("\n") ? text.slice(0, -1) : text;
}

export function getRepoRoot(cwd) {
  return withoutNewline(gitChecked(cwd, ["rev-parse", "--show-toplevel"]).stdout);
}

export function detectDefaultBranch(cwd) {
  const symbolic = git(cwd, ["symbolic-ref", "refs/remotes/origin/HEAD"]);
  if (symbolic.status === 0) {
    const remoteHead = symbolic.stdout.trim();
    if (remoteHead.startsWith("refs/remotes/origin/")) {
      return remoteHead.replace("refs/remotes/origin/", "");
    }
  }

  const candidates = ["main", "master", "trunk"];
  for (const candidate of candidates) {
    const local = git(cwd, ["show-ref", "--verify", "--quiet", `refs/heads/${candidate}`]);
    if (local.status === 0) {
      return candidate;
    }
    const remote = git(cwd, ["show-ref", "--verify", "--quiet", `refs/remotes/origin/${candidate}`]);
    if (remote.status === 0) {
      return `origin/${candidate}`;
    }
  }

  throw new Error("Unable to detect the repository default branch. Pass --base <ref> or use --scope working-tree.");
}

export function getCurrentBranch(cwd) {
  return gitChecked(cwd, ["branch", "--show-current"]).stdout.trim() || "HEAD";
}

export function getWorkingTreeState(cwd) {
  // Impossible OS patch (../../PATCHES.md): every list is read as the NUL-delimited bytes git printed. A display
  // name git quotes (a tab, a non-ASCII byte) is not the file's path, and a name that is not UTF-8 decodes to the
  // same string as another (U+FFFD), so files are opened and counted by their bytes; decoded names are for display.
  const rawNames = (args) => {
    const listed = spawnSync("git", [...args, "-z"], { cwd, maxBuffer: 1 << 28 });
    if (listed.error || listed.status !== 0) {
      throw listed.error ?? new Error(`git ${args.join(" ")} failed: ${String(listed.stderr ?? "")}`);
    }
    const out = [];
    for (let start = 0; start < listed.stdout.length;) {
      const end = listed.stdout.indexOf(0, start);
      const stop = end === -1 ? listed.stdout.length : end;
      if (stop > start) {
        out.push(listed.stdout.subarray(start, stop));
      }
      start = stop + 1;
    }
    return out;
  };
  const stagedRaw = rawNames(["diff", "--cached", "--name-only"]);
  const unstagedRaw = rawNames(["diff", "--name-only"]);
  const untrackedRaw = rawNames(["ls-files", "--others", "--exclude-standard"]);
  // One reversible display for every name, so distinct files never share a heading: a name is shown as is unless
  // it is not UTF-8, starts with a quote or a byte-order mark, or holds a backslash or a control character; then it
  // is quoted, each byte outside printable ASCII (and the quote and backslash) written \xNN. A plain name never
  // starts with a quote, so a plain name and a quoted one cannot read alike.
  const display = (raw) => {
    let text = null;
    try {
      text = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(raw);
    } catch {
      text = null;
    }
    if (text !== null && !/^["\uFEFF]|[\\\u0000-\u001f\u007f]/.test(text)) {
      return text;
    }
    return `"${Array.from(raw, (byte) => (byte >= 0x20 && byte < 0x7f && byte !== 0x22 && byte !== 0x5c
      ? String.fromCharCode(byte) : `\\x${byte.toString(16).padStart(2, "0")}`)).join("")}"`;
  };
  const staged = stagedRaw.map(display);
  const unstaged = unstagedRaw.map(display);
  const untracked = untrackedRaw.map(display);
  // The number of distinct files, by their bytes: the inline-review gate counts this, never decoded names.
  const changedCount = new Set([...stagedRaw, ...unstagedRaw, ...untrackedRaw].map((raw) => raw.toString("hex"))).size;

  return {
    staged,
    unstaged,
    untracked,
    untrackedRaw,
    changedCount,
    isDirty: staged.length > 0 || unstaged.length > 0 || untracked.length > 0
  };
}

export function resolveReviewTarget(cwd, options = {}) {
  ensureGitRepository(cwd);

  const requestedScope = options.scope ?? "auto";
  const baseRef = options.base ?? null;
  const state = getWorkingTreeState(cwd);
  const supportedScopes = new Set(["auto", "working-tree", "branch"]);

  if (baseRef) {
    return {
      mode: "branch",
      label: `branch diff against ${baseRef}`,
      baseRef,
      explicit: true
    };
  }

  if (requestedScope === "working-tree") {
    return {
      mode: "working-tree",
      label: "working tree diff",
      explicit: true
    };
  }

  if (!supportedScopes.has(requestedScope)) {
    throw new Error(
      `Unsupported review scope "${requestedScope}". Use one of: auto, working-tree, branch, or pass --base <ref>.`
    );
  }

  if (requestedScope === "branch") {
    const detectedBase = detectDefaultBranch(cwd);
    return {
      mode: "branch",
      label: `branch diff against ${detectedBase}`,
      baseRef: detectedBase,
      explicit: true
    };
  }

  if (state.isDirty) {
    return {
      mode: "working-tree",
      label: "working tree diff",
      explicit: false
    };
  }

  const detectedBase = detectDefaultBranch(cwd);
  return {
    mode: "branch",
    label: `branch diff against ${detectedBase}`,
    baseRef: detectedBase,
    explicit: false
  };
}

function formatSection(title, body) {
  return [`## ${title}`, "", body.trim() ? body.trim() : "(none)", ""].join("\n");
}

// Impossible OS patch (../../PATCHES.md): the directory holding the file named by the bytes `raw` under `cwd`,
// reached one component at a time through directory descriptors that follow no link, so a parent swapped for a
// symlink after git listed the file cannot lead the read outside the checkout (the bytes reached the reviewer's
// prompt before the dispatcher's candidate check could see anything). Returns {fd, leaf}; the caller closes fd.
function openParentBeneath(cwd, raw) {
  const parts = [];
  for (let start = 0; start <= raw.length;) {
    const slash = raw.indexOf(0x2f, start);
    const stop = slash === -1 ? raw.length : slash;
    const part = raw.subarray(start, stop);
    if (part.length === 0 || part.equals(Buffer.from("..")) || part.equals(Buffer.from("."))) {
      throw new Error("not a plain relative path");
    }
    parts.push(part);
    start = stop + 1;
    if (slash === -1) {
      break;
    }
  }
  const O = fs.constants;
  let fd = fs.openSync(cwd, O.O_RDONLY | O.O_DIRECTORY);
  try {
    for (const part of parts.slice(0, -1)) {
      const next = fs.openSync(Buffer.concat([Buffer.from(`/proc/self/fd/${fd}/`), part]),
                               O.O_RDONLY | O.O_DIRECTORY | O.O_NOFOLLOW);
      fs.closeSync(fd);
      fd = next;
    }
  } catch (error) {
    fs.closeSync(fd);
    throw error;
  }
  return { fd, leaf: Buffer.concat([Buffer.from(`/proc/self/fd/${fd}/`), parts[parts.length - 1]]) };
}

// Exported for its test (a race between listing and reading cannot be driven through the review entry points).
export function formatUntrackedFile(cwd, relativePath, rawPath = null) {
  const raw = rawPath ?? Buffer.from(relativePath);
  let beneath;
  try {
    beneath = openParentBeneath(cwd, raw);
  } catch {
    return `### ${relativePath}\n(skipped: unreadable file)`;
  }
  let fd = null;
  try {
    let stat;
    try {
      stat = fs.lstatSync(beneath.leaf);
    } catch {
      return `### ${relativePath}\n(skipped: unreadable file)`;
    }
    if (stat.isSymbolicLink()) {
      try {
        return `### ${relativePath}\n(symlink to ${fs.readlinkSync(beneath.leaf)})`;
      } catch {
        return `### ${relativePath}\n(skipped: unreadable symlink)`;
      }
    }
    if (stat.isDirectory()) {
      return `### ${relativePath}\n(skipped: directory)`;
    }
    if (!stat.isFile()) {
      return `### ${relativePath}\n(skipped: not a regular file)`;
    }
    if (stat.size > MAX_UNTRACKED_BYTES) {
      return `### ${relativePath}\n(skipped: ${stat.size} bytes exceeds ${MAX_UNTRACKED_BYTES} byte limit)`;
    }

    let buffer;
    try {
      fd = fs.openSync(beneath.leaf, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
      if (!fs.fstatSync(fd).isFile()) {
        return `### ${relativePath}\n(skipped: not a regular file)`;
      }
      const limit = Buffer.alloc(MAX_UNTRACKED_BYTES + 1);
      const read = fs.readSync(fd, limit, 0, limit.length, 0);
      if (read > MAX_UNTRACKED_BYTES) {
        return `### ${relativePath}\n(skipped: exceeds ${MAX_UNTRACKED_BYTES} byte limit)`;
      }
      buffer = limit.subarray(0, read);
    } catch {
      return `### ${relativePath}\n(skipped: unreadable file)`;
    }
    if (!isProbablyText(buffer)) {
      return `### ${relativePath}\n(skipped: binary file)`;
    }
    return [`### ${relativePath}`, "```", buffer.toString("utf8").trimEnd(), "```"].join("\n");
  } finally {
    if (fd !== null) {
      fs.closeSync(fd);
    }
    fs.closeSync(beneath.fd);
  }
}

function collectWorkingTreeContext(cwd, state, options = {}) {
  const includeDiff = options.includeDiff !== false;
  const changedFiles = listUniqueFiles(state.staged, state.unstaged, state.untracked);

  let parts;
  if (includeDiff) {
    const stagedDiff = gitChecked(cwd, ["diff", "--cached", "--binary", "--no-ext-diff", "--submodule=diff"]).stdout;
    const unstagedDiff = gitChecked(cwd, ["diff", "--binary", "--no-ext-diff", "--submodule=diff"]).stdout;
    const status = gitChecked(cwd, ["status", "--short", "--untracked-files=all"]).stdout.trim();
    const untrackedBody = state.untracked
      .map((file, index) => formatUntrackedFile(cwd, file, state.untrackedRaw?.[index] ?? null))
      .join("\n\n");
    parts = [
      formatSection("Git Status", status),
      formatSection("Staged Diff", stagedDiff),
      formatSection("Unstaged Diff", unstagedDiff),
      formatSection("Untracked Files", untrackedBody)
    ];
  } else {
    const stagedStat = gitChecked(cwd, ["diff", "--shortstat", "--cached"]).stdout.trim();
    const unstagedStat = gitChecked(cwd, ["diff", "--shortstat"]).stdout.trim();
    // Impossible OS patch (../../PATCHES.md): in self-collection mode the reviewer reads the checkout, so the
    // untracked files are counted once and listed on demand, not read into the prompt (128 files of 24 KiB were
    // 3 MiB of input here) nor named in three sections (an import of 3,527 files was about 548 KB).
    const trackedStatus = gitChecked(cwd, ["status", "--short", "--untracked-files=no"]).stdout.trim();
    const untrackedBody = state.untracked.length
      ? `${state.untracked.length} untracked file(s); list them with \`git ls-files --others --exclude-standard -z\` and inspect them in the checkout.`
      : "";
    parts = [
      formatSection("Git Status", trackedStatus),
      formatSection("Staged Diff Stat", stagedStat),
      formatSection("Unstaged Diff Stat", unstagedStat),
      formatSection("Changed Files", listUniqueFiles(state.staged, state.unstaged).join("\n")),
      formatSection("Untracked Files", untrackedBody)
    ];
  }

  return {
    mode: "working-tree",
    summary: `Reviewing ${state.staged.length} staged, ${state.unstaged.length} unstaged, and ${state.untracked.length} untracked file(s).`,
    content: parts.join("\n"),
    changedFiles
  };
}

function collectBranchContext(cwd, baseRef, options = {}) {
  const includeDiff = options.includeDiff !== false;
  const comparison = options.comparison ?? buildBranchComparison(cwd, baseRef);
  const currentBranch = getCurrentBranch(cwd);
  const changedFiles = gitChecked(cwd, ["diff", "--name-only", comparison.commitRange]).stdout.trim().split("\n").filter(Boolean);
  const logOutput = gitChecked(cwd, ["log", "--oneline", "--decorate", comparison.commitRange]).stdout.trim();
  const diffStat = gitChecked(cwd, ["diff", "--stat", comparison.commitRange]).stdout.trim();

  return {
    mode: "branch",
    summary: `Reviewing branch ${currentBranch} against ${baseRef} from merge-base ${comparison.mergeBase}.`,
    content: includeDiff
      ? [
          formatSection("Commit Log", logOutput),
          formatSection("Diff Stat", diffStat),
          formatSection(
            "Branch Diff",
            gitChecked(cwd, ["diff", "--binary", "--no-ext-diff", "--submodule=diff", comparison.commitRange]).stdout
          )
        ].join("\n")
      : [
          formatSection("Commit Log", logOutput),
          formatSection("Diff Stat", diffStat),
          formatSection("Changed Files", changedFiles.join("\n"))
        ].join("\n"),
    changedFiles,
    comparison
  };
}

function buildAdversarialCollectionGuidance(options = {}) {
  if (options.includeDiff !== false) {
    return "Use the repository context below as primary evidence.";
  }

  return "The repository context below is a lightweight summary. Inspect the target diff yourself with read-only git commands before finalizing findings.";
}

export function collectReviewContext(cwd, target, options = {}) {
  const repoRoot = getRepoRoot(cwd);
  const currentBranch = getCurrentBranch(repoRoot);
  const maxInlineFiles = normalizeMaxInlineFiles(options.maxInlineFiles);
  const maxInlineDiffBytes = normalizeMaxInlineDiffBytes(options.maxInlineDiffBytes);
  let details;
  let includeDiff;
  let diffBytes;

  if (target.mode === "working-tree") {
    const state = getWorkingTreeState(repoRoot);
    diffBytes = measureCombinedGitOutputBytes(
      repoRoot,
      [
        ["diff", "--cached", "--binary", "--no-ext-diff", "--submodule=diff"],
        ["diff", "--binary", "--no-ext-diff", "--submodule=diff"]
      ],
      maxInlineDiffBytes
    );
    includeDiff =
      options.includeDiff ??
      (state.changedCount <= maxInlineFiles &&
        diffBytes <= maxInlineDiffBytes);
    details = collectWorkingTreeContext(repoRoot, state, { includeDiff });
  } else {
    const comparison = buildBranchComparison(repoRoot, target.baseRef);
    const fileCount = gitChecked(repoRoot, ["diff", "--name-only", comparison.commitRange]).stdout.trim().split("\n").filter(Boolean).length;
    diffBytes = measureGitOutputBytes(
      repoRoot,
      ["diff", "--binary", "--no-ext-diff", "--submodule=diff", comparison.commitRange],
      maxInlineDiffBytes
    );
    includeDiff = options.includeDiff ?? (fileCount <= maxInlineFiles && diffBytes <= maxInlineDiffBytes);
    details = collectBranchContext(repoRoot, target.baseRef, { includeDiff, comparison });
  }

  return {
    cwd: repoRoot,
    repoRoot,
    branch: currentBranch,
    target,
    fileCount: details.changedFiles.length,
    diffBytes,
    inputMode: includeDiff ? "inline-diff" : "self-collect",
    collectionGuidance: buildAdversarialCollectionGuidance({ includeDiff }),
    ...details
  };
}
