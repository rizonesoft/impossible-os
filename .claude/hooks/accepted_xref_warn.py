#!/usr/bin/env python3
import json, sys, re
d = json.load(sys.stdin)
ti = d.get("tool_input", {})
tn = d.get("tool_name", "")
path = ti.get("file_path", "")
if not path:
    sys.exit(0)
p = path.replace(chr(92), "/")
if not p.endswith(".md") or "todo" not in p.split("/"):
    sys.exit(0)
texts = []
if tn == "Write":
    texts.append(ti.get("content", ""))
elif tn == "Edit":
    texts.append(ti.get("new_string", ""))
elif tn == "MultiEdit":
    for e in ti.get("edits", []) or []:
        texts.append(e.get("new_string", ""))
if not texts:
    sys.exit(0)
all_text = "\n".join(texts)
issues = []
for line in all_text.splitlines():
    if ("Accepted" not in line and "Deferred" not in line) or "XREF" not in line:
        continue
    for xref in re.findall(r"XREF:\s*[^,;\]]*?TODO-\d+[^,;\]]*", line):
        xl = xref.lower()
        has_concrete = ("item:" in xl) or ("at line" in xl) or ("retrofit" in xl) or ("helper;" in xl)
        has_paren = "(" in xref
        if not has_paren:
            issues.append("BARE (no parenthetical): " + xref.strip()[:80])
        elif not has_concrete:
            issues.append("SOFT (no item:/at line/retrofit marker): " + xref.strip()[:80])
if not issues:
    sys.exit(0)
msg = ("[Accepted-XREF concreteness CHECK -- review-todo-section step 15] " +
       "New Accepted/Deferred XREF line(s) may be dead-end paper trail. Issues: " + " | ".join(issues[:5]) + ". " +
       "Per skill step 15: every Accepted XREF must point at a CONCRETE [ ] item (names source file/function, describes work). " +
       "For each issue: open target TODO, verify/create a concrete [ ] item, reference as (item: \"NAME\" at line N). " +
       "Bare section-only XREFs and paraphrase-only parentheticals are rejected.")
print(json.dumps({"systemMessage": msg}))
