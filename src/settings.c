#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void settings_defaults(AppSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->version = 1;
    cs_strlcpy(s->reader_backend, "minimind", sizeof(s->reader_backend));
    s->hf_port = 8091;
    cs_strlcpy(s->hf_device, "auto", sizeof(s->hf_device));
}

bool settings_init(AppSettings *s, char *err, size_t err_cap)
{
    settings_defaults(s);
    const char *override = getenv("CELLSCOPE_SETTINGS");
    if (override && *override) {
        cs_strlcpy(s->path, override, sizeof(s->path));
        return true;
    }
    char dir[CS_PATH_MAX];
    if (!cs_app_data_dir(dir, sizeof(dir))) {
        snprintf(err, err_cap, "Cannot determine the CellScope settings directory");
        return false;
    }
    cs_mkdirs(dir);
    cs_path_join(s->path, sizeof(s->path), dir, "settings.json");
    return true;
}

static int json_int(const char *json, const char *key, int fallback)
{
    double v = 0;
    if (!cs_json_get_double(json, key, &v)) return fallback;
    if (v < -2147483647.0 || v > 2147483647.0) return fallback;
    return (int)(v + (v >= 0 ? 0.5 : -0.5));
}

bool settings_load(AppSettings *s, char *err, size_t err_cap)
{
    settings_defaults(s);
    if (!settings_init(s, err, err_cap)) return false;
    if (!cs_file_exists(s->path)) {
        return settings_save(s, err, err_cap);
    }
    uint8_t *data = NULL;
    size_t len = 0;
    if (!cs_read_file(s->path, &data, &len)) {
        snprintf(err, err_cap, "Could not read %s", s->path);
        return false;
    }
    char backend[16] = {0};
    char repo[256] = {0}, path[CS_PATH_MAX] = {0}, device[32] = {0};
    if (cs_json_get_string((const char *)data, "reader_backend", backend, sizeof(backend)) &&
        backend[0]) {
        cs_strlcpy(s->reader_backend, backend, sizeof(s->reader_backend));
    }
    if (cs_json_get_string((const char *)data, "hf_model", repo, sizeof(repo)))
        cs_strlcpy(s->hf_model, repo, sizeof(s->hf_model));
    if (cs_json_get_string((const char *)data, "hf_model_path", path, sizeof(path)))
        cs_strlcpy(s->hf_model_path, path, sizeof(s->hf_model_path));
    if (cs_json_get_string((const char *)data, "hf_device", device, sizeof(device)) &&
        device[0])
        cs_strlcpy(s->hf_device, device, sizeof(s->hf_device));
    s->hf_port = json_int((const char *)data, "hf_port", s->hf_port);
    s->camera_index = json_int((const char *)data, "camera_index", s->camera_index);
    s->version = json_int((const char *)data, "version", 1);
    free(data);

    if (strcmp(s->reader_backend, "minimind") != 0 &&
        strcmp(s->reader_backend, "hf") != 0)
        cs_strlcpy(s->reader_backend, "minimind", sizeof(s->reader_backend));
    if (s->hf_port < 1024 || s->hf_port > 65535) s->hf_port = 8091;
    if (s->camera_index < 0 || s->camera_index > 16) s->camera_index = 0;
    if (!s->hf_device[0]) cs_strlcpy(s->hf_device, "auto", sizeof(s->hf_device));
    return true;
}

bool settings_save(const AppSettings *s, char *err, size_t err_cap)
{
    if (!s->path[0]) {
        snprintf(err, err_cap, "No settings path configured");
        return false;
    }
    char esc_repo[2048], esc_path[CS_PATH_MAX * 2], esc_device[256];
    cs_json_escape(s->hf_model, esc_repo, sizeof(esc_repo));
    cs_json_escape(s->hf_model_path, esc_path, sizeof(esc_path));
    cs_json_escape(s->hf_device, esc_device, sizeof(esc_device));
    char body[CS_PATH_MAX * 3];
    int n = snprintf(body, sizeof(body),
                     "{\n"
                     "  \"version\": %d,\n"
                     "  \"reader_backend\": \"%s\",\n"
                     "  \"hf_model\": \"%s\",\n"
                     "  \"hf_model_path\": \"%s\",\n"
                     "  \"hf_port\": %d,\n"
                     "  \"hf_device\": \"%s\",\n"
                     "  \"camera_index\": %d\n"
                     "}\n",
                     s->version, s->reader_backend, esc_repo, esc_path, s->hf_port,
                     esc_device, s->camera_index);
    if (n <= 0 || (size_t)n >= sizeof(body)) {
        snprintf(err, err_cap, "Settings JSON is too large");
        return false;
    }
    char tmp[CS_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->path);
    if (!cs_write_file(tmp, body, strlen(body))) {
        snprintf(err, err_cap, "Could not write %s", tmp);
        return false;
    }
#ifdef _WIN32
    remove(s->path);
#endif
    if (rename(tmp, s->path) != 0) {
        remove(tmp);
        snprintf(err, err_cap, "Could not replace %s", s->path);
        return false;
    }
    return true;
}

bool settings_set_hf(AppSettings *s, const char *repo, const char *model_path, int port,
                     char *err, size_t err_cap)
{
    cs_strlcpy(s->reader_backend, "hf", sizeof(s->reader_backend));
    cs_strlcpy(s->hf_model, repo, sizeof(s->hf_model));
    cs_strlcpy(s->hf_model_path, model_path, sizeof(s->hf_model_path));
    if (port < 1024 || port > 65535) port = 8091;
    s->hf_port = port;
    return settings_save(s, err, err_cap);
}
