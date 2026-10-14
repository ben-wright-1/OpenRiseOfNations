// Software-render one building the way docs/buildings-spec.md describes: the pre-rendered sprite with team colour
// (alpha 248 texels), its baked shadow (alpha 145), the invisible zbuffer BH3 as a depth-only mesh, and the 3D
// EXISTEVENT add-ons (BUILD_DEFAULT) depth-tested against it. Orthographic game camera at zoom level 6 (3 world units
// per pixel, yaw 45, tilt 60). Game dir: $RON_GAME or ~/Games/RiseOfNations.
// Usage: node re/tools/building-preview.mjs <BUILD-NAME> <out.png> [--team 0..8] [--ms N] [--z] [--tint] [--anim BUILD_BUILD] [--wind deg]
//   --z tints the pixels covered by the zbuffer mesh red, to check its alignment with the sprite; --tint marks add-ons magenta.
//   e.g. node re/tools/building-preview.mjs VILLAGE-DEFAULT-AGE0 re/previews/build_village0.png --team 1 --z
import fs from 'node:fs'; import zlib from 'node:zlib';
import { parseBH3, parseBHA, parseTGA, quatMul, quatRotate, sampleTrack } from './ronassets.mjs';

const G = (process.env.RON_GAME ?? process.env.HOME + '/Games/RiseOfNations') + '/';
const read = f => fs.readFileSync(G + 'Data/' + f, 'latin1');
const attr = (tag, a) => tag.match(new RegExp(`\\b${a}="([^"]*)"`, 'i'))?.[1];
const files = new Map(fs.readdirSync(G + 'art').map(f => [f.toLowerCase(), 'art/' + f]));
const art = p => p && files.get(p.replace(/^\.[\\/]/, '').replace(/\\/g, '/').replace(/^art\//i, '').toLowerCase());
const animFiles = new Map([...read('anim_graphics.xml').matchAll(/<ANIM\s[^>]*>/g)].map(m => [attr(m[0], 'name')?.toLowerCase(), attr(m[0], 'file')]));
const ammo = new Map();
for (const m of read('effects_graphics.xml').matchAll(/<AMMO\s[^>]*>/g)) {
  const k = attr(m[0], 'name').toLowerCase();
  if (!ammo.has(k)) ammo.set(k, { model: attr(m[0], 'model'), texture: attr(m[0], 'texture') });
}
// Default building team colours (TEAM_COLOR_BUILD_*, colors.obj TeamColors::init); index = player colour slot.
const TEAM = [[102, 0, 0], [26, 57, 172], [128, 0, 128], [0, 89, 0], [98, 96, 0], [0, 92, 108], [120, 120, 120], [112, 48, 0], [255, 255, 255]];

const [name, out] = process.argv.slice(2);
const opt = k => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : undefined; };
const team = TEAM[+(opt('--team') ?? 1)], ms = +(opt('--ms') ?? 0), showZ = process.argv.includes('--z'), tintAddons = process.argv.includes('--tint');
const buildAnim = opt('--anim') ?? 'BUILD_DEFAULT', wind = +(opt('--wind') ?? 0);
const bm = [...read('building_graphics.xml').matchAll(/<BUILD\s[^>]*?(?:\/>|>([\s\S]*?)<\/BUILD>)/g)].find(m => attr(m[0], 'name') === name);
if (!bm) throw new Error('no BUILD ' + name);
const open = bm[0].match(/<BUILD\s[^>]*>/)[0];
const sprite = parseTGA(fs.readFileSync(G + art(attr(open, 'texture'))));
const xo = +attr(open, 'spritexoff'), yo = +attr(open, 'spriteyoff');

// Camera (zoom level 6): world point P relative to the building's (half-offset) location -> sprite-relative pixels.
const U = 3, M = 40, W = sprite.width + 2 * M, H = sprite.height + 2 * M, AX = M + xo, AY = M + yo;
const project = p => [AX + 0.70711 * (p[0] - p[1]) / U, AY + (0.35355 * (p[0] + p[1]) - 0.86603 * p[2]) / U, -(0.61237 * p[0] + 0.61237 * p[1] + 0.5 * p[2])];
// Model space -> world, shared by the zbuffer mesh and the add-ons: Rz(270, engine convention) then scale (3, -3, 3).
const toWorld = v => [-3 * v[1], -3 * v[0], 3 * v[2]];

const img = new Float32Array(W * H * 3); for (let i = 0; i < W * H; i++) img.set([70, 90, 60], i * 3);
const zbuf = new Float32Array(W * H).fill(Infinity), zmask = new Uint8Array(W * H);
// 1. Sprite: Team Color Blend pass (alpha == 248: tex * team * 4, opaque), then Building Blend (alpha != 248, src-alpha blend).
for (let y = 0; y < sprite.height; y++) for (let x = 0; x < sprite.width; x++) {
  const s = (y * sprite.width + x) * 4, a = sprite.rgba[s + 3], d = ((y + M) * W + x + M) * 3;
  if (a === 0) continue;
  for (let k = 0; k < 3; k++) {
    const t = sprite.rgba[s + k];
    img[d + k] = a === 248 ? Math.min(255, t * team[k] / 255 * 4) : img[d + k] * (1 - a / 255) + t * a / 255;
  }
}
// 2. Triangle rasterizer (orthographic, so affine UV interpolation is exact).
function raster(P, idx, onPixel) {
  for (let i = 0; i < idx.length; i += 3) {
    const [a, b, c] = [P[idx[i]], P[idx[i + 1]], P[idx[i + 2]]];
    const e = (p, q, x, y) => (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0]);
    const area = e(a, b, c[0], c[1]); if (Math.abs(area) < 1e-9) continue;
    const x0 = Math.max(0, Math.floor(Math.min(a[0], b[0], c[0]))), x1 = Math.min(W - 1, Math.ceil(Math.max(a[0], b[0], c[0])));
    const y0 = Math.max(0, Math.floor(Math.min(a[1], b[1], c[1]))), y1 = Math.min(H - 1, Math.ceil(Math.max(a[1], b[1], c[1])));
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const w0 = e(b, c, x + 0.5, y + 0.5) / area, w1 = e(c, a, x + 0.5, y + 0.5) / area, w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      onPixel(y * W + x, [w0, w1, w2], [idx[i], idx[i + 1], idx[i + 2]]);
    }
  }
}
// Pose a BH3 (world = parent * rest * anim) and return projected vertices.
const compose = (A, B) => ({ q: quatMul(A.q, B.q), t: quatRotate(A.q, B.t).map((v, i) => v + A.t[i]) });
const SLOT = nm => { for (let i = 0; i < 14; i++) { if (nm === 'ammo node ' + i || nm === 'node ' + i) return i; if (nm === 'pivot node ' + i) return i + 4; if (nm === 'smoke node ' + i) return i + 8; } return -1; };
function posed(model, anim, xf = toWorld) {
  const mesh = model.meshes[0], n = mesh.positions.length / 4, P = new Array(n), slots = new Array(14).fill(null);
  const walk = (node, an, pw) => {
    let local = { q: [0, 0, 0, 1], t: [0, 0, 0] };
    if (an && anim) { const s = sampleTrack(an.keys, ms, anim.duration); if (s) local = { q: s.quat, t: s.trans }; }
    const w = compose(compose(pw, { q: node.quat, t: node.trans }), local);
    for (let v = node.first; v < node.first + node.count && v < n; v++) {
      const p = quatRotate(w.q, [mesh.positions[v * 4], mesh.positions[v * 4 + 1], mesh.positions[v * 4 + 2]]);
      P[v] = project(xf(p.map((x, k) => x + w.t[k])));
    }
    const sl = SLOT(node.name.toLowerCase()); if (sl >= 0 && sl < 14) slots[sl] = w;
    node.children.forEach((c, i) => walk(c, an?.children[i], w));
  };
  walk(model.hierarchy, anim?.roots[0], { q: [0, 0, 0, 1], t: [0, 0, 0] });
  return { P, mesh, slots };
}
// 3. zbuffer mesh: depth only ("Building ZBuffer Blend" writes depth, colour alpha 0).
const zfile = art(attr(open, 'zbuffer'));
let hullSlots = new Array(14).fill(null);
if (zfile) {
  const { P, mesh, slots } = posed(parseBH3(fs.readFileSync(G + zfile)), null); hullSlots = slots;
  raster(P, mesh.indices, (i, w, v) => { const z = w[0] * P[v[0]][2] + w[1] * P[v[1]][2] + w[2] * P[v[2]][2]; if (z < zbuf[i]) zbuf[i] = z; zmask[i] = 1; });
}
// 4. Add-ons (EXISTEVENT buildanim=BUILD_DEFAULT), "Ammo Blend": tex, alpha test > 0, depth-tested against the zbuffer mesh.
const events = [...(bm[1] ?? '').matchAll(/<EXISTEVENT\s[^>]*>/g)].map(m => m[0]).filter(t => attr(t, 'buildanim') === buildAnim);
for (const ev of events) {
  const a = ammo.get(attr(ev, 'subject')?.toLowerCase()); if (!a) continue;
  // axis != 0: offset by the hull node's translation (slot `node`, default 7 = "pivot node 3") and face the wind:
  // angle = 45 - wind_angle + 180. World = L + off + S(3,-3,3)(Rz_engine(angle) v), Rz_engine(a) rotating by -a.
  let xf = toWorld;
  if ((+attr(ev, 'axis') || 0) !== 0) {
    const nw = hullSlots[+(attr(ev, 'node') ?? 7)]; if (!nw) continue;
    const off = toWorld(nw.t), ang = (45 - wind + 180) * Math.PI / 180, c = Math.cos(ang), s = Math.sin(ang);
    xf = v => [off[0] + 3 * (v[0] * c + v[1] * s), off[1] - 3 * (-v[0] * s + v[1] * c), off[2] + 3 * v[2]];
  }
  const animFile = art(animFiles.get(attr(ev, 'anim')?.toLowerCase()));
  const tex = parseTGA(fs.readFileSync(G + art(a.texture)));
  const { P, mesh } = posed(parseBH3(fs.readFileSync(G + art(a.model))), animFile ? parseBHA(fs.readFileSync(G + animFile)) : null, xf);
  raster(P, mesh.indices, (i, w, v) => {
    const z = w[0] * P[v[0]][2] + w[1] * P[v[1]][2] + w[2] * P[v[2]][2]; if (z > zbuf[i]) return;
    const u = w[0] * mesh.uvs[v[0] * 2] + w[1] * mesh.uvs[v[1] * 2] + w[2] * mesh.uvs[v[2] * 2];
    const t = w[0] * mesh.uvs[v[0] * 2 + 1] + w[1] * mesh.uvs[v[1] * 2 + 1] + w[2] * mesh.uvs[v[2] * 2 + 1];
    const tx = Math.min(tex.width - 1, Math.max(0, Math.floor((u - Math.floor(u)) * tex.width)));
    const ty = Math.min(tex.height - 1, Math.max(0, Math.floor((t - Math.floor(t)) * tex.height)));
    const s = (ty * tex.width + tx) * 4, al = tex.rgba[s + 3] / 255; if (al === 0) return;
    zbuf[i] = z; for (let k = 0; k < 3; k++) img[i * 3 + k] = img[i * 3 + k] * (1 - al) + (tintAddons ? (tex.rgba[s + k] + [255, 0, 255][k]) / 2 : tex.rgba[s + k]) * al;
  });
}
if (showZ) for (let i = 0; i < W * H; i++) if (zmask[i]) { img[i * 3] = img[i * 3] * 0.6 + 255 * 0.4; img[i * 3 + 1] *= 0.6; img[i * 3 + 2] *= 0.6; }
// Anchor cross (the building's projected location).
for (let d = -4; d <= 4; d++) { for (const [x, y] of [[AX + d, AY], [AX, AY + d]]) img.set([255, 255, 0], (y * W + x) * 3); }

const raw = Buffer.alloc((W * 3 + 1) * H);
for (let y = 0; y < H; y++) for (let x = 0; x < W * 3; x++) raw[y * (W * 3 + 1) + 1 + x] = Math.round(img[y * W * 3 + x]);
const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4, 'latin1'); d.copy(b, 8); b.writeUInt32BE(zlib.crc32(b.subarray(4, 8 + d.length)) >>> 0, 8 + d.length); return b; };
const ih = Buffer.alloc(13); ih.writeUInt32BE(W, 0); ih.writeUInt32BE(H, 4); ih[8] = 8; ih[9] = 2;
fs.writeFileSync(out, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
console.log(`${name}: ${sprite.width}x${sprite.height} sprite, anchor (${xo},${yo}), ${events.length} add-on(s) -> ${out}`);
