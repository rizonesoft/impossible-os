/* ============================================================================
 * test_bootproto_parse.c -- host unit test for bootproto_find().
 *
 * The parser runs in the BOOTLOADER pre-ExitBootServices. There is no
 * kernel-side runtime consumer, so the test lives host-side rather
 * than in src/kernel/test/. Builds as a standalone x86_64 executable;
 * exercises the parser against synthetic ELF fixtures.
 *
 * Fixtures:
 *   1. well-formed ELF with `.bootproto` -> BOOTPROTO_OK + fields match
 *   2. no `.bootproto` section             -> BOOTPROTO_ERR_NOT_FOUND
 *   3. truncated section-header table     -> BOOTPROTO_ERR_SHT_BOUNDS
 *   4. crafted sh_offset past image end   -> BOOTPROTO_ERR_SECT_BOUNDS
 *   5. wrong section size (47 bytes)      -> BOOTPROTO_ERR_SECT_SIZE
 *   6. misaligned section (7-byte offset) -> BOOTPROTO_ERR_SECT_ALIGN
 *   7. e_shentsize = 0                    -> BOOTPROTO_ERR_SHT_ENTSIZE
 *   8. bad ELF magic                      -> BOOTPROTO_ERR_NOT_ELF64
 *   9. NULL image                         -> BOOTPROTO_ERR_NULL_IMAGE
 *  10. image_size too small for ehdr      -> BOOTPROTO_ERR_EHDR_BOUNDS
 *  11. e_shstrndx out of range            -> BOOTPROTO_ERR_SHSTR_IDX
 *  12. shstrtab content past EOF          -> BOOTPROTO_ERR_SHSTR_BOUNDS
 *  13. sh_name offset past shstrtab end   -> BOOTPROTO_ERR_NAME_BOUNDS
 *
 * Build: HOST_CC -I src/boot/uefi -I build tools/test-bootproto/test_bootproto_parse.c
 *        src/boot/uefi/elf_bootproto.c -o build/tools/test-bootproto
 * Run:   build/tools/test-bootproto
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Pull in the bootloader-side types. efi.h typedefs UINT8/UINT32/UINT64
 * which this host test needs. */
#include "efi.h"
#include "elf_types.h"
#include "boot_proto_mirror.h"
#include "elf_bootproto.h"

/* Generated sha header. The host tooling build must set -I build so
 * this resolves. */
#include "boot_proto_sha.h"

/* Test harness minimal. */
static int g_fail;
static int g_pass;

#define EXPECT_EQ(actual, expected, label) do {                               \
    long long _a = (long long)(actual), _e = (long long)(expected);           \
    if (_a != _e) {                                                           \
        fprintf(stderr, "FAIL: %s: got %lld, expected %lld\n",                \
                (label), _a, _e);                                             \
        g_fail++;                                                             \
    } else {                                                                  \
        g_pass++;                                                             \
    }                                                                         \
} while (0)

/* Build a minimal ELF64 image containing an optional `.bootproto`
 * section. Returns a malloc()'d buffer + size. Sections laid out:
 *   [0]           Elf64_Ehdr
 *   [ehsize..]    optional body (NUL byte so shstrtab offset 0 works)
 *   [..]          SHT_NULL entry
 *   [..]          .bootproto entry (if include_bootproto)
 *   [..]          .shstrtab entry
 *   [..]          shstrtab bytes (".bootproto\0.shstrtab\0")
 *   [..]          bootproto body (56 bytes)
 *
 * Caller frees the returned buffer.
 */
static UINT8 *build_fixture_elf(int include_bootproto,
                                 uint32_t custom_bp_size,
                                 int misalign_bp,
                                 uint64_t custom_image_size,
                                 uint64_t *out_size)
{
    /* Single-bootproto fixture. Layout:
     *   0x0000: Elf64_Ehdr (64 bytes)
     *   0x0040: shstrtab bytes (".\0.bootproto\0.shstrtab\0") <= 32 bytes
     *   0x0080: boot_proto_descriptor (56 bytes; optionally misaligned)
     *   0x0100: Elf64_Shdr[N]  (N = 3 if include_bootproto else 2)
     * Simple fixed offsets make the test easy to reason about.
     */
    UINT64 image_size = custom_image_size ? custom_image_size : 4096;
    UINT8 *img = (UINT8 *)calloc(1, image_size);
    if (!img) { perror("calloc"); exit(1); }

    /* ELF header */
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    eh->e_magic    = ELF_MAGIC;
    eh->e_class    = 2;
    eh->e_data     = 1;
    eh->e_version  = 1;
    eh->e_machine  = 0x3E;
    eh->e_ehsize   = sizeof(Elf64_Ehdr);
    eh->e_shentsize= sizeof(Elf64_Shdr);
    eh->e_shoff    = 0x100;
    eh->e_shnum    = include_bootproto ? 3 : 2;
    eh->e_shstrndx = include_bootproto ? 2 : 1;

    /* shstrtab contents at offset 0x40 */
    char *shs = (char *)(img + 0x40);
    shs[0] = 0;  /* empty string at index 0 */
    size_t shs_off = 1;
    size_t bp_name_off = shs_off;
    memcpy(shs + shs_off, ".bootproto", 11); shs_off += 11;
    size_t st_name_off = shs_off;
    memcpy(shs + shs_off, ".shstrtab", 10); shs_off += 10;
    size_t shs_total = shs_off;

    /* bootproto body at 0x80 (optionally misaligned at 0x87) */
    UINT64 bp_offset = misalign_bp ? 0x87u : 0x80u;
    UINT32 bp_size = custom_bp_size ? custom_bp_size :
                     (UINT32)sizeof(struct boot_proto_descriptor);

    if (include_bootproto && bp_offset + bp_size <= image_size) {
        struct boot_proto_descriptor *bp =
            (struct boot_proto_descriptor *)(img + bp_offset);
        bp->magic       = BOOT_PROTO_DESCRIPTOR_MAGIC;
        bp->version     = 9;
        bp->struct_size = 23752;
        static const UINT8 sample_sha[32] = KERNEL_ABI_SHA256;
        memcpy(bp->sha256, sample_sha, 32);
    }

    /* Section headers at 0x100 */
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + 0x100);
    /* [0] SHT_NULL */
    memset(&sh[0], 0, sizeof(sh[0]));

    if (include_bootproto) {
        /* [1] .bootproto */
        sh[1].sh_name   = (UINT32)bp_name_off;
        sh[1].sh_type   = SHT_PROGBITS;
        sh[1].sh_offset = bp_offset;
        sh[1].sh_size   = bp_size;
        sh[1].sh_addralign = 8;

        /* [2] .shstrtab */
        sh[2].sh_name   = (UINT32)st_name_off;
        sh[2].sh_type   = SHT_STRTAB;
        sh[2].sh_offset = 0x40;
        sh[2].sh_size   = shs_total;
    } else {
        /* [1] .shstrtab only (no bootproto) */
        sh[1].sh_name   = (UINT32)st_name_off;
        sh[1].sh_type   = SHT_STRTAB;
        sh[1].sh_offset = 0x40;
        sh[1].sh_size   = shs_total;
    }

    *out_size = image_size;
    return img;
}

static void test_happy_path(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 0, 0, 0, &sz);
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_OK, "happy path returns OK");
    EXPECT_EQ(d.magic,       BOOT_PROTO_DESCRIPTOR_MAGIC, "magic matches");
    EXPECT_EQ(d.version,     9,    "version matches");
    EXPECT_EQ(d.struct_size, 23752, "struct_size matches");
    free(img);
}

static void test_not_found(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(0, 0, 0, 0, &sz);
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_NOT_FOUND, "missing section returns NOT_FOUND");
    free(img);
}

static void test_null_image(void)
{
    struct boot_proto_descriptor d;
    int r = bootproto_find(NULL, 4096, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_NULL_IMAGE, "NULL image -> NULL_IMAGE");
    UINT8 dummy = 0;
    r = bootproto_find(&dummy, 0, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_NULL_IMAGE, "zero size -> NULL_IMAGE");
    r = bootproto_find(&dummy, 1, NULL);
    EXPECT_EQ(r, BOOTPROTO_ERR_NULL_IMAGE, "NULL out_desc -> NULL_IMAGE");
}

static void test_ehdr_too_small(void)
{
    UINT8 buf[4] = {0x7F, 'E', 'L', 'F'};
    struct boot_proto_descriptor d;
    int r = bootproto_find(buf, 4, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_EHDR_BOUNDS, "image < ehdr -> EHDR_BOUNDS");
}

static void test_bad_magic(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 0, 0, 0, &sz);
    /* Corrupt ELF magic */
    ((Elf64_Ehdr *)img)->e_magic = 0xDEADBEEF;
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_NOT_ELF64, "bad e_magic -> NOT_ELF64");
    free(img);
}

static void test_zero_shentsize(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 0, 0, 0, &sz);
    ((Elf64_Ehdr *)img)->e_shentsize = 0;
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_SHT_ENTSIZE, "e_shentsize=0 -> SHT_ENTSIZE");
    free(img);
}

static void test_sht_out_of_bounds(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 0, 0, 0, &sz);
    /* Push shoff past image_size */
    ((Elf64_Ehdr *)img)->e_shoff = sz + 1;
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_SHT_BOUNDS, "shoff past image -> SHT_BOUNDS");
    free(img);
}

static void test_shstrndx_out_of_range(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 0, 0, 0, &sz);
    ((Elf64_Ehdr *)img)->e_shstrndx = 99;  /* > e_shnum */
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_SHSTR_IDX, "shstrndx >= shnum -> SHSTR_IDX");
    free(img);
}

static void test_section_size_mismatch(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 47, 0, 0, &sz);  /* 47-byte section */
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_SECT_SIZE, "wrong sh_size -> SECT_SIZE");
    free(img);
}

static void test_section_misaligned(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 0, 1, 0, &sz);  /* offset 0x87 */
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_SECT_ALIGN, "misaligned offset -> SECT_ALIGN");
    free(img);
}

static void test_section_offset_past_eof(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 0, 0, 0, &sz);
    /* Push the bootproto section's sh_offset past image_size */
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + 0x100);
    sh[1].sh_offset = sz + 8;
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_SECT_BOUNDS,
              "sh_offset past image -> SECT_BOUNDS");
    free(img);
}

static void test_sh_name_past_shstrtab(void)
{
    uint64_t sz;
    UINT8 *img = build_fixture_elf(1, 0, 0, 0, &sz);
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + 0x100);
    /* Push sh[1].sh_name past shstrtab size */
    sh[1].sh_name = 9999;
    struct boot_proto_descriptor d;
    int r = bootproto_find(img, sz, &d);
    EXPECT_EQ(r, BOOTPROTO_ERR_NAME_BOUNDS,
              "sh_name past shstrtab -> NAME_BOUNDS");
    free(img);
}

int main(void)
{
    test_null_image();
    test_ehdr_too_small();
    test_bad_magic();
    test_zero_shentsize();
    test_sht_out_of_bounds();
    test_shstrndx_out_of_range();
    test_not_found();
    test_section_size_mismatch();
    test_section_misaligned();
    test_section_offset_past_eof();
    test_sh_name_past_shstrtab();
    test_happy_path();

    printf("bootproto_parse: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
