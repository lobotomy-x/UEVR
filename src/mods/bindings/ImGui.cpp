/*
This license governs this file (ImGui.cpp), and is separate from the license for the rest of the UEVR codebase.

The MIT License

Copyright (c) 2023-2025 praydog

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef LUA_DEBUG
#define LUA_DEBUG 1 // Fixed: removed '=' from define
#endif
#include <algorithm>
#include <cstdint>
#include <imgui.h>
#include <imgui_internal.h>
#include <unordered_map>
#include <vector>

// Add sol2 include before using sol types
#include <sol/sol.hpp>

#include "../LuaLoader.hpp"
//#include "../dependencies/submodules/ImGuizmo/ImGuizmo.h"
//#include "../dependencies/submodules/ImGuizmo/ImSequencer.h"

//#include "../dependencies/submodules/imnodes/imnodes.h"
#include "Framework.hpp"
#include "utility/ImGui.hpp"

#include "ImGui.hpp"



// set the imgui texture pointer to our own data format
#define ImTextureID uint64_t

namespace {



            
// Storage for drag-drop payloads from Lua
static std::unordered_map<uint64_t, sol::object> g_drag_drop_payloads{};
 static uint64_t g_next_payload_id = 1;

// Cleanup old payloads (call this periodically, e.g., in NewFrame or EndFrame binding)
static void cleanup_old_payloads() {
     if (g_drag_drop_payloads.empty()) return;
    // Keep only the last 100 payloads to prevent memory leaks
    if (g_drag_drop_payloads.size() > 100) {
        auto it = g_drag_drop_payloads.begin();
        std::advance(it, g_drag_drop_payloads.size() - 100);
        g_drag_drop_payloads.erase(g_drag_drop_payloads.begin(), it);
    }
}
} // namespace

namespace api::imgui {
void text_colored(const char* text, sol::object color);
int32_t g_disabled_counts{0};

void cleanup() {
    for (auto i = 0; i < g_disabled_counts; ++i) {
        ImGui::EndDisabled();
    }

    g_disabled_counts = 0;
}

ImVec2 create_imvec2(sol::object obj) {
    ImVec2 out{0.0f, 0.0f};
    if (!obj) return out;
    if (obj.is<Vector2f>()) {
        auto vec = obj.as<Vector2f>();
        out.x = vec.x;
        out.y = vec.y;
    } else if (obj.is<sol::table>()) {
        auto table = obj.as<sol::table>();

        if (table.size() == 2) {
            out.x = table.get<float>(1);
            out.y = table.get<float>(2);
        } else {
            throw sol::error{"Invalid table passed. Table size must be 2."};
        }
    } else if (obj.is<Vector3f>()) {
        auto vec = obj.as<Vector3f>();
        out.x = vec.x;
        out.y = vec.y;
    } else if (obj.is<Vector4f>()) {
        auto vec = obj.as<Vector4f>();
        out.x = vec.x;
        out.y = vec.y;
    }

    return out;
}

ImVec4 create_imvec4(sol::object obj) {
    ImVec4 out{0.0f, 0.0f, 0.0f, 0.0f};

    if (obj.is<Vector4f>()) {
        auto vec = obj.as<Vector4f>();
        out.x = vec.x;
        out.y = vec.y;
        out.z = vec.z;
        out.w = vec.w;
    } else if (obj.is<sol::table>()) {
        auto table = obj.as<sol::table>();

        if (table.size() == 4) {
            out.x = table.get<float>(1);
            out.y = table.get<float>(2);
            out.z = table.get<float>(3);
            out.w = table.get<float>(4);
        } else {
            throw sol::error{"Invalid table passed. Table size must be 4."};
        }
    }

    return out;
}

ImVec4 create_imvec4_color(sol::object obj) {
    ImVec4 out{1.0f, 1.0f, 1.0f, 1.0f};

    if (obj.is<unsigned int>()) {
        auto _uint = obj.as<unsigned int>();
        auto r = _uint & 0xFF;
        auto g = (_uint >> 8) & 0xFF;
        auto b = (_uint >> 16) & 0xFF;
        auto a = (_uint >> 24) & 0xFF;
        out.x = (float)r / 255.0f;
        out.y = (float)g / 255.0f;
        out.z = (float)b / 255.0f;
        out.w = (float)a / 255.0f;
    } else if (obj.is<Vector4f>()) {
        auto vec = obj.as<Vector4f>();
        out.x = vec.x;
        out.y = vec.y;
        out.z = vec.z;
        out.w = vec.w;
    } else if (obj.is<sol::table>()) {
        auto table = obj.as<sol::table>();
        if (table.size() == 4) {
            out.x = table.get<float>(1);
            out.y = table.get<float>(2);
            out.z = table.get<float>(3);
            out.w = table.get<float>(4);
        } else {
            throw sol::error{"Invalid table passed. Table size must be 4."};
        }
    }
    return out;
}

ImU32 create_imu32_color(sol::object obj) {
    if (obj.is<unsigned int>()) {
        return obj.as<ImU32>();
    } else if (obj.is<Vector4f>()) {
        const auto& vec = obj.as<Vector4f>();
        auto r = (unsigned int)std::fmin(255, (int)(vec.x * 255.0f + 0.5f));
        auto g = (unsigned int)std::fmin(255, (int)(vec.y * 255.0f + 0.5f));
        auto b = (unsigned int)std::fmin(255, (int)(vec.z * 255.0f + 0.5f));
        auto a = (unsigned int)std::fmin(255, (int)(vec.w * 255.0f + 0.5f));
        return (a << 24) | (b << 16) | (g << 8) | r;
    } else if (obj.is<sol::table>()) {
        auto table = obj.as<sol::table>();
        if (table.size() == 4) {
            float r = table.get<float>(1);
            float g = table.get<float>(2);
            float b = table.get<float>(3);
            float a = table.get<float>(4);

            auto ri = (unsigned int)std::fmin(255, (int)(r * 255.0f + 0.5f));
            auto gi = (unsigned int)std::fmin(255, (int)(g * 255.0f + 0.5f));
            auto bi = (unsigned int)std::fmin(255, (int)(b * 255.0f + 0.5f));
            auto ai = (unsigned int)std::fmin(255, (int)(a * 255.0f + 0.5f));
            return (ai << 24) | (bi << 16) | (gi << 8) | ri;
        } else {
            throw sol::error{"Invalid table passed. Table size must be 4."};
        }
    }

    return 0xFFFFFFFF;
}

ImVec4 convert_ue32_to_vec4(ImU32 in) {
    return ImGui::ColorConvertU32ToFloat4(in);
}

unsigned int convert_vec4_to_u32(const ImVec4 in) {
    return ImGui::ColorConvertFloat4ToU32(in);
}


void draw_scene_texture(sol::object size_obj) {
    UEVR_FRHITexture2DHandle Handle = uevr::API::get()->param()->sdk->stereo_hook->get_scene_render_target();
    void* native = uevr::API::get()->param()->sdk->frhitexture2d->get_native_resource(Handle);
    ImGui::Image((ImTextureID)native, create_imvec2(size_obj), ImVec2(0, 0), ImGui::GetBackgroundDrawList()->GetClipRectMax());
    /*    if (auto renderer = uevr::API ::get()->param()->renderer; renderer->renderer_type == 0) {
        ()
    }*/
}



#include "../dependencies/submodules/cimgui/cimgui.h"

// Use buttonex to allow passing flags, e.g. hold to repeat
bool button(const char* label, sol::object size_object, sol::object flags_object) {
    if (label == nullptr) {
        label = "";
    }

    const auto size = create_imvec2(size_object);
    ImGuiButtonFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiButtonFlags)(flags_object.as<int>());
    }
    return ImGui::ButtonEx(label, size, flags);
}

bool small_button(const char* label) {
    if (label == nullptr) {
        label = "";
    }

    return ImGui::SmallButton(label);
}

bool invisible_button(const char* id, sol::object size_object, sol::object flags_object) {
    if (id == nullptr) {
        id = "";
    }

    const auto size = create_imvec2(size_object);

    ImGuiButtonFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiButtonFlags)(flags_object.as<int>());
    }

    return ImGui::InvisibleButton(id, size, flags);
}

bool arrow_button(const char* str_id, int dir) {
    if (str_id == nullptr) {
        str_id = "";
    }

    return ImGui::ArrowButton(str_id, (ImGuiDir)dir);
}

// #if LUA_DEBUG==1
void show_metrics_window(sol::object open_obj) {
    bool open = true;
    bool* open_p = nullptr;

    if (!open_obj.is<sol::nil_t>() && open_obj.is<bool>()) {
        open = open_obj.as<bool>();
        open_p = &open;
    }

    ImGui::ShowMetricsWindow(open_p);
}
void show_font_atlas() {
    ImGui::ShowFontAtlas(ImGui::GetIO().Fonts);
}

void show_debug_log_window(sol::object open_obj) {
    bool open = true;
    bool* open_p = nullptr;

    if (!open_obj.is<sol::nil_t>() && open_obj.is<bool>()) {
        open = open_obj.as<bool>();
        open_p = &open;
    }

    ImGui::ShowDebugLogWindow(open_p);
}
void show_stack_tool_window(sol::object open_obj) {
    bool open = true;
    bool* open_p = nullptr;

    if (!open_obj.is<sol::nil_t>() && open_obj.is<bool>()) {
        open = open_obj.as<bool>();
        open_p = &open;
    }

    ImGui::ShowStackToolWindow(open_p);
}

void show_font_selector(const char* label) {
    ImGui::ShowFontSelector(label);
}

void show_demo_window(sol::object open_obj) {
    bool open = true;
    bool* open_p = nullptr;

    if (!open_obj.is<sol::nil_t>() && open_obj.is<bool>()) {
        open = open_obj.as<bool>();
        open_p = &open;
    }
    ImGui::ShowDemoWindow(open_p);
}

// #endif
bool begin_drag_drop_source(sol::object flags_object) {

    ImGuiDragDropFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiDragDropFlags)(flags_object.as<int>());

    }
    return ImGui::BeginDragDropSource(flags);
}



void end_drag_drop_source() {
    ImGui::EndDragDropSource();
}

bool set_drag_drop_payload(const char* type, sol::object data) {
    cleanup_old_payloads();
    uint64_t id = g_next_payload_id++;
    g_drag_drop_payloads[id] = data;
    return ImGui::SetDragDropPayload(type, &id, sizeof(id));
}

bool is_payload_accepted() {
    return ImGui::IsDragDropPayloadBeingAccepted();
}

sol::object accept_payload(sol::this_state s, const char* type, sol::object flags_object) {
    ImGuiDragDropFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiDragDropFlags)(flags_object.as<int>());
    }
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type, flags);
    if (payload && payload->Data && payload->DataSize == sizeof(uint64_t)) {
        uint64_t id = *(const uint64_t*)payload->Data;
        auto it = g_drag_drop_payloads.find(id);
        if (it != g_drag_drop_payloads.end()) {

            return sol::make_object(s, it->second);
        }
    }
    return sol::nil;
}

bool begin_drag_drop_target() {

    return ImGui::BeginDragDropTarget();
}

void end_drag_drop_target() {
    ImGui::EndDragDropTarget();
}

void render_drag_drop(sol::object rect_start, sol::object rect_end) {
    const auto rectstart = create_imvec2(rect_start);
    const auto rectend = create_imvec2(rect_end);
    ImRect bb(rectstart, rectend);
    ImGui::RenderDragDropTargetRectForItem(bb);
}

void accept_drag_drop(const char* type) {
    ImGui::AcceptDragDropPayload(type);
}

void text(const char* text, sol::object flags_object) {
    if (text == nullptr) {
        text = "";
    }
    ImGuiTextFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiTextFlags)(flags_object.as<int>());
    }
    if (flags == 0)
        flags = ImGuiTextFlags_NoWidthForLargeClippedText;
    ImGui::TextEx(text, NULL, flags);
}

void text_colored(const char* text, sol::object color) {
    if (text == nullptr) {
        text = "";
    }

    const auto out_color = create_imvec4_color(color);

    ImGui::TextColored(out_color, text);
}

void bullet() {
    ImGui::Bullet();
}

void bullet_text(const char* text) {
    if (text == nullptr) {
        text = "";
    }
    ImGui::BulletText(text);
}

void label_text(const char* label, const char* fmt) {
    ImGui::LabelText(label, fmt);
}

// you can also set flags individually to get repeat now
void push_button_repeat(bool v) {
    ImGui::PushButtonRepeat(v);
}

void pop_button_repeat() {
    ImGui::PopButtonRepeat();
}

void separator_text(const char* text) {
    ImGui::SeparatorText(text);
}

sol::variadic_results checkbox(sol::this_state s, const char* label, bool v) {
    if (label == nullptr) {
        label = "";
    }

    auto changed = ImGui::Checkbox(label, &v);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}

sol::variadic_results combo(sol::this_state s, const char* label, sol::object selection, sol::table values) {
    if (label == nullptr) {
        label = "";
    }

    const char* preview_value = "";

    if (!values.empty()) {
        if (selection.is<sol::nil_t>() || values.get_or(selection, sol::make_object(s, sol::nil)).is<sol::nil_t>()) {
            selection = (*values.begin()).first;
        }

        auto val_at_selection = values[selection].get<sol::object>();

        if (val_at_selection.is<const char*>()) {
            preview_value = val_at_selection.as<const char*>();
        }
    }

    auto selection_changed = false;

    if (ImGui::BeginCombo(label, preview_value)) {
        for (auto& [key, val] : values) {
            auto val_at_k = values[key].get<sol::object>();

            if (val_at_k.is<const char*>()) {
                auto entry = val_at_k.as<const char*>();

                if (ImGui::Selectable(entry, selection == key)) {
                    selection = key;
                    selection_changed = true;
                }
            }
        }

        ImGui::EndCombo();
    }

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, selection_changed));
    results.push_back(sol::make_object(s, selection));

    return results;
}

sol::variadic_results drag_float(sol::this_state s, const char* label, float v, float v_speed, float v_min, float v_max,
    const char* display_format = "%.3f", sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    auto changed = ImGui::DragFloat(label, &v, v_speed, v_min, v_max, display_format, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}

sol::variadic_results drag_float2(sol::this_state s, const char* label, Vector2f v, float v_speed, float v_min, float v_max,
    const char* display_format = "%.3f", sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    auto changed = ImGui::DragFloat2(label, (float*)&v, v_speed, v_min, v_max, display_format, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}

// Accept any of: Vector3f, Vector3d, or a {x=,y=,z=} / {[1]=,[2]=,[3]=} table.
// Original signature required Vector3f strictly, which broke user wrappers
// that pass Vector3d.new(...) — and even worse, the OLD wrapper definition
// stuck in the Lua state across script reloads because the global imgui table
// retained the bound closure. Making the C++ side type-tolerant means those
// stale wrappers stop throwing. Returns (changed, Vector3f) so the caller
// always gets the same shape back regardless of input.
sol::variadic_results drag_float3(sol::this_state s, const char* label, sol::object v_obj, float v_speed, float v_min, float v_max,
    const char* display_format = "%.3f", sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;
    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    // Coerce v_obj into a Vector3f the ImGui call can edit in place. We keep
    // the input precision in mind for the return (Vector3d input → Vector3d
    // result so caller's `nv.x` doesn't lose precision in pure passthrough).
    Vector3f v{};
    bool input_was_double = false;
    if (v_obj.is<Vector3f>()) {
        v = v_obj.as<Vector3f>();
    } else if (v_obj.is<Vector3d>()) {
        auto vd = v_obj.as<Vector3d>();
        v = Vector3f{(float)vd.x, (float)vd.y, (float)vd.z};
        input_was_double = true;
    } else if (v_obj.is<sol::lua_table>()) {
        auto t = v_obj.as<sol::lua_table>();
        // Support either named keys (x/y/z) or positional 1/2/3.
        auto coerce = [&](const char* key, int idx) -> float {
            sol::object o = t[key];
            if (!o.valid() || o.is<sol::lua_nil_t>()) o = t[idx];
            return o.is<float>() ? o.as<float>() : (o.is<double>() ? (float)o.as<double>() : 0.0f);
        };
        v = Vector3f{coerce("x", 1), coerce("y", 2), coerce("z", 3)};
    } else {
        // Unknown shape — bail with a clearer error than sol's overload mismatch.
        throw sol::error("imgui.drag_float3: v must be Vector3f, Vector3d, or {x,y,z} table");
    }

    auto changed = ImGui::DragFloat3(label, (float*)&v, v_speed, v_min, v_max, display_format, flags);

    sol::variadic_results results{};
    results.push_back(sol::make_object(s, changed));
    if (input_was_double) {
        results.push_back(sol::make_object(s, Vector3d{(double)v.x, (double)v.y, (double)v.z}));
    } else {
        results.push_back(sol::make_object(s, v));
    }
    return results;
}

sol::variadic_results drag_float4(sol::this_state s, const char* label, Vector4f v, float v_speed, float v_min, float v_max,
    const char* display_format = "%.3f", sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    auto changed = ImGui::DragFloat4(label, (float*)&v, v_speed, v_min, v_max, display_format, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}

sol::variadic_results drag_int(sol::this_state s, const char* label, int v, float v_speed, int v_min, int v_max,
    const char* display_format = "%d", sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    auto changed = ImGui::DragInt(label, &v, v_speed, v_min, v_max, display_format, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}

sol::variadic_results slider_float(sol::this_state s, const char* label, float v, float v_min, float v_max,
    const char* display_format = "%.3f", sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    auto changed = ImGui::SliderFloat(label, &v, v_min, v_max, display_format, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}

sol::variadic_results slider_int(
    sol::this_state s, const char* label, int v, int v_min, int v_max, const char* display_format = "%d", sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    auto changed = ImGui::SliderInt(label, &v, v_min, v_max, display_format, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}


sol::variadic_results vslider_float(sol::this_state s, const char* label, float v, float v_min, float v_max,
    const char* display_format = "%.3f", sol::object size_object = nullptr, sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    auto changed = ImGui::VSliderFloat(label, create_imvec2(size_object),  & v, v_min, v_max, display_format, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}
// putting size out of order compared to the actual functions to make it easier to just add a v to existing slider clode
sol::variadic_results vslider_int(sol::this_state s, const char* label,  int v, int v_min, int v_max,
    const char* display_format = "%.3f", sol::object size_object = nullptr, sol::object flags_object = 0) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiSliderFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiSliderFlags)(flags_object.as<int>());
    }

    auto changed = ImGui::VSliderInt(label, create_imvec2(size_object), &v, v_min, v_max, display_format, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, v));

    return results;
}

//
// InputFloat(
// InputFloat2
// InputFloat3
// InputFloat4

// InputInt

// sol::variadic_results input_scalar(sol::this_state s, const char* label, ImGuiDataType data_type, void* p_data, const void* p_step =
// NULL,
//     const void* p_step_fast = NULL, const char* format = NULL, ImGuiInputTextFlags flags = 0);)

//   IMGUI_API bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0,
// const ImVec2& size = ImVec2(0, 0)); // "bool selected" carry the selection state (read-only). Selectable() is clicked is returns true so
//                                    // you can modify your selection state. size.x==0.0: use remaining width, size.x>0.0: specify width.
//                                    // size.y==0.0: use label height, size.y>0.0: specify height

sol::variadic_results input_text(
    sol::this_state s, const char* label, const std::string& v, ImGuiInputTextFlags flags /*, sol::object callback_object*/) {
    flags |= ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways;

    if (label == nullptr) {
        label = "";
    }
    // ImGuiInputTextCallback callback {};

    // if (callback_object) {
    //     flags = (ImGuiSliderFlags)(flags_object.as<int>());

    static std::string buffer{""};
    buffer = v;

    static int selection_start, selection_end;

    static auto input_text_callback = [](ImGuiInputTextCallbackData* data) -> int {
        if ((data->EventFlag & ImGuiInputTextFlags_CallbackResize) != 0) {
            buffer.resize(data->BufTextLen);
            data->Buf = (char*)buffer.c_str();
        }

        selection_start = data->SelectionStart;
        selection_end = data->SelectionEnd;

        return 0;
    };

    auto changed = ImGui::InputText(label, buffer.data(), buffer.capacity() + 1, flags, input_text_callback);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, std::string{buffer.data()}));
    results.push_back(sol::make_object(s, selection_start));
    results.push_back(sol::make_object(s, selection_end));

    return results;
}

sol::variadic_results input_text_multiline(
    sol::this_state s, const char* label, const std::string& v, sol::object size_obj, ImGuiInputTextFlags flags) {
    flags |= ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways;

    if (label == nullptr) {
        label = "";
    }

    static std::string buffer{""};
    buffer = v;

    static int selection_start, selection_end;

    static auto input_text_callback = [](ImGuiInputTextCallbackData* data) -> int {
        if ((data->EventFlag & ImGuiInputTextFlags_CallbackResize) != 0) {
            buffer.resize(data->BufTextLen);
            data->Buf = (char*)buffer.c_str();
        }

        selection_start = data->SelectionStart;
        selection_end = data->SelectionEnd;

        return 0;
    };

    const auto size = create_imvec2(size_obj);

    auto changed = ImGui::InputTextMultiline(label, buffer.data(), buffer.capacity() + 1, size, flags, input_text_callback);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, std::string{buffer.data()}));
    results.push_back(sol::make_object(s, selection_start));
    results.push_back(sol::make_object(s, selection_end));

    return results;
}

bool tree_node(const char* label, sol::object flags_object) {
    if (label == nullptr) {
        label = "";
    }
    ImGuiTreeNodeFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiTreeNodeFlags)(flags_object.as<int>());
    }

    auto tree = ImGui::TreeNodeEx(label, flags);
    //ImGui::PushID(label);
    return tree;
}
bool begin_multi_select(sol::object flags_object, int selection_size, int items_count) {
    ImGuiMultiSelectFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiMultiSelectFlags)(flags_object.as<int>());
    }
    return ImGui::BeginMultiSelect(flags, selection_size, items_count);
}

void end_multi_select() {
    ImGui::EndMultiSelect();
}

bool tree_node_ptr_id(const void* id, const char* label, sol::object flags_object) {
    if (label == nullptr) {
        label = "";
    }
    // Previously if you call ptr/str id version you would think that does enough
    // to distinguish items when iterating a table
    // it does for the nodes but anything inside the nodes doesn't seem to be covered
    // and therefore even when using ptr_id the expansion state can be screwed up
    ImGuiTreeNodeFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiTreeNodeFlags)(flags_object.as<int>());
    }

    auto tree = ImGui::TreeNodeEx(label, flags);
 //   ImGui::PushID(label);
    return tree;
}
                                      

void push_override_id(sol::object id){
    ImGuiID ID{};
    if (id.is<int>()) {
        ID = id.as<ImGuiID>();
    } else if (id.is<const char*>()) {
        ID = ImGui::GetID(id.as<const char*>());
    } else if (id.is<void*>()) {
        ID = ImGui::GetID(id.as<void*>());
    } else {
        throw sol::error("Type must be int, const char* or void*");
    }


    ImGui::PushOverrideID(ID);
}

ImGuiID get_id_from_pos(sol::object pos) {
    return ImGui::GetCurrentWindow()->GetIDFromPos(create_imvec2(pos));
}

 bool tree_node_str_id(const char* id, const char* label, sol::object flags_object) {
    if (label == nullptr) {
        label = "";
    }
    ImGuiTreeNodeFlags flags = 0;

    if (flags_object.is<int>()) {
        flags = (ImGuiTreeNodeFlags)(flags_object.as<int>());
    }
    
    auto tree = ImGui::TreeNodeEx(label, flags);
 //   ImGui::PushID(label);
    return tree;
 }

void tree_pop() {
  //  ImGui::PopID();
    ImGui::TreePop();
}

void same_line() {
    ImGui::SameLine();
}

bool is_item_hovered(sol::object flags_obj) {
    ImGuiHoveredFlags flags{0};

    if (flags_obj.is<int>()) {
        flags = (ImGuiHoveredFlags)flags_obj.as<int>();
    }


    return ImGui::IsItemHovered(flags);
}

ImGuiID get_active_id() {
    return ImGui::GetActiveID();
}
//
//  ImGuiID get_focus_id() {
//    return ImGui::GetFocusID;
//}

ImGuiID get_hovered_id() {
    return ImGui::GetHoveredID();
}

ImGuiID get_item_id() {
    return ImGui::GetItemID();
}

void activate_item_by_id(sol::object id) {
    ImGuiID ID{};
    if (id.is<int>()) {
        ID = id.as<ImGuiID>();
    } else if (id.is<const char*>()) {
        ID = ImGui::GetID(id.as<const char*>());
    } else if (id.is<void*>()) {
        ID = ImGui::GetID(id.as<void*>());
    } else {
        throw sol::error("Type must be int, const char* or void*");
    }

    ImGui::ActivateItemByID(ID);
}

void clear_active_id() {
    ImGui::ClearActiveID();
}

void focus_item() {
    ImGui::FocusItem();
}

void focus_window() {
    if (ImGuiWindow* w = ImGui::GetCurrentWindow()) {
        ImGui::FocusWindow(w);
    }
}

ImGuiContext* create_context(ImFontAtlas* shared_font_atlas) {
    return ImGui::CreateContext(shared_font_atlas);
}
void destroy_context(ImGuiContext* ctx) {
    ImGui::DestroyContext(ctx);
}

ImGuiContext*  get_current_context() {
    return ImGui::GetCurrentContext();
}

void begin_viewport_sidebar(const char* name, sol::object dir, sol::object size, sol::object flags_object = 0) {
    ImGuiViewport* vp  = ImGui::GetMainViewport();
    ImGui::BeginViewportSideBar(name, vp, (ImGuiDir)dir.as<int>(), (float)size.as<float>(), (ImGuiWindowFlags)flags_object.as<int>());
}
void platform_create_window(sol::object vp) {
    ImGuiViewport* viewport;
    if (vp.is<int>()) {
        viewport = ImGui::FindViewportByID(vp.as<int>());
    }
    ImGuiPlatformIO io = ImGui::GetPlatformIO();
    io.Platform_CreateWindow(viewport);
}

//bool set_shortcut_routing(sol::object key_chord, sol::object input_flags, sol::object owner_id) {
//    ImGuiID OwnerID = get_id(owner_id);
//    if (key_chord.is<sol::lua_table>()) {
//    }
//}
//ImGui::SetShortcutRouting(.)
//}

void text_wrapped(const char* text) {
    ImGui::TextWrapped(text);
}

// Mirrors ImGui::TextDisabled — same as text() but rendered in the style's
// disabled colour. Scripts (mine + user) reach for this naturally; without
// the binding they got "attempt to call a nil value (field 'text_disabled')"
// every frame, which then aborted the entire panel.
void text_disabled(const char* text) {
    ImGui::TextDisabled("%s", text);
}

//
//  // This is more or less equivalent to:
////   if (IsItemHovered() || IsItemActive())
////       SetKeyOwner(key, GetItemID());
//// Extensive uses of that (e.g. many calls for a single item) may want to manually perform the tests once and then call SetKeyOwner()
///multiple times. / More advanced usage scenarios may want to call SetKeyOwner() manually based on different condition. / Worth noting is
///that only one item can be hovered and only one item can be active, therefore this usage pattern doesn't need to bother with routing and
///priority.
// void ImGui::SetItemKeyOwner(ImGuiKey key, ImGuiInputFlags flags)
//{
//     ImGuiContext& g = *GImGui;
//     ImGuiID id = g.LastItemData.ID;
//     if (id == 0 || (g.HoveredId != id && g.ActiveId != id))
//         return;
//     if ((flags & ImGuiInputFlags_CondMask_) == 0)
//         flags |= ImGuiInputFlags_CondDefault_;
//     if ((g.HoveredId == id && (flags & ImGuiInputFlags_CondHovered)) || (g.ActiveId == id && (flags & ImGuiInputFlags_CondActive)))
//     {
//         IM_ASSERT((flags & ~ImGuiInputFlags_SupportedBySetItemKeyOwner) == 0); // Passing flags not supported by this function!
//         SetKeyOwner(key, id, flags & ~ImGuiInputFlags_CondMask_);
//     }
// }
//
// bool ImGui::Shortcut(ImGuiKeyChord key_chord, ImGuiID owner_id, ImGuiInputFlags flags)
//{
//     ImGuiContext& g = *GImGui;
//
//     // When using (owner_id == 0/Any): SetShortcutRouting() will use CurrentFocusScopeId and filter with this, so IsKeyPressed() is fine
//     with he 0/Any. if ((flags & ImGuiInputFlags_RouteMask_) == 0)
//         flags |= ImGuiInputFlags_RouteFocused;
//     if (!SetShortcutRouting(key_chord, owner_id, flags))
//         return false;
//
//     if (key_chord & ImGuiMod_Shortcut)
//         key_chord = ConvertShortcutMod(key_chord);
//     ImGuiKey mods = (ImGuiKey)(key_chord & ImGuiMod_Mask_);
//     if (g.IO.KeyMods != mods)
//         return false;
//
//     // Special storage location for mods
//     ImGuiKey key = (ImGuiKey)(key_chord & ~ImGuiMod_Mask_);
//     if (key == ImGuiKey_None)
//         key = ConvertSingleModFlagToKey(&g, mods);
//
//     if (!IsKeyPressed(key, owner_id, (flags & (ImGuiInputFlags_Repeat | (ImGuiInputFlags)ImGuiInputFlags_RepeatRateMask_))))
//         return false;
//     IM_ASSERT((flags & ~ImGuiInputFlags_SupportedByShortcut) == 0); // Passing flags not supported by this function!
//
//     return true;
// }

bool is_item_active() {
    return ImGui::IsItemActive();
}

bool is_item_focused() {
    return ImGui::IsItemFocused();
}

// dock in main panel
void set_next_window_docked(sol::object condition_obj) {
    ImGuiCond condition{};

    if (condition_obj.is<int>()) {
        condition = (ImGuiCond)condition_obj.as<int>();
    }

    ImGuiID dockID = ImGui::GetID("UEVR_Dockspace");
    ImGui::SetNextWindowDockID(dockID, condition);
}

// I'm not sure how far we'll go with this but I'll at least expose basic dockspace creation to lua
void set_next_window_dock_id(sol::object dockid, sol::object condition_obj) {
    ImGuiCond condition{};

    if (condition_obj.is<int>()) {
        condition = (ImGuiCond)condition_obj.as<int>();
    }

    ImGuiID dockID{};
    if (dockid.is<int>()) {
        dockID = ImGui::GetID(dockid.as<int>());
    } else if (dockid.is<const char*>()) {
        dockID = ImGui::GetID(dockid.as<const char*>());
    } else if (dockid.is<void*>()) {
        dockID = ImGui::GetID(dockid.as<void*>());

        ImGui::SetNextWindowDockID(dockID, condition);
    }
}

bool begin_window(const char* name, sol::object open_obj, ImGuiWindowFlags flags = 0) {
    if (name == nullptr) {
        name = "";
    }

    bool open = true;
    bool* open_p = nullptr;

    if (!open_obj.is<sol::nil_t>() && open_obj.is<bool>()) {
        open = open_obj.as<bool>();
        open_p = &open;
    }

    if (!open) {
        return false;
    }

    // Auto-dock new windows into the main UEVR dockspace host on first use,
    // so any script doing `imgui.begin_window("Foo")` from on_frame ends up
    // attached to the workspace covering the game window. The user can drag
    // the title-bar to detach later — SetNextWindowDockID with FirstUseEver
    // only assigns the dock once, then respects the user's choice.
    if (const auto host = Framework::get_main_dockspace_id(); host != 0) {
        ImGui::SetNextWindowDockID(host, ImGuiCond_FirstUseEver);
    }

    ImGui::Begin(name, open_p, flags);

    return open;
}

void end_window() {
    ImGui::End();
}

bool begin_child_window(const char* name, sol::object size_obj, sol::object border_obj, ImGuiWindowFlags flags = 0) {
    if (name == nullptr) {
        name = "";
    }

    const auto size = create_imvec2(size_obj);
    bool border{false};

    if (border_obj.is<bool>()) {
        border = border_obj.as<bool>();
    }

    return ImGui::BeginChild(name, size, border, flags);
}

void end_child_window() {
    ImGui::EndChild();
}

void begin_group() {
    ImGui::BeginGroup();
}

void end_group() {
    ImGui::EndGroup();
}

void begin_rect() {
    ImGui::BeginGroup();
}

void end_rect(sol::object additional_size_obj, sol::object rounding_obj) {
    ImGui::EndGroup();

    float rounding = rounding_obj.is<float>() ? rounding_obj.as<float>() : ImGui::GetStyle().FrameRounding;
    float additional_size = additional_size_obj.is<float>() ? additional_size_obj.as<float>() : 0.0f;

    auto mins = ImGui::GetItemRectMin();
    mins.x -= additional_size;
    mins.y -= additional_size;

    auto maxs = ImGui::GetItemRectMax();
    maxs.x += additional_size;
    maxs.y += additional_size;

    ImGui::GetWindowDrawList()->AddRect(
        mins, maxs, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersAll, 1.0f);
}

void begin_disabled(sol::object disabled_obj) {
    bool disabled{true};

    if (disabled_obj.is<bool>()) {
        disabled = disabled_obj.as<bool>();
    }

    ++g_disabled_counts;
    ImGui::BeginDisabled(disabled);
}

void end_disabled() {
    if (g_disabled_counts > 0) {
        --g_disabled_counts;
        ImGui::EndDisabled();
    }
}

void separator() {
    ImGui::Separator();
}

void spacing() {
    ImGui::Spacing();
}

void new_line() {
    ImGui::NewLine();
}

bool collapsing_header(const char* name) {
    return ImGui::CollapsingHeader(name);
}

int load_font(sol::object filepath_obj, int size /*, sol::object ranges*/) {
    namespace fs = std::filesystem;
    const char* filepath = "doesnt-exist.not-a-real-font";

    if (filepath_obj.is<const char*>()) {
        filepath = filepath_obj.as<const char*>();
    }

    if (std::filesystem::path(filepath).is_absolute()) {
        throw std::runtime_error("Font filepath must not be absolute.");
    }

    if (std::string{filepath}.find("..") != std::string::npos) {
        throw std::runtime_error("Font filepath cannot access parent directories.");
    }

    const auto global_fonts_path = Framework::get_persistent_dir().parent_path() / "UEVR" / "fonts";
    const auto windows_fonts_path = std::filesystem::path("C :\\WINDOWS\\Fonts");
    const auto fonts_path = Framework::get_persistent_dir() / "fonts";

    const auto font_path = std::filesystem::exists(fonts_path / filepath)          ? fonts_path / filepath
                           : std::filesystem::exists(global_fonts_path / filepath) ? (global_fonts_path / filepath)
                                                                                   : windows_fonts_path / filepath;

    fs::create_directories(fonts_path);
    std::vector<ImWchar> ranges_vec{};

    /*   if (ranges.is<sol::table>()) {
           sol::table ranges_table = ranges;

           for (auto i = 1u; i <= ranges_table.size(); ++i) {
               ranges_vec.push_back(ranges_table[i].get<ImWchar>());
           }
       }*/

    return g_framework->add_font(font_path, size /*, ranges_vec*/);
}

void push_font(int font) {
    ImGui::PushFont(g_framework->get_font(font));
}

void pop_font() {
    ImGui::PopFont();
}
void push_font_size(float size) {
    ImGui::PushFont(nullptr, size);
}
int get_default_font_size() {
    return g_framework->get_font_size();
}
void pop_font_size() {
    ImGui::PopFont();
}

sol::variadic_results color_picker(sol::this_state s, const char* label, unsigned int color, sol::object flags_obj) {
    ImGuiColorEditFlags flags{};

    if (flags_obj.is<int>()) {
        flags = (ImGuiColorEditFlags)flags_obj.as<int>();
    }

    auto r = color & 0xFF;
    auto g = (color >> 8) & 0xFF;
    auto b = (color >> 16) & 0xFF;
    auto a = (color >> 24) & 0xFF;

    float col[4]{
        r / 255.0f,
        g / 255.0f,
        b / 255.0f,
        a / 255.0f,
    };

    auto changed = ImGui::ColorPicker4(label, col, flags);

    r = (unsigned int)(col[0] * 255.0f);
    g = (unsigned int)(col[1] * 255.0f);
    b = (unsigned int)(col[2] * 255.0f);
    a = (unsigned int)(col[3] * 255.0f);

    unsigned int new_color = 0;

    new_color |= r;
    new_color |= g << 8;
    new_color |= b << 16;
    new_color |= a << 24;

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, new_color));

    return results;
}

sol::variadic_results color_picker_argb(sol::this_state s, const char* label, unsigned int color, sol::object flags_obj) {
    ImGuiColorEditFlags flags{};

    if (flags_obj.is<int>()) {
        flags = (ImGuiColorEditFlags)flags_obj.as<int>();
    }

    auto r = (color >> 16) & 0xFF;
    auto g = (color >> 8) & 0xFF;
    auto b = color & 0xFF;
    auto a = (color >> 24) & 0xFF;

    float col[4]{
        r / 255.0f,
        g / 255.0f,
        b / 255.0f,
        a / 255.0f,
    };

    auto changed = ImGui::ColorPicker4(label, col, flags);

    r = (unsigned int)(col[0] * 255.0f);
    g = (unsigned int)(col[1] * 255.0f);
    b = (unsigned int)(col[2] * 255.0f);
    a = (unsigned int)(col[3] * 255.0f);

    unsigned int new_color = 0;

    new_color |= r << 16;
    new_color |= g << 8;
    new_color |= b;
    new_color |= a << 24;

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, new_color));

    return results;
}

sol::variadic_results color_picker3(sol::this_state s, const char* label, Vector3f color, sol::object flags_obj) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiColorEditFlags flags{};

    if (flags_obj.is<int>()) {
        flags = (ImGuiColorEditFlags)flags_obj.as<int>();
    }

    auto changed = ImGui::ColorPicker3(label, &color.x, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, color));

    return results;
}

sol::variadic_results color_picker4(sol::this_state s, const char* label, Vector4f color, sol::object flags_obj) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiColorEditFlags flags{};

    if (flags_obj.is<int>()) {
        flags = (ImGuiColorEditFlags)flags_obj.as<int>();
    }

    auto changed = ImGui::ColorPicker4(label, &color.x, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, color));

    return results;
}

sol::variadic_results color_edit(sol::this_state s, const char* label, unsigned int color, sol::object flags_obj) {
    ImGuiColorEditFlags flags{};

    if (flags_obj.is<int>()) {
        flags = (ImGuiColorEditFlags)flags_obj.as<int>();
    }

    auto r = color & 0xFF;
    auto g = (color >> 8) & 0xFF;
    auto b = (color >> 16) & 0xFF;
    auto a = (color >> 24) & 0xFF;

    float col[4]{
        r / 255.0f,
        g / 255.0f,
        b / 255.0f,
        a / 255.0f,
    };

    auto changed = ImGui::ColorEdit4(label, col, flags);

    r = (unsigned int)(col[0] * 255.0f);
    g = (unsigned int)(col[1] * 255.0f);
    b = (unsigned int)(col[2] * 255.0f);
    a = (unsigned int)(col[3] * 255.0f);

    unsigned int new_color = 0;

    new_color |= r;
    new_color |= g << 8;
    new_color |= b << 16;
    new_color |= a << 24;

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, new_color));

    return results;
}

sol::variadic_results color_edit_argb(sol::this_state s, const char* label, unsigned int color, sol::object flags_obj) {
    ImGuiColorEditFlags flags{};

    if (flags_obj.is<int>()) {
        flags = (ImGuiColorEditFlags)flags_obj.as<int>();
    }

    auto r = (color >> 16) & 0xFF;
    auto g = (color >> 8) & 0xFF;
    auto b = color & 0xFF;
    auto a = (color >> 24) & 0xFF;

    float col[4]{
        r / 255.0f,
        g / 255.0f,
        b / 255.0f,
        a / 255.0f,
    };

    auto changed = ImGui::ColorEdit4(label, col, flags);

    r = (unsigned int)(col[0] * 255.0f);
    g = (unsigned int)(col[1] * 255.0f);
    b = (unsigned int)(col[2] * 255.0f);
    a = (unsigned int)(col[3] * 255.0f);

    unsigned int new_color = 0;

    new_color |= r << 16;
    new_color |= g << 8;
    new_color |= b;
    new_color |= a << 24;

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, new_color));

    return results;
}

sol::variadic_results color_edit3(sol::this_state s, const char* label, Vector3f color, sol::object flags_obj) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiColorEditFlags flags{};

    if (flags_obj.is<int>()) {
        flags = (ImGuiColorEditFlags)flags_obj.as<int>();
    }

    auto changed = ImGui::ColorEdit3(label, &color.x, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, color));

    return results;
}

sol::variadic_results color_edit4(sol::this_state s, const char* label, Vector4f color, sol::object flags_obj) {
    if (label == nullptr) {
        label = "";
    }

    ImGuiColorEditFlags flags{};

    if (flags_obj.is<int>()) {
        flags = (ImGuiColorEditFlags)flags_obj.as<int>();
    }

    auto changed = ImGui::ColorEdit4(label, &color.x, flags);

    sol::variadic_results results{};

    results.push_back(sol::make_object(s, changed));
    results.push_back(sol::make_object(s, color));

    return results;
}

void set_next_window_pos(sol::object pos_obj, sol::object condition_obj, sol::object pivot_obj) {
    ImGuiCond condition{};

    if (condition_obj.is<int>()) {
        condition = (ImGuiCond)condition_obj.as<int>();
    }

    auto pos = create_imvec2(pos_obj);
    auto pivot = create_imvec2(pivot_obj);

    ImGui::SetNextWindowPos(pos, condition, pivot);
}

void set_next_window_size(sol::object size_obj, sol::object condition_obj) {
    ImGuiCond condition{};

    if (condition_obj.is<int>()) {
        condition = (ImGuiCond)condition_obj.as<int>();
    }

    auto size = create_imvec2(size_obj);

    ImGui::SetNextWindowSize(size, condition);
}

void set_next_window_scroll(sol::object scroll_obj) {
    auto scroll = create_imvec2(scroll_obj);
    ImGui::SetNextWindowScroll({scroll.x, scroll.y});
}

void align_text_to_frame_padding() {
    ImGui::AlignTextToFramePadding();
}

float get_frame_height() {
    return ImGui::GetFrameHeight();
}

bool radio_button(const char* label, bool active) {
    return ImGui::RadioButton(label, active);
}

void push_id(sol::object id) {
    if (id.is<int>()) {
        ImGui::PushID(id.as<int>());
    } else if (id.is<const char*>()) {
        ImGui::PushID(id.as<const char*>());
    } else if (id.is<void*>()) {
        ImGui::PushID(id.as<void*>());
    } else {
        throw sol::error("Type must be int, const char* or void*");
    }
}

void pop_id() {
    ImGui::PopID();
}

ImGuiID get_id(sol::object id) {
    if (id.is<int>()) {
        return id.as<ImGuiID>();
    } else if (id.is<const char*>()) {
        return ImGui::GetID(id.as<const char*>());
    } else if (id.is<void*>()) {
        return ImGui::GetID(id.as<void*>());
    } else {
        throw sol::error("Type must be int, const char* or void*");
    }

    return 0;
}

Vector2f get_mouse() {
    const auto mouse = ImGui::GetMousePos();

    return Vector2f{
        mouse.x,
        mouse.y,
    };
}

int get_key_index(int imgui_key) {
    return (ImGuiKey)imgui_key;
}

bool is_key_down(int key) {
    return ImGui::IsKeyDown((ImGuiKey)key);
}

bool is_key_pressed(int key) {
    return ImGui::IsKeyPressed((ImGuiKey)key);
}

bool is_key_released(int key) {
    return ImGui::IsKeyReleased((ImGuiKey)key);
}

//// okay this isn't imgui obviously but it feels sensible to put in the imgui table for consistency
// actually on second thought we can just set the UE mouse position
// but lets add a way to remove the cursor patch to the api in general
// void set_mouse_pos(int x, int y) {
//   bool has_cursor_pos_patch = g_framework->has_set_cursor_pos_patch();
//    if (has_cursor_pos_patch) g_framework->remove_set_cursor_pos_patch( );
//
//    SetCursorPos(x, y);
//  if (has_cursor_pos_patch) g_framework->set_cursor_pos_patch( );
//}

bool is_mouse_down(int button) {
    return ImGui::IsMouseDown(button);
}

bool is_mouse_clicked(int button) {
    return ImGui::IsMouseClicked(button);
}

bool is_mouse_released(int button) {
    return ImGui::IsMouseReleased(button);
}

bool is_mouse_double_clicked(int button) {
    return ImGui::IsMouseDoubleClicked(button);
}

//    IMGUI_API bool IsMouseReleasedWithDelay(ImGuiMouseButton button,
//    float delay); // delayed mouse release (use very sparingly!). Generally used with 'delay >= io.MouseDoubleClickTime' + combined with a
//                  // 'io.MouseClickedLastCount==1' test. This is a very rarely used UI idiom, but some apps use this: e.g. MS Explorer
//                  // single click on an icon to rename.
// IMGUI_API int GetMouseClickedCount(
//    ImGuiMouseButton button); // return the number of successive mouse-clicks at the time where a click happen (otherwise 0).
// IMGUI_API bool IsMouseHoveringRect(const ImVec2& r_min, const ImVec2& r_max,
//    bool clip = true); // is mouse hovering given bounding rect (in screen space). clipped by current clipping settings, but disregarding
//    of
//                       // other consideration of focus/window ordering/popup-block.
// IMGUI_API bool IsMousePosValid(
//    const ImVec2* mouse_pos = NULL); // by convention we use (-FLT_MAX,-FLT_MAX) to denote that there is no mouse available
// IMGUI_API bool IsAnyMouseDown();     // [WILL OBSOLETE] is any mouse button held? This was designed for backends, but prefer having
// backend
//                                     // maintain a mask of held mouse buttons, because upcoming input queue system will make this invalid.
// IMGUI_API ImVec2 GetMousePos();      // shortcut to ImGui::GetIO().MousePos provided by user, to be consistent with other calls
// IMGUI_API ImVec2 GetMousePosOnOpeningCurrentPopup(); // retrieve mouse position at the time of opening popup we have BeginPopup() into
//                                                     // (helper to avoid user backing that value themselves)
// IMGUI_API bool IsMouseDragging(
//    ImGuiMouseButton button, float lock_threshold = -1.0f); // is mouse dragging? (uses io.MouseDraggingThreshold if lock_threshold <
//    0.0f)
// IMGUI_API ImVec2 GetMouseDragDelta(ImGuiMouseButton button = 0,
//    float lock_threshold = -1.0f); // return the delta from the initial clicking position while the mouse button is pressed or was just
//                                   // released. This is locked and return 0.0f until the mouse moves past a distance threshold at least
//                                   // once
//                                   // (uses io.MouseDraggingThreshold if lock_threshold < 0.0f)
// IMGUI_API void ResetMouseDragDelta(ImGuiMouseButton button = 0);

void indent(int indent_width) {
    ImGui::Indent(indent_width);
}

void unindent(int indent_width) {
    ImGui::Unindent(indent_width);
}

void begin_tooltip() {
    ImGui::BeginTooltip();
}

void end_tooltip() {
    ImGui::EndTooltip();
}

void set_tooltip(const char* text) {
    if (text == nullptr) {
        text = "";
    }

    ImGui::SetTooltip(text);
}

void open_popup(const char* str_id, sol::object flags_obj) {
    if (str_id == nullptr) {
        str_id = "";
    }

    ImGuiWindowFlags flags{0};

    if (flags_obj.is<int>()) {
        flags = (ImGuiWindowFlags)flags_obj.as<int>();
    }

    ImGui::OpenPopup(str_id, flags);
}

bool begin_popup(const char* str_id, sol::object flags_obj) {
    int flags{0};

    if (flags_obj.is<int>()) {
        flags = flags_obj.as<int>();
    }

    return ImGui::BeginPopup(str_id, flags);
}

// technically was possible to get the input blocking features using the modal flag on normal windows but let's make this available formally
bool begin_popup_modal(const char* str_id, sol::object open_obj, sol::object flags_obj) {

    bool open = true;
    bool* open_p = nullptr;

    if (!open_obj.is<sol::nil_t>() && open_obj.is<bool>()) {
        open = open_obj.as<bool>();
        open_p = &open;
    }

    if (!open) {
        return false;
    }

    int flags{0};

    if (flags_obj.is<int>()) {
        flags = flags_obj.as<int>();
    }

    return ImGui::BeginPopupModal(str_id, open_p, flags);
}

bool begin_popup_context_item(const char* str_id, sol::object flags_obj) {
    int flags{1};

    if (flags_obj.is<int>()) {
        flags = flags_obj.as<int>();
    }

    return ImGui::BeginPopupContextItem(str_id, flags);
}

void end_popup() {
    ImGui::EndPopup();
}

void close_current_popup() {
    ImGui::CloseCurrentPopup();
}

void close_non_modal_popups() {
    ImGui::ClosePopupsExceptModals();
}

bool is_popup_open(const char* str_id) {
    return ImGui::IsPopupOpen(str_id);
}

Vector2f calc_text_size(const char* text, sol::object text_end, sol::object double_hash_hides_text, sol::object wrap_width) {

    auto _text_end = text_end.is<const char*>() ? text_end.as<const char*>() : 0;
    bool hide_text_after_double_hash = double_hash_hides_text.is<bool>() ? double_hash_hides_text.as<bool>() : false;
    float text_wrap_width = wrap_width.is<float>() ? wrap_width.as<float>() : -1.0f;
    const auto result = ImGui::CalcTextSize(text, _text_end, hide_text_after_double_hash, text_wrap_width);
    return Vector2f{
        result.x,
        result.y,
    };
}

Vector2f get_content_region_available() {
    const auto result = ImGui::GetContentRegionAvail();
    return Vector2f{result.x, result.y};
}

Vector2f get_window_size() {
    const auto result = ImGui::GetWindowSize();

    return Vector2f{
        result.x,
        result.y,
    };
}

Vector2f get_window_pos() {
    const auto result = ImGui::GetWindowPos();

    return Vector2f{
        result.x,
        result.y,
    };
}

void set_next_item_open(bool is_open, sol::object condition_obj) {
    ImGuiCond condition{0};

    if (condition_obj.is<int>()) {
        condition = (ImGuiCond)condition_obj.as<int>();
    }

    ImGui::SetNextItemOpen(is_open, condition);
}

bool begin_list_box(const char* label, sol::object size_obj) {
    if (label == nullptr) {
        label = "";
    }

    auto size = create_imvec2(size_obj);

    return ImGui::BeginListBox(label, size);
}

//
// sol::variadic_results list_box(sol::this_state s, const char* label, sol::table items, sol::object selection, sol::object size_obj) {
//    if (label == nullptr) {
//        label = "";
//    }
//    auto count = items.size();
//
//    auto space = ImGui::GetContentRegionAvail();
//    auto size = create_imvec2(size_obj);
//
//    ImGui::PushID(label);
//    if (ImGui::BeginListBox(label, size))
//    {
//        auto it = 0;
//        for (auto& [key, val] : items)
//        {
//            try {
//                    auto id = imgui::get_id(key);
//                    ImGui::PushID(id);
//            }
//            catch (...) {
//                        ImGui::PushID(it);
//            }
//
//            const bool is_selected = (key == selection);
//            if (ImGui::Selectable(val.as<const char*>, is_selected, ImGuiSelectableFlags_AllowDoubleClick))
//                if (is_selected)
//                    ImGui::SetItemDefaultFocus();
//        ImGui::EndListBox();
//    }
//}

void end_list_box() {
    ImGui::EndListBox();
}

bool begin_menu_bar() {
    return ImGui::BeginMenuBar();
}

void end_menu_bar() {
    ImGui::EndMenuBar();
}

bool begin_main_menu_bar() {
    return ImGui::BeginMainMenuBar();
}

void end_main_menu_bar() {
    ImGui::EndMainMenuBar();
}

bool begin_menu(const char* label, sol::object enabled_obj) {
    if (label == nullptr) {
        label = "";
    }

    bool enabled{true};

    if (enabled_obj.is<bool>()) {
        enabled = enabled_obj.as<bool>();
    }

    return ImGui::BeginMenu(label, enabled);
}

void end_menu() {
    ImGui::EndMenu();
}

bool menu_item(const char* label, sol::object shortcut_obj, sol::object selected_obj, sol::object enabled_obj) {
    if (label == nullptr) {
        label = "";
    }

    const char* shortcut{nullptr};
    bool selected{false};
    bool enabled{true};

    if (shortcut_obj.is<const char*>()) {
        shortcut = shortcut_obj.as<const char*>();
    } else {
        shortcut = "";
    }

    if (selected_obj.is<bool>()) {
        selected = selected_obj.as<bool>();
    }

    if (enabled_obj.is<bool>()) {
        enabled = enabled_obj.as<bool>();
    }

    return ImGui::MenuItem(label, shortcut, selected, enabled);
}

// I'll add tab bar stuff if I decide I want it or someone specifically requests it but I think its pointless with docking...
// tabitem
// tabbar
// endtabbar

Vector2f get_display_size() {
    const auto& result = ImGui::GetIO().DisplaySize;

    return Vector2f{
        result.x,
        result.y,
    };
}

void push_item_width(float item_width) {
    ImGui::PushItemWidth(item_width);
}

void pop_item_width() {
    ImGui::PopItemWidth();
}

void set_next_item_width(float item_width) {
    ImGui::SetNextItemWidth(item_width);
}

float calc_item_width() {
    return ImGui::CalcItemWidth();
}
float calc_item_size() {
    // ImGui::CalcItemSize requires parameters; expose wrapper returning 0.0 by default
    return 0.0f;
}
void item_size(sol::object pos, sol::object size, sol::object text_baseline_y) {
    if (text_baseline_y.is<float>()) {
        ImGui::ItemSize(ImRect{create_imvec2(pos), create_imvec2(size)}, text_baseline_y.as<float>());
    } else {
        ImGui::ItemSize(ImRect{create_imvec2(pos), create_imvec2(size)});
    }
}

bool item_add(const char* label, sol::object pos, sol::object size) {
    if (label == nullptr) {
        label = "";
    }

    const auto window = ImGui::GetCurrentWindow();

    if (window == nullptr) {
        return false;
    }

    return ImGui::ItemAdd(ImRect{create_imvec2(pos), create_imvec2(size)}, window->GetID(label));
}

// for tree nodes
bool is_item_clicked() {
    return ImGui::IsItemClicked();
}

bool is_item_edited() {
    return ImGui::IsItemEdited();
}

bool is_item_visible() {
    return ImGui::IsItemVisible();
}

bool is_any_item_hovered() {
    return ImGui::IsAnyItemHovered();
}

bool is_item_toggled_selection() {
    return ImGui::IsItemToggledSelection();
}

bool is_any_item_active() {
    return ImGui::IsAnyItemActive();
}

bool is_any_item_focused() {
    return ImGui::IsAnyItemFocused();
}

bool is_item_toggled_open() {
    return ImGui::IsItemToggledOpen();
}

void set_next_item_allow_overlap() {
    ImGui::SetNextItemAllowOverlap();
}

void push_clip_rect(sol::object min, sol::object max, bool intersect) {
    ImGui::PushClipRect(create_imvec2(min), create_imvec2(max), intersect);
}

void pop_clip_rect() {
    ImGui::PopClipRect();
}

void push_item_flag(sol::object flags_object, sol::object enabled_obj) {
    auto flags = 0;
    if (flags_object.is<int>()) {
        flags = flags_object.as<int>();
    }
    auto enabled = true;
    if (enabled_obj.is<bool>()) {
        enabled = enabled_obj.as<bool>();
    }
    ImGui::PushItemFlag(flags, enabled);
}

void pop_item_flag() {
    ImGui::PopItemFlag();
}

bool selectable(const char* label, bool selected, sol::object flags_obj) {
    ImGuiSelectableFlags flags = 0;

    if (flags_obj.is<int>()) {
        flags = (ImGuiSelectableFlags)flags_obj.as<int>();
    }
    if (label == nullptr) {
        label = "";
    }
    return ImGui::Selectable(label, selected, flags);
}

// as the kind of psychopath who actually intentionally used the potential memory leak here to style the GUI, we should not allow that
// I won't get to it just yet but I'm thinking of taking a pretty heavy handed approach to managing imgui errors
// since the consequence is almost always program crash
void push_style_color(int style_color, sol::object color_obj) {
    if (color_obj.is<int>()) {
        ImGui::PushStyleColor((ImGuiCol)style_color, (ImU32)color_obj.as<int>());
    } else if (color_obj.is<Vector4f>()) {
        ImGui::PushStyleColor((ImGuiCol)style_color, create_imvec4(color_obj));
    }
}

void pop_style_color(sol::object count_obj) {
    int count{1};

    if (count_obj.is<int>()) {
        count = count_obj.as<int>();
    }

    ImGui::PopStyleColor(count);
}

void push_style_var(int idx, sol::object value_obj) {
    if (value_obj.is<float>()) {
        ImGui::PushStyleVar((ImGuiStyleVar)idx, value_obj.as<float>());
    } else if (value_obj.is<Vector2f>()) {
        ImGui::PushStyleVar((ImGuiStyleVar)idx, create_imvec2(value_obj));
    }
}

void pop_style_var(sol::object count_obj) {
    int count{1};

    if (count_obj.is<int>()) {
        count = count_obj.as<int>();
    }

    ImGui::PopStyleVar(count);
}

Vector2f get_cursor_pos() {
    const auto result = ImGui::GetCursorPos();

    return Vector2f{
        result.x,
        result.y,
    };
}

void set_cursor_pos(sol::object pos) {
    ImGui::SetCursorPos(create_imvec2(pos));
}

Vector2f get_cursor_start_pos() {
    const auto result = ImGui::GetCursorStartPos();

    return Vector2f{
        result.x,
        result.y,
    };
}

Vector2f get_cursor_screen_pos() {
    const auto result = ImGui::GetCursorScreenPos();

    return Vector2f{
        result.x,
        result.y,
    };
}

void set_cursor_screen_pos(sol::object pos) {
    ImGui::SetCursorScreenPos(create_imvec2(pos));
}

void set_item_default_focus() {
    ImGui::SetItemDefaultFocus();
}

void set_clipboard(sol::object data) {
    ImGui::SetClipboardText(data.as<const char*>());
}

const char* get_clipboard() {
    return ImGui::GetClipboardText();
}

void progress_bar(float progress, sol::object size, const char* overlay) {
    if (overlay == nullptr) {
        overlay = "";
    }

    ImGui::ProgressBar(progress, create_imvec2(size), overlay);
}

bool begin_table(const char* str_id, int column, sol::object flags_obj, sol::object outer_size_obj, sol::object inner_width_obj) {
    if (str_id == nullptr) {
        str_id = "";
    }

    ImVec2 outer_size{};
    if (outer_size_obj.is<Vector2f>()) {
        const auto& vec = outer_size_obj.as<Vector2f>();
        outer_size = {vec.x, vec.y};
    }

    float inner_width = inner_width_obj.is<float>() ? inner_width_obj.as<float>() : 0.0f;
    int flags = flags_obj.is<int>() ? flags_obj.as<int>() : 0;

    return ImGui::BeginTable(str_id, column, flags, outer_size, inner_width);
}

void end_table() {
    ImGui::EndTable();
}

void table_next_row(sol::object row_flags, sol::object min_row_height) {
    int flags = row_flags.is<int>() ? row_flags.as<int>() : 0;
    float min_height = min_row_height.is<float>() ? min_row_height.as<float>() : 0.0f;
    ImGui::TableNextRow(flags, min_height);
}

bool table_next_column() {
    return ImGui::TableNextColumn();
}

bool table_set_column_index(int column_index) {
    return ImGui::TableSetColumnIndex(column_index);
}

void table_setup_column(const char* label, sol::object flags_obj, sol::object init_width_or_weight_obj, sol::object user_id_obj) {
    if (label == nullptr) {
        label = "";
    }

    auto flags = flags_obj.is<int>() ? flags_obj.as<int>() : 0;
    auto init_width = init_width_or_weight_obj.is<float>() ? init_width_or_weight_obj.as<float>() : 0.0f;
    auto user_id = user_id_obj.is<ImGuiID>() ? user_id_obj.as<ImGuiID>() : 0;

    ImGui::TableSetupColumn(label, flags, init_width, user_id);
}

void table_setup_scroll_freeze(int cols, int rows) {
    ImGui::TableSetupScrollFreeze(cols, rows);
}

void table_headers_row() {
    ImGui::TableHeadersRow();
}

void table_header(const char* label) {
    if (label == nullptr) {
        label = "";
    }

    ImGui::TableHeader(label);
}

int table_get_column_count() {
    return ImGui::TableGetColumnCount();
}

int table_get_column_index() {
    return ImGui::TableGetColumnIndex();
}

int table_get_row_index() {
    return ImGui::TableGetRowIndex();
}

const char* table_get_column_name(int column = -1) {
    return ImGui::TableGetColumnName(column);
}

ImGuiTableColumnFlags table_get_column_flags(sol::object column) {
    return ImGui::TableGetColumnFlags(column.is<int>() ? column.as<int>() : -1);
}

void table_set_bg_color(ImGuiTableBgTarget target, ImU32 color, sol::object column) {
    ImGui::TableSetBgColor(target, color, column.is<int>() ? column.as<int>() : -1);
}

void table_set_bg_color_vec4(ImGuiTableBgTarget target, Vector4f color, sol::object column) {
    ImVec4 _color = {color.x, color.y, color.z, color.w};
    ImGui::TableSetBgColor(target, ImGui::ColorConvertFloat4ToU32(_color), column.is<int>() ? column.as<int>() : -1);
}

ImGuiTableSortSpecs* table_get_sort_specs() {
    return ImGui::TableGetSortSpecs();
}

// Window Drawlist

ImDrawList* get_window_draw_list() {
    return ImGui::GetWindowDrawList();
}

ImDrawList* get_background_draw_list() {
    return ImGui::GetBackgroundDrawList();
}

ImDrawList* get_foreground_draw_list() {
    return ImGui::GetForegroundDrawList();
}

// probably does not work
void draw_image(sol::object image, sol::object min, sol::object max, sol::object uvmin, sol::object uvmax, sol::object color) {
    if (auto dl = ImGui::GetWindowDrawList(); dl != nullptr) {
        auto texture = image.as<UEVR_FRHITexture2DHandle>();
        dl->AddImage(
            texture, create_imvec2(min), create_imvec2(max), create_imvec2(uvmin), create_imvec2(uvmax), create_imu32_color(color));
    }
}

void draw_list_path_clear() {
    if (auto dl = ImGui::GetWindowDrawList(); dl != nullptr) {
        dl->PathClear();
    }
}

void draw_list_path_line_to(sol::object pos_obj) {
    auto pos = create_imvec2(pos_obj);
    if (auto dl = ImGui::GetWindowDrawList(); dl != nullptr) {
        dl->PathLineTo(pos);
    }
}

void draw_list_path_stroke(ImU32 color, bool closed, float thickness) {
    if (auto dl = ImGui::GetWindowDrawList(); dl != nullptr) {
        dl->PathStroke(color, closed, thickness);
    }
}
} // namespace api::imgui

// Scroll APIs
namespace api::imgui {
float get_scroll_x() {
    return ImGui::GetScrollX();
}

float get_scroll_y() {
    return ImGui::GetScrollY();
}

void set_scroll_x(float scroll_x) {
    ImGui::SetScrollX(scroll_x);
}

void set_scroll_y(float scroll_y) {
    ImGui::SetScrollY(scroll_y);
}

float get_scroll_max_x() {
    return ImGui::GetScrollMaxX();
}

float get_scroll_max_y() {
    return ImGui::GetScrollMaxY();
}

void set_scroll_here_x(float center_x_ratio = 0.5f) {
    ImGui::SetScrollHereX(center_x_ratio);
}

void set_scroll_here_y(float center_y_ratio = 0.5f) {
    ImGui::SetScrollHereY(center_y_ratio);
}

void set_scroll_from_pos_x(float local_x, float center_x_ratio = 0.5f) {
    ImGui::SetScrollFromPosX(local_x, center_x_ratio);
}

void set_scroll_from_pos_y(float local_y, float center_y_ratio = 0.5f) {
    ImGui::SetScrollFromPosY(local_y, center_y_ratio);
}
} // namespace api::imgui

/*std::optional<Vector2f> world_to_screen(sol::object world_pos_object) {
/*
    if (world_pos_object.is<sol::nil_t>()) {
        return std::nullopt;
    }

    Vector4f world_pos{};

    if (world_pos_object.is<Vector2f>()) {
        auto& v2f = world_pos_object.as<Vector2f&>();
        world_pos = Vector4f{v2f.x, v2f.y, 0.0f, 1.0f};
    } else if (world_pos_object.is<Vector3f>()) {
        auto& v3f = world_pos_object.as<Vector3f&>();
        world_pos = Vector4f{v3f.x, v3f.y, v3f.z, 1.0f};
    } else if (world_pos_object.is<Vector4f>()) {
        auto& v4f = world_pos_object.as<Vector4f&>();
        world_pos = Vector4f{v4f.x, v4f.y, v4f.z, v4f.w};
    } else {
        return std::nullopt;
    }
    */

/*void world_text(const char* text, sol::object world_pos_object, ImU32 color = 0xFFFFFFFF) {
    auto screen_pos = world_to_screen(world_pos_object);

    if (!screen_pos) {
        return;
    }

    auto draw_list = ImGui::GetBackgroundDrawList();
    draw_list->AddText(ImVec2{screen_pos->x, screen_pos->y}, color, text);
}*/
namespace api::draw {
void text(const char* text, float x, float y, ImU32 color) {
    ImGui::GetWindowDrawList()->AddText(ImVec2{x, y}, color, text);
}
void text_ex(const char* text, float x, float y, ImU32 color, int font, float size) {
    ImFont* _font = g_framework->get_font(font);
    ImGui::GetWindowDrawList()->AddText(_font, size, ImVec2{x, y}, color, text);
}
void filled_rect(float x, float y, float w, float h, ImU32 color) {
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2{x, y}, ImVec2{x + w, y + h}, color);
}

void outline_rect(float x, float y, float w, float h, ImU32 color) {
    ImGui::GetWindowDrawList()->AddRect(ImVec2{x, y}, ImVec2{x + w, y + h}, color);
}

void line(float x1, float y1, float x2, float y2, ImU32 color) {
    ImGui::GetWindowDrawList()->AddLine(ImVec2{x1, y1}, ImVec2{x2, y2}, color);
}

void outline_circle(float x, float y, float radius, ImU32 color, sol::object num_segments) {
    auto segments = num_segments.is<sol::nil_t>() ? 32 : num_segments.as<int>();

    ImGui::GetWindowDrawList()->AddCircle(ImVec2{x, y}, radius, color, segments);
}

void filled_circle(float x, float y, float radius, ImU32 color, sol::object num_segments) {
    auto segments = num_segments.is<sol::nil_t>() ? 32 : num_segments.as<int>();

    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2{x, y}, radius, color, segments);
}

void outline_quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, ImU32 color) {
    ImGui::GetWindowDrawList()->AddQuad(ImVec2{x1, y1}, ImVec2{x2, y2}, ImVec2{x3, y3}, ImVec2{x4, y4}, color);
}

void filled_quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, ImU32 color) {
    ImGui::GetWindowDrawList()->AddQuadFilled(ImVec2{x1, y1}, ImVec2{x2, y2}, ImVec2{x3, y3}, ImVec2{x4, y4}, color);
}

void outline_triangle(float x1, float y1, float x2, float y2, float x3, float y3, ImU32 color) {
    ImGui::GetWindowDrawList()->AddTriangle(ImVec2{x1, y1}, ImVec2{x2, y2}, ImVec2{x3, y3}, color);
}

void filled_triangle(float x1, float y1, float x2, float y2, float x3, float y3, ImU32 color) {
    ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2{x1, y1}, ImVec2{x2, y2}, ImVec2{x3, y3}, color);
}

void outline_polyline(sol::object points, ImU32 color, float thickness) {
    if (points.is<sol::table>()) {
        sol::table points_table = points;

        auto count = points_table.size();

        for (auto i = 1u; i <= count; ++i) {
            sol::object point = points_table.get<sol::object>(i);

            if (point.is<Vector2f>()) {
                auto vec = point.as<Vector2f>();

                ImGui::GetWindowDrawList()->AddLine(ImVec2{vec.x, vec.y}, ImVec2{vec.x, vec.y}, color, thickness);
            }
        }
    }
}

void closed_polyline(sol::object points, ImU32 color, float thickness) {
    if (points.is<sol::table>()) {
        sol::table points_table = points;

        auto count = points_table.size();

        for (auto i = 1u; i <= count; ++i) {
            sol::object point = points_table.get<sol::object>(i);

            if (point.is<Vector2f>()) {
                auto vec = point.as<Vector2f>();

                ImGui::GetWindowDrawList()->AddLine(ImVec2{vec.x, vec.y}, ImVec2{vec.x, vec.y}, color, thickness);
            }
        }

        // Connect last to first
        sol::object first = points_table.get<sol::object>(1);
        sol::object last = points_table.get<sol::object>(count);
        if (first.is<Vector2f>() && last.is<Vector2f>()) {
            auto f = first.as<Vector2f>();
            auto l = last.as<Vector2f>();
            ImGui::GetWindowDrawList()->AddLine(ImVec2{f.x, f.y}, ImVec2{l.x, l.y}, color, thickness);
        }
    }
}

void draw_line(Vector2f p1, Vector2f p2, ImU32 color, float thickness) {
    ImGui::GetWindowDrawList()->AddLine(ImVec2{p1.x, p1.y}, ImVec2{p2.x, p2.y}, color, thickness);
}

void draw_rect(Vector2f pos, Vector2f size, ImU32 color, float rounding, float thickness) {
    ImGui::GetWindowDrawList()->AddRect(ImVec2{pos.x, pos.y}, ImVec2{pos.x + size.x, pos.y + size.y}, color, rounding, thickness);
}

void draw_filled_rect(Vector2f pos, Vector2f size, ImU32 color, float rounding) {
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2{pos.x, pos.y}, ImVec2{pos.x + size.x, pos.y + size.y}, color, rounding);
}

void draw_circle(Vector2f center, float radius, ImU32 color, int num_segments, float thickness) {
    ImGui::GetWindowDrawList()->AddCircle(ImVec2{center.x, center.y}, radius, color, num_segments, thickness);
}

void draw_filled_circle(Vector2f center, float radius, ImU32 color, int num_segments) {
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2{center.x, center.y}, radius, color, num_segments);
}

void draw_ngon(Vector2f center, float radius, ImU32 color, int num_segments, float thickness) {
    ImGui::GetWindowDrawList()->AddNgon(ImVec2{center.x, center.y}, radius, color, num_segments, thickness);
}

void draw_filled_ngon(Vector2f center, float radius, ImU32 color, int num_segments) {
    ImGui::GetWindowDrawList()->AddNgonFilled(ImVec2{center.x, center.y}, radius, color, num_segments);
}

void draw_bezier_curve(Vector2f p1, Vector2f p2, Vector2f p3, ImU32 color, float thickness) {
    // ImDrawList::AddBezierCubic expects 4 control points; approximate using available points by duplicating p3
    ImGui::GetWindowDrawList()->AddBezierCubic(
        ImVec2{p1.x, p1.y}, ImVec2{p2.x, p2.y}, ImVec2{p3.x, p3.y}, ImVec2{p3.x, p3.y}, color, thickness);
}

void draw_quad(Vector2f p1, Vector2f p2, Vector2f p3, Vector2f p4, ImU32 color, float thickness) {
    ImGui::GetWindowDrawList()->AddQuad(ImVec2{p1.x, p1.y}, ImVec2{p2.x, p2.y}, ImVec2{p3.x, p3.y}, ImVec2{p4.x, p4.y}, color, thickness);
}

void draw_filled_quad(Vector2f p1, Vector2f p2, Vector2f p3, Vector2f p4, ImU32 color) {
    ImGui::GetWindowDrawList()->AddQuadFilled(ImVec2{p1.x, p1.y}, ImVec2{p2.x, p2.y}, ImVec2{p3.x, p3.y}, ImVec2{p4.x, p4.y}, color);
}

// sol::variadic_results gizmo(sol::this_state s, int64_t unique_id, Matrix4x4f& transform, sol::object operation_obj, sol::object mode_obj)
// {
//     if (!ImGui::GetIO().MouseDown[0]) {
//         ImGuizmo::Enable(false);
//         ImGuizmo::Enable(true);
//     }
//
//     ImGuizmo::OPERATION operation{};
//     ImGuizmo::MODE mode{};
//
//     if (mode_obj.is<sol::nil_t>()) {
//         mode = ImGuizmo::MODE::WORLD;
//     } else if (mode_obj.is<int>()) {
//         mode = (ImGuizmo::MODE)mode_obj.as<int>();
//     } else {
//         throw sol::error("Invalid mode passed to gizmo");
//     }
//
//     if (operation_obj.is<sol::nil_t>()) {
//         operation = ImGuizmo::OPERATION::UNIVERSAL;
//     } else if (operation_obj.is<int>()) {
//         operation = (ImGuizmo::OPERATION)operation_obj.as<int>();
//     } else {
//         throw sol::error("Invalid operation passed to gizmo");
//     }
//
//     ImGuizmo::SetID(unique_id);
//     bool changed = ::imgui::draw_gizmo(transform, operation, mode);
//
//     sol::variadic_results results{};
//
//     results.push_back(sol::make_object<bool>(s, changed));
//     results.push_back(sol::make_object<Matrix4x4f>(s, transform));
//
//     return results;
// }

} // namespace api::draw
//
// ImVec2 world_to_screen(sol::object world_pos_object) {
//    static auto engine = sdk::UEngine::get();
//    static auto world = engine->get_world();
//    static auto player_controller = sdk::UGameplayStatics::get_player_controller(world, 0);
//    if (world_pos_object.is<Vector3f>()) {
//        auto& v3f = world_pos_object.as<Vector3f&>();
//    } else {
//        return std::nullopt;
//    }
//    Vector2f pos = sdk::UGameplayStatics::world_to_screen(player_controller, v3f);
//
//
//    return imgui::create_imvec2(pos);
//}
//
// void world_text(const char* text, sol::object world_pos_object, ImU32 color = 0xFFFFFFFF) {
//    auto screen_pos = world_to_screen(world_pos_object);
//
//    if (!screen_pos) {
//        return;
//    }
//
//    auto draw_list = ImGui::GetBackgroundDrawList();
//    draw_list->AddText(ImVec2{screen_pos->x, screen_pos->y}, color, text);
//}
//
//
// void sphere(sol::object camera_up, sol::object screen_pos_center, sol::object world_pos_object, float radius, ImU32 color, bool outline)
// {
//    Vector3f world_pos{};
//
//    if (world_pos_object.is<Vector2f>()) {
//        auto& v2f = world_pos_object.as<Vector2f&>();
//        world_pos = Vector3f{v2f.x, v2f.y, 0.0f};
//    } else if (world_pos_object.is<Vector3f>()) {
//        auto& v3f = world_pos_object.as<Vector3f&>();
//        world_pos = Vector3f{v3f.x, v3f.y, v3f.z};
//    } else if (world_pos_object.is<Vector4f>()) {
//        auto& v4f = world_pos_object.as<Vector4f&>();
//        world_pos = Vector3f{v4f.x, v4f.y, v4f.z};
//    } else {
//        return;
//    }

//    ::imgui::draw_sphere(world_pos, radius, color, outline);
//}
//
// void capsule(sol::object camera_up, sol::object screen_pos_center, sol::object start_pos_object, sol::object end_pos_object, float
// radius,
//    ImU32 color, bool outline) {
//    Vector3f start_pos{};
//
//    if (start_pos_object.is<Vector2f>()) {
//        auto& v2f = start_pos_object.as<Vector2f&>();
//        start_pos = Vector3f{v2f.x, v2f.y, 0.0f};
//    } else if (start_pos_object.is<Vector3f>()) {
//        auto& v3f = start_pos_object.as<Vector3f&>();
//        start_pos = Vector3f{v3f.x, v3f.y, v3f.z};
//    } else if (start_pos_object.is<Vector4f>()) {
//        auto& v4f = start_pos_object.as<Vector4f&>();
//        start_pos = Vector3f{v4f.x, v4f.y, v4f.z};
//    } else {
//        return;
//    }

//    Vector3f end_pos{};
//
//    if (end_pos_object.is<Vector2f>()) {
//        auto& v2f = end_pos_object.as<Vector2f&>();
//        end_pos = Vector3f{v2f.x, v2f.y, 0.0f};
//    } else if (end_pos_object.is<Vector3f>()) {
//        auto& v3f = end_pos_object.as<Vector3f&>();
//        end_pos = Vector3f{v3f.x, v3f.y, v3f.z};
//    } else if (end_pos_object.is<Vector4f>()) {
//        auto& v4f = end_pos_object.as<Vector4f&>();
//        end_pos = Vector3f{v4f.x, v4f.y, v4f.z};
//    } else {
//        return;
//    }

//    ::imgui::draw_capsule(start_pos, end_pos, radius, color, outline);
//}

//
// triangle
// ellipse
// ngon
// bezier
// image // read from utextures >:)
// #include "..\pluginloader\FRHITexture2DFunctions.hpp"
//
// void texture_image(sol::object utexture) {
//    const auto native_texture = utexture.is<>() ? flags_obj.as<int>() : 0;
//}
//} // namespace api::draw

void bindings::open_imgui(sol::state_view& lua) {
    auto imgui = lua.create_table();

    // Function bindings (alphabetical, deduplicated)
    imgui["accept_drag_drop"] = api::imgui::accept_drag_drop;
    imgui["accept_payload"] = api::imgui::accept_payload;
    imgui["activate_item_by_id"] = api::imgui::activate_item_by_id;
    imgui["align_text_to_frame_padding"] = api::imgui::align_text_to_frame_padding;
    imgui["arrow_button"] = api::imgui::arrow_button;
    imgui["begin_child_window"] = api::imgui::begin_child_window;
    imgui["begin_disabled"] = api::imgui::begin_disabled;
    imgui["begin_drag_drop_source"] = api::imgui::begin_drag_drop_source;
    imgui["begin_drag_drop_target"] = api::imgui::begin_drag_drop_target;
    imgui["begin_group"] = api::imgui::begin_group;
    imgui["begin_list_box"] = api::imgui::begin_list_box;
    imgui["begin_main_menu_bar"] = api::imgui::begin_main_menu_bar;
    imgui["begin_menu"] = api::imgui::begin_menu;
    imgui["begin_menu_bar"] = api::imgui::begin_menu_bar;
    imgui["begin_multi_select"] = api::imgui::begin_multi_select;
    imgui["begin_popup"] = api::imgui::begin_popup;
    imgui["begin_popup_context_item"] = api::imgui::begin_popup_context_item;
    imgui["begin_popup_modal"] = api::imgui::begin_popup_modal;
    imgui["begin_rect"] = api::imgui::begin_rect;
    imgui["begin_table"] = api::imgui::begin_table;
    imgui["begin_tooltip"] = api::imgui::begin_tooltip;
    imgui["begin_viewport_sidebar"] = api::imgui::begin_viewport_sidebar;
    imgui["begin_window"] = api::imgui::begin_window;
    imgui["bullet"] = api::imgui::bullet;
    imgui["bullet_text"] = api::imgui::bullet_text;
    imgui["button"] = api::imgui::button;
    imgui["calc_item_size"] = api::imgui::calc_item_size;
    imgui["calc_item_width"] = api::imgui::calc_item_width;
    imgui["calc_text_size"] = api::imgui::calc_text_size;
    imgui["checkbox"] = api::imgui::checkbox;
    imgui["clear_active_id"] = api::imgui::clear_active_id;
    imgui["close_current_popup"] = api::imgui::close_current_popup;
    imgui["close_non_modal_popups"] = api::imgui::close_non_modal_popups;
    imgui["collapsing_header"] = api::imgui::collapsing_header;
    imgui["color_edit"] = api::imgui::color_edit;
    imgui["color_edit3"] = api::imgui::color_edit3;
    imgui["color_edit4"] = api::imgui::color_edit4;
    imgui["color_edit_argb"] = api::imgui::color_edit_argb;
    imgui["color_picker"] = api::imgui::color_picker;
    imgui["color_picker3"] = api::imgui::color_picker3;
    imgui["color_picker4"] = api::imgui::color_picker4;
    imgui["color_picker_argb"] = api::imgui::color_picker_argb;
    imgui["combo"] = api::imgui::combo;
    imgui["create_imu32_color"] = api::imgui::create_imu32_color;
    imgui["create_imvec4_color"] = api::imgui::create_imvec4_color;
    imgui["create_platform_window"] = api::imgui::platform_create_window;
    imgui["drag_float"] = api::imgui::drag_float;
    imgui["drag_float2"] = api::imgui::drag_float2;
    imgui["drag_float3"] = api::imgui::drag_float3;
    imgui["drag_float4"] = api::imgui::drag_float4;
    imgui["drag_int"] = api::imgui::drag_int;
    imgui["draw_list_path_clear"] = api::imgui::draw_list_path_clear;
    imgui["draw_list_path_line_to"] = api::imgui::draw_list_path_line_to;
    imgui["draw_list_path_stroke"] = api::imgui::draw_list_path_stroke;
    imgui["draw_scene_texture"] = api::imgui::draw_scene_texture;
    imgui["end_child_window"] = api::imgui::end_child_window;
    imgui["end_disabled"] = api::imgui::end_disabled;
    imgui["end_drag_drop_source"] = api::imgui::end_drag_drop_source;
    imgui["end_drag_drop_target"] = api::imgui::end_drag_drop_target;
    imgui["end_group"] = api::imgui::end_group;
    imgui["end_list_box"] = api::imgui::end_list_box;
    imgui["end_main_menu_bar"] = api::imgui::end_main_menu_bar;
    imgui["end_menu"] = api::imgui::end_menu;
    imgui["end_menu_bar"] = api::imgui::end_menu_bar;
    imgui["end_multi_select"] = api::imgui::end_multi_select;
    imgui["end_popup"] = api::imgui::end_popup;
    imgui["end_rect"] = api::imgui::end_rect;
    imgui["end_table"] = api::imgui::end_table;
    imgui["end_tooltip"] = api::imgui::end_tooltip;
    imgui["end_window"] = api::imgui::end_window;
    imgui["focus_item"] = api::imgui::focus_item;
    imgui["focus_window"] = api::imgui::focus_window;
    imgui["get_active_id"] = api::imgui::get_active_id;
    imgui["get_background_draw_list"] = api::imgui::get_background_draw_list;
    imgui["get_clipboard"] = api::imgui::get_clipboard;
    imgui["get_content_region_available"] = api::imgui::get_content_region_available;
    imgui["get_cursor_pos"] = api::imgui::get_cursor_pos;
    imgui["get_cursor_screen_pos"] = api::imgui::get_cursor_screen_pos;
    imgui["get_cursor_start_pos"] = api::imgui::get_cursor_start_pos;
    imgui["get_default_font_size"] = api::imgui::get_default_font_size;
    imgui["get_display_size"] = api::imgui::get_display_size;
    imgui["get_foreground_draw_list"] = api::imgui::get_foreground_draw_list;
    imgui["get_frame_height"] = api::imgui::get_frame_height;
    imgui["get_hovered_id"] = api::imgui::get_hovered_id;
    imgui["get_id"] = api::imgui::get_id;
    imgui["get_id_from_pos"] = api::imgui::get_id_from_pos;
    imgui["get_item_id"] = api::imgui::get_item_id;
    imgui["get_key_index"] = api::imgui::get_key_index;
    imgui["get_mouse"] = api::imgui::get_mouse;
    imgui["get_scroll_max_x"] = api::imgui::get_scroll_max_x;
    imgui["get_scroll_max_y"] = api::imgui::get_scroll_max_y;
    imgui["get_scroll_x"] = api::imgui::get_scroll_x;
    imgui["get_scroll_y"] = api::imgui::get_scroll_y;
    imgui["get_window_draw_list"] = api::imgui::get_window_draw_list;
    imgui["get_window_pos"] = api::imgui::get_window_pos;
    imgui["get_window_size"] = api::imgui::get_window_size;
    imgui["indent"] = api::imgui::indent;
    imgui["input_text"] = api::imgui::input_text;
    imgui["input_text_multiline"] = api::imgui::input_text_multiline;
    imgui["invisible_button"] = api::imgui::invisible_button;
    imgui["is_any_item_active"] = api::imgui::is_any_item_active;
    imgui["is_any_item_focused"] = api::imgui::is_any_item_focused;
    imgui["is_any_item_hovered"] = api::imgui::is_any_item_hovered;
    imgui["is_item_active"] = api::imgui::is_item_active;
    imgui["is_item_clicked"] = api::imgui::is_item_clicked;
    imgui["is_item_edited"] = api::imgui::is_item_edited;
    imgui["is_item_focused"] = api::imgui::is_item_focused;
    imgui["is_item_hovered"] = api::imgui::is_item_hovered;
    imgui["is_item_toggled_open"] = api::imgui::is_item_toggled_open;
    imgui["is_item_toggled_selection"] = api::imgui::is_item_toggled_selection;
    imgui["is_item_visible"] = api::imgui::is_item_visible;
    imgui["is_key_down"] = api::imgui::is_key_down;
    imgui["is_key_pressed"] = api::imgui::is_key_pressed;
    imgui["is_key_released"] = api::imgui::is_key_released;
    imgui["is_mouse_clicked"] = api::imgui::is_mouse_clicked;
    imgui["is_mouse_double_clicked"] = api::imgui::is_mouse_double_clicked;
    imgui["is_mouse_down"] = api::imgui::is_mouse_down;
    imgui["is_mouse_released"] = api::imgui::is_mouse_released;
    imgui["is_payload_accepted"] = api::imgui::is_payload_accepted;
    imgui["is_popup_open"] = api::imgui::is_popup_open;
    imgui["item_add"] = api::imgui::item_add;
    imgui["item_size"] = api::imgui::item_size;
    imgui["label_text"] = api::imgui::label_text;
    imgui["load_font"] = api::imgui::load_font;
    imgui["menu_item"] = api::imgui::menu_item;
    imgui["new_line"] = api::imgui::new_line;
    imgui["open_popup"] = api::imgui::open_popup;
    imgui["pop_button_repeat"] = api::imgui::pop_button_repeat;
    imgui["pop_clip_rect"] = api::imgui::pop_clip_rect;
    imgui["pop_font"] = api::imgui::pop_font;
    imgui["pop_font_size"] = api::imgui::pop_font_size;
    imgui["pop_id"] = api::imgui::pop_id;
    imgui["pop_item_flag"] = api::imgui::pop_item_flag;
    imgui["pop_item_width"] = api::imgui::pop_item_width;
    imgui["pop_style_color"] = api::imgui::pop_style_color;
    imgui["pop_style_var"] = api::imgui::pop_style_var;
    imgui["progress_bar"] = api::imgui::progress_bar;
    imgui["push_button_repeat"] = api::imgui::push_button_repeat;
    imgui["push_clip_rect"] = api::imgui::push_clip_rect;
    imgui["push_font"] = api::imgui::push_font;
    imgui["push_font_size"] = api::imgui::push_font_size;
    imgui["push_id"] = api::imgui::push_id;
    imgui["push_item_flag"] = api::imgui::push_item_flag;
    imgui["push_item_width"] = api::imgui::push_item_width;
    imgui["push_override_id"] = api::imgui::push_override_id;
    imgui["push_style_color"] = api::imgui::push_style_color;
    imgui["push_style_var"] = api::imgui::push_style_var;
    imgui["radio_button"] = api::imgui::radio_button;
    imgui["render_drag_drop"] = api::imgui::render_drag_drop;
    imgui["same_line"] = api::imgui::same_line;
    imgui["selectable"] = api::imgui::selectable;
    imgui["separator"] = api::imgui::separator;
    imgui["separator_text"] = api::imgui::separator_text;
    imgui["set_clipboard"] = api::imgui::set_clipboard;
    imgui["set_cursor_pos"] = api::imgui::set_cursor_pos;
    imgui["set_cursor_screen_pos"] = api::imgui::set_cursor_screen_pos;
    imgui["set_drag_drop_payload"] = api::imgui::set_drag_drop_payload;
    imgui["set_item_default_focus"] = api::imgui::set_item_default_focus;
    imgui["set_next_item_allow_overlap"] = api::imgui::set_next_item_allow_overlap;
    imgui["set_next_item_open"] = api::imgui::set_next_item_open;
    imgui["set_next_item_width"] = api::imgui::set_next_item_width;
    imgui["set_next_window_dock_id"] = api::imgui::set_next_window_dock_id;
    imgui["set_next_window_docked"] = api::imgui::set_next_window_docked;
    // Returns the ID of the always-on UEVR_MainDockSpace covering the
    // entire game window. begin_window() auto-docks into this on first
    // use, but scripts that build their own dock tree or use direct
    // ImGui::Begin / sub-dockspaces can grab it here.
    imgui["get_main_dockspace_id"] = []() -> ImGuiID { return Framework::get_main_dockspace_id(); };
    imgui["set_next_window_pos"] = api::imgui::set_next_window_pos;
    imgui["set_next_window_scroll"] = api::imgui::set_next_window_scroll;
    imgui["set_next_window_size"] = api::imgui::set_next_window_size;
    imgui["set_scroll_from_pos_x"] = api::imgui::set_scroll_from_pos_x;
    imgui["set_scroll_from_pos_y"] = api::imgui::set_scroll_from_pos_y;
    imgui["set_scroll_here_x"] = api::imgui::set_scroll_here_x;
    imgui["set_scroll_here_y"] = api::imgui::set_scroll_here_y;
    imgui["set_scroll_x"] = api::imgui::set_scroll_x;
    imgui["set_scroll_y"] = api::imgui::set_scroll_y;
    imgui["set_tooltip"] = api::imgui::set_tooltip;
    imgui["show_debug_log_window"] = api::imgui::show_debug_log_window;
    imgui["show_demo_window"] = api::imgui::show_demo_window;
    imgui["show_font_atlas"] = api::imgui::show_font_atlas;
    imgui["show_font_selector"] = api::imgui::show_font_selector;
    imgui["show_metrics_window"] = api::imgui::show_metrics_window;
    imgui["show_stack_tool_window"] = api::imgui::show_stack_tool_window;
    imgui["slider_float"] = api::imgui::slider_float;
    imgui["slider_int"] = api::imgui::slider_int;
    imgui["small_button"] = api::imgui::small_button;
    imgui["spacing"] = api::imgui::spacing;
    imgui["table_get_column_count"] = api::imgui::table_get_column_count;
    imgui["table_get_column_flags"] = api::imgui::table_get_column_flags;
    imgui["table_get_column_index"] = api::imgui::table_get_column_index;
    imgui["table_get_column_name"] = api::imgui::table_get_column_name;
    imgui["table_get_row_index"] = api::imgui::table_get_row_index;
    imgui["table_get_sort_specs"] = api::imgui::table_get_sort_specs;
    imgui["table_header"] = api::imgui::table_header;
    imgui["table_headers_row"] = api::imgui::table_headers_row;
    imgui["table_next_column"] = api::imgui::table_next_column;
    imgui["table_next_row"] = api::imgui::table_next_row;
    imgui["table_set_bg_color"] = api::imgui::table_set_bg_color;
    imgui["table_set_column_index"] = api::imgui::table_set_column_index;
    imgui["table_setup_column"] = api::imgui::table_setup_column;
    imgui["table_setup_scroll_freeze"] = api::imgui::table_setup_scroll_freeze;
    imgui["text"] = api::imgui::text;
    imgui["text_colored"] = api::imgui::text_colored;
    imgui["text_wrapped"] = api::imgui::text_wrapped;
    imgui["text_disabled"] = api::imgui::text_disabled;
    imgui["tree_node"] = api::imgui::tree_node;
    imgui["tree_node_ptr_id"] = api::imgui::tree_node_ptr_id;
    imgui["tree_node_str_id"] = api::imgui::tree_node_str_id;
    imgui["tree_pop"] = api::imgui::tree_pop;
    imgui["unindent"] = api::imgui::unindent;
    imgui["vslider_float"] = api::imgui::vslider_float;
    imgui["vslider_int"] = api::imgui::vslider_int;

    imgui.new_usertype<ImGuiTableSortSpecs>(
        "TableSortSpecs", "specs_dirty", &ImGuiTableSortSpecs::SpecsDirty, "get_specs", [](ImGuiTableSortSpecs* specs) {
            std::vector<ImGuiTableColumnSortSpecs*> out{};

            for (int i = 0; i < specs->SpecsCount; ++i) {
                out.push_back(const_cast<ImGuiTableColumnSortSpecs*>(specs->Specs + i));
            }

            return out;
        });
    imgui.new_usertype<ImGuiTableColumnSortSpecs>("TableColumnSortSpecs", "user_id", &ImGuiTableColumnSortSpecs::ColumnUserID,
        "column_index", &ImGuiTableColumnSortSpecs::ColumnIndex, "sort_order", &ImGuiTableColumnSortSpecs::SortOrder, "sort_direction",
        sol::readonly_property([](ImGuiTableColumnSortSpecs* specs) { return specs->SortDirection; }));
    imgui.new_enum("ColumnFlags", "None", ImGuiTableColumnFlags_None, "DefaultHide", ImGuiTableColumnFlags_DefaultHide, "DefaultSort",
        ImGuiTableColumnFlags_DefaultSort, "WidthStretch", ImGuiTableColumnFlags_WidthStretch, "WidthFixed",
        ImGuiTableColumnFlags_WidthFixed, "NoResize", ImGuiTableColumnFlags_NoResize, "NoReorder", ImGuiTableColumnFlags_NoReorder,
        "NoHide", ImGuiTableColumnFlags_NoHide, "NoClip", ImGuiTableColumnFlags_NoClip, "NoSort", ImGuiTableColumnFlags_NoSort,
        "NoSortAscending", ImGuiTableColumnFlags_NoSortAscending, "NoSortDescending", ImGuiTableColumnFlags_NoSortDescending,
        "NoHeaderWidth", ImGuiTableColumnFlags_NoHeaderWidth, "PreferSortAscending", ImGuiTableColumnFlags_PreferSortAscending,
        "PreferSortDescending", ImGuiTableColumnFlags_PreferSortDescending, "IndentEnable", ImGuiTableColumnFlags_IndentEnable,
        "IndentDisable", ImGuiTableColumnFlags_IndentDisable, "IsEnabled", ImGuiTableColumnFlags_IsEnabled, "IsVisible",
        ImGuiTableColumnFlags_IsVisible, "IsSorted", ImGuiTableColumnFlags_IsSorted, "IsHovered", ImGuiTableColumnFlags_IsHovered);
    imgui.new_enum("BackendFlags", "HasGamepad", ImGuiBackendFlags_HasGamepad, "HasMouseCursors", ImGuiBackendFlags_HasMouseCursors,
            "HasMouseHoveredViewport", ImGuiBackendFlags_HasMouseHoveredViewport, "HasParentViewport", ImGuiBackendFlags_HasParentViewport,
            "HasSetMousePos", ImGuiBackendFlags_HasSetMousePos, "None", ImGuiBackendFlags_None, "PlatformHasViewports",
            ImGuiBackendFlags_PlatformHasViewports, "RendererHasTextures", ImGuiBackendFlags_RendererHasTextures, "RendererHasViewports",
            ImGuiBackendFlags_RendererHasViewports, "RendererHasVtxOffset", ImGuiBackendFlags_RendererHasVtxOffset);

            imgui.new_enum("ButtonFlags", "EnableNav", ImGuiButtonFlags_EnableNav, "MouseButtonLeft", ImGuiButtonFlags_MouseButtonLeft,
            "MouseButtonMask_", ImGuiButtonFlags_MouseButtonMask_, "MouseButtonMiddle", ImGuiButtonFlags_MouseButtonMiddle,
            "MouseButtonRight", ImGuiButtonFlags_MouseButtonRight, "None", ImGuiButtonFlags_None);

            imgui.new_enum("ChildFlags", "AlwaysAutoResize", ImGuiChildFlags_AlwaysAutoResize, "AlwaysUseWindowPadding",
            ImGuiChildFlags_AlwaysUseWindowPadding, "AutoResizeX", ImGuiChildFlags_AutoResizeX, "AutoResizeY", ImGuiChildFlags_AutoResizeY,
            "Borders", ImGuiChildFlags_Borders, "FrameStyle", ImGuiChildFlags_FrameStyle, "NavFlattened", ImGuiChildFlags_NavFlattened,
            "None", ImGuiChildFlags_None, "ResizeX", ImGuiChildFlags_ResizeX, "ResizeY", ImGuiChildFlags_ResizeY);

            imgui.new_enum("Col", "TreeLines", ImGuiCol_TreeLines);

            imgui.new_enum("ColorEditFlags", "AlphaBar", ImGuiColorEditFlags_AlphaBar, "AlphaMask_", ImGuiColorEditFlags_AlphaMask_, "AlphaNoBg",
            ImGuiColorEditFlags_AlphaNoBg, "AlphaOpaque", ImGuiColorEditFlags_AlphaOpaque, "AlphaPreview", ImGuiColorEditFlags_AlphaPreview,
            "AlphaPreviewHalf", ImGuiColorEditFlags_AlphaPreviewHalf, "DataTypeMask_", ImGuiColorEditFlags_DataTypeMask_, "DefaultOptions_",
            ImGuiColorEditFlags_DefaultOptions_, "DisplayHex", ImGuiColorEditFlags_DisplayHex, "DisplayHSV", ImGuiColorEditFlags_DisplayHSV,
            "DisplayMask_", ImGuiColorEditFlags_DisplayMask_, "DisplayRGB", ImGuiColorEditFlags_DisplayRGB, "Float",
            ImGuiColorEditFlags_Float, "HDR", ImGuiColorEditFlags_HDR, "InputHSV", ImGuiColorEditFlags_InputHSV, "InputMask_",
            ImGuiColorEditFlags_InputMask_, "InputRGB", ImGuiColorEditFlags_InputRGB, "NoAlpha", ImGuiColorEditFlags_NoAlpha, "NoBorder",
            ImGuiColorEditFlags_NoBorder, "NoDragDrop", ImGuiColorEditFlags_NoDragDrop, "NoInputs", ImGuiColorEditFlags_NoInputs, "NoLabel",
            ImGuiColorEditFlags_NoLabel, "None", ImGuiColorEditFlags_None, "NoOptions", ImGuiColorEditFlags_NoOptions, "NoPicker",
            ImGuiColorEditFlags_NoPicker, "NoSidePreview", ImGuiColorEditFlags_NoSidePreview, "NoSmallPreview",
            ImGuiColorEditFlags_NoSmallPreview, "NoTooltip", ImGuiColorEditFlags_NoTooltip, "PickerHueBar",
            ImGuiColorEditFlags_PickerHueBar, "PickerHueWheel", ImGuiColorEditFlags_PickerHueWheel, "PickerMask_",
            ImGuiColorEditFlags_PickerMask_, "Uint8", ImGuiColorEditFlags_Uint8);

            imgui.new_enum("ComboFlags", "HeightLarge", ImGuiComboFlags_HeightLarge, "HeightLargest", ImGuiComboFlags_HeightLargest, "HeightMask_",
            ImGuiComboFlags_HeightMask_, "HeightRegular", ImGuiComboFlags_HeightRegular, "HeightSmall", ImGuiComboFlags_HeightSmall,
            "NoArrowButton", ImGuiComboFlags_NoArrowButton, "None", ImGuiComboFlags_None, "NoPreview", ImGuiComboFlags_NoPreview,
            "PopupAlignLeft", ImGuiComboFlags_PopupAlignLeft, "WidthFitPreview", ImGuiComboFlags_WidthFitPreview);

            imgui.new_enum("ConfigFlags", "DockingEnable", ImGuiConfigFlags_DockingEnable, "DpiEnableScaleFonts",
            ImGuiConfigFlags_DpiEnableScaleFonts, "DpiEnableScaleViewports", ImGuiConfigFlags_DpiEnableScaleViewports, "IsSRGB",
            ImGuiConfigFlags_IsSRGB, "IsTouchScreen", ImGuiConfigFlags_IsTouchScreen, "NavEnableGamepad", ImGuiConfigFlags_NavEnableGamepad,
            "NavEnableKeyboard", ImGuiConfigFlags_NavEnableKeyboard, "NavEnableSetMousePos", ImGuiConfigFlags_NavEnableSetMousePos,
            "NavNoCaptureKeyboard", ImGuiConfigFlags_NavNoCaptureKeyboard, "NoKeyboard", ImGuiConfigFlags_NoKeyboard, "NoMouse",
            ImGuiConfigFlags_NoMouse, "NoMouseCursorChange", ImGuiConfigFlags_NoMouseCursorChange, "None", ImGuiConfigFlags_None,
            "ViewportsEnable", ImGuiConfigFlags_ViewportsEnable);

            imgui.new_enum("DockNodeFlags", "AutoHideTabBar", ImGuiDockNodeFlags_AutoHideTabBar, "KeepAliveOnly", ImGuiDockNodeFlags_KeepAliveOnly,
            "NoDockingInCentralNode", ImGuiDockNodeFlags_NoDockingInCentralNode, "NoDockingOverCentralNode",
            ImGuiDockNodeFlags_NoDockingOverCentralNode, "NoDockingSplit", ImGuiDockNodeFlags_NoDockingSplit, "None",
            ImGuiDockNodeFlags_None, "NoResize", ImGuiDockNodeFlags_NoResize, "NoSplit", ImGuiDockNodeFlags_NoSplit, "NoUndocking",
            ImGuiDockNodeFlags_NoUndocking, "PassthruCentralNode", ImGuiDockNodeFlags_PassthruCentralNode);

            imgui.new_enum("DragDropFlags", "AcceptBeforeDelivery", ImGuiDragDropFlags_AcceptBeforeDelivery, "AcceptDrawAsHovered",
            ImGuiDragDropFlags_AcceptDrawAsHovered, "AcceptNoDrawDefaultRect", ImGuiDragDropFlags_AcceptNoDrawDefaultRect,
            "AcceptNoPreviewTooltip", ImGuiDragDropFlags_AcceptNoPreviewTooltip, "AcceptPeekOnly", ImGuiDragDropFlags_AcceptPeekOnly,
            "None", ImGuiDragDropFlags_None, "PayloadAutoExpire", ImGuiDragDropFlags_PayloadAutoExpire, "PayloadNoCrossContext",
            ImGuiDragDropFlags_PayloadNoCrossContext, "PayloadNoCrossProcess", ImGuiDragDropFlags_PayloadNoCrossProcess,
            "SourceAllowNullID", ImGuiDragDropFlags_SourceAllowNullID, "SourceAutoExpirePayload",
            ImGuiDragDropFlags_SourceAutoExpirePayload, "SourceExtern", ImGuiDragDropFlags_SourceExtern, "SourceNoDisableHover",
            ImGuiDragDropFlags_SourceNoDisableHover, "SourceNoHoldToOpenOthers", ImGuiDragDropFlags_SourceNoHoldToOpenOthers,
            "SourceNoPreviewTooltip", ImGuiDragDropFlags_SourceNoPreviewTooltip);

            imgui.new_enum("FocusedFlags", "AnyWindow", ImGuiFocusedFlags_AnyWindow, "ChildWindows", ImGuiFocusedFlags_ChildWindows, "DockHierarchy",
            ImGuiFocusedFlags_DockHierarchy, "None", ImGuiFocusedFlags_None, "NoPopupHierarchy", ImGuiFocusedFlags_NoPopupHierarchy,
            "RootAndChildWindows", ImGuiFocusedFlags_RootAndChildWindows, "RootWindow", ImGuiFocusedFlags_RootWindow);

            imgui.new_enum("HoveredFlags", "AllowWhenBlockedByActiveItem", ImGuiHoveredFlags_AllowWhenBlockedByActiveItem, "AllowWhenBlockedByPopup",
            ImGuiHoveredFlags_AllowWhenBlockedByPopup, "AllowWhenDisabled", ImGuiHoveredFlags_AllowWhenDisabled, "AllowWhenOverlapped",
            ImGuiHoveredFlags_AllowWhenOverlapped, "AllowWhenOverlappedByItem", ImGuiHoveredFlags_AllowWhenOverlappedByItem,
            "AllowWhenOverlappedByWindow", ImGuiHoveredFlags_AllowWhenOverlappedByWindow, "AnyWindow", ImGuiHoveredFlags_AnyWindow,
            "ChildWindows", ImGuiHoveredFlags_ChildWindows, "DelayNone", ImGuiHoveredFlags_DelayNone, "DelayNormal",
            ImGuiHoveredFlags_DelayNormal, "DelayShort", ImGuiHoveredFlags_DelayShort, "DockHierarchy", ImGuiHoveredFlags_DockHierarchy,
            "ForTooltip", ImGuiHoveredFlags_ForTooltip, "NoNavOverride", ImGuiHoveredFlags_NoNavOverride, "None", ImGuiHoveredFlags_None,
            "NoPopupHierarchy", ImGuiHoveredFlags_NoPopupHierarchy, "NoSharedDelay", ImGuiHoveredFlags_NoSharedDelay, "RectOnly",
            ImGuiHoveredFlags_RectOnly, "RootAndChildWindows", ImGuiHoveredFlags_RootAndChildWindows, "RootWindow",
            ImGuiHoveredFlags_RootWindow, "Stationary", ImGuiHoveredFlags_Stationary);

            imgui.new_enum("InputFlags", "None", ImGuiInputFlags_None, "Repeat", ImGuiInputFlags_Repeat, "RouteActive", ImGuiInputFlags_RouteActive,
            "RouteAlways", ImGuiInputFlags_RouteAlways, "RouteFocused", ImGuiInputFlags_RouteFocused, "RouteFromRootWindow",
            ImGuiInputFlags_RouteFromRootWindow, "RouteGlobal", ImGuiInputFlags_RouteGlobal, "RouteOverActive",
            ImGuiInputFlags_RouteOverActive, "RouteOverFocused", ImGuiInputFlags_RouteOverFocused, "RouteUnlessBgFocused",
            ImGuiInputFlags_RouteUnlessBgFocused, "Tooltip", ImGuiInputFlags_Tooltip);

            imgui.new_enum("InputTextFlags", "AllowTabInput", ImGuiInputTextFlags_AllowTabInput, "AlwaysOverwrite",
            ImGuiInputTextFlags_AlwaysOverwrite, "AutoSelectAll", ImGuiInputTextFlags_AutoSelectAll, "CallbackAlways",
            ImGuiInputTextFlags_CallbackAlways, "CallbackCharFilter", ImGuiInputTextFlags_CallbackCharFilter, "CallbackCompletion",
            ImGuiInputTextFlags_CallbackCompletion, "CallbackEdit", ImGuiInputTextFlags_CallbackEdit, "CallbackHistory",
            ImGuiInputTextFlags_CallbackHistory, "CallbackResize", ImGuiInputTextFlags_CallbackResize, "CharsDecimal",
            ImGuiInputTextFlags_CharsDecimal, "CharsHexadecimal", ImGuiInputTextFlags_CharsHexadecimal, "CharsNoBlank",
            ImGuiInputTextFlags_CharsNoBlank, "CharsScientific", ImGuiInputTextFlags_CharsScientific, "CharsUppercase",
            ImGuiInputTextFlags_CharsUppercase, "CtrlEnterForNewLine", ImGuiInputTextFlags_CtrlEnterForNewLine, "DisplayEmptyRefVal",
            ImGuiInputTextFlags_DisplayEmptyRefVal, "ElideLeft", ImGuiInputTextFlags_ElideLeft, "EnterReturnsTrue",
            ImGuiInputTextFlags_EnterReturnsTrue, "EscapeClearsAll", ImGuiInputTextFlags_EscapeClearsAll, "NoHorizontalScroll",
            ImGuiInputTextFlags_NoHorizontalScroll, "None", ImGuiInputTextFlags_None, "NoUndoRedo", ImGuiInputTextFlags_NoUndoRedo,
            "ParseEmptyRefVal", ImGuiInputTextFlags_ParseEmptyRefVal, "Password", ImGuiInputTextFlags_Password, "ReadOnly",
            ImGuiInputTextFlags_ReadOnly, "WordWrap", ImGuiInputTextFlags_WordWrap);

            imgui.new_enum("ItemFlags", "AllowDuplicateId", ImGuiItemFlags_AllowDuplicateId, "AutoClosePopups", ImGuiItemFlags_AutoClosePopups,
            "ButtonRepeat", ImGuiItemFlags_ButtonRepeat, "NoNav", ImGuiItemFlags_NoNav, "NoNavDefaultFocus",
            ImGuiItemFlags_NoNavDefaultFocus, "None", ImGuiItemFlags_None, "NoTabStop", ImGuiItemFlags_NoTabStop);

            imgui.new_enum("PopupFlags", "AnyPopup", ImGuiPopupFlags_AnyPopup, "AnyPopupId", ImGuiPopupFlags_AnyPopupId, "AnyPopupLevel",
            ImGuiPopupFlags_AnyPopupLevel, "MouseButtonDefault_", ImGuiPopupFlags_MouseButtonDefault_, "MouseButtonLeft",
            ImGuiPopupFlags_MouseButtonLeft, "MouseButtonMask_", ImGuiPopupFlags_MouseButtonMask_, "MouseButtonMiddle",
            ImGuiPopupFlags_MouseButtonMiddle, "MouseButtonRight", ImGuiPopupFlags_MouseButtonRight, "None", ImGuiPopupFlags_None,
            "NoOpenOverExistingPopup", ImGuiPopupFlags_NoOpenOverExistingPopup, "NoOpenOverItems", ImGuiPopupFlags_NoOpenOverItems,
            "NoReopen", ImGuiPopupFlags_NoReopen);

            imgui.new_enum("SelectableFlags", "AllowDoubleClick", ImGuiSelectableFlags_AllowDoubleClick, "AllowOverlap",
            ImGuiSelectableFlags_AllowOverlap, "Disabled", ImGuiSelectableFlags_Disabled, "DontClosePopups",
            ImGuiSelectableFlags_DontClosePopups, "Highlight", ImGuiSelectableFlags_Highlight, "NoAutoClosePopups",
            ImGuiSelectableFlags_NoAutoClosePopups, "None", ImGuiSelectableFlags_None, "SelectOnNav", ImGuiSelectableFlags_SelectOnNav,
            "SpanAllColumns", ImGuiSelectableFlags_SpanAllColumns);

            imgui.new_enum("SliderFlags", "AlwaysClamp", ImGuiSliderFlags_AlwaysClamp, "ClampOnInput", ImGuiSliderFlags_ClampOnInput,
            "ClampZeroRange", ImGuiSliderFlags_ClampZeroRange, "InvalidMask_", ImGuiSliderFlags_InvalidMask_, "Logarithmic",
            ImGuiSliderFlags_Logarithmic, "NoInput", ImGuiSliderFlags_NoInput, "None", ImGuiSliderFlags_None, "NoRoundToFormat",
            ImGuiSliderFlags_NoRoundToFormat, "NoSpeedTweaks", ImGuiSliderFlags_NoSpeedTweaks, "WrapAround", ImGuiSliderFlags_WrapAround);

            imgui.new_enum("TabBarFlags", "AutoSelectNewTabs", ImGuiTabBarFlags_AutoSelectNewTabs, "DrawSelectedOverline",
            ImGuiTabBarFlags_DrawSelectedOverline, "FittingPolicyDefault_", ImGuiTabBarFlags_FittingPolicyDefault_, "FittingPolicyMask_",
            ImGuiTabBarFlags_FittingPolicyMask_, "FittingPolicyMixed", ImGuiTabBarFlags_FittingPolicyMixed, "FittingPolicyResizeDown",
            ImGuiTabBarFlags_FittingPolicyResizeDown, "FittingPolicyScroll", ImGuiTabBarFlags_FittingPolicyScroll, "FittingPolicyShrink",
            ImGuiTabBarFlags_FittingPolicyShrink, "NoCloseWithMiddleMouseButton", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton, "None",
            ImGuiTabBarFlags_None, "NoTabListScrollingButtons", ImGuiTabBarFlags_NoTabListScrollingButtons, "NoTooltip",
            ImGuiTabBarFlags_NoTooltip, "Reorderable", ImGuiTabBarFlags_Reorderable, "TabListPopupButton",
            ImGuiTabBarFlags_TabListPopupButton);

            imgui.new_enum("TabItemFlags", "Leading", ImGuiTabItemFlags_Leading, "NoAssumedClosure", ImGuiTabItemFlags_NoAssumedClosure,
            "NoCloseWithMiddleMouseButton", ImGuiTabItemFlags_NoCloseWithMiddleMouseButton, "None", ImGuiTabItemFlags_None, "NoPushId",
            ImGuiTabItemFlags_NoPushId, "NoReorder", ImGuiTabItemFlags_NoReorder, "NoTooltip", ImGuiTabItemFlags_NoTooltip, "SetSelected",
            ImGuiTabItemFlags_SetSelected, "Trailing", ImGuiTabItemFlags_Trailing, "UnsavedDocument", ImGuiTabItemFlags_UnsavedDocument);

            imgui.new_enum("TableColumnFlags", "AngledHeader", ImGuiTableColumnFlags_AngledHeader, "DefaultHide", ImGuiTableColumnFlags_DefaultHide,
            "DefaultSort", ImGuiTableColumnFlags_DefaultSort, "Disabled", ImGuiTableColumnFlags_Disabled, "IndentDisable",
            ImGuiTableColumnFlags_IndentDisable, "IndentEnable", ImGuiTableColumnFlags_IndentEnable, "IndentMask_",
            ImGuiTableColumnFlags_IndentMask_, "IsEnabled", ImGuiTableColumnFlags_IsEnabled, "IsHovered", ImGuiTableColumnFlags_IsHovered,
            "IsSorted", ImGuiTableColumnFlags_IsSorted, "IsVisible", ImGuiTableColumnFlags_IsVisible, "NoClip",
            ImGuiTableColumnFlags_NoClip, "NoDirectResize_", ImGuiTableColumnFlags_NoDirectResize_, "NoHeaderLabel",
            ImGuiTableColumnFlags_NoHeaderLabel, "NoHeaderWidth", ImGuiTableColumnFlags_NoHeaderWidth, "NoHide",
            ImGuiTableColumnFlags_NoHide, "None", ImGuiTableColumnFlags_None, "NoReorder", ImGuiTableColumnFlags_NoReorder, "NoResize",
            ImGuiTableColumnFlags_NoResize, "NoSort", ImGuiTableColumnFlags_NoSort, "NoSortAscending",
            ImGuiTableColumnFlags_NoSortAscending, "NoSortDescending", ImGuiTableColumnFlags_NoSortDescending, "PreferSortAscending",
            ImGuiTableColumnFlags_PreferSortAscending, "PreferSortDescending", ImGuiTableColumnFlags_PreferSortDescending, "StatusMask_",
            ImGuiTableColumnFlags_StatusMask_, "WidthFixed", ImGuiTableColumnFlags_WidthFixed, "WidthMask_",
            ImGuiTableColumnFlags_WidthMask_, "WidthStretch", ImGuiTableColumnFlags_WidthStretch);

            imgui.new_enum("TableFlags", "Borders", ImGuiTableFlags_Borders, "BordersH", ImGuiTableFlags_BordersH, "BordersInner",
            ImGuiTableFlags_BordersInner, "BordersInnerH", ImGuiTableFlags_BordersInnerH, "BordersInnerV", ImGuiTableFlags_BordersInnerV,
            "BordersOuter", ImGuiTableFlags_BordersOuter, "BordersOuterH", ImGuiTableFlags_BordersOuterH, "BordersOuterV",
            ImGuiTableFlags_BordersOuterV, "BordersV", ImGuiTableFlags_BordersV, "ContextMenuInBody", ImGuiTableFlags_ContextMenuInBody,
            "Hideable", ImGuiTableFlags_Hideable, "HighlightHoveredColumn", ImGuiTableFlags_HighlightHoveredColumn, "NoBordersInBody",
            ImGuiTableFlags_NoBordersInBody, "NoBordersInBodyUntilResize", ImGuiTableFlags_NoBordersInBodyUntilResize, "NoClip",
            ImGuiTableFlags_NoClip, "NoHostExtendX", ImGuiTableFlags_NoHostExtendX, "NoHostExtendY", ImGuiTableFlags_NoHostExtendY,
            "NoKeepColumnsVisible", ImGuiTableFlags_NoKeepColumnsVisible, "None", ImGuiTableFlags_None, "NoPadInnerX",
            ImGuiTableFlags_NoPadInnerX, "NoPadOuterX", ImGuiTableFlags_NoPadOuterX, "NoSavedSettings", ImGuiTableFlags_NoSavedSettings,
            "PadOuterX", ImGuiTableFlags_PadOuterX, "PreciseWidths", ImGuiTableFlags_PreciseWidths, "Reorderable",
            ImGuiTableFlags_Reorderable, "Resizable", ImGuiTableFlags_Resizable, "RowBg", ImGuiTableFlags_RowBg, "ScrollX",
            ImGuiTableFlags_ScrollX, "ScrollY", ImGuiTableFlags_ScrollY, "SizingFixedFit", ImGuiTableFlags_SizingFixedFit,
            "SizingFixedSame", ImGuiTableFlags_SizingFixedSame, "SizingMask_", ImGuiTableFlags_SizingMask_, "SizingStretchProp",
            ImGuiTableFlags_SizingStretchProp, "SizingStretchSame", ImGuiTableFlags_SizingStretchSame, "Sortable", ImGuiTableFlags_Sortable,
            "SortMulti", ImGuiTableFlags_SortMulti, "SortTristate", ImGuiTableFlags_SortTristate);

            imgui.new_enum("TableRowFlags", "Headers", ImGuiTableRowFlags_Headers, "None", ImGuiTableRowFlags_None);

            imgui.new_enum("TreeNodeFlags", "AllowOverlap", ImGuiTreeNodeFlags_AllowOverlap, "Bullet", ImGuiTreeNodeFlags_Bullet, "CollapsingHeader",
            ImGuiTreeNodeFlags_CollapsingHeader, "DefaultOpen", ImGuiTreeNodeFlags_DefaultOpen, "DrawLinesFull",
            ImGuiTreeNodeFlags_DrawLinesFull, "DrawLinesNone", ImGuiTreeNodeFlags_DrawLinesNone, "DrawLinesToNodes",
            ImGuiTreeNodeFlags_DrawLinesToNodes, "Framed", ImGuiTreeNodeFlags_Framed, "FramePadding", ImGuiTreeNodeFlags_FramePadding,
            "LabelSpanAllColumns", ImGuiTreeNodeFlags_LabelSpanAllColumns, "Leaf", ImGuiTreeNodeFlags_Leaf, "NavLeftJumpsBackHere",
            ImGuiTreeNodeFlags_NavLeftJumpsBackHere, "NavLeftJumpsToParent", ImGuiTreeNodeFlags_NavLeftJumpsToParent, "NoAutoOpenOnLog",
            ImGuiTreeNodeFlags_NoAutoOpenOnLog, "None", ImGuiTreeNodeFlags_None, "NoTreePushOnOpen", ImGuiTreeNodeFlags_NoTreePushOnOpen,
            "OpenOnArrow", ImGuiTreeNodeFlags_OpenOnArrow, "OpenOnDoubleClick", ImGuiTreeNodeFlags_OpenOnDoubleClick, "Selected",
            ImGuiTreeNodeFlags_Selected, "SpanAllColumns", ImGuiTreeNodeFlags_SpanAllColumns, "SpanAvailWidth",
            ImGuiTreeNodeFlags_SpanAvailWidth, "SpanFullWidth", ImGuiTreeNodeFlags_SpanFullWidth, "SpanLabelWidth",
            ImGuiTreeNodeFlags_SpanLabelWidth, "SpanTextWidth", ImGuiTreeNodeFlags_SpanTextWidth);

            imgui.new_enum("WindowFlags", "AlwaysAutoResize", ImGuiWindowFlags_AlwaysAutoResize, "AlwaysHorizontalScrollbar",
            ImGuiWindowFlags_AlwaysHorizontalScrollbar, "AlwaysVerticalScrollbar", ImGuiWindowFlags_AlwaysVerticalScrollbar, "ChildMenu",
            ImGuiWindowFlags_ChildMenu, "ChildWindow", ImGuiWindowFlags_ChildWindow, "DockNodeHost", ImGuiWindowFlags_DockNodeHost,
            "HorizontalScrollbar", ImGuiWindowFlags_HorizontalScrollbar, "MenuBar", ImGuiWindowFlags_MenuBar, "Modal",
            ImGuiWindowFlags_Modal, "NoBackground", ImGuiWindowFlags_NoBackground, "NoBringToFrontOnFocus",
            ImGuiWindowFlags_NoBringToFrontOnFocus, "NoCollapse", ImGuiWindowFlags_NoCollapse, "NoDecoration",
            ImGuiWindowFlags_NoDecoration, "NoDocking", ImGuiWindowFlags_NoDocking, "NoFocusOnAppearing",
            ImGuiWindowFlags_NoFocusOnAppearing, "NoInputs", ImGuiWindowFlags_NoInputs, "NoMouseInputs", ImGuiWindowFlags_NoMouseInputs,
            "NoMove", ImGuiWindowFlags_NoMove, "NoNav", ImGuiWindowFlags_NoNav, "NoNavFocus", ImGuiWindowFlags_NoNavFocus, "NoNavInputs",
            ImGuiWindowFlags_NoNavInputs, "None", ImGuiWindowFlags_None, "NoResize", ImGuiWindowFlags_NoResize, "NoSavedSettings",
            ImGuiWindowFlags_NoSavedSettings, "NoScrollbar", ImGuiWindowFlags_NoScrollbar, "NoScrollWithMouse",
            ImGuiWindowFlags_NoScrollWithMouse, "NoTitleBar", ImGuiWindowFlags_NoTitleBar, "Popup", ImGuiWindowFlags_Popup, "Tooltip",
            ImGuiWindowFlags_Tooltip, "UnsavedDocument", ImGuiWindowFlags_UnsavedDocument);

    imgui.new_enum("ImGuiKey",

        // Keys
        "Key_None", ImGuiKey_None, "Key_Tab", ImGuiKey_Tab, "Key_LeftArrow", ImGuiKey_LeftArrow, "Key_RightArrow", ImGuiKey_RightArrow,
        "Key_UpArrow", ImGuiKey_UpArrow, "Key_DownArrow", ImGuiKey_DownArrow, "Key_PageUp", ImGuiKey_PageUp, "Key_PageDown",
        ImGuiKey_PageDown, "Key_Home", ImGuiKey_Home, "Key_End", ImGuiKey_End, "Key_Insert", ImGuiKey_Insert, "Key_Delete", ImGuiKey_Delete,
        "Key_Backspace", ImGuiKey_Backspace, "Key_Space", ImGuiKey_Space, "Key_Enter", ImGuiKey_Enter, "Key_Escape", ImGuiKey_Escape,
        "Key_LeftCtrl", ImGuiKey_LeftCtrl, "Key_LeftShift", ImGuiKey_LeftShift, "Key_LeftAlt", ImGuiKey_LeftAlt, "Key_LeftSuper",
        ImGuiKey_LeftSuper, "Key_RightCtrl", ImGuiKey_RightCtrl, "Key_RightShift", ImGuiKey_RightShift, "Key_RightAlt", ImGuiKey_RightAlt,
        "Key_RightSuper", ImGuiKey_RightSuper, "Key_Menu", ImGuiKey_Menu, "Key_0", ImGuiKey_0, "Key_1", ImGuiKey_1, "Key_2", ImGuiKey_2,
        "Key_3", ImGuiKey_3, "Key_4", ImGuiKey_4, "Key_5", ImGuiKey_5, "Key_6", ImGuiKey_6, "Key_7", ImGuiKey_7, "Key_8", ImGuiKey_8,
        "Key_9", ImGuiKey_9, "Key_A", ImGuiKey_A, "Key_B", ImGuiKey_B, "Key_C", ImGuiKey_C, "Key_D", ImGuiKey_D, "Key_E", ImGuiKey_E,
        "Key_F", ImGuiKey_F, "Key_G", ImGuiKey_G, "Key_H", ImGuiKey_H, "Key_I", ImGuiKey_I, "Key_J", ImGuiKey_J, "Key_K", ImGuiKey_K,
        "Key_L", ImGuiKey_L, "Key_M", ImGuiKey_M, "Key_N", ImGuiKey_N, "Key_O", ImGuiKey_O, "Key_P", ImGuiKey_P, "Key_Q", ImGuiKey_Q,
        "Key_R", ImGuiKey_R, "Key_S", ImGuiKey_S, "Key_T", ImGuiKey_T, "Key_U", ImGuiKey_U, "Key_V", ImGuiKey_V, "Key_W", ImGuiKey_W,
        "Key_X", ImGuiKey_X, "Key_Y", ImGuiKey_Y, "Key_Z", ImGuiKey_Z, "Key_F1", ImGuiKey_F1, "Key_F2", ImGuiKey_F2, "Key_F3", ImGuiKey_F3,
        "Key_F4", ImGuiKey_F4, "Key_F5", ImGuiKey_F5, "Key_F6", ImGuiKey_F6, "Key_F7", ImGuiKey_F7, "Key_F8", ImGuiKey_F8, "Key_F9",
        ImGuiKey_F9, "Key_F10", ImGuiKey_F10, "Key_F11", ImGuiKey_F11, "Key_F12", ImGuiKey_F12, "Key_Apostrophe", ImGuiKey_Apostrophe,
        "Key_Comma", ImGuiKey_Comma, "Key_Minus", ImGuiKey_Minus, "Key_Period", ImGuiKey_Period, "Key_Slash", ImGuiKey_Slash,
        "Key_Semicolon", ImGuiKey_Semicolon, "Key_Equal", ImGuiKey_Equal, "Key_LeftBracket", ImGuiKey_LeftBracket, "Key_Backslash",
        ImGuiKey_Backslash, "Key_RightBracket", ImGuiKey_RightBracket, "Key_GraveAccent", ImGuiKey_GraveAccent, "Key_CapsLock",
        ImGuiKey_CapsLock, "Key_ScrollLock", ImGuiKey_ScrollLock, "Key_NumLock", ImGuiKey_NumLock, "Key_PrintScreen", ImGuiKey_PrintScreen,
        "Key_Pause", ImGuiKey_Pause, "Key_Keypad0", ImGuiKey_Keypad0, "Key_Keypad1", ImGuiKey_Keypad1, "Key_Keypad2", ImGuiKey_Keypad2,
        "Key_Keypad3", ImGuiKey_Keypad3, "Key_Keypad4", ImGuiKey_Keypad4, "Key_Keypad5", ImGuiKey_Keypad5, "Key_Keypad6", ImGuiKey_Keypad6,
        "Key_Keypad7", ImGuiKey_Keypad7, "Key_Keypad8", ImGuiKey_Keypad8, "Key_Keypad9", ImGuiKey_Keypad9, "Key_KeypadDecimal",
        ImGuiKey_KeypadDecimal, "Key_KeypadDivide", ImGuiKey_KeypadDivide, "Key_KeypadMultiply", ImGuiKey_KeypadMultiply,
        "Key_KeypadSubtract", ImGuiKey_KeypadSubtract, "Key_KeypadAdd", ImGuiKey_KeypadAdd, "Key_KeypadEnter", ImGuiKey_KeypadEnter,
        "Key_KeypadEqual", ImGuiKey_KeypadEqual, "Key_GamepadStart", ImGuiKey_GamepadStart, "Key_GamepadBack", ImGuiKey_GamepadBack,
        "Key_GamepadFaceLeft", ImGuiKey_GamepadFaceLeft, "Key_GamepadFaceRight", ImGuiKey_GamepadFaceRight, "Key_GamepadFaceUp",
        ImGuiKey_GamepadFaceUp, "Key_GamepadFaceDown", ImGuiKey_GamepadFaceDown, "Key_GamepadDpadLeft", ImGuiKey_GamepadDpadLeft,
        "Key_GamepadDpadRight", ImGuiKey_GamepadDpadRight, "Key_GamepadDpadUp", ImGuiKey_GamepadDpadUp, "Key_GamepadDpadDown",
        ImGuiKey_GamepadDpadDown, "Key_GamepadL1", ImGuiKey_GamepadL1, "Key_GamepadR1", ImGuiKey_GamepadR1, "Key_GamepadL2",
        ImGuiKey_GamepadL2, "Key_GamepadR2", ImGuiKey_GamepadR2, "Key_GamepadL3", ImGuiKey_GamepadL3, "Key_GamepadR3", ImGuiKey_GamepadR3,
        "Key_GamepadLStickLeft", ImGuiKey_GamepadLStickLeft, "Key_GamepadLStickRight", ImGuiKey_GamepadLStickRight, "Key_GamepadLStickUp",
        ImGuiKey_GamepadLStickUp, "Key_GamepadLStickDown", ImGuiKey_GamepadLStickDown, "Key_GamepadRStickLeft", ImGuiKey_GamepadRStickLeft,
        "Key_GamepadRStickRight", ImGuiKey_GamepadRStickRight, "Key_GamepadRStickUp", ImGuiKey_GamepadRStickUp, "Key_GamepadRStickDown",
        ImGuiKey_GamepadRStickDown, "Key_MouseLeft", ImGuiKey_MouseLeft, "Key_MouseRight", ImGuiKey_MouseRight, "Key_MouseMiddle",
        ImGuiKey_MouseMiddle, "Key_MouseX1", ImGuiKey_MouseX1, "Key_MouseX2", ImGuiKey_MouseX2, "Key_MouseWheelX", ImGuiKey_MouseWheelX,
        "Key_MouseWheelY", ImGuiKey_MouseWheelY,

        // Modifiers
        "Mod_None", ImGuiMod_None, "Mod_Ctrl", ImGuiMod_Ctrl, "Mod_Shift", ImGuiMod_Shift, "Mod_Alt", ImGuiMod_Alt, "Mod_Super",
        ImGuiMod_Super, "Mod_Mask_", ImGuiMod_Mask_);

    imgui.new_enum("ImGuiStyleVar", "Alpha", ImGuiStyleVar_Alpha, "DisabledAlpha", ImGuiStyleVar_DisabledAlpha, "WindowPadding",
        ImGuiStyleVar_WindowPadding, "WindowRounding", ImGuiStyleVar_WindowRounding, "WindowBorderSize", ImGuiStyleVar_WindowBorderSize,
        "WindowMinSize", ImGuiStyleVar_WindowMinSize, "WindowTitleAlign", ImGuiStyleVar_WindowTitleAlign, "ChildRounding",
        ImGuiStyleVar_ChildRounding, "ChildBorderSize", ImGuiStyleVar_ChildBorderSize, "PopupRounding", ImGuiStyleVar_PopupRounding,
        "PopupBorderSize", ImGuiStyleVar_PopupBorderSize, "FramePadding", ImGuiStyleVar_FramePadding, "FrameRounding",
        ImGuiStyleVar_FrameRounding, "FrameBorderSize", ImGuiStyleVar_FrameBorderSize, "ItemSpacing", ImGuiStyleVar_ItemSpacing,
        "ItemInnerSpacing", ImGuiStyleVar_ItemInnerSpacing, "IndentSpacing", ImGuiStyleVar_IndentSpacing, "CellPadding",
        ImGuiStyleVar_CellPadding, "ScrollbarSize", ImGuiStyleVar_ScrollbarSize, "ScrollbarRounding", ImGuiStyleVar_ScrollbarRounding,
        "GrabMinSize", ImGuiStyleVar_GrabMinSize, "GrabRounding", ImGuiStyleVar_GrabRounding, "TabRounding", ImGuiStyleVar_TabRounding,
        "ButtonTextAlign", ImGuiStyleVar_ButtonTextAlign, "SelectableTextAlign", ImGuiStyleVar_SelectableTextAlign,
        "SeparatorTextBorderSize", ImGuiStyleVar_SeparatorTextBorderSize, "SeparatorTextAlign", ImGuiStyleVar_SeparatorTextAlign,
        "SeparatorTextPadding", ImGuiStyleVar_SeparatorTextPadding, "COUNT", ImGuiStyleVar_COUNT);

    imgui.new_usertype<ImDrawList>(
        "ImDrawList", "push_clip_rect",
        [](ImDrawList* draw_list, sol::object min, sol::object max, bool intersect_with_current_clip_rect) {
            draw_list->PushClipRect(::api::imgui::create_imvec2(min), ::api::imgui::create_imvec2(max), intersect_with_current_clip_rect);
        },
        "push_clip_rect_fullscreen", &ImDrawList::PushClipRectFullScreen, "pop_clip_rect", &ImDrawList::PopClipRect,

        "add_line",
        [](ImDrawList* draw_list, sol::object p1, sol::object p2, uint32_t col, float thickness) {
            draw_list->AddLine(::api::imgui::create_imvec2(p1), ::api::imgui::create_imvec2(p2), col, thickness);
        },
        "add_rect",
        [](ImDrawList* draw_list, sol::object min, sol::object max, uint32_t col, float rounding, ImDrawFlags flags, float thickness) {
            draw_list->AddRect(::api::imgui::create_imvec2(min), ::api::imgui::create_imvec2(max), col, rounding, flags, thickness);
        },
        "add_rect_filled",
        [](ImDrawList* draw_list, sol::object min, sol::object max, uint32_t col, float rounding, ImDrawFlags flags) {
            draw_list->AddRectFilled(::api::imgui::create_imvec2(min), ::api::imgui::create_imvec2(max), col, rounding, flags);
        },
        "add_rect_filled_multi_color",
        [](ImDrawList* draw_list, sol::object min, sol::object max, uint32_t col_upr_left, uint32_t col_upr_right, uint32_t col_bot_right,
            uint32_t col_bot_left) {
            draw_list->AddRectFilledMultiColor(::api::imgui::create_imvec2(min), ::api::imgui::create_imvec2(max), col_upr_left,
                col_upr_right, col_bot_right, col_bot_left);
        },
        "add_quad",
        [](ImDrawList* draw_list, sol::object p1, sol::object p2, sol::object p3, sol::object p4, uint32_t col, float thickness) {
            draw_list->AddQuad(::api::imgui::create_imvec2(p1), ::api::imgui::create_imvec2(p2), ::api::imgui::create_imvec2(p3),
                ::api::imgui::create_imvec2(p4), col, thickness);
        },
        "add_quad_filled",
        [](ImDrawList* draw_list, sol::object p1, sol::object p2, sol::object p3, sol::object p4, uint32_t col) {
            draw_list->AddQuadFilled(::api::imgui::create_imvec2(p1), ::api::imgui::create_imvec2(p2), ::api::imgui::create_imvec2(p3),
                ::api::imgui::create_imvec2(p4), col);
        },
        "add_triangle",
        [](ImDrawList* draw_list, sol::object p1, sol::object p2, sol::object p3, uint32_t col, float thickness) {
            draw_list->AddTriangle(
                ::api::imgui::create_imvec2(p1), ::api::imgui::create_imvec2(p2), ::api::imgui::create_imvec2(p3), col, thickness);
        },
        "add_triangle_filled",
        [](ImDrawList* draw_list, sol::object p1, sol::object p2, sol::object p3, uint32_t col) {
            draw_list->AddTriangleFilled(
                ::api::imgui::create_imvec2(p1), ::api::imgui::create_imvec2(p2), ::api::imgui::create_imvec2(p3), col);
        },
        "add_circle",
        [](ImDrawList* draw_list, sol::object center, float radius, uint32_t col, int num_segments, float thickness) {
            draw_list->AddCircle(::api::imgui::create_imvec2(center), radius, col, num_segments, thickness);
        },
        "add_circle_filled",
        [](ImDrawList* draw_list, sol::object center, float radius, uint32_t col, int num_segments) {
            draw_list->AddCircleFilled(::api::imgui::create_imvec2(center), radius, col, num_segments);
        },
        "add_ngon",
        [](ImDrawList* draw_list, sol::object center, float radius, uint32_t col, int num_segments, float thickness) {
            draw_list->AddNgon(::api::imgui::create_imvec2(center), radius, col, num_segments, thickness);
        },
        "add_ngon_filled",
        [](ImDrawList* draw_list, sol::object center, float radius, uint32_t col, int num_segments) {
            draw_list->AddNgonFilled(::api::imgui::create_imvec2(center), radius, col, num_segments);
        },
        "add_text",
        [](ImDrawList* draw_list, sol::object pos, uint32_t col, const std::string& text) {
            draw_list->AddText(::api::imgui::create_imvec2(pos), col, text.c_str());
        },
        "add_bezier_cubic",
        [](ImDrawList* draw_list, sol::object p1, sol::object p2, sol::object p3, sol::object p4, uint32_t col, float thickness) {
            draw_list->AddBezierCubic(::api::imgui::create_imvec2(p1), ::api::imgui::create_imvec2(p2), ::api::imgui::create_imvec2(p3),
                ::api::imgui::create_imvec2(p4), col, thickness);
        },
        "add_bezier_quadratic",
        [](ImDrawList* draw_list, sol::object p1, sol::object p2, sol::object p3, uint32_t col, int num_segments) {
            draw_list->AddBezierQuadratic(
                ::api::imgui::create_imvec2(p1), ::api::imgui::create_imvec2(p2), ::api::imgui::create_imvec2(p3), col, num_segments);
        },

        // Path APIs
        "path_clear", &ImDrawList::PathClear, "path_line_to",
        [](ImDrawList* draw_list, sol::object pos) { draw_list->PathLineTo(::api::imgui::create_imvec2(pos)); },
        "path_line_to_merge_duplicate",
        [](ImDrawList* draw_list, sol::object pos) { draw_list->PathLineToMergeDuplicate(::api::imgui::create_imvec2(pos)); },
        "path_fill_convex", &ImDrawList::PathFillConvex, "path_stroke",
        [](ImDrawList* draw_list, uint32_t col, ImDrawFlags flags, float thickness) { draw_list->PathStroke(col, flags, thickness); },
        "path_arc_to",
        [](ImDrawList* draw_list, sol::object center, float radius, float a_min, float a_max, int num_segments) {
            draw_list->PathArcTo(::api::imgui::create_imvec2(center), radius, a_min, a_max, num_segments);
        },
        "path_arc_to_fast",
        [](ImDrawList* draw_list, sol::object center, float radius, int a_min_of_12, int a_max_of_12) {
            draw_list->PathArcToFast(::api::imgui::create_imvec2(center), radius, a_min_of_12, a_max_of_12);
        },
        "path_elliptical_arc_to",
        [](ImDrawList* draw_list, sol::object center, sol::object radius, float a_min, float a_max, int num_segments) {
            // Fallback to circular arc using radius.x
            auto r = ::api::imgui::create_imvec2(radius);
            draw_list->PathArcTo(::api::imgui::create_imvec2(center), r.x, a_min, a_max, num_segments);
        },
        "path_bezier_cubic_curve_to",
        [](ImDrawList* draw_list, sol::object p2, sol::object p3, sol::object p4, int num_segments) {
            draw_list->PathBezierCubicCurveTo(
                ::api::imgui::create_imvec2(p2), ::api::imgui::create_imvec2(p3), ::api::imgui::create_imvec2(p4), num_segments);
        },
        "path_bezier_quadratic_curve_to",
        [](ImDrawList* draw_list, sol::object p2, sol::object p3, int num_segments) {
            draw_list->PathBezierQuadraticCurveTo(::api::imgui::create_imvec2(p2), ::api::imgui::create_imvec2(p3), num_segments);
        },
        "path_rect",
        [](ImDrawList* draw_list, sol::object min, sol::object max, float rounding, ImDrawFlags flags) {
            draw_list->PathRect(::api::imgui::create_imvec2(min), ::api::imgui::create_imvec2(max), rounding, flags);
        });

    lua["imgui"] = imgui;
    //auto imguizmo = lua.create_table();

    //imguizmo["is_over"] = [] { return ImGuizmo::IsOver(); };
    //imguizmo["is_using"] = [] { return ImGuizmo::IsUsing(); };

    //lua["imguizmo"] = imguizmo;

    auto draw = lua.create_table();

    draw["text"] = api::draw::text;
    draw["filled_rect"] = api::draw::filled_rect;
    draw["outline_rect"] = api::draw::outline_rect;
    draw["line"] = api::draw::line;
    draw["outline_circle"] = api::draw::outline_circle;
    draw["filled_circle"] = api::draw::filled_circle;
    draw["outline_quad"] = api::draw::outline_quad;
    draw["filled_quad"] = api::draw::filled_quad;

    // draw["world_to_screen"] = api::draw::world_to_screen;
    // draw["world_text"] = api::draw::world_text;

    // draw["sphere"] = api::draw::sphere;
    // draw["capsule"] = api::draw::capsule;
    // draw["gizmo"] = api::draw::gizmo;
    // draw["cube"] = [](const Matrix4x4f& mat) { ::imgui::draw_cube(mat); };
    // draw["grid"] = [](const Matrix4x4f& mat, float size) { ::imgui::draw_grid(mat, size); };
    lua["draw"] = draw;
}
