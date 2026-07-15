// Generic property editor: ui_handle_properties (the big per-property-type switch used by every
// object/struct inspector), ui_handle_array_property, ui_try_known_struct, and ui_handle_struct —
// split out of UObjectHook.cpp purely for file-size organization (that file was 13k lines). Still
// UObjectHook:: member function definitions (same class, same members, same mutex, same everything)
// — only WHERE the code lives changed, not what it does. See src/mods/uobjecthook/SDKDumper.cpp for
// the existing precedent of this pattern, and uobjecthook/Gizmo.cpp for the first split of this kind.

#include <sstream>
#include <cctype>
#include <cmath>
#include <algorithm>
#include <array>
#include <limits>

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
#include <sdk/ScriptVector.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/FObjectProperty.hpp>
#include <sdk/FArrayProperty.hpp>
#include <sdk/FMapProperty.hpp>
#include <sdk/FSetProperty.hpp>
#include <sdk/UMotionControllerComponent.hpp>

#include <imgui_internal.h>
#include "../VR.hpp"
#include "../LuaLoader.hpp"

#include "../UObjectHook.hpp"

// See m_property_edit_target_stack in the header. Called after any property widget in
// ui_handle_properties reports a real edit; forces the nearest enclosing scene component to actually
// re-render by toggling SetVisibility (deferred to the game thread — this is a ProcessEvent call, not
// safe to make directly from the draw thread).
// Flip-then-restore, NOT set-to-current: UE's SetVisibility early-outs when the new value equals the
// current one (bNewVisibility == bVisible), so calling it with the SAME value is a complete no-op —
// no MarkRenderStateDirty, nothing refreshes. That's exactly why the manual "toggle the checkbox
// twice" workaround was needed: two clicks are two DIFFERENT values in a row. Flipping to !vis then
// back to vis reproduces that with two calls, guaranteeing the flag actually changes at least once so
// the refresh fires, while still leaving visibility at its correct value.
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
            comp->set_visibility(!vis, false);
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
    // inheritance depth (most-derived/most-unique class first, root/base last) instead of
    // alphabetically -- class_depth only covers uclass's own chain, so a group key with no depth
    // entry (shouldn't happen since decl is always one of uclass's supers) falls back to alphabetical
    // via the -1 default.
    std::sort(sorted_fields.begin(), sorted_fields.end(), [&](const FieldEntry& a, const FieldEntry& b) {
        if (s_prop_group_mode == 1) {
            const auto it_a = class_depth.find(a.decl);
            const auto it_b = class_depth.find(b.decl);
            const int da = it_a != class_depth.end() ? it_a->second : -1;
            const int db = it_b != class_depth.end() ? it_b->second : -1;
            if (da != db) return da < db; // lower depth = more derived/unique = first
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
        // further down regardless of this group's state. Exception: "By base class" groups for the
        // common low-value base classes (their own fields are rarely what you're looking for when
        // inspecting a specific object) start COLLAPSED instead.
        if (s_prop_group_mode != 0) {
            static const std::unordered_set<std::wstring> kBaseGroupsCollapsedByDefault = {
                L"Object", L"Actor", L"ActorComponent", L"SceneComponent"
            };
            const std::wstring key = (s_prop_group_mode == 1) ? decl->get_fname().to_string() : propc_type;
            if (!any_group_started || key != cur_group_key) {
                cur_group_key = key;
                any_group_started = true;
                const bool default_collapsed = s_prop_group_mode == 1 && kBaseGroupsCollapsedByDefault.contains(key);
                const auto header_flags = default_collapsed ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen;
                cur_group_open = ImGui::CollapsingHeader((utility::narrow(key) + "##propgrp").c_str(), header_flags);
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
