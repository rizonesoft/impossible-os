/* libFuzzer harness: the manifest's kernel-error value decoder.
 *
 * Input is one `expects_klog=` / `allows_klog=` value as it is read from
 * tests/usermode.manifest on the disk image, so every byte is untrusted. The
 * decoder's own file is compiled here, not a copy (see utest_klog_attr.h).
 * Beyond memory safety, the harness checks the contract on every accepted
 * value: the tag and text fit their caps and the text is printable ASCII.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/klog.h"                  /* KLOG_WATCH_TAG_MAX, KLOG_WATCH_TEXT_MAX */
#include "kernel/test/utest_klog_attr.h"

/* The caps the launcher passes: the watch's own field sizes. */
#define TAG_CAP  KLOG_WATCH_TAG_MAX
#define TEXT_CAP KLOG_WATCH_TEXT_MAX

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *buf, *text = NULL;
    int rc;

    if (size > 4096)
        return 0;
    /* A manifest value is a NUL-terminated token: an embedded NUL ends it. */
    buf = malloc(size + 1);
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';
    rc = utest_klog_attr_decode(buf, TAG_CAP, TEXT_CAP, &text);
    if (rc == UTEST_KLOG_ATTR_OK) {
        size_t tl = strlen(buf), xl = strlen(text);
        if (tl == 0 || tl >= TAG_CAP || xl == 0 || xl >= TEXT_CAP || text != buf + tl + 1)
            abort();
        for (size_t i = 0; i < xl; i++)
            if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7E)
                abort();
    } else if (rc < UTEST_KLOG_ATTR_E_LONG || rc > UTEST_KLOG_ATTR_E_SHAPE) {
        abort();
    }
    free(buf);
    return 0;
}
