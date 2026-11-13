/*
 * frc_span.c - spans, time-lapse rates and cycles per Blink.
 *
 * Implements SPEC S15.4 (spans) and S15.5 (real time: Blinks and pB). The
 * reference for every wording is reference/js/src/core/pure.js (decompose,
 * durationPos, blinkCount, between, cycleLength, RATES, pbText, groupDigits).
 *
 *   frc_span_between, frc_span_of_level   measure a span
 *   frc_span_parts, frc_format_span       count it over the unit ladder, write it
 *   frc_format_span_signed                "+6ι 43λ", "−20ι 21λ"
 *   frc_format_span_in_unit               "−6 236λ": whole units of one unit, cut
 *   frc_span_next_change                  when a countdown's text changes
 *   frc_span_next_change_since            when the text of a span since an instant changes
 *   frc_rate_preset, frc_format_rate      "4ι per Blink"
 *   frc_level_per_blink, frc_format_per_blink   "2 772 pB", "1 per 58 500 Blinks"
 *
 * THE MODEL
 *   A span is a length of POSITION, not of seconds, so it stays exact across
 *   eclipses (the Saros intervals differ in length). It is kept in Blinks:
 *   one Saros is P(12) / 4 = 1,829,953,125 Blinks. frc_span_t holds the whole
 *   Blinks and the rest of one as a 0.32 fraction.
 *
 * INVARIANTS
 *   - Exact rational arithmetic, no doubles. Where the reference rounds a
 *     double (toPrecision, toFixed, Math.round), the same rounding is done on
 *     the exact rational, a half going up.
 *   - THE TIME GRAIN. An instant is a whole microsecond, and an instant the
 *     API returned is a boundary rounded UP to one, so the span between two
 *     instants is known to within a microsecond at each end and no better.
 *     frc_span_between therefore reads a span that comes within one
 *     microsecond of a whole number of quarter Blinks as that number
 *     (snap_to_grid). The quarter-Blink grid holds every cycle start and
 *     threshold of the levels 0..12, every sync, Pentad and date: so the span
 *     from a cycle's start to its end is the cycle's length exactly ("1ι",
 *     never "64λ 24ν"). This is the core's counterpart of the reference's
 *     EPS_POS, 1e-12 Saros added before counting; it is 570 times smaller
 *     (one microsecond instead of 0.57 ms), so a span one millisecond short
 *     of 25 Blinks still reads "24ν" here and "1λ" in the reference.
 *     The microsecond in question is the one that ENDS at the instant; for
 *     an instant exactly on an eclipse it lies in the Saros interval before
 *     it and has that interval's length in position (blink_point_t). No span
 *     between two different instants is brought to nothing.
 *   - The catalogue's closing instant sits at its true place, position
 *     count - 1, when a span is measured (frc_position_at reads it one
 *     microsecond early for the clock; a span to it must not lose that).
 *   - The texts of a span depend on frc_span_t alone, and they are the texts
 *     of the exact span: the fraction is cut to 2^-32 Blink (72 picoseconds)
 *     on the side of every rounding mark that the exact value is on
 *     (frc_i_blink_fraction), so no tie and no threshold is read from the
 *     wrong side.
 */
#include "frc_internal.h"

/* 1.0 as a frc_frac_t would be: the denominator of the 0.32 fraction. */
#define FRAC_ONE ((uint64_t)1 << 32)

/* ------------------------------------------------------------------------- */
/* Rounding                                                                   */
/* ------------------------------------------------------------------------- */

/* a * b / c rounded to the nearest integer, a half going up: what
 * Math.round does, on the exact value. REQUIRES c != 0 and a result that
 * fits 64 bits (each call site says why). */
static uint64_t muldiv_round(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t rem;
    uint64_t q = frc_i_muldiv_floor(a, b, c, &rem);
    /* rem / c >= 1/2  <=>  rem >= c - rem; rem < c, so nothing wraps */
    return q + (rem >= c - rem ? 1u : 0u);
}

/* 1 when num / den >= a / b (den and b positive), compared exactly. */
static int fraction_at_least(uint64_t num, uint64_t den, uint64_t a, uint64_t b)
{
    frc_i_u128_t left, right;
    frc_i_mul64(num, b, &left);
    frc_i_mul64(den, a, &right);
    return frc_i_cmp128(&left, &right) >= 0;
}

/* ------------------------------------------------------------------------- */
/* S15.4  Measuring a span                                                    */
/* ------------------------------------------------------------------------- */

/*
 * An instant on the Blink axis: (quarters + rem / den) quarter Blinks since
 * position 0. Exact. `den` is the length, in microseconds, of the Saros
 * interval that holds THE MICROSECOND THAT ENDS AT THE INSTANT: that is the
 * interval whose microsecond is the instant's grain (see snap_to_grid).
 * Inside an interval 0 <= rem < den. An instant exactly on eclipse k >= 1
 * (the closing instant included) is written from the interval that ends
 * there, as its last quarter Blink complete: quarters = k * P(12) - 1 and
 * rem == den.
 */
typedef struct blink_point {
    uint64_t quarters;
    uint64_t rem;
    uint64_t den;
} blink_point_t;

static frc_status_t blink_point_at(const frc_catalogue_t *cat, frc_time_t t, blink_point_t *out)
{
    frc_i_fraction_t f;
    frc_i_u128_t extent;
    uint64_t prefix;
    uint64_t on_eclipse = 0; /* k * P(12) when the instant is eclipse k >= 1 */
    frc_status_t status = frc_i_fraction_at(cat, t, FRC_I_BLINK_LEVEL, &f);

    if (status == FRC_OK) {
        status = frc_i_extent(cat, FRC_I_BLINK_LEVEL, &prefix, &extent);
    }
    if (status != FRC_OK) {
        return status;
    }
    /* (count - 1) * P(12) < 2^16 * 2^33: the high halves are always empty. */
    out->quarters = f.total.lo;
    out->rem = f.rem;
    out->den = f.den;
    if (f.closing) {
        /* The closing instant is exactly the end of the catalogue, position
         * count - 1 (the clock reads it one microsecond early; a span to it
         * must not lose that). f.den is already the last interval. */
        on_eclipse = extent.lo;
    } else if (f.rem == 0 && f.total.lo != 0 && f.total.lo % prefix == 0) {
        /* eclipse k = total / P: the interval that ends here is k - 1 */
        on_eclipse = f.total.lo;
        out->den = frc_i_cat_interval(cat, (uint16_t)(f.total.lo / prefix - 1u));
    }
    if (on_eclipse != 0) {
        out->quarters = on_eclipse - 1u;
        out->rem = out->den;
    }
    return FRC_OK;
}

/*
 * floor(n * c / m) for n < m < 2^126 and a 32-bit c; the result is below c.
 * Horner over the bits of c, most significant first. The running value r < m
 * is doubled, the bit of c adds n, and every time r reaches m it is reduced:
 * the reductions, weighted like the bits, are the quotient. 2r and r + n stay
 * below 2m < 2^127, so nothing wraps. (For c = 2^k this is plain binary long
 * division.)
 */
static uint32_t scale_floor(const frc_i_u128_t *n, const frc_i_u128_t *m, uint32_t c)
{
    frc_i_u128_t r;
    uint32_t quotient = 0;
    uint8_t bit;

    r.hi = 0;
    r.lo = 0;
    for (bit = 32; bit > 0; bit--) {
        frc_i_add128(&r, &r);
        quotient <<= 1;
        if (frc_i_cmp128(&r, m) >= 0) {
            frc_i_sub128(&r, m);
            quotient += 1u;
        }
        if (((c >> (bit - 1u)) & 1u) != 0) {
            frc_i_add128(&r, n);
            if (frc_i_cmp128(&r, m) >= 0) {
                frc_i_sub128(&r, m);
                quotient += 1u;
            }
        }
    }
    return quotient;
}

/*
 * THE TIME GRAIN (see the top of this file). The span is quarters + n/m
 * quarter Blinks with m = from_den * to_den. Each instant stands for a
 * boundary in the microsecond that ends at it, so the true span lies within
 *
 *     one microsecond at `to`     P(12) / to_den   quarter Blinks below, and
 *     one microsecond at `from`   P(12) / from_den quarter Blinks above
 *
 * the measured one (from_den and to_den are the intervals those two
 * microseconds lie in: blink_point_t). When a whole number of quarter Blinks
 * is strictly inside that window, the span is that number: n becomes 0, and
 * `quarters` one more when the whole number is the next one up. Over the
 * common denominator m the two tests are n < P(12) * from_den and
 * m - n < P(12) * to_den; the products are below 2^33 * 2^63.
 * A span of exactly one microsecond is not inside the window and stays what
 * it is, and so does every span between two different instants: none is
 * brought to nothing. (On a table whose intervals are shorter than P(12)
 * microseconds, two hours, a microsecond is longer than a quarter Blink and
 * every span is a whole number of them: such a table has no finer grain.)
 */
static void snap_to_grid(uint64_t *quarters, frc_i_u128_t *n, const frc_i_u128_t *m,
                         uint64_t from_den, uint64_t to_den)
{
    uint64_t prefix = frc_prefix(FRC_I_BLINK_LEVEL);
    frc_i_u128_t grain, gap;

    frc_i_mul64(prefix, from_den, &grain); /* one microsecond at `to`, over m */
    if (frc_i_cmp128(n, &grain) >= 0) {
        gap.hi = m->hi; /* m - n: how far the next whole quarter Blink is */
        gap.lo = m->lo;
        frc_i_sub128(&gap, n);
        frc_i_mul64(prefix, to_den, &grain); /* one microsecond at `from` */
        if (frc_i_cmp128(&gap, &grain) >= 0) {
            return; /* not within the grain of the grid: the span stands */
        }
        *quarters += 1u;
    }
    n->hi = 0;
    n->lo = 0;
}

/*
 * Every rounding frc_format_span makes is decided at a multiple of 1/20000
 * Blink: the tie of its finest decimal (0.00005), and with it every coarser
 * tie (0.05, 0.005, 0.0005) and threshold (0.1, 0.01, 0.001).
 */
#define ROUNDING_MARKS 20000u

/*
 * The fraction of a Blink (whole + n/m) / 4, for whole = 0..3 quarter Blinks
 * and n < m < 2^126, as a frc_frac_t. Shared (section 9 of frc_internal.h):
 * the light span of frc_pure.c cuts its fraction here too.
 *
 * It is the floor to 2^-32 Blink, with one exception. A floor may step back
 * over a rounding mark: exactly 1.05 Blinks would be stored a hair under
 * 1.05 and read "1ν" instead of "1.1ν". So when a mark lies above the floor
 * and at or below the exact value, the fraction is the next 2^-32 up, which
 * is on the exact value's side of that mark (and of every other one: marks
 * are 215,000 units of 2^-32 apart). The text rounded from the frc_frac_t is
 * then the text of the exact fraction.
 */
frc_frac_t frc_i_blink_fraction(uint8_t whole, const frc_i_u128_t *n, const frc_i_u128_t *m)
{
    /* (whole + n/m) * 2^30: `whole` fills the top two bits, the floor of the
     * rest the other thirty, as in frc_i_fraction_phase */
    uint64_t frac = ((uint64_t)whole << 30) | scale_floor(n, m, (uint32_t)1 << 30);
    /* floor(fraction * 20000): the last mark at or below the exact value */
    uint64_t mark = (uint64_t)whole * (ROUNDING_MARKS / 4u) + scale_floor(n, m, ROUNDING_MARKS / 4u);

    /* mark / 20000 > frac / 2^32: the floor fell below that mark. Both
     * products are below 2^47. A mark is at most 19999 / 20000, so the step
     * up cannot reach 1.0. */
    if (mark * FRAC_ONE > frac * ROUNDING_MARKS) {
        frac += 1u;
    }
    return (frc_frac_t)frac;
}

frc_status_t frc_span_between(const frc_catalogue_t *cat, frc_time_t t1, frc_time_t t2, frc_span_t *out)
{
    blink_point_t from, to; /* the earlier and the later instant */
    frc_i_u128_t n, m, down;
    frc_status_t status;
    uint64_t quarters;
    int backward = (t2 < t1);

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = blink_point_at(cat, backward ? t2 : t1, &from);
    if (status == FRC_OK) {
        status = blink_point_at(cat, backward ? t1 : t2, &to);
    }
    if (status != FRC_OK) {
        return status;
    }
    /* The two fractions are brought to the common denominator den * den,
     * which must leave a bit of headroom in 128 bits. A real Saros interval
     * is 2^49 microseconds; 2^63 would be 292,000 years. */
    if (((from.den | to.den) >> 63) != 0) {
        return FRC_ERR_OVERFLOW;
    }

    /* span = (to.quarters - from.quarters) + to.rem/to.den - from.rem/from.den
     *      = quarters + n / m           quarter Blinks, with 0 <= n < m.
     * `to` is not before `from`, so when its fraction is the smaller one the
     * whole quarters differ by at least 1 and can lend one. */
    quarters = to.quarters - from.quarters;
    frc_i_mul64(from.den, to.den, &m);
    frc_i_mul64(to.rem, from.den, &n);
    frc_i_mul64(from.rem, to.den, &down);
    if (frc_i_cmp128(&n, &down) >= 0) {
        frc_i_sub128(&n, &down);
        if (frc_i_cmp128(&n, &m) == 0) {
            /* a whole quarter: `to` is an eclipse (rem == den) and `from`
             * has no fraction. n <= m always, as to.rem <= to.den. */
            quarters += 1u;
            n.hi = 0;
            n.lo = 0;
        }
    } else {
        quarters -= 1u;
        frc_i_sub128(&down, &n); /* how much the fraction of `to` is short */
        n.hi = m.hi;
        n.lo = m.lo;
        frc_i_sub128(&n, &down);
    }
    snap_to_grid(&quarters, &n, &m, from.den, to.den);

    /* (quarters + n/m) / 4 Blinks: the whole Blinks, and the rest of one */
    out->blinks = quarters >> 2;
    out->frac = frc_i_blink_fraction((uint8_t)(quarters & 3u), &n, &m);
    /* a span too short for frc_span_t (under 2^-32 Blink) has no direction */
    out->negative = (backward && (out->blinks != 0 || out->frac != 0)) ? 1 : 0;
    return FRC_OK;
}

/*
 * cycles of `level` in one Blink for level > 12: the product of the factors
 * from level 12 down, or 0 when it does not fit 64 bits (from level 36 on).
 */
static uint64_t cycles_per_blink(uint8_t level)
{
    uint64_t ratio = 1;
    uint8_t l;
    for (l = FRC_I_BLINK_LEVEL; l < level; l++) {
        if (!frc_i_mul64_fits(ratio, frc_factor(l), &ratio)) {
            return 0;
        }
    }
    return ratio;
}

frc_status_t frc_span_of_level(uint8_t level, frc_span_t *out)
{
    if (out == NULL || level >= FRC_LEVEL_COUNT) {
        return FRC_ERR_ARG;
    }
    out->negative = 0;
    out->blinks = frc_i_level_blinks(level); /* 0 below the Blink */
    out->frac = 0;
    if (level > FRC_I_BLINK_LEVEL) {
        /* 1 / ratio of a Blink is 4 / ratio quarter Blinks, and ratio >= 7.
         * It is cut like a measured span. From level 24 on the ratio passes
         * 2^32 and the fraction is below one unit of frc_frac_t: 0 (as it is
         * when the ratio no longer fits 64 bits at all). */
        uint64_t ratio = cycles_per_blink(level);
        if (ratio != 0) {
            frc_i_u128_t n, m;
            n.hi = 0;
            n.lo = 4;
            m.hi = 0;
            m.lo = ratio;
            out->frac = frc_i_blink_fraction(0, &n, &m);
        }
    }
    return FRC_OK;
}

/* ------------------------------------------------------------------------- */
/* S15.4  Counting a span over the ladder                                     */
/* ------------------------------------------------------------------------- */

uint8_t frc_span_parts(const frc_span_t *span, uint8_t max_parts, int keep_zeros, frc_span_part_t *out)
{
    uint64_t rest;
    uint8_t unit;
    uint8_t n = 0;

    if (span == NULL || out == NULL) {
        return 0;
    }
    rest = span->blinks;
    /* the largest unit that fits; a span shorter than a Blink is led by the
     * Blink, with a count of 0 */
    for (unit = FRC_UNIT_ALPHA; unit < FRC_UNIT_BLINK; unit++) {
        if (frc_unit_blinks((frc_unit_t)unit) <= rest) {
            break;
        }
    }
    for (; unit < FRC_UNIT_COUNT && n < max_parts; unit++) {
        uint64_t length = frc_unit_blinks((frc_unit_t)unit);
        uint64_t count = (length != 0) ? rest / length : 0;
        rest -= count * length;
        /* zero counts are skipped ("3η 5λ", never "3η 0ι 5λ"), except the
         * leading one of a sub-Blink span */
        if (count != 0 || keep_zeros || n == 0) {
            out[n].unit = unit;
            out[n].count = count;
            n++;
        }
    }
    return n;
}

/*
 * pure.blinkCount: a count of Blinks below 10 with its decimal part, with ONE
 * cut-off below 1/1000 Blink (the reference also writes "0" below 1/10000):
 *   exactly zero      "0"                      COUNT_ZERO
 *   under 0.001       "<0.001"                 COUNT_TINY: any non-zero span,
 *                                              never "0" for one
 *   under 1           two significant digits   "0.14", "0.0032"
 *   from 1            one decimal              "2.6", "4"
 * As a number: COUNT_VALUE, the text is value / 10^decimals. The writer and
 * frc_span_next_change both read it from here, so they cannot disagree.
 */
#define COUNT_ZERO  0
#define COUNT_TINY  1
#define COUNT_VALUE 2

static uint8_t blink_count(uint64_t blinks, frc_frac_t frac, uint64_t *value, uint8_t *decimals)
{
    *value = 0;
    *decimals = 0;
    if (blinks != 0) {
        /* blinks < 10 and the rounded tenths of the fraction are <= 10 */
        *value = blinks * 10u + muldiv_round(frac, 10, FRAC_ONE);
        *decimals = 1;
        return COUNT_VALUE;
    }
    if (frac == 0) {
        return COUNT_ZERO;
    }
    if ((uint64_t)frac * 1000u < FRAC_ONE) {
        return COUNT_TINY;
    }
    /* the decade of the value decides how many decimals two significant
     * digits need: [0.1, 1) two, [0.01, 0.1) three, [0.001, 0.01) four */
    *decimals = ((uint64_t)frac * 10u >= FRAC_ONE) ? 2 : ((uint64_t)frac * 100u >= FRAC_ONE) ? 3 : 4;
    /* frac * 10^4 < 2^46; a value that rounds up to the next decade
     * ("0.0996" -> 100 at three decimals) prints right: "0.1" */
    *value = muldiv_round(frac, frc_i_pow_u64(10, *decimals), FRAC_ONE);
    return COUNT_VALUE;
}

static void put_blink_count(frc_i_text_t *w, uint64_t blinks, frc_frac_t frac)
{
    uint64_t value;
    uint8_t decimals;
    uint8_t kind = blink_count(blinks, frac, &value, &decimals);

    if (kind == COUNT_ZERO) {
        frc_i_text_char(w, '0');
    } else if (kind == COUNT_TINY) {
        frc_i_text_char(w, '<');
        frc_i_text_decimal(w, 1, 3);
    } else {
        frc_i_text_decimal(w, value, decimals);
    }
}

/* 1 when the text of a span shows no digit of it: "0ν" and "<0.001ν". Such a
 * text has no sign, in frc_format_span and frc_format_span_signed alike. (A
 * span without whole Blinks is led by the Blink, so it is written as a
 * decimal.) */
static int shows_no_digit(const frc_span_t *span)
{
    return span->blinks == 0 && (uint64_t)span->frac * 1000u < FRAC_ONE;
}

/* The text of frc_format_span, appended to a text in progress (section 9 of
 * frc_internal.h: frc_pure.c writes a light span with it). */
void frc_i_text_span(frc_i_text_t *w, const frc_span_t *span, uint8_t max_parts)
{
    frc_span_part_t parts[FRC_UNIT_COUNT];
    uint8_t n, i;
    int decimal;

    if (span == NULL) {
        /* No caller passes one. The test is what tells the compiler that
         * frc_span_parts below always fills parts[0]: without it gcc -O3
         * reports parts[0].unit as maybe uninitialised (xtensa, riscv32). */
        return;
    }
    if (max_parts == 0) {
        max_parts = 2; /* S15.4: two non-zero parts are the default precision */
    } else if (max_parts > FRC_UNIT_COUNT) {
        max_parts = FRC_UNIT_COUNT;
    }
    n = frc_span_parts(span, max_parts, 0, parts);

    /* The ladder ends at the Blink: a span led by it and shorter than 10
     * Blinks keeps a decimal instead ("2.6ν", "0.14ν"). */
    decimal = (parts[0].unit == FRC_UNIT_BLINK && span->blinks < 10);
    /* "0ν" and "<0.001ν" have no sign (never "−0ν", never "−<0.001ν") */
    if (span->negative && !shows_no_digit(span)) {
        frc_i_text_cp(w, FRC_I_CP_MINUS);
    }
    if (decimal) {
        put_blink_count(w, span->blinks, span->frac);
        frc_i_text_unit_symbol(w, FRC_UNIT_BLINK);
    } else {
        for (i = 0; i < n; i++) {
            if (i != 0) {
                frc_i_text_char(w, ' ');
            }
            frc_i_text_u64(w, parts[i].count);
            frc_i_text_unit_symbol(w, parts[i].unit);
        }
    }
}

int frc_format_span(const frc_span_t *span, uint8_t max_parts, char *buf, size_t size)
{
    frc_i_text_t w;

    if (span == NULL) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_span(&w, span, max_parts);
    return frc_i_text_finish(&w);
}

int frc_format_span_signed(const frc_span_t *span, uint8_t max_parts, char *buf, size_t size)
{
    frc_i_text_t w;

    if (span == NULL) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    /* the minus is frc_i_text_span's own; the plus follows the same rule */
    if (!span->negative && !shows_no_digit(span)) {
        frc_i_text_char(&w, '+');
    }
    frc_i_text_span(&w, span, max_parts);
    return frc_i_text_finish(&w);
}

int frc_format_span_in_unit(const frc_span_t *span, frc_unit_t unit, char *buf, size_t size)
{
    frc_i_text_t w;
    uint64_t length = frc_unit_blinks(unit); /* 0 when `unit` is not a unit */
    uint64_t count;

    if (span == NULL || length == 0) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    /* whole units, cut toward zero: the fraction of a Blink never counts,
     * and a part of the unit is dropped, whatever the sign */
    count = span->blinks / length;
    frc_i_text_init(&w, buf, size);
    if (span->negative && count != 0) {
        frc_i_text_cp(&w, FRC_I_CP_MINUS); /* never "−0λ" */
    }
    frc_i_text_u64_grouped(&w, count);
    frc_i_text_unit_symbol(&w, (uint8_t)unit);
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* When a countdown's text changes                                            */
/* ------------------------------------------------------------------------- */

/*
 * THE MODEL. While `now` runs towards `boundary`, the span between them
 * shrinks, and its text is a step function of it: every text belongs to one
 * interval of spans [low, high). The text changes when the span falls below
 * `low` of the present text. After an `origin` (an elapsed time, or a signed
 * span past its zero) the span grows, and the text changes when it reaches
 * `high`; the same machinery serves both (frc_span_next_change_since). Every such edge is a whole number of 1/20000
 * Blink, a MARK (ROUNDING_MARKS above): a whole Blink count of the parts,
 * or a rounding tie or threshold of the decimal. A span is cut so that it is
 * on the exact value's side of every mark (frc_i_blink_fraction), and the
 * grid snap of frc_span_between only ever moves a span to a whole quarter
 * Blink nearer than one microsecond: so the measured span, and with it the
 * text, never goes back as `now` goes on (monotone), and the instant where
 * the exact span crosses `low` (or `high`) is right to within a microsecond or two.
 * That instant is computed, then the formatter's own class is asked around
 * it, and a bisection over the instants settles the first one that differs.
 * If the computed instant were ever off, the bisection over the whole
 * interval still finds the change (a monotone predicate): the answer is
 * always the formatter's, the arithmetic only makes it fast.
 */

/* What the text of a positive span says, as a number. Spans are kept as
 * whole Blinks + marks (0..19999 of 1/20000 Blink). */
typedef struct span_class {
    uint8_t  kind;         /* COUNT_ZERO, COUNT_TINY or COUNT_VALUE             */
    uint32_t shown_marks;  /* the value the text shows, Blinks + marks          */
    uint64_t shown_blinks;
    uint32_t low_marks;    /* the smallest span with the same text              */
    uint64_t low_blinks;
    uint32_t high_marks;   /* the smallest LARGER span with another text        */
    uint64_t high_blinks;
} span_class_t;

static void class_set(uint64_t *blinks, uint32_t *marks, uint64_t whole, uint64_t in_marks)
{
    *blinks = whole + in_marks / ROUNDING_MARKS;
    *marks = (uint32_t)(in_marks % ROUNDING_MARKS); /* < 20000 */
}

/*
 * The class of a span's text. Two spans have the same text exactly when
 * their classes have the same kind and shown value (the sign aside: the
 * spans compared are all on one side of zero). Every text is written for one
 * interval of spans [low, high).
 *   decimal (below 10 Blinks)  value / 10^d Blinks; it is written for every
 *                              span from half a last digit below it, except
 *                              where the decade begins: "1" ("1.0") from
 *                              0.995, "0.1" from 0.0995, "0.01" from 0.00995,
 *                              "0.001" from exactly 0.001 ("<0.001" below).
 *                              It ends half a last digit above it, the last
 *                              digit being that of the decade of the value
 *                              SHOWN ("0.1" runs to 0.105, not to 0.1005).
 *   parts (from 10 Blinks)     the parts shown are the span cut to the finest
 *                              unit shown (`max_parts` non-zero parts, or
 *                              down to the Blink); "10ν" also covers the
 *                              decimals from 9.95. The text runs to the next
 *                              whole finest unit: every span in between has
 *                              the same coarser counts, so the same finest
 *                              unit and the same cut.
 *   "<0.001ν" runs to 0.001; "0ν" is the zero span alone.
 */
static void span_class_of(const frc_span_t *span, uint8_t max_parts, span_class_t *out)
{
    uint64_t value, rest, length, shown;
    uint8_t decimals, unit, finest, n;

    out->shown_blinks = 0;
    out->shown_marks = 0;
    out->low_blinks = 0;
    out->low_marks = 0;
    out->high_blinks = 0;
    out->high_marks = 0;
    if (span->blinks < 10) {
        uint64_t step, low, mark, high;
        out->kind = blink_count(span->blinks, span->frac, &value, &decimals);
        if (out->kind == COUNT_TINY) {
            out->high_marks = ROUNDING_MARKS / 1000u; /* "0.001ν" from 0.001 */
        }
        if (out->kind != COUNT_VALUE) {
            return;
        }
        /* one last digit in marks: 2000, 200, 20, 2 for d = 1..4 */
        step = 2u * frc_i_pow_u64(10, (uint8_t)(4u - decimals));
        /* value * step < 10 * 20000 + 1 */
        mark = value * step; /* the value shown, in marks */
        if (value == 10u) {
            low = (decimals == 4) ? 20u : mark - step / 20u; /* the decade's start */
        } else {
            low = mark - step / 2u;
        }
        /* The end: half a last digit of the decade of the value shown, which
         * is one decade up when the value rounded up into it ("0.1" from
         * 0.0996, d = 3). From 10 Blinks it is the parts' "10ν", to 11. */
        if (mark >= 10u * ROUNDING_MARKS) {
            high = 11u * ROUNDING_MARKS;
        } else {
            /* one last digit of the decade [10 last, 100 last): mark, a
             * multiple of it, has two significant digits there */
            uint64_t last = 2u; /* 0.0001 Blink in marks */
            while (mark >= 100u * last) {
                last *= 10u;
            }
            high = mark + last / 2u;
        }
        class_set(&out->shown_blinks, &out->shown_marks, 0, mark);
        class_set(&out->low_blinks, &out->low_marks, 0, low);
        class_set(&out->high_blinks, &out->high_marks, 0, high);
        return;
    }
    if (max_parts == 0) {
        max_parts = 2;
    } else if (max_parts > FRC_UNIT_COUNT) {
        max_parts = FRC_UNIT_COUNT;
    }
    /* frc_span_parts without its array: the finest unit of the text */
    rest = span->blinks;
    finest = FRC_UNIT_BLINK;
    n = 0;
    for (unit = FRC_UNIT_ALPHA; unit < FRC_UNIT_COUNT; unit++) {
        length = frc_unit_blinks((frc_unit_t)unit);
        if (rest / length != 0) {
            rest %= length;
            n++;
            if (n == max_parts) {
                finest = unit;
                break;
            }
        }
    }
    length = frc_unit_blinks((frc_unit_t)finest);
    shown = span->blinks / length * length;
    out->kind = COUNT_VALUE;
    out->shown_blinks = shown;
    out->low_blinks = shown;
    out->high_blinks = shown + length; /* a span is below 2^64 - 2^33: fits */
    if (shown == 10u) {
        /* "10ν" is also the decimal 9.95 .. 9.999 */
        out->low_blinks = 9;
        out->low_marks = ROUNDING_MARKS - ROUNDING_MARKS / 20u;
    }
}

static int same_class(const span_class_t *a, const span_class_t *b)
{
    return a->kind == b->kind && a->shown_blinks == b->shown_blinks && a->shown_marks == b->shown_marks;
}

/* 1 when the countdown at `now` has the text of class `was`. */
static int same_text_at(const frc_catalogue_t *cat, frc_time_t now, frc_time_t boundary, uint8_t max_parts,
                        const span_class_t *was)
{
    frc_span_t span;
    span_class_t here;

    if (frc_span_between(cat, now, boundary, &span) != FRC_OK) {
        return 0;
    }
    span_class_of(&span, max_parts, &here);
    return same_class(&here, was);
}

/*
 * The first microsecond at or after (about: see below) the instant where the
 * span from `anchor` is exactly the edge `blinks` + `marks` / 20000, BEFORE
 * the anchor (a countdown to it: position * P(12) = Q - 4 edge quarter
 * Blinks) or AFTER it (a span since it: Q + 4 edge), Q being the anchor's
 * (blink_point_t). The fraction rem/den -+ r/5000 is taken to the nearest
 * 1/den below (far under a microsecond: one quarter Blink is 77 ms on Saros
 * 141). FRC_TIME_NONE when it cannot be placed; the caller then searches the
 * whole interval. It is only a guess: the caller checks it with the
 * formatter's class on both sides.
 */
static frc_time_t instant_of_edge(const frc_catalogue_t *cat, frc_time_t anchor, uint64_t blinks, uint32_t marks,
                                  int after)
{
    blink_point_t end;
    frc_i_u128_t a, b, total;
    uint64_t marks_quarters = marks / 5000u; /* 0..3 */
    uint64_t r = marks % 5000u;
    uint64_t whole, edge, f;

    if (blink_point_at(cat, anchor, &end) != FRC_OK || blinks > (UINT64_MAX >> 3)) {
        return FRC_TIME_NONE;
    }
    edge = 4u * blinks + marks_quarters; /* < 2^63 */
    frc_i_mul64(end.rem, 5000u, &a); /* rem <= den < 2^63: below 2^76, so a.hi < 5000 */
    frc_i_mul64(r, end.den, &b);     /* below 5000 den */
    if (after) {
        /* Q + edge: whole quarters, and rem/den + r/5000 of one more,
         * which may carry (rem == den on an eclipse) */
        if (end.quarters > UINT64_MAX - edge - 2u) {
            return FRC_TIME_NONE;
        }
        whole = end.quarters + edge;
        frc_i_add128(&a, &b);
        frc_i_mul64(end.den, 5000u, &b);
        while (frc_i_cmp128(&a, &b) >= 0) { /* at most twice: a < 2 * 5000 den */
            frc_i_sub128(&a, &b);
            whole += 1u;
        }
        f = frc_i_div128(&a, 5000u, NULL); /* a < 5000 den: f < den */
    } else {
        if (end.quarters < edge) {
            return FRC_TIME_NONE;
        }
        whole = end.quarters - edge;
        if (frc_i_cmp128(&a, &b) >= 0) {
            frc_i_sub128(&a, &b);
            f = frc_i_div128(&a, 5000u, NULL); /* <= rem <= den */
        } else {
            if (whole == 0) {
                return FRC_TIME_NONE;
            }
            whole -= 1u; /* lend a quarter Blink */
            frc_i_sub128(&b, &a);
            f = end.den - frc_i_div128_ceil(&b, 5000u); /* b < 5000 den: in 0 .. den - 1 */
        }
    }
    total.hi = 0;
    total.lo = whole;
    return frc_i_time_after_total(cat, FRC_I_BLINK_LEVEL, &total, f, end.den);
}

/*
 * The text at `t` of the span between `t` and `anchor` (its class) and where
 * the arithmetic places its next change: the low edge of the class when
 * t < anchor (the span shrinks), its high edge when t >= anchor (it grows).
 * 0 when frc_span_between fails at `t`.
 */
static int change_guess(const frc_catalogue_t *cat, frc_time_t t, frc_time_t anchor, uint8_t max_parts,
                        span_class_t *was, frc_time_t *guess)
{
    frc_span_t span;

    if (frc_span_between(cat, t, anchor, &span) != FRC_OK) {
        return 0;
    }
    span_class_of(&span, max_parts, was);
    if (t < anchor) {
        /* "<0.001ν" stays until the span is nothing: "0ν" at the anchor */
        *guess = (was->kind == COUNT_VALUE) ? instant_of_edge(cat, anchor, was->low_blinks, was->low_marks, 0)
                                            : anchor;
    } else {
        /* "0ν" is the anchor alone: one microsecond later it is "<0.001ν"
         * (no span between two different instants is nothing) */
        *guess = (was->kind == COUNT_ZERO) ? t + 1
                                           : instant_of_edge(cat, anchor, was->high_blinks, was->high_marks, 1);
    }
    return 1;
}

frc_time_t frc_i_span_change_guess(const frc_catalogue_t *cat, frc_time_t t, frc_time_t anchor, uint8_t max_parts)
{
    span_class_t was;
    frc_time_t guess;

    return change_guess(cat, t, anchor, max_parts, &was, &guess) ? guess : FRC_TIME_NONE;
}

/*
 * The first instant in (lo, hi] whose text is not `was`, the text at lo
 * being `was` and the text at hi not (a monotone predicate: the span only
 * shrinks or only grows on the way). The guess, when it lies in (lo, hi],
 * narrows the interval to a few microseconds first.
 */
static frc_time_t search_change(const frc_catalogue_t *cat, frc_time_t anchor, uint8_t max_parts,
                                const span_class_t *was, frc_time_t lo, frc_time_t hi, frc_time_t guess)
{
    if (guess != FRC_TIME_NONE && guess > lo && guess <= hi) {
        if (guess - 4 > lo && same_text_at(cat, guess - 4, anchor, max_parts, was)) {
            lo = guess - 4;
        }
        if (guess + 3 < hi && !same_text_at(cat, guess + 3, anchor, max_parts, was)) {
            hi = guess + 3;
        }
    }
    while (hi - lo > 1) {
        /* lo < mid < hi; the difference of two catalogue instants fits int64 */
        frc_time_t mid = lo + (hi - lo) / 2;
        if (same_text_at(cat, mid, anchor, max_parts, was)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return hi;
}

frc_time_t frc_span_next_change(const frc_catalogue_t *cat, frc_time_t t, frc_time_t boundary, uint8_t max_parts)
{
    span_class_t was;
    frc_time_t guess;

    if (t >= boundary || !change_guess(cat, t, boundary, max_parts, &was, &guess)) {
        return FRC_TIME_NONE; /* also an unusable catalogue, an instant outside it, or an overflow */
    }
    if (was.kind != COUNT_VALUE) {
        return boundary; /* "<0.001ν" stays until the span is nothing: "0ν" */
    }
    /* the first change lies in (t, boundary]: same text at t, another at the
     * boundary, whose text is "0ν", which no COUNT_VALUE class is */
    return search_change(cat, boundary, max_parts, &was, t, boundary, guess);
}

frc_time_t frc_span_next_change_since(const frc_catalogue_t *cat, frc_time_t origin, frc_time_t t, uint8_t max_parts)
{
    span_class_t was;
    frc_time_t guess, last, hi;

    if (t < origin || !change_guess(cat, t, origin, max_parts, &was, &guess)) {
        return FRC_TIME_NONE; /* also an unusable catalogue, an instant outside it, or an overflow */
    }
    last = frc_catalogue_last(cat); /* t is inside: the catalogue is usable */
    if (t == last) {
        return FRC_TIME_NONE; /* nothing comes later */
    }
    if (was.kind == COUNT_ZERO) {
        return t + 1; /* from "0ν" at the origin to "<0.001ν" */
    }
    /* the first change lies in (t, hi]: another text at hi. A guess a few
     * microseconds on gives hi; else it is the closing instant, if the text
     * is another there at all */
    hi = last;
    if (guess != FRC_TIME_NONE && guess > t && guess + 3 < last &&
        !same_text_at(cat, guess + 3, origin, max_parts, &was)) {
        hi = guess + 3;
    } else if (same_text_at(cat, last, origin, max_parts, &was)) {
        return FRC_TIME_NONE; /* the text lasts to the catalogue's end */
    }
    return search_change(cat, origin, max_parts, &was, t, hi, guess);
}

/* ------------------------------------------------------------------------- */
/* S15.5  Time-lapse rates                                                    */
/* ------------------------------------------------------------------------- */

/* The presets of pure.RATES after "real time": a whole count of a unit per
 * Blink of real time. Two parallel flash tables, indexed by preset. */
static const uint8_t RATE_UNITS[FRC_RATE_COUNT] FRC_PROGMEM = {
    FRC_UNIT_BLINK,  FRC_UNIT_BLINK,  FRC_UNIT_FLAVON,  FRC_UNIT_FLAVON, FRC_UNIT_PREON, FRC_UNIT_PREON,
    FRC_UNIT_HELION, FRC_UNIT_HELION, FRC_UNIT_NUCLIDE, FRC_UNIT_DELTA,  FRC_UNIT_GAMMA};
static const uint8_t RATE_COUNTS[FRC_RATE_COUNT] FRC_PROGMEM = {0, 5, 1, 13, 1, 4, 1, 11, 1, 1, 1};

static const char REAL_TIME[] FRC_PROGMEM = "real time";
static const char PER_BLINK[] FRC_PROGMEM = " per Blink";

frc_status_t frc_rate_preset(uint8_t i, frc_rate_t *out)
{
    if (out == NULL || i >= FRC_RATE_COUNT) {
        return FRC_ERR_ARG;
    }
    out->unit = FRC_PGM_U8(&RATE_UNITS[i]);
    out->count = FRC_PGM_U8(&RATE_COUNTS[i]);
    /* The hierarchy is a product of whole factors, so the speed-up is a
     * whole number: the count times the unit's length in Blinks. Real time
     * (count 0) is one Blink per Blink. */
    out->factor = (out->count == 0) ? 1u : (uint64_t)out->count * frc_unit_blinks((frc_unit_t)out->unit);
    return FRC_OK;
}

int frc_format_rate(const frc_rate_t *rate, char *buf, size_t size)
{
    frc_i_text_t w;

    if (rate == NULL || rate->unit >= FRC_UNIT_COUNT) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    if (rate->count == 0) {
        frc_i_text_pgm(&w, REAL_TIME);
    } else {
        if (rate->count != 1) {
            frc_i_text_u64(&w, rate->count); /* "η per Blink", but "4ι per Blink" */
        }
        frc_i_text_unit_symbol(&w, rate->unit);
        frc_i_text_pgm(&w, PER_BLINK);
    }
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* S15.5  Cycles per Blink (pB)                                               */
/* ------------------------------------------------------------------------- */

frc_status_t frc_level_per_blink(uint8_t level, uint64_t *num, uint64_t *den)
{
    uint64_t ratio;

    if (num == NULL || den == NULL || level >= FRC_LEVEL_COUNT) {
        return FRC_ERR_ARG;
    }
    /* P(level) / P(12) is a whole number or one over a whole number, because
     * every prefix divides the next: it is in lowest terms as it stands. */
    if (level <= FRC_I_BLINK_LEVEL) {
        *num = 1;
        *den = frc_i_level_blinks(level);
        return FRC_OK;
    }
    ratio = cycles_per_blink(level);
    if (ratio == 0) {
        return FRC_ERR_OVERFLOW; /* from level 36 on the rate needs more than 64 bits */
    }
    *num = ratio;
    *den = 1;
    return FRC_OK;
}

static const char ONE_PER[] FRC_PROGMEM = "1 per ";
static const char BLINKS[] FRC_PROGMEM = " Blinks";
static const char PB[] FRC_PROGMEM = " pB";

/*
 * pure.pbText on the exact rational pb = num / den, with design ruling 3 first:
 *   nothing (0, or den 0)   "—"
 *   pb < 1, num divides den "1 per N Blinks", N = den / num exactly (ruling 3:
 *                           a rung coarser than the Blink, the Flavon's 1/25
 *                           and mu's 1/5 included; the reference writes those
 *                           two "0.04 pB" and "0.2 pB")
 *   pb < 1/25               "1 per N Blinks", N = den / num rounded
 *   pb >= 999.5             a whole number in digit groups   "2 772 pB"
 *   pb >= 99.95             one decimal                      "123.5 pB"
 *   else                    three significant digits         "34.2 pB", "0.0456 pB"
 * Every threshold is compared exactly (the reference's 1e-9 fudge exists to
 * keep exactly 1/25 on the pB side; here it is there without one).
 */
int frc_format_per_blink(uint64_t num, uint64_t den, char *buf, size_t size)
{
    frc_i_text_t w;
    uint8_t decimals;

    frc_i_text_init(&w, buf, size);
    if (num == 0 || den == 0) {
        frc_i_text_cp(&w, FRC_I_CP_EM_DASH);
    } else if (num < den && den % num == 0) {
        /* a whole reciprocal: the rate of a rung coarser than the Blink */
        frc_i_text_pgm(&w, ONE_PER);
        frc_i_text_u64_grouped(&w, den / num);
        frc_i_text_pgm(&w, BLINKS);
    } else if (!fraction_at_least(num, den, 1, 25)) {
        frc_i_text_pgm(&w, ONE_PER);
        /* den / num rounded: at most den, and den + 1 cannot be reached
         * (num = 1 leaves no remainder to round up) */
        frc_i_text_u64_grouped(&w, muldiv_round(den, 1, num));
        frc_i_text_pgm(&w, BLINKS);
    } else {
        if (fraction_at_least(num, den, 1999, 2)) {
            /* num / den rounded: fits for the same reason */
            frc_i_text_u64_grouped(&w, muldiv_round(num, 1, den));
        } else if (fraction_at_least(num, den, 1999, 20)) {
            frc_i_text_decimal(&w, muldiv_round(num, 10, den), 1); /* < 9995 */
        } else {
            /* the decade decides the decimals of three significant digits;
             * the scaled value is below 1000 (it may round up to 1000) */
            decimals = fraction_at_least(num, den, 10, 1) ? 1 : fraction_at_least(num, den, 1, 1) ? 2
                     : fraction_at_least(num, den, 1, 10) ? 3 : 4;
            frc_i_text_decimal(&w, muldiv_round(num, frc_i_pow_u64(10, decimals), den), decimals);
        }
        frc_i_text_pgm(&w, PB);
    }
    return frc_i_text_finish(&w);
}
