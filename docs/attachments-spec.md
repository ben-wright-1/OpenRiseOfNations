# Unit attachments (EXISTEVENT): spec

Status: ✅ verified against the binary (PDB symbols) and against real data. 5,128 of 5,128 unit EXISTEVENTs resolve to an AMMO entry, and the
software previews show the weapons landing in the hands (`re/previews/attach_*.png`, tool `re/tools/attach-preview.mjs`).

Weapons and props such as AK47, Spear, ArcherArrow, MerchantBasket and TrebuchetRopes are **separate BH3 models**. They are rigidly
attached to a named node of the unit's skeleton, and they are drawn only during the time windows listed in `unit_graphics.xml`. The game
draws an attachment as a child `GraphicChad` of the unit, through the same code path that draws flying ammunition (`draw_ammo`).

## 1. Data

`Data/unit_graphics.xml`, inside `<UNIT>`:

```
<EXISTEVENT starttime="0" endtime="9999" anim="CHAR_ATTACK2" type="Spear" node="0"/>
```

| attr | parsed as (`GraphicPieces::init_unit_events`) | meaning |
|---|---|---|
| `anim` | index into the 38 unit action names (`UnitAnimNames`, or the alias table `SecondUnitAnimNames`) | the event belongs to this action only. Events are stored per action. |
| `starttime` | `u16` (absent → -1 → 0xFFFF) | ms into the current action's animation |
| `endtime` | `u16` (absent → 0xFFFF) | ms into the action. Designers write 9999/19999/34463 to mean "until the end". |
| `type` | name of an `<AMMO>` in `Data/effects_graphics.xml` | the attachment's model, texture, render state and optional anim |
| `node` | signed char (absent → -1) | attach slot 0..13 (section 3), or -1 for the unit's model origin |

All 5,128 unit EXISTEVENTs carry all five attributes. The `type` values use 112 distinct names. Duplicate events, such as the same type in
two windows of one action, are simply separate events.

## 2. `type` → AMMO entry

- Match `type` against the `name` of every `<AMMO>` element in `effects_graphics.xml`, in **file order**. The comparison is
  **case-insensitive** and needs the exact length (`String::operator==` uses `_wcsicmp`). **The first match wins.** No match falls back to
  ammo piece 0 (`BuildingTraceBullet`), with the error "BILL! You need to define this ammo type". Every type in the shipped data resolves.
- These AMMO attributes matter for an attachment:
  - `model`: a BH3 file
  - `texture`: a TGA file, bound as both `Texture0` and `Texture1`
  - `renderstate`: one of `Ammo Blend`, `Ammo Team Blend`, `Fire Blend` or `Smoke Blend` (section 6)
  - `anim`: optional; an `anim_graphics.xml` name
- `puncture`, `puff`, `explosive`, the nested `<PARTICLEEVENT>`/`<TRAILEVENT>` and the rest apply to flying ammunition only. Events of an
  attached child are **not** run (section 5).
- There is no scale attribute. The attachment inherits the unit's scale (section 4).

Render states used by the AMMO entries that EXISTEVENTs reference:

| render state | AMMO entries |
|---|---|
| `Ammo Blend` | 86 (weapons and tools) |
| `Ammo Team Blend` | 9 (`MerchantBasket`, `KnightShield*`, `EknightShield*`, `Gunflag`) |
| `Fire Blend` | 9 (muzzle flashes, `Firelance Glow`) |
| `Smoke Blend` | 8 (propellers, `CopterBlade`) |

## 3. `node` → BH3 node (attach slots)

At load time, `BH3Method::load_hier` fills a 14-entry `NodePacket` per model while it walks the BH3 hierarchy in **file (DFS pre-)order**.
For each node it tests i = 0..13 in order, and for each i it tests these four patterns in order. The first match decides the slot.

| node name (case-insensitive, exact) | slot |
|---|---|
| `"ammo node " + i` | i |
| `"pivot node " + i` | i + 4 (the node is also marked not-animated; see the note below) |
| `"smoke node " + i` | i + 8 |
| `"node " + i` | i |

Additional rules:

- Slots ≥ 14 are rejected ("Invalid Node Index"), for example "pivot node 10" or above.
- Slot 13 is first set to the **root node**. Then a node named `"Bip01 Spine"` overwrites **both slots 12 and 13**. Pattern matches can
  overwrite them again. No EXISTEVENT uses slot 12 or 13.
- **Later nodes overwrite earlier ones.** If two nodes share a name, the last one in DFS order owns the slot. For example, `explorer.BH3`
  has two `"ammo node 1"` nodes, and the second one, a child of the root, wins. That is where the explorer's staff lies on the ground
  during CHAR_IDLE3. This is verified visually in `re/previews/attach_explorer_idle3.png`.
- Names must match exactly apart from case. `" ammo node 0"`, `"ammo node0"` and `"Smoke Node 3\t"` match nothing.
- `node=-1`: the attachment uses no node, so its transform is the unit's root transform (the unit's model space). Used by 163 events,
  for example `SpecialforcesBomb`, `logchop`, `Workbench3 Saw` and `Fishingfleet5Wall`.
- `node` ≥ 14, or a slot that is still empty: **the attachment is not drawn** (`draw_ammo` returns early). `ron-check` counts 9 such
  events in the shipped data, so the original draws nothing there either:
  - `lancaster.bh3`, slot 11 (4 events): the node name is `"Smoke Node 3\t"`, with a trailing tab
  - `fighterbomber7.bh3`, slot 3 (4 events): the model has no "ammo node 3"
  - `Mortar.bh3`, slot 2 (1 event): the model has no "ammo node 2"

  (An earlier scan also blamed 50 events on the caravan crew model `artillery_crew_driver.bh3`; no unit using that model has an
  EXISTEVENT, so that count did not reproduce.)

Node values in the data: 0 (2,453 events), 1 (1,591), 2 (780), 3 (91), -1 (163), and 8–11 (50; these are "smoke node 0–3", for example
`JagGrass` at node 9 = "smoke node 1").

Real files, with the slot each EXISTEVENT lands on:

| unit (model) | relevant nodes, DFS order → slot | EXISTEVENTs |
|---|---|---|
| IMMORTAL-DEFAULT-AGE0 (`immortal.bh3`) | `Bip01 R Hand` › `"ammo node 0"` → 0 | `Spear` node 0 → ammo node 0 (right hand). Visible all of CHAR_WALK/IDLE*/DEFAULT, **hidden** at ATTACK2 900–1664 and ATTACK3 365–1198 (the spear is thrown) |
| ASSAULTINFANTRY-ARAB-AGE0 (`assault_infantry_arab.bh3`) | `Bip01 R Hand` › `"ammo node 1"` → 1 › `"smoke node 0"` → 8 › `"ammo node 0"` → 0. Under the root: `"ammo node 2"` → 2, `"ammo node 3"` → 3 | `AK47` node 1 (in hand) for most actions. `AK47` node 2 (root-level node: slung or dropped) for IDLE3 3297–11988 and the DEATH_STAB1/SHOT1/SPLODED1 actions |
| ARCHERS-DEFAULT-AGE0 (`archer.bh3`) | `Bip01 R Hand` › `"ammo node 0"` → 0 | `ArcherArrow` node 0 during DEFAULT/IDLE1-3, and ATTACK1 0–766 + 1733–end (hidden while the arrow flies). No event for WALK/JOG/death actions, so no arrow there |

`ammo node 0` under the hand is also the RELEASEEVENT muzzle/launch point. Because slots are shared, PARTICLEEVENT/RELEASEEVENT `node=`
indices use the same table.

Note, outside the attachment path: the game skips animation keys for "pivot node" nodes (`isDeadNode`). For slots 4–6, `draw_unit` sets
the node's local transform to `Rz(pivot_angles[slot-4])` (turret aim) every frame. In a viewer, use identity (angle 0) for pivot nodes
rather than BHA keys. This matters for attachments that sit under a pivot node.

## 4. Transform

The attachment's model matrix is **exactly the unit's model matrix times the animated world transform of the slot node**:

```
M_attach = M_unit × W_unit[slot]        (slot = node; for node = -1, W = identity)
vertex_world = M_attach × W_attach[n] × v      (v owned by attachment node n)
```

- `W_unit[slot]` is the node's `world_transform` after the unit has been posed for this frame, so it already includes the rest and
  animation transforms. It is the same matrix `pose_model` applies to vertices owned by that node.
- `M_unit` is the unit root placement, built from `misc_rot` (terrain tilt), `Rz(angle)`, the translation, and the scale
  `(sx, sy, sz) = unit scale × guy_scale × (1, -1, 1)`. Note the Y mirror. Use whatever matrix the viewer already uses for the body.
- There is **no extra offset, axis, pivot or scale.** The game does multiply by `Rx(child.angle)`, but EXISTEVENT children always have
  `angle = 0`, so that factor is the identity. The child's own `loc` (0) and `angle` (0) are ignored in favour of the parent's. The
  `Transform` compose is exact here, because node world transforms carry no scale.
- **The attachment BH3 is hierarchical** and has its own rest transforms. For example, `spear.bh3` root `Dummy01` has a −90° Z rest
  rotation, and `InfantryMuzzleFlash.BH3` has 2 mesh nodes and 16 dummies. Pose it with the normal `pose_model` (world = parent × rest ×
  anim), and then apply `M_attach`.
- **Attachment animation:** if the AMMO has a non-empty `anim=`, that BHA is action 0 of the attachment. 14 AMMO entries have one,
  and 9 of those are EXISTEVENT types: `Scoutmap`, `InfantryMuzzleFlash`, `BiplaneMuzzleFlash`, `FighterMuzzleFlash`, `Spy3crossbow`,
  `Spy6Breifcase`, `shrap`, `WhaleChurn3` and `FishChurn1`. An empty `anim=""` (`DutchMerchant3Musket`) counts as none. It is sampled at **the parent's current action time** (the same ms value used for the unit), wrapped
  modulo the attachment clip's own length. Without `anim=`, every node's local transform is the identity (the rest pose).
  Evidence that sampling uses the parent's time: `Scout0 Scroll Idle3` is 12,732 ms long, the same length as the explorer's CHAR_IDLE3.
  It is authored to run in step with the parent.

## 5. Visibility

For each frame, for the unit's current action `a` at time `t` (ms into `a`):

```
draw attachment e  iff  e.anim == a  &&  e.start <= t  &&  e.end >= t
```

- The game's exact test is `last_time <= end && start <= cur_time`, where `last_time` is the start of the current 15 Hz sim tick in
  anim-ms (`frame * 200 / 3`), and `cur_time = last_time + graphic_dt`, clamped to the clip length (`GuyOut::draw`). This is the same as
  the overlap of `[last_time, cur_time]` with `[start, end]`. With a single `t`, use `start <= t <= end`.
- **No EXISTEVENT for the current action means no attachment.** There is no default and no carry-over from the previous action. For
  example, the archer has no arrow while walking, and the AK47 swaps between node 1 and node 2 at IDLE3 3297 ms.
- Several events can be active at once (different types or nodes), and each is drawn.
- Gate: a graphics-detail flag must be on (byte `graphsys->scalability.section_render + 0xF`). Treat it as always on.
- An attached child (it has a parent) never runs its own graphic events, so the AMMO's nested PARTICLE/TRAIL events do not fire
  (`GraphicPieces::draw`).

## 6. Texture, color, render state

The game renders an attachment with `TransformBatchMethod`: CPU-transformed positions, UVs and a **per-vertex diffuse color**, and **no
normals, so it is unlit**. The texture is the AMMO `texture=`; the unit's texture is not used. The diffuse color is:

- **RGB:**
  - `Ammo Team Blend`: the owner's unit team color (`TeamColor::get_unit`, the same color the body uses)
  - otherwise: WHITE, or GREY when the owner is not visible/explored (fog)
- **Alpha:** `min(child.transparent (255), parent.transparent)`, which is 255 for a normal unit
- `tfactor`: (255, 255, 255, the same alpha)

Render states (`Scene::init_render_states`; stage-0 args are the engine's enum, 5 = texture, 0/1 = diffuse):

| state | color | alpha | blend / test |
|---|---|---|---|
| `Ammo Blend` (#31) | tex × diffuse | tex.a × diffuse.a | SRCALPHA / INVSRCALPHA, alpha test `> 0` |
| `Ammo Team Blend` (#33, ps `ps_luminance_CBTtcA4tf`) | lerp(diffuse=team, tex, tex.a), i.e. tex.rgb·tex.a + team·(1−tex.a) | 4 × tex.a × tfactor.a (saturate) | SRCALPHA / INVSRCALPHA, no alpha test, clamp + linear |
| `Fire Blend` (#13) | tex × diffuse | tex.a × diffuse.a | **additive** SRCALPHA / ONE, test `> 0`, enables bit 1 cleared (probably z-write off) |
| `Smoke Blend` (#11) | 2 × tex × diffuse | tex.a × diffuse.a | SRCALPHA / INVSRCALPHA, test `> 0`, enables bit 1 cleared, clamp + linear |

`Ammo Team Blend` uses the same alpha-as-team-mask convention as the unit body: texels with alpha ≈ 64 show the team color.

For the viewer, an acceptable simplification is: texture × white, discard alpha 0 (Ammo Blend), team lerp for Ammo Team Blend, and
additive for Fire Blend. Lighting the attachment like the body is a deviation from the original, which leaves attachments unlit.

## 7. Implementation recipe (viewer)

1. **Load:**
   - Parse every `<AMMO>` in `effects_graphics.xml` into an ordered list (name, model, texture, renderstate, anim). Build a
     lowercase-name → first-entry map.
   - Parse each `<UNIT>`'s `<EXISTEVENT>`s into `{action, start, end, type, node}`. These go in `UnitGraphic`.
   - For each unit BH3, compute `slot[14]` → node with the rules in section 3.
2. **Per frame** (action `a`, time `t`):
   - Pose the unit and keep each node's world transform.
   - For each event with `action == a && start <= t <= end`:
     - Resolve the AMMO entry.
     - Get `W`: identity if `node == -1`; skip the event if `node > 13` or `slot[node]` is empty; otherwise use the slot node's world
       transform.
     - Pose the attachment BH3 at time `t` with its AMMO anim (or the rest pose).
     - Draw it with `M_unit × W` and the AMMO texture and render state.
3. **Cache** attachment models, animations and textures by path. The 112 types use 100 distinct models and 42 textures, and many units
   share them.

Reference implementation in JS: `re/tools/attach-preview.mjs`. Example output:
`node re/tools/attach-preview.mjs IMMORTAL-DEFAULT-AGE0 CHAR_ATTACK2 4 out.png`.

## 8. Building EXISTEVENTs (for contrast; not needed for units)

`building_graphics.xml` uses
`<EXISTEVENT buildanim="BUILD_DEFAULT" subject="village_0ma_addon" anim="village_0ma Default" axis="0"/>`
(`GraphicPieces::init_build_events`).

- `subject` → AMMO, using the same name lookup. `anim` → the add-on's own action. `axis` and `node` (default 7) are optional.
- These are drawn as **independent top-level add-ons** (`find_empty_addon`, a persistent pool keyed by subject, player and object) at the
  building's location, using the building's scale × `build_scale`. They are not children of a node.
- When `axis` ≠ 0, the add-on is offset by the building node's translation, and its angle becomes `45 − wind_angle + 180` (wind-facing
  flags).
- The add-on's anim time is the building's `cur_time` modulo its own clip length.

## Evidence (riseofnations.exe, PDB names)

- **`GraphicPieces::init_unit_events` @008eabb0**, first loop. Element `existevent` (internal string #3901). It creates a `GraphicEvent`
  with `event_type = 0` and stores:
  - +0x0C `start_time` ← `starttime`
  - +0x0E `end_time` ← `endtime`
  - +0x08 `animation` ← `anim` (match against the 38 entries of `UnitAnimNames` / `SecondUnitAnimNames`)
  - +0x14 subject = `first_ammo_piece + i`, where `ammo_names[i] == type` (first match; error and piece 0 on a miss)
  - +0x23 `node`, signed byte, default -1

  `EventPacket::add` @00918dd0 files the event under `events[animation]` (38 lists). Other loops in the same function:
  `existplane` (type 4), `particleevent` (type 3, with `axis` at +0x22), `underlayevent` (type 8), `explosionevent` (type 9).
- **`BH3Method::load_hier` @00915010**:
  - Builds names from internal strings #220 `"ammo node "`, #221 `"pivot node "` (+4, `isDeadNode = 1`), #222 `"smoke node "` (+8)
    and #223 `"node "`, plus the integer i, and calls `NodePacket::set_node` @009182c0 (rejects index ≥ 14).
  - If `nodes[13]` is null, sets it to the current node (so the root). #224 `"Bip01 Spine"` → `nodes[12]` and `nodes[13]`.
  - The `NodePacket` is `RData::packets[0x11]`. Its layout is a vtable, a field, then `HierObj* nodes[14]` at +8.
- **`String::operator==(String const&)` @00a1f140**: compares lengths, then the case-insensitive hash or `_wcsicmp`.
- **`GraphicPieces::draw` @0090b2d0**: dispatches gpiece types 6/8/0xE to `draw_ammo`. It runs `execute_graphic_events` only for
  pieces without a parent.
- **`GraphicPieces::execute_graphic_events` @00909a10**, unit branch, `case 0`:
  - Condition: `scalability flag && last_time <= end_time && start_time <= cur_time`, over the event list of `cur_anim`.
  - Builds a child `GraphicChad`: gpiece = subject; who and o = the parent's; `pass` = node (signed); `loc` = 0; `angle` = 0;
    `cur_anim` = 0; `cur_time` = the parent's `cur_time`; `misc_rot` = the parent's.
  - Calls `parent->add_child(child)` and then `draw(child)`.
- **`GraphicPieces::draw_ammo` @008e68b0**:
  - Color: WHITE, GREY when unseen, or `TeamColor::get_unit` when `render_state == render_states[33]` (offset 0x84). Alpha =
    `min(transparent, parent->transparent)`. Writes it to the sprite color and to `render_state->tfactor`.
  - With a parent: builds `T = init(misc_rot) · rotate_z(parent.angle) · translate(parent.loc) · scale(parent.sx, sy, sz)`, the same
    sequence `draw_unit` uses at 008e8878–008e8a3c. If `pass != -1`, it looks up `parentRData->packets[0x11]->nodes[pass]`; when that
    is null or `pass > 13` it returns without drawing. Otherwise `T = T * node->world_transform (HierObj+0x64)`, and then
    `T = T * Rx(child.angle)`.
  - Writes T to the TransformPacket. Sets the AnimationPacket to (`cur_anim`, `cur_time`), then renders.
- **`HierObj::update_transforms` @00536c60**: `world = parent.world * parent2local * local`. Struct `HierObj`: +0x04
  `parent2local_transform`, +0x34 `local_transform`, +0x64 `world_transform`.
- **`HierObj::set_to_time` @00536e30**: time wrapped modulo `anim_time`. Nodes with `isDeadNode` skip their own keys. A null anim resets
  locals to identity (`Transform::ident`, folded with `~Transform` @00420040).
- **`Transform<float>::operator*` @004217b0**: the composition rotates B's translation by A's quaternion, scales it by A's scale and adds
  A's translation; quaternions multiply and scales multiply. This is exact when B has no scale.
- **`GraphicPieces::init_ammo_data` @008f1490**: reads the AMMO `texture` (→ params `Texture0` and `Texture1`), `model` (→ `BH3 Name`)
  and `renderstate` (looked up by name in `render_states`). `anim` goes through `decipher_animation`, then
  `AnimationPacket::init_animation(0, id)`, and action 1 = none. The RMap is `rmaps[7]`.
- **`GraphicPieces::init_ammo_map` @008f86c0** + **`TransformBatchMethod::execute` @0091f2c0** and **`update_vert_list` @0091a880**:
  - Ammo goes through a CPU batch. Each vertex = `trans + s·(R·S'·(node.world·v))`.
  - The per-vertex color is `sprite_color`. No normals are produced.
- **`Scene::init_render_states` @008ba450**: render state indices 31 `Ammo Blend`, 32 `Ammo Low Detail Blend`, 33 `Ammo Team Blend`
  (`ps_luminance_CBTtcA4tf`), 13 `Fire Blend` (additive), 11 `Smoke Blend` (`MODULATE2X`). `RenderState::init` @00535ba0 sets the
  defaults: arg1 = 5, arg2 = 0, `alpha_ref` = 0.
- **`GuyOut::draw` @005dc490**: `last_time = anim_frame * 200 / 3`, `cur_time = last_time + graphic_dt`, both clamped to the clip
  length. Scale = unit `scale` × `guy_scale` on top of the `GraphicChad` default `(1, -1, 1)`.
- **`GraphicPieces::draw_unit` @008e7da0** (disassembly only):
  - 008e8af3–008e8bcf: for slots 4–6, sets `local_transform` = identity then `rotate_z(pivot_angles[i-4])`, and queues the node for a
    transform update.
  - 008e8bd8: sets the unit's AnimationPacket to (`cur_anim`, `cur_time`).

Data validation: a scratch Node scan of every `<UNIT>`, its BH3, `effects_graphics.xml` and the BHA clip lengths gave these results:
type resolution 5,128/5,128, node −1 = 163, unresolved slot = 9 (`RON_CHECK_VERBOSE=1 ron-check` lists them), and 239 events whose `anim` the unit does not
define (inert).

## Uncertain / not verified

- **Engine texture-arg enum** (5 = texture, 0/1 = diffuse, 6 = tfactor): inferred from the default args and the pixel shader names
  (`CBTtcA4tf`), not from the d3dgl backend. The meaning of the `luminance` shader prefix is unknown.
- **Missing actions:** when a unit lacks an action, the 239 events for that action are assumed inert. If the game substitutes another
  action's clip but keeps the action index (for example CHAR_GROUP_IDLE1), those events could fire. This was not traced.
- **Exact gate flag:** the identity of the scalability byte (`section_render + 0xF`) was not traced to a user option.
  Probably section 15 = AMMO in the `<SECTION>` order of `Data/config_defaults.xml` (farms-spec.md uses 12 = BUILDINGS).
- **Rest-transform quantization:** `load_hier` rounds rest quaternions to 1e-3 and translations to 1e-2, and drops identity rests. This
  is a negligible visual difference, but bit-exact work should replicate it.
