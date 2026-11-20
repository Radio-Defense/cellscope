#include "adb.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proc.h"
#include "util.h"

#ifdef _WIN32
#define strtok_r strtok_s
#endif

static void set_hint(AdbStatus *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->hint, sizeof(st->hint), fmt, ap);
    va_end(ap);
}

void adb_status_init(AdbStatus *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->state = ADB_STATE_UNKNOWN;
}

static bool check_path(const char *p, char *out, size_t cap)
{
    if (!p || !*p) return false;
    if (!cs_file_exists(p)) return false;
    cs_strlcpy(out, p, cap);
    return true;
}

static void expand_tilde(const char *in, char *out, size_t cap)
{
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        const char *home = getenv("HOME");
        if (!home) {
#ifdef _WIN32
            home = getenv("USERPROFILE");
#endif
        }
        if (home) {
            snprintf(out, cap, "%s%s", home, in + 1);
            return;
        }
    }
    cs_strlcpy(out, in, cap);
}

bool adb_find(const char *override_path, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    char tmp[CS_PATH_MAX];
    if (override_path && *override_path) {
        expand_tilde(override_path, tmp, sizeof(tmp));
        return check_path(tmp, out, cap);
    }
    const char *env = getenv("ADB");
    if (env && *env) {
        expand_tilde(env, tmp, sizeof(tmp));
        if (check_path(tmp, out, cap)) return true;
    }
    if (proc_find_in_path("adb", out, cap)) return true;

    const char *candidates[] = {
#ifdef _WIN32
        "C:/platform-tools/adb.exe",
        "C:/Android/platform-tools/adb.exe",
#else
        "/opt/homebrew/bin/adb",
        "/usr/local/bin/adb",
        "/usr/bin/adb",
        "~/Library/Android/sdk/platform-tools/adb",
        "~/Android/Sdk/platform-tools/adb",
        "~/Android/sdk/platform-tools/adb",
#endif
        NULL,
    };
    for (int i = 0; candidates[i]; i++) {
        expand_tilde(candidates[i], tmp, sizeof(tmp));
        if (check_path(tmp, out, cap)) return true;
    }
#ifdef _WIN32
    const char *local = getenv("LOCALAPPDATA");
    if (local) {
        snprintf(tmp, sizeof(tmp), "%s/Android/Sdk/platform-tools/adb.exe", local);
        if (check_path(tmp, out, cap)) return true;
    }
#endif
    return false;
}

void adb_strip_crlf(char *s)
{
    if (!s) return;
    for (char *p = s; *p; p++)
        if (*p == '\r') *p = ' ';
    cs_trim(s);
}

static bool line_is(const char *line, const char *word)
{
    return strncmp(line, word, strlen(word)) == 0;
}

static void sanitize_model(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p == '_') *p = ' ';
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    }
    cs_trim(s);
}

bool adb_check(const char *adb_path, AdbStatus *st)
{
    if (!st) return false;
    int64_t now = cs_now_ms();
    st->last_check_ms = now;
    st->attempted = true;
    st->busy = true;
    st->serial[0] = '\0';
    st->model[0] = '\0';
    st->device_count = 0;
    st->raw[0] = '\0';

    if (!adb_path || !*adb_path) {
        st->state = ADB_STATE_NO_TOOL;
        snprintf(st->detail, sizeof(st->detail), "adb was not found on this computer.");
#ifdef _WIN32
        set_hint(st,
                 "Install Android platform-tools, then add the folder to PATH:\n"
                 "  winget install Google.PlatformTools\n"
                 "or download platform-tools from developer.android.com and set ADB to "
                 "the full path of adb.exe.");
#elif defined(__APPLE__)
        set_hint(st,
                 "Install Android platform-tools:\n"
                 "  brew install android-platform-tools\n"
                 "or set ADB to the full path, e.g.\n"
                 "  export ADB=$HOME/Library/Android/sdk/platform-tools/adb");
#else
        set_hint(st,
                 "Install Android platform-tools (package: android-tools-adb or "
                 "android-sdk-platform-tools), then run `adb version` in a terminal.");
#endif
        st->busy = false;
        return false;
    }

    const char *argv[] = {adb_path, "devices", "-l", NULL};
    char out[16384];
    char err[512];
    int code = -1;
    bool timed_out = false;
    bool ran = proc_run(argv, out, sizeof(out), 5000, &code, &timed_out, err, sizeof(err));
    adb_strip_crlf(out);
    if (!ran) {
        st->state = ADB_STATE_ERROR;
        snprintf(st->detail, sizeof(st->detail), "Could not execute adb.");
        cs_strlcpy(st->hint, err[0] ? err : "Check that adb is executable.", sizeof(st->hint));
        st->busy = false;
        return false;
    }
    if (timed_out) {
        st->state = ADB_STATE_ERROR;
        snprintf(st->detail, sizeof(st->detail), "adb did not respond within 5 seconds.");
        cs_strlcpy(st->hint,
                   "Try `adb kill-server` in a terminal, unplug the phone, then plug it back in.",
                   sizeof(st->hint));
        st->busy = false;
        return false;
    }
    if (code != 0 && !strstr(out, "List of devices")) {
        st->state = ADB_STATE_ERROR;
        snprintf(st->detail, sizeof(st->detail), "adb exited with code %d.", code);
        cs_strlcpy(st->hint, out[0] ? out : (err[0] ? err : "No output from adb."),
                   sizeof(st->hint));
        st->busy = false;
        return false;
    }

    cs_strlcpy(st->raw, out, sizeof(st->raw));

    char *save = NULL;
    char *line = strtok_r(out, "\n", &save);
    bool in_list = false;
    char first_serial[96] = {0};
    AdbState first_state = ADB_STATE_NO_DEVICE;
    int device_count = 0;

    while (line) {
        adb_strip_crlf(line);
        if (!*line) {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        if (!in_list) {
            if (strstr(line, "List of devices attached")) in_list = true;
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        if (line_is(line, "* daemon")) {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        if (line_is(line, "adb:")) {
            cs_strlcpy(st->detail, line, sizeof(st->detail));
            line = strtok_r(NULL, "\n", &save);
            continue;
        }

        char serial[96] = {0};
        char state[64] = {0};
        if (sscanf(line, "%95s %63s", serial, state) != 2) {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        device_count++;
        if (strcmp(state, "device") == 0) {
            if (first_state != ADB_STATE_CONNECTED) {
                cs_strlcpy(first_serial, serial, sizeof(first_serial));
                first_state = ADB_STATE_CONNECTED;
                const char *model = strstr(line, "model:");
                if (model) {
                    model += 6;
                    size_t n = strcspn(model, " \t\r\n");
                    char m[96];
                    n = CS_MIN(n, sizeof(m) - 1);
                    memcpy(m, model, n);
                    m[n] = '\0';
                    sanitize_model(m);
                    cs_strlcpy(st->model, m, sizeof(st->model));
                }
            }
        } else if (strcmp(state, "unauthorized") == 0 && first_state == ADB_STATE_NO_DEVICE) {
            if (!first_serial[0]) cs_strlcpy(first_serial, serial, sizeof(first_serial));
            first_state = ADB_STATE_UNAUTHORIZED;
        } else if (strcmp(state, "offline") == 0 && first_state == ADB_STATE_NO_DEVICE) {
            if (!first_serial[0]) cs_strlcpy(first_serial, serial, sizeof(first_serial));
            first_state = ADB_STATE_OFFLINE;
        } else if (strstr(line, "no permissions") && first_state == ADB_STATE_NO_DEVICE) {
            first_state = ADB_STATE_NO_PERMISSION;
        }
        line = strtok_r(NULL, "\n", &save);
    }

    st->device_count = device_count;
    if (first_state == ADB_STATE_CONNECTED) {
        st->state = ADB_STATE_CONNECTED;
        cs_strlcpy(st->serial, first_serial, sizeof(st->serial));
        if (device_count > 1) {
            snprintf(st->detail, sizeof(st->detail),
                     "%d devices attached; using %s. Disconnect other phones to avoid "
                     "reading the wrong one.",
                     device_count, first_serial);
        } else {
            snprintf(st->detail, sizeof(st->detail), "Connected to %s.",
                     st->model[0] ? st->model : first_serial);
        }
        cs_strlcpy(st->hint,
                   "Keep the phone unlocked. Some OEM builds hide NR fields unless the "
                   "dialer test menu is enabled; the app shows everything ADB exposes.",
                   sizeof(st->hint));
        st->busy = false;
        return true;
    }
    if (first_state == ADB_STATE_UNAUTHORIZED) {
        st->state = ADB_STATE_UNAUTHORIZED;
        cs_strlcpy(st->serial, first_serial, sizeof(st->serial));
        snprintf(st->detail, sizeof(st->detail),
                 "The phone %s is connected but has not authorized this computer.",
                 first_serial);
        set_hint(st,
                 "1. Unlock the phone and look for \"Allow USB debugging?\".\n"
                 "2. Tap Allow and tick \"Always allow from this computer\".\n"
                 "3. If no prompt appears, revoke authorizations in Developer options, "
                 "then replug the cable.");
        st->busy = false;
        return false;
    }
    if (first_state == ADB_STATE_OFFLINE) {
        st->state = ADB_STATE_OFFLINE;
        cs_strlcpy(st->serial, first_serial, sizeof(st->serial));
        snprintf(st->detail, sizeof(st->detail), "The phone %s is offline.", first_serial);
        set_hint(st,
                 "Unplug and replug the USB cable. If it stays offline run:\n"
                 "  adb reconnect offline\n"
                 "then check `adb devices` again.");
        st->busy = false;
        return false;
    }
    if (first_state == ADB_STATE_NO_PERMISSION) {
        st->state = ADB_STATE_NO_PERMISSION;
        snprintf(st->detail, sizeof(st->detail),
                 "USB access is blocked by the operating system.");
        set_hint(st,
                 "On Linux, install udev rules for Android and add your user to plugdev:\n"
                 "  sudo usermod -aG plugdev $USER\n"
                 "then log out and back in. On Windows, install the OEM USB driver.");
        st->busy = false;
        return false;
    }

    st->state = ADB_STATE_NO_DEVICE;
    if (device_count == 0) {
        snprintf(st->detail, sizeof(st->detail), "No Android device is visible to adb.");
    } else {
        snprintf(st->detail, sizeof(st->detail),
                 "adb sees %d device(s), but none is ready.", device_count);
    }
    set_hint(st,
             "1. Use a USB data cable (some charging cables have no data wires).\n"
             "2. Unlock the phone and set USB mode to File transfer / MTP.\n"
             "3. Enable Developer options and USB debugging, then accept the RSA prompt.\n"
             "4. Try another USB port, or use wireless debugging.");
    st->busy = false;
    return false;
}

bool adb_poll_dumpsys(const char *adb_path, const char *serial, char *out, size_t cap,
                      char *err, size_t err_cap)
{
    if (out && cap) out[0] = '\0';
    if (err && err_cap) err[0] = '\0';
    if (!adb_path || !*adb_path || !serial || !*serial) return false;

    const char *argv[] = {adb_path, "-s", serial, "shell", "dumpsys", "telephony.registry",
                          NULL};
    int code = -1;
    bool timed_out = false;
    bool ran = proc_run(argv, out, cap, 6000, &code, &timed_out, err, err_cap);
    if (!ran) return false;
    if (timed_out) {
        snprintf(err, err_cap, "dumpsys timed out");
        return false;
    }
    if (code != 0) {
        if (!err[0]) snprintf(err, err_cap, "adb shell exited with code %d", code);
        if (!out[0]) return false;
    }
    if (strstr(out, "error:") || strstr(out, "Exception")) {
        if (!err[0]) {
            const char *p = strstr(out, "error:");
            if (!p) p = strstr(out, "Exception");
            snprintf(err, err_cap, "%s", p ? p : "adb reported an error");
        }
        return false;
    }
    return out[0] != '\0';
}
