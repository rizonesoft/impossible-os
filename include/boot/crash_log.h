/* crash_log.h -- where a boot's crash log is, told to the next boot.
 *
 * The kernel publishes the physical address of its crash log in this firmware
 * variable; the next boot's loader keeps clear of it and its kernel reads it.
 * Macros only: it compiles with the UEFI types and with the kernel's.
 */
#ifndef BOOT_CRASH_LOG_H
#define BOOT_CRASH_LOG_H

#define BOOT_CRASH_LOG_PAGES 32         /* 128 KiB */
#define BOOT_CRASH_LOG_VAR   u"ImpossibleCrashLog"
#define BOOT_CRASH_LOG_GUID  { 0x494D504F, 0x5354, 0x4F53, \
                               { 0x50, 0x4F, 0x53, 0x54, 0x47, 0x55, 0x49, 0x44 } }

#endif /* BOOT_CRASH_LOG_H */
