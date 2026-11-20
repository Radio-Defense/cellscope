#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cellular.h"
#include "meter.h"
#include "quality.h"
#include "telephony.h"
#include "util.h"
#include "wifi.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                                    \
    do {                                                                                    \
        g_checks++;                                                                         \
        if (!(cond)) {                                                                      \
            g_failures++;                                                                   \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                            \
            fprintf(stderr, __VA_ARGS__);                                                   \
            fprintf(stderr, "\n");                                                          \
        }                                                                                   \
    } while (0)

static void test_cellular(void)
{
    uint64_t hz = 0;
    CHECK(cs_nr_arfcn_to_hz(432030, &hz), "NR ARFCN 432030 should convert");
    CHECK(llabs((long long)hz - 2160150000LL) <= 1,
          "NR 432030 = %llu Hz (want 2160150000)", (unsigned long long)hz);
    CHECK(cs_nr_band_from_arfcn(432030) == 1, "NR 432030 should be n1");

    uint64_t ul = 0;
    CHECK(cs_nr_arfcn_to_ul_hz(432030, 1, &ul), "NR UL conversion");
    CHECK(llabs((long long)ul - 1970150000LL) <= 1,
          "NR n1 UL = %llu Hz (want 1970150000)", (unsigned long long)ul);

    uint64_t dl = 0;
    ul = 0;
    CHECK(cs_lte_earfcn_to_hz(3600, 8, &dl, &ul), "LTE 3600 band 8");
    CHECK(dl == 940000000ULL, "LTE B8 DL = %llu", (unsigned long long)dl);
    CHECK(ul == 895000000ULL, "LTE B8 UL = %llu", (unsigned long long)ul);
    CHECK(cs_lte_band_from_earfcn(3600) == 8, "EARFCN 3600 should be B8");

    char a[16], b[16];
    cs_plmn_split("20408", a, sizeof(a), b, sizeof(b));
    CHECK(strcmp(a, "204") == 0 && strcmp(b, "08") == 0, "PLMN split: %s/%s", a, b);
    char country[32];
    cs_mcc_country("204", country, sizeof(country));
    CHECK(strcmp(country, "Netherlands") == 0, "MCC 204 = %s", country);
    char op[32];
    cs_plmn_operator("20408", op, sizeof(op));
    CHECK(strcmp(op, "KPN") == 0, "PLMN 20408 = %s", op);
}

static const char *k_dumpsys =
    "last known state:\n"
    "  mServiceState=voiceNetworkType=13 dataNetworkType=13 "
    "mCellBandwidths=[10, 20, 20] mOperatorAlphaLong=KPN "
    "isUsingCarrierAggregation=true nrState=CONNECTED\n"
    "  mCellIdentity=CellIdentityLte:{ mCi = 8013078 mPci = 389 mTac = 0 "
    "mEarfcn = 3600 mBands = [8] mBandwidth = 20 mNrArfcn = 432030 "
    "mMcc = 204 mMnc = 08 mAlphaLong = KPN }\n"
    "  mCellIdentityNr=CellIdentityNr:{ mPci = 671 mTac = 0 mNrArfcn = 432030 "
    "mBands = [1] mNci = 123456 mMcc = 204 mMnc = 08 mAlphaLong = KPN }\n"
    "  mSignalStrength=SignalStrength:{ mGsm=CellSignalStrengthGsm:{ rssi=-65 } "
    "mLte=CellSignalStrengthLte:{ rssi=-60 rsrp=-90 ta=11 } "
    "mNr=CellSignalStrengthNr:{ ssRsrp=-89 timingAdvance = 11 } }\n";

static void test_telephony(void)
{
    TelephonyRaw raw;
    CHECK(telephony_parse(k_dumpsys, &raw), "telephony parse: %s", raw.diag);
    CHECK(raw.has_lte && raw.has_nr && raw.nr_serving, "LTE + serving NR");
    CHECK(raw.lte_ci == 8013078, "LTE CI = %d", raw.lte_ci);
    CHECK(raw.lte_pci == 389 && raw.nr_pci == 671, "PCI %d/%d", raw.nr_pci, raw.lte_pci);
    CHECK(raw.lte_earfcn == 3600 && raw.nr_arfcn == 432030, "ARFCN");
    CHECK(raw.ta == 11, "TA = %d", raw.ta);
    CHECK(raw.bw_count == 3, "BW count = %d", raw.bw_count);
    CHECK(raw.ca, "CA flag");

    CellSample s;
    telephony_to_sample(&raw, &s);
    CHECK(strcmp(s.plmn, "20408") == 0, "PLMN %s", s.plmn);
    CHECK(strcmp(s.operator_name, "KPN") == 0, "operator %s", s.operator_name);
    CHECK(strcmp(s.country, "Netherlands") == 0, "country %s", s.country);
    CHECK(strcmp(s.rat, "5G NSA") == 0, "RAT %s", s.rat);
    CHECK(strcmp(s.enb, "31301") == 0, "eNB %s", s.enb);
    CHECK(strcmp(s.cid, "22") == 0, "CID %s", s.cid);
    CHECK(strcmp(s.pci, "671 / 389") == 0, "PCI %s", s.pci);
    CHECK(strcmp(s.arfcn, "432030 / 3600") == 0, "ARFCN %s", s.arfcn);
    CHECK(strcmp(s.band, "N1 / L8") == 0, "band %s", s.band);
    CHECK(strcmp(s.bw, "10 / 20 / 20") == 0, "BW %s", s.bw);
    CHECK(strcmp(s.f_dl, "2160.2 / 940.0") == 0, "DL %s", s.f_dl);
    CHECK(strcmp(s.f_ul, "1970.2 / 895.0") == 0, "UL %s", s.f_ul);
    CHECK(strcmp(s.tac, "0") == 0, "TAC %s", s.tac);
    CHECK(strcmp(s.ta, "11") == 0, "TA %s", s.ta);
    CHECK(s.ca, "sample CA");
}

static void test_meter(void)
{
    MeterReading r;
    CHECK(meter_parse_text("2.160150 GHz", "MHz", &r), "parse GHz");
    CHECK(llabs((long long)r.hz - 2160150000LL) < 10, "GHz hz %.0f", r.hz);
    CHECK(strcmp(r.unit, "GHz") == 0, "unit %s", r.unit);

    CHECK(meter_parse_text("940.0 MHz", "GHz", &r), "parse MHz");
    CHECK(llabs((long long)r.hz - 940000000LL) < 10, "MHz hz %.0f", r.hz);

    CHECK(meter_parse_text("1,234.567 kHz", "Hz", &r), "parse kHz");
    CHECK(llabs((long long)r.hz - 1234567LL) < 10, "kHz hz %.0f", r.hz);

    CHECK(meter_parse_text("2160,150 MHz", "Hz", &r), "parse EU decimal comma");
    CHECK(llabs((long long)r.hz - 2160150000LL) < 10, "comma hz %.0f", r.hz);

    CHECK(meter_parse_text("2.160,150 MHz", "Hz", &r), "parse EU thousands+decimal");
    CHECK(llabs((long long)r.hz - 2160150000LL) < 10, "EU hz %.0f", r.hz);

    CHECK(meter_parse_text("2160.15", "MHz", &r), "parse default MHz");
    CHECK(llabs((long long)r.hz - 2160150000LL) < 10, "default hz %.0f", r.hz);

    CHECK(!meter_parse_text("no reading here", "MHz", &r), "no digits");
    CHECK(!meter_parse_text("", "MHz", &r), "empty");

    const char *json =
        "```json\n{\"visible\":true,\"reading\":\"123.456789\",\"unit\":\"MHz\","
        "\"confidence\":0.93}\n```";
    CHECK(meter_parse_model_output(json, "MHz", &r), "parse JSON");
    CHECK(llabs((long long)r.hz - 123456789LL) < 10, "JSON hz %.0f", r.hz);
    CHECK(fabs(r.confidence - 0.93) < 0.001, "JSON confidence %.2f", r.confidence);
    CHECK(meter_parse_model_output(
              "{\"visible\":true,\"reading\":\"\",\"unit\":\"\",\"confidence\":0.2}",
              "MHz", &r),
          "visible screen with unreadable digits");
    CHECK(r.visible && r.hz == 0, "unreadable screen keeps visibility");

    CHECK(meter_parse_model_output("{\"visible\":false,\"confidence\":0.1}", "MHz", &r),
          "parse invisible JSON");
    CHECK(!r.visible, "invisible flag");

    CHECK(meter_parse_model_output(
              "{\"response\":\"{\\\"visible\\\":true,\\\"reading\\\":\\\"940.0\\\","
              "\\\"unit\\\":\\\"MHz\\\",\\\"confidence\\\":0.8}\"}",
              "MHz", &r),
          "unwrap Ollama response");
    CHECK(llabs((long long)r.hz - 940000000LL) < 10, "Ollama hz %.0f", r.hz);

    MeterTracker t;
    meter_tracker_init(&t);
    CHECK(meter_parse_text("2.160150 GHz", "MHz", &r), "tracker reading");
    CHECK(meter_tracker_update(&t, &r, 1000) == METER_TRACK_NOOP, "first sample");
    CHECK(meter_tracker_update(&t, &r, 2000) == METER_TRACK_RECORD, "stable second");
    CHECK(meter_tracker_update(&t, &r, 3000) == METER_TRACK_NOOP, "no duplicate");
    MeterReading r2 = r;
    r2.hz += 3e6;
    CHECK(meter_tracker_update(&t, &r2, 4000) == METER_TRACK_NOOP, "new candidate");
    CHECK(meter_tracker_update(&t, &r2, 5000) == METER_TRACK_RECORD, "changed value");
}

static void checkerboard(uint8_t *rgb, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int on = ((x / 6) + (y / 6)) & 1;
            uint8_t v = on ? 245 : 15;
            size_t i = ((size_t)y * w + x) * 3;
            rgb[i] = rgb[i + 1] = rgb[i + 2] = v;
        }
    }
}

static void test_quality(void)
{
    int w = 240, h = 120;
    uint8_t *img = malloc((size_t)w * h * 3);
    CHECK(img != NULL, "alloc");

    memset(img, 128, (size_t)w * h * 3);
    FrameQuality blank = frame_quality(img, w, h);
    CHECK(blank.score < 0.25f, "blank score %.2f", blank.score);

    checkerboard(img, w, h);
    FrameQuality check = frame_quality(img, w, h);
    CHECK(check.score > 0.55f, "checkerboard score %.2f", check.score);
    CHECK(check.score > blank.score + 0.3f, "checkerboard > blank");

    memset(img, 255, (size_t)w * h * 3);
    FrameQuality glare = frame_quality(img, w, h);
    CHECK(glare.score < check.score, "glare score %.2f", glare.score);
    CHECK(glare.glare > 0.9f, "glare fraction %.2f", glare.glare);

    CHECK(check.sharpness > blank.sharpness, "sharpness ordering");

    uint8_t sig_flat[64 * 36], sig_check[64 * 36];
    memset(img, 128, (size_t)w * h * 3);
    frame_luma_signature(img, w, h, sig_flat, 64, 36);
    CHECK(signature_motion(sig_flat, sig_flat, 64 * 36) == 0.0f,
          "identical frame signature motion");
    checkerboard(img, w, h);
    frame_luma_signature(img, w, h, sig_check, 64, 36);
    CHECK(signature_motion(sig_flat, sig_check, 64 * 36) > 0.10f,
          "frame motion detects checkerboard change");
    free(img);
}

static void test_wifi(void)
{
    const char *scan =
        "BSSID Frequency RSSI Age(sec) SSID Flags\n"
        "48:5f:08:ed:f7:f1 5200 -68 10.1 synwcs_5G [WPA2-PSK-CCMP][ESS]\n"
        "34:f7:16:92:87:6c 2442 -38 10.1 syrj [WPA-PSK-CCMP][ESS]\n"
        "de:00:1a:bc:b3:4a 2422 -57 10.1 IFLYHub 86 7849 [WPA2-PSK-CCMP][ESS]\n"
        "34:f7:16:92:87:6d 2442 -30 1.0 syrj [WPA2-PSK-CCMP][ESS]\n";
    WifiAp aps[8];
    int n = wifi_parse_scan(scan, 12345, aps, 8);
    CHECK(n == 3, "wifi unique count %d", n);
    CHECK(aps[0].rssi == -30, "strongest first: %d", aps[0].rssi);
    CHECK(aps[2].rssi == -68, "weakest last: %d", aps[2].rssi);
    CHECK(aps[0].id != aps[1].id && aps[1].id != aps[2].id, "unique ids");
    CHECK(aps[0].id == wifi_anonymize("syrj", 12345), "duplicate SSID merged");
    char s[256];
    wifi_format(aps, n, s, sizeof(s));
    CHECK(strchr(s, ';') != NULL, "semicolon format: %s", s);
    CHECK(wifi_anonymize("syrj", 12345) == wifi_anonymize("syrj", 12345),
          "stable anonymisation");
    CHECK(wifi_anonymize("syrj", 12345) != wifi_anonymize("syrj", 54321),
          "salt changes id");
    const char *no_result = "No scan results\n";
    CHECK(wifi_parse_scan(no_result, 1, aps, 8) == 0, "no results");
}

static void test_util(void)
{
    char out[64];
    CHECK(cs_json_get_string("{\"a\":\"x\\ny\",\"b\":1}", "a", out, sizeof(out)), "json");
    CHECK(strcmp(out, "x\ny") == 0, "json string '%s'", out);
    bool b = false;
    CHECK(cs_json_get_bool("{\"visible\":true}", "visible", &b) && b, "json bool");

    uint8_t data[] = {0, 1, 2, 250, 251, 252};
    char *b64 = cs_base64_encode(data, sizeof(data));
    CHECK(b64 && strcmp(b64, "AAEC+vv8") == 0, "base64 %s", b64 ? b64 : "(null)");
    free(b64);

    char grouped[40];
    cs_group_u64(2160150000ULL, grouped, sizeof(grouped));
    CHECK(strcmp(grouped, "2 160 150 000") == 0, "grouping '%s'", grouped);
    char eng[40];
    cs_format_eng(2160150000ULL, eng, sizeof(eng));
    CHECK(strcmp(eng, "2.16015 GHz") == 0, "eng '%s'", eng);
}

int main(void)
{
    test_cellular();
    test_telephony();
    test_meter();
    test_quality();
    test_wifi();
    test_util();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
