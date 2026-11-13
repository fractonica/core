/*
 * fractonica.h - the Fractonica clock core. One header, pure C99, no dependencies.
 *
 * Implements SPEC.md 2.2.0 (the eclipse-anchored harmonic clock): catalogue and
 * position (S2), harmonic hierarchy (S3), phase glyphs (S4), odometer (S5),
 * counters and thresholds (S6), solar sync and Pentads (S7), glyph geometry (S9),
 * notation (S15), the spectral ladder and spatial units (S16), and the "pure"
 * wordings of counts, rates and lengths that name no Earth unit.
 *
 * DESIGN RULES (they hold for every function below)
 *   - No heap, no globals, no recursion, no libc and no libm. Every function is
 *     pure and re-entrant; all outputs go to caller-provided storage.
 *   - All tables are `const` and tagged FRC_PROGMEM, so they live in flash.
 *   - The clock runs on exact integer arithmetic (64-bit with 128-bit
 *     intermediates). Floating point appears only in the optional convenience
 *     API guarded by FRC_NO_FLOAT; no reading ever depends on it.
 *   - Text is UTF-8, written into a caller buffer, always NUL-terminated.
 *
 * VOCABULARY (read this before anything else)
 *   time      frc_time_t: Unix time in MICROSECONDS (UTC, 86400-second days).
 *             Never TT. Feed the device clock straight in (SPEC S2.1).
 *   catalogue the greatest-eclipse instants E[0..n] of one Saros series.
 *   position  a coordinate in Saros units: eclipse k is position k, linear in
 *             time between eclipses. Always kept exact as ordinal + num/den.
 *   level     zero-based rung of the hierarchy, 0..62. Level 0 is Alpha.
 *             depth = level + 1 is the user-facing number. THE API ALWAYS
 *             TAKES LEVELS. (SPEC pitfall 6.)
 *   cycle     one period of a level. A level-L cycle spans 4 / P(L) Saros.
 *   local     index of a cycle inside its parent, 0-based.
 *   unit      one of the nine named units: Alpha Beta Gamma Delta (major),
 *             Nuclide Helion Preon Flavon Blink (minor). See frc_unit_t.
 *   enums     Functions take the enum types (frc_unit_t, frc_quarter_t ...).
 *             STRUCT FIELDS hold the same values in a uint8_t, to keep the
 *             structs small and their layout the same on every ABI; the
 *             field's comment names the enum. C converts by itself; C++ and
 *             Swift need the cast: frc_format_quarter((frc_quarter_t)th.name, ...).
 *
 * BOUNDARY INSTANTS
 *   A boundary (cycle start, threshold, odometer change, sync start) is an
 *   exact rational instant. The API returns it as THE FIRST MICROSECOND AT OR
 *   AFTER THE BOUNDARY (ceiling). So reading the clock at a returned instant
 *   always shows the new state, and "sleep until next_change, then redraw"
 *   never stalls. An interval [start, end) is half-open, with ONE exception:
 *   the catalogue's closing instant belongs to the last cycle, sync and
 *   Pentad (S2.2), so a reading taken exactly there has t == end.
 *   An instant is a whole microsecond, so a returned boundary may lie up to
 *   one microsecond after the exact one. frc_span_between allows for that:
 *   between two returned boundaries of the levels 0..12 (cycles, thresholds,
 *   syncs, Pentads, dates) it gives the exact count of units.
 *   A "next" instant (frc_threshold_t.time, frc_odometer_t.next_change,
 *   frc_sync_t.next.time) is STRICTLY AFTER the instant asked about. At the
 *   catalogue's closing instant nothing comes later: it is FRC_TIME_NONE.
 *
 * ERRORS
 *   Functions return frc_status_t (FRC_OK = 0, errors negative). Functions that
 *   write text return int: the number of bytes written (without the NUL) or a
 *   negative frc_status_t. Outside the catalogue the clock is undefined: the
 *   functions return FRC_ERR_RANGE and never extrapolate (SPEC S2.2).
 *   A NULL pointer or an unusable catalogue is FRC_ERR_ARG, and an argument
 *   error is reported before a range error. The text buffer is the exception:
 *   buf == NULL is a buffer of size 0 and answers FRC_ERR_BUFFER, like every
 *   buffer that is too small. FRC_ERR_ARG from a text function always means
 *   another argument; it then leaves "" in the buffer.
 *
 * QUICK START
 *   const frc_catalogue_t *cat = &FRC_SAROS_141;
 *   frc_time_t now = FRC_TIME_FROM_MS(unix_ms);   // or frc_time_from_ms(unix_ms)
 *   frc_date_t date;   char text[96];
 *   if (frc_date_at(cat, now, &date) == FRC_OK)
 *       frc_format_date(&date, FRC_DATE_SHORT, FRC_UNIT_PREON, text, sizeof text);
 *   // -> "Delta Trifracton End ε#5 34η 29ι"
 *   frc_odometer_t helion;                       // the basic reading (S15.3)
 *   frc_odometer_at(cat, now, 6, 2, 2, &helion); // level 6, 2 strokes, 2 glyphs
 *   // draw helion.digit[0..1] with frc_glyph_from_index + frc_glyph_path,
 *   // redraw at helion.next_change.
 */
#ifndef FRACTONICA_H
#define FRACTONICA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRC_VERSION_MAJOR 0
#define FRC_VERSION_MINOR 2
#define FRC_VERSION_PATCH 0
#define FRC_VERSION "0.2.0"
/** The SPEC.md version this core implements. */
#define FRC_SPEC_VERSION "2.2.0"

/* ------------------------------------------------------------------------- */
/* Platform: flash placement                                                  */
/* ------------------------------------------------------------------------- */

/*
 * FRC_PROGMEM tags every constant table. On ESP32, ARM and WebAssembly `const`
 * data already lives in flash / read-only memory and the tag is empty. On
 * Harvard-style targets (AVR, ESP8266) it expands to PROGMEM and the core reads
 * its tables through pgm_read_* (see src/frc_internal.h).
 *
 * TO OVERRIDE THE DETECTION define BOTH macros before including this header
 * (on the compiler command line, so the library's sources see them too):
 *   FRC_PROGMEM             the tag itself (an attribute, PROGMEM, or nothing)
 *   FRC_PROGMEM_IS_HARVARD  1 when the tag moves the tables out of the data
 *                           address space, so that they must be read with
 *                           pgm_read_byte / pgm_read_word / pgm_read_dword
 *                           (which must then be declared: this header includes
 *                           the pgmspace header on AVR and ESP8266, elsewhere
 *                           include yours first); 0 for plain reads.
 * The tag and the way of reading belong together: a table placed in flash and
 * read as RAM returns garbage without any diagnostic. So on AVR and ESP8266 an
 * overridden FRC_PROGMEM without FRC_PROGMEM_IS_HARVARD is a compile error;
 * on other targets it means plain reads (0).
 *
 * A custom catalogue table (frc_catalogue_t.instants_ms) must carry the tag too.
 */
#ifndef FRC_PROGMEM
#  if defined(__AVR__)
#    include <avr/pgmspace.h>
#    define FRC_PROGMEM PROGMEM
#    define FRC_PROGMEM_IS_HARVARD 1
#  elif defined(ESP8266)
#    include <pgmspace.h>
#    define FRC_PROGMEM PROGMEM
#    define FRC_PROGMEM_IS_HARVARD 1
#  else
#    define FRC_PROGMEM
#    define FRC_PROGMEM_IS_HARVARD 0
#  endif
#elif !defined(FRC_PROGMEM_IS_HARVARD)
#  if defined(__AVR__) || defined(ESP8266)
#    error "FRC_PROGMEM is overridden on a Harvard target: also define FRC_PROGMEM_IS_HARVARD (1 = read tables with pgm_read_*, 0 = plain reads)"
#  else
#    define FRC_PROGMEM_IS_HARVARD 0
#  endif
#elif FRC_PROGMEM_IS_HARVARD
#  if defined(__AVR__)
#    include <avr/pgmspace.h>
#  elif defined(ESP8266)
#    include <pgmspace.h>
#  endif
#endif

/* ------------------------------------------------------------------------- */
/* Basics                                                                     */
/* ------------------------------------------------------------------------- */

/** Unix time in microseconds (UTC, every day 86,400 s). SPEC S2.1. */
typedef int64_t frc_time_t;

/** "No such instant": a boundary that falls outside the catalogue. */
#define FRC_TIME_NONE INT64_MIN

#define FRC_TIME_FROM_S(s)   ((frc_time_t)(s) * 1000000)
#define FRC_TIME_FROM_MS(ms) ((frc_time_t)(ms) * 1000)
#define FRC_US_PER_SECOND    1000000
#define FRC_US_PER_DAY       86400000000LL

/*
 * The same as functions, for bindings that cannot see a C macro (Swift imports
 * neither FRC_TIME_NONE nor a function-like macro; a WebAssembly client sees
 * exported functions only). Unlike the macros they are checked.
 */
/** 1 when `t` is FRC_TIME_NONE, the "no such instant" answer; 0 for a time. */
int frc_time_is_none(frc_time_t t);
/** Unix milliseconds / seconds as a frc_time_t. FRC_TIME_NONE when the value
 *  does not fit microseconds (|ms| > INT64_MAX / 1000, |s| > INT64_MAX / 1000000). */
frc_time_t frc_time_from_ms(int64_t unix_ms);
frc_time_t frc_time_from_s(int64_t unix_s);

typedef enum frc_status {
    FRC_OK           = 0,
    FRC_ERR_RANGE    = -1, /* instant / cycle / sync outside the catalogue; also
                              civil fields that name no real moment
                              (frc_civil_validate, frc_parse_civil)               */
    FRC_ERR_ARG      = -2, /* invalid argument (level, strokes, NULL, field value)  */
    FRC_ERR_OVERFLOW = -3, /* the exact result does not fit the return type        */
    FRC_ERR_BUFFER   = -4, /* text buffer too small (it still holds a valid prefix) */
    FRC_ERR_PARSE    = -5, /* text is not a date / stamp                            */
    FRC_ERR_REFUSED  = -6  /* a device cannot make this rung (S16.4)                */
} frc_status_t;

/** A fraction of a whole in unsigned 0.32 fixed point: 0 = 0, 2^32 would be 1. */
typedef uint32_t frc_frac_t;
#define FRC_FRAC_HALF    0x80000000u
#define FRC_FRAC_QUARTER 0x40000000u
#ifndef FRC_NO_FLOAT
/** frc_frac_t as a double in [0, 1). */
#define FRC_FRAC_TO_DOUBLE(f) ((double)(f) * (1.0 / 4294967296.0))
/** The same as a function (see frc_time_is_none above). Exact: 32 bits fit a double. */
double frc_frac_to_double(frc_frac_t fraction);
#endif

/* ------------------------------------------------------------------------- */
/* S2  Catalogue and position                                                 */
/* ------------------------------------------------------------------------- */

/**
 * The eclipse catalogue of one Saros series (SPEC S2.2, S14).
 * `instants_ms` are Unix MILLISECONDS (the published precision), strictly
 * increasing, tagged FRC_PROGMEM. Ordinal k is instants_ms[k]; the catalogue
 * covers positions 0 .. count-1.
 * A descriptor filled by hand instead of frc_catalogue_init PROMISES what that
 * function checks: count >= 2, a strictly increasing table, and every instant
 * within INT64_MAX / 1000 ms of 1970 (so it fits frc_time_t in microseconds).
 * The core does not validate the table again on every call.
 */
typedef struct frc_catalogue {
    const int64_t *instants_ms; /* E[0..count-1], Unix ms, in flash               */
    uint16_t count;             /* number of eclipses (>= 2)                      */
    uint16_t series;            /* Saros series number, 0 if unnamed              */
    uint16_t anchor;            /* solar-sync anchor ordinal, a multiple of 4     */
} frc_catalogue_t;

/** Solar Saros 141 (SPEC S14): 70 eclipses, 1613-05-19 .. 2857-06-13, anchor 20. */
extern const frc_catalogue_t FRC_SAROS_141;

/**
 * Fill `cat` for another series. Chooses the anchor by the rule of SPEC S7.2:
 * the first ordinal A = 0 (mod 4) whose eclipse falls on or after 1972-01-01
 * (falls back to the last multiple of 4 that still starts a full Alpha, or 0).
 * The first such ordinal is taken even when no full Alpha follows it; the sync
 * functions then answer FRC_ERR_RANGE wherever no whole sync fits.
 * @return FRC_ERR_ARG if cat or instants_ms is NULL, count < 2, the table is
 *         not strictly increasing, or an instant does not fit frc_time_t in
 *         microseconds (|ms| > INT64_MAX / 1000). `cat` is then left untouched.
 */
frc_status_t frc_catalogue_init(frc_catalogue_t *cat, const int64_t *instants_ms,
                                uint16_t count, uint16_t series);

/** Instant of eclipse `ordinal`, or FRC_TIME_NONE when ordinal >= count. */
frc_time_t frc_catalogue_instant(const frc_catalogue_t *cat, uint16_t ordinal);
/** First and last instant of the catalogue; the clock is defined on [first, last].
 *  FRC_TIME_NONE for an unusable catalogue. */
frc_time_t frc_catalogue_first(const frc_catalogue_t *cat);
frc_time_t frc_catalogue_last(const frc_catalogue_t *cat);
/** 1 when first <= t <= last. */
int frc_catalogue_contains(const frc_catalogue_t *cat, frc_time_t t);

/**
 * An exact position: ordinal + num/den Saros units, with 0 <= num < den.
 * `den` is the length of the Saros interval that holds the instant and `num`
 * the time elapsed inside it, both in microseconds.
 */
typedef struct frc_position {
    uint16_t ordinal;
    uint64_t num;
    uint64_t den;
} frc_position_t;

/**
 * Position of instant `t` (SPEC S2.2): E[k] <= t < E[k+1].
 * The catalogue's closing instant belongs to its last interval: t == last reads
 * as one microsecond before it (ordinal count-2, num = den-1).
 * @return FRC_ERR_RANGE outside [first, last]; FRC_ERR_ARG for a NULL argument
 *         or an unusable catalogue.
 */
frc_status_t frc_position_at(const frc_catalogue_t *cat, frc_time_t t, frc_position_t *out);

/**
 * Instant of the rational position n/d Saros units (d > 0): the first
 * microsecond at or after it. n/d == count-1 gives the closing instant.
 * @return FRC_TIME_NONE when n/d > count-1, d == 0 or the catalogue is unusable.
 */
frc_time_t frc_time_at_position(const frc_catalogue_t *cat, uint64_t n, uint64_t d);

#ifndef FRC_NO_FLOAT
/** Position as a double (about 1e-14 Saros of error). Display only. */
double frc_position_to_double(const frc_position_t *pos);
#endif

/* ------------------------------------------------------------------------- */
/* S3  Harmonic hierarchy                                                     */
/* ------------------------------------------------------------------------- */

/** The ladder has 63 rungs: levels 0..62 (depths 1..63). SPEC S16.1. */
#define FRC_LEVEL_COUNT 63
/**
 * Deepest level the CLOCK can read at an instant (cycle index, phase, glyph).
 * Chosen so every cycle index fits 64 bits; its cycles last ~10 ns, far below
 * the microsecond time grain. Static facts (names, periods, lengths) exist for
 * all FRC_LEVEL_COUNT levels.
 */
#define FRC_CLOCK_MAX_LEVEL 21
/** The factor sequence S = [5, 13, 5, 5, 7, 11, 9, 4] repeats with this period. */
#define FRC_SEQUENCE_LENGTH 8

/** S[level mod 8]: how many level+1 cycles one cycle of `level` contains. */
uint8_t frc_factor(uint8_t level);
/** S[(level-1) mod 8]: cycles of `level` inside one parent cycle. 0 for level 0. */
uint8_t frc_siblings(uint8_t level);
/**
 * P(level): level-`level` cycles per 4 Saros (P(0) = 1, P(L) = P(L-1) * S[(L-1) mod 8]).
 * Exact for level <= 22; returns 0 when the value does not fit 64 bits.
 */
uint64_t frc_prefix(uint8_t level);
/** P(level) as an exact decimal string for any level 0..63 (up to 53 digits). */
int frc_format_prefix(uint8_t level, char *buf, size_t size);

/** What a level is in the notation of SPEC S15. */
typedef enum frc_level_kind {
    FRC_KIND_MAJOR   = 0, /* alpha beta gamma delta: said in words               */
    FRC_KIND_UNIT    = 1, /* epsilon eta iota lambda nu: the minor units          */
    FRC_KIND_BETWEEN = 2, /* zeta theta kappa mu: levels, but not units           */
    FRC_KIND_SUB     = 3  /* xi and finer: shorter than a Blink                   */
} frc_level_kind_t;
frc_level_kind_t frc_level_kind(uint8_t level);

/** Greek name of a level (S3.4): "α" .. "ω", then "α1" .. "ο2". At most 5 bytes + NUL.
 *  The rule of S3.4 has no end, so any uint8_t level is named (63 is "π2",
 *  the rung S16.1 mentions and leaves out; 255 is "π10"). */
int frc_format_level_name(uint8_t level, char *buf, size_t size);
/** The letter's English word: "Alpha" .. "Omega", then "Alpha1" ... Any level, as above. */
int frc_format_level_word(uint8_t level, char *buf, size_t size);

/** The nine named units (S15.1), longest first. */
typedef enum frc_unit {
    FRC_UNIT_ALPHA   = 0, /* α level 0   72.12 y    5 Beta                       */
    FRC_UNIT_BETA    = 1, /* β level 1   14.42 y   13 Gamma                      */
    FRC_UNIT_GAMMA   = 2, /* γ level 2    1.11 y    5 Delta                      */
    FRC_UNIT_DELTA   = 3, /* δ level 3   81.05 d    5 Nuclide                    */
    FRC_UNIT_NUCLIDE = 4, /* ε level 4   16.21 d   77 Helion                     */
    FRC_UNIT_HELION  = 5, /* η level 6    5.052 h  36 Preon                      */
    FRC_UNIT_PREON   = 6, /* ι level 8    8.421 min 65 Flavon                    */
    FRC_UNIT_FLAVON  = 7, /* λ level 10   7.773 s  25 Blink                      */
    FRC_UNIT_BLINK   = 8, /* ν level 12 310.92 ms  the finest unit               */
    FRC_UNIT_COUNT   = 9
} frc_unit_t;

/** "Not a level": what frc_unit_level answers for a value that is not a unit. */
#define FRC_LEVEL_NONE 0xFF
/** Level of a unit: 0 1 2 3 4 6 8 10 12. FRC_LEVEL_NONE when `unit` is not a frc_unit_t. */
uint8_t frc_unit_level(frc_unit_t unit);
/** The unit that sits on `level`, or -1 when the level is not a unit. */
int frc_unit_of_level(uint8_t level);
/** Exact length of a unit in Blinks: 1, 25, 1625, 58500, 4504500, ... 7319812500.
 *  0 when `unit` is not a frc_unit_t. */
uint64_t frc_unit_blinks(frc_unit_t unit);
/** How many of the next finer unit make one of this unit (Blink, or not a unit: 0). */
uint8_t frc_unit_parts(frc_unit_t unit);
/** "η". FRC_ERR_ARG (and "") when `unit` is not a frc_unit_t; frc_format_unit_name likewise. */
int frc_format_unit_symbol(frc_unit_t unit, char *buf, size_t size);
/** "Helion", or "Helions" when plural != 0. */
int frc_format_unit_name(frc_unit_t unit, int plural, char *buf, size_t size);

/* ------------------------------------------------------------------------- */
/* S3.2, S6  Cycles, counters and thresholds                                  */
/* ------------------------------------------------------------------------- */

/** The four quarters of a cycle, named by the threshold that ends each (S6.2). */
typedef enum frc_quarter {
    FRC_PEAK   = 0, /* ends at 1/4: glyph value +M                              */
    FRC_NODE   = 1, /* ends at 2/4: value 0, falling                            */
    FRC_VALLEY = 2, /* ends at 3/4: value -M                                    */
    FRC_END    = 3  /* ends at 4/4: the next cycle begins                       */
} frc_quarter_t;

/**
 * One cycle of one level, and where an instant sits in it.
 * Up to level 18 a cycle is longer than a microsecond: start < end, and
 * frc_cycle_at(start) is this very cycle. Levels 19..21 have cycles SHORTER
 * than a microsecond (345 ns .. 9.9 ns): several of them begin inside the same
 * microsecond, so for a cycle asked for by index start may equal end,
 * frc_cycle_at(start) may show a later cycle (by at most 3 / 15 / 102 cycles
 * at level 19 / 20 / 21), and the closing instant, read one microsecond
 * early, shows a cycle up to that many before the last. A cycle returned by
 * frc_cycle_at holds its instant, so start <= t < end at every level, except
 * at the catalogue's closing instant, which belongs to the last cycle: there
 * t == end (and a walk down the tree with `start <= t && t < end` must take
 * the last child). The tiling holds at every level too: end of cycle c ==
 * start of c+1.
 */
typedef struct frc_cycle {
    uint8_t    level;
    uint8_t    quarter;   /* frc_quarter_t the instant is in (0 when by index)  */
    uint8_t    clipped;   /* 1 when the catalogue ends inside this cycle        */
    uint16_t   local;     /* index inside the parent; level 0: == cycle         */
    uint16_t   siblings;  /* cycles per parent; level 0: Alphas in catalogue    */
    uint64_t   cycle;     /* global index: floor(position * P(level) / 4)       */
    frc_frac_t phase;     /* fraction of the cycle done, linear in POSITION     */
    frc_time_t start;     /* first microsecond of the cycle                     */
    frc_time_t end;       /* first microsecond after it (clipped: closing instant) */
} frc_cycle_t;

/**
 * The cycle of `level` running at `t` (S3.2). `local` is also the sibling
 * counter of S6.1: light markers 0..local of `siblings`.
 * @return FRC_ERR_RANGE outside the catalogue, FRC_ERR_ARG when level > FRC_CLOCK_MAX_LEVEL,
 *         FRC_ERR_OVERFLOW when the level's cycle indices do not fit 64 bits on this
 *         catalogue (more than 320 eclipses at level 21, more than 2238 at level 20:
 *         no real series). The same holds for frc_cycle_by_index, frc_cycle_points,
 *         frc_next_threshold, frc_glyph_at and frc_odometer_at.
 */
frc_status_t frc_cycle_at(const frc_catalogue_t *cat, frc_time_t t, uint8_t level, frc_cycle_t *out);

/**
 * The cycle with global index `cycle` (phase and quarter are 0).
 * @return FRC_ERR_RANGE when cycle >= frc_cycle_count(cat, level).
 */
frc_status_t frc_cycle_by_index(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle, frc_cycle_t *out);

/** Number of cycles of `level` in the catalogue, the last partial one included.
 *  0 for an unusable catalogue, level > FRC_CLOCK_MAX_LEVEL, or the overflow above. */
uint64_t frc_cycle_count(const frc_catalogue_t *cat, uint8_t level);

/*
 * Navigating the tree needs no functions:
 *   parent of cycle c at level L       : c / frc_siblings(L)        at level L-1
 *   first child of cycle c at level L  : c * frc_factor(L)          at level L+1
 *   children count                     : frc_factor(L)
 * Half-cycles (the unit of the arc geometry below):
 *   half index = 2 * cycle + (phase >= FRC_FRAC_HALF);  progress = phase << 1
 */

/**
 * The five quarter points of a cycle: out[0] start, [1] peak, [2] node,
 * [3] valley, [4] end. Points beyond the catalogue are FRC_TIME_NONE.
 */
frc_status_t frc_cycle_points(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle, frc_time_t out[5]);

/** A threshold: which quarter boundary, and when. */
typedef struct frc_threshold {
    uint8_t    name; /* frc_quarter_t */
    frc_time_t time; /* FRC_TIME_NONE when it falls after the catalogue */
} frc_threshold_t;

/**
 * The first quarter boundary of `level` strictly after `t` (S6.2). The
 * returned time is > t. Every eclipse is a quarter boundary of every level,
 * so inside the catalogue there always is a next one (the closing instant at
 * the latest); asked AT the closing instant, the time is FRC_TIME_NONE and
 * the name is still the running quarter's.
 */
frc_status_t frc_next_threshold(const frc_catalogue_t *cat, frc_time_t t, uint8_t level, frc_threshold_t *out);

/** "Peak" "Node" "Valley" "End"; lowercase when lower != 0. FRC_ERR_ARG for another value. */
int frc_format_quarter(frc_quarter_t quarter, int lower, char *buf, size_t size);

/**
 * The instant at which cycle `cycle` of `level` reaches `phase`: the first
 * microsecond at or after the exact instant where the cycle's phase (linear
 * in POSITION, as frc_cycle_t.phase) equals phase / 2^32. The inverse of
 * frc_cycle_at's phase, with the ceiling of BOUNDARY INSTANTS: phase 0 gives
 * the cycle's start, FRC_FRAC_QUARTER its peak (frc_cycle_points[1]),
 * FRC_FRAC_HALF its node. So frc_cycle_at(result) has index `cycle` and a
 * phase >= `phase`, and one microsecond earlier the phase is below it (or
 * the cycle has not begun): the smallest such instant. Exceptions, both
 * inside one microsecond of a boundary: when no microsecond of the cycle is
 * at or after that phase, because the phase point and the cycle's end lie
 * in the same microsecond, the result is the cycle's end, which reads as the
 * next cycle. That can happen at ANY level for a phase close enough to the
 * end, and is common where cycles last a few microseconds: often at level 18
 * (about 1.7 us; nearly a third of random phases there), now and then at
 * levels 15..17, and at levels 19..21 a whole cycle may be shorter than one
 * microsecond. And a result equal to the catalogue's closing instant reads
 * one microsecond early (frc_position_at).
 * Replaces a client's interpolation of an instant inside a cycle (a scrub).
 * @return FRC_TIME_NONE for an unusable catalogue, level > FRC_CLOCK_MAX_LEVEL,
 *         cycle >= frc_cycle_count(cat, level), or a phase of the last
 *         (clipped) cycle that lies after the closing instant.
 */
frc_time_t frc_time_at_phase(const frc_catalogue_t *cat, uint8_t level, uint64_t cycle, frc_frac_t phase);

/* ------------------------------------------------------------------------- */
/* S4  Phase glyphs                                                           */
/* ------------------------------------------------------------------------- */

/** A glyph has 1..FRC_MAX_STROKES strokes (trits). 2 strokes = 16 states, 3 = 52, 4 = 160. */
#define FRC_MAX_STROKES 5

/** M = (3^strokes - 1) / 2: the extreme value. A cycle has 4M states. 0 if strokes invalid. */
uint16_t frc_glyph_extreme(uint8_t strokes);
/** 4M: the number of states of one glyph. */
uint16_t frc_glyph_states(uint8_t strokes);

/** One state of a phase glyph (S4.1, S4.2). */
typedef struct frc_glyph {
    uint16_t index;      /* 0 .. 4M-1                                            */
    int16_t  value;      /* -M .. +M                                             */
    int8_t   derivative; /* +1 rising, 0 at peak and valley, -1 falling          */
    uint8_t  dotted;     /* 1 on the falling branch only (pitfall 5)             */
    uint8_t  strokes;
    int8_t   trit[FRC_MAX_STROKES]; /* balanced ternary, trit[0] LEAST significant
                                       (drawn at the top, S9.1); unused are 0   */
} frc_glyph_t;

/** Decode state `index` (taken mod 4M) of a `strokes`-stroke glyph. */
frc_status_t frc_glyph_from_index(uint16_t index, uint8_t strokes, frc_glyph_t *out);
/** The glyph of `level` at `t`, ascending family: index = floor(position*P*M) mod 4M. */
frc_status_t frc_glyph_at(const frc_catalogue_t *cat, frc_time_t t, uint8_t level, uint8_t strokes, frc_glyph_t *out);
/** The same state in the descending family: (index + 2M) mod 4M. 0 if strokes invalid. */
uint16_t frc_glyph_index_descending(uint16_t index, uint8_t strokes);
/** Glyph state of a fraction of a cycle: floor(phase * 4M). 0 if strokes invalid. */
uint16_t frc_glyph_index_of_phase(frc_frac_t phase, uint8_t strokes);
/**
 * The glyph state a cycle shows at the threshold that ENDS quarter `quarter`
 * (S6.2): frc_glyph_index_of_phase of the phase where it falls, decoded.
 *   FRC_PEAK    phase 1/4   state M    value +M, derivative 0
 *   FRC_NODE    phase 1/2   state 2M   value 0, falling (dotted)
 *   FRC_VALLEY  phase 3/4   state 3M   value -M, derivative 0
 *   FRC_END     phase 0     state 0    value 0, rising: the next cycle's start
 * With 2 strokes (M = 4) the states are 4, 8, 12 and 0.
 * @return FRC_ERR_ARG for another quarter, strokes outside 1..FRC_MAX_STROKES
 *         or out == NULL (`out` is then untouched).
 */
frc_status_t frc_glyph_at_quarter(frc_quarter_t quarter, uint8_t strokes, frc_glyph_t *out);

/* ------------------------------------------------------------------------- */
/* S5  Odometer                                                               */
/* ------------------------------------------------------------------------- */

#define FRC_MAX_DIGITS 4

/** k glyphs side by side: digit[0] is the coarse phase, each next one runs a
 *  full cycle inside one state of its left neighbour. */
typedef struct frc_odometer {
    uint8_t    count;                  /* k                                     */
    uint8_t    strokes;                /* d                                     */
    uint16_t   states;                 /* q = 4M, states per digit              */
    uint16_t   digit[FRC_MAX_DIGITS];  /* glyph indices, coarsest first         */
    frc_time_t next_change;            /* when the finest digit next changes,
                                          strictly after the instant read;
                                          FRC_TIME_NONE at the closing instant  */
} frc_odometer_t;

/**
 * Odometer of `level` at `t` with `digits` glyphs of `strokes` strokes (S5).
 * digit[0] equals frc_glyph_at(...).index. Redraw at next_change, do not poll.
 * Every eclipse is a boundary of every odometer grid, so the last state ends
 * exactly at the closing instant: next_change is FRC_TIME_NONE only there.
 * @return FRC_ERR_ARG for strokes outside 1..FRC_MAX_STROKES, digits outside
 *         1..FRC_MAX_DIGITS or level > FRC_CLOCK_MAX_LEVEL (checked before the
 *         instant), FRC_ERR_RANGE outside the catalogue.
 */
frc_status_t frc_odometer_at(const frc_catalogue_t *cat, frc_time_t t, uint8_t level,
                             uint8_t strokes, uint8_t digits, frc_odometer_t *out);

/** Odometer digits of a bare fraction: floor(phase * q^k) split into k digits.
 *  next_change is FRC_TIME_NONE. 2 digits of 2 strokes = the 256-state pair. */
frc_status_t frc_odometer_of_phase(frc_frac_t phase, uint8_t strokes, uint8_t digits, frc_odometer_t *out);

/* ------------------------------------------------------------------------- */
/* S7  Solar sync and Pentads                                                 */
/* ------------------------------------------------------------------------- */

/* The sync is fixed by SPEC S7.2: 19 Helions (level 6) ~ 4 civil days, counted
 * from the catalogue's anchor eclipse. */
#define FRC_SYNC_LEVEL  6
#define FRC_SYNC_CYCLES 19
#define FRC_SYNC_DAYS   4

/** One solar sync and where an instant sits in it (S7.1).
 *  LAYOUT where int64_t is 8-aligned (64-bit hosts, wasm32, ESP32, ARM EABI):
 *  sync @0, quarter @4, helion @5, phase @8, start @16, end @24, nominal @32,
 *  drift_us @40, next @48 (name @48, time @56); 64 bytes. `helion` is new in
 *  0.2.0 and fills what was padding there: no other offset and not the size
 *  changed. Where int64_t is 1-aligned (AVR) there is no padding: `helion`
 *  moved phase and every later field by one byte, so every object and every
 *  stored record built on the 0.1 layout must be rebuilt. */
typedef struct frc_sync {
    int32_t         sync;      /* number, 0 at the anchor, negative before it   */
    uint8_t         quarter;   /* quarter of the sync the instant is in         */
    uint8_t         helion;    /* the running Helion's place in the sync, 0..18
                                  (0 from frc_sync_by_number)                   */
    frc_frac_t      phase;     /* (t - S) / (E - S), linear in TIME: floor of
                                  2^32 times it, on the EXACT rational start S
                                  and end E, not on the rounded fields below   */
    frc_time_t      start;
    frc_time_t      end;
    frc_time_t      nominal;   /* E[anchor] + sync * 4 days                     */
    int64_t         drift_us;  /* start - nominal: the drift against civil days */
    frc_threshold_t next;      /* next quarter of the sync (about a day apart)  */
} frc_sync_t;

/**
 * The solar sync running at `t`. The quarter and next.name are those of the
 * exact phase. `helion` is the place in the sync of the Helion running at t,
 * 0..18 (FRC_SYNC_CYCLES Helions per sync), for every instant this function
 * answers: the Helion (level-6 cycle) index at t minus that of the sync's
 * first Helion. Only a series that closes exactly on a sync has a reading at
 * its closing instant: it is read one microsecond early like every other
 * reading (phase just under 1, quarter FRC_END) and next.time is FRC_TIME_NONE.
 * @return FRC_ERR_RANGE outside the catalogue AND inside its first / last
 *         partial sync, whose start or end would fall outside it;
 *         FRC_ERR_ARG when cat->anchor is not an eclipse that starts a Helion
 *         (it must be a multiple of 4 and < count);
 *         FRC_ERR_OVERFLOW when the nominal start, the drift or the sync's
 *         length times P(6) does not fit 64 bits (hand-built tables only).
 *         frc_sync_by_number and frc_sync_odometer return the same.
 */
frc_status_t frc_sync_at(const frc_catalogue_t *cat, frc_time_t t, frc_sync_t *out);
/** Sync number `number`: phase, quarter and helion are 0 and next is {0, FRC_TIME_NONE}.
 *  FRC_ERR_RANGE if not whole in the catalogue. */
frc_status_t frc_sync_by_number(const frc_catalogue_t *cat, int32_t number, frc_sync_t *out);
/** The sync phase as an odometer (S7.3): reference 3 glyphs of 2 strokes; alternative 2 of 3.
 *  The digits are floor(exact phase * q^k), not derived from the 32-bit phase
 *  field (q^k need not divide 2^32). */
frc_status_t frc_sync_odometer(const frc_catalogue_t *cat, frc_time_t t, uint8_t strokes, uint8_t digits, frc_odometer_t *out);
/**
 * The instant at which sync `sync` reaches `phase`: the first microsecond at
 * or after start + phase / 2^32 * (end - start) on the EXACT start and end
 * (the sync phase is linear in TIME, see frc_sync_t.phase). So frc_sync_at
 * of the result is sync `sync` with a phase >= `phase`, and one microsecond
 * earlier the phase is below it or the sync has not begun. Phase 0 is the
 * sync's start, FRC_FRAC_QUARTER the end of its first quarter (next.time of
 * a reading in that quarter). Replaces a client's interpolation of an
 * instant on the Sun's ring.
 * @return FRC_TIME_NONE wherever frc_sync_by_number(cat, sync) is not FRC_OK:
 *         a sync that is not whole in the catalogue (FRC_ERR_RANGE), a bad
 *         anchor or an unusable catalogue (FRC_ERR_ARG), and a sync whose
 *         nominal start, drift or length times P(6) does not fit 64 bits
 *         (FRC_ERR_OVERFLOW: hand-built tables that span most of the int64
 *         range only), even where the instant itself would fit.
 */
frc_time_t frc_time_at_sync_phase(const frc_catalogue_t *cat, int32_t sync, frc_frac_t phase);

/** A Pentad: about a day, one of the four runs of Helions of a sync (S7.5).
 *  LAYOUT where int64_t is 8-aligned: sync @0, index @4, helion @5, helions @6,
 *  first @7, start @8, end @16; 24 bytes. `first` is new in 0.2.0 and fills
 *  what was padding there: no other offset and not the size changed. Where
 *  int64_t is 1-aligned (AVR) `start` moved from 7 to 8 and `end` with it:
 *  rebuild everything built on the 0.1 layout. */
typedef struct frc_pentad {
    int32_t    sync;
    uint8_t    index;   /* 0 Pentad I, 1 Pentad II, 2 Pentad III, 3 Hypopentad  */
    uint8_t    helion;  /* the running Helion's place in it, 1..5 (1..4)        */
    uint8_t    helions; /* 5, or 4 for the Hypopentad                           */
    uint8_t    first;   /* the place in the sync of its first Helion: the sum
                           of the Helions of the Pentads before it, 0 5 10 15
                           (frc_sync_t.helion of its first Helion); the same
                           for a Pentad cut at the catalogue's edge          */
    frc_time_t start;   /* cut at the catalogue's edges                         */
    frc_time_t end;
} frc_pentad_t;

/** The Pentad running at `t`: start <= t < end, and t == end only at the
 *  catalogue's closing instant, which belongs to the last Pentad. Defined in
 *  the partial syncs too. FRC_ERR_ARG for a bad anchor, as frc_sync_at. */
frc_status_t frc_pentad_at(const frc_catalogue_t *cat, frc_time_t t, frc_pentad_t *out);
/** Pentad `index` (0..3) of sync `sync` (helion is 0). FRC_ERR_RANGE when wholly outside
 *  the catalogue: it ends at or before the first instant, or starts at or after the
 *  closing instant (no Pentad of zero length is returned). FRC_ERR_ARG for index > 3
 *  or a bad anchor. */
frc_status_t frc_pentad_by_number(const frc_catalogue_t *cat, int32_t sync, uint8_t index, frc_pentad_t *out);

typedef enum frc_pentad_form {
    FRC_PENTAD_FULL    = 0, /* "Pentad II"   with helion: "Pentad II, 3rd Helion" */
    FRC_PENTAD_SHORT   = 1, /* "⬠II" / "◻"   with helion: "⬠II·3"                */
    FRC_PENTAD_ASCII   = 2, /* "P2" / "H"    with helion: "P2.3"                  */
    FRC_PENTAD_NUMERAL = 3  /* "II" / ""     the numeral alone, for a drawn mark  */
} frc_pentad_form_t;
/** Write a Pentad. with_helion != 0 needs pentad->helion in 1..5 (frc_pentad_by_number
 *  leaves it 0): FRC_ERR_ARG otherwise, as for an index above 3 or an unknown form.
 *  FRC_PENTAD_NUMERAL writes the Roman numeral alone, "I" "II" "III", for the
 *  mark a client draws (a pentagon around it), and the EMPTY string for the
 *  Hypopentad, whose drawn square is the whole mark (the return value is 0).
 *  It never writes the Helion: with_helion is checked as above and adds
 *  nothing. The longest text of any form is 22 bytes + NUL ("Pentad III, 5th Helion"). */
int frc_format_pentad(const frc_pentad_t *pentad, frc_pentad_form_t form, int with_helion, char *buf, size_t size);

/* ------------------------------------------------------------------------- */
/* S15  Notation: dates, stamps, cycle names, spans, rates                    */
/* ------------------------------------------------------------------------- */

/**
 * A moment said in Fractonica terms (S15.3): ordinals of the major periods,
 * the Nuclide, the Helion NUMBERED FROM 1, and COUNTS of completed Preons,
 * Flavons and Blinks from 0.
 */
typedef struct frc_date {
    uint16_t alpha;      /* 1..Alphas in catalogue (global)                      */
    uint8_t  beta;       /* 1..5   in its Alpha                                  */
    uint8_t  gamma;      /* 1..13  in its Beta                                   */
    uint8_t  delta;      /* 1..5   in its Gamma                                  */
    uint8_t  epsilon;    /* 1..5   Nuclide in its Delta                          */
    uint8_t  eta;        /* 1..77  Helion in its Nuclide                         */
    uint8_t  iota;       /* 0..35  Preons completed in the Helion                */
    uint8_t  lambda;     /* 0..64  Flavons completed in the Preon                */
    uint8_t  nu;         /* 0..24  Blinks completed in the Flavon                */
    uint8_t  quarter[4]; /* frc_quarter_t of alpha, beta, gamma, delta           */
} frc_date_t;

/** The date at `t`. */
frc_status_t frc_date_at(const frc_catalogue_t *cat, frc_time_t t, frc_date_t *out);
/**
 * The first microsecond of the Blink a date names (the quarters are ignored:
 * the counts imply them). Inverse of frc_date_at down to the Blink.
 * @return FRC_ERR_ARG for a field outside its fixed range (alpha 0 included),
 *         FRC_ERR_RANGE when the date is well formed but not in the catalogue
 *         (an Alpha it does not have, or a Blink after its closing instant).
 */
frc_status_t frc_date_to_time(const frc_catalogue_t *cat, const frc_date_t *date, frc_time_t *out);

typedef enum frc_date_form {
    FRC_DATE_FULL          = 0, /* Alpha Hexafracton Valley, Beta Tetrafracton Valley, Gamma Nonafracton Valley, Delta Trifracton End, ε#5 34η 29ι */
    FRC_DATE_SHORT         = 1, /* Delta Trifracton End ε#5 34η 29ι                    */
    FRC_DATE_LETTERS       = 2, /* α#6 β#4 γ#9 δ#3 ε#5 34η 29ι   (the stamp)           */
    FRC_DATE_SHORT_LETTERS = 3  /* δ#3 ε#5 34η 29ι                                     */
} frc_date_form_t;

/**
 * Write a date. `finest` is the last unit shown: FRC_UNIT_DELTA .. FRC_UNIT_BLINK
 * (coarser values are treated as FRC_UNIT_DELTA). The stamp of S15.3 is
 * FRC_DATE_LETTERS down to FRC_UNIT_BLINK. Longest output is under 160 bytes.
 * @return FRC_ERR_ARG for an unknown form or finest, or a quarter[] above 3
 *         (checked in every form, the letter forms included).
 */
int frc_format_date(const frc_date_t *date, frc_date_form_t form, frc_unit_t finest, char *buf, size_t size);

/**
 * One term of the stamp, exactly as FRC_DATE_LETTERS writes it: an ordinal
 * with '#' for Alpha .. Nuclide ("α#6", "β#4", "ε#5"), a count before the
 * symbol for Helion .. Blink ("34η", "29ι", "21λ", "3ν"). The nine terms of a
 * date joined by single spaces are its stamp (FRC_DATE_LETTERS down to
 * FRC_UNIT_BLINK). The field is written as it is held, like frc_format_date
 * does; the quarters are not read. The longest text is 8 bytes + NUL ("α#65535").
 * @return FRC_ERR_ARG (and "") for date == NULL or a `unit` that is not a frc_unit_t.
 */
int frc_format_date_term(const frc_date_t *date, frc_unit_t unit, char *buf, size_t size);

/**
 * Read a date back from any form frc_format_date writes, and from plain-ASCII
 * shorthand (a b g d e h i l n for α β γ δ ε η ι λ ν; "a#6 b#4 g#9 d#3 e#5 34h 29i").
 * A leading Pentad ("Pentad II ·", "⬠II", "P2") and quarter words are skipped.
 * Parts missing at the coarse end (a short form) are taken from `context`
 * (pass NULL to require a full date); parts missing at the fine end are the
 * start: Helion 1, counts 0. `*finest` (may be NULL) receives the finest unit
 * that was written. Quarters of the result are 0; use frc_date_to_time and
 * frc_date_at to normalise.
 * The rules: an ordinal needs its '#' ("a#6", never "a6"); case does not
 * matter; the parts must be consecutive units of the ladder (a gap, a
 * duplicate or a part out of order is FRC_ERR_PARSE); quarter words are read
 * and dropped, not checked against the counts; a Pentad in the ASCII of
 * frc_to_ascii ("PII.3") is skipped too. *out and *finest are written only
 * on success.
 * @return FRC_ERR_PARSE on text it cannot read, also when one of its numbers
 *         is out of range, and for a short form with context == NULL;
 *         FRC_ERR_ARG on a field out of range in text that is a date, or in a
 *         context field the result takes over. (frc_parse_civil, of Earth
 *         dates, answers FRC_ERR_RANGE for a field out of range: each parser
 *         keeps the code its clients already handle.)
 */
frc_status_t frc_parse_date(const char *text, const frc_date_t *context, frc_date_t *out, frc_unit_t *finest);

/** Fracton ordinal (S15.2): "Monofracton" (1) .. "Icosafracton" (20); beyond that "#25". */
int frc_format_ordinal(uint16_t n, char *buf, size_t size);

/**
 * The name of cycle `cycle` (global index) of `level`, level <= FRC_CLOCK_MAX_LEVEL (S15.3):
 *   major levels  "Delta Trifracton"
 *   level 4       "ε#2"
 *   finer         where it starts in its Nuclide: "ε#2 5η 9ι"; a sub-Blink cycle "… 3.43ν"
 * With short_form != 0: "δ#3", "ε#2", or the finest placing count alone ("9ι").
 * A start inside a Blink is rounded to two decimals, a half going up, and
 * trailing zeros are dropped; so a start within half a hundredth of the next
 * Blink reads as that Blink ("… 25ν" for 24.9996, although Blinks count 0..24).
 * @return FRC_ERR_ARG for level > FRC_CLOCK_MAX_LEVEL; FRC_ERR_OVERFLOW for
 *         (level 0, cycle UINT64_MAX), whose ordinal would be 2^64.
 */
int frc_format_cycle_name(uint8_t level, uint64_t cycle, int short_form, char *buf, size_t size);

/** A span of position: a signed count of Blinks plus a fraction of one (S15.4). */
typedef struct frc_span {
    uint8_t    negative;
    uint64_t   blinks;
    frc_frac_t frac;
} frc_span_t;

/**
 * The span from t1 to t2 (negative when t2 < t1), measured in position so it
 * is exact across eclipses. The closing instant counts as position count - 1.
 *
 * THE TIME GRAIN. An instant is a whole microsecond, and a returned boundary
 * is the exact one rounded UP to it, so a span between two instants is known
 * to within one microsecond at each end. A span that comes within that grain
 * of a whole number of QUARTER BLINKS is that number: the quarter-Blink grid
 * holds every cycle start and threshold of levels 0..12, every sync, Pentad
 * and date. So frc_span_between(cycle.start, cycle.end) is the cycle's length
 * exactly ("1ι", never "64λ 24ν"), and likewise from threshold to threshold
 * or between frc_date_to_time of two dates. A span one microsecond long, or
 * a whole microsecond or more off the grid, is left as measured, and no span
 * between two different instants is brought to zero. (The microsecond is that
 * of the Saros interval at the end in question; on Saros 141 it is 3.2e-6
 * Blink.)
 * Otherwise the fraction is the exact one cut to 2^-32 Blink, on the side of
 * every rounding mark of frc_format_span that the exact span is on, so the
 * text of the result is the text of the exact span (an exact 1.05 Blinks
 * reads "1.1ν").
 * @return FRC_ERR_RANGE when t1 or t2 is outside the catalogue; FRC_ERR_ARG
 *         for a NULL argument or an unusable catalogue; FRC_ERR_OVERFLOW when
 *         a Saros interval that holds or ends at t1 or t2 is 2^63 microseconds
 *         or longer (a hand-built table that spans most of the int64 range).
 *         `out` is written only on FRC_OK.
 */
frc_status_t frc_span_between(const frc_catalogue_t *cat, frc_time_t t1, frc_time_t t2, frc_span_t *out);
/**
 * Nominal length of one cycle of `level`, 0..62: ζ = 11η, θ = 4ι, ξ = 1/7 ν.
 * Exact for level <= 12 (a whole number of Blinks). Below the Blink the length
 * is a fraction of one and is cut to frc_span_t's grain, 2^-32 Blink: level 13
 * is floor(2^32 / 7) units, level 23 is 1 unit, and from level 24 on the
 * result is the zero span, still with FRC_OK. For the exact ratio at those
 * levels use frc_level_per_blink (to level 35) or frc_format_prefix.
 * @return FRC_ERR_ARG for level >= FRC_LEVEL_COUNT or out == NULL.
 */
frc_status_t frc_span_of_level(uint8_t level, frc_span_t *out);

typedef struct frc_span_part {
    uint8_t  unit;  /* frc_unit_t */
    uint64_t count;
} frc_span_part_t;

/**
 * Break a span into counts over the unit ladder, greedily from the largest
 * unit that fits. Returns up to `max_parts` (<= FRC_UNIT_COUNT) NON-ZERO parts;
 * with keep_zeros the next `max_parts` consecutive units, zeros included.
 * @return the number of parts written (at least 1: a sub-Blink span gives "0 ν");
 *         0 when max_parts is 0 or a pointer is NULL.
 */
uint8_t frc_span_parts(const frc_span_t *span, uint8_t max_parts, int keep_zeros, frc_span_part_t *out);

/**
 * Write a span: "6ι 43λ", "3η 5λ" (zero parts skipped), "−2ε 5η". A span led by
 * the Blink and shorter than 10 Blinks keeps a decimal: "2.6ν", "0.14ν".
 * `max_parts` 0 means the default precision, 2.
 * (SPEC S15.4 words it "shorter than one Flavon ... led by the Blink"; the
 * reference keeps the decimal below 10 Blinks only, and so does this core:
 * 12.4 Blinks read "12ν". See the open issues in README.md.)
 * The decimal, rounded from the span with a half going up: from 1 Blink
 * one decimal ("2.6ν"; 9.96 reads "10ν"); below 1 two significant digits
 * ("0.14ν", "0.0013ν"; 0.997 reads "1ν").
 * THE CUT-OFF, one rule: a span of exactly zero reads "0ν", and EVERY other
 * span below 1/1000 Blink reads "<0.001ν", however small (a non-zero span
 * never reads "0ν"). At S12 vector A one microsecond is 3.2e-6 Blink: 1, 20,
 * 244 and 310 us read "<0.001ν", 311 us "0.001ν", 400 us "0.0013ν".
 * (frc_span_of_level gives the zero span from level 24 on: "0ν".)
 * The text is rounded from what frc_span_t holds. A span measured by
 * frc_span_between holds its fraction on the exact value's side of every
 * rounding mark, so its text is that of the exact span, ties included.
 * THE SIGN: the minus (U+2212) is written only before a text that shows a
 * non-zero digit of the span. "0ν" and "<0.001ν" have no sign, also for a
 * negative span (never "−0ν", never "−<0.001ν"); "−0.0013ν", "−2ε 5η".
 */
int frc_format_span(const frc_span_t *span, uint8_t max_parts, char *buf, size_t size);

/**
 * A span with its direction always said: the text of frc_format_span led by
 * "+" for a positive span ("+6ι 43λ") and by its U+2212 for a negative one
 * ("−20ι 21λ"), by the same sign rule: a text that shows no digit of the
 * span, "0ν" or "<0.001ν", has no sign. Parts as frc_format_span.
 * The longest text is 52 bytes + NUL (max_parts 9: "−2520111555α 4β 12γ 4δ 4ε 76η 35ι 64λ 24ν").
 * @return FRC_ERR_ARG (and "") for span == NULL.
 */
int frc_format_span_signed(const frc_span_t *span, uint8_t max_parts, char *buf, size_t size);

/**
 * A span counted in whole units of ONE unit, CUT toward zero (never rounded):
 * floor(blinks / frc_unit_blinks(unit)), grouped as frc_format_count, then
 * the unit's symbol, and U+2212 before a non-zero count of a negative span.
 * A drift of -6 236.9 Flavons is "−6 236λ"; 24.9 Blinks in Flavons "0λ".
 * The longest text is 43 bytes + NUL (UINT64_MAX Blinks, negative, in ν).
 * @return FRC_ERR_ARG (and "") for span == NULL or a `unit` that is not a frc_unit_t.
 */
int frc_format_span_in_unit(const frc_span_t *span, frc_unit_t unit, char *buf, size_t size);

/**
 * When a countdown's text next changes: the first instant after `t` at which
 * the text of frc_format_span(frc_span_between(cat, now, boundary), max_parts)
 * differs from its text at `t`. A client that shows a span to a coming
 * boundary sleeps until this instant and writes the text again. Exact: it is
 * the instant the measured span (the time grain of frc_span_between
 * included) first falls below the smallest span with the same text, found
 * from the text's rounding rule and checked against the formatter itself.
 * The text at `boundary` itself is "0ν", so for t < boundary the answer is
 * boundary at the latest (from "<0.001ν" it is boundary).
 * The same instants serve frc_format_span_signed (the sign does not change
 * before the boundary) and every max_parts (above 9 it is 9, as in
 * frc_format_span).
 * A SIGNED SPAN THAT CROSSES ZERO (a threshold shown as "+6ι 43λ", then
 * "−20ι 21λ"): before the boundary use this function, which ends at the
 * boundary; from it on use frc_span_next_change_since(cat, boundary, now,
 * max_parts), whose span has the same length and so the same instants.
 * @return FRC_TIME_NONE when t >= boundary (the span no longer shrinks: see
 *         frc_span_next_change_since), when t or boundary is outside the
 *         catalogue, for an unusable catalogue, and where frc_span_between
 *         answers FRC_ERR_OVERFLOW (a Saros interval that holds t or the
 *         boundary is 2^63 us or longer: hand-built tables only).
 *         max_parts as frc_format_span (0 = 2).
 */
frc_time_t frc_span_next_change(const frc_catalogue_t *cat, frc_time_t t, frc_time_t boundary, uint8_t max_parts);

/**
 * The same for a span that GROWS: the first instant after `t` at which the
 * text of frc_format_span(frc_span_between(cat, origin, now), max_parts)
 * differs from its text at `t`, for t >= origin. It is the schedule of an
 * elapsed time since `origin`, and of a signed span after its zero
 * (frc_span_between(cat, now, boundary) past the boundary has the same
 * length and the same instants, with a minus). The answer is the instant the
 * span first reaches the smallest LARGER span with another text, computed
 * from the rounding rule and checked against the formatter, as above. At
 * t == origin the text is "0ν" and the answer is t + 1 ("<0.001ν"), which
 * lasts until 1/1000 Blink; then "0.001ν", "0.0011ν" ...
 * @return FRC_TIME_NONE when t < origin (use frc_span_next_change), when the
 *         text at the catalogue's closing instant is still the text at `t`
 *         (also when t is the closing instant), when t or origin is outside
 *         the catalogue, for an unusable catalogue, and where
 *         frc_span_between answers FRC_ERR_OVERFLOW. max_parts as
 *         frc_format_span (0 = 2).
 */
frc_time_t frc_span_next_change_since(const frc_catalogue_t *cat, frc_time_t origin, frc_time_t t, uint8_t max_parts);

/** Time-lapse presets (S15.5): whole counts of a unit per Blink of real time. */
#define FRC_RATE_COUNT 11
typedef struct frc_rate {
    uint8_t  unit;   /* frc_unit_t                                              */
    uint8_t  count;  /* 0 for real time                                         */
    uint64_t factor; /* displayed time per real time: 1, 5, 25, 325, 1625, ...  */
} frc_rate_t;
/** Preset i (0 = real time, 1 = 5ν, 2 = λ, 3 = 13λ, 4 = ι, 5 = 4ι, 6 = η, 7 = 11η, 8 = ε, 9 = δ, 10 = γ).
 *  Real time is {FRC_UNIT_BLINK, 0, 1}. FRC_ERR_ARG for i >= FRC_RATE_COUNT. */
frc_status_t frc_rate_preset(uint8_t i, frc_rate_t *out);
/** "real time", "4ι per Blink". */
int frc_format_rate(const frc_rate_t *rate, char *buf, size_t size);

/**
 * The true rate of a level in cycles per Blink (pB), exact: P(level) / P(12)
 * reduced to num/den. xi = 7, omicron = 77, pi = 693, rho = 2772; Helion = 1/58500.
 * @return FRC_ERR_OVERFLOW when the value does not fit 64 bits (level > 35),
 *         FRC_ERR_ARG for level >= FRC_LEVEL_COUNT.
 */
frc_status_t frc_level_per_blink(uint8_t level, uint64_t *num, uint64_t *den);
/**
 * "7 pB", "2 772 pB", "34.2 pB"; a rung coarser than the Blink: "1 per 58 500 Blinks".
 * num/den need not be reduced; a zero num or den writes "—" (U+2014).
 * Design ruling 3, first: a rate below 1 pB whose reciprocal den / num is a
 * whole number reads "1 per N Blinks" with N = den / num exactly, grouped:
 * 1/25 (the Flavon) "1 per 25 Blinks", 1/5 (mu) "1 per 5 Blinks", 2/50
 * "1 per 25 Blinks", 1/2 "1 per 2 Blinks". Every other rate as before: a
 * rate slower than 1/25 also reads "1 per N Blinks" with N = den / num
 * rounded, a half going up (2/51: "1 per 26 Blinks"); otherwise rounded from
 * the exact value, a half going up: three significant digits below 99.95
 * ("0.75 pB" for 3/4, "0.04 pB" for 0.04001), one decimal from there, a
 * whole number grouped with U+202F from 999.5.
 */
int frc_format_per_blink(uint64_t num, uint64_t den, char *buf, size_t size);

/* ------------------------------------------------------------------------- */
/* Pure wording: static facts said without an Earth unit                      */
/* ------------------------------------------------------------------------- */

/*
 * A client in "pure" mode shows no second, no hertz and no metre. These
 * functions are the wordings it needs for the static facts of a level: a
 * count (signed or not), a cycle counted in a unit, a rate in cycles per
 * Blink, a length said as a span of light, what a level is made of and is
 * part of, its prefix with its factors, and the words of the design rulings. They are exact integer arithmetic on the hierarchy
 * alone: no catalogue, no Saros interval and no floating point (they exist
 * under FRC_NO_FLOAT). All naming and number formatting lives here; a client
 * shows the text as it comes and does not compose one of its own.
 */

/**
 * A whole number with its digits in groups of three, separated by U+202F
 * NARROW NO-BREAK SPACE, as every grouped number of this library is written:
 * "999", "1 000", "58 500", "142 033 641 750 000". The longest text
 * (UINT64_MAX) is 38 bytes + NUL.
 */
int frc_format_count(uint64_t n, char *buf, size_t size);

/**
 * How many of `unit` one cycle of `level` holds: P(level of unit) / P(level),
 * always a whole number, written as frc_format_count writes it and followed
 * by the unit's symbol without a space. The Helion (level 6) is "58 500ν" in
 * Blinks, "2 340λ" in Flavons, "36ι" in Preons and "1η" in Helions; level 5
 * is "11η". The longest text is 21 bytes + NUL ("7 319 812 500ν").
 * @return FRC_ERR_ARG when `unit` is not a frc_unit_t, when level > 12 (a
 *         cycle shorter than the Blink holds no whole unit), or when the unit
 *         is coarser than the level (frc_unit_level(unit) < level: a Helion
 *         counted in Nuclides).
 */
int frc_format_level_in_unit(uint8_t level, frc_unit_t unit, char *buf, size_t size);

/**
 * The rate of a rung in cycles per Blink (pB), from the EXACT ratio
 * P(level) / P(12), for every level 0..62 (frc_level_per_blink stops at 35).
 *   level <= 12  byte for byte the text of frc_format_per_blink for the ratio
 *                of frc_level_per_blink(level): "1 pB" (level 12), "1 per 5
 *                Blinks" (mu), "1 per 25 Blinks" (the Flavon), "1 per 325
 *                Blinks" ... "1 per 58 500 Blinks" (the Helion): design
 *                ruling 3. `digits` changes nothing there.
 *   level > 12   the rate is a whole number N: 7, 77, 693, 2 772 ... and 42
 *                digits at level 62.
 *                digits == 0: N in full, grouped: "7 pB", "2 772 pB",
 *                "142 033 641 750 000 pB" (level 29).
 *                digits 2..9: N below 1 000 000 is still written in full
 *                ("900 900 pB", level 19); from 1 000 000 up it is
 *                "d.ddd × 10ⁿ pB" with `digits` significant digits, rounded
 *                half to even from the exact N, in the power-of-ten form of
 *                frc_format_quantity: "1.420 × 10¹⁴ pB" (level 29, 4 digits);
 *                4 504 500 (level 20) is the tie "4.504 × 10⁶ pB", and with 9
 *                digits "4.50450000 × 10⁶ pB". A mantissa that rounds up to
 *                10 is written as 1 with the next exponent ("1.000 × 10ⁿ⁺¹");
 *                no rate of the 63 rungs does, none begins with 95 or more.
 * The longest text is 84 bytes + NUL (level 62 in full).
 * @return FRC_ERR_ARG for level >= FRC_LEVEL_COUNT, digits == 1 or digits > 9.
 *         `digits` is checked at every level, also down to the Blink, where
 *         a valid value has no say.
 */
int frc_format_level_rate(uint8_t level, uint8_t digits, char *buf, size_t size);

/** A speed, exact by definition: num/den metres per second. frc_medium_speed
 *  (S16.3, below) gives those of seven media; a caller may give its own. */
typedef struct frc_speed {
    uint64_t num;
    uint64_t den;
} frc_speed_t;

/**
 * A LENGTH said without metres: the span that light in vacuum needs to cover
 * the distance a wave of speed `speed` covers in one period of `level`. In
 * Blinks it is
 *     B = P(12) / P(level) * speed / c      c = frc_medium_speed(FRC_MEDIUM_VACUUM)
 * exact, and the same on every Saros interval. For light in vacuum it is the
 * rung's own span (level 6: "1η"). One Helion in each medium of S16.3 reads
 * "1η", "24ι 1λ", "3.5ν", "2.05ξ", "5.15ο", "1.80ο", "6.15χ".
 *   B >= 1 Blink    the text of frc_format_span(span, 0) for the span of B
 *                   Blinks: whole Blinks, and the fraction cut to 2^-32 on
 *                   the exact value's side of every rounding mark, as
 *                   frc_span_between cuts it. "24ι 1λ", "3.5ν"; exactly 1.05
 *                   Blinks read "1.1ν".
 *   B < 1 Blink     in the LARGEST RUNG THAT FITS: the coarsest level K in
 *                   13..62 of which B is one cycle or more. The count of
 *                   those cycles, B * P(K) / P(12), is written with THREE
 *                   significant digits, rounded half to even from the exact
 *                   value, then the rung's name as frc_format_level_name
 *                   writes it, without a space: "5.15ο", "10.8τ", "1.00ξ"
 *                   (light at level 13). The count is below the factor of
 *                   rung K-1, so it is "d.dd" or "dd.d". A count that the
 *                   rounding brings up to that factor is one whole cycle of
 *                   the next coarser rung and is written as it: 10.95ο reads
 *                   "1.00ξ", and 0.9999 Blinks read "1.00ν".
 *   below level 62  B is less than one cycle of the last rung: the count of
 *                   its cycles is written as a power of ten with three
 *                   significant digits, in the form of frc_format_quantity
 *                   (a space before the name here only): "8.35 × 10⁻⁸ ο2"
 *                   (level 57, chemical transport). A count that rounds up to
 *                   one whole cycle reads "1.00ο2".
 * with_word != 0 appends " light" to any of them: "1η light", "5.15ο light".
 * A speed above c is allowed; the arithmetic is the same. The longest text
 * is 28 bytes + NUL with the word.
 * @return FRC_ERR_ARG for level >= FRC_LEVEL_COUNT or a speed with a zero num
 *         or den; FRC_ERR_OVERFLOW when the span is 2^64 Blinks or more and
 *         frc_span_t cannot hold it (it takes a speed above 2.5e9 c).
 *         Both leave "" in the buffer.
 */
int frc_format_light_span(uint8_t level, frc_speed_t speed, int with_word, char *buf, size_t size);

/** "light": the word frc_format_light_span appends after a space (design
 *  ruling 2), alone, for a client that sets it in its own style. One
 *  definition serves both. 5 bytes + NUL. */
int frc_format_light_word(char *buf, size_t size);

/** What a level is, in the words of design ruling 4: "major" (FRC_KIND_MAJOR),
 *  "unit", "between" (ζ θ κ μ), "sub-Blink" (ξ and finer). The longest text
 *  is 9 bytes + NUL. FRC_ERR_ARG (and "") for a value that is not a frc_level_kind_t. */
int frc_format_level_kind(frc_level_kind_t kind, char *buf, size_t size);

/** A signed whole number grouped as frc_format_count, U+2212 before a
 *  negative one: "4 820", "−3", "0" (never "−0"). INT64_MIN is
 *  "−9 223 372 036 854 775 808", the longest text: 40 bytes + NUL. */
int frc_format_count_signed(int64_t n, char *buf, size_t size);

/**
 * Which of its siblings a cycle is, counted from ONE: "8 of 9" for local 7 of
 * 9 siblings (frc_cycle_t.local counts from zero). Both numbers grouped as
 * frc_format_count; level 0 counts the Alphas of the catalogue ("6 of 18").
 * The longest text is 20 bytes + NUL ("65 535 of 65 535").
 * @return FRC_ERR_ARG (and "") for cycle == NULL, siblings == 0 or
 *         local >= siblings.
 */
int frc_format_cycle_place(const frc_cycle_t *cycle, char *buf, size_t size);

/**
 * What one cycle of a level is made of: frc_factor(level) cycles of the next
 * level, the count, a space, the next level's name: "9 θ" (level 6, the
 * Helion), "5 β" (level 0), "11 η1" (level 29). Counted in levels, not in
 * units (frc_format_level_in_unit counts in units). The longest text is
 * 6 bytes + NUL ("13 λ2", level 57).
 * @return FRC_ERR_ARG (and "") for level >= FRC_LEVEL_COUNT - 1: the last
 *         rung, level 62, has no level below it in the ladder.
 */
int frc_format_level_children(uint8_t level, char *buf, size_t size);
/**
 * How many cycles of a level its parent holds, and the parent's name:
 * frc_siblings(level), " in ", the name of level - 1: "11 in ζ" (level 6),
 * "5 in α" (level 1). The longest text is 9 bytes + NUL ("13 in κ2", level 58).
 * @return FRC_ERR_ARG (and "") for level 0 (Alpha has no parent) and for
 *         level >= FRC_LEVEL_COUNT.
 */
int frc_format_level_in_parent(uint8_t level, char *buf, size_t size);
/**
 * The same for a UNIT, whose parent unit is not its parent level: how many of
 * it make the next coarser unit, " in ", that unit's symbol:
 * frc_unit_parts(unit - 1). The Helion is "77 in ε" (77 Helions to the
 * Nuclide, where its parent level ζ holds 11); the Blink "25 in λ", the Beta
 * "5 in α". The longest text is 8 bytes + NUL.
 * @return FRC_ERR_ARG (and "") for FRC_UNIT_ALPHA (no coarser unit) and for a
 *         value that is not a frc_unit_t.
 */
int frc_format_unit_in_parent(frc_unit_t unit, char *buf, size_t size);

/**
 * P(level) and how it is made: the exact prefix grouped as frc_format_count,
 * " = ", then the factors of the levels above it joined by " · " (U+00B7
 * with a space on each side): "125 125 = 5 · 13 · 5 · 5 · 7 · 11" (level 6),
 * "5 = 5" (level 1), and "1" alone for level 0, which has no factor.
 * EVERY level 0..62 is written in full, also where P passes 64 bits (from
 * level 23): the core holds every prefix exactly (frc_format_prefix), so
 * there is no overflow and no shortened form. Level 62 is the longest text,
 * 428 bytes + NUL (a 52-digit prefix and 62 factors): FRC_PREFIX_FACTORS_SIZE.
 * The texts grow with the level: up to level 9 they fit 64 bytes with the
 * NUL, up to level 14 96 bytes, up to level 18 128 bytes, up to level 37 256.
 * A buffer too small answers FRC_ERR_BUFFER and, as everywhere, keeps a clean
 * prefix, which here may end inside the number: never show it.
 * @return FRC_ERR_ARG (and "") for level >= FRC_LEVEL_COUNT.
 */
int frc_format_prefix_factors(uint8_t level, char *buf, size_t size);
/** A buffer of this many bytes holds the text of frc_format_prefix_factors
 *  for every level (level 62: 428 bytes + NUL). */
#define FRC_PREFIX_FACTORS_SIZE 429

/* ------------------------------------------------------------------------- */
/* S16  Spectral ladder and spatial units                                     */
/* ------------------------------------------------------------------------- */

/**
 * The length of a Saros interval, num/den MILLISECONDS. The caller chooses
 * which interval the ladder is laid from (S16.1).
 */
typedef struct frc_interval {
    uint64_t num;
    uint64_t den;
} frc_interval_t;

/** The interval that holds instant `t` (closing instant: the last interval). */
frc_status_t frc_interval_at(const frc_catalogue_t *cat, frc_time_t t, frc_interval_t *out);
/** The series' mean interval (E[n] - E[0]) / n, NOT reduced to lowest terms
 *  (Saros 141: 39259007522784 / 69). The Blink of S15.5 is its level 12.
 *  {0, 0} for an unusable catalogue; every ladder function refuses an interval
 *  or a speed with a zero num or den (FRC_ERR_ARG, FRC_BAND_NONE, or 0.0). */
frc_interval_t frc_interval_mean(const frc_catalogue_t *cat);
/** A nominal scale from any Alpha length in milliseconds (interval = alpha / 4). */
frc_interval_t frc_interval_of_alpha_ms(uint64_t alpha_ms);
/** The 2010-2028 interval of Saros 141, 568,972,877,000 ms: the one SPEC's tables use. */
#define FRC_INTERVAL_REFERENCE_MS 568972877000ULL

/**
 * A decimal floating-point value: mantissa x 10^exp10. The mantissa has exactly
 * FRC_SCI_DIGITS significant digits (10^16 <= mantissa < 10^17) and is the exact
 * rational result rounded half-to-even. Float-free; converts to a double exactly
 * enough for display, and formats with frc_format_quantity.
 */
#define FRC_SCI_DIGITS 17
typedef struct frc_sci {
    uint64_t mantissa;
    int16_t  exp10;
} frc_sci_t;

/** T(L) = 4 I / P(L): the period of a level, in seconds. Level 0..62. */
frc_status_t frc_period(uint8_t level, frc_interval_t interval, frc_sci_t *seconds);
/** f(L) = P(L) / (4 I): its frequency, in hertz. */
frc_status_t frc_frequency(uint8_t level, frc_interval_t interval, frc_sci_t *hertz);

/** Media of S16.3. A speed is exact by definition: num/den metres per second
 *  (frc_speed_t, declared with the pure wordings above). */
typedef enum frc_medium {
    FRC_MEDIUM_VACUUM   = 0, /* light in vacuum        299,792,458 m/s (default)  */
    FRC_MEDIUM_BOUNDED  = 1, /* light in bounded medium 200,000,000               */
    FRC_MEDIUM_DIAMOND  = 2, /* diamond sound                18,000               */
    FRC_MEDIUM_WATER    = 3, /* water sound                   1,500               */
    FRC_MEDIUM_AIR      = 4, /* air sound                       343               */
    FRC_MEDIUM_NERVE    = 5, /* nerve conduction                120               */
    FRC_MEDIUM_CHEMICAL = 6, /* chemical transport            0.001               */
    FRC_MEDIUM_COUNT    = 7
} frc_medium_t;
/** The speed of a medium; {0, 0} when `medium` is not a frc_medium_t. */
frc_speed_t frc_medium_speed(frc_medium_t medium);
/** "vacuum" */
int frc_format_medium_id(frc_medium_t medium, char *buf, size_t size);
/** "Light in vacuum" */
int frc_format_medium_name(frc_medium_t medium, char *buf, size_t size);
/** "Vacuum + solid" */
int frc_format_medium_phase(frc_medium_t medium, char *buf, size_t size);

/** length(L) = v * T(L), in metres: the distance covered in one period. */
frc_status_t frc_length(uint8_t level, frc_interval_t interval, frc_speed_t speed, frc_sci_t *metres);

/** Where a frequency falls among sound and light (S16.2). */
typedef enum frc_band {
    FRC_BAND_NONE        = 0, /* below 20 Hz, or from 3.4e29 Hz up              */
    FRC_BAND_AUDIBLE     = 1, /* 20 Hz                                           */
    FRC_BAND_RADIO       = 2, /* 20 kHz                                          */
    FRC_BAND_MICROWAVE   = 3, /* 300 MHz                                         */
    FRC_BAND_INFRARED    = 4, /* 300 GHz                                         */
    FRC_BAND_VISIBLE     = 5, /* 400 THz                                         */
    FRC_BAND_ULTRAVIOLET = 6, /* 790 THz                                         */
    FRC_BAND_XRAY        = 7, /* 30 PHz                                          */
    FRC_BAND_GAMMA       = 8  /* 30 EHz .. 3.4e29 Hz                             */
} frc_band_t;
/** The band of a level, compared on the exact frequency (a boundary belongs to the band it starts). */
frc_band_t frc_band(uint8_t level, frc_interval_t interval);
/** "audible", "gamma ray"; "no band" for FRC_BAND_NONE (design ruling 4; SPEC
 *  S16.1 prints a dash there). FRC_ERR_ARG (and "") for a value that is not
 *  a frc_band_t. The longest text is 11 bytes + NUL ("ultraviolet"). */
int frc_format_band(frc_band_t band, char *buf, size_t size);
/**
 * The edges of a band in hertz, exactly as SPEC S16.2 tabulates them:
 * `low_hz` the frequency it starts at (it belongs to the band), `high_hz` the
 * one it goes up to, not including (where the next band starts). Audible
 * [20 Hz, 20 kHz), radio [20 kHz, 300 MHz), microwave [300 MHz, 300 GHz),
 * infrared [300 GHz, 400 THz), visible [400 THz, 790 THz), ultraviolet
 * [790 THz, 30 PHz), X-ray [30 PHz, 30 EHz), gamma ray [30 EHz, 3.4e29 Hz).
 * As frc_sci_t (exact: 20 Hz is {20000000000000000, -15}); write them with
 * frc_format_quantity(FRC_QUANTITY_FREQUENCY): 1 or 2 digits are the SPEC's.
 * These are the very edges frc_band compares with.
 * THE OPEN ENDS: FRC_BAND_NONE is two open ranges, below 20 Hz (the low edge
 * of FRC_BAND_AUDIBLE) and from 3.4 x 10^29 Hz up (the high edge of
 * FRC_BAND_GAMMA); it has no edges of its own.
 * @return FRC_ERR_ARG for FRC_BAND_NONE, a value that is not a frc_band_t, or
 *         a NULL output; then nothing is written.
 */
frc_status_t frc_band_edges(frc_band_t band, frc_sci_t *low_hz, frc_sci_t *high_hz);

/**
 * A device that makes a rung (S16.4). Rounded once from the exact value, half up.
 *   ticks = round(T(L) * rate)          one period on a timer of `rate` ticks/s
 *   step  = round(f(L) * 2^bits / rate) phase increment per sample, `bits` <= 64
 * A period of EXACTLY half a tick counts 1 tick, and a frequency of EXACTLY
 * rate/2 is made (step 2^(bits-1)).
 * @return FRC_ERR_REFUSED for a period under half a tick, a frequency above
 *         rate/2 (tested first), or a step that rounds to nothing;
 *         FRC_ERR_OVERFLOW if the tick count does not fit 64 bits (a step that
 *         is not refused always fits); FRC_ERR_ARG for rate 0, bits 0 or
 *         bits > 64, level > 62, a zero interval part or a NULL output.
 *         *ticks / *step is 0 whenever the status is not FRC_OK.
 */
frc_status_t frc_device_ticks(uint8_t level, frc_interval_t interval, uint64_t rate, uint64_t *ticks);
frc_status_t frc_device_step(uint8_t level, frc_interval_t interval, uint64_t rate, uint8_t bits, uint64_t *step);

/** What a quantity measures: it selects the unit ladder frc_format_quantity uses. */
typedef enum frc_quantity {
    FRC_QUANTITY_TIME      = 0, /* seconds: ys .. ms, s, min, h, d, y (Julian year) */
    FRC_QUANTITY_FREQUENCY = 1, /* hertz:   pHz .. YHz                               */
    FRC_QUANTITY_LENGTH    = 2  /* metres:  ym .. Pm                                 */
} frc_quantity_t;

/**
 * Write a value the way SPEC S16.1 tabulates it, with `digits` significant
 * digits (1..9; 0 means 4): "310.9 ms", "5.052 h", "72.12 y", "3.216 Hz",
 * "93.21 Mm", and outside the SI prefixes "2.454 × 10⁻²⁵ s".
 * Rounded half to even. What SPEC never prints (the full rule is in
 * tools/oracle/LADDER.md, "The formatting rule"): the unit is chosen on the
 * unrounded value; a rounded value that reaches the next unit is written as
 * one of it ("1.000 min", also s -> min -> h -> d -> y); time from 1000 y up
 * reads "d.ddd × 10ⁿ y"; a length from 10^18 m up and a frequency below 1 pHz
 * use the power-of-ten form in m / Hz.
 * @return FRC_ERR_ARG for digits > 9, an unknown quantity, a NULL value or a
 *         mantissa outside [10^16, 10^17). The shorthands below return the same.
 */
int frc_format_quantity(const frc_sci_t *value, frc_quantity_t quantity, uint8_t digits, char *buf, size_t size);

/** Shorthands: the period, frequency or vacuum-light length of a level, formatted. */
int frc_format_period(uint8_t level, frc_interval_t interval, uint8_t digits, char *buf, size_t size);
int frc_format_frequency(uint8_t level, frc_interval_t interval, uint8_t digits, char *buf, size_t size);
int frc_format_length(uint8_t level, frc_interval_t interval, frc_speed_t speed, uint8_t digits, char *buf, size_t size);

#ifndef FRC_NO_FLOAT
/** The double nearest to the exact value, halves to even (S16.4). Defined for
 *  exp10 in -140..140, which covers every value this library produces
 *  (-109..57); 0.0 outside it and for an invalid or NULL frc_sci_t. */
double frc_sci_to_double(const frc_sci_t *value);
/** Exact-rational results rounded once to the nearest double; 0 on a bad level.
 *  They are rounded from the exact quotient, NOT from the 17 digits of
 *  frc_period / frc_frequency / frc_length: frc_sci_to_double of those may
 *  differ by one unit in the last place. */
double frc_period_seconds(uint8_t level, frc_interval_t interval);
double frc_frequency_hz(uint8_t level, frc_interval_t interval);
double frc_length_metres(uint8_t level, frc_interval_t interval, frc_speed_t speed);
#endif

/* ------------------------------------------------------------------------- */
/* S9  Glyph geometry                                                         */
/* ------------------------------------------------------------------------- */

/*
 * A glyph is a short path in design units (S9.1): stem segment 3 per trit,
 * stroke reach 2, curve bow 1, ink thickness 0.3, round caps and joins.
 * Coordinates are int16 in HUNDREDTHS of a design unit (x right, y down),
 * so the whole geometry is integer and resolution-free.
 */
#define FRC_GLYPH_UNIT        100 /* coordinate units per design unit            */
#define FRC_GLYPH_INK          30 /* stroke thickness                            */
#define FRC_GLYPH_DOT_RADIUS   30
#define FRC_GLYPH_DOT_Y      (-175)

typedef enum frc_path_op {
    FRC_PATH_MOVE  = 0, /* pts: x, y                                             */
    FRC_PATH_LINE  = 1, /* pts: x, y                                             */
    FRC_PATH_CUBIC = 2, /* pts: c1x, c1y, c2x, c2y, x, y                         */
    FRC_PATH_DOT   = 3  /* pts: cx, cy, radius  (a filled circle)                */
} frc_path_op_t;

/** Receives one path command. `pts` is valid only during the call. */
typedef void (*frc_path_sink_t)(void *ctx, frc_path_op_t op, const int16_t *pts);

/** Emit the path of a glyph exactly as S9.1 lists it: stem, one stroke per trit, the dot.
 *  Draws nothing for a NULL glyph or sink, strokes outside 1..FRC_MAX_STROKES
 *  or a trit outside -1..1 (frc_glyph_segments likewise). */
void frc_glyph_path(const frc_glyph_t *glyph, frc_path_sink_t sink, void *ctx);

/** Ink bounds of any glyph of `strokes` strokes: x -215..215, y -220..(300*strokes + 15).
 *  They depend on the stroke count only, never on the value. Any of the four
 *  pointers may be NULL; invalid strokes write 0 to all four. */
void frc_glyph_bounds(uint8_t strokes, int16_t *min_x, int16_t *min_y, int16_t *max_x, int16_t *max_y);

/** Receives one straight segment. */
typedef void (*frc_segment_sink_t)(void *ctx, int16_t x0, int16_t y0, int16_t x1, int16_t y1);
/**
 * The same glyph flattened to line segments, for rasterisers without curves:
 * each cubic becomes `curve_steps` segments (clamped to 1..32). The dot is NOT emitted;
 * draw it as a filled circle at (0, FRC_GLYPH_DOT_Y) when glyph->dotted.
 */
void frc_glyph_segments(const frc_glyph_t *glyph, uint8_t curve_steps, frc_segment_sink_t sink, void *ctx);

/* ------------------------------------------------------------------------- */
/* Arc geometry: nested alternating circular arcs                             */
/* ------------------------------------------------------------------------- */

#ifndef FRC_NO_FLOAT
/*
 * The Fractonica way to draw cycles. The unit is the HALF-CYCLE: each half of
 * a cycle is one circular arc, and the two halves bulge to opposite sides, so
 * a cycle is an S-shaped wave. A half-cycle of level L carries frc_factor(L)
 * half-cycles of level L+1, drawn as arcs that stand on it and alternate
 * outward / inward. Repeat to any depth: every arc is the carrier of the next.
 *
 *   carrier (parent) arc ->  frc_arc_child(parent, m, j, odd)  for j = 0..m-1
 *     m   = frc_factor(parent level)
 *     odd = parity of the child's global half-cycle index
 *           (child half index = parent half index * m + j)
 *
 * A child arc is the circle through the two ends of its slot on the carrier
 * that meets the carrier at right angles; its sweep is slot +/- pi.
 * Angles are radians, measured like atan2 with y DOWN (screen coordinates).
 * To draw a whole cycle as a closed ring, use a full-circle carrier
 * (sweep = 2 pi) and give it 2 * frc_factor(level) children.
 */
typedef struct frc_arc {
    float cx, cy;  /* centre                                                     */
    float radius;
    float start;   /* angle of the arc's first point                             */
    float sweep;   /* signed angle travelled; negative = counter-clockwise       */
} frc_arc_t;

/** Child `local` (0..m-1) of `parent` divided in `m`; `odd` selects the bulge side.
 *  Requires |parent.sweep| / m < pi (so m >= 3 on a full-circle carrier).
 *  m == 1 returns the parent unchanged; a NULL parent, m == 0 or a slot of pi
 *  or more returns the empty arc (all fields 0). */
frc_arc_t frc_arc_child(const frc_arc_t *parent, uint8_t m, uint8_t local, int odd);
/** The point at fraction t (0..1) along an arc. x or y may be NULL. */
void frc_arc_point(const frc_arc_t *arc, float t, float *x, float *y);
/** Level-0 "wave" root: half-cycle `half` is a unit semicircle centred at (2*half + 1, 0),
 *  alternating above / below the axis. */
frc_arc_t frc_arc_wave_root(uint32_t half);

/** sin and cos for any finite angle in radians, |error| < 1e-6. No libm.
 *  NaN for a non-finite argument. Assumes IEEE-754 binary32 floats. */
float frc_sin(float radians);
float frc_cos(float radians);
#define FRC_PI 3.14159265358979323846f
#endif

/* ------------------------------------------------------------------------- */
/* Civil time: the Gregorian translation                                      */
/* ------------------------------------------------------------------------- */

/*
 * Nothing above this line knows a civil calendar. These helpers translate for
 * clients that show Earth time next to the clock (and for devices without a
 * time-zone database). Proleptic Gregorian, any year.
 */
typedef struct frc_civil {
    int32_t  year;
    uint8_t  month;       /* 1..12                                               */
    uint8_t  day;         /* 1..31                                               */
    uint8_t  hour;        /* 0..23                                               */
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday;     /* 0 = Monday .. 6 = Sunday (output only)              */
    uint32_t microsecond;
} frc_civil_t;

/** Civil fields of `t` at a fixed UTC offset in seconds (0 = UTC). */
void frc_civil_from_time(frc_time_t t, int32_t utc_offset_s, frc_civil_t *out);
/**
 * The instant of civil fields at a fixed UTC offset. Fields may overflow, and
 * the rule is linear in every field: the month is carried into the year first
 * (month 13 is next January, month 0 last December), then the instant is the
 * 1st of that month plus (day - 1) days, the hours, minutes, seconds and
 * microseconds, minus the offset. So day 32 is 31 days after the 1st and
 * day 0 the day before it. `weekday` is ignored.
 * @return FRC_TIME_NONE for civil == NULL or a year outside -290000..290000.
 *         That bound is round and lies inside the range of frc_time_t (whose
 *         ends are in the years -290308 and 294247), with room for fields that
 *         overflow; an instant in the few thousand years beyond it, which
 *         frc_civil_from_time does read, has no way back.
 */
frc_time_t frc_time_from_civil(const frc_civil_t *civil, int32_t utc_offset_s);
/**
 * "2026-10-08T12:00:00.000Z" (millisecond ISO 8601; a non-zero offset writes "+02:00").
 * Milliseconds are the microsecond field cut, not rounded. A year outside
 * 0..9999 is written "-0044" / "+10000" (ISO 8601: an ASCII hyphen-minus).
 * frc_parse_civil reads every text written for a year in -290000..290000;
 * the years beyond it, which only the int64 ends of frc_time_t reach, are
 * written but not read back. An offset that is not whole minutes
 * is cut towards zero and keeps its sign ("+00:00" for +59 s); only 0 is "Z".
 * @return FRC_ERR_ARG unless the fields are a date and a time of day (month
 *         1..12, day 1..31, hour <= 23, minute <= 59, second <= 59,
 *         microsecond <= 999999; weekday is ignored).
 */
int frc_format_civil(const frc_civil_t *civil, int32_t utc_offset_s, char *buf, size_t size);
/**
 * SPEC S2.3, for building tables only: reinterpret the proleptic-Gregorian
 * fields of a Unix-millisecond value as a Julian-calendar date.
 * It is the bare reinterpretation, applied to ANY value. Which values need it
 * is the table builder's decision: sources list Julian dates before the
 * reform only, so call it for values below FRC_GREGORIAN_REFORM_MS (the
 * reference's F.correctJulianTimestamp has that test built in).
 */
int64_t frc_julian_fields_to_unix_ms(int64_t unix_ms);
/** 1582-10-15T00:00:00Z, the first day of the Gregorian calendar, in Unix milliseconds. */
#define FRC_GREGORIAN_REFORM_MS (-12219292800000LL)

/**
 * Whether civil fields name a real moment: FRC_OK, or FRC_ERR_RANGE for a day
 * the calendar does not have (31 FEB, 2026-02-29, 2026-04-31, month 0 or 13,
 * day 0 or 32) or a time of day that does not exist (hour 24, minute 60,
 * second 60: no leap second, microsecond 1 000 000), and for a year outside
 * -290000..290000 (the range of frc_time_from_civil; frc_civil_from_time of
 * an instant within a few thousand years of the ends of frc_time_t gives
 * such a year, and the Earth wordings refuse it). 2024-02-29 and
 * 2000-02-29 are FRC_OK, 1900-02-29 is not (proleptic Gregorian).
 * `weekday` is not read. FRC_ERR_ARG for civil == NULL.
 */
frc_status_t frc_civil_validate(const frc_civil_t *civil);

/*
 * EARTH WORDING. A client in Earth mode shows dates, clock times and
 * durations; these functions write them, so three clients do not each
 * compose their own. Weekdays and months are English three-letter capitals
 * ("THU", "OCT"). The core cannot know a zone's abbreviation ("CEST"): a
 * client appends its platform's after the text where it shows one.
 */
typedef enum frc_civil_form {
    FRC_CIVIL_DATE_TIME         = 0,  /* "THU 8 OCT 2026 · 12:00:00"  the date line          */
    FRC_CIVIL_DATE              = 1,  /* "THU 8 OCT 2026"                                      */
    FRC_CIVIL_TIME              = 2,  /* "12:00:00"                   a moment of today        */
    FRC_CIVIL_MINUTE            = 3,  /* "12:00"                      the bezel's "END 12:56"  */
    FRC_CIVIL_DAY_MONTH         = 4,  /* "8 OCT"                                               */
    FRC_CIVIL_DAY_MONTH_TIME    = 5,  /* "9 OCT 01:34"                another day, minutes     */
    FRC_CIVIL_MONTH_YEAR        = 6,  /* "JAN 2028"                                            */
    FRC_CIVIL_OFFSET            = 7,  /* "UTC+3", "UTC−2:30", "UTC"   the offset alone         */
    FRC_CIVIL_DAY_MONTH_YEAR    = 8,  /* "3 FEB 2028"                 a moment far away        */
    FRC_CIVIL_DAY_MONTH_SECONDS = 9,  /* "9 OCT 01:34:05"                                      */
    FRC_CIVIL_ISO_DATE          = 10, /* "2026-10-08"                 the date of frc_format_civil */
    FRC_CIVIL_WEEKDAY           = 11, /* "THU"                        one word of the date     */
    FRC_CIVIL_MONTH             = 12, /* "OCT"                                                 */
    FRC_CIVIL_YEAR              = 13, /* "2026"                                                */
    FRC_CIVIL_FORM_COUNT        = 14
} frc_civil_form_t;

/**
 * Write civil fields in one of the forms above. The fields are those of
 * frc_civil_from_time at the offset in force: the text says them as they
 * are, nothing is rounded. Hours, minutes and seconds have two digits, the
 * day and the year none to spare ("8 OCT 800"). The weekday is computed from
 * the date (civil->weekday is not read). The dot of FRC_CIVIL_DATE_TIME is
 * U+00B7 with a space on each side.
 * A year outside 1..9999 has four digits at least, and a sign unless it is
 * 0 ("0000" for 1 BC, "−0044", "+10000"). The minus is U+2212, as in every
 * text of this library, in every form but FRC_CIVIL_ISO_DATE, which writes
 * the ISO 8601 hyphen-minus of frc_format_civil ("-0044-03-15").
 * frc_parse_civil reads both.
 * `utc_offset_s` is read by FRC_CIVIL_OFFSET only: "UTC" and a sign ("+",
 * U+2212) with the hours and, when not zero, ":" and two digits of minutes;
 * the offset is cut toward zero to whole minutes, and one under a minute is
 * "UTC". That form does not read `civil`, which may then be NULL; it takes
 * an offset under a day either way (|utc_offset_s| < 86400, so at most
 * "UTC+23:59"): the offsets frc_parse_civil reads back.
 * What each form is for (the lines of the design; a client composes the
 * longer ones from these and adds nothing of its own but the zone's name):
 *   a date line                 DATE_TIME, then " " and the platform's zone
 *                               abbreviation
 *   a moment of today           TIME, or MINUTE
 *   a moment of another day     DAY_MONTH_TIME, with seconds DAY_MONTH_SECONDS
 *   a moment far away           DAY_MONTH_YEAR
 *   a bezel's short marks       MINUTE, DAY_MONTH, MONTH_YEAR
 *   a zone without a name       OFFSET
 *   one word of a date          WEEKDAY, MONTH, YEAR (the day, the hours ...
 *                               are plain numbers)
 *   compact lines               ISO_DATE + " · " + TIME + " " + OFFSET;
 *                               ISO_DATE + " " + MINUTE; ISO_DATE + " " + TIME
 * The longest text is 32 bytes + NUL ("SUN 31 DEC −290000 · 23:59:59").
 * @return FRC_ERR_ARG (and "") for an unknown form, civil == NULL (except
 *         for FRC_CIVIL_OFFSET), fields that frc_civil_validate refuses, and
 *         for FRC_CIVIL_OFFSET an offset of a day or more either way.
 */
int frc_format_civil_text(const frc_civil_t *civil, int32_t utc_offset_s, frc_civil_form_t form,
                          char *buf, size_t size);

/**
 * An Earth duration: "23 min 53 s", "1 h 2 min", "3 d 4 h", "0.4 s", "9.27 s".
 * THE RULE, rounded ONCE and then worded (so never "60 s" and never "1 min 60 s"):
 *   below 9.995 s   hundredths of a second, a half going up, trailing zeros
 *                   dropped: 400 000 us "0.4 s", 9 271 000 us "9.27 s",
 *                   5 s "5 s"; under 0.005 s "0 s".
 *   from 9.995 s    the units d (86 400 s), h, min, s. The LEADING unit is
 *                   the largest that fits; the text shows it and the next
 *                   max_parts - 1 units (consecutive, down to the second at
 *                   the finest), and the duration is rounded, a half going
 *                   up, to a whole number of the finest of them. Then it is
 *                   counted from the rounded value: 59.6 s is "1 min",
 *                   59 min 59.6 s with 2 parts is "1 h", 1 h 2 min 40 s
 *                   with 2 parts "1 h 3 min". Parts that are zero are not
 *                   written ("1 h", "2 d 5 min" with 3 parts). 9.995 s is "10 s".
 * Days are not carried into years; a count is grouped as frc_format_count
 * ("1 234 d 5 h"). A negative duration is led by U+2212 unless it reads "0 s".
 * The MAGNITUDE is rounded, so for a negative duration "a half going up" is
 * a half away from zero (−5 000 us reads "−0.01 s", −9.995 s "−10 s"): a
 * duration and its negative read the same but for the sign.
 * `max_parts` 0 means 2; more than 4 means 4. The longest text is 37 bytes
 * + NUL ("−106 751 990 d 23 h 59 min 59 s").
 */
int frc_format_duration(int64_t microseconds, uint8_t max_parts, char *buf, size_t size);

/**
 * Read an Earth date and time typed by a person, or written by
 * frc_format_civil / frc_format_civil_text. Letters in any case. A "space"
 * below is the ASCII space (0x20) and nothing else: a tab, a newline or a
 * no-break space is refused. Spaces may lead and trail the whole text.
 *   weekday   "THU 8 OCT 2026", "thu, 2026-10-08"
 *                                     an optional three-letter weekday before
 *                                     the date, then a separator: spaces, a
 *                                     ',' or both (spaces may stand on either
 *                                     side of the ','); "THU8" is refused. It
 *                                     must be the date's own.
 *   date      "2026-10-08"            ISO: year, '-', month, '-', day; the
 *                                     year 1..6 digits, optionally signed
 *                                     ('+', '-', U+2212); month and day 1..2
 *                                     digits ("2026-1-8"); no spaces inside
 *             "8 OCT 2026"            worded: day (1..2 digits, no sign),
 *                                     month, year (1..6 digits, optionally
 *                                     signed), parted by one space or more;
 *                                     the month as its three letters or its
 *                                     English name ("8 October 2026")
 *   time      " 12:00", "T12:00", " · 12:00:00", "12:00:00.5"
 *                                     after the date, led by spaces, by " · "
 *                                     (U+00B7, spaces around it optional), or,
 *                                     after an ISO date only, by 'T' at once:
 *                                     hours (1..2 digits), ':' and two digits
 *                                     of minutes, optional ':' and two of
 *                                     seconds, and after the seconds only an
 *                                     optional '.' and 1..9 digits of a second
 *                                     (cut to the microsecond; "12:00.5" is
 *                                     refused). Without a time: midnight.
 *   offset    "Z", "UTC", "GMT", "+02:00", "+0200", "+02", "+2", "UTC+3",
 *             "UTC−2:30"              after a time only, with or without
 *                                     spaces before it: "Z", or "UTC" / "GMT"
 *                                     followed AT ONCE by an optional signed
 *                                     offset, or a signed offset alone. A
 *                                     signed offset is '+', '-' or U+2212, then
 *                                     hours of 1..2 digits with an optional ':'
 *                                     and two digits of minutes, or four digits
 *                                     hhmm; three digits ("+230") are refused.
 *                                     Hours 0..23, minutes 0..59; "-00:00" is
 *                                     an offset of 0 that the text names.
 * Examples: "2026-10-08", "2026-10-08 12:00", "2026-10-08T12:00:00Z",
 * "2026-10-08T14:00:00+02:00", "8 OCT 2026", "8 OCT 2026 12:00",
 * "THU 8 OCT 2026 · 12:00:00", "2026-10-08T12:00:00.000Z" (frc_format_civil).
 * Nothing else: a zone's name ("CEST", "EST"), a date without a year,
 * 12-hour times, text before or after. Every text frc_format_civil and
 * frc_format_civil_text write for a year in -290000..290000 is read back
 * (the forms that show a whole date, and the OFFSET form after a time).
 * `*out` receives the fields as written (weekday computed from the date);
 * `*utc_offset_s` the offset in seconds when the text names one, else 0;
 * `*has_offset` 1 or 0. The instant is frc_time_from_civil(out, offset), with
 * the caller's own offset when the text names none. utc_offset_s and
 * has_offset may be NULL; every output is written only on FRC_OK.
 * @return FRC_ERR_ARG for text == NULL or out == NULL; FRC_ERR_PARSE for text
 *         of another shape, or a weekday that is not the date's;
 *         FRC_ERR_RANGE for well-formed text that names no real moment:
 *         month 13, "31 FEB 2026", "2026-02-29", hour 24, minute 60, an
 *         offset of 24 hours or more, a year outside -290000..290000
 *         (frc_civil_validate). The shape is checked first, then the range,
 *         then the weekday: "FRI 8 OCT 2026 24:00" is FRC_ERR_RANGE, "SAT 29
 *         FEB 2024" FRC_ERR_PARSE. (frc_parse_date, of the clock's notation,
 *         answers FRC_ERR_ARG for a field out of range instead.)
 */
frc_status_t frc_parse_civil(const char *text, frc_civil_t *out, int32_t *utc_offset_s, int *has_offset);

/* ------------------------------------------------------------------------- */
/* Text utilities                                                             */
/* ------------------------------------------------------------------------- */

/**
 * Rewrite any text this library produces as plain ASCII, for displays and logs
 * without Greek: α β γ δ ε ζ η θ ι κ λ μ ν ξ ο π ρ σ τ υ φ χ ψ ω become
 * a b g d e z h q i k l m n x o p r s t u f c y w; "−" becomes "-", "×" "x",
 * "·" ".", "µ" "u", "⬠" "P", "◻" "H", superscripts "^-25", narrow spaces " ".
 * Also: no-break and thin spaces " ", en and em dash (the "—" that
 * frc_format_per_blink writes for no rate) "-", final sigma "s", a superscript plus "+" in the caret
 * run. Anything else that is not ASCII becomes "?", one per code point, and
 * one per byte for bytes that are not UTF-8.
 * frc_parse_date reads the result back (a short Pentad "⬠II·3" comes out as
 * "PII.3"). `src` and `buf` must not overlap.
 */
int frc_to_ascii(const char *src, char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* FRACTONICA_H */
