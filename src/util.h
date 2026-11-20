#ifndef CELLSCOPE_UTIL_H
#define CELLSCOPE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CS_PATH_MAX
#define CS_PATH_MAX 4096
#endif

#define CS_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define CS_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CS_MAX(a, b) ((a) > (b) ? (a) : (b))

void cs_strlcpy(char *dst, const char *src, size_t cap);
void cs_strlcat(char *dst, const char *src, size_t cap);
void cs_trim(char *s);
void cs_lower(char *s);
bool cs_starts_with(const char *s, const char *prefix);
bool cs_ends_with(const char *s, const char *suffix);
bool cs_contains_ci(const char *haystack, const char *needle);
void cs_path_join(char *dst, size_t cap, const char *a, const char *b);
bool cs_mkdirs(const char *path);
bool cs_file_exists(const char *path);
bool cs_is_dir(const char *path);
bool cs_exe_dir(char *out, size_t cap);
bool cs_app_data_dir(char *out, size_t cap);
const char *cs_project_root(void);
bool cs_read_file(const char *path, uint8_t **data, size_t *len);
bool cs_write_file(const char *path, const void *data, size_t len);

int64_t cs_now_ms(void);        /* monotonic-ish wall clock, milliseconds */
int64_t cs_epoch_ms(void);
void cs_format_time(int64_t epoch_ms, char *out, size_t cap);     /* 17:41:32 */
void cs_format_time_ms(int64_t epoch_ms, char *out, size_t cap);  /* 17:41:32.204 */
void cs_format_date_stamp(int64_t epoch_ms, char *out, size_t cap); /* 20261120 */

/* Group an integer with thin spaces: 2160150000 -> "2 160 150 000". */
void cs_group_u64(uint64_t value, char *out, size_t cap);
void cs_format_hz(uint64_t hz, char *out, size_t cap);
void cs_format_eng(uint64_t hz, char *out, size_t cap);
void cs_format_mhz(uint64_t hz, char *out, size_t cap, int decimals);

/* Minimal JSON helpers for talking to local model servers. They return false
 * if the key is missing or the value has the wrong type. */
bool cs_json_get_string(const char *json, const char *key, char *out, size_t cap);
bool cs_json_get_bool(const char *json, const char *key, bool *out);
bool cs_json_get_double(const char *json, const char *key, double *out);
void cs_json_escape(const char *in, char *out, size_t cap);
const char *cs_json_skip_ws(const char *p);

/* Base64 for image payloads. Returns malloc'ed NUL-terminated text. */
char *cs_base64_encode(const uint8_t *data, size_t len);

#endif
