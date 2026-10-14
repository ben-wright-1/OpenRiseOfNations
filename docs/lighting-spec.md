# Lighting and post-processing (what the EE frame really computes)

Status: ✅ constants, pass order and formats read from `d3dgl.dll` and `riseofnations.exe` · 🟡 a few behaviours inferred
from code paths that still need a screenshot comparison (see open questions).

Companion docs: [renderstates.md](renderstates.md) (render-state bits → shader macros), [terrain-spec.md](terrain-spec.md)
(terrain mesh, baked vertex light, camera). Shader sources referred to below are the user's own
`shaders\*.hlsl` files; this document only paraphrases their behaviour.

---

## 1. The frame at a glance

All game drawing between `init_hdr` and `end_hdr` goes into an **HDR target**. The backend then tone-maps it into the
back buffer, adds bloom, runs FXAA, and only then are overlays, text and the UI drawn (LDR, directly into the back buffer).

| # | Pass (who) | Target (format, size) | Inputs | Notes |
|---|---|---|---|---|
| 0 | `begin_scene` | – | – | Updates `gCurrentTime`, resets tfactor and depth bias |
| 1 | `clear_buffer(0)` (game) | back buffer RGBA8_UNORM W×H + depth | – | colour (0,0,0,0), depth 1.0 |
| 2 | `begin_drawing` | HDR target | – | HDR target cleared to (0,0,0,0) |
| 3 | `init_hdr` (Scene::render) | **HDR: RGBA16F, W×H, 1 mip**, shares the back buffer's depth (D32 via R32_TYPELESS) | – | Pushed as render target, viewport = full screen |
| 4 | Scene geometry (`Scene::render`) | HDR | – | Terrain → … → objects → particles → water → surf → explosions → nuke (order in terrain-spec B6) |
| 5 | `end_hdr` | back to back buffer | – | Pops the render-target stack |
| 6 | **Luminance** (`do_luminance`) | R16_FLOAT **512×512, full mip chain** | HDR | Viewport 512×512; then `GenerateMips` |
| 7 | **Tone map** (`do_tonemap`) | MRT: RT0 = "Toned" RGBA8_UNORM W×H; RT1 = "Pong" RGB10A2_UNORM W×H | HDR, luminance (mip 9 = 1×1) | b4 = ToneMapData |
| 8a | Bloom on: **blur H** (`do_blur`) | "Ping" RGB10A2_UNORM (W/2)×(H/2) | Pong | Viewport (W/2)×(H/2), BlurData dir (1,0) |
| 8b | Bloom on: **blur V** | Pong, **top-left (W/2)×(H/2) only** | Ping | Same viewport, dir (0,1) |
| 8c | Bloom on: **screen blend** (`do_blend`) | back buffer | Pong (sampled at uv·0.5), Toned | Full viewport |
| 8' | Bloom off | back buffer | Toned | Whole-texture copy (`CopyResource`) |
| 9 | **FXAA** (`D3D11AntiAlias::run`, if enabled) | back buffer | copy of back buffer (RGBA8 W×H) | Copy first, then FXAA into the back buffer |
| 10 | Post-tonemap scene extras (Scene::render) | back buffer | – | Drag box, `ObjectsOut::draw_overlays` (selection, health bars), `flush_ammo`, shock waves, mouse-over text, game stats |
| 11 | UI (`IFaceRenderManager::flush_all`), screen arrows | back buffer | – | UI render states have no lighting/TL bits → plain LDR |
| 12 | `end_drawing`, `end_scene` | – | – | Cursor; **reflection copies** (§8.6); tonemapper nuke overrides reset |
| 13 | `flip` | – | – | Present |

W×H = screen size (`GraphSys::screen_width/height`); W/2 is C integer division. All post passes use the same
full-screen quad (4-vertex strip, uv (0,0) at the top-left). Post-pass state: blend off, depth test off, cull none,
solid fill. The post passes bind **no sampler**: slot s0 keeps whatever the last scene draw bound (16× anisotropic,
wrap/clamp from that render state; the textures have one mip except the luminance chain). A port should use a linear
clamp sampler.

The same `init_hdr … end_hdr → tonemap → AA` sequence is used by the Conquest map (`ConquestWin::on_redraw`).

---

## 2. Scene constants (cbuffer `PerScene`, b0, bound to VS and PS once at device creation)

| Field | Value | Source |
|---|---|---|
| `gView`, `gProjection` | camera matrices (D3D row-vector convention; the backend transposes for the `column_major` declaration) | `set_view_transform` / `set_projection`; camera in terrain-spec B7 |
| `gProjectionInverse` | inverse projection | only used by a helper the pixel shader never calls → unused |
| `gAmbientLight` | **(0.6, 0.716, 0.745, 0.558)** in game | `Game::init_light`: `unit_graphics.xml` `<UNITS ambientr ambientg ambientb ambienti>`, passed unchanged to `set_ambient(r,g,b,a)` |
| `gScreenSize` | current viewport width/height in pixels | written by every `set_viewport` (so 512×512 or W/2×H/2 inside the post passes) and uploaded at the next draw. The post passes draw without re-uploading, so **they see the last scene viewport (W×H)** |
| `gCurrentTime` | seconds since the first frame (`(ms − first ms)/1000`) | `begin_scene` → `update_frame_time`; only used by animated water normals |

Other values of `gAmbientLight`: (0, 0, 0, 1) after `System::init_graphics` (black `Color`), the menu logo light in
`TopMenu::init_light` / `draw_logo` (1,1,1,1), and the `ambient` console command (`GraphicPieces::parse_cmd`). The
tileset does **not** change it. Only `.rgb` is used by the shader; `.w` (0.558) is ignored.

---

## 3. The light (cbuffer `LightData`, b3)

There is one light, **"Sun"**, created by `Game::init_light`: directional, direction (travel vector, world space)
**(−1, 0, −1)/√2**, i.e. the light comes from +x and from above at 45° (world +z is up). The direction is passed through
the light's scene-graph rotation (`D3D11GLLight::update`; the Sun hangs directly under the scene root, expected identity).

The backend stores `Ambient = rgb·a`, `Diffuse = rgb·a`, `Specular = rgb·a` (the 4th component is an intensity). The pixel
shader reads only **`gLight.Direction.xyz` and `gLight.Diffuse.rgb`**: `gLight.Ambient` and `gLight.Specular` are never
used (specular uses the diffuse colour), nor are position/range/attenuation.

The game rewrites the Sun's colours during the frame, so the value depends on what is being drawn:

| Phase | Set by | Light ambient (unused) | **Light diffuse** |
|---|---|---|---|
| Terrain (every frame, start of `TerrainOut::render`) | `TerrainOut::prep_terrain_lighting` | tileset AMBIENT rgb·alpha | **tileset DIFFUSE rgb × alpha** (Dirty: 1.36·(1,1,1); Snowy: 1.2·(0.94,0.96,1.0)) |
| Objects (forced at the start of `ObjectsOut::pop_all_zoomed_in`) and everything drawn after it (water, particles, …) | `GraphicPieces::prep_unit_lighting(force=1, percent=1)` | (1,1,1)·1 | **(1, 1, 1)** |

The light is "on" (`mEnableLight`), so render states with bit 0x200 get `ENABLE_LIGHTING`.

**Direction convention in the shader:** the light vector is `L = −normalize(Direction)` in world space, then rotated into
view space with `gView` (w = 0, not renormalised; the camera view matrix is a rotation + translation, so L stays unit).
Normals are also in view space, so N·L equals the world-space N·L. Specular uses `V = −normalize(viewPos)`, i.e. a
perspective-style eye vector even though the projection is orthographic; reproduce that exactly.

### Tileset LIGHTING (`Data/tilesets.xml`)

`<LIGHTING tint_red tint_green tint_blue>` with `<AMBIENT>` and `<DIFFUSE>`, each holding four `<COLOR>` elements read
**by position** (the `name` attribute is ignored): [0] = intensity ("alpha"), [1..3] = r, g, b
(`TileSet::init` → `ambient_lighting[4]`, `diffuse_lighting[4]`; `GraphicPieces::init_lighting` copies them into
`land_material`). With no tileset loaded, everything is 0. The `tint_*` triple is a load-time texture tint for road and
river textures (`ImageIO::set_tint`), not lighting.

| Tilesets | AMBIENT a; rgb | DIFFUSE a; rgb |
|---|---|---|
| Dirty, Dirty_2, autumn, early/late_autumn, arctic, m_steppe* | 2.0; 0.01, 0.03, 0.18 | 1.36; 1, 1, 1 |
| Snowy | 1.0; 0.43, 0.37, 0.23 | 1.2; 0.94, 0.96, 1.0 |
| SW_Desert, tropical, tropical_wet | 1.0; 0.07916, 0.06, 0.1458333 | 1.2; 1.0, 0.94583, 0.7708 |
| GS_Desert | 1.0; 0.39 ×3 | 1.2; 1.0, 0.94583, 0.7708 |
| jungle | 1.0; 0.39 ×3 | 1.2; 0.6, 0.6, 0.39 |
| african_savannah | 1.0; 0.39 ×3 | 1.2; 1.0, 0.94, 0.85 |

(Read from the user's install at load time; listed here only to make the numbers below concrete.)

---

## 4. Materials (cbuffer `PerMaterial`, b2)

`D3D11Context::set_material` stores **`gAmbient/gDiffuse/gSpecular/gEmissive = rgb × a, w = 1`**, plus `gPower`,
`gTLAlpha` and the UV deltas. `Material::close` (the default) is all zero except `tl_alpha = 1`. Setting a material
flushes pending primitives first, so the values below are exactly what each draw sees.

| Draw | Material (rgb; a) | Resulting constants | Source |
|---|---|---|---|
| **Units** (Unit Blend, lit Death Blend) | ambient (0.6, 0.716, 0.745; 0.558), diffuse (1,1,1; 2.5), specular 0, emissive 0, power 0, tl_alpha 1 — **rgb × percent** | gAmbient = (0.3348, 0.3995, 0.4157)·p, gDiffuse = 2.5·p, gSpecular = 0 | `unit_graphics.xml` `<UNITS diffuser … diffusei>` via `GraphicPieces::init_lighting`; `prep_unit_lighting` |
| **Terrain** (Base Terrain, OceanFloor, Shallows, Water Skirt …) | ambient (1,1,1;1), diffuse (1,1,1;1), rest 0 | gAmbient = gDiffuse = (1,1,1), gSpecular = 0, gPower = 0 | `prep_terrain_lighting` |
| **Water** (Normal Water / Min Water techniques) | ambient (0.2,0.2,0.2;1), diffuse (1,1,1.5;1), specular (40,40,40;1), emissive 0, **power 0.3** | gSpecular = 40, gPower = 0.3 (Torrance roughness) | `TerrainOut::one_time_init` (terrain +0x6698), bound in `render_water` |
| Scene default | ambient (1,1,1;1), diffuse (1,1,1;1) | – | `Scene::init` |
| Nuke mushroom | `tl_alpha` fades: top `1 − 4(t−0.75)`, stalk `0.75·(1 − 4(t−0.75))`, fire `0.9 − 4(t−0.65)` | gTLAlpha | `NukeOut::draw_shroom_*` |

**Unit dimming ("percent" p)** in `GraphicPieces::draw_unit`: p = 1 normally; 0.5 (GREY) and 0.25 (DARK_GREY) for units
that are not currently visible to the local player (two fog/visibility tests); `(128 + c)/255` when the unit's flash
byte c ≠ 0 (c/2 on cards flagged by a scalability byte). Quirk: `draw_unit` first binds the raw `unit_material` (p = 1),
and `prep_unit_lighting(force = 0, p)` returns early when p equals the previous call's p. So a second consecutive unit
with the same p < 1 appears to render undimmed (open question 3).

`gTLAlpha` is 1 for everything except the nuke mushroom. `gEmissive` is uploaded but **no shader uses it**.

---

## 5. Pixel shader lighting (ps_global), paraphrased

Let `c` = saturated result of the texture-stage combiner (renderstates.md §3), `h` = 5 (HDR) or 1 (`NO_HDR`).

**Lit path** (`ENABLE_LIGHTING`, render-state bit 0x200, takes precedence over TL_FAKE_LIGHT):

- N = normalize(interpolated view-space normal); with a normal map, N = tangent-space map normal rotated by
  (T, B, N) (map RG → xy, z = √(1−x²−y²); animated water sums three scrolled samples, see §6).
- L as in §3, V = −normalize(viewPos), H = normalize(L+V), NdotL = N·L (signed), NdotH = |N·H|.
- ambient = max(0, h · gAmbient · gAmbientLight)
- diffuse = max(0, h · (1/π) · gDiffuse · gLight.Diffuse · NdotL)
- specular colour = gSpecular, or the material-map blue channel (`USE_SPEC_MAP`), or the stage-0 texture (`DIFFUSE_AS_SPEC`).
- Default specular (normalised Blinn-Phong): n = max(gPower, 4), or 512·map alpha with a gloss map;
  norm = (n+2)(n+4) / (8π(2^(−n/2) + n)); specular = h · specColour · norm · gLight.Diffuse · NdotH^n · NdotL.
  **Not clamped**: on faces turned away from the light it is negative (only the final `max(0, colour)` clamps).
- Torrance–Sparrow (`SPEC_MODEL_TORRANCE`, water): roughness m = max(gPower, 0.01) or 1 − gloss; Schlick Fresnel with
  r0 = 0.0209 on |V·H|; geometry term min(1, 2·NdotH·|N·V|/|V·H|, 2·NdotH·NdotL/|V·H|); Beckmann distribution
  exp(−tan²α/m²)/(π m² cos⁴α) with α = acos(NdotH); specular = h · specColour · gLight.Diffuse · D·F·G / (4·|N·V|·NdotL),
  also unclamped (and unbounded as NdotL → 0).
- Screen-space reflection (water with "Water Reflections" on): 10 steps of 10 view-space units along −reflect(V, N); at
  each step project, compare with the reflection depth texture (±0.00075); on the first hit add
  saturate(N·V) × reflection-colour texture. Both textures hold the **previous frame** (§8.6).
- **HDR:** rgb = c.rgb^1.45 · (diffuse + ambient) + specular + reflection. **NO_HDR:** rgb = c.rgb · (diffuse + ambient)
  (no pow, no specular, no reflection). Alpha is untouched.

**TL fake light** (bit 0x20000, not lit): rgb = c.rgb^1.9 · h; alpha = c.a · gTLAlpha.

**Neither:** colour = c, unchanged (LDR value written into the HDR target; see open question 2).

Then: colour = max(0, colour); alpha test (`clip`); with `NO_HDR` the result is also saturated. `NUKE_FLASH` doubles h.

**`NO_HDR` is defined only while `GSGraphicCard::minimap_mode` is set**, which `WorldMapBackground::render` does around
its minimap render-to-texture. The main view always uses h = 5.

### Worked coefficients (the main view)

| Draw | Ambient term (per channel) | Diffuse term | Other |
|---|---|---|---|
| Unit, p = 1 | 5·gAmbient·gAmbientLight = **(1.0044, 1.4303, 1.5485)** | **3.9789 · NdotL** (5/π·2.5) | no specular |
| Terrain, Dirty | 5·(0.6, 0.716, 0.745) = **(3.0, 3.58, 3.725)** | **2.1645 · NdotL** (5/π·1.36) | spec/gloss maps when Terrain Maps is on |
| Terrain, Snowy | same | 1.9099 · (0.94, 0.96, 1.0) · NdotL | |
| Water | 5·0.2·gAmbientLight = (0.6, 0.716, 0.745) | 5/π·(1,1,1.5)·NdotL | Torrance, gSpecular 40, m 0.3 |
| TL fake (buildings, trees, smoke, …) | – | – | 5·c^1.9 |

---

## 6. Vertex shader behaviours (vs_global)

- 3D path: position → world (`gWorld`) → view (`gView`); `viewPos` = that view-space position (w = 1); then
  **view z −= 8·gDepthBias**; then `gProjection`. gDepthBias = the render state's integer `zbias` (0, 4 or 10 in the
  named table), reset to 0 by `begin_scene`.
- Normal (and tangent) = (n, 0)·gWorldInverseTranspose, then ·gView; **not normalised** in the VS (the PS normalises N;
  T and the bitangent cross(T, N) are used as they arrive). gWorldInverseTranspose comes from `XMMatrixInverse` of the
  world matrix (`D3D11Context::set_world_transform`); a port can use the usual inverse-transpose.
- Screen-space path (`TRANSFORM_SCREEN_SPACE`, pre-transformed vertices): x, y get **+0.5 pixel**, then
  NDC = (2x/W − 1, 1 − 2y/H) with W, H = `gScreenSize`; z = input z − gDepthBias; **w = 1** (the vertex rhw is ignored,
  so attributes interpolate linearly in screen space). `viewPos` = NDC of the unshifted point with z = 0.
- No fog anywhere (`ENABLE_FOG` is emitted but no shader reads it). Vertex colours pass through unchanged.
- Point sprites (nuke particles) and the nuke flash have their own paths; not lighting-relevant.

---

## 7. Terrain: baked vertex light and shader light are both applied

`TerrainOut::calculate_norms` → `light_land_vert` → `light_vert` bakes, per height vertex, using the Sun as left by
`prep_terrain_lighting` (which always runs first: `Terrain::init`, `terraform_*`):

`baked.rgb = min(1, ambientRGB + diffuseRGB · diffuseA · clamp(N · (−dir), 0, 1))`, stored as bytes (red in the low byte);
note the **ambient intensity is not applied** here. Underwater vertices (z < 0) also get an alpha fade (terrain-spec B3).

`CoordInfo::add_geometry` copies these baked colours into the terrain vertex stream (unless the scenario editor
overrides the colour), and Base Terrain's stage 0 is `MODULATE(TEXTURE, DIFFUSE)` with lighting (0x200) on. So the final
terrain colour is:

`(tex · baked)^1.45 · (ambient term + diffuse term) [+ spec from the _mtrl map]`, ×5 HDR, then tone mapping.

This answers terrain-spec open question 2 ("double lighting") from the code side: yes, both. Base Terrain additionally gets
0x1C000 (normal + spec + gloss maps) per draw when the card supports extended materials (the D3D11 card always does) and
**section 24 "Terrain Maps" = 2**.

---

## 8. HDR post-processing

### 8.1 Tone-mapper parameters (`GSToneMapper`, defaults from `D3D11ToneMapper`'s constructor)

| Field | Default | → ToneMapData | Notes |
|---|---|---|---|
| `mExposure` | 1.0 | gExposure = mExposure + mExposureAdjustment | |
| `mExposureAdjustment` | 0.0 | ↑ | Nuke flash only; reset to 0 each `end_scene` |
| `mWhitePoint` | **3.5** | gWhitePoint | editor range 0–20 |
| `mThreshold` | **0.6** | gThreshold (bloom threshold) | 0–2 |
| `mMinLuminance` | **1.4** | gMinLuminance | 0–1.5 |
| `mSaturation` | 1.0 | gSaturation | Nuke only; reset to 1 each `end_scene` |
| `mBloomStrength` | **6.0** | gStrength | 0–10 |
| `mBloomTightness` | **12.5** | gTightness | 0–20 |
| `mTint` (Color) | 0 (all bytes) | gTint = bytes/255 (r,g,b,a) → no tint (weight a = 0) | Nuke only |

The editor ranges come from `Scene::registration` (debug parameter window "Tone Mapper Exposure", …). Nothing else in
the game writes these fields except `Nuke::draw` (§8.5). Values are pushed to the cbuffer (`update_params`) when
`mDirty` is set, just before the tonemapper runs; `create_param_buffer` uploads the defaults at start-up.

### 8.2 Luminance

Full-screen pass into the 512×512 R16F target (fixed size, independent of aspect): each pixel samples the HDR target at
its uv (bilinear) and writes max(0, dot(rgb, (0.3, 0.59, 0.11))). `GenerateMips` then box-filters down to 1×1, so the
**"average luminance" is the arithmetic mean of a 512×512 resample of the frame**: no log average, no temporal
adaptation, recomputed every frame.

### 8.3 Tone map (two outputs)

Per pixel, with `F(x) = ((0.15x + 0.05)x + 0.004) / ((0.15x + 0.5)x + 0.06) − 0.02/0.3` (the Hable/Uncharted-2 curve
with A..F = 0.15, 0.50, 0.10, 0.20, 0.02, 0.30):

1. avg = max(luminance mip 9, gMinLuminance). (The shader asks for level 10; the chain ends at 9.)
2. col = F(gExposure · hdr.rgb) / F(avg · gWhitePoint)
3. lum = dot(col, (0.3, 0.59, 0.11))
4. **RT0 (Toned)** = lerp(lum, col, gSaturation) · lerp(1, gTint.rgb, gTint.a), alpha 1 → stored RGBA8 (clamped to 0–1).
5. **RT1 (Pong, "bright")** = gStrength · col^gTightness · max(0, lum − gThreshold) / lum, alpha 1 (RGB10A2, clamped;
   lum = 0 gives NaN → stored as 0).

Typical mapping with the defaults (white scale 1/F(4.9) = 1.775 at the minimum average): HDR 1 → 0.39, 2 → 0.63,
3 → 0.80, 5 → 1.0, 8 → 1.18 (clamped). For a brighter frame with average 2: 1 → 0.35, 5 → 0.89.

### 8.4 Bloom (only when section 22 "Bloom" = 2; else Toned is copied to the back buffer)

- 9-tap kernel weights = the standard normal pdf at −4…4 (0.000134, 0.004432, 0.053991, 0.241971, 0.398942, then mirrored).
- Step = 3 / (gScreenSize · gUVScale) in uv, gUVScale = (1, 1). **Quirk:** tap i (0…8) is offset by
  (i − 4) · (i/2) steps, i.e. **0, −1.5, −2, −1.5, 0, +2.5, +6, +10.5, +16** — asymmetric, smearing bloom towards +u/+v
  (right and down). The uv is clamped to [0, gUVScale]. Output alpha 1.
- Pass H: Pong (full-res bright) → Ping, viewport (W/2)×(H/2), direction (1, 0) (one bilinear tap per half-res pixel
  = 2×2 average of the bright buffer).
- Pass V: Ping → **Pong**, same half-size viewport, direction (0, 1): the blurred bloom lands in Pong's top-left quarter;
  the rest of Pong still holds the unblurred bright image.
- Blend into the back buffer: out = 1 − (1 − Pong(uv·0.5)) · (1 − Toned(uv)) (screen blend), alpha = dot(out.rgb,
  (0.3, 0.59, 0.11)). Sampling Pong at uv·0.5 with linear filtering touches the unblurred column/row at the quarter's edge;
  keep the same full-size Pong layout to match.

### 8.5 Nuke overrides (`Nuke::draw`, per active nuke, t = elapsed/duration ≤ 1)

- flash f = (t/0.05)² for t < 0.05, else 1 − min(1, (t − 0.05)/0.45)²; **mExposureAdjustment = 32·f²**.
- s = (t/0.05)² for t < 0.05, else min(1, (t − 0.05)/0.65)²; **mSaturation = s**.
- **mTint** = (238, 193, 14, ⌊255·(0.4·(1 − s))²⌋)/255 (orange, fading to weight 0).
- `end_scene` puts exposure adjustment and saturation back to 0 and 1; the tint keeps its last value (weight 0 at the end).

### 8.6 FXAA and reflections

- **FXAA** (`D3D11AntiAlias`): enabled by the `Anti-Aliasing` preference (prefs2, read at start-up). It copies the back
  buffer to an RGBA8 W×H texture and runs the NVIDIA FXAA PC shader (`Fxaa3_11.inc`, `FXAA_PRESET` 3: edge threshold 1/8,
  min 1/24, 16 search steps, acceleration 1, search threshold 1/4, subpixel on, cap 3/4, trim 1/4; luma = g·(0.587/0.299)
  + r, computed from rgb, not alpha; non-linear (gamma-space) input), rcpFrame = 1/gScreenSize. It runs before overlays,
  text and UI, which are therefore not anti-aliased.
- **Reflection buffers** (`ReflectionManager`, created with the card): two (W/4)×(H/4) textures, colour RGBA16F and depth
  R32. At `end_scene` (`D3D11GLGraphicCard::frame_ended`) the HDR target and the scene depth are resampled into them
  (`D3D11Resample`, ps_resample). Water with `HAS_SCREEN_REFLECTIONS` (enables 0x200000) binds them to t5/t6, so
  **reflections use the previous frame's pre-tonemap image**.
- The screen-quad VS reads `gInvertUV` (b6). Only the reflection resample sets it, to (0, 0), and leaves it bound; before
  that the slot is empty (reads 0). Effectively uv = (0,0) at the top-left for every post pass.

---

## 9. Settings that change the frame

| Setting | Where | Effect |
|---|---|---|
| Bloom (graphics section 22) | `scalability.section_scales[22] == 2` | Blur + screen blend vs plain copy of Toned |
| Water Reflections (section 23) | `section_scales[23] == 2` → technique "Normal Water", else "Min Water" | Normal Water: 0x204 \| 0x384000 (normal map, animated normals, Torrance, screen reflections), no depth write; Min Water: 0x204 \| 0x184000 (no reflections) |
| Terrain Maps (section 24) | `section_scales[24] == 2` | Base Terrain normal/spec/gloss maps |
| Anti-Aliasing | prefs2 `Anti-Aliasing` | FXAA pass |
| `light_units` | `GraphicPieces::init`: 1 when `vertex_process_mode == HARDWARE` (always true on the D3D11 backend) | bit 0x200 on Unit Blend and Death Blend |
| Minimap | `WorldMapBackground::render` sets `minimap_mode` | `NO_HDR` shaders for the minimap texture |

---

## 10. Metal implementation notes

- Formats: HDR `.rgba16Float`; luminance `.r16Float` 512² with mipmaps (`generateMipmaps` on a blit encoder — box filter
  like D3D11 for power-of-two sizes); Toned `.rgba8Unorm`; Ping/Pong `.rgb10a2Unorm`; drawable `.bgra8Unorm`/`.rgba8Unorm`
  **without sRGB** (the original does all post maths in gamma space and never converts). Depth `.depth32Float`, shared
  between the HDR pass and the back-buffer pass.
- Tone map is one fragment function with two colour attachments.
- Reproduce the blur's half-size viewport writing into a full-size Pong, the uv·0.5 lookup, and the asymmetric taps; the
  bloom shape depends on them.
- Constant buffers map 1:1: b0 PerScene, b1 PerObject, b2 PerMaterial, b3 LightData (all stages), b4 ToneMapData or
  BlurData (post passes), b5 nuke data, b6 ResampleData (VS).
- For the 3D view keep D3D's clip-space conventions (see renderstates.md §4) and the TL +0.5 pixel shift.
- Treat the HDR target as cleared to 0 each frame and the tonemapper as stateless (no adaptation history).

---

## 11. Evidence

| What | Where |
|---|---|
| Frame order, HDR bracket, tonemapper/AA calls, bloom = section 22 | `Scene::render` @008b3270 (`init_hdr` +0x224, `end_hdr` +0x228, `get_tonemapper` +0xbc, `get_anti_alias` +0xc0); `ConquestWin::on_redraw` @0077b010; `Game::loop_render` @005917e0 |
| Section ids (22 Bloom, 23 Water Reflections, 24 Terrain Maps) | `Data/optionswin.xml` `section_onoff`; offsets 0x58/0x5c/0x60 into `section_scales` |
| Tonemapper defaults, params → cbuffer, dirty flag | `D3D11ToneMapper::D3D11ToneMapper` @10042c40, `update_params` @10044860, `create_param_buffer` @10043680; `GSToneMapper` layout (d3dgl.pdb) |
| Editor names/ranges | `Scene::registration` @008bc930 |
| Per-frame reset of exposure adjustment / saturation | `D3D11GLSys::end_scene` @10021360 |
| Nuke overrides | `Nuke::draw` @0092b370 |
| Luminance / tonemap / blur / blend passes | `do_luminance` @10044b30, `do_tonemap` @10044ca0, `do_blur` @10044de0, `do_blend` @10045040, `do_run` @10044a30, `update_blur_data` @10045150 |
| Post-pass render targets and formats | `create_luminance_buffer` @10043e40 (512², R16F, GENERATE_MIPS), `create_ping_pong_buffers` @10043910, `create_toned_buffer` @10044150, `create_state_objects` @10044460 |
| HDR target | `D3D11GLSys::create_hdr_buffer` @10023f70 (PF_HDR → RGBA16F in `D3D11GLBuffer::init` @100316c0), `init_hdr` @1001d990 / `end_hdr` @1001d9b0, clear in `begin_drawing` @10020f10 |
| Back buffer / depth formats | `D3D11Context` ctor @10018f60 (R8G8B8A8_UNORM, clear 0, depth 1), `create_depth_stencil_view` @10019fa0 (R32_TYPELESS) |
| FXAA | `D3D11AntiAlias::init` @10017b80 (pref "Anti-Aliasing"), `run` @100186d0, `create_intermediate_buffer` @10018360 |
| Reflection copies | `D3D11GLGraphicCard::frame_ended` @1001d280, `ReflectionManager::createTextures` @10050f00, `D3D11Resample::run` @10041410 |
| gScreenSize / gCurrentTime | `D3D11Context::set_viewport` @1001a350, `update_frame_time` @1001cd40, `D3D11GLSys::begin_scene` @10020c60 |
| cbuffer binding (b0–b3 on VS+PS) and upload | `D3D11Context::create_constant_buffers` @1001bfe0, `update_for_render` @1001c350 |
| Ambient light | `D3D11GLSys::set_ambient` @1001d7c0 (Color, w=1) / @1001d860 (4 floats); callers `Game::init_light` @00590ef0, `System::init_graphics`, `TopMenu::init_light`/`draw_logo`, `GraphicPieces::parse_cmd` (found with `VCallScan`) |
| Sun light | `Game::init_light` @00590ef0; `Light::*` @00924d80…; `D3D11GLLight::set_ambient/diffuse` @100356d0/@10035710 (rgb·a), `update` @10035220, `set` @10035660 (`mEnableLight`) |
| Light colours per phase | `TerrainOut::prep_terrain_lighting` @0085bfd0 (called first in `TerrainOut::render` @008558b0), `GraphicPieces::prep_unit_lighting` @00905c10 (forced in `ObjectsOut::pop_all_zoomed_in` @00655e70; per unit in `GraphicPieces::draw_unit` @008e7da0) |
| Unit/land materials from XML | `GraphicPieces::init_lighting` @00904a30; `TileSet::init` @0087b370 (positional COLOR parse) |
| Material → cbuffer | `D3D11Context::set_material` @1001ab00; `D3D11GLGraphicCard::set_material` @1002b2c0 (flush first); `Material::close` @0053fe20 |
| Water material and techniques | `TerrainOut::one_time_init` @0085c150 ("Normal Water"/"Ocean", "Min Water"/"OceanMin"), `TerrainOut::render_water` @00852cd0 |
| NO_HDR = minimap | `D3D11GLGraphicCard::set_shader_options` @1002db30 (byte `GSGraphicCard::minimap_mode` +0x10); only writer `WorldMapBackground::render` (found with `FieldWriteScan`) |
| light_units | `GraphicPieces::init` @008ffcc0; `D3D11Context::init` sets `VERTEX_PROCESS_HARDWARE` |
| Terrain bake | `TerrainOut::light_vert` @00866f90, `light_land_vert` @00866e50, `calculate_norms` @008710b0, `CoordInfo::add_geometry` @0084bef0 |

Decompiles: `re/decomp/light/` (exe), `re/decomp/light_d3dgl/` (backend, plus `asm/`). Types: `re/analysis/types_light_d3dgl.txt`,
`re/analysis/types_gscard.txt`. New helper scripts: `re/ghidra_scripts/VCallScan.java` (virtual calls through a global
object pointer, by vtable offset), `FieldWriteScan.java` (stores to a field of the object a global points to),
`InsnGrep.java` (regex over all instructions).

---

## Open questions

1. **Screenshot calibration.** Check one frame against the original (Dirty tileset, units on grass, bloom on/off) before
   tuning anything; every constant above is from code, none from pixels.
2. **Unlit, non-TL render states write LDR values into the HDR target** (e.g. Road Blend, River Blend, Circle Blend,
   ZFire, Scaffold, Unit Low Detail Blend: enables 0x84/0x87 without 0x200 or 0x20000). After tone mapping a value of 1.0
   shows at about 0.39. Either those features really look dim in EE, or something brightens them (roads/rivers may have
   their own render paths). Verify against a screenshot. **Circle Blend is resolved**: selection circles (and health
   bars) are drawn by `ObjectsOut::draw_overlays` in step 10, after tone mapping, so they are not dimmed; `ronmap`
   draws them in a post-tonemap overlay pass (`ron::metal::overlay_pass`).
3. The per-unit dimming early-out (§4) suggests consecutive same-percent dimmed units render at full brightness. Verify in
   game (fogged enemy buildings/units), or check whether `draw_unit` draws before the material change matters.
4. The Sun's scene-graph rotation is assumed to be identity (direct child of the scene root). Confirm by reading the
   scene root's transform if lighting looks rotated.
5. Post passes inherit sampler s0 from the last scene draw. If that draw used a point-sampling state (0x400000), the
   original's bloom and FXAA would sample with point filtering. Assumed linear.
6. Water: the vertex colour (WaterTop/WaterBot) and texture combination plus the reflection hit test were not checked
   pixel-for-pixel; the stage setup is in `one_time_init` (stage 1 alpha uses a complemented texture arg).
7. The `Anti-Aliasing` preference default (the call passes 1 as an argument, probably the default) and whether the
   options screen toggles `mAAEnabled` at runtime or only after restart.
8. The mouse-over/selection overlays drawn after tone mapping use whatever render states they ask for; any with
   TL_FAKE_LIGHT (×5, ^1.9) saturate in the LDR back buffer. Confirm which overlay states those are.
