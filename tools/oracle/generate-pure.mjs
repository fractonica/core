#!/usr/bin/env node
/*
 * generate-pure.mjs - the PURE-WORDING ORACLE of fractonica-core.
 *
 * Writes one C header of test vectors, computed with exact BigInt arithmetic:
 *
 *   tests/vectors/oracle_pure.h   the four wordings of "Pure wording" in fractonica.h
 *                                 (src/frc_pure.c): frc_format_count, frc_format_level_in_unit,
 *                                 frc_format_level_rate, frc_format_light_span
 *
 * Nothing here calls the C library or the JavaScript reference. Every rule is
 * stated once more, on exact rationals [numerator, denominator] of BigInts,
 * and rounded ONCE into the text the C function must write:
 *
 *   count          n in groups of three digits separated by U+202F
 *   level in unit  P(level of unit) / P(level), grouped, then the unit's symbol
 *   level rate     P(level) / P(12) cycles per Blink: the per-Blink text of SPEC S15.5
 *                  down to the Blink, the whole number below it (in full, or
 *                  d.ddd x 10^n from 1 000 000 up, half to even)
 *   light span     B = P(12) / P(level) * v / c Blinks: a span of SPEC S15.4 from one
 *                  Blink up; below it three significant digits of the largest rung
 *                  that fits; below the last rung a power of ten of that rung
 *
 * What is read from SPEC.md (docs/SPEC.md), so that no table is typed in twice:
 *   - P(0) .. P(63), the list of S16.4, held against the factor sequence of S3.1;
 *   - the names of the 63 rungs, the table of S16.1, held against the naming
 *     rule of S3.4 as it is stated there (letter L mod 24, then a number);
 *   - the nine units, their symbols and depths, the table of S15.1;
 *   - the seven media and their speeds, the table of S16.3.
 * The generator stops on the first check that fails and then writes nothing.
 *
 * Usage (from anywhere):
 *   node tools/oracle/generate-pure.mjs [--spec SPEC.md] [--out tests/vectors] [--check]
 *   --check   regenerate in memory and exit 1 if the file on disk differs
 *
 * Output is deterministic: no clock, no Math.random (a fixed-seed xorshift64
 * picks the "random" speeds), no locale. The layouts are documented in the
 * header itself and in tools/oracle/README.md, "The pure-wording vectors".
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

/* ------------------------------------------------------------------------- */
/* Command line                                                               */
/* ------------------------------------------------------------------------- */

function parseArgs(argv) {
  const opts = { spec: null, out: path.resolve(HERE, '../../tests/vectors'), checkOnly: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--spec') opts.spec = path.resolve(argv[++i]);
    else if (a === '--out') opts.out = path.resolve(argv[++i]);
    else if (a === '--check') opts.checkOnly = true;
    else throw new Error(`Unknown argument: ${a}`);
  }
  // the repository's own copy first (docs/SPEC.md), then a SPEC.md beside or above the core
  opts.spec ??= [
    path.resolve(HERE, '../../docs/SPEC.md'),
    path.resolve(HERE, '../../SPEC.md'),
    path.resolve(HERE, '../../../SPEC.md'),
  ].find((p) => fs.existsSync(p)) ?? null;
  if (!opts.spec) throw new Error('SPEC.md not found; pass --spec <path>.');
  return opts;
}

/* ------------------------------------------------------------------------- */
/* Checks                                                                     */
/* ------------------------------------------------------------------------- */

let checks = 0;
function check(condition, message) {
  checks++;
  if (!condition) throw new Error(`ORACLE CHECK FAILED: ${typeof message === 'function' ? message() : message}`);
}
function checkEqual(actual, expected, what) {
  check(actual === expected, () => `${what}: got ${JSON.stringify(String(actual))}, expected ${JSON.stringify(String(expected))}`);
}

/* ------------------------------------------------------------------------- */
/* Exact rationals: [n, d] with d > 0                                         */
/* ------------------------------------------------------------------------- */

const mul = ([a, b], [c, d]) => [a * c, b * d];
const div = ([a, b], [c, d]) => [a * d, b * c];
const cmp = ([a, b], [c, d]) => { const l = a * d, r = c * b; return l < r ? -1 : l > r ? 1 : 0; };
const int = (n) => [BigInt(n), 1n];
const ONE = int(1);
const pow10 = (k) => (k >= 0 ? [10n ** BigInt(k), 1n] : [1n, 10n ** BigInt(-k)]);
/** round(n / d) with halves to even, n >= 0, d > 0. */
function roundHalfEven(n, d) {
  let q = n / d;
  const twice = 2n * (n % d);
  if (twice > d || (twice === d && (q & 1n) === 1n)) q += 1n;
  return q;
}
/** round(n / d), a half going up; n >= 0, d > 0. */
const roundHalfUp = (n, d) => (2n * n + d) / (2n * d);

/* ------------------------------------------------------------------------- */
/* Code points (written as numbers: several of these have look-alikes)        */
/* ------------------------------------------------------------------------- */

const cp = (...codes) => String.fromCodePoint(...codes);
const NNBSP = cp(0x202F); // NARROW NO-BREAK SPACE: the digit-group separator
const TIMES = cp(0x00D7); // the multiplication sign
const DASH = cp(0x2014); // "nothing here" in SPEC's tables
/** Superscript minus and digits. 1, 2 and 3 live in Latin-1; the others in U+207x. */
const SUPERSCRIPT = {
  '-': cp(0x207B), 0: cp(0x2070), 1: cp(0x00B9), 2: cp(0x00B2), 3: cp(0x00B3),
  4: cp(0x2074), 5: cp(0x2075), 6: cp(0x2076), 7: cp(0x2077), 8: cp(0x2078), 9: cp(0x2079),
};
const superscript = (k) => [...String(k)].map((c) => SUPERSCRIPT[c]).join('');

/* ------------------------------------------------------------------------- */
/* SPEC S3.1: the hierarchy                                                   */
/* ------------------------------------------------------------------------- */

const SEQUENCE = [5n, 13n, 5n, 5n, 7n, 11n, 9n, 4n];
const LEVEL_COUNT = 63;
const BLINK_LEVEL = 12;
/** P[0..63]: P(0) = 1, P(L) = P(L-1) * S[(L-1) mod 8]. */
const P = [1n];
for (let L = 1; L <= 63; L++) P.push(P[L - 1] * SEQUENCE[(L - 1) % 8]);
/** S[level mod 8]: how many cycles of level + 1 one cycle of `level` holds. */
const factor = (level) => SEQUENCE[level % 8];

/**
 * SPEC S3.4, as it is worded there: "Level L is named with Greek letter L mod 24 of
 * αβγδεζηθικλμνξοπρστυφχψω. After ω the letters repeat with a numeric suffix:
 * level 24 is α1, level 49 is β2."
 * The alphabet is built from code points: 24 letters from U+03B1, without the
 * final sigma U+03C2 that sits between rho and sigma.
 */
const ALPHABET = [];
for (let c = 0x03B1; ALPHABET.length < 24; c++) if (c !== 0x03C2) ALPHABET.push(cp(c));
function levelName(level) {
  const round = Math.floor(level / 24);
  return ALPHABET[level % 24] + (round > 0 ? String(round) : '');
}

/* ------------------------------------------------------------------------- */
/* Reading SPEC.md                                                            */
/* ------------------------------------------------------------------------- */

function section(spec, fromHeading, toHeading) {
  const a = spec.indexOf(fromHeading);
  const b = toHeading ? spec.indexOf(toHeading, a + 1) : spec.length;
  check(a >= 0 && b > a, `SPEC section not found: ${fromHeading}`);
  return spec.slice(a, b);
}
/** The body rows of the first markdown table of `text` whose header starts with `firstHeader`. */
function tableRows(text, firstHeader) {
  const lines = text.split('\n');
  const start = lines.findIndex((l) => l.startsWith(`| ${firstHeader} |`));
  check(start >= 0, `SPEC table not found: ${firstHeader}`);
  const rows = [];
  for (let i = start + 2; i < lines.length && lines[i].startsWith('|'); i++)
    rows.push(lines[i].split('|').slice(1, -1).map((cell) => cell.trim()));
  return rows;
}
/** "299,792,458" or "0.001" as an exact rational in lowest terms of a power of ten. */
function decimal(text) {
  const m = /^(\d+)(?:\.(\d+))?$/.exec(text.replace(/,/g, ''));
  check(m, `not a decimal: ${text}`);
  const fraction = m[2] ?? '';
  let n = BigInt(m[1] + fraction), d = 10n ** BigInt(fraction.length);
  while (d > 1n && n % 10n === 0n) { n /= 10n; d /= 10n; }
  return [n, d];
}

function readSpec(specPath) {
  const spec = fs.readFileSync(specPath, 'utf8');
  check(/^Version 2\.2\.0\./m.test(spec.slice(0, 400)), 'SPEC.md is not version 2.2.0');

  // S16.4: "P(12) = 7319812500", one line per level
  const prefixes = [...section(spec, '### 16.4 ', '### 16.5 ').matchAll(/^P\((\d+)\) = (\d+)$/gm)].map((m) => [Number(m[1]), BigInt(m[2])]);
  check(prefixes.length === 64, 'SPEC S16.4 lists P(0)..P(63)');

  // S16.1: | Depth | Level | Name | ... one row per rung
  const ladder = tableRows(section(spec, '### 16.1 ', '### 16.2 '), 'Depth').map((c) => ({ depth: Number(c[0]), level: Number(c[1]), name: c[2] }));
  check(ladder.length === LEVEL_COUNT, `SPEC S16.1 table has ${ladder.length} rows, expected 63`);

  // S15.1: | Unit | Symbol | Depth | Made of | ...   (the everyday units are printed in bold)
  const units = tableRows(section(spec, '### 15.1 ', '### 15.2 '), 'Unit').map((c) => ({
    name: c[0].replace(/\*/g, ''), symbol: c[1], level: Number(c[2]) - 1, madeOf: c[3],
  }));
  check(units.length === 9, 'SPEC S15.1 lists nine units');

  // S16.3: | Id | Medium | Phase | Speed (m/s) | Note |
  const media = tableRows(section(spec, '### 16.3 ', '### 16.4 '), 'Id').map((c) => ({
    id: c[0].replace(/`/g, ''), name: c[1], speed: decimal(c[3]),
  }));
  check(media.length === 7, 'SPEC S16.3 lists 7 media');

  return { spec, prefixes, ladder, units, media };
}

/* ------------------------------------------------------------------------- */
/* Rule 1: a count in digit groups                                            */
/* ------------------------------------------------------------------------- */

/**
 * A whole number with its digits in groups of three from the right, separated
 * by U+202F. Written out digit by digit (no regular expression, no locale).
 */
function grouped(n) {
  const digits = n.toString();
  let out = '';
  for (let i = 0; i < digits.length; i++) {
    const left = digits.length - i; // digits still to come, this one included
    out += digits[i];
    if (left > 1 && (left - 1) % 3 === 0) out += NNBSP;
  }
  return out;
}

/* ------------------------------------------------------------------------- */
/* Rule 2: how many of a unit one cycle of a level holds                      */
/* ------------------------------------------------------------------------- */

const STATUS_OK = 0, STATUS_ARG = -2, STATUS_OVERFLOW = -3;

/** frc_format_level_in_unit: P(level of unit) / P(level), grouped, then the symbol. */
function levelInUnit(level, unit, units) {
  const u = units[unit];
  if (level > BLINK_LEVEL || u.level < level) return { status: STATUS_ARG, count: 0n, text: '' };
  check(P[u.level] % P[level] === 0n, 'a level holds a whole number of every finer unit');
  const count = P[u.level] / P[level];
  return { status: STATUS_OK, count, text: grouped(count) + u.symbol };
}

/* ------------------------------------------------------------------------- */
/* Rule 3: the rate of a rung in cycles per Blink                             */
/* ------------------------------------------------------------------------- */

/** n / 10^scale as a plain decimal without trailing zeros: (125, 2) -> "1.25", (300, 2) -> "3". */
function decimalText(n, scale) {
  if (scale <= 0) return (n * 10n ** BigInt(-scale)).toString();
  const s = n.toString().padStart(scale + 1, '0');
  const fraction = s.slice(-scale).replace(/0+$/, '');
  return fraction ? `${s.slice(0, -scale)}.${fraction}` : s.slice(0, -scale);
}
/** floor(log10(x)) for a positive rational. */
function decade([n, d]) {
  let e = String(n).length - String(d).length;
  while (cmp([n, d], pow10(e)) < 0) e--;
  while (cmp([n, d], pow10(e + 1)) >= 0) e++;
  return e;
}
/** x rounded to `digits` SIGNIFICANT digits, a half going up, without trailing zeros (S15.5 texts). */
function significantHalfUp(x, digits) {
  const scale = digits - 1 - decade(x);
  const [a, b] = mul(x, pow10(scale));
  return decimalText(roundHalfUp(a, b), scale);
}
/**
 * The per-Blink text of SPEC S15.5 for an exact rate of num/den cycles per Blink,
 * as fractonica.h words it under frc_format_per_blink: below 1 pB with a whole
 * reciprocal "1 per N Blinks" (design ruling 3: the Flavon "1 per 25", mu "1 per 5");
 * slower than 1 per 25 Blinks "1 per N Blinks", N rounded; else three significant
 * digits below 99.95, one decimal from there, a grouped whole number from 999.5;
 * halves go up.
 */
function perBlinkText(num, den) {
  const x = [num, den];
  if (num < den && den % num === 0n) return `1 per ${grouped(den / num)} Blinks`;
  if (cmp(x, [1n, 25n]) < 0) return `1 per ${grouped(roundHalfUp(den, num))} Blinks`;
  let value;
  if (cmp(x, [1999n, 2n]) >= 0) value = grouped(roundHalfUp(num, den));
  else if (cmp(x, [1999n, 20n]) >= 0) value = decimalText(roundHalfUp(num * 10n, den), 1);
  else value = significantHalfUp(x, 3);
  return `${value} pB`;
}

/** "d.ddd": an integer of `digits` digits with the point after the first. */
const mantissaText = (m, digits) => { const s = String(m); check(s.length === digits, 'mantissa length'); return digits > 1 ? `${s[0]}.${s.slice(1)}` : s; };

/**
 * x = m * 10^k with `digits` significant digits, rounded half to even from the
 * exact value; a mantissa that rounds up to 10 becomes 1.00 x 10^(k+1).
 */
function scientific(x, digits) {
  let k = decade(x);
  const [a, b] = mul(x, pow10(digits - 1 - k));
  let m = roundHalfEven(a, b);
  if (m === 10n ** BigInt(digits)) { m /= 10n; k++; }
  return { m, k, text: mantissaText(m, digits) };
}
/** The power-of-ten form of frc_format_quantity: "2.454 x 10^-25 " followed by the unit. */
const powerOfTen = (s, unit) => `${s.text} ${TIMES} 10${superscript(s.k)} ${unit}`;

const RATE_DIGITS = [0, 2, 3, 4, 9];
const FULL_BELOW = 1_000_000n; // a rate below this is always written in full

/**
 * frc_format_level_rate. Down to the Blink the rate is P(level) / P(12) <= 1 and the
 * text is the per-Blink text (digits has no say). Below the Blink it is the whole
 * number N = P(level) / P(12): in full when digits is 0 or N < 1 000 000, else
 * with `digits` significant digits and a power of ten.
 */
function levelRate(level, digits) {
  if (level <= BLINK_LEVEL) return perBlinkText(1n, P[BLINK_LEVEL] / P[level]);
  check(P[level] % P[BLINK_LEVEL] === 0n, 'a Blink holds a whole number of every finer rung');
  const N = P[level] / P[BLINK_LEVEL];
  if (digits === 0 || N < FULL_BELOW) return `${grouped(N)} pB`;
  return powerOfTen(scientific(int(N), digits), 'pB');
}

/* ------------------------------------------------------------------------- */
/* Rule 4: a length as a span of light                                        */
/* ------------------------------------------------------------------------- */

/**
 * SPEC S15.4, on the exact rational: a span of x >= 1 Blinks as counts over the
 * unit ladder, greedily from the largest unit that fits, zero counts skipped,
 * two non-zero parts. A span led by the Blink and shorter than 10 Blinks keeps
 * one decimal, a half going up, trailing zeros dropped ("2.6ν", "4ν", 9.96 "10ν").
 * This is the text frc_format_span(span, 0) gives for a span whose fraction is
 * held on the exact value's side of every rounding mark.
 */
function spanText(x, units) {
  check(cmp(x, ONE) >= 0, 'spanText is used from one Blink up');
  const [n, d] = x;
  const blinksOf = (u) => P[BLINK_LEVEL] / P[u.level];
  const first = units.findIndex((u) => blinksOf(u) * d <= n);
  const blink = units.length - 1;
  if (first === blink && n < 10n * d) return decimalText(roundHalfUp(n * 10n, d), 1) + units[blink].symbol;
  const parts = [];
  let rest = n / d; // whole Blinks: the fraction of one never reaches a part
  for (let u = first; u < units.length && parts.length < 2; u++) {
    const count = rest / blinksOf(units[u]);
    rest -= count * blinksOf(units[u]);
    if (count > 0n) parts.push(`${count}${units[u].symbol}`);
  }
  return parts.join(' ');
}

const FORM_SPAN = 0;        // B >= 1 Blink: a span
const FORM_RUNG = 1;        // B < 1: three digits of the largest rung that fits
const FORM_RUNG_CARRY = 2;  // ... which round up to one whole cycle of the next coarser rung
const FORM_POWER = 3;       // less than one cycle of level 62: a power of ten of it
const FORM_POWER_CARRY = 4; // ... which rounds up to one whole cycle of level 62
const FORM_NAMES = ['span', 'rung', 'rung, carried', 'power of ten', 'power of ten, carried'];
const LAST_LEVEL = LEVEL_COUNT - 1;
const U64_LIMIT = 1n << 64n;

/**
 * frc_format_light_span without the word. speed is [num, den] metres per second,
 * c the speed of light in vacuum. B = P(12) / P(level) * speed / c Blinks, exact.
 */
function lightSpan(level, speed, c, units) {
  const B = mul([P[BLINK_LEVEL], P[level]], div(speed, c));
  if (cmp(B, ONE) >= 0) {
    if (B[0] / B[1] >= U64_LIMIT) return { status: STATUS_OVERFLOW, form: FORM_SPAN, text: '' }; // frc_span_t holds 64 bits of Blinks
    return { status: STATUS_OK, form: FORM_SPAN, text: spanText(B, units) };
  }
  // count(K) = B * P(K) / P(12): how many cycles of rung K the span is
  const countAt = (K) => mul(B, [P[K], P[BLINK_LEVEL]]);
  for (let K = BLINK_LEVEL + 1; K <= LAST_LEVEL; K++) {
    const count = countAt(K);
    if (cmp(count, ONE) < 0) continue;
    // the coarsest rung that fits: 1 <= count < factor(K - 1), because rung K - 1 did not fit
    const whole = factor(K - 1);
    check(cmp(count, int(whole)) < 0, 'the count is below the factor of the rung above');
    const s = scientific(count, 3); // m x 10^(k-2), k = 0 or 1
    check(s.k === 0 || s.k === 1, 'a count has one or two digits before the point');
    if (s.m * 10n ** BigInt(s.k) === whole * 100n) // rounded up to one whole cycle of rung K - 1
      return { status: STATUS_OK, form: FORM_RUNG_CARRY, text: `1.00${levelName(K - 1)}` };
    const digits = String(s.m);
    const text = s.k === 0 ? `${digits[0]}.${digits.slice(1)}` : `${digits.slice(0, 2)}.${digits[2]}`;
    return { status: STATUS_OK, form: FORM_RUNG, text: text + levelName(K) };
  }
  const s = scientific(countAt(LAST_LEVEL), 3);
  check(s.k <= 0, 'below the last rung the count is below 1');
  if (s.k === 0) return { status: STATUS_OK, form: FORM_POWER_CARRY, text: `1.00${levelName(LAST_LEVEL)}` };
  return { status: STATUS_OK, form: FORM_POWER, text: powerOfTen(s, levelName(LAST_LEVEL)) };
}
const WORD = ' light';

/** xorshift64: a fixed stream of 64-bit values, so the "random" vectors never change. */
function makeRandom(seed) {
  const MASK = (1n << 64n) - 1n;
  let s = seed;
  const next = () => { s ^= (s << 13n) & MASK; s ^= s >> 7n; s ^= (s << 17n) & MASK; return s; };
  return { next, below: (n) => next() % BigInt(n) };
}

/* ------------------------------------------------------------------------- */
/* C output                                                                   */
/* ------------------------------------------------------------------------- */

/** A C string literal with the exact UTF-8 bytes; non-ASCII bytes as 3-digit octal escapes. */
function cString(text) {
  let out = '"';
  for (const byte of Buffer.from(text, 'utf8')) {
    if (byte === 0x22 || byte === 0x5c) out += `\\${String.fromCharCode(byte)}`;
    else if (byte >= 0x20 && byte < 0x7f) out += String.fromCharCode(byte);
    else out += `\\${byte.toString(8).padStart(3, '0')}`;
  }
  return `${out}"`;
}
const isAscii = (text) => /^[\x20-\x7e]*$/.test(text);
/** A trailing comment that shows a string in readable form, when its literal holds escapes. */
const readable = (text) => (isAscii(text) ? '' : ` /* ${text} */`);
const u64 = (v) => `UINT64_C(${v})`;
const byteLength = (text) => Buffer.byteLength(text, 'utf8');

/* ------------------------------------------------------------------------- */
/* Main                                                                       */
/* ------------------------------------------------------------------------- */

const opts = parseArgs(process.argv.slice(2));
const spec = readSpec(opts.spec);

/* ---- the hierarchy against SPEC ------------------------------------------ */
spec.prefixes.forEach(([level, value]) => check(P[level] === value, `P(${level}) differs from SPEC S16.4`));
spec.ladder.forEach((row, i) => {
  check(row.level === i && row.depth === i + 1, `SPEC S16.1 row ${i} is out of order`);
  checkEqual(levelName(i), row.name, `name of level ${i} (S3.4 rule against the S16.1 table)`);
});
checkEqual(ALPHABET.join(''), /Greek letter `L mod 24` of `([^`]+)`/.exec(spec.spec)?.[1], 'the alphabet of S3.4');
checkEqual(levelName(24), 'α1', 'S3.4: level 24');
checkEqual(levelName(49), 'β2', 'S3.4: level 49');
checkEqual(levelName(62), 'ο2', 'S16.1: depth 63');

/* ---- the units against SPEC S15.1 ----------------------------------------- */
const units = spec.units;
checkEqual(units.map((u) => u.level).join(' '), '0 1 2 3 4 6 8 10 12', 'unit levels (S15.1 depths - 1)');
units.forEach((u, i) => {
  checkEqual(u.symbol, levelName(u.level), `symbol of ${u.name} is the letter of its level`);
  if (i + 1 < units.length) {
    // "Made of": "5 Beta", "77 Helion (7 ζ × 11 η)"
    const made = /^(\d+) (\w+)/.exec(u.madeOf);
    check(made && made[2] === units[i + 1].name, `S15.1: ${u.name} is made of ${units[i + 1].name}`);
    check(P[units[i + 1].level] / P[u.level] === BigInt(made[1]), `S15.1: ${u.name} holds ${made[1]} ${units[i + 1].name}`);
  } else {
    checkEqual(u.madeOf, DASH, 'S15.1: the Blink is made of nothing');
  }
});
checkEqual(units[8].name, 'Blink', 'the finest unit');
check(units[8].level === BLINK_LEVEL, 'the Blink sits on level 12');

/* ---- the media against SPEC S16.3 ----------------------------------------- */
const media = spec.media;
checkEqual(media.map((m) => m.id).join(' '), 'vacuum bounded diamond water air nerve chemical', 'media (frc_medium_t order)');
const C = media[0].speed;
check(C[0] === 299792458n && C[1] === 1n, 'the speed of light in vacuum is 299,792,458 m/s');
media.forEach((m) => check(m.speed[0] < U64_LIMIT && m.speed[1] < U64_LIMIT, `speed of ${m.id} fits frc_speed_t`));

/* ---- rule 1: counts ------------------------------------------------------- */
const countRows = [];
{
  const seen = new Set();
  const add = (n, note) => {
    if (n < 0n || n >= U64_LIMIT || seen.has(n)) return;
    seen.add(n);
    countRows.push({ n, text: grouped(n), note });
  };
  add(0n, 'zero');
  // around every power of ten: where a digit, and every third time a group, is added
  for (let k = 1; k <= 19; k++) {
    const p = 10n ** BigInt(k);
    add(p - 1n, `10^${k} - 1`); add(p, `10^${k}`); add(p + 1n, `10^${k} + 1`);
  }
  add(U64_LIMIT - 1n, 'UINT64_MAX');
  add(U64_LIMIT - 2n, 'UINT64_MAX - 1');
  add(1n << 63n, '2^63');
  add(1n << 32n, '2^32');
  // the numbers fractonica.h and the design rulings print
  add(58500n, 'Blinks in a Helion'); add(2772n, 'rho per Blink'); add(7319812500n, 'Blinks in an Alpha');
  add(142033641750000n, 'zeta1 (level 29) per Blink');
  // digits that would show a group written the wrong way round or a zero dropped
  add(1002003n, 'zeros inside groups'); add(100000200000030n, 'zeros inside groups');
  add(123456789n, 'all digits'); add(12345678901234567890n, 'twenty digits');
  const random = makeRandom(0x9E3779B97F4A7C15n);
  for (let i = 0; i < 64; i++) add(random.next() >> BigInt(i), `random, ${64 - i} bits`);
}
// what the header promises, typed in
const literalCount = (n, text) => checkEqual(grouped(n), text.replace(/ /g, NNBSP), `grouped(${n})`);
literalCount(999n, '999'); literalCount(1000n, '1 000'); literalCount(58500n, '58 500');
literalCount(142033641750000n, '142 033 641 750 000'); literalCount(U64_LIMIT - 1n, '18 446 744 073 709 551 615');

/* ---- rule 2: a level in a unit -------------------------------------------- */
const inUnitRows = [];
for (let level = 0; level <= BLINK_LEVEL; level++)
  for (let unit = 0; unit < units.length; unit++)
    inUnitRows.push({ level, unit, ...levelInUnit(level, unit, units) });
{
  const textOf = (level, unit) => inUnitRows.find((r) => r.level === level && r.unit === unit).text;
  const HELION = 5, PREON = 6, FLAVON = 7, BLINK = 8;
  checkEqual(textOf(6, BLINK), `58${NNBSP}500ν`, 'a Helion in Blinks');
  checkEqual(textOf(6, FLAVON), `2${NNBSP}340λ`, 'a Helion in Flavons');
  checkEqual(textOf(6, PREON), '36ι', 'a Helion in Preons');
  checkEqual(textOf(6, HELION), '1η', 'a Helion in Helions');
  checkEqual(textOf(5, HELION), '11η', 'level 5 in Helions');
  checkEqual(textOf(0, BLINK), `7${NNBSP}319${NNBSP}812${NNBSP}500ν`, 'an Alpha in Blinks');
  check(inUnitRows.find((r) => r.level === 6 && r.unit === 4).status === STATUS_ARG, 'a Helion in Nuclides is refused');
}

/* ---- rule 3: rates --------------------------------------------------------- */
const rateRows = [];
for (let level = 0; level < LEVEL_COUNT; level++) {
  const below = level > BLINK_LEVEL;
  rateRows.push({
    level,
    // the exact rate P(level) / P(12) in lowest terms, as decimal strings (the numerator has up to 42 digits)
    num: below ? P[level] / P[BLINK_LEVEL] : 1n,
    den: below ? 1n : P[BLINK_LEVEL] / P[level],
    text: RATE_DIGITS.map((digits) => levelRate(level, digits)),
  });
}
{
  const rate = (level, digits) => levelRate(level, digits).replace(new RegExp(NNBSP, 'g'), ' ');
  // fractonica.h, frc_level_per_blink and frc_format_per_blink; SPEC S15.5
  checkEqual(rate(12, 0), '1 pB', 'the Blink');
  checkEqual(rate(11, 0), '1 per 5 Blinks', 'mu (design ruling 3)');
  checkEqual(rate(10, 0), '1 per 25 Blinks', 'the Flavon: exactly 1/25 has a whole reciprocal (design ruling 3)');
  checkEqual(rate(9, 0), '1 per 325 Blinks', 'kappa');
  checkEqual(rate(6, 0), '1 per 58 500 Blinks', 'the Helion');
  checkEqual(rate(0, 0), '1 per 7 319 812 500 Blinks', 'the Alpha');
  // "The true rates of the depths below the Blink are whole numbers: ξ 7, ο 77, π 693 and ρ 2,772 pB."
  checkEqual([13, 14, 15, 16].map((L) => rate(L, 0)).join(', '), '7 pB, 77 pB, 693 pB, 2 772 pB', 'S15.5');
  checkEqual([13, 14, 15, 16].map((L) => rate(L, 4)).join(', '), '7 pB, 77 pB, 693 pB, 2 772 pB', 'S15.5, four digits asked');
  // below 1 000 000 in full whatever is asked; from there a power of ten
  checkEqual(rate(19, 4), '900 900 pB', 'level 19 is the last below a million');
  checkEqual(rate(20, 0), '4 504 500 pB', 'level 20 in full');
  checkEqual(rate(20, 4), `4.504 ${TIMES} 10${superscript(6)} pB`, 'level 20, four digits: 4.5045 is a tie and goes to even');
  checkEqual(rate(20, 2), `4.5 ${TIMES} 10${superscript(6)} pB`, 'level 20, two digits');
  checkEqual(rate(20, 9), `4.50450000 ${TIMES} 10${superscript(6)} pB`, 'level 20, nine digits: zeros fill the precision');
  // the design ruling's example
  check(rateRows.some((r) => r.text[RATE_DIGITS.indexOf(4)] === `1.420 ${TIMES} 10${superscript(14)} pB`), 'ruling 3: "1.420 x 10^14 pB" is a rung');
  // P(62) has 52 digits and P(12) ten: the rate of the last rung is a whole number of 42 digits
  check(String(rateRows[62].num).length === 42 && rateRows[62].num * P[BLINK_LEVEL] === P[62], 'the rate of level 62 has 42 digits');
}

/* ---- rule 4: light spans --------------------------------------------------- */
const lightRows = [];
for (let level = 0; level < LEVEL_COUNT; level++)
  for (let medium = 0; medium < media.length; medium++) {
    const r = lightSpan(level, media[medium].speed, C, units);
    check(r.status === STATUS_OK, 'every medium of S16.3 has a light span at every level');
    lightRows.push({ level, medium, ...r });
  }
{
  const textOf = (level, medium) => lightRows.find((r) => r.level === level && r.medium === medium).text;
  const VACUUM = 0, AIR = 4;
  // light in vacuum: the rung's own span
  checkEqual(textOf(6, VACUUM), '1η', 'a Helion of light');
  checkEqual(textOf(5, VACUUM), '11η', 'a zeta of light');
  checkEqual(textOf(7, VACUUM), '4ι', 'a theta of light');
  checkEqual(textOf(12, VACUUM), '1ν', 'a Blink of light');
  checkEqual(textOf(13, VACUUM), '1.00ξ', 'a xi of light');
  checkEqual(textOf(62, VACUUM), '1.00ο2', 'the last rung of light');
  for (let level = 0; level <= BLINK_LEVEL; level++) {
    const unit = units.find((u) => u.level === level);
    if (unit) checkEqual(textOf(level, VACUUM), `1${unit.symbol}`, `one ${unit.name} of light`);
  }
  // a Helion of sound in air: 58500 * 343 / 299792458 = 0.0669 Blinks = 5.15 omicron
  checkEqual(textOf(6, AIR), '5.15ο', 'a Helion of sound in air');
  // the examples of the function's comment are real rows: a Helion of each medium, and three more
  checkEqual(media.map((m, i) => textOf(6, i)).join(' | '), `1η | 24ι 1λ | 3.5ν | 2.05ξ | 5.15ο | 1.80ο | 6.15χ`, 'a Helion in every medium');
  checkEqual(textOf(12, 2), '10.8τ', 'a Blink of sound in diamond');
  checkEqual(textOf(57, 6), `8.35 ${TIMES} 10${superscript(-8)} ο2`, 'level 57 of chemical transport');
  checkEqual(textOf(0, 1), '3β 4γ', 'an Alpha of light in a bounded medium');
}

/** Speeds chosen to stand on every edge of the rule. c is whole, so k * c / d is exact. */
const customRows = [];
let chosenCount = 0; // the rows chosen by hand come first,
let sweepCount = 0;  // then every twentieth of a Blink, then the random stream
{
  const c = C[0];
  const add = (level, num, den, note, expect) => {
    check(num > 0n && den > 0n && num < U64_LIMIT && den < U64_LIMIT, `custom speed fits frc_speed_t: ${note}`);
    const r = lightSpan(level, [num, den], C, units);
    if (expect !== undefined) checkEqual(r.status === STATUS_OK ? r.text : `status ${r.status}`, expect, `custom: ${note}`);
    customRows.push({ level, num, den, note, ...r });
  };
  const R = (K) => P[K] / P[BLINK_LEVEL]; // cycles of rung K in a Blink
  const MAX = U64_LIMIT - 1n;

  // faster than light: the same arithmetic
  add(6, 2n * c, 1n, 'twice the speed of light', '2η');
  add(6, 3n * c, 2n, '1.5 Helions', '1η 18ι');
  add(0, 1000n * c, 1n, 'a thousand Alphas', '1000α');
  // the decimal of a span led by the Blink: halves go up, on the exact value
  add(12, 105n * c, 100n, 'exactly 1.05 Blinks', '1.1ν');
  add(12, 265n * c, 100n, 'exactly 2.65 Blinks', '2.7ν');
  add(12, 264999n * c, 100000n, '2.64999 Blinks', '2.6ν');
  add(12, 995n * c, 100n, 'exactly 9.95 Blinks', '10ν');
  add(12, 1245n * c, 100n, '12.45 Blinks: no decimal from 10 up', '12ν');
  add(12, 2499n * c, 100n, '24.99 Blinks', '24ν');
  add(11, c, 5n, 'a fifth of light at mu: exactly one Blink', '1ν');
  add(10, 26n * c, 25n, 'one Blink more than a Flavon', '1λ 1ν');
  add(8, 1626n * c, 1625n, 'a Preon and a Blink: the zero Flavons are skipped', '1ι 1ν');
  // a rest that is a whole number of quarter Blinks: frc_span_t takes the rest as whole quarters and a part
  // of one, and an exact half (or quarter) stands on the border between two of them
  add(12, 5n * c, 2n, 'exactly 2.5 Blinks', '2.5ν');
  add(11, c, 2n, 'half a mu of light: 2.5 Blinks again', '2.5ν');
  add(6, 5n * c, 117000n, '2.5 Blinks from the Helion', '2.5ν');
  add(12, 3n * c, 2n, 'exactly 1.5 Blinks', '1.5ν');
  add(12, 19n * c, 2n, 'exactly 9.5 Blinks', '9.5ν');
  add(12, 9n * c, 4n, 'exactly 2.25 Blinks: one quarter, and the tie goes up', '2.3ν');
  add(12, 11n * c, 4n, 'exactly 2.75 Blinks: three quarters', '2.8ν');
  add(12, 39n * c, 4n, 'exactly 9.75 Blinks', '9.8ν');
  add(12, 5n * c + 1n, 2n, 'half a metre per second over 2.5 Blinks', '2.5ν');
  add(12, 5n * c - 1n, 2n, 'half a metre per second under 2.5 Blinks', '2.5ν');
  // just under one Blink: xi, 7 to the Blink
  add(12, 9999n * c, 10000n, '0.9999 Blinks = 6.9993 xi rounds to 7.00: one Blink', '1.00ν');
  add(12, 6994n * c, 7000n, '6.994 xi', '6.99ξ');
  add(12, 6995n * c, 7000n, '6.995 xi: the tie goes to the even 7.00, one Blink', '1.00ν');
  add(12, 6985n * c, 7000n, '6.985 xi: the tie goes to the even 6.98', '6.98ξ');
  add(12, 1005n * c, 7000n, '1.005 xi: the tie goes to the even 1.00', '1.00ξ');
  add(12, 1015n * c, 7000n, '1.015 xi: the tie goes to the even 1.02', '1.02ξ');
  add(12, c, 7n, 'exactly one xi', '1.00ξ');
  add(12, c - 1n, 7n, 'a hair under one xi: 11.0 omicron rounds to one xi', '1.00ξ');
  add(12, 109n * c, 770n, '10.9 omicron exactly', '10.9ο');
  // omicron, 11 to the xi: two digits before the point
  add(12, 9994n * c, 1000n * R(14), '9.994 omicron', '9.99ο');
  add(12, 9995n * c, 1000n * R(14), '9.995 omicron: the tie goes to the even 10.0', '10.0ο');
  add(12, 1045n * c, 100n * R(14), '10.45 omicron: the tie goes to the even 10.4', '10.4ο');
  add(12, 1055n * c, 100n * R(14), '10.55 omicron: the tie goes to the even 10.6', '10.6ο');
  add(12, 1094n * c, 100n * R(14), '10.94 omicron', '10.9ο');
  add(12, 1095n * c, 100n * R(14), '10.95 omicron: the tie goes to the even 11.0, one xi', '1.00ξ');
  // tau, 13 to the sigma
  add(12, 1294n * c, 100n * R(18), '12.94 tau', '12.9τ');
  add(12, 1295n * c, 100n * R(18), '12.95 tau: the tie goes to the even 13.0, one sigma', '1.00σ');
  add(17, 1295n * c, 1300n, 'the same from level 17', '1.00σ');
  add(18, 1294n * c, 100n, 'the same from level 18, faster than light', '12.9τ');
  // a deep level and a fast wave: up the ladder from the level asked
  add(62, MAX, 1n, 'the fastest speed at the last rung');
  add(13, MAX, 1n, 'the fastest speed at xi: billions of Blinks');
  add(40, 1000000n * c, 1n, 'a million times light at level 40');
  add(24, c * R(24), 1n, 'level 24 at the speed that makes it one Blink (the deepest level where that speed fits)', '1ν');
  add(24, c * R(24) - 1n, 1n, 'one metre per second slower: 7.00 xi rounds to one Blink', '1.00ν');
  add(24, 2n * c * R(24) + 1n, 2n, 'half a metre per second faster', '1ν');
  // below the last rung: powers of ten of it
  add(62, c, 10n, 'a tenth of the last rung', `1.00 ${TIMES} 10${superscript(-1)} ο2`);
  add(62, 9994n * c, 10000n, '0.9994 of the last rung', `9.99 ${TIMES} 10${superscript(-1)} ο2`);
  add(62, 9995n * c, 10000n, '0.9995 of the last rung: the tie goes to the even 10.0, one whole cycle', '1.00ο2');
  add(62, 9999n * c, 10000n, '0.9999 of the last rung rounds to one whole cycle', '1.00ο2');
  add(62, 1005n * c, 1000000n, '1.005e-3: the tie goes to the even 1.00', `1.00 ${TIMES} 10${superscript(-3)} ο2`);
  add(62, 1015n * c, 1000000n, '1.015e-3: the tie goes to the even 1.02', `1.02 ${TIMES} 10${superscript(-3)} ο2`);
  add(62, 9995n * c, 10000000n, '9.995e-4 rounds up into the next decade', `1.00 ${TIMES} 10${superscript(-3)} ο2`);
  add(62, 1n, MAX, 'the slowest speed at the last rung: the smallest value there is');
  add(62, 1n, 1n, 'one metre per second at the last rung');
  add(0, 1n, MAX, 'the slowest speed at the Alpha');
  add(12, 1n, MAX, 'the slowest speed at the Blink');
  add(5, MAX, MAX, 'one metre per second written with the largest terms');
  add(30, MAX, MAX - 1n, 'a hair over one metre per second');
  // the largest spans: frc_span_t holds 2^64 - 1 Blinks
  const most = (U64_LIMIT * c - 1n) / P[BLINK_LEVEL]; // the largest num with P(12) * num / c < 2^64
  add(0, most, 1n, 'the fastest wave whose Alpha still fits a span');
  add(0, most + 1n, 1n, 'one metre per second faster: FRC_ERR_OVERFLOW', `status ${STATUS_OVERFLOW}`);
  add(0, MAX, 1n, 'the fastest speed at the Alpha: FRC_ERR_OVERFLOW', `status ${STATUS_OVERFLOW}`);
  add(1, MAX, 1n, 'the fastest speed at the Beta: FRC_ERR_OVERFLOW', `status ${STATUS_OVERFLOW}`);
  add(4, MAX, 1n, 'the fastest speed at the Nuclide: it fits', '37865670α 3β');

  chosenCount = customRows.length;

  // every twentieth of a Blink from 1 to 10: each tenth, each tie between two tenths (the odd k) and each
  // whole number of quarter Blinks (k a multiple of 5), all of them exact. The decimal of the text is
  // stated once more here, on the whole number k: round(k / 2) tenths, a half going up.
  for (let k = 20n; k <= 200n; k++) {
    const tenths = (k + 1n) / 2n;
    add(12, k * c, 20n, `${k}/20 Blinks`, `${tenths / 10n}${tenths % 10n === 0n ? '' : `.${tenths % 10n}`}ν`);
  }
  sweepCount = customRows.length - chosenCount;

  // a fixed stream of speeds of every size, at every level
  const random = makeRandom(0x243F6A8885A308D3n);
  for (let i = 0; i < 378; i++) {
    const level = i % LEVEL_COUNT;
    const num = (random.next() >> random.below(64)) || 1n;
    const den = (random.next() >> random.below(64)) || 1n;
    add(level, num, den, 'random');
  }
}
const formTally = [0, 0, 0, 0, 0];
[...lightRows, ...customRows].forEach((r) => { if (r.status === STATUS_OK) formTally[r.form]++; });
formTally.forEach((n, form) => check(n > 0, `no light span takes the form "${FORM_NAMES[form]}"`));
check(customRows.some((r) => r.status === STATUS_OVERFLOW), 'no custom row overflows');

/* ---- oracle_pure.h --------------------------------------------------------- */
const longest = (texts) => texts.reduce((m, t) => Math.max(m, byteLength(t)), 0);
const LONGEST_COUNT = longest(countRows.map((r) => r.text));
const LONGEST_IN_UNIT = longest(inUnitRows.map((r) => r.text));
const LONGEST_RATE = longest(rateRows.flatMap((r) => r.text));
const LONGEST_LIGHT = longest([...lightRows, ...customRows].map((r) => r.text + WORD));

const o = [];
o.push([
  '/*',
  ' * oracle_pure.h - expected texts of the pure wordings (fractonica.h, "Pure wording"; src/frc_pure.c).',
  ' *',
  ' * GENERATED by tools/oracle/generate-pure.mjs. DO NOT EDIT: change the generator and run it again.',
  ' * Source of truth: SPEC.md 2.2.0 (S3.1 factors, S3.4 names, S15.1 units, S15.4 spans, S15.5 pB, S16.3 media)',
  ' * and the rules of fractonica.h, stated once more on exact BigInt rationals. Nothing here was',
  ' * computed by the C library.',
  ' *',
  ' * Self-contained: include it from a host test; it needs <stdint.h> only.',
  ' * Tables are `static const`; those a test does not read are tagged unused.',
  ' * Strings are UTF-8 with non-ASCII bytes written as octal escapes; the comment beside one shows it.',
  ' * Digit groups are separated by U+202F NARROW NO-BREAK SPACE (\\342\\200\\257).',
  ' */',
].join('\n'));
o.push('#ifndef ORACLE_PURE_H', '#define ORACLE_PURE_H', '', '#include <stdint.h>', '');
o.push('#if defined(__GNUC__) || defined(__clang__)', '#  define ORACLE_PURE_DATA __attribute__((unused)) static const',
  '#else', '#  define ORACLE_PURE_DATA static const', '#endif', '');
o.push('/* frc_status_t values used below. */', `#define ORACLE_PURE_OK ${STATUS_OK}`, `#define ORACLE_PURE_ERR_ARG (${STATUS_ARG})`,
  `#define ORACLE_PURE_ERR_OVERFLOW (${STATUS_OVERFLOW})`, '');
o.push('/* The longest text of each table, in bytes without the NUL. */',
  `#define ORACLE_PURE_LONGEST_COUNT ${LONGEST_COUNT}`, `#define ORACLE_PURE_LONGEST_IN_UNIT ${LONGEST_IN_UNIT}`,
  `#define ORACLE_PURE_LONGEST_RATE ${LONGEST_RATE}`, `#define ORACLE_PURE_LONGEST_LIGHT ${LONGEST_LIGHT} /* with the word */`, '');

o.push('/* ---- frc_format_count ---------------------------------------------------------- */', '');
o.push('/** n and its text: around every power of ten (where a digit or a group is added), the ends of');
o.push(' *  uint64_t, the numbers the header prints, and a fixed random stream of every bit length. */');
o.push('typedef struct oracle_pure_count {', '    uint64_t n;', '    const char *text;', '} oracle_pure_count_t;', '');
o.push(`#define ORACLE_PURE_COUNT_COUNT ${countRows.length}`);
o.push('ORACLE_PURE_DATA oracle_pure_count_t oracle_pure_count[ORACLE_PURE_COUNT_COUNT] = {');
for (const r of countRows) o.push(`    {${u64(r.n)}, ${cString(r.text)}}, /* ${r.note} */`);
o.push('};', '');

o.push('/* ---- frc_format_level_in_unit --------------------------------------------------- */', '');
o.push('/** The nine units of SPEC S15.1 by frc_unit_t: the level each sits on and its symbol. */');
o.push('typedef struct oracle_pure_unit {', '    uint8_t level;', '    const char *symbol;', '    const char *name;', '} oracle_pure_unit_t;', '');
o.push(`#define ORACLE_PURE_UNIT_COUNT ${units.length}`);
o.push('ORACLE_PURE_DATA oracle_pure_unit_t oracle_pure_units[ORACLE_PURE_UNIT_COUNT] = {');
units.forEach((u) => o.push(`    {${u.level}, ${cString(u.symbol)}, ${cString(u.name)}},${readable(u.symbol)}`));
o.push('};', '');
o.push('/**');
o.push(' * Every pair (level 0..12, unit): row level * ORACLE_PURE_UNIT_COUNT + unit.');
o.push(' * status ORACLE_PURE_OK: count = P(level of unit) / P(level) and its text, the count grouped and the');
o.push(' * unit\'s symbol. status ORACLE_PURE_ERR_ARG: the unit is coarser than the level (count 0, text "").');
o.push(' */');
o.push('typedef struct oracle_pure_in_unit {', '    uint8_t level;', '    uint8_t unit; /* frc_unit_t */', '    int8_t status;', '    uint64_t count;',
  '    const char *text;', '} oracle_pure_in_unit_t;', '');
o.push(`#define ORACLE_PURE_IN_UNIT_LEVELS ${BLINK_LEVEL + 1}`);
o.push(`#define ORACLE_PURE_IN_UNIT_COUNT ${inUnitRows.length}`);
o.push('ORACLE_PURE_DATA oracle_pure_in_unit_t oracle_pure_in_unit[ORACLE_PURE_IN_UNIT_COUNT] = {');
for (const r of inUnitRows) o.push(`    {${r.level}, ${r.unit}, ${r.status}, ${u64(r.count)}, ${cString(r.text)}},${readable(r.text)}`);
o.push('};', '');

o.push('/* ---- frc_format_level_rate ------------------------------------------------------ */', '');
o.push('/** The values of `digits` every level is written with: text[i] belongs to oracle_pure_rate_digits[i]. */');
o.push(`#define ORACLE_PURE_RATE_DIGITS ${RATE_DIGITS.length}`);
o.push(`ORACLE_PURE_DATA uint8_t oracle_pure_rate_digits[ORACLE_PURE_RATE_DIGITS] = {${RATE_DIGITS.join(', ')}};`, '');
o.push('/**');
o.push(' * One row per level 0..62. num / den is the exact rate P(level) / P(12) in lowest terms, as decimal');
o.push(' * strings (the numerator of level 62 has 42 digits): 1 / N down to the Blink, N / 1 below it.');
o.push(' * Down to the Blink the five texts are the same (digits has no say there).');
o.push(' */');
o.push('typedef struct oracle_pure_rate {', '    uint8_t level;', '    const char *num;', '    const char *den;',
  '    const char *text[ORACLE_PURE_RATE_DIGITS];', '} oracle_pure_rate_t;', '');
o.push(`#define ORACLE_PURE_RATE_COUNT ${rateRows.length}`);
o.push('ORACLE_PURE_DATA oracle_pure_rate_t oracle_pure_rate[ORACLE_PURE_RATE_COUNT] = {');
for (const r of rateRows) {
  o.push(`    {${r.level}, "${r.num}", "${r.den}", { /* ${levelName(r.level)}: ${r.text[RATE_DIGITS.indexOf(4)]} */`);
  r.text.forEach((t) => o.push(`        ${cString(t)},`));
  o.push('    }},');
}
o.push('};', '');

o.push('/* ---- frc_format_light_span ------------------------------------------------------ */', '');
o.push('/** The media of SPEC S16.3 by frc_medium_t: num / den metres per second. Index 0 is light in vacuum, c. */');
o.push('typedef struct oracle_pure_medium {', '    const char *id;', '    uint64_t num;', '    uint64_t den;', '} oracle_pure_medium_t;', '');
o.push(`#define ORACLE_PURE_MEDIUM_COUNT ${media.length}`);
o.push('ORACLE_PURE_DATA oracle_pure_medium_t oracle_pure_media[ORACLE_PURE_MEDIUM_COUNT] = {');
media.forEach((m) => o.push(`    {${cString(m.id)}, ${u64(m.speed[0])}, ${u64(m.speed[1])}},`));
o.push('};', '');
o.push('/* Which branch of the rule a text comes from. */');
o.push(`#define ORACLE_PURE_FORM_SPAN ${FORM_SPAN}        /* one Blink or more: a span of S15.4 */`);
o.push(`#define ORACLE_PURE_FORM_RUNG ${FORM_RUNG}        /* under a Blink: three digits of the largest rung that fits */`);
o.push(`#define ORACLE_PURE_FORM_RUNG_CARRY ${FORM_RUNG_CARRY}  /* ... rounded up to "1.00" of the next coarser rung */`);
o.push(`#define ORACLE_PURE_FORM_POWER ${FORM_POWER}       /* under one cycle of level 62: a power of ten of it */`);
o.push(`#define ORACLE_PURE_FORM_POWER_CARRY ${FORM_POWER_CARRY} /* ... rounded up to "1.00" of level 62 */`);
o.push(`#define ORACLE_PURE_FORM_COUNT ${FORM_NAMES.length}`, '');
o.push('/**');
o.push(' * Every pair (level 0..62, medium): row level * ORACLE_PURE_MEDIUM_COUNT + medium.');
o.push(' * text is frc_format_light_span(level, speed of the medium, 0), text_word the same with the word.');
o.push(' */');
o.push('typedef struct oracle_pure_light {', '    uint8_t level;', '    uint8_t medium; /* frc_medium_t */', '    uint8_t form;',
  '    const char *text;', '    const char *text_word;', '} oracle_pure_light_t;', '');
o.push(`#define ORACLE_PURE_LIGHT_COUNT ${lightRows.length}`);
o.push('ORACLE_PURE_DATA oracle_pure_light_t oracle_pure_light[ORACLE_PURE_LIGHT_COUNT] = {');
for (const r of lightRows)
  o.push(`    {${r.level}, ${r.medium}, ${r.form}, ${cString(r.text)}, ${cString(r.text + WORD)}}, /* ${levelName(r.level)}, ${media[r.medium].id}: ${r.text} */`);
o.push('};', '');
o.push('/**');
o.push(' * Speeds of the caller\'s own, num / den metres per second: the edges of every branch (ties, carries,');
o.push(' * faster than light, the largest span, the smallest value), every twentieth of a Blink from 1 to 10');
o.push(' * (each tenth, each tie between two, each whole number of quarter Blinks) and a fixed random stream.');
o.push(' * The first ORACLE_PURE_LIGHT_CHOSEN_COUNT rows are the edges chosen by hand.');
o.push(' * status ORACLE_PURE_ERR_OVERFLOW: the span has 2^64 Blinks or more (text "").');
o.push(' */');
o.push('typedef struct oracle_pure_light_custom {', '    uint8_t level;', '    uint64_t num;', '    uint64_t den;', '    int8_t status;', '    uint8_t form;',
  '    const char *text;', '    const char *text_word;', '} oracle_pure_light_custom_t;', '');
o.push(`#define ORACLE_PURE_LIGHT_CHOSEN_COUNT ${chosenCount}`);
o.push(`#define ORACLE_PURE_LIGHT_CUSTOM_COUNT ${customRows.length}`);
o.push('ORACLE_PURE_DATA oracle_pure_light_custom_t oracle_pure_light_custom[ORACLE_PURE_LIGHT_CUSTOM_COUNT] = {');
for (const r of customRows) {
  const ok = r.status === STATUS_OK;
  o.push(`    {${r.level}, ${u64(r.num)}, ${u64(r.den)}, ${r.status}, ${r.form}, ${cString(r.text)}, ${cString(ok ? r.text + WORD : '')}}, /* ${r.note}${ok ? `: ${r.text}` : ''} */`);
}
o.push('};', '', '#endif /* ORACLE_PURE_H */', '');

/* ---- Write ----------------------------------------------------------------- */
const file = path.join(opts.out, 'oracle_pure.h');
const text = o.join('\n');
let stale = false;
if (opts.checkOnly) {
  const onDisk = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null;
  stale = onDisk !== text;
  if (stale) console.error(`generate-pure: ${file} is stale`);
} else {
  fs.mkdirSync(opts.out, { recursive: true });
  fs.writeFileSync(file, text);
}

console.log(`SPEC       ${opts.spec}`);
console.log(`checks     ${checks} passed`);
console.log(`${opts.checkOnly ? 'checked' : 'wrote  '}    ${file}  ${byteLength(text)} bytes`);
console.log(`           counts ${countRows.length}, level in unit ${inUnitRows.length} (${inUnitRows.filter((r) => r.status !== STATUS_OK).length} refused), rates ${rateRows.length} x ${RATE_DIGITS.length} digits,`);
console.log(`           light spans ${lightRows.length} + ${customRows.length} custom (${chosenCount} chosen, ${sweepCount} twentieths of a Blink, ${customRows.length - chosenCount - sweepCount} random; ${customRows.filter((r) => r.status !== STATUS_OK).length} overflow); forms ${FORM_NAMES.map((n, i) => `${n} ${formTally[i]}`).join(', ')}`);
console.log(`           longest text: count ${LONGEST_COUNT}, in unit ${LONGEST_IN_UNIT}, rate ${LONGEST_RATE}, light span ${LONGEST_LIGHT} bytes`);
if (stale) process.exit(1);
