/* tools/jpg2raw.c — Convert JPEG/PNG to raw BGRA pixel data
 *
 * Build: gcc -O2 -o build/tools/jpg2raw tools/jpg2raw.c -lm
 * Usage: ./build/tools/jpg2raw input.{jpg,png} output.raw width height
 *
 * Outputs a raw file of width*height*4 bytes (BGRA format, no header).
 * Uses stb_image for JPEG/PNG decoding and nearest-neighbor resize.
 * Alpha channel is preserved for PNG with transparency.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int src_w, src_h, channels;
    unsigned char *src_pixels;
    unsigned char *dst_pixels;
    int dst_w, dst_h;
    int x, y;
    FILE *fp;

    if (argc != 5) {
        fprintf(stderr, "Usage: %s input.jpg output.raw width height\n", argv[0]);
        return 1;
    }

    dst_w = atoi(argv[3]);
    dst_h = atoi(argv[4]);

    if (dst_w <= 0 || dst_h <= 0) {
        fprintf(stderr, "Invalid dimensions: %dx%d\n", dst_w, dst_h);
        return 1;
    }

    /* Load image (decode to RGBA for alpha support) */
    src_pixels = stbi_load(argv[1], &src_w, &src_h, &channels, 4);
    if (!src_pixels) {
        fprintf(stderr, "Failed to load %s: %s\n", argv[1], stbi_failure_reason());
        return 1;
    }

    fprintf(stderr, "Loaded %s: %dx%d (%d channels)\n", argv[1], src_w, src_h, channels);

    /* Allocate output buffer (BGRA) */
    dst_pixels = (unsigned char *)malloc(dst_w * dst_h * 4);
    if (!dst_pixels) {
        fprintf(stderr, "Out of memory\n");
        stbi_image_free(src_pixels);
        return 1;
    }

    /* Nearest-neighbor resize + RGBA→BGRA conversion */
    for (y = 0; y < dst_h; y++) {
        int sy = y * src_h / dst_h;
        for (x = 0; x < dst_w; x++) {
            int sx = x * src_w / dst_w;
            unsigned char *sp = &src_pixels[(sy * src_w + sx) * 4];
            unsigned char *dp = &dst_pixels[(y * dst_w + x) * 4];
            dp[0] = sp[2];  /* B */
            dp[1] = sp[1];  /* G */
            dp[2] = sp[0];  /* R */
            dp[3] = sp[3];  /* A (preserved from source) */
        }
    }

    stbi_image_free(src_pixels);

    /* Write raw output */
    fp = fopen(argv[2], "wb");
    if (!fp) {
        fprintf(stderr, "Cannot open %s for writing\n", argv[2]);
        free(dst_pixels);
        return 1;
    }

    fwrite(dst_pixels, 1, dst_w * dst_h * 4, fp);
    fclose(fp);
    free(dst_pixels);

    fprintf(stderr, "Wrote %s: %dx%d BGRA (%d bytes)\n",
            argv[2], dst_w, dst_h, dst_w * dst_h * 4);
    return 0;
}
