# Oracle: test vectors from the frozen JavaScript reference

The generators in this folder ask the frozen reference (`../reference/js`, read-only) what a correct
clock reads, and write the answers as self-contained C headers into `tests/vectors/`. The C tests
include those headers and hold the core against them. Nothing here is part of the library.

| Generator | Writes | Covers |
|---|---|---|
| `generate-clock.mjs` (this file documents it) | `oracle_clock.h`, `oracle_notation.h` | the clock at an instant (SPEC §2–§7) and how it is said (SPEC §15) |
| `generate-foundation.mjs` (see its header comment) | `oracle_foundation.h` | catalogue, positions, cycles and odometer steps at microsecond instants |
| `generate-ladder.mjs` (see [`LADDER.md`](LADDER.md)) | `oracle_ladder.h`, `oracle_civil.h` | the spectral ladder (SPEC §16) and the Gregorian translation |
| `generate-geometry.mjs` (see its header comment) | `oracle_geometry.h` | glyph paths (SPEC §9.1) and the nested arcs, as the reference draws them |
| `generate-pure.mjs` (see [The pure-wording vectors](#the-pure-wording-vectors)) | `oracle_pure.h` | the four pure wordings (`src/frc_pure.c`); needs `docs/SPEC.md` only, no reference |

`make vectors` runs every `generate-*.mjs`.

## Regenerating the clock and notation vectors

```
cd core
node tools/oracle/generate-clock.mjs            # writes tests/vectors/oracle_clock.h and oracle_notation.h
node tools/oracle/generate-clock.mjs --check    # regenerates in memory; exit 1 if the files on disk differ
FRACTONICA_REFERENCE=/path/to/reference/js node tools/oracle/generate-clock.mjs
```

- Needs Node with BigInt and top-level await (tested with Node 24). No packages.
- The reference is found through `FRACTONICA_REFERENCE` (relative to the working directory), default
  `../reference/js` beside the core repository. It is never written to.
- The output is deterministic: no clock, no `Math.random` (a fixed-seed mulberry32 picks the instants).
  Two runs give the same bytes, so `--check` can guard a commit.
- The run takes about four seconds and ends with a report: sample counts, the number of cross-checks,
  and how many texts differ from the reference's doubles.

To prove the headers are valid C and that their layout is the documented one, without the core:

```
cc -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -Itests/vectors \
   tools/oracle/check-clock-vectors.c -o /tmp/check-clock-vectors && /tmp/check-clock-vectors
```

`check-clock-vectors.c` reads every table through its typedef and checks the invariants that tie the
fields together (a cycle holds its instant, the quarter is the top two bits of the phase, trits spell
the value, a level's cycle is its parent's child, span parts add up to the span, …) plus SPEC §12
vector A. The headers use positional initialisers, so this is what catches a field written out of order.

## How the reference is driven

| File of the reference | Used for |
|---|---|
| `fractonica.js` (`globalThis.Fractonica`) | every number: `F.create({timeScale: 'unix'})`, then `clock.positionAt`, `timestampAt`, `period`, `phaseAt`, `phaseDigits`, `nextThreshold`, `solarSync({anchor: 20}).at / .digits`, all in exact BigInt rationals |
| `src/core/model.js` (`createModel`) | the reading rules (which cycle, the catalogue edge, Pentads). Its doubles are only **cross-checked**: every exact value is held against the model's (104,816 comparisons, within 0.5 ms for times) |
| `src/core/pure.js` (`createPure`) | all texts of SPEC §15 |

Every time and fraction is rounded **once**, in the generator, with BigInt. Nothing passes through a
`Number` on its way to a header.

Before anything is written the generator asserts that the data reproduces **every number of SPEC §12
and §15.6** (vectors A, B, C and anchor + 1 s: positions, indices, values, odometer digits, counters,
thresholds, sync numbers, starts, phases, drifts, digits, the ISO strings, the full and short dates,
the Helion pairs, the stamp at A and the span `6ι 43λ`), the Pentad statements of §7.5, and, when
`docs/SPEC.md` (or a `SPEC.md` beside the repository) is present, that the reference's catalogue is the table of §14. A failed assertion stops
the run and nothing is written.

## Conventions (they are the core's: `include/fractonica.h`)

| | |
|---|---|
| time | `int64` Unix **microseconds**, fields end in `_us`. Every sample instant is a whole millisecond (the model rounds queries to 1 ms). |
| boundary instant | the **first microsecond at or after** the exact rational instant: `ceil(seconds · 10⁶)`. |
| no instant | `ORACLE_CLOCK_NONE` = `INT64_MIN` = the core's `FRC_TIME_NONE`: the boundary is beyond the catalogue. A boundary exactly on the closing instant has a time. |
| position | `ordinal` k, `num` = t − E[k], `den` = E[k+1] − E[k], in microseconds. |
| phase | `uint32`, `floor(exact fraction · 2³²)`. A **cycle's** phase is linear in *position*. The **sync's** phase is linear in *time*, `(t − start) / (end − start)`, with the **exact rational** start and end, not the ceiled microseconds (6 of 508 readings differ in the last bit otherwise; the header lists them). |
| odometer digits | `floor(exact fraction · qᵏ)`. The sync's 2 × 3-stroke digits are not a function of the 32-bit phase in general (52² does not divide 2³²). |
| drift | `start_us − nominal_us`: the ceiled start minus the exact nominal `E[anchor] + number · 4 days`. |
| quarter / threshold names | 0 peak, 1 node, 2 valley, 3 end (`frc_quarter_t`). |
| trits | least significant first. |
| levels | zero-based, never depths. |
| units | `frc_unit_t`: 0 α, 1 β, 2 γ, 3 δ, 4 ε Nuclide, 5 η Helion, 6 ι Preon, 7 λ Flavon, 8 ν Blink. |
| closing instant | E[69] is **not sampled**: the core reads it one microsecond inside the catalogue, the reference one millisecond inside. They agree at E[69] − 1 ms, sample `ORACLE_CLOCK_SAMPLE_CLOSING_MINUS_1MS`, and nothing later is sampled. |
| strings | raw UTF-8 literals. The minus is U+2212, digit groups are U+202F, "nothing" is U+2014, the Pentad shapes U+2B20 and U+25FB, the middle dot U+00B7. |
| names | every identifier starts `oracle_clock_` / `ORACLE_CLOCK_` or `oracle_notation_` / `ORACLE_NOTATION_`. Every array `X` has a macro `X_COUNT`. Tables are `static const` and tagged unused. Both headers need `<stdint.h>` only and compile as C99, C11 and C++ together with the other oracle headers. |

## The sample instants (515; 511 inside the catalogue)

In this order in `ORACLE_CLOCK_SAMPLES`; `ORACLE_NOTATION_DATES` holds the in-range ones in the same order.

1. `A`, `B`, `C`, `anchor+1s`: the four instants of SPEC §12, indices 0–3 (`ORACLE_CLOCK_SAMPLE_A` …).
2. `E[k]` for k = 0…68, `E[k]+1ms` for k = 0…68, `E[k]-1ms` for k = 1…69.
3. `catalogue#i`: 130 instants uniform over the whole catalogue.
4. `modern#i`: 170 instants uniform over 1970-01-01 … 2100-01-01.
5. Four instants outside the catalogue (`in_range == 0`, every other field 0).

`E[0]`, `E[0]+1ms` and `E[69]-1ms` are in range but inside the first / last partial sync: `sync.has == 0`
while the Pentad is defined and cut at the catalogue's edge.

## `tests/vectors/oracle_clock.h`

```c
#define ORACLE_CLOCK_NONE INT64_MIN
#define ORACLE_CLOCK_SERIES 141
#define ORACLE_CLOCK_ANCHOR 20
int64_t ORACLE_CLOCK_CATALOGUE_MS[ORACLE_CLOCK_CATALOGUE_MS_COUNT /* 70 */];   /* E[k], Unix ms */

#define ORACLE_CLOCK_LEVELS 14                                 /* levels 0..13 */

typedef struct oracle_clock_glyph {
  int16_t value;      /* -M .. +M */
  int8_t  derivative; /* +1 rising, 0 at peak and valley, -1 falling */
  uint8_t dotted;     /* 1 on the falling branch only */
  int8_t  trit[3];    /* trit[0] LEAST significant; unused are 0 */
} oracle_clock_glyph_t;

typedef struct oracle_clock_level {      /* frc_cycle_at, frc_glyph_at, frc_odometer_at(2, 2), frc_next_threshold */
  uint64_t cycle;        /* floor(position * P(level) / 4) */
  uint16_t local;        /* cycle mod siblings; level 0: == cycle */
  uint16_t siblings;     /* S[(level-1) mod 8]; level 0: the number of Alphas in the catalogue (18) */
  uint32_t phase;        /* floor(frac(position * P(level) / 4) * 2^32) */
  uint8_t  quarter;
  uint8_t  clipped;      /* 1 when the catalogue ends inside this cycle */
  int64_t  start_us;
  int64_t  end_us;       /* clipped: the closing instant */
  uint16_t index[3];     /* glyph index for 2, 3 and 4 strokes, ascending family */
  oracle_clock_glyph_t glyph2;   /* the 2-stroke state of index[0] */
  oracle_clock_glyph_t glyph3;   /* the 3-stroke state of index[1] */
  uint16_t odometer[2];  /* 2 glyphs of 2 strokes, coarsest first */
  int64_t  odometer_next_us;     /* next change of its finest digit, or NONE */
  uint8_t  next_name;    /* the next threshold strictly after the instant */
  int64_t  next_us;      /* or NONE */
} oracle_clock_level_t;

typedef struct oracle_clock_sync {       /* frc_sync_at, frc_sync_odometer */
  uint8_t  has;          /* 0 inside the first / last partial sync (FRC_ERR_RANGE); the rest is 0 */
  int32_t  number;
  int64_t  start_us, end_us;             /* declared one per line in the header */
  int64_t  nominal_us;
  int64_t  drift_us;
  uint32_t phase;
  uint8_t  quarter;
  uint8_t  next_name;
  int64_t  next_us;
  uint16_t digits_s2d3[3];   /* 3 glyphs of 2 strokes: the reference odometer */
  uint16_t digits_s3d2[2];   /* 2 glyphs of 3 strokes */
} oracle_clock_sync_t;

typedef struct oracle_clock_pentad {     /* frc_pentad_at */
  uint8_t has;           /* 1 for every in-range sample */
  int32_t sync;
  uint8_t index;         /* 0 Pentad I, 1 II, 2 III, 3 Hypopentad */
  uint8_t helion;        /* 1..5 */
  uint8_t helions;       /* 5, or 4 */
  int64_t start_us;      /* cut at the catalogue's edges */
  int64_t end_us;
} oracle_clock_pentad_t;

typedef struct oracle_clock_sample {
  const char *label;
  int64_t  t_us;
  uint8_t  in_range;
  uint16_t ordinal;
  uint64_t num;
  uint64_t den;
  oracle_clock_level_t level[ORACLE_CLOCK_LEVELS];
  uint16_t odo6_s3d2[2];         /* level 6 odometer: 3 strokes, 2 digits */
  int64_t  odo6_s3d2_next_us;
  uint16_t odo6_s2d3[3];         /* level 6 odometer: 2 strokes, 3 digits */
  int64_t  odo6_s2d3_next_us;
  oracle_clock_sync_t sync;
  oracle_clock_pentad_t pentad;
} oracle_clock_sample_t;
oracle_clock_sample_t ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLES_COUNT /* 515 */];
/* indices: ORACLE_CLOCK_SAMPLE_A, _B, _C, _ANCHOR_PLUS_1S, _CLOSING_MINUS_1MS,
            _FIRST_PARTIAL_SYNC, _LAST_PARTIAL_SYNC */

#define ORACLE_CLOCK_DEEP_FIRST_LEVEL 14
#define ORACLE_CLOCK_DEEP_LEVELS 8                             /* levels 14..21 */
typedef struct oracle_clock_deep_level {
  uint64_t cycle;
  uint16_t local;
  uint16_t siblings;
  uint32_t phase;
  uint8_t  quarter;
  int64_t  start_us;
  int64_t  end_us;
  uint16_t index2;       /* glyph index, 2 strokes */
} oracle_clock_deep_level_t;
typedef struct oracle_clock_deep_sample {
  int64_t  t_us;
  uint16_t sample;       /* index into ORACLE_CLOCK_SAMPLES */
  oracle_clock_deep_level_t level[ORACLE_CLOCK_DEEP_LEVELS];   /* level[i] is level 14 + i */
} oracle_clock_deep_sample_t;
oracle_clock_deep_sample_t ORACLE_CLOCK_DEEP_SAMPLES[ORACLE_CLOCK_DEEP_SAMPLES_COUNT /* 60 */];

typedef struct oracle_clock_cycle {      /* frc_cycle_by_index, frc_cycle_points */
  uint8_t  level;        /* 0..21 */
  uint64_t cycle;
  uint16_t local;
  uint16_t siblings;
  uint8_t  clipped;
  int64_t  start_us;
  int64_t  end_us;       /* clipped: the closing instant */
  int64_t  point_us[5];  /* start, peak, node, valley, end; NONE beyond the catalogue */
} oracle_clock_cycle_t;
oracle_clock_cycle_t ORACLE_CLOCK_CYCLES[ORACLE_CLOCK_CYCLES_COUNT /* 197 */];

uint64_t ORACLE_CLOCK_LEVEL_CYCLES[ORACLE_CLOCK_LEVEL_CYCLES_COUNT /* 22 */];  /* frc_cycle_count, index = level */

typedef struct oracle_clock_sync_by_number {     /* frc_sync_by_number */
  int32_t number;
  uint8_t ok;            /* 0: not whole inside the catalogue (FRC_ERR_RANGE); the rest is 0 */
  int64_t start_us;
  int64_t end_us;
  int64_t nominal_us;
  int64_t drift_us;
} oracle_clock_sync_by_number_t;
#define ORACLE_CLOCK_SYNC_FIRST_WHOLE (-32927)
#define ORACLE_CLOCK_SYNC_LAST_WHOLE 80671
oracle_clock_sync_by_number_t ORACLE_CLOCK_SYNCS_BY_NUMBER[ORACLE_CLOCK_SYNCS_BY_NUMBER_COUNT /* 40 */];

typedef struct oracle_clock_pentad_by_number {   /* frc_pentad_by_number */
  int32_t sync;
  uint8_t index;         /* 0..3 */
  uint8_t ok;            /* 0: wholly outside the catalogue (FRC_ERR_RANGE); the rest is 0 */
  uint8_t helions;       /* 5, or 4 for index 3 */
  int64_t start_us;      /* cut at the catalogue's edges */
  int64_t end_us;
} oracle_clock_pentad_by_number_t;
oracle_clock_pentad_by_number_t ORACLE_CLOCK_PENTADS_BY_NUMBER[ORACLE_CLOCK_PENTADS_BY_NUMBER_COUNT /* 64 */];
```

Notes for the test writer:

- **Deep levels.** The reference model stops at level 13. Levels 14–21 are computed in the generator
  from the exact position and P(L), and agree with the library's `clock.period` / `clock.phaseAt`,
  which accept deeper levels. A cycle shorter than a microsecond can have `start_us == t_us` and
  `end_us == t_us + 1`.
- **Cycles by index.** For each level 0–21: cycles 0 and 1, the last two, the one running at vector A
  and four random ones. The last cycle is clipped for levels 0–7 and whole from level 8 down (4
  divides P(L)). A clipped cycle has `end_us` = the closing instant while `point_us[4]` is NONE; a
  quarter point exactly on the closing instant has its time (level 0, cycle 17: its peak is E[69]).
- **Syncs by number.** The whole syncs of Saros 141 are −32927 … 80671. The table holds 0, ±1, the
  §12 syncs (4820, 2375, 7987), −2380, −2379, 8999, 9000, both ends, the partial syncs just outside
  them (`ok == 0`), numbers far outside and 20 random ones.
- **Pentads by number.** Four rows (index 0–3) per sync, for the §12 syncs, both edges and six random
  syncs. Sync −32928 is the cut first sync: Pentad I lies wholly before the catalogue (`ok == 0`),
  Pentad II starts at E[0]. Sync 80672 is the cut last one: Pentad III ends at E[69], the Hypopentad
  is outside. `helions` stays 5 (or 4) for a cut Pentad.

## `tests/vectors/oracle_notation.h`

```c
#define ORACLE_NOTATION_FINEST_COUNT 6   /* text[finest - 3]: FRC_UNIT_DELTA (3) .. FRC_UNIT_BLINK (8) */
#define ORACLE_NOTATION_FORM_COUNT 4     /* frc_date_form_t: 0 full, 1 short, 2 letters, 3 short letters */

typedef struct oracle_notation_date {    /* frc_date_at, frc_format_date, frc_format_pentad */
  int64_t  t_us;
  uint16_t clock_sample;  /* index into ORACLE_CLOCK_SAMPLES (the same instant) */
  uint16_t alpha;         /* frc_date_t, field for field */
  uint8_t  beta, gamma, delta, epsilon, eta, iota, lambda, nu;
  uint8_t  quarter[4];    /* of alpha, beta, gamma, delta */
  const char *text[ORACLE_NOTATION_FINEST_COUNT][ORACLE_NOTATION_FORM_COUNT];
  const char *stamp;      /* letters down to nu == text[5][2] */
  const char *pentad[3][2];   /* [0 full, 1 short, 2 ascii][0 without, 1 with the helion] */
} oracle_notation_date_t;
oracle_notation_date_t ORACLE_NOTATION_DATES[ORACLE_NOTATION_DATES_COUNT /* 511 */];

typedef struct oracle_notation_span_part {
  uint8_t  unit;          /* frc_unit_t */
  uint64_t count;
} oracle_notation_span_part_t;

typedef struct oracle_notation_span {    /* frc_span_between, frc_span_parts, frc_format_span */
  int64_t  t1_us, t2_us;  /* the span runs from t1 to t2 */
  uint8_t  negative;      /* 1 when t2 < t1; 0 for the zero span */
  uint64_t blinks;        /* floor(|pos2 - pos1| * P(12) / 4) */
  uint32_t frac;          /* floor(the rest of a Blink * 2^32) */
  uint8_t  n[3];          /* parts returned for max_parts 2, 3, 9 without keep_zeros */
  uint8_t  nz[3];         /* the same with keep_zeros */
  oracle_notation_span_part_t parts[9];    /* max_parts 9, non-zero parts; 2 and 3 return its first n[0], n[1] */
  oracle_notation_span_part_t zparts[9];   /* max_parts 9, keep_zeros;     2 and 3 return its first nz[0], nz[1] */
  const char *text[2];    /* frc_format_span with max_parts 2 and 3 */
} oracle_notation_span_t;
oracle_notation_span_t ORACLE_NOTATION_SPANS[ORACLE_NOTATION_SPANS_COUNT /* 404 */];

typedef struct oracle_notation_cycle_length {    /* frc_span_of_level, then frc_format_span(.., 2) */
  uint8_t  level;         /* 0..20, index = level */
  uint64_t blinks;        /* P(12) / P(level), whole Blinks */
  uint32_t frac;          /* the fraction of one, for levels below nu */
  const char *text;       /* pure.cycleLength: "1α", "11η", "4ι", "0.14ν", "<0.001ν", "0ν" */
} oracle_notation_cycle_length_t;
oracle_notation_cycle_length_t ORACLE_NOTATION_CYCLE_LENGTHS[ORACLE_NOTATION_CYCLE_LENGTHS_COUNT /* 21 */];

typedef struct oracle_notation_cycle_name {      /* frc_format_cycle_name */
  uint8_t  level;         /* 0..16 */
  uint64_t cycle;         /* global index */
  const char *name;       /* pure.cycleName:               frc_format_cycle_name(level, cycle, 0) */
  const char *name_short; /* pure.cycleName {short: true}: the name without its Nuclide; no core call */
  const char *short_form; /* pure.cycleShort:              frc_format_cycle_name(level, cycle, 1) */
  uint8_t  carried;       /* 1: a sub-Blink start whose 2 decimals round up to the next whole Blink */
} oracle_notation_cycle_name_t;
oracle_notation_cycle_name_t ORACLE_NOTATION_CYCLE_NAMES[ORACLE_NOTATION_CYCLE_NAMES_COUNT /* 157 */];

typedef struct oracle_notation_ordinal {         /* frc_format_ordinal: 1..22 and 25 */
  uint16_t n;
  const char *text;
} oracle_notation_ordinal_t;
oracle_notation_ordinal_t ORACLE_NOTATION_ORDINALS[ORACLE_NOTATION_ORDINALS_COUNT /* 23 */];

typedef struct oracle_notation_rate {            /* frc_rate_preset(i), frc_format_rate; index = i */
  const char *id;         /* the reference's id: "real", "nu5", "lambda", ... */
  uint8_t  unit;          /* frc_unit_t; preset 0 (real time) has no unit and says 8 here */
  uint8_t  count;         /* 0 for real time */
  uint64_t factor;        /* count * Blinks in the unit: 1, 5, 25, 325, 1625, 6500, 58500, ... */
  const char *label;      /* "real time", "4ι per Blink" */
} oracle_notation_rate_t;
oracle_notation_rate_t ORACLE_NOTATION_RATES[ORACLE_NOTATION_RATES_COUNT /* 11 */];

typedef struct oracle_notation_per_blink {       /* frc_level_per_blink, frc_format_per_blink */
  int8_t   level;         /* 0..24: num/den is P(level) / P(12) reduced (index = level); -1: hand-picked */
  uint64_t num;
  uint64_t den;
  const char *text;
} oracle_notation_per_blink_t;
oracle_notation_per_blink_t ORACLE_NOTATION_PER_BLINK[ORACLE_NOTATION_PER_BLINK_COUNT /* 45 */];

typedef struct oracle_notation_s15_6 {           /* SPEC S15.6, row for row */
  const char *vector;     /* "A", "B", "C", "anchor+1s" */
  int64_t  t_us;
  uint16_t clock_sample;
  const char *full_date;  /* FRC_DATE_FULL down to FRC_UNIT_BLINK */
  const char *short_date; /* FRC_DATE_SHORT down to FRC_UNIT_PREON */
  uint16_t helion_pair[2];
} oracle_notation_s15_6_t;
oracle_notation_s15_6_t ORACLE_NOTATION_S15_6[ORACLE_NOTATION_S15_6_COUNT /* 4 */];
#define ORACLE_NOTATION_S15_6_STAMP_A   "α#6 β#4 γ#9 δ#3 ε#5 34η 29ι 21λ 3ν"
#define ORACLE_NOTATION_S15_6_SPAN_T1_US / _T2_US / _SPAN_TEXT    /* A, A + 3372.428 s, "6ι 43λ" */
```

Notes for the test writer:

- **Dates.** `text[f][form]` is `pure.dateText` with `upTo` = delta, epsilon, eta, iota, lambda, nu for
  f = 0…5, so `frc_format_date(&date, form, (frc_unit_t)(f + 3), …)`. The longest text is 135 bytes.
- **Spans.** 404 pairs: the zero span, the span of §15.6, whole Saros intervals (one Saros is exactly
  `1β 3γ 1δ 1ε 19η 9ι`, four are `1α`), sample neighbours, pairs milliseconds apart (sub-Blink
  decimals), pairs within a millisecond of a whole count of Blinks (1, 2, 3, 9, 10, 11, 24, 25, 26, 50,
  1624, 1625, 1626, 58500, 4504500: the unit boundaries), pairs across an eclipse, random pairs, and
  reversed pairs of each kind. The label of a pair is a comment above its row.
- **Parts are a function of `blinks` alone** (every unit is a whole number of Blinks); the text also
  needs `frac` when it is a decimal. The generator checks that every text here comes out the same
  from the 32-bit `frac` as from the exact rational.
- **Sub-millisecond spans cannot be sampled**, so `"<0.001ν"` and a sub-Blink `"0ν"` appear only as
  cycle lengths (levels 16 and 17–20).
- **Cycle names.** The cycle running at a rotating choice of samples for each level 0–16, cycle 0 of
  every level, seven consecutive ζ cycles, and six sub-Blink rows built to exercise the rounding of
  the decimal ν. Four have `carried == 1`: the reference rounds the start to two decimals, so a ρ
  cycle that starts 0.9996 into Blink 24 is named `… 25ν`. That is the reference's rule, written
  down as it is; test those rows separately if the core should ever choose otherwise.

## Exactness wins

`pure.js` words spans, sub-Blink decimals and pB values in doubles, and adds 10⁻¹² Saros (0.57 ms) to
a span before counting units. The core is exact. For those wording rules the generator therefore
carries its own exact implementation (search `exact rule` in `generate-clock.mjs`), following
`pure.decompose`, `blinkCount`, `durationPos`, `countsAtStart`, `cycleName` and `pbText` step for
step, with three differences:

1. in place of the epsilon a span has the core's **time grain** (`exactSpan`, `src/frc_span.c`): an
   instant is a whole microsecond, and a boundary the core returns is rounded up to one, so a span
   that comes within one microsecond of a whole number of quarter Blinks is that number (the
   microsecond is that of the Saros interval holding the microsecond that ends at the instant). It
   is 570 times finer than the epsilon. The fraction stored in `frc_span_t` is the floor to 2⁻³²,
   one unit more when a rounding mark of the text (a multiple of 1/20000 Blink) lies in that last
   unit (`spanFrac32`), so the text of the stored span is the text of the exact one. The span table
   ends with rows between boundaries as the core returns them (cycle start → end, peak → valley,
   start → node, from the cycle table): 228 rows are on the grain;
2. a half always rounds up (`toPrecision`, `toFixed` and `Math.round` do the same on the exact
   value, but a double is rarely exactly on a half);
3. `pbText` compares with 1/25 exactly, not with `0.04 − 1e-9`.

Every vector is still compared with the reference's own text. The ones that differ are listed in a
comment block at the top of `oracle_notation.h`: currently 52 lines (2 and 3 parts) for 26 span pairs.
25 of them are a span within 0.57 ms below a unit boundary (the reference says `1λ`, exactly it is
`24ν`); one is a decimal whose last digit the double rounds the other way (`0.0096ν` / `0.0097ν`, 3 ms
beside E[11], where the difference of two double positions has lost its digits). Cycle lengths, cycle
names, dates, Pentads, ordinals, rates and pB texts agree with the reference on every vector.

## Changing the vectors

Edit `generate-clock.mjs`, run it, run `check-clock-vectors.c`, commit the generator and both
headers together. Never edit a header by hand, and never edit the reference. New samples go at the
end of `buildSamples()` or of a `build…()` table, so existing indices keep their meaning; the four
§12 instants must stay first.

## The pure-wording vectors

`generate-pure.mjs` writes `tests/vectors/oracle_pure.h` for `tests/test_pure.c`. The frozen reference
has no pure wordings to ask, so this generator is the second statement of their rules: every text is
computed from exact BigInt rationals and rounded once. It calls neither the C library nor the reference.

```
node tools/oracle/generate-pure.mjs            # writes tests/vectors/oracle_pure.h
node tools/oracle/generate-pure.mjs --check    # regenerates in memory; exit 1 if the file on disk differs
node tools/oracle/generate-pure.mjs --spec /path/to/SPEC.md --out /some/dir
```

What it reads from `docs/SPEC.md` and asserts before writing (4,590 checks; a failed one writes nothing):

| From | Used for |
|---|---|
| §16.4, `P(0)` … `P(63)` | held against the recurrence of §3.1 (`P(L) = P(L−1) · S[(L−1) mod 8]`) |
| §16.1, the 63 rows of the ladder | the name of every rung, held against the rule of §3.4 stated in the generator (letter `L mod 24` of the alphabet, built from code points; then the number of the round) |
| §15.1, the nine units | symbol, level (depth − 1) and "made of" count of each unit |
| §16.3, the seven media | the speeds, as exact decimals; `c` is the first |
| the examples of `fractonica.h` and of design rulings 2 and 3 | `1 per 58 500 Blinks`, `2 772 pB`, `1.420 × 10¹⁴ pB`, one Helion in every medium (`1η`, `24ι 1λ`, `3.5ν`, `2.05ξ`, `5.15ο`, `1.80ο`, `6.15χ`) … |

The tables (layouts are in the header's own comments):

| Table | Rows | Holds |
|---|---|---|
| `oracle_pure_count` | 132 | `n` and its grouped text: 10ᵏ − 1, 10ᵏ, 10ᵏ + 1 for k = 1..19, the ends of `uint64_t`, the numbers the header prints, a fixed random stream of every bit length |
| `oracle_pure_units` | 9 | level, symbol and name of each `frc_unit_t` |
| `oracle_pure_in_unit` | 117 | every (level 0..12, unit): status, the count `P(unit level) / P(level)`, the text; 62 pairs are refused (the unit is coarser than the level) |
| `oracle_pure_rate` | 63 | per level: the exact rate as decimal strings `num` / `den` (42 digits at level 62) and the text at `digits` 0, 2, 3, 4, 9 |
| `oracle_pure_media` | 7 | id and speed `num / den` of each `frc_medium_t` |
| `oracle_pure_light` | 441 | every (level 0..62, medium): the text without and with the word, and which branch of the rule it took (`form`) |
| `oracle_pure_light_custom` | 624 | speeds of the caller's own: 65 chosen to stand on an edge (`ORACLE_PURE_LIGHT_CHOSEN_COUNT`: exact ties of the span's decimal, of the three digits and of the power of ten; a rest of exactly one, two or three quarter Blinks; counts that round up to a whole cycle of the rung above; faster than light; the largest span that fits and the first that does not; the smallest value there is), then every twentieth of a Blink from 1 to 10 (181: each tenth, each tie between two tenths, each whole number of quarter Blinks, all exact), then 378 from a fixed xorshift64 stream, six at every level |

The rules, as the generator states them:

- **grouping**: a separator U+202F before every third digit from the right.
- **rate**: down to the Blink the per-Blink text of SPEC §15.5 for `1 / (P(12)/P(L))` (halves up, as
  `frc_format_per_blink` documents); below it `N = P(L)/P(12)` in full when `digits` is 0 or `N < 1 000 000`,
  else `N` to `digits` significant digits half to even and a power of ten (`4 504 500` at four digits is the
  tie `4.504 × 10⁶`).
- **light span**: `B = P(12)/P(L) · v/c`. `B ≥ 1`: the span text of SPEC §15.4 on the exact rational (two
  non-zero parts; led by the Blink and under 10 Blinks one decimal, a half going up). `B < 1`: the first
  `K` in 13..62 with `B · P(K)/P(12) ≥ 1`; that count to three significant digits half to even, then the
  name of rung `K`; a count that rounds to `S[K−1]` is `1.00` of rung `K − 1`. No such `K`: the count of
  level 62 as `d.dd × 10ⁿ ο2`, and `1.00ο2` when it rounds up to one.

`form` (`ORACLE_PURE_FORM_*`) lets the test require that every branch was taken: span 95 rows, rung 647,
rung carried 7, power of ten 120, power of ten carried 2.
