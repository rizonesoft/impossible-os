/* ============================================================================
 * test_boot_confirm.c -- the boot console prompt's pure helpers
 *
 * The prompt itself drives the 8042 and may not run from a test, so the parts
 * that decide an answer are exported and tested here: the scancode decode,
 * the accept set, the make/break step that turns 0x15 into an authorizing 'y',
 * and the overflow-checked timeout budget.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/test/test.h"
#include "kernel/boot_confirm.h"
#include "kernel/drivers/i8042.h"
#include "kernel/drivers/keyboard.h"

void test_register_boot_confirm(void);

#define SC_Y 0x15
#define SC_N 0x31
#define SC_ESC 0x01

static void test_confirm_decode(void)
{
    unsigned sc;
    int decoded = 0;

    TEST_ASSERT_EQ(boot_confirm_scancode_to_ascii(SC_Y), 'y', "0x15 decodes as y");
    TEST_ASSERT_EQ(boot_confirm_scancode_to_ascii(SC_N), 'n', "0x31 decodes as n");
    TEST_ASSERT_EQ(boot_confirm_scancode_to_ascii(SC_ESC), 0x1B, "0x01 decodes as Escape");
    TEST_ASSERT_EQ(boot_confirm_scancode_to_ascii(0x16), 0, "0x16 (u) is not an answer");
    TEST_ASSERT_EQ(boot_confirm_scancode_to_ascii(SC_Y | I8042_BREAK_CODE), 0,
                   "a break code is not decoded as a make");

    /* Every key the prompt decodes must mean the same key to the keyboard
     * driver: the prompt's map is a subset of the driver's, never a rival. */
    for (sc = 0; sc < 0x80; sc++) {
        char ch = boot_confirm_scancode_to_ascii((uint8_t)sc);
        if (!ch)
            continue;
        decoded++;
        TEST_ASSERT_EQ(ch, keyboard_scancode_ascii((uint8_t)sc),
                       "prompt decode agrees with the keyboard driver map");
    }
    TEST_ASSERT_EQ(decoded, 6, "the prompt decodes exactly six keys");
}

static void test_confirm_accept_set(void)
{
    TEST_ASSERT_EQ(boot_confirm_key_accepted('y', "yn"), 1, "y is in yn");
    TEST_ASSERT_EQ(boot_confirm_key_accepted('r', "yn"), 0, "r is not in yn");
    TEST_ASSERT_EQ(boot_confirm_key_accepted('r', NULL), 1, "NULL accepts any key");
    TEST_ASSERT_EQ(boot_confirm_key_accepted('y', ""), 0, "an empty set accepts nothing");
}

static void test_confirm_key_step(void)
{
    char pending = 0;

    /* A complete press: make then break of the same key answers on release. */
    TEST_ASSERT_EQ(boot_confirm_key_step(&pending, SC_Y, NULL), 0, "make of y does not answer");
    TEST_ASSERT_EQ(pending, 'y', "make of y is remembered");
    TEST_ASSERT_EQ(boot_confirm_key_step(&pending, SC_Y, NULL), 0,
                   "a typematic repeat of y does not answer");
    TEST_ASSERT_EQ(boot_confirm_key_step(&pending, SC_Y | I8042_BREAK_CODE, NULL), 'y',
                   "break of the pending y answers y");

    /* A release with no make seen this prompt answers nothing. */
    pending = 0;
    TEST_ASSERT_EQ(boot_confirm_key_step(&pending, SC_Y | I8042_BREAK_CODE, NULL), 0,
                   "a break with no make does not answer");

    /* Release of a different key than the one pressed answers nothing. */
    pending = 0;
    (void)boot_confirm_key_step(&pending, SC_N, NULL);
    TEST_ASSERT_EQ(boot_confirm_key_step(&pending, SC_Y | I8042_BREAK_CODE, NULL), 0,
                   "break of y after make of n does not answer");
    TEST_ASSERT_EQ(boot_confirm_key_step(&pending, SC_N | I8042_BREAK_CODE, NULL), 'n',
                   "break of the pending n answers n");

    /* A key outside the accept set neither arms nor answers. */
    pending = 0;
    TEST_ASSERT_EQ(boot_confirm_key_step(&pending, SC_Y, "rcp"), 0, "y outside rcp");
    TEST_ASSERT_EQ(pending, 0, "an unaccepted make leaves pending clear");

    /* An undecoded byte (a mouse byte that slipped past, say) does nothing. */
    pending = 'y';
    TEST_ASSERT_EQ(boot_confirm_key_step(&pending, 0x16, NULL), 0, "undecoded make");
    TEST_ASSERT_EQ(pending, 'y', "an undecoded byte does not disturb pending");
}

static void test_confirm_budget(void)
{
    uint64_t out = 7;

    TEST_ASSERT_EQ(boot_confirm_budget_ticks(15000, 3000000000ULL, &out), 1, "15 s at 3 GHz");
    TEST_ASSERT_EQ(out, 45000000000ULL, "15 s at 3 GHz is 45e9 ticks");

    out = 7;
    TEST_ASSERT_EQ(boot_confirm_budget_ticks(0, 3000000000ULL, &out), 0, "0 ms is refused");
    TEST_ASSERT_EQ(boot_confirm_budget_ticks(1000, 0, &out), 0, "no TSC is refused");
    TEST_ASSERT_EQ(boot_confirm_budget_ticks(1000, 999, &out), 0, "sub-kHz is refused");
    TEST_ASSERT_EQ(out, 7, "a refusal leaves *out untouched");

    /* per_ms * timeout_ms just past UINT64_MAX must refuse, not wrap. */
    TEST_ASSERT_EQ(boot_confirm_budget_ticks(0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFULL, &out), 0,
                   "an overflowing product is refused");
    TEST_ASSERT_EQ(boot_confirm_budget_ticks(1, 0xFFFFFFFFFFFFFFFFULL, &out), 1,
                   "the largest frequency at 1 ms fits");
    TEST_ASSERT_EQ(out, 0xFFFFFFFFFFFFFFFFULL / 1000ULL, "1 ms is freq / 1000 ticks");
}

void test_register_boot_confirm(void)
{
    test_suite_register_cat("boot_confirm scancode decode", test_confirm_decode, TEST_CAT_BOOT);
    test_suite_register_cat("boot_confirm accept set", test_confirm_accept_set, TEST_CAT_BOOT);
    test_suite_register_cat("boot_confirm answers on release", test_confirm_key_step, TEST_CAT_BOOT);
    test_suite_register_cat("boot_confirm timeout budget", test_confirm_budget, TEST_CAT_BOOT);
}
