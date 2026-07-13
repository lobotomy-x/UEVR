#pragma once

#include <filesystem>
#include <shared_mutex>
#include <unordered_set>
#include <memory>
#include <deque>
#include <future>
#include <atomic>

#include <nlohmann/json.hpp>

#include <safetyhook.hpp>
#include <utility/PointerHook.hpp>

#include "Mod.hpp"

namespace sdk {
class UObjectBase;
class UObject;
class UClass;
class FFieldClass;
class FStructProperty;
class UScriptStruct;
class USceneComponent;
class UActorComponent;
class AActor;
class FArrayProperty;
}

class UObjectHook : public Mod {
public:
    static std::shared_ptr<UObjectHook>& get();

    // True when a transform gizmo is grabbed / hot under the cursor, or the click-select picker is
    // armed, for THIS frame. Computed in draw_component_gizmos (runs during draw_ui, before the
    // global drag-scroll). The drag-scroll reads it to yield in VR, where it shares the left button
    // with the gizmo axis-drag. Same-thread read, so a plain bool is fine.
    bool is_gizmo_or_picker_busy() const { return m_gizmo_or_picker_busy; }
    // True when the gizmo system needs the VR controller pointer live even over EMPTY space (any gizmo
    // shown, or the picker armed/active). OverlayComponent reads this to inject io.MousePos + clicks
    // off-window so VR users can hover/grab gizmo handles that float on the background draw-list.
    bool wants_vr_pointer() const { return m_has_gizmos || m_gizmo_or_picker_busy; }

    std::unordered_set<sdk::UObjectBase*> get_objects_by_class(sdk::UClass* uclass) const {
        std::shared_lock _{m_mutex};
        if (auto it = m_objects_by_class.find(uclass); it != m_objects_by_class.end()) {
            return it->second;
        }

        return {};
    }

    bool exists(sdk::UObjectBase* object) const {
        std::shared_lock _{m_mutex};
        return exists_unsafe(object);
    }

    void activate();

    bool is_disabled() const {
        return m_uobject_hook_disabled;
    }

    void set_disabled(bool disabled) {
        m_uobject_hook_disabled = disabled;
        m_fixed_visibilities = false;
    }

    bool is_fully_hooked() const {
        return m_fully_hooked;
    }

protected:
    std::string_view get_name() const override { return "UObjectHook"; };
    bool is_advanced_mod() const override { return true; }

    std::vector<SidebarEntryInfo> get_sidebar_entries() override {
        return {
            { "Main", true },
            { "Config", false },
            { "Developer", true }
        };
    }

    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;

    void on_pre_engine_tick(sdk::UGameEngine* engine, float delta) override;
    void on_frame() override;
    void on_draw_sidebar_entry(std::string_view in_entry) override;
    void on_draw_ui() override;

    void draw_config();
    void draw_developer();
    void draw_main();
    void draw_gizmo_options();  // shared gizmo/selection/snap/inspector options (Config tab + pop-out window)
    void draw_options_window(); // dockable pop-out of draw_gizmo_options, drag-dockable next to the main view

    // Screen-space translate gizmo: projects each enabled component's origin +
    // world X/Y/Z axis tips through the engine's own ProjectWorldToScreen and
    // draws colored axis lines on the background draw list. Rendered per frame
    // from on_frame for every component in m_gizmo_components.
    void draw_component_gizmos();

    // Click-to-select: when m_click_select_mode is on, a left click in the world (overlay up,
    // not over a widget) deprojects the cursor to a ray and adds the front-most scene component
    // near that ray to m_gizmo_components. Candidates come from the tracked object set; their
    // world position is composed from the reflected RelativeLocation/Rotation/Scale3D by walking
    // the AttachParent chain (plain memory reads, no per-candidate ProcessEvent), so attached
    // components with large relative offsets are ranked at their true screen position too.
    void handle_click_select();

    // Dockable pop-out windows (rendered from on_frame). Toggle via
    // checkboxes at the top of draw_main. Both auto-attach to the UEVR
    // main dockspace host on first show.
    void draw_class_browser_window();
    void draw_function_caller_window();
    // Standalone pop-out of draw_main() — the sidebar "Main" page content (selected-object inspector,
    // attached components, spawn actor, ...) in its own dockable window, reachable via checkbox/hotkey
    // independent of which sidebar tab is selected. Same lifecycle/pattern as the two windows above.
    void draw_main_window();
    // Refresh m_sorted_classes by relaunching the async sort if needed and
    // harvesting any completed task. Called by both the Objects-by-Class
    // view and the Class Browser so the Class Browser populates on its
    // own (it previously depended on the user opening Objects-by-Class
    // first). Throttled internally via m_last_sort_time.
    void pump_class_sort_task();
    // Dedicated dockable inspector window per UClass. Clicking a class row
    // in the Class Browser appends to m_open_class_inspectors; the X on the
    // window removes it. Multiple inspectors can be open side by side for
    // comparing classes. Window title includes the class address to keep
    // ImGui IDs unique even when two classes share short names.
    void draw_class_inspector_window(sdk::UClass* cls);
    std::vector<sdk::UClass*> m_open_class_inspectors{};
    bool m_show_class_browser{false};
    bool m_show_function_caller{false};
    bool m_show_options_window{false}; // dockable pop-out of the gizmo/selection options
    bool m_show_main_window{false};    // dockable pop-out of draw_main() (the sidebar "Main" page)
    // Filter buffer for the class browser (shared across tabs)
    std::string m_class_browser_filter{};
    bool m_all_objects_hide_default{true};      // "All Objects" tab: hide CDOs ("Default__" name prefix)
    bool m_all_objects_hide_gen_variable{true}; // "All Objects" tab: hide Blueprint GEN_VARIABLE default-value holder objects

    void on_pre_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation,
                                             const float world_to_meters, Vector3f* view_location, bool is_double) override;

    void on_post_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation,
                                                      const float world_to_meters, Vector3f* view_location, bool is_double) override;

public:
    struct MotionControllerStateBase {
        enum Hand : uint8_t {
            LEFT = 0,
            RIGHT,
            HMD,
            LAST
        };

        nlohmann::json to_json() const;
        void from_json(const nlohmann::json& data);

        MotionControllerStateBase& operator=(const MotionControllerStateBase& other) = default;

        // State that can be parsed from disk
        glm::quat rotation_offset{glm::identity<glm::quat>()};
        glm::vec3 location_offset{0.0f, 0.0f, 0.0f};
        uint8_t hand{(uint8_t)Hand::RIGHT}; // 2 == HMD
        bool permanent{false};
    };

    // Assert if MotionControllerStateBase is not trivially copyable
    static_assert(std::is_trivially_copyable_v<MotionControllerStateBase>);
    static_assert(std::is_trivially_destructible_v<MotionControllerStateBase>);
    static_assert(std::is_standard_layout_v<MotionControllerStateBase>);

    struct MotionControllerState final : MotionControllerStateBase {
        ~MotionControllerState();

        MotionControllerState& operator=(const MotionControllerStateBase& other) {
            MotionControllerStateBase::operator=(other);
            return *this;
        }

        operator MotionControllerStateBase&() {
            return *this;
        }

        // In-memory state
        sdk::AActor* adjustment_visualizer{nullptr};
        bool adjusting{false};
    };

    std::shared_ptr<MotionControllerState> get_or_add_motion_controller_state(sdk::USceneComponent* component) {
        {
            std::shared_lock _{m_mutex};
            if (auto it = m_motion_controller_attached_components.find(component); it != m_motion_controller_attached_components.end()) {
                return it->second;
            }
        }

        std::unique_lock _{m_mutex};
        auto result = std::make_shared<MotionControllerState>();
        return m_motion_controller_attached_components[component] = result;

        return result;
    }

    std::optional<std::shared_ptr<MotionControllerState>> get_motion_controller_state(sdk::USceneComponent* component) {
        std::shared_lock _{m_mutex};
        if (auto it = m_motion_controller_attached_components.find(component); it != m_motion_controller_attached_components.end()) {
            return it->second;
        }

        return {};
    }

    void remove_motion_controller_state(sdk::USceneComponent* component) {
        std::unique_lock _{m_mutex};
        m_motion_controller_attached_components.erase(component);
    }

    void remove_all_motion_controller_states() {
        std::unique_lock _{m_mutex};
        m_motion_controller_attached_components.clear();
    }

private:
    struct StatePath;
    struct PersistentState;
    struct PersistentCameraState;
    struct PersistentProperties;

    bool exists_unsafe(sdk::UObjectBase* object) const {
        return m_objects.contains(object);
    }

    void hook();
    void add_new_object(sdk::UObjectBase* object);

    void tick_attachments(
        Rotator<float>* view_rotation, const float world_to_meters, Vector3f* view_location, bool is_double
    );

    // Spawn a temporary CameraActor parented to `target` and make the player view through it
    // (SetViewTargetWithBlend), so a selected object can be inspected from a free vantage. Reversible
    // via restore_view_camera() (returns the view to the pawn + destroys the camera). Game-thread.
    void spawn_view_camera(sdk::USceneComponent* target);
    void restore_view_camera();

public:
    // Distance (meters, tracking-space scale) from the game camera to the NEAREST gizmo-target
    // component this frame, or -1 when no gizmo target is shown. OverlayComponent reads this to
    // auto-place the framework UI plane between the player camera and the object being manipulated.
    float get_nearest_gizmo_distance_meters() const;

private:
    // Overlay-material highlight (UE5.1+ UMeshComponent::SetOverlayMaterial): apply m_highlight_material
    // as the overlay material of newly gizmo-selected mesh comps, restore the original on deselect.
    // Both enqueue to the game thread internally and are duplicate-safe.
    void apply_overlay_highlight(sdk::USceneComponent* comp, sdk::UObject* material);
    void restore_overlay_highlight(sdk::USceneComponent* comp);

    void ui_standard_object_context_menu(sdk::UObjectBase* object);
    void ui_handle_object(sdk::UObject* object);
    void ui_handle_properties(void* object, sdk::UStruct* definition);
    void ui_handle_array_property(void* object, sdk::FArrayProperty* definition);
    void ui_handle_functions(void* object, sdk::UStruct* definition);
    void ui_function_context_menu(sdk::UFunction* func, void* object, bool is_real_object);
    void draw_active_function_hooks();
public:
    // Public: the free-function param editor (render_param_editor) reuses this generic struct editor
    // to make non-numeric StructProperty function parameters editable.
    void ui_handle_struct(void* addr, sdk::UStruct* definition);
private:
    // top_level_save: when true, the known-struct row opens the "##known_struct_save" context popup
    // (via OpenPopupOnItemClick on the value widget) so display_context_struct's Save/Edit menu binds
    // to the value row rather than the trailing Copy button. Only the top-level property row passes
    // true; nested/array/member recursions leave it false so they don't open a phantom popup.
    bool ui_try_known_struct(const std::string& label, void* addr, sdk::UStruct* definition, bool top_level_save = false);

    // STUB (disabled by default, crash-prone) — render a UTexture2D/UTexture as an ImGui::Image.
    // See the context dump above its definition in UObjectHook.cpp. Gated on m_show_texture_previews.
    void draw_texture_preview(sdk::UObject* texture);

    // STUB (VR, needs headset) — E1/E2: drive the active gizmo axis from a thumbstick while a
    // component is adjusted in VR, and show the driven axis as if its handle were mouse-clicked.
    // Context dump above the definition in UObjectHook.cpp. Empty no-op until implemented.
    void vr_gizmo_stick_adjust(sdk::USceneComponent* comp);

    void ui_handle_scene_component(sdk::USceneComponent* component);
    void ui_handle_material_interface(sdk::UObject* object);
    void ui_handle_actor(sdk::UObject* object);

    void spawn_overlapper(uint32_t hand = 0);
    void destroy_overlapper();

    std::optional<StatePath> try_get_path(sdk::UObject* target) const;

    static inline const std::vector<std::string> s_allowed_bases {
        "Acknowledged Pawn",
        "Player Controller",
        "Camera Manager",
        "Persistent Level",
        "World"
    };

    static std::filesystem::path get_persistent_dir();
    nlohmann::json serialize_mc_state(const std::vector<std::string>& path, const std::shared_ptr<MotionControllerState>& state);
    nlohmann::json serialize_camera(const std::vector<std::string>& path);
    void save_camera_state(const std::vector<std::string>& path);
    std::optional<StatePath> deserialize_path(const nlohmann::json& data);
    std::shared_ptr<PersistentState> deserialize_mc_state(nlohmann::json& data);
    std::shared_ptr<PersistentState> deserialize_mc_state(std::filesystem::path json_path);
    std::vector<std::shared_ptr<PersistentState>> deserialize_all_mc_states();
    std::shared_ptr<PersistentCameraState> deserialize_camera(const nlohmann::json& data);
    std::shared_ptr<PersistentCameraState> deserialize_camera_state();
    void update_persistent_states();
    void update_motion_controller_components(
        const glm::vec3& hmd_location, const glm::vec3& hmd_euler,
        const glm::vec3& left_hand_location, const glm::vec3& left_hand_euler,
        const glm::vec3& right_hand_location, const glm::vec3& right_hand_euler);

    static void* add_object(void* rcx, void* rdx, void* r8, void* r9, void* stack1, void* stack2, void* stack3, void* stack4);
    static void* destructor(sdk::UObjectBase* object, void* rdx, void* r8, void* r9);

    bool m_hooked{false};
    bool m_fully_hooked{false};
    bool m_wants_activate{false};
    float m_last_delta_time{1000.0f / 60.0f};

    struct DebugInfo {
        uint64_t constructor_calls{0};
        uint64_t destructor_calls{0};
    } m_debug{};

    glm::vec3 m_last_left_grip_location{};
    glm::vec3 m_last_right_grip_location{};
    glm::quat m_last_left_aim_rotation{glm::identity<glm::quat>()};
    glm::quat m_last_right_aim_rotation{glm::identity<glm::quat>()};

    mutable std::shared_mutex m_mutex{};

    struct MetaObject {
        std::wstring full_name{};
        sdk::UClass* uclass{nullptr};
        std::vector<sdk::UClass*> super_classes{};
    };

    std::unordered_set<sdk::UObjectBase*> m_objects{};
    std::unordered_map<sdk::UObjectBase*, std::unique_ptr<MetaObject>> m_meta_objects{};
    std::unordered_map<sdk::UClass*, std::unordered_set<sdk::UObjectBase*>> m_objects_by_class{};

    std::deque<std::unique_ptr<MetaObject>> m_reusable_meta_objects{};

    SafetyHookInline m_add_object_hook{};
    SafetyHookInline m_destructor_hook{};

    std::chrono::steady_clock::time_point m_last_sort_time{};
    std::vector<sdk::UClass*> m_sorted_classes{};
    std::future<std::vector<sdk::UClass*>> m_sorting_task{};

    std::unordered_map<sdk::UClass*, std::function<void (sdk::UObject*)>> m_on_creation_add_component_jobs{};
    // Own mutex (NOT m_mutex): the writer (queue_add) runs inside on_draw_ui which already holds a
    // shared_lock on m_mutex, so taking m_mutex exclusively here would deadlock the non-recursive
    // shared_mutex. An independent mutex serializes the map write against the object-creation hook
    // reader without touching m_mutex.
    std::mutex m_add_component_jobs_mtx{};

    std::deque<sdk::UObject*> m_most_recent_objects{};
    std::unordered_set<sdk::UObject*> m_motion_controller_attached_objects{};

    std::unordered_map<sdk::USceneComponent*, std::shared_ptr<MotionControllerState>> m_motion_controller_attached_components{};
    std::unordered_set<sdk::USceneComponent*> m_gizmo_components{};
    // Attached component currently being dragged by the flat screen-space gizmo. The stereo path reads
    // this to skip its motion-controller follow (which would otherwise clobber the gizmo write every
    // frame) and re-seed its attach offset from the gizmo-updated transform instead. Written on the draw
    // thread, read on the stereo thread.
    std::atomic<sdk::USceneComponent*> m_flat_gizmo_drag_comp{nullptr};
    float m_gizmo_axis_len{50.0f}; // world units (UE cm) for the translate gizmo axes
    float m_gizmo_ring_radius{40.0f}; // rotate-mode ring radius (UE cm), decoupled from the axis length so big translate arrows don't balloon the rotate rings (better centering, esp. in VR)
    float m_gizmo_thickness{4.0f}; // gizmo line thickness (px), applies to all modes
    int m_gizmo_mode{3};           // 0 = translate, 1 = rotate, 2 = scale, 3 = combined (default: combined shows all handles at once)
    bool m_gizmo_local{false};     // transform editor space: false = world, true = relative
    bool m_hide_gizmos_when_ui_closed{true}; // skip gizmo rendering/hit-testing (not the target list) while no UObjectHook panel is open
    bool m_block_passthrough_when_gizmos_visible{true}; // force input capture (block game passthrough) while any gizmo is actually drawn; combined with m_hide_gizmos_when_ui_closed, passthrough re-enables once gizmos are hidden even if targets are still selected
    bool m_auto_gizmo_on_adjust{false}; // VR: auto-show a gizmo on any MC-attached component currently in adjust mode (transient; never modifies m_gizmo_components)
    bool m_click_select_mode{false};    // armed state: left-click in the world adds the front-most scene component to m_gizmo_components (suppresses gizmo-axis dragging while armed). One-shot by default — auto-disarms after a hit unless m_click_select_sticky.
    bool m_click_select_sticky{false};  // keep picking after each hit instead of auto-disarming (multi-pick)
    bool m_click_select_picked_frame{false}; // set by handle_click_select on a pick; suppresses the gizmo-axis grab on that same left-press frame
    bool m_gizmo_or_picker_busy{false};      // gizmo grabbed/hot or picker armed this frame (read by the global drag-scroll to yield in VR)
    bool m_has_gizmos{false};                // any gizmo target is shown this frame (drives wants_vr_pointer() so the VR pointer stays live over empty space)
    bool m_gizmo_show_labels{true};     // draw the per-gizmo actor/component name + transform-metrics text overlay
    bool m_show_texture_previews{false}; // STUB feature gate — render UTexture as ImGui::Image (default OFF; will crash until draw_texture_preview is implemented)
    float m_inspector_item_width{320.0f}; // UObjectHook property-editor max width (px); <=0 = unlimited. Keeps inherited-object rows in a readable column on a wide window.
    bool m_click_select_single{false};    // pick REPLACES the selection (one gizmo target at a time) instead of accumulating
    bool m_gizmo_set_movable{true};       // on click-select, set the component's Mobility to Movable(2) so StaticMeshComponents can actually be moved by the gizmo
    bool m_highlight_selection{true};     // draw a world->screen outline over each gizmo-selected object
    bool m_highlight_overlay_material{false}; // also highlight selected mesh comps via SetOverlayMaterial (UE5.1+; no-op on older engines)
    bool m_highlight_material_search_attempted{false}; // auto-find of a default highlight material was tried this session
    sdk::UObject* m_highlight_material{nullptr}; // the UMaterialInterface used as the overlay highlight (auto-found engine material or user-picked)
    // comp -> original overlay material (possibly null) captured before the highlight was applied, so
    // deselect can restore it. Mutated on the game thread under the unique lock; snapshotted by the
    // draw thread under the shared lock.
    std::unordered_map<sdk::USceneComponent*, sdk::UObject*> m_overlay_mat_originals{};
    // Nearest gizmo-target distance in WORLD units this frame (-1 = none), published by
    // draw_component_gizmos (draw thread), consumed by get_nearest_gizmo_distance_meters.
    std::atomic<float> m_nearest_gizmo_dist_ue{-1.0f};
    float m_last_world_to_meters{100.0f}; // last world_to_meters from the stereo callback (benign cross-thread read)
    float m_snap_translate{10.0f};        // Ctrl-snap step for translate (world units)
    float m_snap_rotate{15.0f};           // Ctrl-snap step for rotate (degrees)
    float m_snap_scale{0.1f};             // Ctrl-snap step for scale
    float m_recenter_distance{150.0f};    // D4: distance (cm) in front of the camera the "Recenter to camera" button places the object
    sdk::USceneComponent* m_driven_comp{nullptr}; // #2 flat: component whose transform slider is being dragged (highlight its gizmo axis)
    int m_driven_axis{-1};                // #2 flat: sub-axis (0/1/2) the inspector slider is driving
    uint32_t m_driven_frame{0};           // #2 flat: ImGui frame the driven axis was last set (expires after a couple frames)
    sdk::USceneComponent* m_last_selected{nullptr}; // most recently click-selected component (main-page display + context-menu target)
    char m_pick_class_filter[128]{}; // picker: restrict candidates to those whose full name contains this (case-insensitive)
    int  m_pick_cycle{0};            // picker: which of the overlapping candidates under the cursor is the active one (scroll to cycle)

    // One picker candidate: the component, a short display label, its full path (shown on
    // hover/idle), and the screen/world points it was found at this scan (reused for the marker draw
    // and the Lua selection-state event without re-projecting).
    struct PickCandidate {
        sdk::USceneComponent* comp{nullptr};
        std::string short_label{};
        std::string full_path{};
        glm::vec2 screen{0.0f, 0.0f};
        glm::vec3 world{0.0f, 0.0f, 0.0f};
    };
    // Cached candidate list so the per-frame overlay doesn't re-scan m_objects (+ screen_to_world
    // ProcessEvent) every frame while the picker is armed — rebuilt only when the cursor moves / the
    // wheel turns / a click happens. Touched only on the draw thread in handle_click_select.
    std::vector<PickCandidate> m_pick_cache{};

    // Fire a Lua custom event ("uobjecthook_picker_selection") with the current picker candidate list
    // — the active (scroll-focused) one moved to index 0 — plus each candidate's screen/world point.
    // Called from handle_click_select whenever the active candidate or the candidate set changes, so
    // Lua-side overlay-material / debug-visualization scripts can follow the picker live.
    void dispatch_picker_selection_event();
    // Fire a Lua custom event ("uobjecthook_gizmo_target") when a component is added to / removed
    // from the gizmo-target set (click-select commit, "Show gizmo" checkbox, "Remove from
    // selection"), so Lua can apply/clear its own overlay-material or debug visualization in step
    // with the built-in gizmo highlight.
    void dispatch_gizmo_target_event(sdk::USceneComponent* comp, bool added);

    // True whenever UObjectHook itself needs exclusive input/cursor even if the main UEVR overlay
    // ("UEVR [...]" window) is closed: its own standalone window is open, a picker mode is armed, any
    // of its pop-out windows are shown, or a gizmo is currently on screen. Used (a) in place of
    // g_framework->is_drawing_ui() for UObjectHook's OWN interactive gates (gizmo drag-start,
    // click-select, the w2s context menu, the pop-out-window list) so those features work
    // independent of the main overlay, and (b) pushed into Framework's input-capture override each
    // frame (see on_frame()) so mouse/keyboard blocking + the always-visible cursor follow suit.
    // Deliberately does NOT touch g_framework->is_drawing_ui() itself — that also drives the VR
    // framework-UI-quad slate/framework swap (OverlayComponent), which is a different concern with
    // its own history of regressions; this stays additive/orthogonal to it.
    bool wants_active_ui() const;
    // Saved world positions for the "Save/Restore position" buttons. Touched from game-thread tasks
    // (GameThreadWorker) for the actual get/set_world_location, and read from the draw thread for the
    // button-enable check, so guard it with its own mutex (independent of m_mutex).
    std::unordered_map<sdk::USceneComponent*, glm::vec3> m_saved_positions{};
    std::mutex m_saved_positions_mtx{};
    sdk::AActor* m_overlap_detection_actor{nullptr};
    sdk::AActor* m_overlap_detection_actor_left{nullptr};

    struct CameraState {
        sdk::UObject* object{nullptr};
        glm::vec3 offset{};
    } m_camera_attach{};

    // Temporary spawned CameraActor used by the "View from spawned camera" action (game-thread owned).
    // Non-null => a view camera is active; the context menu shows "Restore view" instead. Cleared on
    // restore or when the actor is destroyed (object-destructor hook).
    sdk::AActor* m_view_camera_actor{nullptr};

    auto get_spawned_spheres() const {
        std::shared_lock _{m_mutex};
        return m_spawned_spheres;
    }

    std::unordered_set<sdk::USceneComponent*> m_spawned_spheres{};
    std::unordered_set<sdk::USceneComponent*> m_components_with_spheres{};
    std::unordered_map<sdk::USceneComponent*, sdk::USceneComponent*> m_spawned_spheres_to_components{};

    struct ResolvedObject {
    public:
        ResolvedObject() = default;
        ResolvedObject(void* data, sdk::UStruct* definition) : data{data}, definition{definition} {}
        ResolvedObject(std::nullptr_t) : data{nullptr}, definition{nullptr} {}

        operator sdk::UObject*() const noexcept {
            return object;
        }

        operator void*() const noexcept {
            return data;
        }

        bool operator==(void* other) const noexcept {
            return data == other;
        }

        bool operator==(sdk::UObject* other) const noexcept {
            return object == other;
        }

        bool operator==(std::nullptr_t) const noexcept {
            return data == nullptr;
        }

        bool operator!=(std::nullptr_t) const noexcept {
            return data != nullptr;
        }

        template<typename T>
        T as() const noexcept {
            return (T)data;
        }

        template<typename T>
        T as() noexcept {
            return (T)data;
        }

    public:
        union {
            void* data{nullptr};
            sdk::UObject* object;
        };

        sdk::UStruct* definition{nullptr};
        bool is_object{false};
    };

    class StatePath {
    public:
        struct PathScope {
            PathScope(const PathScope&) = delete;
            PathScope& operator=(const PathScope&) = delete;

            PathScope(PathScope&& other) noexcept : m_path(other.m_path) {
                other.m_moved = true;
            }

            PathScope& operator=(PathScope&& other) noexcept {
                if (this != &other) {
                    m_path = other.m_path;
                    other.m_moved = true;
                }
                return *this;
            }

            PathScope(StatePath& path, const std::string& name) : m_path{path} {
                m_path.push(name);
            }

            ~PathScope() {
                if (!m_moved) {
                    m_path.m_path.pop_back();
                }
            }

        private:
            StatePath& m_path;
            bool m_moved{false};
        };

        StatePath() = default;
        StatePath(const std::vector<std::string>& path) : m_path{path} {}

        StatePath& operator=(const std::vector<std::string>& path) {
            m_path = path;
            return *this;
        }

        const auto& path() const {
            return m_path;
        }

        PathScope enter(const std::string& name) {
            return PathScope{*this, name};
        }

        PathScope enter_clean(const std::string& name) {
            clear();
            return PathScope{*this, name};
        }

        bool has_valid_base() const {
            if (m_path.empty()) {
                return false;
            }

            return std::find(s_allowed_bases.begin(), s_allowed_bases.end(), m_path[0]) != s_allowed_bases.end();
        }

        sdk::UObject* resolve_base_object() const;
        ResolvedObject resolve()  const;

    private:
        void clear() {
            m_path.clear();
        }

        void push(const std::string& name) {
            m_path.push_back(name);
        }

        void pop() {
            m_path.pop_back();
        }

        std::vector<std::string> m_path{};
    } m_path;

    struct JsonAssociation {
        std::optional<std::filesystem::path> path_to_json{};
        void erase_json_file() const {
            if (path_to_json.has_value() && std::filesystem::exists(*path_to_json)) {
                std::filesystem::remove(*path_to_json);
            }
        }
    };

    struct PersistentState : JsonAssociation {
        StatePath path{};
        MotionControllerStateBase state{};
        sdk::USceneComponent* last_object{nullptr};
    };

    struct PersistentCameraState : JsonAssociation {
        StatePath path{};
        glm::vec3 offset{};
    };

    struct PersistentProperties : JsonAssociation {
        void save_to_file(std::optional<std::filesystem::path> path = std::nullopt);
        nlohmann::json to_json() const;
        static std::shared_ptr<PersistentProperties> from_json(std::filesystem::path json_path);
        static std::shared_ptr<PersistentProperties> from_json(const nlohmann::json& j);

        StatePath path{};

        struct PropertyState {
            std::wstring name{};
            union {
                uint64_t u64;
                double d;
                float f;
                int32_t i;
                uint8_t u8;
                uint16_t u16;
                bool b;
            } data;
            // For struct properties (Vector/Rotator/Transform/etc.) the 8-byte union can't hold the
            // value, so the raw struct bytes live here. struct_size > 0 means "apply struct_bytes".
            uint8_t struct_bytes[128]{};
            uint32_t struct_size{0};
        };

        std::vector<std::shared_ptr<PropertyState>> properties{};
        bool hide{false};
        bool hide_legacy{false};
        // Stable-locator fallback: when no allowed-base path can reach the object (e.g. a
        // click-selected world actor), we store its full name here and re-resolve it each tick via
        // sdk::find_uobject (which caches HITS + self-invalidates across level loads). Empty => use `path`.
        std::wstring object_locator{};
        // find_uobject only caches hits; a MISS does a full O(N) get_full_name scan of the whole
        // object array. An absent locator target (destroyed / not-yet-spawned / wrong level) would
        // therefore re-scan every tick. This per-bucket cooldown skips the lookup for a while after a
        // miss so the worst case is one scan per ~cooldown ticks instead of one per frame. Transient.
        mutable uint32_t locator_miss_cooldown{0};
    };

    // Resolve a saved property bucket to its live object: prefer the base-relative `path` (survives
    // address changes via the live walk), else fall back to the stable full-name locator. Returns a
    // null ResolvedObject when neither resolves this tick.
    // use_cooldown=true (per-tick reapply): throttle the locator full-array scan after a miss.
    // use_cooldown=false (on-click dedup/save): always do a live lookup so a present object is found
    // even if it was briefly absent, avoiding a stale null that would spawn a duplicate bucket.
    ResolvedObject resolve_persistent_target(const PersistentProperties& pp, bool use_cooldown = true) const;

    glm::vec3 m_last_camera_location{};
    bool object_from_path_or_address(std::string_view object, sdk::UObject* out);

    std::shared_ptr<PersistentCameraState> m_persistent_camera_state{};
    std::vector<std::shared_ptr<PersistentState>> m_persistent_states{};
    std::vector<std::shared_ptr<PersistentProperties>> m_persistent_properties{};
    std::unordered_map<std::string_view, std::string_view> m_inline_uobjecthooks{};
    void reload_persistent_states() {
        m_persistent_states = deserialize_all_mc_states();
        m_persistent_camera_state = deserialize_camera_state();
        m_persistent_properties = deserialize_all_persistent_properties();
    }

    void reset_persistent_states() {
        m_persistent_states.clear();
        m_persistent_properties.clear();
        m_persistent_camera_state.reset();
    }

    std::vector<std::shared_ptr<PersistentProperties>> deserialize_all_persistent_properties() const;

private:
    ModToggle::Ptr m_enabled_at_startup{ModToggle::create(generate_name("EnabledAtStartup"), false)};
    ModToggle::Ptr m_attach_lerp_enabled{ModToggle::create(generate_name("AttachLerpEnabled"), true)};
    ModSlider::Ptr m_attach_lerp_speed{ModSlider::create(generate_name("AttachLerpSpeed"), 0.01f, 30.0f, 15.0f)};

    ModKey::Ptr m_keybind_toggle_uobject_hook{ModKey::create(generate_name("ToggleUObjectHookKey"))};
    // Rebindable keys to switch the gizmo transform mode (Move/Rotate/Scale). Default unbound so they
    // never collide with game/freecam input until the user assigns them in the gizmo options.
    ModKey::Ptr m_keybind_gizmo_move{ModKey::create(generate_name("GizmoMoveKey"))};
    ModKey::Ptr m_keybind_gizmo_rotate{ModKey::create(generate_name("GizmoRotateKey"))};
    ModKey::Ptr m_keybind_gizmo_scale{ModKey::create(generate_name("GizmoScaleKey"))};
    ModKey::Ptr m_keybind_gizmo_combined{ModKey::create(generate_name("GizmoCombinedKey"))};
    ModKey::Ptr m_keybind_pick{ModKey::create(generate_name("PickModeKey"))}; // toggle the click-select picker
    bool m_uobject_hook_disabled{false};
    bool m_fixed_visibilities{false};
    bool m_hide_default_classes{false};

    safetyhook::InlineHook m_process_event_hook{};
    bool m_process_event_listening{true};
    bool m_process_event_flagged_only{false};
    bool m_attempted_hook_process_event{false};
    bool m_hooked_process_event{false};


public:
    void hook_process_event();
    static void* process_event_hook(sdk::UObject* obj, sdk::UFunction* func, void* params, void* r9);
    void draw_process_event_monitor();

private:
    struct CalledFunctionInfo {
        size_t call_count{0};

        struct HeavyData {
            std::vector<uint8_t> params{};
        };

        std::unique_ptr<HeavyData> heavy_data{nullptr};
        bool wants_heavy_data{false};

        // Per-receiver tallies for the ProcessEvent monitor's "By class" / "By caller" grouping —
        // "class" = the receiver's UClass (calls aggregated across every instance of that class),
        // "caller" = the specific receiver UObject instance. Populated in process_event_hook
        // alongside call_count, so grouping needs no extra scan of call history.
        std::unordered_map<sdk::UClass*, size_t> caller_class_counts{};
        std::unordered_map<sdk::UObject*, size_t> caller_instance_counts{};
    };

public:
    // Public (not private) so free functions in UObjectHook.cpp's anonymous namespace — e.g.
    // dispatch_function_monitor_event, which needs to read m_called_functions but isn't a member —
    // can lock it directly, matching m_called_functions itself already being public.
    std::recursive_mutex m_function_mutex{};
    std::unordered_map<sdk::UFunction*, CalledFunctionInfo> m_called_functions{};
    std::deque<sdk::UFunction*> m_most_recent_functions{};
    std::unordered_set<sdk::UFunction*> m_ignored_recent_functions{};

private:
    // Per-frame diff against PluginLoader::get_hooked_functions() so a function hooked via a Lua
    // script's fn:hook_ptr(...) (or a native plugin) — NOT through UObjectHook's own Block/Monitor
    // toggle — still fires "uobjecthook_function_monitor" to Lua the moment it's noticed. Tracks what
    // has already been announced so add/remove is only dispatched on actual change.
    void sync_hooked_functions_to_lua();
    std::unordered_set<sdk::UFunction*> m_known_hooked_funcs{};

public:

    struct {
        int32_t max_calls{0};
        std::array<char, 512> buffer{0};
    } m_process_event_search{};

public:
    UObjectHook() {
        m_options = {
            *m_enabled_at_startup,
            *m_attach_lerp_enabled,
            *m_attach_lerp_speed,
            *m_keybind_toggle_uobject_hook,
            *m_keybind_gizmo_move,
            *m_keybind_gizmo_rotate,
            *m_keybind_gizmo_scale,
            *m_keybind_gizmo_combined,
            *m_keybind_pick
        };
    }

private:
};