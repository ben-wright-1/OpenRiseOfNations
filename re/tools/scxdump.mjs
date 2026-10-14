// Decode Rise of Nations scenario files (.scx / .sce): terrain (docs/terrain-spec.md) and objects/players
// (docs/scenario-objects-spec.md).
// Reconstructed from ScenarioRead::import / load_terrain_chunk and ScenarioWrite::save_terrain_chunk
// (riseofnations.exe, scenariofile.obj). Layout: docs/terrain-spec.md.
//
// Usage:
//   node re/tools/scxdump.mjs <file.scx>                 summary + chunk tree
//   node re/tools/scxdump.mjs <file.scx> --png <prefix>  also write land/height/tcoord PNGs (<prefix>_*.png)
//   node re/tools/scxdump.mjs --survey <dir>              parse every .scx/.sce under <dir>, one line each
//   node re/tools/scxdump.mjs --roundtrip <dir>           re-encode every terrain chunk, compare bytes
//   node re/tools/scxdump.mjs --objects <file.scx> [--json out.json] [--install dir]
//                                                         players + objects with resolved graphics (docs/scenario-objects-spec.md)
//   node re/tools/scxdump.mjs --objects-survey <dir>      chunks 3-11 of every file: strict sizes, round trip, name resolution
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import zlib from 'node:zlib';
import { pathToFileURL } from 'node:url';
import { readChunks } from './ronassets.mjs';

// Top-level chunk ids (ScenarioRead::import switch).
export const TOP = { 2: 'terrain', 3: 'items', 4: 'goods', 5: 'objects', 6: 'orders', 7: 'cities', 8: 'triggers', 9: 'signposts', 10: 'types', 11: 'leaders' };
// Header chunk ids (the first child; its id doubles as the header version).
export const HEADER_IDS = [1, 0x50, 0x52, 0x66, 0x67, 0x6c];
// Children of the terrain chunk (ScenarioRead::load_terrain_chunk switch).
export const TERRAIN = {
  12: 'wcoord(v1)', 14: 'tcoord(v1)', 67: 'wcoord2', 68: 'tcoord2', 69: 'mountains', 73: 'doobers', 81: 'tileset',
  88: 'wcoord3', 89: 'heights', 90: 'sorted_waterline', 91: 'coord_flags', 92: 'rivers', 96: 'cliffs', 99: 'wcoord_flags',
  104: 'height_adjust',
};

const kids = (b, c) => readChunks(b, c.start, c.end);
const find = (list, id) => list.find(c => c.id === id);

export function decompress(raw) {
  return raw[0] === 0x1f && raw[1] === 0x8b ? zlib.gunzipSync(raw) : raw;
}

// LandIndex (WData.land): 0 plains/baseland, 1 sandy, 2 ocean, 3 coast; WData.land_sub: BaseLandSubtype for land 0.
// WCoord file flags (WCoordDataChunk2.flags) -> WData.flags (load_wcoord_chunk3):
export const WFILE = { 4: 'forest', 1: 'rocks', 8: 'coast', 0x20: 'river', 0x10: 'mountains', 0x40: 'flag40', 0x80: 'waterhalf' };
// TCoord file mask (load_tcoord_chunk2) -> TData.mask: 0x10 -> ocean (0x20), 0x20 -> coastal (0x400), 0x02 -> river (0x800).

export function parseScenario(buf) {
  const root = readChunks(buf);
  if (root.length !== 1 || root[0].id !== 0) throw new Error('not a scenario chunk file');
  const top = kids(buf, root[0]);
  const out = { top, header: null, terrain: null };
  const h = top[0];
  if (!HEADER_IDS.includes(h.id)) throw new Error(`unknown header chunk id ${h.id}`);
  out.header = parseHeader(buf, h);
  const tc = find(top, 2);
  if (tc) out.terrain = parseTerrain(buf, tc);
  return out;
}

// Scenario header, ScenHeaderChunk..ScenHeaderChunk6 (ScenarioRead::import). Byte offsets into the chunk payload.
const HEADER_SETTINGS = ['team_style', 'game_rules', 'map_style', 'map_size', 'map_seed', 'scenario_flags', 'game_speed', 'start_tech',
  'end_tech', 'reveal_map', 'elimination', 'victory', 'score_goal', 'time_limit', 'wonderwin', 'popwin', 'econwin', 'rush_rules']; // i32 at 0xd0..0x114
export function parseHeader(buf, h) {
  const i = off => buf.readInt32LE(h.start + off), size = h.end - h.start;
  const r = { version: h.id, size, num_chunks: i(0), magic: i(4) >>> 0, name: wstr(buf, h.start + 8, h.start + 0xd0) };
  HEADER_SETTINGS.forEach((k, n) => { r[k] = i(0xd0 + 4 * n); });
  if (h.id === 0x6c) { // 0x180: per-player cameras
    r.camera_init_x = [...Array(8)].map((_, k) => i(0x118 + 4 * k));
    r.camera_init_y = [...Array(8)].map((_, k) => i(0x138 + 4 * k));
    r.camera_init_zoom = [...Array(8)].map((_, k) => i(0x158 + 4 * k));
    r.ai_sea_map = i(0x178); r.pop_limit = i(0x17c);
  } else if (h.id >= 0x52) { // 0x124 / 0x128 / 0x12c: one camera for every player
    r.camera_init_x = Array(8).fill(i(0x118)); r.camera_init_y = Array(8).fill(i(0x11c)); r.camera_init_zoom = Array(8).fill(i(0x120));
    if (h.id >= 0x66 && size >= 0x128) r.ai_sea_map = i(0x124); // the two shipped 0x66 headers are only 0x124 long
    if (h.id >= 0x67 && size >= 0x12c) r.pop_limit = i(0x128);
  }
  return r;
}

function wstr(b, p, end) {
  let s = '';
  for (; p + 1 < end; p += 2) { const c = b.readUInt16LE(p); if (!c) break; s += String.fromCharCode(c); }
  return s;
}

export function parseTerrain(buf, tc) {
  const t = { children: kids(buf, tc) };
  for (const c of t.children) {
    switch (c.id) {
      case 81: t.tileset = wstr(buf, c.start, c.end); break;
      case 88: case 67: { // wcoord3 / wcoord2: {15: count, xs, ys} {16: u16 flags, u8 land, u8 sub per wcoord}
        const sub = kids(buf, c), hd = find(sub, 15), d = find(sub, 16);
        const count = buf.readUInt32LE(hd.start), xs = buf.readUInt32LE(hd.start + 4), ys = buf.readUInt32LE(hd.start + 8);
        if (count !== xs * ys || d.end - d.start !== count * 4) throw new Error('wcoord size mismatch');
        t.xs = xs; t.ys = ys; t.wcoordVersion = c.id;
        t.wflags = new Uint16Array(count); t.land = new Uint8Array(count); t.landSub = new Uint8Array(count);
        for (let i = 0; i < count; i++) {
          const p = d.start + i * 4;
          t.wflags[i] = buf.readUInt16LE(p); t.land[i] = buf[p + 2]; t.landSub[i] = buf[p + 3];
        }
        break;
      }
      case 68: { // tcoord2: {17: count, txs, tys} {18: u16 mask per tcoord}
        const sub = kids(buf, c), hd = find(sub, 17), d = find(sub, 18);
        const count = buf.readUInt32LE(hd.start);
        t.txs = buf.readUInt32LE(hd.start + 4); t.tys = buf.readUInt32LE(hd.start + 8);
        if (count !== t.txs * t.tys || d.end - d.start !== count * 2) throw new Error('tcoord size mismatch');
        t.tmask = new Uint16Array(count);
        for (let i = 0; i < count; i++) t.tmask[i] = buf.readUInt16LE(d.start + i * 2);
        break;
      }
      case 89: { // heights: f32 per terrain vertex, (4*xs+1) x (4*ys+1), row-major
        const n = (c.end - c.start) / 4;
        t.heights = new Float32Array(n);
        for (let i = 0; i < n; i++) t.heights[i] = buf.readFloatLE(c.start + i * 4);
        break;
      }
      case 90: { // sorted waterline: u32 nLists; per list u32 n, u32[n]
        let p = c.start; const n = buf.readUInt32LE(p); p += 4; t.waterlines = [];
        for (let i = 0; i < n; i++) { const k = buf.readUInt32LE(p); p += 4; const a = []; for (let j = 0; j < k; j++, p += 4) a.push(buf.readUInt32LE(p)); t.waterlines.push(a); }
        if (p !== c.end) throw new Error('waterline size mismatch');
        break;
      }
      case 91: { // coord flags: u16 CoordFlags per terrain coord (vertex), in TerrainOut list order
        const n = (c.end - c.start) / 2;
        t.coordFlags = new Uint16Array(n);
        for (let i = 0; i < n; i++) t.coordFlags[i] = buf.readUInt16LE(c.start + i * 2);
        break;
      }
      case 99: { // wcoord flags: {100: count} {101: u16[count]}
        const sub = kids(buf, c), n = buf.readUInt32LE(find(sub, 100).start), d = find(sub, 101);
        t.wcoordFlags = new Uint16Array(n);
        for (let i = 0; i < n; i++) t.wcoordFlags[i] = buf.readUInt16LE(d.start + i * 2);
        break;
      }
      case 104: { // height adjust (ScenarioEditor::height_mods): {105: u32 size} {106: u8[size]} (size = xs*ys when present)
        const sub = kids(buf, c), n = buf.readUInt32LE(find(sub, 105).start), d = find(sub, 106);
        t.heightAdjust = d ? Uint8Array.from(buf.subarray(d.start, d.start + n)) : new Uint8Array(0);
        break;
      }
      case 69: { // mountains: {70: u32 n} {71: n x MountainsDataChunk{u8 wx, u8 wy, u16 pad, i32 index, u8 num_coords, u8 pad[3]}}
        const sub = kids(buf, c), n = buf.readUInt32LE(find(sub, 70).start), d = find(sub, 71);
        t.mountains = [];
        for (let i = 0; i < n; i++) { const p = d.start + i * 12; t.mountains.push({ wx: buf[p], wy: buf[p + 1], index: buf.readInt32LE(p + 4), numCoords: buf[p + 8] }); }
        break;
      }
      case 92: { // rivers: {93: u32 n} then per river {94: u32 num_nodes} followed by num_nodes x {95: i32 wx, i32 wy}
        const sub = kids(buf, c); t.rivers = [];
        let k = 1;
        const n = buf.readUInt32LE(sub[0].start);
        for (let r = 0; r < n; r++) {
          if (sub[k].id !== 94) throw new Error('river chunk order');
          const m = buf.readUInt32LE(sub[k++].start), nodes = [];
          for (let j = 0; j < m; j++, k++) nodes.push([buf.readInt32LE(sub[k].start), buf.readInt32LE(sub[k].start + 4)]);
          t.rivers.push(nodes);
        }
        break;
      }
      case 96: { // cliffs: {97: u32 n} then n x {98: CliffDataChunk{i32 wx, i32 wy, i32 size, i32 elem_num}}
        const sub = kids(buf, c); t.cliffs = [];
        for (const s of sub.slice(1)) t.cliffs.push({ wx: buf.readInt32LE(s.start), wy: buf.readInt32LE(s.start + 4), size: buf.readInt32LE(s.start + 8), elem: buf.readInt32LE(s.start + 12) });
        break;
      }
      case 73: { // doobers: {74: u32 n} {75: n x {i32 x, i32 y (world units), u8 kind (0/1), u8 pad[3]}}
        const sub = kids(buf, c), n = buf.readUInt32LE(find(sub, 74).start), d = find(sub, 75);
        t.doobers = [];
        for (let i = 0; i < n; i++) { const p = d.start + i * 12; t.doobers.push({ x: buf.readInt32LE(p), y: buf.readInt32LE(p + 4), kind: buf[p + 8] }); }
        break;
      }
      default: (t.raw ??= {})[c.id] = c;
    }
  }
  if (t.heights) {
    t.vxs = t.xs * 4 + 1; t.vys = t.ys * 4 + 1;
    if (t.heights.length !== t.vxs * t.vys) throw new Error(`heights ${t.heights.length} != ${t.vxs}x${t.vys}`);
  }
  return t;
}

// ------------------------------------------------------------------ PNG
export function png(w, h, rgb) {
  const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = b => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
    return Buffer.concat([len, td, c]);
  };
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgb.buffer, rgb.byteOffset + y * w * 3, w * 3).copy(raw, y * (w * 3 + 1) + 1);
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

// land 0 = land (tinted by BaseLandSubtype), 1 = shallow/shelf water, 2 = deep ocean (3 = coast, unused in stock files)
const LAND_RGB = [[96, 150, 60], [70, 130, 190], [20, 50, 120], [214, 196, 120]];
const SUB_TINT = [[0, 0, 0], [20, 10, -20], [40, 20, -30], [70, 30, -40]];

// Image rows are drawn with y increasing downward (row 0 at top). Game orientation is not yet pinned down.
export function writePngs(t, prefix, scale = 8) {
  const W = t.xs * scale, H = t.ys * scale, img = new Uint8Array(W * H * 3);
  for (let y = 0; y < t.ys; y++) for (let x = 0; x < t.xs; x++) {
    const i = y * t.xs + x, f = t.wflags[i];
    let c = [...(LAND_RGB[t.land[i]] ?? [255, 0, 255])];
    if (t.land[i] === 0) c = c.map((v, k) => v + (SUB_TINT[t.landSub[i]]?.[k] ?? 0));
    if (t.land[i] === 1 && (f & 0x80)) c = [120, 170, 170]; // half land / half water shoreline
    if (f & 4) c = [20, 90, 20];        // forest
    if (f & 0x10) c = [120, 110, 100];  // mountains
    if (f & 1) c = [150, 150, 150];     // rocks
    if (f & 0x20) c = [90, 160, 230];   // river
    for (let yy = 0; yy < scale; yy++) for (let xx = 0; xx < scale; xx++) {
      const o = ((y * scale + yy) * W + x * scale + xx) * 3;
      const edge = (f & 8) && (yy === 0 || xx === 0); // coast marker
      img[o] = edge ? 255 : c[0]; img[o + 1] = edge ? 255 : c[1]; img[o + 2] = edge ? 255 : c[2];
    }
  }
  fs.writeFileSync(`${prefix}_land.png`, png(W, H, img));
  if (t.heights) {
    let lo = Infinity, hi = -Infinity;
    for (const v of t.heights) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
    const s = 2, w = t.vxs * s, h = t.vys * s, im = new Uint8Array(w * h * 3);
    for (let y = 0; y < t.vys; y++) for (let x = 0; x < t.vxs; x++) {
      const v = t.heights[y * t.vxs + x], g = Math.round(((v - lo) / (hi - lo || 1)) * 255);
      // simple hillshade from the x/y gradient
      const vx = t.heights[y * t.vxs + Math.min(x + 1, t.vxs - 1)] - v, vy = t.heights[Math.min(y + 1, t.vys - 1) * t.vxs + x] - v;
      const sh = Math.max(0, Math.min(255, 128 - (vx + vy) * 2));
      for (let yy = 0; yy < s; yy++) for (let xx = 0; xx < s; xx++) {
        const o = ((y * s + yy) * w + x * s + xx) * 3; im[o] = g; im[o + 1] = Math.round((g + sh) / 2); im[o + 2] = sh;
      }
    }
    fs.writeFileSync(`${prefix}_height.png`, png(w, h, im));
  }
  if (t.tmask) {
    const s = 2, w = t.txs * s, h = t.tys * s, im = new Uint8Array(w * h * 3);
    for (let i = 0; i < t.txs * t.tys; i++) {
      const m = t.tmask[i];
      const c = m & 2 ? [90, 160, 230] : m & 0x10 ? (m & 0x20 ? [70, 130, 190] : [30, 70, 150]) : [96, 150, 60];
      const x = i % t.txs, y = (i / t.txs) | 0;
      for (let yy = 0; yy < s; yy++) for (let xx = 0; xx < s; xx++) { const o = ((y * s + yy) * w + x * s + xx) * 3; im.set(c, o); }
    }
    fs.writeFileSync(`${prefix}_tcoord.png`, png(w, h, im));
  }
}

function hist(arr) { const m = new Map(); for (const v of arr) m.set(v, (m.get(v) ?? 0) + 1); return [...m].sort((a, b) => a[0] - b[0]).map(([k, v]) => `${k}:${v}`).join(' '); }
const hex = v => '0x' + v.toString(16);

function summary(file) {
  const buf = decompress(fs.readFileSync(file));
  const s = parseScenario(buf), t = s.terrain;
  console.log(`${file}\n  header v=${hex(s.header.version)} magic=${hex(s.header.magic)} name="${s.header.name}"`);
  console.log(`  top chunks: ${s.top.map(c => `${c.id}${TOP[c.id] ? '(' + TOP[c.id] + ')' : ''}`).join(' ')}`);
  if (!t) return s;
  console.log(`  terrain children: ${t.children.map(c => `${c.id}(${TERRAIN[c.id] ?? '?'},${c.end - c.start}B)`).join(' ')}`);
  console.log(`  tileset="${t.tileset}" wcoords ${t.xs}x${t.ys} tcoords ${t.txs}x${t.tys} height verts ${t.vxs}x${t.vys}`);
  console.log(`  land: ${hist(t.land)}   landSub: ${hist(t.landSub)}`);
  console.log(`  wflags: ${hist(t.wflags.map ? Array.from(t.wflags) : t.wflags).split(' ').map(x => { const [k, v] = x.split(':'); return hex(+k) + ':' + v; }).join(' ')}`);
  if (t.tmask) console.log(`  tmask: ${hist(Array.from(t.tmask)).split(' ').map(x => { const [k, v] = x.split(':'); return hex(+k) + ':' + v; }).join(' ')}`);
  if (t.heights) { let lo = Infinity, hi = -Infinity; for (const v of t.heights) { lo = Math.min(lo, v); hi = Math.max(hi, v); } console.log(`  heights: min ${lo.toFixed(2)} max ${hi.toFixed(2)}`); }
  if (t.coordFlags) console.log(`  coord flags: ${t.coordFlags.length} entries`);
  if (t.wcoordFlags) console.log(`  wcoord flags: ${t.wcoordFlags.length} entries: ${hist(Array.from(t.wcoordFlags)).slice(0, 200)}`);
  if (t.waterlines) console.log(`  waterlines: ${t.waterlines.length} lists, sizes ${t.waterlines.map(a => a.length).join(',')}`.slice(0, 200));
  if (t.heightAdjust) console.log(`  height adjust: ${t.heightAdjust.length} bytes: ${hist(Array.from(t.heightAdjust)).slice(0, 160)}`);
  console.log(`  mountains: ${t.mountains?.length ?? 0} ${JSON.stringify(t.mountains?.slice(0, 4))}`);
  console.log(`  rivers: ${t.rivers?.length ?? 0} ${(t.rivers ?? []).map(r => r.length + ' nodes ' + JSON.stringify(r.slice(0, 3))).join('; ')}`.slice(0, 300));
  console.log(`  cliffs: ${t.cliffs?.length ?? 0} ${JSON.stringify(t.cliffs?.slice(0, 4))}`);
  console.log(`  doobers: ${t.doobers?.length ?? 0} kinds ${hist((t.doobers ?? []).map(d => d.kind))} ${JSON.stringify(t.doobers?.slice(0, 3))}`);
  return s;
}

function* walkDir(d) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) yield* walkDir(p);
    else if (/\.sc[xe]$/i.test(e.name)) yield p;
  }
}

// ------------------------------------------------------------------ encoder (round-trip proof)
// Re-encode the decoded terrain children and compare with the original bytes: node scxdump.mjs --roundtrip <dir>
function chunk(id, payload, aux = 0) {
  const h = Buffer.alloc(8); h.writeUInt32LE(payload.length + 8, 0); h.writeUInt16LE(id, 4); h.writeUInt16LE(aux, 6);
  return Buffer.concat([h, payload]);
}
const u32 = (...v) => { const b = Buffer.alloc(4 * v.length); v.forEach((x, i) => b.writeUInt32LE(x >>> 0, 4 * i)); return b; };
const i32 = (...v) => { const b = Buffer.alloc(4 * v.length); v.forEach((x, i) => b.writeInt32LE(x, 4 * i)); return b; };

export function encodeTerrainChild(t, id) {
  switch (id) {
    case 88: case 67: {
      const d = Buffer.alloc(t.xs * t.ys * 4);
      for (let i = 0; i < t.xs * t.ys; i++) { d.writeUInt16LE(t.wflags[i], i * 4); d[i * 4 + 2] = t.land[i]; d[i * 4 + 3] = t.landSub[i]; }
      return chunk(id, Buffer.concat([chunk(15, u32(t.xs * t.ys, t.xs, t.ys)), chunk(16, d)]), 2);
    }
    case 68: {
      const d = Buffer.alloc(t.tmask.length * 2); t.tmask.forEach((v, i) => d.writeUInt16LE(v, i * 2));
      return chunk(68, Buffer.concat([chunk(17, u32(t.tmask.length, t.txs, t.tys)), chunk(18, d)]), 2);
    }
    case 89: { const d = Buffer.alloc(t.heights.length * 4); t.heights.forEach((v, i) => d.writeFloatLE(v, i * 4)); return chunk(89, d); }
    case 90: return chunk(90, Buffer.concat([u32(t.waterlines.length), ...t.waterlines.map(a => u32(a.length, ...a))]));
    case 91: { const d = Buffer.alloc(t.coordFlags.length * 2); t.coordFlags.forEach((v, i) => d.writeUInt16LE(v, i * 2)); return chunk(91, d); }
    case 99: { const d = Buffer.alloc(t.wcoordFlags.length * 2); t.wcoordFlags.forEach((v, i) => d.writeUInt16LE(v, i * 2)); return chunk(99, Buffer.concat([chunk(100, u32(t.wcoordFlags.length)), chunk(101, d)]), 2); }
    case 104: return chunk(104, Buffer.concat([chunk(105, u32(t.heightAdjust.length)), ...(t.heightAdjust.length ? [chunk(106, Buffer.from(t.heightAdjust))] : [])]), t.heightAdjust.length ? 2 : 1);
    case 69: {
      const d = Buffer.alloc(t.mountains.length * 12);
      t.mountains.forEach((m, i) => { const p = i * 12; d[p] = m.wx; d[p + 1] = m.wy; d.writeInt32LE(m.index, p + 4); d[p + 8] = m.numCoords; });
      return chunk(69, Buffer.concat([chunk(70, u32(t.mountains.length)), chunk(71, d)]), 2);
    }
    case 92: {
      const parts = [chunk(93, u32(t.rivers.length))];
      for (const r of t.rivers) { parts.push(chunk(94, u32(r.length))); for (const [x, y] of r) parts.push(chunk(95, i32(x, y))); }
      return chunk(92, Buffer.concat(parts), parts.length);
    }
    case 96: return chunk(96, Buffer.concat([chunk(97, u32(t.cliffs.length)), ...t.cliffs.map(c => chunk(98, i32(c.wx, c.wy, c.size, c.elem)))]), 1 + t.cliffs.length);
    case 73: {
      const d = Buffer.alloc(t.doobers.length * 12);
      t.doobers.forEach((o, i) => { d.writeInt32LE(o.x, i * 12); d.writeInt32LE(o.y, i * 12 + 4); d[i * 12 + 8] = o.kind; });
      return chunk(73, Buffer.concat([chunk(74, u32(t.doobers.length)), chunk(75, d)]), 2);
    }
  }
  return null;
}

// Compare ignoring bytes the game never initialises (struct padding); returns count of differing bytes and where.
export function roundtrip(buf, t) {
  const res = {};
  for (const c of t.children) {
    const enc = encodeTerrainChild(t, c.id);
    if (!enc) continue;
    const orig = buf.subarray(c.start - 8, c.end);
    let diff = 0, first = -1;
    if (enc.length !== orig.length) { res[c.id] = `size ${enc.length} != ${orig.length}`; continue; }
    for (let i = 0; i < enc.length; i++) if (enc[i] !== orig[i]) { diff++; if (first < 0) first = i; }
    res[c.id] = diff ? `${diff} bytes differ (first @${first})` : 'ok';
  }
  return res;
}

// ================================================================== objects, players, goods (chunks 3-11)
// Record layouts are the PDB structs (UnitPropertiesChunk4, BuildPropertiesChunk2, CityPropertiesChunk,
// LeaderPropertiesChunk2/3, GoodsDataChunk, ...) read by ScenarioRead::load_*_chunk. Spec: docs/scenario-objects-spec.md.
// A schema is a list of [name, kind, arg]; kinds: i32 u32 i16 i8 u8 pad(n) wstr(nChars) — the strings are fixed
// wchar_t buffers; bytes after the NUL (and pad bytes) are uninitialised junk the game never reads.
const REC = {
  unit: [ // UnitPropertiesChunk4 (0x244), chunk 83 inside 25
    ['who', 'i32'], ['o', 'i32'], ['o_up', 'i32'], ['o_down', 'i32'], ['angle', 'u32'], ['dest_angle', 'u32'],
    ['tolerance', 'i32'], ['unit_masks', 'u32'], ['mana_burn', 'i16'], ['form', 'i8'], ['stance', 'i8'],
    ['move_mode', 'i8'], ['_p0', 'pad', 1], ['type', 'wstr', 256], ['_p1', 'pad', 2], ['cx', 'i32'], ['cy', 'i32'], ['damage', 'i32'],
    ['inside', 'i32'], ['inside_who', 'i32'], ['group', 'i32'], ['group_scan', 'i32']],
  build: [ // BuildPropertiesChunk2 (0x224), chunk 84 inside 26
    ['who', 'i32'], ['o', 'i32'], ['damage', 'i32'], ['gather_x', 'i32'], ['gather_y', 'i32'], ['cx', 'i32'], ['cy', 'i32'],
    ['type', 'wstr', 256], ['is_active', 'i32'], ['ever_seen', 'u8'], ['_p0', 'pad', 3]],
  city: [ // CityPropertiesChunk (0x2f0), chunk 30 inside 27
    ['who', 'i32'], ['o', 'i32'], ['damage', 'i32'], ['gather_x', 'i32'], ['gather_y', 'i32'], ['cx', 'i32'], ['cy', 'i32'],
    ['city_flags', 'i32'], ['founder', 'i32'], ['name', 'wstr', 100], ['type', 'wstr', 256], ['is_active', 'i32']],
  leader2: [ // LeaderPropertiesChunk2 (0xe8), chunk 87
    ['who', 'i32'], ['tribe', 'i32'], ['leader_flags', 'u32'], ['gov', 'i32'], ['city_name_index', 'i32'], ['name', 'wstr', 100],
    ['is_human', 'i32'], ['handicap', 'i32'], ['team_color', 'u8'], ['_p0', 'pad', 3]],
  leader3: [ // LeaderPropertiesChunk3 (0xec), chunk 107
    ['who', 'i32'], ['tribe', 'i32'], ['leader_flags', 'u32'], ['gov', 'i32'], ['city_name_index', 'i32'], ['name', 'wstr', 100],
    ['is_human', 'i32'], ['handicap', 'i32'], ['team_color', 'u8'], ['_p0', 'pad', 3], ['difficulty', 'i32']],
  placed: [['type', 'wstr', 256], ['cx', 'i32'], ['cy', 'i32']], // GoodsDataChunk / ItemsDataChunk / SignpostsDataChunk (0x208)
  name256: [['type', 'wstr', 256]],                                // TechDataChunk2 / BuildQueueDataChunk2 (0x200)
  resource: [['type', 'i32'], ['amount', 'i32']],                // ResourceDataChunk (plain; the game keeps amount ^ 0x8221 in memory)
  diplo: [['who', 'i32'], ['diplo', 'i32'], ['treaty', 'i32']],  // DiploDataChunk
  queueHdr: [['count', 'i32'], ['first_in_progress', 'i32']],    // BuildQueueHeaderChunk
  group: [['who', 'i32'], ['num_units', 'i32'], ['units', 'i32x', 128]], // GroupPropertiesChunk (0x208)
  order: [['on_what', 'i32'], ['on_who', 'i32'], ['cx', 'i32'], ['cy', 'i32']], // OrderDataChunk
};
const KSIZE = { i32: 4, u32: 4, i16: 2, i8: 1, u8: 1 };
export const recSize = s => s.reduce((n, [, k, a]) => n + (k === 'pad' ? a : k === 'wstr' ? 2 * a : k === 'i32x' ? 4 * a : KSIZE[k]), 0);

function decodeRec(b, p, schema) {
  const r = {};
  for (const [name, k, a] of schema) {
    switch (k) {
      case 'i32': r[name] = b.readInt32LE(p); p += 4; break;
      case 'u32': r[name] = b.readUInt32LE(p); p += 4; break;
      case 'i16': r[name] = b.readInt16LE(p); p += 2; break;
      case 'i8': r[name] = b.readInt8(p); p += 1; break;
      case 'u8': r[name] = b[p]; p += 1; break;
      case 'pad': p += a; break;
      case 'wstr': r[name] = wstr(b, p, p + 2 * a); p += 2 * a; break;
      case 'i32x': r[name] = []; for (let i = 0; i < a; i++) r[name].push(b.readInt32LE(p + 4 * i)); p += 4 * a; break;
    }
  }
  return r;
}
// Encode a record; `mask` marks the bytes the encoder defines (fields, string chars and their NUL).
function encodeRec(r, schema) {
  const n = recSize(schema), out = Buffer.alloc(n), mask = new Uint8Array(n);
  let p = 0;
  const def = (len) => { mask.fill(1, p, p + len); p += len; };
  for (const [name, k, a] of schema) {
    const v = r[name];
    switch (k) {
      case 'i32': out.writeInt32LE(v, p); def(4); break;
      case 'u32': out.writeUInt32LE(v >>> 0, p); def(4); break;
      case 'i16': out.writeInt16LE(v, p); def(2); break;
      case 'i8': out.writeInt8(v, p); def(1); break;
      case 'u8': out[p] = v; def(1); break;
      case 'pad': p += a; break;
      case 'wstr': { const q = p; for (let i = 0; i < v.length; i++) out.writeUInt16LE(v.charCodeAt(i), q + 2 * i);
        mask.fill(1, q, q + 2 * Math.min(a, v.length + 1)); p += 2 * a; break; }
      case 'i32x': for (let i = 0; i < a; i++) out.writeInt32LE(v[i], p + 4 * i); def(4 * a); break;
    }
  }
  return { out, mask };
}

const BAM = 2 ** 32;
export const bamToDeg = a => (a >>> 0) * 360 / BAM;

// Parse the chunk-tree helpers with strict size checks; `bad` collects every leftover / unexpected byte range.
function counted(buf, c, hdrId, dataId, schema, bad, where) {
  const sub = kids(buf, c), h = find(sub, hdrId), d = find(sub, dataId);
  if (!h) { bad.push(`${where}: no ${hdrId}`); return []; }
  const n = buf.readInt32LE(h.start), size = recSize(schema), out = [];
  if (h.end - h.start !== 4) bad.push(`${where}/${hdrId}: size ${h.end - h.start}`);
  const len = d ? d.end - d.start : 0;
  if (len !== n * size) bad.push(`${where}/${dataId}: ${len} bytes for ${n} x ${size}`);
  for (let i = 0; i < n && d; i++) out.push(decodeRec(buf, d.start + i * size, schema));
  for (const s of sub) if (s.id !== hdrId && s.id !== dataId) bad.push(`${where}: unexpected child ${s.id}`);
  return out;
}
function single(buf, c, schema, bad, where) {
  if (c.end - c.start !== recSize(schema)) bad.push(`${where}: ${c.end - c.start} bytes != ${recSize(schema)}`);
  return decodeRec(buf, c.start, schema);
}
// Build/city queue chunk 85: {33 BuildQueueHeaderChunk} {35 count x wchar[256] type names}
function queue(buf, c, bad, where) {
  const sub = kids(buf, c), h = find(sub, 33), d = find(sub, 35);
  const hdr = single(buf, h, REC.queueHdr, bad, where + '/33');
  const names = [];
  if ((d.end - d.start) !== hdr.count * 512) bad.push(`${where}/35: ${d.end - d.start} bytes for ${hdr.count}`);
  for (let i = 0; i < hdr.count; i++) names.push(wstr(buf, d.start + i * 512, d.start + (i + 1) * 512));
  return { ...hdr, names };
}

export function parseObjects(buf, top) {
  const bad = [], o = { leaders: [], items: [], goods: [], units: [], builds: [], cities: [], groups: [], signposts: [], typeMods: [], triggersBytes: 0, bad };
  for (const t of top) {
    const where = String(t.id);
    switch (t.id) {
      case 3: o.items = counted(buf, t, 21, 22, REC.placed, bad, where); break;
      case 4: o.goods = counted(buf, t, 23, 24, REC.placed, bad, where); break;
      case 9: o.signposts = counted(buf, t, 54, 55, REC.placed, bad, where); break;
      case 8: o.triggersBytes = t.end - t.start; break; // always empty in stock files (scripts live in .bhs)
      case 5: // objects: 25 {83 unit} | 26 {84 build, 85 queue}
        for (const c of kids(buf, t)) {
          const sub = kids(buf, c);
          if (c.id === 25 && sub.length === 1 && sub[0].id === 83) o.units.push(single(buf, sub[0], REC.unit, bad, '5/25/83'));
          else if (c.id === 26 && sub.length === 2 && sub[0].id === 84 && sub[1].id === 85)
            o.builds.push({ ...single(buf, sub[0], REC.build, bad, '5/26/84'), queue: queue(buf, sub[1], bad, '5/26/85') });
          else bad.push(`5: unexpected child ${c.id} [${sub.map(s => s.id)}]`);
        }
        break;
      case 7: // cities: 27 {30 city, 85 queue}
        for (const c of kids(buf, t)) {
          const sub = kids(buf, c);
          if (c.id === 27 && sub.length === 2 && sub[0].id === 30 && sub[1].id === 85)
            o.cities.push({ ...single(buf, sub[0], REC.city, bad, '7/27/30'), queue: queue(buf, sub[1], bad, '7/27/85') });
          else bad.push(`7: unexpected child ${c.id}`);
        }
        break;
      case 6: // editor groups and their orders: 47 {48 group} {49 {50 n} {51 n x order}}
        for (const c of kids(buf, t)) {
          const sub = kids(buf, c);
          if (c.id !== 47 || sub.length !== 2 || sub[0].id !== 48 || sub[1].id !== 49) { bad.push(`6: unexpected child ${c.id}`); continue; }
          const g = single(buf, sub[0], REC.group, bad, '6/47/48');
          g.units = g.units.slice(0, Math.max(0, g.num_units)); // rest of units[128] is junk
          g.orders = counted(buf, sub[1], 50, 51, REC.order, bad, '6/47/49');
          o.groups.push(g);
        }
        break;
      case 10: // scenario rule overrides: 56 {58 unit-type bak | 59 build-type bak}; name at +0x0c, rest kept raw
        for (const c of kids(buf, t)) for (const s of kids(buf, c))
          o.typeMods.push({ kind: s.id === 58 ? 'unit' : s.id === 59 ? 'build' : String(s.id), type: wstr(buf, s.start + 12, s.end), bytes: s.end - s.start });
        break;
      case 11: // leaders: 36 per player {87|107 props} {38 techs} {76 known types} {44 resources} {41 diplomacy}
        for (const c of kids(buf, t)) {
          if (c.id !== 36) { bad.push(`11: unexpected child ${c.id}`); continue; }
          const L = {};
          for (const s of kids(buf, c)) {
            switch (s.id) {
              case 87: Object.assign(L, single(buf, s, REC.leader2, bad, '11/36/87')); L.version = 87; break;
              case 107: Object.assign(L, single(buf, s, REC.leader3, bad, '11/36/107')); L.version = 107; break;
              case 38: L.techs = counted(buf, s, 39, 86, REC.name256, bad, '11/36/38').map(r => r.type); break;
              case 76: L.types = counted(buf, s, 77, 78, REC.name256, bad, '11/36/76').map(r => r.type); break;
              case 44: L.resources = counted(buf, s, 45, 46, REC.resource, bad, '11/36/44'); break;
              case 41: L.diplo = counted(buf, s, 42, 43, REC.diplo, bad, '11/36/41'); break;
              default: bad.push(`11/36: unexpected child ${s.id}`);
            }
          }
          o.leaders.push(L);
        }
        break;
    }
  }
  return o;
}

// Byte-level proof: re-encode every record of chunks 3-11 from the decoded fields and compare with the file,
// over the bytes the encoder defines (fields + strings up to their NUL). Returns { compared, diff, junk }.
export function roundtripObjects(buf, top) {
  let compared = 0, diff = 0, junk = 0;
  const cmp = (c, rec, schema) => {
    const { out, mask } = encodeRec(rec, schema);
    for (let i = 0; i < out.length; i++) {
      if (!mask[i]) { junk++; continue; }
      compared++; if (out[i] !== buf[c.start + i]) diff++;
    }
  };
  const recs = (c, hdrId, dataId, schema, list, map = x => x) => {
    const sub = kids(buf, c), d = find(sub, dataId), size = recSize(schema);
    list.forEach((r, i) => cmp({ start: d.start + i * size }, map(r), schema));
  };
  const o = parseObjects(buf, top);
  for (const t of top) {
    if (t.id === 3) recs(t, 21, 22, REC.placed, o.items);
    if (t.id === 4) recs(t, 23, 24, REC.placed, o.goods);
    if (t.id === 5) {
      let iu = 0, ib = 0;
      for (const c of kids(buf, t)) {
        const sub = kids(buf, c);
        if (c.id === 25) cmp(sub[0], o.units[iu++], REC.unit);
        else { const b = o.builds[ib++]; cmp(sub[0], b, REC.build); recs(sub[1], 33, 35, REC.name256, b.queue.names.map(type => ({ type }))); }
      }
    }
    if (t.id === 7) kids(buf, t).forEach((c, i) => { const sub = kids(buf, c); cmp(sub[0], o.cities[i], REC.city); });
    if (t.id === 6) kids(buf, t).forEach((c, i) => {
      const sub = kids(buf, c), g = o.groups[i];
      const units = [...g.units, ...Array(128 - g.units.length).fill(0)];
      const { out, mask } = encodeRec({ ...g, units }, REC.group);
      for (let k = 0; k < 8 + 4 * g.units.length; k++) { compared++; if (out[k] !== buf[sub[0].start + k]) diff++; }
      junk += out.length - 8 - 4 * g.units.length;
      recs(sub[1], 50, 51, REC.order, g.orders);
    });
    if (t.id === 11) kids(buf, t).forEach((c, i) => {
      const L = o.leaders[i];
      for (const s of kids(buf, c)) {
        if (s.id === 87) cmp(s, L, REC.leader2);
        if (s.id === 107) cmp(s, L, REC.leader3);
        if (s.id === 38) recs(s, 39, 86, REC.name256, L.techs.map(type => ({ type })));
        if (s.id === 76) recs(s, 77, 78, REC.name256, L.types.map(type => ({ type })));
        if (s.id === 44) recs(s, 45, 46, REC.resource, L.resources);
        if (s.id === 41) recs(s, 42, 43, REC.diplo, L.diplo);
      }
    });
  }
  return { compared, diff, junk, bad: o.bad };
}

// ------------------------------------------------------------------ name resolution (rules XML -> graphics)
// Scenario records name their type by TypeData.type_name, i.e. the <TYPENAME> element of unitrules/buildingrules
// (falling back to <NAME> for techs, resources and items, which have no TYPENAME). Types::get_type_index is a
// linear, case-sensitive, first-match search over every type.
const xmlText = (rec, tag) => { const m = rec.match(new RegExp(`<${tag}>([^<]*)</${tag}>`)); return m ? m[1].trim() : undefined; };
export function loadCatalog(install) {
  const D = path.join(install, 'Data'), cat = new Map(), add = (k, v) => { if (k !== undefined && !cat.has(k)) cat.set(k, v); };
  const recs = (file, tag) => fs.readFileSync(path.join(D, file), 'latin1').split(`<${tag}>`).slice(1).map(s => s.split(`</${tag}>`)[0]);
  for (const r of recs('unitrules.xml', 'UNIT'))
    add(xmlText(r, 'TYPENAME') ?? xmlText(r, 'NAME'), { kind: 'unit', name: xmlText(r, 'NAME'), graph: xmlText(r, 'GRAPH'), domain: xmlText(r, 'DOMAIN'), tribeMask: xmlText(r, 'TRIBE_MASK') });
  for (const r of recs('buildingrules.xml', 'BUILDING'))
    add(xmlText(r, 'TYPENAME') ?? xmlText(r, 'NAME'), { kind: 'build', name: xmlText(r, 'NAME'), graph: xmlText(r, 'GRAPH'), xs: +xmlText(r, 'X_SIZE'), ys: +xmlText(r, 'Y_SIZE') });
  for (const r of recs('techrules.xml', 'TECH')) add(xmlText(r, 'NAME'), { kind: 'tech', name: xmlText(r, 'NAME'), age: xmlText(r, 'AGE') });
  for (const r of recs('resourcerules.xml', 'RESOURCE')) add(xmlText(r, 'NAME'), { kind: 'good', name: xmlText(r, 'NAME'), xs: +xmlText(r, 'X_SIZE'), ys: +xmlText(r, 'Y_SIZE') });
  for (const r of recs('itemrules.xml', 'ITEM')) add(xmlText(r, 'NAME'), { kind: 'item', name: xmlText(r, 'NAME') });
  return cat;
}
// Tribe index -> tribe file (rules.xml <TRIBES>, 24 entries; conquest maps override the file list, see the spec).
export function loadTribes(install, overrideXml) {
  const src = fs.readFileSync(overrideXml ?? path.join(install, 'Data', 'rules.xml'), 'latin1');
  const block = src.split('<TRIBES>')[1].split('</TRIBES>')[0];
  return block.split(/<TRIBE[\s>]/).slice(1).map(s => {
    const file = xmlText(s, 'FILE'), key = xmlText(s, 'KEY');
    const p = overrideXml ? path.join(install, file.replace(/^\.\//, '')) : path.join(install, 'tribes', file);
    const real = fs.existsSync(p) ? p : findCI(p);
    const x = real ? fs.readFileSync(real, 'latin1') : '';
    const style = t => { const v = xmlText(x, t); return v === undefined ? undefined : parseInt(v, 10); };
    return { file, key, path: real, name: (x.match(/<TRIBE name="([^"]*)"/) ?? [])[1], unitStyle: style('UNIT_CONTINENT'), buildStyle: style('BUILD_CONTINENT'), backupStyle: style('BACKUP_BUILD_CONTINENT') };
  });
}
function findCI(p) { // case-insensitive file lookup (the game runs on a case-insensitive file system)
  let cur = path.parse(p).root;
  for (const part of path.relative(cur, p).split(path.sep)) {
    if (!fs.existsSync(cur)) return null;
    const hit = fs.readdirSync(cur).find(e => e.toLowerCase() === part.toLowerCase());
    if (!hit) return null;
    cur = path.join(cur, hit);
  }
  return cur;
}
// Graphics names. Building: get_build_gpiece (buildings-spec §6). Unit: get_unit_gpiece (age bucket 0/3/5, unit style
// from the tribe's UNIT_CONTINENT, fallback to DEFAULT; Guy::update_gpiece passes the unit's object index o, and an odd o
// selects the -FEMALE variant where one exists). FARM is the special <FARM> element, not a <BUILD>.
export const BUILD_STYLES = ['DEFAULT', 'NEUROPE', 'ASIAN', 'KOREAN', 'AMERICAN', 'ARAB', 'IROQUOIS', 'COLONIAL', 'EINDIAN'];
export const UNIT_STYLES = ['DEFAULT', 'ARAB', 'AMERICAN', 'ASIAN', 'NA', 'INDIA'];
export function loadGraphicNames(install) {
  const D = path.join(install, 'Data'), names = f => new Set([...fs.readFileSync(path.join(D, f), 'latin1').matchAll(/<(?:BUILD|UNIT|GOOD) name="([^"]+)"/g)].map(m => m[1].toUpperCase())); // names match case-insensitively
  return { build: names('building_graphics.xml'), unit: names('unit_graphics.xml'), good: names('effects_graphics.xml') };
}
export function buildGraphic(gfx, graph, tribe, age) {
  graph = graph.toUpperCase();
  if (graph === 'FARM') return 'FARM';
  const styles = [...new Set([BUILD_STYLES[tribe?.buildStyle ?? 0], BUILD_STYLES[tribe?.backupStyle ?? 0], 'DEFAULT'])];
  for (const s of styles) for (let a = age; a >= 0; a--) { const n = `${graph}-${s}-AGE${a}`; if (gfx.build.has(n)) return n; }
  return null;
}
export function unitGraphic(gfx, graph, tribe, age, { female = false, packed = false } = {}) {
  graph = graph.toUpperCase();
  const bucket = age < 5 ? Math.floor(age / 3) : 2, ages = [5, 3, 0].filter(a => a <= [0, 3, 5][bucket]);
  const tail = (female ? '-FEMALE' : '') + (packed ? '-PACKED' : '');
  let ustyle = UNIT_STYLES[tribe?.unitStyle ?? 0];
  // Merchants (TypeIndex 0x3d/0x3e/400 special case): six extra pieces keyed by the BUILD style, used at AGE3
  // (Iroquois also at AGE0); a Turkish (ARAB) build style forces the ARAB unit style. 🟡 type set inferred as MERCHANT.
  if (graph === 'MERCHANT') {
    const bs = tribe?.buildStyle ?? 0;
    if (bs === 5) ustyle = 'ARAB';
    if ((bucket === 1 || (bucket === 0 && bs === 6)) && [1, 3, 6, 7, 8].includes(bs)) {
      const n = `MERCHANT-${BUILD_STYLES[bs]}-AGE${bucket === 1 ? 3 : 0}`;
      if (gfx.unit.has(n)) return n;
    }
  }
  for (const s of [...new Set([ustyle, 'DEFAULT'])]) for (const a of ages) {
    for (const t of [tail, packed ? '-PACKED' : '', '']) { const n = `${graph}-${s}-AGE${a}${t}`; if (gfx.unit.has(n)) return n; }
  }
  return null;
}

// Conquest scenarios use their campaign's tribe file list (conquest/CTW_<Campaign>_Map_01.xml <TRIBES>). 🟡 inferred
// from the folder name; the game picks the list when the conquest campaign starts.
export function tribesFileFor(file, install) {
  const m = path.relative(install, path.resolve(file)).split(path.sep);
  if (m[0]?.toLowerCase() !== 'conquest' || m.length < 3) return null;
  const camp = { alexander: 'Alexander', coldwar: 'Cold_War', napoleon: 'Napoleon', newworld: 'New_World' }[m[1].toLowerCase()];
  const p = camp && path.join(install, 'conquest', `CTW_${camp}_Map_01.xml`);
  return p && fs.existsSync(p) ? p : null;
}

// Everything a still-battlefield renderer needs, in world units (x right, y down the map rows, 768 per wcoord).
// angle_deg: facing in degrees, 0 = towards -y, 90 = towards +x (BAM / 2^32 * 360); the unit chad angle is angle_deg + 180.
export function resolveBattlefield(file, install) {
  const buf = decompress(fs.readFileSync(file)), s = parseScenario(buf), o = parseObjects(buf, s.top.slice(1));
  const cat = loadCatalog(install), gfx = loadGraphicNames(install), tribesXml = tribesFileFor(file, install);
  const tribes = loadTribes(install, tribesXml);
  const players = o.leaders.map(L => {
    const t = tribes[L.tribe];
    return { who: L.who, name: L.name, tribe: L.tribe, tribeName: t?.name, buildStyle: BUILD_STYLES[t?.buildStyle ?? 0], backupStyle: BUILD_STYLES[t?.backupStyle ?? 0],
      unitStyle: UNIT_STYLES[t?.unitStyle ?? 0], colour: L.team_color, human: L.is_human !== 0, age: playerAge(L, cat), _t: t };
  });
  const P = new Map(players.map(p => [p.who, p]));
  const objects = [];
  for (const b of [...o.builds.map(x => ({ ...x, kind: 'build' })), ...o.cities.map(x => ({ ...x, kind: 'city' }))]) {
    const e = cat.get(b.type), p = P.get(b.who);
    objects.push({ kind: b.kind, who: b.who, o: b.o, type: b.type, x: b.cx, y: b.cy, size: [e?.xs, e?.ys], graphic: e && buildGraphic(gfx, e.graph, p?._t, p?.age ?? 0),
      complete: b.is_active !== 0, damage: b.damage, ...(b.kind === 'city' ? { cityName: b.name } : {}) });
  }
  for (const u of o.units) {
    const e = cat.get(u.type), p = P.get(u.who);
    const hidden = !e ? 'unknown type' : u.unit_masks & 8 ? 'crew slot' : u.inside >= 0 ? `inside ${u.inside_who}:${u.inside}` : null;
    objects.push({ kind: 'unit', who: u.who, o: u.o, type: u.type, x: u.cx, y: u.cy, angle_deg: +bamToDeg(u.angle).toFixed(3),
      graphic: e ? unitGraphic(gfx, e.graph, p?._t, p?.age ?? 0, { packed: !!(u.unit_masks & 0x10), female: (u.o & 1) === 1 }) : null, domain: e?.domain, damage: u.damage, hidden });
  }
  for (const g of o.goods) objects.push({ kind: 'good', type: g.type, x: g.cx, y: g.cy, graphic: gfx.good.has(g.type.toUpperCase()) ? g.type : null });
  for (const g of o.items) objects.push({ kind: 'item', type: g.type, x: g.cx, y: g.cy, graphic: null });
  players.forEach(p => delete p._t);
  return { file, xs: s.terrain.xs, ys: s.terrain.ys, tribesFrom: tribesXml ?? path.join(install, 'Data', 'rules.xml'), header: s.header, players, objects, bad: o.bad };
}

// Player age 0..7 (0 Ancient .. 7 Information). Leader::gain_tech adds 1 to the (obfuscated) leader age for every age tech
// (Classical Age .. Information Age) in the leader's tech list; on all stock files this equals 1 + the highest <AGE>.
export function playerAge(leader, cat) {
  return (leader.techs ?? []).filter(t => { const e = cat.get(t); return e?.kind === 'tech' && / Age$/.test(e.name); }).length;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const args = process.argv.slice(2);
  if (args[0] === '--survey') {
    let ok = 0, bad = 0;
    for (const f of walkDir(args[1])) {
      try {
        const s = parseScenario(decompress(fs.readFileSync(f))), t = s.terrain;
        const ids = t.children.map(c => c.id).join(',');
        console.log(`${path.relative(args[1], f)}\tv=${hex(s.header.version)}\t${t.xs}x${t.ys}\t${t.tileset}\t[${ids}]\tmnt=${t.mountains.length} riv=${t.rivers.length} cliff=${t.cliffs.length} doob=${t.doobers.length}`);
        ok++;
      } catch (e) { console.log(`${f}\tERROR ${e.message}`); bad++; }
    }
    console.log(`parsed ${ok}, failed ${bad}`);
  } else if (args[0] === '--objects-survey') {
    // Every file: decode chunks 3-11 with strict sizes, re-encode, resolve every type name against the rules XML.
    const install = args[2] ?? args[1], cat = loadCatalog(install);
    const tot = { files: 0, bad: 0, units: 0, builds: 0, cities: 0, goods: 0, items: 0, leaders: 0, groups: 0, signposts: 0, typeMods: 0, compared: 0, diff: 0, junk: 0 };
    const unresolved = new Map(), kinds = new Map(), leaderVer = new Map(), tribes = new Map(), colors = new Map();
    const miss = (n, f) => { const k = n; if (!unresolved.has(k)) unresolved.set(k, []); unresolved.get(k).push(path.relative(args[1], f)); };
    for (const f of walkDir(args[1])) {
      const buf = decompress(fs.readFileSync(f)), s = parseScenario(buf), top = s.top.slice(1);
      const o = parseObjects(buf, top), rt = roundtripObjects(buf, top);
      tot.files++; tot.compared += rt.compared; tot.diff += rt.diff; tot.junk += rt.junk;
      if (o.bad.length) { tot.bad++; console.log(`${path.relative(args[1], f)}: ${o.bad.slice(0, 5).join('; ')}`); }
      for (const k of ['units', 'builds', 'cities', 'goods', 'items', 'leaders', 'groups', 'signposts', 'typeMods']) tot[k] += o[k].length;
      const want = (list, kind) => list.forEach(r => { const e = cat.get(r.type); if (!e) miss(`${kind}:${r.type}`, f); else if (e.kind !== kind) kinds.set(`${kind}->${e.kind}:${r.type}`, (kinds.get(`${kind}->${e.kind}:${r.type}`) ?? 0) + 1); });
      want(o.units, 'unit'); want(o.builds, 'build'); want(o.cities, 'build'); want(o.goods, 'good'); want(o.items, 'item');
      for (const b of [...o.builds, ...o.cities]) b.queue.names.forEach(n => { if (!cat.get(n)) miss(`queue:${n}`, f); });
      for (const L of o.leaders) {
        leaderVer.set(L.version, (leaderVer.get(L.version) ?? 0) + 1); tribes.set(L.tribe, (tribes.get(L.tribe) ?? 0) + 1);
        colors.set(`${L.who}->${L.team_color}`, (colors.get(`${L.who}->${L.team_color}`) ?? 0) + 1);
        (L.techs ?? []).forEach(n => { if (!cat.get(n)) miss(`tech:${n}`, f); });
        (L.types ?? []).forEach(n => { if (!cat.get(n)) miss(`known:${n}`, f); });
      }
    }
    console.log(JSON.stringify(tot));
    console.log(`leader record versions ${JSON.stringify([...leaderVer])}  tribes ${JSON.stringify([...tribes].sort((a, b) => a[0] - b[0]))}`);
    console.log(`who->team_color ${JSON.stringify([...colors].sort())}`);
    console.log(`kind mismatches: ${JSON.stringify([...kinds])}`);
    console.log(`unresolved names: ${unresolved.size}`);
    for (const [k, v] of [...unresolved].sort((a, b) => b[1].length - a[1].length).slice(0, 60)) console.log(`  ${k}  x${v.length}  e.g. ${v[0]}`);
  } else if (args[0] === '--objects') {
    // One file: players, then every object with its resolved graphics name. --json <out> writes the battlefield list.
    const install = args.includes('--install') ? args[args.indexOf('--install') + 1] : path.join(os.homedir(), 'Games/RiseOfNations');
    const bf = resolveBattlefield(args[1], install);
    console.log(`${args[1]}  ${bf.xs}x${bf.ys} wcoords (${bf.xs * 768} world units)  tribes from ${path.relative(install, bf.tribesFrom)}  bad=[${bf.bad.join('; ')}]`);
    for (const p of bf.players)
      console.log(`  player ${p.who} "${p.name}" tribe ${p.tribe} (${p.tribeName}, build style ${p.buildStyle}/${p.backupStyle}, unit style ${p.unitStyle}) colour slot ${p.colour} human ${p.human} age ${p.age}`);
    const hist = new Map();
    for (const x of bf.objects) { const k = `${x.kind.padEnd(5)} ${String(x.who ?? '-').padStart(2)} ${x.type} -> ${x.graphic}${x.hidden ? ' (hidden: ' + x.hidden + ')' : ''}`; hist.set(k, (hist.get(k) ?? 0) + 1); }
    for (const [k, v] of [...hist].sort()) console.log(`  ${v.toString().padStart(4)} x ${k}`);
    const j = args.indexOf('--json');
    if (j >= 0) { fs.writeFileSync(args[j + 1], JSON.stringify(bf, null, 1)); console.log(`  wrote ${args[j + 1]}`); }
  } else if (args[0] === '--roundtrip') {
  const tally = new Map();
  for (const f of walkDir(args[1])) {
    const buf = decompress(fs.readFileSync(f)), s = parseScenario(buf);
    for (const [id, r] of Object.entries(roundtrip(buf, s.terrain))) {
      const k = `${id}:${r === 'ok' ? 'ok' : 'DIFF'}`; tally.set(k, (tally.get(k) ?? 0) + 1);
      if (r !== 'ok' && (tally.get(k) ?? 0) <= 3) console.log(`${path.relative(args[1], f)} chunk ${id}: ${r}`);
    }
  }
  console.log([...tally].sort().map(([k, v]) => `${k}=${v}`).join('  '));

  } else {
    const s = summary(args[0]);
    const i = args.indexOf('--png');
    if (i >= 0 && s.terrain) { writePngs(s.terrain, args[i + 1]); console.log(`  wrote ${args[i + 1]}_*.png`); }
  }
}
