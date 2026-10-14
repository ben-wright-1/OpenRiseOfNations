# City capture, ownership transfer, elimination and victory

How a reduced city changes hands, what happens to its buildings, units and stockpiles, how losing cities and capitals
eliminates a leader, and how the game decides that it is over. Ported in `engine/sim/capture.{hpp,cpp}` (with hooks in
`cities.cpp`, `construct.cpp`, `combat_damage.cpp`, `game.cpp`, `ai_army.cpp`); see docs/sim-implementation.md
"Capture and victory".

Status: ✅ from the decompiles and the disassembly · 🟡 read from the decompile only, or approximated in the port ·
❓ open. Evidence is listed per section and in §15; no decompiled code is reproduced here (paraphrase and pseudocode
only). Sources: `re/decomp/capture/` (decompiles of the functions below and `asm/` disassembly of
`Build::check_capture`, `Cities::capture_city`, `Leader::defeat`, `Leader::defeat_by`, `Leader::lost_a_city`,
`Game::check_victory`, `GameDaemon::process_victory`, `Leader::victory`, `Search::valid_filter`; `vtables.txt`), plus
`re/decomp/prod_c2/` (`Cities_capture_city`, `City_capture`, `City_close`, `City_assimilate`), `prod_c2b/`
(`Build_check_capture`, `Leader_defeat_by`, `Game_check_victory`, `GameDaemon_process_victory`, `Game_retake_capital`),
`econ_mkt/Leader_defeat`, `combat_C/Build_process`, `combat/Object_do_damage`, `combat/Object_take_damage`,
`scenario/ScenarioRead_import`. Conventions as combat-spec §0 and production-spec §5 (type indices, City fields,
vslots). Leader flags: 0x1 VALID, 0x2 ACTIVE, 0x4 HUMAN, 0x8 co-op, **0x20 VICTORY, 0x40 DEFEATED, 0x80 game over
seen**, 0x400000 "capital already plundered", 0x2000000 RECOMPUTE; `leader_flags2` (+0x4): 0x1 NO_CITY_DEFEAT, 0x2
UNIT_AI_OFF, 0x100 victory flag, 0x200 defeat flag.

## 1. The chain

```
Unit::fight / Object::find_nearby_target ─ Object::valid_target ─┐
Group::action_attack, Unit::do_group_attack (combat-spec 3.3) ────┤
Object::do_damage tail (§2.4) ────────────────────────────────────┼─> Build::check_capture (§3)
Build::process, every 64 frames per city (§2.6) ──────────────────┘        │ attackers stronger
                                                                           v
                                     Cities::capture_city (§4) ─ City::capture (§4.3), Build::swap_team (§5),
                                                                 City::find_buildings, plunder, disposal
                                                                           │
                                     City::close(old record, attacker) ────┘
                                         └─ Leader::lost_a_city (§6.1) ─ Leader::defeat (§6.3) ─ Leader::defeat_by (§6.4)
                                                                             └─ Game::check_victory (§7.1) ─ Leader::victory
Leaders::process_all: Leader::process_elimination (§6.6) + the clocks (§7.4)
GameDaemon::process_all: GameDaemon::process_victory (§7.3)
```

Only cities are capturable: `BuildData::check_capture_eligible` @62d1d0 tests BuildType vslot 0x64 (`is_city` =
`is(414 VILLAGE)`, i.e. Small / Large / Major City and the Forbidden City), and no BuildType overrides that slot
(`re/map/functions.tsv` lists the one implementation @472bc0). **Forts, castles and towers are never captured**; they are
destroyed. `Build::check_capture` has a branch for non-city capturable buildings (§3.4) that no stock type reaches.

## 2. Reducing a city ✅

### 2.1 Eligibility (`BuildData::check_capture_eligible` @62d1d0)

City type, ACTIVE, and `ObjectData::health_level() > 5` for an OBJECT_CITY object (`> 4` otherwise). health_level
(@6534b0, combat-spec 2.8) is 6 only when `hits − damage < 1`: **a city is capturable exactly when it has no hit points
left**. (5 = less than 10 % left, for the non-city branch.)

### 2.2 Cities do not die (`Object::take_damage` @652020, combat-spec 5.4)

A hit on an ACTIVE OBJECT_CITY clamps `damage` at `hits()` and sets NEW_THINK; the function returns 0 (survived). Before
that, for every non-quiet hit by another leader:
- `build_masks |= 0x30` (ATTACKING | UNDER_ATTACK) on the building, **unless** the attacker is a citizen (type 50 / 51,
  `ObjectData::is_peasant` @46d310) or militia (`is(66)`) whose own territory (`WData.who`) is not the building's tile;
  an attacker with `att_o < 0` always marks.
- the building's city record (`Build.city ≥ 0`): `city_flags |= 0xe` (UNDER_ATTACK 0x2 | ATTACKING 0x4 |
  EVER_ATTACKED 0x8), then the AI alarm (`Group::action_alarm` on a temporary group holding the city object) for a
  computer owner (not HUMAN, not co-op) at `get_diff() > 1` when the building is a garrison type (BuildType vslot 0x90)
  or the city object, and the city has no ALARM flag.
- (eject_contents when `damage ≥ hits` and the building cannot carry aircraft.)

### 2.3 Healing stops while attacked (`Build::process` @61edf0, production-spec 5.6)

Every 200 frames (`(frame + o) % 200 == 0`) the city clears ATTACKING, or UNDER_ATTACK if ATTACKING was already clear
(two-step decay). While UNDER_ATTACK is clear and `race == who`, the city object heals `level` hit points every
`city_heal_rate` (4) frames (`repair_damage(level, 0, 1)`). So a city under continuous attack stays at full damage; an
unassimilated captured city (`race ≠ who`, §8) never heals.

### 2.4 The city tail of `Object::do_damage` @64a480 ✅

Before the hit, `local_70` = the target is an ACTIVE OBJECT_CITY with `hits(0) > damage` (still standing). After
`take_damage` and the building collateral (combat-spec 5.5), when the target type `is_city` and the attacker's leader is
not the owner:
1. `Build::check_capture(target, att.o, att.who)`; a capture ends the function.
2. Only for an OBJECT_CITY, ACTIVE target with `damage ≥ hits(0)` (fully reduced):
   - a siege attacker (`ObjectData::is_siege`) of a computer leader (`leader_flags & HUMAN` clear) that carries
     AI_CONTROL (`unit_masks & 0x40000`) and belongs to an army (`Object::get_army` ≥ 0): `Army::charge(army, ox,
     whom)` (ai-spec 12.5);
   - if `local_70 == 0` (the city was already reduced before this hit) and `frame − city.reduce_stamp (+0x1c) < 300`:
     return; else `reduce_stamp = frame` and the "city reduced" UI event for the console player / allies (P).

So the hit that reduces a city to 0 already tries the capture; later hits retry it.

### 2.5 Who else calls `check_capture`

- `Object::valid_target(ox, whom, *capturable)` @648ba0 (combat-spec 2.9.2): for a capture-eligible city every
  validity test of a non-missile shooter calls `check_capture(T, S.o, S.who)`; `valid_target` then answers **invalid**
  (no further attacks on a reduced city) except for a mandatory explicit ATTACK by a vehicle war machine. This runs for
  every capture-eligible candidate of a target scan (combat-spec 2.5) — a capture inside a scan closes the old city
  object and creates the new one at the head of the same wcoord list, behind the scan cursor (the next pointer was
  read before the candidate was evaluated), so the scan continues safely ✅.
- `Group::action_attack` and `Unit::do_group_attack` (combat-spec 3.3.1 step 6, 3.3.3): an eligible city target is
  `check_capture`d by the group leader / a member in range, then the group moves onto it.
- The 64-frame poll (§2.6).

### 2.6 The 64-frame poll (`Build::process`, city object with `city ≥ 0`) ✅

```
if (frame + o) % 64 == 0:                                   // signed 0x8000003f mask, frame + o ≥ 0
    u = −1
    if check_capture_eligible():
        u = ObjectsData::find_unit(x, y, SEARCH_ENEMY, who, 0xc00, 0, FILTER_DOMAIN 0 (land), 0, FILTER_ALL)
        if u ≥ 0 and find_who ≥ 0 and check_capture(u, find_who): return      // Build::process ends here
    if !(leader HUMAN) and u < 0 and WallData::can_capture() (= is_city) and hits(0)/2 < damage
       and !(city UNDER_ATTACK) and get_diff() > 1:
        p = find_unit(x, y, SEARCH_FRIENDLY, who, 0x1e00, 0x200 (same region), FILTER_TYPE 50, 0,
                      FILTER_NOT_BUSY)
        if p ≥ 0: Unit::add_repair_order(p, o, who, QUEUE_NEW, 0)                // AI repairs its damaged city
```
FILTER_NOT_BUSY (Search::valid_filter case 12, jump-table entry @67e019): a unit whose `action_type` is 0 (none),
1 (MOVE_TO) or 7 (GATHER); buildings fail. FILTER_NOT_UNDER_ATTACK (case 8 @67de05): units pass, buildings pass when
`build_masks & 0x20` is clear. (The jump table @67e57c is indexed by `filter − 1`.)

## 3. `Build::check_capture(unit_o, unit_who)` @6276a0 ✅ (decompile + asm)

### 3.1 Gates (any failure returns 0)

1. `check_capture_eligible()`.
2. A garrison (`inside_down` +0x28 ≥ 0): `build_masks |= 0x4000` (EJECTING; the garrison comes out next) and return 0.
3. `(unit_who ≠ owner and not mutually allied (diplos 2 both ways)) or the owner is not ACTIVE`, and `unit_who` ACTIVE.
4. The capturing object `objects[unit_who][unit_o]`: type domain 0 (land), `is_unit`, not a decoy (`unit_masks & 1`),
   `attack() ≠ 0`, type lacks OBJMASK_MISSILE (`obj_masks` +0x1e4 & 0x8000000).
5. `C = cities[owner][city]`: `frame − C.capture_stamp (+0x20) > 74`. (`capture_stamp` is set to `frame` by
   `City::init(loading = 1)`, i.e. on every capture and at scenario load: a city cannot change hands twice within 75
   frames.)

### 3.2 The strength scan

```
R = city ? city_capture_radius (+0x134, 10) : unit_respond_range (+0x18, 12)          // tcoords
reach = R·192;  rw = (R·192 + 0x2ff) / 0x300                                           // wcoords
reg = WData.region at the building's wcoord
def = (frame − C.capture_stamp < 900) ? C.capture_strength (+0x28) : 2
av  = (unit domain == 0) ? get_capture_value(unit) : 0;  att = av
sum[unit_who] = av;  sum[owner] = def                                                 // per-leader sums (8)
for k in [0, circle_radius[rw]) (circle_x/y around the building's wcoord, combat-spec 2.17.1):
    in-map wcoord with a non-empty object list and WData.region == reg:
    for each object (o, w) of the list, newest first (next pointer read first):
        skip the building itself and the capturing unit
        skip !VALID, domain 2 (air) or 1 (sea), decoy units, Search::valid_filter(w, o, −1, −1, FILTER_NOT_UNDER_ATTACK) == 0
        skip vector_dist(|x − bx|, |y − by|) > reach
        v = get_capture_value(object)            // vslot 0xec: units §3.5; buildings 1 (folded `return 1`)
        if v ≠ 0 and the object is a building of the owner:
            v = num_inside(1) + (BuildType is_fort ? 12 : 6) + v      // own buildings defend: 7, forts 13, + garrison
            if !(visible (+0x40) & (1 << unit_who)): visible |= that bit; update_local_seen()   // the attacker sees it
        sum[w] += v
        if w == unit_who or (is_ally(unit_who, w) and is_enemy(owner, w)):   att += v
        elif w == owner or (is_ally(owner, w) and is_enemy(unit_who, w)):    def += v
if !(def < att): return 0
```
Notes: buildings of any leader count (1 each; the owner's 7 or 13); an owner's building that is itself under attack
(`build_masks & 0x20`) does not count. Neutral leaders' objects only add to `sum`. Gaia objects (who 9) index past the
8-entry sum array into a local String of the function (harmless; the port ignores them).

### 3.3 The new owner (city branch)

```
best = unit_who; bestv = 0
for i in 0..7 with (flags & 3) == 3 and (i == unit_who or is_ally(i, unit_who)):
    if bestv < sum[i]: bestv = sum[i]; best = i                         // strict: ties keep the lower index
F = C.founder (+0x60), Rc = C.race (+0x5f)
eligible(p) = p ∉ {owner, best} and is_ally(best, p) and leaders[p] ACTIVE
new = eligible(F) ? F : eligible(Rc) ? Rc : best
n = Cities::capture_city(new, C.city, owner)
cities[new][n].capture_strength = av                                     // the capturing unit's own value
return 1
```
(The original reads `leaders[F]` with `F = −1` for a city that never had a founder; the port treats −1 as ineligible.)

### 3.4 Non-city branch (unreached) 🟡

`n = swap_team(unit_who)`; on success the console messages, `objects[unit_who][n].activate(0, 1, 0)`, `close(0, −1, 0)`
on the original, `mask_me(1, REGEN_TOTAL)` on the copy, return 1; on failure the building dies (`die(0, −1, 0)`) and
the function returns 0. It also reads `cities[owner][Build.city]` for the stamps before branching, i.e. it assumes a
city record. No stock type reaches it (§1).

### 3.5 `UnitData::get_capture_value` @610a70

0 for a decoy or a siege unit (`unit_flags & 0x20000`); else `min(uber_size (+0x308), guy_mark)`, at least 1. One
Hoplite chain member (uber 3, guy_mark 1) is worth 1; a 5-figure squad is worth 1 (uber 1).

## 4. `Cities::capture_city(new_who, idx, old_who)` @733380 ✅ (1200 lines; decompile + asm)

### 4.1 Prelude

1. If `leaders[old].dip[new].agree == 1` (a pending deal): `Leader::action_clear_all(old, new)`.
2. `was_capital = old city CAPITAL`. Persian old owner (tribe bonus 0x17) losing a capital: `find_capital(old, skip
   (idx, old))`; `persian_other = c ≥ 0 and w ≥ 0 and w == old` (another own capital remains).
3. `old.cities_lost (+0x828)++`, `new.cities_captured (+0x824)++`; Steam statistic for the console player (P); both
   leaders RECOMPUTE.
4. `rw = (min(LeaderData::get_radius(old, city type), 64) + 3) / 4` (wcoords, for §4.5).

### 4.2 The city object

List A (old objects to dispose of) starts with the old city object. `n = Build::swap_team(city object, new)` (§5). If
`n ≥ 0`: `objects[new][n].activate(1, 1, 0)` — `loading = 1`: `Cities::init_city` stamps attack / raid / reduce /
capture / assimilation with `frame` (so the assimilation clock and the 75-frame capture guard start now), the name is
left empty, `race / founder` are not set; the founding branch links the new owner's own buildings and swaps foreign
orphans in the radius (`City::find_buildings`, §4.4) — then append `n` to list B (copies to re-mask) and
`plunder = city_plunder_per_level (+0x304, 100) × (level − 1)` of the new city (`new_idx = objects[new][n].city`).
Achievement events (P).

### 4.3 `City::capture(old record, new_idx, new_who, new_o)` @736c40 ✅

Copies into the record `init_city` just created (`new_idx`):
- `city = new_idx`, `who = new`, `o = new_o`, `x / y` = the new object's centre. Scenario bookkeeping
  `ScenarioData::city_lost[old]` / `city_lost_to[old] = new` when scripted rules or CTW run.
- pop: the leader's and the world's pop lose the record's pop value, `pop` (+0x5d) = old pop, the value is added back
  (net zero: `CityData::get_pop_value` depends only on the city object's type).
- `attack_stamp = raid_stamp = frame`; `reg`, `scouted`, `founder` copied.
- `race = new` if old and new are the same leader or mutually allied, or the captor has LEADER_ASSIMILATE_IMMED
  (0x200000), or `has_preq(GLOBAL_GOVERNMENT_BONUS 697)`; else `race = old.race` (unassimilated, §8).
- `plundered`, `was_capital_flags`, `traded_with[8]`, `conquest_node` copied; `city_flags = old & 0xf13f` (drops
  ALARM 0x40 and HAS_TEMPLE / HAS_GRANARY / HAS_LUMBER / HAS_MARKET — `add_to_city` / `activate` set them again as
  the copies join) `| GOT_CAPTURED (0x100)` unless same / mutual allies; name and id copied.
- CAPITAL: `was_capital_flags |= 1 << old`; ORIGINAL_CAPITAL → WAS_ORIG_CAPITAL (0x8000). Unless (old Persian and
  ORIGINAL_CAPITAL): every **unfinished** SENATE (438) of the old leader (objects 2000..build_mark, VALID, not ACTIVE) is
  `Object::disband(1)` (full refund, no draw).
- `city_flags &= 0xbfef` (CAPITAL and ORIGINAL_CAPITAL cleared). If `was_capital_flags` holds the new leader's bit
  (recapturing one's own capital): set CAPITAL, clear the bit; if WAS_ORIG_CAPITAL and `who == founder`: flags =
  `(f & 0x3fff) | 0x4010` (ORIGINAL_CAPITAL | CAPITAL, WAS_ORIG cleared); `race = who`.
- City object `damage = hits(0) − 10`: **a captured city starts with 10 hit points**.

### 4.4 The buildings

If the city object could not be copied (`new_idx == −1`): every building after the city object in the old list
without `'e'` (build_flags 0x10) goes to list A, `plunder += 25` each (they are all destroyed below). Otherwise:
1. `City::capture` (§4.3), then `Armies::update_city(old_o, old, new_o, new)` @6f2d70: armies of every VALID leader
   that is not a pure human seat (`flags & 0xc ≠ 4`) aiming at `(old_o, old)` aim at `(new_o, new)`; the new owner's own
   armies get FORCE_PROCESS (0x80).
2. Walk the old city list from `city_down` of the old city object (old objects stay linked while the walk runs):
   every building **without `'n'` (build_flags 0x2000, "never captured")** goes to list A; a FARM (417) or GRANARY (423)
   of a **Lakota (tribe bonus 0x13) captor** is skipped (it will be destroyed); an ACTIVE one is
   `Build::swap_team(new)` (§5), the copy `activate(1, 1, 0)` and appended to list B, `plunder += 25`. Unfinished
   buildings are not copied. `'n'` buildings stay with the old owner (orphaned when the old record closes).
3. `City::find_buildings(new record)` (production-spec 5.2): links the new owner's own buildings in the radius and
   **captures foreign orphans** (buildings of any other leader with `city < 0` and without `'e'`): `swap_team(new)`; on
   failure the orphan dies (`die(0, −1, 0)`) and find_buildings returns at once (no check_upgrade); else the copy
   `activate(0, 1, 0)`, the original `close(0, −1, 0)`, the copy `mask_me(1, REGEN_TOTAL)`. Then `check_upgrade`.

### 4.5 Units (allied transfers only)

Only when old and new are the same leader or mutually allied (scripted transfers, defeat hand-overs to allies):
over `circle_radius[rw]` wcoords around the new record's (x, y), every **unit** (vslot 8: buildings answer 0) of the
old leader that is VALID and on the map (`is_on_map`): when the old leader has more than one city (`city_num > 1`,
still counting this one) it must lie within `min(get_radius(new, city type), 64)·192` of the city and
`ObjectsData::find_city(x, y, SEARCH_FRIENDLY, new, −, flags 0, FILTER_ALL)` must return this city; then
`Unit::swap_team(new)` (§5.3). In an enemy capture **no unit changes hands, none is ejected or disbanded**: units in
the radius keep fighting.

### 4.6 Plunder

```
despot = a THEDESPOT (352) hero of the new owner within (xs + ys)·96 of the city object (HeroesData::find_hero, gated by
         the leader's live 352 count +0x59be), or a SPITAMENES (365) general on the city object (has_general(0, 365),
         gated by +0x59d8)
russian = old owner has tribe bonus 13 and russian_plunder_steal ≠ 0:
          patch newer than Rise of Legends → (old city was not a capital); older patches → 1
if plunder == 0 and !was_capital: no plunder
elif old GOT_CAPTURED, or (old.capture_stamp ≠ 0 and frame − old.capture_stamp < 4501), or new < 0: no plunder
elif (old flags & 0x400000) or !was_capital or persian_other or team_style == 3:          // ordinary
    to_new = russian ? (despot ? plunder : 0) : (despot ? thedespot_plunder·plunder/100 : plunder)
    to_old = russian ? plunder : 0
    to_new → the NEW leader's poorest good: min stockpile over goods 0..5 that both leaders can use
             (type_avail(·, good, 1)), knowledge (3) excluded, strict '<' from 999999 (ties: lower good);
             added raw to the stockpile (no cap)
    to_old → the OLD leader's poorest good (same goods test), raw
else:                                                                                    // first capital capture
    old flags |= 0x400000
    team_style 1, 2 or 9: plunder = capital_plunder_assassin (+0x30c, 500) × max(1, eliminated), eliminated =
        num_nations − (num_nations − #leaders 0..7 with flags & 3 == 1)
    else plunder = max(plunder, capital_plunder (+0x308, 500))
    to_new / to_old as above, but EVERY usable good (both type_avail, not knowledge) gets the amount: bucket_add for
    the captor (caps apply), raw add for the victim
```
Messages and sounds (P). Note `capital_plunder` and `capital_plunder_assassin` are read from the same XML element
(rules-spec), both 500.

### 4.7 Disposal

List A, in order (the city object is skipped here): unfinished → `Object::disband(0)` (partial refund; no draw for a
building); finished → `close(5, −1, 0)` (dt 5: no ruin, no DEAD_BUILD, no unmask), or `close(0, …)` (destroyed) for a
Lakota captor's farm / granary. Then `mask_me(1, REGEN_SIMPLE)` for every copy in list B.

### 4.8 Capitals

- `was_capital` and `has_wonder(old, FORBIDDENCITY 531)` ∈ {0, 2} (none, or only a CTW one) and `find_capital(old,
  skip (idx, old))` finds no own capital (`c < 0` or `w ≠ old`): `Leader::lost_capital(old, new)` (§6.2).
- The new record is CAPITAL (a recaptured own capital, §4.3) and `find_capital(new, skip (new_idx, new))` finds no
  other own capital: `Leader::recapture_capital(new)`.

### 4.9 Closing the old record

`City::close(old record, attacker = new)` (§6.0; this is where the old leader may be eliminated) and lower
`old.city_mark` past invalid records; the old city object: `city = −1`, `close(5, −1, 0)` (Build::close with no
record: Region::fix_borders, every live city of every leader re-masks its CITY bits, calc_pop_cap). New city object:
`update_hits(0)`, `damage = hits(0) − 10` again, `update_los()`. `Leader::calc_pop_cap(new)`. Returns `new_idx`.

## 5. `swap_team` ✅

### 5.1 `Wall::swap_team(who)` @640c00

`Objects::init_build(who, current_upgrade(who, type), x, y, preplaced 1, −1)` — a full Build::init: a city type is
appended to UnbuiltCities (and an AI owner's first city triggers Build::init's "everyone to the site" group move), others
`find_city` (joining the new owner's city if the CITY bits and a record cover them), gather buildings run
`find_gather_tiles` (**RNG**, production-spec 8.3), farms get no Farms::add (preplaced). Then the copy takes over:
`build_masks` (+0x60), `ever_seen` / `ever_seen_completed` (+0x62 / +0x63); an ACTIVE LIBRARY copy runs
`Build::new_library` (patches newer than Rise of Legends); flags = `(copy & DETECTOR 0x40) | (old & ~0x40)` (VALID,
STARTED, ACTIVE, CITY …); `job_counter`, `job_counter_2`, `constr_time`, `construct_hits`, `damage`, `infiltrated`
(+0x3a), `visible` (+0x40); `update_hits(0)`, `update_los()`. Returns the new o.

### 5.2 `Build::swap_team(who)` @628210

1. MISSILE_SILO (520): `Object::kill_contents`. `Object::eject_contents(1, −1, 0, 1)` (a garrison comes out: Unit::come_out,
   **RNG @61a1aa** per unit). Not a LIBRARY: `clean_queue(1)` (refund).
2. `n = Wall::swap_team(who)`; if `n ≥ 0`: the copy gets `infiltrate` (+0x84), `recharging` (+0x7a), `founder`
   (+0x7f), `max_age` (+0x83) = max(copy, old); a non-flat gather building (vslot 0x90 is_gather and not 0x94 is_flat)
   appends the old claimed gather tiles to its own and takes `mtn / cliff` (+0xb4 / +0xb5), the old ones are cleared;
   a FARM (exact 417) takes the farm record (+0x78) and the record's who / o are rewritten, the old object's slot is
   cleared; `Wall::update_gpiece` (P).
3. SPACEPROGRAM (542): the new owner gets LEADER_REVEAL_ENEMY (0x800) (and the console fog redraw flag).

The original object stays alive; every caller activates the copy and closes or destroys the original itself.

### 5.3 `Unit::swap_team(who)` @6118d0 🟡

A follower delegates to the unit above it (`o_up`) until the captain swaps. The captain: `n = Objects::init_unit(who,
current_upgrade(who, type), x, y, −1, −1, −1)` (a new chain; Guy::init_real **RNG** per guy). Walking both chains along
`o_down`: damage, damage_frac, angle (+0x50), flags (copy's DETECTOR kept), unit_masks (copy's AI_CONTROL 0x40000
kept), form (+0xaa), recharging (+0xae); position (`set_new_location`, or `remove_from_world` for an invalid member);
guy_mark and each guy's state (GuyData +0xc..+0x40 and two bytes; 🟡 the port copies position, angles and turret
angles); `Guy::update_gpiece`. Carried units (`inside_down` chain) are recreated for `who` and put inside the new
carrier (`Unit::go_inside`). Then the old cargo is unlinked and closed, and the old chain closes member by member:
`close(0, −1, 0)` (no death animation, no draw), and because Unit::close re-appends a closed captain behind its
followers (combat-spec 6.2) the walk follows `get_captain` until the last member has closed.

## 6. Elimination

### 6.0 `City::close(attacker)` @737550 (production-spec 5.8) — the parts this spec adds ✅

After the pop / region bookkeeping and before the members are re-homed:
1. Armies: for every VALID leader p, every valid army: if `p == city.who`, `army.city (+0x20) == city index` and the army
   is MUSTERING (`status & 1`): `Army::close`; else if the army targets `(city.o, city.who)`: `target_o = −1`.
2. `city_flags &= ~VALID`, then **`Leader::lost_a_city(was_capital = CAPITAL, own = (race == who), attacker)`**.
3. The member walk: a LIBRARY with queued research hands it to `LeaderData::get_first_library` (the first ACTIVE,
   assimilated, in-city library; the closing one already has `city = −1`): the queue items are copied over that
   library's own (its items are lost) and `queued` moves; without one the queue is refunded (`clean_queue(1)`).

### 6.1 `Leader::lost_a_city(was_capital, own, attacker)` @6dc6d0 ✅ (asm)

```
if balance mode (semaphore byte1 & 2), or (editor and TurnControl paused): return
(console music)
if was_capital and attacker ≥ 0 and elimination == 1 (CAPITAL) and has_wonder(FORBIDDENCITY) == 0:
    for w in 0..7 VALID|ACTIVE with (w == attacker or is_ally(attacker, w)) and has_preq(w, 697 GLOBAL_GOVERNMENT):
        defeat(1, attacker, 1); return
switch elimination:
  2 (CAPITAL SUDDEN DEATH): if was_capital:
        Persian with another own capital (find_capital(−1, −1) → c ≥ 0, w == self): return
        defeat(2, attacker, 0); return
     (not a capital: the default rule below)
  3 (SUDDEN DEATH): if own: defeat(3, attacker, 0); return            // a captured city lost again: nothing
  default (0 conquest, 1 capital): if !(flags2 & NO_CITY_DEFEAT) and city_num == 0: defeat(0, attacker, 0)
```
`city_num` already excludes the closing city. In the default modes a leader is eliminated by losing its last city;
in CAPITAL mode additionally by the capital clock (§6.6).

### 6.2 `Leader::lost_capital(holder)` @6dc870 / `Leader::recapture_capital()` @6d1ba0 ✅

lost_capital, only when the clock is not running (`lost_capital_timer` +0x418 == 0): set it to 1;
`T = get_team_terr(self)`, `E = get_team_terr(holder)`, `a = world land / num_nations` (0 without nations);
`mod = (T + a == 0 or E + a == 0) ? 256 : clamp(256(T + a)/(E + a), 128, 512)`; if both city counts are non-zero and
the holder has fewer cities: `mod = clamp(mod·(own + 2)/(theirs + 2), 128, 768)`; `lost_capital_modifier` (+0x41c) =
mod; `lost_capital_stamp` (+0x414) = `(stamp << 8)/mod + frame` (the stored "minus elapsed" becomes a start frame).
Messages in CAPITAL mode (P). recapture_capital: if running: clear the flag, `stamp = trunc256((stamp − frame)·mod)`
(back to "minus elapsed"). Completing the Forbidden City pauses the clock the same way (production-spec 5.9).

### 6.3 `Leader::defeat(type, attacker, flag)` @6ecb00 ✅

```
defeat_by(attacker, type)                                          // §6.4 — runs even for an already defeated leader
if DEFEATED or balance mode: return
flags2 bit 0x200 = flag;  flags = (flags & ~ACTIVE) | DEFEATED
Armies::leader_defeated(self)                                      // Army::stop for each valid army (§6.5)
flags2 |= UNIT_AI_OFF;  game.musical_chairs = frame;  defeat_type (+0x7dc) = type;  achievement (types 0..4, 8)
every VALID building: clean_queue(0)                               // queues dropped without refund
every VALID unit: planes (vslot 0xc0 is_plane) die(0) (no animation), the others clear_orders; AI_CONTROL cleared
(Player::lose for the seats, notices: P)
if !(game over): Game::check_victory(); if now game over: flags |= 0x80
if multiplayer and not game over: the stockpile is shared — each VALID|ACTIVE ally of self gets, per good both can use
    (not knowledge), bucket_add(scale_tribute(self, self.bucket[g] / #allies)); then self.bucket[0..5] = 0
(end-of-game UI for the console)
```
**Units survive a defeat** (they stand idle; planes die). DefeatIndex: 0 conquest, 1 capital, 2 capital sudden death,
3 sudden death, 4 musical chairs, 5 another leader's victory, 6 resigned, 7 disconnected, 8 Armageddon.

### 6.4 `Leader::defeat_by(attacker, type)` @6d1c80 ✅ (asm for the activate / mask_me arguments)

```
if defeated_by (+0x10) ≥ 0: return                                 // −1 until a conqueror is named
if type ≠ 5:
    humans = #VALID leaders with a human seat (any_humans); live = #of those (≠ self) with flags & 0x63 == 3
    contest = among the other VALID|ACTIVE leaders (stopping at one with VICTORY), two undefeated ones not allied
    if multiplayer and contest and (live ≠ 0 or humans == 0):
        victory == 5 (musical chairs): blow_up_buildings(); blow_up_units()
        else: blow_up_towers(attacker); blow_up_units()
defeat_stamp (+0x6924) = frame
if attacker < 0: return                                            // (defeated_by stays −1)
defeated_by = attacker
if elimination == 3: return
for i in 0..city_mark−1, VALID city records:
    F = founder, Rc = race;  A_active = leaders[attacker] ACTIVE
    if F ≥ 0, F ∉ {self, attacker}, is_ally(attacker, F), A_active: capture_city(F, i, self)
    elif Rc ≥ 0, Rc ∉ {self, attacker}, is_ally(attacker, Rc), A_active: capture_city(Rc, i, self)
    elif self ≠ attacker: n = capture_city(attacker, i, self); if n ≥ 0 and its city object has damage == 0: assimilate
for o in 2000..build_mark−1, VALID buildings:
    unfinished: die(3)
    fort (is_fort), TOWER (439), LOOKOUT (521) or OBJECT_CITY: kept by the loser
    military trainer (BuildTypeData::is_military_trainer) and find_city_at(x, y, attacker, −1, skip 1) < 0: die(3)
    elif self ≠ attacker: n = swap_team(attacker); n < 0 → die(3); else copy activate(0, 1, 0), original close(0, −1, 0),
                          copy mask_me(1, REGEN_SIMPLE)
```
Each `capture_city` closes a record of the loser and calls `lost_a_city` → `defeat` → `defeat_by` (returns at once,
`defeated_by` is set) — the nested `defeat` then runs to completion (it is not yet DEFEATED) inside the outer
`defeat_by`, before the building loop; the outer `defeat` returns early afterwards. Captured cities keep `damage =
hits − 10`, so the "damage == 0 → assimilate" branch practically never fires. Unfinished sites die, forts / towers /
lookouts stay, other buildings change sides (military trainers only inside the conqueror's cities).

Multiplayer helpers: `Leader::blow_up_towers(attacker)` @6d2310 — buildings 2000..mark that are VALID and not cities:
armed ones (type attack +0x1e8 ≠ 0) die(3); unarmed non-wonder military trainers on a tile owned by a third leader w
(`WData.who` ∉ {−1, self, attacker}, not allied to the attacker) die(3). `blow_up_units()` @6d2170 — every VALID unit:
garrisoned units come out first (`Unit::come_out(0)`, **RNG @61a1aa**, and the holder ejects the rest), then die(3)
(death animation **RNG @60fae6 / @60fb24 / @60fb42**). `blow_up_buildings()` @6d22b0 — every VALID building die(3).

### 6.5 Armies on defeat

`Armies::leader_defeated(who)` @6f2f90: `Army::stop` @6f9180 on each valid army: per listed group with members:
Group vslot 0x14 (action_begin; the scenario ignore-orders members are first dropped), and for a unit group
`Group +0x10 = −1` and every VALID on-map member that is not a plane (helicopters count) and not entering / leaving a
transport: MULTIMOVE cleared, `queue_time` (+0xc0) = 0, `close_orders(0)`, `clear_partial_path`, `update_action`,
AUTO_EXPLORE cleared.

### 6.6 `Leader::process_elimination()` @6b8a20 (Leaders::process_all step 4) ✅

CAPITAL mode only, while the capital clock runs: `elapsed = frame − lost_capital_stamp`; if `Game::retake_capital(self)
≤ elapsed`: `find_capital(−1, −1)` → `(c, w)` (the holder of our capital, or `(−1, self)`), `defeat(1, w, 0)`.

`Game::retake_capital(p)` @594530: `base = retake_capital (+0xd04, 3600) < 1 ? 0 : max(1, (ms/2 + xs·retake)/ms)` with
`ms = map_sizes[3].data[0]` (70) and `xs` the map width in wcoords; for `p ≥ 0`: `trunc256(lost_capital_modifier ×
base)`; CTW: ×2 with only a CTW Forbidden City, /2 when the CTW opponent (at war) has one.

## 7. Victory

### 7.1 `Game::check_victory()` @5926b0 ✅ (asm)

```
if game over (semaphore byte0 & 0x40): return
humans = #leaders with a valid human player seat; live = #of those with flags & 0x43 == 3
(balance mode: UnitBalance timeout ends the game)
if live ≠ 0 or humans == 0 (or balance mode):
    first = the first leader with flags & 0x43 == 3; any other such leader not mutually allied with first: return
    (balance mode counts only leaders with live units +0x93c or +0x954)
game over: semaphore byte0 |= 0x40 (TurnControl stops stepping the simulation), recalc borders (P)
limit = armageddon_per_team·num_sides + armageddon_per_nation·num_nations + armageddon; ×3 for starting resources
        7, ×2 for 5 / 6 / 11 / 12; at least 100 for 8
if game.armageddon < limit:
    for i in 0..7 VALID|ACTIVE: flags |= 0x80; skip DEFEATED; the first is the winner; another one that has no VICTORY
        and is not mutually allied with the winner: semaphore byte2 |= 0x40, return
    Leader::victory(winner, 0, 0)
semaphore byte2 |= 0x40; (GameSpy room bookkeeping)
```
So the game ends when every human is out (single player: the player was defeated) or when all surviving leaders are
one alliance.

### 7.2 `Leader::victory(type, flag)` @6ec9b0 ✅

Unless already VICTORY or DEFEATED: `flags |= 0x20`, `flags2` bit 0x100 = flag, `victory_type` (+0x7d8) = type,
achievement, `clean_queue(0)` on every VALID building, Player::win (P); every other VALID|ACTIVE leader gets
`flags |= 0x80` and either `victory(type, flag)` (mutual allies) or `defeat(5, −1, 0)`. Types: 0 conquest, 1 wonders,
2 territory, 4 score, 5 economic, 6 time limit.

### 7.3 `GameDaemon::process_victory()` @730ef0 (frame step 11.2) ✅

1. `Game::get_armageddon() ≤ game.armageddon` → `Game::defeat_all()` @592aa0: every VALID leader with ACTIVE and not
   DEFEATED gets `flags |= 0x80` and `defeat(8, −1, 0)`; every VALID leader `compute_score(1)`.
2. Wonder victory (victory 0, 1, 6): `ww = Game::wonder_winning()` (ai-spec 14). For each VALID|ACTIVE leader i: not on
   the winner's side (`ww < 0`, or `i ≠ ww` and not mutually allied) → stop i's wonder clock if running (`wonderwin_timer`
   +0x44c = 0, `wonderwin_stamp` +0x448 −= frame). On the winner's side: `gg` = some VALID leader that is i or a mutual
   ally of i has GLOBAL_GOVERNMENT_BONUS. If `victory == 0` and (multiplayer or num_nations > 1) and !gg: start the clock
   if stopped (flag 1, stamp += frame); else, for the first winner-side leader of the pass only, `left = wonder_timer() −
   (frame − stamp)` and **`left == 0`** → `victory(i, 1, 0)`. Otherwise (victory 1 / 6, a single nation, or gg), for the
   first winner-side leader: `victory(i, 1, gg)`. `Game::wonder_timer` @5944f0 = map-scaled `wonder_timer` (+0xd08).
3. Territory victory (victory 7, 0, 1; multiplayer or num_nations > 1; not balance): `need = popwins[popwin].data[0]`
   (percent), `L = world land size` (World +0x78). For each VALID|ACTIVE i (L ≠ 0): rivals = other VALID|ACTIVE leaders,
   not mutually allied with i, with `team_terr·100/L ≥ need`. `team_terr(i)·100/L < need` or rivals → stop i's
   territory clock (`popwin_timer` +0x444, `popwin_stamp` +0x440). Else, with gg as above: victory 0 and !gg → start
   it, or `left = popwin_timer() − (frame − stamp) < 1` → `victory(i, 2, 0)`; otherwise `victory(i, 2, gg)`.
   `Game::popwin_timer` @5944b0 = map-scaled `popwin_timer` (+0xd10).
4. Score victory (victory 3): the VALID|ACTIVE leader with the highest `get_team_score` (strict '>' from 0, so ties keep
   the lower index; the team average only with semaphore byte0 & 0x80) reaching `scores[score_goal].data[0]` →
   `victory(best, 4, 0)`.
5. Time limit (victory 4): `limit = time_limits[time_limit].data[0]` (minutes; index ≥ 8: scenario / CTW
   `ScenarioData::custom_time_limit` or 300, else −1). While `tick < limit·60` only warnings (P; the sound uses
   SoundGlobal's own Random, not game_random). Then game_rules 8: the first VALID|ACTIVE leader on team 0 wins
   (`victory(·, 6, 0)`); else the best team score wins (type 6). Port decision: a scenario file with the custom slot
   but neither scripted rules nor CTW (the conquest maps played outside Conquer the World, which the port does not
   model) uses the 300-minute default instead of −1 (which would end the game at frame 0).
6. Musical chairs (victory 5, `frame ≠ 0`): every `chairs[chairs].data[0]·900` frames since `game.musical_chairs`:
   the stamp moves to now; with free-for-all team styles (0, 8, 11 or fewer than two members on the smallest team) the
   VALID|ACTIVE leader with the lowest score (+0x18) is defeated (`defeat(4, −1, 0)`) unless the lowest score is tied;
   with teams, the lowest-scored member of each team 0..3 (unless tied).
7. Economic victory (victory 8): the highest `get_team_economic` reaching `econwins[econwin].data[0]` →
   `victory(best, 5, 0)`.

### 7.4 The clocks (Leaders::process_all step 5) ✅

Every `timer_refresh_ratio` (+0xd00) frames, per ACTIVE leader: each stopped clock whose stamp is negative recovers one
frame (`lost_capital_stamp`, `popwin_stamp`, `wonderwin_stamp` += 1 when the paired flag +0x418 / +0x444 / +0x44c is
0). A running clock's stamp is its (shifted) start frame; a stopped one holds minus the elapsed time.

### 7.5 Game settings

`ScenarioRead::import` @9b1430 copies the header's `elimination`, `victory`, `score_goal`, `time_limit`, `wonderwin`,
`popwin`, `econwin` (scenario-objects-spec §2, 0xf8..0x110) into GameInfo, after `Game::init_rules_and_teams` (whose
team-style default for `wonderwin` therefore does not apply to scenarios).

## 8. Assimilation ✅ (production-spec 5.6 step 5, 5.4)

A captured city keeps `race = old race` (unless same / allied, ASSIMILATE_IMMED or GLOBAL_GOVERNMENT) and carries
GOT_CAPTURED: it does not heal (§2.3), counts as unassimilated for production prices (`BuildData::is_unassimilated`,
production-spec 2), and its libraries are not first libraries. In Build::process (after the capture poll): a THECITIZEN
(356) hero on the city (`has_general(0, 356)`) adds `1 − thecitizen_assimilation_speed` to `assimilation_timer` per
frame; `e = frame − assimilation_timer`, Turks (8) `e = (turk_assimilate + 100)·e/100`, the founder retaking it `e =
(reassimilation + 100)·e/100`; `assimilation_timer (+0xd30, 2000) ≤ e` → `City::assimilate` @738e90: clear
GOT_CAPTURED; if `race ≠ who`: `race = who`, Region::fix_borders (region < 64); **Chinese (14) with
chinese_large_cities: the city object set_type(TOWN 415, 0)** (no mask, pop or stats update: a Major City or the
Forbidden City is downgraded), RECOMPUTE; for each member after the city: an ACTIVE LIBRARY re-runs `new_library` with
ACTIVE cleared around the call; an ACTIVE SENATE spawns the government hero (`Build::train(get_gov_hero)`) when none is
near (find_unit FILTER_GOV_HERO, 0x20000) and `gov_hero_frame ≤ frame`. Message (P). Because `City::init(loading = 1)`
stamped `assimilation_timer = frame` at the capture, assimilation takes 2000 frames (Turks 667, the founder 500).

## 9. RNG draws, in order (`game_random`; every draw is `Random::get(0, 0xffff)`) ✅

`Build::check_capture` itself draws nothing, nor do `Cities::capture_city`, `City::capture`, `City::close`,
`Leader::lost_a_city`, `lost_capital`, `defeat`, `victory`, `check_victory`, `process_victory` (its sounds use
SoundGlobal's Random). The draws of a capture come from the functions it calls, in this order:
1. `Build::swap_team(city object)`: Build::init of the copy (a city has no gather tiles: no draw); garrison ejection is
   impossible here (check_capture requires an empty city).
2. `activate(1, 1, 0)` of the new city: none.
3. Old-list walk, per swapped ACTIVE building in `city_down` order: `eject_contents` → `Unit::come_out` per garrisoned
   unit (@61a1aa, + `Guy::set_anim` variant draws); Build::init of the copy → `find_gather_tiles` shuffle for non-flat
   gather buildings (woodcutter, mine; production-spec 8.3); then `activate(1, 1, 0)` → `Farms::add_animals` for a farm
   whose moved record is a pen (5 animals × 3 draws, farms-spec 6) and the animals' own `init_unit` draws.
4. `City::find_buildings(new)`: per foreign orphan swapped, as 3 (+ `activate(0, 1, 0)`).
5. Allied transfers only: per unit chain `Objects::init_unit` → `Guy::init_real` (1 per guy).
6. Disposal: none (buildings; a destroyed farm's animals die with dt 0).
7. `City::close` → `lost_a_city` → `defeat`: planes die with dt 0 (no draw); multiplayer `blow_up_units` /
   `blow_up_towers`: die(3) per unit (death animation draws @60fae6 / @60fb24 / @60fb42 per unit, combat-spec 6.2),
   `come_out` for garrisons; `defeat_by`'s hand-overs repeat 1-6 per city, then per building swapped as 3.

Corrections to combat-spec §8: items 1 and 11 list `Object::disband` @6459c3 and `Unit::plunder` @605fca / @606159 in the
capture chain. Capture disbands only **buildings** (unfinished list-A sites, unfinished Senates of a lost capital),
which draw nothing; no unit is disbanded or plundered by a capture or a defeat. `Unit::plunder` runs from
`Object::disband` of an Aztec-damaged unit (`unit_masks & 0x4000`) and from `Object::take_damage` deaths (combat-spec
5.4), not from capture.

## 10. Iteration order and determinism notes

- Strength scan: circle order (combat-spec 2.17.1), then each wcoord list newest first; only the sums matter, the order
  only affects the `visible` / update_local_seen side effects (whose fog writes are order-independent).
- New owner: leaders 0..7, strict '>' (lower index wins ties).
- Buildings: the old city list order (`city_down`), then find_buildings' leader 0..7 × object order; disposal in list-A
  order (city list order).
- Units (allied): circle order × list order.
- Plunder: goods 0..5, strict '<' for the poorest good.
- `defeat_by`: city records in index order, then buildings 2000..build_mark.
- Victory: leaders 0..7 throughout.
- A capture during a target scan (`valid_target` → `check_capture`) is safe for the scan (§2.5).

## 11. Data read

Constants: `city_capture_radius` 10 (+0x134), `unit_respond_range` 12 (+0x18), `city_plunder_per_level` 100 (+0x304),
`capital_plunder` / `capital_plunder_assassin` 500 (+0x308 / +0x30c), `thedespot_plunder` (+0xc70),
`russian_plunder_steal` 1 (+0x770), `recapture_city_modifier` 512/256 (+0x6c; combat-spec 5.2), `city_heal_rate` 4
(+0xc10), `assimilation_timer` 2000 (+0xd30), `turk_assimilate` 200 (+0x664), `reassimilation` 300 (+0xd2c),
`thecitizen_assimilation_speed` 4 (+0xc9c), `chinese_large_cities` (+0x788), `retake_capital` 3600 (+0xd04),
`wonder_timer` (+0xd08), `popwin_timer` (+0xd10), `timer_refresh_ratio` (+0xd00), `armageddon` / `_per_nation` /
`_per_team` (+0xd14..+0xd1c), `lakota_raze_price` (Object::disband). Category lists: `map_sizes` (entry 3), `wonderwins`,
`popwins`, `scores`, `time_limits`, `chairs`, `econwins`. Type data: `domain` +0x218, `attack` +0x1e8, `obj_masks`
+0x1e4, `uber_size` +0x308, `unit_flags` 0x20000 (siege), `build_flags` 'e' 0x10 / 'n' 0x2000.

## 12. Port status (engine/sim, 2026-10-13)

Ported: §2 (incl. the take_damage city flags with the citizen / militia exception, the do_damage tail with
Army::charge and reduce_stamp, the 64-frame poll and AI repair call, FILTER_NOT_BUSY), §3, §4, §5.1, §5.2, §5.3
(🟡 guy fields), §6 (all of it; blow_up_* only run in multiplayer), §7.1-7.4 (musical chairs and economic victory
TODO: Game.on_team and LeaderData::get_economic are not ported), §7.5, §8 (the existing assimilation, with the
`chinese_large_cities` condition added). `ron-sim` prints capture / defeat / victory / game-over events and the
victory clocks (`--ai-every`); `Game::events` is a tool log, not simulation state.

Approximations (TODO in code): heroes (`HeroesData::find_hero`, `has_general`: the despot / Spitamenes plunder bonus
and THECITIZEN never apply), garrisons (`num_inside` = 0 in the strength scan, no ejection in swap_team, no AI alarm),
transports (Unit::swap_team's cargo), scripted-rules `city_lost` bookkeeping, CTW retake adjustments, the editor pause
test of lost_a_city, balance mode, the caravan repath of City::close; the conquest maps' custom time limit (§7.3);
`GameInfo.num_nations` (the "multiplayer or more than one nation" test of the wonder / territory clocks) is the port's
default 2, so single-nation scenarios can win by territory (`Game::init_rules_and_teams` @589bb0 sets it from
`GameInfo.players + 1` in single player).

## 13. Corrections to other specs

- production-spec 5.7: "the attacker side (unit_who, its allies at war with the owner)" — the side test is
  `is_ally(unit_who, w) and is_enemy(owner, w)` ✅; the defender gets 6 / 12 **plus the building's own value 1 plus its
  garrison** per own building, and only buildings not under attack count (FILTER_NOT_UNDER_ATTACK). The non-city branch
  kills the building only when swap_team fails (it does not "then die").
- production-spec 5.7 item 4 (units): units change hands only for same-leader / mutual-ally captures (§4.5); there is no
  unit ejection or disbanding in an enemy capture.
- production-spec 5.8: `Leader::defeat_by` hands cities to the conqueror (or founder / race allies) and **destroys
  unfinished sites and outside military trainers, keeps forts / towers / lookouts with the loser and gives the rest to the
  conqueror** (§6.4); `blow_up_towers` runs in multiplayer only.
- combat-spec 5.2: the recapture modifier tests `cities[whom][city].race` (+0x5f), not the founder (+0x60).
- combat-spec §8 items 1 and 11: see §9.
- ai-spec 12.4.5: when the target city is nearly taken the army moves onto the centre; the capture then comes from the
  units' attacks (do_damage tail) and valid_target scans, not from the army code.

## 14. Open questions

1. ❓ Does the original ever reach the non-city branch of `check_capture` (a scenario object of a city type without
   OBJECT_CITY)? The port keeps it.
2. 🟡 Unit::swap_team's GuyData copy (+0xc..+0x40, +0x9d, +0xa2) is mapped to the port's guy fields by position only.
3. 🟡 The FILTER_NOT_UNDER_ATTACK exemption means a defender's building that was hit within the last ~400 frames (the
   0x20 bit decays in Wall::process) adds nothing; confirm with a replay that captures a city while its towers are
   under fire.
4. ❓ `Leader::defeat` calls `Game::check_victory` from inside `Cities::capture_city` (via City::close) before the old city
   object is closed; the game-over flag only stops the next frame, so the capture completes. Replay check pending.

## 15. Evidence

| Function | Address | File |
|---|---|---|
| Build::check_capture | 6276a0 | `re/decomp/prod_c2b/`, `re/decomp/capture/asm/` |
| BuildData::check_capture_eligible | 62d1d0 | `re/decomp/prod_c2b/` |
| UnitData::get_capture_value | 610a70 | `re/decomp/orders/` |
| Search::valid_filter (cases 8, 12; table @67e57c) | 67dbb0 | `re/decomp/ai_port/Search_valid_filter_body.asm` |
| Cities::capture_city | 733380 | `re/decomp/prod_c2/`, `re/decomp/capture/asm/` |
| City::capture | 736c40 | `re/decomp/prod_c2/` |
| City::close | 737550 | `re/decomp/prod_c2/`, `prod_c2_asm/` |
| City::assimilate | 738e90 | `re/decomp/prod_c2/` |
| City::find_buildings | 7384c0 | `re/decomp/prod_c2_asm/` |
| Build::swap_team / Wall::swap_team / Unit::swap_team | 628210 / 640c00 / 6118d0 | `re/decomp/orders/`, `re/decomp/capture/` |
| Armies::update_city / leader_defeated, Army::stop | 6f2d70 / 6f2f90 / 6f9180 | `re/decomp/capture/` |
| Leader::lost_a_city | 6dc6d0 | `re/decomp/capture/asm/` |
| Leader::lost_capital / recapture_capital | 6dc870 / 6d1ba0 | `re/decomp/capture/` |
| Leader::defeat / defeat_by | 6ecb00 / 6d1c80 | `re/decomp/econ_mkt/`, `prod_c2b/`, `re/decomp/capture/asm/` |
| Leader::blow_up_towers / _units / _buildings | 6d2310 / 6d2170 / 6d22b0 | `re/decomp/prod_c2/`, `re/decomp/capture/` |
| Leader::victory | 6ec9b0 | `re/decomp/capture/` |
| Game::check_victory / defeat_all | 5926b0 / 592aa0 | `re/decomp/prod_c2b/`, `re/decomp/capture/asm/` |
| Game::retake_capital / wonder_timer / popwin_timer | 594530 / 5944f0 / 5944b0 | `re/decomp/prod_c2b/`, `capture/` |
| Leader::process_elimination, Leaders::process_all | 6b8a20, 6ed2a0 | `re/decomp/prod_c2b/`, `econ_trade/` |
| GameDaemon::process_victory | 730ef0 | `re/decomp/prod_c2b/` |
| HeroesData::find_hero | 73a1b0 | `re/decomp/capture/` |
| Object::disband, Unit::plunder | 6455c0, 605b00 | `re/decomp/capture/` |
| Object::do_damage (city tail), Object::take_damage | 64a480, 652020 | `re/decomp/combat/` |
| Build::process (poll, assimilation) | 61edf0 | `re/decomp/combat_C/` |
| ScenarioRead::import (game settings) | 9b1430 | `re/decomp/scenario/` |
