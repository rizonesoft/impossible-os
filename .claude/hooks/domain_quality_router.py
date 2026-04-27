#!/usr/bin/env python3
# PreToolUse Edit/Write/MultiEdit reminder: when the agent edits a
# C/H/ASM source file under one of the recognized domain roots,
# emit a `systemMessage` reminding the agent to walk the matching
# domain code-quality skill BEFORE writing.
#
# Extracted from .claude/settings.json inline-Python (TODO-08 §7).
# Behavior is byte-equivalent to the original; the routing table
# is the single source of truth for path-to-skill mapping. Adding
# a new domain quality skill = adding one row here.
#
# Hook category: REMINDER (systemMessage; never blocks). Safe to
# wrap with .claude/hooks/wrap.sh marker-prefilter -- the markers
# are the file extensions the routes care about.
import json
import sys


_ROUTES = (
    ("src/boot/",      "boot-code-quality",
     "UEFI error handling, EBS memory ownership, ACPI/SMBIOS table safety, "
     "framebuffer guards, boot_info ABI sync, serial-before-klog, "
     "POST16 coverage, fallback chains"),
    ("src/desktop/",   "desktop-code-quality",
     "WC framebuffer mapping, pitch!=width, back-buffer pattern, "
     "pixel format check, non-blocking compositor loop"),
    ("src/shell/",     "shell-code-quality",
     "Win32 console API, Windows path conventions, HANDLE-based I/O, "
     "syscall error checking"),
    ("user/",          "userland-code-quality",
     "user libc linkage, no kernel headers, syscall interface only, "
     "Win32 naming, HANDLE I/O"),
    ("src/apps/",      "userland-code-quality",
     "user libc linkage, no kernel headers, syscall interface only, "
     "Win32 naming, HANDLE I/O"),
    ("src/kernel/",    "kernel-code-quality",
     "SMP safety, 5-layer defense, memory rules (kmalloc <= 4 KB), "
     "bare-metal correctness, POST16 boot-path only, complete error paths, "
     "Gate 10 no TODO/FIXME/HACK, Gate 11a no removing Win32 surface"),
    ("include/kernel/", "kernel-code-quality",
     "SMP safety, 5-layer defense, memory rules, bare-metal correctness, "
     "POST16 boot-path only, complete error paths, Gate 10, Gate 11a"),
)


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    path = (d.get("tool_input", {}) or {}).get("file_path", "") or ""
    if not path:
        return 0
    if not path.endswith((".c", ".h", ".asm", ".S")):
        return 0
    p = path.replace("\\", "/")
    for prefix, skill, gates in _ROUTES:
        if prefix in p:
            msg = (
                f"[{skill} REQUIRED] About to edit code at {path}. "
                f"Walk the {skill} gates BEFORE writing: {gates}."
            )
            print(json.dumps({"systemMessage": msg}))
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
