#include "quality.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

FrameQuality frame_quality(const uint8_t *rgb, int w, int h)
{
    FrameQuality q;
    memset(&q, 0, sizeof(q));
    if (!rgb || w < 32 || h < 16) return q;

    int cw = (int)(w * 0.72f);
    int ch = (int)(h * 0.50f);
    int x0 = (w - cw) / 2;
    int y0 = (h - ch) / 2;
    int step = 2;
    int gw = cw / step;
    int gh = ch / step;
    if (gw < 8 || gh < 8) return q;

    uint8_t *gray = malloc((size_t)gw * gh);
    if (!gray) return q;

    double sum = 0, sum2 = 0;
    int clipped = 0, dark = 0;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            const uint8_t *p = rgb + ((size_t)(y0 + y * step) * w + (x0 + x * step)) * 3;
            int lum = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
            gray[(size_t)y * gw + x] = (uint8_t)lum;
            sum += lum;
            sum2 += (double)lum * lum;
            if (lum >= 250) clipped++;
            if (lum <= 5) dark++;
        }
    }
    int n = gw * gh;
    double mean = sum / n;
    double var = sum2 / n - mean * mean;
    if (var < 0) var = 0;
    double sd = sqrt(var);

    /* Laplacian and gradient energy over the ROI. */
    double lap = 0, grad = 0;
    int edge_count = 0;
    int count = 0;
    for (int y = 1; y < gh - 1; y++) {
        for (int x = 1; x < gw - 1; x++) {
            int c = gray[(size_t)y * gw + x];
            int l = gray[(size_t)y * gw + x - 1];
            int r = gray[(size_t)y * gw + x + 1];
            int u = gray[(size_t)(y - 1) * gw + x];
            int d = gray[(size_t)(y + 1) * gw + x];
            int lp = abs(4 * c - l - r - u - d);
            int gx = r - l;
            int gy = d - u;
            double g = sqrt((double)gx * gx + (double)gy * gy);
            lap += lp;
            grad += g;
            if (g > 22.0) edge_count++;
            count++;
        }
    }
    if (count <= 0) {
        free(gray);
        return q;
    }
    double lap_mean = lap / count;
    double grad_mean = grad / count;
    double edge_frac = (double)edge_count / count;

    q.sharpness = clamp01((float)(lap_mean / 18.0));
    q.contrast = clamp01((float)(sd / 42.0));
    q.edges = clamp01((float)(edge_frac / 0.16));
    q.glare = (float)clipped / n;
    q.dark = (float)dark / n;
    q.mean = (float)mean;

    float brightness = 1.0f - fabsf((float)mean - 128.0f) / 128.0f;
    if (brightness < 0) brightness = 0;
    q.score = 0.56f * q.sharpness + 0.22f * q.contrast + 0.13f * q.edges +
              0.09f * brightness;
    q.score -= 3.5f * q.glare + 1.5f * q.dark;
    if (edge_frac < 0.008) q.score -= 0.18f;
    if (grad_mean < 4.0) q.score -= 0.12f;
    q.score = clamp01(q.score);
    free(gray);
    return q;
}
