/* Host definitions of the kernel services a fuzzed parser calls.
 *
 * Only what a parser's validation path needs and nothing it could depend on
 * for correctness: klog prints nothing (its lock and serial I/O have no
 * bearing on the parse). A target that needs more adds it here, never a copy
 * of the parser.
 */
#include <stdarg.h>

#include "kernel/klog.h"

void klog(log_level_t level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}
