#include "cellular.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int band;
    double dl_low_mhz;
    double dl_high_mhz;
    int dl_offset;
    double ul_low_mhz;
    bool tdd;
} LteBand;

static const LteBand k_lte_bands[] = {
    {1, 2110.0, 2170.0, 0, 1920.0, false},
    {2, 1930.0, 1990.0, 600, 1850.0, false},
    {3, 1805.0, 1880.0, 1200, 1710.0, false},
    {4, 2110.0, 2155.0, 1950, 1710.0, false},
    {5, 869.0, 894.0, 2400, 824.0, false},
    {7, 2620.0, 2690.0, 2750, 2500.0, false},
    {8, 925.0, 960.0, 3450, 880.0, false},
    {12, 729.0, 746.0, 5010, 699.0, false},
    {13, 746.0, 756.0, 5180, 777.0, false},
    {14, 758.0, 768.0, 5280, 788.0, false},
    {17, 734.0, 746.0, 5730, 704.0, false},
    {18, 860.0, 875.0, 5850, 815.0, false},
    {19, 875.0, 890.0, 5900, 830.0, false},
    {20, 791.0, 821.0, 6150, 832.0, false},
    {21, 1447.9, 1462.9, 6450, 1447.9, false},
    {25, 1930.0, 1995.0, 8040, 1850.0, false},
    {26, 859.0, 894.0, 8690, 814.0, false},
    {28, 758.0, 803.0, 9210, 703.0, false},
    {30, 2350.0, 2360.0, 9770, 2305.0, false},
    {32, 1452.0, 1496.0, 9920, 1452.0, true},
    {34, 2010.0, 2025.0, 36200, 2010.0, true},
    {38, 2570.0, 2620.0, 37750, 2570.0, true},
    {39, 1880.0, 1920.0, 38250, 1880.0, true},
    {40, 2300.0, 2400.0, 38650, 2300.0, true},
    {41, 2496.0, 2690.0, 39650, 2496.0, true},
    {42, 3400.0, 3600.0, 41590, 3400.0, true},
    {43, 3600.0, 3800.0, 42590, 3600.0, true},
    {46, 5150.0, 5925.0, 46790, 5150.0, true},
    {48, 3550.0, 3700.0, 55240, 3550.0, true},
    {66, 2110.0, 2200.0, 66436, 1710.0, false},
    {71, 617.0, 652.0, 68586, 663.0, false},
};

typedef struct {
    int band;
    int arfcn_low;
    int arfcn_high;
    double dl_low_mhz;
    double dl_high_mhz;
    double ul_offset_mhz; /* add to DL centre; 0 for TDD */
    bool tdd;
} NrBand;

static const NrBand k_nr_bands[] = {
    {1, 422000, 434000, 2110.0, 2170.0, -190.0, false},
    {2, 386000, 398000, 1930.0, 1990.0, -80.0, false},
    {3, 361000, 376000, 1805.0, 1880.0, -95.0, false},
    {5, 173800, 178800, 869.0, 894.0, -45.0, false},
    {7, 524000, 538000, 2620.0, 2690.0, -120.0, false},
    {8, 185000, 192000, 925.0, 960.0, -45.0, false},
    {12, 145800, 149200, 729.0, 746.0, -30.0, false},
    {13, 149200, 151200, 746.0, 756.0, 31.0, false},
    {14, 151600, 153600, 758.0, 768.0, 30.0, false},
    {18, 172000, 175000, 860.0, 875.0, -45.0, false},
    {20, 158200, 164200, 791.0, 821.0, 41.0, false},
    {25, 386000, 399000, 1930.0, 1995.0, -80.0, false},
    {26, 162200, 166200, 859.0, 894.0, -45.0, false},
    {28, 151600, 160600, 758.0, 803.0, -55.0, false},
    {30, 470000, 480000, 2350.0, 2360.0, -45.0, false},
    {38, 514000, 524000, 2570.0, 2620.0, 0.0, true},
    {39, 376000, 382000, 1880.0, 1920.0, 0.0, true},
    {40, 460000, 480000, 2300.0, 2400.0, 0.0, true},
    {41, 499200, 537999, 2496.0, 2690.0, 0.0, true},
    {48, 636666, 646666, 3550.0, 3700.0, 0.0, true},
    {66, 422000, 440000, 2110.0, 2200.0, -400.0, false},
    {71, 123400, 130400, 617.0, 652.0, 46.0, false},
    {78, 620000, 653333, 3300.0, 3800.0, 0.0, true},
    {77, 620000, 680000, 3300.0, 4200.0, 0.0, true},
    {79, 693334, 733333, 4400.0, 5000.0, 0.0, true},
    {257, 2054166, 2104165, 26500.0, 29500.0, 0.0, true},
    {258, 2016667, 2070832, 24250.0, 27500.0, 0.0, true},
    {260, 2229166, 2279166, 37000.0, 40000.0, 0.0, true},
    {261, 2070833, 2084999, 27500.0, 28350.0, 0.0, true},
};

typedef struct {
    const char *mcc;
    const char *country;
} MccCountry;

static const MccCountry k_countries[] = {
    {"202", "Greece"},      {"204", "Netherlands"}, {"206", "Belgium"},
    {"208", "France"},      {"212", "Monaco"},      {"213", "Andorra"},
    {"214", "Spain"},       {"216", "Hungary"},     {"218", "Bosnia & Herz."},
    {"219", "Croatia"},     {"220", "Serbia"},      {"222", "Italy"},
    {"226", "Romania"},     {"228", "Switzerland"}, {"230", "Czechia"},
    {"231", "Slovakia"},    {"232", "Austria"},     {"234", "United Kingdom"},
    {"235", "United Kingdom"}, {"238", "Denmark"},  {"240", "Sweden"},
    {"242", "Norway"},      {"244", "Finland"},     {"246", "Lithuania"},
    {"247", "Latvia"},      {"248", "Estonia"},     {"250", "Russia"},
    {"255", "Ukraine"},     {"260", "Poland"},      {"262", "Germany"},
    {"266", "Gibraltar"},   {"268", "Portugal"},    {"270", "Luxembourg"},
    {"272", "Ireland"},     {"274", "Iceland"},     {"276", "Albania"},
    {"278", "Malta"},       {"280", "Cyprus"},      {"284", "Bulgaria"},
    {"286", "Türkiye"},     {"302", "Canada"},      {"310", "United States"},
    {"311", "United States"}, {"312", "United States"}, {"313", "United States"},
    {"314", "United States"}, {"315", "United States"}, {"316", "United States"},
    {"330", "Puerto Rico"}, {"334", "Mexico"},      {"338", "Jamaica"},
    {"340", "French Antilles"}, {"350", "Bermuda"}, {"358", "Saint Lucia"},
    {"360", "Dominican Rep."}, {"368", "Cuba"},     {"370", "Dominican Rep."},
    {"372", "Haiti"},       {"374", "Trinidad & Tobago"}, {"376", "Turks & Caicos"},
    {"404", "India"},       {"405", "India"},       {"406", "India"},
    {"410", "Pakistan"},    {"412", "Afghanistan"}, {"413", "Sri Lanka"},
    {"414", "Myanmar"},     {"415", "Lebanon"},     {"416", "Jordan"},
    {"417", "Syria"},       {"418", "Iraq"},        {"419", "Kuwait"},
    {"420", "Saudi Arabia"}, {"421", "Yemen"},      {"422", "Oman"},
    {"424", "United Arab Emirates"}, {"425", "Israel"}, {"426", "Bahrain"},
    {"427", "Qatar"},       {"428", "Mongolia"},    {"429", "Nepal"},
    {"430", "United Arab Emirates"}, {"431", "United Arab Emirates"},
    {"432", "Iran"},        {"434", "Uzbekistan"},  {"436", "Tajikistan"},
    {"437", "Kyrgyzstan"},  {"438", "Turkmenistan"}, {"440", "Japan"},
    {"441", "Japan"},       {"450", "South Korea"}, {"452", "Vietnam"},
    {"454", "Hong Kong"},   {"455", "Macao"},       {"456", "Cambodia"},
    {"457", "Laos"},        {"460", "China"},       {"461", "China"},
    {"466", "Taiwan"},      {"467", "North Korea"}, {"470", "Bangladesh"},
    {"472", "Maldives"},    {"502", "Malaysia"},    {"505", "Australia"},
    {"510", "Indonesia"},   {"514", "Timor-Leste"}, {"515", "Philippines"},
    {"520", "Thailand"},    {"525", "Singapore"},   {"528", "Brunei"},
    {"530", "New Zealand"}, {"536", "Nauru"},       {"537", "Papua New Guinea"},
    {"539", "Tonga"},       {"540", "Solomon Is."}, {"541", "Vanuatu"},
    {"542", "Fiji"},        {"544", "American Samoa"}, {"546", "New Caledonia"},
    {"548", "Cook Is."},    {"549", "Samoa"},       {"550", "Micronesia"},
    {"551", "Marshall Is."}, {"552", "Palau"},      {"553", "Tuvalu"},
    {"555", "Niue"},        {"602", "Egypt"},       {"603", "Algeria"},
    {"604", "Morocco"},     {"605", "Tunisia"},     {"606", "Libya"},
    {"607", "Gambia"},      {"608", "Senegal"},     {"609", "Mauritania"},
    {"610", "Mali"},        {"611", "Guinea"},      {"612", "Ivory Coast"},
    {"613", "Burkina Faso"}, {"614", "Niger"},      {"615", "Togo"},
    {"616", "Benin"},       {"617", "Mauritius"},   {"618", "Liberia"},
    {"619", "Sierra Leone"}, {"620", "Ghana"},      {"621", "Nigeria"},
    {"622", "Chad"},        {"623", "Central African Rep."}, {"624", "Cameroon"},
    {"625", "Cape Verde"},  {"626", "São Tomé & Príncipe"}, {"627", "Equatorial Guinea"},
    {"628", "Gabon"},       {"629", "Congo"},       {"630", "DR Congo"},
    {"631", "Angola"},      {"632", "Guinea-Bissau"}, {"633", "Seychelles"},
    {"634", "Sudan"},       {"635", "Rwanda"},      {"636", "Ethiopia"},
    {"637", "Somalia"},     {"638", "Djibouti"},    {"639", "Kenya"},
    {"640", "Tanzania"},    {"641", "Uganda"},      {"642", "Burundi"},
    {"643", "Mozambique"},  {"645", "Zambia"},      {"646", "Madagascar"},
    {"647", "Réunion"},     {"648", "Zimbabwe"},    {"649", "Namibia"},
    {"650", "Malawi"},      {"651", "Lesotho"},     {"652", "Botswana"},
    {"653", "Eswatini"},    {"654", "Comoros"},     {"655", "South Africa"},
    {"657", "Eritrea"},     {"659", "South Sudan"}, {"702", "Belize"},
    {"704", "Guatemala"},   {"706", "El Salvador"}, {"708", "Honduras"},
    {"710", "Nicaragua"},   {"712", "Costa Rica"},  {"714", "Panama"},
    {"716", "Peru"},        {"722", "Argentina"},   {"724", "Brazil"},
    {"730", "Chile"},       {"732", "Colombia"},    {"734", "Venezuela"},
    {"736", "Bolivia"},     {"738", "Guyana"},      {"740", "Ecuador"},
    {"742", "French Guiana"}, {"744", "Paraguay"},  {"746", "Suriname"},
    {"748", "Uruguay"},     {"750", "Falkland Is."},
};

typedef struct {
    const char *plmn;
    const char *name;
} PlmnName;

static const PlmnName k_plmns[] = {
    {"20408", "KPN"},         {"20404", "Vodafone"},   {"20416", "Odido"},
    {"20420", "Telfort"},     {"20412", "Telfort"},    {"20402", "Tele2"},
    {"20601", "Proximus"},    {"20610", "Orange"},     {"20801", "Orange"},
    {"20802", "Orange"},      {"20810", "SFR"},        {"20815", "Free"},
    {"20820", "Bouygues"},    {"21401", "Vodafone"},   {"21403", "Orange"},
    {"21407", "Movistar"},    {"22201", "TIM"},        {"22210", "Vodafone"},
    {"22801", "Swisscom"},    {"22802", "Sunrise"},    {"22803", "Salt"},
    {"23201", "A1"},          {"23203", "Magenta"},    {"23210", "Drei"},
    {"23415", "Vodafone"},    {"23430", "EE"},        {"23420", "Three"},
    {"24001", "Telia"},       {"24002", "Tele2"},      {"24007", "Telenor"},
    {"24201", "Telenor"},     {"24202", "Telia"},      {"24491", "DNA"},
    {"24601", "Telia"},       {"24701", "LMT"},        {"24801", "Telia"},
    {"25001", "MTS"},         {"25002", "MegaFon"},    {"25020", "Tele2"},
    {"25501", "Kyivstar"},    {"25502", "Vodafone"},   {"26001", "Plus"},
    {"26002", "T-Mobile"},    {"26003", "Orange"},     {"26201", "Telekom"},
    {"26202", "Vodafone"},    {"26203", "O2"},         {"28601", "Turkcell"},
    {"28602", "Vodafone"},    {"302720", "Rogers"},    {"310260", "T-Mobile"},
    {"310410", "AT&T"},       {"311480", "Verizon"},   {"334020", "Telcel"},
    {"40445", "Airtel"},      {"40410", "Airtel"},     {"44010", "NTT docomo"},
    {"44020", "SoftBank"},    {"45005", "SK Telecom"}, {"45008", "KT"},
    {"46000", "China Mobile"}, {"46001", "China Unicom"}, {"46003", "China Telecom"},
    {"50501", "Telstra"},     {"50502", "Optus"},      {"53001", "Spark"},
    {"65501", "Vodacom"},     {"65502", "MTN"},        {"72405", "Claro"},
    {"732101", "Claro"},      {"732103", "Movistar"},  {"732130", "Tigo"},
};

void cs_plmn_split(const char *plmn, char *mcc, size_t mcc_cap, char *mnc, size_t mnc_cap)
{
    if (mcc && mcc_cap) mcc[0] = '\0';
    if (mnc && mnc_cap) mnc[0] = '\0';
    if (!plmn) return;
    size_t n = strlen(plmn);
    if (n < 5) return;
    if (mcc && mcc_cap) {
        size_t take = n >= 3 ? 3 : n;
        if (take >= mcc_cap) take = mcc_cap - 1;
        memcpy(mcc, plmn, take);
        mcc[take] = '\0';
    }
    if (mnc && mnc_cap) snprintf(mnc, mnc_cap, "%s", plmn + 3);
}

void cs_mcc_country(const char *mcc, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!mcc) return;
    for (size_t i = 0; i < CS_ARRAY_LEN(k_countries); i++) {
        if (strcmp(k_countries[i].mcc, mcc) == 0) {
            snprintf(out, cap, "%s", k_countries[i].country);
            return;
        }
    }
}

void cs_plmn_operator(const char *plmn, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!plmn) return;
    for (size_t i = 0; i < CS_ARRAY_LEN(k_plmns); i++) {
        if (strcmp(k_plmns[i].plmn, plmn) == 0) {
            snprintf(out, cap, "%s", k_plmns[i].name);
            return;
        }
    }
}

static void nr_arfcn_to_mhz_d(int arfcn, double *mhz)
{
    if (arfcn < 600000) {
        *mhz = (double)arfcn * 0.005;
    } else if (arfcn < 2016667) {
        *mhz = 3000.0 + (double)(arfcn - 600000) * 0.015;
    } else {
        *mhz = 24250.0 + (double)(arfcn - 2016667) * 0.060;
    }
}

bool cs_nr_arfcn_to_hz(int arfcn, uint64_t *hz)
{
    if (!hz || arfcn <= 0) return false;
    double mhz;
    nr_arfcn_to_mhz_d(arfcn, &mhz);
    if (mhz < 100.0 || mhz > 100000.0) return false;
    *hz = (uint64_t)(mhz * 1e6 + 0.5);
    return true;
}

int cs_nr_band_from_arfcn(int arfcn)
{
    for (size_t i = 0; i < CS_ARRAY_LEN(k_nr_bands); i++) {
        if (arfcn >= k_nr_bands[i].arfcn_low && arfcn <= k_nr_bands[i].arfcn_high)
            return k_nr_bands[i].band;
    }
    return 0;
}

bool cs_nr_band_info(int band, double *dl_low, double *dl_high, double *ul_offset, bool *tdd)
{
    for (size_t i = 0; i < CS_ARRAY_LEN(k_nr_bands); i++) {
        if (k_nr_bands[i].band == band) {
            if (dl_low) *dl_low = k_nr_bands[i].dl_low_mhz;
            if (dl_high) *dl_high = k_nr_bands[i].dl_high_mhz;
            if (ul_offset) *ul_offset = k_nr_bands[i].ul_offset_mhz;
            if (tdd) *tdd = k_nr_bands[i].tdd;
            return true;
        }
    }
    return false;
}

bool cs_nr_arfcn_to_ul_hz(int arfcn, int band, uint64_t *ul_hz)
{
    if (!ul_hz) return false;
    uint64_t dl_hz;
    if (!cs_nr_arfcn_to_hz(arfcn, &dl_hz)) return false;
    if (band <= 0) band = cs_nr_band_from_arfcn(arfcn);
    double dl_low, dl_high, off;
    bool tdd;
    if (band > 0 && cs_nr_band_info(band, &dl_low, &dl_high, &off, &tdd)) {
        (void)dl_low;
        (void)dl_high;
        if (tdd) {
            *ul_hz = dl_hz;
        } else {
            double ul = (double)dl_hz / 1e6 + off;
            if (ul > 0) *ul_hz = (uint64_t)(ul * 1e6 + 0.5);
            else *ul_hz = dl_hz;
        }
        return true;
    }
    *ul_hz = dl_hz;
    return true;
}

int cs_lte_band_from_earfcn(int earfcn)
{
    for (size_t i = 0; i < CS_ARRAY_LEN(k_lte_bands); i++) {
        const LteBand *b = &k_lte_bands[i];
        double f = b->dl_low_mhz + (double)(earfcn - b->dl_offset) * 0.1;
        if (earfcn >= b->dl_offset && f >= b->dl_low_mhz - 0.05 && f <= b->dl_high_mhz + 0.05)
            return b->band;
    }
    return 0;
}

bool cs_lte_earfcn_to_hz(int earfcn, int band_hint, uint64_t *dl_hz, uint64_t *ul_hz)
{
    if (earfcn <= 0) return false;
    int band = band_hint > 0 ? band_hint : cs_lte_band_from_earfcn(earfcn);
    if (band <= 0) return false;
    for (size_t i = 0; i < CS_ARRAY_LEN(k_lte_bands); i++) {
        const LteBand *b = &k_lte_bands[i];
        if (b->band != band) continue;
        double dl = b->dl_low_mhz + (double)(earfcn - b->dl_offset) * 0.1;
        double ul = b->tdd ? dl : b->ul_low_mhz + (double)(earfcn - b->dl_offset) * 0.1;
        if (dl <= 0 || (ul <= 0 && !b->tdd)) return false;
        if (dl_hz) *dl_hz = (uint64_t)(dl * 1e6 + 0.5);
        if (ul_hz) *ul_hz = (uint64_t)(ul * 1e6 + 0.5);
        return true;
    }
    return false;
}

void cs_format_mhz_value(uint64_t hz, char *out, size_t cap, int decimals)
{
    snprintf(out, cap, "%.*f", decimals, (double)hz / 1e6);
}
