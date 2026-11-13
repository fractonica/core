#!/usr/bin/env node
/*
 * generate-foundation.mjs - the FOUNDATION ORACLE of fractonica-core.
 *
 * Writes tests/vectors/oracle_foundation.h: what the frozen JavaScript reference
 * (reference/js/fractonica.js, exact BigInt rationals) says about the catalogue,
 * positions, cycles, thresholds and odometer steps of SPEC S2, S3, S5 and S6, in
 * the units of the C API. tests/test_foundation.c holds the C core against it.
 *
 * How the reference's answers become the C API's:
 *   - The reference clock runs on Unix SECONDS as exact rationals. A query instant
 *     here is an integer number of MICROSECONDS, given to it as the rational us / 1e6.
 *   - A boundary instant is rounded ONCE, with a ceiling, to the first microsecond at
 *     or after it (fractonica.h, "BOUNDARY INSTANTS"). "No such instant" is INT64_MIN.
 *   - The catalogue's closing instant belongs to its last interval: it is read as one
 *     microsecond earlier (frc_position_at), where the reference's own apps step back
 *     one millisecond (model.js, EDGE). A "next" instant must be strictly after the
 *     query instant, so AT the closing instant there is none: the reference would
 *     offer the closing instant itself there (the boundary after its earlier reading).
 *   - A phase is floor(fraction of the cycle * 2^32), the frc_frac_t of the header.
 *
 * Nothing in this file re-implements the clock: cycle facts come from clock.period,
 * thresholds from clock.nextThreshold, odometer steps from clock.phaseDigits, times
 * from clock.timestampAt. Only the final rounding to microseconds is done here.
 *
 * Also emitted: the catalogue of F.defaults.unixTimestamps in milliseconds, and the
 * exact P(0..63) of SPEC S16.4 as decimal strings (parsed from SPEC.md, and checked
 * against the reference's clock.prefix before anything is written).
 *
 * Usage (from anywhere):
 *   node tools/oracle/generate-foundation.mjs [--spec SPEC.md] [--reference fractonica.js]
 *                                             [--out tests/vectors]
 *
 * Output is deterministic: no clock, no Math.random (a fixed-seed generator picks
 * the sample instants).
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

/* ------------------------------------------------------------------------- */
/* Command line                                                               */
/* ------------------------------------------------------------------------- */

function parseArgs(argv) {
  const opts = { spec: null, reference: null, out: path.resolve(HERE, '../../tests/vectors') };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--spec') opts.spec = path.resolve(argv[++i]);
    else if (a === '--reference') opts.reference = path.resolve(argv[++i]);
    else if (a === '--out') opts.out = path.resolve(argv[++i]);
    else throw new Error(`Unknown argument: ${a}`);
  }
  const firstExisting = (list) => list.find((p) => fs.existsSync(p)) ?? null;
  // the repository's own copy first (docs/SPEC.md), then a SPEC.md beside or above the core
  opts.spec ??= firstExisting([
    path.resolve(HERE, '../../docs/SPEC.md'),
    path.resolve(HERE, '../../SPEC.md'),
    path.resolve(HERE, '../../../SPEC.md'),
  ]);
  opts.reference ??= firstExisting([path.resolve(HERE, '../../../reference/js/fractonica.js')]);
  if (!opts.spec) throw new Error('SPEC.md not found; pass --spec <path>.');
  if (!opts.reference) throw new Error('Reference library not found; pass --reference <path>.');
  return opts;
}

const opts = parseArgs(process.argv.slice(2));

/* The reference is a classic script that installs globalThis.Fractonica. */
await import(pathToFileURL(opts.reference).href);
const F = globalThis.Fractonica;
if (!F) throw new Error('The reference library did not define globalThis.Fractonica.');
const R = F.rational;
const clock = F.create({ timeScale: 'unix' });

/* ------------------------------------------------------------------------- */
/* Exact helpers                                                              */
/* ------------------------------------------------------------------------- */

const US = 1000000n;
const NONE = -(2n ** 63n); // FRC_TIME_NONE
const floorDiv = (n, d) => (n >= 0n ? n / d : -((-n + d - 1n) / d));
const ceilDiv = (n, d) => -floorDiv(-n, d);
/** A rational number of seconds -> the first microsecond at or after it. */
const ceilUs = (seconds) => ceilDiv(seconds.numerator * US, seconds.denominator);
const check = (cond, message) => {
  if (!cond) throw new Error(`Self-check failed: ${message}`);
};

/** Deterministic 64-bit generator (splitmix64), so the vectors never change by themselves. */
let seed = 0x1234_5678_9abc_def0n;
const MASK = (1n << 64n) - 1n;
function next64() {
  seed = (seed + 0x9e3779b97f4a7c15n) & MASK;
  let z = seed;
  z = ((z ^ (z >> 30n)) * 0xbf58476d1ce4e5b9n) & MASK;
  z = ((z ^ (z >> 27n)) * 0x94d049bb133111ebn) & MASK;
  return z ^ (z >> 31n);
}
const below = (n) => next64() % n;

/* ------------------------------------------------------------------------- */
/* Catalogue                                                                  */
/* ------------------------------------------------------------------------- */

const catalogueMs = F.defaults.unixTimestamps.map((s) => {
  const m = String(s).match(/^(-?)(\d+)\.(\d{3})$/);
  check(m, `timestamp ${s} is not a decimal with three places`);
  return BigInt(m[1] + m[2] + m[3]);
});
const E = catalogueMs.map((ms) => ms * 1000n); // microseconds
const COUNT = E.length;
const FIRST = E[0];
const LAST = E[COUNT - 1];
check(COUNT === 70, 'Saros 141 has 70 eclipses');
check(BigInt(clock.extent) === BigInt(COUNT - 1), 'the reference clock runs on the same catalogue');

/* ------------------------------------------------------------------------- */
/* Prefixes of SPEC S16.4                                                     */
/* ------------------------------------------------------------------------- */

const specText = fs.readFileSync(opts.spec, 'utf8');
const prefixes = [];
for (const m of specText.matchAll(/^P\((\d+)\) = (\d+)$/gm)) {
  check(Number(m[1]) === prefixes.length, `P(${m[1]}) is out of order in SPEC.md`);
  prefixes.push(m[2]);
}
check(prefixes.length === 64, `SPEC.md lists P(0..63), found ${prefixes.length}`);
prefixes.forEach((text, level) =>
  check(clock.prefix(level) === BigInt(text), `P(${level}) of SPEC.md equals the reference's`),
);

/* ------------------------------------------------------------------------- */
/* Readings                                                                   */
/* ------------------------------------------------------------------------- */

const QUARTER = { peak: 0, node: 1, valley: 2, end: 3 };
const MAX_LEVEL = 21; // FRC_CLOCK_MAX_LEVEL

/** The exact position the C core reads at instant `us`. */
function positionAt(us) {
  const read = us === LAST ? us - 1n : us; // the closing instant reads one microsecond earlier
  return clock.positionAt(R(read, US));
}

/**
 * A "next" instant of the reference, as the C API gives it: rounded up to a microsecond,
 * none beyond the catalogue, and none at the closing instant (see the header comment).
 */
function nextInstant(us, time) {
  if (!time || us === LAST) return NONE;
  const next = ceilUs(time);
  check(next > us, 'a next instant is strictly after the query instant');
  return next;
}

/** frc_position_t: ordinal + num/den with den the interval in microseconds. */
function positionVector(us) {
  const read = us === LAST ? us - 1n : us;
  let k = 0;
  while (k + 1 < COUNT - 1 && E[k + 1] <= read) k++;
  const v = { t: us, ordinal: k, num: read - E[k], den: E[k + 1] - E[k] };
  const p = positionAt(us);
  check(
    p.numerator * v.den === (BigInt(k) * v.den + v.num) * p.denominator,
    'ordinal + num/den is the reference position',
  );
  return v;
}

/** frc_cycle_at and frc_next_threshold at instant `us` and `level`. */
function cycleVector(us, level) {
  const p = positionAt(us);
  const P = clock.prefix(level);
  const quarters = p.numerator * P; // position * P = quarters / p.denominator
  const cycle = quarters / (4n * p.denominator);
  const inCycle = quarters - cycle * 4n * p.denominator; // / (4 den) = the fraction of the cycle done
  const phase = (inCycle << 32n) / (4n * p.denominator);
  const quarter = Number((inCycle * 4n) / (4n * p.denominator));

  const counter = clock.counterAt(p, level);
  check(counter.cycle === cycle, 'counterAt agrees on the cycle index');
  const period = clock.period(level, cycle);
  const start = ceilUs(period.startTime);
  const end = ceilUs(period.endTime);
  check(start <= us && (us < end || us === LAST), 'the instant lies in its cycle');

  const threshold = clock.nextThreshold(p, level);
  check(F.compare(threshold.position, p) > 0, 'the next threshold is strictly after the instant');
  check(QUARTER[threshold.name] === quarter, 'the next threshold ends the running quarter');

  return {
    t: us,
    level,
    cycle,
    quarter,
    phase,
    local: counter.local,
    siblings: counter.siblings,
    start,
    end,
    clipped: period.clipped ? 1 : 0,
    points: period.events.map((e) => (e.time ? ceilUs(e.time) : NONE)),
    nextName: QUARTER[threshold.name],
    next: nextInstant(us, threshold.time),
  };
}

/** The odometer step of S5 and when it ends: frc_i_fraction_step / _step_end with w = M * q^(digits-1). */
function stepVector(us, level, strokes, digits) {
  const p = positionAt(us);
  const d = clock.phaseDigits(p, level, { strokes, digits });
  const m = (3n ** BigInt(strokes) - 1n) / 2n;
  return {
    t: us,
    level,
    w: m * (4n * m) ** BigInt(digits - 1),
    step: d.step,
    next: nextInstant(us, d.nextTime),
  };
}

/** frc_time_at_position(n, d). */
function timeVector(n, d) {
  const inside = n <= BigInt(COUNT - 1) * d;
  return { n, d, time: inside ? ceilUs(clock.timestampAt(R(n, d))) : NONE };
}

/* ------------------------------------------------------------------------- */
/* Sample instants                                                            */
/* ------------------------------------------------------------------------- */

const randomInstant = () => FIRST + below(LAST - FIRST + 1n);
const specInstants = [1791460800n, 946684800n, 2886085800n].map((s) => s * US).concat([125593320816000n]);

/* Edges that a floor / ceiling slip would get wrong: the catalogue's two ends, eclipses
 * and the microseconds around them, and instants one microsecond around cycle starts. */
const edgeInstants = [FIRST, FIRST + 1n, LAST - 1n, LAST];
for (const k of [1, 19, 20, 21, 22, 23, 35, 68]) edgeInstants.push(E[k] - 1n, E[k], E[k] + 1n);

const cycles = [];
for (const t of specInstants) for (const level of [0, 3, 6, 7, 12, 21]) cycles.push(cycleVector(t, level));
for (const t of edgeInstants) for (const level of [0, 1, 6, 13, 21]) cycles.push(cycleVector(t, level));
for (let level = 0; level <= MAX_LEVEL; level++) {
  for (let i = 0; i < 16; i++) cycles.push(cycleVector(randomInstant(), level));
  /* the last cycle of the level: clipped unless it ends exactly on the closing instant */
  const lastCycle = clock.count(level) - 1n;
  const p = clock.period(level, lastCycle);
  const a = ceilUs(p.startTime);
  cycles.push(cycleVector(a, level), cycleVector(a + below(LAST - a + 1n), level));
  /* one microsecond either side of a random cycle's start */
  const c = clock.period(level, 1n + below(lastCycle));
  const s = ceilUs(c.startTime);
  cycles.push(cycleVector(s - 1n, level), cycleVector(s, level));
}

const positions = [];
for (const t of [...edgeInstants, ...specInstants]) positions.push(positionVector(t));
for (let i = 0; i < 40; i++) positions.push(positionVector(randomInstant()));

const steps = [];
const GLYPHS = [[1, 1], [2, 1], [2, 2], [2, 3], [3, 1], [3, 2], [4, 2], [5, 4]]; // [strokes, digits]
for (const t of specInstants) for (const level of [6, 7]) for (const [s, d] of [[2, 1], [2, 2], [3, 1]]) steps.push(stepVector(t, level, s, d));
for (let i = 0; i < 160; i++) {
  const [s, d] = GLYPHS[Number(below(BigInt(GLYPHS.length)))];
  steps.push(stepVector(randomInstant(), Number(below(BigInt(MAX_LEVEL + 1))), s, d));
}
for (const [s, d] of GLYPHS) for (const level of [0, 6, 21]) steps.push(stepVector(LAST, level, s, d), stepVector(LAST - 1n, level, s, d));

const times = [];
const extent = BigInt(COUNT - 1);
for (const [n, d] of [[0n, 1n], [1n, 1n], [extent, 1n], [extent + 1n, 1n], [extent * 7n, 7n], [extent * 7n + 1n, 7n], [extent * 7n - 1n, 7n], [1n, 3n], [45n, 2n], [0n, 12345n]]) times.push(timeVector(n, d));
for (let i = 0; i < 60; i++) {
  const d = 1n + below(i % 3 === 0 ? 1000n : i % 3 === 1 ? 10n ** 12n : 1n << 62n);
  const top = extent * d; // n <= top is inside; only tried while it fits 64 bits
  const n = top < MASK ? below(top + (i % 10 === 0 ? d : 1n)) : next64() >> 1n;
  times.push(timeVector(n, d));
}

/* ------------------------------------------------------------------------- */
/* Emit                                                                       */
/* ------------------------------------------------------------------------- */

const i64 = (v) => (v === NONE ? 'INT64_MIN' : `INT64_C(${v})`);
const u64 = (v) => `UINT64_C(${v})`;
const o = [];
o.push(
  '/*',
  ' * oracle_foundation.h - expected values for the foundation of the core: catalogue,',
  ' * positions, cycles, thresholds, odometer steps (SPEC S2, S3, S5, S6) and P(0..63).',
  ' *',
  ' * GENERATED by tools/oracle/generate-foundation.mjs. DO NOT EDIT: change the generator and run it again.',
  ` * Source of truth: the frozen JavaScript reference ${F.version} (exact BigInt rationals) and SPEC.md.`,
  ' *',
  ' * Times are Unix MICROSECONDS. A boundary instant is the first microsecond at or after the exact',
  ' * rational instant; INT64_MIN stands for FRC_TIME_NONE. The closing instant reads one microsecond early.',
  ' * Self-contained: it needs <stdint.h> only.',
  ' */',
  '#ifndef ORACLE_FOUNDATION_H',
  '#define ORACLE_FOUNDATION_H',
  '',
  '#include <stdint.h>',
  '',
  '#if defined(__GNUC__) || defined(__clang__)',
  '#  define ORACLE_FOUNDATION_DATA __attribute__((unused)) static const',
  '#else',
  '#  define ORACLE_FOUNDATION_DATA static const',
  '#endif',
  '',
  '/* ---- Catalogue: F.defaults.unixTimestamps, Unix milliseconds ------------------- */',
  '',
  `#define ORACLE_FOUNDATION_CATALOGUE_COUNT ${COUNT}`,
  `ORACLE_FOUNDATION_DATA int64_t ORACLE_FOUNDATION_CATALOGUE_MS[ORACLE_FOUNDATION_CATALOGUE_COUNT] = {`,
);
for (let i = 0; i < COUNT; i += 4) o.push('    ' + catalogueMs.slice(i, i + 4).map((v) => i64(v) + ',').join(' '));
o.push('};', '');

o.push(
  '/* ---- P(0..63), SPEC S16.4, exact decimal strings ------------------------------- */',
  '',
  `#define ORACLE_FOUNDATION_PREFIX_COUNT ${prefixes.length}`,
  'ORACLE_FOUNDATION_DATA char *const ORACLE_FOUNDATION_PREFIX[ORACLE_FOUNDATION_PREFIX_COUNT] = {',
);
prefixes.forEach((text, level) => o.push(`    "${text}", /* ${level} */`));
o.push('};', '');

o.push(
  '/* ---- frc_position_at ------------------------------------------------------------ */',
  '',
  'typedef struct oracle_foundation_position {',
  '    int64_t  t;       /* instant */',
  '    uint16_t ordinal;',
  '    uint64_t num;',
  '    uint64_t den;',
  '} oracle_foundation_position_t;',
  '',
  `#define ORACLE_FOUNDATION_POSITION_COUNT ${positions.length}`,
  'ORACLE_FOUNDATION_DATA oracle_foundation_position_t ORACLE_FOUNDATION_POSITION[ORACLE_FOUNDATION_POSITION_COUNT] = {',
);
for (const v of positions) o.push(`    {${i64(v.t)}, ${v.ordinal}, ${u64(v.num)}, ${u64(v.den)}},`);
o.push('};', '');

o.push(
  '/* ---- frc_cycle_at, frc_cycle_points, frc_next_threshold --------------------------- */',
  '',
  'typedef struct oracle_foundation_cycle {',
  '    int64_t  t;          /* instant */',
  '    uint8_t  level;',
  '    uint64_t cycle;      /* floor(position * P / 4) */',
  '    uint8_t  quarter;    /* 0 peak, 1 node, 2 valley, 3 end */',
  '    uint32_t phase;      /* floor(fraction of the cycle done * 2^32) */',
  '    uint16_t local;',
  '    uint16_t siblings;',
  '    int64_t  start;',
  '    int64_t  end;        /* the closing instant when clipped */',
  '    uint8_t  clipped;',
  '    int64_t  points[5];  /* start, peak, node, valley, end; INT64_MIN beyond the catalogue */',
  '    uint8_t  next_name;  /* the next threshold strictly after t */',
  '    int64_t  next_time;  /* INT64_MIN beyond the catalogue */',
  '} oracle_foundation_cycle_t;',
  '',
  `#define ORACLE_FOUNDATION_CYCLE_COUNT ${cycles.length}`,
  'ORACLE_FOUNDATION_DATA oracle_foundation_cycle_t ORACLE_FOUNDATION_CYCLE[ORACLE_FOUNDATION_CYCLE_COUNT] = {',
);
for (const v of cycles)
  o.push(
    `    {${i64(v.t)}, ${v.level}, ${u64(v.cycle)}, ${v.quarter}, UINT32_C(${v.phase}), ${v.local}, ${v.siblings}, ` +
      `${i64(v.start)}, ${i64(v.end)}, ${v.clipped}, {${v.points.map(i64).join(', ')}}, ${v.nextName}, ${i64(v.next)}},`,
  );
o.push('};', '');

o.push(
  '/* ---- Odometer steps (SPEC S5): frc_i_fraction_step / frc_i_fraction_step_end ------ */',
  '',
  'typedef struct oracle_foundation_step {',
  '    int64_t  t;      /* instant */',
  '    uint8_t  level;',
  '    uint64_t w;      /* states per quarter cycle: M * (4M)^(digits-1) */',
  '    uint64_t step;   /* floor(position * P * w) mod 4w */',
  '    int64_t  next;   /* when the step changes; INT64_MIN beyond the catalogue */',
  '} oracle_foundation_step_t;',
  '',
  `#define ORACLE_FOUNDATION_STEP_COUNT ${steps.length}`,
  'ORACLE_FOUNDATION_DATA oracle_foundation_step_t ORACLE_FOUNDATION_STEP[ORACLE_FOUNDATION_STEP_COUNT] = {',
);
for (const v of steps) o.push(`    {${i64(v.t)}, ${v.level}, ${u64(v.w)}, ${u64(v.step)}, ${i64(v.next)}},`);
o.push('};', '');

o.push(
  '/* ---- frc_time_at_position ------------------------------------------------------- */',
  '',
  'typedef struct oracle_foundation_time {',
  '    uint64_t n;     /* position n / d Saros */',
  '    uint64_t d;',
  '    int64_t  time;  /* INT64_MIN when n / d is beyond the catalogue */',
  '} oracle_foundation_time_t;',
  '',
  `#define ORACLE_FOUNDATION_TIME_COUNT ${times.length}`,
  'ORACLE_FOUNDATION_DATA oracle_foundation_time_t ORACLE_FOUNDATION_TIME[ORACLE_FOUNDATION_TIME_COUNT] = {',
);
for (const v of times) o.push(`    {${u64(v.n)}, ${u64(v.d)}, ${i64(v.time)}},`);
o.push('};', '', '#endif /* ORACLE_FOUNDATION_H */', '');

fs.mkdirSync(opts.out, { recursive: true });
const outPath = path.join(opts.out, 'oracle_foundation.h');
fs.writeFileSync(outPath, o.join('\n'));
console.log(
  `${path.relative(process.cwd(), outPath)}: ${positions.length} positions, ${cycles.length} cycles, ` +
    `${steps.length} steps, ${times.length} times`,
);
