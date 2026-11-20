#include "vlm.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

#include "cellular.h"
#include "proc.h"
#include "stb_image_write.h"

/* Not declared in the public section of the header, but exported by the
 * implementation in stb_impl.c. */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels,
                                            ptrdiff_t stride_bytes, int x, int y, int n,
                                            int *out_len);

#define VLM_PROMPT_MAX 2048

static const char *k_prompt =
    "You are an optical reader for a handheld pocket frequency meter that a person "
    "is holding up to a webcam. Look at the attached camera frame. A reading is "
    "usable only when the numeric frequency display is clearly visible and in "
    "focus; the screen may be small, angled, held at a distance, mirrored, or "
    "rotated. Mentally correct the orientation before reading. Read the main "
    "measured frequency, not model numbers, channel names, battery icons, voltage or "
    "power fields, or other labels. If several numbers are visible, use the largest, "
    "most central frequency display. Read the digits exactly as shown; never invent "
    "or autocorrect digits. "
    "Determine the unit printed next to the display (Hz, kHz, MHz or GHz). Respond "
    "with JSON only, no markdown, in exactly this form:\n"
    "{\"visible\":true,\"reading\":\"123.456789\",\"unit\":\"MHz\",\"confidence\":0.93}\n"
    "If no display is readable, or it is blurred, cropped, angled or ambiguous, "
    "respond {\"visible\":false,\"reading\":\"\",\"unit\":\"\",\"confidence\":0.0}. "
    "The reading string must contain digits and at most one decimal separator.";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} HttpBuf;

static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    HttpBuf *b = userdata;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static bool http_request(const char *url, const char *body, HttpBuf *out, long *status,
                         char *err, size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    if (status) *status = 0;
    if (err && err_cap) err[0] = '\0';
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_cap, "libcurl initialization failed");
        return false;
    }
    struct curl_slist *headers = NULL;
    if (body) headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, body ? 30000L : 2500L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CellScope/0.1");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    if (rc != CURLE_OK) {
        snprintf(err, err_cap, "%s", curl_easy_strerror(rc));
    } else if (code < 200 || code >= 300) {
        snprintf(err, err_cap, "HTTP %ld", code);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && code >= 200 && code < 300;
}

static void join_url(char *out, size_t cap, const char *base, const char *path)
{
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') n--;
    snprintf(out, cap, "%.*s%s", (int)n, base, path);
}

const char *vlm_prompt_text(void)
{
    return k_prompt;
}

const char *vlm_backend_name(VlmBackend b)
{
    switch (b) {
        case VLM_BACKEND_OLLAMA: return "Ollama";
        case VLM_BACKEND_LLAMA: return "llama.cpp";
        case VLM_BACKEND_TESSERACT: return "Tesseract OCR";
        case VLM_BACKEND_MOCK: return "Demo";
        case VLM_BACKEND_OFF: return "Off";
        default: return "Auto";
    }
}

static bool model_has_vision_hint(const char *name)
{
    return cs_contains_ci(name, "moondream") || cs_contains_ci(name, "llava") ||
           cs_contains_ci(name, "vision") || cs_contains_ci(name, "granite") ||
           cs_contains_ci(name, "qwen2.5vl") || cs_contains_ci(name, "qwen2.5-vl") ||
           cs_contains_ci(name, "minicpm-v") || cs_contains_ci(name, "smolvlm") ||
           cs_contains_ci(name, "gemma3");
}

static bool ollama_probe(VlmConfig *cfg, char *found, size_t found_cap)
{
    char url[512];
    join_url(url, sizeof(url), cfg->url, "/api/tags");
    HttpBuf body = {0};
    char err[256];
    long status = 0;
    if (!http_request(url, NULL, &body, &status, err, sizeof(err))) {
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "Ollama is not reachable at %s (%s). Start it with `ollama serve`.",
                 cfg->url, err[0] ? err : "no response");
        free(body.data);
        return false;
    }

    char chosen[128] = {0};
    if (cfg->model[0]) {
        /* Trust an explicit model name, but verify it exists. */
        char needle[160];
        snprintf(needle, sizeof(needle), "\"name\":\"%s", cfg->model);
        if (strstr(body.data, needle) || strstr(body.data, cfg->model)) {
            cs_strlcpy(chosen, cfg->model, sizeof(chosen));
        }
    }
    if (!chosen[0]) {
        const char *p = body.data;
        while ((p = strstr(p, "\"name\":\"")) != NULL) {
            p += 8;
            const char *end = strchr(p, '"');
            if (!end) break;
            size_t n = (size_t)(end - p);
            if (n < sizeof(chosen)) {
                char name[128];
                memcpy(name, p, n);
                name[n] = '\0';
                bool vision = false;
                const char *cap = strstr(end, "\"capabilities\"");
                if (cap && cap - end < 600) {
                    const char *cap_end = strstr(cap, "]");
                    if (cap_end && cap_end - cap < 600 &&
                        strstr(cap, "\"vision\"") && strstr(cap, "\"vision\"") < cap_end)
                        vision = true;
                }
                if (!vision && model_has_vision_hint(name)) vision = true;
                if (vision) {
                    cs_strlcpy(chosen, name, sizeof(chosen));
                    break;
                }
            }
            p = end;
        }
    }
    free(body.data);

    if (!chosen[0]) {
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "Ollama is running, but no vision model was found. Run "
                 "`ollama pull moondream` (or `ollama pull qwen2.5vl:3b`).");
        return false;
    }
    cs_strlcpy(cfg->model, chosen, sizeof(cfg->model));
    cs_strlcpy(found, chosen, found_cap);
    snprintf(cfg->label, sizeof(cfg->label), "Ollama · %s", chosen);
    snprintf(cfg->detail, sizeof(cfg->detail),
             "Local VLM · %s at %s. Frames are sent only to localhost.", chosen, cfg->url);
    return true;
}

static bool llama_probe(VlmConfig *cfg, char *found, size_t found_cap, bool require_vision)
{
    char url[512];
    join_url(url, sizeof(url), cfg->url, "/v1/models");
    HttpBuf body = {0};
    char err[256];
    long status = 0;
    if (!http_request(url, NULL, &body, &status, err, sizeof(err))) {
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "llama-server is not reachable at %s (%s). Start it with "
                 "`llama-server -m MODEL.gguf --mmproj MMPROJ.gguf --port 8080`.",
                 cfg->url, err[0] ? err : "no response");
        free(body.data);
        return false;
    }

    char chosen[128] = {0};
    if (cfg->model[0]) {
        cs_strlcpy(chosen, cfg->model, sizeof(chosen));
    } else {
        const char *p = body.data;
        while ((p = strstr(p, "\"id\":\"")) != NULL) {
            p += 6;
            const char *end = strchr(p, '"');
            if (!end) break;
            size_t n = (size_t)(end - p);
            if (n < sizeof(chosen)) {
                char id[128];
                memcpy(id, p, n);
                id[n] = '\0';
                if (!require_vision || model_has_vision_hint(id)) {
                    cs_strlcpy(chosen, id, sizeof(chosen));
                    break;
                }
            }
            p = end;
        }
    }
    free(body.data);
    if (!chosen[0]) {
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "llama-server is running but exposes no recognized vision model. "
                 "Start it with a multimodal GGUF and --mmproj, or pass --vlm-model.");
        return false;
    }
    cs_strlcpy(cfg->model, chosen, sizeof(cfg->model));
    cs_strlcpy(found, chosen, found_cap);
    snprintf(cfg->label, sizeof(cfg->label), "llama.cpp · %s", chosen);
    snprintf(cfg->detail, sizeof(cfg->detail),
             "Local VLM via llama-server at %s. Make sure the server was started "
             "with a matching --mmproj file.",
             cfg->url);
    return true;
}

void vlm_config_init(VlmConfig *cfg, const char *mode, const char *url, const char *model,
                     const char *work_dir)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->requested = VLM_BACKEND_AUTO;
    if (mode && *mode) {
        if (strcmp(mode, "ollama") == 0) cfg->requested = VLM_BACKEND_OLLAMA;
        else if (strcmp(mode, "llama") == 0 || strcmp(mode, "llama-server") == 0)
            cfg->requested = VLM_BACKEND_LLAMA;
        else if (strcmp(mode, "tesseract") == 0 || strcmp(mode, "ocr") == 0)
            cfg->requested = VLM_BACKEND_TESSERACT;
        else if (strcmp(mode, "mock") == 0) cfg->requested = VLM_BACKEND_MOCK;
        else if (strcmp(mode, "off") == 0) cfg->requested = VLM_BACKEND_OFF;
    }
    if (url && *url) cs_strlcpy(cfg->url, url, sizeof(cfg->url));
    else if (cfg->requested == VLM_BACKEND_LLAMA) cs_strlcpy(cfg->url, "http://127.0.0.1:8080",
                                                             sizeof(cfg->url));
    else cs_strlcpy(cfg->url, "http://127.0.0.1:11434", sizeof(cfg->url));
    if (model) cs_strlcpy(cfg->model, model, sizeof(cfg->model));
    if (work_dir) cs_strlcpy(cfg->work_dir, work_dir, sizeof(cfg->work_dir));
    cfg->active = VLM_BACKEND_OFF;
    cs_strlcpy(cfg->label, "Not configured", sizeof(cfg->label));
}

void vlm_config_set_workdir(VlmConfig *cfg, const char *dir)
{
    if (cfg && dir) cs_strlcpy(cfg->work_dir, dir, sizeof(cfg->work_dir));
}

static bool tesseract_probe(VlmConfig *cfg)
{
    char found[CS_PATH_MAX];
    if (!proc_find_in_path("tesseract", found, sizeof(found))) {
#ifdef _WIN32
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "Tesseract is not installed. Install it with "
                 "`winget install UB-Mannheim.TesseractOCR` or use a VLM backend.");
#elif defined(__APPLE__)
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "Tesseract is not installed. Install it with `brew install tesseract`, "
                 "or use a VLM backend.");
#else
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "Tesseract is not installed. Install `tesseract-ocr`, or use a VLM "
                 "backend.");
#endif
        return false;
    }
    cs_strlcpy(cfg->tool_path, found, sizeof(cfg->tool_path));
    cs_strlcpy(cfg->label, "Tesseract OCR", sizeof(cfg->label));
    cs_strlcpy(cfg->detail,
               "No vision-language model detected; using local Tesseract OCR as a "
               "fallback. Install Ollama + moondream (or llama-server + SmolVLM) for "
               "true VLM reading.",
               sizeof(cfg->detail));
    return true;
}

void vlm_config_probe(VlmConfig *cfg)
{
    if (!cfg) return;
    cfg->last_probe_ms = cs_now_ms();
    cfg->probed = true;
    cfg->available = false;
    cfg->is_vlm = false;
    cfg->active = VLM_BACKEND_OFF;
    cfg->error[0] = '\0';
    cfg->label[0] = '\0';

    if (cfg->requested == VLM_BACKEND_OFF) {
        cs_strlcpy(cfg->label, "Reader off", sizeof(cfg->label));
        cs_strlcpy(cfg->detail, "Vision reading is disabled (--vlm off).",
                   sizeof(cfg->detail));
        return;
    }
    if (cfg->requested == VLM_BACKEND_MOCK) {
        cfg->active = VLM_BACKEND_MOCK;
        cfg->available = true;
        cfg->is_vlm = true;
        cs_strlcpy(cfg->label, "Demo reader", sizeof(cfg->label));
        cs_strlcpy(cfg->detail, "Synthetic readings — no camera or model is used.",
                   sizeof(cfg->detail));
        return;
    }
    if (cfg->requested == VLM_BACKEND_TESSERACT) {
        if (tesseract_probe(cfg)) {
            cfg->active = VLM_BACKEND_TESSERACT;
            cfg->available = true;
        } else {
            cfg->error[0] = '\0';
            cs_strlcpy(cfg->error, cfg->detail, sizeof(cfg->error));
        }
        return;
    }
    if (cfg->requested == VLM_BACKEND_OLLAMA) {
        char found[128];
        if (ollama_probe(cfg, found, sizeof(found))) {
            cfg->active = VLM_BACKEND_OLLAMA;
            cfg->available = true;
            cfg->is_vlm = true;
        } else {
            cs_strlcpy(cfg->error, cfg->detail, sizeof(cfg->error));
        }
        return;
    }
    if (cfg->requested == VLM_BACKEND_LLAMA) {
        char found[128];
        if (llama_probe(cfg, found, sizeof(found), false)) {
            cfg->active = VLM_BACKEND_LLAMA;
            cfg->available = true;
            cfg->is_vlm = true;
        } else {
            cs_strlcpy(cfg->error, cfg->detail, sizeof(cfg->error));
        }
        return;
    }

    /* Auto: prefer a real VLM on Ollama, then llama-server, then OCR. */
    char found[128];
    VlmConfig saved = *cfg;
    if (ollama_probe(cfg, found, sizeof(found))) {
        cfg->active = VLM_BACKEND_OLLAMA;
        cfg->available = true;
        cfg->is_vlm = true;
        return;
    }
    char ollama_detail[512];
    snprintf(ollama_detail, sizeof(ollama_detail), "%s", cfg->detail);
    *cfg = saved;
    if (llama_probe(cfg, found, sizeof(found), cfg->model[0] == 0)) {
        cfg->active = VLM_BACKEND_LLAMA;
        cfg->available = true;
        cfg->is_vlm = true;
        return;
    }
    char llama_detail[512];
    snprintf(llama_detail, sizeof(llama_detail), "%s", cfg->detail);
    *cfg = saved;
    if (tesseract_probe(cfg)) {
        cfg->active = VLM_BACKEND_TESSERACT;
        cfg->available = true;
        (void)ollama_detail;
        (void)llama_detail;
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "No VLM detected — using local Tesseract OCR as a fallback. Install "
                 "Ollama + moondream (or llama-server + SmolVLM) for true vision reading.");
    } else {
        cfg->active = VLM_BACKEND_OFF;
        snprintf(cfg->detail, sizeof(cfg->detail),
                 "No vision backend available. Install Ollama + moondream, or run "
                 "llama-server with a multimodal GGUF. Tesseract OCR is also missing.");
        cs_strlcpy(cfg->error, cfg->detail, sizeof(cfg->error));
    }
}

static char *encode_png(const uint8_t *rgb, int w, int h, size_t *len)
{
    int n = 0;
    unsigned char *png = stbi_write_png_to_mem(rgb, w * 3, w, h, 3, &n);
    if (!png) return NULL;
    *len = (size_t)n;
    return (char *)png;
}

static bool ollama_read(VlmConfig *cfg, const char *b64, MeterReading *out, char *err,
                        size_t err_cap)
{
    char prompt[VLM_PROMPT_MAX];
    cs_json_escape(k_prompt, prompt, sizeof(prompt));
    size_t cap = strlen(b64) + strlen(cfg->model) + strlen(prompt) + 512;
    char *body = malloc(cap);
    if (!body) {
        snprintf(err, err_cap, "Out of memory");
        return false;
    }
    snprintf(body, cap,
             "{\"model\":\"%s\",\"prompt\":\"%s\",\"images\":[\"%s\"],"
             "\"stream\":false,\"format\":\"json\",\"options\":{\"temperature\":0,"
             "\"num_predict\":96}}",
             cfg->model, prompt, b64);
    char url[512];
    join_url(url, sizeof(url), cfg->url, "/api/generate");
    HttpBuf resp = {0};
    long status = 0;
    char http_err[256];
    bool ok = http_request(url, body, &resp, &status, http_err, sizeof(http_err));
    free(body);
    if (!ok) {
        snprintf(err, err_cap, "Ollama: %s", http_err);
        free(resp.data);
        return false;
    }
    char model_text[4096];
    bool got = cs_json_get_string(resp.data, "response", model_text, sizeof(model_text));
    if (!got || !model_text[0]) {
        snprintf(err, err_cap, "Ollama returned no response field");
        free(resp.data);
        return false;
    }
    bool parsed = meter_parse_model_output(model_text, "MHz", out);
    if (!parsed) {
        snprintf(err, err_cap, "Could not parse model answer: %.180s", model_text);
    } else if (out->confidence <= 0.0) {
        out->confidence = 0.8;
    }
    free(resp.data);
    return parsed;
}

static bool llama_read(VlmConfig *cfg, const char *b64, MeterReading *out, char *err,
                       size_t err_cap)
{
    char prompt[VLM_PROMPT_MAX];
    cs_json_escape(k_prompt, prompt, sizeof(prompt));
    size_t cap = strlen(b64) + strlen(cfg->model) + strlen(prompt) + 768;
    char *body = malloc(cap);
    if (!body) {
        snprintf(err, err_cap, "Out of memory");
        return false;
    }
    snprintf(body, cap,
             "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":["
             "{\"type\":\"text\",\"text\":\"%s\"},"
             "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,%s\"}}"
             "]}],\"max_tokens\":96,\"temperature\":0}",
             cfg->model, prompt, b64);
    char url[512];
    join_url(url, sizeof(url), cfg->url, "/v1/chat/completions");
    HttpBuf resp = {0};
    long status = 0;
    char http_err[256];
    bool ok = http_request(url, body, &resp, &status, http_err, sizeof(http_err));
    free(body);
    if (!ok) {
        snprintf(err, err_cap, "llama-server: %s", http_err);
        free(resp.data);
        return false;
    }
    char model_text[4096];
    bool got = cs_json_get_string(resp.data, "content", model_text, sizeof(model_text));
    if (!got || !model_text[0]) {
        snprintf(err, err_cap, "llama-server returned no message content");
        free(resp.data);
        return false;
    }
    bool parsed = meter_parse_model_output(model_text, "MHz", out);
    if (!parsed) {
        snprintf(err, err_cap, "Could not parse model answer: %.180s", model_text);
    } else if (out->confidence <= 0.0) {
        out->confidence = 0.8;
    }
    free(resp.data);
    return parsed;
}

/* Crop the central guide area, stretch contrast, make the background light,
 * and upscale 2x. Tesseract does much better on that than on a dark camera
 * frame with a tiny display. */
static uint8_t *ocr_preprocess_box(const uint8_t *rgb, int w, int h, float fx, float fy,
                                   float frac_w, float frac_h, int scale, int *ow, int *oh)
{
    int cw = (int)(w * frac_w);
    int ch = (int)(h * frac_h);
    if (cw < 32 || ch < 16) return NULL;
    int x0 = (int)(w * fx);
    int y0 = (int)(h * fy);
    if (x0 + cw > w) cw = w - x0;
    if (y0 + ch > h) ch = h - y0;
    int outw = cw * scale, outh = ch * scale;
    uint8_t *gray = malloc((size_t)cw * ch);
    if (!gray) return NULL;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            const uint8_t *p = rgb + ((size_t)(y + y0) * w + (x + x0)) * 3;
            int lum = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
            gray[(size_t)y * cw + x] = (uint8_t)lum;
        }
    }
    /* Percentile stretch. */
    int hist[256] = {0};
    for (int i = 0; i < cw * ch; i++) hist[gray[i]]++;
    int total = cw * ch, lo = 0, hi = 255, acc = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc > total / 50) {
            lo = i;
            break;
        }
    }
    acc = 0;
    for (int i = 255; i >= 0; i--) {
        acc += hist[i];
        if (acc > total / 50) {
            hi = i;
            break;
        }
    }
    if (hi <= lo) {
        lo = 0;
        hi = 255;
    }
    double mean = 0;
    for (int i = 0; i < total; i++) mean += gray[i];
    mean /= total;
    bool invert = mean < 128.0;
    uint8_t *out = malloc((size_t)outw * outh);
    if (!out) {
        free(gray);
        return NULL;
    }
    for (int y = 0; y < outh; y++) {
        int sy = y / scale;
        for (int x = 0; x < outw; x++) {
            int sx = x / scale;
            int v = gray[(size_t)sy * cw + sx];
            v = (v - lo) * 255 / CS_MAX(1, hi - lo);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            if (invert) v = 255 - v;
            out[(size_t)y * outw + x] = (uint8_t)v;
        }
    }
    free(gray);
    *ow = outw;
    *oh = outh;
    return out;
}

static uint8_t *ocr_preprocess_roi(const uint8_t *rgb, int w, int h, float frac_w,
                                  float frac_h, int scale, int *ow, int *oh)
{
    return ocr_preprocess_box(rgb, w, h, (1.0f - frac_w) / 2.0f, (1.0f - frac_h) / 2.0f,
                              frac_w, frac_h, scale, ow, oh);
}

static uint8_t *ocr_preprocess(const uint8_t *rgb, int w, int h, int *ow, int *oh)
{
    return ocr_preprocess_roi(rgb, w, h, 0.72f, 0.50f, 2, ow, oh);
}

static void ocr_debug(VlmConfig *cfg, const char *fmt, ...)
{
    if (!getenv("CELLSCOPE_OCR_DEBUG") || !cfg->work_dir[0]) return;
    char path[CS_PATH_MAX];
    cs_path_join(path, sizeof(path), cfg->work_dir, "ocr_debug.txt");
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static double unit_scale(const char *unit)
{
    if (!unit) return 0;
    if (strcasecmp(unit, "ghz") == 0) return 1e9;
    if (strcasecmp(unit, "mhz") == 0) return 1e6;
    if (strcasecmp(unit, "khz") == 0) return 1e3;
    if (strcasecmp(unit, "hz") == 0) return 1.0;
    return 0;
}

/* A wide, low-magnification OCR pass used only to find the printed unit when
 * the digit crop did not include it. */
static void scan_unit_text(const char *text, char *out, size_t cap)
{
    char low[4096];
    cs_strlcpy(low, text, sizeof(low));
    cs_lower(low);
    const char *units[] = {"ghz", "mhz", "khz", "hz"};
    const char *names[] = {"GHz", "MHz", "kHz", "Hz"};
    for (size_t i = 0; i < 4; i++) {
        const char *p = low;
        while ((p = strstr(p, units[i])) != NULL) {
            char prev = p == low ? ' ' : p[-1];
            char next = p[strlen(units[i])];
            bool ok_prev = !isalnum((unsigned char)prev);
            bool ok_next = !isalnum((unsigned char)next);
            if (ok_prev && ok_next) {
                cs_strlcpy(out, names[i], cap);
                return;
            }
            p += strlen(units[i]);
        }
    }
}

/* A low-magnification pass over the bottom and right edges, where pocket
 * meters usually print their unit. Only runs when the digit crop omitted it. */
static void detect_unit_ocr(VlmConfig *cfg, const uint8_t *rgb, int w, int h, char *out,
                            size_t cap)
{
    out[0] = '\0';
    struct {
        float fx, fy, fw, fh;
        const char *name;
    } boxes[] = {
        {0.03f, 0.62f, 0.94f, 0.34f, "ocr_unit_bottom.png"},
        {0.62f, 0.15f, 0.35f, 0.70f, "ocr_unit_right.png"},
    };
    for (size_t b = 0; b < CS_ARRAY_LEN(boxes); b++) {
        int ow = 0, oh = 0;
        uint8_t *gray = ocr_preprocess_box(rgb, w, h, boxes[b].fx, boxes[b].fy, boxes[b].fw,
                                           boxes[b].fh, 3, &ow, &oh);
        if (!gray) continue;
        char path[CS_PATH_MAX];
        cs_path_join(path, sizeof(path), cfg->work_dir, boxes[b].name);
        if (!stbi_write_png(path, ow, oh, 1, gray, ow)) {
            free(gray);
            continue;
        }
        free(gray);
        const char *modes[] = {"7", "6"};
        for (size_t m = 0; m < 2 && !out[0]; m++) {
            const char *argv[] = {cfg->tool_path, boxes[b].name, "stdout", "--psm",
                                  modes[m], NULL};
            char text[4096], err[256];
            int code = -1;
            bool timed_out = false;
            if (proc_run_in(cfg->work_dir, argv, text, sizeof(text), 4000, &code,
                            &timed_out, err, sizeof(err)) &&
                !timed_out) {
                scan_unit_text(text, out, cap);
            }
        }
        if (out[0]) return;
    }
}

typedef struct {
    int y0, y1;
    double score;
} OcrBand;

static int cmp_band(const void *a, const void *b)
{
    const OcrBand *x = a, *y = b;
    if (x->score < y->score) return 1;
    if (x->score > y->score) return -1;
    return 0;
}

/* Find horizontal text bands by row-to-row edge density. This works for
 * low-contrast/reflective displays where a simple dark-pixel count is
 * dominated by the camera background. */
static int ocr_find_bands(const uint8_t *gray, int w, int h, OcrBand *out, int max)
{
    if (!gray || w < 32 || h < 32) return 0;
    int *ink = calloc((size_t)h, sizeof(int));
    if (!ink) return 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = gray + (size_t)y * w;
        int n = 0;
        int prev = row[0];
        for (int x = 1; x < w; x++) {
            int d = row[x] - prev;
            if (d < 0) d = -d;
            if (d > 10) n++;
            prev = row[x];
        }
        ink[y] = n;
    }
    int thr = CS_MAX(8, w / 200);
    int count = 0, y = 0;
    while (y < h && count < max) {
        while (y < h && ink[y] < thr) y++;
        if (y >= h) break;
        int start = y;
        int last_ink = y;
        while (y < h) {
            if (ink[y] >= thr) {
                last_ink = y;
            } else if (y - last_ink > 5) {
                break;
            }
            y++;
        }
        int end = last_ink;
        int band_h = end - start + 1;
        if (band_h >= 8 && band_h <= h / 2 + 2) {
            long sum = 0;
            for (int k = start; k <= end; k++) sum += ink[k];
            out[count].y0 = start;
            out[count].y1 = end;
            out[count].score = (double)sum * (1.0 + band_h * 0.012);
            count++;
        }
        y = end + 1;
    }
    free(ink);
    qsort(out, (size_t)count, sizeof(*out), cmp_band);
    return count;
}

/* 0=identity, 1=mirror, 2=180, 3=mirror+180, 4=90cw, 5=270cw,
 * 6=mirror+90, 7=mirror+270 */
static uint8_t *ocr_transform(const uint8_t *src, int w, int h, int mode, int *ow, int *oh)
{
    if (mode == 0) {
        uint8_t *copy = malloc((size_t)w * h);
        if (!copy) return NULL;
        memcpy(copy, src, (size_t)w * h);
        *ow = w;
        *oh = h;
        return copy;
    }
    if (mode == 1 || mode == 3) {
        uint8_t *dst = malloc((size_t)w * h);
        if (!dst) return NULL;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int sy = (mode == 3) ? h - 1 - y : y;
                int sx = (mode == 1) ? w - 1 - x : x;
                dst[(size_t)y * w + x] = src[(size_t)sy * w + sx];
            }
        *ow = w;
        *oh = h;
        return dst;
    }
    if (mode == 2) {
        uint8_t *dst = malloc((size_t)w * h);
        if (!dst) return NULL;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[(size_t)y * w + x] = src[(size_t)(h - 1 - y) * w + (w - 1 - x)];
        *ow = w;
        *oh = h;
        return dst;
    }
    int cw = h, ch = w;
    uint8_t *dst = malloc((size_t)cw * ch);
    if (!dst) return NULL;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            int sx, sy;
            if (mode == 4) {          /* rotate 90 CW */
                sx = w - 1 - y;
                sy = x;
            } else if (mode == 5) {   /* rotate 270 CW */
                sx = y;
                sy = h - 1 - x;
            } else if (mode == 6) {   /* mirror + 90 */
                sx = y;
                sy = x;
            } else {                  /* mirror + 270 */
                sx = w - 1 - y;
                sy = h - 1 - x;
            }
            dst[(size_t)y * cw + x] = src[(size_t)sy * w + sx];
        }
    }
    *ow = cw;
    *oh = ch;
    return dst;
}

static int digit_count(const char *s)
{
    int n = 0;
    for (; s && *s; s++)
        if (isdigit((unsigned char)*s)) n++;
    return n;
}

static bool ocr_parse_text(const char *text, const char *default_unit, MeterReading *out,
                           int *score)
{
    if (!text || !*text) return false;
    char low[4096];
    cs_strlcpy(low, text, sizeof(low));
    cs_lower(low);
    /* Tesseract commonly reads the final z of MHz/GHz/kHz as 2. */
    for (char *p = low; (p = strstr(p, "mh2")) != NULL; p += 3)
        if (!isalnum((unsigned char)p[3])) p[2] = 'z';
    for (char *p = low; (p = strstr(p, "gh2")) != NULL; p += 3)
        if (!isalnum((unsigned char)p[3])) p[2] = 'z';
    for (char *p = low; (p = strstr(p, "kh2")) != NULL; p += 3)
        if (!isalnum((unsigned char)p[3])) p[2] = 'z';
    for (char *p = low; (p = strstr(p, "nhz")) != NULL; p += 3)
        p[0] = 'm';
    for (char *p = low; (p = strstr(p, "whz")) != NULL; p += 3)
        p[0] = 'm';
    const char *start = low;
    char *eq = strstr(low, "req");
    if (!eq) eq = strstr(low, "eq");
    if (eq) start = eq + 2;
    char candidate[4096];
    size_t co = 0;
    for (const char *p = start; *p && co + 1 < sizeof(candidate); p++) {
        if (!isspace((unsigned char)*p)) candidate[co++] = *p;
    }
    candidate[co] = '\0';
    char raw_saved[256];
    cs_strlcpy(raw_saved, text, sizeof(raw_saved));
    MeterReading r;
    if (!meter_parse_text(candidate, default_unit, &r) || !r.visible) return false;
    if (r.hz < 1e6 || r.hz > 1e11) return false;

    /* Inspect the first numeric token: reject the common OCR failure where a
     * reflection produces leading zeros and an implausible huge value. */
    const char *num = candidate;
    while (*num && !isdigit((unsigned char)*num)) num++;
    int int_digits = 0;
    while (num[int_digits] >= '0' && num[int_digits] <= '9') int_digits++;
    bool leading_zero = int_digits > 1 && num[0] == '0';
    bool unit = cs_contains_ci(candidate, "hz");
    if (leading_zero) return false;
    /* A unit-less OCR is a fallback; do not let a reflection turn 2.5 GHz
     * into 52 GHz. */
    if (!unit && r.hz > 12e9) return false;

    int digits = digit_count(candidate);
    if (digits < 4) return false;
    int sc = digits + (unit ? 8 : 0);
    if (eq || strstr(low, "freq")) sc += 4;
    if (r.hz >= 1e6 && r.hz <= 1e11) sc += 2;
    *out = r;
    cs_strlcpy(out->raw, raw_saved, sizeof(out->raw));
    if (out->confidence <= 0.0 || out->confidence > 0.9) out->confidence = 0.56;
    if (score) *score = sc;
    return true;
}

/* Try one orientation: locate horizontal bands, OCR each, and keep the best
 * plausible frequency. Returns true for a high-confidence hit. */
static bool ocr_try_orientation(VlmConfig *cfg, const uint8_t *gray, int w, int h,
                                int mode, const char *default_unit, MeterReading *best,
                                int *best_score, int *calls)
{
    int ow = 0, oh = 0;
    uint8_t *img = ocr_transform(gray, w, h, mode, &ow, &oh);
    if (!img) return false;
    OcrBand bands[12];
    int nb = ocr_find_bands(img, ow, oh, bands, 12);
    bool strong = false;
    ocr_debug(cfg, "=== orientation %d: %d bands on %dx%d ===", mode, nb, ow, oh);
    for (int b = 0; b < nb && b < 4; b++) {
        int pad = 8;
        int y0 = CS_MAX(0, bands[b].y0 - pad);
        int y1 = CS_MIN(oh - 1, bands[b].y1 + pad);
        int bh = y1 - y0 + 1;
        if (bh < 12) continue;
        char path[CS_PATH_MAX];
        cs_path_join(path, sizeof(path), cfg->work_dir, "ocr_band.png");
        if (!stbi_write_png(path, ow, bh, 1, img + (size_t)y0 * ow, ow)) continue;
        if (getenv("CELLSCOPE_OCR_DEBUG")) {
            char dbg[CS_PATH_MAX];
            snprintf(dbg, sizeof(dbg), "%s/ocr_band_%d_%d.png", cfg->work_dir, mode, b);
            stbi_write_png(dbg, ow, bh, 1, img + (size_t)y0 * ow, ow);
        }
        ocr_debug(cfg, "band %d y=%d..%d score=%.1f", b, y0, y1, bands[b].score);
        char text[4096] = {0};
        for (int pass = 0; pass < 2; pass++) {
            const char *psm = pass == 0 ? "7" : "6";
            const char *argv[] = {cfg->tool_path, "ocr_band.png", "stdout", "--psm",
                                  psm, NULL};
            char err[256];
            int code = -1;
            bool timed_out = false;
            if (calls) (*calls)++;
            if (!proc_run_in(cfg->work_dir, argv, text, sizeof(text), 5000, &code,
                             &timed_out, err, sizeof(err)) ||
                timed_out)
                continue;
            ocr_debug(cfg, "  psm %s => %s", psm, text);
            MeterReading r;
            int score = 0;
            if (ocr_parse_text(text, default_unit, &r, &score)) {
                ocr_debug(cfg, "    candidate mode=%d score=%d hz=%.0f raw=%s", mode,
                          score, r.hz, r.raw);
                if (score > *best_score) {
                    *best = r;
                    *best_score = score;
                }
                bool has_unit = cs_contains_ci(text, "hz");
                if (has_unit && digit_count(text) >= 6 && score >= 14) {
                    strong = true;
                    break;
                }
                break;
            }
        }
        if (strong) break;
    }
    free(img);
    return strong;
}

static bool tesseract_read(VlmConfig *cfg, const uint8_t *rgb, int w, int h,
                           const char *default_unit, MeterReading *out, char *err,
                           size_t err_cap)
{
    if (!cfg->tool_path[0]) {
        snprintf(err, err_cap, "Tesseract not found");
        return false;
    }
    if (!cfg->work_dir[0]) {
        snprintf(err, err_cap, "No work directory configured");
        return false;
    }
    int ow = 0, oh = 0;
    uint8_t *gray = ocr_preprocess(rgb, w, h, &ow, &oh);
    if (!gray) {
        snprintf(err, err_cap, "Could not preprocess frame");
        return false;
    }
    if (getenv("CELLSCOPE_OCR_DEBUG")) {
        char dbg[CS_PATH_MAX];
        cs_path_join(dbg, sizeof(dbg), cfg->work_dir, "ocr_frame.png");
        stbi_write_png(dbg, ow, oh, 1, gray, ow);
    }
    MeterReading best;
    memset(&best, 0, sizeof(best));
    int best_score = -1;
    int calls = 0;
    int64_t started = cs_now_ms();
    bool strong = false;
    /* Normal orientation first; mirror next because front-camera previews are
     * commonly mirrored; then 180/flipped and finally quarter turns. */
    const int modes[] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (size_t i = 0; i < CS_ARRAY_LEN(modes); i++) {
        if (ocr_try_orientation(cfg, gray, ow, oh, modes[i],
                                default_unit ? default_unit : "MHz", &best, &best_score,
                                &calls)) {
            strong = true;
            break;
        }
        if (cs_now_ms() - started > 15000 || calls >= 40) break;
    }
    free(gray);
    if (best_score < 12) {
        snprintf(err, err_cap, "OCR found no readable frequency");
        return false;
    }
    if (!cs_contains_ci(best.raw, "hz") && !strong) {
        char unit[8];
        detect_unit_ocr(cfg, rgb, w, h, unit, sizeof(unit));
        if (unit[0]) {
            double from = unit_scale(best.unit[0] ? best.unit : default_unit);
            double to = unit_scale(unit);
            if (from > 0 && to > 0) best.hz = best.hz / from * to;
            cs_strlcpy(best.unit, unit, sizeof(best.unit));
        }
    }
    *out = best;
    err[0] = '\0';
    return true;
}

double vlm_demo_hz(int64_t now_ms)
{
    double t = (double)now_ms / 1000.0;
    double base = 2160150000.0;
    double drift = sin(t * 0.11) * 800.0 + sin(t * 0.73) * 35.0;
    return base + drift;
}

static bool mock_read(MeterReading *out)
{
    double hz = vlm_demo_hz(cs_epoch_ms());
    memset(out, 0, sizeof(*out));
    out->visible = true;
    out->hz = hz;
    out->confidence = 0.94;
    cs_strlcpy(out->unit, "GHz", sizeof(out->unit));
    cs_format_eng((uint64_t)(hz + 0.5), out->raw, sizeof(out->raw));
    return true;
}

bool vlm_read(VlmConfig *cfg, const uint8_t *rgb, int w, int h, const char *default_unit,
              MeterReading *out, int64_t *frame_ms, char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (out) memset(out, 0, sizeof(*out));
    if (!cfg || !cfg->available || !out) {
        snprintf(err, err_cap, "No reader backend is available");
        return false;
    }
    if (frame_ms) *frame_ms = cs_epoch_ms();
    if (cfg->active == VLM_BACKEND_MOCK) return mock_read(out);
    if (!rgb || w <= 0 || h <= 0) {
        snprintf(err, err_cap, "No camera frame");
        return false;
    }

    size_t png_len = 0;
    char *png = encode_png(rgb, w, h, &png_len);
    if (!png) {
        snprintf(err, err_cap, "PNG encoding failed");
        return false;
    }
    if (cfg->active == VLM_BACKEND_TESSERACT) {
        bool ok = tesseract_read(cfg, rgb, w, h, default_unit ? default_unit : "MHz", out,
                                 err, err_cap);
        free(png);
        return ok;
    }

    char *b64 = cs_base64_encode((const uint8_t *)png, png_len);
    free(png);
    if (!b64) {
        snprintf(err, err_cap, "Base64 encoding failed");
        return false;
    }
    bool ok = false;
    if (cfg->active == VLM_BACKEND_OLLAMA)
        ok = ollama_read(cfg, b64, out, err, err_cap);
    else if (cfg->active == VLM_BACKEND_LLAMA)
        ok = llama_read(cfg, b64, out, err, err_cap);
    else
        snprintf(err, err_cap, "Unsupported backend");
    free(b64);
    if (ok) {
        if (out->visible && out->hz > 0) {
            if (out->confidence <= 0.0) out->confidence = 0.75;
        }
    }
    return ok;
}
