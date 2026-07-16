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

### Forked ownership: SMEP/SMAP + KPTI + KASLR claimed by two TODOs

Found during the higher-half-relocation gap audit (2026-07-15). Two TODOs plan
the same capabilities with **no cross-reference in either direction**:

- [Kernel Security Hardening](todo/02-kernel-core/TODO-10-kernel-security-hardening.md)
  -- owns SMEP/SMAP CR4 activation (partial), the KPTI trampoline (shipped:
  `src/kernel/kpti_trampoline.asm`, commit ad2d05e2), KPTI SYSCALL/IDT/user-CR3
  (partial), PCID TLB tagging (partial), and KASLR (partial). Its blocked
  sections carry Deferred stamps pointing at higher-half relocation.
- [Memory Security Hardening](todo/03-memory-concurrency/TODO-02-memory-security.md)
  -- plans SMEP, SMAP, KASLR, and KPTI as unstarted work, zero stamps, marked
  `status: active`.

The kernel-security TODO is demonstrably the live owner (it has the code and the
stamps); the memory-security one reads as the stale duplicate. **The decision
needed:** supersede the memory-security TODO's SMEP/SMAP/KASLR/KPTI sections in
favor of the kernel-security TODO (leaving it to own user-space ASLR, NX/DEP,
CET, and the security layout report), or the reverse. This is a cross-domain
roadmap call touching two TODO files, so the runner did not make it
unilaterally; the
[higher-half relocation TODO](todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md)
meanwhile cross-references both and names the kernel-security TODO as live.

### Bare-metal sign-off: higher-half kernel relocation (per-section)

The [higher-half relocation TODO](todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md)
defers bare-metal validation to a human pass per its runner-autonomy policy.
Needing real-hardware sign-off once they ship: the higher-half bring-up (early
paging + the CR3 switch differ on real CPUs), AP high-half bring-up (GS_BASE
ordering), the framebuffer high handoff (real GOP), the per-process PML4 split,
and the user-base move. The optional 5-level paging (LA57) section needs an
Arrow Lake / Zen 5 host.

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

All prior items in this section were completed on 2026-07-16 (runner disarmed):

- **Skill-catalog CI fix** -- already landed; `scripts/test-ai-system.sh` reports
  79 passed / 0 failed. (Was already committed before the sweep; the entry was
  stale.)
- **`_is_self_teardown` "skill "/"kill " false positive** -- fixed with a
  word-boundary regex + a both-directions regression test in
  `test_phase_guard_wait.py`. Committed `a8417a43`; `run-all.sh` 42 passed / 0
  failed.
- **Vendored third-party libraries** -- LZ4 1.10.0 (BSD-2-Clause), miniz 11.0.2
  (MIT), Mbed TLS 3.6.2 (Apache-2.0 OR GPL-2.0-or-later), cJSON 1.7.18 (MIT)
  reviewed and approved as load-bearing (all GPL-3.0-only compatible).
  `src/libs/PROVENANCE.md` records upstream + version + license per lib; added
  the missing cJSON `LICENSE`. Committed with the provenance record.
- **COUNT.md auto-gen + README count** -- `core.hooksPath` confirmed `.githooks`
  (no srclight hijack); the README lines badge is now regenerated from
  `core_total_lines` by `.githooks/post-commit` into a `COUNT-BADGE` marker
  block, amended alongside COUNT.md in one commit. Committed `fa6e7d0a`.

### Re-record the attended canary before the next unattended arm

The `_is_self_teardown` fix touched `.claude/hooks/run_phase_guard.py`, a
FLOW-CRITICAL control-plane file. Per guardrail Layer 4, the next unattended arm
will REFUSE until the canary is re-recorded against the new HEAD (or overridden
with `--force`).

- [ ] Run an attended session; watch >=1 section ship + a rollover -> relaunch.
- [ ] `bash .claude/skills/overnight-sequencer/arm-sequencer.sh --record-canary "<note>"`
- [ ] Then arm normally. (Or, accepting the risk, arm now with `--force`.)
