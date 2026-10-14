#!/usr/bin/env node
// Reference model of Rise of Nations resource income (docs/economy-spec.md), from the decompiled
//   BuildTypeData::calc_gather @00639e40 (per-building rate: flat / radius / mine / university paths),
//   LeaderData::calc_city_resources @006d5530 + City::calc_gather @00737c60 (city base, taxes, literacy),
//   Leader::calc_gather @006ceee0 (leader-wide post-processing), LeaderData::calc_resource_bonuses @006db030,
//   Leader::calc_resource_caps @006ce900 (commerce caps) and Leader::do_gather @006ce450 (per-frame stockpile).
// Reads the rules from the user's install (never writes to it) through rulesdump.mjs.
//
//   node re/tools/gather-ref.mjs --examples      worked examples of the spec (§10)
//   node re/tools/gather-ref.mjs --selftest      internal consistency checks
//   node re/tools/gather-ref.mjs --constants     the economy constants as loaded
//
// Units: a "rate" is resources per GATHER_RATE (450) frames, times 16 (the game's fixed point: the stockpile
// gains rate / (450*16) per frame, remainders carried in LeaderDataEncrypt.leftover). Map scans (which tiles a
// building covers, forest W-cells in range, claimable mountain W-cells, access tiles) are inputs here, not derived:
// they need the world grid (see docs/economy-spec.md §3.4–3.6 for how the game counts them).
// All arithmetic is int32 with C truncating division, exactly as the binary does it.

import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadRules, startGame, DEFAULT_DATA } from './rulesdump.mjs';
import { readLands } from './rules/techs.mjs';

const i32 = (x) => x | 0;
const mul = (a, b) => Math.imul(a, b);
const div = (a, b) => { if (b === 0) throw new Error('division by zero'); return i32(a / b); };
const tdiv256 = (x) => div(x, 256);   // (x + ((x >> 31) & 0xff)) >> 8
const tdiv16 = (x) => div(x, 16);     // (x + ((x >> 31) & 0xf)) >> 4
const pct = (x, p) => div(mul(p + 100, x), 100);  // ((p + 100) * x) / 100, the binary's percent bonus form

export const G = { FOOD: 0, TIMBER: 1, WEALTH: 2, KNOWLEDGE: 3, METAL: 4, OIL: 5 };
export const GOOD_NAMES = ['food', 'timber', 'wealth', 'knowledge', 'metal', 'oil'];
// BuildTypeData::get_good @0063bd50 (hard-coded), GoodTypeData::get_enhancer @0066fcf0
export const B = { FARM: 417, WOODCUTTER: 418, MINE: 419, UNIVERSITY: 420, OILWELL: 421, OILPLATFORM: 422,
  GRANARY: 423, LUMBERMILL: 424, SMELTER: 425, REFINERY: 426 };
export const BUILD_GOOD = { 417: 0, 418: 1, 419: 4, 420: 3, 421: 5, 422: 5 };

export function load(dataDir = DEFAULT_DATA) {
  const ctx = loadRules(dataDir);
  startGame(ctx);
  ctx.lands = readLands(ctx);
  return ctx;
}

// ---------------------------------------------------------------- city enhancers (City::calc_gather)
// level = LeaderData::get_granary/get_lumbermill/get_smelter (1..5 / 1..4 from GRANARY2..5 etc. techs);
// the city byte = <table>[level - 1] when the city has the enhancer building, else 0.
export function enhancerPct(ctx, good, level) {
  if (!level) return 0;
  const c = ctx.constants;
  if (good === G.FOOD) return c.granary_bonus[level - 1];
  if (good === G.TIMBER) return c.lumbermill_bonus[level - 1];
  if (good === G.METAL) return c.smelter_bonus[level - 1];
  return 0;  // refinery byte is always written 0; oil uses the leader-wide refinery_bonus instead
}
// CityData::enhancer_amount(k, 100) = ((byte + 100) * 100) / 100
const enhNum = (bytePct) => div(mul(bytePct + 100, 100), 100);

// ---------------------------------------------------------------- per building (BuildTypeData::calc_gather)
// Common result: { out[6] (rate, x16), max (slots), per (rate per gatherer) }.
// opts.gatherers = num_gatherers(1, 1) (been_there gatherers, decoys excluded; universities count scholars inside)
// opts.enh = city enhancer byte for the building's good (0 when the building is not in a city)
// opts.bonus256 = per-good 8.8 multiplier array (only applied when p7 != 0, i.e. placement previews; income
//                 passes p7 = 0 and applies calc_resource_bonuses leader-wide instead) — default all 256.
// opts.tribe: TribeIndex of the owner (for has_tribe_bonus), opts.wonders: Set of wonder TypeIndex.
function apply(out, k, amount, per, bonus256) {
  let a = amount, p = per;
  if (bonus256 && bonus256[k] !== 256) { a = tdiv256(mul(a, bonus256[k])); p = tdiv256(mul(p, bonus256[k])); }
  out[k] = i32(out[k] + a);
  return p;
}

// Flat path (FARM, OILWELL, OILPLATFORM: BUILDTYPE_FLAT_GATHER): one gatherer slot, the rate comes from the land
// under the footprint. tiles = [{land: index into rules.xml LANDS, river: bool, owned: bool}] (owned = own/ally/none).
export function flatGather(ctx, type, tiles, opts = {}) {
  const c = ctx.constants, out = [0, 0, 0, 0, 0, 0], raw = [0, 0, 0, 0, 0, 0];
  if (!opts.active && !opts.assumeActive) return { out, max: 1, per: 0 };
  for (const t of tiles) {
    if (t.owned === false) continue;
    for (const m of ctx.lands[t.land].slice(0, 4)) {
      if (m.good < 0) continue;
      raw[m.good] += t.river ? mul(m.num, c.river_resource_value) : m.num;
    }
  }
  const good = BUILD_GOOD[type];
  let per = 0;
  for (let k = 0; k < 6; k++) {
    if (!opts.active || k !== good) continue;
    const r = k === G.OIL ? c.oil_rate : c.peasant_rate;
    per = Math.max(0, tdiv256(mul(raw[k], r)));
    const n = enhNum(opts.enh ?? 0);
    if (n !== 100) per = div(mul(per, n), 100);
    if (k === G.FOOD && opts.tribe === 15) per = pct(per, c.japanese_fishing_boats);  // Japanese (applies to farms)
    per = apply(out, k, mul(per, opts.gatherers ?? 0), per, opts.bonus256);
  }
  if (type === B.FARM && opts.tribe === 7 && c.egyptian_farm_wealth > 0 && (opts.gatherers ?? 0) !== 0)
    out[G.WEALTH] = i32(out[G.WEALTH] + mul(c.egyptian_farm_wealth, 16));
  return { out, max: 1, per };
}

// Radius path (WOODCUTTER and any other non-flat, non-mine, non-university gatherer): slots from the land of the
// W-cells in gather_radius whose top-left tile is in the building's gather_from list (i.e. claimed tree tiles).
// opts.cells = [{land}] for those W-cells (each adds 16 * MAKE num), opts.rivers = river tiles under the footprint,
// opts.access = total_gather_access (claimed tiles with gather access, not hugging the corner), opts.blocked =
// some cell in range was claimed by another building (only used when no slot results).
export function radiusGather(ctx, type, opts = {}) {
  const c = ctx.constants, out = [0, 0, 0, 0, 0, 0], raw = [0, 0, 0, 0, 0, 0];
  if (!opts.active && !opts.assumeActive) return { out, max: 1, per: 0 };
  const good = BUILD_GOOD[type];
  let iroq = 0;
  for (const cell of opts.cells ?? []) {
    let cnt = 0;
    for (const m of ctx.lands[cell.land].slice(0, 4)) {
      if (m.good < 0 || m.good !== good || m.num === 0) continue;
      raw[m.good] += mul(16, m.num); cnt += 16;
    }
    if (opts.tribe === 18) iroq += cnt;  // Iroquois: +1 raw food per counted tile
  }
  raw[0] += iroq;
  let max = 1, per = 0, gatherers = opts.gatherers ?? 0;
  for (let k = 0; k < 6; k++) {
    if (!opts.active) continue;
    const iroqFood = k === 0 && c.iroquois_food !== 0 && opts.tribe === 18;
    if (k !== good && !iroqFood) continue;
    raw[k] = i32(tdiv16(mul(raw[k], opts.rivers ?? 0)) + raw[k]);
    max = tdiv16(raw[k] + 8);
    if (max === 0 && opts.blocked) { max = -1; break; }
    if (max !== 0 && k === G.TIMBER) {
      if (opts.tribe === 10) max += c.french_woodies;
      if (opts.wonders?.has(539)) max += c.taj_farms;
      if (opts.wonders?.has(538)) max += c.kremlin_farms;
    }
    if (k === 0 && opts.tribe === 18 && good === G.TIMBER) per = i32(per + mul(c.iroquois_food, 16));
    else per = tdiv256(c.peasant_rate) << 4;
    const n = enhNum(k === good ? (opts.enh ?? 0) : 0);
    if (n !== 100) per = div(mul(per, n), 100);
    if (k === G.OIL) { per = mul(per, max); if (max > 1) max = 1; }
    let take = Math.min(max, gatherers);
    const acc = mul(opts.access ?? 0, 2);
    take = Math.min(take, acc); gatherers = take; max = Math.min(max, acc);
    per = apply(out, k, mul(take, per), per, opts.bonus256);
  }
  return { out, max, per };
}

// MountainRangeData::gather_size @0089d170 / CliffsData::gather_size @008a8f80: slots of a mine.
export function mineSlots(ctx, size, total, claimable) {
  const c = ctx.constants;
  let g = size < c.mtn_tiny_size ? c.mtn_tiny_gather : size < c.mtn_small_size ? c.mtn_small_gather
    : size < c.mtn_medium_size ? c.mtn_medium_gather : size < c.mtn_large_size ? c.mtn_large_gather : c.mtn_huge_gather;
  if (total !== 0) g = div(mul(g, claimable), total);
  if (claimable !== 0 && g < 1) g = 1;
  return g;
}
// Mine path: opts.slots = gather_size (or -1 when the deposit is claimed by another building).
export function mineGather(ctx, opts = {}) {
  const c = ctx.constants, out = [0, 0, 0, 0, 0, 0];
  if (!opts.active && !opts.assumeActive) return { out, max: 1, per: 0 };
  let max = opts.slots ?? 0;
  if (max !== 0) {
    if (opts.tribe === 12) max += c.german_miners;
    if (opts.wonders?.has(539)) max += c.taj_farms;
    if (opts.wonders?.has(538)) max += c.kremlin_farms;
  }
  let per = tdiv256(c.peasant_rate) << 4;
  const n = enhNum(opts.enh ?? 0);
  if (n !== 100) per = div(mul(per, n), 100);
  const take = Math.min(Math.max(0, max), opts.gatherers ?? 0);
  if (opts.tribe === 2 && c.inca_wealth_per_miner > 0) out[G.WEALTH] = i32(mul(mul(take, c.inca_wealth_per_miner), 16) + out[G.WEALTH]);
  per = apply(out, G.METAL, mul(per, take), per, opts.bonus256);
  return { out, max, per };
}

// University path: 7 slots (max_knowledge_gatherers), per-scholar rate from SCHOLAR_RATE[university level - 1].
export function universityGather(ctx, opts = {}) {
  const c = ctx.constants, out = [0, 0, 0, 0, 0, 0];
  if (!opts.active && !opts.assumeActive) return { out, max: 1, per: 0 };
  const max = 7, take = Math.min(max, opts.gatherers ?? 0), lvl = opts.univLevel ?? 1;
  let per = Math.max(0, tdiv256(mul(c.scholar_rate[lvl - 1], 16)));
  per = apply(out, G.KNOWLEDGE, mul(per, take), per, opts.bonus256);
  return { out, max, per };
}

// ---------------------------------------------------------------- city (LeaderData::calc_city_resources)
// sum = the buildings' outs (already computed), then trade_val, Forbidden City / CEO multipliers, base gather,
// Roman / German extras, taxes and literacy. opts: tradeVal, forbidden, ceo, tribe, metalAvail, numBuildings,
// market, temple, university, library, porcelain.
export function cityResources(ctx, buildingOuts, opts = {}) {
  const c = ctx.constants, out = [0, 0, 0, 0, 0, 0];
  out[G.WEALTH] += opts.tradeVal ?? 0;
  for (const b of buildingOuts) for (let k = 0; k < 6; k++) out[k] = i32(out[k] + b[k]);
  if (opts.forbidden) for (let k = 0; k < 6; k++) out[k] = pct(out[k], c.forbidden_city_gather);
  if (opts.ceo) for (let k = 0; k < 6; k++) out[k] = pct(out[k], c.theceo_production_bonus);
  for (let k = 0; k < 6; k++) {
    if (c.city_gather[k] === 0) continue;
    const a = opts.forbidden && c.forbidden_city_base_gather !== 0 ? c.forbidden_city_base_gather : c.city_gather[k];
    out[k] = i32(out[k] + mul(a, 16));
  }
  if (c.roman_city_gather !== 0 && opts.tribe === 6) out[G.WEALTH] += mul(c.roman_city_gather, 16);
  if (c.german_city_gather !== 0 && opts.tribe === 12) {
    out[G.FOOD] += mul(c.german_city_gather, 16); out[G.TIMBER] += mul(c.german_city_gather, 16);
    if (opts.metalAvail) out[G.METAL] += mul(c.german_city_gather, 16);
  }
  let taxes = c.village_taxes + mul(opts.numBuildings ?? 0, c.building_taxes);
  if (opts.market) taxes += opts.porcelain ? pct(c.market_taxes, c.porcelain_market) : c.market_taxes;
  if (opts.temple) taxes += c.temple_taxes;
  out[G.WEALTH] = i32(out[G.WEALTH] + mul(taxes, 16));
  let lit = c.village_literacy + (opts.university ? c.university_literacy : 0) + (opts.library ? c.library_literacy : 0);
  out[G.KNOWLEDGE] = i32(out[G.KNOWLEDGE] + mul(lit, 16));
  return out;
}

// ---------------------------------------------------------------- leader-wide (Leader::calc_gather tail)
// out = basic_gather<<4 + tribe extras + cities + out-of-city woodcutters/mines + oil wells + idle fishermen /
// merchants (the caller sums those); this applies the steps after the unit loop, in the binary's order.
export function leaderPost(ctx, out0, opts = {}) {
  const c = ctx.constants, out = out0.slice();
  out[G.OIL] = div(mul(mul(opts.refineries ?? 0, c.refinery_bonus) + 100, out[G.OIL]), 100);
  if (opts.capitalism) out[G.OIL] = i32(out[G.OIL] + mul(c.capitalism_oil_prod, 16));
  if (c.inca_wealth_per_miner < 0 && opts.tribe === 2) out[G.WEALTH] += out[G.METAL];
  for (const r of opts.conquestRares ?? []) for (let k = 0; k < 6; k++) out[k] += r[k] ?? 0;  // calc_rare outputs
  if (opts.coffee) for (let k = 0; k < 6; k++) out[k] = pct(out[k], c.coffee_income_bonus);
  if (opts.worldTerritory) {
    let tax = c.territory_taxes[opts.taxation ?? 0];
    if (opts.tribe === 11) tax = pct(tax, c.british_taxation);
    if (opts.ctwMissionaries) tax = pct(tax, c.ctw_missionaries_bonus);
    out[G.WEALTH] = i32(out[G.WEALTH] + div(mul(mul(opts.territory ?? 0, tax), 16), opts.worldTerritory));
    if (opts.tribe === 17 && c.mongol_nomadic_food !== 0)
      out[G.FOOD] = i32(out[G.FOOD] + div(div(mul(mul(opts.numNations ?? 1, opts.territory ?? 0), 800), opts.worldTerritory), c.mongol_nomadic_food));
  }
  // LeaderData::calc_resource_bonuses
  if (opts.tribe === 13) out[G.OIL] = pct(out[G.OIL], c.russian_oil);
  const w = opts.wonders ?? new Set();
  if (w.has(526)) out[G.FOOD] = pct(out[G.FOOD], c.pyramids_food);
  if (w.has(527)) out[G.WEALTH] = pct(out[G.WEALTH], c.colossus_wealth);
  if (w.has(528)) out[G.KNOWLEDGE] = i32(out[G.KNOWLEDGE] + mul(c.hanging_gardens_knowledge, 16));
  if (w.has(535)) out[G.METAL] = pct(out[G.METAL], c.angkor_metal);
  if (w.has(539)) out[G.WEALTH] = pct(out[G.WEALTH], c.taj_wealth);
  if (w.has(540)) out[G.OIL] = pct(out[G.OIL], c.eiffel_oil);
  if (w.has(532)) out[G.TIMBER] = pct(out[G.TIMBER], c.tikal_timber);
  if (opts.virtualReality) for (let k = 0; k < 6; k++) if (k !== 3) out[k] = pct(out[k], c.global_prosperity);
  return out;
}

// ---------------------------------------------------------------- caps (Leader::calc_resource_caps), simplified
// Returns resource_cap[k] (already <<4). opts.commerce = epoch[2] (commerce level 0..7), tribe, wonders,
// republic (0..3), bonusCap[7], virtualReality.
export function resourceCaps(ctx, opts = {}) {
  const c = ctx.constants, caps = [];
  const w = opts.wonders ?? new Set();
  for (let k = 0; k < 7; k++) {
    let v;
    if (k === 3) v = 999;
    else {
      v = c.commerce_cap[opts.commerce ?? 0];
      if (opts.tribe === 11) v = pct(v, c.british_commerce);
      if (k === 2 && c.inca_wealth_cap !== 0 && opts.tribe === 2) v = pct(v, c.inca_wealth_cap);
      else if (k === 1 && c.french_timber_commerce !== 0 && opts.tribe === 10) v = pct(v, c.french_timber_commerce);
      else if (k === 0 && c.egyptian_food_commerce !== 0 && opts.tribe === 7) v = pct(v, c.egyptian_food_commerce);
      if (opts.diamonds) v = pct(v, c.diamonds_commerce);
      if ((k === 0 || k === 2) && w.has(526)) v += c.pyramids_commerce;
      if (k === 1 || k === 2) {
        if (w.has(527)) v += c.colossus_commerce;
        if (k === 2 && w.has(539)) v += c.taj_wealth_commerce;
      }
      if (k === 5) { if (w.has(540)) v += c.eiffel_oil_commerce; if (w.has(538)) v += c.kremlin_commerce; }
      else if (k !== 2 && k < 6) {  // food, timber (after its Colossus bonus), metal; not wealth, not slot 6
        if (w.has(538)) v += c.kremlin_commerce;
        if (k === 1 && w.has(532)) v += c.tikal_timber_commerce;
        else if (k === 4 && w.has(535)) v += c.angkor_metal_commerce;
      }
      const rep = opts.republic ?? 0;
      if (rep === 3) v += c.republic_commerce_bonus3; else if (rep === 2) v += c.republic_commerce_bonus2;
      else if (rep === 1) v += c.republic_commerce_bonus;
      v += (opts.bonusCap ?? [])[k] ?? 0;
      v = v < 0 ? 0 : v > 999 ? 999 : v;
    }
    if (opts.virtualReality) v = 999;
    caps.push(v << 4);
  }
  return caps;
}

// ---------------------------------------------------------------- per frame (Leader::do_gather)
// state = {bucket, leftover, over_cap, income, escrow}; rate = resources[k] - support[k] + base_rate[k].
// opts: cap (resource_cap[k]), handicap (get_gather_handicap), techCost (setup byte), fastRules (info.flags&2 ||
// game_rules == 9), aiSpeed, escrowRate (AI only), frame, k, dutch {base}.
export function doGatherFrame(ctx, st, rate, opts = {}) {
  const c = ctx.constants, k = opts.k ?? 0;
  let r = rate;
  if (r < 0) { st.income = r; st.over_cap = 0; return 0; }
  const cap = opts.cap ?? (999 << 4);
  if (cap < r) { st.over_cap = (cap > 0x3e6f ? 1 : 0) + 1; r = cap; } else st.over_cap = 0;
  if (k !== 3 && opts.dutch) {
    const excess = st.bucket - opts.dutch.base;
    let r2 = r;
    if (excess > 0) {
      r2 = i32(r + mul(div(mul(c.dutch_interest, excess), 100), 16));
      const lim = i32(mul(c.dutch_interest_cap, 16) + cap);
      if (r2 > lim) r2 = lim;
    }
    r = r2 < 0x3e70 ? r2 : 0x3e70;
  }
  st.income = r;
  if (opts.handicap) r = div(mul(opts.handicap + 100, r), 100);
  if (k === 3 && opts.techCost > 4) r = opts.techCost <= 6 ? div(mul(r, 3), 4) : div(r, 2);
  if (opts.fastRules) r = div(mul(r, 3), 2);
  if ((opts.aiSpeed ?? 1) > 1) r = mul(r, opts.aiSpeed);
  const D = mul(c.gather_rate, 16);
  let q = div(r, D);
  st.leftover = i32(st.leftover + (r % D));
  while (st.leftover >= D) { q++; st.leftover -= D; }
  st.bucket = i32(st.bucket + q);
  if (opts.escrowRate !== undefined) {
    const D2 = mul(D, 100), num = mul(opts.escrowRate, r);
    let e = div(num, D2); const rem = num % D2;
    if (rem !== 0) { const period = Math.max(2, div(div(rem, 2) + D2, rem)); if ((opts.frame ?? 0) % period === 0) e++; }
    st.escrow = i32((st.escrow ?? 0) + e);
  }
  return q;
}

// ---------------------------------------------------------------- examples / selftest
function fmt(out) { return out.map((v, k) => `${GOOD_NAMES[k]} ${v}${v ? ` (${(v / 16).toFixed(2)}/450f)` : ''}`).filter((s) => !/ 0$/.test(s)).join(', ') || '0'; }
const LAND = { Land: 0, Sandy: 1, Ocean: 2, Coast: 3, Forest: 4, Mountains: 5, Rocks: 6, Oil: 7, Cliffs: 8 };

export function examples(ctx) {
  const lines = [];
  const say = (s) => lines.push(s);
  const c = ctx.constants;
  const tiles16 = (land, rivers = 0) => Array.from({ length: 16 }, (_, i) => ({ land, river: i < rivers }));
  say('E1 farm, 16 Land tiles, 1 farmer, no granary:');
  let r = flatGather(ctx, B.FARM, tiles16(LAND.Land), { active: true, gatherers: 1 });
  say(`   per farmer ${r.per}, out ${fmt(r.out)}, slots ${r.max}`);
  say('E2 same farm, 4 river tiles, granary level 3 (100%):');
  r = flatGather(ctx, B.FARM, tiles16(LAND.Land, 4), { active: true, gatherers: 1, enh: enhancerPct(ctx, G.FOOD, 3) });
  say(`   raw food ${16 + 4}, per = trunc256(20*${c.peasant_rate}) = 200, x200% = ${r.per}; out ${fmt(r.out)}`);
  say('E3 oil well on 16 Oil tiles, 1 worker:');
  r = flatGather(ctx, B.OILWELL, tiles16(LAND.Oil), { active: true, gatherers: 1 });
  say(`   per = trunc256(16*${c.oil_rate}) = ${r.per}; out ${fmt(r.out)}`);
  say('E4 woodcutter: 5 forest W-cells claimed, no rivers, 7 woodcutters, 40 access tiles, lumber mill level 2 (50%):');
  r = radiusGather(ctx, B.WOODCUTTER, { active: true, cells: Array(5).fill({ land: LAND.Forest }), gatherers: 7, access: 40, enh: enhancerPct(ctx, G.TIMBER, 2) });
  say(`   slots ${r.max}, per ${r.per}, out ${fmt(r.out)} (2 of the 7 do not count)`);
  say('E5 woodcutter as French (+1 slot), same site:');
  r = radiusGather(ctx, B.WOODCUTTER, { active: true, cells: Array(5).fill({ land: LAND.Forest }), gatherers: 7, access: 40, tribe: 10 });
  say(`   slots ${r.max}, out ${fmt(r.out)}`);
  say('E6 mine on a small mountain (150 tiles), 6 of 8 solid W-cells claimable, 5 miners, smelter level 1 (50%):');
  const slots = mineSlots(ctx, 150, 8, 6);
  r = mineGather(ctx, { active: true, slots, gatherers: 5, enh: enhancerPct(ctx, G.METAL, 1) });
  say(`   slots = ${c.mtn_small_gather}*6/8 = ${slots}; per ${r.per}; out ${fmt(r.out)}`);
  say('E7 university, 4 scholars, university level 1 and 3:');
  r = universityGather(ctx, { active: true, gatherers: 4, univLevel: 1 });
  say(`   L1 per ${r.per}, out ${fmt(r.out)}`);
  r = universityGather(ctx, { active: true, gatherers: 4, univLevel: 3 });
  say(`   L3 per ${r.per}, out ${fmt(r.out)}`);
  say('E8 a capital with the E1 farm (x3) + E4 woodcutter, market and university:');
  const farm = flatGather(ctx, B.FARM, tiles16(LAND.Land), { active: true, gatherers: 1 }).out;
  const wood = radiusGather(ctx, B.WOODCUTTER, { active: true, cells: Array(5).fill({ land: LAND.Forest }), gatherers: 5, access: 40 }).out;
  const city = cityResources(ctx, [farm, farm, farm, wood], { market: true, university: true, numBuildings: 6 });
  say(`   city ${fmt(city)}`);
  say('E9 leader total: basic_gather 0 + that city; 2 refineries (none apply: no oil); territory 600 of 12000 tiles, taxation level 2:');
  const tot = leaderPost(ctx, city, { refineries: 2, worldTerritory: 12000, territory: 600, taxation: 2 });
  say(`   ${fmt(tot)}`);
  say('E10 commerce caps at commerce level 1, plain nation, no wonders:');
  const caps = resourceCaps(ctx, { commerce: 1 });
  say(`   caps (x16) ${caps.join(', ')}`);
  say('E11 do_gather over 450 frames for food rate 690 (43.125/450f), cap 1600:');
  const st = { bucket: 200, leftover: 0, over_cap: 0, income: 0 };
  let got = 0;
  for (let f = 1; f <= 450; f++) got += doGatherFrame(ctx, st, 690, { k: 0, cap: caps[0], frame: f });
  say(`   gained ${got} (690*450/7200 = ${690 * 450 / 7200}), leftover ${st.leftover}, bucket ${st.bucket}`);
  say('E12 same with rate 2000 against cap 1600 (capped, over_cap = 1):');
  const st2 = { bucket: 0, leftover: 0, over_cap: 0, income: 0 };
  got = 0; for (let f = 1; f <= 450; f++) got += doGatherFrame(ctx, st2, 2000, { k: 0, cap: caps[0], frame: f });
  say(`   gained ${got} (1600*450/7200 = 100), over_cap ${st2.over_cap}, income ${st2.income}`);
  return lines.join('\n');
}

export function selftest(ctx) {
  const fails = [];
  const ok = (cond, msg) => { if (!cond) fails.push(msg); };
  const c = ctx.constants;
  ok(c.gather_rate === 450 && c.peasant_rate === 2560 && c.oil_rate === 8960, 'constants as in rules-spec §12');
  ok(tdiv256(c.peasant_rate) << 4 === 160, 'radius/mine per-gatherer rate 160');
  for (const rate of [1, 7199, 7200, 7201, 690, 15984]) {
    const st = { bucket: 0, leftover: 0, over_cap: 0, income: 0 };
    let got = 0; for (let f = 0; f < 7200; f++) got += doGatherFrame(ctx, st, rate, { k: 0, cap: 15984 });
    ok(got === Math.min(rate, 15984) && st.leftover === 0, `7200 frames of rate ${rate} give exactly rate (got ${got})`);
  }
  ok(mineSlots(ctx, 50, 4, 1) === 1, 'mine slots floor 1 when anything is claimable');
  ok(mineSlots(ctx, 2000, 10, 0) === 0, 'mine slots 0 when nothing is claimable');
  const caps = resourceCaps(ctx, {});
  ok(caps[3] === 0x3e70, 'knowledge cap 999<<4');
  return fails;
}

async function main(argv) {
  const ctx = load(process.env.RON_DATA || DEFAULT_DATA);
  if (argv.includes('--constants')) {
    const keys = ['starting_goods', 'basic_gather', 'city_gather', 'gather_rate', 'peasant_rate', 'scholar_rate', 'oil_rate',
      'granary_bonus', 'lumbermill_bonus', 'smelter_bonus', 'refinery_bonus', 'commerce_cap', 'woodcutter_radius', 'mine_radius',
      'river_resource_value', 'territory_taxes', 'village_taxes', 'building_taxes', 'market_taxes', 'temple_taxes',
      'village_literacy', 'university_literacy', 'library_literacy', 'mtn_tiny_size', 'mtn_small_size', 'mtn_medium_size',
      'mtn_large_size', 'mtn_tiny_gather', 'mtn_small_gather', 'mtn_medium_gather', 'mtn_large_gather', 'mtn_huge_gather',
      'unit_gather_respond_range', 'dutch_interest', 'dutch_interest_cap'];
    for (const k of keys) console.log(k.padEnd(28), JSON.stringify(ctx.constants[k]));
    console.log('lands'.padEnd(28), JSON.stringify(ctx.lands.map((l) => l.filter((m) => m.good >= 0))));
    return;
  }
  if (argv.includes('--selftest')) {
    const f = selftest(ctx);
    console.log(f.length ? `FAIL\n  ${f.join('\n  ')}` : 'selftest OK');
    if (f.length) process.exitCode = 1;
    return;
  }
  console.log(examples(ctx));
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main(process.argv.slice(2)).catch((e) => { console.error(e.message); process.exitCode = 1; });
