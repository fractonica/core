/*
 * check-clock-vectors.c - a self-check of the generated oracle headers, independent of the core.
 *
 * tests/vectors/oracle_clock.h and oracle_notation.h are written with positional initialisers, so a
 * field out of order in the generator would still compile. This program reads every table through the
 * typedefs and checks the invariants that tie the fields to one another (a cycle holds its instant, the
 * quarter is the top two bits of the phase, trits spell the value, parts add up to the span ...), and the
 * numbers of SPEC S12 for vector A. It proves the headers are valid C and that the layouts documented in
 * tools/oracle/README.md are the ones the data was written in. It does NOT test the core.
 *
 * Build and run from the core repository (host only; uses libc):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -Itests/vectors \
 *      tools/oracle/check-clock-vectors.c -o /tmp/check-clock-vectors && /tmp/check-clock-vectors
 */
#include <stdio.h>
#include <string.h>

#include "oracle_clock.h"
#include "oracle_notation.h"

static int failures = 0;
static long checks = 0;

#define CHECK(cond, ...)                           \
    do {                                           \
        checks++;                                  \
        if (!(cond)) {                             \
            failures++;                            \
            if (failures <= 40) {                  \
                printf("FAIL %s: ", #cond);        \
                printf(__VA_ARGS__);               \
                printf("\n");                      \
            }                                      \
        }                                          \
    } while (0)

static const uint8_t SEQUENCE[8] = {5, 13, 5, 5, 7, 11, 9, 4};
/* frc_unit_t -> Blinks in the unit */
static const uint64_t UNIT_BLINKS[9] = {7319812500ULL, 1463962500ULL, 112612500ULL, 22522500ULL, 4504500ULL, 58500ULL, 1625ULL, 25ULL, 1ULL};

static uint64_t prefix(int level) {
    uint64_t p = 1;
    for (int l = 1; l <= level; l++) p *= SEQUENCE[(l - 1) % 8];
    return p;
}

/* SPEC S4.1: value, derivative and dot of state `index` of a glyph with extreme m. */
static void state_of(int index, int m, int *value, int *derivative) {
    if (index <= m) *value = index;
    else if (index <= 3 * m) *value = 2 * m - index;
    else *value = index - 4 * m;
    *derivative = (index == m || index == 3 * m) ? 0 : (index < m || index > 3 * m) ? 1 : -1;
}

static void check_glyph(const char *label, int level, const oracle_clock_glyph_t *g, int index, int strokes) {
    int m = strokes == 2 ? 4 : 13, value, derivative, spelled = 0, weight = 1;
    state_of(index, m, &value, &derivative);
    for (int i = 0; i < 3; i++, weight *= 3) spelled += g->trit[i] * weight;
    CHECK(g->value == value, "%s level %d strokes %d", label, level, strokes);
    CHECK(g->derivative == derivative, "%s level %d strokes %d", label, level, strokes);
    CHECK(g->dotted == (derivative < 0), "%s level %d strokes %d", label, level, strokes);
    CHECK(spelled == value, "%s level %d strokes %d trits", label, level, strokes);
    if (strokes == 2) CHECK(g->trit[2] == 0, "%s level %d unused trit", label, level);
}

static void check_clock_sample(const oracle_clock_sample_t *s) {
    int64_t first = ORACLE_CLOCK_CATALOGUE_MS[0] * 1000, last = ORACLE_CLOCK_CATALOGUE_MS[ORACLE_CLOCK_CATALOGUE_MS_COUNT - 1] * 1000;
    CHECK(s->t_us % 1000 == 0, "%s whole millisecond", s->label);
    CHECK(s->in_range == (s->t_us >= first && s->t_us <= last), "%s in_range", s->label);
    CHECK(s->pentad.has == s->in_range, "%s pentad.has", s->label);
    if (!s->in_range) {
        CHECK(s->den == 0 && s->sync.has == 0 && s->level[6].cycle == 0, "%s zero when outside", s->label);
        return;
    }
    int64_t e0 = ORACLE_CLOCK_CATALOGUE_MS[s->ordinal] * 1000, e1 = ORACLE_CLOCK_CATALOGUE_MS[s->ordinal + 1] * 1000;
    CHECK(e0 <= s->t_us && s->t_us < e1, "%s ordinal", s->label);
    CHECK(s->num == (uint64_t)(s->t_us - e0) && s->den == (uint64_t)(e1 - e0), "%s num/den", s->label);
    CHECK(s->t_us <= last - 1000, "%s not within 1 ms of the closing instant", s->label);

    for (int level = 0; level < ORACLE_CLOCK_LEVELS; level++) {
        const oracle_clock_level_t *l = &s->level[level];
        CHECK(l->start_us <= s->t_us && s->t_us < l->end_us, "%s level %d holds t", s->label, level);
        CHECK(l->quarter == l->phase >> 30, "%s level %d quarter", s->label, level);
        CHECK(l->next_name == l->quarter, "%s level %d next name", s->label, level);
        CHECK(l->next_us == ORACLE_CLOCK_NONE || l->next_us > s->t_us, "%s level %d next time", s->label, level);
        CHECK(l->next_us == ORACLE_CLOCK_NONE || l->next_us <= l->end_us, "%s level %d next inside the cycle", s->label, level);
        CHECK(l->odometer_next_us == ORACLE_CLOCK_NONE || l->odometer_next_us > s->t_us, "%s level %d odometer next", s->label, level);
        CHECK(l->index[0] == l->phase >> 28, "%s level %d 2-stroke index", s->label, level);
        CHECK(l->odometer[0] == l->index[0] && l->odometer[1] == ((l->phase >> 24) & 15), "%s level %d odometer", s->label, level);
        CHECK(l->index[1] < 52 && l->index[2] < 160, "%s level %d index range", s->label, level);
        CHECK(l->index[1] == (uint16_t)(((uint64_t)l->phase * 52) >> 32) || l->index[1] == (uint16_t)((((uint64_t)l->phase * 52) >> 32) + 1),
              "%s level %d 3-stroke index", s->label, level);
        check_glyph(s->label, level, &l->glyph2, l->index[0], 2);
        check_glyph(s->label, level, &l->glyph3, l->index[1], 3);
        if (level == 0) {
            CHECK(l->local == l->cycle && l->siblings == 18, "%s level 0 counter", s->label);
        } else {
            CHECK(l->siblings == SEQUENCE[(level - 1) % 8], "%s level %d siblings", s->label, level);
            CHECK(l->local == l->cycle % l->siblings, "%s level %d local", s->label, level);
            CHECK(l->cycle / l->siblings == s->level[level - 1].cycle, "%s level %d parent", s->label, level);
            CHECK(l->start_us >= s->level[level - 1].start_us && l->end_us <= s->level[level - 1].end_us, "%s level %d nests", s->label, level);
        }
        if (l->clipped) CHECK(l->end_us == last, "%s level %d clipped end", s->label, level);
    }
    CHECK(s->odo6_s3d2[0] == s->level[6].index[1], "%s 3-stroke odometer", s->label);
    CHECK(s->odo6_s2d3[0] == s->level[6].odometer[0] && s->odo6_s2d3[1] == s->level[6].odometer[1], "%s 3-digit odometer", s->label);
    CHECK(s->odo6_s2d3[2] == ((s->level[6].phase >> 20) & 15), "%s 3-digit odometer fine digit", s->label);
    CHECK(s->odo6_s2d3_next_us == ORACLE_CLOCK_NONE || s->odo6_s2d3_next_us <= s->level[6].odometer_next_us, "%s finer odometer changes sooner", s->label);

    const oracle_clock_sync_t *y = &s->sync;
    if (y->has) {
        CHECK(y->start_us <= s->t_us && s->t_us < y->end_us, "%s sync holds t", s->label);
        CHECK(y->nominal_us == ORACLE_CLOCK_CATALOGUE_MS[ORACLE_CLOCK_ANCHOR] * 1000 + (int64_t)y->number * 4 * 86400000000LL, "%s nominal", s->label);
        CHECK(y->drift_us == y->start_us - y->nominal_us, "%s drift", s->label);
        CHECK(y->quarter == y->phase >> 30 && y->next_name == y->quarter, "%s sync quarter", s->label);
        CHECK(y->next_us > s->t_us && y->next_us <= y->end_us, "%s sync next", s->label);
        CHECK(y->digits_s2d3[0] == y->phase >> 28 && y->digits_s2d3[1] == ((y->phase >> 24) & 15) && y->digits_s2d3[2] == ((y->phase >> 20) & 15),
              "%s sync digits", s->label);
        uint64_t step = ((uint64_t)y->phase * 2704) >> 32; /* 52^2; the 32-bit phase can sit one step short */
        uint64_t got = (uint64_t)y->digits_s3d2[0] * 52 + y->digits_s3d2[1];
        CHECK(got == step || got == step + 1, "%s sync 3-stroke digits", s->label);
        CHECK(s->pentad.sync == y->number, "%s pentad sync", s->label);
        CHECK(s->pentad.start_us >= y->start_us && s->pentad.end_us <= y->end_us, "%s pentad inside its sync", s->label);
    }
    const oracle_clock_pentad_t *p = &s->pentad;
    CHECK(p->start_us <= s->t_us && s->t_us < p->end_us, "%s pentad holds t", s->label);
    CHECK(p->index <= 3 && p->helions == (p->index == 3 ? 4 : 5) && p->helion >= 1 && p->helion <= p->helions, "%s pentad fields", s->label);
    CHECK(p->start_us <= s->level[6].start_us && s->level[6].end_us <= p->end_us, "%s pentad holds its Helion", s->label);
}

static void check_clock(void) {
    for (int k = 1; k < ORACLE_CLOCK_CATALOGUE_MS_COUNT; k++) CHECK(ORACLE_CLOCK_CATALOGUE_MS[k] > ORACLE_CLOCK_CATALOGUE_MS[k - 1], "catalogue order %d", k);
    for (int i = 0; i < ORACLE_CLOCK_SAMPLES_COUNT; i++) check_clock_sample(&ORACLE_CLOCK_SAMPLES[i]);

    for (int i = 0; i < ORACLE_CLOCK_DEEP_SAMPLES_COUNT; i++) {
        const oracle_clock_deep_sample_t *d = &ORACLE_CLOCK_DEEP_SAMPLES[i];
        const oracle_clock_sample_t *s = &ORACLE_CLOCK_SAMPLES[d->sample];
        CHECK(d->sample < ORACLE_CLOCK_SAMPLES_COUNT && s->t_us == d->t_us && s->in_range, "deep %d sample", i);
        for (int j = 0; j < ORACLE_CLOCK_DEEP_LEVELS; j++) {
            const oracle_clock_deep_level_t *l = &d->level[j];
            int level = ORACLE_CLOCK_DEEP_FIRST_LEVEL + j;
            uint64_t parent = j == 0 ? s->level[ORACLE_CLOCK_LEVELS - 1].cycle : d->level[j - 1].cycle;
            CHECK(l->start_us <= d->t_us && d->t_us < l->end_us, "deep %d level %d holds t", i, level);
            CHECK(l->siblings == SEQUENCE[(level - 1) % 8] && l->local == l->cycle % l->siblings, "deep %d level %d local", i, level);
            CHECK(l->cycle / l->siblings == parent, "deep %d level %d parent", i, level);
            CHECK(l->quarter == l->phase >> 30 && l->index2 == l->phase >> 28, "deep %d level %d phase", i, level);
        }
    }

    int64_t last = ORACLE_CLOCK_CATALOGUE_MS[ORACLE_CLOCK_CATALOGUE_MS_COUNT - 1] * 1000;
    int clipped = 0;
    for (int i = 0; i < ORACLE_CLOCK_CYCLES_COUNT; i++) {
        const oracle_clock_cycle_t *c = &ORACLE_CLOCK_CYCLES[i];
        CHECK(c->cycle < ORACLE_CLOCK_LEVEL_CYCLES[c->level], "cycle %d index", i);
        CHECK(c->start_us <= c->end_us && c->point_us[0] == c->start_us, "cycle %d start", i);
        CHECK(c->local == (c->level == 0 ? c->cycle : c->cycle % c->siblings), "cycle %d local", i);
        if (c->clipped) {
            clipped++;
            CHECK(c->end_us == last && c->point_us[4] == ORACLE_CLOCK_NONE && c->cycle == ORACLE_CLOCK_LEVEL_CYCLES[c->level] - 1, "cycle %d clipped", i);
        } else {
            CHECK(c->point_us[4] == c->end_us, "cycle %d end", i);
        }
        for (int k = 1; k < 5; k++)
            CHECK(c->point_us[k] == ORACLE_CLOCK_NONE || (c->point_us[k - 1] != ORACLE_CLOCK_NONE && c->point_us[k] >= c->point_us[k - 1] && c->point_us[k] <= last), "cycle %d point %d", i, k);
    }
    CHECK(clipped >= 8, "clipped cycles in the table: %d", clipped);
    for (int level = 0; level < ORACLE_CLOCK_LEVEL_CYCLES_COUNT; level++) {
        uint64_t x = 69 * prefix(level); /* ceil(69 * P / 4) */
        CHECK(ORACLE_CLOCK_LEVEL_CYCLES[level] == (x + 3) / 4, "cycle count of level %d", level);
    }

    for (int i = 0; i < ORACLE_CLOCK_SYNCS_BY_NUMBER_COUNT; i++) {
        const oracle_clock_sync_by_number_t *y = &ORACLE_CLOCK_SYNCS_BY_NUMBER[i];
        CHECK(y->ok == (y->number >= ORACLE_CLOCK_SYNC_FIRST_WHOLE && y->number <= ORACLE_CLOCK_SYNC_LAST_WHOLE), "sync %d ok", y->number);
        if (!y->ok) continue;
        CHECK(y->drift_us == y->start_us - y->nominal_us && y->start_us < y->end_us, "sync %d drift", y->number);
        for (int j = 0; j < ORACLE_CLOCK_SYNCS_BY_NUMBER_COUNT; j++)
            if (ORACLE_CLOCK_SYNCS_BY_NUMBER[j].ok && ORACLE_CLOCK_SYNCS_BY_NUMBER[j].number == y->number + 1)
                CHECK(ORACLE_CLOCK_SYNCS_BY_NUMBER[j].start_us == y->end_us, "sync %d chains", y->number);
    }
    for (int i = 0; i + 3 < ORACLE_CLOCK_PENTADS_BY_NUMBER_COUNT; i += 4) {
        const oracle_clock_pentad_by_number_t *p = &ORACLE_CLOCK_PENTADS_BY_NUMBER[i];
        for (int k = 0; k < 4; k++) {
            CHECK(p[k].index == k && p[k].sync == p[0].sync, "pentad %d/%d order", p[0].sync, k);
            if (p[k].ok) CHECK(p[k].helions == (k == 3 ? 4 : 5) && p[k].start_us < p[k].end_us, "pentad %d/%d", p[0].sync, k);
            if (k > 0 && p[k].ok && p[k - 1].ok) CHECK(p[k].start_us == p[k - 1].end_us, "pentad %d/%d chains", p[0].sync, k);
        }
    }

    /* SPEC S12, vector A, read back through the typedefs. */
    const oracle_clock_sample_t *a = &ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLE_A];
    CHECK(a->t_us == 1791460800000000LL && a->ordinal == 22, "A");
    CHECK(a->level[6].cycle == 717211 && a->level[6].local == 0 && a->level[6].siblings == 11, "A level 6 counter");
    CHECK(a->level[6].index[0] == 13 && a->level[6].glyph2.value == -3 && a->level[6].glyph2.derivative == 1, "A level 6 glyph");
    CHECK(a->level[6].index[1] == 42 && a->level[6].glyph3.value == -10, "A level 6 3-stroke glyph");
    CHECK(a->level[6].odometer[0] == 13 && a->level[6].odometer[1] == 0, "A level 6 odometer");
    CHECK(a->level[6].next_name == 3 && (a->level[6].next_us + 500) / 1000 == 1791464172428LL, "A level 6 threshold");
    CHECK(a->level[7].cycle == 6454906 && a->level[7].local == 7 && a->level[7].siblings == 9, "A level 7 counter");
    CHECK(a->level[7].index[0] == 5 && a->level[7].glyph2.value == 3 && a->level[7].glyph2.dotted == 1, "A level 7 glyph");
    CHECK(a->level[7].odometer[0] == 5 && a->level[7].odometer[1] == 4, "A level 7 odometer");
    CHECK(a->level[7].next_name == 1 && (a->level[7].next_us + 500) / 1000 == 1791461140937LL, "A level 7 threshold");
    CHECK(a->sync.has && a->sync.number == 4820 && (a->sync.start_us + 500) / 1000 == 1791336849826LL, "A sync");
    CHECK(a->sync.drift_us / 100 == -484699898 && a->sync.next_name == 1 && (a->sync.next_us + 500) / 1000 == 1791509644786LL, "A sync drift, threshold");
    CHECK(a->sync.digits_s2d3[0] == 5 && a->sync.digits_s2d3[1] == 11 && a->sync.digits_s2d3[2] == 13, "A sync digits");
    CHECK(a->sync.digits_s3d2[0] == 18 && a->sync.digits_s3d2[1] == 33, "A sync 3-stroke digits");
    CHECK(a->pentad.sync == 4820 && a->pentad.index == 1 && a->pentad.helion == 2, "A pentad");
    const oracle_clock_sample_t *n = &ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLE_ANCHOR_PLUS_1S];
    CHECK(n->sync.number == 0 && n->sync.drift_us == 0 && n->sync.digits_s2d3[0] + n->sync.digits_s2d3[1] + n->sync.digits_s2d3[2] == 0, "anchor");
    CHECK(ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLE_CLOSING_MINUS_1MS].t_us == last - 1000, "closing sample");
    CHECK(!ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLE_FIRST_PARTIAL_SYNC].sync.has && !ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLE_LAST_PARTIAL_SYNC].sync.has, "partial syncs");
}

static void check_notation(void) {
    size_t longest = 0;
    for (int i = 0; i < ORACLE_NOTATION_DATES_COUNT; i++) {
        const oracle_notation_date_t *d = &ORACLE_NOTATION_DATES[i];
        const oracle_clock_sample_t *s = &ORACLE_CLOCK_SAMPLES[d->clock_sample];
        const oracle_clock_level_t *l = s->level;
        CHECK(d->clock_sample < ORACLE_CLOCK_SAMPLES_COUNT && s->t_us == d->t_us && s->in_range, "date %d sample", i);
        CHECK(d->alpha == l[0].cycle + 1 && d->beta == l[1].local + 1 && d->gamma == l[2].local + 1 && d->delta == l[3].local + 1, "date %d majors", i);
        CHECK(d->epsilon == l[4].local + 1 && d->eta == 11 * l[5].local + l[6].local + 1, "date %d nuclide, helion", i);
        CHECK(d->iota == 4 * l[7].local + l[8].local && d->lambda == 13 * l[9].local + l[10].local && d->nu == 5 * l[11].local + l[12].local, "date %d counts", i);
        CHECK(d->eta >= 1 && d->eta <= 77 && d->iota <= 35 && d->lambda <= 64 && d->nu <= 24, "date %d ranges", i);
        for (int k = 0; k < 4; k++) CHECK(d->quarter[k] == l[k].quarter, "date %d quarter %d", i, k);
        for (int f = 0; f < ORACLE_NOTATION_FINEST_COUNT; f++)
            for (int form = 0; form < ORACLE_NOTATION_FORM_COUNT; form++) {
                size_t len = strlen(d->text[f][form]);
                CHECK(len > 0, "date %d text %d %d", i, f, form);
                if (len > longest) longest = len;
                /* a finer resolution only appends */
                if (f > 0) CHECK(strncmp(d->text[f][form], d->text[f - 1][form], strlen(d->text[f - 1][form])) == 0, "date %d text %d %d grows", i, f, form);
            }
        CHECK(strcmp(d->stamp, d->text[5][2]) == 0, "date %d stamp", i);
        for (int f = 0; f < 3; f++) CHECK(strncmp(d->pentad[f][1], d->pentad[f][0], strlen(d->pentad[f][0])) == 0, "date %d pentad %d", i, f);
        CHECK((s->pentad.index == 3) == (strcmp(d->pentad[2][0], "H") == 0), "date %d hypopentad", i);
    }
    CHECK(longest < 160, "longest date text is %zu bytes", longest);

    for (int i = 0; i < ORACLE_NOTATION_SPANS_COUNT; i++) {
        const oracle_notation_span_t *s = &ORACLE_NOTATION_SPANS[i];
        uint64_t sum = 0, zsum = 0;
        CHECK(s->negative == (s->t2_us < s->t1_us), "span %d sign", i);
        CHECK(s->n[0] <= 2 && s->n[1] <= 3 && s->n[0] <= s->n[1] && s->n[1] <= s->n[2] && s->n[2] >= 1 && s->n[2] <= 9, "span %d part counts", i);
        CHECK(s->nz[0] <= 2 && s->nz[1] <= 3 && s->nz[2] >= s->n[2] && s->nz[2] <= 9, "span %d zero part counts", i);
        for (int k = 0; k < 9; k++) {
            if (k < s->n[2]) {
                sum += s->parts[k].count * UNIT_BLINKS[s->parts[k].unit];
                CHECK(s->parts[k].unit <= 8 && (k == 0 || s->parts[k].unit > s->parts[k - 1].unit), "span %d part %d order", i, k);
                CHECK(s->parts[k].count > 0 || (k == 0 && s->blinks == 0), "span %d part %d is zero", i, k);
            } else {
                CHECK(s->parts[k].unit == 0 && s->parts[k].count == 0, "span %d part %d unused", i, k);
            }
            if (k < s->nz[2]) {
                zsum += s->zparts[k].count * UNIT_BLINKS[s->zparts[k].unit];
                CHECK(k == 0 || s->zparts[k].unit == s->zparts[k - 1].unit + 1, "span %d zero part %d consecutive", i, k);
            }
        }
        CHECK(sum == s->blinks && zsum == s->blinks, "span %d parts add up to %llu Blinks", i, (unsigned long long)s->blinks);
        CHECK(s->zparts[0].unit == s->parts[0].unit && s->zparts[s->nz[2] - 1].unit == 8, "span %d ladder ends", i);
        CHECK(strlen(s->text[0]) > 0 && strlen(s->text[1]) >= strlen(s->text[0]), "span %d text", i);
        /* U+2212 leads a negative span, unless it is too short to show a digit ("0\xCE\xBD" has no sign) */
        CHECK((strncmp(s->text[0], "\xE2\x88\x92", 3) == 0) == (s->negative && strcmp(s->text[0], "0\xCE\xBD") != 0), "span %d minus", i);
    }

    for (int i = 0; i < ORACLE_NOTATION_CYCLE_LENGTHS_COUNT; i++) {
        const oracle_notation_cycle_length_t *c = &ORACLE_NOTATION_CYCLE_LENGTHS[i];
        uint64_t p12 = prefix(12), p = prefix(c->level);
        CHECK(c->level == i && c->blinks == p12 / p, "cycle length %d blinks", i);
        /* below nu one cycle is 1 / (P(level) / P(12)) of a Blink */
        CHECK(c->frac == (c->level <= 12 ? 0 : (uint32_t)((1ULL << 32) / (p / p12))), "cycle length %d fraction", i);
    }
    for (int i = 0; i < ORACLE_NOTATION_CYCLE_NAMES_COUNT; i++) {
        const oracle_notation_cycle_name_t *c = &ORACLE_NOTATION_CYCLE_NAMES[i];
        size_t n = strlen(c->name), m = strlen(c->name_short), k = strlen(c->short_form);
        CHECK(c->level <= 16 && n > 0 && k > 0 && n >= m, "cycle name %d", i);
        CHECK(strcmp(c->name + (n - m), c->name_short) == 0, "cycle name %d: name ends with its short name", i);
        if (c->level > 4) CHECK(m >= k && strcmp(c->name_short + (m - k), c->short_form) == 0, "cycle name %d: short form is the last count", i);
    }
    for (int i = 0; i < ORACLE_NOTATION_ORDINALS_COUNT; i++) CHECK(ORACLE_NOTATION_ORDINALS[i].n >= 1 && strlen(ORACLE_NOTATION_ORDINALS[i].text) > 1, "ordinal %d", i);
    for (int i = 0; i < ORACLE_NOTATION_RATES_COUNT; i++) {
        const oracle_notation_rate_t *r = &ORACLE_NOTATION_RATES[i];
        CHECK(r->factor == (i == 0 ? 1 : r->count * UNIT_BLINKS[r->unit]), "rate %s", r->id);
        CHECK(i == 0 || ORACLE_NOTATION_RATES[i - 1].factor < r->factor, "rate %s order", r->id);
    }
    for (int i = 0; i < ORACLE_NOTATION_PER_BLINK_COUNT; i++) {
        const oracle_notation_per_blink_t *r = &ORACLE_NOTATION_PER_BLINK[i];
        CHECK(r->den > 0 && strlen(r->text) > 0, "pB %d", i);
        if (r->level >= 0) CHECK(r->level == i && r->num * prefix(12) == r->den * prefix(r->level), "pB level %d", r->level);
    }
    for (int i = 0; i < ORACLE_NOTATION_S15_6_COUNT; i++) {
        const oracle_notation_s15_6_t *r = &ORACLE_NOTATION_S15_6[i];
        const oracle_notation_date_t *d = &ORACLE_NOTATION_DATES[i]; /* the S12 instants are the first four of both tables */
        CHECK(r->clock_sample == i && d->t_us == r->t_us, "S15.6 %s sample", r->vector);
        CHECK(strcmp(r->full_date, d->text[5][0]) == 0 && strcmp(r->short_date, d->text[3][1]) == 0, "S15.6 %s texts", r->vector);
        CHECK(r->helion_pair[0] == ORACLE_CLOCK_SAMPLES[i].level[6].odometer[0] && r->helion_pair[1] == ORACLE_CLOCK_SAMPLES[i].level[6].odometer[1], "S15.6 %s pair", r->vector);
    }
    CHECK(strcmp(ORACLE_NOTATION_S15_6_STAMP_A, ORACLE_NOTATION_DATES[0].stamp) == 0, "S15.6 stamp");
    CHECK(ORACLE_NOTATION_S15_6_SPAN_T2_US - ORACLE_NOTATION_S15_6_SPAN_T1_US == 3372428000LL, "S15.6 span");
    CHECK(strcmp(ORACLE_NOTATION_S15_6_SPAN_TEXT, "6\xCE\xB9 43\xCE\xBB") == 0, "S15.6 span text is UTF-8");
    printf("longest date text: %zu bytes\n", longest);
}

int main(void) {
    check_clock();
    check_notation();
    printf("%ld checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
