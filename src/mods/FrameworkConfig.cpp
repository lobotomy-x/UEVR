#include "Framework.hpp"

#include "FrameworkConfig.hpp"

std::shared_ptr<FrameworkConfig>& FrameworkConfig::get() {
     static std::shared_ptr<FrameworkConfig> instance{std::make_shared<FrameworkConfig>()};
     return instance;
}

std::optional<std::string> FrameworkConfig::on_initialize() {
    return Mod::on_initialize();
}

void FrameworkConfig::draw_main() {
    m_menu_key->draw("Menu Key");
    m_show_cursor_key->draw("Show Cursor Key");
    m_remember_menu_state->draw("Remember Menu Open/Closed State");
    m_enable_l3_r3_toggle->draw("Enable L3 + R3 Toggle");
    ImGui::SameLine();
    m_l3_r3_long_press->draw("L3 + R3 Long Press Menu Toggle");
    m_always_show_cursor->draw("Always Show Cursor");
    
    ImGui::Separator();
    if (m_log_level->draw("Log Level")) {
        if (m_log_level->value() >= 0 && m_log_level->value() <= spdlog::level::level_enum::n_levels) {
            spdlog::set_level((spdlog::level::level_enum)m_log_level->value());
        }
    }

    ImGui::Separator();
    // Multiviewport toggle is persisted via ModToggle (lives in config.txt).
    // Disabled by default — when off, ImGui windows are clamped to the game
    // window the same way stock ImGui (single-viewport mode) behaves. Enabling
    // it lets users drag panels out into their own OS windows.
    m_use_multiviewport->draw("Enable ImGui Multi-Viewport (popped-out windows)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("OFF (default): every ImGui window stays inside the game window.\n"
                          "ON: dragging a window outside the game converts it to a top-level OS popup.\n"
                          "Safe to toggle at runtime — ImGui re-absorbs popups on the next frame.");
    }
    // Runtime-only metrics window toggle — not persisted. Sits right beside
    // the multiviewport toggle because this is the developer-facing block.
    if (g_framework != nullptr) {
        ImGui::Checkbox("Show ImGui Metrics / Debugger Window", &g_framework->m_show_imgui_metrics);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Opens ImGui::ShowMetricsWindow — frame stats, window stack inspector,\n"
                              "active draw calls, dock node tree, etc. Useful for diagnosing\n"
                              "missing End()/TreePop() and other ImGui-state issues.");
        }
    }
}

void FrameworkConfig::draw_themes() {
    get_imgui_theme()->draw("Select GUI Theme");

    if (m_font_size->draw("Font Size")) {
        g_framework->set_font_size(m_font_size->value());
    }
}

void FrameworkConfig::on_draw_sidebar_entry(std::string_view in_entry) {
    on_draw_ui();
    ImGui::Separator();

    if (in_entry == "Main") {
        draw_main();
    } else if (in_entry == "GUI/Themes") {
        draw_themes();
    }
}

void FrameworkConfig::on_frame() {
    if (m_show_cursor_key->is_key_down_once()) {
        m_always_show_cursor->toggle();
    }
}

void FrameworkConfig::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    if (m_remember_menu_state->value()) {
        g_framework->set_draw_ui(m_menu_open->value(), false);
    }
    
    g_framework->set_font_size(m_font_size->value());

    if (m_log_level->value() >= 0 && m_log_level->value() <= spdlog::level::level_enum::n_levels) {
        spdlog::set_level((spdlog::level::level_enum)m_log_level->value());   
    }
}

void FrameworkConfig::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}
