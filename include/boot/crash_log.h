/* crash_log.h -- the previous boot's crash log, from kernel to loader to kernel.
 *
 * A kernel publishes the physical address of its crash log in a firmware variable. The
 * next boot's loader copies the log into its own image before anything else can take the
 * pages, and hands the copy over in boot_info. This compiles with the UEFI types and with
 * the kernel's.
 */
#ifndef BOOT_CRASH_LOG_H
#define BOOT_CRASH_LOG_H

#define BOOT_CRASH_LOG_PAGES 32         /* 128 KiB */
#define BOOT_CRASH_LOG_VAR   u"ImpossibleCrashLog"
#define BOOT_CRASH_LOG_GUID  { 0x494D504F, 0x5354, 0x4F53, \
                               { 0x50, 0x4F, 0x53, 0x54, 0x47, 0x55, 0x49, 0x44 } }

/* The log begins with 32-bit words: the magic, how many entries follow the header, and
 * their CRC-32. The kernel pins these to its own structures. */
#define BOOT_CRASH_LOG_MAGIC        0x4B4C4F47u     /* "KLOG" */
#define BOOT_CRASH_LOG_CRC_WORD     2
#define BOOT_CRASH_LOG_HEADER_BYTES 24u
#define BOOT_CRASH_LOG_ENTRY_BYTES  164u
#define BOOT_CRASH_LOG_MAX_BYTES    (BOOT_CRASH_LOG_PAGES * 4096u)

/* How long the log is whose first two words these are; 0 when they begin no log. */
static inline unsigned int boot_crash_log_bytes(unsigned int magic, unsigned int entries)
{
    if (magic != BOOT_CRASH_LOG_MAGIC || entries == 0 ||
        entries > (BOOT_CRASH_LOG_MAX_BYTES - BOOT_CRASH_LOG_HEADER_BYTES) / BOOT_CRASH_LOG_ENTRY_BYTES)
        return 0;
    return BOOT_CRASH_LOG_HEADER_BYTES + entries * BOOT_CRASH_LOG_ENTRY_BYTES;
}

/* The loader takes and gives back whole pages at an address. */
struct boot_crash_log_pages {
    int (*claim)(unsigned long long base, unsigned int pages);      /* nonzero when they were free */
    void (*release)(unsigned long long base, unsigned int pages);
};

/* Copy the log at base into copy (BOOT_CRASH_LOG_MAX_BYTES long). Its pages are claimed
 * first, the header's and then the rest of what it says: free memory is nobody's to read.
 * Answers how long the copy is, or 0 with the reason. The caller checks the CRC. */
static inline unsigned int boot_crash_log_take(unsigned long long base, unsigned int *copy,
                                               struct boot_crash_log_pages pages, const char **why)
{
    const volatile unsigned int *log = (const volatile unsigned int *)(unsigned long long)base;
    unsigned int bytes, rest;

    *why = "not free memory";
    if (!pages.claim(base, 1))
        return 0;
    copy[0] = log[0];
    copy[1] = log[1];
    bytes = boot_crash_log_bytes(copy[0], copy[1]);
    rest = bytes ? (bytes - 1) / 4096 : 0;
    if (bytes == 0) {
        *why = "no log there";
    } else if (rest != 0 && !pages.claim(base + 4096, rest)) {
        bytes = 0;
    } else {
        for (unsigned int i = 2; i < bytes / sizeof(*copy); i++)
            copy[i] = log[i];
        if (rest != 0)
            pages.release(base + 4096, rest);
    }
    pages.release(base, 1);
    return bytes;
}

#endif /* BOOT_CRASH_LOG_H */
