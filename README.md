# Fractonica Core

The Fractonica clock as a dependency-free C99 library: one header, no heap, no libc.

![The Fractonica clock in pure mode for 2026-10-08 12:00 UTC: a ring of nested
arcs with two large glyphs in the centre, and beside it the reading
ε#5 34η 29ι, the four larger cycles (Alpha Hexafracton to Delta Trifracton),
the solar sync and the next thresholds.](docs/hero.jpg)

*The core's reading at SPEC test vector A (2026-10-08T12:00:00Z), drawn by the web client in pure mode.*

## What it is

Fractonica is an eclipse-anchored harmonic clock. Its anchor is the solar Saros series 141.
It counts time in nested cycles, from the Alpha (about 72 years) down through the Helion to the Blink,
and draws them as nested alternating arcs with phase glyphs.
This repository is the shared core that computes all of it: positions, cycles,
glyphs, solar sync, the notation and the spectral ladder.
Every client uses this same code, so they all give the same reading to the microsecond.
The same clock runs in the browser at [fractonica.com](https://fractonica.com).

## The Fractonica repositories

| Repository | What it is |
|---|---|
| **[core](https://github.com/fractonica/core)** (this one) | The clock itself: a dependency-free C99 library, and the specification |
| [ios](https://github.com/fractonica/ios) | A SwiftUI app for iPhone, iPad and Mac, built on the core |
| [web](https://github.com/fractonica/web) | The core as WebAssembly and the clock as a static site, live at [fractonica.com](https://fractonica.com) |
| [esp32](https://github.com/fractonica/esp32) | ESP-IDF firmware for a small physical clock, with a host simulator; not yet run on hardware |

## Quick start

```sh
git clone https://github.com/fractonica/core.git
cd core
make test        # build build/libfractonica.a and run all 13 test programs
make check       # the tests and the host checks; run it before a commit
```

No build system is needed. Compile `src/*.c` with `-Iinclude` beside your
program (the guide has a complete `example.c`):

```sh
cc -std=c99 -Iinclude example.c src/*.c -o example
```

CMake (`fractonica::core`), SwiftPM (`swift build`, module `CFractonica`),
ESP-IDF and PlatformIO are supported too. The guide shows each one.

The clients build against a sibling checkout. Clone core next to the client:

```sh
git clone https://github.com/fractonica/core.git
git clone https://github.com/fractonica/web.git     # or ios, or esp32
```

## What is in it

- Pure C99. The sources include only `<stdint.h>` and `<stddef.h>`.
  As freestanding wasm32 the whole library is a module that imports nothing.
- No heap, no recursion, no mutable global data: `.data` + `.bss` is 0 bytes.
- The clock path is exact integer arithmetic. Floating point exists only in an
  optional API, and `-DFRC_NO_FLOAT` removes it.
- Small: 35,181 bytes of flash for the whole library on ARM Cortex-M3
  (clang `-Os`), 36,892 bytes on ESP32 (Xtensa), 55,614 bytes as a wasm32 module.
- Strict builds with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` for the host,
  Cortex-M3 and M0, ESP32 (Xtensa and RISC-V), wasm32 and AVR, at five optimisation levels.
- 13 test programs check the core against vectors from the exact (BigInt)
  JavaScript reference, against the numbers in the SPEC, and against its own
  promises at tens of thousands of instants.

## Status

Library version 0.2.0. It implements SPEC 2.2.0.
The test suite runs on the host and as 32-bit WebAssembly under node.
The core runs in the live web clock and in the iOS app.
It is compiled and its symbols are checked for Xtensa, RISC-V, Cortex-M and AVR,
but the tests have not run on those CPUs.
Two wording questions (span decimals, one design example) wait for the owner;
the guide lists them under "Known gaps".

## Documentation

- [docs/GUIDE.md](docs/GUIDE.md): the full guide. Guarantees, sizes, the API map,
  build targets, integrations and known gaps.
- [AGENTS.md](AGENTS.md): the guide for coding agents and engineers working in this repository.
- [docs/SPEC.md](docs/SPEC.md): the clock's specification.
- [include/fractonica.h](include/fractonica.h): the public API, with units,
  rounding and edge cases for every function.
- [docs/DESIGN.md](docs/DESIGN.md): the arithmetic and its overflow bounds.
- [CHANGELOG.md](CHANGELOG.md): what each version changed, texts and layouts included.

## Licence

Licensed under the [Apache License 2.0](LICENSE).
