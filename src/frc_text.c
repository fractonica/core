/*
 * frc_text.c - the text writer behind every frc_format_* function, the Greek
 * alphabet helpers, and frc_to_ascii.
 *
 * Implements section 7 of frc_internal.h and "Text utilities" of fractonica.h.
 * It serves the notation of SPEC S3.4 (Greek level names) and S15 (letters,
 * counts, the narrow space in grouped numbers, superscript exponents of S16,
 * and the power-of-ten form "2.454 × 10⁻²⁵ " that the ladder and the pure
 * wordings share).
 *
 * INVARIANTS
 *   - The writer never writes at or past buf[size - 1] except the final NUL,
 *     so one byte is always left for it.
 *   - Overflow is sticky: after the first append that does not fit, nothing
 *     more is appended. The buffer therefore holds a PREFIX of the full text.
 *   - A code point is appended whole or not at all, so that prefix never ends
 *     inside a UTF-8 sequence.
 *   - Text in tables lives in flash and is read byte by byte with FRC_PGM_*.
 *   - Numbers are converted through a 20-byte stack buffer (2^64 has 20
 *     digits); no other storage is used.
 */
#include "frc_internal.h"

/* ------------------------------------------------------------------------- */
/* Writer                                                                     */
/* ------------------------------------------------------------------------- */

void frc_i_text_init(frc_i_text_t *w, char *buf, size_t size)
{
    w->buf = buf;
    w->size = (buf != NULL) ? size : 0;
    w->len = 0;
    /* without room for the NUL there is nothing valid to return */
    w->overflow = (w->size == 0) ? 1 : 0;
    if (w->size != 0) {
        buf[0] = '\0';
    }
}

int frc_i_text_finish(frc_i_text_t *w)
{
    if (w->size != 0) {
        w->buf[w->len] = '\0'; /* len < size always holds */
    }
    return w->overflow ? (int)FRC_ERR_BUFFER : (int)w->len;
}

int frc_i_text_error(char *buf, size_t size, frc_status_t status)
{
    if (buf != NULL && size != 0) {
        buf[0] = '\0';
    }
    return (int)status;
}

/* 1 when `n` more bytes fit (keeping one for the NUL); otherwise latch overflow. */
static int text_room(frc_i_text_t *w, size_t n)
{
    if (w->overflow || n > w->size - 1 - w->len) {
        w->overflow = 1;
        return 0;
    }
    return 1;
}

void frc_i_text_char(frc_i_text_t *w, char c)
{
    if (text_room(w, 1)) {
        w->buf[w->len++] = c;
    }
}

void frc_i_text_cp(frc_i_text_t *w, uint32_t cp)
{
    uint8_t bytes[4];
    uint8_t n, i;

    if (cp < 0x80u) {
        bytes[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800u) {
        bytes[0] = (uint8_t)(0xC0u | (cp >> 6));
        bytes[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp < 0x10000u) {
        bytes[0] = (uint8_t)(0xE0u | (cp >> 12));
        bytes[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        bytes[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else {
        bytes[0] = (uint8_t)(0xF0u | ((cp >> 18) & 0x07u));
        bytes[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        bytes[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        bytes[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
        n = 4;
    }
    if (!text_room(w, n)) {
        return;
    }
    for (i = 0; i < n; i++) {
        w->buf[w->len++] = (char)bytes[i];
    }
}

/* Length of the UTF-8 sequence a lead byte opens (1 for anything malformed). */
static uint8_t utf8_length(uint8_t lead)
{
    if (lead >= 0xF0u) { return 4; }
    if (lead >= 0xE0u) { return 3; }
    if (lead >= 0xC0u) { return 2; }
    return 1;
}

void frc_i_text_pgm(frc_i_text_t *w, const char *flash_text)
{
    const char *p = flash_text;
    if (p == NULL) {
        return;
    }
    for (;;) {
        uint8_t lead = FRC_PGM_U8(p);
        uint8_t n, i;
        if (lead == 0) {
            return;
        }
        n = utf8_length(lead);
        if (!text_room(w, n)) {
            return; /* the whole sequence or nothing */
        }
        for (i = 0; i < n; i++) {
            uint8_t byte = FRC_PGM_U8(p + i);
            if (byte == 0) {
                return; /* a truncated sequence in a table: stop at its NUL */
            }
            w->buf[w->len++] = (char)byte;
        }
        p += n;
    }
}

const char *frc_i_packed_nth(const char *packed, uint8_t n)
{
    /* One pass, one byte at a time, counting the NULs that end items. (Written
     * as a single loop on purpose: an inner "skip to the NUL" loop is turned
     * into a call to strlen by optimisers, and the core must not need libc.) */
    const char *p = packed;
    uint8_t at_item_start = 1;
    while (n > 0) {
        if (FRC_PGM_U8(p) != 0) {
            at_item_start = 0;
        } else if (at_item_start) {
            break; /* an empty item closes the list: stay on it */
        } else {
            at_item_start = 1; /* this NUL ends an item; the next byte starts one */
            n--;
        }
        p++;
    }
    return p;
}

void frc_i_text_nth(frc_i_text_t *w, const char *packed, uint8_t n)
{
    frc_i_text_pgm(w, frc_i_packed_nth(packed, n));
}

/* The decimal digits of `value`, least significant first. Returns their count (>= 1). */
static uint8_t decimal_digits(uint64_t value, char digits[20])
{
    uint8_t n = 0;
    do {
        digits[n++] = (char)('0' + (char)(value % 10u));
        value /= 10u;
    } while (value != 0);
    return n;
}

void frc_i_text_u64_pad(frc_i_text_t *w, uint64_t value, uint8_t width)
{
    char digits[20];
    uint8_t n = decimal_digits(value, digits);
    if (width > 20) {
        width = 20;
    }
    for (; width > n; width--) {
        frc_i_text_char(w, '0');
    }
    while (n > 0) {
        frc_i_text_char(w, digits[--n]);
    }
}

void frc_i_text_u64(frc_i_text_t *w, uint64_t value)
{
    frc_i_text_u64_pad(w, value, 1);
}

/* |value| as unsigned; correct for INT64_MIN, whose negation does not fit int64_t. */
static uint64_t magnitude(int64_t value)
{
    return value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
}

void frc_i_text_decimal(frc_i_text_t *w, uint64_t value, uint8_t decimals)
{
    uint64_t scale = frc_i_pow_u64(10, decimals);
    uint64_t part;

    if (scale == 0) {
        return; /* more decimals than 64 bits hold: outside the contract */
    }
    part = value % scale;
    frc_i_text_u64(w, value / scale);
    if (part == 0) {
        return;
    }
    while (part % 10u == 0) { /* drop the trailing zeros */
        part /= 10u;
        decimals--;
    }
    frc_i_text_char(w, '.');
    frc_i_text_u64_pad(w, part, decimals); /* the zeros after the point */
}

void frc_i_text_u64_grouped(frc_i_text_t *w, uint64_t value)
{
    char digits[20];
    uint8_t n = decimal_digits(value, digits);
    while (n > 0) {
        frc_i_text_char(w, digits[--n]);
        if (n != 0 && n % 3 == 0) { /* a multiple of three digits still to come */
            frc_i_text_cp(w, FRC_I_CP_NNBSP);
        }
    }
}

/* Superscript digits 0..9. Latin-1 already had ¹ ² ³; the others came later
 * at U+2070 + digit, so the ten are not consecutive. A flash table rather
 * than a switch: a compiler would build its own table for the switch, and on
 * AVR that one would sit in RAM. */
static const uint16_t SUPERSCRIPT_CP[10] FRC_PROGMEM = {
    0x2070u, 0x00B9u, 0x00B2u, 0x00B3u, 0x2074u, 0x2075u, 0x2076u, 0x2077u, 0x2078u, 0x2079u};

/* Code point of superscript digit 0..9; taken mod 10. */
static uint32_t superscript_cp(uint8_t digit)
{
    return FRC_PGM_U16(&SUPERSCRIPT_CP[digit % 10u]);
}

void frc_i_text_i64_super(frc_i_text_t *w, int64_t value)
{
    char digits[20];
    uint8_t n = decimal_digits(magnitude(value), digits);
    if (value < 0) {
        frc_i_text_cp(w, FRC_I_CP_SUP_MINUS);
    }
    while (n > 0) {
        frc_i_text_cp(w, superscript_cp((uint8_t)(digits[--n] - '0')));
    }
}

void frc_i_text_mantissa(frc_i_text_t *w, uint64_t r, uint8_t digits, int32_t lead)
{
    int32_t decimals = (int32_t)digits - 1 - lead;
    if (decimals > 0) {
        uint64_t one = frc_i_pow_u64(10, (uint8_t)decimals);
        if (one == 0) {
            return; /* more decimals than 64 bits hold: outside the contract */
        }
        frc_i_text_u64(w, r / one);
        frc_i_text_char(w, '.');
        frc_i_text_u64_pad(w, r % one, (uint8_t)decimals);
    } else {
        frc_i_text_u64(w, r * frc_i_pow_u64(10, (uint8_t)-decimals));
    }
}

void frc_i_text_power_of_ten(frc_i_text_t *w, uint64_t r, uint8_t digits, int32_t exponent)
{
    frc_i_text_mantissa(w, r, digits, 0);
    frc_i_text_char(w, ' ');
    frc_i_text_cp(w, FRC_I_CP_TIMES);
    frc_i_text_char(w, ' ');
    frc_i_text_u64(w, 10);
    frc_i_text_i64_super(w, exponent);
    frc_i_text_char(w, ' ');
}

/* ------------------------------------------------------------------------- */
/* Greek alphabet                                                             */
/* ------------------------------------------------------------------------- */

#define GREEK_ALPHA       0x03B1u
#define GREEK_FINAL_SIGMA 0x03C2u /* between rho (index 16) and sigma (17) */
#define GREEK_OMEGA       0x03C9u
#define GREEK_RHO_INDEX   16

/* ASCII stand-ins by letter index. Latin look- or sound-alikes; h = eta,
 * q = theta, x = xi, f = phi, c = chi, y = psi, w = omega. */
static const char GREEK_ASCII[FRC_I_GREEK_COUNT + 1] FRC_PROGMEM = "abgdezhqiklmnxoprstufcyw";

uint32_t frc_i_greek_cp(uint8_t index)
{
    index = (uint8_t)(index % FRC_I_GREEK_COUNT);
    /* consecutive from alpha, stepping over the final sigma after rho */
    return GREEK_ALPHA + index + (index > GREEK_RHO_INDEX ? 1u : 0u);
}

/* The ASCII stand-in of Greek letter `index` (taken mod 24). */
static char greek_ascii(uint8_t index)
{
    return FRC_PGM_CHAR(&GREEK_ASCII[index % FRC_I_GREEK_COUNT]);
}

int frc_i_greek_index_of_cp(uint32_t cp)
{
    if (cp < GREEK_ALPHA || cp > GREEK_OMEGA) {
        return -1;
    }
    if (cp == GREEK_FINAL_SIGMA) {
        return GREEK_RHO_INDEX + 1; /* read a final sigma as sigma */
    }
    return (int)(cp - GREEK_ALPHA) - (cp > GREEK_FINAL_SIGMA ? 1 : 0);
}

int frc_i_greek_index_of_ascii(char c)
{
    uint8_t i;
    for (i = 0; i < FRC_I_GREEK_COUNT; i++) {
        if (FRC_PGM_CHAR(&GREEK_ASCII[i]) == c) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* frc_to_ascii                                                               */
/* ------------------------------------------------------------------------- */

/* Symbols with an ASCII stand-in (the Greek letters and the superscripts are
 * handled by rule). Two parallel tables keep each one a plain flash array. */
#define SYMBOL_COUNT 11
static const uint16_t SYMBOL_CP[SYMBOL_COUNT] FRC_PROGMEM = {
    0x00A0u,           /* no-break space        -> ' ' */
    FRC_I_CP_MICRO,    /* µ                     -> u   */
    FRC_I_CP_MIDDOT,   /* ·                     -> .   */
    FRC_I_CP_TIMES,    /* ×                     -> x   */
    0x2009u,           /* thin space            -> ' ' */
    0x2013u,           /* en dash               -> -   */
    FRC_I_CP_EM_DASH,  /* —                     -> -   */
    FRC_I_CP_NNBSP,    /* narrow no-break space -> ' ' */
    FRC_I_CP_MINUS,    /* −                     -> -   */
    FRC_I_CP_SQUARE,   /* ◻                     -> H   */
    FRC_I_CP_PENTAGON  /* ⬠                     -> P   */
};
static const char SYMBOL_ASCII[SYMBOL_COUNT + 1] FRC_PROGMEM = " u.x -- -HP";

/* Shared by frc_to_ascii below and by the scanner of frc_parse_date. */
uint8_t frc_i_utf8_decode(const char *s, uint32_t *cp)
{
    uint8_t lead = (uint8_t)s[0];
    uint8_t n = utf8_length(lead);
    uint32_t value;
    uint8_t i;

    if (lead < 0x80u) {
        *cp = lead;
        return 1;
    }
    if (lead < 0xC0u || lead > 0xF7u) { /* a stray continuation or invalid lead */
        *cp = FRC_I_CP_REPLACEMENT;
        return 1;
    }
    /* payload bits of the lead: 5, 4 or 3 for a 2-, 3- or 4-byte sequence */
    value = lead & (0xFFu >> (n + 1));
    for (i = 1; i < n; i++) {
        uint8_t byte = (uint8_t)s[i];
        if ((byte & 0xC0u) != 0x80u) {
            *cp = FRC_I_CP_REPLACEMENT;
            return 1;
        }
        value = (value << 6) | (byte & 0x3Fu);
    }
    /* An overlong form (a small value in a long sequence) is malformed too;
     * accepting it would let "\xC0\x80" smuggle a NUL into the output. */
    if (value < (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u)) {
        *cp = FRC_I_CP_REPLACEMENT;
        return 1;
    }
    *cp = value;
    return n;
}

/* '0'..'9', '-' or '+' for a superscript code point, 0 for anything else. */
static char superscript_ascii(uint32_t cp)
{
    uint8_t digit;
    if (cp == FRC_I_CP_SUP_MINUS) { return '-'; }
    if (cp == FRC_I_CP_SUP_PLUS) { return '+'; }
    for (digit = 0; digit < 10; digit++) {
        if (superscript_cp(digit) == cp) {
            return (char)('0' + (char)digit);
        }
    }
    return 0;
}

/* The ASCII stand-in of a non-ASCII, non-superscript code point; '?' if none. */
static char symbol_ascii(uint32_t cp)
{
    int greek = frc_i_greek_index_of_cp(cp);
    uint8_t i;
    if (greek >= 0) {
        return greek_ascii((uint8_t)greek);
    }
    for (i = 0; i < SYMBOL_COUNT; i++) {
        if (FRC_PGM_U16(&SYMBOL_CP[i]) == cp) {
            return FRC_PGM_CHAR(&SYMBOL_ASCII[i]);
        }
    }
    return '?';
}

int frc_to_ascii(const char *src, char *buf, size_t size)
{
    frc_i_text_t w;
    int in_superscript = 0; /* a run of superscripts gets ONE caret: "^-25" */

    if (src == NULL) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    while (*src != '\0') {
        uint32_t cp;
        char sup;
        src += frc_i_utf8_decode(src, &cp);
        sup = superscript_ascii(cp);
        if (sup != 0) {
            if (!in_superscript) {
                frc_i_text_char(&w, '^');
            }
            frc_i_text_char(&w, sup);
            in_superscript = 1;
            continue;
        }
        in_superscript = 0;
        frc_i_text_char(&w, cp < 0x80u ? (char)cp : symbol_ascii(cp));
    }
    return frc_i_text_finish(&w);
}
