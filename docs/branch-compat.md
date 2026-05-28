# Lua Script Compat Across UEVR Branches

The `luavrlib` branch adds native C++ features that supersede a number of
Lua-side polyfills users wrote against stock UEVR (`master` branch). To
keep scripts working on BOTH builds without shadowing the native versions,
ship `uevr_branch_compat.lua` alongside your scripts and gate polyfills
through it.

## The helper

`uevr_branch_compat.lua` (copy into your `UEVR/Scripts/` directory):

```lua
local M = {}
local ok, branch = pcall(function()
    return uevr.params.functions:get_branch()
end)
M.branch = (ok and branch) or "unknown"
M.is_luavrlib = (M.branch == "luavrlib")
M.is_stock    = not M.is_luavrlib
function M.if_stock(fn)    if M.is_stock    then return fn() end end
function M.if_luavrlib(fn) if M.is_luavrlib then return fn() end end
return M
```

## Pattern: guard a Lua polyfill

```lua
local compat = require("uevr_branch_compat")
compat.if_stock(function()
    -- Only runs on stock UEVR. The luavrlib branch already has
    -- imgui.text_disabled bound natively (commit 35b1b29).
    function imgui.text_disabled(s)
        imgui.text_colored(s, 0xff808080)
    end
end)
```

## Pattern: skip a whole panel / on_draw_ui registration

```lua
local _ok, _compat = pcall(require, "uevr_branch_compat")
if _ok and _compat.is_luavrlib then
    return -- bail before the on_draw_ui callback that registers the panel
end
uevr.sdk.callbacks.on_draw_ui(function() ... end)
```

## What's superseded on luavrlib

| Native feature (luavrlib) | Lua polyfill to skip on luavrlib |
|---|---|
| `imgui.text_disabled` (cbd034d / 35b1b29) | any user `function imgui.text_disabled(s) ... end` |
| `imgui.drag_float3` Vector3f/Vector3d/{x,y,z} coerce (cbd034d) | wrappers that convert vec types before calling old `drag_float3` |
| `Quaternionf.from_euler(vec3)` / `Quaterniond.from_euler(vec3)` (697aa8c) | manual `glm.radians`-based euler→quat factories |
| `Transformf.compose(t,r,s)` / `Transformd.compose(t,r,s)` (49b4f7c) | typed-fragile `Transformf.new(Vector3f, Quaternionf, Vector3f)` callers |
| `FConsoleManager:get_console_objects()` returns ipairs-able table (cbd034d) | manual `TArray<ConsoleObjectElement>` iteration |
| Class Browser + Class Inspector windows (7e3c715, 250ecd9) | Lua-side "UObjectArray Test" / class lister panels |
| Function Caller window + UObject param picker (ee9176a) | Lua-side function-call builder panels |
| TArray scalar element view in inspector (6d98c8c) | Lua dumpers that walk TArray byte-by-byte for inspection |
| `api_fast.*` actor/component fast paths | Lua wrappers calling individual GetActorLocation / SetActorRotation |

## Things that look redundant but are NOT

- `find_fast(...):get_objects_matching()` calls in gameplay scripts (motion
  controllers, skeletal meshes, etc.) are feature implementations, not
  browser UIs. Don't gate these.
- `:get_class_default_object()` lookups for static-library access (Kismet
  System, GameplayStatics) are also feature implementations.
- The `panels = {}` local tables in `UI/UObjectArray.lua` etc. are dead code
  unless something exports them. Check before gating.

## Verifying

The helper logs its branch detection once at load time:

```
[uevr_branch_compat] branch=luavrlib is_luavrlib=true
```

If you see `is_luavrlib=false` on this test build, `get_branch()` isn't
returning what we expect — check the build hash.
