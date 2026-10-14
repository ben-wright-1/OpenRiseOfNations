// Check the building placement spec (docs/buildings-spec.md) against the data: project every <BUILD>'s zbuffer BH3
// with the game's orthographic camera and compare its silhouette with the sprite's opaque texels (alpha 255 or 248),
// anchored at (spritexoff, spriteyoff). Prints median IoU per model->world mapping and per units-per-pixel value.
// Usage: node re/tools/building-zfit.mjs [--sweep]      Game dir: $RON_GAME or ~/Games/RiseOfNations.
import fs from 'node:fs';
import { parseBH3, parseTGA } from './ronassets.mjs';

const G = (process.env.RON_GAME ?? process.env.HOME + '/Games/RiseOfNations') + '/';
const attr = (tag, a) => tag.match(new RegExp(`\\b${a}="([^"]*)"`, 'i'))?.[1];
const files = new Map(fs.readdirSync(G + 'art').map(f => [f.toLowerCase(), 'art/' + f]));
const art = p => p && files.get(p.replace(/^\.[\\/]/, '').replace(/\\/g, '/').replace(/^art\//i, '').toLowerCase());
const builds = [...fs.readFileSync(G + 'Data/building_graphics.xml', 'latin1').matchAll(/<BUILD\s[^>]*>/g)].map(m => m[0])
  .filter(t => art(attr(t, 'texture')) && art(attr(t, 'zbuffer')));

const mappings = { // model space -> world (building at the origin)
  'engine Rz270+S(3,-3,3)': v => [-3 * v[1], -3 * v[0], 3 * v[2]],
  'raw S(3)': v => [3 * v[0], 3 * v[1], 3 * v[2]],
  'swap x/y': v => [3 * v[1], 3 * v[0], 3 * v[2]],
};
function iou(t, map, u) {
  const tex = parseTGA(fs.readFileSync(G + art(attr(t, 'texture')))), m = parseBH3(fs.readFileSync(G + art(attr(t, 'zbuffer'))));
  const W = tex.width, H = tex.height, xo = +attr(t, 'spritexoff'), yo = +attr(t, 'spriteyoff');
  const P = m.meshes[0].positions, idx = m.meshes[0].indices, S = [], cover = new Uint8Array(W * H);
  for (let i = 0; i < P.length; i += 4) {
    const p = map([P[i], P[i + 1], P[i + 2]]);
    S.push([xo + 0.70711 * (p[0] - p[1]) / u, yo + (0.35355 * (p[0] + p[1]) - 0.86603 * p[2]) / u]);
  }
  for (let i = 0; i < idx.length; i += 3) {
    const [a, b, c] = [S[idx[i]], S[idx[i + 1]], S[idx[i + 2]]], e = (p, q, x, y) => (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0]);
    const ar = e(a, b, c[0], c[1]); if (Math.abs(ar) < 1e-9) continue;
    for (let y = Math.max(0, Math.floor(Math.min(a[1], b[1], c[1]))); y <= Math.min(H - 1, Math.ceil(Math.max(a[1], b[1], c[1]))); y++)
      for (let x = Math.max(0, Math.floor(Math.min(a[0], b[0], c[0]))); x <= Math.min(W - 1, Math.ceil(Math.max(a[0], b[0], c[0]))); x++) {
        const w0 = e(b, c, x + 0.5, y + 0.5) / ar, w1 = e(c, a, x + 0.5, y + 0.5) / ar;
        if (w0 >= 0 && w1 >= 0 && w0 + w1 <= 1) cover[y * W + x] = 1;
      }
  }
  let I = 0, U = 0;
  for (let i = 0; i < W * H; i++) { const a = tex.rgba[i * 4 + 3], s = a === 255 || a === 248; if (s && cover[i]) I++; if (s || cover[i]) U++; }
  return I / U;
}
const median = a => a.sort((p, q) => p - q)[a.length >> 1];
for (const [name, map] of Object.entries(mappings))
  console.log(`${name.padEnd(24)} u=3   median IoU ${median(builds.map(t => iou(t, map, 3))).toFixed(3)}  (${builds.length} buildings)`);
if (process.argv.includes('--sweep')) {
  const sub = builds.filter((_, i) => i % 15 === 0);
  for (const u of [2.5, 2.75, 3, 3.25, 3.5, 6]) console.log(`engine mapping u=${u}  median IoU ${median(sub.map(t => iou(t, mappings['engine Rz270+S(3,-3,3)'], u))).toFixed(3)}  (${sub.length} buildings)`);
}
