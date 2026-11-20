#ifndef CELLSCOPE_VLM_H
#define CELLSCOPE_VLM_H

#include <stdbool.h>
#include <stdint.h>

#include "meter.h"
#include "util.h"

typedef enum {
    VLM_BACKEND_AUTO = 0,
    VLM_BACKEND_OLLAMA,
    VLM_BACKEND_LLAMA,
    VLM_BACKEND_MOCK,
    VLM_BACKEND_OFF
} VlmBackend;

typedef struct {
    VlmBackend requested;
    VlmBackend active;
    char url[256];
    char model[128];
    char label[80];
    char detail[1024];
    char error[512];
    bool available;
    bool is_vlm;
    bool probed;
    int64_t last_probe_ms;
} VlmConfig;

void vlm_config_init(VlmConfig *cfg, const char *mode, const char *url, const char *model);
void vlm_config_probe(VlmConfig *cfg);

/* rgb is tightly packed RGB24, w*h*3 bytes. png/png_len are optional; when
 * present they are the encoded frame and avoid a second encode. */
bool vlm_read(VlmConfig *cfg, const uint8_t *rgb, int w, int h, const char *default_unit,
              MeterReading *out, int64_t *frame_ms, char *err, size_t err_cap);
bool vlm_detect(VlmConfig *cfg, const uint8_t *rgb, int w, int h, bool *visible,
                char *err, size_t err_cap);

const char *vlm_backend_name(VlmBackend b);
const char *vlm_prompt_text(void);
double vlm_demo_hz(int64_t now_ms);

#endif
