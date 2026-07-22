#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "Mod.hpp"

class VR;

namespace vrmod {

// Bindable VR controller sources. Deliberately a closed enum (not raw OpenVR action handles) so
// bindings serialize as stable strings, not process-specific handle values. Maps onto VR's EXISTING
// action handles (VR.hpp's m_action_* members) — this does not define any new OpenVR actions, it just
// lets user bindings read the same digital/analog state VR::on_xinput_get_state already reads.
enum class EmuSource : uint8_t {
    None = 0,
    TriggerLeft, TriggerRight,
    GripLeft, GripRight,
    AButtonLeft, AButtonRight,           // primary face button (X/A on Index/Touch controllers)
    BButtonLeft, BButtonRight,           // secondary face button (Y/B)
    JoystickClickLeft, JoystickClickRight,
    DpadUp, DpadRight, DpadDown, DpadLeft,
    SystemButton,
    ThumbrestTouchLeft, ThumbrestTouchRight,
    JoystickAxisLeftX, JoystickAxisLeftY,     // analog, -1..1
    JoystickAxisRightX, JoystickAxisRightY,   // analog, -1..1
    Count
};

enum class EmuOutputKind : uint8_t {
    None = 0,
    XInputButton,   // code = one of the XINPUT_GAMEPAD_* bitmask constants
    XInputAxis,     // code = 0:LeftX 1:LeftY 2:RightX 3:RightY 4:LeftTrigger 5:RightTrigger
    Keyboard,       // code = a Win32 VK_* code
    MouseButton,    // code = 0:Left 1:Right 2:Middle 3:X1 4:X2
    MouseAxisX,     // relative mouse movement, horizontal
    MouseAxisY,     // relative mouse movement, vertical
    Count
};

std::string_view emu_source_to_string(EmuSource s);
EmuSource emu_source_from_string(std::string_view s);
std::string_view emu_output_kind_to_string(EmuOutputKind k);
EmuOutputKind emu_output_kind_from_string(std::string_view s);

struct EmuBinding {
    EmuSource source{EmuSource::None};
    EmuOutputKind kind{EmuOutputKind::None};
    int32_t code{0};
    float scale{1.0f};      // axis outputs: output-per-unit-input multiplier (mouse look sensitivity,
                             // or XInput axis direction when negative); digital outputs: unused
    bool invert{false};     // axis sources only: flip sign before scale
};

// VR controller -> XInput/keyboard/mouse emulation, on top of what VR::on_xinput_get_state already
// hardcodes. User bindings are ADDITIVE to the existing fixed A/B/trigger/grip/dpad -> XInput mapping
// (that mapping stays as-is — rewriting VR's core, already-shipped, headset-sensitive gamepad spoof to
// be fully data-driven is a much larger, riskier change than this pass intends). What this DOES let you
// rebind: any source not already claimed by the fixed mapping (dpad diagonals aren't a thing here, but
// e.g. ThumbrestTouch/SystemButton/JoystickClick are otherwise unused) to an XInput button/axis, AND —
// new capability — any source at all to a synthesized keyboard key or mouse button/movement, for games
// with no gamepad support.
class InputEmulation : public ModComponent {
public:
    std::string_view get_name() const override { return "InputEmulation"; }

    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;
    void on_draw_ui() override;

    // Called once per frame (VR::on_frame(this)). Reads each binding's source, tracks edge transitions
    // for digital outputs (so keyboard/mouse-button presses are exactly one down + one up, not spammed
    // every frame), and synthesizes keyboard/mouse output via SendInput. Does NOT touch XInput state —
    // that's applied separately (see apply_to_xinput) since it has to run inside
    // VR::on_xinput_get_state, not the mod's own on_frame.
    void on_frame(VR& vr);

    // Called from VR::on_xinput_get_state, after the existing hardcoded mapping has already written to
    // `pad`. ORs in additional button bits / adds to axes for any binding targeting an XInput output.
    void apply_to_xinput(VR& vr, XINPUT_GAMEPAD& pad) const;

    // Lua-facing direct injection (uevr.api_fast.input_*) — not tied to a VR source at all, for scripts
    // that want to synthesize input themselves.
    static void inject_key(int32_t vk, bool down);
    static void inject_mouse_button(int32_t button, bool down);
    static void inject_mouse_move(int32_t dx, int32_t dy);

    const std::vector<EmuBinding>& get_bindings() const { return m_bindings; }
    void add_binding(EmuBinding b) { m_bindings.push_back(b); m_dirty = true; }
    void remove_binding(size_t index);
    void clear_bindings() { m_bindings.clear(); m_dirty = true; }

private:
    std::filesystem::path bindings_file_path() const;
    void load_bindings();
    void save_bindings() const;

    // Current analog/digital reading for a source, in [-1, 1] (digital sources read 0 or 1).
    static float read_source(VR& vr, EmuSource source);

    std::vector<EmuBinding> m_bindings{};
    bool m_dirty{false}; // set on any add/remove; on_frame flushes to disk at most once per change, not every frame

    // Digital-output edge tracking, keyed by binding index — so a binding whose *position* in the list
    // changes (add/remove elsewhere) doesn't inherit another binding's held-state. Cleared whenever the
    // list itself changes.
    std::vector<bool> m_held{};

    // UI-only capture state ("click to bind a source" — press a VR button, this becomes that binding's
    // source; separate from ModKey's keyboard capture since the source vocabulary here is fixed/VR-only).
    int m_capture_target_index{-1};
};

} // namespace vrmod
