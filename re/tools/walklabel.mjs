#!/usr/bin/env node
// DataWalk section labels and tag bytes.
//
// walk_data code passes labels as `int_str_array->list + off` (the internal string table, String = 0x14 bytes), i.e. the
// (off / 0x14)-th <STRING> of Data/internal_strings.xml. SaveGame/LoadGame::walk_test write/check ONE byte: the low byte of
// the label's case-insensitive hash (String::generate_hash, second result; primes table @b14500).
//
// Usage: node re/tools/walklabel.mjs [--install DIR] 0x4f24 [0x18a60 ...]    → index, string, tag byte
//        node re/tools/walklabel.mjs --hash "Game Rules"                      → hashes of a literal string
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

// String::generate_hash primes (50 entries, riseofnations.exe @00b14500)
export const PRIMES = [127, 811, 1597, 2131, 2749, 4759, 5527, 5953, 8117, 9539, 10273, 10753, 11159, 12301, 13217, 14207, 15413,
	17681, 18661, 19013, 21089, 22051, 25111, 25801, 27457, 28057, 29581, 30809, 32611, 34469, 36067, 37511, 38723, 40093, 41983,
	43321, 45083, 47431, 49667, 50767, 53453, 55469, 57193, 59369, 61987, 65071, 73421, 77849, 84223, 89009];

// Returns [case-sensitive hash, case-insensitive hash]. towlower is applied per UTF-16 unit (ASCII labels only in practice).
export function ronHash(s) {
	let n = s.length, h = 0, hi = 0;
	for (let i = s.length - 1; i >= 0; i--) {
		const c = s.charCodeAt(i), l = s[i].toLowerCase().charCodeAt(0);
		h = (h + PRIMES[i % 50] * c + PRIMES[c % 50] * n) >>> 0;
		hi = (hi + PRIMES[l % 50] * n + l * PRIMES[i % 50]) >>> 0;
		n--;
	}
	return [h, hi];
}
export const tagByte = s => ronHash(s)[1] & 0xff;

let cache = null;
export function internalStrings(install = path.join(os.homedir(), 'Games/RiseOfNations')) {
	if (cache) return cache;
	const xml = fs.readFileSync(path.join(install, 'Data/internal_strings.xml'), 'utf8');
	const out = [];
	const re = /<STRING\b[^>]*?(?:\/>|>([\s\S]*?)<\/STRING>)/g;
	let m;
	const unesc = s => s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&amp;/g, '&');
	while ((m = re.exec(xml))) out.push(m[1] === undefined ? '' : unesc(m[1]));
	return (cache = out);
}
// off = byte offset into the internal string array (as it appears in decompiled code), or a literal label string.
export function resolveLabel(off, install) {
	if (typeof off === 'string' && !/^(0x)?[0-9a-f]+$/i.test(off)) return { index: null, label: off, byte: tagByte(off) };
	const o = typeof off === 'number' ? off : parseInt(off, 16);
	if (o % 0x14) throw new Error(`label offset 0x${o.toString(16)} is not a multiple of 0x14`);
	const label = internalStrings(install)[o / 0x14];
	if (label === undefined) throw new Error(`label offset 0x${o.toString(16)} out of range`);
	return { index: o / 0x14, label, byte: tagByte(label) };
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
	const a = process.argv.slice(2);
	let install;
	for (let i = 0; i < a.length; i++) {
		if (a[i] === '--install') { install = a[++i]; continue; }
		if (a[i] === '--hash') { const s = a[++i]; const [h, hi] = ronHash(s); console.log(`${JSON.stringify(s)} hash=${h} insensitive=${hi} tag=0x${(hi & 0xff).toString(16)}`); continue; }
		const r = resolveLabel(a[i], install);
		console.log(`${a[i]}  index=${r.index}  ${JSON.stringify(r.label)}  tag=0x${r.byte.toString(16).padStart(2, '0')}`);
	}
}
