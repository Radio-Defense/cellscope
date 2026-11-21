#ifndef CELLSCOPE_SETTINGS_H
#define CELLSCOPE_SETTINGS_H

#include <stdbool.h>

#include "util.h"

typedef struct {
    char path[CS_PATH_MAX];
    int version;
    char reader_backend[16];   /* "minimind" or "hf" */
    char hf_model[256];        /* Hugging Face repo id */
    char hf_model_path[CS_PATH_MAX];
    int hf_port;
    char hf_device[32];        /* auto, cpu, mps, cuda:0 ... */
    int camera_index;
    char upload_api_url[512];
    char upload_email[256];
} AppSettings;

void settings_defaults(AppSettings *s);
bool settings_init(AppSettings *s, char *err, size_t err_cap);
bool settings_load(AppSettings *s, char *err, size_t err_cap);
bool settings_save(const AppSettings *s, char *err, size_t err_cap);
bool settings_set_hf(AppSettings *s, const char *repo, const char *model_path, int port,
                     char *err, size_t err_cap);

#endif
