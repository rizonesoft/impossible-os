---
name: run-verification
description: Verify a TODO's Verification section against a serial log (pasted or file path). Greps for expected markers, marks items PASS/FAIL with evidence. Works with any platform log (WHPX, TCG, VBox, bare metal) and catches regressions against saved logs.
---

# Run TODO Verification Against Serial Log

## When to Use

- User pastes serial output or provides a serial log file path
- User asks to "verify TODO-XX", "check this boot log", or "did anything regress"
- After booting on a new platform and wanting to validate against the TODO checklist
- Regression testing: re-run against a saved log to detect breakage

## Input

The skill needs two things:
1. **A TODO file** with a `## Verification` section
2. **A serial log** -- either:
   - Pasted directly in the conversation
   - A file path (e.g., `build/serial.log`, `build/test.log`)
   - Named with platform for tracking (e.g., "WHPX log from 2026-04-01")

## Workflow

### 1. Read the Verification section

Read the `## Verification` section of the target TODO. Each `- [ ]` line is a check.

### 2. Classify each item

| Category | How to verify against log |
|---|---|
| **Build** | Check if log contains boot output at all (implies build succeeded) |
| **Serial grep** | Search log for expected string |
| **Serial absence** | Confirm string is NOT in log |
| **Phase markers** | Search for `[PHASE0]`…`[PHASE3]` |
| **Unit test results** | Search for `=== N tests passed` summary |
| **Code check** | Grep/Glob the codebase (no log needed) |
| **Cannot verify from log** | Mark as `(not verifiable from serial log)` |

### 3. Run checks against the log

For each verification item:
- **Grep the serial log** for the expected pattern
- **Code-level checks** use Grep/Glob on the codebase
- **Items that can't be verified from a serial log** (visual/framebuffer, "stays up", interactive) get noted

### 4. Update the Verification section

Format rules -- append result to each item, do NOT rewrite the original text:

```markdown
- [x] Item text -- PASS: <evidence> (platform, YYYY-MM-DD)
- [ ] Item text -- FAIL: <what's missing or wrong>
- [ ] Item text -- (not verifiable from serial log)
```

Include the **platform name** in PASS annotations when provided (e.g., "WHPX", "TCG", "bare metal", "VBox").

### 5. Report summary

```
Verification: TODO-XX (platform: WHPX, 2026-04-01)
  PASS: N items
  FAIL: M items  
  Not verifiable: K items

Failed items:
  - <item> -- FAIL: <reason>
```

## Regression Mode

When the user provides a log and says "check for regressions" or "compare with previous":

1. Read the Verification section -- items already marked `[x]` with PASS are the baseline
2. Re-check every previously-passing item against the new log
3. If a previously-passing item now fails: mark it `REGRESSION:` instead of `FAIL:`

```markdown
- [ ] Item text -- REGRESSION: was passing on WHPX (2026-04-01), now missing from log
```

## Guardrails

- Do NOT attempt to boot QEMU or run builds -- only analyze provided logs
- Do NOT mark items `[x]` without evidence from the log
- Do NOT remove existing PASS/FAIL annotations -- append new results or update dates
- If the log is truncated (boot didn't complete), note which phases are missing
- Always report how far the boot got (last PHASE marker seen)

## Common Grep Patterns

| What to find | Pattern |
|---|---|
| Phase markers | `[PHASE0]`, `[PHASE1]`, `[PHASE2]`, `[PHASE3]` |
| Unit test summary | `=== N tests passed, 0 failed ===` |
| Test failures | `[FAIL]` |
| Subsystem ready | `kernel_subsystem_dump` or `Subsystem readiness` |
| Boot tests ran | `Boot Tests` or `Running boot tests` |
| Debug mode | `debug=1` in CONF line |
| Kernel panic | `KERNEL PANIC` or `triple fault` |
| Boot halt | `[BOOT HALT]` |
| Specific driver | `[xhci]`, `[ahci]`, `[nvme]`, `[net]` |
| Boot complete | `[PHASE3]` + `DESKTOP_READY` or `compositor` |
