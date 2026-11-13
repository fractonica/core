/*
 * frc_math.c - portable unsigned 128-bit arithmetic.
 *
 * Implements section 2 of frc_internal.h. It backs SPEC S11 ("Numeric
 * implementation"): the clock is exact, and exactness needs products of two
 * 64-bit numbers.
 *
 * WHY IT EXISTS
 *   The targets (ESP32 xtensa / riscv, 32-bit ARM, wasm32) have no __int128,
 *   so a 128-bit value is two uint64_t and everything is built from 32-bit
 *   digits, which every C99 compiler multiplies and divides natively (64 / 64
 *   division comes from the compiler's own runtime, not from libc).
 *
 * INVARIANTS
 *   - Every function is exact; only the ones named floor / ceil round, and
 *     they round exactly as named.
 *   - Division requires n.hi < d ("the quotient fits 64 bits"). Callers prove
 *     that bound; outside it the result saturates instead of invoking
 *     undefined behaviour.
 *   - A 128-bit value is handed over BY ADDRESS, never by value: a 16-byte
 *     struct argument or return value is copied with a call to memcpy by some
 *     of the target compilers (riscv32 gcc -Os, ARMv6-M, xtensa -O0).
 *   - No state, no loops over data: constant small stack, re-entrant.
 */
#include "frc_internal.h"

#define FRC_I_LOW32 0xFFFFFFFFu

void frc_i_mul64(uint64_t a, uint64_t b, frc_i_u128_t *out)
{
    /* Schoolbook multiplication on 32-bit digits:
     *   a * b = a1*b1 * 2^64 + (a1*b0 + a0*b1) * 2^32 + a0*b0
     * `mid` sums three 32-bit quantities, so it is below 3 * 2^32 and cannot
     * overflow; its upper half is the carry into the high word. */
    uint64_t a0 = a & FRC_I_LOW32, a1 = a >> 32;
    uint64_t b0 = b & FRC_I_LOW32, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & FRC_I_LOW32) + (p10 & FRC_I_LOW32);
    out->lo = (mid << 32) | (p00 & FRC_I_LOW32);
    out->hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

int frc_i_mul64_fits(uint64_t a, uint64_t b, uint64_t *product)
{
    frc_i_u128_t wide;
    frc_i_mul64(a, b, &wide);
    if (wide.hi != 0) {
        return 0;
    }
    *product = wide.lo;
    return 1;
}

uint64_t frc_i_pow_u64(uint64_t base, uint8_t exponent)
{
    uint64_t result = 1;
    while (exponent-- > 0) {
        if (!frc_i_mul64_fits(result, base, &result)) {
            return 0;
        }
    }
    return result;
}

void frc_i_add128(frc_i_u128_t *a, const frc_i_u128_t *b)
{
    /* Both words of b are read before a is written, so b may be a itself. */
    uint64_t lo = a->lo + b->lo;
    uint64_t hi = a->hi + b->hi + (lo < b->lo ? 1u : 0u); /* unsigned wrap = carry */
    a->lo = lo;
    a->hi = hi;
}

void frc_i_add128_64(frc_i_u128_t *a, uint64_t b)
{
    a->lo += b;
    a->hi += (a->lo < b ? 1u : 0u);
}

void frc_i_sub128(frc_i_u128_t *a, const frc_i_u128_t *b)
{
    uint64_t borrow = (a->lo < b->lo) ? 1u : 0u;
    uint64_t hi = a->hi - b->hi - borrow;
    a->lo -= b->lo;
    a->hi = hi;
}

int frc_i_cmp128(const frc_i_u128_t *a, const frc_i_u128_t *b)
{
    if (a->hi != b->hi) {
        return a->hi < b->hi ? -1 : 1;
    }
    if (a->lo != b->lo) {
        return a->lo < b->lo ? -1 : 1;
    }
    return 0;
}

/* Number of leading zero bits of a non-zero value (binary search, no builtins). */
static uint8_t leading_zeros(uint64_t v)
{
    uint8_t n = 0;
    if ((v >> 32) == 0) { n = (uint8_t)(n + 32); v <<= 32; }
    if ((v >> 48) == 0) { n = (uint8_t)(n + 16); v <<= 16; }
    if ((v >> 56) == 0) { n = (uint8_t)(n + 8);  v <<= 8; }
    if ((v >> 60) == 0) { n = (uint8_t)(n + 4);  v <<= 4; }
    if ((v >> 62) == 0) { n = (uint8_t)(n + 2);  v <<= 2; }
    if ((v >> 63) == 0) { n = (uint8_t)(n + 1); }
    return n;
}

/*
 * One step of long division in base 2^32: the quotient digit of
 * (rem * 2^32 + digit) / d, where d = d1 * 2^32 + d0 is normalised (its top
 * bit is set) and rem < d. Knuth, TAOCP vol. 2, algorithm D, steps D3-D4:
 * estimate the digit from the top parts, then correct it at most twice.
 * Writes the new remainder back to *rem.
 */
static uint64_t divide_step(uint64_t *rem, uint64_t digit, uint64_t d)
{
    const uint64_t base = (uint64_t)1 << 32;
    uint64_t d1 = d >> 32, d0 = d & FRC_I_LOW32;
    uint64_t q = *rem / d1;
    uint64_t r = *rem - q * d1;
    /* q may be up to 2 too large; while it is, q * d0 exceeds what is left */
    while (q >= base || q * d0 > ((r << 32) | digit)) {
        q--;
        r += d1;
        if (r >= base) {
            break; /* the comparison can no longer hold */
        }
    }
    /* exact modulo 2^64, and the true remainder is below d < 2^64 */
    *rem = ((*rem << 32) | digit) - q * d;
    return q;
}

uint64_t frc_i_div128(const frc_i_u128_t *n, uint64_t d, uint64_t *rem)
{
    uint64_t r, q1, q0, low;
    uint8_t shift;

    if (d == 0 || n->hi >= d) { /* outside the contract: saturate */
        if (rem != NULL) {
            *rem = 0;
        }
        return UINT64_MAX;
    }
    if (n->hi == 0) { /* fits the native division */
        if (rem != NULL) {
            *rem = n->lo % d;
        }
        return n->lo / d;
    }

    /* Normalise: shift d until its top bit is set, and n by the same amount.
     * n->hi < d guarantees the shifted high word still fits 64 bits. */
    shift = leading_zeros(d);
    d <<= shift;
    r = n->hi;
    low = n->lo;
    if (shift != 0) {
        r = (r << shift) | (low >> (64 - shift));
        low <<= shift;
    }

    q1 = divide_step(&r, low >> 32, d);
    q0 = divide_step(&r, low & FRC_I_LOW32, d);

    if (rem != NULL) {
        *rem = r >> shift; /* undo the normalisation */
    }
    return (q1 << 32) | q0;
}

uint64_t frc_i_div128_ceil(const frc_i_u128_t *n, uint64_t d)
{
    uint64_t rem;
    uint64_t q = frc_i_div128(n, d, &rem);
    return q + (rem != 0 ? 1u : 0u);
}

int64_t frc_i_floor_div(int64_t a, int64_t b)
{
    /* C division truncates towards zero: step one down when it rounded up */
    int64_t q = a / b;
    return (a % b < 0) ? q - 1 : q;
}

uint64_t frc_i_muldiv_floor(uint64_t a, uint64_t b, uint64_t c, uint64_t *rem)
{
    frc_i_u128_t product;
    frc_i_mul64(a, b, &product);
    return frc_i_div128(&product, c, rem);
}

uint64_t frc_i_muldiv_ceil(uint64_t a, uint64_t b, uint64_t c)
{
    frc_i_u128_t product;
    frc_i_mul64(a, b, &product);
    return frc_i_div128_ceil(&product, c);
}
