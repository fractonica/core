/*
 * frc_glyph.c - phase glyphs: the states of a cycle and how they are drawn.
 *
 * Implements SPEC S4.1 (states: index, value, derivative, dot), S4.2 (balanced
 * ternary trits), S9.1 (the path of a glyph in design units) and the pitfall-5
 * rule that the dot marks the falling branch only; with S6.2, the state each
 * threshold shows (frc_glyph_at_quarter).
 *
 * THE MODEL
 *   A glyph of d strokes shows a value in -M .. +M, M = (3^d - 1) / 2. One
 *   cycle walks 0 -> +M -> 0 -> -M -> 0 in 4M states, so each QUARTER of the
 *   cycle has M states. The state of a level at an instant is therefore the
 *   cycle fraction of frc_internal.h (section 5) on a grid of M states per
 *   quarter: index = q4 * M + floor(rem * M / den). The whole position is
 *   never multiplied by P * M.
 *
 * INVARIANTS
 *   - strokes is 1 .. FRC_MAX_STROKES everywhere; anything else is
 *     FRC_ERR_ARG (or 0 from the functions that return a count).
 *   - trit[0] is the LEAST significant trit and is drawn at the top; unused
 *     trits are 0.
 *   - dotted == (derivative < 0). Peak and valley are bare.
 *   - Geometry is integer, in hundredths of a design unit, y down. The path
 *     has ONE source (emit_path); frc_glyph_segments flattens that same path,
 *     so the two can never disagree.
 *   - The stroke of trit -1 is the stroke of trit +1 turned half a turn about
 *     its junction on the stem, exactly, also after flattening.
 */
#include "frc_internal.h"

/* S9.1 in coordinate units (FRC_GLYPH_UNIT per design unit). */
#define STEM_STEP (3 * FRC_GLYPH_UNIT) /* stem per trit                          */
#define BAR_HALF  (1 * FRC_GLYPH_UNIT) /* the zero bar is 2 wide                 */
#define REACH     (2 * FRC_GLYPH_UNIT) /* how far a curved stroke goes sideways  */

/* M of the widest glyph, (3^FRC_MAX_STROKES - 1) / 2: five ones in base 3. */
#define MAX_EXTREME 121
/* Fails to compile if FRC_MAX_STROKES changes without MAX_EXTREME. */
typedef char frc_i_max_extreme_matches_max_strokes[(FRC_MAX_STROKES == 5) ? 1 : -1];

/* ------------------------------------------------------------------------- */
/* S4.1  States                                                               */
/* ------------------------------------------------------------------------- */

uint16_t frc_glyph_extreme(uint8_t strokes)
{
    uint16_t m = 0;
    uint8_t i;

    if (strokes == 0 || strokes > FRC_MAX_STROKES) {
        return 0;
    }
    /* (3^d - 1) / 2 is d ones in base 3: 1, 4, 13, 40, 121 */
    for (i = 0; i < strokes; i++) {
        m = (uint16_t)(3 * m + 1);
    }
    return m;
}

uint16_t frc_glyph_states(uint8_t strokes)
{
    return (uint16_t)(4 * frc_glyph_extreme(strokes)); /* at most 484 */
}

frc_status_t frc_glyph_from_index(uint16_t index, uint8_t strokes, frc_glyph_t *out)
{
    uint16_t m = frc_glyph_extreme(strokes);
    uint16_t k, shifted;
    int16_t value;
    uint8_t i;

    if (m == 0 || out == NULL) {
        return FRC_ERR_ARG;
    }
    k = (uint16_t)(index % (4 * m));

    /* The table of S4.1: up to the peak, down through the node to the
     * valley, and up again. All values are within +-121. */
    if (k <= m) {
        value = (int16_t)k;
    } else if (k <= 3 * m) {
        value = (int16_t)(2 * m - k);
    } else {
        value = (int16_t)(k - 4 * m);
    }
    out->index = k;
    out->value = value;
    out->strokes = strokes;
    if (k == m || k == 3 * m) {
        out->derivative = 0;
    } else {
        out->derivative = (int8_t)((k < m || k > 3 * m) ? 1 : -1);
    }
    out->dotted = (uint8_t)(out->derivative < 0);

    /* S4.2. value + W = sum of (t_i + 1) * 3^i over ALL FRC_MAX_STROKES
     * places, because W = MAX_EXTREME is that many ones in base 3. So the
     * ordinary base-3 digits of value + W, each minus one, are the balanced
     * trits, least significant first. The places a glyph of fewer strokes
     * does not use come out 0 by themselves (|value| <= M needs only
     * `strokes` trits, and the representation is unique), so the loop has no
     * "else 0" branch: gcc -O3 splits such a loop and calls memset. */
    shifted = (uint16_t)(value + (int16_t)MAX_EXTREME);
    for (i = 0; i < FRC_MAX_STROKES; i++) {
        out->trit[i] = (int8_t)((int8_t)(shifted % 3) - 1);
        shifted = (uint16_t)(shifted / 3);
    }
    return FRC_OK;
}

frc_status_t frc_glyph_at(const frc_catalogue_t *cat, frc_time_t t, uint8_t level,
                          uint8_t strokes, frc_glyph_t *out)
{
    frc_i_fraction_t f;
    frc_status_t status;
    uint16_t m = frc_glyph_extreme(strokes);

    if (m == 0 || out == NULL) {
        return FRC_ERR_ARG;
    }
    status = frc_i_fraction_at(cat, t, level, &f);
    if (status != FRC_OK) {
        return status;
    }
    /* floor(position * P * M) mod 4M: the fraction on M states per quarter.
     * The step is below 4M <= 484. */
    return frc_glyph_from_index((uint16_t)frc_i_fraction_step(&f, m), strokes, out);
}

uint16_t frc_glyph_index_descending(uint16_t index, uint8_t strokes)
{
    uint16_t m = frc_glyph_extreme(strokes);

    if (m == 0) {
        return 0;
    }
    /* half a cycle later: the descending family reaches its valley first.
     * Reduce the index first so the sum stays far below 2^16. */
    return (uint16_t)((index % (4 * m) + 2 * m) % (4 * m));
}

uint16_t frc_glyph_index_of_phase(frc_frac_t phase, uint8_t strokes)
{
    /* floor(phase / 2^32 * 4M); 2^32 * 484 fits 64 bits. 0 for bad strokes. */
    return (uint16_t)(((uint64_t)phase * frc_glyph_states(strokes)) >> 32);
}

frc_status_t frc_glyph_at_quarter(frc_quarter_t quarter, uint8_t strokes, frc_glyph_t *out)
{
    frc_frac_t phase;

    if (out == NULL || !FRC_I_BELOW(quarter, FRC_END + 1) || frc_glyph_states(strokes) == 0) {
        return FRC_ERR_ARG;
    }
    /* S6.2: quarter q is ENDED by the threshold at phase (q + 1) / 4: the
     * peak at 1/4, the node at 1/2, the valley at 3/4, and the End at 4/4,
     * which is phase 0 of the next cycle. */
    phase = (frc_frac_t)((((uint32_t)quarter + 1u) & 3u) << 30);
    return frc_glyph_from_index(frc_glyph_index_of_phase(phase, strokes), strokes, out);
}

/* ------------------------------------------------------------------------- */
/* S9.1  The path                                                             */
/* ------------------------------------------------------------------------- */

/*
 * The curved stroke of trit +1, relative to its junction (0, y) on the stem,
 * as two cubics that start where the previous one ended: control 1, control
 * 2, end point. SPEC: "M 0 y C 0 y 0 y+1 1 y+1 C 2 y+1 2 y 2 y" (down and
 * right). Trit -1 is the same numbers negated (up and left).
 */
#define CURVE_CUBICS 2
static const uint8_t CURVE[CURVE_CUBICS][6] FRC_PROGMEM = {
    {0, 0, 0, FRC_GLYPH_UNIT, FRC_GLYPH_UNIT, FRC_GLYPH_UNIT},
    {REACH, FRC_GLYPH_UNIT, REACH, 0, REACH, 0}
};

static void emit_point(frc_path_sink_t sink, void *ctx, frc_path_op_t op, int16_t x, int16_t y)
{
    int16_t pts[2];

    pts[0] = x;
    pts[1] = y;
    sink(ctx, op, pts);
}

/* The stroke of one trit, attached to the stem at (0, y). */
static void emit_stroke(frc_path_sink_t sink, void *ctx, int8_t trit, int16_t y)
{
    int16_t pts[6];
    uint8_t cubic, i;

    if (trit == 0) {
        emit_point(sink, ctx, FRC_PATH_MOVE, (int16_t)-BAR_HALF, y);
        emit_point(sink, ctx, FRC_PATH_LINE, (int16_t)BAR_HALF, y);
        return;
    }
    emit_point(sink, ctx, FRC_PATH_MOVE, 0, y);
    for (cubic = 0; cubic < CURVE_CUBICS; cubic++) {
        /* coordinates stay within +-200 of a junction at 0..1200 */
        for (i = 0; i < 6; i = (uint8_t)(i + 2)) {
            pts[i] = (int16_t)(trit * FRC_PGM_U8(&CURVE[cubic][i]));
            pts[i + 1] = (int16_t)(y + trit * FRC_PGM_U8(&CURVE[cubic][i + 1]));
        }
        sink(ctx, FRC_PATH_CUBIC, pts);
    }
}

/* The one description of a glyph's ink: stem, strokes, dot, in that order. */
static void emit_path(const frc_glyph_t *glyph, frc_path_sink_t sink, void *ctx)
{
    int16_t dot[3];
    uint8_t i;

    for (i = 0; i < glyph->strokes; i++) {
        emit_point(sink, ctx, FRC_PATH_MOVE, 0, (int16_t)(i * STEM_STEP));
        emit_point(sink, ctx, FRC_PATH_LINE, 0, (int16_t)((i + 1) * STEM_STEP));
    }
    for (i = 0; i < glyph->strokes; i++) {
        emit_stroke(sink, ctx, glyph->trit[i], (int16_t)(i * STEM_STEP));
    }
    if (glyph->dotted) {
        dot[0] = 0;
        dot[1] = FRC_GLYPH_DOT_Y;
        dot[2] = FRC_GLYPH_DOT_RADIUS;
        sink(ctx, FRC_PATH_DOT, dot);
    }
}

/* 1 when the glyph can be drawn: a stroke count the library knows and trits
 * that are trits (a hand-filled struct must not send the pen off the page). */
static int drawable(const frc_glyph_t *glyph)
{
    uint8_t i;

    if (glyph == NULL || glyph->strokes == 0 || glyph->strokes > FRC_MAX_STROKES) {
        return 0;
    }
    for (i = 0; i < glyph->strokes; i++) {
        if (glyph->trit[i] < -1 || glyph->trit[i] > 1) {
            return 0;
        }
    }
    return 1;
}

void frc_glyph_path(const frc_glyph_t *glyph, frc_path_sink_t sink, void *ctx)
{
    if (sink == NULL || !drawable(glyph)) {
        return;
    }
    emit_path(glyph, sink, ctx);
}

void frc_glyph_bounds(uint8_t strokes, int16_t *min_x, int16_t *min_y, int16_t *max_x, int16_t *max_y)
{
    /* The skeleton spans x = +-REACH and y from the dot's centre line down to
     * the foot of the stem; round caps add half the ink all around, and the
     * dot its radius above its centre (SPEC S9.1: -2.15..2.15, -2.2..3d+0.15).
     * An invalid stroke count has no glyph: all four are 0. */
    int valid = frc_glyph_extreme(strokes) != 0;
    int16_t side = (int16_t)(valid ? REACH + FRC_GLYPH_INK / 2 : 0);
    int16_t top = (int16_t)(valid ? FRC_GLYPH_DOT_Y - FRC_GLYPH_DOT_RADIUS - FRC_GLYPH_INK / 2 : 0);
    int16_t foot = (int16_t)(valid ? strokes * STEM_STEP + FRC_GLYPH_INK / 2 : 0);

    if (min_x != NULL) {
        *min_x = (int16_t)-side;
    }
    if (max_x != NULL) {
        *max_x = side;
    }
    if (min_y != NULL) {
        *min_y = top;
    }
    if (max_y != NULL) {
        *max_y = foot;
    }
}

/* ------------------------------------------------------------------------- */
/* S9.1  The same path as straight segments                                   */
/* ------------------------------------------------------------------------- */

#define MAX_CURVE_STEPS 32

/* The pen of frc_glyph_segments: a path sink that forwards straight pieces. */
typedef struct flattener {
    frc_segment_sink_t sink;
    void *ctx;
    int16_t x, y;  /* where the pen is */
    uint8_t steps; /* segments per cubic, 1..MAX_CURVE_STEPS */
} flattener_t;

static void line_to(flattener_t *pen, int16_t x, int16_t y)
{
    pen->sink(pen->ctx, pen->x, pen->y, x, y);
    pen->x = x;
    pen->y = y;
}

/* n / d rounded to nearest, halves away from zero (d > 0). Symmetric in the
 * sign of n, which is what keeps the -1 stroke an exact mirror of the +1. */
static int32_t round_div(int32_t n, int32_t d)
{
    int32_t q = ((n < 0 ? -n : n) * 2 + d) / (2 * d);
    return n < 0 ? -q : q;
}

/*
 * One coordinate of a cubic at t = j / n, measured from its start point:
 *     B(t) - p0 = 3 (1-t)^2 t d1 + 3 (1-t) t^2 d2 + t^3 d3,   d_i = p_i - p0
 * With u = n - j the numerator over n^3 is 3 u^2 j d1 + 3 u j^2 d2 + j^3 d3.
 * The three weights sum to less than n^3 <= 32768 and |d_i| <= 200 here, so
 * the numerator stays below 2^23: no overflow in 32 bits.
 */
static int16_t cubic_offset(int32_t d1, int32_t d2, int32_t d3, int32_t j, int32_t n)
{
    int32_t u = n - j;
    int32_t numerator = 3 * u * u * j * d1 + 3 * u * j * j * d2 + j * j * j * d3;

    return (int16_t)round_div(numerator, n * n * n);
}

static void curve_to(flattener_t *pen, const int16_t *pts)
{
    int16_t x0 = pen->x, y0 = pen->y;
    uint8_t j;

    for (j = 1; j <= pen->steps; j++) {
        /* j == steps gives exactly the end point: the numerator is n^3 * d3 */
        line_to(pen,
                (int16_t)(x0 + cubic_offset(pts[0] - x0, pts[2] - x0, pts[4] - x0, j, pen->steps)),
                (int16_t)(y0 + cubic_offset(pts[1] - y0, pts[3] - y0, pts[5] - y0, j, pen->steps)));
    }
}

static void flatten_sink(void *ctx, frc_path_op_t op, const int16_t *pts)
{
    flattener_t *pen = (flattener_t *)ctx;

    if (op == FRC_PATH_MOVE) {
        pen->x = pts[0];
        pen->y = pts[1];
    } else if (op == FRC_PATH_LINE) {
        line_to(pen, pts[0], pts[1]);
    } else if (op == FRC_PATH_CUBIC) {
        curve_to(pen, pts);
    }
    /* FRC_PATH_DOT: not a segment; the caller fills the circle itself */
}

void frc_glyph_segments(const frc_glyph_t *glyph, uint8_t curve_steps, frc_segment_sink_t sink, void *ctx)
{
    flattener_t pen;

    if (sink == NULL || !drawable(glyph)) {
        return;
    }
    if (curve_steps < 1) {
        curve_steps = 1;
    }
    if (curve_steps > MAX_CURVE_STEPS) {
        curve_steps = MAX_CURVE_STEPS;
    }
    pen.sink = sink;
    pen.ctx = ctx;
    pen.x = 0;
    pen.y = 0;
    pen.steps = curve_steps;
    emit_path(glyph, flatten_sink, &pen);
}
