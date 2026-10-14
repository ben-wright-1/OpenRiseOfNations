#!/usr/bin/env node
// Reference model of Rise of Nations production costs and times (docs/production-spec.md §2), from the decompiled
// TypeData::get_cost @00664090, Type::pay_cost @006681f0, LeaderData::get_support_count @006da110,
// LeaderData::calc_science_discount @006da630, ObjectData::train_time @006508c0, TypeData::time @00663f20,
// TypeData/UnitTypeData::research_time @006639a0/@0061d760, Wall::update_construct_time @0063d560,
// BuildData::construct_time @0062d5c0, Wall::do_construct @006434d0 and Unit::do_build @005eebf0.
// Reads the rules from the user's install (never writes to it) through rulesdump.mjs.
//
//   node re/tools/production-ref.mjs Citizen --count 9            cost/time of the 10th citizen
//   node re/tools/production-ref.mjs Barracks --count 1 --tribe Indians
//   node re/tools/production-ref.mjs "Small City" --count 1 --tribe Bantu
//   node re/tools/production-ref.mjs "Medieval Age" --age 1 --sci 1
//   node re/tools/production-ref.mjs Phalanx --upgrade --count 10  research cost of the Phalanx upgrade, 10 hoplites
//   node re/tools/production-ref.mjs Barracks --builders 3        construction frames with 3 builders
//   node re/tools/production-ref.mjs --examples | --selftest
//
// Options: --count N (existing+queued of the kind, see countsFor()), --tribe NAME, --age A (ages researched, 0 =
// Ancient), --mil/--civ/--com/--sci N (library epochs per category = LeaderDataEncrypt.epoch[0..3]), --wonder NAME
// (owned, repeatable), --rare NAME (repeatable), --preq BONUS (DESPOTISM_1, MONARCHY_1, ... repeatable),
// --upgrade (tech bit of the type clear = price/time of researching it), --wonders N (wonders built+unbuilt, for wonder
// ramps), --cities N, --builders N, --bspeed N (BUILDINGS_FASTER_1..3 owned), --no-convert.
//
// All arithmetic is int32 with C truncating division (Math.trunc), as the binary does it. Branches that are not
// modelled are marked "NOT MODELLED" with the address of the code they would come from.

import { fileURLToPath } from 'node:url';
import { loadRules, startGame, DEFAULT_DATA } from './rulesdump.mjs';

// ---------------------------------------------------------------- int helpers
const i32 = (x) => x | 0;
const mul = (a, b) => Math.imul(a, b);
const div = (a, b) => { if (b === 0) throw new Error('division by zero'); return i32(a / b); };
const pct = (pctOff, v) => div(mul(100 - pctOff, v), 100);      // ((100 - p) * v) / 100, the common discount form
const shr8 = (x) => div(x, 256);                                   // (x + ((x >> 31) & 0xff)) >> 8 = trunc(x / 256)

// ---------------------------------------------------------------- constants (TypeIndex, PDB enum)
export const T = {
  CITIZEN: 50, CITIZEN_K: 51, SCHOLAR: 52, SCHOLAR_K: 53, GENERAL: 54, SPY: 58, CARAVAN: 59, MERCHANT: 61,
  MERCHANTDUTCH: 62, MILITIA: 66, PARTISAN: 68, HOPLITES: 132, ELEPHANT0: 261, AAGUN: 281, BOMBER: 304,
  FIGHTERBOMBER: 308, NUKE: 315, FIRERAFT: 334, CARRIER: 351, PRESIDENT: 355, CHARLES: 369, FURTRAPPER: 400,
  SMALLCITY: 414, FARM: 417, MINE: 419, UNIVERSITY: 420, OILWELL: 421, OILPLATFORM: 422, GRANARY: 423,
  BARRACKS: 427, STABLE: 428, AUTOPLANT: 429, SIEGEFACTORY: 430, FACTORY: 431, DOCK: 432, ANCHORAGE: 433,
  SHIPYARD: 434, LIBRARY: 435, MARKET: 436, TEMPLE: 437, SENATE: 438, TOWER: 439, FORTX: 443, AIRBASE: 447,
  MISSILESILO: 520, LOOKOUT: 521, AIRDEFENSE: 523, PYRAMIDS: 526, HANGING: 528, COLOSSEUM: 530, TIKAL: 532,
  PORCELAIN: 533, REDFORT: 534, ANGKOR: 535, VERSAILLES: 536, LIBERTY: 537, KREMLIN: 538, SUPERCOLLIDER: 541,
  SPACEPROGRAM: 542, CLASSICAL: 544, TAXATION: 589, LITERACY: 615, METAL_ALLOYS: 620, DISBAND: 666, RAZE: 646,
};
export const BONUS = { SPIES_GENERALS_CREATED_FASTER: 770, SPIES_GENERALS_CHEAPER: 771, BUILDINGS_CREATED_FASTER: 787,
  RESEARCH_FASTER: 788, TROOPS_FASTER: 789, DESPOTISM_1: 794, DESPOTISM_2: 795, DESPOTISM_3: 796, MONARCHY_1: 800,
  MONARCHY_2: 801, DEMOCRACY_1: 802, DEMOCRACY_2: 803, SOCIALISM_1: 804, INSTANT_UNIT_BONUS: 696 };
// tribe index == tribe-bonus id (LeaderData::has_tribe_bonus @006e1370 compares tribes[tribe]+0x54, = the index)
export const TRIBE = { AZTECS: 0, MAYA: 1, INCA: 2, BANTU: 3, NUBIANS: 4, GREEKS: 5, ROMANS: 6, EGYPTIANS: 7, TURKS: 8,
  SPANISH: 9, FRENCH: 10, BRITISH: 11, GERMANS: 12, RUSSIANS: 13, CHINESE: 14, JAPANESE: 15, KOREANS: 16, MONGOLS: 17,
  IROQUOIS: 18, LAKOTA: 19, AMERICANS: 20, INDIANS: 21, DUTCH: 22, PERSIANS: 23 };
// rare resources = good index (LeaderData::has_rare(r) tests bit r-6 of rare | rare_conquest)
export const RARE = { WINE: 8, PAPYRUS: 9, DYE: 10, SILK: 11, SALT: 13, HORSES: 14, RUBBER: 15, SULPHUR: 16,
  SILVER: 18, TOBACCO: 19, MARBLE: 20, FURS: 21, COTTON: 22, BISON: 23, SUGAR: 27, ALUMINUM: 30, WOOL: 33, COAL: 34,
  URANIUM: 35, GYPSUM: 40, BEAVER: 41, GOLD: 42, SALMON: 46, IRON: 47, MAIZE: 48, INCENSE: 49, RELICS: 7 };
const RES = ['food', 'timber', 'wealth', 'knowledge', 'metal', 'oil'];
const M_CIVILIAN = 4, M_FOOT = 0x20, M_MOUNTED = 0x1000, M_VEHICLE = 0x200000, M_MISSILE = 0x8000000;
const ROLE_COMBAT = 0x10000;

const isUnit = (t) => t >= 50 && t < 414;
const isBuild = (t) => t >= 414 && t < 543;
const isWonder = (t) => t >= 526 && t < 543;
const isTech = (t) => t >= 544 && t < 629;
const isAge = (t) => t >= 544 && t < 551;
const isEpoch = (t) => t >= 551 && t < 579;      // library techs
const isFinal = (t) => t >= 579 && t < 583;
const isSpell = (t) => t >= 629 && t < 684;
// vslot 0x64 is_city = BuildTypeData::is_city @00472bc0 = is(SMALLCITY): 414..416 and Forbidden City 531;
// vslot 0xfc is_fort @00472ba0 = is(FORTX): 443..446 and Red Fort 534; is_tower @00472a30 = is(TOWER): 439..442
let RULES = null;
const isCity = (t) => isA(RULES.types[t], 414);
const isFort = (t) => isA(RULES.types[t], 443);

// ---------------------------------------------------------------- rules / leader state
export function load(dataDir = DEFAULT_DATA) { const ctx = loadRules(dataDir); startGame(ctx); RULES = ctx; return ctx; }

export function findType(ctx, key) {
  if (/^\d+$/.test(String(key))) return ctx.types[+key];
  const k = String(key).toLowerCase();
  for (let t = 50; t < 684; t++) { const o = ctx.types[t]; if (o && o.name && o.name.toLowerCase() === k) return o; }
  throw new Error(`no type named "${key}"`);
}

// A leader state: only what get_cost / train_time read. Counts are maps type -> n.
export function newLeader(over = {}) {
  return {
    tribe: TRIBE.SPANISH,          // Spanish: no bonus in get_cost/train_time (9 only appears in spell timing)
    ages: 0, epoch: [0, 0, 0, 0],  // LeaderDataEncrypt.ages / epoch[mil, civ, com, sci]
    unresearched: new Set(),       // types whose LeaderData.tech bit is clear (=> research price)
    preq: new Set(),               // bonus TypeIndex values for which has_preq is true
    wonders: new Set(), rares: new Set(),
    numUnits: {}, numQueued: {}, numBuildings: {},
    cityNum: 1, scholarMilitia: 0, nukesUsed: 0, missilesUsed: 0,
    wondersAll: 0, teamWondersExtra: 0,   // get_wonders + get_unbuilt_wonders; team (teams_locked) extra count
    unbuiltSCSP: false, teamUnbuiltSCSP: false,
    speed: { building: 0, troops: 0, ships: 0, vehicles: 0 },
    ...over,
  };
}
export function newGame(over = {}) {
  return { startTech: 0, endTech: 7, techCost: 3, numPlayers: 2, numNations: 2, handicap: false, ctw: false,
    scenarioFree: false, editor: false, teamsLocked: false, aiSpeed: 1, ...over };
}

const n = (m, t) => m[t] ?? 0;
const tribeBonus = (L, b) => L.tribe === b;                      // has_tribe_bonus (starting town / city assumed)
const hasRare = (L, r) => L.rares.has(r);
const hasWonder = (L, w) => L.wonders.has(w);
const hasPreq = (L, b) => L.preq.has(b);
const techBit = (L, t) => !L.unresearched.has(t);
function isA(o, x) { if (o.type === x) return true; const l = o.is_list; return !!(l && l.v && l.v.slice(0, l.count).includes(x)); }

// LeaderData::tribe_can_type @006d9380 (unique-unit flag rule for game flag 8 / tribe+0x58 NOT MODELLED)
function tribeCanType(ctx, L, t) { return ((ctx.types[t].tribe_mask >>> (L.tribe & 31)) & 1) !== 0; }
// LeaderData::get_graft @006e30f0 + UnitTypeData::type_graft @0061d790 (tribes[tribe].graft[t-50] for t in 50..401)
function graft(ctx, L, t) {
  if (t < 0) return t;
  if (tribeCanType(ctx, L, t)) return t;
  if (t >= 50 && t < 402) return ctx.tribes[L.tribe].graft[t - 50];
  return t;
}

// type_avail(good, 1) for the six resources (has_preq + type_eligible): simplified to the age preqs of
// resourcerules.xml (knowledge/metal need Classical 544, oil Industrial 548) plus the Greek/German early rules of
// LeaderData::has_preq @006db810. Obsolete goods (has_preq but not eligible) are NOT MODELLED.
function resAvail(ctx, L, r) {
  const C = ctx.constants;
  if (r <= 2) return true;
  if (r === 3) return L.ages >= 1 || (tribeBonus(L, TRIBE.GREEKS) && C.greek_knowledge_early !== 0);
  if (r === 4) return L.ages >= 1 || (tribeBonus(L, TRIBE.GERMANS) && C.german_metal_early !== 0);
  return L.ages >= 5;
}

// ---------------------------------------------------------------- LeaderData::get_support_count @006da110
export function supportCount(ctx, L, t) {
  const o = ctx.types[t];
  if (!isUnit(t)) return 0;
  if ((o.progression & 1) && o.attack !== 0) {
    const cls = (w) => { let s = 0; for (const [k, v] of Object.entries(L.numUnits)) { const u = ctx.types[+k];
      if (u.attack !== 0 && u.where === w && (u.progression & 1)) s += v; }
      for (const [k, v] of Object.entries(L.numQueued)) { const u = ctx.types[+k];
      if (isUnit(+k) && u.attack !== 0 && u.where === w && (u.progression & 1)) s += v; } return s; };
    // barracks_units+barracks_queued etc. are kept by track_unit_type/queue_up for every attack!=0 unit of that
    // `where` (🟡 the leader counters do not test progression; this model only sums progression&1 units)
    if (o.where === T.BARRACKS || o.where === T.STABLE || o.where === T.SIEGEFACTORY || o.where === T.DOCK) return cls(o.where);
    if (o.domain === 2) { let s = 0; for (const m of [L.numUnits, L.numQueued]) for (const [k, v] of Object.entries(m))
      if (isUnit(+k) && ctx.types[+k].domain === 2 && ctx.types[+k].attack !== 0) s += v; return s; }
  }
  let c = n(L.numQueued, t) + n(L.numUnits, t);
  if (t === 315 || t === 316) c += L.nukesUsed;
  if (t === 313 || t === 314) c += L.missilesUsed;
  // Americans free bombers (americans_free_bomber per Airbase/...): NOT MODELLED (@006da110 tail)
  return c;
}

// ---------------------------------------------------------------- TypeData::get_cost @00664090
// Returns the amount of resource r (0..5) for type o, as TypeData::get_cost(r, who, obj, loc, refund, convert, -1).
// opt: { refund (param_5), convert (param_6, default 1), producerUnassimilated, trace: [] }
export function getCost(ctx, o, r, L, G, opt = {}) {
  const C = ctx.constants, t = o.type, steps = opt.trace;
  const note = (what, v) => { if (steps) steps.push(`${RES[r]}: ${what} -> ${v}`); };
  // spells, RADARSPELL, DOW / cancel alliance: see spec §2.1.9; only the factor is modelled here
  if (isSpell(t)) throw new Error('spell costs: NOT MODELLED (get_cost spell branch @00664181 / @00666230)');
  let rr = r;
  // Egyptian scholars pay food instead of wealth (egyptian_scholars_food, 0 in the stock data) @0066411b
  if ((t === 52 || t === 53) && C.egyptian_scholars_food !== 0 && tribeBonus(L, TRIBE.EGYPTIANS)) rr = r === 0 ? 2 : r === 2 ? 0 : r;
  let v = o.costs[rr];
  note('base costs[r]', v);

  if (isUnit(t)) v = unitCost(ctx, o, rr, v, L, G, note);
  else if (isBuild(t)) v = buildCost(ctx, o, rr, v, L, G, opt, note);
  else if (isTech(t)) v = techCost(ctx, o, rr, v, L, G, note);
  else { v = mul(v, C.tech_cost_factor); note('other type x tech_cost_factor', v); }
  if (v === null) return 0;   // a "return 0" branch (free units/buildings)

  // ---- common tail @00666e3a (switchD_00666b91_default)
  if (rr === 1 && hasRare(L, RARE.COAL)) { v = pct(C.coal_timber_cost, v); note('coal', v); }
  else if (rr === 0 && hasRare(L, RARE.SUGAR)) { v = pct(C.sugar_food_cost, v); note('sugar', v); }
  else if (rr === 2 && hasRare(L, RARE.GOLD)) { v = pct(C.gold_wealth_cost, v); note('gold', v); }
  else if (rr === 4 && hasRare(L, RARE.IRON)) { v = pct(C.iron_metal_cost, v); note('iron ore', v); }
  if (hasRare(L, RARE.GYPSUM)) { v = pct(C.gypsum_cost, v); note('gypsum', v); }
  if (opt.producerUnassimilated && isUnit(t)) { v = mul(v, 2); note('unassimilated producer x2', v); }
  // super_missiles (enemy Supercollider, constant 0): NOT MODELLED (@00667025)
  if (tribeBonus(L, TRIBE.INDIANS) && isUnit(t) && [261, 262, 263, 264].some((e) => isA(o, e))) { v = pct(C.indians_elephant_cost, v); note('indians elephants', v); }
  // ---- undiscovered-resource conversion @00667186 (param_6)
  if (opt.convert ?? 1) {
    for (let g = 0; g < 6; g++) {
      if (resAvail(ctx, L, g)) continue;
      const good = ctx.types[g];
      // has_preq(g) false = undiscovered (obsolete branch NOT MODELLED)
      if (good.undiscovered_cost_good === rr) {
        const sub = getCost(ctx, o, g, L, G, { ...opt, trace: null, convert: 1 });
        v = i32(v + shr8(mul(sub, good.undiscovered_cost_rate)));
        note(`+ ${RES[g]} ${sub} x ${good.undiscovered_cost_rate}/256`, v);
      }
    }
  }
  return v;
}

function unitCost(ctx, o, rr, v, L, G, note) {
  const C = ctx.constants, t = o.type, U = ctx.types;
  // (free-unit modes: beaver-pelts flag, scenario units_free) NOT MODELLED except the merchant beaver rule
  if (hasRare(L, RARE.BEAVER) && ((o.unit_flags2 & 0x10) || t === T.MERCHANT || t === T.MERCHANTDUTCH || t === T.FURTRAPPER)) { note('beaver pelts: free', 0); return null; }
  const bit = techBit(L, t);
  if (bit) {
    // queued upgrades of this unit's line add their extra price @00664181..006646ae
    for (let q = 50; q < 402; q++) {
      if (!n(L.numQueued, q)) continue;
      let hit = graft(ctx, L, U[q].from) === t;
      if (!hit) { let j = graft(ctx, L, U[t].jump); while (j >= 0) { if (j === q) { hit = true; break; } j = graft(ctx, L, U[j].jump); } }
      if (hit) { const d = U[q].costs[rr] - o.costs[rr]; if (d > 0) { v += d; note(`queued upgrade ${U[q].name} +diff`, v); } }
    }
  }
  const rampBase = mul(v, C.unit_cost_factor);     // iVar20, used for the ramp caps
  v = rampBase; note('x unit_cost_factor', v);
  const where = o.where;
  if (where === T.BARRACKS && hasRare(L, RARE.SALT)) { v = div(mul(v, 9), 10); note('salt barracks 9/10', v); }
  if (tribeBonus(L, TRIBE.MONGOLS) && where >= 0 && isA(U[where], T.STABLE)) { v = pct(C.mongol_stable_cost, v); note('mongols stable', v); }
  if (tribeBonus(L, TRIBE.FRENCH)) {
    if (where === T.SIEGEFACTORY || where === T.FACTORY) { v = pct(C.french_siege_cost, v); note('french siege', v); }
    else if (isA(o, T.GENERAL)) { v = pct(C.french_special_cost, v); note('french special', v); }
  }
  if (tribeBonus(L, TRIBE.JAPANESE)) {
    if (where === T.BARRACKS) {
      const k = C.japanese_barracks_cost;
      if (k < 0) { const a = Math.min(L.ages, L.epoch[0]); v = div(mul(mul(a, k) + 100, v), 100); }
      else v = pct(k, v);
      note('japanese barracks', v);
    } else if (where === T.DOCK || where === T.ANCHORAGE || where === T.SHIPYARD) { v = pct(C.japanese_ship_cost, v); note('japanese ships', v); }
  }
  if (tribeBonus(L, TRIBE.GERMANS) && isA(o, T.FIRERAFT)) { v = pct(C.german_submarine_cost, v); note('german submarine line', v); }
  if (tribeBonus(L, TRIBE.AMERICANS)) {
    if (where === T.AIRBASE) { v = pct(C.americans_aircraft, v); note('americans airbase', v); }
    if (isA(o, T.CARRIER)) { v = pct(C.americans_aircraft, v); note('americans carrier', v); }
    if (isA(o, T.FIGHTERBOMBER)) { v = pct(C.americans_aircraft, v); note('americans 308-line', v); }
  }
  if (tribeBonus(L, TRIBE.BRITISH) && isA(o, T.AAGUN)) { v = pct(C.british_aa_cost, v); note('british AA', v); }
  if (tribeBonus(L, TRIBE.NUBIANS) && (isA(o, T.CARAVAN) || t === 61 || t === 62 || t === 400)) { v = pct(C.nubian_merchants, v); note('nubian merchants', v); }
  if (tribeBonus(L, TRIBE.RUSSIANS) && isA(o, T.SPY)) { v = pct(C.russian_spy_cost, v); note('russian spy', v); }
  if (tribeBonus(L, TRIBE.ROMANS) && isA(o, T.HOPLITES)) { v = pct(C.roman_legion_cheap, v); note('roman hoplite line', v); }
  if (where === T.BARRACKS || where === T.STABLE || where === T.AUTOPLANT) {
    v = pct(C.terra_cotta_cost, v);   // unconditional (no wonder test), constant 0
    if (hasWonder(L, T.ANGKOR)) { v = pct(C.angkor_ships, v); note('angkor (barracks/stable/autoplant units)', v); }
  }
  if (where === T.STABLE || where === T.AUTOPLANT) {
    if (hasRare(L, RARE.HORSES)) { v = pct(C.horses_stable_cost, v); note('horses', v); }
    if (hasRare(L, RARE.RUBBER)) { v = pct(C.rubber_autoplant_cost, v); note('rubber', v); }
  }
  if ((where === T.SIEGEFACTORY || where === T.FACTORY) && hasRare(L, RARE.SULPHUR)) { v = pct(C.sulphur_siege_cost, v); note('sulphur', v); }
  if ((isA(o, T.SPY) || isA(o, T.GENERAL)) && hasPreq(L, BONUS.SPIES_GENERALS_CHEAPER)) { v = pct(C.spy_general_cost, v); note('spies/generals cheaper', v); }
  // ^ the binary uses is(58,strict=1)/is(54,strict=1); strict is_list NOT MODELLED (own type or is_list)
  if (t === 50 || t === 51) {
    if (hasRare(L, RARE.WOOL)) { v = pct(C.wool_citizens_cost, v); note('wool', v); }
    if (tribeBonus(L, TRIBE.TURKS)) { v = pct(C.turk_citizen, v); note('turk citizen', v); }
  }
  if (o.domain === 1) { if (hasWonder(L, T.ANGKOR)) { v = pct(C.angkor_ships, v); note('angkor ships', v); } }
  else if (o.domain === 2) {
    if (hasRare(L, RARE.ALUMINUM)) { v = pct(C.aluminum_air_cost, v); note('aluminum', v); }
    if (hasWonder(L, T.SPACEPROGRAM)) { v = pct(C.space_air_cost, v); note('space program air', v); }
  }
  if (where === T.BARRACKS) {
    const k = hasPreq(L, BONUS.DESPOTISM_3) ? C.despotism_military_cheaper3 : hasPreq(L, BONUS.DESPOTISM_2) ? C.despotism_military_cheaper2
      : hasPreq(L, BONUS.DESPOTISM_1) ? C.despotism_military_cheaper : null;
    if (k !== null) { v = pct(k, v); note('despotism barracks', v); }
  }
  if (hasWonder(L, T.LIBERTY)) {
    if (o.obj_masks & M_MISSILE) { v = pct(C.liberty_missiles, v); note('liberty missiles', v); }
    else if (isA(o, T.BOMBER)) { v = pct(C.liberty_bombers, v); note('liberty bombers', v); }
  }
  if (hasRare(L, RARE.URANIUM) && isA(o, T.NUKE)) { v = pct(C.uranium_nuke_cost, v); note('uranium', v); }
  if (n(L.numUnits, T.CHARLES)) { v = div(mul(C.charles_unit_cost, v), 100); note('archduke charles', v); }

  if (!bit && !(o.unit_flags & 0x80)) {
    // ---- research price of a unit upgrade @00664e7e..
    if (hasRare(L, RARE.WINE)) { v = pct(C.wine_unit_upgrades, v); note('wine', v); }
    v = shr8(mul(shr8(mul(C.research_premium, v)), o.research_premium_cost)); note('research premium (cost)', v);
    if (o.special_upgrade === rr) { v += o.special_upgrade_cost; note('special_upgrade_cost', v); }
    if (t < 66 || t > 68) {
      for (let q = 50; q < 402; q++) {   // refit: every unit (and queued unit) of a predecessor type
        let hit = q === graft(ctx, L, U[t].from);
        if (!hit) { let j = graft(ctx, L, U[q].jump); while (j >= 0) { if (j === t) { hit = true; break; } j = graft(ctx, L, U[j].jump); } }
        if (!hit) continue;
        let d = o.costs[rr] - U[q].costs[rr];
        if (U[q].costs[rr] === 0) d = div(d, 2);
        if (d <= 0) continue;
        d = Math.min(d, C.unit_refit_max_cost);
        const cnt = n(L.numQueued, q) + n(L.numUnits, q);
        if (cnt) { v += mul(mul(C.unit_cost_factor, cnt), d); note(`refit ${cnt} x ${U[q].name} (d=${d})`, v); }
      }
    }
    if (o.role & ROLE_COMBAT) {
      let d = L.epoch[0] - o.military_level;
      if (d > 0) {
        d = mul(C.military_upgrade_discount, d);
        const span = G.endTech - G.startTech + 1;
        if (span < 8) d = (mul(span, d) + 7) >> 3;
        v = pct(d, v); if (v < 0) v = 0; note('military upgrade discount', v);
      }
      if (tribeBonus(L, TRIBE.AMERICANS) && o.domain === 0) { v = pct(C.americans_upgrade_discount, v); note('americans upgrade', v); }
      if (tribeBonus(L, TRIBE.DUTCH) && o.domain === 1) { v = pct(C.dutch_ship_discount, v); note('dutch ships', v); }
    }
    return v;
  }
  // ---- RoN escalating price ("ramp") @00665538..
  let cnt = supportCount(ctx, L, t);
  if (t === 50 || t === 51) cnt += supportCount(ctx, L, 66) + supportCount(ctx, L, 67) - L.scholarMilitia + supportCount(ctx, L, 68);
  else if (t === 52 || t === 53) cnt += L.scholarMilitia;
  if (isA(o, T.NUKE)) cnt += L.nukesUsed;
  if (cnt > 0) {
    if (o.progression & 2) cnt = div(mul(cnt + 1, cnt), 2);
    let extra = 0, cap;
    if (t === 52 || t === 53) {
      cap = div(mul(C.unit_scholar_ramp_max, rampBase), 100);
      for (let k = 7; k < cnt; k += 7) extra += cnt - k;
    } else {
      const worker = t === 50 || t === 51 || t === 61 || t === 62 || t === 400 || (o.unit_flags2 & 8);
      const max = worker ? C.unit_worker_ramp_max : (o.obj_masks & M_CIVILIAN) ? C.unit_other_civilian_ramp_max : C.unit_military_ramp_max;
      cap = div(mul(max, rampBase), 100);
    }
    for (let i = 0; i < 2; i++) {
      if (o.support[i] !== rr) continue;
      let add = mul(o.support_cost[i], cnt) + extra;
      if (cap !== 0 && cap < add) add = cap;
      if (hasRare(L, RARE.MAIZE)) add = pct(C.maize_ramping_bonus, add);
      v += add; note(`ramp slot ${i}: ${o.support_cost[i]} x ${cnt}${extra ? ` + ${extra}` : ''} (cap ${cap})`, v);
    }
  }
  if (o.role & ROLE_COMBAT) {
    let lvl = o.military_level; if (lvl < 1) lvl = 1;
    let d = L.epoch[0] - lvl;
    if (d > 0) {
      d = mul(C.military_unit_discount, d);
      const span = G.endTech - G.startTech + 1;
      if (span < 8) { d = (mul(span, d) + 7) >> 3; if (d < 1) d = 1; }
      v = pct(d, v); if (v < 0) v = 0; note('military unit discount', v);
    }
  }
  if ((hasPreq(L, BONUS.MONARCHY_1) || hasPreq(L, BONUS.MONARCHY_2)) && where === T.STABLE) { v = pct(C.monarchy_unit_prod, v); note('monarchy stable', v); }
  if (hasPreq(L, BONUS.SOCIALISM_1) && (where === T.SIEGEFACTORY || where === T.AIRBASE || where === T.DOCK)) { v = pct(C.socialism_unit_bonus, v); note('socialism', v); }
  if (hasRare(L, RARE.SALMON) && o.domain === 1) { v = pct(C.salmon_naval_cost, v); note('salmon', v); }
  return v;
}

// building ramp count @006657ee..00665aa9
export function buildRampCount(ctx, L, o, refund = 0) {
  const t = o.type;
  if (isWonder(t)) {
    if (refund) return 0;
    let w = L.wondersAll, c = L.teamWondersExtra;   // teams_locked ? team_wonders - w + team_unbuilt : 0
    if (w > 3) c += w - 3;
    if (w > 6) c += w - 6;
    if (t === T.SUPERCOLLIDER || t === T.SPACEPROGRAM) {
      if (L.unbuiltSCSP) return c + 2 * (w + 1);
      if (L.teamUnbuiltSCSP) return c + 1 + 2 * w;
    }
    return c + 2 * w;
  }
  // cities: every city counts (num_buildings[0..2] + num_queued[414..416])
  if (isCity(t)) return [414, 415, 416].reduce((s, k) => s + n(L.numBuildings, k) + n(L.numQueued, k), 0);
  let c = n(L.numQueued, t) + n(L.numBuildings, t);
  if (t === T.OILPLATFORM) c += n(L.numBuildings, T.OILWELL) + n(L.numQueued, T.OILWELL);
  else if (t === T.OILWELL) c += n(L.numBuildings, T.OILPLATFORM) + n(L.numQueued, T.OILPLATFORM);
  const base = c;
  if ([427, 428, 429, 430, 431, 447, 520].includes(t)) {      // military production buildings
    if (base > 2) c = 2 * base - 2;
    if (base > 5) c += 2 * base - 10;
    if (base > 8) c += 3 * base - 24;
    if (base > 12) c += 4 * base - 48;
  }
  if (t >= 443 && t <= 446) {                                 // forts
    const b = c;
    if (b > 1) c = 2 * b - 1;
    if (b > 3) c += b - 3;
    if (b > 5) c += b - 5;
    if (b > 7) c += b - 7;
  }
  return c;
}

function buildCost(ctx, o, rr, v, L, G, opt, note) {
  const C = ctx.constants, t = o.type, U = ctx.types;
  if (G.scenarioFree) { note('scenario buildings_free', 0); return null; }
  if (tribeBonus(L, TRIBE.IROQUOIS) && isA(o, T.SENATE) && C.iroquois_quick_senate !== 0 && !L.everSenate) { note('iroquois first senate free', 0); return null; }
  v = mul(v, C.build_cost_factor); note('x build_cost_factor', v);
  if (!techBit(L, t) && (o.build_flags & 4)) { v = shr8(mul(C.research_premium, v)); note('research premium', v); }
  const cnt = buildRampCount(ctx, L, o, opt.refund);
  for (let i = 0; i < 2; i++) {
    if (o.support[i] !== rr) continue;
    let add = mul(mul(o.support_cost[i], C.build_support_factor), cnt);
    if (isWonder(t)) add = div(add, 2);
    if (tribeBonus(L, TRIBE.INDIANS) && !isCity(t)) {
      if (isWonder(t) || (t >= 439 && t <= 446) || (t >= 521 && t <= 525)) add = div(add + 1, 2); else add = 0;   // explicit type list
    }
    v += add; note(`ramp slot ${i}: ${o.support_cost[i]} x ${C.build_support_factor} x ${cnt}${isWonder(t) ? ' /2' : ''}`, v);
  }
  if (isCity(t) && !isWonder(t)) {
    if (C.bantu_city_cost !== 0 && tribeBonus(L, TRIBE.BANTU)) { v = pct(C.bantu_city_cost, v); note('bantu city', v); }
    if (hasWonder(L, T.PYRAMIDS)) { v = pct(C.pyramids_city_discount, v); note('pyramids city', v); }
  }
  if (isWonder(t)) {
    if (hasRare(L, RARE.MARBLE)) { v = pct(C.marble_wonder_cost, v); note('marble', v); }
    if (tribeBonus(L, TRIBE.EGYPTIANS)) { v = pct(C.egyptian_wonders, v); note('egyptian wonders', v); }
  }
  if (isA(o, T.TOWER) && tribeBonus(L, TRIBE.KOREANS)) { v = pct(C.korean_tower_cost, v); note('korean tower', v); }
  if ((isFort(t) || isA(o, T.TOWER)) && !isWonder(t)) {
    if (hasWonder(L, T.COLOSSEUM)) { v = pct(C.colosseum_fort_cost, v); note('colosseum fort', v); }
    if (tribeBonus(L, TRIBE.ROMANS)) { v = pct(C.roman_fort_cost, v); note('roman fort', v); }
  }
  if (isA(o, T.TEMPLE) && hasWonder(L, T.TIKAL)) { v = pct(C.tikal_temple_cost, v); note('tikal temple', v); }
  if (t === T.MARKET) { if (tribeBonus(L, TRIBE.NUBIANS)) { v = pct(C.nubian_merchants, v); note('nubian market', v); } }
  else if ((t === T.LIBRARY || t === T.UNIVERSITY) && tribeBonus(L, TRIBE.GREEKS)) { v = pct(C.greek_library_cost, v); note('greek library/university', v); }
  const mayaExcl = [427, 428, 429, 431, 430, 447, 432, 433, 434, 443, 444, 445, 446, 520, 439, 440, 441, 442, 521, 522, 523, 524, 525];
  if (tribeBonus(L, TRIBE.MAYA) && rr === 1 && !mayaExcl.includes(t) && !isWonder(t)) { v = pct(C.maya_building_timber, v); note('maya timber', v); }
  if (tribeBonus(L, TRIBE.BRITISH) && isA(o, T.AIRDEFENSE)) { v = pct(C.british_aa_cost, v); note('british air defense', v); }
  if (t === T.MINE) { if (tribeBonus(L, TRIBE.GERMANS)) { v = pct(C.german_mine_cost, v); note('german mine', v); } }
  else if (t === T.FARM) { if (tribeBonus(L, TRIBE.JAPANESE)) { v = pct(C.japanese_farms_cheaper, v); note('japanese farm', v); } }
  else if (t >= 427 && t <= 431 && hasWonder(L, T.COLOSSEUM)) { v = pct(C.colosseum_barracks_cost, v); note('colosseum barracks', v); }
  if (isA(o, T.LOOKOUT) && hasWonder(L, T.LIBERTY)) { v = pct(C.liberty_air_defense, v); note('liberty air defense', v); }
  return v;
}

// LeaderData::calc_science_discount @006da630
export function scienceDiscount(ctx, L, t, v) {
  if (!isTech(t)) return v;
  let a = ctx.types[t].age;
  if (!isAge(t) && !isEpoch(t)) a += 1;
  return v - div(mul(mul(L.epoch[3] - a, ctx.constants.tech_science_discount), v), 100);
}

function techCost(ctx, o, rr, v, L, G, note) {
  const C = ctx.constants, t = o.type;
  if (G.scenarioFree) { note('scenario techs_free', 0); return null; }
  if (G.ctw && isAge(t) && v !== 0) { v = C.ctw_age_cost; note('CTW age cost', v); }
  v = mul(C.tech_cost_factor, v); note('x tech_cost_factor', v);
  if (t >= 623 && t <= 628 && tribeBonus(L, TRIBE.AMERICANS)) { v = 0; note('americans governments free', v); }
  if (hasRare(L, RARE.INCENSE) && o.where === T.TEMPLE) { v = pct(C.incense_temple_research, v); note('incense', v); }
  // Greek classical/age-knowledge conversion (constants 0 in the stock data): NOT MODELLED (@00666430..)
  v = scienceDiscount(ctx, L, t, v); note('calc_science_discount', v);
  // age-behind discount (needs other leaders' ages): pass L.agesBehind = 1 (one ahead) / 2 (two or more ahead)
  if (isAge(t) && L.agesBehind) {
    const d = rr === 3 ? C.tech_age_behind_knowledge_discount : C.tech_age_behind_discount;
    if (L.agesBehind >= 2) v = d * 2 < 100 ? div(mul(mul(50 - d, v), 2), 100) : 0;
    else v = d < 100 ? pct(d, v) : 0;
    note('age-behind discount', v);
  }
  if (isFinal(t)) {
    let k = 0; for (let f = 579; f < 583; f++) if (L.finals?.has(f)) k += 8;
    k = mul(C.ramp_final, k);
    v = div(mul((k >> 3) + 100, v), 100); note('ramp_final', v);
  }
  const tc = G.techCost;
  if (tc < 2) v = div(mul(v, 2), 3);
  else if (tc > 4) {
    if (tc < 7) v = (!isAge(t) && rr !== 3) ? div(mul(v, 5), 4) : div(mul(v, 3), 2);
    else v = (!isAge(t) && rr !== 3) ? div(mul(v, 3), 2) : mul(v, 2);
  }
  if (tc < 2 || tc > 4) note(`tech_cost setting ${tc}`, v);
  if (isAge(t) || isEpoch(t)) {
    if (tribeBonus(L, TRIBE.GREEKS) && rr !== 3) { v = pct(C.greek_research_cost, v); note('greek research', v); }
    // CTW great thinker cards: NOT MODELLED
  } else {
    if (o.where === T.GRANARY && hasRare(L, RARE.BISON)) { v = pct(C.bison_granary_cost, v); note('bison', v); }
    if (o.where !== T.LIBRARY) {
      if (hasPreq(L, BONUS.DEMOCRACY_2)) { v = pct(C.democracy_tech_bonus2, v); note('democracy 2', v); }
      else if (hasPreq(L, BONUS.DEMOCRACY_1)) { v = pct(C.democracy_tech_bonus, v); note('democracy 1', v); }
    }
  }
  if ((t >= 609 && t <= 614) || (t >= T.METAL_ALLOYS && t < T.METAL_ALLOYS + 3)) {
    if (hasWonder(L, T.HANGING) && C.hanging_gardens_industry_discount) { v = pct(C.hanging_gardens_industry_discount, v); note('hanging gardens', v); }
    if (tribeBonus(L, TRIBE.GERMANS)) { v = pct(C.german_industry_discount, v); note('german industry', v); }
  }
  if (t >= T.TAXATION && t < T.TAXATION + 4 && tribeBonus(L, TRIBE.BRITISH)) { v = pct(C.british_taxation_discount, v); note('british taxation', v); }
  if (rr !== 3 && hasWonder(L, T.VERSAILLES)) {
    const ok = isAge(t) || isEpoch(t) || isFinal(t) || (t >= T.LITERACY && t < T.LITERACY + 5 ? C.versailles_affects_university_upgrades : C.versailles_affects_misc_upgrades);
    if (ok) { v = pct(C.versailles_knowledge_costs, v); note('versailles', v); }
  }
  if (isAge(t) && hasRare(L, RARE.SILVER)) { v = pct(C.silver_age_cost, v); note('silver', v); }
  if (isEpoch(t)) {
    switch (o.cat) {
      case 0:
        if (hasRare(L, RARE.FURS)) { v = pct(C.furs_military, v); note('furs', v); }
        { const k = hasPreq(L, BONUS.DESPOTISM_3) ? C.despotism_military_cheaper3 : hasPreq(L, BONUS.DESPOTISM_2) ? C.despotism_military_cheaper2 : hasPreq(L, BONUS.DESPOTISM_1) ? C.despotism_military_cheaper : null;
          if (k !== null) { v = pct(k, v); note('despotism', v); } }
        if (tribeBonus(L, TRIBE.TURKS)) { v = pct(C.turk_military_cheap, v); note('turks', v); }
        break;
      case 1:
        if (hasRare(L, RARE.DYE)) { v = pct(C.dye_civic_commerce, v); note('dye', v); }
        if (tribeBonus(L, TRIBE.PERSIANS)) { v = pct(C.persians_civic_discount, v); note('persians', v); }
        break;
      case 2:
        if (hasRare(L, RARE.SILK)) { v = pct(C.silk_commerce, v); note('silk', v); }
        if (tribeBonus(L, TRIBE.DUTCH)) { v = pct(C.dutch_commerce_discount, v); note('dutch', v); }
        break;
      case 3:
        if (hasRare(L, RARE.PAPYRUS)) { v = pct(C.papyrus_science_military, v); note('papyrus', v); }
        if (tribeBonus(L, TRIBE.CHINESE)) { v = pct(C.chinese_science_discount, v); note('chinese', v); }
        if (tribeBonus(L, TRIBE.AMERICANS)) { v = pct(C.americans_science_discount, v); note('americans science', v); }
        break;
    }
  }
  return v;
}

// The six amounts Type::pay_cost @006681f0 would charge (unavailable resources are 0, never charged).
export function payCost(ctx, o, L, G, opt = {}) {
  return RES.map((_, r) => (resAvail(ctx, L, r) ? getCost(ctx, o, r, L, G, opt) : 0));
}

// ---------------------------------------------------------------- times (1/100 frame units; accel_* per frame)
// TypeData::time @00663f20 (non-spell): job_time * 100
const typeTime = (o) => mul(o.job_time, 100);
// TypeData::research_time @006639a0 + UnitTypeData::research_time @0061d760
function researchTime(ctx, o) {
  let v = (mul(typeTime(o), ctx.constants.research_tick_premium) >>> 0) >>> 8;
  if (isUnit(o.type)) v = shr8(mul(v, o.research_premium_time));
  return v;
}

// ObjectData::train_time @006508c0 for a queue item of type o at a producer (opt.producerUnassimilated).
export function trainTime(ctx, o, L, G, opt = {}) {
  const C = ctx.constants, t = o.type, U = ctx.types, steps = opt.trace;
  const note = (w, v) => { if (steps) steps.push(`${w} -> ${v}`); };
  const bit = techBit(L, t);
  let v = isUnit(t) && !bit ? researchTime(ctx, o) : typeTime(o);
  note(isUnit(t) && !bit ? 'research_time' : 'job_time x 100', v);
  // DISBAND/RAZE (recursive on the producer's type): NOT MODELLED (@0065093b)
  if (isA(o, T.NUKE) && hasRare(L, RARE.URANIUM)) { v = div(mul(v, 100), C.uranium_nuke_speed + 100); note('uranium', v); }
  if (isUnit(t) && bit) {
    v = div(mul(C.unit_rate_base, v), 100);
    const cap = mul(v, 3);
    v = mul(mul(n(L.numUnits, t), o.job_extra_time), C.unit_rate_progression) + v;
    if (v < 0 || cap < 0) v = 0; else if (cap < v) v = cap;
    note(`unit_rate_base, + ${n(L.numUnits, t)} x job_extra_time x progression, cap x3`, v);
    if (G.handicap) { /* ((200 - get_handicap) * v) / 200: NOT MODELLED (handicap tables) */ }
    if (opt.president) { v = div(mul(v, 100), C.thepresident_unit_build_speed + 100); note('the president', v); }
    const where = o.where;
    if (tribeBonus(L, TRIBE.MONGOLS) && o.attack !== 0 && where >= 0 && isA(U[where], T.STABLE)) { v = div(mul(v, 100), C.mongol_stable_speed + 100); note('mongols', v); }
    if (tribeBonus(L, TRIBE.JAPANESE)) {
      if (where === T.BARRACKS) { let k = C.japanese_barracks_speed; if (k < 0) k = -mul(k, Math.min(L.ages, L.epoch[0])); v = div(mul(v, 100), k + 100); note('japanese barracks', v); }
      if (isA(o, T.CARRIER)) { v = div(mul(v, 100), C.japanese_carrier_speed + 100); note('japanese carrier', v); }
    }
    if (tribeBonus(L, TRIBE.CHINESE) && (t === 50 || t === 51 || t === 61 || t === 62 || t === 400 || (o.unit_flags2 & 8))) {
      v = div(mul(v, 100), C.chinese_citizen_creation + 100); note('chinese citizens', v);
      if (C.chinese_citizen_instant !== 0) { note('chinese_citizen_instant', 1); return 1; }
    }
    if (tribeBonus(L, TRIBE.BRITISH)) {
      if (o.domain === 1) { v = div(mul(v, 100), C.british_ship_speed + 100); note('british ships', v); }
      if (isA(o, 170)) { v = div(mul(v, 100), C.british_archer_speed + 100); note('british archers', v); }
      if (isA(o, T.AAGUN)) { v = div(mul(v, 100), C.british_aa_speed + 100); note('british AA', v); }
    }
    if (tribeBonus(L, TRIBE.FRENCH)) {
      if (where === T.SIEGEFACTORY || where === T.FACTORY) { v = div(mul(v, 100), C.french_siege_speed + 100); note('french siege', v); }
      else if (isA(o, T.GENERAL)) { v = div(mul(v, 100), C.french_special_speed + 100); note('french special', v); }
    }
    if (tribeBonus(L, TRIBE.GERMANS)) {
      if (o.domain === 2) { v = div(mul(v, 100), C.german_air_speed + 100); note('german air', v); }
      if (isA(o, T.FIRERAFT)) { v = div(mul(v, 100), C.german_submarine_speed + 100); note('german subs', v); }
    }
    if (tribeBonus(L, TRIBE.ROMANS) && isA(o, T.HOPLITES)) { v = div(mul(v, 100), C.roman_legion_fast + 100); note('roman legion', v); }
    if ((o.role & ROLE_COMBAT) && hasPreq(L, BONUS.TROOPS_FASTER)) { v = div(mul(v, 3), 4); note('TROOPS_FASTER 3/4', v); }
    const up = o.domain === 1 ? L.speed.ships : (o.obj_masks & (M_FOOT | M_MOUNTED)) ? L.speed.troops : (o.obj_masks & M_VEHICLE) ? L.speed.vehicles : null;
    if (up !== null) { v = div(mul(10 - up, v), 10); if (up) note('speed upgrades', v); }
    if ((where === T.BARRACKS || where === T.STABLE || where === T.DOCK) && hasRare(L, RARE.COTTON)) { v = div(mul(v, 100), C.cotton_creation_speed + 100); note('cotton', v); }
    if ((t === T.GENERAL || t === T.SPY) && hasPreq(L, BONUS.SPIES_GENERALS_CREATED_FASTER)) { v = div(v, 2); note('spies/generals faster', v); }
    if ((t === 50 || t === 51) && hasRare(L, RARE.WOOL)) { v = div(mul(v, 100), C.wool_citizens_speed + 100); note('wool', v); }
    if (where === T.STABLE) {
      const k = hasPreq(L, BONUS.MONARCHY_2) ? C.monarchy_unit_prod2 : hasPreq(L, BONUS.MONARCHY_1) ? C.monarchy_unit_prod : null;
      if (k !== null) { v = div(mul(v, 100), k + 100); note('monarchy', v); }
    }
    if ((where === T.SIEGEFACTORY || where === T.AIRBASE || where === T.DOCK) && hasPreq(L, BONUS.SOCIALISM_1)) { v = div(mul(v, 100), C.socialism_unit_bonus + 100); note('socialism', v); }
    const wk = o.domain === 0 ? (hasWonder(L, T.LIBERTY) ? C.liberty_units : null) : o.domain === 1 ? (hasWonder(L, T.PORCELAIN) ? C.porcelain_ships : null) : (hasWonder(L, T.SPACEPROGRAM) ? C.space_air_speed : null);
    if (wk !== null) { v = div(mul(v, 100), wk + 100); note('liberty/porcelain/space', v); }
    if ((t === T.SPY && hasWonder(L, T.KREMLIN)) || hasPreq(L, BONUS.INSTANT_UNIT_BONUS)) { v = mul(C.accel_train, 5) - 1; note('instant (Kremlin spy / INSTANT_UNIT_BONUS)', v); return v; }
  }
  if (isTech(t)) {
    // ages: -(behind) speedup; epochs: same with ages+epochs; other techs: count_discovered speedup. Inputs:
    // opt.behindWeight (sum of +1 team / +2 enemy per leader ahead) and opt.discovered.
    if (isAge(t) || isEpoch(t)) {
      const w = opt.behindWeight ?? 0;
      if (w) { const k = 2 * G.numNations; v = div(mul(k - w + 2, v), k + 2); note('behind speedup', v); }
      if (tribeBonus(L, TRIBE.GREEKS)) { v = div(mul(v, 100), C.greek_research_speed + 100); note('greek research speed', v); }
    } else if (opt.discovered) { v = div(mul(G.numPlayers - opt.discovered + 1, v), G.numPlayers + 1); note('count_discovered', v); }
    if (hasWonder(L, T.SUPERCOLLIDER)) { v = 0; note('supercollider: research instant', v); }
  }
  if (isTech(t) || ((isUnit(t) || isBuild(t)) && !bit)) {
    if (hasPreq(L, BONUS.RESEARCH_FASTER)) { v = div(mul(v, 3), 4); note('RESEARCH_FASTER', v); }
    if (hasWonder(L, T.ANGKOR)) { v = div(mul(v, 100), C.angkor_research + 100); note('angkor research', v); }
    if (hasRare(L, RARE.RELICS)) { v = pct(C.relics_research_speed, v); note('relics', v); }
    const tc = G.techCost;
    if (tc === 0 || tc === 2) { v = div(mul(v, 2), 3); note('tech_cost setting fast', v); }
    else if (tc === 4 || tc === 6 || tc === 8) { v = div(mul(v, 3), 2); note('tech_cost setting slow', v); }
    const sci = L.epoch[3];
    const age = isTech(t) ? o.age : (o.preq[0] < 0 ? 0 : U[o.preq[0]].age);
    if (age < sci) { v = v + div(-mul(mul(sci - age, C.tech_science_speedup), v), 100); note('tech_science_speedup', v); }
    if (hasWonder(L, T.SUPERCOLLIDER) && isTech(t)) { v = 0; note('supercollider', v); }
  }
  if (opt.producerUnassimilated) { v = div(mul(v, 5), 4); note('unassimilated producer 5/4', v); }
  return v < 1 ? 1 : v;
}

// Frames for a queue item at accel a (Build::do_queue @61e410: the done test precedes the add, so an item whose
// train_time is T completes in the frame after job_counter reaches T: ceil(T/a) + 1 frames from the first advance).
export function trainFrames(ctx, timeVal, G) {
  const a = mul(ctx.constants.accel_train, G.aiSpeed > 1 ? G.aiSpeed : 1);
  if (timeVal === 1) return 1;
  return Math.ceil(timeVal / a) + 1;
}

// Wall::update_construct_time @0063d560 (stored in WallData.constr_time at Wall::init and on calc_wall_stats)
export function constrTime(ctx, o, L) {
  const C = ctx.constants, t = o.type;
  let v = typeTime(o) >>> 0;
  if (tribeBonus(L, TRIBE.MAYA) && !isWonder(t)) v = Math.trunc(mul(v, 100) / (C.maya_building_speed + 100));
  if (hasPreq(L, BONUS.BUILDINGS_CREATED_FASTER)) v = (mul(v, 3) >>> 0) >>> 2;
  if (hasWonder(L, T.VERSAILLES)) v = Math.trunc(mul(v, 100) / (C.versailles_building_speed + 100));
  if (hasRare(L, RARE.TOBACCO)) v = Math.trunc(mul(v, 100) / (C.tobacco_building_speed + 100));
  if (tribeBonus(L, TRIBE.BRITISH) && isA(o, T.AIRDEFENSE)) v = Math.trunc(mul(v, 100) / (C.british_aa_speed + 100));
  const fort = isFort(t);
  if (tribeBonus(L, TRIBE.DUTCH) && fort && !isWonder(t)) v = Math.trunc(mul(v, 100) / (C.dutch_fort_speed + 100));
  if (tribeBonus(L, TRIBE.ROMANS) && fort && !isWonder(t)) v = Math.trunc(mul(v, 100) / (C.roman_fort_speed + 100));
  if (L.cityNum === 0) v = Math.trunc(mul(C.capital_build_time, v) / 100);
  v = Math.trunc(mul(10 - L.speed.building, v) / 10);
  return v;
}
// BuildData::construct_time(0) @0062d5c0 (re-evaluated at every do_construct): American first-wonder, Hanging
// Gardens (constant 0), The President, Iroquois first senate. opt: {president, americanFirstWonder}
export function constructTime(ctx, o, L, opt = {}) {
  const C = ctx.constants;
  let v = constrTime(ctx, o, L);
  if (isWonder(o.type) && tribeBonus(L, TRIBE.AMERICANS) && opt.americanFirstWonder) v = 1;
  if (opt.president) v = Math.trunc(mul(v, 100) / (C.thepresident_building_speed + 100));
  if (tribeBonus(L, TRIBE.IROQUOIS) && isA(o, T.SENATE) && C.iroquois_quick_senate !== 0 && !L.everSenate) v = 0;
  return v < 1 ? 1 : v;
}

// Construction with N builders working every frame (Unit::do_build @5eebf0 -> Wall::do_construct @6434d0):
// builder k (0-based, in unit slot order) adds max(1, amount / (k + 1)), amount = accel_construct (x ai_speed),
// /4 while the site is under attack (not for Koreans). Completion when job_counter >= construct_time.
export function constructFrames(ctx, timeVal, builders, G, opt = {}) {
  let amount = ctx.constants.accel_construct;
  if (opt.underAttack && !opt.korean) amount = div(amount, 4);
  if (G.aiSpeed > 1) amount = mul(amount, G.aiSpeed);
  let perFrame = 0;
  for (let k = 0; k < builders; k++) perFrame += Math.max(1, div(amount, k + 1));
  let jc = 0, f = 0;
  while (jc < timeVal) { jc += perFrame; f++; }
  return { frames: f, perFrame };
}

// ---------------------------------------------------------------- CLI
function countsFor(ctx, o, L, k) {
  // --count N: N existing units/buildings of this kind (the item being bought is not counted: pay_cost runs before
  // num_queued++ / the site's increment_stats)
  if (isUnit(o.type)) {
    if (o.where === T.BARRACKS || o.where === T.STABLE || o.where === T.SIEGEFACTORY || o.where === T.DOCK || o.domain === 2) L.numUnits[o.type] = k;
    else L.numUnits[o.type] = k;
  } else if (isCity(o.type)) L.numBuildings[414] = k;
  else if (isWonder(o.type)) L.wondersAll = k;
  else L.numBuildings[o.type] = k;
}

function fmtCost(c) { return RES.map((n, i) => (c[i] ? `${c[i]} ${n}` : null)).filter(Boolean).join(', ') || '0'; }

function describe(ctx, o, L, G, opt = {}) {
  const cost = payCost(ctx, o, L, G, opt);
  let time = null, frames = null, extra = '';
  if (isBuild(o.type) && techBit(L, o.type)) {
    time = constructTime(ctx, o, L, opt);
    const b = opt.builders ?? 1;
    const cf = constructFrames(ctx, time, b, G);
    frames = cf.frames; extra = ` (${b} builder${b > 1 ? 's' : ''}, ${cf.perFrame}/frame)`;
  } else if (isUnit(o.type) || isTech(o.type) || isBuild(o.type)) {
    time = trainTime(ctx, o, L, G, opt);
    frames = trainFrames(ctx, time, G);
  }
  return { cost, time, frames, extra };
}

function parseArgs(argv) {
  const a = { wonders: [], rares: [], preq: [], _: [] };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const nx = () => argv[++i];
    if (k === '--count') a.count = +nx(); else if (k === '--tribe') a.tribe = nx(); else if (k === '--age') a.age = +nx();
    else if (k === '--mil') a.mil = +nx(); else if (k === '--civ') a.civ = +nx(); else if (k === '--com') a.com = +nx(); else if (k === '--sci') a.sci = +nx();
    else if (k === '--wonder') a.wonders.push(nx()); else if (k === '--rare') a.rares.push(nx()); else if (k === '--preq') a.preq.push(nx());
    else if (k === '--upgrade') a.upgrade = true; else if (k === '--wonders') a.nwonders = +nx(); else if (k === '--cities') a.cities = +nx();
    else if (k === '--builders') a.builders = +nx(); else if (k === '--bspeed') a.bspeed = +nx(); else if (k === '--no-convert') a.noConvert = true;
    else if (k === '--examples') a.examples = true; else if (k === '--selftest') a.selftest = true; else if (k === '--trace') a.trace = true;
    else if (k === '--data') a.data = nx(); else a._.push(k);
  }
  return a;
}

function leaderFrom(ctx, a) {
  const L = newLeader();
  if (a.tribe) { const k = a.tribe.toUpperCase(); if (!(k in TRIBE)) throw new Error(`unknown tribe ${a.tribe}`); L.tribe = TRIBE[k]; }
  if (a.age !== undefined) L.ages = a.age;
  L.epoch = [a.mil ?? 0, a.civ ?? 0, a.com ?? 0, a.sci ?? 0];
  for (const w of a.wonders) L.wonders.add(findType(ctx, w).type);
  for (const r of a.rares) { const k = r.toUpperCase(); if (!(k in RARE)) throw new Error(`unknown rare ${r}`); L.rares.add(RARE[k]); }
  for (const p of a.preq) { const k = p.toUpperCase(); if (!(k in BONUS)) throw new Error(`unknown bonus ${p}`); L.preq.add(BONUS[k]); }
  if (a.bspeed) L.speed.building = a.bspeed;
  if (a.cities !== undefined) L.numBuildings[414] = a.cities;
  return L;
}

function printOne(ctx, o, L, G, a) {
  const trace = a.trace ? [] : null;
  const opt = { builders: a.builders, convert: a.noConvert ? 0 : 1, trace };
  const d = describe(ctx, o, L, G, opt);
  console.log(`${o.name} (type ${o.type})${L.unresearched.has(o.type) ? ' [research]' : ''}: cost ${fmtCost(d.cost)}` +
    (d.time !== null ? `; time ${d.time} (1/100 frame) = ${d.frames} frames${d.extra}` : ''));
  if (trace) for (const s of trace) console.log('   ' + s);
}

// Worked examples (the table in the spec draft).
export function examples(ctx) {
  const G = newGame();
  const rows = [];
  const row = (label, o, L, opt = {}) => { const d = describe(ctx, o, L, G, opt); rows.push([label, fmtCost(d.cost), d.time ?? '', d.frames ?? '', d.extra]); };
  const ty = (s) => findType(ctx, s);
  for (const k of [0, 1, 4, 9, 29]) { const L = newLeader(); L.numUnits[50] = k + 3; row(`Citizen, ${k + 3} citizens owned`, ty('Citizen'), L); }
  { const L = newLeader({ tribe: TRIBE.TURKS }); L.numUnits[50] = 10; row('Citizen, 10 owned, Turks', ty('Citizen'), L); }
  for (const k of [0, 6, 7, 12]) { const L = newLeader({ ages: 1 }); L.numUnits[52] = k; row(`Scholar, ${k} owned (Classical)`, ty('Scholar'), L); }
  for (const k of [0, 1, 4]) { const L = newLeader(); L.numBuildings[427] = k; row(`Barracks, ${k} owned`, ty('Barracks'), L); }
  { const L = newLeader({ tribe: TRIBE.INDIANS }); L.numBuildings[427] = 4; row('Barracks, 4 owned, Indians', ty('Barracks'), L); }
  { const L = newLeader(); L.numBuildings[417] = 5; row('Farm, 5 owned', ty('Farm'), L); }
  { const L = newLeader(); L.numBuildings[414] = 1; row('Small City, 1 owned', ty('Small City'), L); }
  { const L = newLeader({ tribe: TRIBE.BANTU }); L.numBuildings[414] = 1; row('Small City, 1 owned, Bantu', ty('Small City'), L); }
  { const L = newLeader({ wonders: new Set([T.PYRAMIDS]) }); L.numBuildings[414] = 2; row('Small City, 2 owned, Pyramids', ty('Small City'), L); }
  for (const k of [0, 10]) { const L = newLeader(); L.numUnits[132] = k; row(`Hoplites, ${k} barracks units (Ancient)`, ty('Hoplites'), L); }
  { const L = newLeader({ ages: 1 }); L.numUnits[132] = 10; row('Hoplites, 10 barracks units, Classical', ty('Hoplites'), L); }
  { const L = newLeader({ ages: 1, epoch: [2, 0, 0, 0] }); L.numUnits[132] = 10; row('Hoplites, 10 units, Classical, 2 mil. techs', ty('Hoplites'), L); }
  { const L = newLeader({ ages: 1 }); L.unresearched.add(133); L.numUnits[132] = 10; row('Phalanx upgrade, 10 hoplites', ty('Phalanx'), L); }
  { const L = newLeader({ ages: 0 }); row('Classical Age', ty('Classical Age'), L); }
  { const L = newLeader({ ages: 1, epoch: [0, 0, 0, 1] }); row('Medieval Age (1 science)', ty('Medieval Age'), L); }
  { const L = newLeader({ ages: 1, epoch: [0, 0, 0, 0] }); row('Medieval Age (0 science)', ty('Medieval Age'), L); }
  { const L = newLeader({ ages: 1, epoch: [0, 0, 0, 1] }); row('Mathematics (library, 1 science)', ty('Mathematics'), L); }
  { const L = newLeader({ ages: 1, tribe: TRIBE.CHINESE, epoch: [0, 0, 0, 1] }); row('Mathematics, Chinese', ty('Mathematics'), L); }
  { const L = newLeader({ ages: 0 }); row('Library (building)', ty('Library'), L); }
  { const L = newLeader({ ages: 0, tribe: TRIBE.GREEKS }); row('Library, Greeks', ty('Library'), L); }
  { const L = newLeader({ ages: 1 }); L.wondersAll = 0; row('Colossus, first wonder', ty('Colossus'), L); }
  { const L = newLeader({ ages: 1 }); L.wondersAll = 4; row('Colossus, 4 wonders owned', ty('Colossus'), L); }
  { const L = newLeader({ ages: 1, tribe: TRIBE.EGYPTIANS }); L.wondersAll = 4; row('Colossus, 4 wonders, Egyptians', ty('Colossus'), L); }
  for (const b of [1, 2, 4, 8]) { const L = newLeader(); row(`Barracks construction, ${b} builder(s)`, ty('Barracks'), L, { builders: b }); }
  { const L = newLeader({ tribe: TRIBE.MAYA }); row('Barracks construction, 2 builders, Maya', ty('Barracks'), L, { builders: 2 }); }
  return rows;
}

function selftest(ctx) {
  const G = newGame(); let fail = 0;
  const eq = (what, a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) { fail++; console.log(`FAIL ${what}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`); } else console.log(`ok   ${what}`); };
  const ty = (s) => findType(ctx, s);
  // hand-computed from the rules values: Citizen 2f, support 1f/citizen, cap 500% of 20
  { const L = newLeader(); L.numUnits[50] = 9; eq('10th citizen food = 20 + 9', getCost(ctx, ty('Citizen'), 0, L, G), 29); }
  { const L = newLeader(); L.numUnits[50] = 200; eq('citizen ramp capped at 500% of 20', getCost(ctx, ty('Citizen'), 0, L, G), 120); }
  // Barracks 12t x10, support 25t, military production count 2c-2 above 2
  { const L = newLeader(); L.numBuildings[427] = 3; eq('4th barracks timber = 120 + 25*(2*3-2)', getCost(ctx, ty('Barracks'), 1, L, G), 220); }
  { const L = newLeader({ tribe: TRIBE.INDIANS }); L.numBuildings[427] = 3; eq('Indian barracks: no ramp', getCost(ctx, ty('Barracks'), 1, L, G), 120); }
  // conversion: hoplites metal 3 x10 = 30 -> timber 30*320/256 = 37 in the Ancient age
  { const L = newLeader(); eq('ancient hoplites = 50f + 37t', payCost(ctx, ty('Hoplites'), L, G), [50, 37, 0, 0, 0, 0]); }
  // construction: 1 builder 100/frame, 2 builders 150/frame
  eq('Barracks 42000 with 2 builders = 280 frames', constructFrames(ctx, 42000, 2, G).frames, 280);
  eq('per-frame of 4 builders = 100+50+33+25', constructFrames(ctx, 1, 4, G).perFrame, 208);
  // citizen time: 50*100*120/100 = 6000 + n*10*75
  { const L = newLeader(); L.numUnits[50] = 4; eq('citizen time with 4 citizens', trainTime(ctx, ty('Citizen'), L, G), 9000); }
  { const L = newLeader(); L.numUnits[50] = 100; eq('citizen time capped at 3x', trainTime(ctx, ty('Citizen'), L, G), 18000); }
  // science discount: +10% per science level below the tech's level
  { const L = newLeader({ ages: 1, epoch: [0, 0, 0, 0] }); eq('Medieval Age food (age 1), 0 science = 250 + 10%', getCost(ctx, ty('Medieval Age'), 0, L, G), 275); }
  console.log(fail ? `${fail} failure(s)` : 'all ok');
  return fail;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const a = parseArgs(process.argv.slice(2));
  const ctx = load(a.data ?? DEFAULT_DATA);
  if (a.selftest) process.exit(selftest(ctx) ? 1 : 0);
  if (a.examples) {
    const rows = examples(ctx);
    console.log('| Case | Cost | train/construct time (1/100 frame) | frames | note |');
    console.log('|---|---|---|---|---|');
    for (const r of rows) console.log(`| ${r.join(' | ')} |`);
  } else if (a._.length) {
    const o = findType(ctx, a._.join(' '));
    const L = leaderFrom(ctx, a);
    if (a.upgrade) L.unresearched.add(o.type);
    if (a.count !== undefined) countsFor(ctx, o, L, a.count);
    if (a.nwonders !== undefined) L.wondersAll = a.nwonders;
    printOne(ctx, o, L, newGame(), a);
  } else {
    console.log('usage: production-ref.mjs <type name> [--count N] [--tribe X] [--age A] [--trace] ... | --examples | --selftest');
  }
}
