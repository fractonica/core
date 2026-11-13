/*
 * frc_internal.h - shared internals of the Fractonica core. NOT a public header.
 *
 * Every .c file under src/ includes this file and nothing else (it pulls in
 * fractonica.h, <stdint.h> and <stddef.h>). The one exception is the spectral
 * ladder: frc_ladder.c and frc_big.c include frc_big.h (a 384-bit integer
 * nobody else needs; it includes this file in turn). It is the toolbox the
 * public API is built from. Read the eleven sections in order; each one says
 * what it gives you and what you must guarantee when you call it.
 *
 *   1. Flash access        FRC_PGM_*            read a table tagged FRC_PROGMEM
 *   2. 128-bit arithmetic  frc_i_mul64 ...      exact products and quotients
 *   3. Catalogue           frc_i_cat_* ...      instants in microseconds
 *   4. Hierarchy           frc_i_prefix_limbs   exact prefixes beyond 64 bits
 *   5. Cycle fraction      frc_i_fraction ...   THE primitive of every reading
 *   6. Instants            frc_i_time_* ...     when a boundary happens
 *   7. Text                frc_i_text_* ...     the writer behind frc_format_*
 *   8. Odometer            frc_i_odometer_* ... a step count split into glyphs
 *   9. Spans               frc_i_text_span ...  a count of Blinks as a span
 *  10. Whole numbers       frc_i_limbs_round    a number of 54 digits, rounded
 *  11. Civil calendar      frc_i_civil_weekday  the weekday of a date
 *
 * RULES FOR CODE UNDER src/ (see also the top of fractonica.h)
 *   - Include only this header. No libc, no libm: no memcpy / memset / strlen.
 *     Copy with small loops and fill structs field by field (a large struct
 *     copy or `= {0}` initialiser can make the compiler emit memcpy / memset).
 *   - A struct of 16 bytes or more (frc_i_u128_t, frc_interval_t, frc_speed_t
 *     ...) is never passed BY VALUE between functions of the core, never
 *     returned by value from an internal function and never assigned whole
 *     (`a = b`): pass a pointer and copy the fields. The public functions
 *     that take or return such a struct by value (frc_period, frc_interval_mean
 *     ...) hand its address on at once and fill their result field by field.
 *   - No heap, no recursion, no mutable static data, small stack frames.
 *   - No __int128 and no long double: use section 2.
 *   - float / double only inside #ifndef FRC_NO_FLOAT.
 *   - Every constant table is `static const ... FRC_PROGMEM` and is read ONLY
 *     through FRC_PGM_* (section 1). A direct read works on the host and on
 *     ESP32 and then silently returns garbage on AVR / ESP8266.
 *   - Public symbols are frc_*, shared internal symbols are frc_i_* and are
 *     declared here, everything else is `static`. (Three primitives declared
 *     here have one caller under src/ today: frc_i_fraction,
 *     frc_i_time_at_quarter and frc_i_prefix_limbs. They stay shared because
 *     tests/test_foundation.c holds them directly against the oracle; and
 *     frc_i_limbs_round because tests/test_pure.c does, on numbers that no
 *     rung of the ladder has.)
 *   - Compilers turn innocent code into libc calls and RAM tables. Known
 *     traps, all met while writing this core: a loop that zero-fills or
 *     copies an array becomes memset / memcpy (also a four-element one: write
 *     the stores out); a 16-byte struct passed by value is copied with memcpy
 *     by the caller on riscv32 gcc -Os and on ARMv6-M, and one that is
 *     assigned or returned is copied with memcpy by xtensa gcc -O0 (hence the
 *     rule above); a loop that scans for a NUL becomes strlen; a `switch`
 *     that maps small values to constants becomes an anonymous lookup table
 *     in RAM on AVR (use a FRC_PROGMEM table); an overflow guard
 *     `a > UINT64_MAX / b` becomes a 128-bit multiplication, which is a call
 *     to __multi3 on wasm32 (use frc_i_mul64_fits).
 *     `make check-symbols` (host), `make check-freestanding` (wasm32: the
 *     linked module must import nothing) and `make check-cross` (ARM
 *     Cortex-M3 and M0, ESP32 Xtensa and RISC-V, wasm32, AVR, each at -O0,
 *     -Og, -Os, -O2 and -O3, when the compilers are installed) catch these.
 *     `make check-progmem` catches a table that is read without FRC_PGM_*.
 *
 * NAMES USED IN THE COMMENTS
 *   E[k]   instant of eclipse k, in microseconds
 *   I_k    E[k+1] - E[k], the length of Saros interval k, in microseconds
 *   P      frc_prefix(level): level cycles per 4 Saros, so position * P counts
 *          QUARTER cycles and position * P / 4 counts cycles
 */
#ifndef FRC_INTERNAL_H
#define FRC_INTERNAL_H

#include "fractonica.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Range check for an enum (or index) argument: 1 when 0 <= value < count.
 * Use it for every frc_unit_t / frc_quarter_t / frc_medium_t ... parameter.
 * It is ONE unsigned comparison on purpose: an enum is unsigned on some ABIs
 * (arm-none-eabi packs it into a byte), where `value < 0` is a -Wtype-limits
 * error, and signed on others, where a negative value must still be refused.
 */
#define FRC_I_BELOW(value, count) ((uint32_t)(value) < (uint32_t)(count))

/* ------------------------------------------------------------------------- */
/* 1. Flash access                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Declare a table like this and read it like that:
 *
 *     static const uint8_t FACTORS[8] FRC_PROGMEM = {5, 13, 5, 5, 7, 11, 9, 4};
 *     uint8_t f = FRC_PGM_U8(&FACTORS[level & 7]);
 *
 * Each accessor takes the ADDRESS of the element and evaluates it once.
 *
 *     FRC_PGM_U8(addr)    uint8_t          FRC_PGM_I64(addr)   int64_t
 *     FRC_PGM_U16(addr)   uint16_t         FRC_PGM_U64(addr)   uint64_t
 *     FRC_PGM_U32(addr)   uint32_t         FRC_PGM_CHAR(addr)  char
 *
 * There is no accessor for a pointer on purpose: the core keeps no table of
 * pointers (see "Strings" below).
 *
 * Three back ends, chosen at compile time:
 *   default                  a plain read (ESP32, ARM, WebAssembly, hosts)
 *   FRC_PROGMEM_IS_HARVARD   pgm_read_* (AVR, ESP8266; set to 1 by
 *                            fractonica.h, or by whoever overrides FRC_PROGMEM:
 *                            the header always gives the macro a value and
 *                            refuses an override without it on those targets)
 *   FRC_TEST_FAKE_HARVARD    host test hook: every read goes through a small
 *                            inline function that assembles the value byte by
 *                            byte, so the function-call shape of the Harvard
 *                            path compiles and runs in the host tests
 *
 * Strings: keep them in flash as a PACKED LIST, one char array holding the
 * items back to back, each ended by a NUL ("Peak\0Node\0Valley\0End\0"; the
 * literal's own NUL closes the list). frc_i_packed_nth and frc_i_text_nth
 * (section 7) pick item n. A packed list needs no pointer table, so it costs
 * no relocations and no RAM anywhere.
 */
#if defined(FRC_TEST_FAKE_HARVARD)

static inline uint8_t frc_i_fake_pgm_u8(const void *addr)
{
    return *(const uint8_t *)addr;
}
/* The `size`-byte integer stored at `addr`, read one byte at a time in the
 * host's byte order. Built with shifts, not by filling a byte array: a
 * byte-copy loop is what gcc -Os turns into a call to memcpy. */
static inline uint64_t frc_i_fake_pgm_bytes(const void *addr, uint8_t size)
{
    union { uint16_t value; uint8_t bytes[2]; } probe;
    uint64_t value = 0;
    uint8_t i;
    probe.value = 1;
    for (i = 0; i < size; i++) {
        uint64_t byte = frc_i_fake_pgm_u8((const uint8_t *)addr + i);
        if (probe.bytes[0]) {
            value |= byte << (8 * i); /* little-endian: the first byte is the lowest */
        } else {
            value = (value << 8) | byte; /* big-endian: the first byte is the highest */
        }
    }
    return value;
}
#  define FRC_PGM_U8(addr)  frc_i_fake_pgm_u8(addr)
#  define FRC_PGM_U16(addr) ((uint16_t)frc_i_fake_pgm_bytes((addr), 2))
#  define FRC_PGM_U32(addr) ((uint32_t)frc_i_fake_pgm_bytes((addr), 4))
#  define FRC_PGM_U64(addr) frc_i_fake_pgm_bytes((addr), 8)

#elif FRC_PROGMEM_IS_HARVARD

/* AVR and ESP8266 are little-endian and have no 64-bit pgm read: join two
 * 32-bit reads. */
static inline uint64_t frc_i_pgm_u64(const void *addr)
{
    const uint8_t *p = (const uint8_t *)addr;
    uint64_t lo = (uint32_t)pgm_read_dword(p);
    uint64_t hi = (uint32_t)pgm_read_dword(p + 4);
    return lo | (hi << 32);
}
#  define FRC_PGM_U8(addr)  ((uint8_t)pgm_read_byte(addr))
#  define FRC_PGM_U16(addr) ((uint16_t)pgm_read_word(addr))
#  define FRC_PGM_U32(addr) ((uint32_t)pgm_read_dword(addr))
#  define FRC_PGM_U64(addr) frc_i_pgm_u64(addr)

#else

#  define FRC_PGM_U8(addr)  (*(const uint8_t *)(addr))
#  define FRC_PGM_U16(addr) (*(const uint16_t *)(addr))
#  define FRC_PGM_U32(addr) (*(const uint32_t *)(addr))
#  define FRC_PGM_U64(addr) (*(const uint64_t *)(addr))

#endif

#define FRC_PGM_I64(addr)  ((int64_t)FRC_PGM_U64(addr))
#define FRC_PGM_CHAR(addr) ((char)FRC_PGM_U8(addr))

/* ------------------------------------------------------------------------- */
/* 2. 128-bit arithmetic (frc_math.c)                                         */
/* ------------------------------------------------------------------------- */

/*
 * The clock multiplies a time inside a Saros interval (< 2^50 microseconds)
 * by a prefix (< 2^58): the product needs up to 108 bits. The targets (32-bit
 * xtensa / riscv / arm, wasm32) have no 128-bit integer, so these helpers
 * build one from two uint64_t. They are exact; nothing here rounds unless its
 * name says floor or ceil.
 */

/** An unsigned 128-bit integer: hi * 2^64 + lo. */
typedef struct frc_i_u128 {
    uint64_t hi;
    uint64_t lo;
} frc_i_u128_t;

/*
 * A frc_i_u128_t always travels BY ADDRESS: results are written through a
 * pointer, and the sums work in place (a += b). Passing or returning the
 * 16-byte struct by value compiles to a call to memcpy on riscv32 (gcc -Os),
 * on ARMv6-M and on Xtensa at -O0, and the core must not need libc. For the
 * same reason copy one with two field assignments, never with `a = b`.
 */

/** *out = a * b, exact. */
void frc_i_mul64(uint64_t a, uint64_t b, frc_i_u128_t *out);
/**
 * 1 when a * b fits 64 bits, and then *product = a * b; 0 otherwise (*product
 * is left alone). THE overflow test for a product: the usual
 * `a > UINT64_MAX / b` is rewritten by clang into a 128-bit multiplication,
 * which is a call into the compiler runtime (__multi3) on wasm32.
 */
int frc_i_mul64_fits(uint64_t a, uint64_t b, uint64_t *product);
/**
 * base^exponent, or 0 when it does not fit 64 bits (no power is 0 otherwise:
 * a zero base is not used). The one integer power of the core.
 */
uint64_t frc_i_pow_u64(uint64_t base, uint8_t exponent);

/** *a += *b (mod 2^128; the callers' bounds keep it from wrapping). b may be
 *  a itself: that doubles it. */
void frc_i_add128(frc_i_u128_t *a, const frc_i_u128_t *b);
/** *a += b for a 64-bit b. */
void frc_i_add128_64(frc_i_u128_t *a, uint64_t b);
/** *a -= *b (mod 2^128); exact when a >= b. */
void frc_i_sub128(frc_i_u128_t *a, const frc_i_u128_t *b);
/** -1, 0 or +1 as a is less than, equal to or greater than b. */
int frc_i_cmp128(const frc_i_u128_t *a, const frc_i_u128_t *b);

/**
 * floor(n / d); the remainder goes to *rem (rem may be NULL).
 * REQUIRES d != 0 and n->hi < d, which is exactly "the quotient fits 64 bits".
 * Outside that contract nothing is read out of bounds, but the answer is the
 * saturated UINT64_MAX with remainder 0: prove the bound at the call site.
 */
uint64_t frc_i_div128(const frc_i_u128_t *n, uint64_t d, uint64_t *rem);
/** ceil(n / d), same contract as frc_i_div128 (and the ceiling must fit too). */
uint64_t frc_i_div128_ceil(const frc_i_u128_t *n, uint64_t d);

/**
 * floor(a * b / c) with a 128-bit intermediate; a * b mod c goes to *rem
 * (rem may be NULL). REQUIRES c != 0 and a * b / c < 2^64. The usual proof:
 * one factor is < c, so the result is below the other factor.
 */
uint64_t frc_i_muldiv_floor(uint64_t a, uint64_t b, uint64_t c, uint64_t *rem);
/** ceil(a * b / c); same contract (the ceiling must fit 64 bits). */
uint64_t frc_i_muldiv_ceil(uint64_t a, uint64_t b, uint64_t c);

/**
 * floor(a / b) for a signed a and b > 0. C division truncates towards zero;
 * counting days before 1970 or syncs before the anchor must round DOWN.
 */
int64_t frc_i_floor_div(int64_t a, int64_t b);

/* ------------------------------------------------------------------------- */
/* 3. Catalogue (frc_catalogue.c)                                             */
/* ------------------------------------------------------------------------- */

/*
 * The table holds Unix MILLISECONDS; the clock runs on MICROSECONDS. A
 * catalogue is usable when every instant times 1000 fits int64_t (any date
 * within 292,000 years of 1970); frc_catalogue_init checks it, a hand-built
 * frc_catalogue_t promises it.
 */

/** Largest |instants_ms[k]| whose microsecond value fits frc_time_t. */
#define FRC_I_MS_LIMIT (INT64_MAX / 1000)

/** 1 when `cat` can be read: not NULL, has a table and at least two eclipses. */
static inline int frc_i_cat_ok(const frc_catalogue_t *cat)
{
    return cat != NULL && cat->instants_ms != NULL && cat->count >= 2;
}

/** E[k] in microseconds. REQUIRES frc_i_cat_ok(cat) and k < cat->count. */
static inline frc_time_t frc_i_cat_time(const frc_catalogue_t *cat, uint16_t k)
{
    return FRC_PGM_I64(&cat->instants_ms[k]) * 1000;
}

/** I_k = E[k+1] - E[k] in microseconds (> 0). REQUIRES k < cat->count - 1. */
static inline uint64_t frc_i_cat_interval(const frc_catalogue_t *cat, uint16_t k)
{
    /* unsigned subtraction: the difference of two int64 may exceed INT64_MAX */
    return (uint64_t)frc_i_cat_time(cat, (uint16_t)(k + 1)) - (uint64_t)frc_i_cat_time(cat, k);
}

/**
 * The instant of position k + r/d Saros (REQUIRES r < d): the first
 * microsecond at or after it, E[k] + ceil(r * I_k / d).
 * k == count-1 with r == 0 is the closing instant; anything beyond it is
 * FRC_TIME_NONE. REQUIRES frc_i_cat_ok(cat).
 */
frc_time_t frc_i_instant(const frc_catalogue_t *cat, uint64_t k, uint64_t r, uint64_t d);

/* ------------------------------------------------------------------------- */
/* 4. Hierarchy (frc_hierarchy.c)                                             */
/* ------------------------------------------------------------------------- */

/** Deepest level whose prefix fits uint64_t: frc_prefix(22) < 2^64 <= P(23). */
#define FRC_I_PREFIX_MAX_LEVEL 22
/** The Blink sits on level 12 (nu): the finest named unit, and the grain of
 *  dates and spans. */
#define FRC_I_BLINK_LEVEL 12
/** Deepest level frc_format_prefix and frc_i_prefix_limbs know: P(63), 53 digits. */
#define FRC_I_BIG_PREFIX_MAX_LEVEL 63
/** P(63) < 10^54: six base-10^9 limbs hold every prefix. */
#define FRC_I_PREFIX_LIMBS 6
#define FRC_I_LIMB_BASE 1000000000u

/**
 * The exact P(level) for level 0..63 as base-10^9 limbs, least significant
 * first: P = sum over i < n of limbs[i] * 10^(9 i), where n is the return
 * value. ONLY limbs[0 .. n-1] are written; the top one is non-zero.
 * @return n, the number of limbs in use (1..6), or 0 when level > 63.
 */
uint8_t frc_i_prefix_limbs(uint8_t level, uint32_t limbs[FRC_I_PREFIX_LIMBS]);

/**
 * P(to) / P(from) for from <= to <= 63, the product of the factors of the
 * levels from .. to-1, in the limbs of frc_i_prefix_limbs (from == to gives
 * 1). It is how many cycles of level `to` one cycle of level `from` holds:
 * with from = 12, the cycles per Blink of a rung below it, which pass 64
 * bits at level 36 and have 42 digits at level 62.
 * REQUIRES from <= to <= FRC_I_BIG_PREFIX_MAX_LEVEL (the product is then at
 * most P(63) and fits the six limbs). @return the number of limbs in use.
 */
uint8_t frc_i_ratio_limbs(uint8_t from, uint8_t to, uint32_t limbs[FRC_I_PREFIX_LIMBS]);

/**
 * Blinks in one cycle of `level` <= 12: P(12) / P(level), a whole number
 * because the Blink sits on level 12 and every prefix divides the next.
 * 0 for a level below the Blink (its cycle is a fraction of one).
 */
uint64_t frc_i_level_blinks(uint8_t level);

/**
 * The names of the nine units, by frc_unit_t, as a packed list in flash:
 * "Alpha" "Beta" "Gamma" "Delta" "Nuclide" "Helion" "Preon" "Flavon" "Blink".
 * The first four are also the words of the major periods in a date.
 */
extern const char frc_i_unit_names[] FRC_PROGMEM;

/* S7.5: a sync of 19 Helions is three Pentads of five and a Hypopentad of
 * four. Pentad indices are 0..3; the Hypopentad is the last. */
#define FRC_I_PENTAD_COUNT   4
#define FRC_I_PENTAD_HELIONS 5
#define FRC_I_HYPOPENTAD     3

/* ------------------------------------------------------------------------- */
/* 5. Cycle fraction (frc_cycle.c)                                            */
/* ------------------------------------------------------------------------- */

/**
 * What a level spans on a catalogue. Writes P = frc_prefix(level) and the
 * catalogue's whole extent in quarter cycles, (count - 1) * P (exact).
 * Every clock function starts here: it is the one place that validates the
 * catalogue and the level together.
 * @return FRC_ERR_ARG      cat is not usable or level > FRC_CLOCK_MAX_LEVEL
 *         FRC_ERR_OVERFLOW the level's cycle indices do not fit 64 bits on
 *                          this catalogue (more than 320 eclipses at level 21,
 *                          more than 2238 at level 20: no real series)
 */
frc_status_t frc_i_extent(const frc_catalogue_t *cat, uint8_t level,
                          uint64_t *prefix, frc_i_u128_t *extent);

/*
 * THE PRIMITIVE. Every reading of the clock (cycle index, quarter, phase,
 * glyph state, odometer digits, date fields) is one exact number seen through
 * a different grid:
 *
 *     position * P  =  total + rem / den          (0 <= rem < den)
 *
 * With position = k + num/den (frc_position_t) it is computed as
 *
 *     x     = num * P          128-bit
 *     ip    = floor(x / den)   < P, because num < den
 *     rem   = x mod den
 *     total = k * P + ip       = floor(position * P)
 *
 * `total` counts whole QUARTER cycles since position 0, because one cycle of
 * the level spans 4 / P Saros. Hence
 *
 *     cycle = total / 4        the global cycle index of SPEC S3.2
 *     q4    = total % 4        the quarter the instant is in (frc_quarter_t)
 *     (q4 + rem/den) / 4       the exact fraction of the cycle done
 *
 * and the quarter boundaries of the level are exactly the integer values of
 * position * P: the next threshold after the instant is at total + 1, and it
 * is named q4 (FRC_PEAK ends quarter 0, ... FRC_END ends quarter 3).
 *
 * A finer grid needs no new arithmetic. For w states per quarter,
 *
 *     floor((q4 + rem/den) * w) = q4 * w + floor(rem * w / den)
 *
 * is the state inside the cycle, 0 .. 4w-1 (frc_i_fraction_step):
 *     w = M                    glyph index of S4.1  (M = frc_glyph_extreme)
 *     w = M * q^(k-1)          odometer step of S5  (then split in base q)
 * and that state ends at position * P = total + (floor(rem*w/den) + 1) / w
 * (frc_i_fraction_step_end).
 *
 * `total` is 128-bit because (count-1) * P(21) passes 2^64 for a series of
 * more than 80 eclipses; it always stays below 2^66 (frc_i_extent checks),
 * so `cycle` fits 64 bits.
 *
 * THE CLOSING INSTANT. frc_position_at reads the catalogue's last instant as
 * one microsecond before itself, so a boundary can fall "after" that reading
 * and still not after the instant. frc_i_fraction_at marks such a fraction
 * `closing`, and for it every "next" instant (frc_next_threshold,
 * frc_i_fraction_step_end) is FRC_TIME_NONE: nothing changes after the end of
 * the catalogue. This is what makes the promise of fractonica.h hold
 * everywhere: a returned "next" instant is STRICTLY AFTER the query instant,
 * so "sleep until it, then read again" can never spin.
 */
typedef struct frc_i_fraction {
    frc_i_u128_t total; /* floor(position * P): quarter cycles since position 0 */
    uint64_t     cycle; /* total / 4                                           */
    uint64_t     rem;   /* position * P = total + rem / den,  0 <= rem < den   */
    uint64_t     den;   /* the Saros interval that holds the instant, in us    */
    uint8_t      q4;    /* total % 4: the quarter, 0..3                        */
    uint8_t      closing; /* 1 when taken AT the catalogue's closing instant   */
} frc_i_fraction_t;

/**
 * Fill `out` for a position of this catalogue at `level` (`closing` is 0: a
 * position does not say which instant it was read at).
 * @return FRC_ERR_ARG (bad catalogue, level > FRC_CLOCK_MAX_LEVEL, or `pos`
 *         is not a position of the catalogue), FRC_ERR_OVERFLOW (frc_i_extent).
 */
frc_status_t frc_i_fraction(const frc_catalogue_t *cat, const frc_position_t *pos,
                            uint8_t level, frc_i_fraction_t *out);
/** frc_position_at + frc_i_fraction, and sets `closing` when t is the last
 *  instant. Adds FRC_ERR_RANGE outside the catalogue. Prefer this entry point. */
frc_status_t frc_i_fraction_at(const frc_catalogue_t *cat, frc_time_t t,
                               uint8_t level, frc_i_fraction_t *out);

/** The fraction of the cycle done as 0.32 fixed point: (q4 << 30) | floor(rem * 2^30 / den). Exact floor. */
frc_frac_t frc_i_fraction_phase(const frc_i_fraction_t *f);

/**
 * The state inside the cycle on a grid of `w` states per quarter (4w per
 * cycle): floor((q4 + rem/den) * w), in 0 .. 4w-1. REQUIRES 1 <= w < 2^62.
 */
uint64_t frc_i_fraction_step(const frc_i_fraction_t *f, uint64_t w);
/**
 * The first microsecond of the NEXT state of that grid, strictly after the
 * instant `f` was taken at; FRC_TIME_NONE when it starts after the catalogue
 * or `f` is the closing instant. `cat` and `level` must be the ones `f` was
 * computed with.
 */
frc_time_t frc_i_fraction_step_end(const frc_catalogue_t *cat, uint8_t level,
                                   const frc_i_fraction_t *f, uint64_t w);

/* ------------------------------------------------------------------------- */
/* 6. Instants (frc_cycle.c)                                                  */
/* ------------------------------------------------------------------------- */

/*
 * "When is position * P == T?" for the quarter count T of a level. All three
 * return the first microsecond AT OR AFTER the exact rational instant (the
 * ceiling, see BOUNDARY INSTANTS in fractonica.h), the catalogue's closing
 * instant when T is exactly its end, and FRC_TIME_NONE beyond it or for a bad
 * catalogue / level.
 *
 * With k = T / P and r = T % P the instant is position k + r/P, that is
 * E[k] + ceil(r * I_k / P).
 */

/** Instant of position * P == *total. */
frc_time_t frc_i_time_at_total(const frc_catalogue_t *cat, uint8_t level, const frc_i_u128_t *total);

/**
 * Instant of quarter point `quarter` of cycle `cycle`: total = 4 * cycle +
 * quarter. quarter 0 is the cycle's start, 1 peak, 2 node, 3 valley, 4 its
 * end (= the start of cycle + 1). Any cycle index is accepted.
 */
frc_time_t frc_i_time_at_quarter(const frc_catalogue_t *cat, uint8_t level,
                                 uint64_t cycle, uint8_t quarter);

/**
 * Instant of position * P == *total + a/b for a rational offset a/b >= 0
 * (REQUIRES b != 0; a >= b is fine and is carried into total first).
 * Exact: with r = total % P, N = r * I_k and the real y = a * I_k / b,
 *     ceil((N + y) / P) == ceil((N + ceil(y)) / P)
 * because N and P are integers, so two chained 128-bit ceilings give the
 * true ceiling. This is how an odometer state's end is timed.
 */
frc_time_t frc_i_time_after_total(const frc_catalogue_t *cat, uint8_t level,
                                  const frc_i_u128_t *total, uint64_t a, uint64_t b);

/** "Peak" "Node" "Valley" "End" by frc_quarter_t, a packed list in flash. */
extern const char frc_i_quarter_names[] FRC_PROGMEM;

/* ------------------------------------------------------------------------- */
/* 7. Text (frc_text.c)                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Every frc_format_* function is:
 *
 *     frc_i_text_t w;
 *     frc_i_text_init(&w, buf, size);
 *     frc_i_text_...(&w, ...);            append, append, append
 *     return frc_i_text_finish(&w);       length, or FRC_ERR_BUFFER
 *
 * The writer never writes past `size` and always keeps one byte for the NUL.
 * When something does not fit, `overflow` is set and EVERY later append is
 * dropped, so the buffer holds a clean prefix of the full text: it never ends
 * in the middle of a UTF-8 sequence (a code point is appended whole or not at
 * all) and never has a hole. buf == NULL or size == 0 is safe: nothing is
 * written and finish returns FRC_ERR_BUFFER (a NULL buffer is a buffer of
 * size 0, as fractonica.h says under ERRORS).
 *
 * For an invalid argument return frc_i_text_error(buf, size, FRC_ERR_ARG):
 * it leaves an empty string in the buffer.
 */
typedef struct frc_i_text {
    char  *buf;
    size_t size;     /* capacity in bytes, NUL included; 0 when buf is NULL    */
    size_t len;      /* bytes written so far, always < size (when size > 0)    */
    int    overflow; /* sticky: 1 once an append did not fit                   */
} frc_i_text_t;

void frc_i_text_init(frc_i_text_t *w, char *buf, size_t size);
/** NUL-terminate. @return the length in bytes, or FRC_ERR_BUFFER after an overflow. */
int frc_i_text_finish(frc_i_text_t *w);
/** Store "" in buf (when there is room for it) and return `status`. */
int frc_i_text_error(char *buf, size_t size, frc_status_t status);

/** One ASCII character (c < 0x80). */
void frc_i_text_char(frc_i_text_t *w, char c);
/** One Unicode code point as UTF-8 (1..4 bytes), whole or not at all. */
void frc_i_text_cp(frc_i_text_t *w, uint32_t codepoint);
/** A NUL-terminated UTF-8 string that lives in flash (FRC_PROGMEM). */
void frc_i_text_pgm(frc_i_text_t *w, const char *flash_text);
/** Item `n` (0-based) of a packed list in flash; nothing when the list is shorter. */
void frc_i_text_nth(frc_i_text_t *w, const char *packed, uint8_t n);

/** The symbol of unit `unit` (frc_unit_t): the Greek letter of its level,
 *  "η" for the Helion. Nothing for an invalid unit. (frc_hierarchy.c) */
void frc_i_text_unit_symbol(frc_i_text_t *w, uint8_t unit);
/** The name of level `level` (S3.4), as frc_format_level_name writes it: the
 *  Greek letter of level mod 24, then the round of the alphabet when it is
 *  not the first: "η", "α1", "ο2". (frc_hierarchy.c) */
void frc_i_text_level_name(frc_i_text_t *w, uint8_t level);

/** Decimal: "0", "18446744073709551615". */
void frc_i_text_u64(frc_i_text_t *w, uint64_t value);
/** Decimal, zero-padded on the left to at least `width` digits (width <= 20). */
void frc_i_text_u64_pad(frc_i_text_t *w, uint64_t value, uint8_t width);
/** Decimal in groups of three separated by U+202F: "58 500", "7 319 812 500". */
void frc_i_text_u64_grouped(frc_i_text_t *w, uint64_t value);
/** Decimal in superscript digits, U+207B for the minus: "⁻²⁵". frc_to_ascii reads it back as "^-25". */
void frc_i_text_i64_super(frc_i_text_t *w, int64_t value);
/**
 * value / 10^decimals as JavaScript prints a number: no trailing zeros and no
 * point for a whole number. (125, 2) -> "1.25", (300, 2) -> "3",
 * (96, 4) -> "0.0096". decimals <= 19.
 */
void frc_i_text_decimal(frc_i_text_t *w, uint64_t value, uint8_t decimals);
/**
 * r, an integer of `digits` digits (1..19), with `lead` + 1 of them before the
 * decimal point (lead >= 0) and every digit kept: (3109, 4, 2) -> "310.9",
 * (1000, 4, 0) -> "1.000", (4, 1, 2) -> "400" (digits that were rounded away
 * are zeros, never a false precision).
 */
void frc_i_text_mantissa(frc_i_text_t *w, uint64_t r, uint8_t digits, int32_t lead);
/**
 * The power-of-ten form of SPEC S16.1, "d.ddd × 10ⁿ " with the space that
 * precedes the unit: r as frc_i_text_mantissa(r, digits, 0), a space, U+00D7,
 * a space, "10" and the exponent in superscript digits, a space.
 * (2454, 4, -25) -> "2.454 × 10⁻²⁵ ". The caller writes the unit.
 */
void frc_i_text_power_of_ten(frc_i_text_t *w, uint64_t r, uint8_t digits, int32_t exponent);

/** Flash address of item `n` of a packed list (of the closing empty item when the list is shorter). */
const char *frc_i_packed_nth(const char *packed, uint8_t n);

/**
 * Decode one code point of a NUL-terminated UTF-8 string in RAM.
 * @return the number of bytes used (>= 1). A malformed, overlong or truncated
 * sequence gives FRC_I_CP_REPLACEMENT and uses ONE byte, so a scan always
 * advances and never steps over the NUL (a NUL is not a continuation byte).
 */
uint8_t frc_i_utf8_decode(const char *s, uint32_t *codepoint);
#define FRC_I_CP_REPLACEMENT 0xFFFDu /* what bytes that are not UTF-8 decode to */

/* Code points the notation uses. frc_to_ascii maps each of them (see fractonica.h). */
#define FRC_I_CP_MICRO    0x00B5u /* µ  micro sign                -> u */
#define FRC_I_CP_MIDDOT   0x00B7u /* ·  middle dot                -> . */
#define FRC_I_CP_TIMES    0x00D7u /* ×  multiplication sign       -> x */
#define FRC_I_CP_EM_DASH  0x2014u /* —  "no value"                -> - */
#define FRC_I_CP_NNBSP    0x202Fu /* narrow no-break space        -> ' ' */
#define FRC_I_CP_SUP_PLUS 0x207Au /* ⁺                                 */
#define FRC_I_CP_SUP_MINUS 0x207Bu /* ⁻                                */
#define FRC_I_CP_MINUS    0x2212u /* −  minus sign                -> - */
#define FRC_I_CP_SQUARE   0x25FBu /* ◻  Hypopentad                -> H */
#define FRC_I_CP_PENTAGON 0x2B20u /* ⬠  Pentad                    -> P */

/*
 * The Greek alphabet of SPEC S3.4, by index 0..23:
 *     α β γ δ ε ζ η θ ι κ λ μ ν ξ ο π ρ σ τ υ φ χ ψ ω
 *     a b g d e z h q i k l m n x o p r s t u f c y w     (frc_to_ascii)
 * The letter of a level is index level % 24. No table is needed for the code
 * points: they are consecutive from U+03B1 except for the final sigma U+03C2,
 * which the alphabet skips.
 */
#define FRC_I_GREEK_COUNT 24
/** Code point of Greek letter `index` (taken mod 24). */
uint32_t frc_i_greek_cp(uint8_t index);
/** Index 0..23 of a Greek code point, or -1 (final sigma counts as sigma). */
int frc_i_greek_index_of_cp(uint32_t codepoint);
/** Index 0..23 of an ASCII stand-in (lower case), or -1. */
int frc_i_greek_index_of_ascii(char c);

/* ------------------------------------------------------------------------- */
/* 8. Odometer (frc_odometer.c)                                               */
/* ------------------------------------------------------------------------- */

/*
 * An odometer (SPEC S5) is a number of q^k steps written in base q, where
 * q = 4M is the number of states of one glyph and k the number of glyphs.
 * The level odometer (frc_odometer.c) and the solar-sync odometer
 * (frc_sync.c) differ only in which fraction they count: both validate their
 * arguments and split their step with the two helpers below.
 */

/**
 * Validate (strokes, digits) and give q^digits, the number of steps of one
 * full turn of the odometer, in `*steps`. steps / 4 = M * q^(digits-1) is the
 * grid "states per quarter cycle" of frc_i_fraction_step and always satisfies
 * its bound (< 2^62). Writes nothing else: a caller can still fail later and
 * leave its output untouched.
 * @return FRC_ERR_ARG      steps is NULL, strokes not 1..FRC_MAX_STROKES or
 *                          digits not 1..FRC_MAX_DIGITS
 *         FRC_ERR_OVERFLOW q^digits does not fit 64 bits (cannot happen with
 *                          the present limits: 484^4 < 2^36)
 */
frc_status_t frc_i_odometer_steps(uint8_t strokes, uint8_t digits, uint64_t *steps);

/**
 * Fill `out` for an odometer that stands at `step`: count, strokes, states,
 * and the digits coarsest first, digit[j] = floor(step / q^(digits-1-j)) mod q
 * (unused digits are 0). next_change is set to FRC_TIME_NONE; the caller
 * overwrites it when it knows better.
 * REQUIRES (strokes, digits) accepted by frc_i_odometer_steps and
 * step < q^digits.
 */
void frc_i_odometer_fill(frc_odometer_t *out, uint8_t strokes, uint8_t digits, uint64_t step);

/* ------------------------------------------------------------------------- */
/* 9. Spans (frc_span.c)                                                      */
/* ------------------------------------------------------------------------- */

/*
 * A span is whole Blinks and a 0.32 fraction of one (frc_span_t). Whoever
 * makes one from an exact rational cuts the fraction with
 * frc_i_blink_fraction, and whoever writes one inside a longer text writes it
 * with frc_i_text_span: then the text is the text of the exact span, ties
 * included (see "INVARIANTS" at the top of frc_span.c). frc_span_between and
 * frc_span_of_level do, and so does the light span of frc_pure.c.
 */

/**
 * The fraction of a Blink (whole + n/m) / 4, for whole = 0..3 quarter Blinks
 * and n < m < 2^126, as a frc_frac_t: the floor to 2^-32 Blink, or one unit
 * more when a rounding mark of frc_format_span (a multiple of 1/20000 Blink)
 * lies above that floor and at or below the exact value. The result is on the
 * exact value's side of every mark, and never reaches 1.0.
 */
frc_frac_t frc_i_blink_fraction(uint8_t whole, const frc_i_u128_t *n, const frc_i_u128_t *m);

/**
 * Append a span exactly as frc_format_span writes it (max_parts 0 means the
 * default precision, 2). Nothing is appended for a NULL span.
 */
void frc_i_text_span(frc_i_text_t *w, const frc_span_t *span, uint8_t max_parts);

/**
 * The instant the ARITHMETIC of frc_span_next_change (t < anchor, anchor the
 * boundary) and frc_span_next_change_since (t >= anchor, anchor the origin)
 * places the next change of the text at, before the formatter checks it: the
 * class's low (or high) edge mapped to an instant. The public functions use
 * it to narrow their search to a few microseconds; the tests hold it within
 * three microseconds of their answers, so a wrong guess, which the search
 * would hide by being slow, is found. FRC_TIME_NONE where frc_span_between
 * fails at `t` or the edge cannot be placed.
 */
frc_time_t frc_i_span_change_guess(const frc_catalogue_t *cat, frc_time_t t, frc_time_t anchor, uint8_t max_parts);

/**
 * The speed of light in vacuum in metres per second, exact by the definition
 * of the metre (SPEC S16.3): the first medium of frc_medium_speed, and the c
 * of frc_format_light_span.
 */
#define FRC_I_LIGHT_SPEED 299792458u

/* ------------------------------------------------------------------------- */
/* 10. Whole numbers beyond 64 bits (frc_pure.c)                              */
/* ------------------------------------------------------------------------- */

/**
 * A whole number N > 0 held in `used` base-10^9 limbs (frc_i_prefix_limbs,
 * frc_i_ratio_limbs; the top limb non-zero), rounded to `digits` significant
 * digits (1..18), HALF TO EVEN from the exact N:
 *     N  ~  r * 10^(*exponent - digits + 1)      10^(digits-1) <= r < 10^digits
 * so *exponent is the decimal exponent of the rounded value, what
 * frc_i_text_power_of_ten takes. A number with fewer digits than asked is
 * exact and r ends in zeros (7 at three digits is 700, exponent 0). A number
 * that rounds up to 10^digits (9 996 000 at three digits) comes back as
 * 10^(digits-1) with the exponent raised: 1.00 x 10^7.
 */
uint64_t frc_i_limbs_round(const uint32_t *limbs, uint8_t used, uint8_t digits, int32_t *exponent);

/* ------------------------------------------------------------------------- */
/* 11. Civil calendar (frc_civil.c)                                           */
/* ------------------------------------------------------------------------- */

/**
 * The weekday of a proleptic-Gregorian date, 0 = Monday .. 6 = Sunday (as
 * frc_civil_t.weekday). REQUIRES month 1..12 and |year| <= 290000; the day
 * may be any value (it is counted on from the 1st). frc_earth.c writes and
 * checks weekdays with it.
 */
uint8_t frc_i_civil_weekday(int32_t year, uint8_t month, uint8_t day);

#ifdef __cplusplus
}
#endif

#endif /* FRC_INTERNAL_H */
