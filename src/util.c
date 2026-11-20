#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

void cs_strlcpy(char *dst, const char *src, size_t cap)
{
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void cs_strlcat(char *dst, const char *src, size_t cap)
{
    if (!dst || cap == 0) return;
    size_t d = strlen(dst);
    if (d >= cap - 1) return;
    cs_strlcpy(dst + d, src, cap - d);
}

void cs_trim(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

void cs_lower(char *s)
{
    if (!s) return;
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

bool cs_starts_with(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

bool cs_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t a = strlen(s), b = strlen(suffix);
    return b <= a && strcmp(s + a - b, suffix) == 0;
}

bool cs_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    if (!*needle) return true;
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n) return true;
    }
    return false;
}

void cs_path_join(char *dst, size_t cap, const char *a, const char *b)
{
    if (!dst || cap == 0) return;
    if (!a) a = "";
    if (!b) b = "";
    if (!*a) {
        cs_strlcpy(dst, b, cap);
        return;
    }
    size_t n = strlen(a);
    bool slash = (a[n - 1] == '/' || a[n - 1] == '\\');
    snprintf(dst, cap, "%s%s%s", a, slash ? "" : "/", b);
}

static int cs_mkdir_one(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

bool cs_mkdirs(const char *path)
{
    if (!path || !*path) return false;
    char tmp[CS_PATH_MAX];
    cs_strlcpy(tmp, path, sizeof(tmp));
    size_t n = strlen(tmp);
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\')) tmp[--n] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            /* Preserve Windows drive roots and UNC prefixes. */
            bool skip = (strlen(tmp) == 1);
#ifdef _WIN32
            if (strlen(tmp) == 2 && tmp[1] == ':') skip = true;
#endif
            if (!skip && cs_mkdir_one(tmp) != 0 && errno != EEXIST) return false;
            *p = saved;
        }
    }
    if (cs_mkdir_one(tmp) != 0 && errno != EEXIST) return false;
    return true;
}

bool cs_file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

bool cs_is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

bool cs_exe_dir(char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    char path[CS_PATH_MAX];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (n == 0 || n >= sizeof(path)) return false;
    char *slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash) return false;
    *slash = '\0';
    cs_strlcpy(out, path, cap);
    return true;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return false;
    char *slash = strrchr(path, '/');
    if (!slash) return false;
    *slash = '\0';
    cs_strlcpy(out, path, cap);
    return true;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return false;
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) return false;
    *slash = '\0';
    cs_strlcpy(out, path, cap);
    return true;
#endif
}

bool cs_app_data_dir(char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    const char *override = getenv("CELLSCOPE_HOME");
    if (override && *override) {
        cs_strlcpy(out, override, cap);
        return true;
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
    /* Per-user application data is the normal writable location on Windows.
     * Do not use Program Files: it requires elevation and is not user data. */
    const char *base = getenv("LOCALAPPDATA");
    if (!base) base = getenv("APPDATA");
    if (base && *base) {
        cs_path_join(out, cap, base, "CellScope");
        return true;
    }
    if (home && *home) {
        cs_path_join(out, cap, home, "AppData/Local/CellScope");
        return true;
    }
#else
    /* macOS and Linux: one hidden working directory in the home folder. */
    if (home && *home) {
        cs_path_join(out, cap, home, ".cellscope");
        return true;
    }
#endif
    return false;
}

static bool root_has_project(const char *dir)
{
    if (!dir || !*dir) return false;
    char probe[CS_PATH_MAX];
    cs_path_join(probe, sizeof(probe), dir,
                 "third_party/minimind-v/trainer/train_sft_vlm.py");
    if (cs_file_exists(probe)) return true;
    cs_path_join(probe, sizeof(probe), dir, "scripts/serve_minimind_vlm.py");
    return cs_file_exists(probe);
}

const char *cs_project_root(void)
{
    static char root[CS_PATH_MAX];
    if (root[0]) return root;
    const char *env = getenv("CELLSCOPE_ROOT");
    if (env && root_has_project(env)) {
        cs_strlcpy(root, env, sizeof(root));
        return root;
    }
    char cwd[CS_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        char walk[CS_PATH_MAX];
        cs_strlcpy(walk, cwd, sizeof(walk));
        for (int i = 0; i < 5; i++) {
            if (root_has_project(walk)) {
                cs_strlcpy(root, walk, sizeof(root));
                return root;
            }
            char *slash = strrchr(walk, '/');
            if (!slash) break;
            *slash = '\0';
        }
    }
    char exe[CS_PATH_MAX];
    if (cs_exe_dir(exe, sizeof(exe))) {
        char walk[CS_PATH_MAX];
        cs_strlcpy(walk, exe, sizeof(walk));
        for (int i = 0; i < 5; i++) {
            if (root_has_project(walk)) {
                cs_strlcpy(root, walk, sizeof(root));
                return root;
            }
            char *slash = strrchr(walk, '/');
            if (!slash) break;
            *slash = '\0';
        }
    }
#ifdef CELLSCOPE_SOURCE_ROOT
    if (root_has_project(CELLSCOPE_SOURCE_ROOT)) {
        cs_strlcpy(root, CELLSCOPE_SOURCE_ROOT, sizeof(root));
        return root;
    }
#endif
    if (getcwd(cwd, sizeof(cwd))) cs_strlcpy(root, cwd, sizeof(root));
    return root;
}

bool cs_read_file(const char *path, uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return false;
    }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    *data = buf;
    *len = got;
    return true;
}

bool cs_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

int64_t cs_now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

int64_t cs_epoch_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t / 10000ULL) - 11644473600000LL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void cs_localtime(int64_t epoch_ms, struct tm *tm)
{
    time_t t = (time_t)(epoch_ms / 1000);
#ifdef _WIN32
    localtime_s(tm, &t);
#else
    localtime_r(&t, tm);
#endif
}

void cs_format_time(int64_t epoch_ms, char *out, size_t cap)
{
    struct tm tm;
    cs_localtime(epoch_ms, &tm);
    snprintf(out, cap, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void cs_format_time_ms(int64_t epoch_ms, char *out, size_t cap)
{
    struct tm tm;
    cs_localtime(epoch_ms, &tm);
    snprintf(out, cap, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(epoch_ms % 1000));
}

void cs_format_date_stamp(int64_t epoch_ms, char *out, size_t cap)
{
    struct tm tm;
    cs_localtime(epoch_ms, &tm);
    snprintf(out, cap, "%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void cs_group_u64(uint64_t value, char *out, size_t cap)
{
    char raw[32];
    snprintf(raw, sizeof(raw), "%llu", (unsigned long long)value);
    size_t n = strlen(raw), o = 0;
    if (cap == 0) return;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (i > 0 && (n - i) % 3 == 0 && o + 1 < cap) out[o++] = ' ';
        out[o++] = raw[i];
    }
    out[o] = '\0';
}

void cs_format_hz(uint64_t hz, char *out, size_t cap)
{
    char grouped[40];
    cs_group_u64(hz, grouped, sizeof(grouped));
    snprintf(out, cap, "%s Hz", grouped);
}

void cs_format_eng(uint64_t hz, char *out, size_t cap)
{
    const char *unit = "Hz";
    double v = (double)hz;
    int decimals = 0;
    if (hz >= 1000000000ULL) {
        v = (double)hz / 1e9;
        unit = "GHz";
        decimals = 6;
    } else if (hz >= 1000000ULL) {
        v = (double)hz / 1e6;
        unit = "MHz";
        decimals = 6;
    } else if (hz >= 1000ULL) {
        v = (double)hz / 1e3;
        unit = "kHz";
        decimals = 3;
    }
    char num[64];
    snprintf(num, sizeof(num), "%.*f", decimals, v);
    if (strchr(num, '.')) {
        char *end = num + strlen(num);
        while (end > num && end[-1] == '0') *--end = '\0';
        if (end > num && end[-1] == '.') *--end = '\0';
    }
    snprintf(out, cap, "%s %s", num, unit);
}

void cs_format_mhz(uint64_t hz, char *out, size_t cap, int decimals)
{
    snprintf(out, cap, "%.*f", decimals, (double)hz / 1e6);
}

const char *cs_json_skip_ws(const char *p)
{
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *cs_json_find_key(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, needle)) != NULL) {
        const char *q = cs_json_skip_ws(p + strlen(needle));
        if (*q == ':') return cs_json_skip_ws(q + 1);
        p += strlen(needle);
    }
    return NULL;
}

static int cs_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t cs_utf8_emit(unsigned cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
}

bool cs_json_get_string(const char *json, const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const char *p = cs_json_find_key(json, key);
    if (!p || *p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 4 < cap) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; p++; break;
                case 'r': out[o++] = '\r'; p++; break;
                case 't': out[o++] = '\t'; p++; break;
                case 'b': out[o++] = '\b'; p++; break;
                case 'f': out[o++] = '\f'; p++; break;
                case '/': out[o++] = '/'; p++; break;
                case '\\': out[o++] = '\\'; p++; break;
                case '"': out[o++] = '"'; p++; break;
                case 'u': {
                    unsigned cp = 0;
                    p++;
                    for (int i = 0; i < 4 && cs_hexval(*p) >= 0; i++, p++)
                        cp = (cp << 4) | (unsigned)cs_hexval(*p);
                    o += cs_utf8_emit(cp, out + o);
                    break;
                }
                default: out[o++] = *p ? *p++ : ' '; break;
            }
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return true;
}

bool cs_json_get_bool(const char *json, const char *key, bool *out)
{
    const char *p = cs_json_find_key(json, key);
    if (!p) return false;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool cs_json_get_double(const char *json, const char *key, double *out)
{
    const char *p = cs_json_find_key(json, key);
    if (!p) return false;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

void cs_json_escape(const char *in, char *out, size_t cap)
{
    if (cap == 0) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        if (o + 8 >= cap) break;
        switch (*p) {
            case '"': out[o++] = '\\'; out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            default:
                if (*p < 0x20) {
                    o += (size_t)snprintf(out + o, cap - o, "\\u%04x", *p);
                } else {
                    out[o++] = (char)*p;
                }
                break;
        }
    }
    out[o] = '\0';
}

char *cs_base64_encode(const uint8_t *data, size_t len)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out[o++] = tab[(v >> 18) & 0x3F];
        out[o++] = tab[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? tab[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? tab[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}
