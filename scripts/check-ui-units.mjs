// Glance3D UI units lint — a ratchet.
//
// G3DDp / G3DScale (vtkext/private/module/G3DUnits.h) turn double and missed scaling into compile
// errors wherever a length flows through the types. This catches the places the types cannot see,
// in the desktop UI sources (every module file that includes imgui.h, G3DTheme.h, G3DWidgets.h or
// G3DUnits.h, the latter itself excepted):
//
//   raw-exit          .Raw() / .Factor(): the audit exits of G3DDp / G3DScale. Each use is a unit
//                     boundary (ScaleAllSizes, a log line, the OS loupe) or a bug.
//   font-scale        GetFontSize() / ...: a scale computed from the font instead of read from
//                     G3DWidgets::UiScale() — the second scale factor the single scale replaced.
//   bare-number       a bare length >= 2 in a layout call (ImVec2, Dummy, SameLine, PushStyleVar,
//                     SetCursor*, SetNextItemWidth, ...) or a float >= 2 in a draw call (stroke
//                     thickness, rounding): a design constant that never met the scale.
//   constexpr-length  a `constexpr float` named like a length (margin, padding, gap, size, width,
//                     height, radius, inset, spacing, thickness): a design constant that escaped
//                     G3DDp.
//
// A literal next to `*` or `/` is a ratio, not a length (`2.f * pad`), an integer in a draw call is
// a segment count or a flag, and `12_dp` is already a G3DDp: none of those count. Hits are counted
// per file and rule against scripts/ui-units-baseline.json, and a count may go down but never up.
// Code that genuinely needs one of these says why, on the line or the line above:
//
//     // g3d-units: allow(<rule>) <reason>
//
// The reason is mandatory; an allow that no longer suppresses anything is reported as stale.
//
// Usage:  node scripts/check-ui-units.mjs            check against the baseline
//         node scripts/check-ui-units.mjs --list     also print every hit
//         node scripts/check-ui-units.mjs --update   rewrite the baseline from the current counts
// Exit code: 0 clean, 1 a count went up or an allow has no reason, 2 unusable input.

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const MODULE = 'vtkext/private/module';
const BASELINE = path.join(root, 'scripts', 'ui-units-baseline.json');
const argv = process.argv.slice(2);
const LIST = argv.includes('--list');
const UPDATE = argv.includes('--update');

const RULES = ['raw-exit', 'font-scale', 'bare-number', 'constexpr-length'];
const LAYOUT_CALLS = ['ImVec2', 'Dummy', 'SameLine', 'PushStyleVar', 'SetCursorPos', 'SetCursorPosX',
  'SetCursorPosY', 'SetCursorScreenPos', 'SetNextItemWidth', 'PushItemWidth', 'Indent', 'Unindent',
  'InvisibleButton', 'BeginChild', 'SetNextWindowPos', 'SetNextWindowSize'];
const DRAW_CALLS = ['AddLine', 'AddRect', 'AddRectFilled', 'AddCircle', 'AddCircleFilled',
  'AddTriangle', 'AddTriangleFilled', 'AddPolyline', 'AddNgon', 'PathStroke', 'PathRect'];
const LENGTH_NAME = /(margin|padding|pad|gap|size|width|height|radius|rounding|inset|spacing|thickness)/i;
const ALLOW = /\/\/\s*g3d-units:\s*allow\(([\w-]+)\)\s*(.*)$/;

// --- source preparation -----------------------------------------------------------------------

// Blank out comments and string / char literals, keeping every newline, so a regex over the result
// only ever sees code and every offset still maps to its line.
function codeOnly(src) {
  let out = '';
  let i = 0;
  const n = src.length;
  const blank = (s) => s.replace(/[^\n]/g, ' ');
  while (i < n) {
    const c = src[i];
    const d = src[i + 1];
    if (c === '/' && d === '/') {
      const end = src.indexOf('\n', i);
      const stop = end < 0 ? n : end;
      out += blank(src.slice(i, stop));
      i = stop;
    } else if (c === '/' && d === '*') {
      const end = src.indexOf('*/', i + 2);
      const stop = end < 0 ? n : end + 2;
      out += blank(src.slice(i, stop));
      i = stop;
    } else if (c === 'R' && d === '"' && !/\w/.test(src[i - 1] ?? '')) {
      // raw string R"delim( ... )delim"
      const open = src.indexOf('(', i + 2);
      const delim = src.slice(i + 2, open);
      const end = src.indexOf(`)${delim}"`, open);
      const stop = end < 0 ? n : end + delim.length + 2;
      out += blank(src.slice(i, stop));
      i = stop;
    } else if (c === '"' || (c === "'" && !/[0-9A-Fa-f]/.test(src[i - 1] ?? ''))) {
      let j = i + 1;
      while (j < n && src[j] !== c && src[j] !== '\n') {
        j += src[j] === '\\' ? 2 : 1;
      }
      out += blank(src.slice(i, j + 1));
      i = j + 1;
    } else {
      out += c;
      i += 1;
    }
  }
  return out;
}

function lineStarts(src) {
  const starts = [0];
  for (let i = 0; i < src.length; i++) {
    if (src[i] === '\n') starts.push(i + 1);
  }
  return starts;
}

function lineOf(starts, offset) {
  let lo = 0;
  let hi = starts.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (starts[mid] <= offset) lo = mid;
    else hi = mid - 1;
  }
  return lo + 1; // 1-based
}

// The argument text of the call whose '(' is at `open`, up to the matching ')'.
function callArgs(code, open) {
  let depth = 0;
  for (let i = open; i < code.length; i++) {
    if (code[i] === '(') depth++;
    else if (code[i] === ')' && --depth === 0) return { start: open + 1, end: i };
  }
  return { start: open + 1, end: open + 1 };
}

// The nearest non-blank character before / after a span.
function neighbour(code, from, step) {
  for (let i = from; i >= 0 && i < code.length; i += step) {
    if (!/\s/.test(code[i])) return code[i];
  }
  return '';
}

// --- the rules --------------------------------------------------------------------------------

function scan(code) {
  const hits = []; // { rule, offset }
  let m;

  const rawRe = /\.\s*(Raw|Factor)\s*\(\s*\)/g;
  while ((m = rawRe.exec(code)) !== null) hits.push({ rule: 'raw-exit', offset: m.index });

  const fontRe = /GetFontSize\s*\(\s*\)\s*\//g;
  while ((m = fontRe.exec(code)) !== null) hits.push({ rule: 'font-scale', offset: m.index });

  const cxRe = /\bconstexpr\s+float\s+(\w+)/g;
  while ((m = cxRe.exec(code)) !== null) {
    if (LENGTH_NAME.test(m[1])) hits.push({ rule: 'constexpr-length', offset: m.index });
  }

  const seen = new Set();
  const numRe = /(?<![\w.])(\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fF]?(?![\w.])/g;
  for (const [names, floatsOnly] of [[LAYOUT_CALLS, false], [DRAW_CALLS, true]]) {
    const callRe = new RegExp(String.raw`\b(${names.join('|')})\s*\(`, 'g');
    while ((m = callRe.exec(code)) !== null) {
      const { start, end } = callArgs(code, m.index + m[0].length - 1);
      const args = code.slice(start, end);
      let n;
      numRe.lastIndex = 0;
      while ((n = numRe.exec(args)) !== null) {
        const offset = start + n.index;
        const text = n[0];
        const value = parseFloat(text);
        if (seen.has(offset) || !(value >= 2)) continue;
        if (floatsOnly && !/[.fF]/.test(text)) continue; // a segment count / flag, not a length
        const before = neighbour(code, offset - 1, -1);
        const after = neighbour(code, offset + text.length, 1);
        if ('*/'.includes(before) && before !== '' || '*/'.includes(after) && after !== '') continue;
        if (before === '[') continue; // an index
        seen.add(offset);
        hits.push({ rule: 'bare-number', offset });
      }
    }
  }
  return hits;
}

// --- files ------------------------------------------------------------------------------------

function uiFiles() {
  const dir = path.join(root, MODULE);
  const include = /#include\s+[<"](imgui\.h|G3DTheme\.h|G3DWidgets\.h|G3DUnits\.h)[>"]/;
  return fs
    .readdirSync(dir)
    .filter((f) => /\.(h|cxx)$/.test(f) && f !== 'G3DUnits.h')
    .filter((f) => include.test(fs.readFileSync(path.join(dir, f), 'utf8')))
    .sort()
    .map((f) => `${MODULE}/${f}`);
}

// --- self test: the rules still see what they exist to catch, and leave the rest alone ---------

{
  const sample = [
    'ImGui::Dummy(ImVec2(0.f, 8.f));', // bare-number
    'ImGui::SameLine(0.f, 6.f);', // bare-number
    'dl->AddRectFilled(a, b, col, 4.0f);', // bare-number: a float in a draw call
    'constexpr float margin = 5.f;', // constexpr-length
    'const float k = ImGui::GetFontSize() / 14.f;', // font-scale
    'const float r = G3DTheme::Radius::Card.Raw();', // raw-exit
    'ImGui::Dummy(ImVec2(0.f, 12_dp * s));', // a G3DDp
    'ImGui::Dummy(ImVec2(ts.x + 2.f * padX, 0.f));', // a ratio
    'dl->AddCircleFilled(c, r, col, 24);', // a segment count
    '// ImGui::Dummy(ImVec2(0.f, 8.f));', // a comment
    'const char* t = "ImGui::SameLine(0.f, 9.f)";', // a string
  ].join('\n');
  const got = scan(codeOnly(sample)).map((h) => h.rule).sort().join(',');
  const want = 'bare-number,bare-number,bare-number,constexpr-length,font-scale,raw-exit';
  if (got !== want) {
    console.error(`check-ui-units self test failed: got [${got}], want [${want}]`);
    process.exit(2);
  }
}

// --- main -------------------------------------------------------------------------------------

const counts = {}; // file -> rule -> n
const listing = {}; // file -> rule -> [{ line, text }]
const errors = [];
const stale = [];

for (const rel of uiFiles()) {
  const src = fs.readFileSync(path.join(root, rel), 'utf8');
  const lines = src.split('\n');
  const starts = lineStarts(src);
  const code = codeOnly(src);

  // allow directives: line number -> { rule, reason, used }
  const allows = new Map();
  lines.forEach((text, idx) => {
    const a = ALLOW.exec(text);
    if (!a) return;
    const reason = a[2].trim();
    if (!RULES.includes(a[1])) errors.push(`${rel}:${idx + 1}: unknown rule in allow(${a[1]})`);
    else if (!reason) errors.push(`${rel}:${idx + 1}: allow(${a[1]}) needs a reason`);
    allows.set(idx + 1, { rule: a[1], used: false });
  });

  for (const hit of scan(code)) {
    const line = lineOf(starts, hit.offset);
    const allow = [line, line - 1]
      .map((l) => allows.get(l))
      .find((a) => a && a.rule === hit.rule);
    if (allow) {
      allow.used = true;
      continue;
    }
    counts[rel] ??= {};
    counts[rel][hit.rule] = (counts[rel][hit.rule] ?? 0) + 1;
    listing[rel] ??= {};
    (listing[rel][hit.rule] ??= []).push({ line, text: lines[line - 1].trim() });
  }
  for (const [line, a] of allows) {
    if (!a.used && RULES.includes(a.rule)) stale.push(`${rel}:${line}: allow(${a.rule}) suppresses nothing`);
  }
}

if (UPDATE) {
  const out = {
    _comment:
      'Hit counts of scripts/check-ui-units.mjs per file and rule. A count may go down, never up: ' +
      'fix the hit, or mark a legitimate one with // g3d-units: allow(<rule>) <reason>, then ' +
      're-run the script with --update.',
  };
  for (const file of Object.keys(counts).sort()) out[file] = counts[file];
  fs.writeFileSync(BASELINE, JSON.stringify(out, null, 2) + '\n');
  console.log(`baseline written: ${path.relative(root, BASELINE)}`);
}

let baseline = {};
try {
  baseline = JSON.parse(fs.readFileSync(BASELINE, 'utf8'));
} catch (e) {
  console.error(`cannot read ${path.relative(root, BASELINE)}: ${e.message}`);
  process.exit(2);
}

const increases = [];
const decreases = [];
const files = new Set([...Object.keys(counts), ...Object.keys(baseline).filter((k) => !k.startsWith('_'))]);
for (const file of [...files].sort()) {
  for (const rule of RULES) {
    const now = counts[file]?.[rule] ?? 0;
    const was = baseline[file]?.[rule] ?? 0;
    if (now > was) increases.push({ file, rule, now, was });
    else if (now < was) decreases.push({ file, rule, now, was });
  }
}

const total = Object.values(counts).reduce(
  (sum, byRule) => sum + Object.values(byRule).reduce((a, b) => a + b, 0), 0);
console.log(`UI units lint: ${uiFiles().length} files, ${total} baselined hit(s)`);
if (LIST) {
  for (const file of Object.keys(listing).sort()) {
    for (const rule of RULES) {
      for (const h of listing[file][rule] ?? []) console.log(`  ${rule.padEnd(16)} ${file}:${h.line}: ${h.text}`);
    }
  }
}
for (const e of errors) console.log(`  ERROR ${e}`);
for (const inc of increases) {
  console.log(`  FAIL  ${inc.file}: ${inc.rule} ${inc.was} -> ${inc.now}`);
  for (const h of listing[inc.file]?.[inc.rule] ?? []) console.log(`          ${inc.file}:${h.line}: ${h.text}`);
}
for (const dec of decreases) {
  console.log(`  note  ${dec.file}: ${dec.rule} ${dec.was} -> ${dec.now} (run with --update to lock it in)`);
}
for (const s of stale) console.log(`  note  ${s}`);
if (!increases.length && !errors.length) console.log('  OK');
process.exit(increases.length || errors.length ? 1 : 0);
