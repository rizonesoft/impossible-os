/* ============================================================================
 * dump-common.h -- shared JSON emitter for boot_info ABI manifest dumpers
 *
 * Used by tools/boot-info-manifest/dump-kernel.c and dump-mirror.c. Each
 * dumper walks struct boot_info via __builtin_offsetof + sizeof and writes
 * a JSON object to stdout with this shape:
 *
 *   {
 *     "view": "kernel" | "mirror",
 *     "version": 5,
 *     "struct_size": 22184,
 *     "fields": [
 *       { "name": "header.magic",        "offset": 0,     "size": 4 },
 *       { "name": "header.version",      "offset": 4,     "size": 2 },
 *       { "name": "header.size",         "offset": 6,     "size": 2 },
 *       { "name": "mmap",                "offset": 8,     "size": 16384 },
 *       ...
 *     ],
 *     "sha256": "deadbeef..."
 *   }
 *
 * The SHA-256 is computed by walking the emitted JSON body (without the
 * sha256 line itself) -- a single-value change-detection token for
 * release notes and CI dashboards. The comparison script
 * tools/boot-info-manifest/compare.sh only compares the fields[] array;
 * the sha256 is advisory.
 *
 * No external dependencies. OpenSSL / libcrypto are avoided so this builds
 * on a fresh Ubuntu runner with just `clang-19`. SHA-256 is a small
 * portable implementation below.
 * ============================================================================ */

#ifndef BOOT_INFO_MANIFEST_DUMP_COMMON_H
#define BOOT_INFO_MANIFEST_DUMP_COMMON_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- SHA-256 (public-domain reference implementation, trimmed) ----------- */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buf[64];
    uint32_t buflen;
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t sha256_rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(sha256_ctx *c, const uint8_t *block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i-15], 7) ^ sha256_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = sha256_rotr(w[i-2], 17) ^ sha256_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->state[0],b=c->state[1],cc=c->state[2],d=c->state[3];
    uint32_t e=c->state[4],f=c->state[5],g=c->state[6],h=c->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e,6) ^ sha256_rotr(e,11) ^ sha256_rotr(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_rotr(a,2) ^ sha256_rotr(a,13) ^ sha256_rotr(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void sha256_init(sha256_ctx *c) {
    c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85; c->state[2]=0x3c6ef372; c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f; c->state[5]=0x9b05688c; c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
    c->bitcount = 0;
    c->buflen = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->bitcount += (uint64_t)len * 8;
    while (len > 0) {
        uint32_t space = 64 - c->buflen;
        uint32_t take = len < space ? (uint32_t)len : space;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_transform(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->bitcount;
    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        while (c->buflen < 64) c->buf[c->buflen++] = 0;
        sha256_transform(c, c->buf);
        c->buflen = 0;
    }
    while (c->buflen < 56) c->buf[c->buflen++] = 0;
    for (int i = 7; i >= 0; i--) c->buf[c->buflen++] = (uint8_t)(bits >> (i * 8));
    sha256_transform(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->state[i] >> 24);
        out[i*4+1] = (uint8_t)(c->state[i] >> 16);
        out[i*4+2] = (uint8_t)(c->state[i] >> 8);
        out[i*4+3] = (uint8_t)(c->state[i]);
    }
}

/* --- JSON emission with SHA-256 accumulation ----------------------------- */

/* g_body is the JSON body hashed for the sha256 line. Every manifest_write*
 * call appends to both stdout and the hash context so the SHA stays in
 * sync with whatever the compare script reads. */
static sha256_ctx g_sha;
static int g_fields_emitted;

static void manifest_write(const char *s) {
    fputs(s, stdout);
    sha256_update(&g_sha, s, strlen(s));
}

static void manifest_writef(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        fprintf(stderr, "manifest: vsnprintf truncated (%d)\n", n);
        exit(2);
    }
    manifest_write(buf);
}

static void manifest_begin(const char *view, unsigned version, size_t struct_size) {
    sha256_init(&g_sha);
    g_fields_emitted = 0;
    manifest_writef(
        "{\n"
        "  \"view\": \"%s\",\n"
        "  \"version\": %u,\n"
        "  \"struct_size\": %zu,\n"
        "  \"fields\": [\n",
        view, version, struct_size);
}

static void manifest_field(const char *name, size_t offset, size_t size) {
    if (g_fields_emitted > 0) manifest_write(",\n");
    manifest_writef(
        "    { \"name\": \"%s\", \"offset\": %zu, \"size\": %zu }",
        name, offset, size);
    g_fields_emitted++;
}

static void manifest_end(void) {
    manifest_write("\n  ],\n");
    /* Seal the hash BEFORE writing the sha256 line so the line itself is
     * not part of the input. Reproducers can validate by stripping the
     * sha256 line from the JSON and re-hashing. */
    uint8_t digest[32];
    sha256_final(&g_sha, digest);
    char hex[65];
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i*2]     = H[(digest[i] >> 4) & 0xF];
        hex[i*2 + 1] = H[digest[i] & 0xF];
    }
    hex[64] = 0;
    /* Write directly to stdout; do not hash this line. */
    printf("  \"sha256\": \"%s\"\n}\n", hex);
}

/* --- Field enumeration macros ------------------------------------------- */
/* F(name) expands to manifest_field("name", offsetof(struct boot_info, name),
 * sizeof(((struct boot_info *)0)->name)). Used to keep the dumper code
 * readable and both views using the exact same call sequence. */
#define F(field_ref) \
    manifest_field(#field_ref, \
                   __builtin_offsetof(struct boot_info, field_ref), \
                   sizeof(((struct boot_info *)0)->field_ref))

#endif /* BOOT_INFO_MANIFEST_DUMP_COMMON_H */
