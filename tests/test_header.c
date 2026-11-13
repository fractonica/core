/*
 * test_header.c - the small print of fractonica.h, one check per sentence.
 *
 * The module tests hold the arithmetic against the oracle. This file holds
 * the DOCUMENTED EDGES of the public header: what each function answers for
 * an invalid argument, at the catalogue's closing instant, for the cases the
 * comments single out ("exactly 1/25 reads 1 per 25 Blinks"). When a comment in
 * fractonica.h changes, the matching line here changes with it, and the
 * other way round: neither may drift alone.
 *
 * Sections follow the header from top to bottom. Public API only.
 */
#include "frc_test.h"
#include "fractonica.h"

#include <math.h>

static const frc_catalogue_t *const CAT = &FRC_SAROS_141;

/* An instant well inside the catalogue (S12 vector A) and the two outside it. */
#define INSIDE FRC_TIME_FROM_S(INT64_C(1791460800))

/* ------------------------------------------------------------------------- */
/* ERRORS; S2 catalogue and position                                          */
/* ------------------------------------------------------------------------- */

static const int64_t GOOD_MS[3] FRC_PROGMEM = {0, 1000, 2000};
static const int64_t FLAT_MS[3] FRC_PROGMEM = {0, 1000, 1000};
static const int64_t FAR_MS[2] FRC_PROGMEM = {0, INT64_MAX / 1000 + 1};

static void test_catalogue(void)
{
    frc_catalogue_t cat, untouched;
    frc_position_t position;
    const frc_time_t before = frc_catalogue_first(CAT) - 1;

    TEST_SECTION("basics: time values, the flash tag, enums in a byte");
    /* "1 when `t` is FRC_TIME_NONE ...; 0 for a time" (0 is 1970, a time) */
    CHECK(frc_time_is_none(FRC_TIME_NONE) == 1);
    CHECK(frc_time_is_none(0) == 0 && frc_time_is_none(INT64_MAX) == 0 && frc_time_is_none(INT64_MIN + 1) == 0);
    CHECK(frc_time_is_none(frc_catalogue_instant(CAT, 70)) == 1);
    /* "Unix milliseconds / seconds as a frc_time_t": the macros, as functions */
    CHECK_EQ_I64(frc_time_from_ms(INT64_C(1791460800000)), FRC_TIME_FROM_MS(INT64_C(1791460800000)));
    CHECK_EQ_I64(frc_time_from_s(INT64_C(1791460800)), INSIDE);
    CHECK_EQ_I64(frc_time_from_ms(-1), -1000);
    CHECK_EQ_I64(frc_time_from_s(-1), -1000000);
    CHECK_EQ_I64(frc_time_from_ms(0), 0);
    /* "FRC_TIME_NONE when the value does not fit microseconds
     * (|ms| > INT64_MAX / 1000, |s| > INT64_MAX / 1000000)" */
    CHECK_EQ_I64(frc_time_from_ms(INT64_MAX / 1000), (INT64_MAX / 1000) * 1000);
    CHECK_EQ_I64(frc_time_from_ms(-(INT64_MAX / 1000)), -(INT64_MAX / 1000) * 1000);
    CHECK_EQ_I64(frc_time_from_ms(INT64_MAX / 1000 + 1), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_from_ms(-(INT64_MAX / 1000) - 1), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_from_ms(INT64_MAX), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_from_ms(INT64_MIN), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_from_s(INT64_MAX / 1000000), (INT64_MAX / 1000000) * 1000000);
    CHECK_EQ_I64(frc_time_from_s(INT64_MAX / 1000000 + 1), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_from_s(-(INT64_MAX / 1000000) - 1), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_from_s(INT64_MIN), FRC_TIME_NONE);
#ifndef FRC_NO_FLOAT
    /* "frc_frac_t as a double in [0, 1)"; "Exact: 32 bits fit a double" */
    CHECK(frc_frac_to_double(0) == 0.0 && frc_frac_to_double(FRC_FRAC_HALF) == 0.5);
    CHECK(frc_frac_to_double(FRC_FRAC_QUARTER) == 0.25);
    CHECK(frc_frac_to_double(0xFFFFFFFFu) == FRC_FRAC_TO_DOUBLE(0xFFFFFFFFu) && frc_frac_to_double(0xFFFFFFFFu) < 1.0);
    CHECK(frc_frac_to_double(1) == 1.0 / 4294967296.0);
#endif
    /* "on other targets it means plain reads (0)": the macro always has a value */
    CHECK(FRC_PROGMEM_IS_HARVARD == 0);
    /* "STRUCT FIELDS hold the same values in a uint8_t": one byte each, and
     * the value converts to the enum the functions take */
    {
        frc_threshold_t th;
        frc_cycle_t cycle;
        frc_span_part_t part;
        frc_rate_t rate;
        frc_date_t date;
        char text[16];
        CHECK(sizeof th.name == 1 && sizeof cycle.quarter == 1 && sizeof part.unit == 1 && sizeof rate.unit == 1);
        CHECK(sizeof date.quarter[0] == 1);
        CHECK(frc_next_threshold(CAT, INSIDE, 6, &th) == FRC_OK);
        CHECK(frc_format_quarter((frc_quarter_t)th.name, 0, text, sizeof text) > 0);
    }

    TEST_SECTION("frc_catalogue_init");
    cat.instants_ms = NULL; cat.count = 7; cat.series = 8; cat.anchor = 9;
    untouched = cat;
    /* "FRC_ERR_ARG if cat or instants_ms is NULL, count < 2, the table is not
     * strictly increasing, or an instant does not fit ... `cat` is then left untouched" */
    CHECK(frc_catalogue_init(NULL, GOOD_MS, 3, 0) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, NULL, 3, 0) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, GOOD_MS, 1, 0) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, FLAT_MS, 3, 0) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, FAR_MS, 2, 0) == FRC_ERR_ARG);
    CHECK(cat.instants_ms == untouched.instants_ms && cat.count == 7 && cat.series == 8 && cat.anchor == 9);
    CHECK(frc_catalogue_init(&cat, GOOD_MS, 3, 77) == FRC_OK);
    CHECK(cat.instants_ms == GOOD_MS && cat.count == 3 && cat.series == 77 && cat.anchor == 0);

    TEST_SECTION("catalogue edges");
    /* "FRC_TIME_NONE when ordinal >= count"; "FRC_TIME_NONE for an unusable catalogue" */
    CHECK_EQ_I64(frc_catalogue_instant(CAT, 70), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_catalogue_first(NULL), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_catalogue_last(NULL), FRC_TIME_NONE);
    CHECK(frc_catalogue_contains(NULL, 0) == 0);
    /* "1 when first <= t <= last" */
    CHECK(frc_catalogue_contains(CAT, frc_catalogue_first(CAT)) == 1);
    CHECK(frc_catalogue_contains(CAT, frc_catalogue_last(CAT)) == 1);
    CHECK(frc_catalogue_contains(CAT, before) == 0);
    CHECK(frc_catalogue_contains(CAT, frc_catalogue_last(CAT) + 1) == 0);
    /* S14: 70 eclipses, 1613-05-19 .. 2857-06-13, anchor 20 */
    CHECK(CAT->count == 70 && CAT->series == 141 && CAT->anchor == 20);

    TEST_SECTION("frc_position_at / frc_time_at_position");
    /* "FRC_ERR_RANGE outside [first, last]; FRC_ERR_ARG for a NULL argument or an unusable catalogue" */
    CHECK(frc_position_at(CAT, before, &position) == FRC_ERR_RANGE);
    CHECK(frc_position_at(CAT, INSIDE, NULL) == FRC_ERR_ARG);
    CHECK(frc_position_at(NULL, INSIDE, &position) == FRC_ERR_ARG);
    /* "t == last reads as one microsecond before it (ordinal count-2, num = den-1)" */
    CHECK(frc_position_at(CAT, frc_catalogue_last(CAT), &position) == FRC_OK);
    CHECK(position.ordinal == 68 && position.num == position.den - 1);
    /* "n/d == count-1 gives the closing instant"; "FRC_TIME_NONE when n/d > count-1, d == 0 ..." */
    CHECK_EQ_I64(frc_time_at_position(CAT, 69, 1), frc_catalogue_last(CAT));
    CHECK_EQ_I64(frc_time_at_position(CAT, 138, 2), frc_catalogue_last(CAT));
    CHECK_EQ_I64(frc_time_at_position(CAT, 139, 2), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_position(CAT, 1, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_position(NULL, 1, 1), FRC_TIME_NONE);
    /* "the first microsecond at or after it": a third of an interval is not whole */
    {
        frc_time_t third = frc_time_at_position(CAT, 1, 3);
        frc_time_t span = frc_catalogue_instant(CAT, 1) - frc_catalogue_instant(CAT, 0);
        CHECK(3 * (third - frc_catalogue_first(CAT)) >= span && 3 * (third - 1 - frc_catalogue_first(CAT)) < span);
    }
}

/* ------------------------------------------------------------------------- */
/* S3 hierarchy, S3.2 / S6 cycles                                             */
/* ------------------------------------------------------------------------- */

static void test_hierarchy_and_cycles(void)
{
    frc_cycle_t cycle;
    frc_threshold_t next;
    frc_time_t points[5];
    char text[32];
    const frc_time_t after = frc_catalogue_last(CAT) + 1;

    TEST_SECTION("hierarchy edges");
    /* "Exact for level <= 22; returns 0 when the value does not fit 64 bits" */
    CHECK(frc_prefix(22) != 0 && frc_prefix(23) == 0);
    CHECK_EQ_U64(frc_siblings(0), 0);
    /* "any uint8_t level is named (63 is π2 ...; 255 is π10)": 255 = 10 * 24 + 15 */
    CHECK(frc_format_level_name(63, text, sizeof text) > 0);
    CHECK_STR(text, "π2");
    CHECK(frc_format_level_name(255, text, sizeof text) > 0);
    CHECK_STR(text, "π10");
    CHECK(frc_format_level_word(23, text, sizeof text) > 0);
    CHECK_STR(text, "Omega");
    CHECK(frc_format_level_word(24, text, sizeof text) > 0);
    CHECK_STR(text, "Alpha1");
    /* "P(level) as an exact decimal string for any level 0..63" */
    CHECK(frc_format_prefix(64, text, sizeof text) == FRC_ERR_ARG);
    /* frc_level_kind */
    CHECK(frc_level_kind(3) == FRC_KIND_MAJOR && frc_level_kind(4) == FRC_KIND_UNIT &&
          frc_level_kind(5) == FRC_KIND_BETWEEN && frc_level_kind(12) == FRC_KIND_UNIT &&
          frc_level_kind(13) == FRC_KIND_SUB);
    /* "FRC_LEVEL_NONE when `unit` is not a frc_unit_t"; "-1 when the level is not a unit" */
    CHECK_EQ_U64(frc_unit_level(FRC_UNIT_COUNT), FRC_LEVEL_NONE);
    CHECK(frc_unit_of_level(5) == -1 && frc_unit_of_level(13) == -1 && frc_unit_of_level(6) == FRC_UNIT_HELION);
    CHECK_EQ_U64(frc_unit_blinks(FRC_UNIT_COUNT), 0);
    CHECK_EQ_U64(frc_unit_blinks(FRC_UNIT_ALPHA), UINT64_C(7319812500));
    CHECK_EQ_U64(frc_unit_parts(FRC_UNIT_BLINK), 0);
    CHECK_EQ_U64(frc_unit_parts(FRC_UNIT_COUNT), 0);
    text[0] = 'x';
    CHECK(frc_format_unit_symbol(FRC_UNIT_COUNT, text, sizeof text) == FRC_ERR_ARG && text[0] == '\0');
    CHECK(frc_format_unit_name(FRC_UNIT_COUNT, 0, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_unit_name(FRC_UNIT_HELION, 1, text, sizeof text) > 0);
    CHECK_STR(text, "Helions");

    TEST_SECTION("text buffers");
    /* "the number of bytes written (without the NUL) or a negative frc_status_t";
     * "FRC_ERR_BUFFER: text buffer too small (it still holds a valid prefix)" */
    CHECK_EQ_I64(frc_format_unit_name(FRC_UNIT_HELION, 0, text, sizeof text), 6);
    CHECK(frc_format_unit_name(FRC_UNIT_HELION, 0, text, 7) == 6);
    CHECK(frc_format_unit_name(FRC_UNIT_HELION, 0, text, 6) == FRC_ERR_BUFFER);
    CHECK_STR(text, "Helio");
    CHECK(frc_format_unit_name(FRC_UNIT_HELION, 0, text, 0) == FRC_ERR_BUFFER);
    /* "buf == NULL is a buffer of size 0 and answers FRC_ERR_BUFFER"; "FRC_ERR_ARG
     * from a text function always means another argument" (and it wins, as
     * argument errors do); "it then leaves \"\" in the buffer" */
    CHECK(frc_format_unit_name(FRC_UNIT_HELION, 0, NULL, 16) == FRC_ERR_BUFFER);
    CHECK(frc_format_unit_name(FRC_UNIT_HELION, 0, NULL, 0) == FRC_ERR_BUFFER);
    CHECK(frc_format_unit_name(FRC_UNIT_COUNT, 0, NULL, 16) == FRC_ERR_ARG);
    CHECK(frc_format_quarter(FRC_END, 0, NULL, 16) == FRC_ERR_BUFFER);
    CHECK(frc_to_ascii("\xCE\xB7", NULL, 16) == FRC_ERR_BUFFER);
    text[0] = 'x';
    CHECK(frc_format_unit_name(FRC_UNIT_COUNT, 0, text, sizeof text) == FRC_ERR_ARG && text[0] == '\0');
    /* a code point is written whole or not at all: "η" is two bytes */
    CHECK(frc_format_unit_symbol(FRC_UNIT_HELION, text, 2) == FRC_ERR_BUFFER && text[0] == '\0');
    CHECK(frc_format_unit_symbol(FRC_UNIT_HELION, text, 3) == 2);

    TEST_SECTION("frc_cycle_*");
    /* "FRC_ERR_RANGE outside the catalogue, FRC_ERR_ARG when level > FRC_CLOCK_MAX_LEVEL";
     * "an argument error is reported before a range error" */
    CHECK(frc_cycle_at(CAT, after, 6, &cycle) == FRC_ERR_RANGE);
    CHECK(frc_cycle_at(CAT, INSIDE, FRC_CLOCK_MAX_LEVEL + 1, &cycle) == FRC_ERR_ARG);
    CHECK(frc_cycle_at(CAT, after, FRC_CLOCK_MAX_LEVEL + 1, &cycle) == FRC_ERR_ARG);
    CHECK(frc_cycle_at(CAT, after, 6, NULL) == FRC_ERR_ARG);
    CHECK(frc_cycle_at(NULL, INSIDE, 6, &cycle) == FRC_ERR_ARG);
    CHECK(frc_cycle_at(CAT, INSIDE, FRC_CLOCK_MAX_LEVEL, &cycle) == FRC_OK);
    /* "FRC_ERR_RANGE when cycle >= frc_cycle_count(cat, level)" */
    CHECK(frc_cycle_by_index(CAT, 0, 17, &cycle) == FRC_OK);
    CHECK(frc_cycle_by_index(CAT, 0, 18, &cycle) == FRC_ERR_RANGE);
    CHECK(frc_cycle_by_index(CAT, 22, 0, &cycle) == FRC_ERR_ARG);
    /* "0 for an unusable catalogue, level > FRC_CLOCK_MAX_LEVEL" */
    CHECK_EQ_U64(frc_cycle_count(NULL, 0), 0);
    CHECK_EQ_U64(frc_cycle_count(CAT, 22), 0);
    /* "clipped: 1 when the catalogue ends inside this cycle"; "(clipped: closing
     * instant)"; "Points beyond the catalogue are FRC_TIME_NONE". The 18th Alpha
     * of Saros 141 has one Saros of its four: it is cut at its peak. */
    CHECK(frc_cycle_by_index(CAT, 0, 17, &cycle) == FRC_OK);
    CHECK(cycle.clipped == 1 && cycle.end == frc_catalogue_last(CAT) && cycle.local == 17 && cycle.siblings == 18);
    CHECK(frc_cycle_points(CAT, 0, 17, points) == FRC_OK);
    CHECK(points[0] == cycle.start && points[1] == frc_catalogue_last(CAT));
    CHECK(points[2] == FRC_TIME_NONE && points[3] == FRC_TIME_NONE && points[4] == FRC_TIME_NONE);
    CHECK(frc_cycle_points(CAT, 0, 18, points) == FRC_ERR_RANGE);
    /* "start <= t < end at every level, except at the catalogue's closing
     * instant, which belongs to the last cycle: there t == end"; BOUNDARY
     * INSTANTS: "a reading taken exactly there has t == end" */
    {
        const frc_time_t last = frc_catalogue_last(CAT);
        uint8_t level;
        for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
            frc_cycle_t child;
            CHECK(frc_cycle_at(CAT, INSIDE, level, &cycle) == FRC_OK);
            CHECK(cycle.start <= INSIDE && INSIDE < cycle.end);
            CHECK(frc_cycle_at(CAT, last - 1, level, &cycle) == FRC_OK);
            CHECK(cycle.start <= last - 1 && last - 1 < cycle.end);
            CHECK(frc_cycle_at(CAT, last, level, &cycle) == FRC_OK);
            CHECK(cycle.start <= last && cycle.end == last);
            /* "a walk down the tree ... must take the last child": the child
             * running at the closing instant ends there too. (Below level 18
             * a cycle is longer than a microsecond and it is the very last
             * cycle of its level.) */
            if (level < 18) {
                CHECK(cycle.cycle == frc_cycle_count(CAT, level) - 1);
                CHECK(frc_cycle_at(CAT, last, (uint8_t)(level + 1), &child) == FRC_OK);
                CHECK(child.end == last && child.cycle / frc_siblings((uint8_t)(level + 1)) == cycle.cycle);
            }
        }
    }

    TEST_SECTION("frc_next_threshold");
    /* "The returned time is > t ... any boundary asked for AT the closing
     * instant has time FRC_TIME_NONE (the name is still the running quarter's)" */
    CHECK(frc_next_threshold(CAT, frc_catalogue_last(CAT), 0, &next) == FRC_OK);
    CHECK(next.time == FRC_TIME_NONE && next.name == FRC_PEAK);
    CHECK(frc_next_threshold(CAT, frc_catalogue_last(CAT) - 1, 0, &next) == FRC_OK);
    CHECK(next.time == frc_catalogue_last(CAT) && next.name == FRC_PEAK);
    CHECK(frc_next_threshold(CAT, after, 0, &next) == FRC_ERR_RANGE);
    CHECK(frc_next_threshold(CAT, INSIDE, 0, NULL) == FRC_ERR_ARG);

    /* "lowercase when lower != 0. FRC_ERR_ARG for another value" */
    CHECK(frc_format_quarter(FRC_VALLEY, 1, text, sizeof text) > 0);
    CHECK_STR(text, "valley");
    CHECK(frc_format_quarter((frc_quarter_t)4, 0, text, sizeof text) == FRC_ERR_ARG);
}

/* ------------------------------------------------------------------------- */
/* S4 glyphs, S5 odometer, S9 drawing                                         */
/* ------------------------------------------------------------------------- */

static int sink_calls;
static void count_path(void *ctx, frc_path_op_t op, const int16_t *pts)
{
    (void)ctx; (void)op; (void)pts;
    sink_calls++;
}
static void count_segment(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    (void)ctx; (void)x0; (void)y0; (void)x1; (void)y1;
    sink_calls++;
}

static void test_glyphs_and_odometers(void)
{
    frc_glyph_t glyph;
    frc_odometer_t odometer;
    int16_t a = 1, b = 1, c = 1, d = 1;
    int steps_1, steps_32, steps_99;

    TEST_SECTION("glyphs");
    /* "0 if strokes invalid" */
    CHECK(frc_glyph_extreme(0) == 0 && frc_glyph_extreme(6) == 0 && frc_glyph_extreme(5) == 121);
    CHECK(frc_glyph_states(0) == 0 && frc_glyph_states(5) == 484);
    CHECK(frc_glyph_index_descending(3, 0) == 0 && frc_glyph_index_of_phase(FRC_FRAC_HALF, 9) == 0);
    /* "Decode state `index` (taken mod 4M)" */
    CHECK(frc_glyph_from_index(16 + 5, 2, &glyph) == FRC_OK && glyph.index == 5);
    CHECK(frc_glyph_from_index(0, 0, &glyph) == FRC_ERR_ARG);
    CHECK(frc_glyph_from_index(0, 2, NULL) == FRC_ERR_ARG);
    /* "unused are 0" */
    CHECK(frc_glyph_from_index(13, 2, &glyph) == FRC_OK);
    CHECK(glyph.trit[2] == 0 && glyph.trit[3] == 0 && glyph.trit[4] == 0);
    /* "floor(phase * 4M)"; "half index ... (phase >= FRC_FRAC_HALF)" */
    CHECK(frc_glyph_index_of_phase(FRC_FRAC_QUARTER, 2) == 4 && frc_glyph_index_of_phase(FRC_FRAC_HALF, 2) == 8);
    CHECK(frc_glyph_index_of_phase(FRC_FRAC_QUARTER - 1, 2) == 3);
    CHECK(frc_glyph_at(CAT, INSIDE, 6, 0, &glyph) == FRC_ERR_ARG);
    CHECK(frc_glyph_at(CAT, frc_catalogue_last(CAT) + 1, 6, 0, &glyph) == FRC_ERR_ARG);
    CHECK(frc_glyph_at(CAT, frc_catalogue_last(CAT) + 1, 6, 2, &glyph) == FRC_ERR_RANGE);

    TEST_SECTION("odometers");
    /* "FRC_ERR_ARG for strokes outside 1..FRC_MAX_STROKES, digits outside
     * 1..FRC_MAX_DIGITS or level > FRC_CLOCK_MAX_LEVEL (checked before the instant)" */
    CHECK(frc_odometer_at(CAT, INSIDE, 6, 0, 2, &odometer) == FRC_ERR_ARG);
    CHECK(frc_odometer_at(CAT, INSIDE, 6, 6, 2, &odometer) == FRC_ERR_ARG);
    CHECK(frc_odometer_at(CAT, INSIDE, 6, 2, 0, &odometer) == FRC_ERR_ARG);
    CHECK(frc_odometer_at(CAT, INSIDE, 6, 2, 5, &odometer) == FRC_ERR_ARG);
    CHECK(frc_odometer_at(CAT, INSIDE, 22, 2, 2, &odometer) == FRC_ERR_ARG);
    CHECK(frc_odometer_at(CAT, frc_catalogue_last(CAT) + 1, 6, 9, 2, &odometer) == FRC_ERR_ARG);
    CHECK(frc_odometer_at(CAT, frc_catalogue_last(CAT) + 1, 6, 2, 2, &odometer) == FRC_ERR_RANGE);
    CHECK(frc_odometer_at(CAT, INSIDE, 6, 2, 2, NULL) == FRC_ERR_ARG);
    /* "next_change is FRC_TIME_NONE only there" (the closing instant) */
    CHECK(frc_odometer_at(CAT, frc_catalogue_last(CAT), 6, 5, 4, &odometer) == FRC_OK);
    CHECK_EQ_I64(odometer.next_change, FRC_TIME_NONE);
    CHECK(frc_odometer_at(CAT, frc_catalogue_last(CAT) - 1, 6, 5, 4, &odometer) == FRC_OK);
    CHECK_EQ_I64(odometer.next_change, frc_catalogue_last(CAT));
    /* "floor(phase * q^k) split into k digits. next_change is FRC_TIME_NONE.
     * 2 digits of 2 strokes = the 256-state pair" */
    CHECK(frc_odometer_of_phase(FRC_FRAC_HALF + FRC_FRAC_QUARTER / 16, 2, 2, &odometer) == FRC_OK);
    CHECK(odometer.count == 2 && odometer.strokes == 2 && odometer.states == 16);
    CHECK(odometer.digit[0] == 8 && odometer.digit[1] == 4 && odometer.digit[2] == 0 && odometer.digit[3] == 0);
    CHECK_EQ_I64(odometer.next_change, FRC_TIME_NONE);
    CHECK(frc_odometer_of_phase(0xFFFFFFFFu, 5, 4, &odometer) == FRC_OK);
    CHECK(odometer.digit[0] == 483 && odometer.digit[1] == 483 && odometer.digit[2] == 483);
    CHECK(frc_odometer_of_phase(0, 0, 1, &odometer) == FRC_ERR_ARG);

    TEST_SECTION("drawing with bad input");
    /* "Any of the four pointers may be NULL; invalid strokes write 0 to all four" */
    frc_glyph_bounds(2, NULL, NULL, NULL, NULL);
    frc_glyph_bounds(2, NULL, &b, NULL, &d);
    CHECK(b == -220 && d == 615);
    frc_glyph_bounds(0, &a, &b, &c, &d);
    CHECK(a == 0 && b == 0 && c == 0 && d == 0);
    /* "Draws nothing for a NULL glyph or sink, strokes outside 1..FRC_MAX_STROKES
     * or a trit outside -1..1 (frc_glyph_segments likewise)" */
    CHECK(frc_glyph_from_index(5, 2, &glyph) == FRC_OK);
    sink_calls = 0;
    frc_glyph_path(NULL, count_path, NULL);
    frc_glyph_path(&glyph, NULL, NULL);
    frc_glyph_segments(NULL, 4, count_segment, NULL);
    frc_glyph_segments(&glyph, 4, NULL, NULL);
    glyph.strokes = 6;
    frc_glyph_path(&glyph, count_path, NULL);
    frc_glyph_segments(&glyph, 4, count_segment, NULL);
    glyph.strokes = 2;
    glyph.trit[1] = 2;
    frc_glyph_path(&glyph, count_path, NULL);
    frc_glyph_segments(&glyph, 4, count_segment, NULL);
    CHECK_EQ_I64(sink_calls, 0);
    /* "each cubic becomes `curve_steps` segments (clamped to 1..32). The dot is NOT emitted" */
    CHECK(frc_glyph_from_index(5, 2, &glyph) == FRC_OK); /* value 3 falling: trits (0, +1), dotted */
    CHECK(glyph.dotted && glyph.trit[0] == 0 && glyph.trit[1] == 1);
    sink_calls = 0;
    frc_glyph_segments(&glyph, 1, count_segment, NULL);
    steps_1 = sink_calls; /* 2 stem + 1 bar + 2 cubics of 1 segment */
    CHECK_EQ_I64(steps_1, 5);
    sink_calls = 0;
    frc_glyph_segments(&glyph, 0, count_segment, NULL);
    CHECK_EQ_I64(sink_calls, steps_1);
    sink_calls = 0;
    frc_glyph_segments(&glyph, 32, count_segment, NULL);
    steps_32 = sink_calls;
    CHECK_EQ_I64(steps_32, 3 + 2 * 32);
    sink_calls = 0;
    frc_glyph_segments(&glyph, 99, count_segment, NULL);
    steps_99 = sink_calls;
    CHECK_EQ_I64(steps_99, steps_32);
}

/* ------------------------------------------------------------------------- */
/* S7 sync and Pentads                                                        */
/* ------------------------------------------------------------------------- */

static const int64_t SHORT_MS[6] FRC_PROGMEM = {
    INT64_C(0), INT64_C(568972877000), INT64_C(1137945754000), INT64_C(1706918631000),
    INT64_C(2275891508000), INT64_C(2844864385000)};

static void test_sync_and_pentads(void)
{
    frc_catalogue_t odd;
    frc_sync_t sync;
    frc_pentad_t pentad;
    frc_odometer_t odometer;
    char text[48];

    TEST_SECTION("sync edges");
    /* "FRC_ERR_RANGE outside the catalogue AND inside its first / last partial sync" */
    CHECK(frc_sync_at(CAT, frc_catalogue_first(CAT), &sync) == FRC_ERR_RANGE);
    CHECK(frc_sync_at(CAT, frc_catalogue_last(CAT), &sync) == FRC_ERR_RANGE);
    CHECK(frc_sync_at(CAT, frc_catalogue_first(CAT) - 1, &sync) == FRC_ERR_RANGE);
    CHECK(frc_sync_at(CAT, INSIDE, NULL) == FRC_ERR_ARG);
    /* "Defined in the partial syncs too" */
    CHECK(frc_pentad_at(CAT, frc_catalogue_first(CAT), &pentad) == FRC_OK);
    CHECK(pentad.start == frc_catalogue_first(CAT)); /* "cut at the catalogue's edges" */
    CHECK(frc_pentad_at(CAT, frc_catalogue_last(CAT), &pentad) == FRC_OK);
    CHECK(pentad.end == frc_catalogue_last(CAT));
    /* "Sync number `number`: phase and quarter are 0 and next is {0, FRC_TIME_NONE}" */
    CHECK(frc_sync_by_number(CAT, 0, &sync) == FRC_OK);
    CHECK(sync.sync == 0 && sync.phase == 0 && sync.quarter == 0 && sync.next.name == 0);
    CHECK_EQ_I64(sync.next.time, FRC_TIME_NONE);
    CHECK(sync.start == frc_catalogue_instant(CAT, 20) && sync.drift_us == 0);
    /* "FRC_ERR_RANGE if not whole in the catalogue" */
    CHECK(frc_sync_by_number(CAT, INT32_MAX, &sync) == FRC_ERR_RANGE);
    CHECK(frc_sync_by_number(CAT, INT32_MIN, &sync) == FRC_ERR_RANGE);
    /* "helion is 0"; "FRC_ERR_ARG for index > 3" */
    CHECK(frc_pentad_by_number(CAT, 0, 3, &pentad) == FRC_OK && pentad.helion == 0 && pentad.helions == 4);
    CHECK(frc_pentad_by_number(CAT, 0, 4, &pentad) == FRC_ERR_ARG);
    CHECK(frc_pentad_by_number(CAT, INT32_MAX, 0, &pentad) == FRC_ERR_RANGE);
    /* "start <= t < end, and t == end only at the catalogue's closing instant" */
    CHECK(frc_pentad_at(CAT, INSIDE, &pentad) == FRC_OK && pentad.start <= INSIDE && INSIDE < pentad.end);
    CHECK(frc_pentad_at(CAT, frc_catalogue_last(CAT), &pentad) == FRC_OK);
    CHECK(pentad.start < frc_catalogue_last(CAT) && pentad.end == frc_catalogue_last(CAT));
    /* "FRC_ERR_RANGE when wholly outside the catalogue: it ends at or before
     * the first instant, or starts at or after the closing instant (no Pentad
     * of zero length is returned)". Five eclipses anchored at the first are
     * 4 Saros = 125125 Helions = 6585 syncs and 10 Helions: Pentad II of sync
     * 6585 ends exactly at the closing instant and Pentad III would start there. */
    {
        static const int64_t FIVE_MS[5] FRC_PROGMEM = {INT64_C(100000000000), INT64_C(668000000000),
                                                       INT64_C(1236000000000), INT64_C(1804000000000),
                                                       INT64_C(2372000000000)};
        frc_catalogue_t five;
        frc_pentad_t untouched;
        CHECK(frc_catalogue_init(&five, FIVE_MS, 5, 0) == FRC_OK && five.anchor == 0);
        CHECK(frc_pentad_by_number(&five, 6585, 1, &pentad) == FRC_OK);
        CHECK(pentad.start < pentad.end && pentad.end == frc_catalogue_last(&five));
        memset(&pentad, 0x5A, sizeof pentad);
        untouched = pentad;
        CHECK(frc_pentad_by_number(&five, 6585, 2, &pentad) == FRC_ERR_RANGE);
        CHECK(frc_pentad_by_number(&five, 6585, 3, &pentad) == FRC_ERR_RANGE);
        CHECK(memcmp(&pentad, &untouched, sizeof pentad) == 0);
        /* the mirror image at the first instant: sync 0, Pentad I starts there;
         * the Hypopentad of sync -1 would END there */
        CHECK(frc_pentad_by_number(&five, 0, 0, &pentad) == FRC_OK && pentad.start == frc_catalogue_first(&five));
        CHECK(frc_pentad_by_number(&five, -1, 3, &pentad) == FRC_ERR_RANGE);
    }

    /* "FRC_ERR_ARG when cat->anchor is not an eclipse that starts a Helion (it
     * must be a multiple of 4 and < count)" */
    CHECK(frc_catalogue_init(&odd, SHORT_MS, 6, 0) == FRC_OK);
    CHECK(frc_sync_at(&odd, FRC_TIME_FROM_MS(INT64_C(1000000000000)), &sync) == FRC_OK);
    odd.anchor = 2;
    CHECK(frc_sync_at(&odd, FRC_TIME_FROM_MS(INT64_C(1000000000000)), &sync) == FRC_ERR_ARG);
    CHECK(frc_sync_by_number(&odd, 0, &sync) == FRC_ERR_ARG);
    CHECK(frc_sync_odometer(&odd, FRC_TIME_FROM_MS(INT64_C(1000000000000)), 2, 3, &odometer) == FRC_ERR_ARG);
    CHECK(frc_pentad_at(&odd, FRC_TIME_FROM_MS(INT64_C(1000000000000)), &pentad) == FRC_ERR_ARG);
    CHECK(frc_pentad_by_number(&odd, 0, 0, &pentad) == FRC_ERR_ARG);
    odd.anchor = 8;
    CHECK(frc_sync_at(&odd, FRC_TIME_FROM_MS(INT64_C(1000000000000)), &sync) == FRC_ERR_ARG);
    /* "Only a series that closes exactly on a sync has a reading at its closing
     * instant". This one does not (one Saros is 31281.25 Helions, not a
     * multiple of 19): its last sync is partial. tests/test_sync.c builds a
     * series that does, and reads it there. */
    odd.anchor = 4;
    CHECK(frc_sync_at(&odd, frc_catalogue_last(&odd), &sync) == FRC_ERR_RANGE);

    TEST_SECTION("frc_format_pentad");
    CHECK(frc_pentad_by_number(CAT, 0, 1, &pentad) == FRC_OK);
    /* "with_helion != 0 needs pentad->helion in 1..5 (frc_pentad_by_number leaves it 0)" */
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_FULL, 1, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_FULL, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Pentad II");
    pentad.helion = 3;
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_FULL, 1, text, sizeof text) > 0);
    CHECK_STR(text, "Pentad II, 3rd Helion");
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_SHORT, 1, text, sizeof text) > 0);
    CHECK_STR(text, "⬠II·3");
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_ASCII, 1, text, sizeof text) > 0);
    CHECK_STR(text, "P2.3");
    /* FRC_PENTAD_NUMERAL: "II", and "" for the Hypopentad; with_helion adds nothing */
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_NUMERAL, 1, text, sizeof text) == 2);
    CHECK_STR(text, "II");
    pentad.index = 3;
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_NUMERAL, 0, text, sizeof text) == 0);
    CHECK_STR(text, "");
    pentad.index = 1;
    CHECK(frc_format_pentad(&pentad, (frc_pentad_form_t)4, 0, text, sizeof text) == FRC_ERR_ARG);
    pentad.index = 4;
    CHECK(frc_format_pentad(&pentad, FRC_PENTAD_FULL, 0, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_pentad(NULL, FRC_PENTAD_FULL, 0, text, sizeof text) == FRC_ERR_ARG);
}

/* ------------------------------------------------------------------------- */
/* S15 notation                                                               */
/* ------------------------------------------------------------------------- */

static frc_date_t a_date(void)
{
    /* S15.6 vector A */
    frc_date_t d;
    d.alpha = 6; d.beta = 4; d.gamma = 9; d.delta = 3; d.epsilon = 5;
    d.eta = 34; d.iota = 29; d.lambda = 21; d.nu = 3;
    d.quarter[0] = FRC_VALLEY; d.quarter[1] = FRC_VALLEY; d.quarter[2] = FRC_VALLEY; d.quarter[3] = FRC_END;
    return d;
}

static void test_dates(void)
{
    frc_date_t date = a_date(), parsed;
    frc_unit_t finest = FRC_UNIT_COUNT;
    frc_time_t t;
    char text[200];

    TEST_SECTION("frc_date_to_time");
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_OK);
    CHECK(t <= INSIDE && INSIDE - t < 312000);
    /* "FRC_ERR_ARG for a field outside its fixed range (alpha 0 included)" */
    date.alpha = 0;
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_ERR_ARG);
    date = a_date(); date.eta = 0;
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_ERR_ARG);
    date = a_date(); date.eta = 78;
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_ERR_ARG);
    date = a_date(); date.iota = 36;
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_ERR_ARG);
    date = a_date(); date.nu = 25;
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_ERR_ARG);
    /* "FRC_ERR_RANGE when the date is well formed but not in the catalogue (an
     * Alpha it does not have, or a Blink after its closing instant)" */
    date = a_date(); date.alpha = 19;
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_ERR_RANGE);
    date = a_date(); date.alpha = 18; date.beta = 5; /* the 18th Alpha is cut at its peak */
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_ERR_RANGE);
    /* "the quarters are ignored" */
    date = a_date(); date.quarter[0] = 99;
    CHECK(frc_date_to_time(CAT, &date, &t) == FRC_OK);

    TEST_SECTION("frc_format_date");
    date = a_date();
    /* "coarser values are treated as FRC_UNIT_DELTA" */
    CHECK(frc_format_date(&date, FRC_DATE_SHORT_LETTERS, FRC_UNIT_ALPHA, text, sizeof text) > 0);
    CHECK_STR(text, "δ#3");
    CHECK(frc_format_date(&date, FRC_DATE_SHORT, FRC_UNIT_DELTA, text, sizeof text) > 0);
    CHECK_STR(text, "Delta Trifracton End");
    /* "FRC_ERR_ARG for an unknown form or finest, or a quarter[] above 3
     * (checked in every form, the letter forms included)" */
    CHECK(frc_format_date(&date, (frc_date_form_t)4, FRC_UNIT_BLINK, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_date(&date, FRC_DATE_FULL, FRC_UNIT_COUNT, text, sizeof text) == FRC_ERR_ARG);
    date.quarter[1] = 4;
    CHECK(frc_format_date(&date, FRC_DATE_LETTERS, FRC_UNIT_BLINK, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_date(NULL, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text) == FRC_ERR_ARG);
    /* "Longest output is under 160 bytes": the longest ordinals and counts */
    date = a_date();
    date.alpha = 14; date.beta = 4; date.gamma = 4; date.delta = 4; /* Tetradecafracton, Tetrafracton */
    date.quarter[0] = date.quarter[1] = date.quarter[2] = date.quarter[3] = FRC_VALLEY;
    date.epsilon = 5; date.eta = 77; date.iota = 35; date.lambda = 64; date.nu = 24;
    CHECK(frc_format_date(&date, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text) < 160);
    CHECK(frc_format_date(&date, FRC_DATE_FULL, FRC_UNIT_BLINK, text, 160) > 0);

    TEST_SECTION("frc_parse_date");
    date = a_date();
    /* the header's own example */
    CHECK(frc_parse_date("a#6 b#4 g#9 d#3 e#5 34h 29i", NULL, &parsed, &finest) == FRC_OK);
    CHECK(parsed.alpha == 6 && parsed.beta == 4 && parsed.gamma == 9 && parsed.delta == 3 && parsed.epsilon == 5 &&
          parsed.eta == 34 && parsed.iota == 29 && finest == FRC_UNIT_PREON);
    /* "parts missing at the fine end are the start: Helion 1, counts 0"; "Quarters of the result are 0" */
    CHECK(frc_parse_date("α#6 β#4 γ#9 δ#3", NULL, &parsed, &finest) == FRC_OK);
    CHECK(parsed.epsilon == 1 && parsed.eta == 1 && parsed.iota == 0 && parsed.lambda == 0 && parsed.nu == 0);
    CHECK(finest == FRC_UNIT_DELTA && parsed.quarter[0] == 0 && parsed.quarter[3] == 0);
    /* "Parts missing at the coarse end (a short form) are taken from `context`" */
    CHECK(frc_parse_date("34η 29ι", &date, &parsed, &finest) == FRC_OK);
    CHECK(parsed.alpha == 6 && parsed.delta == 3 && parsed.epsilon == 5 && parsed.eta == 34 && parsed.iota == 29 &&
          parsed.lambda == 0 && finest == FRC_UNIT_PREON);
    /* "(pass NULL to require a full date)": "a short form with context == NULL" is FRC_ERR_PARSE */
    CHECK(frc_parse_date("34η 29ι", NULL, &parsed, NULL) == FRC_ERR_PARSE);
    /* "A leading Pentad (Pentad II ·, ⬠II, P2) and quarter words are skipped" */
    CHECK(frc_parse_date("Pentad II · Delta Trifracton End ε#5 34η 29ι", &date, &parsed, NULL) == FRC_OK);
    CHECK(parsed.delta == 3 && parsed.eta == 34);
    CHECK(frc_parse_date("⬠II δ#3 ε#5 34η", &date, &parsed, NULL) == FRC_OK);
    CHECK(frc_parse_date("P2 d#3 e#5 34h", &date, &parsed, NULL) == FRC_OK);
    CHECK(frc_parse_date("PII.3 d#3 e#5 34h", &date, &parsed, NULL) == FRC_OK); /* frc_to_ascii of "⬠II·3" */
    CHECK(frc_parse_date("H d#3 e#5 34h", &date, &parsed, NULL) == FRC_OK);
    /* "quarter words are read and dropped, not checked against the counts" */
    CHECK(frc_parse_date("Delta Trifracton Peak ε#5 34η 29ι", &date, &parsed, NULL) == FRC_OK);
    /* "case does not matter" */
    CHECK(frc_parse_date("ALPHA HEXAFRACTON, beta #4, Γ#9, D#3", NULL, &parsed, NULL) == FRC_OK);
    CHECK(parsed.alpha == 6 && parsed.beta == 4 && parsed.gamma == 9 && parsed.delta == 3);
    /* "an ordinal needs its '#' (a#6, never a6)" */
    CHECK(frc_parse_date("a6 b4 g9 d3", NULL, &parsed, NULL) == FRC_ERR_PARSE);
    CHECK(frc_parse_date("6.4.9.3", NULL, &parsed, NULL) == FRC_ERR_PARSE);
    /* "a gap, a duplicate or a part out of order is FRC_ERR_PARSE" */
    CHECK(frc_parse_date("δ#3 34η", &date, &parsed, NULL) == FRC_ERR_PARSE);
    CHECK(frc_parse_date("δ#3 δ#3", &date, &parsed, NULL) == FRC_ERR_PARSE);
    CHECK(frc_parse_date("29ι 34η", &date, &parsed, NULL) == FRC_ERR_PARSE);
    CHECK(frc_parse_date("", &date, &parsed, NULL) == FRC_ERR_PARSE);
    CHECK(frc_parse_date("Pentad II", &date, &parsed, NULL) == FRC_ERR_PARSE);
    CHECK(frc_parse_date("2026-10-08", &date, &parsed, NULL) == FRC_ERR_PARSE);
    CHECK(frc_parse_date("34 Helions", &date, &parsed, NULL) == FRC_ERR_PARSE);
    /* "FRC_ERR_ARG on a field out of range in text that is a date" ... */
    CHECK(frc_parse_date("δ#3 ε#6", &date, &parsed, NULL) == FRC_ERR_ARG);
    CHECK(frc_parse_date("78η", &date, &parsed, NULL) == FRC_ERR_ARG);
    CHECK(frc_parse_date("0η", &date, &parsed, NULL) == FRC_ERR_ARG);
    CHECK(frc_parse_date("99999999999999999999η", &date, &parsed, NULL) == FRC_ERR_ARG);
    /* ... "FRC_ERR_PARSE ... also when one of its numbers is out of range" in text that is not a date */
    CHECK(frc_parse_date("78η and more", &date, &parsed, NULL) == FRC_ERR_PARSE);
    /* ... "or in a context field the result takes over" */
    date.delta = 6;
    CHECK(frc_parse_date("ε#5 34η", &date, &parsed, NULL) == FRC_ERR_ARG);
    CHECK(frc_parse_date("δ#3 ε#5 34η", &date, &parsed, NULL) == FRC_OK); /* delta is not taken over */
    /* "*out and *finest are written only on success" */
    parsed.alpha = 4242;
    finest = FRC_UNIT_COUNT;
    CHECK(frc_parse_date("δ#3 ε#6", &date, &parsed, &finest) == FRC_ERR_ARG);
    CHECK(frc_parse_date("nonsense", &date, &parsed, &finest) == FRC_ERR_PARSE);
    CHECK(parsed.alpha == 4242 && finest == FRC_UNIT_COUNT);
    CHECK(frc_parse_date(NULL, &date, &parsed, NULL) == FRC_ERR_ARG);
    CHECK(frc_parse_date("δ#3", &date, NULL, NULL) == FRC_ERR_ARG);
}

static void test_names_spans_rates(void)
{
    frc_span_t span;
    frc_span_part_t parts[FRC_UNIT_COUNT];
    frc_rate_t rate;
    uint64_t num = 1, den = 1;
    char text[96];

    TEST_SECTION("frc_format_ordinal / frc_format_cycle_name");
    /* "Monofracton (1) .. Icosafracton (20); beyond that #25" */
    CHECK(frc_format_ordinal(20, text, sizeof text) > 0);
    CHECK_STR(text, "Icosafracton");
    CHECK(frc_format_ordinal(25, text, sizeof text) > 0);
    CHECK_STR(text, "#25");
    CHECK(frc_format_ordinal(0, text, sizeof text) > 0);
    CHECK_STR(text, "#0");
    /* the examples of the comment */
    CHECK(frc_format_cycle_name(3, 2, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Delta Trifracton");
    CHECK(frc_format_cycle_name(4, 1, 0, text, sizeof text) > 0);
    CHECK_STR(text, "ε#2");
    CHECK(frc_format_cycle_name(8, (77 + 4) * 36 + 9, 0, text, sizeof text) > 0);
    CHECK_STR(text, "ε#2 5η 9ι");
    /* "With short_form != 0: δ#3, ε#2, or the finest placing count alone (9ι)" */
    CHECK(frc_format_cycle_name(3, 2, 1, text, sizeof text) > 0);
    CHECK_STR(text, "δ#3");
    CHECK(frc_format_cycle_name(4, 1, 1, text, sizeof text) > 0);
    CHECK_STR(text, "ε#2");
    CHECK(frc_format_cycle_name(8, (77 + 4) * 36 + 9, 1, text, sizeof text) > 0);
    CHECK_STR(text, "9ι");
    /* "a start within half a hundredth of the next Blink reads as that Blink
     * (... 25ν for 24.9996 ...)": level 21 has 31,531,500 cycles in a Blink */
    CHECK(frc_format_cycle_name(21, UINT64_C(31531500) * 25 - 1, 1, text, sizeof text) > 0);
    CHECK_STR(text, "25ν");
    CHECK(frc_format_cycle_name(21, UINT64_C(31531500) * 24, 1, text, sizeof text) > 0);
    CHECK_STR(text, "24ν");
    /* "FRC_ERR_ARG for level > FRC_CLOCK_MAX_LEVEL; FRC_ERR_OVERFLOW for (level 0, cycle UINT64_MAX)" */
    CHECK(frc_format_cycle_name(22, 0, 0, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_cycle_name(0, UINT64_MAX, 0, text, sizeof text) == FRC_ERR_OVERFLOW);
    CHECK(frc_format_cycle_name(0, UINT64_MAX - 1, 1, text, sizeof text) > 0);
    CHECK_STR(text, "α#18446744073709551615");

    TEST_SECTION("spans");
    /* "FRC_ERR_RANGE" outside, exact across eclipses, signed */
    CHECK(frc_span_between(CAT, INSIDE, frc_catalogue_last(CAT) + 1, &span) == FRC_ERR_RANGE);
    CHECK(frc_span_between(CAT, INSIDE, INSIDE, NULL) == FRC_ERR_ARG);
    CHECK(frc_span_between(CAT, INSIDE, INSIDE, &span) == FRC_OK);
    CHECK(span.negative == 0 && span.blinks == 0 && span.frac == 0);
    /* one whole Saros is P(12) / 4 Blinks, whatever its length in seconds */
    CHECK(frc_span_between(CAT, frc_catalogue_instant(CAT, 3), frc_catalogue_instant(CAT, 4), &span) == FRC_OK);
    CHECK(span.negative == 0 && span.blinks == UINT64_C(1829953125) && span.frac == 0);
    CHECK(frc_span_between(CAT, frc_catalogue_last(CAT), frc_catalogue_first(CAT), &span) == FRC_OK);
    CHECK(span.negative == 1 && span.blinks == 69 * UINT64_C(1829953125) && span.frac == 0);
    /* "`out` is written only on FRC_OK" */
    span.negative = 0x5A; span.blinks = 0x5A5A; span.frac = 0x5A5A5A5Au;
    CHECK(frc_span_between(CAT, INSIDE, frc_catalogue_last(CAT) + 1, &span) == FRC_ERR_RANGE);
    CHECK(frc_span_between(NULL, INSIDE, INSIDE, &span) == FRC_ERR_ARG);
    CHECK(span.negative == 0x5A && span.blinks == 0x5A5A && span.frac == 0x5A5A5A5Au);
    /* THE TIME GRAIN: "frc_span_between(cycle.start, cycle.end) is the cycle's
     * length exactly (\"1ι\", never \"64λ 24ν\"), and likewise from threshold to
     * threshold or between frc_date_to_time of two dates" */
    {
        frc_cycle_t preon;
        frc_threshold_t first, second;
        frc_date_t date;
        frc_time_t t1, t2;
        CHECK(frc_cycle_at(CAT, INSIDE, 8, &preon) == FRC_OK);
        CHECK(frc_span_between(CAT, preon.start, preon.end, &span) == FRC_OK);
        CHECK(span.negative == 0 && span.blinks == 1625 && span.frac == 0);
        CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
        CHECK_STR(text, "1ι");
        CHECK(frc_span_between(CAT, preon.end, preon.start, &span) == FRC_OK);
        CHECK(span.negative == 1 && span.blinks == 1625 && span.frac == 0);
        /* a quarter Helion is 9 Preons */
        CHECK(frc_next_threshold(CAT, INSIDE, 6, &first) == FRC_OK);
        CHECK(frc_next_threshold(CAT, first.time, 6, &second) == FRC_OK);
        CHECK(frc_span_between(CAT, first.time, second.time, &span) == FRC_OK);
        CHECK(span.blinks == 9 * 1625 && span.frac == 0);
        /* two dates one Helion apart */
        CHECK(frc_date_at(CAT, INSIDE, &date) == FRC_OK && date.eta < 77);
        CHECK(frc_date_to_time(CAT, &date, &t1) == FRC_OK);
        date.eta++;
        CHECK(frc_date_to_time(CAT, &date, &t2) == FRC_OK);
        CHECK(frc_span_between(CAT, t1, t2, &span) == FRC_OK);
        CHECK(span.blinks == 58500 && span.frac == 0);
        /* "A span one microsecond long ... is left as measured" */
        CHECK(frc_span_between(CAT, preon.start, preon.start + 1, &span) == FRC_OK);
        CHECK(span.blinks == 0 && span.frac != 0);
        /* "no span between two different instants is brought to zero": the
         * microsecond that ends at an eclipse */
        CHECK(frc_span_between(CAT, frc_catalogue_instant(CAT, 30), frc_catalogue_instant(CAT, 30) - 1, &span) == FRC_OK);
        CHECK(span.negative == 1 && span.blinks == 0 && span.frac != 0);
        /* "... or a whole microsecond or more off the grid": 2 us past the Preon,
         * 2 us short of it */
        CHECK(frc_span_between(CAT, preon.start, preon.end + 2, &span) == FRC_OK);
        CHECK(span.blinks == 1625 && span.frac != 0);
        CHECK(frc_span_between(CAT, preon.start + 2, preon.end, &span) == FRC_OK);
        CHECK(span.blinks == 1624 && span.frac != 0);
    }
    /* "the text of the result is the text of the exact span (an exact 1.05
     * Blinks reads \"1.1ν\")": on a table whose Blink is exactly 20 ms */
    {
        static const int64_t TIES_MS[3] FRC_PROGMEM = {0, INT64_C(36599062500), INT64_C(73198125000)};
        frc_catalogue_t ties;
        CHECK(frc_catalogue_init(&ties, TIES_MS, 3, 0) == FRC_OK);
        CHECK(frc_span_between(&ties, 0, 21000, &span) == FRC_OK);
        CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
        CHECK_STR(text, "1.1ν");
    }
    /* "FRC_ERR_OVERFLOW when a Saros interval that holds or ends at t1 or t2
     * is 2^63 microseconds or longer (a hand-built table that spans most of
     * the int64 range)" */
    {
        static const int64_t WHOLE_RANGE_MS[2] FRC_PROGMEM = {-(INT64_MAX / 1000), INT64_MAX / 1000};
        frc_catalogue_t wide;
        CHECK(frc_catalogue_init(&wide, WHOLE_RANGE_MS, 2, 0) == FRC_OK);
        span.negative = 0x5A; span.blinks = 0x5A5A; span.frac = 0x5A5A5A5Au;
        CHECK(frc_span_between(&wide, 0, 1000000, &span) == FRC_ERR_OVERFLOW);
        CHECK(frc_span_between(&wide, frc_catalogue_first(&wide), frc_catalogue_last(&wide), &span) == FRC_ERR_OVERFLOW);
        CHECK(span.negative == 0x5A && span.blinks == 0x5A5A && span.frac == 0x5A5A5A5Au);
    }
    /* "Nominal length of one cycle of `level`, 0..62: ζ = 11η, θ = 4ι, ξ = 1/7 ν";
     * "Exact for level <= 12 (a whole number of Blinks)" */
    CHECK(frc_span_of_level(5, &span) == FRC_OK && span.blinks == 11 * UINT64_C(58500) && span.frac == 0);
    CHECK(frc_span_of_level(7, &span) == FRC_OK && span.blinks == 4 * 1625 && span.frac == 0);
    CHECK(frc_span_of_level(12, &span) == FRC_OK && span.blinks == 1 && span.frac == 0);
    /* "level 13 is floor(2^32 / 7) units, level 23 is 1 unit, and from level
     * 24 on the result is the zero span, still with FRC_OK" */
    CHECK(frc_span_of_level(13, &span) == FRC_OK && span.blinks == 0 && span.frac == 0xFFFFFFFFu / 7);
    CHECK(frc_span_of_level(23, &span) == FRC_OK && span.blinks == 0 && span.frac == 1);
    CHECK(frc_span_of_level(24, &span) == FRC_OK && span.negative == 0 && span.blinks == 0 && span.frac == 0);
    CHECK(frc_span_of_level(62, &span) == FRC_OK && span.blinks == 0 && span.frac == 0);
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "0ν");
    /* "For the exact ratio at those levels use frc_level_per_blink (to level 35)" */
    {
        uint64_t per_num, per_den;
        CHECK(frc_level_per_blink(24, &per_num, &per_den) == FRC_OK && per_num == UINT64_C(12486474000) && per_den == 1);
        CHECK(frc_level_per_blink(35, &per_num, &per_den) == FRC_OK && per_den == 1);
    }
    /* "FRC_ERR_ARG for level >= FRC_LEVEL_COUNT or out == NULL" */
    CHECK(frc_span_of_level(63, &span) == FRC_ERR_ARG);
    CHECK(frc_span_of_level(0, NULL) == FRC_ERR_ARG);

    /* frc_span_parts: "greedily from the largest unit that fits ... NON-ZERO parts;
     * with keep_zeros the next max_parts consecutive units, zeros included" */
    span.negative = 0; span.blinks = 3 * UINT64_C(58500) + 5 * 25; span.frac = 0;
    CHECK_EQ_U64(frc_span_parts(&span, 2, 0, parts), 2);
    CHECK(parts[0].unit == FRC_UNIT_HELION && parts[0].count == 3 && parts[1].unit == FRC_UNIT_FLAVON && parts[1].count == 5);
    CHECK_EQ_U64(frc_span_parts(&span, 2, 1, parts), 2);
    CHECK(parts[0].unit == FRC_UNIT_HELION && parts[0].count == 3 && parts[1].unit == FRC_UNIT_PREON && parts[1].count == 0);
    /* "at least 1: a sub-Blink span gives 0 ν"; "0 when max_parts is 0 or a pointer is NULL" */
    span.blinks = 0; span.frac = 12345;
    CHECK_EQ_U64(frc_span_parts(&span, 3, 0, parts), 1);
    CHECK(parts[0].unit == FRC_UNIT_BLINK && parts[0].count == 0);
    CHECK_EQ_U64(frc_span_parts(&span, 0, 0, parts), 0);
    CHECK_EQ_U64(frc_span_parts(NULL, 2, 0, parts), 0);
    CHECK_EQ_U64(frc_span_parts(&span, 2, 0, NULL), 0);

    /* frc_format_span: the decimal rule, each clause of the comment */
    {
        static const struct { uint8_t negative; uint64_t blinks; double frac; const char *text; } spans[] = {
            {0, 2, 0.6, "2.6ν"},        {0, 9, 0.96, "10ν"},        {0, 0, 0.14, "0.14ν"},
            {0, 0, 0.997, "1ν"},        {0, 0, 0.0005, "<0.001ν"},  {0, 0, 0.00005, "<0.001ν"},
            {1, 0, 0.00005, "<0.001ν"}, {1, 0, 0.0005, "<0.001ν"},  {1, 2, 0.6, "−2.6ν"},
            {0, 0, 0.0, "0ν"},          {1, 0, 0.0, "0ν"},          {1, 0, 0.0011, "−0.0011ν"},
            {0, 10, 0.6, "10ν"},        {0, 24, 0.99, "24ν"},       {0, 4, 0.0, "4ν"},
            {0, 0, 0.0032, "0.0032ν"},  {0, 2, 0.25, "2.3ν"},       {1, 25, 0.0, "−1λ"},
        };
        size_t i;
        for (i = 0; i < sizeof spans / sizeof spans[0]; i++) {
            span.negative = spans[i].negative;
            span.blinks = spans[i].blinks;
            span.frac = (frc_frac_t)(spans[i].frac * 4294967296.0);
            CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
            CHECK_STR(text, spans[i].text);
        }
    }
    /* "the reference keeps the decimal below 10 Blinks only, and so does this
     * core: 12.4 Blinks read \"12ν\"" */
    span.negative = 0; span.blinks = 12; span.frac = 0x66666666u;
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "12ν");
    span.blinks = 9;
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "9.4ν");
    /* "The text is rounded from what frc_span_t holds": a hand-filled span a
     * hair under 1.05 reads "1ν" */
    span.blinks = 1; span.frac = 214748364u; /* floor(0.05 * 2^32) */
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1ν");
    /* "max_parts 0 means the default precision, 2" */
    span.negative = 0; span.blinks = UINT64_C(4504500) * 2 + 58500 * 5 + 1625 * 7 + 25 * 9 + 11; span.frac = 0;
    CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "2ε 5η");
    CHECK(frc_format_span(&span, 5, text, sizeof text) > 0);
    CHECK_STR(text, "2ε 5η 7ι 9λ 11ν");
    CHECK(frc_format_span(NULL, 0, text, sizeof text) == FRC_ERR_ARG);

    TEST_SECTION("rates and pB");
    /* "Real time is {FRC_UNIT_BLINK, 0, 1}. FRC_ERR_ARG for i >= FRC_RATE_COUNT" */
    CHECK(frc_rate_preset(0, &rate) == FRC_OK && rate.unit == FRC_UNIT_BLINK && rate.count == 0 && rate.factor == 1);
    CHECK(frc_rate_preset(5, &rate) == FRC_OK && rate.unit == FRC_UNIT_PREON && rate.count == 4 && rate.factor == 6500);
    CHECK(frc_rate_preset(FRC_RATE_COUNT, &rate) == FRC_ERR_ARG);
    CHECK(frc_format_rate(&rate, text, sizeof text) > 0);
    CHECK_STR(text, "4ι per Blink");
    /* "xi = 7, omicron = 77, pi = 693, rho = 2772; Helion = 1/58500" */
    CHECK(frc_level_per_blink(6, &num, &den) == FRC_OK && num == 1 && den == 58500);
    CHECK(frc_level_per_blink(12, &num, &den) == FRC_OK && num == 1 && den == 1);
    /* "FRC_ERR_OVERFLOW when the value does not fit 64 bits (level > 35)" */
    CHECK(frc_level_per_blink(35, &num, &den) == FRC_OK && num == UINT64_C(18279729693225000000) && den == 1);
    CHECK(frc_level_per_blink(36, &num, &den) == FRC_ERR_OVERFLOW);
    CHECK(frc_level_per_blink(63, &num, &den) == FRC_ERR_ARG);
    /* frc_format_per_blink, each clause of the comment */
    {
        static const struct { uint64_t num, den; const char *text; } rates[] = {
            {7, 1, "7 pB"},                      {342, 10, "34.2 pB"},       {14, 2, "7 pB"},
            {0, 1, "—"},                         {1, 0, "—"},                {1, 25, "1 per 25 Blinks"},
            {1, 5, "1 per 5 Blinks"},            {2, 50, "1 per 25 Blinks"}, {3, 4, "0.75 pB"},
            {1, 20, "1 per 20 Blinks"},          {4001, 100000, "0.04 pB"},
            {1, 26, "1 per 26 Blinks"},          {1, 58500, "1 per 58\xE2\x80\xAF" "500 Blinks"},
            {2, 51, "1 per 26 Blinks"},          {9994, 100, "99.9 pB"},     {9995, 100, "100 pB"},
            {12345, 100, "123.5 pB"},            {99949, 100, "999.5 pB"},   {9995, 10, "1\xE2\x80\xAF" "000 pB"},
            {2772, 1, "2\xE2\x80\xAF" "772 pB"}, {456, 10000, "0.0456 pB"},  {5, 2, "2.5 pB"},
        };
        size_t i;
        for (i = 0; i < sizeof rates / sizeof rates[0]; i++) {
            CHECK(frc_format_per_blink(rates[i].num, rates[i].den, text, sizeof text) > 0);
            CHECK_STR(text, rates[i].text);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Pure wording                                                               */
/* ------------------------------------------------------------------------- */

static void test_pure_wording(void)
{
    const frc_speed_t c = frc_medium_speed(FRC_MEDIUM_VACUUM);
    frc_speed_t speed;
    char text[96];
    int medium;

    TEST_SECTION("frc_format_count / frc_format_level_in_unit");
    /* "999", "1 000", "58 500", "142 033 641 750 000"; "The longest text (UINT64_MAX) is 38 bytes + NUL" */
    CHECK(frc_format_count(999, text, sizeof text) > 0);
    CHECK_STR(text, "999");
    CHECK(frc_format_count(1000, text, sizeof text) > 0);
    CHECK_STR(text, "1\xE2\x80\xAF" "000");
    CHECK(frc_format_count(58500, text, sizeof text) > 0);
    CHECK_STR(text, "58\xE2\x80\xAF" "500");
    CHECK(frc_format_count(UINT64_C(142033641750000), text, sizeof text) > 0);
    CHECK_STR(text, "142\xE2\x80\xAF" "033\xE2\x80\xAF" "641\xE2\x80\xAF" "750\xE2\x80\xAF" "000");
    CHECK_EQ_I64(frc_format_count(UINT64_MAX, text, 39), 38);
    CHECK_EQ_I64(frc_format_count(UINT64_MAX, text, 38), FRC_ERR_BUFFER);
    /* "The Helion (level 6) is 58 500ν in Blinks, 2 340λ in Flavons, 36ι in Preons and 1η in Helions;
     * level 5 is 11η" */
    CHECK(frc_format_level_in_unit(6, FRC_UNIT_BLINK, text, sizeof text) > 0);
    CHECK_STR(text, "58\xE2\x80\xAF" "500ν");
    CHECK(frc_format_level_in_unit(6, FRC_UNIT_FLAVON, text, sizeof text) > 0);
    CHECK_STR(text, "2\xE2\x80\xAF" "340λ");
    CHECK(frc_format_level_in_unit(6, FRC_UNIT_PREON, text, sizeof text) > 0);
    CHECK_STR(text, "36ι");
    CHECK(frc_format_level_in_unit(6, FRC_UNIT_HELION, text, sizeof text) > 0);
    CHECK_STR(text, "1η");
    CHECK(frc_format_level_in_unit(5, FRC_UNIT_HELION, text, sizeof text) > 0);
    CHECK_STR(text, "11η");
    /* "The longest text is 21 bytes + NUL (7 319 812 500ν)" */
    CHECK_EQ_I64(frc_format_level_in_unit(0, FRC_UNIT_BLINK, text, 22), 21);
    CHECK_STR(text, "7\xE2\x80\xAF" "319\xE2\x80\xAF" "812\xE2\x80\xAF" "500ν");
    /* "FRC_ERR_ARG when unit is not a frc_unit_t, when level > 12 ..., or when the unit is coarser
     * than the level (... a Helion counted in Nuclides)"; it leaves "" */
    text[0] = 'x';
    CHECK(frc_format_level_in_unit(6, FRC_UNIT_COUNT, text, sizeof text) == FRC_ERR_ARG && text[0] == '\0');
    CHECK(frc_format_level_in_unit(6, (frc_unit_t)-1, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_in_unit(13, FRC_UNIT_BLINK, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_in_unit(12, FRC_UNIT_BLINK, text, sizeof text) > 0);
    CHECK(frc_format_level_in_unit(6, FRC_UNIT_NUCLIDE, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_in_unit(4, FRC_UNIT_NUCLIDE, text, sizeof text) > 0);
    /* the buffer is the exception: NULL is a buffer of size 0 */
    CHECK(frc_format_level_in_unit(6, FRC_UNIT_BLINK, NULL, 0) == FRC_ERR_BUFFER);
    CHECK(frc_format_level_in_unit(13, FRC_UNIT_BLINK, NULL, 0) == FRC_ERR_ARG);

    TEST_SECTION("frc_format_level_rate");
    /* "level <= 12: byte for byte the text of frc_format_per_blink for the ratio of
     * frc_level_per_blink(level): 1 pB (level 12), 1 per 5 Blinks, 1 per 25 Blinks,
     * 1 per 325 Blinks ...
     * 1 per 58 500 Blinks (the Helion). `digits` changes nothing there." */
    {
        static const char *const down[4] = {"1 pB", "1 per 5 Blinks", "1 per 25 Blinks", "1 per 325 Blinks"};
        uint8_t level, digits;
        for (level = 0; level <= 12; level++) {
            uint64_t num, den;
            char want[48];
            CHECK(frc_level_per_blink(level, &num, &den) == FRC_OK);
            CHECK(frc_format_per_blink(num, den, want, sizeof want) > 0);
            for (digits = 0; digits <= 9; digits += (digits == 0 ? 2 : 1)) {
                CHECK(frc_format_level_rate(level, digits, text, sizeof text) > 0);
                CHECK_STR(text, want);
            }
            if (level >= 9) {
                CHECK_STR(text, down[12 - level]);
            }
        }
        CHECK(frc_format_level_rate(6, 0, text, sizeof text) > 0);
        CHECK_STR(text, "1 per 58\xE2\x80\xAF" "500 Blinks");
    }
    /* "the rate is a whole number N: 7, 77, 693, 2 772"; "digits == 0: N in full, grouped" */
    CHECK(frc_format_level_rate(13, 0, text, sizeof text) > 0);
    CHECK_STR(text, "7 pB");
    CHECK(frc_format_level_rate(14, 0, text, sizeof text) > 0);
    CHECK_STR(text, "77 pB");
    CHECK(frc_format_level_rate(15, 0, text, sizeof text) > 0);
    CHECK_STR(text, "693 pB");
    CHECK(frc_format_level_rate(16, 0, text, sizeof text) > 0);
    CHECK_STR(text, "2\xE2\x80\xAF" "772 pB");
    CHECK(frc_format_level_rate(29, 0, text, sizeof text) > 0);
    CHECK_STR(text, "142\xE2\x80\xAF" "033\xE2\x80\xAF" "641\xE2\x80\xAF" "750\xE2\x80\xAF" "000 pB");
    /* "and 42 digits at level 62"; "The longest text is 84 bytes + NUL (level 62 in full)" */
    CHECK_EQ_I64(frc_format_level_rate(62, 0, text, 85), 84);
    CHECK_EQ_I64(frc_format_level_rate(62, 0, text, 84), FRC_ERR_BUFFER);
    {
        int digits = 0;
        size_t i;
        CHECK(frc_format_level_rate(62, 0, text, sizeof text) > 0);
        for (i = 0; text[i] != '\0'; i++) {
            digits += (text[i] >= '0' && text[i] <= '9');
        }
        CHECK_EQ_I64(digits, 42);
    }
    /* "digits 2..9: N below 1 000 000 is still written in full (900 900 pB, level 19)" */
    CHECK(frc_format_level_rate(19, 2, text, sizeof text) > 0);
    CHECK_STR(text, "900\xE2\x80\xAF" "900 pB");
    CHECK(frc_format_level_rate(19, 9, text, sizeof text) > 0);
    CHECK_STR(text, "900\xE2\x80\xAF" "900 pB");
    /* "from 1 000 000 up it is d.ddd × 10ⁿ pB ...: 1.420 × 10¹⁴ pB (level 29, 4 digits)" */
    CHECK(frc_format_level_rate(29, 4, text, sizeof text) > 0);
    CHECK_STR(text, "1.420 × 10¹⁴ pB");
    /* "4 504 500 (level 20) is the tie 4.504 × 10⁶ pB, and with 9 digits 4.50450000 × 10⁶ pB" */
    CHECK(frc_format_level_rate(20, 0, text, sizeof text) > 0);
    CHECK_STR(text, "4\xE2\x80\xAF" "504\xE2\x80\xAF" "500 pB");
    CHECK(frc_format_level_rate(20, 4, text, sizeof text) > 0);
    CHECK_STR(text, "4.504 × 10⁶ pB");
    CHECK(frc_format_level_rate(20, 9, text, sizeof text) > 0);
    CHECK_STR(text, "4.50450000 × 10⁶ pB");
    /* "A mantissa that rounds up to 10 is written as 1 with the next exponent; no rate of the 63
     * rungs does, none begins with 95 or more": the first digit in full is the first digit rounded */
    {
        uint8_t level, digits;
        char full[96];
        for (level = 20; level < FRC_LEVEL_COUNT; level++) {
            CHECK(frc_format_level_rate(level, 0, full, sizeof full) > 0);
            CHECK(!(full[0] == '9' && full[1] >= '5' && full[1] <= '9')); /* (a group space is no digit) */
            for (digits = 2; digits <= 9; digits++) {
                CHECK(frc_format_level_rate(level, digits, text, sizeof text) > 0);
                CHECK(text[0] == full[0] && text[1] == '.');
            }
        }
    }
    /* "in the power-of-ten form of frc_format_quantity": the same sign, superscripts and spaces */
    {
        frc_sci_t value = {UINT64_C(14203364175000000), -2}; /* 1.4203364175e14 */
        char quantity[48];
        size_t shared = strlen("1.420 × 10¹⁴ ");
        CHECK(frc_format_quantity(&value, FRC_QUANTITY_FREQUENCY, 4, quantity, sizeof quantity) > 0);
        CHECK_STR(quantity, "142.0 THz"); /* inside the prefixes: no power of ten here */
        value.exp10 = 30; /* 1.42e46 Hz: above them */
        CHECK(frc_format_quantity(&value, FRC_QUANTITY_FREQUENCY, 4, quantity, sizeof quantity) > 0);
        CHECK_STR(quantity, "1.420 × 10⁴⁶ Hz");
        CHECK(frc_format_level_rate(29, 4, text, sizeof text) > 0);
        CHECK(strncmp(text, quantity, strlen("1.420 × 10")) == 0);
        CHECK(text[shared - 1] == ' ' && strcmp(text + shared, "pB") == 0);
    }
    /* "FRC_ERR_ARG for level >= FRC_LEVEL_COUNT, digits == 1 or digits > 9. `digits` is checked at
     * every level, also down to the Blink" */
    text[0] = 'x';
    CHECK(frc_format_level_rate(FRC_LEVEL_COUNT, 0, text, sizeof text) == FRC_ERR_ARG && text[0] == '\0');
    CHECK(frc_format_level_rate(20, 1, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_rate(20, 10, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_rate(6, 1, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_rate(6, 10, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_rate(6, 9, text, sizeof text) > 0);
    CHECK(frc_format_level_rate(FRC_LEVEL_COUNT - 1, 9, text, sizeof text) > 0);

    TEST_SECTION("frc_format_light_span");
    /* "c = frc_medium_speed(FRC_MEDIUM_VACUUM)"; "For light in vacuum it is the rung's own span
     * (level 6: 1η)" */
    CHECK(c.num == 299792458 && c.den == 1);
    CHECK(frc_format_light_span(6, c, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1η");
    /* "One Helion in each medium of S16.3 reads 1η, 24ι 1λ, 3.5ν, 2.05ξ, 5.15ο, 1.80ο, 6.15χ" */
    {
        static const char *const helion[FRC_MEDIUM_COUNT] = {"1η", "24ι 1λ", "3.5ν", "2.05ξ", "5.15ο", "1.80ο", "6.15χ"};
        for (medium = 0; medium < FRC_MEDIUM_COUNT; medium++) {
            CHECK(frc_format_light_span(6, frc_medium_speed((frc_medium_t)medium), 0, text, sizeof text) > 0);
            CHECK_STR(text, helion[medium]);
        }
    }
    /* "B >= 1 Blink: the text of frc_format_span(span, 0) ...; exactly 1.05 Blinks read 1.1ν" */
    speed.num = UINT64_C(105) * 299792458; speed.den = 100;
    CHECK(frc_format_light_span(12, speed, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1.1ν");
    /* "5.15ο, 10.8τ, 1.00ξ (light at level 13)" */
    CHECK(frc_format_light_span(12, frc_medium_speed(FRC_MEDIUM_DIAMOND), 0, text, sizeof text) > 0);
    CHECK_STR(text, "10.8τ");
    CHECK(frc_format_light_span(13, c, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1.00ξ");
    /* "rounded half to even from the exact value": 1.005ξ and 1.015ξ */
    speed.num = UINT64_C(1005) * 299792458; speed.den = 7000;
    CHECK(frc_format_light_span(12, speed, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1.00ξ");
    speed.num = UINT64_C(1015) * 299792458; speed.den = 7000;
    CHECK(frc_format_light_span(12, speed, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1.02ξ");
    /* "A count that the rounding brings up to that factor is one whole cycle of the next coarser
     * rung ...: 10.95ο reads 1.00ξ, and 0.9999 Blinks read 1.00ν" */
    speed.num = UINT64_C(1095) * 299792458; speed.den = 7700;
    CHECK(frc_format_light_span(12, speed, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1.00ξ");
    speed.num = UINT64_C(1094) * 299792458; speed.den = 7700;
    CHECK(frc_format_light_span(12, speed, 0, text, sizeof text) > 0);
    CHECK_STR(text, "10.9ο");
    speed.num = UINT64_C(9999) * 299792458; speed.den = 10000;
    CHECK(frc_format_light_span(12, speed, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1.00ν");
    /* "below level 62 ...: 8.35 × 10⁻⁸ ο2 (level 57, chemical transport). A count that rounds up
     * to one whole cycle reads 1.00ο2" */
    CHECK(frc_format_light_span(57, frc_medium_speed(FRC_MEDIUM_CHEMICAL), 0, text, sizeof text) > 0);
    CHECK_STR(text, "8.35 × 10⁻⁸ ο2");
    CHECK(frc_format_light_span(62, speed, 0, text, sizeof text) > 0); /* 0.9999 of a cycle */
    CHECK_STR(text, "1.00ο2");
    CHECK(frc_format_light_span(62, c, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1.00ο2");
    /* "with_word != 0 appends " light" to any of them: 1η light, 5.15ο light" */
    CHECK(frc_format_light_span(6, c, 1, text, sizeof text) > 0);
    CHECK_STR(text, "1η light");
    CHECK(frc_format_light_span(6, frc_medium_speed(FRC_MEDIUM_AIR), 7, text, sizeof text) > 0);
    CHECK_STR(text, "5.15ο light");
    CHECK(frc_format_light_span(57, frc_medium_speed(FRC_MEDIUM_CHEMICAL), 1, text, sizeof text) > 0);
    CHECK_STR(text, "8.35 × 10⁻⁸ ο2 light");
    /* "A speed above c is allowed" */
    speed.num = UINT64_C(2) * 299792458; speed.den = 1;
    CHECK(frc_format_light_span(6, speed, 0, text, sizeof text) > 0);
    CHECK_STR(text, "2η");
    /* "The longest text is 28 bytes + NUL with the word": the slowest speed at the last rung */
    speed.num = 1; speed.den = UINT64_MAX;
    CHECK_EQ_I64(frc_format_light_span(62, speed, 1, text, 29), 28);
    CHECK_STR(text, "1.81 × 10⁻²⁸ ο2 light");
    /* "FRC_ERR_ARG for level >= FRC_LEVEL_COUNT or a speed with a zero num or den" */
    text[0] = 'x';
    CHECK(frc_format_light_span(FRC_LEVEL_COUNT, c, 0, text, sizeof text) == FRC_ERR_ARG && text[0] == '\0');
    speed.num = 0; speed.den = 1;
    CHECK(frc_format_light_span(6, speed, 0, text, sizeof text) == FRC_ERR_ARG);
    speed.num = 1; speed.den = 0;
    CHECK(frc_format_light_span(6, speed, 0, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_light_span(6, frc_medium_speed(FRC_MEDIUM_COUNT), 0, text, sizeof text) == FRC_ERR_ARG);
    /* "FRC_ERR_OVERFLOW when the span is 2^64 Blinks or more ... (it takes a speed above 2.5e9 c).
     * Both leave "" in the buffer": the last speed whose Alpha fits, and the next */
    speed.num = UINT64_C(755510438000197362); speed.den = 1; /* 2.52e9 c */
    CHECK(frc_format_light_span(0, speed, 0, text, sizeof text) > 0);
    CHECK_STR(text, "2520111556α 3β");
    speed.num += 1;
    text[0] = 'x';
    CHECK(frc_format_light_span(0, speed, 0, text, sizeof text) == FRC_ERR_OVERFLOW && text[0] == '\0');
    speed.num = UINT64_C(2500000000) * 299792458; /* 2.5e9 c still fits, at the Alpha and so everywhere */
    CHECK(frc_format_light_span(0, speed, 0, text, sizeof text) > 0);
}

/* ------------------------------------------------------------------------- */
/* S16 ladder                                                                 */
/* ------------------------------------------------------------------------- */

static void test_ladder(void)
{
    frc_interval_t reference, nothing, interval;
    frc_speed_t speed;
    frc_sci_t value;
    uint64_t count = 99;
    char text[64];

    reference.num = FRC_INTERVAL_REFERENCE_MS;
    reference.den = 1;
    nothing.num = 0;
    nothing.den = 0;

    TEST_SECTION("intervals and media");
    /* "The interval that holds instant t (closing instant: the last interval)" */
    CHECK(frc_interval_at(CAT, frc_catalogue_last(CAT), &interval) == FRC_OK);
    CHECK(interval.den != 0 &&
          interval.num * 1000 == (uint64_t)(frc_catalogue_instant(CAT, 69) - frc_catalogue_instant(CAT, 68)) * interval.den);
    CHECK(frc_interval_at(CAT, frc_catalogue_last(CAT) + 1, &interval) == FRC_ERR_RANGE);
    CHECK(frc_interval_at(CAT, INSIDE, NULL) == FRC_ERR_ARG);
    /* "NOT reduced to lowest terms (Saros 141: 39259007522784 / 69)"; "{0, 0} for an unusable catalogue" */
    interval = frc_interval_mean(CAT);
    CHECK(interval.num == UINT64_C(39259007522784) && interval.den == 69);
    interval = frc_interval_mean(NULL);
    CHECK(interval.num == 0 && interval.den == 0);
    /* "A nominal scale from any Alpha length in milliseconds (interval = alpha / 4)" */
    interval = frc_interval_of_alpha_ms(4 * FRC_INTERVAL_REFERENCE_MS);
    CHECK(interval.den != 0 && interval.num == FRC_INTERVAL_REFERENCE_MS * interval.den);
    /* "{0, 0} when `medium` is not a frc_medium_t" */
    speed = frc_medium_speed(FRC_MEDIUM_COUNT);
    CHECK(speed.num == 0 && speed.den == 0);
    CHECK(frc_format_medium_id(FRC_MEDIUM_COUNT, text, sizeof text) == FRC_ERR_ARG);

    TEST_SECTION("zero intervals and bad levels are refused");
    /* "every ladder function refuses an interval or a speed with a zero num or
     * den (FRC_ERR_ARG, FRC_BAND_NONE, or 0.0)"; "Level 0..62" */
    CHECK(frc_period(12, nothing, &value) == FRC_ERR_ARG);
    CHECK(frc_frequency(12, nothing, &value) == FRC_ERR_ARG);
    CHECK(frc_length(12, reference, speed, &value) == FRC_ERR_ARG);
    CHECK(frc_band(12, nothing) == FRC_BAND_NONE);
    CHECK(frc_period(63, reference, &value) == FRC_ERR_ARG);
    CHECK(frc_period(62, reference, &value) == FRC_OK);
    /* "exactly FRC_SCI_DIGITS significant digits (10^16 <= mantissa < 10^17)" */
    CHECK(value.mantissa >= UINT64_C(10000000000000000) && value.mantissa < UINT64_C(100000000000000000));
    CHECK_EQ_I64(value.exp10, -43 - 16); /* 4.834 x 10^-43 s */
    CHECK(frc_period(12, reference, NULL) == FRC_ERR_ARG);

    TEST_SECTION("devices");
    /* 48 kHz: a Blink is 14924.26 ticks, half up */
    CHECK(frc_device_ticks(12, reference, 48000, &count) == FRC_OK);
    CHECK_EQ_U64(count, 14924);
    /* "FRC_ERR_REFUSED for a period under half a tick" - level 21 lasts 9.86 ns */
    CHECK(frc_device_ticks(21, reference, 48000, &count) == FRC_ERR_REFUSED);
    CHECK_EQ_U64(count, 0); /* "*ticks / *step is 0 whenever the status is not FRC_OK" */
    /* "A period of EXACTLY half a tick counts 1 tick": T(0) = 4 I, I = 125 ms, rate 1 Hz */
    interval.num = 125; interval.den = 1;
    CHECK(frc_device_ticks(0, interval, 1, &count) == FRC_OK);
    CHECK_EQ_U64(count, 1);
    interval.num = 124;
    CHECK(frc_device_ticks(0, interval, 1, &count) == FRC_ERR_REFUSED);
    /* "FRC_ERR_OVERFLOW if the tick count does not fit 64 bits": an Alpha at 10^12 ticks/s */
    CHECK(frc_device_ticks(0, reference, UINT64_C(1000000000000), &count) == FRC_ERR_OVERFLOW);
    CHECK_EQ_U64(count, 0);
    /* "a frequency of EXACTLY rate/2 is made (step 2^(bits-1))": f(0) = 2 Hz at rate 4 */
    CHECK(frc_device_step(0, interval, 4, 16, &count) == FRC_ERR_REFUSED); /* I = 124 ms: just above 2 Hz */
    interval.num = 125;
    CHECK(frc_device_step(0, interval, 4, 16, &count) == FRC_OK);
    CHECK_EQ_U64(count, 0x8000);
    CHECK(frc_device_step(0, interval, 4, 64, &count) == FRC_OK);
    CHECK_EQ_U64(count, UINT64_C(0x8000000000000000));
    /* "or a step that rounds to nothing" */
    CHECK(frc_device_step(0, reference, 48000, 16, &count) == FRC_ERR_REFUSED);
    CHECK_EQ_U64(count, 0);
    /* "FRC_ERR_ARG for rate 0, bits 0 or bits > 64, level > 62, a zero interval part or a NULL output" */
    count = 99;
    CHECK(frc_device_step(12, reference, 48000, 0, &count) == FRC_ERR_ARG && count == 0);
    CHECK(frc_device_step(12, reference, 48000, 65, &count) == FRC_ERR_ARG);
    CHECK(frc_device_step(12, reference, 0, 32, &count) == FRC_ERR_ARG);
    CHECK(frc_device_step(63, reference, 48000, 32, &count) == FRC_ERR_ARG);
    CHECK(frc_device_step(12, nothing, 48000, 32, &count) == FRC_ERR_ARG);
    CHECK(frc_device_step(12, reference, 48000, 32, NULL) == FRC_ERR_ARG);
    CHECK(frc_device_ticks(12, reference, 0, &count) == FRC_ERR_ARG);
    CHECK(frc_device_ticks(63, reference, 48000, &count) == FRC_ERR_ARG);
    CHECK(frc_device_ticks(12, nothing, 48000, &count) == FRC_ERR_ARG);
    CHECK(frc_device_ticks(12, reference, 48000, NULL) == FRC_ERR_ARG);

    TEST_SECTION("frc_format_quantity");
    /* the examples of the comment */
    CHECK(frc_period(12, reference, &value) == FRC_OK);
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 0, text, sizeof text) > 0);
    CHECK_STR(text, "310.9 ms");
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 9, text, sizeof text) > 0);
    CHECK_STR(text, "310.922105 ms"); /* 51724807 / 166359375 s = 0.31092210486... */
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 1, text, sizeof text) > 0);
    CHECK_STR(text, "300 ms");
    /* "FRC_ERR_ARG for digits > 9, an unknown quantity, a NULL value or a
     * mantissa outside [10^16, 10^17)" */
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 10, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_quantity(&value, (frc_quantity_t)3, 4, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_quantity(NULL, FRC_QUANTITY_TIME, 4, text, sizeof text) == FRC_ERR_ARG);
    value.mantissa = UINT64_C(100000000000000000);
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 4, text, sizeof text) == FRC_ERR_ARG);
    value.mantissa = UINT64_C(9999999999999999);
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 4, text, sizeof text) == FRC_ERR_ARG);
    value.mantissa = 0;
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 4, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_period(12, reference, 10, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_period(12, nothing, 4, text, sizeof text) == FRC_ERR_ARG);
    /* "a rounded value that reaches the next unit is written as one of it (1.000 min ...)" */
    value.mantissa = UINT64_C(59999600000000000); value.exp10 = -15; /* 59.9996 s */
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 4, text, sizeof text) > 0);
    CHECK_STR(text, "1.000 min");
    /* "time from 1000 y up reads d.ddd × 10ⁿ y" */
    value.mantissa = UINT64_C(31557600000000000); value.exp10 = -16 + 11; /* 10^4 Julian years */
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_TIME, 4, text, sizeof text) > 0);
    CHECK_STR(text, "1.000 × 10⁴ y");
    /* "a length from 10^18 m up and a frequency below 1 pHz use the power-of-ten form in m / Hz" */
    value.mantissa = UINT64_C(25000000000000000); value.exp10 = -16 + 18;
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_LENGTH, 4, text, sizeof text) > 0);
    CHECK_STR(text, "2.500 × 10¹⁸ m");
    value.exp10 = -16 - 13;
    CHECK(frc_format_quantity(&value, FRC_QUANTITY_FREQUENCY, 4, text, sizeof text) > 0);
    CHECK_STR(text, "2.500 × 10⁻¹³ Hz");
    /* "\"no band\" for FRC_BAND_NONE (design ruling 4)" */
    CHECK(frc_format_band(FRC_BAND_NONE, text, sizeof text) == 7);
    CHECK_STR(text, "no band");
    CHECK(frc_format_band((frc_band_t)9, text, sizeof text) == FRC_ERR_ARG);

#ifndef FRC_NO_FLOAT
    TEST_SECTION("doubles");
    /* "Defined for exp10 in -140..140 ...; 0.0 outside it and for an invalid or NULL frc_sci_t" */
    value.mantissa = UINT64_C(10000000000000000);
    value.exp10 = -16;
    CHECK(frc_sci_to_double(&value) == 1.0);
    value.exp10 = 140;
    CHECK(frc_sci_to_double(&value) == 1e156);
    value.exp10 = -140;
    CHECK(frc_sci_to_double(&value) == 1e-124);
    value.exp10 = 141;
    CHECK(frc_sci_to_double(&value) == 0.0);
    value.exp10 = -141;
    CHECK(frc_sci_to_double(&value) == 0.0);
    value.mantissa = 5;
    value.exp10 = 0;
    CHECK(frc_sci_to_double(&value) == 0.0);
    CHECK(frc_sci_to_double(NULL) == 0.0);
    /* "0 on a bad level" */
    CHECK(frc_period_seconds(63, reference) == 0.0 && frc_frequency_hz(63, reference) == 0.0);
    CHECK(frc_period_seconds(12, nothing) == 0.0);
    CHECK(frc_length_metres(12, reference, speed) == 0.0);
    /* "FRC_FRAC_TO_DOUBLE: frc_frac_t as a double in [0, 1)" */
    CHECK(FRC_FRAC_TO_DOUBLE(FRC_FRAC_HALF) == 0.5 && FRC_FRAC_TO_DOUBLE(0xFFFFFFFFu) < 1.0);
#endif
}

/* ------------------------------------------------------------------------- */
/* Arc geometry                                                               */
/* ------------------------------------------------------------------------- */

#ifndef FRC_NO_FLOAT
static void test_arcs(void)
{
    frc_arc_t root = frc_arc_wave_root(0), child;
    float x = 9, y = 9;

    TEST_SECTION("arcs");
    /* "half-cycle `half` is a unit semicircle centred at (2*half + 1, 0)" */
    CHECK(root.cx == 1.0f && root.cy == 0.0f && root.radius == 1.0f && fabsf(fabsf(root.sweep) - FRC_PI) < 1e-6f);
    root = frc_arc_wave_root(3);
    CHECK(root.cx == 7.0f);
    /* "m == 1 returns the parent unchanged" */
    child = frc_arc_child(&root, 1, 0, 1);
    CHECK(child.cx == root.cx && child.cy == root.cy && child.radius == root.radius &&
          child.start == root.start && child.sweep == root.sweep);
    /* "a NULL parent, m == 0 or a slot of pi or more returns the empty arc (all fields 0)" */
    child = frc_arc_child(NULL, 5, 0, 0);
    CHECK(child.cx == 0 && child.cy == 0 && child.radius == 0 && child.start == 0 && child.sweep == 0);
    child = frc_arc_child(&root, 0, 0, 0);
    CHECK(child.radius == 0 && child.sweep == 0);
    root.sweep = 2 * FRC_PI; /* a full-circle carrier: "m >= 3" */
    child = frc_arc_child(&root, 2, 0, 0);
    CHECK(child.radius == 0 && child.sweep == 0);
    child = frc_arc_child(&root, 3, 0, 0);
    CHECK(child.radius > 0);
    /* "x or y may be NULL" */
    root = frc_arc_wave_root(0);
    frc_arc_point(&root, 0.0f, &x, NULL);
    frc_arc_point(&root, 0.0f, NULL, &y);
    frc_arc_point(&root, 0.0f, NULL, NULL);
    CHECK(fabsf(x - 0.0f) < 1e-6f && fabsf(y) < 1e-6f);
    frc_arc_point(&root, 1.0f, &x, &y);
    CHECK(fabsf(x - 2.0f) < 1e-5f && fabsf(y) < 1e-5f);
    /* "|error| < 1e-6 ... NaN for a non-finite argument" */
    CHECK(fabsf(frc_sin(FRC_PI / 6) - 0.5f) < 1e-6f && fabsf(frc_cos(FRC_PI / 3) - 0.5f) < 1e-6f);
    CHECK(isnan(frc_sin(NAN)) && isnan(frc_cos(INFINITY)) && isnan(frc_sin(-INFINITY)));
}
#endif

/* ------------------------------------------------------------------------- */
/* Civil time and text utilities                                              */
/* ------------------------------------------------------------------------- */

static frc_civil_t civil_of(int32_t year, int month, int day, int hour, int minute, int second, uint32_t us)
{
    frc_civil_t c;
    c.year = year; c.month = (uint8_t)month; c.day = (uint8_t)day;
    c.hour = (uint8_t)hour; c.minute = (uint8_t)minute; c.second = (uint8_t)second;
    c.weekday = 0; c.microsecond = us;
    return c;
}

static void test_civil_and_text(void)
{
    frc_civil_t civil;
    char text[64];

    TEST_SECTION("civil time");
    /* "2026-10-08T12:00:00.000Z"; "weekday 0 = Monday .. 6 = Sunday" */
    frc_civil_from_time(INSIDE, 0, &civil);
    CHECK(civil.year == 2026 && civil.month == 10 && civil.day == 8 && civil.hour == 12 && civil.weekday == 3);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) == 24);
    CHECK_STR(text, "2026-10-08T12:00:00.000Z");
    /* "a non-zero offset writes +02:00" */
    frc_civil_from_time(INSIDE, 7200, &civil);
    CHECK(frc_format_civil(&civil, 7200, text, sizeof text) > 0);
    CHECK_STR(text, "2026-10-08T14:00:00.000+02:00");
    /* "the month is carried into the year first (month 13 is next January,
     * month 0 last December) ... day 32 is 31 days after the 1st and day 0 the day before it" */
    civil = civil_of(2026, 13, 1, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_time_from_civil(&civil, 0), FRC_TIME_FROM_S(INT64_C(1798761600))); /* 2027-01-01 */
    civil = civil_of(2027, 0, 1, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_time_from_civil(&civil, 0), FRC_TIME_FROM_S(INT64_C(1796083200))); /* 2026-12-01 */
    civil = civil_of(2026, 1, 32, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_time_from_civil(&civil, 0), FRC_TIME_FROM_S(INT64_C(1769904000))); /* 2026-02-01 */
    civil = civil_of(2026, 2, 0, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_time_from_civil(&civil, 0), FRC_TIME_FROM_S(INT64_C(1769817600))); /* 2026-01-31 */
    civil = civil_of(2026, 10, 8, 14, 0, 0, 0);
    CHECK_EQ_I64(frc_time_from_civil(&civil, 7200), INSIDE);
    /* "FRC_TIME_NONE for civil == NULL or a year outside -290000..290000" */
    CHECK_EQ_I64(frc_time_from_civil(NULL, 0), FRC_TIME_NONE);
    civil = civil_of(290001, 1, 1, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_time_from_civil(&civil, 0), FRC_TIME_NONE);
    civil = civil_of(-290001, 1, 1, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_time_from_civil(&civil, 0), FRC_TIME_NONE);
    civil = civil_of(290000, 12, 31, 0, 0, 0, 0);
    CHECK(frc_time_from_civil(&civil, 0) != FRC_TIME_NONE);
    /* "Milliseconds are the microsecond field cut, not rounded" */
    civil = civil_of(2026, 10, 8, 12, 0, 0, 999999);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) > 0);
    CHECK_STR(text, "2026-10-08T12:00:00.999Z");
    /* "A year outside 0..9999 is written -0044 / +10000" */
    civil = civil_of(-44, 3, 15, 0, 0, 0, 0);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) > 0);
    CHECK_STR(text, "-0044-03-15T00:00:00.000Z");
    civil = civil_of(10000, 1, 1, 0, 0, 0, 0);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) > 0);
    CHECK_STR(text, "+10000-01-01T00:00:00.000Z");
    /* "An offset that is not whole minutes is cut towards zero and keeps its
     * sign (+00:00 for +59 s); only 0 is Z" */
    civil = civil_of(2026, 10, 8, 12, 0, 0, 0);
    CHECK(frc_format_civil(&civil, 59, text, sizeof text) > 0);
    CHECK_STR(text, "2026-10-08T12:00:00.000+00:00");
    CHECK(frc_format_civil(&civil, -59, text, sizeof text) > 0);
    CHECK_STR(text, "2026-10-08T12:00:00.000-00:00");
    CHECK(frc_format_civil(&civil, -(5 * 3600 + 30 * 60 + 59), text, sizeof text) > 0);
    CHECK_STR(text, "2026-10-08T12:00:00.000-05:30");
    /* "FRC_ERR_ARG unless the fields are a date and a time of day" */
    civil = civil_of(2026, 13, 1, 0, 0, 0, 0);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) == FRC_ERR_ARG);
    civil = civil_of(2026, 1, 0, 0, 0, 0, 0);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) == FRC_ERR_ARG);
    civil = civil_of(2026, 1, 1, 24, 0, 0, 0);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) == FRC_ERR_ARG);
    civil = civil_of(2026, 1, 1, 0, 60, 0, 0);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) == FRC_ERR_ARG);
    civil = civil_of(2026, 1, 1, 0, 0, 60, 0);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) == FRC_ERR_ARG);
    civil = civil_of(2026, 1, 1, 0, 0, 0, 1000000);
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_civil(NULL, 0, text, sizeof text) == FRC_ERR_ARG);

    /* frc_julian_fields_to_unix_ms: "the bare reinterpretation, applied to ANY
     * value ... call it for values below FRC_GREGORIAN_REFORM_MS" */
    frc_civil_from_time(FRC_TIME_FROM_MS(FRC_GREGORIAN_REFORM_MS), 0, &civil);
    CHECK(civil.year == 1582 && civil.month == 10 && civil.day == 15 && civil.hour == 0 && civil.microsecond == 0);
    /* S2.3: "wrong by the calendar difference: 10 days near 1582" - also when
     * asked about a value after the reform, where the difference keeps growing */
    CHECK_EQ_I64(frc_julian_fields_to_unix_ms(FRC_GREGORIAN_REFORM_MS - 1) - (FRC_GREGORIAN_REFORM_MS - 1),
                 INT64_C(10) * 86400000);
    CHECK_EQ_I64(frc_julian_fields_to_unix_ms(FRC_GREGORIAN_REFORM_MS) - FRC_GREGORIAN_REFORM_MS, INT64_C(10) * 86400000);
    CHECK_EQ_I64(frc_julian_fields_to_unix_ms(0), INT64_C(13) * 86400000); /* 1970: 13 days apart */

    TEST_SECTION("frc_to_ascii");
    /* the mappings of the comment, in its order */
    CHECK(frc_to_ascii("αβγδεζηθικλμνξοπρστυφχψω", text, sizeof text) == 24);
    CHECK_STR(text, "abgdezhqiklmnxoprstufcyw");
    CHECK(frc_to_ascii("−2ε 5η", text, sizeof text) > 0);
    CHECK_STR(text, "-2e 5h");
    CHECK(frc_to_ascii("2.454 × 10⁻²⁵ s", text, sizeof text) > 0);
    CHECK_STR(text, "2.454 x 10^-25 s");
    CHECK(frc_to_ascii("⬠II·3 448.7 µs ◻", text, sizeof text) > 0);
    CHECK_STR(text, "PII.3 448.7 us H");
    CHECK(frc_to_ascii("58\xE2\x80\xAF" "500", text, sizeof text) > 0); /* U+202F narrow no-break space */
    CHECK_STR(text, "58 500");
    /* "no-break and thin spaces, en and em dash, final sigma, a superscript plus in the caret run" */
    CHECK(frc_to_ascii("a\xC2\xA0" "b\xE2\x80\x89" "c \xE2\x80\x93 \xE2\x80\x94 \xCF\x82 10\xE2\x81\xBA\xC2\xB9\xC2\xB2",
                       text, sizeof text) > 0);
    CHECK_STR(text, "a b c - - s 10^+12");
    /* "Anything else that is not ASCII becomes ?, one per code point, and one
     * per byte for bytes that are not UTF-8" */
    CHECK(frc_to_ascii("\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8C\x91 \xFF\x80 \xE2\x82", text, sizeof text) > 0);
    CHECK_STR(text, "? ? ? ?? ??");
    CHECK(frc_to_ascii(NULL, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_to_ascii("αβγ", text, 3) == FRC_ERR_BUFFER);
    CHECK_STR(text, "ab");
}


/* ------------------------------------------------------------------------- */
/* The consolidated requests: each new sentence of fractonica.h               */
/* ------------------------------------------------------------------------- */

static void test_requests(void)
{
    char text[600];
    frc_cycle_t cycle;
    frc_glyph_t glyph;
    frc_span_t span;
    frc_date_t date;
    frc_sync_t sync;
    frc_pentad_t pentad;
    frc_sci_t low, high;
    frc_civil_t civil;
    frc_time_t points[5];
    int32_t offset;
    int has;

    TEST_SECTION("frc_time_at_phase");
    CHECK(frc_cycle_at(CAT, INSIDE, 6, &cycle) == FRC_OK);
    CHECK(frc_cycle_points(CAT, 6, cycle.cycle, points) == FRC_OK);
    /* "phase 0 gives the cycle's start, FRC_FRAC_QUARTER its peak, FRC_FRAC_HALF its node" */
    CHECK_EQ_I64(frc_time_at_phase(CAT, 6, cycle.cycle, 0), cycle.start);
    CHECK_EQ_I64(frc_time_at_phase(CAT, 6, cycle.cycle, FRC_FRAC_QUARTER), points[1]);
    CHECK_EQ_I64(frc_time_at_phase(CAT, 6, cycle.cycle, FRC_FRAC_HALF), points[2]);
    /* "frc_cycle_at(result) has index `cycle` and a phase >= `phase`, and one
     * microsecond earlier the phase is below it" */
    {
        frc_time_t r = frc_time_at_phase(CAT, 6, cycle.cycle, cycle.phase);
        frc_cycle_t at;
        CHECK(r <= INSIDE);
        CHECK(frc_cycle_at(CAT, r, 6, &at) == FRC_OK && at.cycle == cycle.cycle && at.phase >= cycle.phase);
        CHECK(frc_cycle_at(CAT, r - 1, 6, &at) == FRC_OK && at.cycle == cycle.cycle && at.phase < cycle.phase);
    }
    /* "FRC_TIME_NONE for an unusable catalogue, level > FRC_CLOCK_MAX_LEVEL,
     * cycle >= frc_cycle_count(cat, level), or a phase of the last (clipped)
     * cycle that lies after the closing instant" */
    CHECK_EQ_I64(frc_time_at_phase(NULL, 6, 0, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_phase(CAT, FRC_CLOCK_MAX_LEVEL + 1, 0, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_phase(CAT, 6, frc_cycle_count(CAT, 6), 0), FRC_TIME_NONE);
    CHECK(frc_cycle_by_index(CAT, 0, frc_cycle_count(CAT, 0) - 1, &cycle) == FRC_OK && cycle.clipped);
    CHECK_EQ_I64(frc_time_at_phase(CAT, 0, cycle.cycle, 0xFFFFFFFFu), FRC_TIME_NONE);
    {
        /* "That can happen at ANY level ... often at level 18": a phase in the
         * last microsecond of a cycle gives its end, which reads as the next */
        frc_cycle_t at;
        uint64_t c, first = 0, hits = 0;
        CHECK(frc_cycle_at(CAT, INSIDE, 18, &at) == FRC_OK);
        first = at.cycle;
        for (c = first; c < first + 300; c++) {
            frc_time_t r = frc_time_at_phase(CAT, 18, c, 0xFFFFFFFFu);
            CHECK(frc_cycle_at(CAT, r, 18, &at) == FRC_OK && (at.cycle == c || at.cycle == c + 1));
            if (at.cycle == c + 1) {
                CHECK(frc_cycle_by_index(CAT, 18, c, &at) == FRC_OK && r == at.end);
                hits++;
            }
        }
        CHECK(hits > 100); /* common there */
    }

    TEST_SECTION("frc_glyph_at_quarter");
    /* "With 2 strokes (M = 4) the states are 4, 8, 12 and 0" */
    CHECK(frc_glyph_at_quarter(FRC_PEAK, 2, &glyph) == FRC_OK && glyph.index == 4 && glyph.value == 4);
    CHECK(frc_glyph_at_quarter(FRC_NODE, 2, &glyph) == FRC_OK && glyph.index == 8 && glyph.value == 0
          && glyph.dotted);
    CHECK(frc_glyph_at_quarter(FRC_VALLEY, 2, &glyph) == FRC_OK && glyph.index == 12 && glyph.value == -4);
    CHECK(frc_glyph_at_quarter(FRC_END, 2, &glyph) == FRC_OK && glyph.index == 0 && glyph.derivative == 1);
    CHECK(frc_glyph_at_quarter((frc_quarter_t)4, 2, &glyph) == FRC_ERR_ARG);
    CHECK(frc_glyph_at_quarter(FRC_PEAK, 6, &glyph) == FRC_ERR_ARG);
    CHECK(frc_glyph_at_quarter(FRC_PEAK, 2, NULL) == FRC_ERR_ARG);

    TEST_SECTION("frc_sync_t.helion, frc_time_at_sync_phase, frc_pentad_t.first");
    CHECK(frc_sync_at(CAT, INSIDE, &sync) == FRC_OK);
    CHECK(sync.helion <= 18);
    CHECK(frc_pentad_at(CAT, INSIDE, &pentad) == FRC_OK);
    CHECK_EQ_U64(pentad.first + pentad.helion - 1u, sync.helion);
    CHECK(frc_sync_by_number(CAT, sync.sync, &sync) == FRC_OK && sync.helion == 0);
    /* "Phase 0 is the sync's start" */
    CHECK_EQ_I64(frc_time_at_sync_phase(CAT, sync.sync, 0), sync.start);
    CHECK_EQ_I64(frc_time_at_sync_phase(CAT, INT32_MAX, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_sync_phase(NULL, 0, 0), FRC_TIME_NONE);
    {
        /* "and a sync whose nominal start, drift or length times P(6) does not
         * fit 64 bits (FRC_ERR_OVERFLOW ...), even where the instant itself would fit" */
        static const int64_t WIDE_MS[2] FRC_PROGMEM = {-(INT64_MAX / 1000), INT64_MAX / 1000};
        frc_catalogue_t wide;
        CHECK(frc_catalogue_init(&wide, WIDE_MS, 2, 0) == FRC_OK);
        CHECK(frc_sync_by_number(&wide, 0, &sync) == FRC_ERR_OVERFLOW);
        CHECK_EQ_I64(frc_time_at_sync_phase(&wide, 0, 0), FRC_TIME_NONE);
    }
    {
        frc_sync_t at;
        CHECK(frc_sync_at(CAT, INSIDE, &at) == FRC_OK);
        /* "FRC_FRAC_QUARTER the end of its first quarter" */
        CHECK(frc_sync_at(CAT, at.start, &sync) == FRC_OK);
        CHECK_EQ_I64(frc_time_at_sync_phase(CAT, at.sync, FRC_FRAC_QUARTER), sync.next.time);
    }
    {
        uint8_t i;
        for (i = 0; i < 4; i++) {
            CHECK(frc_pentad_by_number(CAT, 3, i, &pentad) == FRC_OK && pentad.first == 5u * i); /* 0 5 10 15 */
        }
    }

    TEST_SECTION("frc_format_date_term");
    CHECK(frc_date_at(CAT, INSIDE, &date) == FRC_OK);
    CHECK(frc_format_date_term(&date, FRC_UNIT_ALPHA, text, sizeof text) > 0);
    CHECK_STR(text, "α#6");
    CHECK(frc_format_date_term(&date, FRC_UNIT_NUCLIDE, text, sizeof text) > 0);
    CHECK_STR(text, "ε#5");
    CHECK(frc_format_date_term(&date, FRC_UNIT_HELION, text, sizeof text) > 0);
    CHECK_STR(text, "34η");
    CHECK(frc_format_date_term(&date, FRC_UNIT_PREON, text, sizeof text) > 0);
    CHECK_STR(text, "29ι");
    CHECK(frc_format_date_term(&date, FRC_UNIT_BLINK, text, sizeof text) > 0);
    CHECK_STR(text, "3ν");
    CHECK(frc_format_date_term(&date, FRC_UNIT_COUNT, text, sizeof text) == FRC_ERR_ARG && text[0] == '\0');
    CHECK(frc_format_date_term(NULL, FRC_UNIT_BLINK, text, sizeof text) == FRC_ERR_ARG);

    TEST_SECTION("frc_format_span_signed, frc_format_span_in_unit");
    span.negative = 0; span.blinks = 6 * 1625 + 43 * 25; span.frac = 0;
    CHECK(frc_format_span_signed(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "+6ι 43λ");
    span.negative = 1; span.blinks = 20 * 1625 + 21 * 25;
    CHECK(frc_format_span_signed(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "−20ι 21λ");
    span.blinks = 0; span.frac = 0;
    CHECK(frc_format_span_signed(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "0ν");
    span.frac = 1;
    CHECK(frc_format_span_signed(&span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "<0.001ν");
    CHECK(frc_format_span_signed(NULL, 0, text, sizeof text) == FRC_ERR_ARG);
    /* "A drift of -6 236.9 Flavons is "−6 236λ"; 24.9 Blinks in Flavons "0λ"" */
    span.negative = 1; span.blinks = 6236 * 25 + 22; span.frac = 0xE6666666u;
    CHECK(frc_format_span_in_unit(&span, FRC_UNIT_FLAVON, text, sizeof text) > 0);
    CHECK_STR(text, "−6 236λ");
    span.negative = 0; span.blinks = 24; span.frac = 0xE6666666u;
    CHECK(frc_format_span_in_unit(&span, FRC_UNIT_FLAVON, text, sizeof text) > 0);
    CHECK_STR(text, "0λ");
    CHECK(frc_format_span_in_unit(&span, FRC_UNIT_COUNT, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_span_in_unit(NULL, FRC_UNIT_BLINK, text, sizeof text) == FRC_ERR_ARG);

    TEST_SECTION("frc_span_next_change");
    {
        frc_threshold_t next;
        frc_time_t change;
        char before[64], after[64];
        CHECK(frc_next_threshold(CAT, INSIDE, 6, &next) == FRC_OK);
        change = frc_span_next_change(CAT, INSIDE, next.time, 0);
        CHECK(change > INSIDE && change <= next.time);
        CHECK(frc_span_between(CAT, change - 1, next.time, &span) == FRC_OK);
        frc_format_span(&span, 0, before, sizeof before);
        CHECK(frc_span_between(CAT, INSIDE, next.time, &span) == FRC_OK);
        frc_format_span(&span, 0, after, sizeof after);
        CHECK_STR(before, after); /* the text at t holds to the microsecond before */
        CHECK(frc_span_between(CAT, change, next.time, &span) == FRC_OK);
        frc_format_span(&span, 0, after, sizeof after);
        CHECK(strcmp(before, after) != 0);
        /* "from "<0.001ν" it is boundary"; "FRC_TIME_NONE when t >= boundary" */
        CHECK_EQ_I64(frc_span_next_change(CAT, next.time - 1, next.time, 0), next.time);
        CHECK_EQ_I64(frc_span_next_change(CAT, next.time, next.time, 0), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change(CAT, next.time + 1, next.time, 0), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change(NULL, INSIDE, next.time, 0), FRC_TIME_NONE);
        /* "max_parts above 9 is 9" */
        CHECK_EQ_I64(frc_span_next_change(CAT, INSIDE, next.time, 200), frc_span_next_change(CAT, INSIDE, next.time, 9));
    }
    {
        /* "NONE ... where frc_span_between answers FRC_ERR_OVERFLOW" */
        static const int64_t WIDE_MS[2] FRC_PROGMEM = {-(INT64_MAX / 1000), INT64_MAX / 1000};
        frc_catalogue_t wide;
        CHECK(frc_catalogue_init(&wide, WIDE_MS, 2, 0) == FRC_OK);
        CHECK(frc_span_between(&wide, 0, 1000, &span) == FRC_ERR_OVERFLOW);
        CHECK_EQ_I64(frc_span_next_change(&wide, 0, 1000, 2), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change_since(&wide, 0, 1000, 2), FRC_TIME_NONE);
    }

    TEST_SECTION("frc_span_next_change_since");
    {
        frc_time_t change;
        char before[64], after[64];
        /* "At t == origin the text is "0ν" and the answer is t + 1" */
        CHECK_EQ_I64(frc_span_next_change_since(CAT, INSIDE, INSIDE, 0), INSIDE + 1);
        /* ""<0.001ν", which lasts until 1/1000 Blink; then "0.001ν"" (311 us at vector A) */
        change = frc_span_next_change_since(CAT, INSIDE, INSIDE + 1, 0);
        CHECK_EQ_I64(change, INSIDE + 311);
        CHECK(frc_span_between(CAT, INSIDE, change, &span) == FRC_OK);
        frc_format_span(&span, 0, after, sizeof after);
        CHECK_STR(after, "0.001ν");
        /* a signed span past its zero: the same instants, with a minus */
        change = frc_span_next_change_since(CAT, INSIDE, INSIDE + 5000000, 0);
        CHECK(change > INSIDE + 5000000);
        CHECK(frc_span_between(CAT, change - 1, INSIDE, &span) == FRC_OK);
        frc_format_span_signed(&span, 0, before, sizeof before);
        CHECK(frc_span_between(CAT, INSIDE + 5000000, INSIDE, &span) == FRC_OK);
        frc_format_span_signed(&span, 0, after, sizeof after);
        CHECK_STR(before, after);
        CHECK(strncmp(after, "−", 3) == 0);
        CHECK(frc_span_between(CAT, change, INSIDE, &span) == FRC_OK);
        frc_format_span_signed(&span, 0, after, sizeof after);
        CHECK(strcmp(before, after) != 0);
        /* "FRC_TIME_NONE when t < origin", at the closing instant, NULL, outside */
        CHECK_EQ_I64(frc_span_next_change_since(CAT, INSIDE, INSIDE - 1, 0), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change_since(CAT, INSIDE, frc_catalogue_last(CAT), 0), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change_since(NULL, INSIDE, INSIDE, 0), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change_since(CAT, INSIDE, frc_catalogue_last(CAT) + 1, 0), FRC_TIME_NONE);
        /* "when the text at the closing instant is still the text at t" */
        CHECK_EQ_I64(frc_span_next_change_since(CAT, frc_catalogue_first(CAT), frc_catalogue_last(CAT) - 1, 1),
                     FRC_TIME_NONE);
    }

    TEST_SECTION("pure wording: light word, kinds, signed counts, places, children, parents, factors");
    CHECK(frc_format_light_word(text, sizeof text) == 5);
    CHECK_STR(text, "light");
    CHECK(frc_format_level_kind(FRC_KIND_MAJOR, text, sizeof text) > 0);
    CHECK_STR(text, "major");
    CHECK(frc_format_level_kind(FRC_KIND_UNIT, text, sizeof text) > 0);
    CHECK_STR(text, "unit");
    CHECK(frc_format_level_kind(FRC_KIND_BETWEEN, text, sizeof text) > 0);
    CHECK_STR(text, "between");
    CHECK(frc_format_level_kind(FRC_KIND_SUB, text, sizeof text) == 9);
    CHECK_STR(text, "sub-Blink");
    CHECK(frc_format_level_kind((frc_level_kind_t)4, text, sizeof text) == FRC_ERR_ARG && text[0] == '\0');
    CHECK(frc_format_count_signed(4820, text, sizeof text) > 0);
    CHECK_STR(text, "4 820");
    CHECK(frc_format_count_signed(-3, text, sizeof text) > 0);
    CHECK_STR(text, "−3");
    CHECK(frc_format_count_signed(0, text, sizeof text) > 0);
    CHECK_STR(text, "0");
    CHECK(frc_format_count_signed(INT64_MIN, text, sizeof text) == 40);
    CHECK_STR(text, "−9 223 372 036 854 775 808");
    cycle.local = 7; cycle.siblings = 9;
    CHECK(frc_format_cycle_place(&cycle, text, sizeof text) > 0);
    CHECK_STR(text, "8 of 9");
    cycle.local = 9;
    CHECK(frc_format_cycle_place(&cycle, text, sizeof text) == FRC_ERR_ARG);
    cycle.siblings = 0;
    CHECK(frc_format_cycle_place(&cycle, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_cycle_place(NULL, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_children(6, text, sizeof text) > 0);
    CHECK_STR(text, "9 θ");
    CHECK(frc_format_level_children(0, text, sizeof text) > 0);
    CHECK_STR(text, "5 β");
    CHECK(frc_format_level_children(29, text, sizeof text) > 0);
    CHECK_STR(text, "11 η1");
    CHECK(frc_format_level_children(57, text, sizeof text) == 6);
    CHECK_STR(text, "13 λ2");
    CHECK(frc_format_level_children(62, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_in_parent(6, text, sizeof text) > 0);
    CHECK_STR(text, "11 in ζ");
    CHECK(frc_format_level_in_parent(1, text, sizeof text) > 0);
    CHECK_STR(text, "5 in α");
    CHECK(frc_format_level_in_parent(58, text, sizeof text) == 9);
    CHECK_STR(text, "13 in κ2");
    CHECK(frc_format_level_in_parent(0, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_level_in_parent(63, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_unit_in_parent(FRC_UNIT_HELION, text, sizeof text) > 0);
    CHECK_STR(text, "77 in ε");
    CHECK(frc_format_unit_in_parent(FRC_UNIT_BLINK, text, sizeof text) > 0);
    CHECK_STR(text, "25 in λ");
    CHECK(frc_format_unit_in_parent(FRC_UNIT_BETA, text, sizeof text) > 0);
    CHECK_STR(text, "5 in α");
    CHECK(frc_format_unit_in_parent(FRC_UNIT_ALPHA, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_unit_in_parent(FRC_UNIT_COUNT, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_format_prefix_factors(6, text, sizeof text) > 0);
    CHECK_STR(text, "125 125 = 5 · 13 · 5 · 5 · 7 · 11");
    CHECK(frc_format_prefix_factors(1, text, sizeof text) > 0);
    CHECK_STR(text, "5 = 5");
    CHECK(frc_format_prefix_factors(0, text, sizeof text) > 0);
    CHECK_STR(text, "1");
    CHECK(frc_format_prefix_factors(62, text, sizeof text) == 428);
    CHECK(frc_format_prefix_factors(63, text, sizeof text) == FRC_ERR_ARG);
    {
        /* "FRC_PREFIX_FACTORS_SIZE"; "up to level 9 they fit 64 bytes with the NUL,
         * up to level 14 96 bytes, up to level 18 128 bytes, up to level 37 256" */
        char big[FRC_PREFIX_FACTORS_SIZE];
        static const struct { uint8_t last; size_t size; } fit[4] = {{9, 64}, {14, 96}, {18, 128}, {37, 256}};
        int k;
        CHECK(sizeof big == 429);
        CHECK(frc_format_prefix_factors(62, big, sizeof big) == 428);
        CHECK(frc_format_prefix_factors(62, big, sizeof big - 1) == FRC_ERR_BUFFER);
        for (k = 0; k < 4; k++) {
            CHECK(frc_format_prefix_factors(fit[k].last, big, fit[k].size) > 0);
            CHECK(frc_format_prefix_factors((uint8_t)(fit[k].last + 1), big, fit[k].size) == FRC_ERR_BUFFER);
        }
    }

    TEST_SECTION("A1, A2, A3: the changed texts");
    CHECK(frc_format_level_rate(10, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1 per 25 Blinks");
    CHECK(frc_format_level_rate(11, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1 per 5 Blinks");
    CHECK(frc_format_per_blink(1, 2, text, sizeof text) > 0);
    CHECK_STR(text, "1 per 2 Blinks");
    CHECK(frc_format_band(FRC_BAND_NONE, text, sizeof text) > 0);
    CHECK_STR(text, "no band");
    {
        /* "At S12 vector A ... 1, 20, 244 and 310 us read "<0.001ν", 311 us "0.001ν", 400 us "0.0013ν"" */
        static const struct { int64_t us; const char *text; } cut[6] = {
            {1, "<0.001ν"}, {20, "<0.001ν"}, {244, "<0.001ν"}, {310, "<0.001ν"}, {311, "0.001ν"}, {400, "0.0013ν"}};
        int i;
        for (i = 0; i < 6; i++) {
            CHECK(frc_span_between(CAT, INSIDE, INSIDE + cut[i].us, &span) == FRC_OK);
            CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
            CHECK_STR(text, cut[i].text);
        }
        /* "never "−0ν", never "−<0.001ν"; "−0.0013ν"" */
        CHECK(frc_span_between(CAT, INSIDE + 244, INSIDE, &span) == FRC_OK);
        CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
        CHECK_STR(text, "<0.001ν");
        CHECK(frc_span_between(CAT, INSIDE + 400, INSIDE, &span) == FRC_OK);
        CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
        CHECK_STR(text, "−0.0013ν");
    }

    TEST_SECTION("frc_band_edges");
    CHECK(frc_band_edges(FRC_BAND_AUDIBLE, &low, &high) == FRC_OK);
    CHECK(low.mantissa == UINT64_C(20000000000000000) && low.exp10 == -15);
    CHECK(frc_format_quantity(&high, FRC_QUANTITY_FREQUENCY, 1, text, sizeof text) > 0);
    CHECK_STR(text, "20 kHz");
    CHECK(frc_band_edges(FRC_BAND_GAMMA, &low, &high) == FRC_OK);
    CHECK(frc_format_quantity(&high, FRC_QUANTITY_FREQUENCY, 2, text, sizeof text) > 0);
    CHECK_STR(text, "3.4 × 10²⁹ Hz");
    CHECK(frc_band_edges(FRC_BAND_NONE, &low, &high) == FRC_ERR_ARG);

    TEST_SECTION("Earth wording");
    civil.year = 2026; civil.month = 10; civil.day = 8; civil.hour = 12; civil.minute = 0; civil.second = 0;
    civil.microsecond = 0; civil.weekday = 0;
    CHECK(frc_format_civil_text(&civil, 0, FRC_CIVIL_DATE_TIME, text, sizeof text) > 0);
    CHECK_STR(text, "THU 8 OCT 2026 · 12:00:00");
    CHECK(frc_format_civil_text(NULL, -9000, FRC_CIVIL_OFFSET, text, sizeof text) > 0);
    CHECK_STR(text, "UTC−2:30");
    /* "an offset under a day either way ... at most "UTC+23:59"" */
    CHECK(frc_format_civil_text(NULL, 86399, FRC_CIVIL_OFFSET, text, sizeof text) > 0);
    CHECK_STR(text, "UTC+23:59");
    CHECK(frc_format_civil_text(NULL, 86400, FRC_CIVIL_OFFSET, text, sizeof text) == FRC_ERR_ARG && text[0] == '\0');
    CHECK(frc_format_civil_text(NULL, -86400, FRC_CIVIL_OFFSET, text, sizeof text) == FRC_ERR_ARG);
    /* "The minus is U+2212 ... in every form but FRC_CIVIL_ISO_DATE" */
    civil.year = -44; civil.month = 3; civil.day = 15;
    CHECK(frc_format_civil_text(&civil, 0, FRC_CIVIL_DATE, text, sizeof text) > 0);
    CHECK_STR(text, "THU 15 MAR −0044");
    CHECK(frc_format_civil_text(&civil, 0, FRC_CIVIL_ISO_DATE, text, sizeof text) > 0);
    CHECK_STR(text, "-0044-03-15");
    civil.year = 0;
    CHECK(frc_format_civil_text(&civil, 0, FRC_CIVIL_YEAR, text, sizeof text) > 0);
    CHECK_STR(text, "0000");
    civil.year = 2026; civil.month = 10; civil.day = 8;
    CHECK(frc_civil_validate(&civil) == FRC_OK);
    civil.month = 2; civil.day = 31;
    CHECK(frc_civil_validate(&civil) == FRC_ERR_RANGE);
    CHECK(frc_format_civil_text(&civil, 0, FRC_CIVIL_DATE, text, sizeof text) == FRC_ERR_ARG);
    CHECK(frc_civil_validate(NULL) == FRC_ERR_ARG);
    CHECK(frc_format_duration(INT64_C(1433000000), 0, text, sizeof text) > 0);
    CHECK_STR(text, "23 min 53 s");
    CHECK(frc_format_duration(59600000, 0, text, sizeof text) > 0);
    CHECK_STR(text, "1 min");
    CHECK(frc_format_duration(400000, 0, text, sizeof text) > 0);
    CHECK_STR(text, "0.4 s");
    /* "−5 000 us reads "−0.01 s", −9.995 s "−10 s"" */
    CHECK(frc_format_duration(-5000, 0, text, sizeof text) > 0);
    CHECK_STR(text, "−0.01 s");
    CHECK(frc_format_duration(-9995000, 0, text, sizeof text) > 0);
    CHECK_STR(text, "−10 s");
    CHECK(frc_parse_civil("2026-10-08T14:00:00+02:00", &civil, &offset, &has) == FRC_OK);
    CHECK(civil.hour == 14 && offset == 7200 && has == 1 && civil.weekday == 3);
    CHECK(frc_parse_civil("31 FEB 2026", &civil, &offset, &has) == FRC_ERR_RANGE);
    CHECK(frc_parse_civil("FRI 8 OCT 2026", &civil, &offset, &has) == FRC_ERR_PARSE);
    CHECK(frc_parse_civil(NULL, &civil, &offset, &has) == FRC_ERR_ARG);
    /* "a tab, a newline or a no-break space is refused" */
    CHECK(frc_parse_civil("2026-10-08\t12:00", &civil, &offset, &has) == FRC_ERR_PARSE);
    CHECK(frc_parse_civil("2026-10-08\xC2\xA0" "12:00", &civil, &offset, &has) == FRC_ERR_PARSE);
    /* "spaces, a ',' or both ...; "THU8" is refused" */
    CHECK(frc_parse_civil("THU , 8 OCT 2026", &civil, &offset, &has) == FRC_OK);
    CHECK(frc_parse_civil("thu, 2026-10-08", &civil, &offset, &has) == FRC_OK);
    CHECK(frc_parse_civil("THU8 OCT 2026", &civil, &offset, &has) == FRC_ERR_PARSE);
    /* "'T' at once, after an ISO date only"; ""12:00.5" is refused" */
    CHECK(frc_parse_civil("8 OCT 2026T12:00", &civil, &offset, &has) == FRC_ERR_PARSE);
    CHECK(frc_parse_civil("2026-10-08 12:00.5", &civil, &offset, &has) == FRC_ERR_PARSE);
    /* "three digits ("+230") are refused"; ""-00:00" is an offset of 0 that the text names" */
    CHECK(frc_parse_civil("2026-10-08 12:00 +230", &civil, &offset, &has) == FRC_ERR_PARSE);
    CHECK(frc_parse_civil("2026-10-08 12:00 +2", &civil, &offset, &has) == FRC_OK && offset == 7200);
    CHECK(frc_parse_civil("2026-10-08 12:00 -00:00", &civil, &offset, &has) == FRC_OK && offset == 0 && has == 1);
    CHECK(frc_parse_civil("2026-10-08 12:00 EST", &civil, &offset, &has) == FRC_ERR_PARSE);
    CHECK(frc_parse_civil("2026-10-08 12:00 UTC +3", &civil, &offset, &has) == FRC_ERR_PARSE);
    /* "The shape is checked first, then the range, then the weekday" */
    CHECK(frc_parse_civil("FRI 8 OCT 2026 24:00", &civil, &offset, &has) == FRC_ERR_RANGE);
    CHECK(frc_parse_civil("SAT 29 FEB 2024", &civil, &offset, &has) == FRC_ERR_PARSE);
    /* "a year outside -290000..290000" */
    CHECK(frc_parse_civil("290001-01-01", &civil, &offset, &has) == FRC_ERR_RANGE);
    /* "frc_parse_civil reads every text written for a year in -290000..290000" */
    civil.year = -290000; civil.month = 1; civil.day = 1; civil.hour = 0; civil.minute = 0; civil.second = 0;
    civil.microsecond = 0;
    CHECK(frc_format_civil(&civil, 0, text, sizeof text) > 0);
    CHECK(frc_parse_civil(text, &civil, &offset, &has) == FRC_OK && civil.year == -290000);
    /* "frc_civil_from_time of an instant within a few thousand years of the
     * ends of frc_time_t gives such a year, and the Earth wordings refuse it" */
    frc_civil_from_time(INT64_MAX, 0, &civil);
    CHECK(civil.year == 294247 && frc_civil_validate(&civil) == FRC_ERR_RANGE);
    CHECK(frc_time_from_civil(&civil, 0) == FRC_TIME_NONE);
}

int main(void)
{
    test_catalogue();
    test_hierarchy_and_cycles();
    test_glyphs_and_odometers();
    test_sync_and_pentads();
    test_dates();
    test_names_spans_rates();
    test_pure_wording();
    test_ladder();
#ifndef FRC_NO_FLOAT
    test_arcs();
#endif
    test_civil_and_text();
    test_requests();
    return TEST_SUMMARY("header");
}
