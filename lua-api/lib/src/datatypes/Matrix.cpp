#include <datatypes/StructObject.hpp>
#include "datatypes/Matrix.hpp"
#include "datatypes/Transform.hpp"

namespace lua::datatypes {


void bind_matrix_struct(sol::state_view& lua) {
   

    using Vector4f = lua::datatypes::Vector4f;
    using Vector4d = lua::datatypes::Vector4d;
    using Quaternionf = lua::datatypes::Quaternionf;
    using Quaterniond = lua::datatypes::Quaterniond;
      // add Matrix4x4f (glm::mat4) usertype
    lua.new_usertype<Matrix4x4f>("Matrix4x4f",
        sol::meta_function::construct, 
         sol::constructors<
         Matrix4x4f(),
         Matrix4x4f(const Vector4f&, const Vector4f&, const Vector4f&, const Vector4f&),
         Matrix4x4f(float, float, float, float,
                    float, float, float, float,
                    float, float, float, float,
                    float, float, float, float)
        >(),
        "clone", [](Matrix4x4f& m) -> Matrix4x4f { return m; },
        "identity", []() { return glm::identity<Matrix4x4f>(); },
        "to_quat", [] (Matrix4x4f& m) -> Quaternionf{ return glm::quat(m); }, 
        "inverse", [](Matrix4x4f& m) -> Matrix4x4f { return glm::inverse(m); },
        "invert", [] (Matrix4x4f& m) { m = glm::inverse(m); },
//        "interpolate", [](Matrix4x4f& m1, Matrix4x4f& m2, float t) { return glm::interpolate(m1, m2, t); },
 //       "matrix_rotation", [](Matrix4x4f& m) { return glm::extractMatrixRotation(m); },
        sol::meta_function::multiplication, sol::overload(
            [](Matrix4x4f& lhs, Matrix4x4f& rhs) {
                return lhs * rhs;
            },
            [](Matrix4x4f& lhs, Vector4f& rhs) {

                return lhs * rhs;
            }
        ),
        "decompose",
        [](const Matrix4x4f& m) -> Transformf {
            auto t = Transformf();
            glm::vec3 scale, translation, skew;
            glm::quat rotation;
            glm::vec4 perspective;
            glm::decompose(m, scale, rotation, translation, skew, perspective);
            t.translation = translation;
            t.rotation = rotation;
            t.scale3d = scale;
            return t;
        },
        "transform_point", [](const Matrix4x4f& m, const Vector3f& p) { return Vector3f(m * glm::vec4(p, 1.0f)); },
        "transform_vector",
        [](const Matrix4x4f& m, const Vector3f& v) { return Vector3f(m * glm::vec4(v, 0.0f)); },


        sol::meta_function::index, [](sol::this_state s, Matrix4x4f& lhs, sol::object index_obj) -> sol::object {
            if (!index_obj.is<int>()) {
                return sol::make_object(s, sol::lua_nil);
            }

            const auto index = index_obj.as<int>();

            if (index >= 4) {
                return sol::make_object(s, sol::lua_nil);
            }

            return sol::make_object(s, &lhs[index]);
        },
        sol::meta_function::new_index, [](Matrix4x4f& lhs, int index, Vector4f& rhs) {
            lhs[index] = rhs;
        }
    );
 lua.new_usertype<Matrix4x4d>("Matrix4x4d",
        sol::meta_function::construct, 
         sol::constructors<
         Matrix4x4d(),
         Matrix4x4d(const Vector4d&, const Vector4d&, const Vector4d&, const Vector4d&),
         Matrix4x4d(double, double, double, double,
                    double, double, double, double,
                    double, double, double, double,
                    double, double, double, double)
        >(),
        "clone", [](Matrix4x4d& m) -> Matrix4x4d { return m; },
        "identity", []() { return glm::identity<Matrix4x4d>(); },
        "to_quat", [] (Matrix4x4d& m) -> Quaterniond{ return glm::dquat(m); },
        "inverse", [] (Matrix4x4d& m) { return glm::inverse(m); },
        "invert", [] (Matrix4x4d& m) { m = glm::inverse(m); },
//        "interpolate", [](Matrix4x4f& m1, Matrix4x4f& m2, float t) { return glm::interpolate(m1, m2, t); },
 //       "matrix_rotation", [](Matrix4x4f& m) { return glm::extractMatrixRotation(m); },
        sol::meta_function::multiplication, sol::overload(
            [](Matrix4x4d& lhs, Matrix4x4d& rhs) {
                return lhs * rhs;
            },
            [](Matrix4x4d& lhs, Vector4d& rhs) {

                return lhs * rhs;
            }
        ),
        "decompose",
        [](const Matrix4x4d& m) -> Transformd {
            auto t = Transformd();
            glm::dvec3 scale, translation, skew;
            glm::dquat rotation;
            glm::dvec4 perspective;
            glm::decompose(m, scale, rotation, translation, skew, perspective);
            t.translation = translation;
            t.rotation = rotation;
            t.scale3d = scale;
            return t;
        },
        "transform_point", 
                [](const Matrix4x4d& m, const Vector3d& p) { return Vector3d(m * glm::dvec4(p, 1.0f)); }, 
        "transform_vector",
                [](const Matrix4x4d& m, const Vector3d& v) { return Vector3d(m * glm::dvec4(v, 0.0f)); },



        sol::meta_function::index, [](sol::this_state s, Matrix4x4d& lhs, sol::object index_obj) -> sol::object {
            if (!index_obj.is<int>()) {
                return sol::make_object(s, sol::lua_nil);
            }

            const auto index = index_obj.as<int>();

            if (index >= 4) {
                return sol::make_object(s, sol::lua_nil);
            }

            return sol::make_object(s, &lhs[index]);
        },
        sol::meta_function::new_index, [](Matrix4x4d& lhs, int index, Vector4d& rhs) {
            lhs[index] = rhs;
        }
    );


        }     



}               


 