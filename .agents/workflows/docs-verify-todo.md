---
description: How to verify a previously implemented TODO section
---

# Verify a Completed TODO Section

// turbo-all

## Steps

1. **Read the verification prompt** in the TODO section. It should list specific checks.

2. **For each `[x]` item — verify in source code:**
   - File exists at the expected path
   - Function/struct signatures match the TODO description
   - No placeholder or stub implementations left behind
   - Cross-references are accurate (open referenced sections, confirm they exist)

3. **Clean build:**
   ```bash
   bash scripts/build.sh clean
   ```
   Verify: `tail -1 build/build.log` shows `=== BUILD OK ===`

4. **Run in QEMU:**
   ```bash
   bash scripts/build.sh run
   ```
   Let the OS boot fully (wait for desktop or terminal prompt).

5. **Verify serial output matches expectations:**
   - All `[OK]` messages listed in the TODO appear in boot log
   - No new `[!!]`, `[FAIL]`, `PANIC`, or `FAULT` messages
   - Memory allocations succeed (no `kmalloc: out of memory` or PMM failures)

6. **Check for regressions in related subsystems:**
   - If the TODO touches framebuffer → verify desktop renders correctly
   - If the TODO touches interrupts → verify keyboard/mouse still work
   - If the TODO touches filesystem → verify boot splash loads assets
   - If the TODO touches memory → verify no corruption in unrelated subsystems

7. **If issues found:**
   - Update the TODO: uncheck broken items `[ ]`, add notes explaining the issue
   - Fix the code, rebuild, and re-verify from step 3
   - Once fixed, re-mark items `[x]` and commit the fix

## Output

Report to the user:
- ✅ Items verified (list)
- ⚠️ Issues found and fixed (if any)
- Build status and boot log summary
