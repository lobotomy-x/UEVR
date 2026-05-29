#include <fstream>
#include <algorithm>
#include <utility/Logging.hpp>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/UObjectBase.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FField.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/FEnumProperty.hpp>
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
#include <sdk/UMotionControllerComponent.hpp>
   
#include <imgui_internal.h>
#include "uobjecthook/SDKDumper.hpp"
#include "VR.hpp"
#include "PluginLoader.hpp"

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
// Payload type identifiers used by ImGui's drag-and-drop machinery. Anything
// elsewhere in UObjectHook can publish a payload of the same type and the
// function-caller target will pick it up.
constexpr const char* kDragPayloadUObject = "UEVR_UObject";
constexpr const char* kDragPayloadUClass  = "UEVR_UClass";

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
    // Text-input fallback for drop targets (Object / Class slots) — see
    // render_object_text_input / render_class_text_input. Keyed by prop name
    // so each ObjectProperty has its own buffer that survives frames.
    std::unordered_map<std::string, std::string>     obj_text;
    std::unordered_map<std::string, std::string>     cls_text;
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

    // Short-name fallback — linear scan. O(N) but find_uobject already does
    // a wider linear scan internally for full-name misses, so this is no
    // worse and we're inside a debug menu anyway.
    auto arr = sdk::FUObjectArray::get();
    if (arr == nullptr) return nullptr;
    const auto wq = utility::widen(q);
    const auto count = arr->get_object_count();
    for (int32_t i = 0; i < count; ++i) {
        auto item = arr->get_object(i);
        if (item == nullptr || item->object == nullptr) continue;
        auto obj = reinterpret_cast<sdk::UObject*>(item->object);
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
constexpr int kLiveCallerSlotCount = 4;
struct LiveSlot {
    sdk::UObject* target{};
    std::string target_text;          // text-input fallback for the target
    std::string fn_name;
    sdk::UFunction* resolved{};
    std::string resolved_label;
    std::string resolve_error;
};
static std::array<LiveSlot, kLiveCallerSlotCount> s_live_slots{};

// Forward decls — render_live_caller_slots() uses both render_function_call()
// and render_object_text_input(), defined further down in this TU.
void render_function_call(sdk::UObject* self, sdk::UFunction* fn);
sdk::UObject* render_object_text_input(const char* id, std::string& buf);

void render_live_caller_slots() {
    for (int i = 0; i < kLiveCallerSlotCount; ++i) {
        ImGui::PushID(i);
        utility::ScopeGuard pop_id{[]() { ImGui::PopID(); }};

        ImGui::Text("Slot %d", i + 1);
        ImGui::Indent();

        auto& slot = s_live_slots[i];
        const std::string target_label = slot.target == nullptr
            ? std::string{"[drop UObject here]"}
            : (std::string{"["} + std::to_string((uintptr_t)slot.target) + "] "
               + utility::narrow(slot.target->get_full_name()));
        ImGui::Button(target_label.c_str(), ImVec2{0, 0});
        if (auto dropped = accept_object_drop(); dropped != nullptr) {
            if (slot.target != dropped) {
                slot.resolved = nullptr;
                slot.resolved_label.clear();
                slot.resolve_error.clear();
            }
            slot.target = dropped;
        }
        if (slot.target != nullptr) {
            ImGui::SameLine();
            if (ImGui::SmallButton("clear target")) {
                slot.target = nullptr;
                slot.resolved = nullptr;
                slot.resolved_label.clear();
            }
        }
        // Text-input fallback: type "Character" / "Class /Script/Engine.Character" / "0xADDR".
        ImGui::SameLine();
        if (auto resolved = render_object_text_input("##slot_target", slot.target_text);
            resolved != nullptr) {
            if (slot.target != resolved) {
                slot.resolved = nullptr;
                slot.resolved_label.clear();
                slot.resolve_error.clear();
            }
            slot.target = resolved;
        }

        // Function name + Resolve. Enter triggers Resolve.
        std::array<char, 256> name_buf{};
        const auto copy_n = std::min(slot.fn_name.size(), name_buf.size() - 1);
        std::memcpy(name_buf.data(), slot.fn_name.data(), copy_n);
        bool want_resolve = false;
        if (ImGui::InputText("function name", name_buf.data(), name_buf.size(),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            slot.fn_name.assign(name_buf.data());
            want_resolve = true;
        } else if (std::strcmp(name_buf.data(), slot.fn_name.c_str()) != 0) {
            slot.fn_name.assign(name_buf.data());
            slot.resolved = nullptr;
            slot.resolved_label.clear();
            slot.resolve_error.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Resolve")) want_resolve = true;
        if (want_resolve) {
            slot.resolved = nullptr;
            slot.resolved_label.clear();
            slot.resolve_error.clear();
            if (slot.target == nullptr) {
                slot.resolve_error = "no target";
            } else if (slot.fn_name.empty()) {
                slot.resolve_error = "empty name";
            } else {
                const auto wname = utility::widen(slot.fn_name);
                sdk::UFunction* fn = nullptr;
                if (auto* cls = slot.target->get_class(); cls != nullptr) {
                    fn = cls->find_function(wname.c_str());
                }
                if (fn == nullptr) {
                    slot.resolve_error = "no function with that name on the target's class";
                } else {
                    slot.resolved = fn;
                    slot.resolved_label = utility::narrow(fn->get_full_name());
                }
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
    case L"ByteProperty"_fnv:
    case L"UInt16Property"_fnv:
    case L"Int16Property"_fnv:
    case L"Int8Property"_fnv:
    case L"IntProperty"_fnv:
    case L"UInt32Property"_fnv:
    case L"UIntProperty"_fnv:
    case L"EnumProperty"_fnv: {
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
        // Same drop-target widget for every UObject-flavoured property —
        // the encode step below handles packing into the appropriate
        // (raw pointer / FWeakObjectPtr / FSoftObjectPtr) layout.
        sdk::UObject*& slot = s.objs[prop_name_narrow];
        const auto label = slot == nullptr
            ? std::string{"[drop UObject here] "} + prop_name_narrow
            : std::string{"[obj "} + std::to_string((uintptr_t)slot) + "] " + prop_name_narrow;
        ImGui::Button(label.c_str(), ImVec2{0, 0});
        if (auto dropped = accept_object_drop(); dropped != nullptr) {
            slot = dropped;
        }
        if (slot != nullptr) {
            ImGui::SameLine();
            if (ImGui::SmallButton("clear")) {
                slot = nullptr;
            }
        }
        // Text-input fallback: type a name (full or short) or 0xADDR and
        // press Enter to populate the slot without needing a drag-source.
        // Mirrors the user's Scripts/ImGui.lua object_lookup pattern.
        ImGui::SameLine();
        if (auto resolved = render_object_text_input(("##objtext_" + prop_name_narrow).c_str(),
                                                     s.obj_text[prop_name_narrow]);
            resolved != nullptr) {
            slot = resolved;
        }
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
        break;
    }
    case L"StructProperty"_fnv: {
        // Render up to 4 floats from scratch state; on Call we'll memcpy this
        // into the per-struct layout (FVector 12B, FVector2D 8B, FRotator 12B,
        // FQuat 16B, FLinearColor 16B, FTransform unsupported here).
        auto& v = s.vec[prop_name_narrow];
        const auto sp = (sdk::FStructProperty*)prop;
        const auto strukt = sp != nullptr ? sp->get_struct() : nullptr;
        if (strukt == nullptr) {
            ImGui::Text("StructProperty %s — unknown struct", prop_name_narrow.c_str());
            break;
        }
        const auto sname = utility::narrow(strukt->get_fname().to_string());
        ImGui::Text("[%s]", sname.c_str());
        if (sname == "Vector2D") {
            ImGui::DragFloat2(prop_name_narrow.c_str(), &v.x, 0.1f);
        } else if (sname == "Vector" || sname == "Rotator") {
            ImGui::DragFloat3(prop_name_narrow.c_str(), &v.x, 0.1f);
        } else if (sname == "Quat" || sname == "LinearColor" || sname == "Vector4") {
            ImGui::DragFloat4(prop_name_narrow.c_str(), &v.x, 0.1f);
        } else {
            ImGui::TextDisabled("(unsupported struct in caller — call manually via Lua)");
        }
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
            raw[1] = item->serial_number;
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
            raw[1] = item->serial_number;
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
            raw[1] = item->serial_number;
        } else {
            raw[1] = 0;
        }
        return true;
    }
    case L"StructProperty"_fnv: {
        // Pack the float scratch into the underlying struct layout. We only
        // handle the well-known small POD structs here; everything else stays
        // zero-initialised.
        const auto sp = (sdk::FStructProperty*)prop;
        const auto strukt = sp->get_struct();
        if (strukt == nullptr) return false;
        const auto sname = utility::narrow(strukt->get_fname().to_string());
        auto& v = s.vec[name];
        const bool is_ue5_vec = sdk::ScriptVector::static_struct() != nullptr &&
            sdk::ScriptVector::static_struct()->get_struct_size() == sizeof(glm::vec<3, double>);
        if (sname == "Vector2D") {
            if (is_ue5_vec) {
                *(double*)(out + 0) = (double)v.x;
                *(double*)(out + 8) = (double)v.y;
            } else {
                *(float*)(out + 0) = v.x;
                *(float*)(out + 4) = v.y;
            }
            return true;
        }
        if (sname == "Vector" || sname == "Rotator") {
            if (is_ue5_vec) {
                *(double*)(out + 0)  = (double)v.x;
                *(double*)(out + 8)  = (double)v.y;
                *(double*)(out + 16) = (double)v.z;
            } else {
                *(float*)(out + 0) = v.x;
                *(float*)(out + 4) = v.y;
                *(float*)(out + 8) = v.z;
            }
            return true;
        }
        if (sname == "Quat" || sname == "LinearColor" || sname == "Vector4") {
            if (is_ue5_vec && sname == "Quat") {
                *(double*)(out + 0)  = (double)v.x;
                *(double*)(out + 8)  = (double)v.y;
                *(double*)(out + 16) = (double)v.z;
                *(double*)(out + 24) = (double)v.w;
            } else {
                *(float*)(out + 0)  = v.x;
                *(float*)(out + 4)  = v.y;
                *(float*)(out + 8)  = v.z;
                *(float*)(out + 12) = v.w;
            }
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

// Render `prop` after a call to provide a read-out of the value it holds.
// Used for return values and out parameters.
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
    case L"IntProperty"_fnv:
    case L"EnumProperty"_fnv:        return std::to_string(*(int32_t*)in);
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
        if (item == nullptr || item->object == nullptr) {
            return std::format("<dead weak: idx={}>", obj_index);
        }
        if (item->serial_number != serial) {
            return std::format("<stale weak: idx={}, expected_serial={}, got={}>", obj_index, serial, item->serial_number);
        }
        try {
            return std::format("[{:#x}] {} (weak)", (uintptr_t)item->object, utility::narrow(((sdk::UObject*)item->object)->get_full_name()));
        } catch (...) {
            return std::format("[{:#x}] (weak)", (uintptr_t)item->object);
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
            if (item != nullptr && item->object != nullptr && item->serial_number == serial) {
                try {
                    return std::format("[{:#x}] {} (soft, loaded)", (uintptr_t)item->object, utility::narrow(((sdk::UObject*)item->object)->get_full_name()));
                } catch (...) {
                    return std::format("[{:#x}] (soft)", (uintptr_t)item->object);
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
    case L"DelegateProperty"_fnv:
    case L"MulticastDelegateProperty"_fnv:
    case L"MulticastInlineDelegateProperty"_fnv:
    case L"MulticastSparseDelegateProperty"_fnv:
        return "<delegate>";
    case L"MapProperty"_fnv:
        return "<TMap: read not implemented>";
    case L"SetProperty"_fnv:
        return "<TSet: read not implemented>";
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
        auto fmt4 = [in]() {
            return std::format("({:.3f}, {:.3f}, {:.3f}, {:.3f})", *(float*)(in+0), *(float*)(in+4), *(float*)(in+8), *(float*)(in+12));
        };
        if (sname == "Vector" || sname == "Rotator") return "[" + sname + "] " + fmt3();
        if (sname == "Vector2D")                     return "[Vector2D] " + fmt2();
        if (sname == "Vector4" || sname == "Quat" || sname == "LinearColor") return "[" + sname + "] " + fmt4();
        return "<struct " + sname + ">";
    }
    default:
        return std::string{"<"} + utility::narrow(pc->get_name().to_string()) + " unsupported>";
    }
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
            ImGui::Text("→ %s", s.return_repr.c_str());
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
        ImGui::Text("→ %s", s.return_repr.c_str());
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

        if (object == nullptr || object->object == nullptr) {
            continue;
        }

        add_new_object(object->object);
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

        if (object != nullptr && object->object != nullptr) {
            first_obj = (sdk::UObject*)object->object;
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

void* UObjectHook::process_event_hook(sdk::UObject* obj, sdk::UFunction* func, void* params, void* r9) {
    auto& hook = UObjectHook::get();

    bool do_heavy_data_once = false;

    if (hook->m_process_event_listening) {
        std::scoped_lock _{hook->m_function_mutex};
        
        auto& data = hook->m_called_functions[func];
        ++data.call_count;

        hook->m_most_recent_functions.push_front(func);

        if (hook->m_most_recent_functions.size() > 200) {
            hook->m_most_recent_functions.pop_back();
        }
    }

    auto result = hook->m_process_event_hook.unsafe_call<void*>(obj, func, params, r9);

    if (hook->m_process_event_listening) {
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
                        std::shared_lock _{m_mutex};

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

    if (!set_defaults && m_enabled_at_startup->value()) {
        m_wants_activate = true;
    }
}

void UObjectHook::on_config_save(utility::Config& cfg) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
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

        if (state.adjusting) {
            // Create a temporary actor that visualizes how we're adjusting the component
            if (state.adjustment_visualizer == nullptr) {
                auto ugs = sdk::UGameplayStatics::get();
                auto visualizer = ugs->spawn_actor(sdk::UGameEngine::get()->get_world(), sdk::AActor::static_class(), orig_position);

                if (visualizer != nullptr) {
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

                    std::unique_lock _{m_mutex};
                    state.adjustment_visualizer = visualizer;
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

            comp->set_world_location(adjusted_location, false, false);
            comp->set_world_rotation(adjusted_euler, false, false);
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

            auto obj = prop_base->path.resolve();

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
                default:
                    // OH NO!!!!! anyways
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

    case "PersistentLevel"_fnv: {

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

            for (auto comp : components) {
                const auto& comp_fname = comp->get_fname();
                const auto comp_name = comp_fname.to_string_remove_numbers();
                const auto comp_ends_with_number = comp_fname.get_number() != 0;

                const auto comp_expanded_name = utility::narrow(comp->get_class()->get_fname().to_string() + L" " + comp_name);
                const auto is_match = comp_ends_with_number ? next_it->starts_with(comp_expanded_name) 
                                                            : *next_it == comp_expanded_name;

                if (is_match) {
                    previous_data = comp;
                    previous_data_desc = comp->get_class();
                    ++it;
                    break;
                }
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

                bool found = false;

                // Now look for the object in the array
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
                        found = true;
                        previous_data = obj;
                        previous_data_desc = obj->get_class();
                        ++it;
                        ++it;
                        break;
                    }
                }

                if (!found) {
                    return nullptr;
                }
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

    // Dockable pop-out windows live OUTSIDE the sidebar tree (which only
    // renders when the user has UObjectHook focused as a sidebar entry).
    // We draw them every imgui frame instead, gated on their toggle bools.
    // Both windows auto-attach to Framework's main dockspace via
    // SetNextWindowDockID(FirstUseEver) just like Lua imgui.begin_window.
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

    if (!ImGui::BeginTabBar("ClassBrowserTabs")) {
        return;
    }
    utility::ScopeGuard tab_guard{[]() { ImGui::EndTabBar(); }};

    // ---- Classes tab -------------------------------------------------------
    if (ImGui::BeginTabItem("Classes")) {
        ImGui::TextDisabled("%zu classes total — drag into a Class slot or click to inspect",
            m_sorted_classes.size());

        // Native vs Blueprint split:
        //   Native:    full name contains "/Script/"   (engine + game C++ modules)
        //   Blueprint: anything else                   (typically /Game/... assets)
        // We render the *same* m_sorted_classes through this lambda twice with
        // opposite predicates so we don't have to keep two sorted lists.
        auto render_class_list = [&](const char* child_id, bool want_native) {
            if (ImGui::BeginChild(child_id, ImVec2(0, 0), ImGuiChildFlags_Borders)) {
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
                    const auto narrow = utility::narrow(full);
                    ImGui::PushID(uclass);
                    // Click → open dedicated Class Inspector window for this
                    // class. We don't deduplicate (clicking the same class
                    // twice just brings the existing window to focus next
                    // frame because ImGui keys the window by ###addr).
                    if (ImGui::Selectable(narrow.c_str())) {
                        if (std::find(m_open_class_inspectors.begin(), m_open_class_inspectors.end(), uclass)
                                == m_open_class_inspectors.end()) {
                            m_open_class_inspectors.push_back(uclass);
                        }
                    }
                    // Drag source for the UEVR_UClass payload that the function
                    // caller's Class drop targets accept.
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("UEVR_UClass", &uclass, sizeof(uclass));
                        ImGui::Text("UClass: %s", narrow.c_str());
                        ImGui::EndDragDropSource();
                    }
                    ImGui::PopID();
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

        if (ImGui::BeginTabBar("ClassesSubTabs")) {
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

    // ---- ScriptStructs tab -------------------------------------------------
    if (ImGui::BeginTabItem("ScriptStructs")) {
        static const auto script_struct_class = sdk::UScriptStruct::static_class();
        ImGui::TextDisabled("walks FUObjectArray looking for UScriptStruct instances");
        if (ImGui::BeginChild("ss_list", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            if (script_struct_class == nullptr) {
                ImGui::Text("UScriptStruct::static_class() returned null");
            } else {
                auto arr = sdk::FUObjectArray::get();
                const auto count = arr ? arr->get_object_count() : 0;
                int shown = 0;
                for (int32_t i = 0; i < count && shown < 5000; ++i) {
                    auto item = arr->get_object(i);
                    if (item == nullptr || item->object == nullptr) continue;
                    auto obj = (sdk::UObject*)item->object;
                    auto cls = obj->get_class();
                    if (cls == nullptr || !cls->is_a(script_struct_class)) continue;
                    std::wstring full;
                    try { full = obj->get_full_name(); } catch (...) { continue; }
                    if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                    const auto narrow = utility::narrow(full);
                    ImGui::PushID(obj);
                    ImGui::Selectable(narrow.c_str());
                    if (ImGui::BeginDragDropSource()) {
                        // UScriptStruct is a UObject, publish as UObject so
                        // generic Object drop targets accept it. Specialised
                        // struct-drop targets can sniff is_a(UScriptStruct).
                        ImGui::SetDragDropPayload("UEVR_UObject", &obj, sizeof(obj));
                        ImGui::Text("UScriptStruct: %s", narrow.c_str());
                        ImGui::EndDragDropSource();
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

    // ---- Enums tab ---------------------------------------------------------
    if (ImGui::BeginTabItem("Enums")) {
        static const auto enum_class = sdk::find_uobject<sdk::UClass>(L"Class /Script/CoreUObject.Enum");
        ImGui::TextDisabled("walks FUObjectArray looking for UEnum instances");
        if (ImGui::BeginChild("enum_list", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            if (enum_class == nullptr) {
                ImGui::Text("CoreUObject.Enum not found");
            } else {
                auto arr = sdk::FUObjectArray::get();
                const auto count = arr ? arr->get_object_count() : 0;
                int shown = 0;
                for (int32_t i = 0; i < count && shown < 5000; ++i) {
                    auto item = arr->get_object(i);
                    if (item == nullptr || item->object == nullptr) continue;
                    auto obj = (sdk::UObject*)item->object;
                    auto cls = obj->get_class();
                    if (cls == nullptr || !cls->is_a(enum_class)) continue;
                    std::wstring full;
                    try { full = obj->get_full_name(); } catch (...) { continue; }
                    if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                    const auto narrow = utility::narrow(full);
                    ImGui::PushID(obj);
                    ImGui::Selectable(narrow.c_str());
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("UEVR_UObject", &obj, sizeof(obj));
                        ImGui::Text("UEnum: %s", narrow.c_str());
                        ImGui::EndDragDropSource();
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
            auto arr = sdk::FUObjectArray::get();
            const auto count = arr ? arr->get_object_count() : 0;
            int shown = 0;
            const int kFnCap = has_filter ? 5000 : 500;
            for (int32_t i = 0; i < count && shown < kFnCap; ++i) {
                auto item = arr->get_object(i);
                if (item == nullptr || item->object == nullptr) continue;
                auto obj = (sdk::UObject*)item->object;
                auto cls = obj->get_class();
                if (cls == nullptr || !cls->is_a(func_class)) continue;
                std::wstring full;
                try { full = obj->get_full_name(); } catch (...) { continue; }
                if (has_filter && full.find(wfilter) == std::wstring::npos) continue;
                const auto narrow = utility::narrow(full);
                ImGui::PushID(obj);
                ImGui::Selectable(narrow.c_str());
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

    // ---- Default Object tab -----------------------------------------------
    if (ImGui::BeginTabItem("Default Object")) {
        auto cdo = cls->get_class_default_object();
        if (cdo == nullptr) {
            ImGui::TextDisabled("class has no CDO");
        } else {
            // ui_handle_properties iterates the super chain and renders each
            // property with the editor / drop targets the rest of UObjectHook
            // already uses. Pass the CDO as the object so reads see real
            // default values instead of zero-initialized memory.
            ui_handle_properties(cdo, cls);
        }
        ImGui::EndTabItem();
    }

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

    // ---- Raw inspect tab (full ui_handle_object) --------------------------
    if (ImGui::BeginTabItem("Raw")) {
        // ui_handle_object on a UClass shows the Default Object subtree,
        // Outer, plus any specialised handlers (material, widget component
        // patcher, etc.). Useful as an "everything-else" fallback.
        ui_handle_object((sdk::UObject*)cls);
        ImGui::EndTabItem();
    }

    if (!open) {
        std::erase(m_open_class_inspectors, cls);
    }
}

void UObjectHook::draw_function_caller_window() {
    uobjecthook_dock_into_host_once();
    if (!ImGui::Begin("UEVR Function Caller", &m_show_function_caller)) {
        ImGui::End();
        return;
    }
    utility::ScopeGuard end_guard{[]() { ImGui::End(); }};

    ImGui::TextDisabled("Same slots as the inline caller in UObjectHook → Main.");
    ImGui::TextDisabled("State is shared; drop UObjects from the Class Browser, the in-tree views, or type names.");
    ImGui::Separator();
    render_live_caller_slots();
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
}

void UObjectHook::draw_developer() {
    if (ImGui::Button("Dump SDK")) {
        SDKDumper::dump();
    }
    ImGui::Text("Constructor calls: %llu", m_debug.constructor_calls);
    ImGui::Text("Destructor calls: %llu", m_debug.destructor_calls);

    ImGui::Separator();
    if (!m_attempted_hook_process_event) {
        if (ImGui::Button("Create ProcessEvent hook")) {
            GameThreadWorker::get().enqueue([this]() {
                hook_process_event();
            });
        }
    } else if (m_hooked_process_event) {
        ImGui::Checkbox("ProcessEvent Listener", &m_process_event_listening);
        

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
                }

                ImGui::TreePop();
            }

            if (ImGui::TreeNode("All Called Functions")) {
                ImGui::SliderInt("Max Calls", &m_process_event_search.max_calls, 0, 10000);
                ImGui::InputText("Search", m_process_event_search.buffer.data(), m_process_event_search.buffer.size());

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

                for (auto& ufunc : functions_sorted_by_call_count) {
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
                    
                    const auto made = ImGui::TreeNode(utility::narrow(ufunc->get_full_name()).c_str());

                    ImGui::SameLine();
                    ImGui::Text(" (%llu)", m_called_functions[ufunc].call_count);

                    if (made) {
                        auto& data = m_called_functions[ufunc];
                        data.wants_heavy_data = true;

                        if (data.heavy_data != nullptr) {
                            // Param inspector.
                            ui_handle_struct(data.heavy_data->params.data(), ufunc);
                        }

                        ImGui::TreePop();
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
    ImGui::Checkbox("Class Browser window", &m_show_class_browser);
    ImGui::SameLine();
    ImGui::Checkbox("Function Caller window", &m_show_function_caller);
    ImGui::Separator();

    // Live Function Caller — pinned workbench-style widget that sits ABOVE the
    // deep object tree so the user does not have to drill through
    // Objects-by-class → SomeUClass → SomeObject → Functions → fn each time
    // they want to invoke a function. Same slot state (s_live_slots in the
    // anonymous namespace) is shared with the dockable pop-out window, so
    // changes made in either surface stay in sync.
    ImGui::SetNextItemOpen(true, ImGuiCond_Once); // open on first show for discoverability
    if (ImGui::TreeNode("Live Function Caller (inline)")) {
        ImGui::TextDisabled("Drop a UObject / type a name or 0xADDR; type the function name; Enter or Resolve.");
        ImGui::TextDisabled("State is shared with the pop-out 'Function Caller window' above.");
        ImGui::Separator();
        render_live_caller_slots();
        ImGui::TreePop();
    }

    if (!m_motion_controller_attached_components.empty()) {

        if (ImGui::TreeNode("Attached Components")) {
            if (ImGui::Button("Detach all")) {
                m_motion_controller_attached_components.clear();

                for (auto persistent_state : m_persistent_states) {
                    if (persistent_state != nullptr) {
                        persistent_state->erase_json_file();
                    }
                }

                m_persistent_states.clear();
            }

            // make a copy because the user could press the detach button while iterating
            auto attached = m_motion_controller_attached_components;

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
    ImGuiID CommonID = ImGui::GetID("Common Objects");
    // Display common objects like things related to the player
    if (ImGui::TreeNode("Common Objects")) {
        ImGui::TreeNodeSetOpen(ImGui::GetID("Objects By class"), false);
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
        ImGui::TreeNodeSetOpen(CommonID, false);
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

                if (ImGui::InputText("Add Component Permanently", component_add_name, sizeof(component_add_name), ImGuiInputTextFlags_::ImGuiInputTextFlags_EnterReturnsTrue)) {
                    const auto component_c = sdk::find_uobject<sdk::UClass>(utility::widen(component_add_name));

                    if (component_c != nullptr) {
                        m_on_creation_add_component_jobs[uclass] = [this, component_c](sdk::UObject* object) {
                            if (!this->exists(object)) {
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
                    } else {
                        strcpy_s(component_add_name, "Nonexistent component");
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
        if (ImGui::Button("Test lua")) {

                      const auto hex = (std::stringstream{} << std::hex << (uintptr_t)object).str();
       


                std::string_view lua_text = "local obj =  uevr.api:to_uobject(tonumber(" + hex + "), 16)); print(tostring(obj))";
                         //"local obj =  uevr.api:to_uobject(" + hex + ")"+
                         //   R"(
                         //       local root = obj.GetRootComponent and obj:GetRootComponent()
                         //       if root then 
                         //           for i = 1, root:GetNumChildrenComponents() do
                         //                  local comp = root:GetChildComponent(i)
                         //                  if comp then comp:K2_DetachFromComponent(1,1,1, true)
                         //                   comp:K2_DestroyComponent()
                         //                   end
                         //            end
                         //            root:K2_DestroyComponent()
                         //            obj:K2_DestroyActor()
                         //       elseif obj.K2_DestroyComponent then
                         //           obj:K2_DestroyComponent()
                         //        end                                 

                         //       )";
                         //  
                PluginLoader::get()->do_lua_string(lua_text.data(), "destroy");
                    
               
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

    ImGui::Text("%s", utility::narrow(object->get_full_name()).data());

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
            auto comp = (sdk::UActorComponent*)object;

            comp->destroy_component();
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
          
            // Per-component lua console.
            // The previous implementation had four serious bugs stacked on top of each other:
            //   - `static std::string_view text = std::string + std::string` left a string_view
            //     pointing at the temporary's destroyed buffer.
            //   - `char* input{}` was an uninitialized null pointer.
            //   - `strcpy_s(input, sizeof(text.data()), text.data())` copied into nullptr with a
            //     size of sizeof(const char*) = 8.
            //   - `ImGui::InputTextMultiline(..., input, sizeof(input), ...)` passed buf=nullptr
            //     and size=8 to imgui.
            // Replaced with a per-component buffer indexed by component address.
            const auto hex = (std::stringstream{} << std::hex << (uintptr_t)comp).str();
            if (ImGui::TreeNode("Lua")) {
                constexpr size_t kBufSize = 4096;
                static std::unordered_map<uintptr_t, std::array<char, kBufSize>> s_buffers;
                auto [it, inserted] = s_buffers.try_emplace((uintptr_t)comp);
                if (inserted) {
                    const auto initial = std::string{"local comp = uevr.api:to_uobject(0x"} + hex + ")\n";
                    const auto n = std::min(initial.size(), kBufSize - 1);
                    std::memcpy(it->second.data(), initial.data(), n);
                    it->second[n] = '\0';
                }
                auto& buf = it->second;

                const auto size = ImGui::GetContentRegionAvail();
                ImGui::InputTextMultiline("##luainput", buf.data(), buf.size(),
                    ImVec2(size.x, std::max(120.0f, size.y * 0.5f)),
                    ImGuiInputTextFlags_AllowTabInput);
                if (ImGui::Button("Execute")) {
                    PluginLoader::get()->do_lua_string(buf.data(), utility::narrow(comp->get_full_name()).data());
                }
                ImGui::TreePop();
            }
  
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
            //static sdk::FName* attach_socket{};
            //static std::vector<std::wstring> socket_combo={L"None"};
            //if (socket_combo.size() == 1) {
            //

            if (ImGui::Button("Attach Camera to head")) {
                
            auto head_socket = L"None";
            auto neck_socket = L"None";
            for (auto& n : comp->get_all_socket_names()) {
                const auto& name = n;
                if (name.to_string().contains(L"head")) {
                    head_socket = name.to_string().c_str();
                } else if (name.to_string().contains(L"neck")) {
                    neck_socket = name.to_string().c_str();
                }
            }

            void* addr = (void*)((uintptr_t)comp);
            const auto hex = (std::stringstream{} << std::hex << (uintptr_t)addr).str();
            static std::string_view text = "_G.uobjecthook_cache = _G.uobjecthook_cache or {}\n local function attach_to_head()\nlocal comp = uevr.api:to_uobject(" + hex + ")" +
                     "\nlocal head_socket_name = \"" +
                                           utility::narrow(head_socket) +
                                           "\"" + 
                "\nlocal neck_socket_name = \"" +
                                           utility::narrow(neck_socket) + "\"" +
                     R"(
                            local ukismetmath = uevr.api:find_uobject("Class /Script/Engine.KismetMathLibrary"):get_class_default_object()
                            local pc = uevr.api:get_player_controller(0)
                            local spring = uevr.api:add_component_by_class(pc, uevr.api:find_uobject("Class /Script/Engine.SpringArmComponent"), false)
                            local cam_actor = uevr.api:spawn_object(uevr.api:find_uobject("Class /Script/Engine.CameraActor"), pc)
                            spring:K2_AttachToComponent(comp, neck_socket_name, 2, 2, 2, true)
                            spring.TargetArmLength = 12
                            spring.SocketOffset = Vector3f.new(0, 0, 0)
                            spring.TargetOffset = comp:GetSocketTransform(head_socket_name, 1).Translation -  comp:GetSocketTransform(neck_socket_name, 1).Translation
                            spring.bDoCollisionTest = true
                            spring.ProbeSize = 12.001
                            spring.bUsePawnControlRotation = true
                            spring.bEnableCameraLag = false
                            spring.bEnableCameraRotationLag = true
                            spring.CameraRotationLagSpeed = 55.0
                            local new_cam_comp = cam_actor.CameraComponent
                            new_cam_comp:K2_AttachToComponent(spring,
                                ("SpringEndpoint"), 2, 0, 2, true)
                            new_cam_comp.bUsePawnControlRotation = true
                            new_cam_comp.bShouldSnapRotationWhenAttached = true
                            new_cam_comp.bShouldBeAttached = true
                            new_cam_comp.RelativeLocation.Z = 10.5
                        pc:ClientSetViewTarget(cam_actor, {})
                        end
                        print("from c++")
                        attach_to_head()
                    )";

                //m_camera_attach.object = comp;
                //m_camera_attach.offset = glm::vec3{0.0f, 0.0f, 0.0f};
                PluginLoader::get()->do_lua_string(text.data(), "attach_vt");

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

    glm::vec3  loc = comp->get_world_location();
    glm::vec3 rot = comp->get_world_rotation();

    // Finetuning of the controller rotation offset
    // Convert to pitch/yaw/roll first.
    std::string_view rotid = "World Rotation##" + hex;
    std::string_view locid = "World Location##" + hex;

    ImGui::PushID(rotid.data());
    if (ImGui::DragFloat3(rotid.data(), &rot.x, 0.1f)) {
        comp->set_world_rotation(rot, true, false);
    }
    ImGui::PopID();
    ImGui::PushID(locid.data());
    if (ImGui::DragFloat3(locid.data(), &loc.x, 0.1f)) {
        comp->set_world_location(rot, true, false);
    }
    ImGui::PopID();
    ImGui::PushID("CamOffset");
 if (ImGui::DragFloat3("Camera Offset", &m_camera_attach.offset.x, 0.1f)) {
        if (m_persistent_camera_state != nullptr) {
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
                if (existing_prop->path.resolve() == comp) {
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
                if (existing_prop->path.resolve() == comp) {
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
            if (existing_prop->path.resolve() == comp) {
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

        std::sort(components.begin(), components.end(), [](sdk::UObject* a, sdk::UObject* b) {
            return a->get_full_name() < b->get_full_name();
        });

        for (auto comp : components) {
            auto comp_obj = (sdk::UObject*)comp;

            ImGui::PushID(comp_obj);
            // not using full_name because its HUGE
            std::wstring comp_name = comp->get_class()->get_fname().to_string() + L" " + comp->get_fname().to_string();
            const auto made = ImGui::TreeNode(utility::narrow(comp_name).data());

            if (made) {
                auto scope2 = m_path.enter(utility::narrow(comp_name));
                ui_handle_object(comp_obj);
                ImGui::TreePop();
            }

            ImGui::PopID();
        }

        ImGui::TreePop();
    }
}

void UObjectHook::ui_handle_functions(void* object, sdk::UStruct* uclass) {
    if (uclass == nullptr) {
        return;
    }

    const bool is_real_object = object != nullptr && m_objects.contains((sdk::UObject*)object);
    auto object_real = (sdk::UObject*)object;

    std::vector<sdk::UFunction*> sorted_functions{};
    static const auto ufunction_t = sdk::UFunction::static_class();

    for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
        auto funcs = super->get_children();

        for (auto func = funcs; func != nullptr; func = func->get_next()) {
            if (func->get_class()->is_a(ufunction_t)) {
                sorted_functions.push_back((sdk::UFunction*)func);
            }
        }
    }

    std::sort(sorted_functions.begin(), sorted_functions.end(),
        [](sdk::UFunction* a, sdk::UFunction* b) { return a->get_fname().to_string() < b->get_fname().to_string(); });

    for (auto func : sorted_functions) {
        ImGui::PushID((void*)func);

        utility::ScopeGuard pop_guard{[]() { ImGui::PopID(); }};


        ui_standard_object_context_menu(func);

        if (m_called_functions.contains(func)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4{0.0f, 1.0f, 0.0f, 1.0f}, "[Called]");
        }

        if (ImGui::TreeNode(utility::narrow(func->get_fname().to_string()).data())) {
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
            }

            ImGui::TreePop();
        }
    }
}

void UObjectHook::ui_handle_properties(void* object, sdk::UStruct* uclass) {

    auto previous_path = m_path;

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

    std::vector<sdk::FField*> sorted_fields{};

    for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
        auto props = super->get_child_properties();

        for (auto prop = props; prop != nullptr; prop = prop->get_next()) {
            sorted_fields.push_back(prop);
        }
    }

    std::sort(sorted_fields.begin(), sorted_fields.end(), [](sdk::FField* a, sdk::FField* b) {
        return a->get_field_name().to_string() < b->get_field_name().to_string();
    });

    for (auto prop : sorted_fields) {
        auto propc = prop->get_class();
        const auto propc_type = propc->get_name().to_string();

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
        // Right-click lambda for supported properties, usually for saving.
        auto display_context = [&](auto value) {
            if (!ImGui::BeginPopupContextItem()) {
                return;
            }

            if (!previous_path.has_valid_base()) {
                ImGui::Text("Can't save, did not start from a valid base");
                ImGui::EndPopup();
                return;
            }

            auto save_logic = [&](bool unsave = false) {
                const auto field_name = prop->get_field_name().to_string();
                std::shared_ptr<PersistentProperties> props{};

                // Find existing one if possible
                for (const auto& existing_prop : m_persistent_properties) {
                    if (existing_prop->path.resolve() == object) {
                        props = existing_prop;
                        break;
                    }
                }

                // Add new one if necessary
                if (props == nullptr) {
                    props = std::make_shared<PersistentProperties>();
                    props->path = StatePath{previous_path.path()};
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
                
                // Concat the entire path together and hash it to get a unique name
                std::string concat_path{};
                for (const auto& p : previous_path.path()) {
                    concat_path += p;
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
        

            ImGui::EndPopup();
        };
      
        const auto& prop_name =  utility::narrow(prop->get_field_name().to_string());
        sdk::FProperty* fprop = ((sdk::FProperty*)prop);

        
        switch (hash_type) {


        case L"FloatProperty"_fnv:
            {
                auto& value = *(float*)((uintptr_t)object + fprop->get_offset());
                ImGui::DragFloat(prop_name.data(), &value, 0.01f);
                display_context(value);
            }
            break;
        case L"DoubleProperty"_fnv:
            {
                auto& value = *(double*)((uintptr_t)object + fprop->get_offset());
                float casted_value = (float)value;
                if (ImGui::DragFloat(prop_name.data(), (float*)&casted_value, 0.01f)) {
                    value = (double)casted_value;
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
                }
                display_context(value);
            }
            break;
        case L"UInt32Property"_fnv:
        case L"IntProperty"_fnv:
            {
                auto& value = *(int32_t*)((uintptr_t)object + fprop->get_offset());
            ImGui::DragInt(prop_name.data(), &value, 1);
                display_context(value);
            }
            break;
        case L"UInt64Property"_fnv:
            {
                auto& value = *(uint64_t*)((uintptr_t)object + fprop->get_offset());
            ImGui::DragScalar(prop_name.data(), ImGuiDataType_U64, &value, 1);
                display_context(value);
            }
            break;
        case L"BoolProperty"_fnv:
            {
                auto boolprop = (sdk::FBoolProperty*)prop;
                auto value = boolprop->get_value_from_object(object);
                if (ImGui::Checkbox(prop_name.data(), &value)) {
                    boolprop->set_value_in_object(object, value);
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
                }
                display_context(value);
            }
            break;
        case L"InterfaceProperty"_fnv:
        case L"ObjectProperty"_fnv:
        case L"ClassProperty"_fnv:
            {
                auto& value = *(sdk::UObject**)((uintptr_t)object + fprop->get_offset());

                if (ImGui::TreeNode(prop_name.data())) {
                    auto scope2 = m_path.enter(prop_name);
                    ui_handle_object(value);
                    if (ImGui::BeginPopupContextItem()) {
                        edit_property_flags(utility::narrow(prop->get_field_name().to_string()), (sdk::FProperty*)prop);
                        ImGui::EndPopup();
                    }
                    ImGui::TreePop();
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
                        if (item->object != nullptr && item->serial_number == serial) {
                            resolved = (sdk::UObject*)item->object;
                        } else {
                            stale = true;
                        }
                    }
                }
                if (resolved != nullptr) {
                    if (ImGui::TreeNode(prop_name.data())) {
                        auto scope2 = m_path.enter(prop_name);
                        ui_handle_object(resolved);
                        ImGui::TreePop();
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
                        if (item->object != nullptr && item->serial_number == serial) {
                            resolved = (sdk::UObject*)item->object;
                        }
                    }
                }
                if (resolved != nullptr) {
                    if (ImGui::TreeNode(prop_name.data())) {
                        auto scope2 = m_path.enter(prop_name);
                        ui_handle_object(resolved);
                        ImGui::TreePop();
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
        case L"DelegateProperty"_fnv:
        case L"MulticastDelegateProperty"_fnv:
        case L"MulticastInlineDelegateProperty"_fnv:
        case L"MulticastSparseDelegateProperty"_fnv:
            ImGui::Text("%s: ", prop_name.data());
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("<delegate>");
            break;
        case L"MapProperty"_fnv:
            ImGui::Text("%s: ", prop_name.data());
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("<TMap (display TODO)>");
            break;
        case L"SetProperty"_fnv:
            ImGui::Text("%s: ", prop_name.data());
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("<TSet (display TODO)>");
            break;
        case L"StructProperty"_fnv:
            {
                void* addr = (void*)((uintptr_t)object + fprop->get_offset());

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
                    edit_property_flags(utility::narrow(prop->get_field_name().to_string()), (sdk::FProperty*)prop);
                    ImGui::EndPopup();
                }

                if (ImGui::TreeNode(prop_name.data())) {
                    auto scope2 = m_path.enter(utility::narrow(prop->get_field_name().to_string()));
                    ui_handle_struct(addr, ((sdk::FStructProperty*)prop)->get_struct());
                    ImGui::TreePop();
                }
            }
            break;
        case L"Function"_fnv:
            break;
        case L"ArrayProperty"_fnv:
            if (ImGui::TreeNode(prop_name.data())) {
                auto scope2 = m_path.enter(prop_name);
                ui_handle_array_property(object, (sdk::FArrayProperty*)prop);
                ImGui::TreePop();
            }

            break;
        case L"NameProperty"_fnv:
            {
                const auto& value = *(sdk::FName*)((uintptr_t)object + fprop->get_offset());
                const auto wstr = value.to_string();
                const auto str = utility::narrow(wstr);

                ImGui::Text("%s: ", prop_name.data());
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4{3.0f / 255.0f, 232.0f / 255.0f, 252.0f / 255.0f, 1.0f}, "%s", str.data());
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
        std::string prop_name_edit = prop_name + "EditFlags";                                            
        if (ImGui::TreeNode(prop_name_edit.c_str())){
                edit_property_flags(utility::narrow(prop->get_field_name().to_string()), (sdk::FProperty*)prop);
                ImGui::TreePop();    
        }
    }
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
        const auto& array_obj = *(sdk::TArray<sdk::FName*>*)((uintptr_t)addr + prop->get_offset());

        for (auto obj : array_obj) {
        const auto wstr = obj->to_string();
        const auto str = utility::narrow(wstr);

        ImGui::Text("%s: ", str.data());
    }
     break;
    }
    case "InterfaceProperty"_fnv:
    case "ObjectProperty"_fnv:
    {
        const auto& array_obj = *(sdk::TArray<sdk::UObject*>*)((uintptr_t)addr + prop->get_offset());

        for (auto obj : array_obj) {
            std::wstring name = obj->get_class()->get_fname().to_string() + L" " + obj->get_fname().to_string();
            const auto narrow_name = utility::narrow(name);

            if (ImGui::TreeNode(narrow_name.data())) {
                auto scope = m_path.enter(narrow_name);
                ui_handle_object(obj);
                ImGui::TreePop();
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
            if (item == nullptr || item->object == nullptr || item->serial_number != serial) {
                ImGui::BulletText("[%d] <stale weak idx=%d>", i, obj_index);
                continue;
            }
            auto obj = (sdk::UObject*)item->object;
            const auto label = std::format("[{}] {} {}", i,
                utility::narrow(obj->get_class()->get_fname().to_string()),
                utility::narrow(obj->get_fname().to_string()));
            if (ImGui::TreeNode(label.c_str())) {
                ui_handle_object(obj);
                ImGui::TreePop();
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
                    if (item->object != nullptr && item->serial_number == serial) {
                        resolved = (sdk::UObject*)item->object;
                    }
                }
            }
            if (resolved != nullptr) {
                const auto label = std::format("[{}] {} {} (soft, loaded)", i,
                    utility::narrow(resolved->get_class()->get_fname().to_string()),
                    utility::narrow(resolved->get_fname().to_string()));
                if (ImGui::TreeNode(label.c_str())) {
                    ui_handle_object(resolved);
                    ImGui::TreePop();
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
    default:
        ImGui::Text("Array of %s (unsupported)", inner_c_type.data());
        break;
    };
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
        std::string concat_path{};
        for (const auto& p : this->path.path()) {
            concat_path += p;
        }

        const auto hash_str = std::to_string(utility::hash(concat_path)) + "_props.json";
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

    for (const auto& prop : properties) {
        json["properties"].push_back({
            {"name", utility::narrow(prop->name)},
            {"data", prop->data.u64}
        });
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