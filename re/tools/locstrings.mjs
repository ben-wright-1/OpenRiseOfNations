// Localised string tables (docs/ui-spec.md §3.5): internal_strings.xml (int_str_array), translated_strings.xml
// (loc_str_array), string_lookup.xml (String::lookup by category), and the String hash (String::generate_hash).
//
//   node re/tools/locstrings.mjs hash "text"                 case-sensitive and insensitive hashes
//   node re/tools/locstrings.mjs int 0x1b4b8 | 5590          internal string by byte offset (multiple of 0x14) or index
//   node re/tools/locstrings.mjs loc 0x2a30 | 543 [--lang .7] translated string by offset or index
//   node re/tools/locstrings.mjs lookup "Game Rules:" [CATEGORY] [--lang .12]
//                                                            String::lookup: find the STRING whose hash matches
//   node re/tools/locstrings.mjs cats [--lang .7]            string_lookup categories, names recovered by hashing
//                                                            every internal string
//   node re/tools/locstrings.mjs check                       verify the hash against every hashed table
// --lang takes the file suffix (.7 German, .10 Spanish, .12 French, .16 Italian; English is the bare file).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(`--${n}`); return i >= 0 ? argv.splice(i, 2)[1] : d; };
const install = opt('install', path.join(os.homedir(), 'Games/RiseOfNations'));
const lang = opt('lang', '');
const [cmd, a1, a2] = argv;

// basic:str.obj primes[50] (0x00b14500) and String::generate_hash @00a1b6b0
const PRIMES = [127, 811, 1597, 2131, 2749, 4759, 5527, 5953, 8117, 9539, 10273, 10753, 11159, 12301, 13217, 14207,
  15413, 17681, 18661, 19013, 21089, 22051, 25111, 25801, 27457, 28057, 29581, 30809, 32611, 34469, 36067, 37511,
  38723, 40093, 41983, 43321, 45083, 47431, 49667, 50767, 53453, 55469, 57193, 59369, 61987, 65071, 73421, 77849,
  84223, 89009];
export function strHash(s) {
  // Walk from the last UTF-16 unit to the first; n counts down from the length. Sums wrap at 32 bits.
  let h = 0, hi = 0, n = s.length;
  for (let u = n - 1; u >= 0; u--) {
    const c = s.charCodeAt(u), l = s[u].toLowerCase().charCodeAt(0); // towlower
    h = (h + PRIMES[u % 50] * c + PRIMES[c % 50] * n) >>> 0;
    hi = (hi + PRIMES[l % 50] * n + l * PRIMES[u % 50]) >>> 0;
    n--;
  }
  return { hash: h, hash_i: hi };
}

const unescape = s => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&apos;/g, "'")
  .replace(/&#(\d+);/g, (_, d) => String.fromCharCode(+d)).replace(/&#x([0-9a-f]+);/gi, (_, h) => String.fromCharCode(parseInt(h, 16)))
  .replace(/&amp;/g, '&');
function readFile(name) {
  let fn = path.join(install, 'Data', name + lang);
  if (!fs.existsSync(fn)) fn = path.join(install, 'Data', name);
  return fs.readFileSync(fn, 'utf8');
}
// StringTable::init @00a28520: every <STRING> under ROOT in file order; element i lives at byte offset i * 0x14
// (sizeof(String)) in the table's array, which is how compiled code addresses it.
function table(name) {
  return [...readFile(name).matchAll(/<STRING\b([^>]*?)(?:\/>|>([\s\S]*?)<\/STRING>)/g)].map(m => ({
    attrs: Object.fromEntries([...m[1].matchAll(/(\w+)="([^"]*)"/g)].map(x => [x[1], x[2]])), text: unescape(m[2] ?? ''),
  }));
}
const idx = v => (/^0x/i.test(v) ? parseInt(v, 16) / 0x14 : +v);
function lookupCats() {
  const xml = readFile('string_lookup.xml'), cats = new Map();
  for (const m of xml.matchAll(/<CAT hash="(\d+)"[^>]*>([\s\S]*?)<\/CAT>/g)) {
    cats.set(+m[1], [...m[2].matchAll(/<STRING\b([^>]*?)(?:\/>|>([\s\S]*?)<\/STRING>)/g)].map(s => ({
      hash: +(/hash="(\d+)"/.exec(s[1])?.[1] ?? -1), hash_i: +(/hash_i="(\d+)"/.exec(s[1])?.[1] ?? -1), text: unescape(s[2] ?? ''),
    })));
  }
  return cats;
}
function catNames() {
  const names = new Map();
  for (const s of table('internal_strings.xml')) names.set(strHash(s.text).hash, s.text);
  return names;
}

if (cmd === 'hash') {
  console.log(JSON.stringify({ text: a1, ...strHash(a1) }));
} else if (cmd === 'int' || cmd === 'loc') {
  const t = table(cmd === 'int' ? 'internal_strings.xml' : 'translated_strings.xml'), i = idx(a1);
  if (!Number.isInteger(i)) throw new Error('offset is not a multiple of 0x14');
  console.log(`#${i} @0x${(i * 0x14).toString(16)}: ${JSON.stringify(t[i]?.text)}`);
} else if (cmd === 'lookup') {
  // String::lookup @00a1ddb0: hash the text and the category, find CAT by hash then STRING by hash (or hash_i).
  const cats = lookupCats(), names = catNames(), { hash, hash_i } = strHash(a1);
  const list = a2 ? [[strHash(a2).hash, cats.get(strHash(a2).hash) ?? []]] : [...cats];
  for (const [ch, strs] of list) {
    const hit = strs.find(s => s.hash === hash) ?? strs.find(s => s.hash_i === hash_i);
    if (hit) console.log(`${names.get(ch) ?? ch}: ${JSON.stringify(hit.text)}`);
  }
} else if (cmd === 'cats') {
  const cats = lookupCats(), names = catNames();
  for (const [h, s] of cats) console.log(`${String(h).padStart(10)} ${(names.get(h) ?? '?').padEnd(24)} ${s.length} strings`);
} else if (cmd === 'check') {
  let ok = 0, bad = 0;
  const tally = (want, text) => { if (want === strHash(text).hash) ok++; else bad++; };
  for (const s of table('internal_strings.xml')) if (s.attrs.hash) tally(+s.attrs.hash, s.text);
  console.log(`internal_strings.xml: ${ok} hash matches, ${bad} mismatches`);
  ok = bad = 0;
  for (const strs of lookupCats().values()) for (const s of strs) tally(s.hash, s.text);
  console.log(`string_lookup.xml (English text = key): ${ok} matches, ${bad} mismatches`);
  ok = bad = 0;
  for (const s of table('typenames.xml')) if (s.attrs.hash) tally(+s.attrs.hash, s.text);
  console.log(`typenames.xml: ${ok} matches, ${bad} mismatches`);
  ok = bad = 0;
  for (const s of table('translated_strings.xml')) if (s.attrs.hash) tally(+s.attrs.hash, s.text);
  console.log(`translated_strings.xml: ${ok} matches, ${bad} mismatches (positional table; hash is not of the text)`);
} else {
  console.error('usage: locstrings.mjs hash|int|loc|lookup|cats|check ...');
  process.exit(2);
}
