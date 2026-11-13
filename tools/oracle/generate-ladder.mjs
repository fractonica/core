#!/usr/bin/env node
/*
 * generate-ladder.mjs - the LADDER ORACLE of fractonica-core.
 *
 * Writes two C headers of test vectors, computed with exact BigInt arithmetic:
 *
 *   tests/vectors/oracle_ladder.h   SPEC S16: periods, frequencies, lengths, bands,
 *                                   doubles, device ticks / steps, formatted text
 *   tests/vectors/oracle_civil.h    the Gregorian translation and SPEC S2.3
 *
 * Nothing here is rounded before the last step: every value is an exact rational
 * [numerator, denominator] of BigInts and is rounded ONCE into the form the C API
 * returns (17-digit decimal, IEEE-754 double, integer tick count, text).
 *
 * The generator also checks itself while it runs, and stops on the first failure:
 *   - P(L) against the list of SPEC S16.4, the exact fractions and rounded values
 *     of SPEC S16.5, the prose figures of S16.1 and S16.3;
 *   - all 63 x 3 formatted strings of the table of SPEC S16.1;
 *   - every period, frequency and length against the frozen JavaScript reference
 *     (F.createScale), F.media, F.defaults.unixTimestamps, F.correctJulianTimestamp;
 *   - every double against the runtime's own decimal-to-double conversion;
 *   - every civil vector against the runtime's Date.
 *
 * Usage (from anywhere):
 *   node tools/oracle/generate-ladder.mjs [--spec SPEC.md] [--reference fractonica.js]
 *                                         [--no-reference] [--out tests/vectors]
 *
 * Output is deterministic: no clock, no Math.random, no locale. See LADDER.md for
 * the struct layouts and the formatting rules the vectors define.
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

/* ------------------------------------------------------------------------- */
/* Command line                                                               */
/* ------------------------------------------------------------------------- */

function parseArgs(argv) {
  const opts = { spec: null, reference: null, useReference: true, out: path.resolve(HERE, '../../tests/vectors') };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--spec') opts.spec = path.resolve(argv[++i]);
    else if (a === '--reference') opts.reference = path.resolve(argv[++i]);
    else if (a === '--no-reference') opts.useReference = false;
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
  if (opts.useReference && !opts.reference)
    throw new Error('Reference library not found; pass --reference <path> or --no-reference.');
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

const gcd = (a, b) => { a = a < 0n ? -a : a; b = b < 0n ? -b : b; while (b) [a, b] = [b, a % b]; return a; };
const reduce = ([n, d]) => { const g = gcd(n, d); return [n / g, d / g]; };
const mul = ([a, b], [c, d]) => [a * c, b * d];
const div = ([a, b], [c, d]) => [a * d, b * c];
const cmp = ([a, b], [c, d]) => { const l = a * d, r = c * b; return l < r ? -1 : l > r ? 1 : 0; };
const pow10 = (k) => (k >= 0 ? [10n ** BigInt(k), 1n] : [1n, 10n ** BigInt(-k)]);
const int = (n) => [BigInt(n), 1n];
/** floor(a / b) for b > 0 and any sign of a (BigInt division truncates). */
const floorDiv = (a, b) => { const q = a / b; return a % b < 0n ? q - 1n : q; };
const floorMod = (a, b) => a - floorDiv(a, b) * b;
/** round(n / d) with halves to even, n >= 0, d > 0. */
function roundHalfEven(n, d) {
  let q = n / d;
  const twice = 2n * (n % d);
  if (twice > d || (twice === d && (q & 1n) === 1n)) q += 1n;
  return q;
}
/** A decimal literal such as "59.9996", "-11253795486.968" or "1.2345e-30" as an exact rational. */
function decimal(text) {
  const m = /^(-?)(\d+)(?:\.(\d+))?(?:e([+-]?\d+))?$/.exec(text);
  if (!m) throw new Error(`Bad decimal: ${text}`);
  const frac = m[3] ?? '';
  const [n, d] = reduce(mul([BigInt(m[2] + frac), 1n], pow10(Number(m[4] ?? 0) - frac.length)));
  return [m[1] ? -n : n, d];
}

/* ------------------------------------------------------------------------- */
/* SPEC S3.1 / S16: the ladder                                                */
/* ------------------------------------------------------------------------- */

const SEQUENCE = [5n, 13n, 5n, 5n, 7n, 11n, 9n, 4n];
const LEVEL_COUNT = 63;
/** P[0..63]; P(63) exists only for the "depth 64" remark of S16.1 and the prefix strings. */
const P = [1n];
for (let L = 1; L <= 63; L++) P.push(P[L - 1] * SEQUENCE[(L - 1) % 8]);
const siblings = (L) => (L === 0 ? 0 : Number(SEQUENCE[(L - 1) % 8]));

/** An interval is {n, d} milliseconds. T(L) = 4 I / (1000 P(L)) seconds. */
const period = (L, I) => [4n * I.n, 1000n * I.d * P[L]];
const frequency = (L, I) => [1000n * I.d * P[L], 4n * I.n];
const length = (L, I, speed) => mul(speed, period(L, I));

const EM_DASH = String.fromCodePoint(0x2014); // SPEC's "nothing here": no band, no siblings
const BAND_NAMES = [EM_DASH, 'audible', 'radio', 'microwave', 'infrared', 'visible', 'ultraviolet', 'X-ray', 'gamma ray'];
/** Lower edge of bands 1..8, then the top of the gamma band (SPEC S16.2). */
const BAND_EDGES = ['20', '2e4', '3e8', '3e11', '4e14', '7.9e14', '3e16', '3e19', '3.4e29'].map(decimal);
/** frc_band_t of an exact frequency: an edge belongs to the band that starts there. */
function bandOf(f) {
  let passed = 0;
  for (const edge of BAND_EDGES) if (cmp(f, edge) >= 0) passed++;
  return passed === BAND_EDGES.length ? 0 : passed;
}

/* ------------------------------------------------------------------------- */
/* Rounding an exact rational into the forms of the C API                     */
/* ------------------------------------------------------------------------- */

/** frc_sci_t: {m, e} with 1e16 <= m < 1e17 and m * 10^e = the value rounded half-to-even. */
function toSci(x) {
  const [n, d] = x;
  check(n > 0n && d > 0n, 'toSci needs a positive value');
  const lo = 10n ** 16n, hi = 10n ** 17n;
  let e = String(n).length - String(d).length - 17;
  const scaled = (k) => (k >= 0 ? [n, d * 10n ** BigInt(k)] : [n * 10n ** BigInt(-k), d]);
  for (;;) {
    const [a, b] = scaled(e);
    if (a < b * lo) e--; else if (a >= b * hi) e++; else break;
  }
  const [a, b] = scaled(e);
  let m = roundHalfEven(a, b);
  if (m === hi) { m = lo; e++; }
  return { m, e };
}
const sciValue = ({ m, e }) => reduce(mul([m, 1n], pow10(e)));

/** Bits of the IEEE-754 double nearest to an exact positive rational, ties to even. */
function toDoubleBits(x) {
  const [n, d] = x;
  check(n > 0n && d > 0n, 'toDoubleBits needs a positive value');
  const lo = 1n << 52n, hi = 1n << 53n;
  let b = n.toString(2).length - d.toString(2).length - 53;
  const scaled = (k) => (k >= 0 ? [n, d << BigInt(k)] : [n << BigInt(-k), d]);
  for (;;) {
    const [p, q] = scaled(b);
    if (p < q * lo) b--; else if (p >= q * hi) b++; else break;
  }
  const [p, q] = scaled(b);
  let m = roundHalfEven(p, q);
  if (m === hi) { m = lo; b++; }
  const biased = b + 52 + 1023;
  check(biased >= 1 && biased <= 2046, 'value outside the normal double range');
  return (BigInt(biased) << 52n) | (m - lo);
}
/** The same conversion done by the runtime, from a 40-digit decimal: an independent check. */
function runtimeDoubleBits(x) {
  const [n, d] = x;
  let e = String(n).length - String(d).length - 40;
  const digits = e >= 0 ? n / (d * 10n ** BigInt(e)) : (n * 10n ** BigInt(-e)) / d;
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, Number(`${digits}e${e}`));
  return view.getBigUint64(0);
}
function checkedDoubleBits(x, what) {
  const bits = toDoubleBits(x);
  check(bits === runtimeDoubleBits(x), `double of ${what} disagrees with the runtime`);
  return bits;
}

/* ------------------------------------------------------------------------- */
/* SPEC S16.4: a device that makes a rung                                     */
/* ------------------------------------------------------------------------- */

const STATUS_OK = 0, STATUS_OVERFLOW = -3, STATUS_REFUSED = -6;
const U64_LIMIT = 1n << 64n;
/** round(x) with a half going up, x >= 0. */
const roundHalfUp = ([n, d]) => (2n * n + d) / (2n * d);

/** ticks = round(T(L) * rate). Refused under half a tick; overflow from 2^64. */
function deviceTicks(L, I, rate) {
  const x = mul(period(L, I), int(rate));
  if (cmp(x, [1n, 2n]) < 0) return { status: STATUS_REFUSED, value: 0n };
  const ticks = roundHalfUp(x);
  if (ticks >= U64_LIMIT) return { status: STATUS_OVERFLOW, value: 0n };
  return { status: STATUS_OK, value: ticks };
}
/**
 * step = round(f(L) * 2^bits / rate). Refused above rate / 2 (checked first) and
 * when the step rounds to 0. A step that is not refused is at most 2^(bits-1),
 * so with bits <= 64 it always fits: the overflow branch is unreachable and is
 * kept only to mirror the contract.
 */
function deviceStep(L, I, rate, bits) {
  const f = frequency(L, I);
  if (cmp(f, [BigInt(rate), 2n]) > 0) return { status: STATUS_REFUSED, value: 0n };
  const step = roundHalfUp(div(mul(f, [1n << BigInt(bits), 1n]), int(rate)));
  if (step === 0n) return { status: STATUS_REFUSED, value: 0n };
  if (step >= U64_LIMIT) return { status: STATUS_OVERFLOW, value: 0n };
  return { status: STATUS_OK, value: step };
}

/* ------------------------------------------------------------------------- */
/* The formatting rule of frc_format_quantity (see LADDER.md, "Formatting")   */
/* ------------------------------------------------------------------------- */

const QUANTITY_TIME = 0, QUANTITY_FREQUENCY = 1, QUANTITY_LENGTH = 2;
// Written as code points, because several of these have look-alikes.
const cp = (...codes) => String.fromCodePoint(...codes);
const MICRO = cp(0x00B5); // the micro SIGN, as SPEC.md prints it (not the Greek letter mu, U+03BC)
const TIMES = cp(0x00D7); // the multiplication sign
/** Superscript minus and digits. 1, 2 and 3 live in Latin-1; the others in U+207x. */
const SUPERSCRIPT = {
  '-': cp(0x207B), 0: cp(0x2070), 1: cp(0x00B9), 2: cp(0x00B2), 3: cp(0x00B3),
  4: cp(0x2074), 5: cp(0x2075), 6: cp(0x2076), 7: cp(0x2077), 8: cp(0x2078), 9: cp(0x2079),
};
const superscript = (k) => [...String(k)].map((c) => SUPERSCRIPT[c]).join('');

const decimalUnits = (names, firstExp) => names.map((name, i) => ({ name, size: pow10(firstExp + 3 * i) }));
/**
 * A ladder is its units in ascending order, each with its exact size in the base
 * unit, and a ceiling: values from `top.limit` up are written in powers of ten of
 * `top.unit`. Values below the first unit are written in powers of ten of `base`.
 */
const LADDERS = {
  [QUANTITY_TIME]: {
    base: 's',
    units: [
      ...decimalUnits(['ys', 'zs', 'as', 'fs', 'ps', 'ns', `${MICRO}s`, 'ms', 's'], -24),
      { name: 'min', size: int(60) }, { name: 'h', size: int(3600) }, { name: 'd', size: int(86400) },
      { name: 'y', size: int(31557600) }, // the Julian year, 365.25 d
    ],
    top: { unit: 'y', size: int(31557600), limit: int(31557600000) }, // from 1000 y: "d.ddd x 10^n y"
  },
  [QUANTITY_FREQUENCY]: {
    base: 'Hz',
    units: decimalUnits(['pHz', 'nHz', `${MICRO}Hz`, 'mHz', 'Hz', 'kHz', 'MHz', 'GHz', 'THz', 'PHz', 'EHz', 'ZHz', 'YHz'], -12),
    top: { unit: 'Hz', size: int(1), limit: pow10(27) },
  },
  [QUANTITY_LENGTH]: {
    base: 'm',
    units: decimalUnits(['ym', 'zm', 'am', 'fm', 'pm', 'nm', `${MICRO}m`, 'mm', 'm', 'km', 'Mm', 'Gm', 'Tm', 'Pm'], -24),
    top: { unit: 'm', size: int(1), limit: pow10(18) },
  },
};

/** "1", "1.0", "1.000": the number one with `digits` significant digits. */
const oneText = (digits) => (digits > 1 ? `1.${'0'.repeat(digits - 1)}` : '1');

/** x = m * 10^k with 1 <= m < 10; m rounded half-to-even to `digits` significant digits. */
function scientific(x, digits) {
  const [n, d] = x;
  let k = String(n).length - String(d).length;
  while (cmp(x, pow10(k)) < 0) k--;
  while (cmp(x, pow10(k + 1)) >= 0) k++;
  const [a, b] = mul(x, pow10(digits - 1 - k));
  let m = roundHalfEven(a, b);
  if (m === 10n ** BigInt(digits)) { m /= 10n; k++; } // 9.9996 -> 1.000 x 10^(k+1)
  const s = String(m);
  return { k, isOne: m === 10n ** BigInt(digits - 1), mantissa: digits > 1 ? `${s[0]}.${s.slice(1)}` : s };
}

/**
 * A mantissa 1 <= m < 1000 rounded half-to-even to `digits` significant digits.
 * Returns its text and its exact rounded value (for the carry test).
 *   digits 4:  310.92.. -> "310.9"   9.9996 -> "10.00"   digits 1:  439.4 -> "400"
 */
function roundMantissa(m, digits) {
  const intDigits = String(m[0] / m[1]).length;
  let decimals = digits - intDigits; // negative: round to tens or hundreds
  const [a, b] = mul(m, pow10(decimals));
  let r = roundHalfEven(a, b);
  // Gained an integer digit (9.9996 -> 10.00, 99.96 -> 100.0): drop one decimal to keep `digits` digits.
  if (r === 10n ** BigInt(digits) && decimals > 0) { r /= 10n; decimals--; }
  if (decimals > 0) {
    const s = String(r).padStart(decimals + 1, '0');
    return { text: `${s.slice(0, -decimals)}.${s.slice(-decimals)}`, value: [r, 10n ** BigInt(decimals)] };
  }
  const value = r * 10n ** BigInt(-decimals); // 95 at 1 digit -> 10 x 10 = "100"
  return { text: String(value), value: [value, 1n] };
}

/** The text of an exact positive value `x` (in s, Hz or m). digits 1..9, 0 means 4. */
function formatQuantity(x, quantity, digits) {
  if (digits === 0) digits = 4;
  check(digits >= 1 && digits <= 9, 'digits must be 0..9');
  const { base, units, top } = LADDERS[quantity];
  if (cmp(x, units[0].size) < 0) { // below the ladder: powers of ten of the base unit
    const s = scientific(x, digits);
    // Rounding may carry the value exactly onto the first unit: it is then written in that unit.
    if (s.isOne && cmp(pow10(s.k), units[0].size) === 0) return `${oneText(digits)} ${units[0].name}`;
    return `${s.mantissa} ${TIMES} 10${superscript(s.k)} ${base}`;
  }
  if (cmp(x, top.limit) >= 0) { // above the ladder: powers of ten of the top unit
    const s = scientific(div(x, top.size), digits);
    return `${s.mantissa} ${TIMES} 10${superscript(s.k)} ${top.unit}`;
  }
  let i = 0;
  while (i + 1 < units.length && cmp(x, units[i + 1].size) >= 0) i++; // the unit is chosen on the UNROUNDED value
  const next = i + 1 < units.length ? units[i + 1].size : top.limit;
  const rounded = roundMantissa(div(x, units[i].size), digits);
  if (cmp(mul(rounded.value, units[i].size), next) >= 0) { // carry: 999.96 ms -> 1.000 s, 59.9996 s -> 1.000 min
    if (i + 1 < units.length) return `${oneText(digits)} ${units[i + 1].name}`;
    const s = scientific(div(top.limit, top.size), digits); // 999.96 YHz -> 1.000 x 10^27 Hz
    return `${s.mantissa} ${TIMES} 10${superscript(s.k)} ${top.unit}`;
  }
  return `${rounded.text} ${units[i].name}`;
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

function readSpec(specPath) {
  const spec = fs.readFileSync(specPath, 'utf8');
  check(/^Version 2\.2\.0\./m.test(spec.slice(0, 400)), 'SPEC.md is not version 2.2.0');

  const ladder = tableRows(section(spec, '### 16.1 ', '### 16.2 '), 'Depth').map((c) => ({
    depth: Number(c[0]), level: Number(c[1]), name: c[2], unit: c[3], siblings: c[4],
    period: c[5], frequency: c[6], band: c[7], length: c[8],
  }));
  check(ladder.length === LEVEL_COUNT, `SPEC S16.1 table has ${ladder.length} rows, expected 63`);
  ladder.forEach((row, i) => check(row.level === i && row.depth === i + 1, `SPEC S16.1 row ${i} is out of order`));

  const media = tableRows(section(spec, '### 16.3 ', '### 16.4 '), 'Id').map((c) => ({
    id: c[0].replace(/`/g, ''), name: c[1], phase: c[2], speed: decimal(c[3].replace(/,/g, '')),
  }));
  check(media.length === 7, 'SPEC S16.3 lists 7 media');

  // S16.2: | Band | From | Up to, not including |
  const bands = tableRows(section(spec, '### 16.2 ', '### 16.3 '), 'Band').map((c) => ({ name: c[0], from: c[1], upTo: c[2] }));
  check(bands.length === 8, 'SPEC S16.2 lists 8 bands');

  const prefixes = [...section(spec, '### 16.4 ', '### 16.5 ').matchAll(/^P\((\d+)\) = (\d+)$/gm)].map((m) => [Number(m[1]), m[2]]);
  check(prefixes.length === 64, 'SPEC S16.4 lists P(0)..P(63)');

  const block = /```\n([\s\S]*?)```/.exec(section(spec, '## 14. ', '## 15. '));
  const saros141 = block[1].trim().split(/\s+/).map((s) => {
    const m = /^(-?)(\d+)\.(\d{3})$/.exec(s);
    check(m, `SPEC S14 value is not seconds with 3 decimals: ${s}`);
    return BigInt(m[2] + m[3]) * (m[1] ? -1n : 1n); // Unix milliseconds
  });
  check(saros141.length === 70, 'SPEC S14 lists 70 eclipses');

  return { ladder, media, bands, bandFrom: bands.map((b) => b.from), prefixes, saros141 };
}

/* ------------------------------------------------------------------------- */
/* Proleptic Gregorian calendar (days since 1970-01-01), own arithmetic       */
/* ------------------------------------------------------------------------- */

/** Days from civil, any year; month 1..12, day may run past the month. */
function daysFromCivil(y, m, d) {
  y = m <= 2n ? y - 1n : y;
  const era = floorDiv(y, 400n);
  const yoe = y - era * 400n;
  const doy = (153n * (m > 2n ? m - 3n : m + 9n) + 2n) / 5n + d - 1n;
  const doe = yoe * 365n + yoe / 4n - yoe / 100n + doy;
  return era * 146097n + doe - 719468n;
}
function civilFromDays(days) {
  const z = days + 719468n;
  const era = floorDiv(z, 146097n);
  const doe = z - era * 146097n;
  const yoe = (doe - doe / 1460n + doe / 36524n - doe / 146096n) / 365n;
  const doy = doe - (365n * yoe + yoe / 4n - yoe / 100n);
  const mp = (5n * doy + 2n) / 153n;
  const day = doy - (153n * mp + 2n) / 5n + 1n;
  const month = mp < 10n ? mp + 3n : mp - 9n;
  return { year: yoe + era * 400n + (month <= 2n ? 1n : 0n), month, day };
}
const US_PER_DAY = 86400000000n;

/** frc_civil_from_time: the fields of instant t (Unix microseconds) at a fixed offset. */
function civilFromTime(tUs, offsetS) {
  const local = tUs + BigInt(offsetS) * 1000000n;
  const days = floorDiv(local, US_PER_DAY);
  const within = local - days * US_PER_DAY;
  const { year, month, day } = civilFromDays(days);
  return {
    year, month, day,
    hour: within / 3600000000n, minute: (within / 60000000n) % 60n, second: (within / 1000000n) % 60n,
    microsecond: within % 1000000n,
    weekday: floorMod(days + 3n, 7n), // 1970-01-01 was a Thursday; 0 = Monday
  };
}
/**
 * frc_time_from_civil, linear in every field: the month is first carried into the
 * year (month 13 is January of the next year, month 0 December of the one before),
 * then days, hours, minutes, seconds and microseconds are simply added.
 */
function timeFromCivil(c, offsetS) {
  const m0 = BigInt(c.month) - 1n;
  const year = BigInt(c.year) + floorDiv(m0, 12n);
  const month = floorMod(m0, 12n) + 1n;
  const days = daysFromCivil(year, month, 1n) + BigInt(c.day) - 1n;
  return days * US_PER_DAY + BigInt(c.hour) * 3600000000n + BigInt(c.minute) * 60000000n
    + BigInt(c.second) * 1000000n + BigInt(c.microsecond) - BigInt(offsetS) * 1000000n;
}
const pad = (v, n) => String(v).padStart(n, '0');
/**
 * frc_format_civil. Years 0..9999 are four digits; a negative year is "-" and at
 * least four digits ("-0044"); a year above 9999 is "+" and its digits ("+12345").
 * Milliseconds are the microseconds truncated. Offset 0 is "Z", any other "+HH:MM" / "-HH:MM".
 */
function formatCivil(c, offsetS) {
  const y = c.year < 0n ? `-${pad(-c.year, 4)}` : c.year > 9999n ? `+${c.year}` : pad(c.year, 4);
  const abs = Math.abs(offsetS);
  check(abs % 60 === 0, 'vector offsets are whole minutes');
  const zone = offsetS === 0 ? 'Z' : `${offsetS < 0 ? '-' : '+'}${pad(Math.floor(abs / 3600), 2)}:${pad((abs % 3600) / 60, 2)}`;
  return `${y}-${pad(c.month, 2)}-${pad(c.day, 2)}T${pad(c.hour, 2)}:${pad(c.minute, 2)}:${pad(c.second, 2)}.${pad(c.microsecond / 1000n, 3)}${zone}`;
}
/** SPEC S2.3: read the proleptic-Gregorian fields of a Unix-millisecond value as a Julian date. */
function julianFieldsToUnixMs(ms) {
  const days = floorDiv(ms, 86400000n);
  const within = ms - days * 86400000n;
  const { year, month, day } = civilFromDays(days);
  const a = (14n - month) / 12n;
  const y = year + 4800n - a;
  const m = month + 12n * a - 3n;
  const jdn = day + (153n * m + 2n) / 5n + 365n * y + floorDiv(y, 4n) - 32083n;
  return (jdn - 2440588n) * 86400000n + within;
}
const REFORM_MS = -12219292800000n; // 1582-10-15T00:00:00Z, the first Gregorian day

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
const i64 = (v) => `INT64_C(${v})`;
const hex64 = (v) => `UINT64_C(0x${v.toString(16).toUpperCase().padStart(16, '0')})`;
const cSci = ({ m, e }) => `{${u64(m)}, ${e}}`;

function banner(title, lines) {
  return [
    '/*',
    ` * ${title}`,
    ' *',
    ' * GENERATED by tools/oracle/generate-ladder.mjs. DO NOT EDIT: change the generator and run it again.',
    ' * Source of truth: SPEC.md 2.2.0. Every value is rounded once from an exact BigInt rational.',
    ' * Layouts and rules: tools/oracle/LADDER.md.',
    ' *',
    ...lines.map((l) => ` * ${l}`.trimEnd()),
    ' */',
  ].join('\n');
}

/* ------------------------------------------------------------------------- */
/* Main                                                                       */
/* ------------------------------------------------------------------------- */

const opts = parseArgs(process.argv.slice(2));
const spec = readSpec(opts.spec);

let F = null;
if (opts.useReference) {
  await import(pathToFileURL(opts.reference).href);
  F = globalThis.Fractonica;
  check(F && typeof F.createScale === 'function', 'reference library did not load');
}

/* ---- P(L) ---------------------------------------------------------------- */
for (const [L, text] of spec.prefixes) checkEqual(String(P[L]), text, `P(${L}) of SPEC S16.4`);

/* ---- Media --------------------------------------------------------------- */
const MEDIA = spec.media;
checkEqual(MEDIA.map((m) => m.id).join(' '), 'vacuum bounded diamond water air nerve chemical', 'media order');
if (F) MEDIA.forEach((m, i) => {
  const r = F.media[i];
  check(r.id === m.id && r.name === m.name && r.phase === m.phase
    && cmp(m.speed, [r.speed.numerator, r.speed.denominator]) === 0, `medium ${m.id} differs from the reference`);
});
const VACUUM = MEDIA[0].speed;

/* ---- Bands --------------------------------------------------------------- */
{
  /** "20 Hz", "300 MHz", "3.4 × 10²⁹ Hz" as an exact number of hertz. */
  const hertz = (text) => {
    const plain = /^([\d.]+) (k|M|G|T|P|E|)Hz$/.exec(text);
    if (plain) return mul(decimal(plain[1]), pow10(3 * ' kMGTPE'.indexOf(plain[2] || ' ')));
    const power = new RegExp(`^([\\d.]+) ${TIMES} 10(.+) Hz$`).exec(text);
    check(power, `SPEC S16.2 frequency not understood: ${text}`);
    const exponent = [...power[2]].map((c) => Object.keys(SUPERSCRIPT).find((k) => SUPERSCRIPT[k] === c)).join('');
    return mul(decimal(power[1]), pow10(Number(exponent)));
  };
  spec.bands.forEach((b, i) => {
    checkEqual(b.name, BAND_NAMES[i + 1], `name of band ${i + 1}`);
    check(cmp(hertz(b.from), BAND_EDGES[i]) === 0, `lower edge of ${b.name}`);
    check(cmp(hertz(b.upTo), BAND_EDGES[i + 1]) === 0, `upper edge of ${b.name}`);
  });
}

/* ---- Intervals ----------------------------------------------------------- */
if (F) {
  const seconds = F.defaults.unixTimestamps.map(decimal);
  check(seconds.length === 70, 'reference lists 70 eclipses');
  seconds.forEach((r, k) => check(cmp(mul(r, int(1000)), int(spec.saros141[k])) === 0, `eclipse ${k} of SPEC S14 differs from the reference`));
}
const E = spec.saros141;
const REFERENCE_MS = 568972877000n;
check(E[23] - E[22] === REFERENCE_MS, 'the reference interval is E[23] - E[22]');

/** The reciprocal of 439.3883434 pHz, in whole milliseconds (half up): the reference library's nominal Alpha. */
const ALPHA_RECIPROCAL_MS = roundHalfUp(div(int(1000), decimal('439.3883434e-12')));
/** Four theoretical Saros of 6585.3211 days. */
const ALPHA_THEORY_MS = (() => { const [n, d] = reduce(mul(decimal('6585.3211'), int(4n * 86400000n))); check(d === 1n, 'whole ms'); return n; })();

const intervals = [
  { name: 'reference', note: 'FRC_INTERVAL_REFERENCE_MS, Saros 141 ordinal 22 -> 23 (2010-2028)', n: REFERENCE_MS, d: 1n, alpha: 0n },
  { name: 'mean', note: 'frc_interval_mean(&FRC_SAROS_141) = (E[69] - E[0]) / 69', n: E[69] - E[0], d: 69n, alpha: 0n },
  ...Array.from({ length: 69 }, (_, k) => ({
    name: `saros141[${k}]`, note: `E[${k + 1}] - E[${k}]`, n: E[k + 1] - E[k], d: 1n, alpha: 0n,
  })),
  { name: 'alpha-reciprocal', note: 'frc_interval_of_alpha_ms: Alpha = 1 / 439.3883434 pHz, whole ms', n: ALPHA_RECIPROCAL_MS, d: 4n, alpha: ALPHA_RECIPROCAL_MS },
  { name: 'alpha-theory', note: 'frc_interval_of_alpha_ms: Alpha = 4 x 6585.3211 days', n: ALPHA_THEORY_MS, d: 4n, alpha: ALPHA_THEORY_MS },
];
const IV_REFERENCE = 0, IV_MEAN = 1;
checkEqual(`${intervals[IV_MEAN].n}/${intervals[IV_MEAN].d}`, '39259007522784/69', 'mean interval of SPEC S16.5');
intervals.forEach((I) => check(I.n > 0n && I.n < U64_LIMIT && I.d < U64_LIMIT, `interval ${I.name} fits 64 bits`));

/* ---- Cross-check of the exact values against the reference library ------- */
if (F) {
  const same = (x, r, what) => check(cmp(x, [r.numerator, r.denominator]) === 0, `${what} differs from the reference`);
  for (const I of intervals) {
    const scale = F.createScale({ basePeriod: F.rational(4n * I.n, 1000n * I.d) });
    for (let L = 0; L < LEVEL_COUNT; L++) {
      same(period(L, I), scale.period(L), `T(${L}) on ${I.name}`);
      same(frequency(L, I), scale.frequency(L), `f(${L}) on ${I.name}`);
      for (const m of MEDIA) same(length(L, I, m.speed), scale.lengthExact(L, m.id), `length(${L}, ${m.id}) on ${I.name}`);
    }
  }
}

/* ---- SPEC S16.5 and the prose figures ------------------------------------ */
{
  const R = intervals[IV_REFERENCE], M = intervals[IV_MEAN];
  const fraction = (x) => { const [n, d] = reduce(x); return `${n}/${d}`; };
  const speed = (id) => MEDIA.find((m) => m.id === id).speed;
  checkEqual(fraction(period(12, R)), '51724807/166359375', 'T(12)');
  checkEqual(fraction(frequency(13, R)), '1164515625/51724807', 'f(13)');
  checkEqual(fraction(frequency(62, R)), '107008321717714395645982788862060546875000000000000/51724807', 'f(62)');
  checkEqual(fraction(length(12, R, VACUUM)), '2215243861443658/23765625', 'length(12)');
  checkEqual(fraction(length(12, R, speed('air'))), '2534515543/23765625', 'length(12, air)');
  checkEqual(fraction(length(12, R, speed('nerve'))), '413798456/11090625', 'length(12, nerve)');
  checkEqual(fraction(length(12, R, speed('chemical'))), '51724807/166359375000', 'length(12, chemical)');
  checkEqual(fraction(length(29, R, VACUUM)), '1107621930721829/1687759133607421875000', 'length(29)');
  checkEqual(fraction(frequency(12, M)), '250529296875/77894856196', 'f(12) on the mean interval');

  checkEqual(formatQuantity(period(12, R), QUANTITY_TIME, 6), '310.922 ms', 'S16.5 T(12)');
  checkEqual(formatQuantity(frequency(13, R), QUANTITY_FREQUENCY, 6), '22.5137 Hz', 'S16.5 f(13)');
  checkEqual(formatQuantity(frequency(62, R), QUANTITY_FREQUENCY, 5), `2.0688 ${TIMES} 10${superscript(42)} Hz`, 'S16.5 f(62)');
  checkEqual(formatQuantity(length(12, R, VACUUM), QUANTITY_LENGTH, 6), '93.2121 Mm', 'S16.5 length(12): 93,212.1 km');
  checkEqual(formatQuantity(length(12, R, speed('air')), QUANTITY_LENGTH, 6), '106.646 m', 'S16.5 length(12, air)');
  checkEqual(formatQuantity(length(12, R, speed('nerve')), QUANTITY_LENGTH, 6), '37.3107 m', 'S16.5 length(12, nerve)');
  checkEqual(formatQuantity(length(12, R, speed('chemical')), QUANTITY_LENGTH, 6), `310.922 ${MICRO}m`, 'S16.5 length(12, chemical)');
  checkEqual(formatQuantity(length(29, R, VACUUM), QUANTITY_LENGTH, 6), '656.268 nm', 'S16.5 length(29)');
  checkEqual(formatQuantity(frequency(12, M), QUANTITY_FREQUENCY, 6), '3.21625 Hz', 'S16.5 mean f(12)');
  checkEqual(formatQuantity(period(12, M), QUANTITY_TIME, 6), '310.921 ms', 'S16.5 mean T(12)');

  // S16.1: "Depth 64 would last 5.371 x 10^-44 s ... light would cross 1.610 x 10^-35 m".
  const T63 = [4n * R.n, 1000n * R.d * P[63]];
  checkEqual(formatQuantity(T63, QUANTITY_TIME, 4), `5.371 ${TIMES} 10${superscript(-44)} s`, 'S16.1 depth 64 period');
  checkEqual(formatQuantity(mul(VACUUM, T63), QUANTITY_LENGTH, 4), `1.610 ${TIMES} 10${superscript(-35)} m`, 'S16.1 depth 64 length');
  // S16.3: 72.12 light-years, 93,212 km, 106.6 m, 37.31 m, 656.3 nm.
  checkEqual(roundMantissa(div(length(0, R, VACUUM), int(9460730472580800n)), 4).text, '72.12', 'S16.3 Alpha in light-years');
  checkEqual(formatQuantity(length(12, R, VACUUM), QUANTITY_LENGTH, 5), '93.212 Mm', 'S16.3 Blink of light');
  checkEqual(formatQuantity(length(12, R, speed('air')), QUANTITY_LENGTH, 4), '106.6 m', 'S16.3 Blink of sound');
  checkEqual(formatQuantity(length(12, R, speed('nerve')), QUANTITY_LENGTH, 4), '37.31 m', 'S16.3 Blink along a nerve');
  checkEqual(formatQuantity(length(29, R, VACUUM), QUANTITY_LENGTH, 4), '656.3 nm', 'S16.3 red light');
}

/* ---- The table of SPEC S16.1 against the formatting rule ----------------- */
const GREEK = 'αβγδεζηθικλμνξοπρστυφχψω';
for (const row of spec.ladder) {
  const L = row.level, R = intervals[IV_REFERENCE];
  checkEqual(formatQuantity(period(L, R), QUANTITY_TIME, 4), row.period, `S16.1 period of level ${L}`);
  checkEqual(formatQuantity(frequency(L, R), QUANTITY_FREQUENCY, 4), row.frequency, `S16.1 frequency of level ${L}`);
  checkEqual(formatQuantity(length(L, R, VACUUM), QUANTITY_LENGTH, 4), row.length, `S16.1 length of level ${L}`);
  checkEqual(BAND_NAMES[bandOf(frequency(L, R))], row.band, `S16.1 band of level ${L}`);
  checkEqual(L === 0 ? EM_DASH : String(siblings(L)), row.siblings, `S16.1 siblings of level ${L}`);
  checkEqual(GREEK[L % 24] + (L >= 24 ? String(Math.floor(L / 24)) : ''), row.name, `S16.1 name of level ${L}`);
}

/* ---- Ladder vectors ------------------------------------------------------ */
const sciDoubles = []; // {sci, bits}: for frc_sci_to_double
let sciDoubleDiffers = 0;
function ladderRow(iv, L, full) {
  const I = intervals[iv];
  const T = period(L, I), f = frequency(L, I), len = length(L, I, VACUUM);
  const row = {
    iv, L, band: bandOf(f),
    period: toSci(T), frequency: toSci(f),
    lengths: (full ? MEDIA : MEDIA.slice(0, 1)).map((m) => toSci(length(L, I, m.speed))),
    periodBits: checkedDoubleBits(T, `T(${L})`), frequencyBits: checkedDoubleBits(f, `f(${L})`), lengthBits: checkedDoubleBits(len, `length(${L})`),
  };
  if (full) {
    for (const [sci, exactBits] of [[row.period, row.periodBits], [row.frequency, row.frequencyBits], [row.lengths[0], row.lengthBits]]) {
      const bits = checkedDoubleBits(sciValue(sci), 'a 17-digit decimal');
      if (bits !== exactBits) sciDoubleDiffers++;
      sciDoubles.push({ sci, bits });
    }
  }
  return row;
}
const fullRows = [];
for (const iv of [IV_REFERENCE, IV_MEAN]) for (let L = 0; L < LEVEL_COUNT; L++) fullRows.push(ladderRow(iv, L, true));
const SPOT_LEVELS = [0, 6, 12, 13, 29, 30, 31, 47, 62];
const spotRows = [];
for (let iv = 2; iv < intervals.length; iv++) for (const L of SPOT_LEVELS) spotRows.push(ladderRow(iv, L, false));

/* ---- Band edges: intervals that put a level exactly on a boundary -------- */
// f(L) = 1000 P(L) / (4 I) = B  <=>  I = 1000 P(L) / (4 B) milliseconds.
// Only the edges a real catalogue could come near are kept: 1e11 .. 1e13 ms.
const bandEdgeRows = [];
const edgeName = (edgeIndex) => (edgeIndex + 1 < BAND_NAMES.length ? `${BAND_NAMES[edgeIndex + 1]} from ${spec.bandFrom[edgeIndex]}` : 'gamma ray up to 3.4e29 Hz');
/** The continued-fraction convergents p/q of n/d: the closest fractions with small terms, alternately below and above. */
function convergents(n, d) {
  const out = [];
  let [p0, q0, p1, q1] = [0n, 1n, 1n, 0n];
  while (d !== 0n) {
    const a = n / d;
    [n, d] = [d, n - a * d];
    [p0, q0, p1, q1] = [p1, q1, a * p1 + p0, a * q1 + q0];
    out.push([p1, q1]);
  }
  return out;
}
BAND_EDGES.forEach((B, edgeIndex) => {
  let found = 0;
  for (let L = 0; L < LEVEL_COUNT; L++) {
    const exact = reduce(div([1000n * P[L], 4n], B));
    if (cmp(exact, pow10(11)) < 0 || cmp(exact, pow10(13)) > 0) continue;
    const add = (n, d, what) => bandEdgeRows.push({ I: { n, d }, L, band: bandOf(frequency(L, { n, d })), note: `${edgeName(edgeIndex)}: ${what}` });
    const [n, d] = exact;
    if (n < 1n << 63n && d < 1n << 63n) {
      add(n, d, 'exactly on the edge');
      add(n + 1n, d, 'a hair slower than the edge');
      add(n - 1n, d, 'a hair faster than the edge');
    } else {
      // The edge itself does not fit 64 bits: take the two closest fractions that do, one on each side.
      const fits = convergents(n, d).filter(([p, q]) => p < 1n << 63n && q < 1n << 32n);
      const [below, above] = fits.slice(-2).sort((x, y) => cmp(x, y));
      check(cmp(below, exact) < 0 && cmp(above, exact) > 0, 'convergents straddle the edge');
      add(above[0], above[1], 'the closest 64-bit interval slower than the edge');
      add(below[0], below[1], 'the closest 64-bit interval faster than the edge');
    }
    found++;
  }
  check(found > 0, `band edge ${edgeIndex} has a vector`);
});
{
  // On an edge the band is the one that starts there; a hair slower is the band below; faster, the same band.
  const seen = new Set(bandEdgeRows.map((r) => r.band));
  for (let band = 0; band <= 8; band++) check(seen.has(band), `band ${band} appears among the edge vectors`);
}

/* ---- Formatted text ------------------------------------------------------ */
const formatRows = [];
/** One vector for frc_format_period / _frequency / _length; both input paths must agree. */
function addFormat(iv, L, quantity, medium, digits) {
  const I = intervals[iv];
  const exact = quantity === QUANTITY_TIME ? period(L, I) : quantity === QUANTITY_FREQUENCY ? frequency(L, I) : length(L, I, MEDIA[medium].speed);
  const text = formatQuantity(exact, quantity, digits);
  // The same text must come out of the 17-digit value (frc_period + frc_format_quantity).
  checkEqual(formatQuantity(sciValue(toSci(exact)), quantity, digits), text, `format via frc_sci_t, level ${L}, quantity ${quantity}, digits ${digits}`);
  formatRows.push({ iv, L, quantity, medium, digits, text });
}
for (const iv of [IV_REFERENCE, IV_MEAN]) {
  for (let L = 0; L < LEVEL_COUNT; L++)
    for (const quantity of [QUANTITY_TIME, QUANTITY_FREQUENCY, QUANTITY_LENGTH]) addFormat(iv, L, quantity, 0, 4);
  for (const L of [12, 29]) for (let medium = 1; medium < MEDIA.length; medium++) addFormat(iv, L, QUANTITY_LENGTH, medium, 4);
}
for (const digits of [0, 1, 2, 3, 6, 9])
  for (const L of [0, 2, 6, 9, 12, 13, 29, 41, 44, 51, 62])
    for (const quantity of [QUANTITY_TIME, QUANTITY_FREQUENCY, QUANTITY_LENGTH]) addFormat(IV_REFERENCE, L, quantity, 0, digits);
for (const digits of [1, 2, 3, 6, 9]) for (const medium of [4, 6]) addFormat(IV_MEAN, 12, QUANTITY_LENGTH, medium, digits);
// The first 189 rows are the SPEC table itself.
spec.ladder.forEach((row, L) => {
  checkEqual(formatRows[3 * L].text, row.period, 'format row order');
  checkEqual(formatRows[3 * L + 1].text, row.frequency, 'format row order');
  checkEqual(formatRows[3 * L + 2].text, row.length, 'format row order');
});

/* ---- frc_format_quantity on hand-made values: the edges of the rule ------ */
const X = TIMES, sup = superscript;
const QUANTITY_CASES = [
  // [quantity, exact decimal value in the base unit, digits, expected text]
  // -- the unit is chosen on the unrounded value; exact unit sizes read 1.000
  [QUANTITY_TIME, '1', 4, '1.000 s'], [QUANTITY_TIME, '0.001', 4, '1.000 ms'], [QUANTITY_TIME, '60', 4, '1.000 min'],
  [QUANTITY_TIME, '3600', 4, '1.000 h'], [QUANTITY_TIME, '86400', 4, '1.000 d'], [QUANTITY_TIME, '31557600', 4, '1.000 y'],
  [QUANTITY_TIME, '1e-24', 4, '1.000 ys'], [QUANTITY_TIME, '31557599', 4, '365.2 d'], [QUANTITY_TIME, '59.994', 4, '59.99 s'],
  [QUANTITY_TIME, '12.5', 4, '12.50 s'], [QUANTITY_TIME, '750', 4, '12.50 min'], [QUANTITY_TIME, '45000', 4, '12.50 h'],
  [QUANTITY_TIME, '8640000', 4, '100.0 d'], [QUANTITY_TIME, '3155760000', 4, '100.0 y'], [QUANTITY_TIME, '31526042400', 4, '999.0 y'],
  // -- halves go to even, on the value in the chosen unit
  [QUANTITY_TIME, '1.2345', 4, '1.234 s'], [QUANTITY_TIME, '1.2355', 4, '1.236 s'], [QUANTITY_TIME, '1.2345000000000001', 4, '1.235 s'],
  [QUANTITY_TIME, '1.2344999999999999', 4, '1.234 s'], [QUANTITY_TIME, '2.5', 1, '2 s'], [QUANTITY_TIME, '3.5', 1, '4 s'],
  [QUANTITY_TIME, '0.25', 2, '250 ms'], [QUANTITY_TIME, '0.25', 1, '200 ms'], [QUANTITY_TIME, '0.35', 1, '400 ms'],
  [QUANTITY_TIME, '38957857.2', 4, '1.234 y'], [QUANTITY_TIME, '38989414.8', 4, '1.236 y'], [QUANTITY_TIME, '74.07', 4, '1.234 min'],
  [QUANTITY_TIME, '4444.2', 4, '1.234 h'], [QUANTITY_TIME, '106660.8', 4, '1.234 d'], [QUANTITY_TIME, '0.99985', 4, '999.8 ms'],
  // -- a mantissa that gains a digit
  [QUANTITY_TIME, '9.9996', 4, '10.00 s'], [QUANTITY_TIME, '0.099996', 4, '100.0 ms'], [QUANTITY_TIME, '9.5', 1, '10 s'],
  [QUANTITY_TIME, '0.0996', 2, '100 ms'], [QUANTITY_TIME, '0.095', 1, '100 ms'], [QUANTITY_TIME, '0.085', 1, '80 ms'],
  // -- carry into the next unit
  [QUANTITY_TIME, '0.99996', 4, '1.000 s'], [QUANTITY_TIME, '0.99995', 4, '1.000 s'], [QUANTITY_TIME, '0.00099996', 4, '1.000 ms'],
  [QUANTITY_TIME, '59.9996', 4, '1.000 min'], [QUANTITY_TIME, '59.995', 4, '1.000 min'], [QUANTITY_TIME, '55', 1, '1 min'],
  [QUANTITY_TIME, '54', 1, '50 s'], [QUANTITY_TIME, '3599.99', 4, '1.000 h'], [QUANTITY_TIME, '3599.6', 4, '59.99 min'],
  [QUANTITY_TIME, '86399.9', 4, '1.000 d'], [QUANTITY_TIME, '86300', 4, '23.97 h'], [QUANTITY_TIME, '84600', 2, '1.0 d'],
  [QUANTITY_TIME, '84000', 2, '23 h'], [QUANTITY_TIME, '31557599.99', 6, '1.00000 y'], [QUANTITY_TIME, '31557599', 7, '1.000000 y'],
  [QUANTITY_TIME, '31557599', 9, '365.249988 d'], [QUANTITY_TIME, '31104000', 1, '1 y'], [QUANTITY_TIME, '29376000', 1, '300 d'],
  [QUANTITY_TIME, '31536000', 2, '360 d'], [QUANTITY_TIME, '31556000', 2, '1.0 y'], [QUANTITY_TIME, '31556000', 3, '365 d'],
  // -- above the ladder: powers of ten of the year
  [QUANTITY_TIME, '31557600000', 4, `1.000 ${X} 10${sup(3)} y`], [QUANTITY_TIME, '31556337696', 4, `1.000 ${X} 10${sup(3)} y`],
  [QUANTITY_TIME, '38957857200', 4, `1.234 ${X} 10${sup(3)} y`], [QUANTITY_TIME, '7.3786976294838206e16', 4, `2.338 ${X} 10${sup(9)} y`],
  [QUANTITY_TIME, '3.15576e17', 2, `1.0 ${X} 10${sup(10)} y`], [QUANTITY_TIME, '31557600000', 1, `1 ${X} 10${sup(3)} y`],
  // -- below the ladder: powers of ten of the second; a carry onto 1 ys is written in ys
  [QUANTITY_TIME, '9.9994e-25', 4, `9.999 ${X} 10${sup(-25)} s`], [QUANTITY_TIME, '9.9996e-25', 4, '1.000 ys'],
  [QUANTITY_TIME, '9.9996e-26', 4, `1.000 ${X} 10${sup(-25)} s`], [QUANTITY_TIME, '1.2345e-30', 4, `1.234 ${X} 10${sup(-30)} s`],
  [QUANTITY_TIME, '5.391247e-44', 7, `5.391247 ${X} 10${sup(-44)} s`], [QUANTITY_TIME, '5.391247e-44', 1, `5 ${X} 10${sup(-44)} s`],
  [QUANTITY_TIME, '1e-100', 3, `1.00 ${X} 10${sup(-100)} s`], [QUANTITY_TIME, '9.5e-25', 1, '1 ys'], [QUANTITY_TIME, '8.5e-25', 1, `8 ${X} 10${sup(-25)} s`],
  // -- frequency
  [QUANTITY_FREQUENCY, '1', 4, '1.000 Hz'], [QUANTITY_FREQUENCY, '20', 4, '20.00 Hz'], [QUANTITY_FREQUENCY, '440', 4, '440.0 Hz'],
  [QUANTITY_FREQUENCY, '440', 0, '440.0 Hz'], [QUANTITY_FREQUENCY, '440', 1, '400 Hz'], [QUANTITY_FREQUENCY, '440', 2, '440 Hz'],
  [QUANTITY_FREQUENCY, '440', 9, '440.000000 Hz'], [QUANTITY_FREQUENCY, '999.96', 4, '1.000 kHz'], [QUANTITY_FREQUENCY, '999.94', 4, '999.9 Hz'],
  [QUANTITY_FREQUENCY, '32768', 4, '32.77 kHz'], [QUANTITY_FREQUENCY, '32768', 5, '32.768 kHz'], [QUANTITY_FREQUENCY, '1e-12', 4, '1.000 pHz'],
  [QUANTITY_FREQUENCY, '9.9996e-13', 4, '1.000 pHz'], [QUANTITY_FREQUENCY, '9.9994e-13', 4, `9.999 ${X} 10${sup(-13)} Hz`],
  [QUANTITY_FREQUENCY, '5e-13', 4, `5.000 ${X} 10${sup(-13)} Hz`], [QUANTITY_FREQUENCY, '1e-9', 4, '1.000 nHz'],
  [QUANTITY_FREQUENCY, '9.9996e26', 4, `1.000 ${X} 10${sup(27)} Hz`], [QUANTITY_FREQUENCY, '9.9994e26', 4, '999.9 YHz'],
  [QUANTITY_FREQUENCY, '1e27', 4, `1.000 ${X} 10${sup(27)} Hz`], [QUANTITY_FREQUENCY, '3.4e29', 4, `3.400 ${X} 10${sup(29)} Hz`],
  [QUANTITY_FREQUENCY, '9.9996e29', 4, `1.000 ${X} 10${sup(30)} Hz`], [QUANTITY_FREQUENCY, '1.2345e100', 4, `1.234 ${X} 10${sup(100)} Hz`],
  // -- length
  [QUANTITY_LENGTH, '1', 4, '1.000 m'], [QUANTITY_LENGTH, '299792458', 4, '299.8 Mm'], [QUANTITY_LENGTH, '299792458', 9, '299.792458 Mm'],
  [QUANTITY_LENGTH, '299792458', 3, '300 Mm'], [QUANTITY_LENGTH, '999.96', 4, '1.000 km'], [QUANTITY_LENGTH, '0.001', 4, '1.000 mm'],
  [QUANTITY_LENGTH, '0.01', 4, '10.00 mm'], [QUANTITY_LENGTH, '9460730472580800', 4, '9.461 Pm'], [QUANTITY_LENGTH, '1e18', 4, `1.000 ${X} 10${sup(18)} m`],
  [QUANTITY_LENGTH, '9.9996e17', 4, `1.000 ${X} 10${sup(18)} m`], [QUANTITY_LENGTH, '9.9994e17', 4, '999.9 Pm'],
  [QUANTITY_LENGTH, '8.8e26', 2, `8.8 ${X} 10${sup(26)} m`], [QUANTITY_LENGTH, '1e-24', 4, '1.000 ym'], [QUANTITY_LENGTH, '9.9996e-25', 4, '1.000 ym'],
  [QUANTITY_LENGTH, '1.616255e-35', 7, `1.616255 ${X} 10${sup(-35)} m`], [QUANTITY_LENGTH, '1.616255e-35', 4, `1.616 ${X} 10${sup(-35)} m`],
  [QUANTITY_LENGTH, '5.5e-7', 4, `550.0 nm`], [QUANTITY_LENGTH, '1.5e-6', 2, `1.5 ${MICRO}m`],
];
const quantityRows = QUANTITY_CASES.map(([quantity, value, digits, expected]) => {
  const exact = decimal(value);
  const sci = toSci(exact);
  check(cmp(sciValue(sci), exact) === 0, `quantity case ${value} is not exact in 17 digits`);
  checkEqual(formatQuantity(exact, quantity, digits), expected, `quantity case ${value} at ${digits} digits`);
  return { sci, quantity, digits, text: expected, note: value };
});
// And the values of the ladder itself, as frc_sci_t input.
for (const row of fullRows.filter((r) => r.iv === IV_REFERENCE && r.L % 5 === 2)) {
  for (const [sci, quantity] of [[row.period, QUANTITY_TIME], [row.frequency, QUANTITY_FREQUENCY], [row.lengths[4], QUANTITY_LENGTH]])
    for (const digits of [3, 4, 7])
      quantityRows.push({ sci, quantity, digits, text: formatQuantity(sciValue(sci), quantity, digits), note: `level ${row.L}` });
}

/* ---- Device vectors ------------------------------------------------------ */
const DEVICE_LEVELS = Array.from({ length: 41 }, (_, L) => L);
const DEVICE_RATES = [1n, 1000n, 32768n, 1000000n, 80000000n, 240000000n];
const TICK_RATES = [...DEVICE_RATES, 10n ** 12n, U64_LIMIT - 1n]; // the last two reach FRC_ERR_OVERFLOW
const STEP_BITS = [16, 32, 48, 64];
const tickRows = [], stepRows = [];
const tally = { ticks: {}, steps: {} };
for (const iv of [IV_REFERENCE, IV_MEAN]) {
  for (const L of DEVICE_LEVELS) {
    for (const rate of TICK_RATES) {
      const r = deviceTicks(L, intervals[iv], rate);
      tickRows.push({ iv, L, rate, ...r });
      tally.ticks[r.status] = (tally.ticks[r.status] ?? 0) + 1;
    }
    for (const rate of DEVICE_RATES) for (const bits of STEP_BITS) {
      const r = deviceStep(L, intervals[iv], rate, bits);
      stepRows.push({ iv, L, rate, bits, ...r });
      tally.steps[r.status] = (tally.steps[r.status] ?? 0) + 1;
    }
  }
}
check(tally.ticks[STATUS_OK] > 0 && tally.ticks[STATUS_REFUSED] > 0 && tally.ticks[STATUS_OVERFLOW] > 0, 'tick vectors cover ok, refused and overflow');
check(tally.steps[STATUS_OK] > 0 && tally.steps[STATUS_REFUSED] > 0 && !tally.steps[STATUS_OVERFLOW], 'step vectors cover ok and refused');

/* ---- oracle_ladder.h ----------------------------------------------------- */
const ladderHeader = [];
{
  const o = ladderHeader;
  o.push(banner('oracle_ladder.h - expected values of the spectral ladder (SPEC S16).', [
    'Self-contained: include it from a host test; it needs <stdint.h> only.',
    'Tables are `static const`; those a test does not read are tagged unused.',
    'Strings are UTF-8 with non-ASCII bytes written as octal escapes; the comment beside one shows it.',
  ]));
  o.push('#ifndef ORACLE_LADDER_H', '#define ORACLE_LADDER_H', '', '#include <stdint.h>', '');
  o.push('#if defined(__GNUC__) || defined(__clang__)', '#  define ORACLE_LADDER_DATA __attribute__((unused)) static const',
    '#else', '#  define ORACLE_LADDER_DATA static const', '#endif', '');
  o.push('/* frc_status_t values used below. */', '#define ORACLE_LADDER_OK 0', '#define ORACLE_LADDER_OVERFLOW (-3)', '#define ORACLE_LADDER_REFUSED (-6)', '');

  o.push('/** frc_sci_t: mantissa x 10^exp10, 1e16 <= mantissa < 1e17, the exact value rounded half-to-even. */');
  o.push('typedef struct oracle_ladder_sci {', '    uint64_t mantissa;', '    int16_t  exp10;', '} oracle_ladder_sci_t;', '');

  o.push('/* ---- Intervals -------------------------------------------------------------- */', '');
  o.push('/** A Saros interval of num/den milliseconds (frc_interval_t). alpha_ms is non-zero for the two');
  o.push(' *  nominal ones: there the interval is frc_interval_of_alpha_ms(alpha_ms), equal in VALUE to num/den. */');
  o.push('typedef struct oracle_ladder_interval {', '    const char *name;', '    uint64_t num;', '    uint64_t den;', '    uint64_t alpha_ms;', '} oracle_ladder_interval_t;', '');
  o.push(`#define ORACLE_LADDER_INTERVAL_COUNT ${intervals.length}`);
  o.push(`#define ORACLE_LADDER_INTERVAL_REFERENCE ${IV_REFERENCE}`, `#define ORACLE_LADDER_INTERVAL_MEAN ${IV_MEAN}`);
  o.push('#define ORACLE_LADDER_INTERVAL_SAROS141_FIRST 2 /* index 2 + k is E[k+1] - E[k], k = 0..68 */');
  o.push(`#define ORACLE_LADDER_INTERVAL_ALPHA_RECIPROCAL ${intervals.length - 2}`, `#define ORACLE_LADDER_INTERVAL_ALPHA_THEORY ${intervals.length - 1}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_interval_t oracle_ladder_intervals[ORACLE_LADDER_INTERVAL_COUNT] = {');
  intervals.forEach((I, i) => o.push(`    {${cString(I.name)}, ${u64(I.n)}, ${u64(I.d)}, ${u64(I.alpha)}}, /* ${i}: ${I.note} */`));
  o.push('};', '');

  o.push('/* ---- P(L), SPEC S16.4: frc_format_prefix ---------------------------------------- */', '');
  o.push('#define ORACLE_LADDER_PREFIX_COUNT 64');
  o.push('ORACLE_LADDER_DATA char *const oracle_ladder_prefix[ORACLE_LADDER_PREFIX_COUNT] = {');
  P.forEach((p, L) => o.push(`    "${p}", /* P(${L}) */`));
  o.push('};', '');

  o.push('/* ---- Media, SPEC S16.3 ------------------------------------------------------------ */', '');
  o.push('typedef struct oracle_ladder_medium {', '    const char *id;', '    const char *name;', '    const char *phase;', '    uint64_t speed_num; /* metres per second = speed_num / speed_den */', '    uint64_t speed_den;', '} oracle_ladder_medium_t;', '');
  o.push('#define ORACLE_LADDER_MEDIUM_COUNT 7');
  o.push('ORACLE_LADDER_DATA oracle_ladder_medium_t oracle_ladder_media[ORACLE_LADDER_MEDIUM_COUNT] = {');
  MEDIA.forEach((m) => o.push(`    {${cString(m.id)}, ${cString(m.name)}, ${cString(m.phase)}, ${u64(m.speed[0])}, ${u64(m.speed[1])}},`));
  o.push('};', '');

  o.push('/* ---- Bands, SPEC S16.2: frc_format_band, indexed by frc_band_t ------------------- */', '');
  o.push('#define ORACLE_LADDER_BAND_COUNT 9');
  o.push('ORACLE_LADDER_DATA char *const oracle_ladder_band_names[ORACLE_LADDER_BAND_COUNT] = {');
  BAND_NAMES.forEach((b) => o.push(`    ${cString(b)},${readable(b)}`));
  o.push('};', '');

  o.push('/* ---- The full ladder on the reference and the mean interval ---------------------- */', '');
  o.push('/** Levels 0..62 of oracle_ladder_intervals[interval]. length[m] is for frc_medium_t m. The *_bits are');
  o.push(' *  IEEE-754 doubles nearest to the EXACT value: frc_period_seconds, frc_frequency_hz and');
  o.push(' *  frc_length_metres with the vacuum speed. */');
  o.push('typedef struct oracle_ladder_full {', '    uint8_t interval;', '    uint8_t level;', '    uint8_t band; /* frc_band_t */',
    '    oracle_ladder_sci_t period;    /* seconds */', '    oracle_ladder_sci_t frequency; /* hertz   */', '    oracle_ladder_sci_t length[7]; /* metres  */',
    '    uint64_t period_bits;', '    uint64_t frequency_bits;', '    uint64_t length_bits; /* vacuum */', '} oracle_ladder_full_t;', '');
  o.push(`#define ORACLE_LADDER_FULL_COUNT ${fullRows.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_full_t oracle_ladder_full[ORACLE_LADDER_FULL_COUNT] = {');
  for (const r of fullRows) {
    o.push(`    {${r.iv}, ${r.L}, ${r.band}, ${cSci(r.period)}, ${cSci(r.frequency)},`);
    o.push(`     {${r.lengths.slice(0, 4).map(cSci).join(', ')},`);
    o.push(`      ${r.lengths.slice(4).map(cSci).join(', ')}},`);
    o.push(`     ${hex64(r.periodBits)}, ${hex64(r.frequencyBits)}, ${hex64(r.lengthBits)}},`);
  }
  o.push('};', '');

  o.push('/* ---- A spread of levels on every other interval ------------------------------------ */', '');
  o.push(`/** Levels ${SPOT_LEVELS.join(', ')} of intervals 2..${intervals.length - 1}. Length is light in vacuum. */`);
  o.push('typedef struct oracle_ladder_spot {', '    uint8_t interval;', '    uint8_t level;', '    uint8_t band;',
    '    oracle_ladder_sci_t period;', '    oracle_ladder_sci_t frequency;', '    oracle_ladder_sci_t length;',
    '    uint64_t period_bits;', '    uint64_t frequency_bits;', '    uint64_t length_bits;', '} oracle_ladder_spot_t;', '');
  o.push(`#define ORACLE_LADDER_SPOT_COUNT ${spotRows.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_spot_t oracle_ladder_spot[ORACLE_LADDER_SPOT_COUNT] = {');
  for (const r of spotRows)
    o.push(`    {${r.iv}, ${r.L}, ${r.band}, ${cSci(r.period)}, ${cSci(r.frequency)}, ${cSci(r.lengths[0])}, ${hex64(r.periodBits)}, ${hex64(r.frequencyBits)}, ${hex64(r.lengthBits)}},`);
  o.push('};', '');

  o.push('/* ---- Band edges: frc_band compares the exact frequency ----------------------------- */', '');
  o.push('/** An interval num/den ms that puts f(level) exactly on a band boundary, and its two neighbours');
  o.push(' *  (num + 1: a hair slower, the band below; num - 1: a hair faster, the band that starts there). */');
  o.push('typedef struct oracle_ladder_band_edge {', '    uint64_t num;', '    uint64_t den;', '    uint8_t level;', '    uint8_t band;', '} oracle_ladder_band_edge_t;', '');
  o.push(`#define ORACLE_LADDER_BAND_EDGE_COUNT ${bandEdgeRows.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_band_edge_t oracle_ladder_band_edges[ORACLE_LADDER_BAND_EDGE_COUNT] = {');
  for (const r of bandEdgeRows) o.push(`    {${u64(r.I.n)}, ${u64(r.I.d)}, ${r.L}, ${r.band}}, /* ${r.note} */`);
  o.push('};', '');

  o.push('/* ---- The table of SPEC S16.1, exactly as printed ----------------------------------- */', '');
  o.push('/** Row L is level L on the reference interval: frc_format_level_name, frc_siblings (0 for level 0),');
  o.push(' *  frc_format_period, frc_format_frequency, frc_format_band and frc_format_length (vacuum) at 4 digits. */');
  o.push('typedef struct oracle_ladder_spec_row {', '    uint8_t level;', '    uint8_t siblings;', '    const char *name;', '    const char *period;',
    '    const char *frequency;', '    const char *band;', '    const char *length;', '} oracle_ladder_spec_row_t;', '');
  o.push(`#define ORACLE_LADDER_SPEC_ROW_COUNT ${spec.ladder.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_spec_row_t oracle_ladder_spec_rows[ORACLE_LADDER_SPEC_ROW_COUNT] = {');
  for (const row of spec.ladder) {
    o.push(`    {${row.level}, ${siblings(row.level)}, ${cString(row.name)}, ${cString(row.period)}, ${cString(row.frequency)}, ${cString(row.band)}, ${cString(row.length)}},`);
    o.push(`        /* ${row.name} | ${row.period} | ${row.frequency} | ${row.band} | ${row.length} */`);
  }
  o.push('};', '');

  o.push('/* ---- Formatted text of a level ------------------------------------------------------- */', '');
  o.push('#define ORACLE_LADDER_QUANTITY_TIME 0', '#define ORACLE_LADDER_QUANTITY_FREQUENCY 1', '#define ORACLE_LADDER_QUANTITY_LENGTH 2', '');
  o.push('/** quantity 0: frc_format_period(level, I, digits); 1: frc_format_frequency; 2: frc_format_length with');
  o.push(' *  the speed of oracle_ladder_media[medium] (medium is 0 for the other two). digits 0 means 4. */');
  o.push('typedef struct oracle_ladder_format {', '    uint8_t interval;', '    uint8_t level;', '    uint8_t quantity;', '    uint8_t medium;', '    uint8_t digits;', '    const char *text;', '} oracle_ladder_format_t;', '');
  o.push(`#define ORACLE_LADDER_FORMAT_COUNT ${formatRows.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_format_t oracle_ladder_format[ORACLE_LADDER_FORMAT_COUNT] = {');
  for (const r of formatRows) o.push(`    {${r.iv}, ${r.L}, ${r.quantity}, ${r.medium}, ${r.digits}, ${cString(r.text)}},${readable(r.text)}`);
  o.push('};', '');

  o.push('/* ---- frc_format_quantity on a given frc_sci_t ------------------------------------------ */', '');
  o.push('typedef struct oracle_ladder_quantity {', '    oracle_ladder_sci_t value;', '    uint8_t quantity;', '    uint8_t digits;', '    const char *text;', '} oracle_ladder_quantity_t;', '');
  o.push(`#define ORACLE_LADDER_QUANTITY_COUNT ${quantityRows.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_quantity_t oracle_ladder_quantity[ORACLE_LADDER_QUANTITY_COUNT] = {');
  for (const r of quantityRows) o.push(`    {${cSci(r.sci)}, ${r.quantity}, ${r.digits}, ${cString(r.text)}}, /* ${r.note} -> ${r.text} */`);
  o.push('};', '');

  o.push('/* ---- frc_sci_to_double --------------------------------------------------------------- */', '');
  o.push('/** The double nearest to mantissa x 10^exp10 itself, ties to even. The values are the period, frequency');
  o.push(` *  and vacuum length of oracle_ladder_full, in that order. ${sciDoubleDiffers} of them differ by one unit in the`);
  o.push(' *  last place from the double of the exact rational: rounding twice is not rounding once. */');
  o.push('typedef struct oracle_ladder_sci_double {', '    oracle_ladder_sci_t value;', '    uint64_t bits;', '} oracle_ladder_sci_double_t;', '');
  o.push(`#define ORACLE_LADDER_SCI_DOUBLE_COUNT ${sciDoubles.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_sci_double_t oracle_ladder_sci_double[ORACLE_LADDER_SCI_DOUBLE_COUNT] = {');
  for (const r of sciDoubles) o.push(`    {${cSci(r.sci)}, ${hex64(r.bits)}},`);
  o.push('};', '');

  o.push('/* ---- Devices, SPEC S16.4 --------------------------------------------------------------- */', '');
  o.push('/** frc_device_ticks(level, I, rate): status 0 with the count, -6 refused (under half a tick),');
  o.push(' *  -3 overflow (the count is 2^64 or more). ticks is 0 unless status is 0. */');
  o.push('typedef struct oracle_ladder_ticks {', '    uint8_t interval;', '    uint8_t level;', '    int8_t status;', '    uint64_t rate;', '    uint64_t ticks;', '} oracle_ladder_ticks_t;', '');
  o.push(`#define ORACLE_LADDER_TICKS_COUNT ${tickRows.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_ticks_t oracle_ladder_ticks[ORACLE_LADDER_TICKS_COUNT] = {');
  for (const r of tickRows) o.push(`    {${r.iv}, ${r.L}, ${r.status}, ${u64(r.rate)}, ${u64(r.value)}},`);
  o.push('};', '');
  o.push('/** frc_device_step(level, I, rate, bits): status 0 with the step, -6 refused (f above rate / 2, or a');
  o.push(' *  step that rounds to 0). step is 0 unless status is 0. */');
  o.push('typedef struct oracle_ladder_step {', '    uint8_t interval;', '    uint8_t level;', '    uint8_t bits;', '    int8_t status;', '    uint64_t rate;', '    uint64_t step;', '} oracle_ladder_step_t;', '');
  o.push(`#define ORACLE_LADDER_STEP_COUNT ${stepRows.length}`);
  o.push('ORACLE_LADDER_DATA oracle_ladder_step_t oracle_ladder_steps[ORACLE_LADDER_STEP_COUNT] = {');
  for (const r of stepRows) o.push(`    {${r.iv}, ${r.L}, ${r.bits}, ${r.status}, ${u64(r.rate)}, ${u64(r.value)}},`);
  o.push('};', '', '#endif /* ORACLE_LADDER_H */', '');
}

/* ---- Civil vectors ------------------------------------------------------- */
const civilRows = [];
let civilChosen = 0; // how many are hand-picked; the rest are the pseudo-random spread
const seenCivil = new Set();
function addCivil(tUs, offsetS, note = '') {
  const key = `${tUs}/${offsetS}`;
  if (seenCivil.has(key)) return;
  seenCivil.add(key);
  const c = civilFromTime(tUs, offsetS);
  check(timeFromCivil(c, offsetS) === tUs, 'civil fields convert back to the instant');
  // The runtime's calendar, as a second opinion (Date is proleptic Gregorian over +-271821 years).
  const localMs = floorDiv(tUs + BigInt(offsetS) * 1000000n, 1000n);
  const date = new Date(Number(localMs));
  check(BigInt(date.getUTCFullYear()) === c.year && date.getUTCMonth() + 1 === Number(c.month) && date.getUTCDate() === Number(c.day)
    && date.getUTCHours() === Number(c.hour) && date.getUTCMinutes() === Number(c.minute) && date.getUTCSeconds() === Number(c.second)
    && date.getUTCMilliseconds() === Number(c.microsecond / 1000n) && (date.getUTCDay() + 6) % 7 === Number(c.weekday),
  `civil fields of ${tUs} at ${offsetS} differ from Date`);
  const iso = formatCivil(c, offsetS);
  if (offsetS === 0 && c.year >= 0n && c.year <= 9999n) checkEqual(iso, date.toISOString(), 'ISO text');
  civilRows.push({ tUs, offsetS, c, iso, note });
}
/** A vector given by its LOCAL fields at an offset. */
function addCivilFields(year, month, day, hour, minute, second, microsecond, offsetS, note) {
  addCivil(timeFromCivil({ year, month, day, hour, minute, second, microsecond }, offsetS), offsetS, note);
}
{
  addCivil(0n, 0, 'the Unix epoch');
  addCivil(-1n, 0, 'one microsecond before it');
  addCivil(1n, 0); addCivil(999n, 0, 'under a millisecond: the text shows .000'); addCivil(1000n, 0);
  addCivil(-1000n, 0); addCivil(-1001n, 0); addCivil(999999n, 0); addCivil(1000000n, 0); addCivil(-1000000n, 0);
  addCivil(86400000000n - 1n, 0); addCivil(86400000000n, 0); addCivil(-86400000000n, 0); addCivil(-86400000001n, 0);
  addCivil(1791460800000000n, 0, 'SPEC S12 vector A'); addCivil(946684800000000n, 0, 'SPEC S12 vector B'); addCivil(2886085800000000n, 0, 'SPEC S12 vector C');
  addCivil(E[0] * 1000n, 0, 'first eclipse of Saros 141'); addCivil(E[69] * 1000n, 0, 'last eclipse of Saros 141');
  addCivil(E[20] * 1000n, 0, 'the anchor eclipse'); addCivil(E[22] * 1000n, 0); addCivil(E[23] * 1000n, 0);
  addCivil(1791460800000000n, 7200, 'the header example at +02:00'); addCivil(1791460800000000n, 19800); addCivil(1791460800000000n, -34200);
  // Leap days and the century rule.
  for (const [y, note] of [[2000, 'divisible by 400: leap'], [1900, 'divisible by 100: not leap'], [2100, 'not leap'], [2400, 'leap'], [1600, 'leap'],
    [2024, 'leap'], [2023, 'not leap'], [4, 'leap'], [0, 'year 0 is leap'], [-4, 'leap'], [-100, 'not leap'], [-400, 'leap'], [-2000, 'leap'], [6000, 'leap'], [5900, 'not leap']]) {
    addCivilFields(y, 2, 28, 23, 59, 59, 999999, 0, `${y}: ${note}`);
    addCivil(timeFromCivil({ year: y, month: 2, day: 28, hour: 23, minute: 59, second: 59, microsecond: 999999 }, 0) + 1n, 0);
    addCivil(timeFromCivil({ year: y, month: 3, day: 1, hour: 0, minute: 0, second: 0, microsecond: 0 }, 0), 0);
  }
  // Year ends, and the edges of the ISO year formats.
  for (const y of [-2000, -1000, -45, -1, 0, 1, 99, 100, 999, 1000, 1582, 1969, 1970, 1999, 2038, 2999, 6000, 9999]) {
    addCivilFields(y, 12, 31, 23, 59, 59, 999999, 0, `last microsecond of ${y}`);
    addCivilFields(y + 1, 1, 1, 0, 0, 0, 0, 0);
  }
  addCivilFields(-44, 3, 15, 12, 0, 0, 0, 0, 'a negative year: "-0044"');
  addCivilFields(-2000, 1, 1, 0, 0, 0, 0, 0, 'the first instant of the range');
  addCivilFields(6000, 12, 31, 23, 59, 59, 999999, 0, 'the last instant of the range');
  addCivilFields(10000, 1, 1, 0, 0, 0, 0, 0, 'beyond 9999: "+10000"');
  addCivilFields(12345, 6, 7, 8, 9, 10, 111213, 0, '"+12345"');
  addCivilFields(-12345, 6, 7, 8, 9, 10, 111213, 0, '"-12345"');
  addCivilFields(2038, 1, 19, 3, 14, 7, 999999, 0, 'the last second of a signed 32-bit time');
  addCivilFields(2038, 1, 19, 3, 14, 8, 0, 0);
  addCivilFields(1901, 12, 13, 20, 45, 52, 0, 0);
  // Offsets that move the date, the month and the year.
  for (const off of [19800, -34200, 3600, -18000, 20700, 45900, 50400, -43200, 34200, -12600, 60, -60]) {
    addCivilFields(2026, 1, 1, 0, 0, 0, 0, off, 'local new year');
    addCivilFields(2025, 12, 31, 23, 59, 59, 999999, off);
    addCivil(timeFromCivil({ year: 2026, month: 1, day: 1, hour: 0, minute: 0, second: 0, microsecond: 0 }, 0), off, 'UTC new year, seen locally');
    addCivil(timeFromCivil({ year: 2024, month: 2, day: 29, hour: 12, minute: 0, second: 0, microsecond: 500 }, 0), off);
  }
  addCivilFields(-1, 12, 31, 20, 0, 0, 0, -34200, 'the offset crosses into year 0');
  addCivilFields(0, 1, 1, 3, 0, 0, 0, 19800);
  addCivilFields(9999, 12, 31, 22, 0, 0, 0, -18000);
  addCivilFields(10000, 1, 1, 1, 30, 0, 0, 19800);
  // A fixed pseudo-random spread: half over the whole range, half near the present.
  const random = makeRandom(0x9E3779B97F4A7C15n);
  const OFFSETS = [0, 0, 0, 0, 19800, -34200, 3600, -28800, 32400, 45900, -12600, 50400];
  const wide = [timeFromCivil({ year: -2000, month: 1, day: 1, hour: 0, minute: 0, second: 0, microsecond: 0 }, 0),
    timeFromCivil({ year: 6001, month: 1, day: 1, hour: 0, minute: 0, second: 0, microsecond: 0 }, 0)];
  const near = [timeFromCivil({ year: 1900, month: 1, day: 1, hour: 0, minute: 0, second: 0, microsecond: 0 }, 0),
    timeFromCivil({ year: 2101, month: 1, day: 1, hour: 0, minute: 0, second: 0, microsecond: 0 }, 0)];
  civilChosen = civilRows.length;
  while (civilRows.length < 300) {
    const [lo, hi] = civilRows.length % 2 ? wide : near;
    let t = lo + random.next() % (hi - lo);
    const grain = random.below(4);
    if (grain === 0n) t -= floorMod(t, 1000n);          // whole milliseconds
    else if (grain === 1n) t -= floorMod(t, 1000000n);  // whole seconds
    addCivil(t, OFFSETS[Number(random.below(OFFSETS.length))]);
  }
  for (const r of civilRows) check(r.tUs > -(1n << 63n) && r.tUs < 1n << 63n, 'civil instant fits int64');
}

/* frc_time_from_civil with fields that are not a normal date. */
const KIND_NORMAL = 0, KIND_OVERFLOW = 1, KIND_UNDERFLOW = 2;
const rollRows = [];
function addRoll(kind, year, month, day, hour, minute, second, microsecond, offsetS, note) {
  const fields = { year, month, day, hour, minute, second, microsecond };
  const tUs = timeFromCivil(fields, offsetS);
  check(tUs > -(1n << 63n) && tUs < (1n << 63n), 'rolled instant fits int64');
  const c = civilFromTime(tUs, offsetS);
  if (kind === KIND_NORMAL) check(Number(c.year) === year && Number(c.month) === month && Number(c.day) === day, 'a normal date is its own fields');
  rollRows.push({ kind, fields, offsetS, tUs, note: `${note}: ${formatCivil(c, offsetS)}` });
}
{
  const N = KIND_NORMAL, O = KIND_OVERFLOW, U = KIND_UNDERFLOW;
  addRoll(N, 2026, 10, 8, 12, 0, 0, 0, 0, 'a normal date');
  addRoll(N, 2026, 10, 8, 14, 0, 0, 0, 7200, 'a normal date at +02:00');
  addRoll(O, 2026, 1, 32, 0, 0, 0, 0, 0, 'January 32');
  addRoll(O, 2026, 2, 29, 0, 0, 0, 0, 0, 'February 29 of a common year');
  addRoll(O, 2024, 2, 30, 0, 0, 0, 0, 0, 'February 30 of a leap year');
  addRoll(O, 2026, 4, 31, 6, 0, 0, 0, 0, 'April 31');
  addRoll(O, 2026, 12, 32, 0, 0, 0, 0, 0, 'December 32');
  addRoll(O, 2026, 1, 255, 0, 0, 0, 0, 0, 'day 255');
  addRoll(O, 2026, 13, 1, 0, 0, 0, 0, 0, 'month 13');
  addRoll(O, 2026, 14, 29, 0, 0, 0, 0, 0, 'month 14, day 29 (2027 is a common year)');
  addRoll(O, 2027, 14, 29, 0, 0, 0, 0, 0, 'month 14, day 29 (2028 is a leap year)');
  addRoll(O, 2026, 15, 1, 0, 0, 0, 0, 0, 'month 15');
  addRoll(O, 2026, 24, 31, 0, 0, 0, 0, 0, 'month 24');
  addRoll(O, 2026, 25, 1, 0, 0, 0, 0, 0, 'month 25');
  addRoll(O, 2026, 255, 255, 0, 0, 0, 0, 0, 'month 255, day 255');
  addRoll(O, 2026, 10, 8, 24, 0, 0, 0, 0, 'hour 24');
  addRoll(O, 2026, 10, 8, 25, 61, 61, 0, 0, 'hour 25, minute 61, second 61');
  addRoll(O, 2026, 10, 8, 255, 255, 255, 0, 0, 'hour, minute and second 255');
  addRoll(O, 2026, 10, 8, 23, 60, 0, 0, 0, 'minute 60');
  addRoll(O, 2026, 12, 31, 23, 59, 60, 0, 0, 'second 60: a leap second reads as the next second');
  addRoll(O, 2026, 10, 8, 12, 0, 0, 1000000, 0, 'a million microseconds');
  addRoll(O, 2026, 10, 8, 12, 0, 0, 4294967295, 0, 'the largest microsecond field');
  addRoll(O, 2026, 12, 31, 23, 59, 59, 1000000, 0, 'a carry through every field');
  addRoll(O, 2026, 255, 255, 255, 255, 255, 4294967295, -43200, 'every field at its largest');
  addRoll(O, -1, 13, 1, 0, 0, 0, 0, 0, 'month 13 of year -1');
  addRoll(O, -2000, 12, 32, 0, 0, 0, 0, 19800, 'December 32 of year -2000 at +05:30');
  addRoll(O, 1969, 12, 31, 23, 59, 60, 0, 0, 'second 60 onto the epoch');
  addRoll(O, 9999, 12, 31, 24, 0, 0, 0, 0, 'hour 24 into year 10000');
  addRoll(U, 2026, 1, 0, 0, 0, 0, 0, 0, 'day 0 is the day before the 1st');
  addRoll(U, 2026, 3, 0, 0, 0, 0, 0, 0, 'March 0 of a common year');
  addRoll(U, 2024, 3, 0, 0, 0, 0, 0, 0, 'March 0 of a leap year');
  addRoll(U, 2026, 0, 1, 0, 0, 0, 0, 0, 'month 0 is December of the year before');
  addRoll(U, 2026, 0, 0, 0, 0, 0, 0, 0, 'month 0, day 0');
  addRoll(U, 0, 0, 0, 0, 0, 0, 0, 0, 'year 0, month 0, day 0');
  addRoll(U, 1970, 1, 0, 24, 0, 0, 0, 0, 'day 0 and hour 24 cancel');
}

/* frc_julian_fields_to_unix_ms */
const julianRows = [];
function addJulian(ms, label = '') {
  const fixed = julianFieldsToUnixMs(ms);
  // The reference library applies the same re-reading before the reform and passes later values through.
  const library = ms < REFORM_MS ? fixed : ms;
  if (F) {
    const r = F.correctJulianTimestamp(F.rational(ms, 1000n));
    const seconds = typeof r === 'object' ? [r.numerator, r.denominator] : decimal(String(r)); // a whole number comes back as a Number
    check(cmp(mul(seconds, int(1000)), int(library)) === 0, `Julian re-reading of ${ms} differs from the reference`);
  }
  check((fixed - ms) % 86400000n === 0n, 'the re-reading moves a value by whole days');
  const days = (fixed - ms) / 86400000n;
  const iso = (v) => formatCivil(civilFromTime(v * 1000n, 0), 0);
  const moved = days === 0n ? 'unchanged' : `${days > 0n ? '+' : ''}${days} d -> ${iso(fixed)}`;
  julianRows.push({ ms, fixed, library, note: `${iso(ms)}: ${moved}${ms >= REFORM_MS ? '; library unchanged' : ''}${label ? ` (${label})` : ''}` });
}
{
  const at = (y, mo, d, h = 0, mi = 0, s = 0, ms = 0) => timeFromCivil({ year: y, month: mo, day: d, hour: h, minute: mi, second: s, microsecond: ms * 1000 }, 0) / 1000n;
  check(at(1582, 10, 15) === REFORM_MS, 'the reform instant');
  // Before the reform: the formula and the library agree. The shift is 10 days in 1582 and one
  // less for each century back that the Gregorian calendar skips as a leap year.
  addJulian(at(1582, 10, 14, 23, 59, 59, 999), 'the last millisecond the library corrects');
  addJulian(at(1582, 10, 4), 'Julian 1582-10-04 was the eve of the reform');
  addJulian(at(1582, 1, 1));
  addJulian(at(1500, 3, 1), 'after the Julian-only leap day of 1500');
  addJulian(at(1500, 2, 28), 'before it');
  addJulian(at(1453, 5, 29, 12, 34, 56, 789), 'the time of day is kept');
  addJulian(at(1400, 3, 1)); addJulian(at(1400, 2, 28));
  addJulian(at(1300, 6, 15)); addJulian(at(1200, 6, 15, 23, 59, 59, 999));
  addJulian(at(1066, 10, 14)); addJulian(at(1000, 1, 1)); addJulian(at(800, 12, 25));
  addJulian(at(622, 7, 16)); addJulian(at(525, 1, 1)); addJulian(at(476, 9, 4)); addJulian(at(325, 5, 20));
  addJulian(at(300, 3, 1)); addJulian(at(300, 2, 28), 'the calendars agree from 200-03-01 to 300-02-28');
  addJulian(at(250, 6, 1, 6, 0, 0, 1)); addJulian(at(200, 3, 1)); addJulian(at(200, 2, 28));
  addJulian(at(100, 3, 1)); addJulian(at(100, 2, 28));
  addJulian(at(1, 1, 1)); addJulian(at(0, 12, 31, 23, 59, 59, 999), 'year 0 is 1 BC'); addJulian(at(0, 2, 29), 'a leap day in both calendars');
  addJulian(at(-43, 3, 15), '44 BC'); addJulian(at(-100, 3, 1)); addJulian(at(-100, 2, 28));
  addJulian(at(-500, 6, 15)); addJulian(at(-762, 6, 15, 9), '763 BC'); addJulian(at(-1000, 1, 1));
  addJulian(at(-2000, 1, 1)); addJulian(at(-2000, 12, 31, 23, 59, 59, 999)); addJulian(at(-4712, 1, 1, 12), 'the fields of Julian day 0');
  // From the reform on, the plain re-reading still moves the value; the library returns it unchanged.
  addJulian(REFORM_MS, 'the reform instant');
  addJulian(E[0], 'first eclipse of Saros 141');
  addJulian(at(1700, 3, 1)); addJulian(at(1900, 3, 1));
  addJulian(0n, 'the Unix epoch'); addJulian(at(2026, 10, 8, 12)); addJulian(at(2100, 3, 1));
  check(new Set(julianRows.map((r) => r.ms)).size === julianRows.length, 'Julian vectors are distinct');
}

/* ---- oracle_civil.h ------------------------------------------------------ */
const civilHeader = [];
{
  const o = civilHeader;
  o.push(banner('oracle_civil.h - expected values of the Gregorian translation and of SPEC S2.3.', [
    'Self-contained: include it from a host test; it needs <stdint.h> only.',
    'Proleptic Gregorian calendar, astronomical year numbering (year 0 = 1 BC), weekday 0 = Monday.',
    'Every civil vector was checked against the JavaScript Date of the runtime that generated it.',
  ]));
  o.push('#ifndef ORACLE_CIVIL_H', '#define ORACLE_CIVIL_H', '', '#include <stdint.h>', '');
  o.push('#if defined(__GNUC__) || defined(__clang__)', '#  define ORACLE_CIVIL_DATA __attribute__((unused)) static const',
    '#else', '#  define ORACLE_CIVIL_DATA static const', '#endif', '');

  o.push('/**');
  o.push(' * frc_civil_from_time(t_us, utc_offset_s) gives the fields; frc_time_from_civil(fields, utc_offset_s)');
  o.push(' * gives t_us back; frc_format_civil(fields, utc_offset_s) gives iso.');
  o.push(' * iso: "YYYY-MM-DDTHH:MM:SS.mmm" then "Z" for offset 0, else "+HH:MM" / "-HH:MM". mmm is the');
  o.push(' * microsecond field TRUNCATED to milliseconds. Years 0..9999 take four digits; a negative year is');
  o.push(' * "-" and at least four digits ("-0044", "-12345"); a year above 9999 is "+" and its digits ("+10000").');
  o.push(' */');
  o.push('typedef struct oracle_civil {', '    int64_t  t_us;', '    int32_t  utc_offset_s;', '    int32_t  year;', '    uint8_t  month;', '    uint8_t  day;',
    '    uint8_t  hour;', '    uint8_t  minute;', '    uint8_t  second;', '    uint8_t  weekday; /* 0 = Monday .. 6 = Sunday */', '    uint32_t microsecond;', '    const char *iso;', '} oracle_civil_t;', '');
  o.push(`#define ORACLE_CIVIL_COUNT ${civilRows.length}`);
  o.push('ORACLE_CIVIL_DATA oracle_civil_t oracle_civil[ORACLE_CIVIL_COUNT] = {');
  for (const r of civilRows) {
    const c = r.c;
    o.push(`    {${i64(r.tUs)}, ${r.offsetS}, ${c.year}, ${c.month}, ${c.day}, ${c.hour}, ${c.minute}, ${c.second}, ${c.weekday}, ${c.microsecond}, ${cString(r.iso)}},${r.note ? ` /* ${r.note} */` : ''}`);
  }
  o.push('};', '');

  o.push('/**');
  o.push(' * frc_time_from_civil on fields that are not a normal date. The rule is linear in every field:');
  o.push(' *   year\' = year + floor((month - 1) / 12),  month\' = (month - 1) mod 12 + 1');
  o.push(' *   t = days_from_civil(year\', month\', 1) + (day - 1) days + hour h + minute min + second s');
  o.push(' *       + microsecond us - utc_offset_s');
  o.push(' * kind 0: a normal date. kind 1: fields above their range ("day 32 rolls over").');
  o.push(' * kind 2: month 0 or day 0, which the same rule reads as "one before the first".');
  o.push(' */');
  o.push('#define ORACLE_CIVIL_ROLL_NORMAL 0', '#define ORACLE_CIVIL_ROLL_OVERFLOW 1', '#define ORACLE_CIVIL_ROLL_UNDERFLOW 2');
  o.push('typedef struct oracle_civil_roll {', '    uint8_t  kind;', '    int32_t  year;', '    uint8_t  month;', '    uint8_t  day;', '    uint8_t  hour;',
    '    uint8_t  minute;', '    uint8_t  second;', '    uint32_t microsecond;', '    int32_t  utc_offset_s;', '    int64_t  t_us;', '} oracle_civil_roll_t;', '');
  o.push(`#define ORACLE_CIVIL_ROLL_COUNT ${rollRows.length}`);
  o.push('ORACLE_CIVIL_DATA oracle_civil_roll_t oracle_civil_roll[ORACLE_CIVIL_ROLL_COUNT] = {');
  for (const r of rollRows) {
    const f = r.fields;
    o.push(`    {${r.kind}, ${f.year}, ${f.month}, ${f.day}, ${f.hour}, ${f.minute}, ${f.second}, UINT32_C(${f.microsecond}), ${r.offsetS}, ${i64(r.tUs)}}, /* ${r.note} */`);
  }
  o.push('};', '');

  o.push('/**');
  o.push(' * SPEC S2.3. fixed_ms is the formula applied to unix_ms as it stands: the proleptic-Gregorian fields');
  o.push(' * read as a Julian date. This is what frc_julian_fields_to_unix_ms is documented to return.');
  o.push(' * library_ms is what the reference F.correctJulianTimestamp returns: the same before');
  o.push(' * 1582-10-15T00:00:00Z (unix_ms < ORACLE_JULIAN_REFORM_MS), and unix_ms unchanged from there on.');
  o.push(' */');
  o.push(`#define ORACLE_JULIAN_REFORM_MS ${i64(REFORM_MS)}`);
  o.push('typedef struct oracle_julian {', '    int64_t unix_ms;', '    int64_t fixed_ms;', '    int64_t library_ms;', '} oracle_julian_t;', '');
  o.push(`#define ORACLE_JULIAN_COUNT ${julianRows.length}`);
  o.push('ORACLE_CIVIL_DATA oracle_julian_t oracle_julian[ORACLE_JULIAN_COUNT] = {');
  for (const r of julianRows) o.push(`    {${i64(r.ms)}, ${i64(r.fixed)}, ${i64(r.library)}}, /* ${r.note} */`);
  o.push('};', '', '#endif /* ORACLE_CIVIL_H */', '');
}

/* ---- Write --------------------------------------------------------------- */
fs.mkdirSync(opts.out, { recursive: true });
const ladderPath = path.join(opts.out, 'oracle_ladder.h');
const civilPath = path.join(opts.out, 'oracle_civil.h');
fs.writeFileSync(ladderPath, ladderHeader.join('\n'));
fs.writeFileSync(civilPath, civilHeader.join('\n'));

console.log(`SPEC       ${opts.spec}`);
console.log(`reference  ${F ? opts.reference : '(not used)'}`);
console.log(`checks     ${checks} passed`);
console.log(`ladder     ${ladderPath}`);
console.log(`           intervals ${intervals.length}, full ${fullRows.length}, spot ${spotRows.length}, band edges ${bandEdgeRows.length}, spec rows ${spec.ladder.length},`);
console.log(`           format ${formatRows.length}, quantity ${quantityRows.length}, sci->double ${sciDoubles.length} (${sciDoubleDiffers} differ from the exact double),`);
console.log(`           ticks ${tickRows.length} ${JSON.stringify(tally.ticks)}, steps ${stepRows.length} ${JSON.stringify(tally.steps)}`);
console.log(`civil      ${civilPath}`);
console.log(`           civil ${civilRows.length} (${civilChosen} chosen, ${civilRows.length - civilChosen} spread), roll ${rollRows.length}, julian ${julianRows.length}`);
