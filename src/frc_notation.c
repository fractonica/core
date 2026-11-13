/*
 * frc_notation.c - how a moment is SAID in Fractonica terms: dates and stamps,
 * fracton ordinals, cycle names and Pentad names.
 *
 * Implements SPEC S15.1 - S15.3 (units, names of major periods, counts,
 * ordinals and the date) and the wording of S7.5 (Pentads). The reference
 * for every string is reference/js/src/core/pure.js (dateText, clockText,
 * stamp, pentadText, cycleName, cycleShort, countsAtStart).
 *
 *   frc_date_at, frc_date_to_time      an instant <-> the nine numbers of a date
 *   frc_format_date, frc_parse_date    the four written forms, and reading them back
 *   frc_format_date_term               one term of the stamp: "ε#5", "34η"
 *   frc_format_ordinal                 "Trifracton"
 *   frc_format_cycle_name              "Delta Trifracton", "ε#2 5η 9ι"
 *   frc_format_pentad                  "Pentad II, 3rd Helion", "⬠II·3", "P2.3", "II"
 *
 * THE MODEL
 *   A date is ONE number, the global Blink index B (the level-12 cycle index
 *   of S3.2), written in a mixed radix over the unit ladder of S15.1:
 *
 *       B = (((((((a*5 + b)*13 + g)*5 + d)*5 + e)*77 + h)*36 + i)*65 + l)*25 + n
 *
 *   with one zero-based digit per unit, a (Alpha, global) .. n (Blink). The
 *   radix of a unit is how many of it make the next coarser unit
 *   (frc_unit_parts): 5 Beta, 13 Gamma, 5 Delta, 5 Nuclides, 77 Helions,
 *   36 Preons, 65 Flavons, 25 Blinks. This is the same reading as S15.3:
 *   eta = 11*local(5) + local(6) is the Helion's index among the 7*11 Helions
 *   of its Nuclide, and so on for the levels the ladder skips.
 *
 *   What is WRITTEN is the digit plus a base: Alpha .. Helion are numbered
 *   from 1 (ordinals, and the Helion "like the day of a month"), Preons,
 *   Flavons and Blinks are counts from 0. All code below works on the nine
 *   written numbers as `field[unit]`, indexed by frc_unit_t.
 *
 * INVARIANTS
 *   - Integer only. frc_date_at makes one 128-bit operation: the level-12
 *     cycle fraction (frc_i_fraction_at). Everything else is 64-bit division
 *     by small radices.
 *   - floor(position * P(L)) = floor(T / (P(12) / P(L))) with
 *     T = floor(position * P(12)): nested floors by an integer are exact, so
 *     the quarters of Alpha .. Delta come from the same T as the Blink index.
 *   - frc_date_at(frc_date_to_time(d)) == d: a Blink's start is the first
 *     microsecond AT OR AFTER its exact boundary, so it reads as that Blink.
 *   - frc_parse_date writes `out` only on success and never reads past the
 *     NUL of `text`, whatever bytes it holds.
 */
#include "frc_internal.h"

#define DELTA_LEVEL   3
#define NUCLIDE_LEVEL 4

/* ------------------------------------------------------------------------- */
/* Vocabulary                                                                 */
/* ------------------------------------------------------------------------- */

/* Packed lists in flash (frc_internal.h section 1). The words of the major
 * periods are the first four of frc_i_unit_names (frc_internal.h section 4)
 * and the quarter words are frc_i_quarter_names (section 6): the writer and
 * the parser use the very lists frc_format_unit_name and frc_format_quarter
 * write. */

/* S15.2: ordinal n is stem n-1 followed by "fracton" (11 is the owner's own
 * coinage, "Didecafracton"). Beyond the list an ordinal is written "#n". */
#define ORDINAL_NAMED_MAX 20
static const char ORDINAL_STEMS[] FRC_PROGMEM =
    "Mono\0Di\0Tri\0Tetra\0Penta\0Hexa\0Hepta\0Octa\0Nona\0Deca\0"
    "Dideca\0Dodeca\0Trideca\0Tetradeca\0Pentadeca\0Hexadeca\0Heptadeca\0"
    "Octadeca\0Nonadeca\0Icosa\0";
static const char FRACTON[] FRC_PROGMEM = "fracton";
#define FRACTON_LENGTH 7

/* S7.5: three Pentads of five Helions and the Hypopentad of four
 * (FRC_I_PENTAD_* in frc_internal.h). */
static const char PENTAD_WORD[] FRC_PROGMEM = "Pentad";
static const char HYPOPENTAD_WORD[] FRC_PROGMEM = "Hypopentad";
static const char HELION_WORD[] FRC_PROGMEM = "Helion";
static const char PENTAD_NUMERALS[] FRC_PROGMEM = "I\0II\0III\0";
/* frc_to_ascii writes the short form "⬠II" as "PII". */
static const char PENTAD_ASCII_NUMERALS[] FRC_PROGMEM = "PI\0PII\0PIII\0";
/* 1st 2nd 3rd 4th 5th, by helion - 1; the parser accepts any of them. */
static const char ORDINAL_SUFFIXES[] FRC_PROGMEM = "st\0nd\0rd\0th\0th\0";

/* ------------------------------------------------------------------------- */
/* The nine written numbers of a date                                         */
/* ------------------------------------------------------------------------- */

/* What the digit 0 of a unit is written as: Alpha .. Helion are numbered
 * from 1, Preon .. Blink are counts from 0 (S15.3). */
static uint8_t field_base(uint8_t unit)
{
    return unit < FRC_UNIT_PREON ? 1 : 0;
}

/* How many values a field takes: its radix. Alpha has none (it is global;
 * the catalogue decides how many there are), so it says 0. */
static uint8_t field_radix(uint8_t unit)
{
    return unit == FRC_UNIT_ALPHA ? 0 : frc_unit_parts((frc_unit_t)(unit - 1));
}

/* 1 when `value` can be written for `unit`: beta 1..5, gamma 1..13, delta
 * 1..5, epsilon 1..5, eta 1..77, iota 0..35, lambda 0..64, nu 0..24; an
 * Alpha is any ordinal that fits frc_date_t. */
static int field_ok(uint8_t unit, uint32_t value)
{
    uint32_t base = field_base(unit);
    if (unit == FRC_UNIT_ALPHA) {
        return value >= 1 && value <= UINT16_MAX;
    }
    return value >= base && value - base < field_radix(unit);
}

/*
 * Blink index -> the written numbers of Beta .. Blink (field[FRC_UNIT_ALPHA]
 * is NOT written). Returns what is left after the last division: the
 * zero-based global Alpha index, which may be any size.
 */
static uint64_t split_blinks(uint64_t blink, uint16_t field[FRC_UNIT_COUNT])
{
    uint8_t unit;
    for (unit = FRC_UNIT_BLINK; unit > FRC_UNIT_ALPHA; unit--) {
        uint8_t radix = field_radix(unit);
        if (radix == 0) {
            radix = 1; /* never: every unit below Alpha has a radix */
        }
        field[unit] = (uint16_t)(blink % radix + field_base(unit));
        blink /= radix;
    }
    return blink;
}

/* The inverse: the Blink index nine valid fields name (Horner's rule).
 * With alpha <= 65535 the result is below 65535 * 7,319,812,500 < 2^49. */
static uint64_t join_blinks(const uint16_t field[FRC_UNIT_COUNT])
{
    uint64_t blink = (uint64_t)field[FRC_UNIT_ALPHA] - 1u;
    uint8_t unit;
    for (unit = FRC_UNIT_BETA; unit < FRC_UNIT_COUNT; unit++) {
        blink = blink * field_radix(unit) + (uint64_t)(field[unit] - field_base(unit));
    }
    return blink;
}

/* frc_date_t <-> field[]. Field by field: the struct has no array to index. */
static void fields_of_date(const frc_date_t *date, uint16_t field[FRC_UNIT_COUNT])
{
    field[FRC_UNIT_ALPHA] = date->alpha;
    field[FRC_UNIT_BETA] = date->beta;
    field[FRC_UNIT_GAMMA] = date->gamma;
    field[FRC_UNIT_DELTA] = date->delta;
    field[FRC_UNIT_NUCLIDE] = date->epsilon;
    field[FRC_UNIT_HELION] = date->eta;
    field[FRC_UNIT_PREON] = date->iota;
    field[FRC_UNIT_FLAVON] = date->lambda;
    field[FRC_UNIT_BLINK] = date->nu;
}

/* Every field but alpha is below 256 here (field_ok or split_blinks). */
static void date_of_fields(const uint16_t field[FRC_UNIT_COUNT], frc_date_t *date)
{
    date->alpha = field[FRC_UNIT_ALPHA];
    date->beta = (uint8_t)field[FRC_UNIT_BETA];
    date->gamma = (uint8_t)field[FRC_UNIT_GAMMA];
    date->delta = (uint8_t)field[FRC_UNIT_DELTA];
    date->epsilon = (uint8_t)field[FRC_UNIT_NUCLIDE];
    date->eta = (uint8_t)field[FRC_UNIT_HELION];
    date->iota = (uint8_t)field[FRC_UNIT_PREON];
    date->lambda = (uint8_t)field[FRC_UNIT_FLAVON];
    date->nu = (uint8_t)field[FRC_UNIT_BLINK];
}

/* ------------------------------------------------------------------------- */
/* S15.3  Instant <-> date                                                    */
/* ------------------------------------------------------------------------- */

frc_status_t frc_date_at(const frc_catalogue_t *cat, frc_time_t t, frc_date_t *out)
{
    frc_i_fraction_t f;
    uint16_t field[FRC_UNIT_COUNT];
    frc_status_t status;
    uint64_t quarter_blinks;
    uint8_t unit;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = frc_i_fraction_at(cat, t, FRC_I_BLINK_LEVEL, &f);
    if (status != FRC_OK) {
        return status;
    }
    /* T = floor(position * P(12)) counts quarter Blinks. It fits 64 bits with
     * room to spare: (count - 1) * P(12) < 2^16 * 2^33. The Alpha index is at
     * most (count - 1) / 4, so the ordinal fits uint16_t. */
    quarter_blinks = f.total.lo;
    field[FRC_UNIT_ALPHA] = (uint16_t)(split_blinks(f.cycle, field) + 1u);
    date_of_fields(field, out);

    /* quarter(L) = floor(position * P(L)) mod 4 (S15.2). One quarter of a
     * major cycle is frc_unit_blinks quarter Blinks long, because
     * P(12) / P(L) is that unit's length in Blinks. */
    for (unit = FRC_UNIT_ALPHA; unit <= FRC_UNIT_DELTA; unit++) {
        uint64_t length = frc_unit_blinks((frc_unit_t)unit);
        /* (the cast covers the whole conditional: gcc with UBSan does not
         * fold a narrower one and then warns about the conversion) */
        out->quarter[unit] = (uint8_t)((length != 0) ? ((quarter_blinks / length) & 3u) : 0u);
    }
    return FRC_OK;
}

frc_status_t frc_date_to_time(const frc_catalogue_t *cat, const frc_date_t *date, frc_time_t *out)
{
    uint16_t field[FRC_UNIT_COUNT];
    frc_cycle_t blink;
    frc_status_t status;
    uint8_t unit;

    if (!frc_i_cat_ok(cat) || date == NULL || out == NULL) {
        return FRC_ERR_ARG;
    }
    fields_of_date(date, field);
    for (unit = 0; unit < FRC_UNIT_COUNT; unit++) {
        if (!field_ok(unit, field[unit])) {
            return FRC_ERR_ARG;
        }
    }
    /* A well-formed date beyond the catalogue (an Alpha it does not have, or
     * a Blink after its closing instant) is FRC_ERR_RANGE: the Blink index is
     * not a cycle of the catalogue. */
    status = frc_cycle_by_index(cat, FRC_I_BLINK_LEVEL, join_blinks(field), &blink);
    if (status != FRC_OK) {
        return status;
    }
    *out = blink.start;
    return FRC_OK;
}

/* ------------------------------------------------------------------------- */
/* Writing the parts                                                          */
/* ------------------------------------------------------------------------- */

/* "ε#5": an ordinal, counted from 1 (S15.3). */
static void put_numbered(frc_i_text_t *w, uint8_t unit, uint64_t n)
{
    frc_i_text_unit_symbol(w, unit);
    frc_i_text_char(w, '#');
    frc_i_text_u64(w, n);
}

/* "34η": the form of the minor clock and of spans. */
static void put_counted(frc_i_text_t *w, uint8_t unit, uint64_t n)
{
    frc_i_text_u64(w, n);
    frc_i_text_unit_symbol(w, unit);
}

/* "Trifracton" for 3; outside 1..20 a plain "#25". */
static void put_ordinal(frc_i_text_t *w, uint64_t n)
{
    if (n >= 1 && n <= ORDINAL_NAMED_MAX) {
        frc_i_text_nth(w, ORDINAL_STEMS, (uint8_t)(n - 1));
        frc_i_text_pgm(w, FRACTON);
    } else {
        frc_i_text_char(w, '#');
        frc_i_text_u64(w, n);
    }
}

/* "Delta Trifracton": a major period in words. `unit` is 0..3. */
static void put_major(frc_i_text_t *w, uint8_t unit, uint64_t ordinal)
{
    frc_i_text_nth(w, frc_i_unit_names, unit);
    frc_i_text_char(w, ' ');
    put_ordinal(w, ordinal);
}

int frc_format_ordinal(uint16_t n, char *buf, size_t size)
{
    frc_i_text_t w;
    frc_i_text_init(&w, buf, size);
    put_ordinal(&w, n);
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* S15.3  frc_format_date                                                     */
/* ------------------------------------------------------------------------- */

/*
 * pure.dateText(t, {form, letters, upTo}):
 *   the majors   FULL and LETTERS write Alpha .. Delta, the short forms Delta
 *                alone; in words with the quarter ("Delta Trifracton End")
 *                and joined by ", ", in letters ("δ#3") joined by a space
 *   the minors   "ε#5", then the Helion's number and the counts down to
 *                `finest`, all joined by a space; FULL puts ", " before them
 */
int frc_format_date(const frc_date_t *date, frc_date_form_t form, frc_unit_t finest, char *buf, size_t size)
{
    frc_i_text_t w;
    uint16_t field[FRC_UNIT_COUNT];
    uint8_t unit, first, last;
    int words, all_majors;

    if (date == NULL || !FRC_I_BELOW(form, FRC_DATE_SHORT_LETTERS + 1) || !FRC_I_BELOW(finest, FRC_UNIT_COUNT)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    for (unit = FRC_UNIT_ALPHA; unit <= FRC_UNIT_DELTA; unit++) {
        if (date->quarter[unit] > FRC_END) {
            return frc_i_text_error(buf, size, FRC_ERR_ARG);
        }
    }
    words = (form == FRC_DATE_FULL || form == FRC_DATE_SHORT);
    all_majors = (form == FRC_DATE_FULL || form == FRC_DATE_LETTERS);
    first = all_majors ? (uint8_t)FRC_UNIT_ALPHA : (uint8_t)FRC_UNIT_DELTA;
    /* a date always reaches the Delta */
    last = ((uint8_t)finest < FRC_UNIT_DELTA) ? (uint8_t)FRC_UNIT_DELTA : (uint8_t)finest;
    fields_of_date(date, field);

    frc_i_text_init(&w, buf, size);
    for (unit = first; unit <= last; unit++) {
        if (unit != first) {
            /* a comma joins the majors said in words, and in the full form
             * it also stands before the minor reading */
            int comma = (unit <= FRC_UNIT_DELTA) ? words : (unit == FRC_UNIT_NUCLIDE && form == FRC_DATE_FULL);
            if (comma) {
                frc_i_text_char(&w, ',');
            }
            frc_i_text_char(&w, ' ');
        }
        if (unit <= FRC_UNIT_DELTA && words) {
            put_major(&w, unit, field[unit]);
            frc_i_text_char(&w, ' ');
            frc_i_text_nth(&w, frc_i_quarter_names, date->quarter[unit]);
        } else if (unit <= FRC_UNIT_NUCLIDE) {
            put_numbered(&w, unit, field[unit]);
        } else {
            put_counted(&w, unit, field[unit]);
        }
    }
    return frc_i_text_finish(&w);
}

/* One term of the stamp: the same two writers frc_format_date uses for its
 * letter forms, so a term is byte for byte the stamp's. */
int frc_format_date_term(const frc_date_t *date, frc_unit_t unit, char *buf, size_t size)
{
    frc_i_text_t w;
    uint16_t field[FRC_UNIT_COUNT];

    if (date == NULL || !FRC_I_BELOW(unit, FRC_UNIT_COUNT)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    fields_of_date(date, field);
    frc_i_text_init(&w, buf, size);
    if ((uint8_t)unit <= FRC_UNIT_NUCLIDE) {
        put_numbered(&w, (uint8_t)unit, field[unit]);
    } else {
        put_counted(&w, (uint8_t)unit, field[unit]);
    }
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* S15.3  frc_format_cycle_name                                               */
/* ------------------------------------------------------------------------- */

/*
 * Where cycle `cycle` of `level` (4 .. FRC_CLOCK_MAX_LEVEL) starts on the
 * minor ladder (pure.countsAtStart): fills the written numbers of Nuclide ..
 * Blink (the coarser fields are filled too, but mean nothing: a name places
 * a cycle inside its Delta only). Returns the part of a Blink already run at
 * that start, in hundredths rounded half up: 0 for level <= 12, 0..100 below.
 */
static uint8_t cycle_start(uint8_t level, uint64_t cycle, uint16_t field[FRC_UNIT_COUNT])
{
    uint64_t p = frc_prefix(level);
    uint64_t p_blink = frc_prefix(FRC_I_BLINK_LEVEL);
    uint64_t p_delta = frc_prefix(DELTA_LEVEL);
    uint64_t blink = 0;
    uint64_t hundredths = 0;

    if (p == 0 || p_delta == 0 || p_blink == 0) {
        /* never: the caller checked level <= 21, whose prefix fits 64 bits */
    } else if (level <= FRC_I_BLINK_LEVEL) {
        /* A cycle of the level is p_blink / p whole Blinks. The place inside
         * the Delta repeats with the Delta, so the index is first reduced
         * modulo the cycles of the level in one Delta: the product is then
         * below the 22,522,500 Blinks of a Delta for any `cycle`. */
        blink = (cycle % (p / p_delta)) * (p_blink / p);
    } else {
        /* p / p_blink cycles in one Blink (at most 31,531,500 at level 21):
         * the cycle starts `rest` of them into Blink number cycle / ratio.
         * No start is exactly half a hundredth: 200 * rest / ratio is never
         * an odd integer for these ratios, so half-up is plain rounding. */
        uint64_t ratio = p / p_blink;
        uint64_t rest = cycle % ratio;
        blink = cycle / ratio;
        hundredths = (rest * 200u + ratio) / (ratio * 2u);
    }
    (void)split_blinks(blink, field);
    return (uint8_t)hundredths;
}

/* "11ν", or "11.43ν" for a start inside a Blink (pure.nuWithFrac: two
 * decimals, trailing zeros dropped as JavaScript prints a number). A start
 * within half a hundredth of the next Blink reads as that Blink, as in the
 * reference: 24.997 is written "25ν". */
static void put_blinks_with_hundredths(frc_i_text_t *w, uint16_t blinks, uint8_t hundredths)
{
    frc_i_text_decimal(w, (uint64_t)blinks * 100u + hundredths, 2);
    frc_i_text_unit_symbol(w, FRC_UNIT_BLINK);
}

/*
 * The counts that place the start of a minor cycle (pure.minorStartParts):
 * down to the first unit at or finer than its level, so a zeta (level 5) is
 * placed by the Helion it starts at, a theta (7) by Helion and Preon ...
 * With `finest_only` just the last of them (pure.cycleShort).
 */
static void put_minor_start(frc_i_text_t *w, uint8_t level, const uint16_t field[FRC_UNIT_COUNT],
                            uint8_t hundredths, int finest_only)
{
    /* the finest unit shown: levels 5,6 Helion; 7,8 Preon; 9,10 Flavon; 11+ Blink */
    uint8_t last = (level >= 11) ? (uint8_t)FRC_UNIT_BLINK : (uint8_t)(FRC_UNIT_HELION + (level - 5u) / 2u);
    uint8_t unit;

    for (unit = finest_only ? last : (uint8_t)FRC_UNIT_HELION; unit <= last; unit++) {
        if (!finest_only) {
            frc_i_text_char(w, ' '); /* after "ε#2" */
        }
        if (unit == FRC_UNIT_BLINK) {
            put_blinks_with_hundredths(w, field[unit], hundredths);
        } else {
            put_counted(w, unit, field[unit]);
        }
    }
}

int frc_format_cycle_name(uint8_t level, uint64_t cycle, int short_form, char *buf, size_t size)
{
    frc_i_text_t w;
    uint16_t field[FRC_UNIT_COUNT];
    uint8_t hundredths;

    if (level > FRC_CLOCK_MAX_LEVEL) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    if (level == 0 && cycle == UINT64_MAX) {
        return frc_i_text_error(buf, size, FRC_ERR_OVERFLOW); /* its ordinal is 2^64 */
    }
    frc_i_text_init(&w, buf, size);

    if (level <= DELTA_LEVEL) {
        /* S15.2: the place inside the parent, from 1; an Alpha has no parent
         * and is numbered globally. Units 0..3 sit on levels 0..3. */
        uint8_t siblings = frc_siblings(level);
        uint64_t ordinal = (siblings == 0 ? cycle : cycle % siblings) + 1u;
        if (short_form) {
            put_numbered(&w, level, ordinal);
        } else {
            put_major(&w, level, ordinal);
        }
        return frc_i_text_finish(&w);
    }

    hundredths = cycle_start(level, cycle, field);
    if (level == NUCLIDE_LEVEL || !short_form) {
        put_numbered(&w, FRC_UNIT_NUCLIDE, field[FRC_UNIT_NUCLIDE]);
    }
    if (level > NUCLIDE_LEVEL) {
        put_minor_start(&w, level, field, hundredths, short_form);
    }
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* S7.5  frc_format_pentad                                                    */
/* ------------------------------------------------------------------------- */

int frc_format_pentad(const frc_pentad_t *pentad, frc_pentad_form_t form, int with_helion, char *buf, size_t size)
{
    frc_i_text_t w;
    int hypo;

    if (pentad == NULL || pentad->index >= FRC_I_PENTAD_COUNT || !FRC_I_BELOW(form, FRC_PENTAD_NUMERAL + 1)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    /* frc_pentad_by_number leaves helion 0: there is no place to write */
    if (with_helion && (pentad->helion < 1 || pentad->helion > FRC_I_PENTAD_HELIONS)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    hypo = (pentad->index == FRC_I_HYPOPENTAD);
    frc_i_text_init(&w, buf, size);

    if (form == FRC_PENTAD_FULL) {
        /* "Pentad II", "Hypopentad"; "Pentad II, 3rd Helion" */
        if (hypo) {
            frc_i_text_pgm(&w, HYPOPENTAD_WORD);
        } else {
            frc_i_text_pgm(&w, PENTAD_WORD);
            frc_i_text_char(&w, ' ');
            frc_i_text_nth(&w, PENTAD_NUMERALS, pentad->index);
        }
        if (with_helion) {
            frc_i_text_char(&w, ',');
            frc_i_text_char(&w, ' ');
            frc_i_text_u64(&w, pentad->helion);
            frc_i_text_nth(&w, ORDINAL_SUFFIXES, (uint8_t)(pentad->helion - 1));
            frc_i_text_char(&w, ' ');
            frc_i_text_pgm(&w, HELION_WORD);
        }
    } else if (form == FRC_PENTAD_SHORT) {
        /* the Helion count as a shape: a pentagon for five, a square for the
         * Hypopentad's four (its shape alone says it); "⬠II·3" */
        if (hypo) {
            frc_i_text_cp(&w, FRC_I_CP_SQUARE);
        } else {
            frc_i_text_cp(&w, FRC_I_CP_PENTAGON);
            frc_i_text_nth(&w, PENTAD_NUMERALS, pentad->index);
        }
        if (with_helion) {
            frc_i_text_cp(&w, FRC_I_CP_MIDDOT);
            frc_i_text_u64(&w, pentad->helion);
        }
    } else if (form == FRC_PENTAD_NUMERAL) {
        /* "II" alone, for a mark the client draws; the Hypopentad's mark is
         * its square, with no numeral: the empty text. No Helion. */
        if (!hypo) {
            frc_i_text_nth(&w, PENTAD_NUMERALS, pentad->index);
        }
    } else {
        /* "P2", "H"; "P2.3" */
        if (hypo) {
            frc_i_text_char(&w, 'H');
        } else {
            frc_i_text_char(&w, 'P');
            frc_i_text_u64(&w, (uint64_t)pentad->index + 1u);
        }
        if (with_helion) {
            frc_i_text_char(&w, '.');
            frc_i_text_u64(&w, pentad->helion);
        }
    }
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* frc_parse_date: the scanner                                                */
/* ------------------------------------------------------------------------- */

/*
 * THE GRAMMAR (case does not matter; spaces may stand anywhere between
 * tokens and are never required next to a digit, '#' or a Greek letter)
 *
 *   date     = [ pentad [ "·" | "." | "," ] ]  part { [ "," ] part }
 *   part     = major | nuclide | count
 *   major    = ( "Alpha" | "Beta" | "Gamma" | "Delta" ) ( ordinal-name | "#" n ) [ quarter ]
 *            | ( α | β | γ | δ | a | b | g | d ) "#" n [ quarter ]
 *   nuclide  = ( ε | e ) "#" n
 *   count    = n ( η | ι | λ | ν | h | i | l | n )
 *   quarter  = "Peak" | "Node" | "Valley" | "End"          read and dropped
 *   pentad   = "Pentad" ( I | II | III | 1 | 2 | 3 ) [ [ "," ] k ( st | nd | rd | th ) "Helion" ]
 *            | "Hypopentad" [ ... the same ... ]
 *            | "⬠" ( I | II | III | 1 | 2 | 3 ) [ "·" k ]   | "◻" [ "·" k ]
 *            | "P" ( 1 | 2 | 3 ) [ "." k ]  | "PI" | "PII" | "PIII" [ "." k ]  | "H" [ "." k ]
 *
 * The parts must be CONSECUTIVE units of the ladder α β γ δ ε η ι λ ν, in
 * that order: no duplicate, no gap, nothing out of place. That covers every
 * form frc_format_date writes at every `finest`, the same text after
 * frc_to_ascii, and any tail of a date ("34η 29ι": the coarser parts come
 * from the caller's context).
 */

/* Token kinds. */
#define TOK_END      0 /* the NUL                                             */
#define TOK_NUMBER   1 /* a run of ASCII digits                               */
#define TOK_WORD     2 /* a run of ASCII letters                              */
#define TOK_GREEK    3 /* one Greek letter                                    */
#define TOK_HASH     4 /* #                                                   */
#define TOK_COMMA    5 /* ,                                                   */
#define TOK_DOT      6 /* . or the middle dot                                 */
#define TOK_PENTAGON 7 /* ⬠                                                   */
#define TOK_SQUARE   8 /* ◻                                                   */
#define TOK_BAD      9 /* anything else, malformed UTF-8 included             */

/* Numbers are capped here: one more than any field can hold, so a run of a
 * hundred digits cannot overflow and still reads as "out of range". */
#define NUMBER_CAP 65536u
#define WORD_LENGTH_CAP 255u

#define CP_NBSP        0x00A0u
#define CP_THIN_SPACE  0x2009u
#define CP_GREEK_UPPER_FIRST 0x0391u /* Α */
#define CP_GREEK_UPPER_LAST  0x03A9u /* Ω */
#define CP_GREEK_CASE_STEP   0x20u   /* α - Α */

typedef struct token {
    uint8_t     kind;   /* TOK_*                                              */
    uint8_t     length; /* TOK_WORD: letters, capped at WORD_LENGTH_CAP       */
    uint8_t     letter; /* TOK_GREEK: index in the alphabet, 0..23            */
    uint32_t    number; /* TOK_NUMBER: the value, capped at NUMBER_CAP        */
    const char *word;   /* TOK_WORD: its first letter (in the caller's text)  */
} token_t;

static int is_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
           cp == CP_NBSP || cp == CP_THIN_SPACE || cp == FRC_I_CP_NNBSP;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* ASCII letters differ in bit 5 only. */
static char lower(char c)
{
    return (char)(c | 0x20);
}

/* Read the token at `p` into `tok`; returns where the next one starts (a
 * TOK_END stays on the NUL, so scanning past the end is harmless). */
static const char *scan(const char *p, token_t *tok)
{
    uint32_t cp;
    uint8_t used;
    int greek;

    tok->length = 0;
    tok->letter = 0;
    tok->number = 0;
    for (;;) {
        used = frc_i_utf8_decode(p, &cp); /* malformed bytes read as U+FFFD: TOK_BAD */
        if (!is_space(cp)) {
            break;
        }
        p += used;
    }
    tok->word = p;

    if (cp == 0) {
        tok->kind = TOK_END;
        return p;
    }
    if (cp < 0x80u && is_digit((char)cp)) {
        tok->kind = TOK_NUMBER;
        for (; is_digit(*p); p++) {
            tok->number = tok->number * 10u + (uint32_t)(*p - '0');
            if (tok->number > NUMBER_CAP) {
                tok->number = NUMBER_CAP;
            }
        }
        return p;
    }
    if (cp < 0x80u && is_letter((char)cp)) {
        tok->kind = TOK_WORD;
        for (; is_letter(*p); p++) {
            if (tok->length < WORD_LENGTH_CAP) {
                tok->length++;
            }
        }
        return p;
    }

    p += used;
    if (cp >= CP_GREEK_UPPER_FIRST && cp <= CP_GREEK_UPPER_LAST) {
        cp += CP_GREEK_CASE_STEP; /* a capital reads as its small letter */
    }
    greek = frc_i_greek_index_of_cp(cp);
    if (greek >= 0) {
        tok->kind = TOK_GREEK;
        tok->letter = (uint8_t)greek;
    } else if (cp == '#') {
        tok->kind = TOK_HASH;
    } else if (cp == ',') {
        tok->kind = TOK_COMMA;
    } else if (cp == '.' || cp == FRC_I_CP_MIDDOT) {
        tok->kind = TOK_DOT;
    } else if (cp == FRC_I_CP_PENTAGON) {
        tok->kind = TOK_PENTAGON;
    } else if (cp == FRC_I_CP_SQUARE) {
        tok->kind = TOK_SQUARE;
    } else {
        tok->kind = TOK_BAD;
    }
    return p;
}

/* 1 when the `length` letters at `word` spell the flash string `name`,
 * whatever their case. */
static int letters_spell(const char *word, uint8_t length, const char *name)
{
    uint8_t i;
    for (i = 0; i < length; i++) {
        char want = FRC_PGM_CHAR(name + i);
        if (want == '\0' || lower(word[i]) != lower(want)) {
            return 0;
        }
    }
    return FRC_PGM_CHAR(name + length) == '\0';
}

/* 1 when the token is a word that spells `name`. */
static int word_is(const token_t *tok, const char *name)
{
    return tok->kind == TOK_WORD && letters_spell(tok->word, tok->length, name);
}

/* Index of the letters in a packed flash list, or -1. (frc_i_packed_nth
 * finds each item: it is written so that no compiler turns it into strlen.) */
static int index_in_list(const char *word, uint8_t length, const char *packed)
{
    uint8_t i;
    for (i = 0; i < UINT8_MAX; i++) {
        const char *item = frc_i_packed_nth(packed, i);
        if (FRC_PGM_CHAR(item) == '\0') {
            break; /* the empty item that closes the list */
        }
        if (letters_spell(word, length, item)) {
            return (int)i;
        }
    }
    return -1;
}

/* Index of a word token in a packed list, or -1. */
static int word_in_list(const token_t *tok, const char *packed)
{
    return tok->kind == TOK_WORD ? index_in_list(tok->word, tok->length, packed) : -1;
}

/* 1..20 for a fracton ordinal name ("Trifracton"), 0 for any other token. */
static uint8_t fracton_ordinal(const token_t *tok)
{
    uint8_t stem_length;
    int index;

    if (tok->kind != TOK_WORD || tok->length <= FRACTON_LENGTH) {
        return 0;
    }
    stem_length = (uint8_t)(tok->length - FRACTON_LENGTH);
    if (!letters_spell(tok->word + stem_length, FRACTON_LENGTH, FRACTON)) {
        return 0;
    }
    index = index_in_list(tok->word, stem_length, ORDINAL_STEMS);
    return index < 0 ? 0 : (uint8_t)(index + 1);
}

/* The unit a letter token names (frc_unit_t), or -1: a Greek letter, or its
 * one-letter ASCII stand-in of frc_to_ascii (a b g d e h i l n). */
static int unit_of_letter(const token_t *tok)
{
    int index = -1;
    if (tok->kind == TOK_GREEK) {
        index = tok->letter;
    } else if (tok->kind == TOK_WORD && tok->length == 1) {
        index = frc_i_greek_index_of_ascii(lower(tok->word[0]));
    }
    /* the letter's place in the alphabet is its level: eta is level 6 */
    return index < 0 ? -1 : frc_unit_of_level((uint8_t)index);
}

/* 1 when the token is a number in lo..hi. */
static int number_in(const token_t *tok, uint32_t lo, uint32_t hi)
{
    return tok->kind == TOK_NUMBER && tok->number >= lo && tok->number <= hi;
}

/* ------------------------------------------------------------------------- */
/* frc_parse_date: the Pentad in front                                        */
/* ------------------------------------------------------------------------- */

/* Steps over "I" / "II" / "III" or 1..3 at `p`; NULL when there is none. */
static const char *skip_pentad_numeral(const char *p)
{
    token_t tok;
    p = scan(p, &tok);
    return (word_in_list(&tok, PENTAD_NUMERALS) >= 0 || number_in(&tok, 1, FRC_I_PENTAD_COUNT - 1)) ? p : NULL;
}

/* Steps over ", 3rd Helion" (the comma is optional); `p` itself when the
 * text does not continue that way. */
static const char *skip_helion_words(const char *p)
{
    token_t tok;
    const char *q = scan(p, &tok);
    if (tok.kind == TOK_COMMA) {
        q = scan(q, &tok);
    }
    if (!number_in(&tok, 1, FRC_I_PENTAD_HELIONS)) {
        return p;
    }
    q = scan(q, &tok);
    if (word_in_list(&tok, ORDINAL_SUFFIXES) < 0) {
        return p;
    }
    q = scan(q, &tok);
    return word_is(&tok, HELION_WORD) ? q : p;
}

/* Steps over "·3" / ".3"; `p` itself when there is none. A number that is
 * followed by the letter of a counted unit is not the Helion's place but the
 * date's first count: in "⬠II·3η 29ι" the dot is the separator. */
static const char *skip_helion_digit(const char *p)
{
    token_t tok, after;
    const char *q = scan(p, &tok);
    if (tok.kind != TOK_DOT) {
        return p;
    }
    q = scan(q, &tok);
    if (!number_in(&tok, 1, FRC_I_PENTAD_HELIONS)) {
        return p;
    }
    (void)scan(q, &after);
    return unit_of_letter(&after) >= FRC_UNIT_HELION ? p : q;
}

/*
 * A date may lead with its Pentad, as a civil date leads with the weekday
 * (S15.3). The Pentad is implied by the rest, so it is read past: returns
 * where the date itself starts, or `text` when no Pentad stands in front.
 */
static const char *skip_pentad(const char *text)
{
    token_t tok;
    const char *p = scan(text, &tok);
    const char *q;
    int numbered; /* a numeral must follow */
    int in_words; /* the Helion's place is written ", 3rd Helion", not ".3" */

    if (word_is(&tok, PENTAD_WORD)) {
        numbered = 1;
        in_words = 1;
    } else if (word_is(&tok, HYPOPENTAD_WORD)) {
        numbered = 0;
        in_words = 1;
    } else if (tok.kind == TOK_PENTAGON) {
        numbered = 1;
        in_words = 0;
    } else if (tok.kind == TOK_SQUARE || word_in_list(&tok, PENTAD_ASCII_NUMERALS) >= 0) {
        numbered = 0;
        in_words = 0;
    } else if (tok.kind == TOK_WORD && tok.length == 1 && lower(tok.word[0]) == 'p') {
        numbered = 1; /* "P2" */
        in_words = 0;
    } else if (tok.kind == TOK_WORD && tok.length == 1 && lower(tok.word[0]) == 'h') {
        /* "H": the Hypopentad. No date starts with a bare unit letter (a
         * count is written number first), so this cannot be an eta. */
        numbered = 0;
        in_words = 0;
    } else {
        return text;
    }

    if (numbered) {
        p = skip_pentad_numeral(p);
        if (p == NULL) {
            return text;
        }
    }
    p = in_words ? skip_helion_words(p) : skip_helion_digit(p);

    /* one separator between the Pentad and the date: "Pentad II · Delta ..." */
    q = scan(p, &tok);
    return (tok.kind == TOK_DOT || tok.kind == TOK_COMMA) ? q : p;
}

/* ------------------------------------------------------------------------- */
/* frc_parse_date: the parts                                                  */
/* ------------------------------------------------------------------------- */

/* Reads "#n" at `p`; NULL when it is not there. */
static const char *read_hash_number(const char *p, uint32_t *number)
{
    token_t tok;
    p = scan(p, &tok);
    if (tok.kind != TOK_HASH) {
        return NULL;
    }
    p = scan(p, &tok);
    if (tok.kind != TOK_NUMBER) {
        return NULL;
    }
    *number = tok.number;
    return p;
}

/*
 * Reads one part of a date whose first token `tok` was read already and
 * ended at `p`. Writes the unit and the number as written; returns where the
 * part ends, or NULL when the tokens are not a part.
 */
static const char *read_part(const char *p, const token_t *tok, uint8_t *unit, uint32_t *number)
{
    token_t next;
    const char *q;
    int found;

    if (tok->kind == TOK_NUMBER) {
        /* "34η": a count; only Helion, Preon, Flavon and Blink are counted */
        *number = tok->number;
        p = scan(p, &next);
        found = unit_of_letter(&next);
        if (found < FRC_UNIT_HELION) {
            return NULL;
        }
        *unit = (uint8_t)found;
        return p;
    }

    /* Only the four major periods are said as words: "Nuclide #3" or
     * "34 Helions" is not a date, although the list holds all nine names. */
    found = word_in_list(tok, frc_i_unit_names);
    if (found >= 0 && found <= FRC_UNIT_DELTA) {
        /* "Delta Trifracton", "Delta #3" */
        uint8_t ordinal;
        q = scan(p, &next);
        ordinal = fracton_ordinal(&next);
        if (ordinal != 0) {
            *number = ordinal;
            p = q;
        } else {
            p = read_hash_number(p, number);
        }
    } else {
        /* "δ#3", "e#5": an ordinal; Alpha .. Nuclide are numbered */
        found = unit_of_letter(tok);
        if (found < 0 || found > FRC_UNIT_NUCLIDE) {
            return NULL;
        }
        p = read_hash_number(p, number);
    }
    if (p == NULL) {
        return NULL;
    }
    *unit = (uint8_t)found;

    /* A major period may say its quarter. The counts imply it, so it is not
     * checked against them: a date names a Blink, whatever quarter word the
     * writer attached. */
    if (found <= FRC_UNIT_DELTA) {
        q = scan(p, &next);
        if (word_in_list(&next, frc_i_quarter_names) >= 0) {
            p = q;
        }
    }
    return p;
}

frc_status_t frc_parse_date(const char *text, const frc_date_t *context, frc_date_t *out, frc_unit_t *finest)
{
    uint16_t field[FRC_UNIT_COUNT];
    uint16_t given[FRC_UNIT_COUNT];
    token_t tok;
    const char *p;
    uint8_t unit;
    uint8_t first = FRC_UNIT_COUNT; /* the coarsest unit written; COUNT = none yet */
    uint8_t last = 0;               /* the finest unit written                     */
    int out_of_range = 0;

    if (text == NULL || out == NULL) {
        return FRC_ERR_ARG;
    }
    p = skip_pentad(text);
    for (;;) {
        uint32_t number = 0;
        p = scan(p, &tok);
        if (tok.kind == TOK_END) {
            break;
        }
        if (tok.kind == TOK_COMMA && first != FRC_UNIT_COUNT) {
            p = scan(p, &tok); /* one comma between two parts; a part must follow */
        }
        p = read_part(p, &tok, &unit, &number);
        if (p == NULL) {
            return FRC_ERR_PARSE;
        }
        if (first == FRC_UNIT_COUNT) {
            first = unit;
        } else if (unit != last + 1) {
            return FRC_ERR_PARSE; /* a duplicate, a gap or a part out of order */
        }
        last = unit;
        /* A number out of range does not stop the reading: text that is not
         * a date at all is FRC_ERR_PARSE whatever its numbers are. */
        if (!field_ok(unit, number)) {
            out_of_range = 1;
        }
        field[unit] = (uint16_t)number;
    }
    if (first == FRC_UNIT_COUNT) {
        return FRC_ERR_PARSE; /* nothing, or a Pentad alone */
    }
    if (first != FRC_UNIT_ALPHA && context == NULL) {
        return FRC_ERR_PARSE; /* a short form needs a context to hang from */
    }
    if (out_of_range) {
        return FRC_ERR_ARG;
    }

    /* the coarse end from the context, the fine end is "the start" */
    if (first != FRC_UNIT_ALPHA) {
        fields_of_date(context, given);
        for (unit = 0; unit < first; unit++) {
            if (!field_ok(unit, given[unit])) {
                return FRC_ERR_ARG;
            }
            field[unit] = given[unit];
        }
    }
    for (unit = (uint8_t)(last + 1u); unit < FRC_UNIT_COUNT; unit++) {
        field[unit] = field_base(unit);
    }

    date_of_fields(field, out);
    /* Four stores, not a loop: xtensa gcc -Os and ARMv6-M gcc turn the
     * four-byte zero-fill loop into a call to memset. */
    out->quarter[FRC_UNIT_ALPHA] = 0;
    out->quarter[FRC_UNIT_BETA] = 0;
    out->quarter[FRC_UNIT_GAMMA] = 0;
    out->quarter[FRC_UNIT_DELTA] = 0;
    if (finest != NULL) {
        *finest = (frc_unit_t)last;
    }
    return FRC_OK;
}
