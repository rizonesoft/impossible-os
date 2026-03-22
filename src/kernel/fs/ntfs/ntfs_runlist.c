/* ============================================================================
 * ntfs_runlist.c — Data Run Decoder (§4.1)
 *
 * Decodes the variable-length run-list encoding used by non-resident
 * NTFS attributes. Each run maps a range of Virtual Cluster Numbers (VCNs)
 * to a starting Logical Cluster Number (LCN) and a length.
 * ============================================================================ */

#include "kernel/fs/ntfs.h"
#include "kernel/fs/ntfs_internal.h"

/* Read N bytes as an unsigned integer (little-endian, N ≤ 8) */
static uint64_t read_unsigned(const uint8_t *p, int n)
{
    uint64_t val = 0;
    int i;
    for (i = 0; i < n && i < 8; i++)
        val |= (uint64_t)p[i] << (i * 8);
    return val;
}

/* Read N bytes as a signed integer (little-endian, sign-extended, N ≤ 8) */
static int64_t read_signed(const uint8_t *p, int n)
{
    uint64_t val = 0;
    int i;
    for (i = 0; i < n && i < 8; i++)
        val |= (uint64_t)p[i] << (i * 8);

    /* Sign-extend: if the high bit of the last byte is set, fill upper bits */
    if (n > 0 && n < 8 && (p[n - 1] & 0x80)) {
        uint64_t mask = ~((1ULL << (n * 8)) - 1);
        val |= mask;
    }
    return (int64_t)val;
}

int ntfs_decode_data_runs(const uint8_t *attr, struct ntfs_data_run *runs,
                          int max_runs, struct ntfs_nonres_header *nrhdr)
{
    const uint8_t *p;
    uint16_t run_off;
    uint64_t vcn;
    int64_t  prev_lcn = 0;
    int      count = 0;
    uint8_t  header;

    if (!attr || !runs || max_runs <= 0)
        return -1;

    /* Verify this is a non-resident attribute */
    if (attr[0x08] != 1)
        return -1;

    /* Parse non-resident header fields */
    run_off = ntfs_le16(attr + 0x20);

    if (nrhdr) {
        nrhdr->start_vcn    = ntfs_le64(attr + 0x10);
        nrhdr->last_vcn     = ntfs_le64(attr + 0x18);
        nrhdr->data_run_off = run_off;
        nrhdr->alloc_size   = ntfs_le64(attr + 0x28);
        nrhdr->real_size    = ntfs_le64(attr + 0x30);
        nrhdr->init_size    = ntfs_le64(attr + 0x38);
    }

    /* Starting VCN for run tracking */
    vcn = ntfs_le64(attr + 0x10);

    /* Point to the start of the run-list data */
    p = attr + run_off;

    /* Walk the run-list */
    while (count < max_runs) {
        int len_size;
        int off_size;
        uint64_t run_length;
        int64_t  run_offset;

        header = *p;
        if (header == 0x00)
            break;  /* End of run-list */

        len_size = header & 0x0F;
        off_size = (header >> 4) & 0x0F;

        if (len_size == 0 || len_size > 8 || off_size > 8)
            break;  /* Corrupt run-list */

        p++;  /* Advance past header byte */

        /* Read run length (unsigned) */
        run_length = read_unsigned(p, len_size);
        p += len_size;

        /* Read run offset (signed, relative) — or sparse if off_size == 0 */
        if (off_size > 0) {
            run_offset = read_signed(p, off_size);
            p += off_size;

            prev_lcn += run_offset;
            runs[count].lcn = (uint64_t)prev_lcn;
        } else {
            /* Sparse run — no disk location, reads as zeros */
            runs[count].lcn = NTFS_LCN_SPARSE;
        }

        runs[count].vcn_start = vcn;
        runs[count].length    = run_length;

        vcn += run_length;
        count++;
    }

    return count;
}
