# AGENTS.md — working on fractonica-core

The guide for a coding agent (or engineer) picking this repository up. Read
`include/fractonica.h` in full first: it is the contract, and
`tests/test_header.c` holds its documented edges one by one. This file tells
you where things are and what you must not break.

- **What it is:** the shared, dependency-free C99 core of the Fractonica clock
  (library 0.2.0, implements `docs/SPEC.md` 2.2.0). Consumed by iOS/macOS
  (SwiftPM), web (WebAssembly) and ESP32 firmware.
- **Sources of truth, in order:** `docs/SPEC.md` (behaviour) → `include/fractonica.h`
  (API contract) → `tests/` (both, made executable). Code that disagrees with
  them is the bug.
- **Before a commit:** `make check`. With a full LLVM installed: `make check-all`.
  With cross compilers installed (ESP-IDF, arm-none-eabi, avr): `make check-cross`.

## 1. Vocabulary and invariants (one screen)

| Term | Rule |
|---|---|
| **time** | `frc_time_t` = `int64_t` Unix **microseconds** (UTC, 86,400-second days, never TT). `FRC_TIME_FROM_S` / `FRC_TIME_FROM_MS` convert; `frc_time_from_s` / `frc_time_from_ms` / `frc_time_is_none` are the same for bindings that cannot see macros (Swift, WebAssembly). The catalogue table alone is in **milliseconds**. |
| **level vs depth** | The API takes **levels**, zero-based (0 = Alpha). `depth = level + 1` is a user-facing number and never appears in code. `siblings(L) = S[(L−1) mod 8]`, the factor of the parent. |
| **level limits** | Static facts (names, periods, lengths): 0..62 (`FRC_LEVEL_COUNT` 63). Clock readings at an instant: 0..`FRC_CLOCK_MAX_LEVEL` (21). `frc_prefix` fits `uint64_t` up to 22. |
| **position** | Saros units, kept exact as `ordinal + num/den` (`den` = that interval in µs, `0 ≤ num < den`). Never a double, never a mean Saros. |
| **boundary instant** | The exact rational instant, returned as the **first microsecond at or after it** (ceiling). Reading the clock at a returned instant shows the new state; one microsecond earlier shows the old one. |
| **intervals** | Half-open `[start, end)`. End of cycle `c` == start of `c+1`. The one exception: the catalogue's closing instant belongs to the last cycle, sync and Pentad, so a reading taken exactly there has `t == end`. |
| **spans** | Measured in position (Blinks), never in seconds. An instant is a boundary rounded up to a microsecond, so `frc_span_between` reads a span within one microsecond of a whole number of quarter Blinks as that number: start → end of a cycle is its length exactly. |
| **enum fields** | Functions take the enum types; struct fields hold the same values in a `uint8_t` (`frc_threshold_t.name`, `frc_cycle_t.quarter`, `frc_span_part_t.unit` …). C++ and Swift callers cast. |
| **"next" instants** | Strictly after the instant asked about. At the catalogue's closing instant: `FRC_TIME_NONE`. |
| **catalogue edges** | Defined on `[first, last]`. Outside: `FRC_ERR_RANGE` / `FRC_TIME_NONE`, never extrapolated. The closing instant reads as one microsecond earlier. The last cycle is clipped there. |
| **exactness** | Integer arithmetic only on the clock path; every rounding is named (floor, ceil, half-up, half-even) and happens once, from the exact value. `docs/DESIGN.md` has the identities and overflow bounds. |
| **floats** | Only inside `#ifndef FRC_NO_FLOAT`, only in the functions the header puts there. No reading depends on them. `float` only in arcs (no `double`). |
| **errors** | `frc_status_t`: `FRC_OK` 0, errors negative. Argument errors (`FRC_ERR_ARG`) win over range errors. Text functions return a byte count or a negative status and leave `""` in the buffer on an argument error. A NULL text buffer is a buffer of size 0: `FRC_ERR_BUFFER`, not `FRC_ERR_ARG`. What an output holds after any other error is defined only where the header says so. |
| **text** | UTF-8 into a caller buffer, always NUL-terminated; on `FRC_ERR_BUFFER` the buffer holds a clean prefix (never half a code point). |
| **tables** | `static const … NAME[…] FRC_PROGMEM`, read **only** through `FRC_PGM_*` (`make check-progmem` holds it). Strings are packed lists (`"Peak\0Node\0…"`). |
| **no libc** | `src/` includes `<stdint.h>`, `<stddef.h>` and project headers only. No `memcpy`/`memset`/`strlen`, not even compiler-generated ones. |
| **structs by address** | Inside the core a struct of 16 bytes or more (`frc_i_u128_t`, `frc_interval_t`, `frc_speed_t`, `frc_arc_t` …) is never passed or returned by value and never assigned whole: pass a pointer, copy the fields. |
| **no state** | No heap, no recursion, no mutable static or global data, small stack frames. Every function is re-entrant. `.data` + `.bss` = 0 bytes. |
| **names** | Public `frc_*` / `FRC_*`; shared internal `frc_i_*` / `FRC_I_*` (declared in `src/frc_internal.h` or `src/frc_big.h`); everything else `static`. |

## 2. Hard rules for `src/`

1. Pure C99. Must compile with
   `-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` on every
   target and at `-O0`, `-Og`, `-Os`, `-O2`, `-O3`. Fix warnings with an explicit cast where
   the bound is proven, and say the bound in a comment.
2. No `__int128`, no `long double`: use section 2 of `frc_internal.h`.
3. Do not change a signature or the documented behaviour in `fractonica.h`
   casually. A header comment and its line in `tests/test_header.c` change
   together, never one alone.
4. Each `.c` file starts with a comment: what it implements, which SPEC
   sections, the model, the invariants. Keep it true when you edit the file.
5. Comments explain **why** (the identity, the overflow bound), not what.
6. Do not weaken or delete a test to make it pass.

Compilers turn innocent code into libc calls or RAM tables. All of these
happened here; the checks in section 7 catch them:

| You write | The compiler makes | Write instead |
|---|---|---|
| a loop that zero-fills or copies an array, `= {0}`, a large struct copy | `memset` / `memcpy` | fill field by field; for `frc_i_big_t` use `set_u64`, `mul_u64(dst, src, 1)` |
| a loop that scans for a NUL | `strlen` | keep the scan doing real work per byte |
| a `switch` mapping small values to constants | a lookup table in RAM (AVR) | a `FRC_PROGMEM` table |
| a 16-byte struct passed **by value** (`f(frc_i_u128_t x)`), also a public `frc_interval_t` forwarded by value | the caller copies it with `memcpy` (riscv32 gcc `-Os`, ARMv6-M at every level) | `f(const frc_i_u128_t *x)`; results through an out-pointer |
| a 16-byte struct returned by value from an internal function, or `a = b` on one | `memcpy` (xtensa gcc `-O0`) | an out-parameter; copy the fields |
| `a > UINT64_MAX / b` | 128-bit multiply = `__multi3` (wasm32) | `frc_i_mul64_fits(a, b, &product)` |
| `value < 0` on an enum argument | `-Wtype-limits` error on arm-none-eabi | `FRC_I_BELOW(value, count)` |
| a division whose divisor is not provably non-zero | a trap = `abort()` (avr-gcc -O2) | check the divisor first |
| `cond ? (uint8_t)x : 0` assigned to a `uint8_t` | `-Wconversion` error under gcc + UBSan | cast the whole conditional |

## 3. I need X → call Y

All functions take `const frc_catalogue_t *cat` where a catalogue is needed;
`&FRC_SAROS_141` is the built-in one.

### Catalogue and position (SPEC §2)

| I need | Call |
|---|---|
| the built-in series | `FRC_SAROS_141` (70 eclipses, anchor 20) |
| another series from my own table | `frc_catalogue_init` (validates, chooses the anchor) |
| first / last / k-th eclipse instant | `frc_catalogue_first`, `_last`, `_instant` |
| is `t` inside the clock's domain | `frc_catalogue_contains` |
| position of an instant | `frc_position_at` → `frc_position_t` |
| instant of a rational position `n/d` | `frc_time_at_position` |
| position as a double (display) | `frc_position_to_double` (float API) |
| Unix ms / s → `frc_time_t`, "is it `FRC_TIME_NONE`" without macros (Swift, wasm) | `frc_time_from_ms`, `frc_time_from_s`, `frc_time_is_none`; `frc_frac_to_double` (float API) |

### Hierarchy, names, units (SPEC §3, §15.1)

| I need | Call |
|---|---|
| children per cycle / cycles per parent | `frc_factor(level)` / `frc_siblings(level)` |
| `P(level)` | `frc_prefix` (≤ 22, else 0); as text for 0..63: `frc_format_prefix` |
| is a level a major period, a unit, in between, sub-Blink | `frc_level_kind` |
| "η" / "Eta" for a level | `frc_format_level_name` / `frc_format_level_word` |
| unit ↔ level | `frc_unit_level`, `frc_unit_of_level` |
| a unit in Blinks, parts per unit | `frc_unit_blinks`, `frc_unit_parts` |
| "η" / "Helion(s)" for a unit | `frc_format_unit_symbol` / `frc_format_unit_name` |
| parent / first child of a cycle | arithmetic: `c / frc_siblings(L)`, `c * frc_factor(L)` |

### Cycles, counters, thresholds (SPEC §3.2, §6)

| I need | Call |
|---|---|
| the cycle of a level running at `t` (index, local, siblings, phase, quarter, start, end, clipped) | `frc_cycle_at` |
| a cycle by global index | `frc_cycle_by_index` |
| how many cycles a level has in the catalogue | `frc_cycle_count` |
| start, peak, node, valley, end of a cycle | `frc_cycle_points` |
| the next quarter boundary after `t` | `frc_next_threshold` |
| the instant a cycle reaches a phase (a scrub; the inverse of `.phase`) | `frc_time_at_phase` |
| "Peak" / "node" | `frc_format_quarter` |
| sibling counter (SPEC §6.1) | `frc_cycle_t.local` of `.siblings` |

### Glyphs and odometer (SPEC §4, §5, §9)

| I need | Call |
|---|---|
| the glyph state of a level at `t` | `frc_glyph_at` |
| decode a state index (value, derivative, dot, trits) | `frc_glyph_from_index` |
| `M`, number of states | `frc_glyph_extreme`, `frc_glyph_states` |
| descending family | `frc_glyph_index_descending` |
| glyph index of a bare fraction | `frc_glyph_index_of_phase` |
| the glyph a cycle shows at a threshold (Peak 4, Node 8, Valley 12, End 0 at 2 strokes) | `frc_glyph_at_quarter` |
| k glyphs side by side at `t`, and when to redraw | `frc_odometer_at` → `digit[]`, `next_change` |
| odometer digits of a bare fraction | `frc_odometer_of_phase` |
| draw a glyph: path commands | `frc_glyph_path` (move / line / cubic / dot) |
| draw without curves | `frc_glyph_segments` (+ the dot yourself) |
| glyph bounding box | `frc_glyph_bounds` |

### Solar sync and Pentads (SPEC §7)

| I need | Call |
|---|---|
| the sync running at `t` (number, phase, drift, next quarter) | `frc_sync_at` |
| a sync by number | `frc_sync_by_number` |
| the sync phase as glyph digits | `frc_sync_odometer` (reference: 2 strokes, 3 digits) |
| the running Helion's place in the sync, 0..18 | `frc_sync_t.helion` |
| the instant a sync reaches a phase (a place on the Sun's ring) | `frc_time_at_sync_phase` |
| the Pentad at `t` / by number | `frc_pentad_at` / `frc_pentad_by_number` |
| where a Pentad starts in its sync, 0 5 10 15 | `frc_pentad_t.first` |
| "Pentad II, 3rd Helion", "⬠II·3", "P2.3"; the numeral of a drawn mark "II" | `frc_format_pentad` (`FRC_PENTAD_NUMERAL` for the last) |

### Notation (SPEC §15)

| I need | Call |
|---|---|
| the date at `t` | `frc_date_at` → `frc_date_t` |
| the instant a date names | `frc_date_to_time` |
| write a date (full, short, letters, stamp) | `frc_format_date` |
| one term of the stamp: "ε#5", "34η" | `frc_format_date_term` |
| read a date back (also ASCII shorthand) | `frc_parse_date` |
| "Trifracton" | `frc_format_ordinal` |
| the name of a cycle | `frc_format_cycle_name` |
| the span between two instants (whole units between two returned boundaries) | `frc_span_between` |
| the nominal length of a level's cycle (exact to level 12, then cut to 2⁻³² Blink; zero from level 24) | `frc_span_of_level` |
| the exact ratio of a level below the Blink | `frc_level_per_blink` (to level 35), `frc_format_prefix` |
| a span as unit counts / as text | `frc_span_parts` / `frc_format_span` ("<0.001ν" for any non-zero span below 1/1000 Blink, "0ν" only for zero) |
| a span with its sign always said: "+6ι 43λ", "−20ι 21λ" | `frc_format_span_signed` |
| a span in whole units of one unit, cut: "−6 236λ" | `frc_format_span_in_unit` |
| when a countdown's text changes (sleep until it) | `frc_span_next_change` (`FRC_TIME_NONE` from the boundary on) |
| when the text of an elapsed span, or of a signed span past its zero, changes | `frc_span_next_change_since(cat, origin, now, max_parts)` (a signed display: `frc_span_next_change` before the boundary, this from it on) |
| time-lapse presets | `frc_rate_preset`, `frc_format_rate` |
| cycles per Blink of a level | `frc_level_per_blink`, `frc_format_per_blink` |

### Pure wording: static facts without an Earth unit (design rulings 2 and 3)

A client in pure mode shows no second, hertz or metre, and composes none of
these texts itself: all naming and number formatting is here.

| I need | Call |
|---|---|
| a whole number in digit groups: "58 500" | `frc_format_count` |
| a signed one: "−3" (U+2212) | `frc_format_count_signed` |
| what a level is: "major", "unit", "between", "sub-Blink" (ruling 4) | `frc_format_level_kind` |
| a cycle among its siblings: "8 of 9" | `frc_format_cycle_place` |
| what a level is made of: "9 θ"; how many in its parent: "11 in ζ"; a unit in the next unit: "77 in ε" | `frc_format_level_children`, `frc_format_level_in_parent`, `frc_format_unit_in_parent` |
| P with its factors: "125 125 = 5 · 13 · 5 · 5 · 7 · 11" (every level, in full; a buffer of `FRC_PREFIX_FACTORS_SIZE`, 429 bytes, holds level 62) | `frc_format_prefix_factors` |
| the word "light" alone | `frc_format_light_word` |
| how many of a unit one cycle of a level holds: "58 500ν", "36ι", "11η" | `frc_format_level_in_unit` (level ≤ 12, unit not coarser than the level) |
| the period of a level down to the Blink, as a span: "11η", "4ι" | `frc_span_of_level` + `frc_format_span` |
| the rate of ANY level 0..62 in cycles per Blink: "1 per 58 500 Blinks", "2 772 pB", "1.420 × 10¹⁴ pB" | `frc_format_level_rate` (`digits` 0 = every digit; `frc_level_per_blink` stops at level 35) |
| a length said without metres, as the span light needs to cover it: "1η light", "24ι 1λ light", "5.15ο light" | `frc_format_light_span` (the speed from `frc_medium_speed` or the caller's own) |

### Spectral ladder (SPEC §16)

| I need | Call |
|---|---|
| a Saros interval to lay the ladder from | `frc_interval_at`, `frc_interval_mean`, `frc_interval_of_alpha_ms`, `FRC_INTERVAL_REFERENCE_MS` |
| period / frequency / length, 17 exact digits | `frc_period`, `frc_frequency`, `frc_length` → `frc_sci_t` |
| the same as text | `frc_format_period`, `_frequency`, `_length`, `frc_format_quantity` |
| the same as a double | `frc_period_seconds`, `frc_frequency_hz`, `frc_length_metres`, `frc_sci_to_double` (float API) |
| the speed and names of a medium | `frc_medium_speed`, `frc_format_medium_id`, `_name`, `_phase` |
| the band of a level ("no band" outside every band, ruling 4) | `frc_band`, `frc_format_band` |
| the edges of a band in hertz (SPEC §16.2) | `frc_band_edges` |
| timer ticks / DDS phase step for a rung | `frc_device_ticks`, `frc_device_step` |

### Arcs, civil time, text

| I need | Call |
|---|---|
| nested alternating arcs | `frc_arc_wave_root`, `frc_arc_child`, `frc_arc_point` (float API) |
| sine / cosine without libm | `frc_sin`, `frc_cos` (float API) |
| Gregorian fields of an instant, and back | `frc_civil_from_time`, `frc_time_from_civil` |
| ISO 8601 text | `frc_format_civil` |
| Earth-mode text: "THU 8 OCT 2026 · 12:00:00", "12:56", "9 OCT 01:34", "UTC−2:30" ... | `frc_format_civil_text` (`frc_civil_form_t`; the offset form takes \|offset\| < 1 day) |
| an Earth duration: "23 min 53 s", "0.4 s" | `frc_format_duration` |
| is it a real day and time ("31 FEB" is not) | `frc_civil_validate` |
| read a typed date ("2026-10-08T14:00:00+02:00", "8 OCT 2026 12:00") | `frc_parse_civil` |
| fix Julian fields stored as Gregorian (table building) | `frc_julian_fields_to_unix_ms` |
| plain ASCII of any text this library writes | `frc_to_ascii` |

## 4. File map

```
include/fractonica.h     THE public contract (one header)
src/frc_internal.h       shared internals, 11 sections; every .c includes only this
src/frc_big.h            384-bit unsigned integer (the ladder only)
src/frc_math.c           §2 of internal.h: 128-bit mul / add / sub / cmp / div, muldiv, floor_div
src/frc_catalogue.c      SPEC §2.2, §7.2 anchor rule, §14 table: FRC_SAROS_141, init, position <-> time, frc_i_instant
src/frc_hierarchy.c      SPEC §3.1, §3.4, §15.1: factors, prefixes (64-bit and limbs), level names, the nine units
src/frc_cycle.c          SPEC §3.2, §6: frc_i_extent, THE cycle fraction, boundary instants, cycles, thresholds
src/frc_glyph.c          SPEC §4, §9.1: glyph states, trits, path and segments, bounds
src/frc_odometer.c       SPEC §5: odometer at an instant / of a phase; frc_i_odometer_* helpers
src/frc_sync.c           SPEC §7: solar sync, its odometer, Pentads (clock side)
src/frc_notation.c       SPEC §15.1–15.3, §7.5 wording: dates, parser, ordinals, cycle names, Pentad names
src/frc_span.c           SPEC §15.4–15.5: spans, parts, signed spans, a span in one unit, when a span's text changes (countdown and since), rates, per-Blink
src/frc_pure.c           the pure wordings: counts (signed too), a level in a unit, rates of all 63 rungs in pB, light spans and their word, level kinds, places, children, parents, P with its factors
src/frc_ladder.c         SPEC §16: intervals, periods, frequencies, lengths, media, bands, devices, quantity text, doubles
src/frc_big.c            the big integer behind frc_ladder.c
src/frc_civil.c          Gregorian translation, ISO text, frc_civil_validate, SPEC §2.3 Julian repair
src/frc_earth.c          Earth-mode wordings: civil text forms, durations, frc_parse_civil
src/frc_text.c           §7 of internal.h: text writer, Greek alphabet, UTF-8 decode, frc_to_ascii
src/frc_arc.c            arc geometry, frc_sin / frc_cos (entirely inside FRC_NO_FLOAT)

tests/frc_test.h         the harness: CHECK, CHECK_EQ_I64/U64, CHECK_STR, TEST_SECTION, TEST_SUMMARY
tests/test_*.c           one program each; see section 7
tests/vectors/*.h        GENERATED oracle vectors; never edit by hand
tools/oracle/            the generators (node), README.md, LADDER.md, check-clock-vectors.c;
                         generate-pure.mjs needs SPEC.md only, the others the frozen reference
tools/check-cross.sh     `make check-cross`
tools/check-progmem.sh   `make check-progmem`
docs/SPEC.md             the specification this version implements (a copy; see section 9)
docs/DESIGN.md           the arithmetic design and its bounds
docs/GUIDE.md            the technical guide: guarantees, sizes, API map, build, integrations, known gaps
docs/hero.jpg            the picture on the front page (a web-client screenshot)
README.md                the GitHub front page; details live in docs/GUIDE.md
Makefile                 reference build, tests and proofs
CMakeLists.txt           ESP-IDF component + ordinary CMake library
Package.swift            SwiftPM module CFractonica
library.json             PlatformIO manifest
idf_component.yml        ESP-IDF component manifest
```

Which source calls which (from the objects' symbol tables; there are no cycles):

| Source | Uses |
|---|---|
| `frc_math`, `frc_big`, `frc_arc` | nothing |
| `frc_text` | `frc_math` (the integer power) |
| `frc_catalogue` | `frc_math` |
| `frc_hierarchy` | `frc_text` |
| `frc_civil` | `frc_math`, `frc_text` |
| `frc_earth` | `frc_civil`, `frc_text` |
| `frc_cycle` | `frc_catalogue`, `frc_hierarchy`, `frc_math`, `frc_text` |
| `frc_glyph` | `frc_cycle` |
| `frc_odometer` | `frc_cycle`, `frc_glyph`, `frc_math` |
| `frc_sync` | `frc_catalogue`, `frc_cycle`, `frc_math`, `frc_odometer` |
| `frc_notation` | `frc_cycle`, `frc_hierarchy`, `frc_text` |
| `frc_span` | `frc_cycle`, `frc_hierarchy`, `frc_math`, `frc_text` |
| `frc_pure` | `frc_hierarchy`, `frc_math`, `frc_span`, `frc_text` (no `frc_big`: 128 bits and decimal limbs are enough) |
| `frc_ladder` | `frc_big`, `frc_catalogue`, `frc_hierarchy`, `frc_math`, `frc_text` |

A linker with section garbage collection keeps only what the application
calls: a firmware that reads a date and formats it does not carry the ladder.

## 5. Internal helpers and when to use them

`src/frc_internal.h` documents each one with its contract; this is the index.

| Need | Use | Mind |
|---|---|---|
| read a flash table | `FRC_PGM_U8/U16/U32/U64/I64/CHAR(&table[i])` | pass the **address**; a direct read works on the host and returns garbage on AVR / ESP8266 (`make check-progmem` fails on one). There is no pointer accessor: the core keeps no table of pointers |
| a string list in flash | packed list + `frc_i_packed_nth`, `frc_i_text_nth` | no pointer tables (they cost relocations / RAM). A new function that takes a flash address must be added to `SINKS` in `tools/check-progmem.sh` |
| range-check an enum | `FRC_I_BELOW(value, count)` | one unsigned compare; works for signed and unsigned enum ABIs |
| exact `a·b` | `frc_i_mul64(a, b, &out)` | 128-bit values travel by address, never by value |
| does `a·b` fit 64 bits | `frc_i_mul64_fits(a, b, &product)` | never `a > UINT64_MAX / b` |
| `baseⁿ` | `frc_i_pow_u64` | 0 when it does not fit; the one integer power of the core |
| `floor(a·b/c)`, `ceil(a·b/c)` | `frc_i_muldiv_floor`, `frc_i_muldiv_ceil` | prove `a·b/c < 2⁶⁴` at the call site (usually one factor `< c`) |
| 128/64 division | `frc_i_div128`, `frc_i_div128_ceil` | requires `n->hi < d` |
| 128-bit sums and compare | `frc_i_add128(&a, &b)` (in place), `_add128_64`, `_sub128`, `_cmp128` | |
| floor division of a signed value | `frc_i_floor_div` | C `/` truncates; days before 1970 and syncs before the anchor need floor |
| validate catalogue + level | `frc_i_extent` | every clock function starts here |
| any reading at an instant | `frc_i_fraction_at` → `frc_i_fraction_t` | prefer it over `frc_position_at` + own math; it sets `closing` |
| phase / state on a grid of `w` per quarter | `frc_i_fraction_phase`, `frc_i_fraction_step` | `1 ≤ w < 2⁶²` |
| when the current grid state ends | `frc_i_fraction_step_end` | already strictly-after and closing-aware |
| instant of a boundary | `frc_i_time_at_total`, `frc_i_time_at_quarter`, `frc_i_time_after_total` | all ceil; `FRC_TIME_NONE` beyond the catalogue |
| instant of `k + r/d` Saros | `frc_i_instant` | requires `r < d` |
| write text | `frc_i_text_init` … `frc_i_text_finish`; errors via `frc_i_text_error` | overflow is sticky; code points are written whole |
| numbers in text | `frc_i_text_u64`, `_u64_pad`, `_u64_grouped`, `_i64_super`, `frc_i_text_decimal` (value / 10ᵈ, trailing zeros dropped) | |
| a rounded value with every digit kept, "310.9", "1.000" | `frc_i_text_mantissa` | `r` already has exactly `digits` digits |
| "2.454 × 10⁻²⁵ " (the unit follows) | `frc_i_text_power_of_ten` | THE power-of-ten form; the ladder and the pure wordings share it |
| Greek letters | `frc_i_greek_cp`, `_index_of_cp`, `_index_of_ascii`, `frc_i_text_unit_symbol`, `frc_i_text_level_name` ("ο2") | |
| a span inside a longer text | `frc_i_text_span` | exactly the text of `frc_format_span` |
| a `frc_frac_t` from an exact fraction of a Blink | `frc_i_blink_fraction` | the only cut whose text is the exact span's (ties); `n < m < 2¹²⁶` |
| Blinks in one cycle of a level ≤ 12 | `frc_i_level_blinks` | 0 below the Blink |
| odometer digits from a step | `frc_i_odometer_steps`, `frc_i_odometer_fill` | shared by level and sync odometers |
| exact prefixes beyond 64 bits | `frc_i_prefix_limbs` (base 10⁹) or `frc_i_big_t` | |
| `P(to) / P(from)`, e.g. the cycles per Blink of a rung past level 35 | `frc_i_ratio_limbs` (base 10⁹) | `from ≤ to ≤ 63` |
| a number in limbs to a few digits, half to even | `frc_i_limbs_round` | gives `r` and the decimal exponent for `frc_i_text_power_of_ten` |
| the weekday of a date (Earth wording) | `frc_i_civil_weekday` | month 1..12, \|year\| <= 290000; section 11 |
| the speed of light | `FRC_I_LIGHT_SPEED` | the same constant as `frc_medium_speed(FRC_MEDIUM_VACUUM)` |
| numbers beyond 128 bits | `frc_big.h` | modulo 2³⁸⁴, **no overflow report**: state the bound |

## 6. Recipes

### Add a public function

1. Decide which `.c` owns it (section 4). Declare it in `include/fractonica.h`
   in the matching section, with a doc comment that states arguments, units,
   rounding, every error status, and behaviour at the catalogue's edges.
2. Implement it. Start from `frc_i_fraction_at` for anything read at an
   instant; reuse the text writer for anything that formats. Validate
   arguments before the instant (argument errors win over range errors).
   Float-only functions go inside `#ifndef FRC_NO_FLOAT` in both header and
   source.
3. A helper shared by two `.c` files becomes `frc_i_*`, declared in
   `frc_internal.h` in the right section; otherwise keep it `static`.
4. A new constant table is `static const … NAME[…] FRC_PROGMEM`, read through
   `FRC_PGM_*(&NAME[i])`; `make check-progmem` fails on any other use. A new
   function that receives the address of flash text goes into `SINKS` in
   `tools/check-progmem.sh`, after you have checked that it reads through
   the accessor.
5. Tests: the arithmetic against the oracle or SPEC values in the module's
   `tests/test_*.c`; one check per documented sentence in
   `tests/test_header.c`; a float API test must be guarded by `FRC_NO_FLOAT`.
6. A new `src/*.c` must also be added to `FRACTONICA_SOURCES` in
   `CMakeLists.txt` (the CMake configure step fails if the list and the
   directory disagree). The Makefile, SwiftPM and PlatformIO pick it up alone.
7. `make check`, then `make check-all` and `make check-cross` if you have the
   compilers (a struct passed by value or a zero-fill loop shows up only there). `make check-api` fails if a declared function is not defined or
   a non-`frc_` symbol is exported.
8. Add a row to section 3 of this file.

### Add a unit

The nine units are SPEC §15.1; a tenth is a SPEC change and a contract change
(`frc_unit_t`, `FRC_UNIT_COUNT`, `frc_date_t` and its written forms). Agree the
SPEC and header change first. Then everything that encodes the ladder:

- `include/fractonica.h`: `frc_unit_t`, `FRC_UNIT_COUNT`, `frc_date_t` (one
  field per unit), the `frc_unit_blinks` / `frc_parse_date` comments.
- `src/frc_hierarchy.c`: `UNIT_LEVELS`, `UNIT_PARTS`, `frc_i_unit_names`,
  `frc_level_kind`.
- `src/frc_internal.h`: `FRC_I_BLINK_LEVEL` if the finest unit moves.
- `src/frc_notation.c`: `fields_of_date` / `date_of_fields`, the bases
  (ordinal from 1 vs count from 0), `put_minor_start`, the parser's letters.
- `src/frc_span.c`: the rate presets (`RATE_UNITS`, `RATE_COUNTS`), the
  decimal rule of the finest unit.
- `tools/oracle/generate-clock.mjs` (and the reference it drives), then the
  vectors; `tests/test_notation.c`, `test_span.c`, `test_spec.c`, `test_header.c`.
- `src/frc_pure.c` needs no change (it reads the unit tables), but
  `tools/oracle/generate-pure.mjs` asserts the nine units of SPEC §15.1 and
  `tests/test_pure.c` counts the valid (level, unit) pairs.

### Add a catalogue (another Saros series)

At run time, in the application, no library change:

```c
/* Unix MILLISECONDS of each greatest eclipse, strictly increasing, in flash. */
static const int64_t MY_SERIES_MS[] FRC_PROGMEM = { /* E[0], E[1], ... */ };

frc_catalogue_t cat;
if (frc_catalogue_init(&cat, MY_SERIES_MS,
                       sizeof MY_SERIES_MS / sizeof MY_SERIES_MS[0], 145) != FRC_OK) { /* bad table */ }
```

- The values must be **Unix** milliseconds derived as SPEC §2.1 (TT → UTC with
  leap seconds; ΔT before 1972), not TT (pitfall 1). Dates before 1582-10-15
  from a source that stores Julian fields as Gregorian need
  `frc_julian_fields_to_unix_ms`, once (pitfall 3).
- `frc_catalogue_init` picks the sync anchor by SPEC §7.2 (first ordinal
  ≡ 0 mod 4 on or after 1972-01-01). A descriptor filled by hand promises
  what `init` checks.
- The table must outlive the descriptor and carry `FRC_PROGMEM`.

Built in, as a second `FRC_SAROS_nnn`:

1. Add the table and the `const frc_catalogue_t` to `src/frc_catalogue.c`,
   next to Saros 141 (the descriptor is a plain `const` object; only the table
   carries `FRC_PROGMEM`). Declare it `extern` in `fractonica.h`.
2. Extend a generator in `tools/oracle/` to emit the reference's values for
   that series and test all of them (`tests/test_foundation.c`, "catalogue:
   Saros 141 table" is the model). `tests/test_properties.c` runs
   `check_catalogue(catalogue, name)` once per catalogue: add a call for yours
   and it gets the tiling and next-instant checks.
3. `tools/check-cross.sh` allows exactly one constant outside AVR flash,
   `FRC_SAROS_141`: add the new descriptor's name there.
4. `make size`: 8 bytes of rodata per eclipse.

## 7. Tests and checks: what each proves

`make test` builds `build/libfractonica.a` and runs every `tests/test_*.c`.
`make test-NAME` runs one (`make test-spec`).

| Program | Holds the core against |
|---|---|
| `test_spec` | SPEC.md typed in by hand, public API only: §12 vectors A, B, C, anchor; §15.6; §16.5; the tables of §3.3, §4.1, §7.5, §9.1, §15, §16; the header's QUICK START |
| `test_header` | every documented edge of `fractonica.h`: invalid arguments, the closing instant, the cases the comments single out |
| `test_foundation` | `frc_math`, `frc_catalogue`, `frc_hierarchy`, `frc_cycle`, `frc_text` against `oracle_foundation.h`, SPEC, `__int128`, and identities |
| `test_glyph` | glyph states, paths, odometers against `oracle_clock.h`, `oracle_foundation.h`, `oracle_geometry.h` |
| `test_sync` | sync, sync odometer, Pentads against `oracle_clock.h`; every whole sync of Saros 141; hand-built series for the rare edges (closing exactly on a sync, a Pentad that would start at the closing instant, results that do not fit 64 bits) |
| `test_notation` | dates, parser (incl. fuzz), ordinals, cycle names, Pentad names against `oracle_notation.h` |
| `test_span` | spans, parts, rates, per-Blink against `oracle_notation.h` (and the decades of three significant digits by hand); the time grain (a cycle's start → end is its length, at every level ≤ 12, on three catalogues); exact rounding on a table where every tie is an instant; both next-change functions against the formatter at every microsecond of several windows, in chains and from far away, and their computed guess within 3 µs of the answer (`frc_i_span_change_guess`) |
| `test_pure` | the pure wordings against `oracle_pure.h` (and the words, signed counts, places, children, parents and prefix factors of every level against rules restated with libc) (every count around a digit group, every (level, unit) pair, every level × five precisions, every level × medium with and without the word, 624 speeds on the ties, the quarter Blinks and the carries), a whole number of quarter Blinks at every level against `frc_format_span` of exactly that span, against `frc_format_per_blink` / `frc_format_span` / `frc_format_prefix`, the rounding of a 54-digit number held directly, every buffer size |
| `test_ladder` | `frc_big`, the ladder against `oracle_ladder.h`, the §16.1 table byte for byte, doubles against libc |
| `test_civil` | civil time against `oracle_civil.h` and 5.6 million consecutive days |
| `test_earth` | `frc_civil_validate` on every day of years -400..2400, `frc_format_civil_text` and `frc_format_duration` against rules restated with libc (200,000 random instants, 300,000 durations), `frc_parse_civil` by round trips, every grammar edge (accepted and refused) one by one, a fuzz run on exact-length heap copies with a round-trip oracle, every buffer size |
| `test_arc` | `frc_sin` / `frc_cos` against libm, arcs against `oracle_geometry.h` |
| `test_properties` | the promises at 20,000 random instants and every eclipse, on two catalogues: next instants are the first microsecond of the new state, tilings, round trips, monotony |

| Target | Proves | Needs |
|---|---|---|
| `make check-includes` | `src/` includes `<stdint.h>` and `<stddef.h>` only | grep |
| `make check-progmem` | every `FRC_PROGMEM` table is read through `FRC_PGM_*` only (or handed to a listed flash sink); then plants six direct reads in a copy of `src/` and requires the check to fail on each | bash, grep, sed, awk |
| `make check-api` | every function the header declares is defined; only `frc_*` / `FRC_*` exported | host cc, nm |
| `make check-symbols` | the library references no libc / libm / 128-bit runtime symbol | host cc, nm |
| `make check-nofloat` | library and tests with `-DFRC_NO_FLOAT`: the clock is integer-only | host cc |
| `make check-harvard` | the `pgm_read` code path (`FRC_TEST_FAKE_HARVARD`) computes the same. It does **not** find a table read without its accessor: `check-progmem` does | host cc |
| `make check-sanitize` | tests under ASan + UBSan: no overflow, no out-of-bounds, no UB | host cc |
| `make check` | all of the above plus `make test` | host cc |
| `make check-32` | strict syntax check for armv7-m and riscv32 (32-bit `int`, `size_t`) | LLVM clang |
| `make check-freestanding` | **the no-dependencies proof**: wasm32 objects at -O2/-Os/-O3 link into a module with no imports | LLVM clang, wasm-ld |
| `make size` | flash size per module on armv7-m `-Os`; fails unless `.data` + `.bss` = 0 | LLVM clang, llvm-size |
| `make check-all` | `check` + `check-32` + `check-freestanding` + `size` | LLVM |
| `make check-cross` | real cross compilers that are installed (arm-none-eabi-gcc for Cortex-M3 and M0, ESP32 Xtensa and RISC-V gcc, clang riscv32 and wasm32, avr-gcc): strict build at `-O0`, `-Og`, `-Os`, `-O2`, `-O3` and without floats; no symbol outside `frc_*` and the compiler's 64-bit runtime (so no `memcpy` / `memset`); AVR tables in `.progmem.data`; an overridden `FRC_PROGMEM` on AVR either stops the build or reads flash; on ESP32 (Xtensa, RISC-V) no stack frame of 300 bytes or more (`STACK_LIMIT`), and none of variable size | whatever is installed (the ESP-IDF toolchains are found under `~/.espressif` too); missing ones are skipped |
| `make test-wasm32` | the whole test suite as 32-bit WebAssembly under node | emcc, node |
| `make vectors` | regenerates `tests/vectors/` | node, the frozen reference |

Variables: `CC`, `AR`, `CFLAGS`, `TEST_CFLAGS`, `DEFS`, `BUILD`, `LLVM_BIN`,
`WASM_LD`. Each variant builds into its own `BUILD` directory.

CMake runs the same test programs: `cmake -S . -B build/cmake
-DFRACTONICA_BUILD_TESTS=ON && cmake --build build/cmake && ctest --test-dir
build/cmake`. `swift build` compiles the library as the module `CFractonica`.

## 8. Oracle vectors

`tests/vectors/*.h` are generated from the **frozen JavaScript reference**
(exact BigInt rationals), which is not part of this repository: it is the
read-only checkout `../reference/js` beside it. Never edit a vector header by
hand, and never edit the reference. One generator stands alone:
`generate-pure.mjs` has no reference to ask (the pure wordings are the
core's own), so it states their rules once more on BigInt rationals and reads
its tables (prefixes, rung names, units, media) from `docs/SPEC.md`.

```
make vectors                                      # all generators; needs ../reference/js (SPEC: docs/SPEC.md)
node tools/oracle/generate-clock.mjs --check      # regenerate in memory, exit 1 if the files differ
node tools/oracle/generate-pure.mjs  --check      # the same for oracle_pure.h; needs docs/SPEC.md only
node tools/oracle/generate-foundation.mjs --spec docs/SPEC.md --reference ../reference/js/fractonica.js --out tests/vectors
node tools/oracle/generate-ladder.mjs     --spec docs/SPEC.md --reference ../reference/js/fractonica.js --out tests/vectors
node tools/oracle/generate-geometry.mjs   --reference ../reference/js/fractonica.js --out tests/vectors
FRACTONICA_REFERENCE=../reference/js node tools/oracle/generate-clock.mjs
```

| Generator | Writes | Read by |
|---|---|---|
| `generate-clock.mjs` | `oracle_clock.h`, `oracle_notation.h` | `test_glyph`, `test_sync`, `test_notation` (both), `test_span` |
| `generate-foundation.mjs` | `oracle_foundation.h` | `test_foundation`, `test_glyph` |
| `generate-ladder.mjs` | `oracle_ladder.h`, `oracle_civil.h` | `test_ladder`, `test_civil` |
| `generate-geometry.mjs` | `oracle_geometry.h` | `test_glyph`, `test_arc` |
| `generate-pure.mjs` | `oracle_pure.h` | `test_pure` |

- Output is deterministic (fixed seeds): regenerating without a change gives
  the same bytes. Commit a generator and its headers together.
- The generators assert SPEC §12, §15.6, §16.5 and the §14 table before
  writing anything; a failed assertion writes nothing.
- New samples go at the **end** of a table so existing indices keep their
  meaning. Layouts: `tools/oracle/README.md` (clock, notation), `LADDER.md`.
- The generators look for the specification in `docs/SPEC.md` first, then in
  a `SPEC.md` beside or above the repository; `--spec <path>` names another.
- Where the reference rounds doubles and the core is exact, the generator
  carries the exact rule and lists the differing texts ("Exactness wins").

## 9. SPEC pitfalls (§13) and where each is held

| # | Pitfall | In the code | Tested by |
|---|---|---|---|
| 1 | Mixed time scales (TT vs Unix) | the table in `frc_catalogue.c` is the **Unix** table of SPEC §14; the API has no TT anywhere | `test_foundation` "catalogue: Saros 141 table" (all 70 values against the reference's `unixTimestamps`); `test_spec` "S12 vector A" |
| 2 | Mean Saros | `frc_catalogue.c` uses each `I_k`; `frc_interval_mean` exists only as a ladder scale | `test_sync` "SPEC S7.2: per-sync offsets and the lap"; `test_properties` "the uneven catalogue"; `test_ladder` "frc_interval_at: every Saros 141 interval" |
| 3 | Julian dates before 1582 | `frc_julian_fields_to_unix_ms` (applied by the table builder, once) | `test_civil` "oracle_julian…", "SPEC S2.3, typed in…" |
| 4 | Float precision | no floats: position is `ordinal + num/den`, only the fraction is multiplied by `P` | `make check-nofloat`; `test_glyph` "deep levels 14..21 against the clock oracle"; `test_foundation` "cycles: a 90-eclipse series at level 21" |
| 5 | Dot branch | `frc_glyph_from_index`: `dotted == (derivative < 0)` | `test_glyph` "from_index: the table of S4.1…"; `test_spec` "S4.1 states" |
| 6 | Depth vs level | API takes levels; `frc_siblings(L) = S[(L−1) mod 8]` | `test_foundation` "hierarchy: factors and siblings"; `test_spec` "S3.3 period lengths" |
| 7 | Final clipped cycle | `frc_cycle_t.clipped`, `FRC_TIME_NONE` beyond the catalogue, no "next" at the closing instant | `test_foundation` "cycles: first and last cycle of every level"; `test_glyph` "the closing instant of the catalogue"; `test_header` "frc_next_threshold"; `test_sync` "the partial syncs at the catalogue's edges"; `test_properties` "outside the catalogue" |
| 8 | Stale table (a new leap second) | `SAROS_141_MS` holds TAI − UTC at 37 s from ordinal 23 on | the table is compared with the reference in `test_foundation`; if the upstream table is re-derived, update `SAROS_141_MS`, `docs/SPEC.md` §14 and regenerate the vectors |

SPEC §13 speaks of tests named `pitfall_1_…` in another implementation's
repository; this core holds the same items in the tests listed above.

`docs/SPEC.md` is a copy of the organisation's `SPEC.md` at the version in
`FRC_SPEC_VERSION`. When the specification changes: replace the copy, bump
`FRC_SPEC_VERSION`, regenerate the vectors, and make `test_spec` true again.

## 10. Common mistakes

- **Passing a depth.** "Depth 7" is level 6. `frc_odometer_at(cat, t, 6, …)`.
- **Milliseconds as `frc_time_t`.** Readings land in 1970. Use `FRC_TIME_FROM_MS`.
- **Polling.** Sleep until `next_change` / `next.time` and redraw; the instant
  returned already shows the new state. `FRC_TIME_NONE` means stop.
- **Comparing a returned instant with the exact boundary.** It is the ceiling:
  up to 1 µs later. SPEC prints milliseconds; tests compare after rounding.
- **Deriving sync digits or quarter from `frc_sync_t.phase`, `start`, `end`.**
  They come from the exact boundaries; use `frc_sync_odometer` and
  `frc_sync_t.quarter`.
- **Assuming `frc_sync_at` works everywhere in the catalogue.** In the first
  and last partial sync it is `FRC_ERR_RANGE`; `frc_pentad_at` still answers.
- **Treating `FRC_TIME_NONE` as a time.** It is `INT64_MIN`. Not 0 (0 is 1970).
- **Reading a table without `FRC_PGM_*`.** Passes every test on every host and
  returns garbage on AVR / ESP8266. `make check-progmem` (part of `make
  check`) reads the source and fails on it. What it cannot follow is a flash
  address inside a function that received it as a pointer: those functions
  are listed in `tools/check-progmem.sh` (`SINKS`), are few and short, and
  are the part to review by eye.
- **Overriding `FRC_PROGMEM` alone.** The tag and the way of reading belong
  together: define `FRC_PROGMEM_IS_HARVARD` (1 or 0) with it. On AVR and
  ESP8266 the header refuses an override without it.
- **`= {0}`, a struct assignment, or a struct passed by value in `src/`.** May
  become `memset` / `memcpy` at one optimisation level on one target
  (riscv32 `-Os`, Cortex-M0, xtensa `-O0`). `make check-freestanding` builds
  three levels and `make check-cross` five for that reason.
- **Expecting `frc_span_between` to report sub-microsecond differences
  between boundaries.** An instant is a whole microsecond; a span within that
  grain of a whole number of quarter Blinks is that number.
- **Reading `frc_span_of_level` as exact below the Blink.** From level 13 it
  is cut to 2⁻³² Blink and from level 24 it is the zero span with `FRC_OK`;
  use `frc_level_per_blink` for the ratio, or `frc_format_level_rate` for its
  text at any level.
- **Composing a pure-mode text in a client** ("58 500" + "ν", a rate from
  `frc_format_prefix`, a length from `frc_length`). Call the function of
  "Pure wording": grouping, rounding and the choice of rung live in the core.
- **Using `frc_position_to_double` or `frc_cycle_t.phase` for logic.** They are
  for display; decisions use indices, quarters and instants.
- **Levels 19..21.** Cycles are shorter than a microsecond: `start` may equal
  `end` for a cycle asked by index. Read the note on `frc_cycle_t`.
- **Forgetting `FRC_NO_FLOAT` guards** in a test that calls a float API:
  `make check-nofloat` will not compile it.
- **Editing `tests/vectors/`** instead of the generator.
- **Calling the solar sync "a day counter".** It is 19 Helions ≈ 4 days with a
  drift; Pentads are the day-like parts.

## 11. Versioning and the files that carry it

Library version 0.2.0, SPEC version 2.2.0. A version bump touches:
`FRC_VERSION*` and `FRC_SPEC_VERSION` in `include/fractonica.h` (`test_spec`
checks both), `project(… VERSION …)` in `CMakeLists.txt`,
`library.json`, `idf_component.yml` (CI checks that these agree), this file,
`docs/GUIDE.md`, the version line of `README.md`, and `CHANGELOG.md`:
every added function, every changed layout and every changed text (old → new), for the clients. `Package.swift` carries
no version. Before 1.0 a minor version may change the
API; a patch version never changes a reading.
