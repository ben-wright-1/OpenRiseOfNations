// Dump the built-in default hotkey table (KeyMap) from the exe: the defaults are not in any data file. KeyMap::close
// @007d5de0 fills the 461 KeyMapEntry slots from the static arrays KeyMap::default_keys / default_mouse / default_modes /
// default_types / default_groups / default_dependent, and Console::exec_keymap @007c2ce0 maps an entry through
// KeyMap::keymap_options[i] = (OptionIndex, param) to Options::exec. Names come from the PDB enums KeyMapType (same order
// as KeyMap::keymap_strings, the enum= names in Data/playerprofile.xml) and OptionIndex.
//
// Key word: Windows VK code | 0x10000 Shift | 0x20000 Ctrl | 0x40000 Alt. mouse: 0 none, 1 wheel up, 2 wheel down,
// 3 middle, 4.. extra 1..8. A "dependent" entry has only modifier bits; it takes the key/mouse of the closest preceding
// non-dependent entry (e.g. Ctrl+C / Shift+C follow C). mode = OptionMode (0 standard, 1 build menu, 4 script IDE).
// type = selection context: -1 always, -2/-4 buildings selected, -3 units, -5 aircraft, -6 units (land/sea), >=0 TypeIndex.
// group = bit mask over the 23 key groups in the options hotkey list.
//   node re/tools/keymap.mjs [--all] [--tsv]      (--all also lists unbound entries)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadPdbTypes } from './pdbtypes.mjs';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const all = process.argv.includes('--all'), tsv = process.argv.includes('--tsv');
const exe = fs.readFileSync(path.join(repo, 're/bin/riseofnations.exe'));
const map = fs.readFileSync(path.join(repo, 're/bin/rise_z.map'), 'latin1');
const sym = name => { const m = map.match(new RegExp(`\\?${name}@KeyMap@@\\S*\\s+([0-9a-f]{8})`)); if (!m) throw new Error(name); return parseInt(m[1], 16); };

const pe = exe.readUInt32LE(0x3c), base = exe.readUInt32LE(pe + 52), secs = [];
for (let i = 0, sh = pe + 24 + exe.readUInt16LE(pe + 20); i < exe.readUInt16LE(pe + 6); i++, sh += 40)
	secs.push({ va: base + exe.readUInt32LE(sh + 12), vs: exe.readUInt32LE(sh + 8), raw: exe.readUInt32LE(sh + 20) });
const off = va => { const s = secs.find(s => va >= s.va && va < s.va + s.vs); return s.raw + va - s.va; };
const i32 = (sy, i) => exe.readInt32LE(off(sym(sy) + 4 * i));

const P = loadPdbTypes(path.join(repo, 're/bin/rise.pdb'));
const enumNames = n => { const r = {}; for (const e of P.layout(n).enumerators) if (!(e.value in r)) r[e.value] = e.name; return r; };
const KMT = enumNames('KeyMapType'), OPT = enumNames('OptionIndex'), MODE = enumNames('OptionMode');

const VK = { 8: 'Backspace', 9: 'Tab', 13: 'Enter', 0x13: 'Pause', 0x1b: 'Esc', 0x20: 'Space', 0x21: 'PgUp', 0x22: 'PgDn',
	0x23: 'End', 0x24: 'Home', 0x25: 'Left', 0x26: 'Up', 0x27: 'Right', 0x28: 'Down', 0x2d: 'Insert', 0x2e: 'Delete',
	0x6a: 'Num*', 0x6b: 'Num+', 0x6d: 'Num-', 0x6e: 'Num.', 0x6f: 'Num/', 0xba: ';', 0xbb: '=', 0xbc: ',', 0xbd: '-',
	0xbe: '.', 0xbf: '/', 0xc0: '`', 0xdb: '[', 0xdc: '\\', 0xdd: ']', 0xde: "'" };
const MOUSE = ['', 'WheelUp', 'WheelDown', 'Middle', ...Array.from({ length: 8 }, (_, i) => `MouseX${i + 1}`)];
const vk = k => VK[k] ?? ((k >= 0x30 && k <= 0x39) || (k >= 0x41 && k <= 0x5a) ? String.fromCharCode(k)
	: k >= 0x60 && k <= 0x69 ? `Num${k - 0x60}` : k >= 0x70 && k <= 0x87 ? `F${k - 0x6f}` : `vk${k.toString(16)}`);
const keyString = (key, mouse) => {
	const p = [key & 0x20000 && 'Ctrl', key & 0x40000 && 'Alt', key & 0x10000 && 'Shift'].filter(Boolean);
	const b = key & ~0x70000;
	if (mouse > 0) p.push(MOUSE[mouse]); else if (b) p.push(vk(b)); else return ''; // modifiers alone: unbound
	return p.join('+');
};

const N = (P.layout('KeyMap').size - 4) / P.layout('KeyMapEntry').size; // KeyMapEntry entries[461]
const optBase = sym('keymap_options'), depBase = sym('default_dependent');
let parent = -1;
const rows = [];
for (let i = 0; i < N; i++) {
	const dep = exe[off(depBase + i)];
	if (!dep) parent = i;
	let key = i32('default_keys', i), mouse = i32('default_mouse', i);
	if (dep) { key |= rows[parent].key; mouse ||= rows[parent].mouse; }
	const o = exe.readInt16LE(off(optBase + 4 * i)), p = exe.readInt16LE(off(optBase + 4 * i + 2));
	rows.push({ i, name: (KMT[i] ?? '?').replace(/^KEYMAP_/, ''), key, mouse, keys: keyString(key, mouse),
		mode: MODE[i32('default_modes', i)] ?? i32('default_modes', i), type: i32('default_types', i),
		group: Math.log2(i32('default_groups', i)), dep: dep ? rows[parent].name : '',
		option: o < 0 ? '-' : (OPT[o] ?? o) + (p >= 0 ? `/${p}` : '') });
}
const cols = ['i', 'name', 'keys', 'mode', 'type', 'group', 'option', 'dep'];
if (tsv) console.log(cols.join('\t'));
for (const r of rows) {
	if (!all && !r.keys) continue;
	console.log(tsv ? cols.map(c => r[c]).join('\t')
		: `${String(r.i).padStart(3)} ${r.name.padEnd(36)} ${r.keys.padEnd(18)} ${String(r.mode).padEnd(18)} type=${String(r.type).padEnd(4)} g${String(r.group).padEnd(3)} ${r.option}${r.dep ? '  (dep ' + r.dep + ')' : ''}`);
}
