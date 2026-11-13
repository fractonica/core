/*
 * test_ladder.c - tests of the spectral ladder (SPEC S16) and of the big
 * integer it stands on:
 *
 *   frc_big.c     384-bit unsigned integer   (against slow reference arithmetic written here)
 *   frc_ladder.c  intervals, periods, frequencies, lengths, media, bands,
 *                 devices, the texts of S16.1, the doubles of S16.4
 *
 * Expected values come from three places, in this order of authority:
 *   1. SPEC.md, typed in here by hand: the vectors of S16.5 and the figures in
 *      the prose of S16.3.
 *   2. tests/vectors/oracle_ladder.h, generated from SPEC.md and the frozen
 *      JavaScript reference by tools/oracle/generate-ladder.mjs. EVERY row of
 *      every table that concerns frc_ladder.c is checked.
 *   3. Properties that hold whatever the values are: a 17-digit result is
 *      within half a unit of the exact quotient, a double equals the C
 *      library's conversion of a 37-digit decimal of the exact quotient.
 *
 * The file compiles in every variant of `make check-variants`: the float-only
 * API is guarded by FRC_NO_FLOAT.
 */
#include "frc_test.h"

#include <stdlib.h>

#include "fractonica.h"
#include "frc_big.h"
#include "vectors/oracle_ladder.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static const frc_catalogue_t *const CAT = &FRC_SAROS_141;

/* UTF-8 pieces of the expected texts */
#define TIMES "\303\227" /* × U+00D7 */
#define MICRO "\302\265" /* µ U+00B5 */
#define DASH  "\342\200\224" /* — U+2014 */

/* splitmix64: a fixed-seed generator, so a failure reproduces. */
static uint64_t rng_state = UINT64_C(0x5EC7A11ADDE50001);
static uint64_t rng(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}
/* A random NON-ZERO value of a random width, so small and large operands both occur. */
static uint64_t rng_any_width(void)
{
    uint64_t v = rng() >> (rng() % 64);
    return v != 0 ? v : 1;
}

static frc_interval_t interval_of(unsigned index)
{
    frc_interval_t interval;
    interval.num = oracle_ladder_intervals[index].num;
    interval.den = oracle_ladder_intervals[index].den;
    return interval;
}

static frc_interval_t interval(uint64_t num, uint64_t den)
{
    frc_interval_t i;
    i.num = num;
    i.den = den;
    return i;
}

static frc_speed_t speed(uint64_t num, uint64_t den)
{
    frc_speed_t s;
    s.num = num;
    s.den = den;
    return s;
}

static const frc_interval_t REFERENCE = {FRC_INTERVAL_REFERENCE_MS, 1};

#define CHECK_SCI(actual, expected)                                             \
    do {                                                                        \
        CHECK_EQ_U64((actual).mantissa, (expected).mantissa);                   \
        CHECK_EQ_I64((actual).exp10, (expected).exp10);                         \
    } while (0)

#ifndef FRC_NO_FLOAT
static uint64_t bits_of(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}
#endif

/* ------------------------------------------------------------------------- */
/* Reference big arithmetic: slow, obvious, independent of frc_big.c          */
/* ------------------------------------------------------------------------- */

static void ref_zero(frc_i_big_t *a)
{
    memset(a, 0, sizeof *a);
}

/* A random value with exactly `bits` significant bits (0 gives zero). */
static void ref_random(frc_i_big_t *a, unsigned bits)
{
    unsigned i;
    ref_zero(a);
    if (bits == 0) {
        return;
    }
    for (i = 0; i < FRC_I_BIG_LIMBS; i++) {
        a->limb[i] = (uint32_t)rng();
    }
    for (i = bits; i < FRC_I_BIG_BITS; i++) {
        a->limb[i / 32] &= ~((uint32_t)1 << (i % 32));
    }
    a->limb[(bits - 1) / 32] |= (uint32_t)1 << ((bits - 1) % 32);
}

/* out = a * b mod 2^384, one bit of b at a time. */
static void ref_add(frc_i_big_t *a, const frc_i_big_t *b)
{
    uint64_t carry = 0;
    unsigned i;
    for (i = 0; i < FRC_I_BIG_LIMBS; i++) {
        carry += (uint64_t)a->limb[i] + b->limb[i];
        a->limb[i] = (uint32_t)carry;
        carry >>= 32;
    }
}

static void ref_double(frc_i_big_t *a)
{
    frc_i_big_t copy = *a;
    ref_add(a, &copy);
}

static void ref_mul(const frc_i_big_t *a, const frc_i_big_t *b, frc_i_big_t *out)
{
    frc_i_big_t term = *a;
    unsigned i;
    ref_zero(out);
    for (i = 0; i < FRC_I_BIG_BITS; i++) {
        if ((b->limb[i / 32] >> (i % 32)) & 1u) {
            ref_add(out, &term);
        }
        ref_double(&term);
    }
}

/* a = a - b mod 2^384 */
static void ref_sub(frc_i_big_t *a, const frc_i_big_t *b)
{
    int64_t borrow = 0;
    unsigned i;
    for (i = 0; i < FRC_I_BIG_LIMBS; i++) {
        int64_t v = (int64_t)a->limb[i] - (int64_t)b->limb[i] - borrow;
        borrow = v < 0;
        a->limb[i] = (uint32_t)(v + (borrow ? ((int64_t)1 << 32) : 0));
    }
}

/* a = floor(a / divisor) for a 32-bit divisor; returns the remainder. */
static uint32_t ref_div_small(frc_i_big_t *a, uint32_t divisor)
{
    uint64_t rem = 0;
    int i;
    for (i = FRC_I_BIG_LIMBS - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | a->limb[i];
        a->limb[i] = (uint32_t)(cur / divisor);
        rem = cur % divisor;
    }
    return (uint32_t)rem;
}

static int ref_is_zero(const frc_i_big_t *a)
{
    unsigned i;
    for (i = 0; i < FRC_I_BIG_LIMBS; i++) {
        if (a->limb[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int ref_equal(const frc_i_big_t *a, const frc_i_big_t *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/* Decimal text of a (up to 116 digits + NUL). */
static void ref_decimal(const frc_i_big_t *a, char out[128])
{
    frc_i_big_t rest = *a;
    char digits[128];
    int n = 0, i;
    do {
        digits[n++] = (char)('0' + ref_div_small(&rest, 10));
    } while (!ref_is_zero(&rest));
    for (i = 0; i < n; i++) {
        out[i] = digits[n - 1 - i];
    }
    out[n] = '\0';
}

static void big_u64(frc_i_big_t *a, uint64_t value)
{
    ref_zero(a);
    a->limb[0] = (uint32_t)value;
    a->limb[1] = (uint32_t)(value >> 32);
}

/* a *= 10^exponent with the library multiplication (tested below). */
static void big_mul_pow10(frc_i_big_t *a, int exponent)
{
    while (exponent-- > 0) {
        frc_i_big_mul_u64(a, a, 10);
    }
}

/* ------------------------------------------------------------------------- */
/* frc_big.c                                                                  */
/* ------------------------------------------------------------------------- */

static void test_big_basics(void)
{
    frc_i_big_t a, b;
    unsigned i;

    TEST_SECTION("big: set, low, bits, cmp");
    memset(&a, 0xA5, sizeof a); /* set must write every limb */
    frc_i_big_set_u64(&a, 0);
    CHECK(ref_is_zero(&a));
    CHECK_EQ_U64(frc_i_big_bits(&a), 0);
    CHECK_EQ_U64(frc_i_big_low_u64(&a), 0);

    memset(&a, 0xA5, sizeof a);
    frc_i_big_set_u64(&a, UINT64_C(0xFEDCBA9876543210));
    big_u64(&b, UINT64_C(0xFEDCBA9876543210));
    CHECK(ref_equal(&a, &b));
    CHECK_EQ_U64(frc_i_big_low_u64(&a), UINT64_C(0xFEDCBA9876543210));
    CHECK_EQ_U64(frc_i_big_bits(&a), 64);

    frc_i_big_set_u64(&a, 1);
    CHECK_EQ_U64(frc_i_big_bits(&a), 1);
    frc_i_big_set_u64(&a, UINT64_C(0x100000000));
    CHECK_EQ_U64(frc_i_big_bits(&a), 33);

    for (i = 0; i <= FRC_I_BIG_BITS; i++) {
        ref_random(&a, i);
        CHECK_EQ_U64(frc_i_big_bits(&a), i);
    }

    TEST_SECTION("big: cmp");
    for (i = 0; i < 2000; i++) {
        unsigned bits_a = (unsigned)(rng() % (FRC_I_BIG_BITS + 1));
        unsigned bits_b = (unsigned)(rng() % (FRC_I_BIG_BITS + 1));
        ref_random(&a, bits_a);
        ref_random(&b, bits_b);
        CHECK_EQ_I64(frc_i_big_cmp(&a, &a), 0);
        if (bits_a != bits_b) {
            CHECK_EQ_I64(frc_i_big_cmp(&a, &b), bits_a < bits_b ? -1 : 1);
            CHECK_EQ_I64(frc_i_big_cmp(&b, &a), bits_a < bits_b ? 1 : -1);
        } else if (!ref_equal(&a, &b)) {
            /* same length: a + 1 > a, and the order is antisymmetric */
            CHECK_EQ_I64(frc_i_big_cmp(&a, &b), -frc_i_big_cmp(&b, &a));
        }
    }
    /* a difference in the lowest limb only, under equal high limbs */
    ref_random(&a, 300);
    b = a;
    b.limb[0] ^= 1u;
    CHECK_EQ_I64(frc_i_big_cmp(&a, &b), (a.limb[0] & 1u) ? 1 : -1);
}

static void test_big_mul(void)
{
    frc_i_big_t a, b, m, expected, product;
    char text[128];
    unsigned level, i;

    TEST_SECTION("big: mul builds P(0..63) of SPEC S16.4");
    frc_i_big_set_u64(&a, 1);
    for (level = 0; level < ORACLE_LADDER_PREFIX_COUNT; level++) {
        ref_decimal(&a, text);
        CHECK_STR(text, oracle_ladder_prefix[level]);
        frc_i_big_mul_u64(&a, &a, frc_factor((uint8_t)level));
    }
    /* P(62) has 172 bits (S16.4) */
    frc_i_big_set_u64(&a, 1);
    for (level = 0; level < 62; level++) {
        frc_i_big_mul_u64(&a, &a, frc_factor((uint8_t)level));
    }
    CHECK_EQ_U64(frc_i_big_bits(&a), 172);

    TEST_SECTION("big: mul by any 64-bit value");
    for (i = 0; i < 3000; i++) {
        uint64_t factor = (i % 7 == 0) ? UINT64_MAX : rng() >> (rng() % 64);
        ref_random(&a, (unsigned)(rng() % 321)); /* product fits: 320 + 64 bits */
        big_u64(&m, factor);
        ref_mul(&a, &m, &expected);
        frc_i_big_mul_u64(&product, &a, factor);
        CHECK(ref_equal(&product, &expected));
        b = a;
        frc_i_big_mul_u64(&b, &b, factor); /* in place */
        CHECK(ref_equal(&b, &expected));
    }
    /* every limb at its largest times the largest factor: the carry chain */
    memset(&a, 0xFF, sizeof a);
    a.limb[FRC_I_BIG_LIMBS - 1] = 0;
    a.limb[FRC_I_BIG_LIMBS - 2] = 0;
    big_u64(&m, UINT64_MAX);
    ref_mul(&a, &m, &expected);
    frc_i_big_mul_u64(&a, &a, UINT64_MAX);
    CHECK(ref_equal(&a, &expected));
}

static void test_big_shl(void)
{
    frc_i_big_t a, b, power, expected, shifted;
    unsigned i;

    TEST_SECTION("big: shl");
    for (i = 0; i < 3000; i++) {
        unsigned bits = (unsigned)(rng() % 200);
        unsigned shift = (unsigned)(rng() % 185); /* 199 + 184 < 384 */
        ref_random(&a, bits);
        ref_zero(&power);
        power.limb[shift / 32] = (uint32_t)1 << (shift % 32);
        ref_mul(&a, &power, &expected);
        frc_i_big_shl(&shifted, &a, (uint16_t)shift);
        CHECK(ref_equal(&shifted, &expected));
        b = a;
        frc_i_big_shl(&b, &b, (uint16_t)shift); /* in place */
        CHECK(ref_equal(&b, &expected));
        if (bits != 0) {
            CHECK_EQ_U64(frc_i_big_bits(&shifted), bits + shift);
        }
    }
    /* shifts by 0, by whole limbs, and out of the top */
    ref_random(&a, 100);
    frc_i_big_shl(&shifted, &a, 0);
    CHECK(ref_equal(&shifted, &a));
    frc_i_big_shl(&shifted, &a, 64);
    CHECK_EQ_U64(frc_i_big_bits(&shifted), 164);
    CHECK_EQ_U64(frc_i_big_low_u64(&shifted), 0);
    frc_i_big_shl(&shifted, &a, 383);
    CHECK_EQ_U64(frc_i_big_bits(&shifted), (a.limb[0] & 1u) ? 384 : 0);
    frc_i_big_shl(&shifted, &a, 384);
    CHECK(ref_is_zero(&shifted));
    frc_i_big_shl(&shifted, &a, 1000);
    CHECK(ref_is_zero(&shifted));
}

static void test_big_divmod(void)
{
    frc_i_big_t n, d, q, r, got_q, got_r, zero;
    unsigned i;

    TEST_SECTION("big: divmod gives back q and r of n = q * d + r");
    ref_zero(&zero);
    for (i = 0; i < 3000; i++) {
        unsigned bits_d = 1 + (unsigned)(rng() % 383);
        unsigned bits_q = (unsigned)(rng() % (FRC_I_BIG_BITS - bits_d)); /* q * d + r < 2^384 */
        ref_random(&d, bits_d);
        ref_random(&q, bits_q);
        ref_random(&r, (unsigned)(rng() % bits_d)); /* fewer bits than d: r < d */
        ref_mul(&q, &d, &n);
        ref_add(&n, &r);
        frc_i_big_divmod(&n, &d, &got_q, &got_r);
        CHECK(ref_equal(&got_q, &q));
        CHECK(ref_equal(&got_r, &r));
        CHECK_NOTE("case %u: d has %u bits, q %u", i, bits_d, bits_q);
    }

    TEST_SECTION("big: divmod edges");
    ref_random(&d, 200);
    frc_i_big_divmod(&zero, &d, &got_q, &got_r); /* 0 / d */
    CHECK(ref_is_zero(&got_q));
    CHECK(ref_is_zero(&got_r));
    frc_i_big_divmod(&d, &d, &got_q, &got_r); /* d / d */
    CHECK_EQ_U64(frc_i_big_low_u64(&got_q), 1);
    CHECK_EQ_U64(frc_i_big_bits(&got_q), 1);
    CHECK(ref_is_zero(&got_r));
    ref_random(&n, 199);
    frc_i_big_divmod(&n, &d, &got_q, &got_r); /* n < d */
    CHECK(ref_is_zero(&got_q));
    CHECK(ref_equal(&got_r, &n));
    /* the widest operands: all 384 bits set, divided by itself minus one and by 1 */
    memset(&n, 0xFF, sizeof n);
    d = n;
    d.limb[0] = 0xFFFFFFFEu;
    frc_i_big_divmod(&n, &d, &got_q, &got_r);
    CHECK_EQ_U64(frc_i_big_low_u64(&got_q), 1);
    CHECK_EQ_U64(frc_i_big_bits(&got_q), 1);
    CHECK_EQ_U64(frc_i_big_low_u64(&got_r), 1);
    CHECK_EQ_U64(frc_i_big_bits(&got_r), 1);
    big_u64(&d, 1);
    frc_i_big_divmod(&n, &d, &got_q, &got_r);
    CHECK(ref_equal(&got_q, &n));
    CHECK(ref_is_zero(&got_r));
    /* against the reference short division */
    for (i = 0; i < 500; i++) {
        uint32_t small = (uint32_t)rng() | 1u;
        uint32_t rem;
        ref_random(&n, (unsigned)(rng() % 385));
        big_u64(&d, small);
        frc_i_big_divmod(&n, &d, &got_q, &got_r);
        rem = ref_div_small(&n, small);
        CHECK(ref_equal(&got_q, &n));
        CHECK_EQ_U64(frc_i_big_low_u64(&got_r), rem);
    }
    /* d == 0 is outside the contract, but must come back */
    ref_random(&n, 384);
    frc_i_big_divmod(&n, &zero, &got_q, &got_r);
    CHECK(1);
}

static void test_big_cmp_half(void)
{
    frc_i_big_t d, half, r, one;
    unsigned i;

    TEST_SECTION("big: cmp_half");
    big_u64(&one, 1);
    for (i = 0; i < 2000; i++) {
        unsigned bits = 2 + (unsigned)(rng() % 383);
        int odd;
        ref_random(&d, bits);
        odd = (int)(d.limb[0] & 1u);
        half = d;
        (void)ref_div_small(&half, 2); /* floor(d / 2) */
        /* even d: floor(d/2) is exactly half. odd d: it is just below half. */
        CHECK_EQ_I64(frc_i_big_cmp_half(&half, &d), odd ? -1 : 0);
        r = half;
        ref_add(&r, &one); /* floor(d/2) + 1 < d because d >= 2... and d >= 3 or r == d */
        if (frc_i_big_cmp(&r, &d) < 0) {
            CHECK_EQ_I64(frc_i_big_cmp_half(&r, &d), 1);
        }
        r = half;
        ref_sub(&r, &one);
        CHECK_EQ_I64(frc_i_big_cmp_half(&r, &d), -1);
        CHECK_NOTE("case %u, %u bits", i, bits);
    }
    ref_zero(&r);
    ref_random(&d, 384);
    CHECK_EQ_I64(frc_i_big_cmp_half(&r, &d), -1); /* a remainder of zero */
    /* r = 2^383 < d = 2^384 - 1: twice r does not fit, and is above d */
    ref_zero(&r);
    r.limb[FRC_I_BIG_LIMBS - 1] = 0x80000000u;
    memset(&d, 0xFF, sizeof d);
    CHECK_EQ_I64(frc_i_big_cmp_half(&r, &d), 1);
}

/* ------------------------------------------------------------------------- */
/* Intervals                                                                  */
/* ------------------------------------------------------------------------- */

/* 1 when the two intervals are the same VALUE (a/b == c/d), exact. */
static int same_interval(frc_interval_t a, uint64_t num, uint64_t den)
{
    frc_i_u128_t left, right;
    frc_i_mul64(a.num, den, &left);
    frc_i_mul64(num, a.den, &right);
    return frc_i_cmp128(&left, &right) == 0;
}

static void test_intervals(void)
{
    frc_interval_t got, mean;
    frc_catalogue_t custom;
    static const int64_t TWO_ECLIPSES[2] = {-1000, 2001};
    unsigned k;

    TEST_SECTION("frc_interval_at: every Saros 141 interval");
    for (k = 0; k + 1 < CAT->count; k++) {
        const oracle_ladder_interval_t *expected =
            &oracle_ladder_intervals[ORACLE_LADDER_INTERVAL_SAROS141_FIRST + k];
        frc_time_t start = frc_catalogue_instant(CAT, (uint16_t)k);
        frc_time_t end = frc_catalogue_instant(CAT, (uint16_t)(k + 1));
        frc_time_t at[4];
        unsigned j;
        at[0] = start;           /* an eclipse belongs to the interval it opens */
        at[1] = start + 1;
        at[2] = start + (end - start) / 2;
        at[3] = end - 1;
        for (j = 0; j < 4; j++) {
            got = interval(0, 0);
            CHECK_EQ_I64(frc_interval_at(CAT, at[j], &got), FRC_OK);
            CHECK_EQ_U64(got.num, expected->num);
            CHECK_EQ_U64(got.den, expected->den);
            CHECK_NOTE("interval %u, probe %u", k, j);
        }
    }

    TEST_SECTION("frc_interval_at: edges");
    /* the closing instant belongs to the last interval (S16.1) */
    CHECK_EQ_I64(frc_interval_at(CAT, frc_catalogue_last(CAT), &got), FRC_OK);
    CHECK_EQ_U64(got.num, oracle_ladder_intervals[ORACLE_LADDER_INTERVAL_SAROS141_FIRST + 68].num);
    CHECK_EQ_U64(got.den, 1);
    CHECK_EQ_I64(frc_interval_at(CAT, frc_catalogue_first(CAT), &got), FRC_OK);
    CHECK_EQ_U64(got.num, oracle_ladder_intervals[ORACLE_LADDER_INTERVAL_SAROS141_FIRST].num);
    /* outside the catalogue there is no interval and no ladder */
    CHECK_EQ_I64(frc_interval_at(CAT, frc_catalogue_first(CAT) - 1, &got), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_interval_at(CAT, frc_catalogue_last(CAT) + 1, &got), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_interval_at(CAT, INT64_MIN, &got), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_interval_at(CAT, INT64_MAX, &got), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_interval_at(NULL, 0, &got), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_interval_at(CAT, 0, NULL), FRC_ERR_ARG);
    /* the reference interval is the one that holds SPEC's vector A (2026) */
    CHECK_EQ_I64(frc_interval_at(CAT, FRC_TIME_FROM_S(INT64_C(1791460800)), &got), FRC_OK);
    CHECK_EQ_U64(got.num, FRC_INTERVAL_REFERENCE_MS);
    CHECK_EQ_U64(got.den, 1);
    CHECK_EQ_U64(oracle_ladder_intervals[ORACLE_LADDER_INTERVAL_REFERENCE].num, FRC_INTERVAL_REFERENCE_MS);

    TEST_SECTION("frc_interval_mean");
    mean = frc_interval_mean(CAT);
    /* SPEC S16.5: 39,259,007,522,784 / 69 ms */
    CHECK(same_interval(mean, UINT64_C(39259007522784), 69));
    CHECK(same_interval(mean, oracle_ladder_intervals[ORACLE_LADDER_INTERVAL_MEAN].num,
                        oracle_ladder_intervals[ORACLE_LADDER_INTERVAL_MEAN].den));
    CHECK(mean.den != 0);
    mean = frc_interval_mean(NULL);
    CHECK_EQ_U64(mean.num, 0);
    CHECK_EQ_U64(mean.den, 0);
    CHECK_EQ_I64(frc_catalogue_init(&custom, TWO_ECLIPSES, 2, 0), FRC_OK);
    mean = frc_interval_mean(&custom);
    CHECK(same_interval(mean, 3001, 1));
    CHECK_EQ_I64(frc_interval_at(&custom, 0, &got), FRC_OK);
    CHECK_EQ_U64(got.num, 3001);

    TEST_SECTION("frc_interval_of_alpha_ms");
    for (k = 0; k < ORACLE_LADDER_INTERVAL_COUNT; k++) {
        const oracle_ladder_interval_t *v = &oracle_ladder_intervals[k];
        if (v->alpha_ms != 0) {
            CHECK(same_interval(frc_interval_of_alpha_ms(v->alpha_ms), v->num, v->den));
        }
    }
    CHECK(same_interval(frc_interval_of_alpha_ms(4 * FRC_INTERVAL_REFERENCE_MS), FRC_INTERVAL_REFERENCE_MS, 1));
    CHECK(same_interval(frc_interval_of_alpha_ms(UINT64_MAX), UINT64_MAX, 4));
}

/* ------------------------------------------------------------------------- */
/* The ladder: 17 digits, bands, doubles                                      */
/* ------------------------------------------------------------------------- */

static void test_media(void)
{
    char buf[64];
    unsigned m;

    TEST_SECTION("media of SPEC S16.3");
    CHECK_EQ_U64(ORACLE_LADDER_MEDIUM_COUNT, FRC_MEDIUM_COUNT);
    for (m = 0; m < FRC_MEDIUM_COUNT; m++) {
        const oracle_ladder_medium_t *v = &oracle_ladder_media[m];
        frc_speed_t s = frc_medium_speed((frc_medium_t)m);
        CHECK_EQ_U64(s.num, v->speed_num);
        CHECK_EQ_U64(s.den, v->speed_den);
        CHECK_EQ_I64(frc_format_medium_id((frc_medium_t)m, buf, sizeof buf), (int)strlen(v->id));
        CHECK_STR(buf, v->id);
        CHECK_EQ_I64(frc_format_medium_name((frc_medium_t)m, buf, sizeof buf), (int)strlen(v->name));
        CHECK_STR(buf, v->name);
        CHECK_EQ_I64(frc_format_medium_phase((frc_medium_t)m, buf, sizeof buf), (int)strlen(v->phase));
        CHECK_STR(buf, v->phase);
    }
    /* typed in from SPEC S16.3 */
    CHECK_EQ_U64(frc_medium_speed(FRC_MEDIUM_VACUUM).num, 299792458);
    CHECK_EQ_U64(frc_medium_speed(FRC_MEDIUM_AIR).num, 343);
    CHECK_EQ_U64(frc_medium_speed(FRC_MEDIUM_CHEMICAL).num, 1);
    CHECK_EQ_U64(frc_medium_speed(FRC_MEDIUM_CHEMICAL).den, 1000);
    frc_format_medium_name(FRC_MEDIUM_BOUNDED, buf, sizeof buf);
    CHECK_STR(buf, "Light in bounded medium");
    frc_format_medium_phase(FRC_MEDIUM_CHEMICAL, buf, sizeof buf);
    CHECK_STR(buf, "Liquid + gas + solid");

    TEST_SECTION("media: invalid arguments");
    CHECK_EQ_U64(frc_medium_speed(FRC_MEDIUM_COUNT).num, 0);
    CHECK_EQ_U64(frc_medium_speed(FRC_MEDIUM_COUNT).den, 0);
    CHECK_EQ_U64(frc_medium_speed((frc_medium_t)200).den, 0);
    snprintf(buf, sizeof buf, "junk");
    CHECK_EQ_I64(frc_format_medium_id(FRC_MEDIUM_COUNT, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_STR(buf, "");
    CHECK_EQ_I64(frc_format_medium_name((frc_medium_t)99, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_medium_phase(FRC_MEDIUM_COUNT, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_medium_id(FRC_MEDIUM_VACUUM, NULL, 0), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_format_medium_id(FRC_MEDIUM_VACUUM, buf, 0), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_format_medium_id(FRC_MEDIUM_VACUUM, buf, 6), FRC_ERR_BUFFER);
    CHECK_STR(buf, "vacuu");
    CHECK_EQ_I64(frc_format_medium_id(FRC_MEDIUM_VACUUM, buf, 7), 6);
    CHECK_STR(buf, "vacuum");
}

static void test_full_ladder(void)
{
    unsigned i, m;

    TEST_SECTION("oracle_ladder_full: 63 levels on the reference and the mean interval");
    for (i = 0; i < ORACLE_LADDER_FULL_COUNT; i++) {
        const oracle_ladder_full_t *v = &oracle_ladder_full[i];
        frc_interval_t iv = interval_of(v->interval);
        frc_sci_t got;

        CHECK_EQ_I64(frc_period(v->level, iv, &got), FRC_OK);
        CHECK_SCI(got, v->period);
        CHECK_NOTE("period, interval %u level %u", v->interval, v->level);
        CHECK_EQ_I64(frc_frequency(v->level, iv, &got), FRC_OK);
        CHECK_SCI(got, v->frequency);
        CHECK_NOTE("frequency, interval %u level %u", v->interval, v->level);
        for (m = 0; m < FRC_MEDIUM_COUNT; m++) {
            CHECK_EQ_I64(frc_length(v->level, iv, frc_medium_speed((frc_medium_t)m), &got), FRC_OK);
            CHECK_SCI(got, v->length[m]);
            CHECK_NOTE("length, interval %u level %u medium %u", v->interval, v->level, m);
        }
        CHECK_EQ_I64(frc_band(v->level, iv), v->band);
        CHECK_NOTE("band, interval %u level %u", v->interval, v->level);
#ifndef FRC_NO_FLOAT
        CHECK_EQ_U64(bits_of(frc_period_seconds(v->level, iv)), v->period_bits);
        CHECK_NOTE("period double, interval %u level %u", v->interval, v->level);
        CHECK_EQ_U64(bits_of(frc_frequency_hz(v->level, iv)), v->frequency_bits);
        CHECK_NOTE("frequency double, interval %u level %u", v->interval, v->level);
        CHECK_EQ_U64(bits_of(frc_length_metres(v->level, iv, frc_medium_speed(FRC_MEDIUM_VACUUM))),
                     v->length_bits);
        CHECK_NOTE("length double, interval %u level %u", v->interval, v->level);
#endif
    }
}

static void test_spot_ladder(void)
{
    frc_speed_t c = frc_medium_speed(FRC_MEDIUM_VACUUM);
    unsigned i;

    TEST_SECTION("oracle_ladder_spot: a spread of levels on every other interval");
    for (i = 0; i < ORACLE_LADDER_SPOT_COUNT; i++) {
        const oracle_ladder_spot_t *v = &oracle_ladder_spot[i];
        frc_interval_t iv = interval_of(v->interval);
        frc_sci_t got;

        CHECK_EQ_I64(frc_period(v->level, iv, &got), FRC_OK);
        CHECK_SCI(got, v->period);
        CHECK_EQ_I64(frc_frequency(v->level, iv, &got), FRC_OK);
        CHECK_SCI(got, v->frequency);
        CHECK_EQ_I64(frc_length(v->level, iv, c, &got), FRC_OK);
        CHECK_SCI(got, v->length);
        CHECK_EQ_I64(frc_band(v->level, iv), v->band);
        CHECK_NOTE("interval %u level %u", v->interval, v->level);
#ifndef FRC_NO_FLOAT
        CHECK_EQ_U64(bits_of(frc_period_seconds(v->level, iv)), v->period_bits);
        CHECK_EQ_U64(bits_of(frc_frequency_hz(v->level, iv)), v->frequency_bits);
        CHECK_EQ_U64(bits_of(frc_length_metres(v->level, iv, c)), v->length_bits);
        CHECK_NOTE("doubles, interval %u level %u", v->interval, v->level);
#endif
    }
}

static void test_bands(void)
{
    char buf[32];
    unsigned i;

    TEST_SECTION("oracle_ladder_band_edges: the exact frequency against each edge");
    for (i = 0; i < ORACLE_LADDER_BAND_EDGE_COUNT; i++) {
        const oracle_ladder_band_edge_t *v = &oracle_ladder_band_edges[i];
        CHECK_EQ_I64(frc_band(v->level, interval(v->num, v->den)), v->band);
        CHECK_NOTE("edge vector %u: %" PRIu64 "/%" PRIu64 " ms, level %u", i, v->num, v->den, v->level);
    }

    TEST_SECTION("bands of SPEC S16.1 / S16.2, typed in");
    CHECK_EQ_I64(frc_band(12, REFERENCE), FRC_BAND_NONE);      /* the Blink: 3.2 Hz */
    CHECK_EQ_I64(frc_band(13, REFERENCE), FRC_BAND_AUDIBLE);
    CHECK_EQ_I64(frc_band(16, REFERENCE), FRC_BAND_AUDIBLE);
    CHECK_EQ_I64(frc_band(17, REFERENCE), FRC_BAND_RADIO);
    CHECK_EQ_I64(frc_band(22, REFERENCE), FRC_BAND_MICROWAVE);
    CHECK_EQ_I64(frc_band(26, REFERENCE), FRC_BAND_INFRARED);
    CHECK_EQ_I64(frc_band(29, REFERENCE), FRC_BAND_VISIBLE);   /* zeta1: red light */
    CHECK_EQ_I64(frc_band(30, REFERENCE), FRC_BAND_ULTRAVIOLET);
    CHECK_EQ_I64(frc_band(31, REFERENCE), FRC_BAND_XRAY);
    CHECK_EQ_I64(frc_band(35, REFERENCE), FRC_BAND_GAMMA);
    CHECK_EQ_I64(frc_band(46, REFERENCE), FRC_BAND_GAMMA);
    CHECK_EQ_I64(frc_band(47, REFERENCE), FRC_BAND_NONE);      /* 9.2e29 Hz: past the top */
    CHECK_EQ_I64(frc_band(62, REFERENCE), FRC_BAND_NONE);
    /* an interval of 50 s puts level 0 (f = 1 / (4 I)) at 5 mHz; 1/80 s puts it on 20 Hz exactly */
    CHECK_EQ_I64(frc_band(0, interval(50000, 1)), FRC_BAND_NONE);
    CHECK_EQ_I64(frc_band(0, interval(25, 2)), FRC_BAND_AUDIBLE);
    CHECK_EQ_I64(frc_band(0, interval(26, 2)), FRC_BAND_NONE);
    CHECK_EQ_I64(frc_band(0, interval(1, 80)), FRC_BAND_RADIO); /* 20 kHz exactly */

    TEST_SECTION("frc_band: invalid arguments have no band");
    CHECK_EQ_I64(frc_band(63, REFERENCE), FRC_BAND_NONE);
    CHECK_EQ_I64(frc_band(255, REFERENCE), FRC_BAND_NONE);
    CHECK_EQ_I64(frc_band(20, interval(0, 1)), FRC_BAND_NONE);
    CHECK_EQ_I64(frc_band(20, interval(1, 0)), FRC_BAND_NONE);

    TEST_SECTION("frc_format_band");
    CHECK_EQ_U64(ORACLE_LADDER_BAND_COUNT, FRC_BAND_GAMMA + 1);
    /* the reference names FRC_BAND_NONE with a dash; the core writes "no band"
     * (design ruling 4): the oracle holds the eight names of the bands */
    CHECK_STR(oracle_ladder_band_names[FRC_BAND_NONE], DASH);
    for (i = 1; i < ORACLE_LADDER_BAND_COUNT; i++) {
        CHECK_EQ_I64(frc_format_band((frc_band_t)i, buf, sizeof buf), (int)strlen(oracle_ladder_band_names[i]));
        CHECK_STR(buf, oracle_ladder_band_names[i]);
    }
    CHECK_EQ_I64(frc_format_band(FRC_BAND_NONE, buf, sizeof buf), 7);
    CHECK_STR(buf, "no band");
    frc_format_band(FRC_BAND_XRAY, buf, sizeof buf);
    CHECK_STR(buf, "X-ray");
    frc_format_band(FRC_BAND_GAMMA, buf, sizeof buf);
    CHECK_STR(buf, "gamma ray");
    snprintf(buf, sizeof buf, "junk");
    CHECK_EQ_I64(frc_format_band((frc_band_t)9, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_STR(buf, "");
    CHECK_EQ_I64(frc_format_band(FRC_BAND_AUDIBLE, buf, 7), FRC_ERR_BUFFER);
    CHECK_STR(buf, "audibl");
    /* "no band" is seven bytes: a short buffer keeps a clean prefix */
    CHECK_EQ_I64(frc_format_band(FRC_BAND_NONE, buf, 7), FRC_ERR_BUFFER);
    CHECK_STR(buf, "no ban");
    CHECK_EQ_I64(frc_format_band(FRC_BAND_NONE, buf, 1), FRC_ERR_BUFFER);
    CHECK_STR(buf, "");
    CHECK_EQ_I64(frc_format_band(FRC_BAND_NONE, buf, 8), 7);
    CHECK_EQ_I64(frc_format_band(FRC_BAND_NONE, NULL, 10), FRC_ERR_BUFFER);
}

/* ------------------------------------------------------------------------- */
/* Devices                                                                    */
/* ------------------------------------------------------------------------- */

static void test_devices(void)
{
    /* SPEC-free hand cases: this interval puts level 13 on 20 Hz exactly
     * (it is the first band-edge vector), so T = 1/20 s. */
    const frc_interval_t twenty_hz = {UINT64_C(640483593750), 1};
    uint64_t count;
    unsigned i;

    TEST_SECTION("oracle_ladder_ticks");
    for (i = 0; i < ORACLE_LADDER_TICKS_COUNT; i++) {
        const oracle_ladder_ticks_t *v = &oracle_ladder_ticks[i];
        count = 12345;
        CHECK_EQ_I64(frc_device_ticks(v->level, interval_of(v->interval), v->rate, &count), v->status);
        CHECK_EQ_U64(count, v->ticks);
        CHECK_NOTE("ticks vector %u: interval %u level %u rate %" PRIu64, i, v->interval, v->level, v->rate);
    }

    TEST_SECTION("oracle_ladder_steps");
    for (i = 0; i < ORACLE_LADDER_STEP_COUNT; i++) {
        const oracle_ladder_step_t *v = &oracle_ladder_steps[i];
        count = 12345;
        CHECK_EQ_I64(frc_device_step(v->level, interval_of(v->interval), v->rate, v->bits, &count), v->status);
        CHECK_EQ_U64(count, v->step);
        CHECK_NOTE("step vector %u: interval %u level %u rate %" PRIu64 " bits %u", i, v->interval,
                   v->level, v->rate, v->bits);
    }

    TEST_SECTION("frc_device_ticks: a half goes up, under half a tick is refused");
    CHECK_EQ_I64(frc_device_ticks(13, twenty_hz, 1000, &count), FRC_OK);
    CHECK_EQ_U64(count, 50);
    CHECK_EQ_I64(frc_device_ticks(13, twenty_hz, 20, &count), FRC_OK);
    CHECK_EQ_U64(count, 1);
    CHECK_EQ_I64(frc_device_ticks(13, twenty_hz, 10, &count), FRC_OK); /* 0.5 exactly */
    CHECK_EQ_U64(count, 1);
    CHECK_EQ_I64(frc_device_ticks(13, twenty_hz, 9, &count), FRC_ERR_REFUSED); /* 0.45 */
    CHECK_EQ_U64(count, 0);
    CHECK_EQ_I64(frc_device_ticks(13, twenty_hz, 30, &count), FRC_OK); /* 1.5 */
    CHECK_EQ_U64(count, 2);
    CHECK_EQ_I64(frc_device_ticks(13, twenty_hz, 50, &count), FRC_OK); /* 2.5 */
    CHECK_EQ_U64(count, 3);
    CHECK_EQ_I64(frc_device_ticks(13, twenty_hz, 29, &count), FRC_OK); /* 1.45 */
    CHECK_EQ_U64(count, 1);

    TEST_SECTION("frc_device_ticks: the edge of 64 bits");
    /* level 0, I = num ms: T * rate = 4 num rate / 1000 */
    CHECK_EQ_I64(frc_device_ticks(0, interval(UINT64_MAX, 1), 250, &count), FRC_OK);
    CHECK_EQ_U64(count, UINT64_MAX);
    CHECK_EQ_I64(frc_device_ticks(0, interval(UINT64_C(1) << 62, 1), 1000, &count), FRC_ERR_OVERFLOW);
    CHECK_EQ_U64(count, 0);
    CHECK_EQ_I64(frc_device_ticks(0, interval((UINT64_C(1) << 62) - 1, 1), 1000, &count), FRC_OK);
    CHECK_EQ_U64(count, UINT64_MAX - 3);
    /* 2^65 - 1 = 31 * 8191 * 145295143558111, so this is (2^65 - 1) / 2 = 2^64 - 1/2 ticks:
     * it rounds up to 2^64, which does not fit */
    CHECK_EQ_I64(frc_device_ticks(0, interval(UINT64_C(145295143558111) * 125, 1), 31 * 8191, &count),
                 FRC_ERR_OVERFLOW);
    /* one millisecond less is 1015.684 ticks less: 2^64 - 1016.184, which rounds to 2^64 - 1016 */
    CHECK_EQ_I64(frc_device_ticks(0, interval(UINT64_C(145295143558111) * 125 - 1, 1), 31 * 8191, &count),
                 FRC_OK);
    CHECK_EQ_U64(count, UINT64_MAX - 1015);
    CHECK_EQ_I64(frc_device_ticks(0, interval(UINT64_MAX, 1), UINT64_MAX, &count), FRC_ERR_OVERFLOW);
    CHECK_EQ_I64(frc_device_ticks(62, interval(1, UINT64_MAX), 1, &count), FRC_ERR_REFUSED);

    TEST_SECTION("frc_device_step: half the rate is the limit, a half goes up");
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 40, 64, &count), FRC_OK); /* f == rate / 2 */
    CHECK_EQ_U64(count, UINT64_C(1) << 63);
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 40, 16, &count), FRC_OK);
    CHECK_EQ_U64(count, 32768);
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 40, 1, &count), FRC_OK);
    CHECK_EQ_U64(count, 1);
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 39, 64, &count), FRC_ERR_REFUSED); /* f > rate / 2 */
    CHECK_EQ_U64(count, 0);
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 41, 32, &count), FRC_OK);
    CHECK_EQ_U64(count, 2095105998); /* 20 * 2^32 / 41 = 2095105998.05 */
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 80, 1, &count), FRC_OK); /* 0.5 exactly */
    CHECK_EQ_U64(count, 1);
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 81, 1, &count), FRC_ERR_REFUSED); /* 0.494: nothing */
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 160, 2, &count), FRC_OK); /* 0.5 exactly */
    CHECK_EQ_U64(count, 1);
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, 48000, 32, &count), FRC_OK);
    CHECK_EQ_U64(count, 1789570); /* 20 * 2^32 / 48000 = 1789569.7 */
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, UINT64_MAX, 64, &count), FRC_OK);
    CHECK_EQ_U64(count, 20); /* 20 * 2^64 / (2^64 - 1) */
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, UINT64_MAX, 63, &count), FRC_OK);
    CHECK_EQ_U64(count, 10);
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, UINT64_MAX, 59, &count), FRC_OK);
    CHECK_EQ_U64(count, 1); /* 0.625 */
    CHECK_EQ_I64(frc_device_step(13, twenty_hz, UINT64_MAX, 58, &count), FRC_ERR_REFUSED); /* 0.3125 */

    TEST_SECTION("devices: invalid arguments");
    count = 77;
    CHECK_EQ_I64(frc_device_ticks(63, REFERENCE, 1000, &count), FRC_ERR_ARG);
    CHECK_EQ_U64(count, 0);
    CHECK_EQ_I64(frc_device_ticks(12, REFERENCE, 0, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_ticks(12, interval(0, 1), 1000, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_ticks(12, interval(1, 0), 1000, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_ticks(12, REFERENCE, 1000, NULL), FRC_ERR_ARG);
    count = 77;
    CHECK_EQ_I64(frc_device_step(63, REFERENCE, 48000, 32, &count), FRC_ERR_ARG);
    CHECK_EQ_U64(count, 0);
    CHECK_EQ_I64(frc_device_step(12, REFERENCE, 0, 32, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_step(12, REFERENCE, 48000, 0, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_step(12, REFERENCE, 48000, 65, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_step(12, REFERENCE, 48000, 255, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_step(12, interval(0, 1), 48000, 32, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_step(12, interval(1, 0), 48000, 32, &count), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_device_step(12, REFERENCE, 48000, 32, NULL), FRC_ERR_ARG);
}

/* ------------------------------------------------------------------------- */
/* Text                                                                       */
/* ------------------------------------------------------------------------- */

/* The text of one quantity of a level, with the speed of medium `medium` for a length. */
static int format_level(unsigned quantity, uint8_t level, frc_interval_t iv, unsigned medium,
                        uint8_t digits, char *buf, size_t size)
{
    switch (quantity) {
    case FRC_QUANTITY_TIME:
        return frc_format_period(level, iv, digits, buf, size);
    case FRC_QUANTITY_FREQUENCY:
        return frc_format_frequency(level, iv, digits, buf, size);
    default:
        return frc_format_length(level, iv, frc_medium_speed((frc_medium_t)medium), digits, buf, size);
    }
}

/* The same value as 17 digits. */
static frc_status_t sci_level(unsigned quantity, uint8_t level, frc_interval_t iv, unsigned medium,
                              frc_sci_t *out)
{
    switch (quantity) {
    case FRC_QUANTITY_TIME:
        return frc_period(level, iv, out);
    case FRC_QUANTITY_FREQUENCY:
        return frc_frequency(level, iv, out);
    default:
        return frc_length(level, iv, frc_medium_speed((frc_medium_t)medium), out);
    }
}

static void test_spec_table(void)
{
    char buf[96];
    unsigned i;

    TEST_SECTION("the table of SPEC S16.1, byte for byte");
    CHECK_EQ_U64(ORACLE_LADDER_SPEC_ROW_COUNT, FRC_LEVEL_COUNT);
    for (i = 0; i < ORACLE_LADDER_SPEC_ROW_COUNT; i++) {
        const oracle_ladder_spec_row_t *row = &oracle_ladder_spec_rows[i];
        CHECK_EQ_U64(row->level, i);
        CHECK_EQ_U64(frc_siblings(row->level), row->siblings);
        frc_format_level_name(row->level, buf, sizeof buf);
        CHECK_STR(buf, row->name);
        CHECK_EQ_I64(frc_format_period(row->level, REFERENCE, 4, buf, sizeof buf), (int)strlen(row->period));
        CHECK_STR(buf, row->period);
        CHECK_EQ_I64(frc_format_frequency(row->level, REFERENCE, 4, buf, sizeof buf), (int)strlen(row->frequency));
        CHECK_STR(buf, row->frequency);
        /* SPEC prints a rung outside every band as a dash; the core writes
         * "no band" there (design ruling 4). Every named band byte for byte. */
        if (strcmp(row->band, DASH) == 0) {
            CHECK_EQ_I64(frc_band(row->level, REFERENCE), FRC_BAND_NONE);
            CHECK_EQ_I64(frc_format_band(frc_band(row->level, REFERENCE), buf, sizeof buf), 7);
            CHECK_STR(buf, "no band");
        } else {
            CHECK_EQ_I64(frc_format_band(frc_band(row->level, REFERENCE), buf, sizeof buf), (int)strlen(row->band));
            CHECK_STR(buf, row->band);
        }
        CHECK_EQ_I64(frc_format_length(row->level, REFERENCE, frc_medium_speed(FRC_MEDIUM_VACUUM), 4, buf,
                                       sizeof buf),
                     (int)strlen(row->length));
        CHECK_STR(buf, row->length);
        /* digits 0 means the same four */
        frc_format_period(row->level, REFERENCE, 0, buf, sizeof buf);
        CHECK_STR(buf, row->period);
    }
    /* a few rows typed in from SPEC.md itself */
    frc_format_period(0, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "72.12 y");
    frc_format_frequency(0, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "439.4 pHz");
    frc_format_length(0, REFERENCE, frc_medium_speed(FRC_MEDIUM_VACUUM), 4, buf, sizeof buf);
    CHECK_STR(buf, "682.3 Pm");
    frc_format_period(6, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "5.052 h");
    frc_format_period(7, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "33.68 min");
    frc_format_period(12, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "310.9 ms");
    frc_format_frequency(12, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "3.216 Hz");
    frc_format_length(12, REFERENCE, frc_medium_speed(FRC_MEDIUM_VACUUM), 4, buf, sizeof buf);
    CHECK_STR(buf, "93.21 Mm");
    frc_format_period(15, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "448.7 " MICRO "s");
    frc_format_period(41, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "2.454 " TIMES " 10\342\201\273\302\262\342\201\265 s"); /* 2.454 × 10⁻²⁵ s */
    frc_format_frequency(44, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "1.324 " TIMES " 10\302\262\342\201\267 Hz"); /* 1.324 × 10²⁷ Hz */
    frc_format_period(62, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "4.834 " TIMES " 10\342\201\273\342\201\264\302\263 s"); /* 4.834 × 10⁻⁴³ s */
    frc_format_length(62, REFERENCE, frc_medium_speed(FRC_MEDIUM_VACUUM), 4, buf, sizeof buf);
    CHECK_STR(buf, "1.449 " TIMES " 10\342\201\273\302\263\342\201\264 m"); /* 1.449 × 10⁻³⁴ m */
}

static void test_format_vectors(void)
{
    char buf[96], via_sci[96];
    unsigned i;

    TEST_SECTION("oracle_ladder_format: frc_format_period / _frequency / _length");
    for (i = 0; i < ORACLE_LADDER_FORMAT_COUNT; i++) {
        const oracle_ladder_format_t *v = &oracle_ladder_format[i];
        frc_interval_t iv = interval_of(v->interval);
        frc_sci_t sci;
        int n = format_level(v->quantity, v->level, iv, v->medium, v->digits, buf, sizeof buf);
        CHECK_EQ_I64(n, (int)strlen(v->text));
        CHECK_STR(buf, v->text);
        CHECK_NOTE("format vector %u: interval %u level %u quantity %u medium %u digits %u", i,
                   v->interval, v->level, v->quantity, v->medium, v->digits);
        /* LADDER.md "Two ways in, one text": the 17-digit value formats the same on every vector */
        CHECK_EQ_I64(sci_level(v->quantity, v->level, iv, v->medium, &sci), FRC_OK);
        frc_format_quantity(&sci, (frc_quantity_t)v->quantity, v->digits, via_sci, sizeof via_sci);
        CHECK_STR(via_sci, v->text);
        CHECK_NOTE("format vector %u through frc_sci_t", i);
    }

    TEST_SECTION("oracle_ladder_quantity: frc_format_quantity");
    for (i = 0; i < ORACLE_LADDER_QUANTITY_COUNT; i++) {
        const oracle_ladder_quantity_t *v = &oracle_ladder_quantity[i];
        frc_sci_t sci;
        int n;
        sci.mantissa = v->value.mantissa;
        sci.exp10 = v->value.exp10;
        n = frc_format_quantity(&sci, (frc_quantity_t)v->quantity, v->digits, buf, sizeof buf);
        CHECK_EQ_I64(n, (int)strlen(v->text));
        CHECK_STR(buf, v->text);
        CHECK_NOTE("quantity vector %u: %" PRIu64 "e%d quantity %u digits %u", i, v->value.mantissa,
                   v->value.exp10, v->quantity, v->digits);
    }
}

static void test_format_edges(void)
{
    /* level 41 on the reference interval: "2.454 × 10⁻²⁵ s", 21 bytes */
    static const char FULL[] = "2.454 " TIMES " 10\342\201\273\302\262\342\201\265 s";
    frc_speed_t c = frc_medium_speed(FRC_MEDIUM_VACUUM);
    frc_sci_t sci;
    char buf[96];
    size_t size;

    TEST_SECTION("format: buffers");
    CHECK_EQ_U64(strlen(FULL), 21);
    CHECK_EQ_I64(frc_format_period(41, REFERENCE, 4, buf, 22), 21);
    CHECK_STR(buf, FULL);
    for (size = 1; size <= 21; size++) {
        size_t len;
        memset(buf, '#', sizeof buf);
        CHECK_EQ_I64(frc_format_period(41, REFERENCE, 4, buf, size), FRC_ERR_BUFFER);
        len = strlen(buf);
        CHECK(len < size);                       /* terminated inside the buffer    */
        CHECK(memcmp(buf, FULL, len) == 0);      /* a prefix of the full text       */
        CHECK(buf[size] == '#');                 /* nothing written past the buffer */
        /* never cut inside a UTF-8 sequence: the byte after the prefix starts a character */
        CHECK(((unsigned char)FULL[len] & 0xC0) != 0x80);
    }
    CHECK_EQ_I64(frc_format_period(41, REFERENCE, 4, buf, 0), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_format_period(41, REFERENCE, 4, NULL, 0), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_format_period(41, REFERENCE, 4, NULL, 64), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_format_frequency(12, REFERENCE, 4, buf, 9), 8);
    CHECK_STR(buf, "3.216 Hz");
    CHECK_EQ_I64(frc_format_frequency(12, REFERENCE, 4, buf, 8), FRC_ERR_BUFFER);
    CHECK_STR(buf, "3.216 H");
    CHECK_EQ_I64(frc_format_length(12, REFERENCE, c, 4, buf, 5), FRC_ERR_BUFFER);
    CHECK_STR(buf, "93.2");

    TEST_SECTION("format: invalid arguments");
    snprintf(buf, sizeof buf, "junk");
    CHECK_EQ_I64(frc_format_period(63, REFERENCE, 4, buf, sizeof buf), FRC_ERR_ARG); /* depth 64: nothing */
    CHECK_STR(buf, "");
    CHECK_EQ_I64(frc_format_period(255, REFERENCE, 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_frequency(63, REFERENCE, 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_length(63, REFERENCE, c, 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_period(12, interval(0, 1), 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_period(12, interval(1, 0), 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_frequency(12, interval(0, 0), 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_length(12, REFERENCE, speed(0, 1), 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_length(12, REFERENCE, speed(1, 0), 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_length(12, REFERENCE, frc_medium_speed(FRC_MEDIUM_COUNT), 4, buf, sizeof buf),
                 FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_period(12, REFERENCE, 10, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_period(12, REFERENCE, 255, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_period(12, REFERENCE, 9, buf, sizeof buf), 13);
    CHECK_STR(buf, "310.922105 ms");
    CHECK_EQ_I64(frc_format_period(12, REFERENCE, 1, buf, sizeof buf), 6);
    CHECK_STR(buf, "300 ms");
    /* an invalid argument on a NULL buffer is still the argument's error */
    CHECK_EQ_I64(frc_format_period(63, REFERENCE, 4, NULL, 0), FRC_ERR_ARG);

    TEST_SECTION("frc_format_quantity: invalid arguments");
    sci.mantissa = UINT64_C(31092210463041232);
    sci.exp10 = -17;
    CHECK_EQ_I64(frc_format_quantity(&sci, FRC_QUANTITY_TIME, 4, buf, sizeof buf), 8);
    CHECK_STR(buf, "310.9 ms");
    CHECK_EQ_I64(frc_format_quantity(&sci, FRC_QUANTITY_TIME, 10, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_STR(buf, "");
    CHECK_EQ_I64(frc_format_quantity(&sci, (frc_quantity_t)3, 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_quantity(&sci, (frc_quantity_t)200, 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_quantity(NULL, FRC_QUANTITY_TIME, 4, buf, sizeof buf), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_quantity(&sci, FRC_QUANTITY_TIME, 4, buf, 8), FRC_ERR_BUFFER);
    CHECK_STR(buf, "310.9 m");
    /* a mantissa that does not have 17 digits is not an frc_sci_t */
    sci.mantissa = UINT64_C(9999999999999999);
    CHECK_EQ_I64(frc_format_quantity(&sci, FRC_QUANTITY_TIME, 4, buf, sizeof buf), FRC_ERR_ARG);
    sci.mantissa = UINT64_C(100000000000000000);
    CHECK_EQ_I64(frc_format_quantity(&sci, FRC_QUANTITY_TIME, 4, buf, sizeof buf), FRC_ERR_ARG);
    sci.mantissa = 0;
    CHECK_EQ_I64(frc_format_quantity(&sci, FRC_QUANTITY_LENGTH, 4, buf, sizeof buf), FRC_ERR_ARG);
    sci.mantissa = UINT64_MAX;
    CHECK_EQ_I64(frc_format_quantity(&sci, FRC_QUANTITY_FREQUENCY, 4, buf, sizeof buf), FRC_ERR_ARG);

    TEST_SECTION("frc_format_quantity: the ends of the exponent");
    sci.mantissa = UINT64_C(25000000000000000);
    sci.exp10 = INT16_MAX; /* 2.5 × 10^32783 */
    CHECK(frc_format_quantity(&sci, FRC_QUANTITY_FREQUENCY, 4, buf, sizeof buf) > 0);
    CHECK_STR(buf, "2.500 " TIMES " 10\302\263\302\262\342\201\267\342\201\270\302\263 Hz");
    CHECK(frc_format_quantity(&sci, FRC_QUANTITY_LENGTH, 2, buf, sizeof buf) > 0);
    CHECK_STR(buf, "2.5 " TIMES " 10\302\263\302\262\342\201\267\342\201\270\302\263 m");
    /* in years: 2.5e32783 / 31557600 = 7.922...e32775 */
    CHECK(frc_format_quantity(&sci, FRC_QUANTITY_TIME, 4, buf, sizeof buf) > 0);
    CHECK_STR(buf, "7.922 " TIMES " 10\302\263\302\262\342\201\267\342\201\267\342\201\265 y");
    sci.exp10 = INT16_MIN; /* 2.5 × 10^-32752 */
    CHECK(frc_format_quantity(&sci, FRC_QUANTITY_LENGTH, 4, buf, sizeof buf) > 0);
    CHECK_STR(buf, "2.500 " TIMES " 10\342\201\273\302\263\302\262\342\201\267\342\201\265\302\262 m");
    CHECK(frc_format_quantity(&sci, FRC_QUANTITY_TIME, 1, buf, sizeof buf) > 0);
    CHECK_STR(buf, "2 " TIMES " 10\342\201\273\302\263\302\262\342\201\267\342\201\265\302\262 s");
}

/* ------------------------------------------------------------------------- */
/* SPEC S16.5 and S16.3, typed in                                             */
/* ------------------------------------------------------------------------- */

/*
 * 1 when `sci` is a 17-digit value within half a unit of its last digit of
 * the exact n / d:   | n/d - M * 10^e |  <=  10^e / 2.
 * Cleared of fractions: with A = n * 10^-e and B = M * d (e < 0), or A = n and
 * B = M * d * 10^e, it is 2 |A - B| <= d (* 10^e). A and B are nearly equal,
 * so neither passes 2^57 * d.
 */
static int sci_is_nearest(const frc_sci_t *sci, const frc_i_big_t *n, const frc_i_big_t *d)
{
    frc_i_big_t a, b, unit, diff;
    if (sci->mantissa < UINT64_C(10000000000000000) || sci->mantissa >= UINT64_C(100000000000000000)) {
        return 0;
    }
    frc_i_big_mul_u64(&a, n, 1);
    frc_i_big_mul_u64(&b, d, sci->mantissa);
    frc_i_big_mul_u64(&unit, d, 1);
    if (sci->exp10 < 0) {
        big_mul_pow10(&a, -sci->exp10);
    } else {
        big_mul_pow10(&b, sci->exp10);
        big_mul_pow10(&unit, sci->exp10);
    }
    if (frc_i_big_cmp(&a, &b) >= 0) {
        diff = a;
        ref_sub(&diff, &b);
    } else {
        diff = b;
        ref_sub(&diff, &a);
    }
    ref_double(&diff);
    return frc_i_big_cmp(&diff, &unit) <= 0;
}

/* The same for a fraction of two 64-bit numbers. */
static int sci_is_fraction(const frc_sci_t *sci, uint64_t num, uint64_t den)
{
    frc_i_big_t n, d;
    big_u64(&n, num);
    big_u64(&d, den);
    return sci_is_nearest(sci, &n, &d);
}

static void test_spec_vectors(void)
{
    const frc_interval_t mean = frc_interval_mean(CAT);
    const frc_speed_t vacuum = frc_medium_speed(FRC_MEDIUM_VACUUM);
    const frc_speed_t air = frc_medium_speed(FRC_MEDIUM_AIR);
    const frc_speed_t nerve = frc_medium_speed(FRC_MEDIUM_NERVE);
    const frc_speed_t chemical = frc_medium_speed(FRC_MEDIUM_CHEMICAL);
    frc_sci_t sci;
    char buf[96];

    TEST_SECTION("SPEC S16.5: exact fractions and their rounded values");
    /* T(12), the Blink: 51724807/166359375 s = 310.922 ms */
    CHECK_EQ_I64(frc_period(12, REFERENCE, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, 51724807, 166359375));
    frc_format_period(12, REFERENCE, 6, buf, sizeof buf);
    CHECK_STR(buf, "310.922 ms");
    /* f(13), xi: 1164515625/51724807 Hz = 22.5137 Hz */
    CHECK_EQ_I64(frc_frequency(13, REFERENCE, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, 1164515625, 51724807));
    frc_format_frequency(13, REFERENCE, 6, buf, sizeof buf);
    CHECK_STR(buf, "22.5137 Hz");
    /* f(62), omicron2: 2.0688 × 10⁴² Hz */
    frc_format_frequency(62, REFERENCE, 5, buf, sizeof buf);
    CHECK_STR(buf, "2.0688 " TIMES " 10\342\201\264\302\262 Hz");
    {
        /* its exact fraction: 107008321717714395645982788862060546875000000000000 / 51724807 */
        static const char NUMERATOR[] = "107008321717714395645982788862060546875000000000000";
        frc_i_big_t n, d;
        const char *p;
        frc_i_big_set_u64(&n, 0);
        for (p = NUMERATOR; *p != '\0'; p++) {
            frc_i_big_t digit;
            frc_i_big_mul_u64(&n, &n, 10);
            big_u64(&digit, (uint64_t)(*p - '0'));
            ref_add(&n, &digit);
        }
        big_u64(&d, 51724807);
        CHECK_EQ_I64(frc_frequency(62, REFERENCE, &sci), FRC_OK);
        CHECK(sci_is_nearest(&sci, &n, &d));
        CHECK_EQ_I64(sci.exp10, 26); /* 2.07e42 = 2.07e16 * 10^26 */
    }
    /* length(12), light in vacuum: 2215243861443658/23765625 m = 93,212.1 km */
    CHECK_EQ_I64(frc_length(12, REFERENCE, vacuum, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, UINT64_C(2215243861443658), 23765625));
    frc_format_length(12, REFERENCE, vacuum, 6, buf, sizeof buf);
    CHECK_STR(buf, "93.2121 Mm");
    /* length(12, air): 2534515543/23765625 m = 106.646 m */
    CHECK_EQ_I64(frc_length(12, REFERENCE, air, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, UINT64_C(2534515543), 23765625));
    frc_format_length(12, REFERENCE, air, 6, buf, sizeof buf);
    CHECK_STR(buf, "106.646 m");
    /* length(12, nerve): 413798456/11090625 m = 37.3107 m */
    CHECK_EQ_I64(frc_length(12, REFERENCE, nerve, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, 413798456, 11090625));
    frc_format_length(12, REFERENCE, nerve, 6, buf, sizeof buf);
    CHECK_STR(buf, "37.3107 m");
    /* length(12, chemical): 51724807/166359375000 m = 310.922 µm */
    CHECK_EQ_I64(frc_length(12, REFERENCE, chemical, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, 51724807, UINT64_C(166359375000)));
    frc_format_length(12, REFERENCE, chemical, 6, buf, sizeof buf);
    CHECK_STR(buf, "310.922 " MICRO "m");
    /* length(29), zeta1, light in vacuum: 1107621930721829/1687759133607421875000 m = 656.268 nm */
    {
        frc_i_big_t n, d;
        big_u64(&n, UINT64_C(1107621930721829));
        big_u64(&d, UINT64_C(1687759133607421875)); /* ... * 1000 */
        frc_i_big_mul_u64(&d, &d, 1000);
        CHECK_EQ_I64(frc_length(29, REFERENCE, vacuum, &sci), FRC_OK);
        CHECK(sci_is_nearest(&sci, &n, &d));
    }
    frc_format_length(29, REFERENCE, vacuum, 6, buf, sizeof buf);
    CHECK_STR(buf, "656.268 nm");

    TEST_SECTION("SPEC S16.5: the mean interval of Saros 141");
    /* f(12) = 250529296875/77894856196 Hz = 3.21625 Hz; T(12) = 310.921 ms */
    CHECK_EQ_I64(frc_frequency(12, mean, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, UINT64_C(250529296875), UINT64_C(77894856196)));
    frc_format_frequency(12, mean, 6, buf, sizeof buf);
    CHECK_STR(buf, "3.21625 Hz");
    CHECK_EQ_I64(frc_period(12, mean, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, UINT64_C(77894856196), UINT64_C(250529296875)));
    frc_format_period(12, mean, 6, buf, sizeof buf);
    CHECK_STR(buf, "310.921 ms");

    TEST_SECTION("SPEC S16.3: the figures of the prose");
    /* a Blink is 93,212 km of light in vacuum, 106.6 m of sound in air, 37.31 m along a nerve */
    frc_format_length(12, REFERENCE, vacuum, 5, buf, sizeof buf);
    CHECK_STR(buf, "93.212 Mm");
    frc_format_length(12, REFERENCE, air, 4, buf, sizeof buf);
    CHECK_STR(buf, "106.6 m");
    frc_format_length(12, REFERENCE, nerve, 4, buf, sizeof buf);
    CHECK_STR(buf, "37.31 m");
    /* depth 30 (level 29) is red light, 656.3 nm */
    frc_format_length(29, REFERENCE, vacuum, 4, buf, sizeof buf);
    CHECK_STR(buf, "656.3 nm");
    /* an Alpha of light is 72.12 light-years: the length over a light-year, 9,460,730,472,580,800 m,
     * is the period over a Julian year, so the two share their digits */
    CHECK_EQ_I64(frc_length(0, REFERENCE, vacuum, &sci), FRC_OK);
    CHECK(sci_is_fraction(&sci, UINT64_C(2275891508) * 299792458, 1)); /* 4 I seconds times c */
    frc_format_period(0, REFERENCE, 4, buf, sizeof buf);
    CHECK_STR(buf, "72.12 y");
    /* S3.3: the Alpha of the reference interval is 4 * 568,972,877 s */
    CHECK_EQ_I64(frc_period(0, REFERENCE, &sci), FRC_OK);
    CHECK_EQ_U64(sci.mantissa, UINT64_C(22758915080000000));
    CHECK_EQ_I64(sci.exp10, -7);

    TEST_SECTION("SPEC S16.1: the ladder ends at depth 63");
    sci.mantissa = 1;
    sci.exp10 = 1;
    CHECK_EQ_I64(frc_period(62, REFERENCE, &sci), FRC_OK);
    CHECK_EQ_I64(frc_period(63, REFERENCE, &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_frequency(63, REFERENCE, &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_length(63, REFERENCE, vacuum, &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_period(255, REFERENCE, &sci), FRC_ERR_ARG);

    TEST_SECTION("periods, frequencies, lengths: invalid arguments");
    CHECK_EQ_I64(frc_period(12, REFERENCE, NULL), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_frequency(12, REFERENCE, NULL), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_length(12, REFERENCE, vacuum, NULL), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_period(12, interval(0, 1), &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_period(12, interval(1, 0), &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_frequency(12, interval(0, 1), &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_frequency(12, interval(1, 0), &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_frequency(12, frc_interval_mean(NULL), &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_length(12, interval(0, 1), vacuum, &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_length(12, REFERENCE, speed(0, 1), &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_length(12, REFERENCE, speed(1, 0), &sci), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_length(12, REFERENCE, frc_medium_speed(FRC_MEDIUM_COUNT), &sci), FRC_ERR_ARG);
    /* an interval need not be in lowest terms: the value is what counts */
    {
        frc_sci_t a, b;
        CHECK_EQ_I64(frc_period(20, interval(FRC_INTERVAL_REFERENCE_MS * 3, 3), &a), FRC_OK);
        CHECK_EQ_I64(frc_period(20, REFERENCE, &b), FRC_OK);
        CHECK_SCI(a, b);
        CHECK_EQ_I64(frc_length(20, REFERENCE, speed(299792458u * UINT64_C(7), 7), &a), FRC_OK);
        CHECK_EQ_I64(frc_length(20, REFERENCE, vacuum, &b), FRC_OK);
        CHECK_SCI(a, b);
    }
}

/* ------------------------------------------------------------------------- */
/* Properties on random and extreme inputs                                    */
/* ------------------------------------------------------------------------- */

/* The exact quotient of a level, rebuilt here from P(L) one factor at a time:
 * kind 0 period, 1 frequency, 2 length (= v_num * period / v_den). */
static void exact_quotient(unsigned kind, uint8_t level, frc_interval_t iv, frc_speed_t v,
                           frc_i_big_t *n, frc_i_big_t *d)
{
    frc_i_big_t top, bottom;
    uint8_t step;
    frc_i_big_set_u64(&top, iv.num);
    frc_i_big_mul_u64(&top, &top, 4);
    frc_i_big_set_u64(&bottom, iv.den);
    frc_i_big_mul_u64(&bottom, &bottom, 1000);
    for (step = 0; step < level; step++) {
        frc_i_big_mul_u64(&bottom, &bottom, frc_factor(step));
    }
    if (kind == 2) {
        frc_i_big_mul_u64(&top, &top, v.num);
        frc_i_big_mul_u64(&bottom, &bottom, v.den);
    }
    if (kind == 1) {
        *n = bottom;
        *d = top;
    } else {
        *n = top;
        *d = bottom;
    }
}

#ifndef FRC_NO_FLOAT
/*
 * n / d as a decimal string of 37 significant digits, truncated, with one
 * more digit 1 when the division was not exact (so a value just above a tie
 * of the binary rounding is not read as the tie). The C library's strtod
 * rounds such a string correctly: an independent way to the nearest double.
 */
static void exact_decimal(const frc_i_big_t *n, const frc_i_big_t *d, char out[80])
{
    frc_i_big_t nn, dd, ten_d, q, r;
    char digits[48];
    int e = 0, pos, chunk;

    frc_i_big_mul_u64(&nn, n, 1);
    frc_i_big_mul_u64(&dd, d, 1);
    while (frc_i_big_cmp(&nn, &dd) < 0) {
        frc_i_big_mul_u64(&nn, &nn, 10);
        e--;
    }
    for (;;) {
        frc_i_big_mul_u64(&ten_d, &dd, 10);
        if (frc_i_big_cmp(&nn, &ten_d) < 0) {
            break;
        }
        dd = ten_d;
        e++;
    }
    frc_i_big_divmod(&nn, &dd, &q, &r);
    pos = snprintf(digits, sizeof digits, "%u", (unsigned)frc_i_big_low_u64(&q)); /* one digit, 1..9 */
    for (chunk = 0; chunk < 4; chunk++) {
        frc_i_big_mul_u64(&nn, &r, UINT64_C(1000000000));
        frc_i_big_divmod(&nn, &dd, &q, &r);
        pos += snprintf(digits + pos, sizeof digits - (size_t)pos, "%09u", (unsigned)frc_i_big_low_u64(&q));
    }
    if (!ref_is_zero(&r)) {
        digits[pos++] = '1';
    }
    digits[pos] = '\0';
    snprintf(out, 80, "%c.%se%d", digits[0], digits + 1, e);
}
#endif

static void check_quotients(uint8_t level, frc_interval_t iv, frc_speed_t v, const char *what)
{
    unsigned kind;
    for (kind = 0; kind < 3; kind++) {
        frc_i_big_t n, d;
        frc_sci_t sci;
        frc_status_t status;
        exact_quotient(kind, level, iv, v, &n, &d);
        status = kind == 0 ? frc_period(level, iv, &sci)
                           : kind == 1 ? frc_frequency(level, iv, &sci) : frc_length(level, iv, v, &sci);
        CHECK_EQ_I64(status, FRC_OK);
        CHECK(sci_is_nearest(&sci, &n, &d));
        CHECK_NOTE("%s: kind %u level %u I = %" PRIu64 "/%" PRIu64 " v = %" PRIu64 "/%" PRIu64 " -> %" PRIu64 "e%d",
                   what, kind, level, iv.num, iv.den, v.num, v.den, sci.mantissa, sci.exp10);
#ifndef FRC_NO_FLOAT
        {
            char decimal[80];
            double got = kind == 0 ? frc_period_seconds(level, iv)
                                   : kind == 1 ? frc_frequency_hz(level, iv) : frc_length_metres(level, iv, v);
            exact_decimal(&n, &d, decimal);
            CHECK_EQ_U64(bits_of(got), bits_of(strtod(decimal, NULL)));
            CHECK_NOTE("%s: kind %u level %u I = %" PRIu64 "/%" PRIu64 " v = %" PRIu64 "/%" PRIu64 " = %s",
                       what, kind, level, iv.num, iv.den, v.num, v.den, decimal);
        }
#endif
    }
}

static void test_properties(void)
{
    static const uint64_t ENDS[] = {1, 2, 1000, UINT64_C(0xFFFFFFFF), UINT64_C(0x100000000), UINT64_MAX - 1,
                                    UINT64_MAX};
    const unsigned end_count = (unsigned)(sizeof ENDS / sizeof ENDS[0]);
    unsigned a, b, c, i;

    TEST_SECTION("extreme intervals and speeds: nothing overflows the big integer");
    /* the corners of the OVERFLOW BOUNDS of frc_ladder.c: the smallest and largest
     * numerators and denominators, at the top and the bottom of the ladder */
    for (a = 0; a < end_count; a++) {
        for (b = 0; b < end_count; b++) {
            for (c = 0; c < end_count; c += 3) {
                frc_interval_t iv = interval(ENDS[a], ENDS[b]);
                check_quotients(62, iv, speed(ENDS[c], ENDS[end_count - 1 - c]), "extreme");
                check_quotients(0, iv, speed(ENDS[end_count - 1 - c], ENDS[c]), "extreme");
            }
        }
    }
    {
        /* the widest of all: the smallest and the largest values the API can give */
        frc_sci_t sci;
        char buf[64];
        CHECK_EQ_I64(frc_length(62, interval(1, UINT64_MAX), speed(1, UINT64_MAX), &sci), FRC_OK);
        CHECK_EQ_I64(sci.exp10, -109); /* 2.5e-93 m */
        CHECK_EQ_I64(frc_frequency(62, interval(1, UINT64_MAX), &sci), FRC_OK);
        CHECK_EQ_I64(sci.exp10, 57);   /* 2.2e73 Hz */
        CHECK(frc_format_length(62, interval(1, UINT64_MAX), speed(1, UINT64_MAX), 9, buf, sizeof buf) > 0);
        CHECK_STR(buf, "2.49660777 " TIMES " 10\342\201\273\342\201\271\302\263 m");
        CHECK(frc_format_frequency(62, interval(1, UINT64_MAX), 9, buf, sizeof buf) > 0);
        CHECK_STR(buf, "2.17135064 " TIMES " 10\342\201\267\302\263 Hz");
        /* level 0, the longest interval, in years: 4 * (2^64 - 1) ms */
        CHECK(frc_format_period(0, interval(UINT64_MAX, 1), 9, buf, sizeof buf) > 0);
        CHECK_STR(buf, "2.33816818 " TIMES " 10\342\201\271 y");
        CHECK(frc_format_length(0, interval(UINT64_MAX, 1), speed(UINT64_MAX, 1), 9, buf, sizeof buf) > 0);
        CHECK_STR(buf, "1.36112947 " TIMES " 10\302\263\342\201\266 m");
    }

    TEST_SECTION("random intervals, speeds and levels: 17 digits and doubles are the nearest");
    for (i = 0; i < 400; i++) {
        frc_interval_t iv = interval(rng_any_width(), rng_any_width());
        frc_speed_t v = speed(rng_any_width(), rng_any_width());
        check_quotients((uint8_t)(rng() % FRC_LEVEL_COUNT), iv, v, "random");
    }
    /* realistic ones: an interval near a Saros, the media */
    for (i = 0; i < 200; i++) {
        frc_interval_t iv = interval(FRC_INTERVAL_REFERENCE_MS - 5000000 + rng() % 10000000, 1 + rng() % 70);
        check_quotients((uint8_t)(rng() % FRC_LEVEL_COUNT), iv,
                        frc_medium_speed((frc_medium_t)(rng() % FRC_MEDIUM_COUNT)), "realistic");
    }
}

/* ------------------------------------------------------------------------- */
/* frc_sci_to_double                                                          */
/* ------------------------------------------------------------------------- */

#ifndef FRC_NO_FLOAT
static void test_sci_to_double(void)
{
    frc_sci_t sci;
    char text[64];
    unsigned i, differ = 0;

    TEST_SECTION("oracle_ladder_sci_double");
    for (i = 0; i < ORACLE_LADDER_SCI_DOUBLE_COUNT; i++) {
        const oracle_ladder_sci_double_t *v = &oracle_ladder_sci_double[i];
        sci.mantissa = v->value.mantissa;
        sci.exp10 = v->value.exp10;
        CHECK_EQ_U64(bits_of(frc_sci_to_double(&sci)), v->bits);
        CHECK_NOTE("sci_double vector %u: %" PRIu64 "e%d", i, v->value.mantissa, v->value.exp10);
    }
    /* LADDER.md: 28 of them are not the double of the exact quotient, which is why
     * frc_period_seconds and its siblings do not go through frc_sci_t. The table is
     * period, frequency, vacuum length of each row of oracle_ladder_full. */
    for (i = 0; i < ORACLE_LADDER_FULL_COUNT; i++) {
        const oracle_ladder_full_t *row = &oracle_ladder_full[i];
        differ += oracle_ladder_sci_double[3 * i].bits != row->period_bits;
        differ += oracle_ladder_sci_double[3 * i + 1].bits != row->frequency_bits;
        differ += oracle_ladder_sci_double[3 * i + 2].bits != row->length_bits;
    }
    CHECK_EQ_U64(differ, 28);

    TEST_SECTION("frc_sci_to_double against the C library, exponents -140 .. 140");
    for (i = 0; i < 20000; i++) {
        sci.mantissa = UINT64_C(10000000000000000) + rng() % UINT64_C(90000000000000000);
        sci.exp10 = (int16_t)((int)(rng() % 281) - 140);
        snprintf(text, sizeof text, "%" PRIu64 "e%d", sci.mantissa, sci.exp10);
        CHECK_EQ_U64(bits_of(frc_sci_to_double(&sci)), bits_of(strtod(text, NULL)));
        CHECK_NOTE("%s", text);
    }
    /* values a double holds exactly, and the two ends of the range */
    sci.mantissa = UINT64_C(10000000000000000);
    sci.exp10 = -16;
    CHECK(frc_sci_to_double(&sci) == 1.0);
    sci.mantissa = UINT64_C(50000000000000000);
    sci.exp10 = -17;
    CHECK(frc_sci_to_double(&sci) == 0.5);
    sci.mantissa = UINT64_C(12345678000000000);
    sci.exp10 = -9;
    CHECK(frc_sci_to_double(&sci) == 12345678.0);
    sci.mantissa = UINT64_C(99999999999999999);
    sci.exp10 = 140;
    CHECK_EQ_U64(bits_of(frc_sci_to_double(&sci)), bits_of(strtod("99999999999999999e140", NULL)));
    sci.mantissa = UINT64_C(10000000000000000);
    sci.exp10 = -140;
    CHECK_EQ_U64(bits_of(frc_sci_to_double(&sci)), bits_of(strtod("1e-124", NULL)));
    sci.mantissa = UINT64_C(99999999999999999);
    sci.exp10 = -140;
    CHECK_EQ_U64(bits_of(frc_sci_to_double(&sci)), bits_of(strtod("99999999999999999e-140", NULL)));
    /* halves to even: 2^53 + 1 lies exactly between two doubles (the lower one is even) ... */
    sci.mantissa = UINT64_C(90071992547409930); /* (2^53 + 1) * 10 */
    sci.exp10 = -1;
    CHECK(frc_sci_to_double(&sci) == 9007199254740992.0);
    /* ... and 2^53 + 3 between an odd one below and the even one above */
    sci.mantissa = UINT64_C(90071992547409950);
    CHECK(frc_sci_to_double(&sci) == 9007199254740996.0);

    TEST_SECTION("frc_sci_to_double: outside its domain the answer is 0");
    CHECK(frc_sci_to_double(NULL) == 0.0);
    sci.mantissa = UINT64_C(10000000000000000);
    sci.exp10 = 141;
    CHECK(frc_sci_to_double(&sci) == 0.0);
    sci.exp10 = -141;
    CHECK(frc_sci_to_double(&sci) == 0.0);
    sci.exp10 = INT16_MAX;
    CHECK(frc_sci_to_double(&sci) == 0.0);
    sci.exp10 = INT16_MIN;
    CHECK(frc_sci_to_double(&sci) == 0.0);
    sci.exp10 = 0;
    sci.mantissa = UINT64_C(9999999999999999); /* 16 digits: not an frc_sci_t */
    CHECK(frc_sci_to_double(&sci) == 0.0);
    sci.mantissa = UINT64_C(100000000000000000);
    CHECK(frc_sci_to_double(&sci) == 0.0);
    sci.mantissa = 0;
    CHECK(frc_sci_to_double(&sci) == 0.0);

    TEST_SECTION("doubles: a bad level, interval or speed gives 0");
    CHECK(frc_period_seconds(63, REFERENCE) == 0.0);
    CHECK(frc_frequency_hz(63, REFERENCE) == 0.0);
    CHECK(frc_length_metres(63, REFERENCE, frc_medium_speed(FRC_MEDIUM_VACUUM)) == 0.0);
    CHECK(frc_period_seconds(12, interval(0, 1)) == 0.0);
    CHECK(frc_period_seconds(12, interval(1, 0)) == 0.0);
    CHECK(frc_frequency_hz(12, interval(0, 1)) == 0.0);
    CHECK(frc_frequency_hz(12, interval(1, 0)) == 0.0);
    CHECK(frc_length_metres(12, REFERENCE, speed(0, 1)) == 0.0);
    CHECK(frc_length_metres(12, REFERENCE, speed(1, 0)) == 0.0);
    /* SPEC S3.3 / S16.1: the Alpha of the reference interval is 2,275,891,508 s exactly */
    CHECK(frc_period_seconds(0, REFERENCE) == 2275891508.0);
    /* S16.5, to the digits SPEC prints */
    CHECK(frc_period_seconds(12, REFERENCE) > 0.3109220 && frc_period_seconds(12, REFERENCE) < 0.3109225);
    CHECK(frc_frequency_hz(13, REFERENCE) > 22.51365 && frc_frequency_hz(13, REFERENCE) < 22.51375);
    CHECK(frc_frequency_hz(62, REFERENCE) > 2.06875e42 && frc_frequency_hz(62, REFERENCE) < 2.06885e42);
    CHECK(frc_length_metres(12, REFERENCE, frc_medium_speed(FRC_MEDIUM_AIR)) > 106.6455
          && frc_length_metres(12, REFERENCE, frc_medium_speed(FRC_MEDIUM_AIR)) < 106.6465);
    /* a power of two is exact at any size: level 0 with I = 2^k ms over 2^j */
    CHECK(frc_period_seconds(0, interval(250, 1)) == 1.0);
    CHECK(frc_period_seconds(0, interval(UINT64_C(1) << 63, 1)) == 36893488147419103.232);
    CHECK(frc_frequency_hz(0, interval(250, UINT64_C(1) << 63)) == 9223372036854775808.0);
}
#endif


/* ------------------------------------------------------------------------- */
/* frc_band_edges                                                              */
/* ------------------------------------------------------------------------- */

static void test_band_edges(void)
{
    /* SPEC S16.2, typed in: the edges as the table prints them */
    static const char *const LOW[9] = {NULL, "20 Hz", "20 kHz", "300 MHz", "300 GHz", "400 THz", "790 THz",
                                       "30 PHz", "30 EHz"};
    static const char *const HIGH[9] = {NULL, "20 kHz", "300 MHz", "300 GHz", "400 THz", "790 THz", "30 PHz",
                                        "30 EHz", "3.4 \xC3\x97 10\xC2\xB2\xE2\x81\xB9 Hz"};
    frc_sci_t low, high, previous_high;
    char buf[64];
    int b;

    TEST_SECTION("frc_band_edges: SPEC S16.2");
    for (b = FRC_BAND_AUDIBLE; b <= FRC_BAND_GAMMA; b++) {
        uint8_t digits;
        CHECK_EQ_I64(frc_band_edges((frc_band_t)b, &low, &high), FRC_OK);
        CHECK(low.mantissa >= UINT64_C(10000000000000000) && low.mantissa < UINT64_C(100000000000000000));
        CHECK(high.mantissa >= UINT64_C(10000000000000000) && high.mantissa < UINT64_C(100000000000000000));
        /* one or two significant digits, as the SPEC prints them */
        digits = (low.mantissa % UINT64_C(10000000000000000) == 0) ? 1 : 2;
        CHECK(frc_format_quantity(&low, FRC_QUANTITY_FREQUENCY, digits, buf, sizeof buf) > 0);
        CHECK_STR(buf, LOW[b]);
        digits = (high.mantissa % UINT64_C(10000000000000000) == 0) ? 1 : 2;
        CHECK(frc_format_quantity(&high, FRC_QUANTITY_FREQUENCY, digits, buf, sizeof buf) > 0);
        CHECK_STR(buf, HIGH[b]);
        /* bands abut: each starts where the one below ends */
        if (b > FRC_BAND_AUDIBLE) {
            CHECK(low.mantissa == previous_high.mantissa && low.exp10 == previous_high.exp10);
        }
        previous_high = high;
    }
    CHECK_EQ_I64(frc_band_edges(FRC_BAND_AUDIBLE, &low, &high), FRC_OK);
    CHECK(low.mantissa == UINT64_C(20000000000000000) && low.exp10 == -15); /* the header's example */
    CHECK_EQ_I64(frc_band_edges(FRC_BAND_GAMMA, &low, &high), FRC_OK);
    CHECK(high.mantissa == UINT64_C(34000000000000000) && high.exp10 == 13); /* 3.4e29 */

    TEST_SECTION("frc_band_edges: the very edges frc_band compares with");
    /* f(0) = P(0) / (4 I) with I in ms: f = 250 / I_ms Hz. An edge of e Hz is
     * the interval 250 / e ms; frc_band of it is the band the edge starts. */
    {
        static const struct { uint64_t num, den; frc_band_t at; } edges[3] = {
            {25, 2, FRC_BAND_AUDIBLE},         /* 12.5 ms: 20 Hz */
            {1, 80, FRC_BAND_RADIO},           /* 1/80 ms: 20 kHz */
            {1, 1200000, FRC_BAND_MICROWAVE}}; /* 300 MHz */
        int i;
        for (i = 0; i < 3; i++) {
            CHECK_EQ_I64(frc_band(0, interval(edges[i].num, edges[i].den)), edges[i].at);
            CHECK_EQ_I64(frc_band(0, interval(edges[i].num * 1000001, edges[i].den * 1000000)), edges[i].at - 1);
        }
    }

    TEST_SECTION("frc_band_edges: no band, and the arguments");
    low.mantissa = 7;
    high.mantissa = 7;
    CHECK_EQ_I64(frc_band_edges(FRC_BAND_NONE, &low, &high), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_band_edges((frc_band_t)9, &low, &high), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_band_edges((frc_band_t)-1, &low, &high), FRC_ERR_ARG);
    CHECK(low.mantissa == 7 && high.mantissa == 7); /* nothing written */
    CHECK_EQ_I64(frc_band_edges(FRC_BAND_RADIO, NULL, &high), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_band_edges(FRC_BAND_RADIO, &low, NULL), FRC_ERR_ARG);
}

int main(void)
{
    test_big_basics();
    test_big_mul();
    test_big_shl();
    test_big_divmod();
    test_big_cmp_half();
    test_intervals();
    test_media();
    test_full_ladder();
    test_spot_ladder();
    test_bands();
    test_devices();
    test_spec_table();
    test_format_vectors();
    test_format_edges();
    test_spec_vectors();
    test_properties();
    test_band_edges();
#ifndef FRC_NO_FLOAT
    test_sci_to_double();
#endif
    return TEST_SUMMARY("ladder");
}
