# fractonica-core - host build, tests and the proofs of the design rules.
#
# The library itself needs no build system: compile src/*.c with -Iinclude
# (that is what the SwiftPM, Emscripten and ESP-IDF wrappers do). This
# Makefile builds it for the host with the strict flags, runs the tests, and
# proves the claims fractonica.h makes about the code ("no libc", "tables in
# flash", "integer only", "no RAM").
#
#   make lib                build/libfractonica.a from every src/*.c
#   make test               build and run every tests/test_*.c; fails if one fails
#   make test-NAME          build and run tests/test_NAME.c only
#
#   make check              every check below that needs only the host compiler,
#                           plus `test`: run it before a commit
#   make check-all          `check` and the checks that need LLVM / a wasm linker
#
#   Host compiler only:
#   make check-includes     src/ may include <stdint.h> and <stddef.h> only
#   make check-api          every function fractonica.h declares is defined, and
#                           the library exports nothing but frc_* / frc_i_* / FRC_*
#   make check-symbols      the library must not reference libc or libm
#   make check-nofloat      library and tests built with -DFRC_NO_FLOAT
#   make check-progmem      every FRC_PROGMEM table is read through FRC_PGM_* only
#                           (reads the source; then plants direct reads in a
#                           copy to prove the check catches them)
#   make check-harvard      library and tests built with -DFRC_TEST_FAKE_HARVARD
#                           (every FRC_PGM_* read goes through the pgm_read code path)
#   make check-sanitize     tests under AddressSanitizer + UndefinedBehaviorSanitizer
#
#   Need a full LLVM (clang with cross targets, llvm-nm, llvm-size):
#   make check-32           strict syntax check for 32-bit ARM and RISC-V
#   make check-freestanding THE "NO DEPENDENCIES" PROOF: src/*.c compiled
#                           freestanding for wasm32 and linked into one module
#                           that must import NOTHING (no memcpy, memset,
#                           __multi3, fmod ...). Needs wasm-ld too.
#   make size               text / rodata / data / bss of the library built -Os
#                           for a 32-bit MCU; fails unless data and bss are 0
#
#   Depend on what the machine has (never part of check / check-all):
#   make check-cross        compile for ARM (Cortex-M3, M0), ESP32 (Xtensa,
#                           RISC-V), wasm32 and AVR at -O0 .. -O3 with whichever
#                           cross compilers are installed (also looks in
#                           ~/.espressif); no libc symbol may be referenced
#   make test-wasm32        run the tests as 32-bit WebAssembly under node (emcc)
#   make vectors            regenerate tests/vectors/ from the JavaScript oracle
#   make clean
#
# Variables: CC, AR, CFLAGS (library), TEST_CFLAGS (tests), DEFS (extra flags
# for both, e.g. -DFRC_NO_FLOAT), BUILD (output directory), LLVM_BIN (where a
# full LLVM lives), WASM_LD.
#
# Adding code: drop a .c file in src/ or a test_NAME.c in tests/; both are
# picked up by wildcard. A test that uses a float-only API must guard it with
# #ifndef FRC_NO_FLOAT, or check-nofloat will not compile it.

CC          = cc
AR          = ar
BUILD       = build
DEFS        =

# The strict flags every source under src/ must pass, on every target.
STRICT      = -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -Iinclude -Isrc
# The library: pure C99, every warning is an error.
CFLAGS      = $(STRICT) -O2 -g
# The tests are ordinary hosted programs (libc and libm allowed, C11 for convenience).
TEST_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Iinclude -Isrc -Itests
TEST_LIBS   = -lm

SRC      := $(wildcard src/*.c)
OBJ      := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRC))
LIB      := $(BUILD)/libfractonica.a
TEST_SRC := $(wildcard tests/test_*.c)
TESTS    := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))
# Tests include the test harness and the generated vectors.
TEST_DEPS := $(wildcard tests/*.h) $(wildcard tests/vectors/*.h)
GENERATORS := $(wildcard tools/oracle/generate-*.mjs)

# A full LLVM for the cross checks (Apple's clang has no wasm32 / ARM targets).
LLVM_BIN  ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/lib/llvm/bin))
LLVM_CC    = $(if $(LLVM_BIN),$(LLVM_BIN)/clang,clang)
LLVM_NM    = $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-nm,llvm-nm)
LLVM_SIZE  = $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-size,llvm-size)
LLVM_OBJDUMP = $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-objdump,llvm-objdump)
# The first wasm-ld that actually runs (a Homebrew lld can be out of step with
# its LLVM and die at start-up; Emscripten ships its own).
WASM_LD   ?= $(shell (for l in wasm-ld $(LLVM_BIN)/wasm-ld /opt/homebrew/opt/emscripten/libexec/llvm/bin/wasm-ld \
               /usr/local/opt/emscripten/libexec/llvm/bin/wasm-ld; do \
               if $$l --version >/dev/null 2>&1; then echo $$l; break; fi; done) 2>/dev/null)

.PHONY: all lib test check check-all check-includes check-api check-symbols \
        check-progmem check-nofloat check-harvard check-variants check-sanitize check-32 \
        check-freestanding size check-cross test-wasm32 vectors clean
.SECONDARY: $(TESTS)

all: lib

lib: $(LIB)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFS) -MMD -MP -c $< -o $@

$(LIB): $(OBJ)
	@rm -f $@
	$(AR) rcs $@ $(OBJ)

$(BUILD)/test_%: tests/test_%.c $(LIB) $(TEST_DEPS) include/fractonica.h $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) $(DEFS) $< $(LIB) $(TEST_LIBS) -o $@

# Run every test even after a failure, then fail once at the end.
test: $(TESTS)
	@failed=0; \
	for t in $(TESTS); do \
		./$$t || failed=1; \
	done; \
	if [ $$failed -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED ($(words $(TESTS)) programs)"

test-%: $(BUILD)/test_%
	./$<

check: check-includes check-progmem check-api check-symbols test check-nofloat check-harvard check-sanitize

check-all: check check-32 check-freestanding size

# ---------------------------------------------------------------------------
# Host checks
# ---------------------------------------------------------------------------

check-includes:
	@bad=$$(grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' src/*.c src/*.h \
		| grep -v -E '<(stdint|stddef)\.h>' ; true); \
	if [ -n "$$bad" ]; then \
		echo "check-includes: src/ may include <stdint.h> and <stddef.h> only:"; echo "$$bad"; exit 1; \
	fi; \
	echo "check-includes: ok"

# The header is the contract. (1) Every function it declares must be defined
# in the library: the declarations are taken from the PREPROCESSED header, so
# the FRC_NO_FLOAT blocks are honoured when DEFS says so. (2) The library must
# export only frc_* (public), frc_i_* (shared internals) and FRC_* (data).
check-api: $(LIB)
	@declared=$$($(CC) -E -Iinclude $(DEFS) include/fractonica.h | grep -v '^#' | tr '\n' ' ' \
		| grep -o -E 'frc_[a-z0-9_]+[[:space:]]*\(' | tr -d ' (' | sort -u); \
	defined=$$(nm -g $(LIB) | awk '$$2 ~ /^[A-TV-Z]$$/ {print $$3}' | sed 's/^_//' | sort -u); \
	missing=$$(for f in $$declared; do echo "$$defined" | grep -q -x "$$f" || echo "$$f"; done); \
	foreign=$$(echo "$$defined" | grep -v -E '^(frc_|FRC_)' ; true); \
	if [ -n "$$missing" ]; then \
		echo "check-api: declared in fractonica.h but not defined:"; echo "$$missing"; exit 1; \
	fi; \
	if [ -n "$$foreign" ]; then \
		echo "check-api: the library exports names outside frc_* / FRC_*:"; echo "$$foreign"; exit 1; \
	fi; \
	echo "check-api: ok ($$(echo "$$declared" | wc -l | tr -d ' ') declared functions defined," \
	     "$$(echo "$$defined" | wc -l | tr -d ' ') exported symbols, all frc_* / FRC_*)"

# The core is freestanding: the only undefined symbols an object may have are
# other frc_* functions. Anything else (memcpy, memset, sin, __divti3 ...) is a
# libc / libm / 128-bit dependency that would not link on a bare target.
# (The stack protector's own symbols are the host compiler's, not ours.)
check-symbols: $(LIB)
	@bad=$$(nm -u $(LIB) | awk 'NF && $$NF !~ /:$$/ {print $$NF}' | sort -u \
		| grep -v -E '^_?frc_' | grep -v -E '^_*stack_chk' ; true); \
	if [ -n "$$bad" ]; then \
		echo "check-symbols: the library references non-core symbols:"; echo "$$bad"; exit 1; \
	fi; \
	echo "check-symbols: ok (only frc_* references)"

# Each variant gets its own build directory so objects never mix.
# Without floats: the clock, the notation and the ladder are integer-only.
check-nofloat:
	$(MAKE) BUILD=$(BUILD)/nofloat DEFS=-DFRC_NO_FLOAT check-api check-symbols test

# A table tagged FRC_PROGMEM must be read through FRC_PGM_* and in no other
# way: a direct read compiles everywhere and returns garbage on AVR / ESP8266.
# Nothing that RUNS on a host can see the difference (not check-harvard, and
# not the AVR build of check-cross, which only proves where the tables are
# placed), so this check reads the source. --self-test then plants a direct
# read of each kind in a private copy and requires the check to fail on it.
check-progmem:
	@tools/check-progmem.sh --self-test

# With the Harvard accessors: every FRC_PGM_* read is a function call that
# assembles the value byte by byte, as pgm_read_* does on AVR / ESP8266. This
# run proves the accessor path computes the same. It does NOT find a table
# that is read without its accessor (that read works here like any other):
# `make check-progmem` does.
check-harvard:
	$(MAKE) BUILD=$(BUILD)/harvard DEFS=-DFRC_TEST_FAKE_HARVARD check-symbols test

check-variants: check-nofloat check-harvard

# Library and tests under ASan + UBSan; any report aborts the test.
SANITIZE = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
check-sanitize:
	$(MAKE) BUILD=$(BUILD)/sanitize DEFS="$(SANITIZE)" \
		CFLAGS="$(STRICT) -O1 -g" TEST_CFLAGS="-std=c11 -Wall -Wextra -O1 -g -Iinclude -Isrc -Itests" test

# ---------------------------------------------------------------------------
# Cross checks with LLVM
# ---------------------------------------------------------------------------

# 32-bit targets see what the 64-bit host cannot: int and size_t are 32 bits,
# enums may be unsigned bytes, and -Wconversion finds different truncations.
check-32:
	@for target in armv7m-none-eabi riscv32-unknown-elf; do \
		for defs in "" "-DFRC_NO_FLOAT"; do \
			$(LLVM_CC) --target=$$target -ffreestanding -fsyntax-only $(STRICT) $$defs $(SRC) || exit 1; \
			echo "check-32: ok  $$target $$defs"; \
		done; \
	done

# THE "NO DEPENDENCIES" PROOF. Every source is compiled freestanding for
# wasm32 (no libc, no compiler-rt), at three optimisation levels because each
# one rewrites different loops into library calls. Then:
#   1. no object may have an undefined symbol other than frc_* and the two
#      names the wasm LINKER itself provides (the stack pointer and the
#      function table used for the path-sink callbacks);
#   2. the objects are linked into one module with --allow-undefined, so
#      anything still missing would become an import: the module must have NO
#      import section at all.
FREESTANDING_CC = $(LLVM_CC) --target=wasm32 -ffreestanding -nostdlib $(STRICT)
check-freestanding:
	@if [ -z "$(WASM_LD)" ]; then echo "check-freestanding: no working wasm-ld found (set WASM_LD)"; exit 1; fi
	@for opt in -O2 -Os -O3; do \
		for defs in "" "-DFRC_NO_FLOAT"; do \
			dir=$(BUILD)/freestanding/$$opt$$defs; rm -rf $$dir; mkdir -p $$dir; \
			for f in $(SRC); do \
				$(FREESTANDING_CC) $$opt $$defs -c $$f -o $$dir/$$(basename $$f .c).o || exit 1; \
			done; \
			undefined=$$($(LLVM_NM) -u $$dir/*.o 2>/dev/null | awk 'NF && $$NF !~ /:$$/ {print $$NF}' | sort -u \
				| grep -v -E '^frc_' | grep -v -x -E '__stack_pointer|__indirect_function_table' ; true); \
			if [ -n "$$undefined" ]; then \
				echo "check-freestanding: FAIL $$opt $$defs: undefined symbols: $$(echo $$undefined)"; exit 1; \
			fi; \
			$(WASM_LD) --no-entry --export-all --allow-undefined $$dir/*.o -o $$dir/fractonica.wasm || exit 1; \
			if $(LLVM_OBJDUMP) -h $$dir/fractonica.wasm | grep -q -w IMPORT; then \
				echo "check-freestanding: FAIL $$opt $$defs: the linked module imports something"; exit 1; \
			fi; \
			echo "check-freestanding: ok  $$opt $$defs  ($$(wc -c < $$dir/fractonica.wasm | tr -d ' ') bytes, no imports)"; \
		done; \
	done

# Flash and RAM of the whole library on a 32-bit MCU (ARM Cortex-M3, -Os).
# rodata is the constant tables; data and bss are RAM and must be zero: the
# core keeps no state at all.
SIZE_CC = $(LLVM_CC) --target=armv7m-none-eabi -mthumb -ffreestanding $(STRICT) -Os \
          -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
size:
	@dir=$(BUILD)/size; rm -rf $$dir; mkdir -p $$dir; \
	for f in $(SRC); do $(SIZE_CC) -c $$f -o $$dir/$$(basename $$f .c).o || exit 1; done; \
	echo "size: armv7-m thumb2, clang -Os (bytes)"; \
	$(LLVM_SIZE) -A $$dir/*.o | awk ' \
		/:$$/              { name = $$1; sub(/.*\//, "", name); sub(/\.o$$/, "", name); order[n++] = name } \
		$$1 ~ /^\.text/    { text[name] += $$2 } \
		$$1 ~ /^\.rodata/  { rodata[name] += $$2 } \
		$$1 ~ /^\.data/    { data[name] += $$2 } \
		$$1 ~ /^\.bss/     { bss[name] += $$2 } \
		END { \
			printf "  %-16s %7s %7s %5s %5s\n", "module", "text", "rodata", "data", "bss"; \
			for (i = 0; i < n; i++) { m = order[i]; \
				printf "  %-16s %7d %7d %5d %5d\n", m, text[m], rodata[m], data[m], bss[m]; \
				t += text[m]; r += rodata[m]; d += data[m]; b += bss[m] } \
			printf "  %-16s %7d %7d %5d %5d   flash %d, RAM %d\n", "TOTAL", t, r, d, b, t + r + d, d + b; \
			if (d + b != 0) { print "size: FAIL: the core must have no data and no bss"; exit 1 } \
		}'

# ---------------------------------------------------------------------------
# Whatever cross compilers the machine has
# ---------------------------------------------------------------------------

check-cross:
	@tools/check-cross.sh $(BUILD)/cross

# The whole core and each test, compiled to wasm32 (int, long, size_t and
# pointers are 32-bit, as on ESP32) and run by node.
EMCC = emcc
test-wasm32:
	@mkdir -p $(BUILD)/wasm32
	@failed=0; \
	for t in $(TEST_SRC); do \
		out=$(BUILD)/wasm32/$$(basename $$t .c).js; \
		$(EMCC) $(TEST_CFLAGS) $(DEFS) $$t $(SRC) -o $$out && node $$out || failed=1; \
	done; \
	if [ $$failed -ne 0 ]; then echo "TESTS FAILED (wasm32)"; exit 1; fi; \
	echo "ALL TESTS PASSED (wasm32)"

# Each generator writes its own header(s) into tests/vectors/ (needs node and
# the frozen reference in ../reference/js; see the generator's header).
vectors:
	@for g in $(GENERATORS); do node $$g || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d)
