// Rise of Nations rules loader: non-object types (techs, spells/crafts, bonuses), goods and the item.
// Reimplements (riseofnations.exe): TechType::init @0066c7b0, TechType::set_research @0066cba0,
// TechType::compute_ai_values @0066cdc0, TechType::add_preq_ai @0066c350, TypeData::get_preq @00668700,
// Type::find_preq @00668380, the alternatenames.xml block of Types::init @00669cc0, GoodType::init @0066ea20,
// GoodType::compute_largest_gather @0066e920 (called from Lands::init @0067e730), ItemType::init @00677bc0,
// SpellType::init @00674a80 (+ the cost copy after it in Types::init), BonusType::init @00670280,
// Type::load_cost @00663ba0. See techs.md for the spec.
//
// ctx: see CONTEXT.md. Extra optional input: ctx.gameInfo = {starting_technology, starting_technology2,
// ending_technology, game_rules} (GameInfo at the time Types::init runs; only get_preq/find_preq read it).
// Integer conventions: job_time/res_time/spell_flags are stored as unsigned (>>> 0) like the oracle reads
// them; char fields are sign-extended int8; ai[] entries are int16.
// Allocation uses ctx.newType when the integrator provides it, else the repo skeleton's newType.
import { newType as defaultNewType } from './types.mjs';

export const T = {
  GOODS: 0, NUM_BASIC_GOODS: 6, END_GOODS: 50, UNITS: 50, END_UNITS: 402, BUILDS: 414, END_BUILDS: 543,
  ITEM: 543, TECHS: 544, AGES: 544, END_AGES: 551, EPOCHS: 551, END_EPOCHS: 579, END_TECHS: 629,
  SPELLS: 629, DECLARE_WAR: 668, END_SPELLS: 684, BONUSES: 684, END_BONUSES: 806,
  // building TypeIndex constants used by the AI weighting
  VILLAGE: 414, TOWN: 415, UNIVERSITY: 420, GRANARY: 423, LUMBERMILL: 424, SMELTER: 425, REFINERY: 426,
  DOCK: 432, TEMPLE: 437, FORTX: 443,
  B_1B7: 0x1b7, B_1BB: 0x1bb, B_1BF: 0x1bf,
  // epoch line bases
  SCIENCE: 551, COMMERCE: 558, CIVIC: 565, MILITARY: 572, INTERNATIONAL_LAW: 571, SELECTIVE_SERVICE: 578,
  FOREIGN_TRADE: 684,
  TYPE_NONE: -1, TYPE_ANY_BUILDING: -2, TYPE_ANY_UNIT: -3,
};
// DESPOTISM_2 795, DESPOTISM_3 796, REPUBLIC_2 798, REPUBLIC_3 799, MONARCHY_2 801, DEMOCRACY_2 803: preq[1] = previous level
const GOV_BONUS_WITH_PREV = new Set([795, 796, 798, 799, 801, 803]);

const i8 = (v) => (v << 24) >> 24;
const i16 = (v) => (v << 16) >> 16;
const u32 = (v) => v >>> 0;
const isEpoch = (t) => t >= T.EPOCHS && t < T.END_EPOCHS;   // TypeData::is_epoch_type
const isAge = (t) => t >= T.AGES && t < T.END_AGES;         // TypeData::is_age_type
const isUnit = (t) => t >= T.UNITS && t < T.END_UNITS;      // TypeData::is_unit_type (gaia excluded)
const isBuild = (t) => t >= T.BUILDS && t < T.END_BUILDS;   // TypeData::is_build_type
const isTech = (t) => t >= T.TECHS && t < T.END_TECHS;      // TypeData::is_tech_type
// Virtual num_preq: TechTypeData/BuildTypeData 3; Unit/Good/BonusTypeData 2; TypeData (spells, base) 1.
function numPreq(t) {
  if (isTech(t) || (t >= T.BUILDS && t < T.END_BUILDS)) return 3;
  if (t < T.END_GOODS || (t >= T.UNITS && t < T.BUILDS) || t >= T.BONUSES) return 2;
  return 1;
}

function alloc(ctx, t, kind) {
  if (!ctx.types[t]) ctx.types[t] = (ctx.newType ?? defaultNewType)(t, kind);
  return ctx.types[t];
}

// Type::load_cost(el, dst): text of el, or (when empty) its "cost" attribute; both empty -> dst unchanged.
// Otherwise dst[0..5] = 0 and every "<int><letter>" token separated by '/' with a non-zero amount stores
// dst[res(letter)] = amount (last write wins). Letters: t timber, g wealth, k knowledge, m metal, o oil,
// anything else (incl. f, w, end of string) food.
export function loadCost(X, el, dst) {
  let s = X.text(el);
  if (!s.length) s = X.getAttrib(el, 'cost', '');
  if (!s.length) return;
  for (let i = 0; i < 6; i++) dst[i] = 0;
  for (const { amount, res } of X.costTokens(s)) dst[res] = amount;
}

function setName(ctx, o, name) { // Type::set_name @00663ae0
  o.name = name; o.type_name = name; o.display_name = ctx.lookupDisplayName ? ctx.lookupDisplayName(o, name) : name;
}

// ------------------------------------------------------------------------------------------------ techs
// TechType::init @0066c7b0. Pass 0: name/type_name (so pass 1 tech_key lookups see every tech). Pass 1: data.
export function initTech(ctx, el, t, pass) {
  const X = ctx.X, o = alloc(ctx, t, 'tech');
  o.type = t;
  if (pass === 0) { setName(ctx, o, X.getText(el, 'NAME')); o.type_name = o.name; return 0; }
  o.where = -1;
  o.grid_x = i8(X.getTextNum(el, 'GRID_X', -1)); o.grid_y = i8(X.getTextNum(el, 'GRID_Y', -1));
  // TEX_ID/TEX_COL/TEX_ROW/TEX_X/TEX_Y/TEX_CLIP: icon placement (TypeOut, UI only, not walked).
  o.tex = { id: X.getText(el, 'TEX_ID'), col: i8(X.getTextNum(el, 'TEX_COL', -1)), row: i8(X.getTextNum(el, 'TEX_ROW', -1)),
    x: i16(X.getTextNum(el, 'TEX_X', -1)), y: i16(X.getTextNum(el, 'TEX_Y', -1)), clip: i8(X.getTextNum(el, 'TEX_CLIP', -1)) };
  o.preq = [ctx.techKey(X.getText(el, 'PREQ0')), ctx.techKey(X.getText(el, 'PREQ1')), ctx.techKey(X.getText(el, 'PREQ2'))];
  o.age = X.getTextNum(el, 'AGE', -1);
  const jt = X.getTextNum(el, 'JOB_TIME', -1);
  o.res_time = u32(jt); o.job_time = u32(jt);
  loadCost(X, X.childOrCreate(el, 'COST'), o.costs);
  o.tribe_mask = X.binary(X.getText(el, 'TRIBE_MASK'));
  o.show = [ctx.techKey(X.getText(el, 'SHOW0')), ctx.techKey(X.getText(el, 'SHOW1'))];
  o.ai = new Array(11).fill(0); o.leader_off = 0;
  return 0;
}

// Types::init alternatenames block: ALTERNATENAMES/EPOCHTYPE (exactly 28, document order -> techs 551..578),
// each with exactly 8 STRING children -> alternate_name[0..7] (one per age). The name attribute is unused.
export function loadAlternateNames(ctx) {
  const X = ctx.X;
  const root = X.readXml(ctx.dataDir, 'alternatenames.xml', { lang: ctx.lang ?? null });
  const eps = X.children(X.child(root, 'ALTERNATENAMES'), 'EPOCHTYPE');
  if (eps.length !== 28) throw new Error(`alternatenames.xml: ${eps.length} EPOCHTYPEs, expected 28`);
  eps.forEach((ep, i) => {
    const s = X.children(ep, 'STRING');
    if (s.length !== 8) throw new Error(`alternatenames.xml: EPOCHTYPE ${i} has ${s.length} STRINGs, expected 8`);
    ctx.types[T.EPOCHS + i].alternate_name = s.map((e) => X.text(e));
  });
}

// TechType::set_research @0066cba0 (second pass over techrules.xml TECH, after bonuses are loaded).
export function setResearch(ctx, el, t) {
  const X = ctx.X, o = ctx.types[t];
  o.where = ctx.buildKey(X.getText(el, 'WHERE'));
  for (let b = T.BUILDS; b < T.END_BUILDS; b++) {
    const bt = ctx.types[b];
    if (bt && isA(ctx, bt, o.where, 0)) bt.build_flags = u32(bt.build_flags | 0x8000000); // "researches techs"
  }
  if (isEpoch(t)) {
    const q = Math.trunc((t - T.EPOCHS) / 7); // 0 science, 1 commerce, 2 civic, 3 military
    o.cat = q === 0 ? 3 : q === 1 ? 2 : q === 2 ? 1 : 0;
  } else o.cat = 3;
}

// ObjectTypeData::is(p, strict=0) with an empty is_list == ObjectTypeData::is_slow @00661ae0:
// type == p, or p >= 0 and (graft == p or (from >= 0 and types[from].is(p))). Non-object types: type == p.
export function isA(ctx, o, p, strict = 0) {
  if (o.type === p) return true;
  if (o.kind === 'tech' || o.kind === 'spell' || o.kind === 'bonus') return false;
  if (strict) throw new Error('isA strict not needed here');
  if (p < 0) return false;
  if (o.graft === p) return true;
  return o.from >= 0 ? isA(ctx, ctx.types[o.from], p, 0) : false;
}

// TypeData::get_preq @00668700 with leader = -1 (the iroquois early-government rule needs a leader, so it never
// applies here). Slots 1 and 2 depend on the game's starting/ending technology settings.
export function getPreq(ctx, o, i) {
  const types = ctx.types, gi = ctx.gameInfo ?? {};
  const st = gi.starting_technology ?? 0, st2 = gi.starting_technology2 ?? 0, end = gi.ending_technology ?? 7;
  if (i < 0 || i > 2) return T.TYPE_NONE;
  if (i === 0) return o.preq[0];
  if (i === 2) {
    const p = o.preq[2];
    if (p >= 0) {
      if (isEpoch(p)) {
        const q = getPreq(ctx, o, 1);
        if (q >= 0 && isEpoch(q) && types[p].cat === types[q].cat && p <= q) return T.TYPE_NONE;
      }
      if (isAge(p) && o.where === T.UNIVERSITY && end < types[p].age + 1) return T.TYPE_NONE;
    }
    return p;
  }
  let p = o.preq[1];
  const start = (gi.game_rules ?? 0) === 8 ? Math.min(st, st2) : st;
  if (p >= 0 && (start !== 0 || end < 7)) {
    if (isEpoch(p) && isUnit(o.type) && types[p].cat === 0) {
      const age = types[p].age;
      if (age < start) return T.TYPE_NONE;
      const v = Math.trunc(28 / (end - start + 1)) * (age - start + 1) + 3;
      p = Math.trunc(v / 4) + T.INTERNATIONAL_LAW;
      return p < T.MILITARY ? T.MILITARY : p > T.SELECTIVE_SERVICE ? T.SELECTIVE_SERVICE : p;
    }
    if (start !== 0 && isEpoch(p)) {
      const tp = types[p], age = tp.age;
      if (start <= age && 7 - start > 0) {
        const base = tp.cat === 1 ? T.CIVIC : tp.cat === 2 ? T.COMMERCE : tp.cat === 3 ? T.SCIENCE : T.MILITARY;
        let q = Math.trunc(((age - start + 1) * 7) / (7 - start)) - 1 + base;
        if (o.preq[1] < q) q = o.preq[1];
        const p2 = o.preq[2];
        if (p2 < 0 || !isEpoch(p2) || types[q].cat !== types[p2].cat || p2 <= q) return q;
      }
      return T.TYPE_NONE;
    }
  }
  return p;
}

// Type::find_preq @00668380: 1 if any effective preq slot (0 .. num_preq-1) equals x.
export function findPreq(ctx, o, x) {
  const n = numPreq(o.type);
  for (let i = 0; i < n; i++) if (getPreq(ctx, o, i) === x) return true;
  return false;
}

// TechType::add_preq_ai @0066c350: add `amount` to ai[idx] of every tech preq (effective slots 0..2), recursing
// while depth != 0 (depth -1 recurses through the whole preq tree).
function addPreqAi(ctx, o, idx, amount, depth) {
  for (let i = 0; i < 3; i++) {
    const p = getPreq(ctx, o, i);
    if (p < 0 || !isTech(p)) continue;
    const tp = ctx.types[p];
    tp.ai[idx] = i16(tp.ai[idx] + amount);
    if (depth !== 0) addPreqAi(ctx, tp, idx, amount, depth - 1);
  }
}

// TechType::compute_ai_values @0066cdc0. Runs for every tech (544..628 in order) after set_research.
export function computeAiValues(ctx, t) {
  const types = ctx.types, o = types[t], ai = o.ai, T_ = t;
  const add = (k, v) => { ai[k] = i16(ai[k] + v); };
  // 1. the tech itself
  if (isEpoch(t)) {
    if (o.cat === 3) { add(4, 1); add(10, 1); } else if (o.cat === 1) add(5, 1);
    else if (o.cat === 2) add(1, 1); else if (o.cat === 0) add(0, 1);
  } else if (o.where === T.TEMPLE) add(5, 2);
  else if (o.where === T.FORTX) { add(5, 1); add(0, 1); }
  // 2. units (50..401) that need it
  for (let u = T.UNITS; u < T.END_UNITS; u++) {
    const ut = types[u];
    if (!findPreq(ctx, ut, T_)) continue;
    if (ut.role & 0x10000) add(0, 1);
    if (ut.role & 0x200) { add(1, 1); add(9, 1); }
    if (ut.domain === 0) { if (ut.role & 0x10000) add(8, 1); } else {
      if (ut.carry !== 0) add(3, 1);
      if (ut.domain === 1) add(2, 1); else add(6, 1);
    }
  }
  // 3. buildings (414..542) that need it
  for (let b = T.BUILDS; b < T.END_BUILDS; b++) {
    const bt = types[b];
    if (!findPreq(ctx, bt, T_)) continue;
    const s = bt.from < 0 ? 2 : 1; // base buildings weigh double
    if (isA(ctx, bt, T.VILLAGE, 0)) {
      add(1, 1);
      if (b === T.TOWN) { add(5, 1); add(1, 1); addPreqAi(ctx, o, 1, 1, 0); }
    }
    if (bt.from < 0) {
      if (bt.build_flags & 0x40) { add(1, 2 * s); add(9, 2 * s); }
      if (bt.build_flags & 0x8000000) {
        add(1, 2 * s); add(4, 2 * s);
        if (b === 0x1a7 || b === 0x1a8 || b === 0x1aa || b === 0x1a9) add(9, 2 * s);
      }
      if (isA(ctx, bt, T.DOCK, 0)) { add(2, 1); addPreqAi(ctx, o, 2, 1, -1); }
    }
    if (isA(ctx, bt, T.DOCK, 0)) { add(2, 1); addPreqAi(ctx, o, 2, 1, 2); }
    let f = b; while (types[f].from >= 0) f = types[f].from; // BuildTypeData::fundamental_type
    const ft = types[f];
    if (ft.build_flags & 0x40) { add(1, s); add(9, s); } else if (ft.build_flags & 0x8000000) { add(1, s); add(4, s); }
    else if (isA(ctx, ft, T.B_1B7, 0) || isA(ctx, ft, T.B_1BB, 0) || isA(ctx, ft, T.B_1BF, 0) || ft.attack !== 0) add(0, s);
    else if (isA(ctx, ft, 0x1a7, 0) || isA(ctx, ft, 0x1a8, 0) || isA(ctx, ft, 0x1a9, 0) || isA(ctx, ft, 0x1aa, 0)) { add(1, 8); add(9, 8); }
    if ((ft.build_flags | 0) < 0) { // basic_type(fundamental) == fundamental; bit 31 = trains units
      for (let u = T.UNITS; u < T.END_UNITS; u++) {
        const ut = types[u];
        if (ut.where !== b) continue;
        if (ut.role & 0x180c0) add(0, s); else add(1, 1);
      }
    }
  }
  // 4. techs (544..628) that need it
  for (let k = T.TECHS; k < T.END_TECHS; k++) {
    if (!findPreq(ctx, types[k], T_)) continue;
    add(4, 1);
    if (isEpoch(k)) { add(4, 1); add(0, 1); }
    if (isAge(k)) { add(4, 2); add(0, 2); }
  }
  // 5. goods (0..49), spells (629..683), bonuses (684..805) that need it
  for (let g = 0; g < T.END_GOODS; g++) if (findPreq(ctx, types[g], T_)) { const v = g < 6 ? 4 : 1; add(9, v); add(1, v); }
  for (let s = T.SPELLS; s < T.END_SPELLS; s++) if (findPreq(ctx, types[s], T_)) { add(0, 4); add(1, 2); }
  for (let k = T.BONUSES; k < T.END_BONUSES; k++) if (findPreq(ctx, types[k], T_)) {
    if (k === T.FOREIGN_TRADE) add(1, 1);
    add(1, 1); add(4, 2);
  }
}

// ------------------------------------------------------------------------------------------------ goods
// GoodType::init @0066ea20 (resourcerules.xml RESOURCES/RESOURCE; 50, passes 0 and 1).
export function initGood(ctx, el, t, pass) {
  const X = ctx.X, o = alloc(ctx, t, 'good');
  if (pass === 0) {
    o.type = t;
    setName(ctx, o, X.getText(el, 'NAME'));
    o.graph_name = o.name; o.help_name = o.name; o.type_name = o.name;
    o.abbrev = X.getText(el, 'ABBREV');
    o.x_size = Math.max(1, X.getTextNum(el, 'X_SIZE', -1));
    o.y_size = Math.max(1, X.getTextNum(el, 'Y_SIZE', -1));
    o.block = Math.max(0, X.getTextNum(el, 'BLOCK', -1));
    o.exclude_ctw = Math.max(0, X.getTextNum(el, 'EXCLUDE_CTW', -1));
    o.random_rare_drop = Math.max(0, X.getTextNum(el, 'RANDOM_RARE_DROP', -1));
    o.preq[0] = ctx.techKey(X.getText(el, 'PREQ0'));
    o.preq[1] = ctx.techKey(X.getText(el, 'PREQ1'));
    o.obs = ctx.techKey(X.getText(el, 'OBS'));
    // TEX_*, TEX2_ID/X/Y/XS/YS: icon placement (UI only).
    return 0;
  }
  if (t < T.NUM_BASIC_GOODS) {
    const gr = (n) => ctx.goodKey(X.getText(el, n)), fr = (n) => X.fraction(X.getText(el, n), 0x100);
    o.undiscovered_cost_good = gr('UNDISC_COST_GOOD'); o.undiscovered_cost_rate = fr('UNDISC_COST_RATE');
    o.obsolete_cost_good = gr('OBS_COST_GOOD'); o.obsolete_cost_rate = fr('OBS_COST_RATE');
    o.undiscovered_support_good = gr('UNDISC_SUPPORT_GOOD'); o.undiscovered_support_rate = fr('UNDISC_SUPPORT_RATE');
    o.obsolete_support_good = gr('OBS_SUPPORT_GOOD'); o.obsolete_support_rate = fr('OBS_SUPPORT_RATE');
    o.obsolete_production_good = gr('OBS_PROD_GOOD'); o.obsolete_production_rate = fr('OBS_PROD_RATE');
    o.bonus_type = [-1, -1]; o.bonus_num = [0, 0];
  } else {
    for (let i = 0; i < 2; i++) {
      o.bonus_type[i] = ctx.goodKey(X.getText(el, 'BONUS_TYPE' + i));
      o.bonus_num[i] = X.getTextNum(el, 'BONUS_NUM' + i, -1);
    }
  }
  o.largest_gather = 0;
  o.desc = X.getText(el, 'DESC');          // then String::lookup(.., "DESC") via resourcerules_strings.xml (UI)
  o.abbrev = X.getText(el, 'DESC_ABBREV'); // then String::lookup(.., "DESC_ABBREV") (UI)
  return 0;
}

// GoodType::compute_largest_gather @0066e920, run by Lands::init @0067e730 (Game::init_non_rules_data, i.e.
// after Types::init) for the 6 basic goods only. Lands = rules.xml LANDS/LAND (exactly 9), 4 MAKE each
// (num attribute, type attribute -> good_key). Land indices 1 and 2 are skipped. Result clamped to [1, 2].
export function readLands(ctx) {
  const X = ctx.X, root = X.readXml(ctx.dataDir, 'rules.xml');
  const lands = X.children(X.child(root, 'LANDS'), 'LAND');
  if (lands.length !== 9) throw new Error(`rules.xml: ${lands.length} LANDs, expected 9`);
  return lands.map((l) => X.children(l, 'MAKE').map((m) => ({ num: X.getAttribNum(m, 'num', -1), good: ctx.goodKey(X.getAttrib(m, 'type', '')) })));
}
export function computeLargestGather(ctx, lands = readLands(ctx)) {
  for (let g = 0; g < T.NUM_BASIC_GOODS; g++) {
    const o = ctx.types[g]; let v = 0;
    lands.forEach((makes, i) => { if (i === 1 || i === 2) return; for (const m of makes.slice(0, 4)) if (m.good === g && m.num > v) v = m.num; });
    o.largest_gather = v < 1 ? 1 : v > 2 ? 2 : v;
  }
}

// ------------------------------------------------------------------------------------------------ item
// ItemType::init @00677bc0 (itemrules.xml ITEMS/ITEM, exactly 1 -> type 543; passes 0 and 1, pass 1 is a no-op).
export function initItem(ctx, el, t, pass) {
  const X = ctx.X, o = alloc(ctx, t, 'item');
  if (pass !== 0) return 0;
  o.type = t;
  setName(ctx, o, X.getText(el, 'NAME'));
  o.graph_name = o.name; o.type_name = o.name;
  o.abbrev = X.getText(el, 'ABBREV');
  // TEX_ID/TEX_COL/TEX_ROW/TEX_X/TEX_Y/TEX_CLIP: UI only.
  return 0;
}

// ------------------------------------------------------------------------------------------------ spells
// SpellType::init @00674a80 (craftrules.xml CRAFT, exactly 55 -> 629..683, single pass after units/items).
export function initSpell(ctx, el, t) {
  const X = ctx.X, o = alloc(ctx, t, 'spell'), types = ctx.types;
  o.use = [0, 0, 0, 0, 0, 0];
  o.type = t;
  const name = X.getText(el, 'NAME');
  o.name = name; o.type_name = name;
  o.display_name = ctx.lookupDisplayName ? ctx.lookupDisplayName(o, name) : name;
  o.graph_name = X.getText(el, 'GRAPH'); o.help_name = X.getText(el, 'GRAPH');
  const tn = X.getText(el, 'TYPENAME'); o.type_name = tn.length ? tn : name;
  o.grid_x = i8(X.getTextNum(el, 'GRID_X', -1)); o.grid_y = i8(X.getTextNum(el, 'GRID_Y', -1));
  o.grid_x2 = i8(X.getTextNum(el, 'GRID_X2', -1)); o.grid_y2 = i8(X.getTextNum(el, 'GRID_Y2', -1));
  o.from = objectKey(ctx, X.getText(el, 'FROM'));
  o.from2 = objectKey(ctx, X.getText(el, 'FROM2'));
  o.preq[0] = ctx.techKey(X.getText(el, 'PREQ0'));
  o.job_time = u32(X.getTextNum(el, 'JOB_TIME', -1));
  o.res_time = u32(X.getTextNum(el, 'RES_TIME', -1));
  o.duration = Math.imul(X.getTextNum(el, 'DURATION', -1), 15);
  o.duration_upgrade = Math.imul(X.getTextNum(el, 'DURATION_UPGRADE', -1), 15);
  o.mana = X.getTextNum(el, 'MANA', -1);
  o.spell_range = Math.imul(X.getTextNum(el, 'SPELL_RANGE', -1), 0xc0);
  o.spell_flags = X.unitFlags(X.getText(el, 'FLAGS')); // lower-cased; 'a'..: bit c-'a'; '1'..'9': bit c-0x17 (mod 32)
  loadCost(X, X.childOrCreate(el, 'COST'), o.costs);
  loadCost(X, X.childOrCreate(el, 'COST2'), o.use);
  o.tribe_mask = X.binary(X.getText(el, 'TRIBE_MASK'));
  // FROM: caster flags and the spell's where.
  if (o.from < 0) o.where = -1;
  else {
    if (isUnit(o.from)) { const u = types[o.from]; u.unit_flags2 = u32(u.unit_flags2 | 2); o.where = u.where; }
    else if (isBuild(o.from)) { const b = types[o.from]; b.build_flags = u32(b.build_flags | 0x20000000); o.where = o.from; }
    // FROM2 is only looked at when FROM >= 0.
    if (o.from2 >= 0) {
      if (isUnit(o.from2)) { const u = types[o.from2]; u.unit_flags2 = u32(u.unit_flags2 | 2); }
      else if (isBuild(o.from2)) { const b = types[o.from2]; b.build_flags = u32(b.build_flags | 0x20000000); }
    }
  }
  o.upgrade = -1;
  return 0;
}

// Types::object_key @00668ae0: unit_key (quiet) then build_key; -3 if neither (error).
export function objectKey(ctx, s) {
  const u = ctx.unitKey(s); if (u !== T.TYPE_ANY_UNIT) return u;
  return ctx.buildKey(s);
}

// Types::init after each SpellType::init: spells 669..683 (DECLARE_WAR1..7, CANCEL_ALLIANCE..7) take costs[0..5]
// from 668 (DECLARE_WAR). Done per spell inside the craft loop; 668 is already parsed by then.
export function postSpells(ctx) {
  const src = ctx.types[T.DECLARE_WAR];
  for (let t = T.DECLARE_WAR + 1; t < T.END_SPELLS; t++) ctx.types[t].costs = src.costs.slice();
}

// ------------------------------------------------------------------------------------------------ bonuses
// BonusType::init @00670280 (rules.xml TECHBONUSES/BONUS, exactly 122 -> 684..805, single pass).
export function initBonus(ctx, el, t) {
  const X = ctx.X, o = alloc(ctx, t, 'bonus');
  const preq = X.childOrCreate(el, 'PREQ');
  o.type = t;
  o.desc = X.getText(el, 'DESC'); // then String::lookup(desc, "BONUSDESC") via rules_string_lookup.xml (UI)
  o.preq[0] = ctx.techKey(X.getAttrib(preq, 'preq0', ''));
  o.preq[1] = GOV_BONUS_WITH_PREV.has(t) ? t - 1 : -1;
  return 0;
}
