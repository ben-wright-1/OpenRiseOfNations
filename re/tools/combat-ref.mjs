#!/usr/bin/env node
// Reference model of Rise of Nations combat damage (docs/combat-spec.md §4), from the decompiled
// ObjectData::get_damage @00644130, Object::do_damage @0064a480 and Object::take_damage @00652020.
// Reads the rules from the user's install (never writes to it) through rulesdump.mjs.
//
//   node re/tools/combat-ref.mjs "Hoplites" "Light Cavalry"        one matchup, both directions
//   node re/tools/combat-ref.mjs Bowmen Hoplites --flank 0x40000000 situational modifiers (see --help)
//   node re/tools/combat-ref.mjs --examples                        the worked examples of the spec
//   node re/tools/combat-ref.mjs --selftest                        internal consistency checks
//   node re/tools/combat-ref.mjs --dump out.tsv                    every attacker 50..401 x target 50..542 (plain
//                                                                  situation): get_damage, per-call hp/frac (C++ parity)
//   node re/tools/combat-ref.mjs --dump-situations out.tsv         the same pairs under each SITUATIONS entry
//                                                                  (ron-combat-check --situations parity)
//
// All arithmetic is int32 with C truncating division, exactly as the binary does it. Leader-dependent
// bonuses (nation powers, generals, wonders, rare resources) are not derived here: pass them as explicit
// --attack-bonus / --armor-bonus or the situation flags below.

import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadRules, startGame, DEFAULT_DATA } from './rulesdump.mjs';

// ---------------------------------------------------------------- int helpers
const i32 = (x) => x | 0;
const mul = (a, b) => Math.imul(a, b);
const div = (a, b) => { if (b === 0) throw new Error('division by zero'); return i32(a / b); }; // C truncation
const shr8 = (x) => div(x, 256);  // (x + ((x >> 31) & 0xff)) >> 8 = truncating /256
const u32 = (x) => x >>> 0;

// ObjMaskType bits (PDB enum)
export const M = {
  ARMORED: 1, BOMBARD: 2, CIVILIAN: 4, MUSKET_INF: 8, ELEPHANT: 0x10, FOOT: 0x20, GUN: 0x40, HEAVY_INF: 0x80,
  MODERN_INF: 0x100, CARRY_AIR: 0x200, FOOT_ARCHER: 0x400, LARGE: 0x800, MOUNTED: 0x1000, NAVAL: 0x2000,
  HORSE_ARCHER: 0x4000, SPARSE: 0x8000, LIGHT_INF: 0x10000, ARCHERY: 0x20000, SIEGE: 0x40000, WAR_MACHINE: 0x80000,
  ARMORPIERCE: 0x100000, VEHICLE: 0x200000, MELEE: 0x400000, EXPLOSIVE: 0x800000, HEAVY_CAV: 0x1000000,
  DETECT: 0x2000000, NO_UPGRADE: 0x4000000, MISSILE: 0x8000000, AIR: 0x10000000, LIGHT_CAV: 0x20000000,
  PIKE: 0x40000000, ANTI_AIR: 0x80000000,
};
const UNITTYPE2_PACKER = 4, UNITTYPE2_SUPPLY = 0x40, UNITTYPE2_CARAVAN = 8, UNITTYPE_SIEGE = 0x20000;
// TypeIndex constants used by get_damage
const T = { MILITIA: 66, CATAPULT: 265, FLAMETHROWER: 131, BARK: 323, V2ROCKET: 313, BARRACKS: 427, STABLE: 428,
  FACTORY: 431, FORTX: 443, CASTLE: 444, REDFORT: 534, SUPERCOLLIDER: 541 };

// ---------------------------------------------------------------- rules
export function load(dataDir = DEFAULT_DATA) {
  const ctx = loadRules(dataDir);
  startGame(ctx);
  return ctx;
}

export function findType(ctx, key) {
  if (/^\d+$/.test(String(key))) return ctx.types[+key];
  const k = String(key).toLowerCase();
  for (let t = 50; t < 543; t++) {
    const o = ctx.types[t];
    if (o && (o.name.toLowerCase() === k || (o.type_name ?? '').toLowerCase() === k)) return o;
  }
  throw new Error(`no unit/building named "${key}"`);
}

const isUnit = (o) => o.type >= 50 && o.type < 414;
const isBuild = (o) => o.type >= 414 && o.type < 543;
// ObjectTypeData::is (non-strict): own type, or membership in is_list.
function isA(o, n) {
  if (o.type === n) return true;
  const l = o.is_list;
  return !!(l && l.v && l.v.slice(0, l.count).includes(n));
}
const isSiegeType = (o) => isUnit(o) && (o.unit_flags & UNITTYPE_SIEGE) !== 0;  // UnitTypeData::is_siege (vslot 0x10c)
const isMissileType = (o) => isUnit(o) && (o.obj_masks & M.MISSILE) !== 0;
const isSupplyType = (o) => isUnit(o) && (o.unit_flags2 & UNITTYPE2_SUPPLY) !== 0;
const isCaravanType = (o) => isUnit(o) && (o.unit_flags2 & UNITTYPE2_CARAVAN) !== 0;

// ---------------------------------------------------------------- ObjectData::get_damage @00644130
// att, def: rules types. s: situation (all optional; defaults = two units on open flat ground, owned by
// different players, attacker in its own territory, nothing special). Returns {damage, deathType, steps[]}.
export function getDamage(ctx, att, def, s = {}) {
  const C = ctx.constants;
  const steps = [];
  const note = (what, v) => steps.push([what, v]);
  const balance = ctx.balance[att.type - 50][def.type - 50];
  let deathType = 2;                              // DEATH_SHOT
  let armor = s.armor ?? (i32(def.armor) + (s.armorBonus ?? 0));   // target vslot 0x124 (UnitData::armor = myarmor + general bonuses)
  const attack = s.attack ?? (i32(att.attack) + (s.attackBonus ?? 0)); // attacker vslot 0x120 (×10 units)
  let amask = u32(att.obj_masks);
  const dmask = u32(def.obj_masks);
  const attIsBuild = isBuild(att), defIsBuild = isBuild(def), attIsUnit = isUnit(att), defIsUnit = isUnit(def);
  // Forts/castles and city buildings (SubObject flags & CITY) of a leader with GUNPOWDER_AGE: ARCHERY -> GUN
  if ((att.type === T.CASTLE || att.type === T.FORTX || s.attackerIsCity) && s.attackerHasGunpowder)
    amask = u32((amask & ~M.ARCHERY) | M.GUN);
  let d = div(mul(attack, balance), 100);
  note(`attack ${attack} x balance ${balance}% / 100`, d);
  if (amask & M.MUSKET_INF) { armor = div(mul(armor, 0x85), 100); note('attacker MUSKET_INF: armor x133/100', armor); }
  if (attIsBuild && (dmask & M.SIEGE) && defIsUnit && (def.unit_flags2 & UNITTYPE2_PACKER) && !s.targetPacked) {
    d = div(d, 3); note('building vs unpacked packer siege: /3', d);
  }
  if (attIsBuild && defIsUnit && def.attack !== 0 && s.targetMoving) {
    if (!(dmask & M.FOOT)) {
      if (isSiegeType(def) || isSupplyType(def) || isCaravanType(def)) { d = div(d, 2); note('building vs moving siege/supply/caravan: /2', d); }
    } else { d = div(mul(d, 3), 4); note('building vs moving foot: x3/4', d); }
  }
  if (attIsBuild && defIsBuild && s.targetActive === false) { d = mul(d, 4); note('building vs unfinished building: x4', d); }
  if (defIsBuild && ((att.type >= 50 && att.type <= 53) || isA(att, T.MILITIA)) && !s.targetInAttackerTerritory) {
    d = div(d, 2); note('citizen/militia vs building outside own territory: /2', d);
  }
  if (isA(att, T.V2ROCKET) && defIsBuild && s.targetActive === false) { d = div(d, 2); note('V2 vs unfinished building: /2', d); }
  if (attIsUnit && att.domain === 2 && !(att.obj_masks & M.MISSILE) && def.type === T.REDFORT) {
    d = div(mul(100 - C.red_fort_air_defense, d), 100); note('aircraft vs Red Fort', d);
  }
  if (defIsUnit) {
    if (s.targetPacked) { d = mul(d, 2); note('target packed: x2', d); }
    if ((def.unit_flags2 & UNITTYPE2_PACKER) && !s.targetPacked) { armor += 1; note('unpacked packer: armor +1', armor); }
  }
  if ((amask & M.EXPLOSIVE) || (amask & M.BOMBARD)) deathType = 3;   // DEATH_SPLODED
  if (attIsBuild) deathType = s.buildingShot === 4 ? 3 : 2;            // BuildData::get_shot
  if (s.wellingtonSiegeBonus) { d += C.wellington_siege_attack; note('Wellington', d); }
  if (s.japaneseBarracksAges != null) {                               // has_tribe_bonus(15), attacker from BARRACKS
    let k = C.japanese_damage;
    if (k < 0) k = -mul(k, s.japaneseBarracksAges);
    d = div(mul(k + 100, d), 100); note('Japanese barracks unit', d);
  }
  if ((attIsBuild || (att.domain === 0 && def.domain !== 0)) && defIsUnit &&
      (s.targetSevereAttrition || (def.domain === 1 && s.teamStyle2NotTarget))) {
    d = mul(d, 2); note('x2 (severe attrition / team style)', d);
  }
  if (s.splash) {
    if (defIsUnit) { d = div(d, def.uber_size); note('splash: / target uber_size', d); }
    d = div(mul(att.splash_percent, d), 100); note('splash_percent', d);
    if (defIsUnit) {
      if (isSiegeType(def) && (def.unit_flags2 & UNITTYPE2_PACKER) && !s.targetPacked) { d = mul(d, 3); note('splash vs unpacked packer siege: x3', d); }
      if (isA(def, T.BARK)) { d = div(mul(d, 25), 100); note('splash vs bark line: x25/100', d); }
    }
  }
  if (defIsUnit && s.targetInRiver) { d = shr8(mul(C.river_modifier, d)); note('target in river (z < 0)', d); }
  if (defIsUnit && s.targetDecoy) { armor = 0; d = mul(Math.max(attack, d), 1000); note('decoy', d); }
  if (defIsBuild && s.targetInUnfriendlyTerritory && !s.scenarioNoBorderPenalty) {
    armor = 0;
    if (def.attack === 0) { d = mul(d, 4); note('building in unfriendly territory: armor 0, x4', d); }
    else note('armed building in unfriendly territory: armor 0', d);
  }
  // flank: both units, neither civilian nor air, neither naval
  if (attIsUnit && defIsUnit && !(amask & M.CIVILIAN) && !(dmask & M.CIVILIAN) && !(amask & M.AIR) && !(dmask & M.AIR) &&
      !(amask & M.NAVAL) && !(dmask & M.NAVAL) && s.targetAngle != null && s.attackAngle != null) {
    const f = flankLevel(s.targetAngle, s.attackAngle);
    if (f) {
      let bonus = C.flank_bonus;
      if (dmask & M.VEHICLE) bonus = shr8(mul(C.vehicle_flank_bonus, bonus));
      else if (dmask & M.MOUNTED) bonus = shr8(mul(C.cavalry_flank_bonus, bonus));
      d = div(mul(mul(bonus, f) + 100, d), 100); note(`flank level ${f} (bonus ${bonus}%)`, d);
    }
  }
  let hp = div(d + 5, 10) - armor;
  note(`(d + 5) / 10 - armor ${armor}`, hp);
  if (s.overkill && i32(att.max_range) !== 0 && attIsUnit && defIsUnit) {
    hp = shr8(mul(C.overkill_damage, hp)); note('overkill (second hitter within overkill_frames)', hp);
    if (isA(def, T.CATAPULT) && !isSiegeType(att)) { hp = div(hp, 2); note('overkill vs catapult line: /2', hp); }
  }
  if ((dmask & (M.LIGHT_INF | M.MODERN_INF | M.MUSKET_INF)) && s.targetOnRocky) { hp = shr8(mul(C.rocky_modifier, hp)); note('rocky tile', hp); }
  if (def.domain !== 2 && att.domain !== 2 && !isSiegeType(att) && (s.heightDiff ?? 0) > 0) {
    hp = hp + div(mul(mul(s.heightDiff, C.height_bonus), hp), mul(C.height_increment, 100)); note('height', hp);
  }
  if (defIsUnit && s.targetEntrenched && !isA(att, T.FLAMETHROWER)) {
    const front = s.trenchAngle != null && s.attackAngle != null ? entrenchSector(s.trenchAngle, s.attackAngle) : 0;
    if (s.splash || front === 0) {
      hp = shr8(mul(C.entrenchment_modifier, hp)); note('entrenched', hp);
      if (s.antipater) { hp = shr8(mul(C.antipater_entrench_bonus, hp)); note('Antipater entrenchment', hp); }
    }
  }
  if (s.cossackBonus && C.russian_cossack_damage !== 0 && (isSupplyType(def) || isSiegeType(def))) {
    hp = div(mul(C.russian_cossack_damage + 100, hp), 100); note('Russian stable unit vs supply/siege', hp);
  }
  if (hp < 1 && (att.domain !== 0 || def.domain !== 1) &&
      ((dmask & M.AIR) >>> 0) === (((amask >>> 3) & M.AIR) >>> 0) && !s.splash) { hp = 1; note('minimum 1', hp); }
  if (def.type === T.SUPERCOLLIDER && att.domain === 2 && C.super_immune) { hp = 0; note('super immune', hp); }
  if (defIsBuild && s.recaptureOwnCity) { hp = shr8(mul(C.recapture_city_modifier, hp)); note('recapture own city', hp); }
  return { damage: hp, deathType, balance, steps };
}

// flanking() @0092cfe0 with the cone test of get_damage: 0 = frontal, else 1 or 2.
export function flankLevel(targetAngle, attackAngle) {
  const d = u32(u32(targetAngle) - u32(attackAngle) - 0x80000000);
  if (d < 0x2aaaaaaa) return 0;
  if (d > 0xd5555555) return 0;
  return (u32(d + 0xa0000000) > 0x40000000 ? 1 : 0) + 1;
}
// Entrenchment sector in get_damage: 0 = the hit comes outside the entrenched front.
export function entrenchSector(trenchAngle, attackAngle) {
  const x = u32(trenchAngle - attackAngle);
  if (u32(x + 0x55555556) < 0xaaaaaaac) return (u32(x + 0x20000000) > 0x40000000 ? 1 : 0) + 1;
  return 0;
}

// ---------------------------------------------------------------- Object::do_damage scaling @0064a480
// Returns the (hp, sixteenths) that take_damage receives for one call. mult is the 8.8 multiplier the caller
// passes (0x100 for melee and normal ammo). ammo: true when the hit comes from an Ammo (param_5 >= 0).
export function scaleDamage(att, hp, { mult = 0x100, ammo = false, zoc = false } = {}) {
  let v = hp;
  if (zoc) v = mul(v, 2);                              // target unit_masks & UNIT_ZOC, applied before scaling
  if (isUnit(att)) {
    v = mul(v, mult);
    if (v < 0x101) v = 0x100;
    if (ammo) v = div(v, att.ammo_per_att);
    v = div(v, att.uber_size);
  } else {
    v = div(mul(v, mult), att.ammo_per_att);
  }
  const q16 = div(v, 16);
  return { hp: div(q16, 16), frac: q16 % 16 };
}

// Object::take_damage accumulation: damage += hp + (frac_acc + frac) / 16, frac_acc = (..) % 16.
// Minimum 1/16 per call. Returns the number of calls needed to reach `threshold` from 0 damage.
export function hitsToKill(hit, threshold) {
  let dmg = 0, frac = 0, n = 0;
  let { hp, frac: f } = hit;
  if (hp < 1 && f < 1) f = 1;
  while (dmg < threshold) {
    const t = frac + f;
    dmg += hp + div(t, 16);
    frac = t % 16;
    if (++n > 100000) return Infinity;
  }
  return n;
}
// Per-member hit threshold (take_damage): hits/uber for members, the remainder for the last one.
export function memberThreshold(def, last = false) {
  const h = i32(def.hits), u = isUnit(def) ? def.uber_size : 1;
  if (u <= 1) return h;
  return last ? h - div(mul(u - 1, h), u) : div(h, u);
}

// ---------------------------------------------------------------- reporting
function matchup(ctx, att, def, s, out = console.log) {
  const r = getDamage(ctx, att, def, s);
  const hit = scaleDamage(att, r.damage, { ammo: i32(att.max_range) !== 0 && i32(att.ammo_per_att) > 0, zoc: s.zoc });
  const thr = memberThreshold(def), thrLast = memberThreshold(def, true);
  const n = hitsToKill(hit, thr);
  out(`${att.name} (${att.type}) -> ${def.name} (${def.type})`);
  for (const [w, v] of r.steps) out(`    ${w.padEnd(58)} ${v}`);
  out(`  get_damage = ${r.damage} hp (death type ${r.deathType}); per call from one member: ${hit.hp} + ${hit.frac}/16 hp`);
  out(`  attacker: uber ${isUnit(att) ? att.uber_size : 1}, recharge ${att.recharge}, range ${att.min_range}-${att.max_range}, ammo/att ${att.ammo_per_att}`);
  out(`  defender: hits ${def.hits}, uber ${isUnit(def) ? def.uber_size : 1} -> member threshold ${thr}${thr !== thrLast ? ` (last ${thrLast})` : ''}; ${n} hits to kill one member`);
  return { r, hit, n };
}

// Flagged situations for --dump-situations (names shared with apps/combat-check). The entrenchment entry gives the
// trench angle that puts the hit outside the entrenched front (sector 0), as the default of getDamage does.
export const SITUATIONS = [
  ['splash', { splash: true }], ['overkill', { overkill: true }],
  ['flank_side', { targetAngle: 0, attackAngle: 0x40000000 }], ['flank_rear', { targetAngle: 0x40000000, attackAngle: 0x40000000 }],
  ['packed', { targetPacked: true }], ['moving', { targetMoving: true }], ['river', { targetInRiver: true }],
  ['rocky', { targetOnRocky: true }], ['entrenched', { targetEntrenched: true, trenchAngle: 0x80000000, attackAngle: 0 }],
  ['decoy', { targetDecoy: true }], ['height', { heightDiff: 400 }], ['unfinished', { targetActive: false }],
  ['unfriendly', { targetInUnfriendlyTerritory: true }], ['own_land', { targetInAttackerTerritory: true }],
];

const EXAMPLES = [
  ['Hoplites', 'Light Cavalry'], ['Light Cavalry', 'Hoplites'], ['Bowmen', 'Hoplites'], ['Hoplites', 'Bowmen'],
  ['Slingers', 'Light Cavalry'], ['Light Cavalry', 'Bowmen'], ['Catapult', 'Barracks'], ['Bowmen', 'Light Cavalry'],
];

function selftest(ctx) {
  let bad = 0;
  const check = (c, m) => { if (!c) { bad++; console.log('FAIL', m); } };
  check(flankLevel(0, 0x80000000) === 0, 'head-on is frontal');
  check(flankLevel(0x40000000, 0x40000000) === 1, 'same facing (from behind) gives level 1');
  check(flankLevel(0, 0x40000000) === 2, 'from the side gives level 2');
  const h = findType(ctx, 'Hoplites'), lc = findType(ctx, 'Light Cavalry');
  check(ctx.balance[h.type - 50][lc.type - 50] === 166, 'Hoplites vs Light Cavalry balance cell 166');
  const r = getDamage(ctx, h, lc);
  check(r.damage === 20, `Hoplites vs Light Cavalry = 20 (got ${r.damage})`);
  const hit = scaleDamage(h, r.damage);
  check(hit.hp === 6 && hit.frac === 10, `per-member hit 6+10/16 (got ${hit.hp}+${hit.frac})`);
  check(hitsToKill({ hp: 0, frac: 0 }, 1) === 16, 'minimum 1/16 per hit');
  console.log(bad ? `${bad} failures` : 'selftest OK');
  return bad === 0;
}

const HELP = `usage: combat-ref.mjs <attacker> <defender> [options] | --examples | --selftest
  --data DIR            game Data directory (default ${DEFAULT_DATA})
  --attack-bonus N      add N to the attacker's attack (x10 units, as UnitData::attack adds bonus*10)
  --armor-bonus N       add N to the defender's armor
  --flank TA,AA         target facing angle and attack angle (32-bit binary angles, hex ok)
  --splash --overkill --river --rocky --packed --moving --entrenched --decoy --zoc
  --height N            attacker z - target z (world z units)
  --unfinished          target building not yet active
  --unfriendly          target building in unfriendly territory
  --gunpowder           attacking fort/castle/city owner has GUNPOWDER_AGE`;

async function main(argv) {
  const opt = { data: DEFAULT_DATA, s: {} };
  const pos = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const num = () => Number(argv[++i]);
    switch (a) {
      case '--help': console.log(HELP); return;
      case '--data': opt.data = argv[++i]; break;
      case '--examples': opt.examples = true; break;
      case '--selftest': opt.selftest = true; break;
      case '--dump': opt.dump = argv[++i]; break;
      case '--dump-situations': opt.dumpSituations = argv[++i]; break;
      case '--attack-bonus': opt.s.attackBonus = num(); break;
      case '--armor-bonus': opt.s.armorBonus = num(); break;
      case '--flank': { const [t, k] = argv[++i].split(',').map(Number); opt.s.targetAngle = t; opt.s.attackAngle = k; break; }
      case '--splash': opt.s.splash = true; break;
      case '--overkill': opt.s.overkill = true; break;
      case '--river': opt.s.targetInRiver = true; break;
      case '--rocky': opt.s.targetOnRocky = true; break;
      case '--packed': opt.s.targetPacked = true; break;
      case '--moving': opt.s.targetMoving = true; break;
      case '--entrenched': opt.s.targetEntrenched = true; break;
      case '--decoy': opt.s.targetDecoy = true; break;
      case '--zoc': opt.s.zoc = true; break;
      case '--height': opt.s.heightDiff = num(); break;
      case '--unfinished': opt.s.targetActive = false; break;
      case '--unfriendly': opt.s.targetInUnfriendlyTerritory = true; break;
      case '--gunpowder': opt.s.attackerHasGunpowder = true; break;
      default: pos.push(a);
    }
  }
  const ctx = load(opt.data);
  if (opt.selftest) { process.exitCode = selftest(ctx) ? 0 : 1; return; }
  if (opt.dump) {
    const rows = ['attacker\ttarget\tbalance\tget_damage\tdeath\thp\tfrac'];
    for (let a = 50; a < 402; a++) for (let d = 50; d < 543; d++) {
      if (d >= 402 && d < 414) continue;  // gaia targets: not combat units
      const att = ctx.types[a], def = ctx.types[d];
      const r = getDamage(ctx, att, def, {});
      const hit = scaleDamage(att, r.damage, { ammo: i32(att.max_range) !== 0 && i32(att.ammo_per_att) > 0 });
      rows.push([a, d, r.balance, r.damage, r.deathType, hit.hp, hit.frac].join('\t'));
    }
    (await import('node:fs')).writeFileSync(opt.dump, rows.join('\n') + '\n');
    console.log(`${rows.length - 1} rows -> ${opt.dump}`);
    return;
  }
  if (opt.dumpSituations) {
    const rows = ['situation\tattacker\ttarget\tget_damage\tdeath\thp\tfrac'];
    for (const [name, sit] of SITUATIONS) {
      for (let a = 50; a < 402; a++) for (let d = 50; d < 543; d++) {
        if (d >= 402 && d < 414) continue;
        const att = ctx.types[a], def = ctx.types[d];
        const r = getDamage(ctx, att, def, sit);
        const hit = scaleDamage(att, r.damage, { ammo: i32(att.max_range) !== 0 && i32(att.ammo_per_att) > 0 });
        rows.push([name, a, d, r.damage, r.deathType, hit.hp, hit.frac].join('\t'));
      }
    }
    (await import('node:fs')).writeFileSync(opt.dumpSituations, rows.join('\n') + '\n');
    console.log(`${rows.length - 1} rows -> ${opt.dumpSituations}`);
    return;
  }
  if (opt.examples) {
    for (const [a, d] of EXAMPLES) { matchup(ctx, findType(ctx, a), findType(ctx, d), {}); console.log(); }
    return;
  }
  if (pos.length !== 2) { console.log(HELP); process.exitCode = 2; return; }
  const att = findType(ctx, pos[0]), def = findType(ctx, pos[1]);
  matchup(ctx, att, def, opt.s);
  console.log();
  matchup(ctx, def, att, opt.s);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main(process.argv.slice(2)).catch((e) => { console.error(e.message); process.exitCode = 1; });
