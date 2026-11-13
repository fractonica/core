/*
 * frc_civil.c - civil time: the Gregorian translation of an instant.
 *
 * Implements "Civil time" of fractonica.h and the calendar repair of SPEC
 * S2.3. The clock itself never calls this file: it is for clients that show
 * Earth time beside the clock, and for building a catalogue table.
 *
 * THE MODEL
 *   An instant is Unix microseconds with every day 86,400 s long (SPEC S2.1),
 *   so a day number is floor(t / day) and the time of day is the rest; no
 *   leap seconds, no time-zone rules. A zone is a fixed offset in seconds.
 *   The calendar is the PROLEPTIC Gregorian one for any year, with
 *   astronomical numbering (year 0 is 1 BC, year -1 is 2 BC).
 *
 *   Day number <-> date uses the 400-year "era" of 146,097 days and a year
 *   that starts on 1 March, so the leap day is the last day of the year and
 *   the month lengths follow one formula, (153 m + 2) / 5.
 *
 *   frc_civil_validate says whether fields name a real day and time; the
 *   wordings of Earth mode built on these (frc_format_civil_text, durations,
 *   frc_parse_civil) are in frc_earth.c.
 *
 * INVARIANTS
 *   - Every division is a FLOOR division, so instants before 1970 and years
 *     before 0 need no special case.
 *   - No signed overflow anywhere: a difference that could leave int64_t is
 *     taken in uint64_t (it wraps, the true result is small), and
 *     frc_time_from_civil refuses years whose instants would not fit.
 *   - Weekday 0 is Monday; 1970-01-01 was a Thursday (3).
 */
#include "frc_internal.h"

#define US_PER_SECOND   1000000
#define SECONDS_PER_DAY 86400
#define MS_PER_DAY      86400000

#define DAYS_PER_ERA    146097 /* 400 Gregorian years                          */
#define YEARS_PER_ERA   400
#define ERA_EPOCH_DAYS  719468 /* days from 0000-03-01 to 1970-01-01           */
#define WEEKDAY_OF_EPOCH 3     /* Thursday, counting from Monday = 0           */

/* frc_time_t reaches from year -290308 to +294247. Years inside this limit
 * leave room for the overflowing fields frc_time_from_civil accepts (255
 * months, 255 days, hours ..., a 2^31-second offset) on both sides. */
#define YEAR_LIMIT 290000

/* a - b * floor(a / b): the remainder that goes with frc_i_floor_div, in 0 .. b-1.
 * In uint64_t because b * q may pass INT64_MIN when a is close to it; the
 * wrapped difference is the true one, which is small. */
static int64_t floor_mod(int64_t a, int64_t b)
{
    return (int64_t)((uint64_t)a - (uint64_t)b * (uint64_t)frc_i_floor_div(a, b));
}

/* Day number (0 = 1970-01-01) of the FIRST day of a month, month 1..12. */
static int64_t days_from_month(int64_t year, int64_t month)
{
    /* Count years from 1 March: January and February belong to the year before. */
    int64_t y = (month <= 2) ? year - 1 : year;
    int64_t month_from_march = (month > 2) ? month - 3 : month + 9;
    int64_t era = frc_i_floor_div(y, YEARS_PER_ERA);
    int64_t year_of_era = y - era * YEARS_PER_ERA; /* 0..399 */
    int64_t day_of_year = (153 * month_from_march + 2) / 5;
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * DAYS_PER_ERA + day_of_era - ERA_EPOCH_DAYS;
}

/* The date of a day number. The inverse of days_from_month (+ day - 1). */
static void date_from_days(int64_t days, int64_t *year, uint8_t *month, uint8_t *day)
{
    int64_t z = days + ERA_EPOCH_DAYS;
    int64_t era = frc_i_floor_div(z, DAYS_PER_ERA);
    int64_t day_of_era = z - era * DAYS_PER_ERA; /* 0..146096 */
    /* take out the leap days already passed in the era, then whole years */
    int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524
                           - day_of_era / (DAYS_PER_ERA - 1)) / 365; /* 0..399 */
    int64_t day_of_year = day_of_era
                          - (365 * year_of_era + year_of_era / 4 - year_of_era / 100); /* 0..365 */
    int64_t month_from_march = (5 * day_of_year + 2) / 153; /* 0..11 */
    int64_t m = (month_from_march < 10) ? month_from_march + 3 : month_from_march - 9;

    *day = (uint8_t)(day_of_year - (153 * month_from_march + 2) / 5 + 1);
    *month = (uint8_t)m;
    *year = year_of_era + era * YEARS_PER_ERA + (m <= 2 ? 1 : 0);
}

void frc_civil_from_time(frc_time_t t, int32_t utc_offset_s, frc_civil_t *out)
{
    int64_t days, within, extra_days, year;
    uint32_t second_of_day;

    if (out == NULL) {
        return;
    }
    /* Split into day and time of day BEFORE adding the offset: t + offset
     * could leave int64_t at the far ends, time of day + offset cannot. */
    days = frc_i_floor_div(t, FRC_US_PER_DAY);
    within = floor_mod(t, FRC_US_PER_DAY) + (int64_t)utc_offset_s * US_PER_SECOND;
    extra_days = frc_i_floor_div(within, FRC_US_PER_DAY);
    days += extra_days;
    within -= extra_days * FRC_US_PER_DAY; /* 0 .. one day - 1 */

    date_from_days(days, &year, &out->month, &out->day);
    out->year = (int32_t)year;
    out->weekday = (uint8_t)floor_mod(days + WEEKDAY_OF_EPOCH, 7);
    second_of_day = (uint32_t)(within / US_PER_SECOND);
    out->microsecond = (uint32_t)(within % US_PER_SECOND);
    out->hour = (uint8_t)(second_of_day / 3600u);
    out->minute = (uint8_t)(second_of_day / 60u % 60u);
    out->second = (uint8_t)(second_of_day % 60u);
}

frc_time_t frc_time_from_civil(const frc_civil_t *civil, int32_t utc_offset_s)
{
    int64_t months, year, days, seconds;

    if (civil == NULL) {
        return FRC_TIME_NONE;
    }
    /* The rule is LINEAR in every field, which is what "fields may overflow"
     * means: month 13 is January of the next year (carried into the year
     * first, because months differ in length), and then each field just adds
     * its length: day 32 is 31 days after the 1st, hour 24 is 24 hours after
     * midnight. By the same rule month 0 and day 0 are "one before the first". */
    months = (int64_t)civil->month - 1;
    year = (int64_t)civil->year + frc_i_floor_div(months, 12);
    if (year < -YEAR_LIMIT || year > YEAR_LIMIT) {
        return FRC_TIME_NONE; /* the instant does not fit frc_time_t */
    }
    days = days_from_month(year, floor_mod(months, 12) + 1) + ((int64_t)civil->day - 1);
    seconds = days * SECONDS_PER_DAY + (int64_t)civil->hour * 3600 + (int64_t)civil->minute * 60
              + (int64_t)civil->second - (int64_t)utc_offset_s;
    return seconds * US_PER_SECOND + (int64_t)civil->microsecond;
}

/* Section 11 of frc_internal.h: the weekday of a date, 0 = Monday. */
uint8_t frc_i_civil_weekday(int32_t year, uint8_t month, uint8_t day)
{
    int64_t days = days_from_month(year, month) + (int64_t)day - 1;
    return (uint8_t)floor_mod(days + WEEKDAY_OF_EPOCH, 7);
}

frc_status_t frc_civil_validate(const frc_civil_t *civil)
{
    int64_t next_year, next_month, length;

    if (civil == NULL) {
        return FRC_ERR_ARG;
    }
    if (civil->year < -YEAR_LIMIT || civil->year > YEAR_LIMIT || civil->month < 1 || civil->month > 12
        || civil->day < 1 || civil->hour > 23 || civil->minute > 59 || civil->second > 59
        || civil->microsecond >= US_PER_SECOND) {
        return FRC_ERR_RANGE;
    }
    /* the month's length: from its first day to the next month's first day */
    next_year = (civil->month == 12) ? (int64_t)civil->year + 1 : (int64_t)civil->year;
    next_month = (civil->month == 12) ? 1 : (int64_t)civil->month + 1;
    length = days_from_month(next_year, next_month) - days_from_month(civil->year, civil->month);
    return ((int64_t)civil->day <= length) ? FRC_OK : FRC_ERR_RANGE;
}

int frc_format_civil(const frc_civil_t *civil, int32_t utc_offset_s, char *buf, size_t size)
{
    frc_i_text_t w;
    int64_t year, offset_minutes;

    /* Only a real date and time of day is written: normalise overflowing
     * fields with frc_time_from_civil + frc_civil_from_time first. */
    if (civil == NULL || civil->month < 1 || civil->month > 12 || civil->day < 1 || civil->day > 31
        || civil->hour > 23 || civil->minute > 59 || civil->second > 59
        || civil->microsecond >= US_PER_SECOND) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);

    /* ISO 8601: four digits for 0..9999; outside that a sign is required,
     * "-0044" for 45 BC and "+10000" beyond the four digits. */
    year = civil->year;
    if (year < 0) {
        frc_i_text_char(&w, '-');
        year = -year;
    } else if (year > 9999) {
        frc_i_text_char(&w, '+');
    }
    frc_i_text_u64_pad(&w, (uint64_t)year, 4);
    frc_i_text_char(&w, '-');
    frc_i_text_u64_pad(&w, civil->month, 2);
    frc_i_text_char(&w, '-');
    frc_i_text_u64_pad(&w, civil->day, 2);
    frc_i_text_char(&w, 'T');
    frc_i_text_u64_pad(&w, civil->hour, 2);
    frc_i_text_char(&w, ':');
    frc_i_text_u64_pad(&w, civil->minute, 2);
    frc_i_text_char(&w, ':');
    frc_i_text_u64_pad(&w, civil->second, 2);
    frc_i_text_char(&w, '.');
    /* milliseconds, TRUNCATED: the text never names a later instant than the fields */
    frc_i_text_u64_pad(&w, civil->microsecond / 1000u, 3);

    if (utc_offset_s == 0) {
        frc_i_text_char(&w, 'Z');
    } else {
        /* whole minutes of the offset, towards zero; the sign is the offset's */
        offset_minutes = (int64_t)utc_offset_s / 60;
        frc_i_text_char(&w, utc_offset_s < 0 ? '-' : '+');
        if (offset_minutes < 0) {
            offset_minutes = -offset_minutes;
        }
        frc_i_text_u64_pad(&w, (uint64_t)(offset_minutes / 60), 2);
        frc_i_text_char(&w, ':');
        frc_i_text_u64_pad(&w, (uint64_t)(offset_minutes % 60), 2);
    }
    return frc_i_text_finish(&w);
}

int64_t frc_julian_fields_to_unix_ms(int64_t unix_ms)
{
    /* SPEC S2.3. A source stored a JULIAN calendar date as if its year, month
     * and day were Gregorian. Read those fields back out of the value, then
     * count the days of that date in the Julian calendar instead. */
    int64_t days = frc_i_floor_div(unix_ms, MS_PER_DAY);
    int64_t within = floor_mod(unix_ms, MS_PER_DAY); /* the time of day is kept */
    int64_t year, a, y, m, jdn;
    uint8_t month, day;

    date_from_days(days, &year, &month, &day);

    /* Julian Day Number of a Julian-calendar date: years from 1 March of
     * -4800, a leap year every fourth, no century rule. */
    a = (14 - month) / 12; /* 1 for January and February */
    y = year + 4800 - a;
    m = month + 12 * a - 3;
    jdn = day + (153 * m + 2) / 5 + 365 * y + frc_i_floor_div(y, 4) - 32083;

    /* JDN 2440588 is 1970-01-01. Unsigned so that a value at the very end of
     * int64_t wraps instead of overflowing (it has no date anyway). */
    return (int64_t)((uint64_t)(jdn - 2440588) * (uint64_t)MS_PER_DAY + (uint64_t)within);
}
