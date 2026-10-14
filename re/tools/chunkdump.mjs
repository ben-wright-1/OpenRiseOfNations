// Dump the chunk tree of a BHG chunked file (.BH3 / .BHa).
// Chunk header (from ChunkRead in riseofnations.exe): u32 size (includes the 8-byte header), u16 id, u16 aux.
// Chunks are containers when their payload parses cleanly as a sequence of child chunks.
// Usage: node chunkdump.mjs <file> [--stats]   (--stats: only print a histogram of id paths)
import fs from 'node:fs';

const [, , file, flag] = process.argv;
const b = fs.readFileSync(file);
const stats = new Map();

function looksLikeChildren(start, end) {
  let p = start;
  if (end - start < 8) return false;
  while (p < end) {
    if (end - p < 8) return false;
    const size = b.readUInt32LE(p);
    if (size < 8 || p + size > end) return false;
    p += size;
  }
  return p === end;
}

function preview(start, end) {
  const n = end - start;
  const parts = [`${n}B`];
  if (n >= 4) parts.push(`u32[0]=${b.readUInt32LE(start)}`);
  if (n >= 8) {
    const floats = [];
    for (let p = start + 4; p + 4 <= Math.min(end, start + 20); p += 4) floats.push(b.readFloatLE(p).toPrecision(4));
    parts.push(`f32[1..]=${floats.join(',')}`);
  }
  return parts.join(' ');
}

function walk(start, end, depth, path) {
  let p = start;
  while (p + 8 <= end) {
    const size = b.readUInt32LE(p), id = b.readUInt16LE(p + 4), aux = b.readUInt16LE(p + 6);
    if (size < 8 || p + size > end) {
      if (!flag) console.log(`${'  '.repeat(depth)}!! bad chunk at 0x${p.toString(16)} size=${size}`);
      return;
    }
    const key = `${path}/${id}`;
    stats.set(key, (stats.get(key) ?? 0) + 1);
    const container = looksLikeChildren(p + 8, p + size);
    if (!flag) {
      console.log(`${'  '.repeat(depth)}@0x${p.toString(16).padStart(6, '0')} id=${id} aux=${aux} size=${size}` +
        (container ? '  [container]' : `  ${preview(p + 8, p + size)}`));
    }
    if (container) walk(p + 8, p + size, depth + 1, key);
    p += size;
  }
  if (p !== end && !flag) console.log(`${'  '.repeat(depth)}!! ${end - p} trailing bytes`);
}

walk(0, b.length, 0, '');
if (flag === '--stats') for (const [k, v] of [...stats].sort()) console.log(`${String(v).padStart(5)}  ${k}`);
