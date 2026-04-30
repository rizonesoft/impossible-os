# Scope-Gap Protocol

> Shared by `implement-todo-section` and `verify-todo-section`. Step 6 of both skills walks this protocol when a scope gap is detected. Edits to this file require re-reading both `SKILL.md` files (the existing mirror-sync hook will nudge).

## The Failure Mode

A TODO section takes a feature 80% of the way, and the remaining 20% isn't covered by any existing TODO. The implementer reaches the end of the section's checklist items, the user-visible feature still isn't complete, and the natural-but-wrong response is to:

- Drop a `// TODO: handle BMP later` comment in the source.
- Return `STATUS_NOT_IMPLEMENTED` from the missing path.
- Narrow a switch statement to the formats the section listed (`if (format != PNG && format != JPG) return ERROR;`).
- Write a stub function and hope a future TODO fills it in.

The canonical example: an "image viewer" section listing PNG and JPG decoders. The user-visible feature implies BMP support too. There is no "BMP support" item in this section, no other TODO covers BMP, and the implementer ships PNG/JPG with a stubbed BMP path. Months later, no one remembers.

**Gate 10 of `kernel-code-quality` already forbids `// TODO`/`// FIXME`/`// HACK` comments in shipped code.** This protocol is the structured alternative: when a gap is detected, the paper trail goes into the TODO file (where it is searchable, auditable, and visible to the next implementer), not into a source comment (where it is invisible to anyone not reading that exact file).

---

## A.1 Detection Signals

The protocol fires the moment ANY of these appear during step 6 (implement) or step 6 (verify). All other work stops; walk the decision tree below.

### Code-shaped (you are about to type it)

- `// TODO`, `// FIXME`, `// HACK`, `// XXX` comments (line or block).
- `// for now`, `// placeholder`, `// stub` comments.
- `return STATUS_NOT_IMPLEMENTED;`, `return E_NOTIMPL;`, `return -ENOSYS;`.
- A function body with only a log line and an unimplemented return.
- Identifiers containing `stub`, `nop`, `dummy`, `placeholder`, `unimplemented`.
- A hardcoded fallback value the implementer thinks of as "real result later".
- A switch/conditional that narrows supported input below what the section's user-visible behavior promises (the PNG/JPG-but-not-BMP shape).

### Behavioral (what the user will see)

- The section's checklist items, read literally, promise behavior X, but the code path can only produce X for a subset of inputs.
- A loader/parser/handler that silently drops cases the spec says it should handle.
- A feature that "works" only when reached via one code path, not all code paths that should reach it.

### Architectural (what the integration surface looks like)

- About to add a new public function whose body just delegates to the old behavior (wrapper-without-body).
- About to register a handler in a dispatch table without implementing the handler body.
- About to add a field to a struct and leave every reader of that field unchanged.

**Hard rule:** if any signal fires, the implementer MUST stop writing code and walk A.2 (sizing) -> A.3 (decision tree). No `// TODO`-and-move-on allowed.

---

## A.2 Sizing Card (three tests)

All three must pass for Branch A. Any failure routes to Branch B/C/D.

1. **Description test:** Can you describe the missing work in 1-3 sentences?
   - No -> Branch B/C/D.
2. **File-touch test:** Will the expansion edit how many files (source + headers + tests)?
   - `<= 5 files` -> Branch A eligible.
   - `6-10 files` -> borderline; lean toward deferral.
   - `> 10 files` -> Branch B/C/D.
3. **Subsystem test:** Does the expansion stay inside the same subsystem as the current section?
   - Same subsystem -> Branch A eligible.
   - Crossing subsystems (e.g., kernel -> user-mode, MM -> FS, scheduler -> drivers) -> Branch B/C/D regardless of line count.

### Tiebreak rule (load-bearing)

At the **900-1100 line boundary**, default to deferral (Branch B/C/D), NOT inline. The implementer may override only with an explicit one-line justification in the NOTE callout.

**Rationale:** commits that drift past 1000 lines violate one-section-one-commit discipline. Deferring is cheaper to unwind than a sprawling commit.

### Mid-stream abort

If Branch A is in progress and the expansion grows past the threshold while writing, **roll back the uncommitted expansion code and restart with Branch B**. This is the one place in the pipeline where backtracking is cheaper than continuing.

---

## A.3 Decision Tree

```
Signal fires (A.1)
  |
  v
1-3 sentences? <=5 files? same subsystem? <1000 lines (with 900-1100 tiebreak = defer)?
  |
  YES (clear) -> BRANCH A. Report: "scope gap detected, sizing X/Y/Z, taking Branch A inline."
  NO  (clear) -> Branch B/C/D dispatch (next box).
  BORDERLINE  -> ASK USER which branch (sizing report + recommendation).
                 |
                 v
Branch B/C/D dispatch:
  Gap clearly fits THIS TODO file? -- YES --> Branch B (new section in same file)
  |
  NO
  v
  Run dedup sweep (A.5.1). Strong match in existing TODO? -- YES -> Branch D
  |
  NO match
  v
  Branch C (new TODO file via /create-todo scope-gap mode)
```

**Branch decision authority:** Claude decides automatically for clear cases, reports the choice in chat, and asks the user only when sizing is borderline (900-1100 lines, ambiguous subsystem boundary).

---

## A.4 Branch A: Inline Expansion

**Conditions:** under 1000 lines, same subsystem, clearly described in 1-3 sentences.

### Steps

1. **Announce** in chat:
   ```
   Scope gap detected: section promises <X> but only covers <Y>.
   Sizing: <N sentences>, <M files>, same subsystem (<subsystem name>), estimated <K> lines.
   Taking Branch A (inline expansion).
   ```
2. **Write the code** under normal kernel-code-quality gates. NO `// TODO` comments in the expansion -- Gate 10 still applies.
3. **Add a `[x]` checklist item** to the CURRENT section:
   ```
   - [x] Scope-gap expansion (Branch A): <one-sentence description of what was added and why the original items did not cover it>
   ```
4. **Add a `> [!NOTE]` callout** near the top of the section if the expansion is user-visible or surprising:
   ```
   > [!NOTE]
   > **Scope-gap expansion (Branch A):** Original items covered <X>. During implementation, <missing gap> was discovered: under 1000 lines, same subsystem, clearly describable, so implemented inline. See new checklist item below.
   ```
   Skip the NOTE only if the gap is trivial (e.g., adding an enum value for a format already parsed generically). Default to including it.
5. **Commit message** documents the expansion explicitly. Add a line in the commit body:
   ```
   scope-gap: inline expansion -- <what was added>
   ```
6. **Mid-stream abort:** if the expansion grows past the 1100-line tiebreak threshold, roll back uncommitted changes and restart with Branch B.

### Example

A "Print-to-File for shell" section listed only PDF, but the shell printer backend also supports PostScript and plain TXT. Branch A applies because:
- Description: "Add PS and TXT format handlers to shell print backend."
- Files: `src/shell/print/print_ps.c`, `src/shell/print/print_txt.c`, `src/shell/print/print.h` -> 3 files.
- Subsystem: same (`src/shell/print/`).

The PS and TXT handlers are written inline. A new `[x]` item is added: `- [x] Scope-gap expansion (Branch A): PS and TXT format handlers (originally only PDF was listed)`. A NOTE callout is added at the top of the section. The commit message includes `scope-gap: inline expansion -- PS and TXT print handlers`.

---

## A.5 Branch B: Defer to a New Section in the Same TODO File

**Conditions:** gap belongs in this TODO file, is too big for inline (>= 1000 lines), OR is borderline at 900-1100 and the implementer chose conservative deferral.

### Steps

1. **Create `## N+1. <Title>`** appended to the current TODO file (or inserted at the logical position). Use the normal section structure: intro line, checklist items, `Commit:` line, Test checkpoint.
2. **Add the new row to the Implementation Order table** with `[ ]` status. Use `section-N` notation for its dependencies (the originating section becomes one of them).
3. **Add a Follow-up line to the CURRENT section:**
   ```
   - [ ] Follow-up (Branch B -> section N+1): <gap description>
   ```
4. **Mark affected current-section items `[/]`** (in-progress / depends-on-something-else), NOT `[x]`. This is load-bearing: `[x]` is a lie when the gap remains.
5. **Add `> [!NOTE]`** at the top of the current section explaining the split:
   ```
   > [!NOTE]
   > **Scope-gap deferral (Branch B -> section N+1):** <gap description>. Size or risk exceeded the Branch A inline threshold; split into section N+1 in this file. Items <X>, <Y> below are `[/]` until section N+1 lands.
   ```
6. **OS Comparison table:** if the gap affects a parity claim, downgrade the row from `Done` to `Partial` with a `section N+1` reference.
7. **Validation:** the current section's `Commit:` item cannot be `[x]`'d until the implementer confirms the Implementation Order table, `[/]` markers, and Follow-up line are all in place. Steps 10 and 18 of the pipeline check this.

---

## A.5.1 Dedup Sweep (Mandatory before Branch C)

Before Branch C creates a new TODO file, run this sweep. Time budget: ~30 seconds. If it takes longer, escalate to the `gap-audit-todo` skill instead.

### Steps

1. **Grep all TODO files** for the gap's distinguishing keywords. Use the project's `Grep` tool, not `Bash(grep)`:
   ```
   Grep "<feature keyword>" path=todo
   Grep "<function name the gap would introduce>" path=todo
   Grep "<type name the gap would introduce>" path=todo
   ```
2. **For each match, read the OS Comparison row** in that file. Does it claim the feature already? If yes, that file likely owns the scope.
3. **Classify results:**
   - **Zero matches** -> proceed to Branch C.
   - **Weak matches (mentioned but not owned)** -> still Branch C, but add `-> XREF` from the new TODO to the mentioning files, with reciprocal back-references.
   - **Strong match (existing TODO clearly owns the scope)** -> switch to Branch D.

### Why this is mandatory

A duplicate TODO is expensive to unwind: it fragments the paper trail, confuses future implementers about which file owns the work, and creates conflicting checklists. Branch D exists specifically to prevent this.

---

## A.6 Branch C: New TODO File via /create-todo

**Conditions:** gap spans multiple subsystems or is clearly a feature that needs its own roadmap, AND the dedup sweep returned zero or weak matches.

### Steps

1. **Confirm the dedup sweep is complete and clean** (A.5.1). If you skipped the sweep, stop and run it now.
2. **Invoke `/create-todo` with scope-gap mode.** This is a documented entry point in the create-todo skill (see its "Scope-Gap Mode" workflow sub-section). Mode flag tells create-todo to:
   - Pick up the originating section's context as the Inputs block starting point.
   - Auto-insert the originating section into the new TODO's `-> XREF` as a bidirectional link.
   - Mark the new TODO's row in the domain `INDEX.md` with `(created from scope-gap in <originating TODO> section N)`.
   - Otherwise run the full normal create-todo workflow (research, OS comparison, tests, verification).
3. **Back in the originating section:**
   - Add a Follow-up line:
     ```
     - [ ] Follow-up (Branch C -> XREF: TODO-XX section 1): <gap description>
     ```
   - Mark affected items `[/]`.
   - Add a `> [!NOTE]` callout (same shape as Branch B, citing the new TODO file instead of `section N+1`).
   - Update the OS Comparison row if parity status changed.
4. **Bidirectional XREF rule applies** (already required by create-todo step 3). The new TODO file MUST link back to the originating section.

---

## A.7 Branch D: Add to an Existing TODO

**Conditions:** the dedup sweep found a strong match in an existing TODO file.

### Steps

1. **Do NOT create a new TODO file.** This is the whole point of the dedup check.
2. **Choose the target section** in the existing TODO:
   - If the target section is `[ ]`: add specific checklist items describing the gap.
   - If the target section is `[/]`: add items and update its NOTE callout to reference the new gap source.
   - If the target section is `[x]`: this is actually Branch B applied to the OTHER TODO -- create a new section `## K+1. <Title>` in that TODO. Follow Branch B's rules, but in the target file.
3. **Bidirectional XREFs:**
   - Originating section gets:
     ```
     - [ ] Follow-up (Branch D -> XREF: TODO-XX section K): <gap description>
     ```
   - Target section gets a reciprocal `-> XREF: <originating TODO> section N` line.
4. **Mark originating items `[/]`** as per Branch B/C.
5. **No new TODO file is created. No `INDEX.md` updates** beyond whatever the target TODO's section change requires.

---

## FAQ

**Q: I discovered the gap AFTER step 6 but before step 13 (Codex adversarial review). What now?**

A: Stop, go back to step 6, walk the protocol from the top. The protocol is not skippable by virtue of being late. The whole point is that the gap gets a paper trail before commit -- discovering it at step 12 is just as valid as discovering it at step 6.

**Q: Can verify-mode fix scope gaps inline?**

A: Yes, but only under Branch A criteria (under 1000 lines, routine, same subsystem) AND the NOTE callout must be added retroactively. This is the "one implementation action allowed" allowance in verify-mode step 6, identical in spirit to step 8's routine test gap-fill allowance. Anything bigger -> verify-mode flags the gap, downgrades affected items from `[x]` to `[/]`, and produces a follow-up list for the user to triage via implement-mode on a later run.

**Q: What if Codex flags the gap during step 13 review?**

A: Treat the Codex finding as the detection signal. Walk the protocol then. If the fix is small and routine, it goes through the normal step 14 fix loop. If the fix reveals a deeper gap, escalate to Branch B/C/D as appropriate.

**Q: The hook blocked my edit because my test file uses `STATUS_NOT_IMPLEMENTED`. How do I unblock?**

A: Two options:
1. The hook excludes `src/kernel/test/` automatically -- if your file is under that path, the hook should not have fired. Re-check the path.
2. For legitimate non-test cases (a sentinel for hardware-not-yet-supported, a probe function that intentionally checks the unimplemented path), add a `/* SCOPE-GAP-ALLOWED: <one-line reason> */` comment to opt out. The sentinel is searchable -- run `grep -r SCOPE-GAP-ALLOWED src/` periodically to audit usage and make sure it isn't being abused.

**Q: Is `-ENOSYS` a detection signal?**

A: No. `-ENOSYS` is a legitimate POSIX error return for genuinely unsupported operations on a Linux compatibility layer. Syscall handlers that intentionally return ENOSYS for the unimplemented portion of a partial syscall are correct. The `STATUS_NOT_IMPLEMENTED` rule covers the NT-side equivalent, which is the more common pattern in this codebase.

**Q: How do I commit a Branch A expansion alongside the original section work?**

A: Single commit, one section equals one commit. The commit message body has two lines: the section's `Commit:` line, plus a `scope-gap: inline expansion -- <what>` line. Both the original section work and the inline expansion ship together.

**Q: My branch-B/C/D stub returns `STATUS_NOT_IMPLEMENTED`. Should the stub log a runtime warning on every call?**

A: No. Add a `TEST_PENDING(cond, msg)` test instead -- the test framework's `pending` bucket + `[STUB]` log line is the canonical "what is incomplete?" registry. A runtime klog warning in the stub body just duplicates the test signal and creates N noise lines per boot. The pattern:

```c
/* In the stub function: */
NTSTATUS NtFooBar_handler(...) {
    /* SCOPE-GAP-ALLOWED: pending TODO-XX §N (subsystem name). */
    return STATUS_NOT_IMPLEMENTED;  /* no klog -- TEST_PENDING owns the signal */
}

/* In the corresponding test: */
static void test_foo_pending_features(void) {
    NTSTATUS st = ssdt_dispatch(SSDT_NtFooBar, ...);
    /* Message: name + slot + brief gap. ASCII only. NO TODO refs --
     * they drift; the source comment above the SCOPE-GAP-ALLOWED
     * stub is the durable record. */
    TEST_PENDING(st == STATUS_NOT_IMPLEMENTED,
                 "NtFooBar (0xNN): no <subsystem> yet");
}
```

End-of-run summary then surfaces the pending count so a glance answers "how many features are still incomplete?". See `implement-unit-tests` skill -- "When to use TEST_PENDING vs TEST_ASSERT" section for the full message-format rules (NO TODO refs, ASCII only, under 50 chars, suite name carries subsystem).

---

## Mirror Note

This document is shared by `implement-todo-section/SKILL.md` and `verify-todo-section/SKILL.md`. Edits here propagate to both skills' step 6. The existing mirror-sync hook fires when either `SKILL.md` is edited, but this file is not under the hook's match. **When you edit this file, manually re-read both SKILL.md step 6 sub-bullets** to confirm the references and language still align.
