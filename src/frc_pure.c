/*
 * frc_pure.c - the pure wordings: static facts of a level said without an
 * Earth unit (no second, no hertz, no metre).
 *
 * Implements "Pure wording" of fractonica.h on the notation of SPEC S15.1
 * (units), S15.4 (spans), S15.5 (Blinks and pB), S3.4 (names of the rungs) and
 * the media of S16.3:
 *
 *   frc_format_count           "58 500"
 *   frc_format_level_in_unit   "58 500ν": a cycle of a level counted in a unit
 *   frc_format_level_rate      "2 772 pB", "1.420 × 10¹⁴ pB", "1 per 58 500 Blinks"
 *   frc_format_light_span      "24ι 1λ light", "5.15ο light": a length as the
 *                              span light in vacuum needs to cover it
 *   frc_format_light_word      "light", the word of the light span alone
 *   frc_format_level_kind      "major" "unit" "between" "sub-Blink" (ruling 4)
 *   frc_format_count_signed    "−3", "4 820"
 *   frc_format_cycle_place     "8 of 9": a cycle among its siblings, from one
 *   frc_format_level_children  "9 θ": what one cycle of a level is made of
 *   frc_format_level_in_parent "11 in ζ"; frc_format_unit_in_parent "77 in ε"
 *   frc_format_prefix_factors  "125 125 = 5 · 13 · 5 · 5 · 7 · 11"
 *
 * THE MODEL
 *   Everything here is a ratio of prefixes, and every prefix divides the
 *   next, so every count is a whole number or one over a whole number:
 *       a level in a unit    P(level of unit) / P(level)
 *       a rate               P(level) / P(12) cycles per Blink
 *       a light span         B = P(12) / P(level) * v / c Blinks
 *   No catalogue and no Saros interval enters: the same rung covers the same
 *   number of Blinks of light on every interval, because the period and the
 *   Blink stretch together.
 *
 * INVARIANTS
 *   - Integer arithmetic only; these functions exist under FRC_NO_FLOAT.
 *     Every text is rounded ONCE from the exact value: half to even where
 *     this file rounds (three significant digits of a rung, the digits of a
 *     power of ten), and by frc_span.c's own rules where a span or a
 *     per-Blink text is written.
 *   - Nothing is formatted twice: grouping, the power-of-ten form, the span,
 *     the per-Blink text, the names of units and rungs all come from the
 *     writers the rest of the core uses (frc_text.c, frc_span.c,
 *     frc_hierarchy.c).
 *   - A rate below the Blink passes 64 bits at level 36 and has 42 digits at
 *     level 62: it is kept in the base-10^9 limbs of frc_i_ratio_limbs (24
 *     bytes), where grouping and rounding to a few digits are digit work.
 *   - A light span is top / bottom, two 128-bit integers. They never grow
 *     past 2^108: see frc_format_light_span.
 */
#include "frc_internal.h"

#define LAST_LEVEL (FRC_LEVEL_COUNT - 1)

/* ------------------------------------------------------------------------- */
/* Counts                                                                     */
/* ------------------------------------------------------------------------- */

int frc_format_count(uint64_t n, char *buf, size_t size)
{
    frc_i_text_t w;
    frc_i_text_init(&w, buf, size);
    frc_i_text_u64_grouped(&w, n);
    return frc_i_text_finish(&w);
}

/* The words of design ruling 4, by frc_level_kind_t. */
static const char KIND_WORDS[] FRC_PROGMEM = "major\0unit\0between\0sub-Blink\0";

int frc_format_level_kind(frc_level_kind_t kind, char *buf, size_t size)
{
    frc_i_text_t w;

    if (!FRC_I_BELOW(kind, FRC_KIND_SUB + 1)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_nth(&w, KIND_WORDS, (uint8_t)kind);
    return frc_i_text_finish(&w);
}

int frc_format_count_signed(int64_t n, char *buf, size_t size)
{
    frc_i_text_t w;
    /* |n| in uint64_t without negating INT64_MIN: -(n + 1) fits, then + 1 */
    uint64_t magnitude = (n < 0) ? (uint64_t)(-(n + 1)) + 1u : (uint64_t)n;

    frc_i_text_init(&w, buf, size);
    if (n < 0) {
        frc_i_text_cp(&w, FRC_I_CP_MINUS);
    }
    frc_i_text_u64_grouped(&w, magnitude);
    return frc_i_text_finish(&w);
}

static const char OF_WORD[] FRC_PROGMEM = " of ";
static const char IN_WORD[] FRC_PROGMEM = " in ";

int frc_format_cycle_place(const frc_cycle_t *cycle, char *buf, size_t size)
{
    frc_i_text_t w;

    if (cycle == NULL || cycle->siblings == 0 || cycle->local >= cycle->siblings) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_u64_grouped(&w, (uint64_t)cycle->local + 1u); /* counted from one */
    frc_i_text_pgm(&w, OF_WORD);
    frc_i_text_u64_grouped(&w, cycle->siblings);
    return frc_i_text_finish(&w);
}

int frc_format_level_children(uint8_t level, char *buf, size_t size)
{
    frc_i_text_t w;

    if (level >= LAST_LEVEL) { /* the last rung has no level below it */
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_u64(&w, frc_factor(level));
    frc_i_text_char(&w, ' ');
    frc_i_text_level_name(&w, (uint8_t)(level + 1));
    return frc_i_text_finish(&w);
}

int frc_format_level_in_parent(uint8_t level, char *buf, size_t size)
{
    frc_i_text_t w;

    if (level == 0 || level >= FRC_LEVEL_COUNT) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_u64(&w, frc_siblings(level));
    frc_i_text_pgm(&w, IN_WORD);
    frc_i_text_level_name(&w, (uint8_t)(level - 1));
    return frc_i_text_finish(&w);
}

int frc_format_unit_in_parent(frc_unit_t unit, char *buf, size_t size)
{
    frc_i_text_t w;

    /* the next coarser unit is unit - 1; Alpha has none */
    if (!FRC_I_BELOW(unit, FRC_UNIT_COUNT) || unit == FRC_UNIT_ALPHA) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_u64(&w, frc_unit_parts((frc_unit_t)(unit - 1)));
    frc_i_text_pgm(&w, IN_WORD);
    frc_i_text_unit_symbol(&w, (uint8_t)(unit - 1));
    return frc_i_text_finish(&w);
}

int frc_format_level_in_unit(uint8_t level, frc_unit_t unit, char *buf, size_t size)
{
    frc_i_text_t w;
    uint64_t unit_blinks = frc_unit_blinks(unit); /* 0 when `unit` is not a unit */

    /* Every unit sits on a level 0..12, so a level below the Blink is finer
     * than every unit and is refused by the second test alone; the first
     * says so. frc_unit_level is FRC_LEVEL_NONE (255) for a value that is
     * not a unit, which the zero length has already refused. */
    if (unit_blinks == 0 || level > FRC_I_BLINK_LEVEL || level > frc_unit_level(unit)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    /* P(unit level) / P(level) = (P(12) / P(level)) / (P(12) / P(unit level)):
     * the two lengths in Blinks, and the division is exact */
    frc_i_text_u64_grouped(&w, frc_i_level_blinks(level) / unit_blinks);
    frc_i_text_unit_symbol(&w, (uint8_t)unit);
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* Rates                                                                      */
/* ------------------------------------------------------------------------- */

/* A rate below this is written in full whatever precision is asked. */
#define RATE_FULL_BELOW 1000000u
/* Decimal digits in one limb of frc_i_ratio_limbs. */
#define LIMB_TOP_DIGIT 100000000u

/* A whole number in base-10^9 limbs (the top one non-zero), in digit groups
 * of three: a limb is exactly three groups, so the top limb is grouped as
 * the number it is and every lower one follows as three padded groups. */
static void put_limbs_grouped(frc_i_text_t *w, const uint32_t *limbs, uint8_t used)
{
    frc_i_text_u64_grouped(w, limbs[used - 1]);
    while (used > 1) {
        uint32_t limb, scale;
        used--;
        limb = limbs[used - 1];
        for (scale = 1000000u; scale != 0; scale /= 1000u) {
            frc_i_text_cp(w, FRC_I_CP_NNBSP);
            frc_i_text_u64_pad(w, (limb / scale) % 1000u, 3);
        }
    }
}

/*
 * Section 10 of frc_internal.h: the whole number in the limbs as
 * r * 10^(*exponent - digits + 1), where r has exactly `digits` digits and is
 * the number rounded half to even.
 * One pass over the decimal digits from the top: the first `digits` of them
 * make r, the next one and whether anything non-zero follows it decide the
 * rounding. A number of fewer digits is filled with zeros (it is exact), and
 * one that rounds up to 10^digits comes back as 10^(digits-1) with the
 * exponent raised. (No rate of the ladder does: none begins with 95 or more.
 * tests/test_pure.c holds this function directly on numbers that do.)
 */
uint64_t frc_i_limbs_round(const uint32_t *limbs, uint8_t used, uint8_t digits, int32_t *exponent)
{
    uint64_t r = 0;
    int32_t count = 0;  /* significant digits met so far */
    uint8_t next = 0;   /* the first digit that was not taken */
    uint8_t rest = 0;   /* non-zero when a digit after it is */

    while (used > 0) {
        uint32_t limb = limbs[--used];
        uint32_t scale;
        for (scale = LIMB_TOP_DIGIT; scale != 0; scale /= 10u) {
            uint8_t digit = (uint8_t)((limb / scale) % 10u);
            if (count == 0 && digit == 0) {
                continue; /* the top limb is not nine digits long */
            }
            count++;
            if (count <= digits) {
                r = r * 10u + digit;
            } else if (count == digits + 1) {
                next = digit;
            } else {
                rest |= digit;
            }
        }
    }
    *exponent = count - 1;
    for (; count < digits; count++) {
        r *= 10u; /* fewer digits than asked: exact, written with zeros */
    }
    if (next > 5 || (next == 5 && (rest != 0 || (r & 1u) != 0))) {
        r++;
        if (r == frc_i_pow_u64(10, digits)) { /* 9.9996 -> 10.00: 1.000 of the next decade */
            r /= 10u;
            *exponent += 1;
        }
    }
    return r;
}

int frc_format_level_rate(uint8_t level, uint8_t digits, char *buf, size_t size)
{
    frc_i_text_t w;
    uint32_t limbs[FRC_I_PREFIX_LIMBS];
    uint8_t used;

    if (level >= FRC_LEVEL_COUNT || digits == 1 || digits > 9) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    if (level <= FRC_I_BLINK_LEVEL) {
        /* down to the Blink: one cycle per so many Blinks, said by the
         * per-Blink writer itself, so the two can never drift apart */
        uint64_t num, den;
        frc_status_t status = frc_level_per_blink(level, &num, &den);
        if (status != FRC_OK) {
            return frc_i_text_error(buf, size, status); /* no level <= 12 gets here */
        }
        return frc_format_per_blink(num, den, buf, size);
    }
    /* below the Blink: N = P(level) / P(12), a whole number of up to 42 digits */
    used = frc_i_ratio_limbs(FRC_I_BLINK_LEVEL, level, limbs);
    frc_i_text_init(&w, buf, size);
    if (digits == 0 || (used == 1 && limbs[0] < RATE_FULL_BELOW)) {
        put_limbs_grouped(&w, limbs, used);
        frc_i_text_char(&w, ' ');
    } else {
        int32_t exponent;
        uint64_t r = frc_i_limbs_round(limbs, used, digits, &exponent);
        frc_i_text_power_of_ten(&w, r, digits, exponent); /* ends with the space */
    }
    frc_i_text_char(&w, 'p');
    frc_i_text_char(&w, 'B');
    return frc_i_text_finish(&w);
}

/*
 * P(level) = the product of the factors of the levels above it, written out:
 * the prefix in full from its limbs (no 64-bit limit: level 62 has 52
 * digits), then each factor, joined by " · ".
 */
int frc_format_prefix_factors(uint8_t level, char *buf, size_t size)
{
    frc_i_text_t w;
    uint32_t limbs[FRC_I_PREFIX_LIMBS];
    uint8_t used, l;

    if (level >= FRC_LEVEL_COUNT) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    used = frc_i_prefix_limbs(level, limbs); /* level <= 62: 1..6 limbs */
    frc_i_text_init(&w, buf, size);
    put_limbs_grouped(&w, limbs, used);
    for (l = 0; l < level; l++) {
        frc_i_text_char(&w, ' ');
        if (l == 0) {
            frc_i_text_char(&w, '=');
        } else {
            frc_i_text_cp(&w, FRC_I_CP_MIDDOT);
        }
        frc_i_text_char(&w, ' ');
        frc_i_text_u64(&w, frc_factor(l));
    }
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* Light spans                                                                */
/* ------------------------------------------------------------------------- */

/* The one word of ruling 2: frc_format_light_span appends it after a space,
 * frc_format_light_word writes it alone. */
static const char LIGHT_WORD[] FRC_PROGMEM = "light";

int frc_format_light_word(char *buf, size_t size)
{
    frc_i_text_t w;
    frc_i_text_init(&w, buf, size);
    frc_i_text_pgm(&w, LIGHT_WORD);
    return frc_i_text_finish(&w);
}

/* *a = *a * m for a small m. REQUIRES the product to fit 128 bits. */
static void mul128_small(frc_i_u128_t *a, uint8_t m)
{
    frc_i_u128_t low;
    frc_i_mul64(a->lo, m, &low);
    a->hi = a->hi * m + low.hi;
    a->lo = low.lo;
}

/*
 * floor(n / d) and the remainder, for 128-bit n and d. REQUIRES 0 < d < 2^127.
 * @return 1 when the quotient fits 64 bits; 0 otherwise (*quotient then holds
 * its low 64 bits; the remainder is right either way).
 * Long division in base 2, as frc_i_big_divmod does it: before each step
 * rem < d; the step brings down the next bit of n (2 rem + bit < 2 d < 2^128)
 * and takes d off once when it fits, which is that step's quotient bit.
 */
static int div128_128(const frc_i_u128_t *n, const frc_i_u128_t *d, uint64_t *quotient,
                      frc_i_u128_t *rem)
{
    uint64_t q = 0;
    int fits = 1;
    uint8_t bit;

    rem->hi = 0;
    rem->lo = 0;
    for (bit = 128; bit > 0; bit--) {
        uint64_t word = (bit > 64) ? n->hi : n->lo;
        frc_i_add128(rem, rem);
        rem->lo |= (word >> ((bit - 1u) % 64u)) & 1u;
        if ((q >> 63) != 0) {
            fits = 0; /* a set bit leaves the top */
        }
        q <<= 1;
        if (frc_i_cmp128(rem, d) >= 0) {
            frc_i_sub128(rem, d);
            q |= 1u;
        }
    }
    *quotient = q;
    return fits;
}

/* round(n / d), halves to even. REQUIRES 0 < d < 2^126 and a quotient that
 * fits 64 bits (here it is at most 1000). */
static uint64_t div128_round(const frc_i_u128_t *n, const frc_i_u128_t *d)
{
    frc_i_u128_t rem;
    uint64_t q;
    int half;

    (void)div128_128(n, d, &q, &rem);
    frc_i_add128(&rem, &rem); /* 2 rem against d: the dropped fraction against one half */
    half = frc_i_cmp128(&rem, d);
    if (half > 0 || (half == 0 && (q & 1u) != 0)) {
        q++;
    }
    return q;
}

/*
 * top / bottom >= 1 Blinks as a frc_span_t: the whole Blinks, and the rest of
 * one cut by frc_i_blink_fraction, which is what makes the text of the span
 * the text of the exact value. REQUIRES bottom <= top and bottom < 2^124.
 * @return 0 when the whole Blinks do not fit 64 bits (`span` is then unset).
 */
static int span_of_blinks(const frc_i_u128_t *top, const frc_i_u128_t *bottom, frc_span_t *span)
{
    frc_i_u128_t rest;
    uint64_t blinks;
    uint8_t quarters = 0;

    if (!div128_128(top, bottom, &blinks, &rest)) {
        return 0;
    }
    /* rest / bottom of a Blink is 4 rest / bottom quarter Blinks: the whole
     * quarters (0..3) and a fraction of one, the form the cut takes.
     * 4 rest < 4 bottom < 2^126. A rest of exactly one quarter is a whole
     * quarter and no fraction (>=): the cut REQUIRES its fraction below one
     * (exactly 2.5 Blinks would read "2.3ν" otherwise). */
    frc_i_add128(&rest, &rest);
    frc_i_add128(&rest, &rest);
    while (frc_i_cmp128(&rest, bottom) >= 0) {
        frc_i_sub128(&rest, bottom);
        quarters++;
    }
    span->negative = 0;
    span->blinks = blinks;
    span->frac = frc_i_blink_fraction(quarters, &rest, bottom);
    return 1;
}

/*
 * count = top / bottom cycles of `rung` (13..62), with 1 <= count < f, where
 * f = frc_factor(rung - 1) <= 13 is how many of them make one cycle of the
 * next coarser rung. Three significant digits, half to even: "d.dd" below
 * ten, "dd.d" from ten. A count that rounds up to f is one whole cycle of
 * the rung above and is written as that ("1.00" and its name).
 * REQUIRES bottom < 2^100 (top * 100 < 13 * 100 * bottom then fits).
 */
static void put_rung_count(frc_i_text_t *w, const frc_i_u128_t *top, const frc_i_u128_t *bottom,
                           uint8_t rung)
{
    frc_i_u128_t scaled;
    uint8_t whole = frc_factor((uint8_t)(rung - 1));
    int32_t lead = 0; /* digits before the point, less one */
    uint64_t r;       /* the three digits: count = r / 10^(2 - lead) */

    scaled.hi = bottom->hi;
    scaled.lo = bottom->lo;
    mul128_small(&scaled, 10);
    if (frc_i_cmp128(top, &scaled) >= 0) {
        lead = 1; /* 10 or more: two digits, then one decimal */
    }
    scaled.hi = top->hi;
    scaled.lo = top->lo;
    mul128_small(&scaled, (uint8_t)(lead != 0 ? 10 : 100));
    r = div128_round(&scaled, bottom); /* 100 .. 1000 */
    if (r == 1000u) {
        r = 100u; /* 9.995 -> "10.0": count < 13, so lead was 0 */
        lead++;
    }
    /* in hundredths the rounded count is r (lead 0) or 10 r (lead 1) */
    if (r * (lead != 0 ? 10u : 1u) == 100u * (uint64_t)whole) {
        r = 100u;
        lead = 0;
        rung--; /* 12 names the Blink itself: "1.00ν" */
    }
    frc_i_text_mantissa(w, r, 3, lead);
    frc_i_text_level_name(w, rung);
}

/*
 * count = top / bottom < 1 cycles of the last rung, as a power of ten with
 * three significant digits: "8.35 × 10⁻⁸ ο2". A count that rounds up to 1 is
 * one whole cycle: "1.00ο2". Uses `top` up.
 * REQUIRES 0 < top < bottom < 2^100. The exponent is -29 at the least
 * (top >= 1 and bottom < 2^93 in the one caller), so the loop is short.
 */
static void put_below_ladder(frc_i_text_t *w, frc_i_u128_t *top, const frc_i_u128_t *bottom)
{
    int32_t exponent = 0;
    uint64_t r;

    do { /* to bottom <= top < 10 bottom: the mantissa is in [1, 10) */
        mul128_small(top, 10);
        exponent--;
    } while (frc_i_cmp128(top, bottom) < 0);
    mul128_small(top, 100); /* < 1000 bottom < 2^110 */
    r = div128_round(top, bottom); /* 100 .. 1000 */
    if (r == 1000u) {
        r = 100u;
        exponent++;
    }
    if (exponent == 0) {
        frc_i_text_mantissa(w, r, 3, 0);
    } else {
        frc_i_text_power_of_ten(w, r, 3, exponent); /* ends with the space */
    }
    frc_i_text_level_name(w, LAST_LEVEL);
}

/*
 * THE ARITHMETIC. With E = P(12) / P(level) Blinks in one cycle of a level
 * down to the Blink, and c the speed of light,
 *
 *     B = E * num / (den * c)             level <= 12
 *     B = num / (den * c * R)             level  > 12, R = P(level) / P(12)
 *
 * R has up to 140 bits, so B is never built for a level below the Blink.
 * Instead the function keeps  top / bottom = the count of cycles of `rung`
 * the span is, starting where that is cheap:
 *
 *     level <= 12:  rung = 12,     top = E * num,  bottom = den * c   (the count is B)
 *     level  > 12:  rung = level,  top = num,      bottom = den * c
 *
 * and walks the ladder one factor at a time (a cycle of rung K holds
 * frc_factor(K) cycles of rung K + 1) to the rung the text is said in:
 *
 *     count >= 1:  up, while a whole cycle of the next coarser rung fits
 *                  (bottom *= factor), at most to the Blink. There the count
 *                  is B >= 1 and the text is a span.
 *     count  < 1:  down, until one cycle fits (top *= factor), at most to
 *                  level 62. If even that does not fit, the text is a power
 *                  of ten of level 62.
 *
 * Either walk stops at the coarsest rung K >= 13 with count >= 1, and then
 * count < frc_factor(K - 1), because one step earlier it was below 1.
 *
 * BOUNDS. E < 2^33, num and den < 2^64, c < 2^29: top < 2^97 and bottom <
 * 2^93 at the start. Going up, bottom grows only while it stays <= top, and
 * the candidate bottom * factor < 2^101. Going down, top grows only while it
 * is below bottom, so top < 13 * bottom < 2^97. The texts then scale by at
 * most 1000 (put_rung_count, put_below_ladder): nothing passes 2^108.
 */
int frc_format_light_span(uint8_t level, frc_speed_t speed, int with_word, char *buf, size_t size)
{
    frc_i_text_t w;
    frc_i_u128_t top, bottom, scaled;
    frc_span_t span;
    uint8_t rung;

    if (level >= FRC_LEVEL_COUNT || speed.num == 0 || speed.den == 0) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    if (level > FRC_I_BLINK_LEVEL) {
        rung = level;
        top.hi = 0;
        top.lo = speed.num;
    } else {
        rung = FRC_I_BLINK_LEVEL;
        frc_i_mul64(frc_i_level_blinks(level), speed.num, &top);
    }
    frc_i_mul64(speed.den, FRC_I_LIGHT_SPEED, &bottom);

    if (frc_i_cmp128(&top, &bottom) >= 0) {
        while (rung > FRC_I_BLINK_LEVEL) {
            scaled.hi = bottom.hi;
            scaled.lo = bottom.lo;
            mul128_small(&scaled, frc_factor((uint8_t)(rung - 1)));
            if (frc_i_cmp128(&top, &scaled) < 0) {
                break; /* not a whole cycle of the rung above */
            }
            bottom.hi = scaled.hi;
            bottom.lo = scaled.lo;
            rung--;
        }
    } else {
        while (rung < LAST_LEVEL && frc_i_cmp128(&top, &bottom) < 0) {
            mul128_small(&top, frc_factor(rung));
            rung++;
        }
    }

    /* Only the walk up ends on the Blink, and it ends there with B >= 1. The
     * span is made before anything is written: it is what can fail. */
    if (rung == FRC_I_BLINK_LEVEL && !span_of_blinks(&top, &bottom, &span)) {
        return frc_i_text_error(buf, size, FRC_ERR_OVERFLOW); /* 2^64 Blinks or more */
    }
    frc_i_text_init(&w, buf, size);
    if (rung == FRC_I_BLINK_LEVEL) {
        frc_i_text_span(&w, &span, 0);
    } else if (frc_i_cmp128(&top, &bottom) >= 0) {
        put_rung_count(&w, &top, &bottom, rung);
    } else {
        put_below_ladder(&w, &top, &bottom); /* rung is the last one */
    }
    if (with_word) {
        frc_i_text_char(&w, ' ');
        frc_i_text_pgm(&w, LIGHT_WORD);
    }
    return frc_i_text_finish(&w);
}
