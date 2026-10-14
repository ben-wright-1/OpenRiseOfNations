// Software-render a unit with its EXISTEVENT attachments (weapons/props), following docs/attachments-spec.md.
// Attachments are tinted slightly red so they stand out. Game dir: $RON_GAME or ~/Games/RiseOfNations.
// Usage: node re/tools/attach-preview.mjs <UNIT-NAME> <ACTION> <frames> <out.png> [--noattach]
//   e.g. node re/tools/attach-preview.mjs EXPLORER-DEFAULT-AGE0 CHAR_IDLE3 9 re/previews/attach_explorer_idle3.png
import fs from 'node:fs'; import zlib from 'node:zlib';
import { parseBH3, parseBHA, parseTGA, quatMul, quatRotate, sampleTrack } from './ronassets.mjs';
const G = (process.env.RON_GAME ?? process.env.HOME + '/Games/RiseOfNations') + '/';
const read = f => fs.readFileSync(G + 'Data/' + f, 'latin1');
const attr = (tag, a) => tag.match(new RegExp(`\\b${a}="([^"]*)"`, 'i'))?.[1];
const files = new Map(fs.readdirSync(G + 'art').map(f => [f.toLowerCase(), 'art/' + f]));
const art = p => files.get(p.replace(/^\.[\\/]/, '').replace(/\\/g, '/').replace(/^art\//i, '').toLowerCase());
const animFiles = new Map([...read('anim_graphics.xml').matchAll(/<ANIM\s[^>]*>/g)].map(m => [attr(m[0], 'name').toLowerCase(), attr(m[0], 'file')]));
const ammo = new Map();
for (const m of read('effects_graphics.xml').matchAll(/<AMMO\s[^>]*>/g)) { const k = attr(m[0], 'name').toLowerCase(); if (!ammo.has(k)) ammo.set(k, { model: attr(m[0], 'model'), texture: attr(m[0], 'texture'), anim: attr(m[0], 'anim') }); }
const [unitName, action, framesS, out] = process.argv.slice(2); const noAttach = process.argv.includes('--noattach');
const um = [...read('unit_graphics.xml').matchAll(/<UNIT\s[^>]*>([\s\S]*?)<\/UNIT>/g)].find(m => attr(m[0].match(/<UNIT\s[^>]*>/)[0], 'name') === unitName);
const open = um[0].match(/<UNIT\s[^>]*>/)[0];
const animName = [...um[1].matchAll(/<ANIM\s[^>]*>/g)].find(a => attr(a[0], 'name') === action);
const loadAnim = name => { const f = name && animFiles.get(name.toLowerCase()); const p = f && art(f); return p ? parseBHA(fs.readFileSync(G + p)) : null; };
const unit = { model: parseBH3(fs.readFileSync(G + art(attr(open, 'model')))), tex: parseTGA(fs.readFileSync(G + art(attr(open, 'texture')))), anim: loadAnim(attr(animName[0], 'file')) };
const events = [...um[1].matchAll(/<EXISTEVENT\s[^>]*>/g)].map(a => ({ anim: attr(a[0], 'anim'), start: +attr(a[0], 'starttime'), end: +attr(a[0], 'endtime'), type: attr(a[0], 'type'), node: +attr(a[0], 'node') })).filter(e => e.anim === action);
const attachments = new Map();
for (const e of events) { const k = e.type.toLowerCase(); if (!attachments.has(k)) { const a = ammo.get(k); attachments.set(k, { model: parseBH3(fs.readFileSync(G + art(a.model))), tex: parseTGA(fs.readFileSync(G + art(a.texture))), anim: loadAnim(a.anim) }); } }
const compose = (A, B) => ({ q: quatMul(A.q, B.q), t: quatRotate(A.q, B.t).map((v, i) => v + A.t[i]) });
const ID = { q: [0, 0, 0, 1], t: [0, 0, 0] };
function pose(obj, ms) { // returns [{node, world}] in DFS order, plus slots
  const list = [], slots = new Array(14).fill(null);
  const walk = (node, an, pw) => {
    let local = ID; if (an && obj.anim) { const s = sampleTrack(an.keys, ms, obj.anim.duration); if (s) local = { q: s.quat, t: s.trans }; }
    const world = compose(compose(pw, { q: node.quat, t: node.trans }), local);
    list.push({ node, world });
    const nm = node.name.toLowerCase();
    for (let i = 0; i < 14; i++) { let s = null; if (nm === 'ammo node ' + i || nm === 'node ' + i) s = i; else if (nm === 'pivot node ' + i) s = i + 4; else if (nm === 'smoke node ' + i) s = i + 8; if (s !== null) { if (s < 14) slots[s] = world; break; } }
    node.children.forEach((c, i) => walk(c, an?.children[i], world));
  };
  walk(obj.model.hierarchy, obj.anim?.roots[0], ID);
  return { list, slots };
}
function skin(obj, ms, base) { // base: transform applied after the object's own pose
  const { list, slots } = pose(obj, ms); const mesh = obj.model.meshes[0]; const n = mesh.positions.length / 4;
  const P = new Float32Array(n * 3);
  for (const { node, world } of list) { const w = compose(base, world); for (let v = node.first; v < node.first + node.count && v < n; v++) { const p = quatRotate(w.q, [mesh.positions[v * 4], mesh.positions[v * 4 + 1], mesh.positions[v * 4 + 2]]); for (let k = 0; k < 3; k++) P[v * 3 + k] = p[k] + w.t[k]; } }
  return { P, mesh, tex: obj.tex, slots };
}
function scene(ms) {
  const body = skin(unit, ms, ID); const parts = [body];
  if (!noAttach) for (const e of events) {
    if (!(e.start <= ms && ms <= e.end)) continue;
    let base; if (e.node < 0) base = ID; else if (e.node > 13 || !body.slots[e.node]) continue; else base = body.slots[e.node];
    parts.push(skin(attachments.get(e.type.toLowerCase()), ms, base));
  }
  return parts;
}
const size = 300, frames = +framesS, dur = unit.anim?.duration ?? 0;
const first = scene(0); let c = [0, 0, 0], r = 0, cnt = 0;
for (const p of first) for (let i = 0; i < p.P.length; i += 3) { c[0] += p.P[i]; c[1] += p.P[i + 1]; c[2] += p.P[i + 2]; cnt++; } c = c.map(v => v / cnt);
for (const p of first) for (let i = 0; i < p.P.length; i += 3) r = Math.max(r, Math.hypot(p.P[i] - c[0], p.P[i + 1] - c[1], p.P[i + 2] - c[2]));
const yaw = 35 * Math.PI / 180, pitch = 15 * Math.PI / 180;
const cols = Math.ceil(Math.sqrt(frames)), rows = Math.ceil(frames / cols), W = cols * size, H = rows * size;
const img = new Uint8Array(W * H * 3).fill(60);
for (let fi = 0; fi < frames; fi++) {
  const ms = frames > 1 ? Math.round(dur * fi / frames) : 0, ox = (fi % cols) * size, oy = Math.floor(fi / cols) * size;
  const z = new Float32Array(size * size).fill(Infinity);
  const cy = Math.cos(yaw), sy = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch), dist = r * 3.2, f = size / (2 * Math.tan(0.3));
  const view = (x, y, zz) => { x -= c[0]; y -= c[1]; zz -= c[2]; const x1 = x * cy - y * sy, y1 = x * sy + y * cy; const depth = y1 * cp + zz * sp, up = -y1 * sp + zz * cp; const d = dist + depth; return [size / 2 + x1 * f / d, size / 2 - up * f / d, d]; };
  for (const [pi, part] of scene(ms).entries()) {
    const { P, mesh, tex } = part; const S = []; for (let i = 0; i < P.length; i += 3) S.push(view(P[i], P[i + 1], P[i + 2]));
    const idx = mesh.indices, uv = mesh.uvs;
    for (let t = 0; t + 2 < idx.length; t += 3) {
      const a = idx[t], b = idx[t + 1], cc = idx[t + 2], A = S[a], B = S[b], C = S[cc];
      const area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]); if (!area) continue;
      for (let py = Math.max(0, Math.floor(Math.min(A[1], B[1], C[1]))); py <= Math.min(size - 1, Math.ceil(Math.max(A[1], B[1], C[1]))); py++)
        for (let px = Math.max(0, Math.floor(Math.min(A[0], B[0], C[0]))); px <= Math.min(size - 1, Math.ceil(Math.max(A[0], B[0], C[0]))); px++) {
          const qx = px + .5, qy = py + .5; const w0 = ((B[0] - qx) * (C[1] - qy) - (B[1] - qy) * (C[0] - qx)) / area, w1 = ((C[0] - qx) * (A[1] - qy) - (C[1] - qy) * (A[0] - qx)) / area, w2 = 1 - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue; const d = w0 * A[2] + w1 * B[2] + w2 * C[2]; if (d >= z[py * size + px]) continue;
          const u = w0 * uv[a * 2] + w1 * uv[b * 2] + w2 * uv[cc * 2], v = w0 * uv[a * 2 + 1] + w1 * uv[b * 2 + 1] + w2 * uv[cc * 2 + 1];
          const tx = ((Math.floor(u * tex.width) % tex.width) + tex.width) % tex.width, ty = ((Math.floor(v * tex.height) % tex.height) + tex.height) % tex.height, o = (ty * tex.width + tx) * 4;
          if (pi > 0 && tex.rgba[o + 3] === 0) continue;
          z[py * size + px] = d; const q = ((oy + py) * W + ox + px) * 3;
          const tint = pi > 0 ? [1, 0.85, 0.85] : [1, 1, 1];
          for (let k = 0; k < 3; k++) img[q + k] = Math.min(255, tex.rgba[o + k] * tint[k]);
        }
    }
  }
}
const crcT = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = b => { let c = 0xffffffff; for (const x of b) c = crcT[(c ^ x) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const cb = Buffer.alloc(4); cb.writeUInt32BE(crc(td)); return Buffer.concat([l, td, cb]); };
const raw = Buffer.alloc((W * 3 + 1) * H); for (let y = 0; y < H; y++) Buffer.from(img.buffer, y * W * 3, W * 3).copy(raw, y * (W * 3 + 1) + 1);
const ih = Buffer.alloc(13); ih.writeUInt32BE(W, 0); ih.writeUInt32BE(H, 4); ih[8] = 8; ih[9] = 2;
fs.writeFileSync(out, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
console.log('wrote', out, 'events', events.length, 'anim', dur);
