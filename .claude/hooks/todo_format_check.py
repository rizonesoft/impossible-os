#!/usr/bin/env python3
import json, sys, re
d = json.load(sys.stdin)
ti = d.get("tool_input", {})
tn = d.get("tool_name", "")
path = ti.get("file_path", "")
if not path:
    sys.exit(0)
p = path.replace(chr(92), "/")
if not p.endswith(".md") or "/todo/" not in p:
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

bloated_blocks = []
for m in re.finditer(r"```c\b\n(.*?)```", all_text, re.DOTALL):
    block = m.group(1)
    block_lines = block.count("\n")
    if block_lines < 15:
        continue
    # spec-block opt-out: <!-- spec-block-ok: ... --> within ~3 lines before the opening ```
    pre = all_text[max(0, m.start() - 200):m.start()]
    if "spec-block-ok" in pre:
        continue
    line = all_text[:m.start()].count("\n") + 1
    first_line = block.split("\n", 1)[0][:60]
    bloated_blocks.append((line, block_lines, first_line))

# Forbidden parenthesized N.M labels in checklist items
nm_labels = []
for m in re.finditer(r"^\s*-\s*\[[ x]\]\s*\((\d+\.\d+)\s+([^)]{1,60})\)", all_text, re.MULTILINE):
    line = all_text[:m.start()].count("\n") + 1
    nm_labels.append((line, m.group(1), m.group(2)))

if not bloated_blocks and not nm_labels:
    sys.exit(0)

parts = []
if bloated_blocks:
    rep = []
    for ln, n, first in bloated_blocks[:3]:
        rep.append("line " + str(ln) + " (" + str(n) + " lines): " + first)
    parts.append("Inline ```c``` block(s) over 15 lines: " + " | ".join(rep) +
                 ". Compact to a single bullet naming the type/constants + count + external reference. Per validate-todo-file step 15: TODO is a plan; header file is the implementation. Spec opt-out: add `<!-- spec-block-ok: <reason> -->` above the ``` for genuine wire-format / ABI-contract specs.")
if nm_labels:
    rep = []
    for ln, num, label in nm_labels[:3]:
        rep.append("line " + str(ln) + ": (" + num + " " + label + ")")
    parts.append("Forbidden N.M parenthesized checklist label(s): " + " | ".join(rep) +
                 ". Strip the `(N.M Title)` prefix; the bullet text itself is the label. Same anti-pattern as `### N.M` headings (validate-todo-file step 6).")

print(json.dumps({"systemMessage":
    "[TODO format CHECK -- validate-todo-file step 15] " +
    " ALSO ".join(parts) +
    " The TODO is a plan, not a header file. Implementation lives in src/ + include/."}))
