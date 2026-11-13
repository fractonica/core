/*
 * test_arc.c - arc geometry and the library's own sine and cosine: src/frc_arc.c.
 *
 * Held against
 *   - libm (as doubles), for frc_sin / frc_cos over 300,000 arguments: dense near 0,
 *     uniform over many turns, every float exponent with random mantissas, the multiples
 *     of pi/4 and their neighbours, the largest and smallest floats, both signs
 *   - tests/vectors/oracle_geometry.h, the reference's clock.arc for 115 (level, index)
 *     pairs, with and without relativeTo
 *   - the geometry the header promises: both ends of every child lie on the carrier at
 *     the ends of its slot, consecutive children share an end, the child meets the carrier
 *     at right angles, odd and even children bulge to opposite sides
 * plus invalid input. The whole API is optional: with FRC_NO_FLOAT there is nothing to test.
 */
#include "frc_test.h"
#include "fractonica.h"

#ifdef FRC_NO_FLOAT

int main(void)
{
    return TEST_SUMMARY("arc (FRC_NO_FLOAT: no arc API)");
}

#else

#include <float.h>
#include <math.h>

#include "vectors/oracle_geometry.h"

#define PI_D 3.14159265358979323846

/* ------------------------------------------------------------------------- */
/* frc_sin, frc_cos                                                           */
/* ------------------------------------------------------------------------- */

static double worst_sin, worst_cos;
static float worst_sin_at, worst_cos_at;

/* Hold both functions against libm at x; the truth is the double function of the float's
 * exact value. The header promises |error| < 1e-6. */
static void check_trig(float x)
{
    double es = fabs((double)frc_sin(x) - sin((double)x));
    double ec = fabs((double)frc_cos(x) - cos((double)x));
    if (es > worst_sin) {
        worst_sin = es;
        worst_sin_at = x;
    }
    if (ec > worst_cos) {
        worst_cos = ec;
        worst_cos_at = x;
    }
    CHECK(es < 1e-6);
    CHECK_NOTE("frc_sin(%.9g) = %.9g, libm %.9g", (double)x, (double)frc_sin(x), sin((double)x));
    CHECK(ec < 1e-6);
    CHECK_NOTE("frc_cos(%.9g) = %.9g, libm %.9g", (double)x, (double)frc_cos(x), cos((double)x));
}

/* xorshift32: deterministic, so a failure can be reproduced */
static uint32_t rng_state = 2463534242u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static float rng_unit(void) /* [0, 1) */
{
    return (float)(rng() >> 8) * (1.0f / 16777216.0f);
}

static float float_of_bits(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static void test_trig(void)
{
    int i, k;

    TEST_SECTION("sin and cos: exact cases");
    CHECK(frc_sin(0.0f) == 0.0f);
    CHECK(frc_cos(0.0f) == 1.0f);
    CHECK(frc_sin(-0.0f) == 0.0f);
    CHECK(frc_cos(-0.0f) == 1.0f);
    /* a tiny angle is its own sine, not 0: the fast path does not go through the reduction */
    CHECK(frc_sin(1e-20f) == 1e-20f);
    CHECK(frc_sin(-1e-20f) == -1e-20f);
    CHECK(frc_sin(FLT_MIN) == FLT_MIN);
    CHECK(frc_cos(1e-20f) == 1.0f);

    TEST_SECTION("sin and cos against libm");
    /* 100,000 angles within a few turns, where the arc geometry lives */
    for (i = 0; i < 100000; i++) {
        check_trig((rng_unit() - 0.5f) * 40.0f);
    }
    /* 50,000 over thousands of turns */
    for (i = 0; i < 50000; i++) {
        check_trig((rng_unit() - 0.5f) * 20000.0f);
    }
    /* 20,000 near zero and around the pi/4 switch of the polynomials */
    for (i = 0; i < 10000; i++) {
        check_trig((rng_unit() - 0.5f) * 0.01f);
        check_trig((float)(PI_D / 4) + (rng_unit() - 0.5f) * 0.001f);
    }
    /* 130,000 random bit patterns: every exponent from denormals to 3.4e38, both signs.
     * Huge arguments need the full 1 / (2 pi) table: 1e30 is 10^29 turns. */
    for (i = 0; i < 130000; i++) {
        float x = float_of_bits(rng());
        if (isfinite(x)) {
            check_trig(x);
        }
    }
    /* every binary exponent explicitly, smallest and largest mantissa */
    for (k = 0; k < 255; k++) {
        check_trig(float_of_bits((uint32_t)k << 23));
        check_trig(float_of_bits(((uint32_t)k << 23) | 0x7FFFFFu));
        check_trig(float_of_bits(0x80000000u | ((uint32_t)k << 23) | 0x2AAAAAu));
    }
    /* multiples of pi/4 (where the quadrant changes hands) and the floats beside them */
    for (k = -4000; k <= 4000; k++) {
        float x = (float)(k * (PI_D / 4));
        check_trig(x);
        check_trig(nextafterf(x, 1e9f));
        check_trig(nextafterf(x, -1e9f));
    }
    check_trig(FLT_MAX);
    check_trig(-FLT_MAX);
    check_trig(FLT_MIN);
    check_trig(float_of_bits(1)); /* the smallest denormal */
    check_trig(1e30f);
    check_trig(-1e30f);
    check_trig(1e38f);
    check_trig(16777216.0f);
    check_trig(FRC_PI);
    check_trig(-FRC_PI);
    check_trig(2.0f * FRC_PI);
    printf("     sin/cos: worst error against libm %.3g at %.9g (sin), %.3g at %.9g (cos)\n", worst_sin,
           (double)worst_sin_at, worst_cos, (double)worst_cos_at);
    /* far inside the promise: the worst case is about 1.0e-7 (frc_arc.c says so) */
    CHECK(worst_sin < 2e-7 && worst_cos < 2e-7);

    TEST_SECTION("sin and cos: symmetry and the circle");
    for (i = 0; i < 20000; i++) {
        float x = (i % 2) ? (rng_unit() - 0.5f) * 100.0f : float_of_bits(rng() & 0x7FFFFFFFu);
        float s, c;
        if (!isfinite(x)) {
            continue;
        }
        s = frc_sin(x);
        c = frc_cos(x);
        CHECK(frc_sin(-x) == -s); /* odd, exactly */
        CHECK(frc_cos(-x) == c);  /* even, exactly */
        CHECK(fabs((double)s * s + (double)c * c - 1.0) < 2e-6);
        CHECK(s >= -1.000001f && s <= 1.000001f && c >= -1.000001f && c <= 1.000001f);
    }

    TEST_SECTION("sin and cos: not a number");
    {
        float values[3];
        values[0] = INFINITY;
        values[1] = -INFINITY;
        values[2] = NAN;
        for (i = 0; i < 3; i++) {
            CHECK(isnan(frc_sin(values[i])));
            CHECK(isnan(frc_cos(values[i])));
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Arcs                                                                       */
/* ------------------------------------------------------------------------- */

static frc_arc_t make_arc(float cx, float cy, float radius, float start, float sweep)
{
    frc_arc_t arc;
    arc.cx = cx;
    arc.cy = cy;
    arc.radius = radius;
    arc.start = start;
    arc.sweep = sweep;
    return arc;
}

/* The point at fraction t of an arc, in double with libm: the truth for frc_arc_point. */
static void true_point(const frc_arc_t *arc, double t, double *x, double *y)
{
    double angle = (double)arc->start + (double)arc->sweep * t;
    *x = (double)arc->cx + (double)arc->radius * cos(angle);
    *y = (double)arc->cy + (double)arc->radius * sin(angle);
}

static double distance(double x0, double y0, double x1, double y1)
{
    return hypot(x1 - x0, y1 - y0);
}

static void test_wave_root(void)
{
    static const uint32_t HALVES[] = {0, 1, 2, 3, 11, 34, 137};
    size_t i;

    TEST_SECTION("wave root");
    for (i = 0; i < sizeof HALVES / sizeof HALVES[0]; i++) {
        uint32_t half = HALVES[i];
        frc_arc_t arc = frc_arc_wave_root(half);
        float x, y;
        /* "a unit semicircle centred at (2*half + 1, 0), alternating above / below" */
        CHECK(arc.cx == (float)(2 * half + 1) && arc.cy == 0.0f && arc.radius == 1.0f);
        CHECK(arc.start == FRC_PI);
        CHECK(arc.sweep == ((half & 1) ? -FRC_PI : FRC_PI));
        /* it runs from x = 2*half to 2*half + 2 on the axis: roots join end to end */
        frc_arc_point(&arc, 0.0f, &x, &y);
        CHECK(fabs((double)x - 2.0 * half) < 1e-5 && fabs((double)y) < 1e-5);
        frc_arc_point(&arc, 1.0f, &x, &y);
        CHECK(fabs((double)x - (2.0 * half + 2.0)) < 1e-5 && fabs((double)y) < 1e-5);
        /* y is down: an even half bulges up the screen (y = -1), an odd one down */
        frc_arc_point(&arc, 0.5f, &x, &y);
        CHECK(fabs((double)x - (2.0 * half + 1.0)) < 1e-5);
        CHECK(fabs((double)y - ((half & 1) ? 1.0 : -1.0)) < 1e-5);
    }
}

static void test_arc_point(void)
{
    frc_arc_t arc = make_arc(3.0f, -2.0f, 5.0f, 0.25f, -4.0f);
    float x = 99.0f, y = 99.0f;
    int i;

    TEST_SECTION("arc point");
    for (i = 0; i <= 20; i++) {
        double tx, ty, t = i / 20.0;
        frc_arc_point(&arc, (float)t, &x, &y);
        true_point(&arc, (double)(float)t, &tx, &ty);
        CHECK(fabs((double)x - tx) < 2e-5 && fabs((double)y - ty) < 2e-5);
        /* every point is on the circle */
        CHECK(fabs(distance(x, y, 3.0, -2.0) - 5.0) < 2e-5);
    }
    /* either coordinate may be left out; no arc, no write */
    x = 99.0f;
    y = 99.0f;
    frc_arc_point(&arc, 0.0f, &x, NULL);
    CHECK(fabs((double)x - (3.0 + 5.0 * cos(0.25))) < 2e-5);
    frc_arc_point(&arc, 0.0f, NULL, &y);
    CHECK(fabs((double)y - (-2.0 + 5.0 * sin(0.25))) < 2e-5);
    frc_arc_point(&arc, 0.0f, NULL, NULL);
    x = 99.0f;
    y = 99.0f;
    frc_arc_point(NULL, 0.5f, &x, &y);
    CHECK(x == 99.0f && y == 99.0f);
}

/* Everything the header promises about the m children of one carrier. `tolerance` is
 * relative to the carrier's radius (the header's "within 1e-4 relative"). */
static void check_children(const frc_arc_t *parent, uint8_t m, double tolerance)
{
    double scale = (double)parent->radius, eps = tolerance * scale;
    double delta = (double)parent->sweep / m, half = fabs(delta) / 2;
    uint8_t j;
    int odd;

    for (odd = 0; odd <= 1; odd++) {
        float previous_x = 0.0f, previous_y = 0.0f;
        for (j = 0; j < m; j++) {
            frc_arc_t child = frc_arc_child(parent, m, j, odd);
            float sx, sy, ex, ey;
            double ax, ay, bx, by, centre;

            /* both ends lie on the carrier, at the ends of slot j */
            frc_arc_point(&child, 0.0f, &sx, &sy);
            frc_arc_point(&child, 1.0f, &ex, &ey);
            true_point(parent, (double)j / m, &ax, &ay);
            true_point(parent, (double)(j + 1) / m, &bx, &by);
            CHECK(distance(sx, sy, ax, ay) < eps);
            CHECK(distance(ex, ey, bx, by) < eps);
            CHECK_NOTE("m %d child %d odd %d: end (%g, %g), slot end (%g, %g), radius %g", m, j, odd, (double)ex,
                       (double)ey, bx, by, scale);
            /* consecutive children share an end point */
            if (j > 0) {
                CHECK(distance(sx, sy, previous_x, previous_y) < eps);
            }
            previous_x = ex;
            previous_y = ey;

            /* radius R tan(half), centre at R / cos(half): the circle through the slot's
             * ends that meets the carrier at right angles (d^2 = R^2 + r^2) */
            centre = distance(child.cx, child.cy, parent->cx, parent->cy);
            CHECK(fabs((double)child.radius - scale * tan(half)) < eps * (1.0 + tan(half)));
            CHECK(fabs(centre - scale / cos(half)) < eps * (1.0 + 1.0 / cos(half)));
            CHECK(fabs(centre * centre - scale * scale - (double)child.radius * child.radius) <
                  4 * eps * centre);
            /* the sweep is the slot's angle plus or minus half a turn */
            CHECK(fabs((double)child.sweep - (delta + (odd ? -PI_D : PI_D))) < 1e-5);
            CHECK(child.radius > 0.0f);
        }
    }
    /* odd and even children of the same slot bulge to opposite sides of the carrier */
    for (j = 0; j < m; j++) {
        frc_arc_t even = frc_arc_child(parent, m, j, 0), odd_child = frc_arc_child(parent, m, j, 1);
        float x0, y0, x1, y1;
        double d0, d1;
        frc_arc_point(&even, 0.5f, &x0, &y0);
        frc_arc_point(&odd_child, 0.5f, &x1, &y1);
        d0 = distance(x0, y0, parent->cx, parent->cy) - scale;
        d1 = distance(x1, y1, parent->cx, parent->cy) - scale;
        CHECK(d0 * d1 < 0.0);
        /* same circle, the two complementary arcs of it */
        CHECK(even.cx == odd_child.cx && even.cy == odd_child.cy && even.radius == odd_child.radius &&
              even.start == odd_child.start);
        CHECK(fabs(fabs((double)even.sweep - (double)odd_child.sweep) - 2 * PI_D) < 1e-5);
    }
}

static void test_children(void)
{
    static const uint8_t SEQUENCE[FRC_SEQUENCE_LENGTH] = {5, 13, 5, 5, 7, 11, 9, 4};
    frc_arc_t carrier;
    uint8_t m;
    int i;

    TEST_SECTION("children of the wave roots");
    for (i = 0; i < 4; i++) {
        carrier = frc_arc_wave_root((uint32_t)i);
        for (m = 2; m <= 40; m++) { /* a semicircle in 2 or more slots */
            check_children(&carrier, m, 1e-4);
        }
    }

    TEST_SECTION("children of a ring");
    /* "use a full-circle carrier (sweep = 2 pi) and give it 2 * frc_factor(level) children" */
    for (i = 0; i < FRC_SEQUENCE_LENGTH; i++) {
        CHECK_EQ_U64(frc_factor((uint8_t)i), SEQUENCE[i]);
        carrier = make_arc(0.0f, 0.0f, 1.0f, 0.0f, 2.0f * FRC_PI);
        check_children(&carrier, (uint8_t)(2 * SEQUENCE[i]), 1e-4);
        carrier = make_arc(10.0f, -3.0f, 250.0f, 1.0f, -2.0f * FRC_PI); /* counter-clockwise, in pixels */
        check_children(&carrier, (uint8_t)(2 * SEQUENCE[i]), 1e-4);
    }
    /* "so m >= 3 on a full-circle carrier" */
    carrier = make_arc(0.0f, 0.0f, 1.0f, 0.0f, 2.0f * FRC_PI);
    check_children(&carrier, 3, 1e-4);

    TEST_SECTION("children of arbitrary carriers");
    for (i = 0; i < 400; i++) {
        float sweep = (0.05f + rng_unit() * 6.2f) * ((i & 1) ? -1.0f : 1.0f);
        carrier = make_arc((rng_unit() - 0.5f) * 100.0f, (rng_unit() - 0.5f) * 100.0f, 0.01f + rng_unit() * 300.0f,
                           (rng_unit() - 0.5f) * 20.0f, sweep);
        m = SEQUENCE[i % FRC_SEQUENCE_LENGTH];
        if (fabs((double)sweep) / m < 3.0) { /* keep clear of the semicircle limit */
            /* the centre is up to 50 away and floats carry 7 digits: allow for it */
            check_children(&carrier, m, 1e-4 + 2e-5 * 100.0 / (double)carrier.radius);
        }
    }

    TEST_SECTION("children of children: six levels down");
    {
        /* each arc is the carrier of the next; the promise holds at every depth, relative
         * to that depth's own carrier */
        uint32_t half;
        for (half = 0; half < 2; half++) {
            uint8_t level;
            uint64_t index = half;
            carrier = frc_arc_wave_root(half);
            for (level = 0; level < 6; level++) {
                uint8_t factor = frc_factor(level), local = (uint8_t)((level * 3 + 1) % factor);
                check_children(&carrier, factor, 2e-4);
                index = index * factor + local;
                carrier = frc_arc_child(&carrier, factor, local, (int)(index & 1));
            }
        }
    }

    TEST_SECTION("child: degenerate input");
    {
        frc_arc_t child;
        carrier = make_arc(1.0f, 2.0f, 3.0f, 0.5f, 2.0f);
        /* an undivided carrier is its own child (the reference does the same) */
        child = frc_arc_child(&carrier, 1, 0, 0);
        CHECK(child.cx == 1.0f && child.cy == 2.0f && child.radius == 3.0f && child.start == 0.5f &&
              child.sweep == 2.0f);
        /* nothing to divide, nothing to stand on: an empty arc, never a wild pointer or NaN */
        child = frc_arc_child(&carrier, 0, 0, 0);
        CHECK(child.cx == 0.0f && child.cy == 0.0f && child.radius == 0.0f && child.start == 0.0f &&
              child.sweep == 0.0f);
        child = frc_arc_child(NULL, 5, 0, 0);
        CHECK(child.radius == 0.0f && child.sweep == 0.0f);
        /* "Requires |parent.sweep| / m < pi": a slot of half a turn has no such circle */
        carrier = make_arc(0.0f, 0.0f, 1.0f, 0.0f, 2.0f * FRC_PI);
        child = frc_arc_child(&carrier, 2, 0, 0);
        CHECK(child.radius == 0.0f && child.cx == 0.0f && child.cy == 0.0f);
        carrier.sweep = -2.0f * FRC_PI;
        child = frc_arc_child(&carrier, 2, 1, 1);
        CHECK(child.radius == 0.0f);
        carrier.sweep = NAN;
        child = frc_arc_child(&carrier, 5, 0, 0);
        CHECK(child.radius == 0.0f && child.sweep == 0.0f);
        /* a carrier that does not move has children that do not either: half circles of
         * radius 0 around its start point */
        carrier = make_arc(0.0f, 0.0f, 2.0f, 0.0f, 0.0f);
        child = frc_arc_child(&carrier, 5, 2, 0);
        CHECK(child.radius == 0.0f && fabs((double)child.cx - 2.0) < 1e-6 && fabs((double)child.cy) < 1e-6);
    }
}

/* The difference of two angles, folded into [-pi, pi]. */
static double angle_difference(double a, double b)
{
    double d = fmod(a - b, 2 * PI_D);
    if (d > PI_D) {
        d -= 2 * PI_D;
    }
    if (d < -PI_D) {
        d += 2 * PI_D;
    }
    return d;
}

static void test_against_reference(void)
{
    int i;

    /* clock.arc(level, index, {root: 'wave', relativeTo}) rebuilt from the public API:
     * the wave root of the top ancestor, then one frc_arc_child per level. */
    TEST_SECTION("arcs against the reference");
    for (i = 0; i < ORACLE_GEOMETRY_ARC_COUNT; i++) {
        const oracle_geometry_arc_t *v = &ORACLE_GEOMETRY_ARC[i];
        uint64_t index[FRC_LEVEL_COUNT];
        frc_arc_t arc;
        double centre_tolerance, radius_tolerance;
        int level;

        /* the half-cycle indices of the ancestors: parent = child / factor(parent level) */
        index[v->level] = v->index;
        for (level = v->level - 1; level >= 0; level--) {
            index[level] = index[level + 1] / frc_factor((uint8_t)level);
        }
        arc = frc_arc_wave_root((uint32_t)index[0]);
        for (level = 0; level <= v->level; level++) {
            if (v->rel_level == level) {
                /* relativeTo: this ancestor becomes the unit circle at the origin */
                CHECK_EQ_U64(index[level], v->rel_index);
                arc.cx = 0.0f;
                arc.cy = 0.0f;
                arc.radius = 1.0f;
            }
            if (level < v->level) {
                uint8_t factor = frc_factor((uint8_t)level);
                arc = frc_arc_child(&arc, factor, (uint8_t)(index[level + 1] % factor), (int)(index[level + 1] & 1));
            }
        }
        /* float against double (measured: 9e-7, 4e-7, 2.4e-6 rad, 3e-7 rad). Not rebased,
         * a deep arc is far smaller than the float spacing of its position (the
         * reference's advice for that is relativeTo), so the centre is held to that
         * spacing, not to the radius. */
        centre_tolerance = 5e-6 * (1.0 + fabs(v->cx) + fabs(v->cy));
        radius_tolerance = 5e-6 * v->radius;
        CHECK(fabs((double)arc.cx - v->cx) < centre_tolerance);
        CHECK(fabs((double)arc.cy - v->cy) < centre_tolerance);
        CHECK(fabs((double)arc.radius - v->radius) < radius_tolerance);
        CHECK(fabs(angle_difference((double)arc.start, v->start)) < 2e-5);
        CHECK(fabs((double)arc.sweep - v->sweep) < 2e-6);
        CHECK_NOTE("arc %d: level %d index %" PRIu64 " rel %d: got (%.9g, %.9g) r %.9g start %.9g sweep %.9g", i,
                   v->level, v->index, v->rel_level, (double)arc.cx, (double)arc.cy, (double)arc.radius,
                   (double)arc.start, (double)arc.sweep);
    }
}

int main(void)
{
    test_trig();
    test_wave_root();
    test_arc_point();
    test_children();
    test_against_reference();
    return TEST_SUMMARY("arc");
}

#endif /* FRC_NO_FLOAT */
