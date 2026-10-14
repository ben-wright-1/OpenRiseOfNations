// Readers for Rise of Nations assets, reconstructed from riseofnations.exe:
//   ChunkRead            -> readChunks
//   BH3Method::load_bh3  -> parseBH3   (mesh + node hierarchy, BH3Method::load_hier)
//   AnimObj::load_hier   -> parseBHA   (keyframed node tree)
//   HierObj::set_to_time -> sampleAnim
// See docs/formats.md for the byte layouts.
//
// Rotation convention: quaternions in the files are the conjugate of the standard (column-vector,
// q * v * q^-1) convention. The parsers below return them already conjugated, so callers can use
// standard quaternion math. Verified visually: re/previews/t_conj.png vs t_normal.png.
import fs from 'node:fs';

/** Parse a chunk tree. Header: u32 size (incl. 8-byte header), u16 id, u16 aux. */
export function readChunks(buf, start = 0, end = buf.length) {
  const out = [];
  let p = start;
  while (p + 8 <= end) {
    const size = buf.readUInt32LE(p);
    if (size < 8 || p + size > end) throw new Error(`bad chunk at 0x${p.toString(16)}`);
    out.push({ id: buf.readUInt16LE(p + 4), aux: buf.readUInt16LE(p + 6), start: p + 8, end: p + size });
    p += size;
  }
  return out;
}

// ---------------------------------------------------------------- BH3

function readCountedFloats(buf, c, width) {
  const n = buf.readUInt32LE(c.start);
  const a = new Float32Array(n * width);
  for (let i = 0; i < n * width; i++) a[i] = buf.readFloatLE(c.start + 4 + i * 4);
  return a;
}

/** Node record (chunk id 7 inside id 6), from BH3Method::load_hier. */
function readBH3Node(buf, c) {
  let p = c.start;
  const first = buf.readUInt32LE(p), count = buf.readUInt32LE(p + 4), nameLen = buf.readUInt32LE(p + 8);
  p += 12;
  const name = buf.toString('latin1', p, p + nameLen).replace(/\0.*$/s, '');
  p += nameLen;
  const f = i => buf.readFloatLE(p + i * 4);
  return { first, count, name, quat: [-f(0), -f(1), -f(2), f(3)], trans: [f(4), f(5), f(6)] };
}

function readBH3Hierarchy(buf, c) {
  const kids = readChunks(buf, c.start, c.end);
  const rec = kids.find(k => k.id === 7);
  const node = rec ? readBH3Node(buf, rec) : { first: 0, count: 0, name: '', quat: [0, 0, 0, 1], trans: [0, 0, 0] };
  node.children = kids.filter(k => k.id === 6).map(k => readBH3Hierarchy(buf, k));
  return node;
}

export function parseBH3(buf) {
  const root = readChunks(buf);
  const top = root.length === 1 && root[0].id === 0 ? readChunks(buf, root[0].start, root[0].end) : root;
  const meshes = [];
  let hierarchy = null;
  for (const c of top) {
    if (c.id === 1) {
      const m = {};
      for (const k of readChunks(buf, c.start, c.end)) {
        if (k.id === 2) m.positions = readCountedFloats(buf, k, 4);
        else if (k.id === 3) m.normals = readCountedFloats(buf, k, 4);
        else if (k.id === 4) m.uvs = readCountedFloats(buf, k, 2);
        else if (k.id === 5) {
          const n = buf.readUInt32LE(k.start);
          m.indices = new Uint16Array(n);
          for (let i = 0; i < n; i++) m.indices[i] = buf.readUInt16LE(k.start + 4 + i * 2);
        }
      }
      meshes.push(m);
    } else if (c.id === 6) {
      hierarchy = readBH3Hierarchy(buf, c);
    }
  }
  return { meshes, hierarchy };
}

// ---------------------------------------------------------------- BHA

/** Key track (chunk id 7 inside id 8), from AnimObj::load_hier: u32 n; n x {dt, quat xyzw, trans xyz, pad}. */
function readTrack(buf, c) {
  const n = buf.readUInt32LE(c.start);
  const keys = [];
  let t = 0;
  for (let i = 0; i < n; i++) {
    const p = c.start + 4 + i * 36;
    const f = j => buf.readFloatLE(p + j * 4);
    t += Math.trunc(f(0) * 1000) & 0xffff; // cumulative milliseconds, as the game stores key_times
    keys.push({ time: t, quat: [-f(1), -f(2), -f(3), f(4)], trans: [f(5), f(6), f(7)] });
  }
  return keys;
}

function readBHANode(buf, c) {
  const kids = readChunks(buf, c.start, c.end);
  const tr = kids.find(k => k.id === 7);
  return { keys: tr ? readTrack(buf, tr) : [], children: kids.filter(k => k.id === 8).map(k => readBHANode(buf, k)) };
}

export function parseBHA(buf) {
  const root = readChunks(buf);
  const top = root.length === 1 && root[0].id === 0 ? readChunks(buf, root[0].start, root[0].end) : root;
  const roots = top.filter(k => k.id === 8).map(k => readBHANode(buf, k));
  let duration = 0;
  const visit = n => { if (n.keys.length) duration = Math.max(duration, n.keys[n.keys.length - 1].time); n.children.forEach(visit); };
  roots.forEach(visit);
  return { roots, duration };
}

// ---------------------------------------------------------------- math

export const quatMul = (a, b) => [
  a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
  a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
  a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
];
export function quatRotate(q, v) {
  const [x, y, z, w] = q;
  const tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
  return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)];
}
export function slerp(a, b, t) {
  let d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  const s = d < 0 ? -1 : 1;
  d *= s;
  if (d > 0.9995) {
    const r = a.map((v, i) => v + (s * b[i] - v) * t);
    const l = Math.hypot(...r);
    return r.map(v => v / l);
  }
  const th = Math.acos(d), sn = Math.sin(th);
  const wa = Math.sin((1 - t) * th) / sn, wb = (s * Math.sin(t * th)) / sn;
  return a.map((v, i) => v * wa + b[i] * wb);
}

/** Local transform of one animated node at time ms (HierObj::set_to_time: binary search, slerp + lerp, looping). */
export function sampleTrack(keys, ms, duration) {
  if (!keys.length) return null;
  if (keys.length === 1) return { quat: keys[0].quat, trans: keys[0].trans };
  const t = duration > 0 ? ms % duration : 0;
  let i = 0;
  while (i < keys.length - 1 && keys[i].time < t) i++;
  const k1 = keys[Math.max(0, i - 1)], k2 = keys[i];
  const span = k2.time - k1.time;
  const u = span > 0 ? Math.min(1, Math.max(0, (t - k1.time) / span)) : 0;
  return { quat: slerp(k1.quat, k2.quat, u), trans: k1.trans.map((v, j) => v + (k2.trans[j] - v) * u) };
}

// ---------------------------------------------------------------- TGA

export function parseTGA(buf) {
  const idLen = buf[0], cmapType = buf[1], type = buf[2];
  const cmapLen = buf.readUInt16LE(5), cmapEntry = buf[7];
  const w = buf.readUInt16LE(12), h = buf.readUInt16LE(14), bpp = buf[16], desc = buf[17];
  if (cmapType !== 0 || ![2, 10].includes(type) || ![24, 32].includes(bpp)) {
    throw new Error(`unsupported TGA type=${type} bpp=${bpp} cmap=${cmapType}`);
  }
  let p = 18 + idLen + (cmapType ? cmapLen * Math.ceil(cmapEntry / 8) : 0);
  const px = bpp / 8, rgba = new Uint8Array(w * h * 4);
  const put = (i, s) => { rgba[i * 4] = buf[s + 2]; rgba[i * 4 + 1] = buf[s + 1]; rgba[i * 4 + 2] = buf[s]; rgba[i * 4 + 3] = px === 4 ? buf[s + 3] : 255; };
  if (type === 2) {
    for (let i = 0; i < w * h; i++, p += px) put(i, p);
  } else {
    let i = 0;
    while (i < w * h) {
      const hdr = buf[p++], n = (hdr & 0x7f) + 1;
      if (hdr & 0x80) { for (let k = 0; k < n; k++) put(i++, p); p += px; }
      else for (let k = 0; k < n; k++, p += px) put(i++, p);
    }
  }
  // Normalize to top-left origin.
  if (!(desc & 0x20)) {
    const row = w * 4, tmp = new Uint8Array(row);
    for (let y = 0; y < h >> 1; y++) {
      const a = y * row, b = (h - 1 - y) * row;
      tmp.set(rgba.subarray(a, a + row)); rgba.copyWithin(a, b, b + row); rgba.set(tmp, b);
    }
  }
  return { width: w, height: h, rgba };
}

export const load = (fn, path) => fn(fs.readFileSync(path));
