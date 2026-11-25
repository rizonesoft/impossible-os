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

#endif /* BOOT_CRASH_LOG_H */
