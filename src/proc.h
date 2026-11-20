#ifndef CELLSCOPE_PROC_H
#define CELLSCOPE_PROC_H

#include <stdbool.h>
#include <stddef.h>

/* Run a program without a shell, capture stdout+stderr, and enforce a hard
 * timeout. argv must be NULL-terminated. Returns true when the process was
 * started and completed (regardless of exit code). */
bool proc_run(const char *const argv[], char *out, size_t out_cap, int timeout_ms,
              int *exit_code, bool *timed_out, char *err, size_t err_cap);

/* Same, with an explicit working directory (NULL = inherit). */
bool proc_run_in(const char *cwd, const char *const argv[], char *out, size_t out_cap,
                 int timeout_ms, int *exit_code, bool *timed_out, char *err, size_t err_cap);

/* Run with inherited stdio and stream output to the terminal. Used for long
 * training jobs; returns true when the process was started, with exit code. */
bool proc_run_stream(const char *cwd, const char *const argv[], int *exit_code);

/* Same as proc_run_in, but polls *cancel and kills the child when it becomes
 * non-zero. */
bool proc_run_cancellable(const char *cwd, const char *const argv[], char *out,
                          size_t out_cap, int timeout_ms, volatile int *cancel,
                          int *exit_code, bool *timed_out, char *err, size_t err_cap);

/* Fire-and-forget child with stdout/stderr appended to log_path. */
typedef struct {
    void *handle;
    long pid;
    bool alive;
} ProcChild;

bool proc_spawn(const char *cwd, const char *const argv[], const char *log_path,
                ProcChild *child, char *err, size_t err_cap);
bool proc_child_alive(ProcChild *child);
void proc_child_stop(ProcChild *child, int grace_ms);
void proc_child_close(ProcChild *child);

/* Find an executable in PATH (and, on Windows, PATHEXT). */
bool proc_find_in_path(const char *name, char *out, size_t cap);

#endif
