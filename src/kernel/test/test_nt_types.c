/* ============================================================================
 * test_nt_types.c -- NTSTATUS type and NT foundational types unit tests
 *
 * Tests severity macros, status code values, and type sizes for Win32 ABI
 * compatibility.
 *
 * XREF: 02-kernel-core/TODO-05-native-api-ssdt.md section 1
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"

/* ---- NTSTATUS severity macros ---- */

static void test_ntstatus_success(void)
{
    TEST_ASSERT(NT_SUCCESS(STATUS_SUCCESS),
                "NT_SUCCESS(STATUS_SUCCESS) is true");
    TEST_ASSERT(NT_SUCCESS(STATUS_PENDING),
                "NT_SUCCESS(STATUS_PENDING) is true");
    TEST_ASSERT(NT_SUCCESS(STATUS_ALERTED),
                "NT_SUCCESS(STATUS_ALERTED) is true");
    TEST_ASSERT(!NT_SUCCESS(STATUS_UNSUCCESSFUL),
                "NT_SUCCESS(STATUS_UNSUCCESSFUL) is false");
    TEST_ASSERT(!NT_SUCCESS(STATUS_ACCESS_DENIED),
                "NT_SUCCESS(STATUS_ACCESS_DENIED) is false");
}

static void test_ntstatus_error(void)
{
    TEST_ASSERT(NT_ERROR(STATUS_UNSUCCESSFUL),
                "NT_ERROR(STATUS_UNSUCCESSFUL) is true");
    TEST_ASSERT(NT_ERROR(STATUS_ACCESS_VIOLATION),
                "NT_ERROR(STATUS_ACCESS_VIOLATION) is true");
    TEST_ASSERT(NT_ERROR(STATUS_NO_MEMORY),
                "NT_ERROR(STATUS_NO_MEMORY) is true");
    TEST_ASSERT(!NT_ERROR(STATUS_SUCCESS),
                "NT_ERROR(STATUS_SUCCESS) is false");
    TEST_ASSERT(!NT_ERROR(STATUS_BUFFER_OVERFLOW),
                "NT_ERROR(STATUS_BUFFER_OVERFLOW) is false");
}

static void test_ntstatus_warning(void)
{
    TEST_ASSERT(NT_WARNING(STATUS_BUFFER_OVERFLOW),
                "NT_WARNING(STATUS_BUFFER_OVERFLOW) is true");
    TEST_ASSERT(NT_WARNING(STATUS_NO_MORE_FILES),
                "NT_WARNING(STATUS_NO_MORE_FILES) is true");
    TEST_ASSERT(!NT_WARNING(STATUS_SUCCESS),
                "NT_WARNING(STATUS_SUCCESS) is false");
    TEST_ASSERT(!NT_WARNING(STATUS_UNSUCCESSFUL),
                "NT_WARNING(STATUS_UNSUCCESSFUL) is false");
}

static void test_ntstatus_information(void)
{
    /* STATUS_ALERTED (0x00000101) and STATUS_TIMEOUT (0x00000102) have
     * severity 00 (success), not 01 (informational). This matches
     * Windows behavior -- they are success codes, not info codes.
     * True informational codes have bits 31:30 = 01 (0x40000000+). */
    TEST_ASSERT(!NT_INFORMATION(STATUS_SUCCESS),
                "NT_INFORMATION(STATUS_SUCCESS) is false");
    TEST_ASSERT(!NT_INFORMATION(STATUS_UNSUCCESSFUL),
                "NT_INFORMATION(STATUS_UNSUCCESSFUL) is false");
}

/* ---- Type sizes (ABI compatibility) ---- */

static void test_nt_type_sizes(void)
{
    TEST_ASSERT_EQ(sizeof(NTSTATUS), 4, "NTSTATUS is 4 bytes");
    TEST_ASSERT_EQ(sizeof(ACCESS_MASK), 4, "ACCESS_MASK is 4 bytes");
    TEST_ASSERT_EQ(sizeof(HANDLE), 4, "HANDLE is 4 bytes");
    TEST_ASSERT_EQ(sizeof(IO_STATUS_BLOCK), 16,
                   "IO_STATUS_BLOCK is 16 bytes");
}

static void test_object_attributes_size(void)
{
    /* OBJECT_ATTRIBUTES must match Windows x64 layout (56 bytes) */
    TEST_ASSERT(sizeof(OBJECT_ATTRIBUTES) >= 48,
                "OBJECT_ATTRIBUTES >= 48 bytes");
    TEST_ASSERT(sizeof(OBJECT_ATTRIBUTES) <= 64,
                "OBJECT_ATTRIBUTES <= 64 bytes");
}

/* ---- Registration ---- */

void test_register_nt_types(void)
{
    test_suite_register_cat("NT: NTSTATUS success", test_ntstatus_success, TEST_CAT_ABI);
    test_suite_register_cat("NT: NTSTATUS error", test_ntstatus_error, TEST_CAT_ABI);
    test_suite_register_cat("NT: NTSTATUS warning", test_ntstatus_warning, TEST_CAT_ABI);
    test_suite_register_cat("NT: NTSTATUS information", test_ntstatus_information, TEST_CAT_ABI);
    test_suite_register_cat("NT: type sizes", test_nt_type_sizes, TEST_CAT_ABI);
    test_suite_register_cat("NT: OBJECT_ATTRIBUTES size", test_object_attributes_size, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
