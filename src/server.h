#ifndef CELLSCOPE_SERVER_H
#define CELLSCOPE_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "proc.h"
#include "util.h"

typedef struct {
    ProcChild child;
    bool running;
    bool external;
    int port;
    char base_url[128];
    char log_path[CS_PATH_MAX];
    char weight[64];
    char error[1024];
} MiniMindServer;

bool server_start(MiniMindServer *s, const char *python, const char *script,
                  const char *minimind, const char *save_dir, const char *weight, int port,
                  const char *log_path, char *err, size_t err_cap);
bool server_wait_ready(MiniMindServer *s, int timeout_ms, volatile int *cancel);
bool server_healthy(MiniMindServer *s);
void server_stop(MiniMindServer *s);

#endif
