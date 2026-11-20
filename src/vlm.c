#include "vlm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

#include "cellular.h"

/* Not declared in the public section of the header, but exported by the
 * implementation in stb_impl.c. */
extern unsigned char *stbi_write_png_to_mem(const unsigned char *pixels,
                                            ptrdiff_t stride_bytes, int x, int y, int n,
                                            int *out_len);

#define VLM_PROMPT_MAX 2048

static const char *k_prompt =
    "What frequency is shown on the display? Answer with the number and unit only. "
    "Ignore zero values; if the frequency is zero, answer NONE.";

static const char *k_presence_prompt =
    "Is a frequency counter or device display with numbers visible in this image? "
    "Answer yes or no.";

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

void vlm_config_init(VlmConfig *cfg, const char *mode, const char *url, const char *model)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->requested = VLM_BACKEND_AUTO;
    if (mode && *mode) {
        if (strcmp(mode, "ollama") == 0) cfg->requested = VLM_BACKEND_OLLAMA;
        else if (strcmp(mode, "llama") == 0 || strcmp(mode, "llama-server") == 0)
            cfg->requested = VLM_BACKEND_LLAMA;
        else if (strcmp(mode, "mock") == 0) cfg->requested = VLM_BACKEND_MOCK;
        else if (strcmp(mode, "off") == 0) cfg->requested = VLM_BACKEND_OFF;
    }
    if (url && *url) cs_strlcpy(cfg->url, url, sizeof(cfg->url));
    else if (cfg->requested == VLM_BACKEND_LLAMA) cs_strlcpy(cfg->url, "http://127.0.0.1:8080",
                                                             sizeof(cfg->url));
    else cs_strlcpy(cfg->url, "http://127.0.0.1:11434", sizeof(cfg->url));
    if (model) cs_strlcpy(cfg->model, model, sizeof(cfg->model));
    cfg->active = VLM_BACKEND_OFF;
    cs_strlcpy(cfg->label, "Not configured", sizeof(cfg->label));
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

    /* Auto: prefer a real VLM on Ollama, then llama-server. */
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
    cfg->active = VLM_BACKEND_OFF;
    snprintf(cfg->detail, sizeof(cfg->detail),
             "No vision-language model is available. The local MiniMind/HF reader may "
             "still be starting; otherwise start Ollama or llama-server with a "
             "multimodal model.");
    cs_strlcpy(cfg->error, cfg->detail, sizeof(cfg->error));
}

/* Crop the meter guide region (top 75% of the central 72%x50% ROI) and scale
 * it to 640 px wide. This removes most of the face/background and makes the
 * meter digits large enough for a small VLM. */
static uint8_t *prepare_guide_crop(const uint8_t *rgb, int w, int h, int *ow, int *oh)
{
    if (!rgb || w < 32 || h < 16) return NULL;
    int gw = (int)(w * 0.72f);
    int gh = (int)(h * 0.50f);
    int x0 = (w - gw) / 2;
    int y0 = (h - gh) / 2;
    int ch = (int)(gh * 0.75f);
    if (gw < 32 || ch < 16) return NULL;
    int tw = 640;
    int th = (int)((long)ch * tw / gw);
    if (th < 16) return NULL;
    uint8_t *out = malloc((size_t)tw * th * 3);
    if (!out) return NULL;
    for (int y = 0; y < th; y++) {
        int sy = y0 + (int)((long)y * ch / th);
        if (sy >= h) sy = h - 1;
        for (int x = 0; x < tw; x++) {
            int sx = x0 + (int)((long)x * gw / tw);
            if (sx >= w) sx = w - 1;
            const uint8_t *p = rgb + ((size_t)sy * w + sx) * 3;
            uint8_t *q = out + ((size_t)y * tw + x) * 3;
            q[0] = p[0];
            q[1] = p[1];
            q[2] = p[2];
        }
    }
    *ow = tw;
    *oh = th;
    return out;
}

static char *encode_png(const uint8_t *rgb, int w, int h, size_t *len)
{
    int n = 0;
    unsigned char *png = stbi_write_png_to_mem(rgb, w * 3, w, h, 3, &n);
    if (!png) return NULL;
    *len = (size_t)n;
    return (char *)png;
}

static bool ollama_read(VlmConfig *cfg, const char *b64, const char *default_unit,
                        MeterReading *out, char *err, size_t err_cap)
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
    bool parsed = meter_parse_model_output(model_text, default_unit, out);
    if (!parsed) {
        snprintf(err, err_cap, "Could not parse model answer: %.180s", model_text);
    } else if (out->confidence <= 0.0) {
        out->confidence = 0.8;
    }
    free(resp.data);
    return parsed;
}

static bool llama_read(VlmConfig *cfg, const char *b64, const char *default_unit,
                       MeterReading *out, char *err, size_t err_cap)
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
    bool parsed = meter_parse_model_output(model_text, default_unit, out);
    if (!parsed) {
        snprintf(err, err_cap, "Could not parse model answer: %.180s", model_text);
    } else if (out->confidence <= 0.0) {
        out->confidence = 0.8;
    }
    free(resp.data);
    return parsed;
}

static bool parse_presence_text(const char *text, bool *visible)
{
    if (!text) return false;
    char low[512];
    cs_strlcpy(low, text, sizeof(low));
    cs_lower(low);
    cs_trim(low);
    if (strstr(low, "\"visible\":false") || strstr(low, "\"visible\": false"))
        { *visible = false; return true; }
    if (strstr(low, "\"visible\":true") || strstr(low, "\"visible\": true"))
        { *visible = true; return true; }
    if (strstr(low, "false")) { *visible = false; return true; }
    if (strstr(low, "true")) { *visible = true; return true; }
    for (const char *p = low; *p; p++) {
        if (*p == 'y') { *visible = true; return true; }
        if (*p == 'n') { *visible = false; return true; }
    }
    return false;
}

static bool ollama_detect(VlmConfig *cfg, const char *b64, bool *visible, char *err,
                          size_t err_cap)
{
    char prompt[VLM_PROMPT_MAX];
    cs_json_escape(k_presence_prompt, prompt, sizeof(prompt));
    size_t cap = strlen(b64) + strlen(cfg->model) + strlen(prompt) + 512;
    char *body = malloc(cap);
    if (!body) { snprintf(err, err_cap, "Out of memory"); return false; }
    snprintf(body, cap,
             "{\"model\":\"%s\",\"prompt\":\"%s\",\"images\":[\"%s\"],"
             "\"stream\":false,\"options\":{\"temperature\":0,\"num_predict\":16}}",
             cfg->model, prompt, b64);
    char url[512];
    join_url(url, sizeof(url), cfg->url, "/api/generate");
    HttpBuf resp = {0};
    long status = 0;
    char http_err[256];
    bool ok = http_request(url, body, &resp, &status, http_err, sizeof(http_err));
    free(body);
    if (!ok) { snprintf(err, err_cap, "Ollama: %s", http_err); free(resp.data); return false; }
    char text[1024];
    bool got = cs_json_get_string(resp.data, "response", text, sizeof(text));
    free(resp.data);
    if (!got || !parse_presence_text(text, visible)) {
        snprintf(err, err_cap, "Presence check returned: %.120s", text);
        return false;
    }
    return true;
}

static bool llama_detect(VlmConfig *cfg, const char *b64, bool *visible, char *err,
                         size_t err_cap)
{
    char prompt[VLM_PROMPT_MAX];
    cs_json_escape(k_presence_prompt, prompt, sizeof(prompt));
    size_t cap = strlen(b64) + strlen(cfg->model) + strlen(prompt) + 640;
    char *body = malloc(cap);
    if (!body) { snprintf(err, err_cap, "Out of memory"); return false; }
    snprintf(body, cap,
             "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":["
             "{\"type\":\"text\",\"text\":\"%s\"},"
             "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,%s\"}}"
             "]}],\"max_tokens\":16,\"temperature\":0}",
             cfg->model, prompt, b64);
    char url[512];
    join_url(url, sizeof(url), cfg->url, "/v1/chat/completions");
    HttpBuf resp = {0};
    long status = 0;
    char http_err[256];
    bool ok = http_request(url, body, &resp, &status, http_err, sizeof(http_err));
    free(body);
    if (!ok) { snprintf(err, err_cap, "llama-server: %s", http_err); free(resp.data); return false; }
    char text[1024];
    bool got = cs_json_get_string(resp.data, "content", text, sizeof(text));
    free(resp.data);
    if (!got || !parse_presence_text(text, visible)) {
        snprintf(err, err_cap, "Presence check returned: %.120s", text);
        return false;
    }
    return true;
}

bool vlm_detect(VlmConfig *cfg, const uint8_t *rgb, int w, int h, bool *visible, char *err,
                size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (!visible) return false;
    *visible = false;
    if (!cfg || !cfg->available) {
        snprintf(err, err_cap, "No reader backend is available");
        return false;
    }
    if (cfg->active == VLM_BACKEND_MOCK) { *visible = true; return true; }
    if (!rgb || w <= 0 || h <= 0) {
        snprintf(err, err_cap, "No camera frame");
        return false;
    }
    int cw = 0, ch = 0;
    uint8_t *crop = prepare_guide_crop(rgb, w, h, &cw, &ch);
    if (!crop) {
        snprintf(err, err_cap, "Could not prepare the guide crop");
        return false;
    }
    size_t png_len = 0;
    char *png = encode_png(crop, cw, ch, &png_len);
    free(crop);
    if (!png) { snprintf(err, err_cap, "PNG encoding failed"); return false; }
    char *b64 = cs_base64_encode((const uint8_t *)png, png_len);
    free(png);
    if (!b64) { snprintf(err, err_cap, "Base64 encoding failed"); return false; }
    bool ok = false;
    if (cfg->active == VLM_BACKEND_OLLAMA)
        ok = ollama_detect(cfg, b64, visible, err, err_cap);
    else if (cfg->active == VLM_BACKEND_LLAMA)
        ok = llama_detect(cfg, b64, visible, err, err_cap);
    else
        snprintf(err, err_cap, "Unsupported backend");
    free(b64);
    return ok;
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

    int cw = 0, ch = 0;
    uint8_t *crop = prepare_guide_crop(rgb, w, h, &cw, &ch);
    if (!crop) {
        snprintf(err, err_cap, "Could not prepare the meter guide crop");
        return false;
    }
    size_t png_len = 0;
    char *png = encode_png(crop, cw, ch, &png_len);
    free(crop);
    if (!png) {
        snprintf(err, err_cap, "PNG encoding failed");
        return false;
    }
    char *b64 = cs_base64_encode((const uint8_t *)png, png_len);
    free(png);
    if (!b64) {
        snprintf(err, err_cap, "Base64 encoding failed");
        return false;
    }
    bool ok = false;
    if (cfg->active == VLM_BACKEND_OLLAMA)
        ok = ollama_read(cfg, b64, default_unit, out, err, err_cap);
    else if (cfg->active == VLM_BACKEND_LLAMA)
        ok = llama_read(cfg, b64, default_unit, out, err, err_cap);
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
