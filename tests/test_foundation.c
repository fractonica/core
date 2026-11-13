/*
 * test_foundation.c - tests of the foundation of fractonica-core:
 *
 *   frc_math.c       128-bit helpers           (known values, identities, __int128 when the host has it)
 *   frc_catalogue.c  catalogue, position <-> time, the anchor rule
 *   frc_hierarchy.c  factors, prefixes, level names, units
 *   frc_cycle.c      cycle fraction, cycles, thresholds, boundary instants
 *   frc_text.c       the text writer, frc_to_ascii
 *
 * Expected values come from three places, in this order of authority:
 *   1. SPEC.md, typed in here by hand: the vectors of S12 (A, B, C, anchor),
 *      the tables of S3.3 and S15.1.
 *   2. tests/vectors/oracle_foundation.h, generated from the frozen JavaScript
 *      reference by tools/oracle/generate-foundation.mjs (exact rationals).
 *   3. Properties that must hold whatever the values are (round trips,
 *      n == q * d + r, a cycle's start lies in the cycle).
 *
 * The file compiles in every variant of `make check-variants`: the float-only
 * API is guarded by FRC_NO_FLOAT.
 */
#include "frc_test.h"

#include "fractonica.h"
#include "frc_internal.h"
#include "vectors/oracle_foundation.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static const frc_catalogue_t *const CAT = &FRC_SAROS_141;

/* splitmix64: a fixed-seed generator, so a failure reproduces. */
static uint64_t rng_state = UINT64_C(0x0123456789ABCDEF);
static uint64_t rng(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}
/* A random value of a random width, so small and large operands both occur. */
static uint64_t rng_any_width(void)
{
    return rng() >> (rng() % 64);
}
/* A random instant inside the Saros 141 catalogue. */
static frc_time_t rng_instant(void)
{
    uint64_t span = (uint64_t)(frc_catalogue_last(CAT) - frc_catalogue_first(CAT));
    return frc_catalogue_first(CAT) + (frc_time_t)(rng() % (span + 1));
}

static frc_i_u128_t u128(uint64_t hi, uint64_t lo)
{
    frc_i_u128_t v;
    v.hi = hi;
    v.lo = lo;
    return v;
}

/*
 * The library hands a 128-bit value over by address and adds in place (see
 * frc_internal.h section 2: a 16-byte struct by value costs a memcpy on some
 * targets). The checks below read better with values, so these wrappers make
 * the by-value form for the tests; each one is a single call.
 */
static frc_i_u128_t v_mul64(uint64_t a, uint64_t b)
{
    frc_i_u128_t r;
    frc_i_mul64(a, b, &r);
    return r;
}
static frc_i_u128_t v_add128(frc_i_u128_t a, frc_i_u128_t b)
{
    frc_i_add128(&a, &b);
    return a;
}
static frc_i_u128_t v_add128_64(frc_i_u128_t a, uint64_t b)
{
    frc_i_add128_64(&a, b);
    return a;
}
static frc_i_u128_t v_sub128(frc_i_u128_t a, frc_i_u128_t b)
{
    frc_i_sub128(&a, &b);
    return a;
}
static int v_cmp128(frc_i_u128_t a, frc_i_u128_t b)
{
    return frc_i_cmp128(&a, &b);
}
static uint64_t v_div128(frc_i_u128_t n, uint64_t d, uint64_t *rem)
{
    return frc_i_div128(&n, d, rem);
}
static uint64_t v_div128_ceil(frc_i_u128_t n, uint64_t d)
{
    return frc_i_div128_ceil(&n, d);
}
static frc_time_t v_time_at_total(const frc_catalogue_t *cat, uint8_t level, frc_i_u128_t total)
{
    return frc_i_time_at_total(cat, level, &total);
}
static frc_time_t v_time_after_total(const frc_catalogue_t *cat, uint8_t level, frc_i_u128_t total,
                                     uint64_t a, uint64_t b)
{
    return frc_i_time_after_total(cat, level, &total, a, b);
}

/* Unix microseconds -> the nearest millisecond (SPEC S12 gives milliseconds). */
static int64_t nearest_ms(frc_time_t us)
{
    return us >= 0 ? (us + 500) / 1000 : -((-us + 500) / 1000);
}

/* ------------------------------------------------------------------------- */
/* Flash accessors                                                            */
/* ------------------------------------------------------------------------- */

static const uint8_t  PGM_U8S[3] FRC_PROGMEM = {0, 0x7F, 0xFF};
static const uint16_t PGM_U16S[2] FRC_PROGMEM = {0x1234, 0xFEDC};
static const uint32_t PGM_U32S[2] FRC_PROGMEM = {0x12345678u, 0xFEDCBA98u};
static const uint64_t PGM_U64S[2] FRC_PROGMEM = {UINT64_C(0x0123456789ABCDEF), UINT64_MAX};
static const int64_t  PGM_I64S[2] FRC_PROGMEM = {INT64_C(-11253795486968), INT64_MAX};
static const char     PGM_TEXT[] FRC_PROGMEM = "one\0two\0\0";

static void test_flash_accessors(void)
{
    TEST_SECTION("FRC_PGM_* accessors");
    CHECK_EQ_U64(FRC_PGM_U8(&PGM_U8S[1]), 0x7F);
    CHECK_EQ_U64(FRC_PGM_U8(&PGM_U8S[2]), 0xFF);
    CHECK_EQ_U64(FRC_PGM_U16(&PGM_U16S[0]), 0x1234);
    CHECK_EQ_U64(FRC_PGM_U16(&PGM_U16S[1]), 0xFEDC);
    CHECK_EQ_U64(FRC_PGM_U32(&PGM_U32S[0]), 0x12345678u);
    CHECK_EQ_U64(FRC_PGM_U32(&PGM_U32S[1]), 0xFEDCBA98u);
    CHECK_EQ_U64(FRC_PGM_U64(&PGM_U64S[0]), UINT64_C(0x0123456789ABCDEF));
    CHECK_EQ_U64(FRC_PGM_U64(&PGM_U64S[1]), UINT64_MAX);
    CHECK_EQ_I64(FRC_PGM_I64(&PGM_I64S[0]), INT64_C(-11253795486968));
    CHECK_EQ_I64(FRC_PGM_I64(&PGM_I64S[1]), INT64_MAX);
    CHECK(FRC_PGM_CHAR(&PGM_TEXT[4]) == 't');

    CHECK(frc_i_packed_nth(PGM_TEXT, 0) == PGM_TEXT);
    CHECK(frc_i_packed_nth(PGM_TEXT, 1) == PGM_TEXT + 4);
    CHECK(frc_i_packed_nth(PGM_TEXT, 2) == PGM_TEXT + 8); /* the closing empty item */
    CHECK(frc_i_packed_nth(PGM_TEXT, 200) == PGM_TEXT + 8); /* never walks past it */
}

/* ------------------------------------------------------------------------- */
/* 128-bit helpers                                                            */
/* ------------------------------------------------------------------------- */

static void check_div(frc_i_u128_t n, uint64_t d)
{
    /* q and r are right iff r < d and q * d + r == n: no wider type needed */
    uint64_t r = 12345;
    uint64_t q = v_div128(n, d, &r);
    frc_i_u128_t back = v_add128_64(v_mul64(q, d), r);
    CHECK(r < d);
    CHECK(back.hi == n.hi && back.lo == n.lo);
    CHECK_NOTE("n = %016" PRIx64 "%016" PRIx64 ", d = %016" PRIx64, n.hi, n.lo, d);
    /* the remainder is optional */
    CHECK_EQ_U64(v_div128(n, d, NULL), q);
    CHECK_EQ_U64(v_div128_ceil(n, d), q + (r != 0));
}

static void test_math_known(void)
{
    frc_i_u128_t p;
    uint64_t r;

    TEST_SECTION("128-bit: known values");
    p = v_mul64(0, UINT64_MAX);
    CHECK(p.hi == 0 && p.lo == 0);
    p = v_mul64(1, UINT64_MAX);
    CHECK(p.hi == 0 && p.lo == UINT64_MAX);
    p = v_mul64(UINT64_MAX, UINT64_MAX); /* (2^64 - 1)^2 = 2^128 - 2^65 + 1 */
    CHECK(p.hi == UINT64_C(0xFFFFFFFFFFFFFFFE) && p.lo == 1);
    p = v_mul64(UINT64_C(0x100000000), UINT64_C(0x100000000)); /* 2^32 * 2^32 */
    CHECK(p.hi == 1 && p.lo == 0);
    p = v_mul64(UINT64_C(0xDEADBEEFCAFEBABE), UINT64_C(0x123456789ABCDEF0));
    CHECK(p.hi == UINT64_C(0x0FD5BDEEEB2A01D7) && p.lo == UINT64_C(0xEB689F4EA447D620));
    p = v_mul64(UINT64_C(0xFFFFFFFF00000001), UINT64_C(0x00000001FFFFFFFF));
    CHECK(p.hi == UINT64_C(0x1FFFFFFFD) && p.lo == UINT64_C(0x2FFFFFFFF));
    /* a clock-sized product: one Saros interval in microseconds times P(21) */
    p = v_mul64(UINT64_C(568972877000000), UINT64_C(230804667843750000));
    CHECK_EQ_U64(v_div128(p, UINT64_C(230804667843750000), &r), UINT64_C(568972877000000));
    CHECK_EQ_U64(r, 0);

    p = v_add128(u128(1, UINT64_MAX), u128(2, 1)); /* carry into the high word */
    CHECK(p.hi == 4 && p.lo == 0);
    p = v_add128(u128(5, 6), u128(7, 8));
    CHECK(p.hi == 12 && p.lo == 14);
    p = v_add128_64(u128(7, UINT64_MAX - 1), 3);
    CHECK(p.hi == 8 && p.lo == 1);
    p = v_add128_64(u128(7, 10), 3);
    CHECK(p.hi == 7 && p.lo == 13);
    p = v_sub128(u128(4, 0), u128(1, 1)); /* borrow from the high word */
    CHECK(p.hi == 2 && p.lo == UINT64_MAX);
    p = v_sub128(u128(9, 9), u128(4, 3));
    CHECK(p.hi == 5 && p.lo == 6);

    CHECK(v_cmp128(u128(1, 0), u128(0, UINT64_MAX)) > 0);
    CHECK(v_cmp128(u128(0, UINT64_MAX), u128(1, 0)) < 0);
    CHECK(v_cmp128(u128(3, 5), u128(3, 6)) < 0);
    CHECK(v_cmp128(u128(3, 7), u128(3, 6)) > 0);
    CHECK(v_cmp128(u128(3, 6), u128(3, 6)) == 0);

    CHECK_EQ_U64(v_div128(u128(0, 100), 7, &r), 14);
    CHECK_EQ_U64(r, 2);
    CHECK_EQ_U64(v_div128(u128(1, 0), 3, &r), UINT64_C(0x5555555555555555)); /* 2^64 / 3 */
    CHECK_EQ_U64(r, 1);
    CHECK_EQ_U64(v_div128(u128(UINT64_C(0x0123456789ABCDEF), UINT64_C(0xFEDCBA9876543210)),
                              UINT64_C(0x8000000000000001), &r),
                 UINT64_C(0x02468ACF13579BDF));
    CHECK_EQ_U64(r, UINT64_C(0x7C962FC962FC9631));
    CHECK_EQ_U64(v_div128(u128(UINT64_C(0xFFFFFFFF), UINT64_C(0x1234)), UINT64_C(0x100000001), &r),
                 UINT64_C(0xFFFFFFFE00000002));
    CHECK_EQ_U64(r, UINT64_C(0x1232));
    CHECK_EQ_U64(v_div128(u128(0, 0), 5, &r), 0);
    CHECK_EQ_U64(r, 0);
    CHECK_EQ_U64(v_div128_ceil(u128(0, 0), 5), 0);
    CHECK_EQ_U64(v_div128_ceil(u128(0, 10), 5), 2);
    CHECK_EQ_U64(v_div128_ceil(u128(0, 11), 5), 3);

    /* outside the contract the answer saturates; it never traps */
    CHECK_EQ_U64(v_div128(u128(0, 1), 0, &r), UINT64_MAX);
    CHECK_EQ_U64(r, 0);
    CHECK_EQ_U64(v_div128(u128(5, 0), 5, &r), UINT64_MAX); /* quotient would be 2^64 */
    CHECK_EQ_U64(v_div128(u128(6, 0), 5, NULL), UINT64_MAX);

    CHECK_EQ_U64(frc_i_muldiv_floor(10, 10, 3, &r), 33);
    CHECK_EQ_U64(r, 1);
    CHECK_EQ_U64(frc_i_muldiv_floor(10, 10, 3, NULL), 33);
    CHECK_EQ_U64(frc_i_muldiv_ceil(10, 10, 3), 34);
    CHECK_EQ_U64(frc_i_muldiv_ceil(9, 10, 3), 30);
    CHECK_EQ_U64(frc_i_muldiv_ceil(0, 10, 3), 0);
    CHECK_EQ_U64(frc_i_muldiv_floor(UINT64_C(1) << 63, 6, 4, &r), UINT64_C(0xC000000000000000));
    CHECK_EQ_U64(r, 0);
    CHECK_EQ_U64(frc_i_muldiv_floor(UINT64_MAX, UINT64_MAX, UINT64_MAX, &r), UINT64_MAX);
    CHECK_EQ_U64(r, 0);
    CHECK_EQ_U64(frc_i_muldiv_ceil(UINT64_MAX - 1, UINT64_MAX, UINT64_MAX), UINT64_MAX - 1);

    TEST_SECTION("128-bit: sums in place, products that fit, powers");
    {
        /* a += a doubles: both words of b are read before a is written */
        frc_i_u128_t a = u128(UINT64_C(0x4000000000000001), UINT64_C(0x8000000000000001));
        uint64_t product = 77;
        uint8_t e;

        frc_i_add128(&a, &a);
        CHECK(a.hi == UINT64_C(0x8000000000000003) && a.lo == 2);
        frc_i_sub128(&a, &a);
        CHECK(a.hi == 0 && a.lo == 0);

        CHECK(frc_i_mul64_fits(UINT64_C(0xFFFFFFFF), UINT64_C(0x100000001), &product) == 1);
        CHECK_EQ_U64(product, UINT64_MAX);
        CHECK(frc_i_mul64_fits(0, UINT64_MAX, &product) == 1);
        CHECK_EQ_U64(product, 0);
        product = 77;
        CHECK(frc_i_mul64_fits(UINT64_C(1) << 32, UINT64_C(1) << 32, &product) == 0);
        CHECK_EQ_U64(product, 77); /* left alone when it does not fit */
        /* the result may be one of the factors (frc_i_pow_u64 relies on it) */
        product = 12;
        CHECK(frc_i_mul64_fits(product, 12, &product) == 1);
        CHECK_EQ_U64(product, 144);

        CHECK_EQ_U64(frc_i_pow_u64(10, 0), 1);
        CHECK_EQ_U64(frc_i_pow_u64(10, 19), UINT64_C(10000000000000000000));
        CHECK_EQ_U64(frc_i_pow_u64(10, 20), 0); /* 10^20 > 2^64 */
        CHECK_EQ_U64(frc_i_pow_u64(5, 27), UINT64_C(7450580596923828125));
        CHECK_EQ_U64(frc_i_pow_u64(5, 28), 0);
        CHECK_EQ_U64(frc_i_pow_u64(2, 63), UINT64_C(1) << 63);
        CHECK_EQ_U64(frc_i_pow_u64(2, 64), 0);
        CHECK_EQ_U64(frc_i_pow_u64(484, 4), UINT64_C(54875873536)); /* the widest odometer */
        CHECK_EQ_U64(frc_i_pow_u64(1, 255), 1);
        for (e = 1; e <= 19; e++) {
            CHECK_EQ_U64(frc_i_pow_u64(10, e), frc_i_pow_u64(10, (uint8_t)(e - 1)) * 10u);
        }
    }
}

static void test_math_identities(void)
{
    static const uint64_t DIVISORS[] = {
        1, 2, 3, 10, 0xFFFF, 0x10000, 0xFFFFFFFFu, UINT64_C(0x100000000), UINT64_C(0x100000001),
        UINT64_C(0x7FFFFFFFFFFFFFFF), UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
        UINT64_C(0xFFFFFFFF00000000), UINT64_C(0xFFFFFFFFFFFFFFFE), UINT64_MAX,
        UINT64_C(230804667843750000), UINT64_C(568972877000000)};
    size_t i;
    int k;

    TEST_SECTION("128-bit: division identities");
    for (i = 0; i < sizeof DIVISORS / sizeof DIVISORS[0]; i++) {
        uint64_t d = DIVISORS[i];
        uint64_t r;
        /* the largest dividend the contract allows: d * 2^64 - 1 = d * (2^64 - 1) + (d - 1) */
        CHECK_EQ_U64(v_div128(u128(d - 1, UINT64_MAX), d, &r), UINT64_MAX);
        CHECK_EQ_U64(r, d - 1);
        check_div(u128(d - 1, UINT64_MAX), d);
        check_div(u128(d - 1, 0), d);
        check_div(u128(d / 2, d), d);
        check_div(u128(0, d), d);
        check_div(u128(0, d - 1), d);
    }
    for (k = 0; k < 20000; k++) {
        uint64_t d = rng_any_width();
        frc_i_u128_t n;
        if (d == 0) {
            d = 1;
        }
        n = u128(rng_any_width() % d, rng()); /* hi < d: inside the contract */
        check_div(n, d);
    }
}

#ifdef __SIZEOF_INT128__
/* The host has a 128-bit integer: hold every helper against it directly. */
static void test_math_against_int128(void)
{
    int k;
    TEST_SECTION("128-bit: against the compiler's __int128");
    for (k = 0; k < 200000; k++) {
        uint64_t a = rng_any_width(), b = rng_any_width(), c = rng_any_width();
        uint64_t x = rng(), y = rng();
        unsigned __int128 wide = (unsigned __int128)a * b;
        unsigned __int128 other = ((unsigned __int128)x << 64) | y;
        unsigned __int128 sum = wide + other, diff = wide - other;
        frc_i_u128_t p = v_mul64(a, b);
        frc_i_u128_t s = v_add128(p, u128(x, y));
        frc_i_u128_t d = v_sub128(p, u128(x, y));
        frc_i_u128_t s64 = v_add128_64(p, x);
        int cmp = wide < other ? -1 : wide > other ? 1 : 0;

        CHECK(p.hi == (uint64_t)(wide >> 64) && p.lo == (uint64_t)wide);
        CHECK(s.hi == (uint64_t)(sum >> 64) && s.lo == (uint64_t)sum);
        CHECK(d.hi == (uint64_t)(diff >> 64) && d.lo == (uint64_t)diff);
        CHECK(s64.hi == (uint64_t)((wide + x) >> 64) && s64.lo == (uint64_t)(wide + x));
        CHECK(v_cmp128(p, u128(x, y)) == cmp);

        if (c != 0 && (uint64_t)(wide >> 64) < c) { /* the quotient fits 64 bits */
            uint64_t r = 0;
            unsigned __int128 q = wide / c, rem = wide % c;
            CHECK_EQ_U64(frc_i_muldiv_floor(a, b, c, &r), (uint64_t)q);
            CHECK_EQ_U64(r, (uint64_t)rem);
            if (q + (rem != 0) <= UINT64_MAX) {
                CHECK_EQ_U64(frc_i_muldiv_ceil(a, b, c), (uint64_t)(q + (rem != 0)));
            }
        }
        if (c != 0) {
            uint64_t hi = x % c, r = 0; /* a full-width dividend with hi < divisor */
            unsigned __int128 n = ((unsigned __int128)hi << 64) | y;
            CHECK_EQ_U64(v_div128(u128(hi, y), c, &r), (uint64_t)(n / c));
            CHECK_EQ_U64(r, (uint64_t)(n % c));
        }
    }
}
#endif

/* ------------------------------------------------------------------------- */
/* Catalogue                                                                  */
/* ------------------------------------------------------------------------- */

static void test_catalogue_table(void)
{
    uint16_t k;

    TEST_SECTION("catalogue: Saros 141 table");
    CHECK_EQ_U64(CAT->count, 70);
    CHECK_EQ_U64(CAT->count, ORACLE_FOUNDATION_CATALOGUE_COUNT);
    CHECK_EQ_U64(CAT->series, 141);
    CHECK_EQ_U64(CAT->anchor, 20);
    /* all 70 against F.defaults.unixTimestamps of the reference library */
    for (k = 0; k < 70; k++) {
        CHECK_EQ_I64(FRC_PGM_I64(&CAT->instants_ms[k]), ORACLE_FOUNDATION_CATALOGUE_MS[k]);
        CHECK_EQ_I64(frc_catalogue_instant(CAT, k), ORACLE_FOUNDATION_CATALOGUE_MS[k] * 1000);
        if (k > 0) {
            CHECK(frc_catalogue_instant(CAT, k) > frc_catalogue_instant(CAT, (uint16_t)(k - 1)));
        }
    }
    /* SPEC S14 by hand: the two ends and the anchor (S7.2: Unix 125593319.816) */
    CHECK_EQ_I64(frc_catalogue_instant(CAT, 0), INT64_C(-11253795486968000));
    CHECK_EQ_I64(frc_catalogue_instant(CAT, 20), INT64_C(125593319816000));
    CHECK_EQ_I64(frc_catalogue_instant(CAT, 69), INT64_C(28005212035816000));
    CHECK_EQ_I64(frc_catalogue_instant(CAT, 70), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_catalogue_instant(CAT, 65535), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_catalogue_first(CAT), INT64_C(-11253795486968000));
    CHECK_EQ_I64(frc_catalogue_last(CAT), INT64_C(28005212035816000));
    /* the reference interval of S16.1 is 2010-2028, ordinals 22 -> 23 */
    CHECK_EQ_U64(frc_i_cat_interval(CAT, 22), FRC_INTERVAL_REFERENCE_MS * 1000);

    CHECK_EQ_I64(frc_catalogue_instant(NULL, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_catalogue_first(NULL), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_catalogue_last(NULL), FRC_TIME_NONE);
    CHECK(frc_catalogue_contains(NULL, 0) == 0);
}

static void test_catalogue_edges(void)
{
    const frc_time_t first = frc_catalogue_first(CAT), last = frc_catalogue_last(CAT);
    frc_position_t pos;
    uint16_t k;

    TEST_SECTION("catalogue: edges");
    CHECK(frc_catalogue_contains(CAT, first) == 1);
    CHECK(frc_catalogue_contains(CAT, first - 1) == 0);
    CHECK(frc_catalogue_contains(CAT, last) == 1);
    CHECK(frc_catalogue_contains(CAT, last + 1) == 0);
    CHECK(frc_catalogue_contains(CAT, 0) == 1);
    CHECK(frc_catalogue_contains(CAT, INT64_MIN) == 0);
    CHECK(frc_catalogue_contains(CAT, INT64_MAX) == 0);

    /* the first instant is position 0 exactly */
    CHECK(frc_position_at(CAT, first, &pos) == FRC_OK);
    CHECK_EQ_U64(pos.ordinal, 0);
    CHECK_EQ_U64(pos.num, 0);
    CHECK_EQ_U64(pos.den, (uint64_t)(frc_catalogue_instant(CAT, 1) - first));
    /* one microsecond outside either end: undefined, never extrapolated */
    CHECK(frc_position_at(CAT, first - 1, &pos) == FRC_ERR_RANGE);
    CHECK(frc_position_at(CAT, last + 1, &pos) == FRC_ERR_RANGE);
    CHECK(frc_position_at(CAT, INT64_MIN, &pos) == FRC_ERR_RANGE);
    CHECK(frc_position_at(CAT, INT64_MAX, &pos) == FRC_ERR_RANGE);
    /* the closing instant reads as one microsecond before it */
    CHECK(frc_position_at(CAT, last, &pos) == FRC_OK);
    CHECK_EQ_U64(pos.ordinal, 68);
    CHECK_EQ_U64(pos.den, (uint64_t)(last - frc_catalogue_instant(CAT, 68)));
    CHECK_EQ_U64(pos.num, pos.den - 1);
    CHECK(frc_position_at(CAT, last - 1, &pos) == FRC_OK);
    CHECK_EQ_U64(pos.ordinal, 68);
    CHECK_EQ_U64(pos.num, pos.den - 1);

    /* every eclipse opens its interval; the microsecond before closes the previous one */
    for (k = 1; k < 69; k++) {
        frc_time_t e = frc_catalogue_instant(CAT, k);
        CHECK(frc_position_at(CAT, e, &pos) == FRC_OK);
        CHECK(pos.ordinal == k && pos.num == 0);
        CHECK(frc_position_at(CAT, e - 1, &pos) == FRC_OK);
        CHECK(pos.ordinal == k - 1 && pos.num == pos.den - 1);
        CHECK(frc_position_at(CAT, e + 1, &pos) == FRC_OK);
        CHECK(pos.ordinal == k && pos.num == 1);
    }

    CHECK(frc_position_at(NULL, 0, &pos) == FRC_ERR_ARG);
    CHECK(frc_position_at(CAT, 0, NULL) == FRC_ERR_ARG);
}

static void test_catalogue_oracle(void)
{
    int i;

    TEST_SECTION("catalogue: positions against the oracle");
    for (i = 0; i < ORACLE_FOUNDATION_POSITION_COUNT; i++) {
        const oracle_foundation_position_t *v = &ORACLE_FOUNDATION_POSITION[i];
        frc_position_t pos;
        CHECK(frc_position_at(CAT, v->t, &pos) == FRC_OK);
        CHECK_EQ_U64(pos.ordinal, v->ordinal);
        CHECK_EQ_U64(pos.num, v->num);
        CHECK_EQ_U64(pos.den, v->den);
        CHECK_NOTE("position vector %d, t = %" PRId64, i, v->t);
    }

    TEST_SECTION("catalogue: frc_time_at_position against the oracle");
    for (i = 0; i < ORACLE_FOUNDATION_TIME_COUNT; i++) {
        const oracle_foundation_time_t *v = &ORACLE_FOUNDATION_TIME[i];
        CHECK_EQ_I64(frc_time_at_position(CAT, v->n, v->d), v->time);
        CHECK_NOTE("time vector %d: %" PRIu64 " / %" PRIu64, i, v->n, v->d);
    }
}

static void test_time_at_position(void)
{
    frc_position_t pos;
    uint16_t k;
    int i;

    TEST_SECTION("catalogue: frc_time_at_position");
    for (k = 0; k < 70; k++) { /* position k is eclipse k, whatever the denominator */
        CHECK_EQ_I64(frc_time_at_position(CAT, k, 1), frc_catalogue_instant(CAT, k));
        CHECK_EQ_I64(frc_time_at_position(CAT, (uint64_t)k * 1000003u, 1000003u), frc_catalogue_instant(CAT, k));
    }
    CHECK_EQ_I64(frc_time_at_position(CAT, 69, 1), frc_catalogue_last(CAT)); /* the closing instant */
    CHECK_EQ_I64(frc_time_at_position(CAT, 70, 1), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_position(CAT, 69 * 4 + 1, 4), FRC_TIME_NONE); /* a quarter Saros past it */
    CHECK_EQ_I64(frc_time_at_position(CAT, UINT64_MAX, 1), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_position(CAT, 1, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_time_at_position(NULL, 1, 1), FRC_TIME_NONE);
    /* halfway through the first interval, rounded UP to a whole microsecond */
    {
        uint64_t interval = frc_i_cat_interval(CAT, 0);
        CHECK_EQ_I64(frc_time_at_position(CAT, 1, 2), frc_catalogue_first(CAT) + (frc_time_t)((interval + 1) / 2));
        /* 1/I of the interval is exactly one microsecond; 1/(I+1) is a hair less and still rounds up to it */
        CHECK_EQ_I64(frc_time_at_position(CAT, 1, interval), frc_catalogue_first(CAT) + 1);
        CHECK_EQ_I64(frc_time_at_position(CAT, 1, interval + 1), frc_catalogue_first(CAT) + 1);
        CHECK_EQ_I64(frc_time_at_position(CAT, 1, UINT64_MAX), frc_catalogue_first(CAT) + 1);
    }

    TEST_SECTION("catalogue: position <-> time round trip");
    /* position(t) = ordinal + num/den is exact, so its instant is t itself */
    for (i = 0; i < 5000; i++) {
        frc_time_t t = rng_instant();
        if (t == frc_catalogue_last(CAT)) {
            continue; /* the closing instant reads one microsecond early by design */
        }
        CHECK(frc_position_at(CAT, t, &pos) == FRC_OK);
        CHECK(pos.num < pos.den);
        CHECK_EQ_I64(frc_time_at_position(CAT, (uint64_t)pos.ordinal * pos.den + pos.num, pos.den), t);
        CHECK_EQ_I64(frc_i_instant(CAT, pos.ordinal, pos.num, pos.den), t);
        /* a point strictly between two microseconds belongs to the later one */
        CHECK_EQ_I64(frc_i_instant(CAT, pos.ordinal, 2 * pos.num + 1, 2 * pos.den), t + 1);
    }
}

#ifndef FRC_NO_FLOAT
static void test_position_double(void)
{
    /* SPEC S12: the positions of vectors A, B and C */
    static const struct { int64_t unix_s; double position; } V[] = {
        {1791460800, 22.927850216635}, {946684800, 21.443111640256}, {2886085800, 24.851712757369}};
    frc_position_t pos;
    size_t i;

    TEST_SECTION("catalogue: frc_position_to_double");
    for (i = 0; i < sizeof V / sizeof V[0]; i++) {
        double p, error;
        CHECK(frc_position_at(CAT, FRC_TIME_FROM_S(V[i].unix_s), &pos) == FRC_OK);
        p = frc_position_to_double(&pos);
        error = p - V[i].position;
        CHECK(error > -1e-12 && error < 1e-12); /* the SPEC prints 12 decimals */
        CHECK_NOTE("position %.15f, SPEC %.12f", p, V[i].position);
    }
    CHECK(frc_position_at(CAT, frc_catalogue_instant(CAT, 20), &pos) == FRC_OK);
    CHECK(frc_position_to_double(&pos) == 20.0);
    CHECK(frc_position_to_double(NULL) == 0.0);
}
#endif

static void test_catalogue_init(void)
{
    /* another "series": Saros 141 without its first eclipses, which shifts the ordinals */
    int64_t table[70];
    static const int64_t before_utc[13] = {-1300, -1200, -1100, -1000, -900, -800, -700,
                                           -600,  -500,  -400,  -300,  -200, -100};
    static const int64_t not_increasing[4] = {0, 10, 10, 20};
    static const int64_t decreasing[3] = {30, 20, 10};
    static const int64_t too_late[2] = {0, INT64_MAX / 1000 + 1};
    static const int64_t too_early[2] = {-(INT64_MAX / 1000) - 1, 0};
    static const int64_t widest[2] = {-(INT64_MAX / 1000), INT64_MAX / 1000};
    frc_catalogue_t cat;
    frc_position_t pos;
    uint16_t k, drop;

    TEST_SECTION("catalogue: frc_catalogue_init");
    for (k = 0; k < 70; k++) {
        table[k] = ORACLE_FOUNDATION_CATALOGUE_MS[k];
    }
    CHECK(frc_catalogue_init(&cat, table, 70, 141) == FRC_OK);
    CHECK(cat.instants_ms == table && cat.count == 70 && cat.series == 141);
    CHECK_EQ_U64(cat.anchor, 20); /* SPEC S7.2: the first Alpha of the UTC era, 1973-12-24 */

    /* Drop d leading eclipses: the old ordinal 20 + j becomes 20 + j - d. The first ordinal that
     * is a multiple of 4 AND on or after 1972 is then:
     *   d = 1: 20 (old 21, 1992)   d = 2: 20 (old 22, 2010)   d = 3: 20 (old 23, 2028)   d = 4: 16 (old 20, 1973) */
    for (drop = 1; drop <= 4; drop++) {
        CHECK(frc_catalogue_init(&cat, table + drop, (uint16_t)(70 - drop), 0) == FRC_OK);
        CHECK_EQ_U64(cat.anchor, drop == 4 ? 16 : 20);
        CHECK(cat.anchor % 4 == 0);
        CHECK(frc_catalogue_instant(&cat, cat.anchor) >= FRC_TIME_FROM_MS(INT64_C(63072000000)));
        CHECK_EQ_U64(cat.series, 0);
    }
    /* the 1972-01-01 boundary itself counts as "on or after" */
    {
        static const int64_t at_boundary[6] = {INT64_C(63071999999) - 400, INT64_C(63071999999) - 300,
                                               INT64_C(63071999999) - 200, INT64_C(63071999999) - 100,
                                               INT64_C(63072000000),       INT64_C(63072000000) + 100};
        static const int64_t just_before[9] = {0, 1, 2, 3, INT64_C(63071999999), INT64_C(63072000000),
                                               INT64_C(63072000001), INT64_C(63072000002), INT64_C(63072000003)};
        CHECK(frc_catalogue_init(&cat, at_boundary, 6, 7) == FRC_OK);
        CHECK_EQ_U64(cat.anchor, 4);
        CHECK(frc_catalogue_init(&cat, just_before, 9, 7) == FRC_OK);
        CHECK_EQ_U64(cat.anchor, 8); /* ordinal 4 is one millisecond too early */
    }

    /* A series with no eclipse in the UTC era: the last multiple of 4 that starts a full Alpha. */
    CHECK(frc_catalogue_init(&cat, before_utc, 13, 9) == FRC_OK);
    CHECK_EQ_U64(cat.anchor, 8); /* 8 + 4 <= 12 */
    CHECK(frc_catalogue_init(&cat, before_utc, 12, 9) == FRC_OK);
    CHECK_EQ_U64(cat.anchor, 4); /* 8 + 4 > 11 */
    CHECK(frc_catalogue_init(&cat, before_utc, 9, 9) == FRC_OK);
    CHECK_EQ_U64(cat.anchor, 4);
    CHECK(frc_catalogue_init(&cat, before_utc, 5, 9) == FRC_OK);
    CHECK_EQ_U64(cat.anchor, 0);
    CHECK(frc_catalogue_init(&cat, before_utc, 2, 9) == FRC_OK); /* too short for an Alpha */
    CHECK_EQ_U64(cat.anchor, 0);
    CHECK_EQ_U64(cat.count, 2);
    CHECK_EQ_U64(cat.series, 9);

    /* refused, and `cat` is left as it was */
    cat.count = 2;
    cat.series = 9;
    CHECK(frc_catalogue_init(&cat, before_utc, 1, 1) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, before_utc, 0, 1) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, NULL, 13, 1) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(NULL, before_utc, 13, 1) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, not_increasing, 4, 1) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, decreasing, 3, 1) == FRC_ERR_ARG);
    CHECK(frc_catalogue_init(&cat, too_late, 2, 1) == FRC_ERR_ARG);  /* ms * 1000 would overflow */
    CHECK(frc_catalogue_init(&cat, too_early, 2, 1) == FRC_ERR_ARG);
    CHECK(cat.count == 2 && cat.series == 9 && cat.instants_ms == before_utc);

    /* the widest table that is accepted: one interval longer than INT64_MAX microseconds */
    CHECK(frc_catalogue_init(&cat, widest, 2, 1) == FRC_OK);
    CHECK(frc_position_at(&cat, 0, &pos) == FRC_OK);
    CHECK_EQ_U64(pos.ordinal, 0);
    CHECK_EQ_U64(pos.num, (uint64_t)(INT64_MAX / 1000) * 1000);
    CHECK_EQ_U64(pos.den, (uint64_t)(INT64_MAX / 1000) * 2000);
    CHECK_EQ_I64(frc_time_at_position(&cat, 1, 2), 0);
    CHECK_EQ_I64(frc_time_at_position(&cat, 1, 1), frc_catalogue_last(&cat));

    /* a catalogue that is not usable is refused everywhere, never dereferenced */
    cat.count = 1;
    CHECK(frc_position_at(&cat, 0, &pos) == FRC_ERR_ARG);
    CHECK_EQ_I64(frc_catalogue_first(&cat), FRC_TIME_NONE);
    CHECK_EQ_U64(frc_cycle_count(&cat, 0), 0);
    cat.count = 2;
    cat.instants_ms = NULL;
    CHECK(frc_position_at(&cat, 0, &pos) == FRC_ERR_ARG);
    CHECK_EQ_I64(frc_time_at_position(&cat, 0, 1), FRC_TIME_NONE);
}

/* ------------------------------------------------------------------------- */
/* Hierarchy                                                                  */
/* ------------------------------------------------------------------------- */

static void test_factors_and_prefixes(void)
{
    static const uint8_t S[8] = {5, 13, 5, 5, 7, 11, 9, 4}; /* SPEC S3.1 */
    /* SPEC S3.3, column "Siblings", levels 1..13 */
    static const uint8_t SIBLINGS[14] = {0, 5, 13, 5, 5, 7, 11, 9, 4, 5, 13, 5, 5, 7};
    char text[64], expected[64];
    uint64_t p = 1;
    int level, size;

    TEST_SECTION("hierarchy: factors and siblings");
    for (level = 0; level < 256; level++) {
        CHECK_EQ_U64(frc_factor((uint8_t)level), S[level % 8]);
        CHECK_EQ_U64(frc_siblings((uint8_t)level), level == 0 ? 0 : S[(level - 1) % 8]);
    }
    for (level = 0; level < 14; level++) {
        CHECK_EQ_U64(frc_siblings((uint8_t)level), SIBLINGS[level]);
    }

    TEST_SECTION("hierarchy: frc_prefix");
    for (level = 0; level <= 22; level++) { /* the recurrence P(L) = P(L-1) * S[(L-1) mod 8] */
        CHECK_EQ_U64(frc_prefix((uint8_t)level), p);
        snprintf(text, sizeof text, "%" PRIu64, frc_prefix((uint8_t)level));
        CHECK_STR(text, ORACLE_FOUNDATION_PREFIX[level]);
        p *= S[level % 8];
    }
    CHECK_EQ_U64(frc_prefix(6), 125125);
    CHECK_EQ_U64(frc_prefix(12), UINT64_C(7319812500));
    CHECK_EQ_U64(frc_prefix(21), UINT64_C(230804667843750000));
    CHECK_EQ_U64(frc_prefix(22), UINT64_C(2538851346281250000));
    CHECK_EQ_U64(frc_prefix(23), 0); /* P(23) does not fit 64 bits */
    CHECK_EQ_U64(frc_prefix(62), 0);
    CHECK_EQ_U64(frc_prefix(255), 0);

    TEST_SECTION("hierarchy: frc_format_prefix against SPEC S16.4");
    CHECK_EQ_U64(ORACLE_FOUNDATION_PREFIX_COUNT, 64);
    for (level = 0; level < 64; level++) {
        int n = frc_format_prefix((uint8_t)level, text, sizeof text);
        CHECK_STR(text, ORACLE_FOUNDATION_PREFIX[level]);
        CHECK_EQ_I64(n, (int64_t)strlen(ORACLE_FOUNDATION_PREFIX[level]));
    }
    /* typed from the SPEC, independent of the generated list */
    frc_format_prefix(13, text, sizeof text);
    CHECK_STR(text, "51238687500");
    frc_format_prefix(62, text, sizeof text);
    CHECK_STR(text, "4708366155579433408423242709930664062500000000000000");
    CHECK_EQ_I64(frc_format_prefix(63, text, sizeof text), 53);
    CHECK_STR(text, "42375295400214900675809184389375976562500000000000000");
    strcpy(text, "untouched");
    CHECK_EQ_I64(frc_format_prefix(64, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_format_prefix(255, text, sizeof text), FRC_ERR_ARG);

    TEST_SECTION("hierarchy: frc_format_prefix in a short buffer");
    strcpy(expected, "42375295400214900675809184389375976562500000000000000");
    for (size = 0; size <= 56; size++) {
        char guarded[60];
        int n;
        memset(guarded, '#', sizeof guarded);
        n = frc_format_prefix(63, guarded, (size_t)size);
        if (size >= 54) { /* 53 digits and the NUL */
            CHECK_EQ_I64(n, 53);
            CHECK_STR(guarded, expected);
        } else {
            CHECK_EQ_I64(n, FRC_ERR_BUFFER);
            if (size > 0) { /* a NUL-terminated prefix that fills the buffer */
                CHECK_EQ_U64(strlen(guarded), (uint64_t)size - 1);
                CHECK(strncmp(guarded, expected, (size_t)size - 1) == 0);
            }
        }
        CHECK(guarded[size] == '#' || size >= 54); /* nothing is written past `size` */
        CHECK(guarded[59] == '#');
    }
    CHECK_EQ_I64(frc_format_prefix(0, NULL, 10), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_format_prefix(64, NULL, 0), FRC_ERR_ARG);

    TEST_SECTION("hierarchy: frc_i_prefix_limbs");
    for (level = 0; level <= 22; level++) {
        uint32_t limbs[FRC_I_PREFIX_LIMBS];
        uint8_t used = frc_i_prefix_limbs((uint8_t)level, limbs);
        uint64_t value = 0;
        int i;
        CHECK(used >= 1 && used <= 3);
        CHECK(limbs[used - 1] != 0); /* the top limb in use is never zero */
        for (i = used - 1; i >= 0; i--) {
            CHECK(limbs[i] < FRC_I_LIMB_BASE);
            value = value * FRC_I_LIMB_BASE + limbs[i];
        }
        CHECK_EQ_U64(value, frc_prefix((uint8_t)level));
    }
    {
        /* only the limbs in use are written: the others keep what the caller had */
        uint32_t limbs[FRC_I_PREFIX_LIMBS] = {77, 77, 77, 77, 77, 77};
        CHECK_EQ_U64(frc_i_prefix_limbs(0, limbs), 1);
        CHECK(limbs[0] == 1 && limbs[1] == 77 && limbs[5] == 77);
        CHECK_EQ_U64(frc_i_prefix_limbs(22, limbs), 3); /* 2 538851346 281250000 */
        CHECK(limbs[2] == 2 && limbs[1] == 538851346 && limbs[0] == 281250000 && limbs[3] == 77);
        CHECK_EQ_U64(frc_i_prefix_limbs(64, limbs), 0);
        CHECK(limbs[0] == 281250000 && limbs[3] == 77); /* nothing written for a bad level */
        CHECK_EQ_U64(frc_i_prefix_limbs(63, limbs), 6);
        CHECK_EQ_U64(limbs[5], 42375295);
        CHECK_EQ_U64(limbs[0], 0);
    }
}

static void test_level_names(void)
{
    /* SPEC S3.4: the alphabet, in order */
    static const char GREEK[] = "αβγδεζηθικλμνξοπρστυφχψω";
    static const char *const WORDS[24] = {
        "Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta", "Eta", "Theta", "Iota", "Kappa", "Lambda", "Mu",
        "Nu", "Xi", "Omicron", "Pi", "Rho", "Sigma", "Tau", "Upsilon", "Phi", "Chi", "Psi", "Omega"};
    static const char ASCII[] = "abgdezhqiklmnxoprstufcyw";
    char text[16], expected[32];
    int level;

    TEST_SECTION("hierarchy: level names");
    CHECK_EQ_U64(strlen(GREEK), 48); /* 24 letters of two bytes each */
    for (level = 0; level < 256; level++) {
        int letter = level % 24, round = level / 24, n;
        /* "α", then "α1" from level 24, "α2" from level 48 */
        if (round == 0) {
            snprintf(expected, sizeof expected, "%.2s", GREEK + 2 * letter);
        } else {
            snprintf(expected, sizeof expected, "%.2s%d", GREEK + 2 * letter, round);
        }
        n = frc_format_level_name((uint8_t)level, text, sizeof text);
        CHECK_STR(text, expected);
        CHECK_EQ_I64(n, (int64_t)strlen(expected));
        CHECK(n <= 5);

        if (round == 0) {
            snprintf(expected, sizeof expected, "%s", WORDS[letter]);
        } else {
            snprintf(expected, sizeof expected, "%s%d", WORDS[letter], round);
        }
        n = frc_format_level_word((uint8_t)level, text, sizeof text);
        CHECK_STR(text, expected);
        CHECK_EQ_I64(n, (int64_t)strlen(expected));
    }
    /* the names the SPEC spells out (S3.4, S16.1) */
    frc_format_level_name(0, text, sizeof text);  CHECK_STR(text, "α");
    frc_format_level_name(6, text, sizeof text);  CHECK_STR(text, "η");
    frc_format_level_name(16, text, sizeof text); CHECK_STR(text, "ρ");
    frc_format_level_name(17, text, sizeof text); CHECK_STR(text, "σ"); /* not the final sigma ς */
    frc_format_level_name(23, text, sizeof text); CHECK_STR(text, "ω");
    frc_format_level_name(24, text, sizeof text); CHECK_STR(text, "α1");
    frc_format_level_name(49, text, sizeof text); CHECK_STR(text, "β2");
    frc_format_level_name(62, text, sizeof text); CHECK_STR(text, "ο2");
    frc_format_level_name(63, text, sizeof text); CHECK_STR(text, "π2"); /* depth 64, S16.1 */
    frc_format_level_word(0, text, sizeof text);  CHECK_STR(text, "Alpha");
    frc_format_level_word(13, text, sizeof text); CHECK_STR(text, "Xi");
    frc_format_level_word(23, text, sizeof text); CHECK_STR(text, "Omega");
    frc_format_level_word(24, text, sizeof text); CHECK_STR(text, "Alpha1");
    frc_format_level_word(62, text, sizeof text); CHECK_STR(text, "Omicron2");

    /* a two-byte letter is written whole or not at all */
    CHECK_EQ_I64(frc_format_level_name(24, text, 4), 3);
    CHECK_STR(text, "α1");
    CHECK_EQ_I64(frc_format_level_name(24, text, 3), FRC_ERR_BUFFER);
    CHECK_STR(text, "α");
    strcpy(text, "x");
    CHECK_EQ_I64(frc_format_level_name(24, text, 2), FRC_ERR_BUFFER);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_format_level_name(24, text, 1), FRC_ERR_BUFFER);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_format_level_name(24, text, 0), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_format_level_name(24, NULL, 8), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_format_level_word(4, text, 8), 7);
    CHECK_STR(text, "Epsilon");
    CHECK_EQ_I64(frc_format_level_word(4, text, 7), FRC_ERR_BUFFER);
    CHECK_STR(text, "Epsilo");

    TEST_SECTION("hierarchy: Greek alphabet helpers");
    for (level = 0; level < 24; level++) {
        uint32_t cp = frc_i_greek_cp((uint8_t)level);
        /* decode the two bytes of the literal */
        uint32_t literal = (((uint32_t)(uint8_t)GREEK[2 * level] & 0x1Fu) << 6) | ((uint32_t)(uint8_t)GREEK[2 * level + 1] & 0x3Fu);
        CHECK_EQ_U64(cp, literal);
        CHECK(frc_i_greek_index_of_cp(cp) == level);
        CHECK(frc_i_greek_index_of_ascii(ASCII[level]) == level);
        CHECK_EQ_U64(frc_i_greek_cp((uint8_t)(level + 24)), cp); /* taken mod 24 */
    }
    CHECK(frc_i_greek_index_of_cp(0x03C2) == 17); /* final sigma reads as sigma */
    CHECK(frc_i_greek_index_of_cp(0x03B0) == -1);
    CHECK(frc_i_greek_index_of_cp(0x03CA) == -1);
    CHECK(frc_i_greek_index_of_cp('a') == -1);
    CHECK(frc_i_greek_index_of_ascii('j') == -1); /* j and v stand for no letter */
    CHECK(frc_i_greek_index_of_ascii('v') == -1);
    CHECK(frc_i_greek_index_of_ascii('A') == -1);
    CHECK(frc_i_greek_index_of_ascii('\0') == -1);

    TEST_SECTION("hierarchy: frc_level_kind");
    {
        /* SPEC S15.1: majors 0-3, units 4 6 8 10 12, between 5 7 9 11, sub from 13 */
        static const uint8_t KIND[16] = {0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 1, 2, 1, 3, 3, 3};
        for (level = 0; level < 256; level++) {
            CHECK_EQ_I64(frc_level_kind((uint8_t)level), level < 16 ? KIND[level] : FRC_KIND_SUB);
        }
        CHECK(frc_level_kind(3) == FRC_KIND_MAJOR && frc_level_kind(6) == FRC_KIND_UNIT);
        CHECK(frc_level_kind(7) == FRC_KIND_BETWEEN && frc_level_kind(13) == FRC_KIND_SUB);
    }
}

static void test_units(void)
{
    /* SPEC S15.1, by frc_unit_t */
    static const uint8_t LEVELS[9] = {0, 1, 2, 3, 4, 6, 8, 10, 12};
    static const uint8_t PARTS[9] = {5, 13, 5, 5, 77, 36, 65, 25, 0};
    static const char *const NAMES[9] = {"Alpha", "Beta", "Gamma", "Delta", "Nuclide", "Helion", "Preon", "Flavon", "Blink"};
    static const char *const SYMBOLS[9] = {"α", "β", "γ", "δ", "ε", "η", "ι", "λ", "ν"};
    static const uint64_t BLINKS[9] = {UINT64_C(7319812500), UINT64_C(1463962500), 112612500, 22522500, 4504500, 58500, 1625, 25, 1};
    char text[16], expected[16];
    int u, level;

    TEST_SECTION("hierarchy: units");
    CHECK_EQ_I64(FRC_UNIT_COUNT, 9);
    for (u = 0; u < 9; u++) {
        frc_unit_t unit = (frc_unit_t)u;
        CHECK_EQ_U64(frc_unit_level(unit), LEVELS[u]);
        CHECK_EQ_I64(frc_unit_of_level(LEVELS[u]), u);
        CHECK_EQ_U64(frc_unit_parts(unit), PARTS[u]);
        CHECK_EQ_U64(frc_unit_blinks(unit), BLINKS[u]);
        if (u < 8) {
            /* a unit is `parts` of the next one, and that is the product of the factors between them */
            uint64_t product = 1;
            for (level = LEVELS[u]; level < LEVELS[u + 1]; level++) {
                product *= frc_factor((uint8_t)level);
            }
            CHECK_EQ_U64(frc_unit_parts(unit), product);
            CHECK_EQ_U64(frc_unit_blinks(unit), PARTS[u] * BLINKS[u + 1]);
        }
        CHECK_EQ_I64(frc_format_unit_symbol(unit, text, sizeof text), 2);
        CHECK_STR(text, SYMBOLS[u]);
        CHECK_EQ_I64(frc_format_unit_name(unit, 0, text, sizeof text), (int64_t)strlen(NAMES[u]));
        CHECK_STR(text, NAMES[u]);
        snprintf(expected, sizeof expected, "%ss", NAMES[u]);
        CHECK_EQ_I64(frc_format_unit_name(unit, 1, text, sizeof text), (int64_t)strlen(expected));
        CHECK_STR(text, expected);
        /* the symbol is the letter of the unit's level */
        frc_format_level_name(LEVELS[u], expected, sizeof expected);
        CHECK_STR(SYMBOLS[u], expected);
    }
    for (level = 0; level < 256; level++) {
        int unit = frc_unit_of_level((uint8_t)level);
        int is_unit = level <= 4 || (level <= 12 && level % 2 == 0);
        CHECK((unit >= 0) == is_unit);
        if (unit >= 0) {
            CHECK_EQ_U64(frc_unit_level((frc_unit_t)unit), (uint64_t)level);
        }
    }
    /* not a unit */
    CHECK_EQ_U64(frc_unit_level(FRC_UNIT_COUNT), FRC_LEVEL_NONE);
    CHECK_EQ_U64(FRC_LEVEL_NONE, 255);
    CHECK_EQ_U64(frc_unit_blinks(FRC_UNIT_COUNT), 0);
    /* the same lengths by level: P(12) / P(level), nothing below the Blink */
    {
        uint8_t l;
        for (l = 0; l <= 12; l++) {
            CHECK_EQ_U64(frc_i_level_blinks(l) * frc_prefix(l), frc_prefix(12));
        }
        CHECK_EQ_U64(frc_i_level_blinks(12), 1);
        CHECK_EQ_U64(frc_i_level_blinks(13), 0);
        CHECK_EQ_U64(frc_i_level_blinks(255), 0);
    }
    CHECK_EQ_U64(frc_unit_parts(FRC_UNIT_COUNT), 0);
    strcpy(text, "x");
    CHECK_EQ_I64(frc_format_unit_symbol(FRC_UNIT_COUNT, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_format_unit_name(FRC_UNIT_COUNT, 0, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_unit_name((frc_unit_t)-1, 0, text, sizeof text), FRC_ERR_ARG);
    /* "Helions" needs 8 bytes */
    CHECK_EQ_I64(frc_format_unit_name(FRC_UNIT_HELION, 1, text, 8), 7);
    CHECK_STR(text, "Helions");
    CHECK_EQ_I64(frc_format_unit_name(FRC_UNIT_HELION, 1, text, 7), FRC_ERR_BUFFER);
    CHECK_STR(text, "Helion");
    CHECK_EQ_I64(frc_format_unit_symbol(FRC_UNIT_HELION, text, 2), FRC_ERR_BUFFER);
    CHECK_STR(text, "");
}

/* ------------------------------------------------------------------------- */
/* Cycles and thresholds                                                      */
/* ------------------------------------------------------------------------- */

/* One line of SPEC S12 for one level. */
typedef struct spec_level {
    uint8_t  level;
    int64_t  cycle;      /* -1 where the SPEC does not print it */
    uint16_t local;
    uint16_t siblings;
    uint8_t  next_name;
    int64_t  next_ms;    /* Unix milliseconds, as printed */
    uint8_t  index2;     /* 2-stroke glyph index (16 states)          */
    uint8_t  pair[2];    /* odometer digits, 2 glyphs of 2 strokes    */
} spec_level_t;

typedef struct spec_vector {
    const char  *name;
    int64_t      unix_ms;
    uint16_t     ordinal; /* floor(position) */
    spec_level_t level[2];
} spec_vector_t;

static const spec_vector_t SPEC_VECTORS[] = {
    {"A", INT64_C(1791460800000), 22,
     {{6, 717211, 0, 11, FRC_END, INT64_C(1791464172428), 13, {13, 0}},
      {7, 6454906, 7, 9, FRC_NODE, INT64_C(1791461140937), 5, {5, 4}}}},
    {"B", INT64_C(946684800000), 21,
     {{6, -1, 9, 11, FRC_NODE, INT64_C(946687783046), 5, {5, 6}},
      {7, -1, 3, 9, FRC_PEAK, INT64_C(946685256803), 0, {0, 6}}}},
    {"C", INT64_C(2886085800000), 24,
     {{6, -1, 0, 11, FRC_VALLEY, INT64_C(2886087806395), 10, {10, 3}},
      {7, -1, 5, 9, FRC_END, INT64_C(2886086290651), 12, {12, 1}}}},
};

static void test_spec_vectors(void)
{
    size_t i;
    int j;

    TEST_SECTION("SPEC S12 vectors A, B, C");
    for (i = 0; i < sizeof SPEC_VECTORS / sizeof SPEC_VECTORS[0]; i++) {
        const spec_vector_t *v = &SPEC_VECTORS[i];
        const frc_time_t t = FRC_TIME_FROM_MS(v->unix_ms);
        frc_position_t pos;

        CHECK(frc_position_at(CAT, t, &pos) == FRC_OK);
        CHECK_EQ_U64(pos.ordinal, v->ordinal);
        for (j = 0; j < 2; j++) {
            const spec_level_t *s = &v->level[j];
            frc_cycle_t c;
            frc_threshold_t th;
            frc_i_fraction_t f;

            CHECK(frc_cycle_at(CAT, t, s->level, &c) == FRC_OK);
            if (s->cycle >= 0) {
                CHECK_EQ_U64(c.cycle, (uint64_t)s->cycle);
            }
            CHECK_EQ_U64(c.level, s->level);
            CHECK_EQ_U64(c.local, s->local);
            CHECK_EQ_U64(c.siblings, s->siblings);
            CHECK_EQ_U64(c.local, c.cycle % c.siblings);
            CHECK_EQ_U64(c.clipped, 0);
            CHECK(c.start <= t && t < c.end);
            CHECK_NOTE("vector %s level %d", v->name, s->level);

            CHECK(frc_next_threshold(CAT, t, s->level, &th) == FRC_OK);
            CHECK_EQ_U64(th.name, s->next_name);
            CHECK_EQ_I64(nearest_ms(th.time), s->next_ms);
            CHECK(th.time > t);
            CHECK_EQ_U64(c.quarter, s->next_name); /* a quarter is named by the threshold that ends it */
            if (s->next_name == FRC_END) {
                CHECK_EQ_I64(th.time, c.end);
            }

            /* The glyph index and the odometer of S4.1 / S5 are the same fraction on a finer
             * grid: M = 4 states per quarter, then 4 * 16 for the second glyph. */
            CHECK(frc_i_fraction_at(CAT, t, s->level, &f) == FRC_OK);
            CHECK_EQ_U64(frc_i_fraction_step(&f, 4), s->index2);
            CHECK_EQ_U64(frc_i_fraction_step(&f, 4 * 16), (uint64_t)s->pair[0] * 16 + s->pair[1]);
            CHECK_EQ_U64(s->pair[0], s->index2); /* digit 0 is the single-glyph index */
            CHECK_EQ_U64(f.q4, s->index2 / 4);
            CHECK_EQ_U64(c.phase >> 28, s->index2); /* phase * 16 */
        }
    }

    TEST_SECTION("SPEC S12 vector A, further facts");
    {
        const frc_time_t t = FRC_TIME_FROM_S(INT64_C(1791460800));
        frc_i_fraction_t f;
        frc_threshold_t th;
        CHECK(frc_i_fraction_at(CAT, t, 6, &f) == FRC_OK);
        CHECK_EQ_U64(frc_i_fraction_step(&f, 13), 42); /* the 3-stroke index, M = 13 */
        /* S15.6: "at A the depth-7 end is 3,372.428 s away" */
        CHECK(frc_next_threshold(CAT, t, 6, &th) == FRC_OK);
        CHECK_EQ_I64(nearest_ms(th.time - t), 3372428);
    }

    TEST_SECTION("SPEC S12 anchor vector");
    {
        /* one second after the anchor eclipse: the first Helion of Alpha 6 */
        const frc_time_t t = FRC_TIME_FROM_MS(INT64_C(125593320816));
        frc_position_t pos;
        frc_cycle_t c;
        frc_i_fraction_t f;
        CHECK(frc_position_at(CAT, t, &pos) == FRC_OK);
        CHECK_EQ_U64(pos.ordinal, 20);
        CHECK_EQ_U64(pos.num, 1000000);
        CHECK(frc_cycle_at(CAT, t, 6, &c) == FRC_OK);
        CHECK_EQ_U64(c.cycle, 20 * 125125 / 4); /* anchorCycle = A * P(6) / 4, S7.1 */
        CHECK_EQ_U64(c.local, 0);
        CHECK_EQ_U64(c.siblings, 11);
        CHECK_EQ_U64(c.quarter, FRC_PEAK);
        CHECK_EQ_I64(c.start, frc_catalogue_instant(CAT, 20)); /* the eclipse starts the cycle */
        CHECK(frc_cycle_at(CAT, t, 0, &c) == FRC_OK);
        CHECK_EQ_U64(c.cycle, 5); /* Alpha 6, counted from 0 */
        CHECK_EQ_U64(c.local, 5);
        CHECK_EQ_U64(c.siblings, 18); /* S15.2: Saros 141 needs ordinals up to 18 for Alpha */
        CHECK_EQ_I64(c.start, frc_catalogue_instant(CAT, 20));
        CHECK_EQ_I64(c.end, frc_catalogue_instant(CAT, 24));
        CHECK(frc_i_fraction_at(CAT, t, 6, &f) == FRC_OK);
        CHECK_EQ_U64(frc_i_fraction_step(&f, 4 * 16), 0); /* the Helion pair reads [0, 0], S15.6 */
    }
}

static void test_cycles_against_oracle(void)
{
    int i, q;

    TEST_SECTION("cycles against the oracle");
    for (i = 0; i < ORACLE_FOUNDATION_CYCLE_COUNT; i++) {
        const oracle_foundation_cycle_t *v = &ORACLE_FOUNDATION_CYCLE[i];
        frc_cycle_t c, by_index;
        frc_threshold_t th;
        frc_time_t points[5];
        frc_i_fraction_t f;

        CHECK(frc_cycle_at(CAT, v->t, v->level, &c) == FRC_OK);
        CHECK_EQ_U64(c.level, v->level);
        CHECK_EQ_U64(c.cycle, v->cycle);
        CHECK_EQ_U64(c.quarter, v->quarter);
        CHECK_EQ_U64(c.phase, v->phase);
        CHECK_EQ_U64(c.local, v->local);
        CHECK_EQ_U64(c.siblings, v->siblings);
        CHECK_EQ_I64(c.start, v->start);
        CHECK_EQ_I64(c.end, v->end);
        CHECK_EQ_U64(c.clipped, v->clipped);
        CHECK_NOTE("cycle vector %d: t = %" PRId64 ", level %d", i, v->t, v->level);

        /* the same cycle reached by its index */
        CHECK(frc_cycle_by_index(CAT, v->level, v->cycle, &by_index) == FRC_OK);
        CHECK(by_index.level == c.level && by_index.cycle == c.cycle && by_index.local == c.local &&
              by_index.siblings == c.siblings && by_index.start == c.start && by_index.end == c.end &&
              by_index.clipped == c.clipped);
        CHECK(by_index.quarter == 0 && by_index.phase == 0);

        CHECK(frc_cycle_points(CAT, v->level, v->cycle, points) == FRC_OK);
        for (q = 0; q < 5; q++) {
            CHECK_EQ_I64(points[q], v->points[q]);
        }
        CHECK_EQ_I64(points[0], c.start);
        CHECK_EQ_I64(points[4], c.clipped ? FRC_TIME_NONE : c.end);

        CHECK(frc_next_threshold(CAT, v->t, v->level, &th) == FRC_OK);
        CHECK_EQ_U64(th.name, v->next_name);
        CHECK_EQ_I64(th.time, v->next_time);
        CHECK_NOTE("cycle vector %d: t = %" PRId64 ", level %d", i, v->t, v->level);
        /* it is the next quarter point of the running cycle, and strictly after t. AT the
         * closing instant (which reads one microsecond early) nothing is after t: no time. */
        if (v->t == frc_catalogue_last(CAT)) {
            CHECK_EQ_I64(th.time, FRC_TIME_NONE);
        } else {
            CHECK_EQ_I64(th.time, points[c.quarter + 1]);
            CHECK(th.time == FRC_TIME_NONE || th.time > v->t);
        }

        CHECK(frc_i_fraction_at(CAT, v->t, v->level, &f) == FRC_OK);
        CHECK(f.cycle == v->cycle && f.q4 == v->quarter && f.rem < f.den);
        CHECK_EQ_U64(frc_i_fraction_phase(&f), v->phase);
        CHECK(f.total.hi == (v->cycle >> 62) && f.total.lo == (v->cycle << 2) + v->quarter);
    }

    TEST_SECTION("odometer steps against the oracle");
    for (i = 0; i < ORACLE_FOUNDATION_STEP_COUNT; i++) {
        const oracle_foundation_step_t *v = &ORACLE_FOUNDATION_STEP[i];
        frc_i_fraction_t f;
        frc_time_t end;
        CHECK(frc_i_fraction_at(CAT, v->t, v->level, &f) == FRC_OK);
        CHECK_EQ_U64(frc_i_fraction_step(&f, v->w), v->step);
        end = frc_i_fraction_step_end(CAT, v->level, &f, v->w);
        CHECK_EQ_I64(end, v->next);
        CHECK_NOTE("step vector %d: t = %" PRId64 ", level %d, w = %" PRIu64, i, v->t, v->level, v->w);
        CHECK(end == FRC_TIME_NONE || end > v->t);
        if (v->t == frc_catalogue_last(CAT)) {
            CHECK_EQ_I64(end, FRC_TIME_NONE);
        }
    }
}

static void test_cycle_counts(void)
{
    /* ceil(69 * P(L) / 4) for L = 0..7, worked by hand from the prefixes of SPEC S3.3 */
    static const uint64_t COUNTS[8] = {18, 87, 1122, 5607, 28032, 196219, 2158407, 19425657};
    frc_cycle_t c;
    frc_time_t points[5];
    int level;

    TEST_SECTION("cycles: frc_cycle_count");
    for (level = 0; level < 8; level++) {
        CHECK_EQ_U64(frc_cycle_count(CAT, (uint8_t)level), COUNTS[level]);
    }
    CHECK_EQ_U64(frc_cycle_count(CAT, 21), UINT64_C(3981380520304687500));
    CHECK_EQ_U64(frc_cycle_count(CAT, 22), 0); /* beyond FRC_CLOCK_MAX_LEVEL */
    CHECK_EQ_U64(frc_cycle_count(CAT, 255), 0);
    CHECK_EQ_U64(frc_cycle_count(NULL, 0), 0);

    TEST_SECTION("cycles: first and last cycle of every level");
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        uint64_t count = frc_cycle_count(CAT, (uint8_t)level);
        /* 69 * P / 4 is whole only when P is a multiple of 4: from level 8 (factor 4 at level 7) */
        int whole = (69 * frc_prefix((uint8_t)level)) % 4 == 0;

        CHECK(frc_cycle_by_index(CAT, (uint8_t)level, 0, &c) == FRC_OK);
        CHECK_EQ_I64(c.start, frc_catalogue_first(CAT));
        CHECK_EQ_U64(c.local, 0);
        CHECK_EQ_U64(c.clipped, 0);

        CHECK(frc_cycle_by_index(CAT, (uint8_t)level, count - 1, &c) == FRC_OK);
        CHECK_EQ_I64(c.end, frc_catalogue_last(CAT)); /* clipped or not, it ends at the closing instant */
        CHECK_EQ_U64(c.clipped, whole ? 0 : 1);
        CHECK(frc_cycle_points(CAT, (uint8_t)level, count - 1, points) == FRC_OK);
        CHECK_EQ_I64(points[4], whole ? frc_catalogue_last(CAT) : FRC_TIME_NONE);
        CHECK(frc_cycle_by_index(CAT, (uint8_t)level, count, &c) == FRC_ERR_RANGE);
        CHECK(frc_cycle_points(CAT, (uint8_t)level, count, points) == FRC_ERR_RANGE);
        CHECK(frc_cycle_by_index(CAT, (uint8_t)level, UINT64_MAX, &c) == FRC_ERR_RANGE);

        /* The closing instant reads one microsecond early. Where a cycle is longer than a
         * microsecond (levels 0..18) that is still the last cycle; deeper, it is one of the
         * cycles that the last microsecond holds (3 at level 19, 15 at 20, 102 at 21). */
        CHECK(frc_cycle_at(CAT, frc_catalogue_last(CAT), (uint8_t)level, &c) == FRC_OK);
        if (level <= 18) {
            CHECK_EQ_U64(c.cycle, count - 1);
        } else {
            CHECK(c.cycle < count && count - 1 - c.cycle <= (level == 19 ? 3u : level == 20 ? 15u : 102u));
        }
        CHECK_EQ_I64(c.end, frc_catalogue_last(CAT));
        {
            frc_threshold_t th;
            CHECK(frc_next_threshold(CAT, frc_catalogue_last(CAT), (uint8_t)level, &th) == FRC_OK);
            CHECK_EQ_I64(th.time, FRC_TIME_NONE); /* nothing comes after the closing instant */
            CHECK_EQ_U64(th.name, c.quarter);
            CHECK(frc_next_threshold(CAT, frc_catalogue_last(CAT) - 1, (uint8_t)level, &th) == FRC_OK);
            CHECK(th.time == FRC_TIME_NONE || th.time == frc_catalogue_last(CAT));
        }
        CHECK(frc_cycle_at(CAT, frc_catalogue_first(CAT), (uint8_t)level, &c) == FRC_OK);
        CHECK(c.cycle == 0 && c.quarter == 0 && c.phase == 0);
    }
    /* level 0: the 18th Alpha is one Saros long instead of four (69 = 17 * 4 + 1) */
    CHECK(frc_cycle_by_index(CAT, 0, 17, &c) == FRC_OK);
    CHECK_EQ_I64(c.start, frc_catalogue_instant(CAT, 68));
    CHECK_EQ_I64(c.end, frc_catalogue_instant(CAT, 69));
    CHECK(c.clipped == 1 && c.local == 17 && c.siblings == 18);
    CHECK(frc_cycle_points(CAT, 0, 17, points) == FRC_OK);
    CHECK_EQ_I64(points[0], frc_catalogue_instant(CAT, 68));
    CHECK_EQ_I64(points[1], frc_catalogue_instant(CAT, 69)); /* its peak is the closing instant */
    CHECK(points[2] == FRC_TIME_NONE && points[3] == FRC_TIME_NONE && points[4] == FRC_TIME_NONE);
    /* an Alpha's quarter points are eclipses */
    CHECK(frc_cycle_points(CAT, 0, 5, points) == FRC_OK);
    CHECK(points[0] == frc_catalogue_instant(CAT, 20) && points[1] == frc_catalogue_instant(CAT, 21) &&
          points[2] == frc_catalogue_instant(CAT, 22) && points[3] == frc_catalogue_instant(CAT, 23) &&
          points[4] == frc_catalogue_instant(CAT, 24));
}

static void test_cycle_errors(void)
{
    frc_cycle_t c;
    frc_threshold_t th;
    frc_time_t points[5];
    frc_i_fraction_t f;
    frc_position_t pos;
    const frc_time_t first = frc_catalogue_first(CAT), last = frc_catalogue_last(CAT);

    TEST_SECTION("cycles: errors");
    CHECK(frc_cycle_at(CAT, first - 1, 6, &c) == FRC_ERR_RANGE);
    CHECK(frc_cycle_at(CAT, last + 1, 6, &c) == FRC_ERR_RANGE);
    CHECK(frc_cycle_at(CAT, 0, 22, &c) == FRC_ERR_ARG);
    CHECK(frc_cycle_at(CAT, 0, 255, &c) == FRC_ERR_ARG);
    CHECK(frc_cycle_at(CAT, last + 1, 22, &c) == FRC_ERR_ARG); /* a bad argument wins over the range */
    CHECK(frc_cycle_at(NULL, 0, 6, &c) == FRC_ERR_ARG);
    CHECK(frc_cycle_at(CAT, 0, 6, NULL) == FRC_ERR_ARG);
    CHECK(frc_cycle_by_index(CAT, 22, 0, &c) == FRC_ERR_ARG);
    CHECK(frc_cycle_by_index(NULL, 6, 0, &c) == FRC_ERR_ARG);
    CHECK(frc_cycle_by_index(CAT, 6, 0, NULL) == FRC_ERR_ARG);
    CHECK(frc_cycle_points(CAT, 22, 0, points) == FRC_ERR_ARG);
    CHECK(frc_cycle_points(NULL, 6, 0, points) == FRC_ERR_ARG);
    CHECK(frc_cycle_points(CAT, 6, 0, NULL) == FRC_ERR_ARG);
    CHECK(frc_next_threshold(CAT, first - 1, 6, &th) == FRC_ERR_RANGE);
    CHECK(frc_next_threshold(CAT, last + 1, 6, &th) == FRC_ERR_RANGE);
    CHECK(frc_next_threshold(CAT, 0, 22, &th) == FRC_ERR_ARG);
    CHECK(frc_next_threshold(NULL, 0, 6, &th) == FRC_ERR_ARG);
    CHECK(frc_next_threshold(CAT, 0, 6, NULL) == FRC_ERR_ARG);

    TEST_SECTION("cycles: internal primitives refuse bad input");
    CHECK(frc_i_fraction_at(CAT, first - 1, 6, &f) == FRC_ERR_RANGE);
    CHECK(frc_i_fraction_at(CAT, 0, 22, &f) == FRC_ERR_ARG);
    CHECK(frc_i_fraction_at(NULL, 0, 6, &f) == FRC_ERR_ARG);
    CHECK(frc_i_fraction_at(CAT, 0, 6, NULL) == FRC_ERR_ARG);
    CHECK(frc_position_at(CAT, 0, &pos) == FRC_OK);
    CHECK(frc_i_fraction(CAT, &pos, 6, &f) == FRC_OK);
    CHECK(frc_i_fraction(CAT, NULL, 6, &f) == FRC_ERR_ARG);
    CHECK(frc_i_fraction(CAT, &pos, 6, NULL) == FRC_ERR_ARG);
    pos.num = pos.den; /* not a proper fraction */
    CHECK(frc_i_fraction(CAT, &pos, 6, &f) == FRC_ERR_ARG);
    pos.num = 0;
    pos.den = 0;
    CHECK(frc_i_fraction(CAT, &pos, 6, &f) == FRC_ERR_ARG);
    pos.den = 1;
    pos.ordinal = 69; /* position 69 is the closing instant, which never reads as itself */
    CHECK(frc_i_fraction(CAT, &pos, 6, &f) == FRC_ERR_ARG);

    CHECK_EQ_I64(frc_i_time_at_quarter(CAT, 22, 0, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_i_time_at_quarter(NULL, 6, 0, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(frc_i_time_at_quarter(CAT, 0, UINT64_MAX, 4), FRC_TIME_NONE); /* 4 * cycle passes 2^64 */
    CHECK_EQ_I64(frc_i_time_at_quarter(CAT, 21, UINT64_MAX, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(v_time_at_total(CAT, 0, u128(UINT64_MAX, UINT64_MAX)), FRC_TIME_NONE);
    CHECK_EQ_I64(v_time_after_total(CAT, 6, u128(0, 0), 1, 0), FRC_TIME_NONE);
    CHECK_EQ_I64(v_time_after_total(CAT, 22, u128(0, 0), 1, 2), FRC_TIME_NONE);
    CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(UINT64_MAX, 0), 1, 2), FRC_TIME_NONE);
}

static void test_boundary_instants(void)
{
    uint16_t k;
    int i;

    TEST_SECTION("instants: level 0 quarters are eclipses");
    /* P(0) = 1: position * P == T is eclipse T */
    for (k = 0; k < 70; k++) {
        CHECK_EQ_I64(v_time_at_total(CAT, 0, u128(0, k)), frc_catalogue_instant(CAT, k));
        CHECK_EQ_I64(frc_i_time_at_quarter(CAT, 0, k / 4u, (uint8_t)(k % 4)), frc_catalogue_instant(CAT, k));
        /* an offset of a whole quarter or more is carried into the total */
        CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(0, 0), k, 1), frc_catalogue_instant(CAT, k));
        CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(0, 0), (uint64_t)k * 7, 7), frc_catalogue_instant(CAT, k));
    }
    CHECK_EQ_I64(v_time_at_total(CAT, 0, u128(0, 70)), FRC_TIME_NONE);
    CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(0, 69), 0, 5), frc_catalogue_last(CAT));
    CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(0, 69), 1, 5), FRC_TIME_NONE);
    CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(0, 68), 5, 5), frc_catalogue_last(CAT));
    CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(0, 68), 6, 5), FRC_TIME_NONE);
    CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(0, 0), UINT64_MAX, 1), FRC_TIME_NONE);

    TEST_SECTION("instants: a rational offset agrees with frc_time_at_position");
    /* At level 0, position * P == T + a/b is position (T * b + a) / b: two routes, one answer.
     * The offset crosses eclipses when a > b, which is where normalising matters. */
    for (i = 0; i < 20000; i++) {
        uint64_t b = 1 + rng_any_width() % UINT64_C(1000000000000);
        uint64_t t = rng() % 70;
        uint64_t a = rng_any_width() % (3 * b);
        CHECK_EQ_I64(v_time_after_total(CAT, 0, u128(0, t), a, b), frc_time_at_position(CAT, t * b + a, b));
        CHECK_NOTE("T = %" PRIu64 ", a = %" PRIu64 ", b = %" PRIu64, t, a, b);
    }
    /* At any level, total + a/b over P is position (total * b + a) / (P * b), while that fits 64 bits. */
    for (i = 0; i < 20000; i++) {
        uint8_t level = (uint8_t)(rng() % 13);
        uint64_t p = frc_prefix(level);
        uint64_t b = 1 + rng() % 1000;
        uint64_t total = rng() % (69 * p + 2);
        uint64_t a = rng() % (2 * b);
        CHECK_EQ_I64(v_time_after_total(CAT, level, u128(0, total), a, b),
                     frc_time_at_position(CAT, total * b + a, p * b));
        CHECK_NOTE("level %d, total = %" PRIu64 ", a = %" PRIu64 ", b = %" PRIu64, level, total, a, b);
    }
}

/*
 * The ceiling rule, as a property: the start of cycle c is the first microsecond at or
 * after the exact boundary. So reading the clock AT `start` shows cycle c (or a later one,
 * where a cycle is shorter than a microsecond), and one microsecond earlier shows an
 * earlier cycle. Where cycles are longer than a microsecond (levels 0..18; a level-18
 * cycle lasts 1.7 us, a level-19 cycle 0.35 us) that means exactly c and exactly c - 1.
 * The one exception is stated in the code: a start that rounds up to the closing instant.
 */
static void check_cycle_start(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle)
{
    frc_cycle_t c, at, before, next;
    frc_time_t points[5];
    int q;

    CHECK(frc_cycle_by_index(cat, level, cycle, &c) == FRC_OK);
    CHECK(c.cycle == cycle && c.level == level);
    CHECK(c.start <= c.end);
    CHECK(frc_cycle_at(cat, c.start, level, &at) == FRC_OK);
    if (c.start == frc_catalogue_last(cat)) {
        /* Only below a microsecond: the cycles that begin inside the catalogue's last
         * microsecond round up to the closing instant, and that instant reads one
         * microsecond EARLY (frc_position_at), so it shows an earlier cycle. */
        CHECK(level >= 19);
        CHECK(at.cycle <= cycle);
        CHECK(cycle - at.cycle <= (level == 19 ? 3u : level == 20 ? 15u : 102u));
    } else if (level <= 18) {
        CHECK_EQ_U64(at.cycle, cycle);
        if (level <= 17) { /* a quarter is longer than a microsecond too: it has just begun */
            CHECK_EQ_U64(at.quarter, FRC_PEAK);
        }
        CHECK_EQ_I64(at.start, c.start);
        CHECK_EQ_I64(at.end, c.end);
        CHECK(c.start < c.end);
    } else {
        CHECK(at.cycle >= cycle);
        /* ... but never further than the cycles one microsecond can hold */
        CHECK(at.cycle - cycle <= (level == 19 ? 3u : level == 20 ? 15u : 102u));
    }
    CHECK_NOTE("level %d, cycle %" PRIu64 ": at(start) is in cycle %" PRIu64, level, cycle, at.cycle);

    if (cycle > 0) {
        CHECK(frc_cycle_at(cat, c.start - 1, level, &before) == FRC_OK);
        if (level <= 18) {
            CHECK_EQ_U64(before.cycle, cycle - 1);
            if (level <= 17) {
                CHECK_EQ_U64(before.quarter, FRC_END);
            }
            CHECK_EQ_I64(before.end, c.start); /* [start, end) intervals tile the catalogue */
        } else {
            CHECK(before.cycle < cycle);
        }
        CHECK_NOTE("level %d, cycle %" PRIu64 ": at(start - 1) is in cycle %" PRIu64, level, cycle, before.cycle);
    } else {
        CHECK_EQ_I64(c.start, frc_catalogue_first(cat));
    }

    /* the quarter points are ordered, and the end is the next cycle's start */
    CHECK(frc_cycle_points(cat, level, cycle, points) == FRC_OK);
    CHECK_EQ_I64(points[0], c.start);
    for (q = 1; q < 5; q++) {
        CHECK(points[q] == FRC_TIME_NONE || points[q] >= points[q - 1]);
        CHECK(points[q] != FRC_TIME_NONE || c.clipped);
    }
    if (!c.clipped) {
        CHECK_EQ_I64(points[4], c.end);
        if (frc_cycle_by_index(cat, level, cycle + 1, &next) == FRC_OK) {
            CHECK_EQ_I64(next.start, c.end);
        } else {
            CHECK_EQ_I64(c.end, frc_catalogue_last(cat));
        }
    }
    /* the parent holds its children: SPEC S3.2 and the header's "navigating the tree" */
    if (level > 0) {
        frc_cycle_t parent;
        CHECK(frc_cycle_by_index(cat, (uint8_t)(level - 1), cycle / frc_siblings(level), &parent) == FRC_OK);
        CHECK(parent.start <= c.start && c.end <= parent.end);
        CHECK_EQ_U64(c.local, cycle % frc_siblings(level));
        CHECK_EQ_U64(frc_siblings(level), frc_factor((uint8_t)(level - 1)));
        if (c.local == 0) {
            CHECK_EQ_I64(parent.start, c.start);
        }
    }
}

static void test_cycle_start_property(void)
{
    int i, level;

    TEST_SECTION("cycles: a cycle's start lies in the cycle (Saros 141)");
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        uint64_t count = frc_cycle_count(CAT, (uint8_t)level);
        check_cycle_start(CAT, (uint8_t)level, 0);
        check_cycle_start(CAT, (uint8_t)level, 1);
        check_cycle_start(CAT, (uint8_t)level, count - 1);
        check_cycle_start(CAT, (uint8_t)level, count - 2);
        for (i = 0; i < 30; i++) {
            check_cycle_start(CAT, (uint8_t)level, rng() % count);
        }
    }
    /* cycles that start exactly on an eclipse, where the interval length changes */
    for (level = 0; level <= FRC_CLOCK_MAX_LEVEL; level++) {
        uint16_t k;
        for (k = 4; k < 69; k = (uint16_t)(k + 12)) {
            uint64_t cycle = (uint64_t)k / 4 * frc_prefix((uint8_t)level);
            frc_cycle_t c;
            check_cycle_start(CAT, (uint8_t)level, cycle);
            CHECK(frc_cycle_by_index(CAT, (uint8_t)level, cycle, &c) == FRC_OK);
            CHECK_EQ_I64(c.start, frc_catalogue_instant(CAT, k));
        }
    }
}

static void test_long_catalogue(void)
{
    /* Synthetic series with constant 568,972,877 s intervals. 90 eclipses: at level 21 the
     * quarter count 89 * P(21) = 2.05e19 passes 2^64, the case `total` is 128-bit for. */
    enum { LONG = 90, MANY = 400 };
    static int64_t table[MANY];
    frc_catalogue_t cat;
    frc_cycle_t c;
    frc_threshold_t th;
    frc_i_fraction_t f;
    uint64_t count;
    int i, level;

    for (i = 0; i < MANY; i++) {
        table[i] = INT64_C(-20000000000000) + i * INT64_C(568972877000);
    }

    TEST_SECTION("cycles: a 90-eclipse series at level 21");
    CHECK(frc_catalogue_init(&cat, table, LONG, 0) == FRC_OK);
    count = frc_cycle_count(&cat, 21);
    CHECK_EQ_U64(count, UINT64_C(5135403859523437500)); /* ceil(89 * P(21) / 4) */
    /* the closing instant reads a microsecond early: at most 102 level-21 cycles before the last */
    CHECK(frc_cycle_at(&cat, frc_catalogue_last(&cat), 21, &c) == FRC_OK);
    CHECK(c.cycle < count && count - 1 - c.cycle <= 102);
    CHECK_EQ_I64(c.end, frc_catalogue_last(&cat));
    CHECK(frc_i_fraction_at(&cat, frc_catalogue_last(&cat), 21, &f) == FRC_OK);
    CHECK(f.total.hi == 1); /* the quarter count really is beyond 64 bits here */
    CHECK(frc_next_threshold(&cat, frc_catalogue_last(&cat) - 1, 21, &th) == FRC_OK);
    CHECK_EQ_I64(th.time, frc_catalogue_last(&cat));
    for (level = 19; level <= 21; level++) {
        uint64_t n = frc_cycle_count(&cat, (uint8_t)level);
        check_cycle_start(&cat, (uint8_t)level, n - 1);
        for (i = 0; i < 40; i++) {
            check_cycle_start(&cat, (uint8_t)level, rng() % n);
            /* the top of the range, where the high word of the quarter count is set */
            check_cycle_start(&cat, (uint8_t)level, n - 1 - rng() % (n / 16));
        }
    }
    /* with equal intervals a level-0 cycle is exactly four of them */
    CHECK(frc_cycle_by_index(&cat, 0, 3, &c) == FRC_OK);
    CHECK_EQ_I64(c.end - c.start, 4 * INT64_C(568972877000000));

    TEST_SECTION("cycles: a catalogue too long for a level is refused, not wrapped");
    /* 321 eclipses: ceil(320 * P(21) / 4) >= 2^64. 320 eclipses still fit. */
    CHECK(frc_catalogue_init(&cat, table, 320, 0) == FRC_OK);
    CHECK(frc_cycle_count(&cat, 21) != 0);
    CHECK(frc_cycle_at(&cat, frc_catalogue_last(&cat), 21, &c) == FRC_OK);
    CHECK(c.cycle < frc_cycle_count(&cat, 21) && frc_cycle_count(&cat, 21) - 1 - c.cycle <= 102);
    CHECK(c.cycle > UINT64_C(0xFF00000000000000)); /* the index really is at the top of 64 bits */
    check_cycle_start(&cat, 21, frc_cycle_count(&cat, 21) - 1);
    CHECK(frc_catalogue_init(&cat, table, 321, 0) == FRC_OK);
    CHECK_EQ_U64(frc_cycle_count(&cat, 21), 0);
    CHECK(frc_cycle_at(&cat, 0, 21, &c) == FRC_ERR_OVERFLOW);
    CHECK(frc_cycle_by_index(&cat, 21, 0, &c) == FRC_ERR_OVERFLOW);
    CHECK(frc_next_threshold(&cat, 0, 21, &th) == FRC_ERR_OVERFLOW);
    CHECK_EQ_I64(frc_i_time_at_quarter(&cat, 21, 0, 0), FRC_TIME_NONE);
    CHECK(frc_cycle_at(&cat, 0, 20, &c) == FRC_OK); /* one level up is fine */
    CHECK(frc_catalogue_init(&cat, table, MANY, 0) == FRC_OK);
    CHECK(frc_cycle_at(&cat, 0, 21, &c) == FRC_ERR_OVERFLOW);
    CHECK(frc_cycle_at(&cat, 0, 20, &c) == FRC_OK);
    check_cycle_start(&cat, 20, frc_cycle_count(&cat, 20) - 1);
}

static void test_format_quarter(void)
{
    char text[16];

    TEST_SECTION("frc_format_quarter");
    CHECK_EQ_I64(frc_format_quarter(FRC_PEAK, 0, text, sizeof text), 4);   CHECK_STR(text, "Peak");
    CHECK_EQ_I64(frc_format_quarter(FRC_NODE, 0, text, sizeof text), 4);   CHECK_STR(text, "Node");
    CHECK_EQ_I64(frc_format_quarter(FRC_VALLEY, 0, text, sizeof text), 6); CHECK_STR(text, "Valley");
    CHECK_EQ_I64(frc_format_quarter(FRC_END, 0, text, sizeof text), 3);    CHECK_STR(text, "End");
    CHECK_EQ_I64(frc_format_quarter(FRC_PEAK, 1, text, sizeof text), 4);   CHECK_STR(text, "peak");
    CHECK_EQ_I64(frc_format_quarter(FRC_NODE, 1, text, sizeof text), 4);   CHECK_STR(text, "node");
    CHECK_EQ_I64(frc_format_quarter(FRC_VALLEY, 7, text, sizeof text), 6); CHECK_STR(text, "valley");
    CHECK_EQ_I64(frc_format_quarter(FRC_END, 1, text, sizeof text), 3);    CHECK_STR(text, "end");
    strcpy(text, "x");
    CHECK_EQ_I64(frc_format_quarter((frc_quarter_t)4, 0, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_format_quarter((frc_quarter_t)-1, 0, text, sizeof text), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_quarter(FRC_VALLEY, 0, text, 7), 6);
    CHECK_EQ_I64(frc_format_quarter(FRC_VALLEY, 0, text, 6), FRC_ERR_BUFFER);
    CHECK_STR(text, "Valle");
    CHECK_EQ_I64(frc_format_quarter(FRC_VALLEY, 0, text, 1), FRC_ERR_BUFFER);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_format_quarter(FRC_VALLEY, 0, NULL, 0), FRC_ERR_BUFFER);
}

/* ------------------------------------------------------------------------- */
/* Text                                                                       */
/* ------------------------------------------------------------------------- */

#define NNBSP "\xE2\x80\xAF" /* U+202F narrow no-break space */

static const char FLASH_GREEK[] FRC_PROGMEM = "αβγ";
static const char FLASH_MIXED[] FRC_PROGMEM = "a⬠b"; /* 1 + 3 + 1 bytes */
static const char FLASH_LIST[] FRC_PROGMEM = "Peak\0Node\0Valley\0End\0";

static void test_text_writer(void)
{
    frc_i_text_t w;
    char text[96];

    TEST_SECTION("text: appending");
    frc_i_text_init(&w, text, sizeof text);
    CHECK_STR(text, ""); /* valid from the start */
    frc_i_text_char(&w, 'L');
    frc_i_text_u64(&w, 6);
    frc_i_text_char(&w, ' ');
    frc_i_text_cp(&w, 0x03B7); /* η */
    frc_i_text_char(&w, ' ');
    frc_i_text_pgm(&w, FLASH_GREEK);
    frc_i_text_char(&w, ' ');
    frc_i_text_nth(&w, FLASH_LIST, 2);
    CHECK_EQ_I64(frc_i_text_finish(&w), 19);
    CHECK_STR(text, "L6 η αβγ Valley");
    CHECK(w.overflow == 0 && w.len == 19);

    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_cp(&w, 'A');      /* 1 byte  */
    frc_i_text_cp(&w, 0x00D7);   /* 2 bytes */
    frc_i_text_cp(&w, 0x2212);   /* 3 bytes */
    frc_i_text_cp(&w, 0x1F311);  /* 4 bytes */
    CHECK_EQ_I64(frc_i_text_finish(&w), 10);
    CHECK_STR(text, "A×−\xF0\x9F\x8C\x91");

    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_nth(&w, FLASH_LIST, 0);
    frc_i_text_nth(&w, FLASH_LIST, 3);
    frc_i_text_nth(&w, FLASH_LIST, 4); /* past the list: nothing */
    frc_i_text_nth(&w, FLASH_LIST, 255);
    frc_i_text_pgm(&w, NULL);
    CHECK_EQ_I64(frc_i_text_finish(&w), 7);
    CHECK_STR(text, "PeakEnd");

    TEST_SECTION("text: numbers");
    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_u64(&w, 0);
    frc_i_text_char(&w, ' ');
    frc_i_text_u64(&w, UINT64_MAX);
    frc_i_text_char(&w, ' ');
    frc_i_text_u64(&w, 7);
    CHECK_EQ_I64(frc_i_text_finish(&w), 1 + 1 + 20 + 1 + 1);
    CHECK_STR(text, "0 18446744073709551615 7");

    /* value / 10^decimals as JavaScript prints a number (the header's examples) */
    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_decimal(&w, 125, 2);
    frc_i_text_char(&w, ' ');
    frc_i_text_decimal(&w, 300, 2);
    frc_i_text_char(&w, ' ');
    frc_i_text_decimal(&w, 96, 4);
    frc_i_text_char(&w, ' ');
    frc_i_text_decimal(&w, 1050, 3);
    frc_i_text_char(&w, ' ');
    frc_i_text_decimal(&w, 0, 3);
    frc_i_text_char(&w, ' ');
    frc_i_text_decimal(&w, 42, 0);
    frc_i_text_char(&w, ' ');
    frc_i_text_decimal(&w, UINT64_MAX, 19);
    frc_i_text_char(&w, ' ');
    frc_i_text_decimal(&w, 1, 19);
    CHECK(frc_i_text_finish(&w) > 0);
    CHECK_STR(text, "1.25 3 0.0096 1.05 0 42 1.8446744073709551615 0.0000000000000000001");
    /* more decimals than 64 bits hold: nothing is written */
    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_decimal(&w, 5, 20);
    CHECK_EQ_I64(frc_i_text_finish(&w), 0);

    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_u64_pad(&w, 7, 3);
    frc_i_text_char(&w, ' ');
    frc_i_text_u64_pad(&w, 1234, 2); /* never truncates */
    frc_i_text_char(&w, ' ');
    frc_i_text_u64_pad(&w, 0, 0);
    frc_i_text_char(&w, ' ');
    frc_i_text_u64_pad(&w, 5, 200); /* the width is capped at 20 */
    frc_i_text_finish(&w);
    CHECK_STR(text, "007 1234 0 00000000000000000005");

    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_u64_grouped(&w, 0);
    frc_i_text_char(&w, '|');
    frc_i_text_u64_grouped(&w, 999);
    frc_i_text_char(&w, '|');
    frc_i_text_u64_grouped(&w, 1000);
    frc_i_text_char(&w, '|');
    frc_i_text_u64_grouped(&w, 58500);
    frc_i_text_char(&w, '|');
    frc_i_text_u64_grouped(&w, UINT64_C(7319812500));
    frc_i_text_finish(&w);
    CHECK_STR(text, "0|999|1" NNBSP "000|58" NNBSP "500|7" NNBSP "319" NNBSP "812" NNBSP "500");
    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_u64_grouped(&w, UINT64_MAX);
    CHECK_EQ_I64(frc_i_text_finish(&w), 20 + 6 * 3);
    CHECK_STR(text, "18" NNBSP "446" NNBSP "744" NNBSP "073" NNBSP "709" NNBSP "551" NNBSP "615");

    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_i64_super(&w, -25);
    frc_i_text_char(&w, ' ');
    frc_i_text_i64_super(&w, 1234567890);
    frc_i_text_finish(&w);
    CHECK_STR(text, "⁻²⁵ ¹²³⁴⁵⁶⁷⁸⁹⁰");
}

static void test_text_truncation(void)
{
    static const char *const PREFIXES[8] = {"", "", "", "α", "α", "αβ", "αβ", "αβγ"};
    frc_i_text_t w;
    char guarded[16];
    size_t size;

    TEST_SECTION("text: truncation never splits a UTF-8 sequence");
    /* "αβγ" is 6 bytes: with `size` bytes of buffer the result is the longest whole prefix */
    for (size = 0; size <= 7; size++) {
        int n;
        memset(guarded, '#', sizeof guarded);
        frc_i_text_init(&w, guarded, size);
        frc_i_text_pgm(&w, FLASH_GREEK);
        n = frc_i_text_finish(&w);
        if (size == 7) {
            CHECK_EQ_I64(n, 6);
        } else {
            CHECK_EQ_I64(n, FRC_ERR_BUFFER);
        }
        if (size > 0) {
            CHECK_STR(guarded, PREFIXES[size]);
        }
        CHECK(guarded[size] == '#'); /* nothing at or past buf[size] */
        CHECK(w.len < size || size == 0);

        /* the same through the code-point writer */
        memset(guarded, '#', sizeof guarded);
        frc_i_text_init(&w, guarded, size);
        frc_i_text_cp(&w, 0x03B1);
        frc_i_text_cp(&w, 0x03B2);
        frc_i_text_cp(&w, 0x03B3);
        frc_i_text_finish(&w);
        if (size > 0) {
            CHECK_STR(guarded, PREFIXES[size]);
        }
        CHECK(guarded[size] == '#');
    }

    /* a three-byte sequence in the middle */
    frc_i_text_init(&w, guarded, 4);
    frc_i_text_pgm(&w, FLASH_MIXED);
    CHECK_EQ_I64(frc_i_text_finish(&w), FRC_ERR_BUFFER);
    CHECK_STR(guarded, "a");
    frc_i_text_init(&w, guarded, 5);
    frc_i_text_pgm(&w, FLASH_MIXED);
    CHECK_EQ_I64(frc_i_text_finish(&w), FRC_ERR_BUFFER);
    CHECK_STR(guarded, "a⬠");
    frc_i_text_init(&w, guarded, 6);
    frc_i_text_pgm(&w, FLASH_MIXED);
    CHECK_EQ_I64(frc_i_text_finish(&w), 5);
    CHECK_STR(guarded, "a⬠b");

    TEST_SECTION("text: overflow is sticky, the buffer holds a prefix");
    /* "ab" + α does not fit in 4 bytes; the 'c' after it WOULD fit and must not be written,
     * or the buffer would hold "abc", which is not a prefix of "abαc" */
    frc_i_text_init(&w, guarded, 4);
    frc_i_text_char(&w, 'a');
    frc_i_text_char(&w, 'b');
    frc_i_text_cp(&w, 0x03B1);
    CHECK(w.overflow == 1);
    frc_i_text_char(&w, 'c');
    frc_i_text_u64(&w, 1);
    frc_i_text_pgm(&w, FLASH_LIST);
    CHECK_EQ_I64(frc_i_text_finish(&w), FRC_ERR_BUFFER);
    CHECK_STR(guarded, "ab");

    /* a grouped number cut at its separator */
    frc_i_text_init(&w, guarded, 4);
    frc_i_text_u64_grouped(&w, 58500);
    CHECK_EQ_I64(frc_i_text_finish(&w), FRC_ERR_BUFFER);
    CHECK_STR(guarded, "58");
    frc_i_text_init(&w, guarded, 9);
    frc_i_text_u64_grouped(&w, 58500);
    CHECK_EQ_I64(frc_i_text_finish(&w), 8);
    CHECK_STR(guarded, "58" NNBSP "500");

    /* a number that does not fit keeps its leading digits */
    frc_i_text_init(&w, guarded, 4);
    frc_i_text_u64(&w, 12345);
    CHECK_EQ_I64(frc_i_text_finish(&w), FRC_ERR_BUFFER);
    CHECK_STR(guarded, "123");

    TEST_SECTION("text: no buffer");
    memset(guarded, '#', sizeof guarded);
    frc_i_text_init(&w, guarded, 0);
    frc_i_text_char(&w, 'a');
    frc_i_text_cp(&w, 0x03B1);
    frc_i_text_pgm(&w, FLASH_GREEK);
    frc_i_text_u64_grouped(&w, 123456);
    CHECK_EQ_I64(frc_i_text_finish(&w), FRC_ERR_BUFFER);
    CHECK(guarded[0] == '#');
    frc_i_text_init(&w, NULL, 16); /* a size without a buffer is no buffer */
    frc_i_text_char(&w, 'a');
    frc_i_text_i64_super(&w, -3);
    CHECK_EQ_I64(frc_i_text_finish(&w), FRC_ERR_BUFFER);
    frc_i_text_init(&w, NULL, 0);
    CHECK_EQ_I64(frc_i_text_finish(&w), FRC_ERR_BUFFER); /* even the empty string needs its NUL */
    frc_i_text_init(&w, guarded, 1);
    CHECK_EQ_I64(frc_i_text_finish(&w), 0); /* the empty string fits one byte */
    CHECK_STR(guarded, "");

    strcpy(guarded, "x");
    CHECK_EQ_I64(frc_i_text_error(guarded, sizeof guarded, FRC_ERR_ARG), FRC_ERR_ARG);
    CHECK_STR(guarded, "");
    CHECK_EQ_I64(frc_i_text_error(NULL, 4, FRC_ERR_PARSE), FRC_ERR_PARSE);
    CHECK_EQ_I64(frc_i_text_error(guarded, 0, FRC_ERR_RANGE), FRC_ERR_RANGE);
}

static void check_ascii(const char *source, const char *expected)
{
    char text[128];
    int n = frc_to_ascii(source, text, sizeof text);
    size_t i;
    CHECK_STR(text, expected);
    CHECK_EQ_I64(n, (int64_t)strlen(expected));
    for (i = 0; text[i] != '\0'; i++) {
        CHECK((unsigned char)text[i] < 0x80);
    }
}

static void test_to_ascii(void)
{
    frc_i_text_t w;
    char text[64], ascii[64];
    int n;

    TEST_SECTION("frc_to_ascii");
    check_ascii("", "");
    check_ascii("plain ASCII stays: #5 34 ~!", "plain ASCII stays: #5 34 ~!");
    check_ascii("αβγδεζηθικλμνξοπρστυφχψω", "abgdezhqiklmnxoprstufcyw");
    /* the dates of SPEC S15.6 */
    check_ascii("Delta Trifracton End ε#5 34η 29ι", "Delta Trifracton End e#5 34h 29i");
    check_ascii("α#6 β#4 γ#9 δ#3 ε#5 34η 29ι 21λ 3ν", "a#6 b#4 g#9 d#3 e#5 34h 29i 21l 3n");
    /* spans, S15.4 */
    check_ascii("−2ε 5η", "-2e 5h");
    check_ascii("0.14ν", "0.14n");
    /* quantities of S16.1: superscripts become one caret per run */
    check_ascii("2.454 × 10⁻²⁵ s", "2.454 x 10^-25 s");
    check_ascii("1.324 × 10²⁷ Hz", "1.324 x 10^27 Hz");
    check_ascii("10⁻² 10³ x⁰¹²³⁴⁵⁶⁷⁸⁹", "10^-2 10^3 x^0123456789");
    check_ascii("10⁺⁴", "10^+4");
    check_ascii("448.7 µs", "448.7 us");   /* U+00B5 micro sign */
    check_ascii("1.555 μ", "1.555 m");     /* U+03BC Greek mu is the level letter */
    /* Pentads, S7.5 */
    check_ascii("⬠II·3", "PII.3");
    check_ascii("◻", "H");
    check_ascii("Pentad II · Delta", "Pentad II . Delta");
    /* grouped numbers and the dash of an empty band */
    check_ascii("58" NNBSP "500 Blinks", "58 500 Blinks");
    check_ascii("—", "-");
    check_ascii("a\xC2\xA0" "b\xE2\x80\x89" "c", "a b c"); /* no-break and thin spaces */
    check_ascii("ς", "s"); /* a final sigma reads as sigma */
    check_ascii("line\nbreak\ttab", "line\nbreak\ttab");
    /* anything else becomes one '?' per code point */
    check_ascii("€", "?");
    check_ascii("Ω", "?"); /* capital letters are not part of the notation */
    check_ascii("a\xF0\x9F\x8C\x91z", "a?z");
    /* malformed input: one '?' per bad byte, and decoding carries on */
    check_ascii("a\xFFz", "a?z");
    check_ascii("a\x80z", "a?z");            /* a stray continuation byte */
    check_ascii("a\xCEz", "a?z");            /* a lead byte without its continuation */
    check_ascii("a\xCE", "a?");              /* ... at the end of the string */
    check_ascii("a\xE2\x80", "a??");         /* a three-byte sequence cut short */
    check_ascii("\xC0\x80", "??");           /* an overlong NUL must not end the output */
    check_ascii("\xE0\x80\xB1", "???");      /* an overlong form of a short code point */

    TEST_SECTION("frc_to_ascii: buffer");
    strcpy(text, "x");
    CHECK_EQ_I64(frc_to_ascii(NULL, text, sizeof text), FRC_ERR_ARG);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_to_ascii("αβγδε", text, 6), 5);
    CHECK_STR(text, "abgde");
    CHECK_EQ_I64(frc_to_ascii("αβγδε", text, 4), FRC_ERR_BUFFER);
    CHECK_STR(text, "abg");
    CHECK_EQ_I64(frc_to_ascii("αβγδε", text, 1), FRC_ERR_BUFFER);
    CHECK_STR(text, "");
    CHECK_EQ_I64(frc_to_ascii("αβγδε", text, 0), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_to_ascii("αβγδε", NULL, 16), FRC_ERR_BUFFER);
    CHECK_EQ_I64(frc_to_ascii("", text, 1), 0);

    TEST_SECTION("frc_to_ascii reads back what the writer produces");
    frc_i_text_init(&w, text, sizeof text);
    frc_i_text_u64_grouped(&w, 2772);
    frc_i_text_char(&w, ' ');
    frc_i_text_cp(&w, FRC_I_CP_TIMES);
    frc_i_text_pgm(&w, FLASH_GREEK);
    frc_i_text_cp(&w, FRC_I_CP_MINUS);
    frc_i_text_u64(&w, 10);
    frc_i_text_i64_super(&w, -43);
    frc_i_text_cp(&w, FRC_I_CP_MIDDOT);
    frc_i_text_cp(&w, FRC_I_CP_PENTAGON);
    frc_i_text_cp(&w, FRC_I_CP_SQUARE);
    frc_i_text_cp(&w, FRC_I_CP_MICRO);
    frc_i_text_cp(&w, FRC_I_CP_EM_DASH);
    frc_i_text_finish(&w);
    n = frc_to_ascii(text, ascii, sizeof ascii);
    CHECK_STR(ascii, "2 772 xabg-10^-43.PHu-");
    CHECK_EQ_I64(n, 22);
    {
        /* every level name becomes its ASCII letter plus the same digits */
        int level;
        for (level = 0; level < 64; level++) {
            char expected[8];
            frc_format_level_name((uint8_t)level, text, sizeof text);
            frc_to_ascii(text, ascii, sizeof ascii);
            /* the stand-ins fractonica.h lists under frc_to_ascii */
            static const char STAND_INS[25] = "abgdezhqiklmnxoprstufcyw";
            if (level < 24) {
                snprintf(expected, sizeof expected, "%c", STAND_INS[level]);
            } else {
                snprintf(expected, sizeof expected, "%c%d", STAND_INS[level % 24], level / 24);
            }
            CHECK_STR(ascii, expected);
        }
    }
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    test_flash_accessors();
    test_math_known();
    test_math_identities();
#ifdef __SIZEOF_INT128__
    test_math_against_int128();
#endif
    test_catalogue_table();
    test_catalogue_edges();
    test_catalogue_oracle();
    test_time_at_position();
#ifndef FRC_NO_FLOAT
    test_position_double();
#endif
    test_catalogue_init();
    test_factors_and_prefixes();
    test_level_names();
    test_units();
    test_spec_vectors();
    test_cycles_against_oracle();
    test_cycle_counts();
    test_cycle_errors();
    test_boundary_instants();
    test_cycle_start_property();
    test_long_catalogue();
    test_format_quarter();
    test_text_writer();
    test_text_truncation();
    test_to_ascii();
    return TEST_SUMMARY("test_foundation");
}
