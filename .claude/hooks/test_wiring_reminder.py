#!/usr/bin/env python3
# PostToolUse Edit/Write/MultiEdit reminder: when the agent edits
# a test file under src/kernel/test/, emit a systemMessage with
# the test-wiring checklist (registration, category, runner-init,
# concrete assertions, no tautological constant tests).
#
# Extracted from .claude/settings.json inline-Python (TODO-08
# section 7). Behavior is byte-equivalent to the original.
#
# Hook category: REMINDER. Wrap.sh-eligible (marker:
# `src/kernel/test/`).
import json
import os
import sys


_SKIP_NAMES = ("test_runner.c", "test_main.c")


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    path = (d.get("tool_input", {}) or {}).get("file_path", "") or ""
    if (not path
            or "src/kernel/test/" not in path
            or not path.endswith(".c")):
        return 0
    base = os.path.basename(path)
    if not base.startswith("test_") or base in _SKIP_NAMES:
        return 0

    msg = (
        "[test wiring REMINDER] Edited " + path + ". Verify before "
        "next build: "
        "(1) every new test function is registered via "
        "test_suite_register_cat() in the corresponding "
        "test_register_*() function; "
        "(2) the test_register_*() function is called from "
        "test_runner_init() in test_runner.c; "
        "(3) the TEST_CAT_* category matches the subsystem "
        "(mm/fs/sched/ob/security/ipc/boot/abi/storage/exec); "
        "(4) every assertion has a concrete expected value, not "
        "vague verify-it-works phrasing; "
        "(5) NO tautological constant tests like "
        "TEST_ASSERT_EQ(POST16_FOO, 0xDF20) -- the compiler "
        "enforces #define values, the real protection is the "
        "boot-time uniqueness check. "
        "Tests written but not registered are silent failures."
    )
    print(json.dumps({"systemMessage": msg}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
