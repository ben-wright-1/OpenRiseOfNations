// Summarize an MSVC linker .map: functions grouped by object file and by C++ class.
// Usage: node mapsummary.mjs <file.map> <outdir>
import fs from 'node:fs';
import path from 'node:path';

const [, , mapPath, outDir = '.'] = process.argv;
const lines = fs.readFileSync(mapPath, 'latin1').split(/\r?\n/);

// " 0001:00019e80       ?Foo@Bar@@QAEXXZ   00419e80 f   lib:obj.obj"
const re = /^\s*([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})\s+(f\s+)?(i\s+)?(.*)$/i;

// Pull "Class" (and namespace chain) out of an MSVC decorated name.
function classOf(sym) {
  if (!sym.startsWith('?')) return null;
  let s = sym.slice(1);
  if (s.startsWith('?')) {
    // special member: ??0 ctor, ??1 dtor, ??_G, ??4 operator= ...
    s = s.slice(1).replace(/^(_[0-9A-Z]|[0-9A-Z])/, '');
    const scope = s.split('@@')[0].split('@').filter(Boolean);
    return scope.length ? scope.reverse().join('::') : null;
  }
  const parts = s.split('@@')[0].split('@');
  if (parts.length < 2) return null; // free function
  return parts.slice(1).filter(Boolean).reverse().join('::');
}
function methodOf(sym) {
  if (!sym.startsWith('?')) return sym.replace(/^_/, '').replace(/@\d+$/, '');
  if (sym.startsWith('??0')) return '<ctor>';
  if (sym.startsWith('??1')) return '<dtor>';
  if (sym.startsWith('??')) return '<special>';
  return sym.slice(1).split('@')[0];
}

const funcs = [];
for (const line of lines) {
  const m = re.exec(line);
  if (!m || m[1] !== '0001' || !m[5]) continue; // code section, functions only
  const owner = m[7].trim();
  const [lib, obj] = owner.includes(':') ? owner.split(':') : ['', owner];
  funcs.push({ rva: m[4], sym: m[3], lib, obj, cls: classOf(m[3]), method: methodOf(m[3]) });
}

const byObj = new Map();
const byClass = new Map();
for (const f of funcs) {
  const key = (f.lib ? f.lib + ':' : '') + f.obj;
  if (!byObj.has(key)) byObj.set(key, { count: 0, classes: new Map() });
  const o = byObj.get(key);
  o.count++;
  if (f.cls) o.classes.set(f.cls, (o.classes.get(f.cls) || 0) + 1);
  if (f.cls) {
    if (!byClass.has(f.cls)) byClass.set(f.cls, { count: 0, objs: new Set(), methods: new Set() });
    const c = byClass.get(f.cls);
    c.count++; c.objs.add(key); c.methods.add(f.method);
  }
}

fs.mkdirSync(outDir, { recursive: true });
const objRows = [...byObj].sort((a, b) => b[1].count - a[1].count).map(([k, v]) =>
  `${String(v.count).padStart(5)}  ${k}  | ${[...v.classes].sort((a, b) => b[1] - a[1]).slice(0, 6).map(([c, n]) => `${c}(${n})`).join(', ')}`);
fs.writeFileSync(path.join(outDir, 'modules.txt'), objRows.join('\n') + '\n');

const clsRows = [...byClass].sort((a, b) => b[1].count - a[1].count).map(([k, v]) =>
  `${String(v.count).padStart(5)}  ${k}  [${[...v.objs].join(' ')}]`);
fs.writeFileSync(path.join(outDir, 'classes.txt'), clsRows.join('\n') + '\n');

fs.writeFileSync(path.join(outDir, 'functions.tsv'),
  'rva\tobj\tclass\tmethod\tsymbol\n' + funcs.map(f => `${f.rva}\t${f.lib ? f.lib + ':' : ''}${f.obj}\t${f.cls ?? ''}\t${f.method}\t${f.sym}`).join('\n') + '\n');

const gameObjs = [...byObj.keys()].filter(k => !k.includes(':'));
console.log(`functions: ${funcs.length}, object files: ${byObj.size} (${gameObjs.length} without a library prefix), classes: ${byClass.size}`);
