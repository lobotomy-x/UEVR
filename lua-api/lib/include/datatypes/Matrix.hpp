#pragma once
#include <uevr/API.hpp>
#include "ScriptPrerequisites.hpp"
#include <vector>
#include <datatypes/Vector.hpp>
#include <datatypes/StructObject.hpp>
#include <datatypes/Quaternion.hpp>


namespace lua::datatypes {
   

   using Matrix4x4f = glm::mat4;
    using Matrix4x4d = glm::dmat4;
   void bind_matrix_struct(sol::state_view &lua);


}
