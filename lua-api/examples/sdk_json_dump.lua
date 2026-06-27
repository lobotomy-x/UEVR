-- sdk_json_dump.lua
--
-- Reflection -> JSON dumper built on the UEVR Lua API. Complements the C++ Class Browser
-- "Export -> JSON" buttons (UObjectHook) for users who'd rather dump from a script / the REPL.
--
-- Defines a global table `SDKDump`:
--   SDKDump.struct(name_or_obj)  -> JSON string for a UClass / UScriptStruct / UFunction:
--                                   declared properties (name, type, offset, raw PropertyFlags)
--                                   and declared functions (raw FunctionFlags + params).
--   SDKDump.batch(names)         -> JSON array string; names is a table of full object paths.
--
-- Flags are emitted as raw hex strings ("flags":"0x..."). The C++ export additionally emits
-- decoded flag-name arrays; cross-reference there if you want names. Lua is sandboxed (no `io`),
-- so this returns a string rather than writing a file -- print() it or capture it yourself.
--
-- Example (REPL):
--   print(SDKDump.struct("Class /Script/Engine.Actor"))

local api = uevr.api

local function esc(s)
    return (tostring(s):gsub('\\', '\\\\'):gsub('"', '\\"'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('\t', '\\t'))
end

-- pcall wrapper: returns the call's value, or `default` on error/nil.
local function safe(fn, default)
    local ok, v = pcall(fn)
    if ok and v ~= nil then
        return v
    end
    return default
end

-- find_uobject is reached as a method (api:find_uobject) in the normal script env, but the
-- MCP REPL exposes uevr.api as a plain function table (api.find_uobject). Try both so the
-- same script works either place.
local function find_uobject(name)
    local ok, r = pcall(function() return api:find_uobject(name) end)
    if ok and r ~= nil then
        return r
    end
    ok, r = pcall(function() return api.find_uobject(name) end)
    if ok then
        return r
    end
    return nil
end

local function resolve(obj)
    if type(obj) == "string" then
        return find_uobject(obj)
    end
    return obj
end

-- A UClass returned by find_uobject is already a UStruct, but as_struct() makes the
-- reflection interface explicit and survives binding differences across envs.
local function as_struct(obj)
    return safe(function() return obj:as_struct() end, nil) or obj
end

local function dump_property(f)
    local nm  = safe(function() return f:get_fname():to_string() end, "?")
    local ty  = safe(function() return f:get_class():get_name():to_string() end, "?")
    local p   = safe(function() return f:as_property() end, nil)
    local off = p and safe(function() return p:get_offset() end, -1) or -1
    local fl  = p and safe(function() return p:get_property_flags() end, 0) or 0
    return string.format('{"name":"%s","type":"%s","offset":%d,"flags":"0x%x"}', esc(nm), esc(ty), off, fl)
end

local SDKDump = {}

function SDKDump.struct(obj)
    local raw = resolve(obj)
    if not raw then
        return "null"
    end
    local s = as_struct(raw)

    local name  = safe(function() return raw:get_fname():to_string() end, "")
    local full  = safe(function() return raw:get_full_name() end, "")
    local super = safe(function()
        local sup = s:get_super_struct()
        return sup and sup:get_fname():to_string() or ""
    end, "")

    local props = {}
    pcall(function()
        local f = s:get_child_properties()
        while f do
            props[#props + 1] = dump_property(f)
            f = f:get_next()
        end
    end)

    local fns = {}
    pcall(function()
        local ch = s:get_children()
        while ch do
            local fnobj = safe(function() return ch:as_function() end, nil)
            if fnobj then
                local fnm = safe(function() return ch:get_fname():to_string() end, "?")
                local ffl = safe(function() return fnobj:get_function_flags() end, 0)
                local params = {}
                pcall(function()
                    local p = fnobj:get_child_properties()
                    while p do
                        params[#params + 1] = dump_property(p)
                        p = p:get_next()
                    end
                end)
                fns[#fns + 1] = string.format('{"name":"%s","flags":"0x%x","params":[%s]}',
                    esc(fnm), ffl, table.concat(params, ","))
            end
            ch = ch:get_next()
        end
    end)

    return string.format('{"name":"%s","full_name":"%s","super":"%s","properties":[%s],"functions":[%s]}',
        esc(name), esc(full), esc(super), table.concat(props, ","), table.concat(fns, ","))
end

-- Batch: pass a table of full object paths (e.g. {"Class /Script/Engine.Actor", ...}).
function SDKDump.batch(names)
    local parts = {}
    for _, n in ipairs(names or {}) do
        parts[#parts + 1] = SDKDump.struct(n)
    end
    return "[" .. table.concat(parts, ",") .. "]"
end

_G.SDKDump = SDKDump
print('[sdk_json_dump] loaded. Try: print(SDKDump.struct("Class /Script/Engine.Actor"))')

return SDKDump
