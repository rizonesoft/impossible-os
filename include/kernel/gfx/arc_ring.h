/* ============================================================================
 * arc_ring.h — Anti-aliased arc ring drawing primitive
 *
 * Draws a stroked arc (partial ring) to an arbitrary pixel buffer using
 * integer-only math.  No FPU, no SSE — safe to call from timer ISR context.
 *
 * The arc is defined by:
 *   - center (cx, cy) and outer radius
 *   - stroke width (ring thickness)
 *   - start angle and sweep angle (0–255 maps to 0°–360°)
 *   - fill color (0x00RRGGBB)
 *
 * Part of the Progressive Spinner component (TODO-010.97).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* --- Arc ring size descriptor --- */

struct arc_ring_size {
    int32_t radius;   /* outer radius in pixels */
    int32_t stroke;   /* ring thickness in pixels */
};

/* Return the recommended arc ring size for a given screen height.
 * Scales proportionally so the spinner looks correct at all resolutions. */
struct arc_ring_size arc_ring_size_for_height(uint32_t screen_height);

/* --- Arc ring drawing --- */

/* Draw an anti-aliased arc ring to a pixel buffer.
 *
 * buf        — destination pixel buffer (BGRA32, row-major)
 * buf_w/h    — buffer dimensions in pixels
 * cx, cy     — center of the ring in buffer coordinates
 * radius     — outer radius of the ring in pixels
 * stroke     — ring thickness in pixels (inner radius = radius - stroke)
 * start_256  — start angle of the arc (0–255 maps to 0°–360°)
 * sweep_256  — arc sweep length (0–255 maps to 0°–360°)
 * color      — fill color (0x00RRGGBB — alpha channel ignored, blended to black) */
void arc_ring_draw(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
                   int32_t cx, int32_t cy,
                   int32_t radius, int32_t stroke,
                   uint8_t start_256, uint8_t sweep_256,
                   uint32_t color);
