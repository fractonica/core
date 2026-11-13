#!/bin/bash
#
# check-cross.sh - compile the core for the targets it ships on and prove it is
# freestanding there. Run it as `make check-cross` from the core directory.
#
# The host build cannot see what a 32-bit or Harvard compiler does with the same
# source. Each of these has happened to this code base:
#   - arm-none-eabi packs an enum into an unsigned byte: `value < 0` on an enum
#     argument is a -Wtype-limits ERROR there and nowhere else;
#   - riscv32 clang turned a five-element zero-fill loop into a call to memset;
#   - avr-gcc turned a three-case switch into a lookup table that lives in RAM;
#   - avr-gcc -O2 saw a division whose divisor it could not prove non-zero and
#     planted a trap on that path, which on AVR is a call to abort();
#   - gcc -O3 split a loop with an "else 0" branch and called memset for the
#     zeros; clang rewrote `a > UINT64_MAX / b` into a 128-bit multiplication,
#     which on wasm32 is a call to __multi3.
#
#   - riscv32-esp-elf-gcc -Os and arm-none-eabi-gcc for ARMv6-M (Cortex-M0) copy
#     a 16-byte struct that is passed BY VALUE with a call to memcpy, and
#     xtensa-esp-elf-gcc -O0 does the same for one that is assigned or
#     returned; xtensa-esp-elf-gcc -Os turned a four-byte zero-fill loop into a
#     call to memset.
#
# For every compiler that is installed (the others are skipped, not failed):
#   1. every src/*.c must compile with the strict flags, at -O0, -Og, -Os, -O2
#      and -O3 (each level rewrites different code into library calls);
#   2. the objects may reference only frc_* symbols and the compiler's own
#      runtime for 64-bit arithmetic on a 32-bit or 8-bit CPU (names that start
#      with "__": __aeabi_uldivmod, __udivdi3 ...). memcpy, memset, strlen,
#      sin ... are libc: a failure. On wasm32 there is no such runtime: only
#      the two symbols the linker itself provides are allowed there
#      (`make check-freestanding` then links the module and proves it imports
#      nothing);
#   3. on AVR (the real PROGMEM target) the only constant allowed outside flash
#      is the 8-byte FRC_SAROS_141 descriptor: everything else must be in
#      .progmem.data, or it would be copied into RAM at start-up;
#   4. on AVR, overriding FRC_PROGMEM by hand must either stop the build
#      (FRC_PROGMEM_IS_HARVARD missing) or read the tables from flash;
#   5. on the ESP32 compilers (Xtensa and RISC-V gcc, -Os, the ESP-IDF
#      default) no function may have a stack frame of STACK_LIMIT bytes or more
#      (-fstack-usage): the core is called from small FreeRTOS task stacks, and
#      a frame that grows unnoticed is found here, not on a device.
# This script proves WHERE the tables are; that every read of them goes
# through FRC_PGM_* is proven by tools/check-progmem.sh (`make check-progmem`).
#
# Usage: tools/check-cross.sh [output directory]      (default build/cross)

set -u
cd "$(dirname "$0")/.." || exit 2
OUT="${1:-build/cross}"
STRICT="-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -Iinclude -Isrc"
failed=0
ran=0
# Undefined symbols a target may have besides frc_*: an extended regular
# expression, set before each `target` call. The default is the compiler's
# own runtime (64-bit division and the like).
RUNTIME_OK='^__'

# first_of NAME...: print the first of the commands that exists. A compiler
# that is not on PATH is also looked for where the ESP-IDF installer puts its
# toolchains (~/.espressif/tools/<toolchain>/<version>/<toolchain>/bin, or
# $IDF_TOOLS_PATH); the newest version wins.
first_of() {
    local c found
    for c in "$@"; do
        if command -v "$c" >/dev/null 2>&1; then echo "$c"; return 0; fi
    done
    for c in "$@"; do
        found=$(ls -d "${IDF_TOOLS_PATH:-$HOME/.espressif}"/tools/*/*/*/bin/"$c" 2>/dev/null | sort | tail -1)
        if [ -n "$found" ] && [ -x "$found" ]; then echo "$found"; return 0; fi
    done
    return 1
}

# The largest stack frame a function of the core may have on the ESP32
# compilers, in bytes (the largest today is frc_span_between, about 210).
STACK_LIMIT=${STACK_LIMIT:-300}

# stack_frames LABEL COMPILER [FLAGS...]: compile every source at -Os with
# -fstack-usage and fail on any frame of STACK_LIMIT bytes or more.
stack_frames() {
    local label="$1"; shift
    local dir="$OUT/$label-stack" f big largest
    rm -rf "$dir"; mkdir -p "$dir"
    for f in src/*.c; do
        # shellcheck disable=SC2086
        if ! "$@" $STRICT -Os -fstack-usage -c "$f" -o "$dir/$(basename "$f" .c).o" 2>"$dir/errors.txt"; then
            failed=1
            echo "  FAIL  $label: $f does not compile with -fstack-usage"
            return
        fi
    done
    # a .su line: file:line:col:function <TAB> bytes <TAB> static|dynamic[,bounded]
    big=$(cat "$dir"/*.su | awk -F'\t' -v limit="$STACK_LIMIT" '$2 + 0 >= limit || $3 !~ /^static/ {print $1 " " $2 " " $3}')
    largest=$(cat "$dir"/*.su | sort -t"$(printf '\t')" -k2,2n | tail -1 | awk -F'\t' '{n = split($1, p, ":"); print p[n] " " $2}')
    if [ -n "$big" ]; then
        failed=1
        echo "  FAIL  $label: stack frames of $STACK_LIMIT bytes or more, or not static:"
        echo "$big" | sed 's/^/        /'
    else
        echo "  ok    $label: every stack frame below $STACK_LIMIT bytes (largest: $largest bytes)"
    fi
}

# build LABEL NM COMPILER [FLAGS...]: compile every source, then check the symbols
build() {
    local label="$1" nm="$2"; shift 2
    local dir="$OUT/$label" ok=1 f bad
    rm -rf "$dir"; mkdir -p "$dir"
    for f in src/*.c; do
        # $STRICT is a flag list: it is meant to be split into words
        # shellcheck disable=SC2086
        if ! "$@" $STRICT -c "$f" -o "$dir/$(basename "$f" .c).o" 2>"$dir/errors.txt"; then
            ok=0
            echo "  FAIL  $label: $f does not compile"
            sed 's/^/        /' "$dir/errors.txt" | head -20
        fi
    done
    [ $ok = 1 ] || { failed=1; return; }
    bad=$("$nm" -u "$dir"/*.o 2>/dev/null | awk 'NF && $NF !~ /:$/ {print $NF}' | sort -u | grep -v -E '^_?frc_' | grep -v -E "$RUNTIME_OK")
    if [ -n "$bad" ]; then
        failed=1
        echo "  FAIL  $label: references outside the core and the compiler runtime: $(echo $bad)"
    else
        echo "  ok    $label"
    fi
}

# target TITLE NM COMPILER [FLAGS...]: every optimisation level and the no-float build
target() {
    local title="$1" nm="$2"; shift 2
    local slug
    slug=$(echo "$title" | tr ' /' '--')
    ran=$((ran + 1))
    echo "$title"
    build "$slug-O0" "$nm" "$@" -O0
    build "$slug-Og" "$nm" "$@" -Og
    build "$slug-Os" "$nm" "$@" -Os
    build "$slug-O2" "$nm" "$@" -O2
    build "$slug-O3" "$nm" "$@" -O3
    build "$slug-nofloat" "$nm" "$@" -Os -DFRC_NO_FLOAT
}

# --- 32-bit ARM (Cortex-M) --------------------------------------------------
# Cortex-M0 is ARMv6-M (RP2040, STM32F0/G0, SAMD21): Thumb-1 code, where gcc
# reaches for memcpy far sooner than on the Thumb-2 Cortex-M3.
if CC=$(first_of arm-none-eabi-gcc); then
    target "arm cortex-m3" "${CC%gcc}nm" "$CC" -mcpu=cortex-m3 -mthumb
    target "arm cortex-m0" "${CC%gcc}nm" "$CC" -mcpu=cortex-m0 -mthumb
else
    echo "arm cortex-m3, cortex-m0: skipped (arm-none-eabi-gcc not installed)"
fi

# --- ESP32 (Xtensa and RISC-V) ----------------------------------------------
if CC=$(first_of xtensa-esp32-elf-gcc xtensa-esp-elf-gcc); then
    target "esp32 xtensa" "${CC%gcc}nm" "$CC" -mlongcalls
    stack_frames "esp32-xtensa" "$CC" -mlongcalls
else
    echo "esp32 xtensa: skipped (xtensa-esp32-elf-gcc not installed)"
fi
if CC=$(first_of riscv32-esp-elf-gcc riscv64-unknown-elf-gcc); then
    target "riscv32 gcc" "${CC%gcc}nm" "$CC" -march=rv32imc -mabi=ilp32
    stack_frames "riscv32-gcc" "$CC" -march=rv32imc -mabi=ilp32
else
    echo "riscv32 gcc: skipped (riscv32-esp-elf-gcc not installed)"
fi

# --- clang cross targets (a full LLVM, not Apple's) ---------------------------
LLVM_BIN=""
for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/lib/llvm/bin; do
    [ -x "$d/clang" ] && [ -x "$d/llvm-nm" ] && LLVM_BIN="$d" && break
done
if [ -z "$LLVM_BIN" ] && command -v llvm-nm >/dev/null 2>&1 && command -v clang >/dev/null 2>&1; then
    LLVM_BIN="$(dirname "$(command -v llvm-nm)")"
fi
clang_has() { echo 'int x;' | "$LLVM_BIN/clang" --target="$1" -x c -c - -o /dev/null >/dev/null 2>&1; }
if [ -n "$LLVM_BIN" ] && clang_has riscv32-unknown-elf; then
    target "riscv32 clang" "$LLVM_BIN/llvm-nm" "$LLVM_BIN/clang" --target=riscv32-unknown-elf -march=rv32imc -mabi=ilp32
else
    echo "riscv32 clang: skipped (no clang with the riscv32 target)"
fi
if [ -n "$LLVM_BIN" ] && clang_has wasm32-unknown-unknown; then
    RUNTIME_OK='^__(stack_pointer|indirect_function_table)$'
    target "wasm32 clang" "$LLVM_BIN/llvm-nm" "$LLVM_BIN/clang" --target=wasm32-unknown-unknown
    RUNTIME_OK='^__'
else
    echo "wasm32 clang: skipped (no clang with the wasm32 target)"
fi

# --- AVR: the real FRC_PROGMEM / pgm_read_* path ------------------------------
if CC=$(first_of avr-gcc) && echo '#include <avr/pgmspace.h>' | "$CC" -mmcu=atmega2560 -x c -E - >/dev/null 2>&1; then
    target "avr atmega2560" avr-nm "$CC" -mmcu=atmega2560
    for variant in Os O2 O3; do
        dir="$OUT/avr-atmega2560-$variant"
        [ -d "$dir" ] || continue
        # lower-case r in .progmem.data is flash; anything in .rodata / .data / .bss is RAM
        in_ram=$(avr-objdump -t "$dir"/*.o | awk '$4 ~ /^\.(rodata|data|bss)/ && $NF !~ /^\./ {print $NF}' | sort -u | grep -v '^FRC_SAROS_141$')
        if [ -n "$in_ram" ]; then
            failed=1
            echo "  FAIL  avr-$variant: constants outside flash (they would be copied to RAM): $(echo $in_ram)"
        else
            echo "  ok    avr-$variant: every table is in .progmem.data"
        fi
    done
    # Overriding FRC_PROGMEM (fractonica.h, "TO OVERRIDE THE DETECTION"): on a
    # Harvard target the tag alone would put the tables in flash and read them
    # as RAM. So without FRC_PROGMEM_IS_HARVARD the build must stop, and with
    # it the code must be the very code of the detected build (lpm reads).
    dir="$OUT/avr-override"; rm -rf "$dir"; mkdir -p "$dir"
    # shellcheck disable=SC2086
    if "$CC" -mmcu=atmega2560 -Os $STRICT -include avr/pgmspace.h -DFRC_PROGMEM=PROGMEM \
            -c src/frc_hierarchy.c -o "$dir/unsafe.o" 2>"$dir/unsafe.txt"; then
        failed=1
        echo "  FAIL  avr: FRC_PROGMEM overridden without FRC_PROGMEM_IS_HARVARD compiles (tables would be read as RAM)"
    elif ! grep -q 'FRC_PROGMEM_IS_HARVARD' "$dir/unsafe.txt"; then
        failed=1
        echo "  FAIL  avr: the override without FRC_PROGMEM_IS_HARVARD fails for another reason:"
        sed 's/^/        /' "$dir/unsafe.txt" | head -5
    else
        echo "  ok    avr: FRC_PROGMEM overridden without FRC_PROGMEM_IS_HARVARD is a compile error"
    fi
    # shellcheck disable=SC2086
    if "$CC" -mmcu=atmega2560 -Os $STRICT -DFRC_PROGMEM=PROGMEM -DFRC_PROGMEM_IS_HARVARD=1 \
            -c src/frc_hierarchy.c -o "$dir/frc_hierarchy.o" 2>"$dir/errors.txt" \
        && [ "$(avr-objdump -d "$dir/frc_hierarchy.o" | sed 1,3d)" = "$(avr-objdump -d "$OUT/avr-atmega2560-Os/frc_hierarchy.o" | sed 1,3d)" ] \
        && avr-objdump -d "$dir/frc_hierarchy.o" | grep -q -w -E 'e?lpm'; then
        echo "  ok    avr: FRC_PROGMEM overridden with FRC_PROGMEM_IS_HARVARD=1 reads its tables from flash (lpm), as detected"
    else
        failed=1
        echo "  FAIL  avr: FRC_PROGMEM overridden with FRC_PROGMEM_IS_HARVARD=1 does not build the detected code"
        sed 's/^/        /' "$dir/errors.txt" | head -5
    fi
else
    echo "avr: skipped (avr-gcc with avr-libc not installed)"
fi

if [ $ran = 0 ]; then
    echo "check-cross: no cross compiler found, nothing was checked"
    exit 0
fi
if [ $failed != 0 ]; then
    echo "check-cross: FAILED"
    exit 1
fi
echo "check-cross: ok ($ran targets)"
