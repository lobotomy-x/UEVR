--[[
  mid_hook.lua — register-level "mid-function" hooks from Lua (uevr.hook_create_mid)

  WHAT IT IS
    A mid hook plants a breakpoint-like trampoline at ANY code address and calls your Lua
    function with the live CPU register state. Unlike a normal pre/post UFunction hook it:
      * works on ANY address — native engine code, game code, a pattern-scan result — not just
        reflected UFunctions;
      * gives raw register read/write (rax, rcx, rdx, ... r15, rip, rsp);
      * can read/write arbitrary memory from inside the hook (ctx:read_float / ctx:write_qword ...);
      * can MODIFY state mid-execution — clamp a computed value, redirect a pointer, force a branch.

  WHEN TO USE IT (vs a normal function hook)
    * You found an address via reverse engineering / a pattern scan and want the register state there.
    * The value you need isn't a UFunction arg/return — it's computed into a register mid-body.
    * You want a `this`/object pointer for a type the reflection system can't reach.
    If you only need a UFunction's args/return, a normal pre/post hook is simpler and safer.

  API
    local hook = uevr.hook_create_mid(addr, function(ctx) ... end)
        addr : a numeric address, OR a void*/lightuserdata such as
               some_ufunction:get_native_function(). (uevr.to_address(ptr) converts a pointer
               to a number if you want to keep one around / pass it to call_function.)
        ctx  : registers as fields — ctx.rcx, ctx.rax, ctx.rdx, ctx.r8 ... ctx.rip, ctx.rsp — both
               readable and writable; plus memory helpers ctx:read_qword(a)/read_dword/read_float/...
               and ctx:write_qword(a,v)/write_float(a,v)/...
        returns a hook handle (hook.target_address, hook:remove()) or nil on failure.
    uevr.hook_remove_mid(addr)   -- remove by address (number or pointer)
    hook:remove()                -- or via the handle

  SAFETY — READ THIS
    The callback runs ON THE GAME THREAD, in the middle of the target function, potentially every
    frame. Keep it TINY: snapshot the registers you need into Lua variables and do any heavy work
    (printing, calling game functions, allocating) elsewhere, e.g. in on_pre_engine_tick. A slow or
    reentrant mid hook on a hot function will stutter or crash the game.
]]

local api = uevr.api
UEVR_UObjectHook.activate()

local function fmt_hex(v) return string.format("0x%X", v or 0) end

-- =================================================================================================
-- Reusable tool: a "soft breakpoint". Point it at an address; it records the register state every
-- time that address is hit. Read bp.last for the latest snapshot and bp.hits for the count. Pass
-- { one_shot = true } to auto-remove after the first hit (safe even on per-frame functions).
-- =================================================================================================
local function soft_breakpoint(address, opts)
    opts = opts or {}
    local bp = { hits = 0, last = nil, one_shot = opts.one_shot or false, handle = nil }

    bp.handle = uevr.hook_create_mid(address, function(ctx)
        -- minimal work only: snapshot registers, then get out
        bp.hits = bp.hits + 1
        bp.last = {
            rax = ctx.rax, rcx = ctx.rcx, rdx = ctx.rdx, r8 = ctx.r8, r9 = ctx.r9,
            rip = ctx.rip, rsp = ctx.rsp,
        }
        if bp.one_shot and bp.handle ~= nil then
            bp.handle:remove()
            bp.handle = nil
        end
    end)

    return bp
end

--[[ -----------------------------------------------------------------------------------------------
  The real payoff: MODIFY a value mid-function. Say RE shows a function loads a field-of-view float
  from someStruct (pointer in rbx, field at +0x40) before clamping it elsewhere. You can clamp it
  yourself, on any code path, without touching a single UFunction:

      uevr.hook_create_mid(0x7FF6_1234_5678, function(ctx)
          local p = ctx.rbx + 0x40
          if ctx:read_float(p) > 120.0 then
              ctx:write_float(p, 120.0)   -- write it straight back into game memory
          end
      end)

  Or capture a non-reflected object's `this` pointer the first time some method runs, then use it
  elsewhere via UEVR_UObjectHook / read_*.
----------------------------------------------------------------------------------------------- ]]

-- =================================================================================================
-- Live, self-verifying demo. We hook the NATIVE exec thunk of a common UFunction
-- (PlayerController.GetControlRotation), then call that function ourselves so the hook fires
-- deterministically. For a native UFunction exec the x64 args are
--   (UObject* this -> rcx, FFrame& -> rdx, void* result -> r8)
-- so the captured rcx must equal the address of the object we called it on. We prove exactly that.
-- (A mid hook on the exec thunk only fires for ProcessEvent/script/BP calls — which is why we call
--  it ourselves rather than waiting for the engine, which may invoke the C++ method directly.)
-- =================================================================================================
local demo_done = false

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    if demo_done then return end

    -- need a live PlayerController to call on; wait until the world has one
    local pc_class = api:find_uobject("Class /Script/Engine.PlayerController")
    if pc_class == nil then return end
    local pc = UEVR_UObjectHook.get_first_object_by_class(pc_class)
    if pc == nil then return end

    local fn_obj = api:find_uobject("Function /Script/Engine.PlayerController.GetControlRotation")
    local fn = fn_obj ~= nil and fn_obj:as_function() or nil
    if fn == nil then
        print("[mid_hook] demo: GetControlRotation UFunction not found - edit the target. Demo skipped.")
        demo_done = true
        return
    end

    local addr = uevr.to_address(fn:get_native_function())
    if addr == 0 then
        print("[mid_hook] demo: GetControlRotation has no native code. Demo skipped.")
        demo_done = true
        return
    end

    local bp = soft_breakpoint(addr, { one_shot = true })
    if bp.handle == nil then
        print("[mid_hook] demo: hook_create_mid failed @ " .. fmt_hex(addr))
        demo_done = true
        return
    end
    print("[mid_hook] demo: armed one-shot mid hook on GetControlRotation native @ " .. fmt_hex(addr))

    pc:call("GetControlRotation") -- routes through ProcessEvent -> the hooked exec thunk -> our callback

    if bp.last ~= nil then
        local s = bp.last
        print(string.format("[mid_hook] HIT: rcx(this)=%s rdx(FFrame)=%s r8(result)=%s rip=%s",
            fmt_hex(s.rcx), fmt_hex(s.rdx), fmt_hex(s.r8), fmt_hex(s.rip)))
        print(string.format("[mid_hook] PlayerController address=%s  ==  rcx ?  %s",
            fmt_hex(pc:get_address()), tostring(pc:get_address() == s.rcx)))
        print("[mid_hook] one-shot hook removed itself. Demo complete.")
    else
        print("[mid_hook] demo: hook did not fire on our call; removing it.")
        if bp.handle ~= nil then bp.handle:remove() end
    end

    demo_done = true
end)

uevr.sdk.callbacks.on_script_reset(function()
    print("[mid_hook] reset")
end)

print("[mid_hook] loaded. Waiting for a PlayerController to run the register-capture demo.")
