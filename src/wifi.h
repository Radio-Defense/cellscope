#ifndef CELLSCOPE_WIFI_H
#define CELLSCOPE_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    int rssi;
} WifiAp;

/* Stable per-install anonymisation of an SSID. The salt is random and stored
 * outside the capture CSV, so the visible numbers do not reveal the SSID. */
uint32_t wifi_anonymize(const char *ssid, uint64_t salt);

/* Parses `adb shell cmd wifi list-scan-results` output. Returns unique SSIDs
 * with their strongest RSSI, sorted strongest first. */
int wifi_parse_scan(const char *text, uint64_t salt, WifiAp *out, int max);
void wifi_format(const WifiAp *aps, int count, char *out, size_t cap);

#endif
