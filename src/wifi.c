#include "wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

uint32_t wifi_anonymize(const char *ssid, uint64_t salt)
{
    uint64_t h = 1469598103934665603ULL ^ salt;
    for (const unsigned char *p = (const unsigned char *)(ssid ? ssid : ""); *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    uint32_t v = (uint32_t)h;
    return v ? v : 1u;
}

static int cmp_ap(const void *a, const void *b)
{
    const WifiAp *x = a, *y = b;
    if (x->rssi != y->rssi) return y->rssi - x->rssi;
    if (x->id < y->id) return -1;
    if (x->id > y->id) return 1;
    return 0;
}

static void trim_ssid(char *s)
{
    cs_trim(s);
    size_t n = strlen(s);
    /* The shell table appends capability flags as a space-separated [...]
     * group. Cut at the first such group, not the last bracket inside it. */
    char *bracket = strstr(s, " [");
    if (bracket == NULL) bracket = strchr(s, '[');
    if (bracket && bracket > s) {
        *bracket = '\0';
        while (bracket > s && isspace((unsigned char)bracket[-1])) *--bracket = '\0';
        n = strlen(s);
    }
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

int wifi_parse_scan(const char *text, uint64_t salt, WifiAp *out, int max)
{
    if (!text || !out || max <= 0) return 0;
    int count = 0;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        char bssid[64];
        int freq = 0, rssi = 0;
        double age = 0;
        int consumed = 0;
        int n = sscanf(l, "%63s %d %d %lf %n", bssid, &freq, &rssi, &age, &consumed);
        if (n >= 4 && consumed > 0 && rssi <= 0 && rssi >= -127) {
            char ssid[256];
            cs_strlcpy(ssid, l + consumed, sizeof(ssid));
            trim_ssid(ssid);
            if (ssid[0] && !cs_contains_ci(ssid, "<unknown ssid>") &&
                !cs_contains_ci(ssid, "<unknown>")) {
                uint32_t id = wifi_anonymize(ssid, salt);
                int found = -1;
                for (int i = 0; i < count; i++) {
                    if (out[i].id == id) {
                        found = i;
                        break;
                    }
                }
                if (found >= 0) {
                    if (rssi > out[found].rssi) out[found].rssi = rssi;
                } else if (count < max) {
                    out[count].id = id;
                    out[count].rssi = rssi;
                    count++;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    qsort(out, (size_t)count, sizeof(*out), cmp_ap);
    return count;
}

void wifi_format(const WifiAp *aps, int count, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        int n = snprintf(out + o, cap - o, "%s%u:%d", i ? ";" : "", aps[i].id, aps[i].rssi);
        if (n < 0 || (size_t)n >= cap - o) break;
        o += (size_t)n;
    }
}
