/*
 * test_notation.c - tests of src/frc_notation.c (SPEC S15.1 - S15.3, S7.5):
 *
 *   frc_date_at, frc_date_to_time      an instant <-> the nine numbers of a date
 *   frc_format_date, frc_parse_date    the four written forms, and reading them back
 *   frc_format_ordinal                 fracton ordinals
 *   frc_format_cycle_name              cycle names
 *   frc_format_pentad                  Pentad names
 *
 * Expected values come from three places, in this order of authority:
 *   1. SPEC.md, typed in here by hand: the vectors of S15.6, the examples of
 *      S15.2 and S15.3.
 *   2. tests/vectors/oracle_notation.h (every row of every table that
 *      concerns these functions) and, for the levels behind a date,
 *      tests/vectors/oracle_clock.h. Both are generated from the frozen
 *      JavaScript reference.
 *   3. Properties that hold whatever the values are: round trips, a date
 *      names the Blink it was read in, a parser never accepts garbage.
 *
 * The file needs frc_notation.c and the foundation only; it does not call
 * the sync / Pentad clock (the Pentads it words come from the oracle).
 */
#include "frc_test.h"

#include <stdlib.h>

#include "fractonica.h"
#include "vectors/oracle_clock.h"
#include "vectors/oracle_notation.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static const frc_catalogue_t *const CAT = &FRC_SAROS_141;

/* splitmix64 with a fixed seed, so a failure reproduces. */
static uint64_t rng_state = UINT64_C(0x5EED0F0A7E5);
static uint64_t rng(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}
static frc_time_t rng_instant(void)
{
    uint64_t span = (uint64_t)(frc_catalogue_last(CAT) - frc_catalogue_first(CAT));
    return frc_catalogue_first(CAT) + (frc_time_t)(rng() % (span + 1));
}

/* A date from its nine numbers; quarters 0. */
static frc_date_t date_of(uint16_t alpha, uint8_t beta, uint8_t gamma, uint8_t delta, uint8_t epsilon,
                          uint8_t eta, uint8_t iota, uint8_t lambda, uint8_t nu)
{
    frc_date_t d;
    memset(&d, 0, sizeof d);
    d.alpha = alpha;
    d.beta = beta;
    d.gamma = gamma;
    d.delta = delta;
    d.epsilon = epsilon;
    d.eta = eta;
    d.iota = iota;
    d.lambda = lambda;
    d.nu = nu;
    return d;
}

static frc_date_t date_of_oracle(const oracle_notation_date_t *o)
{
    frc_date_t d = date_of(o->alpha, o->beta, o->gamma, o->delta, o->epsilon, o->eta, o->iota, o->lambda, o->nu);
    memcpy(d.quarter, o->quarter, sizeof d.quarter);
    return d;
}

/* The nine numbers by frc_unit_t, for loops. */
static uint16_t *date_field(frc_date_t *d, int unit, uint16_t *scratch)
{
    uint8_t *bytes[] = {NULL, &d->beta, &d->gamma, &d->delta, &d->epsilon, &d->eta, &d->iota, &d->lambda, &d->nu};
    if (unit == 0) {
        return &d->alpha;
    }
    *scratch = *bytes[unit];
    return scratch;
}
static void set_date_field(frc_date_t *d, int unit, uint16_t value)
{
    uint8_t *bytes[] = {NULL, &d->beta, &d->gamma, &d->delta, &d->epsilon, &d->eta, &d->iota, &d->lambda, &d->nu};
    if (unit == 0) {
        d->alpha = value;
    } else {
        *bytes[unit] = (uint8_t)value;
    }
}

/* What frc_parse_date must return for a text that stops at `finest`: the
 * finer parts are "the start" (ordinals and the Helion 1, counts 0) and the
 * quarters are 0. */
static frc_date_t truncated(const frc_date_t *d, int finest)
{
    frc_date_t out = *d;
    int unit;
    for (unit = finest + 1; unit < FRC_UNIT_COUNT; unit++) {
        set_date_field(&out, unit, unit <= FRC_UNIT_HELION ? 1 : 0);
    }
    memset(out.quarter, 0, sizeof out.quarter);
    return out;
}

static int same_numbers(const frc_date_t *a, const frc_date_t *b)
{
    return a->alpha == b->alpha && a->beta == b->beta && a->gamma == b->gamma && a->delta == b->delta &&
           a->epsilon == b->epsilon && a->eta == b->eta && a->iota == b->iota && a->lambda == b->lambda &&
           a->nu == b->nu;
}
static int same_date(const frc_date_t *a, const frc_date_t *b)
{
    return same_numbers(a, b) && memcmp(a->quarter, b->quarter, sizeof a->quarter) == 0;
}

/* Parse `text` and expect exactly `want` (numbers and zero quarters) and `finest`. */
static void expect_parse(const char *text, const frc_date_t *context, const frc_date_t *want, int finest)
{
    frc_date_t got;
    frc_unit_t got_finest = FRC_UNIT_COUNT;
    frc_status_t status;

    memset(&got, 0xEE, sizeof got);
    status = frc_parse_date(text, context, &got, &got_finest);
    CHECK_EQ_I64(status, FRC_OK);
    CHECK_NOTE("text \"%s\"", text);
    CHECK(status == FRC_OK && same_date(&got, want));
    CHECK_NOTE("text \"%s\" read as %u %u %u %u %u %u %u %u %u", text, got.alpha, got.beta, got.gamma, got.delta,
               got.epsilon, got.eta, got.iota, got.lambda, got.nu);
    CHECK_EQ_I64(got_finest, finest);
    CHECK_NOTE("text \"%s\"", text);
}

/* Parse `text` and expect a failure that leaves `out` and `finest` alone. */
static void expect_reject(const char *text, const frc_date_t *context, frc_status_t want)
{
    frc_date_t got;
    frc_date_t untouched;
    frc_unit_t finest = FRC_UNIT_COUNT;

    memset(&got, 0xEE, sizeof got);
    memset(&untouched, 0xEE, sizeof untouched);
    CHECK_EQ_I64(frc_parse_date(text, context, &got, &finest), want);
    CHECK_NOTE("text \"%s\"", text);
    CHECK(memcmp(&got, &untouched, sizeof got) == 0);
    CHECK_NOTE("text \"%s\": the output was written on failure", text);
    CHECK_EQ_I64(finest, FRC_UNIT_COUNT);
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
            char buf_[400];                                                                  \
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

/* The date of SPEC S12 vector A (S15.6), used all over. */
#define T_A FRC_TIME_FROM_MS(INT64_C(1791460800000))

/* ------------------------------------------------------------------------- */
/* SPEC S15.6, typed by hand                                                  */
/* ------------------------------------------------------------------------- */

static void test_spec_vectors(void)
{
    /* S12: A 2026-10-08T12:00Z, B 2000-01-01T00:00Z, C 2061-06-16T03:50Z,
     * anchor E[20] + 1 s. */
    static const struct {
        const char *name;
        int64_t unix_ms;
        const char *full;
        const char *brief;
    } V[] = {
        {"A", INT64_C(1791460800000),
         "Alpha Hexafracton Valley, Beta Tetrafracton Valley, Gamma Nonafracton Valley, Delta Trifracton End, "
         "\xCE\xB5#5 34\xCE\xB7 29\xCE\xB9 21\xCE\xBB 3\xCE\xBD",
         "Delta Trifracton End \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9"},
        {"B", INT64_C(946684800000),
         "Alpha Hexafracton Node, Beta Difracton End, Gamma Didecafracton Node, Delta Trifracton Node, "
         "\xCE\xB5#2 21\xCE\xB7 12\xCE\xB9 6\xCE\xBB 5\xCE\xBD",
         "Delta Trifracton Node \xCE\xB5#2 21\xCE\xB7 12\xCE\xB9"},
        {"C", INT64_C(2886085800000),
         "Alpha Heptafracton Peak, Beta Difracton Peak, Gamma Monofracton End, Delta Pentafracton Peak, "
         "\xCE\xB5#2 1\xCE\xB7 23\xCE\xB9 1\xCE\xBB 21\xCE\xBD",
         "Delta Pentafracton Peak \xCE\xB5#2 1\xCE\xB7 23\xCE\xB9"},
        {"anchor + 1 s", INT64_C(125593320816),
         "Alpha Hexafracton Peak, Beta Monofracton Peak, Gamma Monofracton Peak, Delta Monofracton Peak, "
         "\xCE\xB5#1 1\xCE\xB7 0\xCE\xB9 0\xCE\xBB 3\xCE\xBD",
         "Delta Monofracton Peak \xCE\xB5#1 1\xCE\xB7 0\xCE\xB9"},
    };
    char text[200];
    frc_date_t d;
    size_t i;

    TEST_SECTION("SPEC S15.6 by hand");
    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        CHECK_EQ_I64(frc_date_at(CAT, FRC_TIME_FROM_MS(V[i].unix_ms), &d), FRC_OK);
        CHECK(frc_format_date(&d, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text) > 0);
        CHECK_STR(text, V[i].full);
        CHECK_NOTE("vector %s", V[i].name);
        CHECK(frc_format_date(&d, FRC_DATE_SHORT, FRC_UNIT_PREON, text, sizeof text) > 0);
        CHECK_STR(text, V[i].brief);
        CHECK_NOTE("vector %s", V[i].name);
    }

    /* "The stamp at A is ..." */
    CHECK_EQ_I64(frc_date_at(CAT, T_A, &d), FRC_OK);
    CHECK(frc_format_date(&d, FRC_DATE_LETTERS, FRC_UNIT_BLINK, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB1#6 \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9 21\xCE\xBB 3\xCE\xBD");
    CHECK_STR(text, ORACLE_NOTATION_S15_6_STAMP_A);

    /* the examples in the comments of fractonica.h */
    CHECK(frc_format_date(&d, FRC_DATE_FULL, FRC_UNIT_PREON, text, sizeof text) > 0);
    CHECK_STR(text, "Alpha Hexafracton Valley, Beta Tetrafracton Valley, Gamma Nonafracton Valley, "
                    "Delta Trifracton End, \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9");
    CHECK(frc_format_date(&d, FRC_DATE_LETTERS, FRC_UNIT_PREON, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB1#6 \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9");
    CHECK(frc_format_date(&d, FRC_DATE_SHORT_LETTERS, FRC_UNIT_PREON, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB4#3 \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9");

    /* the nine numbers and the quarters of A, read off the SPEC text */
    CHECK_EQ_U64(d.alpha, 6);
    CHECK_EQ_U64(d.beta, 4);
    CHECK_EQ_U64(d.gamma, 9);
    CHECK_EQ_U64(d.delta, 3);
    CHECK_EQ_U64(d.epsilon, 5);
    CHECK_EQ_U64(d.eta, 34);
    CHECK_EQ_U64(d.iota, 29);
    CHECK_EQ_U64(d.lambda, 21);
    CHECK_EQ_U64(d.nu, 3);
    CHECK_EQ_U64(d.quarter[0], FRC_VALLEY);
    CHECK_EQ_U64(d.quarter[1], FRC_VALLEY);
    CHECK_EQ_U64(d.quarter[2], FRC_VALLEY);
    CHECK_EQ_U64(d.quarter[3], FRC_END);

    /* the rows of the oracle's own S15.6 table */
    for (i = 0; i < ORACLE_NOTATION_S15_6_COUNT; i++) {
        const oracle_notation_s15_6_t *o = &ORACLE_NOTATION_S15_6[i];
        CHECK_EQ_I64(frc_date_at(CAT, o->t_us, &d), FRC_OK);
        CHECK(frc_format_date(&d, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text) > 0);
        CHECK_STR(text, o->full_date);
        CHECK(frc_format_date(&d, FRC_DATE_SHORT, FRC_UNIT_PREON, text, sizeof text) > 0);
        CHECK_STR(text, o->short_date);
    }
}

/* ------------------------------------------------------------------------- */
/* Oracle: dates                                                              */
/* ------------------------------------------------------------------------- */

static void test_oracle_dates(void)
{
    char text[200];
    size_t i;
    int finest, form;

    TEST_SECTION("oracle dates: frc_date_at");
    for (i = 0; i < ORACLE_NOTATION_DATES_COUNT; i++) {
        const oracle_notation_date_t *o = &ORACLE_NOTATION_DATES[i];
        const oracle_clock_sample_t *s = &ORACLE_CLOCK_SAMPLES[o->clock_sample];
        frc_date_t want = date_of_oracle(o);
        frc_date_t got;
        frc_time_t start = 0;
        frc_cycle_t blink;

        memset(&got, 0xEE, sizeof got);
        CHECK_EQ_I64(frc_date_at(CAT, o->t_us, &got), FRC_OK);
        CHECK(same_date(&got, &want));
        CHECK_NOTE("date %zu at %" PRId64 ": %u %u %u %u %u %u %u %u %u q %u %u %u %u", i, o->t_us, got.alpha,
                   got.beta, got.gamma, got.delta, got.epsilon, got.eta, got.iota, got.lambda, got.nu,
                   got.quarter[0], got.quarter[1], got.quarter[2], got.quarter[3]);

        /* the same reading through the levels of oracle_clock.h and the
         * formulas of SPEC S15.3 (an independent path to the same numbers) */
        CHECK_EQ_I64(s->t_us, o->t_us);
        CHECK_EQ_U64(got.alpha, s->level[0].cycle + 1);
        CHECK_EQ_U64(got.beta, s->level[1].local + 1u);
        CHECK_EQ_U64(got.gamma, s->level[2].local + 1u);
        CHECK_EQ_U64(got.delta, s->level[3].local + 1u);
        CHECK_EQ_U64(got.epsilon, s->level[4].local + 1u);
        CHECK_EQ_U64(got.eta, 11u * s->level[5].local + s->level[6].local + 1u);
        CHECK_EQ_U64(got.iota, 4u * s->level[7].local + s->level[8].local);
        CHECK_EQ_U64(got.lambda, 13u * s->level[9].local + s->level[10].local);
        CHECK_EQ_U64(got.nu, 5u * s->level[11].local + s->level[12].local);
        CHECK_EQ_U64(got.quarter[0], s->level[0].quarter);
        CHECK_EQ_U64(got.quarter[1], s->level[1].quarter);
        CHECK_EQ_U64(got.quarter[2], s->level[2].quarter);
        CHECK_EQ_U64(got.quarter[3], s->level[3].quarter);

        /* frc_date_to_time: the first microsecond of the Blink the date names */
        CHECK_EQ_I64(frc_date_to_time(CAT, &got, &start), FRC_OK);
        CHECK_EQ_I64(start, s->level[12].start_us);
        CHECK(start <= o->t_us);
        CHECK_EQ_I64(frc_cycle_at(CAT, o->t_us, 12, &blink), FRC_OK);
        CHECK_EQ_I64(start, blink.start);
        /* and back: the round trip the header promises */
        {
            frc_date_t again;
            CHECK_EQ_I64(frc_date_at(CAT, start, &again), FRC_OK);
            CHECK(same_numbers(&again, &got));
        }
    }

    TEST_SECTION("oracle dates: frc_format_date");
    for (i = 0; i < ORACLE_NOTATION_DATES_COUNT; i++) {
        const oracle_notation_date_t *o = &ORACLE_NOTATION_DATES[i];
        frc_date_t d = date_of_oracle(o);

        for (finest = FRC_UNIT_DELTA; finest <= FRC_UNIT_BLINK; finest++) {
            for (form = 0; form < ORACLE_NOTATION_FORM_COUNT; form++) {
                const char *want = o->text[finest - FRC_UNIT_DELTA][form];
                int n = frc_format_date(&d, (frc_date_form_t)form, (frc_unit_t)finest, text, sizeof text);
                CHECK_EQ_I64(n, (int)strlen(want));
                CHECK_STR(text, want);
                CHECK_NOTE("date %zu, form %d, finest %d", i, form, finest);
                CHECK(n < 160); /* "Longest output is under 160 bytes" */
            }
        }
        /* the stamp: letters down to the Blink */
        CHECK(frc_format_date(&d, FRC_DATE_LETTERS, FRC_UNIT_BLINK, text, sizeof text) > 0);
        CHECK_STR(text, o->stamp);
        /* a `finest` coarser than the Delta is the Delta */
        for (finest = FRC_UNIT_ALPHA; finest < FRC_UNIT_DELTA; finest++) {
            for (form = 0; form < ORACLE_NOTATION_FORM_COUNT; form++) {
                CHECK(frc_format_date(&d, (frc_date_form_t)form, (frc_unit_t)finest, text, sizeof text) > 0);
                CHECK_STR(text, o->text[0][form]);
            }
        }
    }
}

/* Every text of the oracle read back: as written, after frc_to_ascii, and
 * with a Pentad in front. */
static void test_oracle_parse(void)
{
    static const char *const SEPARATORS[] = {" ", " \xC2\xB7 ", ", ", ". ", "\xC2\xB7", "  "};
    char ascii[200];
    char led[260];
    char pentad_ascii[40];
    size_t i, sep;
    int finest, form, pentad_form, with_helion;

    TEST_SECTION("oracle dates: frc_parse_date");
    for (i = 0; i < ORACLE_NOTATION_DATES_COUNT; i++) {
        const oracle_notation_date_t *o = &ORACLE_NOTATION_DATES[i];
        frc_date_t d = date_of_oracle(o);

        for (finest = FRC_UNIT_DELTA; finest <= FRC_UNIT_BLINK; finest++) {
            frc_date_t want = truncated(&d, finest);
            for (form = 0; form < ORACLE_NOTATION_FORM_COUNT; form++) {
                const char *text = o->text[finest - FRC_UNIT_DELTA][form];
                int full = (form == FRC_DATE_FULL || form == FRC_DATE_LETTERS);

                /* a full date needs no context and ignores one; a short form
                 * takes Alpha, Beta and Gamma from it */
                expect_parse(text, &d, &want, finest);
                if (full) {
                    expect_parse(text, NULL, &want, finest);
                } else if (i % 16 == 0) {
                    expect_reject(text, NULL, FRC_ERR_PARSE);
                }
                CHECK(frc_to_ascii(text, ascii, sizeof ascii) > 0);
                expect_parse(ascii, &d, &want, finest);
            }
        }

        /* "On screen a date leads with its Pentad ... a parser reading a
         * date ignores it" (S15.3). Every Pentad form, a few separators. */
        {
            frc_date_t want = truncated(&d, FRC_UNIT_PREON);
            for (pentad_form = 0; pentad_form < 3; pentad_form++) {
                for (with_helion = 0; with_helion < 2; with_helion++) {
                    const char *pentad = o->pentad[pentad_form][with_helion];
                    for (form = 0; form < ORACLE_NOTATION_FORM_COUNT; form++) {
                        const char *text = o->text[FRC_UNIT_PREON - FRC_UNIT_DELTA][form];
                        sep = (i + (size_t)form + (size_t)pentad_form) % (sizeof SEPARATORS / sizeof SEPARATORS[0]);
                        snprintf(led, sizeof led, "%s%s%s", pentad, SEPARATORS[sep], text);
                        expect_parse(led, &d, &want, FRC_UNIT_PREON);
                        /* the whole line through frc_to_ascii ("⬠II·3" becomes "PII.3") */
                        CHECK(frc_to_ascii(led, ascii, sizeof ascii) > 0);
                        expect_parse(ascii, &d, &want, FRC_UNIT_PREON);
                    }
                    CHECK(frc_to_ascii(pentad, pentad_ascii, sizeof pentad_ascii) > 0);
                    /* a Pentad alone names a day of the sync, not a moment */
                    expect_reject(pentad, &d, FRC_ERR_PARSE);
                    expect_reject(pentad_ascii, &d, FRC_ERR_PARSE);
                }
            }
        }
    }
}

/* The Pentad words of every sample: the Pentad itself comes from the clock
 * oracle (frc_pentad_at belongs to another file), the words from here. */
static void test_oracle_pentads(void)
{
    char text[64];
    size_t i;
    int form, with_helion;

    TEST_SECTION("oracle dates: frc_format_pentad");
    for (i = 0; i < ORACLE_NOTATION_DATES_COUNT; i++) {
        const oracle_notation_date_t *o = &ORACLE_NOTATION_DATES[i];
        const oracle_clock_pentad_t *p = &ORACLE_CLOCK_SAMPLES[o->clock_sample].pentad;
        frc_pentad_t pentad;

        CHECK(p->has);
        memset(&pentad, 0, sizeof pentad);
        pentad.sync = p->sync;
        pentad.index = p->index;
        pentad.helion = p->helion;
        pentad.helions = p->helions;
        pentad.start = p->start_us;
        pentad.end = p->end_us;
        for (form = 0; form < 3; form++) {
            for (with_helion = 0; with_helion < 2; with_helion++) {
                const char *want = o->pentad[form][with_helion];
                int n = frc_format_pentad(&pentad, (frc_pentad_form_t)form, with_helion, text, sizeof text);
                CHECK_EQ_I64(n, (int)strlen(want));
                CHECK_STR(text, want);
                CHECK_NOTE("date %zu, form %d, helion %d", i, form, with_helion);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Dates: properties and edges                                                */
/* ------------------------------------------------------------------------- */

static void test_date_round_trips(void)
{
    char text[200];
    char ascii[200];
    int i, form;

    TEST_SECTION("date round trips at random instants");
    for (i = 0; i < 20000; i++) {
        frc_time_t t = rng_instant();
        frc_date_t d, again, parsed;
        frc_cycle_t blink;
        frc_time_t start = 0;
        frc_unit_t finest = FRC_UNIT_COUNT;
        int level;

        CHECK_EQ_I64(frc_date_at(CAT, t, &d), FRC_OK);
        CHECK_EQ_I64(frc_cycle_at(CAT, t, 12, &blink), FRC_OK);

        /* the date names the running Blink */
        CHECK_EQ_I64(frc_date_to_time(CAT, &d, &start), FRC_OK);
        CHECK_EQ_I64(start, blink.start);
        CHECK(start <= t && t < blink.end);
        CHECK_EQ_I64(frc_date_at(CAT, start, &again), FRC_OK);
        CHECK(same_numbers(&again, &d));
        /* one microsecond earlier is the Blink before (or outside) */
        if (start > frc_catalogue_first(CAT)) {
            CHECK_EQ_I64(frc_date_at(CAT, start - 1, &again), FRC_OK);
            CHECK(!same_numbers(&again, &d));
        }

        /* the quarters are the quarters of the four major cycles */
        for (level = 0; level <= 3; level++) {
            frc_cycle_t major;
            CHECK_EQ_I64(frc_cycle_at(CAT, t, (uint8_t)level, &major), FRC_OK);
            CHECK_EQ_U64(d.quarter[level], major.quarter);
        }

        /* the stamp "identifies a moment exactly and can be read back" */
        for (form = 0; form < 4; form++) {
            CHECK(frc_format_date(&d, (frc_date_form_t)form, FRC_UNIT_BLINK, text, sizeof text) > 0);
            CHECK_EQ_I64(frc_parse_date(text, &d, &parsed, &finest), FRC_OK);
            CHECK(same_numbers(&parsed, &d));
            CHECK_NOTE("text \"%s\"", text);
            CHECK_EQ_I64(finest, FRC_UNIT_BLINK);
            CHECK(frc_to_ascii(text, ascii, sizeof ascii) > 0);
            CHECK_EQ_I64(frc_parse_date(ascii, &d, &parsed, NULL), FRC_OK);
            CHECK(same_numbers(&parsed, &d));
            CHECK_NOTE("text \"%s\"", ascii);
        }
        /* "use frc_date_to_time and frc_date_at to normalise" a parsed date */
        CHECK_EQ_I64(frc_date_to_time(CAT, &parsed, &start), FRC_OK);
        CHECK_EQ_I64(frc_date_at(CAT, start, &again), FRC_OK);
        CHECK(same_numbers(&again, &d));
    }
}

static void test_date_edges(void)
{
    frc_time_t first = frc_catalogue_first(CAT);
    frc_time_t last = frc_catalogue_last(CAT);
    frc_date_t d, e;
    frc_time_t t = 0;
    char text[200];
    int unit;

    TEST_SECTION("date at the catalogue edges");
    /* the opening instant: everything at its start */
    CHECK_EQ_I64(frc_date_at(CAT, first, &d), FRC_OK);
    e = date_of(1, 1, 1, 1, 1, 1, 0, 0, 0);
    CHECK(same_date(&d, &e));
    CHECK(frc_format_date(&d, FRC_DATE_LETTERS, FRC_UNIT_BLINK, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB1#1 \xCE\xB2#1 \xCE\xB3#1 \xCE\xB4#1 \xCE\xB5#1 1\xCE\xB7 0\xCE\xB9 0\xCE\xBB 0\xCE\xBD");
    CHECK_EQ_I64(frc_date_to_time(CAT, &d, &t), FRC_OK);
    CHECK_EQ_I64(t, first);

    /* The closing instant reads as the microsecond before it (S2.2): the
     * last Blink. 69 Saros are 17 Alphas and one Saros = 1β 3γ 1δ 1ε 19η 9ι,
     * so the last Blink is the one before α#18 β#2 γ#4 δ#2 ε#2 20η 9ι. */
    CHECK_EQ_I64(frc_date_at(CAT, last, &d), FRC_OK);
    CHECK_EQ_I64(frc_date_at(CAT, last - 1, &e), FRC_OK);
    CHECK(same_date(&d, &e));
    e = date_of(18, 2, 4, 2, 2, 20, 8, 64, 24);
    CHECK(same_numbers(&d, &e));
    CHECK_EQ_I64(frc_date_to_time(CAT, &d, &t), FRC_OK);
    CHECK(t < last && last - t <= 320000); /* a Blink is about 311 ms */
    /* the very next Blink would start at the closing instant: not a cycle */
    e = date_of(18, 2, 4, 2, 2, 20, 9, 0, 0);
    t = 12345;
    CHECK_EQ_I64(frc_date_to_time(CAT, &e, &t), FRC_ERR_RANGE);
    CHECK_EQ_I64(t, 12345);
    CHECK_EQ_U64(frc_cycle_count(CAT, 0), 18);
    e = date_of(19, 1, 1, 1, 1, 1, 0, 0, 0);
    CHECK_EQ_I64(frc_date_to_time(CAT, &e, &t), FRC_ERR_RANGE);
    e = date_of(65535, 5, 13, 5, 5, 77, 35, 64, 24);
    CHECK_EQ_I64(frc_date_to_time(CAT, &e, &t), FRC_ERR_RANGE);
    /* the last Alpha starts at E[68] */
    e = date_of(18, 1, 1, 1, 1, 1, 0, 0, 0);
    CHECK_EQ_I64(frc_date_to_time(CAT, &e, &t), FRC_OK);
    CHECK_EQ_I64(t, frc_catalogue_instant(CAT, 68));
    /* Alpha 6 starts at the anchor E[20] */
    e = date_of(6, 1, 1, 1, 1, 1, 0, 0, 0);
    CHECK_EQ_I64(frc_date_to_time(CAT, &e, &t), FRC_OK);
    CHECK_EQ_I64(t, frc_catalogue_instant(CAT, 20));

    /* outside the catalogue the clock is undefined */
    memset(&d, 0xEE, sizeof d);
    CHECK_EQ_I64(frc_date_at(CAT, first - 1, &d), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_date_at(CAT, last + 1, &d), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_date_at(CAT, INT64_MIN, &d), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_date_at(CAT, INT64_MAX, &d), FRC_ERR_RANGE);

    TEST_SECTION("date arguments");
    CHECK_EQ_I64(frc_date_at(NULL, T_A, &d), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_date_at(CAT, T_A, NULL), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_date_at(CAT, T_A, &d), FRC_OK);
    CHECK_EQ_I64(frc_date_to_time(NULL, &d, &t), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_date_to_time(CAT, NULL, &t), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_date_to_time(CAT, &d, NULL), FRC_ERR_ARG);

    /* every field, one step outside its range on both sides */
    {
        static const uint16_t LOW[FRC_UNIT_COUNT] = {1, 1, 1, 1, 1, 1, 0, 0, 0};
        static const uint16_t HIGH[FRC_UNIT_COUNT] = {0, 5, 13, 5, 5, 77, 35, 64, 24}; /* alpha: no fixed top */
        for (unit = 0; unit < FRC_UNIT_COUNT; unit++) {
            frc_date_t bad;
            CHECK_EQ_I64(frc_date_at(CAT, T_A, &d), FRC_OK);
            if (LOW[unit] > 0) {
                bad = d;
                set_date_field(&bad, unit, (uint16_t)(LOW[unit] - 1));
                CHECK_EQ_I64(frc_date_to_time(CAT, &bad, &t), FRC_ERR_ARG);
            }
            if (unit != FRC_UNIT_ALPHA) {
                bad = d;
                set_date_field(&bad, unit, (uint16_t)(HIGH[unit] + 1));
                CHECK_EQ_I64(frc_date_to_time(CAT, &bad, &t), FRC_ERR_ARG);
                bad = d;
                set_date_field(&bad, unit, 255);
                CHECK_EQ_I64(frc_date_to_time(CAT, &bad, &t), FRC_ERR_ARG);
                /* both ends of the range are dates (inside Alpha 6) */
                bad = d;
                set_date_field(&bad, unit, LOW[unit]);
                CHECK_EQ_I64(frc_date_to_time(CAT, &bad, &t), FRC_OK);
                bad = d;
                set_date_field(&bad, unit, HIGH[unit]);
                CHECK_EQ_I64(frc_date_to_time(CAT, &bad, &t), FRC_OK);
            }
        }
    }
    /* the quarters are ignored: the counts imply them */
    CHECK_EQ_I64(frc_date_at(CAT, T_A, &d), FRC_OK);
    CHECK_EQ_I64(frc_date_to_time(CAT, &d, &t), FRC_OK);
    memset(d.quarter, 77, sizeof d.quarter);
    {
        frc_time_t t2 = 0;
        CHECK_EQ_I64(frc_date_to_time(CAT, &d, &t2), FRC_OK);
        CHECK_EQ_I64(t2, t);
    }

    TEST_SECTION("date on a small catalogue");
    {
        /* three eclipses a made-up 1000 s apart: half an Alpha */
        static const int64_t TINY[3] FRC_PROGMEM = {0, 1000000, 2000000};
        frc_catalogue_t tiny;
        CHECK_EQ_I64(frc_catalogue_init(&tiny, TINY, 3, 0), FRC_OK);
        CHECK_EQ_I64(frc_date_at(&tiny, 0, &d), FRC_OK);
        e = date_of(1, 1, 1, 1, 1, 1, 0, 0, 0);
        CHECK(same_date(&d, &e));
        /* position 1 = a quarter Alpha = 1β 3γ 1δ 1ε 19η 9ι past the start,
         * and the Alpha enters its second quarter */
        CHECK_EQ_I64(frc_date_at(&tiny, FRC_TIME_FROM_MS(1000000), &d), FRC_OK);
        e = date_of(1, 2, 4, 2, 2, 20, 9, 0, 0);
        CHECK(same_numbers(&d, &e));
        CHECK_EQ_U64(d.quarter[0], FRC_NODE);
        CHECK_EQ_I64(frc_date_to_time(&tiny, &d, &t), FRC_OK);
        CHECK_EQ_I64(t, FRC_TIME_FROM_MS(1000000));
        e = date_of(1, 3, 7, 3, 3, 39, 18, 0, 0); /* position 2: the end */
        CHECK_EQ_I64(frc_date_to_time(&tiny, &e, &t), FRC_ERR_RANGE);
        e = date_of(2, 1, 1, 1, 1, 1, 0, 0, 0);
        CHECK_EQ_I64(frc_date_to_time(&tiny, &e, &t), FRC_ERR_RANGE);
    }
}

#define CALL_FORMAT_DATE_FULL(b, s)  frc_format_date(&edge_date, FRC_DATE_FULL, FRC_UNIT_BLINK, (b), (s))
#define CALL_FORMAT_DATE_STAMP(b, s) frc_format_date(&edge_date, FRC_DATE_LETTERS, FRC_UNIT_BLINK, (b), (s))
#define CALL_FORMAT_DATE_SHORT(b, s) frc_format_date(&edge_date, FRC_DATE_SHORT, FRC_UNIT_PREON, (b), (s))

static void test_format_date_edges(void)
{
    frc_date_t edge_date;
    frc_date_t d;
    char text[200];

    TEST_SECTION("frc_format_date: buffers");
    CHECK_EQ_I64(frc_date_at(CAT, T_A, &edge_date), FRC_OK);
    CHECK_EVERY_SIZE(CALL_FORMAT_DATE_FULL, ORACLE_NOTATION_S15_6[0].full_date);
    CHECK_EVERY_SIZE(CALL_FORMAT_DATE_STAMP, ORACLE_NOTATION_S15_6_STAMP_A);
    CHECK_EVERY_SIZE(CALL_FORMAT_DATE_SHORT, ORACLE_NOTATION_S15_6[0].short_date);

    TEST_SECTION("frc_format_date: arguments");
    d = edge_date;
    strcpy(text, "untouched");
    CHECK_EQ_I64(frc_format_date(NULL, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, ""); /* an error leaves an empty string */
    CHECK_EQ_I64(frc_format_date(&d, (frc_date_form_t)4, FRC_UNIT_BLINK, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_date(&d, (frc_date_form_t)-1, FRC_UNIT_BLINK, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_date(&d, FRC_DATE_FULL, FRC_UNIT_COUNT, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_date(&d, FRC_DATE_FULL, (frc_unit_t)-1, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_date(NULL, FRC_DATE_FULL, FRC_UNIT_BLINK, NULL, 0), FRC_ERR_ARG);
    d.quarter[2] = 4;
    CHECK_EQ_I64(frc_format_date(&d, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, "");

    TEST_SECTION("frc_format_date: the longest and the unnamed");
    /* the longest ordinal names, every count at its top, all quarters "Valley" */
    d = date_of(17, 5, 13, 5, 5, 77, 35, 64, 24);
    memset(d.quarter, FRC_VALLEY, sizeof d.quarter);
    CHECK(frc_format_date(&d, FRC_DATE_FULL, FRC_UNIT_BLINK, text, sizeof text) < 160);
    CHECK_STR(text, "Alpha Heptadecafracton Valley, Beta Pentafracton Valley, Gamma Tridecafracton Valley, "
                    "Delta Pentafracton Valley, \xCE\xB5#5 77\xCE\xB7 35\xCE\xB9 64\xCE\xBB 24\xCE\xBD");
    /* a longer series than Saros 141: past the twentieth name an ordinal is "#n" */
    d.alpha = 25;
    CHECK(frc_format_date(&d, FRC_DATE_FULL, FRC_UNIT_DELTA, text, sizeof text) > 0);
    CHECK_STR(text, "Alpha #25 Valley, Beta Pentafracton Valley, Gamma Tridecafracton Valley, Delta Pentafracton Valley");
    {
        frc_date_t parsed;
        frc_date_t want = truncated(&d, FRC_UNIT_DELTA);
        frc_unit_t finest;
        CHECK_EQ_I64(frc_parse_date(text, NULL, &parsed, &finest), FRC_OK);
        CHECK(same_date(&parsed, &want));
        CHECK_EQ_I64(finest, FRC_UNIT_DELTA);
    }
    d.alpha = 65535;
    CHECK(frc_format_date(&d, FRC_DATE_LETTERS, FRC_UNIT_DELTA, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB1#65535 \xCE\xB2#5 \xCE\xB3#13 \xCE\xB4#5");

    /* the words are the ones the rest of the library uses */
    {
        char word[16];
        int level, quarter;
        for (level = 0; level <= 3; level++) {
            for (quarter = 0; quarter <= 3; quarter++) {
                char want[64];
                int n;
                d = date_of(1, 1, 1, 1, 1, 1, 0, 0, 0);
                memset(d.quarter, quarter, sizeof d.quarter);
                /* the last of the majors in the full form is the Delta; use
                 * cycle names for the level word of each */
                CHECK(frc_format_level_word((uint8_t)level, word, sizeof word) > 0);
                n = snprintf(want, sizeof want, "%s Monofracton", word);
                CHECK(frc_format_cycle_name((uint8_t)level, 0, 0, text, sizeof text) == n);
                CHECK_STR(text, want);
                CHECK(frc_format_quarter((frc_quarter_t)quarter, 0, word, sizeof word) > 0);
                snprintf(want, sizeof want, "Delta Monofracton %s", word);
                CHECK(frc_format_date(&d, FRC_DATE_SHORT, FRC_UNIT_DELTA, text, sizeof text) > 0);
                CHECK_STR(text, want);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* frc_parse_date by hand                                                     */
/* ------------------------------------------------------------------------- */

static void test_parse_accepts(void)
{
    /* vector A down to the Preon, and a context that is another moment */
    frc_date_t a = date_of(6, 4, 9, 3, 5, 34, 29, 0, 0);
    frc_date_t context = date_of(7, 2, 1, 5, 2, 1, 23, 1, 21);
    frc_date_t want;

    TEST_SECTION("frc_parse_date: accepted spellings");
    /* the shorthand of fractonica.h */
    expect_parse("a#6 b#4 g#9 d#3 e#5 34h 29i", NULL, &a, FRC_UNIT_PREON);
    expect_parse("A#6 B#4 G#9 D#3 E#5 34H 29I", NULL, &a, FRC_UNIT_PREON);
    /* letters, words, and a mix of the two */
    expect_parse("\xCE\xB1#6 \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9", NULL, &a, FRC_UNIT_PREON);
    expect_parse("Alpha Hexafracton Valley, Beta Tetrafracton Valley, Gamma Nonafracton Valley, "
                 "Delta Trifracton End, \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9", NULL, &a, FRC_UNIT_PREON);
    expect_parse("alpha hexafracton beta tetrafracton gamma nonafracton delta trifracton e#5 34h 29i", NULL, &a,
                 FRC_UNIT_PREON);
    expect_parse("ALPHA HEXAFRACTON VALLEY BETA TETRAFRACTON GAMMA NONAFRACTON DELTA TRIFRACTON END E#5 34H 29I",
                 NULL, &a, FRC_UNIT_PREON);
    expect_parse("Alpha #6, Beta #4, Gamma #9, Delta #3, \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9", NULL, &a, FRC_UNIT_PREON);
    expect_parse("Alpha#6 Peak Beta#4 Node Gamma#9 Valley Delta#3 End e#5 34h 29i", NULL, &a, FRC_UNIT_PREON);
    expect_parse("\xCE\xB1#6 Valley \xCE\xB2#4 \xCE\xB3#9 \xCE\xB4#3 End \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9", NULL, &a,
                 FRC_UNIT_PREON);
    expect_parse("a#6,b#4,g#9,d#3,e#5,34h,29i", NULL, &a, FRC_UNIT_PREON);
    expect_parse("a#6b#4g#9d#3e#5 34h29i", NULL, &a, FRC_UNIT_PREON);
    /* capital Greek letters */
    expect_parse("\xCE\x91#6 \xCE\x92#4 \xCE\x93#9 \xCE\x94#3 \xCE\x95#5 34\xCE\x97 29\xCE\x99", NULL, &a, FRC_UNIT_PREON);
    /* any run of spaces: ASCII, tab, newline, U+00A0, U+202F, U+2009 */
    expect_parse("  \xCE\xB1 # 6\t\xCE\xB2#4\n\xCE\xB3#9\xC2\xA0\xCE\xB4#3\xE2\x80\xAF\xCE\xB5#5\xE2\x80\x89" "34 \xCE\xB7  29 \xCE\xB9  ",
                 NULL, &a, FRC_UNIT_PREON);

    TEST_SECTION("frc_parse_date: the fine end is the start");
    want = date_of(6, 1, 1, 1, 1, 1, 0, 0, 0);
    expect_parse("\xCE\xB1#6", NULL, &want, FRC_UNIT_ALPHA);
    expect_parse("Alpha Hexafracton", NULL, &want, FRC_UNIT_ALPHA);
    expect_parse("Alpha Hexafracton Valley", NULL, &want, FRC_UNIT_ALPHA);
    want = date_of(6, 4, 1, 1, 1, 1, 0, 0, 0);
    expect_parse("a#6 b#4", NULL, &want, FRC_UNIT_BETA);
    want = date_of(6, 4, 9, 3, 5, 1, 0, 0, 0);
    expect_parse("a#6 b#4 g#9 d#3 e#5", NULL, &want, FRC_UNIT_NUCLIDE);
    want = date_of(6, 4, 9, 3, 5, 34, 29, 21, 3);
    expect_parse("a#6 b#4 g#9 d#3 e#5 34h 29i 21l 3n", NULL, &want, FRC_UNIT_BLINK);
    /* the extremes of every field */
    want = date_of(65535, 5, 13, 5, 5, 77, 35, 64, 24);
    expect_parse("a#65535 b#5 g#13 d#5 e#5 77h 35i 64l 24n", NULL, &want, FRC_UNIT_BLINK);
    want = date_of(1, 1, 1, 1, 1, 1, 0, 0, 0);
    expect_parse("a#1 b#1 g#1 d#1 e#1 1h 0i 0l 0n", NULL, &want, FRC_UNIT_BLINK);
    expect_parse("a#0001 b#01 g#1 d#1 e#1 01h 00i 0l 000n", NULL, &want, FRC_UNIT_BLINK);
    /* every fracton name */
    {
        static const char *const NAMES[] = {
            "Monofracton", "Difracton", "Trifracton", "Tetrafracton", "Pentafracton", "Hexafracton", "Heptafracton",
            "Octafracton", "Nonafracton", "Decafracton", "Didecafracton", "Dodecafracton", "Tridecafracton",
            "Tetradecafracton", "Pentadecafracton", "Hexadecafracton", "Heptadecafracton", "Octadecafracton",
            "Nonadecafracton", "Icosafracton"};
        char text[64];
        uint16_t n;
        for (n = 1; n <= 20; n++) {
            snprintf(text, sizeof text, "Alpha %s", NAMES[n - 1]);
            want = date_of(n, 1, 1, 1, 1, 1, 0, 0, 0);
            expect_parse(text, NULL, &want, FRC_UNIT_ALPHA);
            CHECK(frc_format_ordinal(n, text, sizeof text) > 0);
            CHECK_STR(text, NAMES[n - 1]); /* S15.2, typed here by hand */
        }
    }

    TEST_SECTION("frc_parse_date: the coarse end comes from the context");
    want = date_of(7, 2, 1, 3, 5, 34, 29, 0, 0);
    expect_parse("Delta Trifracton End \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9", &context, &want, FRC_UNIT_PREON);
    expect_parse("d#3 e#5 34h 29i", &context, &want, FRC_UNIT_PREON);
    want = date_of(7, 2, 1, 5, 4, 1, 0, 0, 0);
    expect_parse("\xCE\xB5#4", &context, &want, FRC_UNIT_NUCLIDE);
    want = date_of(7, 2, 1, 5, 2, 34, 17, 0, 0);
    expect_parse("34\xCE\xB7 17\xCE\xB9", &context, &want, FRC_UNIT_PREON); /* "inside the shown Nuclide" */
    want = date_of(7, 2, 1, 5, 2, 1, 29, 0, 0);
    expect_parse("29i", &context, &want, FRC_UNIT_PREON);
    want = date_of(7, 2, 1, 5, 2, 1, 23, 1, 7);
    expect_parse("7\xCE\xBD", &context, &want, FRC_UNIT_BLINK);
    want = date_of(7, 2, 12, 1, 1, 1, 0, 0, 0);
    expect_parse("Gamma Dodecafracton", &context, &want, FRC_UNIT_GAMMA);
    /* the context's quarters do not leak into the result */
    memset(context.quarter, 3, sizeof context.quarter);
    want = date_of(7, 2, 1, 5, 2, 9, 0, 0, 0);
    expect_parse("9h", &context, &want, FRC_UNIT_HELION);
    memset(context.quarter, 0, sizeof context.quarter);

    TEST_SECTION("frc_parse_date: a leading Pentad is read past");
    want = date_of(7, 2, 1, 3, 2, 5, 5, 0, 0);
    /* the two lines of SPEC S15.3 */
    expect_parse("Pentad II \xC2\xB7 Delta Trifracton Node \xCE\xB5#2 5\xCE\xB7 5\xCE\xB9", &context, &want, FRC_UNIT_PREON);
    expect_parse("\xE2\xAC\xA0II \xCE\xB4#3 \xCE\xB5#2 5\xCE\xB7 5\xCE\xB9", &context, &want, FRC_UNIT_PREON);
    expect_parse("Pentad I \xCE\xB4#3 \xCE\xB5#2 5\xCE\xB7 5\xCE\xB9", &context, &want, FRC_UNIT_PREON);
    expect_parse("pentad iii, d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("Pentad 2 . d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("Pentad II, 3rd Helion \xC2\xB7 d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("Pentad II 1st helion, d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("Hypopentad d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("Hypopentad, 4th Helion, d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("\xE2\xAC\xA0III\xC2\xB7" "5 \xCE\xB4#3 \xCE\xB5#2 5\xCE\xB7 5\xCE\xB9", &context, &want, FRC_UNIT_PREON);
    expect_parse("\xE2\x97\xBB \xCE\xB4#3 \xCE\xB5#2 5\xCE\xB7 5\xCE\xB9", &context, &want, FRC_UNIT_PREON);
    expect_parse("\xE2\x97\xBB\xC2\xB7" "4 \xC2\xB7 \xCE\xB4#3 \xCE\xB5#2 5\xCE\xB7 5\xCE\xB9", &context, &want, FRC_UNIT_PREON);
    expect_parse("P2 d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("P2.3 d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("p1.5, d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("PII.3 d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("PIII d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("H d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("H.4 d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    expect_parse("h . d#3 e#2 5h 5i", &context, &want, FRC_UNIT_PREON);
    /* a Pentad before a full date */
    expect_parse("P3 a#6 b#4 g#9 d#3 e#5 34h 29i", NULL, &a, FRC_UNIT_PREON);
    /* A number after the dot that carries a unit letter is the date's first
     * count, not the Helion's place in the Pentad. */
    want = date_of(7, 2, 1, 5, 2, 3, 29, 0, 0);
    expect_parse("\xE2\xAC\xA0II\xC2\xB7" "3\xCE\xB7 29\xCE\xB9", &context, &want, FRC_UNIT_PREON);
    expect_parse("P2.3h 29i", &context, &want, FRC_UNIT_PREON);
    expect_parse("H.3h 29i", &context, &want, FRC_UNIT_PREON);
    want = date_of(7, 2, 1, 5, 2, 12, 29, 0, 0);
    expect_parse("P2.3 12h 29i", &context, &want, FRC_UNIT_PREON);
}

static void test_parse_rejects(void)
{
    frc_date_t context = date_of(7, 2, 1, 5, 2, 1, 23, 1, 21);
    frc_date_t bad_context;
    frc_date_t out;
    size_t i;

    static const char *const NOT_A_DATE[] = {
        /* nothing */
        "", " ", "\t\n", "\xC2\xA0", ",", ".", "#",
        /* garbage */
        "garbage", "hello world", "2026-10-08", "12:00", "now", "pos 22.9", "+2e 5h", "next Helion",
        /* a Pentad alone, or something that only starts like one */
        "Pentad II", "Pentad", "Pentad IV d#3", "Pentad 4 d#3", "Pentad 0 d#3", "Pentad d#3", "P4 d#3", "P d#3",
        "\xE2\xAC\xA0 d#3", "\xE2\xAC\xA0IV d#3", "Hypopentad", "H", "P2.3", "\xE2\x97\xBB\xC2\xB7" "3",
        "Pentad II \xC2\xB7 \xC2\xB7 d#3", "Pentad II, , d#3", "Pentad II Pentad II d#3",
        "Pentad II, 3rd d#3", "Pentad II, 6th Helion d#3", "Pentad II, 3 Helion d#3", "P2.6 d#3", "P2.0 d#3",
        /* duplicates */
        "d#3 d#3", "a#6 a#6", "34h 34h", "34h 29i 29i", "Delta Trifracton d#3", "e#5 e#5 34h",
        /* out of order */
        "e#5 d#3", "29i 34h", "b#4 a#6", "34h e#5", "3n 21l", "d#3 34h e#5",
        /* gaps */
        "a#6 g#9", "d#3 34h", "e#5 29i", "34h 21l", "a#6 b#4 g#9 d#3 34h", "a#6 d#3",
        /* the wrong kind of number for the unit */
        "5e", "34e", "h#34", "i#29", "\xCE\xB7#34", "3d", "6a", "5\xCE\xB5", "Delta 3", "d3", "a6 b4 g9", "6.4.9",
        /* levels that are not units, units that are not date parts */
        "z#3", "3z", "\xCE\xB6#3", "3\xCE\xB8", "3q", "3k", "3m", "3x", "x#3", "3\xCE\xBE", "3\xCF\x89", "3w",
        "Epsilon Trifracton", "Epsilon #3", "Nuclide #3", "34 Helion", "34 Helions", "Zeta Trifracton",
        /* broken parts */
        "d#", "d", "#3", "34", "d##3", "d#3#", "d#-3", "d#+3", "d#3.5 e#1", "34.5h", "-34h", "\xE2\x88\x92" "34h",
        "d#three", "Delta", "Delta End", "Delta Trifracton End End", "Delta End Trifracton",
        "Delta Trifractons", "Delta Trifracto", "Delta fracton", "Delta Triffracton", "Delta Unfracton",
        "Delta Trifracton Middle", "Delta Trifracton Begin", "Peak", "End", "Trifracton", "Delta, Trifracton",
        "d#3 Peak Peak", "e#5 Peak", "34h Peak",
        /* commas in the wrong place, trailing things */
        ",d#3", "d#3,", "d#3,,e#5", "d#3 , , e#5", "d#3 e#5 34h garbage", "d#3 e#5 34h 29i 21l 3n 7", "d#3;e#5",
        "d#3 e#5 34h!", "d#3 e#5 34h 29i 21l 3n 7\xCE\xBE", "d#3 e#5\x01", "d#3/e#5", "d#3 \xC2\xB7 e#5",
        /* bytes that are not text */
        "\xFF", "d#3\xFF", "\xCE", "34\xCE", "\xE2\xAC", "\xE2\xAC\xA0", "\xC0\x80", "\xC1\xA4#3", "\xE0\x80\xA4#3",
        "34\xED\xA0\x80", "\xF8\x88\x80\x80\x80", "d#3 \x80",
        /* overlong forms of a real letter (alpha in three and in four bytes) */
        "\xE0\x8E\xB1#6", "\xF0\x80\x8E\xB1#6", "\xE0\x8E\xB1#6 b#4",
    };
    static const char *const OUT_OF_RANGE[] = {
        "a#0", "a#0 b#1", "a#65536", "a#99999999999999999999999999",
        "b#0", "b#6", "g#0", "g#14", "d#0", "d#6", "e#0", "e#6",
        "0h", "78h", "36i", "65l", "25n", "999999999999999999999999999999h", "4294967296h", "4294967330h",
        "Beta Hexafracton", "Delta Hexafracton", "Gamma Tetradecafracton", "Alpha #0", "Delta #0",
        "a#6 b#4 g#9 d#3 e#5 34h 29i 21l 25n", "a#6 b#4 g#9 d#3 e#5 0h", "d#3 e#5 78h 29i", "P2 d#9 e#5",
    };

    TEST_SECTION("frc_parse_date: not a date");
    for (i = 0; i < sizeof NOT_A_DATE / sizeof NOT_A_DATE[0]; i++) {
        expect_reject(NOT_A_DATE[i], &context, FRC_ERR_PARSE);
        expect_reject(NOT_A_DATE[i], NULL, FRC_ERR_PARSE);
    }

    TEST_SECTION("frc_parse_date: a field out of range");
    for (i = 0; i < sizeof OUT_OF_RANGE / sizeof OUT_OF_RANGE[0]; i++) {
        expect_reject(OUT_OF_RANGE[i], &context, FRC_ERR_ARG);
    }
    /* text that is not a date stays FRC_ERR_PARSE whatever its numbers are */
    expect_reject("78h garbage", &context, FRC_ERR_PARSE);
    expect_reject("d#9 d#9", &context, FRC_ERR_PARSE);
    expect_reject("e#9 d#3", &context, FRC_ERR_PARSE);
    /* a short form without a context is not a (full) date */
    expect_reject("d#3 e#5 34h 29i", NULL, FRC_ERR_PARSE);
    expect_reject("34h", NULL, FRC_ERR_PARSE);
    expect_reject("78h", NULL, FRC_ERR_PARSE);
    expect_reject("Delta Trifracton End \xCE\xB5#5 34\xCE\xB7 29\xCE\xB9", NULL, FRC_ERR_PARSE);
    /* a context whose own fields are out of range cannot complete a date */
    bad_context = context;
    bad_context.beta = 0;
    expect_reject("34h", &bad_context, FRC_ERR_ARG);
    bad_context = context;
    bad_context.alpha = 0;
    expect_reject("d#3", &bad_context, FRC_ERR_ARG);
    bad_context = context;
    bad_context.epsilon = 6;
    expect_reject("34h", &bad_context, FRC_ERR_ARG);
    /* ... but the fields the text replaces are not looked at */
    bad_context = context;
    bad_context.epsilon = 6;
    bad_context.nu = 200;
    CHECK_EQ_I64(frc_parse_date("e#1 2h", &bad_context, &out, NULL), FRC_OK);
    CHECK_EQ_I64(frc_parse_date("a#6", &bad_context, &out, NULL), FRC_OK);

    /* A sequence cut short by the NUL is not text, and nothing after the NUL
     * is read: here the bytes behind it would complete U+2009 (a space) and
     * a second part. */
    {
        static const char CUT[] = "d#3\xE2\0\x89" "e#5";
        CHECK_EQ_U64(strlen(CUT), 4);
        expect_reject(CUT, &context, FRC_ERR_PARSE);
    }

    TEST_SECTION("frc_parse_date: arguments");
    CHECK_EQ_I64(frc_parse_date(NULL, &context, &out, NULL), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_parse_date("d#3", &context, NULL, NULL), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_parse_date(NULL, NULL, NULL, NULL), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_parse_date("d#3", &context, &out, NULL), FRC_OK); /* finest may be NULL */
}

/* Random and mutated text never crashes the parser, never reads past the
 * NUL, and whatever it accepts is a date whose fields are in range. Each text
 * is parsed from a heap block of exactly its size, so that a build with
 * -fsanitize=address catches a single byte read after the NUL. */
static void test_parse_fuzz(void)
{
    static const char *const PIECES[] = {
        "a", "b", "g", "d", "e", "h", "i", "l", "n", "#", " ", ",", ".", "0", "1", "3", "5", "9", "34", "77",
        "\xCE\xB1", "\xCE\xB4", "\xCE\xB5", "\xCE\xB7", "\xCE\xB9", "\xCE\xBB", "\xCE\xBD", "\xC2\xB7", "\xE2\xAC\xA0",
        "\xE2\x97\xBB", "Delta ", "Alpha ", "Trifracton ", "Peak ", "End ", "Pentad ", "II", "P2", "H", "rd Helion",
        "\xFF", "\xCE", "\xE2", "\xE2\x80\xAF", "-", "Hypopentad ", "fracton", "x", "z",
    };
    const size_t piece_count = sizeof PIECES / sizeof PIECES[0];
    frc_date_t context = date_of(7, 2, 1, 5, 2, 1, 23, 1, 21);
    char text[160];
    char *exact;
    int i, accepted = 0;

    TEST_SECTION("frc_parse_date: fuzz");
    for (i = 0; i < 200000; i++) {
        frc_date_t out;
        frc_unit_t finest = FRC_UNIT_COUNT;
        frc_status_t status;
        size_t length = 0;
        int pieces = 1 + (int)(rng() % 9);

        text[0] = '\0';
        if (i % 3 == 0) {
            /* a valid stamp with one byte damaged or one piece spliced in */
            frc_date_t d;
            size_t at;
            CHECK_EQ_I64(frc_date_at(CAT, rng_instant(), &d), FRC_OK);
            frc_format_date(&d, (frc_date_form_t)(rng() % 4), (frc_unit_t)(3 + rng() % 6), text, 100);
            length = strlen(text);
            at = (size_t)(rng() % (length + 1));
            if (rng() % 2) {
                text[at] = (char)(rng() % 255 + 1);
                if (at == length) {
                    text[at + 1] = '\0';
                }
            } else {
                const char *piece = PIECES[rng() % piece_count];
                memmove(text + at + strlen(piece), text + at, length - at + 1);
                memcpy(text + at, piece, strlen(piece));
            }
        } else {
            while (pieces-- > 0) {
                const char *piece = PIECES[rng() % piece_count];
                size_t n = strlen(piece);
                memcpy(text + length, piece, n + 1);
                length += n;
            }
        }
        memset(&out, 0xEE, sizeof out);
        length = strlen(text);
        exact = malloc(length + 1);
        if (exact == NULL) {
            CHECK(exact != NULL);
            return;
        }
        memcpy(exact, text, length + 1);
        status = frc_parse_date(exact, (i % 5 == 0) ? NULL : &context, &out, &finest);
        free(exact);
        CHECK(status == FRC_OK || status == FRC_ERR_PARSE || status == FRC_ERR_ARG);
        if (status == FRC_OK) {
            accepted++;
            CHECK(out.alpha >= 1 && out.beta >= 1 && out.beta <= 5 && out.gamma >= 1 && out.gamma <= 13 &&
                  out.delta >= 1 && out.delta <= 5 && out.epsilon >= 1 && out.epsilon <= 5 && out.eta >= 1 &&
                  out.eta <= 77 && out.iota <= 35 && out.lambda <= 64 && out.nu <= 24);
            CHECK_NOTE("text \"%s\"", text);
            CHECK(out.quarter[0] == 0 && out.quarter[1] == 0 && out.quarter[2] == 0 && out.quarter[3] == 0);
            CHECK((int)finest >= 0 && finest < FRC_UNIT_COUNT);
        } else {
            CHECK(out.alpha == 0xEEEE && finest == FRC_UNIT_COUNT);
        }
    }
    /* the mix must exercise both outcomes to mean anything */
    CHECK(accepted > 1000);
    CHECK_NOTE("only %d of the fuzz texts were accepted", accepted);
    CHECK(accepted < 190000);
    CHECK_NOTE("%d of the fuzz texts were accepted", accepted);
}

/* ------------------------------------------------------------------------- */
/* Ordinals                                                                   */
/* ------------------------------------------------------------------------- */

#define CALL_ORDINAL_17(b, s) frc_format_ordinal(17, (b), (s))
#define CALL_ORDINAL_HASH(b, s) frc_format_ordinal(65535, (b), (s))

static void test_ordinals(void)
{
    char text[40];
    size_t i;

    TEST_SECTION("oracle ordinals");
    for (i = 0; i < ORACLE_NOTATION_ORDINALS_COUNT; i++) {
        const oracle_notation_ordinal_t *o = &ORACLE_NOTATION_ORDINALS[i];
        CHECK_EQ_I64(frc_format_ordinal(o->n, text, sizeof text), (int)strlen(o->text));
        CHECK_STR(text, o->text);
    }
    /* SPEC S15.2: "Delta Trifracton is the 3rd Delta"; 11 is the owner's coinage */
    CHECK(frc_format_ordinal(3, text, sizeof text) > 0);
    CHECK_STR(text, "Trifracton");
    CHECK(frc_format_ordinal(11, text, sizeof text) > 0);
    CHECK_STR(text, "Didecafracton");
    CHECK(frc_format_ordinal(18, text, sizeof text) > 0);
    CHECK_STR(text, "Octadecafracton");
    /* outside the table */
    CHECK(frc_format_ordinal(0, text, sizeof text) > 0);
    CHECK_STR(text, "#0");
    CHECK(frc_format_ordinal(21, text, sizeof text) > 0);
    CHECK_STR(text, "#21");
    CHECK(frc_format_ordinal(65535, text, sizeof text) > 0);
    CHECK_STR(text, "#65535");
    CHECK_EVERY_SIZE(CALL_ORDINAL_17, "Heptadecafracton");
    CHECK_EVERY_SIZE(CALL_ORDINAL_HASH, "#65535");
}

/* ------------------------------------------------------------------------- */
/* Cycle names                                                                */
/* ------------------------------------------------------------------------- */

#define CALL_NAME_LONG(b, s)  frc_format_cycle_name(14, UINT64_C(3718950786756), 0, (b), (s))
#define CALL_NAME_SHORT(b, s) frc_format_cycle_name(14, UINT64_C(3718950786756), 1, (b), (s))
#define CALL_NAME_MAJOR(b, s) frc_format_cycle_name(3, 2032, 0, (b), (s))

static void test_cycle_names(void)
{
    char text[100];
    char want[100];
    size_t i;
    int level;

    TEST_SECTION("oracle cycle names");
    for (i = 0; i < ORACLE_NOTATION_CYCLE_NAMES_COUNT; i++) {
        const oracle_notation_cycle_name_t *o = &ORACLE_NOTATION_CYCLE_NAMES[i];
        CHECK_EQ_I64(frc_format_cycle_name(o->level, o->cycle, 0, text, sizeof text), (int)strlen(o->name));
        CHECK_STR(text, o->name);
        CHECK_NOTE("level %u cycle %" PRIu64, o->level, o->cycle);
        CHECK_EQ_I64(frc_format_cycle_name(o->level, o->cycle, 1, text, sizeof text), (int)strlen(o->short_form));
        CHECK_STR(text, o->short_form);
        CHECK_NOTE("level %u cycle %" PRIu64 " (short)", o->level, o->cycle);
        CHECK_EQ_I64(frc_format_cycle_name(o->level, o->cycle, 7, text, sizeof text), (int)strlen(o->short_form));
    }

    TEST_SECTION("cycle names by hand (SPEC S15.3)");
    /* "a depth-9 cycle is ε#2 5η 9ι": level 8; the Preon numbered
     * (1*77 + 4)*36 + 9 inside its Delta */
    CHECK(frc_format_cycle_name(8, (1 * 77 + 4) * 36 + 9, 0, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB5#2 5\xCE\xB7 9\xCE\xB9");
    /* "a depth-8 cycle ε#2 5η 8ι (θ starts on multiples of 4ι)": level 7 */
    CHECK(frc_format_cycle_name(7, (1 * 77 + 4) * 9 + 2, 0, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB5#2 5\xCE\xB7 8\xCE\xB9");
    /* "a depth-14 cycle … 3.43ν": level 13, the 4th xi of the Blink after 3 */
    CHECK(frc_format_cycle_name(13, 3 * 7 + 3, 0, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB5#1 1\xCE\xB7 0\xCE\xB9 0\xCE\xBB 3.43\xCE\xBD");
    CHECK(frc_format_cycle_name(13, 3 * 7 + 3, 1, text, sizeof text) > 0);
    CHECK_STR(text, "3.43\xCE\xBD");
    /* fractonica.h: "Delta Trifracton", "ε#2", "δ#3", "9ι" */
    CHECK(frc_format_cycle_name(3, 2, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Delta Trifracton");
    CHECK(frc_format_cycle_name(3, 2, 1, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB4#3");
    CHECK(frc_format_cycle_name(4, 1, 0, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB5#2");
    CHECK(frc_format_cycle_name(4, 1, 1, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB5#2");
    CHECK(frc_format_cycle_name(8, (1 * 77 + 4) * 36 + 9, 1, text, sizeof text) > 0);
    CHECK_STR(text, "9\xCE\xB9");
    /* pure.js: "ζ cycles begin at Helions 1, 12, 23 … 67" */
    for (i = 0; i < 7; i++) {
        snprintf(want, sizeof want, "\xCE\xB5#1 %d\xCE\xB7", 1 + 11 * (int)i);
        CHECK(frc_format_cycle_name(5, i, 0, text, sizeof text) > 0);
        CHECK_STR(text, want);
    }
    /* an Alpha is numbered globally; past the twentieth name "#n" */
    CHECK(frc_format_cycle_name(0, 5, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Alpha Hexafracton");
    CHECK(frc_format_cycle_name(0, 19, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Alpha Icosafracton");
    CHECK(frc_format_cycle_name(0, 24, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Alpha #25");
    CHECK(frc_format_cycle_name(0, 24, 1, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB1#25");
    CHECK(frc_format_cycle_name(0, UINT64_MAX - 1, 1, text, sizeof text) > 0);
    CHECK_STR(text, "\xCE\xB1#18446744073709551615");
    /* the others inside their parent: 5 Betas, 13 Gammas, 5 Deltas */
    CHECK(frc_format_cycle_name(1, 5 * 7 + 4, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Beta Pentafracton");
    CHECK(frc_format_cycle_name(2, 13 * 9 + 12, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Gamma Tridecafracton");
    CHECK(frc_format_cycle_name(3, 5 * 1000 + 0, 0, text, sizeof text) > 0);
    CHECK_STR(text, "Delta Monofracton");

    TEST_SECTION("cycle names: sub-Blink decimals");
    /* level 13 has 7 cycles in a Blink: k/7 to two decimals, zeros dropped */
    {
        static const char *const SEVENTHS[7] = {"0", "0.14", "0.29", "0.43", "0.57", "0.71", "0.86"};
        for (i = 0; i < 7; i++) {
            snprintf(want, sizeof want, "%s\xCE\xBD", SEVENTHS[i]);
            CHECK(frc_format_cycle_name(13, i, 1, text, sizeof text) > 0);
            CHECK_STR(text, want);
        }
    }
    /* level 16 has 2772 in a Blink: 0.1 (277.2 -> the nearest hundredth),
     * 0.5 exactly, and 2771/2772 = 0.9996, which the two decimals of the
     * reference turn into the next whole Blink */
    CHECK(frc_format_cycle_name(16, 1386, 1, text, sizeof text) > 0);
    CHECK_STR(text, "0.5\xCE\xBD");
    CHECK(frc_format_cycle_name(16, 277, 1, text, sizeof text) > 0);
    CHECK_STR(text, "0.1\xCE\xBD");
    CHECK(frc_format_cycle_name(16, 14, 1, text, sizeof text) > 0);
    CHECK_STR(text, "0.01\xCE\xBD");
    CHECK(frc_format_cycle_name(16, 13, 1, text, sizeof text) > 0);
    CHECK_STR(text, "0\xCE\xBD"); /* 13/2772 = 0.0047 */
    CHECK(frc_format_cycle_name(16, 2771, 1, text, sizeof text) > 0);
    CHECK_STR(text, "1\xCE\xBD");
    CHECK(frc_format_cycle_name(16, 2772 * 24 + 2771, 1, text, sizeof text) > 0);
    CHECK_STR(text, "25\xCE\xBD");
    CHECK(frc_format_cycle_name(16, 2772 * 10 + 2772 / 4, 1, text, sizeof text) > 0);
    CHECK_STR(text, "10.25\xCE\xBD");

    TEST_SECTION("cycle names: the running cycle agrees with the date");
    for (i = 0; i < 4000; i++) {
        frc_time_t t = rng_instant();
        for (level = 0; level <= 12; level++) {
            frc_cycle_t c;
            frc_date_t d;
            char word[16];
            int n = 0;

            CHECK_EQ_I64(frc_cycle_at(CAT, t, (uint8_t)level, &c), FRC_OK);
            /* the name says where the cycle STARTS: read the date there */
            CHECK_EQ_I64(frc_date_at(CAT, c.start, &d), FRC_OK);
            if (level <= 3) {
                char ordinal[32];
                uint16_t scratch;
                uint16_t value = *date_field(&d, level, &scratch);
                CHECK(frc_format_level_word((uint8_t)level, word, sizeof word) > 0);
                CHECK(frc_format_ordinal(value, ordinal, sizeof ordinal) > 0);
                snprintf(want, sizeof want, "%s %s", word, ordinal);
            } else {
                n = snprintf(want, sizeof want, "\xCE\xB5#%u", d.epsilon);
                if (level >= 5) { n += snprintf(want + n, sizeof want - (size_t)n, " %u\xCE\xB7", d.eta); }
                if (level >= 7) { n += snprintf(want + n, sizeof want - (size_t)n, " %u\xCE\xB9", d.iota); }
                if (level >= 9) { n += snprintf(want + n, sizeof want - (size_t)n, " %u\xCE\xBB", d.lambda); }
                if (level >= 11) { n += snprintf(want + n, sizeof want - (size_t)n, " %u\xCE\xBD", d.nu); }
            }
            CHECK(frc_format_cycle_name((uint8_t)level, c.cycle, 0, text, sizeof text) > 0);
            CHECK_STR(text, want);
            CHECK_NOTE("level %d cycle %" PRIu64, level, c.cycle);
        }
    }

    TEST_SECTION("cycle names: any index, every level");
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        /* cycles of the level in one Delta: the name below the Delta repeats with it */
        uint64_t per_delta = level >= 3 ? frc_prefix((uint8_t)level) / frc_prefix(3) : 0;
        for (i = 0; i < 300; i++) {
            uint64_t cycle = rng() >> (rng() % 64);
            char again[100];
            int n = frc_format_cycle_name((uint8_t)level, cycle, 0, text, sizeof text);
            CHECK(n > 0 && n < 60);
            CHECK(frc_format_cycle_name((uint8_t)level, cycle, 1, again, sizeof again) > 0);
            if (level >= 4 && cycle < UINT64_MAX - per_delta) {
                CHECK(frc_format_cycle_name((uint8_t)level, cycle + per_delta, 0, again, sizeof again) == n);
                CHECK_STR(again, text);
            }
        }
        CHECK(frc_format_cycle_name((uint8_t)level, UINT64_MAX - 1, 0, text, sizeof text) > 0);
        if (level > 0) {
            CHECK(frc_format_cycle_name((uint8_t)level, UINT64_MAX, 0, text, sizeof text) > 0);
        }
    }

    TEST_SECTION("cycle names: arguments and buffers");
    strcpy(text, "untouched");
    CHECK_EQ_I64(frc_format_cycle_name(FRC_CLOCK_MAX_LEVEL + 1, 0, 0, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_format_cycle_name(62, 0, 0, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_cycle_name(255, 0, 1, text, sizeof text), FRC_ERR_ARG);
    /* the ordinal of Alpha 2^64 - 1 would be 2^64 */
    CHECK_EQ_I64(frc_format_cycle_name(0, UINT64_MAX, 0, text, sizeof text), FRC_ERR_OVERFLOW);
    CHECK_EQ_I64(frc_format_cycle_name(0, UINT64_MAX, 1, text, sizeof text), FRC_ERR_OVERFLOW);
    CHECK_EVERY_SIZE(CALL_NAME_LONG, "\xCE\xB5#3 14\xCE\xB7 32\xCE\xB9 26\xCE\xBB 15.66\xCE\xBD");
    CHECK_EVERY_SIZE(CALL_NAME_SHORT, "15.66\xCE\xBD");
    CHECK_EVERY_SIZE(CALL_NAME_MAJOR, "Delta Trifracton");
}

/* ------------------------------------------------------------------------- */
/* Pentads                                                                    */
/* ------------------------------------------------------------------------- */

static frc_pentad_t edge_pentad;
#define CALL_PENTAD_FULL(b, s)  frc_format_pentad(&edge_pentad, FRC_PENTAD_FULL, 1, (b), (s))
#define CALL_PENTAD_SHORT(b, s) frc_format_pentad(&edge_pentad, FRC_PENTAD_SHORT, 1, (b), (s))
#define CALL_PENTAD_ASCII(b, s) frc_format_pentad(&edge_pentad, FRC_PENTAD_ASCII, 1, (b), (s))

static void test_pentads(void)
{
    /* every Pentad and every place in it, typed by hand from pure.js */
    static const char *const FULL[4] = {"Pentad I", "Pentad II", "Pentad III", "Hypopentad"};
    static const char *const BRIEF[4] = {"\xE2\xAC\xA0I", "\xE2\xAC\xA0II", "\xE2\xAC\xA0III", "\xE2\x97\xBB"};
    static const char *const ASCII[4] = {"P1", "P2", "P3", "H"};
    static const char *const PLACE[5] = {"1st", "2nd", "3rd", "4th", "5th"};
    frc_pentad_t p;
    char text[64];
    char want[64];
    char ascii[64];
    int index, helion;

    TEST_SECTION("frc_format_pentad by hand");
    memset(&p, 0, sizeof p);
    for (index = 0; index < 4; index++) {
        p.index = (uint8_t)index;
        p.helions = index == 3 ? 4 : 5;
        p.helion = 0; /* as frc_pentad_by_number leaves it */
        CHECK(frc_format_pentad(&p, FRC_PENTAD_FULL, 0, text, sizeof text) > 0);
        CHECK_STR(text, FULL[index]);
        CHECK(frc_format_pentad(&p, FRC_PENTAD_SHORT, 0, text, sizeof text) > 0);
        CHECK_STR(text, BRIEF[index]);
        CHECK(frc_format_pentad(&p, FRC_PENTAD_ASCII, 0, text, sizeof text) > 0);
        CHECK_STR(text, ASCII[index]);
        /* frc_to_ascii of the short form is what the parser reads as "PII" */
        CHECK(frc_to_ascii(BRIEF[index], ascii, sizeof ascii) > 0);
        CHECK(ascii[0] == (index == 3 ? 'H' : 'P'));
        /* no Helion to place */
        CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_FULL, 1, text, sizeof text), FRC_ERR_ARG);
        CHECK_STR(text, "");

        for (helion = 1; helion <= p.helions; helion++) {
            p.helion = (uint8_t)helion;
            snprintf(want, sizeof want, "%s, %s Helion", FULL[index], PLACE[helion - 1]);
            CHECK(frc_format_pentad(&p, FRC_PENTAD_FULL, 1, text, sizeof text) > 0);
            CHECK_STR(text, want);
            snprintf(want, sizeof want, "%s\xC2\xB7%d", BRIEF[index], helion);
            CHECK(frc_format_pentad(&p, FRC_PENTAD_SHORT, 1, text, sizeof text) > 0);
            CHECK_STR(text, want);
            snprintf(want, sizeof want, "%s.%d", ASCII[index], helion);
            CHECK(frc_format_pentad(&p, FRC_PENTAD_ASCII, 1, text, sizeof text) > 0);
            CHECK_STR(text, want);
        }
    }
    /* the examples of fractonica.h */
    p.index = 1;
    p.helion = 3;
    CHECK(frc_format_pentad(&p, FRC_PENTAD_FULL, 1, text, sizeof text) > 0);
    CHECK_STR(text, "Pentad II, 3rd Helion");
    CHECK(frc_format_pentad(&p, FRC_PENTAD_SHORT, 1, text, sizeof text) > 0);
    CHECK_STR(text, "\xE2\xAC\xA0II\xC2\xB7" "3");
    CHECK(frc_format_pentad(&p, FRC_PENTAD_ASCII, 1, text, sizeof text) > 0);
    CHECK_STR(text, "P2.3");

    TEST_SECTION("frc_format_pentad: FRC_PENTAD_NUMERAL");
    {
        static const char *const NUMERAL[4] = {"I", "II", "III", ""};
        for (index = 0; index < 4; index++) {
            p.index = (uint8_t)index;
            for (helion = 0; helion <= 5; helion++) {
                p.helion = (uint8_t)helion;
                memset(text, '#', sizeof text);
                /* the numeral alone; the Hypopentad's is the empty text */
                CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_NUMERAL, 0, text, sizeof text), (int)strlen(NUMERAL[index]));
                CHECK_STR(text, NUMERAL[index]);
                /* with_helion is checked as in every form and adds nothing */
                CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_NUMERAL, 1, text, sizeof text),
                             helion >= 1 ? (int)strlen(NUMERAL[index]) : FRC_ERR_ARG);
                if (helion >= 1) {
                    CHECK_STR(text, NUMERAL[index]);
                }
                /* and it is the short form without its sign */
                if (index < 3) {
                    char brief[32];
                    CHECK(frc_format_pentad(&p, FRC_PENTAD_SHORT, 0, brief, sizeof brief) > 0);
                    CHECK_STR(brief + 3, NUMERAL[index]);
                }
            }
        }
        p.index = 3;
        CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_NUMERAL, 0, text, 1), 0);
        CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_NUMERAL, 0, text, 0), FRC_ERR_BUFFER);
        p.index = 2;
        CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_NUMERAL, 0, text, 3), FRC_ERR_BUFFER);
        CHECK_STR(text, "II");
        CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_NUMERAL, 0, text, 4), 3);
        p.index = 1;
        p.helion = 3;
    }

    TEST_SECTION("frc_format_pentad: arguments and buffers");
    CHECK_EQ_I64(frc_format_pentad(NULL, FRC_PENTAD_FULL, 0, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_pentad(&p, (frc_pentad_form_t)4, 0, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_pentad(&p, (frc_pentad_form_t)-1, 0, text, sizeof text), FRC_ERR_ARG);
    p.index = 4;
    CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_FULL, 0, text, sizeof text), FRC_ERR_ARG);
    p.index = 1;
    p.helion = 6;
    CHECK_EQ_I64(frc_format_pentad(&p, FRC_PENTAD_SHORT, 1, text, sizeof text), FRC_ERR_ARG);
    CHECK(frc_format_pentad(&p, FRC_PENTAD_SHORT, 0, text, sizeof text) > 0); /* not asked for: not checked */

    memset(&edge_pentad, 0, sizeof edge_pentad);
    edge_pentad.index = 2;
    edge_pentad.helion = 2;
    edge_pentad.helions = 5;
    CHECK_EVERY_SIZE(CALL_PENTAD_FULL, "Pentad III, 2nd Helion");
    CHECK_EVERY_SIZE(CALL_PENTAD_SHORT, "\xE2\xAC\xA0III\xC2\xB7" "2");
    CHECK_EVERY_SIZE(CALL_PENTAD_ASCII, "P3.2");
    edge_pentad.index = 3;
    CHECK_EVERY_SIZE(CALL_PENTAD_SHORT, "\xE2\x97\xBB\xC2\xB7" "2");
}


/* ------------------------------------------------------------------------- */
/* frc_format_date_term                                                        */
/* ------------------------------------------------------------------------- */

static void test_date_terms(void)
{
    frc_date_t date;
    char stamp[160], joined[160], term[32];
    int i;
    uint8_t unit;

    TEST_SECTION("frc_format_date_term: the nine terms are the stamp");
    for (i = 0; i < 50000; i++) {
        size_t o = 0;
        CHECK(frc_date_at(CAT, rng_instant(), &date) == FRC_OK);
        CHECK(frc_format_date(&date, FRC_DATE_LETTERS, FRC_UNIT_BLINK, stamp, sizeof stamp) > 0);
        joined[0] = '\0';
        for (unit = 0; unit < FRC_UNIT_COUNT; unit++) {
            int n = frc_format_date_term(&date, (frc_unit_t)unit, term, sizeof term);
            CHECK(n > 0 && (size_t)n == strlen(term));
            o += (size_t)snprintf(joined + o, sizeof joined - o, "%s%s", unit ? " " : "", term);
        }
        CHECK_STR(joined, stamp);
        /* and the short-letter form's terms, from the Delta down to any finest unit */
        date.quarter[0] = 7; /* quarters are not read */
        CHECK(frc_format_date_term(&date, FRC_UNIT_HELION, term, sizeof term) > 0);
    }
    /* the examples of fractonica.h: the stamp of S12 vector A */
    CHECK(frc_date_at(CAT, FRC_TIME_FROM_S(INT64_C(1791460800)), &date) == FRC_OK);
    CHECK(frc_format_date_term(&date, FRC_UNIT_ALPHA, term, sizeof term) > 0);
    CHECK_STR(term, "\xCE\xB1#6");
    CHECK(frc_format_date_term(&date, FRC_UNIT_NUCLIDE, term, sizeof term) > 0);
    CHECK_STR(term, "\xCE\xB5#5");
    CHECK(frc_format_date_term(&date, FRC_UNIT_HELION, term, sizeof term) > 0);
    CHECK_STR(term, "34\xCE\xB7");
    CHECK(frc_format_date_term(&date, FRC_UNIT_PREON, term, sizeof term) > 0);
    CHECK_STR(term, "29\xCE\xB9");
    CHECK(frc_format_date_term(&date, FRC_UNIT_BLINK, term, sizeof term) > 0);
    CHECK_STR(term, "3\xCE\xBD");
    /* the longest: an Alpha of five digits */
    date.alpha = 65535;
    CHECK_EQ_I64(frc_format_date_term(&date, FRC_UNIT_ALPHA, term, sizeof term), 8);
    CHECK_STR(term, "\xCE\xB1#65535");
    CHECK_EQ_I64(frc_format_date_term(&date, FRC_UNIT_ALPHA, term, 8), FRC_ERR_BUFFER);
    CHECK_STR(term, "\xCE\xB1#6553");
    CHECK_EQ_I64(frc_format_date_term(&date, FRC_UNIT_ALPHA, term, 2), FRC_ERR_BUFFER);
    CHECK_STR(term, ""); /* never half a letter */
    CHECK_EQ_I64(frc_format_date_term(&date, FRC_UNIT_ALPHA, NULL, 0), FRC_ERR_BUFFER);
    /* refusals: "" */
    snprintf(term, sizeof term, "junk");
    CHECK_EQ_I64(frc_format_date_term(&date, FRC_UNIT_COUNT, term, sizeof term), FRC_ERR_ARG);
    CHECK_STR(term, "");
    CHECK_EQ_I64(frc_format_date_term(&date, (frc_unit_t)-1, term, sizeof term), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_date_term(NULL, FRC_UNIT_BLINK, term, sizeof term), FRC_ERR_ARG);
}

int main(void)
{
    test_spec_vectors();
    test_oracle_dates();
    test_oracle_parse();
    test_oracle_pentads();
    test_date_round_trips();
    test_date_edges();
    test_format_date_edges();
    test_parse_accepts();
    test_parse_rejects();
    test_parse_fuzz();
    test_ordinals();
    test_cycle_names();
    test_pentads();
    test_date_terms();
    return TEST_SUMMARY("notation");
}
