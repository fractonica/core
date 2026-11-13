/*
 * frc_big.c - the fixed-width unsigned big integer of frc_big.h.
 *
 * It backs SPEC S16.4 ("Numeric implementation"): the ladder is made of exact
 * integers that pass 128 bits, and every result is rounded once from them.
 *
 * INVARIANTS
 *   - A value is twelve 32-bit limbs, least significant first; every function
 *     reads and writes all twelve, so there is no length to keep in step.
 *   - Arithmetic is modulo 2^384. The callers prove their values fit.
 *   - Schoolbook algorithms on 32-bit digits with 64-bit intermediates, which
 *     every C99 compiler has natively. Division is shift-and-subtract, one
 *     bit of the dividend per step: slow (a few thousand limb operations) and
 *     small, which is the right trade for a path that runs once per text.
 *   - No function copies or clears a value with a plain loop: see "WHY THERE
 *     IS NO COPY AND NO CLEAR" in frc_big.h.
 */
#include "frc_big.h"

#define LOW32 0xFFFFFFFFu

void frc_i_big_set_u64(frc_i_big_t *a, uint64_t value)
{
    /* `rest` is volatile ON PURPOSE. Without it a compiler sees that limbs
     * 2..11 are always zero, merges those ten stores and, on targets where 40
     * bytes is past its inline limit, emits a call to memset: riscv32 clang
     * does at every level, arm-none-eabi-gcc at -O2 (checked while writing
     * this). A value read from a volatile object is unknown to every
     * optimiser, so these stay twelve ordinary stores. */
    volatile uint64_t rest = value;
    uint8_t i;
    for (i = 0; i < FRC_I_BIG_LIMBS; i++) {
        uint64_t now = rest;
        a->limb[i] = (uint32_t)(now & LOW32);
        rest = now >> 32;
    }
}

uint64_t frc_i_big_low_u64(const frc_i_big_t *a)
{
    return ((uint64_t)a->limb[1] << 32) | a->limb[0];
}

uint16_t frc_i_big_bits(const frc_i_big_t *a)
{
    uint8_t i = FRC_I_BIG_LIMBS;
    while (i > 0) {
        uint32_t top = a->limb[--i];
        if (top != 0) {
            uint16_t bits = (uint16_t)(32u * i);
            while (top != 0) {
                bits++;
                top >>= 1;
            }
            return bits;
        }
    }
    return 0;
}

int frc_i_big_cmp(const frc_i_big_t *a, const frc_i_big_t *b)
{
    uint8_t i = FRC_I_BIG_LIMBS;
    while (i > 0) {
        i--;
        if (a->limb[i] != b->limb[i]) {
            return a->limb[i] < b->limb[i] ? -1 : 1;
        }
    }
    return 0;
}

void frc_i_big_mul_u64(frc_i_big_t *dst, const frc_i_big_t *src, uint64_t m)
{
    /* With m = m1 * 2^32 + m0, limb i of the product collects
     *     src[i] * m0  +  src[i-1] * m1  +  carry
     * Each product is below 2^64, so the two are summed in halves: the low
     * halves (three 32-bit terms, below 3 * 2^32) give the limb and a small
     * carry; the high halves and that carry (below 2^34 in all) are the next
     * carry. `below` keeps src[i-1] after dst[i-1] was written, which is what
     * lets dst be src. */
    uint64_t m0 = m & LOW32, m1 = m >> 32;
    uint64_t carry = 0;
    uint32_t below = 0;
    uint8_t i;
    for (i = 0; i < FRC_I_BIG_LIMBS; i++) {
        uint32_t here = src->limb[i];
        uint64_t p0 = here * m0;
        uint64_t p1 = below * m1;
        uint64_t low = (p0 & LOW32) + (p1 & LOW32) + (carry & LOW32);
        dst->limb[i] = (uint32_t)(low & LOW32);
        carry = (p0 >> 32) + (p1 >> 32) + (carry >> 32) + (low >> 32);
        below = here;
    }
}

void frc_i_big_shl(frc_i_big_t *dst, const frc_i_big_t *src, uint16_t bits)
{
    uint16_t whole = (uint16_t)(bits / 32u); /* limbs to move up */
    uint8_t part = (uint8_t)(bits % 32u);    /* bits to move inside a limb */
    uint8_t i = FRC_I_BIG_LIMBS;
    /* From the top limb down: dst[i] is made of src[i - whole] and the limb
     * below it, which a downward pass has not overwritten yet when dst is src. */
    while (i > 0) {
        uint32_t limb = 0;
        i--;
        if (i >= whole) {
            limb = src->limb[i - whole] << part;
            if (part != 0 && i > whole) {
                /* part is 1..31 here: a shift by 32 would be undefined */
                limb |= src->limb[i - whole - 1] >> (32u - part);
            }
        }
        dst->limb[i] = limb;
    }
}

/* r = 2 r + bit. REQUIRES the result to fit (see frc_i_big_divmod). */
static void shift_in(frc_i_big_t *r, uint32_t bit)
{
    uint8_t i;
    for (i = 0; i < FRC_I_BIG_LIMBS; i++) {
        uint32_t out = r->limb[i] >> 31;
        r->limb[i] = (r->limb[i] << 1) | bit;
        bit = out;
    }
}

/* r = r - d. REQUIRES r >= d. */
static void subtract(frc_i_big_t *r, const frc_i_big_t *d)
{
    uint32_t borrow = 0;
    uint8_t i;
    for (i = 0; i < FRC_I_BIG_LIMBS; i++) {
        /* 2^32 + r[i] - d[i] - borrow is in 1 .. 2^33 - 1: bit 32 clear means
         * the limb had to borrow */
        uint64_t v = (((uint64_t)1 << 32) | r->limb[i]) - d->limb[i] - borrow;
        r->limb[i] = (uint32_t)(v & LOW32);
        borrow = (uint32_t)(1u - (uint32_t)(v >> 32));
    }
}

void frc_i_big_divmod(const frc_i_big_t *n, const frc_i_big_t *d, frc_i_big_t *q, frc_i_big_t *r)
{
    /* Long division in base 2. Before each step r < d; the step brings down
     * the next bit of n (r = 2 r + bit < 2 d) and takes d off once if it
     * fits, which is that step's quotient bit.
     * 2 r + bit cannot overflow: r is what is left of the bits of n read so
     * far after whole multiples of d were taken off, so it never exceeds
     * those leading bits of n, and they are a 384-bit number. */
    uint16_t bit = frc_i_big_bits(n);
    frc_i_big_set_u64(q, 0);
    frc_i_big_set_u64(r, 0);
    while (bit > 0) {
        bit--;
        shift_in(r, (n->limb[bit / 32u] >> (bit % 32u)) & 1u);
        if (frc_i_big_cmp(r, d) >= 0) {
            subtract(r, d);
            q->limb[bit / 32u] |= (uint32_t)1 << (bit % 32u);
        }
    }
}

int frc_i_big_cmp_half(const frc_i_big_t *r, const frc_i_big_t *d)
{
    /* Compare 2 r with d limb by limb from the top, making each limb of 2 r
     * on the way, so no temporary is needed. */
    uint8_t i = FRC_I_BIG_LIMBS;
    if ((r->limb[FRC_I_BIG_LIMBS - 1] >> 31) != 0) {
        return 1; /* 2 r does not fit 384 bits, d does */
    }
    while (i > 0) {
        uint32_t twice;
        i--;
        twice = r->limb[i] << 1;
        if (i > 0) {
            twice |= r->limb[i - 1] >> 31;
        }
        if (twice != d->limb[i]) {
            return twice < d->limb[i] ? -1 : 1;
        }
    }
    return 0;
}
