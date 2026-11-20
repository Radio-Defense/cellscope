#ifndef CELLSCOPE_RUNTIME_H
#define CELLSCOPE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "util.h"

#define CELLSCOPE_RUNTIME_VERSION "1"

typedef enum {
    RT_STAGE_IDLE = 0,
    RT_STAGE_PREPARE,
    RT_STAGE_UV,
    RT_STAGE_PYTHON,
    RT_STAGE_VENV,
    RT_STAGE_PACKAGES,
    RT_STAGE_MODELS,
    RT_STAGE_READY,
    RT_STAGE_ERROR
} RuntimeStage;

typedef struct {
    char app_data[CS_PATH_MAX];
    char dir[CS_PATH_MAX];
    char project_root[CS_PATH_MAX];
    char minimind[CS_PATH_MAX];
    char scripts[CS_PATH_MAX];
    char training[CS_PATH_MAX];
    char uv[CS_PATH_MAX];
    char python_install[CS_PATH_MAX];
    char venv[CS_PATH_MAX];
    char python[CS_PATH_MAX];
    char cache[CS_PATH_MAX];
    char marker[CS_PATH_MAX];
    char bootstrap_log[CS_PATH_MAX];
    int port;
    volatile int cancel;

    RuntimeStage stage;
    int percent;
    char message[512];
    char detail[1024];
    char error[1024];
    bool ready;
    bool models_ready;
    bool using_custom_weight;
} PythonRuntime;

typedef void (*RuntimeProgressFn)(void *ctx, const PythonRuntime *rt);

bool runtime_init(PythonRuntime *rt, const char *project_root, char *err, size_t err_cap);
bool runtime_ensure(PythonRuntime *rt, RuntimeProgressFn cb, void *ctx, bool with_models);
bool runtime_is_ready(const PythonRuntime *rt);
bool runtime_custom_checkpoint(const PythonRuntime *rt, char *out, size_t cap);
void runtime_cancel(PythonRuntime *rt);

#endif
