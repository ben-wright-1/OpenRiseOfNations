// Read the BigHugeScript lexer (flex) and parser (bison 1.x) tables out of the user's own riseofnations.exe.
// Nothing game-derived is stored in the repo: the tables are read at run time from the install.
// Addresses and lengths are the PDB data symbols (sym:.*yy.* dump, see docs/script-spec.md §2).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const DEFAULT_GAME = path.join(os.homedir(), 'Games/RiseOfNations');

// [va, element type, count]
const TABLES = {
  // flex (BigHugeScript.l.obj)
  yy_ec: [0xb03500, 'i32', 256],
  yy_meta: [0xb04650, 'i32', 65],
  yy_base: [0xb043f0, 'i16', 299],
  yy_def: [0xb04760, 'i16', 299],
  yy_nxt: [0xb03900, 'i16', 522],
  yy_chk: [0xb03fb0, 'i16', 522],
  yy_accept: [0xb03d70, 'i16', 281],
  // bison (BigHugeScript.obj, yyparse @9ba430)
  yytranslate: [0xb02d70, 'u8', 320],
  yypact: [0xb02a50, 'i16', 394],
  yydefact: [0xb02eb0, 'i16', 394],
  yypgoto: [0xb01580, 'i16', 84],
  yydefgoto: [0xb031d0, 'i16', 84],
  yytable: [0xb01640, 'i16', 1036],
  yycheck: [0xb021e0, 'i16', 1036],
  yyr1: [0xb01e60, 'i16', 222],
  yyr2: [0xb02020, 'i16', 222],
};

export function findExe(game = DEFAULT_GAME) {
  for (const name of fs.readdirSync(game)) if (name.toLowerCase() === 'riseofnations.exe') return path.join(game, name);
  throw new Error(`riseofnations.exe not found in ${game}`);
}

export function peReader(file) {
  const b = fs.readFileSync(file);
  const pe = b.readUInt32LE(0x3c);
  const nsec = b.readUInt16LE(pe + 6), optSize = b.readUInt16LE(pe + 20);
  const imageBase = b.readUInt32LE(pe + 24 + 28);
  const secs = [];
  for (let i = 0; i < nsec; i++) {
    const s = pe + 24 + optSize + i * 40;
    secs.push({ va: b.readUInt32LE(s + 12) + imageBase, vsize: b.readUInt32LE(s + 8), raw: b.readUInt32LE(s + 20), rsize: b.readUInt32LE(s + 16) });
  }
  const off = (va) => {
    for (const s of secs) if (va >= s.va && va < s.va + Math.max(s.vsize, s.rsize)) return s.raw + (va - s.va);
    throw new Error(`va 0x${va.toString(16)} not in a section`);
  };
  return { b, off };
}

export function loadTables(game = DEFAULT_GAME) {
  const { b, off } = peReader(findExe(game));
  const t = {};
  for (const [name, [va, ty, n]] of Object.entries(TABLES)) {
    const o = off(va), a = new Int32Array(n);
    for (let i = 0; i < n; i++) {
      a[i] = ty === 'i32' ? b.readInt32LE(o + 4 * i) : ty === 'i16' ? b.readInt16LE(o + 2 * i) : b.readUInt8(o + i);
    }
    t[name] = a;
  }
  // bison constants visible in yyparse @9ba430: YYFINAL 393 (0x189), YYLAST 1035 (0x40b), YYFLAG -32768,
  // YYNTBASE from yytranslate's "unknown" value 0xae (= YYNTOKENS... see derive()), YYMAXUTOK 319.
  t.YYFINAL = 393; t.YYLAST = 1035; t.YYFLAG = -32768; t.YYMAXUTOK = 319; t.YYUNDEF = 0xae;
  // flex constants visible in Lexer::yylex @9bc8e0: jam state base 457 (0x1c9), last DFA state 280 (0x118).
  t.YY_JAMBASE = 457; t.YY_LASTDFA = 280;
  return t;
}
