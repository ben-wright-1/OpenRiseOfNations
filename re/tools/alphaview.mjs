// Write a TGA's RGB and alpha channel side by side as PNG (to inspect what alpha encodes).
// Usage: node alphaview.mjs <in.tga> <out.png>
import fs from 'node:fs';
import zlib from 'node:zlib';
import { load, parseTGA } from './ronassets.mjs';
const t = load(parseTGA, process.argv[2]);
const W = t.width * 2, H = t.height, img = Buffer.alloc((W * 3 + 1) * H);
const hist = new Map();
for (let y = 0; y < H; y++) for (let x = 0; x < t.width; x++) {
  const s = (y * t.width + x) * 4, a = t.rgba[s + 3], o = y * (W * 3 + 1) + 1;
  hist.set(a, (hist.get(a) ?? 0) + 1);
  for (let k = 0; k < 3; k++) { img[o + x * 3 + k] = t.rgba[s + k]; img[o + (t.width + x) * 3 + k] = a; }
}
const crcT = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc = b => { let c = ~0; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; };
const ch = (ty, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(ty), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
const ih = Buffer.alloc(13); ih.writeUInt32BE(W); ih.writeUInt32BE(H, 4); ih[8] = 8; ih[9] = 2;
fs.writeFileSync(process.argv[3], Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), ch('IHDR', ih), ch('IDAT', zlib.deflateSync(img)), ch('IEND', Buffer.alloc(0))]));
console.log(`${t.width}x${t.height}; distinct alpha values: ${hist.size}; top:`, [...hist].sort((a, b) => b[1] - a[1]).slice(0, 6));
