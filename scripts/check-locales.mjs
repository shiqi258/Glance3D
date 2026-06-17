// Glance3D i18n catalog checker.
//
// Scans the source for translation calls (translate("..."), Translate("..."),
// tr("...")) and the CLI option help texts, then compares the collected English
// source-keys against resources/locales/zh-CN.json. Reports:
//   - missing: source-keys with no zh-CN translation (will fall back to English),
//   - orphan : zh-CN entries no longer referenced by the source.
//
// Usage:  node scripts/check-locales.mjs
// Exit code is non-zero when anything is missing (handy for CI).

import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

// Directories scanned for translation calls.
const SOURCE_DIRS = [
  'application',
  'library/src',
  'vtkext/private/module',
];

// A C++ double-quoted string literal: handles \" and other escapes.
const STR = String.raw`"(?:[^"\\]|\\.)*"`;
// translate(...) / Translate(...) / tr(...) whose first argument is one or more
// adjacent string literals (C++ concatenates them, e.g. "part 1 " "part 2").
const CALL_RE = new RegExp(
  String.raw`(?:\bt(?:ranslate|r)|\bTranslate)\s*\(\s*(${STR}(?:\s*${STR})*)`, 'g');

// Concatenate the adjacent string literals captured as one source-key.
function literalsToKey(seq) {
  const re = /"((?:[^"\\]|\\.)*)"/g;
  let out = '';
  let m;
  while ((m = re.exec(seq)) !== null) {
    out += unescapeLiteral(m[1]);
  }
  return out;
}

function walk(dir, out) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === 'testing') continue;
      walk(full, out);
    } else if (/\.(cxx|cpp|cc|h|hxx)$/.test(entry.name)) {
      out.push(full);
    }
  }
}

// Decode a C/JSON-style escaped literal into the real runtime string.
function unescapeLiteral(s) {
  return s.replace(/\\(["\\nt])/g, (_, c) =>
    c === 'n' ? '\n' : c === 't' ? '\t' : c);
}

// --- ICU MessageFormat subset lint ------------------------------------------
// Mirrors G3DLocaleCore::FormatMessage: validates `{name}`, `{n, number}`,
// `{n, plural, ...}`, `{x, select, ...}` and collects the argument names used.

// Index of the '}' matching the '{' at `open`, honoring nesting; -1 if unbalanced.
function matchBrace(s, open) {
  let depth = 0;
  for (let i = open; i < s.length; i++) {
    if (s[i] === '{') depth++;
    else if (s[i] === '}' && --depth === 0) return i;
  }
  return -1;
}

// Parse a plural/select body into [{ selector, message }].
function parseSubs(body) {
  const subs = [];
  let i = 0;
  while (i < body.length) {
    while (i < body.length && /\s/.test(body[i])) i++;
    if (i >= body.length) break;
    const start = i;
    while (i < body.length && body[i] !== '{' && !/\s/.test(body[i])) i++;
    const selector = body.slice(start, i);
    while (i < body.length && /\s/.test(body[i])) i++;
    if (body[i] !== '{') break;
    const close = matchBrace(body, i);
    if (close === -1) break;
    subs.push({ selector, message: body.slice(i + 1, close) });
    i = close + 1;
  }
  return subs;
}

// Collect argument names and structural errors from an ICU message.
function lintIcu(msg) {
  const args = new Set();
  const errors = [];
  function walk(s) {
    let i = 0;
    while (i < s.length) {
      const c = s[i];
      if (c === '{') {
        const close = matchBrace(s, i);
        if (close === -1) { errors.push('unbalanced "{"'); return; }
        handle(s.slice(i + 1, close));
        i = close + 1;
      } else {
        if (c === '}') { errors.push('unexpected "}"'); return; }
        i++;
      }
    }
  }
  function handle(inner) {
    const c1 = inner.indexOf(',');
    const name = (c1 === -1 ? inner : inner.slice(0, c1)).trim();
    if (name) args.add(name);
    if (c1 === -1) return; // simple {name}
    const rest = inner.slice(c1 + 1);
    const c2 = rest.indexOf(',');
    const type = (c2 === -1 ? rest : rest.slice(0, c2)).trim();
    if (type === 'number') return;
    if (type === 'plural' || type === 'select') {
      const subs = parseSubs(c2 === -1 ? '' : rest.slice(c2 + 1));
      if (!subs.some((sub) => sub.selector === 'other')) {
        errors.push(`${type} {${name}} has no "other" branch`);
      }
      for (const sub of subs) walk(sub.message); // nested placeholders / args
    } else {
      errors.push(`unknown arg type "${type}" in {${name}}`);
    }
  }
  walk(msg);
  return { args, errors };
}

const keys = new Set();

// 1) Translation calls in the source.
for (const d of SOURCE_DIRS) {
  const dir = path.join(root, d);
  if (!fs.existsSync(dir)) continue;
  const files = [];
  walk(dir, files);
  for (const file of files) {
    const txt = fs.readFileSync(file, 'utf8');
    let m;
    while ((m = CALL_RE.exec(txt)) !== null) {
      keys.add(literalsToKey(m[1]));
    }
  }
}

// 2) CLI option help texts (translated at print time by their English source).
const cliPath = path.join(root, 'resources/cli-options.json');
if (fs.existsSync(cliPath)) {
  const cli = JSON.parse(fs.readFileSync(cliPath, 'utf8'));
  for (const group of cli.groups ?? []) {
    for (const opt of group.options ?? []) {
      if (typeof opt.helpText === 'string') keys.add(opt.helpText);
    }
  }
}

// Load the zh-CN catalog.
const zhPath = path.join(root, 'resources/locales/zh-CN.json');
const zh = JSON.parse(fs.readFileSync(zhPath, 'utf8'));
const zhKeys = new Set(Object.keys(zh).filter((k) => !k.startsWith('_meta')));

const missing = [...keys].filter((k) => !zh[k]).sort();
const orphan = [...zhKeys].filter((k) => !keys.has(k)).sort();

console.log(`Scanned source keys: ${keys.size}`);
console.log(`zh-CN entries:       ${zhKeys.size}`);
console.log(`Missing (no zh-CN):  ${missing.length}`);
for (const k of missing) console.log(`  - ${JSON.stringify(k)}`);
console.log(`Orphan (unused zh):  ${orphan.length}`);
for (const k of orphan) console.log(`  ~ ${JSON.stringify(k)}`);

// 3) ICU lint: source keys must be structurally valid; each zh value must be
//    valid and reference exactly the argument names its key declares.
const lintErrors = [];
for (const k of keys) {
  for (const e of lintIcu(k).errors) lintErrors.push(`key ${JSON.stringify(k)}: ${e}`);
}
for (const [k, v] of Object.entries(zh)) {
  if (k.startsWith('_meta') || typeof v !== 'string') continue;
  const keyArgs = lintIcu(k).args;
  const val = lintIcu(v);
  for (const e of val.errors) lintErrors.push(`value ${JSON.stringify(k)}: ${e}`);
  const extra = [...val.args].filter((a) => !keyArgs.has(a));
  const dropped = [...keyArgs].filter((a) => !val.args.has(a));
  if (extra.length || dropped.length) {
    const parts = [];
    if (extra.length) parts.push(`value uses unknown {${extra.join('}, {')}}`);
    if (dropped.length) parts.push(`value omits {${dropped.join('}, {')}}`);
    lintErrors.push(`args ${JSON.stringify(k)}: ${parts.join('; ')}`);
  }
}

console.log(`ICU lint errors:     ${lintErrors.length}`);
for (const e of lintErrors) console.log(`  ! ${e}`);

process.exit(missing.length > 0 || lintErrors.length > 0 ? 1 : 0);
