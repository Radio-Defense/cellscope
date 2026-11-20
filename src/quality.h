#ifndef CELLSCOPE_QUALITY_H
#define CELLSCOPE_QUALITY_H

#include <stdint.h>

typedef struct {
    float score;      /* 0..1, higher is more legible */
    float sharpness;  /* 0..1 */
    float contrast;   /* 0..1 */
    float edges;      /* 0..1 */
    float glare;      /* fraction of clipped highlights */
    float dark;       /* fraction of crushed shadows */
    float mean;       /* mean luma 0..255 */
} FrameQuality;

/* Scores the central 72% x 50% region, the same region as the UI guide.
 * rgb is tightly packed RGB24. */
FrameQuality frame_quality(const uint8_t *rgb, int w, int h);

/* Downscaled luma signature and mean absolute signature difference (0..1).
 * Used for inter-frame motion/stability, not sharpness. */
void frame_luma_signature(const uint8_t *rgb, int w, int h, uint8_t *out, int gw, int gh);
float signature_motion(const uint8_t *a, const uint8_t *b, int n);

#endif
