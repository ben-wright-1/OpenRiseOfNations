// XML access and string parsing exactly as Rise of Nations' rules loader sees it (docs/rules-spec.md,
// "XML reader" and "Value parsers"). The game loads files through MSXML (DOMDocument2, validateOnParse off,
// preserveWhiteSpace off) wrapped by basic:xml.obj (XML, XMLNode, XMLElement) and parses values with
// basic:str.obj (String::convert_int, binary, hex, fraction, ...). Everything here mirrors those functions.
import fs from 'node:fs';
import path from 'node:path';

// ---------------------------------------------------------------- minimal DOM
const ENT = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" };
const decodeEnt = (s) => s.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|amp|lt|gt|quot|apos);/g, (m, e) =>
  e[0] === '#' ? String.fromCodePoint(e[1] === 'x' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10)) : ENT[e]);

export function parseXml(src) {
  if (src.charCodeAt(0) === 0xfeff) src = src.slice(1);
  const root = { tag: '#document', attrs: {}, children: [], parent: null };
  let cur = root, i = 0;
  const n = src.length;
  while (i < n) {
    const lt = src.indexOf('<', i);
    const textEnd = lt < 0 ? n : lt;
    if (textEnd > i) cur.children.push({ text: decodeEnt(src.slice(i, textEnd)) });
    if (lt < 0) break;
    if (src.startsWith('<!--', lt)) { const e = src.indexOf('-->', lt + 4); i = e + 3; continue; }
    if (src.startsWith('<![CDATA[', lt)) { const e = src.indexOf(']]>', lt + 9); cur.children.push({ text: src.slice(lt + 9, e), cdata: true }); i = e + 3; continue; }
    if (src.startsWith('<?', lt)) { i = src.indexOf('?>', lt + 2) + 2; continue; }
    if (src.startsWith('<!', lt)) { i = src.indexOf('>', lt + 2) + 1; continue; } // DOCTYPE (none in the rules files)
    if (src[lt + 1] === '/') {
      const e = src.indexOf('>', lt); const tag = src.slice(lt + 2, e).trim();
      if (cur.tag !== tag) throw new Error(`mismatched </${tag}> (open <${cur.tag}>) at ${lt}`);
      cur = cur.parent; i = e + 1; continue;
    }
    // start tag
    let j = lt + 1; while (j < n && !/[\s/>]/.test(src[j])) j++;
    const el = { tag: src.slice(lt + 1, j), attrs: {}, children: [], parent: cur };
    for (;;) {
      while (/\s/.test(src[j])) j++;
      if (src[j] === '/' && src[j + 1] === '>') { j += 2; cur.children.push(el); break; }
      if (src[j] === '>') { j += 1; cur.children.push(el); cur = el; break; }
      let k = j; while (!/[\s=]/.test(src[k])) k++;
      const an = src.slice(j, k); while (/\s/.test(src[k])) k++;
      if (src[k] !== '=') throw new Error(`bad attribute at ${k}`);
      k++; while (/\s/.test(src[k])) k++;
      const q = src[k]; const e = src.indexOf(q, k + 1);
      el.attrs[an] = decodeEnt(src.slice(k + 1, e)); j = e + 1;
    }
    i = j;
  }
  return root.children.find((c) => c.tag);
}

// Language-suffixed variants (name.xml.<suffix>) are preferred when present (XML::init @00a279e0, gLanguageSuffix):
// English has no suffix; German .7, Spanish .10, French .12, Italian .16 (the .4/.9/.17/.18 files are unused).
export function readXml(dataDir, name, { lang = null } = {}) {
  let file = path.join(dataDir, name);
  if (lang && fs.existsSync(file + '.' + lang)) file += '.' + lang;
  const raw = fs.readFileSync(file);
  const src = raw[0] === 0xff && raw[1] === 0xfe ? raw.toString('utf16le') : raw.toString('utf8');
  return parseXml(src);
}

const isWs = (c) => c === ' ' || c === '\t' || c === '\r' || c === '\n';
function preserves(el) { for (let e = el; e; e = e.parent) if (e.attrs && e.attrs['xml:space']) return e.attrs['xml:space'] === 'preserve'; return false; }
function rawText(el) { let s = ''; for (const c of el.children) s += c.tag ? rawText(c) : c.text; return s; }

// IXMLDOMNode::get_text with preserveWhiteSpace = false: descendant text and CDATA concatenated, leading and
// trailing white space removed (xml:space="preserve" keeps it).
export function text(el) {
  if (!el) return '';
  const s = rawText(el);
  if (preserves(el)) return s;
  let a = 0, b = s.length; while (a < b && isWs(s[a])) a++; while (b > a && isWs(s[b - 1])) b--;
  return s.slice(a, b);
}

// XMLNode::get_element(name, create, exact): selectSingleNode on the name, upper-cased unless exact.
// XPath child selection is case sensitive, so an element spelt in lower case in the data is never found.
export const child = (el, name, exact = false) => {
  if (!el) return null; const t = exact ? name : name.toUpperCase();
  return el.children.find((c) => c.tag === t) ?? null;
};
// XMLNode::get_elements(name): selectNodes, element nodes only, document order.
export const children = (el, name, exact = false) => {
  if (!el) return []; const t = exact ? name : name.toUpperCase();
  return el.children.filter((c) => c.tag === t);
};
// get_element(name, create = 1): a missing child is created (empty) and attached, as the game does.
export function childOrCreate(el, name) {
  let c = child(el, name); if (c) return c;
  c = { tag: name.toUpperCase(), attrs: {}, children: [], parent: el, created: true }; el.children.push(c); return c;
}
// XMLElement::get_text(name): text of the first child called name ('' when absent); no name = own text.
export const getText = (el, name) => (name ? text(child(el, name)) : text(el));
// XMLElement::get_text_num(name, def): def when the text is empty, else String::number (convert_int).
export const getTextNum = (el, name, def = -1) => { const s = getText(el, name); return s.length ? convertInt(s) : def; };
export const getAttrib = (el, name, def = '') => (el && el.attrs[name] !== undefined ? el.attrs[name] : def);
export const getAttribNum = (el, name, def = -1) => { const s = getAttrib(el, name); return s.length ? convertInt(s) : def; };

// ---------------------------------------------------------------- value parsers (basic:str.obj)
export const i32 = (x) => x | 0;
const isDigit = (c) => c >= '0' && c <= '9';
const iswspace = (c) => c === ' ' || (c >= '\t' && c <= '\r');
// MSVC _wtoi: skip white space, optional sign, decimal digits; saturates at INT_MIN/INT_MAX on overflow.
export function wtoi(s, p = 0) {
  while (p < s.length && iswspace(s[p])) p++;
  let neg = false; if (s[p] === '+' || s[p] === '-') { neg = s[p] === '-'; p++; }
  let v = 0; while (p < s.length && isDigit(s[p])) { v = v * 10 + (s.charCodeAt(p) - 48); p++; if (v > 2 ** 31) v = 2 ** 31; }
  v = neg ? -v : v; return v > 0x7fffffff ? 0x7fffffff : v;
}
// String::binary: from start, while '0'/'1': v = 2v + bit (int32 wrap). Most significant digit first.
export function binary(s, p = 0) { let v = 0; while (p < s.length && (s[p] === '0' || s[p] === '1')) { v = i32(v * 2 + (s.charCodeAt(p) - 48)); p++; } return v; }
// String::hex: while isxdigit: v = 16v + digit (int32 wrap).
export function hex(s, p = 0) {
  let v = 0;
  while (p < s.length && /[0-9a-fA-F]/.test(s[p])) { v = i32(Math.imul(v, 16) + parseInt(s[p], 16)); p++; }
  return v;
}
// String::convert_int: "0b..." binary, "0x..." hex, "0d..." decimal after the prefix, else _wtoi.
export function convertInt(s) {
  if (s[0] === '0') {
    const c = s[1];
    if (c === 'b' || c === 'B') return binary(s, 2);
    if (c === 'x' || c === 'X') return hex(s, 2);
    if (c === 'd' || c === 'D') return wtoi(s, 2);
  }
  return wtoi(s);
}
// C integer division (truncates toward zero) on int32 operands.
export const cdiv = (a, b) => i32(Math.trunc(a / b));
// String::fraction(mul): _wtoi(s) * mul / _wtoi(after '/'); no '/' -> denominator 1; zero denominator -> 0.
export function fraction(s, mul) {
  const a = wtoi(s); const k = s.indexOf('/'); const d = k < 0 ? 1 : wtoi(s, k + 1);
  return d === 0 ? 0 : cdiv(i32(Math.imul(a, mul)), d);
}
// _wcsicmp equality in the "C" locale (ASCII case folding).
export const iequals = (a, b) => a.length === b.length && a.replace(/[A-Z]/g, (c) => c.toLowerCase()) === b.replace(/[A-Z]/g, (c) => c.toLowerCase());
export const toUpper = (s) => s.replace(/[a-z]/g, (c) => c.toUpperCase());
export const toLower = (s) => s.replace(/[A-Z]/g, (c) => c.toLowerCase());

// arith.obj degrees_to_angle: 360 degrees = 2^32 (BAM), computed in pieces to avoid rounding drift.
export function degreesToAngle(d) {
  const r90 = d % 90, q90 = cdiv(d, 90), q45 = cdiv(r90, 45), r45 = r90 % 45;
  const q30 = cdiv(r45, 30), r30 = r45 % 30, q15 = cdiv(r30, 15), r15 = r30 % 15, q5 = cdiv(r15, 5);
  return i32(Math.imul(i32(Math.imul(q30, 0x71c71c7) + q5), 3) + Math.imul(q45 + q90 * 2, 0x20000000)
    + Math.imul(q15, 0xaaaaaaa) + Math.imul(r15, 0xb60b60));
}

// Resource letter after an amount (Type::load_cost, ObjectType::load_support/load_upgrade):
// t timber, g wealth, k knowledge, m metal, o oil, anything else (f, end of string) food.
export const RES_LETTER = { t: 1, g: 2, k: 3, m: 4, o: 5 };
// Tokenise "<int><letter>[...]/<int><letter>..." the way those three functions walk the string.
// Returns [{amount, res}] for tokens whose amount is non-zero.
export function costTokens(s) {
  const out = []; let p = 0;
  while (p < s.length) {
    const amount = convertInt(s.slice(p));
    if (amount !== 0) { let q = p; while (q < s.length && isDigit(s[q])) q++; out.push({ amount, res: RES_LETTER[s[q]] ?? 0, at: p }); }
    const k = s.indexOf('/', p); if (k < 0) break; p = k + 1;
  }
  return out;
}

// Letter-flag strings. OBJ_MASK (upper-cased): 'A'..'Z' -> bits 0..25, '1'..'9' -> bits 26..34 (mod 32),
// anything else ignored. FLAGS (lower-cased): '1'..'9' -> c - 0x17, every other character -> (c - 'a') & 31.
export function objMask(s) {
  let m = 0; for (const c of toUpper(s)) {
    const k = c.charCodeAt(0);
    if (k >= 0x31 && k <= 0x39) m |= 1 << ((k - 0x17) & 31);
    else if (k >= 0x41 && k <= 0x5a) m |= 1 << (k - 0x41);
  }
  return m | 0;
}
export function unitFlags(s) {
  let m = 0; for (const c of toLower(s)) {
    const k = c.charCodeAt(0);
    m |= 1 << ((k >= 0x31 && k <= 0x39 ? k - 0x17 : k - 0x61) & 31);
  }
  return m >>> 0;
}
