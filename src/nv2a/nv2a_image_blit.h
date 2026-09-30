/* Bounds and scaling for explicit NV09F copies; no surface lifetime policy. */
#ifndef NV2A_IMAGE_BLIT_H
#define NV2A_IMAGE_BLIT_H
#include <stdint.h>
typedef struct PgraphImageBlitRect {
    uint32_t left, top, right, bottom;
} PgraphImageBlitRect;

static int pgraph_image_blit_scale_rect(uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t guest_width, uint32_t guest_height,
    uint32_t host_width, uint32_t host_height, PgraphImageBlitRect *rect)
{
    if (!rect || !width || !height || !guest_width || !guest_height ||
        !host_width || !host_height || x >= guest_width || y >= guest_height ||
        width > guest_width - x || height > guest_height - y) return 0;
    rect->left = (uint32_t)((uint64_t)x * host_width / guest_width);
    rect->top = (uint32_t)((uint64_t)y * host_height / guest_height);
    rect->right = (uint32_t)((uint64_t)(x + width) * host_width / guest_width);
    rect->bottom = (uint32_t)((uint64_t)(y + height) * host_height / guest_height);
    return rect->right > rect->left && rect->bottom > rect->top;
}
#endif
