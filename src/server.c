#include "server.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep((DWORD)(ms))
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((useconds_t)((ms) * 1000))
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} HttpBuf;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    HttpBuf *b = ud;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
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

static bool http_get(const char *url, char *out, size_t cap, long *status)
{
    if (out && cap) out[0] = '\0';
    if (status) *status = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    HttpBuf b = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 600L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1800L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CellScope/0.1");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    curl_easy_cleanup(curl);
    if (out) {
        if (b.data) cs_strlcpy(out, b.data, cap);
        else out[0] = '\0';
    }
    free(b.data);
    return rc == CURLE_OK && code >= 200 && code < 300;
}

static void read_log_tail(const char *path, char *out, size_t cap)
{
    out[0] = '\0';
    uint8_t *data = NULL;
    size_t len = 0;
    if (!cs_read_file(path, &data, &len)) return;
    const char *p = (const char *)data;
    if (len > 3500) p = p + len - 3500;
    cs_strlcpy(out, p, cap);
    free(data);
}

bool server_healthy(MiniMindServer *s)
{
    if (!s) return false;
    char url[256], body[4096];
    snprintf(url, sizeof(url), "%s/health", s->base_url);
    long status = 0;
    if (!http_get(url, body, sizeof(body), &status)) return false;
    const char *id = s->health_id[0] ? s->health_id : "minimind-meter";
    return status == 200 && strstr(body, id) != NULL;
}

bool server_start(MiniMindServer *s, const char *python, const char *script,
                  const char *minimind, const char *save_dir, const char *weight, int port,
                  const char *log_path, char *err, size_t err_cap)
{
    memset(s, 0, sizeof(*s));
    s->port = port;
    snprintf(s->base_url, sizeof(s->base_url), "http://127.0.0.1:%d", port);
    cs_strlcpy(s->log_path, log_path, sizeof(s->log_path));
    cs_strlcpy(s->weight, weight, sizeof(s->weight));
    cs_strlcpy(s->health_id, "minimind-meter", sizeof(s->health_id));
    cs_strlcpy(s->kind, "minimind", sizeof(s->kind));
    if (err && err_cap) err[0] = '\0';

    if (server_healthy(s)) {
        s->running = true;
        s->external = true;
        return true;
    }
    char checkpoint[CS_PATH_MAX];
    snprintf(checkpoint, sizeof(checkpoint), "%s/%s_768.pth", save_dir, weight);
    if (!cs_file_exists(checkpoint)) {
        snprintf(err, err_cap, "MiniMind checkpoint not found: %s", checkpoint);
        return false;
    }
    const char *argv[] = {python,
                          script,
                          "--minimind",
                          minimind,
                          "--save_dir",
                          save_dir,
                          "--weight",
                          weight,
                          "--port",
                          NULL,
                          NULL};
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    argv[9] = portbuf;
    FILE *f = fopen(log_path, "a");
    if (f) {
        fprintf(f, "\n=== starting MiniMind server ===\n%s %s\n", python, script);
        fclose(f);
    }
    if (!proc_spawn(minimind, argv, log_path, &s->child, err, err_cap)) {
        s->running = false;
        return false;
    }
    s->running = true;
    return true;
}

bool server_start_hf(MiniMindServer *s, const char *python, const char *script,
                     const char *model_path, int port, const char *device,
                     const char *log_path, char *err, size_t err_cap)
{
    memset(s, 0, sizeof(*s));
    s->port = port;
    snprintf(s->base_url, sizeof(s->base_url), "http://127.0.0.1:%d", port);
    cs_strlcpy(s->log_path, log_path, sizeof(s->log_path));
    cs_strlcpy(s->health_id, "cellscope-hf", sizeof(s->health_id));
    cs_strlcpy(s->kind, "hf", sizeof(s->kind));
    if (err && err_cap) err[0] = '\0';

    if (server_healthy(s)) {
        s->running = true;
        s->external = true;
        return true;
    }
    char config[CS_PATH_MAX];
    cs_path_join(config, sizeof(config), model_path, "config.json");
    if (!cs_file_exists(config)) {
        snprintf(err, err_cap, "Hugging Face model directory is incomplete: %s", model_path);
        return false;
    }
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    const char *dev = (device && *device) ? device : "auto";
    const char *argv[] = {python,
                          script,
                          "--model",
                          model_path,
                          "--port",
                          portbuf,
                          "--device",
                          dev,
                          "--host",
                          "127.0.0.1",
                          NULL};
    FILE *f = fopen(log_path, "a");
    if (f) {
        fprintf(f, "\n=== starting Hugging Face reader server ===\n%s %s --model %s --port %d\n",
                python, script, model_path, port);
        fclose(f);
    }
    if (!proc_spawn(model_path, argv, log_path, &s->child, err, err_cap)) {
        s->running = false;
        return false;
    }
    s->running = true;
    return true;
}

bool server_wait_ready(MiniMindServer *s, int timeout_ms, volatile int *cancel)
{
    if (!s) return false;
    int waited = 0;
    while (waited < timeout_ms) {
        if (cancel && *cancel) {
            snprintf(s->error, sizeof(s->error), "Server startup cancelled");
            return false;
        }
        if (server_healthy(s)) return true;
        if (!s->external && !proc_child_alive(&s->child)) {
            char tail[4000];
            read_log_tail(s->log_path, tail, sizeof(tail));
            snprintf(s->error, sizeof(s->error),
                     "Reader server exited during startup.%s%s",
                     tail[0] ? "\n" : "", tail);
            s->running = false;
            return false;
        }
        sleep_ms(500);
        waited += 500;
    }
    char tail[4000];
    read_log_tail(s->log_path, tail, sizeof(tail));
    snprintf(s->error, sizeof(s->error), "Reader server did not become ready.%s%s",
             tail[0] ? "\n" : "", tail);
    return false;
}

void server_stop(MiniMindServer *s)
{
    if (!s) return;
    if (!s->external) proc_child_close(&s->child);
    s->running = false;
}
