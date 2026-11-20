#ifndef CELLSCOPE_ADB_H
#define CELLSCOPE_ADB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

#define ADB_DUMPSYS_MAX (1024 * 1024)

typedef enum {
    ADB_STATE_UNKNOWN = 0,
    ADB_STATE_NO_TOOL,
    ADB_STATE_NO_DEVICE,
    ADB_STATE_UNAUTHORIZED,
    ADB_STATE_OFFLINE,
    ADB_STATE_NO_PERMISSION,
    ADB_STATE_CONNECTED,
    ADB_STATE_ERROR
} AdbState;

typedef struct {
    AdbState state;
    char adb_path[CS_PATH_MAX];
    char serial[96];
    char model[96];
    char detail[512];
    char hint[1024];
    char raw[1024];
    int device_count;
    int64_t last_check_ms;
    bool attempted;
    bool busy;
} AdbStatus;

void adb_status_init(AdbStatus *st);
bool adb_find(const char *override_path, char *out, size_t cap);

/* Runs `adb devices -l` and fills st. Returns true only for a usable device. */
bool adb_check(const char *adb_path, AdbStatus *st);

/* Runs dumpsys and returns the captured text (out is NUL-terminated). */
bool adb_poll_dumpsys(const char *adb_path, const char *serial, char *out, size_t cap,
                      char *err, size_t err_cap);

/* `cmd wifi list-scan-results`; requires Android 11+ and Wi-Fi/location enabled. */
bool adb_wifi_scan(const char *adb_path, const char *serial, char *out, size_t cap,
                   char *err, size_t err_cap);
bool adb_wifi_start_scan(const char *adb_path, const char *serial);

void adb_strip_crlf(char *s);

#endif
