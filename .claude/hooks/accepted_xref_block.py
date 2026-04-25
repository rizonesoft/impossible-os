#!/usr/bin/env python3
import json, sys, subprocess, re
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
bare = []
soft = []
for line in diff.splitlines():
    if not line.startswith("+") or line.startswith("+++"):
        continue
    body = line[1:]
    if ("Accepted" not in body and "Deferred" not in body) or "XREF" not in body:
        continue
    for xref in re.findall(r"XREF:\s*[^,;\]]*?TODO-\d+[^,;\]]*", body):
        xl = xref.lower()
        has_concrete = ("item:" in xl) or ("at line" in xl) or ("retrofit" in xl) or ("helper;" in xl)
        has_paren = "(" in xref
        if not has_paren:
            bare.append(xref.strip()[:100])
        elif not has_concrete:
            soft.append(xref.strip()[:100])
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
