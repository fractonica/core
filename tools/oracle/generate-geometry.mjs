#!/usr/bin/env node
/*
 * generate-geometry.mjs - the GEOMETRY ORACLE of fractonica-core.
 *
 * Writes tests/vectors/oracle_geometry.h: what the frozen JavaScript reference
 * (reference/js/fractonica.js) draws.
 *
 *   glyphs  CBT.geometry(CBT.fromIndex(i, {depth: d}), {thickness: 0.3}) for every
 *           state of d = 1..4 strokes: the SVG path (SPEC S9.1), whether the state
 *           carries the dot, and the bounds of each d. tests/test_glyph.c prints the
 *           output of frc_glyph_path in the same notation and compares the strings.
 *   arcs    clock.arc(level, halfIndex, {root: 'wave', relativeTo}) for a spread of
 *           levels and half-cycle indices, as doubles. tests/test_arc.c rebuilds each
 *           one with frc_arc_wave_root + frc_arc_child and compares.
 *
 * Nothing here re-implements the geometry; the generator only checks, before it
 * writes, that the reference still says what SPEC S9.1 says (dot at (0, -1.75) with
 * radius 0.3, bounds x -2.15..2.15, y -2.2..3d + 0.15, stems before strokes).
 *
 * Usage (from anywhere):
 *   node tools/oracle/generate-geometry.mjs [--reference fractonica.js] [--out tests/vectors]
 *
 * Output is deterministic: no clock and no Math.random.
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

function parseArgs(argv) {
  const opts = {
    reference: path.resolve(HERE, '../../../reference/js/fractonica.js'),
    out: path.resolve(HERE, '../../tests/vectors'),
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--reference') opts.reference = path.resolve(argv[++i]);
    else if (a === '--out') opts.out = path.resolve(argv[++i]);
    else throw new Error(`Unknown argument: ${a}`);
  }
  if (!fs.existsSync(opts.reference)) throw new Error('Reference library not found; pass --reference <path>.');
  return opts;
}

const opts = parseArgs(process.argv.slice(2));

/* The reference is a classic script that installs globalThis.Fractonica. */
await import(pathToFileURL(opts.reference).href);
const F = globalThis.Fractonica;
if (!F) throw new Error('The reference library did not define globalThis.Fractonica.');
const clock = F.create({ timeScale: 'unix' });

const check = (cond, message) => {
  if (!cond) throw new Error(`Self-check failed: ${message}`);
};
const near = (a, b) => Math.abs(a - b) < 1e-12;

/* ------------------------------------------------------------------------- */
/* Glyphs                                                                     */
/* ------------------------------------------------------------------------- */

const STYLE = { thickness: 0.3 }; // SPEC S9.1; every other dimension is the reference's default
const MAX_STROKES = 4;

const glyphs = [];
const bounds = [];
for (let d = 1; d <= MAX_STROKES; d++) {
  const states = 2 * (3 ** d - 1);
  for (let i = 0; i < states; i++) {
    const glyph = F.CBT.fromIndex(i, { depth: d });
    const geo = F.CBT.geometry(glyph, STYLE);
    check(geo.path === [...geo.stems, ...geo.strokes].join(' '), 'path is stems then strokes');
    check(geo.stems.length === d && geo.strokes.length === d, 'one stem piece and one stroke per trit');
    check((geo.dot !== null) === (glyph.derivative === -1), 'the dot marks the falling branch only');
    if (geo.dot) check(geo.dot.x === 0 && near(geo.dot.y, -1.75) && near(geo.dot.radius, 0.3), 'dot of S9.1');
    const b = geo.bounds;
    check(near(b.minX, -2.15) && near(b.maxX, 2.15) && near(b.minY, -2.2) && near(b.maxY, 3 * d + 0.15), 'bounds of S9.1');
    glyphs.push({ strokes: d, index: i, dotted: geo.dot ? 1 : 0, path: geo.path });
  }
  bounds.push({ strokes: d, minX: -215, minY: -220, maxX: 215, maxY: 300 * d + 15 });
}

/* ------------------------------------------------------------------------- */
/* Arcs                                                                       */
/* ------------------------------------------------------------------------- */

const factor = (level) => F.defaults.sequence[level % F.defaults.sequence.length];
/** The first half-cycle index at `level` under half-cycle `index` of `fromLevel`. */
function descend(fromLevel, index, level, pick) {
  let i = BigInt(index);
  for (let l = fromLevel; l < level; l++) i = i * BigInt(factor(l)) + BigInt(pick(l) % factor(l));
  return i;
}

const arcs = [];
function addArc(level, index, relativeTo) {
  const a = clock.arc(level, index, relativeTo ? { root: 'wave', relativeTo } : { root: 'wave' });
  for (const v of [a.center.x, a.center.y, a.radius, a.start, a.sweep]) check(Number.isFinite(v), 'finite arc');
  arcs.push({
    level,
    index: BigInt(index),
    relLevel: relativeTo ? relativeTo.level : -1,
    relIndex: relativeTo ? BigInt(relativeTo.index) : 0n,
    a,
  });
}

/* the roots themselves, both bulges */
for (const r of [0, 1, 2, 11, 34]) addArc(0, r);
/* every child of two roots (even and odd), and every grandchild of one child of each */
for (const r of [0, 11]) {
  for (let j = 0; j < factor(0); j++) addArc(1, r * factor(0) + j);
  for (const j of [0, 3]) {
    const parent = r * factor(0) + j;
    for (let k = 0; k < factor(1); k++) addArc(2, parent * factor(1) + k);
  }
}
/* single chains down the levels: first children, last children, and a mixed walk */
for (let level = 3; level <= 9; level++) {
  addArc(level, descend(0, 5, level, () => 0));
  addArc(level, descend(0, 6, level, (l) => factor(l) - 1));
  addArc(level, descend(0, 23, level, (l) => 2 * l + 1));
}
/* relativeTo: the same shapes rebased on an ancestor (unit circle at the origin) */
for (const [refLevel, level] of [[0, 0], [0, 2], [1, 1], [1, 3], [2, 5], [4, 6], [6, 8], [6, 10], [9, 12]]) {
  const refIndex = descend(0, 23, refLevel, (l) => 2 * l + 1);
  for (const pick of [() => 0, (l) => factor(l) - 1, (l) => 3 * l + 2]) {
    addArc(level, descend(refLevel, refIndex, level, pick), { level: refLevel, index: refIndex });
  }
}

/* ------------------------------------------------------------------------- */
/* Emit                                                                       */
/* ------------------------------------------------------------------------- */

const cString = (s) => `"${s}"`; // paths are digits, spaces, letters, '-' and '.'
const dbl = (x) => {
  const s = String(x); // shortest text that reads back as the same double
  return /[.e]/.test(s) ? s : s + '.0';
};

const o = [];
o.push(
  '/*',
  ' * oracle_geometry.h - GENERATED by tools/oracle/generate-geometry.mjs. DO NOT EDIT: regenerate.',
  ' *',
  ' * What the frozen JavaScript reference (fractonica.js) draws: glyph paths (SPEC S9.1) and arcs.',
  ' * Self-contained C99: it needs <stdint.h> only and none of the fractonica.h types.',
  ' *',
  ' * CONVENTIONS',
  ' *   path     the reference\'s SVG path in DESIGN UNITS (the core\'s int16 coordinates divided by 100):',
  ' *            for each trit the stem piece "M0 y L0 y+3", then for each trit its stroke, joined by one space.',
  ' *            Numbers are written the JavaScript way: "-1", "2", never "2.0".',
  ' *   bounds   in the core\'s units, hundredths of a design unit.',
  ' *   arcs     clock.arc(level, index, {root: "wave", relativeTo}): centre, radius, start angle and signed',
  ' *            sweep in radians, as doubles. `index` is the global HALF-CYCLE index at `level`',
  ' *            (child = parent * factor(parent level) + local). The reference reduces `start` to [0, 2 pi);',
  ' *            compare angles modulo a turn. rel_level -1: not rebased. Otherwise the ancestor half-cycle',
  ' *            rel_index of level rel_level is drawn as the unit circle at the origin (its start and sweep kept).',
  ' */',
  '#ifndef ORACLE_GEOMETRY_H',
  '#define ORACLE_GEOMETRY_H',
  '',
  '#include <stdint.h>',
  '',
  '#if defined(__GNUC__) || defined(__clang__)',
  '#  define ORACLE_GEOMETRY_DATA __attribute__((unused)) static const',
  '#else',
  '#  define ORACLE_GEOMETRY_DATA static const',
  '#endif',
  '',
  '/* ---- glyph paths: frc_glyph_from_index + frc_glyph_path ------------------------------------------------ */',
  '',
  'typedef struct oracle_geometry_glyph {',
  '    uint8_t     strokes;',
  '    uint16_t    index;  /* 0 .. 4M-1 */',
  '    uint8_t     dotted; /* 1: a filled circle at (0, -1.75), radius 0.3 */',
  '    const char *path;',
  '} oracle_geometry_glyph_t;',
  '',
  `#define ORACLE_GEOMETRY_GLYPH_COUNT ${glyphs.length}`,
  'ORACLE_GEOMETRY_DATA oracle_geometry_glyph_t ORACLE_GEOMETRY_GLYPH[ORACLE_GEOMETRY_GLYPH_COUNT] = {',
);
for (const g of glyphs) o.push(`    {${g.strokes}, ${g.index}, ${g.dotted}, ${cString(g.path)}},`);
o.push(
  '};',
  '',
  '/* ---- glyph bounds: frc_glyph_bounds --------------------------------------------------------------------- */',
  '',
  'typedef struct oracle_geometry_bounds {',
  '    uint8_t strokes;',
  '    int16_t min_x, min_y, max_x, max_y;',
  '} oracle_geometry_bounds_t;',
  '',
  `#define ORACLE_GEOMETRY_BOUNDS_COUNT ${bounds.length}`,
  'ORACLE_GEOMETRY_DATA oracle_geometry_bounds_t ORACLE_GEOMETRY_BOUNDS[ORACLE_GEOMETRY_BOUNDS_COUNT] = {',
);
for (const b of bounds) o.push(`    {${b.strokes}, ${b.minX}, ${b.minY}, ${b.maxX}, ${b.maxY}},`);
o.push(
  '};',
  '',
  '/* ---- arcs: frc_arc_wave_root + frc_arc_child ------------------------------------------------------------- */',
  '',
  'typedef struct oracle_geometry_arc {',
  '    uint8_t  level;',
  '    uint64_t index;     /* global half-cycle index at `level` */',
  '    int8_t   rel_level; /* -1, or the level of the ancestor the drawing is rebased on */',
  '    uint64_t rel_index; /* that ancestor\'s half-cycle index */',
  '    double   cx, cy, radius, start, sweep;',
  '} oracle_geometry_arc_t;',
  '',
  `#define ORACLE_GEOMETRY_ARC_COUNT ${arcs.length}`,
  'ORACLE_GEOMETRY_DATA oracle_geometry_arc_t ORACLE_GEOMETRY_ARC[ORACLE_GEOMETRY_ARC_COUNT] = {',
);
for (const v of arcs) {
  o.push(
    `    {${v.level}, UINT64_C(${v.index}), ${v.relLevel}, UINT64_C(${v.relIndex}), ` +
      `${dbl(v.a.center.x)}, ${dbl(v.a.center.y)}, ${dbl(v.a.radius)}, ${dbl(v.a.start)}, ${dbl(v.a.sweep)}},`,
  );
}
o.push('};', '', '#endif /* ORACLE_GEOMETRY_H */', '');

fs.mkdirSync(opts.out, { recursive: true });
const outPath = path.join(opts.out, 'oracle_geometry.h');
fs.writeFileSync(outPath, o.join('\n'));
console.log(`${path.relative(process.cwd(), outPath)}: ${glyphs.length} glyph paths, ${bounds.length} bounds, ${arcs.length} arcs`);
