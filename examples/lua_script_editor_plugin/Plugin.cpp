// lua_script_editor_plugin: a full Lua editor + REPL window inside UEVR's
// overlay. Uses ImGuiColorTextEdit (TextEditor.cpp / TextDiff.cpp) for syntax
// highlighting and links its own copy of ImGui — same version as UEVR's
// renderlib (1.92.5) so the in-process ABI matches.
//
// Bridge to the host:
//   * on_imgui_frame fires with the host's ImGuiContext+allocators. We rebind
//     them on every call so our static ImGui copy operates against UEVR's data.
//   * exec_lua_chunk runs the editor buffer in UEVR's main lua state.
//   * get_persistent_dir() gives us the scripts folder so the file list and
//     save/open buttons round-trip to disk.
//
// Hotkeys (only when the editor window has focus):
//   F10        toggle window
//   Ctrl+S     save
//   Ctrl+R     run buffer
//   Ctrl+Enter run buffer

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

// One persistent TextEditor instance. Lazy-init on first render so its imgui
// constructor (which queries fonts) runs after we bind UEVR's context.
TextEditor* g_editor = nullptr;

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
std::string g_current_path;
bool g_modified = false;
std::string g_status;

// Output log
std::deque<std::string> g_log;
constexpr size_t LOG_MAX = 200;

bool g_autorun_target = false;
bool g_save_to_global = false;   // when true, Save uses g_global_scripts_dir
bool g_show_whitespace = false;
char g_filename_buf[128] = "untitled.lua";

// Bottom-pane tabs: Output / Globals / Modules
enum class BottomTab { Output, Globals, Modules };
BottomTab g_bottom_tab = BottomTab::Output;

// Buffers populated by exec_lua_chunk; rendered as monospace dumps.
constexpr size_t LUA_DUMP_BUF = 64 * 1024;
std::vector<char> g_globals_buf(LUA_DUMP_BUF);
std::vector<char> g_modules_buf(LUA_DUMP_BUF);
bool g_globals_autorefresh = false;
bool g_modules_autorefresh = false;
uint64_t g_dump_tick = 0;

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

void load_into_editor(const fs::path& p) {
    if (!g_editor) return;
    auto txt = read_file(p);
    g_editor->SetText(txt);
    g_current_path = p.string();
    std::snprintf(g_filename_buf, sizeof(g_filename_buf), "%s",
        p.filename().string().c_str());
    g_modified = false;
    g_status = "loaded " + p.filename().string()
        + " (" + std::to_string(txt.size()) + " bytes)";
    log(g_status);
}

bool save_current(bool save_as) {
    if (!g_editor) return false;

    fs::path target;
    if (save_as || g_current_path.empty()) {
        fs::path root = g_save_to_global ? g_global_scripts_dir : g_scripts_dir;
        if (g_autorun_target) root /= "autorun";
        target = root / g_filename_buf;
        if (target.extension() != ".lua") target += ".lua";
    } else {
        target = fs::path(g_current_path);
    }

    if (!write_file(target, g_editor->GetText())) {
        g_status = "save FAILED: " + target.string();
        log(g_status);
        return false;
    }
    g_current_path = target.string();
    g_modified = false;
    g_status = "saved " + target.filename().string();
    log(g_status);
    rescan_scripts();
    return true;
}

void run_buffer() {
    auto fns = API::get()->param()->functions;
    if (fns->exec_lua_chunk == nullptr) {
        g_status = "exec_lua_chunk unavailable — rebuild UEVRBackend";
        log(g_status);
        return;
    }
    if (!g_editor) return;

    const std::string code = g_editor->GetText();
    char out[1024] = {};
    const std::string label = g_current_path.empty() ? "lua_editor.buffer" : g_current_path;
    const bool ok = fns->exec_lua_chunk(code.c_str(), label.c_str(), out, sizeof(out));

    g_status = ok ? "run OK" : "run FAILED";
    log("== run " + label + " (" + std::to_string(code.size()) + " bytes) ==");
    if (out[0]) log(std::string(ok ? "= " : "!! ") + out);
}

void reset_lua_scripts() {
    auto fns = API::get()->param()->functions;
    if (fns->reset_lua_scripts == nullptr) {
        g_status = "reset_lua_scripts unavailable";
        log(g_status);
        return;
    }
    fns->reset_lua_scripts();
    log("reset_lua_scripts invoked — scripts dir reloaded");
}

// Lua-side dump chunks. Both return a single big string; the C++ side just
// writes it into the per-tab buffer for paint.
//
// Globals: enumerate pairs(_G), skip the well-known built-ins so the user
// sees their own state plus UEVR-installed tables. Type, size hint, short
// value preview when scalar.
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
        local n=0; for _ in pairs(v) do n=n+1; if n>9999 then break end end
        return string.format('table[#=%d]', n)
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
    local ok, v = pcall(function() return _G[k] end)
    out[#out+1] = string.format('%-32s %-12s %s', k, type(v), fmt(v))
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

void draw_toolbar() {
    if (ImGui::Button("New")) {
        g_editor->SetText("-- new script\n");
        g_current_path.clear();
        std::snprintf(g_filename_buf, sizeof(g_filename_buf), "untitled.lua");
        g_modified = false;
        g_status = "new buffer";
    }
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
        g_editor->SetShowWhitespacesEnabled(g_show_whitespace);
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
                load_into_editor(g_scripts[i].path);
            }
            if (selected && ImGui::IsItemHovered() && ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Open")) load_into_editor(g_scripts[i].path);
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

void draw_dump_inner(const char* tab, std::vector<char>& buf, bool& autorefresh,
                     void (*refresh)()) {
    if (ImGui::Button("Refresh")) refresh();
    ImGui::SameLine();
    ImGui::Checkbox("auto (60f)", &autorefresh);
    if (autorefresh && (g_dump_tick % 60) == 0) refresh();
    ImGui::SameLine();
    if (ImGui::Button("Copy")) ImGui::SetClipboardText(buf.data());
    ImGui::SameLine();
    const size_t len = std::strlen(buf.data());
    ImGui::Text("(%zu bytes)", len);
    ImGui::Separator();

    if (ImGui::BeginChild(tab, ImVec2(0, 0), false,
            ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::PushFont(nullptr); // default; styling pass could pick a mono font.
        ImGui::TextUnformatted(buf.data(), buf.data() + len);
        ImGui::PopFont();
    }
    ImGui::EndChild();
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
            draw_dump_inner("##globals_view", g_globals_buf,
                g_globals_autorefresh, refresh_globals);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Modules")) {
            g_bottom_tab = BottomTab::Modules;
            draw_dump_inner("##modules_view", g_modules_buf,
                g_modules_autorefresh, refresh_modules);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void hotkeys() {
    const auto& io = ImGui::GetIO();
    if (!io.KeyCtrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) save_current(false);
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) run_buffer();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) run_buffer();
}

void render(UEVR_ImGuiFrameCbData* data) {
    if (!g_window_open) return;
    if (data == nullptr) return;

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
        API::get()->log_info("%s ImGui ABI ok: %s (sizeof ctx=%zu, io=%zu)",
            TAG, ImGui::GetVersion(),
            sizeof(ImGuiStyle), sizeof(ImGuiIO));
    }

    if (g_editor == nullptr) {
        g_editor = new TextEditor();
        g_editor->SetLanguage(TextEditor::Language::Lua());
        g_editor->SetTabSize(2);
        g_editor->SetText(
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
            if (ImGui::MenuItem("New"))       { g_editor->SetText("-- new\n"); g_current_path.clear(); }
            if (ImGui::MenuItem("Save", "Ctrl+S"))    save_current(false);
            if (ImGui::MenuItem("Save As"))           save_current(true);
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
                g_editor->SetShowWhitespacesEnabled(g_show_whitespace);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    draw_toolbar();
    ImGui::Separator();

    const auto avail = ImGui::GetContentRegionAvail();
    const float left_w = 220.0f;
    const float bottom_h = 160.0f;

    // Left: file list
    ImGui::BeginChild("##left", ImVec2(left_w, avail.y - bottom_h - 8.0f), true);
    draw_file_pane();
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: editor
    ImGui::BeginChild("##editor", ImVec2(0, avail.y - bottom_h - 8.0f), true);
    const std::string title = g_current_path.empty()
        ? std::string("(untitled)") : fs::path(g_current_path).filename().string();
    ImGui::TextDisabled("%s%s", title.c_str(), g_modified ? " *" : "");
    ImGui::Separator();
    g_editor->Render("##textedit", ImVec2(-1, -1), false);
    ImGui::EndChild();

    // Bottom: tabbed pane (Output / Globals / Modules)
    ImGui::BeginChild("##bottom", ImVec2(0, bottom_h), true);
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
            });
        }

        api.log_info("%s ready (F10 to toggle)", TAG);
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
