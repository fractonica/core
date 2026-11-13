# Changelog

Library versions of fractonica-core. Before 1.0 a minor version may change
the API (README.md, "Versioning"); every change of a text or a layout is
listed here so that a client can follow it.

## 0.2.0

Implements SPEC 2.2.0, as 0.1.0 did. No clock reading changed: every cycle,
glyph, odometer, sync, date and instant is what 0.1.0 gave.

### API additions

Pure wording (`frc_pure.c`): `frc_format_level_kind`, `frc_format_count_signed`,
`frc_format_cycle_place`, `frc_format_level_children`,
`frc_format_level_in_parent`, `frc_format_unit_in_parent`,
`frc_format_prefix_factors` (and `FRC_PREFIX_FACTORS_SIZE`, 429, the buffer
that holds every level), `frc_format_light_word`.

Notation and spans: `frc_format_date_term`, `frc_format_span_signed`,
`frc_format_span_in_unit`, `frc_span_next_change` (when a countdown's text
changes), `frc_span_next_change_since` (the same for a span that grows: an
elapsed time, or a signed span past its zero), `FRC_PENTAD_NUMERAL` (a
`frc_pentad_form_t`).

Clock: `frc_time_at_phase` (the instant a cycle reaches a phase),
`frc_time_at_sync_phase` (the instant a sync reaches a phase),
`frc_glyph_at_quarter` (the glyph at a threshold).

Ladder: `frc_band_edges`.

Earth wording (new source `src/frc_earth.c`): `frc_format_civil_text` with
`frc_civil_form_t` (14 forms), `frc_format_duration`, `frc_civil_validate`,
`frc_parse_civil`.

### ABI changes

- `frc_sync_t.helion` (`uint8_t`, 0..18, after `quarter`): the running
  Helion's place in the sync.
- `frc_pentad_t.first` (`uint8_t`, 0 5 10 15, after `helions`): the place in
  the sync of the Pentad's first Helion.
- Where `int64_t` is 8-aligned (64-bit hosts, wasm32, ESP32, ARM EABI) both
  fill what was padding: no other offset and neither size changed (64 and 24
  bytes). Where it is 1-aligned (AVR) every later field moved by one byte
  (`frc_sync_t.phase` and after; `frc_pentad_t.start` from 7 to 8): rebuild
  everything built on 0.1.0 there.

### Changed texts (0.1.0 -> 0.2.0)

| Function | 0.1.0 | 0.2.0 |
|---|---|---|
| `frc_format_per_blink(num, den)` with num < den and num dividing den (design ruling 3) | `0.5 pB` (1/2), `0.2 pB` (1/5), `0.1 pB` (1/10), `0.04 pB` (1/25), `0.333 pB` (1/3) ... | `1 per 2 Blinks`, `1 per 5 Blinks`, `1 per 10 Blinks`, `1 per 25 Blinks`, `1 per 3 Blinks` ... (N = den / num exactly, grouped) |
| `frc_format_level_rate` level 10 (the Flavon), any `digits` | `0.04 pB` | `1 per 25 Blinks` |
| `frc_format_level_rate` level 11 (mu), any `digits` | `0.2 pB` | `1 per 5 Blinks` |
| `frc_format_span` (and every text that writes a span) of a non-zero span below 1/10000 Blink | `0ν` | `<0.001ν` |
| `frc_format_span` of a negative span from 1/10000 to 1/1000 Blink | `−<0.001ν` | `<0.001ν` (no sign before a text that shows no digit) |
| `frc_format_band(FRC_BAND_NONE)` | `—` (U+2014) | `no band` (design ruling 4) |

`0ν` is now the zero span alone; `frc_span_of_level` still gives the zero span
from level 24 on.

Texts of functions that are new in 0.2.0 but changed after the first
unverified draft of this release (for clients that followed that draft):

| Function | draft | 0.2.0 |
|---|---|---|
| `frc_format_civil_text`, every form that shows a year but `FRC_CIVIL_ISO_DATE`, a year below 0 | `15 MAR -0044` (ASCII hyphen-minus) | `15 MAR −0044` (U+2212); `FRC_CIVIL_ISO_DATE` stays `-0044-03-15` (ISO 8601) |
| `frc_format_civil_text(FRC_CIVIL_OFFSET)` with \|offset\| >= 86400 s | `UTC+24`, `UTC−596523:14` | `FRC_ERR_ARG` and `""` (the offsets `frc_parse_civil` reads back) |
| `frc_parse_civil` | a tab read as a space; `THU8 OCT 2026` and `8 OCT 2026T12:00` accepted; `THU , 8 OCT 2026` refused | spaces are ASCII spaces only; a weekday needs a separator (spaces, a `,` or both, spaces on either side of the `,`); `T` only after an ISO date |

The longest text of `frc_format_civil_text` is now 32 bytes + NUL (it was 30).
