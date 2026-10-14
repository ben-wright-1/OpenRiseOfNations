// List the mouse cursors (Data/cursors.xml) and decode Windows .cur files from the install.
// cursors.xml row N is cursor index N for Cursors::set(N) (index -1 = row 0, "Main Cursor"). Each row names a TGA, a
// sub-rectangle (negative x/y pick a cell in a strip of (xs+1)-pixel cells: x = 1 - (xs+1)*x, Cursors::init @007d0270),
// a hotspot, and the exe resource ids of the GDI colour (gdi_id) and black-and-white (bw_id) fallback cursors.
// The .cur files (root cursorNN.cur, art/SC_*.cur colour, art/SC_*2.cur 1-bpp) look like the sources of those resources.
//
//   node re/tools/cursors.mjs [--install DIR] [--xml cursors_uwp.xml]   table of cursors.xml + header of every .cur file
//   node re/tools/cursors.mjs --cur FILE.cur...                          decode .cur headers (hotspot, size, bpp)
//   node re/tools/cursors.mjs --png <index|name|FILE.cur> [out.png]      write one cursor as PNG (default re/previews/cursors/)
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { parseTGA } from './ronassets.mjs';

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args.splice(i, 2)[1] : def; };
const install = opt('--install', path.join(os.homedir(), 'Games/RiseOfNations'));
const xmlName = opt('--xml', 'cursors.xml');
const pngArg = opt('--png');
const curMode = args[0] === '--cur' ? args.shift() : null;
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// Case-insensitive lookup of a Windows-style relative path (".\art\X.tga") under the install.
function resolveCI(rel) {
	let dir = install;
	for (const part of rel.replace(/\\/g, '/').split('/').filter(p => p && p !== '.')) {
		const hit = fs.readdirSync(dir).find(n => n.toLowerCase() === part.toLowerCase());
		if (!hit) return null;
		dir = path.join(dir, hit);
	}
	return dir;
}

export function readCursorsXml(file = path.join(install, 'Data', xmlName)) {
	const xml = fs.readFileSync(file, 'utf8');
	return [...xml.matchAll(/<CURSOR\b([^>]*)>/g)].map((m, index) => {
		const a = Object.fromEntries([...m[1].matchAll(/(\w+)="([^"]*)"/g)].map(([, k, v]) => [k, v]));
		const n = k => Number(a[k]);
		const xs = n('xs'), ys = n('ys');
		const cell = (v, s) => (v < 0 ? 1 - (s + 1) * v : v);
		return { index, name: a.name, file: a.file, x: cell(n('x'), xs), y: cell(n('y'), ys), xs, ys,
			hot_x: n('hot_x'), hot_y: n('hot_y'), gdi_id: n('gdi_id'), bw_id: n('bw_id') };
	});
}

// ICONDIR/CURSORDIR + per-image BITMAPINFOHEADER (or embedded PNG).
export function parseCur(buf) {
	const type = buf.readUInt16LE(2), count = buf.readUInt16LE(4);
	if (buf.readUInt16LE(0) !== 0 || (type !== 1 && type !== 2)) throw new Error('not an .ico/.cur file');
	const images = [];
	for (let i = 0; i < count; i++) {
		const e = 6 + i * 16, offset = buf.readUInt32LE(e + 12), bytes = buf.readUInt32LE(e + 8);
		const img = { w: buf[e] || 256, h: buf[e + 1] || 256, hot_x: buf.readUInt16LE(e + 4), hot_y: buf.readUInt16LE(e + 6),
			bytes, offset };
		if (buf.readUInt32BE(offset) === 0x89504e47) Object.assign(img, { png: true, bpp: 32 });
		else Object.assign(img, { bpp: buf.readUInt16LE(offset + 14), compression: buf.readUInt32LE(offset + 16),
			colors: buf.readUInt32LE(offset + 32), hdr: buf.readUInt32LE(offset) });
		images.push(img);
	}
	return { type: type === 2 ? 'cur' : 'ico', images };
}

// Decode one uncompressed DIB image to top-down RGBA (XOR bitmap + 1-bpp AND transparency mask).
export function decodeCurImage(buf, img) {
	if (img.png) throw new Error('embedded PNG: extract bytes directly');
	const { w, h, bpp } = img, o = img.offset, rgba = new Uint8Array(w * h * 4);
	const ncol = bpp <= 8 ? (img.colors || 1 << bpp) : 0, pal = o + img.hdr, xor = pal + ncol * 4;
	const xstride = ((w * bpp + 31) >> 5) << 2, astride = ((w + 31) >> 5) << 2, and = xor + xstride * h;
	for (let y = 0; y < h; y++) {
		const row = xor + (h - 1 - y) * xstride, arow = and + (h - 1 - y) * astride;
		for (let x = 0; x < w; x++) {
			let b, g, r, a = 255;
			if (bpp <= 8) {
				const bit = x * bpp, idx = (buf[row + (bit >> 3)] >> (8 - bpp - (bit & 7))) & ((1 << bpp) - 1);
				[b, g, r] = [buf[pal + idx * 4], buf[pal + idx * 4 + 1], buf[pal + idx * 4 + 2]];
			} else {
				const p = row + x * (bpp >> 3);
				[b, g, r] = [buf[p], buf[p + 1], buf[p + 2]];
				if (bpp === 32) a = buf[p + 3];
			}
			if (bpp < 32 && (buf[arow + (x >> 3)] >> (7 - (x & 7))) & 1) a = 0; // AND bit set: transparent (or invert)
			rgba.set([r, g, b, a], (y * w + x) * 4);
		}
	}
	return { width: w, height: h, rgba };
}

function writePng(file, { width, height, rgba }) {
	const raw = Buffer.alloc((width * 4 + 1) * height);
	for (let y = 0; y < height; y++) Buffer.from(rgba.buffer, rgba.byteOffset + y * width * 4, width * 4).copy(raw, y * (width * 4 + 1) + 1);
	const crcT = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
	const crc = b => { let c = ~0; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return ~c >>> 0; };
	const chunk = (ty, d) => { const l = Buffer.alloc(4), c = Buffer.alloc(4), td = Buffer.concat([Buffer.from(ty), d]); l.writeUInt32BE(d.length); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
	const ih = Buffer.alloc(13); ih.writeUInt32BE(width); ih.writeUInt32BE(height, 4); ih[8] = 8; ih[9] = 6;
	fs.mkdirSync(path.dirname(file), { recursive: true });
	fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}

const curLine = (name, buf) => {
	try {
		const c = parseCur(buf);
		return `${name}\t${c.type} ` + c.images.map(i => `${i.w}x${i.h} ${i.bpp}bpp hot(${i.hot_x},${i.hot_y})${i.png ? ' png' : ''}`).join(', ');
	} catch (e) { return `${name}\t${e.message}`; }
};

if (fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
	if (curMode) {
		for (const f of args) console.log(curLine(f, fs.readFileSync(f)));
	} else if (pngArg !== undefined) {
		const out = args[0];
		if (/\.(cur|ico)$/i.test(pngArg)) {
			const buf = fs.readFileSync(fs.existsSync(pngArg) ? pngArg : resolveCI(pngArg));
			const img = decodeCurImage(buf, parseCur(buf).images[0]);
			const dest = out ?? path.join(repo, 're/previews/cursors', path.basename(pngArg).replace(/\.\w+$/, '.png'));
			writePng(dest, img); console.log(`wrote ${dest} (${img.width}x${img.height})`);
		} else {
			const rows = readCursorsXml();
			const c = /^\d+$/.test(pngArg) ? rows[+pngArg] : rows.find(r => r.name.toLowerCase() === pngArg.toLowerCase());
			if (!c) throw new Error(`no cursor ${pngArg}`);
			const t = parseTGA(fs.readFileSync(resolveCI(c.file)));
			const rgba = new Uint8Array(c.xs * c.ys * 4);
			for (let y = 0; y < c.ys; y++) for (let x = 0; x < c.xs; x++) {
				const sx = c.x + x, sy = c.y + y;
				if (sx < t.width && sy < t.height) rgba.set(t.rgba.subarray((sy * t.width + sx) * 4, (sy * t.width + sx) * 4 + 4), (y * c.xs + x) * 4);
			}
			const dest = out ?? path.join(repo, 're/previews/cursors', `${String(c.index).padStart(2, '0')}_${c.name.replace(/\W+/g, '_')}.png`);
			writePng(dest, { width: c.xs, height: c.ys, rgba }); console.log(`wrote ${dest} (${c.xs}x${c.ys} from ${c.file} @${c.x},${c.y}, hot ${c.hot_x},${c.hot_y})`);
		}
	} else {
		console.log('idx\tname\tfile\trect\thot\tgdi_id\tbw_id\ttga');
		for (const c of readCursorsXml()) {
			const f = resolveCI(c.file);
			const tga = f ? (() => { const b = fs.readFileSync(f); return `${b.readUInt16LE(12)}x${b.readUInt16LE(14)}x${b[16]}`; })() : 'MISSING';
			console.log([c.index, c.name, c.file, `${c.x},${c.y} ${c.xs}x${c.ys}`, `${c.hot_x},${c.hot_y}`, c.gdi_id, c.bw_id, tga].join('\t'));
		}
		console.log('\n.cur files');
		for (const dir of ['.', 'art']) {
			const d = resolveCI(dir);
			for (const n of fs.readdirSync(d).filter(n => /\.cur$/i.test(n)).sort()) console.log(curLine(path.join(dir, n), fs.readFileSync(path.join(d, n))));
		}
	}
}
