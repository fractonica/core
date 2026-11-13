/*
 * test_span.c - tests of src/frc_span.c (SPEC S15.4 and S15.5):
 *
 *   frc_span_between, frc_span_of_level     measuring a span
 *   frc_span_parts, frc_format_span         counting and writing it
 *   frc_rate_preset, frc_format_rate        time-lapse presets
 *   frc_level_per_blink, frc_format_per_blink   cycles per Blink
 *   frc_span_next_change, frc_span_next_change_since   when a span's text changes
 *
 * Expected values come from three places, in this order of authority:
 *   1. SPEC.md, typed in here by hand: the span of S15.6, the cycle lengths
 *      of S15.4, the presets and the pB values of S15.5.
 *   2. tests/vectors/oracle_notation.h, every row of the span, cycle-length,
 *      rate and per-Blink tables (generated from the frozen JavaScript
 *      reference with exact rationals; where the reference's own double text
 *      differs, the header lists it and carries the exact text).
 *   3. Properties: a span and its reverse, spans that add up, a span against
 *      the Blink index and phase of the two instants, parts that add up.
 *   4. The time grain: between two instants the API returned (cycle starts
 *      and ends, thresholds, dates, syncs, Pentads) a span is the exact
 *      count; and on a table where exact rounding ties exist, every text is
 *      the text of the exact rational span.
 */
#include "frc_test.h"

#include "fractonica.h"
#include "frc_internal.h" /* frc_i_span_change_guess: the fast path of the next-change search */
#include "vectors/oracle_notation.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static const frc_catalogue_t *const CAT = &FRC_SAROS_141;

#define NU     "\xCE\xBD"     /* ν */
#define LAMBDA "\xCE\xBB"     /* λ */
#define IOTA   "\xCE\xB9"     /* ι */
#define ETA    "\xCE\xB7"     /* η */
#define EPS    "\xCE\xB5"     /* ε */
#define MINUS  "\xE2\x88\x92" /* − U+2212 */
#define NNBSP  "\xE2\x80\xAF" /* U+202F, the digit-group space */
#define DASH   "\xE2\x80\x94" /* — U+2014 */

/* One Saros is P(12) / 4 Blinks (S15.1: an Alpha is 7,319,812,500 Blinks). */
#define BLINKS_PER_SAROS UINT64_C(1829953125)
#define FRAC_ONE (UINT64_C(1) << 32)

/* splitmix64 with a fixed seed, so a failure reproduces. */
static uint64_t rng_state = UINT64_C(0x5BA9C0DE);
static uint64_t rng(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}
/* A random instant of the catalogue before its closing instant. */
static frc_time_t rng_instant(void)
{
    uint64_t span = (uint64_t)(frc_catalogue_last(CAT) - frc_catalogue_first(CAT));
    return frc_catalogue_first(CAT) + (frc_time_t)(rng() % span);
}

static frc_span_t span_of(int negative, uint64_t blinks, uint32_t frac)
{
    frc_span_t s;
    s.negative = (uint8_t)negative;
    s.blinks = blinks;
    s.frac = frac;
    return s;
}

/* floor(x * 2^32) for a decimal fraction given as n / d. */
static uint32_t frac_of(uint64_t n, uint64_t d)
{
    return (uint32_t)((n << 32) / d);
}

/* a - b in units of 2^-32 Blink, for two spans less than 2^31 Blinks apart. */
static int64_t units_between(uint64_t a_blinks, uint64_t a_frac, uint64_t b_blinks, uint64_t b_frac)
{
    return (int64_t)(a_blinks - b_blinks) * (int64_t)FRAC_ONE + ((int64_t)a_frac - (int64_t)b_frac);
}

/* One microsecond of position at `t`, in units of 2^-32 Blink, rounded up:
 * the grain of a span that begins or ends there (13,810 on Saros 141).
 * P(12) / den quarter Blinks, times 2^30; P(12) * 2^30 < 2^63. */
static uint64_t grain_units(const frc_catalogue_t *cat, frc_time_t t)
{
    frc_position_t p;
    if (frc_position_at(cat, t, &p) != FRC_OK) {
        return 0;
    }
    return (frc_prefix(12) << 30) / p.den + 1;
}

/* 1 when the span is a whole number of quarter Blinks: what the time grain
 * snaps to (frc_span_between), and only then may it differ from the floor. */
static int on_quarter_grid(const frc_span_t *s)
{
    return (s->frac & 0x3FFFFFFFu) == 0;
}

/* 1 when a rounding mark (a multiple of 1/20000 Blink) lies in the last unit
 * of the fraction: in (frac - 1, frac] / 2^32. Only such a fraction may be one
 * unit above the floor of the exact value (fractonica.h, frc_span_t). */
static int mark_in_last_unit(const frc_span_t *s)
{
    return s->frac != 0 && ((uint64_t)(s->frac - 1) * 20000u) / FRAC_ONE != ((uint64_t)s->frac * 20000u) / FRAC_ONE;
}

/* A span as one number of 2^-32 Blinks (the spans used with it are short). */
static uint64_t as_fixed(const frc_span_t *s)
{
    return (s->blinks << 32) | s->frac;
}

/* Format a span and compare. */
static void expect_span_text(frc_span_t span, uint8_t max_parts, const char *want)
{
    char text[160];
    int n = frc_format_span(&span, max_parts, text, sizeof text);
    CHECK_EQ_I64(n, (int)strlen(want));
    CHECK_STR(text, want);
    CHECK_NOTE("span %s%" PRIu64 " + %u / 2^32 Blinks, max_parts %u", span.negative ? "-" : "", span.blinks,
               span.frac, max_parts);
}

static void expect_per_blink(uint64_t num, uint64_t den, const char *want)
{
    char text[80];
    int n = frc_format_per_blink(num, den, text, sizeof text);
    CHECK_EQ_I64(n, (int)strlen(want));
    CHECK_STR(text, want);
    CHECK_NOTE("%" PRIu64 " / %" PRIu64 " per Blink", num, den);
}

/* strnlen, which strict C11 does not have. */
static size_t bounded_length(const char *s, size_t limit)
{
    size_t n = 0;
    while (n < limit && s[n] != '\0') {
        n++;
    }
    return n;
}

/*
 * The contract of every text writer, checked for every buffer size:
 *   size > length   returns the length and writes the full text
 *   size <= length  returns FRC_ERR_BUFFER and leaves a NUL-terminated prefix
 *                   that ends on a code-point boundary
 *   nothing is written past `size`; NULL and size 0 are safe
 * CALL(buf, size) is a macro that makes the call.
 */
#define CHECK_EVERY_SIZE(CALL, FULL)                                                         \
    do {                                                                                     \
        const char *full_ = (FULL);                                                          \
        size_t length_ = strlen(full_);                                                      \
        size_t size_;                                                                        \
        for (size_ = 0; size_ <= length_ + 2; size_++) {                                     \
            char buf_[200];                                                                  \
            int n_;                                                                          \
            memset(buf_, '#', sizeof buf_);                                                  \
            n_ = CALL(buf_, size_);                                                          \
            CHECK(buf_[size_] == '#'); /* never past the buffer */                           \
            if (size_ > length_) {                                                           \
                CHECK_EQ_I64(n_, (int)length_);                                              \
                CHECK_STR(buf_, full_);                                                      \
            } else {                                                                         \
                CHECK_EQ_I64(n_, FRC_ERR_BUFFER);                                            \
                if (size_ > 0) {                                                             \
                    size_t kept_ = bounded_length(buf_, size_);                              \
                    CHECK(kept_ < size_);                                                    \
                    CHECK(strncmp(buf_, full_, kept_) == 0);                                 \
                    CHECK(((unsigned char)full_[kept_] & 0xC0u) != 0x80u);                   \
                }                                                                            \
            }                                                                                \
        }                                                                                    \
        CHECK_EQ_I64(CALL(NULL, 64), FRC_ERR_BUFFER);                                        \
        CHECK_EQ_I64(CALL(NULL, 0), FRC_ERR_BUFFER);                                         \
    } while (0)

/* An argument error: the status and "" in the buffer, also with no buffer. */
#define CHECK_REFUSED_SPAN(CALL)                                                             \
    do {                                                                                     \
        char buf_[40];                                                                       \
        memset(buf_, '#', sizeof buf_);                                                      \
        CHECK_EQ_I64(CALL(buf_, sizeof buf_), FRC_ERR_ARG);                                  \
        CHECK(buf_[0] == '\0' && buf_[1] == '#');                                           \
        CHECK_EQ_I64(CALL(NULL, 0), FRC_ERR_ARG);                                            \
    } while (0)

/* ------------------------------------------------------------------------- */
/* SPEC, typed by hand                                                        */
/* ------------------------------------------------------------------------- */

static void test_spec_vectors(void)
{
    /* S15.4: "depths 1-5, 7, 9, 11 and 13 are one unit each. The depths
     * between are whole counts of the next unit: ζ = 11η, θ = 4ι, κ = 13λ,
     * μ = 5ν, and ξ = 0.14ν." By LEVEL = depth - 1. */
    static const char *const CYCLE_LENGTH[14] = {
        "1\xCE\xB1", "1\xCE\xB2", "1\xCE\xB3", "1\xCE\xB4", "1" EPS, "11" ETA, "1" ETA,
        "4" IOTA,    "1" IOTA,    "13" LAMBDA, "1" LAMBDA,  "5" NU,  "1" NU,   "0.14" NU};
    /* S15.5: "5ν (5×), λ (25×), 13λ (325×), ι (1,625×), 4ι (6,500×),
     * η (58,500×), 11η (643,500×), then ε, δ, γ per Blink" */
    static const uint64_t FACTOR[FRC_RATE_COUNT] = {1,     5,      25,      325,      1625,     6500,
                                                    58500, 643500, 4504500, 22522500, 112612500};
    static const char *const LABEL[FRC_RATE_COUNT] = {
        "real time",        "5" NU " per Blink", LAMBDA " per Blink",   "13" LAMBDA " per Blink",
        IOTA " per Blink",  "4" IOTA " per Blink", ETA " per Blink",    "11" ETA " per Blink",
        EPS " per Blink",   "\xCE\xB4 per Blink", "\xCE\xB3 per Blink"};
    frc_span_t span;
    frc_rate_t rate;
    uint64_t num, den;
    char text[80];
    uint8_t level, i;

    TEST_SECTION("SPEC S15.6: the span at A");
    /* "At A the depth-7 end is 3,372.428 s away, which is the span 6ι 43λ" */
    CHECK_EQ_I64(frc_span_between(CAT, FRC_TIME_FROM_MS(INT64_C(1791460800000)),
                                  FRC_TIME_FROM_MS(INT64_C(1791460800000) + 3372428), &span), FRC_OK);
    expect_span_text(span, 0, "6" IOTA " 43" LAMBDA);
    expect_span_text(span, 2, "6" IOTA " 43" LAMBDA);
    CHECK_EQ_I64(frc_span_between(CAT, ORACLE_NOTATION_S15_6_SPAN_T1_US, ORACLE_NOTATION_S15_6_SPAN_T2_US, &span),
                 FRC_OK);
    expect_span_text(span, 2, ORACLE_NOTATION_S15_6_SPAN_TEXT);

    TEST_SECTION("SPEC S15.4: exact cycle lengths");
    for (level = 0; level < 14; level++) {
        CHECK_EQ_I64(frc_span_of_level(level, &span), FRC_OK);
        expect_span_text(span, 2, CYCLE_LENGTH[level]);
    }
    /* S15.1: Alpha 5 Beta 13 Gamma 5 Delta 5 Nuclide 77 Helion 36 Preon 65 Flavon 25 Blink */
    CHECK_EQ_I64(frc_span_of_level(0, &span), FRC_OK);
    CHECK_EQ_U64(span.blinks, UINT64_C(5) * 13 * 5 * 5 * 77 * 36 * 65 * 25);
    CHECK_EQ_I64(frc_span_of_level(6, &span), FRC_OK);
    CHECK_EQ_U64(span.blinks, 36 * 65 * 25);

    TEST_SECTION("SPEC S15.5: presets and pB");
    for (i = 0; i < FRC_RATE_COUNT; i++) {
        CHECK_EQ_I64(frc_rate_preset(i, &rate), FRC_OK);
        CHECK_EQ_U64(rate.factor, FACTOR[i]);
        CHECK(frc_format_rate(&rate, text, sizeof text) > 0);
        CHECK_STR(text, LABEL[i]);
    }
    /* "The true rates of the depths below the Blink are whole numbers:
     * ξ 7, ο 77, π 693 and ρ 2,772 pB" (levels 13..16) */
    CHECK_EQ_I64(frc_level_per_blink(13, &num, &den), FRC_OK);
    CHECK(num == 7 && den == 1);
    CHECK_EQ_I64(frc_level_per_blink(14, &num, &den), FRC_OK);
    CHECK(num == 77 && den == 1);
    CHECK_EQ_I64(frc_level_per_blink(15, &num, &den), FRC_OK);
    CHECK(num == 693 && den == 1);
    CHECK_EQ_I64(frc_level_per_blink(16, &num, &den), FRC_OK);
    CHECK(num == 2772 && den == 1);
    expect_per_blink(num, den, "2" NNBSP "772 pB");
    /* fractonica.h: "Helion = 1/58500", "1 per 58 500 Blinks" */
    CHECK_EQ_I64(frc_level_per_blink(6, &num, &den), FRC_OK);
    CHECK(num == 1 && den == 58500);
    expect_per_blink(num, den, "1 per 58" NNBSP "500 Blinks");
    /* "A 110 Hz tone is 34.2 pB": 110 Hz times the mean Blink. The mean
     * interval is 39259007522784/69 ms (S16.1), a Blink is 4/P(12) of it:
     * 110 * 4 * 39259007522784 / (69 * 1000 * 7319812500) per Blink. */
    expect_per_blink(UINT64_C(110) * 4 * UINT64_C(39259007522784), UINT64_C(69) * 1000 * UINT64_C(7319812500), "34.2 pB");
    expect_per_blink(7, 1, "7 pB");
}

/* ------------------------------------------------------------------------- */
/* Oracle: spans                                                              */
/* ------------------------------------------------------------------------- */

static void test_oracle_spans(void)
{
    static const uint8_t MAX_PARTS[3] = {2, 3, 9};
    size_t i;
    int k, j, zeros;

    TEST_SECTION("oracle spans");
    for (i = 0; i < ORACLE_NOTATION_SPANS_COUNT; i++) {
        const oracle_notation_span_t *o = &ORACLE_NOTATION_SPANS[i];
        frc_span_t span, back;
        frc_span_part_t parts[FRC_UNIT_COUNT + 2];

        memset(&span, 0xEE, sizeof span);
        CHECK_EQ_I64(frc_span_between(CAT, o->t1_us, o->t2_us, &span), FRC_OK);
        CHECK_EQ_U64(span.negative, o->negative);
        CHECK_EQ_U64(span.blinks, o->blinks);
        CHECK_EQ_U64(span.frac, o->frac);
        CHECK_NOTE("span %zu: %" PRId64 " -> %" PRId64, i, o->t1_us, o->t2_us);

        /* the same span walked the other way */
        CHECK_EQ_I64(frc_span_between(CAT, o->t2_us, o->t1_us, &back), FRC_OK);
        CHECK(back.blinks == span.blinks && back.frac == span.frac);
        CHECK_EQ_U64(back.negative, (o->t1_us != o->t2_us) ? !o->negative : 0);

        for (zeros = 0; zeros < 2; zeros++) {
            const uint8_t *want_n = zeros ? o->nz : o->n;
            const oracle_notation_span_part_t *want = zeros ? o->zparts : o->parts;
            for (k = 0; k < 3; k++) {
                uint8_t n;
                memset(parts, 0xEE, sizeof parts);
                n = frc_span_parts(&span, MAX_PARTS[k], zeros, parts);
                CHECK_EQ_U64(n, want_n[k]);
                CHECK_NOTE("span %zu, max_parts %u, keep_zeros %d", i, MAX_PARTS[k], zeros);
                for (j = 0; j < n && j < FRC_UNIT_COUNT; j++) {
                    CHECK(parts[j].unit == want[j].unit && parts[j].count == want[j].count);
                    CHECK_NOTE("span %zu, max_parts %u, keep_zeros %d, part %d: %" PRIu64 " of unit %u", i,
                               MAX_PARTS[k], zeros, j, parts[j].count, parts[j].unit);
                }
                /* nothing is written after the parts returned */
                CHECK(parts[n].unit == 0xEE);
            }
        }

        expect_span_text(span, 2, o->text[0]);
        expect_span_text(span, 3, o->text[1]);
        expect_span_text(span, 0, o->text[0]); /* 0 means the default precision, 2 */
    }

    TEST_SECTION("oracle cycle lengths");
    for (i = 0; i < ORACLE_NOTATION_CYCLE_LENGTHS_COUNT; i++) {
        const oracle_notation_cycle_length_t *o = &ORACLE_NOTATION_CYCLE_LENGTHS[i];
        frc_span_t span;
        memset(&span, 0xEE, sizeof span);
        CHECK_EQ_I64(frc_span_of_level(o->level, &span), FRC_OK);
        CHECK_EQ_U64(span.negative, 0);
        CHECK_EQ_U64(span.blinks, o->blinks);
        CHECK_EQ_U64(span.frac, o->frac);
        expect_span_text(span, 2, o->text);
        expect_span_text(span, 0, o->text);
    }
}

/* ------------------------------------------------------------------------- */
/* Measuring: properties and edges                                            */
/* ------------------------------------------------------------------------- */

static void test_span_of_level(void)
{
    frc_span_t span;
    uint8_t level;

    TEST_SECTION("frc_span_of_level: every level");
    for (level = 0; level < FRC_LEVEL_COUNT; level++) {
        memset(&span, 0xEE, sizeof span);
        CHECK_EQ_I64(frc_span_of_level(level, &span), FRC_OK);
        CHECK_EQ_U64(span.negative, 0);
        if (level <= 12) {
            /* P(12) / P(level) whole Blinks */
            CHECK_EQ_U64(span.blinks, frc_prefix(12) / frc_prefix(level));
            CHECK_EQ_U64(span.blinks * frc_prefix(level), frc_prefix(12));
            CHECK_EQ_U64(span.frac, 0);
        } else if (level <= 22) {
            /* floor(2^32 * P(12) / P(level)); P(level) / P(12) is whole */
            uint64_t ratio = frc_prefix(level) / frc_prefix(12);
            CHECK_EQ_U64(ratio * frc_prefix(12), frc_prefix(level));
            CHECK_EQ_U64(span.blinks, 0);
            CHECK_EQ_U64(span.frac, FRAC_ONE / ratio);
        } else {
            /* level 23 is 1 / 3,121,618,500 of a Blink: 1.38 units of 2^-32.
             * From level 24 (1 / 12,486,474,000) on it is below one unit. */
            CHECK_EQ_U64(span.blinks, 0);
            CHECK_EQ_U64(span.frac, level == 23 ? 1 : 0);
        }
    }
    /* each level is one factor finer than the one above: lengths divide */
    for (level = 1; level <= 12; level++) {
        frc_span_t parent;
        CHECK_EQ_I64(frc_span_of_level((uint8_t)(level - 1), &parent), FRC_OK);
        CHECK_EQ_I64(frc_span_of_level(level, &span), FRC_OK);
        CHECK_EQ_U64(parent.blinks, span.blinks * frc_siblings(level));
    }
    CHECK_EQ_I64(frc_span_of_level(FRC_LEVEL_COUNT, &span), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_span_of_level(255, &span), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_span_of_level(0, NULL), FRC_ERR_ARG);
}

static void test_span_between(void)
{
    frc_time_t first = frc_catalogue_first(CAT);
    frc_time_t last = frc_catalogue_last(CAT);
    frc_span_t span, other;
    int i;
    uint16_t k;

    TEST_SECTION("frc_span_between: eclipses and the catalogue's edges");
    /* eclipse k is position k: any two eclipses are a whole number of Saros apart */
    for (k = 0; k + 1 < CAT->count; k++) {
        uint16_t to = (uint16_t)(k + 1 + (k * 7u) % (uint16_t)(CAT->count - 1 - k));
        CHECK_EQ_I64(frc_span_between(CAT, frc_catalogue_instant(CAT, k), frc_catalogue_instant(CAT, to), &span), FRC_OK);
        CHECK_EQ_U64(span.blinks, (uint64_t)(to - k) * BLINKS_PER_SAROS);
        CHECK_EQ_U64(span.frac, 0);
        CHECK_EQ_U64(span.negative, 0);
    }
    /* The closing instant sits at position count - 1 exactly, although the
     * clock reads it one microsecond early: the whole catalogue is 69 Saros. */
    CHECK_EQ_I64(frc_span_between(CAT, first, last, &span), FRC_OK);
    CHECK_EQ_U64(span.blinks, UINT64_C(69) * BLINKS_PER_SAROS);
    CHECK_EQ_U64(span.frac, 0);
    CHECK_EQ_U64(span.negative, 0);
    expect_span_text(span, 2, "17\xCE\xB1 1\xCE\xB2");
    expect_span_text(span, 9, "17\xCE\xB1 1\xCE\xB2 3\xCE\xB3 1\xCE\xB4 1" EPS " 19" ETA " 9" IOTA);
    CHECK_EQ_I64(frc_span_between(CAT, last, first, &span), FRC_OK);
    CHECK_EQ_U64(span.blinks, UINT64_C(69) * BLINKS_PER_SAROS);
    CHECK_EQ_U64(span.negative, 1);
    expect_span_text(span, 2, MINUS "17\xCE\xB1 1\xCE\xB2");
    /* the last microsecond is a real, tiny span: 1 us of the 311 ms Blink */
    CHECK_EQ_I64(frc_span_between(CAT, last - 1, last, &span), FRC_OK);
    CHECK_EQ_U64(span.blinks, 0);
    CHECK(span.frac > 13000 && span.frac < 14500); /* 2^32 / 311,000 = 13,810 */
    CHECK_EQ_I64(frc_span_between(CAT, first, first + 1, &other), FRC_OK);
    CHECK(other.frac > 13000 && other.frac < 14500);
    /* a span to the closing instant continues the spans just before it */
    CHECK_EQ_I64(frc_span_between(CAT, last - 2000000, last - 1, &span), FRC_OK);
    CHECK_EQ_I64(frc_span_between(CAT, last - 2000000, last, &other), FRC_OK);
    CHECK(as_fixed(&other) > as_fixed(&span) && as_fixed(&other) - as_fixed(&span) < 14500);
    CHECK_EQ_I64(frc_span_between(CAT, last, last, &span), FRC_OK);
    CHECK(span.blinks == 0 && span.frac == 0 && span.negative == 0);

    TEST_SECTION("frc_span_between: the zero span");
    CHECK_EQ_I64(frc_span_between(CAT, first, first, &span), FRC_OK);
    CHECK(span.blinks == 0 && span.frac == 0 && span.negative == 0);
    expect_span_text(span, 2, "0" NU);

    TEST_SECTION("frc_span_between: outside the catalogue, arguments");
    memset(&span, 0xEE, sizeof span);
    CHECK_EQ_I64(frc_span_between(CAT, first - 1, last, &span), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_span_between(CAT, first, last + 1, &span), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_span_between(CAT, last + 1, first, &span), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_span_between(CAT, INT64_MIN, INT64_MAX, &span), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_span_between(CAT, INT64_MAX, INT64_MIN, &span), FRC_ERR_RANGE);
    CHECK(span.negative == 0xEE && span.frac == 0xEEEEEEEEu); /* untouched on failure */
    CHECK_EQ_I64(frc_span_between(NULL, first, last, &span), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_span_between(CAT, first, last, NULL), FRC_ERR_ARG);

    TEST_SECTION("frc_span_between: against the Blink index and phase");
    for (i = 0; i < 20000; i++) {
        frc_time_t t1 = rng_instant();
        /* half of the pairs close together (same interval, same Flavon),
         * half anywhere in the catalogue */
        frc_time_t t2 = (i % 2) ? rng_instant() : t1 + (frc_time_t)(rng() % 20000000);
        frc_cycle_t c1, c2;
        frc_span_t forward;
        uint64_t whole, low, high;
        int64_t fraction, off;

        if (t2 >= last) {
            t2 = last - 1;
        }
        if (t2 < t1) {
            frc_time_t swap = t1;
            t1 = t2;
            t2 = swap;
        }
        CHECK_EQ_I64(frc_span_between(CAT, t1, t2, &forward), FRC_OK);
        CHECK_EQ_U64(forward.negative, 0);
        CHECK_EQ_I64(frc_span_between(CAT, t2, t1, &other), FRC_OK);
        CHECK(other.blinks == forward.blinks && other.frac == forward.frac);
        CHECK_EQ_U64(other.negative, t1 != t2);

        /* "measured in position via the Blink index and the Blink phase at
         * both instants": (cycle2 + phase2) - (cycle1 + phase1). Each phase
         * is floored to 2^-32, so the exact span lies strictly between that
         * difference minus one unit and plus one unit; its floor is the
         * difference or one less. The span is that floor, except
         *   - one unit more where a rounding mark lies in its last unit, and
         *   - on the time grain: a whole number of quarter Blinks that the
         *     exact span comes within one microsecond of (below it by less
         *     than a microsecond at t1, above it by less than one at t2). */
        CHECK_EQ_I64(frc_cycle_at(CAT, t1, 12, &c1), FRC_OK);
        CHECK_EQ_I64(frc_cycle_at(CAT, t2, 12, &c2), FRC_OK);
        whole = c2.cycle - c1.cycle;
        fraction = (int64_t)c2.phase - (int64_t)c1.phase;
        if (fraction < 0) {
            whole -= 1;
            fraction += (int64_t)FRAC_ONE;
        }
        high = whole;
        low = (uint64_t)fraction;
        off = units_between(forward.blinks, forward.frac, high, low);
        if (on_quarter_grid(&forward)) {
            CHECK(off >= -(int64_t)grain_units(CAT, t2) && off <= (int64_t)grain_units(CAT, t1));
        } else {
            CHECK(off == 0 || off == -1 || (off == 1 && mark_in_last_unit(&forward)));
        }
        CHECK_NOTE("%" PRId64 " -> %" PRId64 ": span %" PRIu64 " + %u, from the cycles %" PRIu64 " + %" PRIu64, t1,
                   t2, forward.blinks, forward.frac, high, low);
    }

    TEST_SECTION("frc_span_between: spans add up");
    for (i = 0; i < 20000; i++) {
        frc_time_t t[3];
        frc_span_t ab, bc, ac;
        uint64_t sum_blinks, sum_frac;
        int a, b;

        t[0] = rng_instant();
        t[1] = (i % 2) ? rng_instant() : t[0] + (frc_time_t)(rng() % 5000000);
        t[2] = (i % 4 < 2) ? rng_instant() : t[1] + (frc_time_t)(rng() % 5000000);
        for (a = 0; a < 3; a++) {
            if (t[a] > last) {
                t[a] = last;
            }
        }
        for (a = 0; a < 3; a++) {
            for (b = a + 1; b < 3; b++) {
                if (t[b] < t[a]) {
                    frc_time_t swap = t[a];
                    t[a] = t[b];
                    t[b] = swap;
                }
            }
        }
        CHECK_EQ_I64(frc_span_between(CAT, t[0], t[1], &ab), FRC_OK);
        CHECK_EQ_I64(frc_span_between(CAT, t[1], t[2], &bc), FRC_OK);
        CHECK_EQ_I64(frc_span_between(CAT, t[0], t[2], &ac), FRC_OK);
        /* floor(x) + floor(y) <= floor(x + y) <= floor(x) + floor(y) + 1, in
         * units of 2^-32. A fraction with a rounding mark in its last unit
         * may be one unit above its floor, and a span on the time grain is
         * within a microsecond of position of the exact one. */
        sum_blinks = ab.blinks + bc.blinks;
        sum_frac = (uint64_t)ab.frac + bc.frac;
        if (sum_frac >= FRAC_ONE) {
            sum_frac -= FRAC_ONE;
            sum_blinks += 1;
        }
        {
            int64_t off = units_between(ac.blinks, ac.frac, sum_blinks, sum_frac);
            int64_t grain = (int64_t)grain_units(CAT, t[0]) + (int64_t)grain_units(CAT, t[2]);
            if (on_quarter_grid(&ab) || on_quarter_grid(&bc) || on_quarter_grid(&ac)) {
                CHECK(off >= -2 * grain && off <= 2 * grain);
            } else if (mark_in_last_unit(&ab) || mark_in_last_unit(&bc) || mark_in_last_unit(&ac)) {
                CHECK(off >= -2 && off <= 2);
            } else {
                CHECK(off == 0 || off == 1);
            }
        }
        CHECK_NOTE("%" PRId64 " %" PRId64 " %" PRId64, t[0], t[1], t[2]);
    }

    TEST_SECTION("frc_span_between: a catalogue of its own");
    {
        /* Intervals of 1 s and 3 s. Position is linear inside each: half way
         * through either one is half a Saros. */
        static const int64_t ODD[3] FRC_PROGMEM = {1000, 2000, 5000};
        frc_catalogue_t odd;
        CHECK_EQ_I64(frc_catalogue_init(&odd, ODD, 3, 0), FRC_OK);
        CHECK_EQ_I64(frc_span_between(&odd, FRC_TIME_FROM_MS(1500), FRC_TIME_FROM_MS(3500), &span), FRC_OK);
        CHECK_EQ_U64(span.blinks, BLINKS_PER_SAROS); /* 0.5 + 0.5 Saros */
        CHECK_EQ_U64(span.frac, 0);
        CHECK_EQ_I64(frc_span_between(&odd, FRC_TIME_FROM_MS(1250), FRC_TIME_FROM_MS(2750), &span), FRC_OK);
        /* 0.75 + 0.25 Saros */
        CHECK_EQ_U64(span.blinks, BLINKS_PER_SAROS);
        CHECK_EQ_U64(span.frac, 0);
        /* 0.9 of the first interval to 1/3 of the second: 0.1 + 1/3 Saros =
         * 13/30 of 1,829,953,125 = 792,979,687.5 Blinks */
        CHECK_EQ_I64(frc_span_between(&odd, FRC_TIME_FROM_MS(1900), FRC_TIME_FROM_MS(3000), &span), FRC_OK);
        CHECK_EQ_U64(span.blinks, 792979687);
        CHECK_EQ_U64(span.frac, 0x80000000u);
        CHECK_EQ_I64(frc_span_between(&odd, FRC_TIME_FROM_MS(1000), FRC_TIME_FROM_MS(5000), &span), FRC_OK);
        CHECK_EQ_U64(span.blinks, 2 * BLINKS_PER_SAROS);
        CHECK_EQ_U64(span.frac, 0);
    }

    TEST_SECTION("frc_span_between: intervals as long as the int64 range");
    {
        /* Two intervals of 4.6e18 us, just under 2^63: the two fractions still
         * have a common denominator in 128 bits, and the span is exact. */
        static const int64_t HALVES_MS[3] FRC_PROGMEM = {-INT64_C(4611686018427387), 0, INT64_C(4611686018427387)};
        /* One interval of 1.8e19 us, the whole range: 2^63 or more. The span
         * cannot be measured exactly and says so; nothing is written. */
        static const int64_t WHOLE_MS[2] FRC_PROGMEM = {-(INT64_MAX / 1000), INT64_MAX / 1000};
        frc_catalogue_t wide;
        frc_time_t half = INT64_C(2305843009213693500); /* half of one interval, in us */

        CHECK_EQ_I64(frc_catalogue_init(&wide, HALVES_MS, 3, 0), FRC_OK);
        CHECK_EQ_I64(frc_span_between(&wide, frc_catalogue_first(&wide), frc_catalogue_last(&wide), &span), FRC_OK);
        CHECK(span.negative == 0 && span.blinks == 2 * BLINKS_PER_SAROS && span.frac == 0);
        CHECK_EQ_I64(frc_span_between(&wide, -half, 0, &span), FRC_OK); /* half a Saros: 914,976,562.5 Blinks */
        CHECK(span.blinks == BLINKS_PER_SAROS / 2 && span.frac == 0x80000000u);
        CHECK_EQ_I64(frc_span_between(&wide, half, -half, &span), FRC_OK);
        CHECK(span.negative == 1 && span.blinks == BLINKS_PER_SAROS && span.frac == 0);
        /* three quarters of one interval and a quarter of the other, across the eclipse */
        CHECK_EQ_I64(frc_span_between(&wide, -half - half / 2, half / 2, &span), FRC_OK);
        CHECK(span.blinks == BLINKS_PER_SAROS && span.frac == 0);

        CHECK_EQ_I64(frc_catalogue_init(&wide, WHOLE_MS, 2, 0), FRC_OK);
        memset(&span, 0xEE, sizeof span);
        CHECK_EQ_I64(frc_span_between(&wide, 0, 1000000, &span), FRC_ERR_OVERFLOW);
        CHECK_EQ_I64(frc_span_between(&wide, 1000000, 0, &span), FRC_ERR_OVERFLOW);
        CHECK_EQ_I64(frc_span_between(&wide, frc_catalogue_first(&wide), frc_catalogue_last(&wide), &span),
                     FRC_ERR_OVERFLOW);
        CHECK_EQ_I64(frc_span_between(&wide, 0, 0, &span), FRC_ERR_OVERFLOW);
        CHECK(span.negative == 0xEE && span.frac == 0xEEEEEEEEu); /* untouched on failure */
        /* an argument or range error still comes first */
        CHECK_EQ_I64(frc_span_between(&wide, 0, INT64_MAX, &span), FRC_ERR_RANGE);
    }
}

/* ------------------------------------------------------------------------- */
/* The time grain: spans between instants the API returned                    */
/* ------------------------------------------------------------------------- */

/* The span from t1 to t2 must be exactly `blinks` + `frac`, forward and back. */
static void expect_exact_span(const frc_catalogue_t *cat, frc_time_t t1, frc_time_t t2, uint64_t blinks,
                              uint32_t frac, const char *what, int level)
{
    frc_span_t span, back;

    CHECK_EQ_I64(frc_span_between(cat, t1, t2, &span), FRC_OK);
    CHECK(span.negative == 0 && span.blinks == blinks && span.frac == frac);
    CHECK_NOTE("%s, level %d: %" PRId64 " -> %" PRId64 " reads %" PRIu64 " + %u / 2^32 Blinks, expected %" PRIu64
               " + %u", what, level, t1, t2, span.blinks, span.frac, blinks, frac);
    CHECK_EQ_I64(frc_span_between(cat, t2, t1, &back), FRC_OK);
    CHECK(back.negative == 1 && back.blinks == blinks && back.frac == frac);
}

/* frc_date_to_time of a stamp. */
static frc_time_t time_of_stamp(const char *stamp)
{
    frc_date_t date;
    frc_time_t t = FRC_TIME_NONE;
    CHECK_EQ_I64(frc_parse_date(stamp, NULL, &date, NULL), FRC_OK);
    CHECK_EQ_I64(frc_date_to_time(CAT, &date, &t), FRC_OK);
    return t;
}

/* Cycles of `level` from `first`, `count` in a row: each one's start -> end
 * is the level's length, and each quarter (threshold to threshold) a quarter
 * of it, although every instant is a boundary rounded up to the microsecond. */
static void check_cycles_on_the_grain(const frc_catalogue_t *cat, uint8_t level, uint64_t first, uint64_t count)
{
    frc_span_t length;
    char want[64], got[64];
    uint64_t c, cycles = frc_cycle_count(cat, level);
    uint64_t quarter_blinks;
    uint32_t quarter_frac;

    CHECK_EQ_I64(frc_span_of_level(level, &length), FRC_OK);
    CHECK(frc_format_span(&length, 2, want, sizeof want) > 0);
    /* a quarter of B Blinks is B quarter Blinks: B / 4 whole and B mod 4 quarters */
    quarter_blinks = length.blinks / 4;
    quarter_frac = (uint32_t)(length.blinks % 4) << 30;

    for (c = first; c < first + count && c < cycles; c++) {
        frc_cycle_t cycle;
        frc_time_t points[5];
        frc_span_t span;
        int q;

        CHECK_EQ_I64(frc_cycle_by_index(cat, level, c, &cycle), FRC_OK);
        if (cycle.clipped) {
            continue; /* the catalogue ends inside it: not a whole cycle */
        }
        expect_exact_span(cat, cycle.start, cycle.end, length.blinks, 0, "cycle start -> end", level);
        CHECK_EQ_I64(frc_span_between(cat, cycle.start, cycle.end, &span), FRC_OK);
        CHECK(frc_format_span(&span, 2, got, sizeof got) > 0);
        CHECK_STR(got, want);
        CHECK_EQ_I64(frc_cycle_points(cat, level, c, points), FRC_OK);
        for (q = 0; q < 4; q++) {
            expect_exact_span(cat, points[q], points[q + 1], quarter_blinks, quarter_frac, "quarter", level);
        }
        expect_exact_span(cat, points[1], points[3], length.blinks / 2, (uint32_t)(length.blinks % 2) << 31,
                          "peak -> valley", level);
    }
}

static void test_time_grain(void)
{
    static const int64_t UNEVEN_MS[6] FRC_PROGMEM = {
        INT64_C(-1000000000000), INT64_C(-431000000123), INT64_C(138000000777),
        INT64_C(707999999001),   INT64_C(1277000000456), INT64_C(1845999999999)};
    frc_catalogue_t uneven;
    frc_cycle_t at_a;
    frc_span_t span;
    frc_threshold_t th, next;
    frc_time_t t, t1, t2;
    uint8_t level;
    uint16_t k;
    int i, misses;
    char text[64];

    TEST_SECTION("time grain: adjacent dates are one unit apart");
    /* The stamps of SPEC vector A's Preon and the next one. Each instant is
     * the first microsecond of its Preon; measured without the grain the pair
     * is 3 * 10^-7 Blink short of 1625 and reads "64λ 24ν". */
    t1 = time_of_stamp("\xCE\xB1#6 \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9");
    t2 = time_of_stamp("\xCE\xB1#6 \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 \xCE\xB5#5 34\xCE\xB7 30\xCE\xB9");
    CHECK_EQ_I64(t1, INT64_C(1791460635689016));
    CHECK_EQ_I64(t2, INT64_C(1791461140937436));
    expect_exact_span(CAT, t1, t2, 1625, 0, "29 Preons -> 30 Preons", 8);
    CHECK_EQ_I64(frc_span_between(CAT, t1, t2, &span), FRC_OK);
    expect_span_text(span, 2, "1" IOTA);
    /* one Helion on, and one Blink on */
    t2 = time_of_stamp("\xCE\xB1#6 \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 \xCE\xB5#5 35\xCE\xB7 29\xCE\xB9");
    expect_exact_span(CAT, t1, t2, 58500, 0, "34th Helion -> 35th Helion", 6);
    t2 = time_of_stamp("\xCE\xB1#6 \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9 0\xCE\xBB 1\xCE\xBD");
    expect_exact_span(CAT, t1, t2, 1, 0, "one Blink on", 12);

    TEST_SECTION("time grain: a cycle's start -> end is its length (levels 0..12)");
    for (level = 0; level <= 12; level++) {
        uint64_t cycles = frc_cycle_count(CAT, level);
        /* 2000 in a row from the cycle of SPEC vector A, and 500 spread over
         * the whole catalogue (every eclipse interval has its own Blink) */
        CHECK_EQ_I64(frc_cycle_at(CAT, FRC_TIME_FROM_MS(INT64_C(1791460800000)), level, &at_a), FRC_OK);
        check_cycles_on_the_grain(CAT, level, at_a.cycle, 2000);
        check_cycles_on_the_grain(CAT, level, 0, 20);
        for (i = 0; i < 500; i++) {
            check_cycles_on_the_grain(CAT, level, rng() % cycles, 1);
        }
    }

    TEST_SECTION("time grain: waking at each threshold, the next one is a quarter away");
    /* the pattern fractonica.h recommends: sleep until the threshold, read again */
    for (level = 0; level <= 12; level++) {
        frc_span_t length;
        CHECK_EQ_I64(frc_span_of_level(level, &length), FRC_OK);
        CHECK_EQ_I64(frc_next_threshold(CAT, FRC_TIME_FROM_MS(INT64_C(1791460800000)), level, &th), FRC_OK);
        for (i = 0; i < 400 && th.time != FRC_TIME_NONE; i++) {
            CHECK_EQ_I64(frc_next_threshold(CAT, th.time, level, &next), FRC_OK);
            if (next.time == FRC_TIME_NONE) {
                break;
            }
            expect_exact_span(CAT, th.time, next.time, length.blinks / 4, (uint32_t)(length.blinks % 4) << 30,
                              "threshold -> next threshold", level);
            th = next;
        }
    }
    /* the Helion threshold of the review: a quarter Helion is exactly 9 Preons */
    CHECK_EQ_I64(frc_next_threshold(CAT, INT64_C(1791464172427957), 6, &th), FRC_OK);
    CHECK_EQ_I64(th.time, INT64_C(1791468719663737));
    CHECK_EQ_I64(frc_span_between(CAT, INT64_C(1791464172427957), th.time, &span), FRC_OK);
    expect_span_text(span, 2, "9" IOTA);

    TEST_SECTION("time grain: syncs and Pentads");
    {
        frc_sync_t sync;
        frc_pentad_t pentad;
        int32_t number;
        uint8_t index;
        for (number = 4800; number < 4900; number++) {
            CHECK_EQ_I64(frc_sync_by_number(CAT, number, &sync), FRC_OK);
            expect_exact_span(CAT, sync.start, sync.end, UINT64_C(19) * 58500, 0, "sync", 6);
            for (index = 0; index < 4; index++) {
                CHECK_EQ_I64(frc_pentad_by_number(CAT, number, index, &pentad), FRC_OK);
                expect_exact_span(CAT, pentad.start, pentad.end, (uint64_t)pentad.helions * 58500, 0, "Pentad", 6);
            }
        }
        CHECK_EQ_I64(frc_sync_by_number(CAT, 4820, &sync), FRC_OK);
        CHECK_EQ_I64(frc_span_between(CAT, sync.start, sync.end, &span), FRC_OK);
        expect_span_text(span, 2, "19" ETA);
    }

    TEST_SECTION("time grain: another catalogue, with uneven intervals");
    CHECK_EQ_I64(frc_catalogue_init(&uneven, UNEVEN_MS, 6, 0), FRC_OK);
    for (level = 0; level <= 12; level++) {
        uint64_t cycles = frc_cycle_count(&uneven, level);
        check_cycles_on_the_grain(&uneven, level, 0, 300);
        check_cycles_on_the_grain(&uneven, level, cycles / 2, 300);
        check_cycles_on_the_grain(&uneven, level, cycles > 300 ? cycles - 300 : 0, 300);
    }

    TEST_SECTION("time grain: what it leaves alone");
    /* A level below the Blink is not on the quarter-Blink grid: its cycle of
     * 1/7 Blink is measured as it is, to within a microsecond at each end. */
    CHECK_EQ_I64(frc_cycle_at(CAT, FRC_TIME_FROM_MS(INT64_C(1791460800000)), 13, &at_a), FRC_OK);
    misses = 0;
    for (i = 0; i < 2000; i++) {
        frc_cycle_t c;
        frc_span_t length;
        int64_t off;
        CHECK_EQ_I64(frc_cycle_by_index(CAT, 13, at_a.cycle + (uint64_t)i, &c), FRC_OK);
        CHECK_EQ_I64(frc_span_between(CAT, c.start, c.end, &span), FRC_OK);
        CHECK_EQ_I64(frc_span_of_level(13, &length), FRC_OK);
        off = units_between(span.blinks, span.frac, length.blinks, length.frac);
        CHECK(off > -(int64_t)grain_units(CAT, c.start) - 2 && off < (int64_t)grain_units(CAT, c.end) + 2);
        misses += (off != 0);
        expect_span_text(span, 2, "0.14" NU);
    }
    CHECK(misses > 1000); /* the rounded instants do show there */
    /* One microsecond is not "within a microsecond": it stays a span, on
     * both sides of every eclipse and across it. The microsecond that ends AT
     * an eclipse lies in the interval before it, so that interval's
     * microsecond is the eclipse's grain: no span between two different
     * instants is brought to nothing (and so none loses its direction). */
    for (k = 1; k < CAT->count; k++) {
        frc_time_t e = frc_catalogue_instant(CAT, k);
        CHECK_EQ_I64(frc_span_between(CAT, e - 2, e - 1, &span), FRC_OK);
        CHECK(span.blinks == 0 && span.frac > 13000 && span.frac < 14500);
        CHECK_EQ_I64(frc_span_between(CAT, e - 1, e, &span), FRC_OK);
        CHECK(span.blinks == 0 && span.frac > 13000 && span.frac < 14500 && span.negative == 0);
        CHECK_EQ_I64(frc_span_between(CAT, e, e - 1, &span), FRC_OK);
        CHECK(span.blinks == 0 && span.frac > 13000 && span.frac < 14500 && span.negative == 1);
        if (k + 1 == CAT->count) {
            break; /* nothing after the closing instant */
        }
        CHECK_EQ_I64(frc_span_between(CAT, e, e + 1, &span), FRC_OK);
        CHECK(span.blinks == 0 && span.frac > 13000 && span.frac < 14500);
        CHECK_EQ_I64(frc_span_between(CAT, e - 1, e + 1, &span), FRC_OK);
        CHECK(span.blinks == 0 && span.frac > 26000 && span.frac < 29000);
        /* a Saros and one microsecond of the SAME interval as the later
         * instant's own grain: exactly on the edge of the window, not in it */
        CHECK_EQ_I64(frc_span_between(CAT, frc_catalogue_instant(CAT, (uint16_t)(k - 1)), e + 1, &span), FRC_OK);
        CHECK(span.blinks == BLINKS_PER_SAROS && span.frac > 13000 && span.frac < 14500);
        /* A Saros and one microsecond of the interval BEFORE it: the grain of
         * the later instant (an eclipse) is a microsecond of the interval
         * that ends there, so this span is inside the window exactly when
         * that microsecond is the longer one, i.e. that interval the shorter. */
        {
            frc_interval_t before, running;
            CHECK_EQ_I64(frc_interval_at(CAT, e - 1, &before), FRC_OK);
            CHECK_EQ_I64(frc_interval_at(CAT, e, &running), FRC_OK);
            CHECK_EQ_I64(frc_span_between(CAT, e - 1, frc_catalogue_instant(CAT, (uint16_t)(k + 1)), &span), FRC_OK);
            if (running.num < before.num) {
                CHECK(span.blinks == BLINKS_PER_SAROS && span.frac == 0);
            } else {
                CHECK(span.blinks == BLINKS_PER_SAROS && span.frac > 13000 && span.frac < 14500);
            }
        }
    }
    /* Every pair of different instants around an eclipse has a non-zero span
     * with a direction: on a table whose neighbouring intervals are 9.5 and
     * 28.5 years (a microsecond is three times longer on one side), and on
     * one with intervals of 1 s and 3 s, where a microsecond is longer than a
     * quarter Blink. */
    {
        static const int64_t LOPSIDED_MS[4] FRC_PROGMEM = {0, INT64_C(300000000000), INT64_C(1200000000000),
                                                           INT64_C(1500000000000)};
        static const int64_t SECONDS_MS[4] FRC_PROGMEM = {1000, 2000, 5000, 6000};
        frc_catalogue_t table[2];
        int which, a, b;
        CHECK_EQ_I64(frc_catalogue_init(&table[0], LOPSIDED_MS, 4, 0), FRC_OK);
        CHECK_EQ_I64(frc_catalogue_init(&table[1], SECONDS_MS, 4, 0), FRC_OK);
        for (which = 0; which < 2; which++) {
            for (k = 1; k + 1 < table[which].count; k++) {
                frc_time_t e = frc_catalogue_instant(&table[which], k);
                for (a = -3; a <= 3; a++) {
                    for (b = a + 1; b <= 3; b++) {
                        frc_span_t back;
                        CHECK_EQ_I64(frc_span_between(&table[which], e + a, e + b, &span), FRC_OK);
                        CHECK_EQ_I64(frc_span_between(&table[which], e + b, e + a, &back), FRC_OK);
                        CHECK((span.blinks != 0 || span.frac != 0) && span.negative == 0);
                        CHECK(back.blinks == span.blinks && back.frac == span.frac && back.negative == 1);
                        CHECK_NOTE("table %d, eclipse %u, %d us -> %d us", which, k, a, b);
                    }
                }
            }
        }
        /* the eclipses stay whole Saros apart on both */
        CHECK_EQ_I64(frc_span_between(&table[0], frc_catalogue_instant(&table[0], 1),
                                      frc_catalogue_instant(&table[0], 3), &span), FRC_OK);
        CHECK(span.blinks == 2 * BLINKS_PER_SAROS && span.frac == 0);
        CHECK_EQ_I64(frc_span_between(&table[1], frc_catalogue_instant(&table[1], 0),
                                      frc_catalogue_instant(&table[1], 2), &span), FRC_OK);
        CHECK(span.blinks == 2 * BLINKS_PER_SAROS && span.frac == 0);
    }
    /* a millisecond is a thousand grains: a span 1 ms short of a Preon stays short */
    t = time_of_stamp("\xCE\xB1#6 \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9");
    CHECK_EQ_I64(frc_span_between(CAT, t, t + 505248420 - 1000, &span), FRC_OK);
    CHECK(frc_format_span(&span, 2, text, sizeof text) > 0);
    CHECK_STR(text, "64" LAMBDA " 24" NU);
    /* The window is one microsecond wide on each side, no more. This pair
     * measures 0.024 us under the Preon (the review's "frac 4294966958"):
     * one microsecond earlier it is 1.024 us under and stays short; one
     * later it is 0.976 us over and is still the Preon; two later it is not. */
    CHECK_EQ_I64(frc_span_between(CAT, t, t + 505248420 - 1, &span), FRC_OK);
    CHECK(span.blinks == 1624 && span.frac > 0xFFFF0000u);
    CHECK_EQ_I64(frc_span_between(CAT, t, t + 505248420, &span), FRC_OK);
    CHECK(span.blinks == 1625 && span.frac == 0);
    CHECK_EQ_I64(frc_span_between(CAT, t, t + 505248420 + 1, &span), FRC_OK);
    CHECK(span.blinks == 1625 && span.frac == 0);
    CHECK_EQ_I64(frc_span_between(CAT, t, t + 505248420 + 2, &span), FRC_OK);
    CHECK(span.blinks == 1625 && span.frac > 13000 && span.frac < 29000);
}

/* ------------------------------------------------------------------------- */
/* Exact rounding: a table on which ties and thresholds are exact             */
/* ------------------------------------------------------------------------- */

/* The text of n / d Blinks (below 10), by the rule of fractonica.h, in exact
 * integer arithmetic: an independent copy of the rule, not of the code. */
static void exact_blink_text(uint64_t n, uint64_t d, char *out, size_t size)
{
    uint64_t scaled;
    int decimals;
    char digits[32];
    size_t len;

    if (n == 0) {
        snprintf(out, size, "0" NU); /* exactly nothing, and only that */
        return;
    }
    if (n * 1000 < d) {
        snprintf(out, size, "<0.001" NU);
        return;
    }
    /* from 1 Blink one decimal; below, two significant digits */
    decimals = (n >= d) ? 1 : (n * 10 >= d) ? 2 : (n * 100 >= d) ? 3 : 4;
    scaled = n;
    for (int i = 0; i < decimals; i++) {
        scaled *= 10;
    }
    scaled = (2 * scaled + d) / (2 * d); /* a half goes up */
    snprintf(digits, sizeof digits, "%0*" PRIu64, decimals + 1, scaled);
    len = strlen(digits);
    /* insert the point, then drop trailing zeros and a bare point */
    snprintf(out, size, "%.*s.%s", (int)(len - (size_t)decimals), digits, digits + len - (size_t)decimals);
    len = strlen(out);
    while (out[len - 1] == '0') {
        out[--len] = '\0';
    }
    if (out[len - 1] == '.') {
        out[--len] = '\0';
    }
    snprintf(out + len, size - len, NU);
}

static void test_exact_rounding(void)
{
    /* Two intervals of 36,599,062,500 ms: a Blink is exactly 20 ms there, so
     * one microsecond is 1/20000 Blink and every rounding tie and threshold
     * of frc_format_span is an instant. (Saros 141 has no such instant.) */
    static const int64_t TIES_MS[3] FRC_PROGMEM = {0, INT64_C(36599062500), INT64_C(73198125000)};
    frc_catalogue_t ties;
    frc_span_t span;
    char want[32], got[32];
    frc_time_t dt, base;
    int k;

    TEST_SECTION("exact rounding: ties and thresholds are read from the exact span");
    CHECK_EQ_I64(frc_catalogue_init(&ties, TIES_MS, 3, 0), FRC_OK);
    /* the cases of the review, typed in */
    CHECK_EQ_I64(frc_span_between(&ties, 0, 21000, &span), FRC_OK); /* 1.05 */
    expect_span_text(span, 2, "1.1" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 0, 2900, &span), FRC_OK); /* 0.145 */
    expect_span_text(span, 2, "0.15" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 0, 20, &span), FRC_OK); /* 1/1000: not below it */
    expect_span_text(span, 2, "0.001" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 0, 19, &span), FRC_OK);
    expect_span_text(span, 2, "<0.001" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 0, 2, &span), FRC_OK); /* 1/10000 */
    expect_span_text(span, 2, "<0.001" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 0, 1, &span), FRC_OK); /* 1/20000: not zero */
    expect_span_text(span, 2, "<0.001" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 1, 0, &span), FRC_OK); /* and without a sign */
    expect_span_text(span, 2, "<0.001" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 19, 0, &span), FRC_OK);
    expect_span_text(span, 2, "<0.001" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 20, 0, &span), FRC_OK);
    expect_span_text(span, 2, MINUS "0.001" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 7, 7, &span), FRC_OK);  /* only an exact zero */
    expect_span_text(span, 2, "0" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 0, 50000, &span), FRC_OK); /* 2.5, exactly representable */
    CHECK(span.blinks == 2 && span.frac == 0x80000000u);
    expect_span_text(span, 2, "2.5" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 0, 199000, &span), FRC_OK); /* 9.95 */
    expect_span_text(span, 2, "10" NU);
    CHECK_EQ_I64(frc_span_between(&ties, 21000, 0, &span), FRC_OK);
    expect_span_text(span, 2, MINUS "1.1" NU);

    /* every span below 10 Blinks, from three starts (one across the eclipse) */
    for (k = 0; k < 3; k++) {
        base = (k == 0) ? 0 : (k == 1) ? FRC_TIME_FROM_MS(INT64_C(36599062500)) - 100003 : 777777777;
        for (dt = 0; dt < 200000; dt++) {
            CHECK_EQ_I64(frc_span_between(&ties, base, base + dt, &span), FRC_OK);
            /* the stored span is within one unit of the exact dt / 20000 */
            CHECK(span.blinks == (uint64_t)dt / 20000);
            CHECK(frc_format_span(&span, 2, got, sizeof got) > 0);
            exact_blink_text((uint64_t)dt, 20000, want, sizeof want);
            CHECK_STR(got, want);
            CHECK_NOTE("start %" PRId64 ", %" PRId64 " us = %" PRId64 "/20000 Blink", base, dt, dt);
        }
    }

    TEST_SECTION("exact rounding: the fraction of a level's length");
    {
        /* no level's length sits on a mark, so each is the plain floor */
        frc_span_t length;
        uint8_t level;
        for (level = 13; level < FRC_LEVEL_COUNT; level++) {
            CHECK_EQ_I64(frc_span_of_level(level, &length), FRC_OK);
            CHECK(length.blinks == 0 && !mark_in_last_unit(&length));
        }
    }
}

/* ------------------------------------------------------------------------- */
/* The one cut-off below 1/1000 Blink (fractonica.h, frc_format_span)          */
/* ------------------------------------------------------------------------- */

static void test_cutoff(void)
{
    /* At S12 vector A one microsecond is 3.216e-6 Blink: 310 us is 9.97e-4
     * and 311 us 1.0003e-3. Every non-zero span below 1/1000 Blink reads
     * "<0.001ν" whatever its sign, and only an exact zero reads "0ν". */
    static const struct { int64_t us; const char *forward; const char *back; } cases[] = {
        {0, "0" NU, "0" NU},
        {1, "<0.001" NU, "<0.001" NU},
        {20, "<0.001" NU, "<0.001" NU},
        {244, "<0.001" NU, "<0.001" NU},
        {310, "<0.001" NU, "<0.001" NU},
        {311, "0.001" NU, MINUS "0.001" NU},
        {400, "0.0013" NU, MINUS "0.0013" NU},
    };
    const frc_time_t a = FRC_TIME_FROM_S(INT64_C(1791460800));
    frc_span_t span;
    size_t i;
    int64_t us;
    char text[32];

    TEST_SECTION("frc_format_span: the cut-off below 1/1000 Blink at vector A");
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK_EQ_I64(frc_span_between(CAT, a, a + cases[i].us, &span), FRC_OK);
        expect_span_text(span, 2, cases[i].forward);
        CHECK_EQ_I64(frc_span_between(CAT, a + cases[i].us, a, &span), FRC_OK);
        expect_span_text(span, 2, cases[i].back);
    }
    /* one rule, no gap: every span of 1..310 us, both ways, is "<0.001ν" */
    for (us = 1; us <= 310; us++) {
        CHECK_EQ_I64(frc_span_between(CAT, a, a + us, &span), FRC_OK);
        CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
        CHECK_STR(text, "<0.001" NU);
        CHECK_EQ_I64(frc_span_between(CAT, a + us, a, &span), FRC_OK);
        CHECK(frc_format_span(&span, 0, text, sizeof text) > 0);
        CHECK_STR(text, "<0.001" NU);
        CHECK_NOTE("%" PRId64 " us", us);
    }
}

/* ------------------------------------------------------------------------- */
/* Parts and text                                                             */
/* ------------------------------------------------------------------------- */

static frc_span_t edge_span;
#define CALL_SPAN_2(b, s) frc_format_span(&edge_span, 2, (b), (s))
#define CALL_SPAN_9(b, s) frc_format_span(&edge_span, 9, (b), (s))

static void test_span_parts_and_text(void)
{
    frc_span_part_t parts[FRC_UNIT_COUNT + 2];
    frc_span_t span;
    char text[160];
    uint8_t n, max_parts;
    int i, j;

    TEST_SECTION("frc_span_parts: the parts add up");
    for (i = 0; i < 20000; i++) {
        uint64_t sum = 0;
        span = span_of((int)(rng() & 1), rng() >> (rng() % 64), (uint32_t)rng());
        for (max_parts = 1; max_parts <= FRC_UNIT_COUNT; max_parts++) {
            int zeros;
            for (zeros = 0; zeros < 2; zeros++) {
                memset(parts, 0xEE, sizeof parts);
                n = frc_span_parts(&span, max_parts, zeros, parts);
                CHECK(n >= 1 && n <= max_parts);
                CHECK(parts[n].unit == 0xEE);
                sum = 0;
                for (j = 0; j < n; j++) {
                    CHECK(parts[j].unit < FRC_UNIT_COUNT);
                    /* units descend the ladder; each count is below what makes the next unit up */
                    if (j > 0) {
                        CHECK(parts[j].unit > parts[j - 1].unit);
                        CHECK(parts[j].count < frc_unit_parts((frc_unit_t)(parts[j].unit - 1)));
                        if (zeros) {
                            CHECK(parts[j].unit == parts[j - 1].unit + 1);
                        } else {
                            CHECK(parts[j].count > 0);
                        }
                    }
                    sum += parts[j].count * frc_unit_blinks((frc_unit_t)parts[j].unit);
                }
                CHECK(sum <= span.blinks);
                /* what is left is less than one of the last unit counted */
                CHECK(span.blinks - sum < frc_unit_blinks((frc_unit_t)parts[n - 1].unit));
            }
        }
        /* all nine with zeros account for every Blink */
        n = frc_span_parts(&span, FRC_UNIT_COUNT, 1, parts);
        sum = 0;
        for (j = 0; j < n; j++) {
            sum += parts[j].count * frc_unit_blinks((frc_unit_t)parts[j].unit);
        }
        CHECK_EQ_U64(sum, span.blinks);
        CHECK_EQ_U64(parts[n - 1].unit, FRC_UNIT_BLINK);
    }

    TEST_SECTION("frc_span_parts by hand");
    /* SPEC S15.4: "a span reads 3η 5λ, never 3η 0ι 5λ" */
    span = span_of(0, 3 * 58500 + 5 * 25, 0);
    n = frc_span_parts(&span, 2, 0, parts);
    CHECK(n == 2 && parts[0].unit == FRC_UNIT_HELION && parts[0].count == 3 && parts[1].unit == FRC_UNIT_FLAVON &&
          parts[1].count == 5);
    n = frc_span_parts(&span, 2, 1, parts);
    CHECK(n == 2 && parts[0].unit == FRC_UNIT_HELION && parts[0].count == 3 && parts[1].unit == FRC_UNIT_PREON &&
          parts[1].count == 0);
    n = frc_span_parts(&span, 9, 1, parts);
    CHECK(n == 4 && parts[2].unit == FRC_UNIT_FLAVON && parts[2].count == 5 && parts[3].unit == FRC_UNIT_BLINK &&
          parts[3].count == 0);
    n = frc_span_parts(&span, 1, 0, parts);
    CHECK(n == 1 && parts[0].unit == FRC_UNIT_HELION && parts[0].count == 3);
    expect_span_text(span, 2, "3" ETA " 5" LAMBDA);
    /* a sub-Blink span is one part, 0 Blinks */
    span = span_of(0, 0, 0x40000000u);
    n = frc_span_parts(&span, 9, 0, parts);
    CHECK(n == 1 && parts[0].unit == FRC_UNIT_BLINK && parts[0].count == 0);
    n = frc_span_parts(&span, 9, 1, parts);
    CHECK(n == 1 && parts[0].unit == FRC_UNIT_BLINK && parts[0].count == 0);
    /* the Alpha is the top of the ladder: its count is unbounded */
    span = span_of(0, UINT64_MAX, 0);
    n = frc_span_parts(&span, 9, 0, parts);
    CHECK(n >= 1 && parts[0].unit == FRC_UNIT_ALPHA && parts[0].count == UINT64_MAX / UINT64_C(7319812500));
    /* a request for more than the ladder has is the whole ladder */
    span = span_of(0, UINT64_C(7319812500) - 1, 0);
    n = frc_span_parts(&span, 200, 1, parts);
    CHECK_EQ_U64(n, 8); /* β .. ν */
    CHECK(parts[0].unit == FRC_UNIT_BETA && parts[0].count == 4 && parts[7].unit == FRC_UNIT_BLINK && parts[7].count == 24);
    /* nothing to write into */
    memset(parts, 0xEE, sizeof parts);
    CHECK_EQ_U64(frc_span_parts(&span, 0, 0, parts), 0);
    CHECK(parts[0].unit == 0xEE);
    CHECK_EQ_U64(frc_span_parts(NULL, 2, 0, parts), 0);
    CHECK_EQ_U64(frc_span_parts(&span, 2, 0, NULL), 0);

    TEST_SECTION("frc_format_span by hand");
    /* fractonica.h: "6ι 43λ", "3η 5λ", "−2ε 5η", "2.6ν", "0.14ν" */
    expect_span_text(span_of(0, 6 * 1625 + 43 * 25 + 21, 0), 2, "6" IOTA " 43" LAMBDA);
    expect_span_text(span_of(1, 2 * 4504500 + 5 * 58500, 12345), 2, MINUS "2" EPS " 5" ETA);
    expect_span_text(span_of(0, 2, frac_of(6, 10) + 1), 2, "2.6" NU);
    expect_span_text(span_of(0, 0, frac_of(14, 100) + 1), 2, "0.14" NU);
    expect_span_text(span_of(1, 0, frac_of(14, 100) + 1), 2, MINUS "0.14" NU);
    /* the minus is U+2212, three bytes, and stands only before a non-zero
     * digit: never before "0ν" or "<0.001ν" */
    expect_span_text(span_of(1, 0, 0), 2, "0" NU);
    expect_span_text(span_of(1, 0, 1), 2, "<0.001" NU);
    expect_span_text(span_of(1, 0, 429496), 2, "<0.001" NU);
    expect_span_text(span_of(1, 5, 0), 2, MINUS "5" NU);

    /* pure.blinkCount, branch by branch (x = the count of Blinks), with the
     * core's one cut-off: x == 0 "0"; 0 < x < 0.001 "<0.001"; x < 1 two
     * significant digits; else one decimal. 2^32 / 10^3 = 4,294,967.3 */
    expect_span_text(span_of(0, 0, 0), 2, "0" NU);
    expect_span_text(span_of(0, 0, 1), 2, "<0.001" NU);
    expect_span_text(span_of(0, 0, 429496), 2, "<0.001" NU);
    expect_span_text(span_of(0, 0, 429497), 2, "<0.001" NU);
    expect_span_text(span_of(0, 0, 4294967), 2, "<0.001" NU);
    expect_span_text(span_of(0, 0, 4294968), 2, "0.001" NU);
    /* The reference puts a minus before "<0.001"; the core does not: the
     * text names no digit of the span, so it has no sign either. */
    expect_span_text(span_of(1, 0, 429497), 2, "<0.001" NU);
    expect_span_text(span_of(1, 0, 4294967), 2, "<0.001" NU);
    expect_span_text(span_of(1, 0, 4294968), 2, MINUS "0.001" NU);
    expect_span_text(span_of(0, 0, frac_of(1, 1000) + 1), 2, "0.001" NU);
    expect_span_text(span_of(0, 0, frac_of(123, 100000) + 1), 2, "0.0012" NU);
    expect_span_text(span_of(0, 0, frac_of(125, 100000) + 1), 2, "0.0013" NU);  /* a half goes up */
    expect_span_text(span_of(0, 0, frac_of(1249, 1000000)), 2, "0.0012" NU);
    expect_span_text(span_of(0, 0, frac_of(995, 100000) + 1), 2, "0.01" NU);    /* 0.00995 -> 0.010 */
    expect_span_text(span_of(0, 0, frac_of(1, 100) + 1), 2, "0.01" NU);
    expect_span_text(span_of(0, 0, frac_of(5, 100) + 1), 2, "0.05" NU);
    expect_span_text(span_of(0, 0, frac_of(996, 10000) + 1), 2, "0.1" NU);      /* 0.0996 -> 0.10 */
    expect_span_text(span_of(0, 0, frac_of(1, 10) + 1), 2, "0.1" NU);
    expect_span_text(span_of(0, 0, frac_of(1, 7)), 2, "0.14" NU);               /* a xi, S15.4 */
    expect_span_text(span_of(0, 0, frac_of(1, 2)), 2, "0.5" NU);
    expect_span_text(span_of(0, 0, frac_of(994, 1000)), 2, "0.99" NU);
    expect_span_text(span_of(0, 0, frac_of(995, 1000) + 1), 2, "1" NU);         /* 0.995 -> 1.0 */
    expect_span_text(span_of(0, 0, 0xFFFFFFFFu), 2, "1" NU);
    expect_span_text(span_of(0, 1, 0), 2, "1" NU);
    expect_span_text(span_of(0, 1, frac_of(4, 100)), 2, "1" NU);
    expect_span_text(span_of(0, 1, frac_of(5, 100) + 1), 2, "1.1" NU);          /* 1.05 -> 1.1 */
    expect_span_text(span_of(0, 4, 0), 2, "4" NU);
    expect_span_text(span_of(0, 9, frac_of(94, 100)), 2, "9.9" NU);
    expect_span_text(span_of(0, 9, frac_of(95, 100) + 1), 2, "10" NU);          /* 9.95 -> 10 */
    expect_span_text(span_of(0, 9, 0xFFFFFFFFu), 2, "10" NU);
    /* from 10 Blinks on the decimal is gone: whole counts only */
    expect_span_text(span_of(0, 10, 0xFFFFFFFFu), 2, "10" NU);
    expect_span_text(span_of(0, 24, 0xFFFFFFFFu), 2, "24" NU);
    expect_span_text(span_of(0, 25, 0), 2, "1" LAMBDA);
    expect_span_text(span_of(0, 26, 0x80000000u), 2, "1" LAMBDA " 1" NU);
    expect_span_text(span_of(0, 26, 0x80000000u), 1, "1" LAMBDA);
    /* a Blink count below 10 behind a larger unit stays whole */
    expect_span_text(span_of(0, 25 + 2, frac_of(6, 10)), 2, "1" LAMBDA " 2" NU);
    /* precision */
    span = span_of(0, UINT64_C(7319812500) * 3 + UINT64_C(1463962500) * 2 + 112612500 * 7 + 22522500 * 4 +
                          4504500 * 3 + 58500 * 76 + 1625 * 35 + 25 * 64 + 24, 0);
    expect_span_text(span, 1, "3\xCE\xB1");
    expect_span_text(span, 2, "3\xCE\xB1 2\xCE\xB2");
    expect_span_text(span, 0, "3\xCE\xB1 2\xCE\xB2");
    expect_span_text(span, 9, "3\xCE\xB1 2\xCE\xB2 7\xCE\xB3 4\xCE\xB4 3" EPS " 76" ETA " 35" IOTA " 64" LAMBDA " 24" NU);
    expect_span_text(span, 255, "3\xCE\xB1 2\xCE\xB2 7\xCE\xB3 4\xCE\xB4 3" EPS " 76" ETA " 35" IOTA " 64" LAMBDA " 24" NU);
    /* counts are plain digits, never grouped */
    expect_span_text(span_of(1, UINT64_MAX, 0xFFFFFFFFu), 2, MINUS "2520111556\xCE\xB1 3\xCE\xB2");

    TEST_SECTION("frc_format_span: arguments and buffers");
    strcpy(text, "untouched");
    CHECK_EQ_I64(frc_format_span(NULL, 2, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, "");
    edge_span = span;
    edge_span.negative = 1;
    CHECK_EVERY_SIZE(CALL_SPAN_2, MINUS "3\xCE\xB1 2\xCE\xB2");
    CHECK_EVERY_SIZE(CALL_SPAN_9, MINUS "3\xCE\xB1 2\xCE\xB2 7\xCE\xB3 4\xCE\xB4 3" EPS " 76" ETA " 35" IOTA " 64" LAMBDA " 24" NU);
    edge_span = span_of(1, 0, frac_of(123, 100000) + 1);
    CHECK_EVERY_SIZE(CALL_SPAN_2, MINUS "0.0012" NU);
    edge_span = span_of(0, 0, 429497);
    CHECK_EVERY_SIZE(CALL_SPAN_2, "<0.001" NU);
}

/* ------------------------------------------------------------------------- */
/* Rates                                                                      */
/* ------------------------------------------------------------------------- */

static frc_rate_t edge_rate;
#define CALL_RATE(b, s) frc_format_rate(&edge_rate, (b), (s))

static void test_rates(void)
{
    frc_rate_t rate;
    char text[80];
    uint8_t i;

    TEST_SECTION("oracle rates");
    for (i = 0; i < ORACLE_NOTATION_RATES_COUNT; i++) {
        const oracle_notation_rate_t *o = &ORACLE_NOTATION_RATES[i];
        memset(&rate, 0xEE, sizeof rate);
        CHECK_EQ_I64(frc_rate_preset(i, &rate), FRC_OK);
        CHECK_EQ_U64(rate.unit, o->unit);
        CHECK_EQ_U64(rate.count, o->count);
        CHECK_EQ_U64(rate.factor, o->factor);
        CHECK_NOTE("preset %u (%s)", i, o->id);
        CHECK_EQ_I64(frc_format_rate(&rate, text, sizeof text), (int)strlen(o->label));
        CHECK_STR(text, o->label);
        /* the factor is the count times the unit's Blinks (real time: 1) */
        CHECK_EQ_U64(rate.factor, rate.count == 0 ? 1 : rate.count * frc_unit_blinks((frc_unit_t)rate.unit));
    }
    CHECK_EQ_U64(ORACLE_NOTATION_RATES_COUNT, FRC_RATE_COUNT);

    TEST_SECTION("rates: arguments, other rates, buffers");
    memset(&rate, 0xEE, sizeof rate);
    CHECK_EQ_I64(frc_rate_preset(FRC_RATE_COUNT, &rate), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_rate_preset(255, &rate), FRC_ERR_ARG);
    CHECK(rate.unit == 0xEE); /* untouched */
    CHECK_EQ_I64(frc_rate_preset(0, NULL), FRC_ERR_ARG);
    /* a rate that is not a preset is written by the same rule */
    rate.unit = FRC_UNIT_HELION;
    rate.count = 3;
    rate.factor = 3 * 58500;
    CHECK(frc_format_rate(&rate, text, sizeof text) > 0);
    CHECK_STR(text, "3" ETA " per Blink");
    rate.unit = FRC_UNIT_ALPHA;
    rate.count = 255;
    CHECK(frc_format_rate(&rate, text, sizeof text) > 0);
    CHECK_STR(text, "255\xCE\xB1 per Blink");
    rate.unit = FRC_UNIT_COUNT;
    strcpy(text, "untouched");
    CHECK_EQ_I64(frc_format_rate(&rate, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_format_rate(NULL, text, sizeof text), FRC_ERR_ARG);

    CHECK_EQ_I64(frc_rate_preset(5, &edge_rate), FRC_OK);
    CHECK_EVERY_SIZE(CALL_RATE, "4" IOTA " per Blink");
    CHECK_EQ_I64(frc_rate_preset(0, &edge_rate), FRC_OK);
    CHECK_EVERY_SIZE(CALL_RATE, "real time");
}

/* ------------------------------------------------------------------------- */
/* Cycles per Blink                                                           */
/* ------------------------------------------------------------------------- */

#define CALL_PB_WHOLE(b, s)  frc_format_per_blink(12345678901, 10, (b), (s))
#define CALL_PB_SLOW(b, s)   frc_format_per_blink(1, 58500, (b), (s))
#define CALL_PB_SMALL(b, s)  frc_format_per_blink(57, 1250, (b), (s))
#define CALL_PB_NOTHING(b, s) frc_format_per_blink(0, 1, (b), (s))

static void test_per_blink(void)
{
    uint64_t num, den, product;
    size_t i;
    uint8_t level;
    int overflowed;

    TEST_SECTION("oracle per Blink");
    for (i = 0; i < ORACLE_NOTATION_PER_BLINK_COUNT; i++) {
        const oracle_notation_per_blink_t *o = &ORACLE_NOTATION_PER_BLINK[i];
        if (o->level >= 0) {
            num = den = 0xEEEE;
            CHECK_EQ_I64(frc_level_per_blink((uint8_t)o->level, &num, &den), FRC_OK);
            CHECK_EQ_U64(num, o->num);
            CHECK_EQ_U64(den, o->den);
        }
        expect_per_blink(o->num, o->den, o->text);
    }

    TEST_SECTION("frc_level_per_blink: every level");
    product = 1;
    overflowed = 0;
    for (level = 0; level < FRC_LEVEL_COUNT; level++) {
        frc_status_t status;
        num = den = 0xEEEE;
        status = frc_level_per_blink(level, &num, &den);
        if (level <= 12) {
            CHECK_EQ_I64(status, FRC_OK);
            CHECK_EQ_U64(num, 1);
            CHECK_EQ_U64(den * frc_prefix(level), frc_prefix(12));
            continue;
        }
        /* the product of the factors of levels 12 .. level - 1, while it fits */
        if (!overflowed) {
            uint8_t factor = frc_factor((uint8_t)(level - 1));
            if (product > UINT64_MAX / factor) {
                overflowed = 1;
            } else {
                product *= factor;
            }
        }
        if (overflowed) {
            CHECK_EQ_I64(status, FRC_ERR_OVERFLOW);
            CHECK(num == 0xEEEE && den == 0xEEEE); /* untouched */
        } else {
            CHECK_EQ_I64(status, FRC_OK);
            CHECK_EQ_U64(num, product);
            CHECK_EQ_U64(den, 1);
            if (level <= 22) {
                CHECK_EQ_U64(num * frc_prefix(12), frc_prefix(level));
            }
        }
        /* "FRC_ERR_OVERFLOW when the value does not fit (level > 30 or so)":
         * P(35) / P(12) = 1.828e19 is the last below 2^64 = 1.845e19 */
        CHECK_EQ_I64(overflowed, level > 35);
    }
    CHECK_EQ_I64(frc_level_per_blink(35, &num, &den), FRC_OK);
    CHECK_EQ_U64(num, UINT64_C(18279729693225000000));
    CHECK_EQ_I64(frc_level_per_blink(36, &num, &den), FRC_ERR_OVERFLOW);
    CHECK_EQ_I64(frc_level_per_blink(FRC_LEVEL_COUNT, &num, &den), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_level_per_blink(255, &num, &den), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_level_per_blink(13, NULL, &den), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_level_per_blink(13, &num, NULL), FRC_ERR_ARG);

    TEST_SECTION("frc_format_per_blink by hand");
    /* nothing */
    expect_per_blink(0, 1, DASH);
    expect_per_blink(0, 0, DASH);
    expect_per_blink(5, 0, DASH);
    /* design ruling 3: below 1 pB with a whole reciprocal, "1 per N Blinks"
     * (N = den / num exactly, grouped): the Flavon and mu, and any other */
    expect_per_blink(1, 25, "1 per 25 Blinks");
    expect_per_blink(4, 100, "1 per 25 Blinks");
    expect_per_blink(2, 50, "1 per 25 Blinks");
    expect_per_blink(1, 5, "1 per 5 Blinks");
    expect_per_blink(1, 2, "1 per 2 Blinks");
    expect_per_blink(3, 6, "1 per 2 Blinks");
    expect_per_blink(UINT64_MAX / 3, UINT64_MAX, "1 per 3 Blinks");
    expect_per_blink(UINT64_MAX - 1, UINT64_MAX, "1 pB");        /* no whole reciprocal: kept */
    /* slower than 1 per 25 Blinks: "1 per N Blinks", N rounded, grouped */
    expect_per_blink(1000000000, UINT64_C(25000000001), "1 per 25 Blinks"); /* a hair slower: exact, no epsilon */
    expect_per_blink(1, 26, "1 per 26 Blinks");
    expect_per_blink(2, 51, "1 per 26 Blinks");  /* 25.5: the half goes up */
    expect_per_blink(2, 53, "1 per 27 Blinks");
    expect_per_blink(1, 999, "1 per 999 Blinks");
    expect_per_blink(1, 1000, "1 per 1" NNBSP "000 Blinks");
    expect_per_blink(1, UINT64_C(7319812500), "1 per 7" NNBSP "319" NNBSP "812" NNBSP "500 Blinks");
    expect_per_blink(1, UINT64_MAX, "1 per 18" NNBSP "446" NNBSP "744" NNBSP "073" NNBSP "709" NNBSP "551" NNBSP "615 Blinks");
    expect_per_blink(3, UINT64_MAX, "1 per 6" NNBSP "148" NNBSP "914" NNBSP "691" NNBSP "236" NNBSP "517" NNBSP "205 Blinks");
    expect_per_blink(UINT64_MAX / 26, UINT64_MAX, "1 per 26 Blinks");
    /* three significant digits below 99.95 */
    expect_per_blink(4001, 100000, "0.04 pB");   /* 0.04001 */
    expect_per_blink(4005, 100000, "0.0401 pB"); /* 0.04005: the half goes up */
    expect_per_blink(999, 10000, "0.0999 pB");
    expect_per_blink(9995, 100000, "0.1 pB");    /* 0.09995 -> 0.1000 */
    expect_per_blink(1, 10, "1 per 10 Blinks");  /* a whole reciprocal (ruling 3) */
    expect_per_blink(2, 10, "1 per 5 Blinks");
    expect_per_blink(1, 3, "1 per 3 Blinks");
    expect_per_blink(3, 10, "0.3 pB");          /* no whole reciprocal: three digits as before */
    expect_per_blink(2, 3, "0.667 pB");
    expect_per_blink(3, 4, "0.75 pB");
    expect_per_blink(2, 5, "0.4 pB");
    expect_per_blink(9995, 10000, "1 pB");       /* 0.9995 -> 1.00 */
    expect_per_blink(1, 1, "1 pB");
    expect_per_blink(1234, 1000, "1.23 pB");
    expect_per_blink(1235, 1000, "1.24 pB");
    expect_per_blink(9995, 1000, "10 pB");       /* 9.995 -> 10.0 */
    expect_per_blink(10, 1, "10 pB");
    expect_per_blink(1234, 100, "12.3 pB");
    expect_per_blink(1235, 100, "12.4 pB");
    expect_per_blink(9994, 100, "99.9 pB");
    expect_per_blink(99949999, 1000000, "99.9 pB");
    /* the decades of three significant digits, on both sides of 0.1, 1 and
     * 10 (no ladder rate reaches them: direct callers only) */
    expect_per_blink(41, 4, "10.3 pB");          /* 10.25: one decimal from 10 */
    expect_per_blink(1049, 100, "10.5 pB");
    expect_per_blink(1001, 100, "10 pB");        /* 10.01 -> 10.0 */
    expect_per_blink(99999, 10000, "10 pB");     /* 9.9999: two decimals, up into 10 */
    expect_per_blink(99, 10, "9.9 pB");
    expect_per_blink(9949, 1000, "9.95 pB");     /* 9.949: two decimals below 10 */
    expect_per_blink(7456, 1000, "7.46 pB");
    expect_per_blink(1234, 10000, "0.123 pB");   /* three decimals from 0.1 */
    expect_per_blink(101, 1000, "0.101 pB");
    expect_per_blink(100, 1001, "0.0999 pB");    /* four decimals below 0.1 */
    expect_per_blink(9999, 100000, "0.1 pB");    /* 0.09999 -> 0.1000 */
    expect_per_blink(10049, 100000, "0.1 pB");   /* 0.10049 -> 0.100 */
    /* one decimal from 99.95 */
    expect_per_blink(9995, 100, "100 pB");       /* 99.95 -> 100.0 */
    expect_per_blink(100, 1, "100 pB");
    expect_per_blink(10004, 100, "100 pB");
    expect_per_blink(10005, 100, "100.1 pB");
    expect_per_blink(99944, 100, "999.4 pB");
    expect_per_blink(99949, 100, "999.5 pB");    /* 999.49 */
    /* whole and grouped from 999.5 */
    expect_per_blink(9995, 10, "1" NNBSP "000 pB");
    expect_per_blink(1000, 1, "1" NNBSP "000 pB");
    expect_per_blink(20009, 20, "1" NNBSP "000 pB"); /* 1000.45 */
    expect_per_blink(2001, 2, "1" NNBSP "001 pB");   /* 1000.5 */
    expect_per_blink(999999, 1, "999" NNBSP "999 pB");
    expect_per_blink(UINT64_MAX, 1, "18" NNBSP "446" NNBSP "744" NNBSP "073" NNBSP "709" NNBSP "551" NNBSP "615 pB");
    expect_per_blink(UINT64_MAX, 2, "9" NNBSP "223" NNBSP "372" NNBSP "036" NNBSP "854" NNBSP "775" NNBSP "808 pB");
    expect_per_blink(UINT64_MAX, UINT64_MAX, "1 pB");
    expect_per_blink(UINT64_MAX, UINT64_MAX - 1, "1 pB");
    expect_per_blink(UINT64_MAX - 1, UINT64_MAX, "1 pB");
    /* a fraction says the same whatever it is multiplied by */
    for (i = 0; i < 20000; i++) {
        uint64_t n = (rng() >> (20 + rng() % 44)) + 1;
        uint64_t d = (rng() >> (20 + rng() % 44)) + 1;
        uint64_t scale = rng() % 1000 + 2;
        char plain[80], scaled[80];
        CHECK(frc_format_per_blink(n, d, plain, sizeof plain) > 0);
        CHECK(frc_format_per_blink(n * scale, d * scale, scaled, sizeof scaled) > 0);
        CHECK_STR(scaled, plain);
        CHECK_NOTE("%" PRIu64 " / %" PRIu64 " times %" PRIu64, n, d, scale);
    }

    TEST_SECTION("frc_format_per_blink: buffers");
    CHECK_EVERY_SIZE(CALL_PB_WHOLE, "1" NNBSP "234" NNBSP "567" NNBSP "890 pB");
    CHECK_EVERY_SIZE(CALL_PB_SLOW, "1 per 58" NNBSP "500 Blinks");
    CHECK_EVERY_SIZE(CALL_PB_SMALL, "0.0456 pB");
    CHECK_EVERY_SIZE(CALL_PB_NOTHING, DASH);
}


/* ------------------------------------------------------------------------- */
/* Signed spans, a span in one unit                                           */
/* ------------------------------------------------------------------------- */

static frc_span_t signed_span;
#define CALL_SIGNED(b, s) frc_format_span_signed(&signed_span, 9, (b), (s))
#define CALL_SIGNED_NULL(b, s) frc_format_span_signed(NULL, 2, (b), (s))
static frc_unit_t in_unit_unit;
#define CALL_IN_UNIT(b, s) frc_format_span_in_unit(&signed_span, in_unit_unit, (b), (s))
#define CALL_IN_UNIT_NULL(b, s) frc_format_span_in_unit(NULL, FRC_UNIT_BLINK, (b), (s))

static void test_signed_and_in_unit(void)
{
    char text[200], plain[200], want[200];
    int i;
    uint8_t max_parts, unit;

    TEST_SECTION("frc_format_span_signed: the sign of frc_format_span, and a plus");
    expect_span_text(span_of(0, 6 * 1625 + 43 * 25, 0), 2, "6" IOTA " 43" LAMBDA);
    signed_span = span_of(0, 6 * 1625 + 43 * 25, 0);
    CHECK(frc_format_span_signed(&signed_span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "+6" IOTA " 43" LAMBDA);
    signed_span = span_of(1, 20 * 1625 + 21 * 25, 0);
    CHECK(frc_format_span_signed(&signed_span, 0, text, sizeof text) > 0);
    CHECK_STR(text, MINUS "20" IOTA " 21" LAMBDA);
    signed_span = span_of(0, 0, 0);
    CHECK(frc_format_span_signed(&signed_span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "0" NU);
    signed_span = span_of(1, 0, 0);
    CHECK(frc_format_span_signed(&signed_span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "0" NU);
    signed_span = span_of(0, 0, 1);
    CHECK(frc_format_span_signed(&signed_span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "<0.001" NU);
    signed_span = span_of(1, 0, 4294967);
    CHECK(frc_format_span_signed(&signed_span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "<0.001" NU);
    signed_span = span_of(0, 0, 4294968);
    CHECK(frc_format_span_signed(&signed_span, 0, text, sizeof text) > 0);
    CHECK_STR(text, "+0.001" NU);
    for (i = 0; i < 200000; i++) {
        frc_span_t span = span_of((int)(rng() & 1), rng() >> (rng() % 64), (uint32_t)(rng() >> (rng() % 64)));
        max_parts = (uint8_t)(rng() % 11);
        CHECK(frc_format_span(&span, max_parts, plain, sizeof plain) > 0);
        CHECK(frc_format_span_signed(&span, max_parts, text, sizeof text) > 0);
        /* the text of frc_format_span, with "+" before a positive one that shows a digit */
        if (span.negative || strcmp(plain, "0" NU) == 0 || strcmp(plain, "<0.001" NU) == 0) {
            CHECK_STR(text, plain);
        } else {
            snprintf(want, sizeof want, "+%s", plain);
            CHECK_STR(text, want);
        }
    }
    signed_span = span_of(0, UINT64_C(2520111555) * UINT64_C(7319812500) + 4 * UINT64_C(1463962500)
                             + 12 * UINT64_C(112612500) + 4 * UINT64_C(22522500) + 4 * UINT64_C(4504500)
                             + 76 * UINT64_C(58500) + 35 * 1625 + 64 * 25 + 24, 0);
    CHECK_EVERY_SIZE(CALL_SIGNED, "+2520111555" "\xCE\xB1" " 4" "\xCE\xB2" " 12" "\xCE\xB3" " 4" "\xCE\xB4"
                                  " 4" EPS " 76" ETA " 35" IOTA " 64" LAMBDA " 24" NU);
    signed_span.negative = 1;
    CHECK(frc_format_span_signed(&signed_span, 9, text, sizeof text) == 52); /* the longest text */
    CHECK_REFUSED_SPAN(CALL_SIGNED_NULL);

    TEST_SECTION("frc_format_span_in_unit: whole units, cut toward zero");
    signed_span = span_of(1, 6236 * 25 + 24, 0xFFFFFFFFu); /* -6 236.99 Flavons */
    in_unit_unit = FRC_UNIT_FLAVON;
    CHECK_EVERY_SIZE(CALL_IN_UNIT, MINUS "6" NNBSP "236" LAMBDA);
    signed_span = span_of(0, 24, 0xFFFFFFFFu);
    CHECK(frc_format_span_in_unit(&signed_span, FRC_UNIT_FLAVON, text, sizeof text) > 0);
    CHECK_STR(text, "0" LAMBDA);
    signed_span.negative = 1;
    CHECK(frc_format_span_in_unit(&signed_span, FRC_UNIT_FLAVON, text, sizeof text) > 0);
    CHECK_STR(text, "0" LAMBDA); /* never "−0λ" */
    signed_span = span_of(1, UINT64_MAX, 0);
    in_unit_unit = FRC_UNIT_BLINK;
    CHECK_EVERY_SIZE(CALL_IN_UNIT, MINUS "18" NNBSP "446" NNBSP "744" NNBSP "073" NNBSP "709" NNBSP "551" NNBSP "615" NU);
    for (i = 0; i < 100000; i++) {
        frc_span_t span = span_of((int)(rng() & 1), rng() >> (rng() % 64), (uint32_t)rng());
        for (unit = 0; unit < FRC_UNIT_COUNT; unit++) {
            uint64_t count = span.blinks / frc_unit_blinks((frc_unit_t)unit);
            char digits[64];
            CHECK(frc_format_count(count, digits, sizeof digits) > 0);
            CHECK(frc_format_unit_symbol((frc_unit_t)unit, plain, sizeof plain) > 0);
            snprintf(want, sizeof want, "%s%s%s", (span.negative && count) ? MINUS : "", digits, plain);
            CHECK(frc_format_span_in_unit(&span, (frc_unit_t)unit, text, sizeof text) > 0);
            CHECK_STR(text, want);
        }
    }
    in_unit_unit = FRC_UNIT_COUNT;
    CHECK_REFUSED_SPAN(CALL_IN_UNIT);
    in_unit_unit = (frc_unit_t)-1;
    CHECK_REFUSED_SPAN(CALL_IN_UNIT);
    CHECK_REFUSED_SPAN(CALL_IN_UNIT_NULL);
}

/* ------------------------------------------------------------------------- */
/* frc_span_next_change, by brute force against the formatter                 */
/* ------------------------------------------------------------------------- */

/* The countdown's text at `now`. */
static void countdown_text(const frc_catalogue_t *cat, frc_time_t now, frc_time_t boundary, uint8_t max_parts,
                           char *out, size_t size)
{
    frc_span_t span;
    out[0] = '\0';
    CHECK_EQ_I64(frc_span_between(cat, now, boundary, &span), FRC_OK);
    CHECK(frc_format_span(&span, max_parts, out, size) > 0);
}

/* Walk every microsecond of [from, boundary]: at each change of the text,
 * frc_span_next_change of the last change (and of a few instants since) must
 * be exactly here. */
static void walk_every_microsecond(const frc_catalogue_t *cat, frc_time_t from, frc_time_t boundary,
                                   uint8_t max_parts)
{
    char text[160], before[160];
    frc_time_t now, last = from, probe;
    long changes = 0;

    countdown_text(cat, from, boundary, max_parts, before, sizeof before);
    for (now = from + 1; now <= boundary; now++) {
        countdown_text(cat, now, boundary, max_parts, text, sizeof text);
        if (strcmp(text, before) != 0) {
            CHECK_EQ_I64(frc_span_next_change(cat, last, boundary, max_parts), now);
            CHECK_NOTE("from %" PRId64 " (\"%s\") the text is \"%s\" at %" PRId64 ", max_parts %u", last, before,
                       text, now, max_parts);
            probe = last + (frc_time_t)(rng() % (uint64_t)(now - last));
            CHECK_EQ_I64(frc_span_next_change(cat, probe, boundary, max_parts), now);
            CHECK_EQ_I64(frc_span_next_change(cat, now - 1, boundary, max_parts), now);
            last = now;
            strcpy(before, text);
            changes++;
        }
    }
    CHECK_STR(before, "0" NU); /* the boundary itself */
    CHECK_EQ_I64(last, boundary);
    CHECK(changes > 10);
}

static void test_next_change(void)
{
    static const int64_t TIES_MS[3] FRC_PROGMEM = {0, INT64_C(36599062500), INT64_C(73198125000)};
    frc_catalogue_t ties;
    char text[160], at[160];
    int i, step;
    uint8_t level;

    TEST_SECTION("frc_span_next_change: every microsecond of the last 10.5 Blinks (decimals, <0.001, 0)");
    {
        /* vector A, and a boundary ON an eclipse with the countdown in the interval before it */
        const frc_time_t a = FRC_TIME_FROM_S(INT64_C(1791460800));
        const frc_time_t e = frc_catalogue_instant(CAT, 21);
        walk_every_microsecond(CAT, a - 3300000, a, 2);
        walk_every_microsecond(CAT, e - 3300000, e, 0);
        walk_every_microsecond(CAT, e - 2000000, e, 1);
    }
    TEST_SECTION("frc_span_next_change: every microsecond on a table where every mark is an instant");
    CHECK_EQ_I64(frc_catalogue_init(&ties, TIES_MS, 3, 0), FRC_OK);
    /* a Blink is 20 000 us: every tie and threshold of the text is an instant */
    walk_every_microsecond(&ties, 0, 30 * 20000 + 17, 2);
    walk_every_microsecond(&ties, FRC_TIME_FROM_MS(INT64_C(36599062500)) - 400000, FRC_TIME_FROM_MS(INT64_C(36599062500)) + 300000, 3);

    TEST_SECTION("frc_span_next_change: chains to thresholds of every level 0..12, each step checked");
    for (i = 0; i < 1500; i++) {
        frc_time_t t = rng_instant(), now, next;
        frc_threshold_t th;
        uint8_t max_parts = (uint8_t)(rng() % 5);
        level = (uint8_t)(rng() % 13);
        CHECK(frc_next_threshold(CAT, t, level, &th) == FRC_OK);
        if (th.time == FRC_TIME_NONE) {
            continue;
        }
        if (i % 3 == 0) {
            /* start at a random point of the last 1/1000 of the way */
            t = th.time - 1 - (frc_time_t)(rng() % (uint64_t)((th.time - t) / 1000 + 1));
        }
        now = t;
        for (step = 0; step < 30 && now < th.time; step++) {
            countdown_text(CAT, now, th.time, max_parts, at, sizeof at);
            next = frc_span_next_change(CAT, now, th.time, max_parts);
            CHECK(next > now && next <= th.time);
            countdown_text(CAT, next, th.time, max_parts, text, sizeof text);
            CHECK(strcmp(text, at) != 0); /* it changes there */
            CHECK_NOTE("level %u, %" PRId64 " -> %" PRId64 ", \"%s\" -> \"%s\"", level, now, next, at, text);
            countdown_text(CAT, next - 1, th.time, max_parts, text, sizeof text);
            CHECK_STR(text, at); /* and not a microsecond earlier */
            CHECK_NOTE("level %u, %" PRId64 " -> %" PRId64, level, now, next);
            if (next - now > 2) {
                /* nor anywhere between */
                frc_time_t probe = now + 1 + (frc_time_t)(rng() % (uint64_t)(next - now - 1));
                countdown_text(CAT, probe, th.time, max_parts, text, sizeof text);
                CHECK_STR(text, at);
            }
            now = (rng() % 4 == 0) ? next : next + (frc_time_t)(rng() % 3); /* also from inside a text */
        }
    }

    TEST_SECTION("frc_span_next_change: from far away, every max_parts");
    for (i = 0; i < 300; i++) {
        frc_time_t t = rng_instant(), boundary = rng_instant(), next;
        uint8_t max_parts = (uint8_t)(rng() % 11);
        if (t > boundary) {
            frc_time_t swap = t;
            t = boundary;
            boundary = swap;
        }
        if (t == boundary) {
            continue;
        }
        countdown_text(CAT, t, boundary, max_parts, at, sizeof at);
        next = frc_span_next_change(CAT, t, boundary, max_parts);
        CHECK(next > t && next <= boundary);
        countdown_text(CAT, next, boundary, max_parts, text, sizeof text);
        CHECK(strcmp(text, at) != 0);
        countdown_text(CAT, next - 1, boundary, max_parts, text, sizeof text);
        CHECK_STR(text, at);
        CHECK_NOTE("%" PRId64 " -> %" PRId64 " (boundary %" PRId64 "), max_parts %u", t, next, boundary, max_parts);
    }

    TEST_SECTION("frc_span_next_change: the edges");
    {
        const frc_time_t a = FRC_TIME_FROM_S(INT64_C(1791460800));
        CHECK_EQ_I64(frc_span_next_change(CAT, a, a, 2), FRC_TIME_NONE);       /* t >= boundary */
        CHECK_EQ_I64(frc_span_next_change(CAT, a + 1, a, 2), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change(CAT, a - 1, a, 2), a);               /* "<0.001ν" -> "0ν" */
        CHECK_EQ_I64(frc_span_next_change(CAT, a - 310, a, 2), a);
        CHECK_EQ_I64(frc_span_next_change(NULL, a - 1, a, 2), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change(CAT, frc_catalogue_first(CAT) - 1, a, 2), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change(CAT, a, frc_catalogue_last(CAT) + 1, 2), FRC_TIME_NONE);
        /* up to the closing instant, which is a boundary like any other */
        walk_every_microsecond(CAT, frc_catalogue_last(CAT) - 3200000, frc_catalogue_last(CAT), 2);
        /* from the first instant */
        CHECK(frc_span_next_change(CAT, frc_catalogue_first(CAT), a, 2) > frc_catalogue_first(CAT));
    }
}


/* The text of a span since `origin` at `now`. */
static void elapsed_text(const frc_catalogue_t *cat, frc_time_t origin, frc_time_t now, uint8_t max_parts,
                         char *out, size_t size)
{
    frc_span_t span;
    out[0] = '\0';
    CHECK_EQ_I64(frc_span_between(cat, origin, now, &span), FRC_OK);
    CHECK(frc_format_span(&span, max_parts, out, size) > 0);
}

/* Walk every microsecond of [origin, to]: at each change of the text of the
 * span since `origin`, frc_span_next_change_since of the last change (and of
 * a few instants since) must be exactly here. The signed span to `origin`
 * (past its zero) changes at the same instants, with a minus. */
static void walk_since(const frc_catalogue_t *cat, frc_time_t origin, frc_time_t to, uint8_t max_parts)
{
    char text[160], before[160], signed_text[160], want[170];
    frc_time_t now, last = origin, probe;
    long changes = 0;
    frc_span_t span;

    elapsed_text(cat, origin, origin, max_parts, before, sizeof before);
    CHECK_STR(before, "0" NU);
    for (now = origin + 1; now <= to; now++) {
        elapsed_text(cat, origin, now, max_parts, text, sizeof text);
        if (strcmp(text, before) != 0) {
            CHECK_EQ_I64(frc_span_next_change_since(cat, origin, last, max_parts), now);
            CHECK_NOTE("since %" PRId64 ": from %" PRId64 " (\"%s\") the text is \"%s\" at %" PRId64 ", max_parts %u",
                       origin, last, before, text, now, max_parts);
            probe = last + (frc_time_t)(rng() % (uint64_t)(now - last));
            CHECK_EQ_I64(frc_span_next_change_since(cat, origin, probe, max_parts), now);
            CHECK_EQ_I64(frc_span_next_change_since(cat, origin, now - 1, max_parts), now);
            /* the signed countdown past its boundary: the same text, signed */
            CHECK_EQ_I64(frc_span_between(cat, now, origin, &span), FRC_OK);
            CHECK(frc_format_span_signed(&span, max_parts, signed_text, sizeof signed_text) > 0);
            snprintf(want, sizeof want, "%s%s", (text[0] == '<') ? "" : MINUS, text);
            CHECK_STR(signed_text, want);
            last = now;
            strcpy(before, text);
            changes++;
        }
    }
    CHECK(changes > 10);
}

static void test_next_change_since(void)
{
    static const int64_t TIES_MS[3] FRC_PROGMEM = {0, INT64_C(36599062500), INT64_C(73198125000)};
    static const int64_t WIDE_MS[2] FRC_PROGMEM = {-(INT64_MAX / 1000), INT64_MAX / 1000};
    frc_catalogue_t ties, wide;
    char text[160], at[160];
    int i, step;
    const frc_time_t a = FRC_TIME_FROM_S(INT64_C(1791460800));

    TEST_SECTION("frc_span_next_change_since: every microsecond of the first 10.5 Blinks (0, <0.001, decimals, parts)");
    walk_since(CAT, a, a + 3300000, 2);
    walk_since(CAT, frc_catalogue_instant(CAT, 21), frc_catalogue_instant(CAT, 21) + 3300000, 0);
    walk_since(CAT, a - 1700000, a + 1600000, 1); /* across an instant that is no boundary of its own */
    walk_since(CAT, frc_catalogue_first(CAT), frc_catalogue_first(CAT) + 3300000, 9);
    /* growing across an eclipse */
    walk_since(CAT, frc_catalogue_instant(CAT, 30) - 900000, frc_catalogue_instant(CAT, 30) + 900000, 2);

    TEST_SECTION("frc_span_next_change_since: every microsecond on a table where every mark is an instant");
    CHECK_EQ_I64(frc_catalogue_init(&ties, TIES_MS, 3, 0), FRC_OK);
    walk_since(&ties, 0, 30 * 20000 + 17, 2);
    walk_since(&ties, FRC_TIME_FROM_MS(INT64_C(36599062500)) - 300000,
               FRC_TIME_FROM_MS(INT64_C(36599062500)) + 400000, 3);

    TEST_SECTION("frc_span_next_change_since: chains from origins at every level 0..12, each step checked");
    for (i = 0; i < 1500; i++) {
        frc_time_t origin = rng_instant(), now, next;
        uint8_t max_parts = (uint8_t)(rng() % 5);
        uint8_t level = (uint8_t)(rng() % 13);
        frc_cycle_t cycle;
        CHECK(frc_cycle_at(CAT, origin, level, &cycle) == FRC_OK);
        origin = cycle.start; /* an elapsed time since a cycle began */
        now = origin + ((i % 3 == 0) ? 0 : (frc_time_t)(rng() % (uint64_t)(cycle.end - cycle.start)));
        for (step = 0; step < 30; step++) {
            elapsed_text(CAT, origin, now, max_parts, at, sizeof at);
            next = frc_span_next_change_since(CAT, origin, now, max_parts);
            if (next == FRC_TIME_NONE) {
                elapsed_text(CAT, origin, frc_catalogue_last(CAT), max_parts, text, sizeof text);
                CHECK_STR(text, at); /* only when nothing changes before the end */
                break;
            }
            CHECK(next > now && next <= frc_catalogue_last(CAT));
            elapsed_text(CAT, origin, next, max_parts, text, sizeof text);
            CHECK(strcmp(text, at) != 0); /* it changes there */
            CHECK_NOTE("level %u, since %" PRId64 ": %" PRId64 " -> %" PRId64 ", \"%s\" -> \"%s\"", level, origin,
                       now, next, at, text);
            elapsed_text(CAT, origin, next - 1, max_parts, text, sizeof text);
            CHECK_STR(text, at); /* and not a microsecond earlier */
            if (next - now > 2) {
                frc_time_t probe = now + 1 + (frc_time_t)(rng() % (uint64_t)(next - now - 1));
                elapsed_text(CAT, origin, probe, max_parts, text, sizeof text);
                CHECK_STR(text, at); /* nor anywhere between */
            }
            now = (rng() % 4 == 0) ? next : next + (frc_time_t)(rng() % 3);
            if (now > frc_catalogue_last(CAT)) {
                break;
            }
        }
    }

    TEST_SECTION("frc_span_next_change_since: from far away, every max_parts");
    for (i = 0; i < 300; i++) {
        frc_time_t origin = rng_instant(), t = rng_instant(), next;
        uint8_t max_parts = (uint8_t)(rng() % 11);
        if (t < origin) {
            frc_time_t swap = t;
            t = origin;
            origin = swap;
        }
        elapsed_text(CAT, origin, t, max_parts, at, sizeof at);
        next = frc_span_next_change_since(CAT, origin, t, max_parts);
        if (next == FRC_TIME_NONE) {
            elapsed_text(CAT, origin, frc_catalogue_last(CAT), max_parts, text, sizeof text);
            CHECK_STR(text, at);
            continue;
        }
        CHECK(next > t && next <= frc_catalogue_last(CAT));
        elapsed_text(CAT, origin, next, max_parts, text, sizeof text);
        CHECK(strcmp(text, at) != 0);
        elapsed_text(CAT, origin, next - 1, max_parts, text, sizeof text);
        CHECK_STR(text, at);
        CHECK_NOTE("since %" PRId64 ": %" PRId64 " -> %" PRId64 ", max_parts %u", origin, t, next, max_parts);
    }

    TEST_SECTION("frc_span_next_change_since: the edges");
    CHECK_EQ_I64(frc_span_next_change_since(CAT, a, a, 2), a + 1);          /* "0ν" -> "<0.001ν" */
    CHECK_EQ_I64(frc_span_next_change_since(CAT, a, a + 1, 2), a + 311);    /* "<0.001ν" -> "0.001ν" (S15.4: 311 us) */
    CHECK_EQ_I64(frc_span_next_change_since(CAT, a, a - 1, 2), FRC_TIME_NONE); /* t < origin */
    CHECK_EQ_I64(frc_span_next_change_since(NULL, a, a, 2), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_span_next_change_since(CAT, frc_catalogue_first(CAT) - 1, a, 2), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_span_next_change_since(CAT, a, frc_catalogue_last(CAT) + 1, 2), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_span_next_change_since(CAT, a, frc_catalogue_last(CAT), 2), FRC_TIME_NONE); /* nothing later */
    CHECK_EQ_I64(frc_span_next_change_since(CAT, frc_catalogue_last(CAT), frc_catalogue_last(CAT), 2), FRC_TIME_NONE);
    /* the text at the closing instant is still the text: "17α" to the end */
    elapsed_text(CAT, frc_catalogue_first(CAT), frc_catalogue_last(CAT) - 1, 1, text, sizeof text);
    CHECK_STR(text, "17\xCE\xB1");
    CHECK_EQ_I64(frc_span_next_change_since(CAT, frc_catalogue_first(CAT), frc_catalogue_last(CAT) - 1, 1),
                 FRC_TIME_NONE);
    /* growing to the closing instant, which is an instant like any other */
    walk_since(CAT, frc_catalogue_last(CAT) - 3200000, frc_catalogue_last(CAT), 2);
    /* the boundary of a countdown hands over to the span since it */
    CHECK_EQ_I64(frc_span_next_change(CAT, a - 1, a, 2), a);
    CHECK_EQ_I64(frc_span_next_change_since(CAT, a, a, 2), a + 1);

    TEST_SECTION("next change: NONE where frc_span_between overflows (an interval of 2^63 us or more)");
    CHECK_EQ_I64(frc_catalogue_init(&wide, WIDE_MS, 2, 0), FRC_OK);
    {
        frc_span_t span;
        CHECK_EQ_I64(frc_span_between(&wide, 0, 1000, &span), FRC_ERR_OVERFLOW);
        CHECK_EQ_I64(frc_span_next_change(&wide, 0, 1000, 2), FRC_TIME_NONE);
        CHECK_EQ_I64(frc_span_next_change_since(&wide, 0, 1000, 2), FRC_TIME_NONE);
    }
}

/* ------------------------------------------------------------------------- */
/* The fast path of the next-change search                                    */
/* ------------------------------------------------------------------------- */

/*
 * Both next-change functions place the change with arithmetic
 * (frc_i_span_change_guess), then confirm it with the formatter a few
 * microseconds either side; a bisection would still find a change that a
 * wrong guess misplaces, only slower (up to ~60 span evaluations instead of
 * ~3), so the answers alone cannot tell. Here the guess itself is held within
 * the window the search trusts it to: three microseconds of the answer.
 */
static void expect_guess_near(const frc_catalogue_t *cat, frc_time_t t, frc_time_t anchor, uint8_t max_parts,
                              frc_time_t answer)
{
    frc_time_t guess = frc_i_span_change_guess(cat, t, anchor, max_parts);
    CHECK(guess != FRC_TIME_NONE && guess >= answer - 3 && guess <= answer + 3);
    CHECK_NOTE("t %" PRId64 ", anchor %" PRId64 ", max_parts %u: answer %" PRId64 ", guess %" PRId64, t, anchor,
               max_parts, answer, guess);
}

static void test_change_guess(void)
{
    static const int64_t TIES_MS[3] FRC_PROGMEM = {0, INT64_C(36599062500), INT64_C(73198125000)};
    frc_catalogue_t ties;
    int i, k;

    TEST_SECTION("next change: the computed instant is within 3 us of the answer (countdowns)");
    CHECK_EQ_I64(frc_catalogue_init(&ties, TIES_MS, 3, 0), FRC_OK);
    for (i = 0; i < 4000; i++) {
        const frc_catalogue_t *cat = (i % 4 == 3) ? &ties : CAT;
        uint64_t extent = (uint64_t)(frc_catalogue_last(cat) - frc_catalogue_first(cat));
        frc_time_t t = frc_catalogue_first(cat) + (frc_time_t)(rng() % extent);
        /* spans from a microsecond to the whole catalogue, every scale */
        frc_time_t reach = (frc_time_t)(rng() >> (rng() % 62)) + 1;
        frc_time_t boundary = (reach < frc_catalogue_last(cat) - t) ? t + reach : frc_catalogue_last(cat);
        uint8_t max_parts = (uint8_t)(rng() % 11);
        frc_time_t now = t, next;
        if (boundary <= t) {
            continue;
        }
        for (k = 0; k < 6 && now < boundary; k++) {
            next = frc_span_next_change(cat, now, boundary, max_parts);
            expect_guess_near(cat, now, boundary, max_parts, next);
            now = next;
        }
    }

    TEST_SECTION("next change: the computed instant is within 3 us of the answer (spans since)");
    for (i = 0; i < 4000; i++) {
        const frc_catalogue_t *cat = (i % 4 == 3) ? &ties : CAT;
        uint64_t extent = (uint64_t)(frc_catalogue_last(cat) - frc_catalogue_first(cat));
        frc_time_t origin = frc_catalogue_first(cat) + (frc_time_t)(rng() % extent);
        frc_time_t reach = (frc_time_t)(rng() >> (rng() % 62));
        frc_time_t now = (reach < frc_catalogue_last(cat) - origin) ? origin + reach : origin;
        uint8_t max_parts = (uint8_t)(rng() % 11);
        frc_time_t next;
        for (k = 0; k < 6; k++) {
            next = frc_span_next_change_since(cat, origin, now, max_parts);
            if (next == FRC_TIME_NONE) {
                break;
            }
            expect_guess_near(cat, now, origin, max_parts, next);
            now = next;
        }
    }
}

int main(void)
{
    test_spec_vectors();
    test_oracle_spans();
    test_span_of_level();
    test_span_between();
    test_time_grain();
    test_exact_rounding();
    test_cutoff();
    test_span_parts_and_text();
    test_rates();
    test_per_blink();
    test_signed_and_in_unit();
    test_next_change();
    test_next_change_since();
    test_change_guess();
    return TEST_SUMMARY("span");
}
