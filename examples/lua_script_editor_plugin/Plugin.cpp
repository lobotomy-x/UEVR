// lua_script_editor_plugin: a full Lua editor + REPL window inside UEVR's
// overlay. Uses ImGuiColorTextEdit (TextEditor.cpp / TextDiff.cpp) for syntax
// highlighting and links its own copy of ImGui — same version + imconfig as
// UEVR's own build, so the in-process ABI matches (see CMakeLists.txt).
//
// Bridge to the host:
//   * on_imgui_frame fires with the host's ImGuiContext+allocators. We rebind
//     them on every call so our static ImGui copy operates against UEVR's data.
//   * Only render while fns->is_drawing_ui() is true — that's the exact flag
//     Framework's own input-eating logic checks (Framework::is_drawing_ui()
//     returns m_draw_ui). Rendering unconditionally meant our window wanted
//     keyboard/mouse focus even while the host had decided input belongs to
//     the game, so keystrokes leaked through to the game AND our editor at
//     the same time, and there was no way to bring the window back once
//     closed except reloading — both fixed by tying our visibility to the
//     same show/hide state as UEVR's own menu.
//   * exec_lua_chunk runs code in UEVR's main lua state.
//   * get_persistent_dir() gives us the scripts folder so the file list and
//     save/open buttons round-trip to disk.
//
// Hotkeys (only when the editor window has focus):
//   F10        toggle window (only takes effect while UEVR's own UI is open)
//   Ctrl+S     save
//   Ctrl+R     run buffer
//   Ctrl+Enter run buffer
//
// Known gaps not attempted here (flagged, not implemented):
//   * No automatic dot-triggered autocomplete while typing — the Inspect tab
//     gives the same live-member-discovery value as an explicit action
//     instead, which is far lower-risk to the core typing experience.
//   * No breakpoints/stepping. That needs lua_sethook cooperation wired
//     through PluginLoader.cpp/LuaLoader.cpp — a separate, larger effort.
//   * No raw memory-address object pinning. Expression-based watches
//     (re-evaluated every refresh) are used instead — deliberately, since a
//     cached raw pointer to something UE's GC/allocator can reuse is exactly
//     the bug class fixed in SDKFast.cpp's find_class a few commits back.

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

#include "TextEditor.h"

#include "uevr/Plugin.hpp"

using namespace uevr;
namespace fs = std::filesystem;

namespace {

constexpr const char* TAG = "[lua_editor]";
constexpr WPARAM TOGGLE_KEY = VK_F10;

bool g_window_open = true;

// ---------------------------------------------------------------------------
// Multi-document tabs. Each open file/buffer owns its own TextEditor instance
// (heap-allocated — ImGuiColorTextEdit isn't cheaply copyable/movable).
// Modified-state is derived from the undo index at last save/load instead of
// diffing text every frame.
// ---------------------------------------------------------------------------
struct Document {
    TextEditor* editor = nullptr;
    std::string path;          // empty = untitled, never saved
    std::string display_name;  // filename, or "untitledN.lua"
    size_t saved_undo_index = 0;
};
std::vector<Document> g_docs;
int g_active_doc = -1;
int g_pending_select_doc = -1; // set right after opening/creating a doc
int g_untitled_counter = 1;

bool doc_modified(const Document& d) {
    return d.editor != nullptr && d.editor->GetUndoIndex() != d.saved_undo_index;
}

// Scripts directories.
//   g_scripts_dir        = <persistent>/scripts/        (per-game, from UEVR's get_persistent_dir)
//   g_global_scripts_dir = %APPDATA%/UnrealVRMod/UEVR/scripts/  (loaded for every game)
fs::path g_scripts_dir;
fs::path g_global_scripts_dir;

// Loaded file list (re-scanned on demand).
struct ScriptEntry {
    std::string name;       // display name (with prefix like [autorun] or [global])
    fs::path    path;       // full path
    bool        autorun;
    bool        global;
};
std::vector<ScriptEntry> g_scripts;
int  g_selected = -1;
std::string g_status;

// Output log
std::deque<std::string> g_log;
constexpr size_t LOG_MAX = 200;

bool g_autorun_target = false;
bool g_save_to_global = false;   // when true, Save uses g_global_scripts_dir
bool g_show_whitespace = false;
char g_filename_buf[128] = "untitled.lua";

// Bottom-pane tabs: Output / Globals / Modules / Inspect
enum class BottomTab { Output, Globals, Modules, Inspect };
BottomTab g_bottom_tab = BottomTab::Output;

// Buffers populated by exec_lua_chunk. The text dumps remain for "Copy"; the
// interactive views parse machine rows out of g_tree_buf.
constexpr size_t LUA_DUMP_BUF = 64 * 1024;
std::vector<char> g_globals_buf(LUA_DUMP_BUF);
std::vector<char> g_modules_buf(LUA_DUMP_BUF);
std::vector<char> g_tree_buf(LUA_DUMP_BUF);
bool g_globals_autorefresh = false;
bool g_modules_autorefresh = false;
uint64_t g_dump_tick = 0;

// Perf guard: a "Refresh" on a tree with many already-expanded nodes used to
// re-query every one of them synchronously in the SAME frame (one
// exec_lua_chunk round-trip per node), producing a visible stutter. Cap how
// many nodes can (re)load per frame; the rest simply stay stale one more
// frame and retry, spreading the cost out instead of bursting it.
int g_loads_this_frame = 0;
constexpr int MAX_LOADS_PER_FRAME = 2;

// Resizable layout (dragged via splitters).
float g_left_w   = 220.0f;
float g_bottom_h = 160.0f;

// ---------------------------------------------------------------------------
// Interactive Lua tree: nodes lazily populated by per-path queries against the
// main lua state. `expr` is a Lua expression that reaches the node from a root
// (e.g. _G["APIUE"]["cfg"][3]); children are fetched on first expand and
// re-fetched after a Refresh (loaded=false while imgui keeps the open state).
// ---------------------------------------------------------------------------
struct LuaNode {
    std::string display;    // key as shown
    std::string suffix;     // accessor suffix, already quoted by Lua: ["k"] / [3]
    std::string expr;       // full path expression (parent.expr + suffix)
    std::string type;       // lua type name
    std::string preview;    // short value preview
    bool expandable = false;
    bool loaded = false;
    std::vector<LuaNode> children;
};
LuaNode g_globals_tree; // children = user globals
LuaNode g_modules_tree; // children = package.loaded entries
bool g_globals_tree_ready = false;
bool g_modules_tree_ready = false;

// Ad-hoc expression inspector (Inspect tab).
char g_inspect_expr[256] = "uevr.vr";
LuaNode g_inspect_node;
std::string g_inspect_header;
bool g_inspect_has_result = false;

struct WatchEntry {
    std::string expr;
    std::string type;
    std::string preview;
};
std::vector<WatchEntry> g_watches;
bool g_watch_autorefresh = true;

void log(const std::string& s) {
    if (g_log.size() >= LOG_MAX) g_log.pop_front();
    g_log.push_back(s);
}

void rescan_scripts() {
    g_scripts.clear();

    auto scan = [&](const fs::path& dir, bool autorun, bool global) {
        std::error_code ec;
        if (dir.empty() || !fs::exists(dir, ec)) return;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            auto ext = e.path().extension().string();
            if (ext != ".lua") continue;
            ScriptEntry se;
            se.path    = e.path();
            se.autorun = autorun;
            se.global  = global;
            std::string prefix;
            if (global)  prefix += "[global] ";
            if (autorun) prefix += "[autorun] ";
            se.name = prefix + e.path().filename().string();
            g_scripts.push_back(std::move(se));
        }
    };
    // Global first so they sort to the top of each section.
    scan(g_global_scripts_dir,             false, true);
    scan(g_global_scripts_dir / "autorun", true,  true);
    scan(g_scripts_dir,                    false, false);
    scan(g_scripts_dir / "autorun",        true,  false);
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(content.data(), content.size());
    return true;
}

// ---------------------------------------------------------------------------
// Document management
// ---------------------------------------------------------------------------
int new_document(const std::string& initial_text, const std::string& path = "") {
    Document d;
    d.editor = new TextEditor();
    d.editor->SetLanguage(TextEditor::Language::Lua());
    d.editor->SetTabSize(2);
    d.editor->SetShowWhitespacesEnabled(g_show_whitespace);
    d.editor->SetText(initial_text);
    d.path = path;
    d.display_name = path.empty()
        ? ("untitled" + std::to_string(g_untitled_counter++) + ".lua")
        : fs::path(path).filename().string();
    d.saved_undo_index = d.editor->GetUndoIndex();
    g_docs.push_back(std::move(d));
    const int idx = (int)g_docs.size() - 1;
    g_active_doc = idx;
    g_pending_select_doc = idx;
    return idx;
}

void open_document(const fs::path& p) {
    const std::string ps = p.string();
    for (int i = 0; i < (int)g_docs.size(); ++i) {
        if (g_docs[i].path == ps) {
            g_active_doc = i;
            g_pending_select_doc = i;
            return;
        }
    }
    auto txt = read_file(p);
    new_document(txt, ps);
    g_status = "loaded " + p.filename().string() + " (" + std::to_string(txt.size()) + " bytes)";
    log(g_status);
}

void close_document(int idx) {
    if (idx < 0 || idx >= (int)g_docs.size()) return;
    delete g_docs[idx].editor;
    g_docs.erase(g_docs.begin() + idx);
    if (g_docs.empty()) g_active_doc = -1;
    else if (g_active_doc >= (int)g_docs.size()) g_active_doc = (int)g_docs.size() - 1;
}

bool save_current(bool save_as) {
    if (g_active_doc < 0) return false;
    auto& doc = g_docs[g_active_doc];

    fs::path target;
    if (save_as || doc.path.empty()) {
        fs::path root = g_save_to_global ? g_global_scripts_dir : g_scripts_dir;
        if (g_autorun_target) root /= "autorun";
        target = root / g_filename_buf;
        if (target.extension() != ".lua") target += ".lua";
    } else {
        target = fs::path(doc.path);
    }

    if (!write_file(target, doc.editor->GetText())) {
        g_status = "save FAILED: " + target.string();
        log(g_status);
        return false;
    }
    doc.path = target.string();
    doc.display_name = target.filename().string();
    doc.saved_undo_index = doc.editor->GetUndoIndex();
    g_status = "saved " + target.filename().string();
    log(g_status);
    rescan_scripts();
    return true;
}

void insert_at_cursor(const std::string& text) {
    if (g_active_doc < 0) return;
    auto& doc = g_docs[g_active_doc];
    auto pos = doc.editor->GetMainCursorPosition();
    doc.editor->ReplaceSectionText(pos.line, pos.column, pos.line, pos.column, text);
}

// ---------------------------------------------------------------------------
// Run buffer, with print() output captured into the Output pane.
//
// The whole thing (print-shadow install/restore + the user's code + output
// formatting) runs as ONE exec_lua_chunk call, so there's no window between
// separate calls where some other thread's unrelated print() could land in
// our capture buffer. The user's raw source is embedded via a dynamically
// sized Lua long-bracket literal ([==[ ... ]==], level chosen so the closing
// sequence can't collide with anything already in their code) — this needs
// zero escaping, unlike wrapping their text in a quoted "..." string.
// ---------------------------------------------------------------------------
std::string lua_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    out += "\"";
    return out;
}

std::string lua_long_bracket(const std::string& code) {
    int level = 0;
    for (; level < 20; ++level) {
        const std::string closer = "]" + std::string(level, '=') + "]";
        if (code.find(closer) == std::string::npos) break;
    }
    const std::string opener = "[" + std::string(level, '=') + "[";
    const std::string closer = "]" + std::string(level, '=') + "]";
    // A leading newline right after the opening bracket is swallowed by Lua's
    // long-bracket rule, but insert one deliberately anyway: it guards against
    // the (impossible here, since opener ends in `[`, but kept for clarity)
    // case of the very first user characters altering how the bracket parses.
    return opener + "\n" + code + closer;
}

void run_buffer() {
    if (g_active_doc < 0) { g_status = "no active document"; return; }
    auto fns = API::get()->param()->functions;
    if (fns->exec_lua_chunk == nullptr) {
        g_status = "exec_lua_chunk unavailable — rebuild UEVRBackend";
        log(g_status);
        return;
    }

    auto& doc = g_docs[g_active_doc];
    const std::string code = doc.editor->GetText();
    const std::string label = doc.path.empty() ? doc.display_name : doc.path;

    std::string wrapper;
    wrapper += "local __orig_print = print\n";
    wrapper += "local __buf = {}\n";
    wrapper += "print = function(...)\n";
    wrapper += "    local n = select('#', ...)\n";
    wrapper += "    local parts = {}\n";
    wrapper += "    for i = 1, n do parts[i] = tostring(select(i, ...)) end\n";
    wrapper += "    __buf[#__buf + 1] = table.concat(parts, '\\t')\n";
    wrapper += "end\n";
    wrapper += "local __chunk, __load_err = load(" + lua_long_bracket(code) + ", "
             + lua_quote(label) + ", 't')\n";
    wrapper += "local __ok, __ret\n";
    wrapper += "if __chunk then __ok, __ret = pcall(__chunk) else __ok, __ret = false, __load_err end\n";
    wrapper += "print = __orig_print\n";
    wrapper += "local __out = table.concat(__buf, '\\n')\n";
    wrapper += "if not __ok then\n";
    wrapper += "    return __out .. (#__out > 0 and '\\n' or '') .. '!! ' .. tostring(__ret)\n";
    wrapper += "end\n";
    wrapper += "if __ret ~= nil then\n";
    wrapper += "    return __out .. (#__out > 0 and '\\n' or '') .. '= ' .. tostring(__ret)\n";
    wrapper += "end\n";
    wrapper += "return __out\n";

    char out[4096] = {};
    const bool ok = fns->exec_lua_chunk(wrapper.c_str(), (label + ".wrapper").c_str(), out, sizeof(out));

    g_status = ok ? "run OK" : "run FAILED";
    log("== run " + label + " (" + std::to_string(code.size()) + " bytes) ==");
    if (out[0]) log(out);
    else if (!ok) log("!! wrapper execution failed (see log.txt for the C++-side error)");
}

void reset_lua_scripts() {
    auto fns = API::get()->param()->functions;
    if (fns->reset_lua_scripts == nullptr) {
        g_status = "reset_lua_scripts unavailable";
        log(g_status);
        return;
    }
    // Safe to call directly from here: the PluginLoader-side shim now goes
    // through LuaLoader's deferred request_script_reset() flag instead of
    // tearing the lua_State down synchronously (that used to crash — the
    // teardown could run while another thread was mid-execution of a lua
    // callback against the same state).
    fns->reset_lua_scripts();
    log("reset_lua_scripts requested — applies at the top of next frame");
}

// Lua-side dump chunks. Both return a single big string; the C++ side just
// writes it into the per-tab buffer for paint.
constexpr const char* DUMP_GLOBALS_LUA = R"LUA(
local _ok, _result = pcall(function()
local out = {}
local builtin = {
    _G=true, _VERSION=true, assert=true, collectgarbage=true, dofile=true,
    error=true, getmetatable=true, ipairs=true, load=true, loadfile=true,
    next=true, pairs=true, pcall=true, print=true, rawequal=true, rawget=true,
    rawlen=true, rawset=true, require=true, select=true, setmetatable=true,
    tonumber=true, tostring=true, type=true, unpack=true, xpcall=true,
    coroutine=true, debug=true, io=true, math=true, os=true, package=true,
    string=true, table=true, utf8=true, bit32=true,
}
local function fmt(v)
    local t = type(v)
    if t == 'string'  then
        local s = v:gsub('\n', '\\n'):gsub('\r', '\\r')
        if #s > 60 then s = s:sub(1,60)..'..' end
        return string.format('%q', s)
    end
    if t == 'number'  then return tostring(v) end
    if t == 'boolean' then return tostring(v) end
    if t == 'nil'     then return 'nil' end
    if t == 'function' then
        local info = debug.getinfo(v, 'S')
        if info and info.short_src then
            return string.format('function@%s:%d', info.short_src, info.linedefined or 0)
        end
        return 'function'
    end
    if t == 'table'   then
        local okc, n = pcall(function()
            local c=0; for _ in pairs(v) do c=c+1; if c>9999 then break end end; return c
        end)
        if okc then return string.format('table[#=%d]', n) end
        return 'table[?]'
    end
    return t
end
local keys = {}
for k in pairs(_G) do
    local ks = tostring(k)
    if not builtin[ks] then keys[#keys+1] = ks end
end
table.sort(keys)
table.insert(out, string.format('-- %d user globals (of %d total)', #keys,
    (function() local n=0; for _ in pairs(_G) do n=n+1 end; return n end)()))
for _, k in ipairs(keys) do
    local row_ok, row = pcall(function()
        local ok, v = pcall(function() return _G[k] end)
        if not ok then return string.format('%-32s %-12s %s', k, '?', '<unreadable>') end
        return string.format('%-32s %-12s %s', k, type(v), fmt(v))
    end)
    out[#out+1] = row_ok and row or string.format('%-32s %-12s %s', k, '?', '<error>')
end
return table.concat(out, '\n')
end)
if not _ok then return '!! globals dump error: '..tostring(_result) end
return _result
)LUA";

constexpr const char* DUMP_MODULES_LUA = R"LUA(
local _ok, _result = pcall(function()
local out = {}
local loaded = package and package.loaded or {}
local keys = {}
for k in pairs(loaded) do keys[#keys+1] = tostring(k) end
table.sort(keys)
table.insert(out, string.format('-- %d entries in package.loaded', #keys))
for _, k in ipairs(keys) do
    local v = loaded[k]
    local t = type(v)
    local hint = ''
    if t == 'table' then
        local n=0; for _ in pairs(v) do n=n+1 end
        hint = string.format('[#=%d]', n)
    end
    out[#out+1] = string.format('%-48s %s%s', k, t, hint)
end
if package and package.path then
    out[#out+1] = ''
    out[#out+1] = '-- package.path:'
    for p in package.path:gmatch('[^;]+') do out[#out+1] = '  '..p end
end
return table.concat(out, '\n')
end)
if not _ok then return '!! modules dump error: '..tostring(_result) end
return _result
)LUA";

void refresh_globals() {
    auto fns = API::get()->param()->functions;
    if (fns->exec_lua_chunk == nullptr) {
        std::snprintf(g_globals_buf.data(), g_globals_buf.size(),
            "exec_lua_chunk unavailable — rebuild UEVRBackend");
        return;
    }
    fns->exec_lua_chunk(DUMP_GLOBALS_LUA, "lua_editor.dump_globals",
        g_globals_buf.data(), (unsigned)g_globals_buf.size());
}

void refresh_modules() {
    auto fns = API::get()->param()->functions;
    if (fns->exec_lua_chunk == nullptr) {
        std::snprintf(g_modules_buf.data(), g_modules_buf.size(),
            "exec_lua_chunk unavailable");
        return;
    }
    fns->exec_lua_chunk(DUMP_MODULES_LUA, "lua_editor.dump_modules",
        g_modules_buf.data(), (unsigned)g_modules_buf.size());
}

// ---------------------------------------------------------------------------
// Tree queries. Each chunk prints one child per line:
//   display \31 suffix \31 type \31 preview \31 expandable(0/1)
// \31 (unit separator) can't collide: previews strip control chars Lua-side.
// The shared emitter is wrapped around a target expression; every risky step
// (index, pairs, tostring) is pcall-guarded so one poison value can't kill the
// query (sol containers like glm matrices throw from __pairs).
//
// Metatable walk climbs the __index chain (not just one level): sol2 usually
// flattens sol::bases<...> into a single metatable, but this defends against
// any binding pattern that instead chains a second/third table, and against
// future sol2 changes — capped depth + cycle guard via a `seen` table of
// already-visited metatable identities.
// ---------------------------------------------------------------------------
constexpr const char* TREE_EMIT_PRELUDE = R"LUA(
local SEP = string.char(31)
local function clean(s)
    s = tostring(s):gsub('%c', ' ')
    if #s > 96 then s = s:sub(1, 96) .. '..' end
    return s
end
local function mt_name(x)
    local nm
    pcall(function()
        local mt = getmetatable(x)
        if type(mt) == 'table' and mt.__name then nm = tostring(mt.__name) end
    end)
    return nm
end
local function preview(x)
    local t = type(x)
    if t == 'string'   then return clean(string.format('%q', x)) end
    if t == 'number' or t == 'boolean' then return tostring(x) end
    if t == 'nil'      then return 'nil' end
    if t == 'function' then
        local i = debug.getinfo(x, 'S')
        if i and i.short_src and i.what ~= 'C' then
            return string.format('fn@%s:%d', i.short_src, i.linedefined or 0)
        end
        return 'function(C)'
    end
    if t == 'table' then
        local ok, n = pcall(function()
            local c = 0; for _ in pairs(x) do c = c + 1; if c > 999 then break end end; return c
        end)
        local nm = mt_name(x)
        return (nm and (nm .. ' ') or '') .. (ok and ('#' .. n) or '#?')
    end
    if t == 'userdata' then
        -- sol2 usertype instance: show bound C++ type name + tostring
        local ok, s = pcall(function() return tostring(x) end)
        local nm = mt_name(x)
        return clean((nm and (nm .. ': ') or 'userdata: ') .. (ok and s or '?'))
    end
    local ok, s = pcall(clean, x)
    return ok and s or t
end
-- push one child row, deduped by key. `tag` annotates provenance (e.g. meta).
local function add_entry(entries, seen, k, cv, tag)
    local ks = tostring(k)
    if seen[ks] then return end
    seen[ks] = true
    local kt = type(k)
    local suffix = (kt == 'string' and string.format('[%q]', k))
                or (kt == 'number' and string.format('[%s]', tostring(k))) or ''
    local vt = type(cv)
    local okp, pv = pcall(preview, cv)
    -- tables AND userdata (usertype instances) can be drilled further
    local expandable = ((vt == 'table' or vt == 'userdata') and suffix ~= '') and '1' or '0'
    entries[#entries + 1] = {
        d = clean(k) .. (tag or ''), s = suffix, t = vt, p = okp and pv or '?', e = expandable,
    }
end
-- Enumerate everything reachable on v: raw pairs (tables) PLUS the metatable
-- chain. sol2 stores usertype methods AS STRING KEYS in the metatable
-- (metamethods are __-prefixed); walking __index (and, defensively, any
-- further __index chain beyond it) is what surfaces bound C++ functions that
-- pairs(v) alone never sees.
local function emit_children(v, rows)
    local entries = {}
    local seen = {}
    local cnt = 0
    local LIMIT = 800
    local function bump() cnt = cnt + 1; return cnt <= LIMIT end

    if type(v) == 'table' then
        pcall(function()
            for k, cv in pairs(v) do
                if not bump() then return end
                add_entry(entries, seen, k, cv)
            end
        end)
    end
    pcall(function()
        local seen_mt = {}
        local mt = getmetatable(v)
        local depth = 0
        while type(mt) == 'table' and not seen_mt[mt] and depth < 12 do
            seen_mt[mt] = true
            depth = depth + 1
            for k, cv in pairs(mt) do
                if not bump() then return end
                local ks = tostring(k)
                if ks ~= '__index' then
                    add_entry(entries, seen, k, cv, ks:sub(1, 2) == '__' and '  (meta)' or '  (method)')
                end
            end
            local idx = mt.__index
            if type(idx) == 'table' and idx ~= mt and idx ~= v and not seen_mt[idx] then
                for k, cv in pairs(idx) do
                    if not bump() then return end
                    add_entry(entries, seen, k, cv, '  (member)')
                end
                seen_mt[idx] = true
                mt = getmetatable(idx)
            else
                mt = nil
            end
        end
    end)
    if cnt > LIMIT then
        entries[#entries + 1] = { d = '(truncated at ' .. LIMIT .. ')', s = '', t = '', p = '', e = '0' }
    end
    if #entries == 0 then
        entries[#entries + 1] = { d = '(no enumerable members)', s = '', t = '', p = '', e = '0' }
    end
    table.sort(entries, function(a, b) return a.d < b.d end)
    for _, en in ipairs(entries) do
        rows[#rows + 1] = table.concat({ en.d, en.s, en.t, en.p, en.e }, SEP)
    end
end
)LUA";

// Fetch children of an arbitrary path expression.
std::string build_children_chunk(const std::string& expr) {
    std::string chunk;
    chunk += "local _ok, _res = pcall(function()\n";
    chunk += TREE_EMIT_PRELUDE;
    chunk += "local ok_get, v = pcall(function() return " + expr + " end)\n";
    chunk += "if not ok_get then return '(unreadable: ' .. clean(v) .. ')' end\n";
    chunk += "if type(v) ~= 'table' and type(v) ~= 'userdata' then return '(not indexable: ' .. type(v) .. ')' end\n";
    chunk += "local rows = {}\n";
    chunk += "emit_children(v, rows)\n";
    chunk += "return table.concat(rows, '\\n')\n";
    chunk += "end)\n";
    chunk += "if not _ok then return '(query error: ' .. tostring(_res) .. ')' end\n";
    chunk += "return _res\n";
    return chunk;
}

// Header line for an arbitrary expression: "type  preview".
std::string build_eval_header_chunk(const std::string& expr) {
    std::string chunk;
    chunk += "local _ok, _res = pcall(function()\n";
    chunk += TREE_EMIT_PRELUDE;
    chunk += "local ok_get, v = pcall(function() return " + expr + " end)\n";
    chunk += "if not ok_get then return '(unreadable: ' .. clean(v) .. ')' end\n";
    chunk += "return type(v) .. '  ' .. preview(v)\n";
    chunk += "end)\n";
    chunk += "if not _ok then return '(eval error: ' .. tostring(_res) .. ')' end\n";
    chunk += "return _res\n";
    return chunk;
}

// Roots: user globals (builtins skipped) / package.loaded.
constexpr const char* TREE_GLOBALS_ROOTS = R"LUA(
local _ok, _res = pcall(function()
local builtin = {
    _G=true, _VERSION=true, assert=true, collectgarbage=true, dofile=true,
    error=true, getmetatable=true, ipairs=true, load=true, loadfile=true,
    next=true, pairs=true, pcall=true, print=true, rawequal=true, rawget=true,
    rawlen=true, rawset=true, require=true, select=true, setmetatable=true,
    tonumber=true, tostring=true, type=true, unpack=true, xpcall=true,
    coroutine=true, debug=true, io=true, math=true, os=true, package=true,
    string=true, table=true, utf8=true, bit32=true,
}
)LUA";

std::string build_roots_chunk(bool globals) {
    std::string chunk;
    if (globals) {
        chunk += TREE_GLOBALS_ROOTS;         // opens pcall + builtin set
        chunk += TREE_EMIT_PRELUDE;
        // filtered copy of _G so emit_children only sees user globals
        chunk += "local v = {}\n";
        chunk += "for k, cv in pairs(_G) do if not builtin[tostring(k)] then v[k] = cv end end\n";
    } else {
        chunk += "local _ok, _res = pcall(function()\n";
        chunk += TREE_EMIT_PRELUDE;
        chunk += "local v = package and package.loaded or {}\n";
    }
    chunk += "local rows = {}\n";
    chunk += "emit_children(v, rows)\n";
    chunk += "return table.concat(rows, '\\n')\n";
    chunk += "end)\n";
    chunk += "if not _ok then return '(query error: ' .. tostring(_res) .. ')' end\n";
    chunk += "return _res\n";
    return chunk;
}

// Parse \31-separated rows into children of `parent`. Root-relative exprs are
// built as parent.expr + suffix; suffix-less rows are informational leaves.
void parse_rows_into(LuaNode& parent, const char* buf) {
    parent.children.clear();
    parent.loaded = true;
    const char* p = buf;
    while (p && *p) {
        const char* nl = std::strchr(p, '\n');
        std::string line = nl ? std::string(p, nl) : std::string(p);
        p = nl ? nl + 1 : nullptr;
        if (line.empty()) continue;

        LuaNode n;
        std::string fields[5];
        size_t start = 0;
        for (int f = 0; f < 5; ++f) {
            size_t sep = line.find('\x1f', start);
            if (sep == std::string::npos) { fields[f] = line.substr(start); start = line.size(); }
            else { fields[f] = line.substr(start, sep - start); start = sep + 1; }
        }
        n.display = fields[0];
        n.suffix  = fields[1];
        n.type    = fields[2];
        n.preview = fields[3];
        n.expandable = (fields[4] == "1");
        n.expr = n.suffix.empty() ? std::string() : parent.expr + n.suffix;
        parent.children.push_back(std::move(n));
    }
}

void load_children(LuaNode& n) {
    auto fns = API::get()->param()->functions;
    if (fns->exec_lua_chunk == nullptr || n.expr.empty()) { n.loaded = true; return; }
    g_tree_buf[0] = 0;
    const auto chunk = build_children_chunk(n.expr);
    fns->exec_lua_chunk(chunk.c_str(), "lua_editor.tree_children",
        g_tree_buf.data(), (unsigned)g_tree_buf.size());
    parse_rows_into(n, g_tree_buf.data());
}

void refresh_tree(bool globals) {
    auto fns = API::get()->param()->functions;
    LuaNode& root = globals ? g_globals_tree : g_modules_tree;
    root.expr = globals ? "_G" : "package.loaded";
    if (fns->exec_lua_chunk == nullptr) { root.children.clear(); root.loaded = true; return; }
    g_tree_buf[0] = 0;
    const auto chunk = build_roots_chunk(globals);
    fns->exec_lua_chunk(chunk.c_str(),
        globals ? "lua_editor.tree_globals" : "lua_editor.tree_modules",
        g_tree_buf.data(), (unsigned)g_tree_buf.size());
    parse_rows_into(root, g_tree_buf.data());
    (globals ? g_globals_tree_ready : g_modules_tree_ready) = true;
}

// Mark every loaded node stale (keeps imgui's open state; visible-open nodes
// lazily re-query next frame, budget-limited — see g_loads_this_frame).
void invalidate_tree(LuaNode& n) {
    n.loaded = false;
    for (auto& c : n.children) invalidate_tree(c);
}

void inspect_eval() {
    auto fns = API::get()->param()->functions;
    if (fns->exec_lua_chunk == nullptr) {
        g_inspect_header = "exec_lua_chunk unavailable — rebuild UEVRBackend";
        g_inspect_has_result = true;
        return;
    }
    g_inspect_node.expr = g_inspect_expr;
    load_children(g_inspect_node);

    char buf[512] = {};
    const auto hchunk = build_eval_header_chunk(g_inspect_expr);
    fns->exec_lua_chunk(hchunk.c_str(), "lua_editor.inspect_header", buf, sizeof(buf));
    g_inspect_header = buf;
    g_inspect_has_result = true;
}

void refresh_watch(int idx) {
    if (idx < 0 || idx >= (int)g_watches.size()) return;
    auto fns = API::get()->param()->functions;
    if (fns->exec_lua_chunk == nullptr) return;
    auto& w = g_watches[idx];
    const auto chunk = build_eval_header_chunk(w.expr);
    char buf[512] = {};
    fns->exec_lua_chunk(chunk.c_str(), "lua_editor.watch", buf, sizeof(buf));
    const std::string line(buf);
    const auto sp = line.find("  ");
    if (sp != std::string::npos) { w.type = line.substr(0, sp); w.preview = line.substr(sp + 2); }
    else { w.type.clear(); w.preview = line; }
}

void draw_toolbar() {
    if (ImGui::Button("New")) new_document("-- new script\n");
    ImGui::SameLine();

    if (ImGui::Button("Save"))    save_current(false);
    ImGui::SameLine();
    if (ImGui::Button("Save As")) save_current(true);
    ImGui::SameLine();
    if (ImGui::Button("Reload list")) rescan_scripts();
    ImGui::SameLine();
    ImGui::Separator();

    if (ImGui::Button("Run (Ctrl+R)")) run_buffer();
    ImGui::SameLine();
    if (ImGui::Button("Reset scripts")) reset_lua_scripts();
    ImGui::SameLine();
    ImGui::Checkbox("autorun", &g_autorun_target);
    ImGui::SameLine();
    ImGui::Checkbox("save to global", &g_save_to_global);
    ImGui::SameLine();
    if (ImGui::Checkbox("show whitespace", &g_show_whitespace)) {
        for (auto& d : g_docs) d.editor->SetShowWhitespacesEnabled(g_show_whitespace);
    }

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("filename", g_filename_buf, sizeof(g_filename_buf));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s", g_status.c_str());
}

void draw_file_pane() {
    ImGui::TextDisabled("scripts/  (%zu)", g_scripts.size());
    ImGui::Separator();

    if (ImGui::BeginChild("##files", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int i = 0; i < (int)g_scripts.size(); ++i) {
            const bool selected = (i == g_selected);
            if (ImGui::Selectable(g_scripts[i].name.c_str(), selected)) {
                g_selected = i;
                open_document(g_scripts[i].path);
            }
            if (selected && ImGui::IsItemHovered() && ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Open")) open_document(g_scripts[i].path);
                if (ImGui::MenuItem("Delete")) {
                    std::error_code ec;
                    fs::remove(g_scripts[i].path, ec);
                    if (!ec) {
                        log("deleted " + g_scripts[i].path.filename().string());
                        rescan_scripts();
                        g_selected = -1;
                    }
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();
}

void draw_log_inner() {
    if (ImGui::Button("Clear log")) g_log.clear();
    ImGui::SameLine();
    ImGui::Text("(%zu lines)", g_log.size());
    ImGui::Separator();

    if (ImGui::BeginChild("##log", ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar)) {
        for (auto& line : g_log) {
            if (!line.empty() && line[0] == '!') {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", line.c_str());
            } else if (line.rfind("==", 0) == 0) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", line.c_str());
            } else {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
}

// Property-editor style row. Expandables are tree nodes; expansion lazily
// queries the lua state, budget-limited per frame (see g_loads_this_frame).
void render_lua_node(LuaNode& n) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    if (n.expandable && !n.expr.empty()) {
        ImGui::PushID(n.suffix.c_str());
        const bool open = ImGui::TreeNodeEx(n.display.c_str(),
            ImGuiTreeNodeFlags_SpanFullWidth);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", n.type.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(n.preview.c_str());
        if (open) {
            if (!n.loaded && g_loads_this_frame < MAX_LOADS_PER_FRAME) {
                load_children(n);
                ++g_loads_this_frame;
            }
            for (auto& c : n.children) render_lua_node(c);
            ImGui::TreePop();
        }
        ImGui::PopID();
    } else {
        ImGui::TreeNodeEx(n.display.c_str(),
            ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
            ImGuiTreeNodeFlags_SpanFullWidth);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", n.type.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(n.preview.c_str());
        // right-click a leaf to copy its value preview
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::SetClipboardText(n.preview.c_str());
        }
    }
}

void draw_tree_inner(bool globals, std::vector<char>& copy_buf, bool& autorefresh,
                     void (*copy_refresh)()) {
    LuaNode& root = globals ? g_globals_tree : g_modules_tree;
    bool& ready = globals ? g_globals_tree_ready : g_modules_tree_ready;

    if (ImGui::Button("Refresh")) { refresh_tree(globals); invalidate_tree(root); root.loaded = true; }
    ImGui::SameLine();
    ImGui::Checkbox("auto (60f)", &autorefresh);
    // Staggered phase (globals on :00, modules on :30) so both autorefreshes
    // never land in the same frame and double the per-frame query burst.
    if (autorefresh && (g_dump_tick % 60) == (globals ? 0u : 30u)) { refresh_tree(globals); }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) { copy_refresh(); ImGui::SetClipboardText(copy_buf.data()); }
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu roots; expand to query nested tables live)", root.children.size());
    ImGui::Separator();

    if (!ready) refresh_tree(globals);

    if (ImGui::BeginTable(globals ? "##globals_tbl" : "##modules_tbl", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("key",   ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("type",  ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableHeadersRow();
        for (auto& c : root.children) render_lua_node(c);
        ImGui::EndTable();
    }
}

void draw_inspect_inner() {
    ImGui::TextDisabled("Type any Lua expression reaching from a global, e.g. uevr.vr, _G.APIUE, find_fast('Pawn').");
    ImGui::SetNextItemWidth(420.0f);
    const bool enter = ImGui::InputText("##inspect_expr", g_inspect_expr, sizeof(g_inspect_expr),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Eval") || enter) inspect_eval();
    ImGui::SameLine();
    if (ImGui::Button("Pin as watch") && g_inspect_expr[0] != '\0') {
        g_watches.push_back(WatchEntry{ g_inspect_expr, "", "" });
        refresh_watch((int)g_watches.size() - 1);
    }

    if (g_inspect_has_result) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", g_inspect_header.c_str());
        if (ImGui::BeginTable("##inspect_tbl", 4,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp, ImVec2(0, 220))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("member", ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("type",   ImGuiTableColumnFlags_WidthStretch, 0.15f);
            ImGui::TableSetupColumn("value",  ImGuiTableColumnFlags_WidthStretch, 0.35f);
            ImGui::TableSetupColumn("##ins",  ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableHeadersRow();
            for (auto& c : g_inspect_node.children) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.display.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", c.type.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.preview.c_str());
                ImGui::TableNextColumn();
                if (!c.suffix.empty()) {
                    ImGui::PushID(&c);
                    if (ImGui::SmallButton("ins")) {
                        insert_at_cursor(std::string(g_inspect_expr) + c.suffix);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    ImGui::Text("Watches (%zu)", g_watches.size());
    ImGui::SameLine();
    ImGui::Checkbox("auto (30f)", &g_watch_autorefresh);
    if (g_watch_autorefresh && (g_dump_tick % 30) == 0) {
        for (int i = 0; i < (int)g_watches.size(); ++i) refresh_watch(i);
    }

    int remove_idx = -1;
    if (ImGui::BeginTable("##watch_tbl", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 140))) {
        ImGui::TableSetupColumn("expr",  ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("type",  ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        ImGui::TableSetupColumn("##x",   ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)g_watches.size(); ++i) {
            auto& w = g_watches[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(w.expr.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", w.type.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(w.preview.c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::SmallButton("x")) remove_idx = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove_idx >= 0) g_watches.erase(g_watches.begin() + remove_idx);
}

void draw_bottom_pane() {
    if (ImGui::BeginTabBar("##bottom_tabs")) {
        if (ImGui::BeginTabItem("Output")) {
            g_bottom_tab = BottomTab::Output;
            draw_log_inner();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Globals")) {
            g_bottom_tab = BottomTab::Globals;
            draw_tree_inner(true, g_globals_buf, g_globals_autorefresh, refresh_globals);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Modules")) {
            g_bottom_tab = BottomTab::Modules;
            draw_tree_inner(false, g_modules_buf, g_modules_autorefresh, refresh_modules);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Inspect")) {
            g_bottom_tab = BottomTab::Inspect;
            draw_inspect_inner();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// Draggable splitter. Vertical=true separates left/right (resizes width);
// otherwise top/bottom (resizes height). Uses an InvisibleButton so it works
// inside any layout; hover shows the resize cursor.
void splitter(const char* id, bool vertical, float* value, float min_v, float max_v) {
    ImGui::PushID(id);
    const float thickness = 6.0f;
    ImGui::InvisibleButton("##split",
        vertical ? ImVec2(thickness, -1.0f) : ImVec2(-1.0f, thickness));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive()) {
        const float d = vertical ? ImGui::GetIO().MouseDelta.x : -ImGui::GetIO().MouseDelta.y;
        *value = *value + d;
        if (*value < min_v) *value = min_v;
        if (*value > max_v) *value = max_v;
    }
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(mn, mx,
        ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive :
            ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator));
    ImGui::PopID();
}

void hotkeys() {
    const auto& io = ImGui::GetIO();
    if (!io.KeyCtrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) save_current(false);
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) run_buffer();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) run_buffer();
}

void draw_doc_tabs() {
    if (g_docs.empty()) return;
    int close_idx = -1;
    if (ImGui::BeginTabBar("##doc_tabs",
            ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < (int)g_docs.size(); ++i) {
            auto& doc = g_docs[i];
            bool open = true;
            ImGui::PushID(i);
            const std::string label = doc.display_name + (doc_modified(doc) ? " *" : "") + "###tab";
            ImGuiTabItemFlags flags = 0;
            if (i == g_pending_select_doc) flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                g_active_doc = i;
                ImGui::EndTabItem();
            }
            ImGui::PopID();
            if (!open) close_idx = i;
        }
        ImGui::EndTabBar();
    }
    g_pending_select_doc = -1;
    if (close_idx >= 0) close_document(close_idx);
}

void render(UEVR_ImGuiFrameCbData* data) {
    if (data == nullptr) return;

    auto& api = *API::get();
    const auto fns = api.param()->functions;

    // Only draw while UEVR's own menu is up: is_drawing_ui() is exactly the
    // flag Framework's input-eating logic checks (Framework::is_drawing_ui()
    // == m_draw_ui), so tying our visibility (and therefore our desire for
    // keyboard/mouse focus) to it means our window is never on-screen wanting
    // input at a moment the host has decided input belongs to the game. It
    // also means the SAME hotkey/menu action that opens UEVR's own UI brings
    // this panel back — no more "reload plugins to get the window back".
    if (fns->is_drawing_ui != nullptr && !fns->is_drawing_ui()) return;
    if (!g_window_open) return;

    g_loads_this_frame = 0;

    // Rebind to UEVR's ImGui context every frame. Our DLL has its own
    // ImGuiContext* TLS slot — pointing it at the host's context makes every
    // ImGui:: call we make go through host data (windows list, draw lists,
    // etc.). The allocator hand-off avoids cross-heap free.
    ImGui::SetCurrentContext((ImGuiContext*)data->context);
    ImGui::SetAllocatorFunctions(
        (ImGuiMemAllocFunc)data->malloc_fn,
        (ImGuiMemFreeFunc)data->free_fn,
        data->user_data);

    // Defends against ABI drift between this plugin's imgui sources and the
    // ones UEVR compiled. The macro asserts on size mismatches but our
    // imconfig sets IM_ASSERT to (void)0, so the call is harmless when
    // matched and gives us a hard NULL deref tracepoint when not.
    static bool s_version_logged = false;
    if (!s_version_logged) {
        s_version_logged = true;
        IMGUI_CHECKVERSION();
        api.log_info("%s ImGui ABI ok: %s (sizeof style=%zu, io=%zu)",
            TAG, ImGui::GetVersion(),
            sizeof(ImGuiStyle), sizeof(ImGuiIO));
    }

    if (g_docs.empty()) {
        new_document(
            "-- UEVR lua editor\n"
            "-- press Ctrl+R or 'Run' to execute in UEVR's main lua state.\n"
            "print('hello from editor, _VERSION = ' .. tostring(_VERSION))\n"
            "return 1 + 1\n");
        g_status = "ready";
        rescan_scripts();
    }

    ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UEVR Lua Editor", &g_window_open,
            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New"))               new_document("-- new script\n");
            if (ImGui::MenuItem("Save", "Ctrl+S"))    save_current(false);
            if (ImGui::MenuItem("Save As"))           save_current(true);
            if (ImGui::MenuItem("Close tab") && g_active_doc >= 0) close_document(g_active_doc);
            if (ImGui::MenuItem("Rescan list"))       rescan_scripts();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Run")) {
            if (ImGui::MenuItem("Run buffer", "Ctrl+R"))    run_buffer();
            if (ImGui::MenuItem("Reset scripts"))           reset_lua_scripts();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Show whitespace", nullptr, &g_show_whitespace)) {
                for (auto& d : g_docs) d.editor->SetShowWhitespacesEnabled(g_show_whitespace);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    draw_toolbar();
    ImGui::Separator();

    const auto avail = ImGui::GetContentRegionAvail();

    // clamp the draggable sizes to the window
    const float splitter_px = 6.0f;
    if (g_bottom_h > avail.y - 80.0f) g_bottom_h = avail.y - 80.0f;
    if (g_bottom_h < 40.0f)           g_bottom_h = 40.0f;
    if (g_left_w   > avail.x - 200.0f) g_left_w = avail.x - 200.0f;
    if (g_left_w   < 80.0f)            g_left_w = 80.0f;
    const float top_h = avail.y - g_bottom_h - splitter_px;

    // Left: file list
    ImGui::BeginChild("##left", ImVec2(g_left_w, top_h), true);
    draw_file_pane();
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);

    // vertical splitter: file list | editor
    ImGui::BeginChild("##vsplit_host", ImVec2(splitter_px, top_h), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    splitter("vsplit", true, &g_left_w, 80.0f, avail.x - 200.0f);
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);

    // Right: document tabs + editor
    ImGui::BeginChild("##editor", ImVec2(0, top_h), true);
    draw_doc_tabs();
    if (g_active_doc >= 0 && g_active_doc < (int)g_docs.size()) {
        g_docs[g_active_doc].editor->Render("##textedit", ImVec2(-1, -1), false);
    } else {
        ImGui::TextDisabled("(no document open — click New or a file on the left)");
    }
    ImGui::EndChild();

    // horizontal splitter: editor row / bottom pane (drag up = grow bottom)
    splitter("hsplit", false, &g_bottom_h, 40.0f, avail.y - 80.0f);

    // Bottom: tabbed pane (Output / Globals / Modules / Inspect)
    ImGui::BeginChild("##bottom", ImVec2(0, g_bottom_h), true);
    draw_bottom_pane();
    ImGui::EndChild();
    ++g_dump_tick;

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) hotkeys();

    ImGui::End();
}

} // namespace

class LuaScriptEditorPlugin : public Plugin {
public:
    void on_initialize() override {
        auto& api = *API::get();
        const auto fns = api.param()->functions;

        api.log_info("%s init. branch=%s commit=%s", TAG,
            fns->get_branch(), fns->get_commit_hash());

        // Per-game scripts: under UEVR's get_persistent_dir().
        wchar_t buf[512] = {};
        if (fns->get_persistent_dir) {
            fns->get_persistent_dir(buf, 512);
            g_scripts_dir = fs::path(buf) / L"scripts";
        }
        // Global scripts: %APPDATA%\UnrealVRMod\UEVR\scripts (loaded for every game).
        if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata != nullptr) {
            g_global_scripts_dir = fs::path(appdata) / L"UnrealVRMod" / L"UEVR" / L"scripts";
        }
        api.log_info("%s scripts dir       : %s", TAG, g_scripts_dir.string().c_str());
        api.log_info("%s global scripts dir: %s", TAG, g_global_scripts_dir.string().c_str());

        if (fns->is_drawing_ui == nullptr) {
            api.log_warn("%s is_drawing_ui NULL — window will render unconditionally "
                "(older UEVRBackend.dll); rebuild to fix input leaking to the game", TAG);
        }

        if (fns->on_imgui_frame == nullptr) {
            api.log_warn("%s on_imgui_frame NULL — rebuild UEVRBackend.dll", TAG);
            return;
        }

        fns->on_imgui_frame([](UEVR_ImGuiFrameCbData* d) {
            try { render(d); } catch (...) {
                static bool once = false;
                if (!once) { once = true; API::get()->log_error("%s render exception", TAG); }
            }
        });

        if (fns->on_lua_state_destroyed) {
            fns->on_lua_state_destroyed([](lua_State*) {
                log("[host] lua state destroyed");
                // Trees/inspect/watches point at expressions in the OLD state;
                // mark everything stale so the next Refresh/auto-tick
                // re-queries against the freshly rebuilt one instead of
                // silently showing pre-reset data.
                g_globals_tree_ready = false;
                g_modules_tree_ready = false;
                invalidate_tree(g_globals_tree);
                invalidate_tree(g_modules_tree);
                g_inspect_has_result = false;
            });
        }

        api.log_info("%s ready (F10 toggles within UEVR's own menu; open UEVR's UI to see it)", TAG);
    }

    bool on_message(HWND, UINT msg, WPARAM wparam, LPARAM) override {
        if (msg == WM_KEYDOWN && wparam == TOGGLE_KEY) {
            g_window_open = !g_window_open;
            API::get()->log_info("%s window=%s", TAG,
                g_window_open ? "open" : "closed");
        }
        return true;
    }
};

static LuaScriptEditorPlugin g_plugin_instance;
