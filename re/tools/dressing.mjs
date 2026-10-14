// Reference implementation of the static terrain dressing: mountain overlays (+ snowcaps), river troughs and river
// water quads, land doobers (billboards), and the unit team-colour table. See docs/terrain-dressing-spec.md.
// Mirrors (paraphrased, no game code): MountainRange::init, TerrainOut::adjust_for_mountains / fill_mountain_data,
// CoordInfo::add_geometry (snowcap), TerrainOut::add_weather_underlay, River::calculate, Spline::clamp_knots /
// generate_bspline / calc_bspline_basis / build_normals / next_at_dist, RiverOut::create_sections,
// RiverSection::create_trough_and_water / create_trough_edge, RiverSectionOut::create_water_plane, RiverOut::render,
// Fractal::init / get_height, Doober::draw, GraphicPieces::draw_doober, ZSpriteAccumMethod::execute, TeamColors::init.
//
//   import { buildMountains, buildRivers, buildDoobers, readTileset } from './dressing.mjs';
//
// CLI (stats + self-checks): node re/tools/dressing.mjs <file.scx> [--install ~/Games/RiseOfNations]

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseScenario, decompress } from './scxdump.mjs';
import { parseTGA } from './ronassets.mjs';
import { resolveCI } from './transitions.mjs';

const f32 = Math.fround;
export const WCOORD = 768, TCOORD = 192;

// div_3_table[(int)v >> 6]: world units -> tcoord, floor division (the table also covers negative indices).
export const tcoordOf = v => Math.floor(Math.floor(Math.trunc(v) / 64) / 3);

// Random::get(lo, hi) on a 32-bit LCG state (same generator as the tree placement and the texture fractal).
export function lcg(seed) {
  let s = seed >>> 0;
  return (lo, hi) => { s = (Math.imul(s, 0x19660d) + 0x3c6ef35f) >>> 0; return lo + Math.floor(((s & 0xffff) * (hi - lo)) / 65536); };
}

// ------------------------------------------------------------------ data files

const texCache = new Map();
export function loadTexture(install, rel) {
  const key = install + '|' + rel.toLowerCase();
  if (!texCache.has(key)) {
    const p = resolveCI(install, rel.replace(/\\+/g, '/'));
    if (!p) throw new Error(`texture not found: ${rel}`);
    texCache.set(key, parseTGA(fs.readFileSync(p)));
  }
  return texCache.get(key);
}

/** The <TILESET> entry of Data/tilesets.xml (case-insensitive name match), reduced to what the dressing needs. */
export function readTileset(install, name) {
  const xml = fs.readFileSync(path.join(install, 'Data/tilesets.xml'), 'latin1');
  const m = [...xml.matchAll(/<TILESET name="([^"]+)"([^>]*)>([\s\S]*?)<\/TILESET>/g)].find(b => b[1].toLowerCase() === name.toLowerCase());
  if (!m) throw new Error(`tileset ${name} not found`);
  const [, tsName, attrs, body] = m;
  const attr = k => attrs.match(new RegExp(`${k}="([^"]*)"`))?.[1];
  const section = k => body.match(new RegExp(`<${k}\\b[^>]*>([\\s\\S]*?)</${k}>`))?.[1] ?? '';
  const file = k => section(k).match(/file="([^"]+)"/)?.[1];
  const color = (s, k) => +(s.match(new RegExp(`<COLOR name="${k}">([^<]+)<`))?.[1] ?? 0);
  const amb = section('AMBIENT'), dif = section('DIFFUSE');
  return {
    name: tsName,
    snowcaps: attr('snowcaps') === '1',
    mountains: { base: section('MOUNTAINS').match(/<BASETEX file="([^"]+)"/)?.[1], alpha: section('MOUNTAINS').match(/<ALPHATEX file="([^"]+)"/)?.[1] },
    rivers: { normal: file('RIVERTEX_NORMAL'), delta: file('RIVERTEX_DELTA'), halfdelta: file('RIVERTEX_HALFDELTA'), water: file('RIVERTEX_WATER') },
    underlays: Object.fromEntries([...section('UNDERLAYS').matchAll(/<TEXTURE id="([^"]+)" file="([^"]+)"/g)].map(u => [u[1], u[2]])),
    lighting: {
      ambient: ['red', 'green', 'blue'].map(c => color(amb, c) * color(amb, 'alpha')),
      diffuse: ['red', 'green', 'blue'].map(c => color(dif, c) * color(dif, 'alpha')),
    },
  };
}

/** Mountain range templates, in Data/effects_graphics.xml <MOUNTAINS> order (= the range index stored in chunk 69). */
export function readMountainTemplates(install) {
  const xml = fs.readFileSync(path.join(install, 'Data/effects_graphics.xml'), 'latin1');
  const sec = xml.match(/<MOUNTAINS>([\s\S]*?)<\/MOUNTAINS>/)?.[1] ?? '';
  return [...sec.matchAll(/<MOUNTAIN\b([^>]*)>([\s\S]*?)<\/MOUNTAIN>/g)].map((m, index) => {
    const a = k => m[1].match(new RegExp(`${k}="([^"]*)"`))?.[1] ?? '';
    const f = k => m[2].match(new RegExp(`<${k} file="([^"]+)"`))?.[1] ?? '';
    const area = a('area').toLowerCase();
    // Mountains::init: "sm"/"sml" -> small (1), "med" -> 2, "lg" -> 3; anything else keeps size 0
    const size = area === 'sm' || area === 'sml' ? 1 : area === 'med' ? 2 : area === 'lg' ? 3 : 0;
    return { index, area, size, height: +a('height'), template: f('TEMPLATE_TEX'), mainAlpha: f('MAIN_ALPHA_TEX'), ringAlpha: f('RING_ALPHA_TEX') };
  });
}

// ------------------------------------------------------------------ terrain helpers

export const heightAt = (t, tx, ty) => t.heights[Math.min(Math.max(ty, 0), t.vys - 1) * t.vxs + Math.min(Math.max(tx, 0), t.vxs - 1)];

/** TerrainOut::find_data_z: the vertex grid split into (x,y)(x+1,y)(x,y+1) / (x,y+1)(x+1,y)(x+1,y+1). */
export function findDataZ(t, x, y) {
  const fx = x / TCOORD, fy = y / TCOORD;
  const tx = Math.min(Math.max(Math.floor(fx), 0), t.vxs - 2), ty = Math.min(Math.max(Math.floor(fy), 0), t.vys - 2);
  const dx = fx - tx, dy = fy - ty;
  const h00 = heightAt(t, tx, ty), h10 = heightAt(t, tx + 1, ty), h01 = heightAt(t, tx, ty + 1), h11 = heightAt(t, tx + 1, ty + 1);
  return dx + dy <= 1 ? h00 + (h10 - h00) * dx + (h01 - h00) * dy : h11 + (h01 - h11) * (1 - dx) + (h10 - h11) * (1 - dy);
}

/** TData water test as the river code does it (`mask & 0x30 == 0x20`): file bit 0x10 of chunk 68. */
export const isWaterTcoord = (t, tx, ty) => tx >= 0 && ty >= 0 && tx < t.txs && ty < t.tys && (t.tmask[ty * t.txs + tx] & 0x10) !== 0;

// ------------------------------------------------------------------ mountains

/**
 * MountainRange::init: turn a TEMPLATE_TEX displacement image into a template mesh.
 * One vertex per 4x4-pixel sample (= one tcoord, 48 world units per pixel) whose alpha is non-zero; z = blue byte *
 * height*3/255; faces on every sample cell whose 4 corners exist; UV0 = pixel/16 (one repeat per wcoord); UV1 = the
 * position inside the bounding box of all non-zero-alpha pixels (maps the 128x128 MAIN/RING alpha textures).
 */
export function buildRange(img, height) {
  const W = img.width, H = img.height, A = (x, y) => img.rgba[(y * W + x) * 4 + 3], B = (x, y) => img.rgba[(y * W + x) * 4 + 2];
  const ox = (W >> 1) % 16, oy = (H >> 1) % 16, hz = f32(height * 3);
  let minx = W, miny = H, maxx = 0, maxy = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (A(x, y)) { minx = Math.min(minx, x); miny = Math.min(miny, y); maxx = Math.max(maxx, x); maxy = Math.max(maxy, y); }
  const tw = (W >> 2) + 3, th = (H >> 2) + 3, tl = new Int32Array(tw * th).fill(-1);
  const verts = [], uv0 = [], uv1 = [];
  let peak = null, peakZ = 0;
  for (let y = oy; y < H; y += 4) for (let x = ox; x < W; x += 4) {
    if (!A(x, y)) continue;
    tl[(y >> 2) * tw + (x >> 2)] = verts.length;
    const vx = f32(f32((x - (W >> 1)) * 192) * 0.25), vy = f32(f32((y - (H >> 1)) * 192) * 0.25), vz = f32(f32(f32(B(x, y)) * hz) / 255);
    verts.push({ x: vx, y: vy, z: vz, px: x, py: y });
    if (vz > peakZ) { peakZ = vz; peak = { x: vx, y: vy, z: vz, tx: tcoordOf(vx), ty: tcoordOf(vy) }; }
    uv0.push([f32(x * 0.0625), f32(y * 0.0625)]);
    uv1.push([f32((x - minx) / (maxx - minx)), f32((y - miny) / (maxy - miny))]);
  }
  const faces = [], cells = [], wcoords = [];
  const hw = (W >> 1) - ox, hh = (H >> 1) - oy;
  for (let y = oy; y < H; y += 4) for (let x = ox; x < W; x += 4) {
    const a = tl[(y >> 2) * tw + (x >> 2)], b = tl[(y >> 2) * tw + (x >> 2) + 1], c = tl[((y >> 2) + 1) * tw + (x >> 2)], d = tl[((y >> 2) + 1) * tw + (x >> 2) + 1];
    if (a < 0 || b < 0 || c < 0 || d < 0) continue;
    faces.push(a, b, c, c, b, d); // same diagonal as the terrain mesh
    cells.push([Math.trunc(x / 4) - Math.trunc(hw / 4), Math.trunc(y / 4) - Math.trunc(hh / 4)]);
    const w = [Math.trunc(x / 16) - Math.trunc(hw / 16), Math.trunc(y / 16) - Math.trunc(hh / 16)];
    if (!wcoords.some(q => q[0] === w[0] && q[1] === w[1])) wcoords.push(w);
  }
  // Far-zoom LOD ("mid"): an 8-pixel (2-tcoord) grid over every 2x2 tcoord block that holds at least one complete cell.
  const fw = (W >> 3) + 3, fl = new Int32Array(fw * ((H >> 3) + 3)).fill(-1), lodVerts = [], lodFaces = [];
  const full = (yy, xx) => [[0, 0], [0, 1], [1, 0], [1, 1]].every(([j, i]) => tl[((yy >> 2) + j) * tw + (xx >> 2) + i] >= 0);
  for (let y = oy; y < H; y += 8) for (let x = ox; x < W; x += 8) {
    if (!(full(y, x) || full(y, x + 4) || full(y + 4, x) || full(y + 4, x + 4))) continue;
    for (const dy of [0, 8]) for (const dx of [0, 8]) {
      const k = ((y + dy) >> 3) * fw + ((x + dx) >> 3);
      if (fl[k] >= 0) continue;
      fl[k] = lodVerts.length;
      const t = tl[((y + dy) >> 2) * tw + ((x + dx) >> 2)];
      lodVerts.push(t >= 0 ? { ...verts[t] } : { x: f32((x + dx - (W >> 1)) * 48), y: f32((y + dy - (H >> 1)) * 48), z: 0, px: x + dx, py: y + dy });
    }
  }
  for (let y = oy; y < H; y += 8) for (let x = ox; x < W; x += 8) {
    const a = fl[(y >> 3) * fw + (x >> 3)], b = fl[(y >> 3) * fw + (x >> 3) + 1], c = fl[((y >> 3) + 1) * fw + (x >> 3)], d = fl[((y >> 3) + 1) * fw + (x >> 3) + 1];
    if (a >= 0 && b >= 0 && c >= 0 && d >= 0) lodFaces.push(a, b, c, c, b, d);
  }
  return { W, H, verts, uv0, uv1, faces, cells, wcoords, peak, bbox: [minx, miny, maxx, maxy], lodVerts, lodFaces };
}

/**
 * Place every chunk-69 stamp. With stored heights (scenarios) the game calls adjust_for_mountains(1): template vertices
 * are matched to terrain grid vertices and take the *stored* height (template z is only added on the random-map path).
 * Returns per stamp: vertices {x,y,z,uv0,uv1,tx,ty}, triangle list, covered wcoords, and the snowcap underlay if any.
 */
export function buildMountains(t, install, { templates = readMountainTemplates(install), tileset } = {}) {
  const ranges = new Map(), stamps = [];
  for (const s of t.mountains ?? []) {
    const tpl = templates[s.index];
    if (!tpl) { stamps.push({ ...s, error: 'invalid range index' }); continue; }
    if (!ranges.has(s.index)) ranges.set(s.index, buildRange(loadTexture(install, tpl.template), tpl.height));
    const r = ranges.get(s.index);
    const stx = s.wx * 4, sty = s.wy * 4; // loc = (wx*768, wy*768): the template centre sits on the wcoord's NW corner
    const byT = new Map(r.verts.map((v, i) => [`${Math.round(v.x / 192)},${Math.round(v.y / 192)}`, i]));
    const remap = new Int32Array(r.verts.length).fill(-1), verts = [];
    for (const [mx, my] of r.wcoords) {
      const wx = s.wx + mx, wy = s.wy + my;
      for (let gy = 0; gy <= 4; gy++) for (let gx = 0; gx <= 4; gx++) {
        const tx = wx * 4 + gx, ty = wy * 4 + gy, i = byT.get(`${tx - stx},${ty - sty}`);
        if (i === undefined || remap[i] >= 0) continue;
        remap[i] = verts.length;
        verts.push({ x: tx * TCOORD, y: ty * TCOORD, z: heightAt(t, tx, ty), tz: r.verts[i].z, tx, ty, uv0: r.uv0[i], uv1: r.uv1[i] });
      }
    }
    const tris = [];
    for (let i = 0; i < r.faces.length; i++) tris.push(remap[r.faces[i]]);
    // Snowcap (CoordInfo flag 0x200): medium and large ranges, on the wcoord that holds the template's highest vertex.
    let snowcap = null;
    const ptx = stx + r.peak.tx, pty = sty + r.peak.ty;
    const onPeak = r.wcoords.some(([mx, my]) => s.wx + mx === ptx >> 2 && s.wy + my === pty >> 2);
    if (onPeak && tpl.size !== 1 && (!tileset || tileset.snowcaps)) {
      const size = tpl.size === 2 || tpl.size === 3 ? 6 : 0;
      if (size) {
        const lim = t.xs * 4 - size; // the game clamps both axes against the map width
        const x0 = Math.min(Math.max(((ptx >> 2) * 4 + (ptx & 3) - 2) - (size >> 1) + 2, 0), lim);
        const y0 = Math.min(Math.max(((pty >> 2) * 4 + (pty & 3) - 2) - (size >> 1) + 2, 0), lim);
        snowcap = { x0, y0, size, peak: [ptx, pty] };
      }
    }
    stamps.push({ ...s, range: s.index, template: tpl, verts, tris, wcoords: r.wcoords.map(([mx, my]) => [s.wx + mx, s.wy + my]), snowcap, coordCheck: r.wcoords.length === s.numCoords });
  }
  return { ranges, stamps };
}

/**
 * TerrainOut::add_weather_underlay for the snowcap (variant mode 1, aligned): the patch is the terrain's own vertex grid
 * over [x0, x0+size] (snapped to even tcoords, size grows to stay even). UV0 spans the whole texture once; UV1 is one
 * quadrant of the 2x2 alpha atlas. `quadrant` comes from random_map_pool (filled with the non-synchronised
 * internal_random at Terrain::init), so the game's choice is not reproducible; pass 0..3.
 */
export function snowcapPatch(t, cap, quadrant = 0) {
  let { x0, y0 } = cap, w = cap.size, h = cap.size;
  if (x0 & 1) { x0--; w++; if (w & 1) w++; }
  if (y0 & 1) { y0--; h++; if (h & 1) h++; }
  const u0 = quadrant === 1 || quadrant === 3 ? 0.5 : 0, v0 = quadrant >= 2 ? 0.5 : 0;
  const verts = [];
  for (let j = 0; j <= h; j++) for (let i = 0; i <= w; i++) {
    const tx = x0 + i, ty = y0 + j;
    verts.push({ x: tx * TCOORD, y: ty * TCOORD, z: heightAt(t, tx, ty), tx, ty, uv0: [i / w, j / h], uv1: [u0 + i * 0.5 / w, v0 + j * 0.5 / h], alpha: 0.5 });
  }
  const tris = [];
  for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
    const a = j * (w + 1) + i, b = a + 1, c = a + w + 1, d = c + 1;
    tris.push(a, b, c, c, b, d);
  }
  return { x0, y0, w, h, verts, tris };
}

// ------------------------------------------------------------------ fractal (rivers)

/** Fractal::init(xs, ys, smooth, -, flags, seed) + get_height. Midpoint displacement on an (xs+1) x (ys+1) byte grid. */
export function makeFractal(xs, ys, smooth, flags, seed) {
  const col = ys + 1, F = new Uint8Array((xs + 1) * col), at = (x, y) => F[x * col + y];
  const rnd = lcg(seed), f1 = flags & 1;
  smooth = Math.max(0, Math.min(5, smooth));
  const clamp = v => v < 0 ? 0 : v > 255 ? 255 : v;
  for (let L = smooth; L >= 0; L--) {
    const mask = (1 << (L + 1)) - 1, s = 1 << L, amp = 1 << (7 - smooth + L), span = (1 << (8 - smooth + L)) - 1;
    if (!f1) for (let y = 0; y <= ys; y++) F[xs * col + y] = F[y];
    for (let cx = 0; cx < (xs >> L) + f1; cx++) for (let cy = 0; cy < (ys >> L); cy++) {
      const X = cx << L, Y = cy << L;
      if (L === smooth) { F[X * col + Y] = rnd(0, 255); continue; }
      if ((mask & X) === 0) {
        if ((mask & Y) === 0) continue; // set on a coarser level
        F[X * col + Y] = clamp(((at(X, Y + s) + at(X, Y - s) + 1) >> 1) + rnd(0, span) - amp);
      } else if ((mask & Y) === 0) {
        F[X * col + Y] = clamp(((at(X + s, Y) + 1 + at(X - s, Y)) >> 1) + rnd(0, span) - amp);
      } else {
        F[X * col + Y] = clamp(((at(X - s, Y - s) + at(X + s, Y + s) + at(X - s, Y + s) + at(X + s, Y - s) + 2) >> 2) + rnd(0, span) - amp);
      }
    }
  }
  if (!f1) for (let y = 0; y <= ys; y++) F[xs * col + y] = F[y];
  const xinc = xs / (xs - f1 + 1), yinc = 1;
  return (x, y) => {
    const fx = xinc * (x + 0.5), fy = yinc * (y + 0.5), ix = Math.trunc(fx), iy = Math.trunc(fy), dx = fx - ix, dy = fy - iy;
    let v = (1 - dx) * (1 - dy) * at(ix, iy) + 0.0;
    v += (1 - dy) * dx * at(ix + 1, iy);
    v += (1 - dx) * dy * at(ix, iy + 1);
    v += at(ix + 1, iy + 1) * dy * dx;
    const i = Math.min(255, Math.max(0, Math.trunc(v)));
    return flags & 2 ? (i * 100) >> 8 : i;
  };
}

// ------------------------------------------------------------------ spline (rivers)

/** Spline type 3 ("General B-Spline (Basis)"), clamped uniform knots, Cox-de Boor with the exe's guards. */
export function generateBSpline(P, degree, segs) {
  const n = P.length;
  if (n <= degree) degree = n - 1;
  if (segs < degree) segs = degree;
  const knots = [];
  let k = 0;
  for (let i = 0; i <= n + degree; i++) { if (i > degree && i <= n) k += 1; knots.push(k); }
  const m = knots.length, last = knots[m - 1];
  const basis = (t, i, d) => {
    if (i + d >= m) return 1;
    if (d === 0) return knots[i] <= t && t < knots[i + 1] ? 1 : 0;
    let a = 0;
    const d1 = f32(t - knots[i]), w1 = f32(knots[i + d] - knots[i]);
    if (d1 > 0 && w1 > 0) a = f32(basis(t, i, d - 1) * f32(d1 / w1));
    const d2 = f32(knots[i + d + 1] - t), w2 = f32(knots[i + d + 1] - knots[i + 1]);
    if (d2 > 0 && w2 > 0) a = f32(a + f32(basis(t, i + 1, d - 1) * f32(d2 / w2)));
    return a;
  };
  const step = f32((last - knots[0]) / segs);
  const out = [[...P[0]]];
  let t = f32(knots[0] + step), span = 0;
  while (last > t) {
    let i = span;
    while (i + 1 < m) { if (!(t > knots[i + 1]) && knots[i] < knots[i + 1]) break; i++; }
    span = i;
    if (span > m - 1) break;
    const p = [0, 0, 0];
    for (let j = Math.max(0, span - degree); j <= span; j++) {
      const N = basis(t, j, degree);
      for (let c = 0; c < 3; c++) p[c] = f32(p[c] + f32(P[j][c] * N));
    }
    out.push(p);
    t = f32(t + step);
  }
  if (f32(last - f32(t - step)) > 0.001) out.push([...P[n - 1]]);
  return out;
}

const norm3 = v => { const l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; if (l2 === 0 || l2 === 1) return v; const s = 1 / Math.sqrt(l2); return v.map(c => f32(c * s)); };

/** Spline::build_normals with flag 0x20 (up = +z): side vector (dy, -dx, 0), averaged with the previous segment's. */
export function buildNormals(pts) {
  const normals = [];
  let prev = [0, 0, 0], length = 0;
  for (let j = 1; j < pts.length; j++) {
    const d = [f32(pts[j][0] - pts[j - 1][0]), f32(pts[j][1] - pts[j - 1][1]), f32(pts[j][2] - pts[j - 1][2])];
    length = f32(length + f32(Math.hypot(...d)));
    const n = norm3([d[1], -d[0], 0]);
    normals.push(norm3([prev[0] + n[0], prev[1] + n[1], prev[2] + n[2]]));
    prev = n;
  }
  normals.push(prev);
  return { normals, length };
}

/** Spline::next_at_dist walker (arc length along the evaluated polyline, end snap at 0.33 * step). */
export function splineWalker(pts, normals, length) {
  let acc = 0, target = 0, idx = 0;
  if (pts.length === 0 || length === 0) { acc = -1; target = -1; idx = -1; }
  const seg = i => f32(Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1], pts[i][2] - pts[i - 1][2]));
  return step => {
    if (acc < 0 || idx < 0) return { more: false };
    idx += 1; target = f32(target + step);
    const thr = f32(step * f32(0.33));
    let s = -1;
    while (idx < pts.length && target > acc) { s = seg(idx); acc = f32(acc + s); idx++; }
    idx -= 1;
    if (acc > target && idx < pts.length && !(f32(length - target) < thr)) {
      if (s < 0) s = seg(idx);
      const f = s === 0 ? 0 : f32((acc - target) / s);
      const p = [0, 1, 2].map(c => f32(pts[idx][c] + f32((pts[idx - 1][c] - pts[idx][c]) * f)));
      const n = norm3([0, 1, 2].map(c => f32(normals[idx][c] + (normals[idx - 1][c] - normals[idx][c]) * f)));
      return { more: true, p, n };
    }
    const p = [...pts[idx]], n = [...normals[idx]];
    if (idx + 1 < pts.length && acc < length) return { more: true, p, n };
    acc = length; idx = -1;
    return { more: false, p, n };
  };
}

// ------------------------------------------------------------------ rivers

export const RIVER = {
  maxWidth: 0.75,      // rivers+0x5c "max_width(WCoords)"
  maxDepth: -60,       // rivers+0xfc "max_depth(3Dunits)"
  sectionsPerWcoord: 1.7, // rivers+0x100 "section/wcoord"
  speed: 1.0,          // rivers+0x104 "river speed" (UV animation, not used by the static mesh)
  wakeSpeed: 0.005,    // rivers+0x108 "riv wake speed"
  bankLift: true,      // rivers+0xf8 (byte, 1): adds -0.25 * maxDepth to every trough vertex
};
const PROFILE = [1, 0.75, 0.26, 0, 0, 0, 0.26, 0.75, 1]; // trough cross-section, bank -> centre -> bank

/**
 * One river from its stored chunk-92 polyline (wcoords). Returns the evaluated spline, the sections (27 trough vertices
 * each: three 9-vertex cross-sections at the start, middle and end), the water quads and the texture per section.
 */
export function buildRiver(t, nodes, cfg = RIVER) {
  const P = nodes.map(([wx, wy]) => [f32(wx * WCOORD + WCOORD / 2), f32(wy * WCOORD + WCOORD / 2), 0]);
  let lenW = 0; // RiverOut+4: control polyline length in wcoords
  for (let i = 1; i < P.length; i++) lenW = f32(lenW + f32(f32(Math.sqrt(f32(f32((P[i][1] - P[i - 1][1]) ** 2) + f32((P[i][0] - P[i - 1][0]) ** 2)))) / 768));
  const segs = Math.trunc(f32(lenW * f32(cfg.sectionsPerWcoord))) & 0xffff;
  if (P.length < 2) return { nodes, sections: [], pts: P };
  const pts = generateBSpline(P, 3, segs);
  if (pts.length < 2) return { nodes, sections: [], pts };
  const { normals, length } = buildNormals(pts);
  const next = splineWalker(pts, normals, length);
  const step = f32(768 / f32(cfg.sectionsPerWcoord));
  const frac = makeFractal(50, 50, 2, 2, 0); // River::one_time_init: Fractal::init(50, 50, smooth 2, -, flags 2, seed 0)
  const maxW = f32(cfg.maxWidth);
  let a0 = 0, a4 = 0, prevW = maxW, ocean = false, cc = -1, first = true, index = 0;
  let { p: A, n: nA } = next(0);
  const sections = [];
  for (;;) {
    if (++a0 > 49) { a4++; a0 = 0; if (a4 > 49) a4 = 0; }
    const w0 = prevW;
    let w = ocean ? Math.min(f32(maxW + maxW), f32(w0 * 1.5)) : f32(f32(f32(frac(a0, a4)) * f32(0.005) + 0.5) * maxW);
    prevW = w;
    const r = next(step);
    const B = r.p, nB = r.n;
    let [v0, v1] = index & 1 ? [0.5, 1] : [0, 0.5];
    if (ocean && (cc & 1)) [v0, v1] = index & 1 ? [0, 0.5] : [0.5, 1];
    let flags = r.more ? 0 : 0x544;
    if (ocean && index === cc + 1) flags |= 0x204;
    if (first) { flags |= 0x11; first = false; }
    if (!ocean) {
      const mx = tcoordOf(f32((A[0] + B[0]) * 0.5)), my = tcoordOf(f32((A[1] + B[1]) * 0.5));
      if (isWaterTcoord(t, mx, my)) {
        ocean = true; cc = index;
        if (index & 1) [v0, v1] = [0, 0.5];
        w = f32(w * 1.5); prevW = w;
      }
    } else if (cc + 1 < index) flags |= 7;
    sections.push(buildSection(t, index, A, nA, B, nB, w0, w, v0, v1, flags, cfg));
    A = B; nA = nB; index++;
    if (!r.more) break;
  }
  for (const s of sections) s.texture = cc !== -1 && cc <= s.index ? (cc & 1 ? 'halfdelta' : 'delta') : 'normal';
  return { nodes, pts, normals, length, segs, lenW, sections, oceanSection: cc };
}

/** RiverSection::create_trough_and_water / create_trough_edge / RiverSectionOut::create_water_plane (stored heights). */
function buildSection(t, index, A, nA, B, nB, w0, w1, v0, v1, flags, cfg) {
  const mid = [f32((A[0] + B[0]) * 0.5), f32((A[1] + B[1]) * 0.5), f32((A[2] + B[2]) * 0.5)];
  const nMid = [0, 1, 2].map(c => f32((nA[c] + nB[c]) * 0.5)); // not renormalised: the middle cross-section is narrower on bends
  const midFlag = (flags & 6) === 4 ? (flags & 2) | 0x200 : flags & 2;
  const verts = [
    ...edge(t, A, nA, w0, v0, flags & 1, cfg),
    ...edge(t, mid, nMid, f32((w0 + w1) * 0.5), f32((v0 + v1) * 0.5), midFlag, cfg),
    ...edge(t, B, nB, w1, v1, flags & 4, cfg),
  ];
  const tris = [];
  for (let row = 0; row < 18; row += 9) for (let k = 0; k < 8; k++) tris.push(row + k, row + k + 1, row + k + 9, row + k + 9, row + k + 1, row + k + 10);
  const a0 = flags & 0x10 ? 0 : 255, a1 = flags & 0x40 ? 0 : 255;
  const water = {
    verts: [[verts[1], 0, v0, a0], [verts[7], 1, v0, a0], [verts[19], 0, v1, a1], [verts[25], 1, v1, a1]]
      .map(([v, u, vv, a]) => ({ x: v.x, y: v.y, z: 0, uv1: [u, vv], alpha: a })),
    tris: [0, 1, 2, 2, 1, 3],
  };
  return { index, A, B, w0, w1, v0, v1, flags, verts, tris, water };
}

function edge(t, P, N, width, v, flag, cfg) {
  const s = [0, 1, 2].map(c => f32(f32(f32(f32(width * N[c]) * 2) * 768) * 0.125)); // N * width * 192
  const lift = cfg.bankLift ? f32(f32(cfg.maxDepth * 0.25) * -1) : 0;
  const out = [];
  let x = f32(P[0] - f32(s[0] * 4)), y = f32(P[1] - f32(s[1] * 4));
  const color = flag ? (flag & 0x800 ? 0x88 : 0) : 255;
  const mapW = t.xs * WCOORD;
  for (let k = 0; k < 9; k++) {
    let z = f32(f32(cfg.maxDepth - f32(PROFILE[k] * cfg.maxDepth)) + lift);
    const tx = tcoordOf(x), ty = tcoordOf(y);
    const inMap = tx >= 0 && ty >= 0 && tx < t.txs && ty < t.tys;
    if (inMap) {
      if (isWaterTcoord(t, tx, ty) && z < 0 && Math.abs(k - 4) <= 1) z = f32(f32(f32(z - 30) + z) * 0.5);
      out.push({ x, y, z, uv: [k * 0.125, v], alpha: color, k });
    } else {
      out.push({ x: Math.min(Math.max(x, 0), mapW), y: Math.min(Math.max(y, 0), mapW), z, uv: [k * 0.125, v], alpha: 0, k, offMap: true });
    }
    x = f32(x + s[0]); y = f32(y + s[1]);
  }
  return out;
}

export function buildRivers(t, cfg = RIVER) { return (t.rivers ?? []).map(nodes => buildRiver(t, nodes, cfg)); }

// ------------------------------------------------------------------ land doobers

/** graphics.txt "#<TILESET>_LAND DOOBERS" (else "#LAND DOOBERS"); names always come from "#LAND DOOBERS". */
export function readLandDoobers(install, tilesetName) {
  const txt = fs.readFileSync(path.join(install, 'graphics.txt'), 'latin1').split(/\r?\n/);
  const section = name => {
    const i = txt.findIndex(l => l.trim().toLowerCase() === '#' + name.toLowerCase());
    if (i < 0) return null;
    const rows = [];
    for (let j = i + 1; j < txt.length && txt[j].trim() && !txt[j].startsWith('#') && !txt[j].startsWith(';'); j++) {
      const f = txt[j].split(',').map(s => s.trim());
      rows.push({ name: f[0], file: f[1], rows: parseInt(f[2]), cols: parseInt(f[3]), type: f[4], centering: parseFloat(f[5]), mode: f[6] ?? '' });
    }
    return rows;
  };
  const names = (section('LAND DOOBERS') ?? []).map(r => r.name);
  const rows = section(`${tilesetName}_LAND DOOBERS`) ?? section('LAND DOOBERS');
  return { names, rows };
}

/**
 * Doober::draw + GraphicPieces::draw_doober + ZSpriteAccumMethod::execute for the chunk-73 doobers (kind 0 "bush",
 * kind 1 "rock"). The atlas cell is a hash of the position ("1 Random"), not an RNG. Quad: camera-facing in the
 * (1,-1,0) / +z plane, bottom edge on the ground, 3 world units per texel across and 2*sqrt(3) per texel up.
 */
export function buildDoobers(t, install, tilesetName) {
  const { names, rows } = readLandDoobers(install, tilesetName);
  const KXY = 0.75 * Math.SQRT2, KZ = 2 * Math.sqrt(3); // see spec: half-width = cellW*KXY on x and y, height = cellH*KZ
  const out = [];
  for (const d of t.doobers ?? []) {
    const name = d.kind === 0 ? 'bush' : d.kind === 1 ? 'rock' : null;
    const idx = names.findIndex(n => n.toLowerCase() === name);
    const piece = rows[idx];
    if (!piece) continue;
    const tx = tcoordOf(d.x), ty = tcoordOf(d.y);
    const z = Math.trunc(findDataZ(t, d.x, d.y)); // stored as int (Coord)
    const x = f32(d.x + f32(piece.centering * 192)), y = f32(d.y + f32(piece.centering * 192));
    const random = piece.mode !== ''; // RData flag 8: set for any non-empty 7th field ("1 Random", "Selected")
    const row = random ? Math.trunc(f32(f32(y * f32(13.117)) + f32(x * f32(27.53)))) : 0;
    const col = random ? Math.trunc(Math.abs(f32(f32(x * f32(13.932)) - f32(y * f32(17.57))))) : 0;
    const tex = loadTexture(install, piece.file);
    const cellW = Math.trunc(tex.width / piece.rows), cellH = Math.trunc(tex.height / piece.cols);
    const r = row % piece.rows, c = col % piece.cols;
    const hw = cellW * KXY, h = cellH * KZ;
    const u0 = (cellW * c) / tex.width, u1 = (cellW * c + cellW) / tex.width, v0 = (cellH * r) / tex.height, v1 = (cellH * r + cellH) / tex.height;
    out.push({
      ...d, name, piece, x, y, z, row: r, col: c, hiddenTcoord: [tx, ty],
      verts: [
        { x: x - hw, y: y + hw, z: z + h, uv: [u0, v0] }, { x: x + hw, y: y - hw, z: z + h, uv: [u1, v0] },
        { x: x - hw, y: y + hw, z, uv: [u0, v1] }, { x: x + hw, y: y - hw, z, uv: [u1, v1] },
      ],
      tris: [0, 1, 2, 2, 1, 3],
    });
  }
  return out;
}

// ------------------------------------------------------------------ team colours

/** TeamColors::init defaults (rise.pdb statics, RGB), slot = Leader colour index (Leader+0x6928). Slot 8 = WHITE. */
export const TEAM_COLOR_UNIT = [[255, 0, 0], [34, 82, 255], [255, 0, 255], [0, 208, 0], [255, 255, 0], [0, 255, 255], [255, 255, 255], [255, 105, 0], [255, 255, 255]];
export const TEAM_COLOR_UNIT_TINT = [[127, 47, 47], [47, 47, 159], [127, 63, 127], [63, 127, 63], [127, 127, 63], [63, 127, 127], [191, 191, 191], [127, 95, 47], [255, 255, 255]];
export const TEAM_COLOR_BUILD = [[102, 0, 0], [26, 57, 172], [128, 0, 128], [0, 89, 0], [98, 96, 0], [0, 92, 108], [120, 120, 120], [112, 48, 0], [255, 255, 255]];

/** TeamColor::get_unit: the slot colour, or BLUE/CYAN/WHITE/RED (self/ally/neutral/enemy) with the "relation colours" preference. */
export function unitTeamColor(slot, { relationColors = false, self = false, ally = false, enemy = true } = {}) {
  if (!relationColors) return TEAM_COLOR_UNIT[slot];
  return self ? TEAM_COLOR_UNIT[1] : ally ? TEAM_COLOR_UNIT[5] : !enemy ? TEAM_COLOR_UNIT[6] : TEAM_COLOR_UNIT[0];
}

// ------------------------------------------------------------------ CLI: stats and self-checks

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  const args = process.argv.slice(2);
  const ii = args.indexOf('--install');
  const install = ii >= 0 ? args.splice(ii, 2)[1] : path.join(os.homedir(), 'Games/RiseOfNations');
  const sc = parseScenario(decompress(fs.readFileSync(args[0])));
  const t = sc.terrain, ts = readTileset(install, t.tileset);
  console.log(`${path.basename(args[0])}: ${t.xs}x${t.ys} tileset ${ts.name}`);

  const M = buildMountains(t, install, { tileset: ts });
  for (const s of M.stamps) {
    if (s.error) { console.log(`  mountain @${s.wx},${s.wy}: ${s.error}`); continue; }
    const dz = s.verts.map(v => v.z - v.tz), base = dz.reduce((a, b) => a + b, 0) / dz.length;
    const sd = Math.sqrt(dz.reduce((a, b) => a + (b - base) ** 2, 0) / dz.length);
    console.log(`  mountain @${s.wx},${s.wy} range ${s.range} (${s.template.template.split(/[\\/]/).pop()}, ${s.template.area}): ${s.verts.length} verts, ${s.tris.length / 3} tris, ` +
      `${s.wcoords.length} wcoords (file says ${s.numCoords}${s.coordCheck ? ', ok' : ', MISMATCH'}); stored z - template z = ${base.toFixed(1)} ± ${sd.toFixed(1)}` +
      (s.snowcap ? `; snowcap at tcoord ${s.snowcap.x0},${s.snowcap.y0} (peak ${s.snowcap.peak})` : ''));
  }
  const R = buildRivers(t);
  R.forEach((r, i) => {
    // Check: the stored heights were carved from these troughs at map generation, so the terrain's lowest point across
    // each in-map cross-section should sit at the trough centre (k = 4).
    let n = 0, off = 0, hit = 0;
    for (const s of r.sections) for (let e = 0; e < 3; e++) {
      const row = s.verts.slice(e * 9, e * 9 + 9);
      if (row.some(v => v.offMap)) continue;
      const hs = row.map(v => findDataZ(t, v.x, v.y));
      const kmin = hs.indexOf(Math.min(...hs));
      n++; off += Math.abs(kmin - 4); if (Math.abs(kmin - 4) <= 1) hit++;
    }
    console.log(`  river ${i}: ${r.nodes.length} nodes, len ${r.lenW.toFixed(2)} wcoords, ${r.segs} spline segs -> ${r.pts.length} points, ${r.sections.length} sections, ocean at ${r.oceanSection}; ` +
      `terrain minimum within ±1 of centre on ${hit}/${n} cross-sections (mean |offset| ${(off / n).toFixed(2)} of 8)`);
  });
  const D = buildDoobers(t, install, t.tileset);
  const cells = new Map();
  for (const d of D) { const k = `${d.name}:${d.row},${d.col}`; cells.set(k, (cells.get(k) ?? 0) + 1); }
  console.log(`  doobers: ${D.length} (${D.filter(d => d.name === 'bush').length} bush, ${D.filter(d => d.name === 'rock').length} rock), ${cells.size} distinct atlas cells used`);
}
