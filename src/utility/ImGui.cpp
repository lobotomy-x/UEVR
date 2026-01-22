#include <imgui.h>
#include <imgui_internal.h>
 #include <sdk/APlayerController.hpp>
#include <sdk/UGameplayStatics.hpp>
   #include <glm/fwd.hpp>
#include <Framework.hpp>
#include <mods/UObjectHook.hpp>
#include <mods/VR.hpp>

#include "ImGui.hpp"

namespace imgui {
bool is_point_intersecting_any(float x, float y) {
    const auto ctx = ImGui::GetCurrentContext();

    if (ctx == nullptr) {
        return false;
    }

    for (int i = 0; i < ctx->Windows.Size; i++) {
        const auto window = ctx->Windows[i];

        if (window->WasActive && window->Active) {
            if (x >= window->Pos.x && x <= window->Pos.x + window->Size.x &&
                y >= window->Pos.y && y <= window->Pos.y + window->Size.y)
            {
                return true;
            }
        }
    }

    return false;
}




void imgui::draw_sphere(const Vector3f& camera_up, const Vector2f& screen_pos_center, const Vector3f& center, float radius, ImU32 color, bool outline) {
    static auto engine = sdk::UEngine::get();
    static auto world = engine->get_world();
    static auto player_controller = sdk::UGameplayStatics::get_player_controller(world, 0);
    if (screen_pos_center) {
        const auto pos_top = center + (glm::normalize(*camera_up) * radius);
        const auto screen_pos_top = sdk::UGameplayStatics::world_to_screen(player_controller, pos_top);

        if (screen_pos_top) {
            const auto radius2d = glm::length(*screen_pos_top - *screen_pos_center);

            // Inner
            ImGui::GetBackgroundDrawList()->AddCircleFilled(*(ImVec2*)&*screen_pos_center, radius2d, color, 32);

            // Outline
            if (outline) {
                ImGui::GetBackgroundDrawList()->AddCircle(
                    *(ImVec2*)&*screen_pos_center, radius2d, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 1.0f)), 32);
            }
        }
    }
}

void imgui::draw_capsule(const Vector3f& camera_up, const Vector2f& screen_pos_center, const Vector3f& start, const Vector3f& end, float radius,
    ImU32 color, bool outline) {
    static auto engine = sdk::UEngine::get();
    static auto world = engine->get_world();
    static auto player_controller = sdk::UGameplayStatics::get_player_controller(world, 0);

    auto get_screen_radius = [&](const Vector3f& pos, float radius) -> std::optional<std::tuple<float, Vector2f>> {

        if (screen_pos_center) {
            const auto pos_top = pos + (glm::normalize(*camera_up) * radius);
            const auto screen_pos_top = sdk::UGameplayStatics::world_to_screen(player_controller, pos_top);

            if (screen_pos_top) {
                const auto radius2d = glm::length(*screen_pos_top - *screen_pos_center);

                return std::make_tuple(radius2d, *screen_pos_center);
            }
        }

        return std::nullopt;
    };

    // Capsule
    draw_sphere(start, radius, color, true);
    draw_sphere(end, radius, color, true);

    // Get the half points of the circles and draw a filled rectangle between them
    const auto top_screen_radius = get_screen_radius(start, radius);
    const auto bottom_screen_radius = get_screen_radius(end, radius);

    if (top_screen_radius && bottom_screen_radius) {
        const auto top_radius = std::get<0>(*top_screen_radius);
        const auto bottom_radius = std::get<0>(*bottom_screen_radius);

        const auto top_circle_start = std::get<1>(*top_screen_radius);
        const auto bottom_circle_start = std::get<1>(*bottom_screen_radius);

        const auto delta = glm::normalize(bottom_circle_start - top_circle_start);
        const auto angle = glm::atan(delta.y, delta.x) + glm::radians(90.0f);

        // Now get the halfway point(s) of the circles
        float top_x_right = top_circle_start.x + (top_radius * std::cos(angle));
        float top_y_right = top_circle_start.y + (top_radius * std::sin(angle));

        float bottom_x_right = bottom_circle_start.x + (bottom_radius * std::cos(angle));
        float bottom_y_right = bottom_circle_start.y + (bottom_radius * std::sin(angle));

        float top_x_left = top_circle_start.x + (top_radius * std::cos(angle + glm::radians(180.0f)));
        float top_y_left = top_circle_start.y + (top_radius * std::sin(angle + glm::radians(180.0f)));

        float bottom_x_left = bottom_circle_start.x + (bottom_radius * std::cos(angle + glm::radians(180.0f)));
        float bottom_y_left = bottom_circle_start.y + (bottom_radius * std::sin(angle + glm::radians(180.0f)));

        // Draw a quad
        ImGui::GetBackgroundDrawList()->AddQuadFilled(ImVec2(top_x_left, top_y_left), ImVec2(bottom_x_left, bottom_y_left),
            ImVec2(bottom_x_right, bottom_y_right), ImVec2(top_x_right, top_y_right), color);

        if (outline) {
            ImGui::GetBackgroundDrawList()->AddQuad(ImVec2(top_x_left, top_y_left), ImVec2(bottom_x_left, bottom_y_left),
                ImVec2(bottom_x_right, bottom_y_right), ImVec2(top_x_right, top_y_right),
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 1.0f)));
        }
    }
}

}