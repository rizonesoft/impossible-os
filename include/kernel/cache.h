/* ============================================================================
 * cache.h -- Cache writeback primitives for durability across a hardware reset
 *
 * These exist for ONE property that ordinary barriers cannot provide: getting
 * bytes out of the cache hierarchy and into DRAM before the machine resets.
 *
 * `barrier.h`'s mb()/wmb()/rmb() order VISIBILITY between CPUs and do nothing
 * about durability -- on x86 every CPU already sees a store through the
 * coherent cache, so a modified line can sit dirty in cache indefinitely and
 * still satisfy every fence in the tree. That is fine until the machine
 * RESETS: Intel SDM Vol. 3A Table 9-1 footnote 6 states that internal caches
 * are invalid after power-up and RESET, and nothing in the SDM says RESET
 * writes modified lines back first. A crash record published with a plain
 * store into write-back memory and followed by a platform reset can therefore
 * never reach DRAM, and the next boot finds nothing. Every emulator this repo
 * gates on models no cache hierarchy, so the failure is invisible in QEMU,
 * KVM and WHPX and appears only on real silicon.
 *
 * THE ORDERING RULE FOR ANY CROSS-BOOT RECORD, and it is about WHEN THE WORD
 * IS STORED, not about the order of the flushes:
 *
 *   1. store a zero (unpublished) publication word, and flush its line;
 *   2. write the whole body and flush the whole record, while that word still
 *      reads zero;
 *   3. store the real publication word, and flush its line.
 *
 * A reset at any point then finds either no record or a complete one.
 *
 * Do NOT publish first and try to order the flushes behind it -- body lines,
 * then the publication line. That looks equivalent and is not: it holds only
 * where CLFLUSH gives line granularity, and collapses on the full-writeback
 * fallback, which commits many lines with no ordering boundary between them
 * and can therefore land a valid magic and CRC while later body lines are
 * still stale. Correctness must not depend on range granularity, and
 * publishing only after the body is durable is what removes that dependency:
 * there is nothing valid in memory for a reader to find early.
 *
 * Step 1 has one precondition: the record must have an owner. Durably clearing
 * a valid record is only safe where something arbitrates who may replace it,
 * because a writer that dies between the clear and the republish has destroyed
 * the previous record and written nothing. A single-slot region with no
 * ownership must therefore SKIP the step-1 flush and let step 2 carry the
 * clear -- a reader that catches the in-between state fails the CRC, which is
 * the same fail-closed outcome, without the destructive window.
 *
 * The API is architecture-neutral so arch-neutral callers (klog.c) can use it;
 * the x86-64 implementation lives in src/kernel/cache.c.
 *
 * PANIC-SAFE BY CONSTRUCTION. No locks, no allocation, no klog, no global
 * state and no initialization step. Every call probes the EXECUTING CPU, so
 * these are callable from any CPU at any point in boot, including before any
 * feature table has been built and from an AP whose features were never
 * enumerated.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Bounds a cache line size has to fall inside to be believed. CPUID reports it
 * in 8-byte units through an 8-bit field, so the representable range is 8 to
 * 2040 bytes; 4096 is the page-size ceiling above which a "line" is not one.
 * A value outside these bounds (or one that is not a power of two) means the
 * probe cannot be trusted, and the caller falls back to a full writeback
 * rather than flushing a stride it guessed. */
#define CACHE_LINE_MIN   8u
#define CACHE_LINE_MAX   4096u

/* Cache line size of the CALLING CPU, or 0 when targeted flushing is not
 * available -- either the processor does not enumerate CLFLUSH or the
 * reported size fails the bounds/power-of-two check above. 0 is the caller's
 * signal that there is no line granularity to order anything by. */
uint32_t cache_line_size(void);

/* PURE decoder behind cache_line_size(), exposed so its three rejections can
 * be driven from a unit test with synthetic CPUID register values. None of
 * them is reachable on a given host, and they are what keep the panic path
 * from executing an unsupported CLFLUSH or flushing on a stride that skips
 * dirty lines. Arguments are CPUID leaf-0 EAX and leaf-1 EBX/EDX. */
uint32_t cache_decode_line_size(uint32_t max_basic_leaf, uint32_t leaf1_ebx,
                                uint32_t leaf1_edx);

/* Plan modes returned by cache_plan_range(). */
#define CACHE_PLAN_NONE    0   /* nothing to do (zero length) */
#define CACHE_PLAN_RANGE   1   /* flush *out_lines lines from *out_first */
#define CACHE_PLAN_ALL     2   /* no line granularity, or a range that wraps */

/* PURE planner behind cache_writeback_range(), exposed for the same reason:
 * the address arithmetic it performs -- align-down, wrap detection, line
 * count -- is the part that can be wrong, and it is not observable from
 * outside a flush. Yields a line COUNT rather than an end address on purpose:
 * an end-bounded loop can wrap `p` past the top of the address space and never
 * terminate, and the kernel image window ends at UINT64_MAX. */
int cache_plan_range(uintptr_t addr, uint64_t len, uint32_t line,
                     uintptr_t *out_first, uint64_t *out_lines);

/* Write every cache line overlapping [addr, addr + len) back to memory,
 * fenced before and after so the flush observes prior stores and later work
 * observes the flush. A zero length does nothing.
 *
 * CALLER OWES A MAPPED, CANONICAL RANGE. CLFLUSH takes a memory operand, so it
 * raises #PF on a not-present page and #GP on a non-canonical address like any
 * other access. Every other panic hazard is closed inside this module -- no
 * locks, no allocation, no logging, no global state, and the CPUID gate that
 * prevents a #UD on a processor without CLFLUSH -- but this one cannot be, and
 * a fault raised in here destroys the evidence the call was made to save. In
 * tree the callers pass the PMM-reserved identity-mapped evidence page and a
 * crash region clamped to its own allocated size; a new panic-path caller must
 * be equally sure of its range.
 *
 * The fallback path is exempt: it issues no memory operand at all.
 *
 * When cache_line_size() reports 0 this falls back to cache_writeback_all(),
 * which is the strongest thing available without CLFLUSH and NOT an equivalent
 * guarantee: WBINVD writes back the executing processor's own caches, but the
 * SDM has it signal any external cache rather than wait for one, so the
 * fallback inherits that weaker completion property. It is a belt for a case
 * that does not arise on any x86-64 part this kernel targets -- CLFLUSH is
 * enumerated by all of them -- and the honest reading is "best effort where
 * targeted flushing does not exist", not "the same promise everywhere". */
void cache_writeback_range(const void *addr, uint64_t len);

/* There is deliberately NO variant taking a pre-probed line size. One was
 * written to save the two serializing CPUIDs per call on a path that makes
 * several ordered flushes, and removed again: a caller holding a probe result
 * across calls is only correct while it cannot migrate, and nothing in this
 * API can enforce that. A migrated caller can issue CLFLUSH on a CPU that does
 * not implement it (#UD, inside the panic path, destroying the evidence the
 * call exists to save) or reuse a larger stride and silently skip dirty lines.
 * The saving was measured at roughly 420 cycles on bare-metal Rocket Lake,
 * against a collector whose takeover budget is 200,000 spins -- not a rate
 * worth putting a pinning precondition on a public panic-path API.
 *
 * Probing per call NARROWS that window to one call rather than closing it: the
 * probe and the flush loop inside a single call are not atomic either, so a
 * preemptible caller migrated mid-loop still finishes on a CPU it did not
 * probe. Every in-tree caller is immune (the panic paths run under cli, and
 * klog crash recovery runs before SMP), and the consequence is nil on any
 * homogeneous x86-64 part. The honest claim is therefore "each call describes
 * the CPU that began it", which is what lets this be called from an AP and
 * before any feature table exists. */

/* Write the entire cache hierarchy back to memory. Expensive, and correct
 * only as a LOCAL act: it runs on the calling CPU and does not quiesce the
 * others, so it is defence in depth before a reset rather than proof that
 * every CPU's dirty state reached DRAM. Writers that care about their own
 * record persist it themselves with cache_writeback_range(). */
void cache_writeback_all(void);
