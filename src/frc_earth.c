/*
 * frc_earth.c - the wordings of Earth mode: civil dates and times as text,
 * Earth durations, and a typed date read back.
 *
 * Implements "Civil time" of fractonica.h beyond the ISO text of frc_civil.c:
 *
 *   frc_format_civil_text   "THU 8 OCT 2026 · 12:00:00", "12:56", "9 OCT 01:34",
 *                           "JAN 2028", "UTC−2:30" ... (frc_civil_form_t)
 *   frc_format_duration     "23 min 53 s", "1 h 2 min", "3 d 4 h", "0.4 s"
 *   frc_parse_civil         "2026-10-08T14:00:00+02:00", "8 OCT 2026 12:00" ...
 *
 * Nothing here is part of the clock: the clients show these texts in Earth
 * mode only (design ruling 11), and the core owns them so that the three
 * clients stop composing their own. No SPEC section defines them; the forms
 * are those of design/astrolabe/DESIGN.md S9 as the clients wrote them.
 *
 * THE MODEL
 *   A civil text says the fields of frc_civil_t as they are: the caller has
 *   already chosen the offset (frc_civil_from_time). Nothing is rounded, and
 *   the weekday is computed from the date (frc_i_civil_weekday), never read.
 *   A negative year is led by U+2212 like every other number of the library,
 *   except in the ISO date, which keeps ISO 8601's hyphen-minus. The offset
 *   form takes offsets under a day, the ones the parser reads back.
 *   A duration is the one place that rounds: ONCE, half up, to the finest
 *   unit the text shows, and the parts are counted from that rounded value,
 *   so a carry happens before the wording ("1 min", never "60 s").
 *
 * INVARIANTS
 *   - Integer only; no table of pointers: the words are packed lists in flash
 *     (frc_internal.h section 1), read through FRC_PGM_* and the sinks
 *     frc_i_text_nth / earth_word_index (tools/check-progmem.sh).
 *   - The parser never reads past the NUL of its text, whatever bytes it
 *     holds, and writes its outputs only on FRC_OK. Its grammar is the
 *     header's, exactly: a space is the ASCII space and nothing else, a
 *     weekday needs a separator after it, 'T' leads a time after an ISO date
 *     only. The shape is checked first, then the range, then the weekday.
 *   - A form is chosen by a flag table in flash, not a switch (a switch can
 *     become a jump table in RAM on AVR).
 */
#include "frc_internal.h"

/* English three-letter capitals, by frc_civil_t.weekday (0 = Monday) and by
 * month - 1; the full month names are read by the parser only. */
static const char WEEKDAY_NAMES[] FRC_PROGMEM = "MON\0TUE\0WED\0THU\0FRI\0SAT\0SUN\0";
static const char MONTH_NAMES[] FRC_PROGMEM = "JAN\0FEB\0MAR\0APR\0MAY\0JUN\0JUL\0AUG\0SEP\0OCT\0NOV\0DEC\0";
static const char MONTH_WORDS[] FRC_PROGMEM =
    "JANUARY\0FEBRUARY\0MARCH\0APRIL\0MAY\0JUNE\0JULY\0AUGUST\0SEPTEMBER\0OCTOBER\0NOVEMBER\0DECEMBER\0";
/* The words of a zero offset: "UTC" is written, both are read. */
static const char ZERO_ZONES[] FRC_PROGMEM = "UTC\0GMT\0";

/* ------------------------------------------------------------------------- */
/* Civil text                                                                 */
/* ------------------------------------------------------------------------- */

/* What each frc_civil_form_t writes, in this order: the words of the date,
 * then the time. FRC_CIVIL_OFFSET is none of them (0) and is written apart. */
#define PART_WEEKDAY 0x01u /* "THU"                                          */
#define PART_DAY     0x02u /* "8"                                            */
#define PART_MONTH   0x04u /* "OCT"                                          */
#define PART_YEAR    0x08u /* "2026"                                         */
#define PART_ISO     0x10u /* "2026-10-08"                                   */
#define PART_MINUTE  0x20u /* "12:00"                                        */
#define PART_SECOND  0x40u /* ":00" after the minute                         */
#define PART_DOT     0x80u /* " · " between the date and the time            */

static const uint8_t FORM_PARTS[FRC_CIVIL_FORM_COUNT] FRC_PROGMEM = {
    PART_WEEKDAY | PART_DAY | PART_MONTH | PART_YEAR | PART_DOT | PART_MINUTE | PART_SECOND, /* DATE_TIME  */
    PART_WEEKDAY | PART_DAY | PART_MONTH | PART_YEAR,                                       /* DATE       */
    PART_MINUTE | PART_SECOND,                                                              /* TIME       */
    PART_MINUTE,                                                                            /* MINUTE     */
    PART_DAY | PART_MONTH,                                                                  /* DAY_MONTH  */
    PART_DAY | PART_MONTH | PART_MINUTE,                                                    /* ..._TIME   */
    PART_MONTH | PART_YEAR,                                                                 /* MONTH_YEAR */
    0,                                                                                      /* OFFSET     */
    PART_DAY | PART_MONTH | PART_YEAR,                                                      /* ..._YEAR   */
    PART_DAY | PART_MONTH | PART_MINUTE | PART_SECOND,                                      /* ..._SECONDS */
    PART_ISO,                                                                               /* ISO_DATE   */
    PART_WEEKDAY,                                                                           /* WEEKDAY    */
    PART_MONTH,                                                                             /* MONTH      */
    PART_YEAR,                                                                              /* YEAR       */
};

/* The year with four digits at least, and a sign outside 0..9999 ("-0044",
 * "+10000"). The ISO date writes the hyphen-minus of frc_format_civil, the
 * worded forms the U+2212 of every other text. |year| <= 290000 here. */
static void put_iso_year(frc_i_text_t *w, int32_t year, int iso)
{
    int64_t value = year;
    if (value < 0) {
        if (iso) {
            frc_i_text_char(w, '-');
        } else {
            frc_i_text_cp(w, FRC_I_CP_MINUS);
        }
        value = -value;
    } else if (value > 9999) {
        frc_i_text_char(w, '+');
    }
    frc_i_text_u64_pad(w, (uint64_t)value, 4);
}

/* A year in words: plain for 1..9999 ("800", "2026"), four digits and a
 * sign outside ("0000", "−0044", "+10000"). */
static void put_year(frc_i_text_t *w, int32_t year)
{
    if (year >= 1 && year <= 9999) {
        frc_i_text_u64(w, (uint64_t)year);
    } else {
        put_iso_year(w, year, 0);
    }
}

/* "UTC", "UTC+3", "UTC−2:30": whole minutes of the offset, cut toward zero.
 * |offset| < 86400 s (checked by the caller): at most "UTC+23:59". */
static void put_offset(frc_i_text_t *w, int32_t utc_offset_s)
{
    int64_t minutes = (int64_t)utc_offset_s / 60; /* C division cuts toward zero */

    frc_i_text_nth(w, ZERO_ZONES, 0);
    if (minutes == 0) {
        return;
    }
    if (minutes < 0) {
        frc_i_text_cp(w, FRC_I_CP_MINUS);
        minutes = -minutes;
    } else {
        frc_i_text_char(w, '+');
    }
    frc_i_text_u64(w, (uint64_t)(minutes / 60));
    if (minutes % 60 != 0) {
        frc_i_text_char(w, ':');
        frc_i_text_u64_pad(w, (uint64_t)(minutes % 60), 2);
    }
}

/* A space before every word but the first. */
static void put_gap(frc_i_text_t *w, int *started)
{
    if (*started) {
        frc_i_text_char(w, ' ');
    }
    *started = 1;
}

int frc_format_civil_text(const frc_civil_t *civil, int32_t utc_offset_s, frc_civil_form_t form,
                          char *buf, size_t size)
{
    frc_i_text_t w;
    uint8_t parts;
    int started = 0;

    if (!FRC_I_BELOW(form, FRC_CIVIL_FORM_COUNT)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    if (form == FRC_CIVIL_OFFSET) {
        /* under a day either way: the offsets frc_parse_civil reads back */
        if (utc_offset_s <= -86400 || utc_offset_s >= 86400) {
            return frc_i_text_error(buf, size, FRC_ERR_ARG);
        }
        frc_i_text_init(&w, buf, size);
        put_offset(&w, utc_offset_s);
        return frc_i_text_finish(&w);
    }
    /* only a real day and time of day is written (also NULL: FRC_ERR_ARG) */
    if (frc_civil_validate(civil) != FRC_OK) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    parts = FRC_PGM_U8(&FORM_PARTS[form]);
    frc_i_text_init(&w, buf, size);
    if (parts & PART_WEEKDAY) {
        put_gap(&w, &started);
        frc_i_text_nth(&w, WEEKDAY_NAMES, frc_i_civil_weekday(civil->year, civil->month, civil->day));
    }
    if (parts & PART_DAY) {
        put_gap(&w, &started);
        frc_i_text_u64(&w, civil->day);
    }
    if (parts & PART_MONTH) {
        put_gap(&w, &started);
        frc_i_text_nth(&w, MONTH_NAMES, (uint8_t)(civil->month - 1));
    }
    if (parts & PART_YEAR) {
        put_gap(&w, &started);
        put_year(&w, civil->year);
    }
    if (parts & PART_ISO) {
        put_gap(&w, &started);
        put_iso_year(&w, civil->year, 1);
        frc_i_text_char(&w, '-');
        frc_i_text_u64_pad(&w, civil->month, 2);
        frc_i_text_char(&w, '-');
        frc_i_text_u64_pad(&w, civil->day, 2);
    }
    if (parts & PART_MINUTE) {
        if (parts & PART_DOT) {
            frc_i_text_char(&w, ' ');
            frc_i_text_cp(&w, FRC_I_CP_MIDDOT);
        }
        put_gap(&w, &started);
        frc_i_text_u64_pad(&w, civil->hour, 2);
        frc_i_text_char(&w, ':');
        frc_i_text_u64_pad(&w, civil->minute, 2);
        if (parts & PART_SECOND) {
            frc_i_text_char(&w, ':');
            frc_i_text_u64_pad(&w, civil->second, 2);
        }
    }
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* Durations                                                                  */
/* ------------------------------------------------------------------------- */

#define DURATION_UNITS 4
/* d h min s, coarsest first, in seconds */
static const uint32_t DURATION_SECONDS[DURATION_UNITS] FRC_PROGMEM = {86400u, 3600u, 60u, 1u};
static const char DURATION_NAMES[] FRC_PROGMEM = "d\0h\0min\0s\0";

#define US_PER_S 1000000u
/* 9.995 s in hundredths: from here a duration is said in whole units. */
#define DECIMAL_BELOW 1000u

/* The coarsest unit whose length in seconds is at most `seconds` (>= 1). */
static uint8_t leading_unit(uint64_t seconds)
{
    uint8_t unit = 0;
    while (unit + 1 < DURATION_UNITS && FRC_PGM_U32(&DURATION_SECONDS[unit]) > seconds) {
        unit++;
    }
    return unit;
}

int frc_format_duration(int64_t microseconds, uint8_t max_parts, char *buf, size_t size)
{
    frc_i_text_t w;
    /* |microseconds| without negating INT64_MIN */
    uint64_t magnitude = (microseconds < 0) ? (uint64_t)(-(microseconds + 1)) + 1u : (uint64_t)microseconds;
    uint64_t hundredths = magnitude / 10000u + ((magnitude % 10000u >= 5000u) ? 1u : 0u);
    uint64_t length, count, seconds;
    uint8_t lead, finest, unit;
    int started = 0;

    frc_i_text_init(&w, buf, size);
    if (hundredths == 0) {
        frc_i_text_char(&w, '0'); /* no sign: nothing is left to have one */
        frc_i_text_char(&w, ' ');
        frc_i_text_char(&w, 's');
        return frc_i_text_finish(&w);
    }
    if (microseconds < 0) {
        frc_i_text_cp(&w, FRC_I_CP_MINUS);
    }
    if (hundredths < DECIMAL_BELOW) {
        frc_i_text_decimal(&w, hundredths, 2); /* "0.4", "9.27", "5" */
        frc_i_text_char(&w, ' ');
        frc_i_text_char(&w, 's');
        return frc_i_text_finish(&w);
    }
    if (max_parts == 0) {
        max_parts = 2;
    } else if (max_parts > DURATION_UNITS) {
        max_parts = DURATION_UNITS;
    }
    /* Round once, half up, to the finest unit shown (lead + max_parts - 1).
     * magnitude >= 9.995 s, so the second fits and leads at least. */
    lead = leading_unit(magnitude / US_PER_S);
    finest = (uint8_t)(lead + max_parts - 1); /* <= 3 + 4 - 1 */
    if (finest >= DURATION_UNITS) {
        finest = DURATION_UNITS - 1u;
    }
    length = (uint64_t)FRC_PGM_U32(&DURATION_SECONDS[finest]) * US_PER_S; /* <= 8.64e10 */
    count = magnitude / length;
    if (magnitude % length >= length - magnitude % length) {
        count++; /* a half goes up */
    }
    /* count * length / 10^6 <= 2^63 / 10^6 + one day: fits */
    seconds = count * FRC_PGM_U32(&DURATION_SECONDS[finest]);
    /* A carry may have made a coarser unit lead (59.6 s -> 60 s = 1 min).
     * Everything below the new leading unit is then 0, so the rounding
     * above is also the rounding to the new finest unit: nothing is rounded
     * twice. */
    lead = leading_unit(seconds);
    finest = (uint8_t)(lead + max_parts - 1); /* <= 3 + 4 - 1 */
    if (finest >= DURATION_UNITS) {
        finest = DURATION_UNITS - 1u;
    }
    for (unit = lead; unit <= finest; unit++) {
        uint64_t per = FRC_PGM_U32(&DURATION_SECONDS[unit]);
        uint64_t value = seconds / per;
        seconds %= per;
        if (value != 0) { /* zero parts are not written; the leading one never is zero */
            put_gap(&w, &started);
            frc_i_text_u64_grouped(&w, value);
            frc_i_text_char(&w, ' ');
            frc_i_text_nth(&w, DURATION_NAMES, unit);
        }
    }
    return frc_i_text_finish(&w);
}

/* ------------------------------------------------------------------------- */
/* Reading a civil date                                                       */
/* ------------------------------------------------------------------------- */

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

/* ASCII spaces only: the header promises no other white space. */
static const char *skip_spaces(const char *p)
{
    while (*p == ' ') {
        p++;
    }
    return p;
}

/* The letters at `p`, at most 16 counted (a longer word matches nothing). */
static uint8_t word_length(const char *p)
{
    uint8_t n = 0;
    while (n < 16 && is_letter(p[n])) {
        n++;
    }
    return n;
}

/* Index of the `length` letters at `word` in a packed list in flash, any
 * case, the whole item; -1 when none is spelt. A flash sink: it reads the
 * list through FRC_PGM_CHAR only. */
static int earth_word_index(const char *word, uint8_t length, const char *packed)
{
    uint8_t n;

    for (n = 0; n < 16; n++) {
        const char *name = frc_i_packed_nth(packed, n);
        uint8_t i = 0;
        if (FRC_PGM_CHAR(name) == '\0') {
            return -1; /* the closing empty item: the list is done */
        }
        while (i < length && FRC_PGM_CHAR(name + i) == upper(word[i])) {
            i++;
        }
        if (i == length && FRC_PGM_CHAR(name + i) == '\0') {
            return n;
        }
    }
    return -1;
}

/* A run of digits: at least `min`, at most `max` of them (a longer run is
 * refused, not cut). NULL when the run does not fit; else past it, with the
 * value and the number of digits. */
static const char *read_digits(const char *p, uint8_t min, uint8_t max, uint32_t *value, uint8_t *count)
{
    uint32_t v = 0;
    uint8_t n = 0;

    while (is_digit(p[n])) {
        if (n == max) {
            return NULL;
        }
        v = v * 10u + (uint32_t)(p[n] - '0'); /* at most 9 digits: < 2^30 */
        n++;
    }
    if (n < min) {
        return NULL;
    }
    *value = v;
    if (count != NULL) {
        *count = n;
    }
    return p + n;
}

/* '+', '-' or U+2212 at `p`: past it, with the sign; `p` itself when none. */
static const char *read_sign(const char *p, int *negative)
{
    *negative = 0;
    if (*p == '+') {
        return p + 1;
    }
    if (*p == '-') {
        *negative = 1;
        return p + 1;
    }
    /* U+2212 is E2 88 92; the tests stop at a NUL, which no byte here is */
    if ((unsigned char)p[0] == 0xE2u && (unsigned char)p[1] == 0x88u && (unsigned char)p[2] == 0x92u) {
        *negative = 1;
        return p + 3;
    }
    return p;
}

/*
 * What was read is kept in separate scalars, not in a struct that is zeroed
 * at the start: clang (riscv32 -Os) turns the zeroing of a run of struct
 * fields into a call to memset.
 */

/* "2026-10-08" (the sign, if any, already read): month and day. */
static const char *read_iso_date(const char *p, uint32_t *month, uint32_t *day)
{
    if (*p != '-') {
        return NULL;
    }
    p = read_digits(p + 1, 1, 2, month, NULL);
    if (p == NULL || *p != '-') {
        return NULL;
    }
    return read_digits(p + 1, 1, 2, day, NULL);
}

/* "OCT 2026" / "October 2026" after the day: the month, and the year with
 * its sign. */
static const char *read_worded_date(const char *p, uint32_t *month, uint32_t *year, int *negative)
{
    uint8_t length = word_length(p);
    int index = (length == 3) ? earth_word_index(p, length, MONTH_NAMES) : earth_word_index(p, length, MONTH_WORDS);
    const char *q;

    if (length == 0 || index < 0) {
        return NULL;
    }
    *month = (uint32_t)index + 1u;
    q = skip_spaces(p + length);
    if (q == p + length) {
        return NULL; /* a space must part the month and the year */
    }
    q = read_sign(q, negative);
    return read_digits(q, 1, 6, year, NULL);
}

/* "12:00", "12:00:00", "12:00:00.123456789": hours, minutes, and the
 * seconds and microsecond when the text has them (else they stay as they
 * are: 0). */
static const char *read_time(const char *p, uint32_t *hours, uint32_t *minutes, uint32_t *seconds,
                             uint32_t *microsecond)
{
    uint32_t fraction;
    uint8_t digits;

    p = read_digits(p, 1, 2, hours, NULL);
    if (p == NULL || *p != ':') {
        return NULL;
    }
    p = read_digits(p + 1, 2, 2, minutes, NULL);
    if (p == NULL || *p != ':') {
        return p;
    }
    p = read_digits(p + 1, 2, 2, seconds, NULL);
    if (p == NULL || *p != '.') {
        return p;
    }
    p = read_digits(p + 1, 1, 9, &fraction, &digits);
    if (p != NULL) {
        /* to the microsecond, cut: six digits, padded or dropped */
        for (; digits < 6; digits++) {
            fraction *= 10u;
        }
        for (; digits > 6; digits--) {
            fraction /= 10u;
        }
        *microsecond = fraction;
    }
    return p;
}

/* "+02:00", "+0200", "+02", "−2:30" (the sign not yet read): the offset in
 * seconds, and whether it is out of 0..23 h, 0..59 min. NULL when it is not
 * an offset. */
static const char *read_offset(const char *p, int32_t *offset_s, int *out_of_range)
{
    uint32_t value, hours, minutes = 0;
    uint8_t digits;
    int negative;
    const char *q = read_sign(p, &negative);

    if (q == p) {
        return NULL;
    }
    q = read_digits(q, 1, 4, &value, &digits);
    if (q == NULL || digits == 3) {
        return NULL;
    }
    if (digits == 4) {
        hours = value / 100u; /* "0200" */
        minutes = value % 100u;
    } else {
        hours = value;
        if (*q == ':') {
            q = read_digits(q + 1, 2, 2, &minutes, NULL);
            if (q == NULL) {
                return NULL;
            }
        }
    }
    *out_of_range = (hours > 23u || minutes > 59u);
    /* hours <= 99, minutes <= 99: fits */
    *offset_s = (int32_t)(hours * 3600u + minutes * 60u);
    if (negative) {
        *offset_s = -*offset_s;
    }
    return q;
}

/* An offset after the time: "Z", "UTC", "UTC+3", "GMT", "+02:00". `p`
 * itself when there is none; *has_offset is set when there is one. */
static const char *read_zone(const char *p, int32_t *offset_s, int *has_offset, int *out_of_range)
{
    const char *q = skip_spaces(p);
    const char *after;

    if ((*q == 'Z' || *q == 'z') && !is_letter(q[1])) {
        *has_offset = 1;
        return q + 1;
    }
    if (word_length(q) == 3 && earth_word_index(q, 3, ZERO_ZONES) >= 0) {
        *has_offset = 1;
        after = read_offset(q + 3, offset_s, out_of_range); /* "UTC+3": the offset follows at once */
        return (after != NULL) ? after : q + 3;
    }
    after = read_offset(q, offset_s, out_of_range);
    if (after == NULL) {
        return p;
    }
    *has_offset = 1;
    return after;
}

frc_status_t frc_parse_civil(const char *text, frc_civil_t *out, int32_t *utc_offset_s, int *has_offset)
{
    frc_civil_t civil;
    const char *p, *q;
    uint32_t first, month = 0, day = 0, year = 0, hours = 0, minutes = 0, seconds = 0, microsecond = 0;
    int32_t offset_s = 0;
    int weekday = -1, negative = 0, offset_given = 0, out_of_range = 0, time_follows = 0, iso = 0;
    uint8_t length, digits;

    if (text == NULL || out == NULL) {
        return FRC_ERR_ARG;
    }

    /* an optional weekday: "THU 8", "thu, 8", "THU,8", "THU , 8"; a
     * separator must follow it ("THU8" is refused) */
    p = skip_spaces(text);
    length = word_length(p);
    if (length != 0) {
        weekday = (length == 3) ? earth_word_index(p, 3, WEEKDAY_NAMES) : -1;
        if (weekday < 0) {
            return FRC_ERR_PARSE;
        }
        q = skip_spaces(p + 3);
        if (*q == ',') {
            q = skip_spaces(q + 1);
        }
        if (q == p + 3) {
            return FRC_ERR_PARSE;
        }
        p = q;
    }

    /* the date: ISO ("[sign]YYYY-MM-DD") or worded ("D MON YYYY") */
    q = read_sign(p, &negative);
    q = read_digits(q, 1, 6, &first, &digits);
    if (q == NULL) {
        return FRC_ERR_PARSE;
    }
    if (*q == '-') {
        year = first;
        iso = 1;
        q = read_iso_date(q, &month, &day);
    } else if (q != p + digits || digits > 2 || skip_spaces(q) == q) {
        return FRC_ERR_PARSE; /* a signed day, a long one, or no space after it */
    } else {
        day = first;
        q = read_worded_date(skip_spaces(q), &month, &year, &negative);
    }
    if (q == NULL) {
        return FRC_ERR_PARSE;
    }
    p = q;

    /* the time: after 'T' (an ISO date's only), after spaces, or after " · " */
    if (iso && (*p == 'T' || *p == 't')) {
        p++;
        time_follows = 1;
    } else {
        q = skip_spaces(p);
        if ((unsigned char)q[0] == 0xC2u && (unsigned char)q[1] == 0xB7u) { /* U+00B7 */
            p = skip_spaces(q + 2);
            time_follows = 1;
        } else if (q != p && is_digit(*q)) {
            p = q;
            time_follows = 1;
        }
    }
    if (time_follows) {
        p = read_time(p, &hours, &minutes, &seconds, &microsecond);
        if (p == NULL) {
            return FRC_ERR_PARSE;
        }
        p = read_zone(p, &offset_s, &offset_given, &out_of_range);
    }
    if (*skip_spaces(p) != '\0') {
        return FRC_ERR_PARSE;
    }

    /* every number fits its field: two digits at most, the year six */
    civil.year = negative ? -(int32_t)year : (int32_t)year;
    civil.month = (uint8_t)month;
    civil.day = (uint8_t)day;
    civil.hour = (uint8_t)hours;
    civil.minute = (uint8_t)minutes;
    civil.second = (uint8_t)seconds;
    civil.microsecond = microsecond;
    if (frc_civil_validate(&civil) != FRC_OK || out_of_range) {
        return FRC_ERR_RANGE;
    }
    civil.weekday = frc_i_civil_weekday(civil.year, civil.month, civil.day);
    if (weekday >= 0 && weekday != civil.weekday) {
        return FRC_ERR_PARSE; /* "FRI 8 OCT 2026": not that day's name */
    }
    /* field by field: a whole-struct copy may become memcpy (frc_internal.h) */
    out->year = civil.year;
    out->month = civil.month;
    out->day = civil.day;
    out->hour = civil.hour;
    out->minute = civil.minute;
    out->second = civil.second;
    out->weekday = civil.weekday;
    out->microsecond = civil.microsecond;
    if (utc_offset_s != NULL) {
        *utc_offset_s = offset_s;
    }
    if (has_offset != NULL) {
        *has_offset = offset_given;
    }
    return FRC_OK;
}
