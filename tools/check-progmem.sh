#!/bin/bash
#
# check-progmem.sh - every table tagged FRC_PROGMEM must be read through its
# FRC_PGM_* accessor. Run it as `make check-progmem` from the core directory.
#
# WHY. On AVR and ESP8266 a FRC_PROGMEM table lives in flash, outside the data
# address space. `FACTORS[i]` then compiles cleanly and reads RAM at a flash
# address: garbage, on those targets only. No compiler and no test on a host,
# an ESP32 or an ARM notices (there the tag is empty and the read is right),
# and `make check-harvard` cannot notice either: its fake accessors read the
# same memory a direct read does. So the rule of src/frc_internal.h section 1
# is held here, by reading the source.
#
# THE RULE. In src/*.c and src/*.h, with comments removed:
#   1. A table  `NAME[...] FRC_PROGMEM`  may appear only as
#        its declaration                    static const uint8_t NAME[8] FRC_PROGMEM = ...
#        an accessor read                   FRC_PGM_U8(&NAME[i])
#        a size                             sizeof NAME
#        a whole argument of a flash sink   frc_i_text_pgm(&w, NAME)
#        the table of a catalogue           const frc_catalogue_t X = {NAME, ...
#      Anything else (NAME[i], *NAME, NAME + 1, an argument of another
#      function) is a failure. So is the tag in any other position than
#      after the array brackets: this script would not see that table.
#   2. A flash sink is a function that takes the ADDRESS of flash text and
#      reads it with FRC_PGM_CHAR / FRC_PGM_U8. They are listed in SINKS
#      below. Inside a sink the address travels in a plain pointer, which this
#      script cannot follow: THE BODIES OF THE SINKS ARE THE PART A REVIEWER
#      MUST READ. Keep them few and short, and add a new one to the list only
#      after checking that it reads through the accessor.
#   3. frc_catalogue_t.instants_ms is the caller's flash table: it is read
#      only as FRC_PGM_I64(&...instants_ms[k]).
#
# Usage: tools/check-progmem.sh [--self-test] [source directory]   (default src)
#   --self-test   after the check, plant a direct read of each kind in a
#                 private copy of the sources and require the check to fail
#                 on every one: the gate is proven to bite.

set -u
cd "$(dirname "$0")/.." || exit 2

SINKS='frc_i_text_pgm|frc_i_text_nth|frc_i_packed_nth|format_medium|index_in_list|letters_spell|word_in_list|word_is|earth_word_index'

# strip_comments FILE: the file without /* */ and // comments, line for line.
# String literals are kept as they are (none holds a comment mark).
strip_comments() {
    awk '
    {
        line = $0; out = ""
        while (length(line) > 0) {
            if (in_comment) {
                end = index(line, "*/")
                if (end == 0) { line = "" } else { line = substr(line, end + 2); in_comment = 0 }
            } else {
                block = index(line, "/*"); slash = index(line, "//")
                if (slash > 0 && (block == 0 || slash < block)) { out = out substr(line, 1, slash - 1); line = "" }
                else if (block > 0) { out = out substr(line, 1, block - 1); line = substr(line, block + 2); in_comment = 1 }
                else { out = out line; line = "" }
            }
        }
        print out
    }' "$1"
}

# check DIR: print every violation as "file:line: text"; return 1 when there is one.
check() {
    local dir="$1" bad=0 names name file code left hits
    names=$(grep -h -o -E '[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])+ FRC_PROGMEM' "$dir"/*.c "$dir"/*.h \
        | sed -E 's/\[.*//' | sort -u)
    if [ -z "$names" ]; then
        echo "check-progmem: no FRC_PROGMEM table found in $dir (wrong directory?)"
        return 1
    fi
    for file in "$dir"/*.c "$dir"/*.h; do
        code=$(strip_comments "$file" | grep -n '')
        for name in $names; do
            # drop the allowed forms of this name, then see whether it is still there
            left=$(echo "$code" | grep -w -e "$name" \
                | sed -E "s/$name(\[[^]]*\])+ FRC_PROGMEM//g" \
                | sed -E "s/FRC_PGM_[A-Z0-9]+\(&$name\[//g" \
                | sed -E "s/sizeof *\(? *$name//g" \
                | sed -E "s/frc_catalogue_t [A-Za-z0-9_]+ = \{$name,//g" \
                | sed -E "/(^|[^A-Za-z0-9_])($SINKS)\(/ s/([(,] *)$name( *[,)])/\1\2/g")
            hits=$(echo "$left" | grep -w -e "$name")
            if [ -n "$hits" ]; then
                bad=1
                echo "$hits" | sed "s|^|$file:|; s|\$|      <- $name is read or passed without FRC_PGM_*|"
            fi
        done
        # the tag in any other position would hide a table from rule 1
        hits=$(echo "$code" | grep -w -e FRC_PROGMEM | grep -v -E '[A-Za-z0-9_]+(\[[^]]*\])+ FRC_PROGMEM( =|;)')
        if [ -n "$hits" ]; then
            bad=1
            echo "$hits" | sed "s|^|$file:|; s|\$|      <- declare a table as NAME[...] FRC_PROGMEM|"
        fi
        # rule 3: the catalogue table
        hits=$(echo "$code" | sed -E 's/FRC_PGM_I64\(&[A-Za-z0-9_>.-]*instants_ms\[//g; s/int64_t \*instants_ms//g' \
            | grep -E 'instants_ms *\[|\*+\(? *[A-Za-z0-9_>.-]*instants_ms')
        if [ -n "$hits" ]; then
            bad=1
            echo "$hits" | sed "s|^|$file:|; s|\$|      <- instants_ms is read without FRC_PGM_I64|"
        fi
    done
    [ $bad = 0 ] || return 1
    echo "check-progmem: ok ($(echo "$names" | wc -l | tr -d ' ') tables in $dir, every read through FRC_PGM_*)"
}

# plant LABEL FILE FROM TO: the check must fail on a copy where FROM became TO.
plant() {
    local label="$1" file="$2" from="$3" to="$4" copy
    copy=$(mktemp -d "${TMPDIR:-/tmp}/frc-progmem.XXXXXX") || exit 2
    cp src/*.c src/*.h "$copy"/
    if ! grep -q -F -e "$from" "$copy/$file"; then
        echo "check-progmem: self-test is stale: '$from' is not in $file any more"
        rm -rf "$copy"; return 1
    fi
    # the patterns hold no '|' and no '&'
    sed "s|$(printf '%s' "$from" | sed 's/[][\.*^$]/\\&/g')|$to|" "$copy/$file" > "$copy/$file.new" && mv "$copy/$file.new" "$copy/$file"
    if check "$copy" >/dev/null; then
        echo "check-progmem: SELF-TEST FAILED: not caught: $label"
        rm -rf "$copy"; return 1
    fi
    rm -rf "$copy"
    echo "  caught  $label"
}

self_test=0
if [ "${1:-}" = "--self-test" ]; then self_test=1; shift; fi
dir="${1:-src}"

check "$dir" || { echo "check-progmem: FAILED"; exit 1; }
if [ $self_test = 1 ]; then
    failed=0
    plant "a table indexed directly (FACTORS[...])" frc_hierarchy.c \
        'FRC_PGM_U8(&FACTORS[level % FRC_SEQUENCE_LENGTH])' 'FACTORS[level % FRC_SEQUENCE_LENGTH]' || failed=1
    plant "a 64-bit table indexed directly (PREFIXES[level])" frc_hierarchy.c \
        'FRC_PGM_U64(&PREFIXES[level])' 'PREFIXES[level]' || failed=1
    plant "a table dereferenced (*RATE_UNITS)" frc_span.c \
        'FRC_PGM_U8(&RATE_UNITS[i])' '*(RATE_UNITS + i)' || failed=1
    plant "flash text handed to a function that is not a flash sink" frc_span.c \
        'frc_i_text_pgm(&w, REAL_TIME);' 'put_ram_text(\&w, REAL_TIME);' || failed=1
    plant "a table declared in a form this script cannot see" frc_span.c \
        'REAL_TIME[] FRC_PROGMEM' '*const REAL_TIME FRC_PROGMEM' || failed=1
    plant "the catalogue table read directly (instants_ms[k])" frc_internal.h \
        'FRC_PGM_I64(&cat->instants_ms[k])' 'cat->instants_ms[k]' || failed=1
    [ $failed = 0 ] || { echo "check-progmem: FAILED"; exit 1; }
    echo "check-progmem: self-test ok (every planted direct read was caught)"
fi
