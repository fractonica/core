#!/usr/bin/env node
/**
 * generate-clock.mjs - the CLOCK + NOTATION oracle of fractonica-core.
 *
 * Asks the frozen JavaScript reference what a correct clock reads and writes the answers as two
 * self-contained C headers of test vectors:
 *
 *     tests/vectors/oracle_clock.h      SPEC S2-S7: position, cycles, glyphs, odometers, thresholds, sync, Pentads
 *     tests/vectors/oracle_notation.h   SPEC S15:   dates, stamps, Pentad words, spans, cycle names, rates, pB
 *
 * Run from the core repository:
 *
 *     node tools/oracle/generate-clock.mjs            write both headers
 *     node tools/oracle/generate-clock.mjs --check    regenerate in memory and fail if the files on disk differ
 *
 * The reference is found through the environment variable FRACTONICA_REFERENCE (default ../reference/js,
 * relative to the core repository). It is read-only: this script never writes there.
 *
 * HOW THE REFERENCE IS DRIVEN
 *   fractonica.js    defines globalThis.Fractonica: the exact BigInt-rational library (SPEC S1-S14).
 *   src/core/model.js (createModel)  the reading rules of the apps: 1 ms rounding, the catalogue edge, Pentads.
 *   src/core/pure.js  (createPure)   the notation of SPEC S15.
 *   Every time and every fraction written to a header comes from the library's EXACT rational API
 *   (positionAt, timestampAt, period, phaseAt, phaseDigits, nextThreshold, solarSync) and is rounded once,
 *   here, with BigInt. The model's doubles are only used to cross-check those values (CROSS-CHECKS below).
 *
 * CONVENTIONS OF THE C CORE (include/fractonica.h is the contract; the vectors are written in its terms)
 *   time       int64 Unix MICROSECONDS. Sample instants are whole milliseconds, because the model rounds to 1 ms.
 *   boundary   the FIRST MICROSECOND AT OR AFTER the exact rational instant: ceil(seconds * 1e6).
 *   NONE       a boundary beyond the catalogue is ORACLE_CLOCK_NONE (INT64_MIN), FRC_TIME_NONE in the core.
 *   position   ordinal k, num = t - E[k], den = E[k+1] - E[k], both in microseconds.
 *   phase      floor(exact fraction * 2^32) as uint32. A cycle's phase is linear in POSITION; the sync's phase
 *              is linear in TIME, (t - start) / (end - start) with the EXACT rational start and end.
 *   drift      sync start (the ceiled microsecond) - nominal (an exact microsecond count).
 *   trits      least significant first.
 *   levels     zero-based. Never depths.
 *
 * EXACTNESS WINS
 *   pure.js words spans, sub-Blink decimals and pB values in doubles, with a 1e-12 Saros epsilon. The core is
 *   exact, so this script re-implements those few wording rules in exact arithmetic (search "exact rule") and
 *   emits the exact text. Every vector where the reference's double text differs is listed in a comment block
 *   at the top of oracle_notation.h.
 *
 * DETERMINISM
 *   No Date.now, no Math.random: samples come from a fixed-seed PRNG (mulberry32). The same reference gives
 *   byte-identical headers on every run.
 */

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

// ---------------------------------------------------------------------------------------------------------
// Locations and the reference
// ---------------------------------------------------------------------------------------------------------

const CORE = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const REFERENCE = process.env.FRACTONICA_REFERENCE
  ? path.resolve(process.cwd(), process.env.FRACTONICA_REFERENCE)
  : path.resolve(CORE, '..', 'reference', 'js');
const OUT_DIR = path.join(CORE, 'tests', 'vectors');
const CHECK_ONLY = process.argv.includes('--check');

const refFile = (rel) => pathToFileURL(path.join(REFERENCE, rel)).href;
if (!fs.existsSync(path.join(REFERENCE, 'fractonica.js'))) {
  console.error(`generate-clock: no fractonica.js in ${REFERENCE} (set FRACTONICA_REFERENCE)`);
  process.exit(2);
}
await import(refFile('fractonica.js')); // defines globalThis.Fractonica
const F = globalThis.Fractonica;
const { createModel } = await import(refFile('src/core/model.js'));
const { createPure, QUARTER_WORDS } = await import(refFile('src/core/pure.js'));

const model = createModel(); // Saros 141 on the Unix clock, sync anchor 20
const pure = createPure(model);
const clock = model.clock;
const R = F.rational;
const compare = F.compare;

// ---------------------------------------------------------------------------------------------------------
// Exact arithmetic helpers (BigInt)
// ---------------------------------------------------------------------------------------------------------

const floorDiv = (a, b) => {
  const q = a / b;
  return a % b !== 0n && a < 0n !== b < 0n ? q - 1n : q;
};
const ceilDiv = (a, b) => -floorDiv(-a, b);
const absBig = (a) => (a < 0n ? -a : a);
const gcd = (a, b) => {
  [a, b] = [absBig(a), absBig(b)];
  while (b) [a, b] = [b, a % b];
  return a;
};
/** round(n / d), a half going up; n >= 0, d > 0. */
const roundHalfUp = (n, d) => (2n * n + d) / (2n * d);
const US_PER_S = 1_000_000n;
/** A boundary instant: the first microsecond at or after an exact rational count of seconds. */
const ceilUs = (seconds) => ceilDiv(seconds.numerator * US_PER_S, seconds.denominator);
/** floor(n / d * 2^32) for 0 <= n < d: a fraction as frc_frac_t. */
const frac32 = (n, d) => {
  const f = (n << 32n) / d;
  assert(f >= 0n && f < 1n << 32n, 'fraction outside [0, 1)');
  return f;
};

// ---------------------------------------------------------------------------------------------------------
// The catalogue and the hierarchy
// ---------------------------------------------------------------------------------------------------------

const SERIES = 141;
const ANCHOR = 20;
const SYNC_LEVEL = 6;
const SYNC_CYCLES = 19n;
const SYNC_DAYS = 4n;
const US_PER_DAY = 86_400_000_000n;
const SEQUENCE = [...F.defaults.sequence];
assert.deepEqual(SEQUENCE, [5, 13, 5, 5, 7, 11, 9, 4]);
assert.equal(model.series, SERIES);
assert.equal(model.anchor, ANCHOR);

const EXTENT = Number(clock.extent); // 69: the catalogue covers positions 0..69
/** E[k] in Unix milliseconds, exactly as published. */
const E_MS = clock.timestamps.map((r) => {
  assert((r.numerator * 1000n) % r.denominator === 0n, 'catalogue instant is not a whole millisecond');
  return (r.numerator * 1000n) / r.denominator;
});
const E_US = E_MS.map((ms) => ms * 1000n);
assert.equal(E_MS.length, EXTENT + 1);
const FIRST_US = E_US[0];
const LAST_US = E_US[EXTENT];

const MODEL_MAX_LEVEL = 13; // the reference model reads levels 0..13
const CLOCK_MAX_LEVEL = 21; // FRC_CLOCK_MAX_LEVEL
const prefix = (level) => clock.prefix(level);
const P12 = prefix(12);
const siblingsOf = (level) => (level === 0 ? clock.count(0) : BigInt(SEQUENCE[(level - 1) % SEQUENCE.length]));
const extreme = (strokes) => (3n ** BigInt(strokes) - 1n) / 2n;

const QUARTER_NAMES = ['peak', 'node', 'valley', 'end'];
const quarterIndex = (name) => {
  const i = QUARTER_NAMES.indexOf(name);
  assert(i >= 0, `unknown threshold name ${name}`);
  return i;
};

// ---------------------------------------------------------------------------------------------------------
// Samples: deterministic instants, whole milliseconds
// ---------------------------------------------------------------------------------------------------------

/** mulberry32: a tiny PRNG with a fixed seed, so the vectors never change between runs. */
function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0);
  };
}
const next32 = mulberry32(0x0141_2026);
/** A uniform BigInt in [0, range). */
const randomBelow = (range) => ((BigInt(next32()) << 32n) | BigInt(next32())) % range;
const randomBetween = (lo, hi) => lo + randomBelow(hi - lo + 1n);

const A_MS = 1_791_460_800_000n; // SPEC S12 vector A, 2026-10-08T12:00:00Z
const B_MS = 946_684_800_000n; //   vector B, 2000-01-01T00:00:00Z
const C_MS = 2_886_085_800_000n; // vector C, 2061-06-15T18:30:00Z
const ANCHOR_PLUS_1S_MS = E_MS[ANCHOR] + 1000n;

function buildSamples() {
  const samples = [];
  const seen = new Set();
  const add = (label, tMs) => {
    if (seen.has(tMs)) return;
    seen.add(tMs);
    samples.push({ label, tMs });
  };
  // 1. The four SPEC S12 instants, FIRST and in this order.
  add('A', A_MS);
  add('B', B_MS);
  add('C', C_MS);
  add('anchor+1s', ANCHOR_PLUS_1S_MS);
  // 2. Every eclipse and the milliseconds beside it. E[69] itself is left out: the core reads the closing
  //    instant one MICROSECOND inside the catalogue and the reference one MILLISECOND inside it, so they agree
  //    from E[69] - 1 ms down, which is the one sample taken there.
  for (let k = 0; k < EXTENT; k++) add(`E[${k}]`, E_MS[k]);
  for (let k = 0; k < EXTENT; k++) add(`E[${k}]+1ms`, E_MS[k] + 1n);
  for (let k = 1; k <= EXTENT; k++) add(`E[${k}]-1ms`, E_MS[k] - 1n);
  // 3. Uniform over the whole catalogue (never inside its last 2 ms).
  for (let i = 0; i < 130; i++) add(`catalogue#${i}`, randomBetween(E_MS[0], E_MS[EXTENT] - 2n));
  // 4. Uniform over 1970-01-01 .. 2100-01-01, where clients live.
  for (let i = 0; i < 170; i++) add(`modern#${i}`, randomBetween(0n, 4_102_444_800_000n));
  // 5. Outside the catalogue: the clock is undefined there (SPEC S2.2).
  add('E[0]-1ms (outside)', E_MS[0] - 1n);
  add(`E[${EXTENT}]+1ms (outside)`, E_MS[EXTENT] + 1n);
  add('far before (outside)', E_MS[0] - 86_400_000n * 36_525n);
  add('far after (outside)', E_MS[EXTENT] + 86_400_000n * 36_525n);
  for (const s of samples) {
    s.tUs = s.tMs * 1000n;
    s.tSec = Number(s.tMs) / 1000; // what the model and pure.js are asked
    s.inRange = s.tMs >= E_MS[0] && s.tMs <= E_MS[EXTENT];
    assert(!(s.inRange && s.tMs > E_MS[EXTENT] - 1n), 'a sample sits within 1 ms of the closing instant');
  }
  return samples;
}

// ---------------------------------------------------------------------------------------------------------
// Cross-checks against the model's doubles
// ---------------------------------------------------------------------------------------------------------

let crossChecks = 0;
/** An exact microsecond value against a double of seconds from the model (which carries ~1e-5 s of error). */
function nearSeconds(us, seconds, what) {
  crossChecks++;
  if (us === null || seconds === null) {
    assert(us === null && seconds === null, `${what}: one side has no instant (${us} vs ${seconds})`);
    return;
  }
  const diff = Math.abs(Number(us) / 1e6 - seconds);
  assert(diff < 5e-4, `${what}: exact ${us} us vs model ${seconds} s (off by ${diff} s)`);
}
function same(actual, expected, what) {
  crossChecks++;
  assert.deepEqual(actual, expected, what);
}

// ---------------------------------------------------------------------------------------------------------
// Reading the clock (exact)
// ---------------------------------------------------------------------------------------------------------

/** The exact position of an in-range instant: the library's rational, and the core's ordinal + num/den. */
function positionOf(tMs) {
  const p = clock.positionAt(R(tMs, 1000n));
  const tUs = tMs * 1000n;
  let k = EXTENT - 1;
  while (E_US[k] > tUs) k--;
  const num = tUs - E_US[k];
  const den = E_US[k + 1] - E_US[k];
  assert(num >= 0n && num < den, 'instant on or after the closing eclipse');
  assert(p.numerator * den === (BigInt(k) * den + num) * p.denominator, 'ordinal + num/den is not the position');
  return { p, ordinal: k, num, den };
}

/** value, derivative, dotted and trits (LEAST significant first) of a glyph state, from the library. */
function glyphOf(phase, strokes) {
  const value = Number(phase.value);
  const trits = [...phase.trits].reverse(); // the library lists the most significant trit first
  assert.equal(trits.length, strokes);
  assert.equal(trits.reduce((sum, t, i) => sum + t * 3 ** i, 0), value, 'trits do not spell the value');
  return { index: Number(phase.index), value, derivative: phase.derivative, dotted: phase.derivative < 0 ? 1 : 0, trits };
}

/** The glyph index of SPEC S4.1 computed here, without the library: floor(position * P * M) mod 4M. */
const glyphIndex = (p, level, strokes) => {
  const m = extreme(strokes);
  return Number(((p.numerator * prefix(level) * m) / p.denominator) % (4n * m));
};

function odometerOf(p, level, strokes, digits) {
  const d = clock.phaseDigits(p, level, { strokes, digits });
  return { digits: d.digits.map(Number), next: d.nextTime ? ceilUs(d.nextTime) : null };
}

/**
 * Everything about one level at one position. Works for any level the library accepts; levels above 13 are
 * outside the reference model, so their cycle, phase and glyph are computed here from P(L) and then checked
 * against clock.period / clock.phaseAt, which do accept them.
 */
function readLevel(p, level) {
  const x = p.numerator * prefix(level);
  const quad = 4n * p.denominator; // position * P / 4 = x / quad
  const cycle = x / quad;
  const rem = x - cycle * quad;
  const siblings = siblingsOf(level);
  const per = clock.period(level, cycle);
  const glyphs = [2, 3, 4].map((strokes) => {
    const g = glyphOf(clock.phaseAt(p, level, { strokes }), strokes);
    assert.equal(g.index, glyphIndex(p, level, strokes), 'glyph index differs from S4.1');
    return g;
  });
  const threshold = clock.nextThreshold(p, level);
  const out = {
    level,
    cycle,
    local: level === 0 ? cycle : cycle % siblings,
    siblings,
    phase: frac32(rem, quad),
    quarter: Number((4n * rem) / quad),
    clipped: per.clipped ? 1 : 0,
    start: ceilUs(per.startTime),
    end: ceilUs(per.endTime), // a clipped cycle ends at the closing instant
    glyphs,
    odometer: odometerOf(p, level, 2, 2),
    nextName: quarterIndex(threshold.name),
    nextTime: threshold.time ? ceilUs(threshold.time) : null,
  };
  // The next threshold is the one that ends the running quarter (S6.2, S15.2).
  assert.equal(out.nextName, out.quarter, 'next threshold is not the end of the running quarter');
  assert.equal(out.odometer.digits[0], glyphs[0].index, 'odometer digit 0 is not the glyph index');
  return out;
}

/** The model's reading of the same level, held against the exact one. */
function crossCheckLevel(sample, l) {
  const m = model.levelAt(sample.tSec, l.level, { strokes: 2 });
  const what = `${sample.label} level ${l.level}`;
  same(m.cycleBig, l.cycle, `${what} cycle`);
  same(m.local, Number(l.local), `${what} local`);
  same(m.siblings, Number(l.siblings), `${what} siblings`);
  same(m.quarter, l.quarter, `${what} quarter`);
  same([m.index, m.value, m.derivative, m.dotted ? 1 : 0], [l.glyphs[0].index, l.glyphs[0].value, l.glyphs[0].derivative, l.glyphs[0].dotted], `${what} glyph`);
  same(m.clipped ? 1 : 0, l.clipped, `${what} clipped`);
  crossChecks++;
  assert(Math.abs(m.phase - Number(l.phase) / 2 ** 32) < 1e-9, `${what} phase`);
  nearSeconds(l.start, m.start, `${what} start`);
  nearSeconds(l.end, m.end, `${what} end`);
  same(m.next.name, QUARTER_NAMES[l.nextName], `${what} next threshold`);
  nearSeconds(l.nextTime, m.next.time, `${what} next threshold time`);
  const odo = model.odometerAt(sample.tSec, l.level, { strokes: 2, digits: 2 });
  same(odo.digits, l.odometer.digits, `${what} odometer`);
  nearSeconds(l.odometer.next, odo.nextTime, `${what} odometer next`);
}

// ---- solar sync (SPEC S7.1) -------------------------------------------------------------------------------

const solar = clock.solarSync({ anchor: ANCHOR }); // level 6, 19 cycles, 4 days
const ANCHOR_CYCLE = (BigInt(ANCHOR) * prefix(SYNC_LEVEL)) / 4n; // the anchor's Helion: 625625
const syncPosition = (number) => R((ANCHOR_CYCLE + SYNC_CYCLES * BigInt(number)) * 4n, prefix(SYNC_LEVEL));
const nominalUs = (number) => E_US[ANCHOR] + BigInt(number) * SYNC_DAYS * US_PER_DAY;
const insideCatalogue = (lo, hi) => compare(lo, R(0n)) >= 0 && compare(hi, R(clock.extent)) <= 0;

const syncPhaseRoundedDiffers = []; // labels of the samples whose phase from the ROUNDED start and end would differ
let syncDigitsFromPhase32Differ = 0; // how often 2 x 3-stroke digits taken from the 32-bit phase would differ

/** The sync running at t, or null inside the catalogue's first / last partial sync (the library throws there). */
function readSync(sample) {
  const t = R(sample.tMs, 1000n);
  let at;
  try {
    at = solar.at(t);
  } catch (err) {
    if (err instanceof RangeError) return null;
    throw err;
  }
  const number = Number(at.sync);
  const start = ceilUs(at.startTime);
  const end = ceilUs(at.endTime);
  const nominal = nominalUs(number);
  assert.equal(ceilUs(at.nominalStart), nominal, 'nominal start is not a whole microsecond count');
  const out = {
    number,
    start,
    end,
    nominal,
    drift: start - nominal,
    phase: frac32(at.phase.numerator, at.phase.denominator),
    quarter: at.quarter,
    nextName: quarterIndex(at.threshold.name),
    nextTime: ceilUs(at.threshold.time),
    digitsS2D3: solar.digits(t, { strokes: 2, digits: 3 }).map(Number), // the reference: 3 glyphs of 2 strokes
    digitsS3D2: solar.digits(t, { strokes: 3, digits: 2 }).map(Number), // the alternative: 2 glyphs of 3 strokes
  };
  assert.equal(out.quarter, Number(out.phase >> 30n), 'sync quarter is not the top two bits of its phase');
  if (frac32(sample.tUs - start, end - start) !== out.phase) syncPhaseRoundedDiffers.push(sample.label);
  // 16^3 divides 2^32, so the 3 x 2-stroke digits are bits of the phase; 52^2 does not, so those are not.
  assert.deepEqual(out.digitsS2D3, [28n, 24n, 20n].map((shift) => Number((out.phase >> shift) & 15n)));
  if (Number((out.phase * 2704n) >> 32n) !== out.digitsS3D2[0] * 52 + out.digitsS3D2[1]) syncDigitsFromPhase32Differ++;
  const m = model.solarAt(sample.tSec);
  const what = `${sample.label} sync`;
  same(m.sync, number, what);
  nearSeconds(start, m.start, `${what} start`);
  nearSeconds(end, m.end, `${what} end`);
  nearSeconds(nominal, m.nominalStart, `${what} nominal`);
  nearSeconds(out.drift, m.drift, `${what} drift`);
  same(m.quarter, out.quarter, `${what} quarter`);
  same(m.threshold.name, QUARTER_NAMES[out.nextName], `${what} threshold`);
  nearSeconds(out.nextTime, m.threshold.time, `${what} threshold time`);
  same(m.digits3, out.digitsS2D3, `${what} digits 3 x 2-stroke`);
  same(m.digits2x3, out.digitsS3D2, `${what} digits 2 x 3-stroke`);
  return out;
}

/** Sync `number` as frc_sync_by_number gives it, or null when it is not whole inside the catalogue. */
function syncByNumber(number) {
  const lo = syncPosition(number);
  const hi = syncPosition(number + 1);
  const m = model.solarSyncByNumber(number);
  if (!insideCatalogue(lo, hi)) {
    same(m, null, `sync ${number} should be outside`);
    return null;
  }
  const start = ceilUs(clock.timestampAt(lo));
  const end = ceilUs(clock.timestampAt(hi));
  const nominal = nominalUs(number);
  nearSeconds(start, m.start, `sync ${number} start`);
  nearSeconds(end, m.end, `sync ${number} end`);
  nearSeconds(nominal, m.nominalStart, `sync ${number} nominal`);
  return { number, start, end, nominal, drift: start - nominal };
}

// ---- Pentads (SPEC S7.5) ------------------------------------------------------------------------------------

/** Start and end of Pentad `index` of sync `number`, cut at the catalogue's edges; null when wholly outside. */
function pentadByNumber(number, index) {
  const first = ANCHOR_CYCLE + SYNC_CYCLES * BigInt(number) + 5n * BigInt(index);
  const helions = index === 3 ? 4 : 5;
  const lo = R(first * 4n, prefix(SYNC_LEVEL));
  const hi = R((first + BigInt(helions)) * 4n, prefix(SYNC_LEVEL));
  const extent = R(clock.extent);
  const m = model.pentadByNumber(number, index);
  if (compare(hi, R(0n)) <= 0 || compare(lo, extent) >= 0) {
    same(m, null, `pentad ${number}/${index} should be outside`);
    return null;
  }
  const start = compare(lo, R(0n)) < 0 ? FIRST_US : ceilUs(clock.timestampAt(lo));
  const end = compare(hi, extent) > 0 ? LAST_US : ceilUs(clock.timestampAt(hi));
  nearSeconds(start, m.start, `pentad ${number}/${index} start`);
  nearSeconds(end, m.end, `pentad ${number}/${index} end`);
  return { sync: number, index, helions, start, end };
}

/** The Pentad running at a sample, from its Helion (level 6) cycle. Defined in the partial syncs too. */
function readPentad(sample, helionCycle) {
  const k = helionCycle - ANCHOR_CYCLE; // Helions since the anchor's Helion
  const sync = floorDiv(k, SYNC_CYCLES);
  const within = Number(k - sync * SYNC_CYCLES); // 0..18
  const index = Math.min(3, Math.floor(within / 5));
  const span = pentadByNumber(Number(sync), index);
  assert(span, 'a Pentad running inside the catalogue has a span');
  const out = { ...span, helion: within - 5 * index + 1 };
  const m = model.pentadAt(sample.tSec);
  same([m.sync, m.index, m.helion, m.helions], [out.sync, out.index, out.helion, out.helions], `${sample.label} pentad`);
  nearSeconds(out.start, m.start, `${sample.label} pentad start`);
  nearSeconds(out.end, m.end, `${sample.label} pentad end`);
  return out;
}

// ---- one sample ---------------------------------------------------------------------------------------------

function readSample(sample) {
  same(model.frame(sample.tSec).inRange, sample.inRange, `${sample.label} in range`);
  if (!sample.inRange) return;
  const pos = positionOf(sample.tMs);
  sample.pos = pos;
  sample.levels = [];
  for (let level = 0; level <= MODEL_MAX_LEVEL; level++) {
    const l = readLevel(pos.p, level);
    crossCheckLevel(sample, l);
    sample.levels.push(l);
  }
  sample.odo6S3D2 = odometerOf(pos.p, 6, 3, 2);
  sample.odo6S2D3 = odometerOf(pos.p, 6, 2, 3);
  same(model.odometerAt(sample.tSec, 6, { strokes: 3, digits: 2 }).digits, sample.odo6S3D2.digits, `${sample.label} odometer 3 strokes`);
  same(model.odometerAt(sample.tSec, 6, { strokes: 2, digits: 3 }).digits, sample.odo6S2D3.digits, `${sample.label} odometer 3 digits`);
  sample.sync = readSync(sample);
  sample.pentad = readPentad(sample, sample.levels[SYNC_LEVEL].cycle);
  if (sample.sync) assert.equal(sample.sync.number, sample.pentad.sync, 'sync and Pentad disagree on the sync number');
}

// ---- deep levels 14..21 -------------------------------------------------------------------------------------

function readDeep(sample) {
  const levels = [];
  for (let level = MODEL_MAX_LEVEL + 1; level <= CLOCK_MAX_LEVEL; level++) {
    const l = readLevel(sample.pos.p, level); // readLevel already checks itself against clock.period / phaseAt
    assert(l.cycle < 1n << 63n, 'cycle index does not fit 64 bits');
    assert(l.start <= sample.tUs && sample.tUs < l.end, 'the instant is not inside its deep cycle');
    levels.push(l);
  }
  return levels;
}

// ---- cycles by index ----------------------------------------------------------------------------------------

function cycleByIndex(level, cycle) {
  const per = clock.period(level, cycle);
  const siblings = siblingsOf(level);
  const points = per.events.map((e) => (e.time ? ceilUs(e.time) : null)); // begin, peak, node, valley, end
  const out = {
    level,
    cycle,
    local: level === 0 ? cycle : cycle % siblings,
    siblings,
    clipped: per.clipped ? 1 : 0,
    start: ceilUs(per.startTime),
    end: ceilUs(per.endTime),
    points,
  };
  assert.equal(points[0], out.start);
  if (!out.clipped) assert.equal(points[4], out.end);
  else assert.equal(out.end, LAST_US, 'a clipped cycle ends at the closing instant');
  if (level <= MODEL_MAX_LEVEL) {
    const m = model.cycleByIndex(level, cycle);
    nearSeconds(out.start, m.start, `cycle ${level}/${cycle} start`);
    nearSeconds(out.end, m.end, `cycle ${level}/${cycle} end`);
    m.events.forEach((e, i) => nearSeconds(points[i], e.time, `cycle ${level}/${cycle} point ${i}`));
  }
  return out;
}

function buildCycleTable() {
  const rows = [];
  const seen = new Set();
  const add = (level, cycle) => {
    const key = `${level}:${cycle}`;
    if (seen.has(key) || cycle < 0n || cycle >= clock.count(level)) return;
    seen.add(key);
    rows.push(cycleByIndex(level, cycle));
  };
  const a = positionOf(A_MS).p;
  for (let level = 0; level <= CLOCK_MAX_LEVEL; level++) {
    const count = clock.count(level);
    add(level, 0n); // the first cycle
    add(level, 1n);
    add(level, count - 1n); // the LAST cycle: clipped for levels 0..7, whole from level 8 down (4 divides P)
    add(level, count - 2n);
    add(level, (a.numerator * prefix(level)) / (4n * a.denominator)); // the cycle running at vector A
    for (let i = 0; i < 4; i++) add(level, randomBelow(count));
  }
  return rows;
}

// ---------------------------------------------------------------------------------------------------------
// Notation (SPEC S15)
// ---------------------------------------------------------------------------------------------------------

const UNIT_LEVELS = [0, 1, 2, 3, 4, 6, 8, 10, 12]; // frc_unit_t order: alpha .. nu
const UNIT_SYMBOLS = ['α', 'β', 'γ', 'δ', 'ε', 'η', 'ι', 'λ', 'ν'];
const UNIT_IDS = ['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'eta', 'iota', 'lambda', 'nu'];
const UNIT_BLINKS = UNIT_LEVELS.map((level) => P12 / prefix(level)); // exact: 7319812500 .. 25, 1
const NU = 8;
const MINUS = '−';
const NNBSP = ' '; // the narrow no-break space of digit groups
const DASH = '—';

const FINEST = ['delta', 'epsilon', 'eta', 'iota', 'lambda', 'nu']; // index = frc_unit_t - FRC_UNIT_DELTA
const DATE_FORMS = [
  { form: 'full', letters: false }, //  FRC_DATE_FULL
  { form: 'short', letters: false }, // FRC_DATE_SHORT
  { form: 'full', letters: true }, //   FRC_DATE_LETTERS
  { form: 'short', letters: true }, //  FRC_DATE_SHORT_LETTERS
];
const PENTAD_FORMS = ['full', 'short', 'ascii']; // frc_pentad_form_t

function readDate(sample) {
  const t = sample.tSec;
  const d = pure.date(t);
  const c = pure.clock(t);
  const L = sample.levels;
  const date = {
    alpha: d.alpha, beta: d.beta, gamma: d.gamma, delta: d.delta, epsilon: d.epsilon,
    eta: c.eta, iota: c.iota, lambda: c.lambda, nu: c.nu,
    quarter: ['alpha', 'beta', 'gamma', 'delta'].map((key) => QUARTER_WORDS.indexOf(d.quarter[key])),
  };
  // The same fields from the exact level readings (SPEC S15.3).
  const local = (level) => Number(L[level].local);
  same(
    date,
    {
      alpha: Number(L[0].cycle) + 1, beta: local(1) + 1, gamma: local(2) + 1, delta: local(3) + 1, epsilon: local(4) + 1,
      eta: 11 * local(5) + local(6) + 1, iota: 4 * local(7) + local(8), lambda: 13 * local(9) + local(10), nu: 5 * local(11) + local(12),
      quarter: [0, 1, 2, 3].map((level) => L[level].quarter),
    },
    `${sample.label} date fields`,
  );
  date.text = FINEST.map((upTo) => DATE_FORMS.map((f) => pure.dateText(t, { ...f, upTo })));
  date.stamp = pure.stamp(t, 'nu');
  same(date.stamp, `α#${date.alpha} β#${date.beta} γ#${date.gamma} δ#${date.delta} ε#${date.epsilon} ${date.eta}η ${date.iota}ι ${date.lambda}λ ${date.nu}ν`, `${sample.label} stamp`);
  same(date.stamp, date.text[5][2], `${sample.label} stamp is the letters form down to nu`);
  date.pentad = PENTAD_FORMS.map((form) => [pure.pentadText(t, { form }), pure.pentadText(t, { form, helion: true })]);
  return date;
}

// ---- exact rule: decimals -----------------------------------------------------------------------------------

/** n / 10^scale as a plain decimal without trailing zeros: (125, 2) -> "1.25", (300, 2) -> "3". */
function decimalText(n, scale) {
  if (scale <= 0) return (n * 10n ** BigInt(-scale)).toString();
  const s = n.toString().padStart(scale + 1, '0');
  const fraction = s.slice(-scale).replace(/0+$/, '');
  return fraction ? `${s.slice(0, -scale)}.${fraction}` : s.slice(0, -scale);
}
/** x = n/d (> 0) rounded to `digits` SIGNIFICANT digits, a half going up: Number(x.toPrecision(digits)) said exactly. */
function significantText(n, d, digits) {
  assert(n > 0n && d > 0n);
  const atLeast = (e) => (e >= 0 ? n >= d * 10n ** BigInt(e) : n * 10n ** BigInt(-e) >= d); // x >= 10^e
  let e = 0; // 10^e <= x < 10^(e+1)
  while (atLeast(e + 1)) e++;
  while (!atLeast(e)) e--;
  const scale = digits - 1 - e; // decimals kept
  const scaled = scale >= 0 ? roundHalfUp(n * 10n ** BigInt(scale), d) : roundHalfUp(n, d * 10n ** BigInt(-scale));
  return decimalText(scaled, scale);
}
/** x = n/d rounded to `decimals` decimals, a half going up: Math.round(x * 10) / 10 said exactly. */
const fixedText = (n, d, decimals) => decimalText(roundHalfUp(n * 10n ** BigInt(decimals), d), decimals);
/** 2772 -> "2 772" with narrow no-break spaces (pure.groupDigits). */
const groupDigits = (n) => n.toString().replace(/\B(?=(\d{3})+(?!\d))/g, NNBSP);

// ---- exact rule: spans (pure.decompose / blinkCount / durationPos without the epsilon) -------------------------

/**
 * Break a span of x = n/d Blinks into counts over the unit ladder, greedily from the largest unit that fits.
 * Up to `parts` NON-ZERO counts; with keepZeros the next `parts` consecutive units, zeros included.
 * A span shorter than a Blink gives one part, 0 nu.
 */
function decompose(n, d, parts, keepZeros) {
  let first = UNIT_BLINKS.findIndex((len) => len * d <= n);
  if (first < 0) first = NU;
  const out = [];
  let rest = n;
  for (let unit = first; unit <= NU && out.length < parts; unit++) {
    const count = rest / (UNIT_BLINKS[unit] * d);
    rest -= count * UNIT_BLINKS[unit] * d;
    if (count > 0n || keepZeros || out.length === 0) out.push({ unit, count });
  }
  return out;
}
/**
 * pure.blinkCount: a count of Blinks with a decimal part, "0", "<0.001", "0.14", "2.6", "4"; with the core's
 * one cut-off rule (fractonica.h, frc_format_span): "0" for exactly nothing, "<0.001" for every non-zero span
 * below 1/1000 Blink (the reference writes "0" below 1/10000).
 */
function blinkCount(n, d) {
  if (n === 0n) return '0';
  if (n * 1000n < d) return '<0.001';
  if (n < d) return significantText(n, d, 2);
  return fixedText(n, d, 1);
}
/** pure.durationPos: the text of a span of n/d Blinks. Led by nu and shorter than 10 Blinks it keeps a decimal. */
function spanText(n, d, negative, parts) {
  const cs = decompose(n, d, parts, false);
  const text = cs[0].unit === NU && n < 10n * d
    ? `${blinkCount(n, d)}ν`
    : cs.map(({ unit, count }) => `${count}${UNIT_SYMBOLS[unit]}`).join(' ');
  // the minus only before a text with a non-zero digit, and "<0.001" counts as none: "0ν" and "<0.001ν" are
  // unsigned (fractonica.h, frc_format_span; the reference writes "−<0.001ν")
  return negative && /[1-9]/.test(text) && !text.startsWith('<') ? MINUS + text : text;
}

/**
 * Every rounding of a span's text is decided at a multiple of 1/20000 Blink: the tie of the finest decimal
 * shown (0.00005) and with it every coarser tie and threshold (0.05, 0.005, 0.0005; 0.1, 0.01, 0.001, 0.0001).
 */
const ROUNDING_MARKS = 20_000n;
/**
 * exact rule: the fraction n/d of a Blink as frc_span_t.frac. It is the floor to 2^-32, stepped up by one
 * unit when a rounding mark lies above the floor and at or below the exact value: the stored fraction is then
 * on the exact value's side of every mark, so a text rounded from it is the text of the exact fraction.
 */
const spanFrac32 = (n, d) => {
  const floor = frac32(n, d);
  const mark = (n * ROUNDING_MARKS) / d; // the last mark at or below n/d
  return mark << 32n > floor * ROUNDING_MARKS ? floor + 1n : floor;
};

/**
 * exact rule: the span between two in-range positions as the core measures it (src/frc_span.c), in Blinks.
 *   1. Exactly: |pos2 - pos1| * P(12) quarter Blinks = quarters + rest / m, with m = den(from) * den(to).
 *   2. THE TIME GRAIN. An instant is a whole microsecond, and an instant the core returns is a boundary
 *      rounded UP to one. So the true span lies within one microsecond of position at `to` below the measured
 *      one and one at `from` above it; when a whole number of quarter Blinks is strictly inside that window,
 *      the span IS that number. (One microsecond at an instant is P(12) / g quarter Blinks, g being the
 *      length of the interval THAT MICROSECOND lies in: for an instant exactly on an eclipse it is the
 *      interval that ends there, see grainDen.) This is what the reference's 1e-12 Saros epsilon does, 570
 *      times finer. A span between two different instants is never brought to nothing.
 *   3. The fraction is cut to 32 bits by spanFrac32.
 */
function exactSpan(a, b) {
  const signed = (BigInt(b.ordinal) * b.den + b.num) * a.den - (BigInt(a.ordinal) * a.den + a.num) * b.den;
  const [from, to] = signed < 0n ? [b, a] : [a, b];
  const m = from.den * to.den;
  const exact = absBig(signed) * P12; // quarter Blinks, times m
  let quarters = exact / m;
  let rest = exact % m;
  // rest / m < P12 / grain(to): less than a microsecond at `to` past a whole quarter Blink
  if (rest * grainDen(to) < P12 * m) rest = 0n;
  // (m - rest) / m < P12 / grain(from): less than one at `from` short of the next
  else if ((m - rest) * grainDen(from) < P12 * m) [quarters, rest] = [quarters + 1n, 0n];
  const n = quarters * m + rest;
  const d = 4n * m;
  assert(n > 0n || signed === 0n, 'the grain brought a span between two instants to nothing');
  return { negative: signed < 0n ? 1 : 0, n, d, blinks: n / d, frac: spanFrac32(n % d, d), grain: n !== exact };
}
/** The length of the Saros interval that holds the microsecond ENDING at a position (an instant of the core). */
function grainDen(p) {
  return p.num === 0n && p.ordinal > 0 ? E_US[p.ordinal] - E_US[p.ordinal - 1] : p.den;
}

/** ordinal + num/den of an in-range instant given in MICROSECONDS (a boundary the core returns). */
function positionOfUs(tUs) {
  let k = EXTENT - 1;
  while (E_US[k] > tUs) k--;
  const num = tUs - E_US[k];
  const den = E_US[k + 1] - E_US[k];
  assert(num >= 0n && num < den, 'instant outside the catalogue, or its closing eclipse');
  return { ordinal: k, num, den };
}

const referenceDiffers = []; // lines of the comment block: where the reference's double text is not the exact text
let spanTextFromTruncated = 0;
let spansOnTheGrain = 0;

function buildSpan(label, t1Ms, t2Ms) {
  return buildSpanUs(label, t1Ms * 1000n, t2Ms * 1000n);
}

/** One row of the span table, for two instants in microseconds. */
function buildSpanUs(label, t1Us, t2Us) {
  const a = positionOfUs(t1Us);
  const b = positionOfUs(t2Us);
  const span = exactSpan(a, b);
  if (span.grain) spansOnTheGrain++;
  const row = {
    label, t1: t1Us, t2: t2Us,
    negative: span.negative, blinks: span.blinks, frac: span.frac,
    parts: decompose(span.n, span.d, 9, false),
    zparts: decompose(span.n, span.d, 9, true),
    text: [2, 3].map((parts) => spanText(span.n, span.d, span.negative, parts)),
  };
  // max_parts 2 and 3 give a PREFIX of the max_parts 9 list (the greedy loop only stops earlier).
  for (const keepZeros of [false, true]) {
    const full = keepZeros ? row.zparts : row.parts;
    for (const parts of [2, 3]) assert.deepEqual(decompose(span.n, span.d, parts, keepZeros), full.slice(0, parts));
  }
  row.n = [2, 3, 9].map((parts) => Math.min(parts, row.parts.length));
  row.nz = [2, 3, 9].map((parts) => Math.min(parts, row.zparts.length));
  // A core that words the span from frc_span_t alone sees a fraction cut to 32 bits: it must read the same.
  for (const parts of [2, 3]) {
    const cut = spanText((span.blinks << 32n) + span.frac, 1n << 32n, span.negative, parts);
    if (cut !== row.text[parts - 2]) spanTextFromTruncated++;
  }
  [2, 3].forEach((parts, i) => {
    const ref = pure.between(Number(t1Us) / 1e6, Number(t2Us) / 1e6, { parts });
    if (ref !== row.text[i]) referenceDiffers.push(`span ${label} (${row.t1} -> ${row.t2}), ${parts} parts: exact "${row.text[i]}", reference "${ref}"`);
  });
  return row;
}

function buildSpans(samples) {
  const rows = [];
  const seen = new Set();
  const inside = (ms) => ms >= E_MS[0] && ms <= E_MS[EXTENT] - 1n;
  const add = (label, t1, t2) => {
    const key = `${t1}:${t2}`;
    if (seen.has(key) || !inside(t1) || !inside(t2)) return;
    seen.add(key);
    rows.push(buildSpan(label, t1, t2));
  };
  const inRange = samples.filter((s) => s.inRange);
  const byTime = [...inRange].sort((x, y) => (x.tMs < y.tMs ? -1 : x.tMs > y.tMs ? 1 : 0));
  // 0. The zero span, and the span of SPEC S15.6: from vector A to the end of its Helion, 3372.428 s later.
  add('zero', A_MS, A_MS);
  add('S15.6', A_MS, A_MS + 3_372_428n);
  // 1. Whole Saros intervals: exact unit counts (one Saros is 1b 3g 1d 1e 19h 9i; four are one Alpha).
  add('1 Saros', E_MS[22], E_MS[23]);
  add('1 Saros keeps zeros', E_MS[0], E_MS[1]);
  add('1 Alpha', E_MS[20], E_MS[24]);
  add('-1 Alpha', E_MS[24], E_MS[20]);
  add('2 Alphas', E_MS[4], E_MS[12]);
  add('whole catalogue', E_MS[0], E_MS[EXTENT] - 1n);
  add('whole catalogue back', E_MS[EXTENT] - 1n, E_MS[0]);
  // 2. Neighbouring samples in time, every other one reversed (a negative span). Four apart, because an
  //    eclipse and the milliseconds beside it are three samples in a row.
  for (let i = 0; i + 4 < byTime.length; i += 7) {
    const [x, y] = [byTime[i].tMs, byTime[i + 4].tMs];
    if ((i / 7) % 2 === 0) add('neighbours', x, y);
    else add('neighbours back', y, x);
  }
  // 3. Pairs milliseconds apart: sub-Blink decimals, "<0.001" and 0.
  const near = [1n, 2n, 3n, 7n, 31n, 50n, 100n, 250n, 300n, 310n, 311n, 312n, 400n, 800n, 1234n, 2000n, 2950n, 3095n];
  for (let i = 0; i < 54; i++) {
    const t = inRange[4 + ((i * 37) % (inRange.length - 4))].tMs;
    const d = near[i % near.length];
    if (i % 5 === 4) add(`-${d} ms`, t + d, t);
    else add(`${d} ms`, t, t + d);
  }
  //    One pair where the reference's double position rounds the decimal the other way (0.00965 exactly).
  add('3 ms back to E[11]', E_MS[11] + 3n, E_MS[11]);
  // 4. Pairs a whole count of Blinks apart, to the millisecond below and above it: the unit boundaries
  //    (10 Blinks ends the decimal, 25 are a Flavon, 1625 a Preon, 58500 a Helion).
  const counts = [1n, 2n, 3n, 9n, 10n, 11n, 24n, 25n, 26n, 50n, 1624n, 1625n, 1626n, 58_500n, 4_504_500n];
  for (let i = 0; i < 60; i++) {
    const s = inRange[(i * 53) % inRange.length];
    const blinks = counts[i % counts.length];
    // one Blink in this interval is den * 4 / P(12) microseconds
    const below = (blinks * s.pos.den * 4n) / P12 / 1000n; // whole milliseconds, just short of `blinks`
    // (when the pair runs over an eclipse the next interval's Blink differs a little: "near", not "exactly")
    add(`near ${blinks} Blinks, the ms below`, s.tMs, s.tMs + below);
    add(`near ${blinks} Blinks, the ms above`, s.tMs, s.tMs + below + 1n);
    if (i % 4 === 0) add(`near ${blinks} Blinks, back`, s.tMs + below + 1n, s.tMs);
  }
  // 5. Pairs spanning an eclipse: the two intervals have different lengths, so only position measures them.
  for (let k = 1; k < EXTENT; k += 3) {
    add(`across E[${k}] 2 ms`, E_MS[k] - 1n, E_MS[k] + 1n);
    add(`across E[${k}] 12 s`, E_MS[k] - 5000n, E_MS[k] + 7000n);
    add(`across E[${k}] back`, E_MS[k] + 86_400_000n, E_MS[k] - 86_400_000n * 3n);
    add(`up to E[${k}]`, E_MS[k] - 250n, E_MS[k]);
  }
  // 6. Random pairs anywhere, half of them reversed.
  for (let i = 0; i < 40; i++) {
    const x = randomBetween(E_MS[0], E_MS[EXTENT] - 2n);
    const y = randomBetween(E_MS[0], E_MS[EXTENT] - 2n);
    add('random', x, y);
  }
  // 7. THE TIME GRAIN: between two boundaries as the core returns them, each the first MICROSECOND at or
  //    after its exact instant. Measured exactly such a span is up to a microsecond off the whole count
  //    ("64λ 24ν" for one Preon); with the grain it is the count. From the cycles of oracle_clock.h, so no
  //    random number is drawn here: start -> end is one cycle, peak -> valley half of one, start -> node too.
  //    Levels 0..12 lie on the quarter-Blink grid; level 13 (a cycle of 1/7 Blink) does not and is measured
  //    as it is.
  const addUs = (label, t1, t2) => {
    const key = `us ${t1}:${t2}`;
    if (seen.has(key) || t1 < FIRST_US || t2 < FIRST_US || t1 >= LAST_US || t2 >= LAST_US) return;
    seen.add(key);
    rows.push(buildSpanUs(label, t1, t2));
  };
  cycleTable.forEach((c, i) => {
    if (c.level > 13 || c.clipped) return;
    const [start, peak, node, valley, end] = c.points;
    if (i % 3 === 2) addUs(`level ${c.level} cycle ${c.cycle}, end back to start`, end, start);
    else addUs(`level ${c.level} cycle ${c.cycle}, start to end`, start, end);
    addUs(`level ${c.level} cycle ${c.cycle}, peak to valley`, peak, valley);
    if (i % 2 === 0) addUs(`level ${c.level} cycle ${c.cycle}, start to node`, start, node);
  });
  return rows;
}

// ---- cycle lengths, cycle names -----------------------------------------------------------------------------

function buildCycleLengths() {
  const rows = [];
  for (let level = 0; level <= 20; level++) {
    const n = P12;
    const d = prefix(level); // one cycle is P(12) / P(level) Blinks
    const text = spanText(n, d, 0, 2);
    const ref = pure.cycleLength(level);
    if (ref !== text) referenceDiffers.push(`cycle length of level ${level}: exact "${text}", reference "${ref}"`);
    rows.push({ level, blinks: n / d, frac: spanFrac32(n % d, d), text });
  }
  return rows;
}

/**
 * exact rule: where cycle `c` of `level` starts on the minor ladder (pure.countsAtStart), and its names
 * (pure.cycleName / cycleShort). The reference rounds a sub-Blink start to 2 decimals in doubles; here the
 * same rounding is exact, a half going up.
 */
function cycleNamesExact(level, c) {
  const ordinal = level === 0 ? c + 1n : (c % siblingsOf(level)) + 1n;
  if (level <= 3) {
    const word = ['Alpha', 'Beta', 'Gamma', 'Delta'][level];
    return { name: `${word} ${pure.ordinalName(Number(ordinal))}`, nameShort: `${word} ${pure.ordinalName(Number(ordinal))}`, short: `${UNIT_SYMBOLS[level]}#${ordinal}`, carried: 0 };
  }
  const at = (L) => (L <= level ? c / (prefix(level) / prefix(L)) : c * (prefix(L) / prefix(level)));
  const local = (L, parent) => at(L) % (prefix(L) / prefix(parent));
  const epsilon = `ε#${local(4, 3) + 1n}`;
  if (level === 4) return { name: epsilon, nameShort: epsilon, short: epsilon, carried: 0 };
  const parts = [];
  let carried = 0;
  if (level >= 5) parts.push(`${local(6, 4) + 1n}η`);
  if (level >= 7) parts.push(`${local(8, 6)}ι`);
  if (level >= 9) parts.push(`${local(10, 8)}λ`);
  if (level >= 11) {
    const nu = local(12, 10);
    if (level <= 12) parts.push(`${nu}ν`);
    else {
      const ratio = prefix(level) / P12; // cycles of this level in one Blink
      const hundredths = roundHalfUp((nu * ratio + (c % ratio)) * 100n, ratio);
      carried = c % ratio !== 0n && hundredths >= (nu + 1n) * 100n ? 1 : 0; // ".997" reads as the next whole Blink
      parts.push(`${decimalText(hundredths, 2)}ν`);
    }
  }
  return { name: [epsilon, ...parts].join(' '), nameShort: parts.join(' '), short: parts.at(-1), carried };
}

function buildCycleNames(samples) {
  const rows = [];
  const seen = new Set();
  const add = (level, cycle) => {
    const key = `${level}:${cycle}`;
    if (seen.has(key)) return;
    seen.add(key);
    const exact = cycleNamesExact(level, cycle);
    const ref = { name: pure.cycleName(level, cycle), nameShort: pure.cycleName(level, cycle, { short: true }), short: pure.cycleShort(level, cycle) };
    for (const key2 of ['name', 'nameShort', 'short']) {
      if (ref[key2] !== exact[key2]) referenceDiffers.push(`cycle name ${key2} of level ${level} cycle ${cycle}: exact "${exact[key2]}", reference "${ref[key2]}"`);
    }
    rows.push({ level, cycle, ...exact });
  };
  const inRange = samples.filter((s) => s.inRange);
  // The cycle running at a sample, 17 levels over a rotating choice of samples (the S12 vectors first).
  for (let i = 0; i < 136; i++) {
    const s = inRange[i < 4 ? i : (i * 29) % inRange.length];
    const level = i % 17;
    add(level, (s.pos.p.numerator * prefix(level)) / (4n * s.pos.p.denominator));
  }
  // The first cycle of every level, and the examples of pure.js: zeta cycles start on Helions 1, 12, 23 ...
  for (let level = 0; level <= 16; level++) add(level, 0n);
  for (let j = 0; j < 7; j++) add(5, 700n + BigInt(j));
  // Sub-Blink starts whose 2-decimal rounding carries into the next whole Blink (rows with carried = 1).
  for (const level of [15, 16]) {
    const ratio = prefix(level) / P12;
    add(level, 5n * ratio - 1n); //            "... 5ν" for a start inside Blink 4
    add(level, 25n * ratio - 1n); //           "... 25ν": inside Blink 24, the last of its Flavon
    add(level, 1_000_003n * ratio + ratio / 2n);
  }
  return rows;
}

// ---- ordinals, rates, per-Blink -----------------------------------------------------------------------------

function buildOrdinals() {
  return [...Array.from({ length: 22 }, (_, i) => i + 1), 25].map((n) => ({ n, text: pure.ordinalName(n) }));
}

function buildRates() {
  return pure.RATES.map((rate, i) => {
    let unit = NU;
    let count = 0n; // real time: no unit count
    let factor = 1n;
    if (i > 0) {
      const m = rate.label.match(/^(\d*)(.) per Blink$/u);
      assert(m, `rate label ${rate.label}`);
      unit = UNIT_SYMBOLS.indexOf(m[2]);
      count = BigInt(m[1] || '1');
      assert(unit >= 0 && rate.id.startsWith(UNIT_IDS[unit]), `rate ${rate.id} names another unit`);
      factor = count * UNIT_BLINKS[unit]; // displayed time per real time, an exact integer
    }
    assert(Math.abs(rate.rate - Number(factor)) / Number(factor) < 1e-11, `rate ${rate.id} is not ${factor}`);
    return { id: rate.id, unit, count, factor, label: rate.label };
  });
}

/**
 * exact rule: pure.pbText for the rational num/den cycles per Blink (no 1e-9 epsilon; halves go up), with
 * design ruling 3 first: below 1 pB with a whole reciprocal it is "1 per N Blinks" (the reference writes
 * "0.04 pB" for the Flavon and "0.2 pB" for mu; those rows are listed as differing from the reference).
 */
function perBlinkText(num, den) {
  if (num === 0n || den === 0n) return DASH;
  if (num < den && den % num === 0n) return `1 per ${groupDigits(den / num)} Blinks`;
  if (num * 25n < den) return `1 per ${groupDigits(roundHalfUp(den, num))} Blinks`; // slower than 1 per 25 Blinks
  let value;
  if (2n * num >= 1999n * den) value = groupDigits(roundHalfUp(num, den)); //   from 999.5: a whole number, grouped
  else if (20n * num >= 1999n * den) value = fixedText(num, den, 1); //         from 99.95: one decimal
  else value = significantText(num, den, 3); //                                  else three significant digits
  return `${value} pB`;
}

function buildPerBlink() {
  const rows = [];
  const add = (level, num, den, note) => {
    const text = perBlinkText(num, den);
    const ref = pure.pbText(Number(num) / Number(den));
    if (ref !== text) referenceDiffers.push(`pB of ${num}/${den}${note ? ` (${note})` : ''}: exact "${text}", reference "${ref}"`);
    rows.push({ level, num, den, text, note });
  };
  // The true rate of a level: P(level) / P(12) cycles per Blink, reduced (frc_level_per_blink).
  for (let level = 0; level <= 24; level++) {
    const g = gcd(prefix(level), P12);
    add(level, prefix(level) / g, P12 / g, `level ${level}`);
  }
  // Hand-picked rationals, one or more for every branch of pbText. level = -1 marks them.
  const mean = { num: 250_529_296_875n, den: 77_894_856_196n }; // f(12) of the mean interval in Hz (SPEC S16.5)
  const tone = { num: 110n * mean.den, den: mean.num }; //          a 110 Hz tone in pB (SPEC S15.5: 34.2 pB)
  const g = gcd(tone.num, tone.den);
  [
    [0n, 1n, 'nothing: the dash'],
    [tone.num / g, tone.den / g, 'a 110 Hz tone, SPEC S15.5'],
    [342n, 10n, 'not reduced'],
    [1n, 2n, 'below one, a whole reciprocal: 1 per 2 Blinks'],
    [7n, 3n, 'three significant digits'],
    [57n, 1250n, '0.0456'],
    [1n, 25n, 'exactly 1 per 25 Blinks: a whole reciprocal (ruling 3)'],
    [2n, 50n, 'the same, not reduced'],
    [1n, 26n, 'just slower than 1 per 25'],
    [3n, 1000n, '1 per 333.3'],
    [2n, 53n, '1 per 26.5: the half goes up'],
    [1n, 58_500n, 'the Helion'],
    [9994n, 100n, '99.94: still three significant digits'],
    [1999n, 20n, '99.95: one decimal from here, and it rounds to 100'],
    [12_345n, 100n, '123.45: the half goes up'],
    [9994n, 10n, '999.4: the last value with a decimal'],
    [1999n, 2n, '999.5: whole and grouped from here'],
    [2772n, 1n, 'rho'],
    [1_234_567n, 1n, 'two groups'],
    [12_345_678_901n, 10n, 'rounds to 1 234 567 890'],
  ].forEach(([num, den, note]) => add(-1, num, den, note));
  return rows;
}

// ---------------------------------------------------------------------------------------------------------
// Build everything
// ---------------------------------------------------------------------------------------------------------

const samples = buildSamples();
samples.forEach(readSample);
const inRangeSamples = samples.filter((s) => s.inRange);
inRangeSamples.forEach((s) => {
  s.date = readDate(s);
});

// ~60 samples for the deep table: the S12 vectors, then every 9th in-range sample.
const deepSamples = inRangeSamples.filter((s, i) => i < 4 || i % 9 === 0);
deepSamples.forEach((s) => {
  s.deep = readDeep(s);
});

const cycleTable = buildCycleTable();

const FIRST_WHOLE_SYNC = Number(ceilDiv(-ANCHOR_CYCLE, SYNC_CYCLES)); // the first sync that starts at or after position 0
const LAST_WHOLE_SYNC = Number(floorDiv(BigInt(EXTENT) * prefix(SYNC_LEVEL) - 4n * ANCHOR_CYCLE, 4n * SYNC_CYCLES)) - 1;
const syncNumbers = [
  0, 1, -1, 4820, 2375, 7987, // the anchor and the S12 vectors
  -2380, -2379, 8999, 9000, // around the UTC era
  FIRST_WHOLE_SYNC, FIRST_WHOLE_SYNC + 1, LAST_WHOLE_SYNC - 1, LAST_WHOLE_SYNC,
  FIRST_WHOLE_SYNC - 1, LAST_WHOLE_SYNC + 1, // the partial syncs at the edges: out of range
  FIRST_WHOLE_SYNC - 2, LAST_WHOLE_SYNC + 2, -1_000_000, 1_000_000, // wholly outside
];
for (let i = 0; i < 20; i++) syncNumbers.push(FIRST_WHOLE_SYNC + Number(randomBelow(BigInt(LAST_WHOLE_SYNC - FIRST_WHOLE_SYNC + 1))));
const syncTable = syncNumbers.map((number) => ({ number, sync: syncByNumber(number) }));
assert(syncByNumber(FIRST_WHOLE_SYNC) && !syncByNumber(FIRST_WHOLE_SYNC - 1), 'first whole sync');
assert(syncByNumber(LAST_WHOLE_SYNC) && !syncByNumber(LAST_WHOLE_SYNC + 1), 'last whole sync');

const pentadSyncs = [
  0, 4820, 2375, 7987,
  FIRST_WHOLE_SYNC - 2, FIRST_WHOLE_SYNC - 1, FIRST_WHOLE_SYNC, // before, the cut first sync, the first whole one
  LAST_WHOLE_SYNC, LAST_WHOLE_SYNC + 1, LAST_WHOLE_SYNC + 2, // the last whole one, the cut last sync, after
];
for (let i = 0; i < 6; i++) pentadSyncs.push(FIRST_WHOLE_SYNC + Number(randomBelow(BigInt(LAST_WHOLE_SYNC - FIRST_WHOLE_SYNC + 1))));
const pentadTable = pentadSyncs.flatMap((sync) => [0, 1, 2, 3].map((index) => ({ sync, index, pentad: pentadByNumber(sync, index) })));

const cycleCounts = Array.from({ length: CLOCK_MAX_LEVEL + 1 }, (_, level) => clock.count(level));

const spans = buildSpans(samples);
assert.equal(spanTextFromTruncated, 0, 'a span text rounded from the 32-bit fraction differs from the exact text');
const cycleLengths = buildCycleLengths();
const cycleNames = buildCycleNames(samples);
const ordinals = buildOrdinals();
const rates = buildRates();
const perBlink = buildPerBlink();

const sampleIndex = new Map(samples.map((s, i) => [s.label, i]));
const S15_6 = ['A', 'B', 'C', 'anchor+1s'].map((label) => {
  const s = samples[sampleIndex.get(label)];
  return {
    label, t: s.tUs, clockIndex: sampleIndex.get(label),
    full: pure.dateText(s.tSec, { form: 'full', upTo: 'nu' }),
    short: pure.dateText(s.tSec, { form: 'short', upTo: 'iota' }),
    pair: s.levels[6].odometer.digits,
  };
});

// ---------------------------------------------------------------------------------------------------------
// Validation: every number of SPEC S12 and S15.6, read from the data that is about to be written
// ---------------------------------------------------------------------------------------------------------

/** Microseconds rounded to the milliseconds SPEC prints, as "seconds.mmm". */
const toMs = (us) => {
  const ms = floorDiv(2n * us + 1000n, 2000n);
  return `${ms / 1000n}.${(ms % 1000n).toString().padStart(3, '0')}`;
};
const toIso = (us) => new Date(Number(floorDiv(2n * us + 1000n, 2000n))).toISOString();
/** Signed microseconds as seconds with 4 decimals, like SPEC's drift. */
const toSeconds4 = (us) => {
  const n = floorDiv(2n * absBig(us) + 100n, 200n);
  return `${us < 0n ? '-' : ''}${n / 10_000n}.${(n % 10_000n).toString().padStart(4, '0')}`;
};
const positionText = (pos) => `${pos.ordinal}.${roundHalfUp(pos.num * 10n ** 12n, pos.den).toString().padStart(12, '0')}`;
const phaseNear = (phase, expected) => assert(Math.abs(Number(phase) / 2 ** 32 - expected) < 7.5e-10, `phase ${Number(phase) / 2 ** 32} is not ${expected}`);
const glyphFacts = (g) => [g.index, g.value, g.derivative, g.dotted];

function validateSpec() {
  const [a, b, c, anchor] = samples;
  assert.deepEqual([a.label, b.label, c.label, anchor.label], ['A', 'B', 'C', 'anchor+1s']);
  const level = (s, L) => s.levels[L];
  const counter = (s, L) => [level(s, L).cycle, Number(level(s, L).local), Number(level(s, L).siblings)];
  const threshold = (s, L) => [QUARTER_NAMES[level(s, L).nextName], toMs(level(s, L).nextTime)];

  // ---- S12 vector A
  assert.equal(a.tUs, 1_791_460_800_000_000n);
  assert.equal(toIso(a.tUs), '2026-10-08T12:00:00.000Z');
  assert.equal(positionText(a.pos), '22.927850216635');
  assert.deepEqual(glyphFacts(level(a, 6).glyphs[0]), [13, -3, 1, 0]);
  assert.deepEqual(glyphFacts(level(a, 7).glyphs[0]), [5, 3, -1, 1]);
  assert.deepEqual(level(a, 6).odometer.digits, [13, 0]);
  assert.deepEqual(level(a, 7).odometer.digits, [5, 4]);
  assert.deepEqual(counter(a, 6), [717_211n, 0, 11]);
  assert.deepEqual(counter(a, 7), [6_454_906n, 7, 9]);
  assert.deepEqual(threshold(a, 6), ['end', '1791464172.428']);
  assert.equal(toIso(level(a, 6).nextTime), '2026-10-08T12:56:12.428Z');
  assert.deepEqual(threshold(a, 7), ['node', '1791461140.937']);
  assert.equal(toIso(level(a, 7).nextTime), '2026-10-08T12:05:40.937Z');
  assert.deepEqual([level(a, 6).glyphs[1].index, level(a, 6).glyphs[1].value], [42, -10]);
  assert.equal(a.sync.number, 4820);
  assert.equal(toMs(a.sync.start), '1791336849.826');
  assert.equal(toIso(a.sync.start), '2026-10-07T01:34:09.826Z');
  assert.equal(toIso(a.sync.end), '2026-10-11T01:33:59.745Z');
  phaseNear(a.sync.phase, 0.358662585);
  assert.equal(toSeconds4(a.sync.drift), '-48469.9899');
  assert.deepEqual(a.sync.digitsS2D3, [5, 11, 13]);
  assert.deepEqual(a.sync.digitsS3D2, [18, 33]);
  assert.deepEqual([QUARTER_NAMES[a.sync.nextName], toMs(a.sync.nextTime)], ['node', '1791509644.786']);
  assert.equal(toIso(a.sync.nextTime), '2026-10-09T01:34:04.786Z');

  // ---- S12 vector B
  assert.equal(b.tUs, 946_684_800_000_000n);
  assert.equal(toIso(b.tUs), '2000-01-01T00:00:00.000Z');
  assert.equal(positionText(b.pos), '21.443111640256');
  assert.deepEqual(glyphFacts(level(b, 6).glyphs[0]), [5, 3, -1, 1]);
  assert.deepEqual(level(b, 6).odometer.digits, [5, 6]);
  assert.deepEqual(counter(b, 6).slice(1), [9, 11]);
  assert.deepEqual(threshold(b, 6), ['node', '946687783.046']);
  assert.equal(toIso(level(b, 6).nextTime), '2000-01-01T00:49:43.046Z');
  assert.equal(level(b, 7).glyphs[0].index, 0);
  assert.deepEqual(level(b, 7).odometer.digits, [0, 6]);
  assert.deepEqual(counter(b, 7).slice(1), [3, 9]);
  assert.deepEqual(threshold(b, 7), ['peak', '946685256.803']);
  assert.equal(toIso(level(b, 7).nextTime), '2000-01-01T00:07:36.803Z');
  assert.equal(b.sync.number, 2375);
  assert.equal(toMs(b.sync.start), '946369476.520');
  assert.equal(toIso(b.sync.start), '1999-12-28T08:24:36.520Z');
  phaseNear(b.sync.phase, 0.912420883);
  assert.equal(toSeconds4(b.sync.drift), '-23843.2957');
  assert.deepEqual(b.sync.digitsS2D3, [14, 9, 9]);
  assert.deepEqual([QUARTER_NAMES[b.sync.nextName], toMs(b.sync.nextTime)], ['end', '946715066.462']);

  // ---- S12 vector C
  assert.equal(c.tUs, 2_886_085_800_000_000n);
  assert.equal(toIso(c.tUs), '2061-06-15T18:30:00.000Z');
  assert.equal(positionText(c.pos), '24.851712757369');
  assert.deepEqual(glyphFacts(level(c, 6).glyphs[0]), [10, -2, -1, 1]);
  assert.deepEqual(level(c, 6).odometer.digits, [10, 3]);
  assert.deepEqual(counter(c, 6).slice(1), [0, 11]);
  assert.deepEqual(threshold(c, 6), ['valley', '2886087806.395']);
  assert.equal(toIso(level(c, 6).nextTime), '2061-06-15T19:03:26.395Z');
  assert.deepEqual(glyphFacts(level(c, 7).glyphs[0]), [12, -4, 0, 0]); // the valley itself: bare
  assert.deepEqual(level(c, 7).odometer.digits, [12, 1]);
  assert.deepEqual(counter(c, 7).slice(1), [5, 9]);
  assert.deepEqual(threshold(c, 7), ['end', '2886086290.651']);
  assert.equal(toIso(level(c, 7).nextTime), '2061-06-15T18:38:10.651Z');
  assert.equal(c.sync.number, 7987);
  assert.equal(toIso(c.sync.start), '2061-06-12T16:31:59.691Z');
  phaseNear(c.sync.phase, 0.770510076);
  assert.equal(toSeconds4(c.sync.drift), '-81000.1249');
  assert.deepEqual(c.sync.digitsS2D3, [12, 5, 4]);
  assert.deepEqual([QUARTER_NAMES[c.sync.nextName], toMs(c.sync.nextTime)], ['end', '2886165109.343']);

  // ---- S12 anchor: one second after the anchor eclipse
  assert.equal(toMs(anchor.tUs), '125593320.816');
  assert.equal(toMs(E_US[ANCHOR]), '125593319.816');
  assert.equal(toIso(E_US[ANCHOR]), '1973-12-24T15:01:59.816Z');
  assert.equal(anchor.sync.number, 0);
  assert.equal(anchor.sync.drift, 0n);
  assert.deepEqual(anchor.sync.digitsS2D3, [0, 0, 0]);

  // ---- S7.5: A is in Pentad II, its 2nd Helion, of sync 4820; the anchor eclipse starts Pentad I of sync 0
  assert.deepEqual([a.pentad.sync, a.pentad.index, a.pentad.helion], [4820, 1, 2]);
  const eclipse = samples[sampleIndex.get(`E[${ANCHOR}]`)];
  assert.deepEqual([eclipse.pentad.sync, eclipse.pentad.index, eclipse.pentad.helion, eclipse.pentad.start], [0, 0, 1, E_US[ANCHOR]]);

  // ---- S15.6
  const rows = [
    ['Alpha Hexafracton Valley, Beta Tetrafracton Valley, Gamma Nonafracton Valley, Delta Trifracton End, ε#5 34η 29ι 21λ 3ν', 'Delta Trifracton End ε#5 34η 29ι', [13, 0]],
    ['Alpha Hexafracton Node, Beta Difracton End, Gamma Didecafracton Node, Delta Trifracton Node, ε#2 21η 12ι 6λ 5ν', 'Delta Trifracton Node ε#2 21η 12ι', [5, 6]],
    ['Alpha Heptafracton Peak, Beta Difracton Peak, Gamma Monofracton End, Delta Pentafracton Peak, ε#2 1η 23ι 1λ 21ν', 'Delta Pentafracton Peak ε#2 1η 23ι', [10, 3]],
    ['Alpha Hexafracton Peak, Beta Monofracton Peak, Gamma Monofracton Peak, Delta Monofracton Peak, ε#1 1η 0ι 0λ 3ν', 'Delta Monofracton Peak ε#1 1η 0ι', [0, 0]],
  ];
  rows.forEach(([full, short, pair], i) => {
    assert.equal(S15_6[i].full, full);
    assert.equal(S15_6[i].short, short);
    assert.deepEqual(S15_6[i].pair, pair);
    // and the same texts sit in the per-sample table
    assert.equal(samples[i].date.text[5][0], full);
    assert.equal(samples[i].date.text[3][1], short);
  });
  assert.equal(a.date.stamp, 'α#6 β#4 γ#9 δ#3 ε#5 34η 29ι 21λ 3ν');
  // "At A the depth-7 end is 3,372.428 s away, which is the span 6ι 43λ."
  assert.equal(toMs(level(a, 6).nextTime - a.tUs), '3372.428');
  const span = spans.find((s) => s.label === 'S15.6');
  assert.deepEqual([span.t2 - span.t1, span.text[0]], [3_372_428_000n, '6ι 43λ']);

  // ---- S3.3 and S15.4 / S15.5 in passing: the unit ladder and the whole-number rates
  assert.deepEqual(UNIT_BLINKS, [7_319_812_500n, 1_463_962_500n, 112_612_500n, 22_522_500n, 4_504_500n, 58_500n, 1625n, 25n, 1n]);
  assert.deepEqual([5, 7, 9, 11, 13].map((L) => cycleLengths[L].text), ['11η', '4ι', '13λ', '5ν', '0.14ν']);
  assert.deepEqual(rates.map((r) => r.factor), [1n, 5n, 25n, 325n, 1625n, 6500n, 58_500n, 643_500n, 4_504_500n, 22_522_500n, 112_612_500n]);
  assert.deepEqual([13, 14, 15, 16].map((L) => perBlink[L].text), ['7 pB', '77 pB', '693 pB', `2${NNBSP}772 pB`]);
  assert.equal(perBlink.find((r) => r.note === 'a 110 Hz tone, SPEC S15.5').text, '34.2 pB');

  // ---- S14, when SPEC.md is at hand: the catalogue the reference runs on is the one SPEC prints
  // the repository's own copy (docs/SPEC.md), else the SPEC.md beside the core
  const spec = [path.resolve(CORE, 'docs', 'SPEC.md'), path.resolve(CORE, '..', 'SPEC.md')].find((f) => fs.existsSync(f))
    ?? path.resolve(CORE, 'docs', 'SPEC.md');
  if (fs.existsSync(spec)) {
    const section = fs.readFileSync(spec, 'utf8').split('## 14. Reference data')[1]?.split('```')[1];
    const printed = (section?.match(/-?\d+\.\d{3}/g) ?? []).map((s) => BigInt(s.replace('.', '')));
    assert.deepEqual(printed, E_MS, 'the reference catalogue is not the table of SPEC S14');
    return true;
  }
  return false;
}
const specCatalogueChecked = validateSpec();

// ---------------------------------------------------------------------------------------------------------
// Writing C
// ---------------------------------------------------------------------------------------------------------

const us = (v) => (v === null ? 'ORACLE_CLOCK_NONE' : v.toString());
const list = (items) => `{${items.join(',')}}`;
/** A C string literal in raw UTF-8. A hex-looking character never follows an escape, because none is written. */
function cString(text) {
  assert(!/[\u0000-\u001f]/.test(text), 'control character in a string');
  return `"${text.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\?\?/g, '?\\?')}"`;
}
const commentSafe = (text) => text.replace(/\*\//g, '* /');

const PREAMBLE = (file, what) => `/*
 * ${file} - GENERATED by tools/oracle/generate-clock.mjs. DO NOT EDIT: regenerate.
 *
 * ${what}
 * Reference: fractonica.js ${F.version} (frozen JavaScript oracle), Saros ${SERIES}, Unix clock, sync anchor ${ANCHOR}.
 * Layouts and conventions are documented in tools/oracle/README.md and repeated at each typedef below.
 * This header is self-contained C99: it needs <stdint.h> only and none of the fractonica.h types.
 * Strings are raw UTF-8.
 */`;

/** Tables are `static const` and tagged unused, so a test may read only some of them without a warning. */
const DATA_MACRO = (name) => `#if defined(__GNUC__) || defined(__clang__)
#  define ${name} __attribute__((unused)) static const
#else
#  define ${name} static const
#endif`;

// ---- oracle_clock.h -------------------------------------------------------------------------------------------

function glyphC(g) {
  const trits = [...g.trits, 0, 0, 0].slice(0, 3);
  return list([g.value, g.derivative, g.dotted, list(trits)]);
}
function levelC(l) {
  return list([
    l.cycle, l.local, l.siblings, l.phase, l.quarter, l.clipped, us(l.start), us(l.end),
    list(l.glyphs.map((g) => g.index)), glyphC(l.glyphs[0]), glyphC(l.glyphs[1]),
    list(l.odometer.digits), us(l.odometer.next), l.nextName, us(l.nextTime),
  ]);
}
const EMPTY_LEVEL = '{0,0,0,0,0,0,0,0,{0,0,0},{0,0,0,{0,0,0}},{0,0,0,{0,0,0}},{0,0},0,0,0}';
const EMPTY_SYNC = '{0,0,0,0,0,0,0,0,0,0,{0,0,0},{0,0}}';
const EMPTY_PENTAD = '{0,0,0,0,0,0,0}';

function syncC(s) {
  if (!s) return EMPTY_SYNC;
  return list([1, s.number, us(s.start), us(s.end), us(s.nominal), s.drift, s.phase, s.quarter, s.nextName, us(s.nextTime), list(s.digitsS2D3), list(s.digitsS3D2)]);
}
function pentadC(p) {
  if (!p) return EMPTY_PENTAD;
  return list([1, p.sync, p.index, p.helion, p.helions, us(p.start), us(p.end)]);
}
function sampleC(s, i) {
  const head = `  /* [${i}] ${commentSafe(s.label)} */\n  {${cString(s.label)},${s.tUs},`;
  if (!s.inRange) {
    return `${head}0,0,0,0,\n   {${Array(MODEL_MAX_LEVEL + 1).fill(EMPTY_LEVEL).join(',\n    ')}},\n   {0,0},0,{0,0,0},0,\n   ${EMPTY_SYNC},${EMPTY_PENTAD}}`;
  }
  return `${head}1,${s.pos.ordinal},${s.pos.num},${s.pos.den},\n   {${s.levels.map(levelC).join(',\n    ')}},\n` +
    `   ${list(s.odo6S3D2.digits)},${us(s.odo6S3D2.next)},${list(s.odo6S2D3.digits)},${us(s.odo6S2D3.next)},\n` +
    `   ${syncC(s.sync)},${pentadC(s.pentad)}}`;
}
function deepC(s) {
  const rows = s.deep.map((l) => list([l.cycle, l.local, l.siblings, l.phase, l.quarter, us(l.start), us(l.end), l.glyphs[0].index]));
  return `  /* ${commentSafe(s.label)} */\n  {${s.tUs},${samples.indexOf(s)},\n   {${rows.join(',\n    ')}}}`;
}

function clockHeader() {
  const closing = sampleIndex.get(`E[${EXTENT}]-1ms`);
  const partialFirst = samples.findIndex((s) => s.inRange && !s.sync && s.tUs < E_US[1]);
  const partialLast = samples.findIndex((s) => s.inRange && !s.sync && s.tUs > E_US[EXTENT - 1]);
  assert(closing >= 0 && partialFirst >= 0 && partialLast >= 0);
  return `${PREAMBLE('oracle_clock.h', 'What a correct clock reads (SPEC S2-S7): position, cycles, glyphs, odometers, thresholds, solar sync, Pentads.')}
/*
 * CONVENTIONS (they are the core's: see include/fractonica.h)
 *   *_us       int64 Unix MICROSECONDS. Every sample instant is a whole millisecond.
 *   boundary   the first microsecond AT OR AFTER the exact rational instant (ceiling).
 *   ORACLE_CLOCK_NONE (INT64_MIN, the core's FRC_TIME_NONE): the boundary falls beyond the catalogue.
 *   phase      floor(exact fraction * 2^32). A cycle's phase is linear in POSITION. The sync's phase is linear
 *              in TIME and uses the EXACT rational start and end, not the ceiled microseconds:
 *              ${syncPhaseRoundedDiffers.length} of the ${inRangeSamples.filter((s) => s.sync).length} sync readings here would differ in the last bit if it were taken from start_us and end_us
 *              (samples ${syncPhaseRoundedDiffers.map((label) => sampleIndex.get(label)).join(', ')}).
 *   digits     odometer digits are floor(exact fraction * q^k). The sync's 3 x 2-stroke digits are the top 12 bits
 *              of its phase (16^3 divides 2^32); its 2 x 3-stroke digits are NOT a function of the 32-bit phase
 *              in general (52^2 does not divide 2^32; ${syncDigitsFromPhase32Differ} readings here would differ): take them from the
 *              exact fraction.
 *   names      0 peak, 1 node, 2 valley, 3 end (frc_quarter_t).
 *   trits      least significant first.
 *   closing    the catalogue's closing instant E[${EXTENT}] is not sampled: the core reads it one microsecond inside
 *              the catalogue, the reference one millisecond inside. They agree at E[${EXTENT}] - 1 ms, which is
 *              ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLE_CLOSING_MINUS_1MS].
 */
#ifndef ORACLE_CLOCK_H
#define ORACLE_CLOCK_H

#include <stdint.h>

#define ORACLE_CLOCK_NONE INT64_MIN
${DATA_MACRO('ORACLE_CLOCK_DATA')}

/* ---- the catalogue the vectors were read on ------------------------------------------------------------ */

#define ORACLE_CLOCK_SERIES ${SERIES}
#define ORACLE_CLOCK_ANCHOR ${ANCHOR}
#define ORACLE_CLOCK_CATALOGUE_MS_COUNT ${E_MS.length}
/* E[0..${EXTENT}] in Unix MILLISECONDS (SPEC S14), as frc_catalogue_t.instants_ms holds them. */
ORACLE_CLOCK_DATA int64_t ORACLE_CLOCK_CATALOGUE_MS[ORACLE_CLOCK_CATALOGUE_MS_COUNT] = {
${chunk(E_MS.map(String), 5).map((row) => `  ${row.join(', ')}`).join(',\n')}
};

/* ---- samples: the clock at an instant --------------------------------------------------------------------- */

#define ORACLE_CLOCK_LEVELS ${MODEL_MAX_LEVEL + 1} /* levels 0..${MODEL_MAX_LEVEL} */

/* A glyph state (frc_glyph_t without its index). */
typedef struct oracle_clock_glyph {
  int16_t value;      /* -M .. +M */
  int8_t  derivative; /* +1 rising, 0 at peak and valley, -1 falling */
  uint8_t dotted;     /* 1 on the falling branch only */
  int8_t  trit[3];    /* balanced ternary, trit[0] LEAST significant; unused are 0 */
} oracle_clock_glyph_t;

/* One level at the sample instant: frc_cycle_at, frc_glyph_at, frc_odometer_at(.., 2, 2), frc_next_threshold. */
typedef struct oracle_clock_level {
  uint64_t cycle;        /* floor(position * P(level) / 4) */
  uint16_t local;        /* cycle mod siblings; level 0: == cycle */
  uint16_t siblings;     /* S[(level-1) mod 8]; level 0: the number of Alphas in the catalogue */
  uint32_t phase;        /* floor(frac(position * P(level) / 4) * 2^32) */
  uint8_t  quarter;      /* floor(4 * that fraction), exact */
  uint8_t  clipped;      /* 1 when the catalogue ends inside this cycle */
  int64_t  start_us;     /* first microsecond of the cycle */
  int64_t  end_us;       /* first microsecond after it; clipped: the closing instant */
  uint16_t index[3];     /* glyph index for 2, 3 and 4 strokes, ascending family */
  oracle_clock_glyph_t glyph2; /* the 2-stroke state of index[0] */
  oracle_clock_glyph_t glyph3; /* the 3-stroke state of index[1] */
  uint16_t odometer[2];  /* odometer of 2 glyphs of 2 strokes, coarsest first; odometer[0] == index[0] */
  int64_t  odometer_next_us; /* when its finest digit next changes, or ORACLE_CLOCK_NONE */
  uint8_t  next_name;    /* the next threshold strictly after the instant */
  int64_t  next_us;      /* its instant, or ORACLE_CLOCK_NONE */
} oracle_clock_level_t;

/* The solar sync at the sample instant: frc_sync_at, frc_sync_odometer. */
typedef struct oracle_clock_sync {
  uint8_t  has;          /* 0 inside the first / last partial sync (frc_sync_at: FRC_ERR_RANGE); the rest is 0 */
  int32_t  number;       /* 0 at the anchor, negative before it */
  int64_t  start_us;
  int64_t  end_us;
  int64_t  nominal_us;   /* E[anchor] + number * 4 days, exact */
  int64_t  drift_us;     /* start_us - nominal_us */
  uint32_t phase;        /* floor((t - start) / (end - start) * 2^32), exact start and end */
  uint8_t  quarter;
  uint8_t  next_name;    /* the next quarter of the sync */
  int64_t  next_us;
  uint16_t digits_s2d3[3]; /* the phase as 3 glyphs of 2 strokes (the reference odometer, S7.3) */
  uint16_t digits_s3d2[2]; /* the phase as 2 glyphs of 3 strokes */
} oracle_clock_sync_t;

/* The Pentad at the sample instant: frc_pentad_at. Defined in the partial syncs too. */
typedef struct oracle_clock_pentad {
  uint8_t has;           /* 1 for every in-range sample */
  int32_t sync;
  uint8_t index;         /* 0 Pentad I, 1 II, 2 III, 3 Hypopentad */
  uint8_t helion;        /* the running Helion's place in it, 1..5 */
  uint8_t helions;       /* 5, or 4 for the Hypopentad */
  int64_t start_us;      /* cut at the catalogue's edges */
  int64_t end_us;
} oracle_clock_pentad_t;

typedef struct oracle_clock_sample {
  const char *label;     /* "A", "E[20]", "E[20]+1ms", "catalogue#7", "modern#3", ... */
  int64_t  t_us;
  uint8_t  in_range;     /* 0: outside the catalogue (frc_position_at: FRC_ERR_RANGE); every field below is 0 */
  uint16_t ordinal;      /* frc_position_t: E[ordinal] <= t < E[ordinal + 1] */
  uint64_t num;          /* t - E[ordinal], microseconds */
  uint64_t den;          /* E[ordinal + 1] - E[ordinal], microseconds */
  oracle_clock_level_t level[ORACLE_CLOCK_LEVELS];
  uint16_t odo6_s3d2[2]; /* level 6 odometer: 3 strokes, 2 digits */
  int64_t  odo6_s3d2_next_us;
  uint16_t odo6_s2d3[3]; /* level 6 odometer: 2 strokes, 3 digits */
  int64_t  odo6_s2d3_next_us;
  oracle_clock_sync_t sync;
  oracle_clock_pentad_t pentad;
} oracle_clock_sample_t;

#define ORACLE_CLOCK_SAMPLES_COUNT ${samples.length}
/* The four instants of SPEC S12 come first, in this order. */
#define ORACLE_CLOCK_SAMPLE_A 0
#define ORACLE_CLOCK_SAMPLE_B 1
#define ORACLE_CLOCK_SAMPLE_C 2
#define ORACLE_CLOCK_SAMPLE_ANCHOR_PLUS_1S 3
/* E[${EXTENT}] - 1 ms: the last instant the core and the reference read alike. */
#define ORACLE_CLOCK_SAMPLE_CLOSING_MINUS_1MS ${closing}
/* In range but inside the first / the last partial sync: sync.has == 0, pentad cut at the edge. */
#define ORACLE_CLOCK_SAMPLE_FIRST_PARTIAL_SYNC ${partialFirst}
#define ORACLE_CLOCK_SAMPLE_LAST_PARTIAL_SYNC ${partialLast}

ORACLE_CLOCK_DATA oracle_clock_sample_t ORACLE_CLOCK_SAMPLES[ORACLE_CLOCK_SAMPLES_COUNT] = {
${samples.map(sampleC).join(',\n')}
};

/* ---- deep levels ${MODEL_MAX_LEVEL + 1}..${CLOCK_MAX_LEVEL} ------------------------------------------------------------------------------- */

#define ORACLE_CLOCK_DEEP_FIRST_LEVEL ${MODEL_MAX_LEVEL + 1}
#define ORACLE_CLOCK_DEEP_LEVELS ${CLOCK_MAX_LEVEL - MODEL_MAX_LEVEL} /* levels ${MODEL_MAX_LEVEL + 1}..${CLOCK_MAX_LEVEL} = FRC_CLOCK_MAX_LEVEL */

/* The reference model stops at level 13; these are computed with BigInt from the exact position and P(L)
 * and agree with the library's clock.period / clock.phaseAt, which accept deeper levels. No cycle is clipped
 * here (4 divides P(L) from level 8 down). Cycles shorter than a microsecond can have start_us == t_us and
 * end_us == t_us + 1. */
typedef struct oracle_clock_deep_level {
  uint64_t cycle;
  uint16_t local;
  uint16_t siblings;
  uint32_t phase;
  uint8_t  quarter;
  int64_t  start_us;
  int64_t  end_us;
  uint16_t index2;       /* glyph index, 2 strokes */
} oracle_clock_deep_level_t;

typedef struct oracle_clock_deep_sample {
  int64_t  t_us;
  uint16_t sample;       /* index into ORACLE_CLOCK_SAMPLES */
  oracle_clock_deep_level_t level[ORACLE_CLOCK_DEEP_LEVELS]; /* level[i] is level ORACLE_CLOCK_DEEP_FIRST_LEVEL + i */
} oracle_clock_deep_sample_t;

#define ORACLE_CLOCK_DEEP_SAMPLES_COUNT ${deepSamples.length}
ORACLE_CLOCK_DATA oracle_clock_deep_sample_t ORACLE_CLOCK_DEEP_SAMPLES[ORACLE_CLOCK_DEEP_SAMPLES_COUNT] = {
${deepSamples.map(deepC).join(',\n')}
};

/* ---- cycles by index: frc_cycle_by_index, frc_cycle_points -------------------------------------------------- */

typedef struct oracle_clock_cycle {
  uint8_t  level;        /* 0..${CLOCK_MAX_LEVEL} */
  uint64_t cycle;        /* global index */
  uint16_t local;
  uint16_t siblings;
  uint8_t  clipped;
  int64_t  start_us;
  int64_t  end_us;       /* clipped: the closing instant */
  int64_t  point_us[5];  /* start, peak, node, valley, end; ORACLE_CLOCK_NONE beyond the catalogue.
                            A clipped cycle has point_us[4] == ORACLE_CLOCK_NONE while end_us is the closing instant. */
} oracle_clock_cycle_t;

#define ORACLE_CLOCK_CYCLES_COUNT ${cycleTable.length}
ORACLE_CLOCK_DATA oracle_clock_cycle_t ORACLE_CLOCK_CYCLES[ORACLE_CLOCK_CYCLES_COUNT] = {
${cycleTable.map((c) => `  ${list([c.level, c.cycle, c.local, c.siblings, c.clipped, us(c.start), us(c.end), list(c.points.map(us))])}`).join(',\n')}
};

/* frc_cycle_count(cat, level) for level 0..${CLOCK_MAX_LEVEL}: the last partial cycle included. Index = level.
 * frc_cycle_by_index(level, count) and beyond is FRC_ERR_RANGE. */
#define ORACLE_CLOCK_LEVEL_CYCLES_COUNT ${cycleCounts.length}
ORACLE_CLOCK_DATA uint64_t ORACLE_CLOCK_LEVEL_CYCLES[ORACLE_CLOCK_LEVEL_CYCLES_COUNT] = {
${cycleCounts.map((c) => `  ${c}`).join(',\n')}
};

/* ---- syncs by number: frc_sync_by_number ---------------------------------------------------------------------- */

typedef struct oracle_clock_sync_by_number {
  int32_t number;
  uint8_t ok;            /* 0: not whole inside the catalogue (FRC_ERR_RANGE); the rest is 0 */
  int64_t start_us;
  int64_t end_us;
  int64_t nominal_us;
  int64_t drift_us;
} oracle_clock_sync_by_number_t;

#define ORACLE_CLOCK_SYNC_FIRST_WHOLE (${FIRST_WHOLE_SYNC})
#define ORACLE_CLOCK_SYNC_LAST_WHOLE ${LAST_WHOLE_SYNC}
#define ORACLE_CLOCK_SYNCS_BY_NUMBER_COUNT ${syncTable.length}
ORACLE_CLOCK_DATA oracle_clock_sync_by_number_t ORACLE_CLOCK_SYNCS_BY_NUMBER[ORACLE_CLOCK_SYNCS_BY_NUMBER_COUNT] = {
${syncTable.map(({ number, sync }) => `  ${sync ? list([number, 1, us(sync.start), us(sync.end), us(sync.nominal), sync.drift]) : list([number, 0, 0, 0, 0, 0])}`).join(',\n')}
};

/* ---- Pentads by number: frc_pentad_by_number --------------------------------------------------------------- */

typedef struct oracle_clock_pentad_by_number {
  int32_t sync;
  uint8_t index;         /* 0..3 */
  uint8_t ok;            /* 0: wholly outside the catalogue (FRC_ERR_RANGE); the rest is 0 */
  uint8_t helions;       /* 5, or 4 for index 3 */
  int64_t start_us;      /* cut at the catalogue's edges: E[0] or E[${EXTENT}] */
  int64_t end_us;
} oracle_clock_pentad_by_number_t;

#define ORACLE_CLOCK_PENTADS_BY_NUMBER_COUNT ${pentadTable.length}
ORACLE_CLOCK_DATA oracle_clock_pentad_by_number_t ORACLE_CLOCK_PENTADS_BY_NUMBER[ORACLE_CLOCK_PENTADS_BY_NUMBER_COUNT] = {
${pentadTable.map(({ sync, index, pentad }) => `  ${pentad ? list([sync, index, 1, pentad.helions, us(pentad.start), us(pentad.end)]) : list([sync, index, 0, 0, 0, 0])}`).join(',\n')}
};

#endif /* ORACLE_CLOCK_H */
`;
}

function chunk(items, size) {
  const out = [];
  for (let i = 0; i < items.length; i += size) out.push(items.slice(i, i + size));
  return out;
}

// ---- oracle_notation.h ------------------------------------------------------------------------------------------

function dateC(s) {
  const d = s.date;
  return `  /* ${commentSafe(s.label)} */\n  {${s.tUs},${samples.indexOf(s)},${[d.alpha, d.beta, d.gamma, d.delta, d.epsilon, d.eta, d.iota, d.lambda, d.nu].join(',')},${list(d.quarter)},\n` +
    `   {${d.text.map((forms) => list(forms.map(cString))).join(',\n    ')}},\n` +
    `   ${cString(d.stamp)},\n   ${list(d.pentad.map((pair) => list(pair.map(cString))))}}`;
}
function partsC(parts) {
  const padded = [...parts.map((p) => list([p.unit, p.count])), ...Array(9).fill('{0,0}')].slice(0, 9);
  return list(padded);
}
function spanC(s) {
  return `  /* ${commentSafe(s.label)} */\n  {${s.t1},${s.t2},${s.negative},${s.blinks},${s.frac},${list(s.n)},${list(s.nz)},\n` +
    `   ${partsC(s.parts)},\n   ${partsC(s.zparts)},\n   ${list(s.text.map(cString))}}`;
}

function notationHeader() {
  const differs = referenceDiffers.length
    ? referenceDiffers.map((line) => ` *   ${commentSafe(line)}`).join('\n')
    : ' *   (none)';
  return `${PREAMBLE('oracle_notation.h', 'How a correct clock SAYS a moment and a span (SPEC S15): dates, stamps, Pentads, spans, cycle names, rates, pB.')}
/*
 * EXACTNESS WINS
 *   The reference (src/core/pure.js) words spans, sub-Blink decimals and pB values in doubles and adds a
 *   1e-12 Saros epsilon (0.57 ms) to a span before counting units. The core is exact, so the texts below follow
 *   the same rules in exact arithmetic: a half always rounds up, and in place of the epsilon a span has the
 *   grain of its instants, one MICROSECOND (a span within a microsecond of a whole number of quarter Blinks is
 *   that number: ${spansOnTheGrain} rows). The ${referenceDiffers.length} vectors where the reference's own text is different are
 *   listed here; for each the EXACT text is the one in the tables.
${differs}
 *
 * UNITS are frc_unit_t: 0 alpha, 1 beta, 2 gamma, 3 delta, 4 epsilon (Nuclide), 5 eta (Helion), 6 iota (Preon),
 * 7 lambda (Flavon), 8 nu (Blink). Quarters are frc_quarter_t: 0 Peak, 1 Node, 2 Valley, 3 End.
 * Times are int64 Unix microseconds; whole milliseconds, except the last span rows ("level L cycle C ..."),
 * which run between boundary instants as the core returns them (the first microsecond at or after each).
 * A span's frac is the floor to 2^-32 Blink, one unit more when a multiple of 1/20000 Blink (where the texts
 * round) lies above that floor and not above the exact value.
 */
#ifndef ORACLE_NOTATION_H
#define ORACLE_NOTATION_H

#include <stdint.h>

${DATA_MACRO('ORACLE_NOTATION_DATA')}

/* ---- dates: frc_date_at, frc_format_date, frc_format_pentad ------------------------------------------------ */

#define ORACLE_NOTATION_FINEST_COUNT 6 /* text[finest - 3]: finest = FRC_UNIT_DELTA (3) .. FRC_UNIT_BLINK (8) */
#define ORACLE_NOTATION_FORM_COUNT 4   /* frc_date_form_t: 0 full, 1 short, 2 letters, 3 short letters */

typedef struct oracle_notation_date {
  int64_t  t_us;
  uint16_t clock_sample;  /* index into ORACLE_CLOCK_SAMPLES of oracle_clock.h (the same instant) */
  uint16_t alpha;         /* frc_date_t, field for field */
  uint8_t  beta, gamma, delta, epsilon, eta, iota, lambda, nu;
  uint8_t  quarter[4];    /* of alpha, beta, gamma, delta */
  const char *text[ORACLE_NOTATION_FINEST_COUNT][ORACLE_NOTATION_FORM_COUNT]; /* frc_format_date(date, form, finest) */
  const char *stamp;      /* the stamp of S15.3: letters, down to nu == text[5][2] */
  const char *pentad[3][2]; /* frc_format_pentad: [0 full, 1 short, 2 ascii][0 without, 1 with the helion] */
} oracle_notation_date_t;

/* Every in-range sample of oracle_clock.h, in the same order (so the four S12 instants are 0..3). */
#define ORACLE_NOTATION_DATES_COUNT ${inRangeSamples.length}
ORACLE_NOTATION_DATA oracle_notation_date_t ORACLE_NOTATION_DATES[ORACLE_NOTATION_DATES_COUNT] = {
${inRangeSamples.map(dateC).join(',\n')}
};

/* ---- spans: frc_span_between, frc_span_parts, frc_format_span ------------------------------------------------ */

typedef struct oracle_notation_span_part {
  uint8_t  unit;          /* frc_unit_t */
  uint64_t count;
} oracle_notation_span_part_t;

typedef struct oracle_notation_span {
  int64_t  t1_us, t2_us;  /* the span runs from t1 to t2 */
  uint8_t  negative;      /* 1 when t2 < t1; 0 for the zero span */
  uint64_t blinks;        /* floor(|pos2 - pos1| * P(12) / 4): whole Blinks, exact across eclipses */
  uint32_t frac;          /* floor(the rest of a Blink * 2^32) */
  uint8_t  n[3];          /* parts frc_span_parts returns for max_parts 2, 3, 9 without keep_zeros */
  uint8_t  nz[3];         /* the same with keep_zeros */
  oracle_notation_span_part_t parts[9];  /* max_parts 9, non-zero parts only (a sub-Blink span: one part, 0 nu).
                                   max_parts 2 and 3 return its first n[0] and n[1] entries. Unused are {0,0}. */
  oracle_notation_span_part_t zparts[9]; /* max_parts 9 with keep_zeros: consecutive units from the largest that fits.
                                   max_parts 2 and 3 return its first nz[0] and nz[1] entries. */
  const char *text[2];    /* frc_format_span with max_parts 2 and 3 (exact rule; U+2212 minus) */
} oracle_notation_span_t;

#define ORACLE_NOTATION_SPANS_COUNT ${spans.length}
ORACLE_NOTATION_DATA oracle_notation_span_t ORACLE_NOTATION_SPANS[ORACLE_NOTATION_SPANS_COUNT] = {
${spans.map(spanC).join(',\n')}
};

/* ---- cycle lengths: frc_span_of_level + frc_format_span(.., 2) == pure.cycleLength ----------------------------- */

typedef struct oracle_notation_cycle_length {
  uint8_t  level;
  uint64_t blinks;        /* P(12) / P(level), whole Blinks */
  uint32_t frac;          /* and the fraction of one, floor(* 2^32): levels below nu */
  const char *text;       /* "1α", "11η", "4ι", "0.14ν", "<0.001ν", "0ν" */
} oracle_notation_cycle_length_t;

#define ORACLE_NOTATION_CYCLE_LENGTHS_COUNT ${cycleLengths.length}
ORACLE_NOTATION_DATA oracle_notation_cycle_length_t ORACLE_NOTATION_CYCLE_LENGTHS[ORACLE_NOTATION_CYCLE_LENGTHS_COUNT] = {
${cycleLengths.map((c) => `  ${list([c.level, c.blinks, c.frac, cString(c.text)])}`).join(',\n')}
};

/* ---- cycle names: frc_format_cycle_name ------------------------------------------------------------------------- */

typedef struct oracle_notation_cycle_name {
  uint8_t  level;         /* 0..16 */
  uint64_t cycle;         /* global index */
  const char *name;       /* pure.cycleName:  frc_format_cycle_name(level, cycle, 0) */
  const char *name_short; /* pure.cycleName {short: true}: the name without its Nuclide ("5η 9ι"); no core call */
  const char *short_form; /* pure.cycleShort: frc_format_cycle_name(level, cycle, 1) */
  uint8_t  carried;       /* 1: a sub-Blink start whose 2 decimals round up to the next whole Blink ("25ν") */
} oracle_notation_cycle_name_t;

#define ORACLE_NOTATION_CYCLE_NAMES_COUNT ${cycleNames.length}
ORACLE_NOTATION_DATA oracle_notation_cycle_name_t ORACLE_NOTATION_CYCLE_NAMES[ORACLE_NOTATION_CYCLE_NAMES_COUNT] = {
${cycleNames.map((c) => `  ${list([c.level, c.cycle, cString(c.name), cString(c.nameShort), cString(c.short), c.carried])}`).join(',\n')}
};

/* ---- fracton ordinals: frc_format_ordinal ----------------------------------------------------------------------- */

typedef struct oracle_notation_ordinal {
  uint16_t n;
  const char *text;
} oracle_notation_ordinal_t;

#define ORACLE_NOTATION_ORDINALS_COUNT ${ordinals.length}
ORACLE_NOTATION_DATA oracle_notation_ordinal_t ORACLE_NOTATION_ORDINALS[ORACLE_NOTATION_ORDINALS_COUNT] = {
${ordinals.map((o) => `  ${list([o.n, cString(o.text)])}`).join(',\n')}
};

/* ---- time-lapse presets: frc_rate_preset, frc_format_rate ------------------------------------------------------ */

typedef struct oracle_notation_rate {
  const char *id;         /* the reference's id of the preset */
  uint8_t  unit;          /* frc_unit_t; preset 0 (real time) has no unit and says 8 here */
  uint8_t  count;         /* 0 for real time */
  uint64_t factor;        /* displayed time per real time: count * Blinks in the unit */
  const char *label;      /* "real time", "4ι per Blink" */
} oracle_notation_rate_t;

#define ORACLE_NOTATION_RATES_COUNT ${rates.length}
ORACLE_NOTATION_DATA oracle_notation_rate_t ORACLE_NOTATION_RATES[ORACLE_NOTATION_RATES_COUNT] = {
${rates.map((r) => `  ${list([cString(r.id), r.unit, r.count, r.factor, cString(r.label)])}`).join(',\n')}
};

/* ---- cycles per Blink: frc_level_per_blink, frc_format_per_blink ------------------------------------------------ */

typedef struct oracle_notation_per_blink {
  int8_t   level;         /* 0..24: num/den is P(level) / P(12) reduced; -1: a hand-picked rational */
  uint64_t num;
  uint64_t den;
  const char *text;       /* digit groups are U+202F; "nothing" is U+2014 */
} oracle_notation_per_blink_t;

#define ORACLE_NOTATION_PER_BLINK_COUNT ${perBlink.length}
ORACLE_NOTATION_DATA oracle_notation_per_blink_t ORACLE_NOTATION_PER_BLINK[ORACLE_NOTATION_PER_BLINK_COUNT] = {
${perBlink.map((r, i) => `  ${list([r.level, r.num, r.den, cString(r.text)])}${i + 1 < perBlink.length ? ',' : ''}${r.level < 0 ? ` /* ${commentSafe(r.note)} */` : ''}`).join('\n')}
};

/* ---- SPEC S15.6, row for row -------------------------------------------------------------------------------------- */

typedef struct oracle_notation_s15_6 {
  const char *vector;     /* "A", "B", "C", "anchor+1s" */
  int64_t  t_us;
  uint16_t clock_sample;  /* index into ORACLE_CLOCK_SAMPLES */
  const char *full_date;  /* FRC_DATE_FULL down to FRC_UNIT_BLINK */
  const char *short_date; /* FRC_DATE_SHORT down to FRC_UNIT_PREON */
  uint16_t helion_pair[2]; /* frc_odometer_at(level 6, 2 strokes, 2 digits) */
} oracle_notation_s15_6_t;

#define ORACLE_NOTATION_S15_6_COUNT ${S15_6.length}
ORACLE_NOTATION_DATA oracle_notation_s15_6_t ORACLE_NOTATION_S15_6[ORACLE_NOTATION_S15_6_COUNT] = {
${S15_6.map((r) => `  ${list([cString(r.label), r.t, r.clockIndex, `\n   ${cString(r.full)}`, `\n   ${cString(r.short)}`, list(r.pair)])}`).join(',\n')}
};

/* "The stamp at A is ..." (FRC_DATE_LETTERS down to FRC_UNIT_BLINK). */
#define ORACLE_NOTATION_S15_6_STAMP_A ${cString(samples[0].date.stamp)}
/* "At A the depth-7 end is 3,372.428 s away, which is the span 6ι 43λ": from A to A + 3372.428 s. */
#define ORACLE_NOTATION_S15_6_SPAN_T1_US INT64_C(${A_MS * 1000n})
#define ORACLE_NOTATION_S15_6_SPAN_T2_US INT64_C(${(A_MS + 3_372_428n) * 1000n})
#define ORACLE_NOTATION_S15_6_SPAN_TEXT ${cString(spans.find((s) => s.label === 'S15.6').text[0])}

#endif /* ORACLE_NOTATION_H */
`;
}

// ---------------------------------------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------------------------------------

const outputs = [
  ['oracle_clock.h', clockHeader()],
  ['oracle_notation.h', notationHeader()],
];
let stale = 0;
for (const [name, text] of outputs) {
  const file = path.join(OUT_DIR, name);
  assert(Buffer.byteLength(text) < 2.2e6, `${name} is ${Buffer.byteLength(text)} bytes: over the size budget`);
  if (CHECK_ONLY) {
    const onDisk = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null;
    if (onDisk !== text) {
      stale++;
      console.error(`generate-clock: ${path.relative(CORE, file)} is stale`);
    }
  } else {
    fs.mkdirSync(OUT_DIR, { recursive: true });
    fs.writeFileSync(file, text);
  }
  console.log(`${CHECK_ONLY ? 'checked' : 'wrote  '} ${path.relative(CORE, file)}  ${Buffer.byteLength(text)} bytes`);
}

const inSync = inRangeSamples.filter((s) => s.sync).length;
console.log(
  [
    `reference        fractonica.js ${F.version} at ${REFERENCE}`,
    `clock samples    ${samples.length} (${inRangeSamples.length} in range, ${inSync} with a whole sync), deep ${deepSamples.length}`,
    `cycles           ${cycleTable.length}, syncs by number ${syncTable.length}, pentads by number ${pentadTable.length}`,
    `notation         ${inRangeSamples.length} dates, ${spans.length} spans, ${cycleNames.length} cycle names, ${perBlink.length} pB`,
    `cross-checks     ${crossChecks} exact values agree with the model's doubles`,
    `SPEC             S12 and S15.6 reproduced${specCatalogueChecked ? ', S14 catalogue identical' : ' (SPEC.md not found: S14 not compared)'}`,
    `sync phase       ${syncPhaseRoundedDiffers.length} of ${inSync} would differ if taken from the rounded start and end`,
    `sync digits      ${syncDigitsFromPhase32Differ} of ${inSync} 2 x 3-stroke readings would differ if taken from the 32-bit phase`,
    `span text        ${spanTextFromTruncated} texts change when the fraction is cut to 32 bits; ${spansOnTheGrain} spans are on the time grain`,
    `reference text   ${referenceDiffers.length} vectors differ from the reference's doubles (listed in oracle_notation.h)`,
  ].join('\n'),
);
if (stale) process.exit(1);
