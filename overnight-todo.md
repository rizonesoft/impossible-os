# Overnight TODO: Operator Punch-List

Human-gated items the overnight runner cannot resolve on its own. Two kinds:

1. **Waiting on a human answer** -- a decision, approval, or judgment only a
   person can make: a design choice, a dependency approval, Secure Boot /
   bare-metal sign-off, or a product call.
2. **Unblockable once the runner is stopped** -- work that is fine to do but
   needs the shared git index free, a pause in the autonomous build loop, or a
   host capability the headless run lacks.

**For the runner (append here, do not silently defer-and-forget):** when you hit
either case, in ADDITION to the normal `[/]` + Deferred stamp + XREF in the
owning TODO, append a one-line entry to THIS file under the matching kind --
name the blocker and the exact follow-up. This file is the human's single
check-when-I-sit-down list. Add an item when:
- something genuinely needs a human answer/approval you cannot supply, OR
- something is blocked only by the run being live (index busy, build loop,
  missing bare-metal/Secure-Boot host) and becomes doable the moment it stops --
  e.g. reviewing/committing third-party source vendored to unblock wiring.

> Check the runner is stopped before acting on anything here:
> `systemctl --user is-active overnight-impossible-os.service` should NOT print `active`.

---

## Waiting on a human answer

### UKI SBAT incremental-rebuild bug (Secure Boot signing path)

`bash scripts/build.sh` run twice without `clean` fails the second time at
`[sbat] FATAL: cannot dump .sbat`. Root cause: the UKI pack step runs
`objcopy --add-section` on `build/tools/BOOTX64.EFI`, which an incremental build
leaves in the *signed* state from the prior build; objcopy corrupts the signed
PE's section table so `.sbat` can no longer be dumped. Dormant unless Secure Boot
signing is active (which is why dev/runner builds currently pass). Fully
diagnosed and reproduced; human-gated (SB trust chain + bare-metal verification
per CLAUDE.md Safety Gates). Tracked in
`todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md`, section 18.

- [ ] Decide the fix shape and record the rationale:
      - (A) sign to a distinct path, keep `build/tools/BOOTX64.EFI` unsigned (most correct)
      - (B) strip the signature from a temp stub copy (`sbattach --remove`) before packing
      - (C) `rm` the stub + UKI before the link step to force an unsigned relink (smallest, defeats incremental caching)
- [ ] Implement without changing what is signed, key material, or SB policy;
      incremental output must byte-match the clean-build output.
- [ ] Verify BOTH modes reach `=== BUILD OK ===` with `[sbat] OK`, and `sbverify`
      passes for `BOOTX64.EFI` and `BOOTX64.UKI.efi`.
- [ ] Verify the signed UKI boots and the SB chain validates on bare metal or a
      Secure-Boot-enrolled VM (cannot be validated in WSL TCG/KVM).
- [ ] Workaround until fixed:
      `rm -f build/tools/BOOTX64.EFI build/tools/BOOTX64.UKI.efi` before a build
      (or `bash scripts/build.sh clean`).

## Unblockable once the runner is stopped

### Land the skill-catalog CI fix

`.claude/skills/README.md` has a verified, uncommitted fix: the 5 specialist-agent
rows were changed from the skill-row backtick-link form `| [`name`](path) |` to
`| [name](path) |` so the AI-workflow catalog check stops counting them as skills
(it was reading 36 vs 31 dirs -> "README.md rows without a dir"). Regression is
back to 79 passed / 0 failed. Held because the runner was committing concurrently.

- [ ] With the runner stopped AND a lint-clean tree (no in-progress
      `src/kernel/lz4.c` / `Makefile` shorthand left mid-section), run:
      `git add .claude/skills/README.md`
      `git commit -m "fix(catalog): keep specialist-agent rows out of the skills README skill-set"`
      `git push`
- [ ] Confirm the "AI workflow regression" check goes green on `main`.
- [ ] (Optional) Commit this `overnight-todo.md` too if you want it tracked.

### Review the vendored third-party libraries

LZ4, miniz, and Mbed TLS source were vendored (commit `38525b91`) to unblock the
kernel-libraries wiring the runner is now doing. Third-party dependency additions
are a CLAUDE.md stop-and-ask gate -- they need a human pass before they are
load-bearing.

- [ ] Review the vendored source (`src/libs/lz4/`, `src/libs/miniz/`, `src/libs/mbedtls/`): license compatibility, upstream version pinned + recorded, no unexpected or modified files vs upstream.
- [ ] Confirm the vendoring + wiring is committed cleanly (no stray untracked `src/libs/...` / `include/libs/...` / `src/kernel/lz4.c` working-tree files left from a mid-section build).

### Fix COUNT.md auto-generation + auto-display the count in README.md

COUNT.md drifted stale (had to be hand-refreshed via `COUNT_ONLY=1 bash .githooks/post-commit`), so the post-commit auto-generation is not reliably keeping it current, and the project `README.md` line-count is not wired to it. Two pieces:

- [ ] Make COUNT.md generation reliable: confirm `.githooks/post-commit` fires on every kernel/source commit (the srclight `core.hooksPath` hijack has silently disabled this chain before; see memory `srclight-hooks-hijack`), and decide whether the count should also refresh outside commits (it only updates post-commit today, so the working tree drifts until the next commit).
- [ ] Auto-display the count in the project `README.md`: pull the Grand Total from COUNT.md into a generated, marker-delimited block the post-commit hook rewrites, so the README count is never hand-maintained or stale.
- [ ] Verify: a fresh commit updates BOTH COUNT.md and the README count block in one step, with no manual touch.
