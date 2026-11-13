# Arithmetic design

How fractonica-core computes, and why every step is exact and overflow-free.
`SPEC.md` (this folder) says *what* the clock reads; this note says *how* the C
core gets there without floating point, without a 128-bit compiler type and
without a single rounding it does not name.

Symbols used throughout:

| Symbol | Meaning |
|---|---|
| `E[k]` | instant of eclipse `k`, Unix **microseconds** (the table stores milliseconds, times 1000) |
| `I_k` | `E[k+1] − E[k]`, the length of Saros interval `k` in microseconds. Real series: about 5.69·10¹⁴ < 2⁵⁰ |
| `n` | `count − 1`, the last ordinal; `count ≤ 65535` (`uint16_t`) |
| `S` | the factor sequence `[5, 13, 5, 5, 7, 11, 9, 4]`, repeating |
| `P(L)` | `frc_prefix(L)`: level-`L` cycles in 4 Saros. `P(0) = 1`, `P(L) = P(L−1) · S[(L−1) mod 8]` |
| `M` | `(3^d − 1) / 2`, the extreme value of a `d`-stroke glyph; `q = 4M` states per glyph |

Sizes that the bounds below rely on:

| Quantity | Value | Bits |
|---|---|---|
| `P(6)` Helion | 125,125 | 17 |
| `P(12)` Blink | 7,319,812,500 | 33 |
| `P(21)` = `P(FRC_CLOCK_MAX_LEVEL)` | 230,804,667,843,750,000 | 58 |
| `P(22)`, the last that fits `uint64_t` | 2,538,851,346,281,250,000 | 62 |
| `P(62)`, the last rung of the ladder | 52 digits | 172 |
| `q^k` at most (5 strokes, 4 glyphs) | 484⁴ = 54,875,873,536 | 36 |

## 1. Why microseconds

`frc_time_t` is `int64_t` Unix microseconds.

- **The catalogue is in milliseconds** (the published precision, SPEC §14), so
  `E[k]` and `I_k` are exact integers in microseconds and a position's
  denominator is an exact integer.
- **States change faster than once a millisecond.** A boundary is a rational
  instant; the API hands it back as an integer. A two-glyph odometer on the
  Blink changes every 1.2 ms, so on a millisecond grain successive states
  would share a returned instant. On a microsecond grain every cycle down to
  level 18 (1.7 µs) is longer than the grain, so `start < end` and "the first
  instant of the new state" is a distinct value for everything a display can
  show.
- **Device clocks are microsecond clocks** (`gettimeofday`, `esp_timer`,
  `Date.now() * 1000`): no conversion on the hot path.
- **It fits.** `int64_t` microseconds cover ±292,271 years around 1970.
  `frc_catalogue_init` refuses a table whose instants would not
  (`|ms| > INT64_MAX / 1000`).

Levels 19..21 have cycles *shorter* than a microsecond (345 ns .. 9.9 ns). They
stay exact as indices and phases; only their boundary instants collide, which
`frc_cycle_t` documents.

## 2. Position: ordinal + num/den

SPEC §2.2: `position(t) = k + (t − E[k]) / I_k` for `E[k] ≤ t < E[k+1]`.

The core never forms that number. `frc_position_t` keeps the three integers

```
ordinal = k        num = t − E[k]        den = I_k        0 ≤ num < den
```

found by binary search over the table (`frc_position_at`). Nothing is rounded
on the way in. Subtractions are done in `uint64_t`, so an interval wider than
`INT64_MAX` in a hand-built table still cannot overflow.

**The closing instant.** `t = E[n]` would give `num == den`. It is read as one
microsecond earlier (`num = den − 1`, ordinal `n − 1`), so `0 ≤ num < den`
holds for every position and the last cycle is clipped *at* `E[n]` (SPEC §2.2,
pitfall 7). Functions that return a "next" instant remember that the reading
was taken there (`frc_i_fraction_t.closing`) and answer `FRC_TIME_NONE`.

This split is pitfall 4 (SPEC §11) taken to its end: the whole position is
never multiplied by a large `P(L)`; only the proper fraction is.

## 3. The cycle fraction: one primitive for every reading

Every reading of the clock is the same exact number seen through a different
grid (`frc_i_fraction`, `src/frc_cycle.c`):

```
position · P  =  total + rem / den          0 ≤ rem < den

x     = num · P              128-bit product
ip    = floor(x / den)       < P because num < den
rem   = x mod den
total = k · P + ip           = floor(position · P)
```

One cycle of level `L` spans `4 / P` Saros, so `position · P` counts **quarter
cycles** since position 0:

```
cycle   = total / 4          SPEC §3.2
quarter = total mod 4        0 peak, 1 node, 2 valley, 3 end
phase   = (quarter + rem/den) / 4
```

and the boundaries of the level (cycle starts and the three thresholds) are
exactly the integer values of `position · P`.

A finer grid of `w` states per quarter needs no new arithmetic, because for an
integer `q4` the floor distributes:

```
floor((q4 + rem/den) · w) = q4 · w + floor(rem · w / den)
```

| Reading | Grid `w` | Function |
|---|---|---|
| phase, 0.32 fixed point | 2³⁰ (`q4` lands in the top two bits) | `frc_i_fraction_phase` |
| glyph index (SPEC §4.1) | `M` | `frc_glyph_at` |
| odometer step (SPEC §5) | `M · q^(k−1)` = `q^k / 4` | `frc_odometer_at` |
| date fields (SPEC §15.3) | level 12, then 64-bit division by small radices | `frc_date_at` |

Overflow bounds:

- `num · P`: `num < 2⁶⁴`, `P < 2⁵⁸`: a 128-bit product (`frc_i_mul64`).
- `ip < P`, so the 128/64 division's quotient fits 64 bits (the contract of
  `frc_i_div128` is exactly `n.hi < d`).
- `total < (k + 1) · P ≤ n · P`. `frc_i_extent` refuses a catalogue for which
  `n · P + 3 ≥ 2⁶⁶` (`FRC_ERR_OVERFLOW`), so `total < 2⁶⁶` and `cycle =
  total / 4` fits 64 bits. That limit is 320 eclipses at level 21 and 2238 at
  level 20; Saros 141 has 70. `total` itself is kept in 128 bits because
  `n · P(21)` passes 2⁶⁴ from 80 eclipses on.
- `rem · w`: `rem < den`, so `floor(rem · w / den) < w`; with `w < 2⁶²` the
  step `q4 · w + …  < 4w` fits 64 bits. The largest `w` in use is
  `484⁴ / 4 < 2³⁴`.

`FRC_CLOCK_MAX_LEVEL = 21` is chosen by these bounds: it is the deepest level
whose cycle indices fit 64 bits on any real series, while `P(22)` is the last
prefix that fits at all.

## 4. 128-bit helpers

The targets (xtensa, riscv32, armv7-m, wasm32) have no `__int128`, and a
compiler that does have it turns `a * b` into a call to `__multi3`, which a
freestanding link does not provide. `src/frc_math.c` builds the few operations
needed from two `uint64_t`:

| Helper | Exactness | Contract |
|---|---|---|
| `frc_i_mul64(a, b, &out)` | exact 64×64→128, schoolbook on 32-bit digits | none |
| `frc_i_mul64_fits(a, b, &product)` | 1 and the product when `a·b < 2⁶⁴`, else 0 | none |
| `frc_i_pow_u64(base, n)` | `baseⁿ`, or 0 when it does not fit 64 bits | none |
| `frc_i_add128(&a, &b)`, `_add128_64`, `_sub128`, `_cmp128` | exact mod 2¹²⁸, in place (`a += b`) | callers keep sums below 2¹²⁸ |
| `frc_i_div128(&n, d, &rem)` | exact floor and remainder (Knuth D, two digits) | `d ≠ 0` and `n.hi < d`, i.e. the quotient fits 64 bits |
| `frc_i_div128_ceil` | exact ceiling | same, and the ceiling must fit |
| `frc_i_muldiv_floor(a, b, c, &rem)` / `_ceil` | `floor` / `ceil` of `a·b/c` | `a·b/c < 2⁶⁴`; the usual proof is "one factor is `< c`" |
| `frc_i_floor_div(a, b)` | signed floor division | `b > 0` |

Outside its contract `frc_i_div128` saturates to `UINT64_MAX` instead of
invoking undefined behaviour, but no call site relies on that: each states its
bound in a comment next to the call.

Two traps are worth naming.

An overflow guard written `a > UINT64_MAX / b` is rewritten by clang into a
128-bit multiplication, i.e. `__multi3` on wasm32. Use `frc_i_mul64_fits`.

A 128-bit value **always travels by address**: results are written through a
pointer and the sums work in place. Passing the 16-byte `frc_i_u128_t` by
value makes the caller copy it with a call to `memcpy` on riscv32
(gcc `-Os`, the ESP32-C3/C6) and on ARMv6-M (Cortex-M0), and returning or
assigning one does the same on Xtensa at `-O0`: 78 call sites in the clock
path before this rule. The same holds for every struct of 16 bytes or more
inside the core (`frc_interval_t`, `frc_speed_t`, `frc_arc_t` …): the public
functions that take or return one by value hand its address on at once and
fill their result field by field. `make check-cross` builds each target at
`-O0`, `-Og`, `-Os`, `-O2` and `-O3` and fails on any reference to `memcpy`,
`memset` or another libc symbol.

## 5. Boundary instants: why ceilings

A boundary is the rational instant where `position · P` reaches an integer
`T`. With `k = T div P` and `r = T mod P` it is

```
position = k + r/P          instant = E[k] + r · I_k / P
```

The API returns **the first microsecond at or after it**:

```
frc_i_instant:  E[k] + ceil(r · I_k / P)
```

Why the ceiling and not floor or nearest:

- Reading the clock at the returned instant `c = ceil(x)` gives
  `position(c) ≥ position(x)`, hence `floor(position · P) ≥ T`: **the new
  state is showing**. At `c − 1 < x` the old one still is. So the returned
  value is precisely "the first microsecond of the new state".
- With a floor, a reading at the returned instant could still show the old
  state; "sleep until `next_change`, read again" would then return the same
  instant forever. With the ceiling that loop always advances, which is the
  promise in `fractonica.h` ("BOUNDARY INSTANTS").
- Intervals come out half-open and tiling: `end` of cycle `c` is `start` of
  `c + 1`, computed by the same expression. (The one closed end is the
  catalogue's closing instant, which belongs to the last cycle: a reading
  taken exactly there has `t == end`.)

Bounds: `r < P` so `ceil(r · I_k / P) ≤ I_k`: the quotient fits 64 bits and
the sum cannot pass `E[k+1]`. `T ≤ n · P` is checked first
(`frc_i_time_at_total`); beyond it the answer is `FRC_TIME_NONE`, never an
extrapolation.

A "next" instant is the boundary `total + 1`, strictly after the query
position, so its ceiling is strictly after the query instant. The one
exception is the closing instant (section 2), which answers `FRC_TIME_NONE`.

## 6. The two-stage ceiling

An odometer state ends at a boundary of a *finer* grid: `position · P = total
+ a/b` with a rational offset (`a/b = (step + 1) / w`). Folding `b` into the
denominator would need `P · b`, up to 2⁵⁸ · 2³⁴: too wide. Instead
(`frc_i_time_after_total`):

```
N = r · I_k                  integer, 128-bit
y = a · I_k / b              real, 0 < y < I_k   (a < b after normalising)

instant − E[k] = ceil((N + y) / P) = ceil((N + ceil(y)) / P)
```

The identity holds because `N` and `P` are integers: for an integer `m`,
`m·P ≥ N + y  ⇔  m·P − N ≥ y  ⇔  m·P − N ≥ ceil(y)`. Two chained exact
ceilings therefore give the true ceiling, with no rounding error.

Bounds: `ceil(y) ≤ I_k` (`a < b`); `N + ceil(y) ≤ (r + 1) · I_k ≤ P · I_k`, so
the final quotient is `≤ I_k` and fits 64 bits.

The solar sync uses the same identity (`span_instant` in `src/frc_sync.c`).

## 7. The solar sync: exact in scaled time

A sync is 19 Helions (SPEC §7.2). Its *phase* is the one reading that is
linear in **time**, not in position: `(t − S) / (E − S)` on the exact rational
start `S` and end `E`. To keep that exact, `src/frc_sync.c` measures time from
`E[origin]` (the eclipse at or before the sync's start) in units of `1/P`
microsecond, `P = P(6)`. The boundary at quarter count `T = k·P + r` is then
the **integer**

```
scaled(T) = (E[k] − E[origin]) · P + r · I_k
```

and a query instant is `(t − E[origin]) · P`. Phase, quarter and odometer
digits are one integer over another:

```
phase  = floor(elapsed · 2³² / length)       elapsed < length  ⇒  < 2³²
digits = floor(elapsed · q^k / length)       not derived from the 32-bit phase
```

They come from the exact boundaries, never from the rounded `start` / `end`
fields (6 of 508 oracle readings would differ in the last bit otherwise).

Bounds: Helion quarter counts are below `2¹⁶ · 2¹⁷` (64-bit arithmetic);
`lead = r · I_origin` is 128-bit; `length = (E − S) · P` must fit 64 bits,
which holds unless a Saros interval exceeds about 7,000 years
(`FRC_ERR_OVERFLOW`, hand-built tables only). `nominal = E[anchor] + number ·
4 days` and the drift are range-checked for the same reason.

Sync numbers before the anchor are negative, so the division by 19 is a floor
division (`frc_i_floor_div`).

## 8. Dates and spans: the Blink axis

A date (SPEC §15.3) is one integer, the global Blink index `B = cycle(12)`,
written in the mixed radix of the unit ladder:

```
B = (((((((a·5 + b)·13 + g)·5 + d)·5 + e)·77 + h)·36 + i)·65 + l)·25 + n
```

`frc_date_at` makes one 128-bit operation (the level-12 cycle fraction) and
then divides by small radices in 64 bits. The quarters of Alpha..Delta come
from the same `total`: `floor(position · P(L)) = floor(T / (P(12) / P(L)))`
with `T = floor(position · P(12))`, because nested floors by an integer are
exact. `n · P(12) < 2¹⁶ · 2³³`, so everything here is 64-bit.

`frc_date_to_time` is the inverse through `frc_cycle_by_index`; the round trip
holds because a Blink's start is a ceiling (section 5).

A span (SPEC §15.4) is a difference of **position**, measured in Blinks so it
stays exact across eclipses whose intervals differ. Each end is `(quarters +
rem/den)` quarter Blinks, so the span is `quarters + n/m` quarter Blinks with
`m = den₁ · den₂` (128-bit) and `0 ≤ n < m`. The whole Blinks are
`quarters / 4`; the 0.32 fraction is `(quarters mod 4 + n/m) · 2³⁰`, whose
floor is exact because the first term is an integer: two bits from the
quarters and 30 bits of `n/m` by binary long division (`scale_floor`).
`frc_span_between` refuses an interval of 2⁶³ µs or more
(`FRC_ERR_OVERFLOW`; that is 292,000 years), so `n < m < 2¹²⁶` and doubling
`n` never wraps. The closing instant is taken at its true position
`count − 1` here, not one microsecond early: a span to the end of the
catalogue must not lose that microsecond.

**The time grain.** Section 5 rounds every boundary *up* to a microsecond, so
two returned instants stand for boundaries `b₁ ∈ (t₁ − 1 µs, t₁]` and
`b₂ ∈ (t₂ − 1 µs, t₂]`. Measured exactly, `t₂ − t₁` is then up to a
microsecond of position away from the true span `b₂ − b₁`: 3.2·10⁻⁶ Blink on
Saros 141, enough to turn one Preon (1625 Blinks) into "64λ 24ν". The true
span lies strictly inside

```
( S − µ₂ , S + µ₁ )      S = measured span,  µᵢ = P(12) / gᵢ quarter Blinks
```

where `gᵢ` is the length of the Saros interval that holds the microsecond
ending at `tᵢ` (for an instant exactly on an eclipse that is the interval
*before* it, which is why such an instant is written from that interval, as
its last quarter Blink complete: `rem = den`). When a whole number of
**quarter Blinks** lies inside that window, the span is that number
(`snap_to_grid`): over the common denominator the two tests are
`n < P(12) · den₁` and `m − n < P(12) · den₂`, products below 2⁹⁶. The
quarter-Blink grid holds every cycle start and threshold of levels 0..12 and
every sync, Pentad and date, so the span between two such instants is the
exact count of units. A span of exactly one microsecond is on the edge of the
window, not inside it: no span between two different instants becomes zero.
This is the core's counterpart of the reference's `EPS_POS` (10⁻¹² Saros added
before counting), 570 times finer: a span one *millisecond* short of a unit
stays short.

**The fraction and the texts.** `frc_format_span` rounds half up at decimals
whose ties and thresholds are all multiples of 1/20000 Blink. A fraction
floored to 2⁻³² can fall just below such a mark that the exact value is on
(1.05 Blinks would read "1ν" instead of "1.1ν"). So `frc_i_blink_fraction` also
computes `floor(fraction · 20000)`, the last mark at or below the exact
value, and when that mark is above the floored fraction it stores the next
2⁻³² up. Marks are 215,000 units apart, so the stored fraction is on the
exact value's side of *every* mark, and the text rounded from `frc_span_t`
is the text of the exact span. The fraction is otherwise the plain floor (a
grain of 72 picoseconds).

**A span in one unit.** `frc_format_span_in_unit` counts whole units of one
unit, `floor(blinks / unit length)`: it is *cut* toward zero, never rounded
(a drift of −6 236.9 Flavons reads "−6 236λ"), and the fraction of a Blink
never counts.

**When the text of a span changes.** A countdown's text (`frc_span_next_change`)
and the text of a span since an instant (`frc_span_next_change_since`) are step
functions of the measured span: every text is written for one interval of
spans `[low, high)` (the class of the text, `span_class_of`), and both ends
are whole numbers of marks (1/20000 Blink): a whole count of the finest unit
shown, or a tie or threshold of the decimal. Because the stored fraction is on
the exact value's side of every mark and the time grain only ever moves a span
to a whole quarter Blink less than a microsecond away, the measured span, and
so the text, is monotone in `now`. The search is then:

1. the class of the text at `t`, and its edge: `low` for a shrinking span,
   `high` for a growing one (`high` of a decimal is half a last digit of the
   decade of the value *shown*: "0.1" runs to 0.105; of the parts it is the
   next whole finest unit);
2. **the guess**: the instant where the exact span crosses that edge,
   `Q ∓ 4 · edge` quarter Blinks from the anchor's point `Q` (blink_point_t),
   the fraction taken to the nearest `1/den` below and timed by
   `frc_i_time_after_total` (`instant_of_edge`);
3. the formatter's own class is asked four microseconds before and three
   after the guess; when they bracket the change, a bisection over at most
   seven microseconds finishes (about three span evaluations in all);
4. otherwise a bisection over the whole interval, `(t, boundary]` or
   `(t, closing instant]`, still finds the first instant whose text differs
   (a monotone predicate), only slower (up to about 60 evaluations).

The answer is always the formatter's; the arithmetic only makes it fast.
`tests/test_span.c` holds the guess itself within three microseconds of the
answer (`frc_i_span_change_guess`), on Saros 141 and on a table where every
mark is an instant, so a wrong guess cannot hide behind the bisection.

The deepest call chain of the library goes through this search:
`frc_span_next_change` → `same_text_at` → `frc_span_between` →
`blink_point_at` → `frc_i_fraction_at` → … → `frc_i_div128`, about 900 bytes
of stack on Xtensa (`-Os`; see README.md for the measured figure). Size a
task's stack for that, not for the largest single frame.

## 9. The big-integer ladder

The spectral ladder (SPEC §16) is exact for all 63 levels, and `P(62)` alone
has 172 bits. `src/frc_ladder.c` computes on `frc_i_big_t` (`src/frc_big.h`):
384 bits in twelve 32-bit limbs on the caller's stack, unsigned, modulo 2³⁸⁴,
with no overflow report. The caller proves the bounds.

For an interval `num/den` milliseconds:

```
T(L) = 4·num / (1000·den·P(L))  s  = top / bottom
f(L) = bottom / top              Hz
length(L) = v_num · top / (v_den · bottom)  m
```

`P(L)` is built as `P(8)^a · P(b)` for `L = 8a + b` (the factor sequence
repeats every 8 levels), from the 64-bit table.

| Step | Largest value | Bound |
|---|---|---|
| `top = 4 · num`, times a speed numerator or a device rate | `2² · 2⁶⁴ · 2⁶⁴` | < 2¹³⁰ |
| `bottom = 1000 · den · P(62)`, times a speed denominator | `2¹⁰ · 2⁶⁴ · 2¹⁷² · 2⁶⁴` | < 2³¹⁰ |
| normalise to `1 ≤ n/d < 10` (multiply the smaller side by tens) | `n < 10 · 2³¹⁰` | < 2³¹⁴ |
| 17 significant digits: `n · 10¹⁶` | | < 2³⁶⁸ |
| civil units: `d · 31,557,600` (the Julian year), then `n · 10⁸` | | < 2³⁶⁶ |
| a double: shift the smaller side until the quotient has 56 bits | | < 2³⁶⁶ |
| a phase step: `bottom` without a speed (< 2²⁴⁶) times `2^bits`, `bits ≤ 64` | | < 2³¹⁰ |

Every result is rounded **once** from the exact quotient:

- nothing walks down the ladder rung by rung, and nothing is computed in
  floating point;
- `frc_sci_t` is the quotient to 17 significant digits; a carry (9.99… →
  10.0…) is folded back so the mantissa always has exactly 17 digits;
- the functions that return a `double` (only without `FRC_NO_FLOAT`) take a
  55/56-bit integer quotient plus a sticky bit, round it to 53 bits themselves,
  and then only scale by powers of two, which is exact. No second rounding,
  and nothing depends on the byte layout of a double;
- bands compare `bottom ≥ top · edge` in integers, so a frequency exactly on
  an edge belongs to the band that starts there.

The big integer has no copy and no clear on purpose: a loop that copies or
zero-fills 48 bytes is rewritten by optimisers into `memcpy` / `memset`, which
a freestanding build does not have (`frc_big.h` explains the idiom).

Beyond 64 bits the prefixes are also needed as text (`frc_format_prefix`):
`frc_i_prefix_limbs` builds `P(0..63)` in at most six base-10⁹ limbs; each step
multiplies by a factor ≤ 13, so `limb · factor + carry < 2³⁴`.

## 10. Pure wordings: one ratio, walked along the ladder

`src/frc_pure.c` words static facts of a level without an Earth unit. All of
them are ratios of prefixes, and every prefix divides the next, so nothing
needs the big integer:

- **A level in a unit** is `P(unit level) / P(level)`, the quotient of two
  lengths in Blinks, at most `P(12)` (33 bits).
- **A rate** below the Blink is the whole number `N = P(level) / P(12)`. It
  passes 64 bits at level 36 and has 42 digits at level 62, so it is built in
  base-10⁹ limbs (`frc_i_ratio_limbs`, the loop of `frc_i_prefix_limbs`
  started at level 12: `limb · factor + carry < 2³⁴`). In that base, digit
  groups of three and a rounding to a few significant digits are digit work:
  `frc_i_limbs_round` takes the first `digits` digits, the next one and
  "anything non-zero after it", and rounds half to even. Down to the Blink
  the rate is `1 / N` and the text is `frc_format_per_blink`'s own.
- **A light span** is `B = P(12) / P(level) · v / c` Blinks for a speed
  `v = num/den`. For a level below the Blink `P(level) / P(12)` has up to 140
  bits, so `B` is never built there. The function keeps one ratio
  `top / bottom` = the count of cycles of a rung `K`, starting where it is
  cheap,

  ```
  level ≤ 12:  K = 12,     top = (P(12)/P(level)) · num < 2⁹⁷,  bottom = den · c < 2⁹³
  level > 12:  K = level,  top = num < 2⁶⁴,                     bottom = den · c
  ```

  and walks the ladder one factor at a time: up (`bottom ·= S[K−1]`, `K−1`)
  while a whole cycle of the next coarser rung still fits, at most to the
  Blink; or down (`top ·= S[K]`, `K+1`) until one cycle fits, at most to level
  62. Going up, `bottom` grows only while it stays `≤ top`; going down, `top`
  grows only while it is `< bottom`, so `top < 13 · bottom`: both stay below
  2¹⁰¹, and the texts scale by at most 1000 more. Two 128-bit integers and a
  128-by-128 shift-and-subtract division are the whole machinery.

  The walk ends in one of three places. On the Blink with `top ≥ bottom`: the
  span is `floor(top / bottom)` Blinks (`FRC_ERR_OVERFLOW` from 2⁶⁴) and the
  rest is cut by `frc_i_blink_fraction`, the cut of section 8, so the text
  `frc_format_span` rounds from `frc_span_t` is the text of the exact value.
  On a rung `K ≥ 13` with `1 ≤ count < S[K−1]`: three significant digits,
  half to even; a count that rounds up to `S[K−1]` is one whole cycle of rung
  `K−1` and is written as that. Or below level 62 with `count < 1`: the count
  is brought to `[1, 10)` by tens (29 at most, as `top ≥ 1` and
  `bottom < 2⁹³`) and written as a power of ten.

The Saros interval never enters: a period and the Blink are the same multiple
of it, so their ratio, and with it every text of this section, is a fact of
the hierarchy.

## 11. Earth wording

`src/frc_earth.c` words civil fields; it knows nothing of the clock.

- **Civil text** (`frc_format_civil_text`) says the fields of `frc_civil_t` as
  they are: nothing is rounded, and the weekday is computed from the date
  (`frc_i_civil_weekday`, a day count modulo 7), never read. A form is a set
  of parts in a flag table in flash (a `switch` could become a jump table in
  RAM on AVR). Years outside 1..9999 have four digits and a sign; the minus is
  U+2212 except in the ISO date, which keeps ISO 8601's hyphen-minus.
- **A duration** (`frc_format_duration`) is the one wording that rounds, and
  it rounds **once**: the magnitude, half up, to a whole number of the finest
  unit the text will show (hundredths of a second below 9.995 s). The parts
  are then counted from that rounded value, so a carry happens before the
  wording ("1 min", never "60 s"). When the carry makes a coarser unit lead,
  everything below it is zero, so the rounding is also the rounding to the
  new finest unit. All of it is 64-bit: `count · unit ≤ 2⁶³ / 10⁶ + one day`.
- **The parser** (`frc_parse_civil`) is a hand-written scanner over the
  grammar the header states, with every number read by a bounded run of
  digits (a longer run is refused, not cut, so no value wraps). It never
  reads past the NUL: each look-ahead stops at the first byte that does not
  match, and the NUL matches nothing. The shape is checked first, then the
  range (`frc_civil_validate`, the offset under a day), then the weekday; the
  outputs are written only at the end, on `FRC_OK`.

## 12. Rounding rules, in one table

| Result | Rule | Where |
|---|---|---|
| cycle index, quarter, glyph index, odometer digits, date fields | floor (exact) | `frc_cycle.c`, `frc_glyph.c`, `frc_odometer.c`, `frc_notation.c` |
| phase (`frc_frac_t`) | floor of the exact value · 2³² | `frc_cycle.c`, `frc_sync.c` |
| span between two instants | a whole number of quarter Blinks when within one microsecond of it (the time grain); the fraction is the floor · 2³², one unit more when a rounding mark of the text lies in that last unit | `frc_span.c` |
| every returned instant | ceiling to the microsecond, once | `frc_catalogue.c`, `frc_cycle.c`, `frc_sync.c` |
| the instant a span's text changes | none of its own: the first microsecond whose measured text differs, found by search (section 8) | `frc_span.c` |
| "next" instants | strictly after the query; `FRC_TIME_NONE` at the closing instant | same |
| span and per-Blink texts (SPEC §15.4, §15.5) | half up; a measured span reads as its exact rational does (the reference rounds doubles) | `frc_span.c` |
| a span in one unit (`frc_format_span_in_unit`) | cut toward zero to whole units | `frc_span.c` |
| sub-Blink decimals of a cycle name | two decimals, half up | `frc_notation.c` |
| `frc_sci_t`, ladder texts, ladder doubles | half to even, once, from the exact quotient | `frc_ladder.c` |
| device ticks and phase steps (SPEC §16.4) | half up | `frc_ladder.c` |
| pure wordings: a rate to `digits` digits, the three digits of a light span below the Blink | half to even, once, from the exact whole number or ratio | `frc_pure.c` |
| pure wordings: a light span from one Blink up, a rate down to the Blink | the span and per-Blink rules above (half up), on the exact value | `frc_pure.c` through `frc_span.c` |
| civil day numbers, sync numbers | floor division (never C truncation) | `frc_civil.c`, `frc_sync.c` |
| civil milliseconds in ISO text | cut, not rounded | `frc_civil.c` |
| civil texts of `frc_format_civil_text`; a parsed fraction of a second | not rounded (the fields as they are); the fraction cut to the microsecond | `frc_earth.c` |
| Earth durations | the magnitude, half up, once, to the finest unit shown; then counted | `frc_earth.c` |

Where the JavaScript reference words a value from a double (and adds 10⁻¹²
Saros, 0.57 ms, before counting span units), the core is exact instead (its
span grain is one microsecond, section 8); the oracle
generator carries the exact rule and lists the 52 texts that differ
(`tools/oracle/README.md`, "Exactness wins").

## 13. Floating point

The clock path has none. `float` / `double` appear only inside
`#ifndef FRC_NO_FLOAT`:

- convenience conversions (`frc_position_to_double`, `FRC_FRAC_TO_DOUBLE`,
  `frc_sci_to_double`, `frc_period_seconds` …), each rounded once from exact
  integers as described above;
- the arc geometry (`src/frc_arc.c`), single precision only. `frc_sin` /
  `frc_cos` reduce the argument with integers against 192 bits of `1/(2π)`, so
  any finite `float` is within 10⁻⁶ of the true value; no libm.

`make check-nofloat` builds and tests the library with `FRC_NO_FLOAT`; no
reading changes.

## 14. What proves it

| Claim | Proof |
|---|---|
| exact readings | `make test`: oracle vectors generated from the BigInt reference, SPEC §12 / §15.6 / §16.5 typed in by hand |
| "next" instants are the first microsecond of the new state; cycles, syncs and Pentads tile | `tests/test_properties.c`, 72 million checks on two catalogues |
| 128-bit helpers | `tests/test_foundation.c` against the compiler's `__int128` where the host has one |
| big integer | `tests/test_ladder.c` against slow reference arithmetic; extreme intervals and speeds |
| no overflow, no undefined behaviour | `make check-sanitize` (ASan + UBSan) |
| no libc, no libm, no compiler 128-bit runtime | `make check-symbols`, `make check-freestanding` (the linked wasm32 module imports nothing), `make check-cross` (Cortex-M3 and M0, ESP32 Xtensa and RISC-V, wasm32, AVR; `-O0` … `-O3`; no `memcpy` / `memset` call anywhere) |
| every flash table is read through `FRC_PGM_*` | `make check-progmem` (reads the source, then proves on planted direct reads that it catches them); `make check-harvard` runs the tests through the accessor code path; `make check-cross` proves on AVR that the tables are in `.progmem.data` and that an overridden `FRC_PROGMEM` cannot be read as RAM |
| spans between returned instants are whole units; ties read from the exact span | `tests/test_span.c` "time grain", "exact rounding" |
| next-change instants are the first microsecond of the new text; the guess is within 3 µs | `tests/test_span.c`: every microsecond of several windows, chains to thresholds, from far away, and the guess itself |
| Earth wordings | `tests/test_earth.c`: every day of 2 000 years, random fields and offsets against rules restated with libc, every grammar edge of the parser, a fuzz run on exact-length heap copies (a read past the NUL fails under `make check-sanitize`) |
| pure wordings: every level, medium and precision; every tie and carry of a light span; the bounds of section 10 at the largest and smallest speeds | `tests/test_pure.c` against `oracle_pure.h` (BigInt rationals, no call into the core) |
| 32-bit `int` / `size_t` | `make check-32`, `make test-wasm32` |
| no RAM | `make size` fails unless `.data` and `.bss` are 0 |
