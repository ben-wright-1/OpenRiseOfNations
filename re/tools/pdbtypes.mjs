#!/usr/bin/env node
// Read class/struct layouts straight from a PDB's type stream (TPI), including base classes, which Ghidra's PDB
// import leaves out for most game classes (e.g. /rise.pdb/Unit has no components there).
//
// Usage:
//   node re/tools/pdbtypes.mjs [--pdb re/bin/rise.pdb] Class [Class...]         flattened layout, one line per field
//   node re/tools/pdbtypes.mjs --tree Class                                       nested layout (base classes shown as blocks)
//   node re/tools/pdbtypes.mjs --json out.json Class [Class...] | --json out.json --all-from file.txt
//   node re/tools/pdbtypes.mjs --enum Name
// Library: import { loadPdbTypes } from './pdbtypes.mjs'  →  { layout(name), flat(name), sizeOf(ti), typeName(ti), find(name) }
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const PRIM = {
	0x03: ['void', 0], 0x08: ['HRESULT', 4], 0x10: ['char', 1], 0x20: ['uchar', 1], 0x70: ['char', 1], 0x71: ['wchar_t', 2],
	0x68: ['int8', 1], 0x69: ['uint8', 1], 0x11: ['short', 2], 0x21: ['ushort', 2], 0x72: ['short', 2], 0x73: ['ushort', 2],
	0x12: ['long', 4], 0x22: ['ulong', 4], 0x74: ['int', 4], 0x75: ['uint', 4], 0x13: ['int64', 8], 0x23: ['uint64', 8],
	0x76: ['int64', 8], 0x77: ['uint64', 8], 0x40: ['float', 4], 0x41: ['double', 8], 0x42: ['long double', 10],
	0x30: ['bool', 1], 0x31: ['bool16', 2], 0x32: ['bool32', 4], 0x7a: ['char16_t', 2], 0x7b: ['char32_t', 4],
};

export function loadPdbTypes(pdbPath) {
	const buf = fs.readFileSync(pdbPath);
	const magic = buf.toString('latin1', 0, 24);
	if (!magic.startsWith('Microsoft C/C++ MSF 7.00')) throw new Error('not an MSF 7.00 PDB');
	const bs = buf.readUInt32LE(32), dirBytes = buf.readUInt32LE(44), mapAddr = buf.readUInt32LE(52);
	const nDirBlocks = Math.ceil(dirBytes / bs);
	const dir = Buffer.alloc(nDirBlocks * bs);
	for (let i = 0; i < nDirBlocks; i++) buf.copy(dir, i * bs, buf.readUInt32LE(mapAddr * bs + i * 4) * bs, buf.readUInt32LE(mapAddr * bs + i * 4) * bs + bs);
	const nStreams = dir.readUInt32LE(0);
	const sizes = [];
	for (let i = 0; i < nStreams; i++) sizes.push(dir.readUInt32LE(4 + i * 4));
	let p = 4 + nStreams * 4;
	const streamBlocks = sizes.map(s => {
		const n = s === 0xffffffff ? 0 : Math.ceil(s / bs);
		const r = [];
		for (let i = 0; i < n; i++) r.push(dir.readUInt32LE(p + i * 4));
		p += n * 4;
		return r;
	});
	const readStream = i => {
		const out = Buffer.alloc(sizes[i]);
		streamBlocks[i].forEach((b, k) => buf.copy(out, k * bs, b * bs, Math.min(b * bs + bs, b * bs + sizes[i] - k * bs)));
		return out;
	};
	const tpi = readStream(2);
	const hdrSize = tpi.readUInt32LE(4), tiBegin = tpi.readUInt32LE(8), tiEnd = tpi.readUInt32LE(12);
	const recs = new Array(tiEnd - tiBegin);
	let off = hdrSize;
	for (let ti = tiBegin; ti < tiEnd; ti++) {
		const len = tpi.readUInt16LE(off);
		recs[ti - tiBegin] = { kind: tpi.readUInt16LE(off + 2), d: tpi.subarray(off + 4, off + 2 + len) };
		off += 2 + len;
	}
	const rec = ti => recs[ti - tiBegin];

	function numeric(d, o) {
		const v = d.readUInt16LE(o);
		if (v < 0x8000) return [v, o + 2];
		switch (v) {
			case 0x8000: return [d.readInt8(o + 2), o + 3];
			case 0x8001: return [d.readInt16LE(o + 2), o + 4];
			case 0x8002: return [d.readUInt16LE(o + 2), o + 4];
			case 0x8003: return [d.readInt32LE(o + 2), o + 6];
			case 0x8004: return [d.readUInt32LE(o + 2), o + 6];
			case 0x8009: return [Number(d.readBigInt64LE(o + 2)), o + 10];
			case 0x800a: return [Number(d.readBigUInt64LE(o + 2)), o + 10];
			default: throw new Error('numeric leaf 0x' + v.toString(16));
		}
	}
	function cstr(d, o) {
		let e = o;
		while (e < d.length && d[e] !== 0) e++;
		return [d.toString('utf8', o, e), e + 1];
	}

	const parsed = new Map();
	function parse(ti) {
		if (parsed.has(ti)) return parsed.get(ti);
		const r = rec(ti);
		if (!r) return null;
		const d = r.d;
		let t = { ti, kind: r.kind };
		switch (r.kind) {
			case 0x1001: t = { ...t, k: 'modifier', type: d.readUInt32LE(0), mods: d.readUInt16LE(4) }; break;
			case 0x1002: {
				const attr = d.readUInt32LE(4);
				t = { ...t, k: 'pointer', type: d.readUInt32LE(0), size: (attr >> 13) & 0x3f, mode: (attr >> 5) & 7 };
				break;
			}
			case 0x1008: t = { ...t, k: 'proc' }; break;
			case 0x1009: t = { ...t, k: 'mfunc' }; break;
			case 0x1205: t = { ...t, k: 'bitfield', type: d.readUInt32LE(0), len: d[4], pos: d[5] }; break;
			case 0x1503: {
				const [size, o] = numeric(d, 8);
				t = { ...t, k: 'array', elem: d.readUInt32LE(0), size, name: cstr(d, o)[0] };
				break;
			}
			case 0x1504: case 0x1505: case 0x1506: {
				const isUnion = r.kind === 0x1506;
				const prop = d.readUInt16LE(2), field = d.readUInt32LE(4);
				const [size, o] = numeric(d, isUnion ? 8 : 16);
				const [name, o2] = cstr(d, o);
				const uniq = prop & 0x200 ? cstr(d, o2)[0] : null;
				t = { ...t, k: isUnion ? 'union' : r.kind === 0x1504 ? 'class' : 'struct', prop, field, size, name, uniq, fwd: !!(prop & 0x80) };
				break;
			}
			case 0x1507: {
				const prop = d.readUInt16LE(2);
				t = { ...t, k: 'enum', prop, utype: d.readUInt32LE(4), field: d.readUInt32LE(8), name: cstr(d, 12)[0], fwd: !!(prop & 0x80) };
				break;
			}
			case 0x1203: t = { ...t, k: 'fieldlist', fields: parseFieldList(d) }; break;
			default: t = { ...t, k: 'other' };
		}
		parsed.set(ti, t);
		return t;
	}
	function parseFieldList(d) {
		const out = [];
		let o = 0;
		while (o < d.length) {
			if (d[o] >= 0xf0) { o += d[o] & 0x0f; continue; }
			const kind = d.readUInt16LE(o);
			o += 2;
			switch (kind) {
				case 0x1400: { // LF_BCLASS
					const type = d.readUInt32LE(o + 2);
					const [offset, o2] = numeric(d, o + 6);
					out.push({ f: 'base', type, offset }); o = o2; break;
				}
				case 0x1401: case 0x1402: { // virtual bases: no fixed offset
					const type = d.readUInt32LE(o + 2);
					let [, o2] = numeric(d, o + 10);
					[, o2] = numeric(d, o2);
					out.push({ f: 'vbase', type }); o = o2; break;
				}
				case 0x1404: out.push({ f: 'index', type: d.readUInt32LE(o + 2) }); o += 6; break;
				case 0x1409: out.push({ f: 'vfptr', type: d.readUInt32LE(o + 2) }); o += 6; break;
				case 0x1502: { // enumerate
					let [value, o2] = numeric(d, o + 2);
					const [name, o3] = cstr(d, o2);
					out.push({ f: 'enum', value, name }); o = o3; break;
				}
				case 0x150d: { // member
					const type = d.readUInt32LE(o + 2);
					const [offset, o2] = numeric(d, o + 6);
					const [name, o3] = cstr(d, o2);
					out.push({ f: 'member', type, offset, name }); o = o3; break;
				}
				case 0x150e: { const [name, o3] = cstr(d, o + 6); out.push({ f: 'static', type: d.readUInt32LE(o + 2), name }); o = o3; break; }
				case 0x150f: { const [name, o3] = cstr(d, o + 6); out.push({ f: 'method', name }); o = o3; break; }
				case 0x1510: { const [name, o3] = cstr(d, o + 6); out.push({ f: 'nested', type: d.readUInt32LE(o + 2), name }); o = o3; break; }
				case 0x1511: { // onemethod
					const attr = d.readUInt16LE(o);
					const mprop = (attr >> 2) & 7;
					let o2 = o + 6;
					if (mprop === 4 || mprop === 6) o2 += 4;
					const [name, o3] = cstr(d, o2);
					out.push({ f: 'method', name, virt: mprop }); o = o3; break;
				}
				default: throw new Error('field leaf 0x' + kind.toString(16));
			}
		}
		return out;
	}

	// name → definition index (non-forward). Prefer unique names; fall back on the plain name.
	const byName = new Map();
	for (let ti = tiBegin; ti < tiEnd; ti++) {
		const k = rec(ti).kind;
		if (k !== 0x1504 && k !== 0x1505 && k !== 0x1506 && k !== 0x1507) continue;
		const t = parse(ti);
		if (t.fwd) continue;
		if (!byName.has(t.name)) byName.set(t.name, ti);
		if (t.uniq && !byName.has('#' + t.uniq)) byName.set('#' + t.uniq, ti);
	}
	function resolve(ti) {
		if (ti < tiBegin) return ti;
		const t = parse(ti);
		if (t && (t.k === 'class' || t.k === 'struct' || t.k === 'union' || t.k === 'enum') && t.fwd) {
			return (t.uniq && byName.get('#' + t.uniq)) || byName.get(t.name) || ti;
		}
		return ti;
	}
	function typeName(ti) {
		if (ti < tiBegin) {
			const base = ti & 0xff, mode = (ti >> 8) & 0xf;
			const n = (PRIM[base] || ['prim' + base.toString(16)])[0];
			return mode ? n + ' *' : n;
		}
		const t = parse(resolve(ti));
		switch (t.k) {
			case 'modifier': return typeName(t.type);
			case 'pointer': return typeName(t.type) + ' *';
			case 'array': { const es = sizeOf(t.elem); return `${typeName(t.elem)}[${es ? t.size / es : '?'}]`; }
			case 'bitfield': return `${typeName(t.type)}:${t.len}@${t.pos}`;
			case 'class': case 'struct': case 'union': case 'enum': return t.name;
			case 'proc': case 'mfunc': return 'func';
			default: return 'ti' + ti.toString(16);
		}
	}
	function sizeOf(ti) {
		if (ti < tiBegin) {
			if ((ti >> 8) & 0xf) return 4;
			return (PRIM[ti & 0xff] || [0, 0])[1];
		}
		const t = parse(resolve(ti));
		switch (t.k) {
			case 'modifier': return sizeOf(t.type);
			case 'pointer': return t.size || 4;
			case 'array': return t.size;
			case 'bitfield': return sizeOf(t.type);
			case 'class': case 'struct': case 'union': return t.size;
			case 'enum': return sizeOf(t.utype);
			default: return 0;
		}
	}
	function fieldsOf(ti) {
		const t = parse(resolve(ti));
		if (!t || !t.field) return [];
		const fl = [];
		let f = t.field;
		while (f) {
			const L = parse(f);
			if (!L) break;
			let next = 0;
			for (const x of L.fields) { if (x.f === 'index') next = x.type; else fl.push(x); }
			f = next;
		}
		return fl;
	}
	function find(name) {
		const ti = byName.get(name);
		return ti === undefined ? null : ti;
	}
	// Nested layout: { name, size, bases:[{offset, layout}], members:[{offset,size,type,name,ti}], vfptr }
	function layout(nameOrTi) {
		const ti = typeof nameOrTi === 'number' ? resolve(nameOrTi) : find(nameOrTi);
		if (ti == null) return null;
		const t = parse(ti);
		const L = { name: t.name, kind: t.k, size: t.size, bases: [], members: [], vfptr: false, ti };
		for (const x of fieldsOf(ti)) {
			if (x.f === 'base') L.bases.push({ offset: x.offset, layout: layout(x.type) });
			else if (x.f === 'vbase') L.bases.push({ offset: null, layout: layout(x.type), virtual: true });
			else if (x.f === 'vfptr') L.vfptr = true;
			else if (x.f === 'member') L.members.push({ offset: x.offset, size: sizeOf(x.type), type: typeName(x.type), name: x.name, ti: x.type });
			else if (x.f === 'enum') (L.enumerators ||= []).push({ name: x.name, value: x.value });
		}
		return L;
	}
	// Flattened: every leaf member with absolute offset and the class that declares it.
	function flat(nameOrTi, base = 0, out = [], owner = null) {
		const L = typeof nameOrTi === 'object' ? nameOrTi : layout(nameOrTi);
		if (!L) return out;
		const declaring = L.name;
		const hasBaseAt0 = L.bases.some(b => b.offset === 0);
		if (L.vfptr && !hasBaseAt0) out.push({ offset: base, size: 4, type: 'vfptr', name: '__vfptr', cls: declaring });
		for (const b of L.bases) if (b.offset != null) flat(b.layout, base + b.offset, out);
		for (const m of L.members) out.push({ offset: base + m.offset, size: m.size, type: m.type, name: m.name, cls: declaring, ti: m.ti });
		out.sort((a, b) => a.offset - b.offset);
		return out;
	}
	return { layout, flat, sizeOf, typeName, find, resolve, parse, fieldsOf, tiBegin, tiEnd };
}

function hex(n, w = 3) { return '0x' + n.toString(16).padStart(w, '0'); }

function printTree(L, base, indent, out) {
	out.push(`${indent}${L.kind} ${L.name} size=${hex(L.size)}${base ? ' @' + hex(base) : ''}${L.vfptr ? ' (vfptr)' : ''}`);
	for (const b of L.bases) {
		if (b.offset == null) { out.push(`${indent}  virtual base ${b.layout.name}`); continue; }
		printTree(b.layout, base + b.offset, indent + '  ', out);
	}
	for (const m of L.members) out.push(`${indent}  +${hex(base + m.offset)} ${hex(m.size, 2)} ${m.type.padEnd(40)} ${m.name}`);
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
	const args = process.argv.slice(2);
	const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
	let pdb = path.join(repo, 're/bin/rise.pdb'), json = null, tree = false, en = false;
	const names = [];
	for (let i = 0; i < args.length; i++) {
		const a = args[i];
		if (a === '--pdb') pdb = args[++i];
		else if (a === '--json') json = args[++i];
		else if (a === '--tree') tree = true;
		else if (a === '--enum') en = true;
		else if (a === '--all-from') names.push(...fs.readFileSync(args[++i], 'utf8').split(/\r?\n/).map(s => s.trim()).filter(Boolean));
		else names.push(a);
	}
	const P = loadPdbTypes(pdb);
	if (json) {
		const out = {};
		for (const n of names) {
			const L = P.layout(n);
			if (!L) { console.error('not found: ' + n); continue; }
			out[n] = { size: L.size, bases: L.bases.map(b => ({ offset: b.offset, name: b.layout.name })),
				fields: P.flat(L).map(({ ti, ...f }) => f) };
		}
		fs.writeFileSync(json, JSON.stringify(out, null, 1));
		console.log(`wrote ${Object.keys(out).length} layouts to ${json}`);
	} else {
		for (const n of names) {
			const L = P.layout(n);
			if (!L) { console.log('not found: ' + n); continue; }
			if (en || L.kind === 'enum') { console.log(`enum ${n}`); for (const e of L.enumerators || []) console.log(`  ${e.name} = ${e.value}`); continue; }
			if (tree) { const o = []; printTree(L, 0, '', o); console.log(o.join('\n')); }
			else {
				console.log(`${L.kind} ${L.name} size=${hex(L.size)}`);
				for (const f of P.flat(L)) console.log(`  +${hex(f.offset)} ${hex(f.size, 2)} ${f.type.padEnd(40)} ${f.name.padEnd(28)} (${f.cls})`);
			}
			console.log();
		}
	}
}
