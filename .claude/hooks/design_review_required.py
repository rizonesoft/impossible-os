#!/usr/bin/env python3
# Pre-implementation design-review gate (TODO-07-style protection):
# once a Skill(implement-todo-section, ...) invocation is seen in the
# transcript, the agent MUST dispatch a Codex design review (Bash to
# codex-companion.mjs adversarial-review with the word "design" in the
# prompt) BEFORE editing any source code. The block fires on Edit /
# Write / MultiEdit on non-markdown / non-.claude files.
#
# Why this hook exists: feedback_codex_design_review_mandatory memory
# tracks the recurring pattern of skipping step 4 of implement-todo-
# section because its conditional ("for complex/high-risk sections")
# wording lets me judge the section as "straightforward enough to
# skip". That judgment has been wrong every time -- TODO-07 §11 + §12
# adversarial reviews caught design-shape issues that a pre-code Codex
# pass would have caught for a fraction of the iteration cost. Memory
# alone has not been enough (per feedback_skill_invocation_drift, the
# durable fix is hooks, not skill edits).
#
# Opt-out: SKIP_DESIGN_REVIEW_HOOK=1 env var. Use sparingly --
# legitimate cases: docs-only sections, stamp-only edits, sections
# explicitly classified as no-design-needed (pure constant additions).
#
# TODO-08 §15 fix (2026-04-27):
#   #1 review-kind recognition -- a Codex dispatch whose prompt carries
#      a `[review-kind: adversarial|consistency|perf|re-adversarial|design]`
#      marker counts as design-equivalent. Rationale: review-pipeline
#      fixes responding to Codex findings ARE design-validated work; the
#      design-review-pre-code rule is for *new* implementation, not for
#      fixing a reviewer's findings. Closes the §4 SMBIOS pain where the
#      hook re-fired on review-pipeline edits despite three real review
#      dispatches having just run.
#   #2 commit-clears-gate -- after a section-ship-style commit subject
#      (`review:` / `stamp:` / `docs:` / `todo:` prefix), reset impl_seen
#      so the next implement-todo-section invocation starts a fresh
#      gate window. Rationale: the gate is meant to enforce design-
#      before-code WITHIN one implement-todo-section flow; once that
#      flow ships, the next session's work shouldn't inherit the gate.
#      Closes the §8-after-§7 pain where docs-only §8 left the gate
#      armed and blocked unrelated §4 SMBIOS edits.
import json
import os
import re
import sys


_CACHE_REL = ".claude/state/transcript-scan-cache.json"


def _repo_root() -> str:
    """Best-effort git repo root for cache file placement. Empty
    string if not in a git repo (cache disabled, fall back to full
    rescan)."""
    try:
        import subprocess
        return subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return ""


def _cache_path(root: str) -> str:
    return os.path.join(root, _CACHE_REL) if root else ""


def _cache_load(cache_file: str, transcript_path: str) -> dict:
    """Return the cache entry for transcript_path, or {} if no entry
    or the cache file is missing/corrupt. Cache entry shape:
        {
          "inode": int, "mtime": float, "size": int,
          "last_byte_offset": int,
          "impl_seen": bool, "design_seen": bool, "impl_args": str,
          "pending_clear_ids": {tu_id: true, ...}
        }
    """
    if not cache_file or not transcript_path or not os.path.isfile(cache_file):
        return {}
    try:
        with open(cache_file, "r", encoding="utf-8") as f:
            cache = json.load(f)
    except Exception:
        return {}
    if not isinstance(cache, dict):
        return {}
    entry = cache.get(transcript_path)
    return entry if isinstance(entry, dict) else {}


def _cache_save(cache_file: str, transcript_path: str, entry: dict) -> None:
    """Atomically write the cache entry for transcript_path. Best-
    effort: silently swallow I/O errors -- worst case the next call
    pays a full rescan."""
    if not cache_file or not transcript_path:
        return
    try:
        os.makedirs(os.path.dirname(cache_file), exist_ok=True)
        cache = {}
        if os.path.isfile(cache_file):
            try:
                with open(cache_file, "r", encoding="utf-8") as f:
                    loaded = json.load(f)
                if isinstance(loaded, dict):
                    cache = loaded
            except Exception:
                pass
        cache[transcript_path] = entry
        tmp = cache_file + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(cache, f)
        os.replace(tmp, cache_file)
    except Exception:
        try:
            os.unlink(cache_file + ".tmp")
        except Exception:
            pass


def _stat_signature(path: str):
    """Return (inode, mtime, size) tuple for cache validation. Empty
    tuple if stat fails."""
    try:
        st = os.stat(path)
        return (st.st_ino, st.st_mtime, st.st_size)
    except Exception:
        return ()


def _is_code_target(path: str) -> bool:
    """Return True if path looks like a source-code edit that should
    wait for the design review.

    Allowed without design review (return False here):
      - Markdown files (TODO sections, docs)
      - .claude/ tooling (settings.json, hooks, skills)
      - Build artifacts (build/, build.log)
      - Test runner BAT files (scripts/debug/**/*.bat)

    Everything else (code: .py, .c, .h, .asm, .sh, .ps1, .ts, etc.) is
    a code edit; block until design review fires.
    """
    if not path:
        return False
    norm = path.replace("\\", "/")
    if norm.endswith(".md"):
        return False
    if "/.claude/" in norm or norm.startswith(".claude/"):
        return False
    if "/build/" in norm or norm.startswith("build/"):
        return False
    # Coverage docs are auto-regenerated, not a code edit.
    if "/docs/test-coverage/" in norm:
        return False
    if norm.endswith(".bat") and "/scripts/debug/" in norm:
        return False
    return True


def _scan_transcript(path: str) -> tuple[bool, bool, str]:
    """Walk the session transcript JSONL and return:
      (impl_seen, design_seen_after_impl, impl_args)
    impl_seen          -- a Skill(implement-todo-section, ...) was invoked
    design_seen_after  -- a Bash dispatch to codex-companion.mjs
                          adversarial-review with "design" in the
                          prompt was invoked AFTER impl_seen
    impl_args          -- args of the most recent implement-todo-section
                          invocation (for the error message)

    TODO-08 §16: incremental rescan via offset cache. If a cached state
    exists for `path` and (inode, mtime, size) match-or-grow vs the
    cached values, seek to last_byte_offset and parse only the appended
    lines starting from cached state. Otherwise full rescan from byte
    0. Cache invalidation: PreCompact hook deletes the cache file (the
    semantic window changes on compaction). All errors fall back to
    full rescan -- the cache is a perf optimization, never a
    correctness boundary.
    """
    if not path or not os.path.exists(path):
        return (False, False, "")

    # §16 cache: try incremental scan first.
    root = _repo_root()
    cache_file = _cache_path(root)
    cached = _cache_load(cache_file, path)
    cur_sig = _stat_signature(path)
    start_offset = 0
    impl_seen = False
    design_seen = False
    impl_args = ""
    pending_clear_ids: dict = {}
    can_incremental = (
        cached
        and cur_sig
        and cached.get("inode") == cur_sig[0]
        and cached.get("mtime", 0) <= cur_sig[1]
        and cached.get("size", 0) <= cur_sig[2]
    )
    if can_incremental:
        start_offset = int(cached.get("last_byte_offset", 0) or 0)
        impl_seen = bool(cached.get("impl_seen", False))
        design_seen = bool(cached.get("design_seen", False))
        impl_args = str(cached.get("impl_args", "") or "")
        pcids = cached.get("pending_clear_ids", {}) or {}
        if isinstance(pcids, dict):
            pending_clear_ids = dict(pcids)
    try:
        f = open(path, encoding="utf-8")
    except OSError:
        return (False, False, "")
    with f:
        if start_offset:
            try:
                f.seek(start_offset)
            except Exception:
                # Seek failed -- fall back to full rescan from 0.
                f.seek(0)
                impl_seen = False
                design_seen = False
                impl_args = ""
                pending_clear_ids = {}
        for line in f:
            try:
                ev = json.loads(line)
            except Exception:
                continue
            msg = ev.get("message")
            if not isinstance(msg, dict):
                continue
            content = msg.get("content")
            if not isinstance(content, list):
                continue
            for c in content:
                if not isinstance(c, dict):
                    continue
                ctype = c.get("type")
                if ctype == "tool_use":
                    name = c.get("name") or ""
                    inp = c.get("input") or {}
                    if name == "Skill":
                        skill = inp.get("skill") or inp.get("name") or ""
                        if skill == "implement-todo-section":
                            impl_seen = True
                            # Reset design flag: every fresh implement-
                            # todo-section call starts a new gate window.
                            design_seen = False
                            impl_args = inp.get("args", "")
                    elif name == "Bash" and impl_seen:
                        cmd = (inp.get("command") or "")
                        # explicit "design" keyword OR a [review-kind: ...]
                        # marker in the prompt counts as design-equivalent.
                        # Review-pipeline dispatches ARE design-validation
                        # work for fixes responding to those reviews.
                        if ("codex-companion.mjs" in cmd
                                and "adversarial-review" in cmd
                                and (re.search(r"\bdesign\b", cmd, re.I)
                                     or re.search(
                                         r"\[review-kind:\s*"
                                         r"(?:adversarial|consistency|perf|"
                                         r"re-adversarial|design)\b",
                                         cmd, re.I))):
                            design_seen = True
                        # a section-ship-style git commit candidate:
                        # subject after `-m "..."` inline OR a HEREDOC
                        # body line starting with `review:` / `stamp:`
                        # / `docs:` / `todo:` (covers `docs/x:` forms).
                        # ONLY MARK PENDING -- the actual gate-clear
                        # waits for the matching tool_result with no
                        # error.
                        elif (re.search(r"\bgit\s+commit\b", cmd) and
                              (re.search(
                                  r"-m\s+[\'\"]\s*"
                                  r"(?:review|stamp|docs|todo)[:/]",
                                  cmd) or
                               re.search(
                                   r"<<\s*[\'\"]?[A-Za-z_]\w*[\'\"]?\s*\n"
                                   r"\s*(?:review|stamp|docs|todo)[:/]",
                                   cmd, re.S))):
                            tu_id = c.get("id")
                            if tu_id:
                                pending_clear_ids[tu_id] = True
                elif ctype == "tool_result":
                    tu_id = c.get("tool_use_id")
                    if tu_id and tu_id in pending_clear_ids:
                        # Successful section-ship commit confirms the
                        # gate-clear. is_error == True means the Bash
                        # actually failed (block hook fired, lint
                        # failed, etc.); only clear on success.
                        if not c.get("is_error"):
                            impl_seen = False
                            design_seen = False
                            impl_args = ""
                        # Either way, drop the pending entry.
                        pending_clear_ids.pop(tu_id, None)
        end_offset = f.tell()
    # §16 cache: persist post-scan state. Re-stat the file to pin the
    # mtime/size/inode to the bytes we actually consumed (not the pre-
    # scan stat above, which can race against an in-flight write).
    final_sig = _stat_signature(path)
    if final_sig and cache_file:
        _cache_save(cache_file, path, {
            "inode": final_sig[0],
            "mtime": final_sig[1],
            "size": final_sig[2],
            "last_byte_offset": end_offset,
            "impl_seen": impl_seen,
            "design_seen": design_seen,
            "impl_args": impl_args,
            "pending_clear_ids": pending_clear_ids,
        })
    return (impl_seen, design_seen, impl_args)


def main() -> int:
    if os.environ.get("SKIP_DESIGN_REVIEW_HOOK", "") == "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    tn = d.get("tool_name", "")
    if tn not in ("Edit", "Write", "MultiEdit"):
        return 0
    ti = d.get("tool_input", {}) or {}
    target = ti.get("file_path") or ti.get("path") or ""
    if not _is_code_target(target):
        return 0

    transcript = d.get("transcript_path", "")
    impl_seen, design_seen, impl_args = _scan_transcript(transcript)
    if not impl_seen:
        return 0
    if design_seen:
        return 0

    sys.stderr.write(
        "[codex-design-review REQUIRED -- pre-code gate] "
        "implement-todo-section was invoked (args: " + impl_args[:120] + ") "
        "but no Codex design dispatch has been seen yet. Step 4 of "
        "implement-todo-section requires a design pass BEFORE editing "
        "any source code -- target " + target + " is a code edit. "
        "Required: dispatch via the Codex plugin BEFORE this Edit/Write, e.g.\n"
        "  node $HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs adversarial-review \"<design review prompt covering the planned section>\"\n"
        "The prompt must contain the word 'design' for this hook to "
        "recognize it as a design pass. After Codex returns, walk every "
        "finding via the receiving-code-review pattern (verify, Fix / "
        "Reject / Accept), then proceed to code. "
        "Allowed while gated: Read / Grep / Glob (info gathering); "
        "Edits to .md / .claude/ / .bat / docs/test-coverage/ files. "
        "Opt-out for legitimate false positives (docs-only sections, "
        "stamp-only edits, pure constant additions): SKIP_DESIGN_REVIEW_HOOK=1 "
        "env var on the next tool call. "
        "Canonical rule: feedback_codex_design_review_mandatory memory + "
        "implement-todo-section SKILL.md step 4."
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
