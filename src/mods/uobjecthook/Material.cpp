// Material apply/edit: per-slot SetMaterial, SetOverlayMaterial, apply_material_to_actor (the
// "Apply to actor" Override/Overlay backend), and ui_handle_material_interface (the material
// interface's own inspector panel) — split out of UObjectHook.cpp purely for file-size organization
// (that file was 13k lines). Still UObjectHook:: member function definitions (same class, same
// members, same mutex, same everything) — only WHERE the code lives changed, not what it does. See
// src/mods/uobjecthook/SDKDumper.cpp for the existing precedent of this pattern, and Gizmo.cpp /
// PropertyEditor.cpp / ClassBrowser.cpp / FunctionHooks.cpp for the earlier splits of this kind.

#include <algorithm>

#include <utility/Logging.hpp>
#include <utility/String.hpp>

#include <sdk/UObjectBase.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FField.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/AActor.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/USceneComponent.hpp>

#include <imgui_internal.h>

#include "../UObjectHook.hpp"

// Payload type identifier for the UObject drag-drop payload — matches the constant of the same name
// in UObjectHook.cpp exactly (internal linkage; the anonymous-namespace original isn't visible from
// this translation unit). Both sides publish/accept the same literal string, so this is safe.
constexpr const char* kDragPayloadUObject = "UEVR_UObject";

// Drop target that returns the dropped pointer on the frame it lands, otherwise nullptr. Caller is
// responsible for placing a target widget (Button / Selectable / Text) immediately before calling
// this. Duplicated from UObjectHook.cpp's own copy (small, self-contained, no cross-TU state) rather
// than promoted — see the ClassBrowser.cpp split for the precedent of this pattern.
static sdk::UObject* accept_object_drop() {
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

// Direct SetMaterial(index, material) on one component's one slot — used by both the material's own
// "Apply to actor" section and the gizmo target context menu's per-slot Materials editor.
void UObjectHook::set_material_override_slot(sdk::UActorComponent* comp, int32_t slot, sdk::UObject* material) {
    GameThreadWorker::get().enqueue([this, comp, slot, material]() {
        if (!this->exists(comp) || (material != nullptr && !this->exists(material))) return;
        try {
            auto* set_fn = comp->get_class()->find_function(L"SetMaterial");
            if (set_fn == nullptr) return;
            struct { int32_t index{}; sdk::UObject* material{}; } sp{};
            sp.index = slot;
            sp.material = material;
            comp->process_event(set_fn, &sp);
        } catch (...) {}
    });
}

// Direct SetOverlayMaterial(material) — deliberately NOT tracked in m_overlay_mat_originals (see the
// header comment): this is a persistent user action, not the transient selection-highlight feature,
// and must not get silently reverted by that feature's own restore pass. Pass nullptr to clear.
void UObjectHook::set_material_overlay(sdk::USceneComponent* comp, sdk::UObject* material) {
    GameThreadWorker::get().enqueue([this, comp, material]() {
        if (!this->exists(comp) || (material != nullptr && !this->exists(material))) return;
        try {
            auto* set_fn = comp->get_class()->find_function(L"SetOverlayMaterial");
            if (set_fn == nullptr) return; // UE4 / non-mesh component — silent no-op, same as the highlight feature
            struct { sdk::UObject* mat{nullptr}; } sp{};
            sp.mat = material;
            comp->process_event(set_fn, &sp);
        } catch (...) {}
    });
}

// Applies `material` to every mesh component on `actor` — every material slot as an override, or the
// whole component as an overlay. Single enqueue that does all the process_event calls directly
// (rather than re-enqueuing through set_material_override_slot/set_material_overlay, which are meant
// to be called from the draw thread and each do their own enqueue).
void UObjectHook::apply_material_to_actor(sdk::AActor* actor, sdk::UObject* material, bool as_overlay) {
    if (actor == nullptr || material == nullptr) return;
    GameThreadWorker::get().enqueue([this, actor, material, as_overlay]() {
        if (!this->exists(actor) || !this->exists(material)) return;
        static const auto mesh_component_t = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.StaticMeshComponent");
        if (mesh_component_t == nullptr) return;
        std::vector<sdk::UActorComponent*> comps;
        try { comps = actor->get_all_components(); } catch (...) { return; }
        for (auto* c : comps) {
            if (c == nullptr || c->get_class() == nullptr || !c->get_class()->is_a(mesh_component_t)) continue;
            auto* comp = (sdk::UActorComponent*)c;
            try {
                if (as_overlay) {
                    auto* set_fn = comp->get_class()->find_function(L"SetOverlayMaterial");
                    if (set_fn == nullptr) continue;
                    struct { sdk::UObject* mat{nullptr}; } sp{};
                    sp.mat = material;
                    comp->process_event(set_fn, &sp);
                    continue;
                }
                auto* get_num_fn = mesh_component_t->find_function(L"GetNumMaterials");
                auto* set_material_fn = mesh_component_t->find_function(L"SetMaterial");
                if (get_num_fn == nullptr || set_material_fn == nullptr) continue;
                struct { int32_t num{}; } np{};
                comp->process_event(get_num_fn, &np);
                for (int32_t i = 0; i < np.num; ++i) {
                    struct { int32_t index{}; sdk::UObject* material{}; } sp{};
                    sp.index = i;
                    sp.material = material;
                    comp->process_event(set_material_fn, &sp);
                }
            } catch (...) {}
        }
    });
}


void UObjectHook::ui_handle_material_interface(sdk::UObject* object) {
    if (object == nullptr) {
        return;
    }

    const auto uclass = object->get_class();

    if (uclass == nullptr) {
        return;
    }

    // Targeted per-actor apply — reuses apply_material_to_actor (the same backend the in-world
    // right-click "Materials" menu uses), rather than the blunt "every mesh component in the level"
    // button below. Defaults the target to whatever's currently gizmo-selected so the common
    // select-then-apply flow needs no drag; drop a different actor onto the target row to retarget.
    ImGui::SeparatorText("Apply to actor");
    {
        static sdk::AActor* s_target_actor = nullptr;
        if (s_target_actor == nullptr || !this->exists_unsafe((sdk::UObjectBase*)s_target_actor)) {
            s_target_actor = (m_last_selected != nullptr && this->exists_unsafe((sdk::UObjectBase*)m_last_selected))
                ? m_last_selected->get_owner() : nullptr;
        }
        std::string target_label = "(drop an actor here)";
        if (s_target_actor != nullptr && this->exists_unsafe((sdk::UObjectBase*)s_target_actor)) {
            try { target_label = shorten_object_path(utility::narrow(s_target_actor->get_full_name())); } catch (...) {}
        }
        ImGui::Selectable(("Target: " + target_label).c_str());
        if (auto dropped = accept_object_drop(); dropped != nullptr) {
            static const auto actor_t = sdk::AActor::static_class();
            if (actor_t != nullptr && dropped->get_class() != nullptr && dropped->get_class()->is_a(actor_t)) {
                s_target_actor = (sdk::AActor*)dropped;
            }
        }
        ImGui::BeginDisabled(s_target_actor == nullptr || !this->exists_unsafe((sdk::UObjectBase*)s_target_actor));
        if (ImGui::Button("Apply as Override")) {
            apply_material_to_actor(s_target_actor, object, false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply as Overlay")) {
            apply_material_to_actor(s_target_actor, object, true);
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Override replaces every material slot; Overlay uses SetOverlayMaterial (UE5.1+ only).");
    }
    ImGui::Separator();

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
