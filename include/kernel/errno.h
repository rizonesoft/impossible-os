/* ============================================================================
 * errno.h -- Kernel error codes (POSIX-compatible subset)
 *
 * Positive integer error constants for kernel-internal use. Functions may
 * return -EFOO (negated) or pass EFOO via out-parameter, depending on API.
 * NT syscalls use NTSTATUS instead; these are for kernel-internal APIs.
 * ============================================================================ */

#pragma once

#define EPERM        1   /* operation not permitted */
#define ENOENT       2   /* no such file or directory */
#define ESRCH        3   /* no such process */
#define EINTR        4   /* interrupted system call */
#define EIO          5   /* I/O error */
#define ENXIO        6   /* no such device or address */
#define E2BIG        7   /* argument list too long */
#define ENOEXEC      8   /* exec format error */
#define EBADF        9   /* bad file descriptor */
#define ECHILD      10   /* no child processes */
#define EAGAIN      11   /* try again */
#define ENOMEM      12   /* out of memory */
#define EACCES      13   /* permission denied */
#define EFAULT      14   /* bad address */
#define EBUSY       16   /* device or resource busy */
#define EEXIST      17   /* file exists */
#define ENODEV      19   /* no such device */
#define ENOTDIR     20   /* not a directory */
#define EISDIR      21   /* is a directory */
#define EINVAL      22   /* invalid argument */
#define ENFILE      23   /* file table overflow */
#define EMFILE      24   /* too many open files */
#define ENOTTY      25   /* inappropriate ioctl for device (no controlling tty) */
#define ENOSPC      28   /* no space left on device */
#define ENOSYS      38   /* function not implemented */
#define ENOTEMPTY   39   /* directory not empty */
#define ERANGE      34   /* result too large */
