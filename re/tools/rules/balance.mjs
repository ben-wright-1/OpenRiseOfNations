// Balance::init / fill_tables / compute_modifier / type_damage reimplementation (see balance.md).
// computeBalance(dataDir, info) -> Int16 values as number[493][493]; row = attacker TypeIndex - 50,
// col = defender TypeIndex - 50, covering types 50..542 (units 50..401, gaia 402..413, buildings 414..542).
//
// info = {
//   types:     array indexed by TypeIndex (at least 50..542), each object with the already-loaded fields
//              type, name (internal <NAME>), obj_masks, age, domain, graft, from, is_list, is_strict_list
//              ({count, v:[...]}), and for units (50..413) unit_flags, unit_flags2. (ctx.types works as is.)
//   constants: {one_age_down, two_ages_down, three_ages_down, four_ages_down, five_ages_down} (ctx.constants).
// }
// The raw balance.xml table is read here (Data/balance.xml).
import * as X from "../rulesxml.mjs";

const BASE_UNIT = 50, FIRST_GAIA = 402, FIRST_BUILD = 414, FIRST_WONDER = 526, END_BUILD = 543;
const RAW = 399; // 352 unit names + 5 lines + 2 objects + 8 ages + 32 flags
const R_LINE = 352, R_OBJ = 357, R_AGE = 359, R_FLAG = 367;
const LINE_STR = ['SIEGE', 'FORTS', 'TOWERS', 'CITIES', 'OBSPOST'];
const OBJECT_STR = ['BUILDINGS', 'UNITS'];
const OBJMASK_STR = ['ARMORED', 'BOMBARD', 'CIVILIAN', 'MUSKET_INF', 'ELEPHANT', 'FOOT', 'GUN', 'HEAVY_INF', 'MODERN_INF',
  'CARRY_AIR', 'FOOT_ARCHER', 'LARGE', 'MOUNTED', 'NAVAL', 'HORSE_ARCHER', 'SPARSE', 'LIGHT_INF', 'ARCHERY', 'SIEGE',
  'WAR_MACHINE', 'ARMORPIERCE', 'VEHICLE', 'MELEE', 'EXPLOSIVE', 'HEAVY_CAV', 'DETECT', 'UNUSED', 'MISSILE', 'AIR',
  'LIGHT_CAV', 'PIKE', 'ANTI_AIR'].map((s) => 'OBJMASK_' + s);

const FLAG_SEP = ''; // internal string table entry 17 (offset 0x154), empty
const i32 = (x) => x | 0;
const mul = (v, k) => Math.imul(v, k);
const pct = (v, k) => i32(Math.trunc(Math.imul(v, k) / 100)); // (v*k)/100, int32 wrap, C truncation
const i16 = (x) => (x << 16) >> 16;

// Balance::lookup_name @005829b0 via lookup_absolute_name @00582910 (raw index 0..398 -> attribute/entry name).
export function balanceName(i, types) {
  let s;
  if (i < R_LINE) s = types[BASE_UNIT + i].name.replaceAll("'", '');
  else if (i < R_OBJ) s = LINE_STR[i - R_LINE];
  else if (i < R_AGE) s = OBJECT_STR[i - R_OBJ];
  else if (i < R_FLAG) s = 'AGE ' + (i - R_AGE);
  else {
    // "Flag " + letter (A..Z, then 1..6) + internal string #17 + OBJMASK name. String #17 is EMPTY in
    // internal_strings.xml, so the result is e.g. "Flag_YOBJMASK_HEAVY_CAV", which never matches balance.xml's
    // "Flag_Y_OBJMASK_HEAVY_CAV": every flag row and column stays 100 (verified against the oracle).
    const b = i - R_FLAG; const c = 65 + b;
    s = 'Flag ' + String.fromCharCode(c > 90 ? c - 42 : c) + FLAG_SEP + OBJMASK_STR[b];
  }
  return s.replaceAll(' ', '_');
}

// Balance::fill_tables @005823f0, first half: 399x399 raw table, raw[row*399+col] (row = ENTRY, col = attribute).
export function readRawTable(dataDir, types) {
  const root = X.readXml(dataDir, 'balance.xml');
  const table = X.child(root, 'TABLE');
  const byName = new Map();
  for (const e of (table ? table.children : [])) if (e.tag === 'ENTRY' && e.attrs.name !== undefined && !byName.has(e.attrs.name)) byName.set(e.attrs.name, e);
  const names = Array.from({ length: RAW }, (_, i) => balanceName(i, types));
  const raw = new Int16Array(RAW * RAW).fill(100);
  for (let r = 0; r < RAW; r++) {
    const e = byName.get(names[r]);
    if (!e) continue;
    for (let c = 0; c < RAW; c++) raw[r * RAW + c] = i16(X.getAttribNum(e, names[c], 100));
  }
  return raw;
}

const isUnit = (t) => t >= BASE_UNIT && t < FIRST_BUILD;   // TypeData::is_unit_type (includes gaia)
const isGaia = (t) => t >= FIRST_GAIA && t < FIRST_BUILD;
const isBuild = (t) => t >= FIRST_BUILD && t < END_BUILD;
const isWonder = (t) => t >= FIRST_WONDER && t < END_BUILD;

export function makeTypeOps(types) {
  // ObjectTypeData::is @0065f7d0 (lists are always filled for 50..542 except gaia, which never reach here).
  const is = (o, x, strict = 0) => {
    if (o.type === x) return true;
    const l = strict ? o.is_strict_list : o.is_list;
    if (!strict && x < 0) return false;
    if (!l || !l.count) return isSlow(o, x, strict);
    for (let i = 0; i < l.count; i++) if (l.v[i] === x) return true;
    return false;
  };
  // ObjectTypeData::is_slow @00661ae0 (fallback for empty lists; not reached with stock data).
  // Non-strict: own type, else graft == x, else recurse into the "from" type (TypeData+0x3c) if >= 0.
  // Strict: units 50..413 only, graft == x and the graft target lacks unit_flags 0x1000000.
  const isSlow = (o, x, strict) => {
    if (o.type === x) return true;
    if (!strict) {
      if (x < 0) return false;
      if (o.graft === x) return true;
      return o.from >= 0 ? isSlow(types[o.from], x, 0) : false;
    }
    if (!isUnit(o.type)) return false;
    return o.graft === x && !(types[x].unit_flags & 0x1000000);
  };
  const isSiege = (o) => isUnit(o.type) && (o.unit_flags & 0x20000) !== 0;     // vslot 0x10c (0 for buildings)
  const isMissile = (o) => isUnit(o.type) && (o.obj_masks & 0x8000000) !== 0;  // vslot 0x114
  const isCaravan = (o) => isUnit(o.type) && (o.unit_flags2 & 8) !== 0;        // vslot 0x130
  const age = (o) => { if (o.age === -1) throw new Error('get_age_slow needed for type ' + o.type); return o.age; };
  return { is, isSiege, isMissile, isCaravan, age };
}

// Balance::return_pack @005821c0
function pack(o, ops) {
  const p = { type: -1, line: -1, obj: -1, age: -1, flags: 0 };
  if (isGaia(o.type)) return p;
  if (isUnit(o.type)) {
    p.type = o.type - BASE_UNIT;
    if (ops.isSiege(o)) p.line = 0;
    p.obj = 1;
  } else {
    if (ops.is(o, 443)) p.line = 1;          // BuildTypeData::is_fort
    else if (ops.is(o, 414)) p.line = 3;     // BuildTypeData::is_city
    else if (ops.is(o, 521)) p.line = 4;
    else if (ops.is(o, 439)) p.line = 2;
    p.obj = 0;
  }
  p.age = ops.age(o);
  p.flags = o.obj_masks;
  return p;
}
function indices(p) {
  const r = [];
  if (p.type >= 0) r.push(p.type);
  if (p.line >= 0) r.push(p.line + R_LINE);
  if (p.obj >= 0) r.push(p.obj + R_OBJ);
  if (p.age >= 0) r.push(p.age + R_AGE);
  for (let b = 0; b < 32; b++) if ((p.flags >>> b) & 1) r.push(b + R_FLAG);
  return r;
}

// Balance::type_damage @0057fb50: hard-coded percentage chain (integer, truncating, in this exact order).
export function typeDamage(A, D, ops, K) {
  const { is, isSiege, isMissile, isCaravan } = ops;
  const am = A.obj_masks | 0, dm = D.obj_masks | 0;
  const at = A.type, dt = D.type;
  const ageA = ops.age(A), ageD = ops.age(D);
  let v = 100;
  if (isUnit(at) && ageD < ageA && isUnit(dt)) {
    const d = ageA - ageD;
    const k = d === 1 ? K.one_age_down : d === 2 ? K.two_ages_down : d === 3 ? K.three_ages_down : d === 4 ? K.four_ages_down : K.five_ages_down;
    v = pct(k + 100, 100); // ((k + 100) * 100) / 100
  }
  if (am & 0x10000) { if (dm & 0x400) v = pct(v, 162); if (dm & 0x4000) v = pct(v, 152); }
  if (am & 0x80) {
    if (!(am & 0x40)) {
      if (dm & 0x1000) v = pct(v, 166);
      if (isBuild(dt)) v = pct(v, 130);
      if (dm & 0x400) v = pct(v, 86);
      if (dm & 8) v = pct(v, 75);
      if (dm & 0x100) v = pct(v, 75);
    } else {
      if (dm & 0x1000) v = pct(v, 257);
      if (dm & 0x4000) v = pct(v, 66);
      if (dm & 0x20000000) v = pct(v, 114);
      if (is(D, 216)) v = pct(v, 225);
      if (is(D, 239)) v = pct(v, 132);
      if (dm & 0x100) v = pct(v, 80);
    }
  }
  if (am & 0x400) {
    if (dm & 0x10000) v = pct(v, 65);
    if (dm & 8) v = pct(v, 90);
    if (dm & 0x80) v = pct(v, 253);
    if (dm & 0x4000) v = pct(v, 138);
    if (dm & 0x8000) v = pct(v, 80);
    if (isBuild(dt)) v = pct(v, 33);
    if (isSiege(D)) v = pct(v, 75);
  }
  if (am & 0x20000000) {
    if (dm & 0x400) v = pct(v, 114);
    if (dm & 0x10000) v = pct(v, 158);
    if (dm & 8) v = pct(v, 149);
    if (dm & 0x100) v = pct(v, 159);
    if (is(D, 123)) v = pct(v, 130);
    if (is(D, 131)) v = pct(v, 125);
    if (dm & 0x40000) v = pct(v, 122);
    if (dm & 1) v = pct(v, 98);
    if (isBuild(dt)) v = pct(v, 33);
    if (isCaravan(D) || dt === 61 || dt === 62 || dt === 400) v = i32(Math.trunc(v / 3));
    if (dt === 50 || dt === 51 || dt === 52 || dt === 53 || is(D, 66)) v = i32(Math.trunc(v / 2));
  }
  if (am & 0x4000) {
    if (isBuild(dt)) v = pct(v, 33);
    if (isSiege(D)) v = pct(v, 75);
    if (dm & 0x20000000) v = pct(v, 57);
    if (dm & 0x400) v = pct(v, 57);
    if (dm & 0x10000) v = pct(v, 66);
    if (dm & 8) v = pct(v, 66);
    if (dm & 0x80) v = pct(v, 160);
    if (am & 0x200000) {
      if (dm & 0x20000000) v = pct(v, 160);
      if (dm & 0x400) v = pct(v, 135);
      if (dm & 0x10000) v = mul(v, 2);
      if (dm & 8) v = mul(v, 2);
      if (dm & 0x80) v = pct(v, 50);
      if (dm & 0x1000000) v = pct(v, 97);
    }
    if (am & 0x40) {
      if (is(D, 123)) v = pct(v, 155);
      if (dm & 1) v = pct(v, 92);
    }
  }
  if (am & 0x1000000) {
    if (dm & 0x400) v = pct(v, 170);
    if (dm & 0x10000) v = pct(v, 179);
    if (dm & 8) v = pct(v, 155);
    if (dm & 0x100) v = pct(v, 170);
    if (is(D, 123)) v = pct(v, 155);
    if (is(D, 131)) v = pct(v, 140);
    if (isBuild(dt)) v = pct(v, 33);
  }
  if (am & 8) {
    v = pct(v, 133);
    if (dm & 0x400) v = pct(v, 125);
    if (dm & 0x80) v = pct(v, 180);
    if (dm & 0x4000) v = pct(v, 114);
    if (isBuild(dt)) v = pct(v, 66);
    if (isSiege(D)) v = pct(v, 75);
    if (ageA === 4 && (dm & 0x80)) v = pct(v, 110);
  }
  if (am & 1) {
    if (is(A, 239) && (dm & 8)) v = pct(v, 155);
    if (is(A, 239) && (dm & 0x100)) v = pct(v, 185);
    if (is(A, 239) && (dm & 0x1000000)) v = pct(v, 115);
    if (is(A, 239) && (dm & 0x20000000)) v = pct(v, 120);
    if (is(A, 239) && (dm & 0x4000)) v = pct(v, 120);
    if (is(A, 239) && is(D, 123)) v = pct(v, 175);
    if (is(A, 216) && (dm & 0x100)) v = pct(v, 175);
    if (is(A, 216) && (dm & 8)) v = pct(v, 175);
    if (is(A, 216) && is(D, 239)) v = pct(v, 120);
    if (isBuild(dt)) v = pct(v, 66);
  }
  if (am & 0x100) {
    if (dm & 0x80) v = pct(v, 260);
    if (is(D, 216)) v = pct(v, 170);
    if (is(D, 239)) v = pct(v, 75);
    if (is(D, 123)) v = pct(v, 66);
    if (dm & 4) v = pct(v, 120);
    if (isSiege(D)) v = pct(v, 75);
    if (isBuild(dt)) v = pct(v, 66);
  }
  if (is(A, 123)) {
    if (dm & 0x10008) v = pct(v, 330);
    if (dm & 0x100) v = pct(v, 330);
    if (dm & 0x80) v = pct(v, 330);
    if (dm & 4) v = mul(v, 5);
    if (dm & 1) v = pct(v, 50);
    if (isBuild(dt)) v = pct(v, 50);
    if (isSiege(D)) v = pct(v, 75);
  }
  if (is(A, 131, 1)) {
    if (dm & 8) v = pct(v, 88);
    if (dm & 0x100) v = pct(v, 80);
    if (dm & 0x1000) v = pct(v, 125);
  }
  if ((am & 0x100000) && (dm & 1)) v = pct(v, 180);
  if (is(A, 66) && (dm & 0x14000)) v = mul(v, 2);
  if ((at === 50 || at === 51 || at === 52 || at === 53) && (dm & 0x14000)) v = mul(v, 2);
  if (am & 0x2000) {
    if (isBuild(dt) && !(am & 0x40000)) v = pct(v, 33);
    if (isUnit(dt) && !(dm & 0x10002000) && !isSiege(D)) v = pct(v, 33);
    if (isUnit(dt) && !(dm & 0x2000) && isSiege(D)) v = pct(v, 66);
    if (is(A, 346) && (dm & 0x2000)) v = pct(v, 33);
    if ((is(A, 323) || is(A, 338)) && (dm & 0x2004) === 0x2004) v = mul(v, 2);
  }
  if (is(A, 334) && is(D, 340)) v = pct(v, 310);
  if (is(A, 334) && is(D, 346)) v = v << 2;
  if (is(A, 334) && is(D, 334)) v = v << 2;
  if (is(A, 340) && is(D, 323)) v = pct(v, 160);
  if (isUnit(at) && !(am & 0x2000)) {
    if (!(am & 0x40000) && (dm & 0x2000)) v = pct(v, 33);
    if (isSiege(A) && (dm & 0x2000)) v = pct(v, 350);
  }
  if ((is(A, 304) || is(A, 308)) && isBuild(dt) && dm < 0) v = pct(v, 25);
  if ((am & 0x10000000) && !isMissile(A) && am < 0) {
    if (isBuild(dt) && dm >= 0) v = pct(v, 15);
    if (isBuild(dt) && dm < 0) v = pct(v, 85);
    if (is(D, 304)) v = mul(v, 2);
    if (isSiege(D)) v = pct(v, 125);
    if (dm & 0x2000) v = pct(v, 350);
  }
  if (is(A, 310)) {
    if (is(D, 334)) v = v << 3;
    if (is(D, 239)) v = pct(v, 450);
  }
  if (isUnit(at) && am >= 0 && is(D, 310)) v = pct(v, 25);
  if (is(A, 313) && isBuild(dt) && isWonder(dt)) v = i32(Math.trunc(v / 2));
  if (is(D, 63)) {
    if (am & 0x400000) v = pct(v, 180);
    if (isBuild(at)) v = i32(Math.trunc(mul(v, 14) / 5));
  }
  const siegeBit = am & 0x40000;
  if (siegeBit && isBuild(dt)) v = pct(v, 430);
  if ((am & 2) && isBuild(dt)) v = pct(v, 250);
  if (is(D, 447) && (A.domain !== 0 || siegeBit)) v = i32(Math.trunc(v / 3));
  if (is(A, 88) && (dm & 0x10000)) v = pct(v, 150);
  if (is(A, 174)) { if (dm & 0x400) v = pct(v, 125); if (dm & 0x4000) v = pct(v, 125); }
  if (is(A, 91)) {
    if (dm & 0x1000000) v = pct(v, 115);
    if (dm & 0x20000000) v = pct(v, 115);
    if (dm & 0x4000) v = pct(v, 115);
  }
  if (am & 0x4000) {
    if (is(A, 191)) { if (dm & 0x10000) v = pct(v, 135); if (dm & 8) v = pct(v, 120); }
    if (is(A, 195) && (dm & 0x4000)) v = pct(v, 150);
    if (is(A, 199)) { if (dm & 0x10000) v = pct(v, 160); if (dm & 8) v = pct(v, 145); }
  }
  if ((am & 0x20000000) && is(A, 224)) v = pct(v, 105);
  if (am & 0x400) {
    if (is(A, 177, 1) && (dm & 0x80)) v = pct(v, 130);
    if (is(A, 178, 1) && (dm & 0x80)) v = pct(v, 130);
    if (is(A, 179, 1) && (dm & 0x80)) v = pct(v, 130);
    if (is(A, 174, 1) && (dm & 0x400)) v = pct(v, 135);
  }
  if ((am & 0x1000000) && is(A, 232)) v = pct(v, 105);
  if (am & 0x80) {
    if (is(A, 146) && (dm & 0x80)) v = mul(v, 2);
    if (is(A, 160, 1)) v = pct(v, 50);
    if (is(A, 149)) {
      if (dm & 0x20000000) v = pct(v, 180);
      if (dm & 0x1000000) v = pct(v, 180);
      if (dm & 0x4000) v = pct(v, 110);
    }
    if (is(A, 151)) v = pct(v, 130);
    if (is(A, 144) && (dm & 0x200000)) v = pct(v, 135);
  }
  if (is(A, 114) && (dm & 0x20)) v = pct(v, 130);
  if (is(A, 130, 1) && (dm & 0x20)) v = pct(v, 130);
  for (const t of [258, 259]) {
    if (is(A, t, 1)) {
      if (dm & 0x10108) v = pct(v, 115);
      if (is(D, 123)) v = pct(v, 120);
    }
  }
  if (!siegeBit) return v;
  if (is(A, 279) && isBuild(dt)) v = pct(v, 135);
  if (is(A, 276) && isBuild(dt)) v = pct(v, 110);
  if (is(A, 274) && isBuild(dt)) v = pct(v, 110);
  return v;
}

// Balance::compute_modifier @00581cc0
export function computeModifier(A, D, raw, ops, K) {
  if (isGaia(A.type) || isGaia(D.type)) return 100;
  const rows = indices(pack(A, ops)), cols = indices(pack(D, ops));
  let m = 100;
  for (const r of rows) for (const c of cols) m = pct(raw[r * RAW + c], m);
  return pct(typeDamage(A, D, ops, K), m);
}

// Balance::fill_tables second half: final_balance_table[a-50][d-50] = (short)compute_modifier(a, d).
export function computeBalance(dataDir, info) {
  const { types, constants } = info;
  const raw = readRawTable(dataDir, types);
  const ops = makeTypeOps(types);
  const out = [];
  for (let a = BASE_UNIT; a < END_BUILD; a++) {
    const row = new Array(END_BUILD - BASE_UNIT);
    for (let d = BASE_UNIT; d < END_BUILD; d++) row[d - BASE_UNIT] = i16(computeModifier(types[a], types[d], raw, ops, constants));
    out.push(row);
  }
  return out;
}
