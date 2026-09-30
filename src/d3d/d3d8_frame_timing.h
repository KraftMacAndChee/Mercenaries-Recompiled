#ifndef D3D8_FRAME_TIMING_H
#define D3D8_FRAME_TIMING_H
#include <stdint.h>
/* Render-thread snapshot of actual successful Present submissions, not vblanks. */
typedef struct D3D8FrameTimingSnapshot {
    uint64_t submitted_frames;
    uint32_t samples;
    double mean_ms, p50_ms, p95_ms, p99_ms, max_ms;
} D3D8FrameTimingSnapshot;
void d3d8_GetFrameTimingSnapshot(D3D8FrameTimingSnapshot *out);
/* Chronological intervals since the caller's previous serial; oldest entries
 * can be overwritten by the bounded ring. Does not consume another reader. */
uint32_t d3d8_GetFrameIntervalHistory(uint64_t after_serial, uint32_t *out,
                                    uint32_t capacity, uint64_t *serial);
#endif
