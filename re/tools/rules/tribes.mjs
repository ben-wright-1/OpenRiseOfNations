// Tribes, categories, names/localisation and game-start rules changes for the RON rules loader.
// Spec and evidence: tribes.md (same folder). Run `node tribes.mjs [dataDir]` for the self-tests vs oracle.json.
//
// Exports
//   PRIMES, generateHash(str) -> [hash, hash_insensitive], stringHash(str, insensitive=false)
//   LANGUAGES, resolveGameFile(dataDir, name, lang), loadGameXml(dataDir, name, lang)
//   loadStringLookup(dataDir, file, lang), stringLookup(table, str, catName)
//   loadTribes(dataDir, opts) -> 24 Tribe records (pre-unit-load state); findTribe(tribes, key)
//   loadCategories(dataDir, opts) -> { tech_cats: [...], unit_cats: [...], ... }; catFindKey, catFindData
//   makeDisplayNameLookup(dataDir, lang), lookupDisplayName(dataDirOrCache, typeObj, name), typeNamesSection(t)
//   unitGraftPass2(ctx, u), unitGraftPass3(ctx, u)       (UnitType::init pass 2 tail / pass 3)
//   initCivMasks(ctx), finalizeGrafting(ctx, opts), applyGrafting(ctx, opts)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import * as X from '../rulesxml.mjs';

export const NUM_TRIBES = 24, BASE_UNITTYPES = 50, NUM_GRAFT = 352, TYPE_NONE = -1;

// ---------------------------------------------------------------- String::generate_hash (@00a1b6b0)
// primes[50] = ?primes@@3PAKA @00b14500 (num_primes = 50 @00b144f8), read from the exe's .rdata.
export const PRIMES = [127, 811, 1597, 2131, 2749, 4759, 5527, 5953, 8117, 9539, 10273, 10753, 11159, 12301,
  13217, 14207, 15413, 17681, 18661, 19013, 21089, 22051, 25111, 25801, 27457, 28057, 29581, 30809, 32611, 34469,
  36067, 37511, 38723, 40093, 41983, 43321, 45083, 47431, 49667, 50767, 53453, 55469, 57193, 59369, 61987, 65071,
  73421, 77849, 84223, 89009];
// MSVC towlower in the "C" locale folds only A-Z (the game never calls setlocale for LC_CTYPE as far as seen).
const towlower = (c) => (c >= 65 && c <= 90 ? c + 32 : c);
// Walks the UTF-16 code units from last to first; n is the 1-based position of the unit.
//   h  += primes[i%50]*c  + primes[c%50]*n
//   hi += primes[lc%50]*n + lc*primes[i%50]        (lc = towlower(c)); all uint32 wrap-around.
export function generateHash(str) {
  if (str == null) return [0, 0];
  let h = 0, hi = 0;
  for (let i = str.length - 1; i >= 0; i--) {
    const c = str.charCodeAt(i), lc = towlower(c), n = i + 1, p = PRIMES[i % 50];
    h = (h + Math.imul(p, c) + Math.imul(PRIMES[c % 50], n)) >>> 0;
    hi = (hi + Math.imul(PRIMES[lc % 50], n) + Math.imul(lc, p)) >>> 0;
  }
  return [h, hi];
}
// A String's hash_value is the case-sensitive hash; hash_value_insensitive the folded one. Empty/null -> 0.
export const stringHash = (str, insensitive = false) => generateHash(str)[insensitive ? 1 : 0];

// ---------------------------------------------------------------- files and languages (XML::init @00a279e0)
// LocalizationManager tables (@00b1ca28 names, @00b1ca54 gLanguageSuffix): LOC_NUM_LANGUAGES = 5.
export const LANGUAGES = [
  { id: 0, name: 'Deutsch', code: 'DE', suffix: '.7' }, { id: 1, name: 'English', code: 'EN', suffix: '' },
  { id: 2, name: 'Español', code: 'ES', suffix: '.10' }, { id: 3, name: 'Français', code: 'FR', suffix: '.12' },
  { id: 4, name: 'Italiano', code: 'IT', suffix: '.16' },
];
const suffixOf = (lang) => (lang == null ? '' : typeof lang === 'number' ? (LANGUAGES[lang]?.suffix ?? '')
  : lang === '' || lang.startsWith('.') ? lang : '.' + lang);
// Case-insensitive path resolution (the game runs on a case-insensitive file system).
function ciResolve(base, rel) {
  let cur = base;
  for (const part of rel.split(/[\\/]+/).filter(Boolean)) {
    const direct = path.join(cur, part);
    if (fs.existsSync(direct)) { cur = direct; continue; }
    let hit = null;
    try { hit = fs.readdirSync(cur).find((e) => e.toLowerCase() === part.toLowerCase()); } catch { return null; }
    if (!hit) return null;
    cur = path.join(cur, hit);
  }
  return cur;
}
// XML::init: a bare file name is "data\<name>"; a name with a directory part is taken relative to the content
// root (install dir). Then, if gLanguageSuffix[lang] is non-empty and "<file><suffix>" opens, that file is used.
export function resolveGameFile(dataDir, name, lang = null) {
  const install = path.dirname(path.resolve(dataDir));
  const hasDir = /[\\/]/.test(name);
  const file = hasDir ? ciResolve(install, name) : ciResolve(dataDir, name);
  if (!file) return null;
  const sfx = suffixOf(lang);
  if (sfx) { const alt = ciResolve(path.dirname(file), path.basename(file) + sfx); if (alt) return alt; }
  return file;
}
const xmlCache = new Map();
export function loadGameXml(dataDir, name, lang = null) {
  const file = resolveGameFile(dataDir, name, lang);
  if (!file) return null;
  if (!xmlCache.has(file)) {
    const raw = fs.readFileSync(file);
    const src = raw[0] === 0xff && raw[1] === 0xfe ? raw.toString('utf16le') : raw.toString('utf8');
    xmlCache.set(file, X.parseXml(src));
  }
  return xmlCache.get(file);
}

// ---------------------------------------------------------------- String::set_string_lookup / String::lookup
// set_string_lookup(file) (@00a19980): empty name -> "string_lookup.xml". Root children <CAT hash=H> (H = hash of
// the category name), each with <STRING hash= hash_i=>text</STRING>. lookup(str, cat) (@00a1ddb0): hash 0 (empty
// string) -> str; find CAT by hash(cat); within it the first entry with hash == hash(str) && hash_i ==
// hash_i(str) -> its text; otherwise str itself (the dev build then appends the string to the file and saves).
export function loadStringLookup(dataDir, file = '', lang = null) {
  const root = loadGameXml(dataDir, file || 'string_lookup.xml', lang);
  const cats = new Map();
  for (const cat of root ? X.children(root, 'CAT', true) : []) {
    const h = X.getAttribNum(cat, 'hash', -1) >>> 0;
    const list = [];
    for (const s of X.children(cat, 'STRING', true))
      list.push({ h: X.getAttribNum(s, 'hash', -1) >>> 0, hi: X.getAttribNum(s, 'hash_i', -1) >>> 0, text: X.text(s) });
    if (!cats.has(h)) cats.set(h, list); // first CAT with that hash wins (linear search)
  }
  return cats;
}
export function stringLookup(table, str, catName) {
  const [h, hi] = generateHash(str);
  if (h === 0) return str;
  const list = table.get(stringHash(catName));
  if (list) for (const e of list) if (e.h === h && e.hi === hi) return e.text;
  return str;
}

// ---------------------------------------------------------------- Tribes (Tribes::init @006ef430)
const trimRight = (s) => s.replace(/ +$/, '');
function readTribeFile(dataDir, file, lang) {
  const root = loadGameXml(dataDir, file, lang); // root element is <TRIBE name="...">
  if (!root) throw new Error(`Can't Open Nation File ${file}`);
  return {
    name: trimRight(X.getAttrib(root, 'name', '')),
    unit_continent: X.getTextNum(root, 'UNIT_CONTINENT', -1),
    build_continent: X.getTextNum(root, 'BUILD_CONTINENT', -1),
    backup_build_continent: X.getTextNum(root, 'BACKUP_BUILD_CONTINENT', -1),
    text_substitute: X.getAttribNum(root, 'substitute', 0),
  };
}
// Returns the 24 Tribe records as they are after Game::init_rules_data and before unit loading.
// opts.lang: language index/suffix; opts.reinit: Tribes::init(1,...) (keep previous grafts: pass opts.previous);
// opts.mod: array of mod FILE strings (GameMod overload; "default" keeps the stock file).
export function loadTribes(dataDir, opts = {}) {
  const { lang = null, reinit = false, previous = null, mod = null } = opts;
  const rules = loadGameXml(dataDir, 'rules.xml', lang);
  const sec = X.child(rules, 'TRIBES', true);
  const els = X.children(sec, 'TRIBE', true);
  if (els.length !== NUM_TRIBES) throw new Error('Number of tribes in rules.xml does not equal NUM_TRIBES');
  const tribes = [];
  for (let i = 0; i < NUM_TRIBES; i++) {
    const el = els[i];
    const prev = previous?.[i];
    const t = {
      file: '', name: prev?.name ?? '', eng_name: trimRight(X.getText(el, 'KEY')), old_name: prev?.old_name ?? '',
      tribe: 0, barbarian: 0, people: 0, backup_build_continent: 0, build_continent: 0, unit_continent: 0,
      text_substitute: 0, graft: prev ? prev.graft.slice() : new Array(NUM_GRAFT).fill(TYPE_NONE),
    };
    const stock = trimRight('TRIBES\\' + X.getText(el, 'FILE'));
    t.file = mod && mod[i] != null && mod[i] !== 'default' ? mod[i] : stock;
    const f = readTribeFile(dataDir, t.file, lang);
    t.name = f.name;
    t.unit_continent = f.unit_continent; t.build_continent = f.build_continent;
    t.backup_build_continent = f.backup_build_continent;
    t.barbarian = 0; t.text_substitute = f.text_substitute; t.tribe = i; t.people = (i / 3) | 0;
    if (!reinit) for (let k = 0; k < NUM_GRAFT; k++) t.graft[k] = BASE_UNITTYPES + k;
    tribes.push(t);
  }
  // A 25th Tribe (index 24) exists with name "Random" and constructor defaults; it is never walked.
  Object.defineProperty(tribes, 'random', { value: { name: 'Random', graft: new Array(NUM_GRAFT).fill(TYPE_NONE) } });
  return tribes;
}
// Tribes::find (@006ef2d0): case-insensitive match on eng_name (the KEY), -1 if none.
export const findTribe = (tribes, key) => tribes.findIndex((t) => X.iequals(t.eng_name, key));
export const tribeOfficialKeyOf = (tribes, i) => tribes[i]?.eng_name;

// ---------------------------------------------------------------- Categories (Categories::init @006f21d0)
// Lists in Game::init_rules_data (@0058ec60) order: [global, expected count, rules.xml CATEGORIES id].
export const CATEGORY_LISTS = [
  ['tech_cats', 4, 'tech_cats'], ['unit_cats', 9, 'unit_cats'], ['build_cats', 3, 'building_cats'],
  ['stances_combat', 6, 'combatstances'], ['stances_worker', 4, 'workerstances'], ['stances_caster', 2, 'casterstances'],
  ['stances_packer', 2, 'packerstances'], ['agename_cats', 8, 'agenameshort'], /* Tribes::init here */
  ['treaties_cats', 3, 'treaties'], ['team_styles', 14, 'gamestyles'], ['game_rules', 12, 'gamerules'],
  ['map_styles', 23, 'mapstyles'], ['diffs', 6, 'difficulties'], ['map_sizes', 7, 'mapsizes'],
  ['victories', 11, 'victories'], ['eliminations', 4, 'eliminations'], ['starting_towns', 4, 'startingtowns'],
  ['starting_resources', 13, 'startingresources'], ['starting_resources2', 5, 'defenderresources'],
  ['tech_costs', 9, 'techcosts'], ['starting_technologies', 10, 'startingtechs'],
  ['starting_technologies2', 5, 'defendertechs'], ['ending_technologies', 8, 'endingtechs'],
  ['reveal_maps', 4, 'revealmaps'], ['pop_limits', 6, 'poplimits'], ['rush_rules', 15, 'rushrules'],
  ['cannon_times', 5, 'cannontimes'], ['handicaps', 21, 'handicaps'], ['player_slots', 12, 'playerslots'],
  ['number_of_players', 8, 'numberofplayers'], ['max_observers', 7, 'maxobservers'], ['game_speeds', 5, 'gamespeeds'],
  ['scores', 12, 'scores'], ['time_limits', 9, 'timelimits'], ['chairs', 8, 'chairs'], ['wonderwins', 13, 'wonderwins'],
  ['popwins', 13, 'popwins'], ['econwins', 9, 'econwins'], ['victorytypes', 7, 'victorytypes'],
  ['gameinfo_flags', 12, 'gameinfo_flags'],
];
// _wcsicmp in the "C" locale: fold A-Z to lower case, compare UTF-16 units.
function wcsicmp(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) { const d = towlower(a.charCodeAt(i)) - towlower(b.charCodeAt(i)); if (d) return d; }
  return a.length - b.length;
}
export function loadCategoryList(rules, id, count, lookupTable) {
  const sec = X.children(rules, 'CATEGORIES', true).find((c) => X.getAttrib(c, 'id') === id);
  if (!sec) return null; // Categories::init returns 1 -> init_rules_data fails
  const items = X.children(sec, 'category');
  const n = count < 0 ? items.length : count; // count > 0 and != items: "Category data mismatch" (continues with count)
  const list = [];
  for (let i = 0; i < n; i++) {
    const e = items[i];
    const name = stringLookup(lookupTable, X.getAttrib(e, 'name', ''), 'CATNAME');
    let key = X.getAttrib(e, 'key', '');
    if (key.length === 0) key = name; // key defaults to the (looked-up) display name
    const desc = stringLookup(lookupTable, X.getAttrib(e, 'desc', ''), 'CATDESC');
    const data = [0, 0, 0, 0, 0, 0]; // ObjectArray<Category> elements are constructed; data[] defaults 0 (see md)
    X.children(e, 'data').slice(0, 6).forEach((d, k) => { data[k] = X.convertInt(X.text(d)); });
    list.push({ name, key, desc, data, alpha_order: i });
  }
  // alpha_order: exchange sort of indices by _wcsicmp(name) > 0 (empty names sort first), then
  // list[idx[k]].alpha_order = k.
  const idx = list.map((_, i) => i);
  for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) {
    const a = list[idx[i]].name, b = list[idx[j]].name;
    const swap = !a || !b ? !!a : wcsicmp(a, b) > 0;
    if (swap) { const t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
  }
  idx.forEach((ci, k) => { list[ci].alpha_order = k; });
  Object.defineProperties(list, {
    id: { value: id }, title: { value: stringLookup(lookupTable, X.getAttrib(sec, 'title', ''), 'CATTITLES') },
    listName: { value: X.getAttrib(sec, 'name', '') || id }, mismatch: { value: count >= 0 && count !== items.length },
  });
  return list;
}
export function loadCategories(dataDir, opts = {}) {
  const { lang = null } = opts;
  const rules = loadGameXml(dataDir, 'rules.xml', lang);
  const table = loadStringLookup(dataDir, '', lang); // Main::main set_string_lookup(EMPTY) -> string_lookup.xml
  const out = {};
  for (const [v, count, id] of CATEGORY_LISTS) out[v] = loadCategoryList(rules, id, count, table);
  out.mods = loadCategoryList(rules, 'mods', -1, table); // GameMods overload @006f1f20 then appends mod entries
  return out;
}
// Categories::find_key (@006f20e0): String== (case-insensitive) on key, -1 (+ Error::key) if none.
export const catFindKey = (list, s) => list.findIndex((c) => X.iequals(c.key, s));
// Categories::find_data (@006f1e30): first index whose data[0] == value (param_1 is ignored), -1 if none.
export const catFindData = (list, value) => list.findIndex((c) => c.data[0] === value);

// ---------------------------------------------------------------- display names (Type::lookup_display_name @006677f0)
// Section of typenames.xml by TypeIndex, in the order the virtual is_* tests run (TypeData defaults, no overrides).
export function typeNamesSection(t) {
  if (t >= 414 && t <= 542) return 'BUILDS';
  if (t >= 50 && t <= 413) return 'UNITS';
  if (t >= 629 && t <= 683) return 'SPELLS';
  if (t === 543) return 'ITEMS';
  if (t < 50) return 'GOODS';
  if (t >= 544 && t <= 550) return 'AGES';
  if (t >= 551 && t <= 578) return 'TECHS'; // is_epoch_type
  if (t >= 684 && t <= 805) return 'BONUSES';
  return 'MISC'; // 579..628 (final/gov/other techs)
}
export function makeDisplayNameLookup(dataDir, lang = null) {
  const root = loadGameXml(dataDir, 'typenames.xml', lang); // ROOT
  const sections = new Map();
  for (const sec of root ? root.children.filter((c) => c.tag) : []) {
    const m = new Map();
    for (const s of X.children(sec, 'STRING', true)) {
      const h = X.getAttrib(s, 'hash', '');
      if (!m.has(h)) m.set(h, X.text(s)); // selectSingleNode: first match
    }
    if (!sections.has(sec.tag)) sections.set(sec.tag, m);
  }
  return { dataDir, lang, sections };
}
const dnCaches = new Map();
// Lookup XPath "<SECTION>/STRING[@hash="<hash_value>"]" (decimal unsigned); found and non-empty -> its text,
// else the name itself (the game also appends <STRING hash= needed="1">name</STRING> and tries to save).
export function lookupDisplayName(dataDirOrCache, typeObj, name) {
  let cache = dataDirOrCache;
  if (typeof cache === 'string') {
    if (!dnCaches.has(cache)) dnCaches.set(cache, makeDisplayNameLookup(cache));
    cache = dnCaches.get(cache);
  }
  const t = typeof typeObj === 'number' ? typeObj : typeObj.type;
  const sec = cache.sections.get(typeNamesSection(t));
  const v = sec?.get(String(stringHash(name)));
  return v ? v : name;
}

// ---------------------------------------------------------------- unit-side grafting
const bit = (t) => 1 << t; // tribe t <-> bit t of tribe_mask (TRIBE_MASK text is MSB first: char 23-t)
// End of UnitType::init pass 2 (@0061ab50): for a unit u with graft g >= 0, every tribe whose bit is set in
// u.tribe_mask gets graft[g-50] = u. Units are processed in document order, so the last writer wins.
export function unitGraftPass2(ctx, u) {
  const o = ctx.types[u], g = o.graft;
  if (g < 0) return;
  for (let t = 0; t < NUM_TRIBES; t++) if (o.tribe_mask & bit(t)) ctx.tribes[t].graft[g - BASE_UNITTYPES] = u;
}
// UnitType::init pass 3: for each tribe t where u is not native (bit clear) but is replaced (graft[u-50] != u),
// every other unit v (50..401) that is not native to t and grafts onto u is redirected to t's replacement of u.
// For gaia u (402..413) graft[u-50] reads past the array (next Tribe's header); no unit grafts onto gaia, so
// this has no effect and is skipped.
export function unitGraftPass3(ctx, u) {
  if (u >= 402) return;
  const o = ctx.types[u];
  for (let t = 0; t < NUM_TRIBES; t++) {
    const tr = ctx.tribes[t];
    if (tr.graft[u - BASE_UNITTYPES] === u || (o.tribe_mask & bit(t))) continue;
    for (let v = 50; v < 402; v++) {
      if (v === u) continue;
      const w = ctx.types[v];
      if (!(w.tribe_mask & bit(t)) && w.graft === u) tr.graft[v - BASE_UNITTYPES] = tr.graft[u - BASE_UNITTYPES];
    }
  }
}
// Types::init_civ_masks (@00669a60) / BuildType::init_civ_masks (@006320c0): civ_graph_mask = 0 for 414..542.
// (The building_graphics.xml probing tests an XMLElement that is never assigned, so no bit is ever set.)
export function initCivMasks(ctx) {
  for (let t = 414; t < 543; t++) if (ctx.types[t]) ctx.types[t].civ_graph_mask = 0;
}
// TypeData::get_preq (@00668700) with player -1. s = starting age (min with starting_technology2 under game
// rule 8), e = ending_technology. Defaults (s=0, e>=7) make it return preq[i] except the slot-2 checks.
const isTech = (t) => t >= 544 && t <= 628, isAge = (t) => t >= 544 && t <= 550, isEpoch = (t) => t >= 551 && t <= 578;
export function getPreq(ctx, o, i, gs = {}) {
  const T = ctx.types;
  const { starting_technology = 0, starting_technology2 = 0, ending_technology = 7, game_rules = 0 } = gs;
  if (i < 0 || i > 2) return TYPE_NONE;
  if (i === 0) return o.preq[0];
  if (i === 2) {
    const p = o.preq[2];
    if (p >= 0) {
      if (isEpoch(p)) {
        const p1 = getPreq(ctx, o, 1, gs);
        if (p1 >= 0 && isEpoch(p1) && T[p].cat === T[p1].cat && p <= p1) return TYPE_NONE;
      }
      if (isAge(p) && o.where === 420 /* UNIVERSITY */ && ending_technology < T[p].age + 1) return TYPE_NONE;
    }
    return p;
  }
  let p = o.preq[1]; // iroquois_govs_early branch needs a player (param_2 >= 0): skipped for -1
  const s = game_rules === 8 ? Math.min(starting_technology2, starting_technology) : starting_technology;
  if (p >= 0 && (s !== 0 || ending_technology < 7)) {
    const isUnit = o.type >= 50 && o.type <= 413;
    if (isEpoch(p) && isUnit && T[p].cat === 0) {
      const age = T[p].age;
      if (age < s) return TYPE_NONE;
      const v = X.i32(Math.trunc(28 / (ending_technology - s + 1)) * (age - s + 1) + 3);
      const q = X.cdiv(v, 4) + 571;
      return q > 571 ? (q < 579 ? q : 578) : 572;
    }
    if (s !== 0 && isEpoch(p)) {
      const age = T[p].age;
      if (s <= age && 7 - s > 0) {
        const c = T[p].cat;
        const base = c === 1 ? 565 : c === 2 ? 558 : c === 3 ? 551 : 572;
        let q = X.cdiv((age - s + 1) * 7, 7 - s) - 1 + base;
        if (o.preq[1] < q) q = o.preq[1];
        const p2 = o.preq[2];
        if (p2 < 0 || !isEpoch(p2) || T[q].cat !== T[p2].cat || p2 <= q) return q;
      }
      return TYPE_NONE;
    }
  }
  return p;
}
// ObjectTypeData::get_age_slow (@00661a00): first preq (i < num_preq: units 2, buildings 3) that is a tech:
// tech.age + 1 if it is an age (544..550), else tech.age; none -> 0.
export function getAgeSlow(ctx, o, gs) {
  const n = o.type >= 414 ? 3 : 2;
  for (let i = 0; i < n; i++) {
    const p = getPreq(ctx, o, i, gs);
    if (p >= 0 && isTech(p)) return isAge(p) ? ctx.types[p].age + 1 : ctx.types[p].age;
  }
  return 0;
}
// UnitTypeData::get_military_level_slow (@0061d4d0): military epoch (551..578 with cat 0) from preq[0], else
// from get_preq(1); level = epoch - 571 (INTERNATIONAL_LAW); otherwise 0.
export function getMilitaryLevelSlow(ctx, o, gs) {
  const T = ctx.types;
  const mil = (p) => p >= 0 && isEpoch(p) && T[p].cat === 0;
  let p = o.preq[0];
  if (!mil(p)) p = getPreq(ctx, o, 1, gs);
  return mil(p) ? p - 571 : 0;
}
// Types::finalize_grafting (@00669840), run from Game::init_data at every game start.
// opts.gameFlags8: (game->info.flags & 8); opts.gameSettings: {starting_technology, starting_technology2,
// ending_technology, game_rules} for get_preq.
export function finalizeGrafting(ctx, opts = {}) {
  const { gameFlags8 = false, gameSettings = {} } = opts;
  let barb = 0;
  for (let t = 0; t < NUM_TRIBES; t++) if (ctx.tribes[t].barbarian) barb |= bit(t);
  for (let u = 50; u < 402; u++) {
    const o = ctx.types[u], g = o.graft;
    if (g !== u && g >= 0 && (o.unit_flags & 0x1000000)) {
      const tg = ctx.types[g];
      if (!gameFlags8) tg.tribe_mask = (tg.tribe_mask & ~o.tribe_mask) | barb;
      else tg.tribe_mask |= o.tribe_mask;
    }
  }
  for (let u = 50; u < 402; u++) {
    const o = ctx.types[u];
    o.age = getAgeSlow(ctx, o, gameSettings);
    o.military_level = getMilitaryLevelSlow(ctx, o, gameSettings);
  }
  for (let b = 414; b < 543; b++) ctx.types[b].age = getAgeSlow(ctx, ctx.types[b], gameSettings);
}
// Load-time unit grafts (pass 2 tails over all units, then pass 3 over all units), then the game-start part.
export function applyUnitGrafts(ctx, lo = 50, hi = 414) {
  for (let u = lo; u < hi; u++) unitGraftPass2(ctx, u);
  for (let u = lo; u < hi; u++) unitGraftPass3(ctx, u);
}
export function applyGrafting(ctx, opts = {}) {
  if (!opts.skipLoadTime) applyUnitGrafts(ctx);
  if (!opts.loadTimeOnly) { initCivMasks(ctx); finalizeGrafting(ctx, opts); }
}
