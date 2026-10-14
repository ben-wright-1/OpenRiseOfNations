// Building types (TypeIndex 414..542): Data/buildingrules.xml <BUILDING>, building_graphics.xml mask keys and
// masks.txt footprints. Spec: build.md next to this file.
//
// Exports (call order inside Types::init @00669cc0):
//   initBuild(ctx, el, t, pass)   BuildType::init @00632340, pass 0 for all 129, then pass 1 for all 129
//   postBuildings(ctx)            Types::init post-loop: domain (BuildType::set_domain @00633390 inlined) and the
//                                 0x4000000 "upgrade chain" build flag
//   loadMasks(ctx, opts)          Types::init: "<GRAPH>-Default-Age0" graphic key -> BuildType::init_masks @00632280,
//                                 then masks.txt via Text::open/get -> init_build_mask @006310b0 /
//                                 init_doober_mask @00630cd0 (not part of the rules image)
// Later steps owned by other modules but defined here so the integrator can call them for buildings:
//   buildIs(ctx, o, x)            ObjectTypeData::is_slow @00661ae0 (strict = 0) for a building
//   initBuildIsList(ctx, t)       ObjectType::init_is_list @00661dd0 for a building
//   finalBuildFlags(ctx, t)       BuildType::init_final_flags @00632070 (0x10000000), run from
//                                 ObjectType::finalize_init_all @0065f4a0 / finalize_init @0065f640
//   buildAgeApprox(ctx, t)        Types::finalize_grafting @00669840 age (ObjectTypeData::get_age_slow @00661a00),
//                                 ignoring the game-setup dependent parts of TypeData::get_preq @00668700
import fs from 'node:fs';
import path from 'node:path';

export const BASE_BUILD = 414, END_BUILD = 543;
export const FARM = 417, OILWELL = 421, OILPLATFORM = 422, VILLAGE = 414;

// Build flag bits (build_flags @0x2c0). Letters a..z from BUILD_FLAGS are bits 0..25 (designer comments);
// the high bits are derived by the loader.
export const BF = {
  LAND: 1 << 0, SEA: 1 << 1, GLOBAL_UPGRADE: 1 << 2, OUTSIDE_CITY: 1 << 4, NON_FRIENDLY: 1 << 5, GATHERER: 1 << 6,
  ON_ROADS: 1 << 7, ADDON_FREE: 1 << 8, ONE_PER_CITY: 1 << 9, ON_WALL: 1 << 10, LARGE_CITY_ONLY: 1 << 11,
  NUBBED: 1 << 12, NOT_CAPTURED: 1 << 13,
  UPGRADE_CHAIN: 0x4000000,   // Types::init post-loop: building has a FROM, or is some building's FROM
  RESEARCHES: 0x8000000,      // TechType::set_research: some tech's WHERE is this building (via is())
  FARM_OR_OIL: 0x10000000,    // init_final_flags: is(FARM) || is(OILWELL) || is(OILPLATFORM)
  CASTS: 0x20000000,          // SpellType::init: building is a craft/spell's FROM or FROM2
  TRAINS_MILITARY: 0x40000000, // UnitType::init pass 2: trains a unit with attack != 0 (see build.md)
  TRAINS: 0x80000000,         // UnitType::init pass 2: some unit's WHERE is this building
};

const i8 = (v) => (v << 24) >> 24;
const i16 = (v) => (v << 16) >> 16;

// OBJ_MASKS (BuildType::init): upper-cased, every character sets a bit: '1'..'9' -> c - 0x17 (26..34),
// anything else -> c - 'A'; the shift count is taken mod 32. Unlike rulesxml.objMask, non-letters are NOT
// ignored (a space would set bit 31). BUILD_FLAGS is the same with lower case and 'a' (== rulesxml.unitFlags).
export function buildObjMask(X, s) {
  let m = 0;
  for (const c of X.toUpper(s)) {
    const k = c.charCodeAt(0);
    m |= 1 << ((k >= 0x31 && k <= 0x39 ? k - 0x17 : k - 0x41) & 31);
  }
  return m >>> 0;
}
export function buildFlagMask(X, s) {
  let m = 0;
  for (const c of X.toLower(s)) {
    const k = c.charCodeAt(0);
    m |= 1 << ((k >= 0x31 && k <= 0x39 ? k - 0x17 : k - 0x61) & 31);
  }
  return m >>> 0;
}

// get_text + "empty -> -1 else String::convert_int" (SUPPORTVALUEn, PLUNDER, BEHIND_HEIGHT). Same result as
// get_text_num(name, -1).
const numOr = (X, el, name, def = -1) => X.getTextNum(el, name, def);

// Type::load_cost @00663ba0 on the COST child (created empty if missing). Text, else the "cost" attribute; empty
// leaves costs untouched, otherwise they are zeroed and each "<n><letter>" token (split on '/') assigns
// costs[res] = n (later tokens overwrite).
export function loadCost(X, costEl, costs) {
  let s = X.text(costEl);
  if (!s.length) s = X.getAttrib(costEl, 'cost', '');
  if (!s.length) return;
  costs.fill(0);
  for (const { amount, res } of X.costTokens(s)) costs[res] = amount;
}

// BuildTypeData::basic_type @00639970: follow FROM to the root.
function basicType(ctx, t) {
  for (let guard = 0; guard < 1000; guard++) {
    const f = ctx.types[t].from;
    if (f < 0) return t;
    t = f;
  }
  throw new Error(`FROM cycle at building ${t}`);
}

// ---------------------------------------------------------------- BuildType::init @00632340
export function initBuild(ctx, el, t, pass) {
  const X = ctx.X, o = ctx.types[t];
  if (pass === 0) { o.abbrev = ''; o.desc = ''; } // BuildType::close @00632060 (Type::close would also run for plain Type)
  o.type = t;
  if (pass === 0) {
    const name = X.getText(el, 'NAME');
    o.name = name; o.type_name = name;                                  // Type::set_name @00663ae0
    o.display_name = ctx.lookupDisplayName ? ctx.lookupDisplayName(o, name) : name;
    o.graph_name = X.getText(el, 'GRAPH');
    o.help_name = X.getText(el, 'GRAPH');
    const tn = X.getText(el, 'TYPENAME');
    o.type_name = tn.length ? tn : o.name;
    o.upgrade = -1;
    o.to = -1;
    return 0;
  }
  // pass != 0 (Types::init only ever calls pass 1 for buildings)
  o.attack = 0; o.armor = 2;                                           // overwritten below
  o.cat = ctx.catKey('build_cats', X.getText(el, 'CAT'));               // Categories::find_key, -1 if unknown
  o.grid_x = i8(X.getTextNum(el, 'GRID_X'));
  o.grid_y = i8(X.getTextNum(el, 'GRID_Y'));
  // Interface-only (TypeOut, not in the rules image): icon page lookup by name in the render manager's texture
  // list (0 if missing) for TEX_ID and TEX_ID + "_HALF"; the rest are raw numbers stored into slot [0][0].
  o.tex_id_name = X.getText(el, 'TEX_ID');
  o.tex_col = i8(X.getTextNum(el, 'TEX_COL'));
  o.tex_row = i8(X.getTextNum(el, 'TEX_ROW'));
  o.tex_x = i16(X.getTextNum(el, 'TEX_X'));
  o.tex_y = i16(X.getTextNum(el, 'TEX_Y'));
  o.tex_clip = i8(X.getTextNum(el, 'TEX_CLIP'));
  o.hits = X.getTextNum(el, 'HITS');
  o.town_hits = X.getTextNum(el, 'TOWN_HITS');
  o.min_city_size = 0;
  o.misery_rate = X.getTextNum(el, 'MISERY');
  o.to_hit = X.getTextNum(el, 'TO_HIT');
  o.attenuate = X.getTextNum(el, 'ATTENUATE');
  o.attack = X.i32(Math.imul(X.getTextNum(el, 'ATTACK'), 10));
  // RANGE "min-max": min = convert_int(whole string); max = convert_int(text after the first '-'), or min if
  // there is no '-'.
  const range = X.getText(el, 'RANGE');
  o.min_range = X.convertInt(range);
  const dash = range.indexOf('-');
  o.max_range = dash < 0 ? o.min_range : X.convertInt(range.slice(dash + 1));
  o.splash_area = X.getTextNum(el, 'SPLASH_AREA');
  o.splash_percent = X.getTextNum(el, 'SPLASH_PERCENT');
  o.ammo_per_att = X.getTextNum(el, 'AMMO_PER_ATT');
  o.proj_speed = X.getTextNum(el, 'PROJ_SPEED');
  o.most_shots = X.getTextNum(el, 'MOST_SHOTS');
  o.garrison_max = X.getTextNum(el, 'GARRISON_MAX');
  o.base_arrows = X.getTextNum(el, 'BASE_ARROWS');
  o.wonder_val = X.getTextNum(el, 'WONDER_VAL');
  o.recharge = X.getTextNum(el, 'RECHARGE');
  o.armor = X.getTextNum(el, 'ARMOR');
  o.fly_high = X.getTextNum(el, 'FLY_HIGH');
  o.fly_low = X.getTextNum(el, 'FLY_LOW');
  o.obj_masks = buildObjMask(X, X.getText(el, 'OBJ_MASKS')) | 0;
  o.x_size = X.getTextNum(el, 'X_SIZE');
  o.y_size = X.getTextNum(el, 'Y_SIZE');
  o.los = X.getTextNum(el, 'LOS');
  o.science_los = X.getTextNum(el, 'SCIENCE_LOS');
  o.build_flags = buildFlagMask(X, X.getText(el, 'BUILD_FLAGS'));
  o.job_time = o.res_time = X.getTextNum(el, 'JOB_TIME') >>> 0;
  o.preq = [0, 1, 2].map((i) => ctx.techKey(X.getText(el, 'PREQ' + i)));
  o.from = ctx.buildKey(X.getText(el, 'FROM'));
  // The predecessor learns its successor unless this building has OBJ_MASKS '1' ("can't upgrade to this type").
  if ((o.obj_masks & 0x4000000) === 0 && o.from >= 0) {
    const p = ctx.types[o.from];
    p.to = t;
    if (o.build_flags & BF.GLOBAL_UPGRADE) p.upgrade = t;
  }
  for (let i = 0; i < 2; i++) {
    o.support[i] = ctx.goodKey(X.getText(el, 'SUPPORT' + i));
    o.support_cost[i] = numOr(X, el, 'SUPPORTVALUE' + i);
  }
  loadCost(X, X.childOrCreate(el, 'COST'), o.costs);
  o.plunder_value = numOr(X, el, 'PLUNDER');
  o.plunder_good = ctx.goodKey(X.getText(el, 'PLUNDER_GOOD'));
  o.behind_height = numOr(X, el, 'BEHIND_HEIGHT');
  o.tribe_mask = X.binary(X.getText(el, 'TRIBE_MASK'));
  o.where = o.from < 0 || basicType(ctx, t) === t ? -1 : o.from;
  o.obs = ctx.techKey(X.getText(el, 'OBSOLETE'));
  o.jump = ctx.buildKey(X.getText(el, 'JUMP'));
  if (o.jump >= 0) o.upgrade = o.jump;
  return 0;
}

// ---------------------------------------------------------------- Types::init post-loop (after pass 1)
export function postBuildings(ctx) {
  for (let t = BASE_BUILD; t < END_BUILD; t++) {
    const o = ctx.types[t];
    o.domain = o.build_flags & BF.SEA ? (o.build_flags & BF.LAND) + 1 : 0; // 0 land, 1 sea only, 2 land+sea
    if (o.from >= 0) {
      const p = ctx.types[o.from];
      p.build_flags = (p.build_flags | BF.UPGRADE_CHAIN) >>> 0;
      o.build_flags = (o.build_flags | BF.UPGRADE_CHAIN) >>> 0;
    }
  }
}

// ---------------------------------------------------------------- masks (Types::init, after the post-loop)
// Text::open / Text::get / Text::item (text.obj) over masks.txt, as used by init_build_mask / init_doober_mask.
export class MaskText {
  constructor(src, X) {
    this.X = X;
    // fgetws in text mode: CRLF -> LF; each line loses everything from its last '\n'.
    this.lines = src.replace(/\r\n/g, '\n').split('\n');
    this.pos = 0; this.itemMark = ''; this.itemBuffer = '';
  }
  // Text::open(file, key): rescan from the top for a line equal (case-insensitive, trailing spaces trimmed)
  // to "#key". Returns true when found (the game returns 0). An empty key opens at the top of the file.
  open(key) {
    this.pos = 0;
    if (!key.length) return true;
    const want = '#' + key;
    while (this.pos < this.lines.length) {
      const l = this.lines[this.pos++].replace(/ +$/, '');
      if (l.length === want.length && l.toLowerCase() === want.toLowerCase()) { this.itemMark = l; return true; }
    }
    return false;
  }
  // Text::get(0): next line that is not blank and not a ';' comment (after leading spaces). A line starting
  // with '#' (the next section) or EOF yields "" without touching the item cursor.
  get() {
    while (this.pos < this.lines.length) {
      let l = this.lines[this.pos++];
      if (l[0] === '#') return '';
      l = l.replace(/^ +/, '');
      if (l[0] === ';') continue;
      l = l.replace(/ +$/, '');
      if (l.length) { this.itemMark = l; return l; }
    }
    return '';
  }
  // Text::item: up to the next ',' (or end); trimmed of spaces; an empty field leaves the previous item.
  item() {
    const k = this.itemMark.indexOf(',');
    const n = k < 0 ? this.itemMark.length : k;
    if (n) this.itemBuffer = this.itemMark.slice(0, n).replace(/^ +| +$/g, '');
    this.itemMark = this.itemMark.slice(k < 0 ? n : n + 1);
    return this.itemBuffer;
  }
}

// opts: { graphicsXml (parsed building_graphics.xml root), masksSrc (masks.txt text), dooberNames (array of
// GraphicPieces doober names, optional), onError(msg) }.
export function loadMasks(ctx, opts = {}) {
  const X = ctx.X;
  const root = opts.graphicsXml ?? X.readXml(ctx.dataDir, 'building_graphics.xml');
  const masksSrc = opts.masksSrc ?? fs.readFileSync(path.join(ctx.installDir ?? path.dirname(ctx.dataDir), 'masks.txt'), 'latin1');
  const err = opts.onError ?? ((m) => { throw new Error(m); });
  // GraphicPieces::build_node_from_gpiece(String) @00905050: ROOT/BUILDINGS/BUILD elements by name attribute,
  // key upper-cased, first match.
  const builds = X.children(X.child(root, 'Buildings'), 'build');
  for (let t = BASE_BUILD; t < END_BUILD; t++) {
    const o = ctx.types[t];
    const key = X.toUpper(o.graph_name + '-Default-Age0');
    const node = builds.find((b) => X.iequals(X.getAttrib(b, 'name', ''), key));
    if (!node) { err(`Failed to locate basic building graphic key: ${o.graph_name}`); continue; }
    o.mask_name = X.getAttrib(node, 'mask', '');       // BuildType::init_masks
    o.doober_name = X.getAttrib(node, 'doobermask', '');
  }
  const txt = new MaskText(masksSrc, X);
  for (let t = BASE_BUILD; t < END_BUILD; t++) {
    const o = ctx.types[t];
    if (txt.open(o.mask_name)) readBuildMask(txt, o, err);
    if (txt.open(o.doober_name)) readDooberMask(txt, o, err, opts.dooberNames);
  }
}

function readHeader(txt, o, err, what) {
  txt.get();
  const w = txt.convert(txt.item()), h = txt.convert(txt.item());
  if (h !== o.y_size || w !== o.x_size) err(`${what} size ${w}x${h} != ${o.x_size}x${o.y_size} for ${o.name}`);
  return [w, h];
}
MaskText.prototype.convert = function (s) { return this.X.convertInt(s); };

// BuildType::init_build_mask @006310b0: "W, H" then H rows of W integers; mask[row * W + col] = (u8) value.
// Only value 1 blocks (BuildType::mask_me @006312a0).
function readBuildMask(txt, o, err) {
  const [w, h] = readHeader(txt, o, err, 'mask');
  const data = new Uint8Array(Math.max(0, w * h));
  for (let r = 0; r < h; r++) {
    txt.get();
    for (let c = 0; c < w; c++) data[r * w + c] = txt.convert(txt.item()) & 0xff;
  }
  o.mask = { w, h, data };
}

// BuildType::init_doober_mask @00630cd0: same grid of words. "0" -> -1; up/down -> BuildNSCorner (dir 1/2),
// left/right -> BuildEWCorner (dir 1/2), random -> BuildDeathFiller (dir 0, flag 0); value =
// dir << 16 | flag << 8 | index in doober_names (flag = 1 for the corner pieces); -1 if the name is unknown.
const DOOBER = { up: ['BuildNSCorner', 1, 1], down: ['BuildNSCorner', 2, 1], left: ['BuildEWCorner', 1, 1],
  right: ['BuildEWCorner', 2, 1], random: ['BuildDeathFiller', 0, 0] };
function readDooberMask(txt, o, err, names) {
  const [w, h] = readHeader(txt, o, err, 'doober mask');
  const data = new Int32Array(Math.max(0, w * h)).fill(-1), words = [];
  for (let r = 0; r < h; r++) {
    txt.get();
    for (let c = 0; c < w; c++) {
      const s = txt.item(); words.push(s);
      if (s === '0') continue;
      const d = DOOBER[s.toLowerCase()];   // String == is case-insensitive
      if (!d || !names) continue;
      const idx = names.findIndex((n) => n.toLowerCase() === d[0].toLowerCase());
      if (idx >= 0) data[r * w + c] = (d[1] << 16) | (d[2] << 8) | idx;
    }
  }
  o.doober_mask = { w, h, data, words };
}

// ---------------------------------------------------------------- finalize-time helpers
// ObjectTypeData::is_slow(x, 0) for a building: itself, its graft, or (recursively) its FROM.
export function buildIs(ctx, o, x) {
  for (let guard = 0; o && guard < 1000; guard++) {
    if (o.type === x) return true;
    if (x < 0) return false;
    if (o.graft === x) return true;
    if (o.from < 0) return false;
    o = ctx.types[o.from];
  }
  return false;
}
// ObjectType::init_is_list for a building: is_list = every TypeIndex x (ascending) with is_slow(x, 0);
// is_strict_list = [type] (is_slow(x, 1) is false for non-units except x == type). SimpleArray grows one
// element at a time (block 1) so capacity == count.
export function initBuildIsList(ctx, t) {
  const o = ctx.types[t], v = [];
  for (let x = 0; x < 806; x++) if (buildIs(ctx, o, x)) v.push(x);
  const sa = (a) => (a.length ? { count: a.length, capacity: a.length, block: 1, flags: 0, v: a } : { count: 0 });
  o.is_list = sa(v); o.is_strict_list = sa([t]);
}
// BuildType::init_final_flags @00632070 (also inlined in ObjectType::finalize_init_all / finalize_init).
export function finalBuildFlags(ctx, t) {
  const o = ctx.types[t];
  if (buildIs(ctx, o, FARM) || buildIs(ctx, o, OILWELL) || buildIs(ctx, o, OILPLATFORM))
    o.build_flags = (o.build_flags | BF.FARM_OR_OIL) >>> 0;
}
// Types::finalize_grafting age for a building: first preq (0..2) that is a tech -> tech.age, +1 if it is an
// age tech (544..550). Approximation: the real TypeData::get_preq drops/adjusts PREQ1/PREQ2 depending on the
// game's start/end age and the leader (Library "color techs"); fine for the default setup.
export function buildAgeApprox(ctx, t) {
  const o = ctx.types[t];
  for (let i = 0; i < 3; i++) {
    const p = o.preq[i];
    if (p >= 544 && p < 629) return ctx.types[p].age + (p <= 550 ? 1 : 0);
  }
  return 0;
}
