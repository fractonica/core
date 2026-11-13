/*
 * test_earth.c - the wordings of Earth mode (src/frc_earth.c) and
 * frc_civil_validate.
 *
 * Each function is held against an INDEPENDENT statement of its rule written
 * here with libc (snprintf, a leap-year rule typed in, a day count of its
 * own), on every day of 2,000 years, on random fields and offsets, and on
 * the edges the header names. The parser is held by round trips through the
 * two writers, typed examples, every refusal the header lists, and a fuzz
 * run (under check-sanitize it is also a memory-safety test).
 */
#include "frc_test.h"
#include "fractonica.h"

#include <stdlib.h>

#define NNBSP "\xE2\x80\xAF"
#define MINUS "\xE2\x88\x92"
#define MIDDOT "\xC2\xB7"

static uint64_t rng_state = UINT64_C(0xE4A27);
static uint64_t rng(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static frc_civil_t civil_of(int32_t y, int mo, int d, int h, int mi, int s, uint32_t us)
{
    frc_civil_t c;
    c.year = y;
    c.month = (uint8_t)mo;
    c.day = (uint8_t)d;
    c.hour = (uint8_t)h;
    c.minute = (uint8_t)mi;
    c.second = (uint8_t)s;
    c.weekday = 99; /* never read */
    c.microsecond = us;
    return c;
}

/* ------------------------------------------------------------------------- */
/* An independent calendar                                                    */
/* ------------------------------------------------------------------------- */

static int leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int month_days(int64_t y, int m)
{
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[m - 1] + (m == 2 && leap(y));
}

/* Days from 1970-01-01, counted day by day from a 400-year cycle start
 * (2000-03-01 style arithmetic avoided on purpose: plain loops). */
static int64_t days_of(int64_t y, int m, int d)
{
    int64_t days = 0, year;
    int month;
    /* move by whole 400-year cycles (146 097 days) to a year near 2000 */
    int64_t shift = 0;
    while (y + shift < 1600) {
        shift += 400;
        days -= 146097;
    }
    while (y + shift >= 2400) {
        shift -= 400;
        days += 146097;
    }
    y += shift;
    if (y >= 1970) {
        for (year = 1970; year < y; year++) {
            days += leap(year) ? 366 : 365;
        }
    } else {
        for (year = y; year < 1970; year++) {
            days -= leap(year) ? 366 : 365;
        }
    }
    for (month = 1; month < m; month++) {
        days += month_days(y, month);
    }
    return days + d - 1;
}

static int weekday_of(int64_t y, int m, int d)
{
    int64_t w = (days_of(y, m, d) + 3) % 7; /* 1970-01-01 a Thursday, Monday = 0 */
    return (int)(w < 0 ? w + 7 : w);
}

static const char *const WEEKDAYS[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
static const char *const MONTHS[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                       "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

/* A year as the worded forms write it (plain != 0: U+2212 before a negative
 * year) or as the ISO date writes it (the hyphen-minus of ISO 8601). */
static void year_text(int32_t y, int plain, char *out, size_t size)
{
    if (plain && y >= 1 && y <= 9999) {
        snprintf(out, size, "%d", (int)y);
    } else if (y < 0) {
        snprintf(out, size, "%s%04d", plain ? MINUS : "-", (int)-y);
    } else if (y > 9999) {
        snprintf(out, size, "+%04d", (int)y);
    } else {
        snprintf(out, size, "%04d", (int)y);
    }
}

/* ------------------------------------------------------------------------- */
/* frc_civil_validate                                                         */
/* ------------------------------------------------------------------------- */

static void test_validate(void)
{
    frc_civil_t c;
    int64_t y;
    int m, d;

    TEST_SECTION("frc_civil_validate: every day of years -400 .. 2400");
    for (y = -400; y <= 2400; y++) {
        for (m = 0; m <= 13; m++) {
            for (d = 0; d <= 32; d++) {
                int real = m >= 1 && m <= 12 && d >= 1 && d <= month_days(y, m);
                c = civil_of((int32_t)y, m, d, 0, 0, 0, 0);
                CHECK_EQ_I64(frc_civil_validate(&c), real ? FRC_OK : FRC_ERR_RANGE);
                CHECK_NOTE("%d-%d-%d", (int)y, m, d);
                if (real) {
                    /* and the core's own calendar agrees: the day comes back */
                    frc_civil_t back;
                    frc_civil_from_time(frc_time_from_civil(&c, 0), 0, &back);
                    CHECK(back.year == y && back.month == m && back.day == d);
                    CHECK_EQ_I64(back.weekday, weekday_of(y, m, d));
                }
            }
        }
    }

    TEST_SECTION("frc_civil_validate: the header's cases");
    c = civil_of(2026, 2, 31, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2026, 2, 29, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2026, 4, 31, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2024, 2, 29, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_OK);
    c = civil_of(2000, 2, 29, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_OK);
    c = civil_of(1900, 2, 29, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2026, 13, 1, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2026, 10, 8, 24, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2026, 10, 8, 23, 60, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2026, 10, 8, 23, 59, 60, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2026, 10, 8, 23, 59, 59, 1000000);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(2026, 10, 8, 23, 59, 59, 999999);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_OK);
    c = civil_of(290000, 12, 31, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_OK);
    c = civil_of(-290000, 1, 1, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_OK);
    c = civil_of(290001, 1, 1, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    c = civil_of(-290001, 1, 1, 0, 0, 0, 0);
    CHECK_EQ_I64(frc_civil_validate(&c), FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_civil_validate(NULL), FRC_ERR_ARG);
}

/* ------------------------------------------------------------------------- */
/* frc_format_civil_text                                                      */
/* ------------------------------------------------------------------------- */

/* The text of a form by the header's rule, written with snprintf. "" for an
 * offset of a day or more, which the header refuses. */
static void civil_text_rule(const frc_civil_t *c, int32_t offset, frc_civil_form_t form, char *out, size_t size)
{
    char year[16], iso_year[16];
    const char *wd = WEEKDAYS[weekday_of(c->year, c->month, c->day)];
    const char *mon = MONTHS[c->month - 1];

    year_text(c->year, 1, year, sizeof year);
    year_text(c->year, 0, iso_year, sizeof iso_year);
    switch (form) {
    case FRC_CIVIL_DATE_TIME:
        snprintf(out, size, "%s %d %s %s " MIDDOT " %02d:%02d:%02d", wd, c->day, mon, year, c->hour, c->minute, c->second);
        break;
    case FRC_CIVIL_DATE: snprintf(out, size, "%s %d %s %s", wd, c->day, mon, year); break;
    case FRC_CIVIL_TIME: snprintf(out, size, "%02d:%02d:%02d", c->hour, c->minute, c->second); break;
    case FRC_CIVIL_MINUTE: snprintf(out, size, "%02d:%02d", c->hour, c->minute); break;
    case FRC_CIVIL_DAY_MONTH: snprintf(out, size, "%d %s", c->day, mon); break;
    case FRC_CIVIL_DAY_MONTH_TIME: snprintf(out, size, "%d %s %02d:%02d", c->day, mon, c->hour, c->minute); break;
    case FRC_CIVIL_MONTH_YEAR: snprintf(out, size, "%s %s", mon, year); break;
    case FRC_CIVIL_OFFSET: {
        long minutes = (long)offset / 60; /* toward zero */
        long a = minutes < 0 ? -minutes : minutes;
        if (offset <= -86400 || offset >= 86400) {
            snprintf(out, size, "%s", "");
        } else if (minutes == 0) {
            snprintf(out, size, "UTC");
        } else if (a % 60 == 0) {
            snprintf(out, size, "UTC%s%ld", minutes < 0 ? MINUS : "+", a / 60);
        } else {
            snprintf(out, size, "UTC%s%ld:%02ld", minutes < 0 ? MINUS : "+", a / 60, a % 60);
        }
        break;
    }
    case FRC_CIVIL_DAY_MONTH_YEAR: snprintf(out, size, "%d %s %s", c->day, mon, year); break;
    case FRC_CIVIL_DAY_MONTH_SECONDS:
        snprintf(out, size, "%d %s %02d:%02d:%02d", c->day, mon, c->hour, c->minute, c->second);
        break;
    case FRC_CIVIL_ISO_DATE: snprintf(out, size, "%s-%02d-%02d", iso_year, c->month, c->day); break;
    case FRC_CIVIL_WEEKDAY: snprintf(out, size, "%s", wd); break;
    case FRC_CIVIL_MONTH: snprintf(out, size, "%s", mon); break;
    case FRC_CIVIL_YEAR: snprintf(out, size, "%s", year); break;
    default: snprintf(out, size, "?"); break;
    }
}

static frc_civil_t every_size_civil;
static int every_size_form;
static int call_civil_text(char *buf, size_t size)
{
    return frc_format_civil_text(&every_size_civil, -9000, (frc_civil_form_t)every_size_form, buf, size);
}

/* Every buffer size: the length, or FRC_ERR_BUFFER with a clean prefix. */
static void check_every_size(int (*call)(char *, size_t), const char *full)
{
    size_t length = strlen(full), size;
    for (size = 0; size <= length + 2; size++) {
        char buf[512];
        int n;
        memset(buf, '#', sizeof buf);
        n = call(buf, size);
        CHECK(buf[size] == '#');
        if (size > length) {
            CHECK_EQ_I64(n, (int)length);
            CHECK_STR(buf, full);
        } else {
            CHECK_EQ_I64(n, FRC_ERR_BUFFER);
            if (size > 0) {
                size_t kept = strlen(buf);
                CHECK(kept < size);
                CHECK(strncmp(buf, full, kept) == 0);
                CHECK(((unsigned char)full[kept] & 0xC0u) != 0x80u); /* never half a code point */
            }
        }
    }
    CHECK_EQ_I64(call(NULL, 64), FRC_ERR_BUFFER);
}

static void test_civil_text(void)
{
    char got[96], want[96];
    frc_civil_t c;
    int form, i;
    size_t longest = 0;

    TEST_SECTION("frc_format_civil_text: the header's examples");
    c = civil_of(2026, 10, 8, 12, 0, 0, 0);
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, FRC_CIVIL_DATE_TIME, got, sizeof got), 26);
    CHECK_STR(got, "THU 8 OCT 2026 " MIDDOT " 12:00:00");
    frc_format_civil_text(&c, 0, FRC_CIVIL_DATE, got, sizeof got);
    CHECK_STR(got, "THU 8 OCT 2026");
    frc_format_civil_text(&c, 0, FRC_CIVIL_TIME, got, sizeof got);
    CHECK_STR(got, "12:00:00");
    frc_format_civil_text(&c, 0, FRC_CIVIL_MINUTE, got, sizeof got);
    CHECK_STR(got, "12:00");
    frc_format_civil_text(&c, 0, FRC_CIVIL_DAY_MONTH, got, sizeof got);
    CHECK_STR(got, "8 OCT");
    frc_format_civil_text(&c, 0, FRC_CIVIL_ISO_DATE, got, sizeof got);
    CHECK_STR(got, "2026-10-08");
    frc_format_civil_text(&c, 0, FRC_CIVIL_WEEKDAY, got, sizeof got);
    CHECK_STR(got, "THU");
    frc_format_civil_text(&c, 0, FRC_CIVIL_MONTH, got, sizeof got);
    CHECK_STR(got, "OCT");
    frc_format_civil_text(&c, 0, FRC_CIVIL_YEAR, got, sizeof got);
    CHECK_STR(got, "2026");
    c = civil_of(2026, 10, 9, 1, 34, 5, 999999);
    frc_format_civil_text(&c, 0, FRC_CIVIL_DAY_MONTH_TIME, got, sizeof got);
    CHECK_STR(got, "9 OCT 01:34");
    frc_format_civil_text(&c, 0, FRC_CIVIL_DAY_MONTH_SECONDS, got, sizeof got);
    CHECK_STR(got, "9 OCT 01:34:05"); /* the fields as they are: nothing rounded */
    c = civil_of(2028, 1, 15, 0, 0, 0, 0);
    frc_format_civil_text(&c, 0, FRC_CIVIL_MONTH_YEAR, got, sizeof got);
    CHECK_STR(got, "JAN 2028");
    c = civil_of(2028, 2, 3, 0, 0, 0, 0);
    frc_format_civil_text(&c, 0, FRC_CIVIL_DAY_MONTH_YEAR, got, sizeof got);
    CHECK_STR(got, "3 FEB 2028");
    /* the offset: "UTC+3", "UTC−2:30", "UTC"; it does not read civil */
    CHECK_EQ_I64(frc_format_civil_text(NULL, 3 * 3600, FRC_CIVIL_OFFSET, got, sizeof got), 5);
    CHECK_STR(got, "UTC+3");
    frc_format_civil_text(NULL, -(2 * 3600 + 30 * 60), FRC_CIVIL_OFFSET, got, sizeof got);
    CHECK_STR(got, "UTC" MINUS "2:30");
    frc_format_civil_text(NULL, 0, FRC_CIVIL_OFFSET, got, sizeof got);
    CHECK_STR(got, "UTC");
    frc_format_civil_text(NULL, 59, FRC_CIVIL_OFFSET, got, sizeof got);
    CHECK_STR(got, "UTC"); /* under a minute */
    frc_format_civil_text(NULL, 5 * 3600 + 45 * 60 + 59, FRC_CIVIL_OFFSET, got, sizeof got);
    CHECK_STR(got, "UTC+5:45"); /* cut toward zero */
    frc_format_civil_text(NULL, -(5 * 3600 + 45 * 60 + 59), FRC_CIVIL_OFFSET, got, sizeof got);
    CHECK_STR(got, "UTC" MINUS "5:45");
    /* under a day either way, the offsets frc_parse_civil reads back */
    CHECK_EQ_I64(frc_format_civil_text(NULL, 86399, FRC_CIVIL_OFFSET, got, sizeof got), 9);
    CHECK_STR(got, "UTC+23:59");
    frc_format_civil_text(NULL, -86399, FRC_CIVIL_OFFSET, got, sizeof got);
    CHECK_STR(got, "UTC" MINUS "23:59");
    {
        static const int32_t refused[4] = {86400, -86400, INT32_MAX, INT32_MIN};
        int k;
        for (k = 0; k < 4; k++) {
            snprintf(got, sizeof got, "junk");
            CHECK_EQ_I64(frc_format_civil_text(NULL, refused[k], FRC_CIVIL_OFFSET, got, sizeof got), FRC_ERR_ARG);
            CHECK_STR(got, "");
            CHECK_NOTE("offset %d", (int)refused[k]);
        }
    }
    /* every offset it writes is read back after a time */
    for (i = -86399; i <= 86399; i += 7) {
        char line[64];
        frc_civil_t back;
        int32_t back_offset;
        int back_has;
        snprintf(line, sizeof line, "2026-10-08 12:00 ");
        CHECK(frc_format_civil_text(NULL, i, FRC_CIVIL_OFFSET, line + 17, sizeof line - 17) > 0);
        CHECK_EQ_I64(frc_parse_civil(line, &back, &back_offset, &back_has), FRC_OK);
        CHECK_EQ_I64(back_offset, i / 60 * 60);
        CHECK_NOTE("\"%s\"", line);
    }
    /* the weekday is computed, never read */
    c = civil_of(2026, 10, 8, 12, 0, 0, 0);
    c.weekday = 6;
    frc_format_civil_text(&c, 0, FRC_CIVIL_WEEKDAY, got, sizeof got);
    CHECK_STR(got, "THU");
    /* years outside 1..9999 as frc_format_civil writes them */
    c = civil_of(800, 10, 8, 12, 0, 0, 0);
    frc_format_civil_text(&c, 0, FRC_CIVIL_DAY_MONTH_YEAR, got, sizeof got);
    CHECK_STR(got, "8 OCT 800");
    c = civil_of(0, 3, 1, 0, 0, 0, 0);
    frc_format_civil_text(&c, 0, FRC_CIVIL_YEAR, got, sizeof got);
    CHECK_STR(got, "0000");
    c = civil_of(-44, 3, 15, 0, 0, 0, 0);
    frc_format_civil_text(&c, 0, FRC_CIVIL_DAY_MONTH_YEAR, got, sizeof got);
    CHECK_STR(got, "15 MAR " MINUS "0044"); /* U+2212, as every text */
    frc_format_civil_text(&c, 9 * 3600, FRC_CIVIL_DATE_TIME, got, sizeof got);
    CHECK_STR(got, "THU 15 MAR " MINUS "0044 " MIDDOT " 00:00:00");
    frc_format_civil_text(&c, 0, FRC_CIVIL_YEAR, got, sizeof got);
    CHECK_STR(got, MINUS "0044");
    frc_format_civil_text(&c, 0, FRC_CIVIL_ISO_DATE, got, sizeof got);
    CHECK_STR(got, "-0044-03-15"); /* ISO 8601: the hyphen-minus of frc_format_civil */
    /* the last year of four plain digits */
    c = civil_of(9999, 12, 31, 0, 0, 0, 0);
    frc_format_civil_text(&c, 0, FRC_CIVIL_ISO_DATE, got, sizeof got);
    CHECK_STR(got, "9999-12-31");
    frc_format_civil_text(&c, 0, FRC_CIVIL_YEAR, got, sizeof got);
    CHECK_STR(got, "9999");
    c = civil_of(1, 1, 1, 0, 0, 0, 0);
    frc_format_civil_text(&c, 0, FRC_CIVIL_ISO_DATE, got, sizeof got);
    CHECK_STR(got, "0001-01-01");
    frc_format_civil_text(&c, 0, FRC_CIVIL_YEAR, got, sizeof got);
    CHECK_STR(got, "1");
    c = civil_of(10000, 1, 1, 0, 0, 0, 0);
    frc_format_civil_text(&c, 0, FRC_CIVIL_MONTH_YEAR, got, sizeof got);
    CHECK_STR(got, "JAN +10000");
    /* refusals: "" in the buffer */
    c = civil_of(2026, 2, 31, 0, 0, 0, 0);
    snprintf(got, sizeof got, "junk");
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, FRC_CIVIL_DATE, got, sizeof got), FRC_ERR_ARG);
    CHECK_STR(got, "");
    c = civil_of(2026, 10, 8, 24, 0, 0, 0);
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, FRC_CIVIL_TIME, got, sizeof got), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_civil_text(NULL, 0, FRC_CIVIL_DATE, got, sizeof got), FRC_ERR_ARG);
    c = civil_of(2026, 10, 8, 12, 0, 0, 0);
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, FRC_CIVIL_FORM_COUNT, got, sizeof got), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, (frc_civil_form_t)-1, got, sizeof got), FRC_ERR_ARG);
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, FRC_CIVIL_DATE, NULL, 0), FRC_ERR_BUFFER);

    TEST_SECTION("frc_format_civil_text: random instants and offsets, every form");
    for (i = 0; i < 200000; i++) {
        /* any instant of years -290000 .. 290000, offsets up to a day either way */
        frc_time_t t = (frc_time_t)(rng() % UINT64_C(9000000000000000000)) * ((rng() & 1) ? 1 : -1);
        int32_t offset = (int32_t)(rng() % 172801) - 86400;
        if (i % 3 == 0) {
            offset = (int32_t)((int64_t)(rng() % 109) - 54) * 900; /* the quarter hours of real zones */
        }
        frc_civil_from_time(t, offset, &c);
        for (form = 0; form < FRC_CIVIL_FORM_COUNT; form++) {
            int n = frc_format_civil_text(&c, offset, (frc_civil_form_t)form, got, sizeof got);
            civil_text_rule(&c, offset, (frc_civil_form_t)form, want, sizeof want);
            CHECK_EQ_I64(n, (want[0] == '\0') ? FRC_ERR_ARG : (int)strlen(want));
            CHECK_STR(got, want);
            CHECK_NOTE("t %" PRId64 ", offset %d, form %d", t, (int)offset, form);
            if (n > 0 && (size_t)n > longest) {
                longest = (size_t)n;
            }
        }
    }
    TEST_SECTION("frc_format_civil_text: the longest text");
    for (form = 0; form < FRC_CIVIL_FORM_COUNT; form++) {
        static const int32_t years[4] = {-290000, 290000, 1, 9999};
        int k;
        for (k = 0; k < 4; k++) {
            c = civil_of(years[k], 12, 31, 23, 59, 59, 0);
            int n = frc_format_civil_text(&c, -86399, (frc_civil_form_t)form, got, sizeof got);
            CHECK(n > 0);
            if ((size_t)n > longest) {
                longest = (size_t)n;
            }
        }
    }
    CHECK_EQ_U64(longest, 32); /* fractonica.h: 32 bytes + NUL */
    c = civil_of(-290000, 12, 31, 23, 59, 59, 0);
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, FRC_CIVIL_DATE_TIME, got, sizeof got), 32);
    CHECK_STR(got, "SUN 31 DEC " MINUS "290000 " MIDDOT " 23:59:59");
    /* the years only the int64 ends of frc_time_t reach are refused */
    frc_civil_from_time(INT64_MAX, 0, &c);
    CHECK_EQ_I64(c.year, 294247);
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, FRC_CIVIL_DATE, got, sizeof got), FRC_ERR_ARG);
    frc_civil_from_time(INT64_MIN, 0, &c);
    CHECK_EQ_I64(c.year, -290308);
    CHECK_EQ_I64(frc_format_civil_text(&c, 0, FRC_CIVIL_YEAR, got, sizeof got), FRC_ERR_ARG);

    TEST_SECTION("frc_format_civil_text: every buffer size");
    every_size_civil = civil_of(2026, 10, 8, 12, 0, 0, 0);
    for (form = 0; form < FRC_CIVIL_FORM_COUNT; form++) {
        every_size_form = form;
        civil_text_rule(&every_size_civil, -9000, (frc_civil_form_t)form, want, sizeof want);
        check_every_size(call_civil_text, want);
    }
}

/* ------------------------------------------------------------------------- */
/* frc_format_duration                                                        */
/* ------------------------------------------------------------------------- */

static void grouped(uint64_t v, char *out, size_t size)
{
    char digits[32];
    size_t n, i, o = 0;
    snprintf(digits, sizeof digits, "%" PRIu64, v);
    n = strlen(digits);
    for (i = 0; i < n && o + 4 < size; i++) {
        out[o++] = digits[i];
        if ((n - i - 1) % 3 == 0 && i + 1 < n) {
            memcpy(out + o, NNBSP, 3);
            o += 3;
        }
    }
    out[o] = '\0';
}

/* The rule of the header, written again without the core's structure:
 * the hundredths below 9.995 s; above, round to the finest of `parts`
 * consecutive units counted from the leading one of the UNROUNDED value,
 * and say the rounded value from ITS leading unit. */
static void duration_rule(int64_t us, unsigned parts, char *out, size_t size)
{
    static const uint64_t unit_s[4] = {86400, 3600, 60, 1};
    static const char *const names[4] = {"d", "h", "min", "s"};
    uint64_t mag = us < 0 ? (uint64_t)0 - (uint64_t)us : (uint64_t)us;
    uint64_t hundredths = (mag + 5000) / 10000;
    char part[64];
    int lead, fine, k;
    uint64_t len, count, sec;

    out[0] = '\0';
    if (parts == 0) {
        parts = 2;
    }
    if (parts > 4) {
        parts = 4;
    }
    if (hundredths == 0) {
        snprintf(out, size, "0 s");
        return;
    }
    if (us < 0) {
        snprintf(out, size, MINUS);
    }
    if (hundredths < 1000) {
        char num[32];
        if (hundredths % 100 == 0) {
            snprintf(num, sizeof num, "%" PRIu64, hundredths / 100);
        } else if (hundredths % 10 == 0) {
            snprintf(num, sizeof num, "%" PRIu64 ".%" PRIu64, hundredths / 100, hundredths % 100 / 10);
        } else {
            snprintf(num, sizeof num, "%" PRIu64 ".%02" PRIu64, hundredths / 100, hundredths % 100);
        }
        strncat(out, num, size - strlen(out) - 1);
        strncat(out, " s", size - strlen(out) - 1);
        return;
    }
    for (lead = 0; lead < 3 && unit_s[lead] * 1000000 > mag; lead++) {
    }
    fine = lead + (int)parts - 1 > 3 ? 3 : lead + (int)parts - 1;
    len = unit_s[fine] * 1000000;
    count = (mag + len / 2) / len; /* half up; len is even */
    sec = count * unit_s[fine];
    for (lead = 0; lead < 3 && unit_s[lead] > sec; lead++) {
    }
    fine = lead + (int)parts - 1 > 3 ? 3 : lead + (int)parts - 1;
    for (k = lead; k <= fine; k++) {
        uint64_t v = sec / unit_s[k];
        sec %= unit_s[k];
        if (v) {
            char g[64];
            grouped(v, g, sizeof g);
            snprintf(part, sizeof part, "%s%s %s", (k == lead) ? "" : " ", g, names[k]);
            strncat(out, part, size - strlen(out) - 1);
        }
    }
}

static int64_t every_size_us;
static int call_duration(char *buf, size_t size)
{
    return frc_format_duration(every_size_us, 4, buf, size);
}

static void expect_duration(int64_t us, uint8_t parts, const char *want)
{
    char got[96];
    CHECK_EQ_I64(frc_format_duration(us, parts, got, sizeof got), (int)strlen(want));
    CHECK_STR(got, want);
    CHECK_NOTE("%" PRId64 " us, %u parts", us, parts);
}

static void test_duration(void)
{
    char got[96], want[96];
    int i;
    unsigned parts;
    size_t longest = 0;

    TEST_SECTION("frc_format_duration: the header's examples");
    expect_duration(INT64_C(1433000000), 0, "23 min 53 s");
    expect_duration(INT64_C(3720000000), 0, "1 h 2 min");
    expect_duration(INT64_C(3600000000) * 76, 0, "3 d 4 h");
    expect_duration(400000, 0, "0.4 s");
    expect_duration(9271000, 0, "9.27 s");
    expect_duration(5000000, 0, "5 s");
    expect_duration(4999, 0, "0 s");
    expect_duration(5000, 0, "0.01 s");
    expect_duration(-4999, 0, "0 s"); /* no sign before nothing */
    expect_duration(-5410000, 0, MINUS "5.41 s");
    expect_duration(9994999, 0, "9.99 s");
    expect_duration(9995000, 0, "10 s");
    expect_duration(59600000, 0, "1 min"); /* never "60 s" */
    expect_duration(59499999, 0, "59 s");
    expect_duration(59500000, 0, "1 min");
    expect_duration(INT64_C(3599600000), 0, "1 h"); /* 59 min 59.6 s, 2 parts: 1 h 0 min */
    expect_duration(INT64_C(3599600000), 3, "1 h");
    expect_duration(INT64_C(3599400000), 3, "59 min 59 s");
    expect_duration(INT64_C(3760000000), 2, "1 h 3 min"); /* 1 h 2 min 40 s */
    expect_duration(INT64_C(3760000000), 3, "1 h 2 min 40 s");
    expect_duration(INT64_C(86400000000) * 2 + INT64_C(300000000), 3, "2 d 5 min");
    expect_duration(INT64_C(86400000000) * 1234 + INT64_C(18000000000), 0, "1" NNBSP "234 d 5 h");
    expect_duration(INT64_C(86399500000), 2, "1 d");
    expect_duration(INT64_C(86399500000), 4, "1 d");
    expect_duration(INT64_C(86399499999), 4, "23 h 59 min 59 s");
    expect_duration(INT64_C(-3720000000), 9, MINUS "1 h 2 min"); /* more than 4 parts: 4 */
    /* the magnitude is rounded: a negative half goes away from zero */
    expect_duration(-5000, 2, MINUS "0.01 s");
    expect_duration(-9995000, 2, MINUS "10 s");
    expect_duration(-59500000, 2, MINUS "1 min");
    for (i = 0; i < 20000; i++) {
        int64_t us = (int64_t)(rng() >> (1 + rng() % 63));
        char plus[96], minus[96];
        parts = (unsigned)(rng() % 6);
        if (i % 5 == 0) {
            us = us / 5000 * 5000; /* the ties of the hundredths, and of the seconds */
        }
        if (us <= 0) {
            continue;
        }
        frc_format_duration(us, (uint8_t)parts, plus, sizeof plus);
        frc_format_duration(-us, (uint8_t)parts, minus, sizeof minus);
        if (strcmp(plus, "0 s") == 0) {
            CHECK_STR(minus, "0 s");
        } else {
            snprintf(want, sizeof want, MINUS "%s", plus);
            CHECK_STR(minus, want); /* the same but for the sign */
        }
        CHECK_NOTE("%" PRId64 " us, %u parts", us, parts);
    }
    expect_duration(INT64_C(3720000000), 1, "1 h");
    expect_duration(INT64_MIN, 4, MINUS "106" NNBSP "751" NNBSP "991 d 4 h 55 s");
    expect_duration(INT64_MAX, 1, "106" NNBSP "751" NNBSP "991 d");

    TEST_SECTION("frc_format_duration: random durations against the rule");
    for (i = 0; i < 300000; i++) {
        int64_t us;
        switch (i % 4) {
        case 0: us = (int64_t)(rng() >> (rng() % 64)); break;                  /* any size */
        case 1: us = (int64_t)(rng() % UINT64_C(120000000)); break;              /* under two minutes */
        case 2: us = (int64_t)(rng() % 4) * 500000 + (int64_t)(rng() % 3) - 1    /* on a half second */
                     + (int64_t)(rng() % 200000) * 1000000; break;
        default: us = (int64_t)((rng() % 1000) * UINT64_C(60000000)) + 30000000 - 1 + (int64_t)(rng() % 3); break;
        }
        if ((rng() & 1) && us != INT64_MIN) {
            us = -us;
        }
        for (parts = 0; parts <= 5; parts++) {
            int n = frc_format_duration(us, (uint8_t)parts, got, sizeof got);
            duration_rule(us, parts, want, sizeof want);
            CHECK_EQ_I64(n, (int)strlen(want));
            CHECK_STR(got, want);
            CHECK_NOTE("%" PRId64 " us, %u parts", us, parts);
            CHECK(strstr(got, " 60 s") == NULL && strstr(got, " 60 min") == NULL && strstr(got, " 24 h") == NULL);
            if (strncmp(got, "60 s", 4) == 0 || strncmp(got, "60 min", 6) == 0 || strncmp(got, "24 h", 4) == 0) {
                CHECK(0);
            }
            if (n > 0 && (size_t)n > longest) {
                longest = (size_t)n;
            }
        }
    }
    /* the longest text: the most days, every part two digits */
    frc_format_duration(-(INT64_C(106751990) * INT64_C(86400000000) + INT64_C(86399000000)), 4, got, sizeof got);
    CHECK_STR(got, MINUS "106" NNBSP "751" NNBSP "990 d 23 h 59 min 59 s");
    CHECK_EQ_U64(strlen(got), 37);
    CHECK(longest <= 37);

    TEST_SECTION("frc_format_duration: every buffer size");
    every_size_us = -(INT64_C(86400000000) * 1234 + INT64_C(3599000000));
    duration_rule(every_size_us, 4, want, sizeof want);
    check_every_size(call_duration, want);
    every_size_us = -5410000;
    check_every_size(call_duration, MINUS "5.41 s");
}

/* ------------------------------------------------------------------------- */
/* frc_parse_civil                                                            */
/* ------------------------------------------------------------------------- */

static void expect_parse(const char *text, int32_t y, int mo, int d, int h, int mi, int s, uint32_t us,
                         int has, int32_t offset)
{
    frc_civil_t c;
    int32_t got_offset = 12345;
    int got_has = 7;
    CHECK_EQ_I64(frc_parse_civil(text, &c, &got_offset, &got_has), FRC_OK);
    CHECK_NOTE("\"%s\"", text);
    if (frc_parse_civil(text, &c, &got_offset, &got_has) == FRC_OK) {
        CHECK(c.year == y && c.month == mo && c.day == d && c.hour == h && c.minute == mi && c.second == s
              && c.microsecond == us);
        CHECK_NOTE("\"%s\": %d-%d-%d %d:%d:%d.%u", text, (int)c.year, c.month, c.day, c.hour, c.minute, c.second,
                   (unsigned)c.microsecond);
        CHECK_EQ_I64(c.weekday, weekday_of(y, mo, d));
        CHECK_EQ_I64(got_has, has);
        CHECK_EQ_I64(got_offset, offset);
        CHECK_NOTE("\"%s\"", text);
    }
}

static void expect_refused(const char *text, frc_status_t status)
{
    frc_civil_t c = civil_of(1, 2, 3, 4, 5, 6, 7);
    int32_t offset = 12345;
    int has = 7;
    CHECK_EQ_I64(frc_parse_civil(text, &c, &offset, &has), status);
    CHECK_NOTE("\"%s\"", text);
    /* nothing is written on failure */
    CHECK(c.year == 1 && c.month == 2 && c.day == 3 && c.hour == 4 && c.microsecond == 7);
    CHECK(offset == 12345 && has == 7);
}

static void test_parse(void)
{
    char text[96];
    frc_civil_t c, back;
    int32_t offset;
    int has, i;

    TEST_SECTION("frc_parse_civil: the header's examples");
    expect_parse("2026-10-08", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("2026-10-08 12:00", 2026, 10, 8, 12, 0, 0, 0, 0, 0);
    expect_parse("2026-10-08T12:00:00Z", 2026, 10, 8, 12, 0, 0, 0, 1, 0);
    expect_parse("2026-10-08T14:00:00+02:00", 2026, 10, 8, 14, 0, 0, 0, 1, 7200);
    expect_parse("8 OCT 2026", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("8 OCT 2026 12:00", 2026, 10, 8, 12, 0, 0, 0, 0, 0);
    expect_parse("THU 8 OCT 2026 " MIDDOT " 12:00:00", 2026, 10, 8, 12, 0, 0, 0, 0, 0);
    expect_parse("2026-10-08T12:00:00.000Z", 2026, 10, 8, 12, 0, 0, 0, 1, 0);
    expect_parse("2026-10-08T12:00:00.5", 2026, 10, 8, 12, 0, 0, 500000, 0, 0);
    expect_parse("2026-10-08T12:00:00.123456789", 2026, 10, 8, 12, 0, 0, 123456, 0, 0);
    expect_parse("  8 october 2026  ", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("thu, 8 Oct 2026 9:05", 2026, 10, 8, 9, 5, 0, 0, 0, 0);
    expect_parse("2026-10-08 12:00 UTC", 2026, 10, 8, 12, 0, 0, 0, 1, 0);
    expect_parse("2026-10-08 12:00 GMT", 2026, 10, 8, 12, 0, 0, 0, 1, 0);
    expect_parse("2026-10-08 12:00 UTC+3", 2026, 10, 8, 12, 0, 0, 0, 1, 10800);
    expect_parse("2026-10-08 12:00 UTC" MINUS "2:30", 2026, 10, 8, 12, 0, 0, 0, 1, -9000);
    expect_parse("2026-10-08T12:00+0200", 2026, 10, 8, 12, 0, 0, 0, 1, 7200);
    expect_parse("2026-10-08T12:00-05", 2026, 10, 8, 12, 0, 0, 0, 1, -18000);
    expect_parse("2026-10-08T12:00:00-00:00", 2026, 10, 8, 12, 0, 0, 0, 1, 0);
    expect_parse("-0044-03-15", -44, 3, 15, 0, 0, 0, 0, 0, 0);
    expect_parse(MINUS "0044-03-15", -44, 3, 15, 0, 0, 0, 0, 0, 0);
    expect_parse("+10000-01-01", 10000, 1, 1, 0, 0, 0, 0, 0, 0);
    expect_parse("15 MAR -0044", -44, 3, 15, 0, 0, 0, 0, 0, 0);
    expect_parse("2024-02-29", 2024, 2, 29, 0, 0, 0, 0, 0, 0);
    expect_parse("1 MAY 2026", 2026, 5, 1, 0, 0, 0, 0, 0, 0);
    /* NULL outputs that may be NULL */
    CHECK_EQ_I64(frc_parse_civil("2026-10-08", &c, NULL, NULL), FRC_OK);

    TEST_SECTION("frc_parse_civil: refusals");
    expect_refused("31 FEB 2026", FRC_ERR_RANGE);
    expect_refused("2026-02-29", FRC_ERR_RANGE);
    expect_refused("2026-13-01", FRC_ERR_RANGE);
    expect_refused("2026-00-10", FRC_ERR_RANGE);
    expect_refused("2026-10-32", FRC_ERR_RANGE);
    expect_refused("2026-10-08T24:00", FRC_ERR_RANGE);
    expect_refused("2026-10-08T12:60", FRC_ERR_RANGE);
    expect_refused("2026-10-08T12:00:60", FRC_ERR_RANGE);
    expect_refused("2026-10-08T12:00+24:00", FRC_ERR_RANGE);
    expect_refused("2026-10-08T12:00+02:60", FRC_ERR_RANGE);
    expect_refused("999999-01-01", FRC_ERR_RANGE);
    expect_refused("FRI 8 OCT 2026", FRC_ERR_PARSE); /* not that day's name */
    expect_refused("", FRC_ERR_PARSE);
    expect_refused("   ", FRC_ERR_PARSE);
    expect_refused("8 OCT", FRC_ERR_PARSE);
    expect_refused("OCT 2026", FRC_ERR_PARSE);
    expect_refused("8 OCTO 2026", FRC_ERR_PARSE);
    expect_refused("8 OCT2026", FRC_ERR_PARSE);
    expect_refused("8OCT 2026", FRC_ERR_PARSE);
    expect_refused("+8 OCT 2026", FRC_ERR_PARSE);
    expect_refused("123 OCT 2026", FRC_ERR_PARSE);
    expect_refused("2026-10", FRC_ERR_PARSE);
    expect_refused("2026-10-008", FRC_ERR_PARSE);
    expect_refused("1234567-01-01", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:0", FRC_ERR_PARSE);
    expect_refused("2026-10-08 123:00", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 CEST", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 PM", FRC_ERR_PARSE);
    expect_refused("2026-10-08Z", FRC_ERR_PARSE);
    expect_refused("2026-10-08T", FRC_ERR_PARSE);
    expect_refused("2026-10-08 " MIDDOT, FRC_ERR_PARSE);
    expect_refused("2026-10-08T12:00:00.", FRC_ERR_PARSE);
    expect_refused("2026-10-08T12:00+123", FRC_ERR_PARSE);
    expect_refused("2026-10-08 and more", FRC_ERR_PARSE);
    expect_refused("THURSDAY 8 OCT 2026", FRC_ERR_PARSE);
    CHECK_EQ_I64(frc_parse_civil(NULL, &c, &offset, &has), FRC_ERR_ARG);

    TEST_SECTION("frc_parse_civil: the grammar's edges, one by one");
    /* spaces are ASCII spaces: no tab, newline or carriage return */
    expect_refused("2026-10-08\t12:00", FRC_ERR_PARSE);
    expect_refused("\t2026-10-08", FRC_ERR_PARSE);
    expect_refused("2026-10-08\t", FRC_ERR_PARSE);
    expect_refused("8\tOCT 2026", FRC_ERR_PARSE);
    expect_refused("2026-10-08 \t12:00", FRC_ERR_PARSE);
    expect_refused("2026-10-08\n", FRC_ERR_PARSE);
    expect_refused("2026-10-08\r12:00", FRC_ERR_PARSE);
    /* a weekday is followed by spaces, a ',' or both */
    expect_parse("THU,2026-10-08", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("THU 2026-10-08", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("THU , 8 OCT 2026", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("thu ,8 oct 2026", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("Thu,  8 Oct 2026", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_refused("THU8 OCT 2026", FRC_ERR_PARSE);
    expect_refused("THU2026-10-08", FRC_ERR_PARSE);
    expect_refused("THU,,8 OCT 2026", FRC_ERR_PARSE);
    expect_refused("THU", FRC_ERR_PARSE);
    expect_refused("THU,", FRC_ERR_PARSE);
    /* every weekday and month word is compared whole, letters in any case */
    expect_parse("8 jan 2026", 2026, 1, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("sat 10 oct 2026", 2026, 10, 10, 0, 0, 0, 0, 0, 0);
    expect_parse("Mon 12 Oct 2026", 2026, 10, 12, 0, 0, 0, 0, 0, 0);
    expect_parse("8 aUgUsT 2026", 2026, 8, 8, 0, 0, 0, 0, 0, 0);
    expect_refused("MON 8 OCT 2026", FRC_ERR_PARSE); /* a Thursday */
    expect_refused("8 XCT 2026", FRC_ERR_PARSE);
    expect_refused("8 OXT 2026", FRC_ERR_PARSE);
    expect_refused("8 OCX 2026", FRC_ERR_PARSE);
    expect_refused("XHU 8 OCT 2026", FRC_ERR_PARSE);
    expect_refused("8 OCTOBERX 2026", FRC_ERR_PARSE);
    expect_refused("8 OCTOBE 2026", FRC_ERR_PARSE);
    /* the ISO date's digits and hyphens */
    expect_parse("2026-1-8", 2026, 1, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("26-10-08", 26, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_refused("2026-010-08", FRC_ERR_PARSE);
    expect_refused("2026--10-08", FRC_ERR_PARSE);
    expect_refused("2026-10x08", FRC_ERR_PARSE);
    expect_refused("2026-10-", FRC_ERR_PARSE);
    expect_refused("2026-10-08-", FRC_ERR_PARSE);
    expect_refused("2026 -10-08", FRC_ERR_PARSE);
    /* the worded date: day, month, year parted by spaces */
    expect_parse("8   OCT   2026", 2026, 10, 8, 0, 0, 0, 0, 0, 0);
    expect_parse("15 MAR " MINUS "0044", -44, 3, 15, 0, 0, 0, 0, 0, 0);
    expect_refused("8 OCT ", FRC_ERR_PARSE);
    expect_refused("8 OCT -", FRC_ERR_PARSE);
    expect_refused("8OCT2026", FRC_ERR_PARSE);
    expect_refused("8 OCT 1234567", FRC_ERR_PARSE);
    /* 'T' only after an ISO date, and at once */
    expect_refused("8 OCT 2026T12:00", FRC_ERR_PARSE);
    expect_refused("2026-10-08 T12:00", FRC_ERR_PARSE);
    expect_refused("2026-10-08T:00", FRC_ERR_PARSE);
    expect_refused("2026-10-08T 12:00", FRC_ERR_PARSE);
    expect_parse("2026-10-08t12:00", 2026, 10, 8, 12, 0, 0, 0, 0, 0);
    /* the time's digits */
    expect_refused("2026-10-08 12:00:5", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00:500", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:000", FRC_ERR_PARSE);
    expect_refused("2026-10-08 :00", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00.5", FRC_ERR_PARSE); /* a fraction needs seconds */
    expect_refused("2026-10-08 12:00:00.1234567890", FRC_ERR_PARSE); /* ten digits */
    expect_parse("2026-10-08 12:00:00.999999999", 2026, 10, 8, 12, 0, 0, 999999, 0, 0);
    expect_parse("2026-10-08 12:00:00.000001", 2026, 10, 8, 12, 0, 0, 1, 0, 0);
    expect_parse("2026-10-08 " MIDDOT "12:00", 2026, 10, 8, 12, 0, 0, 0, 0, 0);
    expect_parse("2026-10-08" MIDDOT "12:00", 2026, 10, 8, 12, 0, 0, 0, 0, 0);
    expect_refused("2026-10-08 " MIDDOT, FRC_ERR_PARSE);
    expect_refused("2026-10-08 " MIDDOT " ", FRC_ERR_PARSE);
    expect_refused("2026-10-08 \xC2", FRC_ERR_PARSE); /* half a dot, then the end */
    /* the offset's shapes */
    expect_parse("2026-10-08 12:00 +2", 2026, 10, 8, 12, 0, 0, 0, 1, 7200);
    expect_parse("2026-10-08 12:00 +1:00", 2026, 10, 8, 12, 0, 0, 0, 1, 3600);
    expect_parse("2026-10-08 12:00 +2:30", 2026, 10, 8, 12, 0, 0, 0, 1, 9000);
    expect_parse("2026-10-08 12:00 UTC+0545", 2026, 10, 8, 12, 0, 0, 0, 1, 20700);
    expect_parse("2026-10-08 12:00 UTC+3:00", 2026, 10, 8, 12, 0, 0, 0, 1, 10800);
    expect_parse("2026-10-08 12:00 GMT-5", 2026, 10, 8, 12, 0, 0, 0, 1, -18000);
    expect_parse("2026-10-08 12:00 utc", 2026, 10, 8, 12, 0, 0, 0, 1, 0);
    expect_parse("2026-10-08 12:00 z", 2026, 10, 8, 12, 0, 0, 0, 1, 0);
    expect_parse("2026-10-08 12:00 -00:00", 2026, 10, 8, 12, 0, 0, 0, 1, 0);
    expect_parse("2026-10-08 12:00 " MINUS "05", 2026, 10, 8, 12, 0, 0, 0, 1, -18000);
    expect_refused("2026-10-08 12:00 EST", FRC_ERR_PARSE);  /* no zone names */
    expect_refused("2026-10-08 12:00 UTX", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 XTC", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 GMX", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 UTC +3", FRC_ERR_PARSE); /* the offset follows UTC at once */
    expect_refused("2026-10-08 12:00 +230", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 UTC+123", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 +02:3", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 +02:", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00+", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 +", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 +02000", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 ZZ", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 Z+2", FRC_ERR_PARSE);
    expect_refused("2026-10-08 12:00 Zulu", FRC_ERR_PARSE);
    expect_refused("2026-10-08 Z", FRC_ERR_PARSE); /* an offset after a time only */
    /* which refusal wins: the shape, then the range, then the weekday */
    expect_refused("FRI 8 OCT 2026 24:00", FRC_ERR_RANGE);
    expect_refused("SAT 29 FEB 2024", FRC_ERR_PARSE); /* a real day, a Thursday */
    expect_refused("MON 31 FEB 2026", FRC_ERR_RANGE);
    expect_refused("FRI 31 FEB 2026 12:00 EST", FRC_ERR_PARSE);
    /* the years: as far as frc_time_from_civil goes, and no further */
    expect_parse("290000-12-31", 290000, 12, 31, 0, 0, 0, 0, 0, 0);
    expect_parse("-290000-01-01", -290000, 1, 1, 0, 0, 0, 0, 0, 0);
    expect_refused("290001-01-01", FRC_ERR_RANGE);
    expect_refused("-290001-12-31", FRC_ERR_RANGE);
    CHECK_EQ_I64(frc_parse_civil("2026-10-08", NULL, &offset, &has), FRC_ERR_ARG);

    TEST_SECTION("frc_parse_civil: round trips through frc_format_civil and frc_format_civil_text");
    for (i = 0; i < 100000; i++) {
        frc_time_t t = (frc_time_t)(rng() % UINT64_C(9000000000000000000)) * ((rng() & 1) ? 1 : -1);
        int32_t zone = (int32_t)((int64_t)(rng() % 95) - 47) * 900;
        frc_civil_from_time(t, zone, &c);
        /* ISO, to the millisecond */
        CHECK(frc_format_civil(&c, zone, text, sizeof text) > 0);
        CHECK_EQ_I64(frc_parse_civil(text, &back, &offset, &has), FRC_OK);
        CHECK_NOTE("\"%s\"", text);
        CHECK(back.year == c.year && back.month == c.month && back.day == c.day && back.hour == c.hour
              && back.minute == c.minute && back.second == c.second && back.weekday == c.weekday
              && back.microsecond == c.microsecond / 1000 * 1000);
        CHECK(has == 1 && offset == zone);
        /* the date line, to the second */
        CHECK(frc_format_civil_text(&c, zone, FRC_CIVIL_DATE_TIME, text, sizeof text) > 0);
        CHECK_EQ_I64(frc_parse_civil(text, &back, &offset, &has), FRC_OK);
        CHECK_NOTE("\"%s\"", text);
        CHECK(back.year == c.year && back.month == c.month && back.day == c.day && back.hour == c.hour
              && back.minute == c.minute && back.second == c.second && back.microsecond == 0);
        CHECK(has == 0 && offset == 0);
        /* the other forms that show a whole date */
        {
            static const frc_civil_form_t dates[3] = {FRC_CIVIL_DATE, FRC_CIVIL_DAY_MONTH_YEAR, FRC_CIVIL_ISO_DATE};
            int k;
            for (k = 0; k < 3; k++) {
                CHECK(frc_format_civil_text(&c, zone, dates[k], text, sizeof text) > 0);
                CHECK_EQ_I64(frc_parse_civil(text, &back, &offset, &has), FRC_OK);
                CHECK_NOTE("\"%s\"", text);
                CHECK(back.year == c.year && back.month == c.month && back.day == c.day && back.hour == 0);
            }
            CHECK(frc_format_civil_text(&c, zone, FRC_CIVIL_DATE_TIME, text, sizeof text) > 0);
        }
        /* the date line with the offset form after it */
        {
            char line[128];
            size_t n = strlen(text);
            snprintf(line, sizeof line, "%s ", text);
            CHECK(frc_format_civil_text(NULL, zone, FRC_CIVIL_OFFSET, line + n + 1, sizeof line - n - 1) > 0);
            CHECK_EQ_I64(frc_parse_civil(line, &back, &offset, &has), FRC_OK);
            CHECK_NOTE("\"%s\"", line);
            CHECK(has == 1 && offset == zone);
        }
    }

    TEST_SECTION("frc_parse_civil: fuzz (no crash, no read past the NUL, nothing written unless FRC_OK)");
    for (i = 0; i < 300000; i++) {
        static const char alphabet[] = "0123456789-+:T .ZzUTCGMOFEBAYJNLPSVD\xE2\x88\x92\xC2\xB7,\t";
        char fuzz[40];
        size_t n = (size_t)(rng() % 32), k;
        frc_status_t status;
        if (i % 2 == 0) {
            /* a valid text, one byte changed */
            frc_civil_from_time((frc_time_t)(rng() % UINT64_C(4000000000000000)), 3600, &c);
            frc_format_civil(&c, 3600, fuzz, sizeof fuzz);
            n = strlen(fuzz);
            fuzz[rng() % n] = alphabet[rng() % (sizeof alphabet - 1)];
        } else {
            for (k = 0; k < n; k++) {
                fuzz[k] = (rng() % 8 == 0) ? (char)(rng() & 0xFF) : alphabet[rng() % (sizeof alphabet - 1)];
                if (fuzz[k] == '\0') {
                    fuzz[k] = 'x';
                }
            }
            fuzz[n] = '\0';
        }
        back = civil_of(1, 2, 3, 4, 5, 6, 7);
        offset = 12345;
        has = 7;
        {
            /* an exact-length copy on the heap: under check-sanitize a read
             * one byte past the NUL is an error */
            size_t length = strlen(fuzz);
            char *exact = (char *)malloc(length + 1);
            CHECK(exact != NULL);
            if (exact == NULL) {
                continue;
            }
            memcpy(exact, fuzz, length + 1);
            status = frc_parse_civil(exact, &back, &offset, &has);
            free(exact);
        }
        CHECK(status == FRC_OK || status == FRC_ERR_PARSE || status == FRC_ERR_RANGE);
        if (status == FRC_OK) {
            frc_civil_t again;
            int32_t again_offset;
            int again_has;
            CHECK_EQ_I64(frc_civil_validate(&back), FRC_OK);
            CHECK(has == 0 || has == 1);
            CHECK(offset > -86400 && offset < 86400 && offset % 60 == 0);
            CHECK(has == 1 || offset == 0);
            CHECK(strchr(fuzz, '\t') == NULL); /* spaces are spaces */
            /* what was read is a moment the ISO writer and the parser agree on */
            CHECK(frc_format_civil(&back, offset, text, sizeof text) > 0);
            CHECK_EQ_I64(frc_parse_civil(text, &again, &again_offset, &again_has), FRC_OK);
            CHECK(again.year == back.year && again.month == back.month && again.day == back.day
                  && again.hour == back.hour && again.minute == back.minute && again.second == back.second
                  && again.microsecond == back.microsecond / 1000 * 1000 && again_offset == offset);
            CHECK_NOTE("\"%s\" -> \"%s\"", fuzz, text);
        } else {
            CHECK(back.year == 1 && back.month == 2 && offset == 12345 && has == 7);
        }
    }
}

int main(void)
{
    test_validate();
    test_civil_text();
    test_duration();
    test_parse();
    return TEST_SUMMARY("earth");
}
