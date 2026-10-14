# BigHugeScript: language, compiler, bytecode, VM, runtime environments, builtins

Status: ✅ lexical grammar and full syntax (the binary's own flex DFA and bison LALR tables, executed by
`re/tools/bhs-parse.mjs`, parse **all 365 shipped `.bhs` files without error**), symbol and scope rules, node → code
patterns for every construct the shipped scripts use, bytecode encoding, the VM's 73 opcodes, value semantics, call and
return protocol, run-to-completion execution model, where each script kind runs in the frame · 🟡 code generation for
constructs no shipped script uses (marked inline), the compiled-image debug tables (§4.4), a few CRT-dependent
conversions (float → string, `powf`) · ❓ open questions in §10. Nothing here is validated against a running game:
no save or replay with compiled scripts exists on this Mac yet (the 6-frame `re/analysis/sample_replay.bin` has no
embedded save). **Saves and replays embed the compiled bytecode** (§4.5), so the first real save becomes a byte-exact
oracle for the compiler.

Deliverables: this document; `re/tools/bhs-tables.mjs` (reads the scanner and parser tables out of the user's
`riseofnations.exe`; nothing game-derived is committed); `re/tools/bhs-parse.mjs` (front end: `--all` parses every
shipped script, `--grammar` prints the reconstructed grammar, `--tokens`, `--usage` call-site counts, `--features`
rule-usage counts, `--selftest`); `re/tools/bhs-builtins.mjs` (generates the builtin table `re/analysis/bhs_builtins.tsv`,
gitignored, from the decompiled registration code). Decompiled C, disassembly and dumps stay local (gitignored):
`re/decomp/script/` (lexer, parser, compiler, symbol tables, VM, runtime; jobs `re/jobs/dc_script_*.txt`,
`asm_script*.txt`), `re/decomp/script_funcs/` (877 builtin bodies, `dc_script_funcs.txt`), `re/decomp/script_rt/`
(game call sites, `dc_script_rt.txt`), `re/analysis/types_script*.txt` (PDB types), `vt_script.txt` (vtables).

Read first: [frame-spec.md](frame-spec.md) (where scripts run in `Game::do_frame`), [state-spec.md](state-spec.md)
(`RunTimeEnv` / `ScriptFile` walks), [replay-spec.md](replay-spec.md) §12 (`game_random`).

## 0. Conventions and overview

- Addresses are `riseofnations.exe` VAs; names are PDB names. Pseudocode is paraphrase, never decompiler text.
- Source files: `script\lexer.cpp`, `compiler.cpp`, `SyntaxTree` (`SyntaxNode`), `SymTable`/`symtype.cpp`, `opcodes.cpp`,
  `VirtualMachine.cpp`, `runtimeenv.cpp`, `scriptfile.cpp`, `scripttype.cpp`, `scriptfuncset.cpp`, `scriptfunctions.cpp`
  (the 842 `ScenarioFuncSet` builtins), `scriptsyslib.cpp` (math/string/array/trigger utilities), `scripttimers.cpp`;
  `BigHugeScript.l` (flex) and `BigHugeScript.y` (bison 1.x, `yyparse` @9ba430).
- Type ids are the **case-insensitive String hash** of the type name (`String::generate_hash`, second result; the
  same hash `re/tools/locstrings.mjs hash` computes): `int` 0x57bad, `float` 0x12f35f, `string` 0x168174, `bool` 0xa6192,
  `void` 0x84048. Array and struct types are hashed from their unique token (§2.5).
- "Kind" = `SymType.type` of a symbol: 0x120 script function (`LocalScriptType`), 0x121 script kind (`StringWrapType`),
  0x122 variable (`VarType`), 0x123 constant (`ConstType`), 0x124 trigger (`TriggerType`), 0x143 trigger toggle,
  0x144 label (`EnumConstType`), 0x145 builtin (`ScriptFunc`); data types use their type id as kind.

```
.bhs text ──flex DFA (Lexer::yylex @9bc8e0)──▶ tokens ──bison (yyparse @9ba430)──▶ SyntaxNode per reduction
   ▲   include "f" opens a nested buffer                      │  SyntaxNode::init → eval() emits OpCode lists at once
   │                                                          ▼  (no separate AST pass: code is generated bottom-up)
   └── identifiers classified against SymTables       COMPLETE_FILE ─▶ LexerFileEntry::write_file: count_code (addresses),
                                                                         SymTable::write + OpCode::write_code ─▶ chunk image
                                                                         ─▶ ScriptFile::load_script_file (in memory)
RunTimeEnv::run_script(name, args) ─▶ ScriptFile::find_script ─▶ VirtualMachine frames ─▶ exec loop to completion
```

There is no AST stage: each reduction builds a node whose `eval()` immediately produces a doubly linked list of
`OpCode`s (`OpCode` 0x28 bytes: `count`, `line`, `op`, `arg1`, `arg2`, `first`, `next`, `prev`), concatenated with
`OpCode::prepend`. The list is serialised when the file completes.

## 1. Lexical grammar

### 1.1 Files

- Read as bytes (`LexerFileEntry::init_buffer`, `cp_inc` 1; `check_unicode` switches to 2-byte units for UTF-16 files,
  none ship). All 365 shipped files are ASCII/Latin-1 with **CRLF** line ends.
- **Only `\r\n` counts as a line break** (`next_line`), and only `\r\n` ends a `//` comment. A bare `\n` is skipped
  everywhere, so in an LF-only file a `//` comment runs to the end of the file and line numbers stay 1. A port must
  feed the original bytes; do not normalise line endings.
- Keywords are case-sensitive (they are DFA patterns); identifiers resolve case-insensitively (§1.4).

### 1.2 The scanner

The scanner is a flex DFA (`yy_ec` 256 classes→64, `yy_base/def/nxt/chk/meta/accept`, jam base 457, last DFA state 280;
addresses in `bhs-tables.mjs`). Longest match, first rule wins on ties; at end of buffer flex re-derives the state
before the NUL (`EOB_ACT_LAST_MATCH`), and a NUL at a token start is end of file. Start conditions: 1 INITIAL,
3 string, 5 block comment, 7 line comment, 9 skip-rest (`Lexer::lex_change(0)`, editor), 11/13 editor string/comment.
Actions (flex rule → effect; LEXER_COMPILE mode):

| Rules | Pattern | Result |
|---|---|---|
| 1–11 | `break case continue default return do else for if switch while` | BREAK CASE CONTINUE DEFAULT RETURN DO ELSE FOR IF SWITCH WHILE |
| 12, 13 | `static`, `ref` | STATIC, REF |
| 14 | `struct` | STRUCT |
| 15, 16 | `array`, `Array` | ARRAY (both spellings) |
| 17, 18 | `trigger`, `run_once` | TRIGGER, RUN_ONCE |
| 19, 20 | `enable_trigger`, `disable_trigger` | ENABLE_TRIGGER, DISABLE_TRIGGER (keywords, not builtins) |
| 21 | `labels` | LABELS |
| 22 | `include` + one blank + `"name"` | opens the include file (§1.6); no token. The name is taken from offset 9 to the last char but one, so exactly one blank is assumed |
| 23, 24 | `false`, `true` | INT_LIT `"0"` / `"1"` (there is no boolean literal) |
| 25 | `parse` | sets `loc_next_string`, then resolves as an identifier (it is the `StringUtilFuncSet::parse` builtin) |
| 26 | identifier `[A-Za-z_][A-Za-z_0-9]*` | `Lexer::resolve_token` (§1.4) |
| 27, 28, 29 | `0x…`, `0b…`, decimal digits | INT_LIT (text) |
| 30 | digits with exponent, no dot (`1e5`) | **INT_LIT** (value = leading digits only: `1e5` → 1) |
| 31, 32 | `.5`, `1.5`, `2.` (optional exponent) | FLOAT_LIT. **No `f` suffix** (corrected 2026-10-13 by the C++ port): `1.5f` is FLOAT_LIT `1.5` then the identifier `f` |
| 33 | `\r\n` | line count +1 |
| 34, 35 | `'c'`, `'\c'` | CHAR_LIT (value = the char code as an int) |
| 36 | `//` | start 7 (to `\r\n`) |
| 37 | `"` | start 3, clear the string buffer |
| 38 | `/*` | start 5; comments nest (`comment_nest`, capped at 16) |
| 39–56 | `>>= <<= += -= *= /= %= >> << ++ -- && \|\| <= >= == != ^=` | RIGHT_ASSIGN … NE_OP, POW_ASSIGN. `&=` and `\|=` are **not** tokens (`&` `=` → AND_OP then `=`) |
| 57–67, 69–76, 79 | `; { } , : = ( ) [ ] . ! - + * / % < > ?` | the character |
| 68 | `&` | **AND_OP** (logical and, same as `&&`) |
| 77 | `^` | **POW_OP** (power) |
| 78 | `\|` | **OR_OP** (logical or, same as `\|\|`) |
| 80, 81 | `=>` and `= ref` | REF_ASSIGN. The second pattern is `=`, **exactly one space**, `ref`; it does not check what follows, so `= refx` is REF_ASSIGN then the identifier `x`, and `=  ref` (two blanks) is `=` then REF |
| 82 | `$S` | LOCALIZE |
| 83 | `~~` | EVAL_END in eval mode, otherwise skipped |
| 84, 110 | blanks (space, tab, `\f`), bare `\n` | skipped. `\v` and a lone `\r` are ERROR_TOKEN |
| 109 | any other char (`$`, `@`, `#`, `~`, `` ` ``, non-ASCII) | ERROR_TOKEN (the parser reports it) |
| 98–102 (start 3) | `\r\n` / `\c` / `"` / text / `\r` | newline in string = error + STRING_LIT; escape; end → STRING_LIT; append |
| 103–107 (start 5) | `\r\n`, `/*`, `*/`, other | count line; nest+1; nest−1 and back to INITIAL at 0; skip |
| EOF | per start condition (rule 0x70 + (start−1)/2) | INITIAL/line comment: END_INCLUDE if an include is open, else end; string/comment: error |

Escapes (`Lexer::append_esc` @9c15a0): `\" \' \\ \f \n \r \t \v`; any other `\c` gives `c`. Token codes are
0x101 IDENTIFIER … 0x13e RETURN (`TOK` in `bhs-parse.mjs`); the parser's `yytranslate` maps them to 90 terminals.
`&`, `|`, `^` therefore never reach the bitwise and xor rules of the grammar (§2.2).

### 1.3 Literals

INT_LIT, FLOAT_LIT, CHAR_LIT and string literals become `ConstType` symbols in the **file's** symbol table, pooled by a
canonical token (`SymTable::add_const_type` @9d9d10; a second occurrence of the same value reuses the symbol). The
lookup is the symbol table's, i.e. **case-insensitive**: `"Hello"` and `"hello"` in one file share one constant with
the first spelling (a script comparing them cannot tell, but a later `print` shows the first spelling):

| Literal | Canonical token / value |
|---|---|
| int | `String::convert_int`: `0b…` binary, `0x…` hex, `0d…` decimal, otherwise `_wtoi` (stops at the first non-digit; MSVC clamps overflow). Token = the decimal value |
| char | the character's code as an int constant |
| float | `String::get_double`, re-printed with `_gcvt_s(…, 1014 digits)` (§5.2); type float (stored as f32) |
| string | token `"` + text; adjacent string literals concatenate (`string_list`) |
| `$S("key")` | `Lexer::localize_string`: the key is looked up in `scen_script_loc.xml` (per language, path from prefs `.\scenario\scriptlibrary\`) and the constant is the localised text |
| `parse("…")` | `loc_next_string` makes the next string literal a localisation key too |

### 1.4 Identifier classification

The language is context sensitive (a "typedef-name" scanner). `Lexer::resolve_token` @9c1460 looks the text up with
`SymTable::find_by_token` @9d8e90 — **case-insensitive** (`String::ignore`, i.e. `_wcsicmp` with a case-insensitive
hash shortcut), walking the current table, then parents; at the current file's table the search also recurses into
the tables of files it included (`find_by_token_recurse_include`). Result:

| Symbol found | Token |
|---|---|
| none | IDENTIFIER (new name) |
| constant (0x123), label (0x144: yields its `ConstType`) | CONST_NAME |
| script function (0x120), script kind (0x121: `scenario`, `conquest`, `ai`), variable (0x122), trigger (0x124) | SCRIPT_NAME, SCRIPT_KIND, VAR_NAME, TRIGGER_NAME — **but** inside a struct body (table type ≥ 0x10000) a match in an outer table gives IDENTIFIER, so member names may reuse outer names |
| `void` | VOID |
| any other type (`int float string bool`, structs) | TYPE_NAME |

Builtins are **not** in the symbol tables: a builtin call is always `IDENTIFIER '('` and is bound during code
generation (§3.6). The root table (`SymTable::init_root` @9d8c80) holds `int float string void bool` and the three script
kinds; everything else is added by the parser actions listed in §2.4.

### 1.5 Example token stream

`int x = y + 1;` inside a script where `y` is declared: TYPE_NAME IDENTIFIER `=` VAR_NAME `+` INT_LIT `;`. If `y` was
never declared, `y` is IDENTIFIER and the statement is a syntax error (an undeclared name cannot be read; it can only be
declared by assignment, §2.4).

### 1.6 Includes

`include "f"` (rule 22, `Lexer::start_include_file` @9c1960, `open_file` @9bff30) is handled by the scanner in place,
anywhere in the token stream. Search order: the including file's directory + name; the name relative to the install
(content directory); each include path (`prefs`, default `.\scenario\scriptlibrary\`, separators `,;`). Nesting is
limited to 16 (`file_stack`). The duplicate-include check compares against an empty string and never fires, so a file
included twice is lexed twice (the shipped libraries do not rely on it). The included file gets its own
`LexerFileEntry` and **file symbol table** (child of the root, type 1), is recorded in the includer's `local_includes`
(so the includer sees its symbols), and becomes the current buffer; `init_buffer` resets the declaration state
(`curr_scope`, `curr_type`, `cur_script`).

At the included file's end the scanner returns END_INCLUDE. The grammar has `file_seq : file_items | file_seq
END_INCLUDE file_items`; reducing either alternative builds a COMPLETE_FILE node whose `eval` writes and loads the
**current** file's compiled image (§4.4) and calls `Lexer::close_file` (pop back to the includer). The reduction happens
with END_INCLUDE (or end of file) as lookahead, so the file switch occurs exactly at the file boundary. Consequence:
items that precede an `include` line in a file (e.g. `labels {…}` in `ai/scripts/defensive.bhs`) are reduced into the
included file's `file_items`; they generate no code, so this only matters for where their symbols live (the includer's
table: symbols are added by the parser actions at the time they are parsed, to the then-current file table).

## 2. Syntax

### 2.1 Grammar

Reconstructed from the LALR tables (`yyr1`, `yyr2`, `yypact`, `yydefact`, `yypgoto`, `yydefgoto`, `yytable`, `yycheck`;
393 reachable states = 392 LR(0) states plus bison's final state, 221 rules, 90 terminals, 84 nonterminals): every LR state has a unique accessing symbol, so a
rule's right-hand side is the accessing symbols of any path of `yyr2[r]` states into a state that reduces it
(`reconstructGrammar` in `bhs-parse.mjs`). Nonterminal names are ours; rule numbers are the binary's (they are the
`case` labels of the action switch in `yyparse`). Token names: `IDENTIFIER INT_LIT FLOAT_LIT CHAR_LIT STRING_LIT`,
`SCRIPT_NAME SCRIPT_KIND VAR_NAME CONST_NAME TRIGGER_NAME TYPE_NAME VOID` (§1.4), operators as in §1.2.

```
string_list   : STRING_LIT | string_list STRING_LIT                                          (1-2)
literal       : CONST_NAME | INT_LIT | FLOAT_LIT | CHAR_LIT | string_list
              | LOCALIZE '(' string_list ')'                                                 (3-8)
var_ref       : VAR_NAME                                                                     (9)
primary       : VAR_NAME | literal | type '(' expr_list ')' | '[' expr_list ']'
              | '(' expr ')' | ERROR_TOKEN                                                   (10-15)
array_index   : postfix '[' expr ']'                                                         (16)
field_access  : postfix '.' IDENTIFIER                                                       (17)
incdec_target : var_ref | array_index | field_access                                         (18-20)
postfix       : primary | call | array_index | field_access                                  (21-24)
unary         : postfix | unary_op unary | '(' type ')' unary                                (25-27)
unary_op      : '-' | '~' | '!'                                                              (28-30)
pow_expr      : unary | pow_expr POW_OP unary                                                (31-32)
mul_expr      : pow_expr | mul_expr ('*' | '/' | '%') pow_expr                               (33-36)
add_expr      : mul_expr | add_expr ('+' | '-') mul_expr                                     (37-39)
shift_expr    : add_expr | shift_expr (LEFT_OP | RIGHT_OP) add_expr                          (40-42)
rel_expr      : shift_expr | rel_expr ('<' | '>' | LE_OP | GE_OP) shift_expr                 (43-47)
eq_expr       : rel_expr | eq_expr (EQ_OP | NE_OP) rel_expr                                  (48-50)
bitand_expr   : eq_expr | bitand_expr '&' eq_expr                                            (51-52)
bitxor_expr   : bitand_expr | bitxor_expr '^' bitand_expr                                    (53-54)
bitor_expr    : bitxor_expr | bitor_expr '|' bitxor_expr                                     (55-56)
and_expr      : bitor_expr | and_expr AND_OP bitor_expr                                      (57-58)
or_expr       : and_expr | or_expr OR_OP and_expr                                            (59-60)
cond_expr     : or_expr | or_expr '?' expr ':' cond_expr                                     (61-62)
expr          : cond_expr                                                                    (63)
assign_expr   : expr | VAR_NAME assign_op expr | array_index assign_op expr
              | field_access assign_op expr | VAR_NAME REF_ASSIGN var_ref
              | incdec_target INC_OP | incdec_target DEC_OP | INC_OP incdec_target
              | DEC_OP incdec_target                                                         (64-72)
assign_op     : '=' | MUL_ASSIGN | DIV_ASSIGN | MOD_ASSIGN | ADD_ASSIGN | SUB_ASSIGN
              | POW_ASSIGN | LEFT_ASSIGN | RIGHT_ASSIGN | AND_ASSIGN | XOR_ASSIGN | OR_ASSIGN  (73-84)
static_kw     : STATIC                         ref_kw : REF                                  (85-86)
type          : TYPE_NAME | ARRAY '<' type '>' | type '[' ']'                                (87-89)
opt_type      : type | VOID | /*empty*/                                                      (90-92)
declarator    : IDENTIFIER | declarator '[' ']'                                              (93-94)
init_declarator : declarator '=' assign_expr | declarator '=' init_declarator
              | declarator REF_ASSIGN var_ref | declarator REF_ASSIGN init_declarator        (95-98)
declarator_item : init_declarator | declarator | declarator '(' expr ')'                     (99-101)
finished_declarator : declarator_item                                                        (102)
declarator_list : finished_declarator | declarator_list ',' finished_declarator              (103-104)
declaration   : type declarator_list ';' | static_kw type declarator_list ';'
              | static_kw declarator_list ';' | init_declarator ';'                          (105-108)
struct_decl   : struct_def ';'                                                               (109)
struct_def    : STRUCT IDENTIFIER struct_open member_decls '}'
              | STRUCT TYPE_NAME struct_open member_decls '}'                                (110-111)
member_decls  : member_decl ';' | member_decls member_decl ';'                               (112-113)
member_decl   : opt_type member_declarator | member_decl ',' member_declarator               (114-115)
member_declarator : declarator                                                               (116)
param         : type declarator | ref_kw type declarator | ref_kw declarator | declarator    (117-120)
param_item    : param | param '=' literal                                                    (121-122)
param_list    : param_item | param_list ',' param_item                                       (123-124)
opt_params    : '(' param_list ')' | '(' ')' | /*empty*/                                     (125-127)
arg_list      : expr | /*empty*/ | arg_list ',' expr                                         (128-130)
expr_list     : expr | /*empty*/ | expr_list ',' expr                                        (131-133)
script_item   : stmt_or_decl | trigger_def | labels_block | struct_decl                      (134-137)
stmt_list     : stmt_or_decl | stmt_list stmt_or_decl                                        (138-139)
stmt_or_decl  : statement | declaration                                                      (140-141)
for_init      : declaration | expr_stmt                                                      (142-143)
statement     : expr_stmt | switch_stmt | if_stmt | loop_stmt | jump_stmt | toggle_stmt      (144-149)
case_list     : case_clause | case_list case_clause                                          (150-151)
case_clause   : case_label stmt_list | case_label                                            (152-153)
case_label    : CASE literal ':' | DEFAULT ':'                                               (154-155)
script_items  : script_item | script_items script_item                                       (156-157)
body          : block_open opt_script_items block_close | statement                          (158-159)
opt_script_items : script_items | /*empty*/                                                  (160-161)
expr_stmt     : ';' | assign_expr ';' | error ';'                                            (162-164)
if_stmt       : IF '(' expr ')' body | IF '(' expr ')' body ELSE body                        (165-166)
switch_stmt   : switch_head block_open case_list block_close                                 (167)
switch_head   : SWITCH '(' expr ')'                                                          (168)
loop_mark     : /*empty*/                                                                    (169)
loop_stmt     : WHILE '(' expr ')' loop_mark body
              | DO loop_mark body WHILE '(' expr ')' ';'
              | FOR '(' for_init expr ';' ')' loop_mark body
              | FOR '(' for_init expr ';' assign_expr ')' loop_mark body                     (170-173)
jump_stmt     : CONTINUE ';' | BREAK ';' | RETURN ';' | RETURN expr ';'                      (174-177)
trigger_head  : TRIGGER IDENTIFIER | TRIGGER                                                 (178-179)
run_once_head : RUN_ONCE                                                                     (180)
trigger_def   : trigger_head '(' expr ')' body | trigger_head '(' ')' body
              | run_once_head body                                                           (181-183)
toggle_kw     : ENABLE_TRIGGER | DISABLE_TRIGGER                                             (184-185)
trigger_ref   : IDENTIFIER | STRING_LIT                                                      (186-187)
toggle_stmt   : toggle_kw '(' trigger_ref ')' | toggle_kw '(' TRIGGER_NAME ')'               (188-189)
program       : file_seq | or_expr EVAL_END                                                  (190-191)
file_seq      : file_items | file_seq END_INCLUDE file_items                                 (192-193)
file_items    : file_item | file_items file_item | error                                     (194-196)
file_item     : script_def | script_fwd | labels_block | struct_decl                         (197-200)
call          : postfix '.' IDENTIFIER '(' arg_list ')' | postfix '.' SCRIPT_NAME '(' arg_list ')'
              | IDENTIFIER '(' arg_list ')' | SCRIPT_NAME '(' arg_list ')'                   (201-204)
block_open    : '{'            struct_open : '{'            block_close : '}'                (205-207)
script_scope  : /*empty*/                                                                    (208)
script_def    : script_head '{' opt_script_items '}'                                         (209)
script_fwd    : script_head ';' | error ';'                                                  (210-211)
script_head   : opt_type SCRIPT_KIND IDENTIFIER script_scope opt_params
              | opt_type SCRIPT_KIND script_scope opt_params
              | opt_type SCRIPT_KIND SCRIPT_NAME script_scope opt_params                     (212-214)
labels_block  : LABELS '{' label_list '}' | LABELS '{' label_list ',' '}' | LABELS '{' '}'   (215-217)
label_list    : IDENTIFIER | IDENTIFIER '=' literal | label_list ',' IDENTIFIER
              | label_list ',' IDENTIFIER '=' literal                                        (218-221)
```

`program : or_expr EVAL_END` is the debugger's expression evaluator (LEXER_EVAL). Error recovery is bison's standard
(`error` tokens in rules 164, 196, 211; three tokens shifted before the next report).

### 2.2 Operators

Precedence (high → low), all binary operators left-associative: postfix (`a[i]`, `a.f`, calls) · unary `- ! ~` and
cast `(type)` · `^` (power) · `* / %` · `+ -` · `<< >>` · `< > <= >=` · `== !=` · bitwise `& ^ |` (unreachable: the
scanner turns `&` `|` into the logical operators and `^` into power) · `&&` · `||` · `?:` (right). Assignment is a
statement form (`assign_expr`), not an expression: it cannot appear inside another expression; `++`/`--` likewise.

### 2.3 Program structure

A file is a sequence of **script definitions**, forward declarations, `labels` blocks and `struct` definitions. There
are no global variables outside scripts. A script header is `[type|void] KIND [name] [(params)]` where KIND is one of
the three script kinds:

```
int ai economic (int who, ref int step, int boom_vs_rush, int num_loops) { … }   // AI production script
scenario { labels {…}  static x = 1;  run_once {…}  trigger Name (cond) {…}  … }  // scenario script, unnamed
conquest ctw_setup_thing (string tribe) { … }                                     // Conquer the World helper
```

- An unnamed script takes the file name without directory and extension (`SymTable::add_script_type` @9d97e0).
  A missing return type means `int` (rule 92). **Scripts can only call scripts of the same kind** (corrected
  2026-10-13: `LocalScriptType::accepts_params` compares `script_type`, so a `scenario` body cannot call an `ai`
  helper; the call does not resolve). Builtins are callable from any kind.
- Parameter defaults (`int f(int a = 3)`, PARAM_DEFAULT) are parsed but **dropped**: the node's eval has no case for
  them, the parameter is an ordinary variable and callers must pass every argument.
- Type, struct and variable names share the case-insensitive symbol tables: `S s;` declares nothing new, `s` resolves
  to the struct type `S`.
- `script_fwd` (`head ;`) declares; a later definition with the same name and parameter types implements it
  (`add_script_type` @9d9920 compares with `LocalScriptType::equals`; different parameter types create an overload).
- Inside a body, **script items** are statements, declarations, `trigger`/`run_once` blocks, `labels` and `struct`.
  Triggers may appear in any script but are only meaningful in the per-frame scenario script (§6.2).
- `labels { A, B = 5, C }` defines integer constants (EnumConstType → ConstType): an unvalued label is previous+1, the
  first unvalued label is **1** (not 0); after a float label the next unvalued label repeats the float; after a string
  label it restarts at 1 (`SymTable::add_enum_type` @9d9ad0). Labels live in the file table (visible to later code in
  the file and to includers).
- `struct Name { type a, b; type c; };` (only `game_structs.bhs` defines structs: `Vector`, `UnitGroup`,
  `ConquestDiploOffer`). Members are declared without initialisers.

### 2.4 Declarations, scopes and symbols

Symbol tables (`SymTable`, 0x58 bytes) form a tree: root (type 0) → one table per file (type 1) → per script table
(created by `script_scope` before the parameters, parent+1) → one table per `{…}` block (`block_open`, parent+1) and
per `case`/`default` clause (rules 154/155). Struct bodies get a type-0x10000 table (`struct_open`). Lookup is from
the current table outward (§1.4), so inner declarations shadow outer ones; a name declared in a block is gone after
its `}`.

The parser actions that add symbols (the scanner sees them immediately afterwards):

| Construct | When the symbol is added | Where |
|---|---|---|
| variable declarator | at the end of each declarator (`finished_declarator`, rule 102 → FINISH_DECL), so `int a = 1, b = a;` sees `a` | current table |
| implicit declaration `x = expr;` (rule 108: `init_declarator ;` with an undeclared x) | at the `;` | current table |
| parameter | at each `param` (PARAM_DECL) | script table |
| struct member | rule 116 | struct table |
| script function | at the end of the header (rules 212–214), before the body (recursion works) | file table |
| struct type | at the closing `}` (rule 110/111) | file table |
| label | each label | file table |
| trigger | at `trigger [name]` / `run_once`, before the condition | the script table |

Declaration state: `curr_type` and `curr_scope` (0, STATIC 0x11d, REF 0x11e) are set by the leading `type`/`static`/
`ref` and **reset to `void` and 0 after every declaration statement** (rules 105–108, 121–122). A declarator created
while `curr_type` is `void` is an **auto variable**: INIT_DECL gives it the initialiser's type (error "can't determine
type" if that is void too); without an initialiser it becomes `int`. So `static first_flag_x = 58;` is a static int and
`right = get_objectives_right();` declares an int local. Parameters without a type are `int`.

Indices (`VarType::init` @9da870): locals and parameters get `script_table.var_counter++`, statics
`script_table.static_var_counter++` (the per-script static slot), struct members `struct_table.var_counter++`.
Indices are allocated when the `declarator` is reduced, in source order, including declarators that never get added
(the inner `b` of `int a = b = 5;`). All locals of a script, in every nested block, share one index space.

`ref` parameters and `ref` locals bind to the caller's object instead of copying (INIT instead of INIT_COPY, §3.3);
`x => y` / `x = ref y` rebinds an existing variable (REF_ASSIGN; error if `x` is itself a ref parameter).

### 2.5 Types and conversions

Simple types: `int` (i32), `float` (f32), `string` (UTF-16 String), `bool` (an int-sized truth value: comparison and
`&&`/`||` results), `void`. Arrays: `T[]` or `array<T>` (`SymTable::add_array_type`; unique token `T[]`, type id its
hash); nested arrays allowed. Structs: named member lists. Values of complex types are reference objects at run time
(§5.1).

Implicit conversion (`SymType::casts_to` @9dbd10, source → target):

| source \ target | int | float | string | bool | other |
|---|---|---|---|---|---|
| int | same | cast | cast | **free** (no op) | ✗ |
| float | ✗ (explicit only) | same | cast | ✗ | ✗ |
| string | ✗ | ✗ | same | ✗ | ✗ |
| bool | ✗ | ✗ | ✗ | same | ✗ |
| array/struct | identical type only | | | | |

"cast" = the compiler appends `CAST target` (or, if the operand is a constant, re-types the constant at compile time:
`float f = 5;` pushes a float constant 5). ✗ needs an explicit `(type)expr`, which is allowed between any two simple
types (`CAST`). `int(x)` is **not** a cast: `type '(' … ')'` only constructs structs (or an empty array).

Operator acceptance (`SymType::accepts_op` @9dbca0, by opcode number): int and bool accept every arithmetic, compare,
logical and bit op (< 0x26); float accepts ops < 0x19 (no `%`, shifts, bit ops, `%=`); string accepts ops < 0x0c
(`= == != += + < > <= >= ! && ||`); arrays and structs only `= == !=`; void nothing.

Binary operators (`SyntaxNode::eval_binary_op` @9df890) first try to convert the right operand to the left's type, then
the left to the right's; the result type is the type converted to (comparisons: `bool`). So `int + float` is float,
`"a" + 5` is string, `5 + "a"` is string as well (left converted).

### 2.6 What the shipped scripts use

`bhs-parse.mjs --features` counts rule reductions over the 365 files (main-file text only). Never used: char literals,
struct constructors `T(…)`, `~`, `^` (power), `<<`/`>>`, `?:`, `=>`/`= ref`, pre-decrement, `*= %= ^= <<= >>=`,
`array<T>`, chained/ref initialisers, sized array declarations `T a(n)`, struct redefinition, multi-member lines,
untyped `ref` params, parameter defaults, empty `[]`, structs inside scripts, **`continue`**, method calls on script
functions. Used heavily: calls to identifiers (39,473 call sites), `if` (10,596), `for` (1,442: all with a step),
`do…while` (208), `while` (88), `switch` (336, `break` inside switch 1,758), `trigger` (1,554) and `run_once` (183),
`enable_trigger`/`disable_trigger` (2,071, all but 4 with a string name), `static` declarations (2,433), implicit
declarations (2,870), array initialisers `[…]` (448), method-style calls `x.f(…)` (492), casts (140), float literals
(15), `return expr` (790). This is the subset a first port must get exactly right (§9).

## 3. Code generation

### 3.1 Nodes

Node types are the PDB enum `SyntaxTypes` (0x140 NOP_TOKEN … 0x173) plus token codes reused as node types (0x106
INC_OP, 0x10a–0x111 comparisons/logic/ref-assign, 0x120 script body, 0x122 variable, 0x123 constant, 0x124 trigger,
0x12f eval, 0x133 case clause, 0x135 if, 0x137 switch, 0x138 while, 0x139 do, 0x13a for, 0x13c continue, 0x13d break,
0x13e return). `Compiler::init_new_node` @9beb60 is variadic (type, symbol, child count, children…) and calls
`SyntaxNode::eval` @9dc710 at once, which sets the node's code list and result type. Child order per rule (from the
disassembly of the action switch, since the variadic children are invisible in the decompile):

| Rule(s) | Node (children) |
|---|---|
| 9, 10 / 11 | VAR(sym) / CONST(sym) |
| 12 / 13 | STRUCT_INITER(type; [expr_list]) / ARRAY_INITER([expr_list]) |
| 16, 17, 24 | ARRAY_INDEX(postfix, index) · STRUCT_ACCESS(name; postfix) · STRUCT_GET(field_access) for reads |
| 26, 27 | unary op node (operand) — '-' UNA_NEGA 0x162, '~' UNA_TILD 0x163, '!' UNA_NOT 0x164 · CAST(type; operand) |
| 32–60 | binary node (left, right): POW_OP, MUL, DIV, MOD, ADD, SUBT, LEFT_OP, RIGHT_OP, LESS, GREA, LE_OP, GE_OP, EQ_OP, NE_OP, AND_BIT, XOR_BIT, OR_BIT, AND_OP, OR_OP |
| 62 | COND_EXP(cond, a, b) |
| 65–72 | ASSIGN_TO_VAR(var; expr) · ASSIGN_TO_ARRAY_INDEX(index, expr) · ASSIGN_TO_STRUCT_FIELD(field, expr) · REF_ASSIGN(var; var_ref) · INC_OP_POST/DEC_OP_POST/INC_OP/DEC_OP(target). The assignment operator travels in `Compiler::last_op` (0 ASSIGN, 0xc MUL_ASSIGN, 0xd DIV_ASSIGN, 0x19 MOD_ASSIGN, 3 ADD_ASSIGN, 0xe SUB_ASSIGN, 0x18 POW_ASSIGN, 0x1a/0x1b shifts, 0x1c–0x1e and/xor/or) |
| 95–102 | INIT_DECL(var; init) · NON_INIT_DECL(var) · ARRAY_SIZED_DECL(var; size) · FINISH_DECL(decl) |
| 103/104, 123/124, 138/139, 150/151, 156/157, 194/195 | GENERIC_LIST: children appended with code concatenated in order |
| 128–133 | arg_list / expr_list GENERIC_LIST — **arg lists keep children unlinked** (the call orders them); expr_list links each new child's code **before** the existing code (elements end up in reverse order) |
| 117–122 | PARAM_DECL(var) · PARAM_DEFAULT(const; param) |
| 152–155 | CASE(header[, stmts]) · CASE_HEADER(const or none) |
| 160/161, 163 | STMT_BLOCK([list]) · STMT(assign_expr) |
| 165/166, 167, 170–173 | IF(c, then[, else]) · SWITCH(head, cases) · WHILE(c, body) · DO(body, c) · FOR(init, c[, step], body) |
| 174–177 | CONTINUE · BREAK · RETURN([expr]) |
| 181–183, 188/189 | TRIGGER(trigger; [cond,] body) · TRIGGER_TOGGLE(toggle) |
| 192/193 | COMPLETE_FILE(file_items) |
| 201–204 | FUNC_CALL(name or script; args[, object]) |
| 209 / 210, 212–214 | script body 0x120(header, items) / SCRIPT_DECL(header) · SCRIPT_HEADER(script; params) |

### 3.2 Representation

`OpCode::get_new` @9c3140 (op in ECX) and `get_line_new` @9c2d50 (op in EDX, explicit line) create instructions with
two operands, each `{u8 kind, u32 value}`: kind 0 none, 1 symbol (encoded per symbol kind at write time), 2 jump target
(another OpCode; encoded as its address), 3 immediate. Line = the lexer's current line unless given. `OpCode::prepend`
splices a list in front. MARKER (0x46) is an empty placeholder (jump targets, empty blocks; never written). When a
file completes, `OpCode::count_code` @9c2ed0 assigns each instruction's byte address (`count`) and the line table, then
`write_code` emits opcode byte + operands (§4.1).

### 3.3 Patterns

Notation: `[x]` = code of x; addresses are labels. Every expression leaves exactly one value on the stack.

**Values and expressions**

| Construct | Code |
|---|---|
| variable / constant | `PUSH v` (pushes the object itself, not a copy) |
| binary `a op b` | `[a] [b] OP` (casts inserted after the converted operand) |
| `-a`, `!a` | `[a] UNA_NEGA` / `[a] UNA_NOT` |
| `a && b` / `a \|\| b` | `[a] JUMP_IF_SC_FALSE L [b] L: CAST_BOOL` / `JUMP_IF_SC_TRUE`. Chains share one CAST_BOOL: a left operand that already ends in this node type's CAST_BOOL donates it, and an operand's own trailing CAST_BOOL becomes a MARKER |
| `c ? a : b` | as `if` with values (🟡 unused) |
| `(T) a` | `[a] CAST T` (or the constant re-typed, or nothing when already T) |
| `a[i]` read | `[a] [i] PUSH_ARRAY_INDEX`; when `a` itself ends in PUSH_ARRAY_INDEX (nested), that op becomes CREATE_ARRAY_INDEX |
| `s.f` read | direct member: `[s] PUSH_STRUCT_FIELD idx`; any field of an **array** value: `[a] PUSH_ARRAY_LENGTH` (so `a.length`, `a.size` all mean length); otherwise a call to builtin `get_<StructName>_<field>(s)` (`resolve_field_func_call` @9dc470, pattern `get_$NAME0_$FIELD1`) |
| `[e0, e1, …, en]` | `[en] … [e1] [e0] CREATE_ARRAY_INITER n, T[]` (all elements must have the first element's type exactly; empty is an error) |
| `S(e0, …)` | `[en] … [e0] CREATE_STRUCT n, S` (each element's type must equal the member's); `S()` = S's create sequence |
| shifts, bitwise, `~` | **no code at all** — the node falls to `eval`'s default case and keeps no instructions, which also drops its operands' code (🟡; unreachable or unused, §2.6) |

**Assignments and statements** (statement value is discarded by `optimize_pop` @9dbdd0: a trailing PUSH is turned into
a MARKER, a MARKER preceded by PUSH loses the PUSH, otherwise a POP is appended)

| Construct | Code |
|---|---|
| `v op= e` | `[e] PUSH v OP` then POP (assign ops leave the target on the stack) |
| `a[i] op= e` | `[e] [a] [i] CREATE_ARRAY_INDEX OP` (writes grow the array, §5.4) |
| `s.f op= e` | direct member: `[e] [s] PUSH_STRUCT_FIELD idx OP`; array `.x = n`: `[n] [a] SET_ARRAY_LENGTH`; otherwise builtin `set_<Struct>_<field>(s, e)` (compound ops wrap a `get_` call; 🟡 unused) |
| `x => y` | `PUSH y INIT x PUSH x` (the PUSH x is removed as a statement value) |
| `v++` / `++v` | `[v] INC_OP_POST` / `[v] INC_OP`; at run time a following POP is consumed (§5.4) |
| expression statement | `[e]` + POP rules above |
| `;` | MARKER |

**Declarations**

| Construct | Code |
|---|---|
| `T v;` | `<create T> INIT v` where create = `CREATE_SIMPLE T` for simple types, `<create elem> CREATE_ARRAY T[]` for arrays (an empty array whose growth template is the element), structs: `StructType::get_create_opcode` (🟡 unused) |
| `T v = e;` | `[e] INIT_COPY v` (copy of the initialiser) |
| `ref T v = e;` | `[e] INIT v` (binds the same object; types must be identical) |
| `T v(n);` | `<create elem> [n] CREATE_ARRAY_DYN T[] INIT v` (🟡 unused) |
| `static …` | the above wrapped as `JUMP_IF_INITED v, L` … `L: MARKER`, so the initialiser runs only while the static slot is empty |
| parameter | `INIT_COPY p` (`INIT p` for `ref`), in parameter order, after the SCRIPT_MARKER |

**Control flow** (`L_brk`/`L_cont` are MARKERs pushed by `Compiler::set_loop_ops` @9becd0 at `loop_mark` and
`set_switch_ops` @9bec20 at `switch_head`)

```
if (c) A            [c] JUMP_IF_NOT L  [A]  L:
if (c) A else B     [c] JUMP_IF_NOT E  [A]  JUMP L  E: [B]  L:
while (c) A         L_cont: [c] JUMP_IF_NOT L_brk  [A]  JUMP L_cont  L_brk:
do A while (c);     [A]  L_cont: [c] JUMP_IF Abegin  L_brk:
for (i; c; s) A     [i]  JUMP C  S: [s] POP  C: [c] JUMP_IF_NOT L_brk  [A]  JUMP S  L_brk:
for (i; c;) A       [i]  C: [c] JUMP_IF_NOT L_brk  [A]  JUMP C  L_brk:
break / continue    JUMP L_brk / JUMP L_cont (innermost); outside any loop: ERROR_TOKEN + compile error
return [e]          [[e] (cast to the return type)] RETURN
switch (e) {        [e]  CASE k1, B1  CASE k2, B2 …  (POP; JUMP Bdefault | POP; JUMP L_brk)
  case k1: X        B1: [X]  B2: [Y] …  L_brk:
```

Conditions must convert to `bool` (`casts_to(bool) ≠ 2`), i.e. int or bool; no CAST_BOOL is emitted for `if`/loops,
the jump tests truthiness directly. In `switch`, case constants are cast to the switch type, duplicates are a compile
error (constants are pooled, so identity compare works), `case` after `default` is an error; the dispatch table is the
CASE instructions in source order followed by the default's `POP; JUMP` (or a synthesised `POP; JUMP L_brk`).

Bugs to reproduce or reject (none affect shipped scripts): `for` never pops `cont_ops`, so `continue` inside a `for`
jumps to a MARKER that is not in the instruction stream (its `count` is its creation number, a garbage address) and
the leaked entry retargets later `continue`s; `default:` not last makes later cases unreachable (and is an error anyway).

**Triggers** (`trigger` keyword; t = the trigger's index in its script)

```
trigger T (c) B     JUMP_IF_BITSET t, L  [c] JUMP_IF_NOT L  BIT_SET t  [B]  L:
trigger T () B      JUMP_IF_BITSET t, L  BIT_SET t  [B]  L:
run_once B          (same as an unnamed trigger without condition)
enable_trigger(T)   BIT_UNSET t        disable_trigger(T)   BIT_SET t
```

Trigger bits are "enabled" flags, all set when the script file loads (`ScriptFile::load_script_info`). JUMP_IF_BITSET
**skips** the trigger when its bit is clear; BIT_SET **clears** the bit (disables) and BIT_UNSET sets it — the names are
inverted relative to their effect. A trigger therefore fires at most once until re-enabled; `run_once` fires on the
first run. `enable_trigger("name")` with a string or a not-yet-declared name creates a toggle symbol `?name`
(`SymTable::add_trigger_toggle`) whose operand is resolved by name at `count_code` (`OpCode::resolve_trigger_toggle`);
an already-declared TRIGGER_NAME gives an immediate index.

**Script prologue and epilogue** (script body node 0x120, `SCRIPT_HEADER` 0x16c)

```
SCRIPT_MARKER s  INIT_COPY p0  INIT_COPY p1 …  [body]  [CREATE_SIMPLE R]  RETURN
```

The final `CREATE_SIMPLE R; RETURN` is added when the body's last instruction is not RETURN: for a non-void simple
return type R the script returns a default value (warning "missing return"); for void just RETURN. The script's entry
`offset` is the SCRIPT_MARKER's address. Script index `s` = the file table's `script_counter++` at the end of the body
(definition order).

### 3.4 Calls

`FUNC_CALL` (`SyntaxNode::resolve_func_call` @9dc030): candidates are the script-function overload chain with that
name, then the builtin overload chain (`ScriptGameInterfaceBase::find_func` @9d5360, case-insensitive). Each candidate's
`accepts_params` returns 0 exact, 1 convertible, 2 no. An exact match wins (two exact matches: "ambiguous"); else a
single convertible match is used with argument casts; else "wrong arguments"/"unknown function" errors. Method syntax
`x.f(args)` passes `x` as the first argument when `x` is a variable of array or struct type (`Array<SyntaxNode*>::insert`),
otherwise it is an error.

`SyntaxNode::set_op_call_to` @9dbee0 then emits (args are prepended one by one, so **argument 0 ends up on top**):

```
script in this file      [argN-1] … [arg0] CALL s                       (usage LINKED)
script in an include     [argN-1] … [arg0] CALL_INCLUDE s, link         (LexerFileEntry::add_link; usage INCLUDED)
builtin                  [defaults…] [argN-1] … [arg0] CALL_GAME f
builtin with varargs     … CALL_GAME_VARIED f, nargs                    (ScriptFunc.flags & 1)
```

Missing trailing arguments of a **script** function are filled with `PUSH default` constants from its
`param_defaults`, pushed deeper than the given arguments. `ScriptFunc::make_params` has the same code for builtins, but
every builtin parameter's `default_value` is empty, so `accepts_params` already rejects a short argument list (§7.1);
builtins effectively have no defaults. `f` is the builtin's global index (`ScriptFunc` +0xc, its position in
`ScriptGameInterfaceBase::functions`, §7). The result type is the callee's return type; void calls push nothing.

### 3.5 Indices and pools

- Constant pool: `ConstType` symbols get their pool index when the image is written (`ConstType::write` @9db920,
  `root.var_counter++`, reset per file), in **symbol-table order**: the tables of included files first (recursively, in
  include order), then the file's own table in insertion order. Insertion order = first occurrence while parsing, which
  includes compile-time re-typed constants and builtin default constants. Operand = index | 0x20000000.
- Variables: operand = the VarType index; statics get 0x40000000 OR-ed in by `VarType::write` (which runs before the
  code is written).
- Triggers: `TriggerType` index = `cur_script.trigger_counter++` at the trigger head.
- Scripts: index per file (§3.3). Builtins: global index (§3.4).

## 4. Bytecode and the compiled image

### 4.1 Encoding

Each instruction is one opcode byte followed by 0, 4 or 8 operand bytes (little-endian u32 each). Operand bytes are
written only for operands that are present (`OpArg::write_arg` @9c27d0): symbol operands write the variable/script/
trigger index, `const index | 0x20000000`, or for type symbols the **type id**; jump operands write the target's byte
address within the file's code; immediates write the value. A symbol operand with a null symbol writes nothing.
MARKER is not written. Addresses are absolute offsets into the file's code block.

### 4.2 Opcodes (`OpCodeTypes`, VM `VirtualMachine::execute_next` @9e0840)

`v` = variable/static/const operand, `T` = type id, `@` = jump address, `n` = immediate. Stack effects are top-last.

| # | Op | Operands | Stack effect and semantics |
|---|---|---|---|
| 0, 3, 12–14, 24–30 | ASSIGN and compound assigns (ADD_ASSIGN 3, MUL 12, DIV 13, SUB 14, POW 24, MOD 25, LEFT 26, RIGHT 27, AND 28, XOR 29, OR 30) | – | `value target → target`: `target.do_operator(op, value)` in place; value freed if temporary |
| 1, 2, 4–8, 10, 11, 15–17, 23, 31–36 | binary (EQ, NE, ADD, LESS, GREA, LE, GE, AND, OR, MUL, DIV, SUBT, POW, MOD, shifts, bit ops) | – | `a b → a.do_operator(op, b)` (new temporary); temporaries a, b freed |
| 9, 22, 37 | UNA_NOT, UNA_NEGA, UNA_TILD | – | `a → a.do_operator(op)` |
| 18, 19 | INC_OP, DEC_OP | – | `t → t` after ±1 in place; if the next byte is POP (0x27) it is consumed and nothing is pushed |
| 20, 21 | INC_OP_POST, DEC_OP_POST | – | `t → copy_of_old_t` then ±1; with a following POP: just ±1 and consume the POP |
| 38 | PUSH | v | push the slot's object (null slot: run-time error "uninitialized") |
| 39 | POP | – | pop; free if temporary |
| 40 | CREATE_SIMPLE | T | push a new default value (0, 0.0, "") (`ScriptType::init_simple`) |
| 41 | CREATE_ARRAY | T | `elem → array` empty array with `elem` as growth template |
| 42 | CREATE_ARRAY_DYN | T | `elem n → array` of n copies (🟡) |
| 43 | CREATE_ARRAY_INITER | n, T | `e_{n-1} … e0 → array[e0…e_{n-1}]` (first pop is element 0) |
| 44 | CREATE_STRUCT | n, S | `m_{n-1} … m0 → struct` |
| 45 | PUSH_ARRAY_INDEX | – | `a i → a[i]` (element object); i < 0 or ≥ length: run-time error |
| 46 | CREATE_ARRAY_INDEX | – | `a i → a[i]`, growing the array to i+1 with copies of its template; i < 0: error |
| 47 | PUSH_STRUCT_FIELD | n | `s → s.member[n]` |
| 48 | PUSH_ARRAY_LENGTH | – | `a → int length` |
| 49 | SET_ARRAY_LENGTH | – | `n a →` resize (shrink frees elements) |
| 50 | INIT_COPY | v | `x →` slot v = copy of x |
| 51 | INIT | v | `x →` slot v = x itself (shared: ref count +1 if x is already a variable) |
| 52 | CAST | T | `x → T(x)` new temporary |
| 53 | CAST_BOOL | – | `x → int (x is null or false ? 0 : 1)` |
| 54 | CALL | s | push a frame for script s of this file |
| 55 | CALL_INCLUDE | s, link | frame for script s of `script_files[linked_files[link]]` |
| 56 | CALL_GAME | f | builtin f with its declared parameter count |
| 57 | CALL_GAME_VARIED | f, n | builtin f with n arguments |
| 58 | CASE | v, @ | compare top (not popped) `== constant v`; equal: pop the switch value and jump; else continue |
| 59 | BREAK | – | debugger breakpoint (the original opcode is restored from `BreakInfo`); never in compiled code |
| 60 | BIT_SET | n | clear trigger bit n (disable) |
| 61 | BIT_UNSET | n | set trigger bit n (enable) |
| 62 | RETURN | – | non-void: the top value is replaced by its copy; end the frame (§5.3) |
| 63 | JUMP | @ | goto |
| 64, 65 | JUMP_IF, JUMP_IF_NOT | @ | pop x; jump if x is true / false (null counts as false) |
| 66, 67 | JUMP_IF_SC_TRUE, JUMP_IF_SC_FALSE | @ | pop x; if x decides the result: push x back and jump; else free x and continue |
| 68 | JUMP_IF_INITED | v, @ | jump if static slot v is non-empty |
| 69 | JUMP_IF_BITSET | n, @ | continue if trigger n is enabled, else jump |
| 70 | MARKER | – | placeholder, never encoded |
| 71 | SCRIPT_MARKER | s | no-op (entry point marker) |
| 72 | ERROR_TOKEN | – | emitted with a compile error; files with errors are not loaded |

Operand presence per opcode follows from this table (the VM advances `bip` accordingly); a decoder can be written from it.

### 4.3 What code a script file produces

One file → one code block containing every script defined in that file (in definition order of their code lists:
the COMPLETE_FILE node's list is the concatenation of the file items). Included files produce their own images and
`ScriptFile`s first.

### 4.4 Compiled image (chunks)

`LexerFileEntry::write_file` @9c05d0 writes a chunk tree (`ChunkWrite`, the BHG chunk format: u32 size including the
header, u16 id, u16 aux; root chunk 0) and loads it immediately with `ScriptFile::read_script_chunk` @9c5440. Order:
chunk 6 links (`write_includes`: count + file names), chunk 7 line table (`OpCode::count_code` → `write_lines`, when the
file defines scripts), `SymTable::write` (scripts 2, constants 3, struct names 9, variable names 8, trigger names 5),
chunk 4 code. A text listing is written to a file only with compiler flag 8 (editor). Nothing is written when the file
had compile errors. 🟡 the debug tables were read from the writers but not exercised.

| Chunk | Payload | Loaded into |
|---|---|---|
| 2 script | i32 index, String name (i32 len + UTF-16), i32 offset, i32 0, i32 return type id, i32 trigger count, i32 param count, per param {i32 type id, i32 is_ref} | `Script` (name, offset, script_type = 0, return_type, trigger bits all 1, params, refs) |
| 3 constant | i32 type id, then i32 / f32 / String | `const_pool` (scope CONST) |
| 4 code | bytes | `ScriptFile.code` |
| 5 trigger | i32 script, i32 trigger index, String name | `Script.trigger_names` |
| 6 links | i32 count, Strings | `linked_file_names` (resolved to `linked_files` indices into `script_files`) |
| 7 lines | i32 count, i32 first-op address per source line (−1 = none) | `line_to_op` (error messages, debugger) |
| 8 variable | i32 script, i32 index (0x40000000 = static), String name | `Script.var_names` / `static_var_names` |
| 9 struct | String name | `ScriptGameInterfaceBase::add_new_type` |

`SymTable::write` @9d8da0 visits tables recursively; variable and trigger chunks carry the table's
`local_script_index`, which is assigned to the script table only when the body completes (node 0x120), while block
tables created during the body copy their parent's index at that time (`add_child_table`): names of block-level
variables are therefore recorded under script 0 or a stale index. 🟡 This matters only for the debug name tables, but
those are saved and checksummed (§4.5). The C++ port keeps this, including `add_child_table` writing the parent's
index into the parent itself. Other image details found by the port: the JUMP_IF_BITSET / BIT_SET of a trigger carry
the line of the `trigger` declaration in the line table; the POP closing a switch's `default` has line 1; an
include file's variables are written in the include's own image only (`clear_children` after writing), not again
in the includer's.

### 4.5 Saved and checksummed state

`RunTimeEnv::walk_data` @9c41a0 walks every `ScriptFile` in `ScriptFile::script_files`: code bytes, scripts
(`Script::walk_data` @9c5f30: static variable values, trigger bits, params, refs, trigger names, variable names, static
variable names, name, offset/return type/script type), the constant pool, `linked_files`; source path, line table,
breakpoints and timestamp only when not checksumming (state-spec.md §5, "Scripts and scenario"). **The compiled code
and the debug name tables are part of the multiplayer checksum and of every save**: a port that wants to load original
saves or stay in sync with original peers must produce byte-identical images (and must execute images loaded from saves,
so the VM has to implement the bytecode, not just the language). Local variables, the run stack and frames are never
saved: scripts run to completion within a frame.

## 5. The virtual machine

### 5.1 Values

Every value is a heap object derived from `ScriptType` (0x10 bytes: vtable, `data_type`, `scope`, `ref_count`) —
`ScriptInt` (i32), `ScriptFloat` (f32), `ScriptString` (String), `ScriptArray` (element pointers + `blank_base`
template), `ScriptObject` (member pointers). `scope` (`VarScope`): 1 TEMP (owned by the stack; freed when consumed),
2 CONST (constant pool), 3 VAR (owned by a variable slot; `ref_count` counts extra bindings), 4 CLEARED, 0x80 array flag.
Virtual slots: +0x0c get_int, +0x10 get_float, +0x14 get_string, +0x18 as array/object, +0x1c duplicate, +0x20 is_false,
+0x24 is_array, +0x2c do_operator(op, other). Objects come from per-class recyclers; there is no garbage collector.

The stack holds object pointers. PUSH pushes the variable's own object, so operators must not mutate their left
operand except the assign family (which is why binary ops allocate temporaries).

### 5.2 Value semantics

- **Run-time types must match exactly**: `do_operator` with a different `data_type` raises "can't use op on types"
  (the compiler's casts normally guarantee this).
- int: C semantics on i32 (wrap-around), `/` and `%` truncate toward zero, division or modulo by 0 → run-time error
  "Can't Divide 0" (result 0, script aborted). `!x` and truthiness are **`x < 1`** (negative numbers are false).
  Comparisons yield ScriptInt 0/1. POW: `(int) powf((float)a, (float)b)`.
- float: IEEE f32 arithmetic (SSE scalar, no x87 extended precision), division by 0.0 is an error, truthiness
  `x <= 0` false, comparisons yield ScriptInt; POW = `(float)(int) powf(a, b)` (truncated!).
- string: `+` concatenates, `+=` appends, `== != < > <= >=` are **case-insensitive** (`_wcsicmp`, C locale: ASCII
  folding only); equality takes a shortcut when both strings carry cached case-insensitive hashes (equal hash and
  length ⇒ equal), so a hash collision compares equal. Truthiness: non-empty.
- Conversions (CAST): int→float exact; float→int truncates; int→string `_itow` decimal; float→string `_gcvt_s(value
  as double, 1014 significant digits)`: the exact decimal expansion of the f32 with `%g` rules and MSVC's trailing
  dot for integral values (🟡 verify, e.g. 2.0 → "2.", 0.1f → "0.100000001490116119384765625"); string→int
  `convert_int` (hex/bin/dec prefixes, `_wtoi`), string→float `String::get_double`.
- arrays: `=` deep-copies elements (`ScriptArray::do_operator` op 0); `==`/`!=` compare element-wise up to the left
  length (`!=` is inverted per element: the first **equal** pair answers 1, "different", and a longer left side
  answers 1; port it as is); INIT_COPY duplicates (temporaries are adopted, not copied).

### 5.3 Execution model

`RunTimeEnv` (`script_run_time` @ebeeb0) owns one run stack and a stack of `VirtualMachine` frames.
`RunTimeEnv::run_script(name, nargs, args…)` @9c4460 / wrapper @43d0e0:

1. `clear()`: reset counters, free the previous return value.
2. `ScriptFile::find_script(name)` @9c6c80: search `script_files` **from the last loaded file backwards**, scripts in
   order, case-insensitive names. Not found → error, return 3.
3. `check_params` @9c3b00: the argument count must equal the script's; the arguments are placed so that argument 0 is
   on top, each checked for its exact type id (error → return 4).
4. Push a `VirtualMachine` (`init` @9e16d0: `bip` = script offset, `expected_stack_size` = stack size − nargs (+1 for a
   non-void script)) and `exec` @9c3600: loop `op = code[bip++]; execute_next(op); bytecodes_executed++` while
   `check_vm_running` @9c39c0 says running.
5. CALL / CALL_INCLUDE push further frames sharing the run stack (`call_script` @9c3840); RETURN (`close_frame` @9c36c0)
   checks that the stack is exactly `expected_stack_size` (error otherwise) and pops the frame; the outermost RETURN ends
   the run with status SUCCESS and `finish` @9c3560 takes the return value from the stack top.

There is **no time slicing, yield, sleep or wait**: every invocation runs to completion inside the calling frame.
"Waiting" in scripts is done with timers and triggers polled on later frames (§6.2). Limits: 0x1000000 (16,777,216)
instructions per `run_script` (counter reset by `clear`) and 65,536 frames; exceeding either is a run-time error. Any
run-time error (`run_time_error` @9c31e0) marks the VM, sets status ERROR, reports through the game interface
(script log), and `exec` returns 6 after `clear()` — the rest of the script is skipped, **side effects already made
stay**. Return codes of `run_script`: 0 success, 2 break (debugger), 3 not found, 4 bad arguments, 6 error.

Builtin calls (`VirtualMachine::call_func` @9e0550): `ScriptFuncSet::call_func` @9d5500 reads the arguments from the
stack top through a `ScriptParamStack`, pops them, and a non-void result is pushed; the stack height is verified.

### 5.4 Determinism notes

- The VM itself uses no randomness and no wall clock; `rand_int`/`rand_real` draw from `game_random` and `rand_seed`
  reseeds it (§7). Iteration orders are all array orders (no hash maps at run time).
- Float: f32 arithmetic and `powf`/`sinf`-family CRT calls (§7) are the only float hazards; shipped scripts use 15
  float literals and no `^`.
- Hash-shortcut string equality (§5.2) depends on whether hashes were cached; port it faithfully (compare cached
  case-insensitive hashes when both exist).
- Scripts that read local-machine state (selection, camera, UI, sound playback) diverge between peers if their result
  feeds the simulation — see the list in §7.
- The scenario script also runs while the game is paused in single player (§6.2): replays must reproduce the pause
  pattern or ignore it (single player only).

## 6. Runtime environments

### 6.1 Script files and compilation

`Compiler::compile(path, reload)` @9bf160: adds the content directory and `.bhs`, skips the compile when that file is
already loaded and unchanged (`ScriptFile::find_script_file` case-insensitive path + `has_changed` timestamps),
otherwise lexes and parses it; every COMPLETE_FILE loads or replaces its `ScriptFile` (`load_script_file` @9c6850:
same source path → replaced in place; else appended to `ScriptFile::script_files` or put in a free slot). Reload modes
(`ScriptReloadType`): FULL (games), MERGE and KEEP_STATE (editor; KEEP_STATE carries static variables over by script
name). When a save is loaded the files come from the save instead (`RunTimeEnv::walk_data`) and nothing is compiled.

### 6.2 Scenario scripts

- Which script: `GameInfo.scenario_script` (the script function's name; for an unnamed `scenario { }` that is the file
  name), compiled in `Setup::build_game` @5ac190 from `GameInfo::get_full_script_path` when the scenario wants a script
  (`ScenarioData::load_scenario_script`, or CTW's own flag); compile failure clears the name and posts a message.
- When: `Game::do_frame` step 5 (frame-spec.md §3), **every simulation frame**, with no arguments, before
  `Leaders::process_all`. Additionally `TurnControl::do_frame_solo` runs it on every loop iteration while the single
  player game is paused, if `load_scenario_script & 0x80`.
- Semantics of one run: the whole `scenario { … }` body executes from the top. Plain locals are re-created each run
  (the VM's variable array is per frame); `static` variables keep their values across frames (their initialisers run
  once, guarded by JUMP_IF_INITED); `run_once` fires on the first run; each `trigger` is polled in source order — if
  enabled and its condition is true it is disabled and its body runs; bodies typically `enable_trigger` themselves or
  others. `labels`/`struct` items produce no code.
- Timers: `set_timer(name, seconds)` / `timer_expired(name)` (`ScriptTimers` in `ScenarioData::timers`, up to 100,
  saved with ScenarioData; semantics in §7).

### 6.3 General powers, scripted rules, CTW setup

- If `setup.game_rules == 10` (semaphore byte1 0x10) or Conquer the World (byte2 0x02), `build_game` compiles
  `ScenarioData::general_powers_script_file` (`scenario/scriptlibrary/general_powers.bhs`) and `do_frame` runs the
  script named after it every frame with `frame > 0`, right after the scenario script.
- CTW battles: `build_game` compiles the territory's setup script (`ConquestGame::get_setup_script`, e.g.
  `conquest/Alexander/thracesetup.bhs`), runs the function named after the file **once** at setup, then
  `World::compute_all_territory`.
- CTW strategic layer (not the RTS simulation): `ConquestLeader::run_strategy_script`, `run_diplo_response_script`,
  `ConquestDiploOffer::make_ai_offer/make_player_offer`, `ConquestGame::find_available_links`,
  `ConquestStyle::get_bonus_amt`, `ConquestStyles::pick_style`, `ConquestWin::post_turn_processing` call scripts from
  `conquest/scripts/` and `scenario/scriptlibrary/ctw_lib.bhs` (out of scope for lockstep; same VM).

### 6.4 AI production scripts

- Compile: `Leaders::init_production_script` @6ed490 (once in `Game::run`, not when loading a save): for leaders 0..7
  that are VALID, not human-without-co-op-AI (`flags & 0xc ≠ 4`) and have a script name (LeaderData +0x6ea4), compile
  `leaders.prod_script_path + name + ".bhs"` (`ai/scripts/economic.bhs`, `defensive.bhs`; both include
  `aibestbuildlibrary.bhs`).
- Run: `Leader::plan_strategy` @6b9620 runs a full strategy pass at frame 0 and when
  `(who·25 + frame) % (200 / ai_speed) == 0`, which ends by setting the production state (LeaderData +0x788) to 1. While
  the state is non-zero, each following call of `plan_strategy` (every frame, `Leaders::strategy_all`) advances
  `Leader::production_ai` @6c1960 by one state: state 1 runs the script (unless LeaderData +0x78c is 0 or
  `starting_resources == 8`), states 2–11 run the C++ production steps (`production_ai_setup`, `found_cities`,
  `research_techs`, `upgrade_units`, `create_units`, `create_buildings`, `make_stuff`).
- Call: `run_script(name, 4, who+1, step, boom_vs_rush, 5)` where `who` = leader index, `step` = a VAR-scoped ScriptInt
  holding LeaderData +0x790 (the script's `ref int step`; written back after the run), `boom_vs_rush` = LeaderData +0x6dd4
  + 2, `num_loops` = 5. Return (labels in the scripts): 1 BLOCK_ON_THIS → state 0 (retry next cycle); 3 SCRIPT_DONE or
  any run failure → +0x78c = 0 (script retired) and continue; 2 DONT_BLOCK_ON_THIS → continue with the C++ steps.

- Script choice (`Leader::init` @6e3930, patch version ≥ 9, leaders that are not a human seat without co-op AI, after
  `random_personality`): `economic` when `boom_vs_rush` (+0x6dd4) < 0, or when it is 0 and
  `(Random::get(game_random, 0, 0xffff) & 0x80000001) != 0` (a **draw during the leader init**), unless the leader has
  tribe bonus 0x13 (Lakota); otherwise `defensive` (and +0x6e20 = 1). `boom_vs_rush` is 1 for GameInfo player flag
  0x1000 or team 1 under team style 3, −1 for flag 0x2000. Same body: +0x78c = 1 (script on), +0x790 = 1 (step).
- Port notes (engine/sim/script_host.cpp): the script file path `ai\scripts\<name>.bhs` stands for
  `leaders.prod_script_path + name + int_str_array[0x169e0]` (the strings sit in an internal table; the result matches
  the install). `production_ai` returns to state 0 at once for human seats (`flags & 4` without `& 8`), `ai_off` and
  `flags2 & 4` (scenario leader file flag 2, e.g. the tutorials' passive AI). States 2-11 call the C++ steps in order
  (2 setup, 3 found_cities, 4 research_techs, 5 upgrade_units, 6 and 9 create_units, 7 and 10 create_buildings,
  8 and 11 make_stuff; after 8 a zero make_stuff returns to state 0; with `starting_resources == 8` every step is
  followed by make_stuff).
- Scenario script path (§6.2): taken here as the scenario file's directory and base name + `.bhs`, run as the script
  named after the base (`GameInfo::get_full_script_path` and `GameInfo.scenario_script` are set by the setup screens;
  not decompiled). 12 conquest battle scenarios set LOAD_SCRIPT without such a file: their scripts come from the CTW
  layer (`ConquestGame`).

## 7. Builtins

Evidence: `re/tools/bhs-builtins.mjs` (generator; reads the gitignored init_funcs/body decompiles, `re/map/functions.tsv`,
the callee closure and the install) → `re/analysis/bhs_builtins.tsv` (one row per registration: set, set/global index,
name, signature, impl, category, RNG, usage, notes). Callee closure: `re/ghidra_scripts/CalleeClosure.java` (new; per-root
callees + referenced globals to depth 4) via `re/jobs/dc_bhsfn_closure.txt` → `re/analysis/bhs_builtin_callees.tsv`.
Vtables: `re/jobs/dc_bhsfn_vt.txt`. Disassembly of timers/utilities: `re/jobs/dc_bhsfn_dis{1,2}.txt` → `re/decomp/bhsfn/asm/`.

### 7.1 Registration, global index, call protocol

- **Totals: 873 registrations, 813 distinct names.** MathUtil 15, TriggerUtil 3, StringUtil 6, ArrayUtil 7,
  ScenarioFuncSet **842** (systems-map's 843 is off by one; `ScenarioFuncSet` has 873 methods in the map = 842 impls +
  29 helpers + ctor/dtor). All 873 impl addresses are distinct (no ICF sharing).
- **Global index = registration order.** `ScriptGameInterface::init` @9e1a20 creates the sets in the order
  MathUtil (0–14), TriggerUtil (15–17), StringUtil (18–23), ArrayUtil (24–30), Scenario (31–872); each
  `init_set` → `init_funcs`. `ScriptFuncSet::add_new_func` @9d4da0 appends the `ScriptFunc` to
  `ScriptGameInterfaceBase::functions` and stores the position in `ScriptFunc.index` (+0xc).
- **OP_CALL_GAME's operand is that global index** (confirmed): `SyntaxNode::set_op_call_to` @9dbee0 emits
  `ScriptFunc.index`; `VirtualMachine::execute_next` passes it to `VirtualMachine::call_func(nargs, f)` @9e0550
  (`nargs = -1` → the function's declared count; CALL_GAME_VARIED supplies it), which indexes `functions[f]` and calls
  `ScriptFuncSet::call_func(set, f, stack)` @9d5500. `Compiler::eval_run` (constant folding) does the same.
  Consequence: compiled images stored in saves (`RunTimeEnv` walk) hard-code these indices, so the port must keep the
  exact registration order of all 873 entries to load original saves.
- **Overloads** (36 names, e.g. `group_*_order`×3, `set_no_attack`×4, `add`×2): `add_new_func` calls
  `ScriptFunc::check_overload` @9d46a0 on the *previous* function once its params are complete: it searches backwards
  over the global table for the nearest earlier same-name function (`String ==`, case-insensitive) and sets that one's
  `next_overload` to it; identical param type lists → registration error. Chains therefore run in registration order
  (the last registered function is never checked). `find_func` @9d5360 returns the first (lowest-index) match
  (case-insensitive) and the compiler walks `next_overload`.
- **Parameters** (`ScriptFunc::add_param` @9d4f20; layout from the PDB: `ScriptParam` 0x48 = name, default_value +0x14,
  validation_type +0x28, sym_type +0x3c, type +0x40, flags +0x44): the 4th argument of `add_param` goes to
  **`validation_type`, not `default_value`**; `default_value` is set to `EMPTY_STRING` for every builtin parameter.
  **Builtins have no defaults** — `accepts_params` @9d4a70 returns 2 (no match) when an argument is missing and
  `default_value` is empty. The only validation
  string is `"active_unit_group"` (11 `group_*_order` overloads, param `g`).
- **Flags**: 4 = by-reference (`accepts_params` requires an exact type match and an argument node of kind 0x122, i.e. a
  variable; 15 params: the ArrayUtil array params except `find`, `get_button_rect` outputs, UnitGroup `add_to_group /
  remove_from_group / clear_group`, `ctw_clear_offer`, `ctw_*_list` / `ctw_get_active_tribes` string[] outputs);
  1 = variadic, set automatically for type 0x64ea2a (only `parse`; a param after it is an error); 2 = pass int/float by
  value instead of by pointer (never set by registrations).
- **Type ids** (case-insensitive hash of the unique token: simple name; arrays `"@"+elem`; structs
  `Name$` + member tokens each followed by `^`, `StructType::get_unique_token` @9db280):
  int 0x57bad, float 0x12f35f, string 0x168174, void 0x84048, bool 0xa6192, **array 0x153c88** ("array", any array),
  **generic 0x27b2c1** ("generic", any value), **string[] 0x20d693** ("@string"), **UnitGroup 0x139fd8d**
  ("UnitGroup$int^@int^"), **ConquestDiploOffer 0x1d0655f3** (its full token), **0x64ea2a = varargs** (token text not
  recovered by hashing ~50k candidate strings; `get_type_name` returns "" for it).
- **`ScriptFuncSet::call_func`** @9d5500: vtable slot 1 (pre-call check) is the folded trivial body in all five sets, so
  it is skipped. Params are processed last to first; param i is stack slot `len-1-i` (arg 0 on top).
  `get_param` @9d43e0: null value → "missing param" run-time error, REJECT; type ≠ declared (unless generic, or array
  param and value is an array) → type-mismatch error, REJECT; if `validation_type` is set, the set's
  `validate_param` (slot 2) decides first. Native argument passing: **int/float → pointer to the VM value's payload**,
  string → `String*`, struct/array → the `ScriptObject*/ScriptArray*` itself, varargs → the `ScriptParamStack*`. Status
  (`ScriptParamProcessStatus`): CUSTOM 0 / STANDARD 3 push one argument; CUSTOM_THIS 1 replaces the C++ `this`;
  SKIP 2 pushes nothing; REJECT 4 → error return; REJECT_CUSTOM 5 → the custom value is the result. The pushed count
  must equal `params_pushed_expected` (`add_new_func`'s 5th arg; equals the script param count for all 873, varargs
  counting as one) else run-time error + error return. Results: string → new ScriptString; int/bool → ScriptInt;
  float → ScriptFloat from ST0; void → nothing; struct → the returned ScriptType.
- **`ScenarioFuncSet::validate_param`** @9e3410 ("active_unit_group"): builds a native `Group` from the script
  `UnitGroup` (`create_game_group` @9e1f40: owner = `nation`, members = valid live units < 2000; **each non-plane member
  gets `close_orders` + `clear_partial_path` + `update_action`**), pushes it with `Groups::push_group(nation-1, …)` and
  passes the `Group*` (CUSTOM); nation invalid → REJECT. `ScriptGameInterface::validate_param` @9e19f0 returns STANDARD.
- **Error returns** (`get_err_return` @9d41d0): int/bool **−1** (written as the bit pattern of −NaN = 0xFFFFFFFF),
  float −1.0, string "", void nothing, struct → run-time error + default struct. Builtin bodies use the same convention
  themselves (−1 / "" on bad who, id, type name).

### 7.2 Argument conventions

| Thing | Convention (evidence) |
|---|---|
| `who` / `whom` / `who_receiver` | **1-based player 1..8** → leader index `who-1`; valid if `< 8` and `LEADER_VALID` (1), most also need `LEADER_ACTIVE` (2) (`valid_who` @9e33c0, `active_who` @9e33e0). 0 = world is never accepted, except "all players" in `set_explored(p,…)` (fog-spec §6.3) |
| object ids (`object_o`, `unit_o`, `build_o`) | **per-player slot** in `objects[who-1]`: units 0..1999 (`valid_unit_o` @9e3360, `active_unit_slot` @9e3160 also requires on-map or a captain), buildings 2000..2999 (`valid_build_o` @9e3300), any < 3000 (`valid_object_o` @9e32a0). An object is always the pair (who, id) |
| city ids | `find_city_id(name)` returns the city's **building id** (City +8, a 2000+ slot of its owner), not an owner; pair it with `find_city_owner` |
| coordinates | **tile** x, y; checked against world tile size (`world+0x18/+0x1c`); converted to the tile centre `t*192+96` Coord. Readers return tiles (`object_position_x` = `div_3_table[(pos ^ 0x63637) >> 6]`, the container's position if garrisoned) |
| type names | case-insensitive (`String::operator==`) linear search of all 806 type slots, first hit: `get_type_index` @a03480 (TypeData name +0x60, or +0xb0 when called with the alternate flag) or the same loop inlined. Then a kind check (unit 0x32..0x19d, building 0x19e..0x21e, tech/gov ranges) |
| upgrades | `*_upgrade` variants map the base type to the player's current line member (`LeaderData::current_upgrade`, `get_graft`; production-spec §6.2.5) |
| resources | type index < 6 (the six goods); stockpiles are XOR 0x8221 (`LeaderDataEncrypt`, economy-spec §0); ages XOR 0x62766 |
| city / territory / tribe / group names | city: case-insensitive vs name +0xa4 or display name +0x90 (`get_city_index` @9e2ba0). Tribe: `Tribes::find`. CTW territory: `conquest_nodes` name +0x7c. Group: `ScenarioData::scenario_groups` (NamedObjectArray, case-insensitive) |
| groups (3 overload families) | (int who) → the named group **`"_Unnamed"+who`** (`default_group` @4ce990); (string name) → that named `ScenarioGroup` (who, unit ids, find counter); (UnitGroup) → the script struct. Orders through families 1/2 use `create_game_group` @9e1dc0 (same order-clearing as above). `create_unit`/`create_unit_upgrade` clear `_Unnamed<who>` and put the new units in it; `create_unit_in_group` appends |
| find_* iteration | stateful round-robin: each `find_*` resumes after the id it last returned, from a slot of `ScenarioData::find_counters[]` (saved; state-spec), wrapping. [1] find_unit and find_military_category (shared), [2] find_build, [3] find_all_military_category, [4] find_idle_citizen, [6] find_idle_military, [9] find_military, [0xb] find_all_military, [0xc] find_city_name, [0xd] city_captured_by (string overload), [0xe] find_capital, [0x10] find_inside_unit, [0x11] find_all_unit, [0x12] find_unit_upgrade, [0x14] find_military_build, [0x16/0x17] find_build_at_city, [0x18] find_inactive_build, [0x1c] train_unit_with_cost, [0x1e] research_tech_with_cost. All 31 start at −1 (`ScenarioFuncSet::init` @a03c30 and `close` @a03650 set them). Repeated calls enumerate; the port must keep the counters |
| orders | applied immediately with `QUEUE_NEW`, with `ScenarioData::ignore_orders` forced to 0 around the `Group::action_*` call (orders-spec §2.1 prelude); return the group's member count or −1 |

### 7.3 Categories (method and counts)

Method (implemented in the generator, reviewed by reading bodies for the top 60 and every borderline name):
utility sets → (e); a `CommandManager::issue_*` / `UnitOut::issue_*` call within closure depth 2 → (b); else a
curated name list → (d) presentation or (e) script environment; else `get_/is_/num_/find_/at_least_/less_than_/have_/
has_/was_/…` names with a non-void return → (a); everything else → (c). Overrides after reading:
`find_extra_starting_loc` → c (appends to `ScenarioData::extra_starting_locs`), `ctw_get_active_tribes` → a,
`pause_game`/`unpause_game` → d, the UnitGroup-struct `add_to_group/remove_from_group/clear_group` → e.

| Category | Registrations | Names used in shipped scripts | Call sites |
|---|---|---|---|
| (a) query | 329 | 220 of 317 | 14,132 |
| (b) lockstep command | 2 | 2 | 9 |
| (c) direct sim mutation | 298 | 202 of 261 | 18,899 |
| (d) UI / presentation / audio / camera | 191 | 107 of 184 | 3,749 |
| (e) script environment | 53 | 20 of 49 | 3,168 |

Key finding: **builtins do not go through the command queue.** Scenario, general-powers and AI production scripts run
inside `Game::do_frame` on every machine (frame-spec §3 steps 5–6; §6 here), so (c) builtins write sim state directly
(`Objects::init_unit`, `Object::die`, `Leader::gain_tech`, `Leader::defeat`, `Cities::capture_city`, `Group::action_*`,
XOR-encrypted stockpiles, `types.list` rules fields, diplomacy tables, `ScenarioData`). Only two issue commands:
`rename_city(who, city_name, new_name)` @9effc0 (`CommandManager::issue_rename_city`; the (who, o, name) overload
@9f00d0 calls `City::set_name` directly) and `unit_eject_order` @9f84e0 (`UnitOut::issue_come_out`). Both pass
`check_accept_issue` @940a70, which refuses during playback (byte0 & 0x10), so a replay re-applies the recorded command
instead; in live MP every machine issues its own copy (see open questions).

(d) is "no sim outcome", not "no saved state": objectives, goals, messages and groups live in `ScenarioData`, which is
saved and checksummed (state-spec §16, replay-spec §11). The UI calls must still update that state identically.

### 7.4 Determinism hazards

- **Multiplayer guard**: 66 bodies test `Game.semaphore byte0 & 4` (multiplayer, frame-spec §3). In all of them except
  `popup_dialog`/`popup_choice` the local-state work only happens in single player (early `return -1`, or the body
  is skipped): `get_console_player`, `*_selected`, `select_*`, `mouse_down`, `get_mouse_*`, `is_game_mouse_near`,
  `key_down/key_released`, `build_*menu_active`, `build_option_selected`, `is_zoomed_*`, `is_on_screen`,
  `is_right_click_action`, camera moves/zooms, `get_camera_x/y`, `get_current_zoom`, `highlight_*`, `*_warning`,
  `sound_finished`, `real_time_millis`, `show_goal_dialog`, `set_instruction/feedback_msg`, `use_basic/advanced_menu`,
  `mouse/hotkey_selection_*`, `pause_game/unpause_game`, `switch_player_control` (also rewrites the console and
  `LEADER_HUMAN`), `set_auto_peasant_level`, `add_flag`. Port: same guard, same −1.
- **Local reads without the guard** (scripts must only use them in single-player content, which is the case in the
  shipped scripts; replays of single-player games still re-run them): `get_object_at_mouse`, `get_object_type_at_mouse`,
  `get_resource_at_mouse`, `is_mouseover_*`, `find_selected_id`, `is_move/rally/patrol/airstrike_selected`,
  `get_button_*`, `get_objectives_*`, `get_screen_width/height`, `is_camera_panning`, `goal_dialog_shown`,
  `get_player_profile_name` (local profile), `get_difficulty` (checks whether the CTW window is visible),
  `play_sound` (return depends on the sound system; busy-waits ≤ 200 ms of `timeGetTime`), `popup_choice` (returns
  the user's button; CTW only, 40+ uses), file builtins (`open_file` resolves under the player-profile directory;
  `file_*`), `print`/`print_line` (`OutputDebugStringW` only).
- **RNG — `game_random` drawn directly** (replay-spec §12; every draw is part of the sim stream):
  `rand_int(lo, hi)` @9e1890 = `Random::get(game_random, lo, hi)` (hi **exclusive**, lo == hi returns lo **without a
  draw**, asserts ≤ 0xFFFF); `rand_real()` @9e18b0 = inline `next()`, `float(0x3F800000 | seed & 0x7FFFFF) − 1`;
  `rand_seed(n)` @9e1900 sets the seed (`n < 0` → `timeGetTime()`, nondeterministic); `rand_get_seed()` @9e1930 reads
  it; `kill_unit` @9f6b20 / `kill_unit_type` @9f6d70: one `get(0,0xFFFF)` per unit killed, death type `r % 3 + 1`;
  `find_extra_starting_loc` @9e5070: one `get(0,0xFFFF) % n` when n > 1.
  **Through AI placement code (depth 1)**: `place_orphan_building*` (`Leader::produce_building`), `place_city_with_cost`
  (`Leader::compute_sites` / `found_cities`, economy-spec §5.9). **Through sim functions (depth 2–4, draws as in those
  specs)**: `place_building*`, `switch_teams`, `switch_building_type`, `create_unit*`, `create_missile_and_target_*`,
  unit/group move/attack/patrol/garrison/flee/explore orders, `gain_tech`, `gain_upgrade`, `city_assimilate`,
  `defeat`, `victory`, `defeat_attacker_and_allies`, `declare_war`, `make_peace`, `make_alliance`, `accept_offer`,
  `reject_offer`, `citizen_repair_order`, `air_unit_transfer_order` (53 builtins in all; column `rng`).
- **`internal_random`**: no builtin draws it directly; only `defeat`, `victory`, `defeat_attacker_and_allies` reach it
  (depth 3–4, end-game presentation). Many mutations reach `SoundGlobal::play` (its own RNG, presentation).
- **Floats**: `sin cos tan asin acos atan` work in **degrees** in f32 SSE (`x*π/180` before, `*180/π` after, π =
  0x40490fdb) and call the CRT `sinf/cosf/tanf/asinf/acosf/atanf`; `sqrt` → `sqrtf`; `min_val/max_val/absl_float`,
  `rand_real`, `set/get_plunder_scale`, `set/get_unit_scale`, `set_arrow_thick` take/return f32. `sin`/`cos` are used
  (12 calls each) in CTW setup scripts to place armies (`(int)(radius * sin(dir) + center)`) → CRT-exact `sinf` needed
  for replay fidelity. `object_health` computes `(int)((float)hp / (float)max * 100)`.

### 7.5 Usage in the 365 shipped scripts

Counts from `re/analysis/bhs_usage.tsv` (`bhs-parse.mjs --usage`, parser-exact call sites); the generator's
comment/string-stripped regex count agrees for every builtin. Directory classes: AI = `ai/scripts` (3 files),
CTW = `conquest/**` (330), scenario = `scenario/**` + `Data/` (32).

- **551 of 813 builtin names are called** (39,957 call sites: AI 817, CTW 32,014, scenario 7,126); 262 are never used.
  Distinct names per class: AI 55, CTW 427, scenario 326; used in only one class: AI 17, CTW 199, scenario 104.
  Method syntax `x.f()` is used with 8 names (`add_to_group` 417×, `group_attack_to_order`, `group_patrol_order`,
  `group_jump_move`, `group_guard_order`, `clear_group`, `remove_from_group`, `group_move_order`, …).
- Overloaded names are counted per name (overload resolution needs argument types).
- **AI scripts use 55 names**, all queries or cost-paying production calls: `num_cities` 83, `get_is_no_nation_powers`
  87, `place_building_with_cost` 63, `num_type_with_queued` 62, `num_city_buildings` 50, `place_building_upgrade_with_cost`
  48, `have_tech` 40, `find_build_at_city` 39, `place_orphan_building_with_cost` 35, `find_inactive_build` 29, `num_type`
  26, `research_tech_with_cost` 24, `find_city_with_num` 24, `can_pay_cost` 23, `age` 19, `find_nation` 19,
  `get_mapstyle` 18, `train_unit_with_cost` 18, … plus `rand_int` 8 and the timers.
  Port notes from the bodies (engine/sim/script_funcs.cpp): `who` is the player number (leader + 1) and most bodies
  require VALID and ACTIVE; type names match `TypeData` names case-insensitively; the `find_*` builtins resume from
  `ScenarioData::find_counters` slots (1 find_unit, 2 find_build, 4 find_idle_citizen, 0x16/0x17 find_build_at_city,
  0x18 find_inactive_build, 0x1c train_unit_with_cost, 0x1e research_tech_with_cost; reset to 0 / 2000 / −1 when
  negative; their initial value is not in the decompiles — −1 assumed, since 0 would make find_build_at_city
  dereference a unit as a building); the searches wrap at `unit_mark` / `build_mark`. Quirks kept: `num_type` of a gaia
  type 402..413 reads `num_queued[t − 402]` (the per-type unit counts end at 401); `num_type` of a good reads the
  encrypted stockpile (`LeaderDataEncrypt + 4t ^ 0x8221`, garbage past the six goods); `was_city_raided` /
  `was_city_attacked` with an unknown city name test every city and count a never-stamped one as recent when
  `seconds ≥ 1`; `can_pay_cost` refuses type 0. `train_unit_*` / `research_tech_with_cost` queue through a one-member
  Group and `Group::action_queue_up` (not the command queue); `train_unit_with_cost` grafts the type
  (`get_graft(current_upgrade(t))`) and picks the next finished building of `current_upgrade(type.where)` (a
  negative building type matches any finished building). `place_*_with_cost` end in `Leader::produce_building` and
  `place_city_with_cost` in `compute_sites` + `found_cities` (AI placement code).
- Top 10 overall: `create_unit_upgrade` 3046/182 files, `create_unit_in_group` 1678/109, `set_timer` 1547/123,
  `place_building_upgrade` 1221/64, `rand_int` 749/93, `object_position_x/y` 717/190, `find_unit` 659/157,
  `give_good` 655/64, `age` 597/52.

### 7.6 Semantics of the most-used builtins

All take/return int unless noted; "−1" = invalid who/id/type/name unless stated. Spec refs are for the wrapped sim code.

| Builtin (calls) | Semantics | Sim function / state (spec) | RNG |
|---|---|---|---|
| `create_unit(who,x,y,type,n)` 548, `create_unit_upgrade` 3046, `create_unit_in_group` 1678 | `ScenarioFuncSet::add_unit` @9e2220: who ACTIVE, `n ≤ 2000`, unit type, tile valid. `_upgrade`/`_in_group` first map type → `current_upgrade`. Clears the default group of the **leader index** (`clear_group(&(who−1))`, i.e. `_Unnamed<who−1>`; not `_in_group`) but adds each new unit to `_Unnamed<who>` (`add_to_group(&who, &o)`, disassembly @9e2340 / @9e243a): the group the `(int who)` order families use is never cleared by `create_unit` (faithful quirk). Transport barges (`is(320)`) are refused. n × (`UnitType::find_nearby_spot(x, y, r0 0, r1 0xc00, step 0, angle 0x55555555, FILTER_NOT_ME)` → `Objects::init_unit`), each added to a local Group; planes get `clear_orders` + a strafe order at terrain z + 1000, carriers their fighter-bombers; ships on land create nothing; land units on ocean are created at the tile centre and `action_transport(1)`-ed (their ids replaced by the transport's). Then (unless planes) `Groups::push_group(who, grp, 0)`, `action_form(0, 0, QUEUE_NEW)` and `Group::dbg_jump_to_action` — which in **single player only** (`semaphore byte0 & 4` clear) teleports the members to where their CHANGE_FORM / move orders lead (`Unit::dbg_jump_to_action` @602ea0). Returns the **last `init_unit` result** (−1 when the last creation failed) or −1 | production-spec §1/§6.4 (init_unit), movement-spec (find_nearby_spot) | via init_unit (anim choice) |
| `place_building(_upgrade)(who,type,city)` 18/1221, `_with_cost` 63, `place_building_upgrade_with_cost` 48 | city by name → its building id → `place_orphan_building*`: building type check, `Leader::produce_building(current_upgrade(t), city_o, 0)` (AI site choice). `_with_cost` first checks affordability (type vfunc +0x84) and returns `produce_building == 0` (1 placed, 0 not) | production-spec (`produce_building`) | yes (AI placement) |
| `set_timer(id, secs)` 1547 | `secs ≤ 0` or empty id → −1; else `ScriptTimers::add_timer(id, tick + secs)` @a049e0: replaces a same-name timer, ordered insert, **max 100** (error, −1), returns 1. `tick` = game seconds (frame-spec §3 step 22: `tick++` every 15 frames) | `ScenarioData::timers` | – |
| `timer_expired(id)` 410 | `ScriptTimers::check(id, tick)` @a04b80: no timer −1; `tick ≥ expiry` → removes it, 1; else 0 | | – |
| `stop_timer(id)` | remove: 1, absent −1 (@9e4c10 / `remove_timer` @a04b20) | | – |
| `rand_int(lo,hi)` 749 | see §7.4 | replay-spec §12 | game_random |
| `object_position_x/y(who,o)` 717/715 | tile of a unit/building (container's if garrisoned): `div_3_table[(pos ^ 0x63637) >> 6]` | movement-spec §1 (0x63637) | – |
| `find_unit(who,type)` 659, `find_build` 544 | next matching live object after the counter (types checked unit/building; "" = any); `find_build` also requires built+active (vfuncs +0x20,+0x4c); −1 none | find_counters | – |
| `give_good(who,res,n)` 655, `set_good` 261, `take_good` | stockpile `+= n` (no clamp) / `= n` (n ≥ 0); res must be one of the 6 goods; XOR 0x8221 | economy-spec §0 (also lists these callers) | – |
| `age(who)` 597 | current age (decrypted XOR 0x62766), needs ACTIVE | | – |
| `add_to_group` 567 | (who,o) → `_Unnamed<who>`; (name,who,o): creates the group (owner who-1), 0 already in, 1 added, −1 not a unit; (UnitGroup&,id): index if present else append (no validation) and return new index | `ScenarioData::scenario_groups` | – |
| `have_tech(who,tech)` 521 | `LeaderData::has_tech` | production-spec §7 | – |
| `group_stance_order(g,stance)` 513, `group_attack_to_order(g,x,y)` 480 and the other `group_*_order` | game group from the group (clears members' orders), then `Group::action_stance` / `action_move_to(…, ATTACK_TO)` etc. with QUEUE_NEW; returns member count (0 = nothing done), −1 bad group/tile | orders-spec §2, movement-spec | via orders |
| `find_city_id(name)` 512 | city building id by name/display name (all players), −1 | | – |
| `gain_tech(who,tech)` 510 | tech name, or a tech-category name (→ next level by the category counter XOR 0x63187); governments refused in CTW unless allowed; grants missing prerequisites first (`TechType::is_ultimate_preq`), then `Leader::gain_tech(t, 0, 0, 0, 1)`; 1 / −1 | production-spec §7.1 | via gain_tech |
| `gain_upgrade(who,type)` 421 | grafted type must be allowed for the tribe; prerequisites, `Leader::gain_tech`, `calc_unit_stats`, `calc_wall_stats`, `World::compute_all_territory`; returns `has_tech` | production-spec §7 | via gain_tech |
| `disable_type_by_tribe(type,tribe)` 471, `enable_type_by_tribe` 354 | unit/building type: clear/set tribe bit in TypeData +0x10, dirty +0x58 = 1; for every VALID leader of that tribe clear its cached availability bit (+0x6c18 / +0x6d00) and set its recalc flag to 2. Returns the **type index** | rules state (`types.list`) | – |
| `disable_type(type)` 239 | every type whose line contains it: TypeData +0x30 = −2, +0x10 = 0, +0x58 = 1; returns type index | rules | – |
| `set_type_build_time(type,secs)` 383 | every type matching: build time field +8 = `secs/100` (min 1); returns secs | rules | – |
| `popup_dialog(msg)` 458 | modal `Popup` (font "popup_dialog"; MP bit passed as a Popup flag), returns 1 | UI | – |
| `kill_unit_type(who,type)` 437, `kill_unit(who,o)`, `kill_unit_anim(who,o,anim 1..4)` 245 | `Object::die(death)`: random 1..3 (one draw each) or `anim-1`; `kill_unit_type` "" = all units; returns count | combat-spec §6.1 | game_random (not `_anim`) |
| `num_type(who,type)` 430, `num_type_upgrade` 289, `num_type_with_queued` | unit count (+0x56fe u16 table) / building count (+0x5222) / stockpile for goods; `_upgrade` uses the current line member; `_with_queued` adds queued (+0x5a22) | | – |
| `at_least_type(who,n,type)` | `n ≤ num_type` (same three tables) | | – |
| `num_cities(who)` 401 | Leader +0x3f8 | | – |
| `play_sound(file)` 384, `sound_finished` 349 | scenario-dir sound on `ScenarioData::sound`; see §7.4 | audio | – |
| `time_later_than(min)` 370, `time()`, `time_sec()` | `tick/60 ≥ min`; `time()` = minutes, `time_sec()` = seconds | | – |
| `get_ctw_int(id)` 369 / `set_ctw_int(id,v)` 255 (void) | `ConquestGame.stored_ints` NamedArray get / set-or-add | CTW state | – |
| `find_nation(who)` 315 (string) | tribe name, "" | | – |
| `parse(fmt, …)` 311 (string) | up to 10 extra args stringified (`ScriptType::get_string`) into `parse_strings[0..9]` (rest cleared), then `String::parse(fmt)` with `parse_ignore_num` = 1 (`$NUM0`, `$STRING0` … substitution: the letters between `$` and the digit are ignored, `$$` is `$`, `$LINK<` stays, `$<…>` gender / plural forms take their first form; ported in `bhs_stdlib.cpp` `parse_format`) | | – |
| `get_difficulty()` 308 | CTW window visible → conquest difficulty, else `info.difficulty + 1` | | – |
| `map_is_land(x,y)` 290 | tile terrain bits `& 0x30 != 0x20`; −1 off map | world-spec | – |
| `defeat(who)` 289 / `victory` 136 | `Leader::defeat(9, −1, 0)` | combat/frame-spec | via defeat |
| `find_capital(who)` 288 (string) | display name of the city flagged capital (round-robin counter [0xe]) | | – |
| `set_no_attack(tribe, terr1..4)` 277 | CTW: appends matching territory indices to the tribe's no-attack list; 1 if any | CTW | – |
| `get_country_owner(name)` 245, `get_territory_owner` | CTW territory owner tribe name (country form resolves the master) | CTW | – |
| `set_base_rate(who,res,n)` 222 | base gather rate `= n << 4` | economy-spec | – |
| `switch_teams(to,from,o)` 219 | transfer object: cities via `Cities::capture_city`, else vfunc +0x94 (new id), wall masks; returns new id | combat-spec (capture) | via capture |
| `get_turn()` 215 | `ConquestGame.cur_round` | | – |
| `objective_complete(id)` 213, `add_objective(msg,id,sound)` 194 | `MessageWin` scenario objectives (state in ScenarioData) | UI | – |
| `set_leader_name(who,s)` 200 | `GameInfo.player[get_player()].name` | | – |
| `get_map_size()` 191 | `world.size << 2` | | – |
| `destroy_building(who,o)` 187 | building vfunc +0x150 (destroy) | combat-spec §6 | – |
| `clear_group` 185 | named/default: remove the ScenarioGroup; UnitGroup&: clear array | | – |
| `object_health(who,o)` 184 | the captain's `(int)((float)left / (float)hits(0) * 100)`: units `myhits − UnitData::total_damage(0)` (the whole uber chain: damage + (uber_size − alive) × hits / uber_size + sixteenths), buildings `hits_left`; logs to game_log | | – |
| `is_defeated(who)` 180 | `LEADER_DEFEATED` | | – |
| `have_alliance(who,whom)` 176 | diplomacy table (Leader +0x74 row) == 2 | | – |
| `num_military_units(who)` 168 | sum of unit counts over unit types with category ≠ 5 | | – |
| `disable_city_defeat(who)` 161 | Leader +4 bit 0 | | – |
| AI: `train_unit_with_cost(who,n,type)` 18 | 1 ≤ n ≤ 2000, affordable (type vfunc +0x84 ≥ n), next building of the producing type (counter [0x1c]) → `Group::action_queue_up(t, n)`; returns building id, 0 none/unaffordable | production-spec §6.2.2 | – |
| AI: `research_tech_with_cost(who,tech)` 24 | have it → 1; else next researching building (counter [0x1e]) where `BuildData::can_queue` → `action_queue_up(t,1)`, returns building id, else 0 | production-spec §6.2.4 | – |
| AI: `can_pay_cost(who,type)` 23 | `type_avail == 4` and affordable → 1/0 | production-spec §7.9.5 | – |
| AI: `num_city_buildings(who,city,type,flag)` 50, `find_build_at_city` 39, `find_inactive_build` 29, `find_city_with_num(who,n)` 24 | `CityData::count_buildings`; walk of the city's building list (counters [0x16]/[0x17]); next building not built/active; n-th city's display name | | – |

Math/string/array utilities (all (e)): `absl_int`, `min_val` (returns b if b ≤ a), `max_val`; `length(s)` =
UTF-16 length; `char_at(s,i)` code unit or 0 out of range; `char_from_int(n)` one-char string; `print`/`print_line`
→ `OutputDebugStringW` only. ArrayUtil (elements must have the array's element `data_type`, else −1):
`add(a)` appends a blank element, returns its index; `add(a,v)` appends a copy, returns index; `insert(a,v,i)`
`0 ≤ i ≤ len`; `remove(a,v)` removes the first equal element, returns its old index; `remove_index(a,i)` returns the
**new length**; `find(a,v)` index or −1 (equality via the value's `do_operator`); `clear(a)` void.
TriggerUtil: `enable_all_triggers()` / `disable_all_triggers()` set/clear every named trigger bit of the current
script (`Script::toggle_all_triggers` @9c5c90; no-op without a running VM); `is_trigger_enabled(name)` 1/0, −1
unknown name or no VM. `enable_trigger`/`disable_trigger` are keywords, not builtins (§1).

### 7.6.1 Port findings (2026-10-13, engine/sim/script_funcs_*.cpp)

- **Game::is_scenario** @43ec70 is `semaphore byte1 & 0x10` (scripted rules, `game_rules == 10`) or `byte2 & 2` (CTW).
  Plain scenarios such as the tutorials (`game_rules` 0) are not "scenarios" in this sense: the scenario tallies
  (`units_killed`, `builds_destroyed`, `city_lost`), `ScenarioData::pop_cap`, the `*_costs_disable` switches,
  `buildings_gather_*` / `building_*_bonus_*` (which return −1 there) and `mouse_ / hotkey_selection_*` only act under
  scripted rules or CTW. `ScenarioFuncSet::init` / `close` set `building_unit_bonus`, `building_resource_bonus`,
  `buildings_gather` and `plunder` to 1.
- **Tallies**: `Unit::close` (last member, not a decoy, who < 8) increments `units_killed[who][type − 50]`;
  `Build::close` increments `builds_destroyed[who][type − 414]` (the two arrays are 0x20 shorts apart in the image);
  `unit_type_killed` / `building_type_destroyed` / `unit_any_killed` read **and clear** them. `Object::take_damage`
  (a building destroyed) and `Object::disband` (an active building) write `last_razed[who] = o`; `City::capture` sets
  `city_lost` bit old owner and `city_lost_to[old owner] = new owner`; `Build::train` writes `last_unit_built_o`.
- `set_explored(who, x, y, r)` compares the **squared** tile distance with `r` (the `sqrtf` result is dropped), loops x
  over `[x − r, x + r]` but y over `[y − r, y + r)` (the clamped last row is skipped), and `World::set_seen`s the fog
  cell `(t >> 1)` with detect; who 0 = every valid leader. `set_explored(who)` = `show_all_map_enable` (REVEAL_ENEMY
  0x800).
- `remove_infiltrated(who, o, spy_who)` stores the boolean `(infiltrated & bit) == 0` into the byte (the original's
  expression), the 2-argument form clears it.
- `set_bonus_cap` always returns −1 (after writing `bonus_cap[good]`, goods < 50); `get_base_rate` = base_rate / 16,
  `gather_rate` = income / 16 (truncating).
- `object_near` / `object_near_build` / `is_game_mouse_near` measure `vector_dist` between **tiles**
  (`div_3_table[pos >> 6]`), `object_near_build` minus `(x_size + y_size) >> 1`; `find_objects` (any_object_near,
  object_type_near, *_near_build) passes the tile **corner** (`t * 192`) and radius `dist * 192` to
  `Objects::find_units` / `find_builds` (SEARCH_FRIENDLY; the circle-table branch takes every live on-map object of the
  wcoord lists without a distance test, the full scan subtracts the type's big_radius (naval: push_size) / the
  building's footprint).
- Objectives: `play_objective_sound(id)` plays the objective's sound through `play_sound` (the same
  `ScenarioData::sound` channel `sound_finished` polls); `objective_complete("")` / `set_objective_sound("")` act on
  every entry; `add_objective_text` / `ctw_add_objective_text` add without the announcement (`display_check` 0).
- UnitGroup family (`active_unit_group`): `validate_param` builds a Group (`create_game_group`: members' orders
  dropped except planes), pushes it with force and passes the slot; the named and `(int who)` families do the same
  through `create_game_group(ScenarioGroup*)`. Every family returns the pushed group's member count.
- `gain_tech(who, category)` / `lose_tech(who, category)`: the tech-category keys select MILITARY_BASE 572, CIVIC 565,
  BARTER 558, WRITTEN_WORD 551 + the line's epoch (lose: − 1); epochs > 7 → −1. Both grant the non-age ultimate
  prerequisites first (`TechType::is_ultimate_preq`, here through `TypeData::get_preq`). `Leader::lose_tech` @6d2850
  and `Leader::set_age` @6d25a0 (gain_next_age) are ported with `Leader::fix_tech_flags` @6d2480.
- `popup_choice` pauses the game around the modal popup (outside the CTW window) and returns 1 for the accept button
  (3-button form: the button index); `popup_dialog` always returns 1.
- `remove_from_group(who, o)` / `(name, who, o)` need the group to belong to that leader and use
  `SimpleArray<int>::remove` @462e70: the first match is dropped and the order kept (the group order feeds
  formations). `find_build_at_city_using_type_name` / `num_city_buildings_using_type_name` have the same bodies as
  `find_build_at_city` / `num_city_buildings` (shared find counters 0x16 / 0x17).
- `set_object_type_attack` / `_max_range` / `_armor` (ObjectTypeData +0x1e8 / +0x1fc / +0x214) and
  `set_unit_type_speed` (UnitTypeData +0x2c0, units only) follow `set_object_type_max_health`: value ≥ 1, every type
  of the line (units 50..401 or buildings 414..542) with `modified` = 1, then `calc_wall_stats` / `calc_unit_stats`
  for each live leader; they return the type index.
- `set_age_diff_penalty(d, pct)` writes Constants one_age_down .. five_ages_down (d clamped to 1..5) and returns 1;
  the balance table was built from them at load, so nothing else changes.
- `have_contact(who, whom)` = `treaties[whom] & 1` (LeaderData +0x94); `disable_take_attrition` sets +0x7fc;
  `change_to_human` sets HUMAN (0 if already human) and clears AI_CONTROL on the live units;
  `city_id_captured(who, o)` consumes the city-lost bit only once building o no longer validates for who.
- CTW reads with no campaign loaded (a conquest battle started on its own): `get_turn` / `get_age` read
  `conquest_game->cur_round` / `cur_age` (0), `get_ctw_campaign` is "" and `get_num_tribute` -1 unless the
  ConquestGame is initialized; `get_ctw_int` returns 0 for an absent name, `set_ctw_int` sets or adds,
  `clear_ctw_val` removes the name from both the int and the string arrays. `ctw_add_objective` adds through
  `MessageWin::add_ctw_objective` with the announcement on, then sets the sound.

### 7.7 Open questions

1. Lockstep behaviour of the two command builtins in live multiplayer (`rename_city` by name, `unit_eject_order`):
   every machine running the script issues its own command (idempotent for rename; check `issue_come_out`).
2. `popup_choice` in multiplayer: Popup gets the MP flag (`<< 0x16`); does `BasePop::exec` then return a fixed value?
   If not, its result is local. Shipped uses are CTW (single player) only.
3. Token text of the varargs type id 0x64ea2a (irrelevant to behaviour; only `parse` uses it).
4. TypeData +0xb0 (the alternate name `get_type_index` can match) — confirm in rules-spec.
5. The 262 unused builtins are categorised by name rule only (plus closure evidence); read each body when it is ported.
6. Return values of builtins whose decompiles drop the final return (Ghidra shows `-1` paths only, e.g. the timer
   wrappers): check the disassembly when porting (done for set_timer/timer_expired/add_timer/check).

## 8. Port plan

Order of work; each step has its own test.

1. **Scanner** — re-express the patterns of §1.2 as a hand-written scanner (the DFA tables are game data and stay in
   the user's exe; the C++ port must not embed them). Test: token streams identical to `bhs-parse.mjs --tokens` for
   all 365 files (dump both, diff).
2. **Parser** — a hand-written LALR-equivalent or recursive-descent parser for the §2.1 grammar with the §2.4 symbol
   actions. Recursive descent works with one token of lookahead plus the scanner's classification; keep the left
   recursion order of the lists (it matters for code order: arg lists vs expr lists). Test: same reduction sequence as
   the table-driven parser (add a `--reductions` dump to `bhs-parse.mjs`), all 365 files.
3. **Code generator** — node patterns of §3, index and pool rules of §3.5, image writer of §4.4. Test: a disassembler
   for §4.1/4.2 (`re/tools/bhs-disasm.mjs`, to write) and golden listings; once a save from the user exists, compare the
   embedded `ScriptFile` images (savdump already decodes the walk) byte for byte with ours for the same scripts. Until
   then, self-consistency: every jump lands on an instruction start, stack depth is balanced per path.
4. **VM** — opcodes of §4.2, values of §5.1–5.2, `run_script` protocol of §5.3. Test: unit tests per opcode and per
   operator/type pair (division by zero, negative truthiness, string case-insensitivity, f32 formatting), then running
   compiled shipped scripts with stub builtins (no error, balanced stack).
5. **Runtime hooks** — scenario script per frame (§6.2), general powers (§6.3), AI production script cadence (§6.4),
   save/load of `RunTimeEnv` (state-walk.md). Test: run the 156 stock scenarios headless for N frames with builtins
   logged; then compare with replays (scenario + AI) once available.
6. **Builtins in priority order** (§7). First the registration table itself: all 873 entries in registration order
   (global indices 0–872 are baked into compiled images, §7.1), with signatures from `bhs_builtins.tsv`, so any
   compiled script binds and unported bodies fail loudly. Then: (a) script environment (timers, trigger utilities,
   math incl. `rand_int`, strings, arrays: 53); (b) the 55 names the AI production scripts call (every skirmish with AI
   players); (c) the top scenario/tutorial functions of §7.6; (d) the CTW battle set (`create_unit_upgrade`,
   `create_unit_in_group`, `place_building_upgrade`, `give_good`, group orders…); (e) presentation builtins as
   local-only stubs with the single-player guards of §7.4, real later; (f) the 262 unused names last. Each builtin is
   tested against the sim function it wraps (the spec column in §7.6) and, finally, against replays.

## 9. Port notes and pitfalls

- Feed original bytes (CRLF, §1.1); classify identifiers against live symbol tables (§1.4); keep `&`/`|` logical and
  `^` power; `true`/`false` are ints; labels start at 1.
- Auto-typed declarations and implicit declarations by assignment are pervasive in the shipped scripts.
- Code order quirks (argument 0 on top, initialiser elements reversed, constant pool order) are needed for byte-exact
  images; behaviour does not depend on them.
- Reject or reproduce: shifts/bitwise emitting nothing, `continue` in `for`, `default` not last.

## 10. Open questions

1. Validate everything against a real save: `ScriptFile` images (code, constant pool order, debug name tables
   including the block-variable script-index quirk of §4.4), static variable values and trigger bits.
2. `String::get_double` and `_gcvt_s` exact behaviour (float literal canonical tokens and float→string casts) — check
   the CRT on a Windows build or with known strings from saves.
3. `powf` results for integer powers (`^` is unused in shipped scripts, so low priority).
4. The token text of the varargs type id 0x64ea2a (only `parse` uses it; behaviour is known, §7.1).
5. Whether `ScriptFile::script_files` is cleared between games in one session (affects `find_script`'s last-wins order
   and `linked_files` indices in saves).
6. Struct default construction (`StructType::get_create_opcode`) and struct `==` semantics — no shipped script
   constructs structs, but `game_structs.bhs` declares three that builtins may return.
