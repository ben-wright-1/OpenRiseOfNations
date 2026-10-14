// Rise of Nations rules loader reimplementation (docs/rules-spec.md). Parses the rules XML exactly like
// Game::init_rules_data @0058ec60 + Types::init @00669cc0 (+ Lands::init) at program start, then applies the
// per-game finalisation (Types::init_civ_masks + Types::finalize_grafting from Game::init_data), and serialises the
// result as the rules image that Game::walk_rules_data writes into replays, so it can be compared byte for byte.
//
// Usage:
//   node re/tools/rulesdump.mjs [--data <Data dir>] [--json out.json] [--image out.bin]
//        load the XML, print a summary; optionally write the tables as JSON and/or the binary rules image
//   node re/tools/rulesdump.mjs --replay <file.rcx|decompressed.bin> [--data dir] [--json out.json]
//        load the XML and compare every field with the rules image embedded in the replay
//   node re/tools/rulesdump.mjs --replay-json <file.rcx> out.json
//        only decode a replay's rules image to JSON
// Game-setup options for the per-game step (they change age / military_level / tribe_mask, spec §7.6):
//   --start-tech N  --end-tech N  --game-rules N  --flag8
//
// Modules: rulesxml.mjs (MSXML-like access, value parsers), rulesimage.mjs (image codec), rules/types.mjs (type core,
// units, finalize_init_all), rules/constants.mjs, rules/tribes.mjs (tribes, categories, names, grafting),
// rules/buildings.mjs, rules/techs.mjs (techs, goods, item, spells, bonuses), rules/balance.mjs.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import * as X from './rulesxml.mjs';
import * as IMG from './rulesimage.mjs';
import * as TY from './rules/types.mjs';
import { loadConstants } from './rules/constants.mjs';
import * as TR from './rules/tribes.mjs';
import * as BU from './rules/buildings.mjs';
import * as TE from './rules/techs.mjs';
import { computeBalance } from './rules/balance.mjs';

export const DEFAULT_DATA = path.join(os.homedir(), 'Games/RiseOfNations/Data');

// Program-start load: Game::init_rules_data then Game::init_non_rules_data (Types::init, Lands::init).
// opts.loadGame: GameInfo values in effect while Types::init runs (starting_technology, starting_technology2,
// ending_technology, game_rules). TypeData::get_preq reads them for the tech AI values and the balance ages.
// Default = a default game (0 / 0 / 7 / 0), which is what the sample replay shows (open question in the spec).
export function loadRules(dataDir = DEFAULT_DATA, opts = {}) {
  const game = { starting_technology: 0, starting_technology2: 0, ending_technology: 7, game_rules: 0, ...(opts.loadGame ?? {}) };
  const ctx = { dataDir, installDir: path.dirname(dataDir), X, lang: opts.lang ?? null, types: new Array(IMG.NUM_TYPES).fill(null), gameInfo: game };
  ctx.newType = TY.newType;

  // ---- Game::init_rules_data
  const C = loadConstants(dataDir);
  ctx.constantsArray = C.array; ctx.constants = C.byName;
  ctx.cats = TR.loadCategories(dataDir);
  ctx.catKey = (list, s) => TR.catFindKey(ctx.cats[list], s);
  for (const [k, l] of Object.entries(ctx.cats)) if (l.mismatch) (ctx.warnings ??= []).push(`Category data mismatch: ${k}`);
  ctx.tribes = TR.loadTribes(dataDir);
  const names = TR.makeDisplayNameLookup(dataDir, ctx.lang);
  ctx.lookupDisplayName = (o, n) => TR.lookupDisplayName(names, o, n);
  TY.makeKeys(ctx);

  // ---- Types::init
  const els = (file, tag, wrap) => { let r = X.readXml(dataDir, file); if (wrap) r = X.child(r, wrap); return X.children(r, tag); };
  const expect = (list, n, what) => { if (list.length !== n) throw new Error(`${what}: ${list.length} elements, expected ${n}`); return list; };

  const techs = expect(els('techrules.xml', 'TECH'), 85, 'techrules.xml TECH');
  for (let pass = 0; pass < 2; pass++) techs.forEach((el, i) => TE.initTech(ctx, el, 544 + i, pass));
  TE.loadAlternateNames(ctx);
  const goods = expect(els('resourcerules.xml', 'RESOURCE', 'RESOURCES'), 50, 'resourcerules.xml RESOURCE');
  for (let pass = 0; pass < 2; pass++) goods.forEach((el, i) => TE.initGood(ctx, el, i, pass));
  const builds = expect(els('buildingrules.xml', 'BUILDING'), 129, 'buildingrules.xml BUILDING');
  for (let pass = 0; pass < 2; pass++) builds.forEach((el, i) => {
    if (pass === 0) ctx.types[414 + i] = TY.newType(414 + i, 'build');
    BU.initBuild(ctx, el, 414 + i, pass);
  });
  BU.postBuildings(ctx);
  if (!opts.noMasks) BU.loadMasks(ctx, { onError: (m) => (ctx.warnings ??= []).push(m) });
  const units = expect(els('unitrules.xml', 'UNIT'), 364, 'unitrules.xml UNIT');
  for (let pass = 0; pass < 5; pass++) {
    let last = 0, lastEl = null;
    units.forEach((el, i) => {
      const t = 50 + i;
      if (pass === 0) ctx.types[t] = TY.newType(t, 'unit');
      // passes >= 2: a unit named like the previous distinct unit is initialised from that unit's element
      if (pass < 2 || last === 0 || !X.iequals(ctx.types[t].name, ctx.types[last].name)) { last = t; lastEl = el; }
      TY.initUnit(ctx, pass < 2 ? el : lastEl, t, pass);
    });
  }
  for (let t = 50; t < 402; t++) TY.fillRelativeValues(ctx, ctx.types[t]);
  const items = expect(els('itemrules.xml', 'ITEM', 'ITEMS'), 1, 'itemrules.xml ITEM');
  for (let pass = 0; pass < 2; pass++) TE.initItem(ctx, items[0], 543, pass);
  const crafts = expect(els('craftrules.xml', 'CRAFT'), 55, 'craftrules.xml CRAFT');
  crafts.forEach((el, i) => TE.initSpell(ctx, el, 629 + i));
  TE.postSpells(ctx);
  const bonuses = expect(els('rules.xml', 'BONUS', 'TECHBONUSES'), 122, 'rules.xml TECHBONUSES/BONUS');
  bonuses.forEach((el, i) => TE.initBonus(ctx, el, 684 + i));
  techs.forEach((el, i) => TE.setResearch(ctx, el, 544 + i));
  for (let t = 544; t < 629; t++) TE.computeAiValues(ctx, t);
  TY.finalizeInitAll(ctx);
  // Balance::init (last step of Types::init). It reads each type's age through ObjectTypeData::get_age, which falls
  // back to get_age_slow while `age` is still -1, i.e. the age under the GameInfo in effect at load time.
  const withAge = ctx.types.map((o) => (o && o.age === -1 && (o.kind === 'unit' || o.kind === 'build') && !(o.type >= 402 && o.type < 414)
    ? { ...o, age: TR.getAgeSlow(ctx, o, game) } : o));
  ctx.balance = computeBalance(dataDir, { types: withAge, constants: ctx.constants });
  // ---- Game::init_non_rules_data after Types::init
  TE.computeLargestGather(ctx); // Lands::init @0067e730
  return ctx;
}

// Per-game finalisation (Game::init_data @0058dca0): Constants::init again (same values), init_civ_masks,
// finalize_grafting. Then the tables equal what RecordGame::write_rules records.
// opts.game: this game's GameInfo (same keys as loadGame); opts.flag8: GameInfo flags & 8.
export function startGame(ctx, opts = {}) {
  const game = { ...ctx.gameInfo, ...(opts.game ?? {}) };
  TR.initCivMasks(ctx);
  TR.finalizeGrafting(ctx, { gameFlags8: !!opts.flag8, gameSettings: game });
  return ctx;
}

// The rules image object (rulesimage.mjs writeImage / readImage shape).
export function toImage(ctx) {
  return { types: ctx.types, constants: ctx.constantsArray, balance: ctx.balance, tribes: ctx.tribes };
}

// Field-by-field comparison with a decoded image; returns [{where, field, ours, theirs}].
export function compareImages(ours, theirs) {
  const out = [], eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const groups = (k) => [IMG.TYPE_FIELDS, ['display_name'],
    ...(k === 'good' || k === 'unit' || k === 'build' || k === 'item' ? [IMG.OBJECT_FIELDS, ['is_list', 'is_strict_list']] : []),
    ...(k === 'unit' ? [IMG.UNIT_FIELDS] : k === 'build' ? [IMG.BUILD_FIELDS] : k === 'good' ? [IMG.GOOD_FIELDS] : []),
    ...(k === 'tech' ? [IMG.TECH_FIELDS, ['alternate_name']] : k === 'spell' ? [IMG.SPELL_FIELDS] : [])];
  const norm = (f, v) => (f === 'is_list' || f === 'is_strict_list') ? (v && v.count ? v : { count: 0 }) : v;
  for (let t = 0; t < IMG.NUM_TYPES; t++) {
    const a = ours.types[t], b = theirs.types[t];
    for (const g of groups(b.kind)) for (const f of g) {
      const n = Array.isArray(f) ? f[0] : f;
      let x = norm(n, a[n]), y = norm(n, b[n]);
      if ((n === 'job_time' || n === 'res_time' || n.endsWith('flags') || n === 'job_extra_time') && typeof x === 'number') { x >>>= 0; y >>>= 0; }
      if (n === 'obj_masks' && typeof x === 'number') { x |= 0; y |= 0; }
      if (!eq(x, y)) out.push({ where: `type ${t} ${a.name ?? ''}`.trim(), field: n, ours: x, theirs: y });
    }
  }
  ours.constants.forEach((v, i) => { if (v !== theirs.constants[i]) out.push({ where: `constants +0x${(i * 4).toString(16)}`, field: 'int', ours: v, theirs: theirs.constants[i] }); });
  for (let r = 0; r < IMG.BALANCE_N; r++) for (let c = 0; c < IMG.BALANCE_N; c++)
    if (ours.balance[r][c] !== theirs.balance[r][c]) out.push({ where: `balance[${r + 50}][${c + 50}]`, field: 'i16', ours: ours.balance[r][c], theirs: theirs.balance[r][c] });
  for (let i = 0; i < IMG.NUM_TRIBES; i++) for (const [n] of IMG.TRIBE_FIELDS)
    if (!eq(ours.tribes[i][n], theirs.tribes[i][n])) out.push({ where: `tribe ${i}`, field: n, ours: ours.tribes[i][n], theirs: theirs.tribes[i][n] });
  return out;
}

// JSON form for a C++ loader / inspection: walked and non-walked fields of every table.
function toJson(ctx) {
  const strip = (o) => { const r = {}; for (const [k, v] of Object.entries(o)) if (typeof v !== 'function') r[k] = v; return r; };
  return { constants: ctx.constants, types: ctx.types.map(strip), tribes: ctx.tribes, balance: ctx.balance,
    categories: Object.fromEntries(['unit_cats', 'build_cats', 'tech_cats'].map((k) => [k, ctx.cats[k]])) };
}

// ---------------------------------------------------------------- CLI
async function main(argv) {
  const arg = (k) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : undefined; };
  const dataDir = arg('--data') ?? DEFAULT_DATA;
  if (arg('--replay-json')) {
    const r = IMG.loadReplayImage(arg('--replay-json'));
    fs.writeFileSync(argv[argv.indexOf('--replay-json') + 2], JSON.stringify(r.rules));
    console.log(`rules image at 0x${r.offset.toString(16)}, ${r.end - r.offset} bytes`);
    return;
  }
  const game = {};
  if (arg('--start-tech')) game.starting_technology = game.starting_technology2 = +arg('--start-tech');
  if (arg('--end-tech')) game.ending_technology = +arg('--end-tech');
  if (arg('--game-rules')) game.game_rules = +arg('--game-rules');
  const ctx = startGame(loadRules(dataDir), { game, flag8: argv.includes('--flag8') });
  const ours = toImage(ctx);
  const image = IMG.writeImage(ours);
  console.log(`loaded ${dataDir}: 806 types, 848 constants, ${IMG.BALANCE_N}x${IMG.BALANCE_N} balance, 24 tribes; image ${image.length} bytes`);
  for (const w of ctx.warnings ?? []) console.log('warning:', w);
  if (arg('--json')) fs.writeFileSync(arg('--json'), JSON.stringify(toJson(ctx), null, 1));
  if (arg('--image')) fs.writeFileSync(arg('--image'), image);
  if (arg('--replay')) {
    const r = IMG.loadReplayImage(arg('--replay'));
    const theirs = r.buffer.subarray(r.offset, r.end);
    const same = Buffer.compare(image, theirs) === 0;
    console.log(`replay image at 0x${r.offset.toString(16)}, ${theirs.length} bytes: ${same ? 'BYTE-IDENTICAL' : 'differs'}`);
    if (!same) {
      const diffs = compareImages(ours, r.rules);
      const byField = {};
      for (const d of diffs) (byField[d.field] ??= []).push(d);
      for (const [f, l] of Object.entries(byField))
        console.log(`  ${f}: ${l.length} mismatches, e.g. ${l[0].where}: ours ${JSON.stringify(l[0].ours).slice(0, 60)} replay ${JSON.stringify(l[0].theirs).slice(0, 60)}`);
      process.exitCode = 1;
    }
  }
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main(process.argv.slice(2));
