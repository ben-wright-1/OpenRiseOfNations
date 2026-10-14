// Software-render a Rise of Nations model (optionally animated) to PNG, to verify the format decoding.
// Usage: node render-preview.mjs <model.bh3> [--tex file.tga] [--anim file.bha] [--frames N] [--yaw deg]
//                                [--pitch deg] [--size px] --out out.png
// With --frames N > 1, renders a contact sheet of N evenly spaced animation frames.
import fs from 'node:fs';
import zlib from 'node:zlib';
import { load, parseBH3, parseBHA, parseTGA, quatMul, quatRotate, sampleTrack } from './ronassets.mjs';

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(`--${name}`); return i >= 0 ? args[i + 1] : def; };
const flag = name => args.includes(`--${name}`);
const modelPath = args[0];
const size = Number(opt('size', 320));
const frames = Number(opt('frames', 1));
const yaw = Number(opt('yaw', 35)) * Math.PI / 180, pitch = Number(opt('pitch', 20)) * Math.PI / 180;

const model = load(parseBH3, modelPath);
const tex = opt('tex') ? load(parseTGA, opt('tex')) : null;
const anim = opt('anim') ? load(parseBHA, opt('anim')) : null;

// ---- transforms as {q, t}; compose(A,B) applies B first, then A
const compose = (A, B) => ({ q: quatMul(A.q, B.q), t: quatRotate(A.q, B.t).map((v, i) => v + A.t[i]) });
const ID = { q: [0, 0, 0, 1], t: [0, 0, 0] };

/** World transform per BH3 node (HierObj::update_transforms: world = parent.world * parent2local * local). */
function pose(ms) {
  const out = [];
  const walk = (node, animNode, parentWorld) => {
    const rest = { q: node.quat, t: node.trans };
    let local = ID;
    if (animNode) {
      const s = sampleTrack(animNode.keys, ms, anim.duration);
      if (s) local = { q: s.quat, t: s.trans };
    }
    const world = compose(compose(parentWorld, rest), local);
    out.push({ node, world });
    node.children.forEach((c, i) => walk(c, animNode?.children[i], world));
  };
  walk(model.hierarchy, anim?.roots[0], ID);
  return out;
}

function skin(ms) {
  const mesh = model.meshes[0];
  const n = mesh.positions.length / 4;
  const P = new Float32Array(n * 3), N = new Float32Array(n * 3);
  for (const { node, world } of pose(ms)) {
    for (let v = node.first; v < node.first + node.count && v < n; v++) {
      const p = quatRotate(world.q, [mesh.positions[v * 4], mesh.positions[v * 4 + 1], mesh.positions[v * 4 + 2]]);
      const nn = quatRotate(world.q, [mesh.normals[v * 4], mesh.normals[v * 4 + 1], mesh.normals[v * 4 + 2]]);
      for (let k = 0; k < 3; k++) { P[v * 3 + k] = p[k] + world.t[k]; N[v * 3 + k] = nn[k]; }
    }
  }
  return { P, N, mesh };
}

// ---- camera framing: fit the bind pose (or first frame) bounding sphere
const first = skin(0);
let c = [0, 0, 0], r = 0;
{
  const n = first.P.length / 3;
  for (let i = 0; i < n; i++) for (let k = 0; k < 3; k++) c[k] += first.P[i * 3 + k] / n;
  for (let i = 0; i < n; i++) r = Math.max(r, Math.hypot(first.P[i * 3] - c[0], first.P[i * 3 + 1] - c[1], first.P[i * 3 + 2] - c[2]));
}

function renderFrame(ms, img, ox, oy, W) {
  const { P, N, mesh } = ms === 0 ? first : skin(ms);
  const z = new Float32Array(size * size).fill(Infinity);
  const cy = Math.cos(yaw), sy = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
  const dist = r * 3.2, f = size / (2 * Math.tan(0.3));
  // World is Z-up. Camera orbits around Z, looking at the model center.
  const view = p => {
    const x = p[0] - c[0], y = p[1] - c[1], zz = p[2] - c[2];
    const x1 = x * cy - y * sy, y1 = x * sy + y * cy;   // yaw
    const depth = y1 * cp + zz * sp, up = -y1 * sp + zz * cp; // pitch
    const d = dist + depth;
    return [size / 2 + (x1 * f) / d, size / 2 - (up * f) / d, d];
  };
  const L = [0.4, -0.5, 0.75], ll = Math.hypot(...L);
  const n = P.length / 3, S = new Array(n);
  for (let i = 0; i < n; i++) S[i] = view([P[i * 3], P[i * 3 + 1], P[i * 3 + 2]]);
  const idx = mesh.indices, uv = mesh.uvs;
  for (let t = 0; t + 2 < idx.length; t += 3) {
    const [a, b, cc] = [idx[t], idx[t + 1], idx[t + 2]];
    const A = S[a], B = S[b], C = S[cc];
    const area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]);
    if (area === 0) continue;
    const x0 = Math.max(0, Math.floor(Math.min(A[0], B[0], C[0]))), x1 = Math.min(size - 1, Math.ceil(Math.max(A[0], B[0], C[0])));
    const y0 = Math.max(0, Math.floor(Math.min(A[1], B[1], C[1]))), y1 = Math.min(size - 1, Math.ceil(Math.max(A[1], B[1], C[1])));
    for (let py = y0; py <= y1; py++) for (let px = x0; px <= x1; px++) {
      const qx = px + 0.5, qy = py + 0.5;
      const w0 = ((B[0] - qx) * (C[1] - qy) - (B[1] - qy) * (C[0] - qx)) / area;
      const w1 = ((C[0] - qx) * (A[1] - qy) - (C[1] - qy) * (A[0] - qx)) / area;
      const w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue; // two-sided: area sign normalizes winding
      const d = w0 * A[2] + w1 * B[2] + w2 * C[2];
      if (d >= z[py * size + px]) continue;
      let col = [200, 200, 200, 255];
      if (tex && uv) {
        const u = w0 * uv[a * 2] + w1 * uv[b * 2] + w2 * uv[cc * 2];
        const v = w0 * uv[a * 2 + 1] + w1 * uv[b * 2 + 1] + w2 * uv[cc * 2 + 1];
        const tx = ((Math.floor(u * tex.width) % tex.width) + tex.width) % tex.width;
        const ty = ((Math.floor(v * tex.height) % tex.height) + tex.height) % tex.height;
        const o = (ty * tex.width + tx) * 4;
        col = [tex.rgba[o], tex.rgba[o + 1], tex.rgba[o + 2], tex.rgba[o + 3]];
      }
      const nx = w0 * N[a * 3] + w1 * N[b * 3] + w2 * N[cc * 3];
      const ny = w0 * N[a * 3 + 1] + w1 * N[b * 3 + 1] + w2 * N[cc * 3 + 1];
      const nz = w0 * N[a * 3 + 2] + w1 * N[b * 3 + 2] + w2 * N[cc * 3 + 2];
      const nl = Math.hypot(nx, ny, nz) || 1;
      const lit = 0.45 + 0.75 * Math.abs((nx * L[0] + ny * L[1] + nz * L[2]) / (nl * ll));
      z[py * size + px] = d;
      const o = ((oy + py) * W + ox + px) * 3;
      for (let k = 0; k < 3; k++) img[o + k] = Math.min(255, col[k] * lit);
    }
  }
}

// ---- layout + PNG
const cols = Math.ceil(Math.sqrt(frames)), rows = Math.ceil(frames / cols);
const W = cols * size, H = rows * size;
const img = new Uint8Array(W * H * 3);
for (let i = 0; i < W * H; i++) { img[i * 3] = 46; img[i * 3 + 1] = 52; img[i * 3 + 2] = 64; }
const dur = anim?.duration ?? 0;
for (let i = 0; i < frames; i++) renderFrame(frames > 1 ? (dur * i) / frames : 0, img, (i % cols) * size, Math.floor(i / cols) * size, W);

function png(w, h, rgb) {
  const crcTable = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
  const crc = b => { let c = 0xffffffff; for (const x of b) c = crcTable[(c ^ x) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
    return Buffer.concat([len, td, c]);
  };
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgb.buffer, y * w * 3, w * 3).copy(raw, y * (w * 3 + 1) + 1);
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
fs.writeFileSync(opt('out', 'preview.png'), png(W, H, img));
console.log(`wrote ${opt('out', 'preview.png')} ${W}x${H}, bounds r=${r.toFixed(1)}, anim ${dur}ms`);
