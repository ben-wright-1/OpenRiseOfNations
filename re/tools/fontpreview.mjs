// Font reference tool: reads Data/fonts.xml and the TrueType files in Data/Fonts, reproduces the EE text metrics
// (d3dgl.dll FreeType path, docs/ui-spec.md §3) and renders strings to a PNG.
//
//   node re/tools/fontpreview.mjs list [--w 1024]
//        The FONT entries of the default CODEPAGE for the RESOLUTION closest to the screen width, with the
//        metrics the game derives (LineHeight, FontHeight, Ascent, Descent, MaxAdvance) and the LOOKUPS table.
//   node re/tools/fontpreview.mjs render "text" [--font Georgia] [--size 14] [--style B] [--color ffffff]
//        [--shadow 000000] [--sx 1 --sy 1] [--bg 202020] [--out re/previews/font.png] [--lang .7]
//        Lays the string out the way D3D11GLFont::write does (glyph cell = advance box without left bearing,
//        cell height = FontHeight, pen += hinted advance, no kerning, "#ICONn" not expanded) and draws it.
//   node re/tools/fontpreview.mjs sample [--w 1024]   One line per font of the chosen resolution.
//
// The rasterizer is unhinted (FreeType renders hinted), so stems can differ by a pixel; the integer metrics follow
// FreeType's scaled-metrics rounding for TrueType faces. Reads only the install; writes only --out.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { parseXML } from './ifacelayout.mjs';

const argv = process.argv.slice(2);
const opt = (n, d) => { const i = argv.indexOf(`--${n}`); return i >= 0 ? argv.splice(i, 2)[1] : d; };
const install = opt('install', path.join(os.homedir(), 'Games/RiseOfNations'));
const screenW = +opt('w', 1024);
const lang = opt('lang', '');
const O = {
  out: opt('out', null), bg: opt('bg', '202020'), shadow: opt('shadow', null), sx: +opt('sx', 1), sy: +opt('sy', 1),
  font: opt('font', 'Georgia'), size: +opt('size', 14), style: opt('style', 'B'), color: opt('color', 'ffffff'),
};
const [cmd = 'list', text = 'Rise of Nations 0123456789'] = argv;

// ------------------------------------------------------------------ TrueType parsing
export function parseTTF(buf) {
  const tables = {};
  const n = buf.readUInt16BE(4);
  for (let i = 0; i < n; i++) {
    const o = 12 + i * 16;
    tables[buf.toString('latin1', o, o + 4)] = { off: buf.readUInt32BE(o + 8), len: buf.readUInt32BE(o + 12) };
  }
  const t = name => tables[name]?.off;
  const head = t('head'), hhea = t('hhea'), maxp = t('maxp'), hmtx = t('hmtx'), loca = t('loca'), glyf = t('glyf');
  const unitsPerEm = buf.readUInt16BE(head + 18), headFlags = buf.readUInt16BE(head + 16);
  const indexToLoc = buf.readInt16BE(head + 50);
  const ascender = buf.readInt16BE(hhea + 4), descender = buf.readInt16BE(hhea + 6), lineGap = buf.readInt16BE(hhea + 8);
  const advanceWidthMax = buf.readUInt16BE(hhea + 10), numHMetrics = buf.readUInt16BE(hhea + 34);
  const numGlyphs = buf.readUInt16BE(maxp + 4);
  // cmap: Windows Unicode BMP (3,1) format 4
  const cmap = t('cmap'), cmapCount = buf.readUInt16BE(cmap + 2);
  let sub = -1;
  for (let i = 0; i < cmapCount; i++) {
    const pid = buf.readUInt16BE(cmap + 4 + i * 8), eid = buf.readUInt16BE(cmap + 6 + i * 8);
    if (pid === 3 && eid === 1) sub = cmap + buf.readUInt32BE(cmap + 8 + i * 8);
  }
  const glyphIndex = c => {
    if (sub < 0 || buf.readUInt16BE(sub) !== 4) return 0;
    const segX2 = buf.readUInt16BE(sub + 6), ends = sub + 14, starts = ends + segX2 + 2;
    const deltas = starts + segX2, ranges = deltas + segX2;
    for (let s = 0; s < segX2; s += 2) {
      if (c > buf.readUInt16BE(ends + s)) continue;
      const start = buf.readUInt16BE(starts + s);
      if (c < start) return 0;
      const delta = buf.readInt16BE(deltas + s), ro = buf.readUInt16BE(ranges + s);
      if (!ro) return (c + delta) & 0xffff;
      const g = buf.readUInt16BE(ranges + s + ro + (c - start) * 2);
      return g ? (g + delta) & 0xffff : 0;
    }
    return 0;
  };
  const locaAt = i => (indexToLoc ? buf.readUInt32BE(loca + i * 4) : buf.readUInt16BE(loca + i * 2) * 2);
  const advance = g => buf.readUInt16BE(hmtx + Math.min(g, numHMetrics - 1) * 4);
  // Glyph outline as contours of {x, y, on} in font units (composites flattened, offsets only).
  function outline(g, depth = 0) {
    const o = glyf + locaAt(g), len = locaAt(g + 1) - locaAt(g);
    if (len <= 0 || depth > 8) return [];
    const nc = buf.readInt16BE(o);
    if (nc < 0) {
      let p = o + 10, res = [], more = true;
      while (more) {
        const flags = buf.readUInt16BE(p), gi = buf.readUInt16BE(p + 2); p += 4;
        let dx, dy;
        if (flags & 1) { dx = buf.readInt16BE(p); dy = buf.readInt16BE(p + 2); p += 4; }
        else { dx = buf.readInt8(p); dy = buf.readInt8(p + 1); p += 2; }
        let a = 1, b = 0, c2 = 0, d = 1;
        if (flags & 8) { a = d = buf.readInt16BE(p) / 16384; p += 2; }
        else if (flags & 0x40) { a = buf.readInt16BE(p) / 16384; d = buf.readInt16BE(p + 2) / 16384; p += 4; }
        else if (flags & 0x80) { a = buf.readInt16BE(p) / 16384; b = buf.readInt16BE(p + 2) / 16384; c2 = buf.readInt16BE(p + 4) / 16384; d = buf.readInt16BE(p + 6) / 16384; p += 8; }
        for (const ct of outline(gi, depth + 1)) res.push(ct.map(q => ({ x: a * q.x + c2 * q.y + dx, y: b * q.x + d * q.y + dy, on: q.on })));
        more = !!(flags & 0x20);
      }
      return res;
    }
    const endPts = []; for (let i = 0; i < nc; i++) endPts.push(buf.readUInt16BE(o + 10 + i * 2));
    const np = endPts.at(-1) + 1, insLen = buf.readUInt16BE(o + 10 + nc * 2);
    let p = o + 12 + nc * 2 + insLen;
    const fl = [];
    while (fl.length < np) { const f = buf[p++]; fl.push(f); if (f & 8) { let r = buf[p++]; while (r--) fl.push(f); } }
    const xs = [], ys = [];
    let v = 0;
    for (const f of fl) { if (f & 2) { const d = buf[p++]; v += f & 16 ? d : -d; } else if (!(f & 16)) { v += buf.readInt16BE(p); p += 2; } xs.push(v); }
    v = 0;
    for (const f of fl) { if (f & 4) { const d = buf[p++]; v += f & 32 ? d : -d; } else if (!(f & 32)) { v += buf.readInt16BE(p); p += 2; } ys.push(v); }
    const res = []; let s = 0;
    for (const e of endPts) { const ct = []; for (let i = s; i <= e; i++) ct.push({ x: xs[i], y: ys[i], on: !!(fl[i] & 1) }); res.push(ct); s = e + 1; }
    return res;
  }
  return { unitsPerEm, headFlags, ascender, descender, lineGap, advanceWidthMax, numGlyphs, glyphIndex, advance, outline };
}

// FreeType FT_Set_Pixel_Sizes(size, size) scaled metrics for a TrueType face (26.6 -> integer pixels).
export function faceMetrics(f, size) {
  const s = size / f.unitsPerEm;
  // TrueType faces with head.flags bit 3 (integer ppem) round ascender/descender; others ceil/floor.
  const intPpem = (f.headFlags & 8) !== 0;
  const asc = intPpem ? Math.round(f.ascender * s) : Math.ceil(f.ascender * s);
  const desc = intPpem ? Math.round(f.descender * s) : Math.floor(f.descender * s);
  const height = Math.round((f.ascender - f.descender + f.lineGap) * s);
  // FT::FontFace::SetupFace: LineHeight = height, FontHeight = ascent - descent, Ascent, Descent, MaxHorizontalAdvance
  return { LineHeight: height, FontHeight: asc - desc, Ascent: asc, Descent: desc, MaxAdvance: Math.round(f.advanceWidthMax * s) };
}

// Rasterize one glyph: returns { w, h, left, top, cov (Uint8Array), advance } with left/top like FT bitmap_left/top.
export function rasterGlyph(f, size, code) {
  const g = f.glyphIndex(code), s = size / f.unitsPerEm;
  const advance = Math.round(f.advance(g) * s); // hinted advances are whole pixels
  const polys = [];
  for (const ct of f.outline(g)) { // flatten quadratic splines
    const pts = [], n = ct.length;
    if (!n) continue;
    let startIdx = ct.findIndex(q => q.on);
    const P = i => ct[(i + n) % n];
    let start;
    if (startIdx < 0) { start = { x: (ct[0].x + ct[1 % n].x) / 2, y: (ct[0].y + ct[1 % n].y) / 2 }; startIdx = 0; }
    else start = ct[startIdx];
    pts.push([start.x * s, start.y * s]);
    let prev = start;
    for (let k = 1; k <= n; k++) {
      const q = P(startIdx + k);
      if (q.on) { pts.push([q.x * s, q.y * s]); prev = q; continue; }
      const nx = P(startIdx + k + 1);
      const end = nx.on ? nx : { x: (q.x + nx.x) / 2, y: (q.y + nx.y) / 2, on: true };
      for (let t = 1; t <= 8; t++) {
        const u = t / 8, a = (1 - u) * (1 - u), b = 2 * u * (1 - u), c = u * u;
        pts.push([(a * prev.x + b * q.x + c * end.x) * s, (a * prev.y + b * q.y + c * end.y) * s]);
      }
      prev = end;
      if (nx.on) k++;
    }
    polys.push(pts);
  }
  let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
  for (const p of polys) for (const [x, y] of p) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
  if (!polys.length) return { w: 0, h: 0, left: 0, top: 0, cov: new Uint8Array(0), advance, metricsWidth: 0 };
  const left = Math.floor(x0), top = Math.ceil(y1), w = Math.ceil(x1) - left, h = top - Math.floor(y0);
  const cov = new Uint8Array(w * h), SS = 4;
  for (let py = 0; py < h; py++) for (let sy = 0; sy < SS; sy++) {
    const y = top - py - (sy + 0.5) / SS, xs = [];
    for (const p of polys) for (let i = 0; i < p.length; i++) {
      const [ax, ay] = p[i], [bx, by] = p[(i + 1) % p.length];
      if ((ay <= y) !== (by <= y)) xs.push({ x: ax + ((y - ay) / (by - ay)) * (bx - ax), d: by > ay ? 1 : -1 });
    }
    xs.sort((a, b) => a.x - b.x);
    let wind = 0;
    for (let i = 0; i < xs.length - 1; i++) { // non-zero winding spans, horizontal coverage exact per pixel
      wind += xs[i].d;
      if (!wind) continue;
      const a = xs[i].x - left, b = xs[i + 1].x - left;
      for (let px = Math.max(0, Math.floor(a)); px < Math.min(w, Math.ceil(b)); px++) {
        const c = Math.min(b, px + 1) - Math.max(a, px);
        if (c > 0) cov[py * w + px] = Math.min(255, cov[py * w + px] + Math.round((c * 255) / SS));
      }
    }
  }
  return { w, h, left, top, cov, advance, metricsWidth: Math.trunc(x1 - x0) };
}

// ------------------------------------------------------------------ fonts.xml
const style2bits = s => [...'BIUSOA'].reduce((m, ch, i) => (s.toUpperCase().includes(ch) ? m | [1, 2, 4, 8, 16, 32][i] : m), 0);
function fontsXml() {
  let fn = path.join(install, 'Data', `fonts.xml${lang}`);
  if (!fs.existsSync(fn)) fn = path.join(install, 'Data', 'fonts.xml');
  const root = parseXML(fs.readFileSync(fn, 'utf8')).children[0];
  const fonts = root.children.find(c => c.name === 'FONTS');
  // FontFamily::init @00521330 takes the CODEPAGE named "default" (compared like String==, case-insensitive here).
  const cp = fonts.children.find(c => c.name === 'CODEPAGE' && c.attrs.name?.toLowerCase() === 'default');
  // RESOLUTION with the smallest |res - screen width|; ties keep the earlier element.
  let best = null, bd = Infinity;
  for (const r of cp.children.filter(c => c.name === 'RESOLUTION')) {
    const d = Math.abs(+r.attrs.res - screenW);
    if (d < bd) { bd = d; best = r; }
  }
  const list = best.children.filter(c => c.name === 'FONT').map(c => ({
    file: c.attrs.filename.replace(/\\/g, '/'), name: c.attrs.name, size: +c.attrs.size,
    texSize: +(c.attrs.tex_size ?? c.attrs.size), style: c.attrs.style ?? '', bits: style2bits(c.attrs.style ?? ''),
  }));
  const lookups = (root.children.find(c => c.name === 'LOOKUPS')?.children ?? []).map(l => ({
    tag: l.attrs.tag, res: l.children.map(r => `${r.attrs.res}:${r.attrs.size}${r.attrs.style ? r.attrs.style : ''}`).join(' '),
  }));
  return { res: +best.attrs.res, list, lookups, ea: +(cp.attrs.use_ea_line_breaking ?? 0) };
}
const ttfCache = new Map();
function loadFace(file) {
  if (!ttfCache.has(file)) {
    const dir = path.join(install, path.dirname(file)), base = path.basename(file).toLowerCase();
    const hit = fs.readdirSync(dir).find(n => n.toLowerCase() === base);
    ttfCache.set(file, parseTTF(fs.readFileSync(path.join(dir, hit))));
  }
  return ttfCache.get(file);
}

// ------------------------------------------------------------------ layout + PNG
function layout(face, size, bits, str) {
  const m = faceMetrics(face, size), glyphs = [];
  let x = 0;
  for (const ch of str) {
    const code = ch.codePointAt(0);
    if (code < 0x20) { glyphs.push(null); continue; } // C1_CNTRL characters get an empty cell
    const g = rasterGlyph(face, size, code);
    // D3D11GLFont::get_character_info: advance = (advance.x >> 6) + bitmap_left (0 after FT_Load_Glyph),
    // pix_width = metrics.width >> 6, pix_height = FontHeight; outline (O) adds 1/2/2. The bitmap is copied to the
    // cell's left edge (bitmap_left dropped), row = Ascent - bitmap_top. The quad sits at the pen position.
    const adv = g.advance + (bits & 16 ? 1 : 0);
    glyphs.push({ g, x, adv });
    x += adv;
  }
  return { m, glyphs, width: x };
}
function png(w, h, rgba) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4, 'latin1'); d.copy(b, 8); b.writeUInt32BE(zlib.crc32(b.subarray(4, 8 + d.length)) >>> 0, 8 + d.length); return b; };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 6;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const hex = s => [parseInt(s.slice(0, 2), 16), parseInt(s.slice(2, 4), 16), parseInt(s.slice(4, 6), 16)];
function drawLine(img, W, lay, ox, oy, rgb, bits) {
  for (const it of lay.glyphs) {
    if (!it) continue;
    const { g } = it, row0 = lay.m.Ascent - g.top;
    for (let y = 0; y < g.h; y++) for (let x = 0; x < g.w; x++) {
      const c = g.cov[y * g.w + x];
      // text render state: alpha test GREATEREQUAL 8, SRCALPHA/INVSRCALPHA, colour = vertex colour (texel RGB white)
      if (c < 8) continue;
      const X = ox + it.x + x, Y = oy + row0 + y;
      if (X < 0 || Y < 0 || X >= W || Y * W * 4 >= img.length) continue;
      const d = (Y * W + X) * 4, a = c / 255;
      for (let k = 0; k < 3; k++) img[d + k] = Math.round(rgb[k] * a + img[d + k] * (1 - a));
    }
    if (bits & 8) { // S: 2-pixel white bar across the middle of the cell (FontRenderManager::render_pendings)
      const yMid = oy + Math.trunc(lay.m.FontHeight / 2);
      for (let y = yMid; y < yMid + 2; y++) for (let x = 0; x < it.adv; x++) {
        const d = (y * W + ox + it.x + x) * 4; if (d >= 0 && d < img.length) { img[d] = rgb[0]; img[d + 1] = rgb[1]; img[d + 2] = rgb[2]; }
      }
    }
  }
}

const fx = fontsXml();
if (cmd === 'list') {
  console.log(`fonts.xml${lang}: CODEPAGE default, RESOLUTION ${fx.res} (screen width ${screenW}), use_ea_line_breaking=${fx.ea}`);
  for (const f of fx.list) {
    const m = faceMetrics(loadFace(f.file), f.size);
    console.log(`${f.name.padEnd(8)} ${String(f.size).padStart(2)}${f.texSize !== f.size ? `/${f.texSize}` : ''} ${(f.style || '-').padEnd(4)} bits=0x${f.bits.toString(16).padStart(2, '0')} ${f.file.padEnd(24)} line=${m.LineHeight} fontH=${m.FontHeight} asc=${m.Ascent} desc=${m.Descent} maxAdv=${m.MaxAdvance}`);
  }
  console.log('LOOKUPS (tag -> res:size[style]):');
  for (const l of fx.lookups) console.log(`  ${l.tag.padEnd(20)} ${l.res}`);
} else if (cmd === 'render' || cmd === 'sample') {
  const out = O.out ?? `re/previews/font_${cmd}.png`;
  const bg = hex(O.bg), shadow = O.shadow, sx = O.sx, sy = O.sy;
  const want = { name: O.font, size: O.size, style: O.style };
  const lines = cmd === 'render'
    ? [{ f: fx.list.find(f => f.name === want.name && f.size === want.size && f.bits === style2bits(want.style)) ?? { file: 'Data/Fonts/arial.ttf', ...want, bits: style2bits(want.style) }, s: text }]
    : fx.list.map(f => ({ f, s: `${f.name} ${f.size} ${f.style || '-'}: ${text}` }));
  const lays = lines.map(({ f, s }) => ({ f, lay: layout(loadFace(f.file), f.size, f.bits, s) }));
  const W = Math.max(...lays.map(l => l.lay.width)) + 8, H = lays.reduce((a, l) => a + l.lay.m.LineHeight + 2, 8);
  const img = new Uint8Array(W * H * 4);
  for (let i = 0; i < W * H; i++) { img[i * 4] = bg[0]; img[i * 4 + 1] = bg[1]; img[i * 4 + 2] = bg[2]; img[i * 4 + 3] = 255; }
  let y = 4;
  for (const { f, lay } of lays) {
    if (shadow) drawLine(img, W, lay, 4 + sx, y + sy, hex(shadow), f.bits); // Buffer text_shadow + shadow_x/y_off
    drawLine(img, W, lay, 4, y, hex(O.color), f.bits);
    console.log(`${f.name} ${f.size} ${f.style || '-'}: width ${lay.width}px, ${lay.glyphs.length} glyphs, line ${lay.m.LineHeight}, cell ${lay.m.FontHeight}`);
    y += lay.m.LineHeight + 2;
  }
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, png(W, H, img));
  console.log(`wrote ${out} (${W}x${H})`);
} else {
  console.error('usage: fontpreview.mjs list|render|sample');
  process.exit(2);
}
