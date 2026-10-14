# OpenRiseOfNations: a Rise of Nations reimplementation for macOS

Goal: understand Rise of Nations: Extended Edition well enough to build our own
engine that runs natively on macOS and loads the game data from a user's own
install (the OpenRA / OpenMW model). Ship no original assets or decompiled code.

**Resuming work? Read `docs/STATUS.md` first** (current state and next steps).

Findings live in `docs/`: start with `docs/systems-map.md`, then `docs/formats.md` and
`docs/graphsys-interface.md`. Update them as findings change.

`.gitignore` keeps game files, Ghidra databases, decompiled C, bulk dumps extracted from the binary (`re/map`, `re/types`,
`re/iface`, `re/calltrees`, `re/analysis`) and renders out of git; keep it that way. Those dumps are regenerated
locally with the scripts in `re/ghidra_scripts/` (e.g. `DumpSymbols`, `DumpTypes`, `CallTree`).

## Target

- Install (Mac): `~/Games/RiseOfNations`, the Windows depot downloaded with SteamCMD (app 287450, 2.6 GB).
  Never modify it. Its exe and `sbl/rise.pdb` are byte-identical to `re/bin/` (checked 2026-10-11).
- Original Windows install: `C:\Program Files (x86)\Steam\steamapps\common\Rise of Nations`
- `riseofnations.exe`: 32-bit x86 MSVC, unpacked, timestamp 0x6674863f (2024-06-20), base 0x00400000
- **Full symbols ship with the game:** `sbl\rise.pdb` (GUID 51d4f219-…-c3361b0d291f, age 1) plus `sbl\rise_z.map`.
  `sbl\` also has PDBs and maps for `d3dgl.dll` (the renderer backend) and the other DLLs.
- Windows saves and replays live under `Documents\My Games\Rise of Nations` of the Windows user

## Reverse-engineering workspace (`re/`)

| Path | What |
|---|---|
| `re/bin/` | Working copies: exe + `rise.pdb` + map, `d3dgl.dll` + PDB + map. Never touch the Steam install |
| `re/ghidra/RON.gpr` | Analyzed Ghidra project for the exe, **PDB applied**. `re/ghidra_d3dgl/` holds the renderer DLL |
| `re/map/` | `functions.tsv` (63k symbols: rva/obj/class/method), `modules.txt`, `classes.txt`, `strings.tsv` |
| `re/calltrees/`, `re/decomp/<topic>/` | Call trees and decompiled C per topic |
| `re/ghidra_scripts/` | `SetPdb`, `CallTree`, `DecompileFunctions`, `DumpVtables`, `ArgFiles` helper |
| `re/tools/` | `mapsummary.mjs`, `strings.mjs`, `chunkdump.mjs` (BH3/BHa), `ghidra-run.sh` (`.ps1` is the Windows original) |
| `re/jobs/` | Argument files for Ghidra script runs |

Run Ghidra scripts against the saved project (read-only, about 5 s per run, no re-analysis):

```
re/tools/ghidra-run.sh [--project RON_d3dgl] DecompileFunctions.java re/jobs/<args>.txt
```

The argument file has one entry per line. For DecompileFunctions: the output dir, then regexes over fully qualified names
(e.g. `Unit::do_attack.*`). For CallTree: the output file, the depth, then root names. Relative paths resolve against the repo
root. Arguments go through files (a Windows-era workaround for cmd.exe mangling regexes) and the scripts still expect that.

Tooling is Node and bash; Python 3 is also available on the Mac.

## Engine code and the Mac test loop

- `engine/assets/`: portable C++17 asset library (chunks, BH3, BHa, TGA, unit catalog, posing). No platform deps.
- `apps/viewer-mac/main.mm`: AppKit + Metal unit viewer; shaders are embedded and compiled at runtime.
  `apps/check/main.cpp`: `ron-check`, a CLI that parses and poses every catalog asset.
- Build in the repo root with `bash build-mac.sh` (Command Line Tools / Xcode clang), then
  `./build/ron-check <game dir>` and `./build/ronviewer <game dir> [--find name]`. File lookups are case-insensitive.
- `./build/ronmap <game dir> <scenario.scx>` renders a scenario (terrain, trees, `--build`/`--unit` placements) with the
  game's camera. Both apps take `--snapshot out.png`: **use snapshots to check rendering yourself** (read the PNG).
- `node re/tools/*-preview.mjs` are software reference renderers (terrain, buildings, attachments) writing to `re/previews/`.
- Code layout: `engine/assets` (readers), `engine/render` (render states, mesh builders, lighting), `platform/metal`
  (Metal backend: combiner shader, state cache, post chain, unit drawing), `apps/` (check, viewer, map).
- `ron-check ~/Games/RiseOfNations` baseline: 802/802 models, 1823/1826 anims, `failures: 0`. The 3 missing .bha files and
  `HERDSEALS-TYPE2`'s `texture=".\art\sealbrown.bh3"` (reported as a game data error) are bugs in the shipped data.

## Tooling notes

- Files from the Windows era may have CRLF; `.gitattributes` normalizes to LF in git.
- Ghidra 12.1.4 and `openjdk@21` come from Homebrew (`brew install ghidra`); `ghidra-run.sh` finds both itself.
  Open `re/ghidra/RON.gpr` in the GUI with `ghidraRun`. If Ghidra refuses to open a project created by another user,
  change the OWNER in its `project.prp` to your user name.
- SteamCMD: `brew install --cask steamcmd`. It needs an interactive Steam login, which the user does themselves.
- The binary uses identical-COMDAT folding: a vtable slot or call target with an unrelated name (e.g.
  `BaseParamRegister::on_field_change`) is often a folded trivial body (`return 0;`), not a real call.
- Claude Code skills can be installed locally in `.claude/skills/` (gitignored), e.g. ghidra-headless,
  binary-analysis-patterns, reverse-engineering, interpreters, cpp-testing, Metal and Apple-platform skills.

## Porting constraints already established

- **Lockstep determinism:** all player input goes through `CommandManager::issue_*` → `CommandPackage` → `process_*`.
  The simulation (`Game::do_frame`) must be bit-identical across machines. Be careful with float behaviour
  (the original is x86 SSE2) and iteration order.
- **The renderer seam is `GraphSys`** (153 virtual slots) plus `GSGraphicCard` (42 slots). The backend also owns the window
  and input, so the macOS backend is Metal plus AppKit or SDL.
- `.rcx` replays (gzip: header + embedded save + command packages) are the test oracle for simulation fidelity.
