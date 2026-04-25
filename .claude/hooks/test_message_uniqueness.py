#!/usr/bin/env python3
import json, sys, re
d = json.load(sys.stdin)
ti = d.get("tool_input", {})
tn = d.get("tool_name", "")
path = ti.get("file_path", "")
if not path:
    sys.exit(0)
p = path.replace(chr(92), "/")
if "src/kernel/test/" not in p or not p.endswith(".c"):
    sys.exit(0)
base = p.rsplit("/", 1)[-1]
if not base.startswith("test_") or base in ("test_runner.c", "test_main.c"):
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
# Find for/while blocks, scan body for TEST_ASSERT with literal message containing no '%'
hits = []
for m in re.finditer(r"\b(for|while)\s*\([^)]*\)\s*\{", all_text):
    start = m.end()
    depth = 1
    i = start
    while i < len(all_text) and depth > 0:
        if all_text[i] == "{":
            depth += 1
        elif all_text[i] == "}":
            depth -= 1
        i += 1
    body = all_text[start:i]
    # skip if body already has snprintf (per-iteration message pattern)
    if "snprintf" in body:
        continue
    for am in re.finditer(r'TEST_ASSERT(?:_EQ|_NEQ|_NULL|_NOT_NULL)?\s*\([^;]*?,\s*"([^"\\]+)"\s*\)', body):
        msg = am.group(1)
        if "%" in msg:
            continue
        hits.append(msg[:60])
if not hits:
    sys.exit(0)
unique = sorted(set(hits))[:5]
print(json.dumps({"systemMessage":
    "[test message uniqueness CHECK -- implement-unit-tests skill] " +
    "Found TEST_ASSERT(s) inside a for/while loop with literal messages (no per-iteration snprintf): " +
    " | ".join('"' + m + '"' for m in unique) +
    ". " +
    "Every passing iteration logs the same line, so a mid-loop failure cannot be diagnosed from boot output. " +
    "For small-N loops (under ~50 iters) build the message via snprintf with a {key, name} table; " +
    "for high-N loops (byte/page scans) track first-mismatch index inside the loop and assert ONCE outside with the index in the message. " +
    "See implement-unit-tests skill Guardrails section for the canonical patterns. " +
    "If the literal already contains '%' or the loop runs only once, this warning is a false positive."}))
