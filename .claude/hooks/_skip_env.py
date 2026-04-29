#!/usr/bin/env python3
# block-via: warning-only (HELPER module -- never blocks; pure read)
"""Shared SKIP-env scanner for PreToolUse gates (TODO-08 partial-enforcement).

Every PreToolUse gate that honors a `SKIP_*` (or similar) opt-out env
variable consumes this helper. Two read paths are merged into one
result, with inline-prefix winning over the harness env when both
carry the same key:

  1. Inline env prefix on a Bash command -- `SKIP_FOO=1 git commit ...`,
     `env SKIP_FOO=1 git commit ...`, `sudo SKIP_FOO=1 ...`. Walks past
     wrapper tokens (env / sudo / doas / nice / nohup / timeout /
     ionice / stdbuf / command / exec) before the first non-wrapper /
     non-env token, matching the existing
     `section_commit_gate._scan_inline_env_prefix()` behavior that
     this helper supersedes.
  2. The hook process's `os.environ` -- harness env propagated when
     `fallback_to_environ=True`.

Each consumer hook passes an EXPLICIT key list -- not a prefix --
because prefix matching would accidentally capture suffixed secrets
that share a prefix with the opt-out (e.g. a `SKIP_REVIEW_HOOK_TOKEN`
secret accidentally matched by a `SKIP_REVIEW_HOOK*` prefix). Codex
design review Q1 fix 2026-04-29.

Inline-wins merge order: a key present in both sources resolves to
the inline value. Same-call intent is more specific than ambient
harness env. Documented + tested. Codex design review Q3 fix.

API:
    read_skip_envs(
        cmd: str = "",
        keys: Iterable[str] = (),
        fallback_to_environ: bool = True,
    ) -> dict[str, str]

    Returns `{KEY: VALUE}` for each requested key found in either
    source. Missing keys are absent from the returned dict (callers
    use `.get(KEY) == "1"` or similar to check presence).

Failure modes (all silent; helper exits returning whatever it can):
    - cmd is None / non-str -> treated as "" (inline scan returns {})
    - shlex.ValueError on malformed quoting -> inline scan returns {}
    - keys is empty -> returns {}
    - fallback_to_environ=False AND cmd has no inline env -> {}

Hook category: HELPER (read-only); never raises, never exits non-zero.
"""

import os
import shlex
from typing import Iterable


# Wrapper tokens that may legitimately PRECEDE inline env-var
# assignments. Mirrors section_commit_gate._scan_inline_env_prefix().
_WRAPPERS = frozenset({
    "env", "sudo", "doas", "nice", "nohup",
    "timeout", "ionice", "stdbuf", "command", "exec",
})


def _scan_inline(cmd: str, keys: Iterable[str]) -> dict[str, str]:
    """Walk the leading env-prefix of `cmd` and return matched keys.

    Stops at the first non-wrapper, non-env-assignment token. Returns
    {} on any parse failure or when no requested key is found.
    """
    if not isinstance(cmd, str) or not cmd:
        return {}
    key_set = set(keys)
    if not key_set:
        return {}
    # Cheap pre-filter: if NONE of the requested keys appear as a
    # substring of the command, skip the shlex tokenization entirely
    # (saves cycles on the common path of plain `git commit` etc.).
    if not any(k in cmd for k in key_set):
        return {}
    try:
        toks = shlex.split(cmd, posix=True, comments=False)
    except ValueError:
        return {}
    out: dict[str, str] = {}
    for tok in toks:
        if "=" in tok and not tok.startswith("="):
            head, val = tok.split("=", 1)
            # Validate KEY syntax: starts with letter or _, then
            # alnum or _. Anything else (e.g. `--flag=x`, `=foo`)
            # ends the env-prefix walk.
            if head and (head[0].isalpha() or head[0] == "_") and all(
                c.isalnum() or c == "_" for c in head
            ):
                if head in key_set:
                    out[head] = val
                continue
            break
        if tok in _WRAPPERS:
            continue
        # First non-wrapper, non-env token ends the env-prefix walk.
        break
    return out


def read_skip_envs(
    cmd: str = "",
    keys: Iterable[str] = (),
    fallback_to_environ: bool = True,
) -> dict[str, str]:
    """Return `{KEY: VALUE}` for each requested key found in cmd's
    inline env-prefix or os.environ. Inline wins on collision.

    Batch alias: when the caller's keys include a `SKIP_*` flag (or
    `SKIP_*_REASON`), and the command/env carries `SKIP_REVIEW_PIPELINE=1`
    + `SKIP_REVIEW_PIPELINE_REASON="<reason>"`, the alias is splatted into
    EVERY requested SKIP_* key so a single batch token unblocks all
    review-pipeline gates on the same call. Reduces the
    SKIP_REVIEW_HOOK + SKIP_SKILL_STEP_BLOCK + SKIP_PHASE1_BLOCK
    five-env stack the agent had to assemble for a single commit.
    """
    if not keys:
        return {}
    # Materialize so we can iterate twice (inline scan + environ
    # fallback) without consuming a generator.
    key_list = list(keys)
    env_result: dict[str, str] = {}
    if fallback_to_environ:
        for k in key_list:
            v = os.environ.get(k)
            if v is not None:
                env_result[k] = v
    inline_result = _scan_inline(cmd, key_list)
    # Inline wins -- merge inline LAST so it overrides matching env.
    merged = dict(env_result)
    merged.update(inline_result)
    # Batch alias splatting. Only fires when at least one requested
    # key is a SKIP_* flag and the alias is asserted on the same call.
    has_skip_flag = any(
        k.startswith("SKIP_") and not k.endswith("_REASON")
        for k in key_list
    )
    if has_skip_flag:
        alias = _scan_inline(
            cmd, ("SKIP_REVIEW_PIPELINE", "SKIP_REVIEW_PIPELINE_REASON")
        )
        if not alias and fallback_to_environ:
            for k in ("SKIP_REVIEW_PIPELINE", "SKIP_REVIEW_PIPELINE_REASON"):
                v = os.environ.get(k)
                if v is not None:
                    alias[k] = v
        if alias.get("SKIP_REVIEW_PIPELINE") == "1":
            reason = alias.get("SKIP_REVIEW_PIPELINE_REASON", "")
            for k in key_list:
                if k.endswith("_REASON") and k not in merged:
                    merged[k] = reason
                elif (k.startswith("SKIP_")
                      and not k.endswith("_REASON")
                      and k not in merged):
                    merged[k] = "1"
    return merged
