/*
 * frc_sync.c - the solar sync and its Pentads: the clock's link to civil days.
 *
 * Implements SPEC S7.1 (definition), S7.2 (reference configuration: level 6,
 * 19 Helions ~ 4 days, counted from the catalogue's anchor eclipse), S7.3
 * (odometer and thresholds of the sync) and S7.5 (Pentads), with the places
 * the Sun's ring needs: the running Helion's place in the sync
 * (frc_sync_t.helion = within below), a Pentad's first Helion (frc_pentad_t
 * .first), and the instant of a sync phase (frc_time_at_sync_phase).
 *
 * THE MODEL
 *   Helions (level-6 cycles) are numbered from the one the anchor eclipse
 *   starts: anchorCycle = A * P / 4, with P = frc_prefix(6). Then
 *
 *       h      = cycle(6) - anchorCycle        Helions since the anchor
 *       sync   = floor(h / 19)                 negative before the anchor
 *       within = h - 19 * sync                 0 .. 18
 *       Pentad = min(3, within / 5)            5 + 5 + 5 + 4 Helions
 *
 *   A sync starts and ends on Helion boundaries, which are the exact
 *   rational instants E[k] + r * I_k / P of frc_internal.h (section 6).
 *   Unlike everything else in the clock, the PHASE of a sync is linear in
 *   TIME: (t - start) / (end - start), with the exact start and end. All of
 *   it is kept exact by measuring time from E[k], the eclipse before the
 *   sync's start, in units of 1/P microsecond (sync_span_t): every boundary
 *   is then an integer, the phase is one integer over another, and each
 *   returned instant is rounded up to the microsecond exactly once.
 *
 * INVARIANTS
 *   - The sync reading exists only for WHOLE syncs: when its start or end
 *     would fall outside the catalogue the answer is FRC_ERR_RANGE (S7.1).
 *     Pentads count whole Helions and are defined in the partial syncs too,
 *     cut at the catalogue's edges (S7.5).
 *   - The anchor must start a Helion: A * P divisible by 4, i.e. A = 0 mod 4
 *     because P(6) = 125125 is odd. Otherwise FRC_ERR_ARG.
 *   - start/end are ceilings of the exact boundaries, as everywhere. The
 *     phase, the quarter and the odometer digits come from the EXACT
 *     boundaries, never from those rounded microseconds (they would differ in
 *     the last bit now and then).
 *   - A "next" instant is strictly after the query, or FRC_TIME_NONE at the
 *     catalogue's closing instant, which is read one microsecond early like
 *     every other reading (frc_position_at).
 *   - Level-6 quarter counts stay below 2^16 * 2^17: they are handled in 64
 *     bits here; only products with a time need 128.
 */
#include "frc_internal.h"

/* Quarter Helions in one sync: positions are counted in quarter cycles. */
#define SYNC_QUARTERS (4 * FRC_SYNC_CYCLES)
/* The civil length a sync is compared with, in microseconds. */
#define SYNC_NOMINAL_US ((uint64_t)FRC_SYNC_DAYS * (uint64_t)FRC_US_PER_DAY)

/* ------------------------------------------------------------------------- */
/* The Helion grid of a catalogue                                             */
/* ------------------------------------------------------------------------- */

typedef struct sync_grid {
    uint64_t prefix;       /* P(6): quarter Helions per Saros               */
    uint64_t extent;       /* (count - 1) * P: the catalogue in quarter Helions */
    int64_t  anchor_cycle; /* A * P / 4: the Helion the anchor eclipse starts */
} sync_grid_t;

static frc_status_t sync_grid(const frc_catalogue_t *cat, sync_grid_t *grid)
{
    frc_i_u128_t extent;
    uint64_t anchor_quarters;
    frc_status_t status;

    status = frc_i_extent(cat, FRC_SYNC_LEVEL, &grid->prefix, &extent);
    if (status != FRC_OK) {
        return status;
    }
    /* anchor < 2^16 and P(6) < 2^17: no overflow, and extent.hi is 0 */
    anchor_quarters = (uint64_t)cat->anchor * grid->prefix;
    if (cat->anchor >= cat->count || anchor_quarters % 4 != 0) {
        return FRC_ERR_ARG; /* the anchor must be an eclipse that starts a Helion */
    }
    grid->extent = extent.lo;
    grid->anchor_cycle = (int64_t)(anchor_quarters / 4);
    return FRC_OK;
}

/* Instant of quarter count `total` (0 .. extent), ceiled to the microsecond. */
static frc_time_t time_at(const frc_catalogue_t *cat, uint64_t total)
{
    frc_i_u128_t wide;
    wide.hi = 0;
    wide.lo = total;
    return frc_i_time_at_total(cat, FRC_SYNC_LEVEL, &wide);
}

/* ------------------------------------------------------------------------- */
/* One whole sync, laid out for exact arithmetic                              */
/* ------------------------------------------------------------------------- */

/*
 * Times inside a sync are measured from E[origin], the eclipse at or before
 * its start, and multiplied by P. The boundary at quarter count T = k * P + r
 * is the instant E[k] + r * I_k / P, so in these units it is the INTEGER
 *
 *     scaled(T) = (E[k] - E[origin]) * P + r * I_k
 *
 * and a query instant t is (t - E[origin]) * P. Nothing is rounded until an
 * instant is handed back (span_instant).
 */
typedef struct sync_span {
    uint64_t     prefix; /* P                                                */
    uint64_t     start;  /* quarter count of the sync's start                */
    uint16_t     origin; /* k of the start: E[origin] <= start < E[origin+1] */
    frc_i_u128_t lead;   /* scaled(start) = r * I_origin; up to ~2^67        */
    uint64_t     length; /* scaled(end) - scaled(start) = (end - start) * P  */
} sync_span_t;

/* *out = scaled(total) as defined above. REQUIRES origin <= total / P and
 * total <= extent (so r != 0 implies that interval k exists). */
static void scaled(const frc_catalogue_t *cat, uint64_t prefix, uint64_t total, uint16_t origin,
                   frc_i_u128_t *out)
{
    uint16_t k = (uint16_t)(total / prefix); /* <= count - 1 */
    uint64_t r = total % prefix;
    uint64_t whole = (uint64_t)frc_i_cat_time(cat, k) - (uint64_t)frc_i_cat_time(cat, origin);

    frc_i_mul64(whole, prefix, out);
    if (r != 0) {
        frc_i_u128_t inside; /* the part of the way into interval k */
        frc_i_mul64(r, frc_i_cat_interval(cat, k), &inside);
        frc_i_add128(out, &inside);
    }
}

/*
 * Lay out sync `number`.
 * @return FRC_ERR_RANGE    its start or end is outside the catalogue
 *         FRC_ERR_OVERFLOW (end - start) * P does not fit 64 bits: a Saros
 *                          interval beyond 7,000 years, no real series
 */
static frc_status_t span_of(const frc_catalogue_t *cat, const sync_grid_t *grid,
                            int64_t number, sync_span_t *span)
{
    /* |number| < 2^31 and anchor_cycle < 2^34: plain 64-bit arithmetic */
    int64_t first = grid->anchor_cycle + FRC_SYNC_CYCLES * number;
    uint64_t start;
    frc_i_u128_t length;

    if (first < 0) {
        return FRC_ERR_RANGE;
    }
    start = (uint64_t)first * 4;
    if (start + SYNC_QUARTERS > grid->extent) {
        return FRC_ERR_RANGE;
    }
    span->prefix = grid->prefix;
    span->start = start;
    span->origin = (uint16_t)(start / grid->prefix); /* < count - 1: start < extent */
    scaled(cat, grid->prefix, start, span->origin, &span->lead);
    scaled(cat, grid->prefix, start + SYNC_QUARTERS, span->origin, &length);
    frc_i_sub128(&length, &span->lead);
    if (length.hi != 0) {
        return FRC_ERR_OVERFLOW;
    }
    span->length = length.lo; /* > 0: the end is a later instant */
    return FRC_OK;
}

/* (t - start) * P for an instant inside the sync (start <= t < end, exact
 * start and end): in 0 .. length - 1, so the low word is the whole value. */
static uint64_t span_elapsed(const frc_catalogue_t *cat, const sync_span_t *span, frc_time_t t)
{
    uint64_t since_origin = (uint64_t)t - (uint64_t)frc_i_cat_time(cat, span->origin);
    frc_i_u128_t elapsed;

    frc_i_mul64(since_origin, span->prefix, &elapsed);
    frc_i_sub128(&elapsed, &span->lead);
    return elapsed.lo;
}

/*
 * The instant start + (a / b) * (end - start) for 0 <= a <= b, b != 0: the
 * first microsecond at or after it. In scaled units it is lead + a*length/b,
 * so the instant is E[origin] + (lead + y) / P with y = a * length / b real.
 * As in frc_i_time_after_total, for integers lead and P
 *     ceil((lead + y) / P) == ceil((lead + ceil(y)) / P),
 * so two exact ceilings give the true one.
 */
static frc_time_t span_instant(const frc_catalogue_t *cat, const sync_span_t *span, uint64_t a, uint64_t b)
{
    uint64_t lift = frc_i_muldiv_ceil(a, span->length, b); /* <= length: a <= b */
    frc_i_u128_t lifted;
    uint64_t offset;

    lifted.hi = span->lead.hi;
    lifted.lo = span->lead.lo;
    frc_i_add128_64(&lifted, lift);
    /* (lead + lift) / P is the microseconds from E[origin] to an instant of
     * the catalogue, so the quotient fits 64 bits */
    offset = frc_i_div128_ceil(&lifted, span->prefix);
    return (frc_time_t)((uint64_t)frc_i_cat_time(cat, span->origin) + offset);
}

/* ------------------------------------------------------------------------- */
/* S7.1  The sync                                                             */
/* ------------------------------------------------------------------------- */

/*
 * E[anchor] + number * 4 days, the civil grid the sync is compared with.
 * Checked: on a real series it is within hours of the sync's start, but a
 * hand-built catalogue may stretch across the whole int64 range.
 */
static frc_status_t nominal_start(const frc_catalogue_t *cat, int64_t number, frc_time_t *out)
{
    uint64_t anchor = (uint64_t)frc_i_cat_time(cat, cat->anchor);
    uint64_t count = (uint64_t)(number < 0 ? -number : number);
    uint64_t shift;
    /* how far a time may move up (down) from the anchor and stay an int64;
     * in two's-complement unsigned arithmetic both differences are exact */
    uint64_t room = (number < 0) ? anchor - (uint64_t)INT64_MIN : (uint64_t)INT64_MAX - anchor;

    if (!frc_i_mul64_fits(count, SYNC_NOMINAL_US, &shift) || shift > room) {
        return FRC_ERR_OVERFLOW;
    }
    *out = (frc_time_t)(number < 0 ? anchor - shift : anchor + shift);
    return FRC_OK;
}

/*
 * Fill what does not depend on an instant: number, start, end, nominal and
 * drift; phase and quarter are 0 and next is {0, FRC_TIME_NONE}, as
 * frc_sync_by_number promises.
 * Writes nothing when it fails.
 */
static frc_status_t fill_sync(const frc_catalogue_t *cat, const sync_span_t *span,
                              int64_t number, frc_sync_t *out)
{
    frc_time_t start = time_at(cat, span->start);
    frc_time_t nominal;
    frc_status_t status = nominal_start(cat, number, &nominal);

    if (status != FRC_OK) {
        return status;
    }
    /* drift = start - nominal is seconds per sync on a real series. Only a
     * hand-built table that spans most of the int64 range, with syncs nothing
     * like four days long, can push the difference out of 64 bits. */
    if ((nominal > 0 && start < INT64_MIN + nominal) || (nominal < 0 && start > INT64_MAX + nominal)) {
        return FRC_ERR_OVERFLOW;
    }
    out->sync = (int32_t)number; /* at most 2^16 * 2^17 / 76 syncs: fits */
    out->start = start;
    out->end = time_at(cat, span->start + SYNC_QUARTERS);
    out->nominal = nominal;
    out->drift_us = start - nominal;
    out->phase = 0;
    out->quarter = 0;
    out->helion = 0;
    out->next.name = 0;
    out->next.time = FRC_TIME_NONE; /* not 0: that would read as the Unix epoch */
    return FRC_OK;
}

/* The running sync at `t`, laid out, and how far `t` is into it. */
typedef struct sync_reading {
    int64_t     number;
    sync_span_t span;
    uint64_t    elapsed; /* (t - start) * P, in 0 .. span.length - 1          */
    uint8_t     closing; /* 1 at the catalogue's closing instant              */
    uint8_t     helion;  /* the running Helion's place in the sync, 0..18     */
} sync_reading_t;

static frc_status_t read_sync(const frc_catalogue_t *cat, frc_time_t t, sync_reading_t *reading)
{
    sync_grid_t grid;
    frc_i_fraction_t f;
    frc_status_t status;

    status = sync_grid(cat, &grid);
    if (status != FRC_OK) {
        return status;
    }
    status = frc_i_fraction_at(cat, t, FRC_SYNC_LEVEL, &f);
    if (status != FRC_OK) {
        return status;
    }
    /* Helion indices are below 2^34: the signed difference is exact */
    reading->number = frc_i_floor_div((int64_t)f.cycle - grid.anchor_cycle, FRC_SYNC_CYCLES);
    /* h - 19 * sync, the floor division's remainder: 0..18 (the closing
     * instant reads one microsecond early, so it is the last Helion's) */
    reading->helion = (uint8_t)((int64_t)f.cycle - grid.anchor_cycle - FRC_SYNC_CYCLES * reading->number);
    status = span_of(cat, &grid, reading->number, &reading->span);
    if (status != FRC_OK) {
        return status;
    }
    /* The closing instant belongs to the sync that ends there: like the
     * position, the phase is read one microsecond early, so it stays < 1. */
    reading->closing = f.closing;
    reading->elapsed = span_elapsed(cat, &reading->span, f.closing ? t - 1 : t);
    return FRC_OK;
}

frc_status_t frc_sync_at(const frc_catalogue_t *cat, frc_time_t t, frc_sync_t *out)
{
    sync_reading_t reading;
    frc_status_t status;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = read_sync(cat, t, &reading);
    if (status != FRC_OK) {
        return status;
    }
    status = fill_sync(cat, &reading.span, reading.number, out);
    if (status != FRC_OK) {
        return status;
    }
    /* elapsed < length, so the quotient is below 2^32. Its top two bits are
     * floor(4 * elapsed / length): the quarter, exactly (nested floors). */
    out->phase = (frc_frac_t)frc_i_muldiv_floor(reading.elapsed, (uint64_t)1 << 32,
                                                reading.span.length, NULL);
    out->quarter = (uint8_t)(out->phase >> 30);
    out->helion = reading.helion;
    /* S7.3: the quarters of the sync are named like a cycle's. The boundary
     * that ends the running quarter is exactly after t, so its ceiling is too. */
    out->next.name = out->quarter;
    out->next.time = reading.closing
                         ? FRC_TIME_NONE
                         : span_instant(cat, &reading.span, (uint64_t)out->quarter + 1, 4);
    return FRC_OK;
}

frc_status_t frc_sync_by_number(const frc_catalogue_t *cat, int32_t number, frc_sync_t *out)
{
    sync_grid_t grid;
    sync_span_t span;
    frc_status_t status;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = sync_grid(cat, &grid);
    if (status != FRC_OK) {
        return status;
    }
    status = span_of(cat, &grid, number, &span);
    if (status != FRC_OK) {
        return status;
    }
    return fill_sync(cat, &span, number, out);
}

/* The inverse of the sync phase: start + phase / 2^32 of the way to the
 * end, on the exact boundaries, ceiled once (span_instant). The phase of
 * frc_sync_at is floor(2^32 * elapsed / length), so it is >= `phase` exactly
 * from that microsecond on. */
frc_time_t frc_time_at_sync_phase(const frc_catalogue_t *cat, int32_t sync, frc_frac_t phase)
{
    sync_grid_t grid;
    sync_span_t span;

    if (sync_grid(cat, &grid) != FRC_OK || span_of(cat, &grid, sync, &span) != FRC_OK) {
        return FRC_TIME_NONE;
    }
    return span_instant(cat, &span, phase, (uint64_t)1 << 32);
}

frc_status_t frc_sync_odometer(const frc_catalogue_t *cat, frc_time_t t, uint8_t strokes,
                               uint8_t digits, frc_odometer_t *out)
{
    sync_reading_t reading;
    frc_status_t status;
    uint64_t steps, step;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = frc_i_odometer_steps(strokes, digits, &steps);
    if (status != FRC_OK) {
        return status;
    }
    status = read_sync(cat, t, &reading);
    if (status != FRC_OK) {
        return status;
    }
    /* S7.3: floor(phase * q^k) on the exact phase elapsed / length (< 1, so
     * the step is below q^k). Not derived from the 32-bit phase: q^k need
     * not divide 2^32. */
    step = frc_i_muldiv_floor(reading.elapsed, steps, reading.span.length, NULL);
    frc_i_odometer_fill(out, strokes, digits, step);
    /* the step ends (step + 1) / q^k of the way through the sync */
    if (!reading.closing) {
        out->next_change = span_instant(cat, &reading.span, step + 1, steps);
    }
    return FRC_OK;
}

/* ------------------------------------------------------------------------- */
/* S7.5  Pentads                                                              */
/* ------------------------------------------------------------------------- */

/* Fill sync, index, helions, start and end of Pentad `index` (0..3) of sync
 * `number`; FRC_ERR_RANGE when no part of it is inside the catalogue. */
static frc_status_t fill_pentad(const frc_catalogue_t *cat, const sync_grid_t *grid,
                                int64_t number, uint8_t index, frc_pentad_t *out)
{
    uint8_t helions = (uint8_t)(index == FRC_I_HYPOPENTAD ? FRC_I_PENTAD_HELIONS - 1 : FRC_I_PENTAD_HELIONS);
    /* first Helion of the Pentad and the first one after it */
    int64_t sync_first = grid->anchor_cycle + FRC_SYNC_CYCLES * number;
    int64_t first = sync_first + FRC_I_PENTAD_HELIONS * (int64_t)index;
    int64_t last = first + helions;

    /* wholly outside: it ends at or before position 0, or starts at or
     * after the closing instant */
    if (last <= 0 || (first >= 0 && (uint64_t)first * 4 >= grid->extent)) {
        return FRC_ERR_RANGE;
    }
    out->sync = (int32_t)number;
    out->index = index;
    out->helion = 0;
    out->helions = helions;
    /* its first Helion's place in the sync: the Helions of the Pentads
     * before it (5 each), 0 5 10 15; the nominal place also for a Pentad cut
     * at the catalogue's first instant */
    out->first = (uint8_t)(first - sync_first);
    /* the catalogue's first and last Pentads are cut at its edges */
    out->start = (first < 0) ? frc_catalogue_first(cat) : time_at(cat, (uint64_t)first * 4);
    out->end = ((uint64_t)last * 4 > grid->extent) ? frc_catalogue_last(cat)
                                                   : time_at(cat, (uint64_t)last * 4);
    return FRC_OK;
}

frc_status_t frc_pentad_at(const frc_catalogue_t *cat, frc_time_t t, frc_pentad_t *out)
{
    sync_grid_t grid;
    frc_i_fraction_t f;
    frc_status_t status;
    int64_t helion, number;
    uint8_t within, index;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = sync_grid(cat, &grid);
    if (status != FRC_OK) {
        return status;
    }
    status = frc_i_fraction_at(cat, t, FRC_SYNC_LEVEL, &f);
    if (status != FRC_OK) {
        return status;
    }
    helion = (int64_t)f.cycle - grid.anchor_cycle; /* Helions since the anchor's */
    number = frc_i_floor_div(helion, FRC_SYNC_CYCLES);
    within = (uint8_t)(helion - FRC_SYNC_CYCLES * number); /* 0..18 */
    index = (uint8_t)(within / FRC_I_PENTAD_HELIONS);
    if (index > FRC_I_HYPOPENTAD) {
        index = FRC_I_HYPOPENTAD; /* cannot happen for 19 = 5 + 5 + 5 + 4; kept as S7.5 writes it */
    }
    /* the running Helion is inside the catalogue, so its Pentad is in range */
    status = fill_pentad(cat, &grid, number, index, out);
    if (status != FRC_OK) {
        return status;
    }
    out->helion = (uint8_t)(within - FRC_I_PENTAD_HELIONS * index + 1);
    return FRC_OK;
}

frc_status_t frc_pentad_by_number(const frc_catalogue_t *cat, int32_t sync, uint8_t index, frc_pentad_t *out)
{
    sync_grid_t grid;
    frc_status_t status;

    if (out == NULL || index > FRC_I_HYPOPENTAD) {
        return FRC_ERR_ARG;
    }
    status = sync_grid(cat, &grid);
    if (status != FRC_OK) {
        return status;
    }
    return fill_pentad(cat, &grid, sync, index, out);
}
