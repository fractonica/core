/*
 * test_glyph.c - phase glyphs (SPEC S4), their geometry (S9.1) and the
 * odometer (S5): src/frc_glyph.c and src/frc_odometer.c.
 *
 * Held against
 *   - tests/vectors/oracle_clock.h       every sample x level: glyph index for 2, 3 and 4
 *                                        strokes, the decoded 2- and 3-stroke states, the
 *                                        2x2 odometer and its next change; the level-6
 *                                        odometers of 3x2 and 2x3; levels 14..21
 *   - tests/vectors/oracle_foundation.h  odometer steps on eight grids, up to 4 glyphs of
 *                                        5 strokes at level 21
 *   - tests/vectors/oracle_geometry.h    the reference's SVG path of all 232 states of
 *                                        1..4 strokes, the dot, the bounds
 *   - SPEC S4.1 (the state table, written out again here), S12 (vectors A, B, C)
 * plus what no oracle can give: invalid arguments, NULL pointers, the edges of the
 * catalogue, and the properties that define a "next change".
 */
#include "frc_test.h"
#include "fractonica.h"

#include "vectors/oracle_clock.h"
#include "vectors/oracle_foundation.h"
#include "vectors/oracle_geometry.h"

#define CAT (&FRC_SAROS_141)

/* ------------------------------------------------------------------------- */
/* S4.1, S4.2  States                                                         */
/* ------------------------------------------------------------------------- */

static void test_extreme_and_states(void)
{
    static const uint16_t EXTREME[6] = {0, 1, 4, 13, 40, 121}; /* (3^d - 1) / 2 */
    int d;

    TEST_SECTION("extreme and states");
    for (d = 1; d <= FRC_MAX_STROKES; d++) {
        CHECK_EQ_U64(frc_glyph_extreme((uint8_t)d), EXTREME[d]);
        CHECK_EQ_U64(frc_glyph_states((uint8_t)d), 4u * EXTREME[d]);
    }
    /* the header's own words: 2 strokes = 16 states, 3 = 52, 4 = 160 */
    CHECK_EQ_U64(frc_glyph_states(2), 16);
    CHECK_EQ_U64(frc_glyph_states(3), 52);
    CHECK_EQ_U64(frc_glyph_states(4), 160);
    CHECK_EQ_U64(frc_glyph_extreme(0), 0);
    CHECK_EQ_U64(frc_glyph_states(0), 0);
    CHECK_EQ_U64(frc_glyph_extreme(FRC_MAX_STROKES + 1), 0);
    CHECK_EQ_U64(frc_glyph_states(FRC_MAX_STROKES + 1), 0);
    CHECK_EQ_U64(frc_glyph_extreme(255), 0);
    CHECK_EQ_U64(frc_glyph_states(255), 0);
}

/* The table of SPEC S4.1, row by row, written independently of the library. */
static void expected_state(int index, int m, int *value, int *derivative, int *dotted)
{
    if (index < m) {
        *value = index, *derivative = 1, *dotted = 0;
    } else if (index == m) {
        *value = m, *derivative = 0, *dotted = 0;
    } else if (index < 3 * m) {
        *value = 2 * m - index, *derivative = -1, *dotted = 1;
    } else if (index == 3 * m) {
        *value = -m, *derivative = 0, *dotted = 0;
    } else {
        *value = index - 4 * m, *derivative = 1, *dotted = 0;
    }
}

static void test_from_index(void)
{
    int d, i, k;

    TEST_SECTION("from_index: the table of S4.1 and the trits of S4.2");
    for (d = 1; d <= FRC_MAX_STROKES; d++) {
        int m = frc_glyph_extreme((uint8_t)d);
        for (i = 0; i < 4 * m; i++) {
            frc_glyph_t g, wrapped;
            int value, derivative, dotted, sum = 0, weight = 1;

            expected_state(i, m, &value, &derivative, &dotted);
            memset(&g, 0x55, sizeof g);
            CHECK(frc_glyph_from_index((uint16_t)i, (uint8_t)d, &g) == FRC_OK);
            CHECK_EQ_U64(g.index, i);
            CHECK_EQ_I64(g.value, value);
            CHECK_EQ_I64(g.derivative, derivative);
            CHECK_EQ_U64(g.dotted, dotted);
            CHECK_EQ_U64(g.strokes, d);
            CHECK_NOTE("strokes %d index %d", d, i);
            /* v = sum of t_i * 3^i, every t_i a trit, the unused ones 0 */
            for (k = 0; k < FRC_MAX_STROKES; k++) {
                CHECK(g.trit[k] >= -1 && g.trit[k] <= 1);
                if (k >= d) {
                    CHECK_EQ_I64(g.trit[k], 0);
                }
                sum += g.trit[k] * weight;
                weight *= 3;
            }
            CHECK_EQ_I64(sum, value);
            CHECK_NOTE("strokes %d index %d", d, i);

            /* "taken mod 4M" */
            CHECK(frc_glyph_from_index((uint16_t)(i + 4 * m), (uint8_t)d, &wrapped) == FRC_OK);
            CHECK(wrapped.index == g.index && wrapped.value == g.value && wrapped.derivative == g.derivative &&
                  wrapped.dotted == g.dotted && memcmp(wrapped.trit, g.trit, sizeof g.trit) == 0);
            CHECK(frc_glyph_from_index((uint16_t)(i + 100 * 4 * m), (uint8_t)d, &wrapped) == FRC_OK);
            CHECK_EQ_U64(wrapped.index, i);
            CHECK_EQ_I64(wrapped.value, value);
        }
    }

    TEST_SECTION("from_index: the sequences SPEC S4.1 prints");
    {
        /* "0 1 2 3 4 3. 2. 1. 0. -1. -2. -3. -4 -3 -2 -1" */
        static const int8_t VALUE2[16] = {0, 1, 2, 3, 4, 3, 2, 1, 0, -1, -2, -3, -4, -3, -2, -1};
        static const uint8_t DOT2[16] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
        frc_glyph_t g;
        for (i = 0; i < 16; i++) {
            CHECK(frc_glyph_from_index((uint16_t)i, 2, &g) == FRC_OK);
            CHECK_EQ_I64(g.value, VALUE2[i]);
            CHECK_EQ_U64(g.dotted, DOT2[i]);
        }
        /* "0 ... 13, 12. ... -12., -13, -12 ... -1" */
        for (i = 0; i < 52; i++) {
            CHECK(frc_glyph_from_index((uint16_t)i, 3, &g) == FRC_OK);
            if (i <= 13) {
                CHECK(g.value == i && !g.dotted);
            } else if (i <= 38) {
                CHECK(g.value == 26 - i && g.dotted);
            } else {
                CHECK(g.value == i - 52 && !g.dotted);
            }
        }
        /* hand-checked trits, least significant first: 5 = -1 + -1*3 + 1*9 */
        CHECK(frc_glyph_from_index(5, 3, &g) == FRC_OK);
        CHECK(g.trit[0] == -1 && g.trit[1] == -1 && g.trit[2] == 1 && g.trit[3] == 0 && g.trit[4] == 0);
        /* -121 = five trits of -1: the valley of the largest glyph */
        CHECK(frc_glyph_from_index(363, 5, &g) == FRC_OK);
        CHECK_EQ_I64(g.value, -121);
        CHECK(g.trit[0] == -1 && g.trit[1] == -1 && g.trit[2] == -1 && g.trit[3] == -1 && g.trit[4] == -1);
        CHECK(g.derivative == 0 && g.dotted == 0); /* both extrema are bare */
        CHECK(frc_glyph_from_index(65535, 5, &g) == FRC_OK);
        CHECK_EQ_U64(g.index, 65535 % 484);
    }

    TEST_SECTION("from_index: invalid arguments");
    {
        frc_glyph_t g;
        memset(&g, 0x55, sizeof g);
        CHECK(frc_glyph_from_index(0, 0, &g) == FRC_ERR_ARG);
        CHECK(frc_glyph_from_index(0, FRC_MAX_STROKES + 1, &g) == FRC_ERR_ARG);
        CHECK(frc_glyph_from_index(0, 255, &g) == FRC_ERR_ARG);
        CHECK(frc_glyph_from_index(0, 2, NULL) == FRC_ERR_ARG);
        CHECK(g.index == 0x5555 && g.strokes == 0x55); /* untouched on error */
    }
}

static void test_index_helpers(void)
{
    int d, i;

    TEST_SECTION("descending family");
    for (d = 1; d <= FRC_MAX_STROKES; d++) {
        int m = frc_glyph_extreme((uint8_t)d);
        for (i = 0; i < 4 * m; i++) {
            frc_glyph_t up, down;
            uint16_t other = frc_glyph_index_descending((uint16_t)i, (uint8_t)d);
            CHECK_EQ_U64(other, (i + 2 * m) % (4 * m));
            /* half a cycle apart: opposite value, same branch direction mirrored */
            CHECK(frc_glyph_from_index((uint16_t)i, (uint8_t)d, &up) == FRC_OK);
            CHECK(frc_glyph_from_index(other, (uint8_t)d, &down) == FRC_OK);
            CHECK_EQ_I64(down.value, -up.value);
            CHECK_EQ_I64(down.derivative, -up.derivative);
            /* twice is the identity; an index beyond 4M wraps first */
            CHECK_EQ_U64(frc_glyph_index_descending(other, (uint8_t)d), i);
            CHECK_EQ_U64(frc_glyph_index_descending((uint16_t)(i + 4 * m), (uint8_t)d), other);
        }
        CHECK_EQ_U64(frc_glyph_index_descending(65535, (uint8_t)d), (65535 % (4 * m) + 2 * m) % (4 * m));
    }
    CHECK_EQ_U64(frc_glyph_index_descending(7, 0), 0);
    CHECK_EQ_U64(frc_glyph_index_descending(7, FRC_MAX_STROKES + 1), 0);

    TEST_SECTION("index of a phase");
    for (d = 1; d <= FRC_MAX_STROKES; d++) {
        uint32_t m = frc_glyph_extreme((uint8_t)d);
        uint32_t phase = 0;
        uint16_t previous = 0;
        /* the four thresholds of S6.2: index 0, M (peak), 2M (node), 3M (valley) */
        CHECK_EQ_U64(frc_glyph_index_of_phase(0, (uint8_t)d), 0);
        CHECK_EQ_U64(frc_glyph_index_of_phase(FRC_FRAC_QUARTER, (uint8_t)d), m);
        CHECK_EQ_U64(frc_glyph_index_of_phase(FRC_FRAC_HALF, (uint8_t)d), 2 * m);
        CHECK_EQ_U64(frc_glyph_index_of_phase(FRC_FRAC_HALF + FRC_FRAC_QUARTER, (uint8_t)d), 3 * m);
        CHECK_EQ_U64(frc_glyph_index_of_phase(FRC_FRAC_QUARTER - 1, (uint8_t)d), m - 1);
        CHECK_EQ_U64(frc_glyph_index_of_phase(0xFFFFFFFFu, (uint8_t)d), 4 * m - 1);
        /* floor(phase * 4M / 2^32), never decreasing */
        for (i = 0; i < 20000; i++) {
            uint16_t index = frc_glyph_index_of_phase(phase, (uint8_t)d);
            CHECK_EQ_U64(index, ((uint64_t)phase * (4 * m)) >> 32);
            CHECK(index >= previous && index < 4 * m);
            previous = index;
            phase += 214748u; /* about 2^32 / 20000 */
        }
    }
    CHECK_EQ_U64(frc_glyph_index_of_phase(FRC_FRAC_HALF, 0), 0);
    CHECK_EQ_U64(frc_glyph_index_of_phase(FRC_FRAC_HALF, FRC_MAX_STROKES + 1), 0);
}

/* ------------------------------------------------------------------------- */
/* Glyphs and odometers at an instant                                         */
/* ------------------------------------------------------------------------- */

static void check_glyph_state(const frc_glyph_t *g, const oracle_clock_glyph_t *e, uint8_t strokes)
{
    int k;
    CHECK_EQ_I64(g->value, e->value);
    CHECK_EQ_I64(g->derivative, e->derivative);
    CHECK_EQ_U64(g->dotted, e->dotted);
    CHECK_EQ_U64(g->strokes, strokes);
    for (k = 0; k < FRC_MAX_STROKES; k++) {
        CHECK_EQ_I64(g->trit[k], k < 3 ? e->trit[k] : 0);
    }
}

static void test_spec_vectors(void)
{
    /* SPEC S12, the rows that concern glyphs and odometers. Depth 7 = level 6. */
    static const struct {
        const char *name;
        int64_t unix_s;
        uint8_t level;
        uint16_t index;
        int8_t value;
        int8_t derivative;
        uint16_t digits[2];
    } V[] = {
        {"A depth 7", INT64_C(1791460800), 6, 13, -3, 1, {13, 0}},
        {"A depth 8", INT64_C(1791460800), 7, 5, 3, -1, {5, 4}},
        {"B depth 7", INT64_C(946684800), 6, 5, 3, -1, {5, 6}},
        {"B depth 8", INT64_C(946684800), 7, 0, 0, 1, {0, 6}},
        {"C depth 7", INT64_C(2886085800), 6, 10, -2, -1, {10, 3}},
        {"C depth 8", INT64_C(2886085800), 7, 12, -4, 0, {12, 1}},
    };
    size_t i;

    TEST_SECTION("SPEC S12 vectors");
    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        frc_time_t t = FRC_TIME_FROM_S(V[i].unix_s);
        frc_glyph_t g;
        frc_odometer_t odo;
        CHECK(frc_glyph_at(CAT, t, V[i].level, 2, &g) == FRC_OK);
        CHECK_EQ_U64(g.index, V[i].index);
        CHECK_EQ_I64(g.value, V[i].value);
        CHECK_EQ_I64(g.derivative, V[i].derivative);
        CHECK_EQ_U64(g.dotted, V[i].derivative < 0);
        CHECK_NOTE("%s", V[i].name);
        CHECK(frc_odometer_at(CAT, t, V[i].level, 2, 2, &odo) == FRC_OK);
        CHECK_EQ_U64(odo.digit[0], V[i].digits[0]);
        CHECK_EQ_U64(odo.digit[1], V[i].digits[1]);
        CHECK_NOTE("%s", V[i].name);
    }
    {
        /* A, depth 7: "3-stroke index / value 42 / -10" */
        frc_glyph_t g;
        CHECK(frc_glyph_at(CAT, FRC_TIME_FROM_S(INT64_C(1791460800)), 6, 3, &g) == FRC_OK);
        CHECK_EQ_U64(g.index, 42);
        CHECK_EQ_I64(g.value, -10);
        CHECK_EQ_I64(g.derivative, 1);
    }
    {
        /* the header's quick start: "frc_odometer_at(cat, now, 6, 2, 2, &helion)" */
        frc_odometer_t helion;
        CHECK(frc_odometer_at(CAT, FRC_TIME_FROM_MS(INT64_C(1791460800000)), 6, 2, 2, &helion) == FRC_OK);
        CHECK(helion.count == 2 && helion.strokes == 2 && helion.states == 16);
        CHECK(helion.digit[0] == 13 && helion.digit[1] == 0 && helion.digit[2] == 0 && helion.digit[3] == 0);
        /* S5: "at depth 7 ... the sub glyph changes every 71 s" */
        CHECK(helion.next_change > FRC_TIME_FROM_S(INT64_C(1791460800)));
        CHECK(helion.next_change - FRC_TIME_FROM_S(INT64_C(1791460800)) <= FRC_TIME_FROM_S(72));
    }
}

static void test_samples_against_oracle(void)
{
    static const uint8_t STROKES[3] = {2, 3, 4};
    int i, level, s, j;

    TEST_SECTION("glyphs and odometers against the clock oracle");
    for (i = 0; i < ORACLE_CLOCK_SAMPLES_COUNT; i++) {
        const oracle_clock_sample_t *v = &ORACLE_CLOCK_SAMPLES[i];
        frc_glyph_t g;
        frc_odometer_t odo;

        if (!v->in_range) {
            /* outside the catalogue the clock is undefined: never extrapolated */
            CHECK(frc_glyph_at(CAT, v->t_us, 6, 2, &g) == FRC_ERR_RANGE);
            CHECK(frc_odometer_at(CAT, v->t_us, 6, 2, 2, &odo) == FRC_ERR_RANGE);
            CHECK_NOTE("sample %d (%s)", i, v->label);
            continue;
        }
        for (level = 0; level < ORACLE_CLOCK_LEVELS; level++) {
            const oracle_clock_level_t *e = &v->level[level];
            frc_cycle_t cycle;

            for (s = 0; s < 3; s++) {
                frc_glyph_t decoded;
                CHECK(frc_glyph_at(CAT, v->t_us, (uint8_t)level, STROKES[s], &g) == FRC_OK);
                CHECK_EQ_U64(g.index, e->index[s]);
                CHECK_NOTE("sample %d (%s) level %d strokes %d", i, v->label, level, STROKES[s]);
                /* the state at an instant is the decoded index, nothing more */
                CHECK(frc_glyph_from_index(e->index[s], STROKES[s], &decoded) == FRC_OK);
                CHECK(g.index == decoded.index && g.value == decoded.value && g.derivative == decoded.derivative &&
                      g.dotted == decoded.dotted && g.strokes == decoded.strokes &&
                      memcmp(g.trit, decoded.trit, sizeof g.trit) == 0);
                if (s == 0) {
                    check_glyph_state(&g, &e->glyph2, 2);
                } else if (s == 1) {
                    check_glyph_state(&g, &e->glyph3, 3);
                }
                CHECK_NOTE("sample %d (%s) level %d strokes %d", i, v->label, level, STROKES[s]);
            }

            CHECK(frc_odometer_at(CAT, v->t_us, (uint8_t)level, 2, 2, &odo) == FRC_OK);
            CHECK(odo.count == 2 && odo.strokes == 2 && odo.states == 16);
            CHECK_EQ_U64(odo.digit[0], e->odometer[0]);
            CHECK_EQ_U64(odo.digit[1], e->odometer[1]);
            CHECK(odo.digit[2] == 0 && odo.digit[3] == 0);
            CHECK_EQ_I64(odo.next_change, e->odometer_next_us);
            CHECK_NOTE("sample %d (%s) level %d", i, v->label, level);
            CHECK_EQ_U64(odo.digit[0], e->index[0]); /* S5: digit 0 is the single-glyph index */
            CHECK(odo.next_change == FRC_TIME_NONE || odo.next_change > v->t_us);

            /* With 16 states the glyph is the top four bits of the cycle's phase and the
             * pair its top eight (16 and 256 divide 2^32, so the floors nest exactly). */
            CHECK(frc_cycle_at(CAT, v->t_us, (uint8_t)level, &cycle) == FRC_OK);
            CHECK_EQ_U64(frc_glyph_index_of_phase(cycle.phase, 2), e->index[0]);
            CHECK(frc_odometer_of_phase(cycle.phase, 2, 2, &odo) == FRC_OK);
            CHECK(odo.digit[0] == e->odometer[0] && odo.digit[1] == e->odometer[1]);
            CHECK_EQ_I64(odo.next_change, FRC_TIME_NONE);
            /* the glyph's quarter is the cycle's quarter: M states in each */
            CHECK_EQ_U64(e->index[1] / 13, cycle.quarter);
            CHECK_EQ_U64(e->index[2] / 40, cycle.quarter);
        }

        CHECK(frc_odometer_at(CAT, v->t_us, 6, 3, 2, &odo) == FRC_OK);
        CHECK(odo.count == 2 && odo.strokes == 3 && odo.states == 52);
        CHECK(odo.digit[0] == v->odo6_s3d2[0] && odo.digit[1] == v->odo6_s3d2[1]);
        CHECK_EQ_I64(odo.next_change, v->odo6_s3d2_next_us);
        CHECK_NOTE("sample %d (%s) 3 strokes x 2", i, v->label);

        CHECK(frc_odometer_at(CAT, v->t_us, 6, 2, 3, &odo) == FRC_OK);
        CHECK(odo.count == 3 && odo.strokes == 2 && odo.states == 16);
        for (j = 0; j < 3; j++) {
            CHECK_EQ_U64(odo.digit[j], v->odo6_s2d3[j]);
        }
        CHECK_EQ_U64(odo.digit[3], 0);
        CHECK_EQ_I64(odo.next_change, v->odo6_s2d3_next_us);
        CHECK_NOTE("sample %d (%s) 2 strokes x 3", i, v->label);
    }

    TEST_SECTION("deep levels 14..21 against the clock oracle");
    for (i = 0; i < ORACLE_CLOCK_DEEP_SAMPLES_COUNT; i++) {
        const oracle_clock_deep_sample_t *v = &ORACLE_CLOCK_DEEP_SAMPLES[i];
        for (j = 0; j < ORACLE_CLOCK_DEEP_LEVELS; j++) {
            uint8_t deep = (uint8_t)(ORACLE_CLOCK_DEEP_FIRST_LEVEL + j);
            frc_glyph_t g;
            frc_odometer_t odo;
            CHECK(frc_glyph_at(CAT, v->t_us, deep, 2, &g) == FRC_OK);
            CHECK_EQ_U64(g.index, v->level[j].index2);
            CHECK_NOTE("deep sample %d level %d", i, deep);
            CHECK_EQ_U64(g.index, v->level[j].phase >> 28);
            CHECK(frc_odometer_at(CAT, v->t_us, deep, 2, 2, &odo) == FRC_OK);
            CHECK_EQ_U64(odo.digit[0] * 16u + odo.digit[1], v->level[j].phase >> 24);
        }
    }
}

/* The odometer step of S5 put back together from its digits. */
static uint64_t step_of(const frc_odometer_t *odo)
{
    uint64_t step = 0;
    int j;
    for (j = 0; j < odo->count; j++) {
        step = step * odo->states + odo->digit[j];
    }
    return step;
}

static void test_steps_against_foundation_oracle(void)
{
    int i, matched = 0;
    uint8_t strokes, digits;

    /* Each oracle row is a grid w = M * q^(k-1) states per quarter. Every (strokes, digits)
     * that makes that grid must read the same step and the same next change. */
    TEST_SECTION("odometer steps against the foundation oracle");
    for (i = 0; i < ORACLE_FOUNDATION_STEP_COUNT; i++) {
        const oracle_foundation_step_t *v = &ORACLE_FOUNDATION_STEP[i];
        int hits = 0;
        for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
            uint64_t grid = frc_glyph_extreme(strokes);
            for (digits = 1; digits <= FRC_MAX_DIGITS; digits++) {
                if (grid == v->w) {
                    frc_odometer_t odo;
                    CHECK(frc_odometer_at(CAT, v->t, v->level, strokes, digits, &odo) == FRC_OK);
                    CHECK_EQ_U64(step_of(&odo), v->step);
                    CHECK_EQ_I64(odo.next_change, v->next);
                    CHECK_NOTE("step vector %d: level %d, %d strokes x %d", i, v->level, strokes, digits);
                    hits++;
                }
                grid *= frc_glyph_states(strokes);
            }
        }
        CHECK(hits >= 1); /* every grid of the oracle is an odometer the API can make */
        matched += hits;
    }
    CHECK(matched > ORACLE_FOUNDATION_STEP_COUNT); /* some grids have two spellings (64 = 2x2 = 1x4) */
}

static void test_odometer_properties(void)
{
    /* instants spread over the catalogue, none of them special */
    static const int64_t T_MS[] = {
        INT64_C(-11253795486968) + 1, INT64_C(-9000000000123), INT64_C(-443379486426), INT64_C(0),
        INT64_C(125593319816), INT64_C(946684800000), INT64_C(1791460800000), INT64_C(1791460800001),
        INT64_C(2886085800000), INT64_C(12345678901234), INT64_C(28005212035816) - 86400000,
    };
    static const uint8_t LEVELS[] = {0, 1, 3, 4, 6, 7, 8, 10, 12, 13, 17, 21};
    size_t a, b;
    uint8_t strokes, digits;
    int exact_steps = 0;

    TEST_SECTION("odometer properties");
    for (a = 0; a < sizeof T_MS / sizeof T_MS[0]; a++) {
        frc_time_t t = FRC_TIME_FROM_MS(T_MS[a]) + (frc_time_t)(a * 137); /* off the millisecond grid too */
        for (b = 0; b < sizeof LEVELS / sizeof LEVELS[0]; b++) {
            uint8_t level = LEVELS[b];
            for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
                frc_glyph_t g;
                frc_odometer_t shorter;
                CHECK(frc_glyph_at(CAT, t, level, strokes, &g) == FRC_OK);
                memset(&shorter, 0, sizeof shorter);
                for (digits = 1; digits <= FRC_MAX_DIGITS; digits++) {
                    frc_odometer_t odo, before, after;
                    frc_cycle_t cycle;
                    uint64_t turn;
                    int j;
                    memset(&odo, 0x55, sizeof odo);
                    CHECK(frc_odometer_at(CAT, t, level, strokes, digits, &odo) == FRC_OK);
                    CHECK(odo.count == digits && odo.strokes == strokes && odo.states == frc_glyph_states(strokes));
                    CHECK_EQ_U64(odo.digit[0], g.index); /* S5: digit 0 is the glyph of S4.1 */
                    for (j = 0; j < FRC_MAX_DIGITS; j++) {
                        CHECK(odo.digit[j] < odo.states);
                        if (j >= digits) {
                            CHECK_EQ_U64(odo.digit[j], 0);
                        }
                        /* one more glyph refines the reading and changes nothing to its left */
                        if (j < digits - 1) {
                            CHECK_EQ_U64(odo.digit[j], shorter.digit[j]);
                        }
                    }
                    CHECK_NOTE("t %" PRId64 " level %d strokes %d digits %d", t, level, strokes, digits);

                    /* next_change is the FIRST microsecond of the next state: strictly after
                     * t, the reading one microsecond before it is still this one, and the
                     * reading at it is the next step. (A state shorter than two microseconds
                     * can be skipped whole by the microsecond grid: no claim is made there.) */
                    CHECK(odo.next_change != FRC_TIME_NONE && odo.next_change > t);
                    CHECK(frc_odometer_at(CAT, odo.next_change - 1, level, strokes, digits, &before) == FRC_OK);
                    CHECK_EQ_U64(step_of(&before), step_of(&odo));
                    CHECK(frc_odometer_at(CAT, odo.next_change, level, strokes, digits, &after) == FRC_OK);
                    CHECK(frc_cycle_at(CAT, t, level, &cycle) == FRC_OK);
                    turn = 1;
                    for (j = 0; j < digits; j++) {
                        turn *= odo.states;
                    }
                    if (odo.next_change == frc_catalogue_last(CAT)) {
                        /* the state ends with the catalogue: its closing instant still reads
                         * as the microsecond before it (S2.2), and nothing follows */
                        CHECK_EQ_U64(step_of(&after), step_of(&odo));
                        CHECK_EQ_I64(after.next_change, FRC_TIME_NONE);
                    } else if ((uint64_t)(cycle.end - cycle.start) / turn >= 2) {
                        CHECK_EQ_U64(step_of(&after), (step_of(&odo) + 1) % turn);
                        exact_steps++;
                    }
                    CHECK_NOTE("t %" PRId64 " level %d strokes %d digits %d", t, level, strokes, digits);
                    shorter = odo;
                }
            }
        }
    }
    CHECK(exact_steps > 1000); /* the "one step further" claim was really put to the test */
}

/* floor(phase * q^k / 2^32) in 64 bits, one digit at a time, to compare with the
 * library's 128-bit product: phase * q^k / 2^32 = q * hi + q * lo / 2^32, where
 * phase * q^(k-1) = hi * 2^32 + lo. */
static uint64_t step_of_phase(uint32_t phase, uint64_t q, int k)
{
    uint64_t lower = 1, a;
    int j;
    for (j = 1; j < k; j++) {
        lower *= q;
    }
    a = (uint64_t)phase * lower; /* q^(k-1) <= 484^3 < 2^32 */
    return q * (a >> 32) + ((q * (a & 0xFFFFFFFFu)) >> 32);
}

static void test_odometer_of_phase(void)
{
    static const uint32_t PHASES[] = {
        0u, 1u, 0x00FFFFFFu, 0x01000000u, 0x0FFFFFFFu, 0x10000000u, 0x3FFFFFFFu, 0x40000000u, 0x55555555u,
        0x7FFFFFFFu, 0x80000000u, 0x9E3779B9u, 0xC0000000u, 0xDEADBEEFu, 0xFFFFFFFEu, 0xFFFFFFFFu,
    };
    size_t a;
    uint8_t strokes, digits;

    TEST_SECTION("odometer of a bare phase");
    for (a = 0; a < sizeof PHASES / sizeof PHASES[0]; a++) {
        for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
            for (digits = 1; digits <= FRC_MAX_DIGITS; digits++) {
                frc_odometer_t odo;
                memset(&odo, 0x55, sizeof odo);
                CHECK(frc_odometer_of_phase(PHASES[a], strokes, digits, &odo) == FRC_OK);
                CHECK(odo.count == digits && odo.strokes == strokes && odo.states == frc_glyph_states(strokes));
                CHECK_EQ_U64(step_of(&odo), step_of_phase(PHASES[a], odo.states, digits));
                CHECK_NOTE("phase %u strokes %d digits %d", (unsigned)PHASES[a], strokes, digits);
                CHECK_EQ_U64(odo.digit[0], frc_glyph_index_of_phase(PHASES[a], strokes));
                CHECK_EQ_I64(odo.next_change, FRC_TIME_NONE);
                CHECK(digits == FRC_MAX_DIGITS || odo.digit[digits] == 0);
            }
        }
    }
    {
        /* "2 digits of 2 strokes = the 256-state pair": the top byte of the phase */
        frc_odometer_t odo;
        CHECK(frc_odometer_of_phase(0xA7000000u, 2, 2, &odo) == FRC_OK);
        CHECK(odo.digit[0] == 0xA && odo.digit[1] == 0x7);
        CHECK(frc_odometer_of_phase(0xA7FFFFFFu, 2, 2, &odo) == FRC_OK);
        CHECK(odo.digit[0] == 0xA && odo.digit[1] == 0x7);
        CHECK(frc_odometer_of_phase(0xFFFFFFFFu, 2, 4, &odo) == FRC_OK);
        CHECK(odo.digit[0] == 15 && odo.digit[1] == 15 && odo.digit[2] == 15 && odo.digit[3] == 15);
    }

    TEST_SECTION("odometer: invalid arguments");
    {
        frc_odometer_t odo;
        frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
        memset(&odo, 0x55, sizeof odo);
        CHECK(frc_odometer_of_phase(0, 0, 2, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_of_phase(0, FRC_MAX_STROKES + 1, 2, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_of_phase(0, 2, 0, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_of_phase(0, 2, FRC_MAX_DIGITS + 1, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_of_phase(0, 2, 2, NULL) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, t, 6, 0, 2, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, t, 6, FRC_MAX_STROKES + 1, 2, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, t, 6, 2, 0, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, t, 6, 2, FRC_MAX_DIGITS + 1, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, t, 6, 2, 2, NULL) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(NULL, t, 6, 2, 2, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, t, FRC_CLOCK_MAX_LEVEL + 1, 2, 2, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, t, 255, 2, 2, &odo) == FRC_ERR_ARG);
        /* an argument error wins over an instant outside the catalogue */
        CHECK(frc_odometer_at(CAT, INT64_MAX, 6, 9, 2, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, INT64_MAX, FRC_CLOCK_MAX_LEVEL + 1, 2, 2, &odo) == FRC_ERR_ARG);
        CHECK(frc_odometer_at(CAT, INT64_MAX, 6, 2, 2, &odo) == FRC_ERR_RANGE);
        CHECK(frc_odometer_at(CAT, FRC_TIME_NONE, 6, 2, 2, &odo) == FRC_ERR_RANGE);
        CHECK(odo.count == 0x55 && odo.states == 0x5555 && odo.digit[0] == 0x5555); /* untouched by any error */
    }
}

static void test_catalogue_edges(void)
{
    frc_time_t first = frc_catalogue_first(CAT), last = frc_catalogue_last(CAT);
    uint8_t level, strokes;

    TEST_SECTION("glyph_at: invalid arguments");
    {
        frc_glyph_t g;
        frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
        memset(&g, 0x55, sizeof g);
        CHECK(frc_glyph_at(NULL, t, 6, 2, &g) == FRC_ERR_ARG);
        CHECK(frc_glyph_at(CAT, t, 6, 0, &g) == FRC_ERR_ARG);
        CHECK(frc_glyph_at(CAT, t, 6, FRC_MAX_STROKES + 1, &g) == FRC_ERR_ARG);
        CHECK(frc_glyph_at(CAT, t, 6, 2, NULL) == FRC_ERR_ARG);
        CHECK(frc_glyph_at(CAT, t, FRC_CLOCK_MAX_LEVEL + 1, 2, &g) == FRC_ERR_ARG);
        CHECK(frc_glyph_at(CAT, INT64_MAX, 6, 0, &g) == FRC_ERR_ARG);
        CHECK(frc_glyph_at(CAT, INT64_MAX, FRC_CLOCK_MAX_LEVEL + 1, 2, &g) == FRC_ERR_ARG);
        CHECK(g.index == 0x5555 && g.strokes == 0x55);
        CHECK(frc_glyph_at(CAT, first - 1, 6, 2, &g) == FRC_ERR_RANGE);
        CHECK(frc_glyph_at(CAT, last + 1, 6, 2, &g) == FRC_ERR_RANGE);
        CHECK(frc_glyph_at(CAT, INT64_MAX, 6, 2, &g) == FRC_ERR_RANGE);
        CHECK(frc_glyph_at(CAT, FRC_TIME_NONE, 6, 2, &g) == FRC_ERR_RANGE);
    }

    TEST_SECTION("the first instant of the catalogue");
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
            frc_glyph_t g;
            frc_odometer_t odo;
            /* position 0: every cycle of every level begins, every glyph reads 0 rising */
            CHECK(frc_glyph_at(CAT, first, level, strokes, &g) == FRC_OK);
            CHECK(g.index == 0 && g.value == 0 && g.derivative == 1 && g.dotted == 0);
            CHECK(frc_odometer_at(CAT, first, level, strokes, FRC_MAX_DIGITS, &odo) == FRC_OK);
            CHECK(odo.digit[0] == 0 && odo.digit[1] == 0 && odo.digit[2] == 0 && odo.digit[3] == 0);
            CHECK(odo.next_change > first);
        }
    }

    TEST_SECTION("the closing instant of the catalogue");
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        frc_glyph_t at_end, inside;
        frc_odometer_t odo, odo_inside;
        /* S2.2: the closing instant belongs to the last cycle; it reads as the
         * microsecond before it, and nothing is scheduled after it (pitfall 7) */
        CHECK(frc_glyph_at(CAT, last, level, 2, &at_end) == FRC_OK);
        CHECK(frc_glyph_at(CAT, last - 1, level, 2, &inside) == FRC_OK);
        CHECK_EQ_U64(at_end.index, inside.index);
        CHECK(frc_odometer_at(CAT, last, level, 2, 2, &odo) == FRC_OK);
        CHECK(frc_odometer_at(CAT, last - 1, level, 2, 2, &odo_inside) == FRC_OK);
        CHECK(odo.digit[0] == odo_inside.digit[0] && odo.digit[1] == odo_inside.digit[1]);
        CHECK_EQ_I64(odo.next_change, FRC_TIME_NONE);
        CHECK_NOTE("level %d", level);
    }
    {
        /* The catalogue has 69 Saros = 17.25 Alphas: the last Alpha is cut after its
         * first quarter, so just before the end the Alpha glyph is about to peak. */
        frc_glyph_t g;
        CHECK(frc_glyph_at(CAT, last - 1, 0, 2, &g) == FRC_OK);
        CHECK_EQ_U64(g.index, 3);
    }
    /* An eclipse is a boundary of every grid (position * P * U is an integer there), so
     * the closing instant ends the running state of every odometer: one microsecond
     * before it, the next change is the closing instant itself, never beyond it. */
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
            uint8_t digits;
            for (digits = 1; digits <= FRC_MAX_DIGITS; digits++) {
                frc_odometer_t odo;
                CHECK(frc_odometer_at(CAT, last - 1, level, strokes, digits, &odo) == FRC_OK);
                CHECK_EQ_I64(odo.next_change, last);
                CHECK_NOTE("level %d strokes %d digits %d", level, strokes, digits);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* S9.1  Geometry                                                             */
/* ------------------------------------------------------------------------- */

#define MAX_OPS 64

typedef struct recorded_path {
    int count;
    frc_path_op_t op[MAX_OPS];
    int16_t pts[MAX_OPS][6];
} recorded_path_t;

static int points_of(frc_path_op_t op)
{
    return op == FRC_PATH_CUBIC ? 6 : op == FRC_PATH_DOT ? 3 : 2;
}

static void record_op(void *ctx, frc_path_op_t op, const int16_t *pts)
{
    recorded_path_t *path = (recorded_path_t *)ctx;
    int i;
    if (path->count < MAX_OPS) {
        path->op[path->count] = op;
        for (i = 0; i < points_of(op); i++) {
            path->pts[path->count][i] = pts[i];
        }
    }
    path->count++;
}

#define SVG_TEXT_SIZE 1024

/* A coordinate in design units the way the reference prints it: "-1", "2", "0.5". */
static void append_number(char *text, int16_t hundredths)
{
    size_t used = strlen(text);
    char *end = text + used;
    size_t room = SVG_TEXT_SIZE - used;
    int magnitude = hundredths < 0 ? -hundredths : hundredths;
    const char *sign = hundredths < 0 ? "-" : "";
    if (magnitude % 100 == 0) {
        snprintf(end, room, "%s%d", sign, magnitude / 100);
    } else if (magnitude % 10 == 0) {
        snprintf(end, room, "%s%d.%d", sign, magnitude / 100, magnitude % 100 / 10);
    } else {
        snprintf(end, room, "%s%d.%02d", sign, magnitude / 100, magnitude % 100);
    }
}

/* The recorded path, without its dot, as the reference's SVG path string
 * (`text` holds SVG_TEXT_SIZE bytes; the longest path here is under 300). */
static void path_to_svg(const recorded_path_t *path, char *text)
{
    static const char LETTER[] = {'M', 'L', 'C'};
    int i, k;
    text[0] = '\0';
    for (i = 0; i < path->count && i < MAX_OPS; i++) {
        size_t used = strlen(text);
        if (path->op[i] == FRC_PATH_DOT) {
            continue;
        }
        snprintf(text + used, SVG_TEXT_SIZE - used, "%s%c", used ? " " : "", LETTER[path->op[i]]);
        for (k = 0; k < points_of(path->op[i]); k++) {
            if (k > 0) {
                used = strlen(text);
                snprintf(text + used, SVG_TEXT_SIZE - used, " ");
            }
            append_number(text, path->pts[i][k]);
        }
    }
}

static void test_path_against_oracle(void)
{
    int i;
    char text[SVG_TEXT_SIZE];

    TEST_SECTION("glyph paths against the reference");
    for (i = 0; i < ORACLE_GEOMETRY_GLYPH_COUNT; i++) {
        const oracle_geometry_glyph_t *v = &ORACLE_GEOMETRY_GLYPH[i];
        frc_glyph_t g;
        recorded_path_t path;

        path.count = 0;
        CHECK(frc_glyph_from_index(v->index, v->strokes, &g) == FRC_OK);
        CHECK_EQ_U64(g.dotted, v->dotted);
        frc_glyph_path(&g, record_op, &path);
        CHECK(path.count > 0 && path.count <= MAX_OPS);
        path_to_svg(&path, text);
        CHECK_STR(text, v->path);
        CHECK_NOTE("strokes %d index %d", v->strokes, v->index);
        /* the dot comes last, and only on the falling branch: (0, -1.75), radius 0.3 */
        if (v->dotted) {
            const int16_t *dot = path.pts[path.count - 1];
            CHECK(path.op[path.count - 1] == FRC_PATH_DOT);
            CHECK(dot[0] == 0 && dot[1] == -175 && dot[2] == 30);
            CHECK(dot[1] == FRC_GLYPH_DOT_Y && dot[2] == FRC_GLYPH_DOT_RADIUS);
        } else {
            CHECK(path.op[path.count - 1] != FRC_PATH_DOT);
        }
    }

    TEST_SECTION("glyph bounds");
    for (i = 0; i < ORACLE_GEOMETRY_BOUNDS_COUNT; i++) {
        const oracle_geometry_bounds_t *v = &ORACLE_GEOMETRY_BOUNDS[i];
        int16_t x0 = 1, y0 = 1, x1 = 1, y1 = 1;
        frc_glyph_bounds(v->strokes, &x0, &y0, &x1, &y1);
        CHECK(x0 == v->min_x && y0 == v->min_y && x1 == v->max_x && y1 == v->max_y);
    }
    {
        int16_t x0 = 1, y0 = 1, x1 = 1, y1 = 1;
        /* the header: x -215..215, y -220..(300*strokes + 15) */
        frc_glyph_bounds(5, &x0, &y0, &x1, &y1);
        CHECK(x0 == -215 && y0 == -220 && x1 == 215 && y1 == 1515);
        /* any of the four may be left out */
        y1 = 1;
        frc_glyph_bounds(2, NULL, NULL, NULL, &y1);
        CHECK_EQ_I64(y1, 615);
        x0 = 1;
        frc_glyph_bounds(2, &x0, NULL, NULL, NULL);
        CHECK_EQ_I64(x0, -215);
        frc_glyph_bounds(2, NULL, NULL, NULL, NULL);
        /* no such glyph: empty bounds, and no int16 overflow for 255 strokes */
        frc_glyph_bounds(0, &x0, &y0, &x1, &y1);
        CHECK(x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0);
        x0 = y0 = x1 = y1 = 1;
        frc_glyph_bounds(255, &x0, &y0, &x1, &y1);
        CHECK(x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0);
    }
}

static void test_path_structure(void)
{
    int d, i, k;

    /* The table of SPEC S9.1 written out again, for every state of every stroke count
     * (the reference oracle stops at 4 strokes). */
    TEST_SECTION("glyph path: the table of S9.1");
    for (d = 1; d <= FRC_MAX_STROKES; d++) {
        int16_t min_x, min_y, max_x, max_y;
        frc_glyph_bounds((uint8_t)d, &min_x, &min_y, &max_x, &max_y);
        for (i = 0; i < frc_glyph_states((uint8_t)d); i++) {
            frc_glyph_t g;
            recorded_path_t path;
            int at = 0;

            path.count = 0;
            CHECK(frc_glyph_from_index((uint16_t)i, (uint8_t)d, &g) == FRC_OK);
            frc_glyph_path(&g, record_op, &path);
            /* "stem: M 0 3i L 0 3i+3 for each trit" */
            for (k = 0; k < d; k++, at += 2) {
                CHECK(path.op[at] == FRC_PATH_MOVE && path.pts[at][0] == 0 && path.pts[at][1] == 300 * k);
                CHECK(path.op[at + 1] == FRC_PATH_LINE && path.pts[at + 1][0] == 0 &&
                      path.pts[at + 1][1] == 300 * k + 300);
            }
            for (k = 0; k < d; k++) {
                int y = 300 * k, t = g.trit[k];
                const int16_t *p = path.pts[at], *q = path.pts[at + 1], *r = path.pts[at + 2];
                if (t == 0) {
                    /* "M -1 y L 1 y" */
                    CHECK(path.op[at] == FRC_PATH_MOVE && p[0] == -100 && p[1] == y);
                    CHECK(path.op[at + 1] == FRC_PATH_LINE && q[0] == 100 && q[1] == y);
                    at += 2;
                } else {
                    /* +1: "M 0 y C 0 y 0 y+1 1 y+1 C 2 y+1 2 y 2 y"; -1: every offset negated */
                    CHECK(path.op[at] == FRC_PATH_MOVE && p[0] == 0 && p[1] == y);
                    CHECK(path.op[at + 1] == FRC_PATH_CUBIC && q[0] == 0 && q[1] == y && q[2] == 0 &&
                          q[3] == y + 100 * t && q[4] == 100 * t && q[5] == y + 100 * t);
                    CHECK(path.op[at + 2] == FRC_PATH_CUBIC && r[0] == 200 * t && r[1] == y + 100 * t &&
                          r[2] == 200 * t && r[3] == y && r[4] == 200 * t && r[5] == y);
                    at += 3;
                }
                CHECK_NOTE("strokes %d index %d trit %d", d, i, k);
            }
            if (g.dotted) {
                CHECK(path.op[at] == FRC_PATH_DOT && path.pts[at][0] == 0 && path.pts[at][1] == -175 &&
                      path.pts[at][2] == 30);
                at++;
            }
            CHECK_EQ_I64(path.count, at); /* nothing else is drawn */

            /* every point of the skeleton is inside the bounds, half the ink from the edge */
            for (k = 0; k < path.count; k++) {
                int n, half = FRC_GLYPH_INK / 2;
                if (path.op[k] == FRC_PATH_DOT) {
                    int r = path.pts[k][2];
                    CHECK(path.pts[k][0] - r >= min_x && path.pts[k][0] + r <= max_x);
                    CHECK(path.pts[k][1] - r - half >= min_y && path.pts[k][1] + r <= max_y);
                    continue;
                }
                for (n = 0; n < points_of(path.op[k]); n += 2) {
                    CHECK(path.pts[k][n] - half >= min_x && path.pts[k][n] + half <= max_x);
                    CHECK(path.pts[k][n + 1] - half >= min_y && path.pts[k][n + 1] + half <= max_y);
                }
            }
        }
    }

    TEST_SECTION("glyph path: bad input draws nothing");
    {
        frc_glyph_t g;
        recorded_path_t path;
        path.count = 0;
        CHECK(frc_glyph_from_index(5, 2, &g) == FRC_OK);
        frc_glyph_path(NULL, record_op, &path);
        frc_glyph_path(&g, NULL, &path);
        g.strokes = 0;
        frc_glyph_path(&g, record_op, &path);
        g.strokes = FRC_MAX_STROKES + 1;
        frc_glyph_path(&g, record_op, &path);
        g.strokes = 2;
        g.trit[1] = 2; /* not a trit */
        frc_glyph_path(&g, record_op, &path);
        CHECK_EQ_I64(path.count, 0);
        /* ctx is passed through untouched, NULL included */
        CHECK(frc_glyph_from_index(5, 2, &g) == FRC_OK);
        frc_glyph_path(&g, record_op, &path);
        CHECK_EQ_I64(path.count, 2 * 2 + 2 + 3 + 1); /* stems, bar, curve, dot */
    }
}

#define MAX_SEGMENTS 512

typedef struct recorded_segments {
    int count;
    int16_t seg[MAX_SEGMENTS][4];
} recorded_segments_t;

static void record_segment(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    recorded_segments_t *out = (recorded_segments_t *)ctx;
    if (out->count < MAX_SEGMENTS) {
        out->seg[out->count][0] = x0;
        out->seg[out->count][1] = y0;
        out->seg[out->count][2] = x1;
        out->seg[out->count][3] = y1;
    }
    out->count++;
}

/* 1 when `value` is the nearest integer to the cubic's coordinate at j / n, by exact
 * integer arithmetic: |n^3 * value - numerator| <= n^3 / 2. p0..p3 are that coordinate
 * of the four control points. */
static int on_cubic(int value, int p0, int p1, int p2, int p3, int j, int n)
{
    int64_t u = n - j;
    int64_t numerator = u * u * u * p0 + 3 * u * u * j * p1 + 3 * u * j * j * p2 + (int64_t)j * j * j * p3;
    int64_t cube = (int64_t)n * n * n;
    int64_t error = cube * value - numerator;
    return 2 * (error < 0 ? -error : error) <= cube;
}

static void test_segments(void)
{
    static const uint8_t STEPS[] = {1, 2, 3, 4, 7, 8, 16, 31, 32};
    static recorded_segments_t segs, mirrored; /* static: too large for a small stack */
    size_t s;
    int d, i, k, j;

    TEST_SECTION("glyph segments");
    for (s = 0; s < sizeof STEPS / sizeof STEPS[0]; s++) {
        int n = STEPS[s];
        for (d = 1; d <= FRC_MAX_STROKES; d += (d == 3 ? 2 : 1)) { /* 1, 2, 3, 5 strokes */
            int16_t min_x, min_y, max_x, max_y;
            frc_glyph_bounds((uint8_t)d, &min_x, &min_y, &max_x, &max_y);
            for (i = 0; i < frc_glyph_states((uint8_t)d); i++) {
                frc_glyph_t g;
                int at = 0, expected = d;

                CHECK(frc_glyph_from_index((uint16_t)i, (uint8_t)d, &g) == FRC_OK);
                segs.count = 0;
                frc_glyph_segments(&g, (uint8_t)n, record_segment, &segs);
                for (k = 0; k < d; k++) {
                    expected += g.trit[k] == 0 ? 1 : 2 * n;
                }
                CHECK_EQ_I64(segs.count, expected); /* the dot is never a segment */
                if (segs.count != expected || segs.count > MAX_SEGMENTS) {
                    continue;
                }
                /* the stem, one piece per trit */
                for (k = 0; k < d; k++, at++) {
                    CHECK(segs.seg[at][0] == 0 && segs.seg[at][1] == 300 * k && segs.seg[at][2] == 0 &&
                          segs.seg[at][3] == 300 * k + 300);
                }
                for (k = 0; k < d; k++) {
                    int y = 300 * k, t = g.trit[k];
                    if (t == 0) {
                        CHECK(segs.seg[at][0] == -100 && segs.seg[at][1] == y && segs.seg[at][2] == 100 &&
                              segs.seg[at][3] == y);
                        at++;
                        continue;
                    }
                    /* two cubics of n segments each: a chain from the junction to the tip */
                    CHECK(segs.seg[at][0] == 0 && segs.seg[at][1] == y);
                    for (j = 1; j < 2 * n; j++) {
                        CHECK(segs.seg[at + j][0] == segs.seg[at + j - 1][2] &&
                              segs.seg[at + j][1] == segs.seg[at + j - 1][3]);
                    }
                    CHECK(segs.seg[at + n - 1][2] == 100 * t && segs.seg[at + n - 1][3] == y + 100 * t);
                    CHECK(segs.seg[at + 2 * n - 1][2] == 200 * t && segs.seg[at + 2 * n - 1][3] == y);
                    /* every vertex is the cubic of S9.1 at j / n, rounded to the nearest unit */
                    for (j = 1; j <= n; j++) {
                        const int16_t *a = segs.seg[at + j - 1], *b = segs.seg[at + n + j - 1];
                        CHECK(on_cubic(a[2], 0, 0, 0, 100 * t, j, n));
                        CHECK(on_cubic(a[3], y, y, y + 100 * t, y + 100 * t, j, n));
                        CHECK(on_cubic(b[2], 100 * t, 200 * t, 200 * t, 200 * t, j, n));
                        CHECK(on_cubic(b[3], y + 100 * t, y + 100 * t, y, y, j, n));
                        CHECK_NOTE("steps %d strokes %d index %d trit %d vertex %d", n, d, i, k, j);
                    }
                    at += 2 * n;
                }
                for (k = 0; k < segs.count; k++) {
                    CHECK(segs.seg[k][0] >= min_x + 15 && segs.seg[k][2] <= max_x - 15 && segs.seg[k][0] <= max_x - 15 &&
                          segs.seg[k][2] >= min_x + 15);
                    CHECK(segs.seg[k][1] >= min_y + 15 && segs.seg[k][3] <= max_y - 15 && segs.seg[k][1] <= max_y - 15 &&
                          segs.seg[k][3] >= min_y + 15);
                }
            }
        }
    }

    TEST_SECTION("glyph segments: the -1 stroke mirrors the +1 stroke");
    for (s = 0; s < sizeof STEPS / sizeof STEPS[0]; s++) {
        frc_glyph_t plus, minus;
        /* one stroke: value +1 is trit +1, value -1 is trit -1 (indices 1 and 3 of 4) */
        CHECK(frc_glyph_from_index(1, 1, &plus) == FRC_OK);
        CHECK(frc_glyph_from_index(3, 1, &minus) == FRC_OK);
        CHECK(plus.trit[0] == 1 && minus.trit[0] == -1);
        segs.count = 0;
        mirrored.count = 0;
        frc_glyph_segments(&plus, STEPS[s], record_segment, &segs);
        frc_glyph_segments(&minus, STEPS[s], record_segment, &mirrored);
        CHECK_EQ_I64(segs.count, mirrored.count);
        for (k = 1; k < segs.count && k < MAX_SEGMENTS; k++) { /* segment 0 is the stem */
            CHECK(mirrored.seg[k][0] == -segs.seg[k][0] && mirrored.seg[k][1] == -segs.seg[k][1] &&
                  mirrored.seg[k][2] == -segs.seg[k][2] && mirrored.seg[k][3] == -segs.seg[k][3]);
        }
    }

    TEST_SECTION("glyph segments: step count limits and bad input");
    {
        frc_glyph_t g;
        CHECK(frc_glyph_from_index(1, 1, &g) == FRC_OK); /* one stem piece, one curve */
        segs.count = 0;
        frc_glyph_segments(&g, 0, record_segment, &segs); /* below the range: 1 */
        CHECK_EQ_I64(segs.count, 1 + 2);
        /* one segment per cubic is its chord: junction -> bend -> tip */
        CHECK(segs.seg[1][0] == 0 && segs.seg[1][1] == 0 && segs.seg[1][2] == 100 && segs.seg[1][3] == 100);
        CHECK(segs.seg[2][0] == 100 && segs.seg[2][1] == 100 && segs.seg[2][2] == 200 && segs.seg[2][3] == 0);
        segs.count = 0;
        frc_glyph_segments(&g, 33, record_segment, &segs); /* above the range: 32 */
        CHECK_EQ_I64(segs.count, 1 + 64);
        segs.count = 0;
        frc_glyph_segments(&g, 255, record_segment, &segs);
        CHECK_EQ_I64(segs.count, 1 + 64);

        segs.count = 0;
        frc_glyph_segments(NULL, 8, record_segment, &segs);
        frc_glyph_segments(&g, 8, NULL, &segs);
        g.strokes = 0;
        frc_glyph_segments(&g, 8, record_segment, &segs);
        g.strokes = FRC_MAX_STROKES + 1;
        frc_glyph_segments(&g, 8, record_segment, &segs);
        g.strokes = 1;
        g.trit[0] = -2;
        frc_glyph_segments(&g, 8, record_segment, &segs);
        CHECK_EQ_I64(segs.count, 0);
    }
}


/* ------------------------------------------------------------------------- */
/* frc_glyph_at_quarter                                                        */
/* ------------------------------------------------------------------------- */

static void test_glyph_at_quarter(void)
{
    frc_glyph_t g, state;
    uint8_t strokes, q, level;

    TEST_SECTION("frc_glyph_at_quarter: the state at each threshold (S6.2, S4.1)");
    for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
        uint16_t m = frc_glyph_extreme(strokes);
        for (q = 0; q <= FRC_END; q++) {
            uint16_t want = (uint16_t)((m * (q + 1u)) % (4u * m));
            CHECK(frc_glyph_at_quarter((frc_quarter_t)q, strokes, &g) == FRC_OK);
            CHECK_EQ_U64(g.index, want);
            CHECK(frc_glyph_from_index(want, strokes, &state) == FRC_OK);
            CHECK(g.value == state.value && g.derivative == state.derivative && g.dotted == state.dotted);
        }
        CHECK(frc_glyph_at_quarter(FRC_PEAK, strokes, &g) == FRC_OK);
        CHECK(g.value == (int16_t)m && g.derivative == 0 && !g.dotted);
        CHECK(frc_glyph_at_quarter(FRC_NODE, strokes, &g) == FRC_OK);
        CHECK(g.value == 0 && g.derivative == -1 && g.dotted);
        CHECK(frc_glyph_at_quarter(FRC_VALLEY, strokes, &g) == FRC_OK);
        CHECK(g.value == -(int16_t)m && g.derivative == 0 && !g.dotted);
        CHECK(frc_glyph_at_quarter(FRC_END, strokes, &g) == FRC_OK);
        CHECK(g.value == 0 && g.derivative == 1 && !g.dotted && g.index == 0);
    }
    /* the lenses' table at 2 strokes: End 0, Peak 4, Node 8, Valley 12 */
    CHECK(frc_glyph_at_quarter(FRC_END, 2, &g) == FRC_OK && g.index == 0);
    CHECK(frc_glyph_at_quarter(FRC_PEAK, 2, &g) == FRC_OK && g.index == 4);
    CHECK(frc_glyph_at_quarter(FRC_NODE, 2, &g) == FRC_OK && g.index == 8);
    CHECK(frc_glyph_at_quarter(FRC_VALLEY, 2, &g) == FRC_OK && g.index == 12);
    /* and what the clock shows at the threshold instants it returns */
    for (level = 0; level <= 10; level++) {
        frc_threshold_t th;
        frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
        int k;
        for (k = 0; k < 8; k++) {
            CHECK(frc_next_threshold(CAT, t, level, &th) == FRC_OK);
            for (strokes = 1; strokes <= FRC_MAX_STROKES; strokes++) {
                CHECK(frc_glyph_at(CAT, th.time, level, strokes, &state) == FRC_OK);
                CHECK(frc_glyph_at_quarter((frc_quarter_t)th.name, strokes, &g) == FRC_OK);
                CHECK_EQ_U64(state.index, g.index);
            }
            t = th.time;
        }
    }
    /* arguments: nothing written */
    g.index = 77;
    CHECK(frc_glyph_at_quarter((frc_quarter_t)4, 2, &g) == FRC_ERR_ARG && g.index == 77);
    CHECK(frc_glyph_at_quarter((frc_quarter_t)-1, 2, &g) == FRC_ERR_ARG && g.index == 77);
    CHECK(frc_glyph_at_quarter(FRC_PEAK, 0, &g) == FRC_ERR_ARG && g.index == 77);
    CHECK(frc_glyph_at_quarter(FRC_PEAK, FRC_MAX_STROKES + 1, &g) == FRC_ERR_ARG && g.index == 77);
    CHECK(frc_glyph_at_quarter(FRC_PEAK, 2, NULL) == FRC_ERR_ARG);
}

int main(void)
{
    test_extreme_and_states();
    test_from_index();
    test_index_helpers();
    test_spec_vectors();
    test_samples_against_oracle();
    test_steps_against_foundation_oracle();
    test_odometer_properties();
    test_odometer_of_phase();
    test_catalogue_edges();
    test_path_against_oracle();
    test_path_structure();
    test_segments();
    test_glyph_at_quarter();
    return TEST_SUMMARY("glyph");
}
