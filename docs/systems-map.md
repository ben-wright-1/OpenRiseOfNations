# Rise of Nations: Extended Edition — systems map

Target: `riseofnations.exe`, 32-bit x86, MSVC, built 2024-06-20 (timestamp `0x6674863f`).
Symbols: `sbl\rise.pdb` (exact GUID match) and `sbl\rise_z.map`. Every name below is the
original Big Huge Games / Skybox name from those files, not a guess.

Evidence files (regenerate with `re/tools/*.mjs` and `re/ghidra_scripts/*.java`):
- `re/map/modules.txt`: functions per object file, with the dominant classes in each
- `re/map/classes.txt`: functions per class, with the object files each class lives in
- `re/map/functions.tsv`: all 63,427 code symbols (rva, obj, class, method, mangled name)
- `re/map/strings.tsv`: all ASCII/UTF-16 strings with file offsets
- `re/ghidra/RON.gpr`: Ghidra project with the PDB applied (open in the Ghidra GUI)
- `re/calltrees/*.txt`, `re/decomp/**.c`: call trees and decompiled functions

## Startup → game → frame (from decompilation; see `re/calltrees/*.txt`, `re/decomp/core/`)

```
WinMain @560940
└─ Main::main @560cf0
   ├─ Main::init                    telemetry, crossplay, MultiplayerManager
   ├─ ModManager::buildModPackages   mod overlay filesystem
   ├─ XML::check_xml_version         MSXML via COM (CoCreateInstance)
   ├─ Version::init, Main::init_string_tables
   ├─ SyncLogger::initialize, Game::load_steam_net_lib
   ├─ System::init @599700           init_graphics (GraphSys/d3dgl.dll), Video, ConsoleWin, KeyMap, init_sound, Taunts
   ├─ Main::init_menus, Main::init_common_data → Game::init_common_data → init_rules_data + init_non_rules_data (Types::init)
   └─ Main::run_game_loop            AutoSave, GameInfo config, JukeBox intro, then a mode:
      Game::run_solo / run_scenario / run_campaign / run_playback / run_gamespy / run_editor ...
      └─ Game::run @584590
         ├─ GameMod::load, CommandManager::check_mp_playback, RecordGame::init_record
         ├─ Game::init_rules_and_teams, Game::init @58c480 (World, TileSet, Rivers, WorldMap, Setup::build_game, ...)
         ├─ CommandManager::init/start, TurnControl::init, RecordGame::write_save_game/write_rules
         └─ while (Game::loop())                                   ← one iteration per OS frame
```

`Game::loop` @591570:
1. `NetDaemon::process_all` (network pump)
2. `TurnControl::do_frame()` returns whether a **simulation step** may run this frame (lockstep pacing). If so, `Game::do_frame`
3. `Game::loop_render(1, render_mode)`. Rendering is decoupled from simulation; it predicts and interpolates using
   `predicted_render_time`, `graphic_percent` and `graphic_dt`, and may skip renders (up to 10) to catch up

`Game::do_frame` @591ef0, the fixed simulation step (order matters for determinism):

| # | Step |
|---|---|
| 1 | Scenario and "general powers" scripts (`RunTimeEnv::run_script`); conquest reinforcements |
| 2 | `Leaders::process_all` (per-player economy and state); rush-rules war declaration at the configured frame |
| 3 | `Leaders::strategy_all` (AI), `GameDaemon::process_all`, `Armies::process_all` |
| 4 | **`Objects::process_all`** (every unit and building) then `Objects::inc_time` |
| 5 | `GraphicEvents::process` |
| 6 | `Leaders::end_process_all`, `Achieve::capture_data`, `Leader::process_event_frame` (each player) |
| 7 | `frame++`; `OrdersMemManager::cycle`; `Roads::scan_and_kill_stray_roads`; **every 15 frames `tick++`** and `TurnControl::check_cannon_time` |
| 8 | Optional autosave, end-game check (`process_end_game`) |

Full step-by-step detail (iteration orders, object lifecycle, `Unit::process` / `Build::process`): [frame-spec.md](frame-spec.md).
Command → group → order → `Unit::work` pipeline: [orders-spec.md](orders-spec.md).
Combat (target acquisition, ATTACK orders, `fight`, projectiles, damage formula, death, healing, every combat
`game_random` draw and a port plan): [combat-spec.md](combat-spec.md), reference tool `re/tools/combat-ref.mjs`.

Before step 1, `do_frame` queues a PLAYER_SPEED statistics command when `(frame & 7) == console.play`. That command runs in the next turn (replay-spec.md §13).

`NetDaemon::process_all` is called between steps; it only moves network messages and does not change the simulation.
Wall-clock reads (`timeGetTime`) feed only profiling and render prediction, never simulation state.

## Code size by origin

| Origin | Functions | What it is |
|---|---|---|
| game (no lib prefix) | 49,641 | Rise of Nations itself, 376 object files |
| `basic` | 9,939 | Skybox Labs EE layer: Steam, lobby, crossplay, telemetry, mods, filesystem, strings |
| `bighuge` | 2,948 | BHG's in-house engine lib: windows/UI widgets, GraphSys front end, fonts, sprites, anim manager |
| `pnglib`, `zlib` | 195 | libpng, zlib |
| CRT etc. | ~300 | MSVC runtime, UnicoWS thunks |

For a port, most of `basic` (Steam, GameSpy, Xbox telemetry, crossplay lobby) gets replaced or dropped.

## Architectural conventions

### X / XData / XOut triples

The codebase separates state, logic and presentation per game object kind:

- `XData` holds plain simulation state plus queries (`UnitData::is_idle`, `hits`, `get_speed`, ...)
- `X` holds simulation behavior (`Unit::do_attack`, `find_path`, `die`, ...)
- `XOut` holds rendering and UI for the object (`UnitOut::draw`, `draw_health_bar`, `draw_shadows`, ...)

There are 86 `*Data` classes, and about 60 of them have a matching `*Out`. Among them: Unit, Build,
City, World, Terrain, Rivers, Roads, Borders, Group, Army, Hero, Leader, Wonder, Nuke, Storm, Animal,
Caravan, PathFinder and all the `*Type` classes. This maps directly onto a port: `Data` + `X` is
the deterministic simulation and `Out` is the renderer-facing layer.

### Lockstep multiplayer (determinism is a hard requirement)

Player input never mutates the simulation directly:

1. UI calls `CommandManager::issue_<verb>`. There are about 70 verbs: move_to, attack, build, gather, garrison, tribute, ...
2. Commands are queued into a `CommandPackage` for a future turn (`turncontrol.obj`, `commandmanager.obj`).
3. Every peer runs `CommandPackage::process_<verb>` for the same turn, in the same order.
4. Peers exchange checksums (`issue_check_sums`, `process_check_random`, `checksums.obj`,
   `SyncLogger`), and on mismatch the game runs `CommandManager::recover_from_oos` / `Game::run_oos_recovery`.

Replays (`.rcx`, `recordgame.obj`) are therefore command logs, and a reimplementation that plays
an original `.rcx` to the same end state proves simulation fidelity.

Details in [replay-spec.md](replay-spec.md): the byte layout of all 82 verbs, the multiplayer XOR and padding, GROUP and current-group
state, turn timing (single player: 1 frame per turn; multiplayer: `ceil(max(2×ping, 400–600 ms)/frame_ms)` frames, one-turn latency),
CHECK_SUMS (Adler-32 per component, multiplayer only), and the single simulation RNG `game_random` (LCG `×0x19660D + 0x3C6EF35F`, seeded
from `GameInfo.seed`, shared by map generation, AI, units and `Guy` animation choice).

## Rendering

Three layers:

1. **Game-side drawing**: `*Out::draw`, `Scene::render`, `GraphicPieces`, `TerrainOut`, `BasicRenderMethods`
   (`BH3Method`, `MOTMethod`), `BasicRenderPackets` (`AnimMeshPacket`, `SpritePacket`, `Vert3Packet`, ...).
2. **`GraphSys`**: an abstract device interface with about 80 virtual methods, among them `begin_scene`, `set_texture`,
   `set_world_transform`, `set_projection`, `set_render_state` and `init_shaders`. Created through `GraphSys::init_system`
   in `bighuge:graphsys.obj`.
3. **`d3dgl.dll`**, the backend. Its only export is `get_graphsy_object_ptr`, which returns a `D3D11GLSys` (the GraphSys
   implementation) that drives a `D3D11GLGraphicCard`. It has its own PDB and map (`sbl\d3dgl.pdb`, `sbl\d3dgl.map`).

HLSL shaders ship as source in `shaders\` (`vs_global.hlsl`, `ps_global.hlsl`, `ps_tonemap.hlsl`, FXAA, ...). The lighting
constants, HDR pass list and tone-mapping/bloom/FXAA parameters are in [lighting-spec.md](lighting-spec.md). Sea water, ocean floor, river
water, surf and the map border are in [water-spec.md](water-spec.md).

The backend also owns the **window, input and event pump** (`SGLSys::do_key/do_mouse/do_timer/...`). The full slot-by-slot
contract is in [graphsys-interface.md](graphsys-interface.md).

In-game interface (interface.xml layout and rect rules, UI render states and draw order, fonts.xml/FreeType text,
string tables and languages, HUD panels, mouse/keyboard/hotkeys/control groups, cursors, port plan): [ui-spec.md](ui-spec.md).

Inside the game, each graphic is a data-driven chain of render **Methods** (`BH3Method` mesh load, `MOTMethod` animation,
`Basic/Banker/Boat/Tree/VShader TransformMethod`, `TextureMethod`, `RenderStateMethod`, `SpriteMethod`, ...) that exchange
typed **Packets** (`Vert4Packet`, `NormPacket`, `TexcrdPacket`, `FacePacket`, `HierarchyPacket`, `AnimationPacket`, ...).

**Port seam:** reimplement the `GraphSys` contract on Metal (from the d3dgl.dll vtable and its call sites) and
translate the HLSL to MSL. Nothing above layer 2 needs to know.

## Data and content

Byte-level details: [formats.md](formats.md). The key mechanism is the **DataWalk** visitor: 263 classes implement
`walk_data(DataWalk*)`, and the same traversal drives save, load, per-subsystem checksums (`CheckSums::check_*`) and sync logs.
Together those methods make up the authoritative list of simulation state: see [state-spec.md](state-spec.md)
(save format, top-level order, checksum parts) and [state-walk.md](state-walk.md) (every class's walk).

| Content | Format | Code |
|---|---|---|
| Rules: units (364), buildings, techs, goods, crafts, resources, tribes | XML with DTD and XMLSpy `.sps`; field comments are inline | `Types`, `UnitType`, `BuildType`, `TechType`, `GoodType`, `basic:xml.obj` |
| Rules sanity checks | Counts are asserted against compile-time enums (`NUM_UNITTYPES`, `NUM_BUILDTYPES`, `NUM_TECHTYPES`, ...) | `Types::init` |
| Rules data model | Full load order, parse rules, field tables, rules image: [rules-spec.md](rules-spec.md); tool `re/tools/rulesdump.mjs` reproduces a replay's rules image byte-exactly | `Types::init`, `Game::walk_rules_data` |
| Localized strings | `*.xml.<langid>` variants | `basic:stringtable.obj` |
| Graphics catalogs | `unit_graphics.xml`, `building_graphics.xml` ([buildings-spec.md](buildings-spec.md); farms: [farms-spec.md](farms-spec.md)), `anim_graphics.xml`, `effects_graphics.xml` | `GraphicPieces` |
| 3D models | `.BH3`, chunked binary (u32 size, u16 type headers; float4 vertices) | `BH3Method`, `BasicRenderPackets` |
| Animations | `.BHa`, chunked binary | `bighuge:animmgr.obj`, `AnimationPacket` |
| Textures | `.tga`, `.png` | `bighuge:imageio.obj`, `bighuge:texture.obj` |
| Scripts (AI, scenarios, triggers) | `.bhs`, plain-text BigHugeScript (C-like); language, compiler, bytecode, VM, runtime hooks and builtins: [script-spec.md](script-spec.md) (tools `re/tools/bhs-parse.mjs`, `bhs-builtins.mjs`) | `lexer`, `compiler`, `SyntaxTree`, `SymTable`, `VirtualMachine`, `opcodes`, `scriptfunctions` (842 `ScenarioFuncSet` builtins + 31 utility builtins) |
| Scenarios and maps | `.scx`, `.sce`: gzip + chunk tree ([terrain-spec.md](terrain-spec.md), objects and players: [scenario-objects-spec.md](scenario-objects-spec.md)) | `scenariofile.obj` (`ScenarioRead`/`ScenarioWrite`), `scenariodata.obj` |
| Random maps | XML map styles (`mapstyles\`) | `map.obj`, `TerrainGroups`, `Tileset` |
| Saves and replays | binary saves; `.rcx` replays | `save.obj` (`SaveGame`/`LoadGame`), `recordgame.obj` |
| Audio | `.wav` plus `soundtypes.xml` | `soundglobal.obj`, `soundtype.obj`, `jukebox.obj` |
| Filesystem and mods | Skybox virtual FS with a mod overlay | `basic:Skybox_Filesystem.obj`, `basic:Skybox_ModManager.obj` |

## Simulation subsystems (by module)

| Subsystem | Main modules |
|---|---|
| Frame loop and game modes | `game.obj` (`Game::run_*`, `loop`, `do_frame`, `loop_render`), `gamemain.obj`, `main_win32.obj` |
| World grid and fog | `world.obj` (`World`, `WorldData`), `regions.obj`, `collcheck.obj`, `TerrainVis.obj`; spec: [world-spec.md](world-spec.md) (grid bits, load-time rebuild, regions, building footprints, unit occupancy, passability); fog of war / LOS: [fog-spec.md](fog-spec.md) (seen grids, LOS radius, stamp shapes, refresh every 100 frames, queries, renderer overlay; tool `re/tools/los-ref.mjs`) |
| Units, orders and combat | `Unit.obj`, `orders.obj`, `groups.obj`, `army.obj`, `Ammo.obj`, `caster.obj`, `unitbalance.obj` ([combat-spec.md](combat-spec.md)) |
| Pathfinding and movement | `PathFinder.obj` (A* over UCoord/TCoord/WCoord steps; road, river and caravan variants), `Unit.obj` (`do_move`, `move_step`), `Guy.obj`, `groups.obj`/`forms.obj`: [movement-spec.md](movement-spec.md) |
| Buildings, cities and borders | `Build.obj`, `City.obj`, `cities.obj`, `Borders.obj`, `Wall.obj`, `forts.obj`, `wonders.obj`; spec: [production-spec.md](production-spec.md) (costs and escalation, times, placement, construction, cities, wonders, training queues, rally points, research and ages, `Build::process`, every RNG draw, port plan; tool `re/tools/production-ref.mjs`) |
| Capture, elimination, victory | `Build::check_capture`, `Cities::capture_city`, `City::capture` / `close` / `assimilate`, `swap_team`, `Leader::lost_a_city` / `defeat` / `defeat_by` / `victory`, `Game::check_victory`, `GameDaemon::process_victory`; spec: [capture-spec.md](capture-spec.md) (capturability, the strength scan, ownership transfer of buildings and units, plunder, capital clock, defeat and the game-over test, victory clocks, every RNG draw) |
| Economy | `leaders.obj` (`Leader::gather`/`calc_gather`/`do_gather`), `farms.obj`, `caravan(s).obj`, `oilwells.obj`, `Good.obj`, `supplies.obj`, `balance.obj`, `gamedaemon.obj` (markets); spec: [economy-spec.md](economy-spec.md) (stockpiles, income, gathering, auto-gather, markets, tribute, rares, trade, pop cap, every RNG draw; tool `re/tools/gather-ref.mjs`) |
| Players, diplomacy, leaders | `players.obj`, `leaders.obj`, `heroes.obj` |
| AI | BigHugeScript scripts in `ai\`, plus `Leader` / `LeaderData` (`leaders.obj`), `Armies` / `Army` (`armies.obj`, `army.obj`); spec: [ai-spec.md](ai-spec.md) (setup draws, `plan_strategy` census, `production_ai` states, make list, sites, building placement, market AI, diplomacy, armies and `find_target`, per-unit AI, every RNG draw, port plan) |
| Conquer the World | `conquest*.obj` (about 30 modules) |
| Terrain features | `Terrain*.obj`, `Rivers`, `Roads`, `Cliffs`, `mountains`, `Surf`, `storm`, `Nuke` |
