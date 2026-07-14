// Gizmo drawing, hit-testing/dragging, the click-select world picker, VR thumbstick gizmo control,
// and the overlay-material highlight helpers — split out of UObjectHook.cpp purely for file-size
// organization (that file was 13k lines). Still UObjectHook:: member function definitions (same
// class, same members, same mutex, same everything) — only WHERE the code lives changed, not what
// it does. See src/mods/uobjecthook/SDKDumper.cpp for the existing precedent of this pattern.

#include <cmath>
#include <algorithm>

#include <utility/Logging.hpp>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/UObjectBase.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FField.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/AActor.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/USceneComponent.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/UMotionControllerComponent.hpp>
#include <sdk/ScriptVector.hpp>
#include <sdk/FBoolProperty.hpp>

#include <imgui_internal.h>
#include "../VR.hpp"
#include "../LuaLoader.hpp"

#include "../UObjectHook.hpp"

// Layout-matched param block for APlayerController::SetViewTargetWithBlend — see UObjectHook.cpp's
// identical copy (spawn_view_camera / the gizmo target context menu's "Set view target to actor") for
// the full rationale. Duplicated rather than shared: it's a pure local memory-layout convenience type
// (never crosses a function-call boundary between translation units), so an independent copy per file
// is fine and avoids adding a shared header just for one struct.
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
					L"Material /ControlRig/M_Manip.M_Manip",
                    L"Material /Engine/EngineMaterials/WidgetMaterial.WidgetMaterial",
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

                // Material drag-drop targets do NOT work inside a hover-cascading BeginMenu chain:
                // starting the drag means moving the mouse off the menu to the source item, which
                // collapses the submenu before you can drag back to drop on it — there's no way to
                // actually complete the drop. The real material-editing UI lives in the actor's own
                // persistent inspector (ui_handle_actor's "Materials" section) instead, where the
                // window stays open regardless of where the drag source is. This is just a shortcut
                // there.
                if (owner != nullptr && ImGui::MenuItem("Inspect materials...")) {
                    m_last_selected = c;
                    ImGui::CloseCurrentPopup();
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
