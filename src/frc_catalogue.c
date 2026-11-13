/*
 * frc_catalogue.c - the eclipse catalogue and the position <-> time mapping.
 *
 * Implements SPEC S2.1 (time values: frc_time_from_ms and friends), S2.2
 * (position from time and back), S7.2 "Other series" (the anchor rule of
 * frc_catalogue_init) and S14 (the Saros 141 table).
 * Section 3 of frc_internal.h is implemented here.
 *
 * THE MODEL
 *   The catalogue is E[0] < E[1] < ... < E[n], n = count - 1. Eclipse k is
 *   position k; in between, position is linear in time:
 *
 *       position(t) = k + (t - E[k]) / I_k     E[k] <= t < E[k+1],  I_k = E[k+1] - E[k]
 *       time(k + r/d) = E[k] + r * I_k / d
 *
 *   Each interval keeps its own real length. There is no mean Saros anywhere
 *   in this file (SPEC pitfall 2).
 *
 * INVARIANTS
 *   - The table stores Unix MILLISECONDS (the published precision); every
 *     instant the API returns is Unix MICROSECONDS = ms * 1000.
 *   - A position is kept exact as ordinal + num/den with den = I_k in
 *     microseconds and 0 <= num < den. Nothing is rounded on the way in.
 *   - On the way out an instant is the FIRST MICROSECOND AT OR AFTER the exact
 *     rational instant: E[k] + ceil(r * I_k / d). The product needs up to 128
 *     bits (frc_i_muldiv_ceil); the quotient is at most I_k because r < d.
 *   - Outside [E[0], E[n]] nothing is defined: FRC_ERR_RANGE / FRC_TIME_NONE,
 *     never an extrapolation. The closing instant E[n] belongs to the last
 *     interval and reads as one microsecond before itself.
 */
#include "frc_internal.h"

/* ------------------------------------------------------------------------- */
/* Solar Saros 141 (SPEC S14)                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Greatest eclipse of each member, Unix milliseconds (UTC). Ordinal 0 is
 * 1613-05-19, ordinal 69 is 2857-06-13. These are the values of SPEC S14 (and
 * of F.defaults.unixTimestamps in the reference library) with the decimal
 * point removed; tests/test_foundation.c checks all 70 against the oracle.
 * Ordinals 0-19 are Delta-T estimates, 20-22 exact, 23-69 assume
 * TAI - UTC = 37 s (SPEC S2.1, pitfall 8).
 */
#define SAROS_141_COUNT 70
static const int64_t SAROS_141_MS[SAROS_141_COUNT] FRC_PROGMEM = {
    -11253795486968LL, -10684827340124LL, -10115859492780LL,  -9546891859112LL, /*  0 */
     -8977924443862LL,  -8408957014511LL,  -7839989594977LL,  -7271021955471LL, /*  4 */
     -6702054209003LL,  -6133086036449LL,  -5564117571512LL,  -4995148587324LL, /*  8 */
     -4426179194600LL,  -3857209183600LL,  -3288238731185LL,  -2719267740393LL, /* 12 */
     -2150296298803LL,  -1581324369140LL,  -1012352078953LL,   -443379486426LL, /* 16 */
       125593319816LL,    694566278816LL,   1263539192816LL,   1832512069816LL, /* 20 */
      2401484716816LL,   2970457153816LL,   3539429150816LL,   4108400821816LL, /* 24 */
      4677371985816LL,   5246342709816LL,   5815312911816LL,   6384282725816LL, /* 28 */
      6953252120816LL,   7522221116816LL,   8091189841816LL,   8660158322816LL, /* 32 */
      9229126685816LL,   9798094931816LL,  10367063272816LL,  10936031708816LL, /* 36 */
     11505000323816LL,  12073969225816LL,  12642938471816LL,  13211908155816LL, /* 40 */
     13780878253816LL,  14349848916816LL,  14918820110816LL,  15487791887816LL, /* 44 */
     16056764232816LL,  16625737153816LL,  17194710612816LL,  17763684496816LL, /* 48 */
     18332658871816LL,  18901633551816LL,  19470608536816LL,  20039583596816LL, /* 52 */
     20608558814816LL,  21177533934816LL,  21746508965816LL,  22315483725816LL, /* 56 */
     22884458266816LL,  23453432422816LL,  24022406224816LL,  24591379597816LL, /* 60 */
     25160352610816LL,  25729325154816LL,  26298297370816LL,  26867269191816LL, /* 64 */
     27436240807816LL,  28005212035816LL                                        /* 68 */
};

/* The descriptor is four small fields that callers read directly, so it is a
 * plain const object (flash on ESP32 / ARM / wasm); only the table it points
 * to carries FRC_PROGMEM. Anchor 20 is the first eclipse of Alpha 6 (S7.2). */
const frc_catalogue_t FRC_SAROS_141 = {SAROS_141_MS, SAROS_141_COUNT, 141, 20};

/* ------------------------------------------------------------------------- */
/* Building a catalogue for another series                                    */
/* ------------------------------------------------------------------------- */

/* 1972-01-01T00:00:00Z, the start of the UTC era, in Unix milliseconds. */
#define UTC_ERA_START_MS 63072000000LL

/*
 * SPEC S7.2 "Other series": the sync is anchored at the series' first Alpha
 * of the UTC era, the first ordinal A = 0 (mod 4) whose eclipse falls on or
 * after 1972-01-01. A series that ended before 1972 (or has no such eclipse)
 * falls back to the last multiple of 4 that still starts a full Alpha
 * (A + 4 <= count - 1), and a series too short for one Alpha to 0.
 */
static uint16_t choose_anchor(const int64_t *instants_ms, uint16_t count)
{
    uint32_t a; /* wider than uint16_t so a += 4 cannot wrap near 65535 */
    for (a = 0; a < count; a += 4) {
        if (FRC_PGM_I64(&instants_ms[a]) >= UTC_ERA_START_MS) {
            return (uint16_t)a;
        }
    }
    if (count >= 5) {
        return (uint16_t)(((count - 5u) / 4u) * 4u);
    }
    return 0;
}

frc_status_t frc_catalogue_init(frc_catalogue_t *cat, const int64_t *instants_ms,
                                uint16_t count, uint16_t series)
{
    int64_t prev;
    uint16_t i;

    if (cat == NULL || instants_ms == NULL || count < 2) {
        return FRC_ERR_ARG;
    }
    /* Strictly increasing, and every instant must survive the * 1000 to
     * microseconds. Checking the two ends of an increasing table covers all. */
    prev = FRC_PGM_I64(&instants_ms[0]);
    if (prev < -FRC_I_MS_LIMIT) {
        return FRC_ERR_ARG;
    }
    for (i = 1; i < count; i++) {
        int64_t e = FRC_PGM_I64(&instants_ms[i]);
        if (e <= prev) {
            return FRC_ERR_ARG;
        }
        prev = e;
    }
    if (prev > FRC_I_MS_LIMIT) {
        return FRC_ERR_ARG;
    }

    cat->instants_ms = instants_ms;
    cat->count = count;
    cat->series = series;
    cat->anchor = choose_anchor(instants_ms, count);
    return FRC_OK;
}

/* ------------------------------------------------------------------------- */
/* Edges                                                                      */
/* ------------------------------------------------------------------------- */

frc_time_t frc_catalogue_instant(const frc_catalogue_t *cat, uint16_t ordinal)
{
    if (!frc_i_cat_ok(cat) || ordinal >= cat->count) {
        return FRC_TIME_NONE;
    }
    return frc_i_cat_time(cat, ordinal);
}

frc_time_t frc_catalogue_first(const frc_catalogue_t *cat)
{
    return frc_catalogue_instant(cat, 0);
}

frc_time_t frc_catalogue_last(const frc_catalogue_t *cat)
{
    if (!frc_i_cat_ok(cat)) {
        return FRC_TIME_NONE;
    }
    return frc_i_cat_time(cat, (uint16_t)(cat->count - 1));
}

int frc_catalogue_contains(const frc_catalogue_t *cat, frc_time_t t)
{
    if (!frc_i_cat_ok(cat)) {
        return 0;
    }
    return t >= frc_i_cat_time(cat, 0) && t <= frc_i_cat_time(cat, (uint16_t)(cat->count - 1));
}

/* ------------------------------------------------------------------------- */
/* Time values (S2.1)                                                         */
/* ------------------------------------------------------------------------- */

/*
 * FRC_TIME_NONE and FRC_TIME_FROM_MS / _S as functions: a Swift or
 * WebAssembly client cannot see a C macro. The conversions are checked, which
 * the macros are not: a value whose microseconds do not fit int64 has no
 * instant.
 */
int frc_time_is_none(frc_time_t t)
{
    return t == FRC_TIME_NONE;
}

/* value * scale, or FRC_TIME_NONE when |value| > limit = INT64_MAX / scale. */
static frc_time_t scaled_time(int64_t value, int64_t scale, int64_t limit)
{
    if (value > limit || value < -limit) {
        return FRC_TIME_NONE;
    }
    return value * scale;
}

frc_time_t frc_time_from_ms(int64_t unix_ms)
{
    return scaled_time(unix_ms, 1000, FRC_I_MS_LIMIT);
}

frc_time_t frc_time_from_s(int64_t unix_s)
{
    return scaled_time(unix_s, FRC_US_PER_SECOND, INT64_MAX / FRC_US_PER_SECOND);
}

/* ------------------------------------------------------------------------- */
/* Position from time (S2.2)                                                  */
/* ------------------------------------------------------------------------- */

frc_status_t frc_position_at(const frc_catalogue_t *cat, frc_time_t t, frc_position_t *out)
{
    uint16_t lo, hi;

    if (!frc_i_cat_ok(cat) || out == NULL) {
        return FRC_ERR_ARG;
    }
    lo = 0;
    hi = (uint16_t)(cat->count - 1);
    if (t < frc_i_cat_time(cat, lo) || t > frc_i_cat_time(cat, hi)) {
        return FRC_ERR_RANGE;
    }
    /* Binary search for the interval: afterwards E[lo] <= t and (t < E[lo+1]
     * or t is the closing instant, which stays in the last interval). */
    while (lo + 1 < hi) {
        uint16_t mid = (uint16_t)((lo + hi) / 2);
        if (frc_i_cat_time(cat, mid) <= t) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    out->ordinal = lo;
    out->den = frc_i_cat_interval(cat, lo);
    /* t - E[lo] is in [0, I_k]; unsigned so a wide interval cannot overflow */
    out->num = (uint64_t)t - (uint64_t)frc_i_cat_time(cat, lo);
    if (out->num == out->den) {
        /* Only the closing instant gets here. It reads as one microsecond
         * earlier, so 0 <= num < den holds for every position (S2.2: the
         * last cycle is clipped AT E[n], nothing starts there). */
        out->num = out->den - 1;
    }
    return FRC_OK;
}

#ifndef FRC_NO_FLOAT
double frc_frac_to_double(frc_frac_t fraction)
{
    return FRC_FRAC_TO_DOUBLE(fraction); /* 32 bits times 2^-32: exact */
}

double frc_position_to_double(const frc_position_t *pos)
{
    if (pos == NULL || pos->den == 0) {
        return 0.0;
    }
    /* num, den < 2^53 for any real interval, so both convert exactly and the
     * only rounding is the division and the final sum: about 1e-14 Saros. */
    return (double)pos->ordinal + (double)pos->num / (double)pos->den;
}
#endif

/* ------------------------------------------------------------------------- */
/* Time from position (S2.2)                                                  */
/* ------------------------------------------------------------------------- */

frc_time_t frc_i_instant(const frc_catalogue_t *cat, uint64_t k, uint64_t r, uint64_t d)
{
    uint64_t last = (uint64_t)cat->count - 1;
    uint64_t offset;

    if (k > last) {
        return FRC_TIME_NONE;
    }
    if (k == last) {
        /* position count-1 exactly is the closing instant; a hair more is outside */
        return r == 0 ? frc_i_cat_time(cat, (uint16_t)k) : FRC_TIME_NONE;
    }
    if (r == 0) {
        return frc_i_cat_time(cat, (uint16_t)k);
    }
    /* ceil(r * I_k / d) <= I_k because r < d, so it fits and the sum cannot
     * pass E[k+1]. Added as unsigned: I_k may exceed INT64_MAX in a wide table. */
    offset = frc_i_muldiv_ceil(r, frc_i_cat_interval(cat, (uint16_t)k), d);
    return (frc_time_t)((uint64_t)frc_i_cat_time(cat, (uint16_t)k) + offset);
}

frc_time_t frc_time_at_position(const frc_catalogue_t *cat, uint64_t n, uint64_t d)
{
    if (!frc_i_cat_ok(cat) || d == 0) {
        return FRC_TIME_NONE;
    }
    /* Split n/d into ordinal and proper fraction first (SPEC S11): n % d < d
     * is what keeps the 128-bit product's quotient inside 64 bits. */
    return frc_i_instant(cat, n / d, n % d, d);
}
