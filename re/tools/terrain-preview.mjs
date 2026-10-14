// Top-down software preview of a scenario's terrain, to check the texture-selection rules in docs/terrain-spec.md
// before writing Metal code. Base pass, plus the transition pass with --transitions (no mountains overlay, no trees).
//   node re/tools/terrain-preview.mjs <file.scx> [--px 24] [--transitions] [--alpha] [--seed n] [--crop x,y,w,h] [--out re/previews/name.png]
//                                     [--install ~/Games/RiseOfNations]
// Per wcoord: texture = tileset BASELAND[land_sub] (land 0) / COASTAL[0] (land 1) / OCEAN[0] (land 2); supertile textures
// use subtile (wx%3, wy%3) of the 512² image. Shading = baked Lambert with the game light (travel dir (-1,0,-1)/√2).
// Water: sea level z=0, colour between WaterTop/WaterBot, alpha 245 at z <= -55 fading to 0 at z = 0 (assumed).
// --transitions: one overlay patch per listed wcoord, textured with the next-lower texture (same subtile rule) and
// alpha-masked by terrain art/transitions/<code>.tga (re/tools/transitions.mjs); patches whose overlay is the coastal
// texture (ocean next to shelf) get the second "CoralTrans" layer (tileset CORALTEX, same mask alpha).
// --alpha: write the mask alpha of every patch as grey instead (white = lower texture), to check edge continuity.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { parseScenario, decompress, png } from './scxdump.mjs';
import { parseTGA } from './ronassets.mjs';
import { buildTransitions, listMasks, maskUV, resolveCI } from './transitions.mjs';

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf('--' + k); return i >= 0 ? args[i + 1] : d; };
const install = opt('install', path.join(os.homedir(), 'Games/RiseOfNations'));
const PX = +opt('px', 24);
const TRANS = args.includes('--transitions') || args.includes('--alpha'), ALPHA = args.includes('--alpha');
const file = args[0];
const sc = parseScenario(decompress(fs.readFileSync(file)));
const t = sc.terrain;

// --- tileset lookup (tiny regex XML reader; enough for tilesets.xml)
const xml = fs.readFileSync(path.join(install, 'Data/tilesets.xml'), 'latin1');
const blocks = [...xml.matchAll(/<TILESET name="([^"]+)"[\s\S]*?<\/TILESET>/g)];
const ts = blocks.find(m => m[1] === t.tileset)?.[0];
if (!ts) throw new Error(`tileset ${t.tileset} not in tilesets.xml`);
const baseDir = ts.match(/<BASEDIR path="([^"]+)"/)[1];
const section = name => (ts.match(new RegExp(`<${name}>([\\s\\S]*?)</${name}>`))?.[1] ?? '');
const bases = name => [...section(name).matchAll(/<BASE name="[^"]*"(?: supertile="(\d)")?>\s*<TEXTURE name="([^"]+)"/g)].map(m => ({ supertile: m[1] === '1', file: m[2] }));
const land = [bases('BASELAND'), bases('COASTAL'), bases('OCEAN')];
const coastBase = +(ts.match(/<COAST_BASE value="(\d+)"/)?.[1] ?? 0);
const coralFile = section('CORALTEX').match(/file="([^"]+)"/)?.[1];

const resolve = rel => resolveCI(install, rel);
const texCache = new Map();
function tex(f) {
  if (!texCache.has(f)) {
    const p = resolve(path.posix.join(baseDir, f)) ?? resolve(path.posix.join('terrain art', f)) ?? resolve(f);
    if (!p) throw new Error(`texture not found: ${f}`);
    texCache.set(f, parseTGA(fs.readFileSync(p)));
  }
  return texCache.get(f);
}

// Texel of land texture (type, sub) at wcoord (wx, wy), local (u, v) in [0,1): supertile subtile or whole image.
function landTexel(type, sub, wx, wy, u, v) {
  const list = land[type], b = list[Math.min(sub, list.length - 1)];
  const im = tex(b.file);
  const n = b.supertile ? Math.floor(im.width / 3) : im.width;
  const ox = b.supertile ? (wx % 3) * n : 0, oy = b.supertile ? (wy % 3) * n : 0;
  const s = ((oy + Math.floor(v * n)) * im.width + ox + Math.floor(u * n)) * 4;
  return [im.rgba[s], im.rgba[s + 1], im.rgba[s + 2]];
}
// Bilinear alpha with clamp addressing (the mask stage is CLAMP; mip bias -1.5 keeps it at the top level here).
function alphaAt(im, u, v) {
  const fx = Math.min(Math.max(u * im.width - 0.5, 0), im.width - 1), fy = Math.min(Math.max(v * im.height - 0.5, 0), im.height - 1);
  const x0 = Math.floor(fx), y0 = Math.floor(fy), x1 = Math.min(x0 + 1, im.width - 1), y1 = Math.min(y0 + 1, im.height - 1);
  const a = (x, y) => im.rgba[(y * im.width + x) * 4 + 3];
  const tx = fx - x0, ty = fy - y0;
  return ((a(x0, y0) * (1 - tx) + a(x1, y0) * tx) * (1 - ty) + (a(x0, y1) * (1 - tx) + a(x1, y1) * tx) * ty) / 255;
}

// --- transitions
const patchAt = new Map();
if (TRANS) {
  const tr = buildTransitions({
    xs: t.xs, ys: t.ys, land: t.land.map(l => Math.min(l, 2)), landSub: t.landSub,
    numTiles: land.map(l => l.length), coastBase, seed: +(opt('seed', sc.header.map_seed ?? 0)), masks: listMasks(install),
  });
  const maskImg = new Map();
  for (const p of tr.patches) {
    if (p.missing) continue;
    if (!maskImg.has(p.mask.file)) maskImg.set(p.mask.file, parseTGA(fs.readFileSync(p.mask.file)));
    patchAt.set(p.y * t.xs + p.x, { ...p, img: maskImg.get(p.mask.file) });
  }
  console.log(`transitions: ${tr.patches.length} listed wcoords, ${patchAt.size} patches`);
}
const coral = TRANS && coralFile ? parseTGA(fs.readFileSync(resolve(coralFile))) : null;

// --- shading
const VX = t.vxs, H = t.heights;
const hAt = (tx, ty) => H[Math.min(ty, t.vys - 1) * VX + Math.min(tx, VX - 1)];
const L = [1 / Math.SQRT2, 0, 1 / Math.SQRT2]; // towards the light
function shade(tx, ty) {
  const dx = hAt(tx + 1, ty) - hAt(Math.max(tx - 1, 0), ty), dy = hAt(tx, ty + 1) - hAt(tx, Math.max(ty - 1, 0));
  const n = [-dx, -dy, 2 * 192], len = Math.hypot(...n);
  return Math.max(0, (n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / len);
}

const [CX, CY, CW, CH] = (opt('crop', '') || `0,0,${t.xs},${t.ys}`).split(',').map(Number); // wcoords
const W = CW * PX, Hh = CH * PX, img = new Uint8Array(W * Hh * 3);
const WATER = [0, 135, 236];
for (let wy = CY; wy < CY + CH; wy++) for (let wx = CX; wx < CX + CW; wx++) {
  const i = wy * t.xs + wx, type = Math.min(t.land[i], 2), sub = type === 0 ? t.landSub[i] : 0;
  const p = patchAt.get(i);
  for (let py = 0; py < PX; py++) for (let px = 0; px < PX; px++) {
    const u = (px + 0.5) / PX, v = (py + 0.5) / PX;
    let c = landTexel(type, sub, wx, wy, u, v);
    let a = 0;
    if (p) {
      const [mu, mv] = maskUV(u, v, p.orient, p.quad);
      a = alphaAt(p.img, mu, mv);
      // overlay: tex0 × diffuse, alpha = mask × vertex alpha, SRCALPHA / INVSRCALPHA over the base pass
      const o = landTexel(p.overLand, p.overSub, wx, wy, u, v);
      c = c.map((x, k) => x * (1 - a) + o[k] * a);
      if (coral && p.overLand === 1 && p.overSub === 0) { // oceanindex bucket: CoralSandy, then CoralTrans
        const s = (Math.min(coral.height - 1, Math.floor(v * coral.height)) * coral.width + Math.min(coral.width - 1, Math.floor(u * coral.width))) * 4;
        c = c.map((x, k) => x * (1 - a) + coral.rgba[s + k] * a);
      }
    }
    const o = (((wy - CY) * PX + py) * W + (wx - CX) * PX + px) * 3;
    if (ALPHA) { const g = p ? a * 255 : 0; img[o] = g; img[o + 1] = p ? g : 40; img[o + 2] = g; continue; }
    // world position of this pixel in tcoord units
    const tx = wx * 4 + u * 4, ty = wy * 4 + v * 4;
    const z = hAt(Math.round(tx), Math.round(ty));
    const lit = Math.min(1, 0.45 + 0.75 * shade(Math.round(tx), Math.round(ty)));
    c = c.map(x => x * lit);
    const wa = z <= -55 ? 245 / 255 : z >= 0 ? 0 : (245 / 255) * (-z / 55);
    c = c.map((x, k) => x * (1 - wa) + WATER[k] * wa);
    img[o] = c[0]; img[o + 1] = c[1]; img[o + 2] = c[2];
  }
}
const suffix = ALPHA ? '_transalpha' : TRANS ? '_trans' : '_terrain';
const out = opt('out', `re/previews/${path.basename(file).replace(/\.\w+$/, '')}${suffix}.png`);
fs.writeFileSync(out, png(W, Hh, img));
console.log(`wrote ${out} ${W}x${Hh}; tileset ${t.tileset}: baseland ${land[0].map(b => b.file).join(',')} coastal ${land[1][0]?.file} ocean ${land[2][0]?.file}`);
