# fractonica-core

The shared core of the Fractonica clock: an eclipse-anchored harmonic clock,
its glyphs, its notation and its spectral ladder, in dependency-free C99.

One header, sixteen source files, no build system required. The same code
runs in the native iOS / macOS app (as a SwiftPM C target), in the web app (as
WebAssembly) and in ESP32 firmware, and gives the same reading on all of them,
to the microsecond.

- Library version **0.2.0**, implements **SPEC 2.2.0** ([docs/SPEC.md](docs/SPEC.md)).
- The public contract is [include/fractonica.h](include/fractonica.h). It
  documents every function with its units, rounding, errors and edge cases;
  `tests/test_header.c` holds those documented edges one by one.
- Working on the code? Start with [AGENTS.md](AGENTS.md). The arithmetic is
  explained in [docs/DESIGN.md](docs/DESIGN.md). What changed between
  versions, texts and layouts included, is in [CHANGELOG.md](CHANGELOG.md).

## Guarantees

| | |
|---|---|
| **No dependencies** | Pure C99. The sources include `<stdint.h>` and `<stddef.h>` only: no libc, no libm, no compiler 128-bit runtime. Compiled freestanding for wasm32, the whole library links into a module that imports nothing (`make check-freestanding`). With the real ESP32 (Xtensa, RISC-V), Cortex-M3 / M0 and AVR compilers no object references `memcpy`, `memset` or any other libc symbol at `-O0`, `-Og`, `-Os`, `-O2` or `-O3` (`make check-cross`). |
| **No heap, no state** | No allocation, no recursion, no mutable static or global data. Every function is re-entrant and writes only to caller-provided storage. `.data` + `.bss` = **0 bytes** (`make size` fails otherwise). |
| **Integer clock** | Every reading is exact integer arithmetic (64-bit with portable 128-bit intermediates). Floating point exists only in an optional convenience API; build with `-DFRC_NO_FLOAT` and it is gone (`make check-nofloat`). |
| **Tables in flash** | Every constant table is `const` and tagged `FRC_PROGMEM`; on Harvard targets (AVR, ESP8266) it is read through `pgm_read_*` and not copied to RAM. `make check-progmem` fails on a table read in any other way. |
| **Exact boundaries** | A boundary instant is returned as the first microsecond at or after the exact rational instant. "Sleep until `next_change`, then redraw" always shows the new state and never stalls. The span between two returned boundaries is the exact count of units (`frc_span_between` of a Preon's start and end is `1ι`). |
| **Strict** | Compiles with `-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` for the host, ARM Cortex-M3 and M0, ESP32 (Xtensa and RISC-V), wasm32 and AVR, at `-O0`, `-Og`, `-Os`, `-O2` and `-O3`. |

Measured size (`make size`: ARM Cortex-M3, thumb2, clang `-Os`, whole library):

| | bytes |
|---|---|
| code (`.text`) | 33,106 |
| constant tables (`.rodata`) | 2,075 |
| **flash total** | **35,181** |
| RAM (`.data` + `.bss`) | **0** |

The same, whole library at `-Os`, with the compilers the devices are built
with (`make check-cross` builds these objects; code and constants together):

| Target | Compiler | bytes |
|---|---|---|
| ESP32 (Xtensa) | xtensa-esp32-elf-gcc 15.2 | 36,892 |
| ESP32-C3 / C6 (RISC-V, rv32imc) | riscv32-esp-elf-gcc 15.2 | 42,236 |
| ARM Cortex-M3 | arm-none-eabi-gcc 13.3 | 30,353 |
| ARM Cortex-M0 | arm-none-eabi-gcc 13.3 | 31,504 |
| AVR ATmega2560 | avr-gcc 9.4 | 87,689 |

A linker drops what the application does not call. An ESP32 program that only
reads the date and formats it links 4,128 bytes of this library
(xtensa-esp32-elf-gcc 15.2 `-Os -ffunction-sections`, `--gc-sections`; the
64-bit division of libgcc not counted; measured for 0.1.0). The whole library
as a freestanding wasm32 module is 55,614 bytes at `-Os` (52,571 without the
float API). The largest stack frame of any function is 192 bytes on Xtensa,
208 on RISC-V and 200 on ARM (`-Os -fstack-usage`); there are no
variable-length frames, and `make check-cross` fails on a frame of 300 bytes
or more on the ESP32 compilers. A task's stack must hold a whole call chain,
not one frame: the deepest is `frc_span_next_change_since` at 944 bytes on
Xtensa (`frc_span_next_change` 928, `frc_span_between` 576,
`frc_format_light_span` 560, `frc_odometer_at` 544, `frc_sync_at` 528;
xtensa-esp32-elf-gcc 15.2 `-Os -fcallgraph-info=su`, the sum of the frames
along the deepest static path; calls through the path sinks not counted).

## Example

```c
#include <stdio.h>
#include "fractonica.h"

int main(void)
{
    const frc_catalogue_t *cat = &FRC_SAROS_141;         /* Solar Saros 141, built in */
    frc_time_t now = FRC_TIME_FROM_S(1791460800);        /* 2026-10-08T12:00:00Z, Unix time */
    frc_date_t date;
    frc_odometer_t helion;
    frc_glyph_t glyph;
    frc_civil_t civil;
    char text[96];

    if (frc_date_at(cat, now, &date) != FRC_OK) return 1;         /* outside the catalogue */
    frc_format_date(&date, FRC_DATE_SHORT, FRC_UNIT_PREON, text, sizeof text);
    printf("%s\n", text);

    frc_odometer_at(cat, now, 6, 2, 2, &helion);         /* level 6 = Helion, 2 strokes, 2 glyphs */
    frc_glyph_from_index(helion.digit[0], helion.strokes, &glyph);
    printf("glyphs %u %u, left value %d\n", helion.digit[0], helion.digit[1], glyph.value);

    frc_civil_from_time(helion.next_change, 0, &civil);  /* sleep until then, redraw */
    frc_format_civil(&civil, 0, text, sizeof text);
    printf("redraw at %s\n", text);
    return 0;
}
```

```
$ cc -std=c99 -Iinclude example.c src/*.c -o example && ./example
Delta Trifracton End ε#5 34η 29ι
glyphs 13 0, left value -3
redraw at 2026-10-08T12:00:33.051Z
```

Things to know before the first call (the header's opening comment has the rest):

- Time is `frc_time_t`: Unix **microseconds**. Feed the device clock straight in.
- The API takes **levels**, zero-based (level 6 is the Helion, "depth 7").
- Outside the catalogue (1613 .. 2857 for Saros 141) functions return
  `FRC_ERR_RANGE`; the clock never extrapolates.
- To draw a glyph, pass it to `frc_glyph_path` (move / line / cubic / dot
  commands in integer design units) or `frc_glyph_segments` (lines only).

## What is in the API

The header is the reference; [AGENTS.md](AGENTS.md) section 3 maps "I need X"
to the call. In one table:

| Part of `fractonica.h` | What it answers | Entry points |
|---|---|---|
| Catalogue and position (SPEC §2) | where an instant sits in the Saros series, exactly | `FRC_SAROS_141`, `frc_position_at`, `frc_time_at_position` |
| Hierarchy and cycles (§3, §6) | the cycle of any level at an instant, its quarter, its next threshold | `frc_cycle_at`, `frc_next_threshold`, `frc_prefix`, `frc_format_level_name` |
| Glyphs and odometer (§4, §5, §9) | what to draw, and when to redraw | `frc_odometer_at`, `frc_glyph_from_index`, `frc_glyph_path`, `frc_glyph_at_quarter` |
| Solar sync and Pentads (§7) | the four-day sync, its day-like parts, the places on the Sun's ring | `frc_sync_at` (`.helion`), `frc_pentad_at` (`.first`), `frc_format_pentad`, `frc_time_at_sync_phase` |
| Notation (§15) | dates, stamps, cycle names, spans, rates, when a span's text changes | `frc_date_at`, `frc_format_date`, `frc_format_date_term`, `frc_parse_date`, `frc_span_between`, `frc_format_span`, `frc_format_span_signed`, `frc_span_next_change`, `frc_span_next_change_since`, `frc_format_per_blink` |
| Pure wording | static facts of a level with no Earth unit in them | `frc_format_count`, `frc_format_count_signed`, `frc_format_level_in_unit`, `frc_format_level_rate`, `frc_format_light_span`, `frc_format_level_children`, `frc_format_prefix_factors` |
| Spectral ladder (§16) | period, frequency, length and band of all 63 rungs, in seconds, hertz and metres | `frc_period`, `frc_frequency`, `frc_length`, `frc_band`, `frc_band_edges`, `frc_format_quantity` |
| Arcs, civil time, text | drawing cycles as nested arcs, the Gregorian translation and the Earth-mode wordings, plain ASCII | `frc_arc_child`, `frc_civil_from_time`, `frc_format_civil`, `frc_format_civil_text`, `frc_format_duration`, `frc_parse_civil`, `frc_to_ascii` |

### Pure wording: no seconds, hertz or metres

The clients have a "pure" mode that shows no Earth unit. These functions word
what such a screen says about a level, from the hierarchy alone (no
catalogue, no Saros interval, no floating point). A client shows the text as
it comes: grouping, rounding and the choice of rung are the core's.

```c
char text[96];
frc_format_count(58500, text, sizeof text);                         /* "58 500" (U+202F between groups) */
frc_format_level_in_unit(6, FRC_UNIT_BLINK, text, sizeof text);     /* "58 500ν": a Helion in Blinks    */
frc_format_level_rate(6, 0, text, sizeof text);                     /* "1 per 58 500 Blinks"            */
frc_format_level_rate(10, 0, text, sizeof text);                    /* "1 per 25 Blinks" (ruling 3)     */
frc_format_level_rate(16, 0, text, sizeof text);                    /* "2 772 pB"                       */
frc_format_level_rate(29, 4, text, sizeof text);                    /* "1.420 × 10¹⁴ pB"                */
frc_format_light_span(6, frc_medium_speed(FRC_MEDIUM_VACUUM), 1, text, sizeof text);  /* "1η light"    */
frc_format_light_span(6, frc_medium_speed(FRC_MEDIUM_BOUNDED), 1, text, sizeof text); /* "24ι 1λ light" */
frc_format_light_span(6, frc_medium_speed(FRC_MEDIUM_AIR), 1, text, sizeof text);     /* "5.15ο light"  */
```

A length is said as the span light in vacuum needs to cover it,
`P(12) / P(level) · v / c` Blinks, exact and the same on every Saros
interval: a span (§15.4) from one Blink up, three significant digits of the
largest rung that fits below it. A rate is the exact `P(level) / P(12)`
cycles per Blink for every level 0..62, 42 digits at the last one.

## Build and test

The library needs no build system: compile `src/*.c` with `-Iinclude`. The
Makefile is the reference build; it runs the tests and proves the guarantees
above.

```
make test            # build build/libfractonica.a and run all 13 test programs
make check           # test + the host checks; run it before a commit
make check-all       # check + the LLVM cross checks + size
```

| Target | What it does | Needs |
|---|---|---|
| `make lib` | `build/libfractonica.a` with the strict flags | a C compiler |
| `make test`, `make test-NAME` | all tests, or `tests/test_NAME.c` alone | |
| `make check` | `check-includes`, `check-progmem`, `check-api`, `check-symbols`, `test`, `check-nofloat`, `check-harvard`, `check-sanitize` | |
| `make check-progmem` | every flash table is read through its `FRC_PGM_*` accessor (reads the source, then proves on planted mistakes that it catches them) | bash, grep, sed, awk |
| `make check-32` | strict syntax check for armv7-m and riscv32 | a full LLVM: clang with cross targets, `llvm-nm`, `llvm-size` (Apple's clang is not enough) |
| `make check-freestanding` | freestanding wasm32 link that must import nothing | LLVM and a working `wasm-ld` |
| `make size` | the size table above; fails unless RAM is 0 | LLVM |
| `make check-cross` | strict builds at five optimisation levels with every installed cross compiler (arm-none-eabi-gcc for Cortex-M3 and M0, the ESP-IDF Xtensa and RISC-V gcc, also when they are only under `~/.espressif`, clang riscv32 and wasm32, avr-gcc); no libc symbol may be referenced; the others are skipped | optional |
| `make test-wasm32` | the test suite as 32-bit WebAssembly under node | Emscripten, node |
| `make vectors` | regenerate the oracle vectors in `tests/vectors/` | node and the reference checkout, see [AGENTS.md](AGENTS.md) |

The tests hold the core against vectors generated from the exact (BigInt)
JavaScript reference implementation, against the numbers of SPEC.md typed in
by hand, and against its own promises at tens of thousands of instants.

With CMake:

```
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release -DFRACTONICA_BUILD_TESTS=ON
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
```

With SwiftPM: `swift build`.

## Using it

Sibling repositories (`ios`, `web`, `esp32`) consume this one as a git
submodule or as a sibling checkout. Nothing is generated: every integration is
"these sources, this include directory".

### Any C or C++ project

```
cc -std=c99 -Iinclude -c src/*.c        # then link the objects, or `make lib`
```

CMake:

```cmake
add_subdirectory(core)                              # this repository
target_link_libraries(app PRIVATE fractonica::core)
```

Options: `FRACTONICA_NO_FLOAT` (defines `FRC_NO_FLOAT` for the library and its
users), `FRACTONICA_BUILD_TESTS`, `FRACTONICA_STRICT` (warnings as errors; on
by default only when this is the top-level project).

### Apple platforms (iOS 16+, macOS 13+, watchOS 9+, tvOS 16+)

`Package.swift` exposes the C target as the module `CFractonica`. The package
identity is the directory or repository name (`core`):

```swift
// Package.swift of the app
dependencies: [.package(path: "../core")],          // or a submodule path, or .package(url:)
targets: [.target(name: "App",
                  dependencies: [.product(name: "CFractonica", package: "core")])]
```

```swift
import CFractonica

let now = frc_time_t(Date().timeIntervalSince1970 * 1_000_000)
var date = frc_date_t()
var text = [CChar](repeating: 0, count: 160)
withUnsafePointer(to: FRC_SAROS_141) { cat in
    if frc_date_at(cat, now, &date) == FRC_OK {
        _ = frc_format_date(&date, FRC_DATE_SHORT, FRC_UNIT_PREON, &text, text.count)
    }
}
print(String(cString: text))
```

What Swift sees of the C API:

- Functions, structs, enums and plain-number macros (`FRC_CLOCK_MAX_LEVEL`,
  `FRC_US_PER_DAY`, `FRC_VERSION` …) import as they are. Fixed arrays are
  tuples: `odometer.digit.0`.
- Function-like macros do not import, and neither does `FRC_TIME_NONE` (it is
  `INT64_MIN`). The header has a function for each: `frc_time_from_ms(ms)`,
  `frc_time_from_s(s)`, `frc_time_is_none(t)` and `frc_frac_to_double(frac)`.
  `if frc_time_is_none(odometer.next_change) == 0 { … }`.
- Struct fields that hold an enum are `UInt8` (`threshold.name`,
  `cycle.quarter`, `part.unit`); the functions take the enum:
  `frc_format_quarter(frc_quarter_t(UInt32(threshold.name)), …)`.

### Web (WebAssembly)

The sources compile for wasm32 as they are, with Emscripten or with a bare
clang. Because the library is freestanding, the module needs no JavaScript
runtime and has no imports:

```
emcc -Os -Iinclude src/*.c --no-entry -sSTANDALONE_WASM \
     -sEXPORTED_FUNCTIONS=_frc_date_at,_frc_format_date,_frc_catalogue_first -o fractonica.wasm
```

`make check-freestanding` builds the whole library that way with clang and
`wasm-ld --export-all` (`build/freestanding/-Os/fractonica.wasm`). From JavaScript:

- `frc_time_t` and every other 64-bit value is a `BigInt`.
- A data symbol such as `FRC_SAROS_141` is exported as a global holding its
  address: pass `exports.FRC_SAROS_141.value` as the catalogue pointer.
- Structs and text buffers live in the module's linear memory; the caller
  chooses where (the library never allocates).

### ESP32 with ESP-IDF

`CMakeLists.txt` registers the sources as a component when `ESP_PLATFORM` is
set. ESP-IDF names a component after its directory, so check it out as
`components/fractonica`:

```
git submodule add <url of this repository> components/fractonica
```

```cmake
# main/CMakeLists.txt
idf_component_register(SRCS "main.c" REQUIRES fractonica)
```

```c
#include <sys/time.h>
#include "fractonica.h"

struct timeval tv;
gettimeofday(&tv, NULL);
frc_time_t now = FRC_TIME_FROM_S(tv.tv_sec) + tv.tv_usec;
```

Built and linked here with ESP-IDF 5.5.1 for the `esp32` target.
`idf_component.yml` is the manifest for the IDF Component Manager.

### PlatformIO (any framework, Arduino included)

`library.json` describes the library (`fractonica-core`: sources in `src/`,
headers in `include/`, any platform, any framework). Put the checkout in the
project's `lib/` directory (a submodule at `lib/core`), or point at a sibling
checkout:

```ini
[env:native]
platform = native
lib_deps = symlink://../core
```

Both ways were built here with `platform = native`; the `lib_deps` line is the
same under `espressif32` with the Arduino or ESP-IDF framework.

### Arduino IDE

Not supported as an installable library. The Arduino library format searches
only `src/` for headers, and this repository keeps its one public header in
`include/` rather than duplicating it. Use PlatformIO or ESP-IDF, both of which
build Arduino-framework projects.

### 8-bit and Harvard targets

On AVR and ESP8266 `FRC_PROGMEM` expands to `PROGMEM` and every table read
goes through `pgm_read_*`. `make check-cross` compiles the library with
avr-gcc and checks that every table is placed in `.progmem.data`;
`make check-progmem` checks that every read of a table uses its accessor. The
one constant outside flash there is the 8-byte `FRC_SAROS_141` descriptor (a
pointer and three counts that callers read directly); its 70-entry table is in
flash. A catalogue table of your own must carry `FRC_PROGMEM` too.

To override the detection (another Harvard toolchain, or a section attribute
of your own) define **both** `FRC_PROGMEM` and `FRC_PROGMEM_IS_HARVARD` (1:
tables are read with `pgm_read_*`; 0: plain reads) on the compiler command
line. On AVR and ESP8266 the header refuses an overridden tag without the
second macro, because a table placed in flash and read as RAM returns garbage
silently.

## Repository layout

```
include/fractonica.h     the public API: the only header a user includes
src/                     16 .c files, frc_internal.h (shared internals), frc_big.h
tests/                   test_*.c (plain C, libc allowed), frc_test.h (the harness)
tests/vectors/           generated oracle vectors (C headers); never edited by hand
tools/oracle/            the generators that write them, and their documentation
tools/check-cross.sh     the cross-compiler checks behind `make check-cross`
tools/check-progmem.sh   the flash-table check behind `make check-progmem`
docs/SPEC.md             the specification, version 2.2.0
docs/DESIGN.md           the arithmetic design and its overflow bounds
AGENTS.md                the guide for people and coding agents changing the code
CHANGELOG.md             what each version added and changed, texts and layouts included
Makefile                 reference build, tests, checks
CMakeLists.txt           ESP-IDF component, or an ordinary CMake library
Package.swift            SwiftPM package FractonicaCore, module CFractonica
library.json             PlatformIO manifest
idf_component.yml        ESP-IDF component manifest
.github/workflows/ci.yml continuous integration: Linux and macOS
```

## Known gaps

- **Span decimals, SPEC §15.4.** The SPEC says a span "shorter than one Flavon
  … led by the Blink" keeps a decimal; the frozen reference keeps it below 10
  Blinks only (`12.4` Blinks read `12ν`), and the core follows the reference,
  the header and the vectors. Whether the SPEC sentence or the 10-Blink cut is
  meant is the owner's decision; changing it is one condition in
  `frc_format_span` plus regenerated vectors.
- **A design example that the arithmetic does not give.** Design ruling 2
  now gives `5.15ο light` for a Helion of sound in air, which is what the core
  writes. Its other example, `6.20χ light` for chemical transport, is not:
  the exact value is 6.1528χ, written `6.15χ` (`frc_format_light_span` follows
  the rule of the ruling, three significant digits half to even). The example
  is the design owner's to correct.
- **Flash pointers inside the text sinks.** `make check-progmem` proves that
  every table is read through `FRC_PGM_*` or handed to one of nine listed
  functions (`SINKS` in `tools/check-progmem.sh`); inside those functions the
  flash address is a plain pointer and the check cannot follow it. They are
  short and reviewed by eye.
- **Not executed on the embedded targets.** The library is compiled and its
  symbols are checked for Xtensa, RISC-V, Cortex-M and AVR; the test suite
  runs on the host and as 32-bit WebAssembly, not on those CPUs.

## Versioning

- `FRC_VERSION` (0.2.0) is the library; `FRC_SPEC_VERSION` (2.2.0) is the
  specification it implements. Both are macros in `fractonica.h`.
- Semantic versioning. Before 1.0 a minor version may change the API. A patch
  version never changes what the clock reads.
- A new SPEC version means a new copy of `docs/SPEC.md`, regenerated vectors
  and a new library version.

## Licence

Not chosen yet: all rights reserved until the owner picks one. See [LICENSE](LICENSE).
