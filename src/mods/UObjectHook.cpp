#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
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
#include <sdk/FMalloc.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/USceneComponent.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/APawn.hpp>
#include <sdk/APlayerCameraManager.hpp>
#include <sdk/ScriptVector.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/FObjectProperty.hpp>
#include <sdk/FArrayProperty.hpp>
#include <sdk/FMapProperty.hpp>
#include <sdk/FSetProperty.hpp>
#include <sdk/UMotionControllerComponent.hpp>
#include <sdk/UTexture.hpp>
#include <sdk/StereoStuff.hpp>

#include <imgui_internal.h>
#include "uobjecthook/SDKDumper.hpp"
#include "VR.hpp"
#include "PluginLoader.hpp"
#include "LuaLoader.hpp"
#include <d3d11.h>

#include "UObjectHook.hpp"

#define VERBOSE_UOBJECTHOOK


// ---------------------------------------------------------------------------
// Interactive function-caller helpers
//
// The previous ui_handle_functions only knew how to call (a) zero-arg
// functions, (b) functions with exactly one bool / int / string parameter, and
// hard-coded the string value to "Hello world!". Anything else just listed the
// parameter names with no editor and no Call button.
//
// The helpers in this block render a per-parameter editor for the common
// property types and a Call button that assembles a parameter buffer of the
// correct size from the property offsets. UObject* / UClass* parameters
// appear as drag-and-drop targets that accept payloads dropped from the
// matching TreeNode label elsewhere in the UObjectHook view.
// ---------------------------------------------------------------------------

namespace {

#pragma region new_gui
// Payload type identifiers used by ImGui's drag-and-drop machinery. Anything
// elsewhere in UObjectHook can publish a payload of the same type and the
// function-caller target will pick it up.
constexpr const char* kDragPayloadUObject = "UEVR_UObject";
constexpr const char* kDragPayloadUClass  = "UEVR_UClass";

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
inline void drag_scroll_current_window() {
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

// Put a string on the Win32 clipboard as CF_TEXT. Shared by the object-label copy actions.
inline void copy_text_to_clipboard(const std::string& s) {
    if (!OpenClipboard(nullptr)) {
        return;
    }
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_DDESHARE, s.size() + 1); h != nullptr) {
        if (char* d = (char*)GlobalLock(h); d != nullptr) {
            std::memcpy(d, s.c_str(), s.size() + 1);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
    }
    CloseClipboard();
}

// Shorten a UObject full name to a compact, readable label by dropping the long package/outer path.
// UE get_full_name() is "ClassName /Package/Path.Outer:Leaf". Used to show "ClassName Leaf", but UE's
// default object naming is "<ClassName><N>" (e.g. "StaticMeshComponent StaticMeshComponent0"), which
// reads as a near-duplicate. Shows "Parent Leaf" instead (the object's immediate outer + its own name)
// — the last two path segments split on '.', ':' or '/' — so entries read like "MyActor
// StaticMeshComponent0" instead. The FULL name is still what search matches and what copy/tooltip use —
// this only affects the on-screen text. Falls back to just the object name when there's no parent
// segment (top-level object).
inline std::string shorten_object_path(std::string_view full) {
    if (full.empty()) {
        return std::string{full};
    }

    const size_t sp = full.find(' '); // get_full_name() = "Class Path"
    const std::string_view path = (sp == std::string_view::npos) ? full : full.substr(sp + 1);

    std::vector<std::string_view> segs;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '.' || path[i] == ':' || path[i] == '/') {
            if (i > start) segs.push_back(path.substr(start, i - start));
            start = i + 1;
        }
    }

    if (segs.empty()) {
        return std::string{path};
    }
    if (segs.size() == 1) {
        return std::string{segs.back()};
    }

    const std::string_view parent = segs[segs.size() - 2];
    const std::string_view object = segs.back();
    std::string out;
    out.reserve(parent.size() + 1 + object.size());
    out.append(parent);
    out.push_back(' ');
    out.append(object);
    return out;
}

// Render a compact object label as wrapped text: shows the shortened form, hovering shows the FULL name
// as a tooltip, right-click copies the FULL name. `full` is the untruncated get_full_name() result
// (optionally with a leading "[0xADDR] " which is preserved on the short form). Display-only.
inline void ui_object_label_compact(const std::string& full,
                                    const ImVec4& color = ImVec4{0.7f, 0.7f, 0.7f, 1.0f}) {
    if (full.empty()) {
        return;
    }

    // Preserve a leading "[0x...] " address prefix (some call sites prepend one) and shorten the rest.
    std::string prefix;
    std::string_view body{full};
    if (!body.empty() && body.front() == '[') {
        if (const size_t rb = body.find("] "); rb != std::string_view::npos) {
            prefix = std::string{body.substr(0, rb + 2)};
            body = body.substr(rb + 2);
        }
    }
    const std::string shortl = prefix + shorten_object_path(body);

    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", shortl.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n\n(right-click to copy full name)", full.c_str());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            copy_text_to_clipboard(full);
        }
    }
}

// Right-click Copy/Paste for a vector/quat widget. Call immediately AFTER the
// DragFloatN so the context menu binds to it. Copy writes "x, y, z[, w]" to the
// clipboard; Paste parses that many floats back (tolerant of commas / () / []).
// Returns true when a paste actually wrote new values.
inline bool vector_copy_paste(const char* popup_id, float* v, int n) {
    bool pasted = false;
    if (ImGui::BeginPopupContextItem(popup_id)) {
        if (ImGui::MenuItem("Copy")) {
            std::stringstream ss;
            for (int i = 0; i < n; ++i) {
                if (i != 0) ss << ", ";
                ss << v[i];
            }
            ImGui::SetClipboardText(ss.str().c_str());
        }
        if (ImGui::MenuItem("Paste")) {
            if (const char* c = ImGui::GetClipboardText(); c != nullptr) {
                std::string s{c};
                for (auto& ch : s) {
                    if (ch == ',' || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ';') {
                        ch = ' ';
                    }
                }
                std::stringstream ss{s};
                float tmp[4]{};
                int got = 0;
                while (got < n && (ss >> tmp[got])) {
                    ++got;
                }
                if (got == n) {
                    for (int i = 0; i < n; ++i) v[i] = tmp[i];
                    pasted = true;
                }
            }
        }
        ImGui::EndPopup();
    }
    return pasted;
}

// Per-slot state for the universal object picker widget (T63). One per
// drop/text/pick slot so the text buffer, popup filter, and the two display
// toggles survive across frames independently.
struct PickerState {
    std::string text;            // short-name / 0xADDR text-input buffer
    std::string filter;          // substring filter inside the "pick..." popup
    bool list_classes = false;   // popup lists UClass objects instead of instances
    bool use_type_filter = true; // popup applies the expected-class IsA filter
};

// Per-(function, object) state for the function-caller widget. Keys are the
// raw function/object pointers; the entry sticks around until the address is
// reused — acceptable for a debug menu.
struct ParamEditState {
    // Scratch fields. For each property field name we keep typed scratch so
    // the editor can survive frames without re-parsing strings.
    std::unordered_map<std::string, bool>     bools;
    std::unordered_map<std::string, int>      ints;
    std::unordered_map<std::string, int64_t>  int64s;
    std::unordered_map<std::string, uint64_t> uint64s;
    std::unordered_map<std::string, float>    floats;
    std::unordered_map<std::string, double>   doubles;
    std::unordered_map<std::string, std::string>     text;   // narrow buffer, edited via InputText
    std::unordered_map<std::string, std::wstring>    wtext;  // committed wide form, kept alive while we point an FString at it
    std::unordered_map<std::string, sdk::UObject*>   objs;   // ObjectProperty / InterfaceProperty slot
    std::unordered_map<std::string, sdk::UClass*>    classes;// ClassProperty slot
    std::unordered_map<std::string, glm::vec4>       vec;    // struct scratch (xyz[w]) for FVector / FRotator / FVector2D / FQuat
    // Generic struct scratch (T64): one double per reflected scalar leaf, in the
    // shared walk order produced by collect_struct_leaves(). Handles any struct
    // whose leaves are all numeric — FVector/FRotator/FQuat/FTransform/FColor/
    // FIntPoint/FMatrix/... — without hardcoding per-engine offsets.
    std::unordered_map<std::string, std::vector<double>> struct_scratch;
    // Generic struct scratch for NON-numeric struct params (members that are Object/Name/Str/Enum/
    // Array/...): a raw byte buffer sized to the UScriptStruct, edited in place by ui_handle_struct
    // (the same generic per-member editor used for live objects). encode_param memcpys it into the
    // params blob. Keyed by prop name so each struct param keeps its own buffer across frames.
    std::unordered_map<std::string, std::vector<uint8_t>> struct_bytes;
    // Per-slot universal-picker state for UObject-typed params (T63).
    std::unordered_map<std::string, PickerState>     pickers;
    // Inline-Lua fallback (T64): editable snippet + lazy-init flag, run via
    // PluginLoader::do_lua_string for params the native encoder can't pack.
    std::string lua_snippet;
    bool        lua_snippet_init = false;
    // Text-input fallback for drop targets (Object / Class slots) — see
    // render_object_text_input / render_class_text_input. Keyed by prop name
    // so each ObjectProperty has its own buffer that survives frames.
    std::unordered_map<std::string, std::string>     obj_text;
    std::unordered_map<std::string, std::string>     cls_text;
    // Search-filter text inside the "pick..." popup for UObject params, keyed
    // by prop name so each slot remembers its filter while open.
    std::unordered_map<std::string, std::string>     picker_filter;
    std::string return_repr{"<no call yet>"};
    std::string error_repr{};
    // If the last call returned an Object/Interface/Class property, also
    // stash the raw pointer so the result display can act as a drag SOURCE
    // (you can drop the returned UObject into another caller slot or any
    // other ObjectProperty drop target without re-finding it through the
    // tree).
    sdk::UObject* return_obj{nullptr};
    sdk::UClass*  return_class{nullptr};
};

struct ParamKey {
    sdk::UFunction* fn{};
    void* self{};
    bool operator==(const ParamKey& o) const { return fn == o.fn && self == o.self; }
};
struct ParamKeyHash {
    size_t operator()(const ParamKey& k) const noexcept {
        return std::hash<void*>{}(k.fn) ^ (std::hash<void*>{}(k.self) << 1);
    }
};

ParamEditState& get_param_state(sdk::UFunction* fn, void* self) {
    static std::unordered_map<ParamKey, ParamEditState, ParamKeyHash> s_state;
    return s_state[{fn, self}];
}

// Attach a drag-source to the *previous* item, carrying an FObject pointer.
// Call this right after `ImGui::TreeNode(label)` (or other selectable widget)
// to make that node draggable.
void make_drag_source_for_object(sdk::UObject* obj, const char* label) {
    if (obj == nullptr) return;
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload(kDragPayloadUObject, &obj, sizeof(obj));
        ImGui::Text("UObject: %s", label != nullptr ? label : "<anon>");
        ImGui::EndDragDropSource();
    }
}

void make_drag_source_for_class(sdk::UClass* c, const char* label) {
    if (c == nullptr) return;
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload(kDragPayloadUClass, &c, sizeof(c));
        ImGui::Text("UClass: %s", label != nullptr ? label : "<anon>");
        ImGui::EndDragDropSource();
    }
}

// Drop target that returns the dropped pointer on the frame it lands,
// otherwise nullptr. Caller is responsible for placing a target widget
// (Button / Selectable / Text) immediately before calling this.
sdk::UObject* accept_object_drop() {
    if (!ImGui::BeginDragDropTarget()) {
        return nullptr;
    }
    sdk::UObject* result = nullptr;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayloadUObject)) {
        if (p->DataSize == (int)sizeof(sdk::UObject*)) {
            result = *reinterpret_cast<sdk::UObject**>(p->Data);
        }
    }
    ImGui::EndDragDropTarget();
    return result;
}

sdk::UClass* accept_class_drop() {
    if (!ImGui::BeginDragDropTarget()) {
        return nullptr;
    }
    sdk::UClass* result = nullptr;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayloadUClass)) {
        if (p->DataSize == (int)sizeof(sdk::UClass*)) {
            result = *reinterpret_cast<sdk::UClass**>(p->Data);
        }
    }
    ImGui::EndDragDropTarget();
    return result;
}

// -----------------------------------------------------------------------------
// Typed-query resolvers (text-input alternative to drag-and-drop).
//
// Three accepted forms, in priority order:
//   1. "0xADDR" / "ADDR"        — hex pointer cast directly. Caller-trusted;
//                                  we do NOT validate via FUObjectArray scan
//                                  (the user knows what they typed and an
//                                  exhaustive scan is expensive). Bad input
//                                  → wild pointer → caller's problem.
//   2. "Class /Script/Foo.Bar"  — full UE name with a space. Routed through
//                                  sdk::find_uobject which already caches.
//   3. "Bar"                    — short name. Linear scan of FUObjectArray
//                                  comparing each object's FName. First hit
//                                  wins (sorted to be insertion order).
//
// Mirrors the user's Lua object_lookup widget in Scripts/ImGui.lua —
// `input:to_address()` + `api:to_uobject(addr)` vs `api:find_uobject(name)`.
// -----------------------------------------------------------------------------
sdk::UObject* resolve_object_query(std::string_view query_raw) {
    std::string q{query_raw};
    while (!q.empty() && std::isspace((unsigned char)q.back())) q.pop_back();
    while (!q.empty() && std::isspace((unsigned char)q.front())) q.erase(0, 1);
    if (q.empty()) return nullptr;

    // Hex address form
    {
        std::string_view hex = q;
        if (hex.starts_with("0x") || hex.starts_with("0X")) hex.remove_prefix(2);
        if (!hex.empty() && std::all_of(hex.begin(), hex.end(), [](char c){
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) {
            try {
                auto addr = std::stoull(std::string{hex}, nullptr, 16);
                if (addr > 0x10000) {
                    return reinterpret_cast<sdk::UObject*>(addr);
                }
            } catch (...) {}
        }
    }

    // Full-name form (contains a space, e.g. "Class /Script/Engine.Character")
    if (q.find(' ') != std::string::npos) {
        return reinterpret_cast<sdk::UObject*>(sdk::find_uobject(utility::widen(q)));
    }

    const auto wq = utility::widen(q);

    // Fast path: short-name -> UClass cache. Classes are stable (never GC'd), so
    // caching them is safe and avoids the O(N) FUObjectArray scan (a get_fname
    // per object) on every keystroke-resolve of a class name.
    {
        static std::unordered_map<std::wstring, sdk::UObject*> s_class_name_cache;
        static bool s_class_cache_built = false;
        if (!s_class_cache_built) {
            s_class_cache_built = true;
            static const auto uclass_t = sdk::UClass::static_class();
            if (auto carr = sdk::FUObjectArray::get(); carr != nullptr) {
                const auto ccount = carr->get_object_count();
                for (int32_t i = 0; i < ccount; ++i) {
                    auto item = carr->get_object(i);
                    if (item == nullptr || item->get_object() == nullptr) continue;
                    auto obj = reinterpret_cast<sdk::UObject*>(item->get_object());
                    try {
                        if (auto c = obj->get_class(); c != nullptr && c->is_a(uclass_t)) {
                            s_class_name_cache.emplace(obj->get_fname().to_string(), obj);
                        }
                    } catch (...) {}
                }
            }
        }
        if (auto it = s_class_name_cache.find(wq); it != s_class_name_cache.end()) {
            return it->second;
        }
    }

    // Short-name fallback — linear scan over live objects (non-class objects and
    // any class created after the cache was built).
    auto arr = sdk::FUObjectArray::get();
    if (arr == nullptr) return nullptr;
    const auto count = arr->get_object_count();
    for (int32_t i = 0; i < count; ++i) {
        auto item = arr->get_object(i);
        if (item == nullptr || item->get_object() == nullptr) continue;
        auto obj = reinterpret_cast<sdk::UObject*>(item->get_object());
        try {
            if (obj->get_fname().to_string() == wq) {
                return obj;
            }
        } catch (...) {}
    }
    return nullptr;
}

// Same parser but requires the resolved object to be (or derive from) UClass.
// Returns nullptr if the query resolves to something that isn't a class.
sdk::UClass* resolve_class_query(std::string_view query_raw) {
    auto obj = resolve_object_query(query_raw);
    if (obj == nullptr) return nullptr;
    auto cls = obj->get_class();
    if (cls == nullptr) return nullptr;
    // is_a checks the class hierarchy; an actual UClass instance passes
    // because its meta-class derives from UClass::static_class().
    if (cls->is_a(sdk::UClass::static_class())) {
        return reinterpret_cast<sdk::UClass*>(obj);
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// Live Function Caller slot state — shared between the in-tree TreeNode (in
// UObjectHook::draw_main) and the dockable pop-out window
// (UObjectHook::draw_function_caller_window). Both surfaces render the same
// kSlotCount slots and any change in one surface is immediately visible in
// the other.
// -----------------------------------------------------------------------------
constexpr int kLiveCallerSlotCount = 2;
struct LiveSlot {
    sdk::UObject* target{};
    std::string target_text;          // text-input fallback for the target
    std::string fn_name;
    sdk::UFunction* resolved{};
    std::string resolved_label;
    std::string resolve_error;
    PickerState picker;               // universal-picker state for the target (T63)
};
static std::array<LiveSlot, kLiveCallerSlotCount> s_live_slots{};

// Forward decls — render_live_caller_slots() uses these, all defined further
// down in this TU.
void render_function_call(sdk::UObject* self, sdk::UFunction* fn);
sdk::UObject* render_object_text_input(const char* id, std::string& buf);
// Universal object picker (T63): drop target + text input + searchable popup +
// common-object quick-picks + instances/classes & type-filter toggles. Returns
// true on the frame `slot` changes. `auto_label`, when non-null and slot!=null,
// replaces the address prefix (used for the WorldContext "[auto: World]" hint).
bool render_universal_object_picker(const char* id_prefix, sdk::UObject*& slot,
                                    PickerState& st, sdk::UClass* expected_class,
                                    bool allow_send_to_caller, const char* auto_label);

void render_live_caller_slots() {
    for (int i = 0; i < kLiveCallerSlotCount; ++i) {
        ImGui::PushID(i);
        utility::ScopeGuard pop_id{[]() { ImGui::PopID(); }};

        ImGui::Text("Slot %d", i + 1);

        auto& slot = s_live_slots[i];
        // Universal picker drives the target: drop / type / pick / common-object
        // quick-picks, all in one widget shared with the per-param editor.
        if (render_universal_object_picker("slot_target", slot.target, slot.picker,
                                           nullptr, false, nullptr)) {
            // Target changed — invalidate the resolved function so the combo
            // re-resolves against the new class.
            slot.resolved = nullptr;
            slot.resolved_label.clear();
            slot.resolve_error.clear();
        }

		ImGui::Indent();

        // Function picker: ONE combo. Preview = the selected function; open it
        // for a filter box + the target's functions. Clicking one selects AND
        // resolves it — no separate text field or Resolve step.
        if (slot.target == nullptr) {
            ImGui::TextDisabled("(drop or type a target above, then pick a function)");
        } else {
            const char* preview = slot.fn_name.empty() ? "select function..." : slot.fn_name.c_str();
            static char s_fn_filter[kLiveCallerSlotCount][128]{};
            ImGui::TextUnformatted(preview); // selected function (the list box below doesn't show a preview)
            if (ImGui::BeginListBox("##function", ImVec2(-FLT_MIN, 180.0f))) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##fnfilter", "filter...", s_fn_filter[i], sizeof(s_fn_filter[i]));
                std::string needle = s_fn_filter[i];
                for (auto& ch : needle) ch = (char)std::tolower((unsigned char)ch);

                if (auto* cls = slot.target->get_class(); cls != nullptr) {
                    static const auto ufunction_t = sdk::UFunction::static_class();
                    std::vector<sdk::UFunction*> funcs{};
                    for (auto super = (sdk::UStruct*)cls; super != nullptr; super = super->get_super_struct()) {
                        for (auto child = super->get_children(); child != nullptr; child = child->get_next()) {
                            if (child->get_class()->is_a(ufunction_t)) {
                                funcs.push_back((sdk::UFunction*)child);
                            }
                        }
                    }
                    std::sort(funcs.begin(), funcs.end(), [](sdk::UFunction* a, sdk::UFunction* b) {
                        return a->get_fname().to_string() < b->get_fname().to_string();
                    });
                    for (auto* fn : funcs) {
                        auto name = utility::narrow(fn->get_fname().to_string());
                        if (!needle.empty()) {
                            std::string lname = name;
                            for (auto& ch : lname) ch = (char)std::tolower((unsigned char)ch);
                            if (lname.find(needle) == std::string::npos) {
                                continue;
                            }
                        }
                        if (ImGui::Selectable(name.c_str(), slot.fn_name == name)) {
                            slot.fn_name = name;
                            slot.resolved = fn;
                            slot.resolved_label = utility::narrow(fn->get_full_name());
                            slot.resolve_error.clear();
                        }
                    }
                }
                ImGui::EndListBox();
            }
        }

        if (!slot.resolve_error.empty()) {
            ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "%s", slot.resolve_error.c_str());
        }
        if (slot.resolved != nullptr) {
            ImGui::TextColored(ImVec4{0.4f, 1.0f, 0.4f, 1.0f}, "→ %s", slot.resolved_label.c_str());
            ImGui::Separator();
            try {
                render_function_call(slot.target, slot.resolved);
            } catch (const std::exception& e) {
                ImGui::TextColored(ImVec4{1, 0.3f, 0.3f, 1}, "render_function_call threw: %s", e.what());
            } catch (...) {
                ImGui::TextColored(ImVec4{1, 0.3f, 0.3f, 1}, "render_function_call threw (unknown)");
            }
        }

        ImGui::Unindent();
        ImGui::Separator();
    }
}

void load_live_caller_slot(sdk::UObject* obj, sdk::UFunction* fn) {
    if (obj == nullptr || fn == nullptr) {
        return;
    }

    int idx = 0;
    for (int i = 0; i < kLiveCallerSlotCount; ++i) {
        if (s_live_slots[i].resolved == nullptr) {
            idx = i;
            break;
        }
    }

    auto& slot = s_live_slots[idx];
    slot.target = obj;
    slot.resolved = fn;
    try { slot.fn_name = utility::narrow(fn->get_fname().to_string()); } catch (...) { slot.fn_name.clear(); }
    try { slot.resolved_label = utility::narrow(fn->get_full_name()); } catch (...) { slot.resolved_label.clear(); }
    slot.resolve_error.clear();
}

// Helper widget: a small InputText next to a drop target that accepts the
// same three forms above. Returns the resolved object if the user pressed
// Enter and the parse succeeded, nullptr otherwise. The input buffer is
// owned by the caller (so per-slot state survives frames).
sdk::UObject* render_object_text_input(const char* id, std::string& buf) {
    std::array<char, 256> raw{};
    const auto copy_n = std::min(buf.size(), raw.size() - 1);
    std::memcpy(raw.data(), buf.data(), copy_n);
    ImGui::SetNextItemWidth(220.0f);
    const bool entered = ImGui::InputTextWithHint(id, "or type name / 0xADDR + Enter",
        raw.data(), raw.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (std::strcmp(raw.data(), buf.c_str()) != 0) {
        buf.assign(raw.data());
    }
    if (entered) {
        if (auto obj = resolve_object_query(buf); obj != nullptr) {
            buf.clear();
            return obj;
        }
    }
    return nullptr;
}

sdk::UClass* render_class_text_input(const char* id, std::string& buf) {
    auto obj = render_object_text_input(id, buf);
    if (obj == nullptr) return nullptr;
    auto cls = obj->get_class();
    if (cls != nullptr && cls->is_a(sdk::UClass::static_class())) {
        return reinterpret_cast<sdk::UClass*>(obj);
    }
    return nullptr;
}

// Popup picker for UObject-typed function parameter slots. Walks FUObjectArray
// for any object whose class IsChildOf `expected_class`, filters by the user's
// substring search, and shows them in a ListBox. Selecting a row returns it;
// otherwise returns nullptr.
//
// `popup_id` must be the popup label OpenPopup() was called with. `filter_buf`
// is the per-slot search string from ParamEditState::picker_filter, so the
// filter persists across frames while the popup is open. `expected_class` may
// be null (untyped UObject param) — in that case we list all live UObjects.
//
// We walk FUObjectArray directly rather than reaching into UObjectHook's
// m_objects map so this stays a free helper. The 5000-row cap keeps the popup
// responsive on games with very large object tables — if a typed filter is
// active we lean more permissive (we already have a class filter cutting the
// list down significantly).
sdk::UObject* render_object_picker_popup(const char* popup_id,
                                         std::string& filter_buf,
                                         sdk::UClass* expected_class,
                                         bool list_classes = false) {
    sdk::UObject* picked = nullptr;
    if (!ImGui::BeginPopup(popup_id)) {
        return nullptr;
    }
    utility::ScopeGuard pop_guard{[]() { ImGui::EndPopup(); }};

    static const auto uclass_t = sdk::UClass::static_class();

    // Header: show what we're filtering against so the user understands the
    // smaller-than-expected list.
    if (list_classes) {
        ImGui::TextDisabled("listing UClass objects");
    } else if (expected_class != nullptr) {
        std::string cls_name;
        try { cls_name = utility::narrow(expected_class->get_full_name()); }
        catch (...) { cls_name = "<unknown>"; }
        ImGui::TextDisabled("matches IsA(%s)", cls_name.c_str());
    } else {
        ImGui::TextDisabled("untyped — all live UObjects");
    }

    // Search filter input
    std::array<char, 256> raw{};
    const auto copy_n = std::min(filter_buf.size(), raw.size() - 1);
    std::memcpy(raw.data(), filter_buf.data(), copy_n);
    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::InputTextWithHint("##picker_filter", "substring filter",
                                 raw.data(), raw.size())) {
        filter_buf.assign(raw.data());
    }
    const auto wfilter = utility::widen(filter_buf);
    const bool has_filter = !wfilter.empty();

    if (ImGui::BeginListBox("##picker_list", ImVec2(420.0f, 280.0f))) {
        drag_scroll_current_window();
        auto arr = sdk::FUObjectArray::get();
        const auto count = arr ? arr->get_object_count() : 0;
        int shown = 0;
        // Cap: more permissive when a substring filter is active because we
        // expect that to thin the list. Without any filter we want to fail
        // fast on huge object tables so the popup doesn't lock up.
        const int kCap = has_filter ? 5000 : 1500;
        for (int32_t i = 0; i < count && shown < kCap; ++i) {
            auto item = arr->get_object(i);
            if (item == nullptr || item->get_object() == nullptr) continue;
            auto obj = (sdk::UObject*)item->get_object();
            auto cls = obj->get_class();
            if (list_classes) {
                // Class mode: only objects that ARE UClasses. Type filter is
                // not applied here (it constrains instances, not metaclasses).
                if (cls == nullptr || uclass_t == nullptr || !cls->is_a(uclass_t)) continue;
            } else if (expected_class != nullptr) {
                if (cls == nullptr || !cls->is_a(expected_class)) continue;
            }
            std::wstring full;
            try { full = obj->get_full_name(); } catch (...) { continue; }
            if (has_filter && full.find(wfilter) == std::wstring::npos) continue; // search matches the FULL name
            const auto narrow = utility::narrow(full);
            const auto shortl = shorten_object_path(narrow);
            ImGui::PushID((void*)obj);
            if (ImGui::Selectable(shortl.c_str())) {
                picked = obj;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", narrow.c_str());
            }
            ImGui::PopID();
            ++shown;
            if (picked != nullptr) break; // stop early once the user clicked one
        }
        if (shown == 0) {
            ImGui::TextDisabled("(no matches)");
        } else if (shown == kCap) {
            ImGui::TextDisabled("(truncated at %d — narrow your filter)", kCap);
        }
        ImGui::EndListBox();
    }

    if (picked != nullptr) {
        // Reset the filter for next open + close the popup.
        filter_buf.clear();
        ImGui::CloseCurrentPopup();
    }
    return picked;
}

// Collect the player-centric "common objects" for the picker quick-pick row.
// Each entry is { short label, pointer }; null pointers are filtered by the
// caller. Every lookup is guarded — a half-initialised world must not crash the
// menu.
std::vector<std::pair<const char*, sdk::UObject*>> gather_common_objects() {
    // Memoize per ImGui frame — get_player_controller is a process_event call,
    // and the picker (hence this) renders once per live-caller slot + once per
    // UObject param, so an uncached call would dispatch reflected game code many
    // times per frame.
    static int s_frame = -1;
    static std::vector<std::pair<const char*, sdk::UObject*>> s_cache;
    const int frame = ImGui::GetFrameCount();
    if (frame == s_frame) {
        return s_cache;
    }
    s_frame = frame;
    s_cache.clear();
    try {
        auto engine = sdk::UGameEngine::get();
        auto world = engine != nullptr ? engine->get_world() : nullptr;
        if (world == nullptr) return s_cache;
        s_cache.emplace_back("World", (sdk::UObject*)world);
        auto pc = sdk::UGameplayStatics::get()->get_player_controller(world, 0);
        if (pc != nullptr) {
            s_cache.emplace_back("PC", (sdk::UObject*)pc);
            if (auto pawn = pc->get_acknowledged_pawn(); pawn != nullptr) {
                s_cache.emplace_back("Pawn", (sdk::UObject*)pawn);
            }
            if (auto cam = pc->get_player_camera_manager(); cam != nullptr) {
                s_cache.emplace_back("Camera", (sdk::UObject*)cam);
            }
        }
    } catch (...) {}
    return s_cache;
}

// Sub-objects reachable from a common object, for the picker dropdowns:
// World->PersistentLevel, PC->HUD, Camera(manager)->ViewTarget, Pawn->its
// Skeletal/Capsule/Camera components + root. All lookups guarded; object-pointer
// property reads return a live pointer or null.
std::vector<std::pair<std::string, sdk::UObject*>> gather_common_object_children(const std::string& kind, sdk::UObject* obj) {
    std::vector<std::pair<std::string, sdk::UObject*>> out;
    if (obj == nullptr) return out;

    auto read_obj_prop = [](sdk::UObject* o, const wchar_t* name) -> sdk::UObject* {
        if (o == nullptr) return nullptr;
        auto data = o->get_property_data(name);
        return data != nullptr ? *(sdk::UObject**)data : nullptr;
    };

    try {
        if (kind == "World") {
            if (auto lvl = read_obj_prop(obj, L"PersistentLevel")) out.emplace_back("PersistentLevel", lvl);
        } else if (kind == "PC") {
            if (auto hud = read_obj_prop(obj, L"MyHUD")) out.emplace_back("HUD", hud);
        } else if (kind == "Camera") {
            // FTViewTarget ViewTarget; first member is AActor* Target.
            if (auto tgt = read_obj_prop(obj, L"ViewTarget")) out.emplace_back("ViewTarget", tgt);
        } else if (kind == "Pawn") {
            auto actor = (sdk::AActor*)obj;
            static const auto skel_c = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");
            static const auto caps_c = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.CapsuleComponent");
            static const auto cam_c  = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.CameraComponent");
            if (auto root = actor->get_root_component()) out.emplace_back("RootComponent", (sdk::UObject*)root);
            if (skel_c != nullptr) {
                int n = 0;
                for (auto c : actor->get_components_by_class(skel_c)) {
                    if (c == nullptr) continue;
                    out.emplace_back(n == 0 ? "SkeletalMesh" : ("SkeletalMesh " + std::to_string(n)), (sdk::UObject*)c);
                    ++n;
                }
            }
            if (caps_c != nullptr) {
                if (auto c = actor->get_component_by_class(caps_c)) out.emplace_back("Capsule", (sdk::UObject*)c);
            }
            if (cam_c != nullptr) {
                int n = 0;
                for (auto c : actor->get_components_by_class(cam_c)) {
                    if (c == nullptr) continue;
                    out.emplace_back(n == 0 ? "CameraComponent" : ("CameraComponent " + std::to_string(n)), (sdk::UObject*)c);
                    ++n;
                }
            }
        }
    } catch (...) {}
    return out;
}

// CDOs of every BlueprintFunctionLibrary subclass (GameplayStatics, Kismet*, ...),
// so the picker / function caller can target a library and call its static
// functions. Built once (the class set is static); the full-array walk + per-class
// is_a is why it's cached rather than rebuilt per frame.
std::vector<std::pair<std::string, sdk::UObject*>>& gather_function_libraries() {
    static std::vector<std::pair<std::string, sdk::UObject*>> s_cache;
    if (!s_cache.empty()) {
        return s_cache;
    }
    try {
        static const auto bfl = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.BlueprintFunctionLibrary");
        static const auto uclass_t = sdk::UClass::static_class();
        if (bfl == nullptr || uclass_t == nullptr) {
            return s_cache;
        }
        auto arr = sdk::FUObjectArray::get();
        const auto count = arr ? arr->get_object_count() : 0;
        for (int32_t i = 0; i < count; ++i) {
            auto item = arr->get_object(i);
            if (item == nullptr || item->get_object() == nullptr) continue;
            auto o = (sdk::UObject*)item->get_object();
            auto oc = o->get_class();
            if (oc == nullptr || !oc->is_a(uclass_t)) continue; // o is a UClass
            auto cls = (sdk::UClass*)o;
            if (cls == bfl || !cls->is_a(bfl)) continue;          // derives from BFL
            if (auto cdo = cls->get_class_default_object()) {
                s_cache.emplace_back(utility::narrow(cls->get_fname().to_string()), cdo);
            }
        }
        std::sort(s_cache.begin(), s_cache.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    } catch (...) {}
    return s_cache;
}

// Set the first live-caller slot that has no target to `obj` (T63 send-to-
// caller). Used by the picker context menu so a picked object can be shot
// straight into the Function Caller without dragging.
void load_live_caller_target(sdk::UObject* obj) {
    if (obj == nullptr) return;
    for (auto& slot : s_live_slots) {
        if (slot.target == nullptr) {
            slot.target = obj;
            slot.resolved = nullptr;
            slot.resolved_label.clear();
            slot.resolve_error.clear();
            return;
        }
    }
    // All full — overwrite slot 0's target (keep its function; user can re-pick).
    s_live_slots[0].target = obj;
    s_live_slots[0].resolved = nullptr;
    s_live_slots[0].resolved_label.clear();
}

bool render_universal_object_picker(const char* id_prefix, sdk::UObject*& slot,
                                    PickerState& st, sdk::UClass* expected_class,
                                    bool allow_send_to_caller, const char* auto_label) {
    bool changed = false;
    ImGui::PushID(id_prefix);
    utility::ScopeGuard pop{[]() { ImGui::PopID(); }};

    // Short button label (drop target). Full name is wrapped underneath so a
    // long path never blows out the row.
    std::string short_label;
    std::string full_label;
    if (slot == nullptr) {
        short_label = "[empty — drop / type / pick]";
    } else if (auto_label != nullptr) {
        short_label = auto_label;
    } else {
        std::string cls_short = "obj";
        try { if (auto c = slot->get_class(); c != nullptr) cls_short = utility::narrow(c->get_fname().to_string()); } catch (...) {}
        short_label = std::format("[{} 0x{:x}]", cls_short, (uintptr_t)slot);
    }
    if (slot != nullptr) {
        try { full_label = utility::narrow(slot->get_full_name()); } catch (...) {}
    }

    ImGui::Button((short_label + "##objbtn").c_str(), ImVec2{0, 0}); // stable ID; label text varies
    if (auto dropped = accept_object_drop(); dropped != nullptr) { slot = dropped; changed = true; }
    // Make the current object draggable so it can be chained elsewhere.
    if (slot != nullptr) {
        make_drag_source_for_object(slot, full_label.c_str());
    }
    // Right-click context menu.
    if (slot != nullptr && ImGui::BeginPopupContextItem("##uctx")) {
        if (ImGui::MenuItem("Copy full name")) {
            if (OpenClipboard(NULL)) {
                EmptyClipboard();
                HGLOBAL h = GlobalAlloc(GMEM_DDESHARE, full_label.size() + 1);
                if (h != nullptr) {
                    char* d = (char*)GlobalLock(h);
                    if (d != nullptr) { strcpy(d, full_label.c_str()); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
                }
                CloseClipboard();
            }
        }
        if (allow_send_to_caller && ImGui::MenuItem("Send to Function Caller (as target)")) {
            load_live_caller_target(slot);
        }
        ImGui::EndPopup();
    }

    if (slot != nullptr) {
        ImGui::SameLine();
        if (ImGui::SmallButton("clear")) { slot = nullptr; changed = true; }
    }

    // Text-input fallback: short name / full name / 0xADDR + Enter.
    ImGui::SameLine();
    if (auto resolved = render_object_text_input("##txt", st.text); resolved != nullptr) {
        slot = resolved; changed = true;
    }

    // Pick popup.
    ImGui::SameLine();
    const auto popup_id = std::string{"upick##"} + id_prefix;
    if (ImGui::SmallButton("pick...")) { ImGui::OpenPopup(popup_id.c_str()); }
    if (auto picked = render_object_picker_popup(popup_id.c_str(), st.filter,
                                                 st.use_type_filter ? expected_class : nullptr,
                                                 st.list_classes);
        picked != nullptr) {
        slot = picked; changed = true;
    }

    // Display toggles.
    ImGui::Checkbox("classes", &st.list_classes);
    if (expected_class != nullptr && !st.list_classes) {
        ImGui::SameLine();
        ImGui::Checkbox("type filter", &st.use_type_filter);
    }

    // Common-object quick-picks. Each opens a dropdown to the base object or its
    // key sub-objects (World->PersistentLevel, PC->HUD, Camera->ViewTarget,
    // Pawn->components). "Libs" lists BlueprintFunctionLibrary CDOs (static fns,
    // e.g. GameplayStatics / Kismet*).
    {
        const auto commons = gather_common_objects();
        bool first = true;
        for (const auto& [name, obj] : commons) {
            if (obj == nullptr) continue;
            if (!first) ImGui::SameLine();
            first = false;
            ImGui::PushID(name);
            if (ImGui::SmallButton(name)) { ImGui::OpenPopup("cpop"); }
            if (ImGui::BeginPopup("cpop")) {
                if (ImGui::MenuItem(name)) { slot = obj; changed = true; }
                const auto children = gather_common_object_children(name, obj);
                if (!children.empty()) { ImGui::Separator(); }
                for (const auto& [clabel, cobj] : children) {
                    if (cobj == nullptr) continue;
                    if (ImGui::MenuItem(clabel.c_str())) { slot = cobj; changed = true; }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        if (!first) { ImGui::SameLine(); }
        if (ImGui::SmallButton("Libs")) { ImGui::OpenPopup("libspop"); }
        if (ImGui::BeginPopup("libspop")) {
            auto& libs = gather_function_libraries();
            if (libs.empty()) {
                ImGui::TextDisabled("no function libraries (not yet populated)");
            } else {
                ImGui::TextDisabled("%zu libraries — static functions via CDO", libs.size());
                if (ImGui::BeginChild("libslist", ImVec2(280.0f, 320.0f))) {
                    for (const auto& [lname, lcdo] : libs) {
                        if (lcdo == nullptr) continue;
                        if (ImGui::Selectable(lname.c_str())) {
                            slot = lcdo;
                            changed = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                ImGui::EndChild();
            }
            ImGui::EndPopup();
        }
    }

    // Compact name of the current value (hover for the full path, right-click to copy it).
    if (slot != nullptr && !full_label.empty()) {
        ui_object_label_compact(full_label);
    }

    return changed;
}

// -----------------------------------------------------------------------------
// Generic struct-leaf walker (T64). Flattens a (possibly nested) UStruct into a
// deterministic list of scalar leaves so the function caller can render and
// encode ANY all-numeric struct — FVector/FRotator/FQuat/FTransform/FColor/
// FIntPoint/FMatrix/... — driving each leaf's width from its own FProperty
// (FloatProperty=4, DoubleProperty=8) instead of guessing UE4-vs-UE5 layout.
// Render and encode MUST both call this so the leaf order they index is shared.
// Returns false if any member is non-numeric (object/name/array/enum/...), in
// which case the caller routes the whole struct to the inline-Lua fallback.
// -----------------------------------------------------------------------------
enum LeafKind { LK_F32, LK_F64, LK_I8, LK_I16, LK_I32, LK_I64, LK_U8, LK_U16, LK_U32, LK_U64, LK_Bool };
struct StructLeaf {
    int32_t offset{};          // absolute, relative to the struct base
    int32_t container_base{};  // immediate-struct base (for FBoolProperty set/get)
    int     kind{LK_F32};
    std::string label;         // "Member" or "Parent.Member"
    sdk::FProperty* prop{};    // for bool set/get
};
int leaf_byte_width(int k) {
    switch (k) {
    case LK_F32: case LK_I32: case LK_U32: return 4;
    case LK_F64: case LK_I64: case LK_U64: return 8;
    case LK_I16: case LK_U16: return 2;
    case LK_I8:  case LK_U8:  case LK_Bool: return 1;
    default: return 4;
    }
}
bool collect_struct_leaves(sdk::UStruct* strukt, int32_t base, const std::string& prefix,
                           std::vector<StructLeaf>& out, int depth = 0) {
    if (strukt == nullptr || depth > 8) return false;
    // Inherited members live on the SuperStruct (get_child_properties lists only
    // the directly-declared FFields). Walk bases first so an inheriting struct is
    // encoded completely instead of leaving inherited fields silently zeroed.
    // Member offsets are absolute within the instance, so the same `base` applies.
    if (auto super = strukt->get_super_struct(); super != nullptr) {
        if (!collect_struct_leaves(super, base, prefix, out, depth + 1)) return false;
    }
    for (auto f = strukt->get_child_properties(); f != nullptr; f = f->get_next()) {
        std::string cname;
        try { cname = utility::narrow(f->get_class()->get_name().to_string()); } catch (...) { return false; }
        if (!cname.contains("Property")) continue;
        auto prop = (sdk::FProperty*)f;
        const int32_t off = base + prop->get_offset();
        std::string mn;
        try { mn = utility::narrow(f->get_field_name().to_string()); } catch (...) { mn = "?"; }
        const std::string label = prefix.empty() ? mn : (prefix + "." + mn);
        const auto h = ::utility::hash(f->get_class()->get_name().to_string());
        switch (h) {
        case L"FloatProperty"_fnv:   out.push_back({off, base, LK_F32, label, prop}); break;
        case L"DoubleProperty"_fnv:  out.push_back({off, base, LK_F64, label, prop}); break;
        case L"Int8Property"_fnv:    out.push_back({off, base, LK_I8,  label, prop}); break;
        case L"Int16Property"_fnv:   out.push_back({off, base, LK_I16, label, prop}); break;
        case L"IntProperty"_fnv:     out.push_back({off, base, LK_I32, label, prop}); break;
        case L"Int64Property"_fnv:   out.push_back({off, base, LK_I64, label, prop}); break;
        case L"ByteProperty"_fnv:    out.push_back({off, base, LK_U8,  label, prop}); break;
        case L"UInt16Property"_fnv:  out.push_back({off, base, LK_U16, label, prop}); break;
        case L"UInt32Property"_fnv:
        case L"UIntProperty"_fnv:    out.push_back({off, base, LK_U32, label, prop}); break;
        case L"UInt64Property"_fnv:  out.push_back({off, base, LK_U64, label, prop}); break;
        case L"BoolProperty"_fnv:    out.push_back({off, base, LK_Bool, label, prop}); break;
        case L"StructProperty"_fnv: {
            auto sp = (sdk::FStructProperty*)prop;
            auto inner = sp != nullptr ? sp->get_struct() : nullptr;
            if (inner == nullptr) return false;
            if (!collect_struct_leaves(inner, off, label, out, depth + 1)) return false;
            break;
        }
        default:
            return false; // non-numeric leaf -> inline-Lua fallback
        }
        if (out.size() > 256) return false; // sanity cap
    }
    return true;
}

// Render a single parameter editor; returns nothing — state is stashed inside
// `s`. `prop_name_narrow` and `name_hash` are precomputed by the caller to
// avoid hashing/converting twice per frame.
void render_param_editor(ParamEditState& s, sdk::FProperty* prop, const std::string& prop_name_narrow, size_t name_hash) {
    using namespace ::utility;
    ImGui::PushID(prop);
    utility::ScopeGuard pop{[]() { ImGui::PopID(); }};

    switch (name_hash) {
    case L"BoolProperty"_fnv: {
        bool& b = s.bools[prop_name_narrow];
        ImGui::Checkbox(prop_name_narrow.c_str(), &b);
        break;
    }
    case L"EnumProperty"_fnv: {
        int& v = s.ints[prop_name_narrow];
        sdk::UEnum* uenum = nullptr;
        try { uenum = ((sdk::FEnumProperty*)prop)->get_enum(); } catch (...) {}
        if (uenum != nullptr) {
            // get_names() does ~512 process_event calls — cache per UEnum.
            static std::unordered_map<sdk::UEnum*, std::vector<std::pair<std::string, int64_t>>> s_param_enum_cache;
            auto it = s_param_enum_cache.find(uenum);
            if (it == s_param_enum_cache.end()) {
                std::vector<std::pair<std::string, int64_t>> names{};
                try { names = uenum->get_names(); } catch (...) {}
                it = s_param_enum_cache.emplace(uenum, std::move(names)).first;
            }
            if (!it->second.empty()) {
                const char* cur_label = "(custom)";
                for (const auto& [n, val] : it->second) { if ((int)val == v) { cur_label = n.c_str(); break; } }
                if (ImGui::BeginCombo(prop_name_narrow.c_str(), cur_label)) {
                    for (const auto& [n, val] : it->second) {
                        if (ImGui::Selectable((n + " = " + std::to_string(val)).c_str(), (int)val == v)) {
                            v = (int)val;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f);
                ImGui::InputInt(("##rawenum_" + prop_name_narrow).c_str(), &v);
                break;
            }
        }
        ImGui::InputInt(prop_name_narrow.c_str(), &v);
        break;
    }
    case L"ByteProperty"_fnv:
    case L"UInt16Property"_fnv:
    case L"Int16Property"_fnv:
    case L"Int8Property"_fnv:
    case L"IntProperty"_fnv:
    case L"UInt32Property"_fnv:
    case L"UIntProperty"_fnv: {
        int& v = s.ints[prop_name_narrow];
        ImGui::InputInt(prop_name_narrow.c_str(), &v);
        break;
    }
    case L"Int64Property"_fnv: {
        int64_t& v = s.int64s[prop_name_narrow];
        ImGui::InputScalar(prop_name_narrow.c_str(), ImGuiDataType_S64, &v);
        break;
    }
    case L"UInt64Property"_fnv: {
        uint64_t& v = s.uint64s[prop_name_narrow];
        ImGui::InputScalar(prop_name_narrow.c_str(), ImGuiDataType_U64, &v);
        break;
    }
    case L"FloatProperty"_fnv: {
        float& v = s.floats[prop_name_narrow];
        ImGui::DragFloat(prop_name_narrow.c_str(), &v, 0.1f);
        break;
    }
    case L"DoubleProperty"_fnv: {
        double& v = s.doubles[prop_name_narrow];
        // ImGui's DragScalar gives us a double-aware editor.
        ImGui::DragScalar(prop_name_narrow.c_str(), ImGuiDataType_Double, &v, 0.1f);
        break;
    }
    case L"NameProperty"_fnv:
    case L"StrProperty"_fnv:
    case L"TextProperty"_fnv: {
        std::string& buf = s.text[prop_name_narrow];
        if (buf.size() < 256) buf.resize(256);
        if (ImGui::InputText(prop_name_narrow.c_str(), buf.data(), buf.size())) {
            // Keep buffer null-terminated by trimming after the first \0.
            buf.resize(std::strlen(buf.c_str()));
            buf.resize(256);
        }
        break;
    }
    case L"InterfaceProperty"_fnv:
    case L"ObjectProperty"_fnv:
    case L"WeakObjectProperty"_fnv:
    case L"LazyObjectProperty"_fnv:
    case L"SoftObjectProperty"_fnv: {
        // Universal picker for every UObject-flavoured property — the encode
        // step below packs the chosen pointer into the appropriate (raw / weak /
        // soft) layout.
        sdk::UObject*& slot = s.objs[prop_name_narrow];
        // WorldContext parameters auto-resolve to the world — the user never has
        // to supply one (GetAllActorsOfClass and friends all take one). Re-fills
        // when cleared; pick a different object to override.
        const bool is_world_ctx = prop_name_narrow.find("WorldContext") != std::string::npos;
        if (is_world_ctx && slot == nullptr) {
            if (auto eng = sdk::UGameEngine::get(); eng != nullptr) {
                slot = (sdk::UObject*)eng->get_world();
            }
        }
        sdk::UClass* expected_class = nullptr;
        // Soft / weak / lazy object props all derive from FObjectPropertyBase
        // (get_property_class); FObjectProperty too. FInterfaceProperty uses a
        // different accessor — skip the hint and let the picker show everything.
        if (name_hash != L"InterfaceProperty"_fnv) {
            expected_class = ((sdk::FObjectProperty*)prop)->get_property_class();
        }
        // Only label "[auto: World]" while the slot still holds the live world.
        const char* auto_lbl = nullptr;
        if (is_world_ctx && slot != nullptr) {
            if (auto eng = sdk::UGameEngine::get(); eng != nullptr && slot == (sdk::UObject*)eng->get_world()) {
                auto_lbl = "[auto: World]";
            }
        }
        ImGui::TextUnformatted(prop_name_narrow.c_str());
        render_universal_object_picker(prop_name_narrow.c_str(), slot, s.pickers[prop_name_narrow],
                                       expected_class, true, auto_lbl);
        break;
    }
    case L"ClassProperty"_fnv:
    case L"SoftClassProperty"_fnv: {
        sdk::UClass*& slot = s.classes[prop_name_narrow];
        const auto label = slot == nullptr
            ? std::string{"[drop UClass here] "} + prop_name_narrow
            : std::string{"[class "} + std::to_string((uintptr_t)slot) + "] " + prop_name_narrow;
        ImGui::Button(label.c_str(), ImVec2{0, 0});
        if (auto dropped = accept_class_drop(); dropped != nullptr) {
            slot = dropped;
        }
        if (slot != nullptr) {
            ImGui::SameLine();
            if (ImGui::SmallButton("clear")) {
                slot = nullptr;
            }
        }
        ImGui::SameLine();
        if (auto resolved = render_class_text_input(("##clstext_" + prop_name_narrow).c_str(),
                                                    s.cls_text[prop_name_narrow]);
            resolved != nullptr) {
            slot = resolved;
        }
        // Pick-from-list (class mode) — searchable list of every UClass.
        ImGui::SameLine();
        {
            const auto popup_id = "clspick_" + prop_name_narrow;
            if (ImGui::SmallButton("pick...")) {
                ImGui::OpenPopup(popup_id.c_str());
            }
            if (auto picked = render_object_picker_popup(popup_id.c_str(),
                                                         s.picker_filter[prop_name_narrow],
                                                         nullptr, /*list_classes=*/true);
                picked != nullptr) {
                if (auto c = picked->get_class(); c != nullptr && c->is_a(sdk::UClass::static_class())) {
                    slot = reinterpret_cast<sdk::UClass*>(picked);
                }
            }
        }
        break;
    }
    case L"StructProperty"_fnv: {
        // Generic reflected-leaf editor (T64): flatten the struct (recursing
        // through nested structs) to scalar leaves and edit each as a double.
        // Handles FVector/FRotator/FQuat/FTransform/FColor/FIntPoint/FMatrix/...
        // engine-agnostically; encode_param walks the SAME leaf order.
        const auto sp = (sdk::FStructProperty*)prop;
        const auto strukt = sp != nullptr ? sp->get_struct() : nullptr;
        if (strukt == nullptr) {
            ImGui::TextDisabled("StructProperty %s — unknown struct", prop_name_narrow.c_str());
            break;
        }
        const auto sname = utility::narrow(strukt->get_fname().to_string());
        std::vector<StructLeaf> leaves;
        const bool ok = collect_struct_leaves(strukt, 0, "", leaves);
        if (!ok || leaves.empty()) {
            // Non-numeric struct (has Object/Name/Str/Enum/Array/... members): edit it generically via
            // ui_handle_struct on a per-param scratch buffer sized to the struct. encode_param memcpys
            // that buffer into the params blob. (The all-numeric fast path below stays for math structs.)
            // Size the scratch by get_properties_size() (the REFLECTED layout) — ui_handle_struct
            // writes members by their reflected offset, which lives within PropertiesSize. struct_size
            // (StructOps.size) is resolved via a brute-forced offset the SDK warns is "not correct
            // always", so trusting it could undersize the buffer and let a member write corrupt the
            // heap. Prefer properties_size; fall back to struct_size only if 0; cap to reject garbage.
            size_t sz = (size_t)strukt->get_properties_size();
            if (sz == 0) sz = (size_t)strukt->get_struct_size();
            if (sz == 0 || sz > (1u << 20)) {
                ImGui::TextDisabled("[%s] %s — unusable struct size (%zu)", sname.c_str(), prop_name_narrow.c_str(), sz);
                break;
            }
            auto& buf = s.struct_bytes[prop_name_narrow];
            try { if (buf.size() != sz) buf.assign(sz, 0); }
            catch (...) { ImGui::TextDisabled("[%s] %s — struct alloc failed", sname.c_str(), prop_name_narrow.c_str()); break; }
            ImGui::Text("[%s] %s", sname.c_str(), prop_name_narrow.c_str());
            ImGui::Indent();
            if (auto uoh = UObjectHook::get(); uoh != nullptr) {
                try { uoh->ui_handle_struct(buf.data(), strukt); }
                catch (...) { ImGui::TextDisabled("(struct editor threw)"); }
            }
            ImGui::Unindent();
            break;
        }
        auto& scratch = s.struct_scratch[prop_name_narrow];
        if (scratch.size() != leaves.size()) {
            scratch.assign(leaves.size(), 0.0);
        }
        ImGui::Text("[%s] %s", sname.c_str(), prop_name_narrow.c_str());
        ImGui::Indent();
        const auto immediate_parent = [](const std::string& lab) -> std::string {
            auto pos = lab.rfind('.');
            return pos == std::string::npos ? std::string{} : lab.substr(0, pos);
        };
        size_t li = 0;
        while (li < leaves.size()) {
            const std::string par = immediate_parent(leaves[li].label);
            size_t lj = li;
            while (lj < leaves.size() && (lj - li) < 4 && immediate_parent(leaves[lj].label) == par) {
                ++lj;
            }
            const int n = (int)(lj - li);
            const std::string row = (n == 1 ? leaves[li].label : (par.empty() ? sname : par))
                                    + "##grp" + prop_name_narrow + std::to_string(li);
            ImGui::DragScalarN(row.c_str(), ImGuiDataType_Double, &scratch[li], n, 0.1f, nullptr, nullptr, "%.4g");
            li = lj;
        }
        ImGui::Unindent();
        break;
    }
    default:
        ImGui::TextDisabled("%s (unsupported in caller)", prop_name_narrow.c_str());
        break;
    }
}

// Write the scratch value for `prop` into the params buffer at the property's
// offset. Returns false if the property type isn't supported (in which case the
// buffer is left zero-initialised at that offset).
bool encode_param(ParamEditState& s, sdk::FProperty* prop, const std::string& name, size_t name_hash, uint8_t* params, size_t params_size) {
    const auto offset = prop->get_offset();
    if (offset < 0 || (size_t)offset >= params_size) {
        return false;
    }

    auto out = params + offset;
    switch (name_hash) {
    case L"BoolProperty"_fnv: {
        auto fbp = (sdk::FBoolProperty*)prop;
        fbp->set_value_in_object(params, s.bools[name]);
        return true;
    }
    case L"ByteProperty"_fnv:        *(uint8_t*)out  = (uint8_t)s.ints[name];  return true;
    case L"Int8Property"_fnv:        *(int8_t*)out   = (int8_t)s.ints[name];   return true;
    case L"UInt16Property"_fnv:      *(uint16_t*)out = (uint16_t)s.ints[name]; return true;
    case L"Int16Property"_fnv:       *(int16_t*)out  = (int16_t)s.ints[name];  return true;
    case L"IntProperty"_fnv:         *(int32_t*)out  = (int32_t)s.ints[name];  return true;
    case L"UInt32Property"_fnv:
    case L"UIntProperty"_fnv:        *(uint32_t*)out = (uint32_t)s.ints[name]; return true;
    case L"EnumProperty"_fnv:        *(int32_t*)out  = (int32_t)s.ints[name];  return true;
    case L"Int64Property"_fnv:       *(int64_t*)out  = s.int64s[name];         return true;
    case L"UInt64Property"_fnv:      *(uint64_t*)out = s.uint64s[name];        return true;
    case L"FloatProperty"_fnv:       *(float*)out    = s.floats[name];         return true;
    case L"DoubleProperty"_fnv:      *(double*)out   = s.doubles[name];        return true;
    case L"NameProperty"_fnv: {
        s.wtext[name] = utility::widen(s.text[name]);
        // FName has a 2-arg constructor (wstring_view, EFindName) — EFindName
        // is in the sdk namespace, not nested in FName.
        *(sdk::FName*)out = sdk::FName{std::wstring_view{s.wtext[name]}, sdk::EFindName::Add};
        return true;
    }
    case L"StrProperty"_fnv: {
        s.wtext[name] = utility::widen(s.text[name]);
        auto& arr = *(sdk::TArrayLite<wchar_t>*)out;
        arr.data = (wchar_t*)s.wtext[name].c_str();
        arr.count = (int32_t)s.wtext[name].size();
        arr.capacity = arr.count + 1;
        return true;
    }
    case L"InterfaceProperty"_fnv:
    case L"ObjectProperty"_fnv:      *(sdk::UObject**)out = s.objs[name];     return true;
    case L"ClassProperty"_fnv:       *(sdk::UClass**)out  = s.classes[name];  return true;
    case L"WeakObjectProperty"_fnv:
    case L"LazyObjectProperty"_fnv: {
        // Pack { ObjectIndex, SerialNumber } from the chosen UObject's
        // FUObjectArray entry. Lazy's trailing FUniqueObjectGuid is left
        // zeroed — only used by editor persistence.
        auto raw = (int32_t*)out;
        auto target = s.objs[name];
        if (target == nullptr) {
            raw[0] = 0; raw[1] = 0;
            return true;
        }
        const auto obj_index = *(int32_t*)((uintptr_t)target + 0xC); // UObjectBase::InternalIndex
        raw[0] = obj_index;
        if (auto item = sdk::FUObjectArray::get()->get_object(obj_index); item != nullptr) {
            raw[1] = item->get_serial_number();
        } else {
            raw[1] = 0;
        }
        return true;
    }
    case L"SoftObjectProperty"_fnv: {
        // Best-effort: just cache the weak prefix from the chosen target.
        // The FSoftObjectPath itself (FName + FString SubPath) is left
        // untouched — for in-game live calls the live weak ref is what
        // matters, persistence is editor-only.
        auto raw = (int32_t*)out;
        auto target = s.objs[name];
        if (target == nullptr) {
            raw[0] = 0; raw[1] = 0;
            return true;
        }
        const auto obj_index = *(int32_t*)((uintptr_t)target + 0xC);
        raw[0] = obj_index;
        if (auto item = sdk::FUObjectArray::get()->get_object(obj_index); item != nullptr) {
            raw[1] = item->get_serial_number();
        } else {
            raw[1] = 0;
        }
        return true;
    }
    case L"SoftClassProperty"_fnv: {
        // Same as SoftObject but the slot holds a UClass* via s.classes —
        // pack it through the FUObjectArray as a UObject*.
        auto raw = (int32_t*)out;
        auto target = s.classes[name];
        if (target == nullptr) {
            raw[0] = 0; raw[1] = 0;
            return true;
        }
        const auto obj_index = *(int32_t*)((uintptr_t)target + 0xC);
        raw[0] = obj_index;
        if (auto item = sdk::FUObjectArray::get()->get_object(obj_index); item != nullptr) {
            raw[1] = item->get_serial_number();
        } else {
            raw[1] = 0;
        }
        return true;
    }
    case L"StructProperty"_fnv: {
        // Generic reflected-leaf encode (T64): walk the SAME leaf order the
        // editor used (collect_struct_leaves) and write each scalar at its own
        // offset/width. Every write is bounds-checked against params_size — a
        // bad offset here is memory corruption on Call.
        const auto sp = (sdk::FStructProperty*)prop;
        const auto strukt = sp != nullptr ? sp->get_struct() : nullptr;
        if (strukt == nullptr) return false;
        std::vector<StructLeaf> leaves;
        if (!collect_struct_leaves(strukt, 0, "", leaves) || leaves.empty()) {
            // Non-numeric struct: the generic editor wrote into s.struct_bytes[name]; copy the whole
            // struct into the params blob (bounds-checked). This is what makes StructProperty params
            // with Object/Name/Enum/... members callable instead of zeroed.
            auto it = s.struct_bytes.find(name);
            if (it == s.struct_bytes.end() || it->second.empty()) return false;
            const size_t sz = it->second.size();
            if ((size_t)offset + sz > params_size) return false;
            std::memcpy(params + (size_t)offset, it->second.data(), sz);
            return true;
        }
        const auto& scratch = s.struct_scratch[name];
        for (size_t i = 0; i < leaves.size(); ++i) {
            const auto& lf = leaves[i];
            const double val = i < scratch.size() ? scratch[i] : 0.0;
            const size_t abs_off = (size_t)offset + (size_t)lf.offset;
            if (abs_off + (size_t)leaf_byte_width(lf.kind) > params_size) {
                return false; // refuse to write past the param buffer
            }
            uint8_t* p = params + abs_off;
            switch (lf.kind) {
            case LK_F32:  *(float*)p    = (float)val; break;
            case LK_F64:  *(double*)p   = val; break;
            case LK_I8:   *(int8_t*)p   = (int8_t)(int64_t)val; break;
            case LK_I16:  *(int16_t*)p  = (int16_t)(int64_t)val; break;
            case LK_I32:  *(int32_t*)p  = (int32_t)(int64_t)val; break;
            case LK_I64:  *(int64_t*)p  = (int64_t)val; break;
            case LK_U8:   *(uint8_t*)p  = (uint8_t)(int64_t)val; break;
            case LK_U16:  *(uint16_t*)p = (uint16_t)(int64_t)val; break;
            case LK_U32:  *(uint32_t*)p = (uint32_t)(int64_t)val; break;
            case LK_U64:  *(uint64_t*)p = (uint64_t)(int64_t)val; break;
            case LK_Bool:
                if (lf.prop != nullptr) {
                    ((sdk::FBoolProperty*)lf.prop)->set_value_in_object(params + (size_t)offset + (size_t)lf.container_base, val != 0.0);
                }
                break;
            default: break;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

// Render `prop` after a call to provide a read-out of the value it holds.
// Used for return values and out parameters.
std::string format_return_value(sdk::FProperty* prop, const uint8_t* params, size_t params_size);

// Read up to `cap` live entries from an FScriptMap/FScriptSet at `base`.
// Returns the slot count (FScriptArray.ArrayNum), or -1 if `base` is unreadable.
// `out` is filled ONLY when the container is safely traversable (sane layout +
// no removed-slot holes); otherwise it's left empty and the caller shows the
// slot count alone. Defined below format_return_value (it formats key/value via it).
int read_map_entries(sdk::FMapProperty* mp, const uint8_t* base, int cap, std::vector<std::pair<std::string, std::string>>& out);
int read_set_entries(sdk::FSetProperty* sp, const uint8_t* base, int cap, std::vector<std::string>& out);

// Format a single FScriptDelegate { int32 ObjectIndex; int32 SerialNumber;
// FName FunctionName; } (16 bytes) as "Object.Full.Name::FunctionName". Layout
// is stable across UE4/5; every read is guarded so a bad ptr degrades to text.
std::string format_script_delegate(const uint8_t* d) {
    if (d == nullptr || IsBadReadPtr((void*)d, 16)) {
        return "<unreadable>";
    }
    const auto obj_index = *(const int32_t*)(d + 0);
    std::string fn;
    try {
        fn = utility::narrow(((const sdk::FName*)(d + 8))->to_string());
    } catch (...) {
        fn = "?";
    }
    if (obj_index <= 0) {
        return (fn.empty() || fn == "None") ? "<unbound>" : "<unbound>::" + fn;
    }
    auto item = sdk::FUObjectArray::get()->get_object(obj_index);
    if (item == nullptr || item->get_object() == nullptr) {
        return std::format("<stale idx={}>::{}", obj_index, fn);
    }
    try {
        return utility::narrow(((sdk::UObject*)item->get_object())->get_full_name()) + "::" + fn;
    } catch (...) {
        return std::format("[{:#x}]::{}", (uintptr_t)item->get_object(), fn);
    }
}

// Inline/regular multicast: FMulticastScriptDelegate { TArray<FScriptDelegate>
// InvocationList; }. Lists each binding; sparse multicast has a different
// layout and is handled separately by the caller.
std::string format_multicast_delegate(const uint8_t* in) {
    const auto& arr = *(const sdk::TArrayLite<uint8_t>*)in;
    if (arr.data == nullptr || arr.count <= 0) {
        return "<no bindings>";
    }
    if (arr.count > 4096 || IsBadReadPtr(arr.data, 16)) {
        return "<delegate: unreadable>";
    }
    constexpr int cap = 8;
    const int n = arr.count < cap ? arr.count : cap;
    std::string out = std::format("[{}] {{", arr.count);
    for (int i = 0; i < n; ++i) {
        if (i != 0) out += ", ";
        out += format_script_delegate(arr.data + (size_t)i * 16);
    }
    if (arr.count > cap) out += std::format(", ...(+{} more)", arr.count - cap);
    out += "}";
    return out;
}

std::string format_return_value(sdk::FProperty* prop, const uint8_t* params, size_t /*params_size*/) {
    const auto pc = prop->get_class();
    if (pc == nullptr) return "<no class>";
    // FFieldClass exposes get_name() (returning FName by ref), not get_fname().
    const auto name_hash = ::utility::hash(pc->get_name().to_string());
    const auto offset = prop->get_offset();
    auto in = params + offset;

    switch (name_hash) {
    case L"BoolProperty"_fnv: {
        auto fbp = (sdk::FBoolProperty*)prop;
        return fbp->get_value_from_object(const_cast<uint8_t*>(params)) ? "true" : "false";
    }
    case L"ByteProperty"_fnv:        return std::to_string(*(uint8_t*)in);
    case L"Int8Property"_fnv:        return std::to_string(*(int8_t*)in);
    case L"Int16Property"_fnv:       return std::to_string(*(int16_t*)in);
    case L"UInt16Property"_fnv:      return std::to_string(*(uint16_t*)in);
    case L"IntProperty"_fnv:         return std::to_string(*(int32_t*)in);
    case L"EnumProperty"_fnv: {
        const auto val = (int64_t)*(int32_t*)in;
        sdk::UEnum* uenum = nullptr;
        try { uenum = ((sdk::FEnumProperty*)prop)->get_enum(); } catch (...) {}
        if (uenum != nullptr) {
            // get_names() does ~512 process_event calls — cache per UEnum.
            static std::unordered_map<sdk::UEnum*, std::vector<std::pair<std::string, int64_t>>> s_ret_enum_cache;
            auto it = s_ret_enum_cache.find(uenum);
            if (it == s_ret_enum_cache.end()) {
                std::vector<std::pair<std::string, int64_t>> names;
                try { names = uenum->get_names(); } catch (...) {}
                it = s_ret_enum_cache.emplace(uenum, std::move(names)).first;
            }
            for (const auto& [n, v] : it->second) {
                if (v == val) return std::format("{} ({})", n, val);
            }
        }
        return std::to_string(val);
    }
    case L"UInt32Property"_fnv:
    case L"UIntProperty"_fnv:        return std::to_string(*(uint32_t*)in);
    case L"Int64Property"_fnv:       return std::to_string(*(int64_t*)in);
    case L"UInt64Property"_fnv:      return std::to_string(*(uint64_t*)in);
    case L"FloatProperty"_fnv:       return std::to_string(*(float*)in);
    case L"DoubleProperty"_fnv:      return std::to_string(*(double*)in);
    case L"NameProperty"_fnv:        return utility::narrow(((sdk::FName*)in)->to_string());
    case L"StrProperty"_fnv: {
        const auto& arr = *(sdk::TArrayLite<wchar_t>*)in;
        if (arr.data == nullptr || arr.count <= 0) return "\"\"";
        return std::string{"\""} + utility::narrow(std::wstring{arr.data, (size_t)arr.count}) + "\"";
    }
    case L"InterfaceProperty"_fnv:
    case L"ObjectProperty"_fnv: {
        auto* obj = *(sdk::UObject**)in;
        if (obj == nullptr) return "nullptr";
        // Pretty-print so the user sees the object's full name (matches the
        // labels used everywhere else in UObjectHook) plus the address in
        // brackets. Falls back to the raw address if name access fails.
        try {
            return std::string{"["} + std::to_string((uintptr_t)obj) + "] " + utility::narrow(obj->get_full_name());
        } catch (...) {
            return std::to_string((uintptr_t)obj);
        }
    }
    case L"ClassProperty"_fnv: {
        auto* c = *(sdk::UClass**)in;
        if (c == nullptr) return "nullptr";
        try {
            return std::string{"["} + std::to_string((uintptr_t)c) + "] " + utility::narrow(c->get_full_name());
        } catch (...) {
            return std::to_string((uintptr_t)c);
        }
    }
    case L"WeakObjectProperty"_fnv:
    case L"LazyObjectProperty"_fnv: {
        // Read { ObjectIndex, SerialNumber } and resolve via FUObjectArray.
        // Stale weak ref shows up as "<stale weak: idx=N>" so the user can
        // tell the difference between a never-set ref and a freed one.
        auto raw = (const int32_t*)in;
        const auto obj_index = raw[0];
        const auto serial = raw[1];
        if (obj_index <= 0) return "nullptr";
        auto item = sdk::FUObjectArray::get()->get_object(obj_index);
        if (item == nullptr || item->get_object() == nullptr) {
            return std::format("<dead weak: idx={}>", obj_index);
        }
        if (item->get_serial_number() != serial) {
            return std::format("<stale weak: idx={}, expected_serial={}, got={}>", obj_index, serial, item->get_serial_number());
        }
        try {
            return std::format("[{:#x}] {} (weak)", (uintptr_t)item->get_object(), utility::narrow(((sdk::UObject*)item->get_object())->get_full_name()));
        } catch (...) {
            return std::format("[{:#x}] (weak)", (uintptr_t)item->get_object());
        }
    }
    case L"SoftObjectProperty"_fnv:
    case L"SoftClassProperty"_fnv: {
        // Fast path: weak prefix is live → return the resolved name.
        // Otherwise fall through to the FSoftObjectPath { FName Asset;
        // FString SubPath } at offset +8 to render the path string.
        auto raw = (const int32_t*)in;
        const auto obj_index = raw[0];
        const auto serial = raw[1];
        if (obj_index > 0) {
            auto item = sdk::FUObjectArray::get()->get_object(obj_index);
            if (item != nullptr && item->get_object() != nullptr && item->get_serial_number() == serial) {
                try {
                    return std::format("[{:#x}] {} (soft, loaded)", (uintptr_t)item->get_object(), utility::narrow(((sdk::UObject*)item->get_object())->get_full_name()));
                } catch (...) {
                    return std::format("[{:#x}] (soft)", (uintptr_t)item->get_object());
                }
            }
        }
        auto* asset_name = (sdk::FName*)(in + 8);
        const auto& sub = *(sdk::TArrayLite<wchar_t>*)(in + 8 + sizeof(sdk::FName));
        std::wstring path;
        try {
            path = asset_name->to_string();
        } catch (...) {
            return "<soft: unreadable FName>";
        }
        if (sub.data != nullptr && sub.count > 0) {
            path += L":";
            const auto len = (size_t)sub.count - (sub.data[sub.count - 1] == L'\0' ? 1 : 0);
            path.append(sub.data, len);
        }
        return std::string{"<soft: "} + utility::narrow(path) + ">";
    }
    case L"ArrayProperty"_fnv: {
        // Snapshot the result NOW — the TArray heap data is freed after the
        // call returns, so persisting a pointer would dangle. Scalar/name/
        // object inner types are formatted inline; struct/other inners get a
        // count summary (use the property-view array handler for full detail).
        const auto inner = ((sdk::FArrayProperty*)prop)->get_inner();
        if (inner == nullptr) return "<array: no inner>";
        const auto ic = inner->get_class();
        if (ic == nullptr) return "<array: no inner class>";
        const auto& arr = *(const sdk::TArrayLite<uint8_t>*)in;
        if (arr.data == nullptr || arr.count <= 0) return "[] (0)";
        const auto ihash = ::utility::hash(ic->get_name().to_string());
        size_t stride = 0;
        std::function<std::string(const uint8_t*)> fmt;
        switch (ihash) {
        case L"BoolProperty"_fnv:   stride = 1; fmt = [](const uint8_t* p) { return *p ? std::string{"true"} : std::string{"false"}; }; break;
        case L"ByteProperty"_fnv:   stride = 1; fmt = [](const uint8_t* p) { return std::to_string(*p); }; break;
        case L"Int8Property"_fnv:   stride = 1; fmt = [](const uint8_t* p) { return std::to_string(*(int8_t*)p); }; break;
        case L"Int16Property"_fnv:  stride = 2; fmt = [](const uint8_t* p) { return std::to_string(*(int16_t*)p); }; break;
        case L"UInt16Property"_fnv: stride = 2; fmt = [](const uint8_t* p) { return std::to_string(*(uint16_t*)p); }; break;
        case L"IntProperty"_fnv:    stride = 4; fmt = [](const uint8_t* p) { return std::to_string(*(int32_t*)p); }; break;
        case L"UInt32Property"_fnv: stride = 4; fmt = [](const uint8_t* p) { return std::to_string(*(uint32_t*)p); }; break;
        case L"FloatProperty"_fnv:  stride = 4; fmt = [](const uint8_t* p) { return std::format("{:.3f}", *(float*)p); }; break;
        case L"Int64Property"_fnv:  stride = 8; fmt = [](const uint8_t* p) { return std::to_string(*(int64_t*)p); }; break;
        case L"UInt64Property"_fnv: stride = 8; fmt = [](const uint8_t* p) { return std::to_string(*(uint64_t*)p); }; break;
        case L"DoubleProperty"_fnv: stride = 8; fmt = [](const uint8_t* p) { return std::format("{:.3f}", *(double*)p); }; break;
        case L"NameProperty"_fnv:   stride = sizeof(sdk::FName); fmt = [](const uint8_t* p) { try { return utility::narrow(((sdk::FName*)p)->to_string()); } catch (...) { return std::string{"<name?>"}; } }; break;
        case L"InterfaceProperty"_fnv:
        case L"ObjectProperty"_fnv:
        case L"ClassProperty"_fnv:  stride = sizeof(void*); fmt = [](const uint8_t* p) { auto o = *(sdk::UObject**)p; if (o == nullptr) return std::string{"nullptr"}; try { return std::string{"["} + std::to_string((uintptr_t)o) + "] " + utility::narrow(o->get_full_name()); } catch (...) { return std::string{"["} + std::to_string((uintptr_t)o) + "]"; } }; break;
        case L"StrProperty"_fnv:    stride = sizeof(sdk::TArrayLite<wchar_t>); fmt = [](const uint8_t* p) { const auto& s = *(const sdk::TArrayLite<wchar_t>*)p; if (s.data == nullptr || s.count <= 0) return std::string{"\"\""}; return std::string{"\""} + utility::narrow(std::wstring{s.data, (size_t)s.count}) + "\""; }; break;
        default: break;
        }
        if (stride == 0 || !fmt) {
            return std::format("[{}] <array of {}, expand in property view>", arr.count, utility::narrow(ic->get_name().to_string()));
        }
        constexpr int cap = 32;
        const int n = arr.count < cap ? arr.count : cap;
        std::string out = std::format("[{}] {{", arr.count);
        for (int i = 0; i < n; ++i) {
            if (i != 0) out += ", ";
            out += fmt(arr.data + (size_t)i * stride);
        }
        if (arr.count > cap) out += std::format(", ...(+{} more)", arr.count - cap);
        out += "}";
        return out;
    }
    case L"DelegateProperty"_fnv:
        return format_script_delegate(in);
    case L"MulticastSparseDelegateProperty"_fnv:
        return "<sparse multicast delegate>";
    case L"MulticastDelegateProperty"_fnv:
    case L"MulticastInlineDelegateProperty"_fnv:
        return format_multicast_delegate(in);
    case L"MapProperty"_fnv: {
        std::vector<std::pair<std::string, std::string>> entries;
        const int num = read_map_entries((sdk::FMapProperty*)prop, in, 16, entries);
        if (num <= 0) return num == 0 ? "<TMap: empty>" : "<TMap: unreadable>";
        if (entries.empty()) return std::format("<TMap: {} slots>", num); // not safely traversable
        std::string out = "{";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) out += ", ";
            out += entries[i].first + " => " + entries[i].second;
        }
        if (num > (int)entries.size()) out += std::format(", ...(+{} more)", num - (int)entries.size());
        out += "}";
        return out;
    }
    case L"SetProperty"_fnv: {
        std::vector<std::string> entries;
        const int num = read_set_entries((sdk::FSetProperty*)prop, in, 16, entries);
        if (num <= 0) return num == 0 ? "<TSet: empty>" : "<TSet: unreadable>";
        if (entries.empty()) return std::format("<TSet: {} slots>", num);
        std::string out = "{";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) out += ", ";
            out += entries[i];
        }
        if (num > (int)entries.size()) out += std::format(", ...(+{} more)", num - (int)entries.size());
        out += "}";
        return out;
    }
    case L"StructProperty"_fnv: {
        // Best-effort: dump well-known small POD structs in human-readable
        // form. Falls back to "<struct unsupported>" for anything else.
        const auto sp = (sdk::FStructProperty*)prop;
        const auto strukt = sp ? sp->get_struct() : nullptr;
        if (strukt == nullptr) return "<struct: unknown layout>";
        const auto sname = utility::narrow(strukt->get_fname().to_string());
        const bool is_ue5_vec = sdk::ScriptVector::static_struct() != nullptr &&
            sdk::ScriptVector::static_struct()->get_struct_size() == sizeof(glm::vec<3, double>);
        auto fmt3 = [in, is_ue5_vec]() {
            if (is_ue5_vec) {
                return std::format("({:.3f}, {:.3f}, {:.3f})", *(double*)(in+0), *(double*)(in+8), *(double*)(in+16));
            }
            return std::format("({:.3f}, {:.3f}, {:.3f})", *(float*)(in+0), *(float*)(in+4), *(float*)(in+8));
        };
        auto fmt2 = [in, is_ue5_vec]() {
            if (is_ue5_vec) {
                return std::format("({:.3f}, {:.3f})", *(double*)(in+0), *(double*)(in+8));
            }
            return std::format("({:.3f}, {:.3f})", *(float*)(in+0), *(float*)(in+4));
        };
        auto fmt4f = [in]() {
            return std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", *(float*)(in+0), *(float*)(in+4), *(float*)(in+8), *(float*)(in+12));
        };
        auto fmt4 = [in, is_ue5_vec]() {
            if (is_ue5_vec) {
                return std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", *(double*)(in+0), *(double*)(in+8), *(double*)(in+16), *(double*)(in+24));
            }
            return std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", *(float*)(in+0), *(float*)(in+4), *(float*)(in+8), *(float*)(in+12));
        };
        if (sname == "Vector" || sname == "Rotator") return "[" + sname + "] " + fmt3();
        if (sname == "Vector2D")                     return "[Vector2D] " + fmt2();
        if (sname == "LinearColor")                  return "[LinearColor] " + fmt4f();
        if (sname == "Vector4" || sname == "Quat")   return "[" + sname + "] " + fmt4();
        // Generic: dump reflected members (handles Transform etc.). A member's
        // offset is relative to the struct start, which is exactly what
        // format_return_value reads against `in`, so pass `in` as the base — the
        // composite layout (float UE4 / double UE5) comes from reflection, and
        // leaf math members fall into the cases above.
        {
            std::string out = "[" + sname + "] {";
            int shown = 0;
            for (auto f = strukt->get_child_properties(); f != nullptr && shown < 16; f = f->get_next()) {
                std::string fcname;
                try { fcname = utility::narrow(f->get_class()->get_name().to_string()); } catch (...) { continue; }
                if (!fcname.contains("Property")) continue;
                std::string mn;
                try { mn = utility::narrow(f->get_field_name().to_string()); } catch (...) { mn = "?"; }
                std::string mv;
                try { mv = format_return_value((sdk::FProperty*)f, in, 0); } catch (...) { mv = "<err>"; }
                if (shown != 0) out += ", ";
                out += mn + "=" + mv;
                ++shown;
            }
            out += "}";
            return shown == 0 ? ("<struct " + sname + ">") : out;
        }
    }
    default:
        return std::string{"<"} + utility::narrow(pc->get_name().to_string()) + " unsupported>";
    }
}

// --- FScriptMap / FScriptSet live readers (forward-declared above). ---
// FScriptMap and FScriptSet both begin with an FScriptSparseArray:
//   +0  FScriptArray { void* Data; int32 ArrayNum; int32 ArrayMax; }
//   +16 FScriptBitArray AllocationFlags (32 bytes)
//   +48 int32 FirstFreeIndex
//   +52 int32 NumFreeIndices
// We only traverse when NumFreeIndices == 0 (no removed-slot holes) so the
// allocation bitmask is never consulted — every slot in [0, ArrayNum) is live.
// Every read is IsBadReadPtr-guarded and every layout value is sanity-gated, so
// a miscalibration leaves `out` empty (caller shows the slot count) rather than
// dereferencing garbage.
namespace {
constexpr int kSparseNumFreeOffset = 52;

bool script_sparse_no_holes(const uint8_t* base, int32_t& out_num, const uint8_t*& out_data) {
    if (base == nullptr || IsBadReadPtr((void*)base, 16)) {
        return false;
    }
    const auto data = *(const uint8_t* const*)(base + 0);
    const auto num = *(const int32_t*)(base + 8);
    if (num < 0 || num > (1 << 20)) {
        return false;
    }
    if (num == 0) { out_num = 0; out_data = nullptr; return true; }
    if (data == nullptr || IsBadReadPtr((void*)data, 1)) {
        return false;
    }
    if (IsBadReadPtr((void*)(base + kSparseNumFreeOffset), sizeof(int32_t))) {
        return false;
    }
    if (*(const int32_t*)(base + kSparseNumFreeOffset) != 0) {
        return false; // has holes — would need the allocation bitmask to skip them
    }
    out_num = num;
    out_data = data;
    return true;
}
} // namespace

int read_map_entries(sdk::FMapProperty* mp, const uint8_t* base, int cap, std::vector<std::pair<std::string, std::string>>& out) {
    if (mp == nullptr || base == nullptr || IsBadReadPtr((void*)base, 16)) {
        return -1;
    }
    const int32_t arr_num = *(const int32_t*)(base + 8);
    int32_t num = 0;
    const uint8_t* data = nullptr;
    if (!script_sparse_no_holes(base, num, data)) {
        return arr_num >= 0 ? arr_num : -1; // count-only fallback
    }
    if (num == 0) {
        return 0;
    }

    auto kp = mp->get_key_prop();
    auto vp = mp->get_value_prop();
    const int32_t stride = mp->get_element_stride();
    const int32_t koff = mp->get_key_offset();
    const int32_t voff = mp->get_value_offset();
    if (kp == nullptr || vp == nullptr || stride <= 0 || stride > 4096 ||
        koff < 0 || koff >= stride || voff < 0 || voff >= stride) {
        return num; // layout failed sanity — count only
    }

    const int n = num < cap ? num : cap;
    for (int i = 0; i < n; ++i) {
        const uint8_t* elem = data + (size_t)i * stride;
        if (IsBadReadPtr((void*)elem, stride)) {
            break;
        }
        try {
            auto kstr = format_return_value(kp, elem + koff - kp->get_offset(), 0);
            auto vstr = format_return_value(vp, elem + voff - vp->get_offset(), 0);
            out.emplace_back(std::move(kstr), std::move(vstr));
        } catch (...) {
            out.emplace_back("<key error>", "<value error>");
        }
    }
    return num;
}

int read_set_entries(sdk::FSetProperty* sp, const uint8_t* base, int cap, std::vector<std::string>& out) {
    if (sp == nullptr || base == nullptr || IsBadReadPtr((void*)base, 16)) {
        return -1;
    }
    const int32_t arr_num = *(const int32_t*)(base + 8);
    int32_t num = 0;
    const uint8_t* data = nullptr;
    if (!script_sparse_no_holes(base, num, data)) {
        return arr_num >= 0 ? arr_num : -1;
    }
    if (num == 0) {
        return 0;
    }

    auto ep = sp->get_element_prop();
    const int32_t stride = sp->get_element_stride();
    const int32_t eoff = sp->get_element_offset();
    if (ep == nullptr || stride <= 0 || stride > 4096 || eoff < 0 || eoff >= stride) {
        return num;
    }

    const int n = num < cap ? num : cap;
    for (int i = 0; i < n; ++i) {
        const uint8_t* elem = data + (size_t)i * stride;
        if (IsBadReadPtr((void*)elem, stride)) {
            break;
        }
        try {
            out.emplace_back(format_return_value(ep, elem + eoff - ep->get_offset(), 0));
        } catch (...) {
            out.emplace_back("<element error>");
        }
    }
    return num;
}

// Render the interactive caller widget for a single (object, function) pair.
// Must be called when ImGui is inside an open container (e.g. between
// TreeNode("MyFunc") and the matching TreePop).
void render_function_call(sdk::UObject* self, sdk::UFunction* fn) {
    if (self == nullptr || fn == nullptr) return;
    auto& s = get_param_state(fn, self);

    auto parameters = fn->get_child_properties();

    // Iterate once for editors. Skip return params (which are an *output*).
    std::vector<sdk::FProperty*> in_params{};
    sdk::FProperty* return_prop = nullptr;
    std::vector<sdk::FProperty*> out_params{};
    for (auto p = parameters; p != nullptr; p = p->get_next()) {
        if (!p->get_class()->get_name().to_string().contains(L"Property")) continue;
        auto prop = (sdk::FProperty*)p;
        if (prop->is_return_param()) { return_prop = prop; continue; }
        if (prop->is_out_param() && !prop->is_reference_param()) {
            out_params.push_back(prop);
            continue;
        }
        in_params.push_back(prop);
    }

    if (in_params.empty() && return_prop == nullptr && out_params.empty()) {
        if (ImGui::Button("Call (no args)")) {
            std::vector<uint8_t> params{};
            params.resize(std::max<size_t>(64, (size_t)fn->get_properties_size()));
            self->process_event(fn, params.data());
            s.return_repr = "<void>";
            s.error_repr.clear();
        }
        if (!s.return_repr.empty()) {
            ImGui::TextWrapped("→ %s", s.return_repr.c_str());
        }
        if (!s.error_repr.empty()) {
            ImGui::TextColored(ImVec4{1, 0.3f, 0.3f, 1}, "err: %s", s.error_repr.c_str());
        }
        return;
    }

    for (auto prop : in_params) {
        const auto name = utility::narrow(prop->get_field_name().to_string());
        const auto name_hash = utility::hash(prop->get_class()->get_name().to_string());
        render_param_editor(s, prop, name, name_hash);
    }

    for (auto prop : out_params) {
        const auto name = utility::narrow(prop->get_field_name().to_string());
        ImGui::TextColored(ImVec4{0, 1, 0, 1}, "[Out] %s", name.c_str());
    }

    if (ImGui::Button("Call")) {
        s.error_repr.clear();
        try {
            const auto params_size = std::max<size_t>(64, (size_t)fn->get_properties_size());
            std::vector<uint8_t> params(params_size, 0);

            for (auto prop : in_params) {
                const auto name = utility::narrow(prop->get_field_name().to_string());
                const auto name_hash = utility::hash(prop->get_class()->get_name().to_string());
                if (!encode_param(s, prop, name, name_hash, params.data(), params_size)) {
                    SPDLOG_WARN("[UObjectHook] caller: unsupported param type {} for {}", name, utility::narrow(fn->get_fname().to_string()));
                }
            }

            self->process_event(fn, params.data());

            // Clear previous return-pointer state — we'll set it again below
            // if the return is an Object/Class type.
            s.return_obj = nullptr;
            s.return_class = nullptr;

            if (return_prop != nullptr) {
                s.return_repr = format_return_value(return_prop, params.data(), params_size);
                // Capture the raw pointer for the drag-source affordance.
                if (const auto rc = return_prop->get_class(); rc != nullptr) {
                    const auto rh = ::utility::hash(rc->get_name().to_string());
                    if (rh == L"ObjectProperty"_fnv || rh == L"InterfaceProperty"_fnv) {
                        s.return_obj = *(sdk::UObject**)(params.data() + return_prop->get_offset());
                    } else if (rh == L"ClassProperty"_fnv) {
                        s.return_class = *(sdk::UClass**)(params.data() + return_prop->get_offset());
                    }
                }
            } else if (!out_params.empty()) {
                std::string acc;
                for (auto* p : out_params) {
                    if (!acc.empty()) acc += " | ";
                    acc += utility::narrow(p->get_field_name().to_string()) + "=" + format_return_value(p, params.data(), params_size);
                }
                s.return_repr = std::move(acc);
            } else {
                s.return_repr = "<void>";
            }
        } catch (const std::exception& e) {
            s.error_repr = e.what();
        } catch (...) {
            s.error_repr = "unknown exception";
        }
    }

    if (!s.return_repr.empty()) {
        ImGui::TextWrapped("→ %s", s.return_repr.c_str());
    }
    // If the return value was a UObject* or UClass*, render a tiny drag
    // handle next to it so the caller can chain the return into another
    // function's parameter slot (or anywhere else that accepts the same
    // drag payload).
    if (s.return_obj != nullptr) {
        ImGui::SameLine();
        ImGui::SmallButton("drag UObject");
        make_drag_source_for_object(s.return_obj, "function return");
    }
    if (s.return_class != nullptr) {
        ImGui::SameLine();
        ImGui::SmallButton("drag UClass");
        make_drag_source_for_class(s.return_class, "function return");
    }
    if (!s.error_repr.empty()) {
        ImGui::TextColored(ImVec4{1, 0.3f, 0.3f, 1}, "err: %s", s.error_repr.c_str());
    }

    // Inline-Lua fallback (T64): a generated, editable snippet that calls this
    // function from the Lua VM, where every math type is constructible. Use for
    // params the native encoder can't pack (non-numeric structs, arrays, maps).
    // do_lua_string is void — this RUNS the call, it does not return a value to
    // the C++ caller. The template is best-effort; adapt to the game's Lua API.
    if (ImGui::TreeNode("Call via Lua (advanced / fallback)")) {
        utility::ScopeGuard lua_pop{[]() { ImGui::TreePop(); }};
        if (!s.lua_snippet_init) {
            s.lua_snippet_init = true;
            std::string full;
            try { full = utility::narrow(self->get_full_name()); } catch (...) { full = "<target>"; }
            std::string fn_short;
            try { fn_short = utility::narrow(fn->get_fname().to_string()); } catch (...) { fn_short = "<Function>"; }
            std::string sig;
            for (auto p : in_params) {
                try {
                    const auto cn = utility::narrow(p->get_class()->get_name().to_string());
                    const auto pn = utility::narrow(p->get_field_name().to_string());
                    if (!sig.empty()) sig += ", ";
                    sig += cn + " " + pn;
                } catch (...) {}
            }
            std::string tmpl;
            tmpl += "-- Editable fallback. Runs in the Lua VM (no value returned to C++).\n";
            tmpl += "-- Target: " + full + "\n";
            tmpl += "-- Function: " + fn_short + "(" + sig + ")\n";
            tmpl += "local obj = uevr.api:find_uobject(\"" + full + "\")\n";
            tmpl += "if obj == nil then print(\"target not found\") return end\n";
            tmpl += "-- edit args below (build structs with Vector()/Quaternion()/etc.):\n";
            tmpl += "local ret = obj:call(\"" + fn_short + "\"--[[, args ]])\n";
            tmpl += "print(\"" + fn_short + " ret =\", tostring(ret))\n";
            s.lua_snippet = std::move(tmpl);
        }
        if (s.lua_snippet.size() < 2048) {
            s.lua_snippet.resize(2048);
        }
        ImGui::InputTextMultiline("##luacall", s.lua_snippet.data(), s.lua_snippet.size(),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8));
        if (ImGui::Button("Run Lua")) {
            const std::string chunk(s.lua_snippet.c_str()); // trim at first NUL
            PluginLoader::get()->do_lua_string(chunk.c_str(), "uobjecthook_caller");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("editable template — adapt to your game's Lua API");
    }
}

} // namespace

std::shared_ptr<UObjectHook>& UObjectHook::get() {
    static std::shared_ptr<UObjectHook> instance = std::make_shared<UObjectHook>();
    return instance;
}

UObjectHook::MotionControllerState::~MotionControllerState() {
    if (this->adjustment_visualizer != nullptr) {
        GameThreadWorker::get().enqueue([vis = this->adjustment_visualizer]() {
            SPDLOG_INFO("[UObjectHook::MotionControllerState] Destroying adjustment visualizer for component {:x}", (uintptr_t)vis);

            if (!UObjectHook::get()->exists(vis)) {
                return;
            }

            vis->destroy_actor();

            SPDLOG_INFO("[UObjectHook::MotionControllerState] Destroyed adjustment visualizer for component {:x}", (uintptr_t)vis);
        });
    }
}

nlohmann::json UObjectHook::MotionControllerStateBase::to_json() const {
    return {
        {"rotation_offset", utility::math::to_json(rotation_offset)},
        {"location_offset", utility::math::to_json(location_offset)},
        {"hand", hand},
        {"permanent", permanent}
    };
}

void UObjectHook::MotionControllerStateBase::from_json(const nlohmann::json& data) {
    if (data.contains("rotation_offset")) {
        rotation_offset = utility::math::from_json_quat(data["rotation_offset"]);
    }

    if (data.contains("location_offset")) {
        location_offset = utility::math::from_json_vec3(data["location_offset"]);
    }

    if (data.contains("hand")) {
        hand = data["hand"].get<uint8_t>();
        hand = hand % (uint8_t)MotionControllerStateBase::Hand::LAST;
    }

    if (data.contains("permanent") && data["permanent"].is_boolean()) {
        permanent = data["permanent"].get<bool>();
    }
}

void UObjectHook::activate() {
    if (m_hooked) {
        return;
    }

    if (GameThreadWorker::get().is_same_thread()) {
        hook();
        return;
    }

    m_wants_activate = true;
}

void UObjectHook::hook() {
    if (m_hooked) {
        return;
    }

    SPDLOG_INFO("[UObjectHook] Hooking UObjectBase");

    m_hooked = true;
    m_wants_activate = false;

    auto destructor_fn = sdk::UObjectBase::get_destructor();

    if (!destructor_fn) {
        SPDLOG_ERROR("[UObjectHook] Failed to find UObjectBase::destructor, cannot hook UObjectBase");
        return;
    }

    auto add_object_fn = sdk::UObjectBase::get_add_object();

    if (!add_object_fn) {
        SPDLOG_ERROR("[UObjectHook] Failed to find UObjectBase::AddObject, cannot hook UObjectBase");
        return;
    }

    m_destructor_hook = safetyhook::create_inline((void**)destructor_fn.value(), &destructor);

    if (!m_destructor_hook) {
        SPDLOG_ERROR("[UObjectHook] Failed to hook UObjectBase::destructor, cannot hook UObjectBase");
        return;
    }

    m_add_object_hook = safetyhook::create_inline((void**)add_object_fn.value(), &add_object);

    if (!m_add_object_hook) {
        SPDLOG_ERROR("[UObjectHook] Failed to hook UObjectBase::AddObject, cannot hook UObjectBase");
        return;
    }

    SPDLOG_INFO("[UObjectHook] Hooked UObjectBase");

    // Add all the objects that already exist
    auto uobjectarray = sdk::FUObjectArray::get();

    for (auto i = 0; i < uobjectarray->get_object_count(); ++i) {
        auto object = uobjectarray->get_object(i);

        if (object == nullptr || object->get_object() == nullptr) {
            continue;
        }

        add_new_object(object->get_object());
    }

    SPDLOG_INFO("[UObjectHook] Added {} existing objects", m_objects.size());

    SPDLOG_INFO("[UObjectHook] Deserializing persistent states");
    reload_persistent_states();
    SPDLOG_INFO("[UObjectHook] Deserialized {} persistent states", m_persistent_states.size());

    m_fully_hooked = true;
}

void UObjectHook::hook_process_event() {
    if (m_attempted_hook_process_event) {
        return;
    }

    m_attempted_hook_process_event = true;

    auto uobjectarray = sdk::FUObjectArray::get();

    if (uobjectarray == nullptr) {
        return;
    }

    const auto process_event_index = sdk::UObject::get_process_event_index();

    if (process_event_index == 0) {
        return;
    }

    sdk::UObject* first_obj = nullptr;

    for (auto i = 0; i < uobjectarray->get_object_count(); ++i) {
        const auto object = uobjectarray->get_object(i);

        if (object != nullptr && object->get_object() != nullptr) {
            first_obj = (sdk::UObject*)object->get_object();
            break;
        }
    }

    if (first_obj != nullptr) {
        const auto vt = *(void***)first_obj;

        if (vt != nullptr) {
            std::scoped_lock _{m_function_mutex};
            auto fn = vt[process_event_index];
            SPDLOG_INFO("[UObjectHook] ProcessEvent {:x}", (uintptr_t)fn);
            m_process_event_hook = safetyhook::create_inline(fn, &process_event_hook);

            if (m_process_event_hook) {
                SPDLOG_INFO("[UObjectHook] Hooked UObject::ProcessEvent");
                m_hooked_process_event = true;
            } else {
                SPDLOG_ERROR("[UObjectHook] Failed to hook UObject::ProcessEvent");
            }
        }
    }
}

namespace {
    bool is_func_monitored(sdk::UFunction* fn);
    bool is_func_blocked(sdk::UFunction* fn);
    uint64_t func_call_count(sdk::UFunction* fn);
}

void* UObjectHook::process_event_hook(sdk::UObject* obj, sdk::UFunction* func, void* params, void* r9) {
    auto& hook = UObjectHook::get();

    bool do_heavy_data_once = false;

    // "Flagged only" mode restricts recording to the functions the user has
    // flagged via Monitor calls (the shared g_monitored_funcs set), so the
    // global ProcessEvent hook can act as a focused per-function monitor
    // instead of recording every call in the game.
    const bool record = hook->m_process_event_listening
        && (!hook->m_process_event_flagged_only || is_func_monitored(func));

    if (record) {
        std::scoped_lock _{hook->m_function_mutex};

        auto& data = hook->m_called_functions[func];
        ++data.call_count;

        // Per-receiver tallies for the ProcessEvent monitor's "By class"/"By caller" grouping.
        // get_class() is a cheap fixed-offset read (unlike get_owner(), which is a ProcessEvent call
        // and would be unsafe/wrong to call from inside this hook) — safe on every call.
        if (obj != nullptr) {
            try {
                if (auto* obj_cls = obj->get_class(); obj_cls != nullptr) {
                    ++data.caller_class_counts[obj_cls];
                }
            } catch (...) {}
            ++data.caller_instance_counts[obj];
        }

        hook->m_most_recent_functions.push_front(func);

        if (hook->m_most_recent_functions.size() > 200) {
            hook->m_most_recent_functions.pop_back();
        }
    }

    auto result = hook->m_process_event_hook.unsafe_call<void*>(obj, func, params, r9);

    if (record) {
        std::scoped_lock _{hook->m_function_mutex};

        auto& data = hook->m_called_functions[func];

        if (data.heavy_data == nullptr) {
            do_heavy_data_once = true;
            data.heavy_data = std::make_unique<CalledFunctionInfo::HeavyData>();

            const auto ps = func->get_properties_size();
            const auto ma = func->get_min_alignment();

            if (ma > 1) {
                data.heavy_data->params.resize(((ps + ma - 1) / ma) * ma);
            } else {
                data.heavy_data->params.resize(ps);
            }

            memset(data.heavy_data->params.data(), 0, data.heavy_data->params.size());
        }

        if ((data.wants_heavy_data || do_heavy_data_once) && !data.heavy_data->params.empty()) {
            auto bak = data.heavy_data->params;

            try {
                memcpy(data.heavy_data->params.data(), params, data.heavy_data->params.size());
            } catch(...) {
                //SPDLOG_ERROR("[UObjectHook] Failed to copy params for function {:x}", (uintptr_t)func);
                return result;
            }

            // Go through all properties and look for arrays and upgrade them so we actually own the data
            // We'll do this by copying the data to a new array
            for (auto prop = func->get_child_properties(); prop != nullptr; prop = prop->get_next()) {
                const auto c = prop->get_class();
                if (c == nullptr) {
                    continue;
                }

                const auto cname = c->get_name().to_string();
                const auto cname_hash = ::utility::hash(cname);

                switch (cname_hash) {
                case L"ArrayProperty"_fnv:
                {
                    using GenericArray = sdk::TArray<void*>;
                    const auto prop_desc = (sdk::FArrayProperty*)prop;

                    size_t inner_size = sizeof(void*);
                    bool supported = false;

                    const auto inner = prop_desc->get_inner();

                    if (inner != nullptr) {
                        const auto inner_c = inner->get_class();

                        if (inner_c != nullptr) {
                            const auto inner_cname = inner_c->get_name().to_string();
                            const auto inner_cname_hash = ::utility::hash(inner_cname);

                            // todo... recursive array stuff? good enough for now
                            switch (inner_cname_hash) {
                            case L"StructProperty"_fnv:
                            {
                                const auto s = ((sdk::FStructProperty*)inner)->get_struct();

                                if (s == nullptr) {
                                    break;
                                }

                                if (s->is_a(sdk::UScriptStruct::static_class())) {
                                    inner_size = s->get_struct_size();
                                } else {
                                    inner_size = s->get_properties_size();
                                }

                                supported = true;

                                break;
                            }
                            case L"ObjectProperty"_fnv:
                            case L"InterfaceProperty"_fnv:
                            {
                                inner_size = sizeof(void*);
                                supported = true;
                                break;
                            }

                            default:
                                break;
                            }
                        }
                    }

                    // uh oh
                    if (prop_desc->get_offset() >= data.heavy_data->params.size()) {
                        break;
                    }

                    auto& arr = *(GenericArray*)((uintptr_t)data.heavy_data->params.data() + prop_desc->get_offset());

                    if (!supported) {
                        arr.count = 0;
                        arr.capacity = 0;
                        arr.data = nullptr;
                        break;
                    }

                    auto& bak_arr = *(GenericArray*)((uintptr_t)bak.data() + prop_desc->get_offset());
                    auto new_arr = sdk::TArray<void*>{};

                    if (bak_arr.data != nullptr) {
                        new_arr = std::move(bak_arr);

                        if (arr.capacity > new_arr.capacity) {
                            new_arr.data = (void**)sdk::FMalloc::get()->realloc(new_arr.data, arr.capacity * inner_size, sizeof(void*));
                            new_arr.capacity = arr.capacity;
                        }

                        new_arr.count = arr.count;
                    } else if (arr.capacity > 0) {
                        new_arr.data = (void**)sdk::FMalloc::get()->malloc(arr.capacity * inner_size, sizeof(void*));
                        std::memset(new_arr.data, 0, arr.capacity * inner_size);
                        new_arr.count = arr.count;
                        new_arr.capacity = arr.capacity;
                    }

                    if (arr.data != nullptr && arr.count > 0 && arr.capacity >= arr.count && new_arr.data != nullptr && !IsBadReadPtr((void*)arr.data, arr.capacity * inner_size)) {
                        memcpy(new_arr.data, arr.data, arr.count * inner_size);
                        arr.data = nullptr;
                        arr.count = 0;
                        arr.capacity = 0;
                        arr = std::move(new_arr);
                    } else {
                        arr.count = 0;
                    }
                }
                    break; // MUST break — without it ArrayProperty fell through into StrProperty and
                           // reinterpreted the array's data as an FString, realloc'ing a non-FMalloc /
                           // already-moved block (FMallocBinned2 "unrecognized block" heap-corruption crash).
                case L"StrProperty"_fnv:
                {
                    using FString = sdk::TArray<wchar_t>;

                    const auto prop_desc = (sdk::FProperty*)prop;

                    // uh oh
                    if (prop_desc->get_offset() >= data.heavy_data->params.size()) {
                        break;
                    }

                    auto& str = *(FString*)((uintptr_t)data.heavy_data->params.data() + prop_desc->get_offset());
                    auto& bak_str = *(FString*)((uintptr_t)bak.data() + prop_desc->get_offset());

                    auto new_str = FString{};

                    // Same thing but much simpler because we know it's wchar_t
                    if (bak_str.data != nullptr) {
                        new_str = std::move(bak_str);

                        if (str.capacity > new_str.capacity) {
                            new_str.data = (wchar_t*)sdk::FMalloc::get()->realloc(new_str.data, str.capacity * sizeof(wchar_t), sizeof(wchar_t));
                            new_str.capacity = str.capacity;
                        }

                        new_str.count = str.count;
                    } else if (str.capacity > 0) {
                        new_str.data = (wchar_t*)sdk::FMalloc::get()->malloc(str.capacity * sizeof(wchar_t), sizeof(wchar_t));
                        std::memset(new_str.data, 0, str.capacity * sizeof(wchar_t));
                        new_str.count = str.count;
                        new_str.capacity = str.capacity;
                    }

                    if (str.data != nullptr && str.count > 0 && str.capacity >= str.count && new_str.data != nullptr && !IsBadReadPtr((void*)str.data, str.capacity * sizeof(wchar_t))) {
                        memcpy(new_str.data, str.data, str.count * sizeof(wchar_t));
                        str.data = nullptr;
                        str.count = 0;
                        str.capacity = 0;
                        str = std::move(new_str);
                    } else {
                        str.count = 0;
                    }
                }
                    break;
                default:
                    break;
                };
            }
        }
    }

    return result;
}

void UObjectHook::add_new_object(sdk::UObjectBase* object) {
    std::unique_lock _{m_mutex};
    std::unique_ptr<MetaObject> meta_object{};

    const auto c = object->get_class();

    if (c == nullptr) {
        return;
    }

    if (!m_reusable_meta_objects.empty()) {
        meta_object = std::move(m_reusable_meta_objects.back());
        m_reusable_meta_objects.pop_back();
    } else {
        meta_object = std::make_unique<MetaObject>();
        meta_object->super_classes.reserve(16);
    }

    m_objects.insert(object);
    meta_object->super_classes.clear();
    meta_object->full_name = object->get_full_name();
    meta_object->uclass = object->get_class();

    m_most_recent_objects.push_front((sdk::UObject*)object);

    if (m_most_recent_objects.size() > 50) {
        m_most_recent_objects.pop_back();
    }

    for (auto super = (sdk::UStruct*)object->get_class(); super != nullptr; super = super->get_super_struct()) {
        meta_object->super_classes.push_back((sdk::UClass*)super);

        m_objects_by_class[(sdk::UClass*)super].insert(object);

        if (auto it = m_on_creation_add_component_jobs.find((sdk::UClass*)super); it != m_on_creation_add_component_jobs.end()) {
            GameThreadWorker::get().enqueue([object, this]() {
                if (!this->exists(object)) {
                    return;
                }

                for (auto super = (sdk::UStruct*)object->get_class(); super != nullptr; super = super->get_super_struct()) {
                    std::function<void(sdk::UObject*)> job{};

                    {
                        std::scoped_lock _{m_add_component_jobs_mtx}; // matches queue_add's writer mutex (not m_mutex)

                        if (auto it = this->m_on_creation_add_component_jobs.find((sdk::UClass*)super); it != this->m_on_creation_add_component_jobs.end()) {
                            job = it->second;
                        }
                    }

                    if (job) {
                        job((sdk::UObject*)object);
                    }
                }
            });
        }
    }

    m_meta_objects[object] = std::move(meta_object);

#ifdef VERBOSE_UOBJECTHOOK
    SPDLOG_INFO("Adding object {:x} {:s}", (uintptr_t)object, utility::narrow(m_meta_objects[object]->full_name));
#endif
}

void UObjectHook::on_config_load(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    // Restore the plain gizmo/inspector toggles (see on_config_save).
    if (!set_defaults) {
        if (auto v = cfg.get<int>("UObjectHook_GizmoMode")) m_gizmo_mode = *v;
        if (auto v = cfg.get<float>("UObjectHook_GizmoThickness")) m_gizmo_thickness = *v;
        if (auto v = cfg.get<float>("UObjectHook_GizmoAxisLen")) m_gizmo_axis_len = *v;
        if (auto v = cfg.get<float>("UObjectHook_GizmoRingRadius")) m_gizmo_ring_radius = *v;
        if (auto v = cfg.get<bool>("UObjectHook_GizmoLocal")) m_gizmo_local = *v;
        if (auto v = cfg.get<bool>("UObjectHook_HideGizmosWhenUiClosed")) m_hide_gizmos_when_ui_closed = *v;
        if (auto v = cfg.get<bool>("UObjectHook_BlockPassthroughWhenGizmosVisible")) m_block_passthrough_when_gizmos_visible = *v;
        if (auto v = cfg.get<bool>("UObjectHook_GizmoShowLabels")) m_gizmo_show_labels = *v;
        if (auto v = cfg.get<bool>("UObjectHook_AutoGizmoOnAdjust")) m_auto_gizmo_on_adjust = *v;
        // Overlay-material highlight menu was removed (unreliable — SetOverlayMaterial silently
        // no-ops on UE4 and the "restore on deselect" bookkeeping never fully worked). Deliberately
        // NOT loading a saved true value here so it can't come back enabled from an old config; the
        // uobjecthook_gizmo_target Lua event is the documented way to build an equivalent by hand now.
        if (auto v = cfg.get<bool>("UObjectHook_ShowTexturePreviews")) m_show_texture_previews = *v;
    }

    if (!set_defaults && m_enabled_at_startup->value()) {
        m_wants_activate = true;
    }
}

void UObjectHook::on_config_save(utility::Config& cfg) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }

    // Persist the plain gizmo/inspector toggles (these are not ModValues) so the Config-tab
    // settings survive restarts.
    cfg.set<int>("UObjectHook_GizmoMode", m_gizmo_mode);
    cfg.set<float>("UObjectHook_GizmoThickness", m_gizmo_thickness);
    cfg.set<float>("UObjectHook_GizmoAxisLen", m_gizmo_axis_len);
    cfg.set<float>("UObjectHook_GizmoRingRadius", m_gizmo_ring_radius);
    cfg.set<bool>("UObjectHook_GizmoLocal", m_gizmo_local);
    cfg.set<bool>("UObjectHook_HideGizmosWhenUiClosed", m_hide_gizmos_when_ui_closed);
    cfg.set<bool>("UObjectHook_BlockPassthroughWhenGizmosVisible", m_block_passthrough_when_gizmos_visible);
    cfg.set<bool>("UObjectHook_GizmoShowLabels", m_gizmo_show_labels);
    cfg.set<bool>("UObjectHook_AutoGizmoOnAdjust", m_auto_gizmo_on_adjust);
    cfg.set<bool>("UObjectHook_HighlightOverlayMaterial", m_highlight_overlay_material);
    cfg.set<bool>("UObjectHook_ShowTexturePreviews", m_show_texture_previews);
}

void UObjectHook::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    m_last_delta_time = delta;

    if (m_wants_activate) {
        hook();
    }

    if (m_fully_hooked) {
        {
            std::shared_lock _{m_mutex};
            const auto ui_active = g_framework->is_drawing_ui();

            for (auto& state : m_motion_controller_attached_components) {
                if (m_overlap_detection_actor == nullptr && state.second->adjusting && ui_active) {
                    state.second->adjusting = false;
                }
            }
        }

        update_persistent_states();
    }
}

const auto quat_converter = glm::quat{Matrix4x4f {
    0, 0, -1, 0,
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 0, 1
}};

// TODO: split this into some functions because its getting a bit massive
void UObjectHook::on_pre_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation,
                                        const float world_to_meters, Vector3f* view_location, bool is_double)
{
    if (!m_fully_hooked) {
        return;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return;
    }

    if (m_uobject_hook_disabled) {
        return;
    }

    auto view_d = (Vector3d*)view_location;
    auto rot_d = (Rotator<double>*)view_rotation;

    if (is_double) {
        m_last_camera_location = glm::vec3{*view_d};
    } else {
        m_last_camera_location = *view_location;
    }

    m_last_world_to_meters = world_to_meters; // for get_nearest_gizmo_distance_meters (benign race)

    if (m_camera_attach.object != nullptr) {
        if (m_camera_attach.object->is_a(sdk::AActor::static_class())) {
            const auto actor = (sdk::AActor*)m_camera_attach.object;
            const auto location = actor->get_actor_location();
            const auto rotation = actor->get_actor_rotation();

            // Conv rotation to glm quat
            const auto rotation_glm_mat = glm::yawPitchRoll(
                glm::radians(-rotation.y),
                glm::radians(rotation.x),
                glm::radians(-rotation.z));
            const auto rotation_glm_quat = glm::quat{rotation_glm_mat};

            const auto adjusted_loc = location - (quat_converter * (rotation_glm_quat * utility::math::ue4_to_glm(m_camera_attach.offset)));

            if (is_double) {
                *view_d = glm::vec<3, double>{adjusted_loc};
            } else {
                *view_location = adjusted_loc;
            }
        } else if (m_camera_attach.object->is_a(sdk::USceneComponent::static_class())) {
            const auto comp = (sdk::USceneComponent*)m_camera_attach.object;
            const auto location = comp->get_world_location();
            const auto rotation = comp->get_world_rotation();

            // Conv rotation to glm quat
            const auto rotation_glm_mat = glm::yawPitchRoll(
                glm::radians(-rotation.y),
                glm::radians(rotation.x),
                glm::radians(-rotation.z));
            const auto rotation_glm_quat = glm::quat{rotation_glm_mat};

            const auto adjusted_loc = location - (quat_converter * (rotation_glm_quat * utility::math::ue4_to_glm(m_camera_attach.offset)));

            if (is_double) {
                *view_d = glm::vec<3, double>{adjusted_loc};
            } else {
                *view_location = adjusted_loc;
            }
        } // else todo?
    }

    if ((view_index + 1) % 2 == 0) {
        tick_attachments(view_rotation, world_to_meters, view_location, is_double);
    }
}

void UObjectHook::on_post_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation,
                                                    const float world_to_meters, Vector3f* view_location, bool is_double)
{
    if (!m_fully_hooked) {
        return;
    }

    if (!VR::get()->is_hmd_active()) {
        return;
    }

    if (m_uobject_hook_disabled) {
        return;
    }

    std::shared_lock _{m_mutex};
    bool any_adjusting = false;
    for (auto& it : m_motion_controller_attached_components) {
        if (it.second->adjusting) {
            any_adjusting = true;
            break;
        }
    }

    VR::get()->set_aim_allowed(!any_adjusting);
}

void UObjectHook::tick_attachments(Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location, bool is_double) {
    if (m_uobject_hook_disabled) {
        return;
    }

    auto& vr = VR::get();
    auto view_d = (Vector3d*)view_location;
    auto rot_d = (Rotator<double>*)view_rotation;

    const auto view_mat_inverse = !is_double ?
        glm::yawPitchRoll(
            glm::radians(-view_rotation->yaw),
            glm::radians(view_rotation->pitch),
            glm::radians(-view_rotation->roll)) :
        glm::yawPitchRoll(
            glm::radians(-(float)rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians(-(float)rot_d->roll));

    const auto view_quat_inverse = glm::quat {
        view_mat_inverse
    };

    auto vqi_norm = glm::normalize(view_quat_inverse);

    // Decoupled Pitch
    if (vr->is_decoupled_pitch_enabled()) {
        vqi_norm = utility::math::flatten(vqi_norm);
    }

    const auto rotation_offset = vr->get_rotation_offset();
    const auto hmd_origin = glm::vec3{vr->get_transform(0)[3]};
    const auto pos = glm::vec3{rotation_offset * (hmd_origin - glm::vec3{vr->get_standing_origin()})};

    const auto adjusted_world_to_meters = world_to_meters * vr->get_world_scale();

    const auto view_quat_inverse_flat = utility::math::flatten(view_quat_inverse);
    const auto offset1 = quat_converter * (glm::normalize(view_quat_inverse_flat) * (pos * adjusted_world_to_meters));

    glm::vec3 final_position{};

    if (is_double) {
        final_position = glm::vec3{*view_d} - offset1;
    } else {
        final_position = *view_location - offset1;
    }

    auto with_mutex = [this](auto fn) {
        std::shared_lock _{m_mutex};
        auto result = fn();

        return result;
    };

    auto comps = with_mutex([this]() { return m_motion_controller_attached_components; });

    const auto is_using_controllers = vr->is_using_controllers();
    const auto has_any_head_components = std::any_of(comps.begin(), comps.end(), [](auto& it) { return it.second->hand == 2; });

    if (!is_using_controllers && !has_any_head_components) {
        return;
    }

    glm::vec3 right_hand_position = vr->get_grip_position(vr->get_right_controller_index());
    glm::quat right_hand_rotation = vr->get_aim_rotation(vr->get_right_controller_index());

    const float lerp_speed = m_attach_lerp_speed->value() * m_last_delta_time;

    if (m_attach_lerp_enabled->value()) {
        auto spherical_distance_right = glm::dot(right_hand_rotation, m_last_right_aim_rotation);

        if (spherical_distance_right < 0.0f) {
            spherical_distance_right = -spherical_distance_right;
        }

        const auto lenr = glm::max(1.0f, glm::length(right_hand_position - m_last_right_grip_location));
        m_last_right_grip_location = glm::lerp(m_last_right_grip_location, right_hand_position, glm::min(1.0f, lerp_speed * lenr));
        m_last_right_aim_rotation = glm::slerp(m_last_right_aim_rotation, right_hand_rotation, lerp_speed * spherical_distance_right);
        right_hand_position = m_last_right_grip_location;
        right_hand_rotation = m_last_right_aim_rotation;
    }

    const auto original_right_hand_rotation = right_hand_rotation;
    const auto original_right_hand_position = right_hand_position - hmd_origin;

    glm::vec3 left_hand_position = vr->get_grip_position(vr->get_left_controller_index());
    glm::quat left_hand_rotation = vr->get_aim_rotation(vr->get_left_controller_index());

    if (m_attach_lerp_enabled->value()) {
        auto spherical_distance_left = glm::dot(left_hand_rotation, m_last_left_aim_rotation);

        if (spherical_distance_left < 0.0f) {
            spherical_distance_left = -spherical_distance_left;
        }

        const auto lenl = glm::max(1.0f, glm::length(left_hand_position - m_last_left_grip_location));
        m_last_left_grip_location = glm::lerp(m_last_left_grip_location, left_hand_position, glm::min(1.0f, lerp_speed * lenl));
        m_last_left_aim_rotation = glm::slerp(m_last_left_aim_rotation, left_hand_rotation, lerp_speed * spherical_distance_left);
        left_hand_position = m_last_left_grip_location;
        left_hand_rotation = m_last_left_aim_rotation;
    }

    right_hand_position = glm::vec3{rotation_offset * (right_hand_position - hmd_origin)};
    left_hand_position = glm::vec3{rotation_offset * (left_hand_position - hmd_origin)};

    right_hand_position = quat_converter * (glm::normalize(view_quat_inverse_flat) * (right_hand_position * adjusted_world_to_meters));
    left_hand_position = quat_converter * (glm::normalize(view_quat_inverse_flat) * (left_hand_position * adjusted_world_to_meters));

    right_hand_position = final_position - right_hand_position;
    left_hand_position = final_position - left_hand_position;

    right_hand_rotation = rotation_offset * right_hand_rotation;
    right_hand_rotation = (glm::normalize(view_quat_inverse_flat) * right_hand_rotation);

    left_hand_rotation = rotation_offset * left_hand_rotation;
    left_hand_rotation = (glm::normalize(view_quat_inverse_flat) * left_hand_rotation);

    //right_hand_rotation = glm::normalize(right_hand_rotation * right_hand_offset_q);
    auto right_hand_euler = glm::degrees(utility::math::euler_angles_from_steamvr(right_hand_rotation));

    //left_hand_rotation = glm::normalize(left_hand_rotation * left_hand_offset_q);
    auto left_hand_euler = glm::degrees(utility::math::euler_angles_from_steamvr(left_hand_rotation));

    const auto head_rotation =  glm::normalize(vqi_norm * (rotation_offset * glm::quat{vr->get_rotation(0)}));
    const auto head_euler = glm::degrees(utility::math::euler_angles_from_steamvr(head_rotation));

    update_motion_controller_components(
        final_position, head_euler,
        left_hand_position, left_hand_euler,
        right_hand_position, right_hand_euler);

    sdk::TArray<sdk::UPrimitiveComponent*> overlapped_components{};
    sdk::TArray<sdk::UPrimitiveComponent*> overlapped_components_left{};

    // Update overlapped components and overlap actor transform
    if (m_overlap_detection_actor != nullptr && this->exists(m_overlap_detection_actor)) {
        m_overlap_detection_actor->set_actor_location(right_hand_position, false, false);
        m_overlap_detection_actor->set_actor_rotation(right_hand_euler, false);

        //if (!g_framework->is_drawing_ui()) {
         overlapped_components = std::move(m_overlap_detection_actor->get_overlapping_components());
  //      }
    }

    // Update overlapped components and overlap actor transform (left)
    if (m_overlap_detection_actor_left != nullptr && this->exists(m_overlap_detection_actor_left)) {
        m_overlap_detection_actor_left->set_actor_location(left_hand_position, false, false);
        m_overlap_detection_actor_left->set_actor_rotation(left_hand_euler, false);

        if (!g_framework->is_drawing_ui()) {
            overlapped_components_left = std::move(m_overlap_detection_actor_left->get_overlapping_components());
        }
    }

    // Check intuitive attachment for overlapped components
    if (!g_framework->is_drawing_ui() && (!overlapped_components.empty() || !overlapped_components_left.empty())) {
        static bool prev_right_a_pressed = false;
        static bool prev_left_a_pressed = false;
        const auto is_a_down_raw_right = vr->is_action_active_any_joystick(vr->get_action_handle(VR::s_action_a_button_right));
        const auto was_a_pressed_right = !prev_right_a_pressed && is_a_down_raw_right;

        const auto is_a_down_raw_left = vr->is_action_active_any_joystick(vr->get_action_handle(VR::s_action_a_button_left));
        const auto was_a_pressed_left = !prev_left_a_pressed && is_a_down_raw_left;

        prev_right_a_pressed = is_a_down_raw_right;
        prev_left_a_pressed = is_a_down_raw_left;

        // Update existing attached components before moving onto overlapped ones.
        for (auto& it : comps) {
            auto& state = *it.second;

            if (state.hand == (uint8_t)MotionControllerStateBase::Hand::LEFT) {
                if (is_a_down_raw_left) {
                    state.adjusting = true;
                } else if (!is_a_down_raw_left) {
                    state.adjusting = false;
                }
            } else if (state.hand == (uint8_t)MotionControllerStateBase::Hand::RIGHT) {
                if (is_a_down_raw_right) {
                    state.adjusting = true;
                } else if (!is_a_down_raw_right) {
                    state.adjusting = false;
                }
            } else {
                state.adjusting = false;
            }
        }

        auto update_overlaps = [&](int32_t hand, const sdk::TArray<sdk::UPrimitiveComponent*>& components) {
            const auto was_pressed = hand == 0 ? was_a_pressed_left : was_a_pressed_right;
            const auto is_pressed = hand == 0 ? is_a_down_raw_left : is_a_down_raw_right;

            for (auto overlap : components) {
                static const auto capsule_component_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.CapsuleComponent");
                if (overlap->get_class()->is_a(capsule_component_t)) {
                    continue;
                }

                {
                    std::shared_lock _{m_mutex};
                    if (m_spawned_spheres_to_components.contains(overlap)) {
                        overlap = (sdk::UPrimitiveComponent*)m_spawned_spheres_to_components[overlap];
                    }
                }

                const auto owner = overlap->get_owner();
                bool owner_is_adjustment_vis = false;

                if (owner == m_overlap_detection_actor || owner == m_overlap_detection_actor_left) {
                    continue;
                }

                // Make sure we don't try to attach to the adjustment visualizer
                {
                    std::shared_lock _{m_mutex};
                    auto it = std::find_if(m_motion_controller_attached_components.begin(), m_motion_controller_attached_components.end(),
                        [&](auto& it) {
                            if (it.second->adjustment_visualizer != nullptr && this->exists_unsafe(it.second->adjustment_visualizer)) {
                                if (it.second->adjustment_visualizer == owner) {
                                    owner_is_adjustment_vis = true;
                                    return true;
                                }
                            }

                            return false;
                        });

                    if (owner_is_adjustment_vis) {
                        continue;
                    }
                }

                if (was_pressed) {
                    auto state = get_or_add_motion_controller_state(overlap);
                    state->adjusting = true;
                    state->hand = hand;
                } /*else if (!is_pressed) {
                    auto state = get_motion_controller_state(overlap);

                    if (state && (*state)->hand == hand) {
                        (*state)->adjusting = false;
                    }
                }*/
            }
        };

        update_overlaps(0, overlapped_components_left);
        update_overlaps(1, overlapped_components);
    }

    for (auto& it : comps) {
        if (!is_using_controllers && it.second->hand != 2) {
            continue;
        }

        auto comp = it.first;
        if (!this->exists(comp) || it.second == nullptr) {
            continue;
        }

        auto& state = *it.second;
        const auto orig_position = comp->get_world_location();
        const auto orig_rotation = comp->get_world_rotation();

        // Convert orig_rotation to quat
        const auto orig_rotation_mat = glm::yawPitchRoll(
            glm::radians(-orig_rotation.y),
            glm::radians(orig_rotation.x),
            glm::radians(-orig_rotation.z));
        const auto orig_rotation_quat = glm::quat{orig_rotation_mat};

        using Hand = MotionControllerStateBase::Hand;
        const auto& hand_rotation = state.hand != Hand::HMD ? (state.hand == Hand::RIGHT ? right_hand_rotation : left_hand_rotation) : head_rotation;
        const auto& hand_position = state.hand != Hand::HMD ? (state.hand == Hand::RIGHT ? right_hand_position : left_hand_position) : final_position;
        const auto& hand_euler = state.hand != Hand::HMD ? (state.hand == Hand::RIGHT ? right_hand_euler : left_hand_euler) : head_euler;

        const auto adjusted_rotation = hand_rotation * glm::inverse(state.rotation_offset);
        const auto adjusted_euler = glm::degrees(utility::math::euler_angles_from_steamvr(adjusted_rotation));
        const auto adjusted_location = hand_position + (quat_converter * (adjusted_rotation * state.location_offset));

        // Flat-gizmo routing: while this attached component is being dragged by the screen-space gizmo,
        // let the gizmo own the world transform — skip the follow + the non-permanent reset below that
        // would clobber it, and continuously re-seed the attach offset from the gizmo-updated transform
        // (the exact capture math the controller-adjust path uses, see the state.adjusting branch) so the
        // follow holds the new pose once the drag ends.
        if (comp == m_flat_gizmo_drag_comp.load()) {
            std::unique_lock _{m_mutex};
            const auto mqi = glm::inverse(orig_rotation_quat);
            state.rotation_offset = mqi * hand_rotation;
            state.location_offset = mqi * utility::math::ue4_to_glm(hand_position - orig_position);
            continue;
        }

        if (state.adjusting) {
            // Create a temporary actor that visualizes how we're adjusting the component
            if (state.adjustment_visualizer == nullptr) {
                auto ugs = sdk::UGameplayStatics::get();
                auto visualizer = ugs->spawn_actor(sdk::UGameEngine::get()->get_world(), sdk::AActor::static_class(), orig_position);

                if (visualizer != nullptr) {
                    // Store the visualizer BEFORE building its components: the 72-box loop below
                    // calls process_event / property reads that can throw, and if it aborts before
                    // the store, the actor is never recorded -> it re-spawns (72 box components)
                    // EVERY frame -> the perf hit + actor leak. Storing first makes the
                    // "== nullptr" gate hold even if the build partially fails.
                    {
                        std::unique_lock _{m_mutex};
                        state.adjustment_visualizer = visualizer;
                    }
                    auto add_comp = [&](sdk::UClass* c, std::function<void(sdk::UActorComponent*)> fn) -> sdk::UActorComponent* {
                        if (c == nullptr) {
                            SPDLOG_ERROR("[UObjectHook] Cannot add component of null class");
                            return nullptr;
                        }

                        auto new_comp = visualizer->add_component_by_class(c);

                        if (new_comp != nullptr) {
                            fn(new_comp);

                            if (new_comp->is_a(sdk::USceneComponent::static_class())) {
                                auto scene_comp = (sdk::USceneComponent*)new_comp;
                                scene_comp->set_hidden_in_game(false);
                            }

                            visualizer->finish_add_component(new_comp);
                        } else {
                            SPDLOG_ERROR("[UObjectHook] Failed to add component {} to adjustment visualizer", utility::narrow(c->get_full_name()));
                        }

                        return new_comp;
                    };

                    add_comp(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SphereComponent"), [](sdk::UActorComponent* new_comp) {
                        struct SphereRadiusParams {
                            float radius{};
                            bool update_overlaps{false};
                        };

                        auto params = SphereRadiusParams{};
                        params.radius = 10.f;

                        const auto fn = new_comp->get_class()->find_function(L"SetSphereRadius");

                        if (fn != nullptr) {
                            new_comp->process_event(fn, &params);
                        }
                    });

                    // Ghetto way of making a "mesh" out of the box components
                    for (auto j = 0; j < 3; ++j) {
                        for (auto i = 1; i < 25; ++i) {
                            add_comp(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.BoxComponent"), [i, j](sdk::UActorComponent* new_comp) {
                                auto color = (uint8_t*)new_comp->get_property_data(L"ShapeColor");

                                if (color != nullptr) {
                                    color[0] = 255;
                                    color[1] = 255;
                                    color[2] = 0;
                                    color[3] = 0;
                                }

                                auto extent = (void*)new_comp->get_property_data(L"BoxExtent");

                                if (extent != nullptr) {
                                    static const bool is_ue5 = sdk::ScriptVector::static_struct()->get_struct_size() == sizeof(glm::vec<3, double>);

                                    const auto ratio = (float)i / 100.0f;

                                    const auto x = j == 0 ? ratio * 25.0f : 25.0f;
                                    const auto y = j == 1 ? ratio * 5.0f : 5.0f;
                                    const auto z = j == 2 ? ratio * 5.0f : 5.0f;

                                    glm::vec3 wanted_ext = glm::vec3{x, y, z};

                                    if (is_ue5) {
                                        *((glm::vec<3, double>*)extent) = wanted_ext;
                                    } else {
                                        *((glm::vec3*)extent) = wanted_ext;
                                    }
                                }
                            });
                        }
                    }

                    //ugs->finish_spawning_actor(visualizer, orig_position);
                    // (visualizer was already stored above, before the component build)
                } else {
                    SPDLOG_ERROR("[UObjectHook] Failed to spawn actor for adjustment visualizer");
                }
            } else {
                state.adjustment_visualizer->set_actor_location(hand_position, false, false);
                state.adjustment_visualizer->set_actor_rotation(hand_euler, false);
            }

            std::unique_lock _{m_mutex};
            const auto mat_inverse =
                glm::yawPitchRoll(
                    glm::radians(-orig_rotation.y),
                    glm::radians(orig_rotation.x),
                    glm::radians(-orig_rotation.z));

            const auto mq = glm::quat{mat_inverse};
            const auto mqi = glm::inverse(mq);

            state.rotation_offset = mqi * hand_rotation;
            state.location_offset = mqi * utility::math::ue4_to_glm(hand_position - orig_position);
        } else {
            if (state.adjustment_visualizer != nullptr) {
                state.adjustment_visualizer->destroy_actor();

                std::unique_lock _{m_mutex};
                state.adjustment_visualizer = nullptr;
            }

            // The shared gizmo mode (set in the flat overlay) also drives the VR
            // controller-attached behavior: Move follows the hand 6DOF (unchanged
            // default), Rotate follows only the hand's rotation, Scale grows/shrinks
            // uniformly as the hand moves away from / toward the component. The flat
            // imgui gizmo can't world-align in VR (it lives on the overlay quad), so
            // VR manipulation reuses this verified controller-attach path instead.
            if (m_gizmo_mode == 1) {
                comp->set_world_rotation(adjusted_euler, false, false);
            } else if (m_gizmo_mode == 2) {
                // Only scale PERMANENT attachments: the non-permanent reset below
                // restores location/rotation each frame but not scale, so writing
                // scale on a preview attachment would accumulate (leak). Dropping the
                // baseline when not scaling means a later scale re-seeds with delta 0
                // (no pop) on its first frame.
                static std::unordered_map<sdk::USceneComponent*, float> s_scale_last_dist;
                if (state.permanent) {
                    const float dist = glm::length(hand_position - orig_position);
                    if (auto it_d = s_scale_last_dist.find(comp); it_d != s_scale_last_dist.end()) {
                        auto sc = comp->get_relative_scale();
                        sc += glm::vec3{(dist - it_d->second) * 0.01f};
                        comp->set_relative_scale(sc);
                    }
                    s_scale_last_dist[comp] = dist;
                } else {
                    s_scale_last_dist.erase(comp);
                }
            } else {
                comp->set_world_location(adjusted_location, false, false);
                comp->set_world_rotation(adjusted_euler, false, false);
            }
        }

        if (!state.permanent) {
            GameThreadWorker::get().enqueue([this, comp, orig_position, orig_rotation]() {

                if (!this->exists(comp)) {
                    return;
                }

                comp->set_world_location(orig_position, false, false);
                comp->set_world_rotation(orig_rotation, false, false);
            });
            GameThreadWorker::get().execute();
        }
    }
}

void UObjectHook::spawn_overlapper(uint32_t hand) {
    GameThreadWorker::get().enqueue(
[this, hand]() {
        auto ugs = sdk::UGameplayStatics::get();
        auto world = sdk::UGameEngine::get()->get_world();

        if (ugs == nullptr || world == nullptr) {
            return;
        }

        auto overlapper = ugs->spawn_actor(world, sdk::AActor::static_class(), glm::vec3{0, 0, 0});

        if (hand == 1) {
            if (m_overlap_detection_actor != nullptr && this->exists(m_overlap_detection_actor)) {
                m_overlap_detection_actor->destroy_actor();
            }

            m_overlap_detection_actor = overlapper;
        } else {
            if (m_overlap_detection_actor_left != nullptr && this->exists(m_overlap_detection_actor_left)) {
                m_overlap_detection_actor_left->destroy_actor();
            }

            m_overlap_detection_actor_left = overlapper;
        }

        if (overlapper != nullptr) {
            auto add_comp = [&](sdk::AActor* target, sdk::UClass* c, std::function<void(sdk::UActorComponent*)> fn) -> sdk::UActorComponent* {
                if (c == nullptr) {
                    SPDLOG_ERROR("[UObjectHook] Cannot add component of null class");
                    return nullptr;
                }

                auto new_comp = target->add_component_by_class(c);

                if (new_comp != nullptr) {
                    fn(new_comp);

                    if (new_comp->is_a(sdk::USceneComponent::static_class())) {
                        auto scene_comp = (sdk::USceneComponent*)new_comp;
                        scene_comp->set_hidden_in_game(false);
                    }

                    target->finish_add_component(new_comp);
                } else {
                    SPDLOG_ERROR("[UObjectHook] Failed to add component {} to target", utility::narrow(c->get_full_name()));
                }

                return new_comp;
            };

            const auto sphere_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SphereComponent");

            add_comp(overlapper, sphere_t, [](sdk::UActorComponent* new_comp) {
                struct SphereRadiusParams {
                    float radius{};
                    bool update_overlaps{true};
                } params{};

                params.radius = 10.0f;

                const auto fn = new_comp->get_class()->find_function(L"SetSphereRadius");

                if (fn != nullptr) {
                    new_comp->process_event(fn, &params);
                }
            });

            const auto skeletal_mesh_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SkeletalMeshComponent");
            const auto meshes = get_objects_by_class(skeletal_mesh_t);

            for (auto obj : meshes) {
                if (!this->exists(obj)) {
                    continue;
                }

                // Dont add the same one twice
                if (m_components_with_spheres.contains((sdk::USceneComponent*)obj)) {
                    continue;
                }

                const auto default_obj = obj->get_class()->get_class_default_object();

                if (obj == default_obj) {
                    continue;
                }

                SPDLOG_INFO("[UObjectHook] Spawning sphere for skeletal mesh {}", utility::narrow(obj->get_full_name()));

                auto mesh = (sdk::USceneComponent*)obj;
                auto owner = mesh->get_owner();

                if (owner == nullptr) {
                    continue;
                }

                const auto owner_default = owner->get_class()->get_class_default_object();

                if (owner == owner_default) {
                    continue;
                }

                SPDLOG_INFO("[UObjectHook] Owner of skeletal mesh is {}", utility::narrow(owner->get_full_name()));

                auto new_sphere = (sdk::USceneComponent*)add_comp(owner, sphere_t, [](sdk::UActorComponent* new_comp) {
                    struct SphereRadiusParams {
                        float radius{};
                        bool update_overlaps{true};
                    } params{};

                    params.radius = 10.0f;

                    const auto fn = new_comp->get_class()->find_function(L"SetSphereRadius");

                    if (fn != nullptr) {
                        new_comp->process_event(fn, &params);
                    }
                });

                if (new_sphere != nullptr) {
                    new_sphere->attach_to(mesh, L"None", 0, true);
                //   new_sphere->set_local_transform(glm::vec3{}, glm::vec4{0, 0, 0, 1}, glm::vec3{1, 1, 1});

                    new_sphere->set_local_transform(glm::vec3{0, 0, 82}, glm::vec4{0, 0, 0, 1}, glm::vec3{1, 1, 1});

                    std::unique_lock _{m_mutex};
                    m_spawned_spheres.insert(new_sphere);
                    m_spawned_spheres_to_components[new_sphere] = mesh;
                    m_components_with_spheres.insert(mesh);
                }
            }
        } else {
            SPDLOG_ERROR("[UObjectHook] Failed to spawn actor for overlapper");
        }
    });
    GameThreadWorker::get().execute();
}

void UObjectHook::destroy_overlapper() {
    GameThreadWorker::get().enqueue([this]() {
        // Destroy all spawned spheres
        auto spheres = get_spawned_spheres();

        for (auto sphere : spheres) {
            if (!this->exists(sphere)) {
                continue;
            }

            sphere->destroy_component();
        }

        {
            std::unique_lock _{m_mutex};
            m_spawned_spheres.clear();
            m_spawned_spheres_to_components.clear();
            m_components_with_spheres.clear();
        }

        if (m_overlap_detection_actor != nullptr && this->exists(m_overlap_detection_actor)) {
            m_overlap_detection_actor->destroy_actor();
            m_overlap_detection_actor = nullptr;
        }

        if (m_overlap_detection_actor_left != nullptr && this->exists(m_overlap_detection_actor_left)) {
            m_overlap_detection_actor_left->destroy_actor();
            m_overlap_detection_actor_left = nullptr;
        }
    });
}

std::filesystem::path UObjectHook::get_persistent_dir() {
    const auto base_dir = Framework::get_persistent_dir();
    const auto uobjecthook_dir = base_dir / "uobjecthook";

    try {
        if (!std::filesystem::exists(uobjecthook_dir)) {
            std::filesystem::create_directories(uobjecthook_dir);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to create persistent directory: {}", e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to create persistent directory");
    }

    return uobjecthook_dir;
}

nlohmann::json UObjectHook::serialize_mc_state(const std::vector<std::string>& path, const std::shared_ptr<MotionControllerState>& state) {
    nlohmann::json result{};

    result["path"] = path;
    result["state"] = state->to_json();
    result["type"] = "motion_controller";

    return result;
}

nlohmann::json UObjectHook::serialize_camera(const std::vector<std::string>& path) {
    nlohmann::json result{};

    result["path"] = path;
    result["offset"] = utility::math::to_json(m_camera_attach.offset);
    result["type"] = "camera";

    // todo: adjustments/offsets, etc...? all it needs is the camera object which is fine

    return result;
}

void UObjectHook::save_camera_state(const std::vector<std::string>& path) {
    auto json = serialize_camera(path);

    const auto wanted_dir = UObjectHook::get_persistent_dir() / "camera_state.json";

    // Create dir if necessary
    try {
        std::filesystem::create_directories(wanted_dir.parent_path());

        if (std::filesystem::exists(wanted_dir.parent_path())) {
            std::ofstream file{wanted_dir};
            file << json.dump(4);
            file.close();

            m_persistent_camera_state = deserialize_camera_state();
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to save camera state: {}", e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to save camera state");
    }
}

// Layout-matched param block for APlayerController::SetViewTargetWithBlend(AActor* NewViewTarget,
// float BlendTime, EViewTargetBlendFunction BlendFunc, float BlendExp, bool bLockOutgoing). Called via
// reflection (no C++ binding). Zero-init then set NewViewTarget + BlendTime.
namespace {
struct SetViewTargetWithBlendParams {
    sdk::AActor* NewViewTarget{nullptr};
    float BlendTime{0.0f};
    uint8_t BlendFunc{0};        // VTBlend_Linear
    uint8_t _pad0[3]{};
    float BlendExp{0.0f};
    bool bLockOutgoing{false};
    uint8_t _pad1[3]{};
};
}

// Spawn a CameraActor parented to `target`, point it at the object, and make the local player view
// through it. All engine work is deferred to the game thread; everything is guarded so a missing class
// / world / PC just no-ops. Tears down any previous view camera first so this never leaks/stacks.
void UObjectHook::spawn_view_camera(sdk::USceneComponent* target) {
    const glm::vec3 cam_loc = m_last_camera_location;
    GameThreadWorker::get().enqueue([this, target, cam_loc]() {
        if (!this->exists(target)) return;
        try {
            auto engine = sdk::UGameEngine::get();
            auto world = engine != nullptr ? engine->get_world() : nullptr;
            if (world == nullptr) return;
            auto ugs = sdk::UGameplayStatics::get();
            if (ugs == nullptr) return;
            auto pc = ugs->get_player_controller(world, 0);
            if (pc == nullptr) return;

            // Remove any existing view camera before spawning a new one.
            if (m_view_camera_actor != nullptr && this->exists(m_view_camera_actor)) {
                try { m_view_camera_actor->destroy_actor(); } catch (...) {}
            }
            m_view_camera_actor = nullptr;

            auto cam_cls = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.CameraActor");
            if (cam_cls == nullptr) return;

            const glm::vec3 tgt = target->get_world_location();
            // Spawn at the player's current eye position (so the initial framing matches what they see),
            // or a sane behind/above offset if we don't have a camera location yet.
            glm::vec3 spawn_pos = cam_loc;
            if (glm::length(spawn_pos - tgt) < 1.0f) {
                spawn_pos = tgt;
            }
            auto cam = ugs->spawn_actor(world, cam_cls, spawn_pos);
            if (cam == nullptr) return;

            // Look from spawn_pos toward the target. UE FRotator is {Pitch (about Y), Yaw (about Z), Roll}.
            const glm::vec3 dir = tgt - spawn_pos;
            const float horiz = glm::length(glm::vec2{dir.x, dir.y});
            const float yaw   = glm::degrees(std::atan2(dir.y, dir.x));
            const float pitch = glm::degrees(std::atan2(dir.z, horiz));
            try { cam->set_actor_rotation(glm::vec3{pitch, yaw, 0.0f}, true); } catch (...) {}

            // Parent the camera to the target so it follows the object. AttachType MUST be
            // 1 = EAttachLocation::KeepWorldPosition — 0 (KeepRelativeOffset) reinterprets the world
            // pose we just set as parent-relative and teleports the camera to the wrong spot.
            if (auto root = cam->get_root_component(); root != nullptr) {
                try { root->attach_to(target, L"None", 1, false); } catch (...) {}
            }

            // Make the local player view through the spawned camera.
            if (auto fn = pc->get_class()->find_function(L"SetViewTargetWithBlend"); fn != nullptr) {
                SetViewTargetWithBlendParams params{};
                params.NewViewTarget = cam;
                params.BlendTime = 0.3f;
                pc->process_event(fn, &params);
            }

            m_view_camera_actor = cam;
            SPDLOG_INFO("[UObjectHook] Spawned view camera {:x} on target {:x}", (uintptr_t)cam, (uintptr_t)target);
        } catch (...) {
            SPDLOG_ERROR("[UObjectHook] spawn_view_camera failed");
        }
    });
}

// Return the player's view to its pawn and destroy the temporary view camera. Game thread; safe to call
// even if no camera is active (clears state + best-effort restores the pawn view).
void UObjectHook::restore_view_camera() {
    GameThreadWorker::get().enqueue([this]() {
        try {
            auto engine = sdk::UGameEngine::get();
            auto world = engine != nullptr ? engine->get_world() : nullptr;
            auto ugs = sdk::UGameplayStatics::get();
            auto pc = (world != nullptr && ugs != nullptr) ? ugs->get_player_controller(world, 0) : nullptr;
            if (pc != nullptr) {
                if (auto fn = pc->get_class()->find_function(L"SetViewTargetWithBlend"); fn != nullptr) {
                    SetViewTargetWithBlendParams params{};
                    params.NewViewTarget = (sdk::AActor*)pc->get_acknowledged_pawn();
                    params.BlendTime = 0.3f;
                    pc->process_event(fn, &params);
                }
            }
            if (m_view_camera_actor != nullptr && this->exists(m_view_camera_actor)) {
                try { m_view_camera_actor->destroy_actor(); } catch (...) {}
            }
            m_view_camera_actor = nullptr;
        } catch (...) {
            SPDLOG_ERROR("[UObjectHook] restore_view_camera failed");
        }
    });
}

float UObjectHook::get_nearest_gizmo_distance_meters() const {
    const float d = m_nearest_gizmo_dist_ue.load();
    if (d <= 0.0f) {
        return -1.0f;
    }

    auto& vr = VR::get();
    const float wtm = m_last_world_to_meters * (vr != nullptr ? vr->get_world_scale() : 1.0f);
    if (wtm <= 0.0001f) {
        return -1.0f;
    }

    return d / wtm;
}

// Overlay-material highlight: set `material` as the component's overlay material (UE5.1+
// UMeshComponent::SetOverlayMaterial), remembering the original so restore_overlay_highlight can put it
// back. Missing SetOverlayMaterial (UE4 / non-mesh comps) is a silent no-op. Duplicate-safe: the map
// check makes repeated enqueues for the same comp idempotent (jobs run sequentially on the game thread).
void UObjectHook::apply_overlay_highlight(sdk::USceneComponent* comp, sdk::UObject* material) {
    GameThreadWorker::get().enqueue([this, comp, material]() {
        if (!this->exists(comp) || material == nullptr || !this->exists(material)) {
            return;
        }
        {
            std::shared_lock _{m_mutex};
            if (m_overlay_mat_originals.contains(comp)) {
                return; // already applied
            }
        }
        try {
            auto klass = comp->get_class();
            if (klass == nullptr) return;
            auto set_fn = klass->find_function(L"SetOverlayMaterial");
            if (set_fn == nullptr) return; // engine/component doesn't support overlay materials
            sdk::UObject* orig = nullptr;
            if (auto get_fn = klass->find_function(L"GetOverlayMaterial"); get_fn != nullptr) {
                struct { sdk::UObject* ret{nullptr}; } gp{};
                comp->process_event(get_fn, &gp);
                orig = gp.ret;
            }
            struct { sdk::UObject* mat{nullptr}; } sp{};
            sp.mat = material;
            comp->process_event(set_fn, &sp);
            {
                std::unique_lock _{m_mutex};
                m_overlay_mat_originals[comp] = orig;
            }
        } catch (...) {}
    });
}

void UObjectHook::restore_overlay_highlight(sdk::USceneComponent* comp) {
    GameThreadWorker::get().enqueue([this, comp]() {
        sdk::UObject* orig = nullptr;
        {
            std::unique_lock _{m_mutex};
            auto it = m_overlay_mat_originals.find(comp);
            if (it == m_overlay_mat_originals.end()) {
                return; // nothing applied (or another queued restore already handled it)
            }
            orig = it->second;
            m_overlay_mat_originals.erase(it);
        }
        if (!this->exists(comp)) {
            return;
        }
        try {
            auto set_fn = comp->get_class()->find_function(L"SetOverlayMaterial");
            if (set_fn == nullptr) return;
            struct { sdk::UObject* mat{nullptr}; } sp{};
            sp.mat = (orig != nullptr && this->exists(orig)) ? orig : nullptr;
            comp->process_event(set_fn, &sp);
        } catch (...) {}
    });
}

std::optional<UObjectHook::StatePath> UObjectHook::deserialize_path(const nlohmann::json& data) {
    if (!data.contains("path")) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (missing path)");
        return std::nullopt;
    }

    // make sure path is an array
    if (!data["path"].is_array()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (path is not an array)");
        return std::nullopt;
    }

    const auto path = data["path"].get<std::vector<std::string>>();

    if (path.empty()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (path is empty)");
        return std::nullopt;
    }

    return StatePath{path};
}

std::shared_ptr<UObjectHook::PersistentState> UObjectHook::deserialize_mc_state(nlohmann::json& data) {
    SPDLOG_INFO("[UObjectHook] inside deserialize_mc_state");

    if (!data.contains("path") || !data.contains("state")) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (missing path or state)");
        return nullptr;
    }

    // make sure path is an array
    if (!data["path"].is_array()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (path is not an array)");
        return nullptr;
    }

    // make sure state is an object
    if (!data["state"].is_object()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (state is not an object)");
        return nullptr;
    }

    if (data.contains("type") && data["type"].is_string()) {
        const auto type = data["type"].get<std::string>();

        if (type != "motion_controller") {
            SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (type is not motion_controller)");
            return nullptr;
        }
    }

    SPDLOG_INFO("[UObjectHook] Deserializing state path...");
    auto path = data["path"].get<std::vector<std::string>>();

    auto persistent_state = std::make_shared<PersistentState>();
    persistent_state->path = path;

    SPDLOG_INFO("[UObjectHook] Deserializing state...");
    persistent_state->state.from_json(data["state"]);

    return persistent_state;
}

std::shared_ptr<UObjectHook::PersistentState> UObjectHook::deserialize_mc_state(std::filesystem::path json_path) {
    if (!std::filesystem::exists(json_path)) {
        return nullptr;
    }

    try {
        auto f = std::ifstream{json_path};

        if (f.is_open()) {
            // Log the file data to make sure we're getting it correctly...
            const auto file_contents = std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};

            SPDLOG_INFO("[UObjectHook] JSON file contents:");
            SPDLOG_INFO("{}", file_contents);

            nlohmann::json data = nlohmann::json::parse(file_contents);

            return deserialize_mc_state(data);
        }

        SPDLOG_ERROR("[UObjectHook] Failed to open JSON file {}", json_path.string());
        return nullptr;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}: {}", json_path.string(), e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}", json_path.string());
    }

    return nullptr;
}

std::vector<std::shared_ptr<UObjectHook::PersistentState>> UObjectHook::deserialize_all_mc_states() try {
    const auto uobjecthook_dir = get_persistent_dir();

    if (!std::filesystem::exists(uobjecthook_dir)) {
        return {};
    }

    // Gather all .json files in this directory
    std::vector<std::filesystem::path> json_files{};
    for (const auto& p : std::filesystem::directory_iterator(uobjecthook_dir)) {
        if (p.path().extension() == ".json") {
            json_files.push_back(p.path());
        }
    }

    std::vector<std::shared_ptr<PersistentState>> result{};
    for (const auto& json_file : json_files) {
        auto state = deserialize_mc_state(json_file);

        if (state != nullptr) {
            state->path_to_json = json_file;
            result.push_back(state);
        }
    }

    return result;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize all motion controller states: {}", e.what());
    return {};
} catch (...) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize all motion controller states");
    return {};
}

std::shared_ptr<UObjectHook::PersistentCameraState> UObjectHook::deserialize_camera(const nlohmann::json& data) {
    const auto path = deserialize_path(data);

    if (!path.has_value()) {
        SPDLOG_ERROR("[UObjectHook] Failed to deserialize camera path");
        return nullptr;
    }

    if (!data.contains("type") || !data["type"].is_string()) {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (missing type)");
        return nullptr;
    }

    if (data["type"].get<std::string>() != "camera") {
        SPDLOG_ERROR("[UObjectHook] Malfomed JSON file (type is not camera)");
        return nullptr;
    }

    auto persistent_state = std::make_shared<PersistentCameraState>();
    persistent_state->path = path.value();

    if (data.contains("offset") && data["offset"].is_object()) {
        persistent_state->offset = utility::math::from_json_vec3(data["offset"]);
    }

    return persistent_state;
}

std::shared_ptr<UObjectHook::PersistentCameraState> UObjectHook::deserialize_camera_state() {
    // Look for camera_state.json
    const auto uobjecthook_dir = get_persistent_dir();
    const auto camera_state_path = uobjecthook_dir / "camera_state.json";

    if (!std::filesystem::exists(camera_state_path)) {
        SPDLOG_ERROR("[UObjectHook] Failed to find camera_state.json");
        return nullptr;
    }

    try {
        auto f = std::ifstream{camera_state_path};

        if (f.is_open()) {
            // Log the file data to make sure we're getting it correctly...
            const auto file_contents = std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};

            SPDLOG_INFO("[UObjectHook] JSON file contents:");
            SPDLOG_INFO("{}", file_contents);

            nlohmann::json data = nlohmann::json::parse(file_contents);

            auto result = deserialize_camera(data);

            if (result != nullptr) {
                result->path_to_json = camera_state_path;
            }

            return result;
        }

        SPDLOG_ERROR("[UObjectHook] Failed to open JSON file {}", camera_state_path.string());
        return nullptr;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}: {}", camera_state_path.string(), e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}", camera_state_path.string());
    }

    return nullptr;
}

UObjectHook::ResolvedObject UObjectHook::resolve_persistent_target(const PersistentProperties& pp, bool use_cooldown) const {
    // Path-based resolution takes priority: it walks live roots each call, so it survives the object
    // being reallocated at a new address, and it's cheaper than a full-name scan.
    if (auto obj = pp.path.resolve(); obj != nullptr) {
        return obj;
    }

    // Stable-locator fallback for objects no allowed base can reach (e.g. click-selected world
    // actors). sdk::find_uobject matches object->get_full_name() == locator and caches HITS with
    // vtable+array-index validation, so a resolved locator is O(1) per tick and self-invalidates on
    // level load. A MISS, however, does a full O(N) get_full_name scan of the whole object array and
    // is NOT cached — so an absent target (destroyed / not-yet-spawned / wrong level) would re-scan
    // every frame. Guard that with a per-bucket cooldown: after a miss, serve null without scanning
    // for a while, so the worst case is one scan per ~cooldown ticks instead of one per frame.
    if (!pp.object_locator.empty()) {
        // Cooldown only THROTTLES the scan rate after a miss — it never hides a returned object for
        // long: we still probe every ~20 ticks, so a reappeared object is reapplied within ~0.3s
        // instead of being ignored for a fixed window. On-click dedup/save callers pass
        // use_cooldown=false to force a live lookup (so they never miss a present object and spawn a
        // duplicate bucket). A hit clears the cooldown.
        if (use_cooldown && pp.locator_miss_cooldown > 0) {
            --pp.locator_miss_cooldown;
            return nullptr;
        }
        if (auto* o = sdk::find_uobject<sdk::UObject>(pp.object_locator); o != nullptr) {
            pp.locator_miss_cooldown = 0;
            return ResolvedObject{o, o->get_class()};
        }
        if (use_cooldown) {
            pp.locator_miss_cooldown = 20; // re-probe in ~20 ticks (~0.3s) rather than every frame
        }
    }

    return nullptr;
}

void UObjectHook::update_persistent_states() {
    if (m_uobject_hook_disabled && m_fixed_visibilities) {
        return;
    }

    // For when we disable UObjectHook
    utility::ScopeGuard ___{[this]() {
        m_fixed_visibilities = true;
    }};

    // Camera state
    if (m_persistent_camera_state != nullptr) {
        auto obj = m_persistent_camera_state->path.resolve();

        if (obj != nullptr) {
            m_camera_attach.object = obj;
            m_camera_attach.offset = m_persistent_camera_state->offset;
        }
    }

    // Motion controller states
    if (!m_persistent_states.empty()) {
        for (const auto& state : m_persistent_states) {
            if (state == nullptr) {
                continue;
            }

            auto obj = state->path.resolve();

            if (obj == nullptr) {
                continue;
            }

            static const auto scene_component_t = sdk::USceneComponent::static_class();

            // TODO? will need some reworking to support properties from arbitrary objects
            if (!obj.definition->is_a(scene_component_t)) {
                continue;
            }

            // Destroy the existing mc state if it exists
            // This can cause issues if the previous object still exists
            // so we need to detach the old one
            if (state->last_object != nullptr && state->last_object != obj) {
                remove_motion_controller_state(state->last_object);
            }

            auto mc_state = get_or_add_motion_controller_state(obj.as<sdk::USceneComponent*>());

            if (mc_state == nullptr) {
                continue;
            }

            if (mc_state->adjusting) {
                state->state = *mc_state;
            } else {
                *mc_state = state->state;
            }

            state->last_object = obj.as<sdk::USceneComponent*>();
        }
    }

    // Persistent properties
    if (!m_persistent_properties.empty()) {
        const auto scene_comp_t = sdk::USceneComponent::static_class();
        const auto primitive_comp_t = sdk::UPrimitiveComponent::static_class();
        for (const auto& prop_base : m_persistent_properties) {
            if (prop_base == nullptr) {
                continue;
            }

            auto obj = resolve_persistent_target(*prop_base);

            if (obj == nullptr) {
                continue;
            }

            if (prop_base->hide) {
                if (obj.definition->is_a(primitive_comp_t)) {
                    auto obj_primcomp = obj.as<sdk::UPrimitiveComponent*>();

                    if (m_uobject_hook_disabled) {
                        obj_primcomp->set_overall_visibility(true, true);
                    } else {
                        obj_primcomp->set_overall_visibility(false, prop_base->hide_legacy);
                    }
                } else if (obj.definition->is_a(scene_comp_t)) {
                    auto obj_scenecomp = obj.as<sdk::USceneComponent*>();

                    if (m_uobject_hook_disabled) {
                        obj_scenecomp->set_visibility(true, false);
                    } else {
                        obj_scenecomp->set_visibility(false, false);
                    }
                }
            }

            for (const auto& prop_state : prop_base->properties) {
                const auto prop_desc = obj.definition->find_property(prop_state->name);

                if (prop_desc == nullptr) {
                    continue;
                }

                const auto prop_t = prop_desc->get_class();

                if (prop_t == nullptr) {
                    continue;
                }

                const auto prop_t_name = prop_t->get_name().to_string();

                switch (utility::hash(utility::narrow(prop_t_name))) {
                case "FloatProperty"_fnv:
                    {
                        auto& value = *(float*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.f;
                    }
                    break;
                case "DoubleProperty"_fnv:
                    {
                        auto& value = *(double*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.d;
                    }
                    break;
                case "UInt32Property"_fnv:
                case "IntProperty"_fnv:
                    {
                        auto& value = *(int32_t*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.i;
                    }
                    break;
                case "UInt64Property"_fnv:
                case "Int64Property"_fnv:
                    {
                        // The inspector lets these be saved (8-byte union) but the reapply switch
                        // previously had no case, so they wrote to disk and never reapplied.
                        auto& value = *(uint64_t*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.u64;
                    }
                    break;
                case "BoolProperty"_fnv:
                    {
                        auto boolprop = (sdk::FBoolProperty*)prop_desc;
                        boolprop->set_value_in_object(obj, prop_state->data.b);
                    }
                    break;
                case "ByteProperty"_fnv:
                    {
                        auto& value = *(uint8_t*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.u8;
                    }
                    break;
                case "UInt16Property"_fnv:
                    {
                        auto& value = *(uint16_t*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = prop_state->data.u16;
                    }
                    break;
                case "NameProperty"_fnv:
                    {
                        // sdk::FName is exactly two int32 pool indices (8 bytes) — the same raw bytes
                        // the generic property editor's display_context already memcpy'd into data.u64
                        // when "Save Property" was hit, so just reinterpret them back. Only valid within
                        // the SAME process run the save happened in (the pool indices are process-
                        // instance-relative), same caveat every other reapplied scalar here already has.
                        auto& value = *(sdk::FName*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                        value = *(sdk::FName*)&prop_state->data;
                    }
                    break;
                case "StructProperty"_fnv:
                    {
                        // Restore the raw struct bytes (Vector/Rotator/Transform/etc.). Clamp the
                        // copy to the actual struct size so a stale/oversized save can't overrun.
                        if (prop_state->struct_size > 0) {
                            uint32_t sz = prop_state->struct_size;
                            if (const auto sp = (sdk::FStructProperty*)prop_desc; sp != nullptr) {
                                if (const auto strukt = sp->get_struct(); strukt != nullptr) {
                                    sz = std::min<uint32_t>(sz, (uint32_t)strukt->get_properties_size());
                                }
                            }
                            sz = std::min<uint32_t>(sz, (uint32_t)sizeof(prop_state->struct_bytes));
                            auto* dst = (void*)(obj.as<uintptr_t>() + ((sdk::FProperty*)prop_desc)->get_offset());
                            memcpy(dst, prop_state->struct_bytes, sz);
                        }
                    }
                    break;
                default:
                    // Saved to disk but no reapply path for this property class — surface it once
                    // so a silently-ignored save is at least diagnosable in the log.
                    SPDLOG_WARN_ONCE("[UObjectHook] persistent property '{}' has unhandled type '{}' — value saved but not reapplied",
                                     utility::narrow(prop_state->name), utility::narrow(prop_t_name));
                    break;
                };
            }
        }
    }
}

void UObjectHook::update_motion_controller_components(
    const glm::vec3& hmd_location, const glm::vec3& hmd_euler,
    const glm::vec3& left_hand_location, const glm::vec3& left_hand_euler,
    const glm::vec3& right_hand_location, const glm::vec3& right_hand_euler)
{
    const auto mc_c = sdk::UMotionControllerComponent::static_class();

    if (mc_c == nullptr) {
        SPDLOG_ERROR_ONCE("[UObjectHook] Failed to find MotionControllerComponent class, cannot update motion controller components");
        return;
    }

    const auto motion_controllers = this->get_objects_by_class(mc_c);

    if (motion_controllers.empty()) {
        return;
    }

    for (auto obj : motion_controllers) {
        auto mc = (sdk::UMotionControllerComponent*)obj;
        if (!this->exists(mc)) {
            continue;
        }

        if (mc == mc_c->get_class_default_object()) {
            continue;
        }

        if (mc->get_outer() == nullptr || !this->exists(mc->get_outer())) {
            continue;
        }

        if (!sdk::UObjectReference{mc}.valid()) {
            continue;
        }

        if (mc->get_player_index() > 0) {
            continue;
        }

        if (mc->has_motion_source()) {
            const auto& motion_source = mc->get_motion_source();
            const auto motion_source_str = motion_source.to_string();

            if (motion_source_str == L"Left") {
                mc->set_world_location(left_hand_location, false, false);
                mc->set_world_rotation(left_hand_euler, false);
            } else if (motion_source_str == L"Right") {
                mc->set_world_location(right_hand_location, false, false);
                mc->set_world_rotation(right_hand_euler, false);
            } else if (motion_source_str == L"Head" || motion_source_str == L"HMD") {
                mc->set_world_location(hmd_location, false, false);
                mc->set_world_rotation(hmd_euler, false);
            }
        } else {
            if (mc->get_hand() == sdk::EControllerHand::Left) {
                mc->set_world_location(left_hand_location, false, false);
                mc->set_world_rotation(left_hand_euler, false);
            } else if (mc->get_hand() == sdk::EControllerHand::Right) {
                mc->set_world_location(right_hand_location, false, false);
                mc->set_world_rotation(right_hand_euler, false);
            }
        }
    }
}

sdk::UObject* UObjectHook::StatePath::resolve_base_object() const {
    if (!this->has_valid_base()) {
        return nullptr;
    }

    auto engine = sdk::UGameEngine::get();
    if (engine == nullptr) {
        return nullptr;
    }

    // TODO: Convert these into an enum or something when we initially parse the JSON file.
    switch (utility::hash(m_path.front())) {
    case "Acknowledged Pawn"_fnv:
    {
        auto world = engine->get_world();
        if (world == nullptr) {
            return nullptr;
        }

        auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

        if (player_controller == nullptr) {
            return nullptr;
        }

        return player_controller->get_acknowledged_pawn();
        break;
    }

    case "Player Controller"_fnv:
    {
        auto world = engine->get_world();
        if (world == nullptr) {
            return nullptr;
        }

        return sdk::UGameplayStatics::get()->get_player_controller(world, 0);
        break;
    }

    case "Camera Manager"_fnv:
    {
        auto world = engine->get_world();
        if (world == nullptr) {
            return nullptr;
        }

        auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

        if (player_controller == nullptr) {
            return nullptr;
        }

        return player_controller->get_player_camera_manager();
        break;
    }

    // s_allowed_bases stores this token WITH a space ("Persistent Level"); the original switch only
    // matched the no-space form, so this base never resolved (dead for try_get_path / level saves).
    // Accept both spellings.
    case "PersistentLevel"_fnv:
    case "Persistent Level"_fnv: {

        auto world = engine->get_world();
        if (world == nullptr) {
            return nullptr;
        }

        auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

        if (player_controller == nullptr) {
            return nullptr;
        }

        return player_controller->get_outer();
        break;
    }

    case "World"_fnv:
    {
        return engine->get_world();
        break;
    }

    default:
        break;
    };

    return nullptr;
}

UObjectHook::ResolvedObject UObjectHook::StatePath::resolve() const {
    const auto base = resolve_base_object();

    if (base == nullptr) {
        return nullptr;
    }

    void* previous_data = base;
    sdk::UStruct* previous_data_desc = base->get_class();

    if (previous_data_desc == nullptr) {
        return nullptr;
    }

    for (auto it = m_path.begin() + 1; it != m_path.end(); ++it) {
        if (previous_data == nullptr || previous_data_desc == nullptr) {
            return nullptr;
        }

        switch (utility::hash(*it)) {
        case "Outer"_fnv:
        {
            static const auto object_t = sdk::UObject::static_class();

            if (!previous_data_desc->is_a(object_t)) {
                return nullptr;
            }

            previous_data = ((sdk::UObject*)previous_data)->get_outer();
            if (previous_data != nullptr) {
                previous_data_desc = ((sdk::UObject*)previous_data)->get_class();
            } else {
                previous_data_desc = nullptr;
            }

            break;
        }
        case "Components"_fnv:
        {
            // Make sure the base is an AActor
            static const auto actor_t = sdk::AActor::static_class();

            if (!previous_data_desc->is_a(actor_t)) {
                return nullptr;
            }

            const auto components = ((sdk::AActor*)previous_data)->get_all_components();

            if (components.empty()) {
                return nullptr;
            }

            auto next_it = it + 1;

            if (next_it == m_path.end()) {
                return nullptr;
            }

            sdk::UActorComponent* matched = nullptr;

            // Pass 1: exact full (numbered) name match. The saved token is "<Class> <FullName>"
            // including the FName number, so this disambiguates among same-base-name siblings
            // (e.g. picks "StaticMeshComponent SM_Door_2" over "..._5"), which the de-numbered
            // prefix match below cannot. Prefer it when the exact instance still exists.
            for (auto comp : components) {
                const auto full_name = utility::narrow(comp->get_class()->get_fname().to_string() + L" " + comp->get_fname().to_string());
                if (*next_it == full_name) {
                    matched = comp;
                    break;
                }
            }

            // Pass 2 (fallback): de-numbered match — original behavior, covers cross-session number
            // drift where the sibling exists but got a different FName number this run.
            if (matched == nullptr) {
                for (auto comp : components) {
                    const auto& comp_fname = comp->get_fname();
                    const auto comp_name = comp_fname.to_string_remove_numbers();
                    const auto comp_ends_with_number = comp_fname.get_number() != 0;

                    const auto comp_expanded_name = utility::narrow(comp->get_class()->get_fname().to_string() + L" " + comp_name);
                    const auto is_match = comp_ends_with_number ? next_it->starts_with(comp_expanded_name)
                                                                : *next_it == comp_expanded_name;

                    if (is_match) {
                        matched = comp;
                        break;
                    }
                }
            }

            if (matched != nullptr) {
                previous_data = matched;
                previous_data_desc = matched->get_class();
                ++it;
            }

            break;
        }
        case "Properties"_fnv:
        {
            auto next_it = it + 1;

            if (next_it == m_path.end()) {
                return nullptr;
            }

            const auto prop_name = *next_it;
            const auto prop_desc = previous_data_desc->find_property(utility::widen(prop_name));

            if (prop_desc == nullptr) {
                return nullptr;
            }

            const auto prop_t = prop_desc->get_class();

            if  (prop_t == nullptr) {
                return nullptr;
            }

            const auto prop_t_name = prop_t->get_name().to_string();
            switch (utility::hash(utility::narrow(prop_t_name))) {
            case "InterfaceProperty"_fnv:
            case "ObjectProperty"_fnv:
            {
                const auto obj_ptr = prop_desc->get_data<sdk::UObject*>(previous_data);
                const auto obj = obj_ptr != nullptr ? *obj_ptr : nullptr;

                if (obj == nullptr) {
                    return nullptr;
                }

                previous_data = obj;
                previous_data_desc = obj->get_class();
                ++it;
                break;
            }
            case "StructProperty"_fnv:
            {
                const auto struct_data = prop_desc->get_data<void*>(previous_data);

                previous_data = struct_data;
                previous_data_desc = ((sdk::FStructProperty*)prop_desc)->get_struct();

                if (previous_data_desc == nullptr || previous_data == nullptr) {
                    return nullptr;
                }

                // ++it because we are examining the properties
                ++it;
                break;
            }
            case "ArrayProperty"_fnv:
            {
                const auto inner = ((sdk::FArrayProperty*)prop_desc)->get_inner();

                if (inner == nullptr) {
                    return nullptr;
                }

                const auto inner_c = inner->get_class();

                if (inner_c == nullptr) {
                    return nullptr;
                }

                const auto inner_c_type = utility::narrow(inner_c->get_name().to_string());

                // only support ObjectProperty for now
                if (inner_c_type != "ObjectProperty" && inner_c_type != "InterfaceProperty") {
                    return nullptr;
                }

                const auto array_ptr = prop_desc->get_data<sdk::TArray<sdk::UObject*>>(previous_data);

                if (array_ptr == nullptr) {
                    return nullptr;
                }

                const auto& arr = *array_ptr;

                if (arr.empty() || arr.data == nullptr) {
                    return nullptr;
                }

                auto prop_it = next_it + 1;

                if (prop_it == m_path.end()) {
                    return nullptr;
                }

                sdk::UObject* matched = nullptr;

                // Pass 1: exact full (numbered) name match — disambiguates numbered array siblings,
                // which the de-numbered prefix match below cannot (same fix as the Components case:
                // the saved token is "<Class> <FullName>" incl. the FName number).
                for (auto obj : arr) {
                    if (obj == nullptr) {
                        continue;
                    }
                    const auto full_name = utility::narrow(obj->get_class()->get_fname().to_string() + L" " + obj->get_fname().to_string());
                    if (*prop_it == full_name) {
                        matched = obj;
                        break;
                    }
                }

                // Pass 2 (fallback): de-numbered match — original behavior, covers cross-session
                // number drift where the element exists but got a different FName number this run.
                if (matched == nullptr) {
                    for (auto obj : arr) {
                        if (obj == nullptr) {
                            continue;
                        }

                        const auto& obj_fname = obj->get_fname();
                        const auto obj_name = obj_fname.to_string_remove_numbers();
                        const auto obj_ends_with_number = obj_fname.get_number() != 0;

                        const auto obj_expanded_name = utility::narrow(obj->get_class()->get_fname().to_string() + L" " + obj_name);
                        const auto is_match = obj_ends_with_number ? prop_it->starts_with(obj_expanded_name)
                                                                   : *prop_it == obj_expanded_name;

                        if (is_match) {
                            matched = obj;
                            break;
                        }
                    }
                }

                if (matched == nullptr) {
                    return nullptr;
                }

                previous_data = matched;
                previous_data_desc = matched->get_class();
                ++it;
                ++it;
                break;
            }

            default:
                SPDLOG_ERROR("[UObjectHook] Unsupported persistent property type {}", utility::narrow(prop_t_name));
                break;
            };

            break;
        }

        default:
            return nullptr;
            break;
        };
    }

    if (previous_data == nullptr || previous_data_desc == nullptr) {
        return nullptr;
    }

    return ResolvedObject{(sdk::UObject*)previous_data, previous_data_desc};
}

void UObjectHook::on_frame() {
    if (m_keybind_toggle_uobject_hook->is_key_down_once()) {
        set_disabled(!is_disabled());
    }

    try { sync_hooked_functions_to_lua(); }
    catch (const std::exception& e) { spdlog::error("[UObjectHook] sync_hooked_functions_to_lua threw: {}", e.what()); }
    catch (...)                     { spdlog::error("[UObjectHook] sync_hooked_functions_to_lua threw (unknown)"); }

    // Rebindable gizmo transform-mode keys. is_key_down_once() returns false while unbound, so these
    // are no-ops until the user assigns them in the gizmo options — no default key collisions.
    if (m_keybind_gizmo_move->is_key_down_once())     { m_gizmo_mode = 0; }
    if (m_keybind_gizmo_rotate->is_key_down_once())   { m_gizmo_mode = 1; }
    if (m_keybind_gizmo_scale->is_key_down_once())    { m_gizmo_mode = 2; }
    if (m_keybind_gizmo_combined->is_key_down_once()) { m_gizmo_mode = 3; }
    if (m_keybind_pick->is_key_down_once())           { m_click_select_mode = !m_click_select_mode; }

    // Push "does UObjectHook itself need exclusive input right now" into Framework every frame —
    // this is what lets mouse/keyboard-to-game blocking and the always-visible cursor follow the
    // picker/gizmo/standalone-window state independent of whether the main "UEVR [...]" panel happens
    // to be open (see Framework::set_force_input_capture). Recomputed and re-pushed every frame, so it
    // naturally clears itself once nothing here is active anymore.
    // wants_active_ui() alone still gates gizmo dragging/click-select below (those must keep working
    // whether or not this toggle is on); force_input_capture additionally lets "block passthrough
    // when gizmos visible" be turned off so an active-but-hidden gizmo (m_hide_gizmos_when_ui_closed)
    // doesn't keep game input blocked purely because targets are still selected.
    const bool ui_panel_reasons = g_framework->is_drawing_ui() || m_show_main_window || m_click_select_mode ||
        m_show_class_browser || m_show_function_caller || m_show_options_window;
    g_framework->set_force_input_capture(
        ui_panel_reasons || (m_block_passthrough_when_gizmos_visible && m_has_gizmos));

    // Quick-access keybind: F2 toggles the Class Browser window whenever UObjectHook is already
    // active (and not typing into a field) — reachable without navigating to the UObjectHook sidebar
    // page.
    if (wants_active_ui() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        m_show_class_browser = !m_show_class_browser;
    }
    // F3 toggles the standalone Main window and is intentionally NOT gated on wants_active_ui() —
    // unlike F2, it needs to work as the BOOTSTRAP that summons UObjectHook with the main "UEVR [...]"
    // panel fully closed and nothing else active yet (the whole point of task #7). Fixed-key
    // collision risk with a game's own F3 binding is an accepted, deliberate tradeoff here (same
    // category of tradeoff F2 already makes, just without the is_drawing_ui() precondition).
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
        m_show_main_window = !m_show_main_window;
    }

    // Dockable pop-out windows live OUTSIDE the sidebar tree (which only
    // renders when the user has UObjectHook focused as a sidebar entry).
    // We draw them every imgui frame instead, gated on their toggle bools.
    // Both windows auto-attach to Framework's main dockspace via
    // SetNextWindowDockID(FirstUseEver) just like Lua imgui.begin_window.
    //
    // Gate on wants_active_ui() (main overlay OR any of UObjectHook's own windows/picker) rather than
    // is_drawing_ui() directly, so these pop-outs keep working with the main "UEVR [...]" panel closed
    // — e.g. the standalone Main window (m_show_main_window) needs to render independent of it.
    if (wants_active_ui()) {
    if (m_show_class_browser) {
        try { draw_class_browser_window(); }
        catch (const std::exception& e) { spdlog::error("[UObjectHook] class browser threw: {}", e.what()); }
        catch (...)                     { spdlog::error("[UObjectHook] class browser threw (unknown)"); }
    }
    if (m_show_function_caller) {
        try { draw_function_caller_window(); }
        catch (const std::exception& e) { spdlog::error("[UObjectHook] function caller window threw: {}", e.what()); }
        catch (...)                     { spdlog::error("[UObjectHook] function caller window threw (unknown)"); }
    }
    if (m_show_options_window) {
        try { draw_options_window(); }
        catch (const std::exception& e) { spdlog::error("[UObjectHook] options window threw: {}", e.what()); }
        catch (...)                     { spdlog::error("[UObjectHook] options window threw (unknown)"); }
    }
    if (m_show_main_window) {
        try { draw_main_window(); }
        catch (const std::exception& e) { spdlog::error("[UObjectHook] main window threw: {}", e.what()); }
        catch (...)                     { spdlog::error("[UObjectHook] main window threw (unknown)"); }
    }
    // Class Inspector windows: one per entry in m_open_class_inspectors.
    // Iterate by index because the inspector's X-close path removes from the
    // list, and we can't mutate while iterating. Copy the snapshot up front so
    // any inspector that itself opens another inspector (e.g. clicking the
    // parent class link) doesn't reenter the loop mid-iteration.
    {
        auto snapshot = m_open_class_inspectors;
        for (auto* cls : snapshot) {
            if (cls == nullptr) continue;
            try { draw_class_inspector_window(cls); }
            catch (const std::exception& e) { spdlog::error("[UObjectHook] class inspector threw: {}", e.what()); }
            catch (...)                     { spdlog::error("[UObjectHook] class inspector threw (unknown)"); }
        }
    }
    } // end is_drawing_ui() gate for pop-out imgui windows

    try { draw_component_gizmos(); }
    catch (const std::exception& e) { spdlog::error("[UObjectHook] gizmo draw threw: {}", e.what()); }
    catch (...)                     { spdlog::error("[UObjectHook] gizmo draw threw (unknown)"); }

    try { draw_light_icons(); }
    catch (const std::exception& e) { spdlog::error("[UObjectHook] light icon draw threw: {}", e.what()); }
    catch (...)                     { spdlog::error("[UObjectHook] light icon draw threw (unknown)"); }
}

void UObjectHook::handle_click_select() {
    // Modal picker: only while UObjectHook itself is active (main overlay open, standalone window
    // shown, or the picker is armed) and the cursor isn't over an imgui widget, one selection per
    // click. wants_active_ui() (not g_framework->is_drawing_ui() directly) lets the armed picker keep
    // working even with the main "UEVR [...]" panel closed.
    if (!wants_active_ui()) {
        return;
    }
    // Esc cancels the armed pick (the natural "never mind" out).
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_click_select_mode = false;
        return;
    }
    if (ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive()) {
        return;
    }
    // NOTE: we no longer early-out on "no click" — the picker runs every frame while armed so it can
    // gather the overlapping candidates under the cursor, let the scroll wheel cycle through them, and
    // draw a w2s list. The actual selection happens only on the left click (gated below).
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const auto& io = ImGui::GetIO();

    // Remap a screen point through the VR overlay transform (identity flat/UI-closed), so the marker
    // and candidate list stay visually aligned with the gizmo/reticle drawing convention used
    // elsewhere in this file.
    auto remap = [](ImVec2 p) -> ImVec2 {
        if (auto vr = VR::get(); vr != nullptr && vr->is_hmd_active()) {
            return vr->get_overlay_component().transform_world_aligned_to_overlay(p);
        }
        return p;
    };

    // Persistent crosshair at the CURRENT mouse/controller-ray screen position — distinct from the
    // green reticle on the focused candidate (which can sit off-axis once you scroll to an overlapping
    // object). This one marks where the pick ray itself is aimed, so cycling through candidates never
    // loses track of the original screen-space click point. Cyan, small, always drawn while armed.
    auto draw_click_marker = [&](const ImVec2& mouse) {
        auto* dl = ImGui::GetForegroundDrawList();
        const ImVec2 c = remap(mouse);
        const ImU32 col = IM_COL32(80, 220, 255, 235);
        const float r = 6.0f;
        dl->AddLine(ImVec2{c.x - r, c.y}, ImVec2{c.x + r, c.y}, col, 1.5f);
        dl->AddLine(ImVec2{c.x, c.y - r}, ImVec2{c.x, c.y + r}, col, 1.5f);
        dl->AddCircle(c, r * 0.55f, col, 12, 1.5f);
    };

    // Candidate overlay (header + capped scrollable list, active row highlighted). Shows the SHORT
    // label for every row; once the active row hasn't changed for kIdleSec, its full path replaces
    // the short label as a looping horizontal marquee (there's no per-row imgui hover here — this is
    // raw foreground-draw-list text following the cursor — so "idled on" means "still the active
    // scroll-cycle target after a beat", not a traditional mouse-hover).
    static sdk::USceneComponent* s_idle_target = nullptr;
    static double s_idle_since = 0.0;
    auto draw_pick_overlay = [](const ImVec2& mouse, const std::vector<PickCandidate>& list, int active) {
        if (list.empty()) return;
        auto* dl = ImGui::GetForegroundDrawList();
        const ImVec2 b{mouse.x + 16.0f, mouse.y - 6.0f};
        const int n = (int)list.size();
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "pick %d/%d  (scroll to cycle, click to select)", active + 1, n);
        dl->AddText(ImVec2{b.x, b.y - 16.0f}, IM_COL32(255, 220, 90, 235), hdr);

        constexpr double kIdleSec = 0.6;   // how long the active row must be unchanged before it marquees
        constexpr float kMarqueePxPerSec = 45.0f;
        constexpr float kMarqueeGapPx = 40.0f;
        constexpr float kMarqueeWidth = 340.0f;

        const int kShow = 8;
        int start = active - kShow / 2;
        if (start < 0) start = 0;
        if (n > kShow && start > n - kShow) start = n - kShow;
        for (int k = start; k < n && k < start + kShow; ++k) {
            const bool act = (k == active);
            const float y = b.y + (k - start) * 16.0f;
            const ImU32 col = act ? IM_COL32(120, 255, 120, 255) : IM_COL32(210, 210, 210, 220);

            const bool marquee = act && list[k].comp == s_idle_target &&
                (ImGui::GetTime() - s_idle_since) > kIdleSec && !list[k].full_path.empty();
            if (marquee) {
                // Clip to a fixed-width box and slide the full path leftward, wrapping with a gap so it
                // loops seamlessly. Two copies drawn side by side; only the visible slice matters.
                const std::string text = "> " + list[k].full_path + std::string((size_t)(kMarqueeGapPx / 6.0f), ' ');
                const float text_w = ImGui::CalcTextSize(text.c_str()).x;
                const float period = text_w > 0.0f ? text_w : 1.0f;
                const float elapsed = (float)(ImGui::GetTime() - s_idle_since - kIdleSec);
                const float scroll = std::fmod(elapsed * kMarqueePxPerSec, period);
                dl->PushClipRect(ImVec2{b.x, y}, ImVec2{b.x + kMarqueeWidth, y + 14.0f}, true);
                for (int rep = 0; rep < 2; ++rep) {
                    const float x = b.x - scroll + rep * period;
                    dl->AddText(ImVec2{x + 1.0f, y + 1.0f}, IM_COL32(0, 0, 0, 200), text.c_str());
                    dl->AddText(ImVec2{x, y}, col, text.c_str());
                }
                dl->PopClipRect();
                continue;
            }

            const std::string row = (act ? "> " : "  ") + list[k].short_label;
            dl->AddText(ImVec2{b.x + 1.0f, y + 1.0f}, IM_COL32(0, 0, 0, 200), row.c_str());
            dl->AddText(ImVec2{b.x, y}, col, row.c_str());
        }
    };

    // Dispatch the Lua selection-state event whenever the active candidate or the candidate SET
    // changes (not on every pixel of mouse movement that leaves both unchanged). Declared up here
    // (rather than after the fast path below) so wheel-only cycling can fire it too.
    static sdk::USceneComponent* s_last_dispatched_active = nullptr;
    static size_t s_last_dispatched_count = SIZE_MAX;

    // Advances m_pick_cycle by the current wheel delta against the EXISTING m_pick_cache, redraws the
    // overlay + focused-candidate reticle, and fires the Lua event on change. No m_objects rescan —
    // shared by the fast "cursor didn't move" path (wheel-only cycling) and the full-rescan path
    // further down, so scroll-cycling never pays for a fresh scan (it used to force one every tick,
    // which is what made it laggy).
    auto cycle_and_draw = [&]() {
        const int n = (int)m_pick_cache.size();
        if (io.MouseWheel != 0.0f) m_pick_cycle -= (io.MouseWheel > 0 ? 1 : -1); // wheel up = nearer/previous
        m_pick_cycle = ((m_pick_cycle % n) + n) % n;
        if (s_idle_target != m_pick_cache[m_pick_cycle].comp) {
            s_idle_target = m_pick_cache[m_pick_cycle].comp;
            s_idle_since = ImGui::GetTime();
        }
        draw_pick_overlay(io.MousePos, m_pick_cache, m_pick_cycle);

        if (s_last_dispatched_active != m_pick_cache[m_pick_cycle].comp || s_last_dispatched_count != m_pick_cache.size()) {
            s_last_dispatched_active = m_pick_cache[m_pick_cycle].comp;
            s_last_dispatched_count = m_pick_cache.size();
            dispatch_picker_selection_event();
        }

        // World-space reticle on the focused cycle candidate (green, distinct from the amber selection
        // box) so it's obvious WHICH overlapping object you're about to pick as you scroll through them.
        if (m_highlight_selection) {
            auto* engine0 = sdk::UGameEngine::get();
            auto* world0 = engine0 != nullptr ? engine0->get_world() : nullptr;
            auto* ugs0 = sdk::UGameplayStatics::get();
            auto* pc0 = (ugs0 != nullptr && world0 != nullptr) ? ugs0->get_player_controller(world0, 0) : nullptr;
            glm::vec2 sp{0.0f, 0.0f};
            if (ugs0 != nullptr && pc0 != nullptr && ugs0->world_to_screen(pc0, m_pick_cache[m_pick_cycle].world, &sp)) {
                ImVec2 c{sp.x, sp.y};
                if (auto vr = VR::get(); vr != nullptr && vr->is_hmd_active()) {
                    c = vr->get_overlay_component().transform_world_aligned_to_overlay(c);
                }
                const auto* vp0 = ImGui::GetMainViewport();
                if (c.x >= vp0->Pos.x && c.x <= vp0->Pos.x + vp0->Size.x &&
                    c.y >= vp0->Pos.y && c.y <= vp0->Pos.y + vp0->Size.y) {
                    auto* dl = ImGui::GetForegroundDrawList();
                    const ImU32 hl = IM_COL32(120, 255, 120, 245); // candidate green (matches the active list row)
                    const float r = 16.0f, b = 7.0f;
                    dl->AddLine(ImVec2{c.x - r, c.y - r}, ImVec2{c.x - r + b, c.y - r}, hl, 2.0f);
                    dl->AddLine(ImVec2{c.x - r, c.y - r}, ImVec2{c.x - r, c.y - r + b}, hl, 2.0f);
                    dl->AddLine(ImVec2{c.x + r, c.y - r}, ImVec2{c.x + r - b, c.y - r}, hl, 2.0f);
                    dl->AddLine(ImVec2{c.x + r, c.y - r}, ImVec2{c.x + r, c.y - r + b}, hl, 2.0f);
                    dl->AddLine(ImVec2{c.x - r, c.y + r}, ImVec2{c.x - r + b, c.y + r}, hl, 2.0f);
                    dl->AddLine(ImVec2{c.x - r, c.y + r}, ImVec2{c.x - r, c.y - r + b}, hl, 2.0f);
                    dl->AddLine(ImVec2{c.x + r, c.y + r}, ImVec2{c.x + r - b, c.y + r}, hl, 2.0f);
                    dl->AddLine(ImVec2{c.x + r, c.y + r}, ImVec2{c.x + r, c.y + r - b}, hl, 2.0f);
                    dl->AddCircleFilled(c, 2.5f, hl);
                }
            }
        }
    };

    // Re-scan m_objects (+ the screen_to_world ProcessEvent) only when the cursor moved / a click
    // happened — otherwise redraw the cached overlay (wheel-only ticks cycle the existing cache via
    // cycle_and_draw above). Keeps an armed-but-idle-or-scrolling picker off the per-frame hot path
    // (the scan holds a shared_lock the game-thread add/destroy hooks want).
    static ImVec2 s_last_pick_mouse{-1e9f, -1e9f};
    const bool pick_moved = std::fabs(io.MousePos.x - s_last_pick_mouse.x) > 0.5f ||
                            std::fabs(io.MousePos.y - s_last_pick_mouse.y) > 0.5f;
    if (!(pick_moved || clicked || m_pick_cache.empty())) {
        cycle_and_draw();
        draw_click_marker(io.MousePos);
        return;
    }
    s_last_pick_mouse = io.MousePos;

    auto engine = sdk::UGameEngine::get();
    auto world = engine != nullptr ? engine->get_world() : nullptr;
    if (world == nullptr) {
        return;
    }
    auto ugs = sdk::UGameplayStatics::get();
    if (ugs == nullptr) {
        return;
    }
    auto pc = ugs->get_player_controller(world, 0);
    if (pc == nullptr) {
        return;
    }

    // Reflected transform properties (cached). World position is composed by walking the
    // AttachParent chain below — plain memory reads, no ProcessEvent — so attached components
    // with big relative offsets (e.g. mesh comps parented deep in an actor) rank correctly too.
    static const sdk::FProperty* s_rel_loc_prop = []() -> sdk::FProperty* {
        auto* cls = sdk::USceneComponent::static_class();
        return cls != nullptr ? cls->find_property(L"RelativeLocation") : nullptr;
    }();
    static const sdk::FProperty* s_rel_rot_prop = []() -> sdk::FProperty* {
        auto* cls = sdk::USceneComponent::static_class();
        return cls != nullptr ? cls->find_property(L"RelativeRotation") : nullptr;
    }();
    static const sdk::FProperty* s_rel_scale_prop = []() -> sdk::FProperty* {
        auto* cls = sdk::USceneComponent::static_class();
        return cls != nullptr ? cls->find_property(L"RelativeScale3D") : nullptr;
    }();
    static const sdk::FProperty* s_attach_parent_prop = []() -> sdk::FProperty* {
        auto* cls = sdk::USceneComponent::static_class();
        return cls != nullptr ? cls->find_property(L"AttachParent") : nullptr;
    }();
    if (s_rel_loc_prop == nullptr) {
        static bool s_warned = false;
        if (!s_warned) { spdlog::warn("[UObjectHook] click-select: RelativeLocation property not found"); s_warned = true; }
        return;
    }
    const bool is_ue5_vec = sdk::ScriptVector::static_struct() != nullptr &&
        sdk::ScriptVector::static_struct()->get_struct_size() == sizeof(glm::vec<3, double>);

    const glm::vec2 screen_pos{io.MousePos.x, io.MousePos.y};

    glm::vec3 ray_origin{0.0f, 0.0f, 0.0f};
    glm::vec3 ray_dir{0.0f, 0.0f, 0.0f};
    if (!ugs->screen_to_world(pc, screen_pos, &ray_origin, &ray_dir)) {
        return;
    }
    const float dir_len = glm::length(ray_dir);
    if (dir_len < 1e-6f) {
        return;
    }
    ray_dir /= dir_len;

    // Single pass under the shared lock: filter to scene components (via the cached super-class
    // list — no virtual calls, no ProcessEvent), compose each candidate's WORLD location from its
    // reflected RelativeLocation/Rotation/Scale3D by walking the AttachParent chain (still plain
    // memory reads), and keep the front-most one inside an angular cone of the click ray. Doing the
    // reads under the lock blocks the add/destructor hooks for the (brief) scan so we never read a
    // component that is being freed on the game thread. Approximations: socket offsets and the
    // bAbsolute* overrides are ignored (rare; the cone radius usually still catches those).
    constexpr float kConeTan = 0.06f;  // ~3.4 deg half-angle of the pick cone
    constexpr float kMinPerp = 40.0f;  // world units: always allow at least this radius up close

    // Read a reflected FVector/FRotator (double on UE5, float on UE4) as glm::vec3.
    auto read_vec3_prop = [is_ue5_vec](const sdk::FProperty* prop, sdk::USceneComponent* c) -> glm::vec3 {
        if (is_ue5_vec) {
            const auto* d = prop->get_data<glm::vec<3, double>>(c);
            return glm::vec3{(float)d->x, (float)d->y, (float)d->z};
        }
        return *prop->get_data<glm::vec3>(c);
    };

    // UE FRotationMatrix rows for an FRotator {x=Pitch, y=Yaw, z=Roll} (degrees). Row-vector
    // convention: world_v = v.x*M[0] + v.y*M[1] + v.z*M[2] — matches UE's TransformVector exactly,
    // so the chain composition reproduces the engine's own parent-relative placement.
    auto ue_rot_rows = [](const glm::vec3& rot) -> std::array<glm::vec3, 3> {
        const float P = glm::radians(rot.x), Y = glm::radians(rot.y), R = glm::radians(rot.z);
        const float SP = std::sin(P), CP = std::cos(P);
        const float SY = std::sin(Y), CY = std::cos(Y);
        const float SR = std::sin(R), CR = std::cos(R);
        return {
            glm::vec3{CP * CY, CP * SY, SP},
            glm::vec3{SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP},
            glm::vec3{-(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP},
        };
    };

    // World location of a component: its RelativeLocation pushed up through each ancestor's
    // relative transform (scale -> rotate -> translate per level). Caller holds the shared lock, so
    // exists_unsafe() vets every AttachParent pointer before it's dereferenced. Depth-capped
    // against pathological/cyclic chains.
    auto compose_world_location = [&](sdk::USceneComponent* comp) -> glm::vec3 {
        glm::vec3 p = read_vec3_prop(s_rel_loc_prop, comp);
        if (s_attach_parent_prop == nullptr) {
            return p;
        }
        auto* parent = *s_attach_parent_prop->get_data<sdk::USceneComponent*>(comp);
        for (int depth = 0; parent != nullptr && depth < 12; ++depth) {
            if (!exists_unsafe((sdk::UObjectBase*)parent)) {
                break; // freed / untracked pointer — stop with the partial composition
            }
            const glm::vec3 pl = read_vec3_prop(s_rel_loc_prop, parent);
            if (s_rel_scale_prop != nullptr) {
                p *= read_vec3_prop(s_rel_scale_prop, parent);
            }
            if (s_rel_rot_prop != nullptr) {
                const auto M = ue_rot_rows(read_vec3_prop(s_rel_rot_prop, parent));
                p = p.x * M[0] + p.y * M[1] + p.z * M[2];
            }
            p += pl;
            parent = *s_attach_parent_prop->get_data<sdk::USceneComponent*>(parent);
        }
        return p;
    };

    std::string class_filter = m_pick_class_filter;
    for (auto& ch : class_filter) ch = (char)std::tolower((unsigned char)ch);

    // Gather ALL candidates inside the pick cone (front-most first), optionally class-filtered, so the
    // user can scroll/swipe to cycle through overlapping objects instead of always getting the nearest.
    // Root components only (see the AttachParent check below for why/how).
    struct Cand { float t; sdk::USceneComponent* comp; glm::vec3 world; std::string name; };
    std::vector<Cand> cands;
    {
        std::shared_lock _{m_mutex};
        auto* scene_cls = sdk::USceneComponent::static_class();
        for (auto* obj : m_objects) {
            auto it = m_meta_objects.find(obj);
            if (it == m_meta_objects.end() || it->second == nullptr) continue;
            const auto& meta = it->second;
            bool is_scene = meta->uclass == scene_cls;
            if (!is_scene) for (auto* sc : meta->super_classes) { if (sc == scene_cls) { is_scene = true; break; } }
            if (!is_scene) continue;
            auto* comp = reinterpret_cast<sdk::USceneComponent*>(obj);
            // Root components only: an actor's RootComponent always sits at the top of ITS OWN
            // attach chain (AttachParent == nullptr), so this is a cheap proxy for "is root" without
            // calling AActor::get_root_component() (a ProcessEvent call — too costly/unsafe to run
            // per-candidate in this per-frame scan). Approximation: an actor attached to ANOTHER actor
            // has a non-null AttachParent on its own root too, so that case is missed (false negative,
            // never a false positive — a true non-root child is never mistaken for a root).
            if (s_attach_parent_prop != nullptr) {
                auto* parent0 = *s_attach_parent_prop->get_data<sdk::USceneComponent*>(comp);
                if (parent0 != nullptr) continue;
            }
            const glm::vec3 p = compose_world_location(comp);
            const glm::vec3 v = p - ray_origin;
            const float t = glm::dot(v, ray_dir);
            if (t <= 1.0f) continue; // behind / on top of the camera
            const float perp = glm::length(v - t * ray_dir);
            if (perp > kMinPerp + kConeTan * t) continue;
            std::string fn = utility::narrow(meta->full_name);
            if (!class_filter.empty()) { // case-insensitive substring of the cached full name
                std::string fl = fn;
                for (auto& ch : fl) ch = (char)std::tolower((unsigned char)ch);
                if (fl.find(class_filter) == std::string::npos) continue;
            }
            cands.push_back({t, comp, p, std::move(fn)});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.t < b.t; });

    // Commit to the frame-persistent cache so an armed-but-idle picker can redraw without re-scanning.
    // short_label is name-first (via the shared shorten_object_path helper); full_path backs the
    // hover/idle marquee, the Lua selection event, and Copy actions. Screen position is projected once
    // here and reused by both the marquee width math and the Lua event payload.
    m_pick_cache.clear();
    m_pick_cache.reserve(cands.size());
    for (auto& c : cands) {
        PickCandidate entry{};
        entry.comp = c.comp;
        entry.full_path = c.name;
        entry.short_label = shorten_object_path(c.name);
        entry.world = c.world;
        glm::vec2 sp{0.0f, 0.0f};
        if (ugs->world_to_screen(pc, c.world, &sp)) {
            entry.screen = sp;
        }
        m_pick_cache.push_back(std::move(entry));
    }

    draw_click_marker(io.MousePos);

    if (m_pick_cache.empty()) {
        m_pick_cycle = 0;
        auto* dl = ImGui::GetForegroundDrawList();
        dl->AddText(ImVec2{io.MousePos.x + 16.0f, io.MousePos.y + 2.0f}, IM_COL32(255, 180, 60, 230),
                    class_filter.empty() ? "(no object under cursor)" : "(no match for filter)");
        if (s_last_dispatched_active != nullptr || s_last_dispatched_count != 0) {
            s_last_dispatched_active = nullptr;
            s_last_dispatched_count = 0;
            dispatch_picker_selection_event();
        }
        return;
    }

    cycle_and_draw();

    if (!clicked) {
        return; // overlay + scroll only; the click below commits the selection
    }
    sdk::USceneComponent* best = m_pick_cache[m_pick_cycle].comp;
    const float best_t = cands[m_pick_cycle].t;

    if (best != nullptr) {
        std::string name{};
        bool inserted = false;
        {
            std::unique_lock _{m_mutex};
            // Re-validate under the write lock: best was chosen under a shared lock that has since
            // been released, so confirm it still lives in the tracked set before inserting (and pull
            // its name from the cached meta rather than calling get_full_name on possibly-freed memory).
            if (m_objects.contains(reinterpret_cast<sdk::UObjectBase*>(best))) {
                if (m_click_select_single) {
                    m_gizmo_components.clear(); // single-select mode: replace rather than accumulate
                }
                m_gizmo_components.insert(best);
                m_last_selected = best;
                inserted = true;
                if (auto it = m_meta_objects.find(reinterpret_cast<sdk::UObjectBase*>(best));
                    it != m_meta_objects.end() && it->second != nullptr) {
                    name = utility::narrow(it->second->full_name);
                }
            }
        }
        if (inserted) {
            spdlog::info("[UObjectHook] click-select added gizmo target: {} (dist {:.0f})", name, best_t);
            dispatch_gizmo_target_event(best, true);
            // Make the component movable so the gizmo can actually translate it — StaticMeshComponents
            // default to Static mobility and won't move otherwise. Writes the reflected Mobility enum
            // byte = EComponentMobility::Movable(2). (handle_click_select runs on the game thread.)
            if (m_gizmo_set_movable) {
                try {
                    if (auto* mob = best->get_class()->find_property(L"Mobility"); mob != nullptr) {
                        *mob->get_data<uint8_t>(best) = 2;
                    }
                } catch (...) {}
            }
            // Tell draw_component_gizmos a pick consumed this left-press, so the gizmo-axis grab is
            // suppressed this frame (the disarm below would otherwise re-enable can_start mid-frame
            // and the same click could grab an axis near the cursor).
            m_click_select_picked_frame = true;
            // Stay armed in MULTI-select mode so successive world clicks keep ADDING gizmo targets —
            // that is what "multigizmo" means, and re-arming "Pick" per object was the friction that
            // made multi-select feel broken. Auto-disarm only in single-select mode (pick one and
            // stop). "keep picking" forces staying armed even in single-select. Esc / the "Picking…"
            // button still cancel at any time.
            if (m_click_select_single && !m_click_select_sticky) {
                m_click_select_mode = false;
            }
        }
    }
}

bool UObjectHook::wants_active_ui() const {
    return g_framework->is_drawing_ui() || m_show_main_window || m_click_select_mode ||
           m_show_class_browser || m_show_function_caller || m_show_options_window || m_has_gizmos;
}

// JSON payload: {"active_index":0,"count":N,"targets":[{"address":hex,"full_name","short_name",
// "screen":{"x","y"},"world":{"x","y","z"}}, ...]}. The active (scroll-focused) candidate is always
// moved to index 0; the rest keep their existing nearest-first order. `address` is plain hex (no "0x"
// prefix — matches object_from_path_or_address's own reader) so Lua can round-trip it through
// uevr.api:to_uobject(tonumber(addr, 16)).
void UObjectHook::dispatch_picker_selection_event() {
    try {
        auto push_target = [](nlohmann::json& arr, const PickCandidate& c) {
            char addr_buf[24];
            std::snprintf(addr_buf, sizeof(addr_buf), "%llx", (unsigned long long)(uintptr_t)c.comp);
            arr.push_back({
                {"address", addr_buf},
                {"full_name", c.full_path},
                {"short_name", c.short_label},
                {"screen", {{"x", c.screen.x}, {"y", c.screen.y}}},
                {"world", {{"x", c.world.x}, {"y", c.world.y}, {"z", c.world.z}}}
            });
        };

        nlohmann::json targets = nlohmann::json::array();
        if (m_pick_cycle >= 0 && m_pick_cycle < (int)m_pick_cache.size()) {
            push_target(targets, m_pick_cache[m_pick_cycle]);
        }
        for (size_t i = 0; i < m_pick_cache.size(); ++i) {
            if ((int)i == m_pick_cycle) continue;
            push_target(targets, m_pick_cache[i]);
        }

        const nlohmann::json payload{
            {"active_index", 0},
            {"count", targets.size()},
            {"targets", targets}
        };
        const auto data = payload.dump();
        // dispatch_lua_event (NOT dispatch_custom_event — that's the native-plugin-to-plugin bus and
        // never reaches Lua on its own) is LuaLoader::dispatch_event, the direct bridge to Lua's
        // uevr.sdk.callbacks.on_lua_event(name, data).
        LuaLoader::get()->dispatch_event("uobjecthook_picker_selection", data);
    } catch (const std::exception& e) {
        spdlog::error("[UObjectHook] dispatch_picker_selection_event failed: {}", e.what());
    } catch (...) {
        spdlog::error("[UObjectHook] dispatch_picker_selection_event failed");
    }
}

// JSON payload: {"address":hex,"full_name","added":bool}. Fired whenever a component enters/leaves
// m_gizmo_components (click-select commit, the "Show gizmo" checkbox, "Remove from selection") so a
// Lua script can apply/clear its own overlay-material or debug visualization in step with the
// built-in gizmo highlight. Callers MUST NOT hold m_mutex when calling this — dispatch_custom_event
// fans out to native-plugin callbacks that may re-enter UObjectHook.
void UObjectHook::dispatch_gizmo_target_event(sdk::USceneComponent* comp, bool added) {
    if (comp == nullptr) {
        return;
    }
    try {
        char addr_buf[24];
        std::snprintf(addr_buf, sizeof(addr_buf), "%llx", (unsigned long long)(uintptr_t)comp);
        std::string full_name;
        {
            std::shared_lock _{m_mutex};
            if (auto it = m_meta_objects.find(reinterpret_cast<sdk::UObjectBase*>(comp));
                it != m_meta_objects.end() && it->second != nullptr) {
                full_name = utility::narrow(it->second->full_name);
            }
        }
        const nlohmann::json payload{
            {"address", addr_buf},
            {"full_name", full_name},
            {"added", added}
        };
        const auto data = payload.dump();
        LuaLoader::get()->dispatch_event("uobjecthook_gizmo_target", data); // see note in dispatch_picker_selection_event
    } catch (const std::exception& e) {
        spdlog::error("[UObjectHook] dispatch_gizmo_target_event failed: {}", e.what());
    } catch (...) {
        spdlog::error("[UObjectHook] dispatch_gizmo_target_event failed");
    }
}

// Toggleable overlay icon over every live light component. Reuses m_objects_by_class (populated by
// walking each object's FULL super chain on creation — see the m_objects_by_class insertion in
// add_object — so looking up PointLightComponent/SpotLightComponent/DirectionalLightComponent's
// UClass directly gets every live instance of that concrete type without needing a base
// ULightComponent lookup). World positions are refreshed on a throttle (get_world_location is a
// ProcessEvent call, and lights rarely move) — only the screen projection re-runs every frame, so the
// icons still track camera movement smoothly.
void UObjectHook::draw_light_icons() {
    if (!m_show_light_icons) {
        return;
    }

    static const auto point_light_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.PointLightComponent");
    static const auto spot_light_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SpotLightComponent");
    static const auto dir_light_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.DirectionalLightComponent");

    const auto now = std::chrono::steady_clock::now();
    if (m_light_icon_cache.empty() || (now - m_light_icon_last_refresh) > std::chrono::milliseconds(500)) {
        m_light_icon_last_refresh = now;
        std::vector<LightIconEntry> fresh;
        auto collect = [&](sdk::UClass* cls, int kind) {
            if (cls == nullptr) return;
            std::shared_lock _{m_mutex};
            auto it = m_objects_by_class.find(cls);
            if (it == m_objects_by_class.end()) return;
            for (auto* base : it->second) {
                if (base == nullptr || !this->exists_unsafe(base)) continue;
                auto* comp = (sdk::USceneComponent*)base;
                try {
                    fresh.push_back(LightIconEntry{comp, comp->get_world_location(), kind});
                } catch (...) {}
            }
        };
        collect(point_light_t, 0);
        collect(spot_light_t, 1);
        collect(dir_light_t, 2);
        m_light_icon_cache = std::move(fresh);
    }

    if (m_light_icon_cache.empty()) {
        return;
    }

    auto engine = sdk::UGameEngine::get();
    auto world = engine != nullptr ? engine->get_world() : nullptr;
    auto ugs = sdk::UGameplayStatics::get();
    auto pc = (world != nullptr && ugs != nullptr) ? ugs->get_player_controller(world, 0) : nullptr;
    if (ugs == nullptr || pc == nullptr) {
        return;
    }

    auto* dl = ImGui::GetForegroundDrawList();
    const auto* vp = ImGui::GetMainViewport();
    for (auto& e : m_light_icon_cache) {
        if (e.light == nullptr || !this->exists_unsafe((sdk::UObjectBase*)e.light)) continue;
        glm::vec2 sp{0.0f, 0.0f};
        if (!ugs->world_to_screen(pc, e.world, &sp)) continue;
        ImVec2 c{sp.x, sp.y};
        if (auto vr = VR::get(); vr != nullptr && vr->is_hmd_active()) {
            c = vr->get_overlay_component().transform_world_aligned_to_overlay(c);
        }
        if (c.x < vp->Pos.x || c.x > vp->Pos.x + vp->Size.x || c.y < vp->Pos.y || c.y > vp->Pos.y + vp->Size.y) continue;

        // Simple bulb glyph: circle + rays. Directional lights get more/longer rays (sun-like);
        // point/spot get a compact 4-ray bulb.
        const ImU32 col = IM_COL32(255, 225, 90, 235);
        constexpr float r = 6.0f;
        dl->AddCircle(c, r, col, 12, 1.5f);
        const int rays = e.kind == 2 ? 8 : 4;
        const float ray_len = e.kind == 2 ? 8.0f : 5.0f;
        for (int i = 0; i < rays; ++i) {
            const float ang = (6.28318530718f / rays) * i;
            const ImVec2 a{c.x + std::cos(ang) * (r + 2.0f), c.y + std::sin(ang) * (r + 2.0f)};
            const ImVec2 b{c.x + std::cos(ang) * (r + 2.0f + ray_len), c.y + std::sin(ang) * (r + 2.0f + ray_len)};
            dl->AddLine(a, b, col, 1.5f);
        }
    }
}

void UObjectHook::draw_component_gizmos() {
    // Per-frame state for the picker/drag-scroll interplay. picked_frame is set by
    // handle_click_select if it consumes the click this frame; busy starts as "picker armed" and is
    // refined to include a grabbed/hot gizmo below (read by the global drag-scroll to yield in VR).
    m_click_select_picked_frame = false;
    m_gizmo_or_picker_busy = m_click_select_mode;

    // Click-to-select runs before the early-out below so it can seed the very first gizmo
    // target. It only does work while m_click_select_mode is on and the left button was
    // just pressed; otherwise it returns immediately.
    if (m_click_select_mode) {
        try { handle_click_select(); }
        catch (const std::exception& e) { spdlog::error("[UObjectHook] click-select threw: {}", e.what()); }
        catch (...)                     { spdlog::error("[UObjectHook] click-select threw (unknown)"); }
    }

    // Which axis of which component is being screen-dragged. Only this function
    // touches it, so a function-local static is enough.
    static sdk::USceneComponent* s_drag_comp = nullptr;
    static int s_drag_axis = -1;

    // This frame's working set: the explicit "Show gizmo" list, plus — when enabled — any
    // component a motion controller is currently adjusting, so a gizmo appears on whatever
    // you grab in VR without ticking "Show gizmo" first. Transient: never mutates
    // m_gizmo_components.
    // Snapshot under the shared lock: UI-thread writers (Clear button, property checkboxes,
    // the w2s context menu's "Remove") mutate m_gizmo_components under the unique lock, so copying
    // it unlocked here could read the container mid-rehash (UB). Matches the mc copy just below.
    std::unordered_set<sdk::USceneComponent*> draw_comps;
    {
        std::shared_lock _{m_mutex};
        draw_comps = m_gizmo_components;
    }
    if (m_auto_gizmo_on_adjust) {
        std::unordered_map<sdk::USceneComponent*, std::shared_ptr<MotionControllerState>> mc;
        {
            std::shared_lock _{m_mutex};
            mc = m_motion_controller_attached_components;
        }
        for (const auto& [comp, state] : mc) {
            if (comp != nullptr && state != nullptr && state->adjusting) {
                draw_comps.insert(comp);
            }
        }
    }

    // Drives wants_vr_pointer(): true whenever any gizmo is shown this frame. OverlayComponent reads
    // it to keep the VR controller pointer live (io.MousePos + clicks) even over EMPTY space, where
    // gizmos float on the background draw-list — otherwise the pointer only updates over imgui
    // windows and VR users can't hover/grab gizmo handles in open space.
    m_has_gizmos = !draw_comps.empty();

    // Cleared here (and re-published after projection below) so the overlay auto-depth falls back to
    // the configured distance on any early-out this frame.
    m_nearest_gizmo_dist_ue.store(-1.0f);

    // Overlay-material highlight sync: apply to newly selected comps, restore deselected ones. Runs
    // BEFORE the empty early-out so turning the feature off / clearing the selection still restores.
    {
        // Lazy auto-find of a default highlight material (config may enable the toggle before any
        // material was ever picked). Engine-content materials that are essentially always loaded.
        if (m_highlight_overlay_material && m_highlight_material == nullptr && !m_highlight_material_search_attempted) {
            m_highlight_material_search_attempted = true;
            GameThreadWorker::get().enqueue([this]() {
                static const wchar_t* candidates[] = {
                    L"Material /Engine/EngineDebugMaterials/DebugMeshMaterial.DebugMeshMaterial",
                    L"Material /Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial",
                    L"Material /Engine/EngineMaterials/WidgetMaterial.WidgetMaterial",
                    L"Material /Engine/EngineMaterials/DefaultDeferredDecalMaterial.DefaultDeferredDecalMaterial",
                };
                for (auto path : candidates) {
                    if (auto mat = sdk::find_uobject<sdk::UObject>(path); mat != nullptr) {
                        m_highlight_material = mat;
                        SPDLOG_INFO("[UObjectHook] Overlay-highlight material auto-selected: {}", utility::narrow(path));
                        return;
                    }
                }
                SPDLOG_WARN("[UObjectHook] No default overlay-highlight material found — pick one in the gizmo options");
            });
        }

        const bool hl_active = m_highlight_overlay_material && m_highlight_material != nullptr;
        std::unordered_map<sdk::USceneComponent*, sdk::UObject*> originals;
        {
            std::shared_lock _{m_mutex};
            originals = m_overlay_mat_originals;
        }
        for (const auto& [comp, orig] : originals) {
            if (!hl_active || !draw_comps.contains(comp)) {
                restore_overlay_highlight(comp);
            }
        }
        if (hl_active) {
            for (auto* comp : draw_comps) {
                if (!originals.contains(comp)) {
                    apply_overlay_highlight(comp, m_highlight_material);
                }
            }
        }
    }

    if (draw_comps.empty()) {
        s_drag_comp = nullptr;
        s_drag_axis = -1;
        return;
    }

    // "Hide (don't remove) gizmos when UI closed": m_gizmo_components stays intact above (targets
    // aren't touched), we just skip rendering/hit-testing/dragging this frame while no UObjectHook
    // panel is open. Deliberately excludes m_has_gizmos/m_click_select_mode from the "UI open" check
    // (unlike wants_active_ui()) so hiding doesn't get stuck permanently true from its own targets.
    // Also un-sets m_has_gizmos: it's what wants_active_ui() (-> force_input_capture) uses to decide
    // whether to block passthrough for "gizmos visible", so a hidden gizmo must stop counting as
    // visible or passthrough would stay blocked purely because targets are still selected.
    if (m_hide_gizmos_when_ui_closed) {
        const bool ui_open = g_framework->is_drawing_ui() || m_show_main_window ||
            m_show_class_browser || m_show_function_caller || m_show_options_window;
        if (!ui_open) {
            m_has_gizmos = false;
            s_drag_comp = nullptr;
            s_drag_axis = -1;
            return;
        }
    }

    auto engine = sdk::UGameEngine::get();
    auto world = engine != nullptr ? engine->get_world() : nullptr;
    if (world == nullptr) {
        return;
    }

    auto ugs = sdk::UGameplayStatics::get();
    if (ugs == nullptr) {
        return;
    }

    auto pc = ugs->get_player_controller(world, 0);
    if (pc == nullptr) {
        return;
    }

    auto* dl = ImGui::GetBackgroundDrawList();
    if (dl == nullptr) {
        return;
    }

    const float kAxisLen = m_gizmo_axis_len; // world units (UE = cm), user-tunable
    constexpr float kHitPx2 = 144.0f;        // 12px hit radius, squared

    // In VR the gizmo is painted onto the framework UI overlay quad. ProjectWorldToScreen
    // produces coords that are world-aligned for the SLATE overlay (UI closed); once the
    // UI opens the framework quad is placed differently, so the same pixels drift. Remap
    // through the overlay so the gizmo stays on the object while the UI is open (the only
    // time it can be grabbed). Identity outside VR / when the UI is closed.
    auto vr = VR::get();
    const bool remap_overlay = vr != nullptr && vr->is_hmd_active();

    auto project = [&](const glm::vec3& wl, ImVec2& out) -> bool {
        glm::vec3 w = wl;
        glm::vec2 sp{0.0f, 0.0f};
        if (!ugs->world_to_screen(pc, w, &sp)) {
            return false; // behind camera / off-screen
        }
        out = ImVec2{sp.x, sp.y};
        if (remap_overlay) {
            out = vr->get_overlay_component().transform_world_aligned_to_overlay(out);
        }
        return true;
    };

    // Squared distance from point p to segment [a,b], in screen pixels.
    auto seg_dist2 = [](const ImVec2& p, const ImVec2& a, const ImVec2& b) -> float {
        const float vx = b.x - a.x, vy = b.y - a.y;
        const float wx = p.x - a.x, wy = p.y - a.y;
        const float vv = vx * vx + vy * vy;
        float t = vv > 0.0f ? (wx * vx + wy * vy) / vv : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const float cx = a.x + t * vx - p.x;
        const float cy = a.y + t * vy - p.y;
        return cx * cx + cy * cy;
    };

    // True if point p is inside the convex quad a->b->c->d (consistent winding).
    // Used so the whole translate plane-handle square is grabbable (D1), not just its corner.
    auto point_in_quad = [](const ImVec2& p, const ImVec2& a, const ImVec2& b, const ImVec2& c, const ImVec2& d) -> bool {
        auto cross = [](const ImVec2& o, const ImVec2& u, const ImVec2& v) -> float {
            return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x);
        };
        const float s0 = cross(a, b, p);
        const float s1 = cross(b, c, p);
        const float s2 = cross(c, d, p);
        const float s3 = cross(d, a, p);
        const bool has_neg = (s0 < 0.0f) || (s1 < 0.0f) || (s2 < 0.0f) || (s3 < 0.0f);
        const bool has_pos = (s0 > 0.0f) || (s1 > 0.0f) || (s2 > 0.0f) || (s3 > 0.0f);
        return !(has_neg && has_pos);
    };

    // Combined (all-in-one) gizmo (m_gizmo_mode == 3) packs translate arrows, rotate rings and
    // scale handles onto ONE interactive gizmo. To stop the handles overlapping they sit at distinct
    // radii: center(0) < translate arrow[0.18..0.82] < scale square(tip) < rotate ring(1.28). scaled_pt
    // pushes a projected point toward/away from the screen origin by a factor — a cheap screen-space
    // scale, exact enough since hit-test and draw use the SAME factor. The drag math is unaffected (it
    // uses each axis's full screen direction, not the visual handle length).
    // Radial layout (swapped vs the first cut so translate is the long OUTER arrow, and
    // scale the inner box): center(0) < scale box(0.45) < translate arrow[0.62..1.0] < rotate ring(1.4).
    constexpr float kRingScaleCombined = 1.40f; // rotate rings: outer
    constexpr float kScaleHandleT = 0.45f;      // scale squares: inner (closer to center than the arrows)
    constexpr float kArrowBaseT = 0.62f;        // translate arrow starts past the scale box
    constexpr float kArrowTipT = 1.0f;          // ...and runs out to the axis tip
    auto scaled_pt = [](const ImVec2& origin, const ImVec2& p, float s) -> ImVec2 {
        return ImVec2{origin.x + (p.x - origin.x) * s, origin.y + (p.y - origin.y) * s};
    };

    // #2 (flat): is gizmo axis i of component c currently being driven by an inspector transform
    // slider? Set by note_driven() in the transform editor; expires a couple frames after the last
    // value change so the highlight tracks active dragging regardless of UI/gizmo draw order.
    const int gizmo_fc = ImGui::GetFrameCount();
    auto driven_hot = [this, gizmo_fc](sdk::USceneComponent* c, int i) -> bool {
        const int age = gizmo_fc - (int)m_driven_frame;
        return m_driven_comp == c && m_driven_axis == i && age >= 0 && age <= 2;
    };

    struct Axis { glm::vec3 dir; ImU32 col; };
    static const Axis axes[3] = {
        { glm::vec3{1.0f, 0.0f, 0.0f}, IM_COL32(255,  60,  60, 255) }, // X red
        { glm::vec3{0.0f, 1.0f, 0.0f}, IM_COL32( 60, 255,  60, 255) }, // Y green
        { glm::vec3{0.0f, 0.0f, 1.0f}, IM_COL32( 80, 120, 255, 255) }, // Z blue
    };

    auto& io = ImGui::GetIO();
    // Only let a drag START when the overlay is up and the cursor isn't over an
    // actual imgui widget. We deliberately do NOT gate on io.WantCaptureMouse:
    // UEVR's overlay runs a fullscreen dockspace host window, so WantCaptureMouse
    // is true across the whole screen even over the transparent central node where
    // the gizmo is visible — gating on it made the gizmo undraggable. Gating on
    // hovered/active item instead lets axis clicks land in open space while still
    // yielding to buttons/sliders. An in-flight drag keeps going regardless.
    const bool can_start = wants_active_ui() && !m_click_select_mode && !m_click_select_picked_frame &&
        !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive();

    // Release / invalidate an in-flight drag.
    if (s_drag_comp != nullptr) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
            !draw_comps.contains(s_drag_comp) || !this->exists(s_drag_comp)) {
            s_drag_comp = nullptr;
            s_drag_axis = -1;
        }
    }

    // Rotate mode draws a standard ring per axis (a circle in the plane normal to
    // that axis) instead of a line + tip; project kRingSeg points around each.
    constexpr int kRingSeg = 20;
    struct Screen {
        sdk::USceneComponent* comp{};
        glm::vec3 origin{};
        ImVec2 s_origin{};
        ImVec2 tip[3]{};
        bool tip_ok[3]{};
        ImVec2 plane[3]{};   // translate-mode 2-axis plane handles (corner point)
        bool plane_ok[3]{};
        ImVec2 ring[3][kRingSeg]{};
        bool ring_ok[3][kRingSeg]{};
    };
    std::vector<Screen> screens{};
    std::vector<sdk::USceneComponent*> dead{};

    for (auto* comp : draw_comps) {
        if (comp == nullptr || !this->exists(comp)) {
            dead.push_back(comp);
            continue;
        }

        Screen sc{};
        sc.comp = comp;
        try {
            sc.origin = comp->get_world_location();
        } catch (...) {
            continue;
        }
        if (!project(sc.origin, sc.s_origin)) {
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            sc.tip_ok[i] = project(sc.origin + axes[i].dir * kAxisLen, sc.tip[i]);
        }
        if (m_gizmo_mode == 0 || m_gizmo_mode == 3) { // translate plane handles AND combined-mode scale-plane handles
            // Plane handle p sits in the plane of axes (p+1) and (p+2), offset a bit
            // out from the origin along both.
            for (int p = 0; p < 3; ++p) {
                const glm::vec3& ua = axes[(p + 1) % 3].dir;
                const glm::vec3& ub = axes[(p + 2) % 3].dir;
                sc.plane_ok[p] = project(sc.origin + (ua + ub) * (kAxisLen * 0.4f), sc.plane[p]);
            }
        }
        if (m_gizmo_mode == 1 || m_gizmo_mode == 3) { // rotate rings: pure-rotate mode AND combined
            // Ring radius is decoupled from the translate axis length (so cranking the gizmo scale for
            // big arrows doesn't balloon the rings and wreck centering, worst in VR) and applies the
            // same way in both pure-rotate and Combined mode; Combined's radial layout still pushes the
            // rings outside the arrows via kRingScaleCombined below.
            const float ring_radius = m_gizmo_ring_radius;
            for (int i = 0; i < 3; ++i) {
                const glm::vec3& u = axes[(i + 1) % 3].dir;
                const glm::vec3& v = axes[(i + 2) % 3].dir;
                for (int s = 0; s < kRingSeg; ++s) {
                    const float t = ((float)s / (float)kRingSeg) * 6.28318530718f;
                    sc.ring_ok[i][s] = project(sc.origin + ring_radius * (ImCos(t) * u + ImSin(t) * v), sc.ring[i][s]);
                }
            }
        }
        screens.push_back(sc);
    }

    // Publish the nearest gizmo-target distance (world units) for the overlay auto-depth.
    // m_last_camera_location comes from the stereo callback (same benign cross-thread read the
    // camera-attach UI already does).
    {
        float best_d = -1.0f;
        for (const auto& sc : screens) {
            const float d = glm::length(sc.origin - m_last_camera_location);
            if (best_d < 0.0f || d < best_d) {
                best_d = d;
            }
        }
        m_nearest_gizmo_dist_ue.store(best_d);
    }

    // Nearest axis to the cursor across all gizmos (for hover highlight + the
    // axis a fresh click would grab).
    sdk::USceneComponent* hover_comp = nullptr;
    int hover_axis = -1;
    if (can_start && s_drag_comp == nullptr) {
        float best = kHitPx2;
        for (const auto& sc : screens) {
            if (m_gizmo_mode == 3) {
                // Combined gizmo: hit-test every handle type with distinct codes — translate arrows
                // (0..2), rotate rings (10..12), scale squares (20..22), uniform center (6).
                for (int i = 0; i < 3; ++i) {
                    if (sc.tip_ok[i]) {
                        const ImVec2 ab = scaled_pt(sc.s_origin, sc.tip[i], kArrowBaseT);
                        const ImVec2 at = scaled_pt(sc.s_origin, sc.tip[i], kArrowTipT);
                        const float da = seg_dist2(io.MousePos, ab, at);
                        if (da < best) { best = da; hover_comp = sc.comp; hover_axis = i; }
                        const ImVec2 sq = scaled_pt(sc.s_origin, sc.tip[i], kScaleHandleT); // scale box (inner)
                        const float dxs = io.MousePos.x - sq.x, dys = io.MousePos.y - sq.y;
                        const float ds = dxs * dxs + dys * dys;
                        if (ds < best) { best = ds; hover_comp = sc.comp; hover_axis = 20 + i; }
                    }
                    for (int s = 0; s < kRingSeg; ++s) {
                        const int s2 = (s + 1) % kRingSeg;
                        if (!sc.ring_ok[i][s] || !sc.ring_ok[i][s2]) continue;
                        const float d2 = seg_dist2(io.MousePos,
                            scaled_pt(sc.s_origin, sc.ring[i][s], kRingScaleCombined),
                            scaled_pt(sc.s_origin, sc.ring[i][s2], kRingScaleCombined));
                        if (d2 < best) { best = d2; hover_comp = sc.comp; hover_axis = 10 + i; }
                    }
                }
                const float dxc = io.MousePos.x - sc.s_origin.x, dyc = io.MousePos.y - sc.s_origin.y;
                const float dc = dxc * dxc + dyc * dyc; // center = uniform scale
                if (dc < best) { best = dc; hover_comp = sc.comp; hover_axis = 6; }
                for (int p = 0; p < 3; ++p) { // scale-plane handles (multiaxis scale): codes 30..32
                    if (!sc.plane_ok[p]) continue;
                    const float dxp = io.MousePos.x - sc.plane[p].x, dyp = io.MousePos.y - sc.plane[p].y;
                    const float dp = dxp * dxp + dyp * dyp;
                    if (dp < best) { best = dp; hover_comp = sc.comp; hover_axis = 30 + p; }
                }
                continue;
            }
            for (int i = 0; i < 3; ++i) {
                if (m_gizmo_mode == 1) {
                    for (int s = 0; s < kRingSeg; ++s) {
                        const int s2 = (s + 1) % kRingSeg;
                        if (!sc.ring_ok[i][s] || !sc.ring_ok[i][s2]) continue;
                        const float d2 = seg_dist2(io.MousePos, sc.ring[i][s], sc.ring[i][s2]);
                        if (d2 < best) { best = d2; hover_comp = sc.comp; hover_axis = i; }
                    }
                } else {
                    if (!sc.tip_ok[i]) continue;
                    const float d2 = seg_dist2(io.MousePos, sc.s_origin, sc.tip[i]);
                    if (d2 < best) { best = d2; hover_comp = sc.comp; hover_axis = i; }
                }
            }
            // Multi-axis handles: translate plane handles (axis 3..5) and the scale
            // center handle (axis 6, uniform).
            if (m_gizmo_mode == 0) {
                for (int p = 0; p < 3; ++p) {
                    if (!sc.plane_ok[p]) continue;
                    const int ia = (p + 1) % 3, ib = (p + 2) % 3;
                    // Corner-point distance (original behavior — keeps the far tip grabbable).
                    const float dx = io.MousePos.x - sc.plane[p].x, dy = io.MousePos.y - sc.plane[p].y;
                    float d2 = dx * dx + dy * dy;
                    // D1: also accept a click anywhere inside the plane quad (origin, ca, corner, cb),
                    // so the whole square grabs the handle — not only its outer tip. Rank an inside-hit
                    // by distance to the quad centroid so it still yields to a single-axis line that
                    // runs closer along one of the quad's edges.
                    if (sc.tip_ok[ia] && sc.tip_ok[ib]) {
                        const ImVec2 ca{sc.s_origin.x + 0.4f * (sc.tip[ia].x - sc.s_origin.x), sc.s_origin.y + 0.4f * (sc.tip[ia].y - sc.s_origin.y)};
                        const ImVec2 cb{sc.s_origin.x + 0.4f * (sc.tip[ib].x - sc.s_origin.x), sc.s_origin.y + 0.4f * (sc.tip[ib].y - sc.s_origin.y)};
                        if (point_in_quad(io.MousePos, sc.s_origin, ca, sc.plane[p], cb)) {
                            const ImVec2 cen{(sc.s_origin.x + ca.x + sc.plane[p].x + cb.x) * 0.25f,
                                             (sc.s_origin.y + ca.y + sc.plane[p].y + cb.y) * 0.25f};
                            const float dxq = io.MousePos.x - cen.x, dyq = io.MousePos.y - cen.y;
                            d2 = std::min(d2, dxq * dxq + dyq * dyq);
                        }
                    }
                    if (d2 < best) { best = d2; hover_comp = sc.comp; hover_axis = 3 + p; }
                }
            } else if (m_gizmo_mode == 2) {
                const float dx = io.MousePos.x - sc.s_origin.x, dy = io.MousePos.y - sc.s_origin.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; hover_comp = sc.comp; hover_axis = 6; }
            }
        }
        if (hover_comp != nullptr && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            s_drag_comp = hover_comp;
            s_drag_axis = hover_axis;
        }
    }

    // A gizmo is grabbed or an axis is hot under the cursor → the global drag-scroll should yield in
    // VR (where it shares the left button with this grab). Combined with the picker-armed state set
    // at the top of the function.
    m_gizmo_or_picker_busy = m_gizmo_or_picker_busy || s_drag_comp != nullptr || hover_comp != nullptr;

    // Apply the active drag. All three modes share the same screen-space mapping
    // (NO view/projection matrix): project origin + axis tip, take the screen
    // vector d for the dragged axis, and use the mouse delta's component along it.
    // Translate maps that to world units, rotate to degrees about the world axis,
    // scale to an additive change on that axis component.
    if (s_drag_comp != nullptr && s_drag_axis >= 0) {
        // Hold Ctrl to snap to the configured steps. Translate/scale snap the absolute value (the
        // gizmo translate axes are world-aligned, so only the dragged component changes); rotate is
        // applied incrementally, so accumulate the swept angle and emit it in snap-sized chunks.
        const bool ctrl_snap = io.KeyCtrl;
        static float s_rot_accum = 0.0f;
        static int s_rot_accum_axis = -1;
        // Reset the swept-angle accumulator when Ctrl is released OR the dragged axis changes, so a
        // leftover partial step from a previous axis doesn't bleed into the next one mid-Ctrl-hold.
        if (!ctrl_snap || s_drag_axis != s_rot_accum_axis) {
            s_rot_accum = 0.0f;
            s_rot_accum_axis = s_drag_axis;
        }
        for (const auto& sc : screens) {
            if (sc.comp != s_drag_comp) continue;
            try {
                // Decode the grabbed handle into (action, axis). Codes 0..2 take their action from the
                // current mode (combined mode 3 -> translate); combined mode also emits 10..12 (rotate)
                // and 20..22 (scale-axis). Codes 3..5 (plane) and 6 (uniform) are handled below.
                int act = -1; // 0 translate, 1 rotate, 2 scale-axis
                int ax = -1;
                if (s_drag_axis >= 0 && s_drag_axis < 3)        { ax = s_drag_axis;      act = (m_gizmo_mode == 1) ? 1 : (m_gizmo_mode == 2) ? 2 : 0; }
                else if (s_drag_axis >= 10 && s_drag_axis < 13) { ax = s_drag_axis - 10; act = 1; }
                else if (s_drag_axis >= 20 && s_drag_axis < 23) { ax = s_drag_axis - 20; act = 2; }

                if (act >= 0 && sc.tip_ok[ax]) {
                    // Single-axis: translate/rotate/scale along axis ax (screen-space mapping).
                    const ImVec2 d{sc.tip[ax].x - sc.s_origin.x, sc.tip[ax].y - sc.s_origin.y};
                    const float len2 = d.x * d.x + d.y * d.y;
                    if (len2 > 1.0f) {
                        const float dot = io.MouseDelta.x * d.x + io.MouseDelta.y * d.y;
                        const float px_along = dot / ImSqrt(len2);
                        if (act == 1) {
                            // Rotate: pixels -> degrees about the world axis. FRotator
                            // {Pitch=x (about Y), Yaw=y (about Z), Roll=z (about X)}.
                            float a = px_along * 0.5f;
                            if (ctrl_snap && m_snap_rotate > 0.0f) {
                                s_rot_accum += a;
                                a = 0.0f;
                                while (s_rot_accum >= m_snap_rotate)  { a += m_snap_rotate; s_rot_accum -= m_snap_rotate; }
                                while (s_rot_accum <= -m_snap_rotate) { a -= m_snap_rotate; s_rot_accum += m_snap_rotate; }
                            }
                            glm::vec3 euler{0.0f, 0.0f, 0.0f};
                            if (ax == 0)      euler.z = a;
                            else if (ax == 1) euler.x = a;
                            else              euler.y = a;
                            if (a != 0.0f) s_drag_comp->add_world_rotation(euler, false, false);
                        } else if (act == 2) {
                            auto scale = s_drag_comp->get_relative_scale();
                            scale[ax] += px_along * 0.01f;
                            if (ctrl_snap && m_snap_scale > 0.0f) {
                                scale[ax] = std::round(scale[ax] / m_snap_scale) * m_snap_scale;
                            }
                            s_drag_comp->set_relative_scale(scale);
                        } else {
                            const float move = (dot / len2) * kAxisLen;
                            if (move != 0.0f) {
                                glm::vec3 newloc = sc.origin + axes[ax].dir * move;
                                if (ctrl_snap && m_snap_translate > 0.0f) {
                                    newloc[ax] = std::round(newloc[ax] / m_snap_translate) * m_snap_translate;
                                }
                                s_drag_comp->set_world_location(newloc, false, false);
                            }
                        }
                    }
                } else if (s_drag_axis >= 3 && s_drag_axis < 6) {
                    // Plane translate: decompose the mouse delta into the two in-plane
                    // axes' screen directions (solve d = a*u_screen + b*v_screen), then
                    // move that many world units along each axis. No view matrix needed.
                    const int p = s_drag_axis - 3;
                    const int ia = (p + 1) % 3, ib = (p + 2) % 3;
                    if (sc.tip_ok[ia] && sc.tip_ok[ib]) {
                        const ImVec2 us{sc.tip[ia].x - sc.s_origin.x, sc.tip[ia].y - sc.s_origin.y};
                        const ImVec2 vs{sc.tip[ib].x - sc.s_origin.x, sc.tip[ib].y - sc.s_origin.y};
                        const float det = us.x * vs.y - vs.x * us.y;
                        if (ImAbs(det) > 1.0f) {
                            const float a = (io.MouseDelta.x * vs.y - vs.x * io.MouseDelta.y) / det;
                            const float b = (us.x * io.MouseDelta.y - io.MouseDelta.x * us.y) / det;
                            if (a != 0.0f || b != 0.0f) {
                                const glm::vec3 mv = axes[ia].dir * (a * kAxisLen) + axes[ib].dir * (b * kAxisLen);
                                glm::vec3 newloc = sc.origin + mv;
                                if (ctrl_snap && m_snap_translate > 0.0f) {
                                    newloc[ia] = std::round(newloc[ia] / m_snap_translate) * m_snap_translate;
                                    newloc[ib] = std::round(newloc[ib] / m_snap_translate) * m_snap_translate;
                                }
                                s_drag_comp->set_world_location(newloc, false, false);
                            }
                        }
                    }
                } else if (s_drag_axis == 6) {
                    // Center handle: uniform scale by vertical drag (drag up = larger).
                    const float delta = -io.MouseDelta.y * 0.01f;
                    if (delta != 0.0f) {
                        auto scale = s_drag_comp->get_relative_scale();
                        scale += glm::vec3{delta, delta, delta};
                        if (ctrl_snap && m_snap_scale > 0.0f) {
                            scale.x = std::round(scale.x / m_snap_scale) * m_snap_scale;
                            scale.y = std::round(scale.y / m_snap_scale) * m_snap_scale;
                            scale.z = std::round(scale.z / m_snap_scale) * m_snap_scale;
                        }
                        s_drag_comp->set_relative_scale(scale);
                    }
                } else if (s_drag_axis >= 30 && s_drag_axis < 33) {
                    // Scale-plane handle (multiaxis scale): scale the two in-plane axes together by the
                    // drag along the plane handle's diagonal screen direction.
                    const int p = s_drag_axis - 30;
                    const int ia = (p + 1) % 3, ib = (p + 2) % 3;
                    if (sc.plane_ok[p]) {
                        const ImVec2 d{sc.plane[p].x - sc.s_origin.x, sc.plane[p].y - sc.s_origin.y};
                        const float len2 = d.x * d.x + d.y * d.y;
                        if (len2 > 1.0f) {
                            const float px_along = (io.MouseDelta.x * d.x + io.MouseDelta.y * d.y) / ImSqrt(len2);
                            if (px_along != 0.0f) {
                                auto scale = s_drag_comp->get_relative_scale();
                                scale[ia] += px_along * 0.01f;
                                scale[ib] += px_along * 0.01f;
                                if (ctrl_snap && m_snap_scale > 0.0f) {
                                    scale[ia] = std::round(scale[ia] / m_snap_scale) * m_snap_scale;
                                    scale[ib] = std::round(scale[ib] / m_snap_scale) * m_snap_scale;
                                }
                                s_drag_comp->set_relative_scale(scale);
                            }
                        }
                    }
                }
            } catch (...) {}
            break;
        }
    }

    // VR attach routing: if the actively-dragged gizmo target is attached to a motion controller, the
    // per-frame attach-follow (update_motion_controller_components) would otherwise clobber the gizmo's
    // world write every frame. Publish the dragged comp so the stereo path skips its follow and re-seeds
    // its location/rotation offset from the gizmo-updated transform instead. Also force it permanent so
    // the change actually persists (non-permanent attachments reset to origin each frame).
    {
        sdk::USceneComponent* routed = nullptr;
        if (s_drag_comp != nullptr) {
            std::shared_lock _{m_mutex};
            if (auto it = m_motion_controller_attached_components.find(s_drag_comp);
                it != m_motion_controller_attached_components.end() && it->second != nullptr) {
                routed = s_drag_comp;
                it->second->permanent = true;
            }
        }
        m_flat_gizmo_drag_comp.store(routed);
    }

    for (const auto& sc : screens) {
        // Selection highlight: a world->screen reticle (corner brackets + center dot) at the object's
        // projected origin so it's obvious what's selected. On-screen only; the edge arrow below
        // already covers the off-screen case. The most-recently-picked one gets a brighter tint.
        if (m_highlight_selection) {
            const auto* vp0 = ImGui::GetMainViewport();
            const bool onscreen = sc.s_origin.x >= vp0->Pos.x && sc.s_origin.x <= vp0->Pos.x + vp0->Size.x &&
                                  sc.s_origin.y >= vp0->Pos.y && sc.s_origin.y <= vp0->Pos.y + vp0->Size.y;
            if (onscreen) {
                const ImU32 hl = (sc.comp == m_last_selected) ? IM_COL32(255, 230, 90, 245) : IM_COL32(255, 170, 30, 210);
                const ImVec2 c = sc.s_origin;
                const float r = 16.0f, b = 7.0f; // half-box, bracket arm length
                dl->AddLine(ImVec2{c.x - r, c.y - r}, ImVec2{c.x - r + b, c.y - r}, hl, 2.0f);
                dl->AddLine(ImVec2{c.x - r, c.y - r}, ImVec2{c.x - r, c.y - r + b}, hl, 2.0f);
                dl->AddLine(ImVec2{c.x + r, c.y - r}, ImVec2{c.x + r - b, c.y - r}, hl, 2.0f);
                dl->AddLine(ImVec2{c.x + r, c.y - r}, ImVec2{c.x + r, c.y - r + b}, hl, 2.0f);
                dl->AddLine(ImVec2{c.x - r, c.y + r}, ImVec2{c.x - r + b, c.y + r}, hl, 2.0f);
                dl->AddLine(ImVec2{c.x - r, c.y + r}, ImVec2{c.x - r, c.y + r - b}, hl, 2.0f);
                dl->AddLine(ImVec2{c.x + r, c.y + r}, ImVec2{c.x + r - b, c.y + r}, hl, 2.0f);
                dl->AddLine(ImVec2{c.x + r, c.y + r}, ImVec2{c.x + r, c.y + r - b}, hl, 2.0f);
                dl->AddCircleFilled(c, 2.5f, hl);
            }
        }

        // Off-screen indicator (D4): if this gizmo's origin projects outside the viewport, draw
        // an edge-clamped arrow + component name pointing toward it instead of the (invisible,
        // off-screen) gizmo, so you know which way to look. Additive; only fires when off-screen.
        {
            const auto* vp = ImGui::GetMainViewport();
            const ImVec2 vmin = vp->Pos;
            const ImVec2 vmax = ImVec2{vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y};
            if (sc.s_origin.x < vmin.x || sc.s_origin.x > vmax.x || sc.s_origin.y < vmin.y || sc.s_origin.y > vmax.y) {
                const float m = 26.0f;
                const ImVec2 ctr{(vmin.x + vmax.x) * 0.5f, (vmin.y + vmax.y) * 0.5f};
                const ImVec2 e{std::clamp(sc.s_origin.x, vmin.x + m, vmax.x - m), std::clamp(sc.s_origin.y, vmin.y + m, vmax.y - m)};
                ImVec2 d{sc.s_origin.x - ctr.x, sc.s_origin.y - ctr.y};
                const float len = ImSqrt(d.x * d.x + d.y * d.y);
                if (len > 0.001f) { d.x /= len; d.y /= len; }
                const ImVec2 perp{-d.y, d.x};
                const float s = 11.0f;
                const ImVec2 tip{e.x + d.x * s, e.y + d.y * s};
                const ImVec2 b1{e.x - d.x * s + perp.x * s * 0.6f, e.y - d.y * s + perp.y * s * 0.6f};
                const ImVec2 b2{e.x - d.x * s - perp.x * s * 0.6f, e.y - d.y * s - perp.y * s * 0.6f};
                dl->AddTriangleFilled(tip, b1, b2, IM_COL32(255, 220, 60, 235));
                try {
                    std::string n = utility::narrow(sc.comp->get_fname().to_string());
                    dl->AddText(ImVec2{e.x + 8.0f, e.y - 6.0f}, IM_COL32(255, 220, 60, 235), n.c_str());
                } catch (...) {}
                continue; // skip the normal (off-screen) gizmo draw for this one
            }
        }

        // Overlay the actor/component short name + live transform metrics next to each gizmo,
        // placed via the same world->screen projection (sc.s_origin already includes the VR
        // overlay remap). Pure additive text — never affects gizmo interaction. Toggleable.
        if (m_gizmo_show_labels) try {
            std::string cname = utility::narrow(sc.comp->get_fname().to_string());
            std::string aname;
            if (auto* owner = sc.comp->get_owner()) {
                try { aname = utility::narrow(owner->get_fname().to_string()); } catch (...) {}
            }
            const auto rot = sc.comp->get_world_rotation();
            const auto scl = sc.comp->get_relative_scale();
            char buf[256];
            snprintf(buf, sizeof(buf), "%s%s%s\nP %.1f %.1f %.1f\nR %.1f %.1f %.1f\nS %.2f %.2f %.2f",
                aname.c_str(), aname.empty() ? "" : " / ", cname.c_str(),
                sc.origin.x, sc.origin.y, sc.origin.z, rot.x, rot.y, rot.z, scl.x, scl.y, scl.z);
            // Drop-shadow for legibility over arbitrary game backgrounds.
            dl->AddText(ImVec2{sc.s_origin.x + 11.0f, sc.s_origin.y + 11.0f}, IM_COL32(0, 0, 0, 200), buf);
            dl->AddText(ImVec2{sc.s_origin.x + 10.0f, sc.s_origin.y + 10.0f}, IM_COL32(255, 255, 255, 235), buf);
        } catch (...) {}

        // STUB no-op (E1/E2): drive this gizmo's axis from a VR thumbstick while adjusting.
        // See vr_gizmo_stick_adjust() for the implementation context.
        if (remap_overlay) {
            vr_gizmo_stick_adjust(sc.comp);
        }

        if (m_gizmo_mode == 3) {
            // Combined (all-in-one) gizmo: rotate rings (outer) + translate arrows + scale
            // squares (at the tips), all on one interactive gizmo. Handle codes: arrow i, ring 10+i,
            // square 20+i, uniform center 6 (white dot drawn below).
            for (int i = 0; i < 3; ++i) {
                const bool ring_hot = (s_drag_comp == sc.comp && s_drag_axis == 10 + i) ||
                                      (hover_comp == sc.comp && hover_axis == 10 + i) || driven_hot(sc.comp, i);
                const float rth = ring_hot ? m_gizmo_thickness * 1.6f : m_gizmo_thickness;
                for (int s = 0; s < kRingSeg; ++s) {
                    const int s2 = (s + 1) % kRingSeg;
                    if (!sc.ring_ok[i][s] || !sc.ring_ok[i][s2]) continue;
                    dl->AddLine(scaled_pt(sc.s_origin, sc.ring[i][s], kRingScaleCombined),
                                scaled_pt(sc.s_origin, sc.ring[i][s2], kRingScaleCombined), axes[i].col, rth);
                }
                if (!sc.tip_ok[i]) continue;
                const bool arrow_hot = (s_drag_comp == sc.comp && s_drag_axis == i) ||
                                       (hover_comp == sc.comp && hover_axis == i) || driven_hot(sc.comp, i);
                const float ath = arrow_hot ? m_gizmo_thickness * 1.6f : m_gizmo_thickness;
                const ImVec2 ab = scaled_pt(sc.s_origin, sc.tip[i], kArrowBaseT);
                const ImVec2 at = scaled_pt(sc.s_origin, sc.tip[i], kArrowTipT);
                dl->AddLine(ab, at, axes[i].col, ath);                                     // translate shaft
                dl->AddCircleFilled(at, arrow_hot ? m_gizmo_thickness * 2.0f : m_gizmo_thickness * 1.5f, axes[i].col); // arrow head
                const bool sq_hot = (s_drag_comp == sc.comp && s_drag_axis == 20 + i) ||
                                    (hover_comp == sc.comp && hover_axis == 20 + i);
                const float r = sq_hot ? m_gizmo_thickness * 2.4f : m_gizmo_thickness * 1.8f;
                const ImVec2 sq = scaled_pt(sc.s_origin, sc.tip[i], kScaleHandleT);
                dl->AddRectFilled(ImVec2{sq.x - r, sq.y - r}, ImVec2{sq.x + r, sq.y + r}, axes[i].col); // scale box (inner)
            }
            // Scale-plane handles (multiaxis scale): a small dark-outlined box at each plane position,
            // tinted by the axis it is perpendicular to (codes 30..32 -> scale the other two axes).
            for (int p = 0; p < 3; ++p) {
                if (!sc.plane_ok[p]) continue;
                const bool hot = (s_drag_comp == sc.comp && s_drag_axis == 30 + p) ||
                                 (hover_comp == sc.comp && hover_axis == 30 + p);
                const float r = hot ? m_gizmo_thickness * 2.2f : m_gizmo_thickness * 1.6f;
                const ImU32 col = (axes[p].col & 0x00FFFFFF) | ((ImU32)(hot ? 255 : 200) << IM_COL32_A_SHIFT);
                dl->AddRectFilled(ImVec2{sc.plane[p].x - r, sc.plane[p].y - r}, ImVec2{sc.plane[p].x + r, sc.plane[p].y + r}, col);
                dl->AddRect(ImVec2{sc.plane[p].x - r, sc.plane[p].y - r}, ImVec2{sc.plane[p].x + r, sc.plane[p].y + r}, IM_COL32(20, 20, 20, 220));
            }
        } else if (m_gizmo_mode == 1) {
            // Rotate: one standard colored ring per axis (projected polyline).
            for (int i = 0; i < 3; ++i) {
                const bool hot = (s_drag_comp == sc.comp && s_drag_axis == i) ||
                                 (hover_comp == sc.comp && hover_axis == i) ||
                                 driven_hot(sc.comp, i);
                const float th = hot ? m_gizmo_thickness * 1.6f : m_gizmo_thickness;
                for (int s = 0; s < kRingSeg; ++s) {
                    const int s2 = (s + 1) % kRingSeg;
                    if (!sc.ring_ok[i][s] || !sc.ring_ok[i][s2]) continue;
                    dl->AddLine(sc.ring[i][s], sc.ring[i][s2], axes[i].col, th);
                }
            }
        } else {
            for (int i = 0; i < 3; ++i) {
                if (!sc.tip_ok[i]) continue;
                const bool hot = (s_drag_comp == sc.comp && s_drag_axis == i) ||
                                 (hover_comp == sc.comp && hover_axis == i) ||
                                 driven_hot(sc.comp, i);
                const float th = hot ? m_gizmo_thickness * 1.6f : m_gizmo_thickness;
                const float r = hot ? m_gizmo_thickness * 2.0f : m_gizmo_thickness * 1.5f;
                dl->AddLine(sc.s_origin, sc.tip[i], axes[i].col, th);
                if (m_gizmo_mode == 2) {
                    dl->AddRectFilled(ImVec2{sc.tip[i].x - r, sc.tip[i].y - r},
                                      ImVec2{sc.tip[i].x + r, sc.tip[i].y + r}, axes[i].col); // scale: box
                } else {
                    dl->AddCircleFilled(sc.tip[i], r, axes[i].col);                    // translate: dot
                }
            }
            // Translate plane handles: a small semi-transparent quad per plane,
            // tinted by the axis it is perpendicular to.
            if (m_gizmo_mode == 0) {
                for (int p = 0; p < 3; ++p) {
                    const int ia = (p + 1) % 3, ib = (p + 2) % 3;
                    if (!sc.plane_ok[p] || !sc.tip_ok[ia] || !sc.tip_ok[ib]) continue;
                    const bool hot = (s_drag_comp == sc.comp && s_drag_axis == 3 + p) ||
                                     (hover_comp == sc.comp && hover_axis == 3 + p);
                    const ImVec2 ca{sc.s_origin.x + 0.4f * (sc.tip[ia].x - sc.s_origin.x), sc.s_origin.y + 0.4f * (sc.tip[ia].y - sc.s_origin.y)};
                    const ImVec2 cb{sc.s_origin.x + 0.4f * (sc.tip[ib].x - sc.s_origin.x), sc.s_origin.y + 0.4f * (sc.tip[ib].y - sc.s_origin.y)};
                    const ImU32 col = (axes[p].col & 0x00FFFFFF) | ((ImU32)(hot ? 180 : 80) << IM_COL32_A_SHIFT);
                    dl->AddQuadFilled(sc.s_origin, ca, sc.plane[p], cb, col);
                }
            }
        }
        const bool center_hot = (m_gizmo_mode == 2 || m_gizmo_mode == 3) &&
            ((s_drag_comp == sc.comp && s_drag_axis == 6) || (hover_comp == sc.comp && hover_axis == 6));
        dl->AddCircleFilled(sc.s_origin, center_hot ? 8.0f : 4.0f, IM_COL32(255, 255, 255, 255));
        // (All three handle types at once are available via the Combined gizmo mode — m_gizmo_mode == 3.)
    }

    // Right-click a selected object (near its w2s reticle) -> a context menu at the cursor. Right
    // mouse is otherwise unused by the gizmo (left = drag, middle = scroll). Engine-touching actions
    // (visibility / position / spawn call process_event) are deferred to the game thread.
    {
        static sdk::USceneComponent* s_ctx_comp = nullptr;
        if (wants_active_ui() && !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            float best = 34.0f * 34.0f; // ~34px pick radius
            sdk::USceneComponent* hit = nullptr;
            for (const auto& sc : screens) {
                const float dx = io.MousePos.x - sc.s_origin.x, dy = io.MousePos.y - sc.s_origin.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; hit = sc.comp; }
            }
            if (hit != nullptr) {
                s_ctx_comp = hit;
                ImGui::OpenPopup("##uobj_sel_ctx");
            }
        }
        if (ImGui::BeginPopup("##uobj_sel_ctx")) {
            sdk::USceneComponent* c = s_ctx_comp;
            if (c != nullptr && this->exists(c)) {
                // Gizmo targets are (in practice) always root components — the picker only ever offers
                // root components as candidates — so the owning actor is what the user actually thinks
                // of as "the thing I selected". Naming, the Properties glance, "Select new target",
                // "Set view target" and "Destroy" below all target the actor when one resolves;
                // attach/detach/duplicate/summon stay on the component `c` itself since those are
                // inherently component-level engine operations.
                sdk::AActor* owner = nullptr;
                try { owner = c->get_owner(); } catch (...) {}

                std::string cn;
                try { cn = utility::narrow((owner != nullptr ? (sdk::UObject*)owner : (sdk::UObject*)c)->get_fname().to_string()); } catch (...) {}
                ImGui::TextDisabled("%s", cn.c_str());
                ImGui::Separator();

                // Gizmo mode switch right from the target's own context menu — no need to go to the
                // Options window just to change Move/Rotate/Scale/Combined.
                if (ImGui::BeginMenu("Gizmo mode")) {
                    if (ImGui::RadioButton("Move##ctxgizmo", &m_gizmo_mode, 0)) ImGui::CloseCurrentPopup();
                    if (ImGui::RadioButton("Rotate##ctxgizmo", &m_gizmo_mode, 1)) ImGui::CloseCurrentPopup();
                    if (ImGui::RadioButton("Scale##ctxgizmo", &m_gizmo_mode, 2)) ImGui::CloseCurrentPopup();
                    if (ImGui::RadioButton("Combined##ctxgizmo", &m_gizmo_mode, 3)) ImGui::CloseCurrentPopup();
                    ImGui::EndMenu();
                }
                ImGui::Separator();

                if (ImGui::MenuItem("Select new target")) {
                    GameThreadWorker::get().enqueue([this, c]() {
                        std::unique_lock _{m_mutex};
                        m_gizmo_components.erase(c);
                    });
                    m_click_select_mode = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Clears this target and arms the picker so the next world click replaces it.");
                }
                if (owner != nullptr && ImGui::MenuItem("Set view target to actor")) {
                    GameThreadWorker::get().enqueue([this, owner]() {
                        if (!this->exists(owner)) return;
                        try {
                            auto* ugs = sdk::UGameplayStatics::get();
                            auto* engine = sdk::UGameEngine::get();
                            auto* world = engine != nullptr ? engine->get_world() : nullptr;
                            auto* pc = (ugs != nullptr && world != nullptr) ? ugs->get_player_controller(world, 0) : nullptr;
                            if (pc == nullptr) return;
                            // Same SetViewTargetWithBlend reflected call spawn_view_camera() uses.
                            if (auto fn = pc->get_class()->find_function(L"SetViewTargetWithBlend"); fn != nullptr) {
                                SetViewTargetWithBlendParams params{};
                                params.NewViewTarget = owner;
                                params.BlendTime = 0.3f;
                                pc->process_event(fn, &params);
                            }
                        } catch (...) {}
                    });
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Destroy")) {
                    GameThreadWorker::get().enqueue([this, c, owner]() {
                        if (owner != nullptr && this->exists(owner)) {
                            cleanup_references_to((sdk::UObjectBase*)owner);
                            try { owner->destroy_actor(); } catch (...) {}
                        } else if (this->exists(c)) {
                            cleanup_references_to((sdk::UObjectBase*)c);
                            try { c->destroy_component(); } catch (...) {}
                        }
                    });
                    ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();

                // Minimal read-only inspection view, right here in the context menu — no need to open
                // the main page tree for a quick look. Properties grouped by declaring class (matches
                // the main property editor's "Group by class" default); Components lists the owning
                // actor's components (a MenuItem click sends it to "Inspect in main page").
                if (ImGui::BeginMenu("Object")) {
                    if (ImGui::BeginMenu("Properties")) {
                        // Compact "name: value" — scalars only; anything else just shows its type so
                        // the menu stays a quick glance, not a full editor (that's still the main page).
                        auto compact_value = [](void* obj, sdk::FProperty* p) -> std::string {
                            std::string cname;
                            try { cname = utility::narrow(p->get_class()->get_name().to_string()); } catch (...) { return "?"; }
                            try {
                                if (cname == "BoolProperty") return ((sdk::FBoolProperty*)p)->get_value_from_object(obj) ? "true" : "false";
                                if (cname == "FloatProperty") return std::format("{:.3f}", *p->get_data<float>(obj));
                                if (cname == "DoubleProperty") return std::format("{:.3f}", *p->get_data<double>(obj));
                                if (cname == "IntProperty") return std::to_string(*p->get_data<int32_t>(obj));
                                if (cname == "Int64Property") return std::to_string(*p->get_data<int64_t>(obj));
                                if (cname == "UInt32Property") return std::to_string(*p->get_data<uint32_t>(obj));
                                if (cname == "ByteProperty") return std::to_string((int)*p->get_data<uint8_t>(obj));
                                if (cname == "NameProperty") return utility::narrow(p->get_data<sdk::FName>(obj)->to_string());
                                if (cname == "ObjectProperty") return *p->get_data<sdk::UObject*>(obj) != nullptr ? "<object>" : "null";
                            } catch (...) {}
                            return "(" + cname + ")";
                        };
                        // Actor properties when one resolves (matches the header naming above), the
                        // component's own properties otherwise (no owner, e.g. a bare component target).
                        void* prop_obj = owner != nullptr ? (void*)owner : (void*)c;
                        auto* prop_uclass = owner != nullptr ? owner->get_class() : c->get_class();
                        for (auto super = (sdk::UStruct*)prop_uclass; super != nullptr; super = super->get_super_struct()) {
                            std::string cls_name;
                            try { cls_name = utility::narrow(super->get_fname().to_string()); } catch (...) { continue; }
                            if (!ImGui::BeginMenu(cls_name.c_str())) continue;
                            for (auto f = super->get_child_properties(); f != nullptr; f = f->get_next()) {
                                auto* p = reinterpret_cast<sdk::FProperty*>(f);
                                std::string pn;
                                try { pn = utility::narrow(p->get_field_name().to_string()); } catch (...) { continue; }
                                const auto row = pn + ": " + compact_value(prop_obj, p);
                                ImGui::MenuItem(row.c_str(), nullptr, false, false); // display-only row
                            }
                            ImGui::EndMenu();
                        }
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu("Components")) {
                        // get_owner() is a ProcessEvent call, but this only runs once per menu OPEN
                        // (not per-frame), unlike the picker's per-candidate scan — fine here.
                        sdk::AActor* owner = nullptr;
                        try { owner = c->get_owner(); } catch (...) {}
                        if (owner == nullptr) {
                            ImGui::TextDisabled("(no owner)");
                        } else {
                            try {
                                auto comps = owner->get_all_components();
                                std::sort(comps.begin(), comps.end(), [](sdk::UObject* a, sdk::UObject* b) {
                                    std::wstring an, bn;
                                    try { an = a->get_fname().to_string(); } catch (...) {}
                                    try { bn = b->get_fname().to_string(); } catch (...) {}
                                    return an < bn;
                                });
                                for (auto* comp_obj : comps) {
                                    if (comp_obj == nullptr) continue;
                                    std::string comp_name;
                                    try { comp_name = utility::narrow(comp_obj->get_fname().to_string()); } catch (...) { continue; }
                                    if (ImGui::MenuItem(comp_name.c_str()) && comp_obj->is_a(sdk::USceneComponent::static_class())) {
                                        m_last_selected = (sdk::USceneComponent*)comp_obj;
                                    }
                                }
                            } catch (...) {}
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }

                // Spawn a new component of a common type on this component's owner.
                if (ImGui::BeginMenu("Add Component")) {
                    static const char* kCommonComponentTypes[] = {
                        "CapsuleComponent", "SphereComponent", "BoxComponent", "StaticMeshComponent",
                        "SkeletalMeshComponent", "CameraComponent", "PointLightComponent",
                        "SpotLightComponent", "AudioComponent", "SceneComponent",
                        "TextRenderComponent", "ArrowComponent", "BillboardComponent"
                    };
                    for (const char* short_type : kCommonComponentTypes) {
                        if (ImGui::MenuItem(short_type)) {
                            const auto class_path = std::wstring{L"Class /Script/Engine."} + utility::widen(short_type);
                            GameThreadWorker::get().enqueue([this, c, class_path]() {
                                if (!this->exists(c)) return;
                                try {
                                    auto* owner = c->get_owner();
                                    auto* comp_c = sdk::find_uobject<sdk::UClass>(class_path);
                                    if (owner == nullptr || comp_c == nullptr) return;
                                    auto* new_comp = owner->add_component_by_class(comp_c);
                                    if (new_comp != nullptr) {
                                        owner->finish_add_component(new_comp);
                                    }
                                } catch (...) {}
                            });
                        }
                    }
                    ImGui::EndMenu();
                }

                // Attach an EXISTING scene component (picked via the universal picker) as a child of c.
                static std::string s_ctx_attach_filter{};
                if (ImGui::MenuItem("Attach Component...")) {
                    s_ctx_attach_filter.clear();
                    ImGui::OpenPopup("##uobj_sel_ctx_attach_picker");
                }
                if (auto* picked = render_object_picker_popup("##uobj_sel_ctx_attach_picker", s_ctx_attach_filter,
                                                               sdk::USceneComponent::static_class(), false);
                    picked != nullptr) {
                    auto* to_attach = (sdk::USceneComponent*)picked;
                    GameThreadWorker::get().enqueue([this, c, to_attach]() {
                        if (this->exists(c) && this->exists(to_attach)) try { to_attach->attach_to(c, L"None", 0, true); } catch (...) {}
                    });
                }

                // Send this component to the Function Caller's target slot, for a quick function call
                // without dragging it there manually.
                if (ImGui::MenuItem("Select Target (Function Caller)")) {
                    load_live_caller_target(c);
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Inspect in main page")) {
                    m_last_selected = c; // game thread; UI reads it benignly
                }
                if (ImGui::MenuItem("Show")) {
                    GameThreadWorker::get().enqueue([this, c]() { if (this->exists(c)) try { c->set_visibility(true, true); } catch (...) {} });
                }
                if (ImGui::MenuItem("Hide")) {
                    GameThreadWorker::get().enqueue([this, c]() { if (this->exists(c)) try { c->set_visibility(false, true); } catch (...) {} });
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save position")) {
                    GameThreadWorker::get().enqueue([this, c]() { if (this->exists(c)) try { const glm::vec3 l = c->get_world_location(); std::scoped_lock _{m_saved_positions_mtx}; m_saved_positions[c] = l; } catch (...) {} });
                }
                bool has_saved = false;
                { std::scoped_lock _{m_saved_positions_mtx}; has_saved = m_saved_positions.contains(c); }
                if (has_saved && ImGui::MenuItem("Restore position")) {
                    GameThreadWorker::get().enqueue([this, c]() {
                        if (!this->exists(c)) return;
                        glm::vec3 l{};
                        { std::scoped_lock _{m_saved_positions_mtx}; auto it = m_saved_positions.find(c); if (it == m_saved_positions.end()) return; l = it->second; }
                        try { c->set_world_location(l, false, false); } catch (...) {}
                    });
                }
                ImGui::Separator();
                // Attach / detach. Intuitive two-object attach: pick the intended PARENT first (it
                // becomes last-selected), then right-click the CHILD here and "Attach to last-selected".
                if (sdk::USceneComponent* parent = m_last_selected; parent != nullptr && parent != c && this->exists(parent)) {
                    std::string pn;
                    try { pn = utility::narrow(parent->get_fname().to_string()); } catch (...) { pn = "<last-selected>"; }
                    if (ImGui::MenuItem(("Attach to last-selected: " + pn).c_str())) {
                        GameThreadWorker::get().enqueue([this, c, parent]() {
                            if (this->exists(c) && this->exists(parent)) try { c->attach_to(parent, L"None", 0, true); } catch (...) {}
                        });
                    }
                }
                if (ImGui::MenuItem("Detach from parent")) {
                    GameThreadWorker::get().enqueue([this, c]() {
                        if (this->exists(c)) try { c->detach_from_parent(true, true); } catch (...) {}
                    });
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Duplicate (spawn owner class here)")) {
                    GameThreadWorker::get().enqueue([this, c]() {
                        if (!this->exists(c)) return;
                        try {
                            auto* owner = c->get_owner();
                            auto* eng = sdk::UGameEngine::get();
                            auto* world = eng != nullptr ? eng->get_world() : nullptr;
                            auto* ugs = sdk::UGameplayStatics::get();
                            if (owner == nullptr || world == nullptr || ugs == nullptr) return;
                            ugs->spawn_actor(world, owner->get_class(), c->get_world_location());
                        } catch (...) {}
                    });
                }
                ImGui::Separator();
                // Summon to camera: place the object on the camera ray through the screen centre, a fixed
                // distance out (same logic as the panel's "Recenter to camera"). Deferred to the game thread.
                if (ImGui::MenuItem("Summon to camera")) {
                    const auto* vp = ImGui::GetMainViewport();
                    const glm::vec2 screen_center{vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f};
                    const float dist = m_recenter_distance;
                    GameThreadWorker::get().enqueue([this, c, screen_center, dist]() {
                        if (!this->exists(c)) return;
                        try {
                            auto engine = sdk::UGameEngine::get();
                            auto world = engine != nullptr ? engine->get_world() : nullptr;
                            if (world == nullptr) return;
                            auto ugs = sdk::UGameplayStatics::get();
                            if (ugs == nullptr) return;
                            auto pc = ugs->get_player_controller(world, 0);
                            if (pc == nullptr) return;
                            glm::vec3 ray_origin{}, ray_dir{};
                            if (!ugs->screen_to_world(pc, screen_center, &ray_origin, &ray_dir)) return;
                            const float len = glm::length(ray_dir);
                            if (len < 1e-6f) return;
                            ray_dir /= len;
                            c->set_world_location(ray_origin + ray_dir * dist, false, false);
                        } catch (...) {}
                    });
                }
                // Spawn a temporary camera parented to the object and view through it, so it can be
                // inspected from a free vantage. Toggles to "Restore view" while one is active.
                if (m_view_camera_actor == nullptr) {
                    if (ImGui::MenuItem("View from spawned camera")) {
                        spawn_view_camera(c);
                    }
                } else if (ImGui::MenuItem("Restore view (remove spawned camera)")) {
                    restore_view_camera();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Remove from selection")) {
                    bool changed = false;
                    {
                        std::unique_lock _{m_mutex};
                        changed = m_gizmo_components.erase(c) > 0;
                    }
                    if (changed) {
                        dispatch_gizmo_target_event(c, false); // outside the lock — dispatch can re-enter
                    }
                }
            }
            ImGui::EndPopup();
        }
    }

    // Prune components that no longer exist. This mutates m_gizmo_components from the game thread, so
    // take the write lock (UI-thread readers use the shared lock); only when there's something to do.
    if (!dead.empty()) {
        std::unique_lock _{m_mutex};
        for (auto* d : dead) {
            m_gizmo_components.erase(d);
        }
    }
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
    // Every live UObject tracked in m_objects (not just one class' instances like the class
    // inspector's Instances tab). CDOs and Blueprint GEN_VARIABLE default-value holder objects are
    // usually noise here (there's one CDO per class, and GEN_VARIABLE objects are Blueprint-internal
    // template storage) — hidden by default via name-substring, since there's no reflected object-flag
    // accessor exposed to check RF_ClassDefaultObject directly.
    if (ImGui::BeginTabItem("All Objects")) {
        ImGui::Checkbox("Hide default objects (CDOs)", &m_all_objects_hide_default);
        ImGui::SameLine();
        ImGui::Checkbox("Hide GEN_VARIABLE objects", &m_all_objects_hide_gen_variable);

        std::vector<sdk::UObjectBase*> snapshot;
        {
            std::shared_lock _{m_mutex};
            snapshot.reserve(m_objects.size());
            for (auto* obj : m_objects) snapshot.push_back(obj);
        }
        ImGui::TextDisabled("%zu objects total", snapshot.size());

        if (ImGui::BeginChild("all_objects_list", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            drag_scroll_current_window();
            std::shared_lock _{m_mutex};
            int shown = 0;
            int matched = 0;
            const int kCap = 5000;
            for (auto* base_obj : snapshot) {
                if (base_obj == nullptr || !this->exists_unsafe(base_obj)) continue;
                std::wstring full_w;
                auto meta_it = m_meta_objects.find(base_obj);
                if (meta_it != m_meta_objects.end() && meta_it->second != nullptr) {
                    full_w = meta_it->second->full_name;
                } else {
                    try { full_w = ((sdk::UObject*)base_obj)->get_full_name(); } catch (...) { continue; }
                }
                if (m_all_objects_hide_default && full_w.find(L"Default__") != std::wstring::npos) continue;
                if (m_all_objects_hide_gen_variable && full_w.find(L"GEN_VARIABLE") != std::wstring::npos) continue;
                if (has_filter && full_w.find(wfilter) == std::wstring::npos) continue;
                ++matched;
                if (shown >= kCap) continue;
                ++shown;

                auto* obj = (sdk::UObject*)base_obj;
                const std::string name = utility::narrow(full_w);
                ImGui::PushID((void*)obj);
                const bool node_open = ImGui::TreeNode(shorten_object_path(name).c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", name.c_str());
                }
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
            }
            if (matched > kCap) {
                ImGui::TextDisabled("(truncated at %d of %d matches — narrow with the filter)", kCap, matched);
            } else if (shown == 0) {
                ImGui::TextDisabled(has_filter ? "no matches for filter" : "(no live objects tracked)");
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

void UObjectHook::draw_function_caller_window() {
    uobjecthook_dock_into_host_once();
    if (!ImGui::Begin("UEVR Function Hooks", &m_show_function_caller)) {
        ImGui::End();
        return;
    }
    utility::ScopeGuard end_guard{[]() { ImGui::End(); }};

    // Collapsing regions instead of tabs so the caller and the hooks/events
    // monitor can be seen at once.
    if (ImGui::CollapsingHeader("Function Caller")) {
        ImGui::TextDisabled("Drop/type a target + pick a function. Shared with the per-object callers.");
        ImGui::Separator();
        render_live_caller_slots();
    }

    // Active hooks (the flagged set: Block/Monitor) and the ProcessEvent monitor
    // are merged — flagged functions are the same set the PE "Flagged only" mode
    // records, so they belong together.
    if (ImGui::CollapsingHeader("Hooks & Events")) {
        ImGui::SeparatorText("Flagged functions (right-click a function -> Block / Monitor)");
        draw_active_function_hooks();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("ProcessEvent monitor");
        draw_process_event_monitor();
    }
}

void UObjectHook::on_draw_ui() {
    activate();

    if (!m_fully_hooked) {
        ImGui::Text("Waiting for UObjectBase to be hooked...");
        return;
    }

    std::shared_lock _{m_mutex};
    std::scoped_lock __{m_function_mutex};

    if (m_uobject_hook_disabled) {
        ImGui::TextColored(ImVec4{1.0f, 0.0f, 0.0f, 1.0f}, "UObjectHook is disabled");
        if (ImGui::Button("Re-enable")) {
            m_uobject_hook_disabled = false;
        }
        return;
    }

    if (ImGui::Button("Reload Persistent States")) {
        reload_persistent_states();
    }

    ImGui::SameLine();

    if (ImGui::Button("Destroy Persistent States")) {
        reset_persistent_states();

        const auto uobjecthook_dir = get_persistent_dir();

        if (std::filesystem::exists(uobjecthook_dir)) {
            for (const auto& p : std::filesystem::directory_iterator(uobjecthook_dir)) {
                if (p.path().extension() == ".json") {
                    std::filesystem::remove(p.path());
                }
            }
        }
    }
}

void UObjectHook::on_draw_sidebar_entry(std::string_view in_entry) {
    on_draw_ui();
    ImGui::Separator();

    if (in_entry == "Main") {
        draw_main();
    } else if (in_entry == "Config") {
        draw_config();
    } else if (in_entry == "Developer") {
        draw_developer();
    }
}

void UObjectHook::draw_config() {
    m_enabled_at_startup->draw("Enabled at Startup");
    m_attach_lerp_enabled->draw("Enable Attach Lerp");
    m_attach_lerp_speed->draw("Attach Lerp Speed");
    m_keybind_toggle_uobject_hook->draw("Disable UObjectHook Key");

    ImGui::Checkbox("Options as separate dockable window", &m_show_options_window);
    ImGui::Separator();
    draw_gizmo_options();
}

// Shared gizmo/selection/snap/inspector options — drawn in the Config tab AND the pop-out window.
// NOTE: plain members, session-scoped (not yet persisted across restarts).
void UObjectHook::draw_gizmo_options() {
    ImGui::SeparatorText("Gizmo mode");
    ImGui::RadioButton("Move##cfg", &m_gizmo_mode, 0); ImGui::SameLine();
    ImGui::RadioButton("Rotate##cfg", &m_gizmo_mode, 1); ImGui::SameLine();
    ImGui::RadioButton("Scale##cfg", &m_gizmo_mode, 2); ImGui::SameLine();
    ImGui::RadioButton("Combined##cfg", &m_gizmo_mode, 3);
    ImGui::TextDisabled("Combined = one gizmo with all handles: rings rotate, arrows move, tip boxes scale, center dot = uniform scale.");
    // Rebindable hotkeys to switch transform mode (default unbound — assign here).
    m_keybind_gizmo_move->draw("Move hotkey");
    m_keybind_gizmo_rotate->draw("Rotate hotkey");
    m_keybind_gizmo_scale->draw("Scale hotkey");
    m_keybind_gizmo_combined->draw("Combined hotkey");

    ImGui::SeparatorText("Appearance");
    ImGui::SliderFloat("Gizmo thickness", &m_gizmo_thickness, 1.0f, 12.0f, "%.1f px");
    ImGui::SliderFloat("Gizmo axis length", &m_gizmo_axis_len, 5.0f, 1000.0f, "%.0f cm");
    ImGui::SliderFloat("Rotate ring radius", &m_gizmo_ring_radius, 5.0f, 300.0f, "%.0f cm");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Rotate-mode ring size, independent of the axis length above.\nSmaller = tighter rings / better centering (especially in VR).");
    }
    ImGui::Checkbox("Gizmo local space", &m_gizmo_local);
    ImGui::Checkbox("Show gizmo labels", &m_gizmo_show_labels);
    ImGui::Checkbox("Show light icons", &m_show_light_icons);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Overlay icon over every live Point/Spot/Directional light component.");
    }
    ImGui::Checkbox("Hide (don't remove) gizmos when UI closed", &m_hide_gizmos_when_ui_closed);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Stops drawing/hit-testing gizmos while no UObjectHook panel is open.\nTargets stay selected — they reappear as soon as a panel (or the picker) is opened again.");
    }
    ImGui::Checkbox("Block game input passthrough while gizmos visible", &m_block_passthrough_when_gizmos_visible);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While on, the game doesn't see mouse/keyboard input whenever a gizmo is actually drawn\n(even with every UObjectHook panel closed), so clicks/drags reliably hit the gizmo instead of the game.\nCombine with \"Hide gizmos when UI closed\" above to get full passthrough back the moment gizmos hide,\neven if targets are still selected.");
    }
    ImGui::TextDisabled("Tip: pick the Combined gizmo mode above to see move + rotate + scale handles at once.");

    ImGui::SeparatorText("Selection / picking");
    ImGui::Checkbox("Auto-gizmo on MC adjust (VR)", &m_auto_gizmo_on_adjust);
    // One-shot picker: arm with the button, click a world object, it auto-disarms (no toggle-off
    // dance). "keep picking" keeps it armed for picking several in a row. Esc also cancels.
    if (m_click_select_mode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55f, 0.25f, 0.10f, 1.0f});
        if (ImGui::Button("Picking… click a world object (Esc to cancel)")) {
            m_click_select_mode = false;
        }
        ImGui::PopStyleColor();
    } else if (ImGui::Button("Pick gizmo target")) {
        m_click_select_mode = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("keep picking##pick", &m_click_select_sticky); // stay armed after each hit (was "sticky")
    m_keybind_pick->draw("Pick mode hotkey"); // toggle the picker without reaching for the button
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("Pick class filter", "substring of class/full name...", m_pick_class_filter, sizeof(m_pick_class_filter));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While picking: scroll to cycle through overlapping objects under the cursor.\nThis filter restricts candidates by name (case-insensitive).");
    }
    {
        size_t n_targets = 0;
        { std::shared_lock _{m_mutex}; n_targets = m_gizmo_components.size(); }
        ImGui::BeginDisabled(n_targets == 0);
        if (ImGui::Button("Clear gizmo targets")) {
            std::unique_lock _{m_mutex};
            m_gizmo_components.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu active", n_targets);
    }
    ImGui::Checkbox("Single-select (pick replaces)", &m_click_select_single);
    ImGui::SameLine();
    ImGui::Checkbox("Highlight selection", &m_highlight_selection);
    ImGui::Checkbox("Set Movable on select (Mobility=2)", &m_gizmo_set_movable);

    ImGui::SeparatorText("Ctrl-snap steps (hold Ctrl while dragging the gizmo)");
    ImGui::SliderFloat("Move snap (cm)", &m_snap_translate, 0.0f, 100.0f, "%.1f");
    ImGui::SliderFloat("Rotate snap (deg)", &m_snap_rotate, 0.0f, 90.0f, "%.1f");
    ImGui::SliderFloat("Scale snap", &m_snap_scale, 0.0f, 1.0f, "%.2f");

    ImGui::SeparatorText("Inspector");
    ImGui::SliderFloat("Property column width", &m_inspector_item_width, 0.0f, 900.0f,
                       m_inspector_item_width <= 0.0f ? "unlimited" : "%.0f px");
    if (g_framework->get_renderer_type() == Framework::RendererType::D3D11) {
        ImGui::Checkbox("Texture previews (D3D11, experimental)", &m_show_texture_previews);
    }
}

// Dockable pop-out of the options — drag its title bar onto the main UObjectHook window to dock it
// next to / tabbed with it (ImGui docking). Same auto-dock-into-host pattern as the other windows.
void UObjectHook::draw_options_window() {
    uobjecthook_dock_into_host_once();
    if (ImGui::Begin("UEVR Selection / Gizmo Options", &m_show_options_window)) {
        draw_gizmo_options();
    }
    ImGui::End();
}

void UObjectHook::draw_developer() {
    if (ImGui::Button("Dump SDK")) {
        SDKDumper::dump();
    }
    ImGui::Text("Constructor calls: %llu", m_debug.constructor_calls);
    ImGui::Text("Destructor calls: %llu", m_debug.destructor_calls);

    ImGui::Separator();
    draw_process_event_monitor();

    ImGui::Separator();

    static std::array<char, 512> address_buffer{};
    ImGui::InputText("Address Lookup", address_buffer.data(), address_buffer.size());

    // Try-catch block around this because it's possible the user could enter invalid input
    // also hex->int conversion can throw
    try {
        auto obj = (sdk::UObject*)std::stoull(address_buffer.data(), nullptr, 16);

        if (obj != nullptr && this->exists(obj)) {
            ImGui::PushID(obj);
            if (ImGui::TreeNode(utility::narrow(obj->get_full_name()).c_str())) {
                ui_handle_object(obj);
                ImGui::TreePop();
            }

            ImGui::PopID();
        }
    } catch (...) {
        // ignore
    }
}

void UObjectHook::draw_process_event_monitor() {
    if (!m_attempted_hook_process_event) {
        if (ImGui::Button("Create ProcessEvent hook")) {
            GameThreadWorker::get().enqueue([this]() {
                hook_process_event();
            });
        }
    } else if (m_hooked_process_event) {
        ImGui::Checkbox("ProcessEvent Listener", &m_process_event_listening);
        ImGui::SameLine();
        ImGui::Checkbox("Flagged only", &m_process_event_flagged_only);
        if (m_process_event_flagged_only) {
            ImGui::TextDisabled("Recording only functions flagged via Monitor calls (right-click a function).");
        }

        if (m_process_event_listening) {

            if (ImGui::Button("Clear Ignored Functions")) {
                m_ignored_recent_functions.clear();
            }

            ImGui::SameLine();

            std::scoped_lock __{m_function_mutex};

            if (ImGui::Button("Clear Called Functions")) {
                m_called_functions.clear();
                m_most_recent_functions.clear();
            }

            if (ImGui::Button("Ignore All Called Functions")) {
                m_ignored_recent_functions.clear();

                for (auto& [ufunc, data] : m_called_functions) {
                    m_ignored_recent_functions.insert(ufunc);
                }
            }

            ImGui::Text("Called functions: %llu", m_called_functions.size());

            std::vector<sdk::UFunction*> functions_to_cleanup{};

            if (ImGui::TreeNodeEx("Recent Functions",ImGuiTreeNodeFlags_DrawLinesToNodes)) {
                for (auto ufunc : m_most_recent_functions) {
                    if (ufunc == nullptr) {
                        continue;
                    }

                    if (m_ignored_recent_functions.contains(ufunc)) {
                        continue;
                    }

                    if (!this->exists(ufunc)) {
                        functions_to_cleanup.push_back(ufunc);
                        continue;
                    }

                    ImGui::PushID(ufunc);

                    utility::ScopeGuard ___{[]() {
                        ImGui::PopID();
                    }};

                    if (ImGui::Button("Ignore")) {
                        m_ignored_recent_functions.insert(ufunc);
                    }

                    ImGui::SameLine();

                    ImGui::Text("%s", utility::narrow(ufunc->get_full_name()).c_str());
                    ui_function_context_menu(ufunc, nullptr, false);
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("All Called Functions")) {
                ImGui::SliderInt("Max Calls", &m_process_event_search.max_calls, 0, 10000);
                ImGui::InputText("Search", m_process_event_search.buffer.data(), m_process_event_search.buffer.size());

                // "By class" groups by the RECEIVER's class (calls aggregated across every instance of
                // that class); "By caller" groups by the specific receiver INSTANCE. Both are populated
                // in process_event_hook alongside call_count (caller_class_counts/caller_instance_counts),
                // so grouping needs no extra scan of call history.
                static int s_pe_group_mode = 0; // 0 = flat, 1 = by class, 2 = by caller
                ImGui::SetNextItemWidth(160.0f);
                ImGui::Combo("group##pe_group", &s_pe_group_mode, "Flat\0By class\0By caller\0");

                std::string_view search{m_process_event_search.buffer.data()};
                std::vector<sdk::UFunction*> functions_sorted_by_call_count{};

                for (auto& [ufunc, data] : m_called_functions) {
                    if (ufunc == nullptr) {
                        continue;
                    }

                    if (!this->exists(ufunc)) {
                        functions_to_cleanup.push_back(ufunc);
                        continue;
                    }

                    if (m_process_event_search.max_calls > 0 && data.call_count > m_process_event_search.max_calls) {
                        continue;
                    }

                    // maybe a TODO here for optimization
                    if (!search.empty() && !utility::narrow(ufunc->get_full_name()).contains(search)) {
                        continue;
                    }

                    functions_sorted_by_call_count.push_back(ufunc);
                }

                std::sort(functions_sorted_by_call_count.begin(), functions_sorted_by_call_count.end(), [this](sdk::UFunction* a, sdk::UFunction* b) {
                    return m_called_functions[a].call_count > m_called_functions[b].call_count;
                });

                // Shared per-function row (Ignore button + expandable name + count + param inspector).
                // `display_count` is the function's GLOBAL call_count in flat mode, or the group-specific
                // tally (how many times THIS class/caller called it) when grouped.
                auto render_function_row = [this](sdk::UFunction* ufunc, size_t display_count) {
                    ImGui::PushID(ufunc);
                    utility::ScopeGuard ___{[]() { ImGui::PopID(); }};

                    if (ImGui::Button("Ignore")) {
                        m_ignored_recent_functions.insert(ufunc);
                    }
                    ImGui::SameLine();
                    const auto made = ImGui::TreeNode(utility::narrow(ufunc->get_full_name()).c_str());
                    ui_function_context_menu(ufunc, nullptr, false);
                    ImGui::SameLine();
                    ImGui::Text(" (%llu)", (unsigned long long)display_count);
                    if (is_func_blocked(ufunc)) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4{1.0f, 0.5f, 0.0f, 1.0f}, "[Blocked]");
                    }
                    if (is_func_monitored(ufunc)) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "[Mon]");
                    }

                    if (made) {
                        auto& data = m_called_functions[ufunc];
                        data.wants_heavy_data = true;
                        if (data.heavy_data != nullptr) {
                            ui_handle_struct(data.heavy_data->params.data(), ufunc); // param inspector
                        }
                        ImGui::TreePop();
                    }
                };

                if (s_pe_group_mode == 0) {
                    for (auto& ufunc : functions_sorted_by_call_count) {
                        if (m_ignored_recent_functions.contains(ufunc)) continue;
                        if (!this->exists(ufunc)) { functions_to_cleanup.push_back(ufunc); continue; }
                        render_function_row(ufunc, m_called_functions[ufunc].call_count);
                    }
                } else {
                    // Build class/caller -> [(function, group-specific count)] from each function's own
                    // tally map, then render one CollapsingHeader per group (busiest group first).
                    struct GroupEntry { sdk::UFunction* func; size_t count; };
                    struct Group { std::string label; void* key{nullptr}; std::vector<GroupEntry> entries; size_t total{0}; };
                    std::unordered_map<void*, Group> groups;

                    for (auto& ufunc : functions_sorted_by_call_count) {
                        if (m_ignored_recent_functions.contains(ufunc)) continue;
                        if (!this->exists(ufunc)) { functions_to_cleanup.push_back(ufunc); continue; }
                        auto& data = m_called_functions[ufunc];

                        if (s_pe_group_mode == 1) {
                            for (auto& [cls, count] : data.caller_class_counts) {
                                if (cls == nullptr || count == 0) continue;
                                auto& g = groups[(void*)cls];
                                if (g.entries.empty() && g.key == nullptr) {
                                    g.key = (void*)cls;
                                    try { g.label = utility::narrow(cls->get_fname().to_string()); } catch (...) { g.label = "<class>"; }
                                }
                                g.entries.push_back({ufunc, count});
                                g.total += count;
                            }
                        } else { // by caller (instance)
                            for (auto& [obj, count] : data.caller_instance_counts) {
                                if (obj == nullptr || count == 0 || !this->exists(obj)) continue;
                                auto& g = groups[(void*)obj];
                                if (g.entries.empty() && g.key == nullptr) {
                                    g.key = (void*)obj;
                                    // Short label (shorten_object_path) — this list can get long with
                                    // many instances; the full path is still one click away (TreeNode).
                                    std::string full;
                                    try { full = utility::narrow(obj->get_full_name()); } catch (...) { full = "<object>"; }
                                    g.label = shorten_object_path(full);
                                }
                                g.entries.push_back({ufunc, count});
                                g.total += count;
                            }
                        }
                    }

                    std::vector<Group*> sorted_groups;
                    sorted_groups.reserve(groups.size());
                    for (auto& [key, g] : groups) sorted_groups.push_back(&g);
                    std::sort(sorted_groups.begin(), sorted_groups.end(), [](const Group* a, const Group* b) { return a->total > b->total; });

                    for (auto* g : sorted_groups) {
                        std::sort(g->entries.begin(), g->entries.end(), [](const GroupEntry& a, const GroupEntry& b) { return a.count > b.count; });
                        ImGui::PushID(g->key);
                        utility::ScopeGuard grp_guard{[]() { ImGui::PopID(); }};
                        if (ImGui::TreeNode((g->label + std::format(" ({} calls, {} functions)", g->total, g->entries.size())).c_str())) {
                            for (auto& e : g->entries) {
                                render_function_row(e.func, e.count);
                            }
                            ImGui::TreePop();
                        }
                    }
                }

                ImGui::TreePop();
            }

            if (!functions_to_cleanup.empty()) {
                spdlog::info("[UObjectHook] Cleaning up {} functions", functions_to_cleanup.size());

                for (auto& ufunc : functions_to_cleanup) {
                    std::erase_if(m_most_recent_functions, [ufunc](auto& it) {
                        return it == ufunc;
                    });

                    m_called_functions.erase(ufunc);
                }
            }
        }
    } else {
        ImGui::Text("Failed to hook ProcessEvent!");
    }
}

// Standalone pop-out of draw_main() — same dockable-window pattern as the Class Browser / Function
// Hooks windows, reachable via its own checkbox/hotkey (F3) so the "Main" page content (selected-
// object inspector, attached components, spawn actor, ...) doesn't require navigating the sidebar.
void UObjectHook::draw_main_window() {
    uobjecthook_dock_into_host_once();
    if (!ImGui::Begin("UEVR Object Hook", &m_show_main_window)) {
        ImGui::End();
        return;
    }
    utility::ScopeGuard end_guard{[]() { ImGui::End(); }};
    draw_main();
}

void UObjectHook::draw_main() {
    // Live Function Caller — pinned workbench-style widget that sits ABOVE the
    // deep object tree so the user does not have to drill through
    // Objects-by-class → SomeUClass → SomeObject → Functions → fn each time
    // they want to invoke a function. Each slot accepts a drag-and-dropped
    // UObject from anywhere in the tree, lets the user type a function name,
    // resolves the UFunction on demand, and then reuses the same per-property
    // editor + Call button as the in-tree caller (via render_function_call).
    //
    // State for each slot lives in a function-local static. We deliberately
    // give it a small fixed size (kSlotCount) instead of an unbounded vector,
    // so the widget is bounded and the user does not have to manually add /
    // remove rows.
    // Toggle buttons for the dockable pop-out windows. Both can be open
    // simultaneously and dock anywhere via the host dockspace.
    ImGui::SeparatorText("Windows");
    ImGui::Checkbox("Class Browser window", &m_show_class_browser);
    ImGui::SameLine();
    ImGui::Checkbox("Function Hooks window", &m_show_function_caller);
    ImGui::SameLine();
    ImGui::Checkbox("Options window", &m_show_options_window);
    ImGui::Checkbox("Standalone Main window (F3)", &m_show_main_window);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pop this page (selected-object inspector, attached components, spawn actor, ...)\nout into its own dockable window, summonable without opening the main UEVR overlay.");
    }

    // Spawners folded into one collapsible subpanel (Spawn Actor + Spawn Overlapper/Overlapped
    // Objects) so the main page isn't dominated by them when you're not actively spawning things.
    ImGui::SeparatorText("Spawners");
    if (ImGui::TreeNode("Spawners##panel")) {
    // Basic spawn-actor: type a full class path (Class /Script/Engine.X) or use one of the common-type
    // quick-spawn buttons. Both place the new actor along the camera's look-at ray, m_recenter_distance
    // out — same placement math as the w2s context menu's "Summon to camera" / "Recenter to camera".
    ImGui::SeparatorText("Spawn Actor");
    {
        // Applies "Name=Value" lines (one property per line) to a freshly-spawned object. Covers the
        // common scalar property types via the same class-name dispatch the context-menu's read-only
        // compact_value uses; anything else (structs, arrays, objects, ...) is skipped rather than
        // guessed at. Runs on the game thread (called from inside the spawn's enqueue).
        auto apply_default_properties = [](sdk::UObject* obj, const std::string& text) {
            if (obj == nullptr || text.empty()) return;
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line)) {
                const size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string name = line.substr(0, eq);
                std::string value = line.substr(eq + 1);
                const auto trim = [](std::string& s) {
                    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
                    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
                };
                trim(name);
                trim(value);
                if (name.empty()) continue;

                auto* cls = obj->get_class();
                auto* prop = cls != nullptr ? cls->find_property(utility::widen(name)) : nullptr;
                if (prop == nullptr) {
                    SPDLOG_WARN("[UObjectHook] Spawn default properties: property '{}' not found", name);
                    continue;
                }
                std::string pcls;
                try { pcls = utility::narrow(prop->get_class()->get_name().to_string()); } catch (...) { continue; }
                try {
                    if (pcls == "BoolProperty") {
                        const bool b = value == "true" || value == "1" || value == "True";
                        ((sdk::FBoolProperty*)prop)->set_value_in_object(obj, b);
                    } else if (pcls == "FloatProperty") {
                        *prop->get_data<float>(obj) = std::stof(value);
                    } else if (pcls == "DoubleProperty") {
                        *prop->get_data<double>(obj) = std::stod(value);
                    } else if (pcls == "IntProperty") {
                        *prop->get_data<int32_t>(obj) = std::stoi(value);
                    } else if (pcls == "Int64Property") {
                        *prop->get_data<int64_t>(obj) = std::stoll(value);
                    } else if (pcls == "UInt32Property") {
                        *prop->get_data<uint32_t>(obj) = (uint32_t)std::stoul(value);
                    } else if (pcls == "ByteProperty") {
                        *prop->get_data<uint8_t>(obj) = (uint8_t)std::stoi(value);
                    } else if (pcls == "NameProperty") {
                        *prop->get_data<sdk::FName>(obj) = sdk::FName{utility::widen(value)};
                    } else if (pcls == "StrProperty") {
                        // Deliberately unsupported: StrProperty's backing TArray<wchar_t> needs
                        // FMalloc-aware realloc bookkeeping to touch safely (see the property-editor's
                        // StrProperty case above this function) — a naive assignment here risks the
                        // exact "unrecognized block" heap corruption that code works around.
                        SPDLOG_WARN("[UObjectHook] Spawn default properties: '{}' is a StrProperty (not supported, skipped)", name);
                    } else {
                        SPDLOG_WARN("[UObjectHook] Spawn default properties: '{}' is a {} (unsupported type, skipped)", name, pcls);
                    }
                } catch (const std::exception& e) {
                    SPDLOG_WARN("[UObjectHook] Spawn default properties: failed to set '{}': {}", name, e.what());
                } catch (...) {}
            }
        };

        static char s_spawn_actor_class[256]{};
        static char s_default_props_buf[1024]{};
        static bool s_attach_gizmo_on_spawn = false;
        static bool s_attach_mc_on_spawn = false;
        static std::string s_default_props_loaded_for; // which class key s_default_props_buf currently reflects

        // Default properties are keyed per-class (m_spawn_default_props_by_class), so switching between
        // a couple of classes you spawn repeatedly doesn't require re-typing each time. Reload the
        // buffer whenever the typed/picked class changes.
        if (s_default_props_loaded_for != s_spawn_actor_class) {
            s_default_props_loaded_for = s_spawn_actor_class;
            auto it = m_spawn_default_props_by_class.find(s_spawn_actor_class);
            if (it != m_spawn_default_props_by_class.end()) {
                strncpy_s(s_default_props_buf, it->second.c_str(), sizeof(s_default_props_buf) - 1);
            } else {
                s_default_props_buf[0] = '\0';
            }
        }

        // Class picker: filtered list of AActor-derived classes from the same m_sorted_classes the
        // class browser uses, so spawning doesn't require knowing/typing the full class path.
        if (ImGui::Button("Browse classes...")) {
            ImGui::OpenPopup("##spawn_class_picker");
        }
        if (ImGui::BeginPopup("##spawn_class_picker")) {
            static char s_class_picker_filter[128]{};
            ImGui::SetNextItemWidth(300.0f);
            ImGui::InputTextWithHint("##spawn_class_filter", "filter...", s_class_picker_filter, sizeof(s_class_picker_filter));
            if (ImGui::BeginChild("spawn_class_list", ImVec2(400.0f, 300.0f), ImGuiChildFlags_Borders)) {
                drag_scroll_current_window();
                static const auto actor_t = sdk::AActor::static_class();
                std::string filter_lower = s_class_picker_filter;
                for (auto& ch : filter_lower) ch = (char)std::tolower((unsigned char)ch);
                std::shared_lock _{m_mutex};
                for (auto* uclass : m_sorted_classes) {
                    if (uclass == nullptr || actor_t == nullptr || !uclass->is_a(actor_t)) continue;
                    auto it = m_meta_objects.find(uclass);
                    if (it == m_meta_objects.end() || it->second == nullptr) continue;
                    const std::string full = utility::narrow(it->second->full_name);
                    std::string full_lower = full;
                    for (auto& ch : full_lower) ch = (char)std::tolower((unsigned char)ch);
                    if (!filter_lower.empty() && full_lower.find(filter_lower) == std::string::npos) continue;
                    const std::string display = shorten_object_path(full);
                    ImGui::PushID(uclass);
                    if (ImGui::Selectable(display.c_str())) {
                        // full = "Class /Script/Engine.StaticMeshActor" — strip the leading "Class " so
                        // it matches the same format s_spawn_actor_class already expects/finds.
                        const size_t sp = full.find(' ');
                        const std::string path = sp == std::string::npos ? full : full.substr(sp + 1);
                        strncpy_s(s_spawn_actor_class, path.c_str(), sizeof(s_spawn_actor_class) - 1);
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", full.c_str());
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }

        if (ImGui::TreeNode("Default properties / attach-on-spawn")) {
            ImGui::TextDisabled("Applies to: %s", s_spawn_actor_class[0] != '\0' ? s_spawn_actor_class : "(no class set)");
            ImGui::TextDisabled("One \"Name=Value\" per line — bool/int/float/name properties only.");
            if (ImGui::InputTextMultiline("##default_props", s_default_props_buf, sizeof(s_default_props_buf), ImVec2(-FLT_MIN, 60.0f))) {
                m_spawn_default_props_by_class[s_spawn_actor_class] = s_default_props_buf;
            }
            ImGui::Checkbox("Attach gizmo on spawn", &s_attach_gizmo_on_spawn);
            ImGui::SameLine();
            ImGui::Checkbox("Attach motion controller on spawn", &s_attach_mc_on_spawn);
            ImGui::TextDisabled("(overlapper-sphere-on-spawn is planned — not implemented yet)");
            ImGui::TreePop();
        }

        // Compute the screen-center ray HERE (draw thread — ImGui state isn't safe to touch from a
        // GameThreadWorker lambda) and defer the actual UGameplayStatics::SpawnActor + screen_to_world
        // ProcessEvent calls to the game thread, same split every other engine-touching action here uses.
        auto spawn_at_look_at = [this, apply_default_properties](const std::wstring& class_path) {
            const auto* vp = ImGui::GetMainViewport();
            const glm::vec2 screen_center{vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f};
            const float dist = m_recenter_distance;
            const std::string default_props = s_default_props_buf;
            const bool attach_gizmo = s_attach_gizmo_on_spawn;
            const bool attach_mc = s_attach_mc_on_spawn;
            GameThreadWorker::get().enqueue([this, class_path, screen_center, dist, apply_default_properties, default_props, attach_gizmo, attach_mc]() {
                try {
                    auto* cls = sdk::find_uobject<sdk::UClass>(class_path);
                    auto* engine = sdk::UGameEngine::get();
                    auto* world = engine != nullptr ? engine->get_world() : nullptr;
                    auto* ugs = sdk::UGameplayStatics::get();
                    if (cls == nullptr || world == nullptr || ugs == nullptr) return;
                    glm::vec3 spawn_pos{0.0f, 0.0f, 0.0f};
                    if (auto* pc = ugs->get_player_controller(world, 0); pc != nullptr) {
                        glm::vec3 ray_origin{}, ray_dir{};
                        if (ugs->screen_to_world(pc, screen_center, &ray_origin, &ray_dir)) {
                            const float len = glm::length(ray_dir);
                            if (len > 1e-6f) {
                                spawn_pos = ray_origin + (ray_dir / len) * dist;
                            }
                        }
                    }
                    // Spawned via UGameplayStatics::SpawnActor -- the actor is added to the level the
                    // normal engine way, not patched into level data directly -- so gizmo/MC/list
                    // tracking below attaches to a real, fully-initialized actor like any other.
                    auto* spawned = ugs->spawn_actor(world, cls, spawn_pos);
                    if (spawned == nullptr) {
                        SPDLOG_WARN("[UObjectHook] Spawn Actor failed for {}", utility::narrow(class_path));
                        return;
                    }

                    apply_default_properties((sdk::UObject*)spawned, default_props);

                    auto* root = spawned->get_root_component();
                    {
                        std::unique_lock _{m_mutex};
                        m_spawned_via_panel.push_back((sdk::UObjectBase*)spawned);
                        if (root != nullptr) {
                            if (attach_gizmo) {
                                m_gizmo_components.insert(root);
                            }
                            if (attach_mc && !m_motion_controller_attached_components.contains(root)) {
                                m_motion_controller_attached_components[root] = std::make_shared<MotionControllerState>();
                            }
                        }
                    }
                } catch (...) {}
            });
        };

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##spawn_actor_class",
                "Class /Script/Engine.StaticMeshActor  (Enter to spawn at the camera's look-at point)",
                s_spawn_actor_class, sizeof(s_spawn_actor_class), ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (sdk::find_uobject<sdk::UClass>(utility::widen(s_spawn_actor_class)) == nullptr) {
                strcpy_s(s_spawn_actor_class, "(class not found)");
            } else {
                spawn_at_look_at(utility::widen(s_spawn_actor_class));
            }
        }

        // Quick-spawn: common actor types, same look-at placement, no path-typing required.
        static const char* kCommonActorTypes[] = {
            "StaticMeshActor", "PointLight", "SpotLight", "DirectionalLight", "CameraActor",
            "TriggerBox", "TriggerSphere", "TargetPoint", "AudioVolume"
        };
        for (size_t i = 0; i < IM_ARRAYSIZE(kCommonActorTypes); ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::SmallButton(kCommonActorTypes[i])) {
                spawn_at_look_at(std::wstring{L"Class /Script/Engine."} + utility::widen(kCommonActorTypes[i]));
            }
        }

        // Foldout list of everything spawned through this panel this session — quick recall without
        // hunting through "Recent Objects" / "All Objects".
        std::vector<sdk::UObjectBase*> spawned_snapshot;
        {
            std::shared_lock _{m_mutex};
            spawned_snapshot = m_spawned_via_panel;
        }
        if (!spawned_snapshot.empty() && ImGui::TreeNode("Spawned Objects")) {
            ImGui::TextDisabled("%zu spawned this session", spawned_snapshot.size());
            if (ImGui::SmallButton("Clear list (does not destroy)")) {
                std::unique_lock _{m_mutex};
                m_spawned_via_panel.clear();
            }
            for (auto* base_obj : spawned_snapshot) {
                if (base_obj == nullptr || !this->exists_unsafe(base_obj)) continue;
                auto* obj = (sdk::UObject*)base_obj;
                std::string name;
                try { name = shorten_object_path(utility::narrow(obj->get_full_name())); } catch (...) { continue; }
                ImGui::PushID((void*)obj);
                if (ImGui::SmallButton("Destroy")) {
                    GameThreadWorker::get().enqueue([this, base_obj]() {
                        if (!this->exists_unsafe(base_obj)) return;
                        cleanup_references_to(base_obj);
                        auto* o = (sdk::UObject*)base_obj;
                        try {
                            if (o->is_a(sdk::AActor::static_class())) {
                                ((sdk::AActor*)o)->destroy_actor();
                            } else if (o->is_a(sdk::UActorComponent::static_class())) {
                                ((sdk::UActorComponent*)o)->destroy_component();
                            }
                        } catch (...) {}
                    });
                    ImGui::PopID();
                    continue;
                }
                ImGui::SameLine();
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
            }
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Spawn Overlapper");
    if (m_overlap_detection_actor == nullptr) {
        if (ImGui::Button("Spawn Overlapper")) {
            spawn_overlapper(0);
            spawn_overlapper(1);
        }
    } else if (!this->exists_unsafe(m_overlap_detection_actor)) {
        m_overlap_detection_actor = nullptr;
    } else {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);

        if (ImGui::TreeNode("Overlapped Objects")) {
            if (ImGui::Button("Destroy Overlapper")) {
                destroy_overlapper();
            }

            ImGui::SameLine();
            bool attach_all = false;
            if (ImGui::Button("Attach all")) {
                attach_all = true;
            }

            auto overlapped_components = m_overlap_detection_actor->get_overlapping_components();

            for (auto& it : overlapped_components) {
                auto comp = (sdk::USceneComponent*)it;
                if (!this->exists_unsafe(comp)) {
                    continue;
                }

                if (m_spawned_spheres.contains(comp) && m_spawned_spheres_to_components.contains(comp)) {
                    comp = m_spawned_spheres_to_components[comp];
                }

                if (attach_all){
                    if (!m_motion_controller_attached_components.contains(comp)) {
                        m_motion_controller_attached_components[comp] = std::make_shared<MotionControllerState>();
                    }
                }

                std::wstring comp_name = comp->get_class()->get_fname().to_string() + L" " + comp->get_fname().to_string();

                const std::string narrow_comp_name = utility::narrow(comp_name);
                if (ImGui::TreeNode(narrow_comp_name.data())) {
                    make_drag_source_for_object(comp, narrow_comp_name.c_str());
                    ui_handle_object(comp);
                    ImGui::TreePop();
                } else {
                    make_drag_source_for_object(comp, narrow_comp_name.c_str());
                }
            }

            ImGui::TreePop();
        }
    }

    ImGui::TreePop(); // Spawners##panel
    }

    // Labelled section header so the user-curated objects below (selected pick, MC-attached
    // components, attached camera, overlapped) are visually distinct from the "Browse all
    // objects" trees further down.
    ImGui::SeparatorText("Selected / attached objects");

    // Most-recently click-selected object, pinned at the top so you can edit what you just picked
    // without drilling the tree. Plain read of m_last_selected (aligned-pointer, benign across
    // threads) validated via exists() before use; we never write it here.
    if (sdk::USceneComponent* sel = m_last_selected; sel != nullptr && this->exists(sel)) {
        std::string sel_name;
        try { sel_name = utility::narrow(sel->get_class()->get_fname().to_string() + L" " + sel->get_fname().to_string()); }
        catch (...) { sel_name = "<selected>"; }
        if (ImGui::CollapsingHeader((std::string("Selected: ") + sel_name + "###lastsel").c_str())) {
            ImGui::PushID("lastsel");

            // Save / restore world position. The actual get/set_world_location call into the engine
            // (process_event), so defer to the game thread; the saved-positions map has its own mutex.
            if (ImGui::Button("Save position")) {
                GameThreadWorker::get().enqueue([this, sel]() {
                    if (!this->exists(sel)) return;
                    try {
                        const glm::vec3 loc = sel->get_world_location();
                        std::scoped_lock _{m_saved_positions_mtx};
                        m_saved_positions[sel] = loc;
                    } catch (...) {}
                });
            }
            ImGui::SameLine();
            bool has_saved = false;
            { std::scoped_lock _{m_saved_positions_mtx}; has_saved = m_saved_positions.contains(sel); }
            ImGui::BeginDisabled(!has_saved);
            if (ImGui::Button("Restore position")) {
                GameThreadWorker::get().enqueue([this, sel]() {
                    if (!this->exists(sel)) return;
                    glm::vec3 loc{};
                    {
                        std::scoped_lock _{m_saved_positions_mtx};
                        auto it = m_saved_positions.find(sel);
                        if (it == m_saved_positions.end()) return;
                        loc = it->second;
                    }
                    try { sel->set_world_location(loc, false, false); } catch (...) {}
                });
            }
            ImGui::EndDisabled();

            // D4: drop the object in front of the camera. Deproject the screen center to a camera
            // ray (origin ~= camera POV, dir ~= forward) and place the object origin + forward*dist.
            // screen_to_world / set_world_location are process_event calls, so run on the game thread;
            // capture the viewport center now (ImGui must not be touched off the UI thread).
            ImGui::SameLine();
            if (ImGui::Button("Recenter to camera")) {
                const auto* vp = ImGui::GetMainViewport();
                const glm::vec2 screen_center{vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f};
                const float dist = m_recenter_distance;
                GameThreadWorker::get().enqueue([this, sel, screen_center, dist]() {
                    if (!this->exists(sel)) return;
                    try {
                        auto engine = sdk::UGameEngine::get();
                        auto world = engine != nullptr ? engine->get_world() : nullptr;
                        if (world == nullptr) return;
                        auto ugs = sdk::UGameplayStatics::get();
                        if (ugs == nullptr) return;
                        auto pc = ugs->get_player_controller(world, 0);
                        if (pc == nullptr) return;
                        glm::vec3 ray_origin{0.0f, 0.0f, 0.0f};
                        glm::vec3 ray_dir{0.0f, 0.0f, 0.0f};
                        if (!ugs->screen_to_world(pc, screen_center, &ray_origin, &ray_dir)) return;
                        const float len = glm::length(ray_dir);
                        if (len < 1e-6f) return;
                        ray_dir /= len;
                        sel->set_world_location(ray_origin + ray_dir * dist, false, false);
                    } catch (...) {}
                });
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::DragFloat("dist (cm)##recenter", &m_recenter_distance, 1.0f, 10.0f, 5000.0f, "%.0f");
            ImGui::Separator();

            try { ui_handle_object((sdk::UObject*)sel); }
            catch (const std::exception& e) { ImGui::TextColored(ImVec4{1,0.3f,0.3f,1}, "selected inspector threw: %s", e.what()); }
            catch (...) { ImGui::TextColored(ImVec4{1,0.3f,0.3f,1}, "selected inspector threw"); }
            ImGui::PopID();
        }
        ImGui::Separator();
    }

    // Snapshot the attached-components map under the shared lock before reading it. draw_main runs
    // with NO m_mutex held (on_draw_sidebar_entry calls on_draw_ui(), which takes AND releases the
    // shared lock, then calls draw_main), so reading/copying this map directly would race the VR tick
    // thread that mutates it under m_mutex — a map copy concurrent with a rehash is UB. Iterate the
    // snapshot.
    decltype(m_motion_controller_attached_components) attached;
    {
        std::shared_lock _{m_mutex};
        attached = m_motion_controller_attached_components;
    }

    if (!attached.empty()) {

        if (ImGui::TreeNode("Attached Components")) {
            if (ImGui::Button("Detach all")) {
                // Defer the clears to the game thread under the UNIQUE lock. draw_main runs with no
                // lock held, so clearing these m_mutex-protected containers here directly would race
                // the VR tick thread. The enqueued task runs with no outer lock, so it takes the
                // unique lock cleanly (no deadlock).
                GameThreadWorker::get().enqueue([this]() {
                    std::unique_lock _{m_mutex};
                    m_motion_controller_attached_components.clear();

                    for (auto persistent_state : m_persistent_states) {
                        if (persistent_state != nullptr) {
                            persistent_state->erase_json_file();
                        }
                    }

                    m_persistent_states.clear();
                });
            }

            for (auto& it : attached) {
                if (!this->exists_unsafe(it.first) || it.second == nullptr) {
                    continue;
                }

                auto comp = it.first;
                std::wstring comp_name = comp->get_class()->get_fname().to_string() + L" " + comp->get_fname().to_string();

                ImGui::PushID(comp);
                const std::string drag_label = utility::narrow(comp_name);
                if (ImGui::TreeNode(drag_label.data())) {
                    make_drag_source_for_object(comp, drag_label.c_str());
                    ui_handle_object(comp);

                    ImGui::TreePop();
                } else {
                    make_drag_source_for_object(comp, drag_label.c_str());
                }
                ImGui::PopID();
            }

            ImGui::TreePop();
        }
    }

    if (m_camera_attach.object != nullptr) {
       if(ImGui::TreeNode("Attached Camera Object")) {
            if (ImGui::Button("Detach Camera")) {
                m_camera_attach.object = nullptr;
                m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};

                if (m_persistent_camera_state != nullptr) {
                    m_persistent_camera_state->erase_json_file();
                }

                m_persistent_camera_state.reset();
            }

            ui_handle_object(m_camera_attach.object);

            ImGui::TreePop();
        }
    }

    // Quick gizmo-target access without opening the Options window: arm the click-select picker, or
    // wipe every current target. Mirrors the same m_click_select_mode / m_gizmo_components the
    // "Selection / picking" section in draw_gizmo_options() drives.
    if (m_click_select_mode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55f, 0.25f, 0.10f, 1.0f});
        if (ImGui::Button("Picking… click a world object (Esc to cancel)")) {
            m_click_select_mode = false;
        }
        ImGui::PopStyleColor();
    } else if (ImGui::Button("Pick a target")) {
        m_click_select_mode = true;
    }
    ImGui::SameLine();
    {
        size_t n_targets = 0;
        { std::shared_lock _{m_mutex}; n_targets = m_gizmo_components.size(); }
        ImGui::BeginDisabled(n_targets == 0);
        if (ImGui::Button("Clear all targets")) {
            std::unique_lock _{m_mutex};
            m_gizmo_components.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu active", n_targets);
    }

    ImGui::SeparatorText("Browse all objects");
    ImGui::Text("Objects: %zu (%zu actual)", m_objects.size(), sdk::FUObjectArray::get()->get_object_count());
    static auto m_ObjectsByClass = false;

    if (ImGui::TreeNode("Recent Objects")) {
        for (auto& object : m_most_recent_objects) {
            if (!this->exists_unsafe(object)) {
                continue;
            }

            const auto obj_label = utility::narrow(object->get_full_name());
            if (ImGui::TreeNode(obj_label.data())) {
                make_drag_source_for_object(object, obj_label.c_str());
                ui_handle_object(object);
                ImGui::TreePop();
            } else {
                make_drag_source_for_object(object, obj_label.c_str());
            }
        }

        ImGui::TreePop();
    }
    // Display common objects like things related to the player
    if (ImGui::TreeNode("Common Objects")) {
        auto engine = sdk::UGameEngine::get();
        auto world = engine != nullptr ? engine->get_world() : nullptr;
        static sdk::UObject* mesh;
        if (world != nullptr) {
            if (ImGui::TreeNode("PlayerController")) {
                ImGui::TreeNodeSetOpen(ImGui::GetID("World"), false);
                ImGui::TreeNodeSetOpen(ImGui::GetID("Acknowledged Pawn"), false);
                ImGui::TreeNodeSetOpen(ImGui::GetID("Camera Manager"), false);
                auto scope = m_path.enter_clean("Player Controller");
                auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

                if (player_controller != nullptr) {
                    ui_handle_object(player_controller);
                } else {
                    ImGui::Text("No player controller");
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Acknowledged Pawn")) {
                ImGui::TreeNodeSetOpen(ImGui::GetID("World"), false);
                ImGui::TreeNodeSetOpen(ImGui::GetID("PlayerController"), false);
                ImGui::TreeNodeSetOpen(ImGui::GetID("Camera Manager"), false);
                auto scope = m_path.enter_clean("Acknowledged Pawn");
                auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

                if (player_controller != nullptr) {
                    auto pawn = player_controller->get_acknowledged_pawn();

                    if (pawn != nullptr) {
                        ui_handle_object(pawn);

                    } else {
                        ImGui::Text("No pawn");
                    }
                } else {
                    ImGui::Text("No player controller");
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Camera Manager")) {
                ImGui::TreeNodeSetOpen(ImGui::GetID("World"), false);
                ImGui::TreeNodeSetOpen(ImGui::GetID("Acknowledged Pawn"), false);
                ImGui::TreeNodeSetOpen(ImGui::GetID("PlayerController"), false);
                auto scope = m_path.enter_clean("Camera Manager");
                auto player_controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

                if (player_controller != nullptr) {
                    auto camera_manager = player_controller->get_player_camera_manager();

                    if (camera_manager != nullptr) {
                        ui_handle_object((sdk::UObject*)camera_manager);
                    } else {
                        ImGui::Text("No camera manager");
                    }
                } else {
                    ImGui::Text("No player controller");
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("World")) {
                ImGui::TreeNodeSetOpen(ImGui::GetID("Camera Manager"), false);
                ImGui::TreeNodeSetOpen(ImGui::GetID("Acknowledged Pawn"), false);
                ImGui::TreeNodeSetOpen(ImGui::GetID("PlayerController"), false);
                auto scope = m_path.enter_clean("World");
                ui_handle_object(world);
                ImGui::TreePop();
            }
        } else {
            ImGui::Text("No world");
        }

        ImGui::TreePop();
    }

    // ALWAYS call TreePop if the outer TreeNode opened, even if something
    // inside throws. The explicit `if (...) { ... TreePop(); }` we had before
    // would skip TreePop on any exception escaping the body — and the
    // in-iteration ScopeGuards only cover the *inner* per-class / per-object
    // TreeNodes, not this outer one. Without this guard, a single throw from
    // a deeply nested ui_handle_struct could leave the right-pane TreeNode
    // unbalanced and trigger ImGui's "Missing TreePop()" recovery.
    // `m_ObjectsByClass` here is a *static local* (declared above in this
    // function), not a member; capture it by reference.
    const bool objects_by_class_open = ImGui::TreeNode("Objects by class");
    m_ObjectsByClass = objects_by_class_open; // legacy: keeps the static cache in sync
    utility::ScopeGuard objects_by_class_guard{[objects_by_class_open]() {
        if (objects_by_class_open) {
            ImGui::TreePop();
        }
    }};
    if (objects_by_class_open) {
        ImGui::Checkbox("Hide Default Classes", &m_hide_default_classes);
        static char filter[256]{};
        ImGui::InputText("Filter", filter, sizeof(filter));

        const bool filter_empty = std::string_view{filter}.empty();

        // Shared with the Class Browser — it kicks off the same task. Idempotent.
        pump_class_sort_task();

        const auto wide_filter = utility::widen(filter);
        const bool made_child = ImGui::BeginChild("Objects by class entries", ImVec2(0, 0),  ImGuiChildFlags_Borders);

        utility::ScopeGuard sg{[made_child]() {
            //if (made_child) {
                // well apparently BeginChild doesn't care about if it returned true or not so...
                // TODO: check this every time we update imgui?
                ImGui::EndChild();
            //}
        }};

        drag_scroll_current_window();

        // Defensive helper: look up an object's meta entry without mutating the
        // map (avoids the silent default-insert that operator[] does on
        // unordered_map<*, unique_ptr<...>>). Under shared_lock the writer
        // can race with us, so we also guard against a stale/null entry.
        auto get_meta = [this](sdk::UObjectBase* obj) -> const MetaObject* {
            if (obj == nullptr) return nullptr;
            auto it = m_meta_objects.find(obj);
            if (it == m_meta_objects.end() || it->second == nullptr) {
                return nullptr;
            }
            return it->second.get();
        };

        for (auto uclass : m_sorted_classes) {
            auto objects_ref_it = m_objects_by_class.find(uclass);
            if (objects_ref_it == m_objects_by_class.end() || objects_ref_it->second.empty()) {
                continue;
            }
            const auto& objects_ref = objects_ref_it->second;

            const auto* uclass_meta = get_meta(uclass);
            if (uclass_meta == nullptr) {
                continue;
            }

            if (objects_ref.size() == 1 && m_hide_default_classes) {
                auto first = *objects_ref.begin();
                if (const auto* first_meta = get_meta(first); first_meta != nullptr) {
                    auto fc = first_meta->uclass;
                    if (fc != nullptr && get_meta(fc) != nullptr && fc->get_class_default_object() == first) {
                        continue;
                    }
                }
            }

            const auto uclass_name = utility::narrow(uclass_meta->full_name);
            bool valid = true;

            if (!filter_empty) {
                valid = false;

                for (auto super = (sdk::UStruct*)uclass; super; super = super->get_super_struct()) {
                    if (auto it = m_meta_objects.find(super); it != m_meta_objects.end() && it->second != nullptr) {
                        if (it->second->full_name.find(wide_filter) != std::wstring::npos) {
                            valid = true;
                            break;
                        }
                    }
                }
            }

            if (!valid) {
                continue;
            }

            // Outer per-uclass TreeNode. We pair it with a ScopeGuard so the
            // TreePop happens even if something inside throws — otherwise
            // ImGui's end-of-frame recovery fires "Missing TreePop()" and
            // poisons the rest of the window's state for the next frame.
            const bool class_node_open = ImGui::TreeNode(uclass_name.data());
            // Register the TreePop guard BEFORE any other call that operates
            // on the previous item (drag source). If make_drag_source_for_class
            // ever throws, the guard still runs and balances the TreeNode.
            utility::ScopeGuard class_node_guard{[class_node_open]() {
                if (class_node_open) {
                    ImGui::TreePop();
                }
            }};
            // Make every class entry in the Objects-by-class view a draggable
            // UClass source so users can drop it into a function caller's
            // ClassProperty slot.
            make_drag_source_for_class(uclass, uclass_name.c_str());

            if (!class_node_open) {
                ui_standard_object_context_menu(uclass);
                continue;
            }

            ui_standard_object_context_menu(uclass);

            std::vector<sdk::UObjectBase*> objects{};
            objects.reserve(objects_ref.size());

            for (auto object : objects_ref) {
                if (m_hide_default_classes) {
                    const auto* obj_meta = get_meta(object);
                    if (obj_meta == nullptr) continue;
                    auto c = obj_meta->uclass;
                    if (c != nullptr && get_meta(c) != nullptr && c->get_class_default_object() != object) {
                        objects.push_back(object);
                    }
                } else {
                    objects.push_back(object);
                }
            }

            std::sort(objects.begin(), objects.end(), [&get_meta](sdk::UObjectBase* a, sdk::UObjectBase* b) {
                const auto* ma = get_meta(a);
                const auto* mb = get_meta(b);
                if (ma == nullptr || mb == nullptr) {
                    return ma < mb; // stable-ish ordering for nulls
                }
                return ma->full_name < mb->full_name;
            });

            if (uclass->is_a(sdk::AActor::static_class())) {
                static char component_add_name[256]{};

                // Resolve a class by full path ("Class /Script/Engine.X") or by short name ("X",
                // retried under /Script/Engine — covers the common engine components).
                auto resolve_class = [](const char* name) -> sdk::UClass* {
                    if (name == nullptr || name[0] == '\0') return nullptr;
                    auto c = sdk::find_uobject<sdk::UClass>(utility::widen(name));
                    if (c == nullptr && std::string_view(name).find('/') == std::string_view::npos) {
                        c = sdk::find_uobject<sdk::UClass>(std::wstring(L"Class /Script/Engine.") + utility::widen(name));
                    }
                    return c;
                };
                // Queue a component to be added to every instance of this actor class on creation
                // (the "Permanently" path). Shared by the text field, drag-drop, and quick buttons.
                auto queue_add = [this, uclass](sdk::UClass* component_c) {
                    if (component_c == nullptr) {
                        strcpy_s(component_add_name, "Nonexistent component");
                        return;
                    }
                    // Independent mutex (see m_add_component_jobs_mtx decl): on_draw_ui already holds
                    // m_mutex (shared) around this whole draw, so we cannot lock m_mutex exclusively
                    // here. This serializes the map write against the object-creation hook reader.
                    std::scoped_lock _{m_add_component_jobs_mtx};
                    m_on_creation_add_component_jobs[uclass] = [this, component_c](sdk::UObject* object) {
                            // Re-validate BOTH the spawned object and the captured component class:
                            // this job lives in the map indefinitely and fires on every future spawn of
                            // the actor class, so a class dropped from the browser (a Blueprint-generated
                            // class can be GC'd) could otherwise become a wild pointer here.
                            if (!this->exists(object) || !this->exists((sdk::UObjectBase*)component_c)) {
                                return;
                            }

                            if (object == object->get_class()->get_class_default_object()) {
                                return;
                            }

                            auto actor = (sdk::AActor*)object;
                            auto component = (sdk::UObject*)actor->add_component_by_class(component_c);

                            if (component != nullptr) {
                                if (component->get_class()->is_a(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SphereComponent"))) {
                                    struct SphereRadiusParams {
                                        float radius{};
                                    };

                                    auto params = SphereRadiusParams{};
                                    params.radius = 100.f;

                                    const auto fn = component->get_class()->find_function(L"SetSphereRadius");

                                    if (fn != nullptr) {
                                        component->process_event(fn, &params);
                                    }
                                }

                                struct {
                                    bool hidden{false};
                                    bool propagate{true};
                                } set_hidden_params{};

                                const auto fn = component->get_class()->find_function(L"SetHiddenInGame");

                                if (fn != nullptr) {
                                    component->process_event(fn, &set_hidden_params);
                                }

                                actor->finish_add_component(component);

                                // Set component_add_name to empty
                                component_add_name[0] = '\0';
                            } else {
                                component_add_name[0] = 'e';
                                component_add_name[1] = 'r';
                                component_add_name[2] = 'r';
                                component_add_name[3] = '\0';
                            }
                    };
                }; // end queue_add

                if (ImGui::InputText("Add Component Permanently", component_add_name, sizeof(component_add_name),
                        ImGuiInputTextFlags_::ImGuiInputTextFlags_EnterReturnsTrue)) {
                    queue_add(resolve_class(component_add_name));
                }
                // Drag a UClass from the class browser onto the field above to add it.
                if (auto* dropped = accept_class_drop(); dropped != nullptr) {
                    queue_add(dropped);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(or drop a UClass here)");

                // Quick-add common components.
                static const char* const k_common_components[] = {
                    "StaticMeshComponent", "PointLightComponent", "SpotLightComponent",
                    "SphereComponent", "BoxComponent", "ArrowComponent", "SceneComponent",
                };
                ImGui::TextDisabled("Common:");
                for (int i = 0; i < (int)IM_ARRAYSIZE(k_common_components); ++i) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(k_common_components[i])) {
                        queue_add(resolve_class(k_common_components[i]));
                    }
                }
            }

            for (const auto& object : objects) {
                const auto* obj_meta = get_meta(object);
                if (obj_meta == nullptr) {
                    // Object lost its meta entry between iteration and render
                    // (concurrent removal). Skip rather than crashing on a
                    // null deref inside the TreeNode label, which would leave
                    // the parent TreeNode unbalanced.
                    continue;
                }

                const auto obj_name = utility::narrow(obj_meta->full_name);
                const bool obj_node_open = ImGui::TreeNode(obj_name.data());
                // Guard MUST be registered before any other call that operates
                // on the previous item — see the per-class comment above.
                utility::ScopeGuard obj_node_guard{[obj_node_open]() {
                    if (obj_node_open) {
                        ImGui::TreePop();
                    }
                }};
                make_drag_source_for_object((sdk::UObject*)object, obj_name.c_str());

                if (!obj_node_open) {
                    ui_standard_object_context_menu(object);
                    continue;
                }

                try {
                    ui_handle_object((sdk::UObject*)object);
                } catch (const std::exception& e) {
                    ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "ui_handle_object threw: %s", e.what());
                } catch (...) {
                    ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "ui_handle_object threw an unknown exception");
                }
            }
        }

        // TreePop is now handled by objects_by_class_guard so it runs even
        // on exception. (Used to be an explicit `ImGui::TreePop();` here.)
    }
}

void UObjectHook::ui_standard_object_context_menu(sdk::UObjectBase* object) {
    if (ImGui::BeginPopupContextItem()) {
        auto sc = [](const std::string& text) {
            if (OpenClipboard(NULL)) {
                EmptyClipboard();
                HGLOBAL hcd = GlobalAlloc(GMEM_DDESHARE, text.size() + 1);
                char* data = (char*)GlobalLock(hcd);
                strcpy(data, text.c_str());
                GlobalUnlock(hcd);
                SetClipboardData(CF_TEXT, hcd);
                CloseClipboard();
            }
        };

        if (ImGui::Button("Copy Name")) {
            // Use find() so a missing entry doesn't default-insert a null
            // unique_ptr and crash the right-click menu (same operator[]
            // hazard as the Objects-by-class iteration).
            if (auto it = m_meta_objects.find(object); it != m_meta_objects.end() && it->second != nullptr) {
                sc(utility::narrow(it->second->full_name));
            }
        }



        if (ImGui::Button("Copy Address")) {
            const auto hex = (std::stringstream{} << std::hex << (uintptr_t)object).str();
            sc(hex);
        }

        // Recenter to camera: only meaningful for scene components (needs set_world_location).
        // Mirrors the "Selected" panel button -- deproject the screen centre to a camera ray on the
        // game thread and place the object at ray_origin + forward * m_recenter_distance. object is a
        // UObjectBase*, so cast to UObject* for is_a and USceneComponent* for the move (same pattern
        // as the m_camera_attach path).
        if (this->exists(object) && ((sdk::UObject*)object)->is_a(sdk::USceneComponent::static_class())) {
            if (ImGui::Button("Recenter to camera")) {
                auto* comp = (sdk::USceneComponent*)object;
                const auto* vp = ImGui::GetMainViewport();
                const glm::vec2 screen_center{vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f};
                const float dist = m_recenter_distance;
                GameThreadWorker::get().enqueue([this, comp, screen_center, dist]() {
                    if (!this->exists(comp)) return;
                    try {
                        auto engine = sdk::UGameEngine::get();
                        auto world = engine != nullptr ? engine->get_world() : nullptr;
                        if (world == nullptr) return;
                        auto ugs = sdk::UGameplayStatics::get();
                        if (ugs == nullptr) return;
                        auto pc = ugs->get_player_controller(world, 0);
                        if (pc == nullptr) return;
                        glm::vec3 ray_origin{0.0f, 0.0f, 0.0f};
                        glm::vec3 ray_dir{0.0f, 0.0f, 0.0f};
                        if (!ugs->screen_to_world(pc, screen_center, &ray_origin, &ray_dir)) return;
                        const float len = glm::length(ray_dir);
                        if (len < 1e-6f) return;
                        ray_dir /= len;
                        comp->set_world_location(ray_origin + ray_dir * dist, false, false);
                    } catch (...) {}
                });
            }
        }

        ImGui::EndPopup();
    }
}

void UObjectHook::ui_handle_object(sdk::UObject* object) {
    if (object == nullptr) {
        ImGui::Text("nullptr");
        return;
    }

    if (!this->exists_unsafe(object)) {
        ImGui::Text("Invalid object");
        return;
    }

    ui_standard_object_context_menu(object);

    const auto uclass = object->get_class();

    if (uclass == nullptr) {
        ImGui::Text("null class");
        return;
    }

    // See m_property_edit_target_stack: tracks the nearest enclosing scene component so a property
    // edit reached through nested ui_handle_object recursion (Outer, ObjectProperty trees, ...) still
    // knows which component to refresh. Inherits the parent's target when this object isn't itself a
    // scene component, rather than clearing it, so drilling into e.g. a non-component sub-object still
    // refreshes the component that owns the path down to it.
    static const auto scene_comp_t = sdk::USceneComponent::static_class();
    sdk::USceneComponent* stack_target = (scene_comp_t != nullptr && uclass->is_a(scene_comp_t))
        ? (sdk::USceneComponent*)object
        : (m_property_edit_target_stack.empty() ? nullptr : m_property_edit_target_stack.back());
    m_property_edit_target_stack.push_back(stack_target);
    utility::ScopeGuard property_edit_stack_guard{[this]() { m_property_edit_target_stack.pop_back(); }};


    if (!this->exists_unsafe(uclass)) {
        ImGui::Text("Invalid class");
        return;
    }

    if (object->is_a(sdk::UClass::static_class())) {
        if (ImGui::TreeNode("Default Object")) {
            auto def = ((sdk::UClass*)object)->get_class_default_object();

            if (def != nullptr) {
                ui_handle_object(def);
            } else {
                ImGui::Text("Null default object");
            }

            ImGui::TreePop();
        }
    }

    // Prominent identity header (class + leaf name, coloured) with the full path dimmed below,
    // so stacked / nested inspected objects are easy to tell apart in the layout.
    {
        std::string full, header;
        try { full = utility::narrow(object->get_full_name()); } catch (...) {}
        try {
            header = utility::narrow(uclass->get_fname().to_string()) + "  " +
                     utility::narrow(object->get_fname().to_string());
        } catch (...) { header = full; }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.55f, 0.85f, 1.0f, 1.0f});
        ImGui::SeparatorText(header.c_str());
        ImGui::PopStyleColor();
        // Main-tree drag support (roadmap): the identity header of ANY inspected
        // object is a drag source, so objects reached through paths with no row
        // handle (Common Objects -> PlayerController/Pawn/World, Outer chains,
        // camera-attach node) can still be dragged onto caller slots / drop
        // targets. SeparatorText is an ID-0 item; the helper passes
        // SourceAllowNullID so attaching to it is legal.
        make_drag_source_for_object(object, header.c_str());
        if (!full.empty()) {
            ImGui::TextDisabled("%s", full.c_str());
        }
    }

    if (ImGui::TreeNode("Outer")) {
        auto outer_scope = m_path.enter("Outer");
        ui_handle_object(object->get_outer());
        ImGui::TreePop();
    }

    static const auto material_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.MaterialInterface");

    if (uclass->is_a(material_t)) {
        ui_handle_material_interface(object);
    }

    static const auto widget_component_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/UMG.WidgetComponent");

    if (uclass->is_a(widget_component_t)) {
        if (ImGui::Button("Set to Screen Space")) {
            static const auto set_widget_space_fn = uclass->find_function(L"SetWidgetSpace");

            if (set_widget_space_fn != nullptr) {
                struct {
                    uint32_t space{1};
                } params{};

                object->process_event(set_widget_space_fn, &params);
            }
        }
    }

    if (uclass->is_a(sdk::UActorComponent::static_class())) {
        if (ImGui::Button("Destroy Component")) {
            // destroy_component() -> AActor::destroy_component() -> ProcessEvent(K2_DestroyComponent),
            // same class of engine-touching call as every other mutating button here (Show/Hide,
            // Attach, Save Position, ...) — those are ALL deferred to the game thread; this one wasn't,
            // which is why it silently did nothing / was unsafe: ProcessEvent from the draw/UI thread
            // races the game thread instead of running on it.
            auto comp = (sdk::UActorComponent*)object;
            GameThreadWorker::get().enqueue([this, comp]() {
                if (!this->exists(comp)) return;
                cleanup_references_to((sdk::UObjectBase*)comp);
                try { comp->destroy_component(); } catch (...) {}
            });
        }
    }

    if (uclass->is_a(sdk::USceneComponent::static_class())) {
        ui_handle_scene_component((sdk::USceneComponent*)object);
    }

    if (uclass->is_a(sdk::AActor::static_class())) {
        ui_handle_actor(object);
    }

    ui_handle_struct(object, uclass);
}

bool UObjectHook::object_from_path_or_address(std::string_view object, sdk::UObject* out) {
    *out = sdk::UObject();
    if (object.contains(' ')) {
        auto _type = object.substr(0, object.find_first_of(' '));
        if (_type.ends_with("Class")) {
            out = sdk::find_uobject<sdk::UClass>(utility::widen(object));
            return out != nullptr;
        } else if (_type.ends_with("ScriptStruct")) {
            out = sdk::find_uobject<sdk::UScriptStruct>(utility::widen(object));
            return out != nullptr;
        } else if (_type.ends_with("Function")) {
            out = sdk::find_uobject<sdk::UFunction>(utility::widen(object));
            return out != nullptr;
        }
      else{
        out = sdk::find_uobject<sdk::UObject>(utility::widen(object));
        return out != nullptr;
        }
    } else {

        out = (sdk::UObject*)std::stoull(object.data(), nullptr, 16);
        return out != nullptr;
        }
     return false;
}


void UObjectHook::ui_handle_scene_component(sdk::USceneComponent* comp) {
    if (comp == nullptr) {
        return;
    }

    {
        bool gizmo = m_gizmo_components.contains(comp);
        if (ImGui::Checkbox("Show gizmo", &gizmo)) {
            // Defer the mutation to the game thread: this draw path can run under a shared_lock (e.g.
            // the class-inspector Instances tab), so taking the unique lock here would deadlock the
            // non-recursive shared_mutex. The worker runs with no lock held; it re-validates the object
            // still lives via m_objects under the same lock (can't call exists() — that re-locks).
            const bool add = gizmo;
            GameThreadWorker::get().enqueue([this, comp, add]() {
                bool changed = false;
                {
                    std::unique_lock _{m_mutex};
                    if (add) {
                        if (m_objects.contains(reinterpret_cast<sdk::UObjectBase*>(comp))) {
                            m_gizmo_components.insert(comp);
                            changed = true;
                        }
                    } else {
                        changed = m_gizmo_components.erase(comp) > 0;
                    }
                }
                if (changed) {
                    dispatch_gizmo_target_event(comp, add); // outside the lock — dispatch can re-enter
                }
            });
        }
        if (gizmo) {
            ImGui::SameLine();
            ImGui::RadioButton("Move##gizmo", &m_gizmo_mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Rotate##gizmo", &m_gizmo_mode, 1);
            ImGui::SameLine();
            ImGui::RadioButton("Scale##gizmo", &m_gizmo_mode, 2);
            ImGui::SameLine();
            if (ImGui::Button("Settings##gizmo")) {
                ImGui::OpenPopup("gizmo_settings");
            }
            // Global gizmo appearance — applies universally to all modes/components.
            if (ImGui::BeginPopup("gizmo_settings")) {
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat("thickness", &m_gizmo_thickness, 1.0f, 12.0f, "%.1f px");
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat("length", &m_gizmo_axis_len, 5.0f, 1000.0f, "%.0f cm");
                ImGui::Checkbox("Auto-gizmo on MC adjust (VR)", &m_auto_gizmo_on_adjust);
                ImGui::Checkbox("Show labels", &m_gizmo_show_labels);
                if (m_click_select_mode) {
                    if (ImGui::Button("Picking… (Esc to cancel)")) { m_click_select_mode = false; }
                } else if (ImGui::Button("Pick gizmo target")) {
                    m_click_select_mode = true;
                }
                ImGui::SameLine();
                ImGui::Checkbox("sticky##pick2", &m_click_select_sticky);
                ImGui::EndPopup();
            }
        }
    }

    bool attached = m_motion_controller_attached_components.contains(comp);

    if (attached) {
        if (ImGui::Button("Detach")) {
            m_motion_controller_attached_components.erase(comp);

            auto existing = std::find_if(m_persistent_states.begin(), m_persistent_states.end(), [&](const auto& state2) {
                return state2 != nullptr && state2->path.resolve() == comp;
            });

            if (existing != m_persistent_states.end()) {
                (*existing)->erase_json_file();
                m_persistent_states.erase(existing);
            }
        }

        if (m_motion_controller_attached_components.contains(comp)) {
            ImGui::SameLine();
            auto& state = m_motion_controller_attached_components[comp];

            if (ImGui::Checkbox("Adjust", &state->adjusting)) {
                if (state->adjusting && m_overlap_detection_actor == nullptr) {
                    VR::get()->set_aim_allowed(false);
                    g_framework->set_draw_ui(false);
                }
            }

            ImGui::SameLine();

            if (ImGui::Checkbox("Permanent Change", &state->permanent)) {
                // Locate the existing persistent state if it exists
                auto existing = std::find_if(m_persistent_states.begin(), m_persistent_states.end(), [&](const auto& state2) {
                    return state2 != nullptr && state2->path.resolve() == comp;
                });

                if (existing != m_persistent_states.end()) {
                    (*existing)->state.permanent = state->permanent;
                }
            }

            auto existing = std::find_if(m_persistent_states.begin(), m_persistent_states.end(), [&](const auto& state2) {
                return state2 != nullptr && state2->path.resolve() == comp;
            });

            // Finetuning of the controller rotation offset
            // Convert to pitch/yaw/roll first.
            auto euler = utility::math::euler_angles_from_steamvr(state->rotation_offset);
            if (ImGui::DragFloat3("RotationOffset", &euler.x, 0.01f)) {
                // Convert back to quaternion
                state->rotation_offset = glm::quat{glm::yawPitchRoll(-euler.y, euler.x, -euler.z)};

                if (existing != m_persistent_states.end()) {
                    (*existing)->state.rotation_offset = state->rotation_offset;
                }
            }

            // Finetuning of the controller position offset.
            if (ImGui::DragFloat3("PositionOffset", &state->location_offset.x, 0.01f)) {
                if (existing != m_persistent_states.end()) {
                    (*existing)->state.location_offset = state->location_offset;
                }
            }

            //for (auto uobject : m_inline_uobjecthooks) {
            //    sdk::UObject* out;
            //    if (UObjectHook::object_from_path_or_address(uobject.first, out)){
            //        if (out == comp) {
            //
            //                }
            //        }
            //
            //}

            auto save_state_logic = [&](const std::vector<std::string>& path) {
                auto json = serialize_mc_state(path, state);

                // Concat the entire path together and hash it to get a unique name
                std::string concat_path{};
                for (const auto& p : path) {
                    concat_path += p;
                }

                const auto hash_str = std::to_string(utility::hash(concat_path)) + "_mc_state.json";
                auto wanted_path = UObjectHook::get_persistent_dir() / hash_str;

                // Use the one this was originally loaded from instead.
                if (existing != m_persistent_states.end() && (*existing)->path_to_json.has_value()) {
                    wanted_path = (*existing)->path_to_json.value();
                }

                // Create dir if necessary
                try {
                    std::filesystem::create_directories(wanted_path.parent_path());

                    if (std::filesystem::exists(wanted_path.parent_path())) {
                        std::ofstream file{wanted_path};
                        file << json.dump(4);
                        file.close();

                        m_persistent_states = deserialize_all_mc_states();
                    }
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("[UObjectHook] Failed to save motion controller state: {}", e.what());
                } catch (...) {
                    SPDLOG_ERROR("[UObjectHook] Failed to save motion controller state");
                }
            };

            // Save state stuff
            // First one is for checking whether we already have an existing persistent state
            // with its own path.
            if (existing != m_persistent_states.end()) {
                if (ImGui::Button("Save state")) {
                    save_state_logic((*existing)->path.path());
                }
            } else if (m_path.has_valid_base()) {
                if (ImGui::Button("Save state")) {
                    save_state_logic(m_path.path());
                }
            } else {
                if (auto path = try_get_path(comp); path.has_value()) {
                    if (ImGui::Button("Save state")) {
                        save_state_logic(path->path());
                    }
                } else {
                    ImGui::Text("Can't save, did not start from a valid base or none of the allowed bases can reach this component");
                }
            }
        }
    } else {
        if (m_camera_attach.object != comp) {
            if (ImGui::Button("Attach left")) {
                m_motion_controller_attached_components[comp] = std::make_shared<MotionControllerState>();
                m_motion_controller_attached_components[comp]->hand = 0;
            }

            ImGui::SameLine();

            if (ImGui::Button("Attach right")) {
                m_motion_controller_attached_components[comp] = std::make_shared<MotionControllerState>();
                m_motion_controller_attached_components[comp]->hand = 1;
            }

            ImGui::SameLine();

            if (ImGui::Button("Attach HMD")) {
                m_motion_controller_attached_components[comp] = std::make_shared<MotionControllerState>();
                m_motion_controller_attached_components[comp]->hand = 2;
            }

            if (ImGui::Button("Attach Camera to")) {
                m_camera_attach.object = comp;
                m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};
            }


            ImGui::SameLine();

            if (ImGui::Button("Attach Camera to (Relative)")) {
                m_camera_attach.object = comp;
                m_camera_attach.offset = glm::vec3{0.0f, 0.0f, m_last_camera_location.z - comp->get_world_location().z};
            }
        } else {
            if (ImGui::Button("Detach")) {
                m_camera_attach.object = nullptr;
                m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};

                if (m_persistent_camera_state != nullptr) {
                    m_persistent_camera_state->erase_json_file();
                }

                m_persistent_camera_state.reset();
            }

            ImGui::SameLine();

            if (m_persistent_camera_state != nullptr && m_persistent_camera_state->path.resolve() == comp) {
                if (ImGui::Button("Save state")) {
                    save_camera_state(m_persistent_camera_state->path.path());
                }
            } else if (m_path.has_valid_base()) {
                if (ImGui::Button("Save state")) {
                    save_camera_state(m_path.path());
                }
            } else if (auto path = try_get_path(comp); path.has_value()) {
                if (ImGui::Button("Save state")) {
                    save_camera_state(path->path());
                }
            } else {
                ImGui::Text("Can't save, did not start from a valid base or none of the allowed bases can reach this component");
            }

            if (ImGui::DragFloat3("Camera Offset", &m_camera_attach.offset.x, 0.1f)) {
                if (m_persistent_camera_state != nullptr) {
                    m_persistent_camera_state->offset = m_camera_attach.offset;
                }
            }
        }
    }

    void* addr = (void*)((uintptr_t)comp);
    const auto hex = (std::stringstream{} << std::hex << (uintptr_t)addr).str();

    // Local toggle switches location/rotation between world and parent-relative
    // space (scale is always relative). Reset buttons go to identity. The Quat row
    // edits the same rotation as a quaternion (routed through euler since the SDK
    // setter is FRotator-based).
    ImGui::Checkbox("Local##xform", &m_gizmo_local);
    ImGui::SameLine();
    ImGui::TextDisabled(m_gizmo_local ? "(relative)" : "(world)");

    glm::vec3 loc = m_gizmo_local ? comp->get_relative_location() : comp->get_world_location();
    glm::vec3 rot = m_gizmo_local ? comp->get_relative_rotation() : comp->get_world_rotation();
    glm::vec3 scale = comp->get_relative_scale();

    const bool local = m_gizmo_local;
    auto set_loc = [comp, local](const glm::vec3& v) {
        if (local) { comp->set_relative_location(v, true, false); }
        else       { comp->set_world_location(v, true, false); }
    };
    auto set_rot = [comp, local](const glm::vec3& v) {
        if (local) { comp->set_relative_rotation(v, true, false); }
        else       { comp->set_world_rotation(v, true, false); }
    };

    // #2 (flat): when a transform DragFloat3 sub-field is dragged, light up the matching gizmo
    // axis (X/Y/Z field -> X/Y/Z handle) so you can see which axis you're driving from the panel.
    // Detect the moved component by diffing before/after; the gizmo expires the highlight after a
    // couple frames so it tracks active dragging.
    auto note_driven = [this, comp](const glm::vec3& before, const glm::vec3& after) {
        int idx = -1; float best = 1e-9f;
        for (int i = 0; i < 3; ++i) { const float d = std::abs(after[i] - before[i]); if (d > best) { best = d; idx = i; } }
        if (idx >= 0) { m_driven_comp = comp; m_driven_axis = idx; m_driven_frame = (uint32_t)ImGui::GetFrameCount(); }
    };

    ImGui::PushID("location");
    if (ImGui::SmallButton("R")) { set_loc(glm::vec3{0.0f, 0.0f, 0.0f}); }
    ImGui::SameLine();
    {
        const glm::vec3 before = loc;
        bool ch = ImGui::DragFloat3(local ? "Location" : "World Location", &loc.x, 0.1f);
        ch |= vector_copy_paste("##ctx", &loc.x, 3);
        if (ch) { note_driven(before, loc); set_loc(loc); }
    }
    ImGui::PopID();

    ImGui::PushID("rotation");
    if (ImGui::SmallButton("R")) { set_rot(glm::vec3{0.0f, 0.0f, 0.0f}); }
    ImGui::SameLine();
    {
        const glm::vec3 before = rot;
        bool ch = ImGui::DragFloat3(local ? "Rotation" : "World Rotation", &rot.x, 0.1f);
        ch |= vector_copy_paste("##ctx", &rot.x, 3);
        if (ch) { note_driven(before, rot); set_rot(rot); }
    }
    {
        // FRotator (pitch=x, yaw=y, roll=z deg) -> quat with the same convention the
        // rest of UObjectHook uses, displayed as xyzw.
        const glm::quat q{glm::yawPitchRoll(glm::radians(-rot.y), glm::radians(rot.x), glm::radians(-rot.z))};
        glm::vec4 qv{q.x, q.y, q.z, q.w};
        bool ch = ImGui::DragFloat4("Quat (xyzw)", &qv.x, 0.01f);
        ch |= vector_copy_paste("##quatctx", &qv.x, 4);
        if (ch) {
            // Guard a near-zero quat (e.g. dragging w to 0): normalize() would
            // produce NaN and a NaN FRotator would corrupt the transform.
            const float ql2 = qv.x * qv.x + qv.y * qv.y + qv.z * qv.z + qv.w * qv.w;
            if (ql2 > 1e-6f) {
                glm::quat nq = glm::normalize(glm::quat{qv.w, qv.x, qv.y, qv.z});
                set_rot(glm::degrees(utility::math::euler_angles_from_steamvr(nq)));
            }
        }
    }
    ImGui::PopID();

    ImGui::PushID("scale");
    if (ImGui::SmallButton("R")) { comp->set_relative_scale(glm::vec3{1.0f, 1.0f, 1.0f}); }
    ImGui::SameLine();
    {
        const glm::vec3 before = scale;
        bool ch = ImGui::DragFloat3("Scale", &scale.x, 0.01f);
        ch |= vector_copy_paste("##ctx", &scale.x, 3);
        if (ch) { note_driven(before, scale); comp->set_relative_scale(scale); }
    }
    ImGui::PopID();
    ImGui::Separator();
    ImGui::PushID("CamOffset");
    {
        bool ch = ImGui::DragFloat3("Camera Offset", &m_camera_attach.offset.x, 0.1f);
        ch |= vector_copy_paste("##ctx", &m_camera_attach.offset.x, 3);
        if (ch && m_persistent_camera_state != nullptr) {
            m_persistent_camera_state->offset = m_camera_attach.offset;
        }
    }
    ImGui::PopID();

    const auto prim_comp_t = sdk::UPrimitiveComponent::static_class();
    auto prim_comp = (sdk::UPrimitiveComponent*)comp;

    bool visible = comp->is_a(prim_comp_t) ? prim_comp->is_rendering_in_main_pass() : comp->is_visible();
    bool legacy_visible = comp->is_visible();

    auto visible_checkbox = ImGui::Checkbox("Visible", &visible);
    ImGui::SameLine();

    const auto legacy_visible_changed = ImGui::Checkbox("Legacy", &legacy_visible);
    visible_checkbox |= legacy_visible_changed;

    if (visible_checkbox) {
        if (legacy_visible_changed) {
            visible = legacy_visible;
        }

        if (comp->is_a(prim_comp_t)) {
            prim_comp->set_overall_visibility(visible, legacy_visible_changed || (visible == true && !legacy_visible));
        } else {
            comp->set_visibility(visible, false);
        }

        if (visible) {
            // Check if we have a persistent property for this component
            std::shared_ptr<PersistentProperties> props{};

            for (const auto& existing_prop : m_persistent_properties) {
                if (resolve_persistent_target(*existing_prop, /*use_cooldown*/ false) == comp) {
                    props = existing_prop;
                    break;
                }
            }

            if (props != nullptr && props->hide) {
                props->hide = false;
                props->save_to_file();
            }
        }

        if (legacy_visible) {
            // Check if we have a persistent property for this component
            std::shared_ptr<PersistentProperties> props{};

            for (const auto& existing_prop : m_persistent_properties) {
                if (resolve_persistent_target(*existing_prop, /*use_cooldown*/ false) == comp) {
                    props = existing_prop;
                    break;
                }
            }

            if (props != nullptr && props->hide_legacy) {
                props->hide_legacy = false;
                props->save_to_file();
            }
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Save Visibility State")) {
        std::shared_ptr<PersistentProperties> props{};

        // Find existing one if possible
        for (const auto& existing_prop : m_persistent_properties) {
            if (resolve_persistent_target(*existing_prop, /*use_cooldown*/ false) == comp) {
                props = existing_prop;
                break;
            }
        }

        // Add new one if necessary
        if (props == nullptr) {
            if (m_path.has_valid_base()) {
                props = std::make_shared<PersistentProperties>();
                props->path = m_path;
            } else if (auto path = try_get_path(comp); path.has_value()) {
                props = std::make_shared<PersistentProperties>();
                props->path = path.value();
            } else {
                SPDLOG_ERROR("[UObjectHook] Can't save visibility state, did not start from a valid base or none of the allowed bases can reach this component");
                return;
            }

            if (props != nullptr) {
                m_persistent_properties.push_back(props);
            }
        }

        if (props != nullptr) {
            props->hide = !visible;
            props->hide_legacy = !legacy_visible;
            props->save_to_file();
        }
    }

    if (ImGui::TreeNode("Sockets")) {
        const auto socket_names = comp->get_all_socket_names();

        for (auto& name : socket_names) {
            //ImGui::Text("%s", utility::narrow(name.to_string()).data());
            if (ImGui::TreeNode(utility::narrow(name.to_string()).data())) {
                auto location = comp->get_socket_location(name.to_string());
                auto rotation = comp->get_socket_rotation(name.to_string());

                if (ImGui::DragFloat3("Location", &location.x, 0.1f)) {
                    //comp->set_socket_location(name, location);
                }

                if (ImGui::DragFloat3("Rotation", &rotation.x, 0.1f)) {
                    //comp->set_socket_rotation(name, rotation);
                }

                if (comp->get_class()->get_fname().to_string().ends_with(L"MeshComponent")) {


                  if (ImGui::Button("Toggle Visibility")){
                        static auto isbonehidden = comp->get_class()->find_function(L"IsBoneHiddenByName");
                        static auto hidebone = comp->get_class()->find_function(L"HideBoneByName");
                        static auto unhidebone = comp->get_class()->find_function(L"UnHideBoneByName");
                        struct {
                            sdk::FName name;
                            bool ReturnValue;
                        } parms;
                        parms.name = name;
                        comp->process_event(isbonehidden, &parms);

                        comp->process_event(parms.ReturnValue ? unhidebone : hidebone, &parms);



                //std::string_view luadata = "local comp = uevr.api:to_uobject(" +
                //     (std::stringstream{} << std::hex << (uintptr_t)comp).str() +  ")\nlocal socket = '" +
                //        (utility::narrow(name.to_string())) + R"('
                //                    if comp:IsBoneHiddenByName(socket) then
                //                    comp:UnHideBoneByName(socket)
                //                    else comp:HideBoneByName(socket)
                //                end)";
                //    PluginLoader::get()->do_lua_string(luadata.data(), "togglevis");
                    }
                }
                ImGui::TreePop();
            }

            }

        ImGui::TreePop();
    }
}

std::optional<UObjectHook::StatePath> UObjectHook::try_get_path(sdk::UObject* target) const {
    const auto component_name = utility::narrow(target->get_class()->get_fname().to_string() + L" " + target->get_fname().to_string());

    // Check if any of our bases can reach this component through their components list.
    for (const auto& allowed_base : s_allowed_bases) {
        const auto possible_path = std::vector<std::string>{allowed_base, "Components", component_name};
        const auto path = StatePath{possible_path};
        const auto resolved = path.resolve();

        if (resolved == target) {
            return path;
        }
    }

    // Try to look through the objects' properties instead now.
    for (const auto& allowed_base : s_allowed_bases) {
        const auto base_obj = StatePath{{allowed_base}}.resolve_base_object();

        if (base_obj == nullptr) {
            continue;
        }

        for (auto field = base_obj->get_class()->get_child_properties(); field != nullptr; field = field->get_next()) {
            if (field->get_class()->get_name().to_string() != L"ObjectProperty") {
                continue;
            }

            const auto prop = (sdk::FObjectProperty*)field;
            const auto obj_ptr = prop->get_data<sdk::UObject*>(base_obj);

            if (obj_ptr == nullptr || *obj_ptr == nullptr) {
                continue;
            }

            const auto obj = *obj_ptr;

            if (obj == target) {
                const auto possible_path = std::vector<std::string>{allowed_base, "Properties", utility::narrow(field->get_field_name().to_string())};
                const auto path = StatePath{possible_path};
                const auto resolved = path.resolve();

                if (resolved == target) {
                    return path;
                }

                break;
            }

            // Traverse that object's components now and see if we can find it there.
            if (obj->get_class()->is_a(sdk::AActor::static_class())) {
                const auto actor = (sdk::AActor*)*obj_ptr;

                for (auto actor_comp : actor->get_all_components()) {
                    if (actor_comp != target) {
                        continue;
                    }

                    const auto possible_path = std::vector<std::string>{allowed_base, "Properties", utility::narrow(field->get_field_name().to_string()), "Components", component_name};
                    const auto path = StatePath{possible_path};
                    const auto resolved = path.resolve();

                    if (resolved == target) {
                        return path;
                    }
                }
            }
        }
    }

    return std::nullopt;
}

void UObjectHook::ui_handle_material_interface(sdk::UObject* object) {
    if (object == nullptr) {
        return;
    }

    const auto uclass = object->get_class();

    if (uclass == nullptr) {
        return;
    }

    if (ImGui::Button("Apply to all actors")) {
        static const auto mesh_component_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.StaticMeshComponent");
        static const auto create_dynamic_mat = mesh_component_t->find_function(L"CreateDynamicMaterialInstance");
        static const auto set_material_fn = mesh_component_t->find_function(L"SetMaterial");
        static const auto get_num_materials_fn = mesh_component_t->find_function(L"GetNumMaterials");

        //const auto actors = m_objects_by_class[sdk::AActor::static_class()];
        const auto components = m_objects_by_class[mesh_component_t];

        //auto components = actor->get_all_components();

        for (auto comp_obj : components) {
            auto comp = (sdk::UActorComponent*)comp_obj;
            if (comp->is_a(mesh_component_t)) {
                if (comp == comp->get_class()->get_class_default_object()) {
                    continue;
                }

                auto owner = comp->get_owner();

                if (owner == nullptr || owner->get_class()->get_class_default_object() == owner) {
                    continue;
                }

                struct {
                    int32_t num{};
                } get_num_materials_params{};

                comp->process_event(get_num_materials_fn, &get_num_materials_params);

                for (int i = 0; i < get_num_materials_params.num; ++i) {
                    GameThreadWorker::get().enqueue([this, i, object, comp]() {
                        if (!this->exists(comp) || !this->exists(object)) {
                            return;
                        }

                        struct {
                            int32_t index{};
                            sdk::UObject* material{};
                            sdk::FName name{L"None"};
                            sdk::UObject* ret{};
                        } params{};

                        params.index = i;
                        params.material = object;

                        comp->process_event(create_dynamic_mat, &params);

                        if (params.ret != nullptr && object->get_full_name().find(L"GizmoMaterial") != std::wstring::npos) {
                            const auto c = params.ret->get_class();
                            static const auto set_vector_param_fn = c->find_function(L"SetVectorParameterValue");

                            struct {
                                sdk::FName name{L"GizmoColor"};
                                glm::vec4 color{};
                            } set_vector_param_params{};

                            set_vector_param_params.color.x = 1.0f;
                            set_vector_param_params.color.y = 0.0f;
                            set_vector_param_params.color.z = 0.0f;
                            set_vector_param_params.color.w = 1.0f;

                            params.ret->process_event(set_vector_param_fn, &set_vector_param_params);
                        }

                        if (params.ret != nullptr && object->get_full_name().find(L"BasicShapeMaterial") != std::wstring::npos) {
                            const auto c = params.ret->get_class();
                            static const auto set_vector_param_fn = c->find_function(L"SetVectorParameterValue");

                            struct {
                                sdk::FName name{L"Color"};
                                glm::vec4 color{};
                            } set_vector_param_params{};

                            set_vector_param_params.color.x = 1.0f;
                            set_vector_param_params.color.y = 0.0f;
                            set_vector_param_params.color.z = 0.0f;
                            set_vector_param_params.color.w = 1.0f;

                            params.ret->process_event(set_vector_param_fn, &set_vector_param_params);
                        }
                    });
                }
            }
        }
    }
}

void UObjectHook::ui_handle_actor(sdk::UObject* object) {
    if (object == nullptr) {
        return;
    }

    const auto uclass = object->get_class();

    if (uclass == nullptr) {
        return;
    }
    static auto pc = sdk::UGameplayStatics::get()->get_player_controller(sdk::UGameEngine::get()->get_world(), 0);
    auto actor = (sdk::AActor*)object;
    if (auto comp = actor->get_root_component()){
   		bool gizmo = m_gizmo_components.contains(comp);
        if (ImGui::Checkbox("Show gizmo", &gizmo)) {
            // Deferred mutation — see the matching note in ui_handle_scene_component. This path can run
            // under a shared_lock, so push the insert/erase to the game thread where no lock is held.
            const bool add = gizmo;
            GameThreadWorker::get().enqueue([this, comp, add]() {
                bool changed = false;
                {
                    std::unique_lock _{m_mutex};
                    if (add) {
                        if (m_objects.contains(reinterpret_cast<sdk::UObjectBase*>(comp))) {
                            m_gizmo_components.insert(comp);
                            changed = true;
                        }
                    } else {
                        changed = m_gizmo_components.erase(comp) > 0;
                    }
                }
                if (changed) {
                    dispatch_gizmo_target_event(comp, add); // outside the lock — dispatch can re-enter
                }
            });
        }
    }

    if (ImGui::Button("Destroy Actor")) {
        GameThreadWorker::get().enqueue([this, actor]() {
            if (!this->exists(actor)) return;
            cleanup_references_to((sdk::UObjectBase*)actor);
            try { actor->destroy_actor(); } catch (...) {}
        });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Removes this actor's gizmo/motion-controller/camera-attach references first, then destroys it.");
    }

        if (m_camera_attach.object != object ){
        if (ImGui::Button("Attach Camera to")) {
            m_camera_attach.object = object;
            m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};
        }

        ImGui::SameLine();

        if (ImGui::Button("Attach Camera to (Relative)")) {
            m_camera_attach.object = object;
            m_camera_attach.offset = glm::vec3{0.0f, 0.0f, m_last_camera_location.z - actor->get_actor_location().z};
        }
    } else {
        if (ImGui::Button("Detach")) {
            m_camera_attach.object = nullptr;
            m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};

            if (m_persistent_camera_state != nullptr) {
                m_persistent_camera_state->erase_json_file();
            }

            m_persistent_camera_state.reset();
        }

        if (m_persistent_camera_state != nullptr && m_persistent_camera_state->path.resolve() == object) {
            if (ImGui::Button("Save state")) {
                save_camera_state(m_persistent_camera_state->path.path());
            }
        } else if (m_path.has_valid_base()) {
            if (ImGui::Button("Save state")) {
                save_camera_state(m_path.path());
            }
        } else if (auto path = try_get_path(object); path.has_value()) {
            if (ImGui::Button("Save state")) {
                save_camera_state(path->path());
            }
        } else {
            ImGui::Text("Can't save, did not start from a valid base or none of the allowed bases can reach this object");
        }

        if (ImGui::DragFloat3("Camera Offset", &m_camera_attach.offset.x, 0.1f)) {
            if (m_persistent_camera_state != nullptr) {
                m_persistent_camera_state->offset = m_camera_attach.offset;
            }
        }
    }

    static char component_add_name[256]{};

    if (ImGui::InputText("Add Component", component_add_name, sizeof(component_add_name), ImGuiInputTextFlags_::ImGuiInputTextFlags_EnterReturnsTrue)) {
        const auto component_c = sdk::find_uobject<sdk::UClass>(utility::widen(component_add_name));

        if (component_c != nullptr) {
            GameThreadWorker::get().enqueue([=, this]() {
                if (!this->exists(object)) {
                    return;
                }

                auto component = (sdk::UObject*)actor->add_component_by_class(component_c);

                if (component != nullptr) {
                    if (component->get_class()->is_a(sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.SphereComponent"))) {
                        struct SphereRadiusParams {
                            float radius{};
                        };

                        auto params = SphereRadiusParams{};
                        params.radius = 100.f;

                        const auto fn = component->get_class()->find_function(L"SetSphereRadius");

                        if (fn != nullptr) {
                            component->process_event(fn, &params);
                        }
                    }

                    struct {
                        bool hidden{false};
                        bool propagate{true};
                    } set_hidden_params{};

                    const auto fn = component->get_class()->find_function(L"SetHiddenInGame");

                    if (fn != nullptr) {
                        component->process_event(fn, &set_hidden_params);
                    }

                    actor->finish_add_component(component);

                    // Set component_add_name to empty
                    component_add_name[0] = '\0';
                } else {
                    component_add_name[0] = 'e';
                    component_add_name[1] = 'r';
                    component_add_name[2] = 'r';
                    component_add_name[3] = '\0';
                }
            });
        } else {
            strcpy_s(component_add_name, "Nonexistent component");
        }
    }

    if (ImGui::TreeNode("Components")) {
        auto scope = m_path.enter("Components");
        auto components = actor->get_all_components();

        // Drop null/stale entries before sorting so the comparator can't deref a bad ptr.
        std::erase_if(components, [this](sdk::UObject* c) { return c == nullptr || !this->exists_unsafe(c); });

        std::sort(components.begin(), components.end(), [](sdk::UObject* a, sdk::UObject* b) {
            std::wstring an, bn;
            try { an = a->get_full_name(); } catch (...) {}
            try { bn = b->get_full_name(); } catch (...) {}
            return an < bn;
        });

        for (auto comp : components) {
            auto comp_obj = (sdk::UObject*)comp;

            ImGui::PushID(comp_obj);
            utility::ScopeGuard id_guard{[]() { ImGui::PopID(); }};
            // not using full_name because its HUGE
            std::wstring comp_name;
            try {
                const auto cls = comp->get_class();
                comp_name = (cls != nullptr ? cls->get_fname().to_string() : std::wstring{L"<no class>"})
                          + L" " + comp->get_fname().to_string();
            } catch (...) {
                comp_name = L"<unreadable component>";
            }
            const auto narrow = utility::narrow(comp_name);
            const bool made = ImGui::TreeNode(narrow.data());
            // Main-tree drag support (roadmap): every component row is a drag
            // source, open or closed, same as the Attached/Overlapped lists.
            make_drag_source_for_object(comp_obj, narrow.c_str());
            utility::ScopeGuard tree_guard{[made]() { if (made) ImGui::TreePop(); }};

            if (made) {
                auto scope2 = m_path.enter(narrow);
                try { ui_handle_object(comp_obj); }
                catch (...) { ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<failed to display component>"); }
            }
        }

        ImGui::TreePop();
    }
}

namespace {
// Forward declaration: defined below (after is_func_blocked/is_func_monitored, which it calls) —
// set_func_blocked/set_func_monitored need to call it at their point of state change.
void dispatch_function_monitor_event(sdk::UFunction* fn, bool added, const char* source);

// User-blocked UFunctions. The pre-hook below returns false for any function in
// this set, which makes PluginLoader::ufunction_hook_intermediary skip the
// original call (the function becomes a no-op). The hook stays installed when a
// function is unblocked — it simply returns true again — so toggling needs no
// unhook path.
std::unordered_set<sdk::UFunction*> g_blocked_funcs{};
std::mutex g_blocked_funcs_mtx{};

bool uobjecthook_block_pre(UEVR_UFunctionHandle fn, UEVR_UObjectHandle, void*, void*) {
    std::scoped_lock _{g_blocked_funcs_mtx};
    return g_blocked_funcs.find((sdk::UFunction*)fn) == g_blocked_funcs.end();
}

bool is_func_blocked(sdk::UFunction* fn) {
    std::scoped_lock _{g_blocked_funcs_mtx};
    return g_blocked_funcs.find(fn) != g_blocked_funcs.end();
}

void set_func_blocked(sdk::UFunction* fn, bool blocked) {
    if (blocked) {
        // hook_ufunction_ptr dedups the same pre-fn pointer, so this is idempotent.
        PluginLoader::get()->hook_ufunction_ptr((UEVR_UFunctionHandle)fn, &uobjecthook_block_pre, nullptr);
        std::scoped_lock _{g_blocked_funcs_mtx};
        g_blocked_funcs.insert(fn);
    } else {
        std::scoped_lock _{g_blocked_funcs_mtx};
        g_blocked_funcs.erase(fn);
    }
    dispatch_function_monitor_event(fn, blocked, "block");
}

// Per-function call monitor: a post-hook counts invocations for any monitored
// function. Same persistent-hook pattern as the blocker — unmonitoring just
// removes it from the set. Counts are read on the render thread, incremented on
// the game thread, so the map is mutex-guarded.
std::unordered_set<sdk::UFunction*> g_monitored_funcs{};
std::unordered_map<sdk::UFunction*, uint64_t> g_func_call_counts{};
std::mutex g_monitor_mtx{};

bool uobjecthook_monitor_post(UEVR_UFunctionHandle fn, UEVR_UObjectHandle, void*, void*) {
    std::scoped_lock _{g_monitor_mtx};
    auto f = (sdk::UFunction*)fn;
    if (g_monitored_funcs.find(f) != g_monitored_funcs.end()) {
        ++g_func_call_counts[f];
    }
    return true;
}

bool is_func_monitored(sdk::UFunction* fn) {
    std::scoped_lock _{g_monitor_mtx};
    return g_monitored_funcs.find(fn) != g_monitored_funcs.end();
}

uint64_t func_call_count(sdk::UFunction* fn) {
    std::scoped_lock _{g_monitor_mtx};
    auto it = g_func_call_counts.find(fn);
    return it != g_func_call_counts.end() ? it->second : 0;
}

void set_func_monitored(sdk::UFunction* fn, bool on) {
    if (on) {
        PluginLoader::get()->hook_ufunction_ptr((UEVR_UFunctionHandle)fn, nullptr, &uobjecthook_monitor_post);
        std::scoped_lock _{g_monitor_mtx};
        g_monitored_funcs.insert(fn);
    } else {
        std::scoped_lock _{g_monitor_mtx};
        g_monitored_funcs.erase(fn);
    }
    dispatch_function_monitor_event(fn, on, "monitor");
}

// Fires "uobjecthook_function_monitor" to Lua whenever a function's hooked/monitored/blocked state
// changes — from the UI (Block/Monitor toggle in ui_function_context_menu, via set_func_blocked/
// set_func_monitored above) OR from ANY external hook_ptr usage (a Lua script's fn:hook_ptr(...), or a
// native plugin) via the per-frame sync in UObjectHook::sync_hooked_functions_to_lua(). `source`
// documents which path triggered this ("block", "monitor", or "external_hook") so a script can tell
// UI-driven monitoring apart from its own hook. Caller info (top_classes/top_callers) comes from
// m_called_functions' per-receiver tallies — process_event_hook populates these for EVERY function it
// records regardless of how that function came to be hooked, as long as the global ProcessEvent
// listener is on.
// JSON payload: {"address":hex,"full_name","added":bool,"blocked":bool,"monitored":bool,
//   "call_count":N,"source","top_classes":[{"name","count"}],"top_callers":[{"address","full_name","count"}]}
void dispatch_function_monitor_event(sdk::UFunction* fn, bool added, const char* source) {
    if (fn == nullptr) {
        return;
    }
    try {
        char addr_buf[24];
        std::snprintf(addr_buf, sizeof(addr_buf), "%llx", (unsigned long long)(uintptr_t)fn);
        std::string full_name;
        try { full_name = utility::narrow(fn->get_full_name()); } catch (...) {}

        uint64_t call_count = 0;
        nlohmann::json top_classes = nlohmann::json::array();
        nlohmann::json top_callers = nlohmann::json::array();
        {
            auto hook = UObjectHook::get();
            std::scoped_lock _{hook->m_function_mutex};
            if (auto it = hook->m_called_functions.find(fn); it != hook->m_called_functions.end()) {
                call_count = it->second.call_count;

                std::vector<std::pair<sdk::UClass*, size_t>> classes(
                    it->second.caller_class_counts.begin(), it->second.caller_class_counts.end());
                std::sort(classes.begin(), classes.end(), [](auto& a, auto& b) { return a.second > b.second; });
                for (size_t i = 0; i < classes.size() && i < 5; ++i) {
                    std::string cname;
                    try { cname = utility::narrow(classes[i].first->get_fname().to_string()); } catch (...) { continue; }
                    top_classes.push_back({{"name", cname}, {"count", classes[i].second}});
                }

                std::vector<std::pair<sdk::UObject*, size_t>> callers(
                    it->second.caller_instance_counts.begin(), it->second.caller_instance_counts.end());
                std::sort(callers.begin(), callers.end(), [](auto& a, auto& b) { return a.second > b.second; });
                for (size_t i = 0; i < callers.size() && i < 5; ++i) {
                    auto* obj = callers[i].first;
                    if (!hook->exists((sdk::UObjectBase*)obj)) continue;
                    char caddr[24];
                    std::snprintf(caddr, sizeof(caddr), "%llx", (unsigned long long)(uintptr_t)obj);
                    std::string oname;
                    try { oname = utility::narrow(obj->get_full_name()); } catch (...) {}
                    top_callers.push_back({{"address", caddr}, {"full_name", oname}, {"count", callers[i].second}});
                }
            }
        }

        const nlohmann::json payload{
            {"address", addr_buf},
            {"full_name", full_name},
            {"added", added},
            {"blocked", is_func_blocked(fn)},
            {"monitored", is_func_monitored(fn)},
            {"call_count", call_count},
            {"source", source},
            {"top_classes", top_classes},
            {"top_callers", top_callers}
        };
        const auto data = payload.dump();
        LuaLoader::get()->dispatch_event("uobjecthook_function_monitor", data);
    } catch (const std::exception& e) {
        spdlog::error("[UObjectHook] dispatch_function_monitor_event failed: {}", e.what());
    } catch (...) {
        spdlog::error("[UObjectHook] dispatch_function_monitor_event failed");
    }
}
} // namespace

// Called once per frame from on_frame(). Diffs PluginLoader::get_hooked_functions() (the single choke
// point ANY hook_ptr call goes through — Lua's fn:hook_ptr(...), a native plugin, or UObjectHook's own
// Block/Monitor) against what's already been announced, and dispatches "uobjecthook_function_monitor"
// for anything new (source = "monitor"/"block" if the UI put it there, else "external_hook") or
// removed. This is what makes the Lua-visible pool bidirectional: a script's own fn:hook_ptr() call is
// picked up here exactly the same way a UI Block/Monitor toggle is (that path ALSO dispatches
// immediately from set_func_blocked/set_func_monitored — this loop is a safety net that additionally
// covers hooks the UI never touched).
void UObjectHook::sync_hooked_functions_to_lua() {
    auto current_vec = PluginLoader::get()->get_hooked_functions();
    std::unordered_set<sdk::UFunction*> current{current_vec.begin(), current_vec.end()};

    for (auto* fn : current) {
        if (fn == nullptr || m_known_hooked_funcs.contains(fn)) {
            continue;
        }
        const char* source = is_func_blocked(fn) ? "block" : (is_func_monitored(fn) ? "monitor" : "external_hook");
        dispatch_function_monitor_event(fn, true, source);
    }
    for (auto* fn : m_known_hooked_funcs) {
        if (fn != nullptr && !current.contains(fn)) {
            dispatch_function_monitor_event(fn, false, "unhooked");
        }
    }

    m_known_hooked_funcs = std::move(current);
}

void UObjectHook::draw_active_function_hooks() {
    // Snapshot under the locks, then render (don't hold a lock across ImGui).
    std::vector<sdk::UFunction*> blocked;
    std::vector<std::pair<sdk::UFunction*, uint64_t>> monitored;
    {
        std::scoped_lock _{g_blocked_funcs_mtx};
        blocked.assign(g_blocked_funcs.begin(), g_blocked_funcs.end());
    }
    {
        std::scoped_lock _{g_monitor_mtx};
        for (auto* f : g_monitored_funcs) {
            auto it = g_func_call_counts.find(f);
            monitored.emplace_back(f, it != g_func_call_counts.end() ? it->second : 0);
        }
    }

    if (blocked.empty() && monitored.empty()) {
        ImGui::TextDisabled("No blocked or monitored functions.");
        return;
    }

    const auto name_of = [](sdk::UFunction* f) -> std::string {
        try { return utility::narrow(f->get_full_name()); } catch (...) { return std::format("[{:#x}]", (uintptr_t)f); }
    };

    if (!blocked.empty()) {
        ImGui::SeparatorText("Blocked (no-op'd)");
        for (auto* f : blocked) {
            ImGui::PushID((void*)f);
            if (ImGui::SmallButton("Unblock")) {
                set_func_blocked(f, false);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(name_of(f).c_str());
            ImGui::PopID();
        }
    }

    if (!monitored.empty()) {
        ImGui::SeparatorText("Monitored");
        if (ImGui::SmallButton("Reset all counts")) {
            std::scoped_lock _{g_monitor_mtx};
            g_func_call_counts.clear();
        }
        for (auto& [f, count] : monitored) {
            ImGui::PushID((void*)f);
            if (ImGui::SmallButton("Stop")) {
                set_func_monitored(f, false);
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "%llu", (unsigned long long)count);
            ImGui::SameLine();
            ImGui::TextUnformatted(name_of(f).c_str());
            ImGui::PopID();
        }
    }
}

void UObjectHook::ui_function_context_menu(sdk::UFunction* func, void* object, bool is_real_object) {
    if (func == nullptr || !ImGui::BeginPopupContextItem()) {
        return;
    }

    const auto set_clipboard = [](const std::string& text) {
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            HGLOBAL hcd = GlobalAlloc(GMEM_DDESHARE, text.size() + 1);
            char* data = (char*)GlobalLock(hcd);
            strcpy(data, text.c_str());
            GlobalUnlock(hcd);
            SetClipboardData(CF_TEXT, hcd);
            CloseClipboard();
        }
    };

    if (ImGui::MenuItem("Copy Name")) {
        try { set_clipboard(utility::narrow(func->get_full_name())); } catch (...) {}
    }
    if (ImGui::MenuItem("Copy Address")) {
        set_clipboard((std::stringstream{} << std::hex << (uintptr_t)func).str());
    }

    ImGui::Separator();

    bool blocked = is_func_blocked(func);
    if (ImGui::MenuItem("Block execution", nullptr, &blocked)) {
        set_func_blocked(func, blocked);
    }

    bool monitored = is_func_monitored(func);
    if (ImGui::MenuItem("Monitor calls", nullptr, &monitored)) {
        set_func_monitored(func, monitored);
    }
    if (monitored) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "(%llu)", (unsigned long long)func_call_count(func));
    }

    if (is_real_object && object != nullptr) {
        ImGui::Separator();
        if (ImGui::MenuItem("Send to Function Caller")) {
            load_live_caller_slot((sdk::UObject*)object, func);
            m_show_function_caller = true;
        }
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Function flags")) {
        static const std::pair<const char*, uint32_t> kFuncFlags[] = {
            {"Final", 0x1u}, {"RequiredAPI", 0x2u}, {"BlueprintAuthorityOnly", 0x4u}, {"BlueprintCosmetic", 0x8u},
            {"Net", 0x40u}, {"NetReliable", 0x80u}, {"NetRequest", 0x100u}, {"Exec", 0x200u}, {"Native", 0x400u},
            {"Event", 0x800u}, {"NetResponse", 0x1000u}, {"Static", 0x2000u}, {"NetMulticast", 0x4000u},
            {"UbergraphFunction", 0x8000u}, {"MulticastDelegate", 0x10000u}, {"Public", 0x20000u},
            {"Private", 0x40000u}, {"Protected", 0x80000u}, {"Delegate", 0x100000u}, {"NetServer", 0x200000u},
            {"HasOutParms", 0x400000u}, {"HasDefaults", 0x800000u}, {"NetClient", 0x1000000u}, {"DLLImport", 0x2000000u},
            {"BlueprintCallable", 0x4000000u}, {"BlueprintEvent", 0x8000000u}, {"BlueprintPure", 0x10000000u},
            {"EditorOnly", 0x20000000u}, {"Const", 0x40000000u}, {"NetValidate", 0x80000000u},
        };
        auto& flags = func->get_function_flags();
        for (auto& [fname, bit] : kFuncFlags) {
            bool set = (flags & bit) != 0;
            if (ImGui::Checkbox(fname, &set)) {
                if (set) { flags |= bit; } else { flags &= ~bit; }
            }
        }
        ImGui::EndMenu();
    }

    ImGui::EndPopup();
}

void UObjectHook::ui_handle_functions(void* object, sdk::UStruct* uclass) {
    if (uclass == nullptr) {
        return;
    }

    const bool is_real_object = object != nullptr && m_objects.contains((sdk::UObject*)object);
    auto object_real = (sdk::UObject*)object;

    static const auto ufunction_t = sdk::UFunction::static_class();

    // Name filter — function lists are often huge; let the user narrow them
    // (case-insensitive substring) to find the function they want to call/hook.
    static char s_func_filter[64] = "";
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##func_filter", "filter functions by name...", s_func_filter, sizeof(s_func_filter));
    // Group by class: one TreeNode per declaring class along the super chain
    // (e.g. Actor -> K2_GetActorRotation, FPSPlayer -> CustomGameFunction)
    // instead of one flat alphabetical list of the whole inheritance. On its own row so it doesn't
    // share the line with the (now full-width) filter box.
    static bool s_group_by_class = true; // default on: functions grouped by declaring class
    ImGui::Checkbox("Group by class", &s_group_by_class);

    std::string filter_lc = s_func_filter;
    std::transform(filter_lc.begin(), filter_lc.end(), filter_lc.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    auto passes_filter = [&](sdk::UFunction* func) -> bool {
        if (filter_lc.empty()) {
            return true;
        }
        auto nm = utility::narrow(func->get_fname().to_string());
        std::transform(nm.begin(), nm.end(), nm.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return nm.find(filter_lc) != std::string::npos;
    };

    auto render_one_function = [&](sdk::UFunction* func) {
        ImGui::PushID((void*)func);

        utility::ScopeGuard pop_guard{[]() { ImGui::PopID(); }};

        const bool node_open = ImGui::TreeNode(utility::narrow(func->get_fname().to_string()).data());
        ui_function_context_menu(func, object, is_real_object);

        if (m_called_functions.contains(func)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "[Called]");
        }
        if (is_func_blocked(func)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{1.0f, 0.5f, 0.0f, 1.0f}, "[Blocked]");
        }
        if (is_func_monitored(func)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{0.4f, 0.8f, 1.0f, 1.0f}, "[Mon %llu]", (unsigned long long)func_call_count(func));
        }

        if (node_open) {
            if (is_real_object) {
                // Show parameter signature (type + name + [Out]/[struct-name]) so
                // the user can tell what the editors below will be writing into.
                auto parameters = func->get_child_properties();
                for (auto param = parameters; param != nullptr; param = param->get_next()) {
                    const auto cname = utility::narrow(param->get_class()->get_name().to_string());
                    ImGui::TextDisabled("%s %s", cname.data(), utility::narrow(param->get_field_name().to_string()).data());

                    if (cname.contains("Property")) {
                        const auto prop = (sdk::FProperty*)param;
                        if (prop->is_out_param()) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "[Out]");
                        }
                        if (cname == "StructProperty") {
                            if (const auto sp = (sdk::FStructProperty*)param; sp != nullptr) {
                                if (const auto strukt = sp->get_struct(); strukt != nullptr) {
                                    const auto struct_name = utility::narrow(strukt->get_full_name());
                                    ImGui::SameLine();
                                    ImGui::TextColored(ImVec4{78.0f/255.0f, 201.0f/255.0f, 176.0f/255.0f, 1.0f}, "[%s]", struct_name.data());
                                }
                            }
                        }
                    }
                }

                ImGui::Separator();
                // Interactive editor + Call button + return-value display.
                render_function_call(object_real, func);
            } else {
                ImGui::TextDisabled("Right-click for hooks / flags. Drop into a Function Caller slot to call.");
            }

            ImGui::TreePop();
        }
    };

    auto collect_direct = [&](sdk::UStruct* strukt, std::vector<sdk::UFunction*>& out) {
        for (auto child = strukt->get_children(); child != nullptr; child = child->get_next()) {
            if (child->get_class()->is_a(ufunction_t)) {
                out.push_back((sdk::UFunction*)child);
            }
        }
    };
    auto sort_by_name = [](std::vector<sdk::UFunction*>& v) {
        std::sort(v.begin(), v.end(),
            [](sdk::UFunction* a, sdk::UFunction* b) { return a->get_fname().to_string() < b->get_fname().to_string(); });
    };

    if (s_group_by_class) {
        // Most-derived class first (the class actually being inspected), then up
        // the chain. Each class's own functions live under its TreeNode.
        for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
            std::vector<sdk::UFunction*> funcs{};
            collect_direct(super, funcs);

            if (!filter_lc.empty()) {
                std::erase_if(funcs, [&](sdk::UFunction* f) { return !passes_filter(f); });
            }
            if (funcs.empty()) {
                continue;
            }
            sort_by_name(funcs);

            ImGui::PushID((void*)super);
            utility::ScopeGuard pop_guard{[]() { ImGui::PopID(); }};

            const auto cls_name = utility::narrow(super->get_fname().to_string());
            // Folded by default (even the leaf/declaring class) so a long inheritance chain doesn't
            // dump every function open at once — matches the folded-by-default convention across
            // every UObjectHook menu/tree.
            if (ImGui::TreeNodeEx((void*)super, ImGuiTreeNodeFlags_None, "%s (%zu)", cls_name.c_str(), funcs.size())) {
                for (auto func : funcs) {
                    render_one_function(func);
                }
                ImGui::TreePop();
            }
        }
    } else {
        std::vector<sdk::UFunction*> sorted_functions{};
        for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
            collect_direct(super, sorted_functions);
        }
        sort_by_name(sorted_functions);

        for (auto func : sorted_functions) {
            if (!passes_filter(func)) {
                continue;
            }
            render_one_function(func);
        }
    }
}

// Game-thread only — call BEFORE the actual destroy_component()/destroy_actor(), so nothing else
// touches the pointer again once the engine call runs. `object` may be a component or an actor;
// gizmo/MC-attachment cleanup only applies when it's a USceneComponent (attaching/gizmo-ing an actor
// always goes through its root component elsewhere in this file), but the recent-objects/spawned-list/
// camera-attach cleanup applies to any UObject.
void UObjectHook::cleanup_references_to(sdk::UObjectBase* object) {
    if (object == nullptr) {
        return;
    }
    auto* obj = (sdk::UObject*)object;
    auto* as_comp = reinterpret_cast<sdk::USceneComponent*>(object);

    bool gizmo_removed = false;
    {
        std::unique_lock _{m_mutex};
        gizmo_removed = m_gizmo_components.erase(as_comp) > 0;
        m_motion_controller_attached_components.erase(as_comp);
        if (m_last_selected == as_comp) {
            m_last_selected = nullptr;
        }
        std::erase(m_spawned_via_panel, object);
        std::erase(m_most_recent_objects, obj);
    }
    if (gizmo_removed) {
        dispatch_gizmo_target_event(as_comp, false); // outside the lock — dispatch can re-enter
    }
    if (m_camera_attach.object == obj) {
        m_camera_attach.object = nullptr;
        m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};
        if (m_persistent_camera_state != nullptr) {
            m_persistent_camera_state->erase_json_file();
        }
        m_persistent_camera_state.reset();
    }
}

// See m_property_edit_target_stack in the header. Called after any property widget in
// ui_handle_properties reports a real edit; forces the nearest enclosing scene component to actually
// re-render by toggling SetVisibility to its own current value (deferred to the game thread — this is
// a ProcessEvent call, not safe to make directly from the draw thread).
void UObjectHook::notify_property_changed() {
    if (m_property_edit_target_stack.empty()) {
        return;
    }
    auto* comp = m_property_edit_target_stack.back();
    if (comp == nullptr) {
        return;
    }
    GameThreadWorker::get().enqueue([this, comp]() {
        if (!this->exists(comp)) return;
        try {
            const bool vis = comp->is_visible();
            comp->set_visibility(vis, false);
        } catch (...) {}
    });
}

void UObjectHook::ui_handle_properties(void* object, sdk::UStruct* uclass) {

    auto previous_path = m_path;

    // One consolidated right-click menu per property row. The scalar/struct cases draw their own
    // popup (Save/Unsave + Edit flags); this flag tells the generic Edit-flags popup at the end of
    // the switch to stand down for those rows. Without it, that popup re-opens on the same
    // right-click and the Save button is never visible — the long-standing "only Edit flags shows,
    // Save never appears" bug that made the save feature look unimplemented.
    bool row_ctx_drawn = false;

    // Resolve a savable, re-resolvable path for `object`: prefer the live navigation path when it
    // starts from an allowed base, else brute-force one with try_get_path (the same fallback the
    // "Save Visibility State" button already uses). nullopt => no allowed base can reach the object,
    // so the save is refused with a hint instead of a hard-gated empty menu.
    // A save target is EITHER a base-relative path (preferred — survives address changes via the
    // live walk) or, when no allowed base can reach the object, its stable full-name locator. The
    // locator drops the navigation-base requirement so arbitrary inspected / click-selected objects
    // (e.g. world actors) become saveable; reapply re-resolves it via sdk::find_uobject.
    struct SaveTarget {
        std::vector<std::string> path{};
        std::wstring locator{};
        bool ok() const { return !path.empty() || !locator.empty(); }
    };
    auto resolve_save_target = [this, &previous_path, object]() -> SaveTarget {
        if (previous_path.has_valid_base()) {
            return SaveTarget{ previous_path.path(), {} };
        }
        // Only treat `object` as a UObject when it's actually registered. When ui_handle_properties
        // is recursed into a struct, `object` is a raw struct-inner pointer that is NOT in m_objects;
        // try_get_path / get_full_name dereference it as a UObject. Wrap in try/catch: the FName path
        // can throw and we are inside an open ImGui popup, where an escaping exception skips EndPopup.
        if (object != nullptr && exists_unsafe(reinterpret_cast<sdk::UObjectBase*>(object))) {
            auto* o = reinterpret_cast<sdk::UObject*>(object);
            try {
                if (auto p = try_get_path(o); p.has_value()) {
                    return SaveTarget{ p->path(), {} };
                }
            } catch (...) {}
            try {
                if (auto fn = o->get_full_name(); !fn.empty()) {
                    return SaveTarget{ {}, fn };
                }
            } catch (...) {}
        }
        return SaveTarget{};
    };

    auto scope = m_path.enter("Properties");

    if (uclass == nullptr) {
        return;
    }


static const std::map<std::string_view, uint64_t> EPropertyFlags = {
    {"CPF_None",  0x0000000000000000},
    {"CPF_Edit",                                0x0000000000000001},
    {"CPF_ConstParm",                           0x0000000000000002},
    {"CPF_BlueprintVisible",                    0x0000000000000004},
    {"CPF_ExportObject",                        0x0000000000000008},
    {"CPF_BlueprintReadOnly",                   0x0000000000000010},
    {"CPF_Net",                                 0x0000000000000020},
    {"CPF_EditFixedSize",                       0x0000000000000040},
    {"CPF_Parm",                                0x0000000000000080},
    {"CPF_OutParm",                             0x0000000000000100},
    {"CPF_ZeroConstructor",                     0x0000000000000200},
    {"CPF_ReturnParm",                          0x0000000000000400},
    {"CPF_DisableEditOnTemplate",               0x0000000000000800},
    {"CPF_Transient",                           0x0000000000002000},
    {"CPF_Config",                              0x0000000000004000},
    {"CPF_DisableEditOnInstance",               0x0000000000010000},
    {"CPF_EditConst",                           0x0000000000020000},
    {"CPF_GlobalConfig",                        0x0000000000040000},
    {"CPF_InstancedReference",                  0x0000000000080000},
    {"CPF_DuplicateTransient",                  0x0000000000200000},
    {"CPF_SubobjectReference",                  0x0000000000400000},
    {"CPF_SaveGame",                            0x0000000001000000},
    {"CPF_NoClear",                             0x0000000002000000},
    {"PF_ReferenceParm",                        0x0000000008000000},
    {"PF_BlueprintAssignable",                  0x0000000010000000},
    {"PF_Deprecated",                           0x0000000020000000},
    {"PF_IsPlainOldData",                       0x0000000040000000},
    {"PF_RepSkip",                              0x0000000080000000},
    {"PF_RepNotify",                            0x0000000100000000},
    {"PF_Interp",                               0x0000000200000000},
    {"PF_NonTransactional",                     0x0000000400000000},
    {"PF_EditorOnly",                           0x0000000800000000},
    {"PF_NoDestructor",                         0x0000001000000000},
    {"CPF_AutoWeak",                            0x0000004000000000},
    {"CPF_ContainsInstancedReference",          0x0000008000000000},
    {"CPF_AssetRegistrySearchable",             0x0000010000000000},
    {"CPF_SimpleDisplay",                       0x0000020000000000},
    {"CPF_AdvancedDisplay",                     0x0000040000000000},
    {"CPF_Protected",                           0x0000080000000000},
    {"CPF_BlueprintCallable",                   0x0000100000000000},
    {"CPF_BlueprintAuthorityOnly",              0x0000200000000000},
    {"CPF_TextExportTransient",                 0x0000400000000000},
    {"CPF_NonPIEDuplicateTransient",            0x0000800000000000},
    {"CPF_ExposeOnSpawn",                       0x0001000000000000},
    {"CPF_PersistentInstance",                  0x0002000000000000},
    {"CPF_UObjectWrapper",                      0x0004000000000000},
    {"CPF_HasGetValueTypeHash",                 0x0008000000000000},
    {"CPF_NativeAccessSpecifierPublic",         0x0010000000000000},
    {"CPF_NativeAccessSpecifierProtected",      0x0020000000000000},
    {"CPF_NativeAccessSpecifierPrivate",        0x0040000000000000},
    {"CPF_SkipSerialization",                   0x0080000000000000}
};

const auto check_flags = [](uint64_t flags){
    std::map<std::string_view, bool> all_flags{};
    for (auto& flag : EPropertyFlags) {
        all_flags[flag.first] = (flags & (flag.second)) != 0;
    }
    return all_flags;
};


    const bool is_real_object = object != nullptr && m_objects.contains((sdk::UObject*)object);

    // Property list controls: filter by property type, and optionally group properties by
    // the class that declares them or by their property type. One inspector is shown at a
    // time, so function-local statics for the control state are fine.
    static char s_prop_name_filter[64]{};   // search properties by NAME (substring)
    static int s_prop_type_idx = 0;          // 0 = All, else index into type_list
    static int s_prop_base_idx = 0;          // 0 = All, else index into base_list
    static int s_prop_group_mode = 1;        // 0 = flat, 1 = by base class (default on), 2 = by type

    // Reset the type/base dropdown selections when switching to a different object — the saved
    // index would otherwise point at a different type/class in the new object's lists.
    static sdk::UStruct* s_last_props_uclass = nullptr;
    if (uclass != s_last_props_uclass) {
        s_last_props_uclass = uclass;
        s_prop_type_idx = 0;
        s_prop_base_idx = 0;
    }

    // Tree connector lines on by default (imgui 1.92) for the inspector's trees.
    ImGui::GetStyle().TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesFull;

    // Collect (property, declaring struct) so grouping/filtering can key off either.
    struct FieldEntry { sdk::FField* prop; sdk::UStruct* decl; };
    std::vector<FieldEntry> sorted_fields{};

    // Depth of each declaring class in the inheritance chain, 0 = uclass itself (most derived),
    // increasing toward the root (UObject). Lets "By base class" grouping sort base-to-derived
    // instead of alphabetically, matching the actual hierarchy instead of coincidental name order.
    std::unordered_map<sdk::UStruct*, int> class_depth{};
    {
        int depth = 0;
        for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
            class_depth[super] = depth++;
        }
    }

    for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
        auto props = super->get_child_properties();

        for (auto prop = props; prop != nullptr; prop = prop->get_next()) {
            sorted_fields.push_back(FieldEntry{prop, super});
        }
    }

    // Build the distinct type + base-class lists for the filter dropdowns (index 0 = "All").
    std::vector<std::string> type_list{"All"};
    std::vector<std::string> base_list{"All"};
    for (const auto& e : sorted_fields) {
        std::string t, b;
        try { t = utility::narrow(e.prop->get_class()->get_name().to_string()); } catch (...) {}
        try { b = utility::narrow(e.decl->get_fname().to_string()); } catch (...) {}
        if (!t.empty() && std::find(type_list.begin(), type_list.end(), t) == type_list.end()) type_list.push_back(t);
        if (!b.empty() && std::find(base_list.begin(), base_list.end(), b) == base_list.end()) base_list.push_back(b);
    }
    std::sort(type_list.begin() + 1, type_list.end());
    std::sort(base_list.begin() + 1, base_list.end());
    if (s_prop_type_idx >= (int)type_list.size()) s_prop_type_idx = 0;
    if (s_prop_base_idx >= (int)base_list.size()) s_prop_base_idx = 0;

    // Controls laid out on TWO rows so they don't crowd each other or overflow a narrow window:
    //   Row 1: property-name search (fills the row).
    //   Row 2: type filter + base-class filter + group mode + the texture-preview stub.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##propnamefilter", "search properties...", s_prop_name_filter, sizeof(s_prop_name_filter));
    {
        std::vector<const char*> items; items.reserve(type_list.size());
        for (const auto& s : type_list) items.push_back(s.c_str());
        ImGui::SetNextItemWidth(130.0f);
        ImGui::Combo("type##proptype", &s_prop_type_idx, items.data(), (int)items.size());
    }
    ImGui::SameLine();
    {
        std::vector<const char*> items; items.reserve(base_list.size());
        for (const auto& s : base_list) items.push_back(s.c_str());
        ImGui::SetNextItemWidth(140.0f);
        ImGui::Combo("base##propbase", &s_prop_base_idx, items.data(), (int)items.size());
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("group##propgroup", &s_prop_group_mode, "Flat\0By base class\0By type\0");
    const bool is_d3d11 = g_framework->get_renderer_type() == Framework::RendererType::D3D11;
    if (is_d3d11) {
        ImGui::SameLine();
        ImGui::Checkbox("tex##stub", &m_show_texture_previews); // STUB, default off (crash-prone)
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Texture previews (D3D11, experimental — crash-prone; default off)");
        }
    }
    if (is_d3d11 && m_show_texture_previews && object != nullptr && uclass != nullptr) {
        std::string tex_cn;
        try { tex_cn = utility::narrow(uclass->get_fname().to_string()); } catch (...) {}
        if (tex_cn.find("Texture") != std::string::npos) {
            draw_texture_preview((sdk::UObject*)object);
        }
    }

    const std::string sel_type = (s_prop_type_idx > 0 && s_prop_type_idx < (int)type_list.size()) ? type_list[s_prop_type_idx] : std::string{};
    const std::string sel_base = (s_prop_base_idx > 0 && s_prop_base_idx < (int)base_list.size()) ? base_list[s_prop_base_idx] : std::string{};

    const auto group_key_of = [&](const FieldEntry& e) -> std::wstring {
        if (s_prop_group_mode == 1) return e.decl->get_fname().to_string();
        if (s_prop_group_mode == 2) return e.prop->get_class()->get_name().to_string();
        return std::wstring{};
    };

    // Sort by group key first (when grouping), then by field name. "By base class" groups sort by
    // inheritance depth (root/base first, most-derived last) instead of alphabetically -- class_depth
    // only covers uclass's own chain, so a group key with no depth entry (shouldn't happen since decl
    // is always one of uclass's supers) falls back to alphabetical via the -1 default.
    std::sort(sorted_fields.begin(), sorted_fields.end(), [&](const FieldEntry& a, const FieldEntry& b) {
        if (s_prop_group_mode == 1) {
            const auto it_a = class_depth.find(a.decl);
            const auto it_b = class_depth.find(b.decl);
            const int da = it_a != class_depth.end() ? it_a->second : -1;
            const int db = it_b != class_depth.end() ? it_b->second : -1;
            if (da != db) return da > db; // higher depth = more base = first
        } else if (s_prop_group_mode != 0) {
            const auto ga = group_key_of(a), gb = group_key_of(b);
            if (ga != gb) return ga < gb;
        }
        return a.prop->get_field_name().to_string() < b.prop->get_field_name().to_string();
    });

    std::wstring cur_group_key{};
    bool cur_group_open = true;
    bool any_group_started = false;

    // Cap property-editor width so inherited-object property rows stay in a readable column even when
    // the UObjectHook window is dragged wide (this replaces the earlier whole-window width cap).
    const bool clamp_item_width = m_inspector_item_width > 0.0f;
    if (clamp_item_width) {
        ImGui::PushItemWidth(m_inspector_item_width);
    }
    // RAII pop: the property loop below has lambda-local returns and can surface exceptions from
    // reflection calls; a manual pop after the loop would be skipped on an uncaught throw and corrupt
    // ImGui's item-width stack. Guard pops on every exit. Nothing is drawn after the loop, so popping
    // at function scope is equivalent to popping right after it.
    utility::ScopeGuard item_width_pop{[clamp_item_width]() { if (clamp_item_width) { ImGui::PopItemWidth(); } }};

    for (auto& entry : sorted_fields) {
        auto prop = entry.prop;
        auto decl = entry.decl;
        auto propc = prop->get_class();
        const auto propc_type = propc->get_name().to_string();

        // Filters combine (AND): property-type dropdown, base-class dropdown, name search.
        if (!sel_type.empty() && utility::narrow(propc_type) != sel_type) {
            continue;
        }
        if (!sel_base.empty()) {
            std::string bn;
            try { bn = utility::narrow(decl->get_fname().to_string()); } catch (...) {}
            if (bn != sel_base) {
                continue;
            }
        }
        if (s_prop_name_filter[0] != '\0') {
            std::string hay = utility::narrow(prop->get_field_name().to_string());
            std::string needle = s_prop_name_filter;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c){ return (char)::tolower(c); });
            std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c){ return (char)::tolower(c); });
            if (hay.find(needle) == std::string::npos) {
                continue;
            }
        }

        // Group header: emit a collapsing header whenever the group key changes; skip the
        // group's properties while it is collapsed. Default OPEN — a by-class/by-type group is
        // just the object's OWN fields bucketed for readability, not a pointer to another object,
        // so it counts as "sensibly grouped data" and should read uncollapsed. Any individual field
        // that IS an object reference still gets its own separately-gated (collapsed) TreeNode
        // further down regardless of this group's state.
        if (s_prop_group_mode != 0) {
            const std::wstring key = (s_prop_group_mode == 1) ? decl->get_fname().to_string() : propc_type;
            if (!any_group_started || key != cur_group_key) {
                cur_group_key = key;
                any_group_started = true;
                cur_group_open = ImGui::CollapsingHeader((utility::narrow(key) + "##propgrp").c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            }
            if (!cur_group_open) {
                continue;
            }
        }

        if (object == nullptr) {
            const auto name = utility::narrow(propc->get_name().to_string());
            const auto field_name = utility::narrow(prop->get_field_name().to_string());
            ImGui::Text("%s %s", name.data(), field_name.data());
            continue;
        }

        auto hash_type = utility::hash(propc_type);

        if (hash_type == L"EnumProperty"_fnv) {
            auto enum_prop = (sdk::FEnumProperty*)prop;
            if (auto numeric_prop = (sdk::FProperty*)enum_prop->get_underlying_prop(); numeric_prop != nullptr) try {
                if (auto nc = numeric_prop->get_class(); nc != nullptr) {
                    const auto nc_name = nc->get_name().to_string_no_numbers(); // Calling this variant so we don't cause a crash if the name is bad.

                    // This check is just in-case we get handed back some bad memory.
                    if (nc_name.contains(L"Property")) {
                        hash_type = utility::hash(nc_name);
                    }
                }
            } catch (...) {
                // Can happen because we haven't rigorously mapped out FEnumProperty yet for older UE versions.
            }
        }

        const auto edit_property_flags = [&](std::string _prop_name, sdk::FProperty* _fprop) {
            auto flags = _fprop->get_property_flags();
            auto current = check_flags(flags);
            for (auto& [flag_name, is_set] : current) {
                ImGui::BulletText("%s", flag_name.data());
                ImGui::SameLine();
                // Previous version did `auto enabled = &_prop.second; if (enabled)` which always
                // evaluated true (it tested the address, not the value), so the checkbox could
                // only ever set bits, never clear them. Also an unused `auto new_flags = flags;`
                // that's been deleted.
                if (ImGui::Checkbox((std::string{"##"} + _prop_name + std::string{flag_name}).c_str(), &is_set)) {
                    for (auto&& [enum_name, bit] : EPropertyFlags) {
                        if (enum_name == flag_name) {
                            if (is_set) {
                                flags |= (uint64_t)bit;
                            } else {
                                flags &= ~(uint64_t)bit;
                            }
                            break;
                        }
                    }
                    _fprop->get_property_flags() = flags;
                }
            }
        };
        // Right-click lambda for supported scalar properties: Save/Unsave + Edit flags in ONE popup.
        auto display_context = [&](auto value) {
            row_ctx_drawn = true; // this row owns its context popup; suppress the generic one below
            if (!ImGui::BeginPopupContextItem()) {
                return;
            }

            const auto target = resolve_save_target();
            if (!target.ok()) {
                ImGui::TextDisabled("Can't save: object has no stable identity");
            } else {
                auto save_logic = [&](bool unsave = false) {
                    std::shared_ptr<PersistentProperties> props{};

                    // Find existing one if possible (path- OR locator-resolved)
                    for (const auto& existing_prop : m_persistent_properties) {
                        if (resolve_persistent_target(*existing_prop, /*use_cooldown*/ false) == object) {
                            props = existing_prop;
                            break;
                        }
                    }

                    // Add new one if necessary
                    if (props == nullptr) {
                        props = std::make_shared<PersistentProperties>();
                        if (!target.path.empty()) {
                            props->path = StatePath{target.path};
                        } else {
                            props->object_locator = target.locator;
                        }
                        m_persistent_properties.push_back(props);
                    }

                    // Add property to list if needed
                    std::shared_ptr<PersistentProperties::PropertyState> state{};

                    for (const auto& existing_state : props->properties) {
                        if (existing_state->name == prop->get_field_name().to_string()) {
                            state = existing_state;
                            break;
                        }
                    }

                    // Add new one if necessary
                    if (state == nullptr) {
                        state = std::make_shared<PersistentProperties::PropertyState>();
                        state->name = prop->get_field_name().to_string();
                        props->properties.push_back(state);
                    }

                    memcpy(&state->data, &value, sizeof(value));

                    if (unsave) {
                        props->properties.erase(
                            std::remove(props->properties.begin(), props->properties.end(), state),
                            props->properties.end()
                        );
                    }

                    // Key the file off the path (or the full-name locator when path-less) and hash it.
                    std::string concat_path{};
                    if (!target.path.empty()) {
                        for (const auto& p : target.path) {
                            concat_path += p;
                        }
                    } else {
                        concat_path = utility::narrow(target.locator);
                    }

                    const auto hash_str = std::to_string(utility::hash(concat_path)) + "_props.json";
                    auto wanted_path = UObjectHook::get_persistent_dir() / hash_str;

                    if (props->path_to_json.has_value()) {
                        wanted_path = props->path_to_json.value();
                    }

                    // Create dir if necessary
                    try {
                        std::filesystem::create_directories(wanted_path.parent_path());

                        if (props->properties.empty()) {
                            // Delete the file if it exists. Happens if we unsave.
                            if (std::filesystem::exists(wanted_path)) {
                                std::filesystem::remove(wanted_path);
                            }

                            // Delete the property entry from m_peristent_properties.
                            m_persistent_properties.erase(
                                std::remove(m_persistent_properties.begin(), m_persistent_properties.end(), props),
                                m_persistent_properties.end()
                            );

                            return;
                        }

                        props->save_to_file(wanted_path);
                    } catch (const std::exception& e) {
                        SPDLOG_ERROR("[UObjectHook] Failed to save persistent properties: {}", e.what());
                    } catch (...) {
                        SPDLOG_ERROR("[UObjectHook] Failed to save persistent properties");
                    }
                };

                if (ImGui::Button("Save Property")) {
                    save_logic();
                }

                if (ImGui::Button("Unsave Property")) {
                    save_logic(true);
                }
                ImGui::Separator();
            }

            if (ImGui::BeginMenu("Edit flags")) {
                edit_property_flags(utility::narrow(prop->get_field_name().to_string()), (sdk::FProperty*)prop);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        };

        // Struct-aware version (Vector/Rotator/Transform/etc.): the 8-byte union can't hold a
        // struct, so we snapshot the raw struct bytes into PropertyState::struct_bytes. Mirrors
        // display_context's save_logic. Also offers Edit flags so struct rows have a full menu.
        auto display_context_struct = [&](void* saddr, int32_t ssize) {
            row_ctx_drawn = true; // this row owns its context popup; suppress the generic one below
            // ui_try_known_struct opened "##known_struct_save" via OpenPopupOnItemClick on the value
            // widget under PushID(addr); replay the same id seed so this BeginPopup matches it and the
            // menu lands on the value row (not the trailing Copy button — the prior no-id binding bug).
            ImGui::PushID(saddr);
            utility::ScopeGuard struct_ctx_pop{[]() { ImGui::PopID(); }};
            if (!ImGui::BeginPopup("##known_struct_save")) {
                return;
            }
            const auto target = resolve_save_target();
            if (!target.ok()) {
                ImGui::TextDisabled("Can't save: object has no stable identity");
            } else {
                auto save_logic = [&](bool unsave = false) {
                std::shared_ptr<PersistentProperties> props{};
                for (const auto& ep : m_persistent_properties) {
                    if (resolve_persistent_target(*ep, /*use_cooldown*/ false) == object) { props = ep; break; }
                }
                if (props == nullptr) {
                    props = std::make_shared<PersistentProperties>();
                    if (!target.path.empty()) {
                        props->path = StatePath{target.path};
                    } else {
                        props->object_locator = target.locator;
                    }
                    m_persistent_properties.push_back(props);
                }
                std::shared_ptr<PersistentProperties::PropertyState> state{};
                for (const auto& es : props->properties) {
                    if (es->name == prop->get_field_name().to_string()) { state = es; break; }
                }
                if (state == nullptr) {
                    state = std::make_shared<PersistentProperties::PropertyState>();
                    state->name = prop->get_field_name().to_string();
                    props->properties.push_back(state);
                }
                const uint32_t n = std::min<uint32_t>((uint32_t)std::max(ssize, 0), (uint32_t)sizeof(state->struct_bytes));
                state->struct_size = n;
                if (saddr != nullptr && n > 0 && !IsBadReadPtr(saddr, n)) {
                    memcpy(state->struct_bytes, saddr, n);
                }
                if (unsave) {
                    props->properties.erase(std::remove(props->properties.begin(), props->properties.end(), state), props->properties.end());
                }
                std::string concat_path{};
                if (!target.path.empty()) { for (const auto& p : target.path) concat_path += p; }
                else { concat_path = utility::narrow(target.locator); }
                auto wanted_path = UObjectHook::get_persistent_dir() / (std::to_string(utility::hash(concat_path)) + "_props.json");
                if (props->path_to_json.has_value()) wanted_path = props->path_to_json.value();
                try {
                    std::filesystem::create_directories(wanted_path.parent_path());
                    if (props->properties.empty()) {
                        if (std::filesystem::exists(wanted_path)) std::filesystem::remove(wanted_path);
                        m_persistent_properties.erase(std::remove(m_persistent_properties.begin(), m_persistent_properties.end(), props), m_persistent_properties.end());
                        return;
                    }
                    props->save_to_file(wanted_path);
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("[UObjectHook] Failed to save persistent struct property: {}", e.what());
                } catch (...) {
                    SPDLOG_ERROR("[UObjectHook] Failed to save persistent struct property");
                }
            };
                if (ImGui::Button("Save Property")) { save_logic(); }
                if (ImGui::Button("Unsave Property")) { save_logic(true); }
                ImGui::Separator();
            }
            if (ImGui::BeginMenu("Edit flags")) {
                edit_property_flags(utility::narrow(prop->get_field_name().to_string()), (sdk::FProperty*)prop);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        };

        const auto& prop_name =  utility::narrow(prop->get_field_name().to_string());
        sdk::FProperty* fprop = ((sdk::FProperty*)prop);

        row_ctx_drawn = false; // reset per row; the cases that own a popup set it true

        switch (hash_type) {


        case L"FloatProperty"_fnv:
            {
                auto& value = *(float*)((uintptr_t)object + fprop->get_offset());
                if (ImGui::DragFloat(prop_name.data(), &value, 0.01f)) {
                    notify_property_changed();
                }
                display_context(value);
            }
            break;
        case L"DoubleProperty"_fnv:
            {
                auto& value = *(double*)((uintptr_t)object + fprop->get_offset());
                float casted_value = (float)value;
                if (ImGui::DragFloat(prop_name.data(), (float*)&casted_value, 0.01f)) {
                    value = (double)casted_value;
                    notify_property_changed();
                }
                display_context(value);
            }
            break;
        case L"UInt16Property"_fnv:
            {
                auto& value = *(uint16_t*)((uintptr_t)object + fprop->get_offset());
                int converted = (int)value;
                if (ImGui::SliderInt(prop_name.data(), (int*)&converted, 0, 65535)) {
                    value = (uint16_t)converted;
                    notify_property_changed();
                }
                display_context(value);
            }
            break;
        case L"UInt32Property"_fnv:
        case L"IntProperty"_fnv:
            {
                auto& value = *(int32_t*)((uintptr_t)object + fprop->get_offset());
                if (ImGui::DragInt(prop_name.data(), &value, 1)) {
                    notify_property_changed();
                }
                display_context(value);
            }
            break;
        case L"UInt64Property"_fnv:
            {
                auto& value = *(uint64_t*)((uintptr_t)object + fprop->get_offset());
                if (ImGui::DragScalar(prop_name.data(), ImGuiDataType_U64, &value, 1)) {
                    notify_property_changed();
                }
                display_context(value);
            }
            break;
        case L"BoolProperty"_fnv:
            {
                auto boolprop = (sdk::FBoolProperty*)prop;
                auto value = boolprop->get_value_from_object(object);
                if (ImGui::Checkbox(prop_name.data(), &value)) {
                    boolprop->set_value_in_object(object, value);
                    notify_property_changed();
                }
                display_context(value);
            }
            break;
        case L"ByteProperty"_fnv:
            {
                auto& value = *(uint8_t*)((uintptr_t)object + fprop->get_offset());
                int converted = (int)value;
                if (ImGui::SliderInt(prop_name.data(), &converted, 0, 255)) {
                    value = (uint8_t)converted;
                    notify_property_changed();
                }
                display_context(value);
            }
            break;
        case L"InterfaceProperty"_fnv:
        case L"ObjectProperty"_fnv:
        case L"ClassProperty"_fnv:
            {
                auto& value = *(sdk::UObject**)((uintptr_t)object + fprop->get_offset());

                const bool open = ImGui::TreeNode(prop_name.data());
                utility::ScopeGuard guard{[open]() { if (open) ImGui::TreePop(); }};
                if (open) {
                    auto scope2 = m_path.enter(prop_name);
                    try { ui_handle_object(value); }
                    catch (...) { ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<failed to display>"); }
                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::BeginMenu("Edit flags")) {
                            edit_property_flags(utility::narrow(prop->get_field_name().to_string()), (sdk::FProperty*)prop);
                            ImGui::EndMenu();
                        }
                        ImGui::EndPopup();
                    }
                }
            }
            break;
        case L"WeakObjectProperty"_fnv:
        case L"LazyObjectProperty"_fnv:
            {
                // Resolve FWeakObjectPtr (UE4/5: { int32 ObjectIndex; int32 ObjectSerialNumber; })
                // via FUObjectArray with serial validation. Stale → red text + index, so the
                // user can tell "ref was never set" from "ref pointed at a freed object".
                auto raw = (int32_t*)((uintptr_t)object + fprop->get_offset());
                const auto obj_index = raw[0];
                const auto serial = raw[1];
                sdk::UObject* resolved = nullptr;
                bool stale = false;
                if (obj_index > 0) {
                    if (auto item = sdk::FUObjectArray::get()->get_object(obj_index); item != nullptr) {
                        if (item->get_object() != nullptr && item->get_serial_number() == serial) {
                            resolved = (sdk::UObject*)item->get_object();
                        } else {
                            stale = true;
                        }
                    }
                }
                if (resolved != nullptr) {
                    const bool open = ImGui::TreeNode(prop_name.data());
                    utility::ScopeGuard guard{[open]() { if (open) ImGui::TreePop(); }};
                    if (open) {
                        auto scope2 = m_path.enter(prop_name);
                        try { ui_handle_object(resolved); }
                        catch (...) { ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<failed to display>"); }
                    }
                } else if (stale) {
                    ImGui::Text("%s: ", prop_name.data());
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(ImVec4{1.0f, 0.5f, 0.5f, 1.0f}, "<stale weak: idx=%d>", obj_index);
                } else {
                    ImGui::Text("%s: ", prop_name.data());
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextDisabled("nullptr (weak)");
                }
            }
            break;
        case L"SoftObjectProperty"_fnv:
        case L"SoftClassProperty"_fnv:
            {
                // FSoftObjectPtr = { FWeakObjectPtr (8B); FSoftObjectPath { FName (8B); FString (16B) } }
                auto base = (uintptr_t)object + fprop->get_offset();
                auto raw = (int32_t*)base;
                const auto obj_index = raw[0];
                const auto serial = raw[1];
                sdk::UObject* resolved = nullptr;
                if (obj_index > 0) {
                    if (auto item = sdk::FUObjectArray::get()->get_object(obj_index); item != nullptr) {
                        if (item->get_object() != nullptr && item->get_serial_number() == serial) {
                            resolved = (sdk::UObject*)item->get_object();
                        }
                    }
                }
                if (resolved != nullptr) {
                    const bool open = ImGui::TreeNode(prop_name.data());
                    utility::ScopeGuard guard{[open]() { if (open) ImGui::TreePop(); }};
                    if (open) {
                        auto scope2 = m_path.enter(prop_name);
                        try { ui_handle_object(resolved); }
                        catch (...) { ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<failed to display>"); }
                    }
                } else {
                    // Not loaded — render the asset path so the user knows what
                    // the reference would resolve to if loaded.
                    auto* asset_name = (sdk::FName*)(base + 8);
                    const auto& sub = *(sdk::TArrayLite<wchar_t>*)(base + 8 + sizeof(sdk::FName));
                    std::wstring path;
                    try { path = asset_name->to_string(); } catch (...) { path = L"<unreadable>"; }
                    if (sub.data != nullptr && sub.count > 0) {
                        path += L":";
                        const auto len = (size_t)sub.count - (sub.data[sub.count - 1] == L'\0' ? 1 : 0);
                        path.append(sub.data, len);
                    }
                    const auto narrow = utility::narrow(path);
                    ImGui::Text("%s: ", prop_name.data());
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(ImVec4{0.8f, 0.7f, 1.0f, 1.0f}, "<soft, not loaded: %s>", narrow.c_str());
                }
            }
            break;
        case L"DelegateProperty"_fnv: {
            const auto d = (const uint8_t*)((uintptr_t)object + fprop->get_offset());
            ImGui::Text("%s: ", prop_name.data());
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextWrapped("%s", format_script_delegate(d).c_str());
            break;
        }
        case L"MulticastSparseDelegateProperty"_fnv:
            ImGui::Text("%s: ", prop_name.data());
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("<sparse multicast delegate>");
            break;
        case L"MulticastDelegateProperty"_fnv:
        case L"MulticastInlineDelegateProperty"_fnv: {
            const auto d = (const uint8_t*)((uintptr_t)object + fprop->get_offset());
            ImGui::Text("%s: ", prop_name.data());
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextWrapped("%s", format_multicast_delegate(d).c_str());
            break;
        }
        case L"MapProperty"_fnv: {
            const auto base = (const uint8_t*)((uintptr_t)object + fprop->get_offset());
            std::vector<std::pair<std::string, std::string>> entries;
            const int num = read_map_entries((sdk::FMapProperty*)fprop, base, 64, entries);
            if (entries.empty()) {
                ImGui::Text("%s: ", prop_name.data());
                ImGui::SameLine(0.0f, 0.0f);
                if (num == 0) ImGui::TextDisabled("<TMap: empty>");
                else if (num < 0) ImGui::TextDisabled("<TMap: unreadable>");
                else ImGui::TextDisabled("<TMap: %d slots>", num);
            } else if (ImGui::TreeNode((void*)fprop, "%s (TMap, %d)", prop_name.data(), num)) {
                for (auto& [k, v] : entries) {
                    ImGui::BulletText("%s => %s", k.c_str(), v.c_str());
                }
                if (num > (int)entries.size()) {
                    ImGui::TextDisabled("...(+%d more)", num - (int)entries.size());
                }
                ImGui::TreePop();
            }
            break;
        }
        case L"SetProperty"_fnv: {
            const auto base = (const uint8_t*)((uintptr_t)object + fprop->get_offset());
            std::vector<std::string> entries;
            const int num = read_set_entries((sdk::FSetProperty*)fprop, base, 64, entries);
            if (entries.empty()) {
                ImGui::Text("%s: ", prop_name.data());
                ImGui::SameLine(0.0f, 0.0f);
                if (num == 0) ImGui::TextDisabled("<TSet: empty>");
                else if (num < 0) ImGui::TextDisabled("<TSet: unreadable>");
                else ImGui::TextDisabled("<TSet: %d slots>", num);
            } else if (ImGui::TreeNode((void*)fprop, "%s (TSet, %d)", prop_name.data(), num)) {
                for (auto& e : entries) {
                    ImGui::BulletText("%s", e.c_str());
                }
                if (num > (int)entries.size()) {
                    ImGui::TextDisabled("...(+%d more)", num - (int)entries.size());
                }
                ImGui::TreePop();
            }
            break;
        }
        case L"StructProperty"_fnv:
            {
                void* addr = (void*)((uintptr_t)object + fprop->get_offset());
                const auto strukt = ((sdk::FStructProperty*)prop)->get_struct();

                if (ui_try_known_struct(prop_name, addr, strukt, /*top_level_save*/ true)) {
                    // Known structs (Vector/Rotator/Transform/...) render compactly + return; attach
                    // the struct-save / edit-flags right-click menu to that row so they're savable too.
                    display_context_struct(addr, strukt != nullptr ? (int32_t)strukt->get_properties_size() : 0);
                    break;
                }

                row_ctx_drawn = true; // unknown struct draws its own Copy/Edit-flags popup
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::Button("Copy Address")) {
                        const auto hex = (std::stringstream{} << std::hex << (uintptr_t)addr).str();

                        if (OpenClipboard(NULL)) {
                            EmptyClipboard();
                            HGLOBAL hcd = GlobalAlloc(GMEM_DDESHARE, hex.size() + 1);
                            char* data = (char*)GlobalLock(hcd);
                            strcpy(data, hex.c_str());
                            GlobalUnlock(hcd);
                            SetClipboardData(CF_TEXT, hcd);
                            CloseClipboard();
                        }
                    }
                    if (ImGui::BeginMenu("Edit flags")) {
                        edit_property_flags(utility::narrow(prop->get_field_name().to_string()), (sdk::FProperty*)prop);
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }

                // A StructProperty is inline data, never itself a pointer to another object — default
                // its field list open ("sensibly grouped data"). Any member that IS an object
                // reference still renders its own separately-gated (collapsed) TreeNode via the
                // recursive ui_handle_struct call below.
                const bool open = ImGui::TreeNodeEx(prop_name.data(), ImGuiTreeNodeFlags_DefaultOpen);
                utility::ScopeGuard guard{[open]() { if (open) ImGui::TreePop(); }};
                if (open) {
                    auto scope2 = m_path.enter(utility::narrow(prop->get_field_name().to_string()));
                    ui_handle_struct(addr, ((sdk::FStructProperty*)prop)->get_struct());
                }
            }
            break;
        case L"Function"_fnv:
            break;
        case L"ArrayProperty"_fnv:
            {
                // Same "uncollapsed unless it points to an object" rule as the struct case above:
                // arrays of plain data (numbers/structs/strings) default open; arrays of object
                // references default collapsed, since expanding one walks a whole nested object
                // per entry (same reasoning as the Object/WeakObject/SoftObject property cases).
                bool is_object_array = false;
                try {
                    if (auto inner = ((sdk::FArrayProperty*)prop)->get_inner(); inner != nullptr) {
                        if (auto inner_c = inner->get_class(); inner_c != nullptr) {
                            switch (utility::hash(inner_c->get_name().to_string())) {
                            case L"InterfaceProperty"_fnv:
                            case L"ObjectProperty"_fnv:
                            case L"WeakObjectProperty"_fnv:
                            case L"LazyObjectProperty"_fnv:
                            case L"SoftObjectProperty"_fnv:
                            case L"ClassProperty"_fnv:
                            case L"SoftClassProperty"_fnv:
                                is_object_array = true;
                                break;
                            default:
                                break;
                            }
                        }
                    }
                } catch (...) {}

                const bool open = ImGui::TreeNodeEx(prop_name.data(), is_object_array ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen);
                utility::ScopeGuard guard{[open]() { if (open) ImGui::TreePop(); }};
                if (open) {
                    auto scope2 = m_path.enter(prop_name);
                    try {
                        ui_handle_array_property(object, (sdk::FArrayProperty*)prop);
                    } catch (const std::exception& e) {
                        ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<array threw: %s>", e.what());
                    } catch (...) {
                        ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<array threw (unknown)>");
                    }
                }
            }
            break;
        case L"NameProperty"_fnv:
            {
                auto& value = *(sdk::FName*)((uintptr_t)object + fprop->get_offset());

                // Editable — per-property scratch text buffer keyed by the field's own live
                // address (unique per instance+property), resynced from the live FName every
                // frame. Commits ONLY on deactivate-after-edit (blur/Enter), NOT per keystroke:
                // EFindName::Add permanently interns whatever we construct into the target
                // process's global FName pool, so committing on every character would intern
                // every in-progress partial string typed. Matches encode_param's one-shot commit
                // for the identical FName{wstring_view, EFindName::Add} construction (line ~1380).
                static std::unordered_map<void*, std::string> s_name_edit_bufs;
                // Evict once the map gets large so a long session's churn of spawned/destroyed
                // objects (each a distinct field address) doesn't leak forever — mirrors the
                // erase-on-cleanup precedent this file already uses for s_scale_last_dist.
                if (s_name_edit_bufs.size() > 512) {
                    s_name_edit_bufs.clear();
                }
                auto& buf = s_name_edit_bufs[(void*)&value];
                buf = utility::narrow(value.to_string());
                // +1 for the null terminator regardless of current content length — ImGui::InputText
                // requires buf_size > strlen(content) or there's no room to type further (and an
                // assert-enabled build trips its own "properly zero-terminated?" assert).
                buf.resize(std::max<size_t>(buf.size() + 1, 256));

                ImGui::InputText(prop_name.data(), buf.data(), buf.size());
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    buf.resize(std::strlen(buf.c_str()));
                    const std::wstring wide = utility::widen(buf);
                    value = sdk::FName{std::wstring_view{wide}, sdk::EFindName::Add};
                    buf.resize(std::max<size_t>(buf.size() + 1, 256));
                    notify_property_changed();
                }
                display_context(value);
            }
            break;
        case L"StrProperty"_fnv:
        {
            using FString = sdk::TArray<wchar_t>;
            const auto& value = *(FString*)((uintptr_t)object + fprop->get_offset());

            if (value.data != nullptr && value.count > 0) {
                const auto str = std::wstring_view{value.data, (size_t)value.count};
                const auto narrow_str = utility::narrow(str);

                ImGui::Text("%s: ", prop_name.data());
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4{3.0f / 255.0f, 232.0f / 255.0f, 252.0f / 255.0f, 1.0f}, "%s", narrow_str.data());
            } else {
                ImGui::Text("%s: ", prop_name.data());
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4{3.0f / 255.0f, 232.0f / 255.0f, 252.0f / 255.0f, 1.0f}, "[empty string]");
            }
            break;
        }
        default:
            {
                const auto name = utility::narrow(propc->get_name().to_string());
                const auto field_name = utility::narrow(prop->get_field_name().to_string());
                ImGui::Text("%s %s", name.data(), field_name.data());
            }
            break;
        };
        // Generic Edit-flags popup for property rows that DON'T draw their own context menu above
        // (Name/Str/Object/Array/Set/Map/etc.). Gated on row_ctx_drawn so it never re-opens on the
        // same right-click as a Save popup and steals it — that double-open was the reason the Save
        // button never appeared on savable rows.
        if (!row_ctx_drawn) {
            std::string prop_name_edit = prop_name + "EditFlags";
            if (ImGui::BeginPopupContextItem(prop_name_edit.c_str())){
                    if (ImGui::BeginMenu("Edit flags")) {
                        edit_property_flags(utility::narrow(prop->get_field_name().to_string()), (sdk::FProperty*)prop);
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
            }
        }
    }
    // item_width_pop (ScopeGuard above) pops here.
}

void UObjectHook::ui_handle_array_property(void* addr, sdk::FArrayProperty* prop) {
    if (addr == nullptr || prop == nullptr) {
        return;
    }

    const auto& array_generic = *(sdk::TArray<void*>*)((uintptr_t)addr + prop->get_offset());

    if (array_generic.data == nullptr || array_generic.count == 0) {
        ImGui::Text("Empty array");
        return;
    }

    const auto inner = prop->get_inner();

    if (inner == nullptr) {
        ImGui::Text("Failed to get inner property");
        return;
    }

    const auto inner_c = inner->get_class();

    if (inner_c == nullptr) {
        ImGui::Text("Failed to get inner property class");
        return;
    }

    const auto inner_c_type = utility::narrow(inner_c->get_name().to_string());

    switch (utility::hash(inner_c_type)) {
    case L"NameProperty"_fnv: {
        // TArray<FName> by value (8 bytes each) — not an array of FName pointers.
        const auto& a = *(sdk::TArray<sdk::FName>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<FName> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) {
            std::wstring s;
            try { s = a.data[i].to_string(); } catch (...) { s = L"<bad>"; }
            ImGui::BulletText("[%d] %s", i, utility::narrow(s).c_str());
        }
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case "InterfaceProperty"_fnv:
    case "ObjectProperty"_fnv:
    {
        const auto& array_obj = *(sdk::TArray<sdk::UObject*>*)((uintptr_t)addr + prop->get_offset());

        int32_t i = -1;
        for (auto obj : array_obj) {
            ++i;
            // Object arrays (e.g. OverrideMaterials) routinely contain null slots.
            if (obj == nullptr) {
                ImGui::BulletText("[%d] nullptr", i);
                continue;
            }
            std::wstring name;
            try {
                const auto cls = obj->get_class();
                name = (cls != nullptr ? cls->get_fname().to_string() : std::wstring{L"<no class>"})
                     + L" " + obj->get_fname().to_string();
            } catch (...) {
                ImGui::BulletText("[%d] <unreadable %p>", i, (void*)obj);
                continue;
            }
            const auto narrow_name = std::format("[{}] {}", i, utility::narrow(name));
            const bool open = ImGui::TreeNode(narrow_name.c_str());
            utility::ScopeGuard guard{[open]() { if (open) ImGui::TreePop(); }};
            if (open) {
                auto scope = m_path.enter(narrow_name);
                try { ui_handle_object(obj); }
                catch (...) { ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<failed to display element %d>", i); }
            }
        }

        break;
    }
    case "WeakObjectProperty"_fnv:
    case "LazyObjectProperty"_fnv:
    {
        // TArray<FWeakObjectPtr> — element stride is 8 (Weak) or 24 (Lazy:
        // 8B weak + 16B FGuid). Resolve each via FUObjectArray.
        const auto stride = (inner_c_type == "WeakObjectProperty") ? (int32_t)8 : (int32_t)24;
        const auto& arr = *(sdk::TArray<uint8_t>*)((uintptr_t)addr + prop->get_offset());
        for (int32_t i = 0; i < arr.count; ++i) {
            auto raw = (int32_t*)((uintptr_t)arr.data + (uintptr_t)i * stride);
            const auto obj_index = raw[0];
            const auto serial = raw[1];
            if (obj_index <= 0) {
                ImGui::BulletText("[%d] nullptr", i);
                continue;
            }
            auto item = sdk::FUObjectArray::get()->get_object(obj_index);
            if (item == nullptr || item->get_object() == nullptr || item->get_serial_number() != serial) {
                ImGui::BulletText("[%d] <stale weak idx=%d>", i, obj_index);
                continue;
            }
            auto obj = (sdk::UObject*)item->get_object();
            const auto label = std::format("[{}] {} {}", i,
                utility::narrow(obj->get_class()->get_fname().to_string()),
                utility::narrow(obj->get_fname().to_string()));
            const bool open = ImGui::TreeNode(label.c_str());
            utility::ScopeGuard guard{[open]() { if (open) ImGui::TreePop(); }};
            if (open) {
                try { ui_handle_object(obj); }
                catch (...) { ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<failed to display>"); }
            }
        }
        break;
    }
    case "SoftObjectProperty"_fnv:
    case "SoftClassProperty"_fnv:
    {
        // TArray<FSoftObjectPtr> — stride 32B (8B weak + 8B FName + 16B FString SubPath).
        constexpr int32_t kStride = 32;
        const auto& arr = *(sdk::TArray<uint8_t>*)((uintptr_t)addr + prop->get_offset());
        for (int32_t i = 0; i < arr.count; ++i) {
            const auto base = (uintptr_t)arr.data + (uintptr_t)i * kStride;
            const auto obj_index = *(int32_t*)base;
            const auto serial = *(int32_t*)(base + 4);
            sdk::UObject* resolved = nullptr;
            if (obj_index > 0) {
                if (auto item = sdk::FUObjectArray::get()->get_object(obj_index); item != nullptr) {
                    if (item->get_object() != nullptr && item->get_serial_number() == serial) {
                        resolved = (sdk::UObject*)item->get_object();
                    }
                }
            }
            if (resolved != nullptr) {
                const auto label = std::format("[{}] {} {} (soft, loaded)", i,
                    utility::narrow(resolved->get_class()->get_fname().to_string()),
                    utility::narrow(resolved->get_fname().to_string()));
                const bool open = ImGui::TreeNode(label.c_str());
                utility::ScopeGuard guard{[open]() { if (open) ImGui::TreePop(); }};
                if (open) {
                    try { ui_handle_object(resolved); }
                    catch (...) { ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "<failed to display>"); }
                }
            } else {
                auto* asset_name = (sdk::FName*)(base + 8);
                const auto& sub = *(sdk::TArrayLite<wchar_t>*)(base + 8 + sizeof(sdk::FName));
                std::wstring path;
                try { path = asset_name->to_string(); } catch (...) { path = L"<unreadable>"; }
                if (sub.data != nullptr && sub.count > 0) {
                    path += L":";
                    const auto len = (size_t)sub.count - (sub.data[sub.count - 1] == L'\0' ? 1 : 0);
                    path.append(sub.data, len);
                }
                ImGui::BulletText("[%d] <soft, not loaded: %s>", i, utility::narrow(path).c_str());
            }
        }
        break;
    }
    case "StructProperty"_fnv:
    {
        // Not really an array of void* but we will skip over individual elements
        // using pointer arithmetic.
        const auto& array_obj = *(sdk::TArray<void*>*)((uintptr_t)addr + prop->get_offset());

        if (array_obj.data == nullptr || array_obj.count == 0) {
            ImGui::Text("Empty array");
            return;
        }

        const auto struct_prop = (sdk::FStructProperty*)inner;
        const auto strukt = struct_prop->get_struct();

        if (strukt == nullptr) {
            ImGui::Text("Cannot determine struct type");
            return;
        }

        auto element_size = strukt->get_struct_size();

        if (element_size == 0) {
            element_size = strukt->get_properties_size();

            if (element_size == 0) {
                ImGui::Text("Cannot determine struct size");
                return;
            }
        }

        for (size_t i = 0; i < array_obj.count; ++i) {
            auto element = (void*)((uintptr_t)array_obj.data + (i * element_size));

            if (element == nullptr) {
                continue;
            }

            if (ui_try_known_struct(std::string("Element ") + std::to_string(i), element, strukt)) {
                continue;
            }

            const bool element_node_open = ImGui::TreeNode((void*)element, "Element %d", i);
            // RAII guard so TreePop always runs even if ui_handle_struct
            // throws — without this the previous `try { TreeNode ... TreePop }
            // catch(...) { Text(...); }` swallowed the exception but left the
            // TreeNode open, which is exactly the missing-TreePop pattern that
            // ImGui's end-of-frame recovery complains about.
            utility::ScopeGuard element_guard{[element_node_open]() {
                if (element_node_open) {
                    ImGui::TreePop();
                }
            }};

            if (!element_node_open) {
                continue;
            }

            try {
                // TODO: Figure out if this can be used with persistent states?
                //auto scope = m_path.enter("Element " + std::to_string(i));
                ui_handle_struct(element, strukt);
            } catch(const std::exception& e) {
                ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "Failed to display element %zu: %s", i, e.what());
            } catch(...) {
                ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "Failed to display element %zu (unknown exception)", i);
            }
        }

        break;
    }
    // --- Scalar element types ----------------------------------------------
    // These all share the same shape: TArray<T> stored inline. We cap the
    // visible row count because some games hold gigantic TArrays (texel data,
    // vertex buffers) and iterating tens of thousands of ImGui::BulletText
    // calls would tank the frame. 1024 is plenty for inspection; anything
    // bigger should go through Lua / file dump.
    case L"FloatProperty"_fnv: {
        const auto& a = *(sdk::TArray<float>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<float> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) ImGui::BulletText("[%d] %.6g", i, a.data[i]);
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"DoubleProperty"_fnv: {
        const auto& a = *(sdk::TArray<double>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<double> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) ImGui::BulletText("[%d] %.10g", i, a.data[i]);
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"IntProperty"_fnv:
    case L"UInt32Property"_fnv: {
        const auto& a = *(sdk::TArray<int32_t>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<int32> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) ImGui::BulletText("[%d] %d (0x%08x)", i, a.data[i], (uint32_t)a.data[i]);
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"Int64Property"_fnv:
    case L"UInt64Property"_fnv: {
        const auto& a = *(sdk::TArray<int64_t>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<int64> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) ImGui::BulletText("[%d] %lld", i, (long long)a.data[i]);
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"Int16Property"_fnv:
    case L"UInt16Property"_fnv: {
        const auto& a = *(sdk::TArray<int16_t>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<int16> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) ImGui::BulletText("[%d] %d", i, (int)a.data[i]);
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"ByteProperty"_fnv:
    case L"Int8Property"_fnv: {
        // 1-byte inner — could be a packed enum, color channel, etc. Show
        // both decimal and hex so the user can spot bit patterns. (BoolProperty
        // in UE serialises as a bit inside a byte per-instance, not as a
        // standalone byte array, so we leave that to the BoolProperty case.)
        const auto& a = *(sdk::TArray<uint8_t>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<byte> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) ImGui::BulletText("[%d] %u (0x%02x)", i, (unsigned)a.data[i], a.data[i]);
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"BoolProperty"_fnv: {
        // TArray<bool> in UE is actually TArray<uint8_t> with 0/non-zero
        // values (it's NOT bit-packed at the array level — that's reserved
        // for non-array bool members).
        const auto& a = *(sdk::TArray<uint8_t>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<bool> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) ImGui::BulletText("[%d] %s", i, a.data[i] ? "true" : "false");
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"EnumProperty"_fnv: {
        // EnumProperty in UE wraps a numeric underlying type. We don't have
        // the FEnumProperty::get_underlying_property binding here, so just
        // dump bytes — sufficient to confirm element count and rough values.
        const auto& a = *(sdk::TArray<uint8_t>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<Enum> count=%d capacity=%d (raw bytes)", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) ImGui::BulletText("[%d] 0x%02x", i, a.data[i]);
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"StrProperty"_fnv: {
        // TArray<FString>; FString itself is a TArray<wchar_t>. Stride is
        // sizeof(TArray<wchar_t>) = 16 bytes (data+count+capacity, with
        // typical layout count==capacity==int32 + 8B data pointer).
        struct FString { wchar_t* data; int32_t count; int32_t capacity; };
        static_assert(sizeof(FString) == 16, "FString layout assumption broken");
        const auto& a = *(sdk::TArray<FString>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<FString> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) {
            const auto& s = a.data[i];
            if (s.data == nullptr || s.count == 0) {
                ImGui::BulletText("[%d] \"\"", i);
            } else {
                // FString is null-terminated; trim the trailing wchar before narrowing.
                const size_t len = (size_t)s.count - (s.data[s.count - 1] == L'\0' ? 1 : 0);
                std::wstring w(s.data, len);
                ImGui::Bullet(); ImGui::TextWrapped("[%d] \"%s\"", i, utility::narrow(w).c_str());
            }
        }
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    case L"ClassProperty"_fnv: {
        // TArray<UClass*> — same layout as object array but the elements are
        // class pointers. (SoftClassProperty is handled with SoftObjectProperty
        // above because soft class refs use the same FSoftObjectPtr layout.)
        const auto& a = *(sdk::TArray<sdk::UObject*>*)((uintptr_t)addr + prop->get_offset());
        ImGui::Text("TArray<UClass*> count=%d capacity=%d", a.count, a.capacity);
        const int32_t cap = std::min(a.count, (int32_t)1024);
        for (int32_t i = 0; i < cap; ++i) {
            auto obj = a.data[i];
            if (obj == nullptr) { ImGui::BulletText("[%d] nullptr", i); continue; }
            std::wstring full;
            try { full = obj->get_full_name(); } catch (...) { full = L"<unreadable>"; }
            ImGui::Bullet(); ImGui::TextWrapped("[%d] %s", i, utility::narrow(full).c_str());
        }
        if (a.count > cap) ImGui::TextDisabled("(truncated at %d)", cap);
        break;
    }
    default:
        // Show the raw element stride if we can guess it, so the user has
        // something to grep on before deciding what handler to add.
        ImGui::Text("Array of %s (no view handler)", inner_c_type.data());
        {
            const auto& a = *(sdk::TArray<void*>*)((uintptr_t)addr + prop->get_offset());
            ImGui::TextDisabled("  count=%d capacity=%d data=%p", a.count, a.capacity, (void*)a.data);
        }
        break;
    };
}

// Reusable copy/paste for POD math structs (Vector/Rotator/Quat/Transform/LinearColor/...).
// "Copy" snapshots the struct's raw bytes into a process-wide clipboard; "Paste" appears only
// for a struct of the SAME type-name and byte-size and writes them back. Renders inline, so
// call it right after the struct's editor row (it issues its own SameLine).
static void ui_struct_clipboard(const std::string& sname, void* addr, int32_t size) {
    struct Clip { char name[64]{}; uint8_t data[128]{}; int32_t size{0}; bool valid{false}; };
    static Clip s_clip{};

    if (addr == nullptr || size <= 0 || size > (int32_t)sizeof(s_clip.data)) {
        return;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Copy") && !IsBadReadPtr(addr, (size_t)size)) {
        memcpy(s_clip.data, addr, (size_t)size);
        s_clip.size = size;
        strncpy_s(s_clip.name, sname.c_str(), sizeof(s_clip.name) - 1);
        s_clip.valid = true;
    }

    // Paste only into a matching struct type+size, so you can't smear a Quat over a Vector.
    if (s_clip.valid && s_clip.size == size && sname == s_clip.name) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Paste") && !IsBadReadPtr(addr, (size_t)size)) {
            memcpy(addr, s_clip.data, (size_t)size);
        }
    }
}

bool UObjectHook::ui_try_known_struct(const std::string& label, void* addr, sdk::UStruct* definition, bool top_level_save) {
    // Bind the save context popup to the value widget that was just drawn (so right-clicking the
    // Vector/Color/Transform row opens Save, not the trailing Copy button). Matched by
    // display_context_struct via PushID(addr) + BeginPopup("##known_struct_save").
    const auto open_save_ctx = [&]() {
        if (top_level_save) {
            ImGui::OpenPopupOnItemClick("##known_struct_save", ImGuiPopupFlags_MouseButtonRight);
        }
    };
    if (addr == nullptr || definition == nullptr) {
        return false;
    }

    std::string sname;
    try {
        sname = utility::narrow(definition->get_fname().to_string());
    } catch (...) {
        return false;
    }

    const ImVec4 tag_color{78.0f / 255.0f, 201.0f / 255.0f, 176.0f / 255.0f, 1.0f};
    const auto base = (uintptr_t)addr;
    const int32_t total = definition->get_properties_size();

    ImGui::PushID(addr);
    utility::ScopeGuard id_guard{[]() { ImGui::PopID(); }};

    if (sname == "Color" && total == 4) {
        if (IsBadReadPtr(addr, 4)) {
            ImGui::Text("%s: <unreadable>", label.c_str());
            return true;
        }
        const auto bytes = (uint8_t*)addr;
        float rgba[4] = {bytes[2] / 255.0f, bytes[1] / 255.0f, bytes[0] / 255.0f, bytes[3] / 255.0f};
        if (ImGui::ColorEdit4(label.c_str(), rgba, ImGuiColorEditFlags_NoInputs)) {
            bytes[2] = (uint8_t)(rgba[0] * 255.0f);
            bytes[1] = (uint8_t)(rgba[1] * 255.0f);
            bytes[0] = (uint8_t)(rgba[2] * 255.0f);
            bytes[3] = (uint8_t)(rgba[3] * 255.0f);
        }
        open_save_ctx(); // bind to the ColorEdit4 (a real item id), not the [Color] text below
        ImGui::SameLine();
        ImGui::TextColored(tag_color, "[Color]");
        return true;
    }

    int comps = 0;
    bool force_float = false;
    bool is_int = false;
    if (sname == "Vector2D" || sname == "Vector2f" || sname == "Vector2d") {
        comps = 2;
    } else if (sname == "IntPoint") {
        comps = 2; is_int = true;
    } else if (sname == "Vector" || sname == "Vector3f" || sname == "Vector3d" || sname == "Rotator") {
        comps = 3;
    } else if (sname == "IntVector") {
        comps = 3; is_int = true;
    } else if (sname == "Quat" || sname == "Vector4" || sname == "Vector4f" || sname == "Vector4d") {
        comps = 4;
    } else if (sname == "LinearColor") {
        comps = 4; force_float = true;
    }

    if (comps > 0) {
        // Only treat as a core math struct when the reported size matches its
        // scalar layout — float, or double for UE5. An unrelated game struct that
        // merely shares a name like "Vector" fails this check and falls through to
        // the generic struct view instead of being edited at the wrong offsets.
        const int32_t fw = comps * 4;
        const int32_t dw = comps * 8;
        const bool size_ok = (is_int || force_float) ? (total == fw) : (total == fw || total == dw);
        if (!size_ok) {
            return false;
        }
        const bool wide = (total == dw);
        if (IsBadReadPtr(addr, (size_t)total)) {
            ImGui::Text("%s: <unreadable>", label.c_str());
            return true;
        }
        if (is_int) {
            ImGui::DragScalarN(label.c_str(), ImGuiDataType_S32, addr, comps, 1.0f);
        } else {
            // "%.4g" keeps it compact — trims trailing zeros / caps sig-figs so a
            // Vector reads "1.5, 0, -42.7" instead of "1.500, 0.000, -42.700".
            ImGui::DragScalarN(label.c_str(), wide ? ImGuiDataType_Double : ImGuiDataType_Float, addr, comps, 0.1f,
                nullptr, nullptr, "%.4g");
        }
        open_save_ctx(); // bind to the DragScalarN value, not the trailing Copy button
        ImGui::SameLine();
        ImGui::TextColored(tag_color, "[%s]", sname.c_str());
        ui_struct_clipboard(sname, addr, total);
        return true;
    }

    if (sname == "Transform" || sname == "Transform3f" || sname == "Transform3d") {
        const bool open = ImGui::TreeNode(label.c_str());
        open_save_ctx(); // bind to the Transform TreeNode header, not the trailing Copy button
        ImGui::SameLine();
        ImGui::TextColored(tag_color, "[Transform]");
        ui_struct_clipboard(sname, addr, total);
        if (open) {
            for (auto field = definition->get_child_properties(); field != nullptr; field = field->get_next()) {
                std::string fcname;
                try { fcname = utility::narrow(field->get_class()->get_name().to_string()); } catch (...) { continue; }
                if (fcname != "StructProperty") {
                    continue;
                }
                const auto member_struct = ((sdk::FStructProperty*)field)->get_struct();
                const auto member_off = ((sdk::FProperty*)field)->get_offset();
                std::string fname;
                try { fname = utility::narrow(field->get_field_name().to_string()); } catch (...) { fname = "?"; }
                if (!ui_try_known_struct(fname, (void*)(base + member_off), member_struct)) {
                    if (ImGui::TreeNode(fname.c_str())) {
                        ui_handle_struct((void*)(base + member_off), member_struct);
                        ImGui::TreePop();
                    }
                }
            }
            ImGui::TreePop();
        }
        return true;
    }

    return false;
}

// Texture2D preview (D3D11) — implemented via the SDK's self-calibrating UTexture accessors
// (UTexture::get_texture_rhi -> FRHITexture::get_native_resource), the same render-resource path
// D3D11Component uses for the UE backbuffer (D3D11Component.cpp:243). No hardcoded offsets.
// Gated behind m_show_texture_previews (default OFF); every deref guarded + try/catch.
// D3D12 not implemented yet (would need an imgui SRV-heap descriptor). SRVs cached per native.
void UObjectHook::draw_texture_preview(sdk::UObject* texture) {
    if (texture == nullptr) {
        return;
    }

    if (g_framework->get_renderer_type() != Framework::RendererType::D3D11) {
        ImGui::TextDisabled("[texture preview: only D3D11 implemented]");
        return;
    }

    // Only Texture2D-family assets: update_render_resource_offset_texture2d calibrates a SHARED
    // static offset off the texture's 2D vtable, so feeding it a non-2D texture would corrupt it.
    std::string tclass;
    try { tclass = utility::narrow(texture->get_class()->get_fname().to_string()); } catch (...) {}
    if (tclass.find("Texture2D") == std::string::npos) {
        ImGui::TextDisabled("[texture preview: only Texture2D supported (%s)]", tclass.c_str());
        return;
    }

    try {
        auto* utex = (sdk::UTexture*)texture;

        // FRHITexture2D is a UE engine class; the SDK only captures its vtable inside UEVR's VR
        // render-target hook, so in flat (no-VR) mode it stays null. A previous heuristic scanned the
        // texture for a vtable living in d3d11/d3d12.dll and used it as FRHITexture2D's — but that is
        // WRONG: UE's D3D11RHI (FD3D11Texture2D : FRHITexture2D) is compiled into the GAME module, so
        // its vtable is NOT in Microsoft's d3d11.dll. The scan instead latched the actual
        // ID3D11Texture2D COM vtable and stored it as FRHITexture2D's, corrupting every subsequent
        // virtual call (get_native_resource) -> GetDesc on garbage -> crash. Until a correct flat-mode
        // RHI hook exists, bail gracefully when the vtable wasn't captured legitimately.
        if (FRHITexture2D::get_vtable() == nullptr) {
            ImGui::TextDisabled("[texture preview unavailable in flat mode — enter VR once to capture the RHI vtable]");
            return;
        }

        sdk::UTexture::update_render_resource_offset_texture2d(utex); // self-calibrate Resource offset (render thread)

        auto* rhi = utex->get_texture_rhi();
        if (rhi == nullptr) { ImGui::TextDisabled("[texture: no RHI (streamed out / not resident?)]"); return; }

        auto* native = (ID3D11Texture2D*)rhi->get_native_resource();
        if (native == nullptr) { ImGui::TextDisabled("[texture: no native D3D11 resource]"); return; }

        // One SRV per native texture. Debug tool: SRVs are cached (leaked) and bounded by the
        // number of distinct textures inspected.
        static std::unordered_map<ID3D11Texture2D*, ID3D11ShaderResourceView*> s_srv_cache{};

        D3D11_TEXTURE2D_DESC desc{};
        native->GetDesc(&desc); // AVs if `native` is bogus -> caught by the surrounding try

        ID3D11ShaderResourceView* srv = nullptr;
        if (auto it = s_srv_cache.find(native); it != s_srv_cache.end()) {
            srv = it->second;
        } else {
            auto device = g_framework->get_d3d11_hook()->get_device();
            if (device == nullptr) { ImGui::TextDisabled("[texture: no D3D11 device]"); return; }

            // Typeless resource formats can't be used directly for an SRV — map to a typed
            // equivalent so typeless textures / render targets still preview instead of failing.
            DXGI_FORMAT srv_format = desc.Format;
            switch (desc.Format) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:     srv_format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:     srv_format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS:  srv_format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: srv_format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
            case DXGI_FORMAT_R32G32B32A32_TYPELESS: srv_format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
            case DXGI_FORMAT_BC1_TYPELESS:          srv_format = DXGI_FORMAT_BC1_UNORM; break;
            case DXGI_FORMAT_BC2_TYPELESS:          srv_format = DXGI_FORMAT_BC2_UNORM; break;
            case DXGI_FORMAT_BC3_TYPELESS:          srv_format = DXGI_FORMAT_BC3_UNORM; break;
            case DXGI_FORMAT_BC7_TYPELESS:          srv_format = DXGI_FORMAT_BC7_UNORM; break;
            default: break;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srv_format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = desc.MipLevels ? desc.MipLevels : 1;
            if (FAILED(device->CreateShaderResourceView(native, &sd, &srv)) || srv == nullptr) {
                ImGui::TextDisabled("[texture: CreateShaderResourceView failed]");
                return;
            }
            s_srv_cache[native] = srv;
        }

        // Aspect-fit into a max preview box.
        const float maxw = 256.0f;
        const float aspect = desc.Height > 0 ? (float)desc.Width / (float)desc.Height : 1.0f;
        const ImVec2 size = aspect >= 1.0f ? ImVec2{maxw, maxw / aspect} : ImVec2{maxw * aspect, maxw};
        ImGui::Text("%ux%u %s", desc.Width, desc.Height, desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? "RGBA8" : "");
        ImGui::Image((ImTextureID)(uintptr_t)srv, size);
    } catch (...) {
        ImGui::TextDisabled("[texture preview: resolve failed (bad offsets / unsupported layout)]");
    }
}

#pragma endregion
// VR thumbstick gizmo control (needs a headset to verify feel/sensitivity, but the logic below is
// exercised the same way every frame a gizmo is shown in VR — it just no-ops at zero stick input).
// Left stick X/Y drives world X/Y; right stick Y drives world Z — same channel mapping regardless of
// m_gizmo_mode, only the OPERATION differs (translate / rotate about that axis / scale that axis).
// Reuses the exact edit calls the mouse-drag path uses (set_world_location / add_world_rotation /
// set_relative_scale) and the exact FRotator-component-per-world-axis mapping the mouse path derived
// (ax 0/X->Roll, 1/Y->Pitch, 2/Z->Yaw) so stick-driven and mouse-driven rotation agree. Also drives
// m_driven_comp/m_driven_axis/m_driven_frame — the SAME "hot axis" mechanism the property-inspector
// slider drag already uses (see driven_hot() in draw_component_gizmos) — so the matching gizmo handle
// highlights while a stick is deflected, no separate VR-specific hot-state needed.
void UObjectHook::vr_gizmo_stick_adjust(sdk::USceneComponent* comp) {
    if (comp == nullptr) {
        return;
    }
    auto vr = VR::get();
    if (vr == nullptr || !vr->is_using_controllers()) {
        return;
    }

    constexpr float kDeadzone = 0.15f;
    constexpr float kTranslatePerSec = 80.0f;   // world cm/sec at full deflection
    constexpr float kRotatePerSec = 90.0f;      // degrees/sec at full deflection
    constexpr float kScalePerSec = 0.5f;        // relative-scale units/sec at full deflection

    auto deadzoned = [](float v) -> float {
        if (std::fabs(v) < kDeadzone) return 0.0f;
        // Rescale so output still reaches +/-1 at full deflection instead of jumping at the
        // deadzone edge.
        const float sign = v < 0.0f ? -1.0f : 1.0f;
        return sign * (std::fabs(v) - kDeadzone) / (1.0f - kDeadzone);
    };

    const auto left = vr->get_left_stick_axis();
    const auto right = vr->get_right_stick_axis();
    // channel[0]=world X (left stick X), channel[1]=world Y (left stick Y), channel[2]=world Z
    // (right stick Y) — matches the task spec's "left-stick X/Y -> gizmo X/Y, right-stick Y -> gizmo Z".
    const float channel[3] = { deadzoned(left.x), deadzoned(left.y), deadzoned(right.y) };
    if (channel[0] == 0.0f && channel[1] == 0.0f && channel[2] == 0.0f) {
        return; // sticks centered — nothing to drive, nothing to highlight
    }

    const float dt = m_last_delta_time;
    int hot_axis = -1;
    float hot_mag = 0.0f;

    try {
        if (m_gizmo_mode == 1) { // Rotate: each channel spins about its matching world axis.
            glm::vec3 euler{0.0f, 0.0f, 0.0f};
            for (int ax = 0; ax < 3; ++ax) {
                if (channel[ax] == 0.0f) continue;
                const float delta = channel[ax] * kRotatePerSec * dt;
                if (ax == 0)      euler.z += delta; // world X -> Roll
                else if (ax == 1) euler.x += delta; // world Y -> Pitch
                else              euler.y += delta; // world Z -> Yaw
                if (std::fabs(channel[ax]) > hot_mag) { hot_mag = std::fabs(channel[ax]); hot_axis = ax; }
            }
            comp->add_world_rotation(euler, false, false);
        } else if (m_gizmo_mode == 2) { // Scale: each channel grows/shrinks its matching axis.
            auto scale = comp->get_relative_scale();
            for (int ax = 0; ax < 3; ++ax) {
                if (channel[ax] == 0.0f) continue;
                scale[ax] += channel[ax] * kScalePerSec * dt;
                if (std::fabs(channel[ax]) > hot_mag) { hot_mag = std::fabs(channel[ax]); hot_axis = ax; }
            }
            comp->set_relative_scale(scale);
        } else { // Translate (0) and Combined (3) both move — Combined has no single "active" mode.
            const glm::vec3 world_axes[3] = {
                glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}
            };
            glm::vec3 offset{0.0f, 0.0f, 0.0f};
            for (int ax = 0; ax < 3; ++ax) {
                if (channel[ax] == 0.0f) continue;
                offset += world_axes[ax] * (channel[ax] * kTranslatePerSec * dt);
                if (std::fabs(channel[ax]) > hot_mag) { hot_mag = std::fabs(channel[ax]); hot_axis = ax; }
            }
            comp->set_world_location(comp->get_world_location() + offset, false, false);
        }
    } catch (...) {
        return;
    }

    if (hot_axis >= 0) {
        m_driven_comp = comp;
        m_driven_axis = hot_axis;
        m_driven_frame = (uint32_t)ImGui::GetFrameCount();
    }
}

void UObjectHook::ui_handle_struct(void* addr, sdk::UStruct* uclass) {
    if (uclass == nullptr) {
        return;
    }

    if (addr != nullptr && this->exists_unsafe((sdk::UObject*)addr) && uclass->is_a(sdk::UStruct::static_class())) {
        uclass = (sdk::UStruct*)addr;
        addr = nullptr;
    }

    // Display inheritance tree
    {
        const bool inh_open = ImGui::TreeNode("Inheritance");
        utility::ScopeGuard inh_guard{[inh_open]() {
            if (inh_open) {
                ImGui::TreePop();
            }
        }};
        if (inh_open) {
            for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
                const auto super_name = utility::narrow(super->get_full_name());
                const bool super_open = ImGui::TreeNode(super_name.data());
                utility::ScopeGuard super_guard{[super_open]() {
                    if (super_open) {
                        ImGui::TreePop();
                    }
                }};
                if (super_open) {
                    try {
                        ui_handle_struct(addr, super);
                    } catch (const std::exception& e) {
                        ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "ui_handle_struct threw: %s", e.what());
                    } catch (...) {
                        ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "ui_handle_struct threw (unknown)");
                    }
                }
            }
        }
    }

    {
        const bool fn_open = ImGui::TreeNode("Functions");
        utility::ScopeGuard fn_guard{[fn_open]() {
            if (fn_open) {
                ImGui::TreePop();
            }
        }};
        if (fn_open) {
            try {
                ui_handle_functions(addr, uclass);
            } catch (const std::exception& e) {
                ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "ui_handle_functions threw: %s", e.what());
            } catch (...) {
                ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "ui_handle_functions threw (unknown)");
            }
        }
    }

    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    {
        const bool props_open = ImGui::TreeNode("Properties");
        utility::ScopeGuard props_guard{[props_open]() {
            if (props_open) {
                ImGui::TreePop();
            }
        }};
        if (props_open) {
            try {
                ui_handle_properties(addr, uclass);
            } catch (const std::exception& e) {
                ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "ui_handle_properties threw: %s", e.what());
            } catch (...) {
                ImGui::TextColored(ImVec4{1.0f, 0.3f, 0.3f, 1.0f}, "ui_handle_properties threw (unknown)");
            }
        }
    }
}

void* UObjectHook::add_object(void* rcx, void* rdx, void* r8, void* r9, void* stack1, void* stack2, void* stack3, void* stack4) {
    auto& hook = UObjectHook::get();
    auto result = hook->m_add_object_hook.unsafe_call<void*>(rcx, rdx, r8, r9, stack1, stack2, stack3, stack4);

    {
        static bool is_rcx = [&]() {
            if (!IsBadReadPtr(rcx, sizeof(void*)) &&
                !IsBadReadPtr(*(void**)rcx, sizeof(void*)) &&
                !IsBadReadPtr(**(void***)rcx, sizeof(void*)))
            {
                SPDLOG_INFO("[UObjectHook] RCX is UObjectBase*");
                return true;
            } else {
                SPDLOG_INFO("[UObjectHook] RDX is UObjectBase*");
                return false;
            }
        }();

        sdk::UObjectBase* obj = nullptr;

        if (is_rcx) {
            obj = (sdk::UObjectBase*)rcx;
        } else {
            obj = (sdk::UObjectBase*)rdx;
        }

        ++hook->m_debug.constructor_calls;
        hook->add_new_object(obj);
    }

    return result;
}

void* UObjectHook::destructor(sdk::UObjectBase* object, void* rdx, void* r8, void* r9) {
    auto& hook = UObjectHook::get();

    {
        std::unique_lock _{hook->m_mutex};

        if (auto it = hook->m_meta_objects.find(object); it != hook->m_meta_objects.end()) {
            ++hook->m_debug.destructor_calls;

#ifdef VERBOSE_UOBJECTHOOK
            SPDLOG_INFO("Removing object {:x} {:s}", (uintptr_t)object, utility::narrow(it->second->full_name));
#endif
            hook->m_objects.erase(object);
            hook->m_motion_controller_attached_components.erase((sdk::USceneComponent*)object);
            hook->m_spawned_spheres.erase((sdk::USceneComponent*)object);
            hook->m_spawned_spheres_to_components.erase((sdk::USceneComponent*)object);
            hook->m_components_with_spheres.erase((sdk::USceneComponent*)object);

            if (object == hook->m_overlap_detection_actor) {
                hook->m_overlap_detection_actor = nullptr;
            }

            if (object == hook->m_overlap_detection_actor_left) {
                hook->m_overlap_detection_actor_left = nullptr;
            }

            if (object == hook->m_camera_attach.object) {
                hook->m_camera_attach.object = nullptr;
            }

            if (object == (sdk::UObjectBase*)hook->m_view_camera_actor) {
                hook->m_view_camera_actor = nullptr;
            }

            if ((sdk::USceneComponent*)object == hook->m_flat_gizmo_drag_comp.load()) {
                hook->m_flat_gizmo_drag_comp.store(nullptr);
            }

            hook->m_overlay_mat_originals.erase((sdk::USceneComponent*)object);

            if (object == (sdk::UObjectBase*)hook->m_highlight_material) {
                hook->m_highlight_material = nullptr;
            }

            for (auto super : it->second->super_classes) {
                hook->m_objects_by_class[super].erase(object);
            }

            hook->m_reusable_meta_objects.push_back(std::move(it->second));
            hook->m_meta_objects.erase(object);
        }
    }

    auto result = hook->m_destructor_hook.unsafe_call<void*>(object, rdx, r8, r9);

    return result;
}

void UObjectHook::PersistentProperties::save_to_file(std::optional<std::filesystem::path> path) try {
    if (!path.has_value()) {
        path = path_to_json;
    }

    if (!path.has_value()) {
        std::string key{};
        for (const auto& p : this->path.path()) {
            key += p;
        }
        // Locator-only buckets have an empty path; key the file off the full-name locator so two
        // different locator saves don't collide on hash("").
        if (key.empty() && !object_locator.empty()) {
            key = utility::narrow(object_locator);
        }

        const auto hash_str = std::to_string(utility::hash(key)) + "_props.json";
        path = UObjectHook::get_persistent_dir() / hash_str;
    }

    std::filesystem::create_directories(path->parent_path());

    if (!std::filesystem::exists(path->parent_path())) {
        SPDLOG_ERROR("[UObjectHook] Failed to create directory {}", path->parent_path().string());
        return;
    }

    this->path_to_json = *path;

    nlohmann::json j = to_json();
    std::ofstream file(*path);
    file << j.dump(4);
} catch (const std::exception& e) {
    SPDLOG_ERROR("[UObjectHook] Failed to save persistent properties: {}", e.what());
} catch (...) {
    SPDLOG_ERROR("[UObjectHook] Failed to save persistent properties");
}

nlohmann::json UObjectHook::PersistentProperties::to_json() const {
    nlohmann::json json{};

    json["path"] = path.path();
    json["properties"] = nlohmann::json::array();
    json["type"] = "properties";
    json["hide"] = hide;
    json["hide_legacy"] = hide_legacy;
    if (!object_locator.empty()) {
        json["object_locator"] = utility::narrow(object_locator);
    }

    for (const auto& prop : properties) {
        nlohmann::json pj{
            {"name", utility::narrow(prop->name)},
            {"data", prop->data.u64}
        };
        if (prop->struct_size > 0) {
            const uint32_t n = std::min<uint32_t>(prop->struct_size, (uint32_t)sizeof(prop->struct_bytes));
            pj["struct_size"] = n;
            pj["struct_bytes"] = std::vector<uint8_t>(prop->struct_bytes, prop->struct_bytes + n);
        }
        json["properties"].push_back(pj);
    }

    return json;
}

std::shared_ptr<UObjectHook::PersistentProperties> UObjectHook::PersistentProperties::from_json(const nlohmann::json& json) try {
    if (!json.contains("path") || !json.contains("properties") || !json.contains("type")) {
        throw std::runtime_error("Missing path or properties");
    }

    // Make sure we're loading the right type
    if (!json["type"].is_string() || json["type"].get<std::string>() != "properties") {
        throw std::runtime_error("Wrong type");
    }

    auto result = std::make_shared<UObjectHook::PersistentProperties>();

    result->path = StatePath{json["path"].get<std::vector<std::string>>()};
    result->properties.clear();

    for (const auto& prop : json["properties"]) {
        if (!prop.contains("name") || !prop.contains("data")) {
            throw std::runtime_error("Missing name or data");
        }

        if (!prop["data"].is_number_unsigned()) {
            throw std::runtime_error("Data is not unsigned");
        }

        if (!prop["name"].is_string()) {
            throw std::runtime_error("Name is not string");
        }

        auto state = std::make_shared<PropertyState>();
        state->name = utility::widen(prop["name"].get<std::string>());
        state->data.u64 = prop["data"].get<uint64_t>();
        if (prop.contains("struct_size") && prop["struct_size"].is_number_unsigned()) {
            state->struct_size = std::min<uint32_t>(prop["struct_size"].get<uint32_t>(), (uint32_t)sizeof(state->struct_bytes));
            if (prop.contains("struct_bytes") && prop["struct_bytes"].is_array()) {
                auto v = prop["struct_bytes"].get<std::vector<uint8_t>>();
                memcpy(state->struct_bytes, v.data(), std::min<size_t>(v.size(), sizeof(state->struct_bytes)));
            }
        }
        result->properties.push_back(state);
    }

    if (json.contains("hide") && json["hide"].is_boolean()) {
        result->hide = json["hide"].get<bool>();
    } else {
        result->hide = false;
    }

    if (json.contains("hide_legacy") && json["hide_legacy"].is_boolean()) {
        result->hide_legacy = json["hide_legacy"].get<bool>();
    } else {
        result->hide_legacy = false;
    }

    if (json.contains("object_locator") && json["object_locator"].is_string()) {
        result->object_locator = utility::widen(json["object_locator"].get<std::string>());
    }

    return result;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize persistent properties: {}", e.what());
    return nullptr;
} catch (...) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize persistent properties");
    return nullptr;
}

std::shared_ptr<UObjectHook::PersistentProperties> UObjectHook::PersistentProperties::from_json(std::filesystem::path json_path) {
    if (!std::filesystem::exists(json_path)) {
        return nullptr;
    }

    try {
        auto f = std::ifstream{json_path};

        if (f.is_open()) {
            const auto file_contents = std::string{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};

            nlohmann::json data = nlohmann::json::parse(file_contents);

            return UObjectHook::PersistentProperties::from_json(data);
        }

        SPDLOG_ERROR("[UObjectHook] Failed to open JSON file {}", json_path.string());
        return nullptr;
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}: {}", json_path.string(), e.what());
    } catch (...) {
        SPDLOG_ERROR("[UObjectHook] Failed to parse JSON file {}", json_path.string());
    }

    return nullptr;
}


std::vector<std::shared_ptr<UObjectHook::PersistentProperties>> UObjectHook::deserialize_all_persistent_properties() const try {
    const auto uobjecthook_dir = get_persistent_dir();

    if (!std::filesystem::exists(uobjecthook_dir)) {
        return {};
    }

    // Gather all .json files in this directory
    std::vector<std::filesystem::path> json_files{};
    for (const auto& p : std::filesystem::directory_iterator(uobjecthook_dir)) {
        if (p.path().extension() == ".json") {
            json_files.push_back(p.path());
        }
    }

    std::vector<std::shared_ptr<UObjectHook::PersistentProperties>> result{};
    for (const auto& json_file : json_files) {
        // load file
        auto state = UObjectHook::PersistentProperties::from_json(json_file);

        if (state != nullptr) {
            state->path_to_json = json_file;
            result.push_back(state);
            SPDLOG_INFO("[UObjectHook] Loaded persistent properties from {}", json_file.string());
        } else {
            SPDLOG_ERROR("[UObjectHook] {} does not appear to be a valid persistent properties file", json_file.string());
        }
    }

    return result;
} catch (const std::exception& e) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize all persistent properties: {}", e.what());
    return {};
} catch (...) {
    SPDLOG_ERROR("[UObjectHook] Failed to deserialize all persistent properties");
    return {};
}