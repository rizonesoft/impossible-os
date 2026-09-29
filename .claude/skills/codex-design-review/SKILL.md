---
name: codex-design-review
description: Pre-implementation design review via Codex. Before writing code for a TODO section, have Codex review the plan for feasibility issues, missing edge cases, SMP hazards, and architectural problems. Catches design flaws before implementation time is spent. Use before implementing a complex or high-risk section.
---

# Codex Design Review

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Prompt Shape

Every dispatch from this skill MUST open its prompt with the marker `[review-kind: design] <todo-path>` on the first non-blank line. The marker is what `.claude/hooks/skill_step_observer.py` and the section-commit four-dispatch gate use to attribute the dispatch. Un-marked dispatches waste a Codex round and block the next section-commit. Canonical reference for all 8 markers: [.claude/skills/codex-prompt-shape.md](../codex-prompt-shape.md).

## Use This Skill When

- About to implement a TODO section that touches SMP-sensitive, boot-path, page tables, interrupt handling, or security-critical code.
- The section's plan looks complex and you want a second opinion before writing code.
- The user asks for a design review or feasibility check on a planned section.

## Skip When (with `SKIP_DESIGN_REVIEW_HOOK=1` env var)

The `design_review_required.py` PreToolUse hook is the always-on backstop -- it BLOCKs Edit/Write on code targets after `implement-todo-section` is invoked unless a design dispatch has fired. To legitimately skip the review, set `SKIP_DESIGN_REVIEW_HOOK=1` on the next tool call AND make sure the section actually fits one of these:

- **Docs-only / markdown-only sections** -- no `.c`/`.h`/`.asm`/`.py` edits.
- **Stamp-only edits** -- adding `> **Verified:**` / `> **Quality reviewed:**` lines without code changes.
- **Pure constant additions** -- a single `#define` or enum value, no logic.
- **Test-only work** -- new `src/kernel/test/test_*.c` registration with no production-code edits.
- **Single-function plumbing** -- a wrapper that delegates to an already-reviewed primitive, no new state, no SMP surface.

Anything else dispatches. "It's host-side Python so no SMP risk" is NOT a skip criterion -- host-side concurrency, lifecycle, and state machines benefit from the design pass (TODO-07 §11/§12 incident).

## Size Discipline (10-min Bash wall)

The Codex review runs as a foreground Bash call, which has a 600000ms (10 min) hard wall. Design reviews historically hit this wall when prompts dump full file contents or carry too many open-ended questions. **Keep the dispatch tight:**

- **Integration surface = names only.** List file paths + function/struct/symbol names that the section will modify or call into. Do NOT paste file contents -- Codex's app-server reads files itself when it needs them. Pasting bloats the prompt without speeding reasoning.
- **Plan = checklist items as written.** Copy from the TODO section verbatim; do not re-narrate.
- **Questions = max 4, most important first.** Open-ended "is this sound?" + 2-3 concrete risks specific to this section. Drop generic questions Codex would ask itself.
- **Constraints = 3-5 lines.** The kernel is freestanding + SMP-safe + identity-mapped + bare-metal. Do not re-explain CLAUDE.md.
- **No section repetition.** If the section's TODO text already states a constraint, don't restate it under "Constraints".

A well-shaped design-review prompt is typically under 80 lines. If yours is over 150 lines, you are pasting file contents -- trim before dispatching.

## Background Fallback (when foreground hits the 10-min wall)

If a tightly-scoped foreground dispatch still hits Bash's 600000ms wall and gets truncated, fall back to background mode. **Do not reach for this preemptively** -- foreground gives you the result in one Bash call; background requires polling. Use it only after a foreground attempt was killed.

**Foreground (default):**
```bash
bash scripts/codex-dispatch.sh '[review-kind: design] <todo-path> <design review prompt>'
```

**Background fallback (after foreground timeout):**
```bash
PROMPT=$(cat <<'EOF'
[review-kind: design] <todo-path> <design review prompt>
EOF
)
bash scripts/codex-bg-dispatch.sh "$PROMPT"
```

The wrapper invokes `codex-companion.mjs task --background --json` (the only background-capable subcommand) and returns immediately with `{jobId, logFile, status}`. The detached worker continues reasoning past the 10-min wall.

**Poll for completion** (each call blocks up to 4 min, repeat until `status: "completed"`):
```bash
node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" status <jobId> --wait --timeout-ms 240000
```

When `status: "completed"`, the same JSON contains `finalMessage` (the review text). If you prefer a single non-blocking check, drop `--wait`.

**Receive integrity caveat.** Background dispatch is a fallback reviewer path, not shipping proof by itself. `codex_review_completed.py` can record the trigger state, but TODO-10 §7 owns normalizing background completion and trusted receipt binding before shared gates may accept it as reviewer evidence. Read the result and apply `superpowers:receiving-code-review` before acting on any finding.

**Stale recovery:** if a background dispatch is abandoned (session crash, agent confusion), cancel the job via `node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" cancel <jobId>`. Wrapper-launched background reviews are manual poll-and-receive only until TODO-10 §7 normalizes completed-output binding.

**Wait discipline (double-poll ban, 2026-07-03).** One wait mechanism per wait. Prefer the CANONICAL waiter `bash scripts/overnight/wait-for-codex-verdict.sh <log>` (B1): it bounds ITSELF under the Bash tool's ~120s default, so a bare call is NEVER killed at 2m -- exit 0 = DONE (prints the verdict tail), exit 3 = STILL RUNNING (and, B2, reports each pending log's byte size + how long since it last grew), just call it again (each call is one idle turn, review runs detached). Exit 4 = STALE: a pending log produced NO new output for `--stale-secs` (default 300s) and is likely hung -- re-dispatch ONLY that log and keep waiting on the rest; do NOT abandon a review still on exit 3 (it is slow-but-alive, which is the B2 signal the runner previously could not see). Do NOT hand-roll `for i in $(seq 1 N); do ... sleep S; done`: those are 540-900s and die at the 120s default every session unless you remember a `timeout:` arg. For a single LONG wait in ONE turn, pass `--max 540 <log>` WITH an explicit Bash `timeout: 600000`. **In the HEADLESS overnight run the long form is MANDATORY (R3, 2026-07-19): `codex_wait_discipline.py` BLOCKs a waiter call without `--max >= 300` + a matching tool `timeout` -- one 9-min wait turn instead of a 51-poll loop (measured run-20260719-022200). Interactive sessions may still use short bare calls.** If a Monitor (or background Bash) is armed instead, HOLD until it fires: no manual `Check ... status` re-polls, no per-poll narrator turns (measured 2026-07-02: 531 holding turns + 610 polls against 40 Monitor arms, ~1,100 wasted turns). Unavoidable manual polls (Monitor timeout, unknowable ETA under host load) run at a 30-60s cadence, never ~10s. Waiting on several review kinds at once: pass ALL logs to the waiter in one call (`bash scripts/overnight/wait-for-codex-verdict.sh f1 f2 f3` -- returns DONE only when every log completed), never serial per-kind poll clusters. **The per-dispatch `.out` artifact (and `review-envelope.py` over it) IS the authoritative review body -- NEVER glob `~/.codex/sessions/**/*.jsonl` for the "full" review (E2).** The Codex thread-id printed in the `.out` does NOT map to a session filename, so that glob returns an unrelated FOREIGN session (measured: a Conclave run misread as the verdict). The `.out` already carries the verdict + severity findings + Next-steps; slice-read it for detail. When reading the envelope, scope it with `--todo <this-section's-todo-path>` (E3) so the accumulating manifest cannot serve a prior section's stale review. This rule applies to every codex-* skill's background waits; they reference this section.

## Workflow

1. **Read the TODO section** -- full text, checklist items, test checkpoint, and all `-> XREF:` dependencies.
2. **Read the integration surface** -- use Grep/Glob to find the existing code that the section will modify or depend on. Identify key structs, APIs, and call paths.
3. **Build a design review prompt** covering:
   - The section's goal and all planned checklist items
   - The existing code it integrates with (file paths, key functions)
   - Known constraints (SMP safety, freestanding kernel, identity mapping, bare metal)
   - Specific questions: "Is this approach correct?", "What edge cases are missing?", "What could this break?"
   - **When the section parses a standard format** (Markdown, HTML, URLs, HTTP headers, JSON, a spec'd binary format), name the reference implementation the plan will CALL and ask the reviewer to object to every hand-rolled part. "Which existing parser should this delegate to?" is the question; "what edge cases does my parser miss?" invites a spiral. Measured 2026-09-29, TODO-10 section 23: 20 review rounds and 62 dispatches, most of them teaching hand-written parsers the specs they approximated; every spiral ended only when the real parser (markdown-it, `html.parser`, Node's WHATWG `URL`) replaced the approximation.
4. **Dispatch to Codex plugin:**
   ```bash
   bash scripts/codex-dispatch.sh '[review-kind: design] <todo-path> <design review prompt>'
   ```
   Frame as design review, not code review -- Codex should analyze the PLAN, not existing code.
5. **Triage findings** -- the PostToolUse hook fires `receiving-code-review` reminder; follow it. **Design-review false-positive watch:** Codex assumes general-OS conventions (e.g., "you need a wait queue here") that don't apply to this freestanding kernel (we use polled completion in init paths); demands "production-ready" configurability the section's spec doesn't require; flags "this will conflict with X" without verifying that X actually behaves the way Codex claims. Read the integration surface files yourself before accepting any conflict claim.
6. **Classify each verified finding:**
   - **Blocker** -- the plan has a verified fundamental flaw that would require redesign after implementation. Fix the TODO section before implementing.
   - **Warning** -- the plan is viable but has verified edge cases or risks to handle during implementation. Note them for the implementer.
   - **Reject** -- finding is wrong or misapplied to this codebase. Document the technical reason.
   - **Clear** -- no significant issues found. Proceed to implementation.
7. **Update the TODO section** if blockers or warnings were found:
   - Add missing checklist items for edge cases Codex identified (verified ones only)
   - Add `> [!WARNING]` callouts for risks
   - Adjust the approach if Codex found a fundamental flaw

## Prompt Template

Follow the size discipline above. Names only, not file contents.

```
Design review for TODO-XX §N: <section title>

Plan (verbatim from TODO):
<paste checklist items, no re-narration>

Integration surface (names only):
- <file_path>: <function/struct names this section modifies or calls>
- <file_path>: <...>
(2-3 files max; Codex reads them itself)

Constraints:
- Freestanding kernel, SMP-safe, identity-mapped, bare-metal
- <any section-specific constraint not in CLAUDE.md>

Questions (pick 3-4 most relevant):
1. Is this approach architecturally sound for <specific concern>?
2. What edge case or failure mode in <specific area> is the plan missing?
3. <one concrete risk specific to this section>
4. <one concrete risk specific to this section>
```

Drop generic "what could break / SMP hazards / simpler alternatives" -- Codex evaluates those automatically. Spend the question budget on section-specific risks.

## Guardrails

- This is a PLAN review, not a code review. Do not dispatch if the code is already written -- use `codex-adversarial-review-section` instead.
- Do not implement code in this skill. Only modify the TODO section text.
- If Codex returns "all clear," proceed with implementation. Do not re-review endlessly.
