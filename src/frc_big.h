/*
 * frc_big.h - a fixed-width unsigned big integer. NOT a public header.
 *
 * WHY IT EXISTS
 *   The spectral ladder (SPEC S16) is exact: P(62) alone has 172 bits, and a
 *   period, a frequency or a length is a quotient of such numbers scaled by
 *   64-bit intervals, 64-bit speeds and powers of ten. No 64-bit or 128-bit
 *   integer holds them, so frc_ladder.c computes on this type. Nothing else
 *   in the core needs it: the clock itself stays on section 2 of
 *   frc_internal.h.
 *
 * WHAT IT IS
 *   FRC_I_BIG_BITS = 384 bits in twelve 32-bit limbs, least significant
 *   first, on the caller's stack (48 bytes). No heap, no length field, no
 *   sign. Only the operations the ladder uses exist.
 *
 * WHAT YOU MUST GUARANTEE
 *   Arithmetic is modulo 2^384 and NOTHING REPORTS AN OVERFLOW. The caller
 *   proves that every value fits; frc_ladder.c states its bounds where it
 *   builds each number (the largest is below 2^368).
 *
 * WHY THERE IS NO COPY AND NO CLEAR
 *   A loop that copies or zero-fills 48 bytes is rewritten by optimisers into
 *   a call to memcpy / memset, which a freestanding build does not have. So:
 *   frc_i_big_mul_u64 and frc_i_big_shl write a DESTINATION from a source
 *   (multiply by 1 or shift by 0 if you ever need a plain copy), and
 *   frc_i_big_set_u64 is the only function that fills a value from nothing.
 */
#ifndef FRC_BIG_H
#define FRC_BIG_H

#include "frc_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FRC_I_BIG_LIMBS 12
#define FRC_I_BIG_BITS  (32 * FRC_I_BIG_LIMBS)

/** value = sum of limb[i] * 2^(32 i). */
typedef struct frc_i_big {
    uint32_t limb[FRC_I_BIG_LIMBS];
} frc_i_big_t;

/** a = value. */
void frc_i_big_set_u64(frc_i_big_t *a, uint64_t value);
/** The low 64 bits of a (all of it when frc_i_big_bits(a) <= 64). */
uint64_t frc_i_big_low_u64(const frc_i_big_t *a);
/** Bit length: 0 for zero, otherwise floor(log2(a)) + 1. */
uint16_t frc_i_big_bits(const frc_i_big_t *a);
/** -1, 0 or +1 as a is less than, equal to or greater than b. */
int frc_i_big_cmp(const frc_i_big_t *a, const frc_i_big_t *b);

/** dst = src * m. dst may be src. */
void frc_i_big_mul_u64(frc_i_big_t *dst, const frc_i_big_t *src, uint64_t m);
/** dst = src * 2^bits. dst may be src. */
void frc_i_big_shl(frc_i_big_t *dst, const frc_i_big_t *src, uint16_t bits);

/**
 * q = floor(n / d) and r = n mod d. REQUIRES d != 0 (with d == 0 the result
 * is meaningless, but nothing traps or loops). q and r must be distinct
 * objects, and neither may be n or d.
 */
void frc_i_big_divmod(const frc_i_big_t *n, const frc_i_big_t *d, frc_i_big_t *q, frc_i_big_t *r);

/**
 * How a remainder compares with half its divisor: the sign of 2 r - d, as
 * -1, 0 or +1. REQUIRES r < d. This is all a rounding needs to know about the
 * fraction r / d that a division dropped: below, exactly, or above one half.
 */
int frc_i_big_cmp_half(const frc_i_big_t *r, const frc_i_big_t *d);

#ifdef __cplusplus
}
#endif

#endif /* FRC_BIG_H */
