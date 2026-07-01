#!/usr/bin/env python3
import json, sys, subprocess, re, os
d = json.load(sys.stdin)
cmd = d.get("tool_input", {}).get("command", "")
if not cmd.startswith("git commit"):
    sys.exit(0)
try:
    diff = subprocess.check_output(["git", "diff", "--cached", "-U0", "--", "todo/"], text=True, timeout=5, stderr=subprocess.DEVNULL)
except Exception:
    sys.exit(0)
if not diff:
    sys.exit(0)
# Tier classification lives in the ONE shared grammar module (scripts/ai-workflow/xref.py)
# so the git hook, the stamp writer, and todo-graph cannot drift. Fail-open with a loud
# note if it is unavailable -- this is a convenience gate, not a security boundary, and
# review-todo-section step 15 is the real backstop; wedging every commit is worse.
sys.path.insert(0, os.path.join(os.environ.get("CLAUDE_PROJECT_DIR", "."), "scripts", "ai-workflow"))
try:
    import xref as xref_mod
except Exception as e:
    sys.stderr.write(f"[accepted-xref] shared xref.py unavailable ({e}); skipping concreteness check\n")
    sys.exit(0)
bare = []
soft = []
for line in diff.splitlines():
    if not line.startswith("+") or line.startswith("+++"):
        continue
    body = line[1:]
    if ("Accepted" not in body and "Deferred" not in body) or "XREF" not in body:
        continue
    bare.extend(xref_mod.bare_clauses(body))
    soft.extend(xref_mod.soft_clauses(body))
if not bare and not soft:
    sys.exit(0)
if bare:
    sys.stderr.write("[Accepted-XREF concreteness BLOCK -- review-todo-section step 15] Staged TODO diff contains BARE XREF(s) with no parenthetical (definite dead-end paper trail): " +
                     " | ".join(bare[:3]) + ". " +
                     "Every Accepted XREF must point at a CONCRETE [ ] checklist item. Open the target TODO section; either quote an existing [ ] item as (item: \"NAME\" at line N) or create a new concrete item NOW that names the source file/function to retrofit, the helper to add, and the validation behavior. " +
                     "Then re-stage the TODO file and retry the commit.")
    if soft:
        sys.stderr.write(" Also SOFT XREF(s) (parenthetical present but no item:/at line/retrofit marker -- verify target has concrete [ ] item): " + " | ".join(soft[:3]) + ".")
    sys.exit(2)
print(json.dumps({"systemMessage": "[Accepted-XREF concreteness WARN -- review-todo-section step 15] Staged TODO diff contains Accepted/Deferred XREF(s) with paraphrase-only parenthetical(s) (no item:/at line/retrofit marker): " + " | ".join(soft[:3]) + ". Verify each target TODO section has a concrete [ ] item addressing the gap; if not, fix the XREF (add item: \"NAME\" at line N) or create the item before proceeding."}))
