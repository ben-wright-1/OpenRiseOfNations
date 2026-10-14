// Reference model of Rise of Nations: EE unit path search (PathFinder::astar_path @683770 and the find_upath wrapper
// @682f30), written from the decompilation for docs/movement-spec.md. It is a specification aid, not engine code:
// every arithmetic step, the neighbour order, the open-list tie-break and the path emission order follow the original.
//
// The world queries (validity, cost, regions) are injected through `ctx` because they belong to the world/terrain
// model (docs/world-spec.md). Names of the injected functions follow the original methods they stand for.
//
//   import { astarPath, vectorDist, findUpathPost } from './astar-ref.mjs'
//
// Self-test (open grid, no obstacles):  node re/tools/astar-ref.mjs --selftest

// ------------------------------------------------------------------------------------------------ primitives

/** vector_dist(dx, dy) @46cff0: integer octagonal distance. Inputs are absolute values (callers abs() first). */
export function vectorDist(dx, dy) {
	const a = Math.abs(dx) >>> 0, b = Math.abs(dy) >>> 0;
	let M, m;
	if (a > b) { M = a; m = b; } else { M = b; m = a; }
	if (M === 0) return 0;
	if (m >= 60000) return ((m + 2 * M) >>> 1) | 0;             // LEA EAX,[m + M*2]; SHR 1 (unsigned)
	return (Math.floor(((m * m) >>> 0) / ((2 * M) >>> 0)) + M) | 0; // unsigned 32-bit m*m, unsigned DIV
}

/** C-style signed division (truncate toward zero), 32-bit. */
export const idiv = (a, b) => (a / b) | 0;

/** move_x / move_y @adcaf0 / @adc400, direction index 1..8 (0 unused). */
export const MOVE_X = [0, -1, 0, 1, 1, 1, 0, -1, -1];
export const MOVE_Y = [0, -1, -1, -1, 0, 1, 1, 1, 0];

/** div_3_table lookups: Coord → UCoord / TCoord / WCoord. div_3_table[v] = floor(v/3), negative v included
 *  (init_coord_lookup_array @681db0), and the shift is arithmetic, so these are floor divisions. */
export const div3 = (v) => Math.floor(v / 3);
export const toU = (c) => div3(c >> 4);   // /48
export const toT = (c) => div3(c >> 6);   // /192
export const toW = (c) => div3(c >> 8);   // /768

// ------------------------------------------------------------------------------------------------ open list
// Tree<PathNode*,int> keyed by node.value (f): an unbalanced BST whose ordered_insert @4796f0 descends left when
// key <= node.key, so a new node lands in-order before every existing node with an equal key. first_open_node()
// removes the leftmost node, so the open list is "lowest value first, newest first among equal values" (LIFO ties).
// In-order position is preserved by removals, so a sorted array with this insertion rule is an exact model.
export let OPEN_TIE_NEW_FIRST = true;
export function setOpenTieNewFirst(v) { OPEN_TIE_NEW_FIRST = !!v; }

class OpenList {
	constructor() { this.a = []; this.byMetric = new Map(); }
	get length() { return this.a.length; }
	insert(node) {
		// stable position among equal keys
		let lo = 0, hi = this.a.length;
		while (lo < hi) {
			const mid = (lo + hi) >> 1, k = this.a[mid].value;
			if (k < node.value || (k === node.value && !OPEN_TIE_NEW_FIRST)) lo = mid + 1; else hi = mid;
		}
		this.a.splice(lo, 0, node);
		this.byMetric.set(node.metric >>> 0, node);
	}
	popMin() { const n = this.a.shift(); if (this.byMetric.get(n.metric >>> 0) === n) this.byMetric.delete(n.metric >>> 0); return n; }
	find(metric) { return this.byMetric.get(metric >>> 0) || null; }
	remove(node) { const i = this.a.indexOf(node); if (i >= 0) this.a.splice(i, 1); if (this.byMetric.get(node.metric >>> 0) === node) this.byMetric.delete(node.metric >>> 0); }
}

const newNode = () => ({ x: 0, y: 0, length: 0, estimate: 0, value: 0, timeout: 0, metric: 0, transport: 0, building: 0, parent: null });

// ------------------------------------------------------------------------------------------------ astar_path
/**
 * astar_path(stack, step, fast) — one search (no time-slicing resume; `saving` paths are described in the spec).
 * stack: array used as Stack<PathData> (top = last element), entries {x, y, tol, flags}. On entry the top is the
 *        start, below it the goal; the tolerance of the entry below the goal (or 0) is the arrival tolerance.
 * step:  0x30 (UCoord), 0xc0 (TCoord) or 0x300 (WCoord).
 * ctx:   { xs, tw, mult, offX, offY, useOffset, antiUnit, limit, noTransportTol, isTransportUnit,
 *          validU(x,y,metric), validT(tx,ty) (= !UnitData::invalid_loc(tx,ty,0,1,1,1,0)), validW(x,y,depth,gx,gy),
 *          calcCost(px,py,x,y,dir,step,depth,node) → int (0x7fffffff = blocked; may set node.transport),
 *          isBuildingTile(tx,ty), onCapUcoord(), random(lo,hi) }
 * returns 1 (path pushed), 0 (failure), -1 (would time-slice: limit exceeded; caller must resume).
 */
export function astarPath(stack, step, fast, ctx) {
	let cap = 50, dirinc = 1, mult = 1, width;
	if (step === 0x30) { cap = 500; mult = Math.max(1, ctx.mult); width = ctx.xs << 4; dirinc = fast ? 2 : 1; }
	else if (step === 0xc0) width = ctx.tw;
	else width = ctx.xs;
	const stride = mult * step;

	const s = stack.pop();
	const g = stack.pop();
	const tol = stack.length ? stack[stack.length - 1].tol : 0;
	const gx = g.x, gy = g.y;
	const open = new OpenList(), closed = new Map();
	const hscale = (h) => (step === 0x30 ? h * 10 : idiv(h * 60, step));

	const start = newNode();
	start.x = s.x; start.y = s.y;
	if (step === 0x300) start.metric = toW(s.x) + toW(s.y) * ctx.xs;
	else if (step === 0xc0) start.metric = toT(s.x) + toT(s.y) * ctx.tw;
	else start.metric = toU(s.x) + toU(s.y) * ctx.xs * 16;
	const dx0 = s.x - gx, dy0 = s.y - gy;
	start.estimate = start.value = hscale(vectorDist(dx0, dy0));
	open.insert(start);
	// initial direction: expansion starts at dir0+1, the orthogonal step toward the goal
	const dir0 = Math.abs(dy0) < Math.abs(dx0) ? (gx < s.x ? 7 : 3) : (s.y <= gy ? 5 : 1);

	let expansions = 0;
	const goalTol = idiv(tol, 2) + stride;
	while (open.length) {
		const n = open.popMin();
		const man = Math.abs(n.x - gx) + Math.abs(n.y - gy);
		const capHit = expansions >= cap * 64;
		const sliceHit = ctx.antiUnit && ctx.limit < expansions;
		if (man <= goalTol || capHit || sliceHit) {
			if (sliceHit && man > goalTol && !fast) { open.insert(n); return -1; }
			let reached = 0;
			if (!(expansions < cap * 64 || fast)) {
				if (step === 0xc0 && ctx.antiUnit) return 0;
				if (step === 0x30) { ctx.onCapUcoord && ctx.onCapUcoord(); return 0; }
				// closest-node fallback (0xc0 without anti_unit, and 0x300): drain the open list in pop order and keep
				// the node strictly closer to the goal by vector_dist (ties keep the earlier one)
				let best = n, bd = vectorDist(n.x - gx, n.y - gy);
				while (open.length) {
					const q = open.popMin(), qd = vectorDist(q.x - gx, q.y - gy);
					if (bd > qd) { best = q; bd = qd; }
				}
				if (ctx.needsTransportFlag && ctx.needsTransportFlag(best, gx, gy)) stack[stack.length - 1].flags |= 4;
				reached = 1;
				return emitPath(stack, best, step, reached, ctx), 1;
			}
			return emitPath(stack, n, step, reached, ctx), 1;
		}
		let dir = dir0 + 1;
		for (let cnt = 1; cnt < 9; cnt += dirinc, dir += dirinc) {
			const d = dir < 9 ? dir : dir - 8;
			const p = newNode();
			p.x = MOVE_X[d] * stride + n.x;
			p.y = MOVE_Y[d] * stride + n.y;
			p.metric = (MOVE_X[d] + n.metric + MOVE_Y[d] * width) | 0;
			let ok;
			if (step === 0x300) {
				ok = n.timeout < 2 ? ctx.validW(p.x + ctx.offX - 0x180, p.y + ctx.offY - 0x180, n.timeout, gx, gy)
					: ctx.validW(p.x, p.y, n.timeout, gx, gy);
			} else if (step === 0xc0) ok = ctx.validT(toT(p.x), toT(p.y));
			else if (mult < 2 || (d & 1) === 0) ok = ctx.validU(p.x, p.y, p.metric);
			else {
				ok = true;
				let ax = MOVE_X[d] * step, ay = MOVE_Y[d] * step;
				for (let k = 1; k <= mult && ok; k++) {
					ok = ctx.validU(n.x + ax, n.y + ay, (ax + n.metric + ay * width) | 0); // bug-faithful key
					ax += MOVE_X[d] * step; ay += MOVE_Y[d] * step;
				}
			}
			if (!ok) continue;
			expansions += (ctx.antiUnit && step === 0xc0) ? 5 : 1;
			p.parent = n;
			let cost = ctx.calcCost(n.x, n.y, p.x, p.y, d, step, n.timeout + 1, p);
			if (cost === 0x7fffffff) continue;
			if (p.x === gx && p.y === gy) cost = idiv(cost, 2);
			p.length = n.length + cost;
			if (closed.has(p.metric >>> 0)) continue;
			const q = open.find(p.metric);
			if (q) { if (q.length <= p.length) continue; open.remove(q); }
			const h = hscale(vectorDist(p.x - gx, p.y - gy));
			p.estimate = h; p.value = p.length + h; p.timeout = n.timeout + 1;
			if (step === 0xc0 && idiv(h, 32) + p.timeout > 120) continue;
			open.insert(p);
		}
		closed.set(n.metric >>> 0, n);
	}
	if (step === 0x30 && ctx.onEmptyUcoord) ctx.onEmptyUcoord();
	return 0;
}

function emitPath(stack, n, step, reached, ctx) {
	if (!n.parent) return true;
	let cur = (step !== 0x300 || reached || n.transport) ? n : n.parent;
	if (cur === n && step === 0xc0) {
		for (let q = n; q.parent; q = q.parent) if (ctx.isBuildingTile(toT(q.x), toT(q.y))) q.building = 1;
	}
	for (; cur; cur = cur.parent) {
		if (!cur.parent && step !== 0x30) break;
		let x = cur.x, y = cur.y, tol;
		if (step === 0x300) { tol = 0x180; if (ctx.useOffset) { x += ctx.offX - 0x180; y += ctx.offY - 0x180; } }
		else if (step === 0x30) tol = 0;
		else {
			if (ctx.useOffset) { x += (ctx.offX % 0xc0) - 0x60; y += (ctx.offY % 0xc0) - 0x60; }
			tol = (!ctx.antiUnit && ctx.isTransportUnit) ? 0 : 0x60;
		}
		let flags = ctx.antiUnit ? 2 : (step === 0x30 ? 2 : 0);
		if (cur.building) flags |= 0x10;
		const top = stack[stack.length - 1];
		if (!top || x !== top.x || y !== top.y) {
			if (cur.transport) { flags |= 4; tol = 0; }
			stack.push({ x, y, tol, flags });
		}
	}
	return true;
}

// ------------------------------------------------------------------------------------------------ find_upath post-pass
/** The straightening pass find_upath @682f30 runs on a successful UCoord path (stack length > 3). */
export function findUpathPost(stack, unitX, unitY) {
	if (stack.length <= 3) return;
	const keep = [];
	let w0 = stack.pop();
	if (w0.x === unitX && w0.y === unitY && !(w0.flags & 1)) w0 = stack.pop();
	keep.push(w0);
	if ((w0.flags & 2) && stack.length > 2) {
		let ax = w0.x, ay = w0.y;
		let b = stack.pop(), c = stack[stack.length - 1];
		while ((b.flags & 2) && (c.flags & 2) && stack.length > 1) {
			let keepB, advance = true;
			if (b.flags & 4) keepB = true;
			else if (ax - b.x === b.x - c.x && ay - b.y === b.y - c.y) keepB = false;           // collinear: drop, A := B
			else if (Math.abs(ax - c.x) === 0x30 && Math.abs(ay - c.y) === 0x30) { keepB = false; advance = false; } // corner cut
			else keepB = true;
			if (keepB) keep.push(b);
			if (advance) { ax = b.x; ay = b.y; }
			b = stack.pop(); c = stack[stack.length - 1];
		}
		stack.push(b);
	}
	while (keep.length) stack.push(keep.pop());
}

// ------------------------------------------------------------------------------------------------ self-test
if (process.argv.includes('--selftest')) {
	const ctx = {
		xs: 8, tw: 32, mult: 1, offX: 0, offY: 0, useOffset: false, antiUnit: 1, limit: 1e9, isTransportUnit: false,
		validU: () => true, validT: () => true, validW: () => true, isBuildingTile: () => false,
		calcCost: (px, py, x, y, d) => 32 + (d & 1) * 8, // calc_cost at 0x30 without transport
	};
	const stack = [{ x: 0, y: 0, tol: 0, flags: 1 }, { x: 10 * 48 + 24, y: 4 * 48 + 24, tol: 0, flags: 0 }, { x: 24, y: 24, tol: 0, flags: 0 }];
	console.log('vector_dist(300,400) =', vectorDist(300, 400), ' r =', astarPath(stack, 0x30, 0, ctx));
	findUpathPost(stack, 24, 24);
	console.log(stack.slice().reverse().map((e) => `(${toU(e.x)},${toU(e.y)})f${e.flags}`).join(' '));
}
