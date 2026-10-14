// Plot a scenario's objects over the top-down terrain preview, to check the object decode (docs/scenario-objects-spec.md):
// positions, owners, facing, footprints. The base image comes from re/tools/terrain-preview.mjs (run as a child process).
//   node re/tools/objects-preview.mjs <file.scx> [--px 24] [--out re/previews/<name>_objects.png] [--install ~/Games/RiseOfNations]
// Buildings and cities: footprint (X_SIZE x Y_SIZE TCoords around the stored centre) outlined in the owner's team colour,
// cities with a bigger centre dot. Units: dot in the team colour plus a white facing tick, direction (sin a, -cos a).
// Goods: white diamonds. Items: yellow crosses. Hidden units (garrisoned / transported / crew slots) are skipped.
// Prints placement checks: land units on water, ships on land, building footprint cells on water.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { parseScenario, decompress, png, parseObjects, loadCatalog } from './scxdump.mjs';

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf('--' + k); return i >= 0 ? args[i + 1] : d; };
const install = opt('install', path.join(os.homedir(), 'Games/RiseOfNations'));
const PX = +opt('px', 24);
const file = args[0];
const out = opt('out', `re/previews/${path.basename(file).replace(/\.\w+$/, '')}_objects.png`);

// --- base image: terrain-preview.mjs, decoded back from its PNG (8-bit RGB/RGBA, non-interlaced)
const here = path.dirname(fileURLToPath(import.meta.url));
const basePng = out.replace(/\.png$/, '') + '_base.png';
execFileSync(process.execPath, [path.join(here, 'terrain-preview.mjs'), file, '--px', String(PX), '--out', basePng, '--install', install], { stdio: 'inherit' });
function readPng(buf) {
  let p = 8, w = 0, h = 0, ct = 0; const idat = [];
  while (p < buf.length) {
    const len = buf.readUInt32BE(p), type = buf.toString('latin1', p + 4, p + 8), d = buf.subarray(p + 8, p + 8 + len);
    if (type === 'IHDR') { w = d.readUInt32BE(0); h = d.readUInt32BE(4); ct = d[9]; if (d[8] !== 8 || d[12]) throw new Error('unsupported PNG'); }
    if (type === 'IDAT') idat.push(d);
    p += 12 + len;
  }
  const bpp = ct === 6 ? 4 : 3, raw = zlib.inflateSync(Buffer.concat(idat)), stride = w * bpp, px = new Uint8Array(w * h * 3), cur = new Uint8Array(stride);
  let prev = new Uint8Array(stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], row = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
      const pred = [0, a, b, (a + b) >> 1, pa <= pb && pa <= pc ? a : pb <= pc ? b : c][f];
      cur[i] = (row[i] + pred) & 255;
    }
    for (let x = 0; x < w; x++) for (let k = 0; k < 3; k++) px[(y * w + x) * 3 + k] = cur[x * bpp + k];
    prev = Uint8Array.from(cur);
  }
  return { w, h, px };
}
const base = readPng(fs.readFileSync(basePng));
fs.unlinkSync(basePng);
const W = base.w, H = base.h, img = base.px;

// --- objects
const sbuf = decompress(fs.readFileSync(file)), scn = parseScenario(sbuf), t = scn.terrain;
const o = parseObjects(sbuf, scn.top.slice(1)), cat = loadCatalog(install);
const S = W / (t.xs * 768); // pixels per world unit
// Team colour slot -> RGB: TEAM_COLOR_BUILD defaults (buildings-spec §4), brightened for the preview.
const TEAM = [[102, 0, 0], [26, 57, 172], [128, 0, 128], [0, 89, 0], [98, 96, 0], [0, 92, 108], [120, 120, 120], [112, 48, 0], [255, 255, 255]]
  .map(c => c.map(v => Math.min(255, v * 2 + 40)));
const colorOf = who => TEAM[o.leaders.find(L => L.who === who)?.team_color ?? who] ?? [255, 0, 255];
const put = (x, y, c) => { x = Math.round(x); y = Math.round(y); if (x < 0 || y < 0 || x >= W || y >= H) return; img.set(c, (y * W + x) * 3); };
const line = (x0, y0, x1, y1, c) => { const n = Math.ceil(Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0))) + 1; for (let i = 0; i <= n; i++) put(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, c); };
const rect = (x0, y0, x1, y1, c) => { line(x0, y0, x1, y0, c); line(x1, y0, x1, y1, c); line(x1, y1, x0, y1, c); line(x0, y1, x0, y0, c); };
const disc = (x, y, r, c) => { for (let dy = -r; dy <= r; dy++) for (let dx = -r; dx <= r; dx++) if (dx * dx + dy * dy <= r * r) put(x + dx, y + dy, c); };
// TCoord water bit (file mask 0x10, terrain-spec §A)
const isWater = (x, y) => { const tx = Math.floor(x / 192), ty = Math.floor(y / 192); if (tx < 0 || ty < 0 || tx >= t.txs || ty >= t.tys) return null; return (t.tmask[ty * t.txs + tx] & 0x10) !== 0; };

const stats = { buildCells: 0, buildCellsOnWater: 0, landUnits: 0, landUnitsOnWater: 0, seaUnits: 0, seaUnitsOnLand: 0, hidden: 0 };
const odd = [];
for (const b of [...o.builds, ...o.cities]) {
  const e = cat.get(b.type), xs = e?.xs || 2, ys = e?.ys || 2, c = colorOf(b.who);
  const x0 = b.cx - xs * 96, y0 = b.cy - ys * 96, x1 = b.cx + xs * 96, y1 = b.cy + ys * 96;
  let wet = 0, n = 0;
  for (let y = y0 + 96; y < y1; y += 192) for (let x = x0 + 96; x < x1; x += 192) { n++; if (isWater(x, y)) wet++; }
  stats.buildCells += n; stats.buildCellsOnWater += wet;
  if (wet * 2 > n && !/Dock|Shipyard|Anchorage|Fishing|Lighthouse|Colossus|Oil Platform/.test(b.type)) odd.push(`${b.type}@${b.cx},${b.cy} ${wet}/${n} wet`);
  rect(x0 * S, y0 * S, x1 * S - 1, y1 * S - 1, c); rect(x0 * S + 1, y0 * S + 1, x1 * S - 2, y1 * S - 2, c);
  disc(b.cx * S, b.cy * S, o.cities.includes(b) ? 3 : 1, c);
}
for (const u of o.units) {
  if (u.inside >= 0 || (u.unit_masks & 8)) { stats.hidden++; continue; }
  const e = cat.get(u.type), c = colorOf(u.who), w = isWater(u.cx, u.cy);
  if (e?.domain === 'Sea') { stats.seaUnits++; if (w === false) { stats.seaUnitsOnLand++; odd.push(`${u.type}@${u.cx},${u.cy} on land`); } }
  else if (e?.domain === 'Land') { stats.landUnits++; if (w) { stats.landUnitsOnWater++; odd.push(`${u.type}@${u.cx},${u.cy} on water`); } }
  const a = (u.angle >>> 0) / 2 ** 32 * 2 * Math.PI, x = u.cx * S, y = u.cy * S;
  disc(x, y, Math.max(1, Math.round(PX / 10)), c);
  line(x, y, x + Math.sin(a) * PX / 3, y - Math.cos(a) * PX / 3, [255, 255, 255]); // facing = (sin a, -cos a)
}
for (const g of o.goods) { const x = g.cx * S, y = g.cy * S, r = PX / 5, c = [255, 255, 255]; line(x - r, y, x, y - r, c); line(x, y - r, x + r, y, c); line(x + r, y, x, y + r, c); line(x, y + r, x - r, y, c); }
for (const g of o.items) { const x = g.cx * S, y = g.cy * S, r = PX / 5, c = [255, 230, 0]; line(x - r, y - r, x + r, y + r, c); line(x - r, y + r, x + r, y - r, c); }

fs.writeFileSync(out, png(W, H, img));
console.log(`objects: ${o.units.length} units, ${o.builds.length} buildings, ${o.cities.length} cities, ${o.goods.length} goods, ${o.items.length} items; players ${o.leaders.map(L => `${L.who}:colour${L.team_color}`).join(' ')}`);
console.log(`checks: ${JSON.stringify(stats)}`);
if (odd.length) console.log(`  suspicious: ${odd.slice(0, 12).join('; ')}${odd.length > 12 ? ` (+${odd.length - 12})` : ''}`);
console.log(`wrote ${out}`);
