/*
 * frc_hierarchy.c - the static facts of the harmonic hierarchy: factors,
 * prefixes, level names and the nine named units.
 *
 * Implements SPEC S3.1 (factors and prefixes), S3.4 (period names), S15.1
 * (major periods and minor units) and the exact prefix list of S16.4.
 * Section 4 of frc_internal.h is implemented here. Nothing in this file
 * looks at a catalogue or at a clock: it is all constants.
 *
 * THE MODEL
 *   S = [5, 13, 5, 5, 7, 11, 9, 4], repeating.
 *   One cycle of level L holds S[L mod 8] cycles of level L+1  (frc_factor).
 *   P(0) = 1, P(L) = P(L-1) * S[(L-1) mod 8]                   (frc_prefix).
 *   P(L) is the number of level-L cycles in 4 Saros.
 *
 * INVARIANTS
 *   - THE API TAKES LEVELS, zero-based. depth = level + 1 never appears here.
 *     siblings(L) = S[(L-1) mod 8]: the factor of the PARENT (SPEC pitfall 6).
 *   - P(L) fits uint64_t up to level 22 (P(22) = 2.5e18 < 2^64 = 1.8e19 <
 *     P(23)); frc_prefix returns 0 beyond that instead of a wrapped value.
 *   - P(63) has 53 digits. frc_i_prefix_limbs builds any P(0..63) exactly in
 *     at most six base-10^9 limbs on the stack (24 bytes): each step
 *     multiplies by a factor <= 13, so limb * factor + carry stays below 2^34
 *     in a uint64_t, and the carry out of the top limb opens the next one.
 *     frc_i_ratio_limbs is the same product started at any level: P(to) /
 *     P(from), which is how many cycles of one level a cycle of another holds.
 *   - A unit's length in Blinks is P(12) / P(unit level), exact, because the
 *     Blink sits on level 12 and every unit level is at or above it.
 */
#include "frc_internal.h"

/* ------------------------------------------------------------------------- */
/* S3.1  Factors and prefixes                                                 */
/* ------------------------------------------------------------------------- */

static const uint8_t FACTORS[FRC_SEQUENCE_LENGTH] FRC_PROGMEM = {5, 13, 5, 5, 7, 11, 9, 4};

/* P(0) .. P(22), the list of SPEC S16.4. A table, not a loop: the clock asks
 * for a prefix on every reading. tests/test_foundation.c checks it against
 * the recurrence and against the SPEC's decimal strings. */
static const uint64_t PREFIXES[FRC_I_PREFIX_MAX_LEVEL + 1] FRC_PROGMEM = {
    1ULL,                   /*  0 alpha            */
    5ULL,                   /*  1 beta             */
    65ULL,                  /*  2 gamma            */
    325ULL,                 /*  3 delta            */
    1625ULL,                /*  4 epsilon  Nuclide */
    11375ULL,               /*  5 zeta             */
    125125ULL,              /*  6 eta      Helion  */
    1126125ULL,             /*  7 theta            */
    4504500ULL,             /*  8 iota     Preon   */
    22522500ULL,            /*  9 kappa            */
    292792500ULL,           /* 10 lambda   Flavon  */
    1463962500ULL,          /* 11 mu               */
    7319812500ULL,          /* 12 nu       Blink   */
    51238687500ULL,         /* 13 xi               */
    563625562500ULL,        /* 14 omicron          */
    5072630062500ULL,       /* 15 pi               */
    20290520250000ULL,      /* 16 rho              */
    101452601250000ULL,     /* 17 sigma            */
    1318883816250000ULL,    /* 18 tau              */
    6594419081250000ULL,    /* 19 upsilon          */
    32972095406250000ULL,   /* 20 phi              */
    230804667843750000ULL,  /* 21 chi   = FRC_CLOCK_MAX_LEVEL */
    2538851346281250000ULL  /* 22 psi   the last that fits 64 bits */
};

uint8_t frc_factor(uint8_t level)
{
    return FRC_PGM_U8(&FACTORS[level % FRC_SEQUENCE_LENGTH]);
}

uint8_t frc_siblings(uint8_t level)
{
    /* level 0 has no parent, hence no siblings */
    return level == 0 ? 0 : frc_factor((uint8_t)(level - 1));
}

uint64_t frc_prefix(uint8_t level)
{
    if (level > FRC_I_PREFIX_MAX_LEVEL) {
        return 0;
    }
    return FRC_PGM_U64(&PREFIXES[level]);
}

uint8_t frc_i_ratio_limbs(uint8_t from, uint8_t to, uint32_t limbs[FRC_I_PREFIX_LIMBS])
{
    uint8_t i, step;
    uint8_t used = 1; /* limbs in use; the number grows a limb at a time */

    limbs[0] = 1;
    /* P(to) / P(from) = S[from] * S[from+1] * ... * S[to-1]: one short
     * multiplication per step, over the limbs in use only. (No limb is
     * zero-filled in advance: a fill loop is what compilers turn into a call
     * to memset.) */
    for (step = from; step < to; step++) {
        uint32_t factor = frc_factor(step);
        uint32_t carry = 0;
        for (i = 0; i < used; i++) {
            /* < 10^9 * 13 + 13 < 2^34 */
            uint64_t v = (uint64_t)limbs[i] * factor + carry;
            limbs[i] = (uint32_t)(v % FRC_I_LIMB_BASE);
            carry = (uint32_t)(v / FRC_I_LIMB_BASE);
        }
        if (carry != 0) {
            /* the product is at most P(63) < 10^54 (to <= 63), so a seventh
             * limb is never needed */
            limbs[used++] = carry;
        }
    }
    return used;
}

uint8_t frc_i_prefix_limbs(uint8_t level, uint32_t limbs[FRC_I_PREFIX_LIMBS])
{
    if (level > FRC_I_BIG_PREFIX_MAX_LEVEL) {
        return 0;
    }
    return frc_i_ratio_limbs(0, level, limbs); /* P(level) = P(level) / P(0) */
}

int frc_format_prefix(uint8_t level, char *buf, size_t size)
{
    uint32_t limbs[FRC_I_PREFIX_LIMBS];
    frc_i_text_t w;
    uint8_t used = frc_i_prefix_limbs(level, limbs);

    if (used == 0) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    /* the top limb as it is, every lower limb as exactly nine digits */
    frc_i_text_u64(&w, limbs[used - 1]);
    while (used > 1) {
        used--;
        frc_i_text_u64_pad(&w, limbs[used - 1], 9);
    }
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* S3.4, S15.1  Names of levels                                               */
/* ------------------------------------------------------------------------- */

/* The level on which each unit sits, by frc_unit_t. */
static const uint8_t UNIT_LEVELS[FRC_UNIT_COUNT] FRC_PROGMEM = {0, 1, 2, 3, 4, 6, 8, 10, 12};

/* How many of the next finer unit make one of this unit (S15.1 "Made of"):
 * the product of the factors between the two unit levels. */
static const uint8_t UNIT_PARTS[FRC_UNIT_COUNT] FRC_PROGMEM = {5, 13, 5, 5, 77, 36, 65, 25, 0};

/* Packed lists (see frc_internal.h section 1). The unit names are shared
 * with the date writer and parser of frc_notation.c. */
const char frc_i_unit_names[] FRC_PROGMEM =
    "Alpha\0Beta\0Gamma\0Delta\0Nuclide\0Helion\0Preon\0Flavon\0Blink\0";
static const char GREEK_WORDS[] FRC_PROGMEM =
    "Alpha\0Beta\0Gamma\0Delta\0Epsilon\0Zeta\0Eta\0Theta\0Iota\0Kappa\0Lambda\0Mu\0"
    "Nu\0Xi\0Omicron\0Pi\0Rho\0Sigma\0Tau\0Upsilon\0Phi\0Chi\0Psi\0Omega\0";

frc_level_kind_t frc_level_kind(uint8_t level)
{
    if (level <= 3) {
        return FRC_KIND_MAJOR;
    }
    if (level > FRC_I_BLINK_LEVEL) {
        return FRC_KIND_SUB;
    }
    /* between Delta and the Blink the units take every other level: 4 6 8 10 12 */
    return (level % 2 == 0) ? FRC_KIND_UNIT : FRC_KIND_BETWEEN;
}

/* The numeric suffix of S3.4: nothing for the first alphabet, then "1", "2", ... */
static void put_alphabet_round(frc_i_text_t *w, uint8_t level)
{
    uint8_t round = (uint8_t)(level / FRC_I_GREEK_COUNT);
    if (round != 0) {
        frc_i_text_u64(w, round);
    }
}

void frc_i_text_level_name(frc_i_text_t *w, uint8_t level)
{
    frc_i_text_cp(w, frc_i_greek_cp((uint8_t)(level % FRC_I_GREEK_COUNT)));
    put_alphabet_round(w, level);
}

int frc_format_level_name(uint8_t level, char *buf, size_t size)
{
    frc_i_text_t w;
    frc_i_text_init(&w, buf, size);
    frc_i_text_level_name(&w, level);
    return frc_i_text_finish(&w);
}

int frc_format_level_word(uint8_t level, char *buf, size_t size)
{
    frc_i_text_t w;
    frc_i_text_init(&w, buf, size);
    frc_i_text_nth(&w, GREEK_WORDS, (uint8_t)(level % FRC_I_GREEK_COUNT));
    put_alphabet_round(&w, level);
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* S15.1  The nine units                                                      */
/* ------------------------------------------------------------------------- */

static int unit_ok(frc_unit_t unit)
{
    return FRC_I_BELOW(unit, FRC_UNIT_COUNT);
}

uint8_t frc_unit_level(frc_unit_t unit)
{
    return unit_ok(unit) ? FRC_PGM_U8(&UNIT_LEVELS[unit]) : (uint8_t)FRC_LEVEL_NONE;
}

int frc_unit_of_level(uint8_t level)
{
    uint8_t i;
    for (i = 0; i < FRC_UNIT_COUNT; i++) {
        if (FRC_PGM_U8(&UNIT_LEVELS[i]) == level) {
            return (int)i;
        }
    }
    return -1;
}

uint64_t frc_i_level_blinks(uint8_t level)
{
    uint64_t prefix;

    if (level > FRC_I_BLINK_LEVEL) {
        return 0;
    }
    /* Cycles of the Blink's level per cycle of `level`. A prefix up to level
     * 12 is never 0; the test is there because the compiler cannot know that,
     * and would plant a trap for "division by zero" (on AVR: a call to abort)
     * on a path that does not exist. */
    prefix = frc_prefix(level);
    return prefix != 0 ? frc_prefix(FRC_I_BLINK_LEVEL) / prefix : 0;
}

uint64_t frc_unit_blinks(frc_unit_t unit)
{
    /* every unit sits on a level at or above the Blink's */
    return unit_ok(unit) ? frc_i_level_blinks(frc_unit_level(unit)) : 0;
}

uint8_t frc_unit_parts(frc_unit_t unit)
{
    return unit_ok(unit) ? FRC_PGM_U8(&UNIT_PARTS[unit]) : 0;
}

void frc_i_text_unit_symbol(frc_i_text_t *w, uint8_t unit)
{
    /* a unit's symbol is the Greek letter of its level: eta for the Helion */
    if (unit < FRC_UNIT_COUNT) {
        frc_i_text_cp(w, frc_i_greek_cp(FRC_PGM_U8(&UNIT_LEVELS[unit])));
    }
}

int frc_format_unit_symbol(frc_unit_t unit, char *buf, size_t size)
{
    frc_i_text_t w;
    if (!unit_ok(unit)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_unit_symbol(&w, (uint8_t)unit);
    return frc_i_text_finish(&w);
}

int frc_format_unit_name(frc_unit_t unit, int plural, char *buf, size_t size)
{
    frc_i_text_t w;
    if (!unit_ok(unit)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_nth(&w, frc_i_unit_names, (uint8_t)unit);
    if (plural) {
        frc_i_text_char(&w, 's');
    }
    return frc_i_text_finish(&w);
}
