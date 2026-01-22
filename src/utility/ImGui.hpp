#pragma once
 #include <glm/fwd.hpp>
#include <Framework.hpp>
#include <mods/UObjectHook.hpp>
#include <mods/VR.hpp>
namespace imgui {
bool is_point_intersecting_any(float x, float y);
void draw_sphere(const Vector3f& camera_up, const Vector2f& screen_pos_center, const Vector3f& center, float radius, ImU32 color, bool outline);
void draw_capsule(const Vector3f& camera_up, const Vector2f& screen_pos_center, const Vector3f& start, const Vector3f& end, float radius,
    ImU32 color, bool outline);




}