#pragma once
  #include "ScriptPrerequisites.hpp"
#include <uevr/API.hpp>
#include <glm/vec4.hpp> 
#include <vector>
#include <datatypes/Vector.hpp>
#include <datatypes/StructObject.hpp>
#include <datatypes/Quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_double.hpp>


#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <glm/vec3.hpp>
#include <datatypes/Transform.hpp>

namespace lua::datatypes {
   using Matrix4x4f = glm::mat4;
    using Matrix4x4d = glm::dmat4;
   void bind_matrix_struct(sol::state_view &lua);
}
