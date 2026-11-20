#ifndef CELLSCOPE_METER_H
#define CELLSCOPE_METER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool visible;
    double hz;
    double confidence;
    char unit[8];
    char raw[256];
} MeterReading;

/* Parse a free-form model answer such as "2.160150 GHz" or the JSON contract
 * {"visible":true,"reading":"2.160150","unit":"GHz","confidence":0.94}. */
bool meter_parse_text(const char *text, const char *default_unit, MeterReading *out);
bool meter_parse_model_output(const char *text, const char *default_unit, MeterReading *out);

void meter_format_hz(uint64_t hz, char *out, size_t cap);
void meter_format_eng(uint64_t hz, char *out, size_t cap);
void meter_format_time_ms(int64_t epoch_ms, char *out, size_t cap);

typedef enum {
    METER_TRACK_NOOP = 0,
    METER_TRACK_RECORD = 1
} MeterTrackAction;

typedef struct {
    bool have_candidate;
    double candidate_hz;
    int stable_count;
    int64_t last_seen_ms;
    bool has_recorded;
    double recorded_hz;
    int64_t recorded_ms;
} MeterTracker;

void meter_tracker_init(MeterTracker *t);
MeterTrackAction meter_tracker_update(MeterTracker *t, const MeterReading *r, int64_t now_ms);
int meter_tracker_stable(const MeterTracker *t);

#endif
