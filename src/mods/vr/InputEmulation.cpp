#include <algorithm>
#include <cstdio>
#include <fstream>

#include <windows.h>

#include <nlohmann/json.hpp>
#include <imgui.h>

#include <utility/String.hpp>

#include "../Framework.hpp"
#include "../VR.hpp"

#include "InputEmulation.hpp"

namespace vrmod {

// clang-format off
static constexpr std::pair<EmuSource, std::string_view> kSourceNames[] = {
    {EmuSource::None, "None"},
    {EmuSource::TriggerLeft, "TriggerLeft"}, {EmuSource::TriggerRight, "TriggerRight"},
    {EmuSource::GripLeft, "GripLeft"}, {EmuSource::GripRight, "GripRight"},
    {EmuSource::AButtonLeft, "AButtonLeft"}, {EmuSource::AButtonRight, "AButtonRight"},
    {EmuSource::BButtonLeft, "BButtonLeft"}, {EmuSource::BButtonRight, "BButtonRight"},
    {EmuSource::JoystickClickLeft, "JoystickClickLeft"}, {EmuSource::JoystickClickRight, "JoystickClickRight"},
    {EmuSource::DpadUp, "DpadUp"}, {EmuSource::DpadRight, "DpadRight"}, {EmuSource::DpadDown, "DpadDown"}, {EmuSource::DpadLeft, "DpadLeft"},
    {EmuSource::SystemButton, "SystemButton"},
    {EmuSource::ThumbrestTouchLeft, "ThumbrestTouchLeft"}, {EmuSource::ThumbrestTouchRight, "ThumbrestTouchRight"},
    {EmuSource::JoystickAxisLeftX, "JoystickAxisLeftX"}, {EmuSource::JoystickAxisLeftY, "JoystickAxisLeftY"},
    {EmuSource::JoystickAxisRightX, "JoystickAxisRightX"}, {EmuSource::JoystickAxisRightY, "JoystickAxisRightY"},
};

static constexpr std::pair<EmuOutputKind, std::string_view> kOutputKindNames[] = {
    {EmuOutputKind::None, "None"},
    {EmuOutputKind::XInputButton, "XInputButton"},
    {EmuOutputKind::XInputAxis, "XInputAxis"},
    {EmuOutputKind::Keyboard, "Keyboard"},
    {EmuOutputKind::MouseButton, "MouseButton"},
    {EmuOutputKind::MouseAxisX, "MouseAxisX"},
    {EmuOutputKind::MouseAxisY, "MouseAxisY"},
};
// clang-format on

std::string_view emu_source_to_string(EmuSource s) {
    for (const auto& [k, v] : kSourceNames) if (k == s) return v;
    return "None";
}
EmuSource emu_source_from_string(std::string_view s) {
    for (const auto& [k, v] : kSourceNames) if (v == s) return k;
    return EmuSource::None;
}
std::string_view emu_output_kind_to_string(EmuOutputKind k) {
    for (const auto& [kk, v] : kOutputKindNames) if (kk == k) return v;
    return "None";
}
EmuOutputKind emu_output_kind_from_string(std::string_view s) {
    for (const auto& [kk, v] : kOutputKindNames) if (v == s) return kk;
    return EmuOutputKind::None;
}

// ---------------------------------------------------------------------------------------------
// Source reading — dispatches onto VR's EXISTING action handles/helpers (VR.hpp friend-granted
// access), so this never touches OpenVR directly and never diverges from what
// VR::on_xinput_get_state itself considers "the trigger" etc.
// ---------------------------------------------------------------------------------------------
float InputEmulation::read_source(VR& vr, EmuSource source) {
    const auto left = vr.get_left_joystick();
    const auto right = vr.get_right_joystick();

    switch (source) {
    case EmuSource::TriggerLeft:  return vr.is_action_active(vr.m_action_trigger, left) ? 1.0f : 0.0f;
    case EmuSource::TriggerRight: return vr.is_action_active(vr.m_action_trigger, right) ? 1.0f : 0.0f;
    case EmuSource::GripLeft:     return vr.is_action_active(vr.m_action_grip, left) ? 1.0f : 0.0f;
    case EmuSource::GripRight:    return vr.is_action_active(vr.m_action_grip, right) ? 1.0f : 0.0f;
    case EmuSource::AButtonLeft:  return vr.is_action_active(vr.m_action_a_button_left, left) ? 1.0f : 0.0f;
    case EmuSource::AButtonRight: return vr.is_action_active(vr.m_action_a_button_right, right) ? 1.0f : 0.0f;
    case EmuSource::BButtonLeft:  return vr.is_action_active(vr.m_action_b_button_left, left) ? 1.0f : 0.0f;
    case EmuSource::BButtonRight: return vr.is_action_active(vr.m_action_b_button_right, right) ? 1.0f : 0.0f;
    case EmuSource::JoystickClickLeft:  return vr.is_action_active(vr.m_action_joystick_click, left) ? 1.0f : 0.0f;
    case EmuSource::JoystickClickRight: return vr.is_action_active(vr.m_action_joystick_click, right) ? 1.0f : 0.0f;
    case EmuSource::DpadUp:    return vr.is_action_active_any_joystick(vr.m_action_dpad_up) ? 1.0f : 0.0f;
    case EmuSource::DpadRight: return vr.is_action_active_any_joystick(vr.m_action_dpad_right) ? 1.0f : 0.0f;
    case EmuSource::DpadDown:  return vr.is_action_active_any_joystick(vr.m_action_dpad_down) ? 1.0f : 0.0f;
    case EmuSource::DpadLeft:  return vr.is_action_active_any_joystick(vr.m_action_dpad_left) ? 1.0f : 0.0f;
    case EmuSource::SystemButton: return vr.is_action_active_any_joystick(vr.m_action_system_button) ? 1.0f : 0.0f;
    case EmuSource::ThumbrestTouchLeft:  return vr.is_action_active(vr.m_action_thumbrest_touch_left, left) ? 1.0f : 0.0f;
    case EmuSource::ThumbrestTouchRight: return vr.is_action_active(vr.m_action_thumbrest_touch_right, right) ? 1.0f : 0.0f;
    case EmuSource::JoystickAxisLeftX:  return vr.get_joystick_axis(left).x;
    case EmuSource::JoystickAxisLeftY:  return vr.get_joystick_axis(left).y;
    case EmuSource::JoystickAxisRightX: return vr.get_joystick_axis(right).x;
    case EmuSource::JoystickAxisRightY: return vr.get_joystick_axis(right).y;
    default: return 0.0f;
    }
}

// ---------------------------------------------------------------------------------------------
// XInput application — additive only (see the class comment in InputEmulation.hpp for why).
// ---------------------------------------------------------------------------------------------
void InputEmulation::apply_to_xinput(VR& vr, XINPUT_GAMEPAD& pad) const {
    for (const auto& b : m_bindings) {
        if (b.source == EmuSource::None) continue;
        const float value = read_source(vr, b.source);

        switch (b.kind) {
        case EmuOutputKind::XInputButton:
            if (value > 0.5f) {
                pad.wButtons |= (WORD)b.code;
            }
            break;
        case EmuOutputKind::XInputAxis: {
            const float signed_value = (b.invert ? -value : value) * b.scale;
            switch (b.code) {
            case 0: pad.sThumbLX = (int16_t)std::clamp<float>((float)pad.sThumbLX + signed_value * 32767.0f, -32767.0f, 32767.0f); break;
            case 1: pad.sThumbLY = (int16_t)std::clamp<float>((float)pad.sThumbLY + signed_value * 32767.0f, -32767.0f, 32767.0f); break;
            case 2: pad.sThumbRX = (int16_t)std::clamp<float>((float)pad.sThumbRX + signed_value * 32767.0f, -32767.0f, 32767.0f); break;
            case 3: pad.sThumbRY = (int16_t)std::clamp<float>((float)pad.sThumbRY + signed_value * 32767.0f, -32767.0f, 32767.0f); break;
            case 4: pad.bLeftTrigger = (BYTE)std::clamp<float>((float)pad.bLeftTrigger + std::max(0.0f, signed_value) * 255.0f, 0.0f, 255.0f); break;
            case 5: pad.bRightTrigger = (BYTE)std::clamp<float>((float)pad.bRightTrigger + std::max(0.0f, signed_value) * 255.0f, 0.0f, 255.0f); break;
            default: break;
            }
            break;
        }
        default:
            break; // Keyboard/Mouse/* handled in on_frame, not here
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Per-frame keyboard/mouse synthesis.
// ---------------------------------------------------------------------------------------------
void InputEmulation::on_frame(VR& vr) {
    if (m_dirty) {
        save_bindings();
        m_dirty = false;
    }

    if (m_held.size() != m_bindings.size()) {
        m_held.assign(m_bindings.size(), false);
    }

    for (size_t i = 0; i < m_bindings.size(); ++i) {
        const auto& b = m_bindings[i];
        if (b.source == EmuSource::None || b.kind == EmuOutputKind::None) continue;

        const float raw = read_source(vr, b.source);

        switch (b.kind) {
        case EmuOutputKind::Keyboard: {
            const bool active = raw > 0.5f;
            if (active != m_held[i]) {
                inject_key(b.code, active);
                m_held[i] = active;
            }
            break;
        }
        case EmuOutputKind::MouseButton: {
            const bool active = raw > 0.5f;
            if (active != m_held[i]) {
                inject_mouse_button(b.code, active);
                m_held[i] = active;
            }
            break;
        }
        case EmuOutputKind::MouseAxisX: {
            const float v = (b.invert ? -raw : raw) * b.scale;
            if (v != 0.0f) {
                inject_mouse_move((int32_t)v, 0);
            }
            break;
        }
        case EmuOutputKind::MouseAxisY: {
            const float v = (b.invert ? -raw : raw) * b.scale;
            if (v != 0.0f) {
                inject_mouse_move(0, (int32_t)v);
            }
            break;
        }
        default:
            break; // XInput* handled in apply_to_xinput, not here
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Direct injection — SendInput, same mechanism any joystick-to-keyboard mapping tool uses. Runs on
// whatever thread calls it (on_frame is the game/mod thread; the Lua-facing statics below can be
// called from a Lua callback on the same thread) — SendInput itself is thread-safe / OS-level.
// ---------------------------------------------------------------------------------------------
void InputEmulation::inject_key(int32_t vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = (WORD)vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputEmulation::inject_mouse_button(int32_t button, bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    switch (button) {
    case 0: input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case 1: input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case 2: input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case 3: input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; input.mi.mouseData = XBUTTON1; break;
    case 4: input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; input.mi.mouseData = XBUTTON2; break;
    default: return;
    }
    SendInput(1, &input, sizeof(INPUT));
}

void InputEmulation::inject_mouse_move(int32_t dx, int32_t dy) {
    if (dx == 0 && dy == 0) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

// ---------------------------------------------------------------------------------------------
// Persistence — a dedicated JSON file (variable-length list of structs, not a great fit for the
// scalar-per-key utility::Config/ModValue system the rest of the mod's settings use), matching the
// established pattern for this class of data elsewhere in this codebase (e.g. UObjectHook's
// PersistentCameraState / PersistentProperties files).
// ---------------------------------------------------------------------------------------------
std::filesystem::path InputEmulation::bindings_file_path() const {
    return Framework::get_persistent_dir("input_emulation.json");
}

void InputEmulation::load_bindings() {
    try {
        const auto path = bindings_file_path();
        if (!std::filesystem::exists(path)) {
            return;
        }
        std::ifstream f{path};
        nlohmann::json j{};
        f >> j;
        if (!j.is_array()) {
            return;
        }
        m_bindings.clear();
        for (const auto& e : j) {
            EmuBinding b{};
            b.source = emu_source_from_string(e.value("source", "None"));
            b.kind = emu_output_kind_from_string(e.value("kind", "None"));
            b.code = e.value("code", 0);
            b.scale = e.value("scale", 1.0f);
            b.invert = e.value("invert", false);
            m_bindings.push_back(b);
        }
        m_held.clear();
        spdlog::info("[InputEmulation] Loaded {} binding(s) from {}", m_bindings.size(), path.string());
    } catch (const std::exception& e) {
        spdlog::error("[InputEmulation] Failed to load bindings: {}", e.what());
    } catch (...) {
        spdlog::error("[InputEmulation] Failed to load bindings (unknown error)");
    }
}

void InputEmulation::save_bindings() const {
    try {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& b : m_bindings) {
            j.push_back({
                {"source", emu_source_to_string(b.source)},
                {"kind", emu_output_kind_to_string(b.kind)},
                {"code", b.code},
                {"scale", b.scale},
                {"invert", b.invert},
            });
        }
        std::ofstream f{bindings_file_path()};
        f << j.dump(2);
    } catch (const std::exception& e) {
        spdlog::error("[InputEmulation] Failed to save bindings: {}", e.what());
    } catch (...) {
        spdlog::error("[InputEmulation] Failed to save bindings (unknown error)");
    }
}

void InputEmulation::remove_binding(size_t index) {
    if (index >= m_bindings.size()) return;
    m_bindings.erase(m_bindings.begin() + (ptrdiff_t)index);
    m_dirty = true;
    m_held.clear(); // indices shifted; re-sized fresh (all-released) next on_frame
}

void InputEmulation::on_config_load(const utility::Config&, bool) {
    load_bindings();
}

void InputEmulation::on_config_save(utility::Config&) {
    // Bindings are saved on their own schedule (m_dirty, flushed from on_frame) rather than tied to
    // the host mod's config-save cadence, since add/remove/edit happen through direct UI actions, not
    // through the ModValue system on_config_save iterates.
    if (m_dirty) {
        save_bindings();
        m_dirty = false;
    }
}

// ---------------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------------
void InputEmulation::on_draw_ui() {
    ImGui::TextWrapped(
        "Bind any VR controller button/axis to a synthesized XInput button/axis, keyboard key, or "
        "mouse button/movement. XInput bindings are ADDITIVE on top of the normal controller-as-"
        "gamepad mapping (they don't replace it) — use this for sources that mapping doesn't already "
        "use, or for keyboard/mouse-only games.");
    ImGui::Separator();

    if (ImGui::Button("Add binding")) {
        add_binding(EmuBinding{});
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_bindings.empty());
    if (ImGui::Button("Clear all")) {
        clear_bindings();
        m_held.clear();
    }
    ImGui::EndDisabled();

    for (size_t i = 0; i < m_bindings.size(); ++i) {
        ImGui::PushID((int)i);
        auto& b = m_bindings[i];

        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Source", emu_source_to_string(b.source).data())) {
            for (const auto& [k, v] : kSourceNames) {
                if (k == EmuSource::None) continue;
                if (ImGui::Selectable(v.data(), b.source == k)) {
                    b.source = k;
                    m_dirty = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::BeginCombo("Output", emu_output_kind_to_string(b.kind).data())) {
            for (const auto& [k, v] : kOutputKindNames) {
                if (k == EmuOutputKind::None) continue;
                if (ImGui::Selectable(v.data(), b.kind == k)) {
                    b.kind = k;
                    m_dirty = true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        switch (b.kind) {
        case EmuOutputKind::XInputButton: {
            static constexpr std::pair<const char*, int32_t> kButtons[] = {
                {"DPAD_UP", XINPUT_GAMEPAD_DPAD_UP}, {"DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN},
                {"DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT}, {"DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
                {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
                {"LEFT_THUMB", XINPUT_GAMEPAD_LEFT_THUMB}, {"RIGHT_THUMB", XINPUT_GAMEPAD_RIGHT_THUMB},
                {"LEFT_SHOULDER", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RIGHT_SHOULDER", XINPUT_GAMEPAD_RIGHT_SHOULDER},
                {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
            };
            const char* cur = "?";
            for (const auto& [name, code] : kButtons) if (code == b.code) cur = name;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::BeginCombo("Button", cur)) {
                for (const auto& [name, code] : kButtons) {
                    if (ImGui::Selectable(name, b.code == code)) { b.code = code; m_dirty = true; }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case EmuOutputKind::XInputAxis: {
            static constexpr const char* kAxes[] = {"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};
            const char* cur = (b.code >= 0 && b.code < 6) ? kAxes[b.code] : "?";
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::BeginCombo("Axis", cur)) {
                for (int a = 0; a < 6; ++a) {
                    if (ImGui::Selectable(kAxes[a], b.code == a)) { b.code = a; m_dirty = true; }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::DragFloat("Scale", &b.scale, 0.01f, -4.0f, 4.0f)) m_dirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Invert", &b.invert)) m_dirty = true;
            break;
        }
        case EmuOutputKind::Keyboard: {
            char buf[64];
            snprintf(buf, sizeof(buf), "VK 0x%02X", b.code);
            ImGui::SetNextItemWidth(90.0f);
            ImGui::TextUnformatted(buf);
            ImGui::SameLine();
            if (ImGui::Button(m_capture_target_index == (int)i ? "Press a key..." : "Set key")) {
                m_capture_target_index = (int)i;
            }
            if (m_capture_target_index == (int)i) {
                // Poll raw VK state directly (ImGui's VK<->ImGuiKey table doesn't cover every VK 1:1) —
                // same "poll every key each frame" approach ModKey::draw already uses for rebinding.
                for (int vk = 0x08; vk < 0xFF; ++vk) {
                    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                    if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
                        b.code = vk;
                        m_dirty = true;
                        m_capture_target_index = -1;
                        break;
                    }
                }
            }
            break;
        }
        case EmuOutputKind::MouseButton: {
            static constexpr const char* kMouseButtons[] = {"Left", "Right", "Middle", "X1", "X2"};
            const char* cur = (b.code >= 0 && b.code < 5) ? kMouseButtons[b.code] : "?";
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::BeginCombo("Button##mouse", cur)) {
                for (int m = 0; m < 5; ++m) {
                    if (ImGui::Selectable(kMouseButtons[m], b.code == m)) { b.code = m; m_dirty = true; }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case EmuOutputKind::MouseAxisX:
        case EmuOutputKind::MouseAxisY: {
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("px/frame", &b.scale, 1.0f, -500.0f, 500.0f)) m_dirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Invert##mouseaxis", &b.invert)) m_dirty = true;
            break;
        }
        default:
            break;
        }

        ImGui::SameLine();
        if (ImGui::Button("X")) {
            remove_binding(i);
            ImGui::PopID();
            break; // list mutated; resume next frame
        }

        ImGui::PopID();
    }
}

} // namespace vrmod
