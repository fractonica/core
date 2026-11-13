/*
 * test_properties.c - what must hold at EVERY instant, checked at 20,000
 * pseudo-random ones (fixed seed) and at every eclipse, on two catalogues.
 *
 * The other tests hold the core against vectors. This one holds it against
 * its own promises (fractonica.h), through the public API only:
 *
 *   next instants   frc_odometer_at().next_change, frc_next_threshold().time,
 *                   frc_sync_at().next.time and frc_sync_odometer().next_change
 *                   are the FIRST microsecond with a new reading: one
 *                   microsecond earlier the reading is still the old one
 *   tiling          cycles of every level, syncs and Pentads cover the
 *                   catalogue without gap or overlap (end of c == start of c+1)
 *   round trips     position -> time, date -> time -> date, and
 *                   frc_parse_date(frc_format_date()) in every form, also
 *                   after frc_to_ascii and behind a Pentad
 *   monotony        phases never run backwards inside a cycle or a sync
 *
 * Catalogues: Saros 141, and a short hand-made series with uneven intervals,
 * whose anchor (12 of 13) starts no full Alpha and whose last cycle of most
 * levels is clipped.
 *
 * A failure prints the instant, the level and the catalogue, so it can be
 * replayed as a one-line test.
 */
#include "frc_test.h"
#include "fractonica.h"

#define RANDOM_INSTANTS 20000
#define SEED UINT64_C(0x141F2AC7041CA)

/* ------------------------------------------------------------------------- */
/* Catalogues and random numbers                                              */
/* ------------------------------------------------------------------------- */

/* 14 eclipses, 1811 .. 2045, intervals of a Saros give or take 1.5 h. */
static const int64_t UNEVEN_MS[14] FRC_PROGMEM = {
    INT64_C(-5000000000123), INT64_C(-4431023843919), INT64_C(-3862055860456), INT64_C(-3293080413967),
    INT64_C(-2724109961252), INT64_C(-2155132860291), INT64_C(-1586157627063), INT64_C(-1017190122672),
    INT64_C(-448218133664),  INT64_C(120758759227),   INT64_C(689726446901),   INT64_C(1258699675910),
    INT64_C(1827674050932),  INT64_C(2396645532997)};

static frc_catalogue_t uneven;

static const frc_catalogue_t *cat;      /* the catalogue under test            */
static const char *cat_name;
static frc_time_t first, last;

/* splitmix64: small, good enough, and the same on every platform. */
static uint64_t random_state;
static uint64_t random_u64(void)
{
    uint64_t z = (random_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* A whole number in lo..hi (the tiny modulo bias does not matter here). */
static int64_t random_between(int64_t lo, int64_t hi)
{
    return lo + (int64_t)(random_u64() % ((uint64_t)(hi - lo) + 1u));
}

/* Printed after a failed check: enough to replay it. */
#define NOTE_AT(t, level) \
    CHECK_NOTE("%s, t = %" PRId64 " us, level %d", cat_name, (int64_t)(t), (int)(level))

/* ------------------------------------------------------------------------- */
/* Small comparisons                                                          */
/* ------------------------------------------------------------------------- */

static int same_digits(const frc_odometer_t *a, const frc_odometer_t *b)
{
    int i;
    if (a->count != b->count || a->strokes != b->strokes || a->states != b->states) {
        return 0;
    }
    for (i = 0; i < FRC_MAX_DIGITS; i++) {
        if (a->digit[i] != b->digit[i]) {
            return 0;
        }
    }
    return 1;
}

static int same_date(const frc_date_t *a, const frc_date_t *b, int with_quarters)
{
    int i;
    if (a->alpha != b->alpha || a->beta != b->beta || a->gamma != b->gamma || a->delta != b->delta ||
        a->epsilon != b->epsilon || a->eta != b->eta || a->iota != b->iota || a->lambda != b->lambda ||
        a->nu != b->nu) {
        return 0;
    }
    for (i = 0; with_quarters && i < 4; i++) {
        if (a->quarter[i] != b->quarter[i]) {
            return 0;
        }
    }
    return 1;
}

/* A cycle shorter than a microsecond (levels 19..21: 345, 69 and 10 ns)
 * cannot be told from its neighbours at the microsecond grain. */
static int longer_than_a_microsecond(uint8_t level)
{
    return level <= 18;
}

/* The same for a QUARTER of a cycle: at level 18 a cycle is 1.726 us, so its
 * quarters (0.43 us) already fall between two microseconds. */
static int quarter_longer_than_a_microsecond(uint8_t level)
{
    return level <= 17;
}

/* ------------------------------------------------------------------------- */
/* Position                                                                   */
/* ------------------------------------------------------------------------- */

static void check_position(frc_time_t t)
{
    frc_position_t p;

    CHECK(frc_catalogue_contains(cat, t));
    CHECK(frc_position_at(cat, t, &p) == FRC_OK);
    CHECK(p.den != 0 && p.num < p.den && p.ordinal < cat->count - 1);
    CHECK(frc_catalogue_instant(cat, p.ordinal) <= t);
    CHECK(t <= frc_catalogue_instant(cat, (uint16_t)(p.ordinal + 1)));
    if (t != last) {
        /* a microsecond instant is an exact position: the way back is exact
         * (ordinal * den + num < 2^16 * 2^50) */
        CHECK_EQ_I64(frc_time_at_position(cat, (uint64_t)p.ordinal * p.den + p.num, p.den), t);
        NOTE_AT(t, -1);
    } else {
        /* the closing instant reads one microsecond early, and is position count-1 */
        CHECK(p.ordinal == cat->count - 2 && p.num == p.den - 1);
        CHECK_EQ_I64(frc_time_at_position(cat, (uint64_t)cat->count - 1, 1), last);
    }
}

/* ------------------------------------------------------------------------- */
/* Cycles: where an instant sits, and the tiling around it                    */
/* ------------------------------------------------------------------------- */

static void check_cycle(frc_time_t t, uint8_t level)
{
    /* how far frc_cycle_at(start) may run ahead of the cycle at levels 19..21,
     * as fractonica.h states it (frc_cycle_t) */
    static const uint8_t slack[3] = {3, 15, 102};
    frc_cycle_t c, same, next, prev, at_start;
    frc_time_t points[5];
    uint64_t count = frc_cycle_count(cat, level);
    int q;

    CHECK(count != 0);
    CHECK(frc_cycle_at(cat, t, level, &c) == FRC_OK);
    CHECK_EQ_U64(c.level, level);
    CHECK(c.cycle < count);
    CHECK(c.start <= t);
    CHECK(t == last ? c.end == last : t < c.end);
    CHECK(c.quarter <= FRC_END && c.quarter == (c.phase >> 30));
    CHECK(c.clipped == 0 || c.clipped == 1);
    CHECK(!c.clipped || (c.cycle == count - 1 && c.end == last));
    if (level == 0) {
        CHECK(c.local == c.cycle && c.siblings == count);
    } else {
        CHECK(c.siblings == frc_siblings(level) && c.local == c.cycle % c.siblings);
    }
    NOTE_AT(t, level);

    /* the same cycle asked for by its index */
    CHECK(frc_cycle_by_index(cat, level, c.cycle, &same) == FRC_OK);
    CHECK(same.start == c.start && same.end == c.end && same.clipped == c.clipped &&
          same.local == c.local && same.siblings == c.siblings && same.phase == 0 && same.quarter == 0);
    NOTE_AT(t, level);

    /* tiling: the neighbours touch it, and nothing lies beyond the last cycle */
    if (c.cycle + 1 < count) {
        CHECK(frc_cycle_by_index(cat, level, c.cycle + 1, &next) == FRC_OK);
        CHECK_EQ_I64(next.start, c.end);
        CHECK(!c.clipped);
    } else {
        CHECK(frc_cycle_by_index(cat, level, c.cycle + 1, &next) == FRC_ERR_RANGE);
        CHECK_EQ_I64(c.end, last);
    }
    NOTE_AT(t, level);
    if (c.cycle > 0) {
        CHECK(frc_cycle_by_index(cat, level, c.cycle - 1, &prev) == FRC_OK);
        CHECK_EQ_I64(prev.end, c.start);
        CHECK(prev.start <= prev.end);
    } else {
        CHECK_EQ_I64(c.start, first);
    }
    NOTE_AT(t, level);

    /* the five quarter points: start, peak, node, valley, end */
    CHECK(frc_cycle_points(cat, level, c.cycle, points) == FRC_OK);
    CHECK_EQ_I64(points[0], c.start);
    if (!c.clipped) {
        CHECK_EQ_I64(points[4], c.end);
    }
    for (q = 0; q < 4; q++) {
        /* a point past the catalogue is FRC_TIME_NONE, and so are all after it */
        CHECK(points[q + 1] == FRC_TIME_NONE || (points[q] != FRC_TIME_NONE && points[q] <= points[q + 1]));
    }
    if (t != last) {
        CHECK(points[c.quarter] <= t);
        CHECK(points[c.quarter + 1] == FRC_TIME_NONE || t < points[c.quarter + 1]);
    }
    NOTE_AT(t, level);

    /* A cycle that holds a whole microsecond (this one holds t) reads as
     * itself at its first microsecond, and the microsecond before belongs to
     * an earlier cycle. */
    CHECK(frc_cycle_at(cat, c.start, level, &at_start) == FRC_OK);
    CHECK_EQ_U64(at_start.cycle, c.cycle);
    CHECK(c.start < c.end || t == last);
    if (c.start > first) {
        CHECK(frc_cycle_at(cat, c.start - 1, level, &prev) == FRC_OK);
        CHECK(prev.cycle < c.cycle);
        if (longer_than_a_microsecond(level)) {
            CHECK_EQ_U64(prev.cycle, c.cycle - 1);
        }
    }
    NOTE_AT(t, level);

    /* Levels 19..21: most cycles hold NO whole microsecond. Such a cycle,
     * asked for by index, has start == end, and its "first microsecond"
     * already belongs to a later cycle: later by at most 3 / 15 / 102. */
    if (!longer_than_a_microsecond(level)) {
        uint64_t index = c.cycle + (uint64_t)random_between(1, 300);
        if (index < count && frc_cycle_by_index(cat, level, index, &next) == FRC_OK && next.start != last) {
            CHECK(next.start <= next.end && next.end - next.start <= 1);
            CHECK(frc_cycle_at(cat, next.start, level, &at_start) == FRC_OK);
            CHECK(at_start.cycle >= index && at_start.cycle - index <= slack[level - 19]);
            CHECK_NOTE("%s, level %d, cycle %" PRIu64 " reads as %" PRIu64 " at its start", cat_name, level, index,
                       at_start.cycle);
        }
    }
    /* and the closing instant, read one microsecond early, shows a cycle at
     * most that many before the last (the very last one up to level 18) */
    if (t == last) {
        CHECK(count - 1 - c.cycle <= (longer_than_a_microsecond(level) ? 0u : slack[level - 19]));
        NOTE_AT(t, level);
    }
}

/* Every cycle of a coarse level, first to last: a chain without gap or
 * overlap from the first instant of the catalogue to its last. */
static void check_tiling_exhaustive(uint8_t level)
{
    uint64_t count = frc_cycle_count(cat, level);
    frc_cycle_t c;
    frc_time_t expected_start = first;
    uint64_t i;

    for (i = 0; i < count; i++) {
        if (frc_cycle_by_index(cat, level, i, &c) != FRC_OK) {
            break;
        }
        if (c.start != expected_start || c.start >= c.end || c.cycle != i || (c.clipped && i != count - 1)) {
            break;
        }
        expected_start = c.end;
    }
    CHECK_EQ_U64(i, count); /* the index where the chain broke, if it did */
    CHECK_NOTE("%s, level %d", cat_name, level);
    CHECK_EQ_I64(expected_start, last);
    CHECK(frc_cycle_by_index(cat, level, count, &c) == FRC_ERR_RANGE);
}

/* ------------------------------------------------------------------------- */
/* Next instants: thresholds and odometers                                    */
/* ------------------------------------------------------------------------- */

static void check_threshold(frc_time_t t, uint8_t level)
{
    frc_threshold_t next;
    frc_cycle_t now, before, after;

    CHECK(frc_next_threshold(cat, t, level, &next) == FRC_OK);
    CHECK(frc_cycle_at(cat, t, level, &now) == FRC_OK);
    CHECK_EQ_U64(next.name, now.quarter); /* a quarter is named by the threshold that ends it */
    if (t == last) {
        CHECK_EQ_I64(next.time, FRC_TIME_NONE);
        return;
    }
    /* Every eclipse is a quarter boundary of every level (position * P is a
     * whole number there), so inside the catalogue there always is a next one. */
    CHECK(next.time != FRC_TIME_NONE && next.time > t && next.time <= last);
    NOTE_AT(t, level);
    if (next.time == FRC_TIME_NONE) {
        return;
    }
    /* one microsecond earlier: still this quarter of this cycle */
    CHECK(frc_cycle_at(cat, next.time - 1, level, &before) == FRC_OK);
    CHECK(before.cycle == now.cycle && before.quarter == now.quarter);
    NOTE_AT(t, level);
    /* at the instant: a later quarter (the closing instant reads early: skip it) */
    if (next.time != last) {
        CHECK(frc_cycle_at(cat, next.time, level, &after) == FRC_OK);
        CHECK(after.cycle > now.cycle || (after.cycle == now.cycle && after.quarter > now.quarter));
        if (quarter_longer_than_a_microsecond(level)) {
            /* exactly the next quarter, not a later one */
            CHECK(after.cycle * 4 + after.quarter == now.cycle * 4 + now.quarter + 1);
        }
        NOTE_AT(t, level);
    }
}

static void check_odometer(frc_time_t t, uint8_t level, uint8_t strokes, uint8_t digits)
{
    frc_odometer_t now, before, after;
    frc_glyph_t glyph;
    frc_cycle_t cycle_now, cycle_after;

    CHECK(frc_odometer_at(cat, t, level, strokes, digits, &now) == FRC_OK);
    CHECK(now.count == digits && now.strokes == strokes && now.states == frc_glyph_states(strokes));
    /* "digit[0] equals frc_glyph_at(...).index" */
    CHECK(frc_glyph_at(cat, t, level, strokes, &glyph) == FRC_OK);
    CHECK_EQ_U64(now.digit[0], glyph.index);
    CHECK(glyph.dotted == (glyph.derivative < 0));
    NOTE_AT(t, level);

    if (t == last) {
        CHECK_EQ_I64(now.next_change, FRC_TIME_NONE);
        return;
    }
    CHECK(now.next_change != FRC_TIME_NONE && now.next_change > t && now.next_change <= last);
    CHECK_NOTE("%s, t = %" PRId64 ", level %d, %d strokes, %d digits", cat_name, (int64_t)t, level, strokes, digits);
    if (now.next_change == FRC_TIME_NONE) {
        return;
    }
    /* one microsecond before the change: the same digits */
    CHECK(frc_odometer_at(cat, now.next_change - 1, level, strokes, digits, &before) == FRC_OK);
    CHECK(same_digits(&before, &now));
    CHECK_NOTE("%s, t = %" PRId64 ", level %d, %d strokes, %d digits", cat_name, (int64_t)t, level, strokes, digits);
    /* at the change: a new reading */
    if (now.next_change != last) {
        CHECK(frc_odometer_at(cat, now.next_change, level, strokes, digits, &after) == FRC_OK);
        CHECK(frc_cycle_at(cat, t, level, &cycle_now) == FRC_OK);
        CHECK(frc_cycle_at(cat, now.next_change, level, &cycle_after) == FRC_OK);
        /* the cycle index and the digits together never repeat ... */
        CHECK(cycle_after.cycle != cycle_now.cycle || !same_digits(&after, &now));
        /* ... and while a whole cycle outlasts a microsecond the digits alone differ */
        if (longer_than_a_microsecond(level)) {
            CHECK(!same_digits(&after, &now));
        }
        CHECK(after.next_change == FRC_TIME_NONE || after.next_change > now.next_change);
        CHECK_NOTE("%s, t = %" PRId64 ", level %d, %d strokes, %d digits", cat_name, (int64_t)t, level, strokes,
                   digits);
    }
}

/* ------------------------------------------------------------------------- */
/* Phases never run backwards                                                 */
/* ------------------------------------------------------------------------- */

static void check_phase_monotonic(frc_time_t t, uint8_t level)
{
    frc_cycle_t a, b, c;
    frc_time_t t2, t3;

    if (t == last) {
        return;
    }
    CHECK(frc_cycle_at(cat, t, level, &a) == FRC_OK);
    /* two later instants of the same cycle, in order */
    t2 = random_between(t, a.end - 1);
    t3 = random_between(t2, a.end - 1);
    CHECK(frc_cycle_at(cat, t2, level, &b) == FRC_OK);
    CHECK(frc_cycle_at(cat, t3, level, &c) == FRC_OK);
    CHECK(b.cycle == a.cycle && c.cycle == a.cycle);
    CHECK(a.phase <= b.phase && b.phase <= c.phase);
    CHECK(a.quarter <= b.quarter && b.quarter <= c.quarter);
    /* the glyph state is a step function of the phase, so it never steps back either */
    {
        frc_glyph_t g1, g2;
        CHECK(frc_glyph_at(cat, t, level, 3, &g1) == FRC_OK);
        CHECK(frc_glyph_at(cat, t3, level, 3, &g2) == FRC_OK);
        CHECK(g1.index <= g2.index);
        /* floor(phase * 4M) from the 32-bit phase is the state, or one short of
         * it right after a change (the phase field is itself a floor) */
        CHECK(frc_glyph_index_of_phase(a.phase, 3) == g1.index ||
              frc_glyph_index_of_phase(a.phase, 3) + 1 == g1.index);
    }
    NOTE_AT(t, level);
    /* the first microsecond of a cycle is less than one microsecond into it:
     * its phase is below 1 us / (the cycle's length, at least end - start - 1) */
    if (longer_than_a_microsecond(level)) {
        CHECK(frc_cycle_at(cat, a.start, level, &b) == FRC_OK);
        CHECK(b.cycle == a.cycle && b.phase <= a.phase);
        NOTE_AT(t, level);
        if (a.end - a.start > 1) {
            CHECK((uint64_t)b.phase * (uint64_t)(a.end - a.start - 1) < (UINT64_C(1) << 32));
            NOTE_AT(t, level);
        }
        if (quarter_longer_than_a_microsecond(level)) {
            CHECK_EQ_U64(b.quarter, 0);
            NOTE_AT(t, level);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Dates                                                                      */
/* ------------------------------------------------------------------------- */

static void check_date_round_trip(frc_time_t t)
{
    frc_date_t date, again, earlier;
    frc_cycle_t blink;
    frc_time_t start;

    CHECK(frc_date_at(cat, t, &date) == FRC_OK);
    CHECK(date.alpha >= 1 && date.alpha <= frc_cycle_count(cat, 0));
    CHECK(date.beta >= 1 && date.beta <= 5 && date.gamma >= 1 && date.gamma <= 13);
    CHECK(date.delta >= 1 && date.delta <= 5 && date.epsilon >= 1 && date.epsilon <= 5);
    CHECK(date.eta >= 1 && date.eta <= 77 && date.iota <= 35 && date.lambda <= 64 && date.nu <= 24);
    NOTE_AT(t, 12);

    /* the date names a Blink: its first microsecond, which is the start of
     * the level-12 cycle the instant is in */
    CHECK(frc_date_to_time(cat, &date, &start) == FRC_OK);
    CHECK(frc_cycle_at(cat, t, 12, &blink) == FRC_OK);
    CHECK_EQ_I64(start, blink.start);
    CHECK(start <= t && t - start < 312 * 1000);
    NOTE_AT(t, 12);

    /* and that instant reads as the same date, quarters included (the quarter
     * boundaries of the major periods fall on Blink boundaries) */
    CHECK(frc_date_at(cat, start, &again) == FRC_OK);
    CHECK(same_date(&again, &date, 1));
    NOTE_AT(t, 12);
    /* one microsecond earlier it is another Blink */
    if (start > first) {
        CHECK(frc_date_at(cat, start - 1, &earlier) == FRC_OK);
        CHECK(!same_date(&earlier, &date, 0));
        NOTE_AT(t, 12);
    }
    /* the major periods of the date are the local indices of S3.2, from 1 */
    {
        frc_cycle_t c;
        CHECK(frc_cycle_at(cat, t, 0, &c) == FRC_OK && c.cycle + 1 == date.alpha && c.quarter == date.quarter[0]);
        CHECK(frc_cycle_at(cat, t, 1, &c) == FRC_OK && c.local + 1 == date.beta && c.quarter == date.quarter[1]);
        CHECK(frc_cycle_at(cat, t, 2, &c) == FRC_OK && c.local + 1 == date.gamma && c.quarter == date.quarter[2]);
        CHECK(frc_cycle_at(cat, t, 3, &c) == FRC_OK && c.local + 1 == date.delta && c.quarter == date.quarter[3]);
        CHECK(frc_cycle_at(cat, t, 4, &c) == FRC_OK && c.local + 1 == date.epsilon);
        NOTE_AT(t, 12);
    }
}

/* What a date reads back as when it was written down to `finest` only:
 * the finer parts are "the start" (Helion 1, counts 0), the quarters 0. */
static frc_date_t truncated(const frc_date_t *date, frc_unit_t finest)
{
    frc_date_t d = *date;
    int i;
    if (finest < FRC_UNIT_NUCLIDE) { d.epsilon = 1; }
    if (finest < FRC_UNIT_HELION) { d.eta = 1; }
    if (finest < FRC_UNIT_PREON) { d.iota = 0; }
    if (finest < FRC_UNIT_FLAVON) { d.lambda = 0; }
    if (finest < FRC_UNIT_BLINK) { d.nu = 0; }
    for (i = 0; i < 4; i++) {
        d.quarter[i] = 0;
    }
    return d;
}

static void check_parse(const char *text, const frc_date_t *context, const frc_date_t *expected,
                        frc_unit_t expected_finest)
{
    frc_date_t parsed;
    frc_unit_t finest = FRC_UNIT_COUNT;
    frc_status_t status = frc_parse_date(text, context, &parsed, &finest);

    CHECK(status == FRC_OK);
    CHECK_NOTE("%s: \"%s\" gave status %d", cat_name, text, (int)status);
    if (status != FRC_OK) {
        return;
    }
    CHECK(same_date(&parsed, expected, 1));
    CHECK_NOTE("%s: \"%s\"", cat_name, text);
    CHECK(finest == expected_finest);
    CHECK_NOTE("%s: \"%s\" finest %d", cat_name, text, (int)finest);
}

static void check_text_round_trip(frc_time_t t)
{
    static const frc_date_form_t forms[4] = {FRC_DATE_FULL, FRC_DATE_SHORT, FRC_DATE_LETTERS,
                                             FRC_DATE_SHORT_LETTERS};
    frc_date_t date, expected;
    frc_pentad_t pentad;
    char text[200], ascii[200], led[280], lead[64];
    int f, finest, n;

    CHECK(frc_date_at(cat, t, &date) == FRC_OK);
    for (f = 0; f < 4; f++) {
        /* the short forms hang from a context: the date itself */
        const int whole = (forms[f] == FRC_DATE_FULL || forms[f] == FRC_DATE_LETTERS);
        const frc_date_t *context = whole ? NULL : &date;
        for (finest = FRC_UNIT_DELTA; finest <= FRC_UNIT_BLINK; finest++) {
            n = frc_format_date(&date, forms[f], (frc_unit_t)finest, text, sizeof text);
            CHECK(n > 0 && n < 160 && (size_t)n == strlen(text));
            expected = truncated(&date, (frc_unit_t)finest);
            check_parse(text, context, &expected, (frc_unit_t)finest);
            /* the same text for a display without Greek */
            n = frc_to_ascii(text, ascii, sizeof ascii);
            CHECK(n > 0 && (size_t)n == strlen(ascii));
            check_parse(ascii, context, &expected, (frc_unit_t)finest);
            /* a short form without a context has nothing to hang from */
            if (!whole) {
                frc_date_t unused;
                CHECK(frc_parse_date(text, NULL, &unused, NULL) == FRC_ERR_PARSE);
            }
        }
    }

    /* "A date leads with its Pentad": one random way of writing both */
    CHECK(frc_pentad_at(cat, t, &pentad) == FRC_OK);
    {
        const frc_pentad_form_t pentad_form = (frc_pentad_form_t)(random_u64() % 3);
        const int with_helion = (int)(random_u64() % 2);
        const char *const separators[4] = {" ", " · ", ", ", " . "};
        const char *separator = separators[random_u64() % 4];
        f = (int)(random_u64() % 4);
        finest = (int)random_between(FRC_UNIT_DELTA, FRC_UNIT_BLINK);
        CHECK(frc_format_pentad(&pentad, pentad_form, with_helion, lead, sizeof lead) > 0);
        CHECK(frc_format_date(&date, forms[f], (frc_unit_t)finest, text, sizeof text) > 0);
        snprintf(led, sizeof led, "%s%s%s", lead, separator, text);
        expected = truncated(&date, (frc_unit_t)finest);
        check_parse(led, &date, &expected, (frc_unit_t)finest);
        CHECK(frc_to_ascii(led, ascii, sizeof ascii) > 0);
        check_parse(ascii, &date, &expected, (frc_unit_t)finest);
    }
}

/* ------------------------------------------------------------------------- */
/* Solar sync and Pentads at an instant                                       */
/* ------------------------------------------------------------------------- */

static void check_sync(frc_time_t t)
{
    frc_sync_t s, same, other;
    frc_pentad_t p, same_pentad;
    frc_cycle_t helion;
    frc_odometer_t now, before, after;
    frc_status_t status;
    uint8_t strokes = (uint8_t)random_between(1, FRC_MAX_STROKES);
    uint8_t digits = (uint8_t)random_between(1, FRC_MAX_DIGITS);

    /* Pentads count whole Helions: defined everywhere in the catalogue */
    CHECK(frc_pentad_at(cat, t, &p) == FRC_OK);
    CHECK(p.index <= 3 && p.helions == (p.index == 3 ? 4 : 5) && p.helion >= 1 && p.helion <= p.helions);
    CHECK(p.start <= t && (t == last ? p.end == last : t < p.end));
    CHECK(frc_pentad_by_number(cat, p.sync, p.index, &same_pentad) == FRC_OK);
    CHECK(same_pentad.start == p.start && same_pentad.end == p.end && same_pentad.helion == 0);
    /* the Pentad is made of whole Helions and holds the running one */
    CHECK(frc_cycle_at(cat, t, FRC_SYNC_LEVEL, &helion) == FRC_OK);
    CHECK(p.start <= helion.start && helion.end <= p.end);
    NOTE_AT(t, FRC_SYNC_LEVEL);

    status = frc_sync_at(cat, t, &s);
    CHECK(status == FRC_OK || status == FRC_ERR_RANGE);
    if (status != FRC_OK) {
        /* only inside the catalogue's first or last partial sync */
        CHECK(frc_sync_by_number(cat, p.sync, &same) == FRC_ERR_RANGE);
        CHECK(frc_sync_odometer(cat, t, strokes, digits, &now) == FRC_ERR_RANGE);
        CHECK(p.start == first || p.end == last || frc_sync_by_number(cat, p.sync - 1, &same) == FRC_ERR_RANGE ||
              frc_sync_by_number(cat, p.sync + 1, &same) == FRC_ERR_RANGE);
        NOTE_AT(t, FRC_SYNC_LEVEL);
        return;
    }
    CHECK_EQ_I64(s.sync, p.sync);
    CHECK(s.start <= t && (t == last ? s.end == last : t < s.end));
    CHECK(s.start <= p.start && p.end <= s.end);
    CHECK(s.quarter == (s.phase >> 30) && s.next.name == s.quarter);
    CHECK_EQ_I64(s.drift_us, s.start - s.nominal);
    CHECK_EQ_I64(s.nominal, frc_catalogue_instant(cat, cat->anchor) + (int64_t)s.sync * FRC_SYNC_DAYS * FRC_US_PER_DAY);
    NOTE_AT(t, FRC_SYNC_LEVEL);

    CHECK(frc_sync_by_number(cat, s.sync, &same) == FRC_OK);
    CHECK(same.start == s.start && same.end == s.end && same.nominal == s.nominal && same.drift_us == s.drift_us);
    CHECK(same.phase == 0 && same.quarter == 0 && same.next.time == FRC_TIME_NONE);

    CHECK(frc_sync_odometer(cat, t, strokes, digits, &now) == FRC_OK);
    if (t == last) {
        CHECK(s.next.time == FRC_TIME_NONE && now.next_change == FRC_TIME_NONE);
        return;
    }

    /* the next quarter of the sync: first microsecond of it */
    CHECK(s.next.time > t && s.next.time <= s.end);
    CHECK(frc_sync_at(cat, s.next.time - 1, &other) == FRC_OK);
    CHECK(other.sync == s.sync && other.quarter == s.quarter && other.phase >= s.phase);
    if (s.quarter == FRC_END) {
        CHECK_EQ_I64(s.next.time, s.end);
    } else {
        CHECK(frc_sync_at(cat, s.next.time, &other) == FRC_OK);
        CHECK(other.sync == s.sync && other.quarter == s.quarter + 1);
    }
    NOTE_AT(t, FRC_SYNC_LEVEL);

    /* the sync odometer: first microsecond of the next state */
    CHECK(now.next_change > t && now.next_change <= s.end);
    CHECK(frc_sync_odometer(cat, now.next_change - 1, strokes, digits, &before) == FRC_OK);
    CHECK(same_digits(&before, &now));
    if (now.next_change < s.end) {
        CHECK(frc_sync_odometer(cat, now.next_change, strokes, digits, &after) == FRC_OK);
        CHECK(!same_digits(&after, &now)); /* the shortest state, 4 days / 484^4, is 6 us */
    } else {
        /* the last state of a sync ends with it: the next sync starts at zero */
        frc_status_t at_end = frc_sync_odometer(cat, now.next_change, strokes, digits, &after);
        CHECK(at_end == FRC_ERR_RANGE || now.next_change == last ||
              (at_end == FRC_OK && after.digit[0] == 0 && after.digit[digits - 1] == 0));
    }
    CHECK_NOTE("%s, t = %" PRId64 ", sync odometer %d strokes, %d digits", cat_name, (int64_t)t, strokes, digits);

    /* the phase is linear in time and never runs backwards inside the sync */
    {
        frc_time_t t2 = random_between(t, s.end - 1);
        CHECK(frc_sync_at(cat, t2, &other) == FRC_OK);
        CHECK(other.sync == s.sync && other.phase >= s.phase && other.quarter >= s.quarter);
        /* (t - start) / (end - start) on the returned fields is within the
         * rounding of start and end (a microsecond each) of the exact phase */
        {
            double approx = (double)(t - s.start) / (double)(s.end - s.start);
            double exact = (double)s.phase / 4294967296.0;
            CHECK(approx - exact < 1e-9 && exact - approx < 1e-9);
        }
        NOTE_AT(t, FRC_SYNC_LEVEL);
    }
}

/* Every sync and every Pentad of the catalogue, in order: a chain without
 * gap or overlap. Pentads go from the first instant to the last (the two at
 * the edges are cut); whole syncs tile what lies between the partial ones. */
static void check_sync_tiling(void)
{
    frc_pentad_t p;
    frc_sync_t s;
    frc_time_t expected_pentad_start = first;
    frc_time_t expected_sync_start = FRC_TIME_NONE;
    int32_t number, first_number, last_number;
    int64_t pentads = 0, syncs = 0, broken = 0;
    uint8_t index;

    CHECK(frc_pentad_at(cat, first, &p) == FRC_OK);
    first_number = p.sync;
    CHECK(frc_pentad_at(cat, last, &p) == FRC_OK);
    last_number = p.sync;
    CHECK(first_number <= 0 && last_number >= first_number);

    for (number = first_number; number <= last_number; number++) {
        frc_status_t whole = frc_sync_by_number(cat, number, &s);
        frc_time_t sync_start = FRC_TIME_NONE, sync_end = FRC_TIME_NONE;
        if (whole == FRC_OK) {
            syncs++;
            /* whole syncs follow each other without a gap */
            if (expected_sync_start != FRC_TIME_NONE && s.start != expected_sync_start) {
                broken++;
            }
            if (s.sync != number || s.start >= s.end) {
                broken++;
            }
            expected_sync_start = s.end;
            sync_start = s.start;
            sync_end = s.end;
        } else if (whole != FRC_ERR_RANGE || (number != first_number && number != last_number)) {
            broken++; /* only the first and the last sync may be partial */
        }
        for (index = 0; index < 4; index++) {
            frc_status_t status = frc_pentad_by_number(cat, number, index, &p);
            if (status == FRC_ERR_RANGE && whole != FRC_OK) {
                continue; /* a Pentad of a partial sync that lies wholly outside */
            }
            if (status != FRC_OK || p.start != expected_pentad_start || p.start >= p.end ||
                p.sync != number || p.index != index || p.helions != (index == 3 ? 4 : 5)) {
                broken++;
                continue;
            }
            /* a whole sync is exactly its four Pentads */
            if (whole == FRC_OK && ((index == 0 && p.start != sync_start) || (index == 3 && p.end != sync_end))) {
                broken++;
            }
            expected_pentad_start = p.end;
            pentads++;
        }
    }
    CHECK_EQ_I64(broken, 0);
    CHECK_NOTE("%s: sync / Pentad chain broken %" PRId64 " times", cat_name, broken);
    CHECK_EQ_I64(expected_pentad_start, last);
    CHECK(syncs >= 1 && pentads >= 4 * syncs);
    /* nothing before the first and after the last */
    CHECK(frc_sync_by_number(cat, first_number - 1, &s) == FRC_ERR_RANGE);
    CHECK(frc_sync_by_number(cat, last_number + 1, &s) == FRC_ERR_RANGE);
    for (index = 0; index < 4; index++) {
        CHECK(frc_pentad_by_number(cat, first_number - 1, index, &p) == FRC_ERR_RANGE);
        CHECK(frc_pentad_by_number(cat, last_number + 1, index, &p) == FRC_ERR_RANGE);
    }
    printf("     %s: %" PRId64 " whole syncs (%d .. %d with the partial ones), %" PRId64 " Pentads tile it\n",
           cat_name, syncs, (int)first_number, (int)last_number, pentads);
}

/* ------------------------------------------------------------------------- */
/* One instant, everything                                                    */
/* ------------------------------------------------------------------------- */

static void check_instant(frc_time_t t)
{
    uint8_t level;
    uint8_t strokes = (uint8_t)random_between(1, FRC_MAX_STROKES);
    uint8_t digits = (uint8_t)random_between(1, FRC_MAX_DIGITS);

    TEST_SECTION("position round trip");
    check_position(t);

    TEST_SECTION("cycles tile the catalogue");
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        check_cycle(t, level);
    }
    TEST_SECTION("next threshold is the first microsecond of the next quarter");
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        check_threshold(t, level);
    }
    TEST_SECTION("odometer next_change is the first microsecond with other digits");
    /* the basic reading of S15.3, and one random odometer on every level */
    check_odometer(t, 6, 2, 2);
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        check_odometer(t, level, strokes, digits);
        strokes = (uint8_t)(strokes % FRC_MAX_STROKES + 1);
        digits = (uint8_t)(digits % FRC_MAX_DIGITS + 1);
    }
    TEST_SECTION("phases are monotonic inside a cycle");
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        check_phase_monotonic(t, level);
    }
    TEST_SECTION("date <-> time round trip");
    check_date_round_trip(t);
    TEST_SECTION("parse(format(date)) round trip");
    check_text_round_trip(t);
    TEST_SECTION("sync and Pentad at an instant");
    check_sync(t);
}


/* ------------------------------------------------------------------------- */
/* The instant of a phase (frc_time_at_phase, frc_time_at_sync_phase), and    */
/* the places of the Sun (frc_sync_t.helion, frc_pentad_t.first)               */
/* ------------------------------------------------------------------------- */

/* Smallest instant of cycle k with a phase >= f: at r the cycle is k and the
 * phase >= f; one microsecond earlier another cycle or a phase below f. */
static void check_phase_instant(uint8_t level, uint64_t k, frc_frac_t f)
{
    frc_cycle_t at, before, cycle;
    frc_time_t r = frc_time_at_phase(cat, level, k, f);

    CHECK(frc_cycle_by_index(cat, level, k, &cycle) == FRC_OK);
    if (r == FRC_TIME_NONE) {
        /* only past the closing instant, in the clipped last cycle */
        CHECK(cycle.clipped);
        CHECK_NOTE("%s, level %u, cycle %" PRIu64 ", phase %u", cat_name, level, k, (unsigned)f);
        return;
    }
    CHECK(r >= cycle.start && r <= cycle.end);
    if (f == 0) {
        CHECK_EQ_I64(r, cycle.start);
    }
    if (r == last || r == cycle.end || level > 18) {
        return; /* read one microsecond early / no microsecond of the cycle left / sub-microsecond cycles */
    }
    CHECK(frc_cycle_at(cat, r, level, &at) == FRC_OK);
    CHECK(at.cycle == k && at.phase >= f);
    CHECK_NOTE("%s, level %u, cycle %" PRIu64 ", phase %u: at %" PRId64 " cycle %" PRIu64 " phase %u", cat_name,
               level, k, (unsigned)f, r, at.cycle, (unsigned)at.phase);
    if (r > first) {
        CHECK(frc_cycle_at(cat, r - 1, level, &before) == FRC_OK);
        CHECK(before.cycle < k || (before.cycle == k && before.phase < f));
        CHECK_NOTE("%s, level %u, cycle %" PRIu64 ", phase %u: before %" PRId64 " cycle %" PRIu64 " phase %u",
                   cat_name, level, k, (unsigned)f, r - 1, before.cycle, (unsigned)before.phase);
    }
}

static void check_sync_phase_instant(int32_t number, frc_frac_t f)
{
    frc_sync_t at, before, sync;
    frc_time_t r = frc_time_at_sync_phase(cat, number, f);

    CHECK(frc_sync_by_number(cat, number, &sync) == FRC_OK);
    CHECK(r != FRC_TIME_NONE && r >= sync.start && r < sync.end);
    if (f == 0) {
        CHECK_EQ_I64(r, sync.start);
    }
    if (r == FRC_TIME_NONE || r == last) {
        return;
    }
    CHECK(frc_sync_at(cat, r, &at) == FRC_OK);
    CHECK(at.sync == number && at.phase >= f);
    CHECK_NOTE("%s, sync %d, phase %u: at %" PRId64 " sync %d phase %u", cat_name, (int)number, (unsigned)f, r,
               (int)at.sync, (unsigned)at.phase);
    if (frc_sync_at(cat, r - 1, &before) == FRC_OK) {
        CHECK(before.sync < number || (before.sync == number && before.phase < f));
        CHECK_NOTE("%s, sync %d, phase %u: before %" PRId64 " phase %u", cat_name, (int)number, (unsigned)f, r - 1,
                   (unsigned)before.phase);
    }
}

static void check_phase_inverses(void)
{
    int i;

    TEST_SECTION("frc_time_at_phase: the smallest instant of the cycle at or after the phase");
    for (i = 0; i < RANDOM_INSTANTS / 2; i++) {
        frc_time_t t = random_between(first, last - 1);
        uint8_t level = (uint8_t)(random_u64() % (FRC_CLOCK_MAX_LEVEL + 1));
        frc_cycle_t c;
        frc_time_t r;
        frc_time_t points[5];
        CHECK(frc_cycle_at(cat, t, level, &c) == FRC_OK);
        /* the reading's own phase: reached at or before t */
        r = frc_time_at_phase(cat, level, c.cycle, c.phase);
        CHECK(r != FRC_TIME_NONE && r <= t && r >= c.start);
        NOTE_AT(t, level);
        check_phase_instant(level, c.cycle, c.phase);
        check_phase_instant(level, c.cycle, (frc_frac_t)random_u64());
        check_phase_instant(level, c.cycle, (frc_frac_t)(random_u64() >> (random_u64() % 32)));
        check_phase_instant(level, c.cycle, 0xFFFFFFFFu);
        check_phase_instant(level, c.cycle, 0);
        /* the quarter points are the phases 1/4, 1/2, 3/4 */
        CHECK(frc_cycle_points(cat, level, c.cycle, points) == FRC_OK);
        CHECK_EQ_I64(frc_time_at_phase(cat, level, c.cycle, 0), points[0]);
        CHECK_EQ_I64(frc_time_at_phase(cat, level, c.cycle, FRC_FRAC_QUARTER), points[1]);
        CHECK_EQ_I64(frc_time_at_phase(cat, level, c.cycle, FRC_FRAC_HALF), points[2]);
        CHECK_EQ_I64(frc_time_at_phase(cat, level, c.cycle, FRC_FRAC_HALF | FRC_FRAC_QUARTER), points[3]);
        NOTE_AT(t, level);
    }
    TEST_SECTION("frc_time_at_phase: the last cycle and the arguments");
    {
        uint8_t level;
        for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
            uint64_t n = frc_cycle_count(cat, level);
            check_phase_instant(level, n - 1, 0);
            check_phase_instant(level, n - 1, 0xFFFFFFFFu);
            check_phase_instant(level, n - 1, FRC_FRAC_HALF);
            CHECK_EQ_I64(frc_time_at_phase(cat, level, n, 0), FRC_TIME_NONE);
            CHECK_EQ_I64(frc_time_at_phase(cat, level, UINT64_MAX, 0), FRC_TIME_NONE);
        }
        CHECK_EQ_I64(frc_time_at_phase(cat, FRC_CLOCK_MAX_LEVEL + 1, 0, 0), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_time_at_phase(NULL, 0, 0, 0), FRC_TIME_NONE);
    }

    TEST_SECTION("frc_time_at_sync_phase: the smallest instant of the sync at or after the phase");
    for (i = 0; i < RANDOM_INSTANTS / 2; i++) {
        frc_time_t t = random_between(first, last - 1);
        frc_sync_t s, again;
        frc_time_t r;
        if (frc_sync_at(cat, t, &s) != FRC_OK) {
            continue; /* a partial sync at an edge */
        }
        r = frc_time_at_sync_phase(cat, s.sync, s.phase);
        CHECK(r <= t && r >= s.start);
        CHECK_NOTE("%s, t = %" PRId64, cat_name, t);
        check_sync_phase_instant(s.sync, s.phase);
        check_sync_phase_instant(s.sync, (frc_frac_t)random_u64());
        check_sync_phase_instant(s.sync, 0xFFFFFFFFu);
        check_sync_phase_instant(s.sync, 0);
        /* the end of the running quarter is next.time */
        if (s.next.time != FRC_TIME_NONE && s.quarter < 3) {
            /* (the End is the next sync's phase 0, not a phase of this one) */
            CHECK_EQ_I64(frc_time_at_sync_phase(cat, s.sync, (frc_frac_t)(((uint32_t)s.quarter + 1u) << 30)),
                         s.next.time);
        }
        /* C1: the running Helion's place, 0..18, by the Helion indices */
        {
            frc_cycle_t helion, opening;
            frc_pentad_t pentad;
            CHECK(frc_cycle_at(cat, t, FRC_SYNC_LEVEL, &helion) == FRC_OK);
            CHECK(frc_cycle_at(cat, s.start, FRC_SYNC_LEVEL, &opening) == FRC_OK);
            CHECK(s.helion < FRC_SYNC_CYCLES);
            CHECK_EQ_U64(s.helion, helion.cycle - opening.cycle);
            /* C2: the Pentad's first Helion and its running one add up */
            CHECK(frc_pentad_at(cat, t, &pentad) == FRC_OK);
            CHECK_EQ_U64(pentad.first, 5u * pentad.index);
            CHECK_EQ_U64((unsigned)pentad.first + pentad.helion - 1u, s.helion);
            CHECK(frc_sync_by_number(cat, s.sync, &again) == FRC_OK && again.helion == 0);
            CHECK_NOTE("%s, t = %" PRId64, cat_name, t);
        }
    }
    CHECK_EQ_I64(frc_time_at_sync_phase(cat, INT32_MIN, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_sync_phase(cat, INT32_MAX, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_sync_phase(NULL, 0, 0), FRC_TIME_NONE);
}

static void check_catalogue(const frc_catalogue_t *catalogue, const char *name)
{
    uint16_t k;
    uint8_t level;
    int i;

    cat = catalogue;
    cat_name = name;
    first = frc_catalogue_first(cat);
    last = frc_catalogue_last(cat);
    random_state = SEED;

    /* the edges, and every eclipse with its two neighbouring microseconds:
     * where each grid of the clock has a boundary */
    for (k = 0; k < cat->count; k++) {
        frc_time_t e = frc_catalogue_instant(cat, k);
        if (k > 0) {
            frc_odometer_t odometer;
            frc_threshold_t next;
            check_instant(e - 1);
            /* an eclipse is a boundary of every grid: one microsecond before
             * it, the next change of anything is the eclipse itself */
            TEST_SECTION("every eclipse is a boundary of every grid");
            for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
                CHECK(frc_next_threshold(cat, e - 1, level, &next) == FRC_OK);
                CHECK_EQ_I64(next.time, e);
                CHECK(frc_odometer_at(cat, e - 1, level, 2, 2, &odometer) == FRC_OK);
                CHECK_EQ_I64(odometer.next_change, e);
                NOTE_AT(e - 1, level);
            }
        }
        check_instant(e);
        if (k + 1 < cat->count) {
            check_instant(e + 1);
        }
    }

    for (i = 0; i < RANDOM_INSTANTS; i++) {
        check_instant(random_between(first, last));
    }

    TEST_SECTION("cycles tile the catalogue, exhaustively at levels 0..6");
    for (level = 0; level <= 6; level++) {
        check_tiling_exhaustive(level);
    }
    TEST_SECTION("syncs and Pentads tile the catalogue");
    check_sync_tiling();
    check_phase_inverses();

    /* outside the catalogue there is nothing */
    TEST_SECTION("outside the catalogue");
    {
        frc_cycle_t c;
        frc_date_t d;
        frc_odometer_t o;
        frc_sync_t s;
        frc_pentad_t p;
        frc_threshold_t th;
        frc_time_t outside[2];
        outside[0] = first - 1;
        outside[1] = last + 1;
        for (i = 0; i < 2; i++) {
            CHECK(!frc_catalogue_contains(cat, outside[i]));
            CHECK(frc_cycle_at(cat, outside[i], 6, &c) == FRC_ERR_RANGE);
            CHECK(frc_date_at(cat, outside[i], &d) == FRC_ERR_RANGE);
            CHECK(frc_odometer_at(cat, outside[i], 6, 2, 2, &o) == FRC_ERR_RANGE);
            CHECK(frc_next_threshold(cat, outside[i], 6, &th) == FRC_ERR_RANGE);
            CHECK(frc_sync_at(cat, outside[i], &s) == FRC_ERR_RANGE);
            CHECK(frc_pentad_at(cat, outside[i], &p) == FRC_ERR_RANGE);
        }
    }
}

int main(void)
{
    check_catalogue(&FRC_SAROS_141, "Saros 141");

    TEST_SECTION("the uneven catalogue");
    CHECK(frc_catalogue_init(&uneven, UNEVEN_MS, 14, 0) == FRC_OK);
    /* S7.2: the first ordinal A = 0 (mod 4) on or after 1972-01-01; here the
     * one before the last eclipse, so no full Alpha follows it */
    CHECK_EQ_U64(uneven.anchor, 12);
    check_catalogue(&uneven, "uneven series");

    return TEST_SUMMARY("properties");
}
