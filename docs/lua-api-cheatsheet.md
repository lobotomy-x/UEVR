# UEVR `luavrlib` — Lua API Cheat Sheet

Quick reference for the in-game Lua API. **★ = new on luavrlib**, **⚡ = changed recently** (signature/behavior). Everything else is standard.

In-game equivalent: the **Cheat Sheet** script panel (`CheatSheet.lua`) renders this same list live.

---

## ★ luavrlib highlights (start here)

| Call | What it does |
|---|---|
| ★ `uevr.api_fast` | Native fast-path table (skips `process_event` where possible). See **api_fast** below. |
| ★ `uevr.api_fast.find_class(name)` | Short-name → `UClass*` (deduped, `/Script/Engine.` fallback, cached). Lua `find_fast` shims this. |
| ★ `mat:transform_vector4(v3)` | World→clip: `m * vec4(v,1)`, **returns vec4 with w** (for the perspective divide in `world_to_screen`). |
| `draw.text(str, x, y, color)` | Draws on the **current window's** draw list (so other windows occlude it). To overlay the game, call inside a full-screen window — `ImGui.CanvasWindow()`. |
| ★ `Vector3:world_to_screen()` | Project a world point to screen px. Uses the cached PlayerCameraManager POV. Returns `Vector2f`; `(0,0)` = off-screen/behind. |

---

## imgui — Windows & layout

- ⚡ `imgui.begin_window(name [, open] [, flags])` / `imgui.end_window()` — **flags now optional** (`begin_window("Foo")` works; nil/omitted = 0). Auto-docks into the UEVR host. Name containing `Canvas` pins to the game viewport.
- ⚡ `imgui.begin_child_window(name [, size] [, border] [, flags])` / `imgui.end_child_window()` — **flags optional**.
- `imgui.begin_group()` / `end_group()` · `begin_rect()` / `end_rect([size][,rounding])` · `begin_tooltip()` / `end_tooltip()` · `begin_disabled([bool])` / `end_disabled()`
- `imgui.begin_viewport_sidebar(name, dir, size, flags)` · `create_platform_window()`
- `imgui.same_line([offset][,spacing])` · `spacing()` · `new_line()` · `separator()` · `separator_text(label)` · `indent([w])` · `unindent([w])`
- `imgui.get_cursor_pos()` / `set_cursor_pos(v2)` · `get_cursor_screen_pos()` / `set_cursor_screen_pos(v2)` · `get_cursor_start_pos()`
- `imgui.get_content_region_available()` · `get_window_pos()` · `get_window_size()` · `get_frame_height()` · `get_display_size()`
- `imgui.set_next_window_pos(v2)` · `set_next_window_size(v2)` · `set_next_window_scroll(v2)` · `set_next_window_dock_id(id[,cond])` · `set_next_window_docked(bool)`
- ★ `imgui.get_main_dockspace_id()` — the UEVR host dockspace id.

## imgui — Scroll

- `imgui.get_scroll_x/y()` · `get_scroll_max_x/y()` · `set_scroll_x/y(v)`
- ⚡ `imgui.set_scroll_here_x([ratio])` · `set_scroll_here_y([ratio])` · `set_scroll_from_pos_x(pos[,ratio])` · `set_scroll_from_pos_y(pos[,ratio])` — **ratio now optional** (defaults 0.5).

## imgui — Text

- `imgui.text(str)` · `text_colored(str, color)` · `text_disabled(str)` · `text_wrapped(str)` · `label_text(label, str)` · `bullet_text(str)` · `bullet()`
- `imgui.calc_text_size(str)` · `calc_item_width()` · `calc_item_size(...)`

## imgui — Buttons & basic widgets

- `imgui.button(label [, size] [, flags])` · `small_button(label)` · `arrow_button(id, dir)` · `invisible_button(id, size[,flags])` · `radio_button(label, active)`
- `imgui.checkbox(label, bool)` → `changed, value`
- `imgui.combo(label, current, items)` → `changed, value` · `begin_list_box / end_list_box` · `begin_multi_select / end_multi_select`
- `imgui.progress_bar(fraction [, size] [, overlay])`
- `imgui.input_text(label, str [, flags])` · `input_text_multiline(label, str [, size] [, flags])`

## imgui — Drag & Slider  *(return `changed, value`)*

- `imgui.drag_float(label, v, speed, min, max, fmt [, flags])`
- `imgui.drag_float2(label, v, ...)` — v = Vector2f / Vector2d / `{x,y}`
- `imgui.drag_float3(label, v, ...)` — v = Vector3f / Vector3d / `{x,y,z}`
- ⚡★ `imgui.drag_float4(label, v, ...)` — **now accepts Vector4f / Vector4d / `{x,y,z,w}`** (was strict Vector4f), with double-precision passthrough — matches drag_float2/3.
- `imgui.drag_int(label, v, speed, min, max, fmt [, flags])`
- `imgui.slider_float(label, v, min, max, fmt [, flags])` · `slider_int(...)` · `vslider_float(label, size, v, min, max, fmt [,flags])` · `vslider_int(...)`

## imgui — Color  *(return `changed, value`)*

- `imgui.color_edit(label, u32 [,flags])` · `color_edit3(label, Vector3f [,flags])` · `color_edit4(label, Vector4f [,flags])` · `color_edit_argb(...)`
- `imgui.color_picker(label, u32 [,flags])` · `color_picker3(label, Vector3f [,flags])` · `color_picker4(label, Vector4f [,flags])` · `color_picker_argb(...)`
- `imgui.create_imu32_color(...)` · `create_imvec4_color(...)`
- *Note:* color3/4 still take a strict `Vector3f`/`Vector4f` (RGB/RGBA). Pass a Vector, not a `{r,g,b}` table.

## imgui — Trees, tables, tabs, menus

- `imgui.tree_node(label [,flags])` / `tree_node_ptr_id(id,label[,flags])` / `tree_node_str_id(id,label[,flags])` / `tree_pop()`
- `imgui.collapsing_header(label [,flags])` · `set_next_item_open(bool[,cond])`
- `imgui.begin_table(id, cols [,flags][,outer_size][,inner_w])` / `end_table()` · `table_next_row([flags][,min_h])` · `table_next_column()` · `table_set_column_index(i)`
- `imgui.table_setup_column(label [,flags][,init_w][,id])` · `table_setup_scroll_freeze(c,r)` · `table_headers_row()` · `table_header(label)`
- ⚡ `imgui.table_get_column_name([col])` — **col now optional** (defaults current). · `table_get_column_count()` · `table_get_column_index()` · `table_get_row_index()` · `table_get_column_flags([col])` · `table_get_sort_specs()` · `table_set_bg_color(...)`
- `imgui.begin_menu_bar / end_menu_bar` · `begin_main_menu_bar / end_main_menu_bar` · `begin_menu(label[,enabled]) / end_menu` · `menu_item(label[,shortcut][,selected][,enabled])`

## imgui — Popups & tooltips

- `imgui.begin_popup(id[,flags]) / end_popup` · `begin_popup_modal(name[,open][,flags])` · `begin_popup_context_item([id][,flags])`
- `imgui.open_popup(id[,flags])` · `close_current_popup()` · `is_popup_open(id[,flags])` · `set_tooltip(str)` · `set_item_default_focus()`

## imgui — Drag & drop

- `imgui.begin_drag_drop_source([flags]) / end_drag_drop_source` · `begin_drag_drop_target / end_drag_drop_target`
- `imgui.set_drag_drop_payload(type, data)` · `accept_drag_drop(type)` / `accept_payload(type)` · `is_payload_accepted()` · `render_drag_drop()`

## imgui — Item / id state

- `imgui.is_item_hovered/active/clicked/edited/focused/visible()` · `is_item_toggled_open/selection()`
- `imgui.is_any_item_hovered/active/focused()` · `is_item_id()` · `get_item_id()` · `item_add(...)` · `item_size(...)`
- `imgui.push_id(x) / pop_id()` · `get_id(x)` · `get_id_from_pos(...)` · `push_override_id(id)` · `activate_item_by_id(id)`
- `imgui.set_next_item_width(w) / push_item_width(w) / pop_item_width()` · `set_next_item_open(...)` · `set_next_item_allow_overlap()`
- `imgui.push_item_flag(flag,bool) / pop_item_flag()` · `push_button_repeat(b) / pop_button_repeat()`
- `imgui.get_active_id()` · `get_hovered_id()` · `clear_active_id()` · `focus_item()` · `focus_window(...)`

## imgui — Input: keyboard & mouse

- `imgui.is_key_down/pressed/released(key)` · `get_key_index(key)`
- `imgui.is_mouse_down/clicked/double_clicked/released(button)` · `get_mouse()` (cursor pos)
- `imgui.get_clipboard()` / `set_clipboard(str)`

## imgui — Style

- `imgui.push_style_color(idx, color) / pop_style_color([count])` · `push_style_var(idx, val) / pop_style_var([count])`
- `imgui.push_font(f)/pop_font()` · `push_font_size(s)/pop_font_size()` · `load_font(path,size[,ranges])` · `get_default_font_size()` · `show_font_selector(label)` · `show_font_atlas()`

## imgui — Draw lists  *(low-level)*

- `imgui.get_foreground_draw_list()` · `get_background_draw_list()` · `get_window_draw_list()` → a `draw_list` with `:add_line/add_rect/add_rect_filled/add_quad/add_triangle/add_circle/add_circle_filled/add_ngon/push_clip_rect` (no `add_text` — use `draw.text`).
- `imgui.draw_list_path_clear()` · `draw_list_path_line_to(v2)` · `draw_list_path_stroke(color, closed, thickness)` — **current-window** list (wrap in `begin_window`/CanvasWindow).
- `imgui.push_clip_rect(min,max,intersect) / pop_clip_rect()`

## imgui — Windows: debug / demo

- `imgui.show_demo_window([open])` · `show_metrics_window([open])` · `show_debug_log_window([open])` · `show_stack_tool_window([open])` · `draw_scene_texture(...)`

---

## `draw` table  *(absolute screen coords; overlays the game)*

- `draw.text(str, x, y, color)` · `draw.text_ex(str, x, y, color, font, size)` — **current-window** draw list (occludable; wrap in a window / `ImGui.CanvasWindow()` to overlay the game).
- `draw.line(x1,y1,x2,y2,color)` · `filled_rect(x,y,w,h,color)` · `outline_rect(x,y,w,h,color)`
- `draw.filled_circle(x,y,r,color[,segs])` · `outline_circle(x,y,r,color[,segs])`
- `draw.filled_quad(x1,y1,...,x4,y4,color)` · `outline_quad(...)`
- *Disabled/commented in the binding (not callable yet):* `draw.world_to_screen`, `world_text`, `sphere`, `capsule`, `gizmo`, `cube`, `grid`.

---

## ★ `uevr.api_fast`  *(native fast paths)*

`get_actor_location/rotation(actor)` · `set_actor_location/rotation(actor, v)` · `get_world_location/rotation(comp)` · `set_world_location/rotation(comp, v)` · `add_world_offset(comp, v)` · `add_world_rotation(comp, r)` · `set_local_transform(comp, t)` · `get_socket_location/rotation(comp, name)` · `get_root_component(actor)` · `get_component_by_class(actor, class)` · `get_all_components(actor)` · `destroy_actor(actor)` · `batch_actor_locations(actors)` · `is_actor(o)` · `is_scene_component(o)` · ★ `find_class(name)` · `refresh_class_cache()`

---

## Vectors / Matrix / Quat  *(metamethods)*

- **Vector2f/3f/4f** (+ `d` double variants): `+ - * /`, `:length()`, `:normalize()`, `:dot(o)`, `:cross(o)` (vec3), `.x/.y/.z/.w`, `Vector3f.new(x,y,z)`.
- ★ **Vector3:world_to_screen()** → `Vector2f` · ★ **Vector3:world_to_ndc()** → `ndcx,ndcy,ndcz,w`.
- **Matrix4x4f/d**: `*` (mat*mat, mat*vec), `:transform_vector(v3)` (w=0, no translation), ★ `:transform_vector4(v3)` → vec4 (w=1, keeps w), ★ `:transform_vector4w(v4)` → vec4, `:inverse()`, `:transpose()`.
- **Quaternionf/d**: `* `, `:normalize()`, `:to_euler()`, etc.

---

## `uevr.api` essentials

- `uevr.api:find_uobject(name)` · `get_player_controller(i)` · `get_local_pawn(i)` · `spawn_object(class, outer)` · `add_component_by_class(actor, class, deferred)` · `to_uobject(addr)`
- `uevr.params.functions.log_info/log_warn/log_error(str)` — ⚡ now **safe** (any string, no varargs crash).
- Callbacks: `uevr.sdk.callbacks.on_frame / on_draw_ui / on_pre_engine_tick / on_post_engine_tick(fn)`.
- UObject methods: `:get_full_name()` · `:get_class()` · `:get_fname()` · `:get_property(name)` · `:set_property(name,val)` · `:call(fn_name, ...)` · `:as_class()/:as_struct()/:as_function()`.

---

## Notes

- ⚡ **Optional/omitted args no longer throw** for the functions marked ⚡ (sol2 ignores C++ defaults, so those params became `sol::object` + default-on-nil). If you hit `no matching function call takes this number of arguments`, an optional param still needs the `sol::object` treatment — report it.
- `draw.*` / `draw_list_path_*` take **screen pixels**, not window-relative.
- Use enum constants for flags (e.g. `imgui.ImGuiWindowFlags_NoTitleBar`); `ImGui.CalcFlags` combines flag names.
