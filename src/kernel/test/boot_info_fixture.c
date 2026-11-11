/* boot_info_fixture.c -- the shared unit-test boot_info (see the header). */
#include "kernel/test/boot_info_fixture.h"

/* 64-byte aligned: at least what every former private copy asked for. */
static struct boot_info s_fixture __attribute__((aligned(64)));

struct boot_info *test_boot_info_fixture(void)
{
    return &s_fixture;
}
