---
name: codex-perf-review
description: Codex-driven performance hot-path review. Focused analysis of ISR paths, spinlock hold times, allocation-in-loop patterns, and O(n^2) algorithms in kernel code. Use when implementing performance-sensitive subsystems or when serial output shows unexpected latency.
---

# Codex Performance Review

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

3. **Evaluate findings by category:**
   - **Allocation in hot path** -- `kmalloc`/`pmm_alloc_frame` called from ISR or per-tick code. Pre-allocate instead.
   - **O(n^2) or worse** -- nested loops over the same data, linear scan where binary search or hash suffices
   - **Lock contention** -- spinlock held across I/O, allocation, or klog. Minimize critical section.
   - **Byte-at-a-time operations** -- `memcpy` or string copy one byte at a time where bulk `rep movsb` or word-size copy exists
   - **Unnecessary TLB flush** -- `vmm_flush_tlb_all()` where a single-page `invlpg` suffices
   - **Cache-hostile patterns** -- struct fields accessed together but split across cache lines; array-of-structs where struct-of-arrays is better
   - **Redundant computation** -- same value computed multiple times in a loop; hoist out of loop
   - **klog in hot path** -- `LOG_DEBUG` calls in per-tick or per-interrupt code. Even when filtered, the format-string evaluation still runs.

4. **Classify severity:**
   - **Critical** -- causes measurable latency on every interrupt/tick (> 1 us per invocation)
   - **High** -- causes latency on common operations (allocation, exec, file open)
   - **Medium** -- causes latency on uncommon paths (error handling, edge cases)
   - **Low** -- micro-optimization with no measurable impact

5. **Fix critical/high findings.** Accept medium/low with justification.

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
