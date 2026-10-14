// The Rise of Nations "rules image": the byte stream Game::walk_rules_data (riseofnations.exe @00589550)
// produces. RecordGame::write_rules embeds it in every .rcx replay, and RecordGame::read_rules loads it back
// in place of the XML. It is a raw dump of the simulation-relevant parts of every rules table, so it is
// the oracle for a reimplemented rules loader (docs/rules-spec.md, "Rules image").
//
// Layout (all little-endian; DataWalk::walk_function writes the raw bytes of [begin, end)):
//   u8  marker("Game Rules")            = 0x92 (low byte of the case-insensitive String hash, SaveGame::walk_test)
//   806 x type record                    (Types::walk_rules_data, vtable slot 0xc4, by TypeIndex)
//   Constants bytes [0x000, 0xd40)       (848 int32)
//   Constants bytes [0x804, 0x808)       (mongol_three_mil_cavalry again)
//   Balance final_balance_table          (493 x 493 int16, row-major)
//   24 x { u8 marker("Tribe rules") = 0x8f; Tribe bytes [0x54,0x6c) (6 int32); graft[352] int32 }
//
// A String is walked as u32 length + UTF-16LE code units. A SimpleArray<u16> is walked as
// i32 count; if count != 0: i32 capacity, u16 (block size), u8 flags (& 0xbf), count x u16.
// In checksum mode (DataWalk.checksum != 0) Strings are skipped; the replay/save form includes them.
import fs from 'node:fs';
import zlib from 'node:zlib';

export const NUM_TYPES = 806;
export const RANGES = {
  goods: [0, 50], units: [50, 414], gaia: [402, 414], buildings: [414, 543], items: [543, 544],
  techs: [544, 629], spells: [629, 684], bonuses: [684, 806],
};
export const kindOf = (t) => t < 50 ? 'good' : t < 414 ? 'unit' : t < 543 ? 'build' : t < 544 ? 'item'
  : t < 629 ? 'tech' : t < 684 ? 'spell' : 'bonus';

// Field lists: [name, kind, count]. kinds: i32, u32, i16, i8, u8. Offsets are the PDB offsets of the
// first field in each walked range (TypeData etc. live at the same offsets in every concrete type).
export const TYPE_FIELDS = [ // TypeData [0x04, 0x5e)
  ['type', 'i32'], ['job_time', 'u32'], ['res_time', 'u32'], ['tribe_mask', 'i32'], ['cat', 'i32'],
  ['costs', 'i32', 6], ['preq', 'i32', 3], ['from', 'i32'], ['where', 'i32'], ['upgrade', 'i32'],
  ['jump', 'i32'], ['obs', 'i32'], ['show', 'i32', 2], ['modified', 'i32'], ['grid_x', 'i8'], ['grid_y', 'i8'],
];
export const OBJECT_FIELDS = [ // ObjectTypeData [0x1e4, 0x27c)
  ['obj_masks', 'i32'], ['attack', 'i32'], ['to_hit', 'i32'], ['attenuate', 'i32'], ['recharge', 'i32'],
  ['min_range', 'i32'], ['max_range', 'i32'], ['splash_area', 'i32'], ['splash_percent', 'i32'],
  ['ammo_per_att', 'i32'], ['proj_speed', 'i32'], ['hits', 'i32'], ['armor', 'i32'], ['domain', 'i32'],
  ['los', 'i32'], ['science_los', 'i32'], ['guy_spacing', 'i32'], ['x_spacing', 'i32'], ['y_spacing', 'i32'],
  ['abil', 'i32'], ['x_size', 'i32'], ['y_size', 'i32'], ['guy_radius', 'i32'], ['block_radius', 'i32'],
  ['big_radius', 'i32'], ['new_block_radius', 'i32'], ['new_big_radius', 'i32'], ['fly_high', 'i32'],
  ['fly_low', 'i32'], ['block_points', 'i32'], ['graft', 'i32'], ['special_upgrade', 'i32'],
  ['special_upgrade_cost', 'i32'], ['support', 'i32', 2], ['support_cost', 'i32', 2], ['age', 'i32'],
];
export const UNIT_FIELDS = [ // UnitTypeData [0x2b4,0x2cc) + [0x2d4,0x2dc) + [0x2dc,0x2e0) + [0x2e0,0x5d4)
  ['unit_flags', 'u32'], ['unit_flags2', 'u32'], ['mode', 'i32'], ['moves', 'i32'], ['turn_speed', 'i32'],
  ['role', 'i32'], /* fire_proj 0x2cc and second_max_range 0x2d0 are not walked */
  ['carry', 'i32'], ['carry_size', 'i32'], ['military_level', 'i32'],
  ['research_premium_cost', 'i32'], ['research_premium_time', 'i32'], ['job_extra_time', 'u32'], ['mana', 'i32'],
  ['control_cost', 'i32'], ['progression', 'i32'], ['push_size', 'i32'], ['push_circles', 'i32'],
  ['target_size', 'i32'], ['squad_size', 'i32'], ['uber_size', 'i32'], ['crew_size', 'i32'], ['base_form', 'i32'],
  ['relative_value', 'i16', 352],
];
export const BUILD_FIELDS = [ // BuildTypeData [0x2b4, 0x2e5)
  ['town_hits', 'i32'], ['min_city_size', 'i32'], ['misery_rate', 'i32'], ['build_flags', 'u32'], ['most_shots', 'i32'],
  ['garrison_max', 'i32'], ['base_arrows', 'i32'], ['wonder_val', 'i32'], ['plunder_value', 'i32'],
  ['plunder_good', 'i32'], ['behind_height', 'i32'], ['to', 'i32'], ['civ_graph_mask', 'i8'],
];
export const GOOD_FIELDS = [ // GoodTypeData [0x2b4, 0x2f8)
  ['undiscovered_cost_good', 'i32'], ['obsolete_cost_good', 'i32'], ['undiscovered_support_good', 'i32'],
  ['obsolete_support_good', 'i32'], ['obsolete_production_good', 'i32'], ['undiscovered_cost_rate', 'i32'],
  ['obsolete_cost_rate', 'i32'], ['undiscovered_support_rate', 'i32'], ['obsolete_support_rate', 'i32'],
  ['obsolete_production_rate', 'i32'], ['bonus_type', 'i32', 2], ['bonus_num', 'i32', 2], ['largest_gather', 'i32'],
  ['block', 'i32'], ['exclude_ctw', 'i32'],
];
export const TECH_FIELDS = [['age', 'i32'], ['ai', 'i16', 11], ['leader_off', 'u8']]; // TechTypeData [0x1c8, 0x1e3)
export const SPELL_FIELDS = [ // SpellTypeData [0x1c8, 0x1f8)
  ['spell_flags', 'u32'], ['spell_range', 'i32'], ['mana', 'i32'], ['duration', 'i32'], ['duration_upgrade', 'i32'],
  ['from2', 'i32'], ['use', 'i32', 6],
];
export const TRIBE_FIELDS = [ // Tribe [0x54, 0x6c) + [0x70, 0x5f0)
  ['tribe', 'i32'], ['barbarian', 'i32'], ['people', 'i32'], ['backup_build_continent', 'i32'],
  ['build_continent', 'i32'], ['unit_continent', 'i32'], ['graft', 'i32', 352],
];
export const NUM_TRIBES = 24, BALANCE_N = 493, CONSTANTS_INTS = 0xd40 / 4;
export const MARKER_GAME_RULES = 0x92, MARKER_TRIBE_RULES = 0x8f;

const SIZE = { i32: 4, u32: 4, i16: 2, u8: 1, i8: 1 };
const RD = { i32: 'readInt32LE', u32: 'readUInt32LE', i16: 'readInt16LE', u8: 'readUInt8', i8: 'readInt8' };
const WR = { i32: 'writeInt32LE', u32: 'writeUInt32LE', i16: 'writeInt16LE', u8: 'writeUInt8', i8: 'writeInt8' };

class Reader {
  constructor(b, p) { this.b = b; this.p = p; }
  v(k) { const x = this.b[RD[k]](this.p); this.p += SIZE[k]; return x; }
  fields(list, o = {}) {
    for (const [n, k, c] of list) o[n] = c ? Array.from({ length: c }, () => this.v(k)) : this.v(k);
    return o;
  }
  str() {
    const n = this.v('u32'); if (n > 4096) throw new Error(`bad string length ${n} at ${this.p - 4}`);
    const s = this.b.toString('utf16le', this.p, this.p + n * 2); this.p += n * 2; return s;
  }
  sa() {
    const n = this.v('i32'); if (n < 0 || n > 4096) throw new Error(`bad SimpleArray count ${n} at ${this.p - 4}`);
    if (!n) return { count: 0 };
    const cap = this.v('i32'), block = this.v('u16' in SIZE ? 'u16' : 'i16'), flags = this.v('u8');
    return { count: n, capacity: cap, block: block & 0xffff, flags, v: Array.from({ length: n }, () => this.b.readUInt16LE((this.p += 2) - 2)) };
  }
}
SIZE.u16 = 2; RD.u16 = 'readUInt16LE'; WR.u16 = 'writeUInt16LE';

class Writer {
  constructor() { this.chunks = []; }
  v(k, x) { const b = Buffer.alloc(SIZE[k]); b[WR[k]](k === 'u32' ? x >>> 0 : x); this.chunks.push(b); }
  fields(list, o) {
    for (const [n, k, c] of list) {
      const x = o[n]; if (x === undefined) throw new Error(`missing field ${n}`);
      if (c) { for (let i = 0; i < c; i++) this.v(k, x[i]); } else this.v(k, x);
    }
  }
  str(s) { s = s ?? ''; this.v('u32', s.length); this.chunks.push(Buffer.from(s, 'utf16le')); }
  sa(a) {
    if (!a || !a.count) { this.v('i32', 0); return; }
    this.v('i32', a.count); this.v('i32', a.capacity); this.v('u16', a.block); this.v('u8', a.flags);
    for (const x of a.v) this.v('u16', x);
  }
  bytes() { return Buffer.concat(this.chunks); }
}

// Parse a rules image starting at offset p (the marker byte). Returns { rules, end }.
export function readImage(b, p, { strings = true } = {}) {
  const r = new Reader(b, p);
  const marker = r.v('u8');
  const types = [];
  for (let t = 0; t < NUM_TYPES; t++) {
    const kind = kindOf(t), o = { kind };
    r.fields(TYPE_FIELDS, o);
    if (o.type !== t) throw new Error(`type record ${t}: type field is ${o.type} (offset ${r.p})`);
    o.display_name = strings ? r.str() : undefined;
    if (kind === 'good' || kind === 'unit' || kind === 'build' || kind === 'item') {
      r.fields(OBJECT_FIELDS, o); o.is_list = r.sa(); o.is_strict_list = r.sa();
      if (kind === 'unit') r.fields(UNIT_FIELDS, o);
      if (kind === 'build') r.fields(BUILD_FIELDS, o);
      if (kind === 'good') r.fields(GOOD_FIELDS, o);
    } else if (kind === 'tech') {
      r.fields(TECH_FIELDS, o); o.alternate_name = strings ? Array.from({ length: 8 }, () => r.str()) : undefined;
    } else if (kind === 'spell') r.fields(SPELL_FIELDS, o);
    types.push(o);
  }
  const constants = Array.from({ length: CONSTANTS_INTS }, () => r.v('i32'));
  const constantsDup = r.v('i32');
  const balance = Array.from({ length: BALANCE_N }, () => Array.from({ length: BALANCE_N }, () => r.v('i16')));
  const tribes = [];
  for (let i = 0; i < NUM_TRIBES; i++) { const m = r.v('u8'); tribes.push(r.fields(TRIBE_FIELDS, { marker: m })); }
  return { rules: { marker, types, constants, constantsDup, balance, tribes }, end: r.p };
}

export function writeImage(rules, { strings = true } = {}) {
  const w = new Writer();
  w.v('u8', MARKER_GAME_RULES);
  for (const o of rules.types) {
    w.fields(TYPE_FIELDS, o);
    if (strings) w.str(o.display_name);
    const kind = o.kind;
    if (kind === 'good' || kind === 'unit' || kind === 'build' || kind === 'item') {
      w.fields(OBJECT_FIELDS, o); w.sa(o.is_list); w.sa(o.is_strict_list);
      if (kind === 'unit') w.fields(UNIT_FIELDS, o);
      if (kind === 'build') w.fields(BUILD_FIELDS, o);
      if (kind === 'good') w.fields(GOOD_FIELDS, o);
    } else if (kind === 'tech') {
      w.fields(TECH_FIELDS, o); if (strings) for (let i = 0; i < 8; i++) w.str(o.alternate_name[i]);
    } else if (kind === 'spell') w.fields(SPELL_FIELDS, o);
  }
  for (const x of rules.constants) w.v('i32', x);
  w.v('i32', rules.constants[0x804 / 4]);
  for (const row of rules.balance) for (const x of row) w.v('i16', x);
  for (const t of rules.tribes) { w.v('u8', MARKER_TRIBE_RULES); w.fields(TRIBE_FIELDS, t); }
  return w.bytes();
}

export function decompress(raw) { return raw[0] === 0x1f && raw[1] === 0x8b ? zlib.gunzipSync(raw) : raw; }

// Find the rules image inside a (decompressed) replay: the first offset where all 806 type records parse.
export function findImage(b) {
  for (let p = 0; p + 200 < b.length; p++) {
    if (b[p] !== MARKER_GAME_RULES || b.readInt32LE(p + 1) !== 0) continue;
    if (b.readInt32LE(p + 1 + 0x0c) !== -1 && b.readInt32LE(p + 1 + 0x0c) === 0) continue;
    try { return { offset: p, ...readImage(b, p) }; } catch { /* keep scanning */ }
  }
  return null;
}

export function loadReplayImage(file) {
  const b = decompress(fs.readFileSync(file));
  const r = findImage(b); if (!r) throw new Error(`no rules image found in ${file}`);
  return { ...r, buffer: b };
}
