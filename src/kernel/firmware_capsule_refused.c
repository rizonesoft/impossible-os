/* ============================================================================
 * firmware_capsule_refused.c -- compile-time + linker-time refusal sentinel
 *
 * Impossible OS does NOT implement firmware capsule delivery.  No
 * UpdateCapsule(), no OsIndications capsule bit, no ESP staging, no
 * reboot-and-flash.  See firmware_advisor.h for the policy rationale.
 *
 * This translation unit serves two layers of refusal:
 *
 * 1. Compile-time: anyone who tries to define CAPSULE_REFUSAL_ENABLED (the
 *    "you-must-be-this-tall-to-write-firmware" gate that a future stance
 *    change would flip) trips a _Static_assert at the top of the file --
 *    the build fails with the policy text inline so the contributor sees
 *    why before they spend more time.  The default-off path keeps the
 *    file cheap (zero data, one symbol) when the policy is in effect.
 *
 * 2. Linker-time: three sentinel symbols (UpdateCapsule, capsule_update_request,
 *    capsule_submit) are defined here as zero-sized data tagged with a
 *    distinctive section name.  Any kernel module or user-mode binary that
 *    declares `extern void UpdateCapsule(...);` and references the symbol
 *    will resolve against THIS object -- but the symbol is data, not code,
 *    so the call site fails at link with "undefined reference to function
 *    that resolves to data" or, if the call is indirect via function pointer,
 *    crashes on first dereference with a clear segment marker
 *    (.firmware_capsule_refused) visible in the link map and panic disas.
 *
 * The two layers are complementary: compile-time catches the "I want to
 * implement capsule writes" intent; linker-time catches the "I quietly
 * pulled in a third-party module that calls UpdateCapsule()" footgun.
 * Removing this file or weakening either layer requires explicit policy
 * review (see firmware_advisor.h).
 * ============================================================================ */

#include "kernel/types.h"

/* Layer 1: compile-time gate.  CAPSULE_REFUSAL_ENABLED is intentionally
 * never defined anywhere in-tree.  A contributor who flips it on (e.g. a
 * Makefile -DCAPSULE_REFUSAL_ENABLED=1) gets the assert text inline.  We
 * use #ifdef rather than always-failing _Static_assert(0, ...) because the
 * latter would break the build for everyone unconditionally; the policy
 * fail must fire only for people trying to disable it. */
#ifdef CAPSULE_REFUSAL_ENABLED
_Static_assert(0,
    "Impossible OS does not implement firmware capsule delivery. "
    "Removing this assert requires explicit policy review -- see "
    "include/kernel/firmware_advisor.h for the rationale and the "
    "stance-change conditions.");
#endif

/* Layer 2: linker-time sentinel symbols.  Tagged with a distinctive section
 * so the .map file flags them clearly: a grep for
 * '.firmware_capsule_refused' in build/kernel.map is the single audit
 * point that any caller wired through to refusal-land.
 *
 * The values are deliberately non-NULL but un-callable (data of size 1).
 * Any direct call site fails to link (function-vs-data mismatch on most
 * linkers); any function-pointer call dereferences a 1-byte data symbol
 * and faults on the first instruction fetch.  Either way the violation
 * surfaces loudly. */
__attribute__((section(".firmware_capsule_refused")))
const uint8_t UpdateCapsule = 0;

__attribute__((section(".firmware_capsule_refused")))
const uint8_t capsule_update_request = 0;

__attribute__((section(".firmware_capsule_refused")))
const uint8_t capsule_submit = 0;

/* Force the sentinel to ship even when nobody references it (otherwise
 * linkers would garbage-collect it).  Anchor symbol that the build can
 * grep for in the map. */
__attribute__((used, section(".firmware_capsule_refused")))
const uint8_t firmware_capsule_refusal_sentinel = 0xFE;
