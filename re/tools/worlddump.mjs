#!/usr/bin/env node
// Reference implementation of the simulation world grid built from a scenario: WData per wcoord, TData per tcoord,
// mountains, forests (tree tcoords), blocking counters, bad-path halo, gather edges, connectivity regions, goods,
// building footprints and unit occupancy. Spec: docs/world-spec.md. Paraphrases (no game code): World::init,
// ScenarioRead::load_wcoord_chunk3 / load_tcoord_chunk2, Mountains::add_mountain / set_behind_flags / set_edge_flags,
// MountainRange::init (tcoord and wcoord lists), World::compute_all_vals / compute_val, World::adjust_for_blocking,
// World::set_blocked_at, World::set_edge_flags, WorldData::has_gather_access, Terrain::eliminate_dead_mount_spots,
// Regions::find_all / find / set_coastals / sort_regions / rebuild_coords, Good::init, Item::init,
// BuildTypeData::snap_center, Wall::start_me / start, BuildType::mask_me / place_roads, Object::add_to_world.
//
//   node re/tools/worlddump.mjs <file.scx> [--install ~/Games/RiseOfNations] [--png re/previews/<name>] [--scale 4]
//                               [--no-objects] [--seed n] [--json out.json]
//   --seed: game_random at load_terrain_chunk (only WData.val depends on it)
//
// PNGs (one block of --scale pixels per tcoord; at scale 4 one pixel per ucoord): <name>_pass.png (ground passability,
// buildings, roads, unit footprints), <name>_sea.png (ship passability), <name>_regions.png (get_tregion per tcoord).

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseScenario, decompress, parseObjects, png } from './scxdump.mjs';
import { readMountainTemplates, loadTexture, buildRange } from './dressing.mjs';
import { loadRules, startGame } from './rulesdump.mjs';

// ------------------------------------------------------------------ constants (PDB enums)

// TData.mask (unnamed enum MASK_GATHER_EDGE)
export const T = {
  CLIFF: 0x1, MOUNTAIN: 0x2, BUILDING: 0x3, MERGED: 0x3, TEMP_BEHIND: 0x4, PERM_BEHIND: 0x8, ROAD: 0x10, WATER: 0x20, TREE: 0x30,
  STARTED2: 0x40, STARTED: 0x80, CITY: 0x100, RESOURCE: 0x200, COASTAL: 0x400, RIVER_B: 0x800, GATHERED: 0x1000,
  BAD_PATH: 0x2000, BLOCKED: 0x4000, GATHER_EDGE: 0x8000,
};
// WData.flags (unnamed enum FLAG_GOODY)
export const F = {
  RESOURCE: 0x1, DEAD_BUILD: 0x2, RIVER_VALLEY: 0x2, COAST: 0x4, ROCK: 0x8, MOUNTAIN: 0x10, FOREST: 0x20, CLIFF: 0x40, ROAD: 0x80,
  HALFLAND: 0x100, NEAR_BLOCKING: 0x200, ORIGINAL_COAST: 0x400, OIL: 0x800, RIVER: 0x1000, RIVERBED: 0x2000, BUILDING: 0x4000,
  GOODY: 0x8000,
};
// compass.obj tables: index 0 = self, 1..8 = NW N NE E SE S SW W, 9..24 = the 5x5 ring
export const MOVE_X = [0, -1, 0, 1, 1, 1, 0, -1, -1, -1, 0, 1, 2, 2, 2, 1, 0, -1, -2, -2, -2, -2, 2, 2, -2];
export const MOVE_Y = [0, -1, -1, -1, 0, 1, 1, 1, 0, -2, -2, -2, -1, 0, 1, 2, 2, 2, 1, 0, -1, -2, -2, 2, 2];
export const ORTHOG_X = [0, 0, 1, 0, -1]; // 1..4 = N E S W
export const ORTHOG_Y = [0, -1, 0, 1, 0];

// Coord -> grid: div_3_table[x >> k] is floor((x >> k) / 3), i.e. floor division also for negative x.
export const wcoordOf = x => Math.floor((x >> 8) / 3);
export const tcoordOf = x => Math.floor((x >> 6) / 3);
export const ucoordOf = x => Math.floor((x >> 4) / 3);

// circle_init @006817f0: integer "circles" (rings r = 0..64) used by many grid scans.
export const CIRCLE = (() => {
  const xs = [], ys = [], radius = new Int32Array(65);
  const dist = (ax, ay) => {
    if (ay < ax) return ax === 0 ? 0 : ay < 60000 ? Math.floor(ay * ay / (ax * 2)) + ax : (ay + ax * 2) >>> 1;
    if (ay === 0) return 0;
    return ax < 60000 ? Math.floor(ax * ax / (ay * 2)) + ay : (ax + ay * 2) >>> 1;
  };
  for (let r = 0; r <= 64; r++) {
    for (let x = -r; x <= r; x++) for (let y = -r; y <= r; y++) if (dist(Math.abs(x), Math.abs(y)) === r) { xs.push(x); ys.push(y); }
    radius[r] = xs.length;
  }
  return { x: Int8Array.from(xs), y: Int8Array.from(ys), radius };
})();

// ------------------------------------------------------------------ the grid

export class World {
  // World::init @006b76f0
  constructor(xs, ys) {
    this.xs = xs; this.ys = ys; this.size = xs * ys;
    this.fog_xs = (xs * 8) >> 2; this.fog_ys = (ys * 8) >> 2;
    this.tile_xs = xs * 4; this.tile_ys = ys * 4; this.tile_size = this.tile_xs * this.tile_ys;
    this.reg_xs = (xs * 4) >> 3; this.reg_ys = (ys * 4) >> 3;
    const n = this.size;
    // WData::WData @006b2310 defaults
    this.flags = new Uint16Array(n); this.land = new Int8Array(n).fill(2); this.landSub = new Uint8Array(n);
    this.region = new Int16Array(n); this.region2 = new Int16Array(n); this.down = new Int16Array(n).fill(-1);
    this.downWho = new Int16Array(n); this.val = new Uint8Array(n); this.goods = new Uint8Array(n); this.light = new Uint8Array(n);
    this.who = new Int8Array(n).fill(-1); this.who2 = new Int8Array(n).fill(-1);
    this.blocked = new Uint8Array(n); this.bad = new Uint8Array(n); this.solid = new Int8Array(n); this.wasSeen = new Uint8Array(n);
    this.tdata = new Uint16Array(this.tile_size);
    this.rngDraws = 0;
  }
  wi(wx, wy) { return wy * this.xs + wx; }
  ti(tx, ty) { return ty * this.tile_xs + tx; }
  wOf(tx, ty) { return (ty >> 2) * this.xs + (tx >> 2); }
  inW(wx, wy) { return wx >= 0 && wy >= 0 && wx < this.xs && wy < this.ys; }
  inT(tx, ty) { return tx >= 0 && ty >= 0 && tx < this.tile_xs && ty < this.tile_ys; }

  // WorldData::is_ocean @006b4830: land 1/2 and not half water
  isOcean(i) { return (this.flags[i] & F.HALFLAND) === 0 && (this.land[i] === 1 || this.land[i] === 2); }
  isTOcean(tx, ty) { return (this.tdata[this.ti(tx, ty)] & 0x30) === T.WATER; }
  isTree(tx, ty) { return (this.tdata[this.ti(tx, ty)] & 0x30) === T.TREE; }

  // WorldData::has_blocked_neighbors @006b2990: any of the 8 neighbours BLOCKED
  hasBlockedNeighbors(tx, ty) {
    for (let k = 1; k <= 8; k++) {
      const x = tx + MOVE_X[k], y = ty + MOVE_Y[k];
      if (this.inT(x, y) && (this.tdata[this.ti(x, y)] & T.BLOCKED)) return true;
    }
    return false;
  }
  setBadPath(ti, wi, on) {
    if (on) { if (!(this.tdata[ti] & T.BAD_PATH)) this.bad[wi]++; this.tdata[ti] |= T.BAD_PATH; }
    else { if (this.tdata[ti] & T.BAD_PATH) this.bad[wi]--; this.tdata[ti] &= ~T.BAD_PATH; }
  }
  // World::set_blocked_at @006b4900
  setBlockedAt(tx, ty, on) {
    const ti = this.ti(tx, ty), wi = this.wOf(tx, ty);
    if (!on) {
      if (this.tdata[ti] & T.BLOCKED) { this.blocked[wi]--; this.solid[wi]--; }
      this.tdata[ti] &= 0x3fff; // also clears GATHER_EDGE (sic: the mask is 0x3fff)
    } else {
      if (!(this.tdata[ti] & T.BLOCKED)) { this.blocked[wi]++; this.solid[wi]++; }
      this.tdata[ti] |= T.BLOCKED;
      this.setBadPath(ti, wi, false);
      this.setRoadAt(tx, ty, false);
    }
    for (let k = 1; k <= 8; k++) {
      const x = tx + MOVE_X[k], y = ty + MOVE_Y[k];
      if (!this.inT(x, y)) continue;
      if (!on) { if (!this.hasBlockedNeighbors(x, y)) this.setBadPath(this.ti(x, y), this.wOf(x, y), false); }
      else this.setBadPath(this.ti(x, y), this.wOf(x, y), true);
    }
    if (!on && this.hasBlockedNeighbors(tx, ty)) this.setBadPath(ti, wi, true);
  }
  // World::set_road_at @006b43b0 (only the "off" path matters at load: no roads exist yet)
  setRoadAt(tx, ty, on) {
    const ti = this.ti(tx, ty), wx = tx >> 2, wy = ty >> 2;
    if (on) { this.tdata[ti] = (this.tdata[ti] & ~0x20) | T.ROAD; this.flags[this.wi(wx, wy)] |= F.ROAD; return; }
    if ((this.tdata[ti] & 0x30) === T.ROAD) this.tdata[ti] &= ~0x30;
    for (let x = wx * 4; x < wx * 4 + 4; x++) for (let y = wy * 4; y < wy * 4 + 4; y++) if ((this.tdata[this.ti(x, y)] & 0x30) === T.ROAD) return;
    this.flags[this.wi(wx, wy)] &= ~F.ROAD;
  }
  // World::set_tree_at @006b2060
  setTreeAt(tx, ty, on) {
    const ti = this.ti(tx, ty), wi = this.wOf(tx, ty);
    if (on) { if ((this.tdata[ti] & 0x30) !== T.TREE) this.solid[wi]--; this.tdata[ti] |= 0x38; return; }
    if ((this.tdata[ti] & 0x30) === T.TREE) { this.tdata[ti] &= ~0x30; this.solid[wi]++; }
    this.tdata[ti] &= ~T.PERM_BEHIND;
  }
  // WorldData::has_gather_access @006b4e50, mode param_5 = 1 (resource tcoord with an open orthogonal side)
  hasGatherAccess(tx, ty) {
    const m = this.tdata[this.ti(tx, ty)];
    if (!((m & 0x30) === T.TREE || (m & 3) === T.MOUNTAIN || (m & 3) === T.CLIFF)) return false;
    for (let k = 1; k <= 4; k++) {
      const x = tx + ORTHOG_X[k], y = ty + ORTHOG_Y[k];
      if (!this.inT(x, y)) continue;
      const n = this.tdata[this.ti(x, y)];
      if ((n & 0x30) !== T.WATER && !(n & T.BLOCKED)) return true;
    }
    return false;
  }
}

// ------------------------------------------------------------------ load: terrain chunk

// ScenarioRead::load_wcoord_chunk3 @009ae370 (file flag -> WData flag) and load_tcoord_chunk2 @009ae1f0
const WFILE_MAP = [[0x04, F.FOREST], [0x01, F.ROCK], [0x08, F.COAST | F.ORIGINAL_COAST], [0x20, F.RIVER], [0x10, F.OIL], [0x40, F.CLIFF], [0x80, F.HALFLAND]];
export function loadTerrain(t) {
  const w = new World(t.xs, t.ys);
  for (let i = 0; i < w.size; i++) {
    let f = 0;
    for (const [a, b] of WFILE_MAP) if (t.wflags[i] & a) f |= b; // FILE_FLAG_MOUNTAIN (2) is ignored: chunk 69 rebuilds it
    w.flags[i] = f; w.land[i] = t.land[i]; w.landSub[i] = t.landSub[i];
    if (w.land[i] === 1 && w.landSub[i] !== 0) w.landSub[i] = 0; // "Bad WCoord found and fixed"
  }
  if (t.txs !== w.tile_xs || t.tys !== w.tile_ys) throw new Error('tcoord grid size mismatch');
  for (let i = 0; i < w.tile_size; i++) {
    const m = t.tmask[i]; let v = 0;
    if (m & 0x10) v = (v & ~0x30) | T.WATER;
    if (m & 0x20) v |= T.COASTAL;
    if (m & 0x02) v |= T.RIVER_B;
    w.tdata[i] = v;
  }
  return w;
}

// MountainRange::init @008998b0, the two lists Mountains::add_mountain uses (on top of the render mesh in dressing.mjs):
//   tcoords: one per complete 4x4-pixel template cell (= buildRange().cells), offsets relative to the stamp's tcoord corner
//   wcoords: every 16x16-pixel block whose 5x5 corner samples (step 4) are all opaque or at least 16 of 25 are opaque
export function mountainLists(img) {
  const W = img.width, H = img.height, A = (x, y) => img.rgba[(y * W + x) * 4 + 3];
  const ox = (W >> 1) % 16, oy = (H >> 1) % 16, hw = (W >> 1) - ox, hh = (H >> 1) - oy;
  const wcoords = [];
  for (let y0 = oy; y0 + 16 < H; y0 += 16) for (let x0 = ox; x0 + 16 < W; x0 += 16) {
    let all = true, n = 0;
    for (let i = 0; i <= 16; i += 4) for (let j = 0; j <= 16; j += 4) { if (A(x0 + i, y0 + j)) n++; else all = false; }
    if (all || n > 15) wcoords.push([Math.trunc(x0 / 16) - Math.trunc(hw / 16), Math.trunc(y0 / 16) - Math.trunc(hh / 16)]);
  }
  return { wcoords };
}

// Mountains::add_mountain @0089c2e0 (scenario path: all validation radii 0) + set_behind_flags @00898050
export function addMountains(w, t, install) {
  const templates = readMountainTemplates(install), cache = new Map(), placed = [];
  for (const s of t.mountains ?? []) {
    const tpl = templates[s.index];
    if (!tpl) continue;
    if (!cache.has(s.index)) {
      const img = loadTexture(install, tpl.template);
      const r = buildRange(img, tpl.height);
      cache.set(s.index, { cells: r.cells, faceW: r.wcoords.length, wcoords: mountainLists(img).wcoords });
    }
    const r = cache.get(s.index);
    if (r.faceW !== s.numCoords) continue; // load_mountains_chunk rejects the stamp ("invalid number of coords")
    for (const [dx, dy] of r.wcoords) {
      const i = w.wi(s.wx + dx, s.wy + dy), f = w.flags[i];
      w.flags[i] = (f & 0xffc3) | (f & 0x404) | F.MOUNTAIN;
    }
    for (const [dx, dy] of r.cells) {
      const tx = s.wx * 4 + dx, ty = s.wy * 4 + dy;
      w.setBlockedAt(tx, ty, true);
      const ti = w.ti(tx, ty); w.tdata[ti] = (w.tdata[ti] & 0xfffe) | T.MOUNTAIN;
    }
    for (const [dx, dy] of r.cells) for (let k = 1; k <= 3; k++) { // NW, N, NE of each mountain tcoord
      const x = s.wx * 4 + dx + MOVE_X[k], y = s.wy * 4 + dy + MOVE_Y[k];
      if (w.inT(x, y) && (w.tdata[w.ti(x, y)] & 3) !== T.MOUNTAIN) w.tdata[w.ti(x, y)] |= T.PERM_BEHIND;
    }
    placed.push({ ...s, cells: r.cells });
  }
  w.mountains = placed;
}

// Lands table (rules.xml <LANDS>, Lands::init @0067e730): MAKE goods per gather-land index (Types::good_key; none = -1)
// 0 Land: Knowledge, Food; 1 Sandy, 2 Ocean, 3 Coast: -; 4 Forest: Timber; 5 Mountains: Metal; 6 Rocks: -; 7 Oil: Oil; 8 Cliffs: Metal
export const LAND_MAKE = [[3, 0, -1, -1], [-1, -1, -1, -1], [-1, -1, -1, -1], [-1, -1, -1, -1], [1, -1, -1, -1], [4, -1, -1, -1], [-1, -1, -1, -1], [5, -1, -1, -1], [4, -1, -1, -1]];
// WorldData::get_land(WCoord, 1) @006b4730: gather-land index
export function gatherLand(w, i) {
  const f = w.flags[i];
  if (f & F.COAST) return 3;
  if (f & F.FOREST) return 4;
  if (f & (F.MOUNTAIN | F.CLIFF)) return 5;
  if (f & F.ROCK) return f & F.OIL ? 7 : 6;
  if (w.isOcean(i) && (f & F.OIL)) return 7;
  return w.land[i];
}
const tdiv = (a, b) => Math.trunc(a / b);

// World::compute_all_vals @006b3790 -> compute_val @006b0350, x outer, y inner. Open water returns 0 without an RNG draw;
// every other wcoord draws Random::get(game_random, 0, 0xffff) once. regionSize(i) is the size of WData.region's Region,
// which during ScenarioRead::import is 0 (Regions::clear_all ran just before load_terrain_chunk).
export function computeAllVals(w, rng, regionSize = () => 0) {
  for (let x = 0; x < w.xs; x++) for (let y = 0; y < w.ys; y++) {
    const i = w.wi(x, y);
    if (w.isOcean(i)) { w.val[i] = 0; w.goods[i] = 0; continue; }
    const make = [0, 0, 0, 0, 0, 0];
    let count = 0, water = 0;
    for (let c = CIRCLE.radius[2]; c < CIRCLE.radius[4]; c++) { // rings 3 and 4
      const cx = x + CIRCLE.x[c], cy = y + CIRCLE.y[c];
      if (!w.inW(cx, cy)) continue;
      if (w.tdata[w.ti(cx * 4 + 2, cy * 4 + 2)] & T.GATHERED) continue;
      const n = w.wi(cx, cy);
      for (const g of LAND_MAKE[gatherLand(w, n)]) if (g >= 0 && g < 6 && g !== 3) make[g]++;
      if ((w.flags[n] & F.HALFLAND) || w.isOcean(n)) water++; else count++;
    }
    for (let k = 0; k < 25; k++) {
      const nx = x + MOVE_X[k], ny = y + MOVE_Y[k];
      if (!w.inW(nx, ny)) { count -= 4; continue; }
      const n = w.wi(nx, ny), f = w.flags[n];
      if (!(f & F.COAST) && (f & 0x70)) count -= 2;
      if ((f & F.HALFLAND) || w.isOcean(n)) count -= 1;
    }
    let v = tdiv(count, 4) + rng(0, 0xffff) % 10;
    w.rngDraws++;
    if (tdiv(count, 5) < water && water < tdiv(count, 3)) v = tdiv(v * 3, 2);
    if (tdiv(count, 3) <= water) v = tdiv(v, 10);
    if (make[1] > 4) v *= Math.min(tdiv(make[1], 4), 5) + 1;
    if (make[4] !== 0) v = tdiv(v * 3, 2);
    const rs = regionSize(i);
    if (rs < tdiv(w.size, 128)) v = tdiv(v, 2);
    if (rs < tdiv(w.size, 64)) v = tdiv(v * 3, 4);
    if (rs < tdiv(w.size, 16)) v = tdiv(v * 3, 4);
    w.val[i] = Math.min(Math.max(v, 0), 255);
    w.goods[i] = (make[0] > 4 ? 1 : 0) | (make[1] > 4 ? 2 : 0) | (make[2] > 4 ? 4 : 0) | (make[4] > 4 ? 0x10 : 0) | 0x20;
  }
}

// Random::get(lo, hi) on game_random (LCG x*0x19660D + 0x3C6EF35F)
export function gameRandom(seed) {
  let s = seed >>> 0;
  return (lo, hi) => { if (lo === hi) return lo; if (lo > hi) [lo, hi] = [hi, lo]; s = (Math.imul(s, 0x19660d) + 0x3c6ef35f) >>> 0; return lo + Math.floor(((s & 0xffff) * (hi - lo)) / 65536); };
}

// ------------------------------------------------------------------ World::adjust_for_blocking @006b3230

export function adjustForBlocking(w, findRegions) {
  // 1. near-blocking halo and forest tree tcoords (y outer, x inner)
  for (let y = 0; y < w.ys; y++) for (let x = 0; x < w.xs; x++) {
    const i = w.wi(x, y);
    if (!(w.flags[i] & 0x70)) continue;
    for (let k = 0; k <= 8; k++) { const nx = x + MOVE_X[k], ny = y + MOVE_Y[k]; if (w.inW(nx, ny)) w.flags[w.wi(nx, ny)] |= F.NEAR_BLOCKING; }
    if (w.flags[i] & F.FOREST) for (let k = 0; k < 16; k++) {
      const tx = x * 4 + (k & 3), ty = y * 4 + (k >> 2), ti = w.ti(tx, ty);
      w.setBlockedAt(tx, ty, true);
      if ((w.tdata[ti] & 0x30) !== T.TREE) w.solid[i]--;
      w.tdata[ti] |= 0x38;
    }
  }
  // 2. Regions: release those flagged RECALC, then Regions::find_all
  findRegions?.(w, 'adjust_for_blocking');
  // 3. Terrain::eliminate_dead_mount_spots @0084fd90
  eliminateDeadMountSpots(w);
  // 4. map-edge water is bad path; forest wcoords drop the tree column/row that faces open land (y outer, x inner)
  for (let y = 0; y < w.ys; y++) for (let x = 0; x < w.xs; x++) {
    const i = w.wi(x, y);
    if ((x === 0 || y === 0 || x === w.xs - 1 || y === w.ys - 1) && !(w.flags[i] & F.HALFLAND) && (w.land[i] === 1 || w.land[i] === 2)) {
      for (let k = 0; k < 16; k++) {
        const tx = x * 4 + (k & 3), ty = y * 4 + (k >> 2);
        if (tx < 2 || ty < 2 || tx > w.tile_xs - 3 || ty > w.tile_ys - 3) w.setBadPath(w.ti(tx, ty), w.wOf(tx, ty), true);
      }
    }
    if (!(w.flags[i] & F.FOREST)) continue;
    const untree = (tx, ty) => {
      const ti = w.ti(tx, ty);
      if ((w.tdata[ti] & 0x30) === T.TREE) { w.tdata[ti] &= 0xffcf; w.solid[w.wOf(tx, ty)]++; }
      w.tdata[ti] &= 0xfff7;
      if ((w.tdata[ti] & 3) !== T.MOUNTAIN) w.setBlockedAt(tx, ty, false);
    };
    if (x + 1 < w.xs && !(w.flags[w.wi(x + 1, y)] & 0x30)) for (let ty = y * 4; ty < y * 4 + 4; ty++) untree(x * 4 + 3, ty);
    if (y + 1 < w.ys && !(w.flags[w.wi(x, y + 1)] & 0x30)) for (let tx = x * 4; tx < x * 4 + 4; tx++) untree(tx, y * 4 + 3);
  }
  // 5. World::set_edge_flags @006b0240: GATHER_EDGE on mountain, cliff (none in stock files) and tree tcoords
  for (const m of w.mountains ?? []) for (const [dx, dy] of m.cells) {
    const tx = m.wx * 4 + dx, ty = m.wy * 4 + dy;
    if (w.hasGatherAccess(tx, ty)) w.tdata[w.ti(tx, ty)] |= T.GATHER_EDGE;
  }
  for (let y = 0; y < w.ys; y++) for (let x = 0; x < w.xs; x++) {
    if (!(w.flags[w.wi(x, y)] & F.FOREST)) continue;
    for (let k = 0; k < 16; k++) { const tx = x * 4 + (k & 3), ty = y * 4 + (k >> 2); if (w.hasGatherAccess(tx, ty)) w.tdata[w.ti(tx, ty)] |= T.GATHER_EDGE; }
  }
}

// Terrain::eliminate_dead_mount_spots @0084fd90: two passes over mountain wcoords; any not-yet-visited wcoord in their
// 3x3 whose in-map orthogonal neighbours are all forest or mountain becomes a fully blocked mountain wcoord.
export function eliminateDeadMountSpots(w) {
  const visited = new Uint8Array(w.xs * w.xs + 8); // DynamicBitMask sized xs*xs (sic), indexed y*xs + x
  w.deadMountSpots = [];
  for (let pass = 0; pass < 2; pass++) for (let y = 0; y < w.ys; y++) for (let x = 0; x < w.xs; x++) {
    if (!(w.flags[w.wi(x, y)] & F.MOUNTAIN)) continue;
    for (let k = 0; k <= 8; k++) {
      const cx = x + MOVE_X[k], cy = y + MOVE_Y[k];
      if (!w.inW(cx, cy)) continue;
      const bit = w.wi(cx, cy);
      if (visited[bit]) continue;
      let open = false;
      for (let o = 1; o <= 4; o++) {
        const nx = cx + ORTHOG_X[o], ny = cy + ORTHOG_Y[o];
        if (w.inW(nx, ny) && !(w.flags[w.wi(nx, ny)] & 0x30)) { open = true; break; }
      }
      if (!open) {
        w.deadMountSpots.push([cx, cy]);
        for (let t = 0; t < 16; t++) {
          const tx = cx * 4 + (t & 3), ty = cy * 4 + (t >> 2);
          if (w.tdata[w.ti(tx, ty)] & T.BLOCKED) continue;
          w.setBlockedAt(tx, ty, true);
          const i = w.wi(cx, cy); w.flags[i] = (w.flags[i] & 0xffc3) | F.MOUNTAIN; // World::set_land(land, -1, FLAG_MOUNTAIN)
        }
      }
      visited[bit] = 1;
    }
  }
}

// ------------------------------------------------------------------ connectivity regions (regions.obj)

// 128 Region records: 0..63 land (0 unused, 0x3f overflow bucket), 0x40..0x7f sea (0x40 unused, 0x7e overflow).
export class Regions {
  constructor() {
    this.r = Array.from({ length: 128 }, (_, i) => ({ region: i, size: 0, rank: 0, flags: 0, climate: 0, coast: 0n, coastal: 0n, coords: [] }));
    this.sea = 0x40; this.land = 0;
  }
  // Regions::clear_all @00680060 (region2 is not cleared)
  clearAll(w) {
    for (const g of this.r) { g.flags = 0; g.climate = 0; g.size = 0; g.coords = []; }
    w.region.fill(0); this.land = 0; this.sea = 0x40;
  }
}
const isWater = (w, i) => (w.flags[i] & F.HALFLAND) === 0 && (w.land[i] === 1 || w.land[i] === 2);

// Regions::find @00680180: breadth-first, 8-neighbour, same water/land class, only unlabelled (0) cells
function regionFind(w, R, x, y, id) {
  const q = [[x, y]], t = isWater(w, w.wi(x, y));
  R.r[id].size = 0; w.region[w.wi(x, y)] = id;
  for (let out = 0; out < q.length; out++) {
    const [cx, cy] = q[out]; R.r[id].size++;
    for (let k = 1; k <= 8; k++) {
      const nx = cx + MOVE_X[k], ny = cy + MOVE_Y[k];
      if (!w.inW(nx, ny)) continue;
      const n = w.wi(nx, ny);
      if (isWater(w, n) === t && w.region[n] === 0) { w.region[n] = id; q.push([nx, ny]); }
    }
  }
}

// Regions::find_all @0067eff0 (+ set_coastals, sort_regions, rebuild_coords). Overflow handling is reproduced only for
// the sea path that real maps can reach; the land overflow (62+ land bodies) throws.
export function regionsFindAll(w, R) {
  for (let y = 0; y < w.ys; y++) for (let x = 0; x < w.xs; x++) {
    const i = w.wi(x, y);
    if (w.region[i] !== 0) continue;
    const t = isWater(w, i);
    let joined = false;
    for (let k = 1; k <= 8 && !joined; k++) {
      const nx = x + MOVE_X[k], ny = y + MOVE_Y[k];
      if (!w.inW(nx, ny)) continue;
      const n = w.wi(nx, ny);
      if (isWater(w, n) === t && w.region[n] !== 0 && w.region[n] !== 0x7f) { w.region[i] = w.region[n]; R.r[w.region[n]].size++; joined = true; }
    }
    if (joined) continue;
    let id, lo, hi;
    if (!t) { id = 1; while (id < 64 && R.r[id].size !== 0) id++; id = Math.min(Math.max(id, 0), 0x3e); lo = 0; hi = 0x3e; }
    else { R.sea++; id = Math.min(Math.max(R.sea, 0x40), 0x7d); lo = 0x40; hi = 0x7d; }
    regionFind(w, R, x, y, id);
    R.r[id].climate = 0;
    if (id === hi) {
      if (!t) throw new Error('land region overflow (62+ land bodies): not reproduced');
      const S = R.r[id].size; let m = -1, min = S;
      for (let k = lo; k < hi; k++) if (R.r[k].size < min) { m = k; min = R.r[k].size; }
      const relabel = (a, b) => { for (let c = 0; c < w.size; c++) if (w.region[c] === a) w.region[c] = b; };
      if (m >= 0) { R.r[hi + 1].size += R.r[m].size; R.r[m].size = 0; relabel(m, hi + 1); R.r[m].size += R.r[id].size; R.r[id].size = 0; relabel(id, m); }
      else { R.r[hi + 1].size += S; R.r[id].size = 0; relabel(id, hi + 1); }
    }
  }
  // region2 of half-water wcoords: the first 8-neighbour that is open water (x outer, y inner)
  for (let x = 0; x < w.xs; x++) for (let y = 0; y < w.ys; y++) {
    const i = w.wi(x, y);
    if (!(w.flags[i] & F.HALFLAND) || w.region2[i] !== 0) continue;
    for (let k = 1; k <= 8; k++) {
      const nx = x + MOVE_X[k], ny = y + MOVE_Y[k];
      if (!w.inW(nx, ny)) continue;
      const n = w.wi(nx, ny);
      if (!(w.flags[n] & F.HALFLAND) && (w.land[n] === 1 || w.land[n] === 2)) { w.region2[i] = w.region[n]; break; }
    }
  }
  regionsSetCoastals(w, R);
  // sort_regions @0067fb90: stable descending by size; land 0..63, sea 0x40..0x7e
  const rank = (from, to) => [...Array(to - from).keys()].map(k => from + k).sort((a, b) => R.r[b].size - R.r[a].size || a - b).forEach((r, k) => { R.r[r].rank = k; });
  rank(0, 64); rank(0x40, 0x7f);
  // rebuild_coords @0067f800: y-major raster order
  for (const g of R.r) g.coords = [];
  for (let y = 0; y < w.ys; y++) for (let x = 0; x < w.xs; x++) R.r[w.region[w.wi(x, y)]].coords.push([x, y]);
  for (const g of R.r) if (g.coords.length !== g.size) throw new Error(`Region Size mismatch ${g.region}`);
}

// Regions::set_coastals @0067fd70 + Region::finalize_coastals @006805b0
function regionsSetCoastals(w, R) {
  for (const g of R.r) { g.coast = 0n; g.coastal = 0n; }
  for (let y = 0; y < w.ys; y++) for (let x = 0; x < w.xs; x++) {
    const Rr = w.region[w.wi(x, y)];
    if (Rr >= 0x40) continue;
    for (let k = 1; k <= 8; k++) {
      const nx = x + MOVE_X[k], ny = y + MOVE_Y[k];
      if (!w.inW(nx, ny)) continue;
      const s = w.region[w.wi(nx, ny)] - 0x40;
      if (s < 0 || s >= 64) continue;
      R.r[Rr].coast |= 1n << BigInt(s);
      for (let j = 9; j <= 48; j++) {
        const mx = x + MOVE_X48[j], my = y + MOVE_Y48[j];
        if (!w.inW(mx, my)) continue;
        const L = w.region[w.wi(mx, my)];
        if (L < 0x40 && L !== Rr) { R.r[Rr].coastal |= 1n << BigInt(L); R.r[L].coastal |= 1n << BigInt(Rr); }
      }
    }
  }
  const bit = (m, b) => (m >> BigInt(b)) & 1n;
  for (let me = 0; me < 64; me++) {
    const M = R.r[me];
    let i = 0;
    while (i < 64) {
      let restart = -1;
      if (i !== me && bit(M.coastal, i)) {
        for (let j = 0; j < 64; j++) {
          if (j === me || j === i) continue;
          if (!(bit(R.r[i].coastal, j) && !bit(M.coastal, j))) continue;
          for (let b = 0; b < 63; b++) if (bit(M.coast, b) && bit(R.r[i].coast, b) && bit(R.r[j].coast, b)) {
            M.coastal |= 1n << BigInt(j); R.r[j].coastal |= 1n << BigInt(me);
            if (restart < 0 && j <= i) restart = j;
          }
        }
      }
      i = restart >= 0 ? restart : i + 1;
    }
  }
}
// move_x/move_y @00adcaf0/@00adc400, first 49 entries: 0 self, 1..8 ring 1, 9..24 ring 2, 25..48 ring 3 ((-3,-3) then clockwise)
export const MOVE_X48 = [...MOVE_X, -3, -2, -1, 0, 1, 2, 3, 3, 3, 3, 3, 3, 3, 2, 1, 0, -1, -2, -3, -3, -3, -3, -3, -3];
export const MOVE_Y48 = [...MOVE_Y, -3, -3, -3, -3, -3, -3, -3, -2, -1, 0, 1, 2, 3, 3, 3, 3, 3, 3, 3, 2, 1, 0, -1, -2];

// WorldData::get_tregion @006b52e0
export const getTRegion = (w, tx, ty) => {
  const i = w.wOf(tx, ty);
  return (w.flags[i] & F.HALFLAND) && (w.tdata[w.ti(tx, ty)] & 0x30) === T.WATER ? w.region2[i] : w.region[i];
};

// ------------------------------------------------------------------ objects: goods, items, buildings, unit footprints

const OIL_GOOD = 5, VILLAGE = 414, FARM = 417, UNIVERSITY = 420, OILWELL = 421, OILPLATFORM = 422, DOCK = 432;
const is = (o, x) => o.is_list?.v?.includes(x) ?? false;
const odd = v => (v & 1) !== 0;

// ScenarioRead::import order: items (3), goods (4), objects (5: units and buildings in file order), orders, cities (7)
export function placeObjects(w, buf, scn, rules) {
  const o = parseObjects(buf, scn.top);
  // Types::get_type_index: TypeData.type_name (<TYPENAME> for units/buildings, <NAME> otherwise), first match wins
  const byName = (lo, hi) => { const m = new Map(); for (let i = lo; i < hi; i++) { const t = rules.types[i]; const k = (t?.type_name || t?.name)?.toLowerCase(); if (k && !m.has(k)) m.set(k, i); } return m; };
  const goods = byName(0, 50), builds = byName(414, 543), units = byName(50, 414);
  const st = { items: 0, goods: 0, goodsBlocking: 0, builds: 0, cities: 0, resnapped: 0, unknown: [], units: 0, unitCells: 0 };
  // Item::init @006770e0: FLAG_GOODY on the item's wcoord (no blocking)
  for (const it of o.items) { w.flags[w.wi(wcoordOf(it.cx), wcoordOf(it.cy))] |= F.GOODY; st.items++; }
  // Good::init @0066da20: RESOURCE on the x_size*y_size tcoords around the centre (x outer, y inner, no bounds check),
  // BLOCKED too when GoodTypeData.block; FLAG_RESOURCE on the centre wcoord. Oil goods mark nothing.
  for (const g of o.goods) {
    const ti = goods.get(g.type.toLowerCase());
    if (ti === undefined) { st.unknown.push(g.type); continue; }
    st.goods++;
    if (ti === OIL_GOOD) continue;
    const gt = rules.types[ti], x0 = tcoordOf(g.cx) - Math.trunc(gt.x_size / 2), y0 = tcoordOf(g.cy) - Math.trunc(gt.y_size / 2);
    for (let x = x0; x < x0 + gt.x_size; x++) for (let y = y0; y < y0 + gt.y_size; y++) {
      w.tdata[w.ti(x, y)] |= T.RESOURCE;
      if (gt.block) w.setBlockedAt(x, y, true);
    }
    if (gt.block) st.goodsBlocking++;
    w.flags[w.wi(wcoordOf(g.cx), wcoordOf(g.cy))] |= F.RESOURCE;
  }
  // Units: CollBlock occupancy (Object::add_to_world @0064d8c0), guy 0 assumed at the unit position
  w.coll = new Uint8Array(w.tile_xs * 4 * w.tile_ys * 4); // one byte per ucoord (the game: 16x16 bits per wcoord)
  const uxs = w.xs * 16, uys = w.ys * 16;
  for (const u of o.units) {
    if (u.inside >= 0 || (u.unit_masks & 8)) continue;
    const ti = units.get(u.type.toLowerCase()), ut = ti !== undefined ? rules.types[ti] : null;
    if (!ut) { st.unknown.push(u.type); continue; }
    st.units++;
    const r = ut.new_block_radius | 0;
    if (r === 0 || ut.domain === 2) continue;
    const ux = ucoordOf(u.cx), uy = ucoordOf(u.cy), reg = getTRegion(w, ux >> 2, uy >> 2);
    for (let dy = -r; dy <= r; dy++) for (let dx = -r; dx <= r; dx++) {
      const cx = ux + dx, cy = uy + dy;
      if (cx < 0 || cy < 0 || cx >= uxs || cy >= uys) continue;
      if (reg >= 0 && w.region[w.wi(cx >> 4, cy >> 4)] !== reg) continue;
      if (!w.coll[cy * uxs + cx]) st.unitCells++;
      w.coll[cy * uxs + cx] = 1;
    }
  }
  const place = (b, isCity) => {
    const ti = builds.get(b.type.toLowerCase());
    if (ti === undefined) { st.unknown.push(b.type); return; }
    const bt = rules.types[ti];
    const c = snapCenter(w, bt, b.cx, b.cy);
    if (c[0] !== b.cx || c[1] !== b.cy) st.resnapped++;
    buildInit(w, bt, c[0], c[1], b.who);
    if (b.is_active) buildStart(w, bt, c[0], c[1], b.who);
    isCity ? st.cities++ : st.builds++;
  };
  for (const b of o.builds) place(b, false);
  for (const b of o.cities) place(b, true);
  return st;
}

// BuildTypeData::snap_center @00636190 (oil: centre wcoord only; the dock/mine/woodcutter search through blocked_site is
// skipped: at scenario load nothing is explored yet, so it cannot succeed for mines and woodcutters; docks: open question)
export function snapCenter(w, bt, cx, cy) {
  if (is(bt, OILWELL) || is(bt, OILPLATFORM)) {
    const wx = wcoordOf(cx), wy = wcoordOf(cy);
    if (w.inW(wx, wy) && (w.flags[w.wi(wx, wy)] & F.OIL)) { cx = wx * 768 + 384; cy = wy * 768 + 384; }
  }
  return [tcoordOf(cx) * 192 + (odd(bt.x_size) ? 96 : 0), tcoordOf(cy) * 192 + (odd(bt.y_size) ? 96 : 0)];
}
const corner = (bt, cx, cy) => [tcoordOf(cx) - (bt.x_size >> 1), tcoordOf(cy) - (bt.y_size >> 1)];

// Build::init -> Wall::start_me(1) @0063d790: foundation marks (STARTED, or STARTED2 when already started)
export function buildInit(w, bt, cx, cy) {
  const [x0, y0] = corner(bt, cx, cy);
  for (let x = x0; x < x0 + bt.x_size; x++) for (let y = y0; y < y0 + bt.y_size; y++) {
    if (!w.inT(x, y)) continue;
    const i = w.ti(x, y);
    if (!(w.tdata[i] & T.STARTED)) w.tdata[i] |= T.STARTED; else w.tdata[i] |= T.STARTED2;
  }
}

// Wall::start @0063e810 (scenario buildings: Build::activate -> Wall::activate -> start): mask_me(1), was_seen, behind tiles.
// kill_competing_buildings and the road regeneration (PathFinder::find_road) are not reproduced.
export function buildStart(w, bt, cx, cy, who) {
  maskMe(w, bt, cx, cy, true);
  const [x0, y0] = corner(bt, cx, cy);
  for (let x = x0; x < x0 + bt.x_size; x++) for (let y = y0; y < y0 + bt.y_size; y++) if (w.inT(x, y)) w.wasSeen[w.wOf(x, y)] |= 1 << who;
  // mark_behind_tiles @0063d230: TEMP_BEHIND on the diagonal band north-west of the footprint
  for (let k = 1; k <= (bt.behind_height | 0); k++) {
    for (let x = x0 - k; x < x0 - k + bt.x_size; x++) if (w.inT(x, y0 - k)) w.tdata[w.ti(x, y0 - k)] |= T.TEMP_BEHIND;
    for (let y = y0 - k + 1; y < y0 - k + bt.y_size; y++) if (w.inT(x0 - k, y)) w.tdata[w.ti(x0 - k, y)] |= T.TEMP_BEHIND;
  }
}

// BuildTypeData::connects_to_roads @006398c0
const connectsToRoads = bt => !((bt.build_flags & 0x50) && !is(bt, UNIVERSITY));

// BuildType::mask_me @006312a0 (add only; city CITY bits need the city radius and are not reproduced)
export function maskMe(w, bt, cx, cy, add) {
  const [x0, y0] = corner(bt, cx, cy), city = is(bt, VILLAGE), conn = connectsToRoads(bt);
  const ci = w.wi(wcoordOf(cx), wcoordOf(cy));
  if (add) w.flags[ci] |= F.BUILDING; else w.flags[ci] &= ~F.BUILDING;
  for (let x = 0; x < bt.x_size; x++) for (let y = 0; y < bt.y_size; y++) {
    const tx = x0 + x, ty = y0 + y;
    if (!w.inT(tx, ty)) continue;
    const i = w.ti(tx, ty);
    w.tdata[i] &= ~(T.STARTED | T.STARTED2);
    if (add) { w.tdata[i] |= T.BUILDING; if (!city && !conn) w.setRoadAt(tx, ty, false); }
    else if ((w.tdata[i] & 3) === 3) w.tdata[i] &= ~3;
    const v = bt.mask?.data?.[bt.x_size * y + x] ?? 0;
    if (v === 1) w.setBlockedAt(tx, ty, add);
    else if (add) { w.setBlockedAt(tx, ty, false); if (city || conn) w.setRoadAt(tx, ty, true); }
  }
  if (is(bt, DOCK)) for (let x = x0 - 3; x < x0 + bt.x_size + 3; x++) for (let y = y0 - 3; y < y0 + bt.y_size + 3; y++) {
    if (w.inT(x, y) && (w.tdata[w.ti(x, y)] & 0x30) === T.WATER) w.setBadPath(w.ti(x, y), w.wOf(x, y), add);
  }
  // BuildType::place_roads @0063c580: cities and road-connecting buildings ring [x0-1, x0+w-1] x [y0-1, y0+h-1]
  if (add && (city || conn)) {
    const ring = [];
    for (let x = x0 - 1; x <= x0 + bt.x_size - 1; x++) ring.push([x, y0 - 1], [x, y0 + bt.y_size - 1]);
    for (let y = y0; y < y0 + bt.y_size - 1; y++) ring.push([x0 - 1, y], [x0 + bt.x_size - 1, y]);
    for (const [x, y] of ring) {
      if (!w.inT(x, y)) continue;
      const m = w.tdata[w.ti(x, y)];
      if ((m & T.BLOCKED) || (m & 0x30) === T.WATER) continue;
      if (((m & 3) !== 3 && !(m & T.STARTED)) || (x !== x0 - 1 && y !== y0 - 1)) w.setRoadAt(x, y, true);
    }
  }
}

// ------------------------------------------------------------------ CLI

function main() {
  const argv = process.argv.slice(2);
  const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
  const file = argv.find(a => !a.startsWith('--') && !argv[argv.indexOf(a) - 1]?.startsWith('--'));
  if (!file) { console.error('usage: worlddump.mjs <file.scx> [--install dir] [--png prefix] [--scale n] [--no-objects] [--seed n] [--json out]'); process.exit(1); }
  const install = opt('--install', path.join(os.homedir(), 'Games/RiseOfNations'));
  const res = buildWorld(path.isAbsolute(file) || fs.existsSync(file) ? file : path.join(install, file), install, { objects: !argv.includes('--no-objects'), seed: +opt('--seed', 0) });
  printSummary(res);
  const prefix = opt('--png');
  if (prefix) writePngs(res, prefix, +opt('--scale', 4));
  const json = opt('--json');
  if (json) {
    const w = res.w, a = x => Array.from(x);
    fs.writeFileSync(json, JSON.stringify({
      xs: w.xs, ys: w.ys, tile_xs: w.tile_xs, tile_ys: w.tile_ys,
      wdata: { flags: a(w.flags), land: a(w.land), land_sub: a(w.landSub), region: a(w.region), region2: a(w.region2), val: a(w.val), goods: a(w.goods),
        blocked: a(w.blocked), bad: a(w.bad), solid: a(w.solid), was_seen: a(w.wasSeen) },
      tdata: a(w.tdata),
      regions: w.regions.r.filter(g => g.size).map(g => ({ region: g.region, size: g.size, rank: g.rank, coast: g.coast.toString(16), coastal: g.coastal.toString(16) })),
    }));
  }
}

export function buildWorld(file, install, { objects = true, seed = 0 } = {}) {
  const buf = decompress(fs.readFileSync(file));
  const scn = parseScenario(buf);
  const t = scn.terrain;
  const w = loadTerrain(t);
  addMountains(w, t, install);
  computeAllVals(w, gameRandom(seed));
  // ScenarioRead::import: Regions::clear_all, World::adjust_for_blocking (runs find_all), then
  // ScenarioEditor::init_terrain: game_random := GameInfo.seed, Regions::clear_all + find_all, compute_all_territory
  const R = new Regions();
  R.clearAll(w);
  adjustForBlocking(w, () => regionsFindAll(w, R));
  R.clearAll(w);
  regionsFindAll(w, R);
  w.regions = R;
  let objects_ = null;
  if (objects) {
    const rules = startGame(loadRules(path.join(install, 'Data')));
    objects_ = placeObjects(w, buf, scn, rules);
  }
  return { file, scn, t, w, objects: objects_ };
}

function hist(a) { const m = new Map(); for (const v of a) m.set(v, (m.get(v) ?? 0) + 1); return [...m].sort((x, y) => x[0] - y[0]); }

export function printSummary({ file, w, objects }) {
  const cnt = bit => { let n = 0; for (const v of w.tdata) if ((v & bit) === bit) n++; return n; };
  const cls = { water: 0, tree: 0, mountain: 0, cliff: 0, building: 0 };
  for (const v of w.tdata) {
    if ((v & 0x30) === 0x20) cls.water++; else if ((v & 0x30) === 0x30) cls.tree++;
    if ((v & 3) === 2) cls.mountain++; else if ((v & 3) === 1) cls.cliff++; else if ((v & 3) === 3) cls.building++;
  }
  console.log(`${path.basename(file)}: ${w.xs}x${w.ys} wcoords, ${w.tile_xs}x${w.tile_ys} tcoords`);
  console.log(`  tcoords: water ${cls.water}, tree ${cls.tree}, mountain ${cls.mountain}, cliff ${cls.cliff}, building ${cls.building}, ` +
    `blocked ${cnt(T.BLOCKED)}, bad_path ${cnt(T.BAD_PATH)}, gather_edge ${cnt(T.GATHER_EDGE)}, perm_behind ${cnt(T.PERM_BEHIND)}`);
  console.log(`  mountain stamps placed: ${w.mountains.length}; dead mountain spots filled: ${w.deadMountSpots.length}; compute_all_vals RNG draws: ${w.rngDraws}`);
  const sumBlocked = w.blocked.reduce((a, b) => a + b, 0), sumBad = w.bad.reduce((a, b) => a + b, 0);
  const R = w.regions;
  if (R) {
    const live = R.r.filter(g => g.size);
    console.log(`  regions: ${live.map(g => `${g.region < 0x40 ? 'L' : 'S'}${g.region.toString(16)}:${g.size}${g.region < 0x40 && g.coast ? ' coast[' + [...Array(64).keys()].filter(b => (g.coast >> BigInt(b)) & 1n).map(b => (0x40 + b).toString(16)) + ']' : ''}`).join(', ')}`);
    let h2 = 0; for (let i = 0; i < w.size; i++) if (w.flags[i] & F.HALFLAND) h2++;
    console.log(`  half-water wcoords: ${h2} (region2 = adjacent sea)`);
  }
  if (objects) console.log(`  objects: ${objects.items} items, ${objects.goods} goods (${objects.goodsBlocking} blocking), ${objects.builds} buildings + ${objects.cities} cities ` +
    `(${objects.resnapped} re-snapped), ${objects.units} units on map covering ${objects.unitCells} ucoords${objects.unknown.length ? `; unknown types: ${[...new Set(objects.unknown)].join(', ')}` : ''}`);
  console.log(`  counters: sum(blocked) ${sumBlocked} (= blocked tcoords ${cnt(T.BLOCKED)}), sum(bad) ${sumBad} (= ${cnt(T.BAD_PATH)}), solid ${JSON.stringify(hist(w.solid))}`);
}

const PALETTE = [[230, 25, 75], [60, 180, 75], [255, 225, 25], [67, 99, 216], [245, 130, 49], [145, 30, 180], [66, 212, 244], [240, 50, 230],
  [191, 239, 69], [250, 190, 212], [70, 153, 144], [220, 190, 255], [154, 99, 36], [255, 250, 200], [128, 0, 0], [170, 255, 195]];
export function writePngs({ w }, prefix, s = 4) {
  const W = w.tile_xs * s, H = w.tile_ys * s;
  const out = (name, colorOf, units = false) => {
    const img = new Uint8Array(W * H * 3);
    for (let ty = 0; ty < w.tile_ys; ty++) for (let tx = 0; tx < w.tile_xs; tx++) {
      const c = colorOf(tx, ty);
      for (let dy = 0; dy < s; dy++) for (let dx = 0; dx < s; dx++) img.set(c, ((ty * s + dy) * W + tx * s + dx) * 3);
    }
    // unit occupancy (CollBlock bits), one pixel per ucoord when s == 4
    if (units && w.coll && s === 4) for (let uy = 0; uy < w.tile_ys * 4; uy++) for (let ux = 0; ux < w.tile_xs * 4; ux++) if (w.coll[uy * w.tile_xs * 4 + ux]) img.set([255, 0, 255], (uy * W + ux) * 3);
    // wcoord grid lines
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if ((x % (4 * s) === 0 || y % (4 * s) === 0)) { const p = (y * W + x) * 3; for (let k = 0; k < 3; k++) img[p + k] = img[p + k] * 0.8; }
    fs.mkdirSync(path.dirname(prefix), { recursive: true });
    fs.writeFileSync(`${prefix}_${name}.png`, png(W, H, img));
  };
  out('pass', (tx, ty) => {
    const m = w.tdata[w.ti(tx, ty)];
    if ((m & 0x30) === T.WATER) return [40, 70, 140];
    if ((m & 0x30) === T.TREE) return m & T.BLOCKED ? [20, 90, 30] : [80, 160, 80];
    if ((m & 3) === T.MOUNTAIN) return [120, 100, 90];
    if ((m & 3) === T.BUILDING) return m & T.BLOCKED ? [200, 60, 60] : [230, 170, 160];
    if (m & T.BLOCKED) return [0, 0, 0];
    if ((m & 0x30) === T.ROAD) return [150, 110, 70];
    if (m & T.BAD_PATH) return [215, 200, 120];
    return [200, 220, 180];
  }, true);
  // ship passability: open water tcoords; BAD_PATH (map edge, next to blocked, docks) and COASTAL are hazards for warships
  out('sea', (tx, ty) => {
    const m = w.tdata[w.ti(tx, ty)];
    if ((m & 0x30) !== T.WATER) return m & T.BLOCKED ? [60, 60, 60] : [150, 150, 130];
    if (m & T.BLOCKED) return [0, 0, 0];
    if (m & T.BAD_PATH) return [120, 140, 200];   // warships: hazard (UnitData::invalid_loc returns 3)
    if (m & T.COASTAL) return [70, 110, 190];     // warships: hazard too; fishing/transport ships may enter
    return [30, 60, 160];
  });
  // regions by tcoord (get_tregion), land regions in saturated colours, seas darkened
  out('regions', (tx, ty) => {
    const r = getTRegion(w, tx, ty), c = PALETTE[r % 16];
    return r >= 0x40 ? c.map(v => v * 0.45) : c;
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) main();
