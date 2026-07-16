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

> **Resolved 2026-07-16 (operator decisions):**
> - **Forked SMEP/SMAP/KASLR/KPTI ownership** ->
>   [kernel-security-hardening](todo/02-kernel-core/TODO-10-kernel-security-hardening.md)
>   is the sole owner (it holds the shipped code + stamps). The overlapping sections
>   in [memory-security](todo/03-memory-concurrency/TODO-02-memory-security.md) (SMEP,
>   SMAP, KASLR, KPTI) are superseded and marked `[~]` with `> **Superseded by**`
>   notes; a reciprocal cross-reference was added in kernel-security-hardening.
>   Memory-security retains user-space ASLR, NX/DEP, CET, and the security-layout report.
> - **UKI SBAT incremental-rebuild bug** -> already fixed by commit `aa3c20db`
>   (2026-06-21, approach A: distinct signed-output paths in `scripts/sign-efi.sh`,
>   wired through the Makefile). The punch-list entry was stale; verified by a double
>   `bash scripts/build.sh` here (both runs `=== BUILD OK ===` + `[sbat] OK`, signing
>   active via `keys/MOK.{key,cer}`). The owning section in
>   [uefi-hardening-secureboot](todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md)
>   is stamped done; its two open follow-ups (bare-metal SB-chain acceptance, unify
>   signed-artifact predicate) are deferred there with cross-references.

### Bare-metal sign-off: higher-half kernel relocation (per-section)

The [higher-half relocation TODO](todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md)
defers bare-metal validation to a human pass per its runner-autonomy policy.
Needing real-hardware sign-off once they ship: the higher-half bring-up (early
paging + the CR3 switch differ on real CPUs), AP high-half bring-up (GS_BASE
ordering), the framebuffer high handoff (real GOP), the per-process PML4 split,
and the user-base move. The optional 5-level paging (LA57) section needs an
Arrow Lake / Zen 5 host.

> Acknowledged 2026-07-16 (operator): accepted as a standing gate. Nothing to
> validate yet -- the named bring-up sections are unshipped (only the memory-map
> design section has landed); this re-activates when they reach real hardware.

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
