# Ladder oracle

`generate-ladder.mjs` writes the expected values for the spectral ladder (SPEC §16) and for the
Gregorian translation, as two self-contained C headers:

| Header | Tests |
|---|---|
| `tests/vectors/oracle_ladder.h` | `frc_period`, `frc_frequency`, `frc_length`, `frc_band`, `frc_device_ticks`, `frc_device_step`, `frc_format_quantity` / `_period` / `_frequency` / `_length` / `_band` / `_prefix`, `frc_sci_to_double`, `frc_period_seconds`, `frc_frequency_hz`, `frc_length_metres`, the media table |
| `tests/vectors/oracle_civil.h` | `frc_civil_from_time`, `frc_time_from_civil`, `frc_format_civil`, `frc_julian_fields_to_unix_ms` |

```
node tools/oracle/generate-ladder.mjs            # reads docs/SPEC.md and ../reference/js, writes tests/vectors
node tools/oracle/generate-ladder.mjs --spec PATH --reference PATH --out DIR
node tools/oracle/generate-ladder.mjs --no-reference     # same output, without the cross-check
```

Node with BigInt and top-level await (tested with Node 24), no packages. The output is deterministic (no clock, no `Math.random`, no locale):
running it twice gives the same bytes. The headers need `<stdint.h>` only, compile as C99, C11 and C++,
and prefix every name with `oracle_ladder_` / `ORACLE_LADDER_`, `oracle_civil` / `ORACLE_CIVIL_` or
`oracle_julian` / `ORACLE_JULIAN_`. Tables are `static const` and tagged unused, so a test may read
only some of them.

## How the values are made

Every value is an exact rational of BigInts and is rounded **once**, into the form the C API returns.

```
T(L)      = 4·n / (1000·d·P(L))  s      for an interval of n/d milliseconds
f(L)      = 1000·d·P(L) / (4·n)  Hz
length(L) = v · T(L)             m      v = num/den m/s
```

The generator stops on the first check that fails. It checks:

- `P(0..63)` against the list of SPEC §16.4;
- the eight exact fractions and the rounded values of SPEC §16.5, and the figures in the prose of
  §16.1 (depth 64) and §16.3 (72.12 light-years, 93,212 km, 106.6 m, 37.31 m, 656.3 nm);
- the names, edges and order of the bands of §16.2, and the media of §16.3, read from SPEC.md;
- all 63 × 3 formatted strings of the table of §16.1, plus its band, siblings and name columns,
  against the rule below;
- with the reference library: every period, frequency and length of all 73 intervals × 63 levels × 7
  media (`F.createScale({basePeriod})`), `F.media`, `F.defaults.unixTimestamps` against SPEC §14, and
  `F.correctJulianTimestamp`;
- every double against the runtime's own decimal-to-double conversion of a 40-digit decimal;
- every civil vector against the runtime's `Date`, and that its fields convert back to the instant.

## oracle_ladder.h

Strings hold exact UTF-8 bytes; a non-ASCII byte is written as an octal escape, and the comment
beside the literal shows the text.

### Intervals

```c
typedef struct { const char *name; uint64_t num, den; uint64_t alpha_ms; } oracle_ladder_interval_t;
oracle_ladder_intervals[ORACLE_LADDER_INTERVAL_COUNT]   /* 73 */
```

| Index | Interval (ms) |
|---|---|
| 0 `ORACLE_LADDER_INTERVAL_REFERENCE` | `568972877000 / 1`, the interval SPEC's tables use (`FRC_INTERVAL_REFERENCE_MS`) |
| 1 `ORACLE_LADDER_INTERVAL_MEAN` | `39259007522784 / 69`, written as SPEC §16.5 writes it (not in lowest terms: both are divisible by 3) |
| 2 + k, k = 0..68 | `E[k+1] − E[k]` of Saros 141, from SPEC §14 |
| 71 `ORACLE_LADDER_INTERVAL_ALPHA_RECIPROCAL` | `2275891054055 / 4`: Alpha = 1 / 439.3883434 pHz, rounded half up to whole ms |
| 72 `ORACLE_LADDER_INTERVAL_ALPHA_THEORY` | `2275886972160 / 4`: Alpha = 4 × 6585.3211 days |

`alpha_ms` is non-zero for the last two: `frc_interval_of_alpha_ms(alpha_ms)` must equal `num/den` in
value. No vector depends on whether an interval is stored in lowest terms.

### The ladder

```c
typedef struct { uint64_t mantissa; int16_t exp10; } oracle_ladder_sci_t;      /* frc_sci_t */

typedef struct {
    uint8_t interval, level, band;              /* band: frc_band_t */
    oracle_ladder_sci_t period, frequency;      /* s, Hz */
    oracle_ladder_sci_t length[7];              /* m, by frc_medium_t */
    uint64_t period_bits, frequency_bits, length_bits;   /* doubles; length is vacuum */
} oracle_ladder_full_t;
oracle_ladder_full[ORACLE_LADDER_FULL_COUNT]    /* 126: levels 0..62 of intervals 0 and 1; row = 63·interval + level */

typedef struct {
    uint8_t interval, level, band;
    oracle_ladder_sci_t period, frequency, length;       /* length: light in vacuum */
    uint64_t period_bits, frequency_bits, length_bits;
} oracle_ladder_spot_t;
oracle_ladder_spot[ORACLE_LADDER_SPOT_COUNT]    /* 639: levels 0 6 12 13 29 30 31 47 62 of intervals 2..72 */
```

**`frc_sci_t`**: `mantissa × 10^exp10` with `10^16 ≤ mantissa < 10^17`, the exact rational rounded
half-to-even at the 17th digit. A mantissa that rounds up to `10^17` becomes `10^16` with `exp10 + 1`.

**Doubles** (`*_bits`, IEEE-754 bit patterns): the double nearest to the **exact rational**, ties to
even. They are for `frc_period_seconds`, `frc_frequency_hz` and `frc_length_metres`.

```c
typedef struct { oracle_ladder_sci_t value; uint64_t bits; } oracle_ladder_sci_double_t;
oracle_ladder_sci_double[ORACLE_LADDER_SCI_DOUBLE_COUNT]   /* 378: period, frequency, vacuum length of each full row */
```

For `frc_sci_to_double`: the double nearest to `mantissa × 10^exp10` itself. **28 of these 378 differ
by one unit in the last place from the double of the exact rational** in `oracle_ladder_full`: 17
digits are not enough to round twice. So `frc_period_seconds` and its two siblings cannot be written
as `frc_sci_to_double(frc_period(...))`; they have to round the exact rational.

### Bands

`band` is `frc_band_t`: 0 none, 1 audible (from 20 Hz), 2 radio (20 kHz), 3 microwave (300 MHz),
4 infrared (300 GHz), 5 visible (400 THz), 6 ultraviolet (790 THz), 7 X-ray (30 PHz), 8 gamma ray
(30 EHz up to, not including, 3.4 × 10²⁹ Hz). The comparison is on the exact frequency; a frequency
equal to an edge is in the band that starts there, and 3.4 × 10²⁹ Hz itself has no band.

```c
oracle_ladder_band_names[9]     /* "—" (U+2014), "audible", "radio", "microwave", "infrared",
                                   "visible", "ultraviolet", "X-ray", "gamma ray" */

typedef struct { uint64_t num, den; uint8_t level, band; } oracle_ladder_band_edge_t;
oracle_ladder_band_edges[ORACLE_LADDER_BAND_EDGE_COUNT]   /* 57 */
```

The edge vectors are intervals `num/den` ms made to put `f(level)` **exactly** on an edge
(`I = 1000·P(L) / (4·B)`), each followed by `num + 1` (a hair slower: the band below) and `num − 1`
(a hair faster: the band that starts there). The neighbours differ from the edge by as little as
4 parts in 10¹⁷, so a comparison made on a double or on the 17-digit value fails them. The top edge,
3.4 × 10²⁹ Hz, has no exact interval in 64 bits; its six vectors are the two closest fractions that
fit, one on each side. Their numerators reach 9 × 10¹⁸, so `4 · num` does not fit 64 bits.

### Devices (SPEC §16.4)

```c
typedef struct { uint8_t interval, level; int8_t status; uint64_t rate, ticks; } oracle_ladder_ticks_t;
oracle_ladder_ticks[ORACLE_LADDER_TICKS_COUNT]   /* 656 */
typedef struct { uint8_t interval, level, bits; int8_t status; uint64_t rate, step; } oracle_ladder_step_t;
oracle_ladder_steps[ORACLE_LADDER_STEP_COUNT]    /* 1968 */
```

Intervals 0 and 1, levels 0..40. `status` is `frc_status_t`: 0 ok, −6 refused, −3 overflow; the value
is 0 unless the status is 0.

- **ticks**: `x = T(L)·rate`. `x < 1/2` → refused. Otherwise `ticks = floor(x + 1/2)`; `ticks ≥ 2^64` →
  overflow. Rates 1, 1000, 32768, 1000000, 80000000, 240000000, and two more that reach the overflow
  branch: 10¹² and 2⁶⁴ − 1. (308 ok, 318 refused, 30 overflow.)
- **step**: `f(L) > rate/2` → refused (checked first, on the exact frequency). Otherwise
  `step = floor(f(L)·2^bits/rate + 1/2)`; `step = 0` → refused. Rates as above without the two extra,
  bits 16, 32, 48, 64. (610 ok, 1358 refused.) A step that is not refused is at most `2^(bits−1)`, so
  with `bits ≤ 64` it always fits: `frc_device_step` can never return overflow, and no vector has it.

### Text

```c
typedef struct { uint8_t level, siblings; const char *name, *period, *frequency, *band, *length; } oracle_ladder_spec_row_t;
oracle_ladder_spec_rows[63]     /* the table of SPEC §16.1, cell for cell, parsed out of SPEC.md */

typedef struct { uint8_t interval, level, quantity, medium, digits; const char *text; } oracle_ladder_format_t;
oracle_ladder_format[ORACLE_LADDER_FORMAT_COUNT]       /* 610 */

typedef struct { oracle_ladder_sci_t value; uint8_t quantity, digits; const char *text; } oracle_ladder_quantity_t;
oracle_ladder_quantity[ORACLE_LADDER_QUANTITY_COUNT]   /* 229 */

oracle_ladder_prefix[64]        /* P(0)..P(63) in decimal: frc_format_prefix */
oracle_ladder_media[7]          /* id, name, phase, speed_num, speed_den of SPEC §16.3 */
```

`quantity` is `frc_quantity_t` (0 time, 1 frequency, 2 length). `oracle_ladder_format` is the text of
`frc_format_period`, `frc_format_frequency` or `frc_format_length` (with the speed of
`oracle_ladder_media[medium]`; `medium` is 0 for the other two):

- rows 0..188: the reference interval at 4 digits, three rows per level in the order period,
  frequency, vacuum length. They equal the SPEC table;
- then levels 12 and 29 in the other six media; then the same two groups on the mean interval;
- then levels 0 2 6 9 12 13 29 41 44 51 62 of the reference interval with `digits` 0, 1, 2, 3, 6, 9,
  and two lengths of the mean interval with 1, 2, 3, 6, 9.

`oracle_ladder_quantity` is `frc_format_quantity` on a given `frc_sci_t`: 112 hand-made values that
sit on every edge of the rule below, each with the text a person worked out (the generator must
reproduce it), and 117 values taken from the ladder.

## The formatting rule

The rule the task gave reproduces all 189 strings of SPEC §16.1 as it stands. SPEC does not print a
value on a unit boundary, a tie, or outside the ladders, so those cases are settled here. This is
the whole rule; `frc_format_quantity` implements it and the vectors hold it to it.

**Input.** A positive value `x` in the base unit (s, Hz, m) and `digits` 1..9; 0 means 4. All
comparisons and roundings are exact, and rounding is half-to-even.

**Units**, ascending, each with its exact size in the base unit:

| Quantity | Units | Ceiling |
|---|---|---|
| time | `ys zs as fs ps ns µs ms s` (10⁻²⁴ … 1), `min` 60, `h` 3600, `d` 86400, `y` 31557600 (Julian year) | 1000 y |
| frequency | `pHz nHz µHz mHz Hz kHz MHz GHz THz PHz EHz ZHz YHz` (10⁻¹² … 10²⁴) | 10²⁷ Hz |
| length | `ym zm am fm pm nm µm mm m km Mm Gm Tm Pm` (10⁻²⁴ … 10¹⁵) | 10¹⁸ m |

1. **Choose the unit on the unrounded value**: the largest unit `u` with `size(u) ≤ x`. So
   `1 ≤ m = x / size(u) < limit`, where `limit` is the next unit's size counted in `u` (1000; 60 for
   `s` and `min`; 24 for `h`; 365.25 for `d`) or, for the last unit, the ceiling (1000).
2. **Round the mantissa** to `digits` significant digits. Let `i` (1, 2 or 3) be the number of digits
   of `floor(m)` and `decimals = digits − i`.
   - `decimals ≥ 0`: `r = round(m · 10^decimals)`. If `r = 10^digits` and `decimals > 0`, the mantissa
     gained a digit: divide `r` by 10 and drop one decimal (9.9996 → `10.00`, 99.96 → `100.0`). Print
     `r` with `decimals` decimals; no decimal point when `decimals` is 0.
   - `decimals < 0`: `r = round(m / 10^−decimals)`, printed as the integer `r · 10^−decimals`
     (439.4 at 1 digit → `400`, 310.9 at 2 → `310`, 95 at 1 → `100`).
3. **Carry.** If the rounded mantissa is `≥ limit`, write one of the next unit: `1`, `1.0`, `1.000`…
   (`digits` significant digits). 999.96 ms → `1.000 s`; 59.9996 s → `1.000 min`; 86399.9 s →
   `1.000 d`; 31557599 s at 7 digits → `1.000000 y`; 55 s at 1 digit → `1 min`. From the last unit the
   carry goes to the ceiling in powers of ten: 999.96 YHz → `1.000 × 10²⁷ Hz`, 999.96 y →
   `1.000 × 10³ y`.
4. Otherwise the text is the mantissa, one space (U+0020) and the unit.

**Below the first unit** (`x < 10⁻²⁴ s`, `10⁻¹² Hz`, `10⁻²⁴ m`): `x = m · 10^k` with `1 ≤ m < 10`,
`m` rounded to `digits` significant digits (`digits − 1` decimals), written `d.ddd × 10ⁿ s`
(`Hz`, `m`). A mantissa that rounds to 10 becomes 1 with `k + 1`. If that carry lands exactly on the
first unit, it is written in the unit: 9.9996 × 10⁻²⁵ s → `1.000 ys`.

**From the ceiling up** (`x ≥ 1000 y`, `10²⁷ Hz`, `10¹⁸ m`): the same power-of-ten form, in years for
time (`x / 31557600`), in Hz and m for the others: `2.338 × 10⁹ y`, `1.324 × 10²⁷ Hz`, `1.000 × 10¹⁸ m`.

**Bytes.** The power-of-ten form is: mantissa, space, `×` (U+00D7), space, `10`, the exponent in
superscript, space, unit. Superscripts: `⁻` U+207B, `⁰` U+2070, `¹` U+00B9, `²` U+00B2, `³` U+00B3,
`⁴`…`⁹` U+2074…U+2079. A positive exponent has no sign. The micro prefix is the micro **sign** `µ`
U+00B5 (bytes C2 B5), as SPEC.md prints it, not the Greek letter μ U+03BC that names level 11.
Every space is U+0020.

**Two ways in, one text.** The text of a level may be made from the exact rational or from its
17-digit `frc_sci_t` (`frc_period` then `frc_format_quantity`). The carry rule makes the two agree
even when the 17-digit rounding crosses a unit boundary. They can only differ when the 17-digit value
lands exactly on a rounding tie that the exact value misses, which needs a run of about 17 − digits
zeros or nines in the value. The generator formats every vector both ways and stops if they differ;
none does.

## oracle_civil.h

Proleptic Gregorian calendar, astronomical years (year 0 is 1 BC), weekday 0 = Monday.

```c
typedef struct {
    int64_t t_us; int32_t utc_offset_s; int32_t year;
    uint8_t month, day, hour, minute, second, weekday; uint32_t microsecond;
    const char *iso;
} oracle_civil_t;
oracle_civil[ORACLE_CIVIL_COUNT]            /* 300 */
```

`frc_civil_from_time(t_us, utc_offset_s)` gives the fields, `frc_time_from_civil` gives `t_us` back,
`frc_format_civil` gives `iso`. 157 chosen instants (the epoch and its neighbours, the three vectors
of SPEC §12, eclipses of Saros 141, leap days and the century rule for years −2000 … 6000, year ends
from −2000 to 9999, offsets that move the date across a day and a year) and 143 from a fixed
pseudo-random stream, half over −2000 … 6000 and half over 1900 … 2100. Offsets are whole minutes:
0, ±60 s, +3600, +7200, +19800, +20700, +32400, +34200, +45900, +50400, −12600, −18000, −28800,
−34200, −43200.

**ISO text.** `YYYY-MM-DDTHH:MM:SS.mmm` then `Z` when the offset is 0, else `+HH:MM` or `-HH:MM` of
the offset.

- `mmm` is the microsecond field **truncated** to milliseconds (999 µs prints `.000`), so the text
  never names a later second than the fields.
- A year 0..9999 is four digits. A negative year is `-` (ASCII hyphen) and at least four digits:
  `-0044`, `-2000`, `-12345`. A year above 9999 is `+` and its digits: `+10000`, `+12345`.
- An offset that is not a whole number of minutes is not in the vectors (suggested: truncate towards
  zero).

```c
typedef struct {
    uint8_t kind; int32_t year; uint8_t month, day, hour, minute, second;
    uint32_t microsecond; int32_t utc_offset_s; int64_t t_us;
} oracle_civil_roll_t;
oracle_civil_roll[ORACLE_CIVIL_ROLL_COUNT]  /* 35 */
```

`frc_time_from_civil` on fields that are not a normal date ("fields may overflow"). The rule is
linear in every field:

```
year'  = year + floor((month − 1) / 12)        month' = (month − 1) mod 12 + 1
t = days_from_civil(year', month', 1) + (day − 1) days + hour h + minute min + second s
    + microsecond µs − utc_offset_s
```

`kind` 0 is a normal date, 1 has fields above their range (day 32 … 255, month 13 … 255, hour 24 … 255,
minute and second 60 … 255, microsecond up to 2³² − 1), 2 has month 0 or day 0, which the same rule
reads as "one before the first" (the header does not say what these do; drop the kind-2 rows if it
decides otherwise).

```c
typedef struct { int64_t unix_ms, fixed_ms, library_ms; } oracle_julian_t;
oracle_julian[ORACLE_JULIAN_COUNT]          /* 43 */
#define ORACLE_JULIAN_REFORM_MS INT64_C(-12219292800000)     /* 1582-10-15T00:00:00Z */
```

SPEC §2.3. `fixed_ms` is the formula applied to `unix_ms` as it stands, with floor division
throughout: this is what `frc_julian_fields_to_unix_ms` is documented to return. `library_ms` is what
the reference `F.correctJulianTimestamp` returns: the same for `unix_ms < ORACLE_JULIAN_REFORM_MS`,
and `unix_ms` unchanged from the reform on. The first 36 rows are before the reform, where the two
agree (years −4712 … 1582, shifts −38 … +10 days, none between 200-03-01 and 300-02-28); the last 7
are from the reform on, where they differ.
