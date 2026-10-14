// Interface layout reader for Data/interface.xml (and the other GraphicWinXML files: topmenu.xml, setupwin.xml, ...).
// Proves the layout format described in docs/ui-spec.md §2: element tree, rect resolution against the parent size
// (XMLElement::get_attrib_rect_ex @00a25ba0), IFACETEX atlas pieces, IFACEBORDER 16-slot borders, IFACETEXLOAD ids.
//
//   node re/tools/ifacelayout.mjs tree [NODE] [--w 1024 --h 768] [--depth N] [--file Data/interface.xml] [--all]
//        Print the element tree. Window-like nodes and IFACERECTs show their rect resolved against the parent size
//        (windows nest: a child window resolves against its parent window's size). --all also lists IFACETEX pieces.
//   node re/tools/ifacelayout.mjs texloads            List IFACETEXLOAD ids -> files (IFaceRenderManager::init).
//   node re/tools/ifacelayout.mjs render NODE [--w 1024 --h 768] [--out re/previews/iface_NODE.png] [--skin AS0]
//        Composite NODE with the generic GraphicWinXML drawing rules (§2.6): "back" texture tiled into the window,
//        IFACEBORDER "main" around it, and every IFACERECT that has a same-named IFACETEX (stretched; *_tile tiled).
//        Panel-specific drawing done in code (resources, minimap, buttons) is not reproduced.
//   node re/tools/ifacelayout.mjs sheet NODE [--out ...]   Contact sheet of every IFACETEX piece under NODE.
//
// Options: --install DIR (default ~/Games/RiseOfNations). Reads only the user's install; writes only --out.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { pathToFileURL } from 'node:url';
import { parseTGA } from './ronassets.mjs';

const argv = process.argv.slice(2);
const opt = (name, def) => { const i = argv.indexOf(`--${name}`); return i >= 0 ? argv.splice(i, 2)[1] : def; };
const flag = name => { const i = argv.indexOf(`--${name}`); if (i >= 0) argv.splice(i, 1); return i >= 0; };
const install = opt('install', path.join(os.homedir(), 'Games/RiseOfNations'));
const file = opt('file', 'Data/interface.xml');
const W = +opt('w', 1024), H = +opt('h', 768), maxDepth = +opt('depth', 99);
const skin = opt('skin', 'AS0');
let out = opt('out', null);
const all = flag('all');
const [cmd = 'tree', nodeName] = argv;

// ------------------------------------------------------------------ minimal XML parser (elements + attributes)
export function parseXML(src) {
  src = src.replace(/<!--[\s\S]*?-->/g, '').replace(/<\?[\s\S]*?\?>/g, '').replace(/<!DOCTYPE[^>]*>/g, '');
  const root = { name: '#root', attrs: {}, children: [] };
  const stack = [root];
  const re = /<(\/?)([A-Za-z_][\w.-]*)((?:\s+[\w:.-]+\s*=\s*(?:"[^"]*"|'[^']*'))*)\s*(\/?)>/g;
  let m;
  while ((m = re.exec(src))) {
    const [, close, name, attrSrc, self] = m;
    if (close) { stack.pop(); continue; }
    const attrs = {};
    for (const a of attrSrc.matchAll(/([\w:.-]+)\s*=\s*(?:"([^"]*)"|'([^']*)')/g)) attrs[a[1]] = (a[2] ?? a[3]).trim();
    const el = { name, attrs, children: [] };
    stack.at(-1).children.push(el);
    if (!self) stack.push(el);
  }
  return root;
}

// ------------------------------------------------------------------ rect resolution (get_attrib_rect_ex)
// Returns {l, r, t, b} like RectTemplate<short> {x1, x2, y1, y2}. pw/ph = parent width/height.
// Attribute lookups are case-insensitive in MSXML? No: names are exact; numbers parse with atoi semantics.
const num = (a, k) => (k in a ? parseInt(a[k], 10) || 0 : undefined);
const per = (v, p) => Math.trunc((v * p) / 1000);
export function resolveRect(a, pw, ph) {
  const w0 = num(a, 'width') ?? 0, h0 = num(a, 'height') ?? 0;
  const pwid = w0 === 0 ? (num(a, 'percent_width') ?? 0) : 0;
  const phei = h0 === 0 ? (num(a, 'percent_height') ?? 0) : 0;
  const w = w0 !== 0 ? w0 : per(pw - (num(a, 'x_percent_diff') ?? 0), pwid);
  const h = h0 !== 0 ? h0 : per(ph - (num(a, 'y_percent_diff') ?? 0), phei);
  let l, r, t, b;
  const first = (...ks) => { for (const k of ks) if (k in a) return [k, num(a, k)]; return [null, 0]; };
  if (w0 > 0 || pwid > 0) { // positive size: anchor the left edge
    const [k, v] = first('left', 'left_inverse', 'left_inverse_percent', 'left_percent', 'center_off_x');
    l = k === 'left' ? v : k === 'left_inverse' ? pw - v : k === 'left_inverse_percent' ? pw - per(v, pw)
      : k === 'left_percent' ? per(v, pw) : k === 'center_off_x' ? v + (Math.trunc(pw / 2) - Math.trunc(w / 2)) : 0;
    r = l + w;
  } else if (w0 < 0 || pwid < 0) { // negative size: anchor the right edge, extend left by |w|
    const [k, v] = first('right', 'right_inverse', 'right_inverse_percent', 'right_percent', 'center_off_x');
    r = k === 'right' ? v : k === 'right_inverse' ? pw - v : k === 'right_inverse_percent' ? pw - per(v, pw)
      : k === 'right_percent' ? per(v, pw) : k === 'center_off_x' ? v + (Math.trunc(pw / 2) - Math.trunc(w / 2)) : 0;
    l = r + w;
  } else { // no size: both edges explicit
    const [kl, vl] = first('left', 'left_inverse', 'left_inverse_percent', 'left_percent');
    l = kl === 'left' ? vl : kl === 'left_inverse' ? pw - vl : kl === 'left_inverse_percent' ? pw - per(vl, pw)
      : kl === 'left_percent' ? per(vl, pw) : 0;
    const [kr, vr] = first('right', 'right_inverse', 'right_inverse_percent', 'right_percent');
    r = kr === 'right' ? vr : kr === 'right_inverse' ? pw - vr : kr === 'right_inverse_percent' ? pw - per(vr, pw)
      : kr === 'right_percent' ? per(vr, pw) : 0;
  }
  if (h0 > 0 || phei > 0) {
    const [k, v] = first('top', 'top_inverse', 'top_inverse_percent', 'top_percent', 'center_off_y');
    t = k === 'top' ? v : k === 'top_inverse' ? ph - v : k === 'top_inverse_percent' ? ph - per(v, ph)
      : k === 'top_percent' ? per(v, ph) : k === 'center_off_y' ? v + Math.trunc((ph - h) / 2) : 0;
    b = t + h;
  } else if (h0 < 0 || phei < 0) {
    const [k, v] = first('bottom', 'bottom_inverse', 'bottom_inverse_percent', 'bottom_percent', 'center_off_y');
    // center_off_y with a negative height reproduces the original's arithmetic (v - (ph - h) / 2).
    b = k === 'bottom' ? v : k === 'bottom_inverse' ? ph - v : k === 'bottom_inverse_percent' ? ph - per(v, ph)
      : k === 'bottom_percent' ? per(v, ph) : k === 'center_off_y' ? v - Math.trunc((ph - h) / 2) : 0;
    t = b + h;
  } else {
    const [kt, vt] = first('top', 'top_inverse', 'top_inverse_percent', 'top_percent');
    t = kt === 'top' ? vt : kt === 'top_inverse' ? ph - vt : kt === 'top_inverse_percent' ? ph - per(vt, ph)
      : kt === 'top_percent' ? per(vt, ph) : 0;
    const [kb, vb] = first('bottom', 'bottom_inverse', 'bottom_inverse_percent', 'bottom_percent');
    b = kb === 'bottom' ? vb : kb === 'bottom_inverse' ? ph - vb : kb === 'bottom_inverse_percent' ? ph - per(vb, ph)
      : kb === 'bottom_percent' ? per(vb, ph) : 0;
  }
  const s16 = v => ((v << 16) >> 16); // RectTemplate<short>
  return { l: s16(l), r: s16(r), t: s16(t), b: s16(b) };
}

// Elements that become child windows (GraphicWinXML::process_element) or that every *Win class creates from a
// tag of its own name. Anything with a rect attribute and children is treated as a window for display purposes.
const PIECES = new Set(['IFACETEX', 'IFACETEXLOAD', 'IFACEBORDER', 'IFACECOLOR', 'IFACEPOINT', 'IFACESPRITE',
  'STRETCHSPRITE', 'FONT', 'FONTCOLOR', 'STYLE', 'CATEGORY', 'CATEGORIES', 'PARAMS', 'CHECK']);
const RECTKEYS = /^(left|right|top|bottom|width|height|percent_|center_off|.*_inverse|.*_percent)/;
const hasRect = a => Object.keys(a).some(k => RECTKEYS.test(k));
const fmt = r => `(${r.l},${r.t})-(${r.r},${r.b}) ${r.r - r.l}x${r.b - r.t}`;

const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href;
const top = isMain ? parseXML(fs.readFileSync(path.join(install, file), 'utf8')).children[0] : null; // ROOT
function find(el, name) {
  if (el.name === name || el.attrs.name === name) return el;
  for (const c of el.children) { const f = find(c, name); if (f) return f; }
  return null;
}

// POSITIONING (GraphicWinXML::add_elements @00516700): child i of the group is shifted by
// (x + i*inc_x, y + i*inc_y); *_percent variants are per mille of the window size; groups nest.
function positioning(a, pw, ph) {
  const v = (k, P) => (k in a ? +a[k] : `${k}_percent` in a ? Math.trunc((+a[`${k}_percent`] * P) / 1000) : 0);
  return { x: v('x', pw), y: v('y', ph), ix: v('inc_x', pw), iy: v('inc_y', ph) };
}
function printTree(el, pw, ph, depth, ox = 0, oy = 0, px = 0, py = 0) {
  if (depth > maxDepth) return;
  const ind = '  '.repeat(depth);
  const a = el.attrs, nm = a.name ? ` "${a.name}"` : a.id ? ` id=${a.id}` : '';
  if (el.name === 'IFACETEX') {
    if (all) console.log(`${ind}IFACETEX${nm} ${a.texture} [${a.left},${a.top} ${a.width}x${a.height}]`);
    return;
  }
  if (PIECES.has(el.name) && el.name !== 'IFACEBORDER' && el.name !== 'IFACEPOINT') {
    if (all || el.name === 'STYLE' || el.name === 'IFACETEXLOAD')
      console.log(`${ind}${el.name}${nm} ${Object.entries(a).filter(([k]) => k !== 'name' && k !== 'id').map(([k, v]) => `${k}=${v}`).join(' ')}`);
    return;
  }
  if (el.name === 'IFACEBORDER') {
    const slots = el.children.filter(c => c.name === 'IFACETEX').map(c => c.attrs.name).join(',');
    console.log(`${ind}IFACEBORDER${nm} [${slots}]`);
    return;
  }
  if (el.name === 'IFACEPOINT') { console.log(`${ind}IFACEPOINT${nm} ${JSON.stringify(a)}`); return; }
  if (el.name === 'POSITIONING') {
    const p = positioning(a, pw, ph);
    console.log(`${ind}POSITIONING ${Object.entries(a).map(([k, v]) => `${k}=${v}`).join(' ')}`);
    let i = 0;
    for (const c of el.children) {
      const shift = !PIECES.has(c.name) || c.name === 'POSITIONING';
      printTree(c, pw, ph, depth + 1, ox, oy, px + p.x + i * p.ix, py + p.y + i * p.iy);
      if (shift) i++;
    }
    return;
  }
  if (hasRect(a)) {
    const r0 = resolveRect(a, pw, ph), r = { l: r0.l + px, r: r0.r + px, t: r0.t + py, b: r0.b + py };
    const abs = { l: r.l + ox, r: r.r + ox, t: r.t + oy, b: r.b + oy };
    console.log(`${ind}${el.name}${nm} ${fmt(r)}  screen ${fmt(abs)}${a.show === '0' ? ' hidden' : ''}`);
    const isWin = el.name !== 'IFACERECT';
    for (const c of el.children) printTree(c, isWin ? r.r - r.l : pw, isWin ? r.b - r.t : ph, depth + 1,
      isWin ? abs.l : ox, isWin ? abs.t : oy);
  } else {
    console.log(`${ind}${el.name}${nm}`);
    for (const c of el.children) printTree(c, pw, ph, depth + 1, ox, oy, px, py);
  }
}

// ------------------------------------------------------------------ textures
function texLoads() {
  const map = new Map();
  const walk = el => {
    if (el.name.toUpperCase() === 'IFACETEXLOAD') map.set(el.attrs.id, el.attrs.file.replace(/\\/g, '/').trim());
    el.children.forEach(walk);
  };
  walk(top);
  return map;
}
const texCache = new Map();
function loadTex(id, loads) {
  if (texCache.has(id)) return texCache.get(id);
  let f = loads.get(id);
  if (!f) { texCache.set(id, null); return null; }
  // IFaceMainBase::age_update swaps TEX_CIV / TEX_LIBRARY_BACK for the player's skin (§2.4); AS0 is the default.
  f = f.replace(/AS0/, skin);
  const dir = path.join(install, path.dirname(f));
  const base = path.basename(f).toLowerCase();
  const hit = fs.readdirSync(dir).find(n => n.toLowerCase() === base);
  const t = hit ? parseTGA(fs.readFileSync(path.join(dir, hit))) : null;
  texCache.set(id, t);
  return t;
}

// ------------------------------------------------------------------ software compositor
function canvas(w, h) { return { w, h, px: new Uint8Array(w * h * 4) }; }
function blit(dst, tex, sx, sy, sw, sh, dx, dy, dw = sw, dh = sh) {
  if (!tex) return;
  for (let y = 0; y < dh; y++) for (let x = 0; x < dw; x++) {
    const X = dx + x, Y = dy + y;
    if (X < 0 || Y < 0 || X >= dst.w || Y >= dst.h) continue;
    const u = sx + Math.min(sw - 1, Math.floor((x * sw) / dw)), v = sy + Math.min(sh - 1, Math.floor((y * sh) / dh));
    if (u < 0 || v < 0 || u >= tex.width || v >= tex.height) continue;
    const s = (v * tex.width + u) * 4, d = (Y * dst.w + X) * 4, a = tex.rgba[s + 3] / 255;
    if (a === 0) continue; // the UI states alpha-test GREATER 0 (§5.2)
    for (let k = 0; k < 3; k++) dst.px[d + k] = Math.round(tex.rgba[s + k] * a + dst.px[d + k] * (1 - a));
    dst.px[d + 3] = Math.max(dst.px[d + 3], tex.rgba[s + 3]);
  }
}
function tile(dst, tex, p, x0, y0, x1, y1) { // IFaceRenderManager::tile_* : repeat the piece, clip the last one
  const pw = +p.width, ph = +p.height;
  if (!(pw > 0 && ph > 0)) return;
  for (let y = y0; y < y1; y += ph) for (let x = x0; x < x1; x += pw)
    blit(dst, tex, +p.left, +p.top, Math.min(pw, x1 - x), Math.min(ph, y1 - y), x, y);
}
function border(dst, b, r, loads) { // IFaceRenderManager::draw_border @0051d260
  const s = {};
  for (const c of b.children) if (c.name === 'IFACETEX') s[c.attrs.name.toLowerCase()] = c.attrs;
  const T = p => p && loadTex(p.texture.trim(), loads), wd = p => (p ? +p.width : 0), ht = p => (p ? +p.height : 0);
  const [c0, c1, c2, c3] = [0, 1, 2, 3].map(i => s[`corner${i}`]);
  const [e0, e1, e2, e3] = [0, 1, 2, 3].map(i => s[`edge${i}`]);
  if (e0) tile(dst, T(e0), e0, r.l + wd(c0), r.t, r.r - wd(c1), r.t + ht(e0));
  if (e2) tile(dst, T(e2), e2, r.l + wd(c3), r.b - ht(e2), r.r - wd(c2), r.b);
  if (e3) tile(dst, T(e3), e3, r.l, r.t + ht(c0), r.l + wd(e3), r.b - ht(c3));
  if (e1) tile(dst, T(e1), e1, r.r - wd(e1), r.t + ht(c1), r.r, r.b - ht(c2));
  if (c0) blit(dst, T(c0), +c0.left, +c0.top, wd(c0), ht(c0), r.l, r.t);
  if (c1) blit(dst, T(c1), +c1.left, +c1.top, wd(c1), ht(c1), r.r - wd(c1), r.t);
  if (c2) blit(dst, T(c2), +c2.left, +c2.top, wd(c2), ht(c2), r.r - wd(c2), r.b - ht(c2));
  if (c3) blit(dst, T(c3), +c3.left, +c3.top, wd(c3), ht(c3), r.l, r.b - ht(c3));
}
function writePNG(c, file) {
  const raw = Buffer.alloc((c.w * 4 + 1) * c.h);
  for (let y = 0; y < c.h; y++) { raw[y * (c.w * 4 + 1)] = 0; Buffer.from(c.px.buffer, y * c.w * 4, c.w * 4).copy(raw, y * (c.w * 4 + 1) + 1); }
  const chunk = (t, d) => { const b = Buffer.alloc(12 + d.length); b.writeUInt32BE(d.length, 0); b.write(t, 4, 'latin1'); d.copy(b, 8); b.writeUInt32BE(zlib.crc32(b.subarray(4, 8 + d.length)) >>> 0, 8 + d.length); return b; };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(c.w, 0); ih.writeUInt32BE(c.h, 4); ih[8] = 8; ih[9] = 6;
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
  console.log(`wrote ${file} (${c.w}x${c.h})`);
}

function renderNode(el) {
  const loads = texLoads();
  const r = hasRect(el.attrs) ? resolveRect(el.attrs, W, H) : { l: 0, r: W, t: 0, b: H };
  const w = r.r - r.l, h = r.b - r.t, c = canvas(w, h);
  const kids = el.children;
  const texs = kids.filter(k => k.name === 'IFACETEX'), rects = kids.filter(k => k.name === 'IFACERECT');
  const back = texs.find(t => t.attrs.name === 'back');
  if (back) tile(c, loadTex(back.attrs.texture.trim(), loads), back.attrs, 0, 0, w, h);
  const used = new Map();
  for (const rc of rects) { // nth IFACERECT pairs with the nth same-named IFACETEX (find_tex/find_rect iteration)
    const n = rc.attrs.name, i = used.get(n) ?? 0; used.set(n, i + 1);
    const tx = texs.filter(t => t.attrs.name === n)[i];
    if (!tx) continue;
    const rr = resolveRect(rc.attrs, w, h), T = loadTex(tx.attrs.texture.trim(), loads);
    if (/_tile$/.test(n) || n.endsWith('tile')) tile(c, T, tx.attrs, rr.l, rr.t, rr.r, rr.b);
    else blit(c, T, +tx.attrs.left, +tx.attrs.top, +tx.attrs.width, +tx.attrs.height, rr.l, rr.t, rr.r - rr.l, rr.b - rr.t);
  }
  const main = kids.find(k => k.name === 'IFACEBORDER' && k.attrs.name === 'main');
  if (main) border(c, main, { l: 0, t: 0, r: w, b: h }, loads);
  for (const b of kids.filter(k => k.name === 'IFACEBORDER' && k.attrs.name !== 'main')) {
    const rr = rects.find(x => x.attrs.name === `${b.attrs.name}_border`);
    if (rr) border(c, b, resolveRect(rr.attrs, w, h), loads);
  }
  return c;
}

function sheet(el) {
  const loads = texLoads(), pieces = [];
  const walk = e => { if (e.name === 'IFACETEX') pieces.push(e.attrs); e.children.forEach(walk); };
  walk(el);
  const cell = 132, cols = 8, rows = Math.ceil(pieces.length / cols), c = canvas(cols * cell, rows * cell);
  pieces.forEach((p, i) => {
    const s = Math.min(1, 128 / Math.max(+p.width || 1, +p.height || 1));
    blit(c, loadTex(p.texture.trim(), loads), +p.left, +p.top, +p.width, +p.height,
      (i % cols) * cell + 2, Math.floor(i / cols) * cell + 2, Math.max(1, Math.round(+p.width * s)), Math.max(1, Math.round(+p.height * s)));
    console.log(`${i}\t${p.name}\t${p.texture.trim()} [${p.left},${p.top} ${p.width}x${p.height}]`);
  });
  return c;
}

if (!isMain) {
  // imported as a library (parseXML, resolveRect)
} else if (cmd === 'tree') {
  const el = nodeName ? find(top, nodeName) : top;
  if (!el) throw new Error(`no node ${nodeName}`);
  console.log(`# ${file} at ${W}x${H}`);
  printTree(el, W, H, 0);
} else if (cmd === 'texloads') {
  for (const [id, f] of texLoads()) console.log(`${id}\t${f}`);
} else if (cmd === 'render' || cmd === 'sheet') {
  const el = find(top, nodeName);
  if (!el) throw new Error(`no node ${nodeName}`);
  out ??= `re/previews/iface_${cmd}_${nodeName}.png`;
  writePNG(cmd === 'render' ? renderNode(el) : sheet(el), out);
} else {
  console.error('usage: ifacelayout.mjs tree|texloads|render|sheet [NODE] [--w W --h H] [--out f.png]');
  process.exit(2);
}
