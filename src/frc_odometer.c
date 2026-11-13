/*
 * frc_odometer.c - the odometer read-out: k glyphs that count one cycle.
 *
 * Implements SPEC S5. Also home of the two helpers of frc_internal.h
 * (section 8) that the solar-sync odometer (S7.3) reuses.
 *
 * THE MODEL
 *   With q = 4M states per glyph, k glyphs count q^k steps per cycle:
 *
 *       step    = floor(position * P * M * q^(k-1)) mod q^k
 *       digit_j = floor(step / q^(k-1-j)) mod q        j = 0 (left) .. k-1
 *
 *   A cycle has four quarters, so that is U = q^k / 4 = M * q^(k-1) steps per
 *   QUARTER: exactly the grid of frc_i_fraction_step (frc_internal.h section
 *   5), step = q4 * U + floor(rem * U / den). The whole position is never
 *   multiplied by P * U. The finest digit changes when the step does, at the
 *   instant frc_i_fraction_step_end gives (ceiling to the microsecond).
 *
 * INVARIANTS
 *   - digit[0] equals frc_glyph_at(...).index for the same strokes.
 *   - next_change is strictly after the query instant. It is FRC_TIME_NONE
 *     only AT the catalogue's closing instant: every eclipse is a boundary of
 *     every grid (position * P * U is an integer there), so the last state
 *     ends exactly with the catalogue, never beyond it.
 *   - Unused digits are 0. Arguments are checked before the instant, so a bad
 *     stroke or digit count is FRC_ERR_ARG even outside the catalogue. On
 *     any error `out` is left untouched.
 *   - q^k <= 484^4 < 2^36 with the present limits; the power is still
 *     computed with an overflow check so that raising FRC_MAX_STROKES or
 *     FRC_MAX_DIGITS cannot silently wrap.
 */
#include "frc_internal.h"

frc_status_t frc_i_odometer_steps(uint8_t strokes, uint8_t digits, uint64_t *steps)
{
    uint16_t states = frc_glyph_states(strokes); /* 0 for a bad stroke count */
    uint64_t turn;

    if (steps == NULL || states == 0 || digits == 0 || digits > FRC_MAX_DIGITS) {
        return FRC_ERR_ARG;
    }
    turn = frc_i_pow_u64(states, digits);
    if (turn == 0) {
        return FRC_ERR_OVERFLOW;
    }
    *steps = turn;
    return FRC_OK;
}

/* The lowest base-`states` digit of *step, which is then divided by the base.
 * 0, and *step untouched, for a place the odometer does not have. */
static uint16_t peel_digit(uint64_t *step, uint16_t states, int in_use)
{
    uint16_t digit = 0;
    if (in_use && states != 0) {
        digit = (uint16_t)(*step % states);
        *step /= states;
    }
    return digit;
}

void frc_i_odometer_fill(frc_odometer_t *out, uint8_t strokes, uint8_t digits, uint64_t step)
{
    uint16_t states = frc_glyph_states(strokes);

    out->count = digits;
    out->strokes = strokes;
    out->states = states;
    out->next_change = FRC_TIME_NONE;
    /* Peel the digits from the fine end; places beyond `digits` read 0.
     * FRC_MAX_DIGITS stores written out, not a loop: gcc -O3 splits the loop
     * in two, and the half that writes the zeros becomes a call to memset
     * (seen on ARMv6-M). The #error keeps the list honest. */
#if FRC_MAX_DIGITS != 4
#  error "frc_i_odometer_fill writes exactly four digits: add a store per digit"
#endif
    out->digit[3] = peel_digit(&step, states, digits > 3);
    out->digit[2] = peel_digit(&step, states, digits > 2);
    out->digit[1] = peel_digit(&step, states, digits > 1);
    out->digit[0] = peel_digit(&step, states, digits > 0);
}

frc_status_t frc_odometer_at(const frc_catalogue_t *cat, frc_time_t t, uint8_t level,
                             uint8_t strokes, uint8_t digits, frc_odometer_t *out)
{
    frc_i_fraction_t f;
    frc_status_t status;
    uint64_t steps, per_quarter;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = frc_i_odometer_steps(strokes, digits, &steps);
    if (status != FRC_OK) {
        return status;
    }
    status = frc_i_fraction_at(cat, t, level, &f);
    if (status != FRC_OK) {
        return status;
    }
    per_quarter = steps / 4; /* U = M * q^(k-1), exact: q = 4M */
    frc_i_odometer_fill(out, strokes, digits, frc_i_fraction_step(&f, per_quarter));
    out->next_change = frc_i_fraction_step_end(cat, level, &f, per_quarter);
    return FRC_OK;
}

frc_status_t frc_odometer_of_phase(frc_frac_t phase, uint8_t strokes, uint8_t digits, frc_odometer_t *out)
{
    frc_status_t status;
    frc_i_u128_t scaled;
    uint64_t steps;

    if (out == NULL) {
        return FRC_ERR_ARG;
    }
    status = frc_i_odometer_steps(strokes, digits, &steps);
    if (status != FRC_OK) {
        return status;
    }
    /* floor(phase / 2^32 * q^k): the product needs up to 68 bits, and its
     * top 64 bits after the shift are below q^k. */
    frc_i_mul64(phase, steps, &scaled);
    frc_i_odometer_fill(out, strokes, digits, (scaled.hi << 32) | (scaled.lo >> 32));
    return FRC_OK;
}
