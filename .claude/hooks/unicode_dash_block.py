#!/usr/bin/env python3
# PreToolUse Edit/Write/MultiEdit BLOCK: rejects edits whose
# new_string / content contains U+2013 (en dash) or U+2014 (em
# dash). Windows serial output and CMD garble these as mojibake.
# The lint pass at scripts/lint.sh would catch them later; this
# hook prevents the bad bytes from ever landing in the file.
#
# Extracted from .claude/settings.json inline-Python (TODO-08 section
# 7). Behavior is byte-equivalent to the original.
#
# Hook category: BLOCKING (sys.exit(2) on hit). NOT a candidate
# for wrap.sh marker-prefilter because the markers ARE the byte
# sequences U+2013/U+2014 themselves, and a JSON payload that
# carries those bytes is exactly the case we want to inspect.
# Direct python3 invocation only.
#
# Implementation note: this file deliberately constructs the
# forbidden characters via chr() rather than embedding them as
# literals, so that this very hook does not block its own write.
#
# Doctrine: CLAUDE.md "Code Style: ASCII Dashes, No Bare Section
# Refs"; docs/infrastructure/code-style-policies.md.
import json
import sys


EM_DASH = chr(0x2014)
EN_DASH = chr(0x2013)


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    ti = d.get("tool_input", {}) or {}
    tn = d.get("tool_name", "")
    texts = []
    if tn == "Write":
        texts.append(ti.get("content", "") or "")
    elif tn == "Edit":
        texts.append(ti.get("new_string", "") or "")
    elif tn == "MultiEdit":
        for e in ti.get("edits", []) or []:
            texts.append(e.get("new_string", "") or "")

    bad = set()
    for t in texts:
        if EM_DASH in t:
            bad.add("em dash (U+2014)")
        if EN_DASH in t:
            bad.add("en dash (U+2013)")
    if not bad:
        return 0

    sys.stderr.write(
        "Unicode dash detected in tool input: "
        + ", ".join(sorted(bad))
        + ". Forbidden: U+2013/U+2014. Do not paste ASCII -- as "
        "typographic dash in prose; rewrite (colon, semicolon, paren, "
        "or split sentences). See CLAUDE.md No Unicode Dashes."
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
