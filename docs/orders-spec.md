# Commands → groups → orders → `Unit::work`

Status: ✅ order types, order list structure and queue positions, the per-frame order machine (`Unit::work`,
`update_action`, `do_job`, `kill_current_order`), the `add_*_order` pattern · 🟡 command verb → `Group::action_*` → unit
calls (§2, from a sub-pass over the decompiles) · ❓ not validated against a running game (no replay with unit commands
exists on this Mac). Behaviour inside the individual `do_<order>` handlers (movement, pathfinding, combat, gathering) is
only outlined (§6); each needs its own spec.

Related: [replay-spec.md](replay-spec.md) §8–§9 (command bytes), [frame-spec.md](frame-spec.md) (where commands and
`Unit::process` run), [state-spec.md](state-spec.md) / [state-walk.md](state-walk.md) (how orders are saved).
Decompiles: `re/decomp/replay/` (issuers, `process_*`), `re/decomp/cmds/`, `cmds2/`, `halt/`, `grp_asm/`, `mv2/`, `atk/`,
`atk2/`, `gth2/`, `bld2/` (Group, Groups, verbs),
`re/decomp/orders/` and `re/decomp/orders2/` (Unit, UnitData, orders, list primitives), `re/decomp/frame/`.

Conventions: as in frame-spec.md (XORed coordinates, `who`/`o` object addressing, flag enums from the PDB). "uid" is
`Object.uid` (+0x30), a 16-bit serial that changes when a slot is reused; orders that target an object store
`(ox, whom, uid)` and treat a uid mismatch as "target gone".

## 1. Groups: the GROUP command and the `Group` objects

### 1.1 Layout and slots

`Group` (0x9d4 bytes, `groups.obj`): +4 `id`, +8 `army`, +0xc `num`, +0x10 `form`, +0x14 `stamp`, +0x18/+0x1c `ox/oy`,
+0x20 `o_dist`, +0x24 `o_angle`, +0x28 `disband`, +0x2c `order_num`, +0x30 `priority`, +0x34 `role`, +0x38
`think_frame`, +0x3c `new_speed`, +0x40 `speed`, +0x44 `form_num`, bytes +0x48 `facing`, +0x49 `buildings`, +0x4a
`who`, +0x4b `march`; then six 128-entry arrays `off_x`, `off_y`, `curr_x`, `curr_y` (int), `angles` (char), `list`
(short object indices). Virtuals: +0 `is_select_group`, +4 `get_num`, +8 `get_num_cap`, +0xc `add`, +0x10 `kill`,
+0x14 `action_begin`.

`Groups` holds **512 slots: 64 per player** (`groups[who·64 + k]`), `last_group[8]` (+0x1c) and `proc_group` (+0x40).
`Groups::clear` @713f20: every slot cleared with its own index as id, `stamp = priority = 0`, `last_group[p] = p·64`,
`proc_group = 0`. A unit records its slot in `UnitData.group` (+0x80, −1 = none). Groups are saved and checksummed
(`Group::walk_data` = raw [+4, +0x4c) then the first `num` entries of each array; state-spec.md §6 part 6), so slot
choice and every field written below are part of the contract.

### 1.2 `CommandPackage::process_group` @94a0c0 (op 0)

1. If `cmd.who` ≠ the sender seat's leader (`GameInfo.player[play].who`), the command is **ignored** — teammates
   included: `LeaderData::is_team` is called but its result is discarded (verified in the disassembly at 94a1f5–94a20b),
   and `package.group` is **left unchanged** (not reset to −1), so following verbs act on the previous current group.
2. Build a temporary Group (`clear(−1)`, `id = −1`):
   - explicit list (`num > 0`): `last_num_received[play] = num`; for each `o` in order: during playback a null slot is
     reported ("broken replay") and skipped; if the object is alive and (not a unit, or `o < unit_mark[who]`):
     `Group::add(o, who, 0, 0)`, store `o` and its uid in `last_objects_received / last_uids_received[play][k++]`, and
     for units set `unit.play` (+0xb6) = sender seat and walk the `o_down` chain doing the same; finally
     `last_num_received[play] = k` (the count of objects actually kept).
   - repeat (`num == 0`): for each stored entry whose object is alive with the same uid: the same add/play/chain walk.
     The stored arrays are not updated.
3. If anything was added: `package.group = Groups::push_group(who, &tmp, 1)`, else `package.group = −1`.
4. Returns `3 + 2·num`.

`CommandPackage.group` is initialised to −1 only when the package object is constructed or `init`ed
(`CommandManager::init`, `CommandPackage::init`); `process_all` and `clear` do not reset it. In single player the local
package's current group therefore survives from one package to the next. Issuers always send GROUP first, so this only
matters for hand-made or corrupt streams.

### 1.3 `Group::add(o, who, down, const)` @714350 and `Group::kill` @714110

`add`: accepted only if the group is empty (then `who` is taken from the argument) or `who` matches. A non-captain unit
(`o_up ≥ 0`) added with `down == 0` is redirected to its captain (`add(o_up)`); with `down ≠ 0` a captain is first
`kill`ed. If the group is empty or `buildings == is_wallbuild(o)` (units and buildings never mix), the object is not yet
a member and `num < 128`: append (`off/curr/angle = 0`, `list[num++] = o`, `buildings` set, `stamp = frame`); for valid
units OR `ptype.role` into `role` and recurse into a live `o_down` with `down = 1`; if `id ≥ 0`, `compute_speed`.

`kill` mirrors it (non-captains redirect to `o_up`; followers recursed first), then removes the member: if
`unit.group == id` it becomes −1; `disband = 0`; arrays shifted down; empty → `clear(−1)` (keeps `id`), otherwise
`stamp = frame`; `speed = new_speed = UnitData::speed(find_leader())` for unit groups with a leader, else 0.

`find_leader` @70ccb0: among valid captains on the map, the first with the strictly lowest `FormData::type_cat`; a
second pass without the on-map requirement (🟡 the decompile is muddled).

### 1.4 `Groups::push_group(who, g, force)` @70f9e0

1. (`force == 0 && num < 2`: release the members' `unit.group` and return −1; never taken from `process_group`.)
2. If `g` equals `groups[last_group[who]]` (`equals_group`: normalises whichever side has `id ≠ −1`, then compares
   `who`, `get_num()`, `buildings` and the member list **in order**), reuse that slot as is (no copy, no stamp).
3. Otherwise `slot = get_open_slot(who)`, `copy_group(groups[slot], g)`, `last_group[who] = slot`.
   `copy_group` @6fa690 copies only `who, num, ox, oy, o_dist, o_angle, buildings, speed` and the six arrays (first
   `num` entries) and sets `stamp = frame`. **`form, army, disband, order_num, priority, role, think_frame, new_speed,
   form_num, facing, march` keep the previous occupant's values** (checksummed: reproduce exactly).
4. For each valid unit member: if `unit.group ≥ 0` and ≠ slot, `groups[unit.group].kill(o, who, 0, 0)`; then
   `unit.group = slot`.

`Groups::get_open_slot(who)` @6fa460 scans slots `who·64 .. who·64+45` (46 slots; 46..63 are never handed out here):
1. the first slot ≠ `last_group[who]` whose virtual `get_num()` is 0 or that holds buildings. `get_num` compacts dead
   members (and normalises when `num < 4` and `id ≥ 0`), so **every slot scanned is mutated**;
2. otherwise the oldest "single" group: `stamp ≤ best` (best starts at `frame`, so ties go to the later slot) and
   `get_num_cap() == 1` (one captain), not `last_group`;
3. fallback (logs "UH OH"): the highest slot with `stamp ≤ frame` that is not `last_group`.
If the chosen slot previously held a unit group, every unit of `who` in `[0, unit_mark)` with `group == slot` gets
`group = −1` (no alive check).

### 1.5 Maintenance and lifetime

`Group::normalize` @711540 and the per-frame `Groups::process` @6fa210 (frame-spec.md §5) remove, back to front,
members that are `< 0`, dead, or (`priority == 0`, `id ≥ 0`, not a building, and `unit.group ≠ id`); then
`find_role` (OR of member roles) and the speed recompute. `Groups::process` visits one slot per VALID leader 0..7 per
frame (`who·64 + proc_group`), then `proc_group = (proc_group + 1) % 64`.

Groups outlive the command: GROUP_MOVE / GROUP_ATTACK / GROUP_ATTACK_TO orders keep the group id (`GroupOrder.id`),
and `Unit::do_group_move` / `do_group_attack` / `repath` work through `Group::kill_group_order`, `kill_group_move`,
`kill_next_order`, `refresh_group_order`, `distribute_attack` and `update_positions` on `groups[id]`.

## 2. Unit verbs: command → `Group::action_*` → unit calls

### 2.1 Common shape

- **Issuer** `CommandManager::issue_<verb>`: `check_accept_issue` @940a70 (refuses during MP playback and in some
  network/turn states), then `add_group` + `add_command` (nothing is sent if `add_group` fails). `add_group` @94bb60
  keeps selected objects that are alive and (not units, or `o < unit_mark`) and, for units, only **captains**; it emits
  `num = 0` when who/objects/uids equal the last group it sent, and fails if nothing is valid. `issue_attack` also
  requires `ox ≥ 0 && whom ≥ 0`. No other simulation preconditions on the sender side.
- **`process_<verb>`**: logging, then `if package.group ≥ 0: groups[package.group].action_<verb>(…)`. Verbs with an
  object target (ATTACK, GARRISON) additionally require `whom < 0 || ox < 0 || objects[whom][ox]` alive. No uid,
  ownership or diplomacy checks at this level.
- **Prelude** of almost every action: if `ScenarioData::ignore_orders` and `who < 8`, members listed in
  `objects_ignoring_orders[who]` are `kill`ed from the group; then `action_begin()` (vfunc +0x14: `disband = 0`).
- **Member iteration** is in `list` order unless noted. A unit is skipped unless valid, on the map (`is_on_map`) and
  not a plane (air units are handled by the FLIGHT verbs).
- **QUEUE_FIRST** at group level (MOVE_TO, FORM, ATTACK, GARRISON) is implemented as: `set_up_insert` (save the
  leader's orders that have flag 0x04), `action_halt`, run the verb with QUEUE_NEW, `finish_insert` (re-issue the saved
  orders as QUEUE_LAST group actions).
- **RNG:** none of these paths draws from `game_random` directly; the one known draw is in ATTACK through
  `Unit::find_attack_pos` (§2.5).
- `semaphore byte1 & 8` (scenario editor) skips cost payment and lets garrison/gather go inside directly.

### 2.2 HALT (op 12): "stop"

`process_halt` @949140 → `Group::action_halt(0)` @70d0c0: prelude; unit groups only: `form = −1`; for each member that
is valid, on the map, not a plane and not entering/exiting a building (`is_entering_or_exiting`): the body of
`Unit::clear_orders` (clear `UNIT_MULTIMOVE`, `path.length = 0`, `close_orders(0)`, `clear_partial_path`,
`update_action`), then clear `UNIT_AUTO_EXPLORE`. Building groups: nothing. The unit is idle the next time `work()`
runs and `do_idle`/`think` may immediately give it new orders (auto-attack, auto-gather).

### 2.3 STANCE (op 2)

**Correction to replay-spec.md §9:** the argument is a stance *option* within the group's stance type, not the type:
`CombatStanceIndex` 0–5 (AGGRESSIVE, DEFENSIVE, STAND_GROUND, RAID, RAZE, HOLD_FIRE), `WorkerStanceIndex` 0–3,
`CasterStanceIndex` / `PackerStanceIndex` 0–1; −1 = next, −2 = previous.

`Group::action_stance(s)` @70d440: requires the group on the map; `action_begin`; `T = get_stance_type()` (the
leader's, or `list[0]`'s for buildings; NONE/CASTER fall back to the first member with another type, else CASTER);
options n = 6/4/2/2; for s < 0, `cur` = the most common option among members (lowest on ties) and s = cur±1 mod n.
Then for each alive member in list order: buildings of type T: `Build.stance` (+0x7e) = s; units of type T (not
planes): `stance` (+0xb1) = s, `flags |= NEW_THINK`, and for COMBAT: s ∈ {0, 3, 4} → `Unit::clear_mandatory` (zero the
mandatory byte of every ATTACK order); s ∈ {1, 2, 5} → for human owners, if the current order and action are not
group-level (flag 0x04), `repath` + `kill_current_order(0)`; other s → `clear_orders`. (`Unit::set_stance`, which walks
the uber chain, is used by LEADER_OPTIONS, not here.)

### 2.4 MOVE_TO (op 7)

`process_move_to` @9497c0 → `action_move_to(x, y, q, set_angle, angle, orders, 1, form, width, disembark)` @70fba0 →
`action_move_near(…, tolerance 0, …)` @704990:

1. Prelude; clamp x/y to the map. **Split** into units on land/not inside (A) and ships / containers of carried units
   (B); if both are non-empty and the group has no army, or only B is non-empty and the target tile is land (then
   transports go to A), `push_group` each part, recurse on each, `clear` this group and return.
2. Unit groups only. QUEUE_FIRST → the insert pattern (§2.1).
3. No leader or plane leader → return. `group.form` = the requested form (9 or −1 → the members' common `unit.form`,
   else ≥ 0); width −1 → average `form_mod` (default 50); origin = `get_loc_to` (QUEUE_LAST) or `get_loc`. AI armies
   (non-human leader, `army ≥ 0`) have extra city/hurry logic.
4. QUEUE_NEW: `clear_orders` for each valid, on-map, non-plane member (AI siege units attacking in an army and packing
   packers keep some orders).
5. **`Group::compute_form`** @707c80: `Form::categorize` first calls **`Group::sort`, which reorders `list`** so
   followers sit next to their captains; then integer formation layout (`Form::compute`, `sin_table`) giving per-slot
   destinations and angles and the leader slot `F.idx`; writes `off_*`, `angles`, `form_num`; `disband = 0`;
   `o_angle/ox/oy` = angle/x/y.
6. For each member (post-sort order; valid, on map, not a plane): AI hurry special cases; set `unit.form` (not for
   types 50–53) and `form_mod`; clamp the slot destination; for i > 0 check land regions against slot 0 (relocate with
   `find_nearby_spot`, or pick a barge/ship crossing); then
   - **group move** when `orders` ∈ {1 MOVE_TO, 2 ATTACK_TO}, the unit is not modern infantry, not an explorer
     (`role & 0x10`) without `UNIT_AI_CONTROL`, `num > 1`, not flagged, not a ship (unit domain 1; the target tile is
     not tested) and `form ≠ 9`:
     `Unit::add_group_move_order(tile48(dx), tile48(dy), (angles[i] << 24) + angle, move_id = (id + frame·10)·100 +
     order_num, slot i, leader = list[F.idx], …, orders, q, explicit = 1, reverse, x, y, disembark)` @5e4710 →
     GROUP_ATTACK_TO for orders 2, else GROUP_MOVE (flags 0x01, 0x04, 0x20 = disembark);
   - otherwise `Unit::add_move_facing_order(tile48(dx), tile48(dy), angle, orders, 1, q, explicit, reverse, x, y,
     disembark)` @5e55c0 → ATTACK_TO / EXPLORE_TO / FLEE_TO for 2/3/4, else MOVE_TO;
   - then clear `UNIT_BUILDER`.
7. `update_positions`; one group path `PathFinder::find_wpath` from the leader to its slot (skipped within 0x900),
   whose waypoints are copied into every member's `path` with that member's slot offset (members path individually if
   the group path is empty); `order_num++`.

Hazard: with `form == 8`, intermediate waypoints read a static `cols` array that is never filled (uninitialised read
in the original); check whether real replays use form 8.

FORM (op 3) → `action_form(form, rotate, q, …)` @707220: pick the formation (−1 next, −3 previous of 5, −2 keep),
QUEUE_NEW/FIRST use the insert pattern, set `unit.form` on all non-plane units, then for LAST/NEW
`action_move_to(get_loc_to(), QUEUE_LAST, rotate ? 1 : 0, base + rotate, MOVE_TO, explicit 0, …)`.

### 2.5 ATTACK (op 4)

`process_attack` @949c30 → `Group::action_attack(ox, whom, mandatory = 1, q, ignore)` @712490 (signature from the
mangled name `?action_attack@Group@@QAEXHHHW4QueuePos@@H@Z`):
1. `ox < 0` or not on the map → return; prelude.
2. Building groups: the three stores of `Build::add_attack_order` are inlined for each member (`build_masks |= 4`,
   always; `attack_ox` / `attack_whom`); `Build::add_attack_order` @622ce0 itself has no callers. `q` ignored; return
   (no `order_num++`).
3. QUEUE_FIRST → insert pattern.
4. If the leader is out of range (from its position, or its queued location for QUEUE_LAST):
   `Unit::find_attack_pos(leader, …, p3 = 1, p8 = 1)` @601280. Its result only feeds UI feedback, but it draws
   `game_random` **only for building/wall targets**, once per *valid* perimeter candidate and capped (combat-spec
   3.8.4, 3.11), and it can clear `mandatory` on the leader's front attack (3.8.3). Eligible enemy cities:
   `Build::check_capture` and a MOVE_TO instead; return (no `order_num++`).
5. Three passes over the members by `domain` (0 land, 1 water, 2 air), list order within each: skip invalid/off-map
   units and those excluded by `ignore` (4 siege, 2 special, 1 spy); planes only retarget a current STRAFE. A member
   is left alone when its action is already a **mandatory** attack on this target, it has at most 2 orders and it is
   out of range from its position — unless it has 2 orders and the front move does not end in range. Otherwise:
   unit and wall targets: `add_move_order(target, …, q)` first when a *unit* target is unseen (with QUEUE_NEW the
   attack's own clear removes that move again — the original behaves like this, replicate it); a non-mandatory
   attack retargets each member through `find_melee_target(min(dist + 192, unit_respond_range·576), …, filter =
   wall ? 2 : 1)`; then `Unit::add_attack_order(target, q, mandatory, explicit = 1)` @5e5410. Unseen *building*
   targets get only the move (plus `check_capture` when already in range). Packers packing/unpacking (only with
   QUEUE_NEW and a seen target): `clear_orders`, cast UNPACK (652) only when already in range, PACK (651) only when
   out of range, then the attack with QUEUE_LAST (combat-spec 3.3.1 step 8).
6. `order_num++`.

### 2.6 GATHER (op 19)

`process_gather` looks the target up in the **group owner's** list. `Group::action_gather(ox, q)` @700b90 (QUEUE_FIRST
behaves as NEW): prelude; `form = −1`; return if the target's resource is not available (`type_avail`); count spare
idle workers; walk members **back to front**: non-workers (and scholars unless the target is a university) go to a
temporary group; workers already gathering are skipped while spare workers remain; workers in another land region are
skipped unless the leader may transport them. Then search outward from the target (spiral `move_x/move_y[k]`, k < 121,
4×4-tile cells) for the first own, valid, active gather building; for each assigned worker: `unit.group = −1`,
`Unit::add_gather_order(b, q, explicit = 1)` (§4.2, registers with `Build::add_gatherer` while below `gather_max`),
an optional civilian spell cast, and `kill` from the group. Remaining members go to the temporary group, which gets
`action_move_near(target, 0xc0, QUEUE_NEW, …, MOVE_TO, …)`.

GARRISON (op 20) → `action_garrison(ox, whom, q, 0)` @700490: QUEUE_FIRST insert pattern; `whom` must be the owner or a
mutual ally (diplomacy value 2); target a valid building with garrison room (or an airbase), assimilated; for each
eligible member (a unit already garrisoning is `repath` + `kill_current_order`): `Unit::add_garrison_order(ox, whom,
find, q', explicit = 1)` @5e4080 with q' = QUEUE_FIRST for workers, QUEUE_LAST after a packing cast, else q.

### 2.7 BUILD (op 25)

`Group::action_build(x, y, x2, y2, type, q)` @707510 (**x2/y2 unused**): `validate_build` (snap to the footprint
centre, `blocked_site`, city lookup, `can_pay_cost`), checks (valid members, city limit, `type_avail == 4`), snap and
re-check, **`pay_cost`** (not in the editor), `Objects::init_build(who, type, x, y, 0, −1)` (the site exists from
now on, unstarted; no refund if creation fails), then `action_swarm_around(o, who, q, BUILD_AT, 1)` @70fbe0: two
passes (land, water) in list order; non-builders go to a temporary group that gets a MOVE_TO; each builder gets a spot
on a ring around the site (`find_nearby_spot`, radius `min(xs, ys)·96 + 48`, halved for farms), then
`add_move_facing_order(spot, …, q', explicit 0, …)`, an optional cast, and **`Unit::add_build_order(o, who,
QUEUE_LAST, 1)`**. Details from production-spec §3.4 (port: `engine/sim/construct.cpp`): busy members
(`UnitData::is_busy`) are skipped, and so are gathering citizens when the site is a gather building and the group has
idle citizens (`count(PEASANTS) − count(GATHER_PEASANTS) − count(BUILD_PEASANTS)` ≠ 0); a worker for whom
`find_nearby_spot` finds no spot gets **no order at all**; AI builders approach with EXPLORE_TO (human: MOVE_TO); for
BUILD_AT the spot is pushed one cell (48) outward when free; a QUEUE_LAST build or repair given explicitly to a gathering
citizen becomes QUEUE_NEW (it drops the gather). The same function serves SWARM_AROUND (op 6) for BUILD_AT (6),
REPAIR (13) and GATHER (7); the temporary group of non-workers always moves with QUEUE_NEW.

### 2.8 QUEUE_UP (op 24): train / research

`Group::action_queue_up(type, num)` @6fdbb0, building groups only. The member list is copied and exchange-sorted by
`Build.queued` (+0x82, ascending). Units and buildings: `num` rounds, each calling `Build::queue_up(type, 1)` @620f40
on **every** active member (so `num` is per building). Research: skipped if already being researched; first over
buildings with an empty queue, then all; stops at the first success. `Build::queue_up` (frame-spec.md §10): library
redirect, `can_pay_cost` / `could_queue` (`can_make`, `queued < queue_size`, scholar cap 6), `pay_cost` (not in the
editor), append the item, `num_queued[type]++`. Population is checked only when the item completes
(`Build::finished`). Corrections from production-spec §6.2 (port: `engine/sim/queues.cpp`): the second argument of
`Build::queue_up` is "use escrow" (1 from the command, 0 for the repeat re-queue and scenario queues), not a count;
"units" means unit types whose tech bit is set and building types without `build_flags & 4` (in-place upgrades), all
other types take the research path; missile silos skip unit types they cannot carry.

### 2.9 GATHER_POINT (op 22): rally points

`Group::action_gather_point(x, y, action, add_to_end)` @6ff1b0, buildings only (units have no rally point). x < 0 or
y < 0 → `clear_gather` on the eligible members. `add_to_end` is forced to 0 if any member gathers inside; action 3
(airbases) also issues FLIGHT; a village-only group snaps the point to a nearby woodcutter/mine; clicking on a building
sets "inside" (Terracotta/Kremlin/Senate clear it). Each eligible member: `Build::add_gather_point(x, y, action,
add_to_end ? QUEUE_LAST : QUEUE_NEW)` @622e70, or `(−1, −1, 0, QUEUE_NEW)` for "inside". The points are applied to each
trained unit by `Unit::come_out` (frame-spec.md §10). Eligibility (production-spec §6.6): universities and missile
silos never take a rally point; a member also needs a training building as its basic type, a garrison limit, or to be
Terracotta / Kremlin / Senate. QUEUE_LAST appends the point at the tail (a waypoint), QUEUE_NEW clears the list first.

### 2.9a BUILDMASK (op 33), DISBAND (op 21), UNQUEUE (op 48)

`Group::action_buildmask(mask, set)` @6fc9a0 **ignores `set`**: it toggles (the first valid building decides, once one
building is cleared all later ones are cleared); only INFINITE (0x40, repeat) and REPEAT_AIR (0x80) are accepted.
`Group::action_disband(all)` @70e260 disbands units (`die(rnd(3) + 1)`, the only draw) and unfinished sites (refund),
and queues a DISBAND item on finished buildings (timed demolition); see production-spec §4.9. UNQUEUE is not a group
verb: object (who, o) with a matching uid; production-spec §6.5.

### 2.10 Signatures observed

`Unit::add_attack_order(int ox, int whom, QueuePos, int mandatory, int explicit)`;
`add_gather_order(int ox, QueuePos, int explicit)`; `add_garrison_order(int ox, int whom, int find, QueuePos, int
explicit)`; `add_build_order(int ox, int whom, QueuePos, int explicit)`; `add_move_order(Coord x, Coord y, int orders,
int p4, QueuePos, int explicit, int, Coord orig_x, Coord orig_y)` → `add_move_facing_order(x/48, y/48, angle, orders,
p4, q, explicit, facing = −1, orig_x, orig_y, 0)`; `add_cast_order(int ox, int whom, Coord x, Coord y, int spell,
QueuePos, int explicit)`; `Build::queue_up(TypeIndex, int) → 0 on success`; `Build::add_gather_point(x, y, action,
QueuePos)`.

## 3. Order objects

### 3.1 Types

`OrderIndex` (PDB enum) → class created by `::get_new_order` @730550 (used by `OrdersMemManager::get_obj` and the save
loader). Sizes and own fields from `rise.pdb`. Every concrete order has `UnitOrder` (`vfptr`, `char flags`) as a
**virtual base**, so the `flags` byte sits at the end of the object (at `vbtable[1]` from the vbptr); `GroupMoveOrder`,
`StrafeOrder`, `AirPatrolOrder`, `AirAttackGroundOrder`, `GroupAttackOrder`, `GroupPatrolOrder` have two non-virtual
bases sharing that virtual base.

| Id | `OrderIndex` | Class (size) | Own fields (offset) |
|---:|---|---|---|
| 0 | NONE | — (no object) | |
| 1 | MOVE_TO | `MoveOrder` (0x5c) | x 4, y 8, angle 0xc, dest 0x10, tolerance 0x14, pause 0x18, retry 0x1c, attempts 0x20, timer 0x24, facing 0x28, dest_x/y 0x2c/0x30, last_x/y 0x34/0x38, coll_x/y 0x3c/0x40, orig_x/y 0x44/0x48, off_x/off_y (i16) 0x4c/0x4e |
| 2 | ATTACK_TO | `AttackToOrder` : MoveOrder (0x5c) | attack-move |
| 3 | EXPLORE_TO | `ExploreToOrder` : MoveOrder (0x5c) | |
| 4 | FLEE_TO | `FleeToOrder` : MoveOrder (0x5c) | |
| 5 | PATROL | `PatrolOrder` (0x4c) | `SimpleArray<Coord>` x_pos 4, y_pos 0x20; waypoint 0x3c. **`get_new_order` returns no object for 5** (only its subclasses 17/22 are created) |
| 6 | BUILD_AT | `BuildOrder` : TargetOrder (0x20) | |
| 7 | GATHER | `GatherOrder` : TargetOrder (0x34) | tx 0x14, ty 0x18, build_type 0x1c, wait 0x20, goto_build 0x24, non_flat_gather 0x25, dist_mod 0x26, been_there 0x27 |
| 8 | BOARD_SHIP | `BoardOrder` : TargetOrder (0x20) | |
| 9 | AWAIT_BOARD | `AwaitBoardOrder` : TargetOrder (0x20) | |
| 10 | ATTACK | `AttackOrder` : TargetOrder (0x30) | def_x/def_y 0x14/0x18, mandatory 0x1c, defensive 0x1d, in_range 0x1e, ever_in_range 0x1f, new_ord 0x20 |
| 11 | FOLLOW | `FollowOrder` : TargetOrder (0x2c) | oxx 0x14, whose 0x18, uid2 0x1c |
| 12 | GUARD | `GuardOrder` : TargetOrder (0x38) | dx, dy, guard_x, guard_y, idle, retry (0x14..0x28) |
| 13 | REPAIR | `RepairOrder` : TargetOrder (0x20) | |
| 14 | CAST_SPELL | `CastOrder` : TargetOrder (0x30) | x 0x14, y 0x18, paid 0x1c, spell 0x20 |
| 15 | TRADE_ROUTE | `TradeOrder` : TargetOrder (0x34) | oxx 0x14, whose 0x18, started 0x1c, loaded 0x20, uid2 0x24 |
| 16 | STRAFE | `StrafeOrder` : AttackOrder, AirOrder (0x54) | xx 0x40, yy 0x44 |
| 17 | AIR_PATROL | `AirPatrolOrder` : PatrolOrder, AirOrder (0x68) | |
| 18 | CHANGE_FORM | `FormOrder` : MoveOrder (0x64) | newform 0x50, delay 0x54 |
| 19 | GROUP_MOVE | `GroupMoveOrder` : MoveOrder, GroupOrder (0x78) | GroupOrder at 0x50: oxx, whose, id, form_id, group_angle; in_group 0x68 |
| 20 | GROUP_ATTACK | `GroupAttackOrder` : AttackOrder, GroupOrder (0x54) | temporary 0x3c, oxxx 0x40, whosoever 0x44 |
| 21 | GROUP_ATTACK_TO | `GroupAttackToOrder` : GroupMoveOrder (0x78) | |
| 22 | GROUP_PATROL | `GroupPatrolOrder` : PatrolOrder, GroupOrder (0x64) | |
| 23 | ATTACK_GROUND | `AttackGroundOrder` (0x20) | att_x 4, att_y 8, accuracy 0xc, attack_unit 0x10 |
| 24 | AIR_ATTACK_GROUND | `AirAttackGroundOrder` : AttackGroundOrder, AirOrder (0x48) | |
| 25 | SPECIAL_ANIM | `SpecialAnimOrder` (0x2c, non-virtual UnitOrder base) | type 8, started 0xc, frames 0x10, data1..4 0x14..0x20, ox 0x24, whom 0x28 |
| 26 | GARRISON | `GarrisonOrder` : TargetOrder (0x24) | search 0x14 |
| 27 | THINK | `ThinkOrder` (8, non-virtual base) | — |

Shared bases: `TargetOrder` (ox 8, whom 0xc, uid 0x10 (u16)); `AirOrder` (oxx, whose, cruising_alt, sharp_turn, old,
returning); `GroupOrder` (oxx, whose, id, form_id, group_angle).

### 3.2 `UnitOrder.flags`

| Bit | Meaning (evidence) |
|---|---|
| 0x01 | Move orders: "path needs (re)computing / not yet at the waypoint". Set by `add_move_facing_order` when its 5th argument is non-zero and by `do_move` when a repath is required; cleared on arrival (`Unit::do_move` @5f7b30) |
| 0x08 | "Return to the defend point" move: set by `fight` / `find_new_target` on the move a DEFENSIVE attacker queues when it gives up; read by `find_def_pos` (combat-spec 3.2.1) |
| 0x10 | Toggled by `fight` on the action after it queues an approach move (re-runs `work` the first time); also on the action by the captain-rewrite path (combat-spec 4.3.3) |
| 0x04 | **Group-level / explicit order** (`UnitOrder::print_details` @72ff80 prints the internal string "group level"). Set from the `add_*_order` "explicit" argument by player commands; always set on THINK. Move orders with this bit count as an *action* in `update_action` (§4.2), and a recharging unit keeps working only if its current order has it (§5) |
| 0x20 | Move orders: set from `add_move_facing_order`'s last argument (❓ meaning not traced) |
| 0x80 | Cleared by `add_attack_order` (❓) |

The save format walks this byte once per virtual-base path (state-spec.md §5).

### 3.3 Virtual interface (slot offsets from the PDB)

`+0x00` dtor, `+0x04 clear`, `+0x08 walk_data`, `+0x0c log_data`, **`+0x10 get_type`**, `+0x14 is_move`, `+0x18 is_attack`,
`+0x1c is_move_attack`, `+0x20 is_targeted`, `+0x24 is_pathed`, `+0x28 is_fleeing`, `+0x2c is_group`, `+0x30 is_air`,
`+0x34 is_patrol`, `+0x38 print_details`, `+0x3c..+0xb0 update_<kind>_order` (28 accessors returning the order as the
named subclass, or null), `+0xb4..+0x124 get_<kind>_order` (const versions). In C++ these are just `dynamic_cast`s; the
`is_*` predicates are:

| Predicate | True for |
|---|---|
| `is_move` | MoveOrder and every subclass (MOVE_TO, ATTACK_TO, EXPLORE_TO, FLEE_TO, CHANGE_FORM, GROUP_MOVE, GROUP_ATTACK_TO) |
| `is_targeted` | TargetOrder subclasses (BUILD_AT, GATHER, BOARD_SHIP, AWAIT_BOARD, ATTACK, FOLLOW, GUARD, REPAIR, CAST_SPELL, TRADE_ROUTE, STRAFE, GROUP_ATTACK, GARRISON) |
| `is_attack` | ATTACK, STRAFE, GROUP_ATTACK, ATTACK_GROUND, AIR_ATTACK_GROUND |
| `is_group` | GROUP_MOVE, GROUP_ATTACK, GROUP_ATTACK_TO, GROUP_PATROL |
| `is_patrol` | AIR_PATROL, GROUP_PATROL |
| `is_air` | STRAFE, AIR_PATROL, AIR_ATTACK_GROUND |
| `is_pathed` / `is_fleeing` | MoveOrder overrides (`is_fleeing` true for FLEE_TO) |

(Derived from the folded slot targets in `re/analysis/vt_orders.txt` and the symbol aliases in `re/map/functions.tsv`;
the exact `is_pathed` condition was not decompiled ❓.)

## 4. The order list

### 4.1 Structure

`UnitData.orderlist` (+0xc8) is an `OrderList` = `LinkListBase<UnitOrder*, uchar, RecycledOrderNode>`: `current_data`
+0xcc, `current_metric` +0xd0, `current_node` +0xd4, `length` +0xd8, `head_node` +0xdc, `ordered` +0xe0. Nodes
(`RecycledOrderNode`, 0x10 bytes: `next`, `prev`, `data`, `metric`) form a **circular doubly linked list**.

- `add(order)` @46d5a0 inserts the new node **before** `head_node` and makes it the new `head_node`
  (`n.next = head; n.prev = head.prev; head.prev.next = n; head.prev = n; head = n`). `metric` is always 0.
- The order being executed is **`head_node->prev`** (the oldest node). Every accessor (`UnitData::get_order` @60b030,
  `Unit::update_order` @6179d0, `UnitData::order_type` @616e80) starts with `current = head_node->prev`.
- Walking towards newer orders uses `prev` again: after `add(A); add(B); add(C)` the execution order is A, B, C.
  (`LinkListBase::tail` @46d700 = `head.prev`, i.e. the front; `prev` @46d6e0 steps to the next order to execute.)
- `remove_current` @46d620 unlinks `current_node` (moving `head` to `head.next` if it was the head), sets `current` to
  `next`, and returns the node to the recycler. The order object itself is released separately (`give_obj`).

Equivalent model for a reimplementation: **a deque in execution order**, `front()` = current order.

### 4.2 Queue positions (`QueuePos`)

Every `Unit::add_<kind>_order(…, QueuePos q, …)` follows the same pattern (e.g. `add_attack_order` @5e5410,
`add_build_order` @5e5210, `add_gather_order` @61a5c0, `add_move_facing_order` @5e55c0, `add_think_order` @5e3df0):

```
if q == QUEUE_NEW (2):                      // replace everything
    unit_masks &= ~UNIT_MULTIMOVE; path.length = 0
    close_orders(0)                         // kill every order, front first (§4.3)
    clear_partial_path(); update_action()
o = ordmgr.get_obj(TYPE); fill fields      // target uid captured now: objects[whom][ox].uid (0xffff if ox/whom < 0)
set/clear flags bit 0x04 from the "explicit" argument
orderlist.add(o)                            // → back of the queue (QUEUE_LAST behaviour)
if q == QUEUE_FIRST (0):
    clear_partial_path()
    head_node = head_node->next             // rotate: the new node becomes head.prev = the FRONT
update_action()
```

So `QUEUE_LAST` (1) appends (shift-queue), `QUEUE_FIRST` (0) pushes in front of the current order **without** removing
it (the interrupted order resumes when the new one completes: used internally, e.g. "move into range, then continue the
attack"), and `QUEUE_NEW` (2) replaces the queue. `add_think_order` always behaves as QUEUE_FIRST.

Type-specific extras seen in the adders:
- `add_move_order` @616ed0 → `add_move_facing_order(tile48(x), tile48(y), find_angle(…), …)`, where
  `tile48(c) = div_3_table[c >> 4]` = c / 48. The stored target is snapped to the centre of a 48-unit cell:
  `x = tile48 × 48 + 24`. `off_x/off_y = x % 768, y % 768` (position inside the tile); `dest_x/y = x/y`;
  `last_x/y = −1`; `angle`, `facing`, `orig_x/y` from the arguments; `dest = pause = retry = timer = 0`.
  The "orders" argument picks the class: 2 → ATTACK_TO, 3 → EXPLORE_TO, 4 → FLEE_TO, otherwise MOVE_TO. With QUEUE_LAST,
  orders = 1 and a unit whose `role & 0x10`, the move becomes EXPLORE_TO and `UNIT_MULTIMOVE` is set.
  With QUEUE_NEW and orders = 2 (attack-move) while the unit is currently attacking a valid target (not an aircraft unless
  the unit can hit air), the attack is re-added in front (QUEUE_FIRST) after the new move.
- `add_attack_order(ox, whom, q, mandatory, explicit)` @5e5410: `mandatory` → +0x1c; `in_range = ever_in_range = 0`;
  `new_ord = 1`; `flags &= ~0x80`; if the unit's combat stance is DEFENSIVE (1) and the order is not explicit,
  `defensive = 1` and `def_x/def_y` = `find_def_pos` — the existing defensive attack's defend point, else the
  `(x, y)` of a front move with flag 0x08, else the centre of the unit's current 48-cell (called after the QUEUE_NEW
  clear, before the new order is added); else `defensive = 0`, `def_x/y = −1` (combat-spec 3.2.1).
- `add_build_order(ox, whom, q, explicit)` @5e5210: sets `UNIT_BUILDER`; if the target is in a different land region
  (`WorldData::get_tregion`) and the owner may transport this unit class, sets `UNIT_CAN_TRANSPORT`.
- `add_gather_order(ox, q, explicit)` @61a5c0: the target is always in the **unit's own** list (`whom = who`); clears
  `UNIT_BUILDER`; stores the building's type in `build_type`; registers with `Build::add_gatherer` when the building
  takes gatherers; sets `non_flat_gather` / `dist_mod` (4 or 10) by building kind.

### 4.3 Finishing orders

`Unit::kill_current_order(no_side_effects)` @5e2cb0 is the only way an order leaves the front:
1. Clear `UNIT_CASTING`.
2. Type-specific cleanup on the front order (skipped parts when the game is loading or `no_side_effects`):
   - move family (1–4, 18, 19, 21): if `facing ≥ 0`, set/clear `UNIT_FACING` (inverted when `reversing`), and if this
     unit is the leader of its group, copy that into the group's facing byte;
   - GATHER: mark the leader `LEADER_RECOMPUTE`; `Build::remove_gatherer` on the target if it is still a building of the
     same owner; clear `UNIT_WITH_STUFF` bits; peasants/scholars (types 50–53) drop held doobers;
   - TRADE_ROUTE: `end_trade_route`; CAST_SPELL: refund unpaid casting costs (`SpellType::unpay_cast_costs`) if `paid`;
   - SPECIAL_ANIM type 1: remove the unit from the target's `launching` list.
3. `remove_current`; if the order was a pathed move, `kill_current_path`; `ordmgr.give_obj(order)`.
4. `clear_partial_path()`, `update_action()`.

`Unit::close_orders(x)` @5e37f0 calls `kill_current_order(x)` until the list is empty. `Unit::clear_orders` @5e3860 =
clear `UNIT_MULTIMOVE`, empty the path, `close_orders(0)`, `clear_partial_path`, `update_action` (the QUEUE_NEW
preamble). `Unit::kill_current_action` @5e2bb0 = `repath` + `kill_current_order(0)`.

### 4.4 `update_action`: the action behind a chain of moves

`Unit::update_action` @60a870 (and the const twins `UnitData::get_action` @608450 / `get_next_action` @608230):

```
orders_x, orders_y = unit position; dest_angle = angle
if list empty: return null
walk from the front: while (order.is_move and not (order.flags & 4)) or order.type == CHANGE_FORM, and not at the newest:
    if it is a move: orders_x/y/dest_angle = that move's x, y, angle
    step to the next order
if the order reached is a move with flags & 4 (or the front itself is a non-move): record its x/y/angle as above
return null if the order reached is still a plain move or CHANGE_FORM, else that order
```

`orders_x/orders_y` (+0x70/+0x74) and `dest_angle` (+0x58) are therefore "where this unit will end up". The returned
**action** (e.g. the ATTACK after an internal "move into range") is what `work()` checks for target validity.

## 5. `Unit::work` @60d180: the per-frame order machine

Called from `Unit::process` for every alive unit on the map (frame-spec.md §9).

```
cur  = front order (or null); type = cur ? cur.get_type() : NONE
every 32 frames (staggered by o): if !(flags & ATTACKING): visible(+0x40) = 0; unit_masks &= ~UNIT_IN_COMBAT
if type != ATTACK:
    flags &= ~ATTACKING
    if UNIT_AUTO_EXPLORE and cur and (cur.flags & 4) and type != EXPLORE_TO: clear UNIT_AUTO_EXPLORE
    if recharging and ptype.max_range == 0 and !(ptype.unit_flags & MELEE_AND_RANGED):
        if !cur or !(cur.flags & 4): return                  // melee unit still recovering from its last blow
caravans (is_caravan and the caravan's state & 6) with order != CAST_SPELL:
    if the action is TRADE_ROUTE: Caravan::build_road when pending; else end_trade_route unless the next order is TRADE_ROUTE
if type in {MOVE_TO, ATTACK_TO, EXPLORE_TO, FLEE_TO, CHANGE_FORM, GROUP_MOVE, GROUP_ATTACK_TO}:
    if type == MOVE_TO and UNIT_MULTIMOVE: re-issue the front move as EXPLORE_TO in front (QUEUE_FIRST) and re-read cur
    if the unit is not a packer, or is packed:
        every 16 frames (staggered): act = update_action()
            if act is targeted and act.type != AWAIT_BOARD:
                GUARD: every 64 frames (staggered) repath()
                else if type != CHANGE_FORM and (cur is not a group order or this unit is the group order's leader oxx/whose):
                    check_target_path(act.target)            // may re-plan the path towards a moving target
            (after a repath / re-plan: cur = update_order(), type re-read)
    else (unpacked packer):
        if it is a group-level move and the only order and already within block_radius of the target: rewrite the
            target to the current position and truncate the path; else if not already in the target 48-cell:
            add_cast_order(PACK spell 651, QUEUE_FIRST)       // pack up before moving
else if type != NONE and (flags & IDLING): leader |= LEADER_RECOMPUTE for a few types; flags &= ~IDLING
if a partial path search is pending (openlist) and cur is not a move: clear_partial_path()
act = update_action()
if !act or !act.is_targeted(): spell_time = 0 (unless type == CAST_SPELL)
else if act.target (ox, whom) ≥ 0:
    if act.type == ATTACK: set_in_danger(this, 0) and, if the target is a unit, set_in_danger(target, 0)
    if objects[whom][ox].uid != act.uid:                     // target died / slot reused
        STRAFE: clear the strafe target, mark it returning; continue
        is_attack(act): if act is the front order: act.ox = act.whom = −1 and RETURN (the attack handler retargets next frame)
        otherwise: repath(); kill_current_order(0); RETURN
    TRADE_ROUTE: same uid check for the second endpoint → repath(); kill_current_order(0); RETURN
if the unit carries aircraft (inside_down ≥ 0) and can_carry(AIR): Object::do_launch
safe (+0xb2) countdown
if type != NONE: idle(+0xb0) = 0; a stealth unit that is not cloaked/marching gets UNIT_BLOW_COVER
if frame − 4 < collide_frame: detect_boat_collision for ships that are not moving
do_job(type, cur)                                            // §6
helicopters (UNITTYPE_HELICOPTER) without an air order: push apart from a same-type helicopter within 0x180
unit_masks &= ~UNIT_ZOC
```

Every 32 frames (staggered) `visible = 0` only while ATTACKING is clear, but UNIT_IN_COMBAT is cleared
unconditionally. For an ATTACK action with a target, `set_in_danger(0)` runs on the unit and (when the target is a unit)
on the target every frame.

`Unit::check_target_path(target)` @5e22d0 (returns 1 when it changed the plan; full spec combat-spec 3.7): an
**unseen target returns 0** (no repath). CAST_SPELL re-validates the spell target; BOARD_SHIP never re-plans. A unit
target in front of the attacker (±60°) or flanked but standing: in range from the current position → `repath`
(the approach stops early), else 0 without examining the planned end point. Buildings and fleeing units are checked
against `orders_x/orders_y`: in range from there → 0. A target that left the map → `repath`. Otherwise, for attacks:
melee units on their last waypoint and building targets keep the plan; group orders are re-issued through
`Group::action_attack` (after `kill_next_order`, when the group is only moving) instead of `find_attack_pos`; plain
attacks `repath` and run `find_attack_pos` from the current position. The DEFENSIVE leash measures the **unit's**
distance from its defend point (`≥ max(max_range, unit_defensive_respond_range) × 192`), applies only to
non-mandatory, non-explicit attacks with `defensive` set, and ends in `find_new_target` (no move added). FOLLOW
re-plans when its target moved more than 0x900 from the path end. `Unit::repath` @5e29b0 kills the move-family
orders at the front of the queue (MOVE_TO, ATTACK_TO, EXPLORE_TO, FLEE_TO, CHANGE_FORM, GROUP_MOVE, GROUP_ATTACK_TO) until a non-move order is in front; a GROUP_MOVE of a grouped unit is removed through
`Group::kill_group_move(order.id)` instead. The action handler then plans a fresh approach.

## 6. `Unit::do_job` @617a10 and the handlers

`do_job(type, order)` is a plain switch:

| Type | Handler | Outline |
|---|---|---|
| NONE | `do_idle` (vfunc +0x184) | §6.1 |
| MOVE_TO, FLEE_TO | `do_move` @5f7b30 | below |
| ATTACK_TO | `do_attack_to` @5f2320 | `do_move`, then if the order is still current and `(o + frame) % 15 == 0`: armed non-supply units look for a melee target (`find_melee_target`) unless an AI army keeps them in formation; others `do_attack_to_pause` |
| EXPLORE_TO | `do_explore_to` @5f24a0 | `do_move`, then every 15 frames (staggered) scouts (`is_captain`) look for goody boxes |
| BUILD_AT | `do_build` @5eebf0 | |
| GATHER | `do_gather` @5ef2a0 | |
| BOARD_SHIP / AWAIT_BOARD | `do_board` / `do_await_board` | |
| ATTACK | `do_attack` @5f1b80 | below |
| FOLLOW / GUARD / REPAIR | `do_follow` / `do_guard` / `do_repair` | |
| CAST_SPELL / TRADE_ROUTE / STRAFE | `do_cast` / `do_trade` / `do_strafe` | |
| AIR_PATROL / CHANGE_FORM | `do_air_patrol` / `do_form_change` | |
| GROUP_MOVE / GROUP_ATTACK / GROUP_ATTACK_TO / GROUP_PATROL | `do_group_move` / `do_group_attack` / `do_group_attack_to` / `do_patrol` | |
| ATTACK_GROUND / AIR_ATTACK_GROUND | `do_attack_ground` / `do_air_attack_ground` | |
| SPECIAL_ANIM / GARRISON | `do_spec_anim` / `do_garrison` | |
| THINK | `do_think_order` @5e5bf0 | `kill_current_order(0)`; if no other order remains and the unit is a peasant/scholar (types 50–53): `think_peasant(1)` (auto-gather) |
| PATROL (5) | — | never created |

### 6.1 Idle: `do_idle` → `check_idle` → `think`

`Unit::do_idle` @60dcd0: reset the animation to default (heroes keep an ATTACK2 pose), `collide = 0`, `check_idle`,
`think`. `Unit::check_idle` @6032c0 advances
the **idle counter** `UnitData.idle` (+0xb0, u8; `work()` sets it to 0 whenever the unit has an order): 0 → 1 → 2 on
consecutive idle frames, then +1 only on frames where `(o + frame) % 16 == 0`; 0xff is turned into 1 (so it cycles
instead of saturating). At 4, an entrenched unit re-faces its entrench angle. Marines (`UNIT2_MARINES`) entrench once
`idle ≥ constants.americans_marine_entrench`. Cloak particles (UI). Finally, if `IDLING` is not set yet:
`flags |= IDLING | NEW_THINK` and, for some types, the leader gets `LEADER_RECOMPUTE`.

`Unit::think` @5f6e40 is the autonomous behaviour of an idle unit (all of it adds orders through the `add_*_order`
API, usually `QUEUE_NEW`):
- non-captain members of a formation copy their captain's current attack;
- runs only on the first idle frame, or every 16/32 frames (staggered) afterwards, and never for leaders with
  `LEADER_UNIT_AI_OFF` (flags2) or units with `UNIT_AI_OFF`;
- military units: engage a nearby attacker (`near_o/near_who`) in range → `add_attack_order(QUEUE_NEW)`; otherwise
  `think_attack` (auto-acquire within the stance's rules), merchants `think_merchant`;
- ACTIVE leaders: peasants `think_peasant` (auto-gather / build), caravans `think_caravan`, rare collectors gather;
- human leaders (`LEADER_HUMAN`) and AI-off games: spell casters `think_spellcaster`, scouts `think_scout`, idle
  notifications (UI); AI-controlled units (`UNIT_AI_CONTROL`): `think_fish`, `think_merchant`, transports, heroes, …

The `think_*` functions decide most of what units do without player input, and several consume `game_random`; they
are the next thing to specify after movement and combat.

### 6.2 `do_move` @5f7b30 (outline)

The full movement and pathfinding model is being written up separately in [movement-spec.md](movement-spec.md); this
is only the shape of the handler as seen from the order machine.

1. Cavalry-archer style units (`UNITTYPE_FIRE_WHILE_MOVING` with an attack) run `cavarch_fight` (every 32 frames when no
   target is cached).
2. **Incremental pathfinding in progress** (`openlist` +0x104 non-null): continue the search
   (`PathFinder::find_upath_restore`), count a repath for the player every 4 frames (staggered, `GameDaemon.repaths`),
   possibly retarget an attack or finish a gather approach early; return.
3. `timer`: counts down; at 1 the move ends (`kill_current_order`, then `work()` again in the same frame).
4. If the action behind the move is an ATTACK whose target is valid and already in range (and not blocked), the move
   ends early.
5. **Waypoints**: the unit's path is a `Stack<PathData>` (`UnitData.path` +0xb8; `PathData = {to_x, to_y, tolerance,
   flags}`). When no waypoint is active (`dest == 0`), pop the next one into `dest_x/dest_y`, `tolerance` (+0x60) and
   check region changes (`get_tregion`), road building for caravans and unit collisions; on reaching a waypoint with
   `flags & 1` (the final one) the move order completes (`kill_current_order`).
6. Path planning when needed: hierarchical searches `PathFinder::find_wpath` (world/region level), `find_tpath`
   (tile level) and `find_upath` (unit level), and `Unit::find_path`. A random detour threshold uses
   `game_random.get(0, 0xffff) % 5` (0x600 / 0xf00 / 0x1800 world units).
7. Otherwise step along: speed from `get_speed` (vfunc +0x17c) × `ai_speed`, ×1.25 for modern infantry; `move_step`.

### 6.3 `do_attack` @5f1b80 (outline)

1. Aircraft carriers (`is(AIRCRAFTCARRIER)`) do not fight themselves: while the target is alive, every 16 frames
   (`frame & 15 == 0`, not staggered) each aircraft in the carrier's `inside_down` chain gets
   `add_strafe_order(target, …, QUEUE_NEW)`; a dead target drops the order.
2. Units without an attack value (`ptype.attack == 0`):
   - transports (`is(TRANSPORTBARGE)` class test) carrying a unit (`inside_down`) that can hit the target move to an
     attack position for it: `find_attack_pos` twice (the second with "ignore units", p8 = 1), falling back to the
     target's position; the move is **explicit** (`add_move_order(…, QUEUE_FIRST, explicit 1)`); or drop the order;
   - members of a group (supply wagons, workers, other non-combatants ordered along with an army): stand-off
     `D = (r + 2)·192` (+192 supply, +192 worker) with `r = GroupData::get_max_range(worker)`, or `(siege max range +
     1)·192` (+384 / +384) when `r == 0`, plus the target building's half footprint; within `D + 0x240` drop the order,
     else `find_nearby_spot` around `project(away from target, D + 192)` in the target's land region,
     `kill_current_order` and `add_move_order(…, QUEUE_FIRST)`; otherwise drop the order (combat-spec 3.5);
   - a non-attacker that is **not** in a group goes on to `fight`.
3. Mandatory attacks by fire-while-moving units cache the target (`cavarch_o/uid/who`, `UNIT2_KEEP_TARGET`); they
   never use the stand-ground swap below.
4. **`Unit::fight(ox, whom, mandatory, 0, 0)`** @5fd4d0: the combat core (combat-spec 4.3). For non-mandatory
   `MELEE_AND_RANGED` units in combat stance 2 (stand ground) the code sets the *type's* `max_range` (ptype +0x1fc,
   0 for every `k`-flag type after loading, so the reset is exact) to `second_max_range` (+0x2d0) for the call and
   back to 0 afterwards; otherwise, if `fight` returns non-zero, `cavarch_fight` runs with `recharging` temporarily 0
   (only for `MELEE_AND_RANGED` types).

## 7. Minimal set for moving and attacking

To make units walk and fight from player commands, a first implementation needs: GROUP with the full slot logic of
§1 (groups are checksummed and their ids are stored in orders); HALT, MOVE_TO and ATTACK (§2); order types MOVE_TO,
ATTACK_TO, GROUP_MOVE, GROUP_ATTACK_TO, ATTACK and THINK; the list semantics of §4; `work()` of §5 (caravans, packers
and aircraft can wait); `Group::compute_form` (including the `Group::sort` reordering) for formation slots; `do_move` /
`do_group_move` with the real PathFinder ([movement-spec.md](movement-spec.md)); `do_attack` / `fight`; and `do_idle`
with `think_attack` for auto-acquisition. Everything that draws from `game_random` (`find_attack_pos`, the detour
threshold in `do_move`, combat rolls, `Guy` animation variants, `Guy::init_real` on unit creation) must be in place
before replays can stay in sync. Then GATHER, BUILD, QUEUE_UP and GATHER_POINT, which add the economy loop
(frame-spec.md §8, §10).

## Evidence

| Function | Address | Used for |
|---|---|---|
| `CommandPackage::process_group / add_group`, `CommandManager::check_accept_issue` | 94a0c0 / 94bb60, 940a70 | §1.2, §2.1 |
| `Group::add / kill / normalize / find_leader / compute_form / set_up_insert`, `Groups::push_group / get_open_slot / copy_group / clear / process` | 714350 / 714110 / 711540 / 70ccb0 / 707c80 / 70e520, 70f9e0 / 6fa460 / 6fa690 / 713f20 / 6fa210 | §1 |
| `Group::action_halt / action_stance / action_move_to / action_move_near / action_form / action_attack / action_gather / action_garrison / action_build / action_swarm_around / action_queue_up / action_gather_point` | 70d0c0 / 70d440 / 70fba0 / 704990 / 707220 / 712490 / 700b90 / 700490 / 707510 / 70fbe0 / 6fdbb0 / 6ff1b0 | §2 |
| `Unit::add_group_move_order / add_garrison_order / add_cast_order`, `Build::add_attack_order / queue_up / add_gather_point` | 5e4710 / 5e4080 / 5e4a60, 622ce0 / 620f40 / 622e70 | §2 |
| `::get_new_order`, `OrderList::walk_data` | 730550, 730270 | §3.1, §4.1 (saved front to back) |
| `LinkListBase<UnitOrder*,…>::add / remove_current / tail / prev / clear`, `Recycler<RecycledOrderNode>::pop` | 46d5a0 / 46d620 / 46d700 / 46d6e0 / 46f600, 46e810 | §4.1 |
| `UnitData::get_order / order_type / get_action / get_next_action / action_type`, `Unit::update_order / update_action` | 60b030 / 616e80 / 608450 / 608230 / 60a850, 6179d0 / 60a870 | §4 |
| `Unit::add_move_order / add_move_facing_order / add_attack_order / add_build_order / add_gather_order / add_think_order` | 616ed0 / 5e55c0 / 5e5410 / 5e5210 / 61a5c0 / 5e3df0 | §4.2 |
| `Unit::kill_current_order / close_orders / clear_orders / kill_current_action` | 5e2cb0 / 5e37f0 / 5e3860 / 5e2bb0 | §4.3 |
| `Unit::work / do_job / do_idle / check_idle / think / do_think_order` | 60d180 / 617a10 / 60dcd0 / 6032c0 / 5f6e40 / 5e5bf0 | §5, §6 |
| `Unit::do_move / do_attack / do_attack_to / do_explore_to / do_flee_to` | 5f7b30 / 5f1b80 / 5f2320 / 5f24a0 / 5f2480 | §6 |
| `UnitOrder::print_details / log_data` | 72ff80 / 47f960 | flag 0x04 label |
| Order vtables, `UnitOrder` method list | `re/analysis/vt_orders.txt`, `rise.pdb` | §3.3 |
| Enums `OrderIndex`, `QueuePos`, `UnitMask`, `UnitFlags`, `UnitFlags2`, `CombatStanceIndex`, `StanceTypes` | `rise.pdb` | names |

## Open questions

1. `UnitOrder.flags` bits 0x20 and 0x80, and `RecycledOrderNode.metric` (always 0 when added; walked in saves) and
   `OrderList.ordered` were not traced to a reader.
2. `is_pathed` exact condition, `check_target_path`, `repath`, `clear_partial_path` semantics are only outlined.
3. The handlers in §6 (movement, PathFinder, `fight`, `do_gather`, `do_build`, group orders) need their own specs;
   without them a reimplementation can run the order machine but not stay bit-identical.
4. Resolved: `do_attack`'s write into the shared `UnitType` (`max_range`) is exact because the loader sets
   `max_range = 0` for every `k` type (rules-spec §7); engine/sim applies the swap through `raw_max_range()`.
5. Resolved: `Unit::find_attack_pos` is specified in combat-spec 3.8 (draws only for building/wall targets, 3.8.4)
   and ported (engine/sim/combat_orders.cpp).
6. MOVE_TO with formation 8 reads a never-filled static `cols` array in the original (uninitialised data). If real
   replays use form 8, the reimplementation must reproduce whatever the original reads (probably zeros).
7. `get_open_slot` only hands out slots 0..45 of each player's 64; what uses 46..63 (AI armies via other paths?) was
   not traced. Diplomacy value 2 = ally and leader flags 0x700 = transport permissions are inferred from use.

