// Extract ASCII and UTF-16LE strings from a binary, with file offsets.
// Usage: node strings.mjs <file> [minLen=4] > out.tsv   (columns: offset, enc, text)
import fs from 'node:fs';

const [, , file, minArg] = process.argv;
const min = Number(minArg ?? 4);
const b = fs.readFileSync(file);
const printable = c => c >= 32 && c < 127;
const rows = [];

let start = -1;
for (let i = 0; i <= b.length; i++) {
  if (i < b.length && printable(b[i])) { if (start < 0) start = i; continue; }
  if (start >= 0 && i - start >= min) rows.push([start, 'a', b.toString('latin1', start, i)]);
  start = -1;
}
for (const align of [0, 1]) {
  start = -1;
  for (let i = align; i + 1 <= b.length; i += 2) {
    const ok = i + 1 < b.length && printable(b[i]) && b[i + 1] === 0;
    if (ok) { if (start < 0) start = i; continue; }
    if (start >= 0 && (i - start) / 2 >= min) rows.push([start, 'w', b.toString('utf16le', start, i)]);
    start = -1;
  }
}
rows.sort((x, y) => x[0] - y[0]);
process.stdout.write(rows.map(r => `${r[0].toString(16).padStart(8, '0')}\t${r[1]}\t${r[2].replace(/[\t\r\n]/g, ' ')}`).join('\n') + '\n');
