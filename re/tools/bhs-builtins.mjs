// Build the BigHugeScript builtin-function table from the decompiled registration code, plus evidence-based
// categories and usage counts over the shipped scripts. Nothing game-derived is stored in this file: every input is
// read at run time (the gitignored decompiles under re/decomp/, re/map/functions.tsv, the optional callee closure
// from re/ghidra_scripts/CalleeClosure.java, and the user's own install). See docs/script-spec.md (builtins).
//
// Usage:
//   node re/tools/bhs-builtins.mjs [--decomp re/decomp] [--game ~/Games/RiseOfNations] [--closure re/analysis/bhs_builtin_callees.tsv]
//                                  [--out re/analysis/bhs_builtins.tsv] [--quiet]
//
// Inputs:
//   <decomp>/script/{Math,Trigger,String,Array}UtilFuncSet_init_funcs@*.c, <decomp>/script/ScenarioFuncSet_init_funcs@*.c
//       ScriptFuncSet::add_new_func(set, RETURN_TYPE, L"name", IMPL, NUM_NATIVE_ARGS) followed by
//       ScriptFunc::add_param(func, TYPE, L"name", VALIDATION_OR_NULL, FLAGS, junk) per script parameter.
//   <decomp>/script_funcs/*.c, <decomp>/script/*Util*.c   builtin bodies (multiplayer-guard and RNG evidence)
//   re/map/functions.tsv                                   impl address -> Class::method
//   closure TSV (optional)                                 per-builtin callee/global closure (CalleeClosure.java, depth 4):
//       re/tools/ghidra-run.sh CalleeClosure.java re/jobs/dc_bhsfn_closure.txt   (job: out file, depth, impl addresses)
//   <game>/**/*.bhs                                        usage counts; <game>/scenario/scriptlibrary/game_structs.bhs for
//                                                          struct type ids
//
// Registration order is the global function index (ScriptGameInterface::init @9e1a20 creates the sets in the order
// MathUtil, TriggerUtil, StringUtil, ArrayUtil, Scenario; ScriptFuncSet::add_new_func @9d4da0 appends to
// ScriptGameInterfaceBase::functions). That index is the operand of OP_CALL_GAME / OP_CALL_GAME_VARIED.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '../..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 ? argv[i + 1] : d; };
const DECOMP = opt('decomp', path.join(repo, 're/decomp'));
const GAME = opt('game', path.join(os.homedir(), 'Games/RiseOfNations'));
const CLOSURE = opt('closure', path.join(repo, 're/analysis/bhs_builtin_callees.tsv'));
const OUT = opt('out', path.join(repo, 're/analysis/bhs_builtins.tsv'));
const FUNCS = opt('functions', path.join(repo, 're/map/functions.tsv'));
const quiet = argv.includes('--quiet');

// ---------------------------------------------------------------------------------------------------------------
// Type ids: String::generate_hash (insensitive variant) of the type's unique token. Simple types use their name,
// arrays "@" + element token (ArrayType::get_unique_token @9da620), structs name + "$" + member tokens each followed
// by "^" (StructType::get_unique_token @9db280).
const PRIMES = [127, 811, 1597, 2131, 2749, 4759, 5527, 5953, 8117, 9539, 10273, 10753, 11159, 12301, 13217, 14207,
  15413, 17681, 18661, 19013, 21089, 22051, 25111, 25801, 27457, 28057, 29581, 30809, 32611, 34469, 36067, 37511,
  38723, 40093, 41983, 43321, 45083, 47431, 49667, 50767, 53453, 55469, 57193, 59369, 61987, 65071, 73421, 77849,
  84223, 89009]; // basic:str.obj primes[50], String::generate_hash @a1b6b0 (same as locstrings.mjs)
function hashI(s) {
  let hi = 0, n = s.length;
  for (let u = n - 1; u >= 0; u--) {
    const l = s[u].toLowerCase().charCodeAt(0);
    hi = (hi + PRIMES[l % 50] * n + l * PRIMES[u % 50]) >>> 0;
    n--;
  }
  return hi;
}
const typeNames = new Map();
const addType = (token, name) => typeNames.set(hashI(token), name);
for (const t of ['int', 'float', 'string', 'void', 'bool']) { addType(t, t); addType('@' + t, t + '[]'); }
addType('array', 'array');     // any array (ScriptFunc::accepts_params: matches every array argument)
addType('generic', 'generic'); // any value (accepts_params / get_param skip the type check)
typeNames.set(0x64ea2a, '...'); // variadic tail; token text not recovered (ScriptFunc::add_param sets ScriptParam.flags |= 1)

function loadStructTypes() {
  const f = findFile(path.join(GAME, 'scenario/scriptlibrary'), 'game_structs.bhs');
  if (!f) return;
  const src = stripComments(fs.readFileSync(f, 'latin1'));
  const tok = new Map([['int', 'int'], ['float', 'float'], ['string', 'string'], ['bool', 'bool']]);
  for (const m of src.matchAll(/struct\s+(\w+)\s*\{([^}]*)\}/g)) {
    let token = m[1] + '$';
    for (const mem of m[2].split(';')) {
      const mm = /^\s*(\w+)\s*(\[\])?\s+\w+\s*$/.exec(mem);
      if (!mm) continue;
      const base = tok.get(mm[1]) ?? mm[1];
      token += (mm[2] ? '@' + base : base) + '^';
    }
    tok.set(m[1], token);
    addType(token, m[1]);
    addType('@' + token, m[1] + '[]');
  }
}
const typeName = id => typeNames.get(id) ?? `0x${id.toString(16)}`;

// ---------------------------------------------------------------------------------------------------------------
function findFile(dir, name) {
  if (!fs.existsSync(dir)) return null;
  const hit = fs.readdirSync(dir).find(f => f.toLowerCase() === name.toLowerCase());
  return hit ? path.join(dir, hit) : null;
}
function walk(dir, out = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out); else out.push(p);
  }
  return out;
}

// Remove comments, string literals and char literals (BigHugeScript lexer rules: //, /* */, "..." with \ escapes, 'c').
function stripComments(src, keepStrings = true) {
  let out = '', i = 0;
  while (i < src.length) {
    const c = src[i], d = src[i + 1];
    if (c === '/' && d === '/') { while (i < src.length && src[i] !== '\n') i++; continue; }
    if (c === '/' && d === '*') { const e = src.indexOf('*/', i + 2); i = e < 0 ? src.length : e + 2; out += ' '; continue; }
    if (c === '"') {
      let j = i + 1;
      while (j < src.length && src[j] !== '"' && src[j] !== '\n') j += src[j] === '\\' ? 2 : 1;
      out += keepStrings ? src.slice(i, j + 1) : '""';
      i = j + 1; continue;
    }
    if (c === "'" && /^'(\\.|[^'\\\n])'/.test(src.slice(i, i + 4))) {
      const len = src[i + 1] === '\\' ? 4 : 3;
      out += keepStrings ? src.slice(i, i + len) : "' '"; i += len; continue;
    }
    out += c; i++;
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// 1. Registrations
const SETS = [ // ScriptGameInterface::init order
  ['MathUtil', 'MathUtilFuncSet'], ['TriggerUtil', 'TriggerUtilFuncSet'], ['StringUtil', 'StringUtilFuncSet'],
  ['ArrayUtil', 'ArrayUtilFuncSet'], ['ScenarioFuncSet', 'ScenarioFuncSet'],
];
const NUM = '(0x[0-9a-fA-F]+|-?\\d+)';
const reFunc = new RegExp(`ScriptFuncSet::add_new_func\\s*\\(\\s*\\(ScriptFuncSet \\*\\)this\\s*,\\s*${NUM}\\s*,\\s*L"((?:[^"\\\\]|\\\\.)*)"\\s*,\\s*${NUM}\\s*,\\s*${NUM}\\s*\\)`, 'y');
const reParam = new RegExp(`ScriptFunc::add_param\\s*\\(\\s*\\w+\\s*,\\s*${NUM}\\s*,\\s*L"((?:[^"\\\\]|\\\\.)*)"\\s*,\\s*(\\(wchar_t \\*\\)0x0|L"(?:[^"\\\\]|\\\\.)*")\\s*,\\s*${NUM}\\s*,`, 'y');
const num = s => s.startsWith('0x') ? parseInt(s, 16) : parseInt(s, 10);

function parseInit(file, set) {
  const s = fs.readFileSync(file, 'utf8').replace(/\s+/g, ' ');
  const funcs = [];
  for (let i = 0; i < s.length; i++) {
    if (s[i] !== 'S') continue;
    reFunc.lastIndex = i;
    let m = reFunc.exec(s);
    if (m) {
      funcs.push({ set, ret: num(m[1]), name: m[2], addr: num(m[3]), nargs: num(m[4]), params: [] });
      i = reFunc.lastIndex - 1; continue;
    }
    reParam.lastIndex = i;
    m = reParam.exec(s);
    if (m) {
      if (!funcs.length) throw new Error(`${file}: add_param before add_new_func`);
      const v = m[3].startsWith('L"') ? m[3].slice(2, -1) : '';
      funcs.at(-1).params.push({ type: num(m[1]), name: m[2], validation: v, flags: num(m[4]) });
      i = reParam.lastIndex - 1;
    }
  }
  const expect = (s.match(/add_new_func/g) || []).length, expectP = (s.match(/add_param/g) || []).length;
  const gotP = funcs.reduce((a, f) => a + f.params.length, 0);
  if (expect !== funcs.length || expectP !== gotP) throw new Error(`${file}: parsed ${funcs.length}/${expect} funcs, ${gotP}/${expectP} params`);
  return funcs;
}

loadStructTypes();
const scriptDir = path.join(DECOMP, 'script');
const builtins = [];
for (const [set, cls] of SETS) {
  const f = fs.readdirSync(scriptDir).find(n => n.startsWith(`${cls}_init_funcs@`));
  if (!f) throw new Error(`missing ${cls}_init_funcs in ${scriptDir}`);
  const list = parseInit(path.join(scriptDir, f), set);
  list.forEach((b, i) => { b.setIndex = i; b.index = builtins.length; builtins.push(b); });
}

// Overloads: ScriptFunc::check_overload @9d46a0 links an earlier same-name function (searching backwards over the
// global table, case-sensitive String ==) to the later one via next_overload; identical parameter type lists are an error.
const byName = new Map();
for (const b of builtins) { if (!byName.has(b.name)) byName.set(b.name, []); byName.get(b.name).push(b); }

// impl names
const implName = new Map();
if (fs.existsSync(FUNCS)) {
  for (const line of fs.readFileSync(FUNCS, 'utf8').split('\n').slice(1)) {
    const [rva, , cls, method] = line.split('\t');
    if (rva) implName.set(parseInt(rva, 16), cls ? `${cls}::${method}` : method);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// 2. Evidence: bodies and callee closure
const bodies = new Map(); // addr -> source
for (const dir of ['script_funcs', 'script']) {
  const d = path.join(DECOMP, dir);
  if (!fs.existsSync(d)) continue;
  for (const f of fs.readdirSync(d)) {
    const m = /@([0-9a-f]{8})\.c$/.exec(f);
    if (m) bodies.set(parseInt(m[1], 16), fs.readFileSync(path.join(d, f), 'utf8'));
  }
}
const closure = new Map(); // addr -> { F: Map(name -> depth), D: Map(name -> depth) }
if (fs.existsSync(CLOSURE)) {
  for (const line of fs.readFileSync(CLOSURE, 'utf8').split('\n')) {
    const [root, kind, depth, name] = line.split('\t');
    if (!name || (kind !== 'F' && kind !== 'D')) continue;
    const a = parseInt(root, 16);
    if (!closure.has(a)) closure.set(a, { F: new Map(), D: new Map() });
    const m = closure.get(a)[kind];
    if (!m.has(name) || m.get(name) > +depth) m.set(name, +depth);
  }
}
const minDepth = (map, re) => { let d = Infinity; for (const [n, v] of map) if (re.test(n) && v < d) d = v; return d; };

// Local-machine state (never replicated in lockstep). Globals named by the PDB; see docs/frame-spec.md §3 for the
// Game.semaphore byte0 & 4 multiplayer bit.
const LOCAL_GLOBALS = /^(MiscAccess::console|console|MiscAccess::camera|camera|BaseCamera::active_camera|MiscAccess::iface_current|MiscAccess::iface_main|IFaceMain::vftable|IFaceDiploBase::current|MiscAccess::scene|scene|graphsys|player_profile|prefs_key|prefs_file|BasePop::current_popup|popup|juke_box|soundsys|Window::.*|ScenarioData::(sound|objective_dlg_viewed|user_warnings|mouse_selection_disabled|hotkey_selection_disabled))$/;
const MP_GUARD = /semaphore\)\.ptr\[0\] & 4/;

// ---------------------------------------------------------------------------------------------------------------
// 3. Categories. Method (also in docs/script-spec.md): the utility sets are script environment; within ScenarioFuncSet
// a name rule assigns a provisional class, then evidence overrides it: a CommandManager::issue_* / UnitOut::issue_*
// call within depth 2 makes it a lockstep command; names on the presentation list stay presentation even when the
// body also touches sim state for the console player only. Everything not matched is a direct mutation.
const CAT = { a: 'query', b: 'command', c: 'mutation', d: 'presentation', e: 'script-env' };
const PRESENTATION = new RegExp('^(' + [
  'add_objective', 'add_objective_text', 'change_objective_text', 'set_objective_sound', 'set_objective_loc',
  'play_objective_sound', 'objective_complete', 'remove_objective', 'clear_objectives', 'add_goal', 'complete_goal',
  'reset_goal', 'remove_goal', 'clear_goals', 'goal_dialog_shown', 'show_goal_dialog', 'set_instruction_msg',
  'set_feedback_msg', 'use_basic_menu', 'use_advanced_menu', 'set_music_volume', 'play_sound', 'play_sound_to',
  'stop_sound', 'sound_finished', 'set_music_mood', 'chat', 'chat_all', 'ping\\w*', 'clear_pings',
  'get_player_profile_name', 'object_selected', 'resource_selected', 'object_type_selected', 'any_object_selected',
  'num_objects_selected', 'select_object', 'select_group', 'find_selected_id', 'clear_selection',
  'mouse_selection_\\w+', 'hotkey_selection_\\w+', 'get_object_at_mouse', 'get_object_type_at_mouse',
  'is_mouseover_\\w+', 'get_resource_at_mouse', '\\w+_warning', 'mouse_down', 'get_mouse_\\w+', 'is_game_mouse_near',
  'key_down', 'key_released', 'build_\\w*menu_active', 'build_option_selected', 'is_zoomed_\\w+', 'is_on_screen',
  'is_\\w+_selected', 'get_button_\\w+', 'get_objectives_\\w+', 'get_screen_\\w+', 'scrolling_\\w+', 'zooming_\\w+',
  'zoom_\\w+', 'move_camera', 'jump_camera', 'center_camera_on', 'is_camera_panning', 'get_current_zoom',
  'get_camera_\\w', 'take_screenshot', 'print_msg', 'set_msg_\\w+', 'clear_msg', 'print_game_msg', 'clear_game_msg',
  'set_game_msg_\\w+', 'popup_dialog', 'popup_choice', 'highlight_\\w+', 'clear_highlight', 'ui_\\w+', 'bubble_text\\w*',
  'is_right_click_action', 'draw_screen_arrow_line', 'cancel_arrow', 'clear_all_arrows', 'start_screen_arrow',
  'start_ping_arrow', 'add_arrow_point', 'draw_arrow_curve', 'set_arrow_\\w+', 'ctw_\\w*objective\\w*',
  'ctw_center_camera', 'ctw_custom_end_screen', 'show_notice', 'add_flag', 'clear_flags', 'real_time_millis',
  'set_victory_message', 'set_defeat_message', 'set_unit_scale', 'get_unit_scale', 'ctw_show_supply_depots',
  'ctw_hide_supply_depots', 'speed_control_\\w+', 'pause_enable', 'pause_disable',
].join('|') + ')$');
const SCRIPT_ENV = /^(set_timer|stop_timer|timer_expired|open_file|close_file|file_\w+|get_curr_filename)$/;
const QUERY = /^(get_\w+|is_\w+|num_\w+|find_\w+|at_least_\w+|less_than_\w+|have_\w+|has_\w+|was_\w+|can_\w+|ctw_is_\w+|ctw_get_\w+|ctw_num_\w+|ctw_territory_has_army|ctw_territory_captured|ctw_deal_accepted|ctw_game_over|ctw_last_battle_victory|ctw_reinforcements_arrived|ctw_territory_list|ctw_colony_list|ctw_bonus_card_list|map_is_\w+|world_[xy]_size|territory_owner|territory_object|time\w*|population|population_cap|age|score|gather_rate|object_(visible|near|near_build|type|type_at|type_near|type_near_build|position_[xy]|health|max_health)|any_object_\w+|is_object_at|unit_category|unit_craft|unit_max_craft|craft_percent|home_base|max_\w+|involved_\w+|city_(captured|id_captured|captured_by|assimilated_by|being_assimilated|alarm_sounded|exists)|building_(destroyed|type_destroyed|active|started|full)|unit_(killed|type_killed|any_killed)|unit_group_near\w*|garrisoned_in|general_is_\w+|offer_(made|accepted|rejected)|amount_offered|diplomacy_open|contact_open|group_id|type_build_time|build_queue_percent_done|resource_position_[xy]|get_spell_name|have_contact|researching_tech\w*|amount\w*|ctw_get_master|ctw_territory_link|ctw_territory_sea_link)$/;

// Checked by reading the body: the name rule gets these wrong.
const OVERRIDE = {
  find_extra_starting_loc: ['c', 'appends to ScenarioData::extra_starting_locs'],
  ctw_get_active_tribes: ['a', 'fills a string[] out parameter'],
  pause_game: ['d', 'TurnControl pause bit; no-op in multiplayer'],
  unpause_game: ['d', 'TurnControl pause bit; no-op in multiplayer'],
};

function categorize(b) {
  if (b.set !== 'ScenarioFuncSet') return { cat: 'e', why: ['utility set'] };
  if (OVERRIDE[b.name]) return { cat: OVERRIDE[b.name][0], why: ['override: ' + OVERRIDE[b.name][1]] };
  // UnitGroup-struct overloads that only edit the script value passed by reference.
  if (/^(add_to_group|remove_from_group|clear_group)$/.test(b.name) && b.params[0]?.flags & 4) return { cat: 'e', why: ['edits the script UnitGroup (ref)'] };
  const cl = closure.get(b.addr);
  if (cl) {
    const dIssue = minDepth(cl.F, /^(CommandManager::issue_(?!check_sums)|UnitOut::issue_)/);
    if (dIssue <= 2) return { cat: 'b', why: [`issue_* at depth ${dIssue}`] };
  }
  if (PRESENTATION.test(b.name)) return { cat: 'd', why: ['name list'] };
  if (SCRIPT_ENV.test(b.name)) return { cat: 'e', why: ['name list'] };
  if (QUERY.test(b.name) && b.ret !== 0x84048) return { cat: 'a', why: ['name rule'] };
  return { cat: 'c', why: ['default'] };
}

// ---------------------------------------------------------------------------------------------------------------
// 4. Usage over the shipped scripts
const DECL_KW = /\b(int|float|string|void|bool|trigger|ai|scenario|conquest|struct)\s*$/;
const usage = new Map(builtins.map(b => [b.name, { calls: 0, files: new Set(), ai: 0, ctw: 0, scen: 0, aiF: new Set(), ctwF: new Set(), scenF: new Set() }]));
let scripts = [];
if (fs.existsSync(GAME)) scripts = walk(GAME).filter(f => f.toLowerCase().endsWith('.bhs'));
const nameRe = new RegExp(`\\b(${[...byName.keys()].sort((a, b) => b.length - a.length).join('|')})\\s*\\(`, 'g');
const classOf = rel => { const top = rel.split(path.sep)[0].toLowerCase(); return top === 'ai' ? 'ai' : top === 'conquest' ? 'ctw' : 'scen'; };
for (const f of scripts) {
  const rel = path.relative(GAME, f), cls = classOf(rel);
  const src = stripComments(fs.readFileSync(f, 'latin1'), false);
  for (const m of src.matchAll(nameRe)) {
    if (DECL_KW.test(src.slice(Math.max(0, m.index - 24), m.index))) continue; // a script function definition/prototype
    const u = usage.get(m[1]);
    u.calls++; u.files.add(rel); u[cls]++; u[cls + 'F'].add(rel);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// 5. Rows
const fmtParam = p => {
  let s = `${typeName(p.type)} ${p.name}`;
  if (p.validation) s += `=<${p.validation}>`;
  const fl = [];
  if (p.flags & 4) fl.push('ref');
  if (p.flags & 2) fl.push('byval');
  if (p.flags & ~6) fl.push(`0x${(p.flags & ~6).toString(16)}`);
  if (fl.length) s += `[${fl.join(',')}]`;
  return s;
};
const rows = [];
const stats = { cat: {}, rng: {}, used: 0, mpGuard: 0, local: 0 };
for (const b of builtins) {
  const { cat, why } = categorize(b);
  const cl = closure.get(b.addr), body = bodies.get(b.addr) ?? '';
  const notes = [];
  let rng = 'none';
  if (cl) {
    const dg = minDepth(cl.D, /^(GameAccess::game_random|game_random)$/), di = minDepth(cl.D, /^internal_random$/);
    const ds = minDepth(cl.D, /^(SoundGlobal::random|SoundType::random)$/);
    if (dg < Infinity) { rng = 'game_random'; notes.push(`game_random@d${dg}`); }
    if (di < Infinity) { rng = rng === 'none' ? 'internal_random' : rng + '+internal_random'; notes.push(`internal_random@d${di}`); }
    if (ds <= 2) notes.push(`sound RNG@d${ds}`);
    const loc = [...cl.D].filter(([n, d]) => d <= 1 && LOCAL_GLOBALS.test(n)).map(([n]) => n);
    if (loc.length) { notes.push('local:' + [...new Set(loc)].join(',')); stats.local++; }
  }
  if (/GameAccess::game_random/.test(body) && rng === 'none') { rng = 'game_random'; notes.push('game_random in body'); }
  if (/timeGetTime/.test(body)) notes.push('timeGetTime');
  if (MP_GUARD.test(body)) { notes.push('MP-guard'); stats.mpGuard++; }
  if (b.ret === 0x12f35f || b.params.some(p => p.type === 0x12f35f)) notes.push('float');
  const ov = byName.get(b.name);
  if (ov.length > 1) notes.push(`overload ${ov.indexOf(b) + 1}/${ov.length}`);
  const scriptParams = b.params.filter(p => p.type !== 0x64ea2a).length + (b.params.some(p => p.type === 0x64ea2a) ? 1 : 0);
  if (b.nargs !== scriptParams) notes.push(`native args ${b.nargs} != params ${b.params.length}`);
  if (!bodies.has(b.addr)) notes.push('no body decompiled');
  const u = usage.get(b.name);
  if (ov.indexOf(b) > 0) notes.push('usage counted on first overload');
  rows.push([
    b.set, b.setIndex, b.index, b.name, typeName(b.ret), b.params.length, b.params.map(fmtParam).join('; '),
    `0x${b.addr.toString(16).padStart(6, '0')}`, implName.get(b.addr) ?? '', `${cat}:${CAT[cat]}`, rng,
    ov.indexOf(b) === 0 ? u.calls : '', ov.indexOf(b) === 0 ? u.files.size : '',
    ov.indexOf(b) === 0 ? `${u.ai}/${u.ctw}/${u.scen}` : '', ov.indexOf(b) === 0 ? `${u.aiF.size}/${u.ctwF.size}/${u.scenF.size}` : '',
    [...why.map(w => 'cat:' + w), ...notes].join('; '),
  ]);
  stats.cat[cat] = (stats.cat[cat] ?? 0) + 1;
  stats.rng[rng] = (stats.rng[rng] ?? 0) + 1;
}
const header = ['set', 'set_index', 'global_index', 'name', 'returns', 'nparams', 'params', 'impl', 'impl_name', 'category', 'rng',
  'calls', 'files', 'calls_ai/ctw/scen', 'files_ai/ctw/scen', 'notes'];
fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, [header, ...rows].map(r => r.join('\t')).join('\n') + '\n');

// ---------------------------------------------------------------------------------------------------------------
if (!quiet) {
  const per = {};
  for (const b of builtins) per[b.set] = (per[b.set] ?? 0) + 1;
  const names = [...byName.keys()];
  const usedNames = names.filter(n => usage.get(n).calls > 0);
  console.log(`builtins: ${builtins.length} registrations (${Object.entries(per).map(([k, v]) => `${k} ${v}`).join(', ')}), ${names.length} distinct names`);
  console.log(`overloaded names: ${names.filter(n => byName.get(n).length > 1).length} (${[...byName].filter(([, v]) => v.length > 1).map(([k, v]) => `${k}×${v.length}`).join(' ')})`);
  console.log(`categories: ${Object.entries(stats.cat).map(([k, v]) => `${k}:${CAT[k]} ${v}`).join(', ')}`);
  console.log(`rng: ${Object.entries(stats.rng).map(([k, v]) => `${k} ${v}`).join(', ')}; MP-guarded bodies ${stats.mpGuard}; local-state refs ${stats.local}`);
  const tn = new Map();
  for (const b of builtins) for (const t of [b.ret, ...b.params.map(p => p.type)]) tn.set(typeName(t), (tn.get(typeName(t)) ?? 0) + 1);
  console.log(`types: ${[...tn].map(([k, v]) => `${k} ${v}`).join(', ')}`);
  console.log(`scripts: ${scripts.length} .bhs; distinct builtin names used: ${usedNames.length} of ${names.length}`);
  const top = usedNames.sort((a, b) => usage.get(b).calls - usage.get(a).calls).slice(0, 20);
  console.log('top 20: ' + top.map(n => `${n} ${usage.get(n).calls}/${usage.get(n).files.size}f`).join(', '));
  console.log(`wrote ${path.relative(process.cwd(), OUT)}`);
}
