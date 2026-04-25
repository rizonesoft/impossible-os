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

deferred_terms = ["STATUS_NOT_IMPLEMENTED", "STATUS_NOT_SUPPORTED", "E_NOTIMPL", "ENOSYS"]

# A) TEST_ASSERT(*, deferred-status) -- should be TEST_PENDING
assert_hits = []
for m in re.finditer(r"TEST_ASSERT(?:_EQ|_NEQ)?\s*\(([^;]+?)\)\s*;", all_text):
    args = m.group(1)
    for term in deferred_terms:
        if term in args:
            line = all_text[:m.start()].count("\n") + 1
            assert_hits.append((line, term, args.strip()[:80]))
            break

# B) TEST_PENDING(...) message format violations
pending_violations = []
for m in re.finditer(r'TEST_PENDING\s*\([^,]+,\s*"([^"\\]+)"\s*\)', all_text):
    msg = m.group(1)
    line = all_text[:m.start()].count("\n") + 1
    bad = []
    if "TODO-" in msg or "TODO " in msg:
        bad.append("contains TODO ref (drifts; put in source comment instead)")
    if "\u00a7" in msg:
        bad.append("contains section sign U+00A7 (mojibake in Windows serial)")
    if "\u2013" in msg or "\u2014" in msg:
        bad.append("contains en/em dash (mojibake in Windows serial)")
    if len(msg) > 80:
        bad.append("over 80 chars (" + str(len(msg)) + ") -- shorten, suite name carries subsystem")
    if bad:
        pending_violations.append((line, msg[:60], bad))

if not assert_hits and not pending_violations:
    sys.exit(0)

parts = []
if assert_hits:
    rep = []
    for ln, term, args in assert_hits[:3]:
        rep.append("line " + str(ln) + " (" + term + "): " + args)
    parts.append("TEST_ASSERT treats deferred-feature sentinel (" +
                 ", ".join(set(t for _, t, _ in assert_hits)) +
                 ") as a passing condition: " + " | ".join(rep) +
                 ". Use TEST_PENDING(cond, msg) instead.")
if pending_violations:
    rep = []
    for ln, msg, bad in pending_violations[:3]:
        rep.append("line " + str(ln) + " \"" + msg + "\": " + "; ".join(bad))
    parts.append("TEST_PENDING message violates format rules: " + " | ".join(rep) +
                 ". Required: ASCII only, NO TODO refs, under 80 chars; suite name carries the subsystem.")

print(json.dumps({"systemMessage":
    "[TEST_PENDING REMINDER -- implement-unit-tests skill] " +
    " ALSO ".join(parts) +
    " Canonical pattern: TEST_PENDING(st == STATUS_NOT_IMPLEMENTED, \"NtFooBar (0xNN): no <subsystem> yet\"). " +
    "TODO refs belong in the SOURCE COMMENT next to the test, not in the runtime message (they drift)."}))
