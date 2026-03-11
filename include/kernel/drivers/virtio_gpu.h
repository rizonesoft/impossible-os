/* ============================================================================
 * virtio_gpu.h — VirtIO-GPU driver (P18-lite: hardware cursor + scanout)
 *
 * Minimal VirtIO-GPU 2D driver focused on:
 *   - Hardware cursor plane (cursor moves at display refresh rate,
 *     independent of compositor)
 *   - Basic scanout (replaces VBE fb_swap with GPU resource flush)
 *
 * References:
 *   - VirtIO spec 1.1 §5.7 (GPU Device)
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- VirtIO-GPU command types ---- */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO       0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF         0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT            0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH         0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

/* Cursor commands */
#define VIRTIO_GPU_CMD_UPDATE_CURSOR          0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR            0x0301

/* Response types */
#define VIRTIO_GPU_RESP_OK_NODATA             0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO       0x1101

/* Pixel formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM      1
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM      67

/* ---- GPU command structures ---- */

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_GET_DISPLAY_INFO response */
#define VIRTIO_GPU_MAX_SCANOUTS 16
struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

/* VIRTIO_GPU_CMD_RESOURCE_CREATE_2D */
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_SET_SCANOUT */
struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING */
struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D */
struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_RESOURCE_FLUSH */
struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_UPDATE_CURSOR / MOVE_CURSOR */
struct virtio_gpu_cursor_pos {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
} __attribute__((packed));

/* ---- Public API ---- */

/* Initialize VirtIO-GPU driver.
 * Returns 0 on success, -1 if no device found. */
int virtio_gpu_init(void);

/* Returns 1 if VirtIO-GPU is active and operational. */
uint8_t virtio_gpu_available(void);

/* Flush a rectangular region to the display.
 * This transfers pixel data from the backing buffer to the GPU resource
 * and then flushes it to the screen.  Replaces fb_swap() / fb_swap_rect(). */
void virtio_gpu_flush_rect(uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height);

/* Flush the entire screen. */
void virtio_gpu_flush(void);

/* Set the hardware cursor image and position.
 * pixels: 64×64 BGRA array, hot_x/hot_y: hotspot offset.
 * Call once when cursor shape changes. */
void virtio_gpu_set_cursor(const uint32_t *pixels,
                           uint32_t width, uint32_t height,
                           uint32_t hot_x, uint32_t hot_y);

/* Move the hardware cursor to a new position.
 * Very lightweight — only sends a MOVE_CURSOR command on the cursor queue.
 * This is the key latency win: cursor moves at GPU refresh rate. */
void virtio_gpu_move_cursor(uint32_t x, uint32_t y);

/* Hide the hardware cursor (set resource_id=0). */
void virtio_gpu_hide_cursor(void);
