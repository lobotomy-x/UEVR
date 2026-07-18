// Class Browser: pump_class_sort_task (the async class-list sort), draw_class_browser_window,
// draw_class_inspector_window, and the sdk_* reflection dump helpers (JSON/Lua export feeding the
// Browser's "Export" buttons) — split out of UObjectHook.cpp purely for file-size organization (that
// file was 13k lines). Still UObjectHook:: member function definitions (same class, same members,
// same mutex, same everything) — only WHERE the code lives changed, not what it does. See
// src/mods/uobjecthook/SDKDumper.cpp for the existing precedent of this pattern, and
// uobjecthook/Gizmo.cpp / PropertyEditor.cpp for the first two splits of this kind.
//
// The sdk_* dump helpers and uobjecthook_dock_into_host_once are plain free functions (internal
// linkage) used ONLY within this file, so they moved here as-is with no promotion needed —
// uobjecthook_dock_into_host_once is duplicated (not shared) with UObjectHook.cpp's own copy since
// it's also called from windows that stayed there (draw_function_caller_window, draw_main_window,
// draw_options_window); a tiny parameterless helper with zero cross-TU state, safe to have one
// internal-linkage copy per translation unit.

#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <map>

#include <nlohmann/json.hpp>

#include <utility/Logging.hpp>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/UObjectBase.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FField.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/FEnumProperty.hpp>
#include <sdk/UEnum.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/AActor.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/USceneComponent.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/FObjectProperty.hpp>
#include <sdk/FArrayProperty.hpp>
#include <sdk/FMapProperty.hpp>
#include <sdk/FSetProperty.hpp>

#include <imgui_internal.h>
#include "../Framework.hpp"
#include "../VR.hpp"
#include "../LuaLoader.hpp"

#include "../UObjectHook.hpp"


// Touch-style drag-to-pan for the current scroll region. Call it inside a
// BeginChild/BeginListBox scope (after the Begin, before the matching End) so
// SetScroll* targets that child. The active scroll target is latched per-window
// via the seeded GetID, so a fast drag that pulls the cursor outside the child
// bounds keeps scrolling until the button releases.
//   Flat: middle mouse button — unbound elsewhere, so it never collides with the
//     left-button drag-drop sources, right-button context menus, or window-move.
//   VR: the controller trigger maps to the left mouse button, so use that; gated
//     on no item hovered so a trigger-drag on empty list space scrolls without
//     stealing item drag-drops.
// Vertically biased + faster than 1:1 (lists are tall; 1:1 felt sluggish).
static void drag_scroll_current_window() {
    auto& io = ImGui::GetIO();
    // In VR, scrolling is done with the thumbstick: the overlay mouse-emulation feeds the right
    // stick into io.MouseWheel, which imgui uses to scroll the hovered window -- no window-move
    // clash. So in VR we do NOT do a trigger-drag here (the trigger is the left mouse button, which
    // imgui also uses to move the window). Only the flat (real-mouse) path keeps middle-button pan.
    if (VR::get()->is_hmd_active()) {
        return;
    }
    const ImGuiMouseButton btn = ImGuiMouseButton_Middle;
    const ImGuiID id = ImGui::GetID("##dragscroll");
    static ImGuiID s_active = 0;

    if (s_active == 0 && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(btn)) {
        s_active = id;
    }

    if (s_active == id) {
        if (ImGui::IsMouseDown(btn)) {
            // Bias hard towards vertical: content here is almost always a tall list, so a drag that's
            // only slightly diagonal shouldn't nudge the list sideways. X only scrolls once the drag is
            // clearly more horizontal than vertical.
            const float ady = std::fabs(io.MouseDelta.y), adx = std::fabs(io.MouseDelta.x);
            if (ady != 0.0f) {
                ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y * 3.0f);
            }
            if (adx > ady * 1.5f) {
                ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x * 1.5f);
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else {
            s_active = 0;
        }
    }
}
namespace {
// ---- SDK reflection dump engine -------------------------------------------
// Self-contained reflection -> JSON or Lua (no external libs). Feeds the Class
// Browser "Export" buttons. Emits per-property name/type/offset + raw
// PropertyFlags (hex) and decoded flag names, and per-function FunctionFlags
// (hex + names) with the param list. Output auto-splits into one file per
// package so individual files stay small.
enum class DumpFmt { Json, Lua };

const char* sdk_ext(DumpFmt fmt) { return fmt == DumpFmt::Lua ? "lua" : "json"; }
const char* sdk_arr_open(DumpFmt fmt) { return fmt == DumpFmt::Lua ? "{" : "["; }
const char* sdk_arr_close(DumpFmt fmt) { return fmt == DumpFmt::Lua ? "}" : "]"; }

// "key": (JSON) or key= (Lua). Every key we emit is a safe Lua bareword identifier.
std::string sdk_key(DumpFmt fmt, const char* k) {
    return fmt == DumpFmt::Lua ? (std::string(k) + "=") : ("\"" + std::string(k) + "\":");
}

std::string sdk_escape(DumpFmt fmt, const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                // JSON uses \uXXXX; Lua has no such escape, so use its decimal \ddd form.
                std::snprintf(buf, sizeof(buf), fmt == DumpFmt::Lua ? "\\%u" : "\\u%04x",
                    static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string sdk_str_array(DumpFmt fmt, const std::vector<std::string>& v) {
    std::string out = sdk_arr_open(fmt);
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ",";
        out += "\"" + sdk_escape(fmt, v[i]) + "\"";
    }
    out += sdk_arr_close(fmt);
    return out;
}

// Well-known EPropertyFlags bits. The raw hex is always emitted too, so an
// unrecognised bit is never lost -- this just adds human-readable names.
std::vector<std::string> sdk_decode_property_flags(uint64_t f) {
    static const std::pair<uint64_t, const char*> table[] = {
        {0x1ull, "Edit"}, {0x2ull, "ConstParm"}, {0x4ull, "BlueprintVisible"}, {0x8ull, "ExposeOnSpawn"},
        {0x10ull, "BlueprintReadOnly"}, {0x20ull, "Net"}, {0x40ull, "EditFixedSize"}, {0x80ull, "Parm"},
        {0x100ull, "OutParm"}, {0x200ull, "ZeroConstructor"}, {0x400ull, "ReturnParm"}, {0x800ull, "DisableEditOnTemplate"},
        {0x2000ull, "Transient"}, {0x4000ull, "Config"}, {0x10000ull, "DisableEditOnInstance"}, {0x20000ull, "EditConst"},
        {0x40000ull, "GlobalConfig"}, {0x80000ull, "InstancedReference"}, {0x200000ull, "DuplicateTransient"},
        {0x2000000ull, "SaveGame"}, {0x4000000ull, "NoClear"}, {0x10000000ull, "ReferenceParm"},
        {0x20000000ull, "BlueprintAssignable"}, {0x40000000ull, "Deprecated"}, {0x80000000ull, "IsPlainOldData"},
        {0x100000000ull, "RepSkip"}, {0x200000000ull, "RepNotify"}, {0x400000000ull, "Interp"}, {0x800000000ull, "NonTransactional"},
        {0x1000000000ull, "EditorOnly"}, {0x2000000000ull, "NoDestructor"}, {0x8000000000ull, "AutoWeak"},
        {0x10000000000ull, "ContainsInstancedReference"}, {0x20000000000ull, "AssetRegistrySearchable"},
        {0x40000000000ull, "SimpleDisplay"}, {0x80000000000ull, "AdvancedDisplay"}, {0x100000000000ull, "Protected"},
        {0x200000000000ull, "BlueprintCallable"}, {0x400000000000ull, "BlueprintAuthorityOnly"}, {0x800000000000ull, "TextExportTransient"},
        {0x1000000000000ull, "NonPIEDuplicateTransient"}, {0x4000000000000ull, "PersistentInstance"},
        {0x8000000000000ull, "UObjectWrapper"}, {0x10000000000000ull, "HasGetValueTypeHash"},
    };
    std::vector<std::string> names;
    for (const auto& [bit, name] : table) {
        if (f & bit) names.emplace_back(name);
    }
    return names;
}

// EFunctionFlags via the UFunction is_* helpers (authoritative for this SDK).
std::vector<std::string> sdk_decode_function_flags(sdk::UFunction* fn) {
    std::vector<std::string> n;
    auto add = [&](bool b, const char* s) { if (b) n.emplace_back(s); };
    add(fn->is_final(), "Final");                          add(fn->is_required_api(), "RequiredAPI");
    add(fn->is_blueprint_authority_only(), "BlueprintAuthorityOnly"); add(fn->is_blueprint_cosmetic(), "BlueprintCosmetic");
    add(fn->is_net(), "Net");                              add(fn->is_net_reliable(), "NetReliable");
    add(fn->is_net_request(), "NetRequest");               add(fn->is_exec(), "Exec");
    add(fn->is_native(), "Native");                        add(fn->is_event(), "Event");
    add(fn->is_net_response(), "NetResponse");             add(fn->is_static(), "Static");
    add(fn->is_net_multicast(), "NetMulticast");           add(fn->is_ubergraph_function(), "UbergraphFunction");
    add(fn->is_multicast_delegate(), "MulticastDelegate"); add(fn->is_public(), "Public");
    add(fn->is_private(), "Private");                      add(fn->is_protected(), "Protected");
    add(fn->is_delegate(), "Delegate");                    add(fn->is_net_server(), "NetServer");
    add(fn->has_out_params(), "HasOutParms");              add(fn->has_defaults(), "HasDefaults");
    add(fn->is_net_client(), "NetClient");                 add(fn->is_dll_import(), "DLLImport");
    add(fn->is_blueprint_callable(), "BlueprintCallable"); add(fn->is_blueprint_event(), "BlueprintEvent");
    add(fn->is_blueprint_pure(), "BlueprintPure");         add(fn->is_editor_only(), "EditorOnly");
    add(fn->is_const(), "Const");                          add(fn->is_net_validate(), "NetValidate");
    return n;
}

std::string sdk_dump_property(DumpFmt fmt, sdk::FProperty* prop) {
    std::string name = "?", type = "?";
    try { name = utility::narrow(prop->get_field_name().to_string()); } catch (...) {}
    try { type = utility::narrow(prop->get_class()->get_name().to_string()); } catch (...) {}
    uint64_t flags = 0; int32_t offset = 0;
    try { flags = prop->get_property_flags(); } catch (...) {}
    try { offset = prop->get_offset(); } catch (...) {}
    char fbuf[24];
    std::snprintf(fbuf, sizeof(fbuf), "0x%llx", static_cast<unsigned long long>(flags));
    return "{" + sdk_key(fmt, "name") + "\"" + sdk_escape(fmt, name) + "\"," +
        sdk_key(fmt, "type") + "\"" + sdk_escape(fmt, type) + "\"," +
        sdk_key(fmt, "offset") + std::to_string(offset) + "," +
        sdk_key(fmt, "flags") + "\"" + fbuf + "\"," +
        sdk_key(fmt, "flag_names") + sdk_str_array(fmt, sdk_decode_property_flags(flags)) + "}";
}

// ---- EmmyLua (LuaLS) SDK emitter ------------------------------------------
// The Lua export is a UE4SS-style TYPE-ANNOTATION sdk for a Lua language server
// (sumneko/lua-language-server): one ---@class per UClass/UScriptStruct with a
// ---@field per property (types resolved to the concrete class/struct/array/map/
// enum), plus function stubs with ---@param/---@return. It is NOT executable Lua
// data — point your LSP at the sdk_dump/classes folder for autocomplete on UEVR
// API objects. (The JSON export remains the machine-readable reflection dump.)

bool sdk_lua_is_keyword(const std::string& s) {
    static const std::unordered_set<std::string> kw{
        "and","break","do","else","elseif","end","false","for","function","goto",
        "if","in","local","nil","not","or","repeat","return","then","true","until","while"};
    return kw.count(s) != 0;
}

// UE name -> a valid Lua identifier for an @field / @param / enum key.
std::string sdk_lua_ident(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        out += (std::isalnum((unsigned char)c) || c == '_') ? c : '_';
    }
    if (out.empty()) out = "_";
    if (std::isdigit((unsigned char)out[0])) out.insert(out.begin(), '_');
    if (sdk_lua_is_keyword(out)) out += "_";
    return out;
}

std::string sdk_obj_name_or(sdk::UObject* o, const char* fb) {
    if (o == nullptr) return fb;
    try { return utility::narrow(o->get_fname().to_string()); } catch (...) { return fb; }
}

// Resolve an FProperty to its LuaLS type string (recurses through array/set/map/inner).
std::string sdk_lua_type(sdk::FProperty* prop) {
    if (prop == nullptr) return "any";
    std::string kind;
    try { kind = utility::narrow(prop->get_class()->get_name().to_string()); } catch (...) { return "any"; }

    if (kind == "BoolProperty") return "boolean";
    if (kind == "FloatProperty" || kind == "DoubleProperty") return "number";
    if (kind == "IntProperty" || kind == "Int8Property" || kind == "Int16Property" || kind == "Int64Property" ||
        kind == "UInt16Property" || kind == "UInt32Property" || kind == "UInt64Property" || kind == "ByteProperty")
        return "integer";
    if (kind == "StrProperty" || kind == "NameProperty" || kind == "TextProperty") return "string";
    if (kind == "EnumProperty") {
        sdk::UEnum* en = nullptr;
        try { en = reinterpret_cast<sdk::FEnumProperty*>(prop)->get_enum(); } catch (...) {}
        return sdk_obj_name_or((sdk::UObject*)en, "integer");
    }
    if (kind == "StructProperty") {
        sdk::UScriptStruct* ss = nullptr;
        try { ss = reinterpret_cast<sdk::FStructProperty*>(prop)->get_struct(); } catch (...) {}
        return sdk_obj_name_or((sdk::UObject*)ss, "table");
    }
    if (kind == "ArrayProperty") {
        sdk::FProperty* inner = nullptr;
        try { inner = reinterpret_cast<sdk::FArrayProperty*>(prop)->get_inner(); } catch (...) {}
        return sdk_lua_type(inner) + "[]";
    }
    if (kind == "SetProperty") {
        sdk::FProperty* e = nullptr;
        try { e = reinterpret_cast<sdk::FSetProperty*>(prop)->get_element_prop(); } catch (...) {}
        return sdk_lua_type(e) + "[]";
    }
    if (kind == "MapProperty") {
        sdk::FProperty* k = nullptr; sdk::FProperty* v = nullptr;
        try { k = reinterpret_cast<sdk::FMapProperty*>(prop)->get_key_prop(); } catch (...) {}
        try { v = reinterpret_cast<sdk::FMapProperty*>(prop)->get_value_prop(); } catch (...) {}
        return "table<" + sdk_lua_type(k) + ", " + sdk_lua_type(v) + ">";
    }
    if (kind == "ObjectProperty" || kind == "ClassProperty" || kind == "WeakObjectProperty" ||
        kind == "LazyObjectProperty" || kind == "SoftObjectProperty" || kind == "SoftClassProperty" ||
        kind == "InterfaceProperty" || kind == "ObjectPtrProperty") {
        sdk::UClass* pc = nullptr;
        try { pc = reinterpret_cast<sdk::FObjectProperty*>(prop)->get_property_class(); } catch (...) {}
        return sdk_obj_name_or((sdk::UObject*)pc, "UObject");
    }
    if (kind == "DelegateProperty" || kind == "MulticastInlineDelegateProperty" ||
        kind == "MulticastSparseDelegateProperty") {
        return "function";
    }
    return "any";
}

// EmmyLua ---@class block for a UClass/UScriptStruct: declared @fields + function stubs. Inherited
// members are provided by the `: Super` chain, so only declared members are emitted per class.
std::string sdk_emmylua_ustruct(sdk::UStruct* strukt) {
    static const auto ufunction_t = sdk::UFunction::static_class();
    const std::string name = sdk_obj_name_or((sdk::UObject*)strukt, "Unknown");
    std::string super;
    if (auto s = strukt->get_super_struct(); s != nullptr) {
        super = sdk_obj_name_or((sdk::UObject*)s, "");
    }

    std::string out = "---@class " + name;
    if (!super.empty()) out += " : " + super;
    out += "\n";

    for (auto f = strukt->get_child_properties(); f != nullptr; f = f->get_next()) {
        auto* p = reinterpret_cast<sdk::FProperty*>(f);
        std::string pn;
        try { pn = utility::narrow(p->get_field_name().to_string()); } catch (...) { continue; }
        out += "---@field " + sdk_lua_ident(pn) + " " + sdk_lua_type(p) + "\n";
    }
    out += name + " = {}\n\n";

    for (auto child = strukt->get_children(); child != nullptr; child = child->get_next()) {
        if (child->get_class() == nullptr || !child->get_class()->is_a(ufunction_t)) continue;
        auto* fn = reinterpret_cast<sdk::UFunction*>(child);
        std::string fn_name;
        try { fn_name = utility::narrow(fn->get_fname().to_string()); } catch (...) { continue; }

        std::string anno;
        std::vector<std::string> in_names;
        std::vector<std::string> ret_types;
        for (auto p = fn->get_child_properties(); p != nullptr; p = p->get_next()) {
            auto* fp = reinterpret_cast<sdk::FProperty*>(p);
            uint64_t fl = 0; try { fl = fp->get_property_flags(); } catch (...) {}
            if ((fl & 0x80) == 0) continue; // not a Parm (skip locals)
            std::string pn; try { pn = utility::narrow(fp->get_field_name().to_string()); } catch (...) { pn = "arg"; }
            const std::string ty = sdk_lua_type(fp);
            const bool is_return = (fl & 0x400) != 0; // ReturnParm
            const bool is_out    = (fl & 0x100) != 0; // OutParm (comes back via ProcessEvent)
            if (is_return || is_out) {
                ret_types.push_back(ty);
            }
            if (!is_return) { // ReturnParm is output-only; everything else is also an input
                const std::string id = sdk_lua_ident(pn);
                anno += "---@param " + id + " " + ty + "\n";
                in_names.push_back(id);
            }
        }
        for (const auto& rt : ret_types) anno += "---@return " + rt + "\n";

        out += anno;
        out += "function " + name + ":" + sdk_lua_ident(fn_name) + "(";
        for (size_t i = 0; i < in_names.size(); ++i) { if (i) out += ", "; out += in_names[i]; }
        out += ") end\n\n";
    }
    return out;
}

// EmmyLua ---@enum block: named integer values (leaf of any "EType::Value" key).
std::string sdk_emmylua_uenum(sdk::UEnum* uenum) {
    const std::string name = sdk_obj_name_or((sdk::UObject*)uenum, "UnknownEnum");
    std::string out = "---@enum " + name + "\n" + name + " = {\n";
    try {
        for (auto& [k, v] : uenum->get_names()) {
            std::string key = k;
            if (const auto cc = key.rfind("::"); cc != std::string::npos) key = key.substr(cc + 2);
            out += "    " + sdk_lua_ident(key) + " = " + std::to_string(v) + ",\n";
        }
    } catch (...) {}
    out += "}\n\n";
    return out;
}

// Dump a UStruct (UClass or UScriptStruct). include_inherited walks the super
// chain for properties/functions; false = declared members only.
std::string sdk_dump_ustruct(DumpFmt fmt, sdk::UStruct* strukt, bool include_inherited) {
    if (fmt == DumpFmt::Lua) {
        return sdk_emmylua_ustruct(strukt); // UE4SS-style annotations, not JSON-as-Lua
    }
    static const auto ufunction_t = sdk::UFunction::static_class();
    std::string name, full, super;
    try { name = utility::narrow(strukt->get_fname().to_string()); } catch (...) {}
    try { full = utility::narrow(strukt->get_full_name()); } catch (...) {}
    if (auto s = strukt->get_super_struct(); s != nullptr) {
        try { super = utility::narrow(s->get_fname().to_string()); } catch (...) {}
    }

    std::string out = "{" + sdk_key(fmt, "name") + "\"" + sdk_escape(fmt, name) + "\"," +
        sdk_key(fmt, "full_name") + "\"" + sdk_escape(fmt, full) + "\"," +
        sdk_key(fmt, "super") + "\"" + sdk_escape(fmt, super) + "\"," +
        sdk_key(fmt, "properties_size") + std::to_string(static_cast<int>(strukt->get_properties_size())) + "," +
        sdk_key(fmt, "properties") + sdk_arr_open(fmt);

    bool first = true;
    for (auto super_s = strukt; super_s != nullptr; super_s = super_s->get_super_struct()) {
        for (auto f = super_s->get_child_properties(); f != nullptr; f = f->get_next()) {
            if (!first) out += ","; first = false;
            out += sdk_dump_property(fmt, reinterpret_cast<sdk::FProperty*>(f));
        }
        if (!include_inherited) break;
    }
    out += sdk_arr_close(fmt);
    out += "," + sdk_key(fmt, "functions") + sdk_arr_open(fmt);

    first = true;
    for (auto super_s = strukt; super_s != nullptr; super_s = super_s->get_super_struct()) {
        for (auto child = super_s->get_children(); child != nullptr; child = child->get_next()) {
            if (child->get_class() == nullptr || !child->get_class()->is_a(ufunction_t)) continue;
            auto* fn = reinterpret_cast<sdk::UFunction*>(child);
            std::string fn_name;
            uint32_t fflags = 0;
            try { fn_name = utility::narrow(fn->get_fname().to_string()); } catch (...) {}
            try { fflags = fn->get_function_flags(); } catch (...) {}
            char fbuf[16];
            std::snprintf(fbuf, sizeof(fbuf), "0x%x", fflags);
            if (!first) out += ","; first = false;
            out += "{" + sdk_key(fmt, "name") + "\"" + sdk_escape(fmt, fn_name) + "\"," +
                   sdk_key(fmt, "flags") + "\"" + fbuf + "\"," +
                   sdk_key(fmt, "flag_names") + sdk_str_array(fmt, sdk_decode_function_flags(fn)) + "," +
                   sdk_key(fmt, "params") + sdk_arr_open(fmt);
            bool pfirst = true;
            for (auto p = fn->get_child_properties(); p != nullptr; p = p->get_next()) {
                if (!pfirst) out += ","; pfirst = false;
                out += sdk_dump_property(fmt, reinterpret_cast<sdk::FProperty*>(p));
            }
            out += sdk_arr_close(fmt);
            out += "}";
        }
        if (!include_inherited) break;
    }
    out += sdk_arr_close(fmt);
    out += "}";
    return out;
}

std::string sdk_dump_uenum(DumpFmt fmt, sdk::UEnum* uenum) {
    if (fmt == DumpFmt::Lua) {
        return sdk_emmylua_uenum(uenum); // UE4SS-style ---@enum, not JSON-as-Lua
    }
    std::string name, full;
    try { name = utility::narrow(uenum->get_fname().to_string()); } catch (...) {}
    try { full = utility::narrow(uenum->get_full_name()); } catch (...) {}
    std::string out = "{" + sdk_key(fmt, "name") + "\"" + sdk_escape(fmt, name) + "\"," +
        sdk_key(fmt, "full_name") + "\"" + sdk_escape(fmt, full) + "\"," +
        sdk_key(fmt, "kind") + "\"enum\"," + sdk_key(fmt, "values") + sdk_arr_open(fmt);
    try {
        auto names = uenum->get_names();
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) out += ",";
            out += "{" + sdk_key(fmt, "name") + "\"" + sdk_escape(fmt, names[i].first) + "\"," +
                   sdk_key(fmt, "value") + std::to_string(names[i].second) + "}";
        }
    } catch (...) {}
    out += sdk_arr_close(fmt);
    out += "}";
    return out;
}

// Package id for grouping, from a full name like "Class /Script/Engine.Actor" -> "Script.Engine".
// Sanitised so it's a safe filename.
std::string sdk_package_of(const std::string& full_name) {
    const auto sp = full_name.find(' ');
    const auto path = (sp != std::string::npos) ? full_name.substr(sp + 1) : full_name;
    const auto dot = path.find('.');
    const std::string pkg = (dot != std::string::npos) ? path.substr(0, dot) : path;
    std::string out;
    for (char c : pkg) {
        if (c == '/') { if (!out.empty()) out += '.'; }
        else if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') out += '_';
        else out += c;
    }
    return out.empty() ? "Misc" : out;
}

// Auto-split writer: one file per package at <profile>/sdk_dump/<kind>/<Package>.<ext>, so no
// single file gets huge. Lua files are a `return { ... }` table; JSON files are {"<kind>":[ ... ]}.
void sdk_write_split(DumpFmt fmt, const char* kind, const std::map<std::string, std::vector<std::string>>& by_package) {
    try {
        const auto root = Framework::get_persistent_dir() / "sdk_dump" / kind;
        std::filesystem::create_directories(root);
        size_t files = 0, items = 0;
        for (const auto& [pkg, parts] : by_package) {
            std::string doc;
            if (fmt == DumpFmt::Lua) {
                // UE4SS-style annotation file: standalone ---@class/@enum blocks, no data wrapper.
                // ---@meta makes the LSP treat these as ambient definitions (available without require).
                doc = "---@meta\n"
                      "-- UEVR SDK type annotations for '" + pkg + "' (UE4SS-style, for Lua LSP autocomplete).\n"
                      "-- Auto-generated by the UEVR Class Browser; NOT executable. Point sumneko/\n"
                      "-- lua-language-server (e.g. .luarc.json workspace.library) at this sdk_dump folder.\n\n";
                for (const auto& part : parts) doc += part; // each block already ends with a blank line
            } else {
                std::string body = sdk_arr_open(fmt);
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (i) body += ",";
                    body += parts[i];
                }
                body += sdk_arr_close(fmt);
                doc = std::string("{") + sdk_key(DumpFmt::Json, kind) + body + "}";
                // Pretty-print so the JSON export isn't a single dense line.
                try { doc = nlohmann::json::parse(doc).dump(2); } catch (...) {}
            }
            const auto path = root / (pkg + "." + sdk_ext(fmt));
            std::ofstream f{path, std::ios::binary | std::ios::trunc};
            f.write(doc.data(), static_cast<std::streamsize>(doc.size()));
            ++files;
            items += parts.size();
        }
        spdlog::info("[UObjectHook] Exported {} {} across {} package file(s) ({}) -> {}",
            items, kind, files, sdk_ext(fmt), root.string());
    } catch (const std::exception& e) {
        spdlog::error("[UObjectHook] SDK dump ({}) failed: {}", kind, e.what());
    }
}
} // namespace


// Rebuild the "All Objects" row cache when a trigger says it's stale — see AllObjectsRow in the
// header for why the tab is cached at all. Safe to call every frame; the common case is three
// comparisons and a return.
//
// Triggers, in the order checked:
//   1. never built yet;
//   2. explicit request (the Refresh button, m_all_objects_force_refresh);
//   3. m_objects.size() changed — objects were spawned/destroyed, so the list definitely moved;
//   4. the auto-refresh timer elapsed — catches changes the COUNT can't see (an object renamed, or an
//      equal number of spawns and destroys between checks).
// The count read in (3) takes the shared lock, but it's one size() call, not a full walk.
void UObjectHook::pump_all_objects_cache() {
    const auto now = std::chrono::steady_clock::now();

    size_t current_count = 0;
    { std::shared_lock _{m_mutex}; current_count = m_objects.size(); }

    const bool never_built = m_all_objects_last_refresh == std::chrono::steady_clock::time_point{};
    const bool count_changed = current_count != m_all_objects_last_count;
    const bool timer_due = m_all_objects_auto_refresh &&
        (now - m_all_objects_last_refresh) >= std::chrono::milliseconds((int64_t)(m_all_objects_refresh_seconds * 1000.0f));

    if (!never_built && !m_all_objects_force_refresh && !count_changed && !timer_due) {
        return;
    }

    m_all_objects_force_refresh = false;
    m_all_objects_last_refresh = now;
    m_all_objects_last_count = current_count;

    // One pass under the lock: copy out the pointer AND the cached wide name, so the expensive part
    // (widen->narrow conversion, shorten_object_path) happens after the lock is released. Objects
    // without meta are skipped rather than calling get_full_name under the lock — the meta map is the
    // authority on what's safely nameable, and anything missing from it will be picked up on the next
    // rebuild once the add hook has populated it.
    std::vector<std::pair<sdk::UObjectBase*, std::wstring>> raw;
    {
        std::shared_lock _{m_mutex};
        raw.reserve(m_objects.size());
        for (auto* obj : m_objects) {
            if (obj == nullptr || !this->exists_unsafe(obj)) continue;
            auto it = m_meta_objects.find(obj);
            if (it == m_meta_objects.end() || it->second == nullptr) continue;
            raw.emplace_back(obj, it->second->full_name);
        }
    }

    m_all_objects_cache.clear();
    m_all_objects_cache.reserve(raw.size());
    for (auto& [obj, full_w] : raw) {
        AllObjectsRow row{};
        row.obj = obj;
        row.full = utility::narrow(full_w);
        row.short_label = shorten_object_path(row.full);
        m_all_objects_cache.push_back(std::move(row));
    }

    // Stable order so rows don't jump between rebuilds (m_objects is an unordered set — its iteration
    // order is arbitrary and can change on rehash, which made the old per-frame list shuffle itself).
    std::sort(m_all_objects_cache.begin(), m_all_objects_cache.end(),
              [](const AllObjectsRow& a, const AllObjectsRow& b) { return a.full < b.full; });
}

// Build/refresh m_sorted_classes by either harvesting a completed async sort
// or kicking off a new one. Idempotent and safe to call every frame from
// multiple views (Class Browser + Objects-by-Class). Throttled to ~2s between
// relaunches so we don't pin a worker thread with the same work over and over.
void UObjectHook::pump_class_sort_task() {
    // Harvest any completed task first.
    if (m_sorting_task.valid()) {
        if (m_sorting_task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            m_sorted_classes = m_sorting_task.get();
        } else {
            // Still running — don't queue another one on top.
            return;
        }
    }

    // Throttle relaunches: the class universe doesn't change every frame, and
    // the sort itself can take noticeable CPU on games with tens of thousands
    // of classes. 2s feels like a reasonable freshness vs cost tradeoff.
    const auto now = std::chrono::steady_clock::now();
    if (m_last_sort_time != std::chrono::steady_clock::time_point{} &&
        (now - m_last_sort_time) < std::chrono::seconds(2)) {
        return;
    }

    auto sort_classes = [this](std::vector<sdk::UClass*> classes) {
        std::sort(classes.begin(), classes.end(), [this](sdk::UClass* a, sdk::UClass* b) {
            std::shared_lock _{m_mutex};
            // find() not operator[] — m_meta_objects is a unique_ptr map; [] would
            // silently default-insert null entries under shared_lock and deref null.
            auto ita = m_meta_objects.find(a);
            auto itb = m_meta_objects.find(b);
            const bool a_ok = ita != m_meta_objects.end() && ita->second != nullptr;
            const bool b_ok = itb != m_meta_objects.end() && itb->second != nullptr;
            if (!a_ok || !b_ok) {
                // Stable fallback ordering by pointer when meta is missing
                // (unordered_map iterators aren't <-comparable).
                return (uintptr_t)a < (uintptr_t)b;
            }
            return ita->second->full_name < itb->second->full_name;
        });
        return classes;
    };

    auto unsorted_classes = std::vector<sdk::UClass*>{};
    {
        std::shared_lock _{m_mutex};
        unsorted_classes.reserve(m_objects_by_class.size());
        for (auto& [c, set] : m_objects_by_class) {
            unsorted_classes.push_back(c);
        }
    }

    m_sorting_task = std::async(std::launch::async, sort_classes, unsorted_classes);
    m_last_sort_time = now;
}

// Helper: docks the next window into the main UEVR dockspace on first use
// (when the host is enabled) AND honours the PageUp "reset all windows"
// pulse by forcing position/collapse back to default on those frames.
static void uobjecthook_dock_into_host_once() {
    if (auto host = Framework::get_main_dockspace_id(); host != 0) {
        ImGui::SetNextWindowDockID(host, ImGuiCond_FirstUseEver);
    }
    if (Framework::is_force_reset_windows()) {
        // Park near the centre when the user hits PageUp, in case the
        // window had drifted offscreen / been dragged into a stale popup
        // viewport. Cond_Always overrides any persisted .ini position.
        ImGuiViewport* vp = ImGui::GetMainViewport();
        if (vp != nullptr) {
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.25f,
                                          vp->WorkPos.y + vp->WorkSize.y * 0.25f),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.5f,
                                            vp->WorkSize.y * 0.5f),
                                    ImGuiCond_Always);
        }
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
    }
}

void UObjectHook::draw_class_browser_window() {
    uobjecthook_dock_into_host_once();
    if (!ImGui::Begin("UEVR Class Browser", &m_show_class_browser)) {
        ImGui::End();
        return;
    }
    utility::ScopeGuard end_guard{[]() { ImGui::End(); }};

    // Independent of Objects-by-Class — the user shouldn't have to open that
    // view first to get the class list to populate. Same task, just called
    // from both surfaces.
    pump_class_sort_task();

    // Refresh / status row. The sort task is async and throttled to ~2s
    // between launches, so newly-spawned classes (level transitions,
    // bp.spawn calls from Lua, etc.) won't show up immediately. The Refresh
    // button bumps m_last_sort_time back to epoch which lets pump kick off
    // a fresh sort on the next call without waiting out the throttle.
    if (ImGui::SmallButton("Refresh")) {
        m_last_sort_time = std::chrono::steady_clock::time_point{};
        pump_class_sort_task(); // launch immediately this frame
    }
    ImGui::SameLine();
    if (m_sorting_task.valid()) {
        ImGui::TextDisabled("(sorting...)");
    } else {
        ImGui::TextDisabled("%zu classes", m_sorted_classes.size());
    }

    // Filter input shared across tabs. The class iteration uses
    // m_sorted_classes (built by pump_class_sort_task above), which is
    // typically a large list (~thousands), so we always filter even if the
    // filter buffer is empty — narrowing happens substring on full name.
    std::array<char, 256> filter_buf{};
    const auto n = std::min(m_class_browser_filter.size(), filter_buf.size() - 1);
    std::memcpy(filter_buf.data(), m_class_browser_filter.data(), n);
    if (ImGui::InputTextWithHint("filter", "type substring of class name",
                                 filter_buf.data(), filter_buf.size())) {
        m_class_browser_filter.assign(filter_buf.data());
    }
    const auto wfilter = utility::widen(m_class_browser_filter);
    const bool has_filter = !wfilter.empty();

    // Export format, shared by all three tab export buttons.
    static int s_dump_fmt_idx = 0; // 0 = JSON, 1 = Lua
    ImGui::SetNextItemWidth(90.0f);
    ImGui::Combo("dump format", &s_dump_fmt_idx, "JSON\0Lua (LSP defs)\0");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("JSON: machine-readable reflection dump (pretty-printed).\n"
                          "Lua (LSP defs): UE4SS-style ---@class / ---@field / ---@enum type\n"
                          "annotations for Lua-language-server autocomplete. Point your\n"
                          ".luarc.json workspace.library at <profile>/sdk_dump/. Not executable.");
    }
    const DumpFmt dump_fmt = s_dump_fmt_idx == 1 ? DumpFmt::Lua : DumpFmt::Json;

    // Specific-or-batch dump: exports every class currently matching the filter (filter to one
    // name for a single class) from the already-validated m_sorted_classes, so there's no
    // FUObjectArray re-classification. Auto-split into one file per package to keep files small.
    // Each class carries declared properties (name/type/offset + PropertyFlags) and functions
    // (FunctionFlags + params).
    if (ImGui::SmallButton("Export classes")) {
        std::vector<sdk::UClass*> to_dump;
        {
            std::shared_lock _{m_mutex};
            to_dump.reserve(m_sorted_classes.size());
            for (auto* uclass : m_sorted_classes) {
                if (uclass == nullptr) continue;
                if (has_filter) {
                    auto it = m_meta_objects.find(uclass);
                    if (it == m_meta_objects.end() || it->second == nullptr) continue;
                    if (it->second->full_name.find(wfilter) == std::wstring::npos) continue;
                }
                to_dump.push_back(uclass);
            }
        }
        std::map<std::string, std::vector<std::string>> by_package;
        for (auto* uclass : to_dump) {
            std::string full;
            try { full = utility::narrow(uclass->get_full_name()); } catch (...) {}
            const auto pkg = sdk_package_of(full);
            try { by_package[pkg].push_back(sdk_dump_ustruct(dump_fmt, reinterpret_cast<sdk::UStruct*>(uclass), false)); }
            catch (...) {}
        }
        sdk_write_split(dump_fmt, "classes", by_package);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Dump the filtered class list (properties+offset+flags, functions+flags+params),\n"
                          "one file per package, to <profile>/sdk_dump/classes/<Package>.%s", sdk_ext(dump_fmt));
    }

    if (!ImGui::BeginTabBar("ClassBrowserTabs")) {
        return;
    }
    utility::ScopeGuard tab_guard{[]() { ImGui::EndTabBar(); }};

    // ---- Classes tab -------------------------------------------------------
    if (ImGui::BeginTabItem("Classes")) {
        ImGui::TextDisabled("%zu classes total — drag into a Class slot or click to inspect",
            m_sorted_classes.size());

        // Shared per-class row: click opens a dedicated Class Inspector window;
        // drag publishes a UEVR_UClass payload. `display` is the visible label —
        // the short leaf name in the tree view, the full path in the flat lists.
        // Callers all already hold m_mutex (shared_lock) around their render_class_row loop, so this
        // count lookup is safe without an extra lock of its own.
        auto render_class_row = [&](sdk::UClass* uclass, const std::wstring& full, const std::string& display) {
            ImGui::PushID(uclass);
            size_t live_count = 0;
            if (auto it = m_objects_by_class.find(uclass); it != m_objects_by_class.end()) {
                live_count = it->second.size();
            }
            const std::string row_label = live_count > 0 ? (display + "  (" + std::to_string(live_count) + ")") : display;
            if (ImGui::Selectable(row_label.c_str())) {
                if (std::find(m_open_class_inspectors.begin(), m_open_class_inspectors.end(), uclass)
                        == m_open_class_inspectors.end()) {
                    m_open_class_inspectors.push_back(uclass);
                }
            }
            if (ImGui::IsItemHovered() && !full.empty()) {
                ImGui::SetTooltip("%s\n%zu live instance(s)", utility::narrow(full).c_str(), live_count); // full path on hover; row shows the short form
            }
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("UEVR_UClass", &uclass, sizeof(uclass));
                ImGui::Text("UClass: %s", utility::narrow(full).c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        };

        // Flat Native vs Blueprint lists (full path per row).
        auto render_class_list = [&](const char* child_id, bool want_native) {
            if (ImGui::BeginChild(child_id, ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                drag_scroll_current_window();
                std::shared_lock _{m_mutex};
                int shown = 0;
                for (auto* uclass : m_sorted_classes) {
                    if (uclass == nullptr) continue;
                    auto it = m_meta_objects.find(uclass);
                    if (it == m_meta_objects.end() || it->second == nullptr) continue;
                    const auto& full = it->second->full_name;
                    const bool is_native = full.find(L"/Script/") != std::wstring::npos;
                    if (is_native != want_native) continue;
                    if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                    render_class_row(uclass, full, shorten_object_path(utility::narrow(full)));
                    ++shown;
                }
                if (shown == 0) {
                    if (m_sorted_classes.empty()) {
                        ImGui::TextDisabled("class list not yet populated — sort task may still be running...");
                    } else if (has_filter) {
                        ImGui::TextDisabled("no matches for filter");
                    } else {
                        ImGui::TextDisabled("no %s classes", want_native ? "native" : "Blueprint");
                    }
                }
            }
            ImGui::EndChild();
        };

        // Hierarchical view: the package path is split into logical segments, so
        // "/Script/Engine.Actor" nests as Script > Engine > Actor (leaf). Module
        // grouping falls out for free (the segment after Script/Game). Node IDs
        // come from the ImGui push stack (one PushID per level) so identical
        // segment names under different parents never collide. Built per frame,
        // but only open nodes recurse.
        auto render_class_tree = [&]() {
            if (ImGui::BeginChild("class_tree", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                drag_scroll_current_window();
                std::shared_lock _{m_mutex};
                struct Node {
                    std::map<std::string, Node> children;
                    std::vector<std::tuple<sdk::UClass*, const std::wstring*, std::string>> leaves;
                    int count = 0;
                };
                Node root;
                for (auto* uclass : m_sorted_classes) {
                    if (uclass == nullptr) continue;
                    auto it = m_meta_objects.find(uclass);
                    if (it == m_meta_objects.end() || it->second == nullptr) continue;
                    const auto& full = it->second->full_name;
                    if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                    const auto sp = full.find(L' ');
                    const std::wstring path = (sp == std::wstring::npos) ? full : full.substr(sp + 1);
                    const auto dot = path.find(L'.');
                    const std::wstring pkg = (dot == std::wstring::npos) ? path : path.substr(0, dot);
                    const std::wstring leaf = (dot == std::wstring::npos) ? path : path.substr(dot + 1);
                    Node* cur = &root;
                    ++cur->count;
                    size_t start = 0;
                    while (start < pkg.size()) {
                        if (pkg[start] == L'/') { ++start; continue; }
                        const auto end = pkg.find(L'/', start);
                        const auto seg = pkg.substr(start, (end == std::wstring::npos ? pkg.size() : end) - start);
                        cur = &cur->children[utility::narrow(seg)];
                        ++cur->count;
                        if (end == std::wstring::npos) break;
                        start = end + 1;
                    }
                    cur->leaves.emplace_back(uclass, &full, utility::narrow(leaf));
                }

                auto draw_node = [&](auto&& self, Node& n) -> void {
                    for (auto& [seg, child] : n.children) {
                        ImGui::PushID(seg.c_str());
                        const auto hdr = seg + "  (" + std::to_string(child.count) + ")";
                        if (ImGui::TreeNode(hdr.c_str())) {
                            self(self, child);
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    for (auto& [uclass, full, leaf] : n.leaves) {
                        render_class_row(uclass, *full, leaf);
                    }
                };

                if (root.count == 0) {
                    ImGui::TextDisabled(m_sorted_classes.empty()
                        ? "class list not yet populated — sort task may still be running..."
                        : "no matches for filter");
                } else {
                    draw_node(draw_node, root);
                }
            }
            ImGui::EndChild();
        };

        // Inheritance (super -> sub) tree -- "organize by class". Built from get_super_struct over
        // the known class set; roots are classes whose super isn't in the list. Click a node's label
        // to inspect it (the arrow / double-click expands); leaf classes reuse the shared class row.
        // When filtering, a node shows if it or any descendant matches.
        auto render_class_hierarchy = [&]() {
            if (ImGui::BeginChild("class_hier", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                drag_scroll_current_window();
                std::shared_lock _{m_mutex};

                std::unordered_set<sdk::UClass*> known;
                for (auto* c : m_sorted_classes) {
                    if (c != nullptr) known.insert(c);
                }
                std::unordered_map<sdk::UClass*, std::vector<sdk::UClass*>> children;
                std::vector<sdk::UClass*> roots;
                for (auto* c : m_sorted_classes) {
                    if (c == nullptr) continue;
                    sdk::UClass* super = nullptr;
                    try { super = (sdk::UClass*)c->get_super_struct(); } catch (...) {}
                    if (super != nullptr && super != c && known.count(super) != 0) {
                        children[super].push_back(c);
                    } else {
                        roots.push_back(c);
                    }
                }

                auto name_of = [&](sdk::UClass* c) -> std::string {
                    try { return utility::narrow(c->get_fname().to_string()); } catch (...) { return "?"; }
                };
                auto full_of = [&](sdk::UClass* c) -> std::wstring {
                    auto it = m_meta_objects.find(c);
                    return (it != m_meta_objects.end() && it->second != nullptr) ? it->second->full_name : std::wstring{};
                };
                const auto by_name = [&](sdk::UClass* a, sdk::UClass* b) { return name_of(a) < name_of(b); };
                std::sort(roots.begin(), roots.end(), by_name);
                for (auto& [parent, kids] : children) {
                    std::sort(kids.begin(), kids.end(), by_name);
                }

                std::unordered_map<sdk::UClass*, bool> vis_cache;
                auto visible = [&](auto&& self, sdk::UClass* c) -> bool {
                    if (!has_filter) return true;
                    if (auto f = vis_cache.find(c); f != vis_cache.end()) return f->second;
                    bool v = full_of(c).find(wfilter) != std::wstring::npos;
                    if (!v) {
                        if (auto ch = children.find(c); ch != children.end()) {
                            for (auto* k : ch->second) { if (self(self, k)) { v = true; break; } }
                        }
                    }
                    vis_cache[c] = v;
                    return v;
                };

                auto draw = [&](auto&& self, sdk::UClass* c) -> void {
                    if (!visible(visible, c)) return;
                    auto ch = children.find(c);
                    const bool has_kids = ch != children.end() && !ch->second.empty();
                    if (!has_kids) {
                        render_class_row(c, full_of(c), name_of(c));
                        return;
                    }
                    ImGui::PushID(c);
                    const auto hdr = name_of(c) + "  (" + std::to_string(ch->second.size()) + ")";
                    const bool open = ImGui::TreeNodeEx(hdr.c_str(),
                        ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick);
                    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                        if (std::find(m_open_class_inspectors.begin(), m_open_class_inspectors.end(), c)
                                == m_open_class_inspectors.end()) {
                            m_open_class_inspectors.push_back(c);
                        }
                    }
                    if (ImGui::BeginDragDropSource()) {
                        auto uc = c;
                        ImGui::SetDragDropPayload("UEVR_UClass", &uc, sizeof(uc));
                        ImGui::Text("UClass: %s", utility::narrow(full_of(c)).c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (open) {
                        for (auto* k : ch->second) { self(self, k); }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                };

                if (roots.empty()) {
                    ImGui::TextDisabled(m_sorted_classes.empty()
                        ? "class list not yet populated — sort task may still be running..."
                        : "no matches for filter");
                } else {
                    for (auto* r : roots) { draw(draw, r); }
                }
            }
            ImGui::EndChild();
        };

        if (ImGui::BeginTabBar("ClassesSubTabs")) {
            // By Package is the default (first) view per feedback.
            if (ImGui::BeginTabItem("By Package")) {
                render_class_tree();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("By Class")) {
                render_class_hierarchy();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Native (/Script/...)")) {
                render_class_list("native_class_list", true);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Blueprints (/Game/...)")) {
                render_class_list("bp_class_list", false);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }

    // ---- All Objects tab ---------------------------------------------------
    // (see pump_all_objects_cache above the tab body for the caching rationale)
    // Every live UObject tracked in m_objects (not just one class' instances like the class
    // inspector's Instances tab). CDOs and Blueprint GEN_VARIABLE default-value holder objects are
    // usually noise here (there's one CDO per class, and GEN_VARIABLE objects are Blueprint-internal
    // template storage) — hidden by default via name-substring, since there's no reflected object-flag
    // accessor exposed to check RF_ClassDefaultObject directly.
    if (ImGui::BeginTabItem("All Objects")) {
        ImGui::Checkbox("Hide default objects (CDOs)", &m_all_objects_hide_default);
        ImGui::SameLine();
        ImGui::Checkbox("Hide GEN_VARIABLE objects", &m_all_objects_hide_gen_variable);

        pump_all_objects_cache();

        if (ImGui::Button("Refresh##all_objects")) {
            m_all_objects_force_refresh = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Rebuild the list now.\n"
                              "It otherwise rebuilds only when the tracked object count changes"
                              " (or on the timer, if auto-refresh is on),\n"
                              "because re-deriving every object's name each frame contends with the"
                              " game thread's object hooks.");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto##all_objects", &m_all_objects_auto_refresh);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Also rebuild periodically, so renames/repossessions that don't change the\n"
                              "object COUNT still show up. Off = count-change and manual only.");
        }
        if (m_all_objects_auto_refresh) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("##all_objects_interval", &m_all_objects_refresh_seconds, 0.25f, 15.0f, "%.2fs");
        }
        ImGui::SameLine();
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_all_objects_last_refresh).count();
        ImGui::TextDisabled("%zu objects  (%.1fs old)", m_all_objects_cache.size(), (double)age / 1000.0);

        if (ImGui::BeginChild("all_objects_list", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            drag_scroll_current_window();
            // No lock and no name derivation here — everything below reads the cache built by
            // pump_all_objects_cache. exists_unsafe is still checked per row because the cache can go
            // stale between rebuilds (that's the deliberate trade), and a freed pointer must never
            // reach ui_handle_object.
            int shown = 0;
            int matched = 0;
            int dead = 0;
            const int kCap = 5000;
            // The filter compares narrow strings now (the cache is narrow), so use the narrow filter
            // text rather than the wide one the other tabs use.
            std::string nfilter = m_class_browser_filter;
            for (auto& row : m_all_objects_cache) {
                if (row.obj == nullptr || !this->exists_unsafe(row.obj)) { ++dead; continue; }
                if (m_all_objects_hide_default && row.full.find("Default__") != std::string::npos) continue;
                if (m_all_objects_hide_gen_variable && row.full.find("GEN_VARIABLE") != std::string::npos) continue;
                if (!nfilter.empty() && row.full.find(nfilter) == std::string::npos) continue;
                ++matched;
                if (shown >= kCap) continue;
                ++shown;

                auto* obj = (sdk::UObject*)row.obj;
                ImGui::PushID((void*)obj);
                const bool node_open = ImGui::TreeNode(row.short_label.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", row.full.c_str());
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("UEVR_UObject", &obj, sizeof(obj));
                    ImGui::Text("UObject: %s", row.full.c_str());
                    ImGui::EndDragDropSource();
                }
                if (node_open) {
                    ui_handle_object(obj);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (matched > kCap) {
                ImGui::TextDisabled("(truncated at %d of %d matches — narrow with the filter)", kCap, matched);
            } else if (shown == 0) {
                ImGui::TextDisabled(!nfilter.empty() ? "no matches for filter" : "(no live objects tracked)");
            }
            // Objects destroyed since the last rebuild. Surfaced rather than silently skipped so a
            // stale cache is visible as staleness instead of looking like the list is just wrong.
            if (dead > 0) {
                ImGui::TextDisabled("(%d cached object%s destroyed since last refresh)", dead, dead == 1 ? "" : "s");
            }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // ---- ScriptStructs tab -------------------------------------------------
    if (ImGui::BeginTabItem("ScriptStructs")) {
        static const auto script_struct_class = sdk::UScriptStruct::static_class();

        // One UScriptStruct row: expandable TreeNode (fields via ui_handle_struct)
        // + a drag source publishing it as a UObject. `display` = compact leaf name,
        // `full_narrow` = full path for the drag tooltip.
        auto render_struct_row = [&](sdk::UObject* obj, const std::string& display, const std::string& full_narrow) {
            ImGui::PushID(obj);
            const bool node_open = ImGui::TreeNode((void*)obj, "%s", display.c_str());
            if (ImGui::BeginDragDropSource()) {
                // UScriptStruct is a UObject, publish as UObject so generic Object drop
                // targets accept it. Struct-drop targets can sniff is_a(UScriptStruct).
                ImGui::SetDragDropPayload("UEVR_UObject", &obj, sizeof(obj));
                ImGui::Text("UScriptStruct: %s", full_narrow.c_str());
                ImGui::EndDragDropSource();
            }
            if (node_open) {
                try { ui_handle_struct(nullptr, (sdk::UStruct*)obj); }
                catch (...) { ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<failed to display struct>"); }
                ImGui::TreePop();
            }
            ImGui::PopID();
        };

        // Single FUObjectArray pass collecting every UScriptStruct with its package
        // path + leaf, so the By-Package tree and Flat list share one walk. Capped to
        // keep the per-frame scan bounded.
        struct SSItem { sdk::UObject* obj; std::string full_narrow; std::string leaf; std::string pkg; };
        std::vector<SSItem> items;
        bool truncated = false;
        if (script_struct_class == nullptr) {
            ImGui::Text("UScriptStruct::static_class() returned null");
        } else {
            auto arr = sdk::FUObjectArray::get();
            const auto count = arr ? arr->get_object_count() : 0;
            for (int32_t i = 0; i < count; ++i) {
                if (items.size() >= 5000) { truncated = true; break; }
                auto item = arr->get_object(i);
                if (item == nullptr || item->get_object() == nullptr) continue;
                auto obj = (sdk::UObject*)item->get_object();
                auto cls = obj->get_class();
                if (cls == nullptr || !cls->is_a(script_struct_class)) continue;
                std::wstring full;
                try { full = obj->get_full_name(); } catch (...) { continue; }
                if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                const auto narrow = utility::narrow(full);
                // Drop the leading "ScriptStruct " class token, then split path into
                // package (before first '.') and leaf (after last '.').
                const auto sp = narrow.find(' ');
                const auto path = (sp != std::string::npos) ? narrow.substr(sp + 1) : narrow;
                const auto last_dot = path.find_last_of('.');
                const auto leaf = (last_dot != std::string::npos) ? path.substr(last_dot + 1) : path;
                const auto pkg_dot = path.find('.');
                const auto pkg = (pkg_dot != std::string::npos) ? path.substr(0, pkg_dot) : path;
                items.push_back({obj, narrow, leaf, pkg});
            }
        }

        ImGui::TextDisabled("%zu UScriptStructs%s", items.size(),
            truncated ? " (capped at 5000 — narrow filter)" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Export structs##ss")) {
            std::map<std::string, std::vector<std::string>> by_package;
            for (const auto& it : items) {
                const auto pkg = sdk_package_of(it.full_narrow);
                try { by_package[pkg].push_back(sdk_dump_ustruct(dump_fmt, reinterpret_cast<sdk::UStruct*>(it.obj), false)); }
                catch (...) {}
            }
            sdk_write_split(dump_fmt, "scriptstructs", by_package);
        }

        if (ImGui::BeginTabBar("ScriptStructSubTabs")) {
            // By Package is the default view (matches the Classes tab).
            if (ImGui::BeginTabItem("By Package")) {
                if (ImGui::BeginChild("ss_tree", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                    drag_scroll_current_window();
                    // Nest by package path segments ("/Script/Engine" -> Script > Engine),
                    // same shape as the Classes "By Package" tree. Node IDs come from the
                    // PushID stack so identical segment names under different parents
                    // never collide.
                    struct Node {
                        std::map<std::string, Node> children;
                        std::vector<const SSItem*> leaves;
                        int count = 0;
                    };
                    Node root;
                    for (const auto& it : items) {
                        Node* cur = &root;
                        ++cur->count;
                        size_t start = 0;
                        while (start < it.pkg.size()) {
                            if (it.pkg[start] == '/') { ++start; continue; }
                            const auto end = it.pkg.find('/', start);
                            const auto seg = it.pkg.substr(start, (end == std::string::npos ? it.pkg.size() : end) - start);
                            cur = &cur->children[seg];
                            ++cur->count;
                            if (end == std::string::npos) break;
                            start = end + 1;
                        }
                        cur->leaves.push_back(&it);
                    }
                    auto draw_node = [&](auto&& self, Node& n) -> void {
                        for (auto& [seg, child] : n.children) {
                            ImGui::PushID(seg.c_str());
                            const auto hdr = seg + "  (" + std::to_string(child.count) + ")";
                            if (ImGui::TreeNode(hdr.c_str())) {
                                self(self, child);
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }
                        for (const auto* it : n.leaves) {
                            render_struct_row(it->obj, it->leaf, it->full_narrow);
                        }
                    };
                    if (root.count == 0) {
                        ImGui::TextDisabled(has_filter ? "no matches for filter" : "no UScriptStructs found");
                    } else {
                        draw_node(draw_node, root);
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Flat")) {
                if (ImGui::BeginChild("ss_flat", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                    drag_scroll_current_window();
                    for (const auto& it : items) {
                        render_struct_row(it.obj, it.leaf, it.full_narrow);
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }

    // ---- Enums tab ---------------------------------------------------------
    if (ImGui::BeginTabItem("Enums")) {
        static const auto enum_class = sdk::find_uobject<sdk::UClass>(L"Class /Script/CoreUObject.Enum");
        ImGui::TextDisabled("walks FUObjectArray looking for UEnum instances");
        ImGui::SameLine();
        if (enum_class != nullptr && ImGui::SmallButton("Export enums##enums")) {
            std::map<std::string, std::vector<std::string>> by_package;
            auto arr = sdk::FUObjectArray::get();
            const auto count = arr ? arr->get_object_count() : 0;
            for (int32_t i = 0; i < count; ++i) {
                auto item = arr->get_object(i);
                if (item == nullptr || item->get_object() == nullptr) continue;
                auto obj = (sdk::UObject*)item->get_object();
                auto cls = obj->get_class();
                if (cls == nullptr || !cls->is_a(enum_class)) continue;
                std::wstring full;
                try { full = obj->get_full_name(); } catch (...) { continue; }
                if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                const auto pkg = sdk_package_of(utility::narrow(full));
                try { by_package[pkg].push_back(sdk_dump_uenum(dump_fmt, (sdk::UEnum*)obj)); } catch (...) {}
            }
            sdk_write_split(dump_fmt, "enums", by_package);
        }
        if (ImGui::BeginChild("enum_list", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            drag_scroll_current_window();
            if (enum_class == nullptr) {
                ImGui::Text("CoreUObject.Enum not found");
            } else {
                auto arr = sdk::FUObjectArray::get();
                const auto count = arr ? arr->get_object_count() : 0;
                int shown = 0;
                for (int32_t i = 0; i < count && shown < 5000; ++i) {
                    auto item = arr->get_object(i);
                    if (item == nullptr || item->get_object() == nullptr) continue;
                    auto obj = (sdk::UObject*)item->get_object();
                    auto cls = obj->get_class();
                    if (cls == nullptr || !cls->is_a(enum_class)) continue;
                    std::wstring full;
                    try { full = obj->get_full_name(); } catch (...) { continue; }
                    if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                    const auto narrow = utility::narrow(full);
                    ImGui::PushID(obj);
                    const bool node_open = ImGui::TreeNode(narrow.c_str());
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("UEVR_UObject", &obj, sizeof(obj));
                        ImGui::Text("UEnum: %s", narrow.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (node_open) {
                        // Resolve enumerator names lazily on first expand and cache
                        // them — get_names() does up to ~512 process_event calls, far
                        // too expensive to run every frame.
                        static std::unordered_map<sdk::UEnum*, std::vector<std::pair<std::string, int64_t>>> s_enum_name_cache;
                        auto uenum = (sdk::UEnum*)obj;
                        auto cached = s_enum_name_cache.find(uenum);
                        if (cached == s_enum_name_cache.end()) {
                            std::vector<std::pair<std::string, int64_t>> names{};
                            try { names = uenum->get_names(); } catch (...) {}
                            cached = s_enum_name_cache.emplace(uenum, std::move(names)).first;
                        }
                        if (cached->second.empty()) {
                            ImGui::TextDisabled("(no enumerator names resolved)");
                        } else {
                            for (const auto& [n, v] : cached->second) {
                                ImGui::Text("%s = %lld", n.c_str(), (long long)v);
                            }
                        }
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                    ++shown;
                }
                if (shown == 5000) ImGui::Text("(truncated at 5000 — narrow your filter)");
            }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // ---- Functions tab -----------------------------------------------------
    if (ImGui::BeginTabItem("Functions")) {
        static const auto func_class = sdk::UFunction::static_class();
        ImGui::TextDisabled("walks FUObjectArray looking for UFunction instances (very large; filter recommended)");
        if (ImGui::BeginChild("fn_list", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            drag_scroll_current_window();
            auto arr = sdk::FUObjectArray::get();
            const auto count = arr ? arr->get_object_count() : 0;
            int shown = 0;
            const int kFnCap = has_filter ? 5000 : 500;
            for (int32_t i = 0; i < count && shown < kFnCap; ++i) {
                auto item = arr->get_object(i);
                if (item == nullptr || item->get_object() == nullptr) continue;
                auto obj = (sdk::UObject*)item->get_object();
                auto cls = obj->get_class();
                if (cls == nullptr || !cls->is_a(func_class)) continue;
                std::wstring full;
                try { full = obj->get_full_name(); } catch (...) { continue; }
                if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                const auto narrow = utility::narrow(full);
                auto* as_func = (sdk::UFunction*)obj;
                ImGui::PushID(obj);
                ImGui::Selectable(narrow.c_str());
                ui_function_context_menu(as_func, nullptr, false); // Block/Monitor/flags — no target object here
                if (is_func_blocked(as_func)) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4{1.0f, 0.5f, 0.0f, 1.0f}, "[Blocked]");
                }
                if (is_func_monitored(as_func)) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "[Mon %llu]", (unsigned long long)func_call_count(as_func));
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("UEVR_UObject", &obj, sizeof(obj));
                    ImGui::Text("UFunction: %s", narrow.c_str());
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
                ++shown;
            }
            if (shown == kFnCap) ImGui::Text("(truncated at %d — narrow your filter)", kFnCap);
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
}

// Dedicated dockable inspector window for one UClass. The Class Browser
// pushes a class onto m_open_class_inspectors when the user clicks a row;
// on_frame iterates the list and calls this for each. Closing the window
// (clicking the X) removes the class from the list at the end of the
// function so the window vanishes next frame.
//
// Layout:
//   - title: class full name (truncated) + ###ptr for ImGui ID uniqueness
//   - top bar: parent class chain (clickable links spawn more inspectors)
//   - tab bar: Default Object | Properties | Functions | Raw
// The Default / Properties / Functions tabs reuse the existing ui_handle_*
// helpers so behaviour matches what Objects-by-Class shows for the CDO.
void UObjectHook::draw_class_inspector_window(sdk::UClass* cls) {
    if (cls == nullptr) return;

    uobjecthook_dock_into_host_once();

    // Resolve display name + window ID. The ###ptr suffix is what ImGui
    // actually keys the window on, so two classes with identical short names
    // (e.g. multiple "Default__SomethingPostProcess_C") still get distinct
    // windows + dock slots.
    std::string title;
    {
        std::shared_lock _{m_mutex};
        auto it = m_meta_objects.find(cls);
        if (it != m_meta_objects.end() && it->second != nullptr) {
            title = utility::narrow(it->second->full_name);
        } else {
            // Meta missing (class showed up after sort, or was evicted). Fall
            // back to a raw name lookup so the inspector still renders.
            try { title = utility::narrow(cls->get_full_name()); }
            catch (...) { title = "<invalid class>"; }
        }
    }
    char id_buf[256]{};
    std::snprintf(id_buf, sizeof(id_buf), "Class Inspector: %s###cls_insp_%p",
        title.c_str(), (void*)cls);

    bool open = true;
    if (!ImGui::Begin(id_buf, &open)) {
        ImGui::End();
        if (!open) {
            std::erase(m_open_class_inspectors, cls);
        }
        return;
    }
    utility::ScopeGuard end_guard{[]() { ImGui::End(); }};

    // Defend against a non-class pointer (stale/freed entry, or an instance that
    // slipped in): rendering it as a UStruct iterates garbage child/super chains
    // and can leave the ImGui ID stack unbalanced ("Missing PopID()"). Bail early.
    static const auto class_class = sdk::UClass::static_class();
    if (!this->exists_unsafe((sdk::UObject*)cls) || cls->get_class() == nullptr
            || class_class == nullptr || !cls->get_class()->is_a(class_class)) {
        ImGui::TextColored(ImVec4{1.0f, 0.4f, 0.4f, 1.0f}, "Not a valid UClass (stale or non-class entry) — close this window.");
        if (!open) {
            std::erase(m_open_class_inspectors, cls);
        }
        return;
    }

    // Header: full name + parent class chain. Each parent is a clickable
    // Selectable that opens another inspector — lets the user walk up the
    // hierarchy without going back to the Class Browser.
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    {
        ImGui::TextDisabled("parent chain:");
        ImGui::SameLine();
        bool any = false;
        for (auto super = cls->get_super_struct(); super != nullptr; super = super->get_super_struct()) {
            std::string sname;
            {
                std::shared_lock _{m_mutex};
                auto sit = m_meta_objects.find(super);
                if (sit != m_meta_objects.end() && sit->second != nullptr) {
                    sname = utility::narrow(sit->second->full_name);
                }
            }
            if (sname.empty()) {
                try { sname = utility::narrow(super->get_full_name()); } catch (...) { continue; }
            }
            // Show just the short tail to keep the chain line readable.
            const auto dot = sname.find_last_of('.');
            const auto short_name = (dot != std::string::npos) ? sname.substr(dot + 1) : sname;
            if (any) { ImGui::SameLine(); ImGui::TextUnformatted("→"); ImGui::SameLine(); }
            any = true;
            ImGui::PushID((void*)super);
            // Only UClass* supers should be openable as inspectors. We can't
            // safely cast every UStruct to UClass — UScriptStruct supers
            // would be wrong — so we test class identity first.
            static const auto class_class = sdk::UClass::static_class();
            const bool is_uclass = super->get_class() != nullptr && super->get_class()->is_a(class_class);
            if (is_uclass) {
                if (ImGui::SmallButton(short_name.c_str())) {
                    auto* super_class = (sdk::UClass*)super;
                    if (std::find(m_open_class_inspectors.begin(), m_open_class_inspectors.end(), super_class)
                            == m_open_class_inspectors.end()) {
                        m_open_class_inspectors.push_back(super_class);
                    }
                }
            } else {
                ImGui::TextUnformatted(short_name.c_str());
            }
            ImGui::PopID();
        }
        if (!any) {
            ImGui::TextDisabled("(none)");
        }
    }
    ImGui::Separator();

    if (!ImGui::BeginTabBar("ClassInspectorTabs")) {
        if (!open) std::erase(m_open_class_inspectors, cls);
        return;
    }
    utility::ScopeGuard tab_guard{[]() { ImGui::EndTabBar(); }};

    // ---- Properties tab (structural, no CDO read) -------------------------
    if (ImGui::BeginTabItem("Properties")) {
        ImGui::TextDisabled("structural FProperty list (no live values)");
        int count = 0;
        for (auto super = (sdk::UStruct*)cls; super != nullptr; super = super->get_super_struct()) {
            for (auto field = super->get_child_properties(); field != nullptr; field = field->get_next()) {
                auto pclass = field->get_class();
                if (pclass == nullptr) continue;
                const auto type_name = utility::narrow(pclass->get_name().to_string());
                if (!type_name.contains("Property")) continue;
                const auto field_name = utility::narrow(field->get_field_name().to_string());
                ImGui::BulletText("%s %s", type_name.c_str(), field_name.c_str());
                ++count;
            }
        }
        if (count == 0) {
            ImGui::TextDisabled("(no FProperties)");
        }
        ImGui::EndTabItem();
    }

    // ---- Functions tab ----------------------------------------------------
    if (ImGui::BeginTabItem("Functions")) {
        // Pass nullptr as object so ui_handle_functions only shows signatures
        // (the Call UI is gated on is_real_object). To actually invoke, the
        // user drags a target into Function Caller.
        ui_handle_functions(nullptr, cls);
        ImGui::EndTabItem();
    }

    // ---- Instances tab ----------------------------------------------------
    // Live UObjects of this class, pulled from m_objects_by_class (the same
    // map Objects-by-Class iterates). Each row is a drag source for the
    // generic UEVR_UObject payload — drop into a Function Caller slot, the
    // motion-controller attach widget, anywhere else that accepts an object.
    // Selecting also pops the address into the picker_text_buffer so the
    // text-input lookups elsewhere can refer to it.
    if (ImGui::BeginTabItem("Instances")) {
        std::shared_lock _{m_mutex};
        auto it = m_objects_by_class.find(cls);
        if (it == m_objects_by_class.end() || it->second.empty()) {
            ImGui::TextDisabled("(no live instances tracked for this class)");
            ImGui::TextDisabled("Note: only objects created after the UObjectHook attached are listed.");
        } else {
            const auto& set = it->second;
            ImGui::TextDisabled("%zu live instances — drag into a UObject slot to use", set.size());
            if (ImGui::BeginChild("instance_list", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
                drag_scroll_current_window();
                // Snapshot pointers into a vector for stable indexing — the
                // set itself can be mutated by the UObjectBase hook on any
                // thread, even with shared_lock held (since the hook takes
                // unique_lock and waits).
                std::vector<sdk::UObjectBase*> snapshot;
                snapshot.reserve(set.size());
                for (auto* obj : set) snapshot.push_back(obj);
                int shown = 0;
                const int kCap = 5000;
                for (auto* base_obj : snapshot) {
                    if (base_obj == nullptr) continue;
                    if (shown >= kCap) {
                        ImGui::TextDisabled("(truncated at %d — narrow by class)", kCap);
                        break;
                    }
                    auto* obj = (sdk::UObject*)base_obj;
                    std::string name;
                    auto meta_it = m_meta_objects.find(base_obj);
                    if (meta_it != m_meta_objects.end() && meta_it->second != nullptr) {
                        name = utility::narrow(meta_it->second->full_name);
                    } else {
                        try { name = utility::narrow(obj->get_full_name()); }
                        catch (...) { continue; }
                    }
                    ImGui::PushID((void*)obj);
                    // Expand a live instance into its own full editor (properties +
                    // function calling on THIS object, not the class default).
                    const bool node_open = ImGui::TreeNode(name.c_str());
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("UEVR_UObject", &obj, sizeof(obj));
                        ImGui::Text("UObject: %s", name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (node_open) {
                        ui_handle_object(obj);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                    ++shown;
                }
            }
            ImGui::EndChild();
        }
        ImGui::EndTabItem();
    }

    if (!open) {
        std::erase(m_open_class_inspectors, cls);
    }
}
