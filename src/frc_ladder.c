/*
 * frc_ladder.c - the spectral ladder and the spatial units.
 *
 * Implements SPEC S16: the period and frequency of every level 0..62 (S16.1),
 * the bands and their edges (S16.2), the media and lengths (S16.3), and the numeric rules of
 * S16.4: exact rationals, one rounding, devices that count a rung. It also
 * writes those values as S16.1 tabulates them ("310.9 ms", "2.454 x 10^-25 s").
 *
 * THE MODEL
 *   An interval is num/den MILLISECONDS (frc_interval_t). For level L
 *
 *       T(L) = 4 num / (1000 den P(L))   seconds      = top / bottom
 *       f(L) = 1000 den P(L) / (4 num)   hertz        = bottom / top
 *       length(L) = v_num T(L) / v_den   metres
 *
 *   `top` and `bottom` are big integers (frc_big.h); period_parts builds them
 *   and everything else in this file is one of those three quotients, taken
 *   apart in a different way: 17 digits (frc_sci_t), a double, a band, a
 *   device count, a text.
 *
 * INVARIANTS
 *   - NOTHING IS COMPUTED IN FLOATING POINT and nothing walks down the ladder
 *     rung by rung. Every result is rounded ONCE from the exact quotient.
 *     The functions that return a double (inside #ifndef FRC_NO_FLOAT) round
 *     the same exact quotient to 53 bits themselves, then only scale by a
 *     power of two, which is exact.
 *   - Rounding: frc_sci_t, doubles and texts round half to even; device
 *     counts round half up (S16.4).
 *   - Levels are 0..62 (FRC_LEVEL_COUNT). An interval or a speed with a zero
 *     numerator or denominator is refused with FRC_ERR_ARG.
 *
 * OVERFLOW BOUNDS (the big integer has 384 bits and does not check)
 *   num, den, v_num, v_den, rate < 2^64 and P(62) < 2^172, so
 *       top    = 4 num (v_num) (rate)           < 2^130
 *       bottom = 1000 den P (v_den)             < 2^310
 *   A quotient is first NORMALISED to 1 <= n/d < 10 by multiplying the
 *   smaller side by tens (ratio_normalise): after it n < 10 * 2^310 < 2^314
 *   and d <= 2^310. The largest numbers made from there:
 *       17 digits      n * 10^16                           < 2^368
 *       civil units    d * 31,557,600 (the year)           < 2^335
 *                      then n < 10 d, times 10^8           < 2^366
 *       a double       the smaller side shifted until the
 *                      quotient has 56 bits                < 2^366
 *       a phase step   bottom * 2^64                       < 2^310
 */
#include "frc_big.h"

/* ------------------------------------------------------------------------- */
/* Small integer helpers                                                      */
/* ------------------------------------------------------------------------- */

#define LADDER_MAX_LEVEL (FRC_LEVEL_COUNT - 1)

/* The largest powers that fit 64 bits: 10^19 < 2^64 and 5^27 < 2^64. */
#define POW10_CHUNK 19
#define POW5_CHUNK  27

/* 10^exponent. REQUIRES exponent <= POW10_CHUNK (frc_i_pow_u64 gives 0 beyond). */
static uint64_t pow10_u64(uint8_t exponent)
{
    return frc_i_pow_u64(10, exponent);
}

/* a = a * base^exponent, `chunk` powers at a time (POW10_CHUNK or POW5_CHUNK). */
static void mul_power(frc_i_big_t *a, uint8_t base, uint8_t chunk, uint16_t exponent)
{
    while (exponent > 0) {
        uint8_t step = exponent < chunk ? (uint8_t)exponent : chunk;
        frc_i_big_mul_u64(a, a, frc_i_pow_u64(base, step));
        exponent = (uint16_t)(exponent - step);
    }
}

/* ------------------------------------------------------------------------- */
/* The exact quotients                                                        */
/* ------------------------------------------------------------------------- */

/*
 * The two integers a level is made of: T(L) = top / bottom seconds and
 * f(L) = bottom / top hertz, with top = 4 num and bottom = 1000 den P(L).
 * @return FRC_ERR_ARG for level > 62 or a zero num / den.
 *
 * The interval (and a speed, below) is taken BY ADDRESS all the way down from
 * the public functions, which receive it by value: handing the 16-byte struct
 * on by value again would make riscv32 gcc -Os and ARMv6-M gcc copy it with a
 * call to memcpy.
 */
static frc_status_t period_parts(uint8_t level, const frc_interval_t *interval,
                                 frc_i_big_t *top, frc_i_big_t *bottom)
{
    if (level > LADDER_MAX_LEVEL || interval->num == 0 || interval->den == 0) {
        return FRC_ERR_ARG;
    }
    frc_i_big_set_u64(top, interval->num);
    frc_i_big_mul_u64(top, top, 4);
    frc_i_big_set_u64(bottom, interval->den);
    frc_i_big_mul_u64(bottom, bottom, 1000);
    /* P(8 a + b) = P(8)^a * P(b), because the factor sequence repeats every
     * 8 levels. Both factors come from the 64-bit table of frc_prefix. */
    while (level >= FRC_SEQUENCE_LENGTH) {
        frc_i_big_mul_u64(bottom, bottom, frc_prefix(FRC_SEQUENCE_LENGTH));
        level = (uint8_t)(level - FRC_SEQUENCE_LENGTH);
    }
    frc_i_big_mul_u64(bottom, bottom, frc_prefix(level));
    return FRC_OK;
}

/*
 * A positive value n / d * 10^e. The power of ten is kept apart so that a
 * value can be brought to 1 <= n/d < 10 ("normalised") without dividing:
 * then e is its decimal exponent, floor(log10(value)).
 */
typedef struct ratio {
    frc_i_big_t n;
    frc_i_big_t d;
    int32_t     e;
} ratio_t;

static frc_status_t ratio_period(uint8_t level, const frc_interval_t *interval, ratio_t *x)
{
    x->e = 0;
    return period_parts(level, interval, &x->n, &x->d);
}

static frc_status_t ratio_frequency(uint8_t level, const frc_interval_t *interval, ratio_t *x)
{
    x->e = 0;
    return period_parts(level, interval, &x->d, &x->n);
}

static frc_status_t ratio_length(uint8_t level, const frc_interval_t *interval,
                                 const frc_speed_t *speed, ratio_t *x)
{
    frc_status_t status = ratio_period(level, interval, x);
    if (status != FRC_OK) {
        return status;
    }
    if (speed->num == 0 || speed->den == 0) {
        return FRC_ERR_ARG;
    }
    frc_i_big_mul_u64(&x->n, &x->n, speed->num);
    frc_i_big_mul_u64(&x->d, &x->d, speed->den);
    return FRC_OK;
}

/* Bring x to d <= n < 10 d, moving the tens into e. REQUIRES n > 0 and d > 0. */
static void ratio_normalise(ratio_t *x)
{
    frc_i_big_t ten_d;
    while (frc_i_big_cmp(&x->n, &x->d) < 0) {
        frc_i_big_mul_u64(&x->n, &x->n, 10);
        x->e--;
    }
    for (;;) {
        frc_i_big_mul_u64(&ten_d, &x->d, 10);
        if (frc_i_big_cmp(&x->n, &ten_d) < 0) {
            return;
        }
        frc_i_big_mul_u64(&x->d, &x->d, 10);
        x->e++;
    }
}

/*
 * The first `digits` (1..17) significant digits of a NORMALISED x, rounded
 * half to even from the exact value: an integer r with `digits` digits such
 * that x is r * 10^(e - digits + 1) to that precision. A value that rounds up
 * to 10^digits (9.9996 -> 10.00) comes back as 10^(digits-1) with e raised by
 * one, so r always has exactly `digits` digits.
 * It uses x up: n is left scaled, only x->e and x->d stay meaningful.
 */
static uint64_t ratio_round(ratio_t *x, uint8_t digits)
{
    frc_i_big_t quotient, rem;
    uint64_t one_more = pow10_u64(digits); /* 10^digits: one digit too many */
    uint64_t r;
    int half;

    /* n / d < 10, so n * 10^(digits-1) / d < 10^digits fits 64 bits */
    frc_i_big_mul_u64(&x->n, &x->n, one_more / 10u);
    frc_i_big_divmod(&x->n, &x->d, &quotient, &rem);
    r = frc_i_big_low_u64(&quotient);
    half = frc_i_big_cmp_half(&rem, &x->d);
    if (half > 0 || (half == 0 && (r & 1u) != 0)) {
        r++;
    }
    if (r == one_more) {
        r = one_more / 10u;
        x->e++;
    }
    return r;
}

/* 1 when a NORMALISED x is at least `whole` (an integer below 10^9). Exact. */
static int ratio_reaches(const ratio_t *x, uint32_t whole)
{
    frc_i_big_t scaled_n, scaled_d;
    if (x->e < 0) {
        return 0; /* x < 1 */
    }
    if (x->e > 8) {
        return 1; /* x >= 10^9 */
    }
    /* n / d * 10^e >= whole  <=>  n * 10^e >= whole * d */
    frc_i_big_mul_u64(&scaled_n, &x->n, pow10_u64((uint8_t)x->e));
    frc_i_big_mul_u64(&scaled_d, &x->d, whole);
    return frc_i_big_cmp(&scaled_n, &scaled_d) >= 0;
}

/* x as 17 significant digits (frc_sci_t). Uses x up. */
static frc_status_t ratio_to_sci(ratio_t *x, frc_sci_t *out)
{
    int32_t exp10;
    ratio_normalise(x);
    out->mantissa = ratio_round(x, FRC_SCI_DIGITS);
    /* the mantissa is an integer of 17 digits: its last digit weighs 10^(e-16) */
    exp10 = x->e - (FRC_SCI_DIGITS - 1);
    if (exp10 < INT16_MIN || exp10 > INT16_MAX) {
        return FRC_ERR_OVERFLOW; /* no value of the ladder comes near: see the bounds above */
    }
    out->exp10 = (int16_t)exp10;
    return FRC_OK;
}

static int sci_ok(const frc_sci_t *value)
{
    return value != NULL && value->mantissa >= pow10_u64(FRC_SCI_DIGITS - 1)
           && value->mantissa < pow10_u64(FRC_SCI_DIGITS);
}

/* ------------------------------------------------------------------------- */
/* S16.1  Intervals, periods and frequencies                                  */
/* ------------------------------------------------------------------------- */

frc_status_t frc_interval_at(const frc_catalogue_t *cat, frc_time_t t, frc_interval_t *out)
{
    frc_position_t pos;
    frc_status_t status;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    /* frc_position_at already applies the rule of S16.1: an eclipse belongs to
     * the interval it opens, the closing instant to the last interval. */
    status = frc_position_at(cat, t, &pos);
    if (status != FRC_OK) {
        return status;
    }
    /* pos.den is that interval in microseconds; the catalogue is in whole
     * milliseconds, so the division is exact */
    out->num = pos.den / 1000u;
    out->den = 1;
    return FRC_OK;
}

frc_interval_t frc_interval_mean(const frc_catalogue_t *cat)
{
    uint64_t num = 0; /* 0/0: "no interval"; every function here refuses it */
    uint64_t den = 0;
    if (frc_i_cat_ok(cat)) {
        uint16_t last = (uint16_t)(cat->count - 1);
        /* (E[n] - E[0]) / n, left as SPEC S16.5 writes it (not reduced). Unsigned:
         * the difference of two int64 may exceed INT64_MAX. */
        num = (uint64_t)FRC_PGM_I64(&cat->instants_ms[last])
              - (uint64_t)FRC_PGM_I64(&cat->instants_ms[0]);
        den = last;
    }
    /* A struct that is returned by value is built in the return statement: a
     * local one that is filled and then returned is copied out with a call to
     * memcpy by xtensa and ARMv6-M gcc at -O0. */
    return (frc_interval_t){num, den};
}

frc_interval_t frc_interval_of_alpha_ms(uint64_t alpha_ms)
{
    /* an Alpha is four Saros intervals; the fraction keeps alpha / 4 exact */
    return (frc_interval_t){alpha_ms, 4};
}

frc_status_t frc_period(uint8_t level, frc_interval_t interval, frc_sci_t *seconds)
{
    ratio_t x;
    frc_status_t status;
    if (seconds == NULL) {
        return FRC_ERR_ARG;
    }
    status = ratio_period(level, &interval, &x);
    return status == FRC_OK ? ratio_to_sci(&x, seconds) : status;
}

frc_status_t frc_frequency(uint8_t level, frc_interval_t interval, frc_sci_t *hertz)
{
    ratio_t x;
    frc_status_t status;
    if (hertz == NULL) {
        return FRC_ERR_ARG;
    }
    status = ratio_frequency(level, &interval, &x);
    return status == FRC_OK ? ratio_to_sci(&x, hertz) : status;
}

/* ------------------------------------------------------------------------- */
/* S16.3  Media and lengths                                                   */
/* ------------------------------------------------------------------------- */

/* The table of S16.3 by frc_medium_t. Speeds are num / den metres per second. */
static const uint32_t MEDIUM_SPEED_NUM[FRC_MEDIUM_COUNT] FRC_PROGMEM = {
    FRC_I_LIGHT_SPEED, 200000000u, 18000u, 1500u, 343u, 120u, 1u};
static const uint16_t MEDIUM_SPEED_DEN[FRC_MEDIUM_COUNT] FRC_PROGMEM = {
    1, 1, 1, 1, 1, 1, 1000}; /* chemical transport: 0.001 m/s */
static const char MEDIUM_IDS[] FRC_PROGMEM =
    "vacuum\0bounded\0diamond\0water\0air\0nerve\0chemical\0";
static const char MEDIUM_NAMES[] FRC_PROGMEM =
    "Light in vacuum\0Light in bounded medium\0Diamond sound\0Water sound\0"
    "Air sound\0Nerve conduction\0Chemical transport\0";
static const char MEDIUM_PHASES[] FRC_PROGMEM =
    "Vacuum\0Vacuum + solid\0Solid\0Liquid\0Gas\0Solid + liquid\0Liquid + gas + solid\0";

frc_speed_t frc_medium_speed(frc_medium_t medium)
{
    uint64_t num = 0; /* 0/0 for something that is not a medium: frc_length refuses it */
    uint64_t den = 0;
    if (FRC_I_BELOW(medium, FRC_MEDIUM_COUNT)) {
        num = FRC_PGM_U32(&MEDIUM_SPEED_NUM[medium]);
        den = FRC_PGM_U16(&MEDIUM_SPEED_DEN[medium]);
    }
    return (frc_speed_t){num, den}; /* built in the return, as in frc_interval_mean */
}

/* Item `medium` of one of the three packed lists above. */
static int format_medium(const char *list, frc_medium_t medium, char *buf, size_t size)
{
    frc_i_text_t w;
    if (!FRC_I_BELOW(medium, FRC_MEDIUM_COUNT)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_nth(&w, list, (uint8_t)medium);
    return frc_i_text_finish(&w);
}

int frc_format_medium_id(frc_medium_t medium, char *buf, size_t size)
{
    return format_medium(MEDIUM_IDS, medium, buf, size);
}

int frc_format_medium_name(frc_medium_t medium, char *buf, size_t size)
{
    return format_medium(MEDIUM_NAMES, medium, buf, size);
}

int frc_format_medium_phase(frc_medium_t medium, char *buf, size_t size)
{
    return format_medium(MEDIUM_PHASES, medium, buf, size);
}

frc_status_t frc_length(uint8_t level, frc_interval_t interval, frc_speed_t speed, frc_sci_t *metres)
{
    ratio_t x;
    frc_status_t status;
    if (metres == NULL) {
        return FRC_ERR_ARG;
    }
    status = ratio_length(level, &interval, &speed, &x);
    return status == FRC_OK ? ratio_to_sci(&x, metres) : status;
}

/* ------------------------------------------------------------------------- */
/* S16.2  Bands                                                               */
/* ------------------------------------------------------------------------- */

/* Where each band starts, in hertz, as mantissa * 10^exponent (3 * 10^19 and
 * 3.4 * 10^29 do not fit 64 bits). Entry i opens frc_band_t i + 1; the last
 * entry is the top of the gamma rays, from which there is no band again. */
#define BAND_EDGE_COUNT 9
static const uint8_t BAND_EDGE_MANTISSA[BAND_EDGE_COUNT] FRC_PROGMEM = {
    2, 2, 3, 3, 4, 79, 3, 3, 34};
static const uint8_t BAND_EDGE_EXPONENT[BAND_EDGE_COUNT] FRC_PROGMEM = {
    1, 4, 8, 11, 14, 13, 16, 19, 28};
/* Names of frc_band_t 0..8. FRC_BAND_NONE reads "no band" (design ruling 4;
 * the reference writes a dash). */
static const char BAND_NAMES[] FRC_PROGMEM =
    "no band\0audible\0radio\0microwave\0infrared\0visible\0ultraviolet\0X-ray\0gamma ray\0";

frc_band_t frc_band(uint8_t level, frc_interval_t interval)
{
    frc_i_big_t top, bottom, edge;
    uint8_t i = BAND_EDGE_COUNT;

    if (period_parts(level, &interval, &top, &bottom) != FRC_OK) {
        return FRC_BAND_NONE;
    }
    /* From the highest edge down, the first one the frequency reaches names
     * the band. f = bottom / top, so
     *     f >= B   <=>   bottom >= top * B
     * in integers: a frequency ON an edge is in the band that starts there.
     * top * B < 2^66 * 34 * 10^28 < 2^165. */
    while (i > 0) {
        i--;
        frc_i_big_mul_u64(&edge, &top, FRC_PGM_U8(&BAND_EDGE_MANTISSA[i]));
        mul_power(&edge, 10, POW10_CHUNK, FRC_PGM_U8(&BAND_EDGE_EXPONENT[i]));
        if (frc_i_big_cmp(&bottom, &edge) >= 0) {
            return (i == BAND_EDGE_COUNT - 1) ? FRC_BAND_NONE : (frc_band_t)(i + 1);
        }
    }
    return FRC_BAND_NONE; /* below 20 Hz */
}

int frc_format_band(frc_band_t band, char *buf, size_t size)
{
    frc_i_text_t w;
    if (!FRC_I_BELOW(band, FRC_BAND_GAMMA + 1)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    frc_i_text_init(&w, buf, size);
    frc_i_text_nth(&w, BAND_NAMES, (uint8_t)band);
    return frc_i_text_finish(&w);
}

/* The edge `i` of the table above as a frc_sci_t: m * 10^e with m of one or
 * two digits is m * 10^(17 - digits) * 10^(e - 17 + digits), 17 digits exact. */
static void band_edge(uint8_t i, frc_sci_t *out)
{
    uint64_t m = FRC_PGM_U8(&BAND_EDGE_MANTISSA[i]);
    uint8_t digits = (m >= 10u) ? 2 : 1;

    out->mantissa = m * frc_i_pow_u64(10, (uint8_t)(FRC_SCI_DIGITS - digits));
    out->exp10 = (int16_t)(FRC_PGM_U8(&BAND_EDGE_EXPONENT[i]) - (FRC_SCI_DIGITS - digits));
}

frc_status_t frc_band_edges(frc_band_t band, frc_sci_t *low_hz, frc_sci_t *high_hz)
{
    /* band b (1..8) runs from edge b - 1 up to edge b; FRC_BAND_NONE is the
     * two open ends, below edge 0 and from edge 8 */
    if (low_hz == NULL || high_hz == NULL || band == FRC_BAND_NONE || !FRC_I_BELOW(band, FRC_BAND_GAMMA + 1)) {
        return FRC_ERR_ARG;
    }
    band_edge((uint8_t)(band - 1), low_hz);
    band_edge((uint8_t)band, high_hz);
    return FRC_OK;
}

/* ------------------------------------------------------------------------- */
/* S16.4  Devices                                                             */
/* ------------------------------------------------------------------------- */

/*
 * round(n / d) with a half going up, as a 64-bit count for a device.
 * @return FRC_ERR_REFUSED  the count rounds to nothing (n / d < 1/2)
 *         FRC_ERR_OVERFLOW it is 2^64 or more
 */
static frc_status_t round_count(const frc_i_big_t *n, const frc_i_big_t *d, uint64_t *count)
{
    frc_i_big_t quotient, rem;
    uint64_t whole;
    int up;

    frc_i_big_divmod(n, d, &quotient, &rem);
    if (frc_i_big_bits(&quotient) > 64) {
        return FRC_ERR_OVERFLOW;
    }
    whole = frc_i_big_low_u64(&quotient);
    up = frc_i_big_cmp_half(&rem, d) >= 0;
    if (up && whole == UINT64_MAX) {
        return FRC_ERR_OVERFLOW; /* 2^64 - 1/2 and above round to 2^64 */
    }
    if (up) {
        whole++;
    }
    if (whole == 0) {
        return FRC_ERR_REFUSED;
    }
    *count = whole;
    return FRC_OK;
}

frc_status_t frc_device_ticks(uint8_t level, frc_interval_t interval, uint64_t rate, uint64_t *ticks)
{
    frc_i_big_t top, bottom;
    frc_status_t status;

    if (ticks == NULL) {
        return FRC_ERR_ARG;
    }
    *ticks = 0;
    if (rate == 0) {
        return FRC_ERR_ARG;
    }
    status = period_parts(level, &interval, &top, &bottom);
    if (status != FRC_OK) {
        return status;
    }
    /* ticks = round(T * rate) = round(top * rate / bottom). A period under
     * half a tick is exactly a count that rounds to 0: round_count refuses it. */
    frc_i_big_mul_u64(&top, &top, rate);
    return round_count(&top, &bottom, ticks);
}

frc_status_t frc_device_step(uint8_t level, frc_interval_t interval, uint64_t rate, uint8_t bits,
                             uint64_t *step)
{
    frc_i_big_t top, bottom, twice;
    frc_status_t status;

    if (step == NULL) {
        return FRC_ERR_ARG;
    }
    *step = 0;
    if (rate == 0 || bits == 0 || bits > 64) {
        return FRC_ERR_ARG;
    }
    status = period_parts(level, &interval, &top, &bottom);
    if (status != FRC_OK) {
        return status;
    }
    /* f = bottom / top. Above half the sample rate it cannot be made:
     *     f > rate / 2   <=>   2 bottom > top * rate
     * Checked first and exactly, so a frequency ON rate / 2 is still made. */
    frc_i_big_mul_u64(&top, &top, rate);
    frc_i_big_mul_u64(&twice, &bottom, 2);
    if (frc_i_big_cmp(&twice, &top) > 0) {
        return FRC_ERR_REFUSED;
    }
    /* step = round(f * 2^bits / rate) = round(bottom * 2^bits / (top * rate)).
     * It is at most 2^(bits-1) because f <= rate / 2, so it always fits. */
    frc_i_big_shl(&bottom, &bottom, bits);
    return round_count(&bottom, &top, step);
}

/* ------------------------------------------------------------------------- */
/* S16.1  Text                                                                */
/* ------------------------------------------------------------------------- */

/*
 * THE FORMATTING RULE (tools/oracle/LADDER.md has it in full, with the cases
 * SPEC never prints). A value is written with `digits` significant digits in
 * the largest unit that is not larger than it:
 *
 *   time       ys zs as fs ps ns µs ms s     then min, h, d, y (Julian year)
 *   frequency  pHz nHz µHz mHz Hz kHz ... YHz
 *   length     ym zm am fm pm nm µm mm m km Mm Gm Tm Pm
 *
 * Below the first unit, and from 1000 of the last one up, the value is
 * written as a power of ten: "2.454 × 10⁻²⁵ s" (time above 1000 y: in years).
 *
 * HOW IT IS DONE. The value is normalised (1 <= n/d < 10 with a decimal
 * exponent e) and rounded ONCE to r, an integer of `digits` digits. From
 * there the text is only a matter of where the point goes:
 *   - SI units step by 1000, so the unit is the multiple of three at or
 *     below e, and e minus that (0, 1 or 2) is how many digits precede the
 *     point. A rounding that carries (999.96 ms) raises e first, so it lands
 *     in the next unit by itself: "1.000 s".
 *   - min, h, d and y are not powers of ten: the value is divided by the
 *     unit's seconds exactly (d is multiplied) BEFORE it is rounded. A
 *     rounded value that reaches the next unit (59.9996 min, or 350 d at one
 *     digit) is written as one of that unit: "1.000 h", "1 y".
 */

/* The SI prefixes from 10^-24 in steps of 10^3, as Latin-1 code points
 * (0xB5 is the micro sign; 0 is "no prefix"). */
#define SI_FIRST_EXPONENT (-24)
static const uint8_t SI_PREFIXES[17] FRC_PROGMEM = {
    'y', 'z', 'a', 'f', 'p', 'n', 0xB5, 'm', 0, 'k', 'M', 'G', 'T', 'P', 'E', 'Z', 'Y'};
/* Which of them each frc_quantity_t uses: first and last index. */
#define QUANTITY_COUNT 3
static const uint8_t SI_FIRST[QUANTITY_COUNT] FRC_PROGMEM = {0, 4, 0};  /* ys, pHz, ym */
static const uint8_t SI_LAST[QUANTITY_COUNT] FRC_PROGMEM = {8, 16, 13}; /* s, YHz, Pm */
static const char BASE_UNITS[] FRC_PROGMEM = "s\0Hz\0m\0";

/* The units of time above the second, their length in seconds, and where
 * each ends, counted in itself and times four (a day ends at 365.25 d). */
#define CIVIL_UNIT_COUNT 4
#define CIVIL_YEAR (CIVIL_UNIT_COUNT - 1)
static const char CIVIL_UNITS[] FRC_PROGMEM = "min\0h\0d\0y\0";
static const uint32_t CIVIL_SECONDS[CIVIL_UNIT_COUNT] FRC_PROGMEM = {
    60u, 3600u, 86400u, 31557600u};
static const uint16_t CIVIL_END_X4[CIVIL_UNIT_COUNT] FRC_PROGMEM = {
    4 * 60, 4 * 24, 1461, 4 * 1000};
/* The second ends at 60 s, in the same form. */
#define SECOND_END_X4 (4 * 60)

/* Decimal exponent of SI prefix `index`. */
static int32_t si_exponent(uint8_t index)
{
    return SI_FIRST_EXPONENT + 3 * (int32_t)index;
}

/*
 * The digits themselves are written by two helpers of frc_text.c, which the
 * pure wordings (frc_pure.c) share: frc_i_text_mantissa puts r, an integer of
 * `digits` digits, with the point after `lead` + 1 of them ("310.9"), and
 * frc_i_text_power_of_ten writes "d.ddd × 10ⁿ " (the unit follows).
 */

/* "1", "1.0", "1.000": one of a unit, to `digits` significant digits. */
static void put_one(frc_i_text_t *w, uint8_t digits)
{
    frc_i_text_mantissa(w, pow10_u64((uint8_t)(digits - 1)), digits, 0);
}

/*
 * 1 when the rounded value r * 10^(e - digits + 1) is at least end_x4 / 4.
 * REQUIRES -9 < e - digits + 1 < 9 (e is 0..2 where this is used), so both
 * sides stay far inside 64 bits.
 */
static int rounded_reaches(uint64_t r, uint8_t digits, int32_t e, uint16_t end_x4)
{
    int32_t shift = e - ((int32_t)digits - 1);
    uint64_t value_x4 = 4u * r;
    uint64_t end = end_x4;
    if (shift >= 0) {
        value_x4 *= pow10_u64((uint8_t)shift);
    } else {
        end *= pow10_u64((uint8_t)-shift);
    }
    return value_x4 >= end;
}

/* A normalised time of 60 s or more, in min, h, d or y. Uses x up. */
static void put_civil_time(frc_i_text_t *w, ratio_t *x, uint8_t digits)
{
    uint8_t unit = 0;
    uint64_t r;

    /* the largest unit that is not larger than the value, before any rounding */
    while (unit < CIVIL_YEAR && ratio_reaches(x, FRC_PGM_U32(&CIVIL_SECONDS[unit + 1]))) {
        unit++;
    }
    frc_i_big_mul_u64(&x->d, &x->d, FRC_PGM_U32(&CIVIL_SECONDS[unit]));
    ratio_normalise(x);
    r = ratio_round(x, digits);

    if (x->e >= 3) {
        /* 1000 or more of the unit. Only years get here (a day ends at
         * 365.25 d), also when 999.96 y rounds up: "1.000 × 10³ y". */
        frc_i_text_power_of_ten(w, r, digits, x->e);
    } else if (rounded_reaches(r, digits, x->e, FRC_PGM_U16(&CIVIL_END_X4[unit]))) {
        put_one(w, digits);
        frc_i_text_char(w, ' ');
        unit++; /* never past the year: 1000 y was taken by the branch above */
    } else {
        frc_i_text_mantissa(w, r, digits, x->e);
        frc_i_text_char(w, ' ');
    }
    frc_i_text_nth(w, CIVIL_UNITS, unit);
}

/* A normalised value on the SI prefixes of its quantity (a time: under 60 s). Uses x up. */
static void put_si(frc_i_text_t *w, ratio_t *x, frc_quantity_t quantity, uint8_t digits)
{
    uint8_t first = FRC_PGM_U8(&SI_FIRST[quantity]);
    uint8_t last = FRC_PGM_U8(&SI_LAST[quantity]);
    uint64_t r = ratio_round(x, digits);
    int32_t e = x->e; /* after the rounding: a carry has already moved it */

    if (quantity == FRC_QUANTITY_TIME && e >= 0 && rounded_reaches(r, digits, e, SECOND_END_X4)) {
        /* 59.9996 s, or 55 s at one digit: a minute */
        put_one(w, digits);
        frc_i_text_char(w, ' ');
        frc_i_text_nth(w, CIVIL_UNITS, 0);
        return;
    }
    if (e < si_exponent(first) || e >= si_exponent(last) + 3) {
        frc_i_text_power_of_ten(w, r, digits, e);
    } else {
        /* e - SI_FIRST_EXPONENT >= 0 here. Seconds run on to 60 s, which is
         * e == 1: it still divides down to the index of "s". */
        uint8_t index = (uint8_t)((e - SI_FIRST_EXPONENT) / 3);
        uint8_t prefix = FRC_PGM_U8(&SI_PREFIXES[index]);
        frc_i_text_mantissa(w, r, digits, e - si_exponent(index));
        frc_i_text_char(w, ' ');
        if (prefix != 0) {
            frc_i_text_cp(w, prefix);
        }
    }
    frc_i_text_nth(w, BASE_UNITS, (uint8_t)quantity);
}

/*
 * Write the exact value x (not yet normalised) of a quantity; `status` is how
 * building x went. The one place every frc_format_* below ends in.
 */
static int format_ratio(frc_status_t status, ratio_t *x, frc_quantity_t quantity, uint8_t digits,
                        char *buf, size_t size)
{
    frc_i_text_t w;

    if (digits == 0) {
        digits = 4; /* the precision of the table of S16.1 */
    }
    if (status == FRC_OK && (digits > 9 || !FRC_I_BELOW(quantity, QUANTITY_COUNT))) {
        status = FRC_ERR_ARG;
    }
    if (status != FRC_OK) {
        return frc_i_text_error(buf, size, status);
    }
    frc_i_text_init(&w, buf, size);
    ratio_normalise(x);
    if (quantity == FRC_QUANTITY_TIME && ratio_reaches(x, FRC_PGM_U32(&CIVIL_SECONDS[0]))) {
        put_civil_time(&w, x, digits);
    } else {
        put_si(&w, x, quantity, digits);
    }
    return frc_i_text_finish(&w);
}

int frc_format_quantity(const frc_sci_t *value, frc_quantity_t quantity, uint8_t digits,
                        char *buf, size_t size)
{
    ratio_t x;
    if (!sci_ok(value)) {
        return frc_i_text_error(buf, size, FRC_ERR_ARG);
    }
    /* mantissa * 10^exp10 is already "n / d * 10^e": any exponent is fine,
     * because the power of ten never enters the big integers */
    frc_i_big_set_u64(&x.n, value->mantissa);
    frc_i_big_set_u64(&x.d, 1);
    x.e = value->exp10;
    return format_ratio(FRC_OK, &x, quantity, digits, buf, size);
}

/* The three below format the EXACT quotient, not its 17-digit frc_sci_t: the
 * last digit shown is rounded once (S16.4). */

int frc_format_period(uint8_t level, frc_interval_t interval, uint8_t digits, char *buf, size_t size)
{
    ratio_t x;
    frc_status_t status = ratio_period(level, &interval, &x);
    return format_ratio(status, &x, FRC_QUANTITY_TIME, digits, buf, size);
}

int frc_format_frequency(uint8_t level, frc_interval_t interval, uint8_t digits, char *buf, size_t size)
{
    ratio_t x;
    frc_status_t status = ratio_frequency(level, &interval, &x);
    return format_ratio(status, &x, FRC_QUANTITY_FREQUENCY, digits, buf, size);
}

int frc_format_length(uint8_t level, frc_interval_t interval, frc_speed_t speed, uint8_t digits,
                      char *buf, size_t size)
{
    ratio_t x;
    frc_status_t status = ratio_length(level, &interval, &speed, &x);
    return format_ratio(status, &x, FRC_QUANTITY_LENGTH, digits, buf, size);
}

/* ------------------------------------------------------------------------- */
/* S16.4  Doubles                                                             */
/* ------------------------------------------------------------------------- */

#ifndef FRC_NO_FLOAT

/* 2^30, the step the final scaling takes: exactly representable. */
#define TWO_30 1073741824.0

/*
 * The double nearest to n / d * 2^exp2, halves to even. REQUIRES n > 0,
 * d > 0 and a result that is a NORMAL double (every value of the ladder is:
 * they run from 10^-93 to 10^74 even with the widest interval and speed).
 *
 * The quotient is taken with 55 or 56 significant bits: shifting the smaller
 * side so that the bit lengths differ by 55 puts n / d in (2^54, 2^56). Its
 * top 53 bits are the double's significand; the bits below them, and whether
 * the division left a remainder ("sticky"), say which way to round. Then the
 * significand is converted (exact: it is below 2^53) and scaled by a power of
 * two (exact while the value stays normal), so no second rounding happens and
 * nothing depends on the byte layout of a double.
 */
static double rational_to_double(const frc_i_big_t *n, const frc_i_big_t *d, int32_t exp2)
{
    frc_i_big_t shifted, quotient, rem;
    int32_t shift = 55 - ((int32_t)frc_i_big_bits(n) - (int32_t)frc_i_big_bits(d));
    uint64_t m;
    uint32_t tail;
    int sticky;
    double value;

    if (shift >= 0) {
        frc_i_big_shl(&shifted, n, (uint16_t)shift);
        frc_i_big_divmod(&shifted, d, &quotient, &rem);
    } else {
        frc_i_big_shl(&shifted, d, (uint16_t)-shift);
        frc_i_big_divmod(n, &shifted, &quotient, &rem);
    }
    m = frc_i_big_low_u64(&quotient); /* 2^54 <= m < 2^56 */
    sticky = frc_i_big_bits(&rem) != 0;
    exp2 -= shift;                    /* value = (m + fraction) * 2^exp2 */

    if ((m >> 55) != 0) {             /* 56 bits: fold the lowest into sticky */
        sticky |= (int)(m & 1u);
        m >>= 1;
        exp2++;
    }
    /* m has 55 bits: 53 to keep and two to round on */
    tail = (uint32_t)(m & 3u);
    m >>= 2;
    exp2 += 2;
    if (tail == 3u || (tail == 2u && (sticky || (m & 1u) != 0))) {
        m++; /* 2^53 at most, which is still exact as a double */
    }

    value = (double)m;
    for (; exp2 >= 30; exp2 -= 30) {
        value *= TWO_30;
    }
    for (; exp2 <= -30; exp2 += 30) {
        value /= TWO_30;
    }
    if (exp2 > 0) {
        value *= (double)((uint32_t)1 << exp2);
    } else if (exp2 < 0) {
        value /= (double)((uint32_t)1 << -exp2);
    }
    return value;
}

/* 10^e = 5^e * 2^e, and the power of two costs nothing in a double. With
 * 5^140 < 2^326 a 17-digit mantissa times it, or it shifted for the 56-bit
 * quotient, stays inside 384 bits. */
#define SCI_DOUBLE_MAX_EXP10 140

double frc_sci_to_double(const frc_sci_t *value)
{
    frc_i_big_t n, d;
    int32_t e;

    if (!sci_ok(value)) {
        return 0.0;
    }
    e = value->exp10;
    if (e < -SCI_DOUBLE_MAX_EXP10 || e > SCI_DOUBLE_MAX_EXP10) {
        /* Outside what this core can round exactly (and far outside the
         * ladder, whose exponents run from -109 to 57). */
        return 0.0;
    }
    frc_i_big_set_u64(&n, value->mantissa);
    frc_i_big_set_u64(&d, 1);
    if (e >= 0) {
        mul_power(&n, 5, POW5_CHUNK, (uint16_t)e);
    } else {
        mul_power(&d, 5, POW5_CHUNK, (uint16_t)-e);
    }
    return rational_to_double(&n, &d, e);
}

double frc_period_seconds(uint8_t level, frc_interval_t interval)
{
    ratio_t x;
    if (ratio_period(level, &interval, &x) != FRC_OK) {
        return 0.0;
    }
    return rational_to_double(&x.n, &x.d, 0);
}

double frc_frequency_hz(uint8_t level, frc_interval_t interval)
{
    ratio_t x;
    if (ratio_frequency(level, &interval, &x) != FRC_OK) {
        return 0.0;
    }
    return rational_to_double(&x.n, &x.d, 0);
}

double frc_length_metres(uint8_t level, frc_interval_t interval, frc_speed_t speed)
{
    ratio_t x;
    if (ratio_length(level, &interval, &speed, &x) != FRC_OK) {
        return 0.0;
    }
    return rational_to_double(&x.n, &x.d, 0);
}

#endif /* FRC_NO_FLOAT */
