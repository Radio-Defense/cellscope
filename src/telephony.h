#ifndef CELLSCOPE_TELEPHONY_H
#define CELLSCOPE_TELEPHONY_H

#include <stdbool.h>
#include <stdint.h>

#include "cellular.h"

typedef struct {
    int64_t ts_ms;
    uint64_t seq;
    char plmn[8];
    char mcc[4];
    char mnc[4];
    char country[32];
    char operator_name[48];
    char tac[12];
    char rat[24];
    char enb[12];
    char cid[12];
    char pci[24];
    char ta[16];
    char arfcn[32];
    char band[24];
    char bw[32];
    bool ca;
    char f_dl[32];
    char f_ul[32];
    char wifi[768];
    bool demo;
    char source[64];
} CellSample;

typedef struct {
    bool ok;
    bool has_lte;
    bool has_nr;
    bool nr_serving;
    bool nr_connected;
    bool ca;
    bool has_tac;

    char plmn[8];
    char mcc[4];
    char mnc[4];
    char alpha_long[48];
    char data_tech[32];
    char diag[192];

    int lte_ci;
    int lte_pci;
    int lte_tac;
    int lte_earfcn;
    int lte_band;
    int lte_bw;
    int nr_pci;
    int nr_tac;
    int nr_arfcn;
    int nr_band;
    int ta;

    int bw[12];
    int bw_count;
} TelephonyRaw;

/* Parse the text output of `adb shell dumpsys telephony.registry`. */
bool telephony_parse(const char *dumpsys, TelephonyRaw *out);
void telephony_to_sample(const TelephonyRaw *raw, CellSample *out);

#endif
