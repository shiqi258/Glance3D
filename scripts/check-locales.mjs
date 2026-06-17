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
const STR = String.raw`"((?:[^"\\]|\\.)*)"`;
// translate(...) / Translate(...) / tr(...) with a single string-literal first arg.
const CALL_RE = new RegExp(String.raw`(?:\bt(?:ranslate|r)|\bTranslate)\s*\(\s*${STR}`, 'g');

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
      keys.add(unescapeLiteral(m[1]));
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

process.exit(missing.length > 0 ? 1 : 0);
