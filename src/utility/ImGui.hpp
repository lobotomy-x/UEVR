#pragma once
 #include <glm/fwd.hpp>
#include <Framework.hpp>
#include <mods/UObjectHook.hpp>
#include <mods/VR.hpp>
 #include "../dependencies/submodules/ImGuizmo/ImGuizmo.h"

namespace imgui {


bool draw_gizmo(Matrix4x4f& mat, IMGUIZMO_NAMESPACE::OPERATION op = IMGUIZMO_NAMESPACE::OPERATION::UNIVERSAL,
    IMGUIZMO_NAMESPACE::MODE mode = IMGUIZMO_NAMESPACE::MODE::WORLD);
void draw_cube(const Matrix4x4f& mat);
void draw_grid(const Matrix4x4f& mat, float size);

bool is_point_intersecting_any(float x, float y);
void draw_sphere(const Vector3f& camera_up, const Vector2f& screen_pos_center, const Vector3f& center, float radius, ImU32 color, bool outline);
void draw_capsule(const Vector3f& camera_up, const Vector2f& screen_pos_center, const Vector3f& start, const Vector3f& end, float radius,
    ImU32 color, bool outline);
void draw_prism(const Vector3f& camera_up, const Vector2f& screen_pos_center, const Vector3f& start, const Vector3f& end, float radius,
    ImU32 color, bool outline);




}