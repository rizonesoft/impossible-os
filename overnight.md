# Overnight Handoff

## Where Claude Stopped

The unattended overnight run stopped on 2026-06-17 at 11:46:40 Africa/Johannesburg because Claude hit the weekly usage limit:

```text
You've hit your weekly limit - resets Jun 19, 9am (Africa/Johannesburg)
```

The last meaningful run log is:

```text
.claude/overnight/reports/run-20260616-211930.log
```

Current roadmap position:

- `todo/01-boot-platform/TODO-27-uefi-advanced.md` section 4 was shipped, reviewed, stamped, committed, and pushed as `192f942b`.
- Section 5, Secure Boot Enforcement Policy, was deferred because it depends on the not-yet-built kernel lockdown mechanism. The concrete owner section was created in `todo/02-kernel-core/TODO-10-kernel-security-hardening.md` section 16, and the deferral was committed and pushed as `a1ff986b`.
- The sequencer advanced to TODO-27 section 6, SMBIOS Extended Type Parsing.
- A Codex design review for section 6 returned two HIGH findings. Both were accepted:
  - Type 3 chassis serial is at formatted byte `d[7]`, asset tag is at `d[8]`, and the parser must guard `length >= 9`.
  - Type 19 extended start and end addresses are at offsets `0x0F` and `0x17`, not the Type 20 offsets. The parser must guard `length >= 0x1F`.
- The run then attempted the first section 6 edit command and hit the weekly limit.

Important working tree note:

- `include/kernel/smbios.h` already has partial section 6 declarations in the working tree: added baseboard version/serial/asset fields, Type 3 chassis fields, Type 16 memory array fields, Type 19 mapped-address fields, and the `smbios_get_chassis_type()` / `smbios_chassis_is_laptop()` declarations.
- Do not assume section 6 has no edits. Inspect the diff before continuing.
- The implementation, tests, registry population, TODO updates, build, review, and commit are not complete.

There are also other pre-existing modified files in the working tree. Treat them as user or prior-run work and do not revert them without explicit instruction.

## TODO: Overnight System Improvements

The priority is reliability, not cleverness. Last time this system was "improved", several changes made it worse and had to be reverted. Make future changes small, reversible, and backed by a narrow regression test before touching the live timer path.

- [ ] Add a tiny fixture test for usage-limit snooze handling. Feed sample output containing `You've hit your weekly limit - resets Jun 19, 9am (Africa/Johannesburg)` and assert that `.claude/overnight/snooze-until` is written.

- [ ] Fix the current usage-limit snooze parser wiring only if the test proves it is broken. The launcher appears to pipe the report tail into `python3 - ... <<'PYEOF'`, which likely feeds Python source through stdin instead of feeding the report text to `sys.stdin.read()`. Prefer a minimal fix: pass the report path as an argv and let the embedded Python open it.

- [ ] Add a pre-flight regression for active snooze. When `snooze-until` is in the future, `scripts/overnight/overnight-launch.sh` should exit before creating a new `run-*.log`. This would have prevented the 10-minute repeated limit logs from 11:50 through 13:40.

- [ ] Keep the single repo-owned launch path. Do not reintroduce the old plugin launcher or a second arming path. The current design goal is one guarded path through `scripts/overnight/overnight-arm.sh` and `scripts/overnight/overnight-launch.sh`.

- [ ] Add a "dry-run launch" mode for `overnight-launch.sh` that exercises pre-flight checks, lock handling, snooze handling, report-path selection, and Claude binary resolution without invoking Claude.

- [ ] Add a small shell regression script, for example `scripts/test-overnight-launch.sh`, with fixture-only tests. Keep it fast and local: no systemd, no Claude invocation, no Codex invocation.

- [ ] Make the launcher write one clear final status line for every skipped launch: `skipped: snooze-active`, `skipped: already-active`, `failed: claude-not-found`, or `ran: claude-exit-N`. This should be plain text and easy to grep.

- [ ] Consider moving usage-limit parsing into a small standalone script under `scripts/overnight/` so it can be tested without executing the launcher. Do this only if it reduces complexity; do not create a new framework around it.

- [ ] Add a conservative report retention command, not an automatic deletion policy. Reports are useful during failures. A manual helper that deletes only ignored, completed `run-*.log` files older than N days is safer than background cleanup.

- [ ] Document the resume rule in this file or the owning overnight skill: after a limit stop, first inspect `git status --short`, then inspect the last meaningful run log, then continue from the last explicit TODO section and account for any partial edits.

- [ ] Do not change systemd timer cadence, lock semantics, ChromeMCP behavior, or guard routing in the same patch as the snooze fix. Those are separate risk surfaces.

- [ ] Before merging any overnight-system change, run the fixture tests plus one manual dry-run transcript and paste the output into the commit message or TODO stamp.
