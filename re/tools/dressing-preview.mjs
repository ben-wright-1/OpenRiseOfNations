// Software preview of a scenario's terrain with the static dressing, through the game camera (orthographic, yaw 45°,
// 30° above the horizon; terrain-spec B7) or top-down. Checks docs/terrain-dressing-spec.md before writing Metal code.
//   node re/tools/dressing-preview.mjs <file.scx> [--u 12] [--crop wx0,wy0,wx1,wy1] [--topdown] [--out re/previews/x.png]
//        [--layers base,trans,rivers,mountains,snow,doobers,water,riverwater] [--quadrant 0..3] [--install dir]
// --u: world units per pixel (zoom 6 = 3, zoom 5 = 4.5, zoom 4 = 6). Layers are drawn in the game's order (spec §6):
// base land (+ transitions) -> river troughs -> mountains -> snowcaps -> doobers -> sea water -> river water.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { parseScenario, decompress, png } from './scxdump.mjs';
import { buildTransitions, listMasks, maskUV, resolveCI } from './transitions.mjs';
import * as D from './dressing.mjs';

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf('--' + k); return i >= 0 ? args[i + 1] : d; };
const install = opt('install', path.join(os.homedir(), 'Games/RiseOfNations'));
const file = args[0];
const U = +opt('u', 12), TOP = args.includes('--topdown');
const LAYERS = new Set(opt('layers', 'base,trans,rivers,mountains,snow,doobers,water,riverwater').split(','));
const sc = parseScenario(decompress(fs.readFileSync(file)));
const t = sc.terrain, ts = D.readTileset(install, t.tileset);
const [CX0, CY0, CX1, CY1] = (opt('crop', '') || `0,0,${t.xs},${t.ys}`).split(',').map(Number);
const inCrop = (x, y, m = 0) => x >= CX0 * 768 - m && x <= CX1 * 768 + m && y >= CY0 * 768 - m && y <= CY1 * 768 + m;

// ------------------------------------------------------------------ projection
const project = TOP
  ? (x, y, z) => [x / U, y / U, -z]
  : (x, y, z) => [0.70711 * (x - y) / U, (0.35355 * (x + y) - 0.86603 * z) / U, -(0.6124 * x + 0.6124 * y + 0.5 * z)];
let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
for (const [x, y] of [[CX0, CY0], [CX1, CY0], [CX0, CY1], [CX1, CY1]]) for (const z of [-300, 1900]) {
  const [px, py] = project(x * 768, y * 768, z);
  minX = Math.min(minX, px); maxX = Math.max(maxX, px); minY = Math.min(minY, py); maxY = Math.max(maxY, py);
}
const W = Math.ceil(maxX - minX) + 2, H = Math.ceil(maxY - minY) + 2;
const col = new Float32Array(W * H * 3), dep = new Float32Array(W * H).fill(Infinity);
for (let i = 0; i < W * H; i++) { col[i * 3] = 0.08; col[i * 3 + 1] = 0.08; col[i * 3 + 2] = 0.1; }
const vtx = (x, y, z, a = {}) => { const [sx, sy, d] = project(x, y, z); return { sx: sx - minX + 1, sy: sy - minY + 1, d, ...a }; };

// ------------------------------------------------------------------ rasteriser
// shade(w0, w1, w2) -> [r, g, b, a] (0..1) or null; depth: 'le' (LESSEQUAL) or null (no test); zwrite; blend (SRCALPHA/INVSRCALPHA)
let fragCount = 0;
function tri(a, b, c, shade, { depth = 'le', zwrite = false, blend = true } = {}) {
  const area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
  if (area === 0) return;
  const x0 = Math.max(0, Math.floor(Math.min(a.sx, b.sx, c.sx))), x1 = Math.min(W - 1, Math.ceil(Math.max(a.sx, b.sx, c.sx)));
  const y0 = Math.max(0, Math.floor(Math.min(a.sy, b.sy, c.sy))), y1 = Math.min(H - 1, Math.ceil(Math.max(a.sy, b.sy, c.sy)));
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
    const px = x + 0.5, py = y + 0.5;
    const w0 = ((b.sx - px) * (c.sy - py) - (b.sy - py) * (c.sx - px)) / area;
    const w1 = ((c.sx - px) * (a.sy - py) - (c.sy - py) * (a.sx - px)) / area;
    const w2 = 1 - w0 - w1;
    if (w0 < -1e-7 || w1 < -1e-7 || w2 < -1e-7) continue;
    const i = y * W + x, d = w0 * a.d + w1 * b.d + w2 * c.d;
    if (depth === 'le' && d > dep[i] + 0.5) continue;
    const s = shade(w0, w1, w2);
    if (!s || s[3] <= 0) continue; // alpha test GREATER 0 on the dressing states
    fragCount++;
    const k = blend ? Math.min(1, s[3]) : 1;
    for (let ch = 0; ch < 3; ch++) col[i * 3 + ch] = col[i * 3 + ch] * (1 - k) + Math.min(1, Math.max(0, s[ch])) * k;
    if (zwrite) dep[i] = d;
  }
}
const lerp3 = (a, b, c, w0, w1, w2, k) => w0 * a[k] + w1 * b[k] + w2 * c[k];

// ------------------------------------------------------------------ textures
const tex = rel => D.loadTexture(install, rel);
function sample(im, u, v, wrap = true) {
  let fx = u * im.width - 0.5, fy = v * im.height - 0.5;
  const w = (n, m) => wrap ? ((n % m) + m) % m : Math.min(Math.max(n, 0), m - 1);
  const x0 = Math.floor(fx), y0 = Math.floor(fy), tx = fx - x0, ty = fy - y0, out = [0, 0, 0, 0];
  for (const [dx, dy, k] of [[0, 0, (1 - tx) * (1 - ty)], [1, 0, tx * (1 - ty)], [0, 1, (1 - tx) * ty], [1, 1, tx * ty]]) {
    const s = (w(y0 + dy, im.height) * im.width + w(x0 + dx, im.width)) * 4;
    for (let c = 0; c < 4; c++) out[c] += im.rgba[s + c] * k;
  }
  return out.map(c => c / 255);
}

// ------------------------------------------------------------------ terrain lighting (B3: baked Lambert, light from +x and above)
const VX = t.vxs, L = [Math.SQRT1_2, 0, Math.SQRT1_2];
const lit = new Float32Array(VX * t.vys * 3);
for (let ty = 0; ty < t.vys; ty++) for (let tx = 0; tx < VX; tx++) {
  const h = (x, y) => D.heightAt(t, x, y);
  const n = [-(h(tx + 1, ty) - h(tx - 1, ty)), -(h(tx, ty + 1) - h(tx, ty - 1)), 2 * 192], l = Math.hypot(...n);
  const nl = Math.max(0, (n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / l);
  for (let c = 0; c < 3; c++) lit[(ty * VX + tx) * 3 + c] = Math.min(1, ts.lighting.ambient[c] + ts.lighting.diffuse[c] * nl);
}
const litAt = (tx, ty) => { const i = (Math.min(Math.max(ty, 0), t.vys - 1) * VX + Math.min(Math.max(tx, 0), VX - 1)) * 3; return [lit[i], lit[i + 1], lit[i + 2]]; };

// ------------------------------------------------------------------ base land + transitions (terrain-spec B2 / B2a)
const tsXml = fs.readFileSync(path.join(install, 'Data/tilesets.xml'), 'latin1').match(new RegExp(`<TILESET name="${t.tileset}"[\\s\\S]*?</TILESET>`, 'i'))[0];
const baseDir = tsXml.match(/<BASEDIR path="([^"]+)"/)[1];
const secX = n => tsXml.match(new RegExp(`<${n}>([\\s\\S]*?)</${n}>`))?.[1] ?? '';
const bases = n => [...secX(n).matchAll(/<BASE name="[^"]*"(?: supertile="(\d)")?>\s*<TEXTURE name="([^"]+)"/g)].map(m => ({ supertile: m[1] === '1', file: m[2] }));
const land = [bases('BASELAND'), bases('COASTAL'), bases('OCEAN')];
const landCache = new Map();
const landTex = f => { if (!landCache.has(f)) landCache.set(f, D.loadTexture(install, resolveCI(install, path.posix.join(baseDir, f)) ? path.posix.join(baseDir, f) : path.posix.join('terrain art', f))); return landCache.get(f); };
function landTexel(type, sub, wx, wy, u, v) {
  const b = land[type][Math.min(sub, land[type].length - 1)], im = landTex(b.file);
  const n = b.supertile ? Math.floor(im.width / 3) : im.width, ox = b.supertile ? (wx % 3) * n : 0, oy = b.supertile ? (wy % 3) * n : 0;
  const s = ((oy + Math.min(n - 1, Math.floor(v * n))) * im.width + ox + Math.min(n - 1, Math.floor(u * n))) * 4;
  return [im.rgba[s] / 255, im.rgba[s + 1] / 255, im.rgba[s + 2] / 255];
}
const patchAt = new Map();
if (LAYERS.has('trans')) {
  const tr = buildTransitions({ xs: t.xs, ys: t.ys, land: t.land.map(l => Math.min(l, 2)), landSub: t.landSub, numTiles: land.map(l => l.length),
    coastBase: +(tsXml.match(/<COAST_BASE value="(\d+)"/)?.[1] ?? 0), seed: sc.header.map_seed ?? 0, masks: listMasks(install) });
  for (const p of tr.patches) if (!p.missing) patchAt.set(p.y * t.xs + p.x, { ...p, img: D.loadTexture(install, path.relative(install, p.mask.file)) });
}
if (LAYERS.has('base')) {
  for (let wy = Math.max(0, CY0 - 1); wy < Math.min(t.ys, CY1 + 1); wy++) for (let wx = Math.max(0, CX0 - 1); wx < Math.min(t.xs, CX1 + 1); wx++) {
    const i = wy * t.xs + wx, type = Math.min(t.land[i], 2), sub = type === 0 ? t.landSub[i] : 0, p = patchAt.get(i);
    const shade = (a, b, c) => (w0, w1, w2) => {
      const u = lerp3(a.uv, b.uv, c.uv, w0, w1, w2, 0), v = lerp3(a.uv, b.uv, c.uv, w0, w1, w2, 1);
      let rgb = landTexel(type, sub, wx, wy, u, v);
      if (p) { // B2a overlay patch: next-lower texture, alpha = mask
        const [mu, mv] = maskUV(u, v, p.orient, p.quad), m = sample(p.img, mu, mv, false)[3];
        const o = landTexel(p.overLand, p.overSub, wx, wy, u, v);
        rgb = rgb.map((x, k) => x * (1 - m) + o[k] * m);
      }
      return [0, 1, 2].map(k => rgb[k] * lerp3(a.lit, b.lit, c.lit, w0, w1, w2, k)).concat(1);
    };
    for (let j = 0; j < 4; j++) for (let k = 0; k < 4; k++) {
      const V = (dx, dy) => { const tx = wx * 4 + k + dx, ty = wy * 4 + j + dy; return vtx(tx * 192, ty * 192, D.heightAt(t, tx, ty), { uv: [(k + dx) / 4, (j + dy) / 4], lit: litAt(tx, ty) }); };
      const v00 = V(0, 0), v10 = V(1, 0), v01 = V(0, 1), v11 = V(1, 1);
      tri(v00, v10, v01, shade(v00, v10, v01), { zwrite: true, blend: false });
      tri(v01, v10, v11, shade(v01, v10, v11), { zwrite: true, blend: false });
    }
  }
}

// ------------------------------------------------------------------ rivers: troughs (state trough_rs: no depth test, tex x diffuse)
const rivers = D.buildRivers(t);
const stats = [];
if (LAYERS.has('rivers')) {
  for (const r of rivers) for (const s of r.sections) {
    if (!inCrop(s.A[0], s.A[1], 1536)) continue;
    const im = tex(ts.rivers[s.texture]);
    const V = s.verts.map(v => vtx(v.x, v.y, v.z, { uv: v.uv, a: v.alpha / 255 }));
    for (let i = 0; i < s.tris.length; i += 3) {
      const [a, b, c] = [V[s.tris[i]], V[s.tris[i + 1]], V[s.tris[i + 2]]];
      tri(a, b, c, (w0, w1, w2) => {
        const tx = sample(im, lerp3(a.uv, b.uv, c.uv, w0, w1, w2, 0), lerp3(a.uv, b.uv, c.uv, w0, w1, w2, 1));
        const al = w0 * a.a + w1 * b.a + w2 * c.a, l = Math.min(1, ts.lighting.ambient[1] + ts.lighting.diffuse[1] * Math.SQRT1_2);
        return [tx[0] * l, tx[1] * l, tx[2] * l, tx[3] * al];
      }, { depth: null });
    }
  }
  stats.push(`rivers ${rivers.length}: ${rivers.map(r => r.sections.length).join('/')} sections`);
}

// ------------------------------------------------------------------ mountains ("Mountain Blend": depth LE + write, alpha test > 0, blend)
const M = D.buildMountains(t, install, { tileset: ts });
if (LAYERS.has('mountains')) {
  const base = tex(ts.mountains.base), alpha = tex(ts.mountains.alpha);
  for (const s of M.stamps) {
    if (s.error) continue;
    const ring = tex(s.template.ringAlpha), main = tex(s.template.mainAlpha);
    const V = s.verts.map(v => vtx(v.x, v.y, v.z, { uv0: v.uv0, uv1: v.uv1, lit: litAt(v.tx, v.ty), wx: v.x, wy: v.y }));
    for (const [t0, t1] of [[alpha, ring], [base, main]]) { // draw 1: ALPHATEX + ring; draw 2: BASETEX + main (repeat_last_prim)
      for (let i = 0; i < s.tris.length; i += 3) {
        const [a, b, c] = [V[s.tris[i]], V[s.tris[i + 1]], V[s.tris[i + 2]]];
        if (!inCrop(a.wx, a.wy, 768)) continue;
        tri(a, b, c, (w0, w1, w2) => {
          const s0 = sample(t0, lerp3(a.uv0, b.uv0, c.uv0, w0, w1, w2, 0), lerp3(a.uv0, b.uv0, c.uv0, w0, w1, w2, 1));
          const s1 = sample(t1, lerp3(a.uv1, b.uv1, c.uv1, w0, w1, w2, 0), lerp3(a.uv1, b.uv1, c.uv1, w0, w1, w2, 1), false);
          // stage 0 MODULATE(tex, diffuse) / SEL1(tex); stage 1 ADDSIGNED(tex, current) / MODULATE(tex, current)
          return [0, 1, 2].map(k => s0[k] * lerp3(a.lit, b.lit, c.lit, w0, w1, w2, k) + s1[k] - 0.5).concat(s0[3] * s1[3]);
        }, { zwrite: true });
      }
    }
  }
  stats.push(`mountains ${M.stamps.length}`);
}

// ------------------------------------------------------------------ snowcaps ("Weathering Underlay": no depth test, rgb = tex(UV0) x diffuse, a = tex(UV1).a)
if (LAYERS.has('snow') && ts.underlays.snowcap) {
  const im = tex(ts.underlays.snowcap);
  for (const s of M.stamps) {
    if (!s.snowcap) continue;
    const p = D.snowcapPatch(t, s.snowcap, +opt('quadrant', 0));
    const V = p.verts.map(v => vtx(v.x, v.y, v.z, { uv0: v.uv0, uv1: v.uv1, lit: litAt(v.tx, v.ty) }));
    for (let i = 0; i < p.tris.length; i += 3) {
      const [a, b, c] = [V[p.tris[i]], V[p.tris[i + 1]], V[p.tris[i + 2]]];
      tri(a, b, c, (w0, w1, w2) => {
        const c0 = sample(im, lerp3(a.uv0, b.uv0, c.uv0, w0, w1, w2, 0), lerp3(a.uv0, b.uv0, c.uv0, w0, w1, w2, 1), false);
        const a1 = sample(im, lerp3(a.uv1, b.uv1, c.uv1, w0, w1, w2, 0), lerp3(a.uv1, b.uv1, c.uv1, w0, w1, w2, 1), false)[3];
        return [0, 1, 2].map(k => c0[k] * lerp3(a.lit, b.lit, c.lit, w0, w1, w2, k)).concat(a1);
      }, { depth: null });
    }
  }
}

// ------------------------------------------------------------------ doobers ("Doober Blend" #7: depth LE, no write, alpha test > 0, tex x white)
const DB = D.buildDoobers(t, install, t.tileset);
if (LAYERS.has('doobers')) {
  for (const d of DB) {
    if (!inCrop(d.x, d.y)) continue;
    const im = tex(d.piece.file);
    const V = d.verts.map(v => vtx(v.x, v.y, v.z, { uv: v.uv }));
    for (let i = 0; i < 6; i += 3) {
      const [a, b, c] = [V[d.tris[i]], V[d.tris[i + 1]], V[d.tris[i + 2]]];
      tri(a, b, c, (w0, w1, w2) => sample(im, lerp3(a.uv, b.uv, c.uv, w0, w1, w2, 0), lerp3(a.uv, b.uv, c.uv, w0, w1, w2, 1), false));
    }
  }
  stats.push(`doobers ${DB.length}`);
}

// ------------------------------------------------------------------ sea water (terrain-spec B4, simplified: flat colour, depth fade)
const WATER = [0, 0.53, 0.93];
if (LAYERS.has('water')) {
  const fade = (h, river) => (h <= -55 ? 245 / 255 : h >= 0 ? 0 : (245 / 255) * (-h / 55)) * (river ? 0.25 : 1);
  for (let ty = Math.max(0, CY0 * 4 - 4); ty < Math.min(t.vys - 1, CY1 * 4 + 4); ty++) for (let tx = Math.max(0, CX0 * 4 - 4); tx < Math.min(VX - 1, CX1 * 4 + 4); tx++) {
    const river = (t.tmask[Math.min(ty, t.tys - 1) * t.txs + Math.min(tx, t.txs - 1)] & 0x02) !== 0;
    const V = (dx, dy) => { const h = D.heightAt(t, tx + dx, ty + dy); return vtx((tx + dx) * 192, (ty + dy) * 192, Math.max(0, h), { a: fade(h, river) }); };
    const v00 = V(0, 0), v10 = V(1, 0), v01 = V(0, 1), v11 = V(1, 1);
    if (v00.a + v10.a + v01.a + v11.a === 0) continue;
    for (const [a, b, c] of [[v00, v10, v01], [v01, v10, v11]]) tri(a, b, c, (w0, w1, w2) => [...WATER, w0 * a.a + w1 * b.a + w2 * c.a]);
  }
}
// ------------------------------------------------------------------ river water quads (water_rs: depth LE, a = diffuse.a x RIVERTEX_WATER.a)
if (LAYERS.has('riverwater')) {
  const im = tex(ts.rivers.water);
  for (const r of rivers) for (const s of r.sections) {
    if (!inCrop(s.A[0], s.A[1], 1536)) continue;
    const V = s.water.verts.map(v => vtx(v.x, v.y, v.z, { uv1: v.uv1, a: v.alpha / 255 }));
    for (let i = 0; i < 6; i += 3) {
      const [a, b, c] = [V[s.water.tris[i]], V[s.water.tris[i + 1]], V[s.water.tris[i + 2]]];
      tri(a, b, c, (w0, w1, w2) => [...WATER, (w0 * a.a + w1 * b.a + w2 * c.a) * sample(im, lerp3(a.uv1, b.uv1, c.uv1, w0, w1, w2, 0), lerp3(a.uv1, b.uv1, c.uv1, w0, w1, w2, 1))[3]]);
    }
  }
}

const out8 = new Uint8Array(W * H * 3);
for (let i = 0; i < out8.length; i++) out8[i] = Math.round(Math.min(1, Math.max(0, col[i])) * 255);
const name = path.basename(file).replace(/\.\w+$/, '');
const out = opt('out', `re/previews/${name}_dressing${TOP ? '_top' : ''}${opt('crop', '') ? '_crop' : ''}.png`);
fs.writeFileSync(out, png(W, H, out8));
console.log(`wrote ${out} ${W}x${H} (u=${U}${TOP ? ', top-down' : ''}); ${stats.join(', ')}; ${fragCount} fragments`);
