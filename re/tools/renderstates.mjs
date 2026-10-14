// Build re/iface/renderstates.json from the decompiled Scene::init_render_states and the game's
// Data/internal_strings.xml (names come from the internal string table: offset / 20 = entry index).
// Usage: node re/tools/renderstates.mjs [gameDir] [--md]   (gameDir defaults to ~/Games/RiseOfNations; --md prints the doc table)
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const game = process.argv.slice(2).find((a) => !a.startsWith('--')) ?? path.join(os.homedir(), 'Games/RiseOfNations');
const src = fs.readFileSync(path.join(root, 're/decomp/renderstate/Scene_init_render_states@008ba450.c'), 'utf8');

// --- internal string table -------------------------------------------------------------------
const xml = fs.readFileSync(path.join(game, 'Data/internal_strings.xml'), 'utf8');
const strs = [];
for (const m of xml.matchAll(/<STRING\b[^>]*?(?:\/>|>([\s\S]*?)<\/STRING>)/g)) strs.push(decode(m[1] ?? ''));
function decode(s) {
	return s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&amp;/g, '&');
}
const STRING_SIZE = 0x14;
const istr = (off) => {
	if (off % STRING_SIZE) throw new Error(`string offset ${off.toString(16)} not a multiple of 0x14`);
	return strs[off / STRING_SIZE];
};

// --- enums (rise.pdb) --------------------------------------------------------------------------
const E = {
	CompareFunc: ['NEVER', 'LESS', 'EQUAL', 'LESSEQUAL', 'GREATER', 'NOTEQUAL', 'GREATEREQUAL', 'ALWAYS'],
	BlendMode: ['NONE', 'ZERO', 'ONE', 'SRCCOLOR', 'INVSRCCOLOR', 'SRCALPHA', 'INVSRCALPHA', 'DESTALPHA', 'INVDESTALPHA', 'DESTCOLOR', 'INVDESTCOLOR', 'SRCALPHASAT'],
	CullMode: ['NONE', 'CW', 'CCW'],
	FillMode: ['NONE', 'POINT', 'WIREFRAME', 'SOLID'],
	ShadeMode: ['NONE', 'FLAT', 'GOURAUD'],
	StencilOp: ['KEEP', 'ZERO', 'REPLACE', 'INCRSAT', 'DECRSAT', 'INVERT', 'INCR', 'DECR'],
	TexAddress: ['WRAP', 'MIRROR', 'CLAMP', 'BORDER', 'MIRRORONCE'],
	MIPFilter: ['NONE', 'POINT', 'LINEAR', 'ANISOTROPIC', 'FLATCUBIC', 'GAUSSIANCUBIC'],
	TextureOp: ['DISABLE', 'SELECTARG1', 'SELECTARG2', 'MODULATE', 'MODULATE2X', 'MODULATE4X', 'ADD', 'ADDSIGNED', 'ADDSIGNED2X', 'SUBTRACT', 'ADDSMOOTH', 'BLENDDIFFUSEALPHA', 'BLENDTEXTUREALPHA', 'BLENDFACTORALPHA', 'BLENDTEXTUREALPHAPM', 'BLENDCURRENTALPHA', 'PREMODULATE', 'MODULATEALPHA_ADDCOLOR', 'MODULATECOLOR_ADDALPHA', 'MODULATEINVALPHA_ADDCOLOR', 'MODULATEINVCOLOR_ADDALPHA', 'BUMPENVMAP', 'BUMPENVMAPLUMINANCE', 'DOTPRODUCT3', 'MULTIPLYADD', 'LERP'],
	TextureArg: ['CURRENT', 'DIFFUSE', 'SELECTMASK', 'SPECULAR', 'TEMP', 'TEXTURE', 'TFACTOR'],
};
const PREFIX = { CMP_: 'CompareFunc', BLENDMODE_: 'BlendMode', CULLMODE_: 'CullMode', FILLMODE_: 'FillMode', SHADEMODE_: 'ShadeMode', STENCILOP_: 'StencilOp', TEXADD_: 'TexAddress', MIP_: 'MIPFilter', TOP_: 'TextureOp' };

// --- RenderState layout (rise.pdb, size 0x168) ---------------------------------------------------
// [offset, field, kind, enum]; arrays of 2 are expanded to field[0], field[1].
const LAYOUT = [
	[0x50, 'enables', 'int'], [0x54, 'zbias', 'int'], [0x58, 'zfunc', 'enum', 'CompareFunc'], [0x5c, 'tfactor', 'color'],
	[0x68, 'fill_mode', 'enum', 'FillMode'], [0x6c, 'shade_mode', 'enum', 'ShadeMode'], [0x70, 'cull_mode', 'enum', 'CullMode'],
	[0x74, 'src_blend', 'enum', 'BlendMode'], [0x78, 'dst_blend', 'enum', 'BlendMode'], [0x7c, 'alpha_func', 'enum', 'CompareFunc'],
	[0x80, 'alpha_ref', 'int'], [0x84, 'stencil_func', 'enum', 'CompareFunc'], [0x88, 'stencil_ref', 'int'],
	[0x8c, 'stencil_fail', 'enum', 'StencilOp'], [0x90, 'stencil_zfail', 'enum', 'StencilOp'], [0x94, 'stencil_pass', 'enum', 'StencilOp'],
	[0x98, 'stencil_mask', 'int'], [0x9c, 'stencil_write_mask', 'int'], [0xa0, 'index_vblend', 'int'],
	[0xa4, 'tex_coord_index', 'int', null, 2], [0xac, 'colorop', 'enum', 'TextureOp', 2], [0xb4, 'colorarg1', 'enum', 'TextureArg', 2],
	[0xbc, 'colorarg2', 'enum', 'TextureArg', 2], [0xc4, 'alphaop', 'enum', 'TextureOp', 2], [0xcc, 'alphaarg1', 'enum', 'TextureArg', 2],
	[0xd4, 'alphaarg2', 'enum', 'TextureArg', 2], [0xdc, 'address_u', 'enum', 'TexAddress', 2], [0xe4, 'address_v', 'enum', 'TexAddress', 2],
	[0xec, 'mag_filter', 'enum', 'MIPFilter', 2], [0xf4, 'min_filter', 'enum', 'MIPFilter', 2], [0xfc, 'mip_filter', 'enum', 'MIPFilter', 2],
	[0x104, 'mipmap_bias', 'float', null, 2], [0x10c, 'clip_plane_constants', 'float', null, 4], [0x11c, 'fog_mode', 'int'],
	[0x120, 'fog_start', 'float'], [0x124, 'fog_end', 'float'], [0x128, 'fog_density', 'float'], [0x12c, 'fog_color', 'color'],
	[0x138, 'delta_uv0.u', 'float'], [0x13c, 'delta_uv0.v', 'float'], [0x140, 'delta_uv1.u', 'float'], [0x144, 'delta_uv1.v', 'float'],
];
const byOff = new Map(), byName = new Map();
for (const [off, name, kind, en, n = 1] of LAYOUT) {
	for (let i = 0; i < n; i++) {
		const f = { name: n > 1 ? `${name}[${i}]` : name, kind, en };
		byOff.set(off + 4 * i, f);
		byName.set(f.name, f);
	}
}

// RenderState::init defaults (@00535ba0)
function defaults() {
	return {
		enables: 3, zbias: 0, zfunc: 3, tfactor: 0xffffffff, fill_mode: 3, shade_mode: 2, cull_mode: 2, src_blend: 2, dst_blend: 1,
		alpha_func: 7, alpha_ref: 0, stencil_func: 7, stencil_ref: 0, stencil_fail: 0, stencil_zfail: 0, stencil_pass: 0,
		stencil_mask: -1, stencil_write_mask: -1, index_vblend: 0,
		'tex_coord_index[0]': 0, 'colorop[0]': 3, 'colorarg1[0]': 5, 'colorarg2[0]': 0, 'alphaop[0]': 1, 'alphaarg1[0]': 5, 'alphaarg2[0]': 0,
		'address_u[0]': 0, 'address_v[0]': 0, 'mag_filter[0]': 1, 'min_filter[0]': 1, 'mip_filter[0]': 0, 'mipmap_bias[0]': 0,
		'tex_coord_index[1]': 1, 'colorop[1]': 0, 'colorarg1[1]': 5, 'colorarg2[1]': 0, 'alphaop[1]': 0, 'alphaarg1[1]': 5, 'alphaarg2[1]': 0,
		'address_u[1]': 0, 'address_v[1]': 0, 'mag_filter[1]': 1, 'min_filter[1]': 1, 'mip_filter[1]': 0, 'mipmap_bias[1]': 0,
		'clip_plane_constants[0]': 0, 'clip_plane_constants[1]': 0, 'clip_plane_constants[2]': 1, 'clip_plane_constants[3]': 0,
		fog_mode: 0, fog_start: 0, fog_end: 1, fog_density: 0.5, fog_color: 0, // BLACK
		'delta_uv0.u': 0, 'delta_uv0.v': 0, 'delta_uv1.u': 0, 'delta_uv1.v': 0,
	};
}

const num = (s) => {
	s = s.trim().replace(/U$/, '');
	if (/^-?0x[0-9a-f]+$/i.test(s)) return parseInt(s, 16) | 0;
	if (/^-?\d+(\.\d+)?(e[-+]?\d+)?$/i.test(s)) return Number(s);
	for (const [p, en] of Object.entries(PREFIX)) if (s.startsWith(p)) {
		const v = E[en].indexOf(s.slice(p.length));
		if (v < 0) throw new Error(`unknown enum ${s}`);
		return v;
	}
	throw new Error(`cannot parse value ${s}`);
};
const f32 = (i) => { const b = Buffer.alloc(4); b.writeInt32LE(i | 0); return b.readFloatLE(0); };

// --- walk the decompiled function ---------------------------------------------------------------
const states = [];
let cur = null;                // { idx, s (base values), alt (bad_team_color overrides) }
let branch = null;             // null | 'good' | 'bad'
const ptrVars = {};            // pSVarNN -> string offset
let pendingShader = null, pendingSituation = null;
const unhandled = [];

function set(field, value, line) {
	if (!cur) throw new Error(`assignment outside a state: ${line}`);
	const f = byName.get(field);
	if (!f) throw new Error(`unknown field ${field}: ${line}`);
	if (f.kind === 'float' && Number.isInteger(value) && !/\./.test(line) && Math.abs(value) > 0xffff) value = f32(value);
	const target = branch === 'bad' ? cur.bad : branch === 'good' ? cur.good : cur.s;
	target[field] = value;
}
function enablesOp(andMask, orMask) {
	const target = branch === 'bad' ? cur.bad : branch === 'good' ? cur.good : cur.s;
	target.enables = ((cur.s.enables & andMask) | orMask) >>> 0;
}

const lines = src.split('\n');
for (let i = 0; i < lines.length; i++) {
	let L = lines[i].trim();
	// join wrapped statements
	while (L && !/[;{}]$/.test(L) && i + 1 < lines.length) L += ' ' + lines[++i].trim();
	let m;
	if ((m = L.match(/^(?:pRVar1|iVar65) = \*\((?:RenderState|int) \*\*?\)\(?render_states\._padding_(?: \+ (0x[0-9a-f]+|\d+))?\)?;$/))) {
		const idx = m[1] ? num(m[1]) / 4 : 0;
		cur = { idx, s: defaults(), good: {}, bad: {}, situations: [] };
		states[idx] = cur;
		branch = null;
		continue;
	}
	if ((m = L.match(/^(pSVar\d+) = \(String \*\)\(\*\(int \*\)&(?:int_str_array|pSVar56)->field_0x10 \+ (0x[0-9a-f]+)\);$/))) {
		ptrVars[m[1]] = num(m[2]);
		continue;
	}
	if ((m = L.match(/^String::operator=\(&pRVar1->name,(pSVar\d+)\);$/))) { cur.name = istr(ptrVars[m[1]]); cur.nameOff = ptrVars[m[1]]; continue; }
	if ((m = L.match(/^String::operator=\(\(String \*\)\(iVar65 \+ 0x3c\), \(String \*\)\(\*\(int \*\)&int_str_array->field_0x10 \+ (0x[0-9a-f]+)\)\);$/))) {
		cur.name = istr(num(m[1])); cur.nameOff = num(m[1]); continue;
	}
	if ((m = L.match(/^String::String\(\(String \*\)&stack0xffffffc8,(pSVar\d+)\);$/))) { pendingShader = istr(ptrVars[m[1]]); continue; }
	if ((m = L.match(/^String::String\(\(String \*\)&stack0xffffffb4,(?:&EMPTY_STRING|\(String \*\)\(\*\(int \*\)&int_str_array->field_0x10 \+ (0x[0-9a-f]+)\))\) ?;$/))) {
		pendingSituation = m[1] ? istr(num(m[1])) : ''; continue;
	}
	if (/^RenderState::add_pshader_situation\((pRVar1|\(RenderState \*\)iVar65),/.test(L)) {
		cur.situations.push({ situation: pendingSituation, pshader: pendingShader });
		pendingShader = pendingSituation = null;
		continue;
	}
	if (/^if \(\(pGVar61->scalability\)\.bad_team_color == 0\) \{$/.test(L)) { branch = 'good'; continue; }
	if (L === '}' && branch === 'good') { continue; }
	if (L === 'else {' && branch === 'good') { branch = 'bad'; continue; }
	if (L === '}' && branch === 'bad') { branch = null; continue; }
	// typed enables
	if ((m = L.match(/^pRVar1->enables = pRVar1->enables (?:& (0x[0-9a-f]+)U? )?\| (0x[0-9a-f]+|\d+);$/))) { enablesOp(m[1] ? num(m[1]) : -1, num(m[2])); continue; }
	if ((m = L.match(/^\*\(uint \*\)\(iVar65 \+ 0x50\) = \*\(uint \*\)\(iVar65 \+ 0x50\) (?:& (0x[0-9a-f]+)U? )?\| (0x[0-9a-f]+|\d+);$/))) { enablesOp(m[1] ? num(m[1]) : -1, num(m[2])); continue; }
	if ((m = L.match(/^(?:pRVar1->enables = pRVar1->enables|\*\(uint \*\)\(iVar65 \+ 0x50\) = \*\(uint \*\)\(iVar65 \+ 0x50\)) & (0x[0-9a-f]+)U?;$/))) { enablesOp(num(m[1]), 0); continue; }
	if (/^\(pRVar1->tfactor\)\.field0_0x0 = WHITE\.field0_0x0;$/.test(L)) { set('tfactor', 0xffffffff, L); continue; }
	if (/^\(pRVar1->tfactor\)\.(?!field0_0x0\.rgb)|^uVar(57|58|59|60) = WHITE\.|^\*\(undefined2 \*\)&\(pRVar1->tfactor\)\.field_0xa = /.test(L)) continue;
	if ((m = L.match(/^\*\(uint \*\)\(iVar65 \+ 0x80\) = (0x[0-9a-f]+) - \(uint\)\(\(pGVar61->scalability\)\.is_SiS != 0\);$/))) {
		set('alpha_ref', num(m[1]), L); cur.special = `alpha_ref is ${num(m[1])} - 1 when scalability.is_SiS (SiS cards)`; continue;
	}
	if ((m = L.match(/^\(pRVar1->tfactor\)\.field0_0x0\.rgb = (\S+);$/))) { set('tfactor', num(m[1]) >>> 0, L); continue; }
	if ((m = L.match(/^pRVar1->([a-z_0-9]+(?:\[\d\])?) = (\S+);$/))) { set(m[1], num(m[2]), L); continue; }
	if ((m = L.match(/^\*\((undefined4|float|int|uint) \*\)\(iVar65 \+ (0x[0-9a-f]+|\d+)\) = (\S+);$/))) {
		const off = num(m[2]), f = byOff.get(off);
		if (off === 0x5c) { set('tfactor', num(m[3]) >>> 0, L); continue; }
		if (off > 0x5c && off < 0x68) continue; // Color w_555/w_565/flags/index, derived from the rgb dword
		if (!f) { unhandled.push(L); continue; }
		set(f.name, m[1] === 'float' ? Number(m[3]) : num(m[3]), L);
		continue;
	}
	// partial writes into the tfactor Color (w_555/w_565/flags/index); the dword at +0x5c is written last
	if (/^\*\((undefined[12]|undefined4) \*\)\(iVar65 \+ (0x5e|0x62|100|0x64|0x5c)\) = /.test(L)) continue;
	if (/^(pSVar56|pGVar61) = |^uVar6[67] = |^local_8 = |^S?Var\d+\.|^SVar\d+\.|^\w+ = int_str_array;/.test(L)) continue;
	if (cur && !/^(\}|ExceptionList = local_10;|return;|)$/.test(L)) unhandled.push(L);
}

// --- apply branches, decode --------------------------------------------------------------------
const enumOf = (field) => byName.get(field)?.en;
function decodeValue(field, v) {
	const en = enumOf(field);
	if (!en) return v;
	if (en === 'TextureArg') return (v < 0 ? '~' : '') + E.TextureArg[v & 0x7fffffff];
	return E[en][v] ?? v;
}
const out = [];
for (const st of states) {
	const s = { ...st.s, ...st.good };
	const bad = Object.keys(st.bad).length ? { ...st.s, ...st.bad } : null;
	const diff = (base, alt) => Object.fromEntries(Object.keys(alt).filter((k) => alt[k] !== base[k]).map((k) => [k, alt[k]]));
	out.push({ index: st.idx, name: st.name, nameStringIndex: st.nameOff / STRING_SIZE, values: s, badTeamColorOverrides: bad ? diff(s, bad) : null, situations: st.situations, special: st.special ?? null });
}

// --- derived backend view (d3dgl.dll D3D11GLGraphicCard::set_*_state, set_shader_options, D3D11PixelShader::compile)
const ENABLE_BITS = {
	0x1: 'ZENABLE', 0x2: 'ZWRITE', 0x4: 'ALPHABLEND', 0x8: 'STENCIL', 0x10: 'FOG', 0x80: 'ALPHATEST', 0x200: 'LIGHTING',
	0x800: 'UNUSED_0x800', 0x4000: 'NORMAL_MAP', 0x8000: 'SPEC_MAP', 0x10000: 'GLOSS_MAP', 0x20000: 'TL_FAKE_LIGHT',
	0x40000: 'DELTA_UV', 0x80000: 'ANIMATE_WATER_NORMALS', 0x100000: 'SPEC_MODEL_TORRANCE', 0x200000: 'SCREEN_REFLECTIONS',
	0x400000: 'POINT_SAMPLING', 0x800000: 'DIFFUSE_AS_SPEC', 0x1000000: 'POINT_SPRITE', 0x2000000: 'NUKE_FLASH',
};
const bitNames = (v) => Object.entries(ENABLE_BITS).filter(([b]) => v & Number(b)).map(([, n]) => n)
	.concat(v & ~Object.keys(ENABLE_BITS).reduce((a, b) => a | Number(b), 0) ? [`UNKNOWN_0x${(v & ~Object.keys(ENABLE_BITS).reduce((a, b) => a | Number(b), 0)).toString(16)}`] : []);
// enables bit -> ShaderCompilationOptions macro (set_shader_options); LIGHTING also needs the context's light switch on,
// NORMAL_MAP needs tangents in the vertex format, ANIMATE_WATER_NORMALS/DIFFUSE_AS_SPEC need NORMAL_MAP/SPEC_MAP.
const BIT_MACRO = [[0x200000, 'HAS_SCREEN_REFLECTIONS'], [0x2000000, 'NUKE_FLASH'], [0x1000000, 'POINT_SPRITE'], [0x10, 'ENABLE_FOG'],
	[0x80, 'ENABLE_ALPHA_TEST'], [0x4000, 'USE_NORMAL_MAP'], [0x8000, 'USE_SPEC_MAP'], [0x10000, 'USE_GLOSS_MAP'], [0x20000, 'TL_FAKE_LIGHT'],
	[0x40000, 'DELTA_UV'], [0x200, 'ENABLE_LIGHTING'], [0x100000, 'SPEC_MODEL_TORRANCE']];
const COLOR_ARG_MACRO = ['COLOR_CURRENT', 'COLOR_DIFFUSE', 'COLOR_DIFFUSE', 'COLOR_SPECULAR', 'COLOR_TEMP', 'COLOR_TEXTURE($N)', 'COLOR_TEXFACTOR'];
const TEXOP_MACRO = ['DISABLE', 'SELECTARG1', 'SELECTARG2', 'MODULATE', 'MODULATE2X', 'MODULATE4X', 'ADD', 'ADDSIGNED', 'ADDSIGNED2X', 'SUBSTRACT', 'ADDSMOOTH', 'BLENDDIFFUSEALPHA', 'BLENDTEXTUREALPHA', 'BLENDFACTORALPHA', 'BLENDTEXTUREALPHAPM', 'BLENDCURRENTALPHA', 'PREMODULATE', 'MODULATEALPHAADDCOLOR', 'MODULATECOLORADDALPHA', 'MODULATEINVALPHAADDCOLOR', 'MODULATEINVCOLORADDALPHA', 'BUMPENVMAP', 'BUMPENVMAPLUMINANCE', 'DOTPRODUCT3', 'MULTIPLYADD', 'LERP'];
const HLSL_HAS_TEXOP = new Set(TEXOP_MACRO.slice(0, 21).concat(['DOTPRODUCT3']));
const MTL_BLEND = [null, 'zero', 'one', 'sourceColor', 'oneMinusSourceColor', 'sourceAlpha', 'oneMinusSourceAlpha', 'destinationAlpha', 'oneMinusDestinationAlpha', 'destinationColor', 'oneMinusDestinationColor', 'sourceAlphaSaturated'];
const MTL_CMP = ['never', 'less', 'equal', 'lessEqual', 'greater', 'notEqual', 'greaterEqual', 'always'];
const MTL_ADDR = ['repeat', 'mirrorRepeat', 'clampToEdge', 'clampToBorderColor', 'mirrorClampToEdge'];
const MTL_CULL = ['none', 'back', 'front']; // D3D11 FrontCounterClockwise=TRUE: CULLMODE_CW -> CULL_BACK, CULLMODE_CCW -> CULL_FRONT
const MTL_FILL = ['fill', 'lines', 'lines', 'fill'];   // d3d11FillModeLookup: NONE->SOLID, POINT->WIREFRAME

function argMacro(v, stage) {
	let a = v < 0 ? (v & 0x7fffffff) + 7 : v;
	if (stage === 0 && (a === 0 || a === 7)) a += 1; // stage 0: CURRENT -> DIFFUSE (and complements)
	const base = COLOR_ARG_MACRO[a % 7].replace('$N', String(stage + 1));
	return a >= 7 ? `COMPLEMENT(${base})` : base;
}
function backend(v) {
	const en = v.enables >>> 0;
	const stage = (i) => ({
		colorop: E.TextureOp[v[`colorop[${i}]`]], colorarg1: decodeValue('colorarg1[0]', v[`colorarg1[${i}]`]), colorarg2: decodeValue('colorarg2[0]', v[`colorarg2[${i}]`]),
		alphaop: E.TextureOp[v[`alphaop[${i}]`]], alphaarg1: decodeValue('alphaarg1[0]', v[`alphaarg1[${i}]`]), alphaarg2: decodeValue('alphaarg2[0]', v[`alphaarg2[${i}]`]),
		texcoord: v[`tex_coord_index[${i}]`], address_u: E.TexAddress[v[`address_u[${i}]`]], address_v: E.TexAddress[v[`address_v[${i}]`]],
		mag_filter: E.MIPFilter[v[`mag_filter[${i}]`]], min_filter: E.MIPFilter[v[`min_filter[${i}]`]], mip_filter: E.MIPFilter[v[`mip_filter[${i}]`]],
		mipmap_bias: v[`mipmap_bias[${i}]`],
	});
	const active0 = v['colorop[0]'] !== 0 || v['alphaop[0]'] !== 0;
	const active1 = v['colorop[1]'] !== 0 || v['alphaop[1]'] !== 0;
	const tsStages = active0 ? (active1 ? 2 : 1) : 0;
	const defines = { PHOENIX_SHADER_COMPILE: '1' };
	for (const [bit, mac] of BIT_MACRO) if (en & bit) defines[mac] = '1';
	if (v['tex_coord_index[0]'] === 1) defines.TEX0_UV1 = '1';
	if (v['tex_coord_index[1]'] === 1) defines.TEX1_UV1 = '1';
	const missingOps = [];
	for (const i of [0, 1]) {
		for (const kind of ['COLOR', 'ALPHA']) {
			const op = TEXOP_MACRO[v[`${kind.toLowerCase()}op[${i}]`]];
			if (!HLSL_HAS_TEXOP.has(op)) missingOps.push(`TS${i + 1}_${kind}OP=${op}`);
			defines[`TS${i + 1}_${kind}OP`] = `TEXOP_${op}(${i + 1}, ${kind})`;
			defines[`TS${i + 1}_${kind}ARG_1`] = argMacro(v[`${kind.toLowerCase()}arg1[${i}]`], i);
			defines[`TS${i + 1}_${kind}ARG_2`] = argMacro(v[`${kind.toLowerCase()}arg2[${i}]`], i);
		}
	}
	defines.ALPHA_TEST_FUNC = `TEST_${E.CompareFunc[v.alpha_func]}`;
	defines.TS_STAGES = String(tsStages);
	const usesTex = (i) => [`colorarg1[${i}]`, `colorarg2[${i}]`, `alphaarg1[${i}]`, `alphaarg2[${i}]`].some((k) => (v[k] & 0x7fffffff) === 5);
	if (usesTex(0)) defines.HAS_TEX0 = '1';
	if (usesTex(1)) defines.HAS_TEX1 = '1';
	const t = v.tfactor >>> 0;
	return {
		stages: [stage(0), stage(1)],
		d3d11: {
			blend: { enable: !!(en & 4), src: E.BlendMode[v.src_blend], dst: E.BlendMode[v.dst_blend], op: 'ADD', srcAlpha: 'ONE', dstAlpha: 'ZERO', opAlpha: 'ADD', writeMask: 'RGBA' },
			depth: { enable: !!(en & 1), write: (en & 3) === 3, func: E.CompareFunc[v.zfunc] },
			stencil: { enable: !!(en & 8) },
			raster: { cull: E.CullMode[v.cull_mode], fill: E.FillMode[v.fill_mode], frontCounterClockwise: true, depthClip: true, hwDepthBias: 0 },
			sampler: { filter: en & 0x400000 ? 'MIN_MAG_MIP_POINT' : 'ANISOTROPIC', maxAnisotropy: 16, addressW: 'CLAMP', mipLODBias: 0 },
			psConstants: { gTexFactor: [t & 255, (t >>> 8) & 255, (t >>> 16) & 255, t >>> 24].map((c) => +(c / 255).toFixed(4)), gAlphaTestValue: en & 0x80 ? +(v.alpha_ref / 255).toFixed(4) : null, gDepthBias: v.zbias },
			shaderDefines: defines,
			undefinedHlslOps: missingOps.filter((m) => (m.startsWith('TS1') ? tsStages >= 1 : tsStages >= 2)),
		},
		metal: {
			colorAttachment: en & 4 ? { blendingEnabled: true, rgbBlendOperation: 'add', sourceRGBBlendFactor: MTL_BLEND[v.src_blend], destinationRGBBlendFactor: MTL_BLEND[v.dst_blend], alphaBlendOperation: 'add', sourceAlphaBlendFactor: 'one', destinationAlphaBlendFactor: 'zero' } : { blendingEnabled: false },
			depthStencil: { depthCompareFunction: en & 1 ? MTL_CMP[v.zfunc] : 'always', isDepthWriteEnabled: (en & 3) === 3 },
			cullMode: MTL_CULL[v.cull_mode], frontFacingWinding: 'counterClockwise', triangleFillMode: MTL_FILL[v.fill_mode],
			sampler: [0, 1].map((i) => ({ minFilter: en & 0x400000 ? 'nearest' : 'linear', magFilter: en & 0x400000 ? 'nearest' : 'linear', mipFilter: en & 0x400000 ? 'nearest' : 'linear', maxAnisotropy: en & 0x400000 ? 1 : 16, sAddressMode: MTL_ADDR[v[`address_u[${i}]`]], tAddressMode: MTL_ADDR[v[`address_v[${i}]`]], rAddressMode: 'clampToEdge' })),
		},
	};
}

const doc = {
	_about: 'Rise of Nations EE named render states (Scene::init_render_states @008ba450), decoded. Generated by re/tools/renderstates.mjs; see docs/renderstates.md.',
	enums: {
		CompareFunc: E.CompareFunc, BlendMode: E.BlendMode, CullMode: E.CullMode, FillMode: E.FillMode, ShadeMode: E.ShadeMode, StencilOp: E.StencilOp,
		TexAddress: E.TexAddress, MIPFilter: E.MIPFilter, TextureOp: E.TextureOp, TextureArg: E.TextureArg,
		enablesBits: Object.fromEntries(Object.entries(ENABLE_BITS).map(([b, n]) => ['0x' + Number(b).toString(16), n])),
	},
	defaults: defaults(),
	states: [],
};
let users = {};
try { users = JSON.parse(fs.readFileSync(path.join(root, 're/iface/renderstate_users.json'), 'utf8')); } catch { /* optional */ }
for (const o of out) {
	const v = o.values;
	const bad = o.badTeamColorOverrides && Object.keys(o.badTeamColorOverrides).length ? o.badTeamColorOverrides : null;
	const b = backend(v);
	doc.states.push({
		index: o.index, name: o.name, nameStringIndex: o.nameStringIndex,
		enables: { raw: '0x' + (v.enables >>> 0).toString(16), bits: bitNames(v.enables >>> 0) },
		blend: { src: E.BlendMode[v.src_blend], dst: E.BlendMode[v.dst_blend] },
		alphaTest: { func: E.CompareFunc[v.alpha_func], ref: v.alpha_ref },
		depth: { func: E.CompareFunc[v.zfunc], bias: v.zbias },
		cull: E.CullMode[v.cull_mode], fill: E.FillMode[v.fill_mode], shade: E.ShadeMode[v.shade_mode],
		tfactor: '0x' + (v.tfactor >>> 0).toString(16).padStart(8, '0'),
		stages: b.stages,
		badTeamColor: bad && { overrides: Object.fromEntries(Object.entries(bad).map(([k, x]) => [k, decodeValue(k, x)])), d3d11ShaderDefines: backend({ ...v, ...bad }).d3d11.shaderDefines },
		special: o.special,
		pixelShaderSituations: o.situations.length ? o.situations : undefined,
		usedBy: users[o.index] ?? [],
		d3d11: b.d3d11,
		metal: b.metal,
		raw: v,
	});
}
const outFile = path.join(root, 're/iface/renderstates.json');
fs.writeFileSync(outFile, JSON.stringify(doc, null, '\t') + '\n');

if (process.argv.includes('--md')) {
	const ts = (st, i) => { const s = st.stages[i]; return `${s.colorop}(${s.colorarg1}, ${s.colorarg2}) / ${s.alphaop}(${s.alphaarg1}, ${s.alphaarg2})`; };
	const short = (s) => s.replace(/SELECTARG/g, 'SEL').replace(/MODULATE/g, 'MOD').replace(/BLENDTEXTUREALPHA/g, 'BLENDTEXA');
	console.log('| # | Name | enables | Blend src/dst | Alpha test | Z func / write / bias | Cull | Stage 0 color / alpha | Stage 1 color / alpha | Addr0 / Addr1 | tfactor | Notes |');
	console.log('|---|---|---|---|---|---|---|---|---|---|---|---|');
	for (const st of doc.states) {
		const en = parseInt(st.enables.raw, 16);
		const notes = [];
		if (st.badTeamColor) notes.push('bad_team_color: ' + Object.entries(st.badTeamColor.overrides).map(([k, x]) => `${k}=${x}`).join(', '));
		if (st.special) notes.push(st.special);
		if (en & 0x400000) notes.push('point sampling');
		if (st.fill !== 'SOLID') notes.push(`fill ${st.fill}`);
		if (st.stages[0].texcoord !== 0 || st.stages[1].texcoord !== 1) notes.push(`texcoord ${st.stages[0].texcoord}/${st.stages[1].texcoord}`);
		if (st.d3d11.undefinedHlslOps.length) notes.push('HLSL lacks ' + st.d3d11.undefinedHlslOps.join(', '));
		const z = en & 1 ? `${st.depth.func} / ${(en & 3) === 3 ? 'on' : 'off'}${st.depth.bias ? ' / ' + st.depth.bias : ''}` : 'off';
		const blend = en & 4 ? `${st.blend.src} / ${st.blend.dst}` : 'off';
		const at = en & 0x80 ? `${st.alphaTest.func} ${st.alphaTest.ref}` : 'off';
		const s1 = st.stages[1].colorop === 'DISABLE' && st.stages[1].alphaop === 'DISABLE' ? '—' : short(ts(st, 1));
		const addr = (i) => (st.stages[i].address_u === st.stages[i].address_v ? st.stages[i].address_u : `${st.stages[i].address_u},${st.stages[i].address_v}`);
		const extra = st.enables.bits.filter((b) => !['ZENABLE', 'ZWRITE', 'ALPHABLEND', 'ALPHATEST'].includes(b));
		console.log(`| ${st.index} | ${st.name} | ${st.enables.raw}${extra.length ? ' (' + extra.join(', ') + ')' : ''} | ${blend} | ${at} | ${z} | ${st.cull} | ${short(ts(st, 0))} | ${s1} | ${addr(0)} / ${addr(1)} | ${st.tfactor === '0xffffffff' ? '' : st.tfactor} | ${notes.join('; ')} |`);
	}
}
export { out, E, decodeValue, unhandled, istr };
