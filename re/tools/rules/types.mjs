// Type core of the rules loader (docs/rules-spec.md): constructor defaults, name keys, units (UnitType::init),
// relative combat values, is-lists and finalize_init_all.
// Context object API used by the per-area loaders (re/tools/rules/*.mjs):
//   ctx.dataDir, ctx.lang, ctx.X (rulesxml.mjs), ctx.types[806] (null until allocated by Types::init),
//   ctx.constants {name: int}, ctx.tribes[24], ctx.cats {name: [{name,key,desc,data[6]}]},
//   ctx.unitKey/buildKey/techKey/goodKey(str, quiet) -> TypeIndex, ctx.catKey(catsName, str),
//   ctx.lookupDisplayName(type, name).
import * as X from '../rulesxml.mjs';

export const TYPE_NONE = -1, TYPE_ANY_BUILDING = -2, TYPE_ANY_UNIT = -3;

// Constructor defaults (Type::Type @00663010, ObjectType::ObjectType @0065f880, UnitType::UnitType @0061d270,
// BuildType::BuildType @006334d0, GoodType::GoodType @0066ff80, SpellTypeData::SpellTypeData @006749b0,
// TechType::TechType @004773a0). Fields the constructors leave uninitialised are set to 0 here and flagged.
export function newType(t, kind) {
  const o = {
    kind, type: 0, job_time: 0, res_time: 0, tribe_mask: -1, cat: 0, costs: [0, 0, 0, 0, 0, 0], preq: [-1, -1, -1],
    from: -1, where: -1, upgrade: -1, jump: -1, obs: -2, show: [-2, -2], modified: 0, grid_x: 0, grid_y: 0,
    name: '', display_name: '', graph_name: '', help_name: '', type_name: '', abbrev: '', desc: '',
  };
  if (kind === 'good' || kind === 'unit' || kind === 'build' || kind === 'item') Object.assign(o, {
    obj_masks: 0, attack: 0, to_hit: 0, attenuate: 0, recharge: 0, min_range: 0, max_range: 0, splash_area: 0,
    splash_percent: 0, ammo_per_att: 0, proj_speed: 0, hits: -1, armor: -1, domain: -1, los: 0, science_los: 0,
    guy_spacing: 0, x_spacing: -1, y_spacing: -1, abil: -1, x_size: 0, y_size: 0, guy_radius: 0, block_radius: 0,
    big_radius: 0, new_block_radius: 0, new_big_radius: 0, fly_high: 0, fly_low: 0, block_points: 0, graft: -1,
    special_upgrade: -1, special_upgrade_cost: 0, support: [-1, -1], support_cost: [0, 0], age: -1,
    is_list: { count: 0 }, is_strict_list: { count: 0 },
  });
  if (kind === 'unit') Object.assign(o, {
    cat: 0, attack: 0, unit_flags: 0, unit_flags2: 0, mode: 0, moves: 0, turn_speed: 0, role: 0, fire_proj: 0,
    second_max_range: 0, carry: 0, carry_size: 0, military_level: 0, research_premium_cost: 0,
    research_premium_time: 0, job_extra_time: 0, mana: 0, control_cost: 0, progression: 0, push_size: 0,
    push_circles: 0, target_size: 0, squad_size: 0, uber_size: 0, crew_size: 0, base_form: 0,
    relative_value: new Array(352).fill(0),
  });
  if (kind === 'build') Object.assign(o, {
    town_hits: 0, min_city_size: 0, misery_rate: 0, build_flags: 0, most_shots: 0, garrison_max: 0, base_arrows: 0,
    wonder_val: 0, plunder_value: 0, plunder_good: 0, behind_height: 0, to: 0, civ_graph_mask: 0,
    mask_name: '', doober_name: '',
  });
  if (kind === 'good') Object.assign(o, {
    undiscovered_cost_good: 0, obsolete_cost_good: 0, undiscovered_support_good: 0, obsolete_support_good: 0,
    obsolete_production_good: 0, undiscovered_cost_rate: 0, obsolete_cost_rate: 0, undiscovered_support_rate: 0,
    obsolete_support_rate: 0, obsolete_production_rate: 0, bonus_type: [0, 0], bonus_num: [0, 0], largest_gather: 0,
    block: 0, exclude_ctw: 0, random_rare_drop: 0,
  });
  if (kind === 'tech') Object.assign(o, { age: 0, ai: new Array(11).fill(0), leader_off: 0, alternate_name: new Array(8).fill('') });
  if (kind === 'spell') Object.assign(o, {
    spell_flags: 0, spell_range: 0, mana: 0, duration: 0, duration_upgrade: 0, from2: 0, use: [0, 0, 0, 0, 0, 0],
    grid_x2: 0, grid_y2: 0,
  });
  return o;
}

// Types::unit_key/build_key/tech_key/good_key (@006694a0/@00669330/@00669610/@006691c0): case-insensitive match
// on type_name over the class range, then "none" -> TYPE_NONE, "disable" -> TYPE_ANY_BUILDING, else
// (error unless quiet) TYPE_ANY_UNIT. Units: 50..401 (gaia 402..413 excluded); buildings 414..542;
// techs 544..628; goods 0..49.
export function makeKeys(ctx) {
  const key = (lo, hi) => (s) => {
    for (let t = lo; t < hi; t++) { const o = ctx.types[t]; if (o && X.iequals(s, o.type_name)) return t; }
    if (X.iequals(s, 'none')) return TYPE_NONE;
    if (X.iequals(s, 'disable')) return TYPE_ANY_BUILDING;
    return TYPE_ANY_UNIT;
  };
  ctx.unitKey = key(50, 402); ctx.buildKey = key(414, 543); ctx.techKey = key(544, 629); ctx.goodKey = key(0, 50);
}

// ---------------------------------------------------------------- units (UnitType::init @0061ab50)
const UB = 50; // BASE_UNITTYPES
const isAgeType = (t) => t >= 544 && t < 551;

// ObjectTypeData::is_slow (@00661ae0): t is this type, its graft, or (recursively) anything its FROM type is.
// Strict form: this type, or its graft when this is a unit and the graft target lacks unit flag 'y'.
export function isSlow(ctx, o, t, strict) {
  if (o.type === t) return true;
  if (!strict) {
    if (t < 0) return false;
    if (o.graft === t) return true;
    if (o.from >= 0) return isSlow(ctx, ctx.types[o.from], t, false);
    return false;
  }
  if (!(o.kind === 'unit')) return false;
  return o.graft === t && !(ctx.types[t].unit_flags & 0x1000000);
}
// ObjectTypeData::is (@0065f7d0): uses is_list / is_strict_list once built, else is_slow. TypeData::is = equality.
export function is(ctx, o, t, strict = false) {
  if (!o.is_list) return o.type === t; // non-object types
  if (o.type === t) return true;
  const list = strict ? o.is_strict_list : o.is_list;
  if (!list.count) return isSlow(ctx, o, t, strict);
  return !strict && t < 0 ? false : list.v.includes(t);
}
const hasObjMask = (o, m) => (o.obj_masks & m) !== 0;

function unitPass0(ctx, o, el) {
  o.name = X.getText(el, 'NAME'); o.type_name = o.name; o.display_name = ctx.lookupDisplayName(o, o.name);
  o.graph_name = X.getText(el, 'GRAPH');
  const tn = X.getText(el, 'TYPENAME'); o.type_name = tn.length ? tn : o.name;
}
function unitPass1(ctx, o, el) {
  o.where = ctx.buildKey(X.getText(el, 'WHERE'));
  o.from = ctx.unitKey(X.getText(el, 'FROM'));
  o.obs = -2;
  o.jump = ctx.unitKey(X.getText(el, 'JUMP'));
  o.tribe_mask = X.binary(X.getText(el, 'TRIBE_MASK'));
  o.graft = ctx.unitKey(X.getText(el, 'GRAFT'));
}
// Upgrade link: FROM's `upgrade` points at this unit unless this unit is flagged '1' (can't upgrade to) or it is a
// unique unit whose FROM is a mainline unit (unless flag 'h').
function linkUpgrade(ctx, o, t) {
  if (o.from < 0) return;
  const f = ctx.types[o.from];
  if (!hasObjMask(o, 0x4000000) && (o.graft < 0 || f.graft >= 0 || (o.unit_flags & 0x80))) f.upgrade = t;
}
// ObjectType::load_support (@00661160) / load_upgrade (@006615b0): only when the element has a COST child.
function loadSupport(o, el) {
  if (!X.child(el, 'COST')) return;
  let s = X.getText(el, 'SUPPORT'); if (!s.length) s = X.getAttrib(X.child(el, 'COST'), 'support');
  o.support = [-1, -1]; o.support_cost = [0, 0];
  const toks = X.costTokens(s); for (let i = 0; i < toks.length && i < 2; i++) { o.support[i] = toks[i].res; o.support_cost[i] = toks[i].amount; }
}
function loadUpgrade(o, el) {
  if (!X.child(el, 'COST')) return;
  let s = X.getText(el, 'UPGRADE'); if (!s.length) s = X.getAttrib(X.child(el, 'COST'), 'upgrade');
  o.special_upgrade = -1; o.special_upgrade_cost = 0;
  const toks = X.costTokens(s); if (toks.length) { o.special_upgrade = toks[0].res; o.special_upgrade_cost = toks[0].amount; }
}
// Type::load_cost (@00663ba0): text of the COST element (or its cost= attribute); zeroes costs[] only when non-empty.
export function loadCost(costEl, costs) {
  let s = X.text(costEl); if (!s.length) s = X.getAttrib(costEl, 'cost');
  if (!s.length) return;
  costs.fill(0); for (const k of X.costTokens(s)) costs[k.res] = k.amount;
}
// UnitType::determine_roles (@0061c320). role bits (RoleIndex names in docs/rules-spec.md).
function determineRoles(ctx, o) {
  let r = 0; const t = o.type;
  if (t >= 50 && t <= 53) r = 0x200;
  if (o.domain === 2) r |= 0x1000;
  else if (o.domain === 0) { r |= 0x40000; if (is(ctx, o, 69 /*SCOUT*/)) r |= 0x10; if (o.cat === 1) r |= 0xc; }
  else { r |= 0x80000; if (is(ctx, o, 323 /*BARK*/)) r |= 0x10; }
  if (o.carry !== 0) r |= 0x8000;
  if (o.attack !== 0 && o.cat !== 5 && o.cat !== 4 && !(r & 0x200)) {
    const before = r; r = before | 0x10000;
    if (o.domain === 1) r = before | 0x12000;
    if (o.domain === 2) r |= 0x4000;
    if (o.domain === 0 && o.cat === 0) { r |= 0x800; r |= is(ctx, o, 132 /*HOPLITES*/) ? 2 : 0x100000; }
    r |= o.max_range !== 0 ? 0x400 : 1;
  } else r |= 0x100;
  o.role = r;
}
function unitPass2(ctx, o, el, t) {
  const C = ctx.constants;
  o.help_name = X.getText(el, 'GRAPH');
  o.grid_x = (X.getTextNum(el, 'GRID_X') << 24) >> 24; o.grid_y = (X.getTextNum(el, 'GRID_Y') << 24) >> 24;
  o.unit_flags = X.unitFlags(X.getText(el, 'FLAGS')); o.unit_flags2 = 0;
  o.to_hit = X.getTextNum(el, 'TO_HIT');
  o.attenuate = Math.abs(X.getTextNum(el, 'ATTENUATE'));
  o.attack = X.i32(X.getTextNum(el, 'ATTACK') * 10); o.mode = 0;
  o.cat = ctx.catKey('unit_cats', X.getText(el, 'CAT'));
  const rng = X.getText(el, 'RANGE');
  o.min_range = X.convertInt(rng);
  const dash = rng.indexOf('-');
  const max = dash < 0 ? o.min_range : X.convertInt(rng.slice(dash + 1));
  o.max_range = max; o.second_max_range = max;
  if (o.unit_flags & 0x400) o.max_range = 0;
  o.splash_area = X.getTextNum(el, 'SPLASH'); o.splash_percent = X.getTextNum(el, 'SPLASH_PERCENT');
  o.ammo_per_att = X.getTextNum(el, 'AMMO_PER_ATT'); o.recharge = X.getTextNum(el, 'RECHARGE');
  o.armor = X.getTextNum(el, 'ARMOR');
  o.obj_masks = X.objMask(X.getText(el, 'OBJ_MASK'));
  const dom = X.getText(el, 'DOMAIN'); o.domain = X.iequals(dom, 'Sea') ? 1 : X.iequals(dom, 'Air') ? 2 : 0;
  o.moves = X.getTextNum(el, 'MOVES');
  o.turn_speed = X.degreesToAngle(X.getTextNum(el, 'TURN_SPEED'));
  o.hits = X.getTextNum(el, 'HITS');
  o.proj_speed = X.getTextNum(el, 'PROJ_SPEED');
  if (o.attack === 0) o.fire_proj = (o.unit_flags & 0x200000) ? 1 : 0;
  else { if (o.max_range !== 0 && o.proj_speed === 0) o.proj_speed = 200; o.fire_proj = 1; }
  o.carry = X.getTextNum(el, 'CARRY'); o.carry_size = X.getTextNum(el, 'CARRY_SIZE');
  o.control_cost = X.getTextNum(el, 'POP'); o.progression = X.getTextNum(el, 'PROGRESSION');
  o.job_time = o.res_time = X.getTextNum(el, 'JOB_TIME') >>> 0;
  const jet = X.getText(el, 'JOB_EXTRA_TIME');
  if (!jet.length) o.job_extra_time = 0;
  else { const a = X.wtoi(jet), k = jet.indexOf('/'), d = k < 0 ? 1 : X.wtoi(jet, k + 1); o.job_extra_time = d === 0 ? 0 : X.cdiv(X.i32(a * 100), d); }
  o.research_premium_time = X.fraction(X.getText(el, 'RESEARCH_PREMIUM_TIME'), 256);
  o.research_premium_cost = X.fraction(X.getText(el, 'RESEARCH_PREMIUM_COST'), 256);
  o.mana = X.getTextNum(el, 'MANA');
  o.los = X.getTextNum(el, 'LOS'); o.science_los = X.getTextNum(el, 'SCIENCE_LOS');
  o.fly_high = X.getTextNum(el, 'FLY_HIGH'); o.fly_low = X.getTextNum(el, 'FLY_LOW');
  o.guy_spacing = X.i32(C.unit_guy_spacing * X.getTextNum(el, 'GUY_SPACING'));
  o.x_spacing = X.i32(C.unit_formation_spacing * X.getTextNum(el, 'X_SPACING'));
  o.y_spacing = X.i32(C.unit_formation_spacing * X.getTextNum(el, 'Y_SPACING'));
  o.x_size = o.y_size = Math.min(X.getTextNum(el, 'CIRCLE_RADIUS'), 10);
  const br = X.getTextNum(el, 'BLOCK_RADIUS');
  o.push_circles = Math.max(1, Math.min(100, X.getTextNum(el, 'PUSH_CIRCLES')));
  let ps = Math.max(1, Math.min(100, X.getTextNum(el, 'PUSH_SIZE'))); if (o.push_circles === 1) ps = br;
  o.push_size = X.i32(C.unit_block_radius * ps);
  o.target_size = X.i32(X.getTextNum(el, 'TARGET_SIZE') * C.unit_block_radius);
  o.preq[0] = ctx.techKey(X.getText(el, 'PREQ0')); o.preq[1] = ctx.techKey(X.getText(el, 'PREQ1'));
  loadCost(X.childOrCreate(el, 'COST'), o.costs);
  loadSupport(o, el); loadUpgrade(o, el);
  o.squad_size = 1;
  o.uber_size = X.getTextNum(el, 'UBER_SIZE'); o.crew_size = X.getTextNum(el, 'CREW_SIZE');
  if (o.preq[0] >= 0 && o.preq[1] === -1 && isAgeType(o.preq[0]) && o.attack !== 0) o.preq[1] = o.preq[0] + 28;
  determineRoles(ctx, o);
  o.new_block_radius = br; o.guy_radius = o.block_radius = X.i32(C.unit_block_radius * br);
  o.big_radius = X.cdiv(X.i32((o.squad_size - 1) * o.guy_spacing), 2) + o.guy_radius;
  o.new_big_radius = X.cdiv(o.big_radius, C.unit_block_radius);
  o.block_points = br > 1 ? 12 : 8;
  linkUpgrade(ctx, o, t);
  if (o.graft >= 0) {
    const g = ctx.types[o.graft];
    o.grid_x = g.grid_x; o.grid_y = g.grid_y;
    if (o.upgrade < 0) o.upgrade = g.upgrade;
    for (let tb = 0; tb < 24; tb++) if (o.tribe_mask & (1 << tb)) ctx.tribes[tb].graft[o.graft - UB] = t;
  }
  if (o.where >= 0 && !(o.unit_flags & 0x8000)) {
    const w = ctx.types[o.where]; w.build_flags = (w.build_flags | 0x80000000) >>> 0;
    if (o.attack !== 0 && !(o.role & 0x100) && !is(ctx, w, 414 /*SMALL_CITY*/)) w.build_flags = (w.build_flags | 0x40000000) >>> 0;
  }
  o.base_form = 0;
}
function unitPass3(ctx, o, t) {
  for (let tb = 0; tb < 24; tb++) {
    const tr = ctx.tribes[tb], bit = 1 << tb;
    if (tr.graft[t - UB] === t || (o.tribe_mask & bit)) continue;
    for (let j = 50; j < 402; j++) {
      const u = ctx.types[j];
      if (j !== t && !(u.tribe_mask & bit) && u.graft === t) tr.graft[j - UB] = tr.graft[t - UB];
    }
  }
}
export function initUnit(ctx, el, t, pass) {
  const o = ctx.types[t];
  o.military_level = -1; // set on every pass, before the pass switch
  if (pass === 0) { o.type = t; unitPass0(ctx, o, el); return; }
  o.type = t;
  if (pass === 1) unitPass1(ctx, o, el);
  else if (pass === 2) unitPass2(ctx, o, el, t);
  else if (pass === 3) unitPass3(ctx, o, t);
  else if (pass === 4) linkUpgrade(ctx, o, t);
}

// UnitType::fill_relative_values (@0061be50): relative_value[j-50] for every combat unit j, 8.8 fixed point - 1.0.
export function fillRelativeValues(ctx, o) {
  if (!(o.role & 0x10000)) return;
  const A = o.obj_masks | 0, h2 = (x) => X.cdiv(X.i32(x * 3), 2), half = (x) => X.cdiv(x, 2);
  for (let j = 50; j < 402; j++) {
    const u = ctx.types[j]; o.relative_value[j - 50] = 0;
    if (!(u.role & 0x10000)) continue;
    const D = u.obj_masks | 0; let v = 0x100;
    if ((A & 0x1000000) && o.max_range === 0 && (D & 0x90408)) v = 0x180;
    if ((A & 0x20000000) && o.max_range === 0 && ((D & 0x10000) || (D & 0x84000))) v = h2(v);
    if ((A & 0x80) && (D & 0x81800)) v = h2(v);
    if ((A & 0x40000000) && (D & 0x1000)) v = h2(v);
    if (is(ctx, o, 123) && (D & 0x10108)) v = h2(v);
    if ((A & 0x108) && is(ctx, u, 123)) v = half(v);
    if ((D & A) & 0x2000) {
      v = h2(v);
      if (is(ctx, o, 340) && is(ctx, u, 346)) v = h2(v);
      if (is(ctx, o, 334) && !is(ctx, u, 334)) v = h2(v);
      if (!is(ctx, o, 334) && is(ctx, u, 334)) v = half(v);
      if (is(ctx, o, 334)) v = half(v);
    }
    let l08b;
    if (A & 0x10) { if (D & 0x80000) v = h2(v); l08b = true; } else l08b = is(ctx, o, 213);
    if (l08b && (D & 0x1010) === 0x1000 && !is(ctx, u, 213)) v = h2(v);
    const A22 = A & 0x400000;
    if (A22 && (D & 1) && !(A & 0x20040)) v = X.cdiv(v, 8);
    if ((A & 0x430000) === 0x20000 && (D & 1)) v = half(v);
    if (A & 0x400) { if (D & 0x80) v = h2(v); if (D & 0x4000) v = h2(v); if (!A22 && (D & 0x10000)) v = half(v); }
    if (A & 0x4000) { if (D & 0x80) v = X.cdiv(X.i32(v * 5), 4); if (!A22 && (D & 0x10000)) v = X.cdiv(X.i32(v * 2), 3); }
    if (A & 0x40) { if (D & 0x80) v = X.i32(v * 2); if ((A & 0x20) && (D & 0x1000)) v = X.cdiv(X.i32(v * 5), 4); }
    if ((A & 0x100000) && (D & 1)) v = h2(v);
    let skip119 = false, goto213 = true;
    if (A & 0x10000000) {
      if (!(A & 0x8000000) && A < 0) { if (D & 0x10000000) v = X.i32(v * 3); else goto213 = false; }
    }
    if (goto213 && (D & 0x10000000) && !(D & 0x8000000)) {
      v = half(v); if (A < 0) { v = X.i32(v * 3); if (A & 0x200000) v = X.i32(v * 2); } skip119 = true;
    }
    if (!skip119 && is(ctx, o, 281)) v = X.cdiv(v, 3);
    o.relative_value[j - 50] = (((v < 0x300 ? (v & 0xffff) : 0x300) - 0x100) << 16) >> 16;
  }
}

// ---------------------------------------------------------------- finalisation
// ObjectType::init_is_list (@00661dd0): ascending TypeIndex lists of everything is_slow(t,0) / is_slow(t,1) accepts.
// SimpleArray growth step 1, so capacity == count; flags byte 0.
export function initIsList(ctx, o) {
  o.is_list = { count: 0 }; o.is_strict_list = { count: 0 };
  const a = [], s = [];
  for (let t = 0; t < 806; t++) { if (isSlow(ctx, o, t, false)) a.push(t); if (isSlow(ctx, o, t, true)) s.push(t); }
  const arr = (v) => (v.length ? { count: v.length, capacity: v.length, block: 1, flags: 0, v } : { count: 0 });
  o.is_list = arr(a); o.is_strict_list = arr(s);
}
// UnitType::init_final_flags (@0061dc70): unit_flags2 bits derived from upgrade lines.
function initFinalFlags(ctx, o) {
  const i = (t) => is(ctx, o, t);
  if (i(265) || i(279) || o.type === 61 || o.type === 62 || o.type === 400 || i(123) || i(317) || i(278)) o.unit_flags2 |= 4;
  if (i(63) || i(352) || i(354) || i(356)) o.unit_flags2 |= 0x40;
  if (i(54)) o.unit_flags2 |= 0x20;
  if (i(59) || i(318)) o.unit_flags2 |= 8;
  if (i(69)) o.unit_flags2 |= 0x10;
  if (i(320) || i(318)) o.unit_flags = (o.unit_flags | 0x10) >>> 0;
  if (i(123) || i(131)) o.unit_flags2 |= 1;
}
// UnitType::init_spellcasters (@0061aae0): unit_flags2 |= 2 when any unit up the FROM chain (or its graft) has it.
function initSpellcasters(ctx, o) {
  let t = o.type;
  for (;;) {
    let u = ctx.types[t];
    if (u.unit_flags2 & 2) break;
    if (u.graft >= 0) { t = u.graft; u = ctx.types[t]; if (u.unit_flags2 & 2) break; }
    t = u.from; if (t < 0) return;
  }
  o.unit_flags2 |= 2;
}
// ObjectType::finalize_init_all (@0065f4a0): units 50..401 then buildings 414..542.
export function finalizeInitAll(ctx) {
  for (let t = 50; t < 402; t++) { const o = ctx.types[t]; initIsList(ctx, o); initFinalFlags(ctx, o); initSpellcasters(ctx, o); }
  for (let t = 414; t < 543; t++) {
    const o = ctx.types[t]; initIsList(ctx, o);
    if (is(ctx, o, 417) || is(ctx, o, 421) || is(ctx, o, 422)) o.build_flags = (o.build_flags | 0x10000000) >>> 0;
  }
}
