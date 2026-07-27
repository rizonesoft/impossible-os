#!/usr/bin/env python3
"""Contract test for agent_result_cache.py (content-addressed agent reports)."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOK = REPO / ".claude/hooks/agent_result_cache.py"

REPORT = ("Evidence map:\n- src/kernel/a.c:10 implements the frob path\n"
          "- include/kernel/a.h:5 contract struct\n"
          "Verify-first: src/kernel/a.c:10-40\n" * 3)


def _mk_fixture(d: pathlib.Path) -> pathlib.Path:
    fx = d / "fx"
    (fx / "src/kernel").mkdir(parents=True)
    (fx / "todo").mkdir()
    subprocess.run(["git", "init", "-q", str(fx)], check=True)
    (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
    (fx / "todo/TODO-01.md").write_text("# t\n")
    subprocess.run(["git", "-C", str(fx), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(fx), "-c", "user.email=t@t",
                    "-c", "user.name=t", "commit", "-qm", "init"],
                   check=True, capture_output=True)
    return fx


def _hook(fx, mode, payload):
    return subprocess.run([sys.executable, str(HOOK), mode],
                          input=json.dumps(payload), capture_output=True,
                          text=True, cwd=str(fx),
                          env={**os.environ})


def _payload(prompt, response=None):
    p = {"tool_name": "Agent",
         "tool_input": {"subagent_type": "kernel-explorer", "prompt": prompt}}
    if response is not None:
        p["tool_response"] = response
    return p


def test_cache_roundtrip_and_invalidation():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        prompt = "Map the frob path integration surface in src/kernel."
        # pre before any store: pass-through (rc 0)
        assert _hook(fx, "pre", _payload(prompt)).returncode == 0
        # post stores the report
        assert _hook(fx, "post", _payload(prompt, REPORT)).returncode == 0
        assert list((fx / ".claude/state/agent-cache").glob("*.json"))
        # identical dispatch -> BLOCK with the cached report
        r = _hook(fx, "pre", _payload(prompt))
        assert r.returncode == 2 and "cached kernel-explorer report" in r.stderr
        assert "frob path" in r.stderr
        # whitespace-only prompt difference still hits (normalized)
        r = _hook(fx, "pre", _payload("Map the frob   path\nintegration surface in src/kernel."))
        assert r.returncode == 2
        # different prompt -> miss
        assert _hook(fx, "pre", _payload(prompt + " fresh run: retest")).returncode == 0
        # source content change -> miss (fingerprint moved)
        (fx / "src/kernel/a.c").write_text("int a(void){return 2;}\n")
        assert _hook(fx, "pre", _payload(prompt)).returncode == 0
        # revert -> hit again (content-addressed, no clock)
        (fx / "src/kernel/a.c").write_text("int a(void){return 1;}\n")
        assert _hook(fx, "pre", _payload(prompt)).returncode == 2


def test_non_cacheable_agent_passthrough():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        p = {"tool_name": "Agent",
             "tool_input": {"subagent_type": "checks-runner", "prompt": "build"},
             "tool_response": REPORT}
        assert _hook(fx, "post", p).returncode == 0
        assert not (fx / ".claude/state/agent-cache").exists()
        assert _hook(fx, "pre", p).returncode == 0


def _leaf_transcript(fx, prompt, report):
    """A subagent's own transcript: first user message == the dispatched prompt,
    then an assistant reply (mirrors the real background-subagent JSONL)."""
    tp = fx / "leaf.jsonl"
    tp.write_text(
        json.dumps({"message": {"role": "user", "content": prompt}}) + "\n"
        + json.dumps({"message": {"role": "assistant",
                     "content": [{"type": "text", "text": report}]}}) + "\n")
    return tp


def test_subagentstop_store_then_pre_hit():
    # The Agent tool runs subagents in the BACKGROUND, so the report only
    # arrives at SubagentStop. Storing there must produce a key a later `pre`
    # (which sees the same prompt via tool_input) hits.
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        prompt = "Map the frob path integration surface in src/kernel."
        tp = _leaf_transcript(fx, prompt, REPORT)
        # pre before store: miss
        assert _hook(fx, "pre", _payload(prompt)).returncode == 0
        # SubagentStop payload (agent_type + last_assistant_message + leaf path)
        stop = {"agent_type": "kernel-explorer",
                "last_assistant_message": REPORT,
                "agent_transcript_path": str(tp)}
        assert _hook(fx, "subagentstop", stop).returncode == 0
        assert list((fx / ".claude/state/agent-cache").glob("*.json")), \
            "SubagentStop did not store a cache entry"
        # now the identical dispatch is served from cache
        r = _hook(fx, "pre", _payload(prompt))
        assert r.returncode == 2 and "frob path" in r.stderr, r.stderr
        # non-cacheable agent type at SubagentStop is ignored
        stop2 = {"agent_type": "checks-runner",
                 "last_assistant_message": REPORT,
                 "agent_transcript_path": str(tp)}
        n0 = len(list((fx / ".claude/state/agent-cache").glob("*.json")))
        assert _hook(fx, "subagentstop", stop2).returncode == 0
        assert len(list((fx / ".claude/state/agent-cache").glob("*.json"))) == n0


def test_kill_switch_and_tiny_reports():
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        prompt = "Map it."
        # tiny (<80 chars) responses are not cached (likely an error string)
        assert _hook(fx, "post", _payload(prompt, "err")).returncode == 0
        assert not list((fx / ".claude/state/agent-cache").glob("*.json")) \
            if (fx / ".claude/state/agent-cache").exists() else True
        # kill switch bypasses even a real hit
        assert _hook(fx, "post", _payload(prompt, REPORT)).returncode == 0
        env = {**os.environ, "AGENT_RESULT_CACHE_DISABLE": "1"}
        r = subprocess.run([sys.executable, str(HOOK), "pre"],
                           input=json.dumps(_payload(prompt)),
                           capture_output=True, text=True, cwd=str(fx), env=env)
        assert r.returncode == 0


def _load_hook():
    import importlib.util
    spec = importlib.util.spec_from_file_location("arc", HOOK)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_canonical_scope_key_hits_across_volatile_prompts():
    # P2.3: two review-evidence-mapper rounds naming the same section+file over
    # an UNCHANGED tree must produce the SAME cache key despite different prose
    # (round number, findings) -- the 22-stores/0-hits fix.
    arc = _load_hook()
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        p_r4 = ("Round 4: verify finding at src/kernel/a.c:10 for "
                "todo/TODO-01.md section 3 -- possible UAF")
        p_r5 = ("Round 5: re-verify src/kernel/a.c:42 in todo/TODO-01.md "
                "section 3 after the lock fix; check finding #7")
        k4 = arc._cache_key(fx, "review-evidence-mapper", p_r4)
        k5 = arc._cache_key(fx, "review-evidence-mapper", p_r5)
        assert k4 and k5 and k4 == k5, (k4, k5)


def test_canonical_scope_key_distinguishes_sections():
    # A different section (different files) must NOT collide -> no wrong hit.
    arc = _load_hook()
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        ka = arc._cache_key(fx, "review-evidence-mapper",
                            "verify todo/TODO-01.md section 3 src/kernel/a.c:1")
        kb = arc._cache_key(fx, "review-evidence-mapper",
                            "verify todo/TODO-01.md section 9 src/kernel/b.c:1")
        assert ka and kb and ka != kb, (ka, kb)


def test_researcher_prompt_still_load_bearing():
    # parity-research-analyst is EXCLUDED from canonicalization: its prompt is
    # the question, so different prompts must give different keys.
    arc = _load_hook()
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        k1 = arc._cache_key(fx, "parity-research-analyst", "how does Win11 do X")
        k2 = arc._cache_key(fx, "parity-research-analyst", "how does Linux do Y")
        assert k1 and k2 and k1 != k2, (k1, k2)


def test_unrelated_edit_does_not_invalidate_a_cached_report():
    """T2-2, the 155-stores/0-hits root cause.

    The key used to include a fingerprint of the WHOLE source tree, so an edit
    to ANY file invalidated every entry. The designed hit case is a mapper
    re-dispatched across review rounds -- and a fix loop EDITS CODE between
    those rounds, so the key always differed and the cache never once hit.
    Editing src/desktop/unrelated.c invalidated a kernel-explorer report about
    src/kernel/. The key is now the SCOPE; freshness is validated separately
    against the paths the report actually covered."""
    arc = _load_hook()
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        (fx / "src/desktop").mkdir(parents=True)
        (fx / "src/desktop/unrelated.c").write_text("int u(void){return 1;}\n")
        prompt = "map src/kernel/a.c for todo/TODO-01.md section 3"
        k1 = arc._cache_key(fx, "kernel-explorer", prompt)
        (fx / "src/desktop/unrelated.c").write_text("int u(void){return 99;}\n")
        k2 = arc._cache_key(fx, "kernel-explorer", prompt)
        assert k1 and k1 == k2, ("unrelated edit still moves the key", k1, k2)


def test_freshness_is_bound_to_covered_paths_only():
    """The hit must survive an unrelated edit and DIE on a covered-file edit.
    A false hit is a quality loss, so the second half matters more."""
    arc = _load_hook()
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        (fx / "src/desktop").mkdir(parents=True)
        (fx / "src/desktop/unrelated.c").write_text("int u(void){return 1;}\n")
        prompt = "map src/kernel/a.c for todo/TODO-01.md section 3"
        arc._store(fx, "kernel-explorer", prompt, "a.c:10 covers src/kernel/a.c")
        key = arc._cache_key(fx, "kernel-explorer", prompt)
        rec = json.loads((fx / arc.CACHE_DIR_REL / f"{key}.json").read_text())
        assert "src/kernel/a.c" in rec["covered"], rec["covered"]

        def fresh():
            return arc._covered_fingerprint(
                fx, "kernel-explorer", rec["covered"]) == rec["covered_fp"]

        assert fresh(), "unchanged tree must be fresh"
        (fx / "src/desktop/unrelated.c").write_text("int u(void){return 99;}\n")
        assert fresh(), "an unrelated edit must NOT invalidate"
        (fx / "src/kernel/a.c").write_text("int a(void){return 1234;}\n")
        assert not fresh(), "an edit to a COVERED file MUST invalidate (stale hit)"


def test_legacy_entry_without_fingerprint_never_hits():
    """Entries stored before this change carry no covered_fp. They must MISS,
    not be trusted -- fail toward a re-run, never toward a stale map."""
    arc = _load_hook()
    with tempfile.TemporaryDirectory() as d:
        fx = _mk_fixture(pathlib.Path(d))
        prompt = "map src/kernel/a.c for todo/TODO-01.md section 3"
        arc._store(fx, "kernel-explorer", prompt, "a.c:10 covers src/kernel/a.c")
        key = arc._cache_key(fx, "kernel-explorer", prompt)
        f = fx / arc.CACHE_DIR_REL / f"{key}.json"
        rec = json.loads(f.read_text()); rec.pop("covered_fp", None)
        f.write_text(json.dumps(rec))
        r = _hook(fx, "pre", {"tool_name": "Task", "tool_input": {
            "subagent_type": "kernel-explorer", "prompt": prompt}})
        assert r.returncode == 0, "a legacy entry must not be served as a hit"


if __name__ == "__main__":
    test_cache_roundtrip_and_invalidation()
    test_non_cacheable_agent_passthrough()
    test_subagentstop_store_then_pre_hit()
    test_kill_switch_and_tiny_reports()
    test_canonical_scope_key_hits_across_volatile_prompts()
    test_canonical_scope_key_distinguishes_sections()
    test_researcher_prompt_still_load_bearing()
    test_unrelated_edit_does_not_invalidate_a_cached_report()
    test_freshness_is_bound_to_covered_paths_only()
    test_legacy_entry_without_fingerprint_never_hits()
    print("PASS: agent-result cache")
