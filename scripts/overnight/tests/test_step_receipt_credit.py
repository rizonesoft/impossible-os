#!/usr/bin/env python3
# v14 close-out (2026-08-16): bundling three review dispatches into ONE Bash
# call satisfied the commit gate (stamps recorded from the artifacts) while
# skill_step_block still demanded step 8 -- the step observer attributes off
# the command line and saw one opaque call. `_receipted_kinds` lets a receipt
# in last-review-stamps.json minted DURING the skill invocation satisfy the
# step, mirroring the _converged_kinds doctrine. The refusal direction is the
# load-bearing half: receipts minted BEFORE the invocation started must never
# credit it, or last section's reviews would wave the next section through.
import json
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / ".claude/hooks"))
import skill_step_block as ssb  # noqa: E402

FAILS = []


def check(name, cond):
    if not cond:
        FAILS.append(name)


TODO = "todo/02-kernel-core/TODO-99-fixture.md"
T0 = 1_000_000_000_000_000_000


def run(stamps, entry):
    with tempfile.TemporaryDirectory() as td:
        state = pathlib.Path(td) / ".claude" / "state"
        state.mkdir(parents=True)
        (state / "last-review-stamps.json").write_text(json.dumps(stamps))
        return ssb._receipted_kinds(td, entry)


entry = {"todo_path": TODO, "started_ts": T0}

# 1. Receipts minted after the invocation started satisfy their kinds.
got = run({TODO: {"adversarial": T0 + 10, "adversarial_head": "abc",
                  "consistency": T0 + 20, "perf": T0 + 30,
                  "section": "7"}}, entry)
check("fresh receipts credit all three kinds",
      got == {"adversarial", "consistency", "perf"})

# 2. REFUSAL: receipts from before the invocation never count.
got = run({TODO: {"adversarial": T0 - 10, "consistency": T0 - 5}}, entry)
check("stale receipts credit nothing", got == set())

# 3. REFUSAL: another TODO's receipts never count.
got = run({"todo/other/TODO-01-x.md": {"adversarial": T0 + 10}}, entry)
check("foreign todo credits nothing", got == set())

# 4. Metadata keys are never kinds.
got = run({TODO: {"adversarial_head": T0 + 10, "section": T0 + 10,
                  "perf_section": T0 + 10}}, entry)
check("metadata keys are filtered", got == set())

# 5. Malformed entry / missing state fail closed to empty.
check("no todo_path -> empty", run({TODO: {"adversarial": T0 + 1}},
                                   {"started_ts": T0}) == set())
check("no started_ts -> empty", run({TODO: {"adversarial": T0 + 1}},
                                    {"todo_path": TODO}) == set())
with tempfile.TemporaryDirectory() as td:
    check("missing state file -> empty",
          ssb._receipted_kinds(td, entry) == set())

if FAILS:
    print("test_step_receipt_credit: FAIL")
    for f in FAILS:
        print(f"  - {f}")
    sys.exit(1)
print("test_step_receipt_credit: OK (7 checks)")
