// Glance3D UI layout invariance checker.
//
// Compares two layout dumps of the same scene written by G3DLayoutProbe (G3D_LAYOUT_DUMP): one
// rendered at UI scale 1, one at UI scale S. The layout tests render with DPI_SCALE + --dpi-aware,
// so the window grows by S as well and the logical layout must not change at all. Every item and
// window must then scale by S:
//   - size    |w_S - S*w_1| <= 0.5 + S, and the same for h. The slack covers CalcTextSize rounding
//             up at each scale. A container (a window, or a child window's box in its parent) may
//             also drift 0.5 px per row it stacks: ImGui snaps each row to whole pixels, and at
//             fractional scales those snaps add up down a column of rows.
//   - offset  the gap to the previous item in the same window (the window's own origin for its
//             first item, the parent window or the viewport for a window) scales the same way,
//             within 1 + S. Measuring gaps rather than absolute positions keeps one wrong item from
//             failing everything laid out after it.
//   - presence  an item of a root window exists at both scales (child windows host list clippers,
//             whose edge rows may legitimately differ; those are only reported).
// A length read in the wrong unit breaks exactly these relations: a size scaled twice grows by S*S,
// one never scaled stays where it was.
//
// Item ids are turned back into readable paths (window/pushed ids/label) by re-hashing string
// literals from the sources with Dear ImGui's own id hash, checked against the probe's sample.
//
// Usage:  node scripts/compare-ui-layout.mjs <scale1.json> <scaleS.json>
//             [--allow <file> --scene <name>]
// The allowlist (testing/ui-layout-allow.json) maps a scene name ("*" = every scene) to entries
//   { "match": "<glob on the item path>", "check": "size|offset|presence|*",
//     "scales": [1.5, 2] (optional), "bug": true|false, "reason": "..." }
// A finding an entry matches is listed but does not fail; an entry that matched nothing is flagged
// as stale, so the list shrinks as the bugs it records are fixed.
// Exit code: 0 clean (allowlisted findings are still listed), 1 findings, 2 unusable input.

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

// --- Dear ImGui's id hash ---------------------------------------------------------------------
// ImHashStr / ImHashData (imgui.cpp): CRC32C, both through the lookup table and SSE4.2, with a
// "###" in a string resetting the hash back to the seed.
const CRC32C = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? (c >>> 1) ^ 0x82f63b78 : c >>> 1;
    }
    t[n] = c >>> 0;
  }
  return t;
})();

function hashBytes(bytes, seed, isString) {
  const start = ~seed >>> 0;
  let crc = start;
  for (let i = 0; i < bytes.length; i++) {
    const c = bytes[i];
    if (isString && c === 0x23 && bytes[i + 1] === 0x23 && bytes[i + 2] === 0x23) {
      crc = start;
      i += 2;
      continue;
    }
    crc = (crc >>> 8) ^ CRC32C[(crc ^ c) & 0xff];
  }
  return ~crc >>> 0;
}

const hashStr = (s, seed) => hashBytes(Buffer.from(s, 'utf8'), seed, true);
function hashInt(n, seed) {
  const b = Buffer.alloc(4);
  b.writeInt32LE(n);
  return hashBytes(b, seed, false);
}

// --- dictionary for turning ids back into names -----------------------------------------------
const DICT_DIRS = ['vtkext/private/module', 'external/imgui'];
const LITERAL = /"((?:[^"\\\n]|\\.)*)"/g;

function unescapeLiteral(s) {
  return s.replace(/\\(["\\nt])/g, (_, c) => (c === 'n' ? '\n' : c === 't' ? '\t' : c));
}

function buildDictionary() {
  const words = new Set();
  const visit = (dir) => {
    if (!fs.existsSync(dir)) return;
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) {
        visit(full);
      } else if (/\.(cxx|cpp|h)$/.test(entry.name)) {
        const text = fs.readFileSync(full, 'utf8');
        for (const m of text.matchAll(LITERAL)) {
          if (m[1].length > 0 && m[1].length < 96) words.add(unescapeLiteral(m[1]));
        }
      }
    }
  };
  for (const d of DICT_DIRS) visit(path.join(root, d));
  // Translated labels double as ids (a text button is identified by its caption).
  const locales = path.join(root, 'resources', 'locales');
  if (fs.existsSync(locales)) {
    for (const f of fs.readdirSync(locales).filter((n) => n.endsWith('.json'))) {
      try {
        const catalog = JSON.parse(fs.readFileSync(path.join(locales, f), 'utf8'));
        for (const [k, v] of Object.entries(catalog)) {
          if (typeof k === 'string') words.add(k);
          if (typeof v === 'string') words.add(v);
        }
      } catch {
        // an unreadable catalog only costs readable names
      }
    }
  }
  return [...words];
}

class Resolver {
  constructor(words) {
    this.words = words;
    this.bySeed = new Map();
  }
  table(seed) {
    let t = this.bySeed.get(seed);
    if (!t) {
      t = new Map();
      for (const w of this.words) t.set(hashStr(w, seed), w);
      for (let n = 0; n < 4096; n++) {
        const h = hashInt(n, seed);
        if (!t.has(h)) t.set(h, String(n));
      }
      this.bySeed.set(seed, t);
    }
    return t;
  }
  name(id, seed) {
    return this.table(seed).get(id) ?? `#${id.toString(16).padStart(8, '0')}`;
  }
}

// --- dumps ------------------------------------------------------------------------------------
function load(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (e) {
    console.error(`cannot read layout dump ${file}: ${e.message}`);
    process.exit(2);
  }
}

// Index a dump: stable keys (id + occurrence), readable paths, windows and their item order.
function index(dump, resolver) {
  const seen = new Map();
  const items = new Map();
  const windows = new Map();
  const order = new Map(); // winId -> [key...] of the window's items, in submission order
  for (const raw of dump.items) {
    const occ = seen.get(raw.id) ?? 0;
    seen.set(raw.id, occ + 1);
    const key = `${raw.id}#${occ}`;
    const [x0, y0, x1, y1] = raw.r;
    const item = { ...raw, key, x0, y0, x1, y1, w: x1 - x0, h: y1 - y0 };
    if (raw.isWin) {
      item.path = raw.win;
      // A child window is named "<parent>/<name>_<id>", the id being the box it occupies in the
      // parent -- which is the item record that places it.
      const m = raw.child ? /_([0-9A-F]{8})$/.exec(raw.win) : null;
      if (m) item.childItemId = parseInt(m[1], 16);
      windows.set(raw.winId, item);
    } else {
      const hops = [];
      for (let i = 1; i < raw.seeds.length; i++) hops.push(resolver.name(raw.seeds[i], raw.seeds[i - 1]));
      const last = raw.seeds.length > 0 ? raw.seeds[raw.seeds.length - 1] : 0;
      const label = raw.label && hashStr(raw.label, last) === raw.id ? raw.label : resolver.name(raw.id, last);
      item.path = [raw.win, ...hops, label].join('/');
      if (!order.has(raw.winId)) order.set(raw.winId, []);
      order.get(raw.winId).push(key);
    }
    items.set(key, item);
  }
  // Rows a window stacks (distinct item tops), and which item record is a child window's box.
  const rows = new Map();
  for (const [winId, keys] of order) {
    rows.set(winId, new Set(keys.map((key) => Math.round(items.get(key).y0))).size);
  }
  const boxOf = new Map();
  for (const w of windows.values()) {
    if (w.childItemId !== undefined) boxOf.set(w.childItemId, w.winId);
  }
  for (const item of items.values()) {
    if (!item.isWin && boxOf.has(item.id)) item.boxOf = boxOf.get(item.id);
  }
  return { items, windows, order, rows };
}

// Per axis, measure from the previous item's far edge when this item starts past it (next row,
// next column) and from its near edge when they share the row/column -- so a previous item of the
// wrong size never shifts what follows it.
function gap(cur, ref, axis) {
  const lo = axis === 'x' ? 'x0' : 'y0';
  const hi = axis === 'x' ? 'x1' : 'y1';
  return cur[lo] >= ref[hi] - 0.5 ? { edge: hi, v: cur[lo] - ref[hi] } : { edge: lo, v: cur[lo] - ref[lo] };
}

// --- main -------------------------------------------------------------------------------------
const args = process.argv.slice(2);
const files = [];
let allowFile = null;
let scene = null;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--allow') allowFile = args[++i];
  else if (args[i] === '--scene') scene = args[++i];
  else files.push(args[i]);
}
if (files.length !== 2) {
  console.error(
    'usage: node scripts/compare-ui-layout.mjs <scale1.json> <scaleS.json> [--allow <file> --scene <name>]');
  process.exit(2);
}

const base = load(files[0]);
const scaled = load(files[1]);
for (const [dump, file] of [[base, files[0]], [scaled, files[1]]]) {
  const check = dump.hashCheck;
  if (!check || hashStr(check.text, 0) !== check.id) {
    console.error(`${file}: this checker's copy of ImHashStr disagrees with the build's; ids cannot be named`);
    process.exit(2);
  }
}

const kFont = scaled.fontSize / base.fontSize;
const kDisplay = scaled.display[0] / base.display[0];
// Windows are placed against the viewport, which grows by the DPI scale; everything inside by the
// UI scale. They coincide whenever 14 * S is a whole number of pixels -- the scales the tests use.
const strict = Math.abs(kFont - kDisplay) < 1e-3;
const k = kFont;
// ImGui scales its style metrics with ScaleAllSizes, which truncates each to whole pixels, and it
// truncates every line position after them. At 1.5 and 2 the metrics the UI uses (paddings 2 4 10,
// item spacing 4 8, scrollbar 12) land on whole pixels and none of that happens; at 18/14 (125% as
// rendered) they are all cut -- WindowPadding 12.86 -> 12, ItemSpacing 5.14 -> 5 -- and a box comes
// out up to a pixel short per padding it includes, a stack of rows a pixel per row. That is
// rounding, not a unit error (a unit error is S x off: 28% of any length at 18/14), so such a scale
// gets that much more room: two truncated paddings per box, one per gap, a pixel per stacked row,
// and never less than 1% of the length -- a second scale factor, the drift an 18/14 run exists to
// catch, is 2.9% off.
const styleExact = [2, 4, 8, 10, 12].every((v) => Math.abs(v * k - Math.round(v * k)) < 1e-3);
const sizeTol = 0.5 + k + (styleExact ? 0 : 2);
const offsetTol = 1 + k + (styleExact ? 0 : 1);
const rowTol = styleExact ? 0.5 : 1;
const relTol = styleExact ? 0 : 0.01;

const resolver = new Resolver(buildDictionary());
const A = index(base, resolver);
const B = index(scaled, resolver);

const findings = [];
const report = (check, a, detail) => findings.push({ check, path: a.path, detail });

// @p rows: rows the box stacks, when it is a container whose height sums them.
function checkSize(a, b, rows = 0) {
  for (const [dim, label] of [['w', 'width'], ['h', 'height']]) {
    const want = a[dim] * k;
    const tol = Math.max(dim === 'h' ? sizeTol + rowTol * rows : sizeTol, relTol * want);
    if (Math.abs(b[dim] - want) > tol) {
      report('size', a, `${label} ${b[dim].toFixed(2)} vs ${want.toFixed(2)} (1x: ${a[dim].toFixed(2)}, tol ${tol.toFixed(2)})`);
    }
  }
}

function checkOffset(a, b, refA, refB, what) {
  for (const axis of ['x', 'y']) {
    const ga = gap(a, refA, axis);
    const lo = axis === 'x' ? 'x0' : 'y0';
    const gb = b[lo] - refB[ga.edge];
    const want = ga.v * k;
    const tol = Math.max(offsetTol, relTol * Math.abs(want));
    if (Math.abs(gb - want) > tol) {
      report('offset', a, `${axis} gap to ${what} ${gb.toFixed(2)} vs ${want.toFixed(2)} (1x: ${ga.v.toFixed(2)}, tol ${tol.toFixed(2)})`);
    }
  }
}

const origin = { x0: 0, y0: 0, x1: 0, y1: 0 };
for (const [winId, wa] of A.windows) {
  const wb = B.windows.get(winId);
  if (!wb) {
    report(wa.child ? 'presence-child' : 'presence', wa, 'window missing at the scaled run');
    continue;
  }
  // A child window is checked through its box in the parent, which is laid out -- and measured --
  // like any other item there; measuring it from the parent's origin as well would blame it for
  // everything above it.
  if (wa.child) continue;
  checkSize(wa, wb, A.rows.get(winId) ?? 0);
  if (strict) checkOffset(wa, wb, origin, origin, 'viewport');
}
for (const [winId] of B.windows) {
  if (!A.windows.has(winId)) {
    const wb = B.windows.get(winId);
    report(wb.child ? 'presence-child' : 'presence', wb, 'window only at the scaled run');
  }
}

for (const [winId, keys] of A.order) {
  const wa = A.windows.get(winId);
  const wb = B.windows.get(winId);
  const childWindow = !!(wa && wa.child);
  let prevA = null;
  let prevB = null;
  for (const key of keys) {
    const a = A.items.get(key);
    const b = B.items.get(key);
    if (!b) {
      report(childWindow ? 'presence-child' : 'presence', a, 'item missing at the scaled run');
      continue;
    }
    checkSize(a, b, a.boxOf !== undefined ? A.rows.get(a.boxOf) ?? 0 : 0);
    // The first item is measured from the window's origin.
    const refA = prevA ?? (wa ? { ...wa, x1: wa.x0, y1: wa.y0 } : null);
    const refB = prevB ?? (wb ? { ...wb, x1: wb.x0, y1: wb.y0 } : null);
    if (refA && refB) checkOffset(a, b, refA, refB, prevA ? 'previous item' : 'window');
    prevA = a;
    prevB = b;
  }
}
for (const [key, b] of B.items) {
  if (!b.isWin && !A.items.has(key)) {
    const wb = B.windows.get(b.winId);
    report(wb && wb.child ? 'presence-child' : 'presence', b, 'item only at the scaled run');
  }
}

// --- allowlist --------------------------------------------------------------------------------
let allow = [];
if (allowFile) {
  try {
    const lists = JSON.parse(fs.readFileSync(allowFile, 'utf8'));
    allow = [...(lists['*'] ?? []), ...(scene ? lists[scene] ?? [] : [])];
  } catch (e) {
    console.error(`cannot read allowlist ${allowFile}: ${e.message}`);
    process.exit(2);
  }
}
const globToRegExp = (g) =>
  new RegExp('^' + g.replace(/[.+^${}()|[\]\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.') + '$');
const rules = allow
  .filter((e) => !e.scales || e.scales.some((v) => Math.abs(v - k) < 1e-3))
  .map((e) => ({ ...e, re: globToRegExp(e.match), used: false }));
for (const f of findings) {
  const rule = rules.find((r) => (r.check === '*' || r.check === f.check) && r.re.test(f.path));
  if (rule) {
    rule.used = true;
    f.allowed = rule;
  }
}

// --- report -----------------------------------------------------------------------------------
const name = `${path.basename(files[0])} -> ${path.basename(files[1])}`;
console.log(`UI layout ${name}: scale x${k.toFixed(4)}${strict ? '' : ' (UI only: viewport grew by x' + kDisplay.toFixed(4) + ')'}, ${A.items.size} items`);
const failing = findings.filter((f) => !f.allowed && f.check !== 'presence-child');
const infos = findings.filter((f) => !f.allowed && f.check === 'presence-child');
const allowed = findings.filter((f) => f.allowed);
for (const f of failing) console.log(`  FAIL ${f.check.padEnd(8)} ${f.path}: ${f.detail}`);
for (const f of allowed) console.log(`  allow ${f.check.padEnd(7)} ${f.path}: ${f.detail} -- ${f.allowed.bug ? 'known bug: ' : ''}${f.allowed.reason}`);
for (const f of infos) console.log(`  info  ${f.check.padEnd(7)} ${f.path}: ${f.detail}`);
for (const r of rules.filter((r) => !r.used)) console.log(`  note  allowlist entry matched nothing (stale?): ${r.match} [${r.check}]`);
console.log(failing.length === 0 ? '  OK' : `  ${failing.length} finding(s)`);
process.exit(failing.length === 0 ? 0 : 1);
