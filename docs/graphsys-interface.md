# The GraphSys backend contract (what a macOS backend must implement)

Source: vtables of `d3dgl.dll` with `d3dgl.pdb` applied. Full slot list with signatures:
`re/iface/d3dgl_vtables.txt` (regenerate with `ghidra_scripts/DumpVtables.java`).

`riseofnations.exe` loads `d3dgl.dll` and calls its only export, `get_graphsy_object_ptr`,
which returns a `D3D11GLSys*`. The class hierarchy is `GraphSys` → `SGLSys` → `D3D11GLSys`. The game
then talks to the backend only through two vtables:

- **`GraphSys`** (153 slots, implemented by `D3D11GLSys`): app/window, display mode, resource factories, frame lifecycle,
  transforms, input pump
- **`GSGraphicCard`** (42 slots, implemented by `D3D11GLGraphicCard`): the per-draw state machine and primitive submission

> Caveat: the binary was linked with identical-COMDAT folding. Trivial default bodies (`return 0`, `{}`) are merged,
> so some slots show an unrelated name (`BaseParamRegister::on_field_change`, `GraphSys::check_capability`, ...).
> Treat those slots as "default no-op / returns 0", and confirm each at its call sites in `riseofnations.exe`.

## GraphSys slot groups

| Slots | Group | Examples |
|---|---|---|
| 0–19 | App and window lifecycle | `init_app(SysMessage*, String*, BufferBase*, ...)`, `cleanup_system`, `show_app`/`hide_app`, `minimize_app`, `get_window_handle`, `set_window_pos`, `get_cursor_pos`, `invalidate_rect`, `get_screen_dimensions`, `is_window_active`, `is_windowed` |
| 23–35 | Display mode and device | `close_device`, `init_display_quick(w, h, PixFormat, ...)`, `check_vidcard_compat`, `enum_device` → `ScreenResolution[]`, `change_resolution`, `enable_vsync`, `capture_screen`, `texture_format_supported` |
| 36–60 | Resource factories (lazy: `XBase` → backend `GSX`) | `get_font_data`, `get_buffer_data`, `get_sprite_data`, `get_light_data`, `get_texture_data(TextureBase*, MemType)`, `get_vertstream_data`, `get_indexbuffer_data`, `get_video_data`, `get_cursor_data`, `get_shader_data`, `get_tonemapper`, `get_anti_alias`, `get_streaming_system`, plus the matching `delete_*_data` |
| 61–68 | Misc systems | sprite scale, `flip(Rect*)`, font system, palette system, profiler |
| 70–99 | Frame and stats | `begin_scene`/`end_scene`, `begin_drawing`/`end_drawing`, `restore_device`, `clear_buffer`/`clear_target_buffer`/`clear_zbuffer`, `set_clear_color`/`set_clear_depth`, `set_ambient`, poly/vert/drawprim counters, `get_card` → `GSGraphicCard*` |
| 100–115 | Transforms and viewport | `set_world_transform(Transform<float>*)`, `set_camera_transform(Matrix<float>*)`, `set_projection(fov?, aspect?, near?, far?, Matrix*)`, `set_projection_parallel`, `set_texture(stage, TextureBase*)`, `get_viewport`/`set_viewport`, `get_graphics_info` |
| 116–131 | **Event pump and input** | `do_timer`, `wait_task`, `do_task`, `do_non_input`, `do_draw`, `do_key`, `do_char`, `do_mouse`, `do_mouse_move`, `do_mouse_stroke`, `flush_keyboard`/`flush_mouse`/`flush_timer`, text batching |
| 133–152 | Extended Edition additions | `set_stock_cursor`, `init_hdr`/`end_hdr`, `load_and_create_shader(String*, ...)`, `set_render_state(RenderState*)`, `render(start, count, PrimitiveType, ...)`, `render(..., D3D11GLIndexBuffer*, ...)`, `get_resample` |

## GSGraphicCard (draw submission)

This is a fixed-function-style state machine with internal batching:

`init`, `close`, `reset`, `flush`, `set_material(Material*)`, `set_texture(stage, TextureBase*)`, `set_render_state(RenderState*)`,
`set_world_transform`, `set_prim_type(PrimitiveType)`, `set_vert_array(Vert3Array* | Vert4Array* | VoidArray*)`,
`set_norm_array`, `set_colors(diffuse, specular)`, `set_color_array`, `set_tex_array(uv0, uv1)`,
`set_pixel_shader_constants(float*, n)`, `set_vert_stream(GSVertStream*)`, `set_index_buffer(GSIndexBuffer*)`,
`set_vertex_shader(id)`, `set_shader_data`, `add_clear_rect`, `set_offset`, `get_internal_stream`/`release_internal_stream`,
`add_tl_prim` (pre-transformed), `add_prim(count)`, `set_render_mode`, `set_poly_render`, `set_prim_render`,
`repeat_last_prim`, `lock_texture`, `lock_renderstate`, `render_normals`, `resolution_changed`.

## Implications for the macOS port

1. The backend owns the window, the input and the main event pump as well as rendering. The Mac backend is
   Metal for drawing plus AppKit (or SDL3) for the window, keyboard, mouse and timers.
2. The draw API is an immediate-mode batcher (`set_*` then `add_prim`). It maps onto a Metal layer that streams vertices
   into ring buffers and caches pipeline states keyed by `RenderState`, the vertex format and the shader id.
3. Shaders: the HLSL in `shaders\` (global VS/PS, tonemap, FXAA, gaussian, luminance, resample, blend) translates to MSL,
   either by hand or with Apple's Metal Shader Converter / SPIRV-Cross. How `set_render_state` turns a `RenderState`
   into D3D11 state and uber-shader `#define`s, and the decoded table of the 50 named states, are in
   [renderstates.md](renderstates.md) (data: `re/iface/renderstates.json`).
4. In our own engine we do not have to match this ABI; it tells us exactly which renderer features the
   game relies on. Doing an ABI-compatible `d3dgl.dll` replacement would only help a Windows-side shim, not a native Mac build.
