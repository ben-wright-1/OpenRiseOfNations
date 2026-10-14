#!/usr/bin/env node
// Reference for the fog-of-war stamp shapes (docs/fog-spec.md §3): the circle tables (circle_init @006817f0), the
// ring tables (ring_init @00681920) and the cell lists that Object::update_seen @00651b80 stamps for a given LOS.
// Clean-room: written from the spec, no decompiled code.
//
// Usage:
//   node re/tools/los-ref.mjs tables                       table sizes, per-radius counts and an FNV-1a hash of each table
//   node re/tools/los-ref.mjs show <los> [full|move] [--project] [--detector]
//                                                          ASCII picture of the fog cells one update_seen call stamps
//                                                          (o = stamped, D = stamped + detect (seen3), + = centre)
//   node re/tools/los-ref.mjs dump circle|ring             one line per entry: index x y (for diffing a C++ port)
//   node re/tools/los-ref.mjs dump stamps                  every stamp range for LOS 0..140 x move x project x
//                                                          detector, one line each (same format as `ron-sim --dump stamps`)
// Library: import { circles, rings, stampList } from './los-ref.mjs'

import { pathToFileURL } from 'node:url';

const CIRCLE_CAP = 0x3249; // circle_x/circle_y capacity; circle_init stops once 0x3249 points exist

// The approximate distance of circle_init / ring_init (unsigned integer division), ax = |x|, ay = |y|.
export function circleDist(ax, ay) {
	if (ay < ax) return ay < 60000 ? Math.floor((ay * ay) / (ax * 2)) + ax : (ay + ax * 2) >>> 1;
	if (ay === 0) return 0;
	return ax < 60000 ? Math.floor((ax * ax) / (ay * 2)) + ay : (ax + ay * 2) >>> 1;
}

// circle_init: ring r (0..64) = offsets with circleDist == r, x outer (-r..r), y inner (-r..r).
// radius[r] = cumulative count after ring r. If the capacity is reached, radius[r..64] = the count and it stops.
export function circles() {
	const x = [], y = [], radius = new Array(65).fill(0);
	for (let r = 0; r <= 64; r++) {
		for (let cx = -r; cx <= r; cx++) {
			for (let cy = -r; cy <= r; cy++) {
				if (circleDist(Math.abs(cx), Math.abs(cy)) !== r) continue;
				x.push(cx); y.push(cy);
				if (x.length > 0x3248) { for (let k = r; k <= 64; k++) radius[k] = x.length; return { x, y, radius, capped: true }; }
			}
		}
		radius[r] = x.length;
	}
	return { x, y, radius, capped: false };
}

const ORTHO = [[0, -1], [1, 0], [0, 1], [-1, 0]]; // orthog_x/y entries 1..4: N E S W

// ring_init: ring r (1..32) = circle ring r, plus, for every original point of that ring with fewer than 2 4-neighbours
// in the ring so far (a point on an axis counts one extra), its N/E/S/W neighbours whose circleDist is < r (appended,
// duplicates possible; appended points are searched as neighbours but not examined themselves).
// radius[0] = 1 (the single point 0,0); radius[r] = cumulative count after ring r.
export function rings(C = circles()) {
	const x = [0], y = [0], radius = new Array(33).fill(0);
	radius[0] = 1;
	for (let r = 1; r <= 32; r++) {
		const ringStart = radius[r - 1];
		for (let i = C.radius[r - 1]; i < C.radius[r]; i++) { x.push(C.x[i]); y.push(C.y[i]); }
		const origEnd = x.length;
		for (let i = ringStart; i < origEnd; i++) {
			const px = x[i], py = y[i];
			let n = 0;
			for (let j = ringStart; j < x.length; j++) if (Math.abs(py - y[j]) + Math.abs(px - x[j]) === 1) n++;
			if (px === 0 || py === 0) n++;
			if (n < 2) {
				for (const [ox, oy] of ORTHO) {
					const nx = px + ox, ny = py + oy;
					if (circleDist(Math.abs(nx), Math.abs(ny)) < r) { x.push(nx); y.push(ny); }
				}
			}
		}
		radius[r] = x.length;
	}
	return { x, y, radius };
}

// The cells one update_seen(ring) call stamps, as offsets from the stamp centre (fog cells of 384 units).
// los = ObjectData::los() (tcoords). opts: { move: ring-only update (update_seen(1)), project: the small-LOS ground-unit
// case whose centre is projected 384 units ahead (the caller supplies the projected centre), detector: SubObject
// flag DETECTOR }. Returns { r, table: 'circle'|'ring', start, end, detectEnd, cells: [{dx, dy, detect}] }.
export function stampList(los, opts = {}, C = circles(), R = rings(C)) {
	if (los === 0) return { r: 0, cells: [] };
	let r = Math.trunc((los * 192) / 384);
	if (r > 64) r = 64;
	const move = !!opts.move;
	let start = 0, ringTab = false;
	if (r < 4) {
		if (opts.project) start = move ? C.radius[Math.max(r - 5, 0) < 1 ? 0 : r - 5] : 0;
		else start = move && r - 1 >= 1 ? C.radius[r - 1] : 0;
	} else if (move) {
		if (r > 32) r = 32;
		start = R.radius[Math.max(r - 1, 0)];
		ringTab = true;
	}
	let detectEnd = 0;
	if (opts.detector) {
		if (ringTab) { if (r > 31) r = 31; detectEnd = R.radius[r]; }
		else detectEnd = C.radius[r];
	}
	let end;
	if (!move) { start = 0; end = C.radius[r]; } else end = ringTab ? R.radius[r] : C.radius[r];
	const T = ringTab ? R : C;
	const cells = [];
	for (let i = start; i < end; i++) cells.push({ dx: T.x[i], dy: T.y[i], detect: i < detectEnd });
	return { r, table: ringTab ? 'ring' : 'circle', start, end, detectEnd, cells };
}

function fnv(arr) {
	let h = 0xcbf29ce484222325n;
	for (const v of arr) { h ^= BigInt(v & 0xff); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; }
	return h.toString(16).padStart(16, '0');
}

function main(argv) {
	const C = circles(), R = rings(C);
	const cmd = argv[0] || 'tables';
	if (cmd === 'tables') {
		console.log(`circle: ${C.x.length} points (cap 0x3249 = ${CIRCLE_CAP}, ${C.capped ? 'CAPPED' : 'not reached'}); hash x ${fnv(C.x)} y ${fnv(C.y)}`);
		console.log('circle_radius:', C.radius.join(' '));
		console.log(`ring: ${R.x.length} points; hash x ${fnv(R.x)} y ${fnv(R.y)}`);
		console.log('ring_radius:', R.radius.join(' '));
		const extra = R.radius.map((v, r) => (r ? v - R.radius[r - 1] - (C.radius[r] - C.radius[r - 1]) : 0));
		console.log('ring extra (gap-fill points added per ring):', extra.join(' '));
		return;
	}
	if (cmd === 'dump' && argv[1] === 'stamps') {
		const out = [];
		for (let los = 0; los <= 140; los++) {
			for (const move of [0, 1]) for (const project of [0, 1]) for (const detector of [0, 1]) {
				const s = stampList(los, { move: !!move, project: !!project, detector: !!detector }, C, R);
				const head = `los ${los} move ${move} project ${project} detector ${detector}: r ${s.r} ` +
					`${s.table || 'circle'} [${s.start || 0}, ${s.end || 0}) detect < ${s.detectEnd || 0}:`;
				out.push(head + s.cells.map((c) => ` ${c.dx},${c.dy}${c.detect ? 'D' : ''}`).join(''));
			}
		}
		console.log(out.join('\n'));
		return;
	}
	if (cmd === 'dump') {
		const T = argv[1] === 'ring' ? R : C;
		T.x.forEach((v, i) => console.log(i, v | 0, T.y[i] | 0)); // | 0: no "-0"
		return;
	}
	if (cmd === 'show') {
		const los = Number(argv[1]);
		const s = stampList(los, { move: argv[2] === 'move', project: argv.includes('--project'), detector: argv.includes('--detector') }, C, R);
		console.log(`los ${los} → r ${s.r} fog cells, ${s.table} table [${s.start}, ${s.end}), detect < ${s.detectEnd}, ${s.cells.length} cells`);
		const m = Math.max(1, s.r);
		const grid = new Map();
		for (const c of s.cells) {
			const k = `${c.dx},${c.dy}`;
			grid.set(k, (grid.get(k) === 'D' || c.detect) ? 'D' : 'o');
		}
		for (let y = -m; y <= m; y++) {
			let line = '';
			for (let x = -m; x <= m; x++) line += grid.get(`${x},${y}`) || (x === 0 && y === 0 ? '+' : '.');
			console.log(line);
		}
		return;
	}
	console.error('usage: los-ref.mjs tables | show <los> [full|move] [--project] [--detector] | dump circle|ring|stamps');
	process.exit(2);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main(process.argv.slice(2));
