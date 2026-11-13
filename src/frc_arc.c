/*
 * frc_arc.c - arc geometry: nested alternating circular arcs, and the sine
 * and cosine they need. Optional: the whole file is inside #ifndef
 * FRC_NO_FLOAT, and no reading of the clock depends on it.
 *
 * Implements the "Arc geometry" section of fractonica.h. It is a port of
 * `child()` and of the 'wave' root of `clock.arc` in the reference library
 * (reference/js/fractonica.js) from double to float; SPEC.md has no section
 * for it.
 *
 * THE MODEL
 *   A carrier arc is divided into m equal slots. The child in slot j is the
 *   circle through the two ends of the slot that crosses the carrier at
 *   right angles. With delta = sweep / m (the slot's angle on the carrier)
 *   and half = |delta| / 2:
 *
 *       centre  on the bisector of the slot, at R / cos(half) from the
 *               carrier's centre (R = carrier radius)
 *       radius  R * tan(half)
 *       start   bisector angle -+ (pi/2 + half): seen from the child's
 *               centre, the slot's first end
 *       sweep   delta +- pi: the short way round for one bulge, the long
 *               way for the other (`odd` chooses)
 *
 *   Both ends of every child lie on the carrier, and consecutive children
 *   share an end point, so the children of one carrier form one curve.
 *
 * INVARIANTS
 *   - float only (no double: AVR has none and Cortex-M would pull in the
 *     soft-double runtime), no libm, no static state.
 *   - frc_sin / frc_cos take ANY finite float and are within 1e-6 of the true
 *     value (worst case measured against libm in tests/test_arc.c: 1.0e-7).
 *     The argument is reduced with integers against 192 bits of 1 / (2 pi),
 *     so 1e30f is as accurate as 1. A non-finite argument gives NaN, as in
 *     libm.
 *   - float is IEEE-754 binary32, which holds on every target of this core.
 *   - Angles are radians, y points DOWN (screen coordinates), so a positive
 *     sweep is clockwise on screen.
 */
#include "frc_internal.h"

#ifndef FRC_NO_FLOAT

#define HALF_PI (FRC_PI / 2.0f)

/* ------------------------------------------------------------------------- */
/* Sine and cosine                                                            */
/* ------------------------------------------------------------------------- */

/*
 * 1 / (2 pi) = 0.28BE60DB 9391054A ... in hexadecimal: the first 192 bits
 * after the point, most significant word first. (Four times this is the 2/pi
 * table of every libm: A2F983 6E4E44 1529FC 2757D1 F534DD C0DB62.)
 */
#define TURN_WORDS 6
static const uint32_t INV_TWO_PI[TURN_WORDS] FRC_PROGMEM = {
    0x28BE60DBu, 0x9391054Au, 0x7F09D5F4u, 0x7D4D3770u, 0x36D8A566u, 0x4F10E410u
};

static uint32_t float_bits(float value)
{
    union {
        float f;
        uint32_t u;
    } pun;

    pun.f = value;
    return pun.u;
}

/* Bits offset .. offset+63 of 1 / (2 pi), counted from the first bit after
 * the point (offset 0). A negative offset reaches in front of the point,
 * where the bits are 0. REQUIRES -63 <= offset <= 32 * (TURN_WORDS - 3) + 31. */
static uint64_t turn_window(int offset)
{
    uint8_t word, shift;
    uint64_t window;

    if (offset < 0) {
        window = ((uint64_t)FRC_PGM_U32(&INV_TWO_PI[0]) << 32) | FRC_PGM_U32(&INV_TWO_PI[1]);
        return window >> (uint8_t)(-offset);
    }
    word = (uint8_t)(offset / 32);
    shift = (uint8_t)(offset % 32);
    window = ((uint64_t)FRC_PGM_U32(&INV_TWO_PI[word]) << 32) | FRC_PGM_U32(&INV_TWO_PI[word + 1]);
    if (shift != 0) {
        window = (window << shift) | (FRC_PGM_U32(&INV_TWO_PI[word + 2]) >> (32 - shift));
    }
    return window;
}

/*
 * Split a finite x > pi/4 into quarter turns and a rest: x = quadrant * pi/2
 * + rest (mod 2 pi) with |rest| <= pi/4. Returns the quadrant, 0..3.
 *
 * x = m * 2^e exactly, with m its 24-bit mantissa. In turns,
 *     x / (2 pi) = m * 2^e * sum of b_i * 2^-i        (b_i: bits of 1/(2 pi))
 * Bits with i <= e give whole turns and are dropped. The next 64 bits, as
 * the integer W, contribute m * W / 2^64 turns: its fraction is the low 64
 * bits of the product m * W, which is what unsigned multiplication keeps.
 * The bits after the window add less than m * 2^-64 < 2^-40 turn.
 */
static uint8_t reduce(float x, float *rest)
{
    uint32_t bits = float_bits(x);
    int exponent = (int)((bits >> 23) & 0xFFu) - 150; /* -24 .. 104 here */
    uint64_t mantissa = (bits & 0x7FFFFFu) | 0x800000u;
    uint64_t turns = mantissa * turn_window(exponent); /* 0.64 fixed point */
    uint64_t in_quarter;
    uint32_t magnitude;
    uint8_t quadrant;
    int negative;

    /* add an eighth of a turn, so the quadrant is the NEAREST multiple of
     * pi/2 and the rest is centred on it */
    turns += (uint64_t)1 << 61;
    quadrant = (uint8_t)(turns >> 62);
    in_quarter = turns & (((uint64_t)1 << 62) - 1); /* 0 .. 2^62 - 1, centre 2^61 */
    negative = in_quarter < ((uint64_t)1 << 61);
    /* distance from the centre, at most 2^61, in units of 2^-64 turn; keep
     * its top 31 bits (2^-34 turn each), more than a float holds */
    magnitude = (uint32_t)((negative ? ((uint64_t)1 << 61) - in_quarter
                                     : in_quarter - ((uint64_t)1 << 61)) >> 30);
    *rest = (float)magnitude * (2.0f * FRC_PI / 17179869184.0f); /* 2 pi / 2^34 */
    if (negative) {
        *rest = -*rest;
    }
    return quadrant;
}

/* Taylor polynomials on [-pi/4, pi/4]. The first dropped terms are
 * y^11 / 11! < 2e-9 and y^10 / 10! < 3e-8 there. */
static float sin_small(float y)
{
    float z = y * y;
    return y * (1.0f + z * (-1.0f / 6.0f + z * (1.0f / 120.0f + z * (-1.0f / 5040.0f + z * (1.0f / 362880.0f)))));
}

static float cos_small(float y)
{
    float z = y * y;
    return 1.0f + z * (-0.5f + z * (1.0f / 24.0f + z * (-1.0f / 720.0f + z * (1.0f / 40320.0f))));
}

/* sin(x + quarters * pi/2) for a finite x >= 0. */
static float sin_shifted(float x, uint8_t quarters)
{
    float rest = x;
    uint8_t quadrant = 0;

    if (x > FRC_PI / 4.0f) {
        quadrant = reduce(x, &rest);
    }
    quadrant = (uint8_t)((quadrant + quarters) & 3);
    /* sin(q * pi/2 + y): sin y, cos y, -sin y, -cos y */
    if (quadrant == 0) {
        return sin_small(rest);
    }
    if (quadrant == 1) {
        return cos_small(rest);
    }
    if (quadrant == 2) {
        return -sin_small(rest);
    }
    return -cos_small(rest);
}

static int is_finite(float x)
{
    return ((float_bits(x) >> 23) & 0xFFu) != 0xFFu;
}

float frc_sin(float radians)
{
    if (!is_finite(radians)) {
        return radians - radians; /* NaN */
    }
    /* odd: sin(-x) = -sin(x) */
    return radians < 0.0f ? -sin_shifted(-radians, 0) : sin_shifted(radians, 0);
}

float frc_cos(float radians)
{
    if (!is_finite(radians)) {
        return radians - radians; /* NaN */
    }
    /* even, and cos(x) = sin(x + pi/2) */
    return sin_shifted(radians < 0.0f ? -radians : radians, 1);
}

/* ------------------------------------------------------------------------- */
/* Arcs                                                                       */
/* ------------------------------------------------------------------------- */

/*
 * The geometry of THE MODEL above, for a carrier that can be divided:
 * REQUIRES m >= 2. Returns 0, and writes nothing, when a slot spans a
 * semicircle or more: no circle through its ends meets the carrier at right
 * angles then (the centre runs off to infinity). NaN ends up there too.
 */
static int divide(const frc_arc_t *parent, uint8_t m, uint8_t local, int odd, float out[5])
{
    float delta = parent->sweep / (float)m; /* the slot's signed angle on the carrier */
    float half = (delta < 0.0f ? -delta : delta) / 2.0f;
    float middle, reach, turn;

    if (!(half < HALF_PI)) {
        return 0;
    }
    middle = parent->start + ((float)local + 0.5f) * delta; /* the slot's bisector */
    reach = parent->radius / frc_cos(half);
    out[0] = parent->cx + reach * frc_cos(middle);
    out[1] = parent->cy + reach * frc_sin(middle);
    out[2] = parent->radius * (frc_sin(half) / frc_cos(half)); /* R tan(half) */
    /* Seen from the child's centre, the slot's first end lies a quarter turn
     * plus `half` back from the bisector, against the carrier's direction. */
    turn = HALF_PI + half;
    if (delta > 0.0f) {
        out[3] = middle - turn;
    } else if (delta < 0.0f) {
        out[3] = middle + turn;
    } else {
        out[3] = middle;
    }
    /* from that end to the other: the slot's own angle, plus or minus half a
     * turn for the side the arc bulges to */
    out[4] = delta + (odd ? -FRC_PI : FRC_PI);
    return 1;
}

frc_arc_t frc_arc_child(const frc_arc_t *parent, uint8_t m, uint8_t local, int odd)
{
    /*
     * The result is assembled in `v` (cx, cy, radius, start, sweep) and
     * returned as ONE compound literal at the single exit. Written the
     * obvious way (`return *parent;`, an all-zero struct for bad input, or a
     * local frc_arc_t that is filled and returned) a compiler emits calls to
     * memcpy / memset for the 20-byte struct, which the core must not need
     * (riscv32 clang -Os and xtensa gcc -O0 do; `make check-cross` catches it).
     */
    float v[5];
    int drawn = 0;

    if (parent != NULL && m == 1) {
        /* an undivided carrier is its own only child (as in the reference) */
        v[0] = parent->cx;
        v[1] = parent->cy;
        v[2] = parent->radius;
        v[3] = parent->start;
        v[4] = parent->sweep;
        drawn = 1;
    } else if (parent != NULL && m > 1) {
        drawn = divide(parent, m, local, odd, v);
    }
    /* nothing to stand on or nothing to draw: the empty arc, radius 0 */
    return (frc_arc_t){drawn ? v[0] : 0.0f, drawn ? v[1] : 0.0f, drawn ? v[2] : 0.0f,
                       drawn ? v[3] : 0.0f, drawn ? v[4] : 0.0f};
}

void frc_arc_point(const frc_arc_t *arc, float t, float *x, float *y)
{
    float angle;

    if (arc == NULL) {
        return;
    }
    angle = arc->start + arc->sweep * t;
    if (x != NULL) {
        *x = arc->cx + arc->radius * frc_cos(angle);
    }
    if (y != NULL) {
        *y = arc->cy + arc->radius * frc_sin(angle);
    }
}

frc_arc_t frc_arc_wave_root(uint32_t half)
{
    /* Unit semicircles side by side on the x axis, each starting at its left
     * end (angle pi) and bulging to alternate sides: centre (2 half + 1, 0),
     * radius 1, start pi, sweep +/- pi. (A compound literal, as in
     * frc_arc_child: a local struct that is returned costs a memcpy at -O0.) */
    return (frc_arc_t){2.0f * (float)half + 1.0f, 0.0f, 1.0f, FRC_PI, (half & 1u) ? -FRC_PI : FRC_PI};
}

#endif /* FRC_NO_FLOAT */
