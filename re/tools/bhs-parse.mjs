// BigHugeScript (.bhs) front end, reproducing riseofnations.exe's own lexer and parser (docs/script-spec.md §1-2).
//
// The scanner is the game's flex DFA and the parser is its bison 1.x LALR automaton, both read at run time from the
// user's riseofnations.exe (re/tools/bhs-tables.mjs). The hand-written parts are what the binary does around them:
// the per-rule lexer actions (Lexer::yylex @9bc8e0), identifier classification against the symbol tables
// (Lexer::resolve_token @9c1460, SymTable::find_by_token @9d8e90), include files (Lexer::start_include_file @9c1960,
// open_file @9bff30, close_file @9bfe20), and the parser actions that create symbols or scopes (yyparse @9ba430,
// SyntaxNode::eval @9dc710 for FINISH_DECL / PARAM_DECL / COMPLETE_FILE).
//
//   node re/tools/bhs-parse.mjs --all [game]            parse every shipped .bhs as a main file; non-zero exit on any error
//   node re/tools/bhs-parse.mjs <file.bhs>...           parse the given files (errors, script/trigger summary)
//   node re/tools/bhs-parse.mjs --grammar               print the grammar reconstructed from the LALR tables
//   node re/tools/bhs-parse.mjs --tokens <file.bhs>     print the token stream (main file only)
//   node re/tools/bhs-parse.mjs --usage [--tsv out]     call-site counts of builtins / script functions over all files
//   node re/tools/bhs-parse.mjs --reductions <file.bhs> the reduction sequence (rule numbers, file switches) for port tests
//   node re/tools/bhs-parse.mjs --features              rule-usage counts over all shipped files (main-file text only)
//   node re/tools/bhs-parse.mjs --selftest              lexer checks on small strings
// Options: --game <dir> (default ~/Games/RiseOfNations), --verbose
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadTables, DEFAULT_GAME } from './bhs-tables.mjs';

// ------------------------------------------------------------------------------------------------ token codes
// Lexer return codes (0x101..0x13e). Names follow the SyntaxTypes / OpCodeTypes vocabulary of the PDB.
export const TOK = {
  IDENTIFIER: 0x101, INT_LIT: 0x102, FLOAT_LIT: 0x103, CHAR_LIT: 0x104, STRING_LIT: 0x105,
  INC_OP: 0x106, DEC_OP: 0x107, LEFT_OP: 0x108, RIGHT_OP: 0x109, LE_OP: 0x10a, GE_OP: 0x10b, EQ_OP: 0x10c,
  NE_OP: 0x10d, POW_OP: 0x10e, AND_OP: 0x10f, OR_OP: 0x110, REF_ASSIGN: 0x111, MUL_ASSIGN: 0x112,
  DIV_ASSIGN: 0x113, MOD_ASSIGN: 0x114, ADD_ASSIGN: 0x115, SUB_ASSIGN: 0x116, LEFT_ASSIGN: 0x117,
  RIGHT_ASSIGN: 0x118, AND_ASSIGN: 0x119, XOR_ASSIGN: 0x11a, OR_ASSIGN: 0x11b, POW_ASSIGN: 0x11c,
  STATIC: 0x11d, REF: 0x11e, ERROR_TOKEN: 0x11f, SCRIPT_NAME: 0x120, SCRIPT_KIND: 0x121, VAR_NAME: 0x122,
  CONST_NAME: 0x123, TRIGGER_NAME: 0x124, TYPE_NAME: 0x125, VOID: 0x126, LOCALIZE: 0x127,
  RUN_ONCE: 0x12a, TRIGGER: 0x12b, ENABLE_TRIGGER: 0x12c, DISABLE_TRIGGER: 0x12d, END_INCLUDE: 0x12e,
  EVAL_END: 0x12f, LABELS: 0x130, STRUCT: 0x131, ARRAY: 0x132, CASE: 0x133, DEFAULT: 0x134, IF: 0x135,
  ELSE: 0x136, SWITCH: 0x137, WHILE: 0x138, DO: 0x139, FOR: 0x13a, CONTINUE: 0x13c, BREAK: 0x13d, RETURN: 0x13e,
};
const TOKNAME = Object.fromEntries(Object.entries(TOK).map(([k, v]) => [v, k]));
export const tokName = (c) => c === 0 ? '$end' : c < 256 ? `'${String.fromCharCode(c)}'` : (TOKNAME[c] ?? `0x${c.toString(16)}`);

// Symbol kinds (SymType.type). Data types use the case-insensitive String hash of their name instead.
export const KIND = { FUNC: 0x120, SCRIPT_KIND: 0x121, VAR: 0x122, CONST: 0x123, TRIGGER: 0x124, TOGGLE: 0x143, ENUM: 0x144 };
const T_INT = 0x57bad, T_FLOAT = 0x12f35f, T_STRING = 0x168174, T_VOID = 0x84048, T_BOOL = 0xa6192;

// ------------------------------------------------------------------------------------------------ flex actions
// Rule numbers are flex's (yy_accept values); start states: 1 INITIAL, 3 string, 5 comment, 7 line comment,
// 9 skip-to-EOF (lex_change(0)), 11/13 editor string/comment. EOF action = 0x70 + (start-1)/2.
// Keyword rules 1..21 return fixed tokens (Lexer::yylex switch).
const KEYWORD_TOKENS = {
  1: TOK.BREAK, 2: TOK.CASE, 3: TOK.CONTINUE, 4: TOK.DEFAULT, 5: TOK.RETURN, 6: TOK.DO, 7: TOK.ELSE, 8: TOK.FOR,
  9: TOK.IF, 10: TOK.SWITCH, 11: TOK.WHILE, 12: TOK.STATIC, 13: TOK.REF, 14: TOK.STRUCT, 15: TOK.ARRAY, 16: TOK.ARRAY,
  17: TOK.TRIGGER, 18: TOK.RUN_ONCE, 19: TOK.ENABLE_TRIGGER, 20: TOK.DISABLE_TRIGGER, 21: TOK.LABELS,
};
const OP_TOKENS = {
  39: TOK.RIGHT_ASSIGN, 40: TOK.LEFT_ASSIGN, 41: TOK.ADD_ASSIGN, 42: TOK.SUB_ASSIGN, 43: TOK.MUL_ASSIGN,
  44: TOK.DIV_ASSIGN, 45: TOK.MOD_ASSIGN, 46: TOK.RIGHT_OP, 47: TOK.LEFT_OP, 48: TOK.INC_OP, 49: TOK.DEC_OP,
  50: TOK.AND_OP, 51: TOK.OR_OP, 52: TOK.LE_OP, 53: TOK.GE_OP, 54: TOK.EQ_OP, 55: TOK.NE_OP, 56: TOK.POW_ASSIGN,
  57: 0x3b, 58: 0x7b, 59: 0x7d, 60: 0x2c, 61: 0x3a, 62: 0x3d, 63: 0x28, 64: 0x29, 65: 0x5b, 66: 0x5d, 67: 0x2e,
  68: TOK.AND_OP /* '&' is logical AND */, 69: 0x21, 70: 0x2d, 71: 0x2b, 72: 0x2a, 73: 0x2f, 74: 0x25, 75: 0x3c,
  76: 0x3e, 77: TOK.POW_OP /* '^' is power */, 78: TOK.OR_OP /* '|' is logical OR */, 79: 0x3f,
  80: TOK.REF_ASSIGN /* => */, 81: TOK.REF_ASSIGN /* = ref */, 82: TOK.LOCALIZE /* $S */, 109: TOK.ERROR_TOKEN,
};
const ESCAPES = { '"': '"', "'": "'", '\\': '\\', f: '\f', n: '\n', r: '\r', t: '\t', v: '\v' };
const unescape = (c) => ESCAPES[c] ?? c;

// ------------------------------------------------------------------------------------------------ symbol tables
class SymTable {
  constructor(parent, type, fileEntry = null) { this.parent = parent; this.type = type; this.syms = []; this.fileEntry = fileEntry; }
  add(sym) { this.syms.push(sym); return sym; }
  // child table: SymTable::add_child_table @9da010 (flag → 0x10000 "struct/function" level unless already above it)
  child(flag) { return new SymTable(this, (!flag || this.type > 0xffff) ? this.type + 1 : 0x10000); }
}
const ieq = (a, b) => a.length === b.length && a.toLowerCase() === b.toLowerCase();

// ------------------------------------------------------------------------------------------------ the compiler front end
export class BhsFrontEnd {
  constructor({ game = DEFAULT_GAME, tables = loadTables(game), verbose = false } = {}) {
    this.game = game; this.t = tables; this.verbose = verbose;
    this.includePaths = ['scenario/scriptlibrary/']; // prefs default ".\scenario\scriptlibrary\" (Lexer::init_once @9c0f90)
    this.fileIndex = null;
  }

  // case-insensitive path lookup relative to the install (NTFS semantics on a case-sensitive Mac volume)
  resolvePath(rel) {
    if (!this.fileIndex) {
      this.fileIndex = new Map();
      const walk = (d) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else this.fileIndex.set(path.relative(this.game, p).toLowerCase(), p); } };
      walk(this.game);
    }
    const norm = path.normalize(rel.replace(/\\/g, '/')).replace(/^\.\//, '');
    if (path.isAbsolute(norm) && fs.existsSync(norm)) return norm;
    return this.fileIndex.get(norm.toLowerCase()) ?? null;
  }

  // ---------------------------------------------------------------- per-compile state
  reset() {
    this.root = new SymTable(null, 0);
    // SymTable::init_root @9d8c80: the five data types from ScriptGameInterfaceBase::init @9d5a60, then three StringWrapType
    // script kinds (kind 0x121). Type ids = case-insensitive hash of the name (String::generate_hash).
    for (const [name, id] of [['int', T_INT], ['float', T_FLOAT], ['string', T_STRING], ['void', T_VOID], ['bool', T_BOOL]])
      this.root.add({ name, kind: id, isType: true });
    for (const name of ['scenario', 'conquest', 'ai']) this.root.add({ name, kind: KIND.SCRIPT_KIND });
    this.typeInt = this.root.syms[0]; this.typeVoid = this.root.syms[3];
    this.curTable = null; this.curScope = 0; this.curType = this.typeInt; this.curScript = null;
    this.fileStack = []; this.cur = null; this.allFiles = [];
    this.errors = []; this.calls = []; this.scripts = []; this.triggers = []; this.labels = 0; this.structs = [];
    this.start = 1; this.commentNest = 0; this.callCond = 0; this.crr = '';
    this.loopStack = []; this.ruleCounts ??= new Map(); this.features ??= new Map();
  }

  feature(k) { this.features.set(k, (this.features.get(k) ?? 0) + 1); }
  countRule(r) {
    this.ruleCounts.set(r, (this.ruleCounts.get(r) ?? 0) + 1);
    if (r === 169) { const vs = this.vstack; let kind = '?'; for (let i = vs.length - 1; i >= Math.max(0, vs.length - 9); i--) if (vs[i] === 'for' || vs[i] === 'while' || vs[i] === 'do') { kind = vs[i]; break; } this.loopStack.push(kind); }
    if (r === 168) this.loopStack.push('switch');
    if (r >= 170 && r <= 173 || r === 167) this.loopStack.pop();
    if (r === 174) { const enclosing = [...this.loopStack].reverse().find(k => k !== 'switch'); this.feature(`continue inside ${enclosing ?? 'nothing'}`); }
    if (r === 175) this.feature(`break inside ${this.loopStack[this.loopStack.length - 1] ?? 'nothing'}`);
  }

  error(msg) { this.errors.push(`${this.cur ? path.relative(this.game, this.cur.file) : '?'}:${this.cur?.line ?? 0}: ${msg}`); }

  // LexerFileEntry::init_buffer @9c0b30 + Lexer::set_curr_buffer @9bfeb0
  openFile(file) {
    const entry = { file, data: fs.readFileSync(file), pos: 0, line: 1, includes: [], scriptTable: null };
    entry.table = new SymTable(this.root, this.root.type + 1, entry);
    if (this.cur) { this.cur.includes.push(entry); this.fileStack.push(this.cur); }
    // init_buffer resets the compiler's declaration state
    this.curScope = 0; this.curType = this.typeInt; this.curScript = null;
    this.cur = entry; this.start = 1; this.curTable = entry.table; this.allFiles.push(entry);
    return entry;
  }
  // Lexer::close_file @9bfe20 (called from SyntaxNode::eval COMPLETE_FILE)
  closeFile() {
    if (!this.fileStack.length) return 1;
    this.cur = this.fileStack.pop(); this.start = 1; this.curTable = this.cur.table;
    return 0;
  }

  // SymTable::find_by_token @9d8e90: walk table → parents; at the current file's table also search its includes
  findByToken(name) {
    for (let t = this.curTable; t; t = t.parent) {
      if (this.cur && t === this.cur.table) { const r = this.findRecurseInclude(this.cur.table, name); if (r) return r; continue; }
      for (const s of t.syms) if (ieq(s.name, name)) return { sym: s, table: t };
    }
    return null;
  }
  findRecurseInclude(table, name) {
    for (const s of table.syms) if (ieq(s.name, name)) return { sym: s, table };
    for (const inc of table.fileEntry?.includes ?? []) { const r = this.findRecurseInclude(inc.table, name); if (r) return r; }
    return null;
  }

  // Lexer::resolve_token @9c1460 (compile mode)
  resolveToken(text) {
    const f = this.findByToken(text);
    if (f) {
      const k = f.sym.kind;
      if (k === KIND.CONST) return [TOK.CONST_NAME, f.sym];
      if (k === KIND.FUNC || k === KIND.SCRIPT_KIND || k === KIND.VAR || k === KIND.TRIGGER) {
        if (this.curTable.type < 0x10000 || this.curTable === f.table) return [k, f.sym];
        return [TOK.IDENTIFIER, text]; // inside a struct body, outer names do not resolve
      }
      if (k === KIND.ENUM) return [TOK.CONST_NAME, f.sym];
      if (k === T_VOID) return [TOK.VOID, f.sym];
      return [TOK.TYPE_NAME, f.sym];
    }
    return [TOK.IDENTIFIER, text];
  }

  // ---------------------------------------------------------------- scanner
  step(s, c) {
    const { yy_chk, yy_base, yy_def, yy_meta, yy_nxt } = this.t;
    while (yy_chk[yy_base[s] + c] !== s) { s = yy_def[s]; if (s > this.t.YY_LASTDFA) c = yy_meta[c]; }
    return yy_nxt[yy_base[s] + c];
  }
  // one DFA match from the current position; returns [rule, text]. A NUL byte (end of buffer) yields rule 111.
  match() {
    const { yy_ec, yy_accept, yy_base } = this.t, d = this.cur.data;
    const startPos = this.cur.pos;
    if (startPos >= d.length) return [0x70 + ((this.start - 1) >> 1), ''];
    // At the end of the buffer flex re-derives the state before the NUL and takes its action (EOB_ACT_LAST_MATCH,
    // yy_get_previous_state); a NUL at the start of a token is end of file.
    let s = this.start, p = startPos, lastAcc = 0, lastPos = startPos;
    while (p < d.length) {
      s = this.step(s, yy_ec[d[p]]); p++;
      if (yy_accept[s]) { lastAcc = yy_accept[s]; lastPos = p; }
      if (yy_base[s] === this.t.YY_JAMBASE) break;
    }
    let rule = yy_accept[s], end = p;
    if (!rule) { rule = lastAcc; end = lastPos; }
    const text = d.toString('latin1', startPos, end);
    this.cur.pos = end;
    return [rule, text];
  }

  // Lexer::yylex @9bc8e0 (LEXER_COMPILE): returns [token, value]
  lex() {
    for (;;) {
      const [rule, text] = this.match();
      if (rule === 0) { this.cur.pos++; continue; }
      if (KEYWORD_TOKENS[rule]) { if (rule === 21) this.start = 1; return [KEYWORD_TOKENS[rule], text]; }
      if (OP_TOKENS[rule]) return [OP_TOKENS[rule], text];
      switch (rule) {
        case 22: { // include "file": text = 'include "x"' (offset 9, drop the closing quote)
          const name = text.slice(9, -1);
          this.includeFile(name);
          continue;
        }
        case 23: return [TOK.INT_LIT, '0']; // false
        case 24: return [TOK.INT_LIT, '1']; // true
        case 25: // 'parse': marks the next string literal for localisation, then resolves like an identifier
        case 26: return this.resolveToken(text);
        case 27: case 28: case 29: case 30: return [TOK.INT_LIT, text]; // hex, binary, decimal, digits+exponent (!)
        case 31: case 32: return [TOK.FLOAT_LIT, text];
        case 33: this.cur.line++; continue; // \r\n
        case 34: return [TOK.CHAR_LIT, text.slice(1, -1)];
        case 35: return [TOK.CHAR_LIT, unescape(text[2])];
        case 36: this.start = 7; continue; // //
        case 37: this.crr = ''; this.start = 3; continue; // "
        case 38: if (this.commentNest < 16) this.commentNest++; this.callCond = 0; this.start = 5; continue; // /*
        case 83: continue; // ~~ outside eval mode
        case 84: continue; // blanks
        case 110: continue; // bare \n: skipped without counting a line (only \r\n counts)
        // line comment (start 7)
        case 85: this.cur.line++; this.start = 1; continue;
        case 86: case 87: continue;
        // string (start 3)
        case 98: this.cur.line++; this.error('newline in string constant'); this.start = 1; return [TOK.STRING_LIT, this.crr];
        case 99: this.crr += unescape(text[1]); continue;
        case 100: this.start = 1; return [TOK.STRING_LIT, this.crr];
        case 101: case 102: this.crr += text; continue;
        // comment (start 5)
        case 103: this.cur.line++; continue;
        case 104: if (this.commentNest < 16) this.commentNest++; continue;
        case 105: if (this.commentNest) this.commentNest--; if (!this.commentNest) this.start = this.callCond * 2 + 1; continue;
        case 106: case 107: continue;
        case 108: return [this.fileStack.length ? TOK.END_INCLUDE : 0, ''];
        // end of file per start condition
        case 0x70: case 0x73: return [this.fileStack.length ? TOK.END_INCLUDE : 0, ''];
        case 0x71: this.error('end of file in string'); this.start = 1; return [TOK.STRING_LIT, this.crr];
        case 0x72: this.error('end of file in comment'); this.start = 1; continue;
        case 0x74: this.start = 1; return [this.fileStack.length ? TOK.END_INCLUDE : 0, ''];
        default: throw new Error(`unhandled flex rule ${rule} (${JSON.stringify(text)})`);
      }
    }
  }

  // Lexer::start_include_file @9c1960 / open_file @9bff30: current file's directory, then as given (install-relative),
  // then each include path. Max include depth 16.
  includeFile(name) {
    if (this.fileStack.length >= 16) { this.error(`includes nested deeper than 16`); return; }
    const dir = path.relative(this.game, path.dirname(this.cur.file));
    let p = this.resolvePath(path.join(dir, name.replace(/\\/g, '/'))) ?? this.resolvePath(name);
    for (const ip of this.includePaths) if (!p) p = this.resolvePath(ip + name.replace(/\\/g, '/'));
    if (!p) { this.error(`can't open include file "${name}"`); return; }
    this.openFile(p);
  }

  // ---------------------------------------------------------------- parser (bison 1.x skeleton as in yyparse @9ba430)
  parseFile(file) {
    this.reset();
    this.mainFile = file;
    this.openFile(file);
    const t = this.t, NT = 90;
    const states = [0], values = [null];
    let state = 0, yychar = -2, lval = null, errStatus = 0, nerrs = 0;
    const translate = (c) => (c >>> 0) <= t.YYMAXUTOK ? t.yytranslate[c] : t.YYUNDEF;
    for (let guard = 0; guard < 5e6; guard++) {
      let n = t.yypact[state], rule;
      let doDefault = n === t.YYFLAG;
      if (!doDefault) {
        if (yychar === -2) { [yychar, lval] = this.lex(); }
        const tok = yychar <= 0 ? 0 : translate(yychar);
        if (yychar <= 0) yychar = 0;
        const i = n + tok;
        if (i < 0 || i > t.YYLAST || t.yycheck[i] !== tok) doDefault = true;
        else {
          const a = t.yytable[i];
          if (a < 0) { if (a === t.YYFLAG) { rule = 0; } else rule = -a; }
          else if (a === 0) rule = 0;
          else {
            if (a === t.YYFINAL) return this.finish(nerrs);
            if (yychar !== 0) yychar = -2;
            states.push(a); values.push(lval); state = a;
            if (errStatus) errStatus--;
            continue;
          }
        }
      }
      if (doDefault) rule = t.yydefact[state];
      if (!rule) { // syntax error and recovery (bison.simple)
        if (errStatus === 0) { nerrs++; this.error(`parse error near ${tokName(yychar)} ${lval && typeof lval === 'string' ? JSON.stringify(lval) : ''}`); }
        else if (errStatus === 3 && yychar === 0) return this.finish(nerrs + 1);
        else if (errStatus === 3) yychar = -2;
        errStatus = 3;
        for (;;) {
          const p = t.yypact[state];
          if (p !== t.YYFLAG) { const i = p + 1; if (i >= 0 && i <= t.YYLAST && t.yycheck[i] === 1) { const a = t.yytable[i]; if (a > 0 && a !== t.YYFINAL) { states.push(a); values.push(lval); state = a; break; } if (a === t.YYFINAL) return this.finish(nerrs); } }
          if (states.length === 1) return this.finish(nerrs + 1);
          states.pop(); values.pop(); state = states[states.length - 1];
        }
        continue;
      }
      // reduce
      const len = t.yyr2[rule];
      const rhs = values.slice(values.length - len);
      this.vstack = values;
      if (this.onReduce) this.onReduce(rule);
      if (this.cur && this.cur.file === this.mainFile) this.countRule(rule);
      const v = this.action(rule, rhs, len ? rhs[0] : null);
      states.length -= len; values.length -= len;
      const lhs = t.yyr1[rule];
      const top = states[states.length - 1];
      const gi = t.yypgoto[lhs - NT] + top;
      state = (gi >= 0 && gi <= t.YYLAST && t.yycheck[gi] === top) ? t.yytable[gi] : t.yydefgoto[lhs - NT];
      states.push(state); values.push(v);
    }
    throw new Error('parser did not terminate');
  }
  finish(nerrs) { return { errors: this.errors, nerrs, scripts: this.scripts, triggers: this.triggers, calls: this.calls, files: this.allFiles.map(f => f.file) }; }

  newVar(name, type) { return { name, kind: KIND.VAR, scope: this.curScope, type: type ?? this.curType }; }
  resetDecl() { this.curScope = 0; this.curType = this.typeVoid; }

  // Semantic actions (yyparse switch, case number = rule number). Only what affects classification of later tokens, plus
  // bookkeeping for statistics. The value of a symbol is whatever the next rule needs.
  action(r, v, dflt) {
    switch (r) {
      case 85: this.curScope = TOK.STATIC; return TOK.STATIC;
      case 86: this.curScope = TOK.REF; return TOK.REF;
      case 87: this.curType = v[0]; return v[0];                                  // type: TYPE_NAME
      case 88: case 89: { const el = r === 88 ? v[2] : v[0]; const at = { name: `${el.name}[]`, kind: 'array', of: el, isType: true }; this.curType = at; return at; }
      case 90: case 91: this.curScope = 0; return v[0];
      case 92: this.curScope = 0; this.curType = this.typeInt; return this.typeInt; // opt_type: empty → int
      case 93: return this.newVar(v[0]);                                           // declarator: IDENTIFIER
      case 94: v[0].type = { name: `${v[0].type?.name}[]`, kind: 'array', of: v[0].type, isType: true }; return v[0];
      case 95: case 96: case 97: case 98: return v[0];
      case 99: case 100: case 101: return v[0];
      case 102: this.curTable.add(v[0]); return v[0];                              // FINISH_DECL adds the variable
      case 105: case 106: case 107: this.resetDecl(); return null;
      case 108: this.curTable.add(v[0]); this.resetDecl(); return null;             // implicit declaration `x = e;`
      case 110: case 111: {                                                        // struct NAME { members }
        const name = r === 110 ? v[1] : v[1].name;
        const st = { name, kind: 'struct', isType: true };
        this.cur.table.add(st); this.structs.push(name);
        this.curTable = this.curTable.parent;                                      // add_struct_type pops the member table
        return st;
      }
      case 116: this.curTable.add(v[0]); return v[0];                              // struct member
      case 117: case 118: case 119: case 120: this.curTable.add(v[v.length - 1]); return v[v.length - 1]; // PARAM_DECL
      case 121: this.resetDecl(); return v[0];
      case 122: this.resetDecl(); return v[0];
      case 152: case 153: this.curTable = this.curTable.parent; return null;        // case clause end
      case 154: case 155: this.curTable = this.curTable.child(0); return null;      // case label opens a scope
      case 178: case 179: {                                                        // trigger [NAME]
        const tr = { name: r === 178 ? v[1] : '', kind: KIND.TRIGGER };
        (this.cur.scriptTable ?? this.cur.table).add(tr); this.triggers.push(tr.name); return tr;
      }
      case 201: case 202: case 203: case 204: {                                    // calls
        const callee = r <= 202 ? v[2] : v[0];                                     // IDENTIFIER text or SCRIPT_NAME symbol
        const name = typeof callee === 'string' ? callee : callee.name;
        this.calls.push({ name, kind: typeof callee === 'string' ? (r <= 202 ? 'method' : 'ident') : 'script', file: this.cur.file });
        return name;
      }
      case 205: this.curTable = this.curTable.child(0); return null;                // block '{'
      case 206: this.curTable = this.curTable.child(1); return null;                // struct body '{'
      case 207: this.curTable = this.curTable.parent; return null;                  // '}'
      case 208: {                                                                  // script scope opens before params
        this.curTable = this.curTable.child(0); this.cur.scriptTable = this.curTable;
        this.curScope = 0; this.curType = this.typeVoid; return null;
      }
      case 209: case 210: this.curTable = this.cur.table; this.cur.scriptTable = null; this.curScript = null; return null;
      case 212: case 213: case 214: {                                              // [type] KIND [name] params
        const name = r === 212 ? v[2] : r === 214 ? v[2].name : '';
        let fn = r === 214 ? v[2] : null;
        if (!fn) { fn = { name, kind: KIND.FUNC, scriptKind: v[1].name, ret: v[0]?.name ?? 'int' }; this.cur.table.add(fn); }
        this.curScript = fn; this.scripts.push(`${v[1].name} ${name || '(unnamed)'}`);
        return fn;
      }
      case 218: case 219: case 220: case 221: {                                     // labels
        const name = r <= 219 ? v[0] : v[2];
        this.cur.table.add({ name, kind: KIND.ENUM }); this.labels++; return null;
      }
      case 192: case 193: this.closeFile(); return null;                           // COMPLETE_FILE
      default: return dflt;
    }
  }

  // ---------------------------------------------------------------- token dump
  tokens(file) {
    this.reset(); this.openFile(file);
    const out = [];
    for (;;) { const [tok, val] = this.lex(); out.push([tok, val]); if (tok === 0) break; if (out.length > 1e6) break; }
    return out;
  }
}

// ------------------------------------------------------------------------------------------------ grammar reconstruction
// The LALR tables carry no right-hand sides (no YYDEBUG). Every state has a unique accessing symbol; a rule's RHS is
// the accessing symbols of any path of yyr2[r] states ending in a state that reduces r (sound for LR automata).
export function reconstructGrammar(t) {
  const NT = 90, NSTATES = t.yypact.length, NSYMS = 174, NRULES = t.yyr1.length;
  const trans = [], reds = [];
  for (let s = 0; s < NSTATES; s++) {
    trans[s] = new Map(); reds[s] = new Set(); if (t.yydefact[s]) reds[s].add(t.yydefact[s]);
    const p = t.yypact[s];
    if (p !== t.YYFLAG) for (let a = 0; a < NT; a++) { const i = p + a; if (i >= 0 && i <= t.YYLAST && t.yycheck[i] === a) { const x = t.yytable[i]; if (x > 0) trans[s].set(a, x); else if (x < 0 && x !== t.YYFLAG) reds[s].add(-x); } }
  }
  for (let A = NT; A < NSYMS; A++) for (let s = 0; s < NSTATES; s++) { const i = t.yypgoto[A - NT] + s; trans[s].set(A, (i >= 0 && i <= t.YYLAST && t.yycheck[i] === s) ? t.yytable[i] : t.yydefgoto[A - NT]); }
  const acc = Array.from({ length: NSTATES }, () => new Set());
  for (let s = 0; s < NSTATES; s++) for (const [a, x] of trans[s]) if (a < NT) acc[x].add(a);
  for (let A = NT; A < NSYMS; A++) { for (let s = 0; s < NSTATES; s++) { const i = t.yypgoto[A - NT] + s; if (i >= 0 && i <= t.YYLAST && t.yycheck[i] === s) acc[t.yytable[i]].add(A); } acc[t.yydefgoto[A - NT]].add(A); }
  const preds = new Map(), reach = new Set([0]);
  const addEdge = (p, X, q) => { if (!preds.has(q)) preds.set(q, new Map()); const m = preds.get(q); const k = p * 1000 + X; if (m.has(k)) return false; m.set(k, p); return true; };
  for (let changed = true; changed;) {
    changed = false;
    for (const s of [...reach]) for (const [a, x] of trans[s]) if (a < NT && x !== t.YYFINAL) { if (addEdge(s, a, x)) changed = true; if (!reach.has(x)) { reach.add(x); changed = true; } }
    for (const s of [...reach]) for (const r of reds[s]) {
      let cur = new Set([s]);
      for (let i = 0; i < t.yyr2[r]; i++) { const nx = new Set(); for (const q of cur) for (const p of preds.get(q)?.values() ?? []) nx.add(p); cur = nx; }
      for (const p of cur) { const g = trans[p].get(t.yyr1[r]); if (addEdge(p, t.yyr1[r], g)) changed = true; if (!reach.has(g)) { reach.add(g); changed = true; } }
    }
  }
  const accOf = (s) => [...acc[s]][0];
  const rules = [null];
  for (let r = 1; r < NRULES; r++) {
    let s = -1; for (const q of reach) if (reds[q].has(r)) { s = q; break; }
    if (s < 0) { rules.push(null); continue; }
    const rhs = []; let q = s;
    for (let i = 0; i < t.yyr2[r]; i++) { rhs.unshift(accOf(q)); q = preds.get(q).values().next().value; }
    rules.push({ lhs: t.yyr1[r], rhs });
  }
  // symbol number → token code
  const code = []; for (let c = 0; c < 320; c++) { const s = t.yytranslate[c]; if (s > 2 && code[s] === undefined) code[s] = c; }
  return { rules, code, NT, unreachable: NSTATES - reach.size };
}

// Names for the 84 nonterminals (ours; the binary has no symbol names). Index = symbol number − 90.
export const NONTERMINALS = [
  'string_list', 'literal', 'var_ref', 'primary', 'array_index', 'field_access', 'incdec_target', 'postfix',
  'unary', 'unary_op', 'pow_expr', 'mul_expr', 'add_expr', 'shift_expr', 'rel_expr', 'eq_expr', 'bitand_expr',
  'bitxor_expr', 'bitor_expr', 'and_expr', 'or_expr', 'cond_expr', 'expr', 'assign_expr', 'assign_op', 'static_kw',
  'ref_kw', 'type', 'opt_type', 'declarator', 'init_declarator', 'declarator_item', 'finished_declarator',
  'declarator_list', 'declaration', 'struct_decl', 'struct_def', 'member_decls', 'member_decl', 'member_declarator',
  'param', 'param_item', 'param_list', 'opt_params', 'arg_list', 'expr_list', 'script_item', 'stmt_list',
  'stmt_or_decl', 'for_init', 'statement', 'case_list', 'case_clause', 'case_label', 'script_items', 'body',
  'opt_script_items', 'expr_stmt', 'if_stmt', 'switch_stmt', 'switch_head', 'loop_mark', 'loop_stmt', 'jump_stmt',
  'trigger_head', 'run_once_head', 'trigger_def', 'toggle_kw', 'trigger_ref', 'toggle_stmt', 'program', 'file_seq',
  'file_items', 'file_item', 'call', 'block_open', 'struct_open', 'block_close', 'script_scope', 'script_def',
  'script_fwd', 'script_head', 'labels_block', 'label_list',
];

export function grammarText(t) {
  const g = reconstructGrammar(t);
  const sym = (s) => s >= g.NT ? NONTERMINALS[s - g.NT] : s === 1 ? 'error' : tokName(g.code[s]);
  const lines = [];
  let prev = -1;
  g.rules.forEach((r, i) => {
    if (!r) return;
    const lhs = sym(r.lhs);
    const body = r.rhs.map(sym).join(' ') || '/* empty */';
    lines.push(`${String(i).padStart(3)}  ${r.lhs === prev ? ' '.repeat(lhs.length) + ' |' : lhs + ' :'} ${body}`);
    prev = r.lhs;
  });
  return { text: lines.join('\n'), g };
}

// ------------------------------------------------------------------------------------------------ CLI
function allScripts(game) {
  const out = [];
  const walk = (d) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (/\.bhs$/i.test(e.name)) out.push(p); } };
  walk(game); return out.sort();
}

function selftest(fe) {
  const cases = [
    ['int x = 0x1F;', ['TYPE_NAME', 'IDENTIFIER', "'='", 'INT_LIT', "';'"]],
    ['a & b | c ^ d', ['IDENTIFIER', 'AND_OP', 'IDENTIFIER', 'OR_OP', 'IDENTIFIER', 'POW_OP', 'IDENTIFIER']],
    ['x = ref y; z => w;', ['IDENTIFIER', 'REF_ASSIGN', 'IDENTIFIER', "';'", 'IDENTIFIER', 'REF_ASSIGN', 'IDENTIFIER', "';'"]],
    ['1e5 1.5 .5 2. 0b101', ['INT_LIT', 'FLOAT_LIT', 'FLOAT_LIT', 'FLOAT_LIT', 'INT_LIT']],
    ["'a' '\\n' true false", ['CHAR_LIT', 'CHAR_LIT', 'INT_LIT', 'INT_LIT']],
    ['"a\\"b" /* x /* y */ z */ $S("k") // c\r\nif', ['STRING_LIT', 'LOCALIZE', "'('", 'STRING_LIT', "')'", 'IF']],
    ['Array array STRUCT Int', ['ARRAY', 'ARRAY', 'IDENTIFIER', 'TYPE_NAME']],
  ];
  let bad = 0;
  const tmp = path.join(fs.mkdtempSync(path.join(process.env.TMPDIR ?? '/tmp', 'bhs-')), 't.bhs');
  for (const [src, want] of cases) {
    fs.writeFileSync(tmp, src);
    const got = fe.tokens(tmp).map(([t]) => tokName(t)).filter(n => n !== '$end');
    const ok = JSON.stringify(got) === JSON.stringify(want);
    if (!ok) bad++;
    console.log(`${ok ? 'ok  ' : 'FAIL'} ${JSON.stringify(src)} → ${got.join(' ')}`);
  }
  return bad;
}

if (fileURLToPath(import.meta.url) === path.resolve(process.argv[1] ?? '')) {
  const args = process.argv.slice(2);
  const opt = (k) => { const i = args.indexOf(k); return i >= 0 ? args.splice(i, 2)[1] : undefined; };
  const game = opt('--game') ?? DEFAULT_GAME;
  const tsv = opt('--tsv');
  const verbose = args.includes('--verbose');
  const fe = new BhsFrontEnd({ game, verbose });
  if (args.includes('--grammar')) {
    const { text, g } = grammarText(fe.t);
    console.log(text);
    console.log(`\n${g.rules.filter(Boolean).length} rules, ${NONTERMINALS.length} nonterminals, unreachable states: ${g.unreachable}`);
  } else if (args.includes('--selftest')) {
    process.exit(selftest(fe) ? 1 : 0);
  } else if (args.includes('--tokens')) {
    const f = args.filter(a => !a.startsWith('--'))[0];
    for (const [tk, val] of fe.tokens(f)) console.log(tokName(tk), typeof val === 'string' ? JSON.stringify(val) : val?.name ?? '');
  } else if (args.includes('--reductions')) {
    const f = args.filter(a => !a.startsWith('--'))[0];
    const out = [];
    fe.onReduce = (r) => out.push(r);
    const res = fe.parseFile(path.resolve(f));
    console.log(out.join(' '));
    if (res.errors.length) { console.error(res.errors.join('\n')); process.exit(1); }
  } else if (args.includes('--features')) {
    const { g } = grammarText(fe.t);
    const sym = (x) => x >= g.NT ? NONTERMINALS[x - g.NT] : x === 1 ? 'error' : tokName(g.code[x]);
    for (const f of allScripts(game)) { try { fe.parseFile(f); } catch (e) { console.log('ERR', f, e.message); } }
    const rows = [...fe.ruleCounts.entries()].sort((a, b) => a[0] - b[0]);
    for (const [r, n] of rows) console.log(`${String(r).padStart(3)} ${String(n).padStart(7)}  ${sym(g.rules[r].lhs)} : ${g.rules[r].rhs.map(sym).join(' ') || '/* empty */'}`);
    const never = g.rules.map((x, i) => i).filter(i => i && !fe.ruleCounts.has(i));
    console.log(`\nrules never used by shipped files (main-file text only): ${never.join(' ')}`);
    for (const [k, n] of fe.features) console.log(`${k}: ${n}`);
  } else if (args.includes('--all') || args.includes('--usage')) {
    const files = allScripts(game);
    let failed = 0; const calls = new Map(); const fileClass = (f) => { const r = path.relative(game, f).toLowerCase(); return r.startsWith('ai/') ? 'ai' : r.startsWith('conquest/') ? 'ctw' : 'scenario'; };
    const stats = { files: 0, scripts: 0, triggers: 0, includes: 0 };
    for (const f of files) {
      let res;
      try { res = fe.parseFile(f); } catch (e) { res = { errors: [String(e.stack ?? e)], calls: [], scripts: [], triggers: [], files: [] }; }
      // Data/trigger_template.bhs is the editor's fragment for "insert trigger" ($NAMEn placeholders, pasted inside a
      // scenario block), not a compilation unit: check it in that context.
      if (res.errors.length && /trigger_template\.bhs$/i.test(f)) {
        const tmp = path.join(fs.mkdtempSync(path.join(process.env.TMPDIR ?? '/tmp', 'bhs-')), 'frag.bhs');
        fs.writeFileSync(tmp, 'scenario {\r\n' + fs.readFileSync(f, 'latin1').replace(/\$[A-Z]+\d+/g, 'placeholder') + '\r\n}\r\n');
        const r2 = fe.parseFile(tmp);
        if (!r2.errors.length) { console.log(`ok   ${path.relative(game, f)} (fragment: parsed inside "scenario { }" with placeholders substituted)`); res = { ...r2, calls: [] }; }
      }
      stats.files++; stats.scripts += res.scripts.length; stats.triggers += res.triggers.length; stats.includes += res.files.length - 1;
      if (res.errors.length) { failed++; console.log(`FAIL ${path.relative(game, f)}\n  ${res.errors.slice(0, 5).join('\n  ')}`); }
      else if (verbose) console.log(`ok   ${path.relative(game, f)} (${res.scripts.length} scripts, ${res.triggers.length} triggers, ${res.files.length - 1} includes)`);
      // usage counts only from the main file's own text (includes are counted when parsed as main files)
      for (const c of res.calls) {
        if (c.file !== f) continue;
        const key = c.name;
        if (!calls.has(key)) calls.set(key, { name: key, kinds: new Set(), total: 0, files: new Set(), ai: 0, ctw: 0, scenario: 0 });
        const e = calls.get(key); e.kinds.add(c.kind); e.total++; e.files.add(f); e[fileClass(f)]++;
      }
    }
    if (args.includes('--usage')) {
      const rows = [...calls.values()].sort((a, b) => b.total - a.total);
      const lines = ['name\tkind\tcalls\tfiles\tai\tctw\tscenario', ...rows.map(e => `${e.name}\t${[...e.kinds].join(',')}\t${e.total}\t${e.files.size}\t${e.ai}\t${e.ctw}\t${e.scenario}`)];
      if (tsv) fs.writeFileSync(tsv, lines.join('\n') + '\n'); else console.log(lines.slice(0, 80).join('\n'));
      console.log(`distinct callees: ${rows.length}`);
    }
    console.log(`${files.length} files: ${files.length - failed} parsed without error, ${failed} failed; ${stats.scripts} scripts, ${stats.triggers} triggers, ${stats.includes} include openings`);
    process.exit(failed ? 1 : 0);
  } else {
    let failed = 0;
    for (const f of args.filter(a => !a.startsWith('--'))) {
      const res = fe.parseFile(path.resolve(f));
      console.log(`${res.errors.length ? 'FAIL' : 'ok  '} ${f}: ${res.scripts.length} scripts [${res.scripts.join(', ')}], ${res.triggers.length} triggers, files ${res.files.length}`);
      for (const e of res.errors) console.log('  ' + e);
      if (res.errors.length) failed++;
    }
    process.exit(failed ? 1 : 0);
  }
}
