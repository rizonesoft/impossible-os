---
name: codex-perf-review
description: Codex-driven performance hot-path review. Focused analysis of ISR paths, spinlock hold times, allocation-in-loop patterns, and O(n^2) algorithms in kernel code. Use when implementing performance-sensitive subsystems or when serial output shows unexpected latency.
---

# Codex Performance Review

> **External-Reviewer Contract:** Codex is a subordinate reviewer, not authority. Every finding from this skill goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot).

## Use This Skill When

- Implementing code in the ISR path, scheduler, or compositor (hot paths).
- A subsystem shows unexpected latency in serial timing output.
- After implementing a section that adds loops, allocations, or lock-protected regions.
- The user asks "is this fast enough?" or "what's slow here?"

## Workflow

1. **Scope the review:**
   - Hot path: specific functions in the interrupt/scheduler/compositor path
   - Subsystem: all functions in a `.c` file
   - Cross-cutting: a call chain from entry point to leaf function

2. **Dispatch to Codex plugin:**
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<perf review prompt>"
   ```

3. **Evaluate findings with `superpowers:receiving-code-review` discipline** -- perf optimizations are the easiest place to add complexity for no measurable benefit. For each finding:
   - **Verify the path is actually hot.** Read the function. Is it really called from an ISR/per-tick path, or only from init/error paths? Codex flags allocations in functions that "look hot" but may run once at boot.
   - **Verify the cost.** Is the allocation/lock/copy actually expensive enough to matter? A `kmalloc` call in a path that runs at 10 Hz is not a problem.
   - **Don't sacrifice correctness or clarity for micro-optimization.** A spinlock that's held "too long" but is also the only correct way to serialize the operation should stay. A linear scan over a 16-element array should not be replaced with a hash table.
   - **Reject "could be faster" without "is measurably slow."** Codex reviews structure, not measurements. Without a profile or serial-timing observation showing the path is actually slow, do not refactor.
4. **Classify severity for verified findings:**
   - **Critical** -- verified to cause measurable latency on every interrupt/tick (> 1 us per invocation)
   - **High** -- verified to cause latency on common operations (allocation, exec, file open)
   - **Medium** -- causes latency on uncommon paths (error handling, edge cases)
   - **Low** -- micro-optimization with no measurable impact -- reject these
   - **Reject** -- finding is on a cold path, or cost is not actually high; document why with code/observation evidence

5. **Fix critical/high findings only.** Accept medium with justification. Reject low and false-positives.

## Prompt Template

```
Performance review for <subsystem>.

Files: <list files>
Hot path: <describe the call chain, e.g., "IRQ -> isr_handler -> schedule -> switch_context">

Analyze:
1. Allocations (kmalloc/pmm_alloc_frame) in interrupt or per-tick paths
2. O(n^2) or worse algorithms (nested loops, linear scans of large arrays)
3. Spinlock hold times -- how much work is done under each lock? Any I/O, allocation, or logging?
4. Byte-at-a-time copy where bulk operations exist
5. TLB flushes (full CR3 reload vs single invlpg)
6. Cache-hostile access patterns (struct layout vs access order)
7. Redundant computation in loops (can it be hoisted?)
8. klog/printk calls in hot paths (even LOG_DEBUG has format overhead)

For each finding: file:line, what it does, why it's slow, suggested fix, estimated impact.
```

## Guardrails

- Do not optimize without measuring. If Codex flags something, verify it's actually on a hot path before changing it.
- Do not sacrifice correctness for performance. A fast race condition is worse than a slow lock.
- Do not add complexity for micro-optimizations. If the improvement is < 1 us and the code is clearer as-is, skip it.
- Focus on algorithmic improvements (O(n) -> O(log n)) over micro-optimizations (instruction scheduling).
- Apply `superpowers:receiving-code-review` discipline -- Codex flags structural perf risks, not measured slowness. Verify each finding is on an actually-hot path with actually-measurable cost before refactoring. "Could be faster" without "is measurably slow" is a reject.
