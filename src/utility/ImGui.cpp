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




void imgui::draw_sphere(const Vector3f& /*camera_up*/, const Vector2f& screen_pos_center, const Vector3f& /*center*/, float radius, ImU32 color, bool outline) {
    // Simplified: draw a circle at provided 2D screen position using radius as pixel radius.
    ImVec2 pos(screen_pos_center.x, screen_pos_center.y);
    ImGui::GetBackgroundDrawList()->AddCircleFilled(pos, radius, color, 32);
    if (outline) {
        ImGui::GetBackgroundDrawList()->AddCircle(pos, radius, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 1.0f)), 32);
    }
}

void imgui::draw_capsule(const Vector3f& /*camera_up*/, const Vector2f& screen_pos_center, const Vector3f& /*start*/, const Vector3f& /*end*/, float radius,
    ImU32 color, bool outline) {
    // Simplified capsule: draw two circles offset vertically and a quad between them using screen_pos_center as center.
    ImVec2 center(screen_pos_center.x, screen_pos_center.y);
    ImVec2 top(center.x, center.y - radius);
    ImVec2 bottom(center.x, center.y + radius);

    ImGui::GetBackgroundDrawList()->AddCircleFilled(top, radius, color, 32);
    ImGui::GetBackgroundDrawList()->AddCircleFilled(bottom, radius, color, 32);
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(top.x - radius, top.y), ImVec2(bottom.x + radius, bottom.y), color);

    if (outline) {
        ImGui::GetBackgroundDrawList()->AddCircle(top, radius, ImGui::GetColorU32(ImVec4(0,0,0,1)), 32);
        ImGui::GetBackgroundDrawList()->AddCircle(bottom, radius, ImGui::GetColorU32(ImVec4(0,0,0,1)), 32);
        ImGui::GetBackgroundDrawList()->AddRect(ImVec2(top.x - radius, top.y), ImVec2(bottom.x + radius, bottom.y), ImGui::GetColorU32(ImVec4(0,0,0,1)));
    }
}

}