# Fractonica Clock Specification

Version 2.2.0. Reference implementation: `fractonica.js` 2.0.0 (§1–§14); the time units and notation of §15 are implemented by the web client in `src/core/pure.js`; the spectral ladder and spatial units of §16 by the library's nominal scales (`F.period`, `F.frequency`, `F.length`, `F.media`) laid from an Alpha of four Saros.

This document specifies an eclipse-anchored harmonic clock and its glyph display, independent of platform. It has everything needed to build a client (iOS, watchOS, web, embedded) that produces the same readings as the reference library and the Zepp watch face. Section 12 lists test vectors to check an implementation against.

Contents:

1. Concepts
2. Time base
3. Harmonic hierarchy
4. Phase glyphs
5. Odometer read-out
6. Counters and thresholds
7. Solar sync
8. Display model
9. Rendering glyphs
10. Interaction and refresh
11. Numeric implementation
12. Test vectors
13. Pitfalls
14. Reference data
15. Time units and notation
16. Spectral ladder and spatial units

## 1. Concepts

- **Catalogue**: the greatest-eclipse instants of one Saros series, such as Solar Saros 141. Consecutive eclipses are one Saros apart, about 18.03 years.
- **Position**: a continuous coordinate in Saros units. The first catalogue eclipse is position 0, the next is 1, and so on. Between eclipses, position is linear in time.
- **Level**: a level of the harmonic hierarchy, zero-based. **Depth** = level + 1 is the user-facing number. Level 0 (depth 1) cycles span 4 Saros.
- **Cycle**: one period at a given level. Each cycle is divided into the states of a **phase glyph**.
- **Glyph**: a balanced-ternary drawing of a phase state. It has one or more **strokes** (trits) stacked on a vertical stem.
- **Odometer**: two or more glyphs side by side. The left glyph is the coarse phase, and each glyph to its right runs a full cycle inside one state of its left neighbour.
- **Solar sync**: a run of N cycles at one level that nearly equals a whole number of days. The run is compared with real days, and the slow mismatch accumulates as **drift**.
- **Major and minor periods**: depths 1–4 (α β γ δ, Alpha to Delta) are the **major** (ultrahelion) periods, said in words. Below them the **minor** (infrahelion) units Nuclide, Helion, Preon, Flavon and Blink take every other depth (5, 7, 9, 11, 13) and are said in letters and counts. §15 defines the notation.

## 2. Time base

### 2.1 Time scale: Unix seconds

Clients work in **Unix seconds**: POSIX time, UTC-based, with every day exactly 86,400 s. The catalogue ships precomputed in Unix seconds (§14). A client feeds the device clock straight in, `t = Date.now() / 1000` or `Date().timeIntervalSince1970`, and needs no TT conversion or leap-second table.

NASA publishes the instants in **TT** (Terrestrial Time; NASA labels them TD). The Unix table is derived once, offline, with the reference library's civil model:

| Era | Conversion | Status |
|---|---|---|
| 1972 – 2027-06-28 | `UTC = TT − (TAI − UTC) − 32.184 s`, using the published leap-second table | exact |
| before 1972 | UT1 estimate `TT − ΔT`, from the NASA ΔT polynomials | approximate (seconds to minutes, growing with age) |
| after the table | TAI − UTC held at 37 s: Resolution 5 of the 28th CGPM (2026) makes UTC continuous from 20 May 2027, with UT1 − UTC allowed to reach 3,600 s either way, and as of November 2026 the IERS has announced no leap second before that date (Bulletin C 72) | by decision, for as long as the resolution stands (it says several centuries); a leap second introduced all the same would shift later values by 1 s each |

Values are rounded to milliseconds. The library generates and exposes them as `F.defaults.unixTimestamps`, and `F.create({timeScale: 'unix'})` builds a clock on them. Regenerate the table if a new leap second is announced.

Properties of the Unix time base:

- **Days are civil days.** 86,400 Unix seconds is one UTC day, so the solar sync (§7) is measured against the civil clock, leap seconds included.
- **Leap seconds sit inside intervals.** A Saros interval in Unix seconds is shorter than in TT by the leap seconds inserted inside it: 14 s in 1973–1992 and 3 s in 2010–2028. Readings between eclipses therefore differ from a TT-interpolated clock by less than that count (under 3 s since 2010, 0.22 s in October 2026), and agree exactly at every eclipse.
- **TT clock.** For scientific work the library also provides a TT clock: `F.create()` (TT is its default scale), with `F.fromUTC` for input.

Never mix the two: a Unix catalogue takes Unix query times, and a TT catalogue takes TT query times.

### 2.2 Position from time

The catalogue holds instants `E[0] < E[1] < … < E[n]` with ordinals `0, 1, …, n`. Ordinals are consecutive unless the catalogue skips recurrences.

```
position(t) = k + (t − E[k]) / (E[k+1] − E[k])     where E[k] ≤ t < E[k+1]
time(position) = E[k] + (position − k) · (E[k+1] − E[k])      k = floor(position)
```

This mapping is piecewise linear and exactly invertible. Each Saros interval keeps its real, slightly different length. **Never substitute a mean Saros length**: real intervals are up to 1.2 h longer than the mean and up to 1.5 h shorter, which changes every derived period and drift (see §7.4).

`t` and `E[k]` are Unix seconds (§2.1). Outside `[E[0], E[n]]` the clock is undefined. Clients show nothing, or an explicit "out of catalogue" state; they must not extrapolate. The last partial cycle at each level is clipped at `E[n]`.

### 2.3 Catalogue data before 1582 (when building a table)

NASA lists dates before 1582-10-15 in the **Julian** calendar. Some data sources store those calendar fields as if they were Gregorian; `saros-eclipse` 0.1.0, from which the reference catalogue was first built, is one. Such timestamps are wrong by the calendar difference: 10 days near 1582, and one day less for each century before that. An interval spanning the reform appears 10 days too long, and intervals spanning a Julian-only leap day lose a day.

Fix: take the stored value's proleptic-Gregorian calendar fields, reinterpret them as a Julian date, and convert back. The library function is `F.correctJulianTimestamp(seconds)`.

```
days    = floor(s / 86400);  within = s − 86400·days
(y,m,d) = civilFromDays(days)            // proleptic Gregorian
JDN     = d + floor((153·m' + 2)/5) + 365·y' + floor(y'/4) − 32083
          where a = floor((14 − m)/12), y' = y + 4800 − a, m' = m + 12a − 3
fixed   = (JDN − 2440588)·86400 + within
```

After the fix, the mean of all 13,026 solar Saros intervals is 6585.3213 days, which agrees with the theoretical 6585.3211 days to 0.3 min. Note that the Saros 141 catalogue starts in 1613 and needs no correction. The reference catalogue of all 180 solar series has the correction in it already: its dates before the reform are counted as Julian, and correcting it again would be the same mistake the other way round.

## 3. Harmonic hierarchy

### 3.1 Factors and prefixes

The default factor sequence repeats:

```
S = [5, 13, 5, 5, 7, 11, 9, 4]
P(0) = 1,   P(L) = P(L−1) · S[(L−1) mod 8]
```

A level-L cycle spans `4 / P(L)` Saros units. Level L contains `S[L mod 8]` cycles of level L+1. A level-L cycle has `S[(L−1) mod 8]` **siblings**, meaning cycles of its own level inside one level L−1 parent.

### 3.2 Cycle index and local index

```
cycle(L)  = floor(position · P(L) / 4)
local(L)  = cycle(L) mod siblings(L)        // 0 = first period of the parent
```

### 3.3 Period lengths

These figures use the 2010–2028 Saros 141 interval. Lengths change slightly between intervals.

| Depth | Level | Name | Unit (§15) | P(L) | Siblings | Cycle length |
|---|---|---|---|---|---|---|
| 1 | 0 | α | Alpha | 1 | — | 72.12 y |
| 2 | 1 | β | Beta | 5 | 5 | 14.42 y |
| 3 | 2 | γ | Gamma | 65 | 13 | 1.11 y |
| 4 | 3 | δ | Delta | 325 | 5 | 81.050 d |
| 5 | 4 | ε | Nuclide | 1,625 | 5 | 16.210 d |
| 6 | 5 | ζ | — | 11,375 | 7 | 2.316 d |
| 7 | 6 | η | Helion | 125,125 | 11 | 5.052 h |
| 8 | 7 | θ | — | 1,126,125 | 9 | 33.683 min |
| 9 | 8 | ι | Preon | 4,504,500 | 4 | 8.421 min |
| 10 | 9 | κ | — | 22,522,500 | 5 | 1.684 min |
| 11 | 10 | λ | Flavon | 292,792,500 | 13 | 7.773 s |
| 12 | 11 | μ | — | 1,463,962,500 | 5 | 1.555 s |
| 13 | 12 | ν | Blink | 7,319,812,500 | 5 | 310.92 ms |
| 14 | 13 | ξ | — | 51,238,687,500 | 7 | 44.42 ms |

The hierarchy goes on below depth 14. §16 lists all 63 depths, down to the edge of the Planck time, as periods, frequencies and lengths.

### 3.4 Period names

Level L is named with Greek letter `L mod 24` of `αβγδεζηθικλμνξοπρστυφχψω`. After ω the letters repeat with a numeric suffix: level 24 is α1, level 49 is β2. The library function is `F.periodName(level)`. The time units built on these levels, and the names of individual cycles, are defined in §15.

## 4. Phase glyphs

### 4.1 States

A glyph with **d strokes** encodes balanced-ternary values with extreme

```
M = (3^d − 1) / 2          d = 2 → M = 4,  d = 3 → M = 13,  d = 4 → M = 40
```

One cycle has `4M` states, numbered 0 to 4M−1. For an ascending family (the default):

```
index(L, d) = floor(position · P(L) · M) mod 4M
```

A descending family adds 2M before the modulo.

Each state has a value and a derivative:

| Index range | Value | Derivative | Dot |
|---|---|---|---|
| 0 … M−1 | index | +1 (rising) | no |
| M | M | 0 (peak) | no |
| M+1 … 3M−1 | 2M − index | −1 (falling) | **yes** |
| 3M | −M | 0 (valley) | no |
| 3M+1 … 4M−1 | index − 4M | +1 (rising) | no |

Two strokes (16 states):

```
0  1  2  3  4  3• 2• 1• 0• -1• -2• -3• -4  -3  -2  -1
```

Three strokes (52 states):

```
0 … 13,  12• … -12•,  -13,  -12 … -1
```

**Dot convention:** the derivative dot marks the falling branch. Every repeated value then reads bare while rising and dotted while falling, and both extrema are bare.

### 4.2 Trits

A value v in −M…M has d balanced-ternary digits (trits) t ∈ {−1, 0, +1}:

```
v = Σ t_i · 3^i      (i = 0 is the least significant trit)
```

The library returns trit arrays most significant first.

## 5. Odometer read-out

A level-L odometer has k glyphs of d strokes each. With `q = 4M`:

```
step   = floor(position · P(L) · M · q^(k−1)) mod q^k
digit_j = floor(step / q^(k−1−j)) mod q          j = 0 (left) … k−1 (right)
```

Digit 0 always equals the single-glyph index from §4.1. Digit j runs one full q-state cycle while digit j−1 holds one state. The finest digit next changes at `position = (floor(position·P(L)·M·q^(k−1)) + 1) / (P(L)·M·q^(k−1))`. Schedule screen updates from that time rather than polling. The library function is `clock.phaseDigits(position, level, {strokes, digits})`, which returns `next` and `nextTime`.

At depth 7 with 2 two-stroke glyphs, the major glyph changes every 18.9 min and the sub glyph every 71 s. At depth 8 they change every 2.1 min and 7.9 s.

## 6. Counters and thresholds

### 6.1 Sibling counter

Show `siblings(L)` markers, one per period of the parent. Light the markers 0 … local(L), meaning all periods up to and including the current one. The library function is `clock.counterAt(position, level)`.

### 6.2 Thresholds

A cycle has four quarter boundaries. For an ascending family they are:

| Quarter end | Name | Glyph state reached |
|---|---|---|
| 1/4 | peak | index M (value +M) |
| 2/4 | node | index 2M (value 0, falling) |
| 3/4 | valley | index 3M (value −M) |
| 4/4 | end | next cycle, index 0 |

A descending family reaches its valley first. The next threshold is the first boundary strictly after `position`; its time is `time(boundary)`. The library function is `clock.nextThreshold(position, level)`, which returns `{name, position, time}`.

## 7. Solar sync

### 7.1 Definition

Choose a level L, a cycle count N and a day count D such that `N · cycle(L) ≈ D days`. Choose an anchor ordinal A whose eclipse starts a level-L cycle; `A · P(L)` must be divisible by 4.

```
anchorCycle = A · P(L) / 4
sync        = floor((cycle(L) − anchorCycle) / N)
syncStart   = time((anchorCycle + N·sync) · 4 / P(L))
syncEnd     = time((anchorCycle + N·(sync+1)) · 4 / P(L))
phase       = (t − syncStart) / (syncEnd − syncStart)        // time-linear, 0 ≤ phase < 1
nominal     = E[A] + sync · D · 86400
drift       = syncStart − nominal                            // accumulated seconds against civil days
```

Sync boundaries coincide exactly with level-L cycle boundaries. Each sync's own offset is `N · cycle(L) − D · 86400`, which depends on the Saros interval the sync falls in.

The library function is `clock.solarSync({anchor, level, cycles, days, daySeconds})`. Its `.at(t)` returns sync, times, phase, drift and the next quarter threshold; `.digits(t, {strokes, digits})` returns odometer digits. `t` is in the clock's time scale, Unix seconds for a Unix clock.

### 7.2 Reference configuration (Saros 141)

- **Anchor:** A = 20, the first eclipse of Alpha 6, at Unix 125593319.816 (1973-12-24 15:01:59.816 UTC; 15:02:44 TT).
- **Period:** L = 6 (depth 7, η), N = 19, D = 4. The relation 19 × 5.052 h ≈ 4 days.
- **Per-sync offset (Unix seconds):** −10.0309 s (1973–1992), −10.0582 s (1992–2010), −10.0807 s (2010–2028), −10.2204 s (2028–2046). The depth-7 grid runs slightly short of civil days, so each sync lands about 10 s earlier. In TT the same offsets are −10.0224, −10.0534, −10.0789 and −10.2204 s; the difference is the leap seconds inside each interval.
- **Equivalent statements:** 171 depth-8 ticks ≈ 4 days, and 171 depth-9 ticks ≈ 1 day. Since 171 · T9 = 19 · T7 / 4 exactly, these are the same drift, about −2.52 s per day.
- **Lap:** the accumulated drift first passes −86,400 s at the sync of 2067-02-01 15:01:58 UTC, 93.11 years after the anchor. At that point the sync time of day has gone once around the clock back to the anchor's 15:02 UTC.

**Other series.** Any Saros series can carry the clock in place of 141; its eclipses become the catalogue (§14) and the
sync keeps L = 6, N = 19, D = 4. The native apps anchor it at the series' first Alpha in the UTC era: the first ordinal
A ≡ 0 (mod 4) whose eclipse falls on or after 1972-01-01 (for 141 that is A = 20, as above; for 145, A = 20, 1999-08-11).
The per-sync offset and the lap differ by series (§7.4): among the 40 series running in 2026 (117 … 156) the offset runs
from −8.34 s (152) to −13.42 s (147) and the lap, walking each series' intervals from 2026, from 70.0 to 113.7 years,
89.3 on average — an Alpha (4 Saros, 72.1 years) and about 17 years more.

### 7.3 Display of the sync

- **Odometer:** show the phase as k glyphs of d strokes, `digits = floor(phase · q^k)` split as in §5.
  - The reference uses 3 two-stroke glyphs: 6 h, 22.5 min and 84 s per state, with 4 left states per day.
  - The alternative is 2 three-stroke glyphs: 1.85 h and 2.1 min per state.
- **Thresholds:** quarters of the sync period, at `syncStart + q/4 · (syncEnd − syncStart)`. They fall about one day apart and are named peak, node, valley and end.
- **Sync time:** the local clock time of `syncStart`, read directly from the Unix value. It moves about 10 s earlier per sync and completes one lap in about 93 years.

### 7.4 Why actual intervals matter

These statistics use the TT intervals of every series. The mean Saros (6585.3211 d) gives −10.77 s per sync and a lap of 87.86 years, which differs visibly from the catalogue values above. Across all solar and lunar series, real intervals range from 6585.2595 d to 6585.3712 d. That puts the per-sync drift between −14.0 s and −8.1 s, and the lap between 68 and 116 years. Where an eclipse falls in Earth's orbit explains about 71% of the variation: intervals starting near perihelion are about 46 min longer, and those near aphelion are about 44 min shorter. No real interval comes close to the 6585.5263 d needed for zero drift, so this drift never runs forward.

### 7.5 Pentads

The Helion is the clock's link to the Sun. A solar day is 4¾ Helions (4.75 × 5.052 h ≈ 23.997 h), so the relation runs
both ways:

- **From the Sun:** when the Sun returns to the same place in the sky a day later, the Helion grid has moved a quarter
  Helion back. The Helion's quarter phase falls at the same time of day every day, and after 4 days (19 Helions, one
  solar sync) the whole grid repeats, a few seconds earlier (§7.2).
- **From the Helion:** after 5 Helions the Sun is a quarter Helion (≈ 1.26 h) later in its day. Three such runs drift
  ¾ Helion; the fourth run is one Helion shorter, which brings the Sun back.

A **Pentad** is one of those runs. Each solar sync (§7.1, anchor A = 20, N = 19 Helions) is divided into four, on whole
Helions, the way a week names its days:

| Index | Name | Short | Helions of the sync | Length | Starts after the sync's start |
|---|---|---|---|---|---|
| 0 | Pentad I | ⬠I | 0 – 4 | 5η ≈ 25.26 h | 0 |
| 1 | Pentad II | ⬠II | 5 – 9 | 5η ≈ 25.26 h | 1 day + ¼η |
| 2 | Pentad III | ⬠III | 10 – 14 | 5η ≈ 25.26 h | 2 days + ½η |
| 3 | Hypopentad | ◻ | 15 – 18 | 4η ≈ 20.21 h | 3 days + ¾η |

So every Pentad is about a day: it begins ¼η later in the day than the one before, and the Hypopentad, one Helion short
(the leap year's mechanism run backwards: it takes a period out instead of adding one), lands the next Pentad I on the
time of day the sync started at.

```
k        = cycle(6) − A · P(6) / 4          Helions since the anchor's Helion
sync     = floor(k / 19)
within   = k − 19 · sync                    0 … 18
index    = min(3, floor(within / 5))
helion   = within − 5 · index + 1           the Helion's place in its Pentad, 1 … 5 (1 … 4 in the Hypopentad)
```

Pentads count whole Helions, so unlike the sync reading (§7.1) they are defined in the catalogue's first and last
partial syncs too (the first and last Pentads are cut at its edges). The short form draws the Helion count as a shape: a
pentagon for the five-Helion Pentads with their numeral, a square alone for the Hypopentad's four. Text that must stay
plain writes `P1`, `P2`, `P3`, `H`.

A date leads with its Pentad, as a civil date leads with the weekday (§15.3). Vector A (§12, 1791460800) is in
Pentad II (its 2nd Helion) of sync 4820; the anchor eclipse itself starts Pentad I of sync 0.

## 8. Display model

The elements below are platform-neutral. The reference watch face is one arrangement of them, on a 192 × 490 px pill-shaped screen.

| Element | Content | Notes |
|---|---|---|
| Period pair (×2) | Odometer of 2 two-stroke glyphs at levels L and L+1 | Longer period at the top. Default depth 7 over depth 8. Teal (#4FD1C5). |
| Sibling counter (×2) | `siblings(L)` dots, lit through `local(L)` | On an arc concentric with the rounded screen end (top arc for the top period). Spread symmetrically, read left to right. |
| Solar odometer | Sync-phase odometer from §7.3 | Between the period pairs. Yellow (#FFD60A). |
| Period letters | Greek names from §3.4 | Shown briefly after a depth change, or permanently on request. Placed in the gaps beside the solar odometer. |
| Info labels | Next threshold of the top period; next solar threshold; sync time | Shown on request for a few seconds; they replace the letters while visible. Local time with seconds, or `Mon D HH:MM` when the time falls on another day. |

Reference geometry for the watch face:

- Glyph stems are 80 px apart in period pairs and 64 px apart in the three-glyph solar odometer; a 192 px width fits no more.
- Counter arcs have radius `width/2 − 10` and an 11.5° step.
- Period pairs sit ±165 px from the screen centre, and letters and labels ±90 px.

## 9. Rendering glyphs

### 9.1 Geometry (design units)

All dimensions are in design units:

- Stem segment per trit: 3.
- Stroke reach: 2.
- Curve bow: 1.
- Zero bar width: 2.
- Ink thickness: 0.3.
- Caps and joins: round.

Trit i (least significant first) occupies the stem from `y = 3i` to `3i + 3`, with its stroke attached at `y = 3i`. The least significant trit is at the top.

| Trit | Path at junction y |
|---|---|
| 0 | `M -1 y L 1 y` (horizontal bar) |
| +1 | `M 0 y C 0 y 0 y+1 1 y+1 C 2 y+1 2 y 2 y` (curve down and right) |
| −1 | `M 0 y C 0 y 0 y−1 −1 y−1 C −2 y−1 −2 y −2 y` (curve up and left) |
| stem | `M 0 3i L 0 3i+3` for each trit |
| dot | circle at (0, −1.75), radius 0.3, filled, only on dotted states |

The bounds of a d-stroke glyph are `x ∈ [−2.15, 2.15]` and `y ∈ [−2.2, 3d + 0.15]`. Bounds depend only on d, never on the value. The library function `CBT.fromIndex(index, {depth: d}).toSVG(style)` produces exactly these paths.

### 9.2 Pixel-perfect raster

Choose an integer scale u (px per unit) such that `0.3 · u` is an odd integer; the reference uses u = 10, giving a 3 px stroke. Then place the origin on a pixel centre:

```
frame x from −2.65,  width 5.3  → 53 px; x = 0 at pixel column 26.5
frame y from −2.45,  height 3d + 3 → 90 px (d=2) / 120 px (d=3); y = 0 at row 24.5
```

Every stem and bar then sits on whole pixel columns or rows with no anti-aliasing; only curves and the dot are smoothed. Draw at exactly 1:1 and never scale at runtime. In the library, `style {scale: 10, pixelAlign: true}` produces such a frame, with slightly larger padding.

On iOS, rasterise with the same arithmetic at the device scale. For example, at 3× use u = 10 px per unit with a 3 px stroke, laid out in points at 10/3 pt per unit. Alternatively, prerender PNGs per state and colour as the watch face does. Snap glyph image origins to whole pixels.

## 10. Interaction and refresh

- **Depth stepping:** tapping the top region moves to a longer period (depth − 1); tapping the bottom moves to a shorter one (depth + 1). Clamp the top depth to 2…11 on a watch and remember the choice. From depth 11 down, the sub glyph changes faster than about 50 ms (30 ms at depth 11, 6 ms at depth 12) and skips states on screen.
- **Info:** a single tap in the middle shows the info labels for about 4 s. A double tap (two taps within 400 ms) toggles persistent period letters.
- **Refresh:** recompute at the next state change of the finest visible glyph (from `nextTime`), or poll at a fraction of that interval, never faster than the display can show.
- **Battery:** avoid animations such as blinking. The finer glyph already shows how soon the coarser one rolls over.
- **Local time:** use the platform time zone. On devices without one, derive the UTC offset from the wall clock (`local − UTC`, rounded to 15 min), and reject offsets beyond ±14 h. Simulators may report a frozen wall clock.

## 11. Numeric implementation

The reference library uses exact BigInt rationals. A client using IEEE-754 doubles (Swift `Double`) must avoid multiplying a whole position by a large `P(L)`:

```
position = (ordinal: Int, fraction: Double)
units    = P(L) · M · q^(k−1)                       // exact integer
step     = ordinal · units + floor(fraction · units)
```

The Unix table has millisecond decimals; storing them as doubles is accurate to well under a microsecond. `ordinal · units` must stay below 2^53, or 2^63 with Int64. The error in `floor(fraction · units)` is about `units · 1.1e−16`, which is negligible for every level and odometer used here. Times from rational positions, `time(N / D)`, should split `N` into `ordinal = N div D` and `(N − ordinal·D)/D` before interpolating. The Saros 141 watch-face port, running on doubles and the Unix table, agrees with the library's exact Unix clock on 4,800 randomly sampled glyph states.

Swift has no built-in BigInt. Int64 holds `P(L)` well beyond depth 14. For exact rational work, such as exporting calendars or deep levels, use a BigInt package or port the library's rational helpers.

## 12. Test vectors

These are Saros 141 vectors on the Unix clock (`F.create({timeScale: 'unix'})`) with the default sequence, ascending family and anchor 20 / level 6 / 19 cycles / 4 days. Times are Unix seconds; ISO strings are UTC.

**A: 2026-10-08T12:00:00Z** (Unix 1791460800, position 22.927850216635)

| Quantity | Depth 7 (L=6) | Depth 8 (L=7) |
|---|---|---|
| index / value / derivative | 13 / −3 / +1 | 5 / 3 / −1 (dotted) |
| odometer digits (2 × 2-stroke) | [13, 0] | [5, 4] |
| cycle / local / siblings | 717211 / 0 / 11 | 6454906 / 7 / 9 |
| next threshold | end at 1791464172.428 (12:56:12.428Z) | node at 1791461140.937 (12:05:40.937Z) |
| 3-stroke index / value | 42 / −10 | — |

The solar sync at A:

| Quantity | Value |
|---|---|
| sync | 4820 |
| start | 1791336849.826 (2026-10-07T01:34:09.826Z) |
| end | 2026-10-11T01:33:59.745Z |
| phase | 0.358662585 |
| drift | −48469.9899 s |
| digits (3 × 2-stroke) | [5, 11, 13] |
| digits (2 × 3-stroke) | [18, 33] |
| next threshold | node at 1791509644.786 (2026-10-09T01:34:04.786Z) |

**B: 2000-01-01T00:00:00Z** (Unix 946684800, position 21.443111640256)

| Quantity | Value |
|---|---|
| depth 7 | index 5 (3, dotted); digits [5, 6]; local 9 of 11; next node at 946687783.046 (00:49:43.046Z) |
| depth 8 | index 0; digits [0, 6]; local 3 of 9; next peak at 946685256.803 (00:07:36.803Z) |
| solar | sync 2375; start 946369476.520 (1999-12-28T08:24:36.520Z); phase 0.912420883; drift −23843.2957 s; digits [14, 9, 9]; next end at 946715066.462 |

**C: 2061-06-15T18:30:00Z** (Unix 2886085800, position 24.851712757369)

| Quantity | Value |
|---|---|
| depth 7 | index 10 (−2, dotted); digits [10, 3]; local 0 of 11; next valley at 2886087806.395 (19:03:26.395Z) |
| depth 8 | index 12 (−4, valley); digits [12, 1]; local 5 of 9; next end at 2886086290.651 (18:38:10.651Z) |
| solar | sync 7987; start 2061-06-12T16:31:59.691Z; phase 0.770510076; drift −81000.1249 s; digits [12, 5, 4]; next end at 2886165109.343 |

**Anchor:** Unix 125593320.816 (one second after the anchor eclipse) gives sync 0, drift 0 and digits [0, 0, 0].

## 13. Pitfalls

These are the mistakes an implementation of the clock makes most easily. The list is a warning to whoever writes one, not a list of open points: each item restates a rule of an earlier section. The engine and the catalogue of this repository (`fractonica-core`, `fractonica-catalogue`) are held against every item by a test named for it, `pitfall_1_…` to `pitfall_8_…`, in the two crates' `tests/pitfalls.rs`.

1. **Mixed time scales.** The client catalogue and query times are both Unix seconds. Adding a TT offset to device time, or using the TT table with device time, shifts every reading by about 69 s today.
2. **Mean Saros.** Use real intervals (§7.4); the mean is wrong by up to 1.5 h per interval.
3. **Julian dates.** NASA's dates before 1582-10-15 are Julian. A source that stores them as if they were Gregorian needs the correction in §2.3, once. The reference catalogue has it already; correcting it again is the same mistake the other way round.
4. **Float precision.** Split position into ordinal and fraction (§11).
5. **Dot branch.** The dot marks the falling branch only (§4.1).
6. **Depth vs level.** Depth = level + 1. Names and siblings use the level, so siblings(L) = S[(L−1) mod 8].
7. **Final clipped cycle.** At the end of the catalogue the last cycle is clipped. Do not invent later events.
8. **Stale table.** After the last published leap second the Unix table holds TAI − UTC at 37 s (§2.1). If another leap second is announced, derive the table again from the TT source; values after it move by 1 s. In this repository that is `tools/catalogue/derive.mjs`, and `data/saros/README.md` has the steps. As of November 2026 none is announced, and none is expected once UTC becomes continuous on 20 May 2027.

## 14. Reference data

Solar Saros 141, greatest eclipse in **Unix seconds** (UTC), derived as in §2.1. Index = ordinal; ordinal 0 is 1613-05-19 and ordinal 69 is 2857-06-13. Ordinals 0–19 (before 1972) are ΔT estimates, 20–22 are exact, and 23–69 assume TAI − UTC = 37 s.

```
 -11253795486.968  -10684827340.124  -10115859492.780   -9546891859.112   -8977924443.862
  -8408957014.511   -7839989594.977   -7271021955.471   -6702054209.003   -6133086036.449
  -5564117571.512   -4995148587.324   -4426179194.600   -3857209183.600   -3288238731.185
  -2719267740.393   -2150296298.803   -1581324369.140   -1012352078.953    -443379486.426
    125593319.816     694566278.816    1263539192.816    1832512069.816    2401484716.816
   2970457153.816    3539429150.816    4108400821.816    4677371985.816    5246342709.816
   5815312911.816    6384282725.816    6953252120.816    7522221116.816    8091189841.816
   8660158322.816    9229126685.816    9798094931.816   10367063272.816   10936031708.816
  11505000323.816   12073969225.816   12642938471.816   13211908155.816   13780878253.816
  14349848916.816   14918820110.816   15487791887.816   16056764232.816   16625737153.816
  17194710612.816   17763684496.816   18332658871.816   18901633551.816   19470608536.816
  20039583596.816   20608558814.816   21177533934.816   21746508965.816   22315483725.816
  22884458266.816   23453432422.816   24022406224.816   24591379597.816   25160352610.816
  25729325154.816   26298297370.816   26867269191.816   27436240807.816   28005212035.816
```

The TT source values (NASA/GSFC, Espenak and Meeus) are `F.defaults.timestamps` in `fractonica.js`, and the Unix values are `F.defaults.unixTimestamps`. Unix = TT − 69.184 s for every ordinal from 23 on.

## 15. Time units and notation

This section defines how a client says a moment and a span **in Fractonica terms only**, without Gregorian dates, Earth time units or percentages. The web client's pure mode uses it throughout. The arithmetic is §3; nothing here changes a reading.

### 15.1 Major and minor periods

The ladder has two parts with a clear break between them.

- **Major (ultrahelion) periods** are depths 1–4: Alpha, Beta, Gamma and Delta. They are said in **words**, with a fracton ordinal and a quarter word (§15.2). Their phase is never drawn, because it follows from the Nuclide and Helion count below them.
- **Minor (infrahelion) units** take **every other depth** below Delta: Nuclide (depth 5), Helion (7), Preon (9), Flavon (11) and Blink (13). They are said in **letters and counts**. The depths in between (ζ θ κ μ, and ξ and finer) remain levels of the hierarchy but are not units and have no names of their own.

| Unit | Symbol | Depth | Made of | Mean length (§3.3) | Everyday analogue |
|---|---|---|---|---|---|
| Alpha | α | 1 | 5 Beta | 72.12 y | an era |
| Beta | β | 2 | 13 Gamma | 14.42 y | |
| Gamma | γ | 3 | 5 Delta | 1.11 y | a year |
| Delta | δ | 4 | 5 Nuclide | 81.05 d | a month |
| Nuclide | ε | 5 | 77 Helion (7 ζ × 11 η) | 16.21 d | a week |
| **Helion** | η | 7 | 36 Preon (9 θ × 4 ι) | 5.052 h | an hour |
| **Preon** | ι | 9 | 65 Flavon (5 κ × 13 λ) | 8.421 min | a minute |
| **Flavon** | λ | 11 | 25 Blink (5 μ × 5 ν) | 7.773 s | a second |
| Blink | ν | 13 | — | 310.92 ms | the finest unit; one tick of real time |

Helion, Preon and Flavon are the everyday units, used like hours, minutes and seconds. The Helion is the unit that locks to the Sun: 19 Helions make one solar sync of about 4 days (§7.2). The analogues are for orientation only; a client in Fractonica-only mode never shows them.

### 15.2 Names of major periods

A major cycle is named by its level name and a **fracton ordinal**: its 1-based position inside its parent, `local(L) + 1` (for Alpha the global `cycle(0) + 1`).

| n | Ordinal | n | Ordinal | n | Ordinal |
|---|---|---|---|---|---|
| 1 | Monofracton | 7 | Heptafracton | 13 | Tridecafracton |
| 2 | Difracton | 8 | Octafracton | 14 | Tetradecafracton |
| 3 | Trifracton | 9 | Nonafracton | 15 | Pentadecafracton |
| 4 | Tetrafracton | 10 | Decafracton | 16 | Hexadecafracton |
| 5 | Pentafracton | 11 | Didecafracton | 17 | Heptadecafracton |
| 6 | Hexafracton | 12 | Dodecafracton | 18 | Octadecafracton |

So "Delta Trifracton" is the 3rd Delta of its Gamma. Saros 141 needs ordinals up to 18 for Alpha, 5 for Beta, 13 for Gamma and 5 for Delta. Only the four major levels have fracton names.

The name is followed by the **quarter** the cycle is in, named by the threshold that ends it (§6.2):

```
quarter(L) = floor(4 · phase(L))        phase(L) = frac(position · P(L) / 4)
0 → Peak    1 → Node    2 → Valley    3 → End
```

"Delta Trifracton Node" is the second quarter of the 3rd Delta, the one that ends at its node.

### 15.3 Counts, ordinals and the date

Numbers in a reading are written in two forms:

- `X#n` is an **ordinal**, counted from 1: `δ#3`, `ε#2`. Majors in letter form, and the Nuclide, use it.
- `nX` is used for the minor clock and for spans. In a date or clock the **Helion is numbered from 1** (`1η` … `77η`
  inside its Nuclide), like the day of a month. Preons, Flavons and Blinks are **counts of completed units from 0**,
  like hours and minutes. In a span every part is a count: `5η 12ι` lasts five Helions and twelve Preons.

The minor reading of a moment is the Nuclide ordinal, the Helion's number and three counts, from the local indices of §3.2:

```
ε  = local(4) + 1                       1 … 5    Nuclide inside its Delta
η  = 11 · local(5) + local(6) + 1       1 … 77   the Helion's number in the Nuclide
ι  = 4  · local(7) + local(8)           0 … 35   Preons completed in the Helion
λ  = 13 · local(9) + local(10)          0 … 64   Flavons completed in the Preon
ν  = 5  · local(11) + local(12)         0 … 24   Blinks completed in the Flavon
```

A **date** is the major periods in words, then the minor reading. Flavons and Blinks are optional, like seconds.

| Form | Example |
|---|---|
| Full | `Alpha Pentafracton Peak, Beta Tetrafracton Valley, Gamma Difracton End, Delta Trifracton Node, ε#2 5η 5ι 5λ 5ν` |
| Short | `Delta Trifracton Node ε#2 5η 5ι` |
| Letters | `α#5 β#4 γ#2 δ#3 ε#2 5η 5ι 5λ` |
| Short letters | `δ#3 ε#2 5η 5ι` |

On screen a date **leads with its Pentad** (§7.5), as a civil date leads with the weekday: `Pentad II · Delta
Trifracton Node ε#2 5η 5ι`, or with the short form, `⬠II δ#3 ε#2 5η 5ι`. The Pentad is implied by the rest, so the
stamp below does not carry it and a parser reading a date ignores it.

The letter forms omit the quarter words, which the counts imply. The full letter form is the **stamp**: it identifies a moment exactly and can be read back. The Nuclide ordinal is required because a Delta has 5 Nuclides but 4 quarters, so a quarter alone does not say which Nuclide a Helion count belongs to.

The basic reading on screen is the **Helion**: its number in the Nuclide and its phase as an odometer of two 2-stroke glyphs (§5 at depth 7, 256 states of about 71 s each). Preons and Flavons give the fine time.

A minor cycle has no name of its own; it is identified by where it starts in its Nuclide. For example, a depth-9 cycle is `ε#2 5η 9ι`, a depth-8 cycle `ε#2 5η 8ι` (θ starts on multiples of 4ι), and a depth-14 cycle `… 3.43ν` (finer than a Blink, so a decimal).

### 15.4 Spans

A span is a list of counts over the ladder α β γ δ ε η ι λ ν, taken greedily from the largest unit that fits. Units with a zero count are skipped, so a span reads `3η 5λ`, never `3η 0ι 5λ`. Two non-zero parts are the default precision. Spans shorter than one Flavon that are led by the Blink keep a decimal (`2.6ν`, `0.14ν`). Measure spans in position, not in seconds, so they stay exact across Saros intervals.

Exact cycle lengths: depths 1–5, 7, 9, 11 and 13 are one unit each. The depths between are whole counts of the next unit: ζ = `11η`, θ = `4ι`, κ = `13λ`, μ = `5ν`, and ξ = `0.14ν`.

### 15.5 Real time: Blinks and pB

Real time is counted in **Blinks**; one Blink is the mean ν cycle, 310.92 ms.

- **Speed.** A time-lapse rate r (displayed seconds per real second) is said as units per Blink: `r · T(ν) / T(unit)`. The natural presets are whole counts: `5ν` (5×), `λ` (25×), `13λ` (325×), `ι` (1,625×), `4ι` (6,500×), `η` (58,500×), `11η` (643,500×), then `ε`, `δ`, `γ` per Blink. Real time itself is one Blink per Blink.
- **Frequency.** A frequency is said in **cycles per Blink (pB)**: `f_pB = f_Hz · T(ν)`. The true rates of the depths below the Blink are whole numbers: ξ 7, ο 77, π 693 and ρ 2,772 pB. A 110 Hz tone is 34.2 pB.

### 15.6 Test vectors

These use the vectors of §12. The Helion pair is the depth-7 odometer of §12, and the minor reading is §15.3 with Blinks.

| Vector | Full date | Short date | Helion pair |
|---|---|---|---|
| A | `Alpha Hexafracton Valley, Beta Tetrafracton Valley, Gamma Nonafracton Valley, Delta Trifracton End, ε#5 34η 29ι 21λ 3ν` | `Delta Trifracton End ε#5 34η 29ι` | [13, 0] |
| B | `Alpha Hexafracton Node, Beta Difracton End, Gamma Didecafracton Node, Delta Trifracton Node, ε#2 21η 12ι 6λ 5ν` | `Delta Trifracton Node ε#2 21η 12ι` | [5, 6] |
| C | `Alpha Heptafracton Peak, Beta Difracton Peak, Gamma Monofracton End, Delta Pentafracton Peak, ε#2 1η 23ι 1λ 21ν` | `Delta Pentafracton Peak ε#2 1η 23ι` | [10, 3] |
| Anchor + 1 s | `Alpha Hexafracton Peak, Beta Monofracton Peak, Gamma Monofracton Peak, Delta Monofracton Peak, ε#1 1η 0ι 0λ 3ν` | `Delta Monofracton Peak ε#1 1η 0ι` | [0, 0] |

The stamp at A is `α#6 β#4 γ#9 δ#3 ε#5 34η 29ι 21λ 3ν`. At A the depth-7 end is 3,372.428 s away (§12), which is the span `6ι 43λ`.

## 16. Spectral ladder and spatial units

### 16.1 The ladder

The hierarchy of §3 does not stop at the levels a clock face shows. Read as a frequency, every level is a rung of one ladder that runs from the Alpha, four Saros long, down to the edge of the Planck time: **63 rungs, depths 1 to 63** (levels 0 to 62).

```
T(L) = 4 · I / P(L)        the period of level L; I is the length of a Saros interval
f(L) = P(L) / (4 · I)      its frequency
```

Which interval `I` is, is the caller's choice, and an implementation gives each of these:

- **The interval that holds an instant** (§2.2): what the clock is doing then. An eclipse belongs to the interval it opens, and the catalogue's closing instant to its last interval. Outside the catalogue there is no interval and no ladder.
- **The series' mean interval**, `(E[n] − E[0]) / n`: one figure for the whole series, which does not step at an eclipse. The Blink of §15.5 is its depth 13.
- **Any Alpha given as a length of time**: a nominal scale, such as the reference library's `F.period`.

Real intervals differ (§7.4), so every frequency steps a little at each eclipse; between two intervals of Saros 141 it differs by up to 14 parts per million.

Names follow §3.4: after ω the letters repeat with a number, so depth 25 is α1 and depth 63 is ο2. The units of §15 go down to the Blink only.

These figures use the 2010–2028 Saros 141 interval, 568,972,877 s, as §3.3 does. The last column is the length of §16.3 for light in vacuum.

| Depth | Level | Name | Unit (§15) | Siblings | Period | Frequency | Band | Light in vacuum |
|---|---|---|---|---|---|---|---|---|
| 1 | 0 | α | Alpha | — | 72.12 y | 439.4 pHz | — | 682.3 Pm |
| 2 | 1 | β | Beta | 5 | 14.42 y | 2.197 nHz | — | 136.5 Pm |
| 3 | 2 | γ | Gamma | 13 | 1.110 y | 28.56 nHz | — | 10.50 Pm |
| 4 | 3 | δ | Delta | 5 | 81.05 d | 142.8 nHz | — | 2.099 Pm |
| 5 | 4 | ε | Nuclide | 5 | 16.21 d | 714.0 nHz | — | 419.9 Tm |
| 6 | 5 | ζ | — | 7 | 2.316 d | 4.998 µHz | — | 59.98 Tm |
| 7 | 6 | η | Helion | 11 | 5.052 h | 54.98 µHz | — | 5.453 Tm |
| 8 | 7 | θ | — | 9 | 33.68 min | 494.8 µHz | — | 605.9 Gm |
| 9 | 8 | ι | Preon | 4 | 8.421 min | 1.979 mHz | — | 151.5 Gm |
| 10 | 9 | κ | — | 5 | 1.684 min | 9.896 mHz | — | 30.29 Gm |
| 11 | 10 | λ | Flavon | 13 | 7.773 s | 128.6 mHz | — | 2.330 Gm |
| 12 | 11 | μ | — | 5 | 1.555 s | 643.2 mHz | — | 466.1 Mm |
| 13 | 12 | ν | Blink | 5 | 310.9 ms | 3.216 Hz | — | 93.21 Mm |
| 14 | 13 | ξ | — | 7 | 44.42 ms | 22.51 Hz | audible | 13.32 Mm |
| 15 | 14 | ο | — | 11 | 4.038 ms | 247.7 Hz | audible | 1.211 Mm |
| 16 | 15 | π | — | 9 | 448.7 µs | 2.229 kHz | audible | 134.5 km |
| 17 | 16 | ρ | — | 4 | 112.2 µs | 8.915 kHz | audible | 33.63 km |
| 18 | 17 | σ | — | 5 | 22.43 µs | 44.58 kHz | radio | 6.725 km |
| 19 | 18 | τ | — | 13 | 1.726 µs | 579.5 kHz | radio | 517.3 m |
| 20 | 19 | υ | — | 5 | 345.1 ns | 2.898 MHz | radio | 103.5 m |
| 21 | 20 | φ | — | 5 | 69.02 ns | 14.49 MHz | radio | 20.69 m |
| 22 | 21 | χ | — | 7 | 9.861 ns | 101.4 MHz | radio | 2.956 m |
| 23 | 22 | ψ | — | 11 | 896.4 ps | 1.116 GHz | microwave | 268.7 mm |
| 24 | 23 | ω | — | 9 | 99.60 ps | 10.04 GHz | microwave | 29.86 mm |
| 25 | 24 | α1 | — | 4 | 24.90 ps | 40.16 GHz | microwave | 7.465 mm |
| 26 | 25 | β1 | — | 5 | 4.980 ps | 200.8 GHz | microwave | 1.493 mm |
| 27 | 26 | γ1 | — | 13 | 383.1 fs | 2.610 THz | infrared | 114.8 µm |
| 28 | 27 | δ1 | — | 5 | 76.62 fs | 13.05 THz | infrared | 22.97 µm |
| 29 | 28 | ε1 | — | 5 | 15.32 fs | 65.26 THz | infrared | 4.594 µm |
| 30 | 29 | ζ1 | — | 7 | 2.189 fs | 456.8 THz | visible | 656.3 nm |
| 31 | 30 | η1 | — | 11 | 199.0 as | 5.025 PHz | ultraviolet | 59.66 nm |
| 32 | 31 | θ1 | — | 9 | 22.11 as | 45.22 PHz | X-ray | 6.629 nm |
| 33 | 32 | ι1 | — | 4 | 5.528 as | 180.9 PHz | X-ray | 1.657 nm |
| 34 | 33 | κ1 | — | 5 | 1.106 as | 904.5 PHz | X-ray | 331.4 pm |
| 35 | 34 | λ1 | — | 13 | 85.05 zs | 11.76 EHz | X-ray | 25.50 pm |
| 36 | 35 | μ1 | — | 5 | 17.01 zs | 58.79 EHz | gamma ray | 5.099 pm |
| 37 | 36 | ν1 | — | 5 | 3.402 zs | 294.0 EHz | gamma ray | 1.020 pm |
| 38 | 37 | ξ1 | — | 7 | 486.0 ys | 2.058 ZHz | gamma ray | 145.7 fm |
| 39 | 38 | ο1 | — | 11 | 44.18 ys | 22.63 ZHz | gamma ray | 13.24 fm |
| 40 | 39 | π1 | — | 9 | 4.909 ys | 203.7 ZHz | gamma ray | 1.472 fm |
| 41 | 40 | ρ1 | — | 4 | 1.227 ys | 814.9 ZHz | gamma ray | 367.9 am |
| 42 | 41 | σ1 | — | 5 | 2.454 × 10⁻²⁵ s | 4.074 YHz | gamma ray | 73.58 am |
| 43 | 42 | τ1 | — | 13 | 1.888 × 10⁻²⁶ s | 52.97 YHz | gamma ray | 5.660 am |
| 44 | 43 | υ1 | — | 5 | 3.776 × 10⁻²⁷ s | 264.8 YHz | gamma ray | 1.132 am |
| 45 | 44 | φ1 | — | 5 | 7.552 × 10⁻²⁸ s | 1.324 × 10²⁷ Hz | gamma ray | 226.4 zm |
| 46 | 45 | χ1 | — | 7 | 1.079 × 10⁻²⁸ s | 9.269 × 10²⁷ Hz | gamma ray | 32.34 zm |
| 47 | 46 | ψ1 | — | 11 | 9.808 × 10⁻³⁰ s | 1.020 × 10²⁹ Hz | gamma ray | 2.940 zm |
| 48 | 47 | ω1 | — | 9 | 1.090 × 10⁻³⁰ s | 9.176 × 10²⁹ Hz | — | 326.7 ym |
| 49 | 48 | α2 | — | 4 | 2.724 × 10⁻³¹ s | 3.671 × 10³⁰ Hz | — | 81.68 ym |
| 50 | 49 | β2 | — | 5 | 5.449 × 10⁻³² s | 1.835 × 10³¹ Hz | — | 16.34 ym |
| 51 | 50 | γ2 | — | 13 | 4.191 × 10⁻³³ s | 2.386 × 10³² Hz | — | 1.257 ym |
| 52 | 51 | δ2 | — | 5 | 8.383 × 10⁻³⁴ s | 1.193 × 10³³ Hz | — | 2.513 × 10⁻²⁵ m |
| 53 | 52 | ε2 | — | 5 | 1.677 × 10⁻³⁴ s | 5.965 × 10³³ Hz | — | 5.026 × 10⁻²⁶ m |
| 54 | 53 | ζ2 | — | 7 | 2.395 × 10⁻³⁵ s | 4.175 × 10³⁴ Hz | — | 7.180 × 10⁻²⁷ m |
| 55 | 54 | η2 | — | 11 | 2.177 × 10⁻³⁶ s | 4.593 × 10³⁵ Hz | — | 6.528 × 10⁻²⁸ m |
| 56 | 55 | θ2 | — | 9 | 2.419 × 10⁻³⁷ s | 4.133 × 10³⁶ Hz | — | 7.253 × 10⁻²⁹ m |
| 57 | 56 | ι2 | — | 4 | 6.048 × 10⁻³⁸ s | 1.653 × 10³⁷ Hz | — | 1.813 × 10⁻²⁹ m |
| 58 | 57 | κ2 | — | 5 | 1.210 × 10⁻³⁸ s | 8.267 × 10³⁷ Hz | — | 3.626 × 10⁻³⁰ m |
| 59 | 58 | λ2 | — | 13 | 9.305 × 10⁻⁴⁰ s | 1.075 × 10³⁹ Hz | — | 2.790 × 10⁻³¹ m |
| 60 | 59 | μ2 | — | 5 | 1.861 × 10⁻⁴⁰ s | 5.374 × 10³⁹ Hz | — | 5.579 × 10⁻³² m |
| 61 | 60 | ν2 | — | 5 | 3.722 × 10⁻⁴¹ s | 2.687 × 10⁴⁰ Hz | — | 1.116 × 10⁻³² m |
| 62 | 61 | ξ2 | — | 7 | 5.317 × 10⁻⁴² s | 1.881 × 10⁴¹ Hz | — | 1.594 × 10⁻³³ m |
| 63 | 62 | ο2 | — | 11 | 4.834 × 10⁻⁴³ s | 2.069 × 10⁴² Hz | — | 1.449 × 10⁻³⁴ m |

Depth 64 (π2) would last 5.371 × 10⁻⁴⁴ s, 0.4 % short of the Planck time (5.391247 × 10⁻⁴⁴ s, CODATA 2018), and light would cross 1.610 × 10⁻³⁵ m in it, the Planck length by the same margin. Nothing shorter is known to have a meaning, so the ladder ends at depth 63 and an implementation gives nothing below it.

### 16.2 Bands

The band says where a frequency falls among sound and light. It is a description, not a rule of the clock.

| Band | From | Up to, not including |
|---|---|---|
| audible | 20 Hz | 20 kHz |
| radio | 20 kHz | 300 MHz |
| microwave | 300 MHz | 300 GHz |
| infrared | 300 GHz | 400 THz |
| visible | 400 THz | 790 THz |
| ultraviolet | 790 THz | 30 PHz |
| X-ray | 30 PHz | 30 EHz |
| gamma ray | 30 EHz | 3.4 × 10²⁹ Hz |

Below 20 Hz, and from 3.4 × 10²⁹ Hz up, a frequency has no band. "Audible" is a band of sound; the others are of electromagnetic radiation, with the boundaries of ISO 21348 rounded, and the top is the most energetic photon observed, about 1.4 PeV. The comparison is made on the exact frequency, and a frequency equal to a boundary belongs to the band that starts there.

### 16.3 Spatial units

A period becomes a length through something that travels: the distance it covers in one period at a constant speed.

```
length(L, medium) = v(medium) · T(L)        in metres
```

| Id | Medium | Phase | Speed (m/s) | Note |
|---|---|---|---|---|
| `vacuum` | Light in vacuum | Vacuum | 299,792,458 | Exact, by the definition of the metre. |
| `bounded` | Light in bounded medium | Vacuum + solid | 200,000,000 | A shared benchmark for a signal in copper or fibre; not the drift of electrons. The real speed depends on the guide. |
| `diamond` | Diamond sound | Solid | 18,000 | Nominal longitudinal sound; depends on direction and mode. |
| `water` | Water sound | Liquid | 1,500 | Nominal; depends on temperature, salinity and pressure. |
| `air` | Air sound | Gas | 343 | Nominal, near 20 °C. |
| `nerve` | Nerve conduction | Solid + liquid | 120 | The upper benchmark of fast conduction, not the speed of every tissue. |
| `chemical` | Chemical transport | Liquid + gas + solid | 0.001 | An abstract proxy of constant speed; not a law of diffusion. |

**Light in vacuum is the default**: a length given without a medium is that one. The other six speeds are nominal benchmarks and exact here by definition: sound in air is 343 m/s whatever the weather. They make lengths comparable; they do not measure anything. A caller with a better figure for its own medium gives the speed itself, and the rule is the same.

The same rule gives the distance covered in any length of time, `v · t`.

In the reference interval an Alpha of light in vacuum is 72.12 light-years (a light-year is 9,460,730,472,580,800 m). A Blink is 93,212 km of light in vacuum, 106.6 m of sound in air and 37.31 m along a nerve. Depth 30 (ζ1) is red light, 656.3 nm.

### 16.4 Numeric implementation

`P(62)` is 4.7 × 10⁵¹ and needs 172 bits: no double holds it, and no 64-bit or 128-bit integer. So:

- `P(L)` is an exact integer, as listed below.
- A period, a frequency and a length are exact rationals. With `I` in whole milliseconds, `T(L) = 4 · I / (1000 · P(L))` seconds and `f(L) = 1000 · P(L) / (4 · I)` hertz.
- A result given as a double is the double nearest to the exact value, halves to even. Every value of this section fits one: they run from about 10⁻⁴⁶ to 10⁴³.
- Never work down the ladder in doubles, dividing rung by rung: each step rounds, and 62 of them show in the last digits.

**A device that makes a rung** rounds once, from the exact value, a half going up:

```
ticks = round(T(L) · r)              one period, counted by a timer that runs at r ticks per second
step  = round(f(L) · 2^b / r)        added each sample to a phase of b bits, at r samples per second
```

A period of less than half a tick cannot be counted. A frequency above half the sample rate cannot be made, nor one so low that the step rounds to nothing. Each is refused, not rounded to something else, so that two devices given the same rung and the same rate count the same number.

```
P(0) = 1
P(1) = 5
P(2) = 65
P(3) = 325
P(4) = 1625
P(5) = 11375
P(6) = 125125
P(7) = 1126125
P(8) = 4504500
P(9) = 22522500
P(10) = 292792500
P(11) = 1463962500
P(12) = 7319812500
P(13) = 51238687500
P(14) = 563625562500
P(15) = 5072630062500
P(16) = 20290520250000
P(17) = 101452601250000
P(18) = 1318883816250000
P(19) = 6594419081250000
P(20) = 32972095406250000
P(21) = 230804667843750000
P(22) = 2538851346281250000
P(23) = 22849662116531250000
P(24) = 91398648466125000000
P(25) = 456993242330625000000
P(26) = 5940912150298125000000
P(27) = 29704560751490625000000
P(28) = 148522803757453125000000
P(29) = 1039659626302171875000000
P(30) = 11436255889323890625000000
P(31) = 102926303003915015625000000
P(32) = 411705212015660062500000000
P(33) = 2058526060078300312500000000
P(34) = 26760838781017904062500000000
P(35) = 133804193905089520312500000000
P(36) = 669020969525447601562500000000
P(37) = 4683146786678133210937500000000
P(38) = 51514614653459465320312500000000
P(39) = 463631531881135187882812500000000
P(40) = 1854526127524540751531250000000000
P(41) = 9272630637622703757656250000000000
P(42) = 120544198289095148849531250000000000
P(43) = 602720991445475744247656250000000000
P(44) = 3013604957227378721238281250000000000
P(45) = 21095234700591651048667968750000000000
P(46) = 232047581706508161535347656250000000000
P(47) = 2088428235358573453818128906250000000000
P(48) = 8353712941434293815272515625000000000000
P(49) = 41768564707171469076362578125000000000000
P(50) = 542991341193229097992713515625000000000000
P(51) = 2714956705966145489963567578125000000000000
P(52) = 13574783529830727449817837890625000000000000
P(53) = 95023484708815092148724865234375000000000000
P(54) = 1045258331796966013635973517578125000000000000
P(55) = 9407324986172694122723761658203125000000000000
P(56) = 37629299944690776490895046632812500000000000000
P(57) = 188146499723453882454475233164062500000000000000
P(58) = 2445904496404900471908178031132812500000000000000
P(59) = 12229522482024502359540890155664062500000000000000
P(60) = 61147612410122511797704450778320312500000000000000
P(61) = 428033286870857582583931155448242187500000000000000
P(62) = 4708366155579433408423242709930664062500000000000000
P(63) = 42375295400214900675809184389375976562500000000000000
```

### 16.5 Test vectors

In the reference interval, `I = 568,972,877,000 ms`:

| Quantity | Exact | Rounded |
|---|---|---|
| `T(12)`, the Blink (ν) | `51724807/166359375` s | 310.922 ms |
| `f(13)`, ξ | `1164515625/51724807` Hz | 22.5137 Hz |
| `f(62)`, ο2 | `107008321717714395645982788862060546875000000000000/51724807` Hz | 2.0688 × 10⁴² Hz |
| `length(12)`, light in vacuum | `2215243861443658/23765625` m | 93,212.1 km |
| `length(12, air)` | `2534515543/23765625` m | 106.646 m |
| `length(12, nerve)` | `413798456/11090625` m | 37.3107 m |
| `length(12, chemical)` | `51724807/166359375000` m | 310.922 µm |
| `length(29)`, ζ1, light in vacuum | `1107621930721829/1687759133607421875000` m | 656.268 nm |

On the mean interval of Saros 141, `39,259,007,522,784 / 69` ms: `f(12) = 250529296875/77894856196` Hz, which is 3.21625 Hz, and `T(12)` is 310.921 ms, the Blink of §15.5.
