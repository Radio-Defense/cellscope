#include "telephony.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define UNAVAILABLE_INT 2147483647

static bool token_boundary(char c)
{
    return !(isalnum((unsigned char)c) || c == '_');
}

static const char *find_token(const char *s, const char *name)
{
    if (!s || !name || !*name) return NULL;
    size_t n = strlen(name);
    const char *p = s;
    while ((p = strstr(p, name)) != NULL) {
        char prev = (p == s) ? ' ' : p[-1];
        char next = p[n];
        if (token_boundary(prev) &&
            (next == '\0' || next == '=' || next == ':' || next == ' ' || next == '\t' ||
             next == '\n' || next == '\r' || next == '}')) {
            return p;
        }
        p += n;
    }
    return NULL;
}

static const char *after_equals(const char *s, const char *name)
{
    const char *p = find_token(s, name);
    if (!p) return NULL;
    p += strlen(name);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static bool get_int(const char *s, const char *name, int *out)
{
    const char *p = after_equals(s, name);
    if (!p) return false;
    if (*p != '-' && !isdigit((unsigned char)*p)) return false;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)v;
    return true;
}

static void get_string(const char *s, const char *name, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    const char *p = after_equals(s, name);
    if (!p) return;
    if (*p == '"') {
        p++;
        size_t o = 0;
        while (*p && *p != '"' && o + 1 < cap) out[o++] = *p++;
        out[o] = '\0';
        return;
    }
    size_t o = 0;
    while (*p && *p != ',' && *p != '}' && *p != '\n' && *p != '\r' && o + 1 < cap)
        out[o++] = *p++;
    out[o] = '\0';
    cs_trim(out);
}

static void get_token_string(const char *s, const char *name, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    const char *p = after_equals(s, name);
    if (!p) return;
    size_t o = 0;
    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '}' && o + 1 < cap)
        out[o++] = *p++;
    out[o] = '\0';
    cs_trim(out);
}

static bool get_list(const char *s, const char *name, int *vals, int max, int *count)
{
    *count = 0;
    const char *p = after_equals(s, name);
    if (!p) return false;
    /* Arrays printed by Java are "[1, 2]" but tolerate "[]" and bare values. */
    if (*p == '[') p++;
    while (*p && *p != ']' && *count < max) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!isdigit((unsigned char)*p) && *p != '-') break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        vals[(*count)++] = (int)v;
        p = end;
    }
    return *count > 0;
}

/* Finds "Tag:{ ... }" and copies the inside of the braces. */
static bool extract_block(const char *text, const char *tag, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    const char *p = text;
    while ((p = strstr(p, tag)) != NULL) {
        const char *brace = strchr(p, '{');
        if (!brace) return false;
        int depth = 0;
        const char *start = brace + 1;
        const char *q = brace;
        for (; *q; q++) {
            if (*q == '{') depth++;
            else if (*q == '}') {
                depth--;
                if (depth == 0) {
                    size_t n = (size_t)(q - start);
                    if (n >= cap) n = cap - 1;
                    memcpy(out, start, n);
                    out[n] = '\0';
                    return true;
                }
            }
        }
        p += strlen(tag);
    }
    return false;
}

static bool value_is(const char *text, const char *key, const char *want)
{
    const char *p = after_equals(text, key);
    if (!p) return false;
    size_t n = strlen(want);
    if (strncmp(p, want, n) != 0) return false;
    char next = p[n];
    return next == '\0' || next == ',' || next == '}' || next == ' ' || next == '\n' ||
           next == '\r';
}

static bool is_unavailable(int v)
{
    return v == UNAVAILABLE_INT || v == -1;
}

static int tech_code(const char *tech)
{
    if (!tech || !*tech) return 0;
    if (strcmp(tech, "LTE") == 0) return 13;
    if (strcmp(tech, "LTE_CA") == 0) return 19;
    if (strcmp(tech, "NR") == 0) return 20;
    if (strcmp(tech, "UMTS") == 0 || strcmp(tech, "HSPA") == 0 ||
        strcmp(tech, "HSDPA") == 0 || strcmp(tech, "HSUPA") == 0)
        return 3;
    if (strcmp(tech, "EDGE") == 0) return 2;
    if (strcmp(tech, "GPRS") == 0 || strcmp(tech, "GSM") == 0) return 1;
    return atoi(tech);
}

bool telephony_parse(const char *dumpsys, TelephonyRaw *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->lte_ci = out->lte_pci = out->lte_tac = out->lte_earfcn = out->lte_band =
        out->lte_bw = -1;
    out->nr_pci = out->nr_tac = out->nr_arfcn = out->nr_band = -1;
    out->ta = -1;
    if (!dumpsys || !*dumpsys) {
        cs_strlcpy(out->diag, "Empty dumpsys output", sizeof(out->diag));
        return false;
    }

    char lte[8192] = {0};
    char nr[8192] = {0};
    char lte_sig[4096] = {0};
    char nr_sig[4096] = {0};
    bool has_lte_block = extract_block(dumpsys, "CellIdentityLte", lte, sizeof(lte));
    bool has_nr_block = extract_block(dumpsys, "CellIdentityNr", nr, sizeof(nr));
    extract_block(dumpsys, "CellSignalStrengthLte", lte_sig, sizeof(lte_sig));
    extract_block(dumpsys, "CellSignalStrengthNr", nr_sig, sizeof(nr_sig));

    int v = 0;
    if (has_lte_block) {
        if (get_int(lte, "mCi", &v) && !is_unavailable(v)) out->lte_ci = v;
        if (get_int(lte, "mPci", &v) && !is_unavailable(v)) out->lte_pci = v;
        if (get_int(lte, "mTac", &v) && !is_unavailable(v)) out->lte_tac = v;
        if (get_int(lte, "mEarfcn", &v) && !is_unavailable(v)) out->lte_earfcn = v;
        if (get_int(lte, "mBandwidth", &v) && !is_unavailable(v)) out->lte_bw = v;
        int bands[16];
        int nb = 0;
        if (get_list(lte, "mBands", bands, 16, &nb) && nb > 0) out->lte_band = bands[0];
        char tmp[32];
        get_token_string(lte, "mMcc", tmp, sizeof(tmp));
        if (isdigit((unsigned char)tmp[0])) cs_strlcpy(out->mcc, tmp, sizeof(out->mcc));
        get_token_string(lte, "mMnc", tmp, sizeof(tmp));
        if (tmp[0]) cs_strlcpy(out->mnc, tmp, sizeof(out->mnc));
        get_string(lte, "mAlphaLong", out->alpha_long, sizeof(out->alpha_long));
    }

    if (has_nr_block) {
        if (get_int(nr, "mPci", &v) && !is_unavailable(v)) out->nr_pci = v;
        if (get_int(nr, "mTac", &v) && !is_unavailable(v)) out->nr_tac = v;
        if (get_int(nr, "mNrArfcn", &v) && !is_unavailable(v)) {
            out->nr_arfcn = v;
        } else if (get_int(nr, "mArfcn", &v) && !is_unavailable(v)) {
            out->nr_arfcn = v;
        }
        int bands[16];
        int nb = 0;
        if (get_list(nr, "mBands", bands, 16, &nb) && nb > 0) out->nr_band = bands[0];
        char tmp[32];
        get_token_string(nr, "mMcc", tmp, sizeof(tmp));
        if (isdigit((unsigned char)tmp[0]) && !out->mcc[0])
            cs_strlcpy(out->mcc, tmp, sizeof(out->mcc));
        get_token_string(nr, "mMnc", tmp, sizeof(tmp));
        if (tmp[0] && !out->mnc[0]) cs_strlcpy(out->mnc, tmp, sizeof(out->mnc));
        if (!out->alpha_long[0])
            get_string(nr, "mAlphaLong", out->alpha_long, sizeof(out->alpha_long));
    }

    out->has_lte = has_lte_block &&
                   (out->lte_ci >= 0 || out->lte_pci >= 0 || out->lte_earfcn > 0);
    out->has_nr = has_nr_block &&
                  (out->nr_pci >= 0 || out->nr_arfcn > 0 || out->nr_band > 0);

    /* Operator / PLMN: cell identity is authoritative, then registration. */
    if (!out->mcc[0] || !out->mnc[0]) {
        char plmn[16] = {0};
        char tmp[32];
        get_string(dumpsys, "rRplmn", tmp, sizeof(tmp));
        cs_trim(tmp);
        if (strlen(tmp) >= 5 && isdigit((unsigned char)tmp[0]))
            cs_strlcpy(plmn, tmp, sizeof(plmn));
        if (!plmn[0]) {
            get_string(dumpsys, "operatorNumeric", tmp, sizeof(tmp));
            cs_trim(tmp);
            if (strlen(tmp) >= 5 && isdigit((unsigned char)tmp[0]))
                cs_strlcpy(plmn, tmp, sizeof(plmn));
        }
        if (plmn[0]) {
            char mcc[4], mnc[4];
            cs_plmn_split(plmn, mcc, sizeof(mcc), mnc, sizeof(mnc));
            if (!out->mcc[0]) cs_strlcpy(out->mcc, mcc, sizeof(out->mcc));
            if (!out->mnc[0]) cs_strlcpy(out->mnc, mnc, sizeof(out->mnc));
        }
    }
    if (!out->alpha_long[0]) {
        get_string(dumpsys, "mOperatorAlphaLong", out->alpha_long, sizeof(out->alpha_long));
        if (!out->alpha_long[0])
            get_string(dumpsys, "operatorAlphaLong", out->alpha_long, sizeof(out->alpha_long));
    }
    if (out->mcc[0] && out->mnc[0])
        snprintf(out->plmn, sizeof(out->plmn), "%s%s", out->mcc, out->mnc);

    get_string(dumpsys, "accessNetworkTechnology", out->data_tech, sizeof(out->data_tech));
    if (!out->data_tech[0]) {
        char tech[32];
        get_string(dumpsys, "dataNetworkType", tech, sizeof(tech));
        cs_strlcpy(out->data_tech, tech, sizeof(out->data_tech));
    }

    out->nr_connected = value_is(dumpsys, "nrState", "CONNECTED") ||
                        value_is(dumpsys, "mNrState", "CONNECTED") ||
                        value_is(dumpsys, "nrState", "NOT_RESTRICTED");
    bool tech_nr = (tech_code(out->data_tech) == 20) ||
                   (strstr(out->data_tech, "NR") != NULL);
    bool tech_lte = (tech_code(out->data_tech) == 13 || tech_code(out->data_tech) == 19) ||
                    (strstr(out->data_tech, "LTE") != NULL);

    /* NR is only shown as serving when it is plausibly the serving layer.
     * Redacted builds often print nrState=**** even for an NSA connection, so
     * presence of an NR cell identity alongside LTE is treated as NSA. */
    out->nr_serving = out->has_nr &&
                      (out->nr_connected || tech_nr || !out->has_lte || !tech_lte);
    if (out->has_nr && out->has_lte && !tech_nr) out->nr_serving = true;

    int ta = -1;
    if (lte_sig[0] && get_int(lte_sig, "ta", &ta) && !is_unavailable(ta)) {
        out->ta = ta;
    }
    if (out->ta < 0 && nr_sig[0] && get_int(nr_sig, "timingAdvance", &ta) &&
        !is_unavailable(ta)) {
        out->ta = ta;
    }
    if (out->ta < 0) {
        if (get_int(dumpsys, "mTimingAdvance", &ta) && !is_unavailable(ta)) out->ta = ta;
    }

    if (out->lte_band <= 0 && out->lte_earfcn > 0)
        out->lte_band = cs_lte_band_from_earfcn(out->lte_earfcn);
    if (out->nr_band <= 0 && out->nr_arfcn > 0)
        out->nr_band = cs_nr_band_from_arfcn(out->nr_arfcn);

    int bws[12] = {0};
    int bw_count = 0;
    get_list(dumpsys, "mCellBandwidths", bws, 12, &bw_count);
    if (bw_count > 0) {
        out->bw_count = bw_count;
        memcpy(out->bw, bws, sizeof(int) * (size_t)bw_count);
    } else if (out->lte_bw > 0) {
        out->bw[0] = out->lte_bw;
        out->bw_count = 1;
    }

    bool ca_flag = false;
    if (value_is(dumpsys, "isUsingCarrierAggregation", "true")) ca_flag = true;
    out->ca = ca_flag || out->bw_count > 1;

    out->has_tac = out->lte_tac >= 0 || out->nr_tac >= 0;
    out->ok = out->has_lte || out->has_nr;
    if (!out->ok) {
        cs_strlcpy(out->diag, "No serving cell identity found in telephony.registry",
                   sizeof(out->diag));
    }
    return out->ok;
}

static void join_pair(char *out, size_t cap, const char *a, const char *b)
{
    if (a && *a && b && *b) snprintf(out, cap, "%s / %s", a, b);
    else if (a && *a) snprintf(out, cap, "%s", a);
    else if (b && *b) snprintf(out, cap, "%s", b);
    else snprintf(out, cap, "—");
}

static void fmt_int(char *out, size_t cap, int v)
{
    if (v < 0) snprintf(out, cap, "—");
    else snprintf(out, cap, "%d", v);
}

void telephony_to_sample(const TelephonyRaw *raw, CellSample *s)
{
    if (!raw || !s) return;
    memset(s, 0, sizeof(*s));
    s->ts_ms = cs_epoch_ms();
    cs_strlcpy(s->plmn, raw->plmn, sizeof(s->plmn));
    cs_strlcpy(s->mcc, raw->mcc, sizeof(s->mcc));
    cs_strlcpy(s->mnc, raw->mnc, sizeof(s->mnc));
    cs_mcc_country(raw->mcc, s->country, sizeof(s->country));
    cs_plmn_operator(raw->plmn, s->operator_name, sizeof(s->operator_name));
    if (!s->operator_name[0]) cs_strlcpy(s->operator_name, raw->alpha_long,
                                         sizeof(s->operator_name));

    bool show_nr = raw->nr_serving && raw->has_nr;
    bool show_lte = raw->has_lte;

    /* TAC: the LTE anchor is the primary TAC for NSA. */
    int tac = raw->lte_tac >= 0 ? raw->lte_tac : raw->nr_tac;
    fmt_int(s->tac, sizeof(s->tac), tac);

    if (show_nr) {
        if (show_lte && (tech_code(raw->data_tech) == 13 ||
                         tech_code(raw->data_tech) == 19 ||
                         !raw->data_tech[0] || strstr(raw->data_tech, "LTE")))
            cs_strlcpy(s->rat, "5G NSA", sizeof(s->rat));
        else
            cs_strlcpy(s->rat, "5G SA", sizeof(s->rat));
    } else if (show_lte) {
        cs_strlcpy(s->rat, raw->ca ? "LTE-A" : "LTE", sizeof(s->rat));
    } else if (tech_code(raw->data_tech) == 3 || strstr(raw->data_tech, "UMTS") ||
               strstr(raw->data_tech, "HSPA")) {
        cs_strlcpy(s->rat, "UMTS", sizeof(s->rat));
    } else if (tech_code(raw->data_tech) == 1 || tech_code(raw->data_tech) == 2 ||
               strstr(raw->data_tech, "EDGE") || strstr(raw->data_tech, "GPRS")) {
        cs_strlcpy(s->rat, "GSM", sizeof(s->rat));
    } else {
        cs_strlcpy(s->rat, "Cellular", sizeof(s->rat));
    }

    /* eNB / CID derived from the 28-bit LTE ECI. */
    if (show_lte && raw->lte_ci > 0 && raw->lte_ci < (1 << 28)) {
        unsigned ci = (unsigned)raw->lte_ci;
        snprintf(s->enb, sizeof(s->enb), "%u", ci >> 8);
        snprintf(s->cid, sizeof(s->cid), "%u", ci & 0xFF);
    } else {
        cs_strlcpy(s->enb, "—", sizeof(s->enb));
        cs_strlcpy(s->cid, "—", sizeof(s->cid));
    }

    char nr_pci[12] = {0}, lte_pci[12] = {0};
    if (show_nr && raw->nr_pci >= 0) snprintf(nr_pci, sizeof(nr_pci), "%d", raw->nr_pci);
    if (show_lte && raw->lte_pci >= 0) snprintf(lte_pci, sizeof(lte_pci), "%d", raw->lte_pci);
    join_pair(s->pci, sizeof(s->pci), nr_pci, lte_pci);

    if (raw->ta >= 0) snprintf(s->ta, sizeof(s->ta), "%d", raw->ta);
    else cs_strlcpy(s->ta, "—", sizeof(s->ta));

    char nr_arfcn[16] = {0}, lte_earfcn[16] = {0};
    if (show_nr && raw->nr_arfcn > 0) snprintf(nr_arfcn, sizeof(nr_arfcn), "%d", raw->nr_arfcn);
    if (show_lte && raw->lte_earfcn > 0)
        snprintf(lte_earfcn, sizeof(lte_earfcn), "%d", raw->lte_earfcn);
    join_pair(s->arfcn, sizeof(s->arfcn), nr_arfcn, lte_earfcn);

    char nr_band[12] = {0}, lte_band[12] = {0};
    if (show_nr && raw->nr_band > 0) snprintf(nr_band, sizeof(nr_band), "N%d", raw->nr_band);
    if (show_lte && raw->lte_band > 0)
        snprintf(lte_band, sizeof(lte_band), "L%d", raw->lte_band);
    join_pair(s->band, sizeof(s->band), nr_band, lte_band);

    if (raw->bw_count > 0) {
        size_t o = 0;
        for (int i = 0; i < raw->bw_count && o + 8 < sizeof(s->bw); i++) {
            o += (size_t)snprintf(s->bw + o, sizeof(s->bw) - o, "%s%d", i ? " / " : "",
                                  raw->bw[i]);
        }
    } else {
        cs_strlcpy(s->bw, "—", sizeof(s->bw));
    }
    s->ca = raw->ca;

    char nr_dl[16] = {0}, lte_dl[16] = {0}, nr_ul[16] = {0}, lte_ul[16] = {0};
    if (show_nr && raw->nr_arfcn > 0) {
        uint64_t dl = 0, ul = 0;
        if (cs_nr_arfcn_to_hz(raw->nr_arfcn, &dl)) {
            cs_format_mhz_value(dl, nr_dl, sizeof(nr_dl), 1);
            if (cs_nr_arfcn_to_ul_hz(raw->nr_arfcn, raw->nr_band, &ul))
                cs_format_mhz_value(ul, nr_ul, sizeof(nr_ul), 1);
        }
    }
    if (show_lte && raw->lte_earfcn > 0) {
        uint64_t dl = 0, ul = 0;
        if (cs_lte_earfcn_to_hz(raw->lte_earfcn, raw->lte_band, &dl, &ul)) {
            cs_format_mhz_value(dl, lte_dl, sizeof(lte_dl), 1);
            cs_format_mhz_value(ul, lte_ul, sizeof(lte_ul), 1);
        }
    }
    join_pair(s->f_dl, sizeof(s->f_dl), nr_dl, lte_dl);
    join_pair(s->f_ul, sizeof(s->f_ul), nr_ul, lte_ul);
    s->demo = false;
    s->source[0] = '\0';
}
