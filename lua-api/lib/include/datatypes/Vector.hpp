#pragma once

#define GLM_ENABLE_EXPERIMENTAL

#include <glm/vec3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_double.hpp>

#include "ScriptPrerequisites.hpp"
#include <uevr/API.hpp>

namespace lua::datatypes {
    using Vector2f = glm::vec2;
    using Vector2d = glm::dvec2;
    using Vector3f = glm::vec3;
    using Vector3d = glm::dvec3;
    using Vector4f = glm::vec4;
    using Vector4d = glm::dvec4;
    using Quaternionf = glm::quat;
    using Quaterniond = glm::dquat;
     using Matrix4x4f = glm::mat4;
    using Matrix4x4d = glm::dmat4;
    
    void bind_vectors(sol::state_view& lua);
}