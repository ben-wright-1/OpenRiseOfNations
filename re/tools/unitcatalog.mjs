// Build the unit graphics catalog (unit_graphics.xml + anim_graphics.xml) and list the art files it references.
// Usage: node unitcatalog.mjs <game-dir> [--files]   (--files: print one relative path per line, for packaging)
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

export function loadCatalog(gameDir) {
  const read = f => fs.readFileSync(path.join(gameDir, 'Data', f), 'latin1');
  const attr = (tag, a) => tag.match(new RegExp(`\\b${a}="([^"]*)"`, 'i'))?.[1];
  const norm = p => p.replace(/^\.[\\/]/, '').replace(/\\/g, '/');

  const animFiles = new Map();
  for (const m of read('anim_graphics.xml').matchAll(/<ANIM\s[^>]*>/g)) {
    animFiles.set(attr(m[0], 'name'), norm(attr(m[0], 'file')));
  }
  const units = [];
  // Each <UNIT ...> opens a block that contains its <ANIM name=".." file="anim name"/> entries.
  const xml = read('unit_graphics.xml');
  const re = /<UNIT\s[^>]*>([\s\S]*?)<\/UNIT>|<UNIT\s[^>]*\/>/g;
  for (const m of xml.matchAll(re)) {
    const open = m[0].match(/<UNIT\s[^>]*>/)[0];
    const model = attr(open, 'model'), texture = attr(open, 'texture');
    if (!model) continue;
    const anims = [...(m[1] ?? '').matchAll(/<ANIM\s[^>]*>/g)].map(a => ({
      action: attr(a[0], 'name'), file: animFiles.get(attr(a[0], 'file')) ?? null,
    }));
    units.push({ name: attr(open, 'name'), model: norm(model), texture: texture && norm(texture), scale: Number(attr(open, 'scale') ?? 1), anims });
  }
  return units;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const gameDir = process.argv[2];
  const units = loadCatalog(gameDir);
  const actual = new Map(fs.readdirSync(path.join(gameDir, 'art')).map(f => [`art/${f}`.toLowerCase(), `art/${f}`]));
  const want = new Set();
  for (const u of units) {
    want.add(u.model);
    if (u.texture) want.add(u.texture);
    for (const a of u.anims) if (a.file) want.add(a.file);
  }
  const found = [], missing = [];
  for (const f of want) (actual.has(f.toLowerCase()) ? found : missing).push(actual.get(f.toLowerCase()) ?? f);
  if (process.argv.includes('--files')) {
    console.log(['Data/unit_graphics.xml', 'Data/anim_graphics.xml', ...found.sort()].join('\n'));
  } else {
    const bytes = found.reduce((s, f) => s + fs.statSync(path.join(gameDir, f)).size, 0);
    const tga = {};
    for (const f of found.filter(f => /\.tga$/i.test(f))) {
      const b = fs.readFileSync(path.join(gameDir, f));
      const k = `type${b[2]}/${b[16]}bpp${b[1] ? '/colormap' : ''}`;
      tga[k] = (tga[k] ?? 0) + 1;
    }
    const exts = {};
    for (const f of found) exts[path.extname(f).toLowerCase()] = (exts[path.extname(f).toLowerCase()] ?? 0) + 1;
    console.log({ units: units.length, files: found.length, missing: missing.length, MB: +(bytes / 1048576).toFixed(1), exts, tga });
    if (missing.length) console.log('missing sample:', missing.slice(0, 8));
  }
}
