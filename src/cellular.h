#ifndef CELLSCOPE_CELLULAR_H
#define CELLSCOPE_CELLULAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PLMN helpers. plmn is the concatenated MCC+MNC string ("20408"). */
void cs_plmn_split(const char *plmn, char *mcc, size_t mcc_cap, char *mnc, size_t mnc_cap);
void cs_mcc_country(const char *mcc, char *out, size_t cap);
void cs_plmn_operator(const char *plmn, char *out, size_t cap);

/* 3GPP frequency conversions, returning centre frequencies in Hz. */
bool cs_nr_arfcn_to_hz(int arfcn, uint64_t *hz);
bool cs_nr_arfcn_to_ul_hz(int arfcn, int band, uint64_t *ul_hz);
bool cs_nr_band_info(int band, double *dl_low, double *dl_high, double *ul_offset, bool *tdd);
bool cs_lte_earfcn_to_hz(int earfcn, int band_hint, uint64_t *dl_hz, uint64_t *ul_hz);
int cs_nr_band_from_arfcn(int arfcn);
int cs_lte_band_from_earfcn(int earfcn);

/* "2160.2" (no unit), suitable for the paired "DL / UL" display. */
void cs_format_mhz_value(uint64_t hz, char *out, size_t cap, int decimals);

#endif
