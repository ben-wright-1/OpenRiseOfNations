// Reference implementation of the terrain transition pass (docs/terrain-spec.md, B2 "Transitions").
// Mirrors TerrainOut::mark_nodes / mark_trans_flags / check_for_tendrils / create_transition_renderables,
// check_blocks / check_connects / check_fills / check_for_diagonal, Transitions::init / find_map and
// Fractal::init / get_height, including their quirks (see the spec for the list).
//
//   import { buildTransitions } from './transitions.mjs';
//   const tr = buildTransitions({ xs, ys, land, landSub, numTiles: [4, 1, 1], coastBase: 3, seed, masks });
//   tr.patches: [{ x, y, land, sub, code, orient, quad, mask }]  (one per wcoord at most)
//
// CLI: node re/tools/transitions.mjs <file.scx> [--install dir]  -> per-patch dump (debugging)

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// move_x / move_y [1..8] (exe 0x00adcaf4 / 0x00adc404): NW N NE E SE S SW W, +y = south (down on a top-down view)
export const MOVE_X = [-1, 0, 1, 1, 1, 0, -1, -1];
export const MOVE_Y = [-1, -1, -1, 0, 1, 1, 1, 0];
const REV3 = [0, 4, 2, 6, 1, 5, 3, 7]; // bit_reversal (0x00af1d58), also local_84 in Transitions::init

// ------------------------------------------------------------------ texture_fractal

// Fractal::init(xs+1, ys+1, smooth 0, -, flags 2, seed = WorldData.seed) as called by TerrainOut::refresh_data,
// then Fractal::get_height (flags & 2 -> 0..99). Returns h(wx, wy).
export function textureFractal(worldXs, worldYs, seed) {
  const xs = worldXs + 1, ys = worldYs + 1, col = ys + 1;
  const f = new Uint8Array((xs + 1) * col); // frac[x][y], x in 0..xs, y in 0..ys
  let s = seed >>> 0;
  const rnd = () => { // Random::get(0, 255)
    s = (Math.imul(s, 0x19660d) + 0x3c6ef35f) >>> 0;
    return ((s & 0xffff) * 255) >>> 16;
  };
  for (let x = 0; x < xs; x++) for (let y = 0; y < ys; y++) f[x * col + y] = rnd(); // smooth 0: one random pass
  for (let y = 0; y <= ys; y++) f[xs * col + y] = f[y]; // wrap: column xs = column 0 (row ys stays 0)
  const xinc = xs / (xs + 1), yinc = ys / ys; // flags & 1 == 0
  return (x, y) => {
    const fx = xinc * (x + 0.5), fy = yinc * (y + 0.5);
    const ix = Math.trunc(fx), iy = Math.trunc(fy), dx = fx - ix, dy = fy - iy;
    const a = f[ix * col + iy], b = f[(ix + 1) * col + iy], c = f[ix * col + iy + 1], d = f[(ix + 1) * col + iy + 1];
    let v = (1 - dx) * (1 - dy) * a + 0.0;
    v += (1 - dy) * dx * b;
    v += (1 - dx) * dy * c;
    v += d * (dy * dx);
    const i = Math.min(255, Math.max(0, Math.trunc(v)));
    return (i * 100) >> 8;
  };
}

// ------------------------------------------------------------------ masks (Transitions::init)

// The 8 orientation codes of a mask file. o = 0..3: rotated o×90° clockwise (y down); o = 4..7: mirrored left-right, then
// rotated (o-4)×90° clockwise. Groups are [top, right, bottom, left] = bits 9-11, 6-8, 3-5, 0-2.
export function orientationCodes(c) {
  const a = c & 7, b = (c >> 3) & 7, cc = (c >> 6) & 7, d = (c >> 9) & 7;
  const g = (t, r, bo, l) => (t << 9) | (r << 6) | (bo << 3) | l;
  const [Ra, Rb, Rc, Rd] = [REV3[a], REV3[b], REV3[cc], REV3[d]];
  return [g(d, cc, b, a), g(a, d, cc, b), g(b, a, d, cc), g(cc, b, a, d),
    g(Rd, Ra, Rb, Rc), g(Rc, Rd, Ra, Rb), g(Rb, Rc, Rd, Ra), g(Ra, Rb, Rc, Rd)];
}

// File list as FileSearch returns it for "./terrain art/transitions/*.tga" (NTFS: case-insensitive name order).
export function listMasks(install) {
  const dir = resolveCI(install, 'terrain art/transitions');
  return fs.readdirSync(dir).filter(f => /^[01]+\.tga$/i.test(f))
    .sort((p, q) => p.toLowerCase() < q.toLowerCase() ? -1 : p.toLowerCase() > q.toLowerCase() ? 1 : 0)
    .map(f => ({ file: path.join(dir, f), code: parseInt(f.slice(0, -4), 2) }))
    .filter(m => m.code !== 0xeff) // init skips 111011111111
    .map((m, i) => ({ ...m, index: i, codes: orientationCodes(m.code) }));
}

export function resolveCI(root, rel) {
  let cur = root;
  for (const part of rel.replace(/\\/g, '/').split('/').filter(p => p && p !== '.')) {
    if (part === '..') { cur = path.dirname(cur); continue; }
    const hit = fs.readdirSync(cur).find(e => e.toLowerCase() === part.toLowerCase());
    if (!hit) return null;
    cur = path.join(cur, hit);
  }
  return cur;
}

// ------------------------------------------------------------------ texture order

// TileSet::is_combo_acceptable(l1, s1, l2, s2): 1 = (l1,s1) is the texture directly below (l2,s2) in the chain,
// 2 = same or directly above, 0 = not adjacent in the chain.
export function makeCombo(numTiles, coastBase) {
  return (p1, p2, p3, p4) => {
    if (p1 === p3) {
      if (Math.abs(p4 - p2) < 2) return (p4 <= p2 ? 1 : 0) + 1;
      return 0;
    }
    if (p1 === 0) { if (p3 === 1) return p2 === coastBase ? 1 : 0; }
    else if (p1 === 1 && p3 === 0) return p4 === coastBase ? 2 : 0;
    if (p3 === 2) return 1;
    if (p3 < p1) { if (p4 === numTiles[p3] - 1) return p2 !== 0 ? 0 : 2; }
    else if (p4 === 0) return p2 === numTiles[p1] - 1 ? 1 : 0;
    return 0;
  };
}

// The overlay texture of a patch (create_transition_renderables; = TileSet::get_acceptable_neighbor).
export function lowerTexture(l, s, numTiles, coastBase) {
  if (s !== 0) return [l, s - 1];
  if (l === 0) return [0, 0];
  if (l === 1) return [0, coastBase];
  return [l - 1, numTiles[l - 1] - 1];
}

// ------------------------------------------------------------------ the pass

const f32 = Math.fround;
const pick = (h, n) => { // Transitions::find_map index choice (single precision)
  let v = f32(h * f32(n / 100));
  if (v >= f32(n - 0.5)) v = n - 1;
  return Math.trunc(v);
};

export function buildTransitions({ xs, ys, land, landSub, numTiles, coastBase, seed = 0, masks }) {
  const combo = makeCombo(numTiles, coastBase);
  const frac = textureFractal(xs, ys, seed);
  const inb = (x, y) => x >= 0 && y >= 0 && x < xs && y < ys;
  const L = (x, y) => land[y * xs + x], S = (x, y) => landSub[y * xs + x];

  // Edge node arrays (TerrainOut +0x499c / +0x49a0), same index formulas as the exe (they only agree with the
  // allocation sizes on square maps). Bits: 0-2 fill ("must be own texture"), 3-5 connect, 6-8 block ("must be
  // lower texture"); within a group bit 2 = low-coordinate end, bit 1 = middle, bit 0 = high end.
  const n = (Math.max(xs, ys) + 2) ** 2;
  const EX = new Uint32Array(n), EY = new Uint32Array(n);
  const ix = (x, y) => (ys + 1) * y + x; // vertical edge on the west side of wcoord (x, y)
  const iy = (x, y) => (xs + 1) * x + y; // horizontal edge on the north side of wcoord (x, y)
  const setX = (x, y, v, sh) => { const i = ix(x, y); EX[i] |= v << sh; if (sh === 6) EX[i] &= ~(v << 3); };
  const setY = (x, y, v, sh) => { const i = iy(x, y); EY[i] |= v << sh; if (sh === 6) EY[i] &= ~(v << 3); };

  const list = [], listed = new Uint8Array(xs * ys);

  function tendrils(x, y, l, s) {
    const lw = [];
    for (let k = 0; k < 8; k++) {
      const nx = x + MOVE_X[k], ny = y + MOVE_Y[k];
      lw.push(inb(nx, ny) && combo(L(nx, ny), S(nx, ny), l, s) === 1 ? 1 : 0);
    }
    const [NW, N, NE, E, SE, Sx, SW, W] = lw;
    if (!N) {
      if (!W) {
        if (!NW) { if (inb(x, y)) EX[ix(x, y)] |= 4; if (inb(x, y)) EY[iy(x, y)] |= 4; }
        else { if (inb(x, y)) setX(x, y, 4, 6); if (inb(x, y)) setY(x, y, 4, 6); }
      }
      if (!E) {
        if (!NE) { if (inb(x + 1, y)) EX[ix(x + 1, y)] |= 4; if (inb(x, y)) EY[iy(x, y)] |= 1; }
        else { if (inb(x + 1, y)) setX(x + 1, y, 4, 6); if (inb(x, y)) setY(x, y, 1, 6); }
      }
    }
    if (!Sx) {
      if (!W) {
        if (!SW) { if (inb(x, y)) EX[ix(x, y)] |= 1; if (inb(x, y + 1)) EY[iy(x, y + 1)] |= 4; }
        else { if (inb(x, y)) setX(x, y, 1, 6); if (inb(x, y + 1)) setY(x, y + 1, 4, 6); }
      }
      if (!E) {
        if (!SE) { if (inb(x + 1, y)) EX[ix(x + 1, y)] |= 1; if (inb(x, y + 1)) EY[iy(x, y + 1)] |= 1; }
        else { if (inb(x + 1, y)) setX(x + 1, y, 1, 6); if (inb(x, y + 1)) setY(x, y + 1, 1, 6); }
      }
    }
    let ret = 0;
    if (N && Sx && !W && !E) { // one-wide north-south strip: block the corners of both side edges
      if (inb(x, y)) setX(x, y, 5, 6);
      if (inb(x + 1, y)) setX(x + 1, y, 5, 6);
      ret = 1;
    } else if (W && E && !N && !Sx) {
      if (inb(x, y)) setY(x, y, 5, 6);
      if (inb(x, y + 1)) setY(x, y + 1, 5, 6);
      ret = 1;
    }
    if (N + Sx + E + W === 3) { // peninsula: the open side gets blocked corners, a "fill" middle, connects beyond
      if (!N) {
        if (inb(x, y)) setY(x, y, 5, 6);
        if (inb(x, y)) EY[iy(x, y)] |= 2;
        if (inb(x, y - 1)) EX[ix(x, y - 1)] |= 0x38;
        if (inb(x + 1, y - 1)) EX[ix(x + 1, y - 1)] |= 0x38;
      } else if (!E) {
        if (inb(x + 1, y)) setX(x + 1, y, 5, 6);
        if (inb(x + 1, y)) EX[ix(x + 1, y)] |= 2;
        if (inb(x + 1, y)) EY[iy(x + 1, y)] |= 0x38;
        if (inb(x + 1, y + 1)) EY[iy(x + 1, y + 1)] |= 0x38;
      } else if (!Sx) {
        if (inb(x, y + 1)) setY(x, y + 1, 5, 6);
        if (inb(x, y + 1)) EY[iy(x, y + 1)] |= 2;
        if (inb(x, y + 1)) EX[ix(x, y + 1)] |= 0x38;
        if (inb(x + 1, y + 1)) EX[ix(x + 1, y + 1)] |= 0x38;
      } else {
        if (inb(x, y)) setX(x, y, 5, 6);
        if (inb(x, y)) EX[ix(x, y)] |= 2;
        if (inb(x - 1, y)) EY[iy(x - 1, y)] |= 0x38;
        if (inb(x - 1, y + 1)) EY[iy(x - 1, y + 1)] |= 0x38;
      }
      ret = 1;
    }
    return ret;
  }

  function mark(x, y) { // TerrainOut::mark_trans_flags
    const l = L(x, y), s = S(x, y);
    let notLower = 0, orth = 0, diag = 0;
    for (let k = 0; k < 8; k++) {
      const dx = MOVE_X[k], dy = MOVE_Y[k], nx = x + dx, ny = y + dy;
      if (!inb(nx, ny)) continue;
      const c = combo(L(nx, ny), S(nx, ny), l, s);
      if (Math.abs(dx) + Math.abs(dy) === 2) { if (c === 1) diag++; continue; }
      orth++;
      if (c !== 1) { notLower++; continue; }
      if (listed[y * xs + x]) continue; // add_unique returned -1: only the first lower neighbour gets here
      listed[y * xs + x] = 1; list.push(y * xs + x);
      setX(x, y, 7, 3);
      if (inb(x + 1, y)) setX(x + 1, y, 7, 3);
      setY(x, y, 7, 3);
      if (inb(x, y + 1)) setY(x, y + 1, 7, 3);
      if (tendrils(x, y, l, s) === 0) {
        if (dx && inb(x + (dx === 1 ? 1 : 0), y)) setX(x + (dx === 1 ? 1 : 0), y, 7, 6);
        if (dy && inb(x, y + (dy === 1 ? 1 : 0))) setY(x, y + (dy === 1 ? 1 : 0), 7, 6);
      }
    }
    if (notLower === orth && diag !== 0) { // only diagonal lower neighbours
      if (!listed[y * xs + x]) {
        listed[y * xs + x] = 1; list.push(y * xs + x);
        // The exe writes all four to the edge_x array (the last two look meant for edge_y).
        EX[ix(x, y)] |= 0x38;
        if (inb(x + 1, y)) EX[ix(x + 1, y)] |= 0x38;
        EX[ix(x, y)] |= 0x38;
        if (inb(x, y + 1)) EX[ix(x, y + 1)] |= 0x38;
      }
      tendrils(x, y, l, s);
    }
  }

  const build = (x, y, sh) => { // TerrainOut::build_trans_flags
    const g = v => (v >>> sh) & 7;
    return (((g(EY[iy(x, y)]) << 3 | g(EX[ix(x + 1, y)])) << 3 | REV3[g(EY[iy(x, y + 1)])]) << 3) | REV3[g(EX[ix(x, y)])];
  };
  const SIDES = [[0, -1, 0xe00], [1, 0, 0x1c0], [0, 1, 0x38], [-1, 0, 7]]; // N E S W

  function checkBlocks(x, y, b) {
    const l = L(x, y), s = S(x, y);
    for (const [dx, dy, m] of SIDES) {
      const nx = x + dx, ny = y + dy;
      if (!inb(nx, ny)) continue;
      if ((b & m) === m) { if (combo(l, s, L(nx, ny), S(nx, ny)) === 1) b &= ~m; }
      else if (combo(L(nx, ny), S(nx, ny), l, s) === 1) b |= m;
    }
    if (b & 0x800) b |= 1;
    if (b & 0x200) b |= 0x100;
    if (b & 0x100) b |= 0x200;
    if (b & 0x40) b |= 0x20;
    if (b & 0x20) b |= 0x40;
    if (b & 8) b |= 4;
    if (b & 4) b |= 8;
    if (b & 1) b |= 0x800;
    return b >>> 0;
  }
  function checkConnects(x, y, c, b) {
    const l = L(x, y), s = S(x, y);
    for (const [dx, dy, m] of SIDES) {
      const nx = x + dx, ny = y + dy;
      if (!inb(nx, ny)) continue;
      if ((c & m) === 0) { if (combo(L(nx, ny), S(nx, ny), l, s) === 1) c = (c | ~(b & m)) >>> 0; } // sic: full NOT
      else if (L(nx, ny) !== l || S(nx, ny) !== s) c = (c & ~m) >>> 0;
    }
    return c;
  }
  function checkFills(x, y, f, b) {
    const l = L(x, y), s = S(x, y);
    for (const [dx, dy, m] of SIDES) {
      const nx = x + dx, ny = y + dy;
      if (!inb(nx, ny)) continue;
      if ((f & m) && (L(nx, ny) !== l || S(nx, ny) !== s)) f &= ~m;
      if (b & m) f &= ~(b & m);
    }
    return f >>> 0;
  }
  function checkDiagonal(x, y, b, f) { // only called with b == 0; out-of-map neighbours read as (0, 0)
    const l = L(x, y), s = S(x, y), f0 = f;
    const at = (nx, ny) => inb(nx, ny) ? [L(nx, ny), S(nx, ny)] : [0, 0];
    const [nN, nE, nS, nW] = [at(x, y - 1), at(x + 1, y), at(x, y + 1), at(x - 1, y)];
    const same = (p, q) => p[0] === q[0] && p[1] === q[1], own = [l, s];
    const A = same(nN, nE) && !same(nN, own), B = same(nN, nW) && !same(nN, own);
    const C = same(nS, nE) && !same(nS, own), D = same(nS, nW) && !same(nS, own);
    if (A) { b = 0xfc0; f |= 0xc; }
    if (B) { b = 0xe07; f |= 0x60; }
    if (C) { b = 0x1f8; f |= 0x801; }
    if (D) { b = 0x3f; f |= 0x300; }
    if (A) { if (B && C && D) { b = 0; f = f0; } }
    else if (!B && !C && !D) {
      const low = (nx, ny) => inb(nx, ny) && combo(L(nx, ny), S(nx, ny), l, s) === 1;
      if (low(x - 1, y - 1)) { b = 0x801; f |= 0x1fc; }
      if (low(x + 1, y - 1)) { b = 0x300; f |= 0x3f; }
      if (low(x - 1, y + 1)) { b = 0xc; f |= 0xfc0; }
      if (low(x + 1, y + 1)) { b = 0x60; f |= 0xe07; }
    }
    return [b >>> 0, f >>> 0];
  }
  function findMap(x, y, blocked, connect, required) { // Transitions::find_map
    const cands = [];
    for (const m of masks) {
      const orients = [];
      m.codes.forEach((v, o) => {
        if ((blocked & v) !== 0 || ((v & required) >>> 0) !== required) return;
        if ((connect & 7) && !(v & 7)) return;
        if ((connect & 0x1c0) && !(v & 0x1c0)) return;
        if ((connect & 0xe00) && !(v & 0xe00)) return;
        if ((connect & 0x38) && !(v & 0x38)) return;
        if (!blocked && !connect && !required && v !== 0) return;
        orients.push(o);
      });
      if (orients.length) cands.push({ m, orients });
    }
    if (!cands.length) return null;
    const h = frac(x, y), c = cands[pick(h, cands.length)];
    return { mask: c.m, orient: c.orients[pick(h, c.orients.length)], h, candidates: cands.length };
  }

  for (let y = 0; y < ys; y++) for (let x = 0; x < xs; x++) mark(x, y); // TerrainOut::mark_nodes

  const patches = [];
  for (const i of list) { // TerrainOut::create_transition_renderables
    const x = i % xs, y = (i / xs) | 0, l = L(x, y), s = S(x, y);
    let block = build(x, y, 6), connect = build(x, y, 3), fill = build(x, y, 0);
    block = checkBlocks(x, y, block);
    connect = checkConnects(x, y, connect, block);
    fill = checkFills(x, y, fill, block);
    if (block === 0) [block, fill] = checkDiagonal(x, y, block, fill);
    let req = fill;
    const t = ((req | connect | block) & 0xe00) === 0; if (t) req |= 0x400;
    const r = ((req | connect | block) & 0x1c0) === 0; if (r) req |= 0x80;
    const bo = ((req | connect | block) & 0x38) === 0; if (bo) req |= 0x10;
    const le = ((req | connect | block) & 7) === 0; if (le) req |= 2;
    if (t) { if (le) req |= 0x801; if (r) req |= 0x300; }
    if (bo) { if (le) req |= 9; if (r) req |= 0x30; } // sic: 9 and 0x30, not 0xc and 0x60
    req >>>= 0;
    const hit = findMap(x, y, block, connect, req);
    const [ol, os_] = lowerTexture(l, s, numTiles, coastBase);
    const rec = { x, y, land: l, sub: s, overLand: ol, overSub: os_, block, connect, required: req };
    if (!hit) { rec.missing = true; patches.push(rec); continue; }
    let qf = f32(hit.h * f32(0.04));
    if (qf >= 3.5) qf = 3;
    const quad = Math.trunc(qf);
    Object.assign(rec, { mask: hit.mask, code: hit.mask.codes[hit.orient], orient: hit.orient, quad, h: hit.h, candidates: hit.candidates });
    patches.push(rec);
  }
  return { patches, frac };
}

// Mask UV for a patch-local (u, v) in [0,1]² (u along +x, v along +y), create_transition_renderables +
// rotate_tex_coords: per orientation, then × 0.5 + quadrant offset.
export function maskUV(u, v, orient, quad) {
  let a, b;
  switch (orient) {
    case 0: a = u; b = v; break;
    case 1: a = v; b = 1 - u; break;
    case 2: a = 1 - u; b = 1 - v; break;
    case 3: a = 1 - v; b = u; break;
    case 4: a = 1 - u; b = v; break;
    case 5: a = 1 - v; b = 1 - u; break;
    case 6: a = u; b = 1 - v; break;
    default: a = v; b = u; break;
  }
  return [a * 0.5 + (quad === 1 || quad === 2 ? 0.5 : 0), b * 0.5 + (quad >= 2 ? 0.5 : 0)];
}

// ------------------------------------------------------------------ CLI dump
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const { parseScenario, decompress } = await import('./scxdump.mjs');
  const args = process.argv.slice(2);
  const opt = (k, d) => { const i = args.indexOf('--' + k); return i >= 0 ? args[i + 1] : d; };
  const install = opt('install', path.join(os.homedir(), 'Games/RiseOfNations'));
  const sc = parseScenario(decompress(fs.readFileSync(args[0])));
  const t = sc.terrain;
  const xml = fs.readFileSync(path.join(install, 'Data/tilesets.xml'), 'latin1');
  const ts = [...xml.matchAll(/<TILESET name="([^"]+)"[\s\S]*?<\/TILESET>/g)].find(m => m[1] === t.tileset)[0];
  const count = name => [...(ts.match(new RegExp(`<${name}>([\\s\\S]*?)</${name}>`))?.[1] ?? '').matchAll(/<BASE /g)].length;
  const tr = buildTransitions({
    xs: t.xs, ys: t.ys, land: t.land, landSub: t.landSub, numTiles: [count('BASELAND'), count('COASTAL'), count('OCEAN')],
    coastBase: +ts.match(/<COAST_BASE value="(\d+)"/)[1], seed: +(opt('seed', sc.header.map_seed ?? 0)), masks: listMasks(install),
  });
  const hist = {};
  for (const p of tr.patches) {
    const k = p.missing ? 'none' : path.basename(p.mask.file, '.tga');
    hist[k] = (hist[k] ?? 0) + 1;
    if (args.includes('--all')) console.log(`${p.x},${p.y} ${p.land}/${p.sub} -> ${p.overLand}/${p.overSub} block ${p.block.toString(2).padStart(12, '0')} conn ${(p.connect & 0xfff).toString(2).padStart(12, '0')} req ${p.required.toString(2).padStart(12, '0')} ${p.missing ? 'NO MAP' : `${path.basename(p.mask.file, '.tga')} o${p.orient} q${p.quad} h${p.h} n${p.candidates}`}`);
  }
  console.log(`${tr.patches.length} patches of ${t.xs * t.ys} wcoords;`, hist);
}
