/*
 * test_sync.c - the solar sync and its Pentads (SPEC S7): src/frc_sync.c.
 *
 * Held against
 *   - tests/vectors/oracle_clock.h  every sample: the sync (number, start, end, nominal,
 *                                   drift, phase, quarter, next threshold), its odometers
 *                                   of 3 x 2 strokes and 2 x 3 strokes, the Pentad; syncs
 *                                   and Pentads by number
 *   - SPEC S12 (the sync rows of vectors A, B, C and the anchor), S7.2 (per-sync offsets,
 *     the lap), S7.5 (the Pentad statements)
 * plus what no oracle can give: every whole sync of the catalogue tiles it without a gap,
 * each returned "next" instant is the first microsecond of the next state, the edges of
 * the catalogue, invalid arguments, and catalogues built to reach the rare branches
 * (a series that closes exactly on a sync, one whose syncs last 607 microseconds, a Pentad
 * that would start at the closing instant, results that do not fit 64 bits).
 */
#include "frc_test.h"
#include "fractonica.h"

#include <stddef.h>

#include "vectors/oracle_clock.h"

#define CAT (&FRC_SAROS_141)
#define SYNC_US (FRC_SYNC_DAYS * FRC_US_PER_DAY) /* 4 civil days */
#define HELION_PREFIX 125125                     /* P(6), SPEC S3.1 */

/* The nearest whole millisecond, for values SPEC prints to the millisecond. */
static int64_t nearest_ms(int64_t us)
{
    int64_t shifted = us + 500;
    return shifted >= 0 ? shifted / 1000 : -((-shifted + 999) / 1000);
}

/* A drift in units of 0.1 ms, the precision SPEC S12 prints ("-48469.9899 s"). */
static int64_t tenth_ms(int64_t us)
{
    return us >= 0 ? (us + 50) / 100 : -((-us + 50) / 100);
}

/* 1 when a 0.32 fixed-point phase is the value SPEC prints with nine decimals, to one
 * unit of the last decimal (the fixed-point floor alone can sit 2.3e-10 below the true
 * phase; the oracle pins every bit). */
static int phase_is(frc_frac_t phase, uint32_t billionths)
{
    int64_t printed = (int64_t)(((uint64_t)billionths << 32) / 1000000000u);
    int64_t error = (int64_t)phase - printed;
    return error >= -4 && error <= 4; /* 1e-9 is 4.3 units of 2^-32 */
}

static int same_sync(const frc_sync_t *a, const frc_sync_t *b)
{
    return a->sync == b->sync && a->start == b->start && a->end == b->end && a->nominal == b->nominal &&
           a->drift_us == b->drift_us;
}

static uint64_t step_of(const frc_odometer_t *odo)
{
    uint64_t step = 0;
    int j;
    for (j = 0; j < odo->count; j++) {
        step = step * odo->states + odo->digit[j];
    }
    return step;
}

/* ------------------------------------------------------------------------- */
/* SPEC vectors                                                               */
/* ------------------------------------------------------------------------- */

static void test_spec_vectors(void)
{
    /* SPEC S12, "The solar sync at A" and the solar rows of B and C */
    static const struct {
        const char *name;
        int64_t unix_s;
        int32_t sync;
        int64_t start_ms;
        uint32_t phase_e9;
        int64_t drift_tenth_ms;
        uint16_t digits[3];
        uint8_t next_name;
        int64_t next_ms;
    } V[] = {
        {"A", INT64_C(1791460800), 4820, INT64_C(1791336849826), 358662585, INT64_C(-484699899), {5, 11, 13},
         FRC_NODE, INT64_C(1791509644786)},
        {"B", INT64_C(946684800), 2375, INT64_C(946369476520), 912420883, INT64_C(-238432957), {14, 9, 9},
         FRC_END, INT64_C(946715066462)},
        {"C", INT64_C(2886085800), 7987, INT64_C(2885819519691), 770510076, INT64_C(-810001249), {12, 5, 4},
         FRC_END, INT64_C(2886165109343)},
    };
    size_t i;
    int j;

    TEST_SECTION("SPEC S12 sync vectors");
    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        frc_time_t t = FRC_TIME_FROM_S(V[i].unix_s);
        frc_sync_t s;
        frc_odometer_t odo;
        CHECK(frc_sync_at(CAT, t, &s) == FRC_OK);
        CHECK_EQ_I64(s.sync, V[i].sync);
        CHECK_EQ_I64(nearest_ms(s.start), V[i].start_ms);
        CHECK(phase_is(s.phase, V[i].phase_e9));
        CHECK_EQ_I64(tenth_ms(s.drift_us), V[i].drift_tenth_ms);
        CHECK_EQ_U64(s.next.name, V[i].next_name);
        CHECK_EQ_I64(nearest_ms(s.next.time), V[i].next_ms);
        CHECK_NOTE("vector %s", V[i].name);
        CHECK(frc_sync_odometer(CAT, t, 2, 3, &odo) == FRC_OK);
        for (j = 0; j < 3; j++) {
            CHECK_EQ_U64(odo.digit[j], V[i].digits[j]);
        }
        CHECK_NOTE("vector %s", V[i].name);
    }
    {
        /* A: end 2026-10-11T01:33:59.745Z, four days less 10.081 s after its start;
         * "digits (2 x 3-stroke) [18, 33]" */
        frc_sync_t s;
        frc_odometer_t odo;
        frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
        CHECK(frc_sync_at(CAT, t, &s) == FRC_OK);
        CHECK_EQ_I64(nearest_ms(s.end), INT64_C(1791336849826) + 4 * 86400000 - 10081);
        CHECK_EQ_U64(s.quarter, FRC_NODE); /* phase 0.3587 is in the second quarter */
        CHECK(frc_sync_odometer(CAT, t, 3, 2, &odo) == FRC_OK);
        CHECK(odo.digit[0] == 18 && odo.digit[1] == 33);
        CHECK(odo.count == 2 && odo.strokes == 3 && odo.states == 52);
    }

    TEST_SECTION("SPEC S12 anchor vector");
    {
        /* "Unix 125593320.816 (one second after the anchor eclipse) gives sync 0, drift 0
         * and digits [0, 0, 0]" */
        frc_sync_t s;
        frc_odometer_t odo;
        frc_time_t anchor = frc_catalogue_instant(CAT, CAT->anchor);
        CHECK_EQ_I64(anchor, FRC_TIME_FROM_MS(INT64_C(125593319816))); /* S7.2: A = 20 */
        CHECK(frc_sync_at(CAT, anchor + FRC_US_PER_SECOND, &s) == FRC_OK);
        CHECK_EQ_I64(s.sync, 0);
        CHECK_EQ_I64(s.drift_us, 0);
        CHECK_EQ_I64(s.start, anchor);
        CHECK_EQ_I64(s.nominal, anchor);
        CHECK(frc_sync_odometer(CAT, anchor + FRC_US_PER_SECOND, 2, 3, &odo) == FRC_OK);
        CHECK(odo.digit[0] == 0 && odo.digit[1] == 0 && odo.digit[2] == 0);
        /* the anchor eclipse itself starts sync 0; the microsecond before it ends sync -1 */
        CHECK(frc_sync_at(CAT, anchor, &s) == FRC_OK);
        CHECK(s.sync == 0 && s.phase == 0 && s.quarter == FRC_PEAK);
        CHECK(frc_sync_at(CAT, anchor - 1, &s) == FRC_OK);
        CHECK(s.sync == -1 && s.quarter == FRC_END && s.end == anchor && s.next.time == anchor);
    }

    TEST_SECTION("SPEC S7.2: per-sync offsets and the lap");
    {
        /* "-10.0309 s (1973-1992), -10.0582 s (1992-2010), -10.0807 s (2010-2028),
         * -10.2204 s (2028-2046)": a sync inside each interval, N * cycle - D * 86400 */
        static const struct {
            int64_t unix_s;
            int64_t offset_tenth_ms;
        } OFFSET[] = {
            {INT64_C(315532800), -100309},  /* 1980 */
            {INT64_C(946684800), -100582},  /* 2000 */
            {INT64_C(1577836800), -100807}, /* 2020 */
            {INT64_C(1893456000), -102204}, /* 2030 */
        };
        frc_sync_t s, lo_sync, hi_sync;
        int32_t lo, hi;
        for (i = 0; i < sizeof OFFSET / sizeof OFFSET[0]; i++) {
            CHECK(frc_sync_at(CAT, FRC_TIME_FROM_S(OFFSET[i].unix_s), &s) == FRC_OK);
            CHECK_EQ_I64(tenth_ms(s.end - s.start - SYNC_US), OFFSET[i].offset_tenth_ms);
        }
        /* "the accumulated drift first passes -86,400 s at the sync of 2067-02-01
         * 15:01:58 UTC, 93.11 years after the anchor": the drift only ever decreases
         * after the anchor, so bisect for the first sync at or past one day. */
        lo = 0;
        hi = 20000;
        CHECK(frc_sync_by_number(CAT, lo, &lo_sync) == FRC_OK && lo_sync.drift_us > -FRC_US_PER_DAY);
        CHECK(frc_sync_by_number(CAT, hi, &hi_sync) == FRC_OK && hi_sync.drift_us <= -FRC_US_PER_DAY);
        while (hi - lo > 1) {
            int32_t mid = lo + (hi - lo) / 2;
            CHECK(frc_sync_by_number(CAT, mid, &s) == FRC_OK);
            if (s.drift_us <= -FRC_US_PER_DAY) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        CHECK(frc_sync_by_number(CAT, hi, &s) == FRC_OK);
        /* 2067-02-01T15:01:58Z = 35460 days and 54118 s after 1970 */
        CHECK_EQ_I64(s.start / FRC_US_PER_SECOND, INT64_C(35460) * 86400 + 54118);
        /* its civil grid time is the anchor's 15:01:59.816, one day later: the lap */
        CHECK_EQ_I64(s.nominal, s.start - s.drift_us);
        CHECK_EQ_I64((s.nominal / 1000) % 86400000, INT64_C(125593319816) % 86400000);
    }

    TEST_SECTION("SPEC S7.5: Pentad statements");
    {
        /* "Vector A is in Pentad II (its 2nd Helion) of sync 4820" */
        frc_pentad_t p;
        frc_time_t anchor = frc_catalogue_instant(CAT, CAT->anchor);
        CHECK(frc_pentad_at(CAT, FRC_TIME_FROM_S(INT64_C(1791460800)), &p) == FRC_OK);
        CHECK(p.sync == 4820 && p.index == 1 && p.helion == 2 && p.helions == 5);
        /* "the anchor eclipse itself starts Pentad I of sync 0" */
        CHECK(frc_pentad_at(CAT, anchor, &p) == FRC_OK);
        CHECK(p.sync == 0 && p.index == 0 && p.helion == 1 && p.helions == 5 && p.start == anchor);
        /* the microsecond before it is the last Helion of the Hypopentad of sync -1 */
        CHECK(frc_pentad_at(CAT, anchor - 1, &p) == FRC_OK);
        CHECK(p.sync == -1 && p.index == 3 && p.helion == 4 && p.helions == 4 && p.end == anchor);
    }
}

/* ------------------------------------------------------------------------- */
/* The oracle                                                                 */
/* ------------------------------------------------------------------------- */

static void test_samples_against_oracle(void)
{
    int i, j, with_sync = 0, partial = 0;

    TEST_SECTION("sync and Pentad against the clock oracle");
    for (i = 0; i < ORACLE_CLOCK_SAMPLES_COUNT; i++) {
        const oracle_clock_sample_t *v = &ORACLE_CLOCK_SAMPLES[i];
        const oracle_clock_sync_t *e = &v->sync;
        const oracle_clock_pentad_t *ep = &v->pentad;
        frc_sync_t s;
        frc_odometer_t odo;
        frc_pentad_t p;

        memset(&s, 0x55, sizeof s);
        if (!v->in_range) {
            /* outside the catalogue nothing is defined */
            CHECK(frc_sync_at(CAT, v->t_us, &s) == FRC_ERR_RANGE);
            CHECK(frc_sync_odometer(CAT, v->t_us, 2, 3, &odo) == FRC_ERR_RANGE);
            CHECK(frc_pentad_at(CAT, v->t_us, &p) == FRC_ERR_RANGE);
            CHECK_NOTE("sample %d (%s)", i, v->label);
            continue;
        }
        if (!e->has) {
            /* inside the first / last partial sync: no sync reading, but a Pentad */
            CHECK(frc_sync_at(CAT, v->t_us, &s) == FRC_ERR_RANGE);
            CHECK(frc_sync_odometer(CAT, v->t_us, 2, 3, &odo) == FRC_ERR_RANGE);
            CHECK(frc_sync_odometer(CAT, v->t_us, 3, 2, &odo) == FRC_ERR_RANGE);
            CHECK_NOTE("sample %d (%s)", i, v->label);
            partial++;
        } else {
            CHECK(frc_sync_at(CAT, v->t_us, &s) == FRC_OK);
            CHECK_EQ_I64(s.sync, e->number);
            CHECK_EQ_I64(s.start, e->start_us);
            CHECK_EQ_I64(s.end, e->end_us);
            CHECK_EQ_I64(s.nominal, e->nominal_us);
            CHECK_EQ_I64(s.drift_us, e->drift_us);
            CHECK_EQ_U64(s.phase, e->phase);
            CHECK_EQ_U64(s.quarter, e->quarter);
            CHECK_EQ_U64(s.next.name, e->next_name);
            CHECK_EQ_I64(s.next.time, e->next_us);
            CHECK_NOTE("sample %d (%s)", i, v->label);

            CHECK(frc_sync_odometer(CAT, v->t_us, 2, 3, &odo) == FRC_OK);
            CHECK(odo.count == 3 && odo.strokes == 2 && odo.states == 16 && odo.digit[3] == 0);
            for (j = 0; j < 3; j++) {
                CHECK_EQ_U64(odo.digit[j], e->digits_s2d3[j]);
            }
            /* 16^3 divides 2^32: the reference odometer is the top 12 bits of the phase */
            CHECK_EQ_U64(step_of(&odo), s.phase >> 20);
            CHECK(odo.next_change > v->t_us && odo.next_change <= s.end);
            CHECK(frc_sync_odometer(CAT, v->t_us, 3, 2, &odo) == FRC_OK);
            CHECK(odo.count == 2 && odo.strokes == 3 && odo.states == 52);
            for (j = 0; j < 2; j++) {
                CHECK_EQ_U64(odo.digit[j], e->digits_s3d2[j]);
            }
            CHECK_NOTE("sample %d (%s)", i, v->label);
            with_sync++;
        }

        CHECK_EQ_U64(ep->has, 1);
        CHECK(frc_pentad_at(CAT, v->t_us, &p) == FRC_OK);
        CHECK_EQ_I64(p.sync, ep->sync);
        CHECK_EQ_U64(p.index, ep->index);
        CHECK_EQ_U64(p.helion, ep->helion);
        CHECK_EQ_U64(p.helions, ep->helions);
        CHECK_EQ_I64(p.start, ep->start_us);
        CHECK_EQ_I64(p.end, ep->end_us);
        CHECK_NOTE("sample %d (%s)", i, v->label);
        if (e->has) {
            CHECK_EQ_I64(p.sync, s.sync); /* a Pentad belongs to the running sync */
        }
    }
    /* the oracle really exercises both branches */
    CHECK(with_sync > 400 && partial >= 3);
    CHECK(!ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLE_FIRST_PARTIAL_SYNC].sync.has);
    CHECK(!ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLE_LAST_PARTIAL_SYNC].sync.has);

    TEST_SECTION("syncs by number against the clock oracle");
    for (i = 0; i < ORACLE_CLOCK_SYNCS_BY_NUMBER_COUNT; i++) {
        const oracle_clock_sync_by_number_t *v = &ORACLE_CLOCK_SYNCS_BY_NUMBER[i];
        frc_sync_t s;
        memset(&s, 0x55, sizeof s);
        if (!v->ok) {
            CHECK(frc_sync_by_number(CAT, v->number, &s) == FRC_ERR_RANGE);
            CHECK_NOTE("sync %d", (int)v->number);
            continue;
        }
        CHECK(frc_sync_by_number(CAT, v->number, &s) == FRC_OK);
        CHECK_EQ_I64(s.sync, v->number);
        CHECK_EQ_I64(s.start, v->start_us);
        CHECK_EQ_I64(s.end, v->end_us);
        CHECK_EQ_I64(s.nominal, v->nominal_us);
        CHECK_EQ_I64(s.drift_us, v->drift_us);
        CHECK_NOTE("sync %d", (int)v->number);
        /* "phase and quarter are 0 and next is {0, FRC_TIME_NONE}" */
        CHECK(s.phase == 0 && s.quarter == 0 && s.next.name == 0 && s.next.time == FRC_TIME_NONE);
    }

    TEST_SECTION("Pentads by number against the clock oracle");
    for (i = 0; i < ORACLE_CLOCK_PENTADS_BY_NUMBER_COUNT; i++) {
        const oracle_clock_pentad_by_number_t *v = &ORACLE_CLOCK_PENTADS_BY_NUMBER[i];
        frc_pentad_t p;
        if (!v->ok) {
            CHECK(frc_pentad_by_number(CAT, v->sync, v->index, &p) == FRC_ERR_RANGE);
            CHECK_NOTE("Pentad %d of sync %d", v->index, (int)v->sync);
            continue;
        }
        CHECK(frc_pentad_by_number(CAT, v->sync, v->index, &p) == FRC_OK);
        CHECK_EQ_I64(p.sync, v->sync);
        CHECK_EQ_U64(p.index, v->index);
        CHECK_EQ_U64(p.helions, v->helions);
        CHECK_EQ_U64(p.helion, 0); /* "helion is 0" */
        CHECK_EQ_I64(p.start, v->start_us);
        CHECK_EQ_I64(p.end, v->end_us);
        CHECK_NOTE("Pentad %d of sync %d", v->index, (int)v->sync);
    }
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                 */
/* ------------------------------------------------------------------------- */

/* Everything a sync reading promises about itself, at one instant of a whole sync. */
static void check_sync_reading(const frc_catalogue_t *cat, frc_time_t t)
{
    frc_sync_t s, by_number, other;
    frc_time_t anchor = frc_catalogue_instant(cat, cat->anchor);
    double exact;

    CHECK(frc_sync_at(cat, t, &s) == FRC_OK);
    CHECK(s.start <= t && t < s.end);
    CHECK_EQ_I64(s.nominal, anchor + (int64_t)s.sync * SYNC_US);
    CHECK_EQ_I64(s.drift_us, s.start - s.nominal);
    CHECK(frc_sync_by_number(cat, s.sync, &by_number) == FRC_OK);
    CHECK(same_sync(&s, &by_number));

    /* time-linear phase. The exact boundaries are within a microsecond of start and
     * end, so the rounded ones give the phase to about 2^32 / (4 days in us) = 0.012 */
    exact = (double)(t - s.start) / (double)(s.end - s.start) * 4294967296.0;
    CHECK((double)s.phase > exact - 2.0 && (double)s.phase < exact + 2.0);
    CHECK_EQ_U64(s.quarter, s.phase >> 30);

    /* the next quarter of the sync: named like the running quarter, strictly after t,
     * and the FIRST microsecond that reads the next quarter */
    CHECK_EQ_U64(s.next.name, s.quarter);
    CHECK(s.next.time > t && s.next.time <= s.end);
    CHECK(frc_sync_at(cat, s.next.time - 1, &other) == FRC_OK);
    CHECK(other.sync == s.sync && other.quarter == s.quarter);
    if (s.quarter == FRC_END) {
        CHECK_EQ_I64(s.next.time, s.end);
    } else {
        CHECK(frc_sync_at(cat, s.next.time, &other) == FRC_OK);
        CHECK(other.sync == s.sync && other.quarter == s.quarter + 1);
        /* quarters of a sync fall about a day apart (S7.3) */
        CHECK(s.next.time - t <= FRC_US_PER_DAY + FRC_US_PER_SECOND);
    }
    CHECK_NOTE("t = %" PRId64, t);
}

/* What a sync odometer promises at one instant: digits in range, and a next change that
 * is the first microsecond of the next step. */
static void check_sync_odometer(const frc_catalogue_t *cat, frc_time_t t, uint8_t strokes, uint8_t digits)
{
    frc_sync_t s;
    frc_odometer_t odo, before, after;
    uint64_t turn = 1;
    int j;

    CHECK(frc_sync_at(cat, t, &s) == FRC_OK);
    memset(&odo, 0x55, sizeof odo);
    CHECK(frc_sync_odometer(cat, t, strokes, digits, &odo) == FRC_OK);
    CHECK(odo.count == digits && odo.strokes == strokes && odo.states == frc_glyph_states(strokes));
    for (j = 0; j < FRC_MAX_DIGITS; j++) {
        CHECK(odo.digit[j] < odo.states && (j < digits || odo.digit[j] == 0));
        if (j < digits) {
            turn *= odo.states;
        }
    }
    /* the coarsest digit moves with the phase: floor(phase * q) up to the last bit */
    CHECK(odo.digit[0] == (((uint64_t)s.phase * odo.states) >> 32) ||
          odo.digit[0] == ((((uint64_t)s.phase + 1) * odo.states) >> 32));

    CHECK(odo.next_change > t && odo.next_change <= s.end);
    CHECK(frc_sync_odometer(cat, odo.next_change - 1, strokes, digits, &before) == FRC_OK);
    CHECK_EQ_U64(step_of(&before), step_of(&odo));
    if (step_of(&odo) == turn - 1) {
        CHECK_EQ_I64(odo.next_change, s.end); /* the last step ends with the sync */
    } else {
        /* the shortest step, 4 days / 484^4, still lasts 6 us: exactly one step further */
        CHECK(frc_sync_odometer(cat, odo.next_change, strokes, digits, &after) == FRC_OK);
        CHECK_EQ_U64(step_of(&after), step_of(&odo) + 1);
    }
    CHECK_NOTE("t = %" PRId64 ", %d strokes x %d", t, strokes, digits);
}

static void test_sync_properties(void)
{
    int i;
    uint8_t strokes, digits;

    TEST_SECTION("sync reading properties");
    for (i = 0; i < ORACLE_CLOCK_SAMPLES_COUNT; i++) {
        const oracle_clock_sample_t *v = &ORACLE_CLOCK_SAMPLES[i];
        if (!v->in_range || !v->sync.has) {
            continue;
        }
        check_sync_reading(CAT, v->t_us);
        check_sync_reading(CAT, v->t_us + 1 + i); /* off the millisecond grid */
        check_sync_reading(CAT, v->sync.start_us);
        check_sync_reading(CAT, v->sync.end_us - 1);
        if (i % 7 == 0) {
            for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
                for (digits = 1; digits <= FRC_MAX_DIGITS; digits++) {
                    check_sync_odometer(CAT, v->t_us + i, strokes, digits);
                }
            }
            check_sync_odometer(CAT, v->sync.start_us, 2, 3);
            check_sync_odometer(CAT, v->sync.end_us - 1, 2, 3);
            check_sync_odometer(CAT, v->sync.end_us - 1, 5, 4);
        }
    }
    {
        /* at a sync's first microsecond every odometer reads zero (the exact start is at
         * most a microsecond earlier: far less than one step of 6 us) */
        frc_sync_t s;
        frc_odometer_t odo;
        CHECK(frc_sync_by_number(CAT, 4820, &s) == FRC_OK);
        for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
            CHECK(frc_sync_odometer(CAT, s.start, strokes, FRC_MAX_DIGITS, &odo) == FRC_OK);
            CHECK_EQ_U64(step_of(&odo), 0);
        }
        /* S7.3: "3 two-stroke glyphs: 6 h, 22.5 min and 84 s per state" */
        CHECK(frc_sync_odometer(CAT, s.start, 2, 3, &odo) == FRC_OK);
        CHECK(odo.next_change - s.start > FRC_TIME_FROM_S(84) && odo.next_change - s.start < FRC_TIME_FROM_S(85));
        CHECK(frc_sync_odometer(CAT, s.start, 2, 1, &odo) == FRC_OK);
        CHECK(odo.next_change - s.start > FRC_TIME_FROM_S(6 * 3600 - 1) &&
              odo.next_change - s.start < FRC_TIME_FROM_S(6 * 3600));
        /* "2 three-stroke glyphs: 1.85 h and 2.1 min per state" */
        CHECK(frc_sync_odometer(CAT, s.start, 3, 2, &odo) == FRC_OK);
        CHECK(odo.next_change - s.start > FRC_TIME_FROM_S(127) && odo.next_change - s.start < FRC_TIME_FROM_S(128));
    }
}

static void test_every_sync(void)
{
    int32_t n;
    frc_sync_t s, previous;
    frc_time_t anchor = frc_catalogue_instant(CAT, CAT->anchor);
    int64_t count = 0;

    /* The whole syncs of Saros 141 are -32927 .. 80671 (oracle). Walk all of them: they
     * tile the catalogue without a gap, each about ten seconds short of four days. */
    TEST_SECTION("every whole sync of the catalogue");
    CHECK(frc_sync_by_number(CAT, ORACLE_CLOCK_SYNC_FIRST_WHOLE, &previous) == FRC_OK);
    CHECK(frc_sync_by_number(CAT, ORACLE_CLOCK_SYNC_FIRST_WHOLE - 1, &s) == FRC_ERR_RANGE);
    CHECK(frc_sync_by_number(CAT, ORACLE_CLOCK_SYNC_LAST_WHOLE + 1, &s) == FRC_ERR_RANGE);
    CHECK(frc_sync_by_number(CAT, INT32_MIN, &s) == FRC_ERR_RANGE);
    CHECK(frc_sync_by_number(CAT, INT32_MAX, &s) == FRC_ERR_RANGE);
    CHECK(previous.start > frc_catalogue_first(CAT)); /* a partial sync comes before it */
    CHECK(previous.start - frc_catalogue_first(CAT) < SYNC_US);
    for (n = ORACLE_CLOCK_SYNC_FIRST_WHOLE + 1; n <= ORACLE_CLOCK_SYNC_LAST_WHOLE; n++) {
        int ok = frc_sync_by_number(CAT, n, &s) == FRC_OK && s.sync == n && s.start == previous.end &&
                 s.end > s.start && s.nominal == anchor + (int64_t)n * SYNC_US &&
                 s.drift_us == s.start - s.nominal &&
                 /* S7.4: real intervals put the offset between -14.0 s and -8.1 s; the
                  * drift never runs forward */
                 s.end - s.start - SYNC_US > -14 * FRC_US_PER_SECOND &&
                 s.end - s.start - SYNC_US < -8 * FRC_US_PER_SECOND && s.drift_us < previous.drift_us;
        count += ok;
        if (!ok) {
            CHECK(ok);
            CHECK_NOTE("sync %d", (int)n);
        }
        previous = s;
    }
    CHECK_EQ_I64(count, ORACLE_CLOCK_SYNC_LAST_WHOLE - ORACLE_CLOCK_SYNC_FIRST_WHOLE);
    CHECK(frc_catalogue_last(CAT) - previous.end > 0 && frc_catalogue_last(CAT) - previous.end < SYNC_US);

    TEST_SECTION("the partial syncs at the catalogue's edges");
    {
        frc_time_t first = frc_catalogue_first(CAT), last = frc_catalogue_last(CAT);
        frc_sync_t first_whole, last_whole;
        frc_odometer_t odo;
        CHECK(frc_sync_by_number(CAT, ORACLE_CLOCK_SYNC_FIRST_WHOLE, &first_whole) == FRC_OK);
        CHECK(frc_sync_by_number(CAT, ORACLE_CLOCK_SYNC_LAST_WHOLE, &last_whole) == FRC_OK);
        /* in range, yet no sync reading: its start or end would be outside (S7.1) */
        CHECK(frc_sync_at(CAT, first, &s) == FRC_ERR_RANGE);
        CHECK(frc_sync_at(CAT, first_whole.start - 1, &s) == FRC_ERR_RANGE);
        CHECK(frc_sync_at(CAT, first_whole.start, &s) == FRC_OK && s.sync == ORACLE_CLOCK_SYNC_FIRST_WHOLE);
        CHECK(frc_sync_at(CAT, last_whole.end - 1, &s) == FRC_OK && s.sync == ORACLE_CLOCK_SYNC_LAST_WHOLE);
        CHECK(frc_sync_at(CAT, last_whole.end, &s) == FRC_ERR_RANGE);
        CHECK(frc_sync_at(CAT, last, &s) == FRC_ERR_RANGE);
        CHECK(frc_sync_odometer(CAT, first, 2, 3, &odo) == FRC_ERR_RANGE);
        CHECK(frc_sync_odometer(CAT, last, 2, 3, &odo) == FRC_ERR_RANGE);
        /* and outside the catalogue */
        CHECK(frc_sync_at(CAT, first - 1, &s) == FRC_ERR_RANGE);
        CHECK(frc_sync_at(CAT, last + 1, &s) == FRC_ERR_RANGE);
        CHECK(frc_sync_at(CAT, INT64_MAX, &s) == FRC_ERR_RANGE);
        CHECK(frc_sync_at(CAT, FRC_TIME_NONE, &s) == FRC_ERR_RANGE);
    }
}

static void test_pentad_properties(void)
{
    int i;
    uint8_t index;
    frc_time_t first = frc_catalogue_first(CAT), last = frc_catalogue_last(CAT);
    uint64_t anchor_cycle = (uint64_t)CAT->anchor * HELION_PREFIX / 4;

    TEST_SECTION("Pentad properties");
    for (i = 0; i < ORACLE_CLOCK_SAMPLES_COUNT; i += 3) {
        const oracle_clock_sample_t *v = &ORACLE_CLOCK_SAMPLES[i];
        frc_pentad_t p, by_number;
        frc_cycle_t helion, edge;
        int64_t first_helion;

        if (!v->in_range) {
            continue;
        }
        CHECK(frc_pentad_at(CAT, v->t_us, &p) == FRC_OK);
        CHECK(p.start <= v->t_us && (v->t_us < p.end || v->t_us == last));
        CHECK(p.index <= 3 && p.helions == (p.index == 3 ? 4 : 5) && p.helion >= 1 && p.helion <= p.helions);
        CHECK(frc_pentad_by_number(CAT, p.sync, p.index, &by_number) == FRC_OK);
        CHECK(by_number.sync == p.sync && by_number.index == p.index && by_number.helions == p.helions &&
              by_number.start == p.start && by_number.end == p.end && by_number.helion == 0);

        /* S7.5 on whole Helions: the running Helion is the Pentad's helion-th */
        CHECK(frc_cycle_at(CAT, v->t_us, FRC_SYNC_LEVEL, &helion) == FRC_OK);
        first_helion = (int64_t)anchor_cycle + (int64_t)FRC_SYNC_CYCLES * p.sync + 5 * p.index;
        CHECK_EQ_I64((int64_t)helion.cycle - first_helion + 1, p.helion);
        if (first_helion >= 0) {
            CHECK(frc_cycle_by_index(CAT, FRC_SYNC_LEVEL, (uint64_t)first_helion, &edge) == FRC_OK);
            CHECK_EQ_I64(p.start, edge.start);
        } else {
            CHECK_EQ_I64(p.start, first); /* cut at the catalogue's first instant */
        }
        if (frc_cycle_by_index(CAT, FRC_SYNC_LEVEL, (uint64_t)(first_helion + p.helions - 1), &edge) == FRC_OK &&
            !edge.clipped) {
            CHECK_EQ_I64(p.end, edge.end);
        } else {
            CHECK_EQ_I64(p.end, last); /* cut at its closing instant */
        }
        CHECK_NOTE("sample %d (%s)", i, v->label);
    }

    TEST_SECTION("Pentads tile their sync");
    {
        static const int32_t SYNCS[] = {ORACLE_CLOCK_SYNC_FIRST_WHOLE, -2380, -1, 0, 1, 2375, 4820, 7987,
                                        ORACLE_CLOCK_SYNC_LAST_WHOLE};
        size_t k;
        for (k = 0; k < sizeof SYNCS / sizeof SYNCS[0]; k++) {
            frc_sync_t s;
            frc_pentad_t p, next;
            CHECK(frc_sync_by_number(CAT, SYNCS[k], &s) == FRC_OK);
            CHECK(frc_pentad_by_number(CAT, SYNCS[k], 0, &p) == FRC_OK);
            CHECK_EQ_I64(p.start, s.start);
            for (index = 1; index <= 3; index++) {
                CHECK(frc_pentad_by_number(CAT, SYNCS[k], index, &next) == FRC_OK);
                CHECK_EQ_I64(next.start, p.end);
                /* "every Pentad is about a day": 5 Helions = 25.26 h */
                CHECK(p.end - p.start > 25 * INT64_C(3600000000) && p.end - p.start < 26 * INT64_C(3600000000));
                p = next;
            }
            CHECK_EQ_I64(p.end, s.end);
            /* the Hypopentad: 4 Helions = 20.21 h */
            CHECK(p.helions == 4 && p.end - p.start > 20 * INT64_C(3600000000) &&
                  p.end - p.start < 21 * INT64_C(3600000000));
        }
    }

    TEST_SECTION("Pentads at the catalogue's edges");
    {
        frc_pentad_t p;
        /* README of the oracle: sync -32928 is the cut first sync; its Pentad I lies before
         * the catalogue and Pentad II starts at E[0]. Sync 80672 is the cut last one. */
        CHECK(frc_pentad_at(CAT, first, &p) == FRC_OK);
        CHECK(p.sync == ORACLE_CLOCK_SYNC_FIRST_WHOLE - 1 && p.start == first && p.end > first);
        CHECK(p.helion >= 1 && p.helion <= p.helions);
        CHECK(frc_pentad_by_number(CAT, ORACLE_CLOCK_SYNC_FIRST_WHOLE - 1, 0, &p) == FRC_ERR_RANGE);
        CHECK(frc_pentad_by_number(CAT, ORACLE_CLOCK_SYNC_FIRST_WHOLE - 1, 3, &p) == FRC_OK);
        CHECK(frc_pentad_by_number(CAT, ORACLE_CLOCK_SYNC_FIRST_WHOLE - 2, 3, &p) == FRC_ERR_RANGE);
        /* the closing instant belongs to the last Pentad, like every reading (S2.2) */
        CHECK(frc_pentad_at(CAT, last, &p) == FRC_OK);
        CHECK(p.sync == ORACLE_CLOCK_SYNC_LAST_WHOLE + 1 && p.end == last && p.start < last);
        {
            frc_pentad_t inside;
            CHECK(frc_pentad_at(CAT, last - 1, &inside) == FRC_OK);
            CHECK(inside.sync == p.sync && inside.index == p.index && inside.helion == p.helion &&
                  inside.start == p.start && inside.end == p.end);
        }
        CHECK(frc_pentad_by_number(CAT, ORACLE_CLOCK_SYNC_LAST_WHOLE + 1, 0, &p) == FRC_OK);
        CHECK(frc_pentad_by_number(CAT, ORACLE_CLOCK_SYNC_LAST_WHOLE + 1, 3, &p) == FRC_ERR_RANGE);
        CHECK(frc_pentad_by_number(CAT, ORACLE_CLOCK_SYNC_LAST_WHOLE + 2, 0, &p) == FRC_ERR_RANGE);
        CHECK(frc_pentad_by_number(CAT, INT32_MIN, 0, &p) == FRC_ERR_RANGE);
        CHECK(frc_pentad_by_number(CAT, INT32_MAX, 3, &p) == FRC_ERR_RANGE);
        CHECK(frc_pentad_at(CAT, first - 1, &p) == FRC_ERR_RANGE);
        CHECK(frc_pentad_at(CAT, last + 1, &p) == FRC_ERR_RANGE);
    }

    TEST_SECTION("a Pentad that would start exactly at the closing instant");
    {
        /* Saros 141 cannot reach this edge (69 * 125125 quarter Helions is odd: its closing
         * instant is inside a Helion). Five eclipses anchored at the first span 4 Saros =
         * 125125 Helions = 6585 syncs and 10 Helions: the catalogue closes exactly where
         * Pentad II of sync 6585 ends and Pentad III would begin. "Wholly outside" includes
         * a Pentad that only touches the closing instant: it has no instant of its own. */
        static const int64_t FIVE_MS[5] = {INT64_C(100000000000), INT64_C(668000000000), INT64_C(1236000000000),
                                           INT64_C(1804000000000), INT64_C(2372000000000)};
        frc_catalogue_t five;
        frc_pentad_t p, at_end;
        frc_sync_t s;
        frc_time_t five_last;
        CHECK(frc_catalogue_init(&five, FIVE_MS, 5, 0) == FRC_OK);
        CHECK_EQ_U64(five.anchor, 0);
        five_last = frc_catalogue_last(&five);
        CHECK(frc_pentad_by_number(&five, 6585, 0, &p) == FRC_OK);
        CHECK(p.helions == 5 && p.start < p.end && p.end < five_last);
        CHECK(frc_pentad_by_number(&five, 6585, 1, &p) == FRC_OK);
        CHECK(p.helions == 5 && p.start < p.end);
        CHECK_EQ_I64(p.end, five_last);
        memset(&p, 0x33, sizeof p);
        CHECK(frc_pentad_by_number(&five, 6585, 2, &p) == FRC_ERR_RANGE);
        CHECK(p.sync == 0x33333333 && p.start == INT64_C(0x3333333333333333)); /* untouched */
        CHECK(frc_pentad_by_number(&five, 6585, 3, &p) == FRC_ERR_RANGE);
        CHECK(frc_pentad_by_number(&five, 6586, 0, &p) == FRC_ERR_RANGE);
        /* the closing instant reads the last real Pentad, in its last Helion */
        CHECK(frc_pentad_at(&five, five_last, &at_end) == FRC_OK);
        CHECK(at_end.sync == 6585 && at_end.index == 1 && at_end.helion == 5 && at_end.end == five_last);
        /* sync 6585 is cut after its tenth Helion: no whole sync, no reading */
        CHECK(frc_sync_by_number(&five, 6584, &s) == FRC_OK);
        CHECK(frc_sync_by_number(&five, 6585, &s) == FRC_ERR_RANGE);
        CHECK(frc_sync_at(&five, five_last, &s) == FRC_ERR_RANGE);
        /* the mirror image at the first instant: Pentad I of sync 0 starts there, and the
         * Hypopentad of sync -1 only touches it */
        CHECK(frc_pentad_by_number(&five, 0, 0, &p) == FRC_OK);
        CHECK_EQ_I64(p.start, frc_catalogue_first(&five));
        CHECK(frc_pentad_by_number(&five, -1, 3, &p) == FRC_ERR_RANGE);
    }
    {
        /* The uneven series of test_other_anchor-style tables: anchored at the LAST eclipse
         * (the only multiple of 4 after 1972), sync 0 would start at the closing instant. */
        static const int64_t LATE_ANCHOR_MS[5] = {INT64_C(-2200000000000), INT64_C(-1632000000000),
                                                  INT64_C(-1064000000000), INT64_C(-496000000000),
                                                  INT64_C(72000000000)};
        frc_catalogue_t late;
        frc_pentad_t p;
        frc_sync_t s;
        CHECK(frc_catalogue_init(&late, LATE_ANCHOR_MS, 5, 0) == FRC_OK);
        CHECK_EQ_U64(late.anchor, 4);
        CHECK(frc_pentad_by_number(&late, 0, 0, &p) == FRC_ERR_RANGE);
        CHECK(frc_sync_by_number(&late, 0, &s) == FRC_ERR_RANGE);
        CHECK(frc_pentad_by_number(&late, -1, 3, &p) == FRC_OK);
        CHECK(p.helions == 4 && p.start < p.end);
        CHECK_EQ_I64(p.end, frc_catalogue_last(&late));
        CHECK(frc_sync_by_number(&late, -1, &s) == FRC_OK);
        CHECK_EQ_I64(s.end, frc_catalogue_last(&late));
    }
}

static void test_invalid_arguments(void)
{
    frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
    frc_sync_t s;
    frc_odometer_t odo;
    frc_pentad_t p;
    frc_catalogue_t bad;

    TEST_SECTION("invalid arguments");
    memset(&s, 0x55, sizeof s);
    memset(&odo, 0x55, sizeof odo);
    memset(&p, 0x55, sizeof p);
    CHECK(frc_sync_at(NULL, t, &s) == FRC_ERR_ARG);
    CHECK(frc_sync_at(CAT, t, NULL) == FRC_ERR_ARG);
    CHECK(frc_sync_by_number(NULL, 0, &s) == FRC_ERR_ARG);
    CHECK(frc_sync_by_number(CAT, 0, NULL) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(NULL, t, 2, 3, &odo) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(CAT, t, 2, 3, NULL) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(CAT, t, 0, 3, &odo) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(CAT, t, FRC_MAX_STROKES + 1, 3, &odo) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(CAT, t, 2, 0, &odo) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(CAT, t, 2, FRC_MAX_DIGITS + 1, &odo) == FRC_ERR_ARG);
    /* a bad stroke count is an argument error even outside the catalogue */
    CHECK(frc_sync_odometer(CAT, INT64_MAX, 0, 3, &odo) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(CAT, INT64_MAX, 2, 3, &odo) == FRC_ERR_RANGE);
    CHECK(frc_sync_odometer(CAT, frc_catalogue_first(CAT), 2, 3, &odo) == FRC_ERR_RANGE);
    CHECK(odo.count == 0x55 && odo.states == 0x5555 && odo.digit[0] == 0x5555); /* untouched by any error */
    CHECK(frc_pentad_at(NULL, t, &p) == FRC_ERR_ARG);
    CHECK(frc_pentad_at(CAT, t, NULL) == FRC_ERR_ARG);
    CHECK(frc_pentad_by_number(NULL, 0, 0, &p) == FRC_ERR_ARG);
    CHECK(frc_pentad_by_number(CAT, 0, 0, NULL) == FRC_ERR_ARG);
    CHECK(frc_pentad_by_number(CAT, 0, 4, &p) == FRC_ERR_ARG);
    CHECK(frc_pentad_by_number(CAT, 0, 255, &p) == FRC_ERR_ARG);
    CHECK(s.sync == 0x55555555 && p.sync == 0x55555555); /* untouched */

    /* S7.1: the anchor must start a Helion, "A * P(L) must be divisible by 4". P(6) is
     * odd, so only a multiple of 4 can; and it must be an eclipse of the catalogue. */
    bad = FRC_SAROS_141;
    bad.anchor = 21;
    CHECK(frc_sync_at(&bad, t, &s) == FRC_ERR_ARG);
    CHECK(frc_sync_by_number(&bad, 0, &s) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(&bad, t, 2, 3, &odo) == FRC_ERR_ARG);
    CHECK(frc_pentad_at(&bad, t, &p) == FRC_ERR_ARG);
    CHECK(frc_pentad_by_number(&bad, 0, 0, &p) == FRC_ERR_ARG);
    bad.anchor = 22;
    CHECK(frc_sync_at(&bad, t, &s) == FRC_ERR_ARG);
    bad.anchor = 72; /* a multiple of 4, but there is no eclipse 72 */
    CHECK(frc_sync_at(&bad, t, &s) == FRC_ERR_ARG);
    CHECK(frc_pentad_at(&bad, t, &p) == FRC_ERR_ARG);
    bad.instants_ms = NULL;
    bad.anchor = 20;
    CHECK(frc_sync_at(&bad, t, &s) == FRC_ERR_ARG);
}

/* ------------------------------------------------------------------------- */
/* Other catalogues                                                           */
/* ------------------------------------------------------------------------- */

static void test_other_anchor(void)
{
    /* The same series counted from another anchor: syncs are renumbered and shifted,
     * the Helion grid they stand on is the same. */
    frc_catalogue_t cat = FRC_SAROS_141;
    frc_sync_t s, reference;
    frc_pentad_t p;
    frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
    frc_time_t anchor;
    int i;

    TEST_SECTION("another anchor");
    cat.anchor = 0;
    anchor = frc_catalogue_instant(&cat, 0);
    CHECK(frc_sync_at(&cat, anchor, &s) == FRC_OK);
    CHECK(s.sync == 0 && s.start == anchor && s.drift_us == 0 && s.phase == 0);
    CHECK(frc_sync_by_number(&cat, -1, &s) == FRC_ERR_RANGE); /* nothing before the first eclipse */
    CHECK(frc_pentad_at(&cat, anchor, &p) == FRC_OK);
    CHECK(p.sync == 0 && p.index == 0 && p.helion == 1 && p.start == anchor);
    CHECK(frc_pentad_by_number(&cat, -1, 3, &p) == FRC_ERR_RANGE);
    for (i = 0; i < ORACLE_CLOCK_SAMPLES_COUNT; i += 5) {
        if (ORACLE_CLOCK_SAMPLES[i].in_range && frc_sync_at(&cat, ORACLE_CLOCK_SAMPLES[i].t_us, &s) == FRC_OK) {
            check_sync_reading(&cat, ORACLE_CLOCK_SAMPLES[i].t_us);
            check_sync_odometer(&cat, ORACLE_CLOCK_SAMPLES[i].t_us, 2, 3);
        }
    }
    /* anchor 24 is 4 Saros = 125125 Helions later; 125125 = 19 * 6585 + 10, so its syncs
     * start 10 Helions into the reference's */
    cat.anchor = 24;
    CHECK(frc_sync_at(&cat, t, &s) == FRC_OK);
    CHECK(frc_sync_at(CAT, t, &reference) == FRC_OK);
    CHECK(s.sync < 0 && s.start != reference.start);
    {
        frc_cycle_t a, b;
        CHECK(frc_cycle_at(&cat, s.start, FRC_SYNC_LEVEL, &a) == FRC_OK);
        CHECK(frc_cycle_at(CAT, reference.start, FRC_SYNC_LEVEL, &b) == FRC_OK);
        CHECK(a.start == s.start && b.start == reference.start);
        CHECK_EQ_I64(((int64_t)a.cycle - (int64_t)b.cycle + 19 * 100000) % 19, 10);
    }
    check_sync_reading(&cat, t);
}

/* A made-up series of 77 eclipses, Saros-like but uneven, starting in 1980. */
#define SYNTHETIC_COUNT 77
static int64_t synthetic_ms[SYNTHETIC_COUNT];

static void build_synthetic(frc_catalogue_t *cat)
{
    int k;
    int64_t at = INT64_C(315532800123);
    for (k = 0; k < SYNTHETIC_COUNT; k++) {
        synthetic_ms[k] = at;
        at += INT64_C(568972877000) + (int64_t)((k * 7919) % 1000) * 86400 - 43200000;
    }
    CHECK(frc_catalogue_init(cat, synthetic_ms, SYNTHETIC_COUNT, 0) == FRC_OK);
}

static void test_series_closing_on_a_sync(void)
{
    /* 76 Saros after the anchor is 76 * 125125 / 4 = 19 * 125125 Helions: a catalogue of
     * 77 eclipses anchored at its first one closes EXACTLY on the end of a sync. Only such
     * a series has a sync reading at its closing instant. */
    frc_catalogue_t cat;
    frc_sync_t s, inside;
    frc_odometer_t odo, odo_inside;
    frc_pentad_t p;
    frc_time_t first, last;
    int32_t last_sync = HELION_PREFIX - 1;
    int i;

    TEST_SECTION("a series that closes on a sync");
    build_synthetic(&cat);
    CHECK_EQ_U64(cat.anchor, 0); /* S7.2: the first A = 0 (mod 4) on or after 1972 */
    first = frc_catalogue_first(&cat);
    last = frc_catalogue_last(&cat);

    CHECK(frc_sync_at(&cat, first, &s) == FRC_OK);
    CHECK(s.sync == 0 && s.start == first && s.phase == 0);
    CHECK(frc_sync_by_number(&cat, last_sync, &s) == FRC_OK);
    CHECK_EQ_I64(s.end, last);
    CHECK(frc_sync_by_number(&cat, last_sync + 1, &s) == FRC_ERR_RANGE);
    CHECK(frc_sync_by_number(&cat, -1, &s) == FRC_ERR_RANGE);

    /* the closing instant reads as the microsecond before it: still inside the last
     * sync, phase just under 1, and nothing is scheduled after it */
    CHECK(frc_sync_at(&cat, last, &s) == FRC_OK);
    CHECK(frc_sync_at(&cat, last - 1, &inside) == FRC_OK);
    CHECK(s.sync == last_sync && same_sync(&s, &inside));
    CHECK_EQ_U64(s.phase, inside.phase);
    CHECK(s.phase > 0xFFFFFF00u && s.quarter == FRC_END && s.next.name == FRC_END);
    CHECK(s.helion == FRC_SYNC_CYCLES - 1 && inside.helion == FRC_SYNC_CYCLES - 1); /* the last Helion */
    CHECK_EQ_I64(s.next.time, FRC_TIME_NONE);
    CHECK_EQ_I64(inside.next.time, last);
    CHECK(frc_sync_odometer(&cat, last, 2, 3, &odo) == FRC_OK);
    CHECK(frc_sync_odometer(&cat, last - 1, 2, 3, &odo_inside) == FRC_OK);
    CHECK(odo.digit[0] == 15 && odo.digit[1] == 15 && odo.digit[2] == 15);
    CHECK_EQ_U64(step_of(&odo), step_of(&odo_inside));
    CHECK_EQ_I64(odo.next_change, FRC_TIME_NONE);
    CHECK_EQ_I64(odo_inside.next_change, last);
    CHECK(frc_sync_at(&cat, last + 1, &s) == FRC_ERR_RANGE);
    CHECK(frc_pentad_at(&cat, last, &p) == FRC_OK);
    CHECK(p.sync == last_sync && p.index == 3 && p.helion == 4 && p.end == last);

    /* readings across the uneven intervals, and at the eclipses a sync straddles */
    for (i = 1; i < SYNTHETIC_COUNT - 1; i += 5) {
        frc_time_t eclipse = frc_catalogue_instant(&cat, (uint16_t)i);
        check_sync_reading(&cat, eclipse);
        check_sync_reading(&cat, eclipse - 1);
        check_sync_reading(&cat, eclipse + 123456789);
        check_sync_odometer(&cat, eclipse, 2, 3);
        check_sync_odometer(&cat, eclipse - 1, 3, 2);
        check_sync_odometer(&cat, eclipse + 123456789, 5, 4);
    }
    /* consecutive syncs share their boundary, also across an eclipse */
    {
        frc_sync_t previous;
        int32_t n;
        CHECK(frc_sync_by_number(&cat, 6500, &previous) == FRC_OK);
        for (n = 6501; n < 6700; n++) { /* 125125 / 19 = 6585.5: eclipse 4 falls in sync 6585 */
            CHECK(frc_sync_by_number(&cat, n, &s) == FRC_OK);
            CHECK_EQ_I64(s.start, previous.end);
            previous = s;
        }
        CHECK(frc_sync_at(&cat, frc_catalogue_instant(&cat, 4), &s) == FRC_OK);
        CHECK_EQ_I64(s.sync, 6585);
        CHECK(s.start < frc_catalogue_instant(&cat, 4) && s.end > frc_catalogue_instant(&cat, 4));
    }
}

static void test_results_that_do_not_fit(void)
{
    frc_catalogue_t cat;
    frc_sync_t s;
    frc_odometer_t odo;
    frc_pentad_t p;

    TEST_SECTION("results that do not fit 64 bits");
    {
        /* Saros intervals of 31,700 years: a sync's length times P passes 2^64. The sync
         * cannot be read exactly, and says so; Pentads need no such product. */
        static const int64_t WIDE_MS[3] = {0, INT64_C(1000000000000000), INT64_C(2000000000000000)};
        cat.instants_ms = WIDE_MS;
        cat.count = 3;
        cat.series = 0;
        cat.anchor = 0;
        CHECK(frc_sync_at(&cat, FRC_TIME_FROM_MS(INT64_C(5)), &s) == FRC_ERR_OVERFLOW);
        CHECK(frc_sync_by_number(&cat, 0, &s) == FRC_ERR_OVERFLOW);
        CHECK(frc_sync_odometer(&cat, FRC_TIME_FROM_MS(INT64_C(5)), 2, 3, &odo) == FRC_ERR_OVERFLOW);
        CHECK(frc_pentad_at(&cat, FRC_TIME_FROM_MS(INT64_C(5)), &p) == FRC_OK);
        CHECK(p.sync == 0 && p.index == 0 && p.helion == 1 && p.start == 0);
    }
    {
        /* A series at the very end of the int64 range: the civil grid E[anchor] + n * 4
         * days leaves the range long before the syncs do (they are far shorter than 4
         * days here). The nominal start, and with it the sync, is refused. */
        static const int64_t LATE_MS[3] = {INT64_C(9223300000000000), INT64_C(9223350000000000),
                                           INT64_C(9223359900000000)};
        cat.instants_ms = LATE_MS;
        cat.count = 3;
        cat.series = 0;
        cat.anchor = 0;
        CHECK(frc_sync_by_number(&cat, 100, &s) == FRC_OK);
        CHECK_EQ_I64(s.nominal, FRC_TIME_FROM_MS(LATE_MS[0]) + 100 * SYNC_US);
        CHECK(frc_sync_by_number(&cat, 300, &s) == FRC_ERR_OVERFLOW);
        CHECK(frc_pentad_by_number(&cat, 300, 0, &p) == FRC_OK);
        CHECK(frc_sync_at(&cat, p.start, &s) == FRC_ERR_OVERFLOW);
    }
    {
        /* and at its very beginning, with the anchor at the far end: negative syncs */
        static const int64_t EARLY_MS[5] = {INT64_C(-9223359900000000), INT64_C(-9223350000000000),
                                            INT64_C(-9223340000000000), INT64_C(-9223330000000000),
                                            INT64_C(-9223300000000000)};
        cat.instants_ms = EARLY_MS;
        cat.count = 5;
        cat.series = 0;
        cat.anchor = 4;
        CHECK(frc_sync_by_number(&cat, -100, &s) == FRC_OK);
        CHECK_EQ_I64(s.nominal, FRC_TIME_FROM_MS(EARLY_MS[4]) - 100 * SYNC_US);
        CHECK(s.end <= FRC_TIME_FROM_MS(EARLY_MS[4]));
        CHECK(frc_sync_by_number(&cat, -300, &s) == FRC_ERR_OVERFLOW);
    }
}

/* 30,401 eclipses one millisecond apart at the very start of the int64 range: a "series"
 * whose syncs last 0.6 microseconds, there only to stretch the arithmetic. The table has
 * room for 32,500: the longer series reaches sync numbers whose civil grid time leaves the
 * int64 range (test_nominal_start_that_does_not_fit). */
#define DENSE_COUNT 30401
#define DENSE_LONG_COUNT 32500
static int64_t dense_ms[DENSE_LONG_COUNT];

static void test_drift_that_does_not_fit(void)
{
    frc_catalogue_t cat;
    frc_sync_t s;
    frc_pentad_t p;
    int k;

    TEST_SECTION("a drift that does not fit 64 bits");
    for (k = 0; k < DENSE_COUNT; k++) {
        dense_ms[k] = INT64_C(-9223000000000000) + k;
    }
    cat.instants_ms = dense_ms;
    cat.count = DENSE_COUNT;
    cat.series = 0;
    cat.anchor = 0;
    /* near the anchor everything fits: sync 10 starts 10 * 76 / 125125 ms after E[0] */
    memset(&s, 0x55, sizeof s);
    CHECK(frc_sync_by_number(&cat, 10, &s) == FRC_OK);
    CHECK_EQ_I64(s.start, FRC_TIME_FROM_MS(dense_ms[0]) + 7); /* ceil(6.07 us) */
    CHECK_EQ_I64(s.nominal, FRC_TIME_FROM_MS(dense_ms[0]) + 10 * SYNC_US);
    CHECK_EQ_I64(s.drift_us, s.start - s.nominal);
    /* a sync of 0.607 us is over before its own first microsecond: 7 us is in sync 11 */
    CHECK(frc_sync_at(&cat, s.start, &s) == FRC_OK);
    CHECK_EQ_I64(s.sync, 11);
    /* Sync 50,000,000 exists (the series has 50,050,000 of them) and starts 30 seconds
     * after E[0]; its civil grid time is 548,000 years later, still an int64. The drift
     * between the two is -1.7e19 us: it has no int64, and the sync is refused whole. */
    memset(&s, 0x55, sizeof s);
    CHECK(frc_sync_by_number(&cat, 50000000, &s) == FRC_ERR_OVERFLOW);
    CHECK(s.sync == 0x55555555 && s.start == INT64_C(0x5555555555555555)); /* untouched */
    CHECK(frc_pentad_by_number(&cat, 50000000, 0, &p) == FRC_OK);
    CHECK(frc_sync_at(&cat, p.start, &s) == FRC_ERR_OVERFLOW);
    CHECK(frc_sync_by_number(&cat, 50049999, &s) == FRC_ERR_OVERFLOW); /* the last one */
    CHECK(frc_sync_by_number(&cat, 50050000, &s) == FRC_ERR_RANGE);    /* beyond the last eclipse */
}

static void test_nominal_start_that_does_not_fit(void)
{
    /* nominal = E[anchor] + number * 4 days must fit int64. Two different things can go
     * wrong, and each needs its own test:
     *   (a) number * 4 days fits 64 bits, but the SUM passes INT64_MAX;
     *   (b) number * 4 days itself does not fit 64 bits.
     * With the anchor at -9.223e18 us the room up to INT64_MAX is 1.8446372e19 us, just
     * under 2^64 = 1.8446744e19: numbers 53,374,920 .. 53,375,995 are case (a), larger ones
     * case (b). In case (a) a sum that wrapped around would land near the sync's real
     * start again, with a drift that fits: only the test on the nominal start itself
     * stops it. The 32,500-eclipse series has 53,505,754 whole syncs, so all of them exist. */
    frc_catalogue_t cat;
    frc_sync_t s;
    frc_pentad_t p;
    int k;

    TEST_SECTION("a nominal start that does not fit 64 bits");
    for (k = 0; k < DENSE_LONG_COUNT; k++) {
        dense_ms[k] = INT64_C(-9223000000000000) + k;
    }
    cat.instants_ms = dense_ms;
    cat.count = DENSE_LONG_COUNT;
    cat.series = 0;
    cat.anchor = 0;
    /* the last number whose grid time is an int64: 53,374,919 * 345,600 s after the anchor */
    CHECK(frc_sync_by_number(&cat, 53374919, &s) == FRC_ERR_OVERFLOW); /* its DRIFT does not fit */
    memset(&s, 0x55, sizeof s);
    CHECK(frc_sync_by_number(&cat, 53374920, &s) == FRC_ERR_OVERFLOW); /* (a), by 3.2e11 us */
    CHECK(frc_sync_by_number(&cat, 53375000, &s) == FRC_ERR_OVERFLOW); /* (a) */
    CHECK(frc_sync_by_number(&cat, 53375995, &s) == FRC_ERR_OVERFLOW); /* (a), the last */
    CHECK(frc_sync_by_number(&cat, 53375996, &s) == FRC_ERR_OVERFLOW); /* (b), the first */
    CHECK(frc_sync_by_number(&cat, 53400000, &s) == FRC_ERR_OVERFLOW); /* (b) */
    CHECK(s.sync == 0x55555555 && s.start == INT64_C(0x5555555555555555)); /* untouched */
    /* the syncs are there: their Pentads read, and the reading at an instant refuses too */
    CHECK(frc_pentad_by_number(&cat, 53375000, 0, &p) == FRC_OK);
    CHECK(frc_sync_at(&cat, p.start, &s) == FRC_ERR_OVERFLOW);
    CHECK(frc_pentad_by_number(&cat, 53400000, 0, &p) == FRC_OK);
    CHECK(frc_sync_at(&cat, p.start, &s) == FRC_ERR_OVERFLOW);
    CHECK(frc_sync_by_number(&cat, 53505753, &s) == FRC_ERR_OVERFLOW); /* the last whole one */
    CHECK(frc_sync_by_number(&cat, 53505754, &s) == FRC_ERR_RANGE);    /* cut by the last eclipse */
}

/* 77 eclipses one second apart: 76 Saros are 19 * 125125 Helions, so this series closes on
 * a sync like the synthetic one, but its syncs last 76 s / 125125 = 607.39 MICROSECONDS.
 * One microsecond is a 600th of the phase: reading the closing instant "one microsecond
 * early" is visible in every bit of the result. */
#define TICKING_COUNT 77
static int64_t ticking_ms[TICKING_COUNT];

static void test_closing_phase_exactly(void)
{
    /* the sync's length in microseconds is 76,000,000 / 125,125 = LENGTH_N / LENGTH_D */
    const uint64_t LENGTH_N = 76000000, LENGTH_D = HELION_PREFIX;
    frc_catalogue_t cat;
    frc_sync_t s, inside, before;
    frc_odometer_t odo;
    frc_time_t last;
    uint64_t phase, step;
    int k;

    TEST_SECTION("the closing instant is read exactly one microsecond early");
    for (k = 0; k < TICKING_COUNT; k++) {
        ticking_ms[k] = INT64_C(946684800000) + 1000 * k; /* 2000-01-01, one per second */
    }
    CHECK(frc_catalogue_init(&cat, ticking_ms, TICKING_COUNT, 0) == FRC_OK);
    CHECK_EQ_U64(cat.anchor, 0);
    last = frc_catalogue_last(&cat);

    CHECK(frc_sync_at(&cat, last, &s) == FRC_OK);
    CHECK_EQ_I64(s.sync, HELION_PREFIX - 1);
    CHECK_EQ_I64(s.end, last);
    CHECK_EQ_I64(s.next.time, FRC_TIME_NONE);
    CHECK(s.quarter == FRC_END && s.next.name == FRC_END);
    /* phase = (L - 1 us) / L with L = LENGTH_N / LENGTH_D us: (LENGTH_N - LENGTH_D) / LENGTH_N.
     * Its floor at 2^32 (the product is below 2^59): */
    phase = (((uint64_t)1 << 32) * (LENGTH_N - LENGTH_D)) / LENGTH_N;
    CHECK_EQ_U64(s.phase, phase);
    /* the same reading as the microsecond before, and not the one two before */
    CHECK(frc_sync_at(&cat, last - 1, &inside) == FRC_OK);
    CHECK(same_sync(&s, &inside));
    CHECK_EQ_U64(inside.phase, phase);
    CHECK_EQ_I64(inside.next.time, last);
    CHECK(frc_sync_at(&cat, last - 2, &before) == FRC_OK);
    CHECK_EQ_U64(before.phase, (((uint64_t)1 << 32) * (LENGTH_N - 2 * LENGTH_D)) / LENGTH_N);
    CHECK(before.phase < s.phase);
    /* the odometer likewise: floor(phase * 16^3) on the exact phase */
    step = (4096 * (LENGTH_N - LENGTH_D)) / LENGTH_N;
    CHECK(frc_sync_odometer(&cat, last, 2, 3, &odo) == FRC_OK);
    CHECK_EQ_U64(step_of(&odo), step);
    CHECK_EQ_I64(odo.next_change, FRC_TIME_NONE);
    CHECK(frc_sync_odometer(&cat, last - 1, 2, 3, &odo) == FRC_OK);
    CHECK_EQ_U64(step_of(&odo), step);
    CHECK(frc_sync_odometer(&cat, last - 2, 2, 3, &odo) == FRC_OK);
    CHECK_EQ_U64(step_of(&odo), (4096 * (LENGTH_N - 2 * LENGTH_D)) / LENGTH_N);
    CHECK(step_of(&odo) < step);
}


/* ------------------------------------------------------------------------- */
/* The new fields and their layout (an ABI the bindings read by offset)       */
/* ------------------------------------------------------------------------- */

static void test_sun_places(void)
{
    frc_sync_t sync;
    frc_pentad_t pentad;
    int32_t number;
    uint8_t index;

    TEST_SECTION("frc_sync_t.helion and frc_pentad_t.first: layout");
    /* the fields fill padding: no other offset and not the size moved */
    CHECK_EQ_U64(offsetof(frc_sync_t, quarter), 4);
    CHECK_EQ_U64(offsetof(frc_sync_t, helion), 5);
    CHECK_EQ_U64(offsetof(frc_pentad_t, helions), 6);
    CHECK_EQ_U64(offsetof(frc_pentad_t, first), 7);
    if (_Alignof(int64_t) == 8) {
        CHECK_EQ_U64(offsetof(frc_sync_t, phase), 8);
        CHECK_EQ_U64(offsetof(frc_sync_t, start), 16);
        CHECK_EQ_U64(offsetof(frc_sync_t, next), 48);
        CHECK_EQ_U64(sizeof(frc_sync_t), 64);
        CHECK_EQ_U64(offsetof(frc_pentad_t, start), 8);
        CHECK_EQ_U64(offsetof(frc_pentad_t, end), 16);
        CHECK_EQ_U64(sizeof(frc_pentad_t), 24);
    }

    TEST_SECTION("frc_sync_t.helion: every Helion of a run of syncs");
    for (number = -2; number <= 40; number++) {
        frc_sync_t whole;
        uint8_t h;
        if (frc_sync_by_number(CAT, number, &whole) != FRC_OK) {
            continue;
        }
        CHECK_EQ_U64(whole.helion, 0);
        for (h = 0; h < FRC_SYNC_CYCLES; h++) {
            frc_cycle_t first_helion, helion;
            CHECK(frc_cycle_at(CAT, whole.start, FRC_SYNC_LEVEL, &first_helion) == FRC_OK);
            CHECK(frc_cycle_by_index(CAT, FRC_SYNC_LEVEL, first_helion.cycle + h, &helion) == FRC_OK);
            CHECK(frc_sync_at(CAT, helion.start, &sync) == FRC_OK);
            CHECK(sync.sync == number && sync.helion == h);
            CHECK(frc_sync_at(CAT, helion.end - 1, &sync) == FRC_OK);
            CHECK(sync.sync == number && sync.helion == h);
            CHECK(frc_pentad_at(CAT, helion.start, &pentad) == FRC_OK);
            CHECK_EQ_U64((unsigned)pentad.first + pentad.helion - 1u, h);
        }
    }

    TEST_SECTION("frc_pentad_t.first: the Helions of the Pentads before it");
    for (number = -40; number <= 40; number++) {
        uint8_t sum = 0;
        for (index = 0; index < 4; index++) {
            if (frc_pentad_by_number(CAT, number, index, &pentad) == FRC_OK) {
                CHECK_EQ_U64(pentad.first, sum);
                CHECK_EQ_U64(pentad.helion, 0);
            }
            sum = (uint8_t)(sum + (index == 3 ? 4 : 5));
        }
        CHECK_EQ_U64(sum, FRC_SYNC_CYCLES);
    }
    /* SPEC S7.5 for Saros 141: 0, 5, 10, 15 */
    for (index = 0; index < 4; index++) {
        CHECK(frc_pentad_by_number(CAT, 0, index, &pentad) == FRC_OK);
        CHECK_EQ_U64(pentad.first, 5u * index);
    }
    /* a Pentad cut at the catalogue's first instant keeps its place */
    CHECK(frc_pentad_at(CAT, frc_catalogue_first(CAT), &pentad) == FRC_OK);
    CHECK(pentad.first == 5u * pentad.index);
    CHECK(frc_pentad_at(CAT, frc_catalogue_last(CAT), &pentad) == FRC_OK);
    CHECK(pentad.first == 5u * pentad.index);
    /* the closing instant of a series that closes on a sync reads the last Helion */
}

int main(void)
{
    test_spec_vectors();
    test_samples_against_oracle();
    test_sync_properties();
    test_every_sync();
    test_pentad_properties();
    test_invalid_arguments();
    test_other_anchor();
    test_series_closing_on_a_sync();
    test_results_that_do_not_fit();
    test_drift_that_does_not_fit();
    test_nominal_start_that_does_not_fit();
    test_closing_phase_exactly();
    test_sun_places();
    return TEST_SUMMARY("sync");
}
