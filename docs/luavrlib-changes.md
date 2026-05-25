# luavrlib branch — change summary

This document captures the user-visible changes on the `luavrlib` branch vs.
upstream `praydog/UEVR` master, focused on the Lua API and the UObjectHook
UI. Use it as a one-stop reference when migrating scripts or learning what's
new in the menu.

## UObjectHook menu

### Live Function Caller (new, pinned panel)
A workbench-style panel at the top of `UObjectHook → Main` with 4 fixed
slots. Each slot accepts a drag-and-dropped UObject (from any TreeNode in
the view), an `InputText` for a function name, and a `Resolve` button (or
Enter in the input). Once resolved, the standard per-property editor +
`Call` button + return-value display renders inline.

Avoids drilling through `Objects-by-class → SomeUClass → SomeObject →
Functions → fn` every time you want to invoke the same function on a
specific actor.

### Drag-and-drop sources on every UObject / UClass TreeNode
Every UObject TreeNode in the Attached / Overlapping / Recent /
Objects-by-class views now publishes a `"UEVR_UObject"` drag payload.
Class entries in Objects-by-class publish `"UEVR_UClass"`. Targets:
- Live Function Caller's object slot
- Per-function editor's `ObjectProperty` / `InterfaceProperty` / `ClassProperty`
  param widgets in the in-tree caller

### Function-call editor (full rewrite of per-param widgets)
The old "Call / Enable / Disable / Hello-world!" buttons were replaced
with a real per-property editor (`render_function_call`). Supported
types:

| Property type                        | Editor              |
|--------------------------------------|---------------------|
| BoolProperty                         | Checkbox            |
| Byte / Int8 / Int16 / UInt16 / Int / UInt32 / Enum | InputInt   |
| Int64 / UInt64                       | InputScalar S64/U64 |
| Float                                | DragFloat           |
| Double                               | DragScalar (double) |
| Name / Str / Text                    | InputText           |
| Object / Interface                   | drag-drop target    |
| Class                                | drag-drop target    |
| StructProperty (Vector / Vector2D / Vector4 / Rotator / Quat / LinearColor) | DragFloat2/3/4 |

`Call` assembles a params buffer of `fn->get_properties_size()` bytes,
walks the parameter list to write each field at its property offset,
runs `process_event`, then displays the return value. Returns that are
`Object` / `Interface` / `Class` properties also render a small drag
handle so you can chain `call A → drag returned obj → call B` without
revisiting the tree.

### Return-value pretty-printing
`ObjectProperty` / `ClassProperty` returns show `[address] FullName`
instead of just the raw address. `StructProperty` returns are formatted
inline for the well-known POD structs (Vector, Vector2D, Vector4,
Rotator, Quat, LinearColor); UE4 vs UE5 vector size is detected at
runtime via `sdk::ScriptVector::static_struct()->get_struct_size()`.

### Missing-TreePop hardening
The "Objects by class" view used `m_meta_objects[obj]` (`operator[]`)
on a `unordered_map<*, unique_ptr<MetaObject>>`. When the key was
missing the map silently default-inserted a null `unique_ptr` (mutating
under a read-only `shared_lock`!) and the next `->full_name` deref
crashed inside the TreeNode body, leaving the parent TreeNode unbalanced.
ImGui's end-of-frame recovery then fired `"Missing TreePop()"` every
time the view was opened with a concurrently-removing UObject.

Replaced with a `find()`-based `get_meta()` helper that returns
nullptr and skips that entry, and wrapped every TreeNode in the
iteration (plus the outer "Objects by class" TreeNode itself) with a
`utility::ScopeGuard` that calls `TreePop` on scope exit. Per-class
`m_objects_by_class[uclass]` operator[] usages were also replaced with
`find()`.

Same hardening pattern applied to:
- `ui_handle_array_property` struct-element loop
  (previous `try { TreeNode … TreePop } catch { Text }` swallowed
  exceptions but left the TreeNode open)
- `ui_handle_struct` Inheritance / Functions / Properties tree nodes

## Lua API

### uevr.api_fast (new namespace)
Direct `sdk::AActor` / `sdk::USceneComponent` accessors that bypass
the generic reflection chain. Same underlying `process_event` per call
— but no Lua-side `__index` lookup, sol2 argument marshalling, or
StructObject return wrap. Roughly 3–5× faster per call on synthetic
benchmarks.

```lua
-- Actor (sdk::AActor — checks is_a internally, returns 0/false on mismatch)
uevr.api_fast.get_actor_location(actor) -> Vector3f
uevr.api_fast.get_actor_rotation(actor) -> Vector3f
uevr.api_fast.set_actor_location(actor, loc, sweep?, teleport?) -> bool
uevr.api_fast.set_actor_rotation(actor, rot, teleport?)         -> bool
uevr.api_fast.get_root_component(actor)                         -> UObject*
uevr.api_fast.get_component_by_class(actor, uclass)             -> UObject*
uevr.api_fast.get_all_components(actor)                         -> UObject[]
uevr.api_fast.destroy_actor(actor)                              -> bool

-- USceneComponent
uevr.api_fast.get_world_location(comp)  -> Vector3f
uevr.api_fast.get_world_rotation(comp)  -> Vector3f
uevr.api_fast.set_world_location(comp, loc, sweep?, teleport?)  -> bool
uevr.api_fast.set_world_rotation(comp, rot, sweep?, teleport?)  -> bool
uevr.api_fast.add_world_offset(comp, off, sweep?, teleport?)    -> bool
uevr.api_fast.add_world_rotation(comp, rot, sweep?, teleport?)  -> bool
uevr.api_fast.set_local_transform(comp, loc, quat, scale, sweep?, teleport?) -> bool
uevr.api_fast.get_socket_location(comp, name)                   -> Vector3f
uevr.api_fast.get_socket_rotation(comp, name)                   -> Vector3f

-- Type checks
uevr.api_fast.is_actor(obj)            -> bool
uevr.api_fast.is_scene_component(obj)  -> bool

-- Batch
uevr.api_fast.batch_actor_locations({a1, a2, ...}) -> Vector3f[]
    -- one sol2 crossing returns a Lua table of N actor locations;
    -- avoids per-element binding overhead in hot loops.
```

`set_local_transform` is a composite: writes location + rotation
(as quat) + scale in a single `K2_SetWorldTransform` call instead of
3 separate dispatches.

### Multistate / worker threads (new)
```lua
uevr.spawn_worker(name, bootstrap_source?)  -> bool   -- spawn a background lua state on its own thread
uevr.send_to_worker(name, source)           -> bool   -- queue a Lua chunk for that worker to run
uevr.stop_worker(name)                      -> bool   -- async stop + janitor join (non-blocking!)
uevr.has_worker(name)                       -> bool
uevr.set_shared(key, value)                          -- cross-state shared map (int/double/float/string/bool)
uevr.get_shared(key)                         -> value
uevr.run_on_game_thread(fn)                          -- defer fn to the next engine tick

uevr.log_info(msg)                                   -- routes to spdlog::info ("[lua] msg")
uevr.log_warn(msg)                                   -- routes to spdlog::warn
uevr.log_error(msg)                                  -- routes to spdlog::error
    -- Workers get the same triple; messages tagged "[lua-worker] msg".
    -- Necessary because workers have no usable stdout, so `print()` from
    -- a worker goes nowhere visible.
```

Workers own a private `ScriptState` (no `uevr.api` surface — most of
that is not thread-safe) and communicate via `set_shared` / `get_shared`.
`stop_worker` is non-blocking: it flips the worker's stop flag, hands
the `thread.join` off to a detached janitor, and returns immediately,
so calling it from a script panel button never freezes the render thread.

### TArray<T> read paths fixed
Reading an `ArrayProperty` from a `UObject*` or function return now
returns a real Lua table for:

- `FloatProperty`, `DoubleProperty` (and all sized int/uint variants)
- `BoolProperty` (decoded to lua booleans)
- `NameProperty` (FName usertype values; call `:to_string()` per element)
- `StrProperty` (each inline FString decoded to a wstring)
- `ObjectProperty`, `InterfaceProperty`, `ClassProperty` (UObject* / UClass*)
- `StructProperty` (StructObject elements, sized via UScriptStruct or
  UStruct properties_size fallback)

Old code cast all of these as `TArray<T*>` and walked
`sizeof(void*)` per element, which produced half-length garbage for
primitive arrays.

### GLM bindings (Vector / Quat / Matrix / Transform) — full rewrite
- Vector2/3/4 in both float and double precision
- Per-arity operator overloads: vec+vec, vec+scalar, scalar+vec, vec*vec
  (component-wise), vec*scalar, scalar*vec, unary minus, ==, /, etc.
- Cross-precision (`to_vec3d` / `to_vec3f`) and cross-arity (`to_vec2` /
  `to_vec3` / `to_vec4`) conversion methods
- Quaternion: `identity`, `slerp`, `conjugate`, `inverse`, `rotator` (Euler),
  `quaternion(from_rot)`, `to_mat4`, `to_vec4`, `rotate(v)` /
  `unrotate(v)`, `x_axis` / `y_axis` / `z_axis`, `q * vec3` / `q * vec4`
  metamethods
- Matrix4x4f/d: `identity`, `decompose` (→ Transform), `inverse` / `invert`,
  `transpose`, `determinant`, `transform_point` / `transform_vector`,
  `mat*vec3` / `mat*vec4` metamethods
- Transformf/d: `compose`, `inverse` / `invert`, `relative` /
  `relative_reversed`, `to_matrix` / `from_matrix`, mutable
  `translation` / `rotation` / `scale3d` fields (plus capitalised
  `Translation` / `Rotation` / `Scale3D` aliases)

### ImGui binding cleanup
- `bindings::open_imgui` deduplicated from 498 lines / 207 unique bindings
  to 207 alphabetical entries
- New (or now-working): `accept_payload`, `activate_item_by_id`,
  `begin_drag_drop_source`, `begin_drag_drop_target`,
  `set_drag_drop_payload`, `vslider_float`, `create_platform_window`,
  `bullet_text`, `text_colored`, `path_arc_to`, `path_arc_to_fast`,
  `path_bezier_cubic_curve_to`, `path_bezier_quadratic_curve_to`,
  `path_clear`, `path_elliptical_arc_to`, `path_fill_convex`,
  `path_line_to`, `path_line_to_merge_duplicate`, `path_rect`,
  `path_stroke`

### Custom inline / mid hooks from Lua
```lua
local h = uevr.hook_create_mid(target_addr, function(ctx)
    -- ctx is a UEVR_HookContext usertype with GPRs, rflags, rip, and
    -- read/write memory helpers; modifying ctx.* writes back into the
    -- target's saved register frame when the hook returns.
end)
uevr.hook_remove_mid(target_addr)
```

Plus `uevr.call_function(target_addr, args...)` for raw native calls
(uses an asmjit-generated stub that respects the Windows x64 ABI).

## ImGui multi-viewport — paused on this branch

`ImGuiConfigFlags_ViewportsEnable` is removed from `IMGUICONFIGFLAGS`
by default. Docking still works (panels dock back inside the host
overlay). The renderer/present-thread/recursion-guard scaffolding is
intact; re-enabling is a one-line change. See
`docs/imgui-multiviewport-status.md` for the open input/focus/z-order
issues and the 4-step plan to validate before flipping the flag back on.
