#!/usr/bin/env python3
import json, sys, re
from pathlib import Path
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

# Fence membership comes from the shared tracker (`scripts/todo_fence.py`).
# Until 2026-11-13 a ```c...``` regex decided it, which missed a tilde fence,
# ended a block at a shorter inner run, and read a ``` inside an HTML comment
# as code; a private grammar is the drift the tracker exists to end, so a
# failed import raises rather than falling back to one.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import todo_fence  # noqa: E402
KIND_CODE_FENCE = todo_fence._cache_schema().KIND_CODE_FENCE

lines = all_text.split("\n")
scan = todo_fence.fence_scan(lines)
leaves = scan.leaf_views
OPENER = re.compile(r"^ {0,3}(`{3,}|~{3,})\s*([^\s`]*)")


def _fence_blocks():
    """(opener line index, info word, body line count) per fenced block. The
    tracker says which lines are fenced; a run of fenced lines is split only
    where a line closes the block its opener started, so two blocks written
    back to back stay two."""
    i, n = 0, len(lines)
    while i < n:
        if scan.codes[i] != KIND_CODE_FENCE:
            i += 1
            continue
        m = OPENER.match(leaves[i])
        run, info = (m.group(1), m.group(2).lower()) if m else ("", "")
        j = i + 1
        while j < n and scan.codes[j] == KIND_CODE_FENCE:
            t = leaves[j].strip()
            if run and t and set(t) == {run[0]} and len(t) >= len(run):
                break
            j += 1
        yield i, info, j - i - 1
        i = j + 1


bloated_blocks = []
for start, info, body in _fence_blocks():
    if info != "c" or body < 15:
        continue
    # spec-block opt-out: <!-- spec-block-ok: ... --> within 3 lines above the opener
    if any("spec-block-ok" in ln for ln in lines[max(0, start - 3):start]):
        continue
    first_line = lines[start + 1][:60] if start + 1 < len(lines) else ""
    bloated_blocks.append((start + 1, body, first_line))

# Forbidden parenthesized N.M labels in checklist items (never inside a fence
# or a comment, where a `- [x] (1.2 ...)` is an example).
nm_labels = []
NM = re.compile(r"^\s*-\s*\[[ x]\]\s*\((\d+\.\d+)\s+([^)]{1,60})\)")
for idx, ln in enumerate(lines):
    if scan.mask[idx]:
        continue
    m = NM.match(ln)
    if m:
        nm_labels.append((idx + 1, m.group(1), m.group(2)))

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
