/* icon_store_test.h -- KERNEL_TESTS-only seams into the icon cache.
 *
 * The unit-test boot loads neither icons.ires nor the icon fonts, so the
 * cache-key-normalization and borrowed-eviction regressions (the window-drag
 * 0xE0000002 heap-corruption class) cannot be reached through file loading.
 * These seams inject entries through the SAME cache_alloc/free_pixels
 * mechanics the production paths use. Implemented in icon_store.c under
 * #ifdef KERNEL_TESTS.
 */
#ifndef KERNEL_TEST_ICON_STORE_TEST_H
#define KERNEL_TEST_ICON_STORE_TEST_H

#ifdef KERNEL_TESTS

#include "kernel/types.h"
#include "icon_store.h"

/* Save/restore wrapper for the store-ready gate; returns the previous value.
 * Tests MUST restore the previous value before returning. */
int icon_store_test_force_ready(int on);

/* Insert a BORROWED cache entry (caller-owned pixels; stands in for an
 * IRES-backed entry). Stored with color=0, exactly like ires_get_bitmap.
 * Returns 0 on success, -1 if no cache slot could be obtained. */
int icon_cache_insert_borrowed_for_test(system_icon_t id, uint32_t size,
                                        uint32_t *pixels,
                                        uint16_t w, uint16_t h);

/* Insert n OWNED 16x16 entries with unique keys (forces LRU eviction when n
 * exceeds the cache capacity). Returns the number actually inserted. */
uint32_t icon_cache_flood_owned_for_test(uint32_t n);

/* Release every cache entry (ownership-aware). Tests that insert entries
 * call this before returning so the per-test heap-leak detector sees a
 * clean exit; the cache repopulates on demand. */
void icon_cache_reset_for_test(void);

#endif /* KERNEL_TESTS */
#endif /* KERNEL_TEST_ICON_STORE_TEST_H */
