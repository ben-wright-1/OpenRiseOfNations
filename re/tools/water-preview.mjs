// Top-down software preview of the sea water, the ocean floor and the underwater terrain fade, following
// docs/water-spec.md, side by side with the simplified sheet the engine currently draws.
//   node re/tools/water-preview.mjs <file.scx> [--px 32] [--crop x,y,w,h] [--seed n] [--hist] [--install ~/Games/RiseOfNations]
// Writes re/previews/<name>_water_{alpha,alpha_ours,orig,ours}.png and prints a coastline jaggedness metric.
//  alpha       water alpha of the original mesh (TerrainOut::create_water_verts + generate_water smoothing +
//              modify_water 13-vertex patches), grey = alpha, dark red = no water vertex
//  alpha_ours  alpha of the engine's current sheet (384 grid, 245*clamp(-h/55), two triangles per cell)
//  orig        composite: land (vertex alpha fades out between z -52 and -89) over black, the ocean-floor sheet,
//              transitions incl. the coral layer, then the water (vertex colour, alpha)
//  ours        composite: opaque land + transitions, then the engine's current water sheet
// Lighting is the preview's plain Lambert (no HDR); the screen gradient uses t = (x + y) / (W + H) as a stand-in
// for the screen-space position (top of the screen = top-left corner of the map).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { parseScenario, decompress, png } from './scxdump.mjs';
import { parseTGA } from './ronassets.mjs';
import { buildTransitions, listMasks, maskUV, resolveCI } from './transitions.mjs';

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf('--' + k); return i >= 0 ? args[i + 1] : d; };
const install = opt('install', path.join(os.homedir(), 'Games/RiseOfNations'));
const PX = +opt('px', 32); // pixels per wcoord
const file = args[0];
const sc = parseScenario(decompress(fs.readFileSync(file)));
const t = sc.terrain;
const XS = t.xs, YS = t.ys, VX = t.vxs, VY = t.vys, H = t.heights;
const hAt = (tx, ty) => H[Math.max(0, Math.min(ty, VY - 1)) * VX + Math.max(0, Math.min(tx, VX - 1))];

// ---------------------------------------------------------------- graphics.txt constants (NEW_TERRAIN_*)
const gfx = fs.readFileSync(path.join(install, 'graphics.txt'), 'latin1').replace(/\r/g, '');
const gnum = (key, d) => { const m = gfx.match(new RegExp('^' + key.replace(/ /g, '\\s+') + '\\s*,\\s*([-0-9.]+)', 'mi')); return m ? +m[1] : d; };
const SOLID = gnum('WaterColor Solid', 245), INVIS = gnum('WaterColor Invis', 0);
const TOP = [gnum('WaterRedTop', 0), gnum('WaterGreenTop', 159), gnum('WaterBlueTop', 255)];
const BOT = [gnum('WaterRedBot', 0), gnum('WaterGreenBot', 111), gnum('WaterBlueBot', 218)];
const FLOOR_TOP = [gnum('OceanFloorRedTop', 255), gnum('OceanFloorGreenTop', 255), gnum('OceanFloorBlueTop', 255)];
const FLOOR_BOT = [gnum('OceanFloorRedBot', 64), gnum('OceanFloorGreenBot', 64), gnum('OceanFloorBlueBot', 64)];
const FLOOR_SCALE = 1 / (1 << gnum('OceanFloorScale', 3));
const COAST_DEPTH = gnum('coast_depth', -101), LAND_HEIGHT = gnum('land_height', 10);
const MIN_CLIP = -25;          // TerrainData::min_clip_level ("OceanWaterMaxHeight"), TerrainOut ctor
const TRANS_BRIGHT = 99;       // OceanVars::trans_bright, Terrain::init
const FLOOR_Z = COAST_DEPTH + LAND_HEIGHT - 10;              // build_ocean_floor: -101
const FADE_D = Math.abs(COAST_DEPTH + LAND_HEIGHT + 2 + 52); // light_land_vert: 37 -> alpha 0 at z = -89

// ---------------------------------------------------------------- per-wcoord / per-tcoord flags
const wf = i => t.wflags[i], cf = i => (t.wcoordFlags ? t.wcoordFlags[i] : 0);
const isRiverT = (tx, ty) => tx >= 0 && ty >= 0 && tx < t.txs && ty < t.tys && (t.tmask[ty * t.txs + tx] & 0x02) !== 0;
// TerrainOut::coord_contains_water
function contains(wx, wy) {
  if (wx < 0 || wy < 0 || wx >= XS || wy >= YS) return false;
  const i = wy * XS + wx;
  if ((wf(i) & 0x08) || t.land[i] === 2 || (wf(i) & 0x80)) return true; // coast, deep ocean, half water
  const f = cf(i);
  if (f & 9) return true;                                                   // WATER | OCEAN
  if (f & 4) {                                                              // COASTAL: any of the 5x5 heights below 0
    for (let j = 0; j <= 4; j++) for (let k = 0; k <= 4; k++) if (hAt(wx * 4 + k, wy * 4 + j) < 0) return true;
  }
  return (f & 0x10) !== 0;                                                  // RIVER
}
// TerrainOut::get_water_fade (tx, ty are height-vertex indices)
function fade(tx, ty, a) {
  const z = hAt(tx, ty);
  if (z > -55) a = z <= MIN_CLIP ? Math.trunc(((z - MIN_CLIP) / -55) * a) & 0xff : 0;
  if (a !== 0 && isRiverT(tx, ty)) a >>= 2;
  return a;
}

// ---------------------------------------------------------------- original water mesh
const W2 = 2 * XS + 1, H2 = 2 * YS + 1;
const vmap = new Int32Array(W2 * H2).fill(-1);
const verts = []; // {x, y, a (0..255 raw, later smoothed)}
const rows = new Map(); // wcoord index -> [row0, row1, row2] first-vertex indices
const setRow = (wx, wy, r, idx) => { const k = wy * XS + wx; if (!rows.has(k)) rows.set(k, [-1, -1, -1]); rows.get(k)[r] = idx; };
for (let py = 0; py < H2; py++) for (let px = 0; px < W2; px += 2) {
  if ((py & 1) === 0) {
    const wx = px >> 1, wy = py >> 1;
    const left = contains(wx - 1, wy - 1) || contains(wx - 1, wy);
    const n0 = verts.length - (left ? 1 : 0);
    let any = false;
    if (contains(wx, wy - 1)) { any = true; setRow(wx, wy - 1, 2, n0); }
    if (contains(wx, wy)) { any = true; setRow(wx, wy, 0, n0); }
    const a0 = fade(4 * wx, 4 * wy, SOLID);
    let a1 = SOLID, a2 = SOLID;
    if (wx < XS) { a1 = fade(4 * wx + 2, 4 * wy, SOLID); a2 = fade(4 * wx + 4, 4 * wy, SOLID); }
    if (!any) continue;
    const X = wx * 768, Y = wy * 768;
    if (!left) { verts.push({ x: X, y: Y, a: a0 }); vmap[py * W2 + px] = verts.length - 1; }
    if (wx >= XS) continue;
    verts.push({ x: X + 384, y: Y, a: a1 }); vmap[py * W2 + px + 1] = verts.length - 1;
    verts.push({ x: X + 768, y: Y, a: a2 }); vmap[py * W2 + px + 2] = verts.length - 1;
  } else {
    const wx = px >> 1, wy = (py - 1) >> 1;
    if (!contains(wx, wy)) continue;
    const left = contains(wx - 1, wy);
    setRow(wx, wy, 1, verts.length - (left ? 1 : 0));
    // Quirk: the middle row samples the heights of the wcoord's top edge (ty = 4*wy), not its centre row.
    const a0 = fade(4 * wx, 4 * wy, SOLID), a1 = fade(4 * wx + 2, 4 * wy, SOLID), a2 = fade(4 * wx + 4, 4 * wy, SOLID);
    const X = wx * 768, Y = wy * 768 + 384;
    if (!left) { verts.push({ x: X, y: Y, a: a0 }); vmap[py * W2 + px] = verts.length - 1; }
    verts.push({ x: X + 384, y: Y, a: a1 }); vmap[py * W2 + px + 1] = verts.length - 1;
    verts.push({ x: X + 768, y: Y, a: a2 }); vmap[py * W2 + px + 2] = verts.length - 1;
  }
}
// generate_water smoothing: in place, raster order, a = min(own, mean of existing 3x3 neighbours), stored squared.
{
  const n = W2 * W2; // the game bounds-checks against (2*xs+1)^2 and lets k-1 / k+1 wrap across rows
  for (let k = 0; k < W2 * H2; k++) {
    const v = vmap[k];
    if (v < 0) continue;
    const own = verts[v].a / 255;
    let sum = own, cnt = 1;
    for (const d of [-1, 1, -W2 - 1, -W2, -W2 + 1, W2 - 1, W2, W2 + 1]) {
      const q = k + d;
      if (q < 0 || q >= n || vmap[q] < 0) continue;
      sum += verts[vmap[q]].a / 255; cnt++;
    }
    const m = Math.min(own, sum / cnt);
    verts[v].a = Math.trunc(m * m * 255) & 0xff;
  }
}
// modify_water: per water wcoord a 3x3 grid + 4 quad centres (average position/alpha of the quad), 16 triangles.
const TEMPLATE = [0, 9, 3, 0, 1, 9, 1, 4, 9, 4, 3, 9, 1, 10, 4, 1, 2, 10, 2, 5, 10, 5, 4, 10,
  3, 11, 6, 3, 4, 11, 4, 7, 11, 7, 6, 11, 4, 5, 12, 5, 8, 12, 8, 7, 12, 7, 4, 12];
const origTris = [];
for (const [, r] of rows) {
  if (r.some(x => x < 0)) continue;
  const g = []; for (const s of r) for (let c = 0; c < 3; c++) g.push(verts[s + c]);
  for (const [a, b, c, d] of [[0, 1, 3, 4], [1, 2, 4, 5], [3, 4, 6, 7], [4, 5, 7, 8]]) {
    g.push({ x: (g[a].x + g[d].x) / 2, y: (g[a].y + g[d].y) / 2, a: Math.trunc((g[a].a + g[b].a + g[c].a + g[d].a) / 4) });
  }
  for (let i = 0; i < TEMPLATE.length; i += 3) origTris.push([g[TEMPLATE[i]], g[TEMPLATE[i + 1]], g[TEMPLATE[i + 2]]]);
}
// The engine's current sheet (engine/render/terrain_mesh.cpp): 384 grid over the whole map, two triangles per cell.
const oursTris = [];
{
  const nx = XS * 2 + 1, ny = YS * 2 + 1, g = [];
  for (let j = 0; j < ny; j++) for (let i = 0; i < nx; i++) {
    const z = hAt(i * 2, j * 2);
    g.push({ x: i * 384, y: j * 384, a: Math.round(245 * Math.min(1, Math.max(0, -z / 55))) });
  }
  for (let j = 0; j + 1 < ny; j++) for (let i = 0; i + 1 < nx; i++) {
    const a = g[j * nx + i], b = g[j * nx + i + 1], c = g[(j + 1) * nx + i], d = g[(j + 1) * nx + i + 1];
    if (!a.a && !b.a && !c.a && !d.a) continue;
    oursTris.push([a, b, c], [c, b, d]);
  }
}

// ---------------------------------------------------------------- rasteriser (Gouraud alpha, top-down)
const [CX, CY, CW, CH] = (opt('crop', '') || `0,0,${XS},${YS}`).split(',').map(Number);
const W = CW * PX, Hh = CH * PX, S = PX / 768; // pixels per world unit
function rasterAlpha(tris) {
  const buf = new Float32Array(W * Hh).fill(-1); // -1 = no water geometry
  for (const [p0, p1, p2] of tris) {
    const xs = [p0, p1, p2].map(p => (p.x - CX * 768) * S), ys = [p0, p1, p2].map(p => (p.y - CY * 768) * S);
    const x0 = Math.max(0, Math.floor(Math.min(...xs))), x1 = Math.min(W - 1, Math.ceil(Math.max(...xs)));
    const y0 = Math.max(0, Math.floor(Math.min(...ys))), y1 = Math.min(Hh - 1, Math.ceil(Math.max(...ys)));
    const den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2]);
    if (!den) continue;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const px = x + 0.5, py = y + 0.5;
      const l0 = ((ys[1] - ys[2]) * (px - xs[2]) + (xs[2] - xs[1]) * (py - ys[2])) / den;
      const l1 = ((ys[2] - ys[0]) * (px - xs[2]) + (xs[0] - xs[2]) * (py - ys[2])) / den;
      const l2 = 1 - l0 - l1;
      if (l0 < -1e-6 || l1 < -1e-6 || l2 < -1e-6) continue;
      buf[y * W + x] = (l0 * p0.a + l1 * p1.a + l2 * p2.a) / 255;
    }
  }
  return buf;
}
const aOrig = rasterAlpha(origTris), aOurs = rasterAlpha(oursTris);

// Jaggedness: length of the alpha = 0.2 iso-line (marching squares) divided by the length of the same iso-line of a
// box-blurred copy (radius = a quarter wcoord). A sawtooth edge is longer than its smoothed version.
function isoLength(buf, thr) {
  let len = 0;
  const v = (x, y) => Math.max(0, buf[y * W + x]);
  for (let y = 0; y + 1 < Hh; y++) for (let x = 0; x + 1 < W; x++) {
    const c = [v(x, y), v(x + 1, y), v(x + 1, y + 1), v(x, y + 1)];
    const pts = [];
    for (let e = 0; e < 4; e++) {
      const a = c[e], b = c[(e + 1) % 4];
      if ((a < thr) !== (b < thr)) {
        const f = (thr - a) / (b - a);
        const P = [[x, y], [x + 1, y], [x + 1, y + 1], [x, y + 1]];
        const A = P[e], B = P[(e + 1) % 4];
        pts.push([A[0] + (B[0] - A[0]) * f, A[1] + (B[1] - A[1]) * f]);
      }
    }
    for (let i = 0; i + 1 < pts.length; i += 2) len += Math.hypot(pts[i][0] - pts[i + 1][0], pts[i][1] - pts[i + 1][1]);
  }
  return len;
}
function blur(buf, r) {
  const tmp = new Float32Array(W * Hh), out = new Float32Array(W * Hh);
  for (let y = 0; y < Hh; y++) for (let x = 0; x < W; x++) {
    let s = 0, n = 0; for (let k = -r; k <= r; k++) { const q = x + k; if (q >= 0 && q < W) { s += Math.max(0, buf[y * W + q]); n++; } }
    tmp[y * W + x] = s / n;
  }
  for (let y = 0; y < Hh; y++) for (let x = 0; x < W; x++) {
    let s = 0, n = 0; for (let k = -r; k <= r; k++) { const q = y + k; if (q >= 0 && q < Hh) { s += tmp[q * W + x]; n++; } }
    out[y * W + x] = s / n;
  }
  return out;
}
const jag = buf => isoLength(buf, 0.2) / Math.max(1, isoLength(blur(buf, Math.max(1, PX >> 2)), 0.2));

// ---------------------------------------------------------------- textures (same lookup as terrain-preview.mjs)
const xml = fs.readFileSync(path.join(install, 'Data/tilesets.xml'), 'latin1');
const ts = [...xml.matchAll(/<TILESET name="([^"]+)"[\s\S]*?<\/TILESET>/g)].find(m => m[1] === t.tileset)?.[0];
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
function landTexel(type, sub, wx, wy, u, v) {
  const list = land[type], b = list[Math.min(sub, list.length - 1)];
  const im = tex(b.file);
  const n = b.supertile ? Math.floor(im.width / 3) : im.width;
  const ox = b.supertile ? (wx % 3) * n : 0, oy = b.supertile ? (wy % 3) * n : 0;
  const s = ((oy + Math.floor(v * n)) * im.width + ox + Math.floor(u * n)) * 4;
  return [im.rgba[s], im.rgba[s + 1], im.rgba[s + 2]];
}
const wrapTexel = (im, u, v) => {
  const x = ((Math.floor(u * im.width) % im.width) + im.width) % im.width, y = ((Math.floor(v * im.height) % im.height) + im.height) % im.height;
  const s = (y * im.width + x) * 4; return [im.rgba[s], im.rgba[s + 1], im.rgba[s + 2]];
};
function alphaAt(im, u, v) {
  const fx = Math.min(Math.max(u * im.width - 0.5, 0), im.width - 1), fy = Math.min(Math.max(v * im.height - 0.5, 0), im.height - 1);
  const x0 = Math.floor(fx), y0 = Math.floor(fy), x1 = Math.min(x0 + 1, im.width - 1), y1 = Math.min(y0 + 1, im.height - 1);
  const a = (x, y) => im.rgba[(y * im.width + x) * 4 + 3];
  const tx = fx - x0, ty = fy - y0;
  return ((a(x0, y0) * (1 - tx) + a(x1, y0) * tx) * (1 - ty) + (a(x0, y1) * (1 - tx) + a(x1, y1) * tx) * ty) / 255;
}
const patchAt = new Map();
{
  const tr = buildTransitions({
    xs: XS, ys: YS, land: t.land.map(l => Math.min(l, 2)), landSub: t.landSub,
    numTiles: land.map(l => l.length), coastBase, seed: +(opt('seed', sc.header.map_seed ?? 0)), masks: listMasks(install),
  });
  const maskImg = new Map();
  for (const p of tr.patches) {
    if (p.missing) continue;
    if (!maskImg.has(p.mask.file)) maskImg.set(p.mask.file, parseTGA(fs.readFileSync(p.mask.file)));
    patchAt.set(p.y * XS + p.x, { ...p, img: maskImg.get(p.mask.file) });
  }
}
const coral = coralFile ? parseTGA(fs.readFileSync(resolve(coralFile))) : null;
const floorTex = tex(land[2][0].file);

// ---------------------------------------------------------------- composites
const L = [1 / Math.SQRT2, 0, 1 / Math.SQRT2];
function shade(tx, ty) {
  const dx = hAt(tx + 1, ty) - hAt(tx - 1, ty), dy = hAt(tx, ty + 1) - hAt(tx, ty - 1);
  const n = [-dx, -dy, 2 * 192], len = Math.hypot(...n);
  return Math.max(0, (n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / len);
}
const landAlphaV = z => z >= 0 ? 1 : 1 - Math.min(Math.max(0, -52 - z), FADE_D) / FADE_D; // light_land_vert
function bilerp(f, tx, ty) {
  const x0 = Math.floor(tx), y0 = Math.floor(ty), fx = tx - x0, fy = ty - y0;
  return (f(x0, y0) * (1 - fx) + f(x0 + 1, y0) * fx) * (1 - fy) + (f(x0, y0 + 1) * (1 - fx) + f(x0 + 1, y0 + 1) * fx) * fy;
}
const floorAt = new Uint8Array((XS + 1) * (YS + 1)); // build_ocean_floor coverage (incl. the top/left ring via wcoord 0)
for (let wy = -1; wy < YS; wy++) for (let wx = -1; wx < XS; wx++) {
  const i = Math.max(0, wy) * XS + Math.max(0, wx);
  if (wx < 0 || wy < 0 || (wf(i) & 0x80) || t.land[i] === 1 || t.land[i] === 2 || (cf(i) & 0x2000)) floorAt[(wy + 1) * (XS + 1) + wx + 1] = 1;
}
const imgO = new Uint8Array(W * Hh * 3), imgU = new Uint8Array(W * Hh * 3), imgA = new Uint8Array(W * Hh * 3), imgB = new Uint8Array(W * Hh * 3);
const lerp3 = (a, b, k) => a.map((x, i) => x + (b[i] - x) * k);
for (let wy = CY; wy < CY + CH; wy++) for (let wx = CX; wx < CX + CW; wx++) {
  const i = wy * XS + wx, type = Math.min(t.land[i], 2), sub = type === 0 ? t.landSub[i] : 0;
  const p = patchAt.get(i);
  for (let py = 0; py < PX; py++) for (let px = 0; px < PX; px++) {
    const u = (px + 0.5) / PX, v = (py + 0.5) / PX;
    const tx = wx * 4 + u * 4, ty = wy * 4 + v * 4;
    const gtx = (wx + u) / XS, gty = (wy + v) / YS, tScr = Math.min(1, Math.max(0, (wx + u + wy + v) / (XS + YS)));
    const z = bilerp(hAt, tx, ty);
    const lit = Math.min(1, 0.45 + 0.75 * shade(Math.round(tx), Math.round(ty)));
    const base = landTexel(type, sub, wx, wy, u, v);
    // overlay patch
    let ma = 0, over = null, isOcean = false;
    if (p) { const [mu, mv] = maskUV(u, v, p.orient, p.quad); ma = alphaAt(p.img, mu, mv); over = landTexel(p.overLand, p.overSub, wx, wy, u, v); isOcean = p.overLand === 1 && p.overSub === 0; }
    const coralC = coral ? wrapTexel(coral, u, v) : [0, 0, 0];

    // ---- ours: opaque land + transitions (+ coral), then the current water sheet
    let c = base.map(x => x * lit);
    if (over) { c = lerp3(c, over.map(x => x * lit), ma); if (isOcean && coral) c = lerp3(c, coralC, ma); }
    const o = (((wy - CY) * PX + py) * W + (wx - CX) * PX + px);
    const aU = Math.max(0, aOurs[o]);
    const wcU = [0, Math.round(159 + (111 - 159) * (wx + u + wy + v) / (XS + YS)), Math.round(255 + (218 - 255) * (wx + u + wy + v) / (XS + YS))];
    c = lerp3(c, wcU, aU);
    imgU.set(c.map(Math.round), o * 3);

    // ---- original
    // 1. land over black with the underwater vertex alpha; underwater vertices (alpha < 221) take the floor tint + 99
    const la = bilerp((x, y) => landAlphaV(hAt(x, y)), tx, ty);
    const tint = lerp3(FLOOR_TOP, FLOOR_BOT, tScr).map(x => 0.6 * x);
    const under = la * 255 < 221; // modify_ocean_floor recolours land vertices whose alpha byte is below 0xdd
    const landLight = under ? tint.map(x => Math.min(255, Math.trunc(x) + TRANS_BRIGHT) / 255) : [lit, lit, lit];
    let o2 = base.map((x, k) => x * landLight[k] * la);
    // 2. ocean floor sheet (opaque, depth-tested, no depth write) wherever it exists and the land is deeper than it
    const fwx = Math.floor((wx + u)), fwy = Math.floor(wy + v);
    if (floorAt[(fwy + 1) * (XS + 1) + fwx + 1] && z < FLOOR_Z) {
      const gx = wx + u, gy = wy + v;
      const t0 = wrapTexel(floorTex, (gx + 10) * FLOOR_SCALE, (gy + 10) * FLOOR_SCALE);
      const t1 = wrapTexel(floorTex, (XS - gx + 10) * FLOOR_SCALE, (YS - gy + 10) * FLOOR_SCALE);
      o2 = t0.map((x, k) => Math.min(255, x * (t1[k] / 255) * 2 * (tint[k] / 255) * lit));
    }
    // 3. transitions: land overlays alpha = mask x vertex alpha; ocean (sand) overlays ignore depth and vertex alpha,
    //    then the coral layer with the same mask
    if (over) {
      if (isOcean) { o2 = lerp3(o2, over.map(x => x * lit), ma); if (coral) o2 = lerp3(o2, coralC.map(x => x * lit), ma); }
      else o2 = lerp3(o2, over.map(x => x * lit), ma * la);
    }
    // 4. water: vertex colour (screen gradient), interpolated alpha
    const aO = Math.max(0, aOrig[o]);
    o2 = lerp3(o2, lerp3(TOP, BOT, tScr), aO);
    imgO.set(o2.map(Math.round), o * 3);
    // alpha views
    const ga = aOrig[o] < 0 ? [70, 0, 0] : [aO * 255, aO * 255, aO * 255];
    const gb = aOurs[o] < 0 ? [70, 0, 0] : [aU * 255, aU * 255, aU * 255];
    imgA.set(ga.map(Math.round), o * 3); imgB.set(gb.map(Math.round), o * 3);
  }
}
const name = path.basename(file).replace(/\.\w+$/, '') + (opt('crop', '') ? '_crop' : '');
for (const [suffix, im] of [['alpha', imgA], ['alpha_ours', imgB], ['orig', imgO], ['ours', imgU]]) {
  const out = `re/previews/${name}_water_${suffix}.png`;
  fs.writeFileSync(out, png(W, Hh, im));
  console.log(`wrote ${out} ${W}x${Hh}`);
}
const nWater = [...rows.keys()].length;
const hist = new Map(); for (const v of verts) hist.set(v.a, (hist.get(v.a) ?? 0) + 1);
console.log(`${XS}x${YS} wcoords, ${nWater} water wcoords, ${verts.length} shared vertices, ${origTris.length} triangles (original); ${oursTris.length} (ours)`);
console.log(`vertex alpha after smoothing: ${hist.get(235) ?? 0} at 235 (deep), ${hist.get(0) ?? 0} at 0, ${verts.length - (hist.get(235) ?? 0) - (hist.get(0) ?? 0)} in between`);
console.log(`coastline jaggedness (iso 0.2 length / blurred): original ${jag(aOrig).toFixed(3)}, ours ${jag(aOurs).toFixed(3)}`);
if (args.includes('--hist')) {
  const top = [...hist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12).map(([a, n]) => `${a}:${n}`).join(' ');
  console.log(`most common smoothed vertex alphas: ${top}`);
}
