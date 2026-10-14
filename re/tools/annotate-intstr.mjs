// Print a Ghidra decompile with the internal strings it references inlined (does not modify the file).
// `*(int *)&int_str_array->field_0x10 + 0xOFF` indexes Data/internal_strings.xml: element OFF/0x14 in file order
// (docs/renderstates.md, "Name resolution").
//   node re/tools/annotate-intstr.mjs <file.c>... [--install ~/Games/RiseOfNations] [--only]
//   --only: print just "offset<TAB>string" for each distinct reference
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const args = process.argv.slice(2);
const ii = args.indexOf('--install');
const install = ii >= 0 ? args.splice(ii, 2)[1] : path.join(os.homedir(), 'Games/RiseOfNations');
const only = args.includes('--only');
const xml = fs.readFileSync(path.join(install, 'Data/internal_strings.xml'), 'utf8');
const strs = [...xml.matchAll(/<STRING\b[^>]*?(?:\/>|>([\s\S]*?)<\/STRING>)/g)].map(m => m[1] ?? '');
const lookup = hex => { const off = parseInt(hex, 16); return off % 0x14 ? undefined : strs[off / 0x14]; };

const seen = new Map();
for (const f of args.filter(a => !a.startsWith('--'))) {
  const src = fs.readFileSync(f, 'utf8');
  // \b after the hex digits stops the match from ending inside a longer literal.
  const out = src.replace(/(int_str_array->field_0x10 \+ 0x([0-9a-f]+)\b\)?)/g, (all, ref, hex) => {
    const s = lookup(hex);
    if (s === undefined) return all;
    seen.set(hex, s);
    return `${ref} /*"${s.replace(/\*\//g, '* /').replace(/\n/g, ' ')}"*/`;
  });
  if (!only) process.stdout.write(out);
}
if (only) for (const [h, s] of seen) console.log(`0x${h}\t${s}`);
