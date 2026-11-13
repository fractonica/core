/*
 * test_spec.c - SPEC.md read as its reader would check it.
 *
 * Every number and every text below is copied by hand from SPEC.md 2.2.0 and
 * asked of the PUBLIC API only (fractonica.h): no internal header, no oracle
 * vector. It is the cross-module end-to-end test: a date here goes through
 * the catalogue, the cycle fraction, the notation and the text writer at once.
 *
 *   S12    test vectors A, B, C and the anchor (glyphs, odometers, counters,
 *          thresholds, the solar sync)
 *   S15.6  dates, the stamp, the Helion pair and the span of vector A
 *   S16.5  exact periods, frequencies and lengths
 * and, because they cost nothing once the helpers exist, the tables and
 * worked examples of S3.3, S4.1, S7.2, S7.5, S9.1, S15.2 - S15.5, S16.1 - S16.3.
 * The QUICK START of fractonica.h is compiled and run as written.
 *
 * HOW THE SPEC'S ROUNDED FIGURES ARE COMPARED
 *   SPEC prints instants to the millisecond; the core returns the first
 *   microsecond at or after the exact instant. An instant matches when it
 *   rounds to SPEC's millisecond. Phases (9 decimals), drifts (0.1 ms) and
 *   positions (12 decimals) are compared the same way, in integers.
 */
#include "frc_test.h"
#include "fractonica.h"

#include <math.h>
#include <stdlib.h>

static const frc_catalogue_t *const CAT = &FRC_SAROS_141;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

/* Microseconds rounded to the nearest millisecond (a half away from zero). */
static int64_t nearest_ms(int64_t us)
{
    return us >= 0 ? (us + 500) / 1000 : -((-us + 500) / 1000);
}

/* Microseconds rounded to units of 0.1 ms: SPEC prints a drift as "-48469.9899 s". */
static int64_t nearest_tenth_ms(int64_t us)
{
    return us >= 0 ? (us + 50) / 100 : -((-us + 50) / 100);
}

/* "2026-10-07T01:34:09.826Z" for an instant rounded to the millisecond. */
static const char *iso_of(frc_time_t t)
{
    static char text[40];
    frc_civil_t civil;
    frc_civil_from_time(FRC_TIME_FROM_MS(nearest_ms(t)), 0, &civil);
    if (frc_format_civil(&civil, 0, text, sizeof text) < 0) {
        text[0] = '\0';
    }
    return text;
}

/* 1 when a 0.32 phase is SPEC's 9-decimal figure. The field is the floor of
 * 2^32 times the phase (so up to 2.4e-10 below it) and SPEC rounds to 1e-9:
 * in units of 1e-10 the field lies in (10 e - 8, 10 e + 5]. */
static int phase_is(frc_frac_t phase, int64_t expected_e9)
{
    /* floor(phase * 10^10 / 2^32) in two steps of 10^5, so nothing passes 64 bits */
    uint64_t scaled = (uint64_t)phase * 100000u;
    uint64_t whole = scaled >> 32, rest = scaled & 0xFFFFFFFFu;
    int64_t e10 = (int64_t)(whole * 100000u + ((rest * 100000u) >> 32));
    return e10 >= expected_e9 * 10 - 8 && e10 <= expected_e9 * 10 + 5;
}

/* The fraction num/den to 12 decimals, rounded half up, as an integer. */
static uint64_t fraction_e12(uint64_t num, uint64_t den)
{
    uint64_t digits = 0, rem = num;
    int i;
    /* long division one decimal at a time: rem < den < 2^50, so rem * 10 fits */
    for (i = 0; i < 12; i++) {
        rem *= 10;
        digits = digits * 10 + rem / den;
        rem %= den;
    }
    return digits + (rem * 2 >= den ? 1 : 0);
}

/*
 * The first 17 significant digits of numerator / denominator * 10^shift10,
 * rounded half to even: the frc_sci_t of an exact fraction, by long division.
 * `numerator` is a decimal string of any length (SPEC prints one of 51
 * digits); denominator < 1.8e18 so that rem * 10 + 9 fits 64 bits.
 */
static frc_sci_t sci_of_fraction(const char *numerator, uint64_t denominator, int shift10)
{
    frc_sci_t out;
    const char *p = numerator;
    uint64_t rem = 0, mantissa = 0;
    int weight = (int)strlen(numerator); /* power of ten of the NEXT quotient digit, plus 1 */
    int have = 0;
    int next, sticky;

    while (have < FRC_SCI_DIGITS) {
        uint64_t q;
        rem = rem * 10 + (uint64_t)(*p ? *p++ - '0' : 0);
        q = rem / denominator;
        rem %= denominator;
        weight--;
        if (have > 0 || q != 0) {
            mantissa = mantissa * 10 + q;
            have++;
        }
    }
    /* the 18th digit and whether anything follows it decide the rounding */
    rem = rem * 10 + (uint64_t)(*p ? *p++ - '0' : 0);
    next = (int)(rem / denominator);
    rem %= denominator;
    sticky = (rem != 0);
    while (*p) {
        sticky |= (*p++ != '0');
    }
    if (next > 5 || (next == 5 && (sticky || (mantissa & 1) != 0))) {
        mantissa++;
        if (mantissa == UINT64_C(100000000000000000)) { /* 99..9 rounded up */
            mantissa /= 10;
            weight++;
        }
    }
    out.mantissa = mantissa;
    out.exp10 = (int16_t)(weight + shift10);
    return out;
}

#define CHECK_SCI(actual, numerator, denominator, shift10)                      \
    do {                                                                        \
        frc_sci_t spec_sci = sci_of_fraction((numerator), (denominator), (shift10)); \
        CHECK_EQ_U64((actual).mantissa, spec_sci.mantissa);                     \
        CHECK_EQ_I64((actual).exp10, spec_sci.exp10);                           \
        CHECK_NOTE("exact value %s / %" PRIu64, (numerator), (uint64_t)(denominator)); \
    } while (0)

static frc_interval_t reference_interval(void)
{
    frc_interval_t interval;
    interval.num = FRC_INTERVAL_REFERENCE_MS;
    interval.den = 1;
    return interval;
}

/* ------------------------------------------------------------------------- */
/* S12  Test vectors: glyphs, odometers, counters and thresholds              */
/* ------------------------------------------------------------------------- */

typedef struct level_vector {
    uint8_t  level;
    uint16_t index;       /* 2-stroke glyph                                   */
    int16_t  value;
    int8_t   derivative;
    uint8_t  dotted;
    uint16_t digits[2];   /* odometer, 2 x 2-stroke                           */
    uint64_t cycle;       /* 0 when SPEC does not print it                    */
    uint16_t local;
    uint16_t siblings;
    uint8_t  next_name;   /* frc_quarter_t                                    */
    int64_t  next_ms;
    const char *next_clock; /* "12:56:12.428Z", the time of day SPEC prints   */
} level_vector_t;

static void check_level(const char *name, frc_time_t t, const level_vector_t *v)
{
    frc_glyph_t glyph;
    frc_odometer_t odometer;
    frc_cycle_t cycle;
    frc_threshold_t next;

    CHECK(frc_glyph_at(CAT, t, v->level, 2, &glyph) == FRC_OK);
    CHECK_EQ_U64(glyph.index, v->index);
    CHECK_EQ_I64(glyph.value, v->value);
    CHECK_EQ_I64(glyph.derivative, v->derivative);
    CHECK_EQ_U64(glyph.dotted, v->dotted);
    CHECK_NOTE("vector %s level %d", name, v->level);

    CHECK(frc_odometer_at(CAT, t, v->level, 2, 2, &odometer) == FRC_OK);
    CHECK_EQ_U64(odometer.count, 2);
    CHECK_EQ_U64(odometer.states, 16);
    CHECK_EQ_U64(odometer.digit[0], v->digits[0]);
    CHECK_EQ_U64(odometer.digit[1], v->digits[1]);
    CHECK_NOTE("vector %s level %d", name, v->level);
    CHECK(odometer.next_change > t);

    CHECK(frc_cycle_at(CAT, t, v->level, &cycle) == FRC_OK);
    if (v->cycle != 0) {
        CHECK_EQ_U64(cycle.cycle, v->cycle);
    }
    CHECK_EQ_U64(cycle.local, v->local);
    CHECK_EQ_U64(cycle.siblings, v->siblings);
    CHECK_EQ_U64(cycle.siblings, frc_siblings(v->level));
    CHECK(cycle.start <= t && t < cycle.end && !cycle.clipped);
    CHECK_NOTE("vector %s level %d", name, v->level);

    CHECK(frc_next_threshold(CAT, t, v->level, &next) == FRC_OK);
    CHECK_EQ_U64(next.name, v->next_name);
    CHECK_EQ_I64(nearest_ms(next.time), v->next_ms);
    /* SPEC adds the time of day: the tail of the ISO string */
    CHECK_STR(iso_of(next.time) + 11, v->next_clock);
    CHECK_NOTE("vector %s level %d", name, v->level);
}

typedef struct sync_vector {
    int32_t     sync;
    int64_t     start_ms;     /* 0 when SPEC gives the ISO string only         */
    const char *start_iso;    /* NULL when SPEC gives the number only          */
    const char *end_iso;      /* NULL when SPEC does not print it              */
    int64_t     phase_e9;
    int64_t     drift_tenth_ms;
    uint16_t    digits[3];    /* 3 x 2-stroke                                  */
    uint8_t     next_name;
    int64_t     next_ms;
} sync_vector_t;

static void check_sync(const char *name, frc_time_t t, const sync_vector_t *v)
{
    frc_sync_t sync;
    frc_odometer_t odometer;
    int j;

    CHECK(frc_sync_at(CAT, t, &sync) == FRC_OK);
    CHECK_EQ_I64(sync.sync, v->sync);
    if (v->start_ms != 0) {
        CHECK_EQ_I64(nearest_ms(sync.start), v->start_ms);
    }
    if (v->start_iso != NULL) {
        CHECK_STR(iso_of(sync.start), v->start_iso);
    }
    if (v->end_iso != NULL) {
        CHECK_STR(iso_of(sync.end), v->end_iso);
    }
    CHECK(phase_is(sync.phase, v->phase_e9));
    CHECK_NOTE("vector %s: phase field %u, SPEC 0.%09" PRId64, name, (unsigned)sync.phase, v->phase_e9);
    CHECK_EQ_I64(nearest_tenth_ms(sync.drift_us), v->drift_tenth_ms);
    /* S7.1: nominal = E[A] + sync * D * 86400 and drift = syncStart - nominal */
    CHECK_EQ_I64(sync.nominal, frc_catalogue_instant(CAT, 20) + (int64_t)v->sync * 4 * FRC_US_PER_DAY);
    CHECK_EQ_I64(sync.drift_us, sync.start - sync.nominal);
    CHECK_EQ_U64(sync.next.name, v->next_name);
    CHECK_EQ_I64(nearest_ms(sync.next.time), v->next_ms);
    CHECK_NOTE("vector %s", name);

    CHECK(frc_sync_odometer(CAT, t, 2, 3, &odometer) == FRC_OK);
    for (j = 0; j < 3; j++) {
        CHECK_EQ_U64(odometer.digit[j], v->digits[j]);
    }
    CHECK_NOTE("vector %s", name);
}

static void test_s12_vector_a(void)
{
    /* A: 2026-10-08T12:00:00Z (Unix 1791460800, position 22.927850216635) */
    static const level_vector_t depth7 = {6, 13, -3, +1, 0, {13, 0}, 717211, 0, 11,
                                          FRC_END, INT64_C(1791464172428), "12:56:12.428Z"};
    static const level_vector_t depth8 = {7, 5, 3, -1, 1, {5, 4}, 6454906, 7, 9,
                                          FRC_NODE, INT64_C(1791461140937), "12:05:40.937Z"};
    static const sync_vector_t solar = {4820, INT64_C(1791336849826), "2026-10-07T01:34:09.826Z",
                                        "2026-10-11T01:33:59.745Z", 358662585, INT64_C(-484699899),
                                        {5, 11, 13}, FRC_NODE, INT64_C(1791509644786)};
    const frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
    frc_position_t position;
    frc_civil_t civil;
    frc_glyph_t glyph;
    frc_odometer_t odometer;
    frc_sync_t sync;

    TEST_SECTION("S12 vector A");
    CHECK_STR(iso_of(t), "2026-10-08T12:00:00.000Z");
    civil.year = 2026; civil.month = 10; civil.day = 8;
    civil.hour = 12; civil.minute = 0; civil.second = 0; civil.weekday = 0; civil.microsecond = 0;
    CHECK_EQ_I64(frc_time_from_civil(&civil, 0), t);

    CHECK(frc_position_at(CAT, t, &position) == FRC_OK);
    CHECK_EQ_U64(position.ordinal, 22);
    CHECK_EQ_U64(fraction_e12(position.num, position.den), UINT64_C(927850216635));
#ifndef FRC_NO_FLOAT
    CHECK(fabs(frc_position_to_double(&position) - 22.927850216635) < 1e-12);
#endif

    check_level("A", t, &depth7);
    check_level("A", t, &depth8);

    /* 3-stroke index / value at depth 7: 42 / -10 */
    CHECK(frc_glyph_at(CAT, t, 6, 3, &glyph) == FRC_OK);
    CHECK_EQ_U64(glyph.index, 42);
    CHECK_EQ_I64(glyph.value, -10);

    check_sync("A", t, &solar);
    /* digits (2 x 3-stroke): [18, 33] */
    CHECK(frc_sync_odometer(CAT, t, 3, 2, &odometer) == FRC_OK);
    CHECK_EQ_U64(odometer.digit[0], 18);
    CHECK_EQ_U64(odometer.digit[1], 33);
    /* the next threshold as SPEC dates it */
    CHECK(frc_sync_at(CAT, t, &sync) == FRC_OK);
    CHECK_STR(iso_of(sync.next.time), "2026-10-09T01:34:04.786Z");
    CHECK_EQ_U64(sync.quarter, FRC_NODE); /* phase 0.3587 is in the second quarter */

    /* S7.2: per-sync offset in the 2010-2028 interval, N * cycle - D * 86400 = -10.0807 s.
     * start and end are each rounded up to the microsecond: allow one unit of 0.1 ms. */
    CHECK(llabs(nearest_tenth_ms(sync.end - sync.start - 4 * FRC_US_PER_DAY) - (-100807)) <= 1);
}

static void test_s12_vector_b(void)
{
    /* B: 2000-01-01T00:00:00Z (Unix 946684800, position 21.443111640256) */
    static const level_vector_t depth7 = {6, 5, 3, -1, 1, {5, 6}, 0, 9, 11,
                                          FRC_NODE, INT64_C(946687783046), "00:49:43.046Z"};
    static const level_vector_t depth8 = {7, 0, 0, +1, 0, {0, 6}, 0, 3, 9,
                                          FRC_PEAK, INT64_C(946685256803), "00:07:36.803Z"};
    static const sync_vector_t solar = {2375, INT64_C(946369476520), "1999-12-28T08:24:36.520Z", NULL,
                                        912420883, INT64_C(-238432957), {14, 9, 9},
                                        FRC_END, INT64_C(946715066462)};
    const frc_time_t t = FRC_TIME_FROM_S(INT64_C(946684800));
    frc_position_t position;

    TEST_SECTION("S12 vector B");
    CHECK_STR(iso_of(t), "2000-01-01T00:00:00.000Z");
    CHECK(frc_position_at(CAT, t, &position) == FRC_OK);
    CHECK_EQ_U64(position.ordinal, 21);
    CHECK_EQ_U64(fraction_e12(position.num, position.den), UINT64_C(443111640256));
    check_level("B", t, &depth7);
    check_level("B", t, &depth8);
    check_sync("B", t, &solar);
}

static void test_s12_vector_c(void)
{
    /* C: 2061-06-15T18:30:00Z (Unix 2886085800, position 24.851712757369) */
    static const level_vector_t depth7 = {6, 10, -2, -1, 1, {10, 3}, 0, 0, 11,
                                          FRC_VALLEY, INT64_C(2886087806395), "19:03:26.395Z"};
    static const level_vector_t depth8 = {7, 12, -4, 0, 0, {12, 1}, 0, 5, 9,
                                          FRC_END, INT64_C(2886086290651), "18:38:10.651Z"};
    static const sync_vector_t solar = {7987, 0, "2061-06-12T16:31:59.691Z", NULL,
                                        770510076, INT64_C(-810001249), {12, 5, 4},
                                        FRC_END, INT64_C(2886165109343)};
    const frc_time_t t = FRC_TIME_FROM_S(INT64_C(2886085800));
    frc_position_t position;

    TEST_SECTION("S12 vector C");
    CHECK_STR(iso_of(t), "2061-06-15T18:30:00.000Z");
    CHECK(frc_position_at(CAT, t, &position) == FRC_OK);
    CHECK_EQ_U64(position.ordinal, 24);
    CHECK_EQ_U64(fraction_e12(position.num, position.den), UINT64_C(851712757369));
    check_level("C", t, &depth7);
    check_level("C", t, &depth8);
    check_sync("C", t, &solar);
}

static void test_s12_anchor(void)
{
    /* Anchor: Unix 125593320.816 (one second after the anchor eclipse) gives
     * sync 0, drift 0 and digits [0, 0, 0]. */
    const frc_time_t t = FRC_TIME_FROM_MS(INT64_C(125593320816));
    frc_sync_t sync;
    frc_odometer_t odometer;
    frc_pentad_t pentad;
    char text[64];

    TEST_SECTION("S12 anchor");
    /* S7.2: A = 20 at Unix 125593319.816, 1973-12-24 15:01:59.816 UTC */
    CHECK_EQ_U64(CAT->anchor, 20);
    CHECK_EQ_I64(frc_catalogue_instant(CAT, 20), FRC_TIME_FROM_MS(INT64_C(125593319816)));
    CHECK_STR(iso_of(frc_catalogue_instant(CAT, 20)), "1973-12-24T15:01:59.816Z");

    CHECK(frc_sync_at(CAT, t, &sync) == FRC_OK);
    CHECK_EQ_I64(sync.sync, 0);
    CHECK_EQ_I64(sync.drift_us, 0);
    CHECK_EQ_I64(sync.start, frc_catalogue_instant(CAT, 20));
    CHECK(frc_sync_odometer(CAT, t, 2, 3, &odometer) == FRC_OK);
    CHECK(odometer.digit[0] == 0 && odometer.digit[1] == 0 && odometer.digit[2] == 0);

    /* S7.5: the anchor eclipse itself starts Pentad I of sync 0 */
    CHECK(frc_pentad_at(CAT, frc_catalogue_instant(CAT, 20), &pentad) == FRC_OK);
    CHECK(pentad.sync == 0 && pentad.index == 0 && pentad.helion == 1 && pentad.helions == 5);
    CHECK_EQ_I64(pentad.start, frc_catalogue_instant(CAT, 20));
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_FULL, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Pentad I");
}

/* ------------------------------------------------------------------------- */
/* S7.5  Pentads                                                              */
/* ------------------------------------------------------------------------- */

static void test_s7_pentads(void)
{
    /* "Vector A (S12, 1791460800) is in Pentad II (its 2nd Helion) of sync 4820" */
    static const char *const full[4] = {"Pentad I", "Pentad II", "Pentad III", "Hypopentad"};
    static const char *const brief[4] = {"⬠I", "⬠II", "⬠III", "◻"};
    static const char *const plain[4] = {"P1", "P2", "P3", "H"};
    const frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
    frc_pentad_t pentad, part;
    frc_sync_t sync;
    char text[64];
    uint8_t i;

    TEST_SECTION("S7.5 Pentads");
    CHECK(frc_pentad_at(CAT, t, &pentad) == FRC_OK);
    CHECK_EQ_I64(pentad.sync, 4820);
    CHECK_EQ_U64(pentad.index, 1);
    CHECK_EQ_U64(pentad.helion, 2);
    CHECK_EQ_U64(pentad.helions, 5);
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_FULL, 1, text, sizeof text) > 0);
    CHECK_STR(text, "Pentad II, 2nd Helion");
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_SHORT, 1, text, sizeof text) > 0);
    CHECK_STR(text, "⬠II·2");
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_ASCII, 1, text, sizeof text) > 0);
    CHECK_STR(text, "P2.2");

    /* the table: names, short forms, plain forms, 5 + 5 + 5 + 4 Helions that
     * tile the sync */
    CHECK(frc_sync_by_number(CAT, 4820, &sync) == FRC_OK);
    for (i = 0; i < 4; i++) {
        CHECK(frc_pentad_by_number(CAT, 4820, i, &part) == FRC_OK);
        CHECK_EQ_U64(part.helions, i == 3 ? 4 : 5);
        CHECK(frc_format_pentad(&part, FRC_PENTAD_FULL, 0, text, sizeof text) > 0);
        CHECK_STR(text, full[i]);
        CHECK(frc_format_pentad(&part, FRC_PENTAD_SHORT, 0, text, sizeof text) > 0);
        CHECK_STR(text, brief[i]);
        CHECK(frc_format_pentad(&part, FRC_PENTAD_ASCII, 0, text, sizeof text) > 0);
        CHECK_STR(text, plain[i]);
        if (i == 0) {
            CHECK_EQ_I64(part.start, sync.start);
        }
        if (i == 3) {
            CHECK_EQ_I64(part.end, sync.end);
        }
    }
    /* "Pentad II ... starts after the sync's start: 1 day + 1/4 eta". A Pentad
     * is 5 Helions, so that is 4 3/4 Helions = 1 day: true to the "-2.52 s per
     * day" of S7.2 (19 Helions are 4 days - 10.0807 s in this interval). */
    CHECK(frc_pentad_by_number(CAT, 4820, 1, &part) == FRC_OK);
    {
        int64_t five_helions = part.start - sync.start;
        int64_t off_a_day = five_helions - five_helions / 20 - FRC_US_PER_DAY; /* 4.75 eta - 1 day */
        CHECK(llabs(off_a_day - (-2520000)) < 1000);
        /* "5 eta = 25.26 h" */
        CHECK(llabs(five_helions - (int64_t)(25.26 * 3600e6)) < (int64_t)(0.005 * 3600e6));
    }
}

/* ------------------------------------------------------------------------- */
/* S3.3, S4.1  Tables of the hierarchy and of the glyph                       */
/* ------------------------------------------------------------------------- */

static void test_s3_period_lengths(void)
{
    /* S3.3: Level, Name, Unit, P(L), Siblings, Cycle length, and how many
     * significant digits SPEC prints for that length. */
    static const struct {
        uint8_t level; const char *name; const char *unit; uint64_t prefix; uint8_t siblings;
        const char *length; uint8_t digits;
    } rows[] = {
        {0, "α", "Alpha", UINT64_C(1), 0, "72.12 y", 4},
        {1, "β", "Beta", UINT64_C(5), 5, "14.42 y", 4},
        {2, "γ", "Gamma", UINT64_C(65), 13, "1.11 y", 3},
        {3, "δ", "Delta", UINT64_C(325), 5, "81.050 d", 5},
        {4, "ε", "Nuclide", UINT64_C(1625), 5, "16.210 d", 5},
        {5, "ζ", "", UINT64_C(11375), 7, "2.316 d", 4},
        {6, "η", "Helion", UINT64_C(125125), 11, "5.052 h", 4},
        {7, "θ", "", UINT64_C(1126125), 9, "33.683 min", 5},
        {8, "ι", "Preon", UINT64_C(4504500), 4, "8.421 min", 4},
        {9, "κ", "", UINT64_C(22522500), 5, "1.684 min", 4},
        {10, "λ", "Flavon", UINT64_C(292792500), 13, "7.773 s", 4},
        {11, "μ", "", UINT64_C(1463962500), 5, "1.555 s", 4},
        {12, "ν", "Blink", UINT64_C(7319812500), 5, "310.92 ms", 5},
        {13, "ξ", "", UINT64_C(51238687500), 7, "44.42 ms", 4},
    };
    char text[64];
    size_t i;

    TEST_SECTION("S3.3 period lengths");
    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        int unit = frc_unit_of_level(rows[i].level);
        CHECK(frc_format_level_name(rows[i].level, text, sizeof text) > 0);
        CHECK_STR(text, rows[i].name);
        if (rows[i].unit[0] != '\0') {
            CHECK(unit >= 0);
            CHECK(frc_format_unit_name((frc_unit_t)unit, 0, text, sizeof text) > 0);
            CHECK_STR(text, rows[i].unit);
        } else {
            CHECK(unit == -1);
        }
        CHECK_EQ_U64(frc_prefix(rows[i].level), rows[i].prefix);
        CHECK_EQ_U64(frc_siblings(rows[i].level), rows[i].siblings);
        CHECK(frc_format_period(rows[i].level, reference_interval(), rows[i].digits, text, sizeof text) > 0);
        CHECK_STR(text, rows[i].length);
    }
    /* S3.1: S = [5, 13, 5, 5, 7, 11, 9, 4] repeats; S3.4: level 24 is α1, level 49 is β2 */
    {
        static const uint8_t sequence[8] = {5, 13, 5, 5, 7, 11, 9, 4};
        uint8_t level;
        for (level = 0; level < FRC_LEVEL_COUNT; level++) {
            CHECK_EQ_U64(frc_factor(level), sequence[level % 8]);
        }
    }
    CHECK(frc_format_level_name(24, text, sizeof text) > 0);
    CHECK_STR(text, "α1");
    CHECK(frc_format_level_name(49, text, sizeof text) > 0);
    CHECK_STR(text, "β2");
}

static void test_s4_states(void)
{
    /* S4.1 "Two strokes (16 states)": 0 1 2 3 4 3• 2• 1• 0• -1• -2• -3• -4 -3 -2 -1 */
    static const int8_t value2[16] = {0, 1, 2, 3, 4, 3, 2, 1, 0, -1, -2, -3, -4, -3, -2, -1};
    static const uint8_t dotted2[16] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    frc_glyph_t glyph;
    uint16_t index;
    uint8_t i;

    TEST_SECTION("S4.1 states");
    /* d = 2 -> M = 4, d = 3 -> M = 13, d = 4 -> M = 40 */
    CHECK_EQ_U64(frc_glyph_extreme(2), 4);
    CHECK_EQ_U64(frc_glyph_extreme(3), 13);
    CHECK_EQ_U64(frc_glyph_extreme(4), 40);
    CHECK_EQ_U64(frc_glyph_states(2), 16);
    CHECK_EQ_U64(frc_glyph_states(3), 52);
    CHECK_EQ_U64(frc_glyph_states(4), 160);

    for (index = 0; index < 16; index++) {
        CHECK(frc_glyph_from_index(index, 2, &glyph) == FRC_OK);
        CHECK_EQ_I64(glyph.value, value2[index]);
        CHECK_EQ_U64(glyph.dotted, dotted2[index]);
        /* S4.2: v = sum of t_i * 3^i, i = 0 the least significant trit */
        CHECK_EQ_I64(glyph.trit[0] + 3 * glyph.trit[1], value2[index]);
    }
    /* "Three strokes (52 states): 0 ... 13, 12• ... -12•, -13, -12 ... -1" */
    for (index = 0; index < 52; index++) {
        int expected = index <= 13 ? index : index <= 39 ? 26 - index : index - 52;
        CHECK(frc_glyph_from_index(index, 3, &glyph) == FRC_OK);
        CHECK_EQ_I64(glyph.value, expected);
        CHECK_EQ_U64(glyph.dotted, (index > 13 && index < 39) ? 1 : 0);
        CHECK_EQ_I64(glyph.derivative, (index == 13 || index == 39) ? 0 : (index < 13 || index > 39) ? 1 : -1);
        for (i = 0; i < 3; i++) {
            CHECK(glyph.trit[i] >= -1 && glyph.trit[i] <= 1);
        }
        CHECK_EQ_I64(glyph.trit[0] + 3 * glyph.trit[1] + 9 * glyph.trit[2], expected);
        /* "A descending family adds 2M before the modulo" */
        CHECK_EQ_U64(frc_glyph_index_descending(index, 3), (index + 26) % 52);
    }
}

/* ------------------------------------------------------------------------- */
/* S9.1  Geometry                                                             */
/* ------------------------------------------------------------------------- */

#define MAX_OPS 32
typedef struct recorded_path {
    int count;
    frc_path_op_t op[MAX_OPS];
    int16_t pts[MAX_OPS][6];
} recorded_path_t;

static void record_op(void *ctx, frc_path_op_t op, const int16_t *pts)
{
    recorded_path_t *path = (recorded_path_t *)ctx;
    int n = (op == FRC_PATH_CUBIC) ? 6 : (op == FRC_PATH_DOT) ? 3 : 2;
    int i;
    if (path->count < MAX_OPS) {
        path->op[path->count] = op;
        for (i = 0; i < 6; i++) {
            path->pts[path->count][i] = (i < n) ? pts[i] : 0;
        }
    }
    path->count++;
}

/* 1 when the recorded path has `op` with these points (design units * 100) somewhere. */
static int path_has(const recorded_path_t *path, frc_path_op_t op, int a, int b, int c, int d, int e, int f)
{
    const int want[6] = {a, b, c, d, e, f};
    int i, j;
    for (i = 0; i < path->count && i < MAX_OPS; i++) {
        int same = (path->op[i] == op);
        for (j = 0; j < 6 && same; j++) {
            same = (path->pts[i][j] == want[j]);
        }
        if (same) {
            return 1;
        }
    }
    return 0;
}

static void test_s9_geometry(void)
{
    recorded_path_t path;
    frc_glyph_t glyph;
    int16_t min_x, min_y, max_x, max_y;
    uint8_t strokes;

    TEST_SECTION("S9.1 geometry");
    /* bounds of a d-stroke glyph: x in [-2.15, 2.15], y in [-2.2, 3d + 0.15] */
    for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
        frc_glyph_bounds(strokes, &min_x, &min_y, &max_x, &max_y);
        CHECK(min_x == -215 && max_x == 215 && min_y == -220 && max_y == 300 * strokes + 15);
    }

    /* A 2-stroke glyph of value 5 = -1 + 2 * 3: trit 0 is -1 (junction y = 0),
     * trit 1 is +1... no: 5 > M. Take value 2 = -1 + 1 * 3: trits (-1, +1), index 2, rising. */
    CHECK(frc_glyph_from_index(2, 2, &glyph) == FRC_OK);
    CHECK(glyph.value == 2 && glyph.trit[0] == -1 && glyph.trit[1] == 1 && !glyph.dotted);
    path.count = 0;
    frc_glyph_path(&glyph, record_op, &path);
    /* stem: M 0 3i L 0 3i+3 for each trit */
    CHECK(path_has(&path, FRC_PATH_MOVE, 0, 0, 0, 0, 0, 0) && path_has(&path, FRC_PATH_LINE, 0, 300, 0, 0, 0, 0));
    CHECK(path_has(&path, FRC_PATH_MOVE, 0, 300, 0, 0, 0, 0) && path_has(&path, FRC_PATH_LINE, 0, 600, 0, 0, 0, 0));
    /* trit -1 at y = 0: M 0 y C 0 y 0 y-1 -1 y-1 C -2 y-1 -2 y -2 y */
    CHECK(path_has(&path, FRC_PATH_CUBIC, 0, 0, 0, -100, -100, -100));
    CHECK(path_has(&path, FRC_PATH_CUBIC, -200, -100, -200, 0, -200, 0));
    /* trit +1 at y = 3: M 0 y C 0 y 0 y+1 1 y+1 C 2 y+1 2 y 2 y */
    CHECK(path_has(&path, FRC_PATH_CUBIC, 0, 300, 0, 400, 100, 400));
    CHECK(path_has(&path, FRC_PATH_CUBIC, 200, 400, 200, 300, 200, 300));
    /* stem 2 x (M, L) + strokes 2 x (M, C, C), no dot while rising */
    CHECK_EQ_I64(path.count, 10);
    CHECK(!path_has(&path, FRC_PATH_DOT, 0, FRC_GLYPH_DOT_Y, FRC_GLYPH_DOT_RADIUS, 0, 0, 0));

    /* value 0 falling (index 8 of 16): two bars M -1 y L 1 y, and the dot at (0, -1.75) r 0.3 */
    CHECK(frc_glyph_from_index(8, 2, &glyph) == FRC_OK);
    CHECK(glyph.value == 0 && glyph.dotted);
    path.count = 0;
    frc_glyph_path(&glyph, record_op, &path);
    CHECK(path_has(&path, FRC_PATH_MOVE, -100, 0, 0, 0, 0, 0) && path_has(&path, FRC_PATH_LINE, 100, 0, 0, 0, 0, 0));
    CHECK(path_has(&path, FRC_PATH_MOVE, -100, 300, 0, 0, 0, 0) && path_has(&path, FRC_PATH_LINE, 100, 300, 0, 0, 0, 0));
    CHECK(path_has(&path, FRC_PATH_DOT, 0, -175, 30, 0, 0, 0));
    CHECK_EQ_I64(path.count, 9); /* stem 4 + bars 4 + the dot */
}

/* ------------------------------------------------------------------------- */
/* S15  Notation                                                              */
/* ------------------------------------------------------------------------- */

static void test_s15_units(void)
{
    /* S15.1: Unit, Symbol, Depth, Made of */
    static const struct { const char *name; const char *symbol; uint8_t depth; uint8_t parts; } rows[FRC_UNIT_COUNT] = {
        {"Alpha", "α", 1, 5},     {"Beta", "β", 2, 13},    {"Gamma", "γ", 3, 5},
        {"Delta", "δ", 4, 5},     {"Nuclide", "ε", 5, 77}, {"Helion", "η", 7, 36},
        {"Preon", "ι", 9, 65},    {"Flavon", "λ", 11, 25}, {"Blink", "ν", 13, 0},
    };
    /* S15.2: the fracton ordinals 1..18 */
    static const char *const ordinals[18] = {
        "Monofracton", "Difracton", "Trifracton", "Tetrafracton", "Pentafracton", "Hexafracton",
        "Heptafracton", "Octafracton", "Nonafracton", "Decafracton", "Didecafracton", "Dodecafracton",
        "Tridecafracton", "Tetradecafracton", "Pentadecafracton", "Hexadecafracton", "Heptadecafracton",
        "Octadecafracton"};
    static const char *const quarters[4] = {"Peak", "Node", "Valley", "End"};
    char text[64];
    uint8_t i;

    TEST_SECTION("S15.1 - S15.2 units and ordinals");
    for (i = 0; i < FRC_UNIT_COUNT; i++) {
        CHECK(frc_format_unit_name((frc_unit_t)i, 0, text, sizeof text) > 0);
        CHECK_STR(text, rows[i].name);
        CHECK(frc_format_unit_symbol((frc_unit_t)i, text, sizeof text) > 0);
        CHECK_STR(text, rows[i].symbol);
        CHECK_EQ_U64(frc_unit_level((frc_unit_t)i) + 1u, rows[i].depth);
        CHECK_EQ_U64(frc_unit_parts((frc_unit_t)i), rows[i].parts);
    }
    for (i = 0; i < 18; i++) {
        CHECK(frc_format_ordinal((uint16_t)(i + 1), text, sizeof text) > 0);
        CHECK_STR(text, ordinals[i]);
    }
    for (i = 0; i < 4; i++) {
        CHECK(frc_format_quarter((frc_quarter_t)i, 0, text, sizeof text) > 0);
        CHECK_STR(text, quarters[i]);
    }
    /* "Saros 141 needs ordinals up to 18 for Alpha" */
    CHECK_EQ_U64(frc_cycle_count(CAT, 0), 18);
}

static void test_s15_date_forms(void)
{
    /* S15.3, the table of forms */
    frc_date_t date, parsed;
    frc_unit_t finest;
    char text[200];

    TEST_SECTION("S15.3 date forms");
    date.alpha = 5; date.beta = 4; date.gamma = 2; date.delta = 3;
    date.epsilon = 2; date.eta = 5; date.iota = 5; date.lambda = 5; date.nu = 5;
    date.quarter[0] = FRC_PEAK; date.quarter[1] = FRC_VALLEY; date.quarter[2] = FRC_END; date.quarter[3] = FRC_NODE;

    CHECK(frc_format_date(&date, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text) > 0);
    CHECK_STR(text, "Alpha Pentafracton Peak, Beta Tetrafracton Valley, Gamma Difracton End, "
                    "Delta Trifracton Node, ε#2 5η 5ι 5λ 5ν");
    CHECK(frc_format_date(&date, FRC_DATE_SHORT, FRC_UNIT_PREON, text, sizeof text) > 0);
    CHECK_STR(text, "Delta Trifracton Node ε#2 5η 5ι");
    CHECK(frc_format_date(&date, FRC_DATE_LETTERS, FRC_UNIT_FLAVON, text, sizeof text) > 0);
    CHECK_STR(text, "α#5 β#4 γ#2 δ#3 ε#2 5η 5ι 5λ");
    CHECK(frc_format_date(&date, FRC_DATE_SHORT_LETTERS, FRC_UNIT_PREON, text, sizeof text) > 0);
    CHECK_STR(text, "δ#3 ε#2 5η 5ι");

    /* "a parser reading a date ignores" the Pentad it leads with */
    CHECK(frc_parse_date("Pentad II · Delta Trifracton Node ε#2 5η 5ι", &date, &parsed, &finest) == FRC_OK);
    CHECK(parsed.alpha == 5 && parsed.beta == 4 && parsed.gamma == 2 && parsed.delta == 3 &&
          parsed.epsilon == 2 && parsed.eta == 5 && parsed.iota == 5 && parsed.lambda == 0 && parsed.nu == 0);
    CHECK(finest == FRC_UNIT_PREON);
    CHECK(frc_parse_date("⬠II δ#3 ε#2 5η 5ι", &date, &parsed, &finest) == FRC_OK);
    CHECK(parsed.delta == 3 && parsed.epsilon == 2 && parsed.eta == 5 && parsed.iota == 5 && finest == FRC_UNIT_PREON);
    /* "The full letter form is the stamp: it ... can be read back" */
    CHECK(frc_parse_date("α#5 β#4 γ#2 δ#3 ε#2 5η 5ι 5λ 5ν", NULL, &parsed, &finest) == FRC_OK);
    CHECK(parsed.alpha == 5 && parsed.beta == 4 && parsed.gamma == 2 && parsed.delta == 3 &&
          parsed.epsilon == 2 && parsed.eta == 5 && parsed.iota == 5 && parsed.lambda == 5 && parsed.nu == 5);
    CHECK(finest == FRC_UNIT_BLINK);
}

static void test_s15_cycle_names_and_spans(void)
{
    frc_span_t span;
    frc_rate_t rate;
    uint64_t num, den;
    char text[96];

    TEST_SECTION("S15.3 cycle names");
    /* "Delta Trifracton is the 3rd Delta of its Gamma" */
    CHECK(frc_format_cycle_name(3, 2, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Delta Trifracton");
    /* "a depth-9 cycle is ε#2 5η 9ι, a depth-8 cycle ε#2 5η 8ι (θ starts on
     * multiples of 4ι), and a depth-14 cycle ... 3.43ν". Inside one Delta:
     * Nuclide 2 (index 1), Helion 5 (index 4), then 9 Preons / 2 thetas. */
    CHECK(frc_format_cycle_name(8, (UINT64_C(1) * 77 + 4) * 36 + 9, 0, text, sizeof text) > 0);
    CHECK_STR(text, "ε#2 5η 9ι");
    CHECK(frc_format_cycle_name(7, (UINT64_C(1) * 77 + 4) * 9 + 2, 0, text, sizeof text) > 0);
    CHECK_STR(text, "ε#2 5η 8ι");
    /* a xi is 1/7 Blink: the 4th xi of Blink 3 starts at 3 + 3/7 = 3.43 */
    CHECK(frc_format_cycle_name(13, 3 * 7 + 3, 0, text, sizeof text) > 0);
    CHECK_STR(text, "ε#1 1η 0ι 0λ 3.43ν");

    TEST_SECTION("S15.4 spans");
    /* "Exact cycle lengths: ... ζ = 11η, θ = 4ι, κ = 13λ, μ = 5ν, and ξ = 0.14ν" */
    {
        static const struct { uint8_t level; const char *text; } lengths[] = {
            {0, "1α"}, {1, "1β"}, {2, "1γ"}, {3, "1δ"}, {4, "1ε"}, {6, "1η"}, {8, "1ι"}, {10, "1λ"}, {12, "1ν"},
            {5, "11η"}, {7, "4ι"}, {9, "13λ"}, {11, "5ν"}, {13, "0.14ν"},
        };
        size_t i;
        for (i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
            CHECK(frc_span_of_level(lengths[i].level, &span) == FRC_OK);
            CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
            CHECK_STR(text, lengths[i].text);
        }
    }
    /* "a span reads 3η 5λ, never 3η 0ι 5λ"; "2.6ν" */
    span.negative = 0;
    span.blinks = 3 * UINT64_C(58500) + 5 * 25;
    span.frac = 0;
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "3η 5λ");
    span.blinks = 2;
    span.frac = (frc_frac_t)(0.6 * 4294967296.0);
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "2.6ν");

    TEST_SECTION("S15.5 Blinks and pB");
    /* presets: 5ν (5x), λ (25x), 13λ (325x), ι (1,625x), 4ι (6,500x), η (58,500x), 11η (643,500x) */
    {
        static const struct { const char *text; uint64_t factor; } presets[8] = {
            {"real time", 1},         {"5ν per Blink", 5},       {"λ per Blink", 25},
            {"13λ per Blink", 325},   {"ι per Blink", 1625},     {"4ι per Blink", 6500},
            {"η per Blink", 58500},   {"11η per Blink", 643500},
        };
        uint8_t i;
        for (i = 0; i < 8; i++) {
            CHECK(frc_rate_preset(i, &rate) == FRC_OK);
            CHECK_EQ_U64(rate.factor, presets[i].factor);
            CHECK(frc_format_rate(&rate, text, sizeof text) > 0);
            CHECK_STR(text, presets[i].text);
        }
        /* "then ε, δ, γ per Blink" */
        CHECK(frc_rate_preset(8, &rate) == FRC_OK && rate.unit == FRC_UNIT_NUCLIDE && rate.count == 1);
        CHECK(frc_rate_preset(9, &rate) == FRC_OK && rate.unit == FRC_UNIT_DELTA && rate.count == 1);
        CHECK(frc_rate_preset(10, &rate) == FRC_OK && rate.unit == FRC_UNIT_GAMMA && rate.count == 1);
    }
    /* "ξ 7, ο 77, π 693 and ρ 2,772 pB" */
    {
        static const struct { uint8_t level; uint64_t rate; const char *text; } rates[4] = {
            {13, 7, "7 pB"}, {14, 77, "77 pB"}, {15, 693, "693 pB"},
            {16, 2772, "2\xE2\x80\xAF" "772 pB"}}; /* grouped with U+202F, the narrow no-break space */
        uint8_t i;
        for (i = 0; i < 4; i++) {
            CHECK(frc_level_per_blink(rates[i].level, &num, &den) == FRC_OK);
            CHECK(num == rates[i].rate && den == 1);
            CHECK(frc_format_per_blink(num, den, text, sizeof text) > 0);
            CHECK_STR(text, rates[i].text);
        }
    }
    /* "A 110 Hz tone is 34.2 pB": f_pB = f_Hz * T(ν), with the mean Blink of
     * S16.5, T(12) = 77894856196 / 250529296875 s */
    CHECK(frc_format_per_blink(110 * UINT64_C(77894856196), UINT64_C(250529296875), text, sizeof text) > 0);
    CHECK_STR(text, "34.2 pB");
}

static void test_s15_6_vectors(void)
{
    static const struct {
        const char *name;
        int64_t     unix_ms;
        const char *full;
        const char *brief;
        uint16_t    pair[2];
    } vectors[4] = {
        {"A", INT64_C(1791460800000),
         "Alpha Hexafracton Valley, Beta Tetrafracton Valley, Gamma Nonafracton Valley, Delta Trifracton End, "
         "ε#5 34η 29ι 21λ 3ν",
         "Delta Trifracton End ε#5 34η 29ι", {13, 0}},
        {"B", INT64_C(946684800000),
         "Alpha Hexafracton Node, Beta Difracton End, Gamma Didecafracton Node, Delta Trifracton Node, "
         "ε#2 21η 12ι 6λ 5ν",
         "Delta Trifracton Node ε#2 21η 12ι", {5, 6}},
        {"C", INT64_C(2886085800000),
         "Alpha Heptafracton Peak, Beta Difracton Peak, Gamma Monofracton End, Delta Pentafracton Peak, "
         "ε#2 1η 23ι 1λ 21ν",
         "Delta Pentafracton Peak ε#2 1η 23ι", {10, 3}},
        {"Anchor + 1 s", INT64_C(125593320816),
         "Alpha Hexafracton Peak, Beta Monofracton Peak, Gamma Monofracton Peak, Delta Monofracton Peak, "
         "ε#1 1η 0ι 0λ 3ν",
         "Delta Monofracton Peak ε#1 1η 0ι", {0, 0}},
    };
    const frc_time_t a = FRC_TIME_FROM_S(INT64_C(1791460800));
    frc_date_t date, parsed;
    frc_odometer_t pair;
    frc_threshold_t next;
    frc_span_t span;
    frc_time_t blink_start;
    char text[200], ascii[200];
    size_t i;

    TEST_SECTION("S15.6 test vectors");
    for (i = 0; i < 4; i++) {
        const frc_time_t t = FRC_TIME_FROM_MS(vectors[i].unix_ms);
        CHECK(frc_date_at(CAT, t, &date) == FRC_OK);
        CHECK(frc_format_date(&date, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text) > 0);
        CHECK_STR(text, vectors[i].full);
        CHECK(frc_format_date(&date, FRC_DATE_SHORT, FRC_UNIT_PREON, text, sizeof text) > 0);
        CHECK_STR(text, vectors[i].brief);
        /* "The Helion pair is the depth-7 odometer of S12" */
        CHECK(frc_odometer_at(CAT, t, 6, 2, 2, &pair) == FRC_OK);
        CHECK(pair.digit[0] == vectors[i].pair[0] && pair.digit[1] == vectors[i].pair[1]);
        CHECK_NOTE("vector %s", vectors[i].name);

        /* the full date reads back, and names the Blink that holds the instant */
        CHECK(frc_parse_date(vectors[i].full, NULL, &parsed, NULL) == FRC_OK);
        CHECK(frc_date_to_time(CAT, &parsed, &blink_start) == FRC_OK);
        CHECK(blink_start <= t && t - blink_start < 312 * 1000); /* a Blink is 310.92 ms */
    }

    /* "The stamp at A is α#6 β#4 γ#9 δ#3 ε#5 34η 29ι 21λ 3ν" */
    CHECK(frc_date_at(CAT, a, &date) == FRC_OK);
    CHECK(frc_format_date(&date, FRC_DATE_LETTERS, FRC_UNIT_BLINK, text, sizeof text) > 0);
    CHECK_STR(text, "α#6 β#4 γ#9 δ#3 ε#5 34η 29ι 21λ 3ν");
    /* ... and in the plain ASCII of the header's example */
    CHECK(frc_to_ascii(text, ascii, sizeof ascii) > 0);
    CHECK_STR(ascii, "a#6 b#4 g#9 d#3 e#5 34h 29i 21l 3n");
    CHECK(frc_parse_date(ascii, NULL, &parsed, NULL) == FRC_OK);
    CHECK(parsed.alpha == 6 && parsed.beta == 4 && parsed.gamma == 9 && parsed.delta == 3 &&
          parsed.epsilon == 5 && parsed.eta == 34 && parsed.iota == 29 && parsed.lambda == 21 && parsed.nu == 3);

    /* "At A the depth-7 end is 3,372.428 s away (S12), which is the span 6ι 43λ" */
    CHECK(frc_next_threshold(CAT, a, 6, &next) == FRC_OK);
    CHECK_EQ_I64(nearest_ms(next.time - a), 3372428);
    CHECK(frc_span_between(CAT, a, next.time, &span) == FRC_OK);
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "6ι 43λ");
    /* and backwards it carries the minus sign */
    CHECK(frc_span_between(CAT, next.time, a, &span) == FRC_OK);
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "−6ι 43λ");
}

/* ------------------------------------------------------------------------- */
/* S16  Spectral ladder                                                       */
/* ------------------------------------------------------------------------- */

static void test_s16_1_ladder(void)
{
    /* The table of S16.1, all 63 rungs: Level, Name, Unit, Siblings, Period,
     * Frequency, Band, Light in vacuum (4 significant digits, the default). */
    static const struct {
        uint8_t level; const char *name; const char *unit; uint8_t siblings;
        const char *period; const char *frequency; frc_band_t band; const char *length;
    } rows[FRC_LEVEL_COUNT] = {
        { 0, "α", "Alpha",  0, "72.12 y", "439.4 pHz", FRC_BAND_NONE, "682.3 Pm"},
        { 1, "β", "Beta",  5, "14.42 y", "2.197 nHz", FRC_BAND_NONE, "136.5 Pm"},
        { 2, "γ", "Gamma", 13, "1.110 y", "28.56 nHz", FRC_BAND_NONE, "10.50 Pm"},
        { 3, "δ", "Delta",  5, "81.05 d", "142.8 nHz", FRC_BAND_NONE, "2.099 Pm"},
        { 4, "ε", "Nuclide",  5, "16.21 d", "714.0 nHz", FRC_BAND_NONE, "419.9 Tm"},
        { 5, "ζ", "",  7, "2.316 d", "4.998 µHz", FRC_BAND_NONE, "59.98 Tm"},
        { 6, "η", "Helion", 11, "5.052 h", "54.98 µHz", FRC_BAND_NONE, "5.453 Tm"},
        { 7, "θ", "",  9, "33.68 min", "494.8 µHz", FRC_BAND_NONE, "605.9 Gm"},
        { 8, "ι", "Preon",  4, "8.421 min", "1.979 mHz", FRC_BAND_NONE, "151.5 Gm"},
        { 9, "κ", "",  5, "1.684 min", "9.896 mHz", FRC_BAND_NONE, "30.29 Gm"},
        {10, "λ", "Flavon", 13, "7.773 s", "128.6 mHz", FRC_BAND_NONE, "2.330 Gm"},
        {11, "μ", "",  5, "1.555 s", "643.2 mHz", FRC_BAND_NONE, "466.1 Mm"},
        {12, "ν", "Blink",  5, "310.9 ms", "3.216 Hz", FRC_BAND_NONE, "93.21 Mm"},
        {13, "ξ", "",  7, "44.42 ms", "22.51 Hz", FRC_BAND_AUDIBLE, "13.32 Mm"},
        {14, "ο", "", 11, "4.038 ms", "247.7 Hz", FRC_BAND_AUDIBLE, "1.211 Mm"},
        {15, "π", "",  9, "448.7 µs", "2.229 kHz", FRC_BAND_AUDIBLE, "134.5 km"},
        {16, "ρ", "",  4, "112.2 µs", "8.915 kHz", FRC_BAND_AUDIBLE, "33.63 km"},
        {17, "σ", "",  5, "22.43 µs", "44.58 kHz", FRC_BAND_RADIO, "6.725 km"},
        {18, "τ", "", 13, "1.726 µs", "579.5 kHz", FRC_BAND_RADIO, "517.3 m"},
        {19, "υ", "",  5, "345.1 ns", "2.898 MHz", FRC_BAND_RADIO, "103.5 m"},
        {20, "φ", "",  5, "69.02 ns", "14.49 MHz", FRC_BAND_RADIO, "20.69 m"},
        {21, "χ", "",  7, "9.861 ns", "101.4 MHz", FRC_BAND_RADIO, "2.956 m"},
        {22, "ψ", "", 11, "896.4 ps", "1.116 GHz", FRC_BAND_MICROWAVE, "268.7 mm"},
        {23, "ω", "",  9, "99.60 ps", "10.04 GHz", FRC_BAND_MICROWAVE, "29.86 mm"},
        {24, "α1", "",  4, "24.90 ps", "40.16 GHz", FRC_BAND_MICROWAVE, "7.465 mm"},
        {25, "β1", "",  5, "4.980 ps", "200.8 GHz", FRC_BAND_MICROWAVE, "1.493 mm"},
        {26, "γ1", "", 13, "383.1 fs", "2.610 THz", FRC_BAND_INFRARED, "114.8 µm"},
        {27, "δ1", "",  5, "76.62 fs", "13.05 THz", FRC_BAND_INFRARED, "22.97 µm"},
        {28, "ε1", "",  5, "15.32 fs", "65.26 THz", FRC_BAND_INFRARED, "4.594 µm"},
        {29, "ζ1", "",  7, "2.189 fs", "456.8 THz", FRC_BAND_VISIBLE, "656.3 nm"},
        {30, "η1", "", 11, "199.0 as", "5.025 PHz", FRC_BAND_ULTRAVIOLET, "59.66 nm"},
        {31, "θ1", "",  9, "22.11 as", "45.22 PHz", FRC_BAND_XRAY, "6.629 nm"},
        {32, "ι1", "",  4, "5.528 as", "180.9 PHz", FRC_BAND_XRAY, "1.657 nm"},
        {33, "κ1", "",  5, "1.106 as", "904.5 PHz", FRC_BAND_XRAY, "331.4 pm"},
        {34, "λ1", "", 13, "85.05 zs", "11.76 EHz", FRC_BAND_XRAY, "25.50 pm"},
        {35, "μ1", "",  5, "17.01 zs", "58.79 EHz", FRC_BAND_GAMMA, "5.099 pm"},
        {36, "ν1", "",  5, "3.402 zs", "294.0 EHz", FRC_BAND_GAMMA, "1.020 pm"},
        {37, "ξ1", "",  7, "486.0 ys", "2.058 ZHz", FRC_BAND_GAMMA, "145.7 fm"},
        {38, "ο1", "", 11, "44.18 ys", "22.63 ZHz", FRC_BAND_GAMMA, "13.24 fm"},
        {39, "π1", "",  9, "4.909 ys", "203.7 ZHz", FRC_BAND_GAMMA, "1.472 fm"},
        {40, "ρ1", "",  4, "1.227 ys", "814.9 ZHz", FRC_BAND_GAMMA, "367.9 am"},
        {41, "σ1", "",  5, "2.454 × 10⁻²⁵ s", "4.074 YHz", FRC_BAND_GAMMA, "73.58 am"},
        {42, "τ1", "", 13, "1.888 × 10⁻²⁶ s", "52.97 YHz", FRC_BAND_GAMMA, "5.660 am"},
        {43, "υ1", "",  5, "3.776 × 10⁻²⁷ s", "264.8 YHz", FRC_BAND_GAMMA, "1.132 am"},
        {44, "φ1", "",  5, "7.552 × 10⁻²⁸ s", "1.324 × 10²⁷ Hz", FRC_BAND_GAMMA, "226.4 zm"},
        {45, "χ1", "",  7, "1.079 × 10⁻²⁸ s", "9.269 × 10²⁷ Hz", FRC_BAND_GAMMA, "32.34 zm"},
        {46, "ψ1", "", 11, "9.808 × 10⁻³⁰ s", "1.020 × 10²⁹ Hz", FRC_BAND_GAMMA, "2.940 zm"},
        {47, "ω1", "",  9, "1.090 × 10⁻³⁰ s", "9.176 × 10²⁹ Hz", FRC_BAND_NONE, "326.7 ym"},
        {48, "α2", "",  4, "2.724 × 10⁻³¹ s", "3.671 × 10³⁰ Hz", FRC_BAND_NONE, "81.68 ym"},
        {49, "β2", "",  5, "5.449 × 10⁻³² s", "1.835 × 10³¹ Hz", FRC_BAND_NONE, "16.34 ym"},
        {50, "γ2", "", 13, "4.191 × 10⁻³³ s", "2.386 × 10³² Hz", FRC_BAND_NONE, "1.257 ym"},
        {51, "δ2", "",  5, "8.383 × 10⁻³⁴ s", "1.193 × 10³³ Hz", FRC_BAND_NONE, "2.513 × 10⁻²⁵ m"},
        {52, "ε2", "",  5, "1.677 × 10⁻³⁴ s", "5.965 × 10³³ Hz", FRC_BAND_NONE, "5.026 × 10⁻²⁶ m"},
        {53, "ζ2", "",  7, "2.395 × 10⁻³⁵ s", "4.175 × 10³⁴ Hz", FRC_BAND_NONE, "7.180 × 10⁻²⁷ m"},
        {54, "η2", "", 11, "2.177 × 10⁻³⁶ s", "4.593 × 10³⁵ Hz", FRC_BAND_NONE, "6.528 × 10⁻²⁸ m"},
        {55, "θ2", "",  9, "2.419 × 10⁻³⁷ s", "4.133 × 10³⁶ Hz", FRC_BAND_NONE, "7.253 × 10⁻²⁹ m"},
        {56, "ι2", "",  4, "6.048 × 10⁻³⁸ s", "1.653 × 10³⁷ Hz", FRC_BAND_NONE, "1.813 × 10⁻²⁹ m"},
        {57, "κ2", "",  5, "1.210 × 10⁻³⁸ s", "8.267 × 10³⁷ Hz", FRC_BAND_NONE, "3.626 × 10⁻³⁰ m"},
        {58, "λ2", "", 13, "9.305 × 10⁻⁴⁰ s", "1.075 × 10³⁹ Hz", FRC_BAND_NONE, "2.790 × 10⁻³¹ m"},
        {59, "μ2", "",  5, "1.861 × 10⁻⁴⁰ s", "5.374 × 10³⁹ Hz", FRC_BAND_NONE, "5.579 × 10⁻³² m"},
        {60, "ν2", "",  5, "3.722 × 10⁻⁴¹ s", "2.687 × 10⁴⁰ Hz", FRC_BAND_NONE, "1.116 × 10⁻³² m"},
        {61, "ξ2", "",  7, "5.317 × 10⁻⁴² s", "1.881 × 10⁴¹ Hz", FRC_BAND_NONE, "1.594 × 10⁻³³ m"},
        {62, "ο2", "", 11, "4.834 × 10⁻⁴³ s", "2.069 × 10⁴² Hz", FRC_BAND_NONE, "1.449 × 10⁻³⁴ m"},
    };
    const frc_interval_t interval = reference_interval();
    const frc_speed_t light = frc_medium_speed(FRC_MEDIUM_VACUUM);
    char text[64];
    uint8_t i;

    TEST_SECTION("S16.1 the ladder");
    for (i = 0; i < FRC_LEVEL_COUNT; i++) {
        int unit = frc_unit_of_level(rows[i].level);
        CHECK_EQ_U64(rows[i].level, i);
        CHECK(frc_format_level_name(i, text, sizeof text) > 0);
        CHECK_STR(text, rows[i].name);
        text[0] = '\0';
        if (unit >= 0) {
            CHECK(frc_format_unit_name((frc_unit_t)unit, 0, text, sizeof text) > 0);
        }
        CHECK_STR(text, rows[i].unit);
        CHECK_EQ_U64(frc_siblings(i), rows[i].siblings);
        CHECK(frc_format_period(i, interval, 0, text, sizeof text) > 0);
        CHECK_STR(text, rows[i].period);
        CHECK(frc_format_frequency(i, interval, 0, text, sizeof text) > 0);
        CHECK_STR(text, rows[i].frequency);
        CHECK_EQ_I64(frc_band(i, interval), rows[i].band);
        CHECK(frc_format_length(i, interval, light, 0, text, sizeof text) > 0);
        CHECK_STR(text, rows[i].length);
        CHECK_NOTE("level %d", i);
    }
    /* "the ladder ends at depth 63 and an implementation gives nothing below it" */
    CHECK(frc_format_period(63, interval, 0, text, sizeof text) == FRC_ERR_ARG);
    /* the interval of the table is the one that holds 2010-2028, i.e. vector A */
    {
        frc_interval_t held;
        CHECK(frc_interval_at(CAT, FRC_TIME_FROM_S(INT64_C(1791460800)), &held) == FRC_OK);
        CHECK(held.den != 0 && held.num == FRC_INTERVAL_REFERENCE_MS * held.den);
    }
}

static void test_s16_2_bands(void)
{
    static const struct { frc_band_t band; const char *name; } bands[9] = {
        {FRC_BAND_NONE, "no band"},     {FRC_BAND_AUDIBLE, "audible"},   {FRC_BAND_RADIO, "radio"},
        {FRC_BAND_MICROWAVE, "microwave"}, {FRC_BAND_INFRARED, "infrared"}, {FRC_BAND_VISIBLE, "visible"},
        {FRC_BAND_ULTRAVIOLET, "ultraviolet"}, {FRC_BAND_XRAY, "X-ray"}, {FRC_BAND_GAMMA, "gamma ray"}};
    /* "a frequency equal to a boundary belongs to the band that starts there":
     * choose the interval so that f(0) = P(0) / (4 I) is exactly the boundary,
     * I = 1000 / (4 f) ms = 250 / f. */
    static const struct { uint64_t hz; frc_band_t band; } edges[5] = {
        {20, FRC_BAND_AUDIBLE},          {20000, FRC_BAND_RADIO},     {300000000, FRC_BAND_MICROWAVE},
        {UINT64_C(300000000000), FRC_BAND_INFRARED}, {UINT64_C(400000000000000), FRC_BAND_VISIBLE}};
    char text[32];
    size_t i;

    TEST_SECTION("S16.2 bands");
    for (i = 0; i < 9; i++) {
        CHECK(frc_format_band(bands[i].band, text, sizeof text) > 0);
        CHECK_STR(text, bands[i].name);
    }
    for (i = 0; i < 5; i++) {
        frc_interval_t exact, slower;
        exact.num = 250;
        exact.den = edges[i].hz;
        CHECK_EQ_I64(frc_band(0, exact), edges[i].band);
        /* a hair longer interval, a hair lower frequency: the band below */
        slower.num = 250 * UINT64_C(1000000) + 1;
        slower.den = edges[i].hz * UINT64_C(1000000);
        CHECK_EQ_I64(frc_band(0, slower), edges[i].band - 1);
    }
}

static void test_s16_3_media(void)
{
    /* S16.3: Id, Medium, Phase, Speed (m/s) */
    static const struct { const char *id; const char *name; const char *phase; uint64_t num; uint64_t den; } media[FRC_MEDIUM_COUNT] = {
        {"vacuum", "Light in vacuum", "Vacuum", 299792458, 1},
        {"bounded", "Light in bounded medium", "Vacuum + solid", 200000000, 1},
        {"diamond", "Diamond sound", "Solid", 18000, 1},
        {"water", "Water sound", "Liquid", 1500, 1},
        {"air", "Air sound", "Gas", 343, 1},
        {"nerve", "Nerve conduction", "Solid + liquid", 120, 1},
        {"chemical", "Chemical transport", "Liquid + gas + solid", 1, 1000},
    };
    const frc_interval_t interval = reference_interval();
    char text[64];
    uint8_t i;

    TEST_SECTION("S16.3 spatial units");
    for (i = 0; i < FRC_MEDIUM_COUNT; i++) {
        frc_speed_t speed = frc_medium_speed((frc_medium_t)i);
        CHECK(speed.den != 0 && speed.num * media[i].den == media[i].num * speed.den);
        CHECK(frc_format_medium_id((frc_medium_t)i, text, sizeof text) > 0);
        CHECK_STR(text, media[i].id);
        CHECK(frc_format_medium_name((frc_medium_t)i, text, sizeof text) > 0);
        CHECK_STR(text, media[i].name);
        CHECK(frc_format_medium_phase((frc_medium_t)i, text, sizeof text) > 0);
        CHECK_STR(text, media[i].phase);
    }
    /* "A Blink is 93,212 km of light in vacuum, 106.6 m of sound in air and
     * 37.31 m along a nerve. Depth 30 (ζ1) is red light, 656.3 nm." */
    CHECK(frc_format_length(12, interval, frc_medium_speed(FRC_MEDIUM_VACUUM), 5, text, sizeof text) > 0);
    CHECK_STR(text, "93.212 Mm");
    CHECK(frc_format_length(12, interval, frc_medium_speed(FRC_MEDIUM_AIR), 4, text, sizeof text) > 0);
    CHECK_STR(text, "106.6 m");
    CHECK(frc_format_length(12, interval, frc_medium_speed(FRC_MEDIUM_NERVE), 4, text, sizeof text) > 0);
    CHECK_STR(text, "37.31 m");
    CHECK(frc_format_length(29, interval, frc_medium_speed(FRC_MEDIUM_VACUUM), 4, text, sizeof text) > 0);
    CHECK_STR(text, "656.3 nm");
    /* "an Alpha of light in vacuum is 72.12 light-years (9,460,730,472,580,800 m)":
     * a light-year is light's way in a Julian year, so it is the Alpha in years */
    CHECK(frc_format_period(0, interval, 4, text, sizeof text) > 0);
    CHECK_STR(text, "72.12 y");
#ifndef FRC_NO_FLOAT
    CHECK(fabs(frc_length_metres(0, interval, frc_medium_speed(FRC_MEDIUM_VACUUM)) / 9460730472580800.0 - 72.12) < 0.005);
#endif
}

static void test_s16_5_vectors(void)
{
    const frc_interval_t interval = reference_interval();
    const frc_interval_t mean = frc_interval_mean(CAT);
    const frc_speed_t vacuum = frc_medium_speed(FRC_MEDIUM_VACUUM);
    const frc_speed_t air = frc_medium_speed(FRC_MEDIUM_AIR);
    const frc_speed_t nerve = frc_medium_speed(FRC_MEDIUM_NERVE);
    const frc_speed_t chemical = frc_medium_speed(FRC_MEDIUM_CHEMICAL);
    frc_sci_t value;
    char text[64];

    TEST_SECTION("S16.5 test vectors (reference interval)");
    /* T(12), the Blink: 51724807/166359375 s = 310.922 ms */
    CHECK(frc_period(12, interval, &value) == FRC_OK);
    CHECK_SCI(value, "51724807", UINT64_C(166359375), 0);
    CHECK(frc_format_period(12, interval, 6, text, sizeof text) > 0);
    CHECK_STR(text, "310.922 ms");

    /* f(13), xi: 1164515625/51724807 Hz = 22.5137 Hz */
    CHECK(frc_frequency(13, interval, &value) == FRC_OK);
    CHECK_SCI(value, "1164515625", UINT64_C(51724807), 0);
    CHECK(frc_format_frequency(13, interval, 6, text, sizeof text) > 0);
    CHECK_STR(text, "22.5137 Hz");

    /* f(62), omicron-2: a 51-digit numerator over 51724807 = 2.0688 x 10^42 Hz */
    CHECK(frc_frequency(62, interval, &value) == FRC_OK);
    CHECK_SCI(value, "107008321717714395645982788862060546875000000000000", UINT64_C(51724807), 0);
    CHECK(frc_format_frequency(62, interval, 5, text, sizeof text) > 0);
    CHECK_STR(text, "2.0688 × 10⁴² Hz");

    /* length(12), light in vacuum: 2215243861443658/23765625 m = 93,212.1 km */
    CHECK(frc_length(12, interval, vacuum, &value) == FRC_OK);
    CHECK_SCI(value, "2215243861443658", UINT64_C(23765625), 0);
    CHECK(frc_format_length(12, interval, vacuum, 6, text, sizeof text) > 0);
    CHECK_STR(text, "93.2121 Mm"); /* SPEC writes the same six digits in km */

    /* length(12, air): 2534515543/23765625 m = 106.646 m */
    CHECK(frc_length(12, interval, air, &value) == FRC_OK);
    CHECK_SCI(value, "2534515543", UINT64_C(23765625), 0);
    CHECK(frc_format_length(12, interval, air, 6, text, sizeof text) > 0);
    CHECK_STR(text, "106.646 m");

    /* length(12, nerve): 413798456/11090625 m = 37.3107 m */
    CHECK(frc_length(12, interval, nerve, &value) == FRC_OK);
    CHECK_SCI(value, "413798456", UINT64_C(11090625), 0);
    CHECK(frc_format_length(12, interval, nerve, 6, text, sizeof text) > 0);
    CHECK_STR(text, "37.3107 m");

    /* length(12, chemical): 51724807/166359375000 m = 310.922 µm */
    CHECK(frc_length(12, interval, chemical, &value) == FRC_OK);
    CHECK_SCI(value, "51724807", UINT64_C(166359375000), 0);
    CHECK(frc_format_length(12, interval, chemical, 6, text, sizeof text) > 0);
    CHECK_STR(text, "310.922 µm");

    /* length(29), zeta-1, light in vacuum:
     * 1107621930721829/1687759133607421875000 m = 656.268 nm
     * (the denominator is written as 1687759133607421875 * 10^3 to fit 64 bits) */
    CHECK(frc_length(29, interval, vacuum, &value) == FRC_OK);
    CHECK_SCI(value, "1107621930721829", UINT64_C(1687759133607421875), -3);
    CHECK(frc_format_length(29, interval, vacuum, 6, text, sizeof text) > 0);
    CHECK_STR(text, "656.268 nm");

    TEST_SECTION("S16.5 test vectors (mean interval)");
    /* "On the mean interval of Saros 141, 39,259,007,522,784 / 69 ms" */
    CHECK(mean.den != 0 && mean.num * 69 == UINT64_C(39259007522784) * mean.den);
    /* (E[n] - E[0]) / n from the two ends of S14 */
    CHECK_EQ_I64(frc_catalogue_last(CAT) - frc_catalogue_first(CAT), INT64_C(39259007522784) * 1000);
    /* f(12) = 250529296875/77894856196 Hz, which is 3.21625 Hz */
    CHECK(frc_frequency(12, mean, &value) == FRC_OK);
    CHECK_SCI(value, "250529296875", UINT64_C(77894856196), 0);
    CHECK(frc_format_frequency(12, mean, 6, text, sizeof text) > 0);
    CHECK_STR(text, "3.21625 Hz");
    /* "and T(12) is 310.921 ms, the Blink of S15.5" */
    CHECK(frc_period(12, mean, &value) == FRC_OK);
    CHECK_SCI(value, "77894856196", UINT64_C(250529296875), 0);
    CHECK(frc_format_period(12, mean, 6, text, sizeof text) > 0);
    CHECK_STR(text, "310.921 ms");

#ifndef FRC_NO_FLOAT
    /* S16.4: "A result given as a double is the double nearest to the exact
     * value". The exact fractions above, divided in double, are within one
     * rounding of it. */
    TEST_SECTION("S16.5 as doubles");
    CHECK(fabs(frc_period_seconds(12, interval) / (51724807.0 / 166359375.0) - 1.0) < 4e-16);
    CHECK(fabs(frc_frequency_hz(13, interval) / (1164515625.0 / 51724807.0) - 1.0) < 4e-16);
    CHECK(fabs(frc_frequency_hz(62, interval) / 2.0688e42 - 1.0) < 5e-5);
    CHECK(fabs(frc_length_metres(12, interval, vacuum) - 93212.1e3) < 0.05e3);
    CHECK(fabs(frc_length_metres(12, interval, air) - 106.646) < 0.0005);
    CHECK(fabs(frc_length_metres(12, interval, nerve) - 37.3107) < 0.00005);
    CHECK(fabs(frc_length_metres(12, interval, chemical) - 310.922e-6) < 0.0005e-6);
    CHECK(fabs(frc_length_metres(29, interval, vacuum) - 656.268e-9) < 0.0005e-9);
    CHECK(fabs(frc_frequency_hz(12, mean) - 3.21625) < 0.000005);
    CHECK(fabs(frc_period_seconds(12, mean) - 310.921e-3) < 0.0005e-3);
#endif
}

static void test_s16_4_prefixes(void)
{
    char text[64];

    TEST_SECTION("S16.4 prefixes");
    /* "P(62) is 4.7 x 10^51 and needs 172 bits" */
    CHECK(frc_format_prefix(22, text, sizeof text) > 0);
    CHECK_STR(text, "2538851346281250000");
    CHECK_EQ_U64(frc_prefix(22), UINT64_C(2538851346281250000));
    CHECK(frc_format_prefix(23, text, sizeof text) > 0);
    CHECK_STR(text, "22849662116531250000");
    CHECK(frc_format_prefix(62, text, sizeof text) > 0);
    CHECK_STR(text, "4708366155579433408423242709930664062500000000000000");
    CHECK(frc_format_prefix(63, text, sizeof text) > 0);
    CHECK_STR(text, "42375295400214900675809184389375976562500000000000000");
}

/* ------------------------------------------------------------------------- */
/* The QUICK START of fractonica.h, as written there                          */
/* ------------------------------------------------------------------------- */

static int quick_start_ops;
static void quick_start_sink(void *ctx, frc_path_op_t op, const int16_t *pts)
{
    (void)ctx;
    (void)op;
    (void)pts;
    quick_start_ops++;
}

static void test_quick_start(void)
{
    const int64_t unix_ms = INT64_C(1791460800000); /* vector A */
    int i;

    TEST_SECTION("QUICK START");
    /* ---- the header's text, verbatim ---- */
    const frc_catalogue_t *cat = &FRC_SAROS_141;
    frc_time_t now = FRC_TIME_FROM_MS(unix_ms);
    frc_date_t date;   char text[96];
    if (frc_date_at(cat, now, &date) == FRC_OK)
        frc_format_date(&date, FRC_DATE_SHORT, FRC_UNIT_PREON, text, sizeof text);
    // -> "Delta Trifracton End ε#5 34η 29ι"
    frc_odometer_t helion;                       // the basic reading (S15.3)
    frc_odometer_at(cat, now, 6, 2, 2, &helion); // level 6, 2 strokes, 2 glyphs
    // draw helion.digit[0..1] with frc_glyph_from_index + frc_glyph_path,
    // redraw at helion.next_change.
    /* ---- end of the header's text ---- */

    CHECK_STR(text, "Delta Trifracton End ε#5 34η 29ι");
    CHECK(helion.digit[0] == 13 && helion.digit[1] == 0);
    for (i = 0; i < helion.count; i++) {
        frc_glyph_t glyph;
        CHECK(frc_glyph_from_index(helion.digit[i], helion.strokes, &glyph) == FRC_OK);
        quick_start_ops = 0;
        frc_glyph_path(&glyph, quick_start_sink, NULL);
        CHECK(quick_start_ops >= 8); /* a stem and a stroke per trit */
    }
    /* "redraw at helion.next_change": at that instant the reading is new,
     * one microsecond earlier it is the one just drawn */
    {
        frc_odometer_t later, before;
        CHECK(helion.next_change > now);
        CHECK(frc_odometer_at(cat, helion.next_change, 6, 2, 2, &later) == FRC_OK);
        CHECK(later.digit[0] == 13 && later.digit[1] == 1);
        CHECK(frc_odometer_at(cat, helion.next_change - 1, 6, 2, 2, &before) == FRC_OK);
        CHECK(before.digit[0] == 13 && before.digit[1] == 0);
        /* S5: "At depth 7 with 2 two-stroke glyphs ... the sub glyph [changes] every 71 s" */
        CHECK(later.next_change - helion.next_change > 70 * FRC_US_PER_SECOND &&
              later.next_change - helion.next_change < 72 * FRC_US_PER_SECOND);
    }
    CHECK_STR(FRC_SPEC_VERSION, "2.2.0");
    /* the library's own version, and its three parts agree with the string */
    CHECK_STR(FRC_VERSION, "0.2.0");
    {
        char parts[32];
        snprintf(parts, sizeof parts, "%d.%d.%d", FRC_VERSION_MAJOR, FRC_VERSION_MINOR, FRC_VERSION_PATCH);
        CHECK_STR(parts, FRC_VERSION);
    }
}

int main(void)
{
    test_s12_vector_a();
    test_s12_vector_b();
    test_s12_vector_c();
    test_s12_anchor();
    test_s7_pentads();
    test_s3_period_lengths();
    test_s4_states();
    test_s9_geometry();
    test_s15_units();
    test_s15_date_forms();
    test_s15_cycle_names_and_spans();
    test_s15_6_vectors();
    test_s16_1_ladder();
    test_s16_2_bands();
    test_s16_3_media();
    test_s16_4_prefixes();
    test_s16_5_vectors();
    test_quick_start();
    return TEST_SUMMARY("spec");
}
