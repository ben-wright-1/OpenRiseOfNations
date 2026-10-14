# Production: construction, training and research

Status: ✅ the production chain — placing a building, building it, activating it, queueing, training, researching,
the per-frame building step and every `game_random` draw on the way — is specified from the decompiles, with the
disassembly checked where the decompiler was unreliable (marked) · 🟡 branches from the decompile only (marked inline)
· ❓ open items in §13. Nothing is validated against a running game yet (no replay with production commands exists on
this Mac).

Deliverables: this document; `re/tools/production-ref.mjs` (cost escalation, train / research / construction times,
construction progress with N builders, reading the rules from `~/Games/RiseOfNations`: `node
re/tools/production-ref.mjs --examples`, `--selftest`, or `<type name> [--count N] [--tribe X] [--age A]`).
Decompiled C and disassembly stay local (gitignored): `re/decomp/prod_main/`, `prod_A*/`, `prod_B*/`, `prod_C*/`,
`prod_c2*/`, `prod_D*/` (jobs `re/jobs/dc_prod_*.txt`), plus the older `orders/`, `bld2/`, `cmds*/`, `rules*/`.

Read first: [frame-spec.md](frame-spec.md) (§6 object pass order, §8 lifecycle, §10 summary of `Build::process`),
[orders-spec.md](orders-spec.md) (§2.7–2.9 BUILD / QUEUE_UP / GATHER_POINT, §4.2 queue positions),
[world-spec.md](world-spec.md) §6 (snap, footprints, `mask_me`, `blocked_site` summary; ported in
`engine/sim/placement.cpp`), [rules-spec.md](rules-spec.md) §6, §8, §9, §12.6 (type fields and constants),
[combat-spec.md](combat-spec.md) (buildings that fire, damage, death). Resources, gathering, markets and population
accounting are in [economy-spec.md](economy-spec.md) (§2 stockpiles and buckets, §3.6 gather tiles, §7.1 pop cap); this
document references it for those and owns everything about buildings and queues.

## 0. Conventions

- Addresses are `riseofnations.exe` VAs, names are PDB names. Pseudocode is paraphrased, never the decompiler text.
- int32 arithmetic with C truncating division unless noted. "rnd(n)" = `game_random.get(0, 0xFFFF) % n` (one
  simulation stream, replay-spec §12). `internal_random` and `SoundGlobal::random` are presentation only.
- "every N frames, staggered" = `(frame + o) % N == 0` (frame-spec conventions). (P) marks presentation-only steps
  (messages, sounds, hints, Steam statistics, `options->rebuild`): a port may drop them, but must not let them feed
  back into the simulation.
- `who` = leader 0..9, `L` = `leaders.list[who]` (`LeaderData`, stride 0x6eec; names from
  `node re/tools/pdbtypes.mjs LeaderData`). Building fields are `BuildData` (+`WallData`) names, §8 lists offsets.
- TypeIndex names from `re/types/typeindex.tsv` (e.g. 50 PEASANTS = Citizen, 414 VILLAGE = city, 427 BARRACKS, 435
  LIBRARY, 526..542 wonders, 544..550 ages). Tribe bonus k = nation index k in rules.xml TRIBES order: 0 Aztecs,
  1 Maya, 2 Inca, 3 Bantu, 4 Nubians, 5 Greeks, 6 Romans, 7 Egyptians, 8 Turks, 9 Spanish, 10 French, 11 British,
  12 Germans, 13 Russians, 14 Chinese, 15 Japanese, 16 Koreans, 17 Mongols, 18 Iroquois, 19 Lakota, 20 Americans,
  21 Indians, 22 Dutch, 23 Persians (`LeaderData::has_tribe_bonus` @6e1370, see fog-spec §4 for its CTW / nomad
  exceptions).
- Virtual slots: Build object vtable @b42174, BuildType (ptype) vtable @b42b94; the slots used are listed in §8.
  Identical-COMDAT folding: `@41bff0` = `return 0`, `@41e0e0` = `return 1`, `@41c000` = `return this`.

## 1. The production chain in one frame

Where each production step runs relative to frame-spec §2–§7 (frame F → F+1). Everything listed mutates simulation
state unless marked (P).

```
CommandManager::process_turn (commands recorded for frame F, file order)          §9
  QUEUE_UP  → Group::action_queue_up → Build::queue_up: can_pay_cost, could_queue, pay_cost (buckets change NOW),
              append QueueItem {job_counter 0, type, captured cost}, num_queued[t]++                      §6.2
  BUILD     → Group::action_build: validate_build, city limit, type_avail, snap + blocked_site, pay_cost,
              Objects::init_build → Build::init (find_gather_tiles: RNG, unbuilt lists, find_city),
              action_swarm_around → workers get MOVE (+ cast) + BUILD_AT orders                          §3
  SWARM_AROUND (BUILD_AT / REPAIR), UNQUEUE, DISBAND, BUILDMASK, GATHER_POINT, cheats                      §9
Game::do_frame
  Leaders::process_all: Leader::gather (economy), then if flagged calc_wall_stats (unfinished sites:
              update_construct_time; update_hits, update_los) and calc_unit_stats                        §7.7
  GameDaemon::process_all: region latch 0x20 → 0x10; check_borders (territory, ≤ 256 wcoords per frame)     §5.10
  Objects::process_all
    unit pass (players from frame % 10, slot order):
      builders: Unit::do_build → Wall::do_construct (first builder: blocked_site → start or disband;
              progress amount/(helpers+1); completion → Build::activate: stats, city founding, free units
              via train (RNG), wonder race, gain_tech for wonder techs)                                   §4
      repairers: Unit::do_repair (pays resources per hp step)                                            §4.8
      carriers: aircraft queue (Unit::process step 3, init_unit: RNG)                                      §6.7
    building pass (players 0..7, slot order):
      Wall::process: seen bookkeeping, AI builder top-up, helpers latch, territory attrition               §8.1
      Build::process (ACTIVE only): ejection, launch, attack (combat), do_queue → finished →
              train / come_out (RNG) | gain_tech | set_type | cast; gather tiles (RNG); Kremlin, Senate,
              Market, Terracotta free units; city capture / assimilation / heal / alarm; roads             §8.2
  Objects::inc_time: Wall::inc_time → update_hits(0) for every unfinished building (hp grow with progress)  §4.5
```

Key consequences:
- **Payment happens at command time** (QUEUE_UP, BUILD), not when production starts; units are pop-checked only when
  they complete (`Build::finished`).
- A building completed by a builder in the unit pass gets its first `Build::process` (and its first `do_queue`) in the
  same frame's building pass. Territory follows next frame at the earliest (incremental, §5.10).
- Units created in the building pass act from the next frame; units created in the unit pass (free units of
  `activate`) are appended to the unit list and are processed in the same pass if their slot is beyond the cursor.
- Every `game_random` draw of this document happens inside one of the boxes above, in the order of §10.

## 2. Costs and times

Scope: what a unit, building, tech or spell costs (`TypeData::get_cost` @00664090 and its wrappers), how the price is
charged, captured in the queue and refunded, and how long training, research and construction take
(`ObjectData::train_time` @006508c0, `Wall::update_construct_time` @0063d560, `BuildData::construct_time` @0062d5c0,
`Wall::do_construct` @006434d0). Reference model: `re/tools/production-ref.mjs` (§2.6).

### 2.0 Units and conventions

- All arithmetic is int32 C (truncating division) unless marked *unsigned*. `pct(p, v)` = `((100 − p) · v) / 100`, the
  form of almost every discount; `/100` is compiled as `imul 0x51eb851f; sar 5; + sign` (= C truncation, ✅ asm).
  `>> 8` on a signed value is compiled as `(x + ((x >> 31) & 0xff)) >> 8` (= truncation toward 0, written `/256`).
- **Resource amounts are plain integers.** XML costs are stored raw (`"5t"` → 5, rules-spec §5) and multiplied by
  `unit_cost_factor` / `build_cost_factor` / `tech_cost_factor` / `spell_cost_factor` (Constants +0x354/+0x358/+0x35c/
  +0x360, all 10) inside `get_cost`. **The ramp terms (`support_cost`, `special_upgrade_cost`) are NOT multiplied by the
  factor** for units; for buildings they are multiplied by `build_support_factor` (+0x37c = 1).
- Resource index r: 0 food, 1 timber, 2 wealth, 3 knowledge, 4 metal, 5 oil (`costs[6]` order).
- **Time unit = 1/100 frame.** `TypeData::time` returns `job_time · 100`; queue items and construction advance by
  `accel_train` / `accel_research` / `accel_construct` (+0x228/+0x230/+0x22c, all 100) per frame (× `ai_speed` when
  > 1, a cheat). So at default constants `job_time` is in frames; `Game::do_frame` ticks once per 15 frames
  (frame-spec §3 step 22), i.e. 15 frames ≈ 1 s of game time at normal speed (the "1/15 s"
  of rules-spec §6.3).
- LeaderData fields are LeaderData offsets (`node re/tools/pdbtypes.mjs LeaderData`); `Leader` = LeaderData + 8.
  Counters: `num_units[t − 50]` (+0x5762, u16), `num_queued[t]` (+0x5a22, u16, any TypeIndex), `num_buildings[t − 414]`
  (+0x555e), `high_buildings[t − 414]` (+0x5660). The decompiler often prints them with a shifted base
  (`field_0x56fe + 2t` = `num_units[t−50]`, `field_0x5222 + 2t` = `num_buildings[t−414]`, `field_0x5324 + 2t` =
  `high_buildings[t−414]`, `high_buildings[t + 0x4f]` in `get_support_count` = `num_units[t−50]`).
  `LeaderData.tech` (+0x6c0c, bits from +0x6c18) has one bit per TypeIndex; for unit/building types the bit means
  "this type is researched/available" (clear = the item in a queue is the **research** of that unit/building upgrade).
  `rare`/`rare_conquest` (+0x6d98/+0x6dc0, bits from +0x6da4/+0x6dcc): bit `good − 6` (`LeaderData::has_rare`
  @006e0770 ORs both). Encrypted values via `data_encrypted` (+0x6eb8): `bucket[r] ^ 0x8221`, `ages ^ 0x62766`,
  `epochs ^ 0x69587`, `epoch[c] ^ 0x63187` (c = library category 0 military, 1 civic, 2 commerce, 3 science).
- `has_tribe_bonus(n)` @006e1370: false if `GameInfo.flags & 4`, or no starting town and `city_num == 0`, or
  `tribe < 0`; true if conquest racial power bit n; false if `leader_flags2 & 0x40`; else `tribes[tribe].bonus(+0x54)
  == n`, and the bonus id equals the tribe index (0 Aztecs … 23 Persians, rulesdump `tribes[i].tribe`). Below,
  tribes are named.
- `is(X)` = type vslot +0x60 (`ObjectTypeData::is`): own type or X in `is_list` (the upgrade line; e.g. `is(334 Fire
  Raft)` is the fire-ship → submarine line, `is(414)` = Small/Large/Major City **and Forbidden City 531**,
  `is(443)` = forts **and Red Fort 534**, `is(521)` = Lookout … SAM 525). Type predicates used:
  `is_city` (vslot 0x64, `BuildTypeData::is_city` @00472bc0 = `is(414)`), `is_fort` (0xfc, @00472ba0 = `is(443)`),
  `is_tower` (0x100, @00472a30 = `is(439)`), `is_caravan` (0x130, units: `unit_flags2 & 8`), `has_objmask(m)` (0xf0,
  units: `obj_masks & m`), `is_missile` (0x114). Type ranges: units 50..413, buildings 414..542, wonders 526..542,
  techs 544..628 (ages 544..550, library "epoch" techs 551..578, finals 579..582), spells 629..683.
- No `game_random` draw anywhere in this section (✅: none of the functions below call `GameAccess::rnd` /
  `game_random`).

### 2.1 `TypeData::get_cost(r, who, o, loc, refund, convert, type_override)` @00664090 — 🟡/✅

Virtual (TypeData vslot 0x78), never overridden. Returns the amount of resource r. Parameters (mangled name
`HHHHHHHW4TypeIndex`, checked against the call sites in §2.4):

| Param | Meaning |
|---|---|
| r | resource 0..5 |
| who | leader (must be ≥ 0 for almost all branches; `-1` is only tolerated by the RADARSPELL / Greek tests) |
| o | producing / casting object slot of `who` (building that trains, caster), or −1 |
| loc | only used by the spell branch (a city index, overridden by the caster's city) |
| refund | 1 when called from `unpay_cost` / `sum_cost(…,1)`: wonder ramp count forced to 0; spell quirk §2.1.5 |
| convert | 1 = add the converted price of currently unavailable resources (§2.1.7). Every caller passes 1 |
| type_override | ≠ −1: take the base `costs[r]` of that type instead of `this`'s (callers seen pass −1) |

Execution order (each step operates on the running value `v`; `rr` = r after the Egyptian swap):

**2.1.1 Prologue**
1. **RADARSPELL** (665) with `who ≥ 0`: `u = current_upgrade(AIRDEFENSE 523)`; if `u ≥ 0` return
   `max(0, types[u].get_cost(r, who, o, loc, refund, convert, −1) − 10)` (✅ asm).
2. **Owned spell** (629..683 and `has_spell(who, type)`): `v = use[r]` (SpellTypeData +0x1e0, the casting cost
   COST2; ✅ asm `[ESI + r·4 + 0x1e0]`), `loc' = loc`; if `o ≥ 0` and the caster object passes vslots 0xc and 0x4c
   (valid, active) and has `flags & 0x20` (CITY) and the spell is not MOVE_CAPITAL_HERE 647 / DISBAND 666:
   `loc' = caster.city`; if `loc' ≥ 0`: **`v = v · −(refund ≠ 0)`** (✅ asm `NEG; SBB; IMUL`: 0 when paying, `−v` when
   refunding — a city casting its own spell is free, and the "refund" then subtracts). AMBUSH 635 cast by a unit with
   `unit_masks & 0x2000`: return 0. DISBAND on an active building: `v *= (x_size + y_size) / 2` (signed /2).
   Then jump to the spell branch (§2.1.5).
3. Otherwise: **Egyptian scholars**: if `type ∈ {52, 53}`, `egyptian_scholars_food` (+0x648, 0 in stock data) ≠ 0,
   `who ≥ 0` and Egyptians: swap r 0 ↔ 2 (`rr`). **All later comparisons with "r" use `rr`.**
   `v = costs[rr]` (or `types[type_override].costs[rr]`).
4. Dispatch on the type: unit (§2.1.2), building (§2.1.3), spell (§2.1.5), tech (§2.1.4), anything else
   (`v *= tech_cost_factor`); then the common tail (§2.1.6) and conversion (§2.1.7).

**2.1.2 Units (50..413)** — 🟡 decompile, key arithmetic ✅ asm (0x664e60..0x6651c0)

Free cases first: if Beaver Pelts (rare 41): return 0 for units with `unit_flags2 & 0x10`, and for MERCHANT 61,
MERCHANTDUTCH 62, FURTRAPPER 400. If scenario/CTW (`semaphore[1] & 0x10` or `semaphore[2] & 2`) and
`ScenarioData::units_free`: return 0.

Let `bit` = `LeaderData.tech` bit of the type (unit available).
1. **Queued upgrade surcharge** (only if `bit`): for q = 50..401 with `num_queued[q] ≠ 0` (ascending, order irrelevant):
   if `graft(unittypes[q].from) == type`, or `type`'s `jump` chain (`j = graft(jump)`, then `graft(unittypes[j].jump)`
   …) reaches q: `v += max(0, unittypes[q].costs[rr] − costs[rr])` (raw, before the factor). `graft(x)` =
   `LeaderData::get_graft` @006e30f0: `x < 0` → x; `tribe_can_type(x)` → x; else `type_graft(who)` (units 50..401:
   `tribes[tribe].graft[x − 50]`, `UnitTypeData::type_graft` @0061d790). Meaning: while the upgrade to q is being
   researched, the unit is priced as the upgraded unit.
2. `v *= unit_cost_factor`; keep **`ramp_base = v`** (used for the ramp caps).
3. Nation / rare / wonder chain, in this order (each `v = pct(p, v)` unless noted):
   - `where == BARRACKS 427` and Salt (13): `v = v · 9 / 10` (hard-coded);
   - Mongols and `where ≥ 0` and `types[where].is(STABLE 428)`: `mongol_stable_cost` (+0x7e8, 10);
   - French: `where ∈ {430 Siege Factory, 431 Factory}`: `french_siege_cost` (+0x6a8, 15); else `is(GENERAL 54)`:
     `french_special_cost` (+0x6a0, 0);
   - Japanese: `where == 427`: `japanese_barracks_cost` (+0x798, −7): if negative,
     `v = ((min(ages, epoch[0]) · k + 100) · v) / 100` (7 % cheaper per age, capped by military techs), else `pct`;
     `where ∈ {432, 433, 434}`: `japanese_ship_cost` (+0x7a0, 10);
   - Germans and `is(334)` (submarine line): `german_submarine_cost` (+0x708, 25);
   - Americans: `where == AIRBASE 447`: `americans_aircraft` (+0x868, 20); then `this.is(351)` again; then `this.is(308)`
     again (can stack);
   - British and `is(281 AA gun)`: `british_aa_cost` (+0x6e0, 25);
   - Nubians and (`is(CARAVAN 59)` or type ∈ {61, 62, 400}): `nubian_merchants` (+0x5c8, 10);
   - Russians and `is(SPY 58)`: `russian_spy_cost` (+0x768, 50);
   - Romans and `is(HOPLITES 132)`: `roman_legion_cheap` (+0x630, 10);
   - `where ∈ {427, 428, 429}`: `terra_cotta_cost` (+0x460, 0) **without any wonder test**, then if Angkor Wat (535):
     `angkor_ships` (+0x4d8, 25) (so Angkor Wat discounts barracks/stable/auto-plant units as well as ships);
   - `where ∈ {428, 429}`: Horses (14) `horses_stable_cost` (+0x8f8, 15); Rubber (15) `rubber_autoplant_cost`
     (+0x8fc, 15);
   - `where ∈ {430, 431}` and Sulphur (16): `sulphur_siege_cost` (+0x900, 20);
   - (`is(58, strict)` or `is(54, strict)`) and `has_preq(SPIES_GENERALS_CHEAPER 771)`: `spy_general_cost` (+0x218, 50);
   - type ∈ {50, 51}: Wool (33) `wool_citizens_cost` (+0x948, 15); Turks `turk_citizen` (+0x678, 33);
   - `domain == 1` (sea) and Angkor Wat: `angkor_ships`; `domain == 2` (air): Aluminum (30) `aluminum_air_cost`
     (+0x934, 15), then Space Program (542) `space_air_cost` (+0x568, 50);
   - `where == 427`: Despotism: `has_preq(DESPOTISM_3 796)` → `despotism_military_cheaper3` (+0x99c), else
     DESPOTISM_2 795 → `…cheaper2` (+0x998), else DESPOTISM_1 794 → `…cheaper` (+0x994) (all 15);
   - Statue of Liberty (537): `is_missile` → `liberty_missiles` (+0x530, 0), else `is(BOMBER 304)` →
     `liberty_bombers` (+0x52c, 33);
   - Uranium (35) and `is(NUKE 315)`: `uranium_nuke_cost` (+0x954, 5);
   - `num_units[369 − 50] ≠ 0` (Archduke Charles alive): `v = (charles_unit_cost · v) / 100` (+0xbc4, 90).
4. **Upgrade research price** (if `!bit` and `!(unit_flags & 0x80)`, flag h "no research needed"):
   - Wine (8): `wine_unit_upgrades` (+0x8d8, 20);
   - `v = ((research_premium · v) / 256 · research_premium_cost) / 256` (+0x3a4 = 256; UnitTypeData +0x2e0, 8.8,
     512 for most units = ×2) (✅ asm);
   - if `special_upgrade == rr`: `v += special_upgrade_cost` (raw);
   - **refit** (type ∉ 66..68): for q = 50..401: if `q == graft(this.from)` or q's jump chain reaches `type`:
     `d = costs[rr] − q.costs[rr]`, halved (signed) if `q.costs[rr] == 0`; if `d > 0`: `d = min(d,
     unit_refit_max_cost)` (+0x390, 40); `n = num_queued[q] + num_units[q − 50]`; `v += unit_cost_factor · n · d`.
     (✅ asm. A dead countdown loop on a local that is always 0 precedes the add.) So upgrading costs more the more
     units of the old type exist, whenever the new type is dearer in that resource;
   - if `role & COMBAT (0x10000)`: `d = epoch[0] − military_level` (`get_military_level_slow` if < 0); if `d > 0`:
     `d *= military_upgrade_discount` (+0x388, 10); `span = ending_technology − starting_technology + 1`
     (GameInfo +0x36/+0x34 bytes); if `span < 8`: `d = (span · d + 7) >> 3`; `v = pct(d, v)`, clamp ≥ 0. Then
     Americans and `domain == 0`: `americans_upgrade_discount` (+0x884, 25); Dutch and `domain == 1`:
     `dutch_ship_discount` (+0x8c0, 10).
5. **Escalating price ("ramp")** (otherwise):
   - `n = get_support_count(type)` (§2.2.8; 0 for `type < 0`);
   - citizens 50/51: `n += support(66) + support(67) − scholar_militia (+0x9f0) + support(68)` (militia, minutemen and
     partisans made from citizens count as citizens); scholars 52/53: `n += scholar_militia`;
   - `this.is(NUKE 315)`: `n += nukes_used` (+0x7bc) — on top of the `nukes_used` already added by
     `get_support_count` for 315/316 (double count, as coded);
   - if `n > 0`: if `progression & 2`: `n = ((n + 1) · n) / 2` (triangular); `extra = 0`;
     - scholars 52/53: `cap = (unit_scholar_ramp_max · ramp_base) / 100` (+0x394, 2000); for k = 7, 14, … while
       `k < n`: `extra += n − k`;
     - else `max` = `unit_worker_ramp_max` (+0x398, 500) for citizens, scholars, merchants (`TypeData::is_merchant`
       @0042dbd0: 61, 62, 400) and caravans (`unit_flags2 & 8`); else `unit_other_civilian_ramp_max` (+0x39c, 200)
       if `obj_masks & CIVILIAN (4)`; else `unit_military_ramp_max` (+0x3a0, 125). `cap = (max · ramp_base) / 100`;
     - for slot i = 0, 1 (in order) with `support[i] == rr`: `add = support_cost[i] · n + extra`; if `cap ≠ 0` and
       `cap < add`: `add = cap`; Maize (48): `add = pct(maize_ramping_bonus (+0x98c, 50), add)`; `v += add`.
   - if `role & COMBAT`: `lvl = max(1, military_level)`; `d = epoch[0] − lvl`; if `d > 0`: `d *=
     military_unit_discount` (+0x38c, 5); if `span < 8`: `d = max(1, (span · d + 7) >> 3)`; `v = pct(d, v)`, clamp ≥ 0;
   - (`has_preq(MONARCHY_1 800)` or `MONARCHY_2 801`) and `where == 428`: `monarchy_unit_prod` (+0x9ac, 25);
   - `has_preq(SOCIALISM_1 804)` and `where ∈ {430, 447, 432}`: `socialism_unit_bonus` (+0x9bc, 25);
   - Salmon (46) and `domain == 1`: `salmon_naval_cost` (+0x984, 20).

**2.1.3 Buildings (414..542)** — 🟡 decompile, ramp counting ✅ asm (0x6657ee..0x665b60)

1. Free: scenario/CTW with `ScenarioData::buildings_free` → 0; Iroquois and `is(SENATE 438)` and
   `iroquois_quick_senate` (+0x834, 1) and `high_buildings[438 − 414] == 0` (never had a senate) → 0.
2. `v *= build_cost_factor`. If the building's tech bit is clear and `build_flags & 4`: `v = (research_premium · v)/256`.
3. **Ramp count n**:
   - wonders (526..542; `is_wonder`, vslot 0x1c): `w = get_wonders() + get_unbuilt_wonders()` (own completed wonders
     in the wonder list + own placed-but-unfinished wonder sites). If `refund ≠ 0`: `n = 0`. Else
     `n = teams_locked ? get_team_wonders() − w + get_team_unbuilt_wonders() : 0` (`Game::teams_locked` @00594880:
     `team_style ∉ {0, 8, 11}`; team counts include self and mutual allies); `if w > 3: n += w − 3`; `if w > 6: n += w − 6`;
     Supercollider/Space Program: own unbuilt SC or SP → `n += 2(w + 1)`; else team unbuilt SC/SP → `n += 2w + 1`;
     every other case (and every other wonder) `n += 2w`;
   - cities (`is_city`, not a wonder): `n = Σ_{t=414..416} (num_buildings[t−414] + num_queued[t])` (all city sizes);
   - other buildings: `n = num_queued[t] + num_buildings[t − 414]` (an unfinished site is in `num_queued`,
     `Wall::increment_stats` @00643270; it moves to `num_buildings` on activation); Oil Platform 422 adds Oil Well 421's
     two counters and vice versa. Military production {427, 428, 429, 430, 431, 447, 520}, with `c` = that count
     (unsigned compares): `c > 2: n = 2c − 2`; `c > 5: n += 2c − 10`; `c > 8: n += 3c − 24`; `c > 12: n += 4c − 48`.
     Forts {443..446}, `c` = count: `c > 1: n = 2c − 1`; `c > 3: n += c − 3`; `c > 5: n += c − 5`; `c > 7: n += c − 7`.
4. For slot i = 0, 1 with `support[i] == rr`: `add = support_cost[i] · build_support_factor · n`; wonders: `add /= 2`;
   Indians (21) and not `is_city`: wonders → `add = (add + 1) / 2`; types 439..446 and 521..525 → `add = (add + 1)/2`;
   **any other building → `add = 0`** (Indian buildings do not escalate); `v += add`.
   (Forbidden City is `is_city`, so an Indian Forbidden City keeps the plain wonder ramp.)
5. Discounts in order: cities (`is_city` and not wonder): Bantu `bantu_city_cost` (+0x5b8, 75, only if ≠ 0), then
   Pyramids (526) `pyramids_city_discount` (+0x434, 33). Wonders: Marble (20) `marble_wonder_cost` (+0x910, 10),
   Egyptians `egyptian_wonders` (+0x63c, 25). `is(TOWER 439)` and Koreans: `korean_tower_cost` (+0x7e4, 33).
   (`is_fort` or `is(439)`) and not wonder: Colosseum (530) `colosseum_fort_cost` (+0x480, 20); Romans
   `roman_fort_cost` (+0x60c, 25). `is(TEMPLE 437)` and Tikal (532): `tikal_temple_cost` (+0x4a4, 0).
   Market 436 and Nubians: `nubian_merchants`; else Library 435 / University 420 and Greeks: `greek_library_cost`
   (+0x5f0, 66). Maya and `rr == 1` and type ∉ {427..431, 447, 432..434, 443..446, 520, 439..442, 521..525} and not a
   wonder: `maya_building_timber` (+0x594, 25). British and `is(523)`: `british_aa_cost`. Mine 419 and Germans:
   `german_mine_cost` (+0x724, 0); else Farm 417 and Japanese: `japanese_farms_cheaper` (+0x7a8, 50); else type ∈
   427..431 and Colosseum: `colosseum_barracks_cost` (+0x484, 0). `is(521)` and Statue of Liberty:
   `liberty_air_defense` (+0x538, 33).

**2.1.4 Techs (544..628)** — 🟡 decompile

1. Scenario with `ScenarioData::techs_free` → 0. CTW (`semaphore[2] & 2`) and age tech and `v ≠ 0`:
   `v = ctw_age_cost` (+0xa6c, 50).
2. `v *= tech_cost_factor`. Governments (623..628, `is_gov_type`) and Americans → `v = 0`. Incense (49) and
   `where == TEMPLE 437`: `incense_temple_research` (+0x990, 25).
3. Greek conversions (constants 0 in stock data): `type == 544` and `greek_classical` (+0x5ec) ≠ 0 and Greeks:
   r 0 → `pct(greek_classical)`, r 3 → `v += (tech_cost_factor · costs[0] · greek_classical) / 100`; otherwise for
   age techs the same with `greek_age_knowledge` (+0x5e8).
4. `v = calc_science_discount(type, v)` (§2.2.6): ±10 % per science level relative to the tech's age.
5. Age techs only — **age-behind discount**: first pass over leaders 0..7: another existing (`flags & 1`) leader that
   is not allied (`other.is_ally(who) == 0`) with `ages_mine + 1 < ages_other` → `d = rr == 3 ?
   tech_age_behind_knowledge_discount (+0x370, 20) : tech_age_behind_discount (+0x36c, 10)`; `v = 2d < 100 ?
   ((50 − d) · v · 2) / 100 : 0`; done. Else second pass: any such leader with `ages_mine < ages_other` → `v = d < 100 ?
   pct(d, v) : 0`. (Result independent of leader order.)
6. Final techs 579..582: `k = 8 ·` (number of finals owned or being researched, `has_tech || researching(f, −1, 0, 0)`);
   `v = (((ramp_final (+0x3ac, 50) · k) / 8 + 100) · v) / 100`.
7. Game setting `tech_cost` (GameInfo byte): `< 2`: `v = 2v/3`; `5, 6`: non-age and `rr ≠ 3` → `5v/4`, else `3v/2`;
   `≥ 7`: non-age and `rr ≠ 3` → `3v/2`, else `2v`. (2..4 unchanged.)
8. Age and library techs: Greeks and `rr ≠ 3`: `greek_research_cost` (+0x5d8, 10); CTW great-thinker cards
   (`num_bonus_cards[7]`): repeatedly `v = (v · 100) / (ctw_great_thinker_discount (+0xadc) + 100)`.
   Other techs: `where == GRANARY 423` and Bison (23): `bison_granary_cost` (+0x91c, 33); `where ≠ LIBRARY 435`:
   DEMOCRACY_2 803 → `democracy_tech_bonus2` (+0x9b8, 20) else DEMOCRACY_1 802 → `democracy_tech_bonus` (+0x9b4, 20).
9. Enhancer techs 609..614 and 620..622: Hanging Gardens (528) and `hanging_gardens_industry_discount` (+0x454, 66) ≠ 0;
   then Germans `german_industry_discount` (+0x738, 50). Temple techs 589..592 and British:
   `british_taxation_discount` (+0x700, 50).
10. `rr ≠ 3` and Versailles (536): applies to age, library and final techs, and to university techs 615..619 only if
    `versailles_affects_university_upgrades` (+0x4f0, 0), other techs only if `versailles_affects_misc_upgrades`
    (+0x4f4, 0): `versailles_knowledge_costs` (+0x4dc, 50).
11. Age techs and Silver (18): `silver_age_cost` (+0x908, 15).
12. Library techs by `cat` (techtypes +0x14): 0 military: Furs (21) `furs_military` (+0x8e8, 25), Despotism 3/2/1, Turks
    `turk_military_cheap` (+0x674, 33); 1 civic: Dye (10) `dye_civic_commerce` (+0x8e0, 25), Persians
    `persians_civic_discount` (+0x8cc, 30); 2 commerce: Silk (11) `silk_commerce` (+0x8e4, 25), Dutch
    `dutch_commerce_discount` (+0x8a0, 10); 3 science: Papyrus (9) `papyrus_science_military` (+0x8dc, 25), Chinese
    `chinese_science_discount` (+0x790, 20), Americans `americans_science_discount` (+0x880, 0).

**2.1.5 Spells (629..683), declarations of war** — 🟡

`v *= spell_cost_factor` unless DISBAND. If `who ≥ 0`:
- **Declare war** (668..675, target = type − 668): `v *= cost_factor_dow(target, type)` (§2.2.7); if not
  `is_ally(who, target)`: `v −= 10 · invaders[target]` (LeaderData +0x290, int[8]), clamp ≥ 0.
- **Cancel alliance** (676..683): `v *= cost_factor_dow(type − 676, type)`.
Then the common tail. (Casting itself, `SpellType::pay_cast_costs` / `unpay_cast_costs`, `Leader::pay_dow`,
`LeaderData::afford_dow`, is the spells/diplomacy spec's subject.)

**2.1.6 Common tail** (every type, @00666e3a) — 🟡
1. One resource-specific rare: `rr == 1` Coal (34) `coal_timber_cost` (+0x950, 25); `rr == 0` Sugar (27)
   `sugar_food_cost` (+0x928, 10); `rr == 2` Gold (42) `gold_wealth_cost` (+0x974, 10); `rr == 4` Iron Ore (47)
   `iron_metal_cost` (+0x988, 10).
2. Gypsum (40): `gypsum_cost` (+0x96c, 10), every resource and type.
3. `o ≥ 0`, the object is a building (vslot 0x20) and `BuildData::is_unassimilated` @0062d470 (in a city whose owner
   (`city+0x5f`) differs from the building's owner, and the building has `flags & CITY` or not `build_flags & 0x10`),
   and the type is a unit: `v *= 2`.
4. Someone has the Supercollider (`game.wonders[15] ≠ 0`) and the type is a unit with `is_missile`: for every other
   leader i (0..7) with `leader_flags & 3 == 3`, not allied, owning the Supercollider:
   `v = ((super_missiles (+0x554, 0) + 100) · v) / 100`.
5. Indians and unit `is(261..264)` (elephant lines): `indians_elephant_cost` (+0x894, 15).

**2.1.7 Unavailable-resource conversion** (`convert ≠ 0`, @00667186) — 🟡
For g = 0..5 with `type_avail(who, g, 1) == 0` (resource not usable): if `!has_preq(g)` (undiscovered) and
`rr == goods[g].undiscovered_cost_good` (+0x2b4): `v += (get_cost(g, who, o, loc', refund, 1, −1) ·
undiscovered_cost_rate (+0x2c8)) / 256`; else if `has_preq(g)` (obsolete) and `rr == obsolete_cost_good` (+0x2b8):
the same with `obsolete_cost_rate` (+0x2cc). Stock data: knowledge → food ×384/256, metal → timber ×320/256, oil →
metal ×384/256 (and, recursively, onward to timber while metal is also unavailable); obsolete: timber → oil ×128/256,
others → wealth ×256/256. The recursion applies all discounts of resource g first. No cycles in stock data.
Example: Ancient-age Hoplites (5f/3m): 50 food + `30·320/256` = 37 timber.

### 2.2 Paying, capturing and refunding

**2.2.1 `TypeData::can_pay_cost(who, loc, o, total)` @00667570** (vslot 0x84) — ✅
- `semaphore[1] & 8` (editor): return 10. Scenario/CTW with `units_free` / `buildings_free` / `techs_free` for that
  kind: return 10.
- `best = −1`; for g = 0..5 with `type_avail(g, 1)`: `c = get_cost(g, who, o, loc, 0, 1, −1)`; if `c ≠ 0`:
  `have = bucket[g] − (total ? 0 : escrow[g])` (escrow = LeaderData +0x468 int[6]); `q = have / c` (signed); `q == 0`
  → return 0; `best = max(best, q)`. Return `best` (10 if no resource is charged).
- Notes: it returns the **largest** quotient, not the smallest (only truthiness matters to the simulation callers).
  A negative `have` with `|have| ≥ c` gives a negative non-zero quotient and **passes** (escrow larger than the
  bucket). QUEUE_UP and BUILD pass `total = 1` (escrow is spendable).

**2.2.2 `Type::pay_cost(who, o, loc, from_escrow, out[6])` @006681f0** (vslot 0xd4) — ✅
For g = 0, 1, …, 5 **in this order**: `out[g] = 0`; if `type_avail(g, 1)`: `c = get_cost(g, who, o, loc, 0, 1, −1)`;
`out[g] = c`; if `!from_escrow` and `bucket[g] − escrow[g] < c`: `escrow[g] = 0`;
`bucket[g] = max(0, bucket[g] − c)` (written back XOR 0x8221); if `from_escrow`: `escrow[g] = max(0, escrow[g] − c)`.
Writes per resource: `bucket[g]` then (if from_escrow) `escrow[g]` — 6 × up to 2 writes, resource order. Unavailable
resources are never charged (their price is folded into available ones by §2.1.7). There is no affordability test
here (callers test `can_pay_cost` first); a short bucket silently clamps to 0.
Callers: `Build::queue_up(type, from_escrow)` (QUEUE_UP passes 1), `Group::action_build` (`pay_cost(who, −1, −1, 1,
NULL)`), `Group::action_spell`, `Leader::produce_building / produce_city` (AI), `SpellType::pay_cast_costs`.

**2.2.3 `Type::unpay_cost(who, o, loc)` @006682f0** (vslot 0xd8) — ✅ unless editor (`semaphore[1] & 8`): for g = 0..5
with `type_avail`: `bucket[g] += get_cost(g, who, o, loc, 1, 1, −1)` (no clamp, no escrow; **recomputed**, so it
refunds the current price with the wonder ramp zeroed). Used by unit build orders (`Unit::check_build_order`,
`Unit::action_unqueue`: cancelling a building that was never placed), `SpellType::unpay_cast_costs`, scenario loaders.

**2.2.4 QueueItem capture and refund** — ✅
`QueueItem` (0x14 bytes): `job_counter` (int), `type` (short), `good[3]` (short), `cost[3]` (short).
`Build::queue_up` @620f40: `can_pay_cost(who, −1, o, 1)` (else `queue_fail = QUEUE_COST`) → `could_queue` →
**`pay_cost(who, o, −1, from_escrow, local[6])`** (skipped in the editor, `local` zeroed) → `queued++` →
`BuildQueue::set_queue(index, type, local, 0)` @6309f0 → `num_queued[type]++` (+ the per-class `*_queued` counters).
So the price is computed **before** the item counts itself. `set_queue` stores the first three non-zero entries of
`local` in resource order as `(good[k], cost[k])`, **truncated to 16 bits**, and fills the rest with `(−1, 0)`. A
fourth non-zero resource (no stock item has one) would be paid but never refunded.
`Build::unpay_cost(i)` @6206e0 (from `Build::unqueue(i, cancel=1)` and `Build::clean_queue` on building death, not in
the editor): for k = 0, 1, 2 with `good[k] ≥ 0`: `bucket[good[k]] += cost[k]` — the **captured** price, not a
recomputation, and no escrow change.
`Build::refund_cost(i)` @620490 (only from `Leader::gain_tech` when a **science** library tech (cat 3, frame ≠ 0) is
gained, for every other tech queued in the first library, **before** `epoch[3]` is incremented): with
`s = epoch[3]`, `a = techtypes[item.type].age`, `d = tech_science_discount` (+0x364, 10), for k = 0, 1, 2 with
`good[k] ≥ 0`: `full = (cost[k] · 100) / (100 − d · (s − a))`; `new = full − ((s − a + 1) · d · full) / 100`;
`bucket[good[k]] += cost[k] − new`; `cost[k] = new` (short). It re-prices queued techs at the new science level and
refunds the difference (a negative difference charges).

**2.2.5 `Type::sum_cost(who, o, refund)` @00668080** — Σ over available g of `get_cost(g, who, o, −1, refund, 1, −1)`
(AI and UI). **`Type::sum_rules_cost()` @006680e0**: `Σ costs[0..5]` × the class factor (combat-spec.md kill value, ~line 402).
**`TypeData::get_limiting_resource(who, o, loc)` @00667730**: the single available resource whose `get_cost > bucket`
(escrow ignored), −1 if none or more than one (UI feedback).

**2.2.6 `LeaderData::calc_science_discount(type, v)` @006da630** — ✅ non-tech types: v. Techs:
`a = techtypes[type].age` (+0x1c8), `+1` unless age or library tech; return `v − ((epoch[3] − a) ·
tech_science_discount · v) / 100`. Negative `epoch[3] − a` **raises** the price (10 % per missing science level).

**2.2.7 `LeaderData::cost_factor_dow(target, type)` @006da590** — ✅ `m = (target == who) || (diplos[target] == 2 &&
target.diplos[who] == 2)` (mutual alliance); `k = m + (type is DECLARE_WAR ? 1 : 0)`; return `k · (ages + 1)`.
So declaring war costs `ages+1` times the base (×2 on an ally); cancelling an alliance costs `ages+1` times (0 when
not allied).

**2.2.8 `LeaderData::get_support_count(t)` @006da110** — ✅ non-units: 0. If `progression & 1` and `attack ≠ 0`:
`where == 427` → `barracks_queued + barracks_units` (+0xa10/+0x9f8); 428 → stable (+0xa14/+0x9fc); 430 → factory
(+0xa18/+0xa00); 432 → dock (+0xa20/+0xa08); else `domain == 2` → air (+0xa24/+0xa0c). Otherwise `n = num_queued[t] +
num_units[t − 50]`; t ∈ {315, 316}: `+ nukes_used`; t ∈ {313, 314}: `+ missiles_used` (+0x7c0); Americans and
`is(304 bomber line)`: `n = max(0, n − americans_free_bomber (+0x864) · (num_buildings[447 − 414] + airbase-type
buildings from reg_buildings))` (🟡 operand of `get_buildings`). The class counters are maintained by
`Build::queue_up` / `unqueue` / `Wall::increment_stats` / `Leader::track_unit_type` for every unit with `attack ≠ 0` by
`where` (Barracks units also bump `combat_queued`).
Stock data: 232 units have `progression 3` (per-class count, triangular), 4 have 2, the rest 0.

**2.2.9 `support_cost` / `control_cost`** — `UnitData::support_cost` @0060fde0 returns 0;
`ObjectData/WallData::support_cost(i)` @006452f0/@0063fa30 return `type.support_cost[i] << 8` (upkeep, economy-spec);
`UnitData::control_cost(all)` @00609d40: 0 for a unit with `unit_masks & 1` unless `all`, else `type.control_cost`
(POP). None of them enters `get_cost`.

### 2.3 Times

**2.3.1 Base times** — ✅
- `TypeData::time(who)` @00663f20 (vslot 0x6c): spells not owned (`!has_spell`) → `res_time · 100`; everything else
  `job_time · 100` (unsigned).
- `TypeData::research_time(who)` @006639a0 (vslot 0x70): spells → `res_time · 100`; else `(time(who) ·
  research_tick_premium (+0x3a8, 256)) >> 8` (unsigned).
- `UnitTypeData::research_time` @0061d760: `(TypeData::research_time · research_premium_time (+0x2e4)) / 256`
  (signed; 512 for most units = ×2).
- `SpellTypeData::get_job_time` @00675800: casting duration (spells spec).

**2.3.2 `ObjectData::train_time(type)` @006508c0** — the time of a queue item (or a carried unit), **re-evaluated
every frame** by `Build::do_queue` @61e410 / `Unit::process` (so it follows tech/unit-count changes while the item
runs). `this` = producer, `who` = its owner. 🟡 decompile, Supercollider/science parts ✅ asm.
1. Base: unit type whose tech bit is clear (upgrade research) → `research_time(who)` (vslot 0x70); else `time(who)`.
2. DISBAND 666 / DEPOPULATE 646 (not RAZE, which is 667; corrected from the port): `v = train_time(producer's own type)`; producer `is(414)` → `· disband_city_rate (+0x380,
   400) / 100`; `is(438)` → `· disband_senate_rate (+0x384, 1600) / 100`; Lakota → `· lakota_raze_speed / 100`; `v /= 2`.
3. `is(315)` and Uranium: `v = (v · 100) / (uranium_nuke_speed (+0x958, 10) + 100)`.
4. **Units with the tech bit set** (training):
   - `v = (unit_rate_base (+0x220, 120) · v) / 100`; `cap = 3v`;
     `v += num_units[t − 50] · job_extra_time (+0x2e8, percent) · unit_rate_progression (+0x224, 75)`;
     `v = (v < 0 || cap < 0) ? 0 : min(v, cap)`. (Each owned unit of that exact type adds 7.5 frames for a "1/10"
     unit; capped at 3× the base.)
   - handicap mode (`semaphore[0] & 4`): `v = ((200 − get_handicap()) · v) / 200`;
   - The President nearby (`has_general(0, 355) ≥ 0`): `v = (v · 100) / (thepresident_unit_build_speed (+0xc84) + 100)`;
   - Mongols, `attack ≠ 0`, `types[where].is(428)`: `/(mongol_stable_speed (+0x7ec, 20) + 100)` (all "/(k+100)"
     steps are `v = (v · 100) / (k + 100)`);
   - Japanese: `where == 427`: `k = japanese_barracks_speed (+0x79c, −10)`, negative → `k = −k · min(ages, epoch[0])`;
     `/(k+100)`; `is(351)`: `/(japanese_carrier_speed + 100)`;
   - Chinese and (citizen 50/51 or merchant or caravan): `/(chinese_citizen_creation (+0x780, 100) + 100)`; if
     `chinese_citizen_instant` (+0x77c, 1): **return 1**;
   - British: `domain == 1` → `british_ship_speed` (+0x6d4, 33); `is(170 Bowmen)` → `british_archer_speed` (0);
     `is(281)` → `british_aa_speed` (+0x6e4, 33);
   - French: `where ∈ {430, 431}` → `french_siege_speed` (+0x6ac, 25); else `is(54)` → `french_special_speed` (0);
   - Germans: `domain == 2` → `german_air_speed` (+0x710, 33); `is(334)` → `german_submarine_speed` (+0x70c, 33);
   - Romans and `is(132)`: `roman_legion_fast` (+0x634, 10);
   - `role & COMBAT` and `has_preq(TROOPS_FASTER 789)`: `v = 3v / 4`;
   - speed upgrades: `domain == 1` → `n = get_ships_speed_upgrade()` (count of SHIPS_FASTER_1..3 748..750 owned);
     else `obj_masks & (FOOT 0x20 | MOUNTED 0x1000)` → `get_troops_speed_upgrade()` (742..744); else `obj_masks &
     VEHICLE 0x200000` → `get_vehicle_speed_upgrade()` (760..762); `v = ((10 − n) · v) / 10`;
   - `where ∈ {427, 428, 432}` and Cotton (22): `cotton_creation_speed` (+0x918, 25);
   - (`is(54, strict)` or `is(58, strict)`) and `has_preq(SPIES_GENERALS_CREATED_FASTER 770)`: `v /= 2`;
   - citizens and Wool: `wool_citizens_speed` (+0x94c, 33);
   - `where == 428`: MONARCHY_2 → `monarchy_unit_prod2` (+0x9b0), else MONARCHY_1 → `monarchy_unit_prod` (+0x9ac);
   - `where ∈ {430, 447, 432}` and SOCIALISM_1: `socialism_unit_bonus`;
   - land + Statue of Liberty → `liberty_units` (+0x520, 0); sea + Porcelain Tower (533) → `porcelain_ships`
     (+0x4ac, 50); air + Space Program → `space_air_speed` (+0x564, 100);
   - (`is(58, strict)` and Kremlin 538) or `has_preq(INSTANT_UNIT_BONUS 696)`: **return `accel_train · 5 − 1`** (499).
5. **Techs** (544..628): age techs: `w = Σ` over leaders 0..7 that exist and have `ages_other + ages_queued_other >
   ages_mine + ages_queued_mine + 1` of (1 if `is_team(other, who)` else 2); library techs: the same with
   `ages + epochs` and `+3` instead of `+1`; if `w ≠ 0`: `v = ((2·num_nations − w + 2) · v) / (2·num_nations + 2)`; then
   Greeks: `/(greek_research_speed (+0x5d4, 100) + 100)`. Other techs: `k = techtype.count_discovered()` (vslot 0x74,
   number of players who already have it); if `k ≠ 0`: `v = ((num_players − k + 1) · v) / (num_players + 1)`.
   Then **Supercollider (541) owned: `v = 0`** (✅ asm 0x6513f7; → time 1, research is instant).
6. **Research modifiers** — techs, and units/buildings whose tech bit is clear (upgrade research):
   `has_preq(RESEARCH_FASTER 788)` → `3v/4`; Angkor Wat → `/(angkor_research (+0x4cc, 0) + 100)`; Relics (7) →
   `pct(relics_research_speed (+0x8d4, 33))`; CTW thinker cards → `/(ctw_great_thinker_time (+0xae0) + 100)` each;
   handicap; game `tech_cost` setting **0 or 2 → `2v/3`, 4, 6 or 8 → `3v/2`** (note: a different grouping than the
   price in §2.1.4 step 7); science speed-up: `a` = tech age, or for units/buildings `techtypes[preq[0]].age` (0 if
   none); if `a < epoch[3]`: `v −= ((epoch[3] − a) · tech_science_speedup (+0x368, 10) · v) / 100`; Supercollider and
   the type is a tech (not unit/building, not 666/667): `v = 0`.
7. Producer is a building that is unassimilated (same test as §2.1.6.3): `v = 5v / 4`.
8. Return `max(1, v)`.

Queue progress (frame-spec §10, `Build::do_queue`): the head item's `job_counter` advances by `accel_train` (units
with the tech bit), `accel_research` (research) or `accel_construct` (building items, DISBAND), × `ai_speed` if > 1,
clamped to `train_time`; the done test `train_time ≤ job_counter` is made before adding (an item with time 1 is done
at once). So a time T takes `ceil(T / 100) + 1` frames from its first advance (the tool's `trainFrames`).

**2.3.3 Construction time** — ✅ / 🟡
- **`Wall::update_construct_time()` @0063d560** → stored in `WallData.constr_time` (+0x50). Called by `Wall::init`
  (site placement) and by `Leader::calc_wall_stats` @006cf7c0 for every alive, **not yet active** building of the
  leader whenever `LEADER_CALC_WALL_STATS` (leader_flags 0x8000000) is set (Leader::process step after `gather`; set
  by tech gains, age changes …). All divisions unsigned:
  `v = time(who)`; Maya and not a wonder: `(v · 100) / (maya_building_speed (+0x590, 20) + 100)`;
  `has_preq(BUILDINGS_CREATED_FASTER 787)`: `(3v) >> 2`; Versailles: `/(versailles_building_speed (+0x4e4, 0) + 100)`;
  Tobacco (19): `/(tobacco_building_speed (+0x90c, 10) + 100)`; British and `is(523)`: `/(british_aa_speed + 100)`;
  Dutch and `is_fort` and not wonder: `/(dutch_fort_speed (+0x8b4, 0) + 100)`; Romans and `is_fort` and not wonder:
  `/(roman_fort_speed (+0x610, 50) + 100)`; `city_num (+0x3f8) == 0` (no city yet, nomad start):
  `(capital_build_time (+0x3bc, 300) · v) / 100`; finally `v = ((10 − get_building_speed_upgrade()) · v) / 10`
  (count of BUILDINGS_FASTER_1..3 754..756 owned, `LeaderData::get_building_speed_upgrade` @006dae90).
- **`BuildData::construct_time(all)` @0062d5c0** (Build vslot 0x18c; `WallData::construct_time` @00642d90 just returns
  `constr_time`) — evaluated at **every** `do_construct` call: `v = constr_time`; wonders, Americans, `get_wonders() ==
  0`, `wonders_built (+0x854) == 0`, type ∉ {541, 542}: if no other leader (0..7, existing, ≠ who) has an alive
  **unfinished** building of the same type, `v = 1` (the first American wonder completes on its first work). If
  `all == 0`: Hanging Gardens in the same city and not a wonder: `pct(hanging_gardens_build_time (+0x44c, 0))`; The
  President nearby (`has_general(0, 355)`): `(v · 100) / (thepresident_building_speed (+0xc90, 33) + 100)`; Iroquois
  first Senate (as §2.1.3.1): `v = 0`. Return `max(1, v)`.

**2.3.4 Construction progress: `Unit::do_build` @005eebf0 → `Wall::do_construct(amount)` @006434d0** — ✅ asm
- Each builder, in the unit pass (frame-spec §6 order), while adjacent and in the BUILD order, calls
  `do_construct(amount)` with `amount = accel_construct`, **`/4` (signed) if the site is under attack**
  (`build_masks & 0x20`, vslot 0x188) unless Koreans with `korean_build_under_fire` (+0x7dc, 1). Unit `unit_masks & 1`
  skips the call.
- `do_construct`: `amount *= ai_speed` if > 1. Unstarted site: `blocked_site(…)` → 0, 0x27..0x29, 0x2b, or 0x2a with
  few enough wonders in the city (Egyptians 2) → `start(1)` (vslot 0x1a4) and continue in the same call; any other
  result → `Object::disband(1)` (buildings: no RNG, `close`) plus a UI message.
- Started and not ACTIVE (`flags & 4 == 0`): `inc = amount / (helpers + 1)` (signed, `helpers` = WallData +0x64 u8 =
  builders already counted on this site this frame); if `build_masks & 0x800` is clear: `recharging (+0x7a)++` on the
  site (🟡 meaning: frames with work) and set 0x800; `helpers++`; `inc = max(inc, 1)`; `job_counter_2 (+0x4c) += inc`;
  `job_counter (+0x48) += inc`; if `construct_time(0) ≤ job_counter`: **`Build::activate(0, 1, 1)`** (vslot 0x1a8),
  UI message, return 1 (the builder's order ends). Else a UI-only 84 % threshold sets `scene->recalc_builds`.
- `Wall::process` (building pass, after all units) moves `helpers > 0` into `build_masks & 0x400`, zeroes `helpers`,
  clears 0x800.
- So N builders on one site add `Σ_{k=0}^{N−1} max(1, amount/(k+1))` per frame: 100, 150, 183, 208, 228, 244, 258,
  270 … (diminishing returns). Completion is in the frame where the sum reaches `construct_time`; which builder
  triggers `activate` (and gets the follow-up order logic of `do_build`) depends on unit slot order; builders after it
  in the same frame see the site ACTIVE and leave via `build_done`. `job_counter` is never rescaled when
  `constr_time` changes.

### 2.4 Who calls what

| Caller | Call | Sim? |
|---|---|---|
| `Build::queue_up` @620f40 (QUEUE_UP) | `can_pay_cost(who, −1, o, 1)`, `pay_cost(who, o, −1, from_escrow=1, out)`; captured into the QueueItem | sim |
| `Group::action_build` @707510 (BUILD) | (skipped if `semaphore[1] & 8`, or scenario/CTW `buildings_free`) `can_pay_cost(who, −1, −1, 1)`, `pay_cost(who, −1, −1, 1, NULL)`, then `Objects::init_build` (no refund if that fails) | sim |
| `Build::unqueue(i, 1)` @6207c0, `Build::clean_queue` @620b60 | `Build::unpay_cost(i)` (captured amounts) | sim |
| `Leader::gain_tech` @6dcb60 | `Build::refund_cost(i)` for queued library techs | sim |
| `Unit::check_build_order` @603470, `Unit::action_unqueue` @5e1f20 | `Type::unpay_cost` (recomputed, refund=1) | sim |
| `Group::action_spell`, `SpellType::pay_cast_costs / unpay_cast_costs`, `Leader::pay_dow`, `LeaderData::afford_dow`, `Options::do_declare_war` | spell / DOW prices | sim (spells spec) |
| `Build::do_queue` @61e410, `Unit::process` @610bc0 | `ObjectData::train_time` every frame | sim |
| `Wall::init` @63e9b0, `Leader::calc_wall_stats` @6cf7c0 | `Wall::update_construct_time` | sim |
| `Wall::do_construct` @6434d0, `Wall::process`, `Wall::update_hits` | `construct_time(0)` (vslot 0x18c) | sim |
| AI: `Leader::can_pay`, `check_income`, `research_techs`, `make_stuff`, `produce_unit/_building/_city`, `create_buildings/_units`, `upgrade_units`, `found_cities` | get_cost / can_pay_cost / sum_cost estimates | sim (AI, not specified here) |
| `IFaceSelected::*`, `IFaceLibrary`, `LeaderOut::warn_resources`, `BuildTypeOut::draw_template`, `ScenarioFuncSet::*`, `Console::hurry_action` | display / scripts | UI / script |

### 2.5 Side effects

Prices only write `bucket[r]` and `escrow[r]` (pay_cost), `bucket[r]` (unpay/refunds) and the QueueItem `cost[]`
(refund_cost) — all part of the checksummed leader state (state-spec). Time evaluation has no side effects; the
construction path writes `job_counter`, `job_counter_2`, `helpers`, `build_masks` 0x800/0x400 and `recharging`, and
`activate` (frame-spec §8.1: city/territory/pop-cap/fog updates, free units with RNG).

### 2.6 Reference tool and worked examples

`re/tools/production-ref.mjs` (Node ES module; loads the install via `rulesdump.mjs`, read-only) implements §2.1
(units, buildings, techs, common tail, conversion; spells/DOW, CTW, scenario and AI paths are marked NOT MODELLED),
`pay_cost`, `get_support_count`, `calc_science_discount`, `train_time` (main branches), `update_construct_time` +
`construct_time`, and the N-builder construction rule. `--selftest` checks hand-computed values; `--examples`
prints the table below; `node re/tools/production-ref.mjs <type> [--count N] [--tribe X] [--age A] [--sci N]
[--upgrade] [--builders N] [--trace]` for one case (`--trace` prints every step).

Default leader: Spanish (no relevant bonus), Ancient age, no techs/wonders/rares, game `tech_cost` 3, 2 players.
"Time" is in 1/100 frame; frames = queue frames (`ceil(T/100)+1`) for units/techs, construction frames with the given
builders for buildings.

| Case | Cost | time (1/100 frame) | frames | note |
|---|---|---|---|---|
| Citizen, 3 citizens owned | 23 food | 8250 | 84 |  |
| Citizen, 4 citizens owned | 24 food | 9000 | 91 |  |
| Citizen, 7 citizens owned | 27 food | 11250 | 114 |  |
| Citizen, 12 citizens owned | 32 food | 15000 | 151 |  |
| Citizen, 32 citizens owned | 52 food | 18000 | 181 |  |
| Citizen, 10 owned, Turks | 23 food | 13500 | 136 |  |
| Scholar, 0 owned (Classical) | 30 wealth | 9000 | 91 |  |
| Scholar, 6 owned (Classical) | 42 wealth | 13500 | 136 |  |
| Scholar, 7 owned (Classical) | 44 wealth | 14250 | 144 |  |
| Scholar, 12 owned (Classical) | 59 wealth | 18000 | 181 |  |
| Barracks, 0 owned | 120 timber | 42000 | 420 | 1 builder, 100/frame |
| Barracks, 1 owned | 145 timber | 42000 | 420 | 1 builder |
| Barracks, 4 owned | 270 timber | 42000 | 420 | 1 builder |
| Barracks, 4 owned, Indians | 120 timber | 42000 | 420 | 1 builder |
| Farm, 5 owned | 60 timber | 15000 | 150 | 1 builder |
| Small City, 1 owned | 60 food, 60 timber | 60000 | 600 | 1 builder |
| Small City, 1 owned, Bantu | 15 food, 15 timber | 60000 | 600 | 1 builder |
| Small City, 2 owned, Pyramids | 73 food, 73 timber | 60000 | 600 | 1 builder |
| Hoplites, 0 barracks units (Ancient) | 50 food, 37 timber | 22560 | 227 | metal converted to timber |
| Hoplites, 10 barracks units (Ancient) | 105 food, 83 timber | 30060 | 302 |  |
| Hoplites, 10 barracks units, Classical | 105 food, 67 metal | 30060 | 302 | metal ramp capped at 125 % |
| Hoplites, 10 units, Classical, 2 mil. techs | 99 food, 63 metal | 30060 | 302 | military unit discount 5 % |
| Phalanx upgrade, 10 hoplites | 100 food, 60 metal | 37600 | 377 | research ×2 premium, no refit (same cost) |
| Classical Age | 250 food | 40000 | 401 |  |
| Medieval Age (1 science) | 250 food, 250 knowledge | 55000 | 551 |  |
| Medieval Age (0 science) | 275 food, 275 knowledge | 55000 | 551 | +10 % science penalty |
| Mathematics (library, 1 science) | 120 wealth, 80 knowledge | 27500 | 276 |  |
| Mathematics, Chinese | 96 wealth, 64 knowledge | 27500 | 276 |  |
| Library (building) | 60 timber | 42000 | 420 | 1 builder |
| Library, Greeks | 20 timber | 42000 | 420 | greek_library_cost 66 % |
| Colossus, first wonder | 200 timber, 200 wealth | 400000 | 4000 | 1 builder |
| Colossus, 4 wonders owned | 470 timber, 470 wealth | 400000 | 4000 | n = 2·4 + 1 = 9, ramp 60·9/2 |
| Colossus, 4 wonders, Egyptians | 352 timber, 352 wealth | 400000 | 4000 |  |
| Barracks construction, 1 builder | 120 timber | 42000 | 420 | 100/frame |
| Barracks construction, 2 builders | 120 timber | 42000 | 280 | 150/frame |
| Barracks construction, 4 builders | 120 timber | 42000 | 202 | 208/frame |
| Barracks construction, 8 builders | 120 timber | 42000 | 156 | 270/frame |
| Barracks construction, 2 builders, Maya | 120 timber | 35000 | 234 | maya_building_speed 20 % |

### 2.7 RNG draws

None in §2. (Construction `start` and `activate` draw RNG; frame-spec §8.1 / §10.)

### 2.8 Iteration-order dependencies

- `pay_cost` charges resources 0..5 in order (one bucket write each, then the escrow write). `set_queue` packs the
  first three non-zero resources in resource order.
- Leader loops (age-behind, research speed-ups, Supercollider missiles, American wonder) are counts or "any" tests:
  order-independent.
- Construction: the per-frame sum is independent of builder order, but the builder whose call activates the site
  (and the follow-up order handling) is the first one in unit-pass order whose increment reaches the target.
- `get_cost` is pure; `can_pay_cost`/`pay_cost` read buckets after earlier resources were charged in the same call,
  which cannot change the result (prices do not read buckets).

### 2.9 Rules/data read (all present in `engine/sim/rules.hpp`)

Type: `costs[6]` (+0x18), `job_time`/`res_time` (+0x08/+0x0c), `where` (+0x40), `from` (+0x3c), `jump` (+0x48),
`preq[0]` (+0x30), `cat` (+0x14), `tribe_mask` (+0x10), `is_list`. ObjectType: `domain` (+0x218), `x_size`/`y_size`,
`special_upgrade`/`_cost` (+0x260/+0x264), `support[2]`/`support_cost[2]` (+0x268/+0x270), `obj_masks` (+0x1e4),
`attack` (+0x1e8). Unit: `unit_flags` (+0x2b4, bit 0x80 h, 0x1000000 y), `unit_flags2` (+0x2b8, 8 caravan, 0x10),
`role` (+0x2c8), `military_level` (+0x2dc), `research_premium_cost`/`_time` (+0x2e0/+0x2e4), `job_extra_time` (+0x2e8),
`progression` (+0x2f4). Building: `build_flags` (+0x2c0, 4 needs research, 0x10). Tech: `age` (+0x1c8). Spell: `use[6]`
(+0x1e0). Good: `undiscovered_cost_good/rate` (+0x2b4/+0x2c8), `obsolete_cost_good/rate` (+0x2b8/+0x2cc). Tribe:
`graft[t − 50]`. Constants: every name/offset quoted above (via `constant_offset(name)`).
LeaderData read: `tribe` (+0xc), `leader_flags`/`2` (+0/+4), `city_num` (+0x3f8), `diplos` (+0x74), `invaders`
(+0x290), `escrow` (+0x468), `wonder_mark` (+0x424), `nukes_used`/`missiles_used` (+0x7bc/+0x7c0), `wonders_built`
(+0x854), `barracks/stable/factory/dock/air_units` (+0x9f8..+0xa0c) and `_queued` (+0xa10..+0xa24), `scholar_militia`
(+0x9f0), `num_buildings`, `high_buildings`, `num_units`, `num_queued`, `ages_queued`/`epochs_queued`
(+0x67f4/+0x67f5), `num_bonus_cards[7]` (+0x68fd), `tech`, `conquest_wonders`, `conquest_racial_powers`, `rare`,
`rare_conquest`, `data_encrypted` (`bucket`, `ages`, `epochs`, `epoch[4]`). GameInfo: `starting/ending_technology`,
`tech_cost`, `team_style`, `num_players`, `num_nations`, `semaphore`, `wonders[15]`.

### 2.10 Open questions

1. ❓ `is(X, strict = 1)` (spies/generals tests): `is_strict_list` semantics are in rules-spec; the tool approximates
   with `is_list`.
2. ❓ `get_support_count` American bomber discount: operand of `get_buildings(…, reg_buildings[0x3c][…])` not decoded.
3. 🟡 `recharging` (+0x7a) incremented by `do_construct` once per frame of work: the field is a union for unfinished
   sites (purpose unknown; maybe AI "abandoned site" detection in `Wall::process`).
4. 🟡 The quirk "a city casting its own spell pays 0 but `unpay_cost` subtracts" (§2.1.1.2) — confirm against the
   spells spec (which spells pass `o` of a city building).
5. 🟡 `type_avail(g, 1)` for goods is `has_preq` + `type_eligible`; the obsolete-good path (timber → oil) is untested
   (when is a basic good "obsolete"?). The tool models only the age prerequisites.
6. Values like Citizen 20 + n food: a port should cross-check against a replay (QUEUE_UP followed by a bucket change
   visible in the checksummed leader state).

### 2.11 Evidence

| Function | Address | Used for |
|---|---|---|
| `TypeData::get_cost` | 00664090 | §2.1 (decompile re/decomp/rules3/, asm re/decomp/prod_A_asm/) |
| `TypeData::can_pay_cost` | 00667570 | §2.2.1 |
| `Type::pay_cost` / `unpay_cost` / `sum_cost` / `sum_rules_cost` | 006681f0 / 006682f0 / 00668080 / 006680e0 | §2.2 |
| `TypeData::get_limiting_resource` | 00667730 | §2.2.5 |
| `Build::queue_up`, `BuildQueue::set_queue` | 00620f40, 006309f0 | capture (asm: argument values) |
| `Build::unpay_cost` / `refund_cost` / `unqueue` / `clean_queue` | 006206e0 / 00620490 / 006207c0 / 00620b60 | refunds |
| `Leader::gain_tech` | 006dcb60 | refund_cost call site, before `epoch[3]++` |
| `Group::action_build` | 00707510 | BUILD payment arguments (asm) |
| `LeaderData::calc_science_discount` / `cost_factor_dow` / `get_support_count` | 006da630 / 006da590 / 006da110 | §2.2.6–8 |
| `LeaderData::has_tribe_bonus` / `has_rare` / `has_wonder` / `has_preq` / `type_avail` | 006e1370 / 006e0770 / 006ebc10 / 006db810 / 006e33a0 | predicates |
| `LeaderData::get_graft`, `tribe_can_type`, `UnitTypeData::type_graft` | 006e30f0, 006d9380, 0061d790 | graft mapping |
| `BuildTypeData::is_city / is_fort / is_tower` | 00472bc0 / 00472ba0 / 00472a30 | `is(414)`, `is(443)`, `is(439)` |
| `BuildData::is_unassimilated`, `Game::teams_locked` | 0062d470, 00594880 | ×2 price / ×5/4 time, wonder team ramp |
| `TypeData::time` / `research_time`, `UnitTypeData::research_time` | 00663f20 / 006639a0, 0061d760 | §2.3.1 |
| `ObjectData::train_time` | 006508c0 | §2.3.2 (asm checked for Supercollider and science speed-up) |
| `Build::do_queue` | 0061e410 | per-frame re-evaluation, accel per item class |
| `Wall::update_construct_time`, `Leader::calc_wall_stats` | 0063d560, 006cf7c0 | §2.3.3 |
| `BuildData::construct_time`, `WallData::construct_time` | 0062d5c0, 00642d90 | §2.3.3 |
| `LeaderData::get_building/troops/ships/vehicle_speed_upgrade` | 006dae90 / 006da850 / 006da800 / 006da8a0 | speed upgrade counts |
| `Unit::do_build`, `Wall::do_construct`, `Wall::increment_stats` | 005eebf0, 006434d0, 00643270 | §2.3.4, site counters |
| `UnitData::support_cost` / `control_cost`, `ObjectData/WallData::support_cost` | 0060fde0 / 00609d40, 006452f0 / 0063fa30 | §2.2.9 |
| `SpellTypeData::get_job_time` | 00675800 | cast time (named only) |

## 3. Placing buildings

Names used below: `who` = acting player; `o` = object slot; `(ox, whom)` = target slot and owner. `tcoord(c) = c / 192`
(`div_3_table[c >> 6]`), `wcoord(c) = c / 768`. Building fields are `WallData`/`BuildData` (PDB, `pdbtypes.mjs
WallData`): `flags` +0x08 (VALID 1, STARTED 2, ACTIVE 4, CITY 0x20), `myhits` +0x20, `damage` +0x24, `job_counter` +0x48,
`job_counter_2` +0x4c, `constr_time` +0x50, `construct_hits` +0x54, `frame_started` +0x5c, `build_masks` +0x60 (s16),
`ever_seen` +0x62, `ever_seen_completed` +0x63, `helpers` +0x64 (u8), `orig_type` +0x6c, `city` +0x72, `wonder` +0x76,
`fort/dock/farm/oil_well` +0x78 (union), `recharging` +0x7a, `stance` +0x7e, `founder` +0x7f, `gather_max` +0x80,
`queued` +0x82, `max_age` +0x83, `build_queue` +0x88, `gather_from` +0x98.

`build_masks` bits (PDB unnamed enum `BUILDMASK_*`): PATHING 1, COMP_GATHER 2, MANDATORY 4, HOLDS_AIR 8, ATTACKING 0x10,
UNDER_ATTACK 0x20, INFINITE 0x40, REPEAT_AIR 0x80, MAKE_ROAD 0x100, NEUTRALIZED 0x200, **CONSTRUCTING 0x400**,
**IN_USE 0x800**, ACTIVATED 0x1000, **ORPHAN 0x2000**, EJECTING 0x4000, JAMMED 0x8000. (frame-spec's "0x4000 bit name ❓"
is EJECTING.)

Type fields (BuildTypeData, rules-spec §8): `type` +0x04, `attack` +0x1e8, `hits` +0x210, `domain` +0x218 (0 land,
1 sea only, 2 both), `x_size`/`y_size` +0x234/+0x238, `build_flags` +0x2c0 (letters a..n = bits 0..13: 'e' 0x10 outside
city radius, 'f' 0x20 non-friendly territory, 'g' 0x40 gatherer, 'j' 0x200 one per city, 'k' 0x400 on wall, 'l' 0x800 large
city only, 'n' 0x2000 not captured with city; 0x10000000 FLAT_GATHER), `to` +0x2e0. All are in `engine/sim/rules.hpp`
(`Type`). Constants are read by name; offsets from rules-spec §12.6 are given in the Rules tables (all reachable through
`Constants::at`).

### 3.1 The BUILD command (op 25) ✅

`CommandPackage::process_build(cmd)` @00948110: log; if `package.group ≥ 0`:
`Group::action_build(x, y, x2, y2, type, queued)` @00707510. Returns 0x19. No uid/ownership checks here.

`Group::action_build` (x2/y2 unused), in order:
1. Return unless `GroupData::is_on_map` @0070c450.
2. Scenario prelude (orders-spec §2.1: members in `objects_ignoring_orders[who]` are killed from the group).
3. `GroupData::validate_build(...)` @00708620 (§3.2); return if 0.
4. Return unless `GroupData::num_valid` @0070d7e0 `> 0`.
5. **City limit**: if `types[type].is_city()` (Type vslot +0x64; for buildings `is(VILLAGE 414)`) and `type ≠
   FORBIDDENCITY (531)`: if `LeaderData::get_total_cities` @006d6060 `≥ get_city_limit` @006d6130 → return (UI feedback +
   `S_CITY_LIMIT_REACHED` for the console player only).
6. `action_begin()` (group vslot +0x14: `disband = 0`).
7. Return unless `LeaderData::type_avail(type, 1)` @006e33a0 `== 4`.
8. The target is pushed on a local `Stack<PathData>` and popped once (the loop runs exactly once):
   `snap_center(x, y, &x, &y, who)` @00636190 (world-spec §6.1, now **with** the dock/mine/woodcutter search because
   `who ≥ 0`), then `r = blocked_site(x, y, who, o_self = −1, gather = null)` @00636a50. **`r ≠ 0` → stop silently**
   (validate_build already did the identical test and messaged the player).
9. `city = (build_flags & ('n' 0x2000 | 'e' 0x10)) ? −1 : ObjectsData::find_city_at(x, y, who, −1, 0)` @0065b870 (§5).
10. **Payment** unless `semaphore[1] & 8` (editor) or (`semaphore[1] & 0x10 || semaphore[2] & 2`) and
    `ScenarioData::buildings_free`: `if !type.can_pay_cost(who, city, −1, 1)` (Type vslot +0x84) →
    `LeaderOut::warn_resources` (UI) and stop; else `type.pay_cost(who, −1, city, 1, 0)` (vslot +0xd4). (Cost §2.)
11. `o = Objects::init_build(who, type, x, y, preplaced 0, forced −1)` @0065d190 (§3.3).
    **`o < 0` (no free slot in 2000..2999) → no refund**: the cost stays paid; console player gets
    `S_TOO_MANY_BUILDINGS` + feedback.
12. `Group::action_swarm_around(o, who, queued, BUILD_AT, explicit 1)` @0070fbe0 (§3.4).

Notes: nothing checks that the group contains a builder before paying; a group of soldiers creates (and pays for) an
unbuilt site and is sent to stand next to it. `queued` is the command's QueuePos (QUEUE_FIRST 0 / LAST 1 / NEW 2); it is
only consumed by `action_swarm_around`, so a shift-queued build (QUEUE_LAST) still creates the site and pays **now**.

### 3.2 `GroupData::validate_build` @00708620 ✅

1. `snap_center(x, y, &x, &y, who)`; `r = blocked_site(x, y, who, −1, null)`.
2. `r ≠ 0`: for the console player only: `S_INVALID_BUILD_LOCATION`, feedback text from `BuildTypeOut::say_blocked(r)`,
   `ScenarioData::user_warnings |= 0x20`. Return 0 for everyone.
3. `city` as in §3.1 step 9; return 1 if `can_pay_cost(who, city, −1, 1)`. Otherwise `warn_resources` (UI), in the
   editor / scenario semaphores `user_warnings |= 1`; return 0. **The affordability test is not skipped by the editor
   or `buildings_free` flags here** (only the payment in §3.1 is); ❓ whether `can_pay_cost` itself returns 1 in those
   modes (§2).

### 3.3 Creating the site: `Objects::init_build` → `Build::init` → `Wall::init` ✅

`Objects::init_build(who, type, x, y, preplaced, forced)` @0065d190:
1. `o = find_free(who, 2000, 3000, &build_mark[who], forced)` @0065ad60 (frame-spec §8.1: lowest dead slot with
   `hold_frames == 0`, else `build_mark++`). Return `o` if `< 0`.
2. `snap_center` again (with `who`), then `objects[who][o].get_build().init(who, type, o, x, y, preplaced)` (vslot
   +0x1a0 = `Build::init` @00629740).
3. Presentation: `GraphicPieces::emit_build_particle` unless type ∈ {FARM, MINE, WOODCUTTER}, preplaced, or domain 1
   (no RNG).

`Build::init(who, type, o, x, y, preplaced)` @00629740, in order:
1. `founder = who`; `max_age` = the leader's encrypted age byte (`data_encrypted+0xdc ^ 0x66`); `wonder = −1`;
   `fort/dock/farm = −1`; `orig_type = type`; `healing = 0`.
2. **`Wall::init(who, type, o, x, y, preplaced)`** @0063e9b0:
   - `Object::init` (uid, `add_to_world`, frame-spec §8.1); `flags &= ~(STARTED|ACTIVE)`;
     the dword at +0x60 = 0 (`build_masks`, `ever_seen`, `ever_seen_completed`); `job_counter = job_counter_2 = 0`;
     `helpers = demolition = 0`; `frame_started = −1`.
   - **`start_me(1)`** @0063d790 (world-spec §6.2: STARTED / STARTED2 tile bits, no blocking).
   - `leaders[who].leader_flags |= LEADER_RECOMPUTE (0x2000000)`.
   - UI: `S_CREATED` if the slot is in the console player's selection; `scene.recalc_builds` if on screen.
   - `update_construct_time()` @0063d560 (§2), `update_hits(0)` @0063f0d0 (§4.5), `update_los()` @0063eeb0,
     `check_ever_seen(0)` @0063ce70 (fog-spec §4).
   - Not preplaced: `tile_corner`; unless FARM: `TerrainOut::terraform_for_building(x0, y0, w, h)`,
     `Terrain::refresh_good_z`, `Farms::recalc_heights`; `SubObject::set_new_location(x, y)`. ❓ heights feed object
     z (ammo spawn z, combat-spec §4); treat terraforming as simulation state until shown otherwise.
   - `Unit::update_z` for every land unit in the object lists of the 81 (`is(VILLAGE)`) or 25 wcoords of
     `move_x/move_y[0..n)` around the centre wcoord (z only).
3. `clear_gather()`.
4. Not preplaced: `leaders[who].buildings_built++` (+0x814) (+ Steam stat, local only).
5. `type.attack ≠ 0` → `leaders[who].defense++` (+0x954).
6. `infiltrate/infiltrate2 = 0` (s16 at +0x84), `recharging = 0`, `attack_whom = −1` (s16 write 0x00ff at +0x81 also sets
   `queued = 0`), `gather_down = −1`, `gather_max = 0`, `city = −1`, `city_down = −1`, `attack_ox = −1`.
7. **Stance** by `get_stance_type()` (combat-spec §2.12): 0 → leader option +0xc; 1 → 2 if the owner is not human and
   `starting_resources == 8`, else leader option +0x4; 2 → 1; 3 → `!(leader option +0x1c bit 3)`; else 0.
8. Not a CITY object (`flags & 0x20`, set by `SubObject::init` from `is_city`): `Build::find_city` @00628180 (§5;
   joins a city → `Build::add_to_city`, which counts only **active** buildings; the site's `num_queued[type]++` comes
   earlier, from `Wall::init` → `SubObject::init` → `Wall::set_type(T, INIT)` → `increment_stats`, §4.6; corrected
   from the port). City object: append `{o, who}` to `unbuilt_cities[who]`; **AI first city**: if the owner is not HUMAN and
   `city_mark == 0` (+0x408, no city yet): put every live unit of `who` that is not a citizen/scholar (types 50..53) and
   not special (`unit_flags +0x2b8 & 0x10`, or vslot +0xd4) into a temporary group, `Groups::push_group(who, g, 1)`
   (**allocates a group slot**), and if a slot was obtained `Group::action_move_near((x+0x900, y+0x900), 0, QUEUE_NEW, 1,
   angle 0x60000000, ATTACK_TO, …)`, or `(x−0x900, y−0x900)` with angle 0xE0000000 when the +0x900 point is off the map.
9. `type.is_fort()` (vslot +0xfc) → append to `unbuilt_forts[who]`. Wonder (`is_wonder`, types 526..542) → append to
   `unbuilt_wonders[who]`.
10. **Queue capacity**: `BuildQueue::init(n)` @006307d0 with n = 20 if `is_military_trainer` or `is_training_building`,
    else 10 if `build_flags & 0x08000000` (researches techs), else 2.
11. `is(FARM)` and not preplaced: `farm = Farms::add(who, o)` (farms-spec).
12. `gather_from` reset (+0xb4 = −1, count +0x9c = 0). If gather type (vslot +0x90): non-flat and not
    `is(UNIVERSITY)` → **`Build::find_gather_tiles`** @00623350 (economy-spec §3.6: claims tiles (TData 0x1000) and
    **RNG-shuffles** them: `4·count` iterations, each `i = count ≤ 1 ? 0 : rnd(count)`, move tile i to the end; draws only
    if tiles were added and `count ≥ 2`); otherwise (flat or university) `gather_max = max_gatherers(o, who, x0, y0)`.
13. `can_carry(AIR)` → `build_masks |= HOLDS_AIR | REPEAT_AIR (0x88)`.

The site is now VALID, not STARTED, not ACTIVE; its tiles carry STARTED/STARTED2 but are not blocked.

### 3.4 Assigning workers: `Group::action_swarm_around(ox, whom, q, order, explicit)` @0070fbe0 ✅

Used for BUILD_AT (from BUILD, SWARM_AROUND op 6, `do_build` re-approach, `find_build_spot`, AI), REPAIR (SWARM_AROUND,
`do_repair`, `find_repair_spot`) and GATHER.
1. A temporary empty group `tmp` is created. Return if `!is_on_map` or `objects[whom][ox]` is not VALID.
2. Scenario prelude; `action_begin()`.
3. `q == QUEUE_FIRST`: `set_up_insert` (save the leader's flag-4 orders), `action_halt(0)`, recurse with QUEUE_NEW,
   `finish_insert` (re-issue saved orders as QUEUE_LAST group actions); return (orders-spec §2.1).
4. Group `form = −1`.
5. `kind` (move kind for the approach) = 1, but for BUILD_AT `1 | (owner HUMAN ? 0 : 2)` → **AI builders approach with
   EXPLORE_TO (3)**, humans with MOVE_TO (1) (movement-spec §3.1 kinds).
6. `skip_gatherers` = 0; for BUILD_AT on a wall/building (vslot +0xc) whose type is a gather type:
   `skip_gatherers = count(COUNT_PEASANTS 1) − count(COUNT_GATHER_PEASANTS 0xb) − count(COUNT_BUILD_PEASANTS 0xe)`.
7. `cx, cy` = target position. Ring radius `R`: building target → `min(x_size, y_size)·96 + 48`; unit target →
   `unittype +0x244`. Target `is(FARM)` and BUILD_AT → `R /= 2`.
8. **Two passes** `pass = 0` (land units, `unittype.domain == 0`) then `pass = 1` (sea units); air units (domain 2) never.
   In each pass, members in **list order**; member `u` is considered only if `is_valid_unit` and `is_on_map`, its domain
   == pass, and NOT (`skip_gatherers ≠ 0` and its current action (`get_action`) is GATHER) and `!is_busy()` @0060a370
   (current order's vslot +0xf4 test or entering/exiting a building). Skipped members get nothing at all.
   - BUILD_AT / REPAIR: a member whose type is not 50/51 (citizen), carries no peasants (`count_inside(COUNT_PEASANTS)
     == 0`) and cannot cast CIVILIANSPELL (659) is added to `tmp` (sent to the site later) and gets no order.
   - GATHER: non-citizen/scholar members that cannot cast 659 → `tmp`; citizens: scholars (52/53) only for
     `is(UNIVERSITY)` targets, peasants (50/51) only for non-universities; others → `tmp`.
   - Otherwise (a worker): `a1 = find_angle(u.x − cx, u.y − cy)`. Footprint type `T` for the spot search: if the target
     is a building with domain 1 (sea) and it is not `is(DOCK)` and the member is a peasant and `can_transport(who) ≥ 3`
     → `current_upgrade(TRANSPORTBARGE)`; else if the target is a land building and the member carries peasants and has
     `inside_down ≥ 0` → the first passenger's type; else the member's own type.
     `UnitType::find_nearby_spot(T, cx, cy, &sx, &sy, R, 0, −1, a1, FILTER_NOT_ME 3, u.o, who, 0, 0, ox, whom, −1)`
     @0061de70 (no RNG; ported in `search.cpp`). **Non-zero (no spot) → the member gets no order.**
     `a2 = find_angle(cx − sx, cy − sy)` (facing the site).
     BUILD_AT only: try one cell further out: `nx = sx > cx ? sx + 48 : sx − 48`, `ny` likewise; if
     `find_nearby_spot(T, nx, ny, …, radius 0, …)` succeeds use it.
     BUILD_AT / REPAIR: `q' = q`, except `q == QUEUE_LAST` and the member's current action is GATHER and `explicit` →
     `q' = QUEUE_NEW` (a gathering citizen shift-ordered to build drops the gather).
     `add_move_facing_order(tile48(sx), tile48(sy), a2, kind, pathed 0, q', f7 0, facing −1, −1, −1, 0)` @005e55c0.
     BUILD_AT: if castable(659) `add_cast_order(−1, −1, u.x, u.y, 659, QUEUE_LAST, explicit)`;
     **`Unit::add_build_order(ox, whom, QUEUE_LAST, explicit)`** @005e5210; target `build_masks &= ~ORPHAN`.
     REPAIR: same cast, then `Unit::add_repair_order(ox, whom, QUEUE_LAST, explicit)` @005e4ff0.
     GATHER: the move only (other kinds report "ILLEGAL SWARM AROUND ORDER").
9. After both passes: if `tmp` is non-empty, `tmp.action_move_to(cx, cy, QUEUE_NEW, …, MOVE_TO, …)` @0070fba0
   (always QUEUE_NEW, whatever `q`).

`Unit::add_build_order(ox, whom, q, explicit)` @005e5210 and `add_repair_order` @005e4ff0 follow orders-spec §4.2:
QUEUE_NEW clears the queue; set **UNIT_BUILDER (0x400)**; new BUILD_AT (6) / REPAIR (13) `TargetOrder` with `ox, whom`,
`uid = objects[whom][ox].uid`; flag bit 4 = explicit; append; QUEUE_FIRST rotates it to the front. If the target's tregion
≠ the unit's and `transport_type(u) ≤ level` (level 3 if `LEADER_CAN_TRANSPORT_CIV 0x100`, 2 if `..._MIL 0x200`, 1 if
`..._SCT 0x400`, else 0) and `can_ever_transport` → `UNIT_CAN_TRANSPORT`. REPAIR additionally requires the target tile not
to be water for that.

### 3.5 Placement validation: `BuildTypeData::blocked_site(cx, cy, who, o_self, *gather)` @00636a50 ✅ (asm)

Returns a `BlockIndex` (PDB enum): 0 NONE, 1 BUILDING, 2 MOUNTAIN, 3 ROCK, 4 RUINS, 5 NO_OIL, 6 FOREST, 7 RARE,
8 NEARBY (unused here), 9 NO_RESOURCES, 10 NO_FOREST, 11 FOREST_TAKEN, 12 NO_MOUNTAIN, 13 MOUNTAIN_TAKEN, 14 WATER,
15 LAND, 16 DOCKLAND, 17 DOCKWATER, 18 DOCKTERR, 19 CITY_RADIUS (unused), 20 CITY_DISTANCE, 21 FORT_DISTANCE,
22 FORT_CITY_DISTANCE, 23 TERRITORY, 24 ENEMY_TERRITORY, 25 ENEMY_TERRITORY2, 26 NEUTRAL_TERRITORY, 27 PEACEFUL_TERRITORY
(unused), 28 COLONIZE, 29 PORT_CITY, 30 PORT_FORT, 31 OUTSIDE_RADIUS, 32 OUTSIDE_TOWN, 33 RAZING_TOWN, 34 OFF_MAP,
35 UNSEEN, 36 SEEN, 37 ROAD, 38 NEED_ROAD (unused), 39 ONE, 40 ONE_OTHER, 41 FARM, 42 WONDER, 43 NEED_WALL, 44 RIVER,
45 LOST_CITY, 46 CANT_TRANSPORT. "editor" below = `game.semaphore[1] & 8`; `who < 0` = a check on behalf of nobody
(scenario load, `blocked_town` callers with −1, AI probes).

```
x0 = tcoord(snap(cx)) − (w >> 1); y0 likewise            // snap: tcoord·192 (+96 if size odd), as tile_corner
if type.is_city() and who ≥ 0:                            // LOST_CITY
    for L in 0..7 with VALID: if L == who or mutual alliance(who, L):
        if leaders[who] is HUMAN and L.field_0x410 > frame − 75 and !editor: return 45
best = 0; unseen = 0
for x in x0 .. x0+w−1:        (outer)
  for y in y0 .. y0+h−1:      (inner)
    r = blocked_tcoord(x, y, who, o_self)
    if r == 34: return 34
    if r ≠ 0 and (best == 0 or r == 23):                   // first non-zero wins, but 23 overrides
        if !editor or r ∉ {32,31,36,24,20,23,26,27,46,45}: best = r
    if who ≥ 0 and !was_seen(x>>1, y>>1, who) and !editor: unseen++
if best == 36: return 36
if unseen > (w·h)/2 and !type.is(DOCK 432): return 36
if best == 14: return 14
loc = blocked_location(cx, cy, x0, y0, who, o_self, gather)
if loc == 0 or (editor and loc ∈ {32,31,36,24,20,23,26,27,46,45}): return best
return loc                                                 // the location rule outranks the per-tile result
```
`L.field_0x410` is `LeaderData.lost_city_stamp`, written by `City::close` (§5.8): a human cannot found a city within 75 frames of losing one (own or a mutual ally's). `was_seen` @006b53f0 /
`was_really_seen` @006b54f0 are fog-spec §7.1 (reveal-map, REVEAL_ENEMY/EXPLORE_MAP and `num_units[0x141]` shortcuts).
`blocked_town(wx, wy, who, _)` @00636d80 = `blocked_site(wx·768, wy·768, who, −1, null)`.

### 3.6 `BuildTypeData::blocked_tcoord(x, y, who, o_self)` @00636db0 ✅ (asm)

Two visibility flags: `seen` = `who < 0 or editor or was_seen(x>>1, y>>1, who)`; `really` = `seen and (who < 0 or
editor or was_really_seen(x>>1, y>>1, who))`. "**C?code**" below means `return seen ? code : 36`; "**R?code**" means
`return really ? code : (seen ? 35 : 36)`. First hit wins. `m` = TData mask, `W` = WData of `(x>>2, y>>2)`.
1. Off the tile map → **34** (no visibility masking).
2. `W.flags < 0` (bit 15, GOODY) → **R?4**.
3. `(m & 3) == 3` (BUILDING) → **R?1**.
4. Unless `type.is(DOCK)`: domain 1 (sea): if `who ≥ 0` and `can_transport(who) == 0` and !editor → **C?46**; if the
   tile is not water (`m & 0x30 ≠ 0x20`) → **C?15**. Domain 0 (land): water tile → **C?14**. (Domain 2 skips.)
5. `m & 0x4000` (BLOCKED): tree (`m & 0x30 == 0x30`) → **C?6**; mountain (`m & 3 == 2` or `W.flags & 0x10`) → **C?2**;
   else **R?7**. Else `m & 0x200` (RESOURCE) → **R?7**.
6. Not oil (`!is(OILWELL 421) && !is(OILPLATFORM 422)`): `W.flags & 8` (ROCK) → **C?3**. Oil types:
   `!(W.flags & 0x800)` (no OIL) → **C?5**.
7. `m & 0x80` (STARTED) and `find_building_placed_at(x, y, who, o_self, who)` @00658c80 ≥ 0 (an **own** unbuilt site
   other than `o_self` covers the tile) → **R?1**.
8. Not 'e' (0x10) and `!(m & 0x100)` (outside every CITY radius) → **C?31**.
9. City or fort (`is_city()` / `is_fort()`, i.e. `is(VILLAGE)` / `is(FORTX 443)`), `who ≥ 0`, not Lakota (tribe bonus
   0x13), and not (fort, Dutch bonus 0x16, `dutch_fort_placement ≠ 0`): **if the tile is unowned (`W.owner < 0`)**:
   `rg = get_tregion(x, y)`; if `reg_cities[who][rg] ≠ 0 or reg_forts[who][rg] ≠ 0` → return `is_fort ? 26 : 23`;
   else for every entry of `unbuilt_cities[who]` then `unbuilt_forts[who]` (list order) with `o ≥ 0`, `o ≠ o_self` and
   tregion of its centre tile == rg → `seen ? (is_fort ? 26 : 23) : 36`. (Expansion must stay inside your own territory
   on a landmass where you already have a city or fort, built or unbuilt; a new landmass may be settled on unowned land.
   `reg_cities/reg_forts` are `ushort[64]`, indexed without a bound check.)
10. 'k' (0x400) → **C?43**.
11. `m & 0x800` (river bank) → `36 + 8·seen` = **C?44**.
12. Gather type ('g', vslot +0x90) and flat (FLAT_GATHER 0x10000000, vslot +0x94): `LandData::get_amount(get_land(x, y, 1),
    get_good())` @0067e6d0 == 0 → **C?9**.
13. Return 0.

### 3.7 `BuildTypeData::blocked_location(cx, cy, x0, y0, who, o_self, *gather)` @006375b0 ✅ (asm)

`tcx = tcoord(cx)`, `tcy = tcoord(cy)`. `dist(a, b)` = `vector_dist` @0046cff0 on `|dx|, |dy|` in tcoords:
`hi = max, lo = min; hi == 0 → 0; lo ≥ 60000 → (lo + 2·hi) >> 1; else lo·lo / (2·hi) + hi` (unsigned arithmetic;
the fort checks inline the same formula). `coast_ok = 0`.

**A. Only if `who ≥ 0`** (else go to F). `rg = get_tregion(tcx, tcy)`.
- A1. Neither city nor fort: unless 'f' (0x20) or `type == DOCK` or editor:
  `nf = non_friendly_territory(x0, y0, who)` (§3.8): `nf ≥ 3` → **24**; `nf == 2` → **26**; `nf == 1` → **26** unless Lakota
  (0x13). Then continue at C.
- A2. City or fort: `in_enemy_territory(x0, y0, who)` → **`city_num ≠ 0 ? 24 : 25`**. Then, if `reg_forts[who][rg] == 0
  and reg_cities[who][rg] == 0 and city_num ≠ 0 and first_city_near_coast ≠ 0` (a new landmass):
  `!has_preq(COLONIZE_BONUS 687)` → **28**; scan `circle_x/y[0 .. circle_radius[first_city_near_coast / 4])` (C
  division) around the centre wcoord `(wcoord(cx), wcoord(cy))`: the first point that is `valid`, `is_ocean` and whose
  WData region has size (`regions[r]+0x14`) ≥ 25 sets `coast_ok = 1`; none → **`is_city ? 29 : 30`**.
- C. City spacing (`is_city`): for L = 0..7 with VALID:
  `sp = city_spacing`; if `reg_terr[who][rg] ≥ regions[rg].size·9/10` → `sp −= relax_city_spacing` (who's territory share
  of the landmass, recomputed per L, same value).
  - `unbuilt_cities[L]` in list order, entries with `o ≥ 0`: counted if `L == who and o ≠ o_self` (own other sites, started
    or not), otherwise only if the site is STARTED (other players' sites, and `o_self` itself); counted site whose centre
    tile's WData region == rg and `dist(site − (tcx, tcy)) ≤ sp` → **20**.
  - If `L == who` or `reg_cities[L][rg] ≠ 0`: every VALID city of L (`cities[L][0 .. city_mark)`) with `c.region (+0xa) ==
    rg` and `dist(tcoord(c.x +0xc, c.y +0x10) − (tcx, tcy)) ≤ sp` → **20**.
- D. Fort spacing (`is_fort`): `rg2` = WData region of the centre **wcoord** (not `get_tregion`). For L = 0..7 with VALID:
  - If `L ≠ who`: `sp2 = (mutual alliance(who, L)) ? fort_spacing : fort_to_enemy_city_spacing`.
    STARTED `unbuilt_cities[L]` in rg2 within `dist ≤ sp2` → **22**; if `reg_cities[L][rg2] ≠ 0`, VALID cities of L in rg2
    within `sp2` → **22**.
  - All L: `unbuilt_forts[L]` (same counting rule as C) in rg2 within **`fort_spacing`** → **`L == who ? 21 : 22`**; if
    `L == who or reg_forts[L][rg2] ≠ 0`: VALID forts of L (`forts[L][0 .. fort count +0x428)`, position of the fort's
    building) within `fort_spacing` → **`L == who ? 21 : 22`**.
- E. Town rules, unless 'e' (0x10): `town = get_town(tcx, tcy, who)` @00639b90 (every in-map footprint tile must have the
  CITY bit, then `find_city_at` / `find_town_at` for 'l'; returns the city's building `o` or −1; §5).
  - `town < 0` → **`'l' ? 32 : 31`**.
  - Any item `i < queued` of the city building's queue with `get_queue(i)` ∈ {DISBAND 666, RAZE 667} → **33**.
  - 'j' (one per city) and `count_buildings(town_city, type, 0, 0) ≠ 0`: for each other VALID own city c (index order):
    `r = min(64, LeaderData::get_radius(c's building type))` (of c's owner); if `dist(c − (tcx, tcy)) ≤ r` and
    `count_buildings(c, type, 0, 0) == 0` → **40**; after the loop → **39**.
  - `type == FARM` exactly and `count_buildings(town_city, FARM, 0, 0) ≥ CityData::get_farm_limit(town_city)` → **41**.
    `get_farm_limit` @00738230 = `LeaderData::get_farm_limit` @006db270 + `(lvl − 1)·farms_per_city_level`, lvl = 2 for
    TOWN 415, 3 for METROPOLIS 416 or FORBIDDENCITY 531, else 1; the leader part = `farms_per_city_base`
    (`egyptian_farms_per_city_base` with tribe bonus 7) `+ taj_farms` (has TAJMAHAL 539) `+ kremlin_farms` (KREMLIN 538)
    `+ olive_farms` (rare or conquest rare resource bit 4.1).
  - Wonder type (526..542) and not `is(REDFORT 534)`: `num_wonders(town_city, 1) > (tribe bonus 7 ? 1 : 0)` → **42**.

**F. Water and adjacency** (also for `who < 0`):
- Docks (`is(DOCK)`): `nw = count_water(x0, y0)`. `nw < (w·h·3)/4`: `nw == 0` → **17**; `check_land_adjacent(x0, y0, who)
  ≠ 0` and !editor → **18**; else **17**. Otherwise `cla = check_land_adjacent`: 1 and !editor → **18**; 2 → **16**.
- Not a dock: domain 1 and `count_water < w·h` → **15**; domain 0 and `count_water ≠ 0` → **14**.
- City or fort: `d = coast_ok ? 1 : 2`; `count_mountains_adjacent(x0, y0, d) ≠ 0` → **2**; `count_trees_adjacent(x0, y0, d)
  ≠ 0` → **6** (a two-tile clear ring, one tile when founding on a new coast).
- Oil types: `who ≥ 0` and `!was_seen(cx / 384, cy / 384, who)` and !editor → **5**.
- Gather type and not flat (woodcutters, mines): `calc_gather(out[6], &amount, &o_self, 0, −1, who, 0, x0, y0, 1, 1, 0,
  list)` @00639e40 (economy-spec §3.4; `list` = `&objects[who][o_self].gather_from` when both ≥ 0, else null).
  `gather` non-null → `*gather = max(amount, 0)`. `amount == 0` → **`MINE ? 12 : (WOODCUTTER ? 10 : 9)`**; `amount < 0`
  → **`WOODCUTTER ? 11 : 13`**.
- Return 0.

### 3.8 Territory and adjacency helpers ✅ (asm)

All iterate x outer, y inner, skip off-map tiles. "owner" = `WData.who` (+0xf, s8) of `(x>>2, y>>2)`; "friendly" =
`owner == who` or `diplos[who][owner] == 2 && diplos[owner][who] == 2` (mutual alliance).
- `non_friendly_territory(x0, y0, who)` @006389c0, footprint, non-water tiles: friendly → skip; `owner < −1` → r = 2;
  `owner == −1` → r = max(r, 1); other owner: if either `diplos` side is 0 (war) → **return 3**, else r = 2. Return r.
- `in_enemy_territory(x0, y0, who)` @00638b40, footprint: owner ≥ 0, not who, not mutually allied → return 1 if
  `city_num ≠ 0`, or if the tile's WData region < 64 and `reg_cities/reg_forts[who][region] ≠ 0`, or if
  `WorldData::is_really_seen(x>>1, y>>1, owner)` @006b42c0 (the owner currently sees it). Else continue; return 0.
- `check_enemy_adjacent(x0, y0, who)` @00638d70 (used by `Wall::process` for docks, not by placement): ring
  `[x0−1, x0+w] × [y0−1, y0+h]` minus the footprint and the 4 corners, non-water: first tile with `owner < 0` → return it;
  first friendly → return that owner; enemy tiles are skipped. Ends with the last enemy owner seen, or −1.
- `count_water(x0, y0)` @00638ce0: footprint tiles with `m & 0x30 == 0x20`.
- `check_land_adjacent(x0, y0, who)` @00638ed0: the same ring; tiles not water and not BLOCKED set `land = 1`; return 0 at
  the first such tile if `who < 0`, friendly, or (`owner == −1` and Lakota). End: `land ? 1 : 2`.
- `count_trees_adjacent(x0, y0, d)` @00639090 / `count_mountains_adjacent` @00639180: box `[x0−d, x0+w+d) × [y0−d,
  y0+h+d)` minus the footprint and the 4 box corners; count `m & 0x30 == 0x30` (trees) / `m & 3 == 2` (mountain).

### 3.9 Which checks run where

- **Simulation**: the full `blocked_site` runs inside command processing (`validate_build`, `action_build`, with the
  issuing player's `who`), in `Wall::do_construct` (first builder, `o_self` = the site, §4.2), in `snap_center` (docks,
  mines, woodcutters), and in AI code. Every rule above is therefore part of the lockstep contract, including the
  fog-dependent ones (35/36, oil 5, `in_enemy_territory`'s `is_really_seen`): they read the issuing player's simulated
  fog, not the local view.
- **UI only**: placement cursor/feedback (`BuildTypeOut::say_blocked`, sounds, `user_warnings`), the city-limit message.
- Placement never changes grid state (world-spec §6.4).

### 3.10 RNG draws (placement)

| Site | Function | Draw | Condition |
|---|---|---|---|
| inline | `Build::find_gather_tiles` @00623350 (from `Build::init`) | `rnd(count)` × `4·count` | woodcutter/mine sites that claimed ≥ 2 tiles |
| — | `Groups::push_group` in `Build::init` | none, but allocates a group slot | AI's first city site |

`snap_center`, `blocked_*`, `find_nearby_spot`, `add_*_order`, `action_build`, `action_swarm_around`: no RNG.

### 3.11 Rules / data read (placement)

| Item | Where | In rules.hpp |
|---|---|---|
| Type: type, x_size, y_size, domain, build_flags, attack, `is` lists | everywhere | yes |
| Constants 0x138 city_spacing, 0x13c relax_city_spacing, 0x140 first_city_near_coast, 0x144 fort_spacing, 0x148 fort_to_enemy_city_spacing, 0x2a0 farms_per_city_base, 0x2a4 farms_per_city_level, 0x508 taj_farms, 0x518 kremlin_farms, 0x650 egyptian_farms_per_city_base, 0x968 olive_farms, 0x8a4 dutch_fort_placement | §3.6–3.7 | via `Constants::at` |
| Leader: leader_flags, diplos +0x74, city_num +0x3f8, city_mark +0x408, field +0x410 (lost-city stamp ❓), fort count +0x428, reg_cities +0x125e, reg_forts +0x12de, reg_terr +0x145e, tribe bonuses 0x7 (Egypt), 0x13 (Lakota), 0x16 (Dutch), tech COLONIZE_BONUS | §3.5–3.8 | sim state |
| World: TData mask, WData flags/owner/region, tregion, regions[].size (+0x14), fog `seen2` | §3.5–3.8 | world.hpp |
| unbuilt_cities / unbuilt_forts per player (append order = creation order) | §3.6–3.7 | new |

### 3.12 Open questions (placement)

1. ~~`LeaderData+0x410`~~: `lost_city_stamp`, written by `City::close` (§5.8).
2. `WData.owner < −1` meaning (non_friendly_territory r = 2) ❓.
3. `find_town_at` ('l' buildings) and `find_city_at` tie rules: §5.
4. Whether `can_pay_cost` is unconditional in the editor / `buildings_free` (validate_build) — §2.
5. Terraforming (`terraform_for_building`) as simulation state ❓.

### 3.13 Evidence (placement)

| Function | Address | Used for |
|---|---|---|
| `CommandPackage::process_build` | 00948110 | op 25 dispatch |
| `Group::action_build` | 00707510 (asm) | order of checks, payment, no refund |
| `GroupData::validate_build` | 00708620 (asm) | pre-check |
| `Objects::init_build`, `Objects::find_free` | 0065d190, 0065ad60 | site creation |
| `Build::init`, `Wall::init`, `Wall::start_me` | 00629740, 0063e9b0, 0063d790 | initial state |
| `Build::find_gather_tiles` | 00623350 | RNG shuffle |
| `Group::action_swarm_around` | 0070fbe0 (asm) | worker assignment |
| `UnitType::find_nearby_spot`, `Unit::add_move_facing_order` | 0061de70, 005e55c0 | spots, approach |
| `Unit::add_build_order`, `Unit::add_repair_order` | 005e5210, 005e4ff0 | order creation |
| `BuildTypeData::blocked_site`, `blocked_tcoord`, `blocked_location`, `blocked_town` | 00636a50, 00636db0, 006375b0, 00636d80 (asm) | rule list |
| `non_friendly_territory`, `in_enemy_territory`, `check_enemy_adjacent`, `count_water`, `check_land_adjacent`, `count_trees_adjacent`, `count_mountains_adjacent`, `get_town` | 006389c0, 00638b40, 00638d70, 00638ce0, 00638ed0, 00639090, 00639180, 00639b90 (asm) | helpers |
| `ObjectsData::find_building_placed_at` | 00658c80 | own-site overlap |
| `vector_dist` | 0046cff0 | octagonal distance |
| `CityData::get_farm_limit`, `LeaderData::get_farm_limit` | 00738230, 006db270 | farm limit |


## 4. Construction, completion, repair, deletion

### 4.1 `Unit::do_build(order)` @005eebf0 (BUILD_AT handler, from `do_job`) ✅ (asm)

`(ox, whom)` from the order. `B = objects[whom][ox]`.
1. `ox < 0 or whom < 0 or !B.is_valid_wall()` (vslot +0xc): `kill_current_order(0)`; if orders remain and the new front
   order's type ≠ NONE → return; else → `build_done(ox, whom, unit_gather_respond_range·192)` (§4.7). (No uid test here;
   ❓ whether `work()` validates the uid first, orders-spec §5.)
2. **`B.is_active()`** (vslot +0x4c, already finished): `kill_current_order(0)`; if more orders → `check_build_order()`
   (§4.7); else:
   - if (`B.is(OILPLATFORM)` or (not UNIT_AI_CONTROL and worker stance ∈ {GATHER 0, BUILD_AND_GATHER 1})) and
     `whom == who` and B's type is a gather type and `!B.is(UNIVERSITY)` → `add_gather_order(ox, QUEUE_NEW, 0)`, UI: drop
     the unit from the console player's selection when > 1 selected, `unit.group (+0x80) = −1`; return.
   - else `build_done(...)`.
3. `!adjacent_to(ox, whom)` (vslot +0x170, `Object::adjacent_to` @00651f40: `attack_dist(target, own position) < 96`;
   sea units use `boat_garrison_max_distance` (+384 if `unittype+0x248 < 4`) unless unit-type flag +0x2b4 & 0x10):
   **re-approach**: keep `explicit = order.flags & 4`, `kill_current_order(0)`, temporary group of this unit,
   `action_swarm_around(ox, unit.who, QUEUE_FIRST, BUILD_AT, explicit)`; return. (Note `whom` is replaced by the unit's
   own `who`.)
4. If the unit stands on the footprint (`WallData::covers_tile(tcoord(u.x), tcoord(u.y))` @006439b0) and B is not a farm
   → same re-approach.
5. `set_anim(B.is(FARM) ? CHAR_SOW 0x23 : CHAR_BUILD 0x21, 0, 1)` @00616f40; face B: `a = find_angle(B.x − u.x,
   B.y − u.y)`; `if angle ≠ a: set_angle(a, _, 0)`.
6. `unit_masks & UNIT_DECOY` → return (decoys pretend).
7. `amount = accel_construct` (0x22c); unless (Korean tribe bonus 0x10 and `korean_build_under_fire ≠ 0`):
   `B.build_masks & UNDER_ATTACK (0x20)` → `amount /= 4` (signed).
8. `if !B.do_construct(amount)` (§4.2) → return.
9. Finished (this call activated B): `kill_current_order(0)`; `ot = order_type()`. If `ot ≠ NONE`: unless (B's type is a
   gather type, `!B.is(UNIVERSITY)`, `unit.group ≥ 0`, and after `Group::normalize` that group has ≥ 2 peasants) →
   `check_build_order()`; return. Then the gather rule of step 2 (same condition) → `add_gather_order(ox, QUEUE_NEW, 0)`;
   else `ot == NONE` → `build_done(ox, whom, unit_gather_respond_range·192)`, otherwise `check_build_order()`.

No `game_random` draw in `do_build` itself (the animation start may draw in `Guy` code ❓, anim-spec).

### 4.2 `Wall::do_construct(amount)` @006434d0 ✅ (asm)

```
if ai_speed > 1: amount *= ai_speed
if !is_started():                                   // first builder of the site
    r = blocked_site(x, y, who, o, null)            // with o_self = this site
    if r == 42 (WONDER) and city ≥ 0 and num_wonders(city, 1) ≤ 1 + (tribe bonus 7 ? 1 : 0): start(1)
    elif r ∈ {0, 39 ONE, 40 ONE_OTHER, 41 FARM, 43 NEED_WALL}: start(1)    // vslot +0x1a4 = Build::start
    else:
        Object::disband(this, 1)                    // full refund, no RNG (§4.9); console: "construction blocked"
        return 0
if is_active(): return 0
inc = amount / (helpers + 1)                        // helpers = builders already served this frame
if !(build_masks & IN_USE):  recharging++ ; build_masks |= IN_USE          // Build+0x7a: frames with construction
helpers++
inc = max(inc, 1)
job_counter_2 += inc;  job_counter += inc           // both u32
if construct_time(0) ≤ job_counter (unsigned):      // vslot +0x18c = BuildData::construct_time
    activate(0, 1, 1)                               // vslot +0x1a8 = Build::activate (§4.6); UI message/event
    return 1
(UI: scene.recalc_builds when progress crosses 0.84)
return 0
```
- So a site whose location has become invalid (territory, spacing, a building or resource on the footprint, …) is
  **disbanded with a full refund** when the first builder arrives; one-per-city / farm-limit / wall violations are
  tolerated at that point, and the wonder limit is re-tested allowing the site itself.
- **Diminishing returns**: in one frame the k-th builder (unit processing order) adds `amount / k` (k = 1, 2, …), at least 1.
  `helpers` is reset by `Wall::process` (building pass, after the unit pass): `helpers == 0 → build_masks &=
  ~CONSTRUCTING`, else `helpers = 0; build_masks |= CONSTRUCTING`; then `build_masks &= ~IN_USE` (frame-spec §10).
- Units standing on the footprint do not block the start (blocked_tcoord ignores units); ❓ how other units leave the
  newly blocked tiles (movement collision code).
- `construct_time(0)` = `BuildData::construct_time` @0062d5c0 over `constr_time` (+0x50, filled by
  `update_construct_time` @0063d560 at init); the formula (Maya, Versailles, Hanging Gardens, THEPRESIDENT, Iroquois senate,
  American first wonder) is §2. It is re-evaluated on every call, so its bonuses apply live.

### 4.3 Per-frame bookkeeping and AI recruiting in `Wall::process` @00640450 (summary; full step list in §8.1)

Construction-relevant parts, all in the building pass:
- every 32 frames (staggered `(frame + o) % 32 == 0`): `ATTACKING` set → clear it, else clear `UNDER_ATTACK` (two-step
  decay). For **non-human owners and unfinished sites**: every 128 frames, `is(OILPLATFORM)` sites with no friendly unit
  targeting them (`find_unit(x, y, SEARCH_FRIENDLY, who, −1, 0, FILTER_TARGET, o, who, FILTER_ALL, 0, 0, _)`) are
  `disband(0)`ed (no RNG; partial refund §4.9). Wonders and undamaged forts recruit: `limit = max(4, helpers)`; for wonders,
  `f = (float)job_counter / (float)construct_time(0)` and for each leader L = 0..7 with VALID|ACTIVE that is not an ally:
  `L.wonderwin_timer ≠ 0` → `limit++`, else each of L's unbuilt wonders of the same type (`is(type, 1)`) with progress
  `fL` (float, same formula) `> f` and (`fL < f + f` or `fL < 0.35f`) → `limit++`. If `helpers < limit`: the first
  `find_unit(x, y, SEARCH_FRIENDLY, who, 0xf00, 0x200, FILTER_TYPE, PEASANTS 50, 0, FILTER_NOT_BUSY, 0, 0, _)` gets
  `add_build_order(o, who, QUEUE_NEW, 0)`. (Float compares: port with IEEE single precision.)
- helpers / CONSTRUCTING / IN_USE as above.
- Territory attrition (every 32 frames, or 16 with rush rules while war is not allowed; skipped with
  `disable_building_attrition` +0x804): owner of the centre wcoord (docks: `check_enemy_adjacent`); hostile owner → an
  **unstarted** site is `disband(0)` (no RNG); a started one (finished or not) takes `take_damage(8, 0, 1, −1, 1,
  0x55555555, −1, −1)`.

### 4.4 Starting: `Build::start(msg)` @006273a0 → `Wall::start` @0063e810 ✅

`Wall::start(_)` (returns 0):
1. `flags |= STARTED`; `frame_started = frame`.
2. **`kill_competing_buildings()`** @0063d3a0: footprint tiles (x outer, y inner, in-map) → `kill_at_tile(x, y)`
   @0063d440: only if `m & 0x40` (STARTED2, two sites overlap): repeat `s = find_building_placed_at(x, y, any owner −1,
   o, who)` (any owner, any state: active buildings are found too); if `s` is STARTED → stop for this tile; else
   `Object::disband(s, 1)` (**full refund to its owner, no
   game_random draw**; the "destroyed" sound uses `SoundGlobal::random`, local) and search again.
3. `Terrain::object_placed(x0, y0, w, h, 1)`.
4. **`mask_me(1, REGEN_FORCE)`** (world-spec §6.3: blocking, roads, CITY bits; docks BAD_PATH).
5. Fog: for footprint tiles (x outer, y inner, in-map), deduplicated on `(x>>1, y>>1)` consecutive pairs:
   `WData(x>>2, y>>2).was_seen (+0x14) |= 1 << who` and `seen2[(y>>1)·fog_xs + (x>>1)] |= 1 << who` (fog-spec §1).
6. `check_ever_seen(0)`; `mark_behind_tiles(1)` (world-spec §6.2).

`Build::start(msg)`: `Wall::start`; then for wonders only (`is_wonder`, 526..542): `update_local_seen()` (vslot +0x164,
fog-spec §3.3), and if `msg`: UI notice + `S_WONDER_STARTED`. The `engine/sim/placement.cpp` `wall_start` covers steps 4–6
(grid part).

### 4.5 Hit points during construction ✅

`Wall::update_hits(full)` @0063f0d0: `myhits = Object::update_hits` × multipliers (Maya bonus 1 `maya_building_hp`,
Roman bonus 6 forts/non-wonders `roman_fort_hp`, `building_hp_upgrade · get_building_hp_upgrade`, Taj Mahal, Red Fort forts,
Nubian markets, senate/city level, temple upgrades: §5/§2 detail). Then:
```
if !active and job_counter < construct_time(0):
    a = max(1, job_counter >> 5); b = max(1, construct_time(0) >> 5)            // unsigned
    non-wonder: construct_hits = max(1, (a·myhits) / b)                          // product as u32, divided as int
    wonder:     construct_hits = (myhits + 1)/2 + max(1, ((myhits/2)·a) / b)
else construct_hits = myhits
if construct_hits ≤ damage and inside_down ≥ 0 and !can_carry(AIR): eject_contents(1, −1, 0, 1)
return full ? myhits : construct_hits
```
`BuildData::hits(0)` (vslot +0x11c) returns `construct_hits` (DEPOPULATE/RAZE queue scaling aside). **Hit points grow
with progress**: `Wall::inc_time` @0063fb60 (Objects::inc_time, every frame) calls `update_hits(0)` for every inactive
building. Damage to an unfinished site (`Object::take_damage`, combat-spec §5.4): non-quiet, non-air hits also subtract
`50·hp` from `job_counter` (clamped at 0; `job_counter_2` is not reduced); death at `damage ≥ construct_hits`; AI sites
with ORPHAN set are disbanded when `hits ≤ 2·damage` (combat-spec RNG table #11 lists `disband` as `rnd(3)+1`: **for
buildings `Object::disband` draws nothing**, §4.9).

### 4.6 Completion: `Build::activate(loading, msg, count)` @00623e20 ✅ (order from the asm call sequence; branch details 🟡)

Called as `activate(0, 1, 1)` by `do_construct`. **The scenario loader calls `activate(0, 0, 0)`**
(`ScenarioRead::load_build_chunk` @9aa6e0 / `load_city_chunk` @9a9e50, corrected from the port): `loading` is 0 at
scenario load, so the `!loading` branches (high-water marks, Chinese towns, `race`/`founder`) run then; `count` = 0
skips the counters and free units. (`loading = 1` callers: capture / saved-game paths ❓.) In order:
1. `is(LIBRARY 435)` → `Build::new_library`. `recharging = 0`.
2. CITY object with Chinese bonus 0xe, `!loading`, `chinese_large_cities ≠ 0` and not FORBIDDENCITY: `set_type(TOWN 415)`
   (vslot +0x84), `mask_me(1, REGEN_NONE)`, `LEADER_CALC_WALL_STATS` (city_num temporarily +1/−1 around it).
3. `!loading && count`: `leaders[who].last_built[type]` (+0x59f8 + 4·type) = o, `ScenarioData::last_build_built_o[who] = o`.
4. **`Wall::activate(loading, msg, 0)`** @0063e4b0:
   `options.rebuild = 1` (UI); `ever_seen_completed |= leaders[who].ally_mask` (+0x6929); not started → `start(msg)` (a
   preplaced/loaded building starts here); `flags |= ACTIVE`; `build_masks |= ACTIVATED`; `job_counter = job_counter_2 =
   0` (`job_counter` = 0x40000000 for AIRDEFENSE 523: it becomes the turret angle); if 'e' or (`is_build` and
   `city ≥ 0`): **`Wall::increment_stats`** @00643270 (active branch: `num_buildings[type−414]++` (+0x555e),
   `reg_buildings[region][type−414]++` (+0x14de, region = WData region of the centre wcoord, only if < 64), military
   trainers appended to the leader's trainer list +0x6e50); then `num_queued[type]` (+0x5a22) `−= 1` if non-zero (the
   matching `++` happened in `increment_stats` while inactive: **`SubObject::init` → `Wall::set_type(T, INIT)` →
`increment_stats` when the site is created** (corrected from the port; `Build::add_to_city` increments only active
buildings without flag 'e')); `leader_flags |=
   LEADER_RECOMPUTE | LEADER_CALC_WALL_STATS`; plain walls (`!is_build`) `update_seen(0)`; UI event/sound;
   `Doober::remove_cover_doober` (presentation ❓).
5. City objects: `UnbuiltCities::remove_unbuilt_city`, `Cities::init_city` → `city`, `City::find_buildings`, stats
   (`cities_built`, `city_num`, `world_cities`, `city_mine`), Achieve event (local), capital flags, Spanish bonus 9 →
   `LEADER_EXPLORE_MAP`, `Leader::calc_pop_cap`, `Region::fix_borders` … (**§5**).
6. `city ≥ 0` → `City::regen_roads`. Forts: `Forts::init_fort` → `fort`, `remove_unbuilt_fort`; docks: `Docks::init_dock`.
   Senate/temple/market/granary/lumber-mill city flags and the gov-hero spawn (SENATE 438: `train(get_gov_hero)` when no
   gov hero is near and `gov_hero_frame ≤ frame`) — §5.
7. Docks, MARKET 436, TEMPLE 437: `gather_slots[2]++`; if it exceeds `gather_slots_high[2]`, record it and (frame ≠ 0,
   `!loading`, `count`, not a scenario or `building_resource_bonus`) `Build::do_bonus(2, 30)`.
8. **Free units on a new high-water mark**: `n = num_buildings[type] + (to ≥ 0 ? get_buildings(to, num_buildings[to]) :
   0)`; `basic = basic_type()` (vslot +0xe4); if `high_buildings[basic] < n` and `!loading`: store n; KREMLIN →
   `LEADER_KREMLIN_SPY`; if `count` and (not editor/scenario or `building_unit_bonus`), by exact type (each unit via
   **`Build::train`** @0062f9b0, which creates the unit (`Unit::init` → `Guy::init_real`, **≥ 1 game_random draw per guy**)
   and runs `come_out` (rally points; AI army join may draw once); "pop room" = `unittype[u]+0x2f0 + control ≤ pop_cap`;
   `check_population(T) == 0` = not capped for T; units pass through `tribe_can_type`/`current_upgrade`) 🟡:

   | Building | Bonus (id) | Constant | Units |
   |---|---|---|---|
   | MARKET 436 | Nubian (4) | nubian_free_caravan 0x5c4 | 1 CARA if not capped |
   | BARRACKS 427 | Iroquois (0x12) | iroquois_free_scout 0x824 | n × SCOUT (pop room each) |
   | BARRACKS | Roman (6) | roman_barracks_legion 0x618, roman_max_legion 0x61c, roman_age_for_1/2/3 0x624.. | legion count by age (min of two encrypted age fields) × HOPLITES line, capped by max |
   | BARRACKS | British (0xb) | british_age_for_1/2/3 0x6f4.. | 1–3 × BOWMEN line by age |
   | BARRACKS | Aztec (0) | aztec_barracks_light 0x57c, aztec_max_light 0x580 | count by age (patch-version dependent) × SLINGERS line |
   | SIEGEFACTORY 430 / FACTORY 431 | Turk (8); French (0xa) | turk_free_siege 0x668; french_free_supply 0x6b4 | n × CATAPULT line; 1 SUPPLYWAGON |
   | DOCK 432 / 433 / 434 | Spanish (9), age < 5; Dutch (0x16); British (0xb) | spanish_free_trireme 0x684; dutch_free_light_ship 0x8b0; british_free_fishermen 0x704 | TRIREME / BARK / FISHERMEN lines |
   | UNIVERSITY 420 | American (0x14) | americans_free_scholar 0x860 | n × SCHOLARS |
   | any fort | French (0xa) | french_free_general 0x6b0 | 1 GENERAL |
   | AIRBASE 447 | German (0xc); American (0x14) with MODERN_AGE | german_free_fighter 0x71c; americans_free_bomber 0x864 | BIPLANE / BOMBER lines |
   | city object | Korean (0x10) | korean_citizens[clamp(city_num−1, 0, 7)] 0x7b0 | n × citizens |
   | STABLE 428 / AUTOPLANT 429 | Mongol (0x11) | mongol_start/free/three_mil_cavalry 0x800/0x7f4/0x804 | HORSEARCHERS line, count by age/patch |
9. Gather buildings: `gather_slots[g] += gather_max` (g: farm 0, woodcutter 1, mine 4, university 3, oil 5); on a new
   `gather_slots_high[g]` (same guards as 7): `do_bonus(g, …)` with `food_bonus_for_farm`, `timber_bonus_per_wood_slot ·
   new_slots`, `knowledge_bonus_for_university`, `metal_bonus_per_mine_slot · new_slots`, `oil_bonus_for_well`; farms:
   `Farms::add_animals(farm)`. Oil well/platform: `OilWells::init_oil_well` → +0x78.
10. `attack ≠ 0` → `defense++`.
11. **Wonders** (526..542): `UnbuiltWonders::remove_unbuilt_wonder`, `wonders_built++`, `Wonders::init_wonder` → `wonder`;
    every other player's (and own other) **unfinished site of the same type** is `Object::disband(…, 1)` (full refund,
    leaders 0..7, slots 2000..build_mark, no RNG); then per-wonder effects (Terracotta `recharging =
    terra_cotta_free·15`, Kremlin spy flag, Forbidden City, Hanging Gardens / Colosseum / Statue of Liberty / Red Fort /
    Tikal free techs via `Leader::gain_tech` (unqueueing them from any building queue), Space Program reveal …), then
    `LEADER_CALC_UNIT_STATS` — **§5**.
12. Always: `LEADER_CALC_WALL_STATS`; `is(DOCK)` → `Leader::check_transport`; conquest-mode infiltration bits;
    `update_hits(0)` (vslot +0x15c), `update_los()` (+0x160), non-city building in a city → `City::check_upgrade`,
    `update_seen(0)` (+0x174).

`Leader::gain_building` @006e06c0 (check_transport for docks) has no direct caller in this build (Callers.java) ❓.

### 4.7 Builders afterwards ✅

`Unit::check_build_order()` @00603470 (orders remain after the finished/invalid site): walk from the front while the
order is MOVE_TO or BUILD_AT:
- MOVE_TO: stop at an explicit move (flag 4) or at the last order; otherwise step on.
- BUILD_AT whose target exists (alive, uid matches): if the target is not active, remember its `o`; if it is a city, fort
  or tower → stop (the order is kept **and** remembered). Otherwise (or target gone/active): `repath()`,
  `kill_current_order(0)`, stop when the list is empty.
Then with the remembered sites: one → `add_build_order(o, who, QUEUE_FIRST, 1)`; several → `find_units(x, y,
SEARCH_FRIENDLY, who, unit_build_respond_range·192, 0x200, FILTER_BUILDREPAIR, 1, 0, x, 1)`, count per site the other
units whose action is BUILD_AT on it, swap the least-staffed (first minimum) into slot 0, re-add all with QUEUE_FIRST from
last to first (so slot 0 runs next). A remembered city/fort/tower site is re-added while its old order is still queued
(duplicate, as in the original).

`Unit::build_done(ox, whom, range)` @00603bf0 (no orders left, or one):
- more than one order → return.
- AI-controlled: `find_build_spot()`, else `find_repair_spot()`, else (unless `starting_resources == 8`)
  `find_gather_spot(range)` (economy-spec §4.6).
- Human: stance BUILD_AND_GATHER/BUILD → `find_build_spot()`; stance GATHER/BUILD_AND_GATHER → `find_gather_spot(
  unit_gather_respond_range·192)` (success: `group = −1`, UI selection drop); stance BUILD_AND_GATHER/BUILD →
  `find_repair_spot()`; finally if `whom == who`, the site is a gather type, `isnt(UNIVERSITY, 1)`, `isnt(OILPLATFORM, 1)`
  and `num_gatherers < gather_max` → `add_gather_order(ox, QUEUE_NEW, 0)`.

`Unit::find_build_spot()` @00603e20: `R = unit_build_respond_range·192`, doubled for worker stance 1 or 2 (stance =
`unit+0xb1` if the type has a WORKER stance, else 0). `Objects::find_builds(x, y, SEARCH_FRIENDLY, who, R, 0x200,
FILTER_CONSTRUCT 6, 0, x, 0)` @0065a120; keep candidates (search order) that are not UNDER_ATTACK and owned by `who`; none
→ 0. `find_units(…, R, 0x200, FILTER_BUILDREPAIR, 1, 0, x, 1)`; per candidate count units whose action is BUILD_AT on it;
first minimum → `action_swarm_around(site, who, QUEUE_LAST, BUILD_AT, explicit 0)` for this unit alone; return 1.
`Unit::find_repair_spot()` @00604320 🟡 (decompile drops returns): same radius rule; non-human owners at difficulty < 2
(leader `+0x50` override in scenarios) do not auto-repair; `find_any_building(…, SEARCH_FRIENDLY, who, R, 0x200,
FILTER_DAMAGED, −1, −1, FILTER_NOT_UNDER_ATTACK, 0, 0)`; the target tile must be own or mutually allied territory;
→ `action_swarm_around(b, who, QUEUE_LAST, REPAIR, 0)`, return 1.

### 4.8 Repair ✅ (asm)

**How it is issued.** The UI right-click (`Console::execute_at_cursor` @007c6630) with citizens on an own/allied building:
unfinished → `GroupOut::issue_swarm_around(o, whom, q, BUILD_AT)`; finished with `damage ≠ 0` (and the target tile not in
a non-allied player's territory, else "invalid order") → `issue_swarm_around(…, REPAIR)` (gather buildings with free
slots get GATHER first). Both send **SWARM_AROUND (op 6)**: `CommandPackage::process_swarm_around` @00949970 →
`action_swarm_around(ox, whom, queued, orders, explicit 1)` if `whom < 0 or ox < 0 or` the target is alive.
**REPAIR (op 16)** has no UI issuer (only `ScenarioData::issue_order`): `process_repair` @00948cb0 →
`Group::action_repair(ox, whom, q)` @007020c0 repairs a **unit target (ships)**: QUEUE_FIRST → insert pattern; else for
members of type 50/51 whose tregion `Region::is_coast` of the target's tregion: QUEUE_LAST → optional cast 659 then
`add_repair_order(ox, whom, QUEUE_LAST, 1)`; otherwise `add_repair_order(…, QUEUE_NEW, 1)` then cast QUEUE_FIRST; and
for each such member the **target unit's** orders are cleared (`clear_orders` inline). ❓ `do_repair` rejects
non-building targets (step 3 below), so ship repair must complete elsewhere (`Unit::do_gather` calls `add_repair_order`).

**`Unit::do_repair(order)` @005ee420** (REPAIR handler):
1. `set_anim(CHAR_REPAIR 0x22, 0, 1)`. `B = objects[whom][ox]`.
2. **Done** if `B.damage == 0`, or `whom ≠ who` without mutual alliance: `kill_current_order(0)`; AI-controlled →
   `find_repair_spot()`; human, `whom == who`, no orders left, stance < 2, B a building of gather type and
   `!is(UNIVERSITY)` → `add_gather_order(ox, QUEUE_NEW, 0)`. Return.
3. Not a building/wall or not active → `kill_current_order(0)`, return.
4. B UNDER_ATTACK and the order is not explicit → kill, return. B's centre wcoord owned by someone other than `whom` who
   is not allied with `whom` → kill, return.
5. `!adjacent_to` → re-approach (`action_swarm_around(ox, unit.who, QUEUE_FIRST, REPAIR, explicit)`), return.
6. DECOY → return.
7. **Rate** (`P` = period in 1/256 hp per frame units):
   ```
   P = (helpers + 1) · ((construct_time(1) << 9) / (hits(0) · accel_construct))     // unsigned division
   if B is a CITY object: P *= 2; if its city's founder (+0x5f) ≠ owner (+0x5e): P *= 2
   Korean (0x10) and korean_repair ≠ 0: P = (100 − korean_repair)·P / 100
   unless (Korean and korean_build_under_fire ≠ 0): UNDER_ATTACK → P <<= 2; ATTACKING → P <<= 2
   helpers++                                            // shares Wall::helpers with construction
   amount = (P ≤ 0) ? B.damage : (256·frame)/P − (256·frame − 256)/P                 // signed
   if amount == 0: return
   ```
   With one repairer a full repair takes about `2 · construct_time(1) / accel_construct` frames; each extra repairer in
   the same frame works at `1/(k)` speed (k-th repairer). Korean: −`korean_repair`% time.
8. **Cost**: `hl = B.hits_left()` (vslot +0x114). For goods g = 0..5 with `type_avail(g, 1)`: `c = B.type.get_cost(g,
   whom, −1, −1, 0, 1, −1)`; if `c ≠ 0`: `r = (float)hits(0) / (float)c`; `pay[g] = (int)((float)(amount + hl) / r) >
   (int)((float)hl / r) ? 1 : 0` (one unit of g each time the repaired hp crosses a `hits/c` boundary; single
   precision, truncation); if `resources[g] < pay[g]` → `kill_current_order(0)`, `repair_stamp` (+0x208) message throttle
   (150 frames; UI), return.
9. For g = 0..5 with `type_avail`: `resources[g] −= pay[g]`; `escrow[g] = max(0, escrow[g] − pay[g])` (+0x468).
10. `B.repair_damage(amount, 0, 1)` (vslot +0x168 → `Build::repair_damage` @00628130 = `Object::repair_damage`
    @00646f90): `damage = min(damage, hits(0))`; `damage_frac = 0`; `damage = max(0, damage − amount)`.

So repair costs resources: in total roughly the building's current cost (`get_cost` at repair time, owner `whom`'s prices)
spread over its full hit points. No RNG.

### 4.9 Deletion and cancellation ✅

**DISBAND (op 21)**: `process_disband(all)` @00948660 → `Group::action_disband(all)` @0070e260:
1. Scenario prelude. `GroupOut::validate_disband()` @0070a4a0 (UI-named but deterministic; decompile drops returns 🟡):
   building groups of more than one member are refused; a finished building with no DISBAND queued is checked for city
   rules (cities with buildings, besieged/allied-unit cities, `CityData::is_disbanding`) ❓ detail (§5); the result also
   requires the DISBAND spell (666) to be castable. Non-zero → stop.
2. Members in **reverse list order**; alive ones: if not (building and active) — units and **unfinished sites** —
   `Object::disband(m, 0)` (console sound for sites), count; stop after the first unless `all`. Active buildings: if
   `count_queue(DISBAND)` (vslot +0x190) is 0 → `Build::queue_up(DISBAND, 0)`: demolition is a timed queue item
   (§6; RAZE 667 is the city counterpart). UI feedback when only queued buildings were hit.

**`Object::disband(full)`** @006455c0 (all paths):
1. Active gather building: every citizen (50..53) whose action is GATHER on it gets `clear_orders`.
2. `full' = full`, or 1 for walls/buildings of a Lakota owner (bonus 0x13) when `lakota_raze_price ≠ 0`.
3. Refund decision: `!full'` and the object is an **unfinished** wall/building and `job_counter < time()` (type time, or
   object vslot +0xa4 with who): `refund = 1`, `ct = construct_time(0)`, `done = job_counter_2`. `full'` → `refund = 1`.
4. Units only: Aztec plunder on `UNIT_AZTEC_DAMAGE` kills.
5. Active building → `ScenarioData::last_razed[who] = o`.
6. Removal: objects that are not walls, or are STARTED: units on the map → disband contents, then **`die(rnd(3) + 1)`**
   (the only RNG draw, **units only**); buildings → `die(1, −1, 0)`. Unstarted sites → `close(0, −1, 0)`.
7. Refund (after removal), goods g = 0..5 with `type_avail(g, 1)`: partial: if `ct ≠ 0 and done < ct`:
   `c = types[type].get_cost(g, who, −1, −1, 0, 1, orig_type)`; `resources[g] += (int)((float)c · (float)(ct − done) /
   (float)ct)` (single precision: `c` and `ct − done` converted, multiplied, divided, truncated). Full:
   `resources[g] += get_cost(g, who, −1, −1, 0, 1, −1)`. UI "plundered" message for the Lakota case.

Consequences: cancelling an unstarted site refunds the **current** cost in full (`done = 0`); a started one refunds the
unworked fraction measured by `job_counter_2` (damage does not change it); competitor sites removed by
`kill_competing_buildings`, by `do_construct`'s blocked test and by a completed wonder get `full = 1`; AI-abandoned and
attrition-disbanded sites get the partial rule. **No `game_random` draw for any building.**

Closing (death or disband): `Build::close` @00628980 (refund the production queue with `clean_queue` unless editor,
`defense−−`, hotkey groups, stats via `Wall::decrement_stats` for active buildings, gather slots, `DEAD_BUILD` on the centre
wcoord unless captured, gather points) → `Wall::close` @0063f6e0 (`mark_behind_tiles(0)` if started, unfinished:
`Doober::remove_cover_doober` and `num_queued[type]−−` if non-zero, started: cover doober (UI), `start_me(0)`, started and
not captured: `mask_me(0, REGEN_SIMPLE)`, `Object::close`, `refresh_nearby_tiles`) → farm/city/fort/dock/wonder removal
(`remove_unbuilt_city/fort/wonder`, `Cities::close_city`, `Region::fix_borders`, `calc_pop_cap`) → `BuildQueue::close`.
(World-spec §6.2; §5 for the city part.)

**AI abandonment** (no RNG): ORPHAN (0x2000) is set by `Leader::check_orphaned_buildings` @006c9f20 (AI; §5 decompiled it)
for unfinished AI sites nobody is assigned to, and cleared whenever `action_swarm_around` gives a BUILD_AT order. That AI
pass also `action_disband`s some sites (unfinished temples/forts or duplicates; untouched sites in enemy-held cells)
and re-assigns idle citizens (`action_swarm_around(…, QUEUE_NEW, BUILD_AT, 1)`) 🟡. `take_damage` disbands ORPHAN AI sites
at `hits ≤ 2·damage`; `Wall::process` disbands target-less AI oil platforms (§4.3).

### 4.10 BUILDMASK (op 33) ✅

`process_buildmask(mask, set)` @00947680 → `Group::action_buildmask(mask, set)` @006fc9a0 (**`set` is ignored**): only for
building groups (`group.buildings` +0x49). Members in list order; for each building with `WallData::valid_buildmask(mask)`
@0063e2a0 (`mask & INFINITE 0x40` needs `BuildData::can_infinite`; `mask & REPEAT_AIR 0x80` needs `can_carry(AIR)`; other
bits are refused): `all_set` starts true; if `!(build_masks & mask) && all_set` → set the bits; else clear them and
`all_set = false`. So the command toggles: the first building decides, and once one building is cleared every later one
is cleared too. UI messages for INFINITE (repeat production). The player can only change **INFINITE (repeat queue)** and
**REPEAT_AIR (aircraft auto-repeat)**.

### 4.11 RNG draws (construction, repair, deletion)

| # | Site | Function | Draw → use | Condition |
|---|---|---|---|---|
| 1 | via `Unit::init` → `Guy::init_real` | `Build::train` from `Build::activate` | ≥ 1 per guy (idle variant) | each free unit (§4.6 step 8; also SENATE gov hero) |
| 2 | via `Unit::come_out` | `Build::train` | AI army join (one draw, frame-spec §10) | AI-owned free units |
| 3 | @6459c3 | `Object::disband` | `rnd(3) + 1` death type | **units only** (never a building) |
| — | `Build::find_gather_tiles` | (placement table §3.10) | | |

No draws in `do_build`, `do_construct`, `start`, `kill_competing_buildings`, `activate` (other than via `train`),
`update_hits`, `do_repair`, `repair_damage`, `check_build_order`, `build_done`, `find_build_spot`, `action_disband`,
`action_buildmask`. `SoundGlobal::random` (sounds) is a separate local stream.

### 4.12 Rules / data read (construction)

| Item | Offset / where | In rules.hpp |
|---|---|---|
| accel_construct | 0x22c | `Constants::at` |
| korean_build_under_fire, korean_repair, korean_citizens[8] | 0x7dc, 0x7e0, 0x7b0 | yes |
| unit_build_respond_range, unit_gather_respond_range | 0x24, 0x28 | yes |
| boat_garrison_max_distance | 0x94 | yes |
| lakota_raze_price | 0x854 | yes |
| food/oil/knowledge/timber/metal bonus | 0xc2c, 0xc30, 0xc34, 0xc38, 0xc3c | yes |
| free-unit constants | table §4.6 | yes |
| hit-point constants (maya_building_hp 0x58c, building_hp_upgrade 0x154, taj_building_hp 0x4f8, red_fort_fort_hps 0x4c8, roman_fort_hp 0x5fc, nubian_hit_points 0x5cc, senate_hp_bonus 0xc48) | §4.5 | yes |
| Type: hits, attack, build_flags, to, time (job_time), costs via get_cost; unit types: domain, type, +0x244 (swarm radius), +0x248/+0x2b4 (boats), +0x2b8 special flag, +0x2f0 pop cost | | Type yes; the unit-type offsets ❓ names |
| Leader: resources (encrypted, `^0x8221`), escrow +0x468, repair_stamp +0x208, num_buildings +0x555e, high_buildings +0x5660, num_queued +0x5a22, reg_buildings +0x14de, gather_slots +0x8a4 / _high +0x8d4, buildings_built +0x814, defense +0x954, cities_built +0x820, wonders_built +0x854, control +0x940, pop_cap +0x7e4, attrition_stamp3 +0x1fc, disable_building_attrition +0x804, wonderwin_timer +0x44c | | sim state |

### 4.13 Corrections to existing specs

- frame-spec §8.1/§8.2/§10 and combat-spec RNG #11: `Object::disband` draws `rnd(3)+1` **only for units**; disbanding a
  building (competing sites, blocked sites, territory attrition of unstarted sites, AI abandonment) draws nothing.
- frame-spec §10 "occasionally disband abandoned sites (RNG)": the `Wall::process` disband applies only to AI
  **oil platforms** nobody targets (every 128 frames), without RNG; the general AI abandonment is
  `Leader::check_orphaned_buildings` + `take_damage` (ORPHAN).
- frame-spec §8.1: `Build::init` queue capacity is 20 / **10 for tech-researching buildings (build_flags 0x08000000)** / 2.
- orders-spec §2.7: `action_swarm_around` skips busy members and, when building a gather building with idle peasants
  available, gathering peasants; a worker with no free spot gets no order; AI builders approach with EXPLORE_TO; a
  QUEUE_LAST build on a gathering citizen becomes QUEUE_NEW when explicit.
- world-spec §6.4: the per-tile result is overridden by any non-zero `blocked_location` result (only 34, 36, 14 and 45
  return early); 35/36 masking applies per rule as in §3.6 (C?/R?); `in_enemy_territory` returns 25 (not 24) for a
  player without cities.

### 4.14 Open questions (construction)

1. `work()` uid validation before `do_build`/`do_repair` (they test only `is_valid_wall`) ❓.
2. How units inside a newly started footprint are displaced ❓.
3. Ship repair path (REPAIR op 16 → `do_repair` rejects non-buildings) ❓.
4. `validate_disband` exact return paths 🟡; `find_repair_spot` early returns 🟡.
5. Free-unit branch details for Roman/British/Aztec/Mongol counts (age fields `+0xdc`/`+0xe8` of the encrypted block,
   patch version ≥ 4) 🟡 — straightforward from the decompile, not re-read in asm.
6. `Leader::gain_building` caller ❓; `Doober` cover doobers (presentation or state?) ❓.

### 4.15 Evidence (construction)

| Function | Address | Used for |
|---|---|---|
| `Unit::do_build` | 005eebf0 (asm) | builder handler |
| `Object::adjacent_to`, `WallData::covers_tile` | 00651f40, 006439b0 | reach tests |
| `Wall::do_construct` | 006434d0 (asm) | progress, start/disband, helpers |
| `Wall::process` | 00640450 (asm) | helpers reset, AI recruiting, oil-platform disband, attrition |
| `BuildData::construct_time`, `Wall::update_construct_time` | 0062d5c0, 0063d560 | completion threshold (§2) |
| `Build::start`, `Wall::start`, `Wall::kill_competing_buildings`, `Wall::kill_at_tile` | 006273a0, 0063e810, 0063d3a0, 0063d440 | start |
| `Wall::update_hits`, `BuildData::hits`, `Wall::inc_time`, `Object::take_damage` | 0063f0d0, 0062e740, 0063fb60, 00652020 | construction hit points |
| `Build::activate`, `Wall::activate`, `Wall::increment_stats`, `Wall::decrement_stats` | 00623e20 (asm call order), 0063e4b0, 00643270, 00642da0 | completion |
| `Build::train`, `Build::do_bonus` | 0062f9b0, 00627140 | free units, bonuses |
| `Unit::check_build_order`, `Unit::build_done`, `Unit::find_build_spot`, `Unit::find_repair_spot`, `Objects::find_builds` | 00603470, 00603bf0, 00603e20 (asm), 00604320, 0065a120 | after completion |
| `Console::execute_at_cursor`, `GroupOut::issue_swarm_around`, `CommandManager::issue_swarm_around`, `CommandPackage::process_swarm_around` | 007c6630, 0070afc0, 009416b0, 00949970 | right-click build/repair |
| `CommandPackage::process_repair`, `Group::action_repair` | 00948cb0, 007020c0 | op 16 (units) |
| `Unit::do_repair`, `Build::repair_damage`, `Object::repair_damage` | 005ee420 (asm), 00628130, 00646f90 | repair rate and cost |
| `CommandPackage::process_disband`, `Group::action_disband`, `GroupOut::validate_disband`, `GroupOut::issue_disband` | 00948660, 0070e260, 0070a4a0, 0070a470 | DISBAND |
| `Object::disband` | 006455c0 (asm for the refund float) | removal, refunds, RNG |
| `Build::close`, `Wall::close` | 00628980, 0063f6e0 | removal |
| `Leader::check_orphaned_buildings` | 006c9f20 (§5.11) | ORPHAN |
| `CommandPackage::process_buildmask`, `Group::action_buildmask`, `WallData::valid_buildmask` | 00947680, 006fc9a0, 0063e2a0 | BUILDMASK |

Decompile/asm outputs: `re/decomp/prod_B/`, `re/decomp/prod_B_asm/` (jobs `re/jobs/dc_prod_B_1..13.txt`); earlier
dirs `re/decomp/{cmds,cmds2,bld2,orders,grid,grid_build,life2,build_rules_asm,combat_D}/`.

## 5. Cities, walls, wonders

Status: ✅ city data, membership, founding, sizes/upgrades, radius, city limit, closing, capital bookkeeping, fort and
wonder registries, territory *triggers*, wonder completion rules and per-wonder production hooks are specified from the
decompiles, with the disassembly checked where argument lists were unclear (`City::find_buildings`,
`Leader::lost_a_city`, the `Cities::init_city` call in `Build::activate`, the `check_upgrade` loop in
`Leader::gain_tech`, `ObjectsData::find_city_at`, `WorldData::is_city_at`) · 🟡 `Cities::capture_city` (1200 lines:
plunder, unit conversion) and `Build::check_capture` are summarised, not transcribed · ❓ the territory influence
algorithm itself (`World::compute_reg_territory` @006b0bb0) is decoded in §5.10.1 (C++ `engine/sim/territory.cpp`).

Decompiles: `re/decomp/prod_c2/` (job `re/jobs/dc_prod_c2_1.txt`), `re/decomp/prod_c2b/` (`dc_prod_c2_2.txt`,
`dc_prod_c2_3.txt`, callers in `prod_c2b/callers.txt`, `callers2.txt`), asm in `re/decomp/prod_c2_asm/`
(`dc_prod_c2_asm1..5.txt`). Older copies used: `re/decomp/grid_regions/World_compute_reg_territory@006b0bb0.c`,
`re/decomp/grid_build/Build_init@00629740.c`, `re/decomp/prod_C/BuildData_can_make@0062db10.c`,
`re/decomp/prod_B/BuildTypeData_already_built@0063ce10.c`, `re/decomp/combat/ObjectData_health_level@006534b0.c`.

Conventions as combat-spec §0. Type indices: 414 VILLAGE ("Small City"), 415 TOWN ("Large City"), 416 METROPOLIS
("Major City"), 423 GRANARY, 424 LUMBERMILL, 425 SMELTER, 426 REFINERY, 432 DOCK, 435 LIBRARY, 436 MARKET,
437 TEMPLE, 438 SENATE, 439 TOWER, 443 FORTX ("Fort"; Castle 444, Fortress 445, Redoubt 446 chain from it),
526..542 wonders (526 PYRAMIDS, 527 COLOSSUS, 528 HANGINGGARDENS, 529 TERRACOTTA, 530 COLOSSEUM, 531 FORBIDDENCITY,
532 TIKAL, 533 PORCELAIN, 534 REDFORT, 535 ANGKORWAT, 536 VERSAILLES, 537 STATUEOFLIBERTY, 538 KREMLIN, 539 TAJMAHAL,
540 EIFFELTOWER, 541 SUPERCOLLIDER, 542 SPACEPROGRAM). Tribe-bonus numbers k = rules.xml TRIBES order (fog-spec §4):
3 Bantu, 6 Romans, 7 Egyptians, 8 Turks, 9 Spanish, 13 Russians, 14 Chinese, 19 Lakota, 20 Americans, 21 Indians,
23 Persians. "is(t)" = the object/type `is(t, 0)` FROM-chain test (rules-spec §8.4). Object vslots: `+0x0c` / `+0x10`
valid, `+0x20` is_build (Build: `return 1`), `+0x2c` is_wonder, `+0x4c` is_active, `+0x50` is_started, `+0x80`/`+0x84`
set_type, `+0x94` swap_team, `+0x150` close, `+0x158` die, `+0x15c` update_hits, `+0x160` update_los, `+0x168`
repair_damage, `+0x174` update_seen, `+0x1a8` activate. BuildType vslots `+0x64` is_city = is(414), `+0xfc` is_fort =
is(443), `+0x108` is_dock, `+0x1c` is_wonder_type (526..542).

### 5.0 Data ✅

#### 5.0.1 `City` / `CityData` (0xc0 / 0xb8; `cities.lists[who]` = `PtrArray<City>[8]`, index = city number)

| Off | Field | Meaning (set by) |
|---|---|---|
| 0x04 | u16 `city_flags` | PDB enum: 0x1 VALID, 0x2 UNDER_ATTACK, 0x4 ATTACKING, 0x8 EVER_ATTACKED, 0x10 CAPITAL, 0x40 ALARM, 0x80 HAS_TEMPLE, 0x100 GOT_CAPTURED, 0x200 HAS_GRANARY, 0x400 HAS_LUMBER, 0x800 HAS_MARKET, 0x1000 VERY_NEAR_BORDER, 0x2000 CROWDED, 0x4000 ORIGINAL_CAPITAL, 0x8000 WAS_ORIG_CAPITAL |
| 0x06 | i16 `city` | own index in `cities.lists[who]` |
| 0x08 | i16 `o` | the city building's object slot (also the head of the city's building list, §5.2) |
| 0x0a | i16 `reg` | land region of the city's wcoord (`WData+4`) at init / capture |
| 0x0c, 0x10 | Coord `x`, `y` | plain (not XORed) copy of the building centre |
| 0x14..0x24 | `attack_stamp`, `raid_stamp`, `reduce_stamp`, `capture_stamp`, `assimilation_timer` | frame stamps |
| 0x28 | `capture_strength` | capturing unit's capture value (`Build::check_capture`) |
| 0x2c | int[8] `traded_with` | |
| 0x4c, 0x4e, 0x50, 0x52, 0x54 | `scouted`, `in_port`, `peasant_dist`, `trade_val`, `conquest_node` | |
| 0x56..0x59 | u8 `granary`, `lumber_mill`, `smelter`, `refinery` | enhancer levels (economy-spec) |
| 0x5a..0x5d | `free`, `busy`, `gatherers`, `pop` | |
| 0x5e | i8 `who` | owner |
| 0x5f | i8 `race` | **the player the city is assimilated to**; `race != who` = unassimilated (captured, §5.6) |
| 0x60 | i8 `founder` | original founder |
| 0x61..0x67 | `plundered`, `ocean`, `land`, `filled`, `bordering`, `ocean_filled`, `dock_tile` | |
| 0x68 | u8 `was_capital_flags` | bit p = "this was player p's capital" (captured capitals) |
| 0x6c | u8[6] `ter` | |
| 0x74 | `Array<CaravanLink>` `vans` | count +0x78, capacity +0x7c, data +0x84 |
| 0x90, 0xa4 | String `name`, `id` | file-only (not checksummed, state-spec §5) |

#### 5.0.2 Other records

- `Fort` (0x10; `forts.lists[who]`): +0 i16 `fort` (own index), +2 i16 `o`, +4 i16 `reg`, +6 `fort_flags` (1 VALID),
  +7 i8 `who`.
- `Wonder` (0x18; `wonders.lists[who]`): +0 i16 `wonder`, +2 i16 `o`, +4 `stamp` (frame of completion), +8 `timer`,
  +0xc `wonder_flags` (1 VALID), +0xd i8 `who`.
- `UnbuiltCities` / `UnbuiltForts` / `UnbuiltWonders`: per-player `Array` of `{i16 o; i8 who}`; entries appended by
  `Build::init` (inlined `add_unbuilt_*`; the out-of-line functions have no callers) and removed by
  `remove_unbuilt_*` in `Build::activate` / `Build::close`.
- `Build` fields used here: +0x72 i16 `city` (city index or −1), +0x74 i16 `city_down` (next building in the city
  list), +0x76 i16 `wonder`, +0x78 i16 `fort` (union with dock/oil_well/farm), +0x7a i16 `recharging` (Kremlin/Terra
  Cotta timer), +0x60 `build_masks` (0x100 = "place roads next 16-frame tick").
- LeaderData: +0x3f8 `city_num` (owned live cities), +0x400 `city_mine` (owned cities incl. Forbidden City, used by the
  city limit), +0x408 `city_mark`, +0x40c `city_name` (next-name counter), +0x410 `lost_city_stamp`,
  +0x414 `lost_capital_stamp`, +0x418 `lost_capital_timer` (flag), +0x41c `lost_capital_modifier` (×256), +0x424
  `wonder_mark`, +0x428 `fort_mark`, +0x448/+0x44c `wonderwin_stamp/_timer`, +0x820 `cities_built`, +0x824
  `cities_captured`, +0x828 `cities_lost`, +0x854 `wonders_built`, +0x858 `wonders_held` (high-water mark), +0x85c
  `senates_built`, +0x8f0 `forts_built`, +0x95c `pop`, +0xe62 `reg_pop[64]`, +0x125e `reg_cities[64]`, +0x12de
  `reg_forts[64]`, +0x145e `reg_terr[64]`, +0x9d8 `territory`, +0x9dc `territory_high`. Encrypted
  `epoch[4]` at `LeaderDataEncrypt+0xe8` (XOR 0x63187): [0] military, **[1] civic (+0xec)**, [2] commerce, [3] science.
- `Game`: +0x618 int[17] `wonders` (bit 0 = "wonder type 526+i has been completed by someone"; never cleared in
  play), +0x6c8 `terra_cotta_total`, +0x6cc `world_pop`, +0x6d0 `world_cities`.
- `Region` (+0x88 each, 128 regions; 0..63 land): +0x14 `size` (wcoords), **+0x2c `borders`** (territory cursor,
  §5.10), +0x8 `flags`.

#### 5.0.3 Slot allocation (`Cities::init_city` @007352c0, `Forts::init_fort` @0073ed70, `Wonders::init_wonder`
@0073c860 — identical) ✅

```
idx = first i in [0, mark) whose record is not VALID           // mark = city_mark / fort_mark / wonder_mark
if none: if mark < list.count: idx = mark                      // reuse an allocated record past the mark
         else: append a new record (new City / Fort / Wonder), idx = count − 1
mark = max(mark, idx + 1)
```
Closing (`Cities::close_city` @00733310, `Forts::close_fort`, `Wonders::close_wonder` @0073c7e0) invalidates the
record, then lowers `mark` while the record at `mark − 1` is invalid. No RNG.

### 5.1 City types, level, radius, pop value ✅

A building is a city when its type `is(VILLAGE 414)`: Small/Large/Major City **and the Forbidden City (531, FROM
Small City)**. `SubObject::init` sets object flag `OBJECT_CITY 0x20` from BuildType vslot 0x64. All helpers key on the
exact type index of the city object:

| Function | VILLAGE / other | TOWN 415 | METROPOLIS 416 or FORBIDDENCITY 531 |
|---|---|---|---|
| `CityData::get_level` @00739340 (also inlined in `City::init`, `get_radius`, territory) | 1 | 2 | 3 |
| `CityData::get_pop_value` @00738450 (→ `LeaderData.pop`, `world_pop`, `reg_pop`) | 1 | 3 | 5 |
| `CityData::pop_cap` @00737d40 (summed by `Leader::calc_pop_cap`) | `village_pop` | ×2 | ×3 |
| `CityData::upgrades_to` @007364f0 | 415 (only for 414; any other non-416/531 type gives 416) | 416 | −1 |

`constants.village_pop` (+0x3e8) is **0** in the stock rules, so cities add nothing to the pop cap (economy-spec owns
pop cap; only the per-city term is stated here).

**Radius** (`LeaderData::get_radius(type)` @006db790, `CityData::get_radius` @00738410), in tcoords:

```
r = city_center_radius (+0x12c, 20) + (level(type) − 1) × city_center_pop_radius (+0x130, 4)
if has_tribe_bonus(21 Indians): r += indians_city_radius (+0x88c, 4)
r = min(r, 64);  CityData::get_radius applies min(·, 64) again
```
So 20 / 24 / 28 tcoords (Indians 24 / 28 / 32). **No tech changes it** (correction to world-spec §6.3 item 5 / §8
item 4, which call it tech dependent). `Wall::mask_me` @00642fc0 passes `CityData::get_radius(city)` when the
building has a city record, else `LeaderData::get_radius(type)` (an unfinished site, or the Chinese upgrade inside
`activate` before `init_city`). `Wall::mask_city` @0063e310 then sets/clears `TFlags.CITY 0x100` on the first
`even_circle_radius[r]` points around the centre tcoord (world-spec §6.3 item 5). CITY bits of all players share the
one bit.

### 5.2 City membership of buildings ✅

Each city owns a singly linked list: head = the city object (`city.o`), next = `Build.city_down` (+0x74), −1 ends.
Non-city buildings in it have `Build.city` (+0x72) = city index; the city object's own `Build.city` is its index too
(set in `activate`).

**Assignment at placement:** `Build::init` @00629740 (§3–4) ends with: city types → append `{o, who}` to
`UnbuiltCities[who]`; all other types → **`Build::find_city`** @00628180. Forts also go to `UnbuiltForts`, wonders to
`UnbuiltWonders`.

`Build::find_city()`:
```
if flags & OBJECT_CITY: return o
if city >= 0: remove_from_city()
c_o = BuildTypeData::get_town(type, tcoord(x), tcoord(y), who)       // a city OBJECT slot, or −1
if c_o < 0: return −1
add_to_city(objects[who][c_o].city); return c_o
```

`BuildTypeData::get_town(tx, ty, who)` @00639b90:
1. `cx = tx·192 + 96`, `cy = ty·192 + 96`; footprint origin `x0 = tcoord(cx') − xs/2` with `cx' = tcoord(cx)·192 + 96`
   if `xs` (+0x234) is odd, else `tcoord(cx)·192` (same for y with `ys` +0x238); i.e. the snapped footprint.
2. For every footprint tcoord (x outer, y inner) that is on the map: if `TFlags & CITY 0x100` is clear → **return −1**
   (the whole in-map footprint must lie inside some city radius; off-map tiles are ignored).
3. If the type lacks `'e'` (0x10) and has `'l'` (0x800, legacy, unused by stock data): `ObjectsData::find_town_at`
   (as below but only cities that `is(TOWN)`); else `ObjectsData::find_city_at(cx, cy, who, type, 0)`.
4. Return `cities[who][idx].o` or −1.

`ObjectsData::find_city_at(x, y, who, type, skip_check)` @0065b870: `find_dist = 99999999`; −1 if leader `who` is not
VALID or (x, y) off map. If `skip_check == 0`: `WorldData::is_city_at(tx, ty, who)` @006b3b80 must hold: CITY bit on
the tile **and** the tile's wcoord territory owner (`WData.who`) is −1, `who`, or an ally of `who`. Then for every
VALID own city in index order: `d = vector_dist(|tc(city.y) − tc(y)|, |tc(city.x) − tc(x)|)` (tcoords,
movement-spec §1); if `d ≤ CityData::get_radius(city)`: if the type has `'j'` (0x200, one per city) and
`CityData::count_buildings(city, type, 0, 0) ≠ 0`, `d += 100`; if `d ≤ find_dist`: take it (**ties go to the higher
index**). Returns the city index.

`Build::add_to_city(c)` @00622380: only if `cities[who][c]` is VALID: `city = c`; for non-city buildings: walk to the
list tail and append (`tail.city_down = o`, `city_down = −1`); if active and the type lacks `'e'`:
`Wall::increment_stats`; if active: TEMPLE → `city_flags |= HAS_TEMPLE`, LUMBERMILL → HAS_LUMBER, GRANARY →
HAS_GRANARY, MARKET → HAS_MARKET.

`Build::remove_from_city()` @00622030 (non-city, `city ≥ 0`): unlink (find predecessor from the head); `City::
regen_roads(city)`; for TEMPLE/LUMBERMILL/GRANARY/MARKET clear the matching flag if `count_buildings(city, t, 0, 1)`
is now 0; `city = −1`; if active and no `'e'`: `Wall::decrement_stats`.

`Wall::increment_stats` @00643270 / `decrement_stats` @00642da0 maintain `LeaderData.num_buildings[t−414]` (+0x555e)
and `reg_buildings` for active buildings (`num_queued` and the per-trainer counters for inactive ones). Consequence:
**an active non-`'e'` building counts in `num_buildings` only while it belongs to a city** (`Wall::activate`
@0063e4b0 increments when `'e'` or `city ≥ 0`; `Build::close` decrements under the same test).

**`City::find_buildings()`** @007384c0 (re-link and capture; called by `Build::activate` (founding), `check_upgrade`,
`capture_city`, `Leader::gain_tech` (civic epoch techs), `Build::finished`, scenario editor):
```
r = min(LeaderData::get_radius(city type), 64)
for w in 0..7 with leader VALID:                                      // index order
  for o in [obj_base[1], build_mark[w]) then [obj_base[2], wall_mark[w]):   // second range is empty in practice
    b = objects[w][o]
    if !b.valid (vslot 0x10) or b.flags & OBJECT_CITY or b.city >= 0: continue
    if w != city.who and b.type has 'e': continue
    d = octagonal distance in tcoords between city object and b (vector_dist formula)
    if d > r: continue
    if w == city.who:
        if b.is(LIBRARY): Build::new_library(b)
        b.add_to_city(city.city)
    else:                                                             // a foreign orphan inside the radius: captured
        n = b.swap_team(city.who)                                     // Build::swap_team @628210 (§3–4)
        if n < 0: b.die(0, −1, 0.0); return                           // NOTE: returns without check_upgrade
        objects[city.who][n].activate(0, 1, 0); b.close(0, −1, 0.0); objects[city.who][n].mask_me(1, REGEN_TOTAL)
check_upgrade()
```
Orphans (own non-city buildings with `city = −1`) arise when a city dies (§5.8); they are re-linked by the next
`find_buildings` of a city that covers them (a new city, a capture, or **any civic epoch tech**, §5.4).

`City::regen_roads` @00738aa0: sets `build_masks |= 0x100` on every building of the list (incl. the city);
`Build::process` (§8.2 step 16) then calls `BuildType::place_roads(…, 1, REGEN_FORCE)` on the next frame with
`(frame + o) % 16 == 0` (world-spec §6.3 item 6; `PathFinder::find_road` is ❓ there).

### 5.3 Founding a city ✅

1. BUILD command → `Group::action_build` @707510 (§3–4) refuses a city type other than FORBIDDENCITY when
   `get_city_limit() ≤ get_total_cities()` (§5.5); `Objects::init_build` creates the unstarted site, `Build::init`
   adds it to `UnbuiltCities`.
2. The first builder starts the site (`Wall::start` → `mask_me(1, REGEN_FORCE)`): **the CITY radius bits are set when
   construction starts**, with `LeaderData::get_radius(type)` (world-spec §6.2). Buildings placed inside meanwhile get
   `city = −1` (`find_city_at` needs a VALID city record) and are linked at completion by `find_buildings`.
3. Completion: `Build::activate(loading, notify, count)` @623e20 (§3–4 owns the generic part). City parts, in order:

```
if flags & OBJECT_CITY:
    city_num += 1                                                     // net 0, undone below
    if has_tribe_bonus(14 Chinese) and !loading and C.chinese_large_cities (+0x788) and !is(FORBIDDENCITY):
        set_type(TOWN, SET_TYPE_NORMAL) (vslot 0x84); mask_me(1, REGEN_NONE); leader_flags |= CALC_WALL_STATS
    city_num −= 1
if !loading and count: last_building_finished[type] = o (LeaderData +0x6070)
Wall::activate(loading, notify, 0)                                    // ACTIVE, increment_stats, update_seen …
if flags & OBJECT_CITY:                                               // ---- founding branch ----
    UnbuiltCities::remove_unbuilt_city(o, who)
    if editor (semaphore byte1 & 8): first = (city_num == 0)
    else:                            first = (city_num == 0 and !(leader_flags2 & LEADER_NO_CITY_DEFEAT 1))
    second_cap = 0
    if !first and has_tribe_bonus(23 Persians):
        find_capital(&c, &w, −1, −1)
        if c >= 0 and w == who and find_capital(&c2, &w2, c, w) gives c2 < 0:   // exactly one own capital
            second_cap = !(leader_flags2 & 1)
    city.idx = Cities::init_city(who, o, loading, first)               // → City::init (below); Build.city = idx
    City::find_buildings(city)                                         // links own buildings, captures orphans, check_upgrade
    if count: cities_built++
    if !loading: city.race = who; city.founder = who                   // (+ achievement event, P)
    city_num++; game.world_cities++; city_mine++
    if has_tribe_bonus(9 Spanish): leader_flags |= LEADER_EXPLORE_MAP (0x1000)
    if first: city_flags |= CAPITAL | ORIGINAL_CAPITAL                 // (already set by City::init)
    elif second_cap: city_flags |= CAPITAL
    Leader::calc_pop_cap; message (P)
else: GRANARY/LUMBERMILL/SMELTER/REFINERY/TEMPLE/MARKET/SENATE special cases (§5.9 Senate; temple → §5.10)
if city >= 0: City::regen_roads(city)
if is_fort: Build.fort = Forts::init_fort(who, o); remove_unbuilt_fort     // §5.11
elif is_dock: Docks::init_dock
… (gather/bonus/free units: §3–4) …
if is_wonder: §5.13
leader_flags |= CALC_WALL_STATS; … update_hits(0); update_los();
if city >= 0 and !(flags & OBJECT_CITY): City::check_upgrade(city)    // every completed building can upgrade its city
update_seen(0)
```
`loading = 1` is passed by `capture_city` and scenario placement, 0 by construction (frame-spec §8.1 `activate(0,1,1)`).

**`City::init(idx, who, o, loading, capital)`** @00737050:
- `city = idx`, `o`, `who`, `pop = 1`; `reg` = `WData.region` at the object's wcoord; `x, y` = object centre.
- `scouted/in_port = 0`; `city_flags = VALID`; `free = busy = gatherers = 0`; `plundered = 0, ocean = 0, land = 9,
  filled = 1`; `ocean_filled = dock_tile = 0`; `trade_val = 0`; `bordering = 0`; `was_capital_flags = 0`;
  `traded_with[8] = 0`; `attack/raid/capture/assimilation/reduce` stamps `= loading ? frame : 0`; `conquest_node = −1`;
  `peasant_dist = 0`; `capture_strength = 0`; enhancer bytes 0; `race = 0`, `founder = −1`; `space`, `ter` zeroed.
- `LeaderData.pop += pop_value`, `game.world_pop += pop_value`; if `reg < 64`: `reg_pop[reg] += pop_value`,
  `reg_cities[reg] += 1`. `Leader::calc_pop_cap`.
- **All 64 land regions: `borders = 0`** (inline `Region::fix_borders`, territory recompute §5.10).
- `capital` → `city_flags |= CAPITAL | ORIGINAL_CAPITAL`.
- `!loading` → `generate_name()`, else the name is cleared (the loader / `City::capture` sets it).
- `vans`: count 0, capacity grown to 10 (all −1).
- `City::fix_world_vals()` @00735aa0: `rw = min((r + 3)/4, 61)` wcoords (r = `LeaderData::get_radius`); for
  `k in [0, circle_radius[rw + 3])` over `circle_x/y` (combat-spec §2.17.1) around the city wcoord, in-map:
  `WData.val (+0xc) >>= 2` if `k < circle_radius[rw]`, else `>>= 1` (AI site value; part of the checksummed WData).

**`City::generate_name`** @00735c90 (no RNG): Forbidden City → name = type name, no counter change. CTW → conquest
name. Otherwise the counter update (LeaderData, checksummed): among leaders 0..7 with the owner's `tribe` (+0xc), pick
`best` = the first such index, replaced by any later one with a strictly larger `city_name`; target = `best` if that
maximum is > 0, else the owner; `leaders[target].city_name++`. If the owner's `city_name` is still 0 in the
"first names" branch (n < 2 or old owner counter 0) it is set to 1. The chosen string (tribe `<CITIES>` list, a
deterministic LFSR `x odd → x ^= 0x170; x >>= 1` seeded by `(tribe + world+0x7c) % span + 1` when several players share
a tribe, or "<tribe> City N") is file-only. 🟡 the string selection is only needed for display.

### 5.4 City sizes and upgrades ✅

Small → Large → Major City is **automatic** (no queue, no cost, no build time): `City::check_upgrade()` @00738b20.
The FROM chain makes TOWN queueable at a Small City in principle (`BuildTypeData::queue_here`: building `where` =
FROM), and `BuildData::can_make` returns 3 ("not enough buildings") for a TOWN-chain type when `can_upgrade` fails
(§6), but `check_upgrade` fires as soon as the same conditions hold, so the queue path is not normally reached.

```
check_upgrade():
  if !ready_to_upgrade(): return
  old = get_pop_value(); t = upgrades_to()
  city object.set_type(t, SET_TYPE_NORMAL)            // vslot 0x80 → Wall::set_type: decrement_stats, SubObject::set_type, increment_stats
  city object.mask_me(1, REGEN_NONE)                  // CITY bits for the larger radius (bits only added)
  leader_flags |= CALC_WALL_STATS
  find_buildings()                                    // links buildings now in range; ends in check_upgrade (recursion)
  pop += new − old; world_pop += new − old; reg_pop[region at city wcoord] += new − old (if < 64)
  Leader::calc_pop_cap; Region::fix_borders (all regions, §5.10)
  console owner: message + S_CITY_UPGRADED (P)
```
Because `find_buildings` re-enters `check_upgrade`, one call can go VILLAGE → TOWN → METROPOLIS in the same frame.

`CityData::ready_to_upgrade` @00736480: false for 416/531; `T = upgrades_to`; requires
`LeaderData::type_avail(T, 1)` (§7: Large City PREQ0 "Medieval Age", Major City "Industrial Age") and
`can_upgrade(T)`. `CityData::can_upgrade(T)` @007383c0: `T == 414` → 1; `T == TOWN` → `enough_kinds(city_buildings
(+0x14c, 5) + 1)`; else `enough_kinds(metro_buildings (+0x150, 9) + 1)`. (`num_kinds_needed` @00736640 and
`LeaderData::get_city_upgrade_level` @006d6d90 are the UI mirrors.)

`CityData::enough_kinds(n)` @00736540: `n = min(n, 24)`; walk the city list **from the city object itself**; count
distinct type indices among objects that are VALID, `is_build` and ACTIVE; return 1 as soon as the count reaches
`n`. The city counts as one kind, so 6 kinds = the city + 5 different building types (10 = city + 9). `num_kinds`
@007366c0 is the same without the cap/early exit.

Triggers of `check_upgrade`: end of `Build::activate` for a non-city building with `city ≥ 0`; end of
`find_buildings`; `Leader::gain_tech(t)` for every building type b with `has_preq(b)` and some `get_preq(b, k, who) ==
t` where `b.is(TOWN)` → `check_upgrade` on every VALID own city (index order). Civic epoch techs (cat 1, 565..571)
additionally run, per VALID own city in index order, `Wall::mask_city(city, add=1, CityData::get_radius)` then
`find_buildings` (→ `check_upgrade`), before the type loop.

Chinese (14): with `chinese_large_cities`, a freshly built city becomes TOWN in `activate` (above), and
`City::assimilate` calls `set_type(TOWN, NORMAL)` on the city object **unconditionally** — a Major City or the
Forbidden City captured by the Chinese is downgraded to TOWN on assimilation, with no `mask_me`, `CALC_WALL_STATS` or
pop update (🟡 quirk read from the decompile; worth a replay check).

### 5.5 City limit ✅

`LeaderData::get_city_limit` @006d6130: `civic = epoch[1] ^ 0x63187`; if Bantu (3) and `civic > 0`: `civic +=
bantu_city_limit` (+0x5a4, 1); `limit = civic + 1`; `if has_wonder(PYRAMIDS) ≠ 0: limit += pyramids_city_limit`
(+0x42c, 1). `LeaderData::get_total_cities` @006d6060: `city_mine + num_queued[414] − (has_wonder(FORBIDDENCITY) ≠ 0)`
(the loop over FROM-replaced types is dead: it is guarded by `types[414]->is_unit_type()`). `at_city_limit` @006e0d30
= `limit ≤ total` (UI); `Group::action_build` makes the same test (§5.3). The Forbidden City is exempt when placed and
does not count afterwards.

### 5.6 Per-frame city processing (`Build::process` §8.2 step 15, city object with `city ≥ 0`) ✅

In this order (frame-spec §10):
1. `(frame + o) % 200 == 0`: if `ATTACKING` clear it, else clear `UNDER_ATTACK` (two-step decay); `plundered--` if
   non-zero.
2. CAPITAL and `info.team_style == 2`: for leaders 0..7 VALID|ACTIVE: first whose `LeaderData::get_target() == who`
   and whose bit is not yet in `ever_seen_completed` (+0x63): set the bit, `update_local_seen` (vslot 0x164), stop.
3. (P) lone-city warning for the console player.
4. `(frame + o) % 64 == 0`: if `check_capture_eligible`: `u = find_unit(x, y, SEARCH_ENEMY, who, 0xc00, 0,
   FILTER_DOMAIN …)`; if found: `if check_capture(u, objects.find_who) return` (§5.7). If the owner is not HUMAN, no
   enemy was found, `WallData::can_capture` and `damage > hits/2`, `!UNDER_ATTACK` and `get_diff() > 1`: a not-busy own
   PEASANTS (50) within 0x1e00 gets `add_repair_order(o, who, QUEUE_NEW)` (AI).
5. Assimilation, if `race != who`: with THECITIZEN (356, `has_general(0, 0x164)`) in range,
   `assimilation_timer += 1 − thecitizen_assimilation_speed` (+0xc9c, 4); `e = frame − assimilation_timer`; Turks (8):
   `e = (turk_assimilate (+0x664, 200) + 100)·e/100`; if `who == founder`: `e = (reassimilation (+0xd2c, 300) + 100)·e
   /100`; if `assimilation_timer (+0xd30, 2000) ≤ e`: `City::assimilate()`.
6. If `!UNDER_ATTACK`: if `damage ≠ 0`, `race == who` and `(frame + o) % city_heal_rate (+0xc10, 4) == 0`:
   `repair_damage(level, 0, 1)` (vslot 0x168). Then if `ALARM`: period 60 (AI) or 200 (human with
   `leader_options.flags & 0x20`; other humans skip); if `(frame + 100 + o) % period == 0` and no seen non-friendly
   unit within `radius·192`: temporary group `action_alarm` (all clear).
7. Step 8 roads (§5.2).

**`City::assimilate()`** @00738e90: clear `GOT_CAPTURED`; if `race != who`: `race = who`; if the city wcoord's region
`< 64`: `Region::fix_borders` (§5.10); Chinese quirk (§5.4); `leader_flags |= RECOMPUTE`; for each building after the
city in the list: active LIBRARY → `new_library` with ACTIVE cleared around the call; active SENATE → government hero
respawn as in `activate` (§3–4/C); message (P). Also called by `Leader::gain_tech` (bonus at bonustypes+0xae4: all
unassimilated own cities), `Leader::defeat_by`, scripts.

### 5.7 Capture ✅ (see capture-spec.md)

The full specification (decompile + asm) is [capture-spec.md](capture-spec.md) §2-§5; the summary below is kept for
orientation and corrected where it was wrong (strength values, the non-city branch, units).

Entry: `Build::check_capture(unit_o, unit_who)` @006276a0, from `Object::valid_target` (combat-spec §2.9.2), the
64-frame poll above, and group attack code. Summary:
- Requires `check_capture_eligible` (@62d1d0, combat-spec §2.9.2) and no garrison (`inside_down < 0`; otherwise sets
  `build_masks |= 0x4000` = eject); attacker not the owner/ally of an ACTIVE owner; the unit a land unit
  (`domain 0`), `is_unit`, not `unit_masks & 1`, `attack() ≠ 0`, type lacks `unit_flags & 0x8000000`; and
  `frame − city.capture_stamp > 74`.
- Strength scan: radius `R = city_capture_radius (+0x134, 10)` tcoords for cities, `unit_respond_range (+0x18, 12)`
  otherwise; over `circle_radius[(R·192 + 0x2ff)/0x300]` wcoords, same region as the building, every listed object
  that is VALID, not air/sea, not a `unit_masks & 1` unit, passes `Search::valid_filter(…, 8)`, within `R·192`: value
  `get_capture_value()` (`UnitData::get_capture_value` @610a70: 0 for siege or `unit_masks & 1`, else
  `min(uber_size (+0x308), guy_mark)` ≥ 1; buildings 1); **every** building of the owner counts `num_inside + 6`
  (`+12` for forts) on top of its 1 and gives the attacker's bit in `visible` (+0x40, then `update_local_seen`);
  `Search::valid_filter(…, 8)` is FILTER_NOT_UNDER_ATTACK (buildings with `build_masks & 0x20` do not count). Attacker side (`unit_who`, its allies at war with the
  owner) vs defender side (owner, its allies at war with the attacker); the defender starts with
  `capture_strength` (if the last capture was < 900 frames ago) else 2; the capturing unit's own value seeds the
  attacker. Capture when `defender < attacker`.
- City: the new owner is the attacking-side leader with the largest summed value (ties: lower index); if the
  original `founder` or `race` is an ally of that leader and ACTIVE, the city goes to them instead. Then
  `Cities::capture_city(new, idx, old)` and `capture_strength` of the new record = the unit's value.
- Non-city capturable building (no stock type: only cities are capturable): `swap_team`, new `activate(0,1,0)`, old
  `close(0,−1,0)`, new `mask_me(1, REGEN_TOTAL)`, return 1; only when `swap_team` fails does the old object die
  (`Object::die(0,−1,0)`, return 0).

**`Cities::capture_city(new_who, idx, old_who)`** @00733380 (🟡 summary of the sim parts, in order):
1. Diplomacy clear-all if pending; Persian second-capital bookkeeping; `old.cities_lost++`, `new.cities_captured++`;
   both `leader_flags |= RECOMPUTE`.
2. List A (to dispose) starts with the city object; `n = city.swap_team(new_who)`; if `n ≥ 0`:
   `objects[new][n].activate(1, 1, 0)` — `loading = 1`, so `init_city` stamps every timer with `frame` (the
   assimilation clock starts now) and the name is left empty — plunder base `city_plunder_per_level (+0x304, 100) ×
   (level − 1)`.
3. If the swap failed: all non-`'e'` buildings of the old list go to list A (+25 plunder each). Else
   `City::capture(new record, old record, new idx, new_who, n)` (below), `Armies::update_city` (AI); then every
   building of the old list without `'n'` (0x2000): to list A; Lakota (19) captor skips farms/granaries; active ones:
   `swap_team(new_who)`, `activate(1, 1, 0)` on the copy, to list B, +25 plunder. `City::find_buildings(new)`.
4. Only when old and new owner are the same player or mutual allies: units of the old owner around the city (with more
   than one old city: inside the radius and with this city as their nearest friendly one) are `swap_team`ed (scripted /
   ally transfers). An enemy capture moves, ejects or disbands no unit.
5. Plunder (🟡): Russians (13) `russian_plunder_steal`; capital `capital_plunder` (+0x308, 500) or, in team styles 1/2/9,
   `capital_plunder_assassin × eliminated players`; THEDESPOT `thedespot_plunder`; paid to the captor's poorest of
   goods 0..5 except knowledge (or to every good via `bucket_add` for capitals), stolen amounts from the victim.
   Messages (P).
6. List A: non-city entries: inactive → `Object::disband(o, 0)` (partial refund); active → `close(5, −1, 0)` (dt 5 =
   no DEAD_BUILD, no unmask), dt 0 for the Lakota farm/granary case. List B: `mask_me(1, REGEN_SIMPLE)`.
7. If the old record was CAPITAL and (no Forbidden City, or only a CTW one) and `find_capital` (skipping this city)
   finds none of the old owner's: `Leader::lost_capital(old, new)`. If the new record is CAPITAL and the new owner has
   no other capital: `Leader::recapture_capital(new)`.
8. `City::close(old record, …, attacker = new_who)` (§5.8) and shrink `city_mark`; the old city object: `city = −1`,
   `close(5, −1, 0)`. New city object: `update_hits(0)`, `damage = hits − 10`.

**`City::capture(old, idx, new_who, new_o)`** @00736c40 ✅ (copies into the freshly created record): `city, who, o,
x, y`; scenario `city_lost` bookkeeping; pop: subtract the new record's pop value, `pop = old.pop`, add it back
(leader and world); `attack_stamp = raid_stamp = frame`; `reg`, `scouted`, `founder`, `plundered`,
`was_capital_flags`, `traded_with[8]`, `conquest_node` copied; `race = new_who` if old and new owner are the same,
mutually allied, the captor has `LEADER_ASSIMILATE_IMMED (0x200000)` or `has_preq(GLOBAL_GOVERNMENT_BONUS 697)`,
else `race = old.race` (stays unassimilated); `city_flags = old.flags & 0xf13f` (drops ALARM and the four HAS_*
flags, re-derived by `find_buildings`/`add_to_city`) `| GOT_CAPTURED` unless same/mutual allies; name and id copied.
If CAPITAL: `was_capital_flags |= 1 << old_who`, ORIGINAL_CAPITAL → also WAS_ORIG_CAPITAL; unless (old owner Persian
and ORIGINAL_CAPITAL) every **inactive** SENATE of the old owner is `Object::disband(o, 1)`. Then clear CAPITAL and
ORIGINAL_CAPITAL; if `was_capital_flags` has the new owner's bit (recapturing one's own capital): set CAPITAL, clear
the bit, and if WAS_ORIG_CAPITAL and `new_who == founder` → flags `(& 0x3fff) | 0x4010`; `race = new_who`. Finally
`damage = hits() − 10` on the city object.

### 5.8 Destruction and closing ✅

`Build::close(dt, …)` @00628980 for a city object (flags had OBJECT_CITY and VALID): … `Wall::close` (which, if
started and `dt ≠ 5`, runs `mask_me(0, REGEN_SIMPLE)` → **clears the CITY bits of the whole radius, including tiles
also covered by neighbouring cities**), restore OBJECT_CITY in `flags`, then: if not active →
`remove_unbuilt_city`; if `city ≥ 0` → `Cities::close_city(who, city, −1)`; `Region::fix_borders`; for every VALID
leader 0..7 (guard `wall_mark[w] ≠ 0`, always true) every valid, active city object `mask_me(1, REGEN_NONE)`
(restores overlapping CITY bits); `Leader::calc_pop_cap`. Then the fort / dock / temple / wonder branches (§5.10,
5.11, 5.13) and `leader_flags |= CALC_WALL_STATS`.

**`City::close(…, attacker)`** @00737550:
- If the city object has OBJECT_CITY: `city_num--`, `world_cities--`, `city_mine--`, `lost_city_stamp = frame`.
- `pop −= pop_value`, `world_pop −= pop_value`; region of the stored x/y: `reg_pop −= pop_value`, `reg_cities −= 1`.
- Armies (AI): close own armies whose home is this city; clear other armies' target if it is this city.
- `city_flags &= ~VALID`; **`Leader::lost_a_city(was_capital = flags & CAPITAL, own = (race == who), attacker)`**.
- Walk the list from the city object: each member `city = −1`; LIBRARY with a research queue moves the queue to
  `get_first_library` or refunds it (`clean_queue(1)`); `next = city_down`; non-`'e'` → `decrement_stats`;
  `city_down = −1`; if the member is valid and not a city: **`Build::find_city()`**.
- `calc_pop_cap`; `o = −1`; strings closed; every caravan in `vans` with a live unit gets `Unit::repath`; `vans`
  freed.

Ordering quirk (🟡, from the decompile; check with a replay): because `Wall::close` cleared the CITY bits before
`City::close` re-homes the members and the remask runs after it, a building inside the dying city's radius stays
orphaned even if another own city also covers it (`get_town` sees no CITY bit). It is re-linked by the next
`find_buildings` that reaches it.

**`Leader::lost_a_city(was_capital, own, attacker)`** @006dc6d0 (✅ asm):
```
if semaphore byte1 & 2, or (editor and turn_control.semaphore[0] & 1): return
(music, P)
if was_capital and attacker >= 0 and info.elimination == 1 (CAPITAL) and !has_wonder(FORBIDDENCITY):
    for w in 0..7 VALID|ACTIVE: if (w == attacker or attacker.is_ally(w)) and w.has_preq(697 GLOBAL_GOVERNMENT_BONUS):
        defeat(1 DEFEAT_CAPITAL, attacker, 1); return
switch info.elimination:
  2 (CAPITAL_SUDDEN): if was_capital: unless Persian with another own capital (find_capital → c >= 0, w == self):
                      defeat(2, attacker, 0); else fall to default
  3 (SUDDEN):         if own: defeat(3, attacker, 0)
  default:            if !(leader_flags2 & NO_CITY_DEFEAT) and city_num == 0: defeat(0 CONQUEST, attacker, 0)
(music, P)
```
`Leader::defeat` / `defeat_by` hand the remaining cities to the conqueror (or to their founder / race leader) via
`capture_city`, destroy unfinished sites and military trainers outside the conqueror's cities, keep forts / towers /
lookouts with the loser and give the other buildings to the conqueror; `Leader::blow_up_towers` @006d2310 and
`blow_up_units` only run in multiplayer. Specified in [capture-spec.md](capture-spec.md) §6.

### 5.9 Capital rules ✅

- The first city founded is the capital (`CAPITAL | ORIGINAL_CAPITAL`), unless `LEADER_NO_CITY_DEFEAT` outside the
  editor. Persians (23) get a second `CAPITAL` on the next city founded while they own exactly one capital.
- `LeaderData::find_capital(&c, &w, skip_c, skip_w)` @006eb930: own cities in index order, first VALID with CAPITAL
  that is not `(skip_c, self)` → `(c, self)`. Else, **if this leader is VALID|ACTIVE**, other leaders 0..7 in order,
  their VALID cities with `was_capital_flags` bit `self` (our captured capital) not equal to `(skip_c, skip_w)` →
  `(c, w)`. Else `(−1, self)`. `holds_capital` = found `w == self`; `has_capital` = true unless elimination is CAPITAL,
  else `holds_capital`.
- Senate moves the capital: in `activate`, a completed SENATE (exact type 438) in a city with `race == who`, when
  `find_capital` gives an own capital city whose list has no SENATE (`count_buildings(SENATE, 0, 1) == 0`): clear
  CAPITAL from every VALID own city (Persians: only from cities without ORIGINAL_CAPITAL), set CAPITAL on the Senate's
  city, `founder = who`, `Region::fix_borders` (if the Senate's wcoord has a region), notice (P). `senates_built++`.
- `Leader::lost_capital(who_new)` @006dc870: if `lost_capital_timer == 0`: set it to 1; `T = get_team_terr()`,
  `E = leaders[who_new].get_team_terr()`, `a = world+0x78 / num_nations` (0 if none); `mod = (T + a == 0 or E + a ==
  0) ? 256 : clamp(256(T + a)/(E + a), 128, 512)`; if both `city_num ≠ 0` and `E's city_num < own city_num`:
  `mod = clamp(mod·(own + 2)/(theirs + 2), 128, 768)`; `lost_capital_modifier = mod`; `lost_capital_stamp =
  (stamp << 8)/mod + frame` (the stored remaining time becomes an absolute deadline). Messages (P).
- `Leader::recapture_capital()` @006d1ba0: if the timer flag is set: clear it, `stamp = trunc256((stamp − frame) ×
  mod)` (back to remaining time). Completing the **Forbidden City** does the same (`activate`: flag cleared,
  `stamp = trunc256((stamp − frame) × mod)`), i.e. it pauses the clock; `Leader::lost_a_city` ignores the
  GLOBAL_GOVERNMENT instant defeat while it stands, and destroying an active Forbidden City while the capital is held
  by someone else re-runs `lost_capital` + `lost_a_city(1, 1, holder)` (sudden death: `lost_a_city(0, 1, w)` for every
  city of leader w founded by the owner). While `lost_capital_timer == 0` the stamp is the negative countdown stepped by
  `Leaders::process_all` (frame-spec §4 step 5). The retake window `Game::retake_capital(p)` @00594530 =
  `trunc256(mod × max(1, (70/2 + xs × retake_capital (+0xd04, 3600)) / 70))` (CTW: ×2 with only a CTW Forbidden City,
  /2 when the CTW opponent has one) is checked by `Leader::process_elimination` (victory domain).

### 5.10 Territory triggers and influence ✅

Territory (`WData.who/who2`, `LeaderData.reg_terr`, `territory`) is **not** recomputed inside `activate`/`close`.
Every trigger only resets the per-region cursor `Region.borders` (+0x2c) to 0 for **all 64 land regions**
(`Region::fix_borders` @00680f60 and `Regions::fix_all_borders` @0067f7d0 ignore `this`; several callers inline the
loop). The work happens in `GameDaemon::check_borders` @00732060 (frame step 11, before `Objects::process_all`, so a
building completed in frame N first affects territory in frame N+1):

```
check_borders():
  this.borders = 0                                       // per-frame budget counter
  any = 0
  for r in 0..63: if region[r].size != 0 and region[r].borders < size:
      any |= World::compute_reg_territory(r, 0); busy = max(busy, 1)
  if any & 2:                                            // some region finished this frame
      if !(any & 1): fog.flags |= 2 (P), busy = max(busy, 1)
      if every region with size != 0 is finished:
          for each VALID leader: territory = Σ reg_terr[r] over regions with size != 0; territory_high = max(…)
```
`World::compute_reg_territory(r, full)` @006b0bb0 (decoded below; C++ `engine/sim/territory.cpp`) returns 0 if the
region was already finished on entry, **1 when the frame budget is used up (`GameDaemon.borders` > 255 — at most 256
wcoords per frame over all regions)**, 2 when the region finished in this call. `World::compute_all_territory`
@006b5700 sets the budget to −1 (unlimited) and recomputes everything at setup (world-spec §9). **So a territory change
spreads over ⌈(Σ region sizes)/256⌉ frames** and must be reproduced with the same per-frame budget and region order
(0..63, wcoords in `Region.coords` order).

Triggers (all reset every land region):

| Event | Site |
|---|---|
| City founded / initialised (incl. capture copy) | `City::init` (inline loop) |
| City upgraded | `City::check_upgrade` |
| City destroyed / record closed | `Build::close` (city branch) |
| City assimilated | `City::assimilate` (only if the city wcoord region < 64) |
| Capital moved by a Senate | `Build::activate` |
| Temple completed in a city / temple closed | `Build::activate` (temple branch, region ≥ 0) / `Build::close` (inline) |
| Fort completed / closed | `Forts::init_fort` (inline, region ≥ 0) / `Forts::close_fort` |
| Colosseum, Eiffel Tower, Tikal completed / closed | `Build::activate` (inline) / `Build::close` |
| Tech: civic epoch, FORTBORDERS2..4 (721..723), TEMPLEBORDERS2..4 (712..714), Russians any age tech, CTW `ctw_wonder_min_civics` reached | `Leader::gain_tech` (`fix_all_borders` / `fix_borders`) |
| Spell assimilate; `Build::finished` (upgrade) | `SpellType::cast_assimilate`, `Build::finished` |

#### 5.10.1 `World::compute_reg_territory(r, full)` @006b0bb0 ✅

Every land wcoord goes to the source (city or fort) with the lowest influence **cost**; int32 C arithmetic throughout
(truncating division), no floats, no RNG. Leaders are the 8 player slots. Constants by name (rules.xml CONSTANTS).

```
if full: region.borders = 0
if region.borders == 0:                                   // a fresh pass over this region
    for each VALID leader p: reg_terr[r] = 0; reg_known_rares[r] = 0; bordering = 0 on each VALID city of p
    region.flags &= ~0x80
if region.borders >= region.size: return 0
if GameDaemon.borders > 255: return 1
(lim, lim_civic, lim_city) = region.flags & 4 (PLAYER) ? world.player_territory_limit{,_civic,_city}
                                                       : world.colonized_territory_limit{,_civic,_city}
// both triples are territory_limit_base / _civic / _city (World::init), so the choice never matters in stock play

per VALID leader p (every term 0 for an invalid slot):                  civic = LeaderDataEncrypt.epoch[1]
    temple_level = TEMPLEBORDERS4/3/2 (714/713/712) held ? 4/3/2 : 1
    temple_terr  = temple_upgrade_terr[temple_level - 1]          (read as fort_upgrade_terr[level + 3])
    TIKAL (532):                 temple_terr = (tikal_temple_hp + 100) * temple_terr / 100
    CtW and num_bonus_cards[32] (LeaderData +0x6916, missionaries):
                                 temple_terr = (ctw_missionaries_bonus + 100) * temple_terr / 100
    fl = FORTBORDERS4/3/2 (723/722/721) held ? 4/3/2 : 1
    fort_level = fl - 1;  fort_terr = fort_upgrade_terr[fl - 1]
    COLOSSEUM (530):             fort_terr += colosseum_fort_borders
    Romans (tribe bonus 6):      fort_terr += roman_fort_borders; fort_level = (roman_fort_borders + 1)/2 + fl - 1
    civic_terr = civic_upgrade_terr[civic]
    range = lim + civic * lim_civic;  bonus = 0;  ru = has_tribe_bonus(13 Russians)
    GEMS rare (rare or rare_conquest bit 23):  bonus = ru ? gems_territory_bonus/2 : gems_territory_bonus
                                               range = lim + civic * lim_civic + lim_civic
    COLOSSEUM:      v = colosseum_territory_bonus;     bonus += ru ? v/2 : v;  range += ((ru ? 1 : v) + 1)/2 * lim_civic
    EIFFEL (540):   v = eiffel_tower_territory_bonus;  same two lines
    Russians:       bonus += russian_borders_per_age * civic + russian_borders
                    range += ((russian_borders != 0) + civic) * lim_civic
    bonus += (h + 15)/25,  h = LeaderData::get_handicap @6da740 for a HUMAN in multiplayer (semaphore byte0 & 4)
                           (= the lobby handicap's rules category data[0]), else 0
    fort_range = fort_level * lim_city + range

base = territory_base << 8
while region.borders < region.size:
    if GameDaemon.borders >= 0: GameDaemon.borders++, and stop if it was >= 256 before the increment
    (wx, wy) = region.coords[region.borders++];  centre tcoord (cx, cy) = (4wx + 2, 4wy + 2)
    best = second = 999999999;  best_who = second_who = -1;  best_city = -1
    for j in 0..7: who = (wx + j) mod 8;  skip unless VALID
        for c in [0, city_mark(who)), VALID cities:
            d = oct(|cx - tcoord(city.x)|, |cy - tcoord(city.y)|)
            level = 1 / 2 (TOWN 415) / 3 (METROPOLIS 416, FORBIDDEN CITY 531), from the city object's type
            skip unless d <= (level-1)*lim_city + (HAS_TEMPLE ? temple_level[who]*lim_city : 0) + range[who]
            d = shrink(d);  q = city.race (the bonuses are the race's: an unassimilated city uses its old owner's)
            s = civic_terr[q] + (HAS_TEMPLE ? temple_terr[q] : 0)
              + (ORIGINAL_CAPITAL or the object is(531) ? capital_territory_bonus
                                                        : city_level_territory_bonus[level-1]) + bonus[q]
            cost = territory_den * d * 256 / (s * city_territory_multiplier + territory_num)
            tag = (q == who) ? who : -2                       // -2: an unassimilated city claims for nobody
            if cost > base or cost >= best: if cost < second: second = cost, second_who = tag
            else: if best_who != -1: second = best, second_who = best_who
                  best = cost, best_who = tag, best_city = c
        for f in [0, fort_mark(who)), VALID forts (position = the fort object's x, y):
            d = oct(...); skip unless d <= fort_range[who];  d = shrink(d)
            s = fort_terr[who] + civic_terr[who] + (object is(534 Red Fort) ? 4 : 0) + bonus[who]
            cost = territory_den * d * 256 / (s * fort_territory_multiplier + territory_num)
            if cost > base or cost >= best: if cost < second: second = cost, second_who = who
            else: second = best, second_who = best_who (even when -1), best = cost, best_who = who, best_city = -1
    WData.who = best_who; WData.who2 = second_who                 (as i8; -2 is stored as is)
    if best_who >= 0:
        if best_city >= 0 and second_who >= 0 and second_who != best_who
           and (diplos[best_who][second_who] == 0 or diplos[second_who][best_who] == 0):   // war either way
            city = cities[best_who][best_city]; city.bordering |= bit(best_who) | bit(second_who)
            if |wy - wcoord(city.y)| + |wx - wcoord(city.x)| < 5: city.city_flags |= VERY_NEAR_BORDER (0x1000)
        reg_terr[best_who][r]++;  region.flags |= 0x80
        the first live good (index order < good_mark) whose wcoord is (wx, wy), if any:
            OIL (5): append its index to L[best_who].oil_patches unless present
            else if best_who > 7 or reveal_map == 3 or REVEAL_ENEMY (0x800) or a FOUCHE is alive
                    or seen2[(2wy+1)·fog_xs + 2wx+1] & ally_mask[best_who]:
                Leader::new_rare(best_who, good); good.ever_seen (+0x20) |= bit(best_who)
if region.borders >= region.size:
    every VALID leader: flags |= RECOMPUTE (0x2000000);  region.flags |= 0x80 | 0x20
return (region.borders >= region.size) + 1
```
`oct(a, b)` (unsigned): with M = max, m = min (ties take the second branch): `M == 0 → 0`, `m < 60000 → m²/(2M) + M`,
else `(m + 2M) >> 1`. `shrink(d)`: `d < 13 → 2d/3`, then `d < 9 → 2d/3`, then `d < 5 → d/2`. `tcoord(c)` =
`div_3_table[c >> 6]`, `wcoord(c)` = `div_3_table[c >> 8]`. Presentation skipped by the port: the `Borders`
border-line cells (the old owner written before the leader loop, `Borders::save_last_who` @008835e0 after it) and on
completion `Borders::add_border_region` @00883780 / the border texture refresh (`Region` +0x30 holds that renderer
slot). `reg_known_rares` is only zeroed here. `Wall::mask_city` (CITY bits) is independent of territory.

#### 5.10.2 `World::compute_all_territory` @006b5700 ✅

```
for each VALID leader: reg_terr[0..63] = 0
GameDaemon.borders = -1
for r in 0..63 with size != 0: borders = 0; compute_reg_territory(r, 1)
fog.flags |= 2 (P)
for r in 64..127: every wcoord: who = who2 = -1
for each VALID leader: territory = Σ reg_terr[r] over land regions with size != 0; territory_high = max(…)
GameDaemon.borders = 0
```

### 5.11 Forts and towers ✅

Forts are buildings that `is(FORTX 443)` (Fort, Castle, Fortress, Redoubt; Red Fort 534 is a wonder FROM … check
rules: it is not in the FORTX chain). `Forts::init_fort(who, o)` (from `activate`): slot allocation §5.0.3, `Fort::init`
@0073eac0 (`fort`, `o`, `who`, VALID, `forts_built++`, `reg` = region at the fort's wcoord, `reg_forts[reg]++` if < 64),
then all land regions `borders = 0` if the fort's region ≥ 0. `Forts::close_fort` @0073ec90 (from `Build::close`):
`Region::fix_borders` if region ≥ 0, `Fort::close` (`reg_forts--` if the object is still valid and reg < 64; record
invalidated), shrink `fort_mark`. `Forts::find_fort(wx, wy, who)` @0073ebe0 linear search by wcoord. Forts are the only
non-city territory sources. **Towers (439 chain) have no territory, no record and no special city handling** (their
LOS/range come from `LeaderData::get_tower_fort_los/_range`, fog/combat specs). `Leader::found_cities` @006c7a60,
`Leader::produce_city` @006cb120 and `Leader::check_orphaned_buildings` @006c9f20 (skips humans: `flags & 0xc == 4`)
are AI-only (sim-affecting through the commands they issue; to be specified with the AI).

### 5.12 Walls ✅ (none)

Rise of Nations has no player-built walls. `Wall` is the base class of every building (`SubObject → Object → Wall →
Build`); "wall stats" are building stats. Evidence: no BUILDING in buildingrules.xml carries the legacy `'k'` (must be
built on a wall) flag or a wall name; `Objects::init_wall` @00658c50 (= `init_build(…, preplaced = 1)`) has **no
callers**; the 3000.. "wall" object range is allocated by nothing (frame-spec §8.1); BUILD's `x2/y2` are unused
(orders-spec §2.7). `Leader::calc_wall_stats` @006cf7c0 (run from `Leaders::process_all` when `LEADER_CALC_WALL_STATS`
is set — set by every `activate`, `close`, `check_upgrade`, the Chinese upgrade, …): for every own building in
`[2000, build_mark)` then `[3000, wall_mark)` that is VALID: if not active `Wall::update_construct_time` (@0063d560,
§2.3.3), then `update_hits(0)` and `update_los()`. `WallData::tile_corner` @00643440 (footprint corner,
world-spec §6.1), `covers_tile` @006439b0 (tile inside `[corner, corner + xs/ys)`), `valid_buildmask` @0063e2a0
(mask 0x40: building `can_infinite`; 0x80: can carry air) and `Wall::kill_at_tile` @0063d440 (from
`kill_competing_buildings`: while a STARTED2 tile holds another unstarted foundation, `Object::disband(it, 1)` —
**no `game_random` for buildings**, see §5.14) are generic building helpers.

### 5.13 Wonders ✅

#### 5.13.1 Registry
`activate` for `is_wonder` (526..542): (console music, P) `remove_unbuilt_wonder(o, who)`; `wonders_built++`;
`Build.wonder = Wonders::init_wonder(who, o)`: slot allocation §5.0.3; record `wonder`, `who`, `stamp = frame`, `o`,
VALID, `timer = Wonder::get_timer()`; **`Game.wonders[type − 526] |= 1`**; `wonders_held = max(wonders_held, live
wonder count)`. `Wonder::init` @0073c5e0 (load path) does the same without the counters. `Wonders::close_wonder`
(from `Build::close` when `wonder ≥ 0`): invalidate, shrink `wonder_mark`; `Build::close` then sets
`CALC_UNIT_STATS`, re-runs `calc_pop_cap` for COLOSSUS, `fix_borders` for COLOSSEUM/EIFFEL/TIKAL, `wonder = −1`; an
unfinished wonder only leaves `UnbuiltWonders`. SPACEPROGRAM closing clears `LEADER_REVEAL_ENEMY` (unless
`has_preq(REVEAL_ENEMY_BONUS)` or `num_units` at +0x59e4 ≠ 0) and calls `GameDaemon::update_all_seen`.

#### 5.13.2 One wonder of each type
- Several players may build the same wonder at once. **When one completes**, `activate` walks leaders 0..7 (VALID) and
  their buildings `[2000, build_mark)` in order and `Object::disband(b, 1)` every VALID, **not active** building of
  exactly the same type index other than itself (own duplicates included): full cost refund, no RNG (§5.14); notice
  to the console player (P).
- From then on `LeaderData::type_avail` (§7) refuses the type for everyone: `BuildTypeData::already_built`
  @0063ce10 = `Game.wonders[t − 526]`, which is never cleared — a destroyed wonder cannot be rebuilt.
- Placement (§3–4, `blocked_location` → BLOCKED_WONDER 42): a city may hold at most 1 wonder (2 for Egyptians, tribe
  7) counting finished and unfinished wonders in its list except REDFORT and city-flagged ones
  (`CityData::num_wonders(1)` @007382b0); REDFORT is exempt from the check.

#### 5.13.3 Value, ownership tests, timers
- `LeaderData::has_wonder(t)` @006ebc10: 0 unless t is a wonder type; bit 2 if CTW `conquest_wonders` has `t − 526`
  and civic epoch ≥ `ctw_wonder_min_civics`; bit 1 if a live own Wonder record's object has type t **and belongs to a
  city (`Build.city ≥ 0`) — except REDFORT, which counts anywhere**. Callers mostly test `≠ 0`. A wonder orphaned by its
  city's death (§5.8) stops working until re-linked.
- `WonderData::get_value` @0073c7a0 / `LeaderData::get_wonder_value` @006ebb90: Σ over live own wonders of the type's
  wonder value (BuildType vslot 0x118, `ObjectTypeData::get_wonder_value` @00661ba0, from `WONDER_VAL` +0x2d0 —
  not re-decoded here). `get_wonders` @006db680 = live count; `get_team_wonders` @006da2d0 = Σ over self and mutual
  allies; `get_wonder_net` @006ebb10 = `max(0, team value − max team value of any VALID non-mutual-ally)`.
- `Wonder::get_timer` @0073c660 = `wt + (542 − type) × wa`, with `wt = wonder_timer (+0xd08, 4500) ≥ 1 ? max(1, (35
  + xs × 4500)/70) : 0` and `wa` likewise from `wonder_age` (+0xd0c, 0) (70 = map size "Standard", `map_sizes[3].data
  [0]`). The record's `timer` is saved; ❓ no sim consumer found. The wonder victory uses `Game::wonder_timer`
  @005944f0 (= `wt`) in `GameDaemon::process_victory` @00730ef0 with `Game::wonder_winning` @005948a0 (leader with the
  highest `get_wonder_net` ≥ `wonderwins[info.wonderwin].data[0]`, ties resolved by allies/value) and LeaderData
  `wonderwin_stamp/_timer` (+0x448/+0x44c); victory domain, 🟡.
- `Leader::compute_wonder_score` @006bc260 is a stub-sized helper (score); `LeaderData::get_*unbuilt_wonder*` feed the
  wonder cost ramp (§5.13.4).

#### 5.13.4 Wonder effects on production

Applied at completion in `Build::activate` (in this order, after the duplicate disband):

| Wonder | Effect at completion | Constant |
|---|---|---|
| TERRACOTTA 529 | `recharging = terra_cotta_free × 15` | +0x464 (30) |
| KREMLIN 538 | `recharging = loading ? kremlin_spy : 1`; if no own SPY (58) with `unit_masks & 0x400` exists: `leader_flags |= KREMLIN_SPY` | +0x514 (30) |
| FORBIDDENCITY 531 | pause the capital-loss clock (§5.9) | |
| HANGINGGARDENS 528 | if `hanging_gardens_upgrades` (0 = off): for techs 609..611, 620..622, 612..614 (in that order) with `has_preq`, `type_eligible(t,1)` and not owned: unqueue (refund) t from every own active building whose type `is(tech.where)` and `count_queue(1, t) ≠ 0`, then `gain_tech(t, …, 1)` | +0x450 |
| COLOSSUS 527 | `calc_pop_cap` | |
| COLOSSEUM 530 | if `colosseum_fort_upgrades` (0): gain techs FORTIFICATION..0x253 that are eligible; territory reset | +0x47c |
| COLOSSEUM, EIFFEL 540, TIKAL 532 | all land regions `borders = 0` (if the wonder's wcoord has a region) | |
| STATUEOFLIBERTY 537 | if `liberty_free_upgrades` (1): for unit types 50..401 with `has_preq`, (`domain == 0` or `where == AIRBASE 447`), not owned, `type_eligible(t, 0)`: `gain_tech(t)` | +0x534 |
| REDFORT 534 (or CTW conquest wonder bit 14) | `red_fort_fortification` (1): gain FORTIFICATION..0x253; `red_fort_tactics` (1): gain END_FINALTYPES..0x249 | +0x4bc/+0x4c0 |
| TIKAL 532 | `tikal_temple_upgrades` (0): gain RELIGION..0x24c | +0x4a8 |
| SPACEPROGRAM 542 | `leader_flags |= LEADER_REVEAL_ENEMY` (fog) | |
| every wonder | `leader_flags |= CALC_UNIT_STATS` | |

Continuous effects (each via `has_wonder(t) ≠ 0` unless noted; formulas are truncating int32 in the stated order,
owned by the functions named):

| Wonder | Production effect | Function | Constant |
|---|---|---|---|
| PYRAMIDS 526 | city limit +1; city build cost `×(100 − 33)/100` (after the Bantu `bantu_city_cost` discount) | `get_city_limit`; `TypeData::get_cost` @00664090 | `pyramids_city_limit` +0x42c, `pyramids_city_discount` +0x434 |
| COLOSSUS 527 | pop cap `+ colossus_pop_cap` after the final clamp | `Leader::calc_pop_cap` @006dc490 | +0x43c (50) |
| HANGINGGARDENS 528 | non-wonder buildings in the same city as an own Hanging Gardens: `time = (100 − hgbt)·time/100` per such wonder (`has_wonder & 2 == 0` required); enhancer upgrade cost `×(100 − 66)/100` | `BuildData::construct_time` @0062d5c0; `get_cost` | `hanging_gardens_build_time` +0x44c (0), `_industry_discount` +0x454 |
| TERRACOTTA 529 | free unit: every frame `recharging--`; at < 1: `recharging = free×15 + (delay == 7 ? (barracks_units × 15)/2 : barracks_units × delay)`, then if `control < pop_cap` train `current_upgrade(SLINGERS or the tribe's replacement)` and `terra_cotta_total++`; unit cost `×(100 − terra_cotta_cost)/100` | `Build::process` §8.2 step 13 (exact type 529); `get_cost` | +0x464, +0x468 (7), +0x460 (0), LeaderData +0x9f8 `barracks_units` |
| COLOSSEUM 530 | fort and tower (non-wonder) cost `×(100 − 20)/100` (before the Roman fort discount); barracks cost `colosseum_barracks_cost` (0) | `get_cost` | +0x480, +0x484 |
| FORBIDDENCITY 531 | not counted by the city limit; level-3 city; capital clock | §5.5, §5.9 | |
| TIKAL 532 | temple cost `×(100 − tikal_temple_cost)/100` (0) | `get_cost` | +0x4a4 |
| PORCELAIN 533 | ship (`domain 1`) train time `×100/(porcelain_ships + 100)` | `ObjectData::train_time` @006508c0 | +0x4ac (50) |
| ANGKORWAT 535 | research time `×100/(angkor_research + 100)` (0); ship cost `angkor_ships` (25) | `train_time`; `get_cost` | +0x4cc, +0x4d8 |
| VERSAILLES 536 | building time `×100/(versailles_building_speed + 100)` (0) on unfinished buildings; knowledge research cost `versailles_knowledge_costs` (50) | `Wall::update_construct_time`; `get_cost` | +0x4e4, +0x4dc |
| STATUEOFLIBERTY 537 | land unit (`domain 0`) train time `×100/(liberty_units + 100)` (0); bombers/missiles/AA cost | `train_time`; `get_cost` | +0x520, +0x52c/+0x530/+0x538 |
| KREMLIN 538 | free spy: when `(frame + o) % 15 == 0` and `KREMLIN_SPY` set: `recharging--`; at < 1 clear the flag, `recharging = kremlin_spy`, `train(SPY)`; the spy gets `unit_masks2 |= 0x400` and is pop-free (`control −= type+0x2f0` unless `unit_masks & 1`) | `Build::process` §8.2 step 10 (exact type 538) | +0x514 |
| SPACEPROGRAM 542 | air train time `×100/(space_air_speed + 100)` (100); air cost `space_air_cost` (50) | `train_time`; `get_cost` | +0x564, +0x568 |
| any wonder | Egyptians (7) `×(100 − egyptian_wonders)/100` after Marble (rare good 20) `×(100 − marble_wonder_cost)/100`; support ramp counts own wonders + own unbuilt wonder sites (+ team counts when teams are locked; +n−3 above 3, +n−6 above 6; Supercollider/Space Program variants), support halved for wonders | `get_cost` (🟡 ramp) | +0x63c (25), +0x910 (10) |
| Americans (20) | a wonder is instant (`construct_time = 1`) while the player holds no wonder, has `wonders_built == 0`, the type is not 541/542 and no other player has an unfinished site of it | `construct_time` | |

City cost ramp (for the cost owner): for city types `get_cost` uses `num_buildings[414..416] + num_queued[414..416]`
as the support multiplier.

### 5.14 RNG draws

| Site | Condition | Draw |
|---|---|---|
| — | City founding, `City::init`, upgrades, radius, membership, `assimilate`, `generate_name`, limit, wonders, forts, territory triggers | **none** |
| indirect | `City::find_buildings` capturing a foreign orphan, `capture_city` swaps, `check_capture` | through `Build::swap_team` / `Unit::swap_team` (new objects: `Build::init → find_gather_tiles`, unit `Guy::init_real`, `eject_contents`), see §3–4 / frame-spec §8 |
| indirect | free units (Terra Cotta, Kremlin, Senate hero) | `Build::train` → `init_unit` → `Guy::init_real` |
| none | `Object::disband(b, 1)` on a **building** (wonder-race losers, senates of a captured capital, `kill_at_tile`) | `disband` draws `rnd(3)` only for non-buildings (`is_build` vslot 0x20 false); a started building gets `die(1)`, an unstarted one `close(0)` |

Iteration-order dependencies: leaders 0..7 and slot order in `find_buildings`, the wonder-duplicate disband, `defeat`
hand-over; city index order in `find_capital`, `find_city_at` (ties → higher index), `check_upgrade` loops in
`gain_tech`; region order 0..63 and the 256-wcoord budget in `check_borders`; city list order (insertion = tail
append, removal = unlink) in `enough_kinds`, `City::close` re-homing.

### 5.15 Rules / data read

Type fields: index (+4), FROM (+0x3c), `where` (+0x40), `build_flags` (+0x2c0: 'e' 0x10, 'j' 0x200, 'l' 0x800,
'n' 0x2000, 'c' 4), `x_size/y_size` (+0x234/+0x238), unit `domain` (+0x218), `attack` (+0x1e8), unit +0x308
(capture value cap), +0x2f0 (pop cost), wonder value (+0x2d0 via vslot 0x118). All in `engine/sim/rules.hpp`
(`Type`: from, where, build_flags, wonder_val, …). Constants (all below 0xd40, so in `rules.hpp` `Constants.v`):
0x018 unit_respond_range, 0x0b8 fort_upgrade_terr[4], 0x0c8 temple_upgrade_terr[5], 0x0dc civic_upgrade_terr[8],
0x0fc capital_territory_bonus, 0x100 city_level_territory_bonus[3], 0x10c/0x110 fort/city_territory_multiplier,
0x114 territory_base, 0x124/0x128 territory_den/num, 0x12c city_center_radius, 0x130 city_center_pop_radius,
0x134 city_capture_radius, 0x14c city_buildings, 0x150 metro_buildings, 0x304 city_plunder_per_level,
0x308/0x30c capital_plunder(_assassin), 0x3bc capital_build_time, 0x3e8 village_pop, 0x42c..0x434 pyramids_*,
0x43c colossus_pop_cap, 0x44c/0x450/0x454 hanging_gardens_*, 0x460/0x464/0x468 terra_cotta_*, 0x46c..0x484
colosseum_*, 0x498/0x4a0/0x4a4/0x4a8 tikal_*, 0x4ac porcelain_ships, 0x4bc/0x4c0 red_fort_*, 0x4cc/0x4d8 angkor_*,
0x4dc/0x4e4 versailles_*, 0x514 kremlin_spy, 0x520..0x538 liberty_*, 0x53c eiffel_tower_territory_bonus,
0x564/0x568 space_air_*, 0x5a4 bantu_city_limit, 0x5b8 bantu_city_cost, 0x604 roman_fort_borders, 0x63c
egyptian_wonders, 0x664 turk_assimilate, 0x760/0x764 russian_borders(_per_age), 0x788 chinese_large_cities, 0x88c
indians_city_radius, 0x910 marble_wonder_cost, 0x930 gems_territory_bonus, 0xc10 city_heal_rate, 0xc9c
thecitizen_assimilation_speed, 0xd04 retake_capital, 0xd08 wonder_timer, 0xd0c wonder_age, 0xd2c reassimilation,
0xd30 assimilation_timer. Categories: `map_sizes[3].data[0]` (70), `wonderwins[i].data[0]`, `pop_limits`.

### 5.16 Side effects on other systems

- Fog/LOS: `activate` → `update_los`, `update_seen`; capital reveal in team style 2 (§5.6 step 2); SPACEPROGRAM
  `LEADER_REVEAL_ENEMY`; `check_capture` sets `visible` bits; `GameDaemon::update_all_seen` on Space Program loss.
- Territory: §5.10 (deferred, budgeted). Placement: CITY bits (§5.1) gate `blocked_site` code 31 and `get_town`.
- Pop: `LeaderData.pop/world_pop/reg_pop` (pop value), `calc_pop_cap` after every city change.
- Stats flags for next frame: `RECOMPUTE`, `CALC_WALL_STATS`, `CALC_UNIT_STATS` (`Leaders::process_all`).
- WData `val` (AI) via `fix_world_vals`; roads via `regen_roads`.

### 5.17 Corrections to existing specs

1. world-spec §6.3 item 5 / §8 item 4: the city radius is **not tech dependent**; formula §5.1. CITY bits are set at
   construction start with the site's level-1 radius.
2. frame-spec §8.1 ("competing buildings on the footprint are disbanded (**RNG**)") and §10 notes on disbanding
   buildings: `Object::disband` on a building draws no `game_random` (only units: `die(rnd(3)+1)`).
3. frame-spec §8.1 / §6: walls do not exist; `Objects::init_wall` is never called (confirms the 3000 range is empty).
4. scenario-objects-spec: city flag bit 2 (`0x4000`) is `CITY_ORIGINAL_CAPITAL` (PDB enum; `FILE_CITY_ORIG_CAPITAL 2`),
   which drives the capital territory bonus and Persian rules (answers its open question 6 in part).
5. Territory is recomputed incrementally (256 wcoords per frame, from `GameDaemon::check_borders`), not at the event.

### 5.18 Open questions

1. ~~`World::compute_reg_territory` influence algorithm~~: decoded, §5.10.1.
2. 🟡 `Cities::capture_city` plunder amounts, Russian steal, ally unit conversion; `Build::check_capture` strength
   loop details (`Search::valid_filter` arguments, the 900-frame branch) — transcribe from the decompile before
   porting capture.
3. 🟡 Chinese assimilation downgrade of Major City / Forbidden City (§5.4) and the orphaning order in `Build::close`
   (§5.8): verify with replays.
4. ❓ Consumer of `Wonder.timer` (saved; nothing in the sim read it in the code examined).
5. 🟡 `compute_reg_territory` reads `tikal_temple_hp` (+0x4a0) for the Tikal temple-border bonus (decompiler field
   name; confirm the offset in the asm).
6. 🟡 GameDaemon `busy` and `fog.flags |= 2` in `check_borders` look presentation-only; `busy` is part of GameDaemon
   state (saved?) — check state-spec.
7. Vslots 0x34/0x38 of TypeData used by `gain_tech` (age tech / epoch tech) were inferred from usage.

### 5.19 Evidence

| Function | Address | Used for |
|---|---|---|
| `Cities::init_city`, `City::init`, `City::fix_world_vals`, `City::generate_name`, `City::set_name` | 007352c0, 00737050, 00735aa0, 00735c90, 00735be0 | founding, record init, naming |
| `Build::activate` | 00623e20 (asm `prod_c2_asm/`) | founding branch, Chinese, Senate capital, temple, fort, wonder completion, check_upgrade call |
| `Build::init` | 00629740 | unbuilt lists, `find_city` |
| `Build::find_city`, `add_to_city`, `remove_from_city` | 00628180, 00622380, 00622030 | membership |
| `BuildTypeData::get_town`, `ObjectsData::find_city_at`, `find_town_at`, `WorldData::is_city_at` | 00639b90, 0065b870, 00658190, 006b3b80 | city lookup |
| `City::find_buildings`, `City::regen_roads` | 007384c0 (asm), 00738aa0 | re-link, capture orphans, roads |
| `City::check_upgrade`, `CityData::ready_to_upgrade / can_upgrade / upgrades_to / enough_kinds / num_kinds / num_kinds_needed / get_level / get_pop_value / pop_cap / get_radius` | 00738b20, 00736480 / 007383c0 / 007364f0 / 00736540 / 007366c0 / 00736640 / 00739340 / 00738450 / 00737d40 / 00738410 | sizes, upgrades |
| `LeaderData::get_radius / get_city_limit / get_total_cities / at_city_limit / get_city_upgrade_level / get_building_cities` | 006db790 / 006d6130 / 006d6060 / 006e0d30 / 006d6d90 / 006e06f0 | radius, limit |
| `Wall::mask_me`, `Wall::mask_city`, `Wall::activate`, `Wall::close`, `Wall::set_type`, `Wall::increment_stats / decrement_stats` | 00642fc0, 0063e310, 0063e4b0, 0063f6e0, 004723b0/00640da0, 00643270 / 00642da0 | CITY bits, stats |
| `Build::process` | 0061edf0 | city block, Kremlin, Terra Cotta |
| `Build::check_capture`, `BuildData::check_capture_eligible`, `UnitData::get_capture_value`, `ObjectData::health_level` | 006276a0, 0062d1d0, 00610a70, 006534b0 | capture trigger |
| `Cities::capture_city`, `City::capture`, `City::assimilate` | 00733380, 00736c40, 00738e90 | capture, assimilation |
| `Build::close`, `City::close`, `Cities::close_city` | 00628980, 00737550 (asm), 00733310 | destruction |
| `Leader::lost_a_city`, `lost_capital`, `recapture_capital`, `LeaderData::find_capital / has_capital / holds_capital`, `Game::retake_capital` | 006dc6d0 (asm), 006dc870, 006d1ba0, 006eb930 / 006d54c0 / 006d54f0, 00594530 | capital rules |
| `Region::fix_borders`, `Regions::fix_all_borders`, `GameDaemon::check_borders`, `GameDaemon::process_all`, `World::compute_all_territory`, `World::compute_reg_territory` | 00680f60, 0067f7d0, 00732060, 00732700, 006b5700, 006b0bb0 | territory trigger, budget, inputs |
| `Forts::init_fort`, `Fort::init`, `Forts::close_fort`, `Fort::close`, `Forts::find_fort` | 0073ed70, 0073eac0, 0073ec90, 0073ea50, 0073ebe0 | forts |
| `Wonders::init_wonder`, `Wonder::init`, `Wonder::get_timer`, `Wonders::close_wonder`, `WonderData::get_value`, `Game::wonder_timer`, `Game::wonder_winning`, `GameDaemon::process_victory` | 0073c860, 0073c5e0, 0073c660, 0073c7e0, 0073c7a0, 005944f0, 005948a0, 00730ef0 | wonders, timer |
| `LeaderData::has_wonder / get_wonder_value / get_wonder_net / get_wonders / get_team_wonders / has_unbuilt_wonder`, `BuildTypeData::already_built`, `CityData::num_wonders`, `BuildTypeData::blocked_location` | 006ebc10 / 006ebb90 / 006ebb10 / 006db680 / 006da2d0 / 006da470, 0063ce10, 007382b0, 006375b0 | wonder rules |
| `BuildData::construct_time`, `Wall::update_construct_time`, `ObjectData::train_time`, `TypeData::get_cost`, `Leader::calc_pop_cap` | 0062d5c0, 0063d560, 006508c0, 00664090, 006dc490 | wonder production effects |
| `Leader::gain_tech` | 006dcb60 (asm) | civic re-mask, check_upgrade loop, border techs, assimilation bonus |
| `Leader::calc_wall_stats`, `Objects::init_wall`, `WallData::tile_corner / covers_tile / valid_buildmask`, `Wall::kill_at_tile` | 006cf7c0, 00658c50, 00643440 / 006439b0 / 0063e2a0, 0063d440 | "walls" |
| `Object::disband` | 006455c0 | refund, RNG only for units |
| `Group::action_build` | 00707510 | city limit at placement |
| `Leader::found_cities`, `produce_city`, `check_orphaned_buildings`, `blow_up_towers`, `defeat_by` | 006c7a60, 006cb120, 006c9f20, 006d2310, 006d1c80 | AI / victory (names only) |

## 6. Training: queues and spawning

Status: ✅ the production queue (data, QUEUE_UP, per-frame `do_queue`, completion, unqueue, repeat, rally points,
`train` and `come_out`) is specified from the decompiles **and** the disassembly (argument lists of every virtual call,
`find_nearby_spot` arguments and output slots, the RNG sites). 🟡 marks branches read from the decompile only (airbase /
helicopter rally re-issuing, Build::close library hand-over, AI `upgrade_units`). Costs and times are §2's
(`TypeData::get_cost` @664090 via `pay_cost`, `ObjectData::train_time` @6508c0 / `TypeData::time` / `research_time`);
what `Leader::gain_tech` @6dcb60 does with a finished research item is §7's (only its queue side-effects are
summarised in §6.9).

Decompiles: `re/decomp/orders/` (Build::*), `re/decomp/bld2/`, `re/decomp/cmds/`, `re/decomp/prod_C/` (new, jobs
`re/jobs/dc_prod_C_1…9.txt`), disassembly `re/decomp/prod_C_asm/` (`do_queue`, `queue_up`, `finished`, `train`,
`come_out`, `unqueue`, `clean_queue`, `action_unqueue`, `action_queue_up`, `add_gather_point`, `can_make`,
`Unit::find_nearby_spot`, `action_spell`), callers `re/decomp/prod_C/callers*.txt`.

Conventions as combat-spec §0. Additional ones used here:
- `o`, `who` = the building's slot and owner. `B.is(t, strict)` = Object vslot +0xb8 (devirtualised to
  `ptype->is(t, strict)` = ObjectTypeData::is, Type vslot +0x60). `L` = `leaders[who]` (LeaderData offsets).
- **Trainable** unit type `t`: `is_unit_type(t)` (50 ≤ t < 414) **and** `L.tech` bit `t` set (bitmask bytes at
  LeaderData +0x6c18, i.e. `LeaderData.tech` +0x6c0c + 0xc header). A unit type whose tech bit is **clear** is an
  *upgrade research item* (e.g. queuing PHALANX at a barracks while it is not yet available researches the Phalanx
  line): it uses `accel_research`, completes through `gain_tech`, and `type_avail` reports 2 for it.
- Type fields by TypeData offset (rules.hpp names): +0x04 `type`, +0x30 `preq[0]`, +0x3c `from`, +0x40 `where`
  (building type that makes it), +0x44 `upgrade`, +0x48 `jump`; ObjectTypeData +0x1e4 `obj_masks`, +0x1e8 `attack`,
  +0x218 `domain` (0 land, 1 sea, 2 air), +0x234/+0x238 `x_size/y_size` (tiles), +0x240 `block_radius`, +0x244
  `big_radius`; UnitTypeData +0x2b4 `unit_flags`, +0x2b8 `unit_flags2`, +0x2c8 `role`, +0x2e8 `job_extra_time`, +0x2f0
  `control_cost` (pop cost), +0x304 `squad_size`, +0x308 `uber_size`, +0x30c `crew_size`; BuildTypeData +0x2c0
  `build_flags`. All are loaded by `engine/sim/rules.hpp` (Type struct) already.
- Constants (all in `rules_constants_table.inc`): `unit_train_distance` 0x84 (3/2 tile = 288), `unit_train_max_distance`
  0x88 (5/2 tile = 480), `boat_train_distance` 0x8c (288), `boat_train_max_distance` 0x90 (8 tiles = 1536),
  `unit_disembark_distance` 0x9c (3 tiles = 576), `max_aircraft_per_carrier` 0xa4 (7), `max_aircraft_per_airbase` 0xa8
  (10), `accel_train` 0x228, `accel_construct` 0x22c, `accel_research` 0x230 (all 100 = "1/1 speed"),
  `tech_science_discount` 0x364. `*GameAccess::ai_speed` (int; multiplies the accel when > 1).
- Type ids used: PEASANTS 50, PEASANTSKOREAN 51, SCHOLARS 52, SCHOLARSKOREAN 53, SPY 58, CARA 59, MERCHANT 61,
  MERCHANTDUTCH 62, FIGHTERBOMBER 308, HELICOPTER 310, NUCLEARMISSILE 315, TRANSPORTBARGE 320, BARK 323, CARRIER 351,
  FURTRAPPER 400, VILLAGE 414, TOWN 415, WOODCUTTER 418, MINE 419, UNIVERSITY 420, OILPLATFORM 422, BARRACKS 427,
  STABLE 428, SIEGEFACTORY 430, DOCK 432, LIBRARY 435, SENATE 438, AIRBASE 447, MISSILESILO 520, TERRACOTTA 529,
  KREMLIN 538, DEPOPULATE 646, BUILDAIR (spell) 661, DISBAND 666.
- `unit_masks` (UnitData +0x68, PDB enum UnitMask): 1 DECOY, 0x40000 AI_CONTROL, 0x200000 REPEAT_AIR, 0x4000000
  MULTIMOVE. `build_masks` (WallData +0x60): **0x40 = infinite (repeat) queue**, 0x80 = repeat-air, 0x8 airbase flag.

### 6.1 Data

#### 6.1.1 `QueueItem` / `BuildQueue` ✅

`QueueItem` (0x14 bytes, PDB): `+0 int job_counter` (progress), `+4 short type`, `+6 short good[3]`, `+0xc short
cost[3]`, +0x12..0x13 padding (not walked). `BuildQueue` (0x10, `BuildQueueData` base): `+0 int queue_size`,
`+4 QueueItem* queue` (+8 vbtable). The queue is a **fixed array of `queue_size` slots**; the used prefix is
`BuildData.queued` (+0x82, uchar).

- `BuildQueue::init(n)` @6307d0: `close()`; `queue_size = n` (only 2, 10, 20 are supported: three free-lists in
  `BuildQueueData::mem_lists`, memory pooling is irrelevant to the simulation); **memset the whole array to 0**, then
  `queue[0].type = −1` (0xffff), `queue[0].job_counter = 0`.
- `close()` @6306c0: returns the array to its pool, `queue_size = 0, queue = null`.
- `set_queue(i, type, int cost6[6] or null, keep_jc)` @6309f0 (i < queue_size): `queue[i].type = type`; `keep_jc == 0`
  → `queue[i].job_counter = 0`; if `cost6` non-null: walk goods g = 0..5, and for each `cost6[g] ≠ 0` store
  `good[k] = g, cost[k] = cost6[g]`, k = 0,1,2 (**at most 3 goods; a 4th non-zero cost is dropped**); fill the unused
  `k..2` with `good = −1, cost = 0`. (null `cost6`: goods/costs untouched — used by gain_tech's relabel, §6.9.)
- `set_job_counter(i, v)` @630ab0, `get_queue(i)` @630670 / `get_job_counter(i)` @630690 (plain reads).
- `un_queue(i, n)` @630ad0: `memcpy(&queue[i], &queue[i+1], (n − i − 1)·0x14)` (n = `queued` before decrement). **The
  vacated slot `n−1` is not cleared** (it keeps a stale copy of the item that moved to `n−2`).
- `walk_data` @6305f0 (docs/state-walk.md "BuildQueue::walk_data"): tag, raw `queue_size`; on load `init(queue_size)`
  (or `close` if 0); then raw bytes [0, 0x12) of **every one of the `queue_size` slots, used or not**. Consequence for
  checksums/saves: the stale tails left by `un_queue`, `init`'s zeroes and `queue[0].type = −1` are all part of the
  persistent state — a port must use exactly the same array semantics (fixed array, memmove, no clearing).

#### 6.1.2 Fields on the building and the leader ✅

| Field | Where | Meaning |
|---|---|---|
| `queued` | BuildData +0x82 (u8) | number of used slots |
| `build_queue` | BuildData +0x88 | the BuildQueue above |
| `stance` | BuildData +0x7e (i8) | stance inherited by trained units (§6.4.3) |
| `gather_max` | BuildData +0x80 (i8) | max workers inside (university scholars, §6.4.3) |
| `gather` | BuildData +0xb8 `GatherPointList` | rally points (§6.6) |
| `build_masks` | WallData +0x60 | bit 0x40 infinite queue, 0x80 repeat-air, 0x8 airbase |
| `BuildData::queue_fail` | static @cab3a4 | last QueueFailIndex (0 SUCCESS, 1 CANT_TRAIN, 2 FULL, 3 NEUTRALIZED, 4 UNASSIMILATED, 5 COST, 6 POP, 7 BUILDING_FULL); walked (state-walk "build_queue_fail") |
| `num_queued[806]` | LeaderData +0x5a22 (u16) | items queued per type, **all buildings of the leader** (there is no per-building count) |
| `barracks_queued, stable_queued, factory_queued, combat_queued, dock_queued, air_queued` | LeaderData +0xa10, +0xa14, +0xa18, +0xa1c, +0xa20, +0xa24 | queued military units by `where` (§6.10) |
| `ages_queued`, `epochs_queued` | LeaderData +0x67f4, +0x67f5 (u8) | queued age / epoch items |
| `tech_frame`, `tech_cat_frame[4]` | LeaderData +0x7c4, +0x7c8 | frame of the last research queued (overall / per category) |
| `pop_cap`, `pop_issues`, `control`, `active` | LeaderData +0x7e4, +0x7e8, +0x940, +0x93c | pop cap (economy-spec), frames blocked by the cap, pop in use, unit count |
| `num_units[352]` | LeaderData +0x5762 (u16, index t − 50) | live units per type (track_unit_type, §6.10) |
| `last_unit_finished[352]` | LeaderData +0x6274 (index t − 50) | last trained `o` per type (written by train) |
| `ScenarioData::last_unit_built_o[who]` | @cc1930 | last trained `o` |
| `num_queued`, `queue_time` | UnitData +0xa0 (i16), +0x64 | carrier aircraft queue (§6.7) |

#### 6.1.3 Capacity: `Build::init` @629740 ✅

`queued = 0` (the 16-bit store `0x00ff` at +0x81 sets `attack_whom = −1, queued = 0`), `clear_gather()`, then
`BuildQueue::init(n)` with: `n = 20` if `BuildTypeData::is_military_trainer` (`build_flags & 0x40000000` of the
`basic_type`) or `is_training_building` (`build_flags & 0x80000000` of the basic type); else `n = 10` if
`build_flags & 0x8000000` (the building's own type); else `n = 2`. `basic_type` @639970 follows `from` (+0x3c) down to
the root of the upgrade line (e.g. FORTRESS → FORT).

### 6.2 QUEUE_UP (op 24)

#### 6.2.1 `CommandPackage::process_queue_up` @948230 ✅

9-byte command `{i32 type, i32 num}` (replay-spec table). Logging only, then `if group ≥ 0:
groups[group].action_queue_up(type, num)`. No uid/ownership checks.

#### 6.2.2 `Group::action_queue_up(t, num)` @6fdbb0 ✅

```
prelude (orders-spec §2.1: ignore_orders kills, then action_begin = vslot +0x14)
if !group.buildings (+0x49): return
copy list[0..num) to a local array S (shorts), in list order
exchange sort, for i = 0 .. num−2:                     // only rows whose ORIGINAL S[i] is valid_wall (+0xc) && is_active (+0x4c)
    for j = i+1 .. num−1:
        if building(S[j]).queued < building(S[i]).queued: swap S[i], S[j]   // strict <, so ties keep list order
action_begin()                                         // called a second time
if (is_unit_type(t) && L.tech bit t) || (is_build_type(t) && !(buildtypes[t].build_flags & 4)):
    repeat num times (rounds):                         // `num` items per building, interleaved
        for k in 0..num_members:                       // order S
            B = S[k]; skip unless B.is_valid_wall && B.is_active && B.is_build (+0x20)
            if B.is(MISSILESILO, 0) && is_unit_type(t) && !B.can_carry(t): skip   // ObjectData::can_carry @645e00
            B.queue_up(t, 1)                           // result ignored
else:                                                  // research (techs, unit-line upgrades, spells, flag-4 buildings)
    if L.researching(t, −1, 0, 0): console-only feedback (sound via SoundGlobal::random — presentation RNG) ; return
    for pass in {0, 1}:
        for k in S order: B = S[k]
            skip unless valid_wall && is_active && is_build && (pass == 1 || B.queued == 0)
            if B.queue_up(t, 1) == 0: return           // first success wins
```

Note the sort only uses `queued` (not queue progress) and is **not stable** in general (selection-style exchanges).

#### 6.2.3 `Build::queue_up(t, use_escrow)` @620f40 ✅ (returns 0 = queued, 1 = refused)

```
if B.is(LIBRARY, 0) && t != DISBAND:
    lib = L.get_first_library()                        // §6.2.6
    if lib < 0: return 1
    if lib != o: return library(lib).queue_up(t, use_escrow)   // all research goes to the first library
    from_library = 1
    (presentation: "final tech" queued message/sound to the other players)
    if is_age_type(t) && game.victory == 9:            // tech-victory rules
        if (L.ages + 1 == game.ending_technology) && !Game::war_allowed():
            Leader::gain_tech(t, −1, −1, 1, 1); return 0     // instant age, no queue entry
        (presentation messages otherwise)
if !types[t].can_pay_cost(who, −1, o, 1): queue_fail = QUEUE_COST; return 1     // vslot +0x84
if !B.could_queue(t): return 1                         // sets queue_fail (§6.2.4)
if is_tech_type(t) (544..628) || L.type_avail(t, 1) == 2: L.tech_frame = frame
if is_tech_type(t): L.tech_cat_frame[techtypes[t].+0x14] = frame
(presentation: unit graphics preload; console-only pop-limit / caravan-limit / aircraft-limit feedback)
if game.semaphore byte1 & 8 (editor / free):  cost6 = {0,0,0,0,0,0}
else: types[t].pay_cost(who, o, −1, use_escrow, cost6)           // Type::pay_cost @6681f0, vslot +0xd4
slot = queued; queued += 1
set_queue(slot, t, cost6, keep_jc = 0)                 // job_counter = 0
if queued == 1: queue[0].job_counter = 0               // redundant
if t ≥ 0:
    L.num_queued[t] += 1
    class_queued(+1) (§6.10) for military unit types (attack ≠ 0)
if is_age_type(t):   L.ages_queued += 1
if is_epoch_type(t): L.epochs_queued += 1
(from_library: GraphicPieces preloads for the next age — presentation)
return 0
```

`Type::pay_cost(who, o, −1, use_escrow, out[6])` @6681f0 (§2 owns the price): for each good g = 0..5 with
`type_avail(g, 1) ≠ 0`: `c = get_cost(g, who, o, −1, 0, 1, −1)`; `out[g] = c`; if `use_escrow == 0` and
`stock[g] − escrow[g] < c` → `escrow[g] = 0`; `stock[g] = max(0, stock[g] − c)` (stock XOR 0x8221 encrypted);
if `use_escrow` → `escrow[g] = max(0, escrow[g] − c)`. Player QUEUE_UP passes 1; the repeat re-queue (§6.3.3) passes
0. **Population is never checked at queue time** (only a console message).

#### 6.2.4 Gates ✅

`BuildData::could_queue(t)` @62da50:
```
queue_fail = QUEUE_SUCCESS
if B.can_make(t, 1) == 0 (vslot +0x198): queue_fail = QUEUE_CANT_TRAIN; return 0
if queued ≥ queue_size: queue_fail = QUEUE_FULL; return 0
if B.is(UNIVERSITY, 0) && types[t].type ∈ {52, 53}:
    if B.count_queue(QUEUE_COUNT_TYPE, SCHOLARS) + B.num_gatherers(0, 0) > 6: queue_fail = QUEUE_FULL; return 0
return 1
```
(scholar cap: queued scholars + scholars inside must be ≤ 6 **before** adding, so a 7th can be queued.)
`BuildData::can_queue(t)` @62da00 (UI): `can_pay_cost(who, −1, o, 1)` else QUEUE_COST, then `could_queue`.

`BuildData::can_make(t, p)` @62db10 (returns 0, or `a` = type_avail code, or 3):
```
if !B.is_active: return 0
if !(semaphore1 & 8) && t ∉ {DISBAND, DEPOPULATE}:
    if B.is_neutralized() (vslot +0x180) || B.is_unassimilated() @62d470: return 0
if !BuildTypeData::queue_here(B.type, t): return 0
a = L.type_avail(t, p); if a == 0: return 0
if is_tech_type(t):  return (L.has_tech(t) || L.researching(t, o, 0, 0)) ? 0 : a
if is_unit_type(t):
    if L.tech bit t && !(B.type.build_flags & 0x10):
        return (B.is_unassimilated() && a ≥ 2) ? 3 : a
    return a
if is_spell_type(t):
    if L.has_spell(t): return SpellTypeData::is_castable(o, who, 0) == 3 ? a : 0
    return B.is(spelltypes[t].where (+0x40), 0) ? a : 0
if is_build_type(t) && B.city ≥ 0 && types[t].is(TOWN, 0) && !CityData::can_upgrade(city, t): return 3
return a
```
`BuildTypeData::queue_here(bt, t)` @639d10: DISBAND → 1. If `preq[0] == −2` or `where < 0` → 0. Unit types:
`bt.is(where, 0)` → 1, else 1 if `types[where].upgrade ≥ 0` and `bt.is(that, 0)`, else 0. Building types:
`bt.is(where, 1)`; others `bt.is(where, 0)`.

`LeaderData::type_avail(t, p)` @6e33a0 (0 = no, 2 = needs research, 4 = yes; 1/3 come from `type_eligible`):
```
if !has_preq(t): return 0
e = type_eligible(t, p) (@6dbd10, §7); if e != 4: return e
if is_unit_type(t):
    if is_gov_hero_type(t) && (leader_flags2 & 0x1000): return 0
    if !(tech bit t): return 2                                    // upgrade research item
    if !types[t].is(MACHINEGUN 123, 0): return 4
    for base in {RIFLEMAN 100, INFANTRY 102, MECHINFANTRY 104}:   // machine guns need one of these lines
        g = tribe_can_type(types[base]) ? base : types[base].type_graft(who)
        if tech bit g: return 4
    return 0
if !is_build_type(t): return (is_gov_type(t) && (leader_flags2 & 0x800)) ? 0 : 4
if buildtypes[t] is a wonder type && already_built: return 0
if buildtypes[t].from ≥ 0 && (build_flags & 4) && !(tech bit t): return 2
if t == SENATE: capital / existing-senate checks (decompile @6e33a0; 🟡 detail) → 0 or 4
return 4
```
`LeaderData::tribe_can_type(type)` @6d9380/@6d9410: for unit types with `unit_flags & 0x1000000` → 0 when
`game.info.flags & 8` or the tribe's +0x58 word is set; otherwise `(tribe_mask (+0x10) >> tribe) & 1 ? 4 : 0`. When it
is 0 the caller uses `type->type_graft(who)` (Type vslot +0x68, the tribe's unique replacement).

`City::could_queue(t)` @737fa0: any building of the city's chain (`+0x74 city_down` links) whose `could_queue(t)` ≠ 0.
`CityData::depop_can_queue()` @738060: the city centre has no DEPOPULATE (646) item (`count_queue(TYPE, 646) == 0`).
Both are UI/AI gates only.

#### 6.2.5 `LeaderData::current_upgrade(t)` @6e3140 ✅ (which type is actually made)

Used by every producer of "a unit of line X" (carrier fighters, barges, gov heroes, AI): returns the most advanced
available member of t's line.
```
if t < 0: return t
loop:
    if is_unit_type(t) || is_build_type(t):
        n = types[t].jump (+0x48); if n ≥ 0: n = graft(n)
        if n ≥ 0 && has_preq(n) && (tech bit n): t = n; continue
    // fallback via `upgrade`
    n = types[t].upgrade (+0x44); if n < 0: return t
    n = graft(n); if n < 0: return t
    if type_avail(n, 1) < 3:                       // not (yet) available
        r = current_upgrade(n); return (r < 0 || r == n) ? t : r
    t = n
```
(`graft(x)` = `tribe_can_type(types[x]) ? x : types[x].type_graft(who)`; @6e1360 is a thunk.) A queued item's
`type` is fixed when queued; when the line is upgraded later, gain_tech relabels queued items (§6.9).

#### 6.2.6 Libraries ✅

`LeaderData::get_first_library()` @6db6c0: the lowest building `o` in [2000, mark) that is VALID, active, `city ≥ 0`,
not unassimilated and `is(LIBRARY, 0)`; −1 if none. All library research lives in that building's queue (queue_up,
unqueue, action_unqueue, get_queue redirect to it; other libraries keep `queued == 0` — an invariant: `clean_queue` on a
non-first library with items would loop forever). `LeaderData::get_building_cities()` @6e06f0 = number of the
leader's own cities (`city_flags & 1`, `race == who`) containing a LIBRARY = **the number of library items processed in
parallel** (§6.3.1). When the first library is destroyed, `Build::close` @628980 appends its items to the new first
library's queue (dropping a DISBAND head), or `clean_queue(1)` if none remains (🟡).

### 6.3 Per frame

#### 6.3.1 `Build::do_queue(slot)` @61e410 ✅ (Build vslot +0x1b4; called as `do_queue(0)` from `Build::process` §8.2 step 8, ACTIVE buildings only)

```
if queued == 0: return
t  = slot < queue_size ? queue[slot].type : −1
if B.is(LIBRARY, 0) && L.get_first_library() != o && t != DISBAND: return
if B.is(MISSILESILO, 1) && B.inside_down (+0x28) ≥ 0 && (tech bit t) && t != DISBAND: return   // silo holds a missile
tt = ObjectData::train_time(t)                       // §2
if is_build_type(t) || t == DISBAND:  acc = accel_construct
elif trainable(t):
    acc = accel_train
    if L.check_population(t):                        // pop_cap < control_cost(t) + control   (@6e1330)
        L.pop_issues += 1                            // EVERY frame the head is a capped trainable unit
        (console only: player flag 0x800 + pop_cap_frame, message every 220 frames — presentation, but the
         `game.info.player[].flags/pop_cap_frame` writes are local-only)
    elif caravan(t) (unit_flags2 & 8): if get_units(t,0) ≥ get_caravan_limit(0): console-only flag 0x400 / message
    elif B.is(AIRBASE,0) && !(unit_flags & 0x20): console-only "airbase full" message
else: acc = accel_research                           // techs, unit-line upgrades, spells
if ai_speed > 1: acc *= ai_speed
jc   = slot < queue_size ? queue[slot].job_counter : −1
eff  = (tt == 1) ? 1 : jc
done = eff ≥ tt                                      // tested BEFORE adding: completes the frame after reaching tt
nv   = min(eff + acc, tt)
(presentation: jc == 0 && t == NUCLEARMISSILE && !(L byte +0x6c3f & 8) && who != console → nuke warning)
if B.is(LIBRARY, 0):                                 // parallel research
    if slot+1 < L.get_building_cities() && slot+1 < queued: do_queue(slot+1)   // recursion FIRST (deeper items first)
    if done:
        if B.finished(t) (vslot +0x1b0) && (B.flags & VALID): B.unqueue(slot, 0)
        return
    queue[slot].job_counter = nv; return
if !done: queue[slot].job_counter = nv; return
queue[slot].job_counter = tt
trainable_t = trainable(t)
r = B.finished(t)                                    // §6.3.2: 1 done, 0 pop-capped, −1 limit
if r > 0:
    repeat = build_masks & 0x40
    B.unqueue(slot, 0)                               // may clear 0x40 if the queue became empty
    if trainable_t && repeat:
        build_masks &= ~0x40
        if B.queue_up(t, 0) == 0: build_masks |= 0x40  // re-queued at the END, cost paid again now (no escrow)
    return                                           // can't pay → item dropped AND repeat switched off
if slot != 0: return
i = BuildQueueData::get_next_non_unit(queued, who)   // first index ≥ 1 whose type is not trainable
if i > 0: do_queue(i); return
if queued < 2 || r ≥ 0: return                        // r == 0 (pop cap) stops here
i = B.is(AIRBASE, 0) ? get_next_helicopter(queued) : get_next_non_caravan(queued)
if i > 0: do_queue(i)
```
Helpers (@630b10/@630bb0/@630c20; each scans indices 1 .. min(n, queue_size)−1, returns the first match or −1):
`get_next_non_unit` — type not a unit type, or a unit type without its tech bit (`has_tech == 0`);
`get_next_helicopter` — `types[type].is(HELICOPTER, 0)`; `get_next_non_caravan` — `type ≠ CARA (59)`.

So: a pop-capped head keeps `job_counter == tt` and retries every frame while the **first** research-like item behind
it runs; a caravan/aircraft-limited head lets the first research item run, else the first non-caravan (or, at an
airbase, the first helicopter) item. Only one extra item advances per frame. Units progress while capped and block only
at completion. Iteration order: queue index order; libraries process slots `0..min(cities_with_library, queued)−1`,
deepest first (each `do_queue(s)` recurses into `s+1` before handling `s`), so with two finishing items the later one is
unqueued first.

#### 6.3.2 `Build::finished(t)` @628490 ✅ (vslot +0x1b0; @470e50 is the `finished(int)` thunk)

```
if trainable(t):
    if L.pop_cap < control_cost(t) + L.control: return 0          // same test as check_population
    if caravan(t) && L.get_units(t, 0) ≥ L.get_caravan_limit(0): return −1
    if B.is(AIRBASE, 0) && !(unit_flags(t) & 0x20) && B.num_aircraft_here(0) ≥ B.num_aircraft_limit(): return −1
    B.train(t); return 1                                          // train's result is ignored
if is_spell_type(t) && L.has_spell(t): SpellType::cast(o, who, 0, 0); return 1
if is_build_type(t) && !(buildtypes[t].build_flags & 4):          // in-place building upgrade
    old = CityData::get_pop_value(city)
    B.set_type(t, SET_TYPE_NORMAL)  (vslot +0x84 → Wall::set_type, §6.8.2)
    Wall::mask_me(1, REGEN_NONE); L.leader_flags |= 0x8000000
    if !(B.flags & 0x20): return 1
    City::find_buildings(city); d = get_pop_value(city) − old
    L.pop += d; game.world_pop += d; if region(B) < 64: L.reg_pop[region] += d
    Leader::calc_pop_cap(); Region::fix_borders(); return 1
// everything else: techs, ages, unit-line upgrades, flag-4 buildings, spells not yet owned
Leader::gain_tech(t, B.x, B.y, 1, 1)                               // §7
if B.is(SENATE, 0) && (h = L.get_gov_hero()) ≥ 0:
    if L.gov_hero_frame (+0xa48) < 0: B.train(h)
    else: u = find_unit(B.x, B.y, FRIENDLY, who, −1, 0x20000, FILTER_GOV_HERO, …); if u ≥ 0: unit(u).set_type(h, 0)
return 1
```
Pop accounting itself (calc_pop_cap @6dc490, check_population) belongs to economy-spec; the only inputs here are
`pop_cap` (+0x7e4), `control` (+0x940) and `control_cost` (+0x2f0). `get_caravan_limit(0)` @6dca50 = epoch(2)+1
+ colossus/taj/silk/nubian bonuses, capped at 99 (the `param ≠ 0` city-count clamp is not used here).
`num_aircraft_limit()` @6454a0: CARRIER-like (`is(CARRIER,1)`) → `max_aircraft_per_carrier`; `is(AIRBASE,1)` →
`max_aircraft_per_airbase`; `is(MISSILESILO,1)` → 1; else 0. `num_aircraft_here(p)` @645330 counts aircraft based there
(🟡 p semantics).

#### 6.3.3 Repeat / infinite queue ✅

- Flag: `build_masks & 0x40` (`BuildData::is_infinite` @62d5b0). Set/cleared only by BUILDMASK (op 33,
  §6.3.4), cleared by `unqueue` when the queue empties, by `clean_queue`, by `action_unqueue` (first click), and by a
  failed re-queue.
- Effect (do_queue above): when a **trainable** item completes, `queue_up(t, 0)` appends a new copy at the end of the
  queue, paying its (current, ramped) cost at that moment through `pay_cost(…, use_escrow = 0)`; the new copy starts at
  `job_counter = 0`. Research items are never repeated. If the re-queue fails (cost, `could_queue`), the flag stays off.

#### 6.3.4 BUILDMASK (op 33): `Group::action_buildmask(mask, set)` @6fc9a0 ✅

`process_buildmask` @947680 → `action_buildmask(buildmask, set)` (9-byte command). **`set` is ignored**: the verb
toggles:
```
if !group.buildings: return
all_on = 1
for each member in list order with is_valid_build (+0x10) && WallData::valid_buildmask(mask) @63e2a0:
    if (build_masks & mask) == 0 && all_on: build_masks |= mask
    else: build_masks &= ~mask; all_on = 0               // from the first member that already had it, clear for all later ones
(console only, mask == 0x40: message + S_INFINITE_QUEUE / _OFF)
```
`valid_buildmask(m)`: `m & 0x40` and the object is a building and `BuildData::can_infinite` @62d4d0 (the type
`is_training_building` and at least one queued item is trainable) → 1; `m & 0x80` and `can_carry(AIR)` → 1; else 0.

### 6.4 Spawning

#### 6.4.1 `Build::train(t)` @62f9b0 ✅ (returns the new captain's `o`, a negative init_unit result, or −1 if the unit died)

```
u = Objects::init_unit(who, t, B.x, B.y, −1, −1, −1)     // frame-spec §8.1: uber_size members, Guy::init_real RNG per guy
if u < 0: return u
ScenarioData::last_unit_built_o[who] = u; L.last_unit_finished[t − 50] = u
U = units[who][u]
if U.get_stance_type() == B.get_stance_type() (vslot +0x108): U.set_stance(B.stance (+0x7e), 0)
U.go_inside(o, who, 0)                                    // captain and, recursively, its o_down followers
if B.type.obj_masks (+0x1e4) & 0x200:                     // aircraft-housing building (airbase): 🟡
    (presentation: options.rebuild = 1)
    if B.gather has points:
        if !(U.type.obj_masks & 0x8000000) && !(U.type.unit_flags & 0x20):      // plane, not missile/heli
            for each gather point i in list order (seek_index): if WorldData::is_valid(p):
                first: U.add_air_patrol_order(p.x, p.y, o, who, 1, q) ; later: append p to that patrol's point arrays
            goto tail
        p = first point
        if is_valid(p):
            b = find_building_at(tile(p.x), tile(p.y), SEARCH_ALL, …); bw = objects.last_who (+0x200)
            if b ≥ 0 && (b,bw) != (o,who):
                if L.is_enemy(bw):
                    if missile && leaders[bw].has_preq(MISSILE_DEFENSE_BONUS 693): console message; return u
                    U.add_strafe_order(b, bw, o, who, 1, QUEUE_NEW, 1); goto tail
                if objects[bw][b].can_carry(u, who) && objects[bw][b].vslot+0x10 && !missile:
                    U.add_strafe_order(−1, −1, b, bw, 1, QUEUE_NEW, 1); goto tail      // rebase
            if !missile: U.add_air_patrol_order(p.x, p.y, o, who, 1, q)
            goto tail
        // invalid point:
        if U.is(HELICOPTER,0) && B.num_aircraft_here(0) > B.num_aircraft_limit(): goto come_out_or_die
        goto tail
    elif U.is(HELICOPTER, 0): goto come_out_or_die
    goto tail
else:
    lim = B.get_garrison_limit() (BuildTypeData::get_garrison_limit(type, who)); if lim == 0: lim = 10
    if B.is(UNIVERSITY, 0):
        if t ∈ {52, 53}: if B.num_inside(1) > B.gather_max: U.come_out(0) else B.check_gatherers() @62f710
        else: U.come_out(0)
    elif B.gather_inside() && B.num_inside(0) ≤ lim && !U.is(CARRIER, 1):
        (console-only message "unit garrisoned")                            // stays inside
    else:
        if U.come_out(0) != 0 && B.num_inside(0) > lim: goto die
    goto tail
come_out_or_die: if U.come_out(0) == 0: goto tail
die: U.die(…) (vslot +0x158; Unit::die → Object::die(0, −1, 0.0); combat-spec §6, RNG) ; return −1
tail:
U.unit_masks &= ~UNIT_MULTIMOVE
if U.is(CARRIER, 1):                                      // a new carrier comes with a full wing
    U.action_unqueue(1)                                   // no-op: num_queued is 0
    f = L.current_upgrade(FIGHTERBOMBER)
    for i in 0 .. U.num_aircraft_limit()−1:               // re-read each iteration
        a = Objects::init_unit(who, f, U.x, U.y, −1, −1, −1); if a ≥ 0: units[who][a].go_inside(u, who, 0)
(presentation: S_CREATED sound and "unit created" message when who == console and frame > 0)
return u
```
So a unit that cannot get out (`come_out` ≠ 0) simply stays garrisoned unless the building is over its garrison
limit, in which case it is killed. `Build::train` is also called directly (no queue) by Build::process (Kremlin, Senate,
Terracotta, markets), Build::activate (tribe bonus units), Leader::gain_tech, City::assimilate, Setup::place_unit.

#### 6.4.2 `Unit::come_out(param)` @617c10 ✅ (returns 0 = out, 1 = could not leave; RNG only at the end)

Locals: `X/Xwho` = container, `ang` = exit angle (init 0x80000000), `r0, r1`, `slot` = temp group (−1), `gp_spot` =
spot near the first rally point, `prev` = previous waypoint, `final`, `fx, fy, fact` = final rally point/action,
`from_bld = 0`, `gpiece = 0`.

**A. Prologue**
```
for g in 0 .. guy_mark−1: guys[g].+0xd0 = 0
if param == 0 && !is_captain (o_up (+0x8e) ≥ 0): return come_out(captain, 0)    // followers redirect to the captain
X = get_inside(&Xwho)                                     // outermost container (ObjectData::get_inside @651a80)
```
**B. Not inside anything (X < 0):** `find_nearby_spot(type, x, y, &sx, &sy, r0 = block_radius, r1 = block_radius +
unit_disembark_distance, step 0, angle 0x80000000, FILTER_NOT_ME, o, who, nocheck 0, uber 0, −1, 0, −1)`; on failure
the same with `nocheck = 1`; on failure `(sx, sy) = (x, y)`. Go to **E** (`set_new_location` only; no x/y store,
no `remove_from_inside`).

**C. Inside X:**
1. `X.is(OILPLATFORM, 0)` and the unit is a land unit (domain 0): `b = current_upgrade(TRANSPORTBARGE)` (−1 →
   TRANSPORTBARGE); `bo = init_unit(who, b, X.x, X.y, −1, −1, −1)` (**RNG**: Guy::init_real per guy); `bo < 0` → return 1;
   `barge.same_damage(o, who)` @5f9400; `Object::insert_inside(barge, o, who)`; `r = barge.come_out(0)`; r == 0 →
   `remove_from_inside(this)`, `insert_inside(this, bo, who)`, return 0; else `barge.die(0, −1, 0.0)` (RNG per
   combat-spec §6), return 1. (🟡 the argument direction of `insert_inside` is literal.)
2. Missile types (`obj_masks & 0x8000000`): `(sx, sy) = X position`, `guys[0].+0x40 = 90.0f`; go to **D**.
3. Otherwise:
   - If not a captain (followers, `param = 1`): `X = this.get_captain()` (vslot +0xe4), `Xwho = who` — followers are
     placed around their captain.
   - If X `is_build`: `from_bld = 1`; if `X.is(UNIVERSITY,0) || X.is(OILPLATFORM,0)`: `L.leader_flags |= 0x2000000`.
   - `gpiece = X.get_gpiece()` (Object vslot +0x34).
   - **Exit angle from the first rally point** — if X ≥ 0, X is a building with rally points and `!X.gather_inside()`:
     `p = first point (list head)`; if `p.action == 3`: (x,y) are an object `(ox = p.x, owho = p.y)`; if `ox ≥ 0`,
     captain and `owho < 8`: `HotKeyGroups::copy(ox, owho, o, who)` (🟡 UI group state); target = that object's
     position; else target = `(p.x, p.y)`. Then
     `find_nearby_spot(target, &gp_spot, r0 0, r1 0x600, step 0, angle 0x55555555, filter, o, who, 0, uber, −1, 0, −1)`
     with `block_radius == 0` → `filter = FILTER_ALL, uber = 1`, else `FILTER_NOT_ME, uber = 0`. On success:
     `ang = find_angle(gp_spot − X)` and the unit's `angle` (+0x50) is written directly with it. (On failure `gp_spot`
     holds the target, per find_nearby_spot's contract.)
   - Ring radii: X a building → `S = (X.x_size + X.y_size)·48`; boats (domain 1): `d = boat_train_distance`, `m =
     boat_train_max_distance`, `S += big_radius` (of the unit); others `d = unit_train_distance`, `m =
     unit_train_max_distance`; `r0 = (X.flags & VALID) ? S + d : 0`; `r1 = S + m`. X a unit (followers) →
     `ang = X.angle`, `r0 = X.type.block_radius`, `r1 = r0 + unit_disembark_distance`.
   - Spot: `block_radius == 0` → `find_nearby_spot(X.pos, &sx, &sy, r0, r1, 0, ang, FILTER_ALL, o, who, 0, 0, −1, 0, −1)`,
     failing that `(2·r0, 2·r1, …, nocheck = 1)`, failing that `(sx, sy) = X.pos`. `block_radius ≠ 0` → the same with
     `FILTER_NOT_ME`, retry with `nocheck = 1` (radii **not** doubled), failing that **return 1** (stays inside).
   **D.** `x = sx, y = sy` (stored encoded), `z = TerrainOut::find_tcoord_z(tile(sx), tile(sy), 1)` (the 3D height is
   simulation state here), `Object::remove_from_inside(this)`.

**E. Placement and housekeeping**
```
set_new_location(sx, sy, 1, 1)
if unit_masks2 & 0x10000 (CEO): update_ceo_position(−1, −1, tile(sx), tile(sy))
if domain == 2: Guy::update_z(guys[0]); if unit_flags & 0x20: guys[0].+0x70 = guys[0].+0x14 = guys[0].+0x14 + 500
if o_down ≥ 0 && follower VALID: follower.come_out(1)          // recursion along the uber chain, before rally orders
if type ∈ {52, 53}: set_anim(CHAR_DEFAULT, 1, 1)
if !(L.leader_flags & 4 /*human*/) && !(is_plane && domain == 2 && !(unit_flags & 0x20)) && type ∉ {50,51,52,53}:
    unit_masks &= ~UNIT_MULTIMOVE; path.length (+0xc0) = 0; close_orders(0); clear_partial_path(); update_action()
if is(NUCLEARMISSILE, 0) && has a current order && order.vslot+0x100 target (x, y) both ≠ −1: L.nukes_launched += 1
```
**F. Rally points** (captain only; followers inherit the captain's angle instead: `if X ≥ 0 && X.is_valid_unit
(vslot +0x8)`: `set_angle(X.angle, …, 1)`):
```
if captain && X ≥ 0 && X.is_build:
    if uber_size > 1: g = {this}; slot = Groups::push_group(who, g, 1)   // temp group for the uber unit
    if (X.flags & 0x20) && X.inside_down < 0 && X.city ≥ 0 && (city.flags (+4) & 0x40):
        found = any unit of who (slot order, VALID, is_worker, action_type == 26) whose update_action() target is (X.o, Xwho)
        if !found && (L.leader_flags & 4): city.flags &= ~0x40
    if X has points && !X.gather_inside() && order_type() == 0:
        n = num_gather; prev = (sx, sy); final = 0
        for i in 0 .. n−1 (list order from head):
            p = point i
            if i ≥ 1:
                if find_nearby_spot(p, &q, 0, 0x600, 0, 0x55555555, FILTER_NOT_ME, o, who, 0, 0, −1, 0, −1) fails: continue
            else: q = gp_spot                              // point 0 uses the spot found for the exit angle
            if i == n−1: final = 1; (fx, fy, fact) = (p.x, p.y, p.action); break          // raw point, not q
            special = 0; skip = 0
            if p.action != 0:
                b = find_any_building_at(tile(p.x), tile(p.y), —, who, 0x11, who, p.y >> 6) ; bw = objects.last_who
                if b ≥ 0:
                    if is_peasant && p.action == 1 && b.is_wallbuild && bw == who:
                        if !b.is_active: special = 1                   // build it
                        elif b.damage != 0: special = 1                // repair it
                        elif b.is_valid_wall && b.type.is_gather_type && !b.is(UNIVERSITY,0)
                             && b.num_gatherers(0,0) < b.gather_max: special = 1
                        else: skip = 1
                    if b.is_active_build && L.is_ally(bw) && b.get_garrison_limit() && type.can_garrison(b.type) && p.action == 1:
                        if b.num_inside(0) + control_cost(0) ≤ limit: special = 1 else skip = 1
                    if attack ≠ 0 && !is_worker && L.is_enemy(bw) && p.action == 2: special = 1
                    if skip: continue
                    if special: final = 1; (fx, fy, fact) = (p.x, p.y, p.action); break
                elif attack ≠ 0 && !is_worker && p.action == 2: continue
            // intermediate waypoint
            if uber_size > 1 && slot ≥ 0:
                Unit::find_nearby_spot(q, &q2, 0, −1, 0, 0x55555555, FILTER_NOT_ME, nocheck 0, uber 1, —, —, −1)
                groups[slot].action_move_to(q2, QUEUE_LAST, set_angle 1, find_angle(q2 − prev), MOVE_TO(1), 1, −1, −1, 0)
            else:
                add_move_facing_order(q.x/48, q.y/48, find_angle(q − prev), orders 1, 0, QUEUE_LAST, explicit 1, −1, −1, −1, 0)
            prev = q
```
(Note the two `find_nearby_spot` results are kept in different registers: the first waypoint uses the search result,
later waypoints are skipped entirely when their own search fails, and the final point is used raw.)

**G. Final rally point** (`final == 1`; else go to H):
```
b = find_any_building_at(tile(fx), tile(fy), —, who, 0x11, who, …); bw = objects.last_who
if b ≥ 0:
    relocate on X's exit ring facing b:  find_nearby_spot(X.pos, &s, r0' = S + unit_train_distance,
        r1' = S + unit_train_max_distance, 0, find_angle(b − X), FILTER_ALL, o, who, 0, 0, −1, 0, −1)   // S of X, never the boat variant
        → on success set_new_location(s, 1, 1)
    if type ∈ {50..53} && b.is_wallbuild:
        if type ∈ {50, 51}:   if !b.is_active && bw == who: add_build_order(b, bw, QUEUE_LAST, 0); final = 0
                              elif b.damage ≠ 0:            add_repair_order(b, bw, QUEUE_LAST, 0); final = 0
                              elif fact && b.is_valid_wall && b.type.is_gather_type && !b.is(UNIVERSITY) && bw == who:
                                                            add_gather_order(b, QUEUE_LAST, 1); final = 0
        else (scholars):      if fact && b.is_valid_wall && b.type.is_gather_type && b.is(UNIVERSITY) && bw == who:
                                                            add_gather_order(b, QUEUE_LAST, 1); final = 0
    if is_caravan && b.is_build && b.is_trade (vslot +0x24) && !L.is_enemy(bw):
        add_trade_order(b, bw, −1, −1, QUEUE_LAST, 0); final = 0
    elif final && b VALID && b.is_build && b.is_active && L.is_ally(bw) && b.get_garrison_limit() ≠ 0
         && type.can_garrison(b.type) && fact ≠ 0:
        add_garrison_order(b, bw, 0, QUEUE_LAST, 0) on the captain, then on every o_down follower; final = 0
    if attack ≠ 0 && L.is_enemy(bw):
        if fact ≠ 0: add_attack_order(b, bw, QUEUE_LAST, 1, 1) on the captain and every follower; done
        elif get_combat_stance() == HOLD_FIRE(5):
            slot ≥ 0 ? groups[slot].action_move_to(fx, fy, QUEUE_NEW, 0, 0, MOVE_TO(1), 0, −1, −1, 0)
                     : add_move_order(fx, fy, 1, 0, QUEUE_NEW, 0, ⟨uninitialised int⟩, −1, −1); done
        elif !type.is_siege (vslot +0x10c):
            slot ≥ 0 ? groups[slot].action_move_to(fx, fy, QUEUE_NEW, 0, 0, ATTACK_TO(2), 0, −1, −1, 0)
            : uber_size ≤ 1 ? add_move_order(fx, fy, 2, 0, QUEUE_NEW, 0, ⟨uninit⟩, −1, −1)
            : add_attack_order(b, bw, QUEUE_LAST, 1, 0) on captain and followers; done
        else: add_attack_order(b, bw, QUEUE_LAST, 1, 0) on captain and followers; done
// no building there, or it matched nothing:
if final:
    u = find_unit_with_radius(fx, fy, —, search = who, radius 1, —, filter 0x11, who, 0); uw = objects.last_who
    if u ≥ 0:
        relocate on X's ring facing u (as above); if attack ≠ 0 && L.is_enemy(uw):
            add_attack_order(u, uw, QUEUE_LAST, fact ? 1 : 0, fact ? 1 : 0) on the captain and every follower; done
    orders = 1
    if attack ≠ 0 && stance ≠ HOLD_FIRE && types[type.where].is(BARRACKS|STABLE|DOCK, 0): orders = 2  // by the unit's `where`, not the building
    if captain:
        if uber_size > 1 && slot ≥ 0:
            Unit::find_nearby_spot((fx, fy), &q, 0, −1, 0, 0x55555555, FILTER_NOT_ME, 0, 1, —, —, −1)
            groups[slot].action_move_to(q, QUEUE_LAST, 1, find_angle(q − prev), orders, 1, −1, −1, 0)
        else:
            relocate on X's ring facing (fx, fy) (same radii/filter as above), set_new_location on success
            add_move_facing_order(fx/48, fy/48, find_angle((fx, fy) − new position), orders, 0, QUEUE_LAST, 1, −1, −1, −1, 0)
```
(`⟨uninit⟩`: the 7th argument of `add_move_order` is pushed from ECX after an unrelated call — undefined in the
original; `add_move_order` @616ed0 forwards it as `reverse`. 🟡 a port should pass 0 and note the divergence risk.)

**H. Epilogue**
```
if from_bld && frame ≠ 0:
    add_spec_anim_order(1, gpiece, 0, —)        // always front-inserted (QUEUE_FIRST pattern), flags |= 4
    order.+0x1c = X.x; +0x20 = X.y; +0x24 = X.o; +0x28 = Xwho; do_spec_anim(order)    // walk-out animation
    // (port: Unit::do_spec_anim type 1 kills the order in this same call unless the unit leaves an airbase, so the
    //  net effect on the order list is nil; engine/sim/queues.cpp)
(presentation: options.rebuild = 1)
if (unit_masks & UNIT_AI_CONTROL) && !is_caravan && type ∉ {MERCHANT 61, MERCHANTDUTCH 62, FURTRAPPER 400}:
    if (unit_flags2 & 0x10 /*special*/) || is(BARK, 0):
        v = game_random.get(0, 0xffff)                       // RNG @61a1bb (BARK) or @61a1d5 (other)
        join = is(BARK) ? (frame & (v % 3)) != 0 : (frame & (v % 2)) != 0     // signed % (v ≥ 0)
    else: join = is(SPY, 0)                                  // no draw
    if join: add_to_army() @5f7740
return 0
```

#### 6.4.3 Stance, garrison and university details ✅

- The trained unit takes `B.stance` only when its stance *type* equals the building's (`UnitData::get_stance_type`
  vs `BuildTypeData::get_stance_type`). `B.stance` is set in `Build::init` from the leader options (decompile @629740
  lines 94–112; 🟡 the option mapping).
- "Gather inside" (`BuildData::gather_inside` @46f180): the **first** rally point has action ≠ 3 and `x < 0 || y < 0`
  (the "inside" point written by GATHER_POINT on its own building, §6.6).
- Universities: scholars stay inside up to `gather_max` and are registered by `check_gatherers`; the rest come out.

### 6.5 Unqueue

#### 6.5.1 UNQUEUE (op 48) ✅

15-byte command `{i32 who, i32 o, i32 index, i16 uid}`. `CommandPackage::process_unqueue` @9466f0:
`objects[who][o]` not a building (`is_wallbuild == 0`): if VALID and `uid` matches → `Unit::action_unqueue(1)` (§6.7);
else (building): if VALID and uid matches → `Build::action_unqueue(index)`. Issued by `BuildOut::issue_unqueue(i)`
@629e70 (UI; skipped when `semaphore byte0 & 0x10`; `check_accept_issue`) and `UnitOut::issue_unqueue` @607bc0.

`Build::action_unqueue(i)` @620280:
```
loop: if B is a library && item max(i,0) is not DISBAND && (lib = first library) != o && lib ≥ 0: B = library(lib); repeat
if queued == 0: return
if build_masks & 0x40: build_masks &= ~0x40 (console sound S_INFINITE_QUEUE_OFF); if i ≥ −1: return   // first click only stops repeating
if i ≤ −10: while queued: B.unqueue(queued−1, 1)
elif i ≤ −5: repeat min(queued, 5) times: B.unqueue(queued−1, 1)
else: B.unqueue(i < 0 ? queued−1 : i, 1)
```

#### 6.5.2 `Build::unqueue(i, cancel)` @6207c0 ✅ (Build vslot +0x1c8)

```
disband = queued && i < queued && i < queue_size && queue[i].type == DISBAND
if B.is(LIBRARY,0) && !disband && (lib = first library) != o && lib ≥ 0:
    library(lib).unqueue(i − queued, cancel); return
if queued == 0 || i > queued − 1: return
(presentation: options.rebuild = 1)
if cancel: while i+1 < queued && type[i] == type[i+1]: i += 1     // cancel the LAST of a run: head progress survives
queue[i].job_counter = 0; t = queue[i].type
if t ≥ 0:
    if L.num_queued[t] ≠ 0: L.num_queued[t] −= 1
    class_queued(−1, t) with zero clamps (§6.10)
if is_age_type(t): L.ages_queued −= 1;  if is_epoch_type(t): L.epochs_queued −= 1    // no clamp
if cancel: B.unpay_cost(i)                    // @6206e0: stock[good[k]] += cost[k] for k = 0..2 with good[k] ≥ 0
if i < queued − 1: un_queue(i, queued)        // shift down, stale tail (§6.1.1)
queued −= 1; if queued == 0: build_masks &= ~0x40
```
Refunds are exact (the stored triples), go to stock only (not escrow), and nothing is refunded for progress (there is
no partial refund). `do_queue` removes finished items with `cancel = 0` (no refund, no run search).

`Build::refund_cost(i)` @620490 (called only by `Leader::gain_tech` for the other queued **tech** items of the first library
when a **science** library tech is gained, §7.1.1 step 3): per stored good k: `newc = cost[k]·100 / (100 − tech_science_discount·(s − tech.age))`;
`newc += trunc((s − tech.age + 1)·tech_science_discount·newc / −100)` with `s` = science epochs before the gain; `stock[good] += cost[k] − newc`;
`cost[k] = newc` (re-prices a queued tech to the cheaper post-age price and refunds the difference).

#### 6.5.3 `Build::clean_queue(refund)` @620b60 ✅

`while queued: unqueue(queued − 1, refund)` (inlined for `Build`, library redirect included), then `build_masks &=
~0x40`. Callers: `Build::close` (death: `refund = !(semaphore1 & 8)` for non-libraries; libraries hand their items over,
§6.2.6), `Build::swap_team` @628210 (capture: `clean_queue(1)` for non-libraries, before `Wall::swap_team`), `City::close`,
`Leader::defeat`/`victory`, editor and script functions. Items are removed from the back.

#### 6.5.4 Queries ✅

`BuildData::get_queue(i)` @62d280 (library-aware: indices ≥ queued map into the first library's queue at `i −
queued`), `find_in_queue(QUEUE_COUNT_TYPE, t)` @62de70 (first index with `types[type].is(t, 1)`, else −1),
`count_queue(kind, t)` @62dee0 (count over `0..queued−1`: PEASANTS → type ∈ {50, 51}; TYPE → `is(t, 1)`; UNITS →
trainable unit types).

### 6.6 GATHER_POINT (op 22): rally points ✅

Storage: `BuildData.gather` (+0xb8), a circular `PtrLinkList<GatherPoint*>` (`head_node` +0xcc, `length` +0xc8,
iterator +0xbc/+0xc0/+0xc4). `GatherPoint` (0x10, heap): `x`, `y` (Coord; for action 3 an object `o, who`), `action`
(u8: 0 move, 1 work/garrison, 2 attack, 3 air/object). No limit on the number of points. Saved: `x, y, action` per
point in list order (state-walk "GatherPoint::walk_data").

List semantics (`LinkListBase::add` @470c60 inserts **before** the head and makes the new node the head):
`Build::add_gather_point(x, y, action, q)` @622e70:
```
if q == QUEUE_NEW: clear_gather()
p = new GatherPoint{x, y, action}; list.add(p)            // p becomes the head
if q == QUEUE_LAST: head = head.next                      // p becomes the tail: iteration from head = click order
if (build_masks & 8) && B.is(AIRBASE, 1): re-issue the patrols of every own VALID air unit whose home_base is B
    (slot order; action-3 points → add_strafe_order(…, QUEUE_NEW); other points → one air patrol order whose point
    list gets the remaining points; keeps the old patrol's +0x4c/+0x50) 🟡
```
`Build::clear_gather()` @623180: free every point; if `(build_masks & 8) && is(AIRBASE,0)`: every own VALID air unit with
`home_base == B`: on the map → `add_strafe_order(−1, −1, o, who, 0, QUEUE_NEW, 0)` (return to base), else
`clear_orders` and remove from `B.launching`.

`Group::action_gather_point(x, y, action, add_to_end)` @6ff1b0 (17-byte command). "Eligible" = `is_valid_wall` (+0xc)
&& `is_build` && `!is(UNIVERSITY,0)` && `!is(MISSILESILO,0)`.
```
action_begin()
if x < 0 || y < 0: clear_gather() on every eligible member; return
clamp x < map_w·0x300, y < map_h·0x300 (to size − 1)
q_last = add_to_end; if any VALID building member that is not a university/silo has gather_inside(): q_last = 0
if action == 3:
    for members (valid_wall, is_build) with is(AIRBASE,0): add_gather_point(x, y, 3, q_last ? LAST : NEW); add to temp group
    if temp group non-empty: temp.action_flight(x, y, ATTACK, 0, 0, 0)
    goto final_loop
if group.buildings && every member is a VILLAGE (count(COUNT_TYPE, VILLAGE) == get_num):
    tile flags & 0x30 == 0x30 → kind = WOODCUTTER; elif tile flags & 3 == 2 → kind = MINE; else skip
    f = find_building(x, y, FRIENDLY, who, 0x600, 0, FILTER_TYPE, kind, 0); if f ≥ 0: (x, y) = f's position (UI ping)
inside = 0
if (tile flags & 3) == 3 && !q_last:
    for eligible members in list order: if WallData::covers_tile(tile(x), tile(y)):
        inside = 1
        if member is TERRACOTTA, KREMLIN or SENATE:
            clear_gather() on the FIRST eligible member (list order); return      // literal: only one member
        break
final_loop:
for members: eligible && !(is(AIRBASE) && action == 3):
    if buildtypes[basic_type].build_flags ≥ 0 (bit 31 clear) && get_garrison_limit() == 0
       && !TERRACOTTA && !KREMLIN && !SENATE: skip
    inside ? add_gather_point(−1, −1, 0, QUEUE_NEW) : add_gather_point(x, y, action, q_last ? QUEUE_LAST : QUEUE_NEW)
(presentation messages)
```
Other writers: `Group::action_city_gather` @701780, `ScenarioFuncSet::set_rally`; `Group::action_recall` clears.
Rally points are applied only by `come_out` (§6.4.2 F–G) and by `train` for aircraft buildings.

### 6.7 Units that produce units (carriers) ✅/🟡

The only unit-side queue is the carrier's aircraft queue (`UnitData.num_queued` +0xa0, `queue_time` +0x64).
- Filled by the SPELL command with BUILDAIR (661): `Group::action_spell` @6fe1a0 (🟡 outer structure): for each member
  (list order) that is a unit, on the map and for which BUILDAIR `is_castable(o, who, 0)`: if `num_queued < 10` and
  `num_aircraft_here(0) < num_aircraft_limit()`: `f = current_upgrade(FIGHTERBOMBER)`; `types[f].can_pay_cost(who, −1,
  −1, 1)` else **return**; `pay_cost(who, −1, −1, 1, null)`; `num_queued += 1`; `L.num_queued[f] += 1`;
  `class_queued(+1, f)`.
- Produced in `Unit::process` step 3 @611158 when `num_queued ≠ 0` and `L.leader_flags & 2`: `f = current_upgrade
  (FIGHTERBOMBER)`; `queue_time += accel_train·(ai_speed > 1 ? ai_speed : 1)`; if `queue_time < train_time(f)` stop.
  If `pop_cap < control_cost(f) + control`: `pop_issues += 1` (console message), `queue_time −= acc` (holds), stop.
  Else `num_queued −= 1; queue_time = 0`; `L.num_queued[f]` −1 (clamped) and `class_queued(−1)`;
  `a = Objects::init_unit(who, f, x, y, −1, −1, −1)` (**RNG** per guy) and `go_inside(carrier)`. No aircraft-limit check.
- `Unit::action_unqueue(refund)` @5e1f20 (UNQUEUE on a unit): if `num_queued == 0` return; `num_queued −= 1` (if it
  reaches 0, `queue_time = 0`); `f = current_upgrade(FIGHTERBOMBER)`; `L.num_queued[f]` −1 (clamped),
  `class_queued(−1)`; `refund` → `types[f].unpay_cost(who, −1, −1)` (Type::unpay_cost @6682f0: adds back
  `get_cost(g, who, −1, −1, 1, 1, −1)` per good — the **current** price, not the price paid).
- `Unit::close` @60ee50: a dying carrier `track_queued(current_upgrade(FIGHTERBOMBER), −num_queued)` and `num_queued
  = 0` (no refund).

### 6.8 Upgrading existing objects: `set_type`

Who calls it: existing units of a line are converted by `Leader::gain_tech` (§7) — for every VALID unit of the
leader in slot order whose type is the upgraded type's `from` (grafted) or lies on the `jump` chain below it, it calls
`set_type(T, SET_TYPE_NORMAL)` (when the new `uber_size` is smaller — only 2 → 1 is supported — the followers are
closed and the captain's `damage = total_damage`). It also relabels queued items (§6.9). `Leader::upgrade_units`
@6c6430 is **AI only** (called by `production_ai`; one `game_random` draw per evaluated candidate when difficulty ≠ 2,
@6c66?? decompile line 267: `(rnd % 3 + 4)·value/5`; ❓ not decoded further). Spells (`cast_civilian`,
`cast_immortal_*`, `cast_to_arms`), `Unit::do_cast` and `Object::eject_contents` also call `set_type`.

#### 6.8.1 `Unit::set_type(T, mode)` @612fa0 ✅ (Unit vslot +0x84; @46ce10 is the thunk)

For `mode == SET_TYPE_NORMAL`:
```
nguys0 = guys.length (+0xe8)
counted = is_captain && (old.control_cost ≠ 0 || is(FIGHTERBOMBER,0) || is_gov_hero) && !(unit_masks & DECOY)
if counted: Leader::track_unit_type(old type, −1, o); L.control −= old.control_cost; L.active −= 1
if is_caravan: L.caras −= 1
for g in T.squad_size .. guy_mark−1:     Objects::kill_guy(who, o, g, 0, −1, 0.0, 1); recycle guy; guys[g] = null
for g in old.squad_size .. nguys0−1:     same (old crew guys)
grow the guys array to T.squad_size + T.crew_size
for g in nguys0 .. T.squad_size−1: guys[g] = null          // a bigger squad is NOT filled (healing regenerates)
guys.length = T.squad_size + T.crew_size
for g in T.squad_size .. guys.length−1: guys[g] = new Guy (Recycler pop), Guy::clear
ptype = objecttypes[T]
guy_mark = min(guy_mark, T.squad_size)
for g in 0 .. guy_mark−1: guys[g].type = T; Guy::init_real(guys[g], 1) (RNG, % 100 idle variant); guys[g].guy_num = g
for g in T.squad_size .. guys.length−1: guys[g].type = T; guy_num = g; who/o set; Guy::init_real(guys[g], 1) (RNG)
```
Then (all modes) `SubObject::set_type(T, mode)` @662450: `ptype = objecttypes[T]`, `Unit::update_gpiece` (+
GraphicEvents preload — presentation). If `mode != LOAD`: `unit_masks2 bit 0x10000 = is(THECEO 357, 1)` (+
`update_ceo_position` when set), `is(MARINES 119, 0)` → `unit_masks2 |= 0x800`, `update_hits(0)` (vslot +0x15c),
`update_los()` (+0x160), `update_armor()`, `update_speed()`. `NORMAL` → `update_gpiece()`; then if counted (re-evaluated
with T): `track_unit_type(T, +1, o)`, `control += T.control_cost`, `active += 1`; caravan → `caras += 1`; guys
`[0, guy_mark)` `Guy::set_new_location(x, y, 1)`; if `game.info.version` is newer than `xpack_rtm_version`: supply
registration follows `unit_flags2 & 0x40` (`Supplies::close_supply` / `init_supply`, `supply` +0x84).

`damage` is untouched, so hit points become `new hits − damage` (no percentage scaling). Order list, position, group,
stance are kept.

#### 6.8.2 `Wall::set_type(T, mode)` @640da0 ✅ (buildings; @4723b0 thunk)

`NORMAL` → `Wall::decrement_stats()` @642da0; `SubObject::set_type(T, mode)` (ptype, `Wall::update_gpiece`); `mode !=
LOAD` → `Wall::increment_stats()` @643270. `increment_stats`: an unfinished site counts as **queued** (`num_queued[type]
+= 1`); an active building: `num_buildings[t − 414] += 1`, `reg_buildings[region][t − 414] += 1` (region < 64),
military trainers appended to `L.mil_trainers`. `decrement_stats` mirrors it (§3–4 owns these counters). No RNG.
Building `damage`, queue and rally points are kept; `Build::finished` adds the city-pop / border refresh (§6.3.2).

### 6.9 Queue side effects of `gain_tech` (summary; §7 owns the function) 🟡

- Queued tech items of the first library other than the gained one get `refund_cost(i)` when the gained type is a
  science library tech (§7.1.1 step 3; formula §2.2.4).
- Unit-line upgrade T: in every building (slot order 2000..mark, VALID, `queued ≠ 0`), each queued unit item whose type
  is `graft(T.from)` or reaches T through the `jump` chain is relabelled: `track_queued(old, −1)`, `set_queue(i, T,
  null, keep_jc = 1)` (progress and paid cost kept), `track_queued(T, +1)`. Carriers: `track_queued(old fighter,
  −num_queued)` / `(new, +num_queued)`.

### 6.10 Leader bookkeeping ✅

`class_queued(±1, t)` (inlined in queue_up / unqueue / clean_queue / carrier code; `Leader::track_queued(t, d)`
@6e0f30 is the same as a function): only for unit types with `attack ≠ 0`, by `where` (+0x40):
BARRACKS → `barracks_queued += d, combat_queued += d`; STABLE → `stable_queued, combat_queued`; SIEGEFACTORY →
`factory_queued`; DOCK → `dock_queued`; otherwise domain 2 → `air_queued`. Decrements are skipped when the counter is
0 (track_queued: `if counter ≠ 0 || d ≥ 0`). `track_queued` also adjusts `num_queued[t]` with the same zero rule.

`Leader::track_unit_type(t, d, o)` @6e0dd0 (Objects::init_unit, Unit::close, set_type, decoys): `num_units[t − 50] +=
d`; attack ≠ 0 → by `where`: `barracks_units` +0x9f8 / `stable_units` +0x9fc (both also `combat_units` +0xa04),
`factory_units` +0xa00, `dock_units` +0xa08, domain 2 `air_units` +0xa0c; peasants → `peasants` +0x978, scholars →
`scholars` +0x97c, `role & 0x10` → `scouts` +0x988; if `o ≥ 0 && is(MILITIA 66, 0)` and that unit's +0x54 type
`is(SCHOLARS, 0)` → `scholar_militia` +0x9f0 += d; else if `is(SPECIALFORCES 75, 0)` → `leader_flags` bit 0x20000 =
`num_units[t] ≠ 0`.

`LeaderData::get_queued(t, base)` @6e0d50: for unit types, `base + Σ get_queued(i, num_queued[i])` over every type i ≠ t
with `graft(types[i].from) == t` (recursively: items of earlier members of the line count as t). `Leader::queued_units()`
@6ce000 (AI): Σ `control_cost` of queued trainable (`type_avail > 3`) unit items over active buildings (slot order).
The cost ramp inputs (`num_units`, `num_queued`, per-type `job_extra_time × unit_rate_progression` in `train_time`)
are §2's.

### 6.11 RNG draws

| Site | Function | Condition | Draw |
|---|---|---|---|
| @61a1bb | `Unit::come_out` | `UNIT_AI_CONTROL`, not caravan, type ∉ {61,62,400}, `is(BARK)` | `v % 3`; join army if `frame & (v%3)` |
| @61a1d5 | `Unit::come_out` | same, `unit_flags2 & 0x10` (special) and not BARK | `v % 2`; join if `frame & (v%2)` |
| per guy (@5db6e9) | `Guy::init_real` via `Objects::init_unit` | every unit created by `train`, carrier wing, BUILDAIR production, oil-platform barge | `% 100` idle variant (frame-spec §8.1, movement-spec RNG table) |
| per guy (@5db6e9) | `Guy::init_real` via `Unit::set_type` | `[0, guy_mark')` and every crew guy, on each conversion | `% 100` |
| combat-spec §6 | `Object::die` | trained unit stuck in an over-full building; failed barge | death RNG |
| decompile l.267 | `Leader::upgrade_units` (AI) | difficulty ≠ 2, per candidate | `% 3` value jitter ❓ |
| — | `Group::action_queue_up` | research refused, console | `SoundGlobal::random` (presentation only) |

No draws in `queue_up`, `do_queue`, `finished`, `unqueue`, `clean_queue`, `add_gather_point`, `action_gather_point`,
`find_nearby_spot`, `current_upgrade`, `type_avail`. Iteration-order dependencies: group member order after the
`queued` sort (§6.2.2), queue index order, library recursion (deepest first), `o_down` chain before rally orders,
gather-point list order (head = first click), unit slot order in `clear_gather` / `add_gather_point` (airbases), building
slot order in `get_first_library`.

### 6.12 Rules / data read

Type: `type`, `preq[0]`, `from`, `where`, `upgrade`, `jump`, `tribe_mask`, `obj_masks` (0x200 aircraft housing,
0x8000000 missile), `attack`, `domain`, `x_size`, `y_size`, `block_radius`, `big_radius`, `unit_flags` (0x20 no-limit
aircraft / helicopter, 0x1000000 tribe-restricted, 0x4000000 gov hero), `unit_flags2` (0x8 caravan, 0x10 special, 0x40
supply), `role` (0x10 scout), `control_cost`, `squad_size`, `uber_size`, `crew_size`, `build_flags` (bit31 training
building → 20 slots, bit30 military trainer → 20, bit27 → 10 slots, 0x10 trains while unassimilated? (can_make), 4 =
upgrade-by-research / not an in-place upgrade), techtypes +0x14 (category), +0x1c8 (age), spelltypes +0x40 (`where`).
All are in `engine/sim/rules.hpp` (`Type` struct) — names map as listed in the conventions. Constants: see conventions;
all present in `rules_constants_table.inc`.

### 6.13 Side effects on other systems

Pop: `control`/`active` via init_unit and set_type, `pop_issues` (do_queue, carriers), `calc_pop_cap` after in-place
building upgrades (economy-spec). Territory/borders: `Region::fix_borders` (building upgrade). Fog/LOS: init_unit
(`update_seen`), set_type (`update_los`), set_new_location in come_out (fog-spec). Stats: `last_unit_finished`,
`last_unit_built_o`, `nukes_launched`, `tech_frame`, `tech_cat_frame`. Orders: rally orders and the walk-out
SPECIAL_ANIM (orders-spec). Groups: `push_group` for uber units (orders-spec §1.4). Armies: `add_to_army` (AI).

### 6.14 Corrections to existing specs

- frame-spec §10: `Build::train` returns the unit's `o` (or < 0); the 0 / −1 / 1 codes are `Build::finished`'s. A
  pop-capped head **keeps progressing** and only blocks at completion; `pop_issues++` every such frame. The "next
  non-unit item" fallback is: first research-like item; for −1 results also the first non-caravan / helicopter item.
  Libraries process up to "cities with a library" items in parallel. The repeat re-queue pays the cost again (no
  escrow) and drops the repeat flag when it fails.
- orders-spec §2.8: the sort is over `queued` with ties in list order; silos skip unit types they cannot carry; the
  "rounds" path also covers building types without `build_flags & 4`; queue_up's 2nd argument is "use escrow".
- orders-spec §2.9: Terracotta/Kremlin/Senate under the cursor clear the rally point of the **first** eligible member
  only; universities and silos are never eligible; buildings with `build_flags` bit 31 clear need a garrison limit (or
  to be Terracotta/Kremlin/Senate) to accept a point.
- BUILDMASK's `set` argument is ignored (toggle, §6.3.4).

### 6.15 Open questions

- ❓ `HotKeyGroups::copy` in come_out (action-3 points with an object): is it pure UI or does it touch groups that
  feed commands? (`@714fd0`.)
- ❓ filter 0x11 of `find_any_building_at` / `find_unit_with_radius` in come_out (FilterIndex beyond the ported 0..5).
- 🟡 `add_move_order`'s uninitialised 7th argument on the HOLD_FIRE / ATTACK_TO final-point paths.
- 🟡 `Object::insert_inside` argument direction in the oil-platform barge path.
- 🟡 airbase rally re-issuing (`add_gather_point` / `train` air branches), `num_aircraft_here(p)`.
- ❓ `Leader::upgrade_units` (AI production) not decoded beyond its one RNG draw.
- 🟡 `type_avail` SENATE branch details; `type_eligible` is §7's.

### 6.16 Evidence

| Function | Address | Used for |
|---|---|---|
| `BuildQueue::init/close/set_queue/set_job_counter/un_queue/walk_data` | 6307d0 / 6306c0 / 6309f0 / 630ab0 / 630ad0 / 6305f0 | §6.1.1 |
| `BuildQueueData::get_next_non_unit/_helicopter/_non_caravan` | 630b10 / 630bb0 / 630c20 | §6.3.1 |
| `Build::init` | 629740 | capacity, `queued = 0` |
| `CommandPackage::process_queue_up` | 948230 | §6.2.1 |
| `Group::action_queue_up` | 6fdbb0 (asm) | §6.2.2 |
| `Build::queue_up` | 620f40 (asm) | §6.2.3 |
| `Type::pay_cost` / `unpay_cost`, `TypeData::can_pay_cost` | 6681f0 / 6682f0 / 667570 | cost plumbing |
| `BuildData::could_queue` / `can_queue` / `can_make` | 62da50 / 62da00 / 62db10 (asm) | §6.2.4 |
| `BuildTypeData::queue_here` | 639d10 | §6.2.4 |
| `LeaderData::type_avail` / `tribe_can_type` | 6e33a0 / 6d9380, 6d9410 | §6.2.4 |
| `LeaderData::current_upgrade` | 6e3140 (6e1360 thunk) | §6.2.5 |
| `LeaderData::get_first_library` / `get_building_cities` | 6db6c0 / 6e06f0 | §6.2.6 |
| `Build::do_queue` | 61e410 (asm) | §6.3.1 |
| `LeaderData::check_population` | 6e1330 | pop test |
| `Build::finished` | 628490 (asm), 470e50 | §6.3.2 |
| `LeaderData::get_caravan_limit`, `ObjectData::num_aircraft_limit` | 6dca50, 6454a0 | limits |
| `CommandPackage::process_buildmask`, `Group::action_buildmask`, `WallData::valid_buildmask`, `BuildData::can_infinite/is_infinite` | 947680, 6fc9a0, 63e2a0, 62d4d0 / 62d5b0 | §6.3.4 |
| `Build::train` | 62f9b0 (asm) | §6.4.1 |
| `Unit::come_out` | 617c10 (asm, all 2789 lines) | §6.4.2 |
| `UnitType::find_nearby_spot`, `Unit::find_nearby_spot` | 61de70, 617010 (asm) | spot search (ported in engine/sim/search.cpp) |
| `ObjectData::get_inside`, `Unit::go_inside`, `BuildData::gather_inside/num_gather/get_first_gather` | 651a80, 61a2e0, 46f180 / 46f0a0 / 46f140 | §6.4 |
| `Unit::add_spec_anim_order`, `Unit::add_to_army` | 5e4160, 5f7740 | §6.4.2 H |
| `CommandPackage::process_unqueue`, `BuildOut::issue_unqueue`, `Build::action_unqueue` | 9466f0, 629e70, 620280 (asm) | §6.5.1 |
| `Build::unqueue`, `Build::unpay_cost`, `Build::refund_cost` | 6207c0 (asm), 6206e0, 620490 | §6.5.2 |
| `Build::clean_queue`, `Build::close`, `Build::swap_team` | 620b60 (asm), 628980, 628210 | §6.5.3 |
| `BuildData::get_queue/find_in_queue/count_queue` | 62d280 / 62de70 / 62dee0 | §6.5.4 |
| `Group::action_gather_point`, `Build::add_gather_point`, `Build::clear_gather`, `LinkListBase<GatherPoint>::add` | 6ff1b0 (asm), 622e70 (asm), 623180, 470c60 (via 46d5a0 twin) | §6.6 |
| `Group::action_spell`, `Unit::process` (step 3), `Unit::action_unqueue`, `Unit::close` | 6fe1a0 (asm), 610bc0, 5e1f20, 60ee50 | §6.7 |
| `Unit::set_type`, `SubObject::set_type`, `Wall::set_type`, `Wall::increment_stats/decrement_stats` | 612fa0, 662450, 640da0, 643270 / 642da0 | §6.8 |
| `Leader::gain_tech` (queue parts only), `Leader::upgrade_units` | 6dcb60, 6c6430 | §6.8–6.9 |
| `Leader::track_queued`, `track_unit_type`, `LeaderData::get_queued`, `Leader::queued_units` | 6e0f30, 6e0dd0, 6e0d50, 6ce000 | §6.10 |

## 7. Research, ages and upgrades

Scope: what happens when a tech / unit upgrade / building upgrade / age is *gained* (and lost), how
availability is decided at runtime, libraries, tech bonuses. Queue mechanics (`Build::do_queue`, `Build::queue_up`,
`Build::finished`, `BuildQueue::set_queue`) are §6; costs and times (`TypeData::get_cost`, `ObjectData::train_time`,
`TypeData::research_time`) are §2; `Unit::set_type` / `Wall::set_type` internals are §6. Conventions as combat-spec §0.
New decompiles: `re/decomp/prod_D/` (C) and `re/decomp/prod_D_asm/` (gain_tech, set_age, lose_tech, current_upgrade,
has_upgrade disassembly); job files `re/jobs/dc_prod_D_1..6.txt`; callers in `re/decomp/prod_D/callers*.txt`.

### 7.0 State and vocabulary ✅

TypeIndex ranges (rules-spec §9): ages 544..550 (CLASSICAL..INFORMATION), epoch ("library") techs 551..578 in four lines
of 7 (science 551, commerce 558, civic 565, military 572), finals 579..582 (MISSILE_SHIELD, GLOBAL_GOVERNMENT,
VIRTUAL_REALITY, NANOTECHNOLOGY), plain techs 583..622, governments 623..628 (DESPOTISM/REPUBLIC, MONARCHY/DEMOCRACY,
SOCIALISM/CAPITALISM, as pairs `623+2k`, `624+2k`), bonuses 684..805.
`TechTypeData.cat` (+0x14): military 0, civic 1, commerce 2, science 3 (all non-epoch techs 3). `TechTypeData.age`
(+0x1c8): 0 = Classical … 6 = Information, 7 = finals.

Per-leader research state (LeaderData; layouts from `pdbtypes`):

| Field | Offset | Meaning |
|---|---|---|
| `tech` | BitMask<806> +0x6c0c (bits at +0x6c18) | one bit per TypeIndex: techs known, **units unlocked/upgraded**, global building upgrades, spells. `BitMask::set` @450360: set → `flags`(+8) = 0; clear → `flags` = 2 if it was 0. `flags` is a cache, not walked (state-walk). |
| `tech_at_start` | +0x6c80 | bits granted by `Leader::init` (start techs/units) |
| `obs_flags` | +0x6cf4 (bits +0x6d00) | type obsolete for this leader (predecessors of upgrades) |
| `data_encrypted->ages` | LeaderDataEncrypt +0xdc, value `^ 0x62766` | leader age = number of ages gained (0..7). `LeaderData::get_age` @43ecd0 |
| `->epochs` | +0xe0, `^ 0x69587` | total epoch techs |
| `->discovered` | +0xe4, `^ 0x13985` | other techs/units gained (score) |
| `->epoch[4]` | +0xe8 + 4·cat, `^ 0x63187` | epoch techs per line: [0] military, [1] civic, [2] commerce, [3] science |
| `age_stamp[7]` | +0x8f8 | frame each age was gained (`age_stamp[t-544]`) |
| `gov` | +0x14 | current government TypeIndex |
| `gov_hero_frame` | +0xa48 | set to 1 by a gov tech gained at frame 0 |
| `misery` | +0x7ec | zeroed by every epoch tech |
| `ally_mask` | +0x6929 (byte) | fog-spec §6.5 |
| `BuildData.max_age` | Build +0x83 (byte) | per building, raised to the leader age |
| `leader_flags` bits | +0x0 | `LeaderFlagIndex`: 0x800 REVEAL_ENEMY, 0x1000 EXPLORE_MAP, 0x2000 ACTIVE_PING, 0x1000000 NEW_TECH, 0x2000000 RECOMPUTE, 0x4000000 CALC_UNIT_STATS, 0x8000000 CALC_WALL_STATS |

A tech bonus is never stored: "has bonus B" is always `has_preq(B)` evaluated on demand (§7.11).

### 7.1 `Leader::gain_tech(t, x, y, notify, upgrade_existing)` @006dcb60 ✅ (from decompile + disassembly)

`@00471110` is a thunk that calls it with `notify = 0` (x, y, upgrade_existing passed through).
`notify` (param_4) only gates messages/sounds; `upgrade_existing` (param_5) gates the conversion of existing units and
queue entries when `t` is a unit type.

Callers (Callers.java): `Build::finished` @628490 (research/upgrade complete: `gain_tech(who, t, bx, by, 1, 1)` for
techs, for unit types whose tech bit is not yet set (= a unit upgrade research) and for global-upgrade buildings
(`build_flags & 4`); see §6), `Build::queue_up` @620f40 (tech-race shortcut, §7.3.4), `Build::activate` @623e20
(wonder-completion free techs, construction section), `Leader::init` @6e3930 (tribe free techs), `Leader::set_age` /
`set_epoch` (scenario/balance), `ScenarioFuncSet::gain_tech/gain_upgrade`, `ConquestGame::*`, `MapScenario::make`,
`ScenarioEditor::*`, and **recursively itself** (free grants below). Every recursive call runs to completion before the
outer call continues (depth-first): order matters for `set_type` order and for nested flag/counter updates.

#### 7.1.1 Ordered side effects

`T = types[t]`, `who = this->who`, `L = leaders[who]`. "console" steps are presentation (UI/sound/messages), listed only
to keep the order readable.

1. `leader_flags |= NEW_TECH (0x1000000)`; `options.rebuild = 1` (UI).
2. If `T.is_gov_type()` and `game.frame == 0`: `gov_hero_frame = 1`.
3. **Science refund.** If `T.is_epoch_type()`, `techtypes[t].cat == 3` (science), `frame != 0` and
   `lib = get_first_library() ≥ 0` (§7.10): for each slot `q` in `0..lib.queued-1` (ascending) with
   `v = BuildQueueData::get_queue(lib+0x88, q)`, `v != t` and `types[v].is_tech_type()`: `Build::refund_cost(lib, q)`
   @620490. Per cost slot k=0..2 of the QueueItem (`good[k] ≥ 0`): `s = epoch[3]` (science count **before** this
   tech is counted), `a = techtypes[v].age`, `D = C.tech_science_discount` (0x364, =10),
   `base = cost[k]·100 / (100 − D·(s−a))`, `new = base − trunc((s−a+1)·D·base / 100)` (int32 truncating; the binary
   divides by −100 via the signed magic 0xAE147AE1), `bucket[good[k]] += cost[k] − new`, `cost[k] = new`.
   I.e. paid costs of other queued library techs are re-priced as if this science tech were already known.
4. **Rush peace end test.** If `T.is_age_type()`, `info.rush_rules != 0` and `t − 543 == rush_rules`:
   `rush_end = !Game::war_allowed()` @594670 (evaluated before the age counter changes). Used in step 42.
5. **Fighter-bomber bookkeeping.** If `T.is(FIGHTERBOMBER 308, 0)`: `old_fb = L.current_upgrade(308)`.
   Else if `T.is(get_graft(JETFIGHTER 295), 1)` (strict): **recursive** `gain_tech(JETFIGHTERBOMBER 309, x, y, 0, 1)`,
   `old_fb = t`. Else `old_fb = −1`.
6. **Counters** (only if `!L.has_tech(t)`, checked before the bit is set):
   age → `ages++`, console `IFaceMainBase::age_update`, `age_stamp[t−544] = frame`;
   epoch → `epochs++`, `epoch[cat]++`;
   anything else (plain/final/gov techs, **unit types**) → `discovered++`.
   Buildings never count (`has_tech(building) == has_preq(building)`, already true).
7. **CTW civic threshold.** If `semaphore bit 17` (byte2 & 2, CTW per fog-spec) and `T` epoch with cat 1 and
   `frame != 0` and `epoch[1] == C.ctw_wonder_min_civics` (0xad4, =4) — the moment conquest wonders switch on
   (`LeaderData::has_wonder` @6ebc10 counts a conquest wonder only while `epoch[1] ≥` that constant): console notice;
   `has_wonder(SPACEPROGRAM 542)` → `leader_flags |= REVEAL_ENEMY`; `has_wonder(STATUEOFLIBERTY 537)` and
   `C.liberty_free_upgrades` (0x534): for u = 50..401 ascending: `has_preq(u)` and (`unittypes[u].domain == 0` or
   `where == AIRBASE 447`) and `!has_tech(u)` and `type_eligible(u, 0)` → `gain_tech(u, 0, 0, 0, 1)` (thunk).
8. `had_good[g] = has_preq(g)` for g = 0..5 (before the bit).
9. **`tech.set(t)`**.
10. Console + `Game::is_game_ready`: Steam achievement if all four finals are known (presentation).
11. `leader_flags |= RECOMPUTE (0x2000000)`.
12. **Newly discovered basic goods** — only if `!(semaphore byte1 & 8)`:
    for g = 0..5: if `!had_good[g]` and `goodtypes[g].preq[0]` (raw, +0x30) `== t`:
    `amt = game.starting[g]` (Game +0x600 + 4g) × `m`, where `m = info.starting_resources2 + 1` if
    `info.game_rules == 8` and `get_team() == 0`; else `C.ctw_nomad_starting_res_x` (0xa60) if CTW and
    `leader_flags2 & 0x80`; else 1. `bucket_add(g, amt)`; then if `info.starting_resources == 8`: `bucket[g]` raw =
    0x104be (= 99999 ^ 0x8221). In stock data Knowledge and Metal have PREQ0 Classical Age and Oil Industrial Age, so
    gaining those ages pays the start amount of that resource.
    Then for g in {0, 1, 4, 5}: if `goodtypes[g].obs == t`: `while bucket[g] > 100: Leader::action_sell(g, 0)`
    (stock data: all basic `obs` = −2, never fires).
    Then if `t == CLASSICAL_AGE`, tribe bonus 5 (Greek) and `C.greek_delay_knowledge` (0x5f8):
    `bucket_add(3, game.starting[3] × m)` (same `m` rule).
13. Console, non-gov, `notify`: "research complete" message, sound `S_UNIT_UPGRADE_COMPLETE` (unit) or
    `S_RESEARCH_COMPLETE`. Console and `t == 5`: camera outdate.
14. If `T` epoch, cat 1 and `epoch[1] == C.ctw_wonder_min_civics` (**not** CTW-gated): `calc_pop_cap`,
    `calc_anti_attrition`, `calc_attrition`, `Region::fix_borders()` (called without a set-up `this`; treat as global 🟡).
15. Bonus ALLY_LOS (688) `preq[0] == t`: `ally_mask |= bit(p)` for every VALID p ≠ who with `is_ally(p)` (fog-spec §6.5).
16. EXPLORE_MAP_BONUS (690) `preq[0] == t`: `leader_flags |= EXPLORE_MAP`; console: `fog.flags |= 2`.
17. REVEAL_ENEMY_BONUS (692) `get_preq(0, who) == t`: `|= REVEAL_ENEMY`; console fog flag. (stock preq: disable)
18. ACTIVE_PING_BONUS (691): `|= ACTIVE_PING (0x2000)`; console fog flag.
19. GLOBAL_GOVERNMENT_BONUS (697) (World Government): for i = 0..L.city_mark−1 over `cities[who]`: city valid and
    `city.+0x5f != who` → `City::assimilate`. Then for p = 0..7, p ≠ who: if `!leaders[p].has_capital(&owner)` and
    (`owner == who` or `is_ally(who, owner)`): `leaders[p].defeat(1, who, 1)`.
20. ARMAGEDDON_BONUS (694): `game.armageddon = max(0, armageddon − 2)`.
21. GRANARY2..GRANARY5 (699..702), each whose `get_preq(0) == t`: `calc_pop_cap` (no break).
22. `Leader::check_transport` @6bc5f0 (TRANSPORT_BONUS → LEADER_CAN_TRANSPORT_* flags and unit_masks 0x800000).
    The flags are recomputed from the transport bonuses and docks, so a scenario's CAN_TRANSPORT file flags are
    cleared by the first call when the leader has no dock and no bonus (port observation).
23. If age: camera outdate (UI); for b = 2000..build_mark−1 alive: `max_age = max(max_age, ages)` (low byte).
24. `t == NUCLEARMISSILE` and `notify`: messages to everyone (presentation).
25. `t == MODERN_AGE` and tribe bonus 0x14 (American): for b = 2000..build_mark−1: alive, `is_active`,
    `is(AIRBASE 447)`: repeat `C.americans_free_bomber` (0x864, =1) times: if `!check_population(BOMBER 304)`
    (`pop_cap ≥ control + unittypes[304].control_cost`): `Build::train(current_upgrade(get_graft(304)))` —
    **creates units** (RNG via unit creation, §RNG).
26. **Unit branch** — if `T.is_unit_type()`: §7.2.1, then **return**.
27. **Building branch** — if `T.is_build_type()`: §7.2.2, then fall through (and return at 28).
28. If `!T.is_tech_type()`: return.
29. Epoch: `misery = 0`; military (cat 0) → `calc_pop_cap`.
30. Final and `notify`: message (presentation).
31. Gov: `gov = t`; if `T.is(GOV_CAPITALISM 628)` and `frame != 0`: `bucket_add(5, C.capitalism_oil_gift)` (0x9c4,
    =500). `notify`: messages (presentation).
32. Age: `notify` → messages, `S_AGE_ADVANCE_*`, jukebox (presentation). If `info.victory == 9` (VICTORY_TECH_RACE),
    not CTW, and `ages == info.ending_technology`: `victory(3 = VICTORY_BY_TECH_RACE, 0)`.
33. CTW and `T` epoch and `victory == 9`: message; if raw `epochs == 0x6959b` (28 epoch techs): `victory(3, 0)`.
34. Console and SEE_ENEMY_BORDERS (689) `get_preq(0) == t`: `scene.recalc_borders` (presentation).
35. GRANARY2..5: first one whose `get_preq(0) == t` → `calc_pop_cap`, break.
36. **Borders**: if any of TEMPLEBORDERS2..4 (712..714) or FORTBORDERS2..4 (721..723) has `get_preq(0) == t`, or
    `T` is a civic epoch, or (tribe bonus 0xd Russian and `T` age): `Regions::fix_all_borders` @67f7d0.
37. Civic epoch: for each own city (i < city_mark, valid): `Wall::mask_city(center, div3[x>>6], div3[y>>6], 1,
    CityData::get_radius())` then `City::find_buildings` (city radius grows with civic level; world-spec).
38. **Global building upgrades / city upgrades**: for b = 414..542 ascending with `has_preq(b)`: for slot i in
    `0..num_preq(b)-1`: if `get_preq(b, i, who) == t`: { if `buildtypes[b].is(TOWN 415)`: `City::check_upgrade` on
    every valid own city (ascending); if `build_flags & 4` and `type_eligible(b, 1)`: `gain_tech(b, 0, 0, 0, 1)`;
    break }.
39. **No-research units**: for u = 50..401: (`get_preq(u,0) == t` or `get_preq(u,1) == t`) and `has_preq(u)` and
    `!has_tech(u)` and `unit_flags & 0x80` (flag h) and `!has_objmask(u, 0x4000000)` (OBJ_MASK '1') and
    `type_eligible(u, 1)` → `gain_tech(u, 0, 0, 0, 1)`.
40. **Free-upgrade loops** (table 7.1.2), in that order.
41. `leader_flags |= CALC_UNIT_STATS | CALC_WALL_STATS` (consumed next `Leaders::process_all`, frame-spec §4 item 3, §7.7).
42. If `rush_end`: message + `S_WAR_WARNING` (presentation); if `Game::teams_locked()`: for p = 0..7 VALID:
    `leaders[p].attrition_stamp (+0x1f4) = 0`; if `!is_neutral(p)`: for q = 0..7 VALID, `!is_neutral(q)`, q ≠ p,
    `!is_ally(p,q)` and `!is_enemy(p,q)`: `leaders[p].set_diplo(q, 0)` (war) and a chat line (presentation).
43. Age: `scene.recalc_builds` (UI); for o = 0..unit_mark−1 alive: `Unit::update_gpiece` (sim: crew track offsets,
    anim-spec §2.1) then `Unit::set_new_location(x, y, 1, 1)` at its own position (no cell change: re-places the guys
    in formation slots with teleport; movement-spec); then for b = 2000..build_mark−1 and w = 3000..wall_mark−1 alive:
    `Wall::update_gpiece` (presentation 🟡).
44. `TerrainOil::gain_tech` @8a2e40 (uses the console leader; terrain patch visuals → presentation).

#### 7.1.2 Free-upgrade loops (step 40)

Two patterns. **P(range[, cond])**: for x in range ascending: if `has_preq(x)` [and cond]: for slot i: if
`get_preq(x, i, who) == t`: `if type_eligible(x, 1): gain_tech(x, 0,0,0,1)`; break.
**U(range)**: for u in range: if `get_preq(u,0) == t` then `q = get_preq(u,1)`, else if `get_preq(u,1) == t` then
`q = get_preq(u,0)`, else skip; if `has_tech(q)` and `type_eligible(u, 1)` → gain. (No `has_preq(u)` test.)

| # | Gate (tribe bonus n / wonder, constant ≠ 0) | Pattern |
|---|---|---|
| 1 | tribe 0xe (Chinese), `chinese_herbal_lore` | P(603..605 Herbal Lore line) |
| 2 | wonder REDFORT 534, `red_fort_fortification` | P(593..595) |
| 3 | REDFORT, `red_fort_tactics` | P(583..585) |
| 4 | tribe 0x13 (Lakota), `lakota_cav_upgrades` | U(187..208 HORSEARCHERS..) then U(216..223 ARMOREDCAR..) |
| 5 | tribe 0x12 (Iroquois), `iroquois_scout_upgrades` | U(69..81 SCOUT..) |
| 6 | tribe 0x15 (Indians), `indians_elephant_upgrades` | U(261..264 HVYELEPHANT..) |
| 7 | tribe 0x10 (Korean), `korean_militia_upgrades` | U(66..68 MILITIA..) |
| 8 | tribe 0x10, `korean_temple_upgrades` | P(586..588) |
| 9 | tribe 0x10, `korean_temple_tax_upgrades` | P(589..592) |
| 10 | tribe 0x17 (Persian), `persians_taxation` | P(589..592) |
| 11 | tribe 0x11 (Mongol), `mongol_free_forage` | P(606..608) |
| 12 | tribe 0xd (Russian), `russian_attrition_upgrades` | P(596..599) |
| 13 | tribe 7 (Egyptian), `egyptian_granary_upgrades` | P(612..614) |
| 14 | tribe 6 (Roman), `roman_fort_upgrades` | P(544..628, cond `where == FORTX 443` and `!has_tech(x)`) |
| 15 | wonder HANGINGGARDENS 528, `hanging_gardens_upgrades` | P(609..611), P(620..622), P(612..614) |
| 16 | tribe 10 (French), `french_lumbermill_upgrades` | P(609..611) |
| 17 | tribe 0xe, `chinese_knowledge_upgrades` | P(615..619) |
| 18 | tribe 0xc (German), `german_metal_upgrades` | P(620..622) |
| 19 | tribe 0xc, `german_industry_upgrades` | P(620..622), P(609..611), P(612..614) |
| 20 | tribe 0xc, `german_heavy_infantry` | P(50..401, cond `where == BARRACKS 427` and (`is(153)` or `is(132 HOPLITES)`)) |
| 21 | tribe 0xc, `german_light_cavalry` (**missing in XML → −1 → enabled**) | P(50..401, `where == 427` and `is(209 HORSE)`) |
| 22 | tribe 0xb (British), `british_archer_upgrades` | P(50..401, `where == 427` and `is(170 BOWMEN)`) |
| 23 | tribe 9 (Spanish), `spanish_scout_upgrades` | P(50..401, `is(69 SCOUT)`) |
| 24 | tribe 8 (Turk), `turk_free_siege_upgrades` | P(50..401, `is(265 CATAPULT)`) |
| 25 | wonder STATUEOFLIBERTY 537, `liberty_free_upgrades` | units 50..401 with `has_preq(u)` and (domain 0 or where AIRBASE): first slot i with `p = get_preq(u,i) ≥ 0`, `p' = is_unit(p) ? get_graft(p) : p`, `p' == get_graft(t)` → `if type_eligible(u, 0)` gain; break |
| 26 | `colosseum_fort_upgrades` and wonder COLOSSEUM 530 | P(593..595) |
| 27 | wonder TIKAL 532, `tikal_temple_upgrades` | P(586..588) |

`is(n)` = `ObjectTypeData::is(n, 0)` (vtable +0x60). Constants: rules-spec §12.6 (all in `Constants.v` by offset).

### 7.2 Unit and building type rewriting (who / when / which) ✅

#### 7.2.1 Unit upgrade gained (step 26)

Reached when a unit type `t` is gained: research of an upgrade at a building (`Build::finished`, only when `t`'s tech
bit is not yet set — if it is set the same queue item *trains* the unit instead), free grants, scenario.
1. `T.is(MILITIA 66)` → `leader_flags |= CALC_UNIT_STATS`.
2. If `upgrade_existing`:
   - For o = 0..unit_mark−1 over `units[who]` (ascending; **includes garrisoned/transported units**, only the VALID
     flag is tested):
     - if `T.is(FIGHTERBOMBER 308)` and the unit `is(AIRCRAFTCARRIER 351)`: `track_queued(old_fb, −unit.num_queued)`
       (+0xa0), `track_queued(t, +unit.num_queued)` (carrier's queued planes move to the new type); no set_type.
     - else with `old = unit.type`: **match** if `old == get_graft(T.from)` (+0x3c) or the jump chain reaches t:
       `j = get_graft(unittypes[old].jump)`; `while j ≥ 0 and j != t: j = get_graft(unittypes[j].jump)`; match iff
       `j == t`. On match: if `T.uber_size < old.uber_size` (+0x308): if `T.uber_size == 1`: non-captain
       (`is_captain` vslot 0xe8 false) → `unit.die(0, −1, 0)` and **skip** set_type; captain →
       `unit.damage (+0x24) = UnitData::total_damage(1, 0)`; (other sizes: debug error, continue). Then
       `unit.set_type(t, 0)` (vslot 0x84 → `Unit::set_type` @612fa0, §6).
   - Queues: for b = 2000..build_mark−1 alive, q = 0..queued−1: `v = own queue[q]`; if `types[v].is_unit_type()` and
     v matches by the same predicate: `track_queued(v, −1)`, `BuildQueue::set_queue(q, t, 0, 1)` (§6),
     `track_queued(t, +1)`.
3. If `!has_objmask(t, 0x4000000)`: for u = 50..401 matching the predicate (u in place of `old`): `tech.set(u)`,
   `obs_flags.set(u)` (predecessors become known and obsolete).
4. Return (no CALC flags, no tech tail).

`die(0, …)` (death type 0) makes no death-animation draws (combat-spec §6.2). Who calls `Unit::set_type` (Callers):
`gain_tech` (via vtable), `Build::finished` (gov hero swap at a Senate), spells (`cast_civilian/to_arms/immortal_*`,
`Unit::do_cast`), `Object::eject_contents`.

#### 7.2.2 Building upgrade gained (step 27)

Reached for global-upgrade buildings (`build_flags & 4`, letter c): from step 38 when the last preq arrives, from
`Build::finished`, scenario. Stock chains: Stable→Auto Plant, Siege Factory→Factory, Dock→Anchorage→Shipyard,
Tower→Keep→Stockade→Bunker, Fort→Castle→Fortress→Redoubt, Lookout→Observation Post→Air Defense Gun→Radar Air
Defense→SAM. (Small→Large→Major City and Forbidden City have no bit 4: they are per-building upgrades done in
`Build::finished` with `set_type` + `mask_me`, or by `City::check_upgrade`.)
1. Buildings o = 2000..build_mark−1, then walls o = 3000..wall_mark−1 (start from `obj_base[1..2]`, ends from the
   mark pointer table at objects +0x1ec 🟡): `w = get_wall(o)`; if alive and `w.type.upgrade (+0x44) == t`:
   `obs_flags.set(w.type)`, `w.set_type(t, 0)` (`Wall::set_type` @640da0: `decrement_stats`, `SubObject::set_type`,
   `increment_stats`). **No `mask_me`** here (unlike `Build::finished`).
2. `buildtypes[t].from ≥ 0` → `obs_flags.set(from)`.
3. Console: placement cursor `drop_type` follows the upgrade (UI).
Then steps 28.. return (not a tech). Multi-step chains resolve in step 38's ascending order (Keep 440 before Stockade
441), each `gain_tech` converting the previous result.

`City::check_upgrade` @738b20 (step 38, also from `City::find_buildings`, `Build::activate`): if
`CityData::ready_to_upgrade`: centre `set_type(upgrades_to(), 0)` (vslot 0x80), `mask_me(1, REGEN_NONE)`,
`CALC_WALL_STATS`, `find_buildings`, pop bookkeeping (`L.pop +0x95c`, `game.world_pop`, `reg_pop[region]`),
`calc_pop_cap`, `Region::fix_borders`, message (console). 🟡 `ready_to_upgrade`/`upgrades_to` belong to the economy part.

### 7.3 Ages ✅

#### 7.3.1 What an age is
An age tech `544+k` (k = 0..6) is researched at the Library (WHERE = Library; queued at the first library, §7.10).
Gaining it does, in `gain_tech` order: `ages++` and `age_stamp` (6), starting resources of goods whose PREQ0 is that
age (12; Knowledge+Metal at Classical, Oil at Industrial), Greek knowledge (12), building `max_age` raise (23),
American free bombers at Modern (25), tech-race victory (32), Russian borders refresh (36), auto-gained units/buildings
whose preq is that age (38, 39: e.g. global building upgrades with an age preq, Large City check_upgrade at Medieval),
tribe/wonder loops (40), CALC flags (41), rush peace end (42), every unit `update_gpiece` + guy re-placement and every
building `update_gpiece` (43).

**Correction to the brief:** `Leader::set_age` @6d25a0 is *not* part of a normal age-up. It is only called by
`UnitBalance::next`, `ScenarioFuncSet::gain_next_age/lose_current_age` and two unnamed sites (007e09d8, 007e0c97).
There is no separate "building max_age / pop cap / city limit / unit upgrade" pass on age-up: pop cap is not age-based
(military epochs, §7.4), the city limit is civic-based, and unit upgrades are explicit researches.

#### 7.3.2 `Leader::set_age(n)` @006d25a0 (scenario/balance)
`n` clamped to 0..7. For age techs from INFORMATION (550) down to `544+n` that are known → `lose_tech`. For ages
`544..543+n` not known → `gain_tech` (thunk: `notify = 0`, `upgrade_existing = 1`). Then every non-age tech 544..628
known whose `has_preq` is now false → `lose_tech`; every unit 50..401 and building 414..542 with the tech bit and
`!has_preq` → `lose_tech`; `reset_obs_flags`, `calc_unit_stats`, `calc_wall_stats` (direct), camera outdate (UI).
`Leader::set_epoch(cat, n)` @6d26f0 is the same for one epoch line (`get_epoch_base(cat)`, n clamped 0..7).

#### 7.3.3 Effects on other players
None directly. Readers of other leaders' ages: `Game::current_age` @5946d0 (max `ages` over VALID leaders; →
`Game::war_allowed` for rush rules), `Leaders::max_age` @6b8370 (max over ACTIVE leaders, initial −1; AI
`Leader::create_units`), `max_human_age`, `max_enemy_age`, `best_human_age_stamp` (AI). The rush-rule end (step 42)
and World Government defeats (step 19) are the only cross-player state changes in `gain_tech`.
`Game::war_allowed`: `rush_rules == 0` → 1; else 0 while `current_age < rush_rules` and (DL < 9 ❓ or
`frame < rush_rules_table[rush_rules].+0x3c × 900`), else 1.

#### 7.3.4 Tech-race shortcut in `Build::queue_up`
When an age is queued with `victory == 9`, `ages + 1 == ending_technology` and `!war_allowed()`:
`gain_tech(t, −1, −1, 1, 1)` immediately and return 0 (§6 owns the rest of queue_up).

Age-up message/sound/jukebox: presentation only.

### 7.4 Epoch (library) lines ✅

Gaining an epoch tech changes `epochs`, `epoch[cat]` (step 6) and `misery = 0` (29). Every effect is then derived on
demand from the counters:

| Line | Counter | Consumers |
|---|---|---|
| military (cat 0) | `epoch[0]` | `Leader::calc_pop_cap` @6dc490 `pop_cap = C.pop_cap[epoch[0]]` (+ city caps, Bantu, VR, Colossus…; called in step 29); unit preq slot 1 remap (§7.9.3) |
| civic (cat 1) | `epoch[1]` | `LeaderData::get_city_limit` @6d6130 `= epoch[1] + 1` (+ `bantu_city_limit` when epoch[1] ≠ 0, + `pyramids_city_limit`); territory: steps 36–37 (`fix_all_borders`, `mask_city` with the grown radius); CTW wonders (7, 14, `has_wonder`) |
| commerce (cat 2) | `epoch[2]` | `Leader::calc_resource_caps` @6ce900 (called every frame from `Leader::gather`, economy-spec): `resource_cap[g] = C.commerce_cap[epoch[2]]` with tribe/wonder/Republic modifiers, `+bonus_cap[g]`, clamp 0..999, then `<< 4`; VR bonus → 0x1166 raw (unlimited) |
| science (cat 3) | `epoch[3]` | research cost discount (`TypeData::get_cost`, §2; `tech_science_discount` per science tech above the tech's age) and the refund of step 3 |

`epochs` gates ages (`has_preq`, §7.9.2). `discovered` feeds the score (not specified here).

### 7.5 Governments ✅
WHERE = Senate. `has_preq` rules (§7.9.2): tier k ≥ 1 (625..628) needs any government of the previous tier;
`type_eligible`: the pair partner (`(t−623 ^ 1) + 623`) must be neither known nor queued (`has_tech_queued`);
`tech_avail` also excludes a partner being researched. Gaining one: `discovered++` (6), `gov = t` (31),
Capitalism oil gift (31), `gov_hero_frame = 1` at frame 0 (2). The gov hero itself is created by `Build::finished`
at the Senate (`get_gov_hero`; `train` if `gov_hero_frame < 0`, else `set_type` of the existing hero; §6).
Government bonuses (794..805, `REPUBLIC_n` etc.) come from `has_preq` with the "govs taken" rule (§7.9.2).

### 7.6 Final techs ✅
579..582, age 7. `has_preq` short-circuits: Information Age + all four last epoch techs (COMPUTERIZATION,
GLOBALIZATION, INTERNATIONAL_LAW, SELECTIVE_SERVICE), no preq-slot loop. `type_eligible` disables them when
`info.game_rules == 11` or `info.flags` bit 31. Effects come through their bonuses: MISSILE_DEFENSE (693),
ARMAGEDDON (694, step 20), VIRTUAL_REALITY (695: unlimited resource caps and pop cap = limit), INSTANT_UNIT (696,
train_time), GLOBAL_GOVERNMENT (697, step 19), STEALTH_STUFF (790).

### 7.7 Recalculation flags ✅
Set: step 41 (every tech except unit types) and `lose_tech` (`|= 0xc000000`); `CALC_UNIT_STATS` alone for militia-line
units (step 26); `CALC_WALL_STATS` by `Build::finished`/`City::check_upgrade`/`Build::close`. Consumed in
`Leaders::process_all` @6ed2a0 after `Leader::gather` (frame-spec §4 item 3): wall flag first (clear, then call), then
unit flag.
- `Leader::calc_wall_stats` @6cf7c0: buildings 2000..build_mark−1 then walls 3000..wall_mark−1, alive: if
  `!is_active` → `Wall::update_construct_time`; `update_hits(0)` (vslot 0x15c); `update_los` (vslot 0x160).
- `Leader::calc_unit_stats` @6cf970: `calc_attrition`, `calc_anti_attrition`; units 0..unit_mark−1 alive:
  `update_los`; if `is_captain`: `update_hits(0)`, `Unit::update_speed`, `Unit::update_armor`.
No RNG in either (none of the callees is a game_random caller).

### 7.8 Losing techs, cheats ✅

#### `Leader::lose_tech(t)` @006d2850 (thunk @0047df20)
Callers: `set_age`, `set_epoch`, `ScenarioFuncSet::lose_tech/lose_upgrade`, Conquest, editor.
- Unit type: if `has_tech(t)`: `discovered--`; clear bit.
- Tech: clear bit; then for every tech T2 in 544..628 with its bit set (ascending): if not both ages:
  `techtypes[t].is_ultimate_preq(T2)` @66c410 (t is a recursive effective preq of T2, leader −1 get_preq rules) →
  `lose_tech(T2)` (recursive); if both ages: `techtypes[t].age ≤ T2 − 544` → `lose_tech(T2)`. Then
  `fix_tech_flags`. Non-age: epoch → `epochs--`, `epoch[cat] = compute_epoch(cat)` @6d6f80 (count of consecutive
  known techs from the line base); else `discovered--`. Age: camera (UI); `ages` recomputed as the count of known
  544..550; console `age_update`; every alive building `max_age = ages` (exact, not max). `TerrainOil::lose_tech` (UI).
- Buildings/others: nothing but the common tail.
- Tail: `reset_obs_flags`, `leader_flags |= CALC_UNIT_STATS | CALC_WALL_STATS`.

#### `Leader::fix_tech_flags` @006d2480
`Regions::fix_all_borders`; `scene.recalc_borders` (UI); `ally_mask = bit(who)` plus every VALID ally if
`has_tech(ALLY_LOS.preq0)`; EXPLORE_MAP set/clear by `has_tech(690.preq0)`; REVEAL_ENEMY set if
`has_tech(692.preq0)` or `has_wonder(SPACEPROGRAM)`, else cleared; ACTIVE_PING by 691; `check_transport`.
Callers: `lose_tech`, `ScenarioEditor::run_test`.

#### `Leader::reset_obs_flags` @006e32a0
Clear `obs_flags`; buildings b with `build_flags & 4`, `has_preq(b)` and `from ≥ 0` → set `from`; for every unit
u0 with its tech bit: every unit u with `get_graft(u.jump) == u0` → set u; `get_graft(u0.from) ≥ 0` → set it.
Callers: `Leader::init`, `lose_tech`, `set_age`, `set_epoch`, `Setup::build_game`.

#### Cheats (opcodes 60/61, replay-spec table)
`Game::action_cheat_give_techs(who)` @593180 (from `CommandPackage::process_cheat_give_techs`): sets **every** bit
0..805 of `leaders[who].tech` directly (writes `flags = 0` each step) — **no** `gain_tech`, no counters, no effects;
then `action_cheat_warning`. `action_cheat_zero_techs` @593120: clears every bit (`flags` → 2 if 0), warning. No RNG.

### 7.9 Availability ✅

#### 7.9.1 `LeaderData::has_tech(t)` @006e0c80
`t == −1` → 1; `t == −2` → 0; `t < 50` → 1 (goods; also −3 and other negatives). Unit types (50..413 by the inlined
range or vslot): `has_preq(t)` false → 0, else the tech bit. Build types: `has_preq(t)`. Everything else: the tech bit.
`has_upgrade(t)` @006e10e0 = the raw bit (no preq check). `has_bonus(b)` @006e0b80 = `BUY_SELL` with tribe bonus 4 →
1, else `has_preq(b)`.

#### 7.9.2 `LeaderData::has_preq(t)` @006db810
1. Tribe early unlocks (each returns 1 when the condition holds, else continues): MARKET: tribe 4;
   KNOWLEDGE / UNIVERSITY: tribe 5 and `greek_knowledge_early` / `greek_university_early`; TOWER, FORTX: tribe 6 and
   `roman_fort_early`; GRANARY: tribe 7 and `egyptian_granary_early`, then German rule; LUMBERMILL: tribe 10 and
   `french_lumbermill_early`, then German rule; REFINERY: tribe 0xd → `has_preq(OILWELL)` decides (returns 1 if it is
   true, continues otherwise); METAL, MINE, SMELTER: tribe 0xc and `german_metal_early` → 1; SMELTER additionally
   tribe 0xc and `german_buildings_early`; TEMPLE: tribe 0x10 and `korean_temple_upgrades`; German rule = tribe 0xc
   and `german_buildings_early`.
2. Finals (`t − 579 < 4` unsigned): return `has_tech` of INFORMATION_AGE, COMPUTERIZATION, GLOBALIZATION,
   INTERNATIONAL_LAW, SELECTIVE_SERVICE (all), skipping 3–5.
3. For slot i in `0..num_preq(t)-1`: `p = get_preq(t, i, who)` (§7.9.3).
   - `p < 0`: require `has_tech(p)` (−1 ok, −2 "disable" fails).
   - `special_preq(t, &p)` @6d7030 true → slot satisfied (it may also rewrite p: see below).
   - `p` a bonus (684..805): `q = get_preq(p, i, who)` (**same slot index i**); if `q < 0`: require
     `get_govs_taken() ≥ 2`, and if `p.preq[0]` is MONARCHY/DEMOCRACY (625/626) require SOCIALISM or CAPITALISM known;
     if `q` is a bonus: require `govs_taken ≥ 3`. Then `p == BUY_SELL` with tribe 4 → ok, else require `has_preq(p)`.
   - else require `has_tech(p)`.
   (`get_govs_taken` @6d69f0 = known techs among 623..628.)
4. Age types: require `epochs ≥ techs_per_age(t)`.
5. Govs ≥ 625: require `has_tech(t − 2)` or `has_tech(((t − 623) ^ 1) + 621)` (any gov of the previous tier).
`special_preq(t, &p)`: returns 1 for Roman fort-line buildings (`t − 439 < 8` unsigned, tribe 6,
`roman_fort_early`, p not an age), and for Bantu (tribe 3) units that are neither `is(227,1)` nor `is(187,1)` when p
is a military epoch. Otherwise returns 0 but may rewrite p: wonders with an age preq and tribe 7 +
`egyptian_wonders_early` → p − 1 (Classical → −1); techs 609..611, 612..614, 620..622 with tribe 0xc +
`german_industry_early` and p a science epoch → p − 1 (p ≤ 551 → −1).

#### 7.9.3 `TypeData::get_preq(o, i, leader)` @00668700 — runtime differences
Same as rules-spec §9.3 (which describes the load-time call with leader −1), plus:
- It reads the **current** `GameInfo` (`starting_technology[2]`, `ending_technology`, `game_rules`), so effective
  preqs depend on the game's settings at runtime: a port must evaluate it live, not cache the load-time result.
- Slot 1, leader ≥ 0, `preq[1]` an age, `o` a government (vslot +0x2c), leader has tribe bonus 0x12 (Iroquois) and
  `iroquois_govs_early`: p = `preq[1] − 1` if that is still an age, else −1 (**correction:** the preq moves one age
  earlier; it is only dropped for a Classical preq). The start/end remap then runs on the new p.
- Slot 2 compares against `get_preq(o, 1, −1)` (leader −1) as at load time.

#### 7.9.4 `type_eligible(t, check)` @006dbd10 — tech side (§6 owns the queue use)
`tribe_can_type(t) != 4` → 0. TOWN with no Large City (incl. graft) built or queued → 4 immediately. Slots: any
`get_preq < −1` → 0; a preq p that is neither a plain tech nor an epoch with `ending_technology < techtypes[p].age` →
0. Goods: `check` and `has_tech(obs)` → 0. Units: NUCLEARMISSILE/ICBM with `game_rules == 11` → 0; with `check`:
`has_tech(obs)` → 0, obs_flags bit → 0, jump chain (graft) entries j with `has_preq(j)`: tech bit j → 0; j flagged
0x200 ('j') while t's bit is clear and t lacks 0x80 → 0. Raw `preq[0..2]` ages beyond `ending_technology` → 0.
Flag 0x200 → 4; else `check_predecessor(get_graft(from))` → 4/0. Buildings: with `check`, `has_tech(obs)` or
obs_flags bit → 0. Techs 544..628: `leader_off` bit `who` → 0; finals disabled (game_rules 11 / flags bit 31); ages
with `ending_technology < age + 1` → 0; gov partner known or queued → 0; else 4.

#### 7.9.5 `type_avail(t, check)` @006e33a0 — tech side
`!has_preq` → 0; `type_eligible != 4` → that value. Units: gov-hero types with `leader_flags2 & 0x1000` → 0; tech
bit clear → **2** (= must be researched/upgraded); `is(MACHINEGUN 123)` needs the bit of grafted 100/102/104 (else 0);
→ 4. Buildings: built wonder → 0; `from ≥ 0`, `build_flags & 4`, bit clear → **2**; Senate rules (§6); → 4. Govs:
`leader_flags2 & 0x800` → 0. Return codes: 0 no, 2 researchable upgrade, 4 available.

#### 7.9.6 Helpers
- `Leader::tech_avail(t, _)` @006da060 (only `Leader::research_techs`, AI): 0 if (tech with `leader_off` bit) or
  `has_tech(t)` or `researching(t, −1, 0, 0)`; govs: 0 if the partner is known or researching; else
  `type_avail(t, 1)`.
- `LeaderData::techs_per_age(age_tech)` @006d7280: `s = (game_rules == 8) ? starting_technology : starting_age()`,
  `e = ending_technology`, `a = techtypes[t].age`. If `s == 0 && e > 6`: `4a + 2` (2, 6, 10, …, 26). Else
  `n = e − s + 1`; `n == 0` → 0; `r = (a − s + 1)·trunc(28 / n) − 2`; if `s == 0 && a == 0 && r == 1` → 2; return r.
- `starting_age()` @006d7320: `game_rules == 8` and team 0: `min(min(st,7) + min(st2,7), e)`; else `min(st, 7)`.
- `all_techs()` @006d7370: `starting_technology == 8` (or `st2 == 8` for game_rules 8, team 0).
- `has_tech_queued(t)` @006d73b0: tech 544..628 with bit clear: any active building (flags & 4) that
  `is(techtypes[t].where)` and whose vslot 0x190(1, t) reports it queued 🟡.
- `researching(t, bld, only_bld, exact)` @006db510: buildings with `build_flags & 4 == 0` force `only_bld = 1`. For
  b = 2000.. active (and `b == bld` if only_bld): any own queue slot v with `v == t`, or (`!exact`, both unit types,
  neither has its bit, `types[t].is(v, 0)`) → 1.
- `current_upgrade(t)` @006e3140 (thunk @006e1360): loop: if t is a unit/building and `j = graft(t.jump) ≥ 0` with
  `has_preq(j)` and bit j → t = j, continue. Else `u = graft(t.upgrade)`; `u < 0` → return t; if
  `type_avail(u, 1) < 3`: `v = current_upgrade(u)`; return `(v < 0 || v == u) ? t : v`; else t = u, continue.
  (Name-wise "the best type of this line the leader can field".)

#### 7.9.7 Starting techs (`Leader::init` @006e3930, tech part)
`ages = starting_age()`, `discovered = 0`, tech and tech_at_start initialised, `epochs = 0`. For x = 544..628: if
(`all_techs()` or `age(x) < start`) and not final and not gov: if not `all_techs()`: plain techs are skipped when any
effective preq is `< −1` or an epoch; epochs are skipped unless (`game_rules == 8`, team 0 and `age ≤ start`); ages
pass. Passing x: set bit in tech and tech_at_start; epoch → `epochs++`, plain → `discovered++` (ages do not change
the counter here). Then `epoch[c] = compute_epoch(c)`; buildings with `type_avail ≠ 0` get their bit; units with
`has_preq` and `tribe_can_type` get bit + tech_at_start unless (not all_techs and the graft of `from` has an age preq
equal to `start − 1` (or `start` for game_rules 4/11)); spells with `type_avail` (and preq ≠ −1 or not spell flag 1);
`reset_obs_flags`; then (not CTW) tribe free techs via `gain_tech`: Dutch next commerce, Russian next civic, Aztec and
Roman next military, American next science (each if that line < 7), Persian DESPOTISM. RNG draws in `Leader::init`
(810, 820) belong to the personality/AI part, not to techs.

### 7.10 Libraries ✅

- `LeaderData::get_first_library()` @006db6c0: lowest o in 2000..build_mark−1 that is alive, `is_active`, has
  `city ≥ 0` (+0x72), `!BuildData::is_unassimilated`, and `is(LIBRARY 435)`; else −1.
- **Redirect:** `Build::queue_up` at any building that `is(LIBRARY)` (any type except DISBAND) forwards to the first
  library (returns 1 if there is none); `BuildData::get_queue` @62d280 at a non-first library maps indices past its own
  queue into the first library's queue (UI listing). So every library tech (all ages, epochs, finals) of a leader lives
  in one queue. Other techs (WHERE Temple/Fort/Tower/Granary/Lumber Mill/Smelter/University/Senate) are queued locally.
- `Build::new_library()` @00627fe0 (from `Build::activate`, `City::find_buildings`, `City::assimilate`,
  `Wall::swap_team`): if this building `is(LIBRARY)`, `first = get_first_library()`, `first ≥ 0` and `this.o < first`
  (this one will become the first): copy the first library's `queued` count and all 0x14-byte QueueItems
  (`job_counter`, type, goods, paid costs — progress kept) into this queue; old `queued = 0`.
- **Library destroyed** (`Build::close` @628980): if the closing building is a library and was the first library
  with items queued: `next = get_first_library()`; none → `clean_queue(this, 1)` (refunds, §6); else append every
  item to `next`'s queue after its existing items (`next.queued += n`); a DISBAND (666) at index 0 stays behind
  (this `queued = 1`), otherwise this `queued = 0`. The in-progress item keeps its `job_counter` but now waits behind
  the next library's items. 🟡 capacity handling of the destination array (§6).

### 7.11 Tech bonuses ✅ (getters) / 🟡 (consumer list from decompile greps)

`has_bonus(b)` = `has_preq(b)` (plus BUY_SELL/tribe 4). Bonus preqs (rules.xml): mostly one tech in `preq0`;
gov bonuses use `preq1` = previous bonus (rules-spec §9.7) and the govs-taken rule (§7.9.2). Count getters (each
counts `has_bonus` over a 3-entry family; BUY_SELL special inlined; result 0..3):

| Getter | Family | Consumers |
|---|---|---|
| `get_troops_speed_upgrade` @6da850 | TROOPS_FASTER_1..3 (742..744; Herbal Lore line) | `ObjectData::train_time` |
| `get_troops_los_upgrade` @6e1110 | TROOPS_LOS_1..3 (745..747) | `Unit::update_los` |
| `get_ships_speed_upgrade` @6da800 | SHIPS_FASTER_1..3 (748..750) | `ObjectData::train_time` |
| `get_building_speed_upgrade` @6dae90 | BUILDINGS_FASTER_1..3 (754..756) | `Wall::update_construct_time` |
| `get_building_hp_upgrade` @6daee0 | BUILDINGS_HP_1..3 (757..759) | `Wall::update_hits` |
| `get_vehicle_speed_upgrade` @6da8a0 | VEHICLES_FASTER_1..3 (760..762) | `ObjectData::train_time` |
| `get_supply_upgrade` @6e0880 | SUPPLY_WAGONS_1..3 (763..765) | `Unit::update_speed/update_hits`, `UnitData::mana`, `SupplyData::get_radius` |
| `get_general_upgrade` @6e0830 | GENERALS_UPGRADE_1..3 (773..775) | `Unit::update_speed/los/hits/process`, `UnitData::armor`, `HeroData::get_radius`, decoy, march/ambush casts |
| `get_spy_upgrade` @6e1090 | SPIES_UPGRADE_1..3 (776..778) | `Unit::update_speed/los/hits`, `SpellTypeData::get_range` |

Other families and where they are read (by name in decompiles): FOREIGN_TRADE/BUY_SELL (`can_buy_sell`, market),
TRANSPORT_BONUS (`check_transport`), COLONIZE_BONUS (`BuildTypeData::blocked_location`), MISSILE_DEFENSE
(`Ammo::do_damage`, `Object::do_launch`, `Build::train`), VIRTUAL_REALITY (`calc_resource_bonuses/caps`,
`calc_pop_cap`), INSTANT_UNIT and RESEARCH_FASTER and SPIES_GENERALS_CREATED_FASTER (`train_time`), CARAVAN_ATTACK
(`ObjectData::attack`), GRANARY/FISHERMEN/LUMBERMILL/SMELTER (`get_granary/fishermen/lumbermill/smelter`,
`CityData::lumber_level`, `calc_rare`), TEMPLEBORDERS/FORTBORDERS (`get_temple_borders/fort_borders`,
`World::compute_reg_territory`), FORTRANGE (`get_building_range`), FORTGARRISON (`get_garrison_limit`), FORTLOS
(`get_fort_los`), ATTRITION (`calc_attrition`), FORAGING (`calc_anti_attrition`, `UnitData::get_attrition`),
UNIVERSITY (`get_university`), SPIES_GENERALS_RECOVER_CRAFT (`Unit::process`), SPIES_GENERALS_CHEAPER and
DESPOTISM/MONARCHY/DEMOCRACY/SOCIALISM (`TypeData::get_cost`), SPIES_CRAFT_FASTER (`SpellTypeData::get_job_time`),
TAX (`get_taxation`), MERCHANTS (`get_merchants_level`), BUILDINGS_CREATED_FASTER (`update_construct_time`),
REPUBLIC (`calc_resource_caps`), CAPITALISM (`calc_gather`), MONARCHY/SOCIALISM (`train_time`). ❓ not found by name
(probably indexed as `base + k`): TEMPLEHP, TEMPLERANGE, HEAL_FASTER (`get_heal_level` @6e12e0 likely), TROOPS_FASTER
789, STEALTH_STUFF, TOWER_FORT_RANGE.

#### Port status (engine/sim)
Loaded (`rules.hpp` `Type`, `rules_techs.cpp`): all tech fields (preq, where, cat, age, costs, job/res_time,
tribe_mask, show, leader_off, ai, alternate names), bonuses (preq[0..1]), goods preq/obs, unit from/upgrade/jump/obs,
uber_size, unit_flags, building upgrade/from/build_flags, `get_preq` with a fixed `GameSetup` (load-time only), and
every Constant by offset. Missing for a port: the whole per-leader state of §7.0 (tech/tech_at_start/obs_flags
bitmasks, the four encrypted counters, age_stamp, gov, gov_hero_frame, misery), `BuildData.max_age`, `game.starting[6]`
and the GameInfo fields (`rush_rules`, `victory`, `starting_resources[2]`, `game_rules`, `starting/ending_technology`,
`info.flags` bit 31) at runtime, and the functions `has_tech`, `has_preq`, `special_preq`, runtime `get_preq`,
`type_eligible`/`type_avail`, `current_upgrade`, `gain_tech`, `lose_tech`, the bonus getters. `game.hpp` `Leader` has
only `age`.

### 7.12 RNG draws

`gain_tech`, `lose_tech`, `set_age`, `fix_tech_flags`, `reset_obs_flags`, `calc_unit_stats`, `calc_wall_stats`,
`has_preq`, `type_eligible`, `current_upgrade`, `refund_cost`, `new_library`, the cheats: **no direct
game_random calls** (none is in the game_random caller list; decompiles checked). Indirect draws only through callees,
at these points and in this order within one `gain_tech`:
1. Step 19 (World Government): `City::assimilate` may `Build::train` a gov hero → unit creation draws
   (`Guy::init_real` idle draws, anim-spec/frame-spec §8); `Leader::defeat` ❓ (no direct draws seen).
2. Step 25 (American Modern Age): `Build::train` of each free bomber → unit creation draws, per airbase in o order.
3. Step 26: `Unit::set_type` (§6.8.1: one `Guy::init_real` draw per kept squad guy and per crew guy, per converted
   unit in slot order); `Object::die(0, −1, 0)` — death type 0, no
   death-animation draw (combat-spec §6.2).
4. Recursive `gain_tech` calls (steps 5, 7, 38, 39, 40) inherit 1–3.
Iteration orders that matter: units by o ascending, buildings 2000.. then walls 3000.., cities by list index,
building types 414→542 then unit types 50→401 then the 27 loops in table order; leaders 0..7.

### 7.13 Rules/data read

Type fields: TypeData `type` +0x04, `cat` +0x14, `costs`, `preq[3]` +0x30, `from` +0x3c, `where` +0x40, `upgrade`
+0x44, `jump` +0x48, `obs` +0x4c; TechTypeData `age` +0x1c8, `leader_off` +0x1e2; UnitTypeData `domain` +0x218,
`unit_flags` +0x2b4 (0x80 h, 0x200 j), `control_cost` +0x2f0, `uber_size` +0x308; ObjectTypeData `obj_masks`
(has_objmask 0x4000000 = '1'), is-lists; BuildTypeData `build_flags` +0x2c0 (bit 2). All loaded in `rules.hpp`.
Constants (offset): tech_science_discount 0x364, pop_cap[8] 0x3c4, commerce_cap[8] 0x400, liberty_free_upgrades
0x534, greek_delay_knowledge 0x5f8, americans_free_bomber 0x864, capitalism_oil_gift 0x9c4, ctw_nomad_starting_res_x
0xa60, ctw_wonder_min_civics 0xad4, plus the tribe/wonder switches of table 7.1.2 and §7.9.2 (rules-spec §12.6). Note
`german_light_cavalry` (0x730) is absent from rules.xml → −1 → treated as enabled.
Game data: techrules (WHERE: ages/epochs/finals Library, govs Senate), resourcerules PREQ0/OBS of goods 0..5,
rules.xml TECHBONUSES `PREQ preq0`.

### 7.14 Open questions

1. ❓ `Region::fix_borders` in step 14 is called with an uninitialised `this` — confirm it is effectively static.
2. ❓ Consumers of `BuildData.max_age` (walked state; presumably building art age / `Wall::update_gpiece`).
3. ❓ Step 43 `Wall::update_gpiece`: presentation or sim (anim-spec only covers units).
4. 🟡 The building/wall loop bounds in step 27 come from a pointer table at objects +0x1ec (taken as
   `[build_mark, wall_mark]`).
5. ❓ `Game::war_allowed`'s `DL < 9` operand (decompiler `extraout_DL`).
6. ❓ `semaphore` bit meanings (bit 17 "CTW", byte1 & 8 "no start resource grants") — named after fog-spec usage only.
7. ❓ Bonus families not found by name (§7.11) and whether HEAL_FASTER is read via `get_heal_level`.
8. ~~`Unit::set_type` RNG~~: it draws (§6.8.1), so unit upgrades consume `game_random`.
9. 🟡 `has_tech_queued`'s Build vslot 0x190(1, t) semantics.

### 7.15 Corrections to existing specs / the brief

- `Leader::set_age` is scenario/balance-only; age-up is handled inline in `gain_tech` (steps 6, 12, 23, 25, 32, 43).
- rules-spec §9.3: with a leader, the Iroquois government rule moves the age preq one age earlier (drops it only for
  Classical), and `get_preq` reads the live GameInfo — not only at load time.
- Unit upgrades convert garrisoned/transported units too (no inside check), replace queued items in every building,
  and mark predecessors as known+obsolete; buildings upgrade via `upgrade == t` without re-masking.
- The give-techs cheat sets bits only (no counters, no effects).

### 7.16 Evidence

| Function | Address | Used for |
|---|---|---|
| `Leader::gain_tech` (+thunk) | 006dcb60 (00471110) | §7.1–7.3 (decompile `re/decomp/fog/`, asm `prod_D_asm/`) |
| `Build::refund_cost` | 00620490 | step 3 formula |
| `Leader::set_age` / `set_epoch` | 006d25a0 / 006d26f0 | §7.3.2 |
| `Leader::lose_tech` (+thunk) | 006d2850 (0047df20) | §7.8 |
| `TechType::is_ultimate_preq` | 0066c410 | lose_tech cascade |
| `Leader::fix_tech_flags` / `reset_obs_flags` | 006d2480 / 006e32a0 | §7.8 |
| `Game::action_cheat_give_techs` / `zero_techs` | 00593180 / 00593120 | cheats |
| `LeaderData::has_tech` / `has_upgrade` / `has_bonus` | 006e0c80 / 006e10e0 / 006e0b80 | §7.9.1 |
| `LeaderData::has_preq` / `special_preq` / `get_govs_taken` | 006db810 / 006d7030 / 006d69f0 | §7.9.2 |
| `TypeData::get_preq` | 00668700 | §7.9.3 |
| `LeaderData::type_eligible` / `type_avail` | 006dbd10 / 006e33a0 | §7.9.4–5 |
| `Leader::tech_avail`, `techs_per_age`, `starting_age`, `all_techs`, `has_tech_queued`, `researching` | 006da060, 006d7280, 006d7320, 006d7370, 006d73b0, 006db510 | §7.9.6 |
| `LeaderData::current_upgrade` (+thunk) | 006e3140 (006e1360) | §7.9.6 |
| `LeaderData::compute_epoch` | 006d6f80 | lose_tech, init |
| `Leader::init` (tech part) | 006e3930 | §7.9.7 |
| `LeaderData::get_first_library`, `Build::new_library`, `Build::queue_up` (redirect), `BuildData::get_queue`, `Build::close` | 006db6c0, 00627fe0, 00620f40, 0062d280, 00628980 | §7.10 |
| `Build::finished` | 00628490 | caller params, unit-upgrade vs train decision |
| `Build::activate` | 00623e20 | wonder free techs, new_library |
| `City::check_upgrade` | 00738b20 | §7.2.2 |
| `Leader::calc_unit_stats` / `calc_wall_stats`, `Leaders::process_all` | 006cf970 / 006cf7c0, 006ed2a0 | §7.7 |
| `Leader::calc_pop_cap`, `calc_resource_caps`, `LeaderData::get_city_limit` | 006dc490, 006ce900, 006d6130 | §7.4 |
| `LeaderData::get_*_upgrade` (9) | 006da800, 006da850, 006da8a0, 006dae90, 006daee0, 006e0830, 006e0880, 006e1090, 006e1110 | §7.11 |
| `Leader::check_transport`, `track_queued`, `check_population` | 006bc5f0, 006e0f30, 006e1330 | steps 22, 26, 25 |
| `Game::current_age`, `war_allowed`, `Leaders::max_age` | 005946d0, 00594670, 006b8370 | §7.3.3 |
| `LeaderData::has_wonder`, `has_tribe_bonus` | 006ebc10, 006e1370 | gates |
| `TerrainOil::gain_tech` | 008a2e40 | presentation |
| `BitMask<256>::set` | 00450360 | bit + flags cache |

## 8. Buildings per frame: `Wall::process` and `Build::process`

Called from `Objects::process_all` @65dce0 (frame-spec §6) for every alive building: players 0..7 in index order,
`o = 2000 .. build_mark[who]−1` (bound re-read each iteration), **after** the whole unit pass of the frame. Buildings
and walls of leader slots 8 and 9 are never processed. Dead slots with `hold_frames ≠ 0` either eject one garrisoned
unit (`build_masks & 0x4000`, `Build::process_ejection`, which also adds 1 to `hold_frames`) or count `hold_frames`
down. Field names: `BuildData` (`node re/tools/pdbtypes.mjs BuildData`): `job_counter` +0x48, `constr_time` +0x50,
`construct_hits` +0x54, `frame_started` +0x5c, `build_masks` +0x60 (short), `ever_seen` +0x62, `helpers` +0x64,
`city` +0x72, `wonder` +0x76, `recharging` +0x7a, `attack_ox` +0x7c, `stance` +0x7e, `founder` +0x7f, `gather_max`
+0x80, `attack_whom` +0x81, `queued` +0x82, `max_age` +0x83, `build_queue` +0x88, `gather_from` +0x98 (MiningList),
`gather` +0xb8 (GatherPointList).

Object vtable of `Build` @b42174 (the slots used here): +0x2c `is_wonder`
(BuildData @472320: type 526..542), +0x4c `is_active` (`flags & 4`), +0x50 `is_started` (`flags & 2`), +0xb8 `is`,
+0x11c `hits`, +0x148 `has_objmask`, +0x164 `update_local_seen`, +0x168 `repair_damage`, +0x16c `take_damage`,
+0x180 `is_neutralized` (WallData @63ed30: **always 0**), +0x18c `construct_time`, +0x1a4 `start`, +0x1a8 `activate`,
+0x1b0 `finished`, +0x1b4 `do_queue`, +0x1c8 `unqueue`. BuildType (ptype) vtable @b42b94: +0x60 `is`, +0x6c `time`,
+0x78 `get_cost`, +0x90 `is_gather_type`, +0x94 `is_flat`, +0x98 `is_mine`, +0xfc `is_fort`, +0x100 `is_tower`,
+0x108 `is_dock`.

### 8.1 `Wall::process` @640450 (runs first, for every alive building, started or not) ✅ (asm-checked branches)

1. **Seen bookkeeping**: if `frame ≠ 0` and `(frame & 7) == who`: `targeted = trunc(targeted / 4)` (signed char),
   `Wall::check_ever_seen(0)` (fog-spec).
2. **Every 32 frames, staggered** (`(frame + o) % 32 == 0`):
   - `build_masks`: if ATTACKING (0x10) is set clear it, else clear UNDER_ATTACK (0x20) (two-step decay; the bits are
     set by combat code). `build_masks` bit names (PDB): PATHING 1, COMP_GATHER 2, MANDATORY 4, HOLDS_AIR 8,
     ATTACKING 0x10, UNDER_ATTACK 0x20, INFINITE 0x40, REPEAT_AIR 0x80, MAKE_ROAD 0x100, NEUTRALIZED 0x200,
     CONSTRUCTING 0x400, IN_USE 0x800, ACTIVATED 0x1000, ORPHAN 0x2000, EJECTING 0x4000, JAMMED 0x8000.
   - Only for **unfinished** sites (`!is_active`) of **non-human** owners (`!(leader_flags & HUMAN 4)`):
     a. Every 128 frames staggered (`(frame + o) % 128 == 0`): if the type `is(OILPLATFORM 422)` and
        `ObjectsData::find_unit(x, y, SEARCH_FRIENDLY, who, radius −1, 0, FILTER_TARGET, o, who, FILTER_ALL …) < 0`
        (no own unit has this site as its target): **`Object::disband(0)`** and return. (No RNG, §4.9.)
     b. If `is_wonder` or (`ptype.is_fort` and `damage == 0`): AI builder top-up.
        `want = max(4, helpers)`. For wonders: `f = (float)job_counter / (float)construct_time(0)` (both u32 →
        double → float, single-precision divide). For each leader L = 0..7 with `(leader_flags & 3) == 3` that is
        **not** allied with `who` (`LeaderData::is_ally`): if `L.wonderwin_timer` (+0x44c) ≠ 0 → `want++`; else for
        each entry of `unbuilt_wonders.lists[L]` (object index list) whose building `is(this type, strict 1)`:
        `f2` = its progress fraction (same formula); if `f < f2` and (`f2 < f + f` or `f2 < 0.35f`) → `want++`.
        Then if `helpers < want`: `u = find_unit(x, y, SEARCH_FRIENDLY, who, radius 0xf00, 0x200, FILTER_TYPE,
        PEASANTS 50, 0, FILTER_NOT_BUSY …)`; if found: `Unit::add_build_order(o, who, QUEUE_NEW, explicit 0)`.
        Note `helpers` here is the count accumulated by builders **this frame** (the unit pass ran first).
3. **Helpers latch** (every frame): if `helpers ≠ 0` → `helpers = 0`, `build_masks |= CONSTRUCTING` (0x400, "built
   or repaired this frame"); else clear CONSTRUCTING. Then clear IN_USE (0x800, set by the first `do_construct` of
   the frame, §4.2).
4. **Territory attrition** (asm @640879–@640bb7): only on `(frame + o) % 16 == 0`, and only if
   `LeaderData.disable_building_attrition` (+0x804) is 0. If `setup.rush_rules ≠ 0` and `!Game::war_allowed()`: go on
   (period 16). Otherwise require also `(frame + o) % 32 == 0` (period 32). (The odd inversion is faithful.)
   - `owner = WData[wcoord(x, y)].who` (+0xf, signed char); for docks (`ptype.is_dock`) instead
     `BuildTypeData::check_enemy_adjacent(tile_corner, who)`.
   - Return if `owner < 0`, `owner == who`, or `is_ally(who, owner)`.
   - If `ever_seen` lacks bit `owner` and `L[owner].leader_flags & 0x2000`: set it, `check_ever_seen(1)`.
   - Unstarted (`!is_started`): **`Object::disband(0)`** (closes the site; partial refund, §4.9). Started:
     `take_damage(8, 0, 1, −1, 1, 0x55555555, −1, −1)` (vslot +0x16c); return if it returned non-zero (destroyed);
     return if `LeaderData.attrition_stamp3` (+0x1fc) ≠ 0.
   - (P) local player: "attrition" message and sound. Then **`attrition_stamp3 = frame`** for every owner (sim
     state; LeaderData is walked). Only `Wall::process` writes `attrition_stamp3` (InsnGrep @640bb7; nothing clears
     it after the leader is initialised), so after the first damaging hit every later started site returns before
     the message, while unstarted-site disbands keep overwriting it with the current frame.

RNG: none directly (both disbands are of buildings; `take_damage(8, …)` on a building may roll the bird-flock 5 %
of `Object::take_damage`, combat-spec §5).

### 8.2 `Build::process` @61edf0 ✅ (structure from the decompile; branches marked)

1. `Wall::process()` (§8.1). Return unless **active** (`flags & 4`): an unfinished site does nothing else.
2. `healing` (+0x38) countdown (if ≠ 0, −1).
3. `build_masks & EJECTING` (0x4000) → `Build::process_ejection` @6201e0: if `inside_down ≥ 0`, `Unit::come_out(captain of the
   first occupant, 1)` (§6.4.2, **RNG** for AI units); one occupant per frame. If `inside_down < 0`: clear
   0x4000.
4. `build_masks & HOLDS_AIR` (8) → `Object::do_launch` (aircraft launch, **RNG**; combat/air spec).
5. If `ptype.attack ≠ 0`: the attack step of combat-spec §4.13.1 (`do_attack` every frame with a target, else every
   32 frames staggered or when `near_o` is in range; AA turret rotation ≤ 5°/frame). `fire_ammo` draws RNG.
6. **Neutralized branch** (`is_neutralized`, vslot +0x180): returns 0 for every `Build` (the compiler devirtualizes it
   to "skip" when the vtable is `Build::vftable` @61ef96), so only the re-enable half runs: if `build_masks & NEUTRALIZED` (0x200)
   → clear it, `options->rebuild = 1` (UI) and, if `city ≥ 0`, set the city's `city_flags` bit for the building's
   kind: TEMPLE 437 → 0x80, LUMBERMILL 424 → 0x400, GRANARY 423 → 0x200, MARKET 436 → 0x800 (`is(t, 0)`). 0x200 is
   never set by this function (❓ set by capture / loading).
7. **Government-hero healing** (`french_supply_heal_rate`): if the owner has at least one living THESENATOR 353,
   THEPRESIDENT 355 or THECEO 357 (`LeaderData.num_units[t−50]` at +0x59c0/+0x59c4/+0x59c8), the building is damaged
   (`damage ≠ 0` or `damage_frac ≠ 0`), the rate is ≠ 0 and `(o + frame) % rate == 0`: `owner = WData.who` at the
   building's wcoord. Heal if (`has_general(0, 355) ≥ 0` and (`owner < 0` or allied)) or (`has_general(0, 355) < 0`,
   `owner ≥ 0`, allied, and `has_general(0, 353) ≥ 0` or `has_general(0, 357) ≥ 0`). Heal = `repair_damage(1, 0, 1)`
   (vslot +0x168), then `healing = max(healing, rate)`.
8. **`do_queue(0)`** (vslot +0x1b4, `Build::do_queue` @61e410): §6.3 — the production step. Units trained
   here are created during the building pass; they first act next frame (frame-spec §8.3).
9. **Gather tiles**: if `ptype.is_gather_type` and not `ptype.is_flat` (not a farm): region `r = WData.reg` (+4) of
   the building's wcoord; if `regions[r].flags & 0x20` → `Build::verify_gather_tiles` @623570; if `& 0x10` →
   `Build::find_gather_tiles` @623350 (§8.3, **RNG**). (Region flags 0x10/0x20 are the one-frame "changed" latch of
   `GameDaemon::process_all` step 5, frame-spec §5.)
10. **Kremlin** (type == 538, exact type compare): on `(o + frame) % 15 == 0`, if `leader_flags & 0x10000000` and
    `--recharging < 1`: clear that flag, `recharging = constants.kremlin_spy`, `train(SPY)`; if it returned a unit:
    `unit_masks2 |= 0x400` and `control −= (unit_masks & 1) ? 0 : ptype.control_cost` (the free spy does not use
    population). Leader flag 0x10000000 ("Kremlin spy ready") is set by `Build::activate` (@624c35, @625f28) and by
    `Unit::close` (@60ef26, the free spy died) and cleared here.
11. **Government hero** (`LeaderData.gov_hero_frame` +0xa48 == `frame + 1`): if the type is SENATE 438 →
    `train(LeaderData::get_gov_hero())` when ≥ 0. If this is a **city** (`flags & 0x20`): `n = num_buildings[SENATE]`
    plus, if `types[SENATE].to ≥ 0`, `LeaderData::get_buildings(to, num_buildings[to])`; if `n < 1` →
    `train(get_gov_hero())` at the city. (So the hero spawns at the Senate, or at every city when there is none.)
12. **Market caravans** (type == MARKET 436, tribe bonus 0x17): on `(o + frame) % 15 == 0`, if `caras` (+0x980) <
    `LeaderData::get_caravan_limit(1)` and `check_population(CARA) == 0` → `train(CARA)`.
13. **Terracotta Army** (type == TERRACOTTA 529): `--recharging`; when `< 1`: `recharging = terra_cotta_free·15 +
    (terra_cotta_delay == 7 ? barracks_units·15/2 : barracks_units·terra_cotta_delay)` (16-bit arithmetic;
    `barracks_units` LeaderData +0x9f8); unit = SLINGERS 82 if `tribe_can_type(SLINGERS)` else
    `SLINGERS.type_graft(tribe)`, then `current_upgrade(unit)`; if ≥ 0 and `control < pop_cap`: `train(unit)` and
    `game.terra_cotta_total++`.
14. (P) Every 256 frames staggered: local-player hint bubbles (`warn_caravans_stamp` +0x6ec0, university/scholar
    hint +0x6ec4). These LeaderData words are **not walked** (frame-spec open question 6); seat-local.
15. **City centres** (`flags & 0x20` CITY and `city ≥ 0`; `C = cities.lists[who][city]`):
    a. Every 200 frames staggered (`(frame + o) % 200 == 0`): if `C.city_flags & 4` clear 4 else clear 2;
       `C.plundered` (+0x61) −1 if ≠ 0.
    b. If `C.city_flags & 0x10` and `setup.team_style == 2`: for leaders L = 0..7 with `(flags & 3) == 3` and
       `LeaderData::get_target(L) == who` and `ever_seen` lacking bit L: set it, `update_local_seen()`, stop.
    c. (P) every 16 frames: local hint (`warn_cities_stamp` +0x6ebc, not walked).
    d. Every 64 frames staggered: if `check_capture_eligible()`: `e = find_unit(x, y, SEARCH_ENEMY, who, 0xc00, 0,
       FILTER_DOMAIN …)`; if `e ≥ 0` and `check_capture(e, found_who)` ≠ 0 → **return** (captured; the object changed
       owner). Then, for non-human owners with no enemy found, `can_capture()`, `damage > hits()/2`, `C.city_flags &
       2` clear and `LeaderData::get_diff() > 1`: a friendly not-busy citizen within 0x1e00 gets
       `add_repair_order(o, who, QUEUE_NEW, 0)` (AI auto-repair).
    e. **Assimilation** (`C.race` +0x5f ≠ `who`, i.e. a captured city): if THECITIZEN 356 is inside
       (`has_general(0, 356) ≥ 0`): `C.assimilation_timer += 1 − thecitizen_assimilation_speed`. `t = frame −
       C.assimilation_timer`; Turks (tribe bonus 8): `t = (turk_assimilate + 100)·t / 100`; if `who == C.founder`
       (+0x60): `t = (reassimilation + 100)·t / 100`; if `t ≥ assimilation_timer` (constant) → `City::assimilate`
       (§5.6).
    f. If `C.city_flags & 2` is clear: **city healing** — `damage ≠ 0`, `C.race == who` and `(frame + o) %
       city_heal_rate == 0` → `repair_damage(CityData::get_level(C), 0, 1)`. **City alarm**: if `C.city_flags &
       0x40`: period 200 for a human owner with leader option bit 0x20, 60 for an AI owner (a human without the
       option skips); on `(frame + 100 + o) % period == 0`, if `ObjectsData::find(C.x, C.y, SEARCH_NON_FRIENDLY, who,
       get_radius(C)·192, …, FILTER_SEEN)` finds nothing → a temporary `Group` with this building runs
       `Group::action_alarm` (alarm off: garrisoned citizens go back to work).
16. **Roads**: if `build_masks & MAKE_ROAD` (0x100) and `(frame + o) % 16 == 0`: clear 0x100, `BuildType::place_roads(x, y, o,
    who, 1, REGEN_FORCE)` (world-spec §6.3; REGEN_FORCE paths a road to the city, ❓ not reproduced).

### 8.3 `Build::find_gather_tiles` @623350 (the only direct `game_random` user in `Build::process`) ✅

Only for gather types that are not flat (woodcutters, mines, oil wells; not farms) and not `is(UNIVERSITY 420)`.
1. `old = gather_from.count`; `BuildTypeData::find_gather_tcoords(x, y, &gather_from, who)` appends candidate tcoords
   (economy-spec); every listed tcoord gets TData bit 0x1000.
2. **Shuffle**, only if the count grew (`old < n`, n = new count): repeat `4·n` times: `i = (n < 2) ? 0 :
   rnd(n)` (**one `game_random` draw per step when n ≥ 2**), remove entry i (`Array::remove` by value: the first equal
   tcoord) and append it at the end (the array grows by its block if full). n stays constant.
3. `gather_max = BuildTypeData::max_gatherers(o, who, corner tcoord)` (economy-spec).

Called from `Build::init` @629740 when the site is created (so **placing** a woodcutter/mine/oil well draws `4·n`
numbers at command time, inside `process_build`) and from step 9 whenever the region changed.
`verify_gather_tiles` @623570 drops entries that are no longer valid (no RNG).

### 8.4 Ordering consequences

- Builders work in the **unit pass** (`Unit::do_build` → `Wall::do_construct`), so a site can start, take damage
  from builders' progress and **activate** (with `Build::activate`'s side effects, including free units via `train`)
  *before* any building of that frame is processed. The new building then gets its first full `Build::process`
  (attack, `do_queue`, gather tiles, city steps) in the same frame's building pass, because every building slot
  (≥ 2000) is processed after all units.
- `helpers` counts builders per frame: incremented by `do_construct` during the unit pass, consumed by
  `Wall::process` (AI top-up uses it, then the latch resets it). The diminishing-return divisor therefore depends on
  the unit pass order (players start at `frame % 10`, units in slot order): the first builder of a frame adds
  `amount/1`, the second `amount/2`, … (§4.2).
- Units created by `train` in the building pass are appended to the unit list (or reuse the lowest free slot) and
  first act next frame; their `Guy::init_real` draws happen inside `Objects::init_unit` at training time.
- Players' buildings are processed 0..7 regardless of the rotating unit start; within a player, slot order. A building
  that dies during the pass keeps its slot held 30 frames (frame-spec §8.2).

## 9. Player commands

Byte layouts: replay-spec §6. Every `process_*` first writes a SyncLogger / GameLog line (presentation), then acts.
Group verbs act on the package's current group (`group ≥ 0`, set by GROUP) and do nothing without one; none of them
re-checks ownership or uids beyond what is listed (the GROUP command only admits live objects, orders-spec §1).

| Op | Verb | `CommandPackage::process_*` → effect | Spec |
|---:|---|---|---|
| 6 | SWARM_AROUND | `process_swarm_around` @949970: if `whom < 0`, `ox < 0` or the target is alive → `Group::action_swarm_around(ox, whom, queued, orders, explicit 1)`. **This is how the UI builds an unfinished site (BUILD_AT 6), repairs (REPAIR 13) and sends workers to gather** (right-click, `Console::execute_at_cursor` @7c6630 → `GroupOut::issue_swarm_around`) | §3.4, §4.8 |
| 16 | REPAIR | `process_repair` @948cb0: if `whom < 0`, `ox < 0` or the target is alive → `Group::action_repair(ox, whom, queued)` @7020c0 (citizens on a coast repair a **unit**, i.e. ships; no UI issuer, scenarios only) | §4.8 |
| 21 | DISBAND | `process_disband` @948660 → `Group::action_disband(all)` @70e260: units and unfinished sites are disbanded (refund, units draw `rnd(3)+1`); finished buildings get a DISBAND queue item (timed demolition) | §4.9 |
| 22 | GATHER_POINT | `process_gather_point` @948510 → `Group::action_gather_point(x, y, action, add_to_end)` @6ff1b0 (rally points) | §6.6 |
| 23 | SPELL | `Group::action_spell` @6fe1a0: BUILDAIR (661) fills carrier aircraft queues (pays at once) | §6.7 |
| 24 | QUEUE_UP | `process_queue_up` @948230 → `Group::action_queue_up(type, num)` @6fdbb0: train / research / upgrade / in-place building upgrade; pays at once | §6.2 |
| 25 | BUILD | `process_build` @948110 → `Group::action_build(x, y, x2, y2, type, queued)` @707510 (x2/y2 unused): validate, pay, create the site, assign workers | §3.1 |
| 26 | EJECTALL | `Group::action_eject_all` → `come_out` per occupant (RNG for AI units) | §6.4.2 |
| 33 | BUILDMASK | `process_buildmask` @947680 → `Group::action_buildmask(mask, set)` @6fc9a0: **toggles** INFINITE (repeat queue, 0x40) or REPEAT_AIR (0x80); `set` is ignored | §6.3.4 |
| 48 | UNQUEUE | `process_unqueue` @9466f0 (not a group verb): object `(who, o)` alive with matching `uid`: a building → `Build::action_unqueue(index)` @620280 (first click on a repeating queue only stops repeating; index −5 / ≤ −10 remove 5 / all from the back); a unit → `Unit::action_unqueue(1)` @5e1f20 (carrier aircraft, refund at the current price). Issuers `BuildOut::issue_unqueue` @629e70, `UnitOut::issue_unqueue` @607bc0 | §6.5 |
| 49 | COME_OUT | `process_come_out` @9465d0 → `Unit::action_come_out` (garrison exit through `come_out`) | §6.4.2 |
| 60 / 61 | CHEAT_GIVE_TECHS / CHEAT_ZERO_TECHS | `Game::action_cheat_give_techs` @593180 / `…zero_techs` @593120: set / clear every bit of `leaders[who].tech` (no counters, no `gain_tech` effects) | §7.8 |
| 65 / 66 | CHEAT_INCREASE / ZERO_BUCKETS | resources (economy-spec) | |
| 67 | CHEAT_INIT_UNIT | `Game::action_cheat_init_unit(who, t, x, y)` → `Objects::init_unit` (RNG per guy) | |
| 73 | LEADER_OPTIONS | worker / building stance defaults read by `Build::init` (building stance), `build_done` (idle-worker behaviour) | §3.3, §4.7 |

## 10. The RNG contract of production

All draws are `game_random.get(0, 0xffff)` (`rnd(n)` = that `% n`). Listed in the order they can occur inside one
frame (§1). **No draw** happens in: `get_cost`, `can_pay_cost`, `pay_cost`, refunds, `train_time`, construction times,
`blocked_*`, `snap_center`, `validate_build`, `action_build` (other than through `Build::init`), `action_swarm_around`,
`add_build_order`, `do_build`, `do_construct`, `Build::start`, `kill_competing_buildings`, `update_hits`,
`do_repair`, `repair_damage`, `Object::disband` **of a building**, `queue_up`, `could_queue`, `can_make`, `type_avail`,
`do_queue`, `finished` (other than through `train` / `gain_tech`), `unqueue`, `clean_queue`, `add_gather_point`,
`action_gather_point`, `action_buildmask`, `gain_tech` / `lose_tech` / `set_age` (other than through their callees),
`calc_unit_stats`, `calc_wall_stats`, City founding / upgrade / naming / closing, wonders, forts, territory triggers,
`Wall::process`, the rest of `Build::process`. `SoundGlobal::random` (sounds in `action_queue_up`, `kill_at_tile`, …)
is a separate presentation stream.

| # | When (§1 box) | Site | Draws | Condition |
|---|---|---|---|---|
| 1 | command: BUILD | `Build::find_gather_tiles` @623350 from `Build::init` | `4·n` × `rnd(n)` (shuffle) | non-flat gather type (woodcutter, mine, oil well/platform; not farm, not university), the tile list grew, `n ≥ 2` |
| 2 | unit pass: `Build::activate` | `Objects::init_unit` → `Unit::init` → `Guy::init_real` @5db6e9 | ≥ 1 per guy (idle variant `% 100`) | every free unit of §4.6 step 8 (tribe bonuses), carrier wings, gov hero, American bombers (gain_tech step 25) |
| 3 | after 2 | `Unit::come_out` @61a1bb / @61a1d5 | 1 (`v % 3` BARK, `v % 2` special units) | AI-controlled (`UNIT_AI_CONTROL`), not caravan / merchant / fur trapper, BARK or `unit_flags2 & 0x10` |
| 4 | unit pass: carriers | `init_unit` (aircraft) | as 2 | carrier aircraft queue completes (§6.7) |
| 5 | unit pass / commands | `Unit::set_type` → `Guy::init_real` | 1 per kept squad guy and per crew guy | every existing unit converted by a unit-line upgrade (`gain_tech` §7.2.1), in unit slot order; gov hero swap at a Senate; spells |
| 6 | building pass: `Build::process` step 3 | `Build::process_ejection` → `come_out` | as 3 | garrison ejection (EJECTING) of AI units |
| 7 | building pass: `Build::process` step 4–5 | `Object::do_launch`, `fire_ammo` | combat-spec §4 | aircraft launch, buildings that fire |
| 8 | building pass: `do_queue` → `finished` → `train` | `init_unit` (2), `come_out` (3), `Object::die` (combat-spec §6) | as 2/3; death draws | trained unit; a unit that cannot leave an over-full building is killed; the oil-platform barge |
| 9 | building pass: `finished` → `gain_tech` | through `City::assimilate` (World Government) and `Build::train` (American bombers) and `set_type` (5) | as 2/3/5 | §7.1 RNG list |
| 10 | building pass: `Build::process` step 9 | `find_gather_tiles` (1) | as 1 | region of the building changed (region flag 0x10) |
| 11 | building pass: `Build::process` steps 10–13 | `train` (2, 3) | as 2/3 | Kremlin spy, gov hero, market caravan (Persians), Terracotta Army |
| 12 | any: unit disband | `Object::disband` @6459c3 | 1 (`rnd(3) + 1` death type) | **units only** (DISBAND command on units, AI) |
| 13 | AI | `Leader::upgrade_units` @6c6430 | 1 per evaluated candidate (`% 3`) | AI production, difficulty ≠ 2 (❓ AI spec) |

Iteration-order dependencies that decide the order of these draws: group member order (BUILD: `action_swarm_around`
land pass then sea pass, list order; QUEUE_UP: members sorted by `queued`, ties in list order; `num` rounds
interleaved over members); queue index order, library slots deepest first; unit slot order in `gain_tech`'s
conversion loop and in `calc_unit_stats`; building slot order (2000..build_mark) in the building pass,
`activate`'s wonder-race disband, `find_buildings`, `get_first_library`; leaders 0..7 everywhere except the unit pass
(starts at `frame % 10`); the uber chain (`o_down`) before rally orders in `come_out`.

## 11. Rules data and runtime state

**Rules (all loaded already).** Every Type field and every constant this document reads is in `engine/sim/rules.hpp`
(`Type`, `Constants` by byte offset via `constant_offset(name)` / `Constants::at`): TypeData (`costs`, `job_time`,
`res_time`, `preq[3]`, `from`, `where`, `upgrade`, `jump`, `obs`, `cat`, `tribe_mask`), ObjectTypeData (`obj_masks`,
`attack`, `hits`, `domain`, `x_size`/`y_size`, `block_radius`, `big_radius`, `special_upgrade(_cost)`,
`support`/`support_cost`, `is_list`, `is_strict_list`), UnitTypeData (`unit_flags`, `unit_flags2`, `role`,
`military_level`, `research_premium_cost/_time`, `job_extra_time`, `control_cost`, `progression`, `squad_size`,
`uber_size`, `crew_size`), BuildTypeData (`build_flags` incl. the derived high bits, `to`, `wonder_val`,
`garrison_max`, `base_arrows`, `most_shots`, the build mask), TechTypeData (`age`, `leader_off`), SpellTypeData (`use`,
`where`), GoodTypeData (`undiscovered/obsolete_cost_good/_rate`, `preq`, `obs`), Tribe grafts, the categories
(`map_sizes`, `wonderwins`). Two loader details matter at runtime: `TypeData::get_preq` must be evaluated **live**
with the game's `GameInfo` and the leader (rules-spec §9.3 only covers the load-time call, §7.9.3), and the
`german_light_cavalry` constant is absent from rules.xml (−1 = enabled, §7.1.2).

**Missing runtime state** (to add to `engine/sim`; walk order and checksum membership in state-walk.md):
- `LeaderData`: resource buckets and escrow (economy-spec), `tech` / `tech_at_start` / `obs_flags` BitMask<806>,
  the encrypted block (`ages`, `epochs`, `discovered`, `epoch[4]`, buckets — store plain, XOR on walk), `age_stamp[7]`,
  `gov`, `gov_hero_frame`, `misery`, `num_units[352]`, `num_queued[806]`, `num_buildings[129]`, `high_buildings[129]`,
  `reg_buildings`, the per-class `*_units` / `*_queued` counters, `ages_queued`, `epochs_queued`, `tech_frame`,
  `tech_cat_frame[4]`, `pop`, `pop_cap`, `pop_issues`, `control`, `active`, `caras`, `city_num`, `city_mine`,
  `city_mark`, `city_name`, `fort_mark`, `wonder_mark`, `lost_city_stamp`, the capital timer words, `reg_cities`,
  `reg_forts`, `reg_terr`, `reg_pop`, `buildings_built`, `cities_built`, `wonders_built`, `wonders_held`,
  `senates_built`, `forts_built`, `defense`, `last_unit_finished`, `last_building_finished`, `gather_slots(_high)`,
  `attrition_stamp3`, `disable_building_attrition`, `repair_stamp`, `nukes_used/launched`, `missiles_used`,
  `scholar_militia`, `ally_mask`, leader flag bits (NEW_TECH, RECOMPUTE, CALC_UNIT_STATS, CALC_WALL_STATS,
  KREMLIN_SPY, EXPLORE_MAP, REVEAL_ENEMY, ACTIVE_PING, CAN_TRANSPORT_*).
- `BuildData` / `WallData` (objects.hpp `Build` has only a stub): `job_counter`, `job_counter_2`, `constr_time`,
  `construct_hits`, `frame_started`, `build_masks` (16 bits, names in §8.1), `ever_seen`, `ever_seen_completed`,
  `helpers`, `demolition`, `orig_type`, `gather_down`, `city`, `city_down`, `wonder`, `fort/dock/farm/oil_well`,
  `recharging`, `attack_ox`, `stance`, `founder`, `gather_max`, `attack_whom`, `queued`, `max_age`, `infiltrate(2)`,
  `build_queue` (fixed array, §6.1.1), `gather_from` (MiningList), `gather` (GatherPointList).
- `UnitData`: `num_queued`, `queue_time` (carriers).
- Globals: `cities.lists[8]` (City records, §5.0.1), `forts`, `wonders`, `unbuilt_cities/forts/wonders`,
  `Game.wonders[17]`, `terra_cotta_total`, `world_pop`, `world_cities`, `Game.starting[6]`,
  `BuildData::queue_fail`, `Region.borders`, `GameDaemon.borders`, GameInfo (`rush_rules`, `victory`, `tech_cost`,
  `starting_resources(2)`, `game_rules`, `starting/ending_technology`, `elimination`, `team_style`, `flags`).

## 12. Reference tool

`re/tools/production-ref.mjs` (§2.6): `--selftest` checks hand-computed values (citizen ramp and its 3× time cap,
builder sums, science penalty on an age); `--examples` prints the table of §2.6; `<type> [--count N] [--tribe X]
[--age A] [--mil/--civ/--com/--sci N] [--wonder W] [--rare R] [--preq BONUS] [--upgrade] [--builders N] [--trace]`
prices one case with a trace of every step. It models `get_cost` for units, buildings and techs (common tail and
unavailable-resource conversion included), `pay_cost`, `get_support_count`, `calc_science_discount`, `train_time`
(main branches), `update_construct_time` + `construct_time` and the N-builder construction rule; spells, declarations
of war, CTW, scenario and AI branches are marked NOT MODELLED with their addresses. A C++ port of §2 should reproduce
its `--examples` table exactly before being trusted, then be cross-checked against replay buckets.

## 13. Port plan (ordered)

Each step: implement from this spec, add a self-test (or a `ron-sim` digest component), keep `ron-check`,
`ron-rules`, `ron-world` green.

1. **Leader production state and predicates.** Add the LeaderData fields of §11 to `game.hpp` `Leader` (plain values,
   XOR only when walking), with `Leader::init`'s tech part (§7.9.7). Implement `has_tribe_bonus`, `has_rare`,
   `has_wonder`, `get_graft`/`tribe_can_type`, runtime `get_preq`, `has_tech`, `has_preq`/`special_preq`,
   `type_eligible`, `type_avail`, `current_upgrade` (also unblocks the `LeaderData::current_upgrade` stub of
   `groups.cpp`), `techs_per_age`, `researching`, `get_first_library`.
2. **Costs and times** (§2): `get_cost`, `can_pay_cost`, `pay_cost`, `unpay_cost`, `get_support_count`,
   `calc_science_discount`, `cost_factor_dow`, `ObjectData::train_time`, `TypeData::time`/`research_time`,
   `Wall::update_construct_time`, `BuildData::construct_time`. Gate: match `production-ref.mjs --examples` byte for
   byte (port its cases into a C++ self-test).
3. **Build data model** (§3.3, §6.1): full `Build` fields, `BuildQueue` with the fixed-array / stale-tail semantics,
   GatherPointList, unbuilt lists, `Build::init` including `find_gather_tiles` (RNG) — and **create scenario units and
   buildings interleaved in file order** (sim-implementation "known deviations"), since buildings now draw RNG.
4. **Placement and the BUILD command** (§3): `blocked_site` / `blocked_tcoord` / `blocked_location` and helpers (on
   top of `placement.cpp`), `validate_build`, `action_build`, `action_swarm_around`, `add_build_order` /
   `add_repair_order`, SWARM_AROUND and BUILD in `commands.cpp`.
5. **Construction** (§4): `do_build`, `do_construct` (helpers, IN_USE), `Build::start` (+ `kill_competing_buildings`,
   using `placement.cpp`'s `wall_start`), `Build::activate` in stages (generic stats / counters, then cities §5.3,
   free units, wonders), construction hit points in `Wall::inc_time`, `Wall::process` (§8.1),
   `check_build_order` / `build_done` / `find_build_spot`.
6. **Cities** (§5): City/Fort/Wonder records and slot allocation, `find_city` / `get_town` / `find_city_at`,
   `add_to_city` / `remove_from_city`, `find_buildings`, `check_upgrade`, radius, city limit, `City::close`, capital
   rules, territory triggers (`Region.borders` resets) with `check_borders`' 256-wcoord budget and the influence
   algorithm (`World::compute_reg_territory`, §5.10.1).
7. **Queues and training** (§6): `queue_up`, `could_queue` / `can_make` / `queue_here`, `do_queue`, `finished`,
   `train`, `come_out` (rally points), `unqueue`, `clean_queue`, `action_unqueue`, BUILDMASK, repeat; QUEUE_UP,
   UNQUEUE, GATHER_POINT, BUILDMASK commands; carriers.
8. **Research** (§7): `gain_tech` (ordered steps; start with bits, counters, science refund, CALC flags, unit and
   building branches, ages and epochs; then bonuses and tribe loops), `Unit::set_type` / `Wall::set_type`,
   `calc_unit_stats` / `calc_wall_stats` in `Leaders::process_all`, `lose_tech`, cheats.
9. **`Build::process`** remaining steps (§8.2): specials (Kremlin, Senate, Market, Terracotta), city per-frame
   steps; capture after the combat port (`check_capture`, `capture_city`, §5.7).
10. **Repair and deletion** (§4.8, §4.9): `do_repair` (float pay steps), DISBAND (`action_disband`,
    `Object::disband` refunds in single precision), demolition queue items.
11. **Harness and UI**: `ron-sim --build WHO:o..@X,Y/type`, `--queue WHO:o@type[xN]`, digests over LeaderData
    production fields, queues and buildings; `ronmap` build menu, placement ghost (`blocked_site` code → feedback
    text), train/research buttons, rally points; validation against replays with production once the user provides
    them (QUEUE_UP followed by bucket changes is the first oracle).

## 14. Open questions and corrections

Open items, by owner section (details in each section's "Open questions"):
1. ~~`World::compute_reg_territory` @6b0bb0~~ — decoded (§5.10.1, `engine/sim/territory.cpp`).
2. ❓ `LeaderData+0x410` writer is `City::close` (`lost_city_stamp`, §5.8) — confirm against the LOST_CITY rule of
   `blocked_site` (§3.5).
3. 🟡 Terraforming at placement (`terraform_for_building`): heights feed object z (simulation?) (§3.3).
4. ❓ How units standing on a newly started footprint are displaced (§4.2).
5. ❓ Ship repair through REPAIR op 16 (`do_repair` rejects non-buildings) (§4.8).
6. 🟡 `validate_disband`, `find_repair_spot` return paths; free-unit counts of Roman/British/Aztec/Mongol bonuses (§4).
7. 🟡 `Cities::capture_city` plunder and unit conversion; `Build::check_capture` strength loop (§5.7).
8. 🟡 Chinese assimilation downgrade and the city-close orphaning order (§5.4, §5.8) — replay checks.
9. ❓ `HotKeyGroups::copy` in `come_out`; filter 0x11; `add_move_order`'s uninitialised 7th argument (§6.4.2).
10. ❓ `Leader::upgrade_units` (AI) beyond its RNG draw (§6.8); AI cost estimates (§2.4).
11. 🟡 `is(X, strict)` semantics in the spy/general cost tests; the American free-bomber term of
    `get_support_count`; the meaning of `recharging` on unfinished sites (§2).
12. ❓ Consumers of `BuildData.max_age` and `Wonder.timer`; whether `Wall::update_gpiece` on age-up is simulation
    (§5.13, §7).
13. ❓ Bonus families not found by name (TEMPLEHP, TEMPLERANGE, HEAL_FASTER, TOWER_FORT_RANGE, STEALTH_STUFF) (§7.11).

Corrections found while porting (engine/sim, 2026-10-12), folded into the sections above where they belong:
- Scenario load: objects are created in file order with cities (chunk 7) **before** objects (chunk 5);
  `Objects::init_build(who, type, x, y, preplaced 0, o)` then `activate(0, 0, 0)`; queue items go through
  `Build::queue_up(t, 0)` followed by `Type::unpay_cost(who, −1, −1)` (a refund at the recomputed price, so the
  stockpile can change by the ramp difference); `first_in_progress` sets the first item's `job_counter` to **the item's
  type index** for buildings (asm @9aab37, a quirk) and to `train_time` for cities. Leaders: `Leader::init(who, tribe,
  colour)`, then the record (gov from record +0xc, handicap, difficulty, human, leader flags), then the player chunk's
  children in file order (38 techs / 76 types via `gain_tech(t, 0, 0, _, 1)` stopping at the first unknown name, 41
  diplomacy, 44 stockpile). `World::compute_all_territory` runs in `init_terrain` (before objects) and again after
  them.
- `Unbuilt*::remove_unbuilt_*` overwrite the removed entry with the last one (order not preserved).
- `Unit::close` decrements `control` / `active` / `track_unit_type` only for the **last** member of an uber chain;
  `Objects::init_unit` takes the counters back from every follower (`track_unit_type(−1)`, `control`, `active`,
  `units_built`), so a batch counts once.
- `BuildTypeData::max_gatherers` previews `calc_gather` with (assume 0, active 1, gatherers 1) (§3.7).
- Mines claim the deposit's tiles that are **not trees** (`mask & 0x30 ≠ 0x30`), not "not water"; woodcutters test
  the W-cell's centre tile `(4wx+2, 4wy+2)` for the claimed bit and collect only tree tiles (economy-spec §3.6).
- Free units of `Build::activate` step 8 are given only outside scenarios or with `ScenarioData::building_unit_bonus`.

Corrections to existing specs found here (to fold back when those specs are next edited):
- frame-spec §8.1 / §8.2 / §10 and combat-spec's RNG table (#11): `Object::disband` draws `rnd(3)+1` **only for
  units**. Disbanding a building (competing sites, blocked sites, territory attrition, AI abandonment, wonder-race
  losers) draws nothing: unstarted → `close(0)`, started → `die(1)`.
- frame-spec §10: the `Wall::process` disband applies only to AI oil platforms nobody targets (every 128 frames);
  general AI abandonment is `Leader::check_orphaned_buildings` + `take_damage` on ORPHAN sites. Territory attrition
  runs every 32 frames, or every 16 while rush rules forbid war (§8.1).
- frame-spec §8.1 / §10: queue capacity 20 / **10 for tech-researching buildings** / 2; `Build::train` returns the new
  unit's `o` (0 / −1 / 1 are `Build::finished`'s codes); a pop-capped head keeps progressing and blocks only at
  completion (`pop_issues++` each frame); libraries run up to "cities with a library" items in parallel; the repeat
  re-queue pays again without escrow and drops the repeat flag when it fails; construction adds
  `max(1, amount/(helpers+1))` and `amount = accel_construct / 4` while the site is under attack.
- orders-spec §2.7–2.9: worker assignment details (§3.4); QUEUE_UP's sort and silo rule (§6.2.2); `queue_up`'s second
  argument is "use escrow"; rally-point eligibility (§6.6); BUILDMASK's `set` argument is ignored (it toggles).
- world-spec §6.3 item 5 / §8: the city radius is **not** tech dependent (20 / 24 / 28 tcoords, Indians +4, §5.1);
  §6.4: a non-zero `blocked_location` result overrides the per-tile result (only 34, 36, 14 and 45 return early) and
  `in_enemy_territory` gives 25 for a player without cities.
- rules-spec §6.3: `job_time` is in frames (time unit 1/100 frame, advanced 100 per frame), not "1/15 s"; §9.3:
  `get_preq` reads the live GameInfo, and the Iroquois government rule moves the age preq one age earlier.
- frame-spec §6 / §8.1: player-built walls do not exist in Rise of Nations; `Objects::init_wall` has no callers and the
  3000+ range stays empty.
- The brief's "`Leader::set_age`: building max_age": `set_age` is scenario/balance only; a normal age-up is handled
  inline in `gain_tech` (§7.3).
