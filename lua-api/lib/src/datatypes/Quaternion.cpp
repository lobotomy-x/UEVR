#include <datatypes/Quaternion.hpp>

namespace lua::datatypes {
    void bind_quaternions(sol::state_view& lua) {
        #define BIND_QUATERNION_LIKE(name, datatype, name2, name3, name4) \
            lua.new_usertype<name>(#name, \
                "set", [](name& v, datatype x, datatype y, datatype z, datatype w) { v.x = x; v.y = y; v.z = z; v.w = w; }, \
                "x", &name::x, \
                "y", &name::y, \
                "z", &name::z, \
                "w", &name::w, \
                "X", &name::x, \
                "Y", &name::y, \
                "Z", &name::z, \
                "W", &name::w, \
                "length", [](const name& v) { return glm::length(v); }, \
                "normalize", [](name& v) { v = glm::normalize(v); }, \
                "normalized", [](const name& v) { return glm::normalize(v); }, \
                "conjugate", [](const name& v) { return glm::conjugate(v); }, \
                "inverse", [](const name& v) { return glm::inverse(v); }, \
                "identity", []() { return glm::identity<name>(); }, \
                "to_mat4", [](const name& q) { return name4{q}; }, \
                "to_vec4", [](const name& v) { return name2{v.x, v.y, v.z, v.w}; }, \
                "dot", [](const name& v1, const name& v2) { return glm::dot(v1, v2); }, \
                "slerp", [](const name& v1, const name& v2, datatype t) { return glm::slerp(v1, v2, t); }, \
                sol::meta_function::addition, [](const name& lhs, const name& rhs) { return lhs + rhs; }, \
                sol::meta_function::subtraction, [](const name& lhs, const name& rhs) { return lhs - rhs; }, \
                sol::meta_function::multiplication, \
                sol::overload([](const name& lhs, datatype scalar) ->name{return lhs * scalar;}, \
                    [](const name& lhs, const name& rhs) -> name {return lhs * rhs;}, \
                    [](const name& lhs, const name3& rhs) -> name3 {return lhs * rhs;}, \
                    [](const name& lhs, const name2& rhs) -> name2 {return lhs * rhs; }), \
            sol::meta_function::index, [](sol::this_state s, const name& lhs, sol::object index_obj) -> sol::object { \
            if (!index_obj.is<int>()) { \
                return sol::make_object(s, sol::lua_nil); \
            } \
            const auto index = index_obj.as<int>(); \
            if (index >= 4) { \
                return sol::make_object(s, sol::lua_nil); \
            } \
            return sol::make_object(s, lhs[index]); \
        }, \
        sol::meta_function::new_index, [](name& lhs, int index, datatype rhs) { \
            if (index < 4) { \
                lhs[index] = rhs; \
            } \
        }
        #define BIND_QUATERNION_LIKE_END() \
            );

        using Matrix4x4f = glm::mat4x4;

        BIND_QUATERNION_LIKE(Quaternionf, float, Vector4f, Vector3f, Matrix4x4f),
            sol::meta_function::construct, sol::constructors<Quaternionf(float, float, float, float)>()
        BIND_QUATERNION_LIKE_END();
        
        BIND_QUATERNION_LIKE(Quaterniond, double, Vector4d, Matrix4x4f),
            sol::meta_function::construct, sol::constructors<Quaterniond(double, double, double, double)>()
        BIND_QUATERNION_LIKE_END();

               //"to_euler", [](const name& q) ->name2 { return utility::math::euler_angles(name4{q}); }, 
    }
}