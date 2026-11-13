/*
 * frc_cycle.c - cycles, counters and thresholds: where an instant sits in a
 * level, and when the level's boundaries happen.
 *
 * Implements SPEC S3.2 (cycle and local index), S6.1 (sibling counter), S6.2
 * (thresholds) and the clipping rule of S2.2 / pitfall 7. Sections 5 and 6 of
 * frc_internal.h (the cycle fraction and the boundary instants, on which the
 * glyph, odometer, sync and date code are built) are implemented here, and
 * their inverse for a client: frc_time_at_phase, the instant of a phase.
 *
 * THE MODEL
 *   P = frc_prefix(level) is the number of level cycles in 4 Saros, so
 *
 *       position * P      counts QUARTER cycles since position 0
 *       position * P / 4  counts cycles
 *
 *   For an instant, position * P = total + rem/den exactly (frc_i_fraction):
 *   cycle = total / 4, quarter = total % 4, and the boundaries of the level
 *   (cycle starts and the peak / node / valley thresholds) are precisely the
 *   integer values of position * P. A boundary T happens at position T / P,
 *   that is at E[k] + r * I_k / P with k = T / P, r = T % P.
 *
 * INVARIANTS
 *   - Integer only. Products go through the 128-bit helpers; every division
 *     has its "quotient fits 64 bits" bound stated where it is made.
 *   - A boundary instant is the first microsecond AT OR AFTER the exact
 *     rational instant (ceiling). So position(start of cycle c) is in cycle c
 *     and position(start - 1 us) is in cycle c - 1.
 *   - Nothing exists beyond the catalogue: the last cycle of a level is
 *     clipped at the closing instant, and a boundary past it is FRC_TIME_NONE.
 *   - A "next" instant is strictly after the query instant, or FRC_TIME_NONE.
 *     At the closing instant itself there is no next instant at all.
 *   - Levels above FRC_CLOCK_MAX_LEVEL are FRC_ERR_ARG. `total` may need more
 *     than 64 bits on a long series at level 21, so it is kept in 128; it is
 *     always below 2^66 and the cycle index always fits 64 bits
 *     (frc_i_extent refuses a catalogue for which it would not).
 */
#include "frc_internal.h"

/* ------------------------------------------------------------------------- */
/* Small 128-bit conveniences for quarter counts                              */
/* ------------------------------------------------------------------------- */

/* *total = 4 * cycle + quarter as 128 bits (4 * cycle alone can pass 2^64). */
static void total_of(uint64_t cycle, uint8_t quarter, frc_i_u128_t *total)
{
    total->hi = cycle >> 62;
    total->lo = cycle << 2;
    frc_i_add128_64(total, quarter);
}

/* total / 4. REQUIRES total < 2^66 so the quotient fits. */
static uint64_t cycle_of(const frc_i_u128_t *total)
{
    return (total->hi << 62) | (total->lo >> 2);
}

/* ------------------------------------------------------------------------- */
/* The extent of a level on a catalogue                                       */
/* ------------------------------------------------------------------------- */

frc_status_t frc_i_extent(const frc_catalogue_t *cat, uint8_t level,
                          uint64_t *prefix, frc_i_u128_t *extent)
{
    uint64_t p;
    frc_i_u128_t ext;

    if (!frc_i_cat_ok(cat) || level > FRC_CLOCK_MAX_LEVEL) {
        return FRC_ERR_ARG;
    }
    p = frc_prefix(level);
    frc_i_mul64((uint64_t)cat->count - 1, p, &ext);
    /* The cycle count is ceil(ext / 4) = (ext + 3) / 4. It fits 64 bits iff
     * ext + 3 < 2^66, i.e. ext <= 2^66 - 4: the high word is at most 3, and
     * when it is 3 the low word leaves room for the 3. */
    if (ext.hi > 3 || (ext.hi == 3 && ext.lo > UINT64_MAX - 3)) {
        return FRC_ERR_OVERFLOW;
    }
    *prefix = p;
    extent->hi = ext.hi; /* written only on success, field by field */
    extent->lo = ext.lo;
    return FRC_OK;
}

uint64_t frc_cycle_count(const frc_catalogue_t *cat, uint8_t level)
{
    uint64_t p;
    frc_i_u128_t ext;

    if (frc_i_extent(cat, level, &p, &ext) != FRC_OK) {
        return 0;
    }
    /* ceil((count - 1) * P / 4): the last, partial cycle counts as one.
     * frc_i_extent has checked that ext + 3 stays below 2^66. */
    frc_i_add128_64(&ext, 3);
    return cycle_of(&ext);
}

/* ------------------------------------------------------------------------- */
/* The cycle fraction of an instant (frc_internal.h section 5)                */
/* ------------------------------------------------------------------------- */

frc_status_t frc_i_fraction(const frc_catalogue_t *cat, const frc_position_t *pos,
                            uint8_t level, frc_i_fraction_t *out)
{
    uint64_t p, ip, rem;
    frc_i_u128_t ext;
    frc_status_t status;

    if (pos == NULL || out == NULL) {
        return FRC_ERR_ARG;
    }
    status = frc_i_extent(cat, level, &p, &ext);
    if (status != FRC_OK) {
        return status;
    }
    if (pos->den == 0 || pos->num >= pos->den || pos->ordinal >= cat->count - 1) {
        return FRC_ERR_ARG; /* not a position of this catalogue */
    }
    /* num * P / den < P because num < den: the quotient fits 64 bits. */
    ip = frc_i_muldiv_floor(pos->num, p, pos->den, &rem);
    /* ordinal * P + ip < (ordinal + 1) * P <= ext < 2^66: no wrap, and
     * total / 4 fits 64 bits. */
    frc_i_mul64(pos->ordinal, p, &out->total);
    frc_i_add128_64(&out->total, ip);
    out->cycle = cycle_of(&out->total);
    out->q4 = (uint8_t)(out->total.lo & 3u);
    out->rem = rem;
    out->den = pos->den;
    out->closing = 0;
    return FRC_OK;
}

frc_status_t frc_i_fraction_at(const frc_catalogue_t *cat, frc_time_t t,
                               uint8_t level, frc_i_fraction_t *out)
{
    frc_position_t pos;
    frc_status_t status;

    /* argument errors win over range errors: check the level first */
    if (!frc_i_cat_ok(cat) || out == NULL || level > FRC_CLOCK_MAX_LEVEL) {
        return FRC_ERR_ARG;
    }
    status = frc_position_at(cat, t, &pos);
    if (status != FRC_OK) {
        return status;
    }
    status = frc_i_fraction(cat, &pos, level, out);
    if (status == FRC_OK) {
        /* The closing instant was read one microsecond early. Remember it,
         * so no boundary of that reading is offered as a "next" instant. */
        out->closing = (t == frc_catalogue_last(cat)) ? 1 : 0;
    }
    return status;
}

frc_frac_t frc_i_fraction_phase(const frc_i_fraction_t *f)
{
    /* (q4 + rem/den) / 4 in 0.32 fixed point is (q4 + rem/den) * 2^30:
     * q4 lands in the top two bits and floor(rem * 2^30 / den) < 2^30 fills
     * the rest. The floor of the sum is the sum of q4 * 2^30 and that floor,
     * so the result is the exact floor of the true phase. */
    uint64_t low = frc_i_muldiv_floor(f->rem, (uint64_t)1 << 30, f->den, NULL);
    return ((frc_frac_t)f->q4 << 30) | (frc_frac_t)low;
}

/* floor(rem * w / den): the state inside the current quarter, 0 .. w-1. */
static uint64_t step_in_quarter(const frc_i_fraction_t *f, uint64_t w)
{
    return frc_i_muldiv_floor(f->rem, w, f->den, NULL); /* < w because rem < den */
}

uint64_t frc_i_fraction_step(const frc_i_fraction_t *f, uint64_t w)
{
    /* floor((q4 + rem/den) * w) = q4 * w + floor(rem * w / den) */
    return (uint64_t)f->q4 * w + step_in_quarter(f, w);
}

frc_time_t frc_i_fraction_step_end(const frc_catalogue_t *cat, uint8_t level,
                                   const frc_i_fraction_t *f, uint64_t w)
{
    if (w == 0 || f->closing) {
        return FRC_TIME_NONE; /* nothing changes after the closing instant */
    }
    /* the state after the current one starts (step + 1) / w into the quarter */
    return frc_i_time_after_total(cat, level, &f->total, step_in_quarter(f, w) + 1, w);
}

/* ------------------------------------------------------------------------- */
/* Boundary instants (frc_internal.h section 6)                               */
/* ------------------------------------------------------------------------- */

frc_time_t frc_i_time_at_total(const frc_catalogue_t *cat, uint8_t level, const frc_i_u128_t *total)
{
    uint64_t p, k, r;
    frc_i_u128_t ext;

    if (frc_i_extent(cat, level, &p, &ext) != FRC_OK || frc_i_cmp128(total, &ext) > 0) {
        return FRC_TIME_NONE;
    }
    /* total <= ext = (count - 1) * P, so k = total / P <= count - 1 fits */
    k = frc_i_div128(total, p, &r);
    return frc_i_instant(cat, k, r, p);
}

frc_time_t frc_i_time_at_quarter(const frc_catalogue_t *cat, uint8_t level,
                                 uint64_t cycle, uint8_t quarter)
{
    frc_i_u128_t total;
    total_of(cycle, quarter, &total);
    return frc_i_time_at_total(cat, level, &total);
}

frc_time_t frc_i_time_after_total(const frc_catalogue_t *cat, uint8_t level,
                                  const frc_i_u128_t *total, uint64_t a, uint64_t b)
{
    uint64_t p, k, r, interval, lift, offset;
    frc_i_u128_t ext, whole, n;

    if (b == 0 || frc_i_extent(cat, level, &p, &ext) != FRC_OK ||
        frc_i_cmp128(total, &ext) > 0) {
        return FRC_TIME_NONE;
    }
    /* Normalise a/b to whole + proper fraction, so the fraction stays inside
     * ONE Saros interval's quarter. total <= ext < 2^66: adding < 2^64 cannot wrap. */
    whole.hi = total->hi;
    whole.lo = total->lo;
    frc_i_add128_64(&whole, a / b);
    a %= b;
    if (a == 0) {
        return frc_i_time_at_total(cat, level, &whole);
    }
    if (frc_i_cmp128(&whole, &ext) >= 0) {
        return FRC_TIME_NONE; /* a positive fraction past the last boundary */
    }
    k = frc_i_div128(&whole, p, &r); /* whole < ext: k <= count - 2, r < P */
    interval = frc_i_cat_interval(cat, (uint16_t)k);

    /* The instant is E[k] + (r + a/b) * I / P. With N = r * I (an integer)
     * and y = a * I / b (real, 0 < y < I):
     *     ceil((N + y) / P) == ceil((N + ceil(y)) / P)
     * since m * P >= N + y  <=>  m * P - N >= y  <=>  m * P - N >= ceil(y)
     * for integers m, P, N. Two exact ceilings, no rounding error. */
    lift = frc_i_muldiv_ceil(a, interval, b); /* ceil(y) <= I because a < b */
    frc_i_mul64(r, interval, &n);
    frc_i_add128_64(&n, lift);
    /* n <= (r + 1) * I <= P * I, so n / P <= I fits 64 bits */
    offset = frc_i_div128_ceil(&n, p);
    return (frc_time_t)((uint64_t)frc_i_cat_time(cat, (uint16_t)k) + offset);
}

/* ------------------------------------------------------------------------- */
/* S3.2, S6.1  Cycles                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Fill everything about cycle `cycle` that does not depend on an instant.
 * REQUIRES a validated (cat, level) and cycle < frc_cycle_count(cat, level).
 */
static void fill_cycle(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle, frc_cycle_t *out)
{
    frc_time_t end = frc_i_time_at_quarter(cat, level, cycle, 4);

    out->level = level;
    out->cycle = cycle;
    if (level == 0) {
        /* Alpha has no parent: its "siblings" are the Alphas of the whole
         * catalogue, at most ceil(65534 / 4), so both fit uint16_t. */
        out->siblings = (uint16_t)frc_cycle_count(cat, 0);
        out->local = (uint16_t)cycle;
    } else {
        out->siblings = frc_siblings(level);
        out->local = (uint16_t)(cycle % out->siblings);
    }
    out->start = frc_i_time_at_quarter(cat, level, cycle, 0);
    /* a nominal end past the catalogue: the cycle is cut at the closing instant */
    out->clipped = (end == FRC_TIME_NONE) ? 1 : 0;
    out->end = out->clipped ? frc_catalogue_last(cat) : end;
}

frc_status_t frc_cycle_at(const frc_catalogue_t *cat, frc_time_t t, uint8_t level, frc_cycle_t *out)
{
    frc_i_fraction_t f;
    frc_status_t status;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = frc_i_fraction_at(cat, t, level, &f);
    if (status != FRC_OK) {
        return status;
    }
    fill_cycle(cat, level, f.cycle, out);
    out->quarter = f.q4;
    out->phase = frc_i_fraction_phase(&f);
    return FRC_OK;
}

/* Is `cycle` a cycle of `level` on this catalogue? The argument errors of
 * frc_i_extent first, then FRC_ERR_RANGE for an index past the last cycle. */
static frc_status_t check_cycle_index(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle)
{
    uint64_t p;
    frc_i_u128_t ext;
    frc_status_t status = frc_i_extent(cat, level, &p, &ext);

    if (status != FRC_OK) {
        return status;
    }
    return (cycle < frc_cycle_count(cat, level)) ? FRC_OK : FRC_ERR_RANGE;
}

frc_status_t frc_cycle_by_index(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle, frc_cycle_t *out)
{
    frc_status_t status;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = check_cycle_index(cat, level, cycle);
    if (status != FRC_OK) {
        return status;
    }
    fill_cycle(cat, level, cycle, out);
    out->quarter = 0;
    out->phase = 0;
    return FRC_OK;
}

frc_status_t frc_cycle_points(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle, frc_time_t out[5])
{
    frc_status_t status;
    uint8_t quarter;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = check_cycle_index(cat, level, cycle);
    if (status != FRC_OK) {
        return status;
    }
    /* points of a clipped cycle that fall after the catalogue come out as
     * FRC_TIME_NONE: they are not invented (pitfall 7) */
    for (quarter = 0; quarter <= 4; quarter++) {
        out[quarter] = frc_i_time_at_quarter(cat, level, cycle, quarter);
    }
    return FRC_OK;
}

/*
 * The instant of a phase of a cycle. phase / 2^32 of the cycle is
 * 4 * phase / 2^32 = (phase >> 30) + (phase mod 2^30) / 2^30 quarters, so the
 * boundary is position * P = 4 * cycle + (phase >> 30) + low / 2^30: a whole
 * quarter count and a proper fraction, which frc_i_time_after_total times
 * exactly (ceiling). frc_cycle_at's phase is the floor of 2^30 times that
 * same fraction, hence the inverse: at the returned microsecond the phase is
 * >= `phase`, one microsecond earlier it is below.
 */
frc_time_t frc_time_at_phase(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle, frc_frac_t phase)
{
    frc_i_u128_t total;

    if (check_cycle_index(cat, level, cycle) != FRC_OK) {
        return FRC_TIME_NONE;
    }
    total_of(cycle, (uint8_t)(phase >> 30), &total);
    /* past the closing instant (the clipped last cycle): FRC_TIME_NONE */
    return frc_i_time_after_total(cat, level, &total, phase & 0x3FFFFFFFu, (uint64_t)1 << 30);
}

/* ------------------------------------------------------------------------- */
/* S6.2  Thresholds                                                           */
/* ------------------------------------------------------------------------- */

frc_status_t frc_next_threshold(const frc_catalogue_t *cat, frc_time_t t, uint8_t level, frc_threshold_t *out)
{
    frc_i_fraction_t f;
    frc_status_t status;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = frc_i_fraction_at(cat, t, level, &f);
    if (status != FRC_OK) {
        return status;
    }
    /* Boundaries are the integer values of position * P. floor(position * P)
     * = total, so total + 1 is the first one STRICTLY after the instant, even
     * when the instant sits exactly on a boundary. It ends quarter q4, and a
     * quarter is named by the threshold that ends it. */
    out->name = f.q4;
    frc_i_add128_64(&f.total, 1);
    out->time = frc_i_time_at_total(cat, level, &f.total);
    if (f.closing) {
        /* The closing instant reads one microsecond early, so the boundary
         * found above is not after `t` itself (it is `t` at the latest).
         * Nothing changes after the end of the catalogue: the quarter keeps
         * its name, and no time is promised. Elsewhere the ceiling of a
         * boundary strictly after position(t) is always > t. */
        out->time = FRC_TIME_NONE;
    }
    return FRC_OK;
}

/* Shared with the date writer and parser of frc_notation.c. */
const char frc_i_quarter_names[] FRC_PROGMEM = "Peak\0Node\0Valley\0End\0";

int frc_format_quarter(frc_quarter_t quarter, int lower, char *buf, size_t size)
{
    frc_i_text_t w;
    const char *name;

    if (!FRC_I_BELOW(quarter, FRC_END + 1)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    name = frc_i_packed_nth(frc_i_quarter_names, (uint8_t)quarter);
    frc_i_text_init(&w, buf, size);
    /* the names are ASCII with a capital first letter: bit 5 makes it lower case */
    frc_i_text_char(&w, (char)(FRC_PGM_CHAR(name) | (lower ? 0x20 : 0)));
    frc_i_text_pgm(&w, name + 1);
    return frc_i_text_finish(&w);
}
