#include "meter.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct {
    const char *name;
    double hz;
} MeterUnit;

static const MeterUnit k_units[] = {
    {"ghz", 1e9},
    {"mhz", 1e6},
    {"khz", 1e3},
    {"hz", 1.0},
};

static double unit_multiplier(const char *unit)
{
    if (!unit || !*unit) return 0.0;
    char low[16];
    cs_strlcpy(low, unit, sizeof(low));
    cs_lower(low);
    cs_trim(low);
    for (size_t i = 0; i < CS_ARRAY_LEN(k_units); i++) {
        if (strcmp(low, k_units[i].name) == 0) return k_units[i].hz;
    }
    return 0.0;
}

static bool find_unit_near(const char *text, size_t num_start, size_t num_end, char *unit,
                           size_t cap)
{
    /* Prefer a unit after the number, then one immediately before it. */
    size_t i = num_end;
    while (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') i++;
    if (strncmp(text + i, "ghz", 3) == 0) { cs_strlcpy(unit, "GHz", cap); return true; }
    if (strncmp(text + i, "mhz", 3) == 0) { cs_strlcpy(unit, "MHz", cap); return true; }
    if (strncmp(text + i, "khz", 3) == 0) { cs_strlcpy(unit, "kHz", cap); return true; }
    if (strncmp(text + i, "hz", 2) == 0) { cs_strlcpy(unit, "Hz", cap); return true; }
    if (strncmp(text + i, "g hz", 4) == 0) { cs_strlcpy(unit, "GHz", cap); return true; }
    if (strncmp(text + i, "m hz", 4) == 0) { cs_strlcpy(unit, "MHz", cap); return true; }
    if (strncmp(text + i, "k hz", 4) == 0) { cs_strlcpy(unit, "kHz", cap); return true; }

    size_t j = num_start;
    while (j > 0 && (text[j - 1] == ' ' || text[j - 1] == '\t')) j--;
    if (j >= 3 && strncmp(text + j - 3, "ghz", 3) == 0) {
        cs_strlcpy(unit, "GHz", cap);
        return true;
    }
    if (j >= 3 && strncmp(text + j - 3, "mhz", 3) == 0) {
        cs_strlcpy(unit, "MHz", cap);
        return true;
    }
    if (j >= 3 && strncmp(text + j - 3, "khz", 3) == 0) {
        cs_strlcpy(unit, "kHz", cap);
        return true;
    }
    if (j >= 2 && strncmp(text + j - 2, "hz", 2) == 0) {
        cs_strlcpy(unit, "Hz", cap);
        return true;
    }
    return false;
}

/* Convert a numeric token with either '.' or ',' separators into a double.
 * The heuristic is deliberately conservative: when both separators exist the
 * rightmost one is the decimal separator; a lone comma with exactly three
 * trailing digits is treated as a thousands separator for MHz/kHz/Hz, and as
 * a decimal separator for GHz (where 2,160 GHz is the common display form). */
static bool parse_number_token(const char *tok, const char *unit, double *out)
{
    char clean[64];
    size_t n = strlen(tok);
    if (n >= sizeof(clean)) return false;

    /* Choose one decimal separator.
     * - both separators: the rightmost is decimal;
     * - multiple dots: the rightmost is decimal, earlier dots are grouping;
     * - multiple commas: a final group of three is grouping, otherwise the
     *   rightmost comma is decimal;
     * - a single comma with three trailing digits is grouping for MHz/kHz/Hz,
     *   but decimal for GHz. */
    const char *last_dot = strrchr(tok, '.');
    const char *last_comma = strrchr(tok, ',');
    char decimal = 0;
    size_t decimal_pos = (size_t)-1;
    if (last_dot && last_comma) {
        if (last_dot > last_comma) { decimal = '.'; decimal_pos = (size_t)(last_dot - tok); }
        else { decimal = ','; decimal_pos = (size_t)(last_comma - tok); }
    } else if (last_dot) {
        decimal = '.';
        decimal_pos = (size_t)(last_dot - tok);
    } else if (last_comma) {
        const char *after = last_comma + 1;
        size_t digits = strlen(after);
        bool multiple = strchr(tok, ',') != last_comma;
        if (multiple && digits == 3) {
            decimal = 0;
        } else if (!multiple && digits == 3 && unit && strcmp(unit, "GHz") != 0) {
            decimal = 0;
        } else {
            decimal = ',';
            decimal_pos = (size_t)(last_comma - tok);
        }
    }

    size_t o = 0;
    bool wrote_decimal = false;
    for (size_t i = 0; i < n; i++) {
        char c = tok[i];
        if (isdigit((unsigned char)c)) {
            clean[o++] = c;
        } else if (c == decimal && decimal && i == decimal_pos && !wrote_decimal) {
            clean[o++] = '.';
            wrote_decimal = true;
        } else if (c == '.' || c == ',' || c == ' ') {
            continue;
        } else if (c == '-' && o == 0) {
            clean[o++] = c;
        }
    }
    clean[o] = '\0';
    if (!o) return false;
    char *end = NULL;
    double v = strtod(clean, &end);
    if (end == clean) return false;
    *out = v;
    return true;
}

bool meter_parse_text(const char *text, const char *default_unit, MeterReading *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text || !*text) return false;
    cs_strlcpy(out->raw, text, sizeof(out->raw));

    char low[1024];
    cs_strlcpy(low, text, sizeof(low));
    cs_lower(low);

    bool any = false;
    for (size_t i = 0; low[i]; i++) {
        if (low[i] == '-' && low[i + 1]) i++;
        if (!isdigit((unsigned char)low[i])) continue;
        /* Skip digits that are part of an identifier such as "Band 8". */
        if (i > 0 && (isalpha((unsigned char)low[i - 1]) || low[i - 1] == '_')) continue;

        size_t start = (i > 0 && low[i - 1] == '-') ? i - 1 : i;
        size_t end = i;
        while (low[end] && (isdigit((unsigned char)low[end]) || low[end] == '.' ||
                            low[end] == ',')) {
            end++;
        }
        /* Reject a number followed immediately by more letters (e.g. 3GPP). */
        if (isalpha((unsigned char)low[end]) && strncmp(low + end, "ghz", 3) != 0 &&
            strncmp(low + end, "mhz", 3) != 0 && strncmp(low + end, "khz", 3) != 0 &&
            strncmp(low + end, "hz", 2) != 0) {
            continue;
        }

        char tok[64];
        size_t tn = end - start;
        if (tn == 0 || tn >= sizeof(tok)) continue;
        memcpy(tok, low + start, tn);
        tok[tn] = '\0';

        char unit[8] = {0};
        if (!find_unit_near(low, start, end, unit, sizeof(unit))) {
            if (default_unit && *default_unit) cs_strlcpy(unit, default_unit, sizeof(unit));
        }
        double mult = unit_multiplier(unit);
        if (mult <= 0.0) mult = 1e6; /* harmless safe default: MHz */

        double value = 0.0;
        if (!parse_number_token(tok, unit, &value)) continue;
        double hz = value * mult;
        if ((hz < 1.0 || hz > 1e11) && strchr(tok, ',')) {
            /* A European decimal comma (e.g. 2160,150 MHz) can look like a
             * thousands separator. If the first interpretation is impossible,
             * force the rightmost comma to be the decimal separator. */
            char alt[64];
            cs_strlcpy(alt, tok, sizeof(alt));
            char *comma = strrchr(alt, ',');
            if (comma) {
                *comma = '.';
                char *other = strchr(alt, ',');
                if (other) memmove(other, other + 1, strlen(other));
                double v2 = 0;
                if (parse_number_token(alt, "GHz", &v2)) {
                    double hz2 = v2 * mult;
                    if (hz2 >= 1.0 && hz2 <= 1e11) hz = hz2;
                }
            }
        }
        /* Zero is a status/placeholder (for example CTCSS 0.0 Hz), never a
         * valid measured frequency. Ignore it and keep scanning the text. */
        if (hz <= 0.0 || hz > 1e11) continue;
        out->hz = hz;
        out->visible = true;
        if (unit[0]) cs_strlcpy(out->unit, unit, sizeof(out->unit));
        out->confidence = 0.75;
        any = true;
        break;
    }
    return any;
}

bool meter_parse_model_output(const char *text, const char *default_unit, MeterReading *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text || !*text) return false;

    /* Models often wrap JSON in ```json fences or prose. */
    const char *brace = strchr(text, '{');
    if (brace) {
        char reading[128] = {0};
        char unit[16] = {0};
        bool visible = true;
        bool has_visible = cs_json_get_bool(brace, "visible", &visible);
        bool has_reading = cs_json_get_string(brace, "reading", reading, sizeof(reading));
        cs_json_get_string(brace, "unit", unit, sizeof(unit));
        double confidence = 0.0;
        bool has_conf = cs_json_get_double(brace, "confidence", &confidence);
        if (has_visible && !visible) {
            out->visible = false;
            out->confidence = has_conf ? confidence : 0.0;
            cs_strlcpy(out->raw, text, sizeof(out->raw));
            return true;
        }
        if (has_reading && reading[0]) {
            char combined[192];
            if (unit[0])
                snprintf(combined, sizeof(combined), "%s %s", reading, unit);
            else
                cs_strlcpy(combined, reading, sizeof(combined));
            if (meter_parse_text(combined, default_unit, out)) {
                if (has_conf) out->confidence = confidence;
                cs_strlcpy(out->raw, text, sizeof(out->raw));
                cs_strlcpy(out->unit, unit, sizeof(out->unit));
                return true;
            }
            /* The screen is present but these digits were not parseable. Keep
             * the visibility session alive; it may recover on the next frame. */
            if (has_visible && visible) {
                out->visible = true;
                out->hz = 0;
                out->confidence = has_conf ? confidence : 0.0;
                cs_strlcpy(out->raw, text, sizeof(out->raw));
                return true;
            }
            return false;
        }
        if (has_visible && visible) {
            out->visible = true;
            out->hz = 0;
            out->confidence = has_conf ? confidence : 0.0;
            cs_strlcpy(out->raw, text, sizeof(out->raw));
            return true;
        }
    }

    /* Ollama returns the model text in "response"; llama-server puts it in
     * choices[0].message.content. If given the envelope, unwrap it. */
    char inner[2048];
    if (cs_json_get_string(text, "response", inner, sizeof(inner)) && inner[0] &&
        strcmp(inner, text) != 0) {
        return meter_parse_model_output(inner, default_unit, out);
    }
    if (cs_json_get_string(text, "content", inner, sizeof(inner)) && inner[0] &&
        strcmp(inner, text) != 0) {
        return meter_parse_model_output(inner, default_unit, out);
    }

    return meter_parse_text(text, default_unit, out);
}

void meter_format_hz(uint64_t hz, char *out, size_t cap)
{
    cs_format_hz(hz, out, cap);
}

void meter_format_eng(uint64_t hz, char *out, size_t cap)
{
    cs_format_eng(hz, out, cap);
}

void meter_format_time_ms(int64_t epoch_ms, char *out, size_t cap)
{
    cs_format_time_ms(epoch_ms, out, cap);
}

void meter_tracker_init(MeterTracker *t)
{
    if (t) memset(t, 0, sizeof(*t));
}

int meter_tracker_stable(const MeterTracker *t)
{
    return t ? t->stable_count : 0;
}

MeterTrackAction meter_tracker_update(MeterTracker *t, const MeterReading *r, int64_t now_ms)
{
    if (!t || !r) return METER_TRACK_NOOP;
    if (!r->visible || r->hz <= 0) {
        t->have_candidate = false;
        t->stable_count = 0;
        return METER_TRACK_NOOP;
    }

    double tolerance = CS_MAX(80.0, r->hz * 0.0002);
    if (t->have_candidate && fabs(r->hz - t->candidate_hz) <= tolerance) {
        t->stable_count++;
        t->candidate_hz = (t->candidate_hz * 0.7) + (r->hz * 0.3);
    } else {
        t->have_candidate = true;
        t->candidate_hz = r->hz;
        t->stable_count = 1;
    }
    t->last_seen_ms = now_ms;

    if (t->stable_count < 2) return METER_TRACK_NOOP;

    bool changed = !t->has_recorded ||
                   fabs(t->candidate_hz - t->recorded_hz) / CS_MAX(1.0, t->recorded_hz) >
                       0.0005;
    bool heartbeat = t->has_recorded && now_ms - t->recorded_ms > 30000;
    if (!changed && !heartbeat) return METER_TRACK_NOOP;

    t->has_recorded = true;
    t->recorded_hz = t->candidate_hz;
    t->recorded_ms = now_ms;
    return METER_TRACK_RECORD;
}
